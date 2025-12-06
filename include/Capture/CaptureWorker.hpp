
#ifndef __CaptureWorker_H__
#define __CaptureWorker_H__

#include "Utils.hpp"
#include <QDebug>

class RtspCaptureThread : public QThread {
    Q_OBJECT
public:
    RtspCaptureThread(const QString &streamId,
                      const QString &url,
                      QObject *parent = nullptr);
    ~RtspCaptureThread() override;

    void requestStop() {
        m_abort.storeRelease(1);
    }

    void setWithUserInterface(bool c)
    {
        m_userInterface = c;
    }

signals:
    // For recorder
    void streamInfoReady(const StreamInfo &info);
    void videoPacketReady(const EncodedVideoPacket &packet);

    // For display
    void frameReady(const QString &streamId,
                    const cv::Mat &frame);

    // Online/offline status
    void streamOnlineChanged(const QString &streamId, bool online);

protected:
    void run() override;

public slots:
    // Called via HttpHanlder when /stream/start or /stream/stop is hit
    void onStreamStartRequested(const QString &streamId);
    void onStreamStopRequested(const QString &streamId);

private:
    bool openInput();
    void closeInput();
    cv::Mat makeNoSignalFrame(int w, int h,QString);

private:
    QString m_streamId;
    QString m_url;

    AVFormatContext *m_fmtCtx{nullptr};
    AVCodecContext  *m_codecCtx{nullptr};
    SwsContext      *m_swsCtx{nullptr};
    int              m_videoStreamIndex{-1};

    int              m_width{640};
    int              m_height{480};
    AVPixelFormat    m_srcPixFmt{AV_PIX_FMT_NONE};
    AVPixelFormat    m_dstPixFmt{AV_PIX_FMT_BGR24};

    QAtomicInteger<int> m_abort{0};
    bool           m_online{false};
    bool           m_userInterface{false};

    QAtomicInteger<int> m_enableStreaming{0}; // 1=enabled, 0=disabled

    QMutex guard;

};

#endif /* __CaptureWorker_H__ */
