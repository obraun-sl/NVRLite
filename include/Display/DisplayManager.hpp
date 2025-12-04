
#ifndef __DisplayManager_H__
#define __DisplayManager_H__

#include "Utils.hpp"
#include "Recording/MP4Recorder.hpp"

class DisplayManager : public QObject {
    Q_OBJECT
public:
    DisplayManager(QHash<QString, Mp4RecorderWorker*> *recorders,
                   const QStringList &streamIds,
                   QObject *parent = nullptr)
        : QObject(parent), m_recorders(recorders), m_streamIds(streamIds)
    {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &DisplayManager::updateDisplay);
        m_timer->start(30); // ~33 FPS
        cv::namedWindow("RTSP Grid", cv::WINDOW_NORMAL);
    }

    ~DisplayManager()
    {
        if (m_timer)
        {
            m_timer->stop();
            delete m_timer;
            m_timer = nullptr;
        }
    }

public slots:
    void onFrame(const QString &streamId,const cv::Mat &frame) {
        QMutexLocker locker(&m_mutex);
        m_lastFrames[streamId] = frame.clone();
    }

private slots:
    void updateDisplay() {


        QMutexLocker locker(&m_mutex);
        if (m_lastFrames.empty())
            return;

        int n = m_lastFrames.size();
        int cols = std::ceil(std::sqrt(n));
        int rows = std::ceil(n / (double)cols);

        int cell_w = 320;
        int cell_h = 240;

        cv::Mat grid(rows * cell_h, cols * cell_w, CV_8UC3, cv::Scalar(0,0,0));
        int idx = 0;
        for (auto it = m_lastFrames.begin(); it != m_lastFrames.end(); ++it, ++idx) {
            int r = idx / cols;
            int c = idx % cols;
            cv::Rect roi(c * cell_w, r * cell_h, cell_w, cell_h);

            cv::Mat dstRoi = grid(roi);
            if (!it.value().empty()) {
                cv::Mat resized;
                cv::resize(it.value(), resized, cv::Size(cell_w, cell_h));
                resized.copyTo(dstRoi);

                // Optional: overlay streamId
                cv::putText(resized,
                            it.key().toStdString(),
                            cv::Point(10, 20),
                            cv::FONT_HERSHEY_SIMPLEX,
                            0.5,
                            cv::Scalar(0,255,0),
                            1);
            }
        }

        locker.unlock();

        cv::imshow("RTSP Grid", grid);
        int key = cv::waitKey(1);
        if (key == 'c' || key == 'C') {
            qInfo() << "Start recording for all streams";
            for (const auto &id : m_streamIds) {
                if (m_recorders->contains(id)) {
                    (*m_recorders)[id]->startRecording(); // queued to recorder thread
                }
            }
        } else if (key == 's' || key == 'S') {
            qInfo() << "Stop recording for all streams";
            for (const auto &id : m_streamIds) {
                if (m_recorders->contains(id)) {
                    (*m_recorders)[id]->stopRecording(); // queued
                }
            }
        }
    }

private:
    QHash<QString, Mp4RecorderWorker*> *m_recorders;
    QStringList  m_streamIds;

    QTimer *m_timer = nullptr;
    QMutex  m_mutex;
    QHash<QString, cv::Mat> m_lastFrames;
};


#endif /* __DisplayManager_H__ */
