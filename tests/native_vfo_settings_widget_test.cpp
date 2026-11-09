#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeVfoSettingsWidget.h"
#include "gui/NativeFilterSettingsWidget.h"
#include "gui/NetworkAudioLevelsWidget.h"
#include "gui/NativePowerLimitsWidget.h"
#include "gui/NativeTransmitInputsWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QAccessible>
#include <QScopeGuard>
#include <QLocale>
#include <QByteArray>
#include <QList>
#include <QCoreApplication>
#include <QMetaObject>
#include <QVariantMap>
#include <QtTest>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPixmap>
#include <QDir>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <limits>
#include <thread>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend, bool administrator = false) {
        backend.m_clock.start(); backend.m_loginAdministrator = administrator;
        backend.m_protocol.start("fixture-user", "fixture-password", administrator, 0);
        backend.m_protocol.receive("##CN1;", 1); backend.m_protocol.receive("##ID1;", 2); backend.ready();
    }
    static QVariantMap relative(const KenwoodBackend& backend) { return backend.relativeTuningStatus(); }
    static void observe(KenwoodBackend& backend, const QByteArray& value) { backend.adopt(value); }
};
}
using namespace AetherSDR;
namespace {
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

// Production radio/model/widget probe; observers precede widget connections.
struct ActualFixture {
    QList<QByteArray> commands;
    std::function<void(const QString&, const QVariantMap&)> onStatus;
    std::function<void(quint64, const QVariant&)> onResult;
    std::function<void(const QByteArray&)> onWrite;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    bool administrator = false;
    explicit ActualFixture(bool admin = false) : administrator(admin) { install(); }
    void install() {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& wire) { commands.append(wire); if (onWrite) { onWrite(wire); } return true; }, {}});
        backend = owned.get(); model.setBackendForTest(std::move(owned), "kenwood");
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& state) { if (onStatus) { onStatus(kind, state); } });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model,
            [this](quint64 id, const QVariant& result) { if (onResult) { onResult(id, result); } });
        kenwood::KenwoodBackendTestAccess::authenticate(*backend, administrator);
        for (const QByteArray& value : {QByteArray("ID024"), QByteArray("XV0"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("FB00007074000"), QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX"), QByteArray("MO20")}) { observe(value); }
        // setBackendForTest omits setupBackend's connection relay. This public
        // notification binds the production extension route without copying it.
        emit model.connectionStateChanged(true);
    }
    void observe(const QByteArray& value) { kenwood::KenwoodBackendTestAccess::observe(*backend, value); }
};
void axesPushCannotOverwriteNestedFmVocabulary() {
    ActualFixture f; NativeFilterSettingsWidget widget(f.model);
    f.observe("EX00610 003"); f.observe("FL001"); f.observe("SL010"); f.observe("SH0005");
    bool nested = false;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) {
        if (!nested && kind == "filter" && state.value("mode") == "CW") {
            nested = true; f.observe("SF0000140740004");
        }
    };
    f.onWrite = [&](const QByteArray& wire) { if (wire == "SL011;") { f.observe("SL011"); } };
    auto* low = widget.findChild<QComboBox*>("nativeFilterLow"); auto* high = widget.findChild<QComboBox*>("nativeFilterHigh");
    if (low && !low->isEnabled()) { std::cerr << "CW axis unavailable: " << low->accessibleDescription().toStdString() << " mode=" << widget.findChild<QLabel*>("nativeFilterMode")->text().toStdString() << '\n'; }
    require(low && high && low->isEnabled(), "actual confirmed CW native axis control enabled");
    low->setCurrentIndex(11); QMetaObject::invokeMethod(low, "activated", Qt::DirectConnection, Q_ARG(int, 11));
    require(nested && f.commands.contains("SL011;"), "actual axis edit observes older CW response before nested FM status");
    auto* mode = widget.findChild<QLabel*>("nativeFilterMode");
    require(mode && mode->text() == "Current mode: FM" && low->count() == 12 && high->count() == 24,
        "filter axes vocabulary must remain nested current FM rather than outer stale CW layout");
}
void tuningPendingCannotOutliveTfSetWithdrawal() {
    ActualFixture f; NativeVfoSettingsWidget widget(f.model);
    f.observe("FS00"); f.observe("MH0"); f.observe("LK0"); bool nested = false;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) {
        if (!nested && kind == "tuning" && state.value("pending").toMap().value("receiveFine").toBool()) {
            nested = true; f.observe("TS1");
        }
    };
    auto* fine = widget.findChild<QCheckBox*>("nativeTuning_receiveFine"); require(fine && fine->isEnabled(), "actual fresh receive FINE checkbox enabled");
    f.commands.clear(); fine->setFocus(); QTest::keyClick(fine, Qt::Key_Space);
    require(nested && !f.commands.contains("FS1;"), "actual FINE event retires before setter dispatch");
    for (const QString& field : QStringList{"receiveFine", "mhzStep", "locked"}) {
        auto* box = widget.findChild<QCheckBox*>("nativeTuning_" + field);
        require(box && box->checkState() == Qt::PartiallyChecked && !box->isEnabled(),
            "tuning confirmed checkboxes remain unknown after TF-SET despite old outer pending push");
    }
}
void vfoConfirmedReadbackCannotRegressAfterNestedFrequency() {
    ActualFixture f; NativeVfoSettingsWidget widget(f.model); bool nested = false;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) {
        if (!nested && kind == "vfo" && state.value("frequencyAHz").toLongLong() == 14075000) {
            nested = true; f.observe("FA00014076000");
        }
    };
    // This is a radio observation operation: the earlier subscriber injects
    // newer observed truth before the older outer publication reaches the page.
    f.observe("FA00014075000");
    require(nested, "VFO observation probe receives nested newer radio frequency before outer status delivery");
    auto* confirmed = widget.findChild<QLabel*>("nativeVfoReadback");
    require(confirmed && confirmed->text().contains("Confirmed A: 14.076000 MHz"),
        "confirmed VFO label must show nested radio14.076 instead of outer14.075, independently of draft cursor");
}

