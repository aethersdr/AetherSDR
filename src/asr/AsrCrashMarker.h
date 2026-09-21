#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>
#include <utility>

namespace AetherSDR {

// Surviving an ASR fault nothing in the process can catch (#5190).
//
// ggml device discovery and the whisper model load can take the process down
// with a signal — an illegal instruction on a CPU the ggml build does not
// support (#4509), a null dereference after a failed GPU allocation (#4972).
// No catch block sees those, so the only place they can be detected is the next
// launch: before entering either stage the controller persists an "attempt in
// flight" marker and clears it when the stage completes. A marker still present
// at startup means the previous run died inside that stage.
//
// A surviving marker becomes a *fault record*, which is what stands the risky
// configuration down — and keeps doing so on later launches — until the app
// version changes, the device at that index changes, or the operator asks for
// another attempt. The record ACCUMULATES: a second GPU that dies keeps the
// first one retired (asrMergeFault), so a machine whose every GPU faults walks
// GPU -> GPU -> CPU -> off and stops, instead of alternating between two devices
// forever. All of it is plain fields inside the one CopyAssist settings document
// (Principle V), so a support bundle carries it for free.
//
// Everything here is pure and whisper-free so the decision is unit testable
// without staging a crash.

inline constexpr const char* kAsrStageDiscovery = "discovery";
inline constexpr const char* kAsrStageLoad = "load";

// A GPU an earlier fault already condemned (kept when a later one is recorded).
struct AsrRetiredDevice {
    int device = -1;
    QString name;
    bool operator==(const AsrRetiredDevice& o) const { return device == o.device && name == o.name; }
};

// What was being attempted. `device` is the gpu_device index, or -1 for CPU
// (always -1 for the discovery stage, which has no single device).
struct AsrAttempt {
    QString stage;
    int device = -1;
    QString deviceName;
    QString tier;
    quint64 vramFreeMb = 0;
    quint64 vramTotalMb = 0;
    QString appVersion;
    QString startedUtc; // ISO 8601
    // Fault records only: GPUs condemned by EARLIER faults under this same app
    // version. Empty on a marker.
    QList<AsrRetiredDevice> retired;

