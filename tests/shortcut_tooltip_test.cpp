// A control names its bound key in its tooltip and accessible description,
// read from ControlActionRegistry (RFC #5484). Socket-free: no radio, no
// transmitter; the settings store is a temporary profile.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/ShortcutManager.h"
#include "gui/ControlActionRegistry.h"
#include "gui/ShortcutTooltips.h"

#include <QApplication>
#include <QPushButton>
#include <QtTest>

#include <memory>

using namespace AetherSDR;

namespace {

const QString kMoxText = QStringLiteral("Toggle manual transmit on or off");
const QString kTuneText = QStringLiteral("Start or stop tune carrier");
const QString kVoxReason = QStringLiteral(
    "Unavailable: this radio has no voice-operated transmit that AetherSDR can control.");

// The wiring MainWindow::registerShortcutActions does, minus the window.
struct Rig {
    ShortcutManager shortcuts;
    ControlActionRegistry registry;
    QWidget root;
    std::unique_ptr<ShortcutTooltips> tips;

    explicit Rig(bool shortcutsEnabled)
    {
        shortcuts.registerAction("mox_toggle", "MOX Toggle", "TX", QKeySequence(Qt::Key_T), [] {});
        shortcuts.registerAction("tune_toggle", "TUNE Toggle", "TX", QKeySequence(), [] {});
        // Known to the keyboard side only: the registry has no such action.
        shortcuts.registerAction("not_in_registry", "Elsewhere", "TX", QKeySequence(Qt::Key_J),
                                 [] {});
        registry.addPair("mox_toggle", "tx.mox");
        registry.addPair("tune_toggle", "tx.tune");
        registry.setBindingLookup([this](const QString& id) {
            const ShortcutManager::Action* a = shortcuts.action(id);
            return a ? a->currentKey : QKeySequence();
        });
        tips = std::make_unique<ShortcutTooltips>(registry);
        tips->setShortcutsEnabled(shortcutsEnabled);
        QObject::connect(&shortcuts, &ShortcutManager::bindingsChanged, tips.get(),
                         &ShortcutTooltips::refresh);
    }

    QPushButton* button(const char* id, const QString& description, const QString& tip = {})
    {
        auto* b = new QPushButton(&root);
        b->setAccessibleDescription(description);
        b->setToolTip(tip);
        markShortcutAction(b, id);
        return b;
    }
};

} // namespace

