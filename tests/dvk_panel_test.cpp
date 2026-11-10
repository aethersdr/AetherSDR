// DVK panel driving a VoiceKeyer — the panel's contract, with no radio behind it.
// Run: ./build/dvk_panel_test

#include "TestSettingsProfile.h"
#include "core/TxKeyingMarker.h"
#include "gui/DvkPanel.h"
#include "models/VoiceKeyer.h"

#include <QApplication>
#include <QAction>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStringList>
#include <QPointer>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QMenu>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <string>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-52s %s\n", ok ? "[ OK ]" : "[FAIL]", name, detail.c_str());
    if (!ok) ++g_failed;
}

// Records what the panel asked for; state changes are pushed by the test.
class FakeKeyer : public VoiceKeyer {
public:
    QStringList calls;
    Status st{Idle};
    int active{-1};
    QVector<VoiceKeyerRecording> recs;
    QString label{QStringLiteral("Radio")};

    Status status() const override { return st; }
    int activeId() const override { return active; }
    const QVector<VoiceKeyerRecording>& recordings() const override { return recs; }

    void recStart(int id) override { calls << QString("recStart %1").arg(id); }
    void recStop(int id) override { calls << QString("recStop %1").arg(id); }
    void previewStart(int id) override { calls << QString("previewStart %1").arg(id); }
    void previewStop(int id) override { calls << QString("previewStop %1").arg(id); }
    void playbackStart(int id) override { calls << QString("playbackStart %1").arg(id); }
    void playbackStop(int id) override { calls << QString("playbackStop %1").arg(id); }
    void clear(int id) override { calls << QString("clear %1").arg(id); }
    void remove(int id) override { calls << QString("remove %1").arg(id); }
    void setName(int id, const QString& n) override { calls << QString("setName %1 %2").arg(id).arg(n); }
    void importWav(int id, const QString& p) override { calls << QString("importWav %1 %2").arg(id).arg(p); }
    void exportWav(int id, const QString& p) override { calls << QString("exportWav %1 %2").arg(id).arg(p); }
    bool canTransferWav() const override { return true; }
    bool isTransferring() const override { return false; }
    QString sourceLabel() const override { return label; }
    // The panel reads its recording cap and name limit from the keyer, so a
    // fake states its own; 10 s matches the radio DVK's.
    int maxMs{10'000};
    int maxRecordingMs() const override { return maxMs; }
    int maxNameBytes() const override { return 61; }

    void addRecording(int id, const QString& name, int ms)
    {
        recs.append({id, name, ms});
        emit recordingChanged(id);
    }
    void pushStatus(Status s, int id)
    {
        st = s;
        active = id;
        emit statusChanged(s, id);
    }
};

QPushButton* buttonByText(DvkPanel& panel, const QString& text)
{
    for (auto* b : panel.findChildren<QPushButton*>())
        if (b->text() == text) return b;
    return nullptr;
}

QLabel* statusLabel(DvkPanel& panel)
{
    return panel.findChild<QLabel*>(QStringLiteral("dvkStatus"));
}

QLabel* labelWithText(DvkPanel& panel, const QString& text)
{
    for (auto* l : panel.findChildren<QLabel*>())
        if (l->text() == text) return l;
    return nullptr;
}

QShortcut* shortcutFor(DvkPanel& panel, Qt::Key key)
{
    for (auto* sc : panel.findChildren<QShortcut*>())
        if (sc->key() == QKeySequence(key)) return sc;
    return nullptr;
}

std::string joined(const QStringList& calls)
{
    return calls.join(QStringLiteral(" | ")).toStdString();
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("dvk_panel_test"));
    QApplication app(argc, argv);

    FakeKeyer keyer;
    DvkPanel panel(&keyer);
    QLabel* status = statusLabel(panel);
    QPushButton* f1 = buttonByText(panel, QStringLiteral("F1"));
    QPushButton* f2 = buttonByText(panel, QStringLiteral("F2"));
    QPushButton* f5 = buttonByText(panel, QStringLiteral("F5"));
    QPushButton* rec = buttonByText(panel, QString::fromUtf8("● REC"));
    report("panel_widgets_found", status && f1 && f2 && f5 && rec);
    if (!(status && f1 && f2 && f5 && rec)) {
        std::printf("\nFAILED (cannot continue)\n");
        return 1;
    }

