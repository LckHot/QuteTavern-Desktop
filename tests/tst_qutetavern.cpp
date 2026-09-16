// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Unit tests for the launcher's pure logic (no GUI, no WebEngine): the parts
// that break silently - path resolution, output parsing, validation.
//
//   cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
//
// CI runs them before the packaging jobs (see .github/workflows/build.yml).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "Updater.h"
#include "Util.h"

namespace {

bool writeText(const QString &path, const QByteArray &content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(content) == content.size();
}

} // namespace

class TestQuteTavern : public QObject {
    Q_OBJECT

private slots:
    void ansiStrip_removesColorSequences();
    void ansiStrip_keepsPlainText();

    void stDataDir_matchesThePlatformConvention();
    void stDataDir_honorsXdgDataHome();

    void readConfigPort_readsTopLevelPort();
    void readConfigPort_missingFileIsZero();
    void readConfigPort_ignoresInvalidValues();

    void validateRoot_acceptsCheckout();
    void validateRoot_rejectsNonSillyTavern();

    void pickLatestVersionTag_skipsPreReleases();
    void pickLatestVersionTag_emptyWhenOnlySpecialTags();

    void parseNetstatListeners_ignoresTheStateLanguage();
    void parseNetstatListeners_skipsNonListeningRows();

    void npmInstallArgs_followStartSh();
};

void TestQuteTavern::ansiStrip_removesColorSequences()
{
    // SillyTavern prints the readiness URL with color codes around it; the
    // "Go to:" regex in Backend must see the clean text
    const QString colored =
        QStringLiteral("Go to: \x1b[34mhttp://127.0.0.1:8000/\x1b[39m to open SillyTavern");
    QCOMPARE(Util::ansiStrip(colored),
             QStringLiteral("Go to: http://127.0.0.1:8000/ to open SillyTavern"));
}

void TestQuteTavern::ansiStrip_keepsPlainText()
{
    const QString plain = QStringLiteral("SillyTavern is listening on IPv4: 127.0.0.1:8000");
    QCOMPARE(Util::ansiStrip(plain), plain);
}

void TestQuteTavern::stDataDir_matchesThePlatformConvention()
{
    // Every platform ends in the SillyTavern component; Windows additionally
    // appends env-paths' \Data directory (builds use '/', also on Windows)
    const QString dir = Util::stDataDir();
    QVERIFY(dir.endsWith(QStringLiteral("SillyTavern"))
            || dir.endsWith(QStringLiteral("SillyTavern/Data")));
#if defined(Q_OS_WIN)
    QVERIFY(dir.endsWith(QStringLiteral("SillyTavern/Data")));
    QVERIFY(!dir.contains(QStringLiteral("Roaming")));
#endif
}

void TestQuteTavern::stDataDir_honorsXdgDataHome()
{
#if defined(Q_OS_LINUX)
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray old = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", tmp.path().toUtf8());
    QCOMPARE(Util::stDataDir(), tmp.path() + QStringLiteral("/SillyTavern"));
    if (old.isEmpty())
        qunsetenv("XDG_DATA_HOME");
    else
        qputenv("XDG_DATA_HOME", old);
#else
    QSKIP("XDG_DATA_HOME is a Linux convention; other platforms use fixed paths");
#endif
}

void TestQuteTavern::readConfigPort_readsTopLevelPort()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString cfg = tmp.path() + QStringLiteral("/config.yaml");
    QVERIFY(writeText(cfg, "dataRoot: /tmp/st\nport: 8123\nlisten: false\n"));
    QCOMPARE(Util::readConfigPort(cfg), 8123);
}

void TestQuteTavern::readConfigPort_missingFileIsZero()
{
    QCOMPARE(Util::readConfigPort(QStringLiteral("/nonexistent-dir/config.yaml")), 0);
}

void TestQuteTavern::readConfigPort_ignoresInvalidValues()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString cfg = tmp.path() + QStringLiteral("/config.yaml");
    QVERIFY(writeText(cfg, "port: not-a-number\n"));
    QCOMPARE(Util::readConfigPort(cfg), 0);
    QVERIFY(writeText(cfg, "port: 70000\n")); // out of range
    QCOMPARE(Util::readConfigPort(cfg), 0);
    QVERIFY(writeText(cfg, "port: 443\n")); // minimal valid port
    QCOMPARE(Util::readConfigPort(cfg), 443);
}

