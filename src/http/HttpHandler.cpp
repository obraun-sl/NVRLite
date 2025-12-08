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

    // --- Create Routes ----
    createRoutes();

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



void HttpDataServer::createRoutes()
{
    // ---------- ROUTES ----------
    // ==> POST /stream/start
    // ==> POST /stream/stop
    // ==> POST /record/start
    // ==> POST /record/stop


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

    // 2) POST /record/stop
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


    // 3) --- POST /stream/start
    // Body: { "stream_id": "stream_1" }
    // Emits: startStreamRequested(QString)
    m_server.Post("/stream/start", [this](const httplib::Request& req, httplib::Response& res) {
        json response;
        response["status"] = "error";
         try {
            auto j = json::parse(req.body);
            std::string sid;
            std::string err;
            if (!j.contains("stream_id") || !j["stream_id"].is_string()) {
                response["message"] = "Missing or invalid 'stream_id'";
                res.status = 400;
            } else {
                sid = j["stream_id"];
                QString streamId = QString::fromStdString(sid);
                if (mVerboseLevel > 0) {
                    qDebug() << "[HTTP] POST /stream/start for stream:" << streamId;
                }
                emit startStreamRequested(streamId);
                response["status"] = "ok";
                response["stream_id"] = sid;
                res.status = 200;
            }
        } catch (const std::exception& e) {
            response["message"] = std::string("JSON parse error: ") + e.what();
            res.status = 400;
        }
        res.set_content(response.dump(), "application/json");
    });


     // 4) --- POST /stream/stop
     // Body: { "stream_id": "stream_1" }
     // Emits: stopStreamRequested(QString)
     m_server.Post("/stream/stop", [this](const httplib::Request& req, httplib::Response& res) {
         json response;
         response["status"] = "error";

         try {
             auto j = json::parse(req.body);
             std::string sid;
             std::string err;
             if (!j.contains("stream_id") || !j["stream_id"].is_string()) {
                 response["message"] = "Missing or invalid 'stream_id'";
                 res.status = 400;
             } else {
                 sid = j["stream_id"];
                 QString streamId = QString::fromStdString(sid);
                 if (mVerboseLevel > 0) {
                     qDebug() << "[HTTP] POST /stream/stop for stream:" << streamId;
                 }

                 emit stopStreamRequested(streamId);
                 response["status"] = "ok";
                 response["stream_id"] = sid;
                 res.status = 200;
             }
         } catch (const std::exception& e) {
             response["message"] = std::string("JSON parse error: ") + e.what();
             res.status = 400;
         }
         res.set_content(response.dump(), "application/json");
     });


     // 5) GET /stream/status
     //    Optional query param: ?stream_id=stream_1
     //    Response (all streams):
     //    {
     //      "status": "ok",
     //      "streams": [
     //        { "stream_id": "stream_1", "recording": true, "streaming": true, "file": "rec_stream_1_....mp4" },
     //        { "stream_id": "stream_2", "recording": false, "streaming": true, "file": null }
     //        { "stream_id": "stream_3", "recording": false, "streaming": false, "file": null }
     //      ]
     //    }
     //
     //    Or single stream:
     //    {
     //      "status": "ok",
     //      "stream": {
     //        "stream_id": "stream_1",
     //        "recording": true,
     //        "streaming": true,
     //        "file": "rec_stream_1_....mp4"
     //      }
     //    }
     //
     //    If unknown stream_id: { "status": "not_found", "message": "..."} (HTTP 404)
     m_server.Get("/stream/status", [this](const httplib::Request& req, httplib::Response& res) {
         json response;

         // --- Single stream mode: ?stream_id=<xxx> ---
         if (req.has_param("stream_id")) {
             const std::string sid = req.get_param_value("stream_id");
             const QString streamId = QString::fromStdString(sid);

             QReadLocker locker(&m_filesLock);
             const bool known = m_knownStreams.contains(streamId);

             if (!known) {
                 response["status"]  = "not_found";
                 response["message"] = "Unknown stream_id";
                 res.status = 404;
             } else {
                 json s;
                 s["stream_id"] = sid;

                 bool streaming = m_streamingState.value(streamId, false);
                 s["streaming"] = streaming;

                 bool rec = m_recordingState.value(streamId, false);
                 s["recording"] = rec;

                 if (m_lastRecordingFile.contains(streamId)) {
                     s["file"] = m_lastRecordingFile.value(streamId).toStdString();
                 } else {
                     s["file"] = nullptr;
                 }

                 response["status"] = "ok";
                 response["stream"] = s;
                 res.status = 200;
             }

             res.set_content(response.dump(), "application/json");
             return;
         }

         // --- All streams mode ---
         json streams = json::array();
         {
             QReadLocker locker(&m_filesLock);

             for (const QString &id : m_knownStreams) {
                 json s;
                 const std::string sid = id.toStdString();
                 s["stream_id"] = sid;

                 bool streaming = m_streamingState.value(id, false);
                 s["streaming"] = streaming;

                 bool rec = m_recordingState.value(id, false);
                 s["recording"] = rec;

                 if (m_lastRecordingFile.contains(id)) {
                     s["file"] = m_lastRecordingFile.value(id).toStdString();
                 } else {
                     s["file"] = nullptr;
                 }

                 streams.push_back(s);
             }
         }

         response["status"]  = "ok";
         response["streams"] = streams;
         res.status = 200;
         res.set_content(response.dump(), "application/json");
     });


    // Default 404
    m_server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content("Not Found", "text/plain");
    });
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

void HttpDataServer::onStreamOnlineChanged(const QString &streamId, bool online)
{
    QWriteLocker locker(&m_filesLock);
    m_knownStreams.insert(streamId);
    m_streamingState[streamId] = online;
}

void HttpDataServer::registerStream(const QString &streamId)
{
    QWriteLocker locker(&m_filesLock);
    if (!m_knownStreams.contains(streamId)) {
        m_knownStreams.insert(streamId);
        // initialize default state
        if (!m_recordingState.contains(streamId)) {
            m_recordingState.insert(streamId, false);
        }
    }
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Registered stream:" << streamId;
    }
}


// ---------- Slots for recorder notifications ----------

void HttpDataServer::onRecordingStarted(const QString& streamId, const QString& filePath) {
    QWriteLocker locker(&m_filesLock);
    m_lastRecordingFile[streamId] = filePath;
    m_recordingState[streamId]    = true;
    m_knownStreams.insert(streamId);          // ensure it's known
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording started:" << streamId << "->" << filePath;
    }
}

void HttpDataServer::onRecordingStopped(const QString& streamId) {
    // We keep the lastRecordingFile entry so /record/stop can still return the last file
    m_recordingState[streamId] = false;
    m_knownStreams.insert(streamId);          // ensure it's known
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording stopped:" << streamId;
    }
}
