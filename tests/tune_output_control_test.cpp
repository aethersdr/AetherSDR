// Injected command/reply sinks and recorded status: no radio, peer or RF.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AppSettings.h"
#include "core/backends/TuneOutputPolicy.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QStringList>
#include <cstdio>
#include <memory>
#include <vector>

namespace AetherSDR {
class TuneOutputControlTestAccess {
public:
    static void identity(RadioModel& radio, const QString& version)
    {
        radio.m_version = version;
        radio.m_lastInfo.family = QStringLiteral("flex");
    }
    static void injectWire(RadioModel& radio, RadioConnection& connection, QStringList& wire)
    {
        connection.m_commandSinkForTest = [&wire](quint32, const QString& command) { wire << command; };
        radio.m_connection = &connection;
        radio.m_flexBackend = dynamic_cast<FlexBackend*>(radio.m_backend.get());
    }
    static void apply(RadioModel& radio) { radio.applyTuneInhibit(); }
    static void restore(RadioModel& radio) { radio.restoreTuneInhibit(); }
    static void disconnect(RadioModel& radio) { radio.onDisconnected(); }
    static void expire(RadioModel& radio, int bandId)
    {
        radio.expireTuneOutputRequest(bandId, radio.m_tuneOutputBands.value(bandId).requestId,
                                     radio.m_tuneOutputGeneration);
    }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    failures += !passed;
}
class ConnectedFlex final : public FlexBackend {
public:
    bool connected = true;
    bool isConnected() const override { return connected; }
};
struct Fixture {
    QString version = QStringLiteral("4.3.16.42034");
    QStringList wire;
    RadioConnection inert;
    RadioModel radio;
    ConnectedFlex* backend = nullptr;
    std::vector<FlexBackend::CommandReply> replies;
    Fixture()
    {
        auto owned = std::make_unique<ConnectedFlex>();
        backend = owned.get();
        backend->connected = false;
        backend->setFirmwareVersionProvider([this] { return version; });
        backend->setCommandSink([this](const QString& command, FlexBackend::CommandReply reply) {
            wire << command;
            if (reply) { replies.push_back(std::move(reply)); }
            return true;
        });
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        TuneOutputControlTestAccess::identity(radio, version);
        TuneOutputControlTestAccess::injectWire(radio, inert, wire);
        check(radio.automationApplySliceFixture(0, QStringLiteral("A")), "install injected TX slice");
        SliceDelta delta;
        delta.txSlice = true;
        delta.frequency = 14.1;
        radio.slice(0)->applyChanges(delta);
        backend->connected = true;
        band(9, QStringLiteral("20"), QStringLiteral("0"));
        band(5, QStringLiteral("40"), QStringLiteral("1"));
        wire.clear();
    }
    void band(int id, const QString& name, const QString& value)
    {
        const QString object = QStringLiteral("interlock band %1").arg(id);
        const QMap<QString, QString> fields{{QStringLiteral("band_name"), name},
            {QStringLiteral("tune_tx_suppress"), value}, {QStringLiteral("acc_tx_enabled"), QStringLiteral("1")},
            {QStringLiteral("tx1_enabled"), QStringLiteral("1")}, {QStringLiteral("tx2_enabled"), QStringLiteral("1")},
            {QStringLiteral("tx3_enabled"), QStringLiteral("1")}};
        radio.handleStatusForTest(object, fields);
    }
    void general(bool suppressed)
    {
        radio.handleStatusForTest(QStringLiteral("interlock"),
            {{QStringLiteral("tune_tx_suppress"), suppressed ? QStringLiteral("1") : QStringLiteral("0")}});
    }
    void frequency(double mhz)
    {
        SliceDelta delta;
        delta.frequency = mhz;
        radio.slice(0)->applyChanges(delta);
    }
};

void policyAndParser()
{
    for (const QString& version : {QStringLiteral("4.3.0"), QStringLiteral("4.3.16.42034"),
                                  QStringLiteral("4.10.0"), QStringLiteral("5.0.0")}) {
        check(TuneOutputPolicy::firmwareSupportsSuppression(version)
            && !TuneOutputPolicy::clientManagesRca(version), "complete 4.3+ version delegates RCA to radio");
    }
    for (const QString& version : {QString(), QStringLiteral("4.3"), QStringLiteral("4.3.0beta"),
            QStringLiteral("4.3.0.1.2"), QStringLiteral("-4.3.0"), QStringLiteral(" 4.3.0"),
            QStringLiteral("4.3.0 "), QStringLiteral("4.3..0"), QStringLiteral("4.3.-1"),
            QStringLiteral("4.3.2147483648"), QStringLiteral("999999999999999999.0.0"),
            QStringLiteral("4.2.20.41343"), QStringLiteral("3.10.0")}) {
        check(!TuneOutputPolicy::firmwareSupportsSuppression(version)
            && TuneOutputPolicy::clientManagesRca(version), "unknown, malformed and old versions preserve legacy ownership");
    }
    Fixture f;
    int observations = 0;
    QObject::connect(f.backend, &IRadioBackend::tuneOutputSuppressionObserved, &f.radio,
                     [&observations](int, bool) { ++observations; });
    for (const QString& token : {QString(), QStringLiteral("true"), QStringLiteral("01"),
                                QStringLiteral("-1"), QStringLiteral("2"), QStringLiteral("1 ")}) {
        f.backend->decodeTuneOutputStatus(QStringLiteral("interlock band 9"),
                                         {{QStringLiteral("tune_tx_suppress"), token}});
    }
    for (const QString& object : {QStringLiteral("interlock"), QStringLiteral("interlock band -1"),
            QStringLiteral("interlock band 256"), QStringLiteral("interlock band 99999999999999"),
            QStringLiteral("interlock band 9 suffix")}) {
        f.backend->decodeTuneOutputStatus(object, {{QStringLiteral("tune_tx_suppress"), QStringLiteral("1")}});
    }
    check(observations == 0 && f.wire.isEmpty(), "malformed and unscoped status has no observation or write");
    f.backend->decodeTuneOutputStatus(QStringLiteral("interlock band 26"),
                                     {{QStringLiteral("tune_tx_suppress"), QStringLiteral("1")}});
    check(observations == 1, "noncontiguous reported IDs above fifteen are decoded");
    check(!f.radio.tuneOutputControl(26).haveValue, "unnamed observation alone cannot make a row editable");
    f.backend->requestTuneOutputSuppression(9, true, 0);
    f.backend->requestTuneOutputSuppression(-1, true, 1);
    f.backend->requestTuneOutputSuppression(256, true, 2);
    f.backend->requestTuneOutputSuppression(17, true, 3);
    f.version = QStringLiteral("4.3");
    f.backend->requestTuneOutputSuppression(9, true, 4);
    check(f.wire.isEmpty(), "invalid IDs, unreported rows, zero request and unqualified firmware never write");
    for (int id : {-1, 17, 256}) {
        f.radio.requestTuneOutputSuppression(id, true);
    }
    check(f.wire.isEmpty(), "model refuses invalid, unknown and unsupported row requests");
}

void authorityAndReplies()
{
    Fixture f;
    test::SeamThreadAffinityProbe probe(f.backend);
    test::attachAllSeamSignals(probe);
    TuneOutputControlState twenty = f.radio.tuneOutputControl(9);
    TuneOutputControlState forty = f.radio.tuneOutputControl(5);
    check(twenty.supported && twenty.haveValue && !twenty.suppressed && !twenty.pending
        && forty.haveValue && forty.suppressed && !forty.pending, "rows start with their independent radio observations");
    check(f.wire.isEmpty(), "construction, status and row state queries never write");
    f.radio.requestTuneOutputSuppression(9, true);
    f.radio.requestTuneOutputSuppression(5, false);
    check(f.wire == QStringList{QStringLiteral("interlock bandset 9 tune_tx_suppress=1"),
                               QStringLiteral("interlock bandset 5 tune_tx_suppress=0")},
          "two concurrent row edits address exactly their reported IDs and no other keys");
    check(f.radio.tuneOutputControl(9).pending && !f.radio.tuneOutputControl(9).suppressed
        && f.radio.tuneOutputControl(5).pending && f.radio.tuneOutputControl(5).suppressed,
          "each pending row retains its last confirmed state independently");
    f.replies[0](0, QString());
    check(f.wire.last() == QStringLiteral("sub tx all") && f.radio.tuneOutputControl(9).pending
        && f.radio.tuneOutputControl(5).pending, "ACK requests readback without confirming either pending row");
    f.band(5, QStringLiteral("40"), QStringLiteral("0"));
    check(!f.radio.tuneOutputControl(5).pending && !f.radio.tuneOutputControl(5).suppressed
        && f.radio.tuneOutputControl(9).pending, "one row's echo before ACK cannot complete another row");
    f.replies[1](0, QString());
    f.band(9, QStringLiteral("20"), QStringLiteral("0"));
    check(f.radio.tuneOutputControl(9).pending, "mismatching readback remains pending");
    f.band(9, QStringLiteral("20"), QStringLiteral("1"));
    check(f.radio.tuneOutputControl(9).suppressed && !f.radio.tuneOutputControl(9).pending,
          "matching per-band readback confirms only the addressed row");
    f.radio.requestTuneOutputSuppression(9, false);
    f.replies[2](0x5000002d, QStringLiteral("unknown field"));
    check(!f.radio.tuneOutputControl(9).pending && f.radio.tuneOutputControl(9).suppressed
        && !f.radio.tuneOutputControl(9).reason.isEmpty() && f.radio.tuneOutputControl(5).reason.isEmpty(),
          "ACK failure is visible only on its row while confirmed values survive");
    f.radio.requestTuneOutputSuppression(9, false);
    f.band(9, QStringLiteral("20"), QStringLiteral("0"));
    const qsizetype before = f.wire.size();
    f.replies[3](0, QString());
    check(!f.radio.tuneOutputControl(9).pending && !f.radio.tuneOutputControl(9).suppressed
        && f.wire.size() == before + 1, "echo-before-ACK stays confirmed and late success only requests readback");
    f.radio.requestTuneOutputSuppression(5, true);
    f.replies[4](0, QString());
    f.band(5, QStringLiteral("40"), QStringLiteral("0"));
    check(f.radio.tuneOutputControl(5).pending, "successful no-effect ACK and unchanged row readback cannot confirm");
    TuneOutputControlTestAccess::expire(f.radio, 5);
    check(!f.radio.tuneOutputControl(5).pending && !f.radio.tuneOutputControl(5).suppressed
        && !f.radio.tuneOutputControl(5).reason.isEmpty(), "missing row readback times out without guessing state");
    f.radio.requestTuneOutputSuppression(5, true);
    const qsizetype latest = f.wire.size();
    f.replies[4](0, QString());
    check(f.wire.size() == latest && f.radio.tuneOutputControl(5).pending,
          "old callback cannot complete or re-read over a newer request for the same row");
    f.radio.requestTuneOutputSuppression(9, true);
    f.backend->connected = false;
    TuneOutputControlTestAccess::disconnect(f.radio);
    f.wire.clear();
    f.replies[5](0, QString());
    f.replies[6](0, QString());
    check(f.wire.isEmpty() && !f.radio.tuneOutputControl(5).haveValue && !f.radio.tuneOutputControl(5).pending
        && !f.radio.tuneOutputControl(9).haveValue && !f.radio.tuneOutputControl(9).pending,
          "disconnect invalidates all row values and late completions");
    f.backend->connected = true;
    TuneOutputControlTestAccess::identity(f.radio, f.version);
    check(!f.radio.tuneOutputControl(5).haveValue && !f.radio.tuneOutputControl(9).haveValue,
          "cached band names alone do not restore stale suppression values after reconnect");
    f.radio.requestTuneOutputSuppression(9, true);
    check(f.wire.isEmpty(), "reconnect cannot write until a fresh per-row suppression observation arrives");
    f.band(9, QStringLiteral("20"), QStringLiteral("0"));
    f.radio.requestTuneOutputSuppression(9, true);
    check(probe.violations().isEmpty() && probe.afterDisconnect().isEmpty()
        && probe.count(QStringLiteral("tuneOutputSuppressionObserved")) > 0
        && probe.count(QStringLiteral("tuneOutputSuppressionCommandFinished")) > 0,
          "suppression observations and completions honor seam thread affinity");
    const FlexBackend::CommandReply old = f.replies.back();
    auto replacement = std::make_unique<ConnectedFlex>();
    replacement->setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
    f.radio.setBackendForTest(std::move(replacement), QStringLiteral("flex"));
    f.wire.clear();
    old(0, QString());
    check(f.wire.isEmpty() && !f.radio.tuneOutputControl(9).haveValue,
          "backend replacement invalidates row state and destroyed-backend callbacks");
}

void explicitRowsIgnoreTxSelection()
{
    Fixture f;
    // These exact noncontiguous IDs/names were reported by FLEX-6700 4.3.16.
    struct Row { int id; const char* name; };
    for (const Row& row : {Row{22, "2"}, Row{21, "4"}, Row{25, "2200"},
                           Row{26, "630"}, Row{24, "GEN"}}) {
        f.band(row.id, QLatin1String(row.name), QStringLiteral("0"));
    }
    f.general(true);
    check(!f.radio.tuneOutputControl(9).suppressed && !f.radio.tuneOutputControl(26).suppressed,
          "general active-band status never overwrites explicit row observations");
    for (double mhz : {144.2, 70.2, 0.1375, 0.475, 100.0}) {
        f.frequency(mhz);
        f.band(26, QStringLiteral("630"), QStringLiteral("0"));
        f.wire.clear();
        f.radio.requestTuneOutputSuppression(26, true);
        check(f.wire == QStringList{QStringLiteral("interlock bandset 26 tune_tx_suppress=1")},
              "a row edit always targets its reported ID regardless of TX frequency, with no GEN fallback");
        f.band(26, QStringLiteral("630"), QStringLiteral("1"));
        f.replies.back()(0, QString());
    }
    SliceDelta delta;
    delta.txSlice = false;
    f.radio.slice(0)->applyChanges(delta);
    f.wire.clear();
    f.radio.requestTuneOutputSuppression(26, false);
    check(f.wire == QStringList{QStringLiteral("interlock bandset 26 tune_tx_suppress=0")},
          "reported row configuration does not require or choose a TX slice");
    f.radio.handleStatusForTest(QStringLiteral("interlock band 17"),
                               {{QStringLiteral("band_name"), QStringLiteral("unconfirmed")}});
    f.wire.clear();
    f.radio.requestTuneOutputSuppression(17, true);
    check(!f.radio.tuneOutputControl(17).haveValue && !f.radio.tuneOutputControl(17).reason.isEmpty()
        && f.wire.isEmpty(), "named row without suppression readback stays unavailable");
}

void genericSinkCompatibility()
{
    QStringList wire;
    RadioConnection inert;
    RadioModel radio;
    TuneOutputControlTestAccess::injectWire(radio, inert, wire);
    check(!radio.isConnected(), "generic command fixture is disconnected");
    check(radio.backend()->requestRadioNickname(QStringLiteral("Test")) == ReceiveDispatch::Dispatched,
          "existing typed generic control dispatch is retained");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(wire == QStringList{QStringLiteral("radio name Test")},
          "reply-capable generic sink preserves existing disconnected command forwarding");
}

void droppedDispatch()
{
    Fixture f;
    f.backend->setCommandSink([](const QString&, FlexBackend::CommandReply) { return false; });
    f.radio.requestTuneOutputSuppression(9, true);
    const TuneOutputControlState state = f.radio.tuneOutputControl(9);
    check(!state.pending && state.haveValue && !state.suppressed && !state.reason.isEmpty()
        && f.wire.isEmpty(), "a dropped command preserves readback and immediately retires pending state");
}

void firmwareOwnedPreferenceChanges()
{
    Fixture f;
    AppSettings& settings = AppSettings::instance();
    for (const char* key : {"TuneInhibitAccTx", "TuneInhibitTx1", "TuneInhibitTx2", "TuneInhibitTx3"}) {
        settings.setValue(QLatin1String(key), QStringLiteral("True"));
    }
    TuneOutputControlTestAccess::apply(f.radio);
    check(f.wire == QStringList{QStringLiteral("interlock bandset 9 acc_tx_enabled=0")},
          "firmware-owned RCA leaves only selected ACC in the ownership snapshot");
    settings.setValue(QStringLiteral("TuneInhibitAccTx"), QStringLiteral("False"));
    f.version = QStringLiteral("4.2.20.41343");
    TuneOutputControlTestAccess::identity(f.radio, f.version);
    TuneOutputControlTestAccess::restore(f.radio);
    check(f.wire == QStringList{QStringLiteral("interlock bandset 9 acc_tx_enabled=0"),
                               QStringLiteral("interlock bandset 9 acc_tx_enabled=1")},
          "preference and firmware changes restore captured ACC without acquiring RCA ownership");
}

void readbackBeforeFirmwareIdentity()
{
    Fixture f;
    f.backend->connected = false;
    TuneOutputControlTestAccess::disconnect(f.radio);
    f.version = QStringLiteral("1.4.0.0");
    TuneOutputControlTestAccess::identity(f.radio, f.version);
    f.backend->connected = true;
    f.wire.clear();
    // Reconnect and manual-IP sessions subscribe before info supplies software_ver.
    f.band(5, QStringLiteral("40"), QStringLiteral("1"));
    f.band(9, QStringLiteral("20"), QStringLiteral("0"));
    f.radio.requestTuneOutputSuppression(9, true);
    check(!f.radio.tuneOutputControl(5).supported && !f.radio.tuneOutputControl(5).haveValue
              && f.wire.isEmpty(), "early readback cannot enable controls or writes before firmware qualification");
    f.version = QStringLiteral("4.3.16.42034");
    TuneOutputControlTestAccess::identity(f.radio, f.version);
    emit f.radio.infoChanged();
    check(f.radio.tuneOutputControl(5).haveValue && f.radio.tuneOutputControl(5).suppressed
              && f.radio.tuneOutputControl(9).haveValue && !f.radio.tuneOutputControl(9).suppressed
              && f.wire.isEmpty(), "firmware identity unlocks earlier per-band readback without another radio event or write");
    f.radio.requestTuneOutputSuppression(9, true);
    check(f.wire == QStringList{QStringLiteral("interlock bandset 9 tune_tx_suppress=1")},
          "the qualified row accepts an explicit request after status-first reconnect");
    f.backend->connected = false;
    TuneOutputControlTestAccess::disconnect(f.radio);
    f.version = QStringLiteral("4.3.16.42034");
    TuneOutputControlTestAccess::identity(f.radio, f.version);
    f.backend->connected = true;
    check(!f.radio.tuneOutputControl(5).haveValue && !f.radio.tuneOutputControl(9).haveValue,
          "a later session never inherits early readback from the previous connection");
}

void legacyOwnership()
{
    AppSettings& settings = AppSettings::instance();
    for (const char* key : {"TuneInhibitAccTx", "TuneInhibitTx1", "TuneInhibitTx2", "TuneInhibitTx3"}) {
        settings.setValue(QLatin1String(key), QStringLiteral("True"));
    }
    settings.save();
    Fixture f;
    for (const QString& version : {QStringLiteral("4.2.20.41343"), QStringLiteral("3.10.0"), QString(),
                                  QStringLiteral("4.3"), QStringLiteral("4.3.0"), QStringLiteral("4.3.16.42034")}) {
        f.version = version;
        TuneOutputControlTestAccess::identity(f.radio, version);
        f.band(9, QStringLiteral("20"), QStringLiteral("0"));
        f.general(false);
        f.wire.clear();
        TuneOutputControlTestAccess::apply(f.radio);
        TuneOutputControlTestAccess::restore(f.radio);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QStringList expected;
        for (int value : {0, 1}) {
            expected << QStringLiteral("interlock bandset 9 acc_tx_enabled=%1").arg(value);
            if (TuneOutputPolicy::clientManagesRca(version)) {
                for (int output : {1, 2, 3}) {
                    expected << QStringLiteral("interlock bandset 9 tx%1_enabled=%2").arg(output).arg(value);
                }
            }
        }
        check(f.wire == expected, "production apply AND restore preserve ACC and legacy RCA exactly while newer firmware owns RCA");
        f.band(9, QStringLiteral("20"), QStringLiteral("1"));
        f.general(true);
        f.wire.clear();
        TuneOutputControlTestAccess::apply(f.radio);
        TuneOutputControlTestAccess::restore(f.radio);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(f.wire == expected, "radio property true or false never changes firmware ownership routing");
        for (const char* key : {"TuneInhibitAccTx", "TuneInhibitTx1", "TuneInhibitTx2", "TuneInhibitTx3"}) {
            check(settings.value(QLatin1String(key)).toString() == QLatin1String("True"), "legacy preference remains untouched");
        }
    }
}

void ownershipChangeDuringTune()
{
    AppSettings& settings = AppSettings::instance();
    for (const char* key : {"TuneInhibitAccTx", "TuneInhibitTx1", "TuneInhibitTx2", "TuneInhibitTx3"}) {
        settings.setValue(QLatin1String(key), QStringLiteral("True"));
    }
    struct Versions { const char* start; const char* stop; };
    for (const Versions& versions : {Versions{"", "4.3.16.42034"},
                                    Versions{"1.4.0.0", "4.3.16.42034"},
                                    Versions{"4.2.20.41343", "4.3.16.42034"},
                                    Versions{"4.3.16.42034", "4.2.20.41343"}}) {
        Fixture f;
        f.version = QLatin1String(versions.start);
        TuneOutputControlTestAccess::identity(f.radio, f.version);
        f.band(9, QStringLiteral("20"), QStringLiteral("0"));
        f.wire.clear();
        TuneOutputControlTestAccess::apply(f.radio);
        const QStringList applied = f.wire;
        check(!applied.isEmpty(), "TUNE start actually dispatches inhibit requests");
        f.version = QLatin1String(versions.stop);
        TuneOutputControlTestAccess::identity(f.radio, f.version);
        emit f.radio.infoChanged();
        f.wire.clear();
        TuneOutputControlTestAccess::restore(f.radio);
        QStringList expected = applied;
        for (QString& command : expected) {
            command.chop(1);
            command += QLatin1Char('1');
        }
        check(f.wire == expected,
              "restore retains start-time RCA ownership when firmware identity changes during TUNE");
        f.wire.clear();
        TuneOutputControlTestAccess::apply(f.radio);
        QStringList nextCycle{QStringLiteral("interlock bandset 9 acc_tx_enabled=0")};
        if (TuneOutputPolicy::clientManagesRca(f.version)) {
            for (int output : {1, 2, 3}) {
                nextCycle << QStringLiteral("interlock bandset 9 tx%1_enabled=0").arg(output);
            }
        }
        check(f.wire == nextCycle, "the next TUNE cycle evaluates the current firmware ownership anew");
        TuneOutputControlTestAccess::restore(f.radio);
    }
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("tune-output-control"));
    if (!profile.isValid()) { return 1; }
    QCoreApplication app(argc, argv);
    policyAndParser();
    authorityAndReplies();
    explicitRowsIgnoreTxSelection();
    genericSinkCompatibility();
    droppedDispatch();
    firmwareOwnedPreferenceChanges();
    readbackBeforeFirmwareIdentity();
    legacyOwnership();
    ownershipChangeDuringTune();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
