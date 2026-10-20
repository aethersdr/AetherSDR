// Real typed model/decoder/encoder paths, with an injected sink and no radio peer.
#include "TestSettingsProfile.h"
#include "core/backends/DspParameter.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/sim/SimBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMap>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <array>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace AetherSDR {
class RadioModelSliceLifecycleTestAccess {
public:
    static void identity(RadioModel& radio, const QString& model, const QString& version)
    {
        radio.m_model = model;
        radio.m_version = version;
        emit radio.infoChanged();
    }
    static void own(RadioModel& radio, bool owned)
    {
        if (owned) { radio.m_ownedSliceIds.insert(0); }
        else { radio.m_ownedSliceIds.remove(0); }
        emit radio.slotOccupancyChanged(0);
    }
    static void foreign(RadioModel& radio, bool foreign)
    {
        if (foreign) { radio.m_foreignSliceOwners.insert(0, 0x1234u); }
        else { radio.m_foreignSliceOwners.remove(0); }
    }
    static void removeSlice(RadioModel& radio)
    {
        radio.handleSliceStatus(0, {}, true);
    }
    static void license(RadioModel& radio, const QString& enabled)
    {
        radio.onStatusReceived(QStringLiteral("license feature"), {
            {QStringLiteral("name"), QStringLiteral("noise_reduction")},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("reason"), QStringLiteral("LICENSE_FILE")}});
    }
    static void expiredSubscription(RadioModel& radio)
    {
        radio.m_licenseSubscription = QStringLiteral("SmartSDR");
        radio.m_licenseExpirationDate = QStringLiteral("01/01/2000");
    }
    static void retire(RadioModel& radio)
    {
        radio.m_intentionalDisconnect = true;
        radio.onDisconnected();
    }
    static void reclaim(RadioModel& radio)
    {
        radio.stageSessionModelsForReconnect();
        identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16.42034"));
        own(radio, true);
        FlexBackend* previous = radio.m_flexBackend;
        radio.m_flexBackend = qobject_cast<FlexBackend*>(radio.backend());
        radio.handleSliceStatus(0, {{QStringLiteral("in_use"), QStringLiteral("1")},
            {QStringLiteral("RF_frequency"), QStringLiteral("14.2")},
            {QStringLiteral("mode"), QStringLiteral("USB")}}, false);
        radio.m_flexBackend = previous;
    }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(bool passed, const char* claim)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", claim);
    failures += !passed;
}

struct WireCase {
    DspParameter parameter;
    const char* command;
    const char* status;
    int value;
    const char* encoded;
};

// Independent expected protocol table from published FlexLib 4.3.16 setters/parsers.
constexpr std::array<WireCase, 17> kCases{{
    {DspParameter::NrDelay, "nr_delay", "nr_delay", 123, "123"},
    {DspParameter::NrWindow, "nr_wlen", "nr_wlen", 128, "128"},
    {DspParameter::NrIsdft, "nr_isdft_mode", "nr_isdft_mode", 1, "half"},
    {DspParameter::NrAdapt, "nr_adapt_mode", "nr_adapt_mode", 2, "slow"},
    {DspParameter::AnfDelay, "anf_delay", "anf_delay", 5, "5"},
    {DspParameter::AnfWindow, "anf_wlen", "anf_wlen", 256, "256"},
    {DspParameter::AnfIsdft, "anf_isdft_mode", "anf_isdft_mode", 2, "zero"},
    {DspParameter::AnfAdapt, "anf_adapt_mode", "anf_adapt_mode", 1, "fast"},
    {DspParameter::NrlDelay, "lms_nr_delay", "nrl_delay", 9, "9"},
    {DspParameter::NrlTaps, "lms_nr_taps", "nrl_filter_size", 97, "97"},
    {DspParameter::NrlLeakage, "lms_nr_leakage_level", "nrl_leakage_level", 54, "54"},
    {DspParameter::AnflDelay, "lms_anf_delay", "anfl_delay", 11, "11"},
    {DspParameter::AnflTaps, "lms_anf_taps", "anfl_filter_size", 111, "111"},
    {DspParameter::AnflLeakage, "lms_anf_leakage_level", "anfl_leakage_level", 22, "22"},
    {DspParameter::AnftMinDb, "anft_min_db", "anft_min_db", -105, "-105"},
    {DspParameter::AnftToneDiff, "anft_tone_db_diff", "anft_tone_db_diff", 17, "17"},
    {DspParameter::NrfWindow, "nrf_wlen", "nrf_wlen", 4096, "4096"}
}};

QMap<QString, QString> statusValues()
{
    QMap<QString, QString> result;
    for (const WireCase& item : kCases) {
        result.insert(QString::fromLatin1(item.status), QString::fromLatin1(item.encoded));
    }
    return result;
}

class ConnectedFlex final : public FlexBackend {
public:
    bool connected{true};
    bool isConnected() const override { return connected; }
};

