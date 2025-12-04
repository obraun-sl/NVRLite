#include "Http/HttpHandler.hpp"
#include <QDebug>
#include <QDateTime>
#include <chrono>

HttpDataServer::HttpDataServer(QObject* parent)
    : QObject(parent) {}

HttpDataServer::~HttpDataServer() {
    stop();
}

void HttpDataServer::setPayload(const QByteArray& payload, const QString& contentType) {
    QWriteLocker locker(&m_lock);
    m_payload = payload;
    m_contentType = contentType;
}

bool HttpDataServer::start(const QString& host, quint16 port) {
    if (m_running.load()) return false;

    m_host = host;
    m_port = port;
    m_running.store(true);

    const std::string hostStr = m_host.toStdString();
    const int portInt = static_cast<int>(m_port);

    using json = sl::json;

    // ---------- ROUTES ----------

    // 1) POST /record/start
    //    Body: { "stream_id": "stream_1" }
    //    Response: { "status": "ok", "stream_id": "stream_1" } or error
    m_server.Post("/record/start", [this](const httplib::Request& req, httplib::Response& res) {
        json response;
        response["status"] = "error";

        try {
            auto j = json::parse(req.body);
            if (!j.contains("stream_id") || !j["stream_id"].is_string()) {
                response["message"] = "Missing or invalid 'stream_id'";
                res.status = 400;
            } else {
                QString streamId = QString::fromStdString(j["stream_id"].get<std::string>());

                if (mVerboseLevel > 0) {
                    qDebug() << "[HTTP] POST /record/start for stream:" << streamId;
                }

                // Emit Qt signal (async -> queued to main thread/recorder threads)
                emit startRecordingRequested(streamId);

                response["status"] = "ok";
                response["stream_id"] = j["stream_id"];
                res.status = 200;
            }
        } catch (const std::exception& e) {
            response["message"] = std::string("JSON parse error: ") + e.what();
            res.status = 400;
        }

        std::string body = response.dump();
        res.set_content(body, "application/json");
    });

    // 3) POST /record/stop
    //    Body: { "stream_id": "stream_1" }
    //    Response: { "status": "ok", "stream_id": "...", "file": "path/to/file.mp4" }
    //              or { "status": "error", "message": "..." }
    m_server.Post("/record/stop", [this](const httplib::Request& req, httplib::Response& res) {
        json response;
        response["status"] = "error";

        try {
            auto j = json::parse(req.body);
            if (!j.contains("stream_id") || !j["stream_id"].is_string()) {
                response["message"] = "Missing or invalid 'stream_id'";
                res.status = 400;
            } else {
                QString streamId = QString::fromStdString(j["stream_id"].get<std::string>());

                if (mVerboseLevel > 0) {
                    qDebug() << "[HTTP] POST /record/stop for stream:" << streamId;
                }

                // Look up last recording file for this stream
                QString filePath;
                {
                    QReadLocker locker(&m_filesLock);
                    if (m_lastRecordingFile.contains(streamId)) {
                        filePath = m_lastRecordingFile.value(streamId);
                    }
                }

                // Emit Qt signal to actually stop recording (async)
                emit stopRecordingRequested(streamId);

                if (filePath.isEmpty()) {
                    response["status"] = "warning";
                    response["stream_id"] = j["stream_id"];
                    response["file"] = nullptr;  // no known file
                    response["message"] = "No known recording file for this stream (maybe never started?)";
                } else {
                    response["status"] = "ok";
                    response["stream_id"] = j["stream_id"];
                    response["file"] = filePath.toStdString();
                }

                res.status = 200;
            }
        } catch (const std::exception& e) {
            response["message"] = std::string("JSON parse error: ") + e.what();
            res.status = 400;
        }

        std::string body = response.dump();
        res.set_content(body, "application/json");
    });

    // Default 404
    m_server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content("Not Found", "text/plain");
    });

    // ---------- Launch blocking listen() on a background thread ----------
    m_thread = std::thread([this, hostStr, portInt]() {
        emit started(QString::fromStdString(hostStr), static_cast<quint16>(portInt));
        bool ok = m_server.listen(hostStr.c_str(), portInt);
        m_running.store(false);
        emit stopped();

        if (!ok) {
            m_server.stop();
        }
    });

    return true;
}

void HttpDataServer::stop() {
    if (!m_running.load()) {
        if (m_thread.joinable()) m_thread.join();
        return;
    }
    m_server.stop();  // thread-safe; unblocks listen()
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false);
}

QByteArray HttpDataServer::readCurrentPayload(HttpDataServer* self, QString& contentTypeOut) {
    QReadLocker locker(&self->m_lock);
    contentTypeOut = self->m_contentType;
    return self->m_payload;
}

// ---------- Slots for recorder notifications ----------

void HttpDataServer::onRecordingStarted(const QString& streamId, const QString& filePath) {
    QWriteLocker locker(&m_filesLock);
    m_lastRecordingFile[streamId] = filePath;
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording started:" << streamId << "->" << filePath;
    }
}

void HttpDataServer::onRecordingStopped(const QString& streamId) {
    // We keep the lastRecordingFile entry so /record/stop can still return the last file
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording stopped:" << streamId;
    }
}
