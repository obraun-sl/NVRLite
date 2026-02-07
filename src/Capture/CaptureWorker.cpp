#include "Capture/CaptureWorker.hpp"
#include <chrono>


RtspCaptureThread::RtspCaptureThread(const QString &streamId,
                                     const QString &url,
                                     QObject *parent)
    : QThread(parent)
    , m_streamId(streamId)
    , m_url(url)
{
}

RtspCaptureThread::~RtspCaptureThread() {
    requestStop();
    wait();
    closeInput();
}

void RtspCaptureThread::onStreamStartRequested(const QString &streamId)
{
    qInfo() << "[CAP]" << m_streamId << "{onStreamStartRequested} handle START"<<streamId;
    if (streamId != m_streamId)
        return;
    m_enableStreaming.storeRelease(1);
    qInfo() << "[CAP]" << m_streamId << "streaming ENABLED via HTTP";
}

void RtspCaptureThread::onStreamStopRequested(const QString &streamId)
{
    qInfo() << "[CAP]" << m_streamId << "{onStreamStopRequested} handle STOP "<<streamId;
    if (streamId != m_streamId)
        return;

    m_enableStreaming.storeRelease(0);
    {
        QMutexLocker locker(&guard);
        closeInput();
        if (m_online) {
            m_online = false;
            emit streamOnlineChanged(m_streamId, false);
        }
    }
    qInfo() << "[CAP]" << m_streamId << "streaming DISABLED via HTTP";
}

bool RtspCaptureThread::openInput() {
    closeInput();


    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); // 5s
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);
    av_dict_set(&opts, "reorder_queue_size", "1", 0);

    // Optional: help FFmpeg find codec params for H.264 over RTSP
    av_dict_set(&opts, "probesize", "5000000", 0);        // bytes
    av_dict_set(&opts, "analyzeduration", "1000000", 0);  // microseconds

    int ret = avformat_open_input(&m_fmtCtx,
                                  m_url.toUtf8().constData(),
                                  nullptr,
                                  &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "avformat_open_input failed:" << ret;
        m_fmtCtx = nullptr;
        return false;
    }

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "avformat_find_stream_info failed:" << ret;
        closeInput();
        return false;
    }

    // Find video stream
    m_videoStreamIndex =
            av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "no video stream found";
        closeInput();
        return false;
    }

    AVStream *vs = m_fmtCtx->streams[m_videoStreamIndex];
    AVCodecParameters *par = vs->codecpar;

    const AVCodec *dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        qWarning() << "[CAP]" << m_streamId
                   << "no decoder for codec_id" << par->codec_id;
        closeInput();
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(dec);
    if (!m_codecCtx) {
        qWarning() << "[CAP]" << m_streamId << "alloc codec ctx failed";
        closeInput();
        return false;
    }

    ret = avcodec_parameters_to_context(m_codecCtx, par);
    if (ret < 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "avcodec_parameters_to_context failed:" << ret;
        closeInput();
        return false;
    }

    m_codecCtx->thread_count = 1;
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    ret = avcodec_open2(m_codecCtx, dec, nullptr);
    if (ret < 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "avcodec_open2 failed:" << ret;
        closeInput();
        return false;
    }

    // These may be 0 / unknown at this point for H.264 over RTSP – that's OK.
    m_width     = par->width;
    m_height    = par->height;
    m_srcPixFmt = static_cast<AVPixelFormat>(m_codecCtx->pix_fmt);

    if (m_width <= 0 || m_height <= 0) {
        qWarning() << "[CAP]" << m_streamId
                   << "codec parameters have no valid size yet; will use first decoded frame";
        // We keep default m_width/m_height (640x480) for NO SIGNAL frame only.
    } else {
        qDebug() << "[CAP]" << m_streamId
                 << "codec parameters size:"
                 << "w=" << m_width
                 << "h=" << m_height;
    }

    // do'nt create swsCtx here; we will create it on first decoded frame
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    // Notify recorder about stream info (time_base + codec id are known)
    StreamInfo info;
    info.streamId = m_streamId;
    info.width    = (m_width);  // may still be 0
    info.height   = (m_height);
    info.timeBase = vs->time_base;
    info.codecId  = par->codec_id;

    // copy extradata from codec parameters if present
    if (par->extradata && par->extradata_size > 0) {
        info.extradata = QByteArray(
                    reinterpret_cast<const char*>(par->extradata),
                    par->extradata_size
                    );
    } else {
        info.extradata.clear();
    }

    emit streamInfoReady(info);

    return true;
}