    bool isValid() const { return !stage.isEmpty(); }
};

inline QString asrAttemptToJson(const AsrAttempt& a)
{
    if (!a.isValid()) {
        return QString();
    }
    QJsonObject o;
    o.insert(QStringLiteral("stage"), a.stage);
    o.insert(QStringLiteral("device"), a.device);
    o.insert(QStringLiteral("deviceName"), a.deviceName);
    o.insert(QStringLiteral("tier"), a.tier);
    o.insert(QStringLiteral("vramFreeMb"), static_cast<qint64>(a.vramFreeMb));
    o.insert(QStringLiteral("vramTotalMb"), static_cast<qint64>(a.vramTotalMb));
    o.insert(QStringLiteral("appVersion"), a.appVersion);
    o.insert(QStringLiteral("startedUtc"), a.startedUtc);
    if (!a.retired.isEmpty()) {
        QJsonArray arr;
        for (const AsrRetiredDevice& r : a.retired) {
            QJsonObject ro;
            ro.insert(QStringLiteral("device"), r.device);
            ro.insert(QStringLiteral("name"), r.name);
            arr.append(ro);
        }
        o.insert(QStringLiteral("retired"), arr);
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// Anything unparseable, or without a known stage, reads as "no attempt": a
// damaged field must never stand a working configuration down.
inline AsrAttempt asrAttemptFromJson(const QString& json)
{
    AsrAttempt a;
    if (json.isEmpty()) {
        return a;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        return a;
    }
    const QJsonObject o = doc.object();
    const QString stage = o.value(QStringLiteral("stage")).toString();
    if (stage != QLatin1String(kAsrStageDiscovery) && stage != QLatin1String(kAsrStageLoad)) {
        return a;
    }
    a.stage = stage;
    a.device = o.value(QStringLiteral("device")).toInt(-1);
    a.deviceName = o.value(QStringLiteral("deviceName")).toString();
    a.tier = o.value(QStringLiteral("tier")).toString();
    a.vramFreeMb = static_cast<quint64>(o.value(QStringLiteral("vramFreeMb")).toInteger(0));
    a.vramTotalMb = static_cast<quint64>(o.value(QStringLiteral("vramTotalMb")).toInteger(0));
    a.appVersion = o.value(QStringLiteral("appVersion")).toString();
    a.startedUtc = o.value(QStringLiteral("startedUtc")).toString();
    const QJsonArray arr = o.value(QStringLiteral("retired")).toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject ro = v.toObject();
        AsrRetiredDevice r;
        r.device = ro.value(QStringLiteral("device")).toInt(-1);
        r.name = ro.value(QStringLiteral("name")).toString();
        if (r.device >= 0 && !a.retired.contains(r)) {
            a.retired.append(r);
        }
    }
    return a;
}

// The marker to persist once a load aimed at a GPU is about to run on CPU
// instead (WhisperAsrBackend::load() retries there after a CAUGHT GPU failure
// that latched the device, and starts there when it is not entering the GPU it
// was aimed at). From that moment a death is a CPU death — the
// #4972-then-#4509 sequence — and must classify as DisableAsr, not RetireGpu:
// retiring the GPU would send the next launch straight back onto the CPU path
// that killed this one. The GPU's own failure was caught, so it needs no record.
// Anything that is not a load marker passes through untouched.
inline AsrAttempt asrAttemptOnCpuFallback(const AsrAttempt& armed)
{
    AsrAttempt a = armed;
    if (a.stage != QLatin1String(kAsrStageLoad)) {
        return a;
    }
    a.device = -1;
    a.deviceName.clear();
    a.vramFreeMb = 0;
    a.vramTotalMb = 0;
    return a;
}

// The same, on the persisted field's text — what the controller's CPU-fallback
// hook hands to CopyAssistSettings::updateValue(). Empty or unparseable in,
// empty out: nothing armed must never become a marker.
inline QString asrMarkerJsonOnCpuFallback(const QString& json)
{
    return asrAttemptToJson(asrAttemptOnCpuFallback(asrAttemptFromJson(json)));
}

// The record to keep when a marker survived (`died`) and an older record may
// already exist (`previous`). The new fault becomes the record's subject; a GPU
// the previous record condemned stays condemned, as long as both come from the
// same app version (an upgrade wipes the slate — see asrFaultAction's Forget).
// Without this the record would name only the LATEST device, and two faulting
// GPUs would hand the decode back and forth on every launch.
inline AsrAttempt asrMergeFault(const AsrAttempt& previous, const AsrAttempt& died)
{
    AsrAttempt merged = died;
    merged.retired.clear();
    if (!died.isValid() || !previous.isValid() || previous.appVersion != died.appVersion) {
        return merged;
    }
    QList<AsrRetiredDevice> carry = previous.retired;
    if (previous.stage == QLatin1String(kAsrStageLoad) && previous.device >= 0) {
        carry.append({previous.device, previous.deviceName});
    }
    for (const AsrRetiredDevice& r : carry) {
        const bool isSubject = (r.device == merged.device && r.name == merged.deviceName);
        if (r.device >= 0 && !isSubject && !merged.retired.contains(r)) {
            merged.retired.append(r);
        }
    }
    return merged;
}

// Which markers are persisted right now, and when each may be cleared. Pure so
// the two rules that are easy to get wrong are unit tested rather than read:
//  - discovery and the load own SEPARATE slots. A probe that timed out is still
//    running when the operator's load is armed, and neither may clear — or
//    overwrite — the other's marker;
//  - loads queue on the one ASR worker thread, so a tier change during a load
//    puts a second one behind it: only the LAST outstanding load clears.
// Each method returns true when the caller must write (arm*) or clear the
// corresponding persisted field.
class AsrMarkerState {
public:
    bool armDiscovery()
    {
        m_discovery = true;
        return true;
    }
    bool discoveryFinished()
    {
        const bool wasArmed = m_discovery;
        m_discovery = false;
        return wasArmed;
    }
    bool armLoad()
    {
        ++m_loads;
        m_loadPersisted = true;
        return true; // rewritten each time: the newest attempt is the one to describe
    }
    bool loadSettled() // one queued load reported ready or failed
    {
        if (m_loads > 0) {
            --m_loads;
        }
        return m_loads == 0 && std::exchange(m_loadPersisted, false);
    }
    bool engineTornDown() // the worker was joined: nothing queued can still run
    {
        m_loads = 0;
        return std::exchange(m_loadPersisted, false);
    }
    bool discoveryArmed() const { return m_discovery; }
    int loadsInFlight() const { return m_loads; }

private:
    bool m_discovery = false;
    bool m_loadPersisted = false;
    int m_loads = 0;
};

enum class AsrFaultAction {
    None,       // no record: run normally
    AwaitDevices, // a GPU fault whose device cannot be checked until discovery has run
    Forget,     // the record is stale (other version / other device): drop it, run normally
    RetireGpu,  // that GPU took the process down: keep it out, decode elsewhere
    DisableAsr, // ggml itself cannot run here: no local speech engine this session
};

// What a fault record calls for on THIS launch.
//
// `currentDeviceName` is the description of the device now at the record's
// index: nullopt while discovery has not run yet, empty when no device sits at
// that index any more. The remedies differ in kind because the faults do:
//  - a GPU load that died condemns that GPU, and CPU is a real fallback;
//  - a death in discovery, or in a load that was already on CPU, means ggml
//    cannot run on this machine at all — forcing CPU would only die again, so
//    the local engine stays off (#4509 shape).
// A record from another app version is dropped rather than honoured: a new
// build ships a new whisper/ggml, so the attempt deserves to be made again. So
// is one whose GPU index now names different hardware.
inline AsrFaultAction asrFaultAction(const AsrAttempt& fault, const QString& currentAppVersion,
                                     const std::optional<QString>& currentDeviceName)
{
    if (!fault.isValid()) {
        return AsrFaultAction::None;
    }
    if (fault.appVersion != currentAppVersion) {
        return AsrFaultAction::Forget;
    }
    if (fault.stage == QLatin1String(kAsrStageDiscovery) || fault.device < 0) {
        return AsrFaultAction::DisableAsr;
    }
    if (!currentDeviceName.has_value()) {
        return AsrFaultAction::AwaitDevices;
    }
    if (currentDeviceName->isEmpty() || *currentDeviceName != fault.deviceName) {
        return AsrFaultAction::Forget;
    }
    return AsrFaultAction::RetireGpu;
}

} // namespace AetherSDR