struct Fixture {
    RadioModel radio;
    ConnectedFlex* backend{nullptr};
    QStringList commands;
    QStringList readbacks;
    QList<FlexBackend::CommandReply> replies;

    Fixture()
    {
        auto owned = std::make_unique<ConnectedFlex>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
        backend->setModelProvider([this] { return radio.model(); });
        backend->setFirmwareVersionProvider([this] { return radio.softwareVersion(); });
        backend->setDspResetLicenseProvider([this]() -> std::optional<bool> {
            const LicenseFeatureState feature = radio.licenseFeature(QStringLiteral("NOISE_REDUCTION"));
            return feature.seen ? std::optional<bool>(feature.enabled) : std::nullopt;
        });
        backend->setSliceCommandSink([this](const QString& command) { commands.append(command); });
        backend->setCommandSink([this](const QString& command, FlexBackend::CommandReply reply) {
            if (command == QStringLiteral("sub slice all")) {
                readbacks.append(command);
            } else {
                commands.append(command);
            }
            if (reply) {
                replies.append(std::move(reply));
            }
            return true;
        });
        identity(QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16.42034"));
        SliceDelta initial;
        initial.panId = QStringLiteral("0x40000000");
        initial.mode = QStringLiteral("USB");
        initial.inUse = true;
        initial.nr = true;
        initial.nrLevel = 73;
        initial.nrl = true;
        initial.nrfLevel = 51;
        emit backend->sliceChanged(0, initial);
        RadioModelSliceLifecycleTestAccess::own(radio, true);
        commands.clear();
    }
    SliceModel* slice() { return radio.slice(0); }
    void identity(const QString& model, const QString& version)
    { RadioModelSliceLifecycleTestAccess::identity(radio, model, version); }
    void license(bool enabled = true)
    { RadioModelSliceLifecycleTestAccess::license(radio, enabled ? QStringLiteral("1") : QStringLiteral("0")); }
    void report() { backend->decodeSliceStatus(0, statusValues()); }
    bool request(DspParameter parameter, int value)
    { return radio.requestDspParameter(slice(), radio.receiveControlSession(), {parameter, value}); }
};

void exactEncodingAndReadback()
{
    Fixture f;
    check(f.slice()->dspParameters().isEmpty() && f.commands.isEmpty(),
          "construction has no advanced defaults, optimistic writes or commands");
    f.license();
    check(!f.request(DspParameter::NrDelay, 123) && f.commands.isEmpty(),
          "an unobserved parameter cannot authorize an operator edit");
    QSignalSpy changed(f.slice(), &SliceModel::dspParametersChanged);
    const quint64 revision = f.slice()->dspParametersRevision();
    f.report();
    check(f.slice()->dspParameters().size() == 17 && changed.size() == 1
              && f.slice()->dspParametersRevision() == revision + 1 && f.commands.isEmpty(),
          "one real decoder report publishes all 17 observations and one revision without feedback");
    for (const WireCase& item : kCases) {
        check(f.slice()->dspParameter(item.parameter) == item.value,
              "published status name decodes to the canonical parameter value");
        f.commands.clear();
        check(f.request(item.parameter, item.value)
                  && f.commands == QStringList{QStringLiteral("slice set 0 %1=%2")
                      .arg(QString::fromLatin1(item.command), QString::fromLatin1(item.encoded))},
              "deliberate request emits exactly the published setter key/value once");
    }
    f.commands.clear();
    const DspParameterValues before = f.slice()->dspParameters();
    const quint64 beforeRequest = f.slice()->dspParametersRevision();
    check(f.request(DspParameter::NrDelay, 151)
              && f.slice()->dspParameters() == before
              && f.slice()->dspParametersRevision() == beforeRequest,
          "dispatch remains a request until radio status confirms it");
    f.commands.clear();
    f.backend->decodeSliceStatus(0, {{QStringLiteral("nr_delay"), QStringLiteral("151")}});
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 151
              && f.slice()->dspParameter(DspParameter::NrlTaps) == 97 && f.commands.isEmpty(),
          "sparse profile status updates only reported fields without replaying a command");
    const quint64 beforeEcho = f.slice()->dspParametersRevision();
    f.backend->decodeSliceStatus(0, {{QStringLiteral("nr_delay"), QStringLiteral("151")}});
    check(f.slice()->dspParametersRevision() == beforeEcho + 1,
          "same-value profile/reset echo advances the draft-invalidating observation revision");
    check(f.radio.requestDspParameterReset(f.slice(), f.radio.receiveControlSession(), DspParameterResetTarget::Nr),
          "existing typed reset remains available alongside advanced parameters");
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 151,
          "reset dispatch never substitutes guessed local parameter defaults");
    f.commands.clear();
    f.backend->decodeSliceStatus(0, {{QStringLiteral("nr_delay"), QStringLiteral("64")},
        {QStringLiteral("nr_wlen"), QStringLiteral("256")}});
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 64
              && f.slice()->dspParameter(DspParameter::NrWindow) == 256
              && f.slice()->nrOn() && f.slice()->nrLevel() == 73 && f.slice()->nrlOn()
              && f.commands.isEmpty(), "reset echo converges through production status without changing unrelated DSP");
}

