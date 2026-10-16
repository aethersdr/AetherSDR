// Published FlexLib API 4.3.16 Radio.cs GPSFixState/ParseGPSFixState is the
// wire authority. All delivery is injected: no peer, socket bind, or radio.
#include "TestSettingsProfile.h"
#include "core/RadioDiscovery.h"
#include "core/backends/GpsDelta.h"
#include "core/backends/flex/CommandParser.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "core/backends/flex/WanConnection.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QSignalSpy>
#include <QStringList>

#include <array>
#include <cstdio>
#include <memory>
#include <thread>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

// An injected transport with no connect/disconnect effects. Production session
// selection and reset still run, including when the backend family is reused.
class InertBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override { ++connects; }
    void disconnectRadio() override {}
    bool isConnected() const override { return false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    int connects{0};
};

GpsDelta decode(FlexBackend& backend, const QString& body)
{
    QSignalSpy spy(&backend, &IRadioBackend::gpsChanged);
    backend.decodeGpsStatus(body);
    check(spy.count() == 1, "one typed GPS delta per decoded status");
    return spy.isEmpty() ? GpsDelta{} : spy.at(0).at(0).value<GpsDelta>();
}

void deliver(RadioModel& model, const GpsDelta& delta)
{
    emit model.backend()->gpsChanged(delta);
}

GpsDelta fullReport()
{
    GpsDelta delta;
    delta.status = QStringLiteral("Fine Lock");
    delta.fixState = GpsFixState::ThreeD;
    delta.positionValid = true;
    delta.source = QStringLiteral("GPSDO");
    delta.tracked = 8;
    delta.visible = 12;
    delta.grid = QStringLiteral("DN18rg");
    delta.altitude = QStringLiteral("644 m");
    delta.lat = QStringLiteral("48.27");
    delta.lon = QStringLiteral("-116.56");
    delta.time = QStringLiteral("05:25:20Z");
    delta.date = QStringLiteral("2026-10-14");
    delta.speed = QStringLiteral("0 kts");
    delta.track = QStringLiteral("273.4");
    delta.freqError = QStringLiteral("0 ppb");
    delta.ntpEnabled = true;
    delta.ntpServer = QStringLiteral("time.example.test");
    delta.gpsTimeCorrectionEnabled = true;
    delta.ntpSyncStatus = QStringLiteral("synced");
    return delta;
}

