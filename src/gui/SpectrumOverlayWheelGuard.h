#pragma once

#include <QObject>
#include <QPointer>

class QEvent;
class QScrollArea;
class QWheelEvent;
class QWidget;

namespace AetherSDR {

// Owns wheel routing at the boundary between SpectrumOverlayMenu's floating
// widgets and the SpectrumWidget beneath them. Interactive controls that
// deliberately handle wheel input keep it; everything else is either routed
// to the Display panel's scroll area or consumed before it can tune a slice.
class SpectrumOverlayWheelGuard final : public QObject {
public:
    enum class BoundaryMode {
        Consume,
        ScrollDisplay
    };

    explicit SpectrumOverlayWheelGuard(QObject* parent = nullptr);

    void setDisplayScrollArea(QScrollArea* scrollArea);
    void guardTree(QWidget* root, BoundaryMode mode);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool intentionallyOwnsWheel(QWidget* widget) const;
    bool isScrollAreaViewport(QWidget* widget) const;
    void routeToDisplayScroll(QWheelEvent* wheelEvent);

    QPointer<QScrollArea> m_displayScrollArea;
    bool m_forwardingDisplayWheel{false};
};

} // namespace AetherSDR
