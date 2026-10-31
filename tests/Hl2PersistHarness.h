#pragma once

// Shared plumbing for the HL2 persistence tests: one "session" is a real
// Hl2Backend that is restored, connected (no wire peer: TEST-NET-1, nothing
// answers) and brought through the link-up push, the way RadioModel brings it
// up on a reconnect. Capture, store and load go through the production
// currentOperatingState() and RadioStateMemory, so what is compared is what a
// restart would find on disk.

#include "core/AppSettings.h"
#include "core/RadioSettingsScope.h"
#include "core/RadioStateMemory.h"
#include "core/backends/hl2/Hl2Backend.h"

#include "TestDspBuildWait.h"

#include <QJsonObject>
#include <QSignalSpy>
#include <QString>

#include <cstdio>
#include <optional>

namespace AetherSDR::hl2 {

// Runs the link-up push without a radio: connectRadio() here never links.
struct Hl2DspReadbackTestAccess {
    static void pushInitialState(Hl2Backend& backend) { backend.pushInitialState(); }
};

}  // namespace AetherSDR::hl2

namespace AetherSDR::persist {

inline int g_failures = 0;

inline void check(bool ok, const QString& what)
{
    std::fprintf(stdout, "%s %s\n", ok ? "[ OK ]" : "[FAIL]", qPrintable(what));
    if (!ok)
        ++g_failures;
}

class Session {
public:
    hl2::Hl2Backend backend;
    QString panId;

    // `restored` empty means a radio with nothing remembered, which comes up on
    // `startHz`. A non-empty one comes up on what it remembers.
    Session(const QString& serial, const RestoredRadioState& restored, double startHz)
    {
        QObject::connect(&backend, &IRadioBackend::panCenterBandwidthChanged,
                         &backend, [this](const QString& id, double, double) { panId = id; });
        QSignalSpy finished(&backend, &hl2::Hl2Backend::dspSetupFinished);
        backend.applyRestoredState(restored);
        RadioConnectRequest request;
        request.host = QStringLiteral("192.0.2.1");   // TEST-NET-1, never routable
        request.port = 1024;
        request.serial = serial;
        request.params.insert(QStringLiteral("boardMaxRx"), 4);
        if (restored.isEmpty())
            request.params.insert(QStringLiteral("rxFrequencyHz"), startHz);
        backend.connectRadio(request);
        test::awaitDspBuild("hl2_persist", [&] { return !finished.isEmpty(); });
        hl2::Hl2DspReadbackTestAccess::pushInitialState(backend);
        // Publish the pan identity; tuning to where we already are moves nothing.
        backend.setSliceFrequency(0, backend.currentOperatingState().rfFrequencyHz);
    }
    ~Session() { backend.disconnectRadio(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    int liveLnaDb() const
    {
        return backend.healthSnapshot().values.value(QStringLiteral("lnaGainDb"), 999).toInt();
    }
    // The drive actually held for the band the session is on is what a capture
    // writes under that band's key.
    int liveDriveFor(const QString& band) const { return driveOf(backend.currentOperatingState(), band); }

    static int driveOf(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("txSetpoints")).toObject()
            .value(QStringLiteral("driveByBand")).toObject().value(band).toInt(-999);
    }
    static int lnaOf(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("rfGain")).toObject()
            .value(QStringLiteral("lnaDbByBand")).toObject().value(band).toInt(-999);
    }
    static bool hasDrive(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("txSetpoints")).toObject()
            .value(QStringLiteral("driveByBand")).toObject().contains(band);
    }
    static bool hasLna(const RestoredRadioState& s, const QString& band)
    {
        return s.extension.value(QStringLiteral("rfGain")).toObject()
            .value(QStringLiteral("lnaDbByBand")).toObject().contains(band);
    }
};

// store() then load() under the radio's scope: what the next launch reads.
inline RestoredRadioState throughDisk(const RadioSettingsScope& scope,
                                      const RadioCapabilities& caps,
                                      const RestoredRadioState& captured, bool* stored = nullptr)
{
    const bool ok = RadioStateMemory::store(scope, caps, captured);
    if (stored)
        *stored = ok;
    return RadioStateMemory::load(scope, caps);
}

}  // namespace AetherSDR::persist