void boundsAndMalformedInput()
{
    Fixture f;
    f.license();
    f.report();
    check(dspParameterSpecs().size() == 17, "all six algorithms have their 17 descriptors");
    for (const DspParameterSpec& spec : dspParameterSpecs()) {
        check(validDspParameterValue(spec.parameter, spec.minimum)
                  && validDspParameterValue(spec.parameter, spec.maximum),
              "each published numeric/enum boundary is accepted");
        for (const int endpoint : {spec.minimum, spec.maximum}) {
            f.commands.clear();
            check(f.request(spec.parameter, endpoint) && f.commands.size() == 1,
                  "production request path accepts both published endpoints");
        }
        if (!spec.choices.isEmpty()) {
            const bool inverse = spec.parameter == DspParameter::NrIsdft || spec.parameter == DspParameter::AnfIsdft;
            const QStringList expected = inverse ? QStringList{"one", "half", "zero"}
                : QStringList{"auto", "fast", "slow"};
            const WireCase* wire = nullptr;
            for (const WireCase& candidate : kCases) {
                if (candidate.parameter == spec.parameter) { wire = &candidate; break; }
            }
            for (int choice = 0; choice < expected.size(); ++choice) {
                f.commands.clear();
                check(wire && f.request(spec.parameter, choice)
                    && f.commands == QStringList{QStringLiteral("slice set 0 %1=%2")
                        .arg(QString::fromLatin1(wire->command), expected[choice])},
                    "all enum choices use independently specified published strings");
            }
        }
        for (const int invalid : {spec.minimum - 1, spec.maximum + 1,
                std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
            f.commands.clear();
            check(!validDspParameterValue(spec.parameter, invalid)
                      && !f.request(spec.parameter, invalid)
                      && f.backend->requestDspParameter(0, {spec.parameter, invalid}) == ReceiveDispatch::Unsupported
                      && f.commands.isEmpty(), "range violations are rejected at both command boundaries");
        }
        if (spec.powerOfTwo) {
            check(!f.request(spec.parameter, spec.minimum + 1), "in-range non-power window values are rejected");
            for (int value = spec.minimum; value <= spec.maximum; value *= 2) {
                check(f.request(spec.parameter, value), "each supported power-of-two window is writable");
            }
        }
    }
    f.commands.clear();
    for (const WireCase& item : kCases) {
        const DspParameterSpec* spec = dspParameterSpec(item.parameter);
        const DspParameterValues before = f.slice()->dspParameters();
        const quint64 revision = f.slice()->dspParametersRevision();
        QStringList malformed{QString(), QStringLiteral("garbage"), QStringLiteral("1.5"),
            QStringLiteral("2147483648"), QStringLiteral("NaN")};
        malformed.append(QString::number(spec->minimum - 1));
        malformed.append(QString::number(spec->maximum + 1));
        if (spec->powerOfTwo) { malformed.append(QString::number(spec->minimum + 1)); }
        if (!spec->choices.isEmpty()) {
            malformed.append({QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2"),
                QStringLiteral("AUTO"), QStringLiteral("HALF"), QStringLiteral(" half ")});
        }
        for (const QString& value : malformed) {
            f.backend->decodeSliceStatus(0, {{QString::fromLatin1(item.status), value}});
            check(f.slice()->dspParameters() == before && f.slice()->dspParametersRevision() == revision
                      && f.commands.isEmpty(), "malformed/out-of-domain status is ignored without clamping or feedback");
        }
        if (QString::fromLatin1(item.command) != QString::fromLatin1(item.status)) {
            f.backend->decodeSliceStatus(0, {{QString::fromLatin1(item.command), QStringLiteral("1")}});
            check(f.slice()->dspParameters() == before && f.slice()->dspParametersRevision() == revision,
                  "LMS command spelling is not mistaken for a published status key");
        }
    }
    for (const int invalid : {-1, 17, 999}) {
        const DspParameter parameter = static_cast<DspParameter>(invalid);
        check(!dspParameterSpec(parameter) && !f.request(parameter, 1)
                  && f.backend->requestDspParameter(0, {parameter, 1}) == ReceiveDispatch::Unsupported,
              "unknown parameter enum cannot become a wire key");
    }
    const DspParameterValues before = f.slice()->dspParameters();
    const quint64 revision = f.slice()->dspParametersRevision();
    SliceDelta injected;
    injected.dspParameters = {{DspParameter::NrDelay, 0}, {DspParameter::NrWindow, 48},
        {DspParameter::AnfAdapt, 3}, {static_cast<DspParameter>(999), 1}};
    f.slice()->applyChanges(injected);
    check(f.slice()->dspParameters() == before && f.slice()->dspParametersRevision() == revision,
          "vendor-neutral injected deltas cannot bypass parameter validation");
    injected.dspParameters.insert(DspParameter::NrlTaps, 101);
    f.slice()->applyChanges(injected);
    check(f.slice()->dspParameter(DspParameter::NrlTaps) == 101
              && f.slice()->dspParameter(DspParameter::NrDelay) == before.value(DspParameter::NrDelay)
              && f.slice()->dspParametersRevision() == revision + 1,
          "mixed delta retains valid non-power LMS taps while dropping malformed companions");
}

void firmwareModelAndLicense()
{
    Fixture f;
    f.report();
    check(!f.request(DspParameter::NrDelay, 1) && f.commands.isEmpty(), "unknown feature entitlement fails closed");
    f.license(false);
    check(!f.request(DspParameter::NrDelay, 1) && f.commands.isEmpty(), "explicit feature denial fails closed");
    f.license();
    RadioModelSliceLifecycleTestAccess::expiredSubscription(f.radio);
    check(f.request(DspParameter::NrDelay, 1), "confirmed retained entitlement works after subscription expiration");
    for (const QString& version : {QString(), QStringLiteral("unknown"), QStringLiteral("4.2.20.41343"),
            QStringLiteral("4.3"), QStringLiteral("4.3.15"), QStringLiteral("4.3.16junk"),
            QStringLiteral("4.3.16.42034junk"), QStringLiteral(" 4.3.16"), QStringLiteral("+4.3.16")}) {
        f.identity(QStringLiteral("FLEX-8600"), version);
        f.commands.clear();
        check(!f.radio.backendCapabilities().dspParameters && !f.request(DspParameter::NrDelay, 1)
                  && f.backend->requestDspParameter(0, {DspParameter::NrDelay, 1}) == ReceiveDispatch::Unsupported
                  && f.commands.isEmpty(), "older/malformed/unknown firmware cannot use the unproved advanced contract");
    }
    for (const QString& version : {QStringLiteral("4.3.16"), QStringLiteral("4.3.16.42034"), QStringLiteral("5.0.0")}) {
        f.identity(QStringLiteral("FLEX-8600"), version);
        check(f.request(DspParameter::NrDelay, 1), "verified firmware boundary and newer versions are supported");
    }
    for (const QString& model : {QStringLiteral("FLEX-6300"), QStringLiteral("FLEX-6400"),
            QStringLiteral("FLEX-6500"), QStringLiteral("FLEX-6600"), QStringLiteral("FLEX-6700")}) {
        f.identity(model, QStringLiteral("4.3.16"));
        f.commands.clear();
        check(f.request(DspParameter::NrlTaps, 17) && f.request(DspParameter::AnftMinDb, -100),
              "known 6000 models support entitled base advanced algorithms");
        f.commands.clear();
        check(!f.request(DspParameter::NrfWindow, 1024) && f.commands.isEmpty(),
              "6000 models refuse the extended-platform NRF parameter");
    }
    for (const QString& model : {QStringLiteral("FLEX-8400"), QStringLiteral("FLEX-8600"),
            QStringLiteral("AU-510"), QStringLiteral("AU-520")}) {
        f.identity(model, QStringLiteral("4.3.16"));
        check(f.request(DspParameter::NrfWindow, 1024), "8000/Aurora models support entitled NRF window");
    }
    f.identity(QStringLiteral("UNKNOWN"), QStringLiteral("4.3.16"));
    f.commands.clear();
    check(!f.request(DspParameter::NrDelay, 1) && f.commands.isEmpty(), "unknown hardware cannot authorize advanced writes");
    f.identity(QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16"));
    for (const int id : {-1, 4, std::numeric_limits<int>::max()}) {
        check(f.backend->requestDspParameter(id, {DspParameter::NrDelay, 1}) == ReceiveDispatch::Unsupported,
              "backend rejects invalid or unavailable slice identifiers");
    }
    f.license(false);
    check(f.backend->requestDspParameter(0, {DspParameter::NrDelay, 1}) == ReceiveDispatch::Unsupported,
          "backend rechecks entitlement instead of trusting GUI admission");

    SliceModel legacy(0);
    FlexBackend decoder;
    QObject::connect(&decoder, &IRadioBackend::sliceChanged, &legacy,
        [&legacy](int, const SliceDelta& delta) { legacy.applyChanges(delta); });
    decoder.decodeSliceStatus(0, {{QStringLiteral("nr_adapt_mode"), QStringLiteral("2")},
        {QStringLiteral("nr_isdft_mode"), QStringLiteral("1")},
        {QStringLiteral("anf_adapt_mode"), QStringLiteral("0")},
        {QStringLiteral("anf_isdft_mode"), QStringLiteral("2")}});
    check(legacy.dspParameters().isEmpty(), "unproved 4.2 numeric enum mappings remain unknown");
    decoder.decodeSliceStatus(0, {{QStringLiteral("nr_adapt_mode"), QStringLiteral("auto")},
        {QStringLiteral("nr_isdft_mode"), QStringLiteral("half")}});
    check(legacy.dspParameter(DspParameter::NrAdapt) == 0 && legacy.dspParameter(DspParameter::NrIsdft) == 1,
          "published 4.3 strings normalize to canonical indices without inferring legacy codes");

    f.license();
    f.commands.clear();
    check(f.backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nrf, SliceDspRequest::Field::Level, false, 50})
              == ReceiveDispatch::Unsupported && f.commands.isEmpty(), "verified current NRF contract refuses obsolete level writes");
    f.identity(QStringLiteral("FLEX-8600"), QStringLiteral("4.2.20.41343"));
    check(f.backend->requestSliceDsp(0, {SliceDspRequest::Feature::Nrf, SliceDspRequest::Field::Level, false, 50})
              == ReceiveDispatch::Dispatched && f.commands == QStringList{QStringLiteral("slice set 0 nrf_level=50")},
          "existing published 4.2 NRF setter is preserved outside the advanced editing contract");
}

