#include "Http/HttpHandler.hpp"
#include <QDebug>
#include <QDateTime>
#include <chrono>
#include <QFileInfo>
#include <QFile>
#include <QDir>

/// Helpers
static bool isSafeBasename(const QString& f) {
    // Only allow a simple basename; disallow traversal and path separators
    if (f.isEmpty()) return false;
    if (f.contains("..")) return false;
    if (f.contains('/') || f.contains('\\')) return false;
    return true;
}

static QString resolveUnderBase(const QString& baseDir, const QString& baseName) {
    QDir root(baseDir);
    return root.absoluteFilePath(baseName);
}


/// Class
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
                const QString streamId = QString::fromStdString(j["stream_id"].get<std::string>());

                // Validate stream_id: must exist in configured/known streams
                // if not, return error.
                {
                    QReadLocker locker(&m_filesLock);
                    if (!m_knownStreams.contains(streamId)) {
                        response["status"]  = "failed";
                        response["message"] = "Unknown 'stream_id'";
                        res.status = 404;
                        res.set_content(response.dump(), "application/json");
                        return;
                    }
                }

                if (mVerboseLevel > 0) {
                    qDebug() << "[HTTP] POST /record/start for stream:" << streamId;
                }

                // if already recording or start already pending, return ok.
                {
                    QWriteLocker locker(&m_filesLock);
                    const bool isRec = m_recordingState.value(streamId, false);
                    const bool isPending = m_recordingPending.value(streamId, false);
                    if (isRec) {
                        response["status"] = "ok";
                        response["stream_id"] = j["stream_id"];
                        response["message"] = "already recording";
                        if (m_lastRecordingFile.contains(streamId)) {
                            response["file"] = m_lastRecordingFile.value(streamId).toStdString();
                        } else {
                            response["file"] = nullptr;
                        }
                        res.status = 200;
                        res.set_content(response.dump(), "application/json");
                        return;
                    }
                    if (isPending) {
                        response["status"] = "ok";
                        response["stream_id"] = j["stream_id"];
                        response["message"] = "start already pending";
                        res.status = 202;
                        res.set_content(response.dump(), "application/json");
                        return;
                    }

                    // Mark start pending right away so a fast /record/stop won't think "never started".
                    m_recordingPending[streamId] = true;
                    m_stopPending[streamId] = false;
                    // Clear any stale file from prior runs; recorder will fill it on onRecordingStarted().
                    m_lastRecordingFile.remove(streamId);
                }

                // Emit Qt signal (async -> queued to main thread/recorder threads)
                emit startRecordingRequested(streamId);

                // Try briefly to get the file path (if the recorder is fast enough, we can return a definitive 200).
                QString filePath;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                // Wait maximum 2 seconds
                while (std::chrono::steady_clock::now() < deadline) {
                    {
                        QReadLocker locker(&m_filesLock);
                        if (m_lastRecordingFile.contains(streamId)) {
                            filePath = m_lastRecordingFile.value(streamId);
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }

                // Get the result if available. Return OK if found / timeout if not.
                response["stream_id"] = j["stream_id"];

                if (filePath.isEmpty()) {
                    response["status"]  = "failed";
                    response["message"] = "timeout waiting for recording file to be created/known";
                    response["file"]    = nullptr;
                    res.status = 500;
                } else {
                    response["status"] = "ok";
                    response["file"]   = filePath.toStdString();
                    res.status = 200;
                }
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
                const QString streamId = QString::fromStdString(j["stream_id"].get<std::string>());

                // Validate stream_id
                {
                    QReadLocker locker(&m_filesLock);
                    if (!m_knownStreams.contains(streamId)) {
                        response["status"]  = "failed";
                        response["message"] = "Unknown 'stream_id'";
                        res.status = 404;
                        res.set_content(response.dump(), "application/json");
                        return;
                    }
                }

                if (mVerboseLevel > 0) {
                    qDebug() << "[HTTP] POST /record/stop for stream:" << streamId;
                }

                bool wasRecording = false;
                bool wasPending   = false;
                {
                    QReadLocker locker(&m_filesLock);
                    wasRecording = m_recordingState.value(streamId, false);
                    wasPending   = m_recordingPending.value(streamId, false);
                }

                // Idempotent: if not recording and no pending start, return ok.
                if (!wasRecording && !wasPending) {
                    response["status"] = "ok";
                    response["stream_id"] = j["stream_id"];
                    response["message"] = "not recording";
                    res.status = 200;
                    res.set_content(response.dump(), "application/json");
                    return;
                }

                // If start is still pending (file not known yet), remember the stop request so we can stop as soon as start is confirmed.
                if (wasPending && !wasRecording) {
                    QWriteLocker locker(&m_filesLock);
                    m_stopPending[streamId] = true;
                }

                // Emit Qt signal to actually stop recording (async)
                emit stopRecordingRequested(streamId);

                // Try briefly to obtain the file path (covers race: start accepted but onRecordingStarted not yet delivered).
                QString filePath;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
                while (std::chrono::steady_clock::now() < deadline) {
                    {
                        QReadLocker locker(&m_filesLock);
                        if (m_lastRecordingFile.contains(streamId)) {
                            filePath = m_lastRecordingFile.value(streamId);
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }

                response["status"] = "ok";
                response["stream_id"] = j["stream_id"];
                if (filePath.isEmpty()) {
                    response["file"] = nullptr;
                    response["message"] = "stop requested; recording file not yet known";
                } else {
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

    // POST /files/remove
    // Accepts either:
    //   - query: ?file=xxxx
    //   - JSON body: { "file": "xxxx" }
    m_server.Post("/files/remove", [this](const httplib::Request& req, httplib::Response& res) {
        json response;
        response["status"] = "error";

        QString fileParam;

        // Prefer query param if present
        if (req.has_param("file")) {
            fileParam = QString::fromStdString(req.get_param_value("file"));
        } else {
            // Try JSON body
            try {
                if (!req.body.empty()) {
                    auto j = json::parse(req.body);
                    if (j.contains("file") && j["file"].is_string()) {
                        fileParam = QString::fromStdString(j["file"].get<std::string>());
                    }
                }
            } catch (...) {
                // ignore JSON parse error, handled below
            }
        }

        if (fileParam.isEmpty()) {
            response["message"] = "Missing 'file' (query ?file=... or JSON body {\"file\":\"...\"})";
            res.status = 400;
            res.set_content(response.dump(), "application/json");
            return;
        }

        if (!isSafeBasename(fileParam)) {
            response["message"] = "Invalid 'file' (must be basename only, no path/traversal)";
            res.status = 400;
            res.set_content(response.dump(), "application/json");
            return;
        }

        const QString filePath = resolveUnderBase(mFolderBasePath, fileParam);
        QFileInfo fi(filePath);

        if (!fi.exists() || !fi.isFile()) {
            response["status"]  = "failed";
            response["message"] = "File not found";
            response["file"]    = fileParam.toStdString();
            res.status = 404;
            res.set_content(response.dump(), "application/json");
            return;
        }

        QFile f(filePath);
        if (!f.remove()) {
            response["status"]  = "failed";
            response["message"] = "Failed to delete file";
            response["file"]    = fileParam.toStdString();
            res.status = 500;
            res.set_content(response.dump(), "application/json");
            return;
        }

        response["status"] = "ok";
        response["file"]   = fileParam.toStdString();
        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });


    // GET /files/status?file=xxxxxx
    m_server.Get("/files/status", [this](const httplib::Request& req, httplib::Response& res) {
        json response;
        response["status"] = "error";

        if (!req.has_param("file")) {
            response["message"] = "Missing 'file' query parameter";
            res.status = 400;
            res.set_content(response.dump(), "application/json");
            return;
        }

        const QString fileParam = QString::fromStdString(req.get_param_value("file"));
        if (!isSafeBasename(fileParam)) {
            response["message"] = "Invalid 'file' (must be basename only, no path/traversal)";
            res.status = 400;
            res.set_content(response.dump(), "application/json");
            return;
        }

        const QString filePath = resolveUnderBase(mFolderBasePath, fileParam);
        QFileInfo fi(filePath);

        if (!fi.exists() || !fi.isFile()) {
            response["status"]  = "failed";
            response["message"] = "File not found";
            response["file"]    = fileParam.toStdString();
            res.status = 404;
            res.set_content(response.dump(), "application/json");
            return;
        }

        response["status"] = "ok";
        response["file"]   = fileParam.toStdString();
        response["path"]   = fi.absoluteFilePath().toStdString();
        response["folder_base"] = mFolderBasePath.toStdString();

        response["size_bytes"] = static_cast<long long>(fi.size());
        response["suffix"] = fi.suffix().toStdString();

        response["last_modified_utc"] =
            fi.lastModified().toUTC().toString(Qt::ISODate).toStdString();

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        response["birth_time_utc"] =
            fi.birthTime().toUTC().toString(Qt::ISODate).toStdString();
#endif

        // created() exists in Qt5 but may be filesystem-dependent; keep it anyway
        response["created_utc"] =
            fi.created().toUTC().toString(Qt::ISODate).toStdString();

        response["is_readable"] = fi.isReadable();
        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });

    // GET /files/list
    // Optional:
    //   ?ext=mp4   (default: mp4)
    //   ?all=1     (list all files, ignore ext)
    m_server.Get("/files/list", [this](const httplib::Request& req, httplib::Response& res) {
        json response;

        QDir dir(mFolderBasePath);
        if (!dir.exists()) {
            response["status"]  = "failed";
            response["message"] = "Base folder does not exist";
            response["folder_base"] = mFolderBasePath.toStdString();
            res.status = 500;
            res.set_content(response.dump(), "application/json");
            return;
        }

        bool listAll = false;
        if (req.has_param("all")) {
            const std::string v = req.get_param_value("all");
            listAll = (v == "1" || v == "true" || v == "yes");
        }

        QString ext = "mp4";
        if (req.has_param("ext")) {
            ext = QString::fromStdString(req.get_param_value("ext")).trimmed();
            if (ext.startsWith('.')) ext = ext.mid(1);
            if (ext.isEmpty()) ext = "mp4";
        }

        // Only files, no directories. Newest first.
        dir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks);
        dir.setSorting(QDir::Time | QDir::Reversed);

        if (!listAll) {
            dir.setNameFilters(QStringList() << QString("*.%1").arg(ext));
        }

        const QFileInfoList entries = dir.entryInfoList();

        json files = json::array();
        for (const QFileInfo& fi : entries) {
            if (!fi.isFile()) continue;

            json f;
            f["name"] = fi.fileName().toStdString();
            f["size_bytes"] = static_cast<long long>(fi.size());
            f["last_modified_utc"] =
                fi.lastModified().toUTC().toString(Qt::ISODate).toStdString();

            files.push_back(f);
        }

        response["status"] = "ok";
        response["folder_base"] = mFolderBasePath.toStdString();
        response["count"] = static_cast<int>(files.size());
        response["ext_filter"] = listAll ? "*" : ext.toStdString();
        response["files"] = files;

        res.status = 200;
        res.set_content(response.dump(), "application/json");
    });



    // Default 404 Error
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
    m_recordingPending[streamId]  = false;
    m_knownStreams.insert(streamId);          // ensure it's known

    // If a /record/stop arrived while start was pending, stop immediately now that we know the file.
    const bool stopNow = m_stopPending.value(streamId, false);
    m_stopPending[streamId] = false;
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording started:" << streamId << "->" << filePath;
    }

    locker.unlock();
    if (stopNow) {
        if (mVerboseLevel > 0) {
            qDebug() << "[HTTP] Stop was requested while start was pending; stopping now:" << streamId;
        }
        emit stopRecordingRequested(streamId);
    }
}

void HttpDataServer::onRecordingStopped(const QString& streamId) {
    QWriteLocker locker(&m_filesLock);
    // We keep the lastRecordingFile entry so /record/stop can still return the last file
    m_recordingState[streamId]   = false;
    m_recordingPending[streamId] = false;
    m_stopPending[streamId]      = false;
    m_knownStreams.insert(streamId);          // ensure it's known
    if (mVerboseLevel > 0) {
        qDebug() << "[HTTP] Recording stopped:" << streamId;
    }
}
