/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2012-2014 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/

#include "AVDemuxThread.h"
#include "QtAV/AVDemuxer.h"
#include "QtAV/AVDecoder.h"
#include "QtAV/Packet.h"
#include "AVThread.h"
#include <QtCore/QTimer>
#include <QtCore/QEventLoop>
#include "utils/Logger.h"

#define RESUME_ONCE_ON_SEEK 0

namespace QtAV {

class QueueEmptyCall : public PacketQueue::StateChangeCallback
{
public:
    QueueEmptyCall(AVDemuxThread* thread):
        mDemuxThread(thread)
    {}
    virtual void call() {
        if (!mDemuxThread)
            return;
        if (mDemuxThread->isEnd())
            return;
        AVThread *thread = mDemuxThread->videoThread();
        //qDebug("try wake up video queue");
        if (thread)
            thread->packetQueue()->blockFull(false);
        //qDebug("try wake up audio queue");
        thread = mDemuxThread->audioThread();
        if (thread)
            thread->packetQueue()->blockFull(false);
    }
private:
    AVDemuxThread *mDemuxThread;
};

AVDemuxThread::AVDemuxThread(QObject *parent) :
    QThread(parent)
  , paused(false)
  , user_paused(false)
  , end(true)
  , demuxer(0)
  , audio_thread(0)
  , video_thread(0)
  , nb_next_frame(0)
{
    seek_tasks.setCapacity(1);
    seek_tasks.blockFull(false);
}

AVDemuxThread::AVDemuxThread(AVDemuxer *dmx, QObject *parent) :
    QThread(parent)
  , paused(false)
  , end(true)
  , audio_thread(0)
  , video_thread(0)
{
    setDemuxer(dmx);
    seek_tasks.setCapacity(1);
    seek_tasks.blockFull(false);
}

void AVDemuxThread::setDemuxer(AVDemuxer *dmx)
{
    demuxer = dmx;
}

void AVDemuxThread::setAVThread(AVThread*& pOld, AVThread *pNew)
{
    if (pOld == pNew)
        return;
    if (pOld) {
        if (pOld->isRunning())
            pOld->stop();
    }
    pOld = pNew;
    if (!pNew)
        return;
    pOld->packetQueue()->setEmptyCallback(new QueueEmptyCall(this));
}

void AVDemuxThread::setAudioThread(AVThread *thread)
{
    setAVThread(audio_thread, thread);
}

void AVDemuxThread::setVideoThread(AVThread *thread)
{
    setAVThread(video_thread, thread);
}

AVThread* AVDemuxThread::videoThread()
{
    return video_thread;
}

AVThread* AVDemuxThread::audioThread()
{
    return audio_thread;
}

void AVDemuxThread::seek(qint64 pos)
{
    end = false;
    // queue maybe blocked by put()
    if (audio_thread) {
        audio_thread->setDemuxEnded(false);
        audio_thread->packetQueue()->clear();
    }
    if (video_thread) {
        video_thread->setDemuxEnded(false);
        video_thread->packetQueue()->clear();
    }
    class SeekTask : public QRunnable {
    public:
        SeekTask(AVDemuxThread *dt, qint64 t)
            : demux_thread(dt)
            , position(t)
        {}
        void run() {
            demux_thread->seekInternal(position);
        }
    private:
        AVDemuxThread *demux_thread;
        qint64 position;
    };
    newSeekRequest(new SeekTask(this, pos));
}

void AVDemuxThread::seekInternal(qint64 pos)
{
    if (audio_thread) {
        audio_thread->setDemuxEnded(false);
        audio_thread->packetQueue()->clear();
    }
    if (video_thread) {
        video_thread->setDemuxEnded(false);
        video_thread->packetQueue()->clear();
    }
    qDebug("seek to %lld ms (%f%%)", pos, double(pos)/double(demuxer->duration())*100.0);
    demuxer->seek(pos);
    // TODO: why queue may not empty?
    if (audio_thread) {
        audio_thread->packetQueue()->clear();
        audio_thread->packetQueue()->put(Packet());
    }
    if (video_thread) {
        video_thread->packetQueue()->clear();
        // TODO: the first frame (key frame) will not be decoded correctly if flush() is called.
        video_thread->packetQueue()->put(Packet());
    }
    //if (subtitle_thread) {
    //     subtitle_thread->packetQueue()->clear();
    //    subtitle_thread->packetQueue()->put(Packet());
    //}

    if (isPaused() && (video_thread || audio_thread)) {
        AVThread *thread = video_thread ? video_thread : audio_thread;
        thread->pause(false);
        pauseInternal(false);
        emit requestClockPause(false); // need direct connection
        // direct connection is fine here
        connect(thread, SIGNAL(frameDelivered()), this, SLOT(frameDeliveredSeekOnPause()), Qt::DirectConnection);
    }
}

void AVDemuxThread::newSeekRequest(QRunnable *r)
{
    if (seek_tasks.size() >= seek_tasks.capacity()) {
        QRunnable *r = seek_tasks.take();
        if (r->autoDelete())
            delete r;
    }
    seek_tasks.put(r);
}

void AVDemuxThread::processNextSeekTask()
{
    if (seek_tasks.isEmpty())
        return;
    QRunnable *task = seek_tasks.take();
    if (!task)
        return;
    task->run();
    if (task->autoDelete())
        delete task;
}

void AVDemuxThread::pauseInternal(bool value)
{
    paused = value;
}

void AVDemuxThread::processNextPauseTask()
{
    if (pause_tasks.isEmpty())
        return;
    QRunnable *task = pause_tasks.dequeue();
    if (!task)
        return;
    task->run();
    if (task->autoDelete())
        delete task;
}

bool AVDemuxThread::isPaused() const
{
    return paused;
}

bool AVDemuxThread::isEnd() const
{
    return end;
}

//No more data to put. So stop blocking the queue to take the reset elements
void AVDemuxThread::stop()
{
    //this will not affect the pause state if we pause the output
    //TODO: why remove blockFull(false) can not play another file?
    if (audio_thread) {
        audio_thread->setDemuxEnded(true);
        audio_thread->packetQueue()->clear();
        audio_thread->packetQueue()->blockFull(false); //??
        while (audio_thread->isRunning()) {
            qDebug("stopping audio thread.......");
            audio_thread->stop();
            audio_thread->wait(500);
        }
    }
    if (video_thread) {
        video_thread->setDemuxEnded(true);
        video_thread->packetQueue()->clear();
        video_thread->packetQueue()->blockFull(false); //?
        while (video_thread->isRunning()) {
            qDebug("stopping video thread.......");
            video_thread->stop();
            video_thread->wait(500);
        }
    }
    pause(false);
    cond.wakeAll();
    qDebug("all avthread finished. try to exit demux thread<<<<<<");
    end = true;
}

void AVDemuxThread::pause(bool p)
{
    if (paused == p)
        return;
    paused = p;
    user_paused = paused;
    if (!paused)
        cond.wakeAll();
}

void AVDemuxThread::nextFrame()
{
    pause(true); // must pause AVDemuxThread (set user_paused true)
    AVThread *t = video_thread;
    bool connected = false;
    if (t) {
        t->pause(false);
        t->packetQueue()->blockFull(false);
        if (!connected) {
            connect(t, SIGNAL(frameDelivered()), this, SLOT(frameDeliveredNextFrame()), Qt::DirectConnection);
            connected = true;
        }
    }
    t = audio_thread;
    if (t) {
        t->pause(false);
        t->packetQueue()->blockFull(false);
        if (!connected) {
            connect(t, SIGNAL(frameDelivered()), this, SLOT(frameDeliveredNextFrame()), Qt::DirectConnection);
            connected = true;
        }
    }
    emit requestClockPause(false);
    nb_next_frame.ref();
    pauseInternal(false);
}

void AVDemuxThread::frameDeliveredSeekOnPause()
{
    AVThread *thread = video_thread ? video_thread : audio_thread;
    Q_ASSERT(thread);
    disconnect(thread, SIGNAL(frameDelivered()), this, SLOT(frameDeliveredSeekOnPause()));
    if (user_paused) {
        pause(true); // restore pause state
        emit requestClockPause(true); // need direct connection
    // pause video/audio thread
        thread->pause(true);
    }
}

void AVDemuxThread::frameDeliveredNextFrame()
{
    AVThread *thread = video_thread ? video_thread : audio_thread;
    Q_ASSERT(thread);
    if (nb_next_frame.deref()) {
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0) || QT_VERSION >= QT_VERSION_CHECK(5, 3, 0)
        Q_ASSERT_X((int)nb_next_frame > 0, "frameDeliveredNextFrame", "internal error. frameDeliveredNextFrame must be > 0");
#else
        Q_ASSERT_X((int)nb_next_frame.load() > 0, "frameDeliveredNextFrame", "internal error. frameDeliveredNextFrame must be > 0");
#endif
        return;
    }
    disconnect(thread, SIGNAL(frameDelivered()), this, SLOT(frameDeliveredNextFrame()));
    if (user_paused) {
        pause(true); // restore pause state
        emit requestClockPause(true); // need direct connection
    // pause both video and audio thread
        if (video_thread)
            video_thread->pause(true);
        if (audio_thread)
            audio_thread->pause(true);
    }
}