void identitySessionAndMode()
{
    Fixture f;
    f.license();
    f.report();
    SliceModel stale(0);
    const quint64 session = f.radio.receiveControlSession();
    check(!f.radio.requestDspParameter(&stale, session, {DspParameter::NrDelay, 1}),
          "another object reusing a current slice id cannot issue an advanced request");
    RadioModelSliceLifecycleTestAccess::own(f.radio, false);
    check(!f.request(DspParameter::NrDelay, 1), "unconfirmed slice ownership fails closed");
    RadioModelSliceLifecycleTestAccess::own(f.radio, true);
    RadioModelSliceLifecycleTestAccess::foreign(f.radio, true);
    check(!f.request(DspParameter::NrDelay, 1), "foreign occupancy fails closed even with stale local ownership");
    RadioModelSliceLifecycleTestAccess::foreign(f.radio, false);
    f.slice()->setExternalReceiveAudioReplacementMute(true, false);
    check(!f.request(DspParameter::NrDelay, 1), "external receive replacement blocks radio-owned parameter edits");
    f.slice()->setExternalReceiveAudioReplacementMute(false, false);
    for (const QString& mode : {QStringLiteral("CW"), QStringLiteral("RTTY"), QStringLiteral("DIGU"),
            QStringLiteral("DIGL"), QStringLiteral("FDVU"), QStringLiteral("FDVL")}) {
        f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), mode}});
        check(f.request(DspParameter::NrDelay, 1) && !f.request(DspParameter::AnfDelay, 1)
                  && !f.request(DspParameter::AnflDelay, 1) && !f.request(DspParameter::AnftMinDb, -100),
              "confirmed mode permits reduction and refuses inapplicable notch parameters");
    }
    for (const QString& mode : {QStringLiteral("FM"), QStringLiteral("NFM"), QStringLiteral("DFM"),
            QStringLiteral("DSTR"), QStringLiteral("UNKNOWN"), QString()}) {
        f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), mode}});
        check(!f.request(DspParameter::NrDelay, 1), "unsupported receive mode cannot edit advanced parameters");
    }
    f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("USB")}});
    f.slice()->setMode(QStringLiteral("CW"));
    f.commands.clear();
    check(!f.request(DspParameter::NrDelay, 1), "optimistic mode change waits for radio confirmation");
    f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("USB")}});
    f.commands.clear();
    bool modelDispatched = true;
    ReceiveDispatch backendDispatched = ReceiveDispatch::Dispatched;
    SliceModel* source = f.slice();
    std::thread other([&] {
        modelDispatched = f.radio.requestDspParameter(source, session, {DspParameter::NrDelay, 1});
        backendDispatched = f.backend->requestDspParameter(0, {DspParameter::NrDelay, 1});
    });
    other.join();
    check(!modelDispatched && backendDispatched == ReceiveDispatch::Unsupported && f.commands.isEmpty(),
          "model and backend reject off-owner-thread requests synchronously");
    f.backend->connected = false;
    check(!f.request(DspParameter::NrDelay, 1), "disconnected radio cannot accept advanced edits");
    f.backend->connected = true;
    const quint64 beforeInvalidate = source->dspParametersRevision();
    QSignalSpy observations(source, &SliceModel::dspParametersChanged);
    source->invalidateFrequencyObservation();
    check(source->dspParameters().isEmpty() && source->dspParametersRevision() == beforeInvalidate + 1
              && observations.size() == 1 && !f.request(DspParameter::NrDelay, 1),
          "session observation invalidation clears every advanced value and invalidates drafts");
    f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("USB")}});
    check(!f.request(DspParameter::NrDelay, 1), "fresh mode alone cannot revive stale parameter readback");
    f.report();
    check(f.request(DspParameter::NrDelay, 1), "fresh session observations permit a new deliberate edit");
    f.commands.clear();
    RadioModelSliceLifecycleTestAccess::retire(f.radio);
    RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
    f.license();
    check(f.slice() == source && source->dspParameters().isEmpty(),
          "same-object reconnect retains identity but clears advanced observations");
    f.report();
    check(!f.radio.requestDspParameter(source, session, {DspParameter::NrDelay, 1}) && f.commands.isEmpty(),
          "captured old session cannot edit a reclaimed slice after fresh status");
    check(f.request(DspParameter::NrDelay, 1), "current session can make a new deliberate edit after reclaim");
    const quint64 beforeReplacement = f.radio.receiveControlSession();
    const QPointer<SliceModel> retired(source);
    f.radio.setBackendForTest(std::make_unique<ConnectedFlex>(), QStringLiteral("flex"));
    check(beforeReplacement != f.radio.receiveControlSession()
              && (!retired || !f.radio.requestDspParameter(retired.data(), beforeReplacement, {DspParameter::NrDelay, 1})),
          "backend replacement retires the control session and old source admission");
    SimBackend sim;
    check(!sim.capabilities().dspParameters
              && sim.requestDspParameter(0, {DspParameter::NrDelay, 1}) == ReceiveDispatch::Unsupported,
          "unsupported backend advertises no advanced feature and inherits explicit refusal");
}

