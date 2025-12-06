#include "Capture/CaptureWorker.hpp"
#include "Display/DisplayManager.hpp"
#include "Recording/MP4Recorder.hpp"
#include "Http/HttpHandler.hpp"
#include <QCoreApplication>


int main(int argc, char *argv[]) {

    av_register_all();
    avformat_network_init();

    QCoreApplication app(argc, argv);
    app.setApplicationName("NVRLite");
    app.setApplicationVersion(APP_VERSION);
    qDebug()<<" Version "<<APP_VERSION;

    // --config <file> ---
    QString configPath;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--config" && i + 1 < argc) {
            configPath = QString::fromLocal8Bit(argv[++i]);
        } else {
            qWarning() << "[CFG] Unknown argument ignored:" << arg;
        }
    }

    if (configPath.isEmpty()) {
        qCritical() << "Usage:";
        qCritical() << "  " << argv[0] << " --config config.json";
        return -1;
    }

    qRegisterMetaType<EncodedVideoPacket>("EncodedVideoPacket");
    qRegisterMetaType<StreamInfo>("StreamInfo");
    qRegisterMetaType<cv::Mat>("cv::Mat");
    qRegisterMetaType<AVRational>("AVRational");


    // --- Load config ---
    AppConfig mAppConfig;
    if (!loadConfigFile(configPath,mAppConfig)) {
        return -1;
    }


    QList<RtspCaptureThread*> captureThreads;
    QHash<QString, Mp4RecorderWorker*> recorders;
    QHash<QString, RtspCaptureThread*> captureById;
    QList<QThread*> recorderThreads;
    QStringList streamIds;

    // --- Create per-stream capture + per-stream recorder thread ---
    for (const auto &cfg : mAppConfig.streamConfigs) {
        const QString &url      = cfg.url;
        const QString &streamId = cfg.id;
        streamIds << streamId;

        auto *cap = new RtspCaptureThread(streamId, url, &app);
        cap->setWithUserInterface((bool)mAppConfig.displayMode);
        captureThreads << cap; /// Add in list for display
        captureById.insert(streamId, cap); /// Add in Hash for Http Connection

        // Recorder worker + recorder thread
        QThread *recThread = new QThread(&app);
        Mp4RecorderWorker *recWorker = new Mp4RecorderWorker(streamId);
        recWorker->moveToThread(recThread);

        QObject::connect(recThread, &QThread::finished,
                         recWorker, &QObject::deleteLater);

        // Connect capture -> recorder (queued)
        QObject::connect(cap, &RtspCaptureThread::videoPacketReady,
                         recWorker, &Mp4RecorderWorker::onPacket,
                         Qt::QueuedConnection);
        QObject::connect(cap, &RtspCaptureThread::streamInfoReady,
                         recWorker, &Mp4RecorderWorker::onStreamInfo,
                         Qt::QueuedConnection);

        recorders.insert(streamId, recWorker);
        recorderThreads << recThread;

        recThread->start();
    }

    // Display manager
    DisplayManager* display=nullptr;
    if (mAppConfig.displayMode==1)
    {
        display = new DisplayManager(&recorders, streamIds);
        for (auto *cap : captureThreads) {
            QObject::connect(cap, &RtspCaptureThread::frameReady,
                             display, &DisplayManager::onFrame,
                             Qt::QueuedConnection);
        }
    }

    HttpDataServer httpServer;
    httpServer.setVerboseLevel(1);

    // For each streamId ==> recorder:
    for (const auto &streamId : streamIds) {
        Mp4RecorderWorker* recWorker = recorders[streamId];

        // HTTP -> Recorder (via signals)
        QObject::connect(&httpServer, &HttpDataServer::startRecordingRequested,
                         [recWorker, streamId](const QString &id){
                             if (id == streamId)
                                 QMetaObject::invokeMethod(recWorker, "startRecording", Qt::QueuedConnection);
                         });

        QObject::connect(&httpServer, &HttpDataServer::stopRecordingRequested,
                         [recWorker, streamId](const QString &id){
                             if (id == streamId)
                                 QMetaObject::invokeMethod(recWorker, "stopRecording", Qt::QueuedConnection);
                         });

        // Recorder -> HTTP (to track file names)
        QObject::connect(recWorker, &Mp4RecorderWorker::recordingStarted,
                         &httpServer, &HttpDataServer::onRecordingStarted,
                         Qt::QueuedConnection);

        QObject::connect(recWorker, &Mp4RecorderWorker::recordingStopped,
                         &httpServer, &HttpDataServer::onRecordingStopped,
                         Qt::QueuedConnection);
    }

    // HTTP -> Capture threads: stream start/stop
    for (const auto &streamId : streamIds) {
        RtspCaptureThread *cap = captureById.value(streamId, nullptr);
        if (!cap) continue;
        QObject::connect(&httpServer, &HttpDataServer::startStreamRequested,
                         cap, &RtspCaptureThread::onStreamStartRequested, Qt::QueuedConnection);
        QObject::connect(&httpServer, &HttpDataServer::stopStreamRequested,
                         cap, &RtspCaptureThread::onStreamStopRequested, Qt::QueuedConnection);

        if (mAppConfig.autostart==1)
           cap->onStreamStartRequested(streamId);
    }

    // Start HTTP server
    httpServer.start("0.0.0.0", mAppConfig.httpPort);


    // Start all capture threads
    for (auto *cap : captureThreads) {
        cap->start();
    }

    int ret = app.exec();

    // Clean up capture threads
    for (auto *cap : captureThreads) {
        cap->requestInterruption();
        cap->wait();
        delete cap;
    }

    // Stop recorder threads
    for (auto *t : recorderThreads) {
        t->quit();
        t->wait();
        delete t;
    }

    if (display)
        delete display;


    avformat_network_deinit();
    cv::destroyAllWindows();
    return 0;
}
