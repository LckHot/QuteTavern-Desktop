// SPDX-License-Identifier: AGPL-3.0-or-later
#include "MainWindow.h"

#include "EnvCheck.h"

#include <QApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>

int main(int argc, char *argv[])
{
    // Qt WebEngine needs shared OpenGL contexts when several windows use it
    // (must be set before the QApplication is constructed)
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    // No organization name on purpose: configuration and runtime components live
    // right in the per-application directories (~/.config/QuteTavern,
    // ~/.local/share/QuteTavern). The SillyTavern data directory is a separate,
    // fixed location and unaffected by this.
    QApplication::setApplicationName(QStringLiteral("QuteTavern"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.2"));
    QApplication::setDesktopFileName(QStringLiteral("qutetavern.desktop"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/icon.png")));

    // Single instance: an already running instance is asked to raise itself
    const QString key = QStringLiteral("qutetavern-singleton");
    {
        QLocalSocket probe;
        probe.connectToServer(key);
        if (probe.waitForConnected(300)) {
            probe.write("raise\n");
            probe.flush();
            probe.waitForBytesWritten(300);
            return 0;
        }
    }
    QLocalServer::removeServer(key);
    QLocalServer server;
    server.listen(key);

    MainWindow w;

    // Environment check: when node/git are missing the user can either quit or
    // download portable copies into the application data directory. Quitting
    // terminates the application.
    if (!EnvCheck::ensureEnvironment(nullptr))
        return 0;

    w.show();

    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            QLocalSocket *conn = server.nextPendingConnection();
            conn->deleteLater();
        }
        w.show();
        w.raise();
        w.activateWindow();
    });

    return app.exec();
}