void axesRevisionValidationGetAndReentry() {
    ActualFixture f; QVariantMap current;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) { if (kind == "filter") { current = state; } };
    NativeFilterSettingsWidget widget(f.model); f.observe("EX00610 003"); f.observe("FL001"); f.observe("SL010"); f.observe("SH0005");
    const QVariantMap old = current; f.observe("SF0000140740004"); emit f.backend->extensionStatus("kenwood", "filter", old);
    auto* mode = widget.findChild<QLabel*>("nativeFilterMode"); auto* low = widget.findChild<QComboBox*>("nativeFilterLow");
    require(mode->text() == "Current mode: FM" && low->count() == 12, "older axes push cannot restore CW vocabulary");
    int recoveryIndex = 1;
    for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
        const quint64 before = current.value("statusRevision").toString().toULongLong();
        QVariantMap bad = current; bad["statusRevision"] = high; bad["editable"] = 1; emit f.backend->extensionStatus("kenwood", "filter", bad);
        require(!low->isEnabled(), "malformed high axes body fails closed");
        // A mode transition requires current bank confirmation before axes are
        // admissible; an SL frame alone cannot retire that bank barrier.
        f.observe("FL001");
        f.observe("SL0" + QByteArray::number(recoveryIndex).rightJustified(2, '0'));
        f.observe("SH0" + QByteArray::number(recoveryIndex + 4).rightJustified(3, '0'));
        ++recoveryIndex;
        require(current.value("statusRevision").toString().toULongLong() > before
            && current.value("statusRevision").toString().toULongLong() < high.toULongLong(),
            "qualified current bank/axes produce a fresh lower revision after malformed high token");
        require(mode->text() == "Current mode: FM" && low->count() == 12 && low->isEnabled(), "current lower axes push recovers without poisoned floor");
    }
    // Page binding itself invokes the production filter.get route.
    bool nestedGet = false; f.onResult = [&](quint64, const QVariant& value) {
        if (!nestedGet && value.toMap().contains("axes")) { nestedGet = true; f.observe("SF0000140740003"); }
    };
    NativeFilterSettingsWidget second(f.model);
    require(nestedGet && second.findChild<QLabel*>("nativeFilterMode")->text() == "Current mode: CW", "newer current axes push supersedes older GET response"); f.onResult = {};
    const bool active = QAccessible::isActive(); const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
    const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
    QAccessible::setActive(true); if (!QAccessible::isActive()) { std::cerr << "SKIP axes a11y callback: inactive platform backend\n"; return; }
    f.observe("FL001"); f.observe("SL010"); f.observe("SH0005"); bool nested = false;
    accessibilityCallback = [&](QAccessibleEvent* event) {
        if (!nested && event->type() == QAccessible::ValueChanged && event->object() == low) { nested = true; f.observe("SF0000140740004"); }
    };
    f.observe("SL011"); require(nested && mode->text() == "Current mode: FM" && low->count() == 12, "axes a11y nested mode keeps actual vocabulary");
}
void newBindingResetsFourConsumerFloors() {
    ActualFixture f; NativeVfoSettingsWidget vfo(f.model); NativeFilterSettingsWidget filter(f.model);
    for (int read = 0; read < 20; ++read) { f.observe("FA00014074000"); f.observe("FS00"); f.observe("MH0"); f.observe("LK0"); f.observe("SL010"); }
    f.install(); f.observe("EX00610 003"); f.observe("FL001"); f.observe("SL010"); f.observe("SH0005");
    f.observe("FA00014079000"); f.observe("FS00"); f.observe("MH0"); f.observe("LK0");
    require(vfo.findChild<QLabel*>("nativeVfoReadback")->text().contains("14.079000 MHz"), "new VFO producer lower sequence accepted after binding reset");
    require(vfo.findChild<QCheckBox*>("nativeTuning_receiveFine")->isEnabled(), "new tuning producer lower sequence accepted after binding reset");
    require(filter.findChild<QComboBox*>("nativeFilterLow")->currentIndex() == 10, "new axes producer accepted after binding reset");
    QVariantMap bankQuery;
    constexpr quint64 bankRequest = 0x42414e4bULL;
    bool bankResultReceived = false;
    QString bankError;
    f.onResult = [&](quint64 request, const QVariant& result) {
        if (request == bankRequest) {
            bankResultReceived = true;
            require(result.typeId() == QMetaType::QVariantMap, "bank getter returns typed descriptor map");
            bankQuery = result.toMap();
        }
    };
    const QMetaObject::Connection errorConnection = QObject::connect(f.backend, &IRadioBackend::extensionError, &f.model,
        [&](quint64 request, const QString& error) { if (request == bankRequest) { bankError = error; } });
    f.model.invokeBackendExtension("kenwood", "filter.banks.get", bankRequest);
    QObject::disconnect(errorConnection);
    require(bankError.isEmpty() && bankResultReceived && bankQuery.value("controls").typeId() == QMetaType::QVariantList,
        "no-argument actual bank query is admitted and returns a controls descriptor");
    bool observedBankA = false;
    for (const QVariant& row : bankQuery.value("controls").toList()) {
        const QVariantMap control = row.toMap();
        if (control.value("id") == "active") { observedBankA = control.contains("value") && control.value("value").toInt() == 0; }
    }
    require(observedBankA, "actual getter confirms FL001 means bankA0 with optional roofing installed1");
    f.onResult = {};
    auto* active = filter.findChild<QComboBox*>("nativeFilterBank_active");
    require(active && active->currentIndex() >= 0 && active->currentData().toInt() == 0, "new bank producer accepted after binding reset");
}
void itemModelReentryCannotCorruptNewerAxesVocabulary() {
    ActualFixture f; NativeFilterSettingsWidget widget(f.model);
    f.observe("EX00610 003"); f.observe("FL001"); f.observe("SL010"); f.observe("SH0005");
    auto* low = widget.findChild<QComboBox*>("nativeFilterLow"); auto* high = widget.findChild<QComboBox*>("nativeFilterHigh");
    require(low && high && low->isEnabled(), "item-model probe starts from qualified CW axis");
    bool nested = false;
    QObject::connect(low->model(), &QAbstractItemModel::rowsInserted, &f.model,
        [&](const QModelIndex&, int, int) {
            if (!nested) { nested = true; f.observe("SF0000140740004"); }
        });
    f.observe("SL011");
    require(nested, "combo item-model rowsInserted delivered synchronous nested FM status");
    require(widget.findChild<QLabel*>("nativeFilterMode")->text() == "Current mode: FM"
        && low->count() == 12 && high->count() == 24,
        "item-model reentry must preserve exact newer FM vocabulary without stale outer insertions");
}
void malformedVfoMetadataCannotOverwriteExistingDraft() {
    ActualFixture f; QVariantMap current;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) { if (kind == "vfo") { current = state; } };
    NativeVfoSettingsWidget widget(f.model); widget.resize(800, 650); widget.show(); QApplication::processEvents();
    f.observe("FA00014075000"); auto* draft = widget.findChild<QDoubleSpinBox*>("nativeVfoAFrequency");
    require(draft, "draft-preservation probe has actual VFO A editor");
    draft->setFocus(); draft->selectAll(); QTest::keyClicks(draft, draft->locale().toString(14.123, 'f', 6)); draft->interpretText();
    require(qAbs(draft->value() - 14.123) < 0.0000001, "locale-aware existing user draft qualified before invalid metadata");
    QVariantMap bad = current; bad["statusRevision"] = QString("1000000"); bad["txActive"] = 1;
    emit f.backend->extensionStatus("kenwood", "vfo", bad); f.observe("FA00014076000");
    require(widget.findChild<QLabel*>("nativeVfoReadback")->text().contains("Confirmed A: 14.076000 MHz"),
        "valid lower current producer readback recovers after malformed metadata");
    require(qAbs(draft->value() - 14.123) < 0.0000001,
        "malformed metadata and later valid frequency observation must preserve established user draft14.123");
}
void actualVfoApplyUsesLocaleAwareUserInput() {
    ActualFixture f; NativeVfoSettingsWidget widget(f.model); widget.resize(800, 650); widget.show(); QApplication::processEvents();
    auto* draft = widget.findChild<QDoubleSpinBox*>("nativeVfoAFrequency"); auto* apply = widget.findChild<QPushButton*>("nativeVfoAApply");
    require(draft && apply && apply->isEnabled(), "visible actual VFO Apply is available");
    draft->setFocus(); draft->selectAll(); QTest::keyClicks(draft, draft->locale().toString(14.075, 'f', 6));
    draft->interpretText();
    require(qAbs(draft->value() - 14.075) < 0.0000001, "locale-aware draft entered expected MHz before applying");
    f.commands.clear(); QTest::mouseClick(apply, Qt::LeftButton);
    require(f.commands.contains("FA00014075000;"), "actual visible locale-aware Apply sends physical VFO A target");
    f.observe("FA00014075000");
    require(widget.findChild<QLabel*>("nativeVfoReadback")->text().contains("Confirmed A: 14.075000 MHz"), "actual Apply is confirmed only by received frequency");
}
void vfoAndTuningMalformedOrderingRecovery() {
    ActualFixture f; QVariantMap vfoState; QVariantMap tuningState;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) { if (kind == "vfo") { vfoState = state; } if (kind == "tuning") { tuningState = state; } };
    NativeVfoSettingsWidget widget(f.model); f.observe("FA00014075000"); f.observe("FS00"); f.observe("MH0"); f.observe("LK0");
    const QVariantMap oldVfo = vfoState; f.observe("FA00014076000"); emit f.backend->extensionStatus("kenwood", "vfo", oldVfo);
    auto* label = widget.findChild<QLabel*>("nativeVfoReadback"); require(label->text().contains("14.076000 MHz"), "older valid VFO push ignored");
    const QList<QVariant> badRevisions{QVariant(1), QVariant(true), QVariant("01"), QVariant("-1"), QVariant("18446744073709551616")};
    for (const QVariant& revision : badRevisions) {
        QVariantMap bad = vfoState; bad["statusRevision"] = revision; emit f.backend->extensionStatus("kenwood", "vfo", bad);
        require(label->text().contains("Confirmed A: Unknown"), "malformed VFO revision cannot display confirmed frequency");
        f.observe("FA00014076000"); require(label->text().contains("14.076000 MHz"), "current actual VFO push recovers malformed descriptor");
    }
    for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
        for (const QString& field : QStringList{"frequencyAHz", "txActive", "modeA", "transmitSelectable"}) {
            QVariantMap bad = vfoState; bad["statusRevision"] = high;
            bad[field] = field == "frequencyAHz" ? QVariant(true) : field == "txActive" ? QVariant(1)
                : field == "modeA" ? QVariant(QString()) : QVariant(1);
            emit f.backend->extensionStatus("kenwood", "vfo", bad);
            require(label->text().contains("Confirmed A: Unknown"), "malformed typed VFO payload fails closed");
            f.observe("FA00014076000"); require(label->text().contains("14.076000 MHz"), "malformed high VFO revision does not poison actual lower push recovery");
        }
        QVariantMap bad = tuningState; bad["statusRevision"] = high; bad["receiveFine"] = 1;
        emit f.backend->extensionStatus("kenwood", "tuning", bad);
        auto* fine = widget.findChild<QCheckBox*>("nativeTuning_receiveFine"); require(fine->checkState() == Qt::PartiallyChecked, "malformed tuning bool becomes unknown");
        auto* read = widget.findChild<QPushButton*>("nativeTuningRead"); require(read->isEnabled(), "tuning Read remains available after malformed description");
        f.commands.clear(); read->click();
        require(f.commands.contains("FS;") && f.commands.contains("MH;") && f.commands.contains("LK;"), "actual tuning Read recovery queries all fields");
        f.observe("FS00"); f.observe("MH0"); f.observe("LK0"); require(fine->checkState() == Qt::Unchecked && fine->isEnabled(), "fresh lower tuning producer revision recovers");
    }
}
void newerVfoGetAndAccessibilityReentry() {
    ActualFixture f; bool nestedGet = false;
    f.onResult = [&](quint64, const QVariant& value) { if (!nestedGet && value.toMap().contains("frequencyAHz")) { nestedGet = true; f.observe("FA00014076000"); } };
    NativeVfoSettingsWidget widget(f.model); auto* label = widget.findChild<QLabel*>("nativeVfoReadback");
    require(nestedGet && label->text().contains("14.076000 MHz"), "nested current VFO push supersedes older initial GET result");
    f.onResult = {};
    const bool active = QAccessible::isActive(); const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
    const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
    QAccessible::setActive(true);
    if (!QAccessible::isActive()) { std::cerr << "SKIP explicit VFO accessibility callback: no active platform backend\n"; return; }
    bool nested = false;
    accessibilityCallback = [&](QAccessibleEvent* event) {
        if (!nested && event->type() == QAccessible::ValueChanged && event->object() == label) { nested = true; f.observe("FA00014078000"); }
    };
    f.observe("FA00014077000");
    require(nested && label->text().contains("14.078000 MHz"), "a11y reentry retains nested latest confirmed VFO readback");
    nested = false; accessibilityCallback = [&](QAccessibleEvent* event) {
        if (!nested && event->type() == QAccessible::ValueChanged && event->object() == label) { nested = true; f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false); }
    };
    f.observe("FA00014079000"); require(nested && label->text().contains("Confirmed A: Unknown"), "a11y disconnect cannot restore retired confirmed frequency");
}

