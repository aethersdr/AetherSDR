#pragma once

class QHBoxLayout;
class QLabel;
class QPushButton;
class QString;
class QWidget;

namespace AetherSDR {

// The panadapter's canon header: two transparent pieces laid over the top
// kHeight px of the spectrum (`host`), as children of it like its other
// overlays. The grip and slice title sit at the left, the buttons at the
// right. The empty middle between them is the spectrum's own, so clicks,
// drags and the wheel there reach it; the pieces themselves keep their mouse
// and wheel events (Qt::WA_NoMousePropagation), so a click on the title never
// tunes the spectrum behind it.
//
// PanadapterApplet owns the behavior (the buttons' signals, floating drag,
// canvas moves) and adds the canon close button to buttonRow.
struct PanadapterHeader {
    static constexpr int kHeight = 22;

    QWidget*     titleBar{nullptr};   // grip + title; accessible name panTitleBar
    QLabel*      title{nullptr};
    QWidget*     buttons{nullptr};
    QHBoxLayout* buttonRow{nullptr};
    QPushButton* popOut{nullptr};     // hidden until the owner shows it
    QPushButton* maximize{nullptr};   // hidden until the owner shows it

    static PanadapterHeader build(QWidget* host);

    // The sheet for the header's pieces and any button added to buttonRow.
    static QString styleSheet();

    // Places both pieces at the top of a host `hostWidth` wide: the title at
    // its natural width at the left, the buttons right-aligned. Returns the
    // width the visible buttons take at the top right, 0 when none shows.
    int place(int hostWidth) const;
};

} // namespace AetherSDR
