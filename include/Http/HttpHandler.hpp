#ifndef _HTTP_MANAGER_H
#define _HTTP_MANAGER_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QReadWriteLock>
#include <QHash>
#include <atomic>
#include <thread>
#include <QSet>

// cpp-httlib (header-only)
#include "httplib.h"
#include "Http/json.hpp"

class HttpDataServer : public QObject {
    Q_OBJECT
public:
    explicit HttpDataServer(QObject* parent = nullptr);
    ~HttpDataServer() override;

    // Start/stop server (non-blocking). Returns false if already running.
    bool start(const QString& host = "0.0.0.0", quint16 port = 8090);
    void stop();

    void setVerboseLevel(int lvl){ mVerboseLevel = lvl; }
    void setFolderBase(QString p) {mFolderBasePath=p;}

    // Optional: still allow setting a shared JSON payload for /data
    void setPayload(const QByteArray& payload, const QString& contentType = "application/json");

signals:
    void started(const QString& host, quint16 port);
    void stopped();
    void requestServed(quint64 bytes);   // emitted on successful GET /data

    // These are emitted when HTTP /record/start or /record/stop is called.
    void startRecordingRequested(const QString& streamId);
    void stopRecordingRequested(const QString& streamId);

    // These are emitted when HTTP /stream/start or /stream/stop is called.
    void startStreamRequested(const QString &streamId);
    void stopStreamRequested(const QString &streamId);

private :
    void createRoutes();


public slots:
    void onRecordingStarted(const QString& streamId, const QString& filePath);
    void onRecordingStopped(const QString& streamId);

    // Connected to RtspCaptureThread::streamOnlineChanged(streamId, online) to get stream status
    void onStreamOnlineChanged(const QString &streamId, bool online);

    // Register a known stream ID (called from main/config loading)
    void registerStream(const QString &streamId);

private:
    using json = sl::json;

    // Handler glue
    static QByteArray readCurrentPayload(HttpDataServer* self, QString& contentTypeOut);

    // Internal state
    httplib::Server m_server;
    std::thread     m_thread;
    std::atomic_bool m_running{false};

    QString m_host;
    quint16 m_port{0};

    // Shared payload for /data
    QByteArray    m_payload;
    QString       m_contentType{QStringLiteral("application/json")};
    QReadWriteLock m_lock;

    // Track last recording file per stream_id
    QReadWriteLock         m_filesLock;
    QHash<QString, QString> m_lastRecordingFile; // streamId -> filePath
    QHash<QString, bool>    m_recordingState;    // streamId -> isRecording
    // When /record/start is accepted but recorder hasn't yet emitted onRecordingStarted()
    QHash<QString, bool>    m_recordingPending;  // streamId -> start requested, waiting for file
    // If /record/stop is received while start is still pending (file unknown), remember it.
    QHash<QString, bool>    m_stopPending;       // streamId -> stop requested while pending
    QHash<QString, bool>    m_streamingState;    // streamId -> is streaming
    QSet<QString>           m_knownStreams;      // all configured/known streams

    int mVerboseLevel = 0;
    QString mFolderBasePath="~/";
};

#endif // _HTTP_MANAGER_H
