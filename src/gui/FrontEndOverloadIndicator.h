#pragma once

// Front-end overload lamp and text beside the RF Gain slider (RFC #5535). Draws
// what FrontEndOverloadPresentation.h decides; its only rule is the red latch,
// which needs a clock. Starts hidden and is shown only for a backend that emits
// frontEndOverloadChanged, like the Auto checkbox beside it.

#include "core/backends/FrontEndOverload.h"
#include "gui/FrontEndOverloadPresentation.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class QLabel;

class FrontEndOverloadIndicator : public QWidget {
    Q_OBJECT
public:
    explicit FrontEndOverloadIndicator(QWidget* parent = nullptr);

    void setState(const AetherSDR::FrontEndOverload& state);
    [[nodiscard]] AetherSDR::FrontEndOverload state() const { return m_state; }

    // What the lamp is showing RIGHT NOW, which is not lampFor(level) while the
    // latch is holding. Exposed for the widget test.
    [[nodiscard]] AetherSDR::gui::LampColour shownLamp() const;

protected:
    void paintEvent(QPaintEvent* e) override;
    QSize sizeHint() const override;

private:
    void refresh();
    void announceIfWorthIt(AetherSDR::FrontEndLevel before);

    AetherSDR::FrontEndOverload m_state;
    QLabel* m_text = nullptr;

    // THE RED LATCH. A converter that rails for 200 ms and recovers is exactly
    // the event a glance misses, and it is also the event that matters most --
    // #5535 measured the clean-to-clipped transition at 3-5 dB wide, so a brief
    // excursion is the warning that the next one will not be brief. The lamp
    // therefore stays red for a beat after the level drops back.
    //
    // The TEXT is not latched, only the lamp: the operator reading the line
    // should see what is true now, while the lamp says what just happened.
    QElapsedTimer m_redSince;
    QTimer m_latchTimer;
    static constexpr int kRedLatchMs = 1200;
};