void TestQuteTavern::validateRoot_acceptsCheckout()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeText(tmp.path() + QStringLiteral("/server.js"), "// fake server\n"));
    QVERIFY(writeText(tmp.path() + QStringLiteral("/package.json"),
                      "{\"name\": \"sillytavern\", \"version\": \"1.2.3\"}"));
    const auto rc = Util::validateRoot(tmp.path());
    QVERIFY2(rc.ok, qPrintable(rc.error));
    QCOMPARE(rc.version, QStringLiteral("1.2.3"));
    QVERIFY(!rc.isGitRepo);
}

void TestQuteTavern::validateRoot_rejectsNonSillyTavern()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(writeText(tmp.path() + QStringLiteral("/server.js"), "// fake server\n"));
    QVERIFY(writeText(tmp.path() + QStringLiteral("/package.json"),
                      "{\"name\": \"something-else\"}"));
    const auto rc = Util::validateRoot(tmp.path());
    QVERIFY(!rc.ok);
    QVERIFY(!rc.error.isEmpty());
}

void TestQuteTavern::pickLatestVersionTag_skipsPreReleases()
{
    const QStringList tags{
        QStringLiteral("1.20.0-rc1"), // first by -v:refname, must be skipped
        QStringLiteral("1.19.1"),
        QStringLiteral("1.19.0"),
    };
    QCOMPARE(Updater::pickLatestVersionTag(tags), QStringLiteral("1.19.1"));
}

void TestQuteTavern::pickLatestVersionTag_emptyWhenOnlySpecialTags()
{
    // Plain numeric tags are the only accepted form ("v2.0.0" is not one)
    const QStringList tags{QStringLiteral("release"), QStringLiteral("v2.0.0")};
    QVERIFY(Updater::pickLatestVersionTag(tags).isEmpty());
}

void TestQuteTavern::parseNetstatListeners_ignoresTheStateLanguage()
{
    // German Windows netstat output: the state column reads "ABHÖREN". The
    // parser must not depend on it - a wildcard peer address means listening.
    // 18000 must not match a query for 8000, and the duplicate PID from the
    // IPv4+IPv6 rows is emitted once.
    const QString output = QString::fromUtf8(
        "\nAktive Verbindungen\n\n"
        "  Proto  Lokale Adresse         Remoteadresse          Status       PID\n"
        "  TCP    127.0.0.1:8000         0.0.0.0:0              ABHÖREN      4242\n"
        "  TCP    [::]:8000              [::]:0                 ABHÖREN      4242\n"
        "  TCP    0.0.0.0:18000          0.0.0.0:0              ABHÖREN      9999\n");
    QCOMPARE(Util::parseNetstatListeners(output, 8000), (QList<qint64>{4242}));
}

void TestQuteTavern::parseNetstatListeners_skipsNonListeningRows()
{
    const QString output = QStringLiteral(
        "  TCP    127.0.0.1:8000         127.0.0.1:58527        ESTABLISHED  1111\n"
        "  TCP    127.0.0.1:8000         0.0.0.0:0              LISTENING    7777\n"
        "  TCP    0.0.0.0:18000          0.0.0.0:0              LISTENING    9999\n");
    QCOMPARE(Util::parseNetstatListeners(output, 8000), (QList<qint64>{7777}));
}

void TestQuteTavern::npmInstallArgs_followStartSh()
{
    // The policy mirrors SillyTavern's start.sh; --no-save is the guard that
    // keeps a git checkout clean for the next update
    const QStringList args = Util::npmInstallArgs();
    QCOMPARE(args.first(), QStringLiteral("install"));
    QVERIFY(args.contains(QStringLiteral("--no-save")));
    QVERIFY(args.contains(QStringLiteral("--omit=dev")));
    QVERIFY(args.contains(QStringLiteral("--ignore-scripts")));
}

QTEST_GUILESS_MAIN(TestQuteTavern)
#include "tst_qutetavern.moc"
