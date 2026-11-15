#pragma once

#include "core/ClientEq.h"
#include <QWidget>

class QHBoxLayout;

namespace AetherSDR {

class AudioEngine;

// Top-of-editor row: one small icon button per active band showing the
// filter-type curve shape (HP slope, low shelf, bell, high shelf, LP slope)
// in the band's palette color. Click cycles type forward; shift-click
// cycles backward. Right-click exposes the explicit type menu (handled
// by the canvas's existing context menu — icons route through the same
// AudioEngine mutation path).
//
// The row reflows when activeBandCount() changes; selected band gets a
// bright ring. Inactive trailing slots are hidden (not shown greyed).
class ClientEqIconRow : public QWidget {
    Q_OBJECT

public:
    explicit ClientEqIconRow(QWidget* parent = nullptr);

    void setEq(ClientEq* eq);
    void setAudioEngine(AudioEngine* engine);  // for persistence on edit

signals:
    void bandSelected(int idx);

public slots:
    // Called when the ClientEq instance changes on the audio side (add /
    // delete band, type change via context menu, path switch). Rebuilds
    // the icon row to match the current state.
    void refresh();
    void setSelectedBand(int idx);

private:
    class IconButton;  // defined in the .cpp

    void rebuild();

    ClientEq*    m_eq{nullptr};
    AudioEngine* m_audio{nullptr};
    QHBoxLayout* m_layout{nullptr};
    int          m_selectedBand{-1};
};

} // namespace AetherSDR
