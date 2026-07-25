#include "gui/SpectrumOverlayWheelGuard.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
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
    testNonScrollableBoundaries();

    std::printf("\n%s\n",
                g_failed == 0
                    ? "All tests passed."
                    : "Failures present.");
    return g_failed == 0 ? 0 : 1;
}
