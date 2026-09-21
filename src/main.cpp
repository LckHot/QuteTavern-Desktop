// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AppController.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QWindow>

#include <QtWebEngineQuick/qtwebenginequickglobal.h>

int main(int argc, char *argv[])
{
    // Qt WebEngine needs shared OpenGL contexts when several windows use it
    // (must be set before the QGuiApplication is constructed)
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    // Qt Quick entry point of Qt WebEngine (official order: before the app object)
    QtWebEngineQuick::initialize();

    QGuiApplication app(argc, argv);
    // No organization name on purpose: configuration and runtime components live
    // right in the per-application directories (~/.config/QuteTavern,
    // ~/.local/share/QuteTavern). The SillyTavern data directory is a separate,
    // fixed location and unaffected by this.
    QGuiApplication::setApplicationName(QStringLiteral("QuteTavern"));
    QGuiApplication::setApplicationVersion(QStringLiteral("2.0.0"));
    QGuiApplication::setDesktopFileName(QStringLiteral("qutetavern.desktop"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));

    // Single instance: an already running instance is asked to raise itself.
    // A first instance that is still starting up may not answer within the
    // first window, so the probe retries once before we assume it is dead.
    const QString key = QStringLiteral("qutetavern-singleton");
    for (int attempt = 0; attempt < 2; ++attempt) {
        QLocalSocket probe;
        probe.connectToServer(key);
        if (probe.waitForConnected(attempt == 0 ? 300 : 1500)) {
            probe.write("raise\n");
            probe.flush();
            probe.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer server;
    if (!server.listen(key)) {
        // Stale socket left behind by a crashed instance: remove it and retry.
        // This only runs after listen() failed - removing the socket
        // unconditionally could steal it from a live instance (see above).
        QLocalServer::removeServer(key);
        if (!server.listen(key))
            qWarning("single-instance socket %s is unavailable; starting without it",
                     qPrintable(key));
    }

    // The whole UI lives in QML (qml/Main.qml); this object is the only bridge to
    // the C++ logic layer (Backend / Installer / Updater / Util / Settings /
    // EnvCheck). The environment check (node/git) is owned by it as well: the
    // QML dialog appears on top of the management window instead of blocking
    // before it is shown.
    AppController controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("App"), &controller);
    engine.loadFromModule("QuteTavern", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;

    // Raising applies to the management window; the ST window is raised through
    // AppController::raiseStWindow() instead.
    auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            QLocalSocket *conn = server.nextPendingConnection();
            conn->deleteLater();
        }
        if (!window)
            return;
        window->show();
        window->raise();
        window->requestActivate();
    });

    return app.exec();
}
