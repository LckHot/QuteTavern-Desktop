// SPDX-License-Identifier: AGPL-3.0-or-later
#include "EnvCheck.h"

#include "Util.h"

#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QVBoxLayout>

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

class EnvDialog : public QDialog {
    Q_OBJECT

public:
    explicit EnvDialog(bool nodeMissing, bool gitMissing, QWidget *parent)
        : QDialog(parent)
        , m_nodeMissing(nodeMissing)
        , m_gitMissing(gitMissing)
    {
        setWindowTitle(QStringLiteral("Environment check"));
        setModal(true);
        resize(560, 300);

        auto *lay = new QVBoxLayout(this);
        lay->setSpacing(10);

        m_arch = nodeArch();
        const QString cpu = QSysInfo::currentCpuArchitecture();

        QString body = QStringLiteral(
            "SillyTavern needs the following components; these are missing:\n\n");
        body += QStringLiteral("CPU architecture: %1%2\n\n")
                    .arg(cpu,
                         m_arch.isEmpty()
                             ? QStringLiteral(" (no official Node.js build for it)")
                             : QStringLiteral(" (will download the %1 build)").arg(m_arch));
        if (nodeMissing)
            body += QStringLiteral(
                "  ✗ Node.js - will download the official portable build (~30 MB) "
                "into the application directory\n");
        else
            body += QStringLiteral("  ✓ Node.js\n");
#if defined(Q_OS_WIN)
        if (gitMissing)
            body += QStringLiteral(
                "  ✗ git - will download Git for Windows (MinGit, ~45 MB) "
                "into the application directory\n");
        else
            body += QStringLiteral("  ✓ git\n");
#elif defined(Q_OS_MAC)
        if (gitMissing)
            body += QStringLiteral(
                "  ✗ git - on macOS git ships with the Xcode command line tools, "
                "run: xcode-select --install\n");
        else
            body += QStringLiteral("  ✓ git\n");
#else
        if (gitMissing)
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

        auto *text = new QLabel(body, this);
        text->setWordWrap(true);
        m_status = new QLabel(this);
        m_status->setWordWrap(true);
        m_bar = new QProgressBar(this);
        m_bar->hide();

        m_exitBtn = new QPushButton(QStringLiteral("Quit"), this);
        m_dlBtn = new QPushButton(
            nodeMissing ? QStringLiteral("Download and install into the application directory")
                        : QStringLiteral("Download git into the application directory"),
            this);
        m_dlBtn->setDefault(true);
        // Unsupported CPU architecture: never guess an instruction set - disable
        // the download and point at a manual installation instead
        if (m_arch.isEmpty()) {
            m_dlBtn->setEnabled(false);
            m_status->setText(QStringLiteral(
                "There is no official Node.js binary for this CPU architecture (%1).\n"
                "Please quit and install Node.js (>= 22) and git manually, "
                "then start the app again.")
                .arg(cpu));
        }
        auto *bbox = new QHBoxLayout();
        bbox->addWidget(m_exitBtn);
        bbox->addStretch(1);
        bbox->addWidget(m_dlBtn);

        lay->addWidget(text, 1);
        lay->addWidget(m_status);
        lay->addWidget(m_bar);
        lay->addLayout(bbox);

        connect(m_exitBtn, &QPushButton::clicked, this, [this] {
            m_aborted = true;
            reject();
        });
        connect(m_dlBtn, &QPushButton::clicked, this, [this] { startDownloads(); });
    }

    bool succeeded() const { return m_done && !m_aborted; }

private slots:
    void startDownloads()
    {
        if (m_arch.isEmpty() || m_aborted)
            return;
        m_dlBtn->setEnabled(false);
        m_exitBtn->setText(QStringLiteral("Cancel"));
        m_bar->show();
        m_step = 0;
        nextStep();
    }

private:
    void nextStep();
    void fetchNodeVersion();
    void fetchMinGitUrl();
    void downloadToFile(const QUrl &url, const std::function<void(const QString &)> &onSaved);
    void finishOk();

    bool m_nodeMissing, m_gitMissing;
    int m_step = 0;          // 0 = node, 1 = git
    bool m_done = false;
    bool m_aborted = false;
    QString m_arch;          // Node download identifier (x64/arm64); empty = unsupported CPU
    QString m_nodeVersion = QString::fromLatin1(kFallbackNodeVersion);
    QLabel *m_status = nullptr;
    QProgressBar *m_bar = nullptr;
    QPushButton *m_exitBtn = nullptr;
    QPushButton *m_dlBtn = nullptr;
    QNetworkAccessManager m_nam;
    QTemporaryFile m_tmp;
};

void EnvDialog::nextStep()
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