int actualOutputGetter(ActualFixture& fixture) {
    constexpr quint64 request = 0x4155444fULL;
    QVariantMap state; bool received = false; QString error;
    const QMetaObject::Connection resultConnection = QObject::connect(fixture.backend, &IRadioBackend::extensionResult, &fixture.model,
        [&](quint64 id, const QVariant& value) { if (id == request) { received = true; require(value.typeId() == QMetaType::QVariantMap, "audio getter returns typed map"); state = value.toMap(); } });
    const QMetaObject::Connection errorConnection = QObject::connect(fixture.backend, &IRadioBackend::extensionError, &fixture.model,
        [&](quint64 id, const QString& value) { if (id == request) { error = value; } });
    fixture.model.invokeBackendExtension("kenwood", "audio.levels.get", request);
    QObject::disconnect(resultConnection); QObject::disconnect(errorConnection);
    require(received && error.isEmpty() && state.value("outputLevel").typeId() == QMetaType::Int, "no-argument production audio getter is admitted with known output");
    require(!state.contains("inputLevel"), "output-only observations do not invent an input level");
    return state.value("outputLevel").toInt();
}
void requireNoAudioLevelSetters(const ActualFixture& fixture) {
    for (const QByteArray& wire : fixture.commands) {
        require(!((wire.startsWith("##KN31") || wire.startsWith("##KN30")) && wire.size() == 10),
            "RX output readback probes never issue input/output setters or Apply actions");
    }
}
void audioOutputOlderPushCannotReplaceNewerObservation() {
    ActualFixture f; bool nested = false;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) {
        if (!nested && kind == "audio.levels" && state.value("outputLevel").toInt() == 60) {
            nested = true; f.observe("##KN31070");
        }
    };
    NetworkAudioLevelsWidget widget(f.model); f.observe("##KN31060");
    require(nested, "earlier output-status subscriber observes newer70 before outer60 delivery"); f.onStatus = {};
    require(actualOutputGetter(f) == 70, "actual backend output remains latest70 after nested push"); requireNoAudioLevelSetters(f);
    auto* confirmed = widget.findChild<QLabel*>("networkAudioLevelReadback");
    require(confirmed && confirmed->text().contains("Output: 70"), "confirmed RX output must retain newer70 after older outer push60");
}
void audioOutputOlderGetCannotReplaceNewerObservation() {
    ActualFixture f; f.observe("##KN31060"); bool nested = false;
    f.onResult = [&](quint64, const QVariant& result) {
        const QVariantMap state = result.toMap();
        if (!nested && state.contains("outputLevel") && state.value("outputLevel").toInt() == 60) {
            nested = true; f.observe("##KN31070");
        }
    };
    NetworkAudioLevelsWidget widget(f.model); require(nested, "initial matching GET snapshot60 follows newer production output70 push"); f.onResult = {};
    require(actualOutputGetter(f) == 70, "actual backend getter confirms latest70 after old matching GET"); requireNoAudioLevelSetters(f);
    auto* confirmed = widget.findChild<QLabel*>("networkAudioLevelReadback");
    require(confirmed && confirmed->text().contains("Output: 70"), "confirmed RX output must retain newer70 after older matching GET60");
}

void audioOutputMetadataOrderingAndDraftRecovery() {
    ActualFixture f(true); QVariantMap current;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) { if (kind == "audio.levels") { current = state; } };
    NetworkAudioLevelsWidget widget(f.model); widget.resize(750, 300); widget.show(); QApplication::processEvents();
    f.observe("##KN31060"); const QVariantMap old = current; f.observe("##KN31070");
    auto* label = widget.findChild<QLabel*>("networkAudioLevelReadback"); auto* output = widget.findChild<QSpinBox*>("networkAudioOutputLevel");
    require(label && output && output->isEnabled(), "administrator fixture permits staging RX output draft without Apply");
    emit f.backend->extensionStatus("kenwood", "audio.levels", old);
    require(label->text().contains("Output: 70"), "older output push cannot replace latest confirmed output");
    f.observe("##KN31070");
    require(current.value("statusRevision").toString().toULongLong() > old.value("statusRevision").toString().toULongLong()
        && current.value("outputLevel").toInt() == 70, "fresh real output publication qualifies later malformed-body validation independently of stale-push rejection");
    output->setFocus(); output->selectAll(); QTest::keyClicks(output, "83"); output->interpretText();
    require(output->value() == 83, "actual keyboard stages output-only draft83");
    const QList<QVariant> malformedLevels{QVariant(-1), QVariant(101), QVariant(2.5), QVariant(true), QVariant("70"),
        QVariant(std::numeric_limits<double>::quiet_NaN()), QVariant(std::numeric_limits<double>::infinity())};
    for (const QVariant& value : malformedLevels) {
        QVariantMap bad = current; bad["outputLevel"] = value; emit f.backend->extensionStatus("kenwood", "audio.levels", bad);
        require(label->text().contains("Output: Unknown"), "malformed output level type/domain fails closed");
        f.observe("##KN31070"); require(label->text().contains("Output: 70") && output->value() == 83, "current output recovers without overwriting staged draft");
    }
    for (const QVariant& revision : QList<QVariant>{QVariant(1), QVariant(true), QVariant("01"), QVariant("-1"), QVariant("18446744073709551616")}) {
        QVariantMap bad = current; bad["statusRevision"] = revision; emit f.backend->extensionStatus("kenwood", "audio.levels", bad);
        require(label->text().contains("Output: Unknown"), "invalid output revision cannot authorize confirmation");
        f.observe("##KN31070"); require(label->text().contains("Output: 70"), "actual current output push recovers invalid revision");
    }
    for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
        QVariantMap bad = current; bad["statusRevision"] = high; bad["writable"] = 1; emit f.backend->extensionStatus("kenwood", "audio.levels", bad);
        require(label->text().contains("Output: Unknown"), "malformed high output payload fails closed");
        require(actualOutputGetter(f) == 70, "actual no-argument getter retains current70 after malformed high payload");
        f.observe("##KN31070"); require(current.value("statusRevision").toString().toULongLong() < high.toULongLong()
            && label->text().contains("Output: 70") && output->value() == 83, "malformed canonical high output sequence cannot poison recovery or draft");
    }
    // Keep the established generic integral-number contract, including Double.
    for (const QVariant& value : QList<QVariant>{QVariant(0), QVariant(100), QVariant(70.0), QVariant::fromValue(70u), QVariant::fromValue(70LL)}) {
        QVariantMap valid = current; valid["outputLevel"] = value; emit f.backend->extensionStatus("kenwood", "audio.levels", valid);
        require(label->text().contains(QString("Output: %1").arg(value.toInt())), "valid integral output numeric types and endpoints preserved");
    }
    for (int read = 0; read < 20; ++read) { f.observe("##KN31070"); }
    const quint64 retiredRevision = current.value("statusRevision").toString().toULongLong();
    f.install(); f.observe("##KN31042");
    require(current.value("statusRevision").toString().toULongLong() < retiredRevision
        && label->text().contains("Output: 42") && output->value() == 42, "new backend binding accepts lower output producer floor and seeds its own draft");
    requireNoAudioLevelSetters(f); require(actualOutputGetter(f) == 42, "new binding actual output getter agrees");
}