    // Controls that key the transmitter carry the automation bridge's TX
    // marker, so an agent can never press them against a live radio (#3646);
    // record, stop and local PLAY never key and stay drivable.
    {
        bool keyingMarked = true;
        for (int id = 1; id <= 12; ++id) {
            QPushButton* b = buttonByText(panel, QStringLiteral("F%1").arg(id));
            keyingMarked = keyingMarked && b && b->property(kTxKeyingProperty).toBool();
        }
        QPushButton* stop = buttonByText(panel, QString::fromUtf8("■ STOP"));
        QPushButton* play = buttonByText(panel, QString::fromUtf8("▶ PLAY"));
        QPushButton* prev = buttonByText(panel, QString::fromUtf8("◀ PREV"));
        report("play_and_fkeys_are_marked_tx_keying",
               keyingMarked && play && play->property(kTxKeyingProperty).toBool()
                   && stop && prev
                   && !rec->property(kTxKeyingProperty).toBool()
                   && !stop->property(kTxKeyingProperty).toBool()
                   && !prev->property(kTxKeyingProperty).toBool());

        // The row reads REC, STOP, PLAY, PREV.
        QHBoxLayout* row = nullptr;
        for (auto* l : panel.findChildren<QHBoxLayout*>())
            if (l->indexOf(rec) >= 0) row = l;
        const bool ordered = row && stop && play && prev
            && row->indexOf(rec) < row->indexOf(stop)
            && row->indexOf(stop) < row->indexOf(play)
            && row->indexOf(play) < row->indexOf(prev);
        report("buttons_read_rec_stop_play_prev", ordered);

        // PREV previews to this computer; PLAY goes on the air.
        keyer.addRecording(1, QStringLiteral("Test"), 1000);
        keyer.calls.clear();
        prev->click();
        keyer.pushStatus(VoiceKeyer::Idle, -1);
        play->click();
        keyer.pushStatus(VoiceKeyer::Idle, -1);
        report("prev_previews_and_play_transmits",
               keyer.calls == QStringList({"previewStart 1", "playbackStart 1"}),
               joined(keyer.calls));
        keyer.recs.clear();
        keyer.calls.clear();
    }

    // An empty slot never goes on the air.
    f1->click();
    report("fkey_on_empty_slot_does_not_transmit", keyer.calls.isEmpty(), joined(keyer.calls));

    // A recorded slot does.
    keyer.addRecording(2, QStringLiteral("CQ"), 3000);
    keyer.calls.clear();
    f2->click();
    report("fkey_plays_recorded_slot",
           keyer.calls == QStringList{"playbackStart 2"}, joined(keyer.calls));

    // Pressing it again while it plays stops it.
    keyer.pushStatus(VoiceKeyer::Playback, 2);
    keyer.calls.clear();
    f2->click();
    report("fkey_again_while_playing_stops",
           keyer.calls == QStringList{"playbackStop 2"}, joined(keyer.calls));
    keyer.pushStatus(VoiceKeyer::Idle, -1);

    // REC records into the selected slot, and a second press stops it.
    keyer.calls.clear();
    f5->click();  // empty: selects slot 5 without transmitting
    rec->click();
    keyer.pushStatus(VoiceKeyer::Recording, 5);
    rec->click();
    report("rec_button_records_selected_slot",
           keyer.calls == QStringList({"recStart 5", "recStop 5"}), joined(keyer.calls));
    keyer.pushStatus(VoiceKeyer::Idle, -1);

    keyer.pushStatus(VoiceKeyer::Disabled, -1);
    report("disabled_status_text",
           status->text() == QString::fromUtf8("Disabled · not licensed (SmartSDR+)"),
           status->text().toStdString());

    emit keyer.commandFailed(QStringLiteral("rec_start"), 3, 1u,
                             QStringLiteral("port already in use on radio"));
    report("refusal_is_shown",
           status->text() == QLatin1String(
               "Record failed (slot 3): port already in use on radio"),
           status->text().toStdString());
    // …and stands out, through the themed `tone` property rather than an inline
    // stylesheet. The next status is quiet again.
    const bool refusalLoud = status->property("tone").toString() == QLatin1String("error");
    keyer.pushStatus(VoiceKeyer::Idle, -1);
    report("refusal_is_marked_error_until_the_next_status",
           refusalLoud && status->property("tone").toString().isEmpty(),
           status->property("tone").toString().toStdString());

