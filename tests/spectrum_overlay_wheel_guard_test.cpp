#include "gui/SpectrumOverlayWheelGuard.h"
#include "gui/GuardedSlider.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

class SpectrumProbe final : public QWidget {
public:
    using QWidget::QWidget;

    int wheelCount{0};

protected:
    void wheelEvent(QWheelEvent* event) override
    {
        ++wheelCount;
        event->accept();
    }
};

void sendWheel(QWidget* target, int angleDeltaY = -120)
{
    const QPointF localPosition = target->rect().center();
    const QPointF globalPosition =
        target->mapToGlobal(localPosition.toPoint());
    QWheelEvent event(
        localPosition,
        globalPosition,
        QPoint(),
        QPoint(0, angleDeltaY),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QApplication::sendEvent(target, &event);
}

void testDisplayRouting()
{
    SpectrumProbe spectrum;
    spectrum.resize(500, 300);

    QWidget displayPanel(&spectrum);
    displayPanel.resize(180, 180);
    auto* panelLayout = new QVBoxLayout(&displayPanel);
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    scroll.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    QWidget content;
    content.setMinimumHeight(700);
    auto* contentLayout = new QVBoxLayout(&content);
    QLabel label(QStringLiteral("Display label"), &content);
    QComboBox combo(&content);
    for (int index = 0; index < 30; ++index) {
        combo.addItem(QStringLiteral("Choice %1").arg(index));
    }
    combo.setMaxVisibleItems(5);
    combo.setStyleSheet(QStringLiteral("QComboBox { combobox-popup: 0; }"));
    combo.view()->setFixedHeight(70);
    QSlider slider(Qt::Horizontal, &content);
    slider.setRange(0, 10);
    slider.setValue(5);
    contentLayout->addWidget(&label);
    contentLayout->addWidget(&combo);
    contentLayout->addWidget(&slider);
    contentLayout->addStretch();
    scroll.setWidget(&content);
    panelLayout->addWidget(&scroll);

    SpectrumOverlayWheelGuard guard;
    guard.setDisplayScrollArea(&scroll);
    guard.guardTree(
        &displayPanel,
        SpectrumOverlayWheelGuard::BoundaryMode::ScrollDisplay);

    spectrum.show();
    displayPanel.show();
    QApplication::processEvents();

    scroll.verticalScrollBar()->setValue(0);
    sendWheel(&label);
    report("Display label wheel scrolls the vertical panel",
           scroll.verticalScrollBar()->value() > 0);
    report("Display label wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);

    scroll.verticalScrollBar()->setValue(0);
    combo.setCurrentIndex(1);
    sendWheel(&combo);
    report("closed Display combo does not change selection",
           combo.currentIndex() == 1);
    report("closed Display combo wheel scrolls the vertical panel",
           scroll.verticalScrollBar()->value() > 0);
    report("closed Display combo wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);

    scroll.verticalScrollBar()->setValue(0);
    combo.showPopup();
    QApplication::processEvents();
    QAbstractItemView* comboView = combo.view();
    comboView->verticalScrollBar()->setValue(0);
    sendWheel(comboView->viewport());
    report("open Display combo scrolls its popup list",
           comboView->verticalScrollBar()->value() > 0);
    report("open Display combo does not scroll the panel behind it",
           scroll.verticalScrollBar()->value() == 0);
    report("open Display combo wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);
    combo.hidePopup();

    const int sliderBefore = slider.value();
    sendWheel(&slider, 120);
    report("Display slider retains deliberate wheel behavior",
           slider.value() != sliderBefore);
    report("Display slider wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);

    scroll.verticalScrollBar()->setValue(
        scroll.verticalScrollBar()->maximum());
    sendWheel(&label);
    report("Display wheel at scroll limit is still contained",
           spectrum.wheelCount == 0);
}

void testDisplayPanelResizeClamp()
{
    const QSize contentHint(306, 594);
    constexpr int scrollBarExtent = 14;

    const QSize initialSize = constrainedDisplayPanelSize(
        contentHint, 700, scrollBarExtent);
    report("Display panel uses its content height when the host is tall",
           initialSize == QSize(308, 596));

    const QSize resized = constrainedDisplayPanelSize(
        contentHint, 505, scrollBarExtent);
    report("Display panel re-clamps after its host shrinks",
           resized == QSize(322, 505));
    report("re-clamped Display panel leaves scrollable overflow",
           contentHint.height() > resized.height() - 2);
}

// Mirrors SpectrumOverlayMenu::layoutDisplayPanel()'s composition of the two
// pure helpers, so the geometry the widget actually receives is under test —
// not just the size half of it.
QRect displayPanelGeometry(const QSize& contentHint, int hostHeight,
                           int scrollBarExtent, int menuBottom, int menuRight)
{
    const QSize panelSize = constrainedDisplayPanelSize(
        contentHint, hostHeight, scrollBarExtent);
    const int panelTop = constrainedDisplayPanelTop(
        menuBottom, panelSize.height(), hostHeight);
    return QRect(QPoint(menuRight, panelTop), panelSize);
}

void testDisplayPanelStaysInsideHost()
{
    const QSize contentHint(306, 594);
    constexpr int scrollBarExtent = 14;
    constexpr int menuRight = 40;
    // The regression: a panadapter shorter than the overlay menu. Clamping the
    // HEIGHT to the host is not enough — bottom-anchoring to the menu then
    // pushes the pane's tail below the host, where the parent clips it and no
    // scroll offset can reach it (#3969 follow-up).
    constexpr int shortHost = 150;
    constexpr int menuBottom = 194;

    const QRect clipped = displayPanelGeometry(
        contentHint, shortHost, scrollBarExtent, menuBottom, menuRight);
    report("Display pane bottom stays inside a short host",
           clipped.bottom() < shortHost);
    report("Display pane top stays inside a short host",
           clipped.top() >= 0);
    report("short host still yields a full-height scrollable pane",
           clipped.height() == shortHost);

    // A pane short enough to sit entirely above menuBottom keeps its natural
    // bottom-anchored placement — the clamp must not disturb the common case.
    constexpr int tallHost = 700;
    const QSize shortContent(306, 100);
    const QRect anchored = displayPanelGeometry(
        shortContent, tallHost, scrollBarExtent, menuBottom, menuRight);
    report("Display pane stays bottom-anchored to the menu when it fits",
           anchored.top() + anchored.height() == menuBottom);
    report("Display pane bottom stays inside a tall host",
           anchored.bottom() < tallHost);

    // A pane taller than the menu's bottom edge is flush to the top rather
    // than pushed off-screen (the pre-existing max(0, ...) behaviour).
    const QRect topFlush = displayPanelGeometry(
        contentHint, tallHost, scrollBarExtent, menuBottom, menuRight);
    report("over-tall Display pane sits flush with the host top",
           topFlush.top() == 0);

    // Sweep: containment must hold for every host height, not just the two
    // hand-picked cases above.
    bool containedEverywhere = true;
    for (int hostHeight = 20; hostHeight <= 900; ++hostHeight) {
        const QRect geometry = displayPanelGeometry(
            contentHint, hostHeight, scrollBarExtent, menuBottom, menuRight);
        if (geometry.top() < 0 || geometry.bottom() >= hostHeight) {
            containedEverywhere = false;
            break;
        }
    }
    report("Display pane is contained for every host height", containedEverywhere);
}

// Finding 3: sliders span most of each Display row, so wheeling to scroll would
// otherwise change Averaging/FPS/Opacity. The project's existing global controls
// lock (#745) is the opt-in that frees the wheel for scrolling; verify it
// composes with the guard — the slider's ignored wheel must reach the guarded
// content widget and be routed to the scroll area, never to the spectrum.
void testControlsLockFreesDisplayScrolling()
{
    SpectrumProbe spectrum;
    spectrum.resize(500, 300);

    QWidget displayPanel(&spectrum);
    displayPanel.resize(180, 180);
    auto* panelLayout = new QVBoxLayout(&displayPanel);
    QScrollArea scroll;
    scroll.setWidgetResizable(true);
    scroll.setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    QWidget content;
    content.setMinimumHeight(700);
    auto* contentLayout = new QVBoxLayout(&content);
    auto* slider = new GuardedSlider(Qt::Horizontal, &content);
    slider->setRange(0, 100);
    slider->setValue(50);
    contentLayout->addWidget(slider);
    contentLayout->addStretch();
    scroll.setWidget(&content);
    panelLayout->addWidget(&scroll);

    SpectrumOverlayWheelGuard guard;
    guard.setDisplayScrollArea(&scroll);
    guard.guardTree(
        &displayPanel,
        SpectrumOverlayWheelGuard::BoundaryMode::ScrollDisplay);

    spectrum.show();
    displayPanel.show();
    QApplication::processEvents();

    // Unlocked: the slider deliberately owns the wheel (#570/#1026).
    ControlsLock::setLocked(false);
    scroll.verticalScrollBar()->setValue(0);
    const int valueBefore = slider->value();
    sendWheel(slider, 120);
    report("unlocked Display slider still adjusts on wheel",
           slider->value() != valueBefore);
    report("unlocked Display slider does not scroll the pane",
           scroll.verticalScrollBar()->value() == 0);
    report("unlocked Display slider wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);

    // Locked: the wheel scrolls the pane instead, over the sliders too.
    ControlsLock::setLocked(true);
    scroll.verticalScrollBar()->setValue(0);
    const int lockedValueBefore = slider->value();
    sendWheel(slider);
    report("locked Display slider does not change value",
           slider->value() == lockedValueBefore);
    report("locked Display slider wheel scrolls the pane",
           scroll.verticalScrollBar()->value() > 0);
    report("locked Display slider wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);
    ControlsLock::setLocked(false);
}

void testNonScrollableBoundaries()
{
    SpectrumProbe spectrum;
    spectrum.resize(500, 300);

    QWidget panel(&spectrum);
    auto* layout = new QVBoxLayout(&panel);
    QLabel label(QStringLiteral("ordinary panel content"), &panel);
    QComboBox combo(&panel);
    combo.addItems({QStringLiteral("One"), QStringLiteral("Two")});
    layout->addWidget(&label);
    layout->addWidget(&combo);

    QWidget mainStrip(&spectrum);
    QPushButton stripButton(QStringLiteral("Display"), &mainStrip);

    QWidget memoryPanel(&spectrum);
    auto* memoryLayout = new QVBoxLayout(&memoryPanel);
    QTableWidget memoryTable(30, 1, &memoryPanel);
    memoryLayout->addWidget(&memoryTable);

    SpectrumOverlayWheelGuard guard;
    guard.guardTree(
        &panel, SpectrumOverlayWheelGuard::BoundaryMode::Consume);
    guard.guardTree(
        &mainStrip, SpectrumOverlayWheelGuard::BoundaryMode::Consume);
    guard.guardTree(
        &memoryPanel, SpectrumOverlayWheelGuard::BoundaryMode::Consume);

    spectrum.show();
    panel.show();
    mainStrip.show();
    memoryPanel.show();
    QApplication::processEvents();

    sendWheel(&label);
    report("non-scrollable panel content consumes wheel input",
           spectrum.wheelCount == 0);

    combo.setCurrentIndex(0);
    sendWheel(&combo);
    report("closed combo in non-scrollable panel stays unchanged",
           combo.currentIndex() == 0);
    report("closed combo in non-scrollable panel cannot tune",
           spectrum.wheelCount == 0);

    sendWheel(&stripButton);
    report("main overlay strip child cannot leak wheel input",
           spectrum.wheelCount == 0);

    memoryTable.verticalScrollBar()->setValue(0);
    sendWheel(memoryTable.viewport());
    report("Memory table retains deliberate list scrolling",
           memoryTable.verticalScrollBar()->value() > 0);
    report("Memory table wheel does not reach SpectrumWidget",
           spectrum.wheelCount == 0);
}

} // namespace

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    std::printf("Spectrum overlay wheel ownership test harness\n\n");
    testDisplayRouting();
    testDisplayPanelResizeClamp();
    testDisplayPanelStaysInsideHost();
    testControlsLockFreesDisplayScrolling();
    testNonScrollableBoundaries();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : "Failures present.");
    return g_failed == 0 ? 0 : 1;
}
