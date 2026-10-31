// Ulanzi Dial — Control Mapping on the style guide's CanonWindow (RFC #6226):
// a fixed-size canon dialog whose pills Return never resets, whose status line
// follows the dial's state, and whose controls carry accessible names.
// Socket-free and device-free: no backend, no shortcut or MIDI manager.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/UlanziDialMappings.h"
#include "gui/CanonWindow.h"
#include "gui/FramelessResizer.h"
#include "gui/UlanziDialMapperDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTest>
#include <QToolButton>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* name)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    g_failures += !ok;
}

QList<QComboBox*> pills(QWidget& dlg)
{
    return dlg.findChildren<QComboBox*>(QStringLiteral("UlanziPill"));
}

QLabel* status(QWidget& dlg)
{
    return dlg.findChild<QLabel*>(QStringLiteral("UlanziStatus"));
}

void announce(UlanziDialMapperDialog& dlg, bool connected, const QString& name)
{
    QMetaObject::invokeMethod(&dlg, "onConnectionChanged",
                              Q_ARG(bool, connected), Q_ARG(QString, name));
}

// A canon dialog that keeps its fixed size: its pills sit at fixed points on
// the dial picture, so it opts out of FramelessResizer.
void fixedSizeCanonDialog()
{
    UlanziDialMapperDialog dlg(nullptr, nullptr, nullptr);
    check(qobject_cast<CanonWindow*>(&dlg) != nullptr, "the mapper is a CanonWindow");
    check(dlg.minimumSize() == dlg.maximumSize(), "it keeps a fixed size");
    check(dlg.findChild<FramelessResizer*>() == nullptr,
          "it installs no FramelessResizer, since its contents are pixel-placed");
    check(dlg.height() <= 700, "its fixed height fits a 768-px screen");
    // A fixed size overrides the layout's minimum, so a font that needs more
    // would clip the status row instead of growing the window.
    dlg.show();
    check(QTest::qWaitForWindowExposed(&dlg), "the window is exposed");
    check(dlg.bodyWidget()->layout()->minimumSize().height() <= dlg.bodyWidget()->height(),
          "the fixed height holds the header, canvas and status row");
}

// Return in a pill must not press Reset to Defaults: on the PersistentDialog,
// a keyboard user tabbing through the pills could rewrite every mapping.
void returnInAPillDoesNotReset()
{
    UlanziDialMapperDialog dlg(nullptr, nullptr, nullptr);
    dlg.show();
    check(QTest::qWaitForWindowExposed(&dlg), "the window is exposed");
    const QList<QComboBox*> all = pills(dlg);
    check(!all.isEmpty(), "the mapper has pills");
    if (all.isEmpty()) return;
    QComboBox* pill = all.first();
    // "(unassigned)" is first; any other index differs from at least one
    // pill's default, so pick the last item for a value Reset would undo.
    const int chosen = pill->count() - 1;
    for (QComboBox* p : all)
        p->setCurrentIndex(qMin(chosen, p->count() - 1));
    const QList<int> before = [&] {
        QList<int> v;
        for (QComboBox* p : all) v << p->currentIndex();
        return v;
    }();
    pill->setFocus();
    QTest::keyClick(pill, Qt::Key_Return);
    QTest::keyClick(&dlg, Qt::Key_Return);
    QList<int> after;
    for (QComboBox* p : all) after << p->currentIndex();
    check(after == before, "Return in a pill leaves every mapping as it was");
}

// The status line's state follows the dial: connected, disconnected, and
// turned off in Radio Setup, where a disconnect must not read as a fault.
void statusFollowsTheDial()
{
    UlanziDialMappings::setEnabled(true);
    UlanziDialMapperDialog dlg(nullptr, nullptr, nullptr);
    QLabel* line = status(dlg);
    check(line != nullptr, "the status line exists");
    if (!line) return;
    check(line->text() == QStringLiteral("Ulanzi Dial manager unavailable"),
          "with no backend the line says so");

    announce(dlg, true, QStringLiteral("Ulanzi D200 Keyboard"));
    check(line->property("ulanziState").toString() == QStringLiteral("connected")
              && line->text() == QStringLiteral("Connected — Ulanzi D200"),
          "a connected dial reads Connected, without the Keyboard suffix");

    announce(dlg, false, QString());
    check(line->property("ulanziState").toString() == QStringLiteral("disconnected"),
          "an enabled dial that drops reads Disconnected");

    UlanziDialMappings::setEnabled(false);
    announce(dlg, false, QString());
    check(line->property("ulanziState").toString() == QStringLiteral("off")
              && line->text().startsWith(QStringLiteral("Turned off")),
          "a dial turned off in Radio Setup reads off, not Disconnected");
    UlanziDialMappings::setEnabled(true);
}

// Every pill, the status line and the Last event readout carry a name.
void controlsAreNamed()
{
    UlanziDialMapperDialog dlg(nullptr, nullptr, nullptr);
    QSet<QString> names;
    bool allNamed = true;
    for (QComboBox* p : pills(dlg)) {
        allNamed = allNamed && !p->accessibleName().isEmpty();
        names.insert(p->accessibleName());
    }
    check(allNamed && names.size() == pills(dlg).size(),
          "every pill has its own accessible name");
    check(names.contains(QStringLiteral("Top Left key action"))
              && names.contains(QStringLiteral("Single tap action")),
          "a pill's name says which control it maps");
    QLabel* line = status(dlg);
    auto* last = dlg.findChild<QLabel*>(QStringLiteral("UlanziLastEvent"));
    check(line && !line->accessibleName().isEmpty(), "the status line is named");
    check(last && !last->accessibleName().isEmpty(), "the Last event readout is named");
}

// MainWindow opens the window with WA_DeleteOnClose. Close, Esc, Ctrl+W /
// Cmd+W and the corner button each destroy it, so the next open is fresh.
void everyCloseDestroysTheWindow()
{
    const char* names[] = {
        "Close destroys the window",
        "Esc destroys the window",
        "Ctrl+W / Cmd+W destroys the window",
        "the corner close button destroys the window",
    };
    for (int path = 0; path < 4; ++path) {
        QPointer<UlanziDialMapperDialog> dlg = new UlanziDialMapperDialog(nullptr, nullptr, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
        const bool exposed = QTest::qWaitForWindowExposed(dlg);
        if (path == 0) {
            for (QPushButton* b : dlg->findChildren<QPushButton*>()) {
                if (b->text() == QStringLiteral("Close")) {
                    b->click();
                    break;
                }
            }
        } else if (path == 1) {
            QTest::keyClick(dlg, Qt::Key_Escape);
        } else if (path == 2) {
            QTest::keySequence(dlg, QKeySequence::Close);
        } else {
            for (QToolButton* b : dlg->findChildren<QToolButton*>()) {
                if (b->accessibleName() == QStringLiteral("Close")) {
                    QTest::mouseClick(b, Qt::LeftButton);
                    break;
                }
            }
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        check(exposed && dlg.isNull(), names[path]);
        delete dlg.data();
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("ulanzi-dial-mapper-dialog"));
    if (!profile.isValid()) {
        return 1;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppSettings::instance().load();

    fixedSizeCanonDialog();
    returnInAPillDoesNotReset();
    statusFollowsTheDial();
    controlsAreNamed();
    everyCloseDestroysTheWindow();

    return g_failures ? 1 : 0;
}
