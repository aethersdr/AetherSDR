// TCI2.1 §4.1 p14: operating frequency limits in Hz.
// Public protocol paths and production profile decoding, without any sockets.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/TciProtocol.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "models/RadioModel.h"
#include <QByteArray>
#include <QCoreApplication>
#include <QPair>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtGlobal>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
using namespace AetherSDR;
namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static bool profile(KenwoodBackend& backend, const RadioConnectRequest& request)
    {
        // Reuse the actual production validator/variant decoder; connectRadio
        // itself is deliberately not called, because it creates real sockets.
        if (!KenwoodBackend::validateConnectRequest(request).isEmpty()) { return false; }
        const std::optional<qint64> maximum = maximumFrequencyForVariant(request.params.value("kenwood.variant").toString());
        if (!maximum) { return false; }
        backend.m_maximumFrequencyHz = *maximum;
        return true;
    }
};
}
namespace {
void require(bool condition, const char* claim)
{
    if (!condition) { std::cerr << claim << '\n'; std::exit(1); }
}
class BoundsBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    int mutations = 0;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override { ++mutations; }
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override { ++mutations; }
    void setSliceMode(int, const QString&) override { ++mutations; }
    void setSliceFilter(int, int, int) override { ++mutations; }
    void setSliceAgc(int, const QString&, int) override { ++mutations; }
    void setPanCenter(const QString&, double, PanCenterIntent) override { ++mutations; }
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override { ++mutations; }
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override { ++mutations; }
};
QString initLimits(TciProtocol& protocol)
{
    const QString init = protocol.generateInitBurst();
    require(init.endsWith("ready;"), "limit handling preserves completion of initialization");
    QString limits;
    int count = 0;
    for (const QString& command : init.split(';', Qt::SkipEmptyParts)) {
        if (command.startsWith("vfo_limits:")) { limits = command + ';'; ++count; }
    }
    require(count <= 1, "init advertises at most one operating interval");
    return limits;
}
void sameLimits(TciProtocol& protocol, const QString& expected)
{
    require(initLimits(protocol) == expected, "initialization reflects the declared receive interval");
    require(protocol.handleCommand("vfo_limits") == expected, "GET and initialization agree for a declared interval");
    require(protocol.pendingNotification().isEmpty(), "range metadata never requests radio mutation");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("tci-receive-limits"));
    require(settings.isValid(), "isolated settings profile");
    QCoreApplication application(argc, argv);
    AppSettings::instance().load();
    TciProtocol empty(nullptr);
    require(initLimits(empty) == "vfo_limits:1000,75000000;"
        && empty.handleCommand("vfo_limits") == "vfo_limits:30000,54000000;",
        "null-model legacy assumptions remain unchanged, including their prior mismatch");
    RadioModel radio;
    auto owned = std::make_unique<BoundsBackend>();
    BoundsBackend* bounds = owned.get();
    radio.setBackendForTest(std::move(owned), "sim");
    TciProtocol protocol(&radio);
    require(initLimits(protocol) == "vfo_limits:1000,75000000;"
        && protocol.handleCommand("vfo_limits") == "vfo_limits:30000,54000000;",
        "zero/zero unreported capabilities preserve existing Flex/Sim assumptions");
    struct Range { double low; double high; const char* expected; };
    const QList<Range> known{{30000, 60000000, "vfo_limits:30000,60000000;"},
        {30000, 74800000, "vfo_limits:30000,74800000;"},
        {24000, 1766000000, "vfo_limits:24000,1766000000;"},
        {30000, 470000000, "vfo_limits:30000,470000000;"},
        {0, 61440000, "vfo_limits:0,61440000;"},
        {30000.25, 60000000.75, "vfo_limits:30001,60000000;"}};
    for (const Range& range : known) {
        bounds->caps.tuningMinHz = range.low;
        bounds->caps.tuningMaxHz = range.high;
        require(initLimits(protocol) == "vfo_limits:1000,75000000;"
            && protocol.handleCommand("vfo_limits") == "vfo_limits:30000,54000000;",
            "non-opt-in backends retain baseline limits even with a declared native interval");
    }
    bounds->caps.receiveClientControl = ReceiveClientControl{{ReceiveClientControl::Feature::FrequencyLimits}};
    for (const Range& range : known) {
        bounds->caps.tuningMinHz = range.low;
        bounds->caps.tuningMaxHz = range.high;
        sameLimits(protocol, QString::fromLatin1(range.expected));
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const double exclusive = -static_cast<double>(std::numeric_limits<qint64>::min());
    const QList<QPair<double, double>> invalid{{-1, 100}, {100, 0}, {100, 99}, {100, 100},
        {nan, 100}, {0, nan}, {inf, inf}, {0, inf}, {0, exclusive},
        {0, std::numeric_limits<double>::max()}, {1.1, 1.9}};
    for (const QPair<double, double>& range : invalid) {
        bounds->caps.tuningMinHz = range.first;
        bounds->caps.tuningMaxHz = range.second;
        sameLimits(protocol, {});
    }
    const double largest = std::nextafter(exclusive, 0.0);
    bounds->caps.tuningMinHz = 0;
    bounds->caps.tuningMaxHz = largest;
    sameLimits(protocol, QStringLiteral("vfo_limits:0,%1;").arg(static_cast<qint64>(largest)));
    require(bounds->mutations == 0, "all range metadata paths remain strictly read-only");
    for (const QString& variant : {QStringLiteral("k"), QStringLiteral("e")}) {
        RadioModel kenwoodRadio;
        int writes = 0;
        auto backend = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [&](const QByteArray&) { ++writes; return false; }, [&](const QByteArray&) { ++writes; return false; }});
        RadioConnectRequest request;
        request.host = "fixture.invalid";
        request.port = 60000;
        request.params = {{QStringLiteral("kenwood.variant"), variant},
            {QStringLiteral("kenwood.username"), QStringLiteral("fixture")},
            {QStringLiteral("kenwood.password"), QStringLiteral("fixture")}};
        require(kenwood::KenwoodBackendTestAccess::profile(*backend, request), "production regional profile validation/decoding succeeds");
        kenwoodRadio.setBackendForTest(std::move(backend), "kenwood");
        TciProtocol kenwoodProtocol(&kenwoodRadio);
        sameLimits(kenwoodProtocol, variant == "k" ? "vfo_limits:30000,60000000;" : "vfo_limits:30000,74800000;");
        require(writes == 0, "production Kenwood capability reporting opens no transport and writes no commands");
    }
    std::cout << "TCI receive limit checks passed\n";
}
