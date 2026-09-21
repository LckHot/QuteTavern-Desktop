// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

QString Settings::configPath()
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return base + "/config.json";
}

Settings Settings::load()
{
    Settings s;

    auto readJson = [](const QString &path) -> QJsonObject {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(f.readAll()).object();
    };

    QJsonObject obj = readJson(configPath());

#ifdef Q_OS_LINUX
    // Legacy configuration path used by earlier launcher builds
    // (~/.config/sillytavern-launcher/config.json)
    if (obj.isEmpty()) {
        const QString legacy = QDir::homePath() + "/.config/sillytavern-launcher/config.json";
        obj = readJson(legacy);
    }
#endif

    s.stRoot = obj.value("st_root").toString();
    const auto args = obj.value("extra_backend_args").toArray();
    for (const auto &v : args)
        s.extraBackendArgs << v.toString();
    s.autoMaximize = obj.value("auto_maximize").toBool(true);
    const auto ws = obj.value("window_state").toObject();
    if (!ws.isEmpty()) {
        s.hasWindowState = true;
        s.windowState.x = ws.value("x").toInt();
        s.windowState.y = ws.value("y").toInt();
        s.windowState.w = ws.value("width").toInt(1280);
        s.windowState.h = ws.value("height").toInt(850);
    }
    return s;
}

bool Settings::save() const
{
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject ws;
    ws["x"] = windowState.x;
    ws["y"] = windowState.y;
    ws["width"] = windowState.w;
    ws["height"] = windowState.h;

    QJsonObject obj;
    obj["st_root"] = stRoot;
    QJsonArray args;
    for (const auto &a : extraBackendArgs)
        args.append(a);
    obj["extra_backend_args"] = args;
    obj["auto_maximize"] = autoMaximize;
    obj["window_state"] = hasWindowState ? ws : QJsonValue();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return true;
}