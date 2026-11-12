#include "gui/WindowRepaintScheduler.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QTest>
#include <QWidget>
#include <memory>

using AetherSDR::WindowRepaintScheduler;

class UpdateWindow : public QWidget {
public:
    int value = 0;
    int renderedValue = -1;
    int deliveries = 0;
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            renderedValue = value;
            ++deliveries;
        }
        return QWidget::event(event);
    }
};

class WindowRepaintSchedulerTest : public QObject {
    Q_OBJECT
private slots:
    void burstKeepsFinalStateAndStopsWhenIdle()
    {
        UpdateWindow window;
        auto* scheduler = WindowRepaintScheduler::ensureFor(&window);
        QCOMPARE(WindowRepaintScheduler::ensureFor(&window), scheduler);
        QEvent update(QEvent::UpdateRequest);
        window.value = 1;
        QCoreApplication::sendEvent(&window, &update);
        QCOMPARE(window.renderedValue, 1); // first update after idle is immediate
        for (int value = 2; value <= 100; ++value) {
            window.value = value;
            QCoreApplication::sendEvent(&window, &update);
        }
        QVERIFY(scheduler->statsSnapshot().value("pending").toBool());
        QVERIFY(window.deliveries < 100);
        QTRY_COMPARE_WITH_TIMEOUT(window.renderedValue, 100, 250);
        const int count = window.deliveries;
        QTest::qWait(50);
        QCOMPARE(window.deliveries, count); // no idle repaint pump
        QVERIFY(!scheduler->statsSnapshot().value("pending").toBool());
    }

    void disablingDeliversPendingFinalUpdate()
    {
        UpdateWindow window;
        auto* scheduler = WindowRepaintScheduler::ensureFor(&window);
        QEvent update(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(&window, &update);
        window.value = 42;
        QCoreApplication::sendEvent(&window, &update);
        scheduler->setEnabled(false);
        QCOMPARE(window.renderedValue, 42);
        const int before = window.deliveries;
        window.value = 43;
        QCoreApplication::sendEvent(&window, &update);
        QCOMPARE(window.renderedValue, 43);
        QCOMPARE(window.deliveries, before + 1);
        QTest::qWait(50);
        QCOMPARE(window.deliveries, before + 1);
    }

    void pendingTimerDiesWithWindow()
    {
        auto window = std::make_unique<UpdateWindow>();
        QPointer<WindowRepaintScheduler> scheduler = WindowRepaintScheduler::ensureFor(window.get());
        QEvent update(QEvent::UpdateRequest);
        QCoreApplication::sendEvent(window.get(), &update);
        QCoreApplication::sendEvent(window.get(), &update);
        QVERIFY(scheduler->statsSnapshot().value("pending").toBool());
        window.reset();
        QVERIFY(scheduler.isNull());
        QTest::qWait(50);
    }

    void realSoftwareWidgetStillPaintsAfterHideAndResize()
    {
        UpdateWindow window;
        WindowRepaintScheduler::ensureFor(&window);
        window.show();
        QTRY_VERIFY(window.deliveries > 0);
        window.hide();
        window.value = 17;
        window.resize(350, 250);
        window.update();
        window.show();
        QTRY_COMPARE_WITH_TIMEOUT(window.renderedValue, 17, 500);
    }
};

QTEST_MAIN(WindowRepaintSchedulerTest)
#include "window_repaint_scheduler_test.moc"
