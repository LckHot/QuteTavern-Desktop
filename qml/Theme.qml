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

    // Dialog geometry. One design width for every dialog, and the margin that
    // keeps the footer buttons clear of the dialog's edges - a Dialog does not
    // inset a custom footer itself (its padding only affects the content item).
    readonly property int dialogWidth: 560
    readonly property int dialogMargin: 12
}
