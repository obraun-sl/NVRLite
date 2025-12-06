#ifndef _HTTP_MANAGER_H
#define _HTTP_MANAGER_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QReadWriteLock>
#include <QHash>
#include <atomic>
#include <thread>

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


public slots:
    // Connect these slots to your Mp4RecorderWorker signals:
    //  Mp4RecorderWorker::recordingStarted(streamId, filePath)
    //  Mp4RecorderWorker::recordingStopped(streamId)
    void onRecordingStarted(const QString& streamId, const QString& filePath);
    void onRecordingStopped(const QString& streamId);

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

    int mVerboseLevel = 0;
};

#endif // _HTTP_MANAGER_H
