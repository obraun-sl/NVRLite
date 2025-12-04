
#ifndef __MP4Recorder_H__
#define __MP4Recorder_H__

#include "Utils.hpp"
#include <QDebug>

class Mp4RecorderWorker : public QObject {
    Q_OBJECT
public:
    explicit Mp4RecorderWorker(const QString &streamId, QObject *parent = nullptr)
        : QObject(parent), m_streamId(streamId) {
        m_infoReady=false;
    }

signals:
    void recordingStarted(const QString &streamId, const QString &filePath);
    void recordingStopped(const QString &streamId);

public slots:

    void setPreBufferingTime(float c) { pre_buffering_time = c;}

    void onStreamInfo(const StreamInfo &info)
    {
        m_codecId   = info.codecId;
        m_timeBase  = info.timeBase;
        m_width     = info.width;
        m_height    = info.height;
        m_extradata = info.extradata;
        m_infoReady = true;
        qInfo() << "[REC]" << m_streamId << "stream info ready";
    }

    void onPacket(const EncodedVideoPacket &packet) {
        // This slot runs in recorder's own thread (queued connection)
        // Prebuffer or write depending on recording state
        if (!m_recording) {
            // Prebuffer for pre-roll
            m_prebuffer.push_back(packet);
            if (!m_prebuffer.empty()) {
                const EncodedVideoPacket &last = m_prebuffer.back();
                int64_t last_ts = (last.pts != AV_NOPTS_VALUE) ? last.pts : last.dts;
                if (last_ts != AV_NOPTS_VALUE) {
                    double last_sec = last_ts * av_q2d(last.time_base);
                    while (!m_prebuffer.empty()) {
                        const EncodedVideoPacket &first = m_prebuffer.front();
                        int64_t first_ts = (first.pts != AV_NOPTS_VALUE) ? first.pts : first.dts;
                        if (first_ts == AV_NOPTS_VALUE) break;
                        double first_sec = first_ts * av_q2d(first.time_base);
                        if (last_sec - first_sec > pre_buffering_time) {
                            m_prebuffer.pop_front();
                        } else {
                            break;
                        }
                    }
                }
            }
        } else {
            writePacket(packet);
        }
    }

    void startRecording() {
        if (m_recording) {
            qInfo() << "[REC]" << m_streamId << "already recording";
            return;
        }
        if (!m_infoReady) {
            qWarning() << "[REC]" << m_streamId << "stream info not ready";
            return;
        }

        QString filename = makeRecordFilename(m_streamId);

        if (avformat_alloc_output_context2(&m_outCtx, nullptr, "mp4",
                                           filename.toUtf8().constData()) < 0 || !m_outCtx) {
            qWarning() << "[REC]" << m_streamId << "failed to alloc output context";
            return;
        }

        m_outStream = avformat_new_stream(m_outCtx, nullptr);
        if (!m_outStream) {
            avformat_free_context(m_outCtx);
            m_outCtx = nullptr;
            return;
        }

        AVCodecParameters *cp = m_outStream->codecpar;
        memset(cp, 0, sizeof(*cp));
        cp->codec_type = AVMEDIA_TYPE_VIDEO;
        cp->codec_id   = (AVCodecID)m_codecId;
        cp->codec_tag  = 0;              // let muxer choose
        cp->width      = m_width;
        cp->height     = m_height;
        if (!m_extradata.isEmpty()) {
            cp->extradata_size = m_extradata.size();
            cp->extradata = (uint8_t*)av_malloc(cp->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(cp->extradata, m_extradata.constData(), cp->extradata_size);
            memset(cp->extradata + cp->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
        m_outStream->time_base = m_timeBase;

        if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&m_outCtx->pb, filename.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
                avformat_free_context(m_outCtx);
                m_outCtx = nullptr;
                m_outStream = nullptr;
                return;
            }
        }

        if (avformat_write_header(m_outCtx, nullptr) < 0) {
            if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&m_outCtx->pb);
            }
            avformat_free_context(m_outCtx);
            m_outCtx = nullptr;
            m_outStream = nullptr;
            return;
        }

