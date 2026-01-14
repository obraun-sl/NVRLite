
#ifndef __Utils_H__
#define __Utils_H__

#include <qelapsedtimer.h>
#include <thread>
#include <QtCore/qthread.h>
#include <QtCore/qtimer.h>
#include <QMutex>
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/time.h>
}

#include <iostream>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <deque>
#include <cmath>

#include <QFile>
#include "Http/json.hpp"   // from nlohmann::json
#include <QDebug>
#include <QDir>

#define APP_VERSION "0.2.3"

// ---------------- EncodedVideoPacket (for signals) ----------------
struct EncodedVideoPacket {
    QString  streamId;
    QByteArray data;
    int64_t pts = AV_NOPTS_VALUE;
    int64_t dts = AV_NOPTS_VALUE;
    int64_t duration = 0;
    bool   key = false;
    AVRational time_base{1,1};
};

struct StreamInfo {
    QString     streamId;
    int         width{0};
    int         height{0};
    AVRational  timeBase{1, 90000};
    AVCodecID   codecId{AV_CODEC_ID_NONE};
    QByteArray extradata;
};


Q_DECLARE_METATYPE(EncodedVideoPacket)
Q_DECLARE_METATYPE(StreamInfo)
Q_DECLARE_METATYPE(AVRational)


//// Helpers ////
inline static void log_error(const std::string &msg, int errnum) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, errbuf, sizeof(errbuf));
    std::cerr << msg << " (err=" << errnum << "): " << errbuf << std::endl;
}

// helper: format epoch microseconds to "YYYY-MM-DD HH:MM:SS.uuuuuu"
static QString format_epoch_us(int64_t epoch_us) {
    std::time_t sec = epoch_us / 1000000;
    int usec = epoch_us % 1000000;

    std::tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &sec);
#else
    localtime_r(&sec, &tm_buf);
#endif

    char timebuf[64];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    std::ostringstream oss;
    oss << timebuf << "." << std::setfill('0') << std::setw(6) << usec;
    return QString::fromStdString(oss.str());
}

// helper: create a filename like "rec_2025-11-29_12-58-03.mp4"
static QString makeRecordFilename(const QString& streamId, const QString& folder)
{
    std::time_t t = std::time(nullptr);
    std::tm tm_buf{};

#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm_buf);

    QString filename = QStringLiteral("rec_%1_%2.mp4").arg(streamId, buf);

    // QDir takes care of the correct separator for the platform
    return QDir(folder).filePath(filename);
}


struct StreamConfig {
    QString id;
    QString url;
};

struct AppConfig {
    QList<StreamConfig> streamConfigs;
    quint16 httpPort = 8090;
    int displayMode = 0;
    int autostart = 0;
    float prebufferingTime = 5;
    float postbufferingTime = 0.5;
    QString rec_base_folder = "./";
};

inline static bool loadConfigFile(const QString &path,
                           AppConfig& config)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCritical() << "[CFG] Failed to open config file:" << path;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();
    qDebug()<<"[CFG] Reading config file  = "<<file.fileName();
    try {
        sl::json j = sl::json::parse(data.constData());


        // rec_base_folder (optional, default 8090)
        config.rec_base_folder = "./";
        if (j.contains("rec_base_folder") && j["rec_base_folder"].is_string()) {
            config.rec_base_folder = QString::fromStdString(j["rec_base_folder"].get<std::string>());
            QDir t_dir  = QDir(config.rec_base_folder);
            if (!t_dir.exists())
            {
                bool r = QDir().mkdir(config.rec_base_folder);
                if (r)
                    qDebug()<<"[CFG] Creating DIR = "<<config.rec_base_folder;
                else
                    qDebug()<<"[CFG] ERROR : Cannot create DIR = "<<config.rec_base_folder;
            }
        }

        // http_port (optional, default 8090)
        config.httpPort = 8090;
        if (j.contains("http_port") && j["http_port"].is_number_integer()) {
            int p = j["http_port"].get<int>();
            if (p > 0 && p <= 65535)
                config.httpPort = static_cast<quint16>(p);
        }
        else
            qWarning() << "[CFG] http_port entry not found in config. Using Default = "<<config.httpPort;

        /// Display Mode
        config.displayMode = 0;
        if (j.contains("display_mode") && j["display_mode"].is_number_integer()) {
            int p = j["display_mode"].get<int>();
            if (p > 0 && p <= 1)
                config.displayMode = p;
        }
        else
          qWarning() << "[CFG] display_mode entry not found in config. Using Default = "<<config.displayMode;

        /// Auto start stream
        config.autostart = 0;
        if (j.contains("autostart") && j["autostart"].is_number_integer()) {
            int p = j["autostart"].get<int>();
            if (p > 0 && p <= 1)
                config.autostart = p;
        }
        else
          qWarning() << "[CFG] autostart entry not found in config. Using Default = "<<config.autostart;


        /// Pre buffering Time
        config.prebufferingTime = 5.0;
        if (j.contains("pre_buffering_time") && j["pre_buffering_time"].is_number_float()) {
            config.prebufferingTime = j["pre_buffering_time"].get<float>();
        }
        else
          qWarning() << "[CFG] pre_buffering_time entry not found in config. Using Default = "<<config.prebufferingTime;

        /// Post buffering Time
        config.postbufferingTime = 0.5;
        if (j.contains("post_buffering_time") && j["post_buffering_time"].is_number_float()) {
            config.postbufferingTime = j["post_buffering_time"].get<float>();
        }
        else
          qWarning() << "[CFG] post_buffering_time entry not found in config. Using Default = "<<config.postbufferingTime;

        // streams array
        if (!j.contains("streams") || !j["streams"].is_array()) {
            qCritical() << "[CFG] 'streams' array missing or invalid in config";
            return false;
        }

        for (const auto &s : j["streams"]) {
            if (!s.contains("id") || !s.contains("url") ||
                !s["id"].is_string() || !s["url"].is_string()) {
                qWarning() << "[CFG] Skipping invalid stream entry in config";
                continue;
            }
            StreamConfig sc;
            sc.id  = QString::fromStdString(s["id"].get<std::string>());
            sc.url = QString::fromStdString(s["url"].get<std::string>());
            config.streamConfigs.push_back(sc);
        }

        if (config.streamConfigs.isEmpty()) {
            qCritical() << "[CFG] No valid streams found in config";
            return false;
        }
        return true;

    } catch (const std::exception &e) {
        qCritical() << "[CFG] JSON parse error in config:" << e.what();
        return false;
    }
}


#endif /* __Utils_H__ */
