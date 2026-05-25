#pragma once

#include "PersistentDialog.h"
#include "core/ThemeManager.h"

#include <QWidget>

class QSpinBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMouseEvent;
class QPaintEvent;
class QContextMenuEvent;

namespace AetherSDR {

// Visual gradient preview + draggable stop markers, used inside
// GradientEditorDialog.  Owns no theme state — the dialog drives all
// mutations; this widget just signals user gestures and re-paints from
// the gradient it was last setGradient()'d with.
class GradientStrip : public QWidget {
    Q_OBJECT
public:
    explicit GradientStrip(QWidget* parent = nullptr);
    void                 setGradient(const ThemeGradient& g);
    const ThemeGradient& gradient() const { return m_g; }

    // Index of the stop currently rendered with a selection halo, or -1
    // when nothing is selected.  Driven by the dialog's stop list so the
    // strip + list stay in sync.
    void setSelectedStop(int index);

signals:
    // Emitted while the user drags a marker.  `newAt` is clamped to
    // [0, 1] and already snaps past adjacent stops if the drag crosses
    // them (so the dialog can reorder the stop list to match).
    void stopMoved(int index, qreal newAt);

    // Single-click on a marker — typically opens the colour picker.
    void stopActivated(int index);

    // Double-click on the strip body (away from any marker) — typically
    // adds a new stop at the click position.
    void requestNewStop(qreal at);

    // Context-menu (right-click) on a marker — typically asks the user
    // whether to delete.
    void requestDeleteStop(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    int   hitTestStop(const QPoint& p) const;
    qreal xToPosition(int x) const;
    int   positionToX(qreal at) const;
    QRect stripRect() const;     // gradient preview rectangle
    QRect markerRect(qreal at) const;

    ThemeGradient m_g;
    int           m_draggingIndex{-1};
    int           m_selectedIndex{-1};
};


// Modal dialog for editing a single linear or radial gradient token
// (waterfall.colormap.default, .grayscale, .blueGreen, .fire, .plasma
// and any other gradient leaves that flatten() finds).
//
// Workflow:
//   * Open from ThemeEditorDialog when the user clicks a gradient row.
//   * User adds / moves / recolours / deletes stops, adjusts angle (or
//     centre/radius for radial).  Live preview shows the result.
//   * OK → ThemeManager::setGradient(token, currentGradient()) which
//     emits themeChanged() and re-paints every consumer.
//   * Cancel → no-op; original on-disk value preserved.
class GradientEditorDialog : public PersistentDialog {
    Q_OBJECT
public:
    GradientEditorDialog(const QString& tokenName,
                         const ThemeGradient& initial,
                         QWidget* parent = nullptr);

    const ThemeGradient& currentGradient() const { return m_gradient; }

private slots:
    void onStopMoved(int index, qreal newAt);
    void onStopActivated(int index);
    void onRequestNewStop(qreal at);
    void onRequestDeleteStop(int index);
    void onStopListSelectionChanged();
    void onAngleSpinChanged(int deg);
    void onAddStopBtnClicked();
    void onDeleteStopBtnClicked();
    void onEditColorBtnClicked();
    void onResetToDefaultClicked();

private:
    void rebuildStopList();
    void syncStripAndList();
    void selectStop(int index);
    int  selectedStopIndex() const;
    void sortStopsByAt();      // keeps the stop list ordered after drags

    QString        m_tokenName;
    ThemeGradient  m_gradient;
    GradientStrip* m_strip{nullptr};
    QListWidget*   m_stopList{nullptr};
    QSpinBox*      m_angleSpin{nullptr};
    QLabel*        m_angleLabel{nullptr};
};

} // namespace AetherSDR