    emit keyer.transferStatusChanged(QStringLiteral("Uploading slot 2"));
    const bool progressShown = status->text() == QLatin1String("Uploading slot 2");
    emit keyer.transferFinished(false, QStringLiteral("timeout"));
    report("transfer_progress_and_failure_are_shown",
           progressShown && status->text() == QLatin1String("Transfer failed: timeout"),
           status->text().toStdString());

    // F-key shortcuts start disabled (MainWindow enables them by mode) and,
    // once enabled, play the slot.
    QShortcut* f2Key = shortcutFor(panel, Qt::Key_F2);
    const bool startsDisabled = f2Key && !f2Key->isEnabled();
    panel.setShortcutsEnabled(true);
    keyer.pushStatus(VoiceKeyer::Idle, -1);
    keyer.calls.clear();
    if (f2Key) emit f2Key->activated();
    report("fkey_shortcut_disabled_until_enabled_then_plays",
           startsDisabled && keyer.calls == QStringList{"playbackStart 2"}, joined(keyer.calls));

    report("title_names_the_source",
           labelWithText(panel, QStringLiteral("Digital Voice Keyer (Radio)")) != nullptr);

    // A slot the keyer no longer holds shows as empty, not as its old name.
    keyer.recs.clear();
    emit keyer.recordingChanged(2);
    report("vanished_recording_resets_its_row",
           labelWithText(panel, QStringLiteral("CQ")) == nullptr
               && labelWithText(panel, QStringLiteral("Recording 2")) != nullptr);
    keyer.addRecording(2, QStringLiteral("CQ"), 3000);

    // A recorded slot stops reading as empty. The themed stylesheet dims a row
    // through its `empty` style property, so that property is the observable —
    // the panel no longer writes per-label colours.
    emit keyer.recordingChanged(2);
    {
        QLabel* name = labelWithText(panel, QStringLiteral("CQ"));
        QPushButton* f2Btn = buttonByText(panel, QStringLiteral("F2"));
        QPushButton* f3Btn = buttonByText(panel, QStringLiteral("F3"));
        report("recorded_row_drops_empty_while_an_unused_slot_keeps_it",
               name && !name->property("empty").toBool()
                   && f2Btn && !f2Btn->property("empty").toBool()
                   && f3Btn && f3Btn->property("empty").toBool());
    }

    // Switching keyers re-reads everything and cuts the old keyer off.
    FakeKeyer local;
    local.label = QStringLiteral("Local");
    local.maxMs = 30'000;
    local.recs.append({4, QStringLiteral("Local CQ"), 2500});
    const QString radioRecDescription = rec->accessibleDescription();
    panel.setKeyer(&local);
    // REC tells a screen reader the bound keyer's own limit, not the radio
    // DVK's 10 s whatever is bound.
    report("rec_description_names_the_bound_keyers_limit",
           radioRecDescription.contains(QLatin1String("up to 10 seconds"))
               && rec->accessibleDescription().contains(QLatin1String("up to 30 seconds")),
           (radioRecDescription + QStringLiteral(" | ") + rec->accessibleDescription()).toStdString());
    const bool titleOk = labelWithText(panel, QStringLiteral("Digital Voice Keyer (Local)")) != nullptr;
    const bool rowsOk = labelWithText(panel, QStringLiteral("Local CQ")) != nullptr
                        && labelWithText(panel, QStringLiteral("CQ")) == nullptr;
    emit keyer.commandFailed(QStringLiteral("rec_start"), 1, 1u, QStringLiteral("old keyer"));
    const bool oldCutOff = !status->text().contains(QLatin1String("old keyer"));
    keyer.calls.clear();
    buttonByText(panel, QStringLiteral("F4"))->click();
    report("set_keyer_rereads_rows_title_and_routes_presses",
           titleOk && rowsOk && oldCutOff && keyer.calls.isEmpty()
               && local.calls == QStringList{"playbackStart 4"},
           joined(local.calls));

    // The local keyer on a radio it is not yet verified on says so in the title.
    panel.setSourceQualifier(QStringLiteral("Beta"));
    const bool betaShown = labelWithText(panel, QString::fromUtf8("Digital Voice Keyer (Local · Beta)")) != nullptr;
    panel.setSourceQualifier(QString());
    report("title_carries_a_beta_qualifier_and_drops_it",
           betaShown && labelWithText(panel, QStringLiteral("Digital Voice Keyer (Local)")) != nullptr);

