// SPDX-License-Identifier: AGPL-3.0-or-later
pragma Singleton
import QtQuick

// Single place for the palette the widget version used inline. Kept as a
// singleton so the QML pages and dialogs cannot drift apart.
QtObject {
    readonly property color dim: "#888888"
    readonly property color idle: "#9a8fb0"
    readonly property color ok: "#4ade80"
    readonly property color warn: "#facc15"
    readonly property color err: "#f87171"
    readonly property color accent: "#7c6cf0"
    readonly property string mono: "monospace"
}