void observationInvalidationLifetime()
{
    for (int notification = 0; notification < 3; ++notification) {
        QPointer<SliceModel> dying = new SliceModel(0);
        SliceDelta delta;
        delta.frequency = 14.2;
        delta.mode = QStringLiteral("USB");
        delta.dspParameters.insert(DspParameter::NrDelay, 25);
        dying->applyChanges(delta);
        const auto remove = [source = dying.data()] { delete source; };
        if (notification == 0) {
            QObject::connect(dying, &SliceModel::agcThresholdModeChanged, dying, remove);
        } else if (notification == 1) {
            QObject::connect(dying, &SliceModel::receiveObservationChanged, dying, remove);
        } else {
            QObject::connect(dying, &SliceModel::frequencyReported, dying, remove);
        }
        dying->invalidateFrequencyObservation();
        check(!dying, "session observation invalidation survives synchronous model retirement");
    }
}

void flushReadbacks()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
}

void acknowledgedReadback()
{
    Fixture f;
    f.license();
    f.report();
    const DspParameterValues before = f.slice()->dspParameters();
    check(f.request(DspParameter::NrDelay, 151) && f.replies.size() == 1
              && f.readbacks.isEmpty() && f.slice()->dspParameters() == before,
          "advanced setter dispatch waits for acceptance and retains authoritative observations");
    const FlexBackend::CommandReply reply = f.replies.takeFirst();
    const quint64 revision = f.slice()->dspParametersRevision();
    f.backend->decodeSliceStatus(0, {{QStringLiteral("nr_delay"), QStringLiteral("141")}});
    check(f.slice()->dspParametersRevision() == revision + 1 && f.readbacks.isEmpty(),
          "independent value status before the reply neither triggers nor cancels the accepted refresh");
    reply(0, {});
    reply(0, {});
    check(f.readbacks.isEmpty(), "acceptance schedules readback without a reentrant wire call");
    flushReadbacks();
    check(f.readbacks == QStringList{QStringLiteral("sub slice all")}
              && f.slice()->dspParameter(DspParameter::NrDelay) == 141,
          "duplicate successful replies cause exactly one read-only refresh and no guessed value");
    f.backend->decodeSliceStatus(0, {{QStringLiteral("nr_delay"), QStringLiteral("151")}});
    check(f.slice()->dspParameter(DspParameter::NrDelay) == 151
              && f.commands == QStringList{QStringLiteral("slice set 0 nr_delay=151")}
              && f.readbacks.size() == 1,
          "fresh radio readback alone confirms the edit without feedback or another refresh");
    f.readbacks.clear();
    check(f.request(DspParameter::NrDelay, 152), "a later explicit edit can request its own readback");
    const FlexBackend::CommandReply refused = f.replies.takeFirst();
    refused(0x50000001, QStringLiteral("refused"));
    refused(0, {});
    flushReadbacks();
    check(f.readbacks.isEmpty() && f.slice()->dspParameter(DspParameter::NrDelay) == 151,
          "a rejected first reply cannot be revived by a duplicate success or update local state");
}

