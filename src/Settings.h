// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>

// Launcher configuration (JSON); the platform specific location comes from
// QStandardPaths.
struct WindowGeometry {
    int x = 0, y = 0, w = 1280, h = 850;
};

class QJsonObject;

struct Settings {
    QString stRoot;               // SillyTavern install directory (empty = not bound)
    QStringList extraBackendArgs; // extra arguments passed to server.js
    bool autoMaximize = true;     // maximize the ST window when it opens
    bool hasWindowState = false;
    WindowGeometry windowState;

    static QString configPath();
    static Settings load(); // falls back to the legacy config path on Linux
    bool save() const;
};