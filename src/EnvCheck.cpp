// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EnvCheck.h"

#include "Util.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSysInfo>
#include <QThread>

namespace {

// Fallback version used when the release index cannot be queried
constexpr const char *kFallbackNodeVersion = "v26.8.2";

// CPU architecture -> Node.js download identifier. Only 64 bit x86/arm are
// supported; anything else returns an empty string (never guess an instruction
// set silently - better to disable the download and say so).
// Note: MinGit has no native arm64 build for Windows; the x64 one runs under
// the system emulation layer.
QString nodeArch()
{
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == QStringLiteral("x86_64") || cpu == QStringLiteral("amd64"))
        return QStringLiteral("x64");
    if (cpu == QStringLiteral("arm64") || cpu == QStringLiteral("aarch64"))
        return QStringLiteral("arm64");
    return QString();
}

QString nodeDownloadUrl(const QString &version, const QString &arch)
{
    const QString v = version.startsWith('v') ? version : QStringLiteral("v") + version;
#if defined(Q_OS_WIN)
    return QStringLiteral("https://nodejs.org/dist/%1/node-%1-win-%2.zip").arg(v, arch);
#elif defined(Q_OS_MAC)
    return QStringLiteral("https://nodejs.org/dist/%1/node-%1-darwin-%2.tar.gz").arg(v, arch);
#else
    return QStringLiteral("https://nodejs.org/dist/%1/node-%1-linux-%2.tar.gz").arg(v, arch);
#endif
}

} // namespace

EnvCheck::EnvCheck(QObject *parent)
    : QObject(parent)
    , m_nodeVersion(QString::fromLatin1(kFallbackNodeVersion))
{
    m_nodeMissing = Util::findCommand(QStringLiteral("node")).isEmpty();
    m_gitMissing = Util::findCommand(QStringLiteral("git")).isEmpty();
    if (!m_nodeMissing && !m_gitMissing)
        return; // nothing to do: the dialog stays hidden

    m_visible = true;
    m_arch = nodeArch();
    const QString cpu = QSysInfo::currentCpuArchitecture();

    QString body = QStringLiteral("SillyTavern needs the following components; these are missing:\n\n");
    body += QStringLiteral("CPU architecture: %1%2\n\n")
                .arg(cpu,
                     m_arch.isEmpty()
                         ? QStringLiteral(" (no official Node.js build for it)")
                         : QStringLiteral(" (will download the %1 build)").arg(m_arch));
    if (m_nodeMissing)
        body += QStringLiteral(
            "  ✗ Node.js - will download the official portable build (~30 MB) "
            "into the application directory\n");
    else
        body += QStringLiteral("  ✓ Node.js\n");
#if defined(Q_OS_WIN)
    if (m_gitMissing)
        body += QStringLiteral(
            "  ✗ git - will download Git for Windows (MinGit, ~45 MB) "
            "into the application directory\n");
    else
        body += QStringLiteral("  ✓ git\n");
#elif defined(Q_OS_MAC)
    if (m_gitMissing)
        body += QStringLiteral(
            "  ✗ git - on macOS git ships with the Xcode command line tools, "
            "run: xcode-select --install\n");
    else
        body += QStringLiteral("  ✓ git\n");
#else
    if (m_gitMissing)
        body += QStringLiteral(
            "  ✗ git - there is no official portable build for Linux; install it "
            "from your distribution, e.g. sudo dnf install git\n"
            "      (without git, \"Install a new copy\" and \"Check for updates\" are "
            "unavailable; starting the backend still works)\n");
    else
        body += QStringLiteral("  ✓ git\n");
#endif
    body += QStringLiteral(
        "\nDownloaded components are stored in the application data directory and do "
        "not affect the system environment. You can also quit and install them yourself.");
    m_body = body;

    m_downloadLabel = m_nodeMissing
                          ? QStringLiteral("Download and install into the application directory")
                          : QStringLiteral("Download git into the application directory");
    // Unsupported CPU architecture: never guess an instruction set - disable the
    // download and point at a manual installation instead
    if (m_arch.isEmpty()) {
        m_canDownload = false;
        m_status = QStringLiteral(
                       "There is no official Node.js binary for this CPU architecture (%1).\n"
                       "Please quit and install Node.js (>= 22) and git manually, "
                       "then start the app again.")
                       .arg(cpu);
    }
}

void EnvCheck::setStatus(const QString &text)
{
    if (m_status == text)
        return;
    m_status = text;
    emit changed();
}

