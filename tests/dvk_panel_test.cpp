// DVK panel driving a VoiceKeyer — the panel's contract, with no radio behind it.
// Run: ./build/dvk_panel_test

#include "TestSettingsProfile.h"
#include "core/TxKeyingMarker.h"
#include "gui/DvkPanel.h"
#include "models/VoiceKeyer.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStringList>

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
    int maxRecordingMs() const override { return 10'000; }
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
           status->text() == QString::fromUtf8("Disabled · SmartSDR+ required"),
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
    local.recs.append({4, QStringLiteral("Local CQ"), 2500});
    panel.setKeyer(&local);
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

    std::printf("\n%s (%d failed)\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed ? 1 : 0;
}