    // An inline rename belongs to the keyer it started on. Rebinding the panel
    // mid-edit — a source change, or Automatic following a licence status —
    // must not commit it to the other keyer, where a local name would land on
    // the radio's shared slot (#6292 review). Edit, rebind, then submit.
    {
        FakeKeyer first;
        FakeKeyer other;
        DvkPanel renamePanel(&first);
        QLabel* name = labelWithText(renamePanel, first.defaultSlotName(1));
        QLineEdit* editor = nullptr;
        if (name) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(2, 2), QPointF(2, 2),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(name, &dbl);
            for (QLineEdit* e : renamePanel.findChildren<QLineEdit*>()) {
                if (e->property("dvkRole").toString() == QLatin1String("rename"))
                    editor = e;
            }
        }
        const bool editing = editor != nullptr;
        const QPointer<QLineEdit> held(editor);
        if (editor)
            editor->setText(QStringLiteral("Started on first"));
        renamePanel.setKeyer(&other);
        if (held)
            emit held->returnPressed();   // the operator presses Return after the swap
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const auto renamed = [](const FakeKeyer& k) {
            return std::any_of(k.calls.cbegin(), k.calls.cend(),
                               [](const QString& c) { return c.startsWith(QLatin1String("setName")); });
        };
        report("rename_does_not_follow_a_rebind_to_the_other_keyer",
               editing && !renamed(first) && !renamed(other),
               (QStringLiteral("editing=%1 first=[%2] other=[%3]").arg(editing)
                    .arg(joined(first.calls).c_str(), joined(other.calls).c_str())).toStdString());
    }

    {
        FakeKeyer first;
        FakeKeyer other;
        first.addRecording(1, QStringLiteral("Local CQ"), 1000);
        DvkPanel menuPanel(&first);
        QWidget* row = menuPanel.findChild<QWidget*>(QStringLiteral("dvkSlot1"));
        bool triggered = false;
        QTimer::singleShot(0, &menuPanel, [&] {
            QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                return;
            }
            menuPanel.setKeyer(&other);
            for (QAction* action : menu->actions()) {
                if (action->text() == QLatin1String("Clear")) {
                    triggered = true;
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });
        if (row) {
            emit row->customContextMenuRequested(QPoint(2, 2));
        }
        report("slot_menu_does_not_follow_a_rebind_to_the_other_keyer",
               row && triggered && first.calls.isEmpty() && other.calls.isEmpty(),
               (QStringLiteral("triggered=%1 first=[%2] other=[%3]").arg(triggered)
                    .arg(joined(first.calls).c_str(), joined(other.calls).c_str())).toStdString());
    }

    // The keyer source is chosen from the panel's own settings button: it is
    // a real, focusable, named button, and pressing it asks MainWindow (which
    // holds the licence state) for the menu.
    {
        FakeKeyer k;
        DvkPanel gearPanel(&k);
        QPushButton* gear = gearPanel.findChild<QPushButton*>(QStringLiteral("dvkSettings"));
        int requested = 0;
        QObject::connect(&gearPanel, &DvkPanel::settingsRequested, [&] { ++requested; });
        if (gear) {
            gear->click();
        }
        // Return presses it too: Space may be bound to PTT, which the app-wide
        // filter takes before the button sees it.
        int byReturn = 0;
        if (gear) {
            const int before = requested;
            QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(gear, &ret);
            byReturn = requested - before;
        }
        report("settings_button_is_pressed_by_return", byReturn == 1,
               QStringLiteral("requests=%1").arg(byReturn).toStdString());
        report("settings_button_requests_the_source_menu",
               gear && requested == 1 + byReturn && gear->focusPolicy() != Qt::NoFocus
                   && gear->accessibleName() == QLatin1String("Voice keyer settings"),
               (QStringLiteral("found=%1 requested=%2").arg(gear != nullptr).arg(requested))
                   .toStdString());
    }

    // The keyboard path to a slot's menu (#7152): the Menu key or Shift+F10 on
    // a slot's focused F-key opens that slot's menu, the one a right-click on
    // the row opens; plain F10 does not, and an unavailable keyer offers none.
    {
        FakeKeyer k;
        k.addRecording(3, QStringLiteral("Contest CQ"), 1000);
        DvkPanel keysPanel(&k);
        QPushButton* f3 = buttonByText(keysPanel, QStringLiteral("F3"));
        // Sends one key press to the F-key and reports whether a slot menu
        // opened; with clear set, picks Clear from it.
        const auto press = [&](int key, Qt::KeyboardModifiers mods, bool clear) {
            bool opened = false;
            QTimer::singleShot(0, &keysPanel, [&] {
                QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) {
                    return;
                }
                opened = true;
                for (QAction* action : menu->actions()) {
                    if (clear && action->text() == QLatin1String("Clear")) {
                        action->trigger();
                        break;
                    }
                }
                menu->close();
            });
            if (f3) {
                QKeyEvent ev(QEvent::KeyPress, key, mods);
                QApplication::sendEvent(f3, &ev);
            }
            QCoreApplication::processEvents();   // retire the timer if nothing opened
            return opened;
        };

        const bool focusable = f3 && f3->focusPolicy() != Qt::NoFocus && f3->isEnabled();
        // Shown, as it is whenever its F-keys can have focus: the first popup
        // from a panel that was never shown is not reported active on macOS.
        keysPanel.show();
        QCoreApplication::processEvents();
        const bool byMenuKey = press(Qt::Key_Menu, Qt::NoModifier, true);
        const bool clearedSlot3 = k.calls.contains(QStringLiteral("clear 3"));
        report("menu_key_on_an_fkey_opens_that_slots_menu",
               focusable && byMenuKey && clearedSlot3,
               (QStringLiteral("focusable=%1 opened=%2 calls=[%3]")
                    .arg(focusable).arg(byMenuKey).arg(joined(k.calls).c_str())).toStdString());

        report("shift_f10_on_an_fkey_opens_that_slots_menu",
               press(Qt::Key_F10, Qt::ShiftModifier, false));

        // Where the platform raises a keyboard context-menu event instead of a
        // key press (the Menu key on Linux, Shift+F10 on Windows), Qt sends it
        // to the focused F-key. The F-key ignores it and Qt passes it up to the
        // row, whose custom context menu is the slot's menu.
        k.calls.clear();
        bool byNativeEvent = false;
        QTimer::singleShot(0, &keysPanel, [&] {
            QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) {
                return;
            }
            byNativeEvent = true;
            for (QAction* action : menu->actions()) {
                if (action->text() == QLatin1String("Clear")) {
                    action->trigger();
                    break;
                }
            }
            menu->close();
        });
        if (f3) {
            QContextMenuEvent ev(QContextMenuEvent::Keyboard, QPoint(0, 0),
                                 f3->mapToGlobal(QPoint(0, 0)));
            QApplication::sendEvent(f3, &ev);
        }
        QCoreApplication::processEvents();
        report("keyboard_context_menu_event_on_an_fkey_opens_that_slots_menu",
               byNativeEvent && k.calls.contains(QStringLiteral("clear 3")),
               (QStringLiteral("opened=%1 calls=[%2]").arg(byNativeEvent)
                    .arg(joined(k.calls).c_str())).toStdString());

        k.calls.clear();
        const bool byPlainF10 = press(Qt::Key_F10, Qt::NoModifier, false);
        report("plain_f10_on_an_fkey_opens_no_menu", !byPlainF10 && k.calls.isEmpty(),
               joined(k.calls));

        keysPanel.setUnavailableReason(QStringLiteral("This radio has no DVK. Choose Local with ⚙."));
        report("unavailable_keyer_opens_no_slot_menu_from_the_keyboard",
               !press(Qt::Key_Menu, Qt::NoModifier, false)
                   && !press(Qt::Key_F10, Qt::ShiftModifier, false));
    }

    // Tab order (#7152): the settings button is the panel's first stop, so the
    // keyer source is one Tab away, and the slots' F-keys follow in slot
    // order, each one Tab from the next, so every slot's menu is reachable.
    {
        FakeKeyer k;
        DvkPanel chainPanel(&k);
        chainPanel.show();
        QCoreApplication::processEvents();
        QStringList stops;
        QWidget* start = chainPanel.findChild<QWidget*>(QStringLiteral("dvkSettings"));
        for (QWidget* w = start; w; ) {
            if ((w->focusPolicy() & Qt::TabFocus) && w->isVisibleTo(&chainPanel)
                && chainPanel.isAncestorOf(w)) {
                stops << w->objectName();
            }
            w = w->nextInFocusChain();
            if (w == start) {
                break;
            }
        }
        QStringList fkeys;
        for (const QString& name : stops) {
            if (name.startsWith(QLatin1String("dvkPlaySlot"))) {
                fkeys << name;
            }
        }
        QStringList expectedFkeys;
        for (int id = 1; id <= 12; ++id) {
            expectedFkeys << QStringLiteral("dvkPlaySlot%1").arg(id);
        }
        report("tab_order_settings_first_then_fkeys_in_slot_order",
               !stops.isEmpty() && stops.first() == QLatin1String("dvkSettings")
                   && fkeys == expectedFkeys
                   && stops.indexOf(QStringLiteral("dvkPlaySlot1"))
                          < stops.indexOf(QStringLiteral("dvkRecord")),
               stops.join(QLatin1Char(' ')).toStdString());
    }

    // A keyer the radio cannot serve leaves the panel open (that is where the
    // source is changed) with every slot and transport control disabled and the
    // reason on the status line; only the settings button still acts.
    {
        FakeKeyer k;
        k.addRecording(1, QStringLiteral("CQ"), 1000);
        DvkPanel blockedPanel(&k);
        const QString reason = QStringLiteral("Radio DVK not licensed (SmartSDR+). Choose Local with ⚙.");
        blockedPanel.setUnavailableReason(reason);
        QPushButton* bf1 = buttonByText(blockedPanel, QStringLiteral("F1"));
        QPushButton* brec = buttonByText(blockedPanel, QString::fromUtf8("● REC"));
        QPushButton* bplay = buttonByText(blockedPanel, QString::fromUtf8("▶ PLAY"));
        QPushButton* bprev = buttonByText(blockedPanel, QString::fromUtf8("◀ PREV"));
        QPushButton* gear = blockedPanel.findChild<QPushButton*>(QStringLiteral("dvkSettings"));
        QLabel* bstatus = statusLabel(blockedPanel);
        const bool allFound = bf1 && brec && bplay && bprev && gear && bstatus;
        const bool controlsOff = allFound && !bf1->isEnabled() && !brec->isEnabled()
                                 && !bplay->isEnabled() && !bprev->isEnabled();
        const bool reasonShown = allFound && bstatus->text() == reason;
        if (allFound) {
            bf1->click();   // a disabled button ignores this; the guard is belt and braces
        }
        bool menuShown = false;
        QTimer::singleShot(0, &blockedPanel, [&] {
            if (QWidget* popup = QApplication::activePopupWidget()) {
                menuShown = true;
                popup->close();
            }
        });
        if (QWidget* row = blockedPanel.findChild<QWidget*>(QStringLiteral("dvkSlot1"))) {
            emit row->customContextMenuRequested(QPoint(2, 2));
        }
        QCoreApplication::processEvents();
        bool renameOpened = false;
        if (QLabel* name = labelWithText(blockedPanel, QStringLiteral("CQ"))) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(2, 2), QPointF(2, 2),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(name, &dbl);
            for (QLineEdit* e : blockedPanel.findChildren<QLineEdit*>()) {
                renameOpened = renameOpened || e->isVisibleTo(&blockedPanel);
            }
        }
        report("unavailable_keyer_offers_no_rename", !renameOpened);
        report("unavailable_keyer_disables_slots_and_transport_not_settings",
               controlsOff && reasonShown && gear->isEnabled() && !menuShown && k.calls.isEmpty(),
               (QStringLiteral("found=%1 off=%2 reason=%3 menu=%4 calls=[%5]")
                    .arg(allFound).arg(controlsOff).arg(reasonShown).arg(menuShown)
                    .arg(joined(k.calls).c_str())).toStdString());

        blockedPanel.setUnavailableReason(QString());
        report("clearing_the_reason_restores_the_controls",
               allFound && bf1->isEnabled() && brec->isEnabled() && bplay->isEnabled()
                   && bstatus->text() == QLatin1String("Idle"),
               allFound ? bstatus->text().toStdString() : std::string());

        // An operation already in flight can still be ended.
        k.pushStatus(VoiceKeyer::Playback, 1);
        blockedPanel.setUnavailableReason(reason);
        QPushButton* bstop = buttonByText(blockedPanel, QString::fromUtf8("■ STOP"));
        report("unavailable_keyer_still_lets_stop_end_an_operation",
               allFound && bstop && bstop->isEnabled() && bplay->isEnabled());
        k.pushStatus(VoiceKeyer::Idle, -1);
    }

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