void RtspCaptureThread::closeInput() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    m_videoStreamIndex = -1;
}

cv::Mat RtspCaptureThread::makeNoSignalFrame(int w, int h,QString text) {
    cv::Mat img(h, w, CV_8UC3, cv::Scalar(40, 40, 40)); // dark gray
    cv::putText(img,
                text.toStdString(),
                cv::Point(w/8, h/2),
                cv::FONT_HERSHEY_SIMPLEX,
                1.5,
                cv::Scalar(0, 0, 255),
                3);
    return img;
}


void RtspCaptureThread::run() {
    qDebug() << "[CAP]" << m_streamId << "thread started";

    AVPacket *pkt  = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();

    if (!pkt || !frame) {
        qWarning() << "[CAP]" << m_streamId << "failed to allocate pkt/frame";
        if (pkt)   av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        return;
    }

    // We'll reuse a "NO SIGNAL" frame; size might adjust after first successful open
    cv::Mat noSignal = makeNoSignalFrame(m_width, m_height,"NO SIGNAL");
    emit frameReady(m_streamId, noSignal.clone());


    using clock = std::chrono::steady_clock;

    while (!m_abort.loadAcquire()) {

        // If streaming is disabled, ensure we are offline and idle
        if (!m_enableStreaming.loadAcquire()) {
            // Protect this section (but not the sleep)
            {
                QMutexLocker locker(&guard);
                if (m_fmtCtx) {
                    closeInput();
                }
                if (m_online) {
                    m_online = false;
                    emit streamOnlineChanged(m_streamId, false);
                    if(mVerboseLevel>0)
                        qDebug() << "[CAP]" << m_streamId << "==> Stream status changed to false";
                }
            }
            // Make a NOSIG image to display
            cv::Mat noSignal = makeNoSignalFrame(m_width, m_height,"NO SIGNAL");
            emit frameReady(m_streamId, noSignal.clone());
            QThread::msleep(100);
            continue;
        }
        else
        {
        QMutexLocker locker(&guard);
        // Ensure RTSP is open. If not, attempt every 5 seconds and show NO SIGNAL
        if (!m_fmtCtx) {
            noSignal = makeNoSignalFrame(m_width, m_height,"ACQUIRING");
            emit frameReady(m_streamId, noSignal.clone());
            if (!openInput()) {
                if (m_online) {
                    m_online = false;
                    emit streamOnlineChanged(m_streamId, false);
                    if(mVerboseLevel>0)
                        qDebug() << "[CAP]" << m_streamId << "==> Stream status changed to false";
                }

                // Build a NO SIGNAL frame with our current notion of size
                noSignal = makeNoSignalFrame(m_width, m_height,"STREAM FAILED");
                qWarning() << "[CAP]" << m_streamId << "will retry RTSP in 5 seconds";
                auto startWait    = clock::now();
                auto lastEmit     = startWait;
                const int fpsNoSignal = 5;     // NO SIGNAL frame rate
                const int emitMs      = 1000 / fpsNoSignal;

                while (!m_abort.loadAcquire()) {
                    auto now          = clock::now();
                    auto msSinceStart = std::chrono::duration_cast<std::chrono::milliseconds>(now - startWait).count();
                    if (msSinceStart >= 5000)  // 5s
                        break;

                    auto msSinceEmit = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count();
                    if (msSinceEmit >= emitMs) {
                        emit frameReady(m_streamId, noSignal.clone());
                        lastEmit = now;
                    }

                    QThread::msleep(10);
                }
                continue; // will retry openInput()
                if(mVerboseLevel>0)
                    qDebug() << "[CAP]" << m_streamId << "==> Will retry input loading";

            } else {
                // Just successfully opened
                if (!m_online) {
                    m_online = true;
                    emit streamOnlineChanged(m_streamId, true);
                    if(mVerboseLevel>0)
                        qDebug() << "[CAP]" << m_streamId << "==> Stream status changed to true";
                }
            }
        }

        // Normal streaming loop
        if (m_online)
        {
            int ret = av_read_frame(m_fmtCtx, pkt);
            if (ret < 0) {
                qWarning() << "[CAP]" << m_streamId
                           << "av_read_frame error:" << ret
                           << " -> closing and will retry";
                closeInput();
                if (m_online) {
                    m_online = false;
                    emit streamOnlineChanged(m_streamId, false);
                }
                continue; // go back to reconnect logic
            }

            if (pkt->stream_index != m_videoStreamIndex) {
                av_packet_unref(pkt);
                continue;
            }

            // Build EncodedVideoPacket for recorder
            EncodedVideoPacket evp;
            evp.streamId = m_streamId;   // if you include this field; if not, remove
            evp.data = QByteArray(reinterpret_cast<const char*>(pkt->data),
                                  pkt->size);
            evp.pts      = pkt->pts;
            evp.dts      = pkt->dts;
            evp.duration = pkt->duration;
            evp.key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            evp.time_base = m_fmtCtx->streams[m_videoStreamIndex]->time_base;
            emit videoPacketReady(evp);

            // Decode for display
            ret = avcodec_send_packet(m_codecCtx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                qWarning() << "[CAP]" << m_streamId
                           << "avcodec_send_packet failed:" << ret;
                continue;
            }


            while (ret >= 0 && !m_abort.loadAcquire()) {
                ret = avcodec_receive_frame(m_codecCtx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) {
                    qWarning() << "[CAP]" << m_streamId
                               << "avcodec_receive_frame failed:" << ret;
                    break;
                }

                // Initialize swscale *here* once we know real size/format
                if (!m_swsCtx) {
                    m_width     = frame->width;
                    m_height    = frame->height;
                    m_srcPixFmt = static_cast<AVPixelFormat>(frame->format);

                    qDebug() << "[CAP]" << m_streamId
                             << "got first frame:"
                             << "w=" << m_width
                             << "h=" << m_height
                             << "fmt=" << m_srcPixFmt;

                    // Notify recorder about stream info (time_base + codec id are known)
                    StreamInfo info;
                    info.streamId = m_streamId;
                    info.width    = m_width;  // may still be 0
                    info.height   = m_height;
                    info.timeBase = evp.time_base;
                    info.codecId  = m_codecCtx->codec_id;

                    // Prefer extradata from codec context if available
                    if (m_codecCtx->extradata && m_codecCtx->extradata_size > 0) {
                        info.extradata = QByteArray(
                                    reinterpret_cast<const char*>(m_codecCtx->extradata),
                                    m_codecCtx->extradata_size
                                    );
                    } else {
                        // Fallback: try the stream codecpar
                        AVStream *vs = m_fmtCtx->streams[m_videoStreamIndex];
                        AVCodecParameters *par = vs->codecpar;
                        if (par->extradata && par->extradata_size > 0) {
                            info.extradata = QByteArray(
                                        reinterpret_cast<const char*>(par->extradata),
                                        par->extradata_size
                                        );
                        } else {
                            info.extradata.clear();
                        }
                    }

                    emit streamInfoReady(info);

                    m_swsCtx = sws_getContext(
                                m_width, m_height, m_srcPixFmt,
                                m_width, m_height, m_dstPixFmt,
                                SWS_BILINEAR, nullptr, nullptr, nullptr
                                );

                    if (!m_swsCtx) {
                        qWarning() << "[CAP]" << m_streamId
                                   << "sws_getContext failed on first frame";
                        break;
                    }
                }


                /// Only necessary when UserInterface is needed.
                /// Save some CPU usage
                if (m_userInterface)
                {
                    cv::Mat bgr(m_height, m_width, CV_8UC3);
                    uint8_t *dstData[4]    = { bgr.data, nullptr, nullptr, nullptr };
                    int      dstLinesize[4] = { static_cast<int>(bgr.step), 0, 0, 0 };

                    sws_scale(m_swsCtx,
                              frame->data,
                              frame->linesize,
                              0,
                              m_height,
                              dstData,
                              dstLinesize);

                    emit frameReady(m_streamId, bgr);
                }

            }
        }

        }
        QThread::usleep(500);
    }

    QMutexLocker locker(&guard);
    closeInput();
    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (m_online) {
        m_online = false;
        emit streamOnlineChanged(m_streamId, false);
        if(mVerboseLevel>0)
            qDebug() << "[CAP]" << m_streamId << "==> Stream status changed to false";
    }

    qDebug() << "[CAP]" << m_streamId << "thread finished";
}
