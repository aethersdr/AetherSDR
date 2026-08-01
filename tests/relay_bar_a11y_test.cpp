// RelayBar accessibility announcement contract (#4565).
//
// RelayBar::setValue() is driven by TunerModel::stateChanged, which relays
// TGXL hardware state pushes with no rate limiting on the wire — an ATU
// auto-tune sweep can step the relay position many times in a burst. A
// focused bar therefore debounces its announcements to ~10 Hz and publishes
// the settled position (docs/a11y.md, "Throttle high-rate updaters").
//
// Because those pushes arrive regardless of focus, the last-published value
// is forgotten on focus loss: a position that moved while unfocused and came
// back must not be mistaken for "unchanged" and go unannounced.
//
// Modelled on tests/s_meter_geometry_test.cpp::testAccessibilityAnnouncements().

#include "gui/HGauge.h"

#include <QAccessible>
#include <QApplication>
#include <QEventLoop>
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

    // Mirrors the C1 relay indicator in TunerApplet.
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    auto* bar = new AetherSDR::RelayBar(QStringLiteral("C1"), &host);
    layout->addWidget(bar);
    auto* sibling = new QWidget(&host);
    sibling->setFocusPolicy(Qt::StrongFocus);
    layout->addWidget(sibling);
    host.show();
    host.activateWindow();
    bar->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    expect(bar->hasFocus(), QStringLiteral("announcement fixture receives focus"));

    // ── 1. A relay step waits for the interval, then announces once ──────────
    announcements.clear();
    bar->setValue(120);
    waitForEvents(kBelowInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("a relay step waits for the throttled interval, got %1")
               .arg(joined(announcements)));
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1 && announcements.constFirst() == QStringLiteral("120"),
           QStringLiteral("a settled relay step announces exactly once, got %1")
               .arg(joined(announcements)));

    // ── 2. A sweep collapses to the settled position ─────────────────────────
    announcements.clear();
    for (int v = 121; v <= 160; ++v) {
        bar->setValue(v);
    }
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1,
           QStringLiteral("an ATU sweep emits exactly one update, got %1")
               .arg(joined(announcements)));
    expect(!announcements.isEmpty()
               && announcements.constLast() == QStringLiteral("160"),
           QStringLiteral("the sweep announces the settled position, got %1")
               .arg(joined(announcements)));

    // ── 3. An unchanged position is not re-announced ─────────────────────────
    announcements.clear();
    bar->setValue(160);
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("an unchanged position is not announced again, got %1")
               .arg(joined(announcements)));

    // ── 4. Focus loss forgets the last published position ────────────────────
    // Hardware pushes keep arriving while the bar is unfocused, so a position
    // that wandered away and back must still be announced when focus returns.
    sibling->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    expect(!bar->hasFocus(), QStringLiteral("the relay bar gives up focus"));

    announcements.clear();
    bar->setValue(200);             // unfocused — silent, as intended
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("an unfocused relay step stays silent, got %1")
               .arg(joined(announcements)));

    bar->setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    announcements.clear();
    bar->setValue(160);             // back to the pre-focus-loss position
    waitForEvents(kPastInterval);
    expect(announcements.size() == 1 && announcements.constFirst() == QStringLiteral("160"),
           QStringLiteral("a position that wandered away and back is announced, got %1")
               .arg(joined(announcements)));

    // ── 5. With AT inactive nothing is published at all ──────────────────────
    QAccessible::setActive(false);
    announcements.clear();
    bar->setValue(10);
    waitForEvents(kPastInterval);
    expect(announcements.isEmpty(),
           QStringLiteral("no announcements are published when no AT is active, got %1")
               .arg(joined(announcements)));

    QAccessible::installUpdateHandler(previousHandler);
    QAccessible::setActive(wasAccessible);
    g_announcements = nullptr;

    if (g_failures == 0) {
        std::cout << "All RelayBar accessibility announcement checks passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
