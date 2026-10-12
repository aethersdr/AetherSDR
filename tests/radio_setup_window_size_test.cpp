// Radio Setup is 960 px wide, always; the operator sets only its height.
//
// Pinned here, each with its control:
//   1. the width is fixed at 960 (minimum == maximum) and the height runs from
//      720 up;
//   2. a resize asking for another width keeps 960 and takes the height;
//   3. a geometry saved at another width (before the width was fixed) is
//      restored at 960, with its height.
//
// No hardware and no transport: a bare RadioModel, never connected.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QtTest>

using namespace AetherSDR;

namespace {
const char* const kGeometryKey = "RadioSetupDialogGeometry";
}

class RadioSetupWindowSizeTest : public QObject {
    Q_OBJECT
private slots:

    void theWidthIsFixedAndTheHeightIsFree()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        QCOMPARE(dialog.minimumWidth(), 960);
        QCOMPARE(dialog.maximumWidth(), 960);
        QCOMPARE(dialog.minimumHeight(), 720);
        QCOMPARE(dialog.maximumHeight(), QWIDGETSIZE_MAX);
    }

    void aResizeKeepsTheWidthAndTakesTheHeight()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.resize(1300, 900);
        QTRY_COMPARE(dialog.size(), QSize(960, 900));
        dialog.resize(700, 800);
        QTRY_COMPARE(dialog.size(), QSize(960, 800));
    }

    void aWiderSavedGeometryRestoresAt960()
    {
        auto& settings = AppSettings::instance();
        // Saved by the window when its width was free: x,y,w,h.
        settings.setValue(QLatin1String(kGeometryKey), QStringLiteral("100,100,1400,850"));
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // CONTROL: the restore ran (the saved height arrived), so the width
        // below is the clamp, not a restore that never happened.
        QTRY_COMPARE(dialog.height(), 850);
        QCOMPARE(dialog.width(), 960);
        dialog.hide();
        settings.remove(QLatin1String(kGeometryKey));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-window-size"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupWindowSizeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_window_size_test.moc"