void synchronousReplyDispatch()
{
    for (const bool dispatched : {false, true}) {
        Fixture f;
        f.license();
        f.report();
        int setters = 0;
        int refreshes = 0;
        bool refreshedInsideWriter = false;
        f.backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply reply) {
            if (command == QStringLiteral("sub slice all")) {
                ++refreshes;
                return true;
            }
            ++setters;
            reply(0, {});
            reply(0, {});
            flushReadbacks();
            refreshedInsideWriter = refreshes != 0;
            return dispatched;
        });
        check(f.request(DspParameter::NrDelay, 151) == dispatched && setters == 1
                  && !refreshedInsideWriter && refreshes == 0,
              "synchronous replies cannot refresh before the writer establishes dispatch outcome");
        flushReadbacks();
        check(refreshes == (dispatched ? 1 : 0)
                  && f.slice()->dspParameter(DspParameter::NrDelay) == 123,
              "synchronous success refreshes once only when actual dispatch succeeded");
    }
    Fixture f;
    f.license();
    f.report();
    int writes = 0;
    f.backend->setCommandSink(std::function<void(const QString&)>([&](const QString&) { ++writes; }));
    check(!f.request(DspParameter::NrDelay, 151) && writes == 0 && f.commands.isEmpty(),
          "encoding-only observers cannot masquerade as an answered advanced command sink");
    f.backend->setCommandSink(std::function<bool(const QString&, FlexBackend::CommandReply)>{});
    check(!f.request(DspParameter::NrDelay, 151) && f.commands.isEmpty(),
          "missing answered writer refuses advanced dispatch despite an ordinary slice sink");
}