void EnvDialog::fetchNodeVersion()
{
    m_status->setText(QStringLiteral("Fetching the latest Node.js LTS version..."));
    QNetworkRequest req{QUrl(QStringLiteral("https://nodejs.org/dist/index.json"))};
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        if (m_aborted)
            return;
        if (r->error() == QNetworkReply::NoError) {
            const auto arr = QJsonDocument::fromJson(r->readAll()).array();
            // index.json is sorted newest first, so the first entry is the
            // latest release (Node 20 and older are end of life)
            if (!arr.isEmpty()) {
                const QString latest = arr.first().toObject().value("version").toString();
                if (!latest.isEmpty())
                    m_nodeVersion = latest;
            }
        }
        m_status->setText(QStringLiteral("Downloading Node.js %1 (%2)...")
                              .arg(m_nodeVersion, m_arch));
        downloadToFile(QUrl(nodeDownloadUrl(m_nodeVersion, m_arch)),
                       [this](const QString &path) {
                           m_status->setText(QStringLiteral("Extracting Node.js..."));
                           const QString dest = Util::runtimeDir() + "/node-dist";
                           QDir(dest).removeRecursively();
                           QString err;
                           // The official archives contain a single top-level
                           // node-vX-<os>-<arch>/ directory; stripping it yields
                           // <dest>/bin/node (on Windows <dest>/node.exe)
                           if (!Util::extractArchive(path, dest, 1, &err)) {
                               m_status->setText(QStringLiteral("Extraction failed: %1").arg(err));
                               m_exitBtn->setText(QStringLiteral("Close"));
                               m_dlBtn->setEnabled(true);
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
                               m_status->setText(QStringLiteral(
                                   "The extracted archive does not contain a node executable "
                                   "(unexpected layout).\n"
                                   "You can quit and install Node.js (>= 22) manually."));
                               m_exitBtn->setText(QStringLiteral("Close"));
                               m_dlBtn->setEnabled(true);
                               return;
                           }
                           m_step = 1;
                           nextStep();
                       });
    });
}

void EnvDialog::fetchMinGitUrl()
{
    m_status->setText(QStringLiteral("Fetching the MinGit download URL..."));
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
                if (name.startsWith("MinGit-") && name.endsWith("-64-bit.zip")) {
                    url = a.toObject().value("browser_download_url").toString();
                    break;
                }
            }
        }
        if (url.isEmpty()) {
            m_status->setText(QStringLiteral(
                "Could not resolve the MinGit URL (install git on the system and restart "
                "the app later)"));
            finishOk();
            return;
        }
        m_status->setText(QStringLiteral("Downloading MinGit..."));
        downloadToFile(QUrl(url), [this](const QString &path) {
            m_status->setText(QStringLiteral("Extracting MinGit..."));
            const QString dest = Util::runtimeDir() + "/mingit";
            QDir(dest).removeRecursively();
            QString err;
            // The MinGit zip has no top-level directory, so nothing is stripped
            if (!Util::extractArchive(path, dest, 0, &err)) {
                m_status->setText(QStringLiteral("Extraction failed: %1").arg(err));
            }
            finishOk();
        });
    });
}

void EnvDialog::downloadToFile(const QUrl &url,
                               const std::function<void(const QString &)> &onSaved)
{
    if (!m_tmp.open()) {
        m_status->setText(QStringLiteral("Could not create a temporary file"));
        return;
    }
    m_tmp.resize(0);
    QNetworkRequest req{url};
    QNetworkReply *r = m_nam.get(req);
    connect(r, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) {
            m_bar->setRange(0, 100);
            m_bar->setValue(int(got * 100 / total));
        } else {
            m_bar->setRange(0, 0); // indeterminate
        }
    });
    // Stream to disk: the archives are 30-45 MB and should not be held in memory
    connect(r, &QNetworkReply::readyRead, this, [this, r] { m_tmp.write(r->readAll()); });
    connect(r, &QNetworkReply::finished, this, [this, r, onSaved] {
        r->deleteLater();
        if (m_aborted)
            return;
        if (r->error() != QNetworkReply::NoError) {
            m_status->setText(QStringLiteral("Download failed: %1\n"
                                             "Check your network and retry, or quit and "
                                             "install the component yourself.")
                                  .arg(r->errorString()));
            m_exitBtn->setText(QStringLiteral("Close"));
            m_dlBtn->setEnabled(true);
            return;
        }
        m_tmp.write(r->readAll()); // drain whatever is left in the buffer
        m_tmp.flush();
        m_tmp.close();
        onSaved(m_tmp.fileName());
    });
}

void EnvDialog::finishOk()
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
    m_status->setText(QStringLiteral("Installation finished.\n%1").arg(result));
    m_bar->hide();
    m_exitBtn->setText(QStringLiteral("Close"));
    m_dlBtn->setText(QStringLiteral("Continue"));
    disconnect(m_dlBtn, &QPushButton::clicked, nullptr, nullptr);
    connect(m_dlBtn, &QPushButton::clicked, this, [this] {
        m_aborted = false;
        accept();
    });
    m_dlBtn->setEnabled(true);
    m_dlBtn->setDefault(true);
}

} // namespace

namespace EnvCheck {

bool ensureEnvironment(QWidget *parent)
{
    const bool nodeMissing = Util::findCommand(QStringLiteral("node")).isEmpty();
    const bool gitMissing = Util::findCommand(QStringLiteral("git")).isEmpty();
    if (!nodeMissing && !gitMissing)
        return true;

    EnvDialog dlg(nodeMissing, gitMissing, parent);
    dlg.exec();
    // "Continue" = accept; "Quit" - or simply closing the window (Qt rejects) -
    // terminates the application. When the downloads already finished
    // (succeeded), closing the window still lets the app start.
    return dlg.succeeded() || dlg.result() == QDialog::Accepted;
}

} // namespace EnvCheck

#include "EnvCheck.moc"