// RangeSlider accessibility announcement contract (#4565).
//
// Two behaviours have to hold at the same time, and they pull against each
// other, which is why they are pinned together here:
//
//  1. Arrow-key nudges auto-repeat at the OS key-repeat rate, so value
//     announcements debounce to ~10 Hz and a burst collapses to the settled
//     value (docs/a11y.md, "Throttle high-rate updaters").
//  2. Handle-focus moves (Tab/Backtab) are discrete, user-triggered events —
//     they must announce immediately and must NOT be swallowed by that
//     debounce when an arrow key follows inside the interval.
//
// Modelled on tests/s_meter_geometry_test.cpp::testAccessibilityAnnouncements(),
// which pins the same throttle for SMeterWidget.

#include "gui/RangeSlider.h"

#include <QAccessible>
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include <iostream>

namespace {

int g_failures = 0;

QVector<QString>* g_announcements = nullptr;

void captureAccessibleValueUpdate(QAccessibleEvent* event)
{
    if (!g_announcements || event->type() != QAccessible::ValueChanged) {
        return;
    }
    const auto* valueEvent = static_cast<const QAccessibleValueChangeEvent*>(event);
    g_announcements->push_back(valueEvent->value().toString());
}

void expect(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        ++g_failures;
    }
}

void waitForEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

QString joined(const QVector<QString>& announcements)
{
    return QStringLiteral("[%1]").arg(announcements.join(QStringLiteral(" | ")));
}

void sendKey(QWidget* widget, int key)
{
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(widget, &press);
}

// The debounce interval is 100 ms; settle past it before asserting.
constexpr int kBelowInterval = 45;
constexpr int kPastInterval  = 150;

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    QVector<QString> announcements;
    g_announcements = &announcements;
    const QAccessible::UpdateHandler previousHandler =
        QAccessible::installUpdateHandler(captureAccessibleValueUpdate);
    const bool wasAccessible = QAccessible::isActive();
    QAccessible::setActive(true);

    // Mirrors the CW decoder Pitch slider in PanadapterApplet.
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* slider = new RangeSlider(300, 1200, 500, 700, QStringLiteral("Pitch"), {}, &host);
    layout->addWidget(slider);
    auto* sibling = new QWidget(&host);
    sibling->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(sibling);
    host.show();
    host.activateWindow();
    slider->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    expect(slider->hasFocus(), QStringLiteral("announcement fixture receives focus"));

    // ── 1. A single nudge waits for the interval, then announces once ────────
    announcements.clear();
    sendKey(slider, Qt::Key_Right);
    waitForEvents(kBelowInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("a nudge waits for the throttled interval, got %1")
               .arg(joined(announcements)));
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1 && announcements.constFirst() == QStringLiteral("Low 501"),
           QStringLiteral("a settled nudge announces exactly once, got %1")
               .arg(joined(announcements)));

    // ── 2. A key-repeat burst collapses to the settled value ─────────────────
    announcements.clear();
    for (int i = 0; i < 20; ++i) {
        sendKey(slider, Qt::Key_Right);
    }
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1,
           QStringLiteral("an arrow-key burst emits exactly one update, got %1")
               .arg(joined(announcements)));
    expect(slider->low() == 521 && announcements.constLast() == QStringLiteral("Low 521"),
           QStringLiteral("the burst announces the settled value, got %1 for low=%2")
               .arg(joined(announcements))
               .arg(slider->low()));

    // ── 3. An unchanged value is not re-announced ────────────────────────────
    announcements.clear();
    slider->setLow(slider->low());
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("an unchanged value is not announced again, got %1")
               .arg(joined(announcements)));

    // ── 4. Tab announces the handle move immediately ─────────────────────────
    announcements.clear();
    sendKey(slider, Qt::Key_Tab);
    expect(announcements.size() == 1
               && announcements.constFirst() == QStringLiteral("High handle focused, 700"),
           QStringLiteral("Tab announces the handle move without waiting, got %1")
               .arg(joined(announcements)));
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1,
           QStringLiteral("the immediate handle announcement is not repeated, got %1")
               .arg(joined(announcements)));

    // ── 5. A handle move followed by a nudge inside the interval keeps BOTH ──
    // This is the regression the debounce introduced: routing the focus move
    // through the timer let the following arrow key overwrite the pending text,
    // so the user heard the new value but was never told the handle changed.
    announcements.clear();
    sendKey(slider, Qt::Key_Backtab);
    sendKey(slider, Qt::Key_Left);
    waitForEvents(kPastInterval);
    expect(announcements.size() == 2,
           QStringLiteral("a handle move plus an immediate nudge emit both updates, got %1")
               .arg(joined(announcements)));
    expect(!announcements.isEmpty()
               && announcements.constFirst() == QStringLiteral("Low handle focused, 521"),
           QStringLiteral("the handle move is announced first, got %1")
               .arg(joined(announcements)));
    expect(announcements.size() == 2
               && announcements.constLast() == QStringLiteral("Low 520"),
           QStringLiteral("the nudged value is still announced after it, got %1")
               .arg(joined(announcements)));

    // ── 6. Focus loss forgets the last published value ───────────────────────
    // Values can move unannounced while unfocused, so a value that wanders away
    // and back must not be mistaken for "unchanged" when focus returns.
    announcements.clear();
    slider->setHigh(900);
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1 && announcements.constFirst() == QStringLiteral("High 900"),
           QStringLiteral("baseline announcement before focus loss, got %1")
               .arg(joined(announcements)));

    sibling->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    expect(!slider->hasFocus(), QStringLiteral("the slider gives up focus"));

    announcements.clear();
    slider->setHigh(1000);          // unfocused — silent, as intended
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("an unfocused change stays silent, got %1")
               .arg(joined(announcements)));

    slider->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    announcements.clear();
    slider->setHigh(900);           // back to the pre-focus-loss value
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1 && announcements.constFirst() == QStringLiteral("High 900"),
           QStringLiteral("a value that wandered away and back is announced, got %1")
               .arg(joined(announcements)));

    // ── 7. With AT inactive nothing is published at all ──────────────────────
    QAccessible::setActive(false);
    announcements.clear();
    sendKey(slider, Qt::Key_Left);
    sendKey(slider, Qt::Key_Tab);
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("no announcements are published when no AT is active, got %1")
               .arg(joined(announcements)));

    QAccessible::installUpdateHandler(previousHandler);
    QAccessible::setActive(wasAccessible);
    g_announcements = nullptr;

    if (g_failures == 0) {
        std::cout << "All RangeSlider accessibility announcement checks passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