void staleReadbackFences()
{
    for (int boundary = 0; boundary < 8; ++boundary) {
        Fixture f;
        f.license();
        f.report();
        check(f.request(DspParameter::NrDelay, 151), "current context admits the original advanced request");
        const FlexBackend::CommandReply reply = f.replies.takeFirst();
        const QPointer<SliceModel> original(f.slice());
        if (boundary == 0) {
            f.license(false);
        } else if (boundary == 1) {
            RadioModelSliceLifecycleTestAccess::own(f.radio, false);
        } else if (boundary == 2) {
            f.backend->connected = false;
        } else if (boundary == 3) {
            f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("FM")}});
        } else if (boundary == 4) {
            original->invalidateFrequencyObservation();
            f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("USB")}});
            f.report();
        } else if (boundary == 5) {
            RadioModelSliceLifecycleTestAccess::retire(f.radio);
            RadioModelSliceLifecycleTestAccess::reclaim(f.radio);
            f.license();
            f.report();
            check(f.slice() == original.data(), "reconnect can reclaim the exact source object before the old reply");
        } else if (boundary == 6) {
            f.radio.setBackendForTest(std::make_unique<ConnectedFlex>(), QStringLiteral("flex"));
        } else {
            RadioModelSliceLifecycleTestAccess::removeSlice(f.radio);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            f.backend->decodeSliceStatus(0, {{QStringLiteral("mode"), QStringLiteral("USB")},
                {QStringLiteral("nr_delay"), QStringLiteral("123")}});
            RadioModelSliceLifecycleTestAccess::own(f.radio, true);
            check(!original && f.slice(), "a deleted source id can be reused by a different slice object");
        }
        reply(0, {});
        flushReadbacks();
        check(f.readbacks.isEmpty(),
              "late accepted reply cannot refresh after entitlement, ownership, mode, source or session retirement");
    }

    for (const bool rebindBeforeReply : {false, true}) {
        Fixture f;
        f.license();
        f.report();
        int replacementWrites = 0;
        check(f.request(DspParameter::NrDelay, 151), "writer-rebind test dispatches one original setter");
        const FlexBackend::CommandReply reply = f.replies.takeFirst();
        if (!rebindBeforeReply) { reply(0, {}); }
        f.backend->setCommandSink([&](const QString&, FlexBackend::CommandReply) {
            ++replacementWrites;
            return true;
        });
        if (rebindBeforeReply) { reply(0, {}); }
        flushReadbacks();
        check(f.readbacks.isEmpty() && replacementWrites == 0,
              "answered writer rebinding fences both late replies and already queued readbacks");
    }
    Fixture f;
    f.license();
    f.report();
    check(f.request(DspParameter::NrDelay, 151), "void-writer rebind test dispatches original setter");
    const FlexBackend::CommandReply reply = f.replies.takeFirst();
    int replacementWrites = 0;
    f.backend->setCommandSink(std::function<void(const QString&)>([&](const QString&) { ++replacementWrites; }));
    reply(0, {});
    flushReadbacks();
    check(f.readbacks.isEmpty() && replacementWrites == 0,
          "encoding-only writer overload also retires outstanding advanced readback authority");
}