void EnvCheck::setProgress(double value)
{
    if (qFuzzyCompare(m_progress, value))
        return;
    m_progress = value;
    emit changed();
}

void EnvCheck::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit changed();
}

void EnvCheck::download()
{
    if (!m_canDownload || m_busy || m_done || m_aborted)
        return;
    setBusy(true);
    m_exitLabel = QStringLiteral("Cancel");
    setProgress(-1);
    emit changed();
    m_step = 0;
    nextStep();
}

void EnvCheck::accept()
{
    m_visible = false;
    emit changed();
}

void EnvCheck::quit()
{
    m_aborted = true;
    emit quitRequested();
}

void EnvCheck::nextStep()
{
    if (m_aborted)
        return;
    if (m_step == 0) {
        if (m_nodeMissing)
            fetchNodeVersion();
        else {
            m_step = 1;
            nextStep();
        }
    } else if (m_step == 1) {
#if defined(Q_OS_WIN)
        if (m_gitMissing)
            fetchMinGitUrl();
        else
            finishOk();
#else
        finishOk();
#endif
    }
}

void EnvCheck::fetchNodeVersion()
{
    setStatus(QStringLiteral("Fetching the latest Node.js LTS version..."));
    QNetworkRequest req{QUrl(QStringLiteral("https://nodejs.org/dist/index.json"))};
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (m_aborted)
            return;
        if (r->error() == QNetworkReply::NoError) {
            const auto arr = QJsonDocument::fromJson(r->readAll()).array();
            // index.json is sorted newest first and marks LTS releases with a
            // codename in "lts" (false on current releases): take the newest LTS
            if (!arr.isEmpty()) {
                for (const QJsonValue &entry : arr) {
                    const QJsonObject obj = entry.toObject();
                    if (obj.value("lts").isString()) {
                        m_nodeVersion = obj.value("version").toString();
                        break;
                    }
                }
                if (m_nodeVersion.isEmpty())
                    m_nodeVersion = arr.first().toObject().value("version").toString();
            }
        }
        setStatus(QStringLiteral("Downloading Node.js %1 (%2)...").arg(m_nodeVersion, m_arch));
        downloadToFile(QUrl(nodeDownloadUrl(m_nodeVersion, m_arch)), [this](const QString &path) {
            setStatus(QStringLiteral("Extracting Node.js..."));
            const QString dest = Util::runtimeDir() + QStringLiteral("/node-dist");
            QDir(dest).removeRecursively();
            // The official archives contain a single top-level
            // node-vX-<os>-<arch>/ directory; stripping it yields
            // <dest>/bin/node (on Windows <dest>/node.exe)
            extractArchiveAsync(path, dest, 1, [this, dest](bool ok, const QString &err) {
                if (m_aborted)
                    return;
                if (!ok) {
                    setStatus(QStringLiteral("Extraction failed: %1").arg(err));
                    m_exitLabel = QStringLiteral("Close");
                    setBusy(false);
                    emit changed();
                    return;
                }
                // Verify the extracted layout (catches unexpected archives).
                // The preprocessor condition stays outside of
                // QStringLiteral(): MSVC cannot expand a macro whose
                // argument contains preprocessor directives.
#ifdef Q_OS_WIN
                const QString nodeExe = QStringLiteral("/node.exe");
#else
                const QString nodeExe = QStringLiteral("/node");
#endif
                const QString nodeBin = Util::nodeBinDir() + nodeExe;
                if (!QFileInfo::exists(nodeBin)) {
                    QDir(dest).removeRecursively();
                    setStatus(QStringLiteral(
                        "The extracted archive does not contain a node executable "
                        "(unexpected layout).\n"
                        "You can quit and install Node.js (>= 22) manually."));
                    m_exitLabel = QStringLiteral("Close");
                    setBusy(false);
                    emit changed();
                    return;
                }
                m_step = 1;
                nextStep();
            });
        });
    });
}

