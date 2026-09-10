#pragma once

#include "core/SpeProtocol.h"

#include <QImage>
#include <QWidget>

namespace AetherSDR {

// Live mirror of the SPE Expert's front-panel LCD (8 rows x 40 columns of
// 6x8 glyph cells), rendered from the amplifier's own font ROM so it shows
// exactly what the operator would see standing at the amp — menus,
// tuning-step screens, frequency readout and all. Fed by
// SpeConnection::lcdFrameReceived; shown only in SpeApplet's floating
// presentation (the docked rail has no room for it).
//
// Dedicated semantic tokens preserve the hardware's green-on-dark palette
// while keeping custom themes and live theme changes authoritative.
class SpeLcdWidget : public QWidget {
    Q_OBJECT

public:
    explicit SpeLcdWidget(QWidget* parent = nullptr);

    void setFrame(const Spe::Lcd::Frame& frame);
    // Stale = refreshes stopped arriving but the connection is still up:
    // the last image stays visible, dimmed. A dropped display frame or two
    // on a lossy proxy link must read as a hiccup, not blank the mirror to
    // the idle glass and back (the keys' freshness gate is the applet's
    // concern, not this widget's).
    void setStale(bool stale);
    // Back to the idle glass (dim "no display data" hint) — used when the
    // connection drops or a floating presentation opens with no current
    // frame; a stale-but-connected mirror keeps its image via setStale.
    void clear();

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void renderFrame();  // rebuilds m_image from m_frame at 1x

    Spe::Lcd::Frame m_frame;
    bool m_hasFrame{false};
    bool m_stale{false};
    QImage m_image;  // native-resolution render, integer-scaled at paint
};

}  // namespace AetherSDR
