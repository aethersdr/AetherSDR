#pragma once

#include <QObject>
#include <Qt>

class QWidget;

namespace MasterSDR {

// Adds all-edge resize to a Qt::FramelessWindowHint top-level QWidget using
// QWindow::startSystemResize() — the same compositor-managed path used for
// startSystemMove in TitleBar / ContainerWidget / PanadapterApplet.
//
// The filter is installed on the QWindow (native handle) rather than on the
// QWidget tree.  QWindow receives all platform mouse events *before* they
// are dispatched to the widget hierarchy, so resize intent is detected across
// the whole window without touching any child widget's event stream.  This
// avoids breaking nested controls (knobs, drag tiles, sub-containers) that
// happen to be near the window edges.
//
// Timing: the native QWindow is created lazily on first show().  If install()
// is called before show() (e.g. from the constructor), the filter starts on
// the QWidget itself and migrates to the QWindow on QEvent::WinIdChange.
//
// Usage:
//   FramelessResizer::install(this);       // from a QWidget constructor
//   FramelessResizer::install(win, 6);     // explicit margin
class FramelessResizer : public QObject {
    Q_OBJECT
public:
    static void install(QWidget* window, int margin = 6);
    ~FramelessResizer() override;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    explicit FramelessResizer(QWidget* window, int margin);
    Qt::Edges edgesAt(const QPoint& windowPos) const;
    void enterEdgeZone(Qt::Edges edges);
    void leaveEdgeZone();

    QWidget*  m_window{nullptr};
    int       m_margin{6};
    bool      m_cursorOverridden{false};
    Qt::Edges m_lastEdges{};
};

} // namespace MasterSDR
