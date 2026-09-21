// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTemporaryFile>
#include <QUrl>

#include <functional>

// Startup environment check (Qt-free UI): when node/git are missing the QML
// dialog (qml/EnvDialog.qml) offers to download portable copies into the
// application data directory, or to quit and install them manually.
//
// Only the UI moved to QML - detection, the release queries, the streaming
// download and the asynchronous extraction are the same code the widget
// version used; progress and status are exposed as properties instead of
// being written into widgets.
class EnvCheck : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool visible READ visible NOTIFY changed)
    Q_PROPERTY(QString body READ body CONSTANT) // what is missing (per platform)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed) // -1 = indeterminate
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool done READ done NOTIFY changed)
    Q_PROPERTY(bool canDownload READ canDownload NOTIFY changed)
    Q_PROPERTY(QString downloadLabel READ downloadLabel NOTIFY changed)
    Q_PROPERTY(QString exitLabel READ exitLabel NOTIFY changed)

public:
    explicit EnvCheck(QObject *parent = nullptr);

    bool visible() const { return m_visible; }
    QString body() const { return m_body; }
    QString status() const { return m_status; }
    double progress() const { return m_progress; }
    bool busy() const { return m_busy; }
    bool done() const { return m_done; }
    bool canDownload() const { return m_canDownload; }
    QString downloadLabel() const { return m_downloadLabel; }
    QString exitLabel() const { return m_exitLabel; }

    // Start the downloads (node, then MinGit on Windows)
    Q_INVOKABLE void download();
    // "Continue": the components are installed, let the application start
    Q_INVOKABLE void accept();
    // "Quit" (or closing the dialog without finishing): terminate the application
    Q_INVOKABLE void quit();

signals:
    void changed();
    void quitRequested();

private:
    void nextStep();
    void fetchNodeVersion();
#ifdef Q_OS_WIN
    void fetchMinGitUrl(); // MinGit is the portable git fallback for Windows only
#endif
    void downloadToFile(const QUrl &url, const std::function<void(const QString &)> &onSaved);
    // Extraction blocks for seconds to minutes (the system tar runs to
    // completion); it happens on a worker and reports back on the main thread
    void extractArchiveAsync(const QString &archive, const QString &destDir, int stripComponents,
                             const std::function<void(bool, const QString &err)> &done);
    void finishOk();
    void setStatus(const QString &text);
    void setProgress(double value);
    void setBusy(bool busy);

    bool m_visible = false;
    QString m_body;
    QString m_status;
    double m_progress = -1;
    bool m_busy = false;
    bool m_done = false;
    bool m_aborted = false;
    bool m_canDownload = true;
    QString m_downloadLabel;
    QString m_exitLabel = QStringLiteral("Quit");

    bool m_nodeMissing = false;
    bool m_gitMissing = false;
    int m_step = 0; // 0 = node, 1 = git
    QString m_arch; // Node download identifier (x64/arm64); empty = unsupported CPU
    QString m_nodeVersion;
    QNetworkAccessManager m_nam;
    QTemporaryFile m_tmp;
};