// An injected extension endpoint, not a firmware or socket simulator.
class VfoBackend final : public IRadioBackend {
public:
    QString requestedVerb;
    QVariant requestedValue;
    int edits = 0;
    int powerQueries = 0;
    bool connected = true;
    QVariantMap state{{"statusRevision", QString("0")}, {"receive", "A"}, {"transmit", "A"}, {"split", false},
        {"frequencyAHz", 14074000}, {"frequencyBHz", 7074000}, {"txActive", false}};
    QVariantMap filterState{{"statusRevision", QString("0")}, {"mode", "AM"}, {"context", "session:mode"}, {"editable", true},
        {"axes", QVariantList{
            QVariantMap{{"id", "low"}, {"label", "Low cut"}, {"valuesHz", QVariantList{0, 100, 200, 300}}, {"index", 1}},
            QVariantMap{{"id", "high"}, {"label", "High cut"}, {"valuesHz", QVariantList{2000, 3000, 5000}}, {"index", 2}}}}};
    QVariantMap inputState{{"context", "input-session"}, {"editable", true}, {"send", 0}, {"data", 3}};
    QVariantMap levelState{{"statusRevision", QString("0")}, {"context", "levels-session"}, {"writable", true}, {"inputLevel", 50}, {"outputLevel", 60}};
    QVariantMap powerState{{"context", "power-session"}, {"editable", true}, {"enabled", false},
        {"limits", QVariantList{QVariantMap{{"type", 0}, {"band", 0}, {"watts", 100}},
            QVariantMap{{"type", 5}, {"band", 5}, {"watts", 10}}}}};
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        caps.tuningMinHz = 30000;
        caps.tuningMaxHz = 59999999;
        caps.extensionNamespaces = {QStringLiteral("injected-vfo")};
        caps.extensions.insert(QStringLiteral("injected-vfo"), QVariantMap{{"vfoControlVersion", 1}, {"nativeFilterControlVersion", 1},
            {"networkAudioLevelsVersion", 1}, {"powerLimitControlVersion", 1},
            {"transmitInputControlVersion", 1}, {"transmitInputChoices", QStringList{"Microphone", "ACC 2", "USB Audio", "LAN",
                "Microphone + ACC 2", "Microphone + USB Audio", "Microphone + LAN"}},
            {"powerLimitTypes", QStringList{"SSB", "CW", "FSK/PSK", "FM/AM", "DATA", "TX tuning"}},
            {"powerLimitBands", QStringList{"1.8 MHz", "3.5 MHz", "5 MHz", "7 MHz", "10 MHz", "14 MHz"}}});
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString& ns, const QString& verb, quint64 id, const QVariant& value) override
    {
        require(ns == "injected-vfo", "widget discovers namespace without a vendor-family branch");
        if (verb == "inputs.get") { emit extensionResult(id, inputState); return; }
        if (verb == "vfo.get") { emit extensionResult(id, state); return; }
        if (verb == "filter.get") { emit extensionResult(id, filterState); return; }
        if (verb == "audio.levels.get") { emit extensionResult(id, levelState); return; }
        if (verb == "power.limits.get") { emit extensionResult(id, powerState); return; }
        if (verb == "power.limits.query") { ++powerQueries; emit extensionResult(id, powerState); return; }
        requestedVerb = verb;
        requestedValue = value;
        ++edits;
        emit extensionResult(id, true);
    }
    void publish() { emit extensionStatus("injected-vfo", "vfo", state); }
    void publishFilter() { emit extensionStatus("injected-vfo", "filter", filterState); }
    void publishInputs() { emit extensionStatus("injected-vfo", "inputs", inputState); }
    void publishLevels() { emit extensionStatus("injected-vfo", "audio.levels", levelState); }
    void publishPower() { emit extensionStatus("injected-vfo", "power.limits", powerState); }
};
}

void relativeReadUi(ActualFixture& f, NativeVfoSettingsWidget& widget, int target)
{
    f.observe("FR" + QByteArray::number(target));
    widget.findChild<QPushButton*>("nativeRelativeRead")->click();
    for (const QByteArray& reply : QList<QByteArray>{"FR" + QByteArray::number(target), "FT" + QByteArray::number(1 - target), "TS0", "XV0", "MV0", "QR00",
        "FA00014074000", "FB00007074000", "SF0000140740003", "SF1000070740003", "FS00", "MH0", "LK0"}) { f.observe(reply); }
    require(widget.findChild<QComboBox*>("nativeRelativeMultiplier")->isEnabled(), "real native Read route qualifies step editors");
}
void nativeRelativeActualRoute()
{
    for (int target = 0; target < 2; ++target) {
        for (int direction = 0; direction < 2; ++direction) {
            for (int multiplier = 0; multiplier < 6; ++multiplier) {
                ActualFixture f; NativeVfoSettingsWidget widget(f.model); relativeReadUi(f, widget, target);
                auto* dir = widget.findChild<QComboBox*>("nativeRelativeFcDirection"); auto* mult = widget.findChild<QComboBox*>("nativeRelativeMultiplier");
                dir->setCurrentIndex(direction); emit dir->activated(direction); mult->setCurrentIndex(multiplier); emit mult->activated(multiplier);
                auto* apply = widget.findChild<QPushButton*>("nativeRelativeFcApply"); require(apply->isEnabled(), "native FC parameters require explicit Apply");
                const QString before = widget.findChild<QLabel*>("nativeVfoReadback")->text(); f.commands.clear(); QTest::mouseClick(apply, Qt::LeftButton);
                require(f.commands.contains("FC" + QByteArray::number(direction) + QByteArray::number(multiplier) + ';'), "all FC multipliers traverse real widget/model/backend route");
                require(widget.findChild<QLabel*>("nativeVfoReadback")->text() == before, "native action never predicts confirmed frequency");
            }
            for (int count : {-1, 0, 1, 99}) {
                ActualFixture f; NativeVfoSettingsWidget widget(f.model); relativeReadUi(f, widget, target);
                auto* dir = widget.findChild<QComboBox*>("nativeRelativeUdDirection"); dir->setCurrentIndex(direction); emit dir->activated(direction);
                if (count >= 0) {
                    QTest::mouseClick(widget.findChild<QCheckBox*>("nativeRelativeSingle"), Qt::LeftButton);
                    widget.findChild<QSpinBox*>("nativeRelativeSteps")->setValue(count);
                }
                auto* apply = widget.findChild<QPushButton*>("nativeRelativeUdApply"); require(apply->isEnabled(), "native UD parameters require explicit Apply");
                const QString qa = qEnvironmentVariable("AETHER_RELATIVE_QA_DIR");
                if (!qa.isEmpty() && target == 1 && direction == 0 && count == 99) {
                    widget.resize(850, 1050); widget.show(); QApplication::processEvents();
                    require(QDir().mkpath(qa) && widget.grab().save(qa + "/relative-ready.png"), "native relative panel screenshot saved");
                }
                f.commands.clear(); QTest::mouseClick(apply, Qt::LeftButton);
                const QByteArray suffix = count < 0 ? QByteArray{} : QByteArray::number(count).rightJustified(2, '0');
                require(f.commands.contains("UD" + QByteArray::number(target) + QByteArray::number(direction) + suffix + ';'), "UD literal form and receive target survive real UI route");
            }
        }
    }
}
void nativeRelativeTransverterRefusal()
{
    ActualFixture f; NativeVfoSettingsWidget widget(f.model); relativeReadUi(f, widget, 0);
    f.observe("XV1");
    require(!f.backend->isConnected(), "native transverter On retires the unsupported session");
    auto* fc = widget.findChild<QPushButton*>("nativeRelativeFcApply");
    auto* ud = widget.findChild<QPushButton*>("nativeRelativeUdApply");
    require(fc && ud && !fc->isEnabled() && !ud->isEnabled(),
        "retired transverter session leaves both native step actions visibly unavailable");
    f.commands.clear(); fc->click(); ud->click();
    require(f.commands.isEmpty(), "unavailable native step actions cannot write after XV retirement");
}