class ShortcutTooltipTest : public QObject {
    Q_OBJECT

private slots:
    void boundKeyWithShortcutsOn()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        QCOMPARE(mox->toolTip(), kMoxText + QStringLiteral(" (T)"));
        QCOMPARE(mox->accessibleDescription(), kMoxText + QStringLiteral(" (T)"));
    }

    void boundKeyWithShortcutsOffIsQualifiedNotHidden()
    {
        Rig rig(false);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        const QString off = kMoxText + QStringLiteral(" (T — shortcuts disabled)");
        QCOMPARE(mox->toolTip(), off);
        QCOMPARE(mox->accessibleDescription(), off);

        rig.tips->setShortcutsEnabled(true);
        QCOMPARE(mox->toolTip(), kMoxText + QStringLiteral(" (T)"));
        rig.tips->setShortcutsEnabled(false);
        QCOMPARE(mox->toolTip(), off);
    }

    void unboundKeyChangesNothing()
    {
        Rig rig(true);
        QPushButton* tune = rig.button("tune_toggle", kTuneText, kTuneText);
        QPushButton* bare = rig.button("tune_toggle", kTuneText);
        // Bound on the keyboard side, but not a registry action.
        QPushButton* foreign = rig.button("not_in_registry", kTuneText);
        rig.tips->refresh();
        QCOMPARE(tune->toolTip(), kTuneText);
        QCOMPARE(tune->accessibleDescription(), kTuneText);
        QCOMPARE(bare->toolTip(), QString());
        QCOMPARE(bare->accessibleDescription(), kTuneText);
        QCOMPARE(foreign->toolTip(), QString());
        QCOMPARE(foreign->accessibleDescription(), kTuneText);
    }

    void liveRemapRewritesWithoutStacking()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        QPushButton* tune = rig.button("tune_toggle", kTuneText, kTuneText);
        rig.tips->refresh();

        rig.shortcuts.setBinding("mox_toggle", QKeySequence(Qt::Key_M));
        QCOMPARE(mox->toolTip(), kMoxText + QStringLiteral(" (M)"));
        QCOMPARE(mox->accessibleDescription(), kMoxText + QStringLiteral(" (M)"));

        rig.shortcuts.setBinding("tune_toggle", QKeySequence(Qt::Key_U));
        QCOMPARE(tune->toolTip(), kTuneText + QStringLiteral(" (U)"));

        // Unbinding restores the owner's own texts exactly.
        rig.shortcuts.clearBinding("mox_toggle");
        QCOMPARE(mox->toolTip(), QString());
        QCOMPARE(mox->accessibleDescription(), kMoxText);
    }

    void availabilityReasonFollowsTheKey()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();

        // What an owner does when the radio lacks the control (PhoneApplet).
        mox->setToolTip(kVoxReason);
        mox->setAccessibleDescription(kVoxReason);
        mox->setEnabled(false);
        QCoreApplication::processEvents();
        QCOMPARE(mox->toolTip(), QStringLiteral("(T) ") + kVoxReason);
        QCOMPARE(mox->accessibleDescription(), QStringLiteral("(T) ") + kVoxReason);

        mox->setToolTip(QString());
        mox->setAccessibleDescription(kMoxText);
        mox->setEnabled(true);
        QCoreApplication::processEvents();
        QCOMPARE(mox->toolTip(), kMoxText + QStringLiteral(" (T)"));
        QCOMPARE(mox->accessibleDescription(), kMoxText + QStringLiteral(" (T)"));

        // TxApplet's ATU order: enabled state first, then the two texts.
        mox->setEnabled(false);
        mox->setToolTip(kVoxReason);
        mox->setAccessibleDescription(kVoxReason);
        QCoreApplication::processEvents();
        QCOMPARE(mox->toolTip(), QStringLiteral("(T) ") + kVoxReason);
        QCOMPARE(mox->accessibleDescription(), QStringLiteral("(T) ") + kVoxReason);
    }

    void focusReReadsARewrittenDescription()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        mox->setAccessibleDescription(kTuneText);   // no event reaches the filter
        QFocusEvent in(QEvent::FocusIn, Qt::TabFocusReason);
        QCoreApplication::sendEvent(mox, &in);
        QCOMPARE(mox->accessibleDescription(), kTuneText + QStringLiteral(" (T)"));
    }

    void tooltipEventReReadsARewrittenDescription()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        mox->setAccessibleDescription(kTuneText);
        QHelpEvent help(QEvent::ToolTip, QPoint(1, 1), mox->mapToGlobal(QPoint(1, 1)));
        QCoreApplication::sendEvent(mox, &help);
        QCOMPARE(mox->toolTip(), kTuneText + QStringLiteral(" (T)"));
    }

    void aDisabledParentDoesNotMakeTheControlUnavailable()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        rig.root.setEnabled(false);
        QCoreApplication::processEvents();
        QCOMPARE(mox->accessibleDescription(), kMoxText + QStringLiteral(" (T)"));
    }

    void aControlWithNoTextsStillNamesItsKey()
    {
        Rig rig(true);
        QPushButton* atu = rig.button("mox_toggle", QString());
        rig.tips->refresh();
        QCOMPARE(atu->toolTip(), QStringLiteral("(T)"));
        QCOMPARE(atu->accessibleDescription(), QStringLiteral("(T)"));
    }

    void sidedKeyIsNamed()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        rig.shortcuts.setBinding("mox_toggle", QKeySequence(kShortcutKeyLeftCtrl));
        QCOMPARE(mox->toolTip(), kMoxText + QStringLiteral(" (Left Ctrl)"));
        QCOMPARE(mox->accessibleDescription(), kMoxText + QStringLiteral(" (Left Ctrl)"));
    }

    // PROC's capability update rewrites the description, which sends no event.
    void annotateReReadsARewrittenDescription()
    {
        Rig rig(true);
        QPushButton* proc = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        proc->setAccessibleDescription(kTuneText);
        rig.tips->annotate(&rig.root);
        QCOMPARE(proc->accessibleDescription(), kTuneText + QStringLiteral(" (T)"));
    }

    void annotateWatchesAControlBuiltAfterRefresh()
    {
        Rig rig(true);
        rig.tips->refresh();
        QPushButton* late = rig.button("mox_toggle", kMoxText);
        rig.tips->annotate(&rig.root);
        QCOMPARE(late->accessibleDescription(), kMoxText + QStringLiteral(" (T)"));
        late->setToolTip(kTuneText);
        QCoreApplication::processEvents();
        QCOMPARE(late->toolTip(), kTuneText + QStringLiteral(" (T)"));
    }

    // No owner disables a marked control without writing a reason today;
    // this pins the order for one that does.
    void disablingWithoutATextChangeMovesTheKeyFirst()
    {
        Rig rig(true);
        QPushButton* mox = rig.button("mox_toggle", kMoxText);
        rig.tips->refresh();
        mox->setEnabled(false);
        QCoreApplication::processEvents();
        QCOMPARE(mox->accessibleDescription(), QStringLiteral("(T) ") + kMoxText);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-shortcut-tooltip-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    ShortcutTooltipTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "shortcut_tooltip_test.moc"