        m_recStartPts = AV_NOPTS_VALUE;
        m_recording = true;

        // Flush prebuffer
        for (const auto &p : m_prebuffer) {
            writePacket(p);
        }
        m_prebuffer.clear();

        emit recordingStarted(m_streamId, filename);
        qInfo() << "[REC]" << m_streamId << "started recording ->" << filename;
    }

    void stopRecording() {
        if (!m_recording)
            return;

        if (m_outCtx) {
            av_write_trailer(m_outCtx);
            if (!(m_outCtx->oformat->flags & AVFMT_NOFILE)) {
                avio_closep(&m_outCtx->pb);
            }
            if (m_outStream && m_outStream->codecpar && m_outStream->codecpar->extradata) {
                av_freep(&m_outStream->codecpar->extradata);
            }
            avformat_free_context(m_outCtx);
        }

        m_outCtx = nullptr;
        m_outStream = nullptr;
        m_recStartPts = AV_NOPTS_VALUE;
        m_recording = false;

        emit recordingStopped(m_streamId);
        qInfo() << "[REC]" << m_streamId << "stopped recording";
    }

private:
    QString m_streamId;
    bool    m_infoReady   = false;
    int     m_codecId     = 0;
    AVRational m_timeBase{1,1};
    int     m_width       = 0;
    int     m_height      = 0;
    QByteArray m_extradata;

    float pre_buffering_time = 5.0;

    bool           m_recording   = false;
    AVFormatContext *m_outCtx    = nullptr;
    AVStream        *m_outStream = nullptr;
    int64_t         m_recStartPts = AV_NOPTS_VALUE;

    std::deque<EncodedVideoPacket> m_prebuffer;

    static QString makeRecordFilename(const QString &streamId) {
        std::time_t t = std::time(nullptr);
        std::tm tm_buf;
        localtime_r(&t, &tm_buf);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm_buf);
        return QString("rec_%1_%2.mp4").arg(streamId, buf);
    }

    void writePacket(const EncodedVideoPacket &packet) {
        if (!m_recording || !m_outCtx || !m_outStream) return;

        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = (uint8_t*)packet.data.constData();
        pkt.size = packet.data.size();
        pkt.flags = packet.key ? AV_PKT_FLAG_KEY : 0;
        pkt.stream_index = m_outStream->index;

        int64_t src_pts = (packet.pts != AV_NOPTS_VALUE) ? packet.pts : packet.dts;
        if (m_recStartPts == AV_NOPTS_VALUE && src_pts != AV_NOPTS_VALUE) {
            m_recStartPts = src_pts;
        }

        if (packet.pts != AV_NOPTS_VALUE && m_recStartPts != AV_NOPTS_VALUE) {
            pkt.pts = av_rescale_q(packet.pts - m_recStartPts,
                                   packet.time_base,
                                   m_outStream->time_base);
        } else {
            pkt.pts = AV_NOPTS_VALUE;
        }

        if (packet.dts != AV_NOPTS_VALUE && m_recStartPts != AV_NOPTS_VALUE) {
            pkt.dts = av_rescale_q(packet.dts - m_recStartPts,
                                   packet.time_base,
                                   m_outStream->time_base);
        } else {
            pkt.dts = AV_NOPTS_VALUE;
        }

        if (packet.duration > 0) {
            pkt.duration = av_rescale_q(packet.duration,
                                        packet.time_base,
                                        m_outStream->time_base);
        } else {
            pkt.duration = 0;
        }

        pkt.pos = -1;

        int wret = av_interleaved_write_frame(m_outCtx, &pkt);
        if (wret < 0) {
            log_error("[REC] Error writing frame", wret);
        }
    }
};


#endif /* __MP4Recorder_H__ */