void nativeRelativeBindingReentry()
{
    const bool active = QAccessible::isActive();
    const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
    const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
    QAccessible::setActive(true);
    if (!QAccessible::isActive()) { std::cerr << "SKIP relative binding callback: inactive accessibility backend\n"; return; }
    ActualFixture f; NativeVfoSettingsWidget widget(f.model); relativeReadUi(f, widget, 0);
    auto* reason = widget.findChild<QLabel*>("nativeRelativeReason"); bool nested = false;
    const auto releaseCallback = qScopeGuard([] { accessibilityCallback = {}; });
    accessibilityCallback = [&](QAccessibleEvent* event) {
        if (!nested && event->type() == QAccessible::NameChanged && event->object() == reason && f.backend->isConnected()) {
            nested = true; f.install();
        }
    };
    f.install();
    require(nested, "relative binding callback actually replaces current backend during render");
    f.commands.clear(); widget.findChild<QPushButton*>("nativeRelativeRead")->click();
    require(f.commands.contains("FR;"), "new relative binding remains usable after nested backend replacement");
}

void nativeRelativeMalformedStatusRecovery()
{
    ActualFixture f; NativeVfoSettingsWidget widget(f.model); relativeReadUi(f, widget, 0);
    QVariantMap good = kenwood::KenwoodBackendTestAccess::relative(*f.backend); good["statusRevision"] = QStringLiteral("100");
    emit f.backend->extensionStatus("kenwood", "relativeTuning", good);
    QVariantMap bad = good; bad["statusRevision"] = QStringLiteral("99999999"); bad["editable"] = false;
    QVariantMap read = bad.value("read").toMap(); read["pending"] = QStringLiteral("BAD"); read["busy"] = true; bad["read"] = read;
    emit f.backend->extensionStatus("kenwood", "relativeTuning", bad);
    good["statusRevision"] = QStringLiteral("101"); emit f.backend->extensionStatus("kenwood", "relativeTuning", good);
    require(widget.findChild<QComboBox*>("nativeRelativeMultiplier")->isEnabled(), "malformed high native-read revision cannot poison valid status recovery");
    bad = good; bad["statusRevision"] = QStringLiteral("99999999"); bad["editable"] = false;
    read = bad.value("read").toMap(); read["pending"] = QStringLiteral("FR"); read["busy"] = true; read["total"] = 0; read["completed"] = 0; bad["read"] = read;
    emit f.backend->extensionStatus("kenwood", "relativeTuning", bad);
    good["statusRevision"] = QStringLiteral("102"); emit f.backend->extensionStatus("kenwood", "relativeTuning", good);
    require(widget.findChild<QComboBox*>("nativeRelativeMultiplier")->isEnabled(), "inconsistent read counts cannot poison producer revision floor");
    good["statusRevision"] = QStringLiteral("103"); good["editable"] = false; good["frequencyHz"] = QVariant::fromValue<qint64>(0);
    emit f.backend->extensionStatus("kenwood", "relativeTuning", good);
    require(widget.findChild<QLabel*>("nativeRelativeObserved")->text().contains("unknown"), "missing native frequency is unknown, never displayed as measured zero Hz");
}

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("native-vfo-widget"));
    require(settings.isValid(), "isolated test settings");
    QApplication application(argc, argv);
    AppSettings::instance().load();
    if (application.arguments().contains("--relative-only")) { nativeRelativeMalformedStatusRecovery(); nativeRelativeActualRoute(); nativeRelativeTransverterRefusal(); nativeRelativeBindingReentry(); return 0; }
    nativeRelativeMalformedStatusRecovery(); nativeRelativeActualRoute(); nativeRelativeTransverterRefusal(); nativeRelativeBindingReentry();
    if (application.arguments().contains("--audio-output-order-probe=push")) { audioOutputOlderPushCannotReplaceNewerObservation(); return 0; }
    if (application.arguments().contains("--audio-output-order-probe=get")) { audioOutputOlderGetCannotReplaceNewerObservation(); return 0; }
    if (application.arguments().contains("--status-order-probe=item-model")) { itemModelReentryCannotCorruptNewerAxesVocabulary(); return 0; }
    if (application.arguments().contains("--status-order-probe=draft")) { malformedVfoMetadataCannotOverwriteExistingDraft(); return 0; }
    if (application.arguments().contains("--status-order-probe=axes")) { axesPushCannotOverwriteNestedFmVocabulary(); return 0; }
    if (application.arguments().contains("--status-order-probe=tuning")) { tuningPendingCannotOutliveTfSetWithdrawal(); return 0; }
    if (application.arguments().contains("--status-order-probe=vfo")) { vfoConfirmedReadbackCannotRegressAfterNestedFrequency(); return 0; }
    RadioModel model;
    auto owned = std::make_unique<VfoBackend>();
    VfoBackend* source = owned.get();
    model.setBackendForTest(std::move(owned), QStringLiteral("injected"));
    NativeVfoSettingsWidget widget(model);
    widget.resize(620, 420);
    widget.show();
    QApplication::processEvents();
    QComboBox* receive = widget.findChild<QComboBox*>(QStringLiteral("nativeReceiveVfo"));
    QComboBox* transmit = widget.findChild<QComboBox*>(QStringLiteral("nativeTransmitVfo"));
    QDoubleSpinBox* frequency = widget.findChild<QDoubleSpinBox*>(QStringLiteral("nativeVfoBFrequency"));
    QPushButton* apply = widget.findChild<QPushButton*>(QStringLiteral("nativeVfoBApply"));
    QLabel* readback = widget.findChild<QLabel*>(QStringLiteral("nativeVfoReadback"));
    require(receive && transmit && frequency && apply && readback, "native VFO controls exist");
    require(receive->isEnabled() && frequency->value() == 7.074, "confirmed state seeds connected controls");
    transmit->setCurrentIndex(1);
    transmit->activated(1);
    require(source->requestedVerb == "vfo.transmit" && source->requestedValue == "B",
        "explicit TX VFO selection reaches native control extension");
    require(transmit->currentIndex() == 0 && readback->text().contains("Split: Off"),
        "successful local handoff does not invent confirmed split");
    source->state["transmit"] = "B";
    source->state["split"] = true;
    source->publish();
    require(transmit->currentIndex() == 1 && readback->text().contains("Split: On"),
        "radio status confirms split selection");
    frequency->setValue(14.2);
    require(source->edits == 1, "frequency entry alone cannot tune an inactive VFO");
    apply->click();
    require(source->requestedVerb == "vfo.frequency"
        && source->requestedValue.toMap().value("vfo") == "B"
        && source->requestedValue.toMap().value("frequencyHz").toLongLong() == 14200000,
        "Apply sends a complete integer-Hz request for the chosen VFO");
    require(readback->text().contains("7.074000"), "frequency display waits for readback");
    source->state["frequencyBHz"] = 14200000;
    source->publish();
    require(readback->text().contains("14.200000"), "live VFO readback adopts front-panel changes");
    const QString artifact = qEnvironmentVariable("AETHER_VFO_TEST_SCREENSHOT");
    if (!artifact.isEmpty()) {
        require(widget.grab().save(artifact), "save optional widget review image");
    }
    source->state["txActive"] = true;
    source->publish();
    require(!receive->isEnabled() && !apply->isEnabled()
        && !receive->accessibleDescription().isEmpty(), "TX dims controls with an accessible reason");
    source->state["txActive"] = false;
    source->publish();
    require(receive->isEnabled(), "receive readback restores controls");
    source->state.remove("receive");
    source->state.remove("transmit");
    source->state.remove("split");
    source->state["receiveSource"] = "memory";
    source->state["transmitSource"] = "memory";
    source->state["transmitSelectable"] = false;
    source->publish();
    require(receive->isEnabled() && !transmit->isEnabled()
        && !transmit->accessibleDescription().isEmpty() && readback->text().contains("Memory channel (unmapped)"),
        "unmapped memory source is explicit and permits receive-VFO recovery while FT stays dimmed");
    if (!artifact.isEmpty()) {
        QApplication::processEvents();
        require(widget.grab().save(artifact + ".memory.png"), "save unmapped-memory widget review image");
    }
    const int beforeMemoryEdit = source->edits;
    transmit->activated(1);
    require(source->edits == beforeMemoryEdit, "disabled memory FT selector cannot submit a programmatic activation");
    receive->activated(0);
    require(source->requestedVerb == "vfo.receive" && source->requestedValue == "A", "receive selector can explicitly leave memory operation");
    source->state = {{"statusRevision", QString("0")}, {"receive", "B"}, {"transmit", "B"}, {"split", false},
        {"frequencyAHz", 14074000}, {"frequencyBHz", 7074000}, {"txActive", false}};
    std::thread queuedVfo([source] { source->publish(); });
    queuedVfo.join();
    source->connected = false;
    emit model.connectionStateChanged(false);
    const int editsBeforeReconnect = source->edits;
    source->state = {{"statusRevision", QString("0")}, {"receive", "A"}, {"transmit", "A"}, {"split", false}, {"txActive", false}};
    source->connected = true;
    emit model.connectionStateChanged(true);
    require(!frequency->isEnabled() && !apply->isEnabled() && frequency->value() == frequency->minimum(),
        "reconnect discards old staged frequency and withholds Apply until current physical readback");
    QApplication::processEvents();
    require(!frequency->isEnabled() && !apply->isEnabled() && frequency->value() == frequency->minimum()
        && receive->currentIndex() == 0 && transmit->currentIndex() == 0,
        "queued old VFO status cannot seed or enable a pending same-object reconnect");
    source->state["frequencyAHz"] = 28074000;
    source->state["frequencyBHz"] = 21074000;
    source->publish();
    QApplication::processEvents();
    require(receive->currentIndex() == 0 && transmit->currentIndex() == 0
        && readback->text().contains("28.074000") && readback->text().contains("21.074000")
        && frequency->value() == 21.074 && frequency->isEnabled() && apply->isEnabled()
        && source->edits == editsBeforeReconnect,
        "queued old VFO status cannot overwrite readback or seed fields after same-object reconnect");
    auto replacement = std::make_unique<VfoBackend>();
    replacement->state["receive"] = "B";
    replacement->state["frequencyBHz"] = 21074000;
    model.setBackendForTest(std::move(replacement), QStringLiteral("replacement"));
    emit model.connectionStateChanged(true);
    require(receive->currentIndex() == 1 && frequency->value() == 21.074,
        "backend replacement retires old edits and seeds the new radio");
    RadioModel unavailable;
    NativeVfoSettingsWidget unsupported(unavailable);
    QComboBox* dimmed = unsupported.findChild<QComboBox*>(QStringLiteral("nativeReceiveVfo"));
    require(dimmed && !dimmed->isEnabled() && !dimmed->accessibleDescription().isEmpty(),
        "unsupported and disconnected controls remain visible with a reason");
    RadioModel filterModel;
    auto filterOwned = std::make_unique<VfoBackend>();
    VfoBackend* filterSource = filterOwned.get();
    filterModel.setBackendForTest(std::move(filterOwned), QStringLiteral("native-filter-test"));
    NativeFilterSettingsWidget filters(filterModel);
    filters.resize(620, 320);
    filters.show();
    QApplication::processEvents();
    QComboBox* low = filters.findChild<QComboBox*>(QStringLiteral("nativeFilterLow"));
    QComboBox* high = filters.findChild<QComboBox*>(QStringLiteral("nativeFilterHigh"));
    require(low && high && low->isEnabled() && high->isEnabled()
        && low->currentIndex() == 1 && high->currentText() == "5000 Hz",
        "native filter page discovers versioned axes and confirmed values");
    low->setCurrentIndex(3);
    low->activated(3);
    require(filterSource->requestedVerb == "filter.set"
        && filterSource->requestedValue.toMap() == QVariantMap{{"axis", "low"}, {"index", 3}, {"context", "session:mode"}}
        && low->currentIndex() == 1 && high->currentIndex() == 2,
        "native selection sends one context-bound setting and retains confirmed companions");
    QVariantList axes = filterSource->filterState.value("axes").toList();
    QVariantMap first = axes[0].toMap();
    first["index"] = 3;
    axes[0] = first;
    filterSource->filterState["axes"] = axes;
    filterSource->publishFilter();
    require(low->currentIndex() == 3 && filterSource->edits == 1,
        "filter readback updates the UI without issuing a second write");
    const QString filterArtifact = qEnvironmentVariable("AETHER_FILTER_TEST_SCREENSHOT");
    if (!filterArtifact.isEmpty()) { require(filters.grab().save(filterArtifact), "save optional native filter review image"); }
    filterSource->filterState["editable"] = false;
    filterSource->publishFilter();
    require(!low->isEnabled() && !high->isEnabled() && !low->accessibleDescription().isEmpty(),
        "noneditable native state dims controls with accessible reasons");
    filterSource->filterState = {{"statusRevision", QString("0")}, {"mode", "PSK"}, {"context", "session:next-mode"}, {"editable", true},
        {"axes", QVariantList{QVariantMap{{"id", "low"}, {"label", "Passband width"},
            {"valuesHz", QVariantList{50, 100, 3000}}, {"index", 2}}}}};
    filterSource->publishFilter();
    require(low->isEnabled() && low->accessibleName() == "Passband width" && !high->isEnabled()
        && !high->isHidden() && filterSource->edits == 1,
        "width-only mode changes labels and dims the missing companion without writing");
    filterSource->filterState["axes"] = QVariantList{QVariantMap{{"id", "low"}, {"label", "Invalid"},
        {"valuesHz", QVariantList{50, 100}}, {"index", 2}}};
    filterSource->publishFilter();
    require(!low->isEnabled() && !high->isEnabled(), "malformed native layout cannot enable a control");
    const auto rejectLayout = [&](const QVariantList& invalidAxes) {
        filterSource->filterState["axes"] = invalidAxes;
        filterSource->publishFilter();
        require(!low->isEnabled() && !high->isEnabled() && filterSource->edits == 1,
            "invalid native metadata cannot enable controls or generate a write");
    };
    QVariantList oversizedValues;
    for (int index = 0; index < 257; ++index) { oversizedValues.append(index); }
    rejectLayout({QVariantMap{{"id", "low"}, {"label", "Oversized"},
        {"valuesHz", oversizedValues}, {"index", 0}}});
    rejectLayout({QVariantMap{{"id", "low"}, {"label", "Nonintegral"},
        {"valuesHz", QVariantList{50.5}}, {"index", 0}}});
    rejectLayout({QVariantMap{{"id", "low"}, {"label", "Fractional index"},
        {"valuesHz", QVariantList{50, 100}}, {"index", 0.5}}});
    const QVariantMap duplicate{{"id", "low"}, {"label", "Duplicate"},
        {"valuesHz", QVariantList{50}}, {"index", 0}};
    rejectLayout({duplicate, duplicate});
    rejectLayout({QVariantMap{{"id", "arbitrary"}, {"label", "Unknown axis"},
        {"valuesHz", QVariantList{50}}, {"index", 0}}});
    auto filterReplacement = std::make_unique<VfoBackend>();
    filterReplacement->filterState["context"] = "new-session";
    filterModel.setBackendForTest(std::move(filterReplacement), QStringLiteral("filter-replacement"));
    emit filterModel.connectionStateChanged(true);
    require(low->isEnabled() && low->currentIndex() == 1, "backend replacement seeds a new filter session");
    NativeFilterSettingsWidget unsupportedFilters(unavailable);
    QComboBox* unsupportedLow = unsupportedFilters.findChild<QComboBox*>(QStringLiteral("nativeFilterLow"));
    require(unsupportedLow && !unsupportedLow->isEnabled() && !unsupportedLow->accessibleDescription().isEmpty(),
        "disconnected native filters retain visible unavailable controls");
    RadioModel audioModel;
    auto audioOwned = std::make_unique<VfoBackend>();
    VfoBackend* audioSource = audioOwned.get();
    audioModel.setBackendForTest(std::move(audioOwned), QStringLiteral("network-levels-test"));
    NetworkAudioLevelsWidget levels(audioModel);
    levels.resize(620, 380);
    levels.show();
    QApplication::processEvents();
    QSpinBox* input = levels.findChild<QSpinBox*>(QStringLiteral("networkAudioInputLevel"));
    QSpinBox* output = levels.findChild<QSpinBox*>(QStringLiteral("networkAudioOutputLevel"));
    QPushButton* inputApply = levels.findChild<QPushButton*>(QStringLiteral("networkAudioInputApply"));
    QPushButton* outputApply = levels.findChild<QPushButton*>(QStringLiteral("networkAudioOutputApply"));
    QLabel* levelReadback = levels.findChild<QLabel*>(QStringLiteral("networkAudioLevelReadback"));
    require(input && output && inputApply && outputApply && levelReadback
        && input->value() == 50 && output->value() == 60 && inputApply->isEnabled(),
        "network levels discover a versioned namespace and seed confirmed administrator settings");
    input->setValue(25);
    require(audioSource->edits == 0, "staging a requested level cannot write the radio");
    inputApply->click();
    require(audioSource->requestedVerb == "audio.levels.set"
        && audioSource->requestedValue.toMap() == QVariantMap{{"direction", "input"}, {"level", 25}, {"context", "levels-session"}}
        && levelReadback->text().contains("input: 50") && levelReadback->text().contains("Output: 60"),
        "input Apply sends one session-bound setting without inventing confirmed level changes");
    audioSource->levelState["inputLevel"] = 25;
    audioSource->publishLevels();
    require(levelReadback->text().contains("input: 25") && audioSource->edits == 1,
        "VoIP readback updates confirmation without another write");
    output->setValue(0);
    outputApply->click();
    require(audioSource->requestedValue.toMap().value("direction") == "output"
        && audioSource->requestedValue.toMap().value("level").toInt() == 0,
        "output Apply is independent and accepts the native zero endpoint");
    const QString levelArtifact = qEnvironmentVariable("AETHER_NETWORK_LEVEL_TEST_SCREENSHOT");
    if (!levelArtifact.isEmpty()) { require(levels.grab().save(levelArtifact), "save optional network-level review image"); }
    audioSource->levelState["writable"] = false;
    audioSource->publishLevels();
    inputApply->click();
    require(!input->isEnabled() && !outputApply->isEnabled() && !input->accessibleDescription().isEmpty()
        && levelReadback->text().contains("input: 25") && audioSource->edits == 2,
        "ordinary accounts can read levels while edits stay dimmed with a reason");
    audioSource->levelState["writable"] = true;
    audioSource->levelState["inputLevel"] = 101;
    audioSource->publishLevels();
    require(!inputApply->isEnabled() && !outputApply->isEnabled(), "out-of-range level metadata cannot enable controls");
    audioSource->levelState["inputLevel"] = 2.5;
    audioSource->publishLevels();
    require(!inputApply->isEnabled(), "fractional native level metadata is rejected");
    audioSource->levelState["inputLevel"] = true;
    audioSource->publishLevels();
    require(!inputApply->isEnabled(), "boolean values cannot masquerade as native gain");
    audioSource->levelState["inputLevel"] = QStringLiteral("30");
    audioSource->publishLevels();
    require(!inputApply->isEnabled(), "text values cannot masquerade as native gain");
    audioSource->levelState["inputLevel"] = 25;
    audioSource->publishLevels();
    std::thread queuedPublisher([audioSource] { audioSource->publishLevels(); });
    queuedPublisher.join();
    audioSource->connected = false;
    emit audioModel.connectionStateChanged(false);
    audioSource->levelState = {{"statusRevision", QString("0")}, {"context", "same-backend-new-session"}, {"writable", true}, {"inputLevel", 11}, {"outputLevel", 22}};
    audioSource->connected = true;
    emit audioModel.connectionStateChanged(true);
    QApplication::processEvents();
    require(levelReadback->text().contains("input: 11") && levelReadback->text().contains("Output: 22"),
        "queued status from an old binding cannot overwrite a same-object reconnect");
    auto audioReplacement = std::make_unique<VfoBackend>();
    audioReplacement->levelState = {{"statusRevision", QString("0")}, {"context", "new-level-session"}, {"writable", true}, {"inputLevel", 10}, {"outputLevel", 20}};
    audioModel.setBackendForTest(std::move(audioReplacement), QStringLiteral("audio-replacement"));
    emit audioModel.connectionStateChanged(true);
    require(input->value() == 10 && output->value() == 20 && inputApply->isEnabled(),
        "new backend retires staged levels and seeds its own native settings");
    NetworkAudioLevelsWidget unsupportedLevels(unavailable);
    QSpinBox* unsupportedInput = unsupportedLevels.findChild<QSpinBox*>(QStringLiteral("networkAudioInputLevel"));
    require(unsupportedInput && !unsupportedInput->isEnabled() && !unsupportedInput->accessibleDescription().isEmpty(),
        "unsupported network-level controls remain visible with a reason");
    RadioModel powerModel;
    auto powerOwned = std::make_unique<VfoBackend>();
    VfoBackend* powerSource = powerOwned.get();
    powerModel.setBackendForTest(std::move(powerOwned), QStringLiteral("power-limits-test"));
    NativePowerLimitsWidget limits(powerModel);
    limits.resize(640, 430);
    limits.show();
    QApplication::processEvents();
    QComboBox* limitType = limits.findChild<QComboBox*>(QStringLiteral("nativePowerLimitType"));
    QComboBox* limitBand = limits.findChild<QComboBox*>(QStringLiteral("nativePowerLimitBand"));
    QSpinBox* limitWatts = limits.findChild<QSpinBox*>(QStringLiteral("nativePowerLimitWatts"));
    QPushButton* limitApply = limits.findChild<QPushButton*>(QStringLiteral("nativePowerLimitApply"));
    QPushButton* limitRead = limits.findChild<QPushButton*>(QStringLiteral("nativePowerLimitRead"));
    QCheckBox* limiterEnabled = limits.findChild<QCheckBox*>(QStringLiteral("nativePowerLimiterEnabled"));
    QLabel* limitReadback = limits.findChild<QLabel*>(QStringLiteral("nativePowerLimitReadback"));
    require(limitType && limitBand && limitWatts && limitApply && limitRead && limiterEnabled && limitReadback
        && powerSource->powerQueries == 1 && powerSource->edits == 0 && limitWatts->value() == 100,
        "native power page discovers metadata and queries without restoring settings");
    limitType->setCurrentIndex(5);
    limitType->activated(5);
    limitBand->setCurrentIndex(5);
    limitBand->activated(5);
    require(powerSource->powerQueries == 3 && powerSource->edits == 0 && limitWatts->value() == 10
        && limitReadback->text().contains("10 W"), "band and type selection read the independent tuning row without changing RF controls");
    limitWatts->setValue(20);
    require(powerSource->edits == 0, "entering a requested limit alone does not write");
    limitApply->click();
    require(powerSource->requestedVerb == "power.limits.set"
        && powerSource->requestedValue.toMap() == QVariantMap{{"type", 5}, {"band", 5}, {"watts", 20}, {"context", "power-session"}}
        && limitReadback->text().contains("10 W") && !limiterEnabled->isChecked(),
        "Apply isolates one row and does not invent readback or enable the global limiter");
    powerSource->powerState["limits"] = QVariantList{QVariantMap{{"type", 0}, {"band", 0}, {"watts", 100}},
        QVariantMap{{"type", 5}, {"band", 5}, {"watts", 20}}};
    powerSource->publishPower();
    require(limitReadback->text().contains("20 W") && powerSource->edits == 1, "native power readback cannot echo a second write");
    limiterEnabled->click();
    require(powerSource->requestedVerb == "power.limiter.set"
        && powerSource->requestedValue.toMap() == QVariantMap{{"enabled", true}, {"context", "power-session"}}
        && !limiterEnabled->isChecked(), "global limiter toggle waits for its independent radio echo");
    powerSource->powerState["enabled"] = true;
    powerSource->publishPower();
    require(limiterEnabled->isChecked() && powerSource->edits == 2, "confirmed limiter state updates without a configuration loop");
    const QString powerArtifact = qEnvironmentVariable("AETHER_POWER_LIMIT_TEST_SCREENSHOT");
    if (!powerArtifact.isEmpty()) { require(limits.grab().save(powerArtifact), "save optional power-limit review image"); }
    powerSource->powerState["editable"] = false;
    powerSource->publishPower();
    const int queriesBeforeTx = powerSource->powerQueries;
    limitRead->click();
    limitApply->click();
    require(!limitWatts->isEnabled() && !limiterEnabled->isEnabled() && !limitApply->accessibleDescription().isEmpty()
        && limitRead->isEnabled() && powerSource->powerQueries == queriesBeforeTx + 1 && powerSource->edits == 2,
        "unconfirmed receive operation dims writes while preserving explicit reads");
    powerSource->powerState["editable"] = true;
    powerSource->powerState["limits"] = QVariantList{QVariantMap{{"type", 5}, {"band", 5}, {"watts", 999}}};
    powerSource->publishPower();
    require(!limitApply->isEnabled() && !limiterEnabled->isEnabled(), "malformed power metadata cannot enable native writes");
    limitRead->click();
    require(!limitApply->isEnabled() && powerSource->edits == 2, "rereading malformed metadata cannot recurse or generate a write");
    powerSource->powerState["limits"] = QVariantList{QVariantMap{{"type", 0}, {"band", 0}, {"watts", 100}},
        QVariantMap{{"type", 5}, {"band", 5}, {"watts", 20}}};
    powerSource->publishPower();
    std::thread queuedPower([powerSource] { powerSource->publishPower(); });
    queuedPower.join();
    powerSource->connected = false;
    emit powerModel.connectionStateChanged(false);
    powerSource->powerState = {{"context", "new-power-session"}, {"editable", true}, {"enabled", false},
        {"limits", QVariantList{QVariantMap{{"type", 0}, {"band", 0}, {"watts", 30}}}}};
    powerSource->connected = true;
    emit powerModel.connectionStateChanged(true);
    QApplication::processEvents();
    require(limitReadback->text().contains("30 W") && limitWatts->value() == 30 && !limiterEnabled->isChecked(),
        "old queued power status cannot overwrite a reconnected same-object binding");
    NativePowerLimitsWidget unsupportedPower(unavailable);
    QSpinBox* unsupportedWatts = unsupportedPower.findChild<QSpinBox*>(QStringLiteral("nativePowerLimitWatts"));
    require(unsupportedWatts && !unsupportedWatts->isEnabled() && !unsupportedWatts->accessibleDescription().isEmpty(),
        "unsupported native power configuration remains visible with a reason");
    RadioModel inputModel;
    auto inputOwned = std::make_unique<VfoBackend>();
    VfoBackend* inputSource = inputOwned.get();
    inputModel.setBackendForTest(std::move(inputOwned), QStringLiteral("injected"));
    NativeTransmitInputsWidget inputs(inputModel);
    inputs.resize(650, 340);
    inputs.show();
    QApplication::processEvents();
    QComboBox* sendInput = inputs.findChild<QComboBox*>(QStringLiteral("nativeSendInput"));
    QComboBox* dataInput = inputs.findChild<QComboBox*>(QStringLiteral("nativeDataInput"));
    QLabel* inputReadback = inputs.findChild<QLabel*>(QStringLiteral("nativeTransmitInputReadback"));
    require(sendInput && dataInput && inputReadback && sendInput->isEnabled() && dataInput->isEnabled()
        && sendInput->count() == 7 && sendInput->currentIndex() == 0 && dataInput->currentIndex() == 3,
        "native input page consumes independent confirmed selections and advertised choices");
    sendInput->setCurrentIndex(6);
    emit sendInput->activated(6);
    require(inputSource->requestedVerb == "inputs.set"
        && inputSource->requestedValue.toMap() == QVariantMap{{"path", "send"}, {"selection", 6}, {"context", "input-session"}}
        && sendInput->currentIndex() == 0 && dataInput->currentIndex() == 3,
        "native input choice emits a full explicit pair while painting confirmed state");
    inputSource->inputState.remove("send");
    inputSource->publishInputs();
    emit sendInput->activated(1);
    require(!sendInput->isEnabled() && sendInput->currentIndex() == -1 && dataInput->isEnabled()
        && inputSource->edits == 1 && !sendInput->accessibleDescription().isEmpty(),
        "pending or unknown path stays visible and dimmed without affecting its companion");
    inputSource->inputState["send"] = 6;
    inputSource->publishInputs();
    require(sendInput->isEnabled() && sendInput->currentIndex() == 6 && inputSource->edits == 1
        && inputReadback->text().contains("Microphone + LAN"), "radio input readback updates without a second write");
    const QString inputArtifact = qEnvironmentVariable("AETHER_INPUT_TEST_SCREENSHOT");
    if (!inputArtifact.isEmpty()) { require(inputs.grab().save(inputArtifact), "save native input review image"); }
    inputSource->inputState["editable"] = false;
    inputSource->publishInputs();
    emit dataInput->activated(0);
    require(!sendInput->isEnabled() && !dataInput->isEnabled() && inputSource->edits == 1,
        "reported transmission or local intent prevents even programmatic activation");
    inputSource->inputState["editable"] = true;
    inputSource->inputState["data"] = true;
    inputSource->publishInputs();
    require(!sendInput->isEnabled() && !dataInput->isEnabled(), "malformed input metadata cannot enable controls");
    inputSource->inputState["data"] = 3;
    inputSource->publishInputs();
    std::thread queuedInputs([inputSource] { inputSource->publishInputs(); });
    queuedInputs.join();
    inputSource->connected = false;
    emit inputModel.connectionStateChanged(false);
    inputSource->inputState = {{"context", "new-input-session"}, {"editable", true}, {"data", 0}};
    inputSource->connected = true;
    emit inputModel.connectionStateChanged(true);
    QApplication::processEvents();
    require(sendInput->currentIndex() == -1 && !sendInput->isEnabled() && dataInput->currentIndex() == 0
        && inputSource->edits == 1, "old queued input status cannot restore selections after reconnect");
    emit dataInput->activated(2);
    require(inputSource->requestedValue.toMap().value("context") == "new-input-session", "input requests retain their current session context");
    NativeTransmitInputsWidget unsupportedInputs(unavailable);
    QComboBox* unsupportedSend = unsupportedInputs.findChild<QComboBox*>(QStringLiteral("nativeSendInput"));
    require(unsupportedSend && !unsupportedSend->isEnabled() && !unsupportedSend->accessibleDescription().isEmpty(),
        "unsupported native inputs remain visible with an accessible reason");
    axesPushCannotOverwriteNestedFmVocabulary(); tuningPendingCannotOutliveTfSetWithdrawal(); vfoConfirmedReadbackCannotRegressAfterNestedFrequency();
    axesRevisionValidationGetAndReentry(); newBindingResetsFourConsumerFloors();
    itemModelReentryCannotCorruptNewerAxesVocabulary(); malformedVfoMetadataCannotOverwriteExistingDraft();
    actualVfoApplyUsesLocaleAwareUserInput(); vfoAndTuningMalformedOrderingRecovery(); newerVfoGetAndAccessibilityReentry();
    audioOutputOlderPushCannotReplaceNewerObservation(); audioOutputOlderGetCannotReplaceNewerObservation(); audioOutputMetadataOrderingAndDraftRecovery();
    return 0;
}