void AVDemuxThread::run()
{
    end = false;
    if (audio_thread && !audio_thread->isRunning())
        audio_thread->start(QThread::HighPriority);
    if (video_thread && !video_thread->isRunning())
        video_thread->start();

    int running_threads = 0;
    if (audio_thread)
        ++running_threads;
    if (video_thread)
        ++running_threads;
    qDebug("demux thread start running...%d avthreads", running_threads);

    audio_stream = demuxer->audioStream();
    video_stream = demuxer->videoStream();
    int index = 0;
    Packet pkt;
    pause(false);
    qDebug("get av queue a/v thread = %p %p", audio_thread, video_thread);
    PacketQueue *aqueue = audio_thread ? audio_thread->packetQueue() : 0;
    PacketQueue *vqueue = video_thread ? video_thread->packetQueue() : 0;
    if (aqueue) {
        aqueue->clear();
        aqueue->setBlocking(true);
    }
    if (vqueue) {
        vqueue->clear();
        vqueue->setBlocking(true);
    }
    while (!end) {
        processNextSeekTask();
        if (tryPause()) {
            continue; //the queue is empty and will block
        }
        running_threads = (audio_thread && audio_thread->isRunning()) + (video_thread && video_thread->isRunning());
        if (!running_threads) {
            qDebug("no running avthreads. exit demuxer thread");
            break;
        }
        QMutexLocker locker(&buffer_mutex);
        Q_UNUSED(locker);
        if (end) {
            break;
        }
        if (!demuxer->readFrame()) {
            continue;
        }
        index = demuxer->stream();
        pkt = *demuxer->packet(); //TODO: how to avoid additional copy?
        //connect to stop is ok too
        if (pkt.isEnd()) {
            qDebug("read end packet %d A:%d V:%d", index, audio_stream, video_stream);
            end = true;
            //avthread can stop. do not clear queue, make sure all data are played
            if (audio_thread) {
                audio_thread->setDemuxEnded(true);
            }
            if (video_thread) {
                video_thread->setDemuxEnded(true);
            }
            break;
        }
        /*1 is empty but another is enough, then do not block to
          ensure the empty one can put packets immediatly.
          But usually it will not happen, why?
        */
        /* demux thread will be blocked only when 1 queue is full and still put
         * if vqueue is full and aqueue becomes empty, then demux thread
         * will be blocked. so we should wake up another queue when empty(or threshold?).
         * TODO: the video stream and audio stream may be group by group. provide it
         * stream data: aaaaaaavvvvvvvaaaaaaaavvvvvvvvvaaaaaa, it happens
         * stream data: aavavvavvavavavavavavavavvvaavavavava, it's ok
         */
        //TODO: use cache queue, take from cache queue if not empty?
        if (index == audio_stream) {
            /* if vqueue if not blocked and full, and aqueue is empty, then put to
             * vqueue will block demuex thread
             */
            if (aqueue) {
                if (!audio_thread || !audio_thread->isRunning()) {
                    aqueue->clear();
                    continue;
                }
                // always block full if no vqueue because empty callback may set false
                // attached picture is cover for song, 1 frame
                aqueue->blockFull(!video_thread || !video_thread->isRunning() || !vqueue || (vqueue->isEnough() || demuxer->hasAttacedPicture()));
                aqueue->put(pkt); //affect video_thread
            }
        } else if (index == video_stream) {
            if (vqueue) {
                if (!video_thread || !video_thread->isRunning()) {
                    vqueue->clear();
                    continue;
                }
                vqueue->blockFull(!audio_thread || !audio_thread->isRunning() || !aqueue || aqueue->isEnough());
                vqueue->put(pkt); //affect audio_thread
            }
        } else { //subtitle
            continue;
        }
    }
    //flush. seeking will be omitted when stopped
    if (aqueue)
        aqueue->put(Packet());
    if (vqueue)
        vqueue->put(Packet());
    while (audio_thread && audio_thread->isRunning()) {
        qDebug("waiting audio thread.......");
        audio_thread->wait(500);
    }
    while (video_thread && video_thread->isRunning()) {
        qDebug("waiting video thread.......");
        video_thread->wait(500);
    }
    qDebug("Demux thread stops running....");
}

bool AVDemuxThread::tryPause(unsigned long timeout)
{
    if (!paused)
        return false;
    QMutexLocker lock(&buffer_mutex);
    Q_UNUSED(lock);
    cond.wait(&buffer_mutex, timeout);
    return true;
}


} //namespace QtAV