void checkCleared(const RadioModel& model)
{
    check(model.gpsFixState() == GpsFixState::Unknown, "reset clears fix to Unknown");
    check(!model.gpsPositionValid(), "reset clears position validity");
    check(!model.gpsTimeValid(), "reset clears time validity");
    check(model.gpsStatus().isEmpty() && model.gpsSource().isEmpty(), "reset clears receiver and source");
    check(model.gpsTracked() == 0 && model.gpsVisible() == 0, "reset clears satellite counts");
    check(model.gpsGrid().isEmpty() && model.gpsAltitude().isEmpty()
          && model.gpsLat().isEmpty() && model.gpsLon().isEmpty(), "reset clears position text");
    check(model.gpsTime().isEmpty() && model.gpsDate().isEmpty()
          && model.gpsSpeed().isEmpty() && model.gpsTrack().isEmpty()
          && model.gpsFreqError().isEmpty(), "reset clears timing and motion text");
    check(!model.gpsNtpEnabled() && model.gpsNtpServer().isEmpty()
          && !model.gpsTimeCorrectionEnabled() && model.gpsNtpSyncStatus().isEmpty(),
          "reset clears time configuration");
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("gps-fix-state-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    qRegisterMetaType<GpsDelta>();
    FlexBackend decoder;

    struct FixCase { const char* wire; GpsFixState expected; };
    const std::array cases{
        FixCase{"3D", GpsFixState::ThreeD}, FixCase{"3d", GpsFixState::ThreeD},
        FixCase{"2D", GpsFixState::TwoD}, FixCase{"2d", GpsFixState::TwoD},
        FixCase{"TIME_ONLY", GpsFixState::TimeOnly}, FixCase{"tImE_oNlY", GpsFixState::TimeOnly},
        FixCase{"NO_FIX", GpsFixState::NoFix}, FixCase{"nO_fIx", GpsFixState::NoFix},
        FixCase{"", GpsFixState::Unknown}, FixCase{"UNKNOWN", GpsFixState::Unknown},
        FixCase{"4D", GpsFixState::Unknown}, FixCase{" 3D", GpsFixState::Unknown},
        FixCase{"3D ", GpsFixState::Unknown}, FixCase{"NO FIX", GpsFixState::Unknown},
        FixCase{"3D=other", GpsFixState::Unknown}
    };
    for (const FixCase& item : cases) {
        const GpsDelta delta = decode(decoder, QStringLiteral("FiX=") + QLatin1String(item.wire));
        check(delta.fixState == item.expected, item.wire);
        check(!delta.positionValid && !delta.status && !delta.source,
              "fix alone never invents position validity, receiver lock or source");
    }
    check(!decode(decoder, QStringLiteral("status=Fine Lock#tracked=8")).fixState,
          "old firmware status leaves fix absent");
    check(!decode(decoder, QStringLiteral("fix#=3D#tracked=8")).fixState,
          "malformed field leaves fix absent");

    WanConnection wan;
    RadioModel model;
    QSignalSpy reports(&model, &RadioModel::gpsStatusChanged);
    QSignalSpy timeSettings(&model, &RadioModel::gpsTimeSettingsChanged);
    checkCleared(model);
    deliver(model, decode(decoder, QStringLiteral("status=Fine Lock#lat=48.27#lon=-116.56")));
    check(model.gpsFixState() == GpsFixState::Unknown && model.gpsPositionValid() && model.gpsTimeValid(),
          "old firmware retains legacy locked-position validity without a fix field");
    const int beforeFix = reports.count();
    GpsDelta fixOnly;
    fixOnly.fixState = GpsFixState::NoFix;
    fixOnly.ntpEnabled = true;
    deliver(model, fixOnly);
    check(reports.count() == beforeFix + 1 && timeSettings.count() == 1,
          "fix counts as telemetry even when clock settings share the delta");
    check(!model.gpsPositionValid() && model.gpsStatus() == QLatin1String("Fine Lock"),
          "NO_FIX vetoes legacy validity without overwriting receiver lock");
    check(!model.gpsTimeValid(), "NO_FIX vetoes independently established time validity");
    fixOnly.ntpEnabled.reset();
    fixOnly.fixState = GpsFixState::TimeOnly;
    deliver(model, fixOnly);
    check(!model.gpsPositionValid(), "TIME_ONLY vetoes legacy validity");
    check(model.gpsTimeValid(), "TIME_ONLY preserves independently established time validity");
    deliver(model, decode(decoder, QStringLiteral("visible=11#time=05:26:00Z")));
    check(model.gpsFixState() == GpsFixState::TimeOnly && !model.gpsPositionValid(),
          "partial status preserves the previous fix and its validity veto");
    deliver(model, decode(decoder, QStringLiteral("fix=2D")));
    check(model.gpsPositionValid(), "2D allows independently established legacy validity");
    deliver(model, decode(decoder, QStringLiteral("fix=nonsense")));
    check(model.gpsFixState() == GpsFixState::Unknown && model.gpsPositionValid(),
          "reported unknown clears known fix and falls back to legacy validity");
    deliver(model, decode(decoder, QStringLiteral("status=Present#tracked=12#fix=3D")));
    check(!model.gpsPositionValid() && model.gpsStatus() == QLatin1String("Present"),
          "3D and satellite count never manufacture receiver lock or validity");
    deliver(model, decode(decoder, QStringLiteral("fix=2D")));
    check(!model.gpsPositionValid(), "2D alone never sets position validity true");
    check(!model.gpsTimeValid(), "2D alone never sets time validity true");
    deliver(model, decode(decoder, QStringLiteral("fix=TIME_ONLY")));
    check(!model.gpsTimeValid(), "TIME_ONLY alone never sets time validity true");
    deliver(model, decode(decoder, QStringLiteral("fix=3D")));
    check(!model.gpsTimeValid(), "3D alone never sets time validity true");

    deliver(model, fullReport());
    reports.clear();
    const ParsedMessage rawGps = CommandParser::parseLine(QStringLiteral(
        "S0|gps status=Fine Lock#fix=3D#lat=48.27#lon=-116.56"));
    RadioConnection* lan = model.connection();
    std::thread queuedLanReport([lan, rawGps] { emit lan->messageReceived(rawGps); });
    queuedLanReport.join();
    check(reports.isEmpty(), "raw LAN GPS report is queued before disconnect");
    check(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection),
          "production disconnect slot is reachable without transport");
    checkCleared(model);
    check(reports.count() == 1, "disconnect notifies GPS consumers of reset");
    const int clearedTimeSettings = timeSettings.count();
    check(QMetaObject::invokeMethod(&model, "onDisconnected", Qt::DirectConnection),
          "repeated empty reset remains reachable");
    check(reports.count() == 1 && timeSettings.count() == clearedTimeSettings,
          "reset of already empty GPS state emits no duplicate notifications");
    QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
    checkCleared(model);
    check(reports.count() == 1, "queued raw LAN GPS cannot undo disconnect reset");

    deliver(model, fullReport());
    reports.clear();
    check(model.rebuildBackendForTest(QStringLiteral("sim")), "production family replacement builds");
    checkCleared(model);
    check(reports.count() >= 1, "family replacement notifies GPS consumers of reset");

    auto backend = std::make_unique<InertBackend>();
    InertBackend* transport = backend.get();
    model.setBackendForTest(std::move(backend), QStringLiteral("gps-test"));
    for (const char* serial : {"radio-a", "radio-b"}) {
        deliver(model, fullReport());
        reports.clear();
        RadioInfo info;
        info.family = QStringLiteral("gps-test");
        info.serial = QLatin1String(serial);
        std::thread queuedSessionReport([transport] { emit transport->gpsChanged(fullReport()); });
        queuedSessionReport.join();
        model.connectToRadio(info);
        QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
        checkCleared(model);
        check(model.backend() == transport, "same-family session reuses backend");
        check(reports.count() == 1, "new same-family LAN session publishes GPS reset");
    }
    check(transport->connects == 2, "both LAN session attempts used the injected transport");
    deliver(model, fullReport());
    reports.clear();
    model.connectViaWan(&wan, QStringLiteral("192.0.2.1"), 4992);
    checkCleared(model);
    check(reports.count() == 1, "new WAN session publishes GPS reset before any status");

    // A real Flex decoder on the model proves a fresh raw subscription survives
    // the epoch transition. No transport is connected or opened.
    check(model.rebuildBackendForTest(QStringLiteral("flex")), "raw-message test installs Flex decoder");
    model.connectViaWan(&wan, QStringLiteral("192.0.2.3"), 4992);
    // WAN source stays unopened. Only its real messageReceived subscription is
    // exercised, including a repeated session with exactly the same objects.
    QSignalSpy profiles(&model, &RadioModel::globalProfilesChanged);
    const ParsedMessage queuedProfile = CommandParser::parseLine(QStringLiteral(
        "S0|profile global list=Profile one^Profile two"));
    std::thread queuedWanReport([&wan, rawGps, queuedProfile] {
        emit wan.messageReceived(rawGps);
        emit wan.messageReceived(queuedProfile);
    });
    queuedWanReport.join();
    check(profiles.isEmpty(), "raw profile delivery is queued before GPS epoch reset");
    model.connectViaWan(&wan, QStringLiteral("192.0.2.2"), 4992);
    reports.clear();
    QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
    checkCleared(model);
    check(reports.isEmpty(), "queued raw WAN GPS cannot undo same-family session reset");
    check(profiles.count() == 1
          && model.globalProfiles() == QStringList{QStringLiteral("Profile one"), QStringLiteral("Profile two")},
          "GPS epoch reset preserves queued non-GPS profile dispatch exactly once");
    emit wan.messageReceived(CommandParser::parseLine(QStringLiteral(
        "S0|profile global current=Fresh profile")));
    check(profiles.count() == 2 && model.activeGlobalProfile() == QLatin1String("Fresh profile"),
          "fresh raw non-GPS status reaches one receiver after session rebind");

    emit wan.messageReceived(rawGps);
    check(reports.count() == 1, "fresh raw GPS reaches one receiver after session rebind");
    check(model.gpsFixState() == GpsFixState::ThreeD && model.gpsPositionValid()
          && model.gpsTimeValid(), "fresh raw GPS still reaches the model after session rebind");

    deliver(model, fullReport());
    reports.clear();
    model.setBackendForTest(std::make_unique<InertBackend>(), QStringLiteral("gps-test"));
    checkCleared(model);
    check(reports.count() == 1, "injected backend replacement uses the GPS reset path");

    IRadioBackend* oldBackend = model.backend();
    // Intentional lifetime fault injection: emission from another thread posts
    // the existing AutoConnection before replacement; no event drain intervenes.
    std::thread queuedOldReport([oldBackend] { emit oldBackend->gpsChanged(fullReport()); });
    queuedOldReport.join();
    check(reports.count() == 1, "old GPS delivery is queued, not already applied");
    check(model.rebuildBackendForTest(QStringLiteral("sim")), "swap retires the queued report's backend");
    GpsDelta fresh;
    fresh.fixState = GpsFixState::TimeOnly;
    fresh.source = QStringLiteral("new session");
    deliver(model, fresh);
    reports.clear();
    QCoreApplication::sendPostedEvents(&model, QEvent::MetaCall);
    check(model.gpsFixState() == GpsFixState::TimeOnly
          && model.gpsSource() == QLatin1String("new session") && !model.gpsPositionValid(),
          "queued old-backend GPS cannot contaminate the new family");
    check(reports.isEmpty(), "discarded old GPS report emits no consumer notification");

    std::printf("gps_fix_state_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
