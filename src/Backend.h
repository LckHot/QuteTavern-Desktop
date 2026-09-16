// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include <atomic>

class QThread;

// SillyTavern backend process management (QProcess state machine, everything on
// the main thread): probe (HTTP) -> foreign instance decided by the UI ->
// dependency check -> spawn node -> parse the "Go to:" URL -> Running.
// Graceful stop is SIGTERM -> 5s -> SIGKILL.
class Backend : public QObject {
    Q_OBJECT

public:
    enum class Status { Stopped, Starting, Running, Stopping, Error };
    Q_ENUM(Status)

    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    void start();                 // async; foreign instances are reported via foreignInstanceFound
    Q_INVOKABLE void stop();      // invokable by name (the update flow requests a stop from a worker)
    void installDepsThenStart();  // "install dependencies and retry" from the error state
    void foreignAttach(int port); // UI decision: connect to the running foreign instance
    void foreignTakeover();       // UI decision: kill the foreign instance and take over
    void foreignCancel();         // UI decision: cancel the startup

    Status status() const { return m_status; }
    QString url() const { return m_url; }
    QString lastError() const { return m_lastError; }
    bool needsNpmInstall() const { return m_needsNpm; }
    QString stRoot() const;

    // Block until the backend has fully stopped (called from worker threads)
    bool waitStopped(int timeoutMs) const;

signals:
    void logLine(const QString &line);
    void stateChanged();
    void foreignInstanceFound(int port); // port is busy; waiting for a foreignXxx decision

public slots:
    // Log entry point for the install/update workers: they reach it through
    // QMetaObject::invokeMethod(qApp, ...) because log() appends to m_tail and
    // is only safe on this object's own thread
    void log(const QString &line);

private slots:
    void onProbeConnected();
    void onProbeReadyRead();
    void onProbeFailed();
    void onProcReadyRead();
    void onProcFinished(int code, QProcess::ExitStatus st);
    void onProcError(QProcess::ProcessError e);
    void onNpmReadyRead();
    void onNpmFinished(int code, QProcess::ExitStatus st);
    void onNpmError(QProcess::ProcessError e);
    void onNpmTimeout();
    void onStartTimeout();
    void onKillTimeout();

private:
    void beginProbe();
    void probeDone(bool alive);
    void afterProbeContinue(); // dependency check -> spawn
    void startNpmInstall();
    void spawnNode();
    void setStatus(Status s);
    void setRunning(const QString &url);
    void setError(const QString &msg, bool needsNpm);
    void finishStop();
    void cleanupProbe();
    void classifyExit(int code);

    Status m_status = Status::Stopped;
    QString m_url;
    QString m_lastError;
    bool m_needsNpm = false;
    bool m_stopping = false;
    bool m_timeoutAbort = false;
    bool m_npmTimeout = false;

    QProcess *m_proc = nullptr;
    QProcess *m_npm = nullptr;
    QTcpSocket *m_probe = nullptr;
    QThread *m_takeoverThread = nullptr;
    QTimer m_probeTimer;
    QTimer m_startTimer;  // 90s readiness timeout
    QTimer m_killTimer;   // 5s hard kill after SIGTERM
    QTimer m_npmTimer;    // 10min dependency installation timeout
    QByteArray m_lineBuf; // line splitting buffer for node output
    QByteArray m_probeBuf;// probe reply buffer (accumulated until the first line break)
    QStringList m_tail;   // recent log lines of this run (used to classify exits)
    std::atomic_bool m_nodeAlive{false};
};