void reentrantReadbackLifetime()
{
    int refreshes = 0;
    FlexBackend::CommandReply late;
    auto fixture = std::make_unique<Fixture>();
    fixture->license();
    fixture->report();
    fixture->backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply reply) {
        if (reply) { late = std::move(reply); }
        if (command == QStringLiteral("sub slice all")) { ++refreshes; }
        return true;
    });
    check(fixture->request(DspParameter::NrDelay, 151), "model lifetime test dispatches the original setter");
    fixture.reset();
    late(0, {});
    flushReadbacks();
    check(refreshes == 0, "a callback retained beyond model and backend destruction cannot call its copied writer");

    auto configure = [](FlexBackend& backend) {
        backend.setModelProvider([] { return QStringLiteral("FLEX-8600"); });
        backend.setFirmwareVersionProvider([] { return QStringLiteral("4.3.16"); });
        backend.setDspResetLicenseProvider([]() -> std::optional<bool> { return true; });
    };
    {
        auto backend = std::make_unique<FlexBackend>();
        configure(*backend);
        backend->setCommandSink([&](const QString&, FlexBackend::CommandReply reply) {
            late = std::move(reply);
            return true;
        });
        bool retireInFence = false;
        const RadioControlFence current = [&] {
            if (retireInFence) { backend.reset(); }
            return true;
        };
        check(backend->requestDspParameter(0, {DspParameter::NrDelay, 151}, current) == ReceiveDispatch::Dispatched,
              "direct backend request retains its explicit composition fence");
        retireInFence = true;
        late(0, {});
        flushReadbacks();
        check(!backend, "refresh fence can synchronously destroy its backend without a stale member access");
    }
    {
        auto backend = std::make_unique<FlexBackend>();
        configure(*backend);
        backend->setCommandSink([&](const QString&, FlexBackend::CommandReply) { ++refreshes; return true; });
        FlexBackend* target = backend.get();
        const ReceiveDispatch result = target->requestDspParameter(0, {DspParameter::NrDelay, 151}, [&] {
            backend.reset();
            return true;
        });
        check(!backend && result == ReceiveDispatch::Unsupported && refreshes == 0,
              "pre-dispatch fence destruction refuses the setter without a stale member access");
    }
    {
        auto backend = std::make_unique<FlexBackend>();
        configure(*backend);
        int replacementWrites = 0;
        backend->setCommandSink([&](const QString&, FlexBackend::CommandReply reply) {
            late = std::move(reply);
            return true;
        });
        bool rebindInFence = false;
        const RadioControlFence current = [&] {
            if (rebindInFence) {
                backend->setCommandSink([&](const QString&, FlexBackend::CommandReply) {
                    ++replacementWrites;
                    return true;
                });
            }
            return true;
        };
        check(backend->requestDspParameter(0, {DspParameter::NrDelay, 151}, current) == ReceiveDispatch::Dispatched,
              "reentrant rebind test admits the original answered writer");
        rebindInFence = true;
        late(0, {});
        flushReadbacks();
        check(replacementWrites == 0, "a fence that rebinds the writer cannot refresh through either binding");
    }
    {
        Fixture f;
        f.license();
        f.report();
        int queries = 0;
        f.backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply reply) {
            if (command == QStringLiteral("sub slice all")) { ++queries; return true; }
            reply(0, {});
            f.radio.setBackendForTest(std::make_unique<ConnectedFlex>(), QStringLiteral("flex"));
            return true;
        });
        check(f.request(DspParameter::NrDelay, 151), "setter can synchronously retire its backend after acknowledgement");
        flushReadbacks();
        check(queries == 0, "a copied writer survives setter-time backend destruction and cannot refresh the replacement");
    }
}

void productionProviders()
{
    RadioModel radio;
    RadioModelSliceLifecycleTestAccess::identity(radio, QStringLiteral("FLEX-8600"), QStringLiteral("4.3.16"));
    check(radio.rebuildBackendForTest(QStringLiteral("flex")), "production setup constructs the Flex backend");
    auto* backend = qobject_cast<FlexBackend*>(radio.backend());
    if (!backend) { check(false, "production setup selected Flex"); return; }
    QStringList commands;
    backend->setSliceCommandSink([&](const QString& command) { commands.append(command); });
    backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply) {
        commands.append(command);
        return true;
    });
    check(backend->requestDspParameter(0, {DspParameter::NrDelay, 64}) == ReceiveDispatch::Unsupported,
          "production provider starts with unknown advanced entitlement");
    RadioModelSliceLifecycleTestAccess::license(radio, QStringLiteral("1"));
    check(backend->requestDspParameter(0, {DspParameter::NrDelay, 64}) == ReceiveDispatch::Dispatched
              && commands == QStringList{QStringLiteral("slice set 0 nr_delay=64")},
          "production setup wires normalized retained license and firmware providers");
    commands.clear();
    RadioModelSliceLifecycleTestAccess::license(radio, QStringLiteral("0"));
    check(backend->requestDspParameter(0, {DspParameter::NrDelay, 64}) == ReceiveDispatch::Unsupported
              && commands.isEmpty(), "production provider observes feature revocation at dispatch");
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dsp-advanced-parameters"));
    QCoreApplication app(argc, argv);
    exactEncodingAndReadback();
    boundsAndMalformedInput();
    firmwareModelAndLicense();
    identitySessionAndMode();
    productionProviders();
    observationInvalidationLifetime();
    acknowledgedReadback();
    synchronousReplyDispatch();
    staleReadbackFences();
    reentrantReadbackLifetime();
    return failures ? 1 : 0;
}