#ifdef Q_OS_WIN
// Windows only: elsewhere git either ships with the OS tooling (macOS) or has to
// come from the distribution - the dialog says so and downloadToFile is not used.
void EnvCheck::fetchMinGitUrl()
{
    setStatus(QStringLiteral("Fetching the MinGit download URL..."));
    QNetworkRequest req{
        QUrl(QStringLiteral("https://api.github.com/repos/git-for-windows/git/releases/latest"))};
    req.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (m_aborted)
            return;
        QString url;
        if (r->error() == QNetworkReply::NoError) {
            const auto assets =
                QJsonDocument::fromJson(r->readAll()).object().value("assets").toArray();
            for (const auto &a : assets) {
                const QString name = a.toObject().value("name").toString();
                if (name.startsWith(QStringLiteral("MinGit-")) && name.endsWith(QStringLiteral("-64-bit.zip"))) {
                    url = a.toObject().value("browser_download_url").toString();
                    break;
                }
            }
        }
        if (url.isEmpty()) {
            setStatus(QStringLiteral(
                "Could not resolve the MinGit URL (install git on the system and restart "
                "the app later)"));
            finishOk();
            return;
        }
        setStatus(QStringLiteral("Downloading MinGit..."));
        downloadToFile(QUrl(url), [this](const QString &path) {
            setStatus(QStringLiteral("Extracting MinGit..."));
            const QString dest = Util::runtimeDir() + QStringLiteral("/mingit");
            QDir(dest).removeRecursively();
            // The MinGit zip has no top-level directory, so nothing is stripped
            extractArchiveAsync(path, dest, 0, [this](bool ok, const QString &err) {
                if (m_aborted)
                    return;
                if (!ok)
                    setStatus(QStringLiteral("Extraction failed: %1").arg(err));
                finishOk();
            });
        });
    });
}
#endif // Q_OS_WIN

void EnvCheck::downloadToFile(const QUrl &url, const std::function<void(const QString &)> &onSaved)
{
    if (!m_tmp.open()) {
        setStatus(QStringLiteral("Could not create a temporary file"));
        setBusy(false);
        return;
    }
    m_tmp.resize(0);
    QNetworkRequest req{url};
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0)
            setProgress(double(got) * 100.0 / double(total));
        else
            setProgress(-1); // indeterminate
    });
    // Stream to disk: the archives are 30-45 MB and should not be held in memory
    connect(r, &QNetworkReply::readyRead, this, [this, r] { m_tmp.write(r->readAll()); });
    connect(r, &QNetworkReply::finished, this, [this, r, onSaved] {
        r->deleteLater();
        if (m_aborted)
            return;
        if (r->error() != QNetworkReply::NoError) {
            setStatus(QStringLiteral("Download failed: %1\n"
                                     "Check your network and retry, or quit and "
                                     "install the component yourself.")
                          .arg(r->errorString()));
            m_exitLabel = QStringLiteral("Close");
            setBusy(false);
            emit changed();
            return;
        }
        m_tmp.write(r->readAll()); // drain whatever is left in the buffer
        m_tmp.flush();
        m_tmp.close();
        onSaved(m_tmp.fileName());
    });
}

void EnvCheck::extractArchiveAsync(const QString &archive, const QString &destDir, int stripComponents,
                                   const std::function<void(bool, const QString &)> &done)
{
    // The archives are 30-45 MB and extraction needs seconds to a minute:
    // blocking here would freeze the window (DESIGN section 8.1 asks for the
    // opposite). The result is posted back to qApp and the QPointer drops it
    // when the object is gone.
    setProgress(-1); // indeterminate: extraction reports no progress
    const QPointer<EnvCheck> guard(this);
    auto *worker = QThread::create([archive, destDir, stripComponents, done, guard] {
        QString err;
        const bool ok = Util::extractArchive(archive, destDir, stripComponents, &err);
        QMetaObject::invokeMethod(qApp, [guard, done, ok, err] {
            if (!guard)
                return;
            guard->setProgress(100);
            done(ok, err);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void EnvCheck::finishOk()
{
    m_done = true;
    // Re-check
    QString result;
    const QString node = Util::findCommand(QStringLiteral("node"));
    result += node.isEmpty() ? QStringLiteral("✗ Node.js still not found\n")
                             : QStringLiteral("✓ Node.js: %1\n").arg(node);
    const QString git = Util::findCommand(QStringLiteral("git"));
    result += git.isEmpty() ? QStringLiteral("✗ git still not found\n")
                            : QStringLiteral("✓ git: %1\n").arg(git);
    setStatus(QStringLiteral("Installation finished.\n%1").arg(result));
    m_progress = -1;
    setBusy(false);
    m_exitLabel = QStringLiteral("Close");
    m_downloadLabel = QStringLiteral("Continue");
    m_canDownload = true;
    emit changed();
}
