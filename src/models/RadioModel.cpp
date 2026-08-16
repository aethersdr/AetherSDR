#include "RadioModel.h"
#include "core/GuiClientIdentityPolicy.h"
#include "AntennaAliasStore.h"
#include "BandDefs.h"
#include "BandSettings.h"
#include "DeclaredBands.h"
#include "core/CommandParser.h"
#include "core/backends/flex/FlexBackend.h"   // aetherd RFC 2.2 radio-facing seam
#include "core/backends/sim/SimBackend.h"     // RFC #4288 demo-mode backend (Route A)
#include "core/backends/hl2/Hl2Backend.h"      // aetherd Gap A — HL2 backend (family "hl2")
#include "core/backends/icom/IcomCivBackend.h"  // Icom networked radios (family "icom")
#include "core/backends/icom/IcomCredentials.h"  // password: keychain, never settings
#include "core/backends/icom/IcomSettings.h"     // host/user/ports (Principle V)
#include "core/AppSettings.h"
#include "core/RadioStateMemory.h"  // RFC #4603 typed restore handoff
#include "core/ShutdownTrace.h"
#include "core/CwTrace.h"
#include "core/DigitalVoiceModeRegistry.h"
#include "core/DigitalVoiceWaveformProcess.h"
#include "core/LogManager.h"
#include "core/MemoryFieldValues.h"
#include "core/backends/MemoryWireCodec.h"
#include "core/PerfTelemetry.h"
#include "core/StreamStatus.h"
#include "core/UdpRegistrationPolicy.h"
#include "core/WaterfallRate.h"
#include "ProfileLoadCommand.h"
#include "RadioStatusOwnership.h"
#include "SliceRecreatePolicy.h"
#include "TransmitInhibitPolicy.h"
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDateTime>
#include <QFileInfo>
#include <QSysInfo>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace AetherSDR {

namespace {

// aetherd Gap B (Step 2): synthetic panadapter stream-id base for backends whose
// spectra arrive via IRadioBackend::spectrumFrameReady (HL2). Kept well clear of
// the radio-assigned Flex stream-ids. streamId = base + panId. (Step 3 will
// namespace this by session so ids stay unique across concurrent radios.)
constexpr quint32 kNeutralPanStreamIdBase = 0xE1000000u;
// Internal sentinel for a command issued to a backend that owns no command
// plane. Not a SmartSDR protocol response code; numbered alongside
// kProfileLoadSuppressedCommandCode (0x50000061), the other such drop.
constexpr int kNoCommandPlaneCode = 0x50000063;
// Waterfall ids must be distinct from pan ids: the UI routes waterfall rows by
// PanadapterModel::wfStreamId() and spectrum frames by panStreamId().
constexpr quint32 kNeutralWfStreamIdBase  = 0xE2000000u;

// PanadapterModel derives its numeric stream id by parsing panId/waterfallId
// (base-0, so a "0x" prefix is honoured). Build ids in that form.
QString neutralPanIdString(int panIdx)
{
    return QStringLiteral("0x%1").arg(kNeutralPanStreamIdBase + static_cast<quint32>(panIdx),
                                      8, 16, QLatin1Char('0'));
}
QString neutralWfIdString(int panIdx)
{
    return QStringLiteral("0x%1").arg(kNeutralWfStreamIdBase + static_cast<quint32>(panIdx),
                                      8, 16, QLatin1Char('0'));
}

// A pan index that cannot collide with a real one, for the "no id at all" case.
constexpr int kNeutralPanIndexNone = -1;

constexpr int kDefaultPanDimensionThreshold = 100;
constexpr int kSessionRestorePruneDelayMs = 5000;

// parseDeclaredBands() moved to DeclaredBands.{h,cpp} so the Principle-VII
// validation (allow-list against BandDefs, dedup, case-fold) has a light,
// dependency-free test target (declared_bands_test). Behaviour unchanged.

QString normalizedLicenseFeatureName(const QString& name)
{
    return name.trimmed().toLower();
}

QJsonArray toJsonArray(const QStringList& values)
{
    QJsonArray array;
    for (const QString& value : values)
        array.append(value);
    return array;
}

QJsonArray toJsonArray(const QVector<int>& values)
{
    QJsonArray array;
    for (int value : values)
        array.append(value);
    return array;
}

QJsonArray toJsonArray(const QSet<int>& values)
{
    QList<int> sorted = values.values();
    std::sort(sorted.begin(), sorted.end());

    QJsonArray array;
    for (int value : sorted)
        array.append(value);
    return array;
}

QString atuStatusToString(ATUStatus status)
{
    switch (status) {
    case ATUStatus::None:         return "None";
    case ATUStatus::NotStarted:   return "NotStarted";
    case ATUStatus::InProgress:   return "InProgress";
    case ATUStatus::Bypass:       return "Bypass";
    case ATUStatus::Successful:   return "Successful";
    case ATUStatus::OK:           return "OK";
    case ATUStatus::FailBypass:   return "FailBypass";
    case ATUStatus::Fail:         return "Fail";
    case ATUStatus::Aborted:      return "Aborted";
    case ATUStatus::ManualBypass: return "ManualBypass";
    }
    return "Unknown";
}

bool statusFlagSet(const QMap<QString, QString>& kvs, const QString& key)
{
    const QString value = kvs.value(key).trimmed();
    return value == QStringLiteral("1")
        || value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

// isProfileOwnedRadioStateWrite() moved to ProfileLoadCommand.h so the
// classification contract has a light, dependency-free test target
// (profile_load_command_test). Behaviour unchanged. (#4142)

void appendUniqueAntennaToken(QStringList& tokens, const QString& token)
{
    if (!token.isEmpty() && !tokens.contains(token))
        tokens.append(token);
}

QString cleanClientText(QString value)
{
    value.replace(QChar(0x7f), QLatin1Char(' '));
    return value.trimmed();
}

quint32 parseClientHandle(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        text = text.mid(2);

    bool ok = false;
    const quint32 handle = text.toUInt(&ok, 16);
    return ok ? handle : 0;
}

// parseStreamToken — identical to parseStatusHandle; use the shared version.
inline quint32 parseStreamToken(QString text) { return parseStatusHandle(std::move(text)); }

QString hexId(quint32 value)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(value, 16).rightJustified(8, QLatin1Char('0')));
}

QString hexCode(int value)
{
    return QStringLiteral("0x%1")
        .arg(QString::number(static_cast<quint32>(value), 16).rightJustified(8, QLatin1Char('0')));
}

QString normalizePanadapterId(QString text)
{
    const QString trimmed = text.trimmed();
    const QString normalized = RadioStatusOwnership::normalizedFlexId(trimmed);
    return normalized.isEmpty() ? trimmed : normalized;
}

QString parsePanadapterCreateId(const QString& body)
{
    return RadioStatusOwnership::parsePanafallCreatePanId(body);
}

struct StreamObjectParts {
    bool valid{false};
    quint32 streamId{0};
    QString action;
};

StreamObjectParts parseStreamObject(const QString& object, const QString& prefix)
{
    if (!object.startsWith(prefix + QLatin1Char(' ')))
        return {};

    const QString rest = object.mid(prefix.size() + 1).trimmed();
    const int firstSpace = rest.indexOf(QLatin1Char(' '));
    const QString idText = firstSpace >= 0 ? rest.left(firstSpace) : rest;

    StreamObjectParts parts;
    parts.streamId = parseStreamToken(idText);
    parts.valid = parts.streamId != 0;
    if (firstSpace >= 0)
        parts.action = rest.mid(firstSpace + 1).trimmed();
    return parts;
}

bool isDaxStreamType(const QString& type)
{
    return type == QStringLiteral("dax_rx")
        || type == QStringLiteral("dax_tx")
        || type == QStringLiteral("dax_mic")
        || type == QStringLiteral("dax_iq");
}

bool streamStatusRemoved(const StreamObjectParts& stream,
                         const QMap<QString, QString>& kvs)
{
    return stream.action == QStringLiteral("removed")
        || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QStringLiteral("0");
}

bool looksLikeClientId(const QString& value)
{
    static const QRegularExpression guidRe(
        QStringLiteral(R"(^\{?[0-9A-Fa-f]{8}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{4}-?[0-9A-Fa-f]{12}\}?$)"));
    return guidRe.match(value.trimmed()).hasMatch();
}

QString clientConnectionSource(const QMap<QString, QString>& kvs)
{
    const QStringList keys = {
        QStringLiteral("ip"),
        QStringLiteral("client_ip"),
        QStringLiteral("remote_ip"),
        QStringLiteral("name")
    };

    for (const QString& key : keys) {
        const QString value = cleanClientText(kvs.value(key));
        if (!value.isEmpty() && !looksLikeClientId(value))
            return value;
    }

    return {};
}

bool isRoutineClientConnectionInfo(const QString& text)
{
    static const QRegularExpression clientInfoRe(
        QStringLiteral(R"(^Client\s+(?:connected|disconnected)\s+from\s+IP\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return clientInfoRe.match(text.trimmed()).hasMatch();
}

QJsonObject panToJson(const PanadapterModel* pan, const QString& activePanId)
{
    QJsonObject obj;
    obj["pan_id"] = pan->panId();
    obj["active"] = pan->panId() == activePanId;
    obj["waterfall_id"] = pan->waterfallId();
    obj["center_mhz"] = pan->centerMhz();
    obj["bandwidth_mhz"] = pan->bandwidthMhz();
    obj["min_dbm"] = pan->minDbm();
    obj["max_dbm"] = pan->maxDbm();
    obj["antennas"] = toJsonArray(pan->antList());
    obj["rf_gain"] = pan->rfGain();
    obj["rf_gain_low"] = pan->rfGainLow();
    obj["rf_gain_high"] = pan->rfGainHigh();
    obj["rf_gain_step"] = pan->rfGainStep();
    obj["preamp"] = pan->preamp();
    obj["wnb_active"] = pan->wnbActive();
    obj["wnb_level"] = pan->wnbLevel();
    obj["resized"] = pan->isResized();
    obj["waterfall_configured"] = pan->isWaterfallConfigured();
    return obj;
}

QJsonObject panSliceConnectionStatus(const QJsonObject& pan, const QJsonArray& slices)
{
    const QString panId = pan["pan_id"].toString();
    QVector<int> connectedSliceIds;
    QVector<int> activeSliceIds;
    QVector<int> txSliceIds;

    for (const QJsonValue& value : slices) {
        const QJsonObject slice = value.toObject();
        if (slice["pan_id"].toString() != panId || !slice["slice_id"].isDouble())
            continue;

        const int sliceId = slice["slice_id"].toInt();
        connectedSliceIds.append(sliceId);
        if (slice["active"].toBool())
            activeSliceIds.append(sliceId);
        if (slice["tx_slice"].toBool())
            txSliceIds.append(sliceId);
    }

    QJsonObject status;
    status["pan_id"] = panId;
    status["connected_slice_ids"] = toJsonArray(connectedSliceIds);
    status["active_slice_ids"] = toJsonArray(activeSliceIds);
    status["tx_slice_ids"] = toJsonArray(txSliceIds);
    status["connected_slice_count"] = connectedSliceIds.size();
    status["active_slice_count"] = activeSliceIds.size();
    status["has_connected_slice"] = !connectedSliceIds.isEmpty();
    status["has_active_slice"] = !activeSliceIds.isEmpty();
    status["has_tx_slice"] = !txSliceIds.isEmpty();

    if (connectedSliceIds.isEmpty()) {
        status["state"] = QStringLiteral("no_slice_connected");
        status["attention_required"] = true;
        status["summary"] = QStringLiteral("No slice connected.");
        status["possible_issue"] =
            QStringLiteral("Panadapter exists but no SliceModel references it; the slice may have been closed, failed to attach, or fallen out of the app cache.");
    } else if (activeSliceIds.size() > 1) {
        status["state"] = QStringLiteral("multiple_active_slices_connected");
        status["attention_required"] = true;
        status["summary"] = QStringLiteral("Multiple active slices are connected to this panadapter.");
        status["possible_issue"] =
            QStringLiteral("More than one connected slice is marked active for the same panadapter.");
    } else if (activeSliceIds.isEmpty()) {
        status["state"] = QStringLiteral("slice_connected_no_active");
        status["attention_required"] = false;
        status["summary"] = QStringLiteral("Slice connected, but none of the connected slices is currently active.");
    } else {
        status["state"] = QStringLiteral("active_slice_connected");
        status["attention_required"] = false;
        status["summary"] = QStringLiteral("Active slice connected.");
    }

    return status;
}

QJsonObject slicePanadapterConnectionStatus(int sliceId,
                                            const QString& panId,
                                            bool panadapterPresent,
                                            bool activePanadapter)
{
    QJsonObject status;
    status["slice_id"] = sliceId;
    status["pan_id"] = panId;
    status["panadapter_present"] = panadapterPresent;
    status["active_panadapter"] = activePanadapter;

    if (panId.trimmed().isEmpty()) {
        status["state"] = QStringLiteral("no_panadapter_id");
        status["attention_required"] = true;
        status["summary"] = QStringLiteral("Slice has no panadapter id.");
        status["possible_issue"] =
            QStringLiteral("The slice exists in the app cache without a panadapter association.");
    } else if (!panadapterPresent) {
        status["state"] = QStringLiteral("panadapter_missing");
        status["attention_required"] = true;
        status["summary"] = QStringLiteral("Slice references a panadapter that is not currently tracked.");
        status["possible_issue"] =
            QStringLiteral("The linked panadapter may have closed, crashed, or failed to create before the slice cache was updated.");
    } else if (!activePanadapter) {
        status["state"] = QStringLiteral("panadapter_connected_inactive");
        status["attention_required"] = false;
        status["summary"] = QStringLiteral("Slice is connected to a tracked, inactive panadapter.");
    } else {
        status["state"] = QStringLiteral("active_panadapter_connected");
        status["attention_required"] = false;
        status["summary"] = QStringLiteral("Slice is connected to the active panadapter.");
    }

    return status;
}

QJsonObject xvtrToJson(const RadioModel::XvtrInfo& xvtr)
{
    QJsonObject obj;
    obj["index"] = xvtr.index;
    obj["order"] = xvtr.order;
    obj["name"] = xvtr.name;
    obj["rf_freq_mhz"] = xvtr.rfFreq;
    obj["if_freq_mhz"] = xvtr.ifFreq;
    obj["offset_mhz"] = xvtr.rfFreq - xvtr.ifFreq;
    obj["lo_error"] = xvtr.loError;
    obj["rx_gain"] = xvtr.rxGain;
    obj["max_power"] = xvtr.maxPower;
    obj["rx_only"] = xvtr.rxOnly;
    obj["is_valid"] = xvtr.isValid;
    obj["has_is_valid"] = xvtr.hasIsValid;
    return obj;
}

QJsonObject clientInfoToJson(quint32 handle,
                             quint32 ourHandle,
                             quint32 txHandle,
                             const RadioModel::ClientInfo& info)
{
    QJsonObject obj;
    obj["role"] = (handle == ourHandle) ? "current_app" : "other_client";
    obj["owns_tx"] = (txHandle != 0 && handle == txHandle);
    obj["program"] = info.program;
    obj["source"] = info.source;
    obj["local_ptt"] = info.localPtt;
    obj["tx_antenna"] = info.txAntenna;
    obj["tx_freq_mhz"] = info.txFreqMhz;
    return obj;
}

} // namespace

// aetherd Gap A (HL2 Phase 1c): the minimal backend-selection seam. Maps a radio
// family string to its IRadioBackend. "flex" (the default, and any unrecognized
// value) preserves the historical hard-wired FlexBackend; "hl2" selects the
// Hermes-Lite 2 backend and "icom" the Icom networked-radio backend. A fuller
// step-3 registry supersedes this later.

// RFC #4603 proposal B: hand the remembered operating state to a backend
// whose declared ClientSettingsDomains make the client its memory. Called
// immediately before connectRadio(); a backend with an empty declaration
// (Flex, Sim) never sees restored state — the radio-authoritative policy
// (Constitution II/III) is structurally untouched.
// RFC #4603 PR 3: the debounced store write. Gated by the same predicate as
// the restore path — capability-shaped, never family-shaped. `force` is the
// disconnect-flush path: isConnected() is already false there, but the
// backend still holds the session's final state and the scope still resolves
// to that radio's identity.
void RadioModel::persistOperatingState(bool force)
{
    m_operatingStateSaveTimer.stop();
    m_operatingStateMaxWaitTimer.stop();
    if (!m_backend || (!force && !isConnected())) {
        return;
    }
    const RadioCapabilities caps = m_backend->capabilities();
    if (!RadioStateMemory::shouldEngage(caps)) {
        return;
    }
    RadioStateMemory::store(settingsScope(), caps,
                            m_backend->currentOperatingState());
}

void RadioModel::flushPendingOperatingState()
{
    // Only when something is actually pending: a disconnect with no unsaved
    // edits must not rewrite the document (and a Flex/Sim backend never has
    // a pending timer to begin with).
    if (!m_operatingStateSaveTimer.isActive()
        && !m_operatingStateMaxWaitTimer.isActive()) {
        return;
    }
    persistOperatingState(true);
}

void RadioModel::invokeBackendExtension(const QString& ns, const QString& verb,
                                        quint64 requestId, const QVariant& arg)
{
    if (!m_backend) {
        return;
    }
    m_backend->invokeExtension(ns, verb, requestId, arg);
}

void RadioModel::setPcAudioEnabled(bool on)
{
    if (!m_backend || m_backend->capabilities().family != QLatin1String("icom")) {
        return;
    }
    m_backend->invokeExtension(QStringLiteral("icom"),
                               QStringLiteral("audio.pc"), 0, on);
}

void RadioModel::notePcAudioEnabled(bool on)
{
    if (!m_backend || m_backend->capabilities().family != QLatin1String("icom")) {
        return;
    }
    m_backend->invokeExtension(QStringLiteral("icom"),
                               QStringLiteral("audio.pc.state"), 0, on);
}

void RadioModel::handRestoredStateToBackend(const QString& serial)
{
    if (!m_backend) {
        return;
    }
    const RadioCapabilities caps = m_backend->capabilities();
    if (!RadioStateMemory::shouldEngage(caps)) {
        return;
    }
    // Stop any capture pending from a previous same-family session: the
    // flush-on-disconnect already persisted it under the OLD radio's scope,
    // and a stale timer crossing the swap would write radio A's snapshot
    // under radio B's identity (PR #4619 review, Ozy311 finding 1).
    m_operatingStateSaveTimer.stop();
    m_operatingStateMaxWaitTimer.stop();

    const RestoredRadioState state =
        RadioStateMemory::load(RadioSettingsScope(m_family, serial), caps);
    // UNCONDITIONALLY — an empty state is the reset that stops a same-family
    // backend reuse leaking radio A's maps and live members into radio B
    // ("this radio has no memory" is information, not a no-op).
    m_backend->applyRestoredState(state);
}


// Family-specific connect parameters.
//
// The neutral RadioConnectRequest carries host/port/serial; anything a family
// needs beyond that rides in `params`, namespaced by family. Icom is the first
// family that cannot connect without a credential, so this is where the
// username (settings) and password (keychain session cache) are attached.
//
// SYNCHRONOUS by necessity: this runs on the connect path, including from the
// auto-reconnect timer, and a keychain read is async. IcomCredentials keeps a
// process-lifetime session cache primed by the connect dialog precisely so this
// call cannot block on the keyring — see its header.
static void populateFamilyParams(RadioConnectRequest& req, const QString& family)
{
    if (family.compare(QLatin1String("icom"), Qt::CaseInsensitive) != 0)
        return;
    req.params.insert(QStringLiteral("icom.username"), IcomSettings::username());
    req.params.insert(QStringLiteral("icom.password"), IcomCredentials::sessionPassword());
    req.params.insert(QStringLiteral("icom.serialPort"), IcomSettings::serialPort());
    req.params.insert(QStringLiteral("icom.audioPort"), IcomSettings::audioPort());
    // THE CI-V ADDRESS IS OMITTED WHEN NOBODY CHOSE ONE, and the omission is the
    // signal. Before this, a blank field was written back as kDefaultCivAddress
    // and arrived here indistinguishable from a deliberate IC-705 pick, so the
    // backend had nothing to branch on and every unconfigured radio was
    // addressed at 0xA4 — correct for one model in the table and silently
    // ignored by all the others. IcomSettings::kDefaultCivAddress exists to make
    // exactly this distinction; it had no way to reach the backend.
    //
    // Pinned says whether the wire may correct it: a picked MODEL is a shortcut
    // for an address and may be corrected, a typed CUSTOM address is a device
    // selection on a possibly-shared bus and may not. See IcomSettings.h.
    switch (IcomSettings::civSelection()) {
    case IcomSettings::CivSelection::Auto:
        break;
    case IcomSettings::CivSelection::Model:
        req.params.insert(QStringLiteral("icom.civAddress"), IcomSettings::civAddress());
        break;
    case IcomSettings::CivSelection::Custom:
        req.params.insert(QStringLiteral("icom.civAddress"), IcomSettings::civAddress());
        req.params.insert(QStringLiteral("icom.civAddressPinned"), true);
        break;
    }

    // LOW BANDWIDTH MODE REACHES THIS FAMILY NOW.
    //
    // The connect dialog has offered the checkbox all along and it did nothing
    // here — `LowBandwidthConnect` was read on the Flex path only, so the one
    // control an operator reaches for on a weak link was inert on the family
    // that needs it most. An Icom session is uncompressed LPCM in both
    // directions: 768 kbps each way at 48 kHz, and a measured link starves at
    // that rate — transmit audio stops reaching the modulator for seconds and
    // the CI-V replies sharing the link stretch from 200 ms to 1.4 s.
    //
    // 16 kHz is a third of the traffic and costs nothing audible: SSB is a
    // 3 kHz passband and FT8 is one tone.
    // NOT WIRED YET, and deliberately: see below.
    //
    // Lowering the rate alone BREAKS TRANSMIT. kAudioFrameBytes is 1920, which
    // is 20 ms at 48 kHz and 60 ms at 16 kHz — the frame SIZE is fixed while
    // its DURATION scales with the rate, so the radio's jitter buffer sees
    // frames at a third of the cadence it expects and discards them. Measured:
    // zero forward power for a full 20 s key, nothing audible on a receiver
    // beside the radio, and no trace on the radio's own panadapter, while the
    // CI-V link stayed healthy at 255 ms meter ages.
    //
    // The framing has to scale with the rate before this can be offered, and
    // the 1364/556 packet split is 48 kHz-shaped too. Until then a 48 kHz
    // session that occasionally stutters beats a 16 kHz one that cannot
    // transmit at all.
}

std::unique_ptr<IRadioBackend> RadioModel::makeBackend(const QString& family)
{
    if (family.compare(QLatin1String("hl2"), Qt::CaseInsensitive) == 0)
        return std::make_unique<hl2::Hl2Backend>();
    // Icom networked radios (IC-705, IC-7300MK2, …). Like HL2 this is a pure
    // seam backend — it owns no RadioConnection and no PanadapterStream, so the
    // dynamic_cast chain in setupBackend() correctly skips it and every model
    // update arrives as a normalized delta.
    //
    // The radio here is authoritative about its own operating state, so unlike
    // HL2 it declares NO ClientSettingsDomains and must never be pushed a
    // restored state (Constitution II/III).
    if (family.compare(QLatin1String("icom"), Qt::CaseInsensitive) == 0)
        return std::make_unique<icom::IcomCivBackend>();
    // RFC #4288 demo mode: the synthetic radio is selected here like any other
    // family, which is what completes the "wire it through the real SimBackend
    // factory" ask. Unlike HL2 it is a Route A hybrid — it owns a RadioConnection
    // and a PanadapterStream and vends them, so setupBackend() harvests those the
    // same way it does for Flex (see the dynamic_cast below).
    if (family.compare(QLatin1String("sim"), Qt::CaseInsensitive) == 0)
        return std::make_unique<SimBackend>();
    return std::make_unique<FlexBackend>();
}

void RadioModel::setupBackend(const QString& family)
{
    // Builds m_backend for `family` and wires everything that hangs off it.
    // Called from the constructor and again whenever the operator picks a radio
    // of a different family, so it must be safe to run more than once: every
    // connection made here has the backend (or a backend-owned object) as sender
    // or receiver, so destroying the old backend in teardownBackend() removes
    // them all automatically and re-running cannot duplicate them.
    //
    // That is a RULE for anything added here, not an observation. Qt drops a
    // connection only when one of its two objects dies, so a connection whose
    // sender AND receiver both outlive the backend — `this` on both ends, or a
    // RadioModel value member such as m_transmitModel as sender — is invisible
    // to teardownBackend() and gains one live copy per family switch (#4599).
    // Those belong in the constructor's model-lifetime block instead, even when
    // the lambda body talks to m_backend: re-reading m_backend and m_family at
    // call time is exactly what makes a once-installed callback correct across
    // a swap.
    //
    // One carve-out, already below: the per-slice intents wired inside the
    // sliceChanged handler name a SliceModel as sender, and that is a RadioModel
    // child, so teardownBackend() does not drop those either. They are still
    // right here — each connect runs once per slice CREATION (guarded on !s),
    // and dropAllSessionModelsForFamilySwitch() deletes every slice before the
    // swap. What the rule is really about is a sender that lives as long as the
    // RadioModel: `this`, or a value member such as m_transmitModel.
    m_family = family.isEmpty() ? QStringLiteral("flex") : family.toLower();

    {
        // aetherd Gap A/B: build the backend for m_family. The Flex-specific
        // construction wiring below is
        // guarded by a dynamic_cast so the Flex path stays byte-identical and a
        // non-Flex backend simply skips it (it owns its own wire objects).
        m_backend = makeBackend(m_family);
        if (auto* flex = dynamic_cast<FlexBackend*>(m_backend.get())) {
            flex->setCommandSink([this](const QString& cmd){ sendCommand(cmd); });
            // Slice verbs route through the TX-inhibit-guarded slice sink (§6), so
            // moving slice encode behind the seam keeps TX safety above it.
            flex->setSliceCommandSink([this](const QString& cmd){
                sendSliceCommand(nullptr, cmd);   // guard looks up the slice from cmd
            });
            flex->setModelProvider([this]{ return m_model; });
            m_connection = flex->connection();   // non-owning; the backend owns it
            m_panStream  = flex->panStream();    // non-owning; the backend owns it
            m_flexBackend = flex;                // transitional alias (2.3)

            // aetherd Gap B (Step 1): forward Flex's render signals into the
            // backend-neutral feed 1:1. Signal-to-signal, signature-identical → no
            // transformation and no behaviour change; the UI binds to the RadioModel
            // panFeed* signals instead of panStream() so the render path is
            // family-agnostic. Signal-to-signal preserves the original thread hop
            // (PanadapterStream worker → RadioModel thread), so batching/pacing is
            // unchanged. Valid for the whole life (panStream == RadioModel life).
            connect(m_panStream, &PanadapterStream::spectrumReady,
                    this, &RadioModel::panFeedSpectrumReady);
            connect(m_panStream, &PanadapterStream::waterfallRowReady,
                    this, &RadioModel::panFeedWaterfallRowReady);
            connect(m_panStream, &PanadapterStream::waterfallAutoBlackLevel,
                    this, &RadioModel::panFeedWaterfallAutoBlackLevel);
        } else if (auto* sim = dynamic_cast<SimBackend*>(m_backend.get())) {
            // RFC #4288 Route A: SimBackend also owns a RadioConnection and a
            // PanadapterStream (running synthetically — no socket, no wire), so it
            // vends them here exactly as FlexBackend does. That keeps RadioModel's
            // ~46 m_connection->/m_panStream-> sites pointing at real objects
            // instead of needing null-guards, and routes the demo's spectrum
            // through the same neutral render feed as every other family.
            m_connection  = sim->connection();   // non-owning; SimBackend owns it
            m_panStream   = sim->panStream();    // non-owning; SimBackend owns it
            m_flexBackend = nullptr;             // no Flex alias in demo mode
            // ⚠ Deliberately DO NOT wire the PanadapterStream render signals here.
            // The demo has TWO spectrum producers: this stream's legacy shim tick
            // (PanadapterStream::tickSyntheticDemo -> spectrumReady) and the seam
            // (SimBackend::onAudioTick -> IRadioBackend::spectrumFrameReady, wired
            // for every backend just below). Wiring both pushed two independent
            // generators, on two different timers, into the same panFeed* render
            // feed — the display received interleaved frames from each, which is
            // what made the noise floor jump about (and was audible, since the
            // audio comes from the same NoiseMixer scene). The seam is the correct
            // producer post-re-home, so the stream's copy stays disconnected.
            // The stream is still vended above because ~46 RadioModel call sites
            // dereference m_panStream; it just no longer feeds the renderer.
        }
    }

    // aetherd Gap B (Step 2): backends that deliver spectra via the normalized
    // IRadioBackend data-plane signal (HL2) feed the neutral render feed here.
    // Flex uses the PanadapterStream passthrough wired above and never emits this,
    // so this connect is harmless for Flex and load-bearing for HL2.
    connect(m_backend.get(), &IRadioBackend::spectrumFrameReady,
            this, &RadioModel::onBackendSpectrumFrame);
    // Liveness stamps, on the arrival edge rather than anywhere downstream: a
    // frame that arrives and is then discarded still proves the link is alive,
    // and that is the question these answer.
    connect(m_backend.get(), &IRadioBackend::spectrumFrameReady, this,
            [this](int, const QByteArray&) {
        m_lastSpectrumMs = QDateTime::currentMSecsSinceEpoch();
    });
    connect(m_backend.get(), &IRadioBackend::audioFrameReady, this,
            [this](const QByteArray&) {
        m_lastAudioMs = QDateTime::currentMSecsSinceEpoch();
    });

    // Demodulated RX audio from backends that produce it in-process (HL2).
    // Signal-to-signal: the payload is already the engine's native format.
    connect(m_backend.get(), &IRadioBackend::audioFrameReady,
            this, &RadioModel::backendAudioFrameReady);

    // Per-slice demodulated audio. Signal-to-signal like the mixed feed above:
    // the payload is already the engine's native format.
    connect(m_backend.get(), &IRadioBackend::sliceAudioFrameReady,
            this, &RadioModel::backendSliceAudioFrameReady);

    // Pick the one producer for the normalized RX-audio bus. Done here, once
    // per backend, so every consumer of rxDemodAudioReady is family-blind and
    // survives a swap without rewiring. See the signal's header comment.
    wireRxDemodAudioBus();

    // aetherd RFC 2.3: the first converted touchpoint. The backend decodes the
    // universal pan center/bandwidth from Flex status and emits this normalized
    // signal; RadioModel drives the addressed PanadapterModel. (Template for the
    // remaining universal fields and the other mixed models.)
    connect(m_backend.get(), &IRadioBackend::panCenterBandwidthChanged, this,
            [this](const QString& panId, double centerMhz, double bandwidthMhz) {
        // aetherd Gap B (Step 2c): remember the geometry even when no
        // PanadapterModel resolves — an HL2 session has none, and the neutral
        // waterfall rows below still need the band edges.
        // Which pane this geometry belongs to. Was pan index 0 unconditionally,
        // which is right at one receiver and wrong at four: every pan's
        // waterfall then scaled against the first pan's band edges, so three of
        // them drew the correct spectrum over the wrong frequency axis.
        const int panIdx = m_flexBackend ? kNeutralPanIndexNone
                                         : neutralPanIndexFor(panId);
        if (panIdx != kNeutralPanIndexNone) {
            m_backendPanCenterMhz[panIdx] = centerMhz;
            m_backendPanBandwidthMhz[panIdx] = bandwidthMhz;
        }
        auto* pan = resolveBackendPan(panId);
        if (!pan && !m_flexBackend && !m_connection) {
            // aetherd Gap B (Step 2c): materialise the pan for a non-Flex backend
            // WITHOUT a wire. No Flex "display pan" status exists to create one,
            // so without this there is no PanadapterModel to route frames to and
            // the UI never builds a pane — the render path was falling back to the
            // active spectrum widget, which is null when no pan applet exists.
            //
            // The !m_connection gate: a backend that vends its own RadioConnection
            // (the demo's Route A SimBackend) claims its pans from its own wire
            // status, so minting a neutral twin here created a second, ownerless
            // pan applet — the ghost "Slice A" of #4671. Its pans arrive through
            // the claim path; only truly wire-less backends (HL2) materialise.
            //
            // One pane per backend pan id, so a multi-receiver backend gets a
            // pane each instead of four receivers sharing one.
            pan = ensureOwnedPanadapter(neutralPanIdString(panIdx));
            if (pan)
                pan->setWaterfallId(neutralWfIdString(panIdx));
        }
        if (!pan) return;
        const bool spanChanged = pan->setCenterBandwidth(centerMhz, bandwidthMhz);
        // A backend that snaps a requested span to fixed hardware rates (HL2
        // offers four) reports back the span it actually runs. When that equals
        // what the model already held, the change-gated setter emits nothing —
        // and that is exactly the case the view most needs to hear about, because
        // it applied the operator's request optimistically and is now wider than
        // the data. Scoped to backends that stream raw spectra so Flex status
        // echoes, which are frequent and never refuse anything, keep their
        // existing no-op behaviour. (#4470)
        if (!spanChanged && shapesDisplayRatesLocally()) {
            pan->republishCenterBandwidth();
        }
        // Legacy signal MainWindow still consumes (unchanged behavior).
        emit panadapterInfoChanged(pan->centerMhz(), pan->bandwidthMhz());
    });

    // aetherd RFC 2.3: min/max dBm — the second universal pan field. The backend
    // decodes the display level range; RadioModel applies it to the addressed
    // pan and preserves the two side-effects the old inline block owned (for the
    // pan-resolved case): the panStream setDbmRange (only when the range actually
    // changed, to avoid a redundant GPU-scale reset) and the legacy
    // panadapterLevelChanged signal. (The old code also emitted a synthesized-
    // default panadapterLevelChanged on the pan==null path; that signal now has
    // no live consumer — per-pan levelChanged is used instead — so the no-pan
    // emit is intentionally dropped rather than resurrected. #4065 review.)
    connect(m_backend.get(), &IRadioBackend::panRangeChanged, this,
            [this](const QString& panId, double minDbm, double maxDbm) {
        auto* pan = resolveBackendPan(panId);
        if (!pan) return;
        if (pan->setRange(minDbm, maxDbm)) {
            // ⛔ m_panStream IS NULL on a backend that does not own one. It is
            // assigned only on the Flex and Sim paths (see setupBackend), so an
            // Icom — which decodes its scope inside the backend and never uses
            // PanadapterStream — leaves it nullptr. Every other use in this file
            // guards it; this one did not, and the deref crashed the GUI thread
            // the instant the backend reported a dBm range. Four dumps, all
            // identical: read of 0x30 in Qt6Core, reached from here.
            //
            // The range still reaches the model above, which is the part the
            // display needs. The stream call is the FFT decoder's copy, and a
            // backend with no PanadapterStream has no decoder to inform.
            if (m_panStream)
                m_panStream->setDbmRange(pan->panStreamId(), pan->minDbm(), pan->maxDbm());
        }
        emit panadapterLevelChanged(pan->minDbm(), pan->maxDbm());
    });

    // The pan's real span limits, reported by the backend (the X-axis
    // counterpart to panRangeChanged). Re-emitted for the GUI because the zoom
    // clamp lives in SpectrumWidget, which has no PanadapterModel of its own —
    // and the applets are built before a backend connects, so a connect-time
    // report has to reach the widgets already on screen.
    connect(m_backend.get(), &IRadioBackend::panBandwidthLimitsChanged, this,
            [this](const QString& panId, double minMhz, double maxMhz) {
        auto* pan = resolveBackendPan(panId);
        if (!pan) return;
        if (pan->setBandwidthLimits(minMhz, maxMhz)) {
            emit panBandwidthLimitsChanged(pan->panId(),
                                           pan->minBandwidthMhz(),
                                           pan->maxBandwidthMhz());
        }
    });

    // aetherd RFC 2.3: rfgain + antenna — universal pan fields (promoted per the
    // 2026-07-05 classification). The backend decodes them; RadioModel drives the
    // addressed pan. The antenna-list handler ALSO drives RadioModel's own
    // m_antList/antListChanged, converging what used to be a second independent
    // parse of ant_list in handlePanadapterStatus onto this single source.
    connect(m_backend.get(), &IRadioBackend::panRfGainChanged, this,
            [this](const QString& panId, int gain) {
        if (auto* pan = resolveBackendPan(panId)) pan->setRfGain(gain);
    });
    connect(m_backend.get(), &IRadioBackend::panRfGainInfoChanged, this,
            [this](const QString& panId, int low, int high, int step,
                   const QString& unitSuffix) {
        if (step <= 0)
            return;                       // a zero step would freeze the slider
        if (auto* pan = resolveBackendPan(panId))
            pan->setRfGainInfo(low, high, step, unitSuffix);
    });
    // Discrete receive front-end stages. Labels describe the control; the step
    // is where it currently sits. Both are pass-through — the backend owns the
    // vocabulary, because only it knows what its radio's positions are called.
    connect(m_backend.get(), &IRadioBackend::panPreampInfoChanged, this,
            [this](const QString& panId, const QStringList& labels) {
        if (auto* pan = resolveBackendPan(panId)) pan->setPreampLabels(labels);
    });
    connect(m_backend.get(), &IRadioBackend::panPreampChanged, this,
            [this](const QString& panId, int step) {
        if (auto* pan = resolveBackendPan(panId)) pan->setPreampStep(step);
    });
    connect(m_backend.get(), &IRadioBackend::panAttenuatorInfoChanged, this,
            [this](const QString& panId, const QStringList& labels) {
        if (auto* pan = resolveBackendPan(panId)) pan->setAttenuatorLabels(labels);
    });
    connect(m_backend.get(), &IRadioBackend::panAttenuatorChanged, this,
            [this](const QString& panId, int step) {
        if (auto* pan = resolveBackendPan(panId)) pan->setAttenuatorStep(step);
    });
    connect(m_backend.get(), &IRadioBackend::panRxAntennaChanged, this,
            [this](const QString& panId, const QString& ant) {
        if (auto* pan = resolveBackendPan(panId)) pan->setRxAntenna(ant);
    });
    connect(m_backend.get(), &IRadioBackend::panAntennaListChanged, this,
            [this](const QString& panId, const QStringList& ants) {
        if (auto* pan = resolveBackendPan(panId)) pan->setAntList(ants);
        // Converged RadioModel-level antenna list (the old inline dual-parse).
        // Not gated on a resolved pan — matches the old unconditional emit.
        if (ants != m_antList) {
            m_antList = ants;
            emit antListChanged(m_antList);
        }
    });
    connect(m_backend.get(), &IRadioBackend::panWaterfallLineDurationChanged, this,
            [this](const QString& panId, int ms) {
        if (auto* pan = resolveBackendPan(panId)) pan->setWaterfallLineDuration(ms);
    });

    // The backend confirms a pan is GONE. Only now is the pane dropped: the
    // backend can refuse a close (the last receiver on an HL2), and tearing the
    // model down optimistically would leave a receiver streaming into nothing.
    // Notch state reported BY the backend. Only a backend whose notches live in
    // this process emits these — a Flex reports TNFs as `tnf <id> …` status on
    // its command plane, which handleStatus() already decodes into the same
    // model. Both routes end at TnfModel, so the panadapter overlay is drawn
    // from one place regardless of where the notch physically is.
    connect(m_backend.get(), &IRadioBackend::notchChanged, this,
            [this](int notchId, const NotchDelta& delta) {
        m_tnfModel.applyNotchDelta(notchId, delta);
    });
    connect(m_backend.get(), &IRadioBackend::notchRemoved, this, [this](int notchId) {
        m_tnfModel.removeTnf(notchId);
    });

    connect(m_backend.get(), &IRadioBackend::panRemoved, this,
            [this](const QString& backendPanId) {
        auto* pan = resolveBackendPan(backendPanId);
        if (!pan)
            return;
        const QString modelPanId = pan->panId();
        // Retire the id translation for this pan. The index IS reused —
        // neutralPanIndexFor() hands out the lowest free one, deliberately, and
        // says why. What makes that safe is the cleanup below: freeing the index
        // also drops the center, bandwidth and waterfall-row state recorded
        // against it, so the pan that inherits the number cannot inherit the
        // previous occupant's geometry and start painting with it.
        if (const int idx = m_backendPanIndex.take(backendPanId); true) {
            m_backendPanIdByIndex.remove(idx);
            m_backendPanCenterMhz.remove(idx);
            m_backendPanBandwidthMhz.remove(idx);
            m_backendWfLastRowNs.remove(idx);
        }
        m_panadapters.remove(modelPanId);
        if (m_panStream) {
            m_panStream->unregisterPanStream(pan->panStreamId());
            m_panStream->unregisterWfStream(pan->wfStreamId());
        }
        qCDebug(lcProtocol) << "RadioModel: backend pan removed" << backendPanId
                            << "->" << modelPanId;
        emit panadapterRemoved(modelPanId);
        pan->deleteLater();
        if (m_activePanId == modelPanId) {
            m_activePanId = m_panadapters.isEmpty() ? QString()
                                                    : m_panadapters.firstKey();
        }
    });

    // The backend confirms a slice is GONE. Paired with panRemoved above,
    // because on a backend where a slice IS a receiver, closing one retires
    // both. Without this the SliceModel outlived its receiver and every later
    // capacity check counted it.
    connect(m_backend.get(), &IRadioBackend::sliceRemoved, this,
            [this](int sliceId) {
        SliceModel* s = slice(sliceId);
        if (!s)
            return;
        m_slices.removeAll(s);
        qCDebug(lcProtocol) << "RadioModel: backend slice removed" << sliceId;
        // Removing the transmit slice moves transmit without any slice delta to
        // announce it, so the TX-waveform meter binding has to be recomputed
        // here as well as on sliceChanged.
        m_meterModel.setActiveTxSlice(activeTxSliceNum());
        emit sliceRemoved(sliceId);
        emit slotOccupancyChanged(sliceId);
        s->deleteLater();
    });

    // The pan's front end is wide (its band filter had to be bypassed).
    connect(m_backend.get(), &IRadioBackend::panWideChanged, this,
            [this](const QString& panId, bool wide) {
        if (auto* pan = resolveBackendPan(panId))
            pan->setWide(wide);
    });

    // aetherd RFC 2.3 extension channel: Flex-specific pan fields ride the
    // namespaced extensionStatus channel; RadioModel routes them to the addressed
    // PanadapterModel. Two kinds: "panWnb" (noise blanker) and "panState" (wide,
    // loop, fps, preamp, DAX-IQ, MultiFlex client_handle, waterfall id). Other
    // namespaces/kinds are ignored here.
    connect(m_backend.get(), &IRadioBackend::extensionStatus, this,
            [this](const QString& ns, const QString& kind, const QVariantMap& fields) {
        if (ns != QLatin1String("flex")) {
            return;
        }
        if (kind != QLatin1String("panWnb") && kind != QLatin1String("panState")) {
            return;
        }
        auto* pan = resolvePan(fields.value("panId").toString());
        if (!pan) return;
        if (kind == QLatin1String("panWnb")) {
            pan->applyWnbExtension(fields);
        } else {
            pan->applyStateExtension(fields);
        }
    });

    // aetherd RFC 2.3: MeterModel touchpoint. The backend decodes the SmartSDR
    // meter-status wire format; RadioModel reconstructs the MeterDef (present-
    // only, exactly as the old inline handleMeterStatus parse did) and drives the
    // MeterModel. Meter *values* stay on the VITA-49 data plane (below).
    // #4070: the backend now emits a typed MeterDef — no key-string reconstruction.
    // THE METER LIST ARRIVES AFTER THE CONNECT EDGE, and one capability gate
    // reads it — so that gate has to be re-evaluated when it changes.
    //
    // MainWindow's mic-level gauge follows hasMicPeakMeter() rather than a
    // capability flag, which is right: a Flex forbids mic-input selection too
    // and still publishes MICPEAK, so only the meter's absence means the face
    // can never move. But applyCapabilitiesToUi() runs on capabilitiesChanged,
    // and FlexBackend never emits it — grep says the only backend that does is
    // Sim. So on a Flex the gate ran exactly once, at connect, while
    // m_micPeakIdx was still -1, and hid a gauge that was about to start
    // working. Its visibility then depended on whether an unrelated oscillator
    // or GPS status message happened to land afterwards.
    connect(m_backend.get(), &IRadioBackend::meterDefined, this,
            [this](const MeterDef& def) {
        const bool hadMicPeak = m_meterModel.hasMicPeakMeter();
        m_meterModel.defineMeter(def);
        if (hadMicPeak != m_meterModel.hasMicPeakMeter())
            publishCapabilities(isConnected());
    });
    connect(m_backend.get(), &IRadioBackend::meterRemoved, this,
            [this](int index) {
        const bool hadMicPeak = m_meterModel.hasMicPeakMeter();
        m_meterModel.removeMeter(index);
        // Symmetric on purpose: a meter that goes away must hide the face
        // again, or a radio swap leaves a dead gauge on screen.
        if (hadMicPeak != m_meterModel.hasMicPeakMeter())
            publishCapabilities(isConnected());
    });

    // A backend may revise its own capabilities mid-session (SimBackend does so
    // on connect; a Flex refines its seeded table as touchpoints convert), and
    // until now nothing above the seam listened — connectionStateChanged was the
    // only hook, so a post-connect revision never reached the UI. Republishing
    // through the same helper means every capability consumer has exactly one
    // signal to bind to and cannot observe a stale picture.
    connect(m_backend.get(), &IRadioBackend::capabilitiesChanged, this,
            [this] { publishCapabilities(isConnected()); });

    // The capture half of RadioStateMemory (RFC #4603 PR 3): a backend that
    // declares client-owned settings domains reports state movement; one
    // debounced store per burst of changes. Engagement is capability-shaped —
    // for an empty declaration (Flex, Sim) the signal is never emitted AND
    // the store call is gated again in persistOperatingState().
    connect(m_backend.get(), &IRadioBackend::operatingStateChanged, this,
            [this] {
        m_operatingStateSaveTimer.start();
        if (!m_operatingStateMaxWaitTimer.isActive()) {
            m_operatingStateMaxWaitTimer.start();
        }
    });
    // Flush the pending capture BEFORE the rest of the disconnect teardown
    // touches identity — this connection is made first, and same-thread
    // signal delivery runs slots in connection order, so the scope still
    // resolves to the radio the pending edits belong to (PR #4619 review:
    // the last tune before Disconnect was exactly the state being lost).
    connect(m_backend.get(), &IRadioBackend::disconnected, this,
            &RadioModel::flushPendingOperatingState);

    // Meter VALUES from a backend that decodes its own telemetry.
    //
    // Flex streams meter values on the VITA-49 data plane, so this signal had no
    // consumer at all and every non-Flex meter reading was computed and then
    // dropped on the floor — the HL2 S-meter was correct for a while before
    // anyone noticed it never reached the UI.
    //
    // meterId is "SOURCE:NAME" (e.g. "TX:FWDPWR"), matching MeterDef's own
    // source/name pair rather than inventing a second naming scheme.
    connect(m_backend.get(), &IRadioBackend::meterUpdate, this,
            [this](const QString& meterId, double value) {
        const int colon = meterId.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            return;
        m_meterModel.updateValueByName(meterId.left(colon), meterId.mid(colon + 1),
                                       static_cast<float>(value));
    });

    // aetherd RFC 2.3: SliceModel touchpoint. The backend decodes Flex slice
    // status into a typed SliceDelta; RadioModel routes it to the addressed slice.
    // This is an AutoConnection: because FlexBackend shares RadioModel's thread it
    // resolves to a synchronous DirectConnection today, so a slice just appended
    // to m_slices is populated before the sliceAdded UI notify below. (If a
    // backend is ever moved to a worker thread this becomes queued — the ordering
    // guarantee would then need an explicit populate step, not Qt::DirectConnection
    // across threads. #4068 review.)
    connect(m_backend.get(), &IRadioBackend::sliceChanged, this,
            [this](int sliceId, const SliceDelta& delta) {
        SliceModel* s = slice(sliceId);
        // A non-Flex backend labels its pan with its own id ("hl2"), but the UI
        // associates a slice with a pan by matching PanadapterModel::panId(). Left
        // unmapped the slice belongs to no pan, so no slice flag is drawn on the
        // panadapter even though the VFO shows the right frequency. Re-address the
        // delta at the pan we materialised.
        //
        // ...but ONLY when we materialised one. A backend that vends its own
        // RadioConnection claims its pans from its own wire, so its pan ids ARE
        // the model keys — the same discriminator resolveBackendPan() uses. The
        // demo announces slice.panId = "0x40000000", already a key; rewriting it
        // into the neutral space pointed the slice at a pan nothing holds, and
        // every slice-to-pane association keys off that exact equality (pan title
        // bar, adaptive RX filter, auto-squelch, centre-lock, band recall). (#4671)
        SliceDelta mapped = delta;
        if (!m_flexBackend && !m_connection && mapped.panId) {
            // Through the SAME allocator the geometry handler uses, so a slice
            // lands on the pane its own receiver feeds. Pinned to index 0 while
            // one pan existed; at four receivers that put every slice flag on
            // the first panadapter.
            mapped.panId = neutralPanIdString(neutralPanIndexFor(*mapped.panId));
        }
        if (!s && !m_flexBackend) {
            // aetherd Gap B (Step 2c): no Flex "slice" status ever runs for a
            // non-Flex backend, so nothing would create the model and every delta
            // would be dropped (slice panel stuck at 0.000000). Materialise it on
            // the first delta and route mode intents back through the seam.
            s = new SliceModel(sliceId, this);
            connect(s, &SliceModel::modeChangeRequested, this,
                    [this, s](const QString& mode) {
                if (m_backend) m_backend->setSliceMode(s->sliceId(), mode);
            });
            // Tuning and filter intents route through the seam too. A non-Flex
            // backend never sees SliceModel::commandReady (that carries Flex
            // wire text through the Flex-only slice sink), so without these the
            // operator's tune/filter changes update the UI and are then dropped.
            // Both signals are OPERATOR-issued only — radio-status application
            // does not emit them — so echoing radio state back as a command
            // cannot happen (Principle II).
            connect(s, &SliceModel::frequencyCommandIssued, this,
                    [this, s](double mhz) {
                if (m_backend) m_backend->setSliceFrequency(s->sliceId(), mhz * 1.0e6);
            });
            connect(s, &SliceModel::filterCommandIssued, this,
                    [this, s](int lowHz, int highHz) {
                if (m_backend) m_backend->setSliceFilter(s->sliceId(), lowHz, highHz);
            });
            // AGC is the same shape: the RX applet's mode combo and threshold
            // slider drive SliceModel, whose Flex wire text a non-Flex backend
            // never sees. Without this the controls move, the model updates and
            // the DSP keeps whatever it was opened with — a dead slider.
            connect(s, &SliceModel::agcCommandIssued, this,
                    [this, s](const QString& mode, int thresholdDb) {
                if (m_backend) m_backend->setSliceAgc(s->sliceId(), mode, thresholdDb);
            });
            // Receive DSP the radio runs. Same reasoning as AGC above: the
            // applet toggles drive SliceModel, whose Flex wire text a non-Flex
            // backend never sees, so without these the controls move and the
            // radio's own NR/NB/notch/squelch keep whatever state they had.
            connect(s, &SliceModel::noiseReductionCommandIssued, this,
                    [this, s](bool on, int level) {
                if (m_backend) m_backend->setSliceNoiseReduction(s->sliceId(), on, level);
            });
            connect(s, &SliceModel::noiseBlankerCommandIssued, this,
                    [this, s](bool on, int level) {
                if (m_backend) m_backend->setSliceNoiseBlanker(s->sliceId(), on, level);
            });
            connect(s, &SliceModel::autoNotchCommandIssued, this, [this, s](bool on) {
                if (m_backend) m_backend->setSliceAutoNotch(s->sliceId(), on);
            });
            connect(s, &SliceModel::manualNotchCommandIssued, this,
                    [this, s](bool on, int position) {
                if (m_backend) m_backend->setSliceManualNotch(s->sliceId(), on, position);
            });
            connect(s, &SliceModel::squelchCommandIssued, this,
                    [this, s](bool on, int level) {
                if (m_backend) m_backend->setSliceSquelch(s->sliceId(), on, level);
            });
            // RIT / XIT. The control already existed in VfoWidget and drove
            // SliceModel; only the last hop to the seam was missing.
            connect(s, &SliceModel::ritCommandIssued, this, [this](bool on, int hz) {
                if (!m_backend) return;
                m_backend->setRitEnabled(on);
                m_backend->setRitOffset(hz);
            });
            connect(s, &SliceModel::xitCommandIssued, this, [this](bool on, int hz) {
                if (!m_backend) return;
                m_backend->setXitEnabled(on);
                // The TRANSMIT offset verb, which defaults to the receive one
                // for a radio with a single shared register (Icom) and is
                // overridable by a radio with two (Flex).
                m_backend->setXitOffset(hz);
            });

            wireSliceAudioIntentsToBackend(s);
            m_slices.append(s);
            s->applyChanges(mapped);
            m_meterModel.setActiveTxSlice(activeTxSliceNum());
            emit sliceAdded(s);
            return;
        }
        if (s) {
            s->applyChanges(mapped);
            // Which slice owns transmit decides which TX-waveform meters resolve
            // — MeterModel::compPeakIndexForActiveTxSlice() keys the compression
            // meter off it, and it initialises to -1 meaning "none".
            //
            // The five existing calls all live in handleSliceStatus(), the Flex
            // TEXT status path, which a seam backend never reaches. So on any
            // non-Flex family m_activeTxSlice stayed -1 forever: TX:COMPPEAK was
            // defined, its value arrived and was stored, and nothing ever read it
            // because no index resolved. The gauge sat at zero while the
            // compressor behind it was working.
            //
            // Unconditional rather than gated on delta.txSlice: setActiveTxSlice()
            // early-returns when the value is unchanged, and the transmit slice
            // can also move because a slice was REMOVED, which carries no delta
            // at all.
            m_meterModel.setActiveTxSlice(activeTxSliceNum());
        }
    });

    // aetherd RFC 2.3: TransmitModel touchpoint. The backend decodes the five
    // Flex transmit-family status planes (transmit/interlock/ATU/APD/APD-sampler)
    // into a typed TransmitDelta; RadioModel drives the TransmitModel. Driven
    // synchronously from the matching decode*Status() calls in the status
    // handlers (main-thread AutoConnection → DirectConnection).
    connect(m_backend.get(), &IRadioBackend::transmitChanged, this,
            [this](const TransmitDelta& delta) {
                // A backend-reported MOX edge is radio state, not local intent.
                // Keep it out of TransmitModel::moxChanged, whose consumers
                // own this client's audio, DAX, recorder and serial PTT.
                if (delta.mox)
                    publishBackendTransmitEdge(*delta.mox);
                m_transmitModel.applyChanges(delta);
            });

    // aetherd 2.4 (#4094): power-amp status decoded in the backend drives AmpModel.
    connect(m_backend.get(), &IRadioBackend::amplifierChanged, this,
            [this](const AmpDelta& delta) { m_amplifier.applyChanges(delta); });

    // aetherd 2.4 (#4092): TGXL tuner status decoded in the backend drives TunerModel.
    connect(m_backend.get(), &IRadioBackend::tunerChanged, this,
            [this](const TunerDelta& delta) { m_tunerModel.applyChanges(delta); });

    // aetherd RFC 2.3 (RadioModel residual): radio-global status decoded in the
    // backend drives RadioModel's own state via applyRadioChanges.
    connect(m_backend.get(), &IRadioBackend::radioChanged, this,
            [this](const RadioDelta& delta) { applyRadioChanges(delta); });

    // aetherd RFC 2.3 (RadioModel residual): GPS / memory-slot / profile status
    // decoded in the backend drive RadioModel's own state via the apply* methods.
    connect(m_backend.get(), &IRadioBackend::gpsChanged, this,
            [this](const GpsDelta& delta) { applyGpsChanges(delta); });
    connect(m_backend.get(), &IRadioBackend::memoryChanged, this,
            [this](const MemoryDelta& delta) { applyMemoryChanges(delta); });
    connect(m_backend.get(), &IRadioBackend::profileChanged, this,
            [this](const ProfileDelta& delta) { applyProfileChanges(delta); });

    // NOTE: the three connects below hang off the Flex PanadapterStream, which a
    // backend carrying its own IQ does not have. They ran unconditionally, so an
    // HL2 setup logged "QObject::connect: invalid nullptr parameter" and wired
    // nothing -- including meterDataReady, which is part of why the S-meter has
    // never reached the UI on this backend.
    //
    // Guarded individually rather than with an early return: the m_connection
    // and IRadioBackend connects further down are interleaved with these and ARE
    // needed by a self-IQ backend. Returning early here would have skipped
    // IRadioBackend::connected and broken HL2 connection outright.

    // Centralized DAX RX channel ownership (#3305): PanadapterStream decides
    // WHEN a dax_rx stream must exist (refcounted acquire/release from the
    // bridge/TCI/RADE); RadioModel is the command plane that makes it so.
    if (m_panStream)
    connect(m_panStream, &PanadapterStream::daxStreamCreateNeeded,
            this, [this](int ch) {
        if (!isConnected()) {
            // Dropped create (connect gap): tell the manager so the latch
            // clears and its retry cadence re-fires — otherwise the channel
            // wedges with createPending stuck true (the #3669 wedge class).
            m_panStream->notifyDaxCreateFailed(ch);
            return;
        }
        sendCmd(QString("stream create type=dax_rx dax_channel=%1").arg(ch),
                [this, ch](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcDax) << "RadioModel: dax_rx stream create for channel"
                                 << ch << "failed, code" << Qt::hex << code << body;
                m_panStream->notifyDaxCreateFailed(ch);
                return;
            }
            // Success needs no action here. The #1439 legacy client-
            // registration nudge is decided in handleDaxRxStreamRegistry, when
            // the registration status has definitively told us whether the
            // radio auto-bound the stream (slice=<letter>) — deciding here
            // would race that status: on WAN/SmartLink (and any firmware that
            // binds after the create reply) the binding isn't known yet, so a
            // reply-first ordering would fire a same-value `slice set dax=`
            // re-assert and blip audio, the very thing the gate avoids (#4017).
        });
    });
    if (m_panStream)
    connect(m_panStream, &PanadapterStream::daxStreamRemoveNeeded,
            this, [this](quint32 streamId, int ch) {
        Q_UNUSED(ch);
        if (!isConnected()) return;
        sendCommand(QString("stream remove 0x%1").arg(streamId, 0, 16));
    });

    // RadioConnection (created + owned by the backend above, on its own worker
    // thread #502 so TCP I/O never blocks paintEvent) — wire its signals to us.
    // Signals from RadioConnection auto-queue to main thread (#502)
    //
    // RadioConnection is the Flex TCP command channel; a self-IQ backend has
    // none and m_connection is null, so each of these logged an "invalid
    // nullptr parameter" connect. The lifecycle it would have carried arrives
    // through the neutral IRadioBackend signals below instead, which is why the
    // block after this one is gated on !m_connection.
    if (m_connection) {
    connect(m_connection, &RadioConnection::statusReceived,
            this, &RadioModel::onStatusReceived);
    connect(m_connection, &RadioConnection::messageReceived,
            this, &RadioModel::onMessageReceived);
    connect(m_connection, &RadioConnection::connected,
            this, &RadioModel::onConnected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &RadioModel::onDisconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &RadioModel::onConnectionError);
    connect(m_connection, &RadioConnection::versionReceived,
            this, &RadioModel::onVersionReceived);

    // Response callbacks: RadioConnection emits commandResponse on worker thread,
    // we dispatch to the matching callback on the main thread. (#502)
    connect(m_connection, &RadioConnection::commandResponse,
            this, [this](quint32 seq, int code, const QString& body) {
        auto it = m_pendingCallbacks.find(seq);
        if (it != m_pendingCallbacks.end()) {
            it.value()(code, body);
            m_pendingCallbacks.erase(it);
        }
    });

    }  // if (m_connection)

    // aetherd Gap B (Step 2b): a backend that does not drive the lifecycle
    // through a Flex RadioConnection drives it through the neutral
    // IRadioBackend signals instead.
    //
    // The guard MUST mirror the connect dispatch in connectToRadio(): that does
    // `if (m_connection) <dial the wire> else if (m_backend) <seam connect>`, so
    // whichever side initiates the connect is the side that reports the lifecycle.
    // Hence "!m_connection" here.
    //
    // A previous revision relaxed this to "!m_flexBackend" so that
    // SimBackend::disconnectRadio() (the `sim disconnect` fault) would be heard —
    // it emitted IRadioBackend::disconnected into nothing, leaving the model
    // "connected" with dead audio. But SimBackend is an RFC #4288 Route A hybrid:
    // it vends a synthetic RadioConnection AND re-emits that connection's
    // lifecycle as its own IRadioBackend signals. With the relaxed guard, sim was
    // the one family where BOTH blocks were live, so a single wire event reached
    // onConnected/onDisconnected TWICE — running registerAsGuiClient twice (two
    // GUI-client batches, two 10 s UDP health timers, a second m_panStream->start()),
    // emitting connectionStateChanged twice, and staging session models twice
    // (which zeroed m_staleSessionOwnHandle and defeated the #3977 reclaim guard).
    //
    // The real fix for `sim disconnect` belongs on the other side of the seam:
    // SimBackend::disconnectRadio() now tears down its synthetic connection, so the
    // wire reports the disconnect through this single path. See SimBackend.cpp.
    if (!m_connection) {
        connect(m_backend.get(), &IRadioBackend::connected,
                this, &RadioModel::onConnected);
        connect(m_backend.get(), &IRadioBackend::disconnected,
                this, &RadioModel::onDisconnected);
        connect(m_backend.get(), &IRadioBackend::connectionError,
                this, &RadioModel::onConnectionError);
        // Advisory only — deliberately NOT routed through onConnectionError,
        // which starts the reconnect timer. Re-emitted for the UI to surface.
        connect(m_backend.get(), &IRadioBackend::configurationWarning,
                this, &RadioModel::configurationWarning);
    }

    // Transport counters from a backend that owns its own socket. Wired
    // unconditionally: a backend that measures nothing never emits this, and one
    // that does is the only source the network readouts have.
    connect(m_backend.get(), &IRadioBackend::linkStatsUpdated,
            this, &RadioModel::applyBackendLinkStats);

    // Forward VITA-49 meter packets to MeterModel (cross-thread, auto-queued)
    if (m_panStream)
    connect(m_panStream, &PanadapterStream::meterDataReady,
            &m_meterModel, &MeterModel::updateValues);

    // HAND THE FRESH BACKEND THE MIC GAIN THE MODEL ALREADY HOLDS.
    //
    // The seam wired in the constructor carries operator INTENT — it fires when
    // the slider moves, and a backend rebuild is not the slider moving. So
    // without this, a family swap silently parts the two: the new modulator is
    // constructed at its own 1.0 default (Hl2TxDsp::m_micGain) while
    // TransmitModel::m_micLevel still holds the operator's position, because
    // nothing resets that model and micLevel is not persisted for
    // applyRestoredState() to restore. Connect an HL2, set MIC to 80, visit the
    // demo or a Flex, come back: the slider reads 80, the snapshot's micLevel
    // reads 80, and the radio is transmitting at unity.
    //
    // That is the readback-agreeing-with-the-failure shape this whole change
    // exists to eliminate, so it cannot be left standing one seam over. Pushing
    // here rather than in the connect path because the disagreement is created
    // by CONSTRUCTION, not by connecting — the modulator is wrong the moment it
    // exists, and a backend that is never connected should still answer
    // healthSnapshot() honestly.
    //
    // Free on the constructor's own call, where TransmitModel is at its 50 and
    // 50 maps to the 1.0 the modulator already holds. Same Flex gate as the
    // seam: on a Flex the slider's `transmit set miclevel=` reaches the radio's
    // own preamp and this must not double it.
    if (m_backend && !usesFlexCommandPlane())
        m_backend->setMicGain(m_transmitModel.micLevel());
}

void RadioModel::applyBackendLinkStats(const IRadioBackend::LinkStats& stats)
{
    if (!stats.reported)
        return;

    const bool first = !m_linkStats.reported;
    m_linkStats = stats;
    // What this transport can MEASURE, latched separately from what it measured
    // this second. Sticky-once-true so a window that closes with no samples in
    // it does not flip a readout back to "not measured" mid-session, and read by
    // hasLinkRtt() / hasLinkTiming() after stopNetworkMonitor() has dropped the
    // counters. See the member declaration for why the distinction matters.
    m_backendLinkShape.reports = true;
    if (stats.rttMs >= 0)
        m_backendLinkShape.hasRtt = true;
    if (stats.gapMs >= 0)
        m_backendLinkShape.hasTiming = true;

    if (first) {
        // First snapshot of the session. resetNetworkHealthSamples() (called
        // from the shared reset) now reads the new source, so the deltas are
        // seeded from THIS snapshot rather than from zero — otherwise a
        // reconnect's entire prior packet count lands in the first loss-window
        // sample and scores the link as a catastrophe on its first second.
        //
        // Shared with startNetworkMonitor() rather than open-coded: the two
        // drifted once, and the field this path had forgotten (m_lastPingRtt)
        // is invisible on a transport that reports rttMs < 0, so the previous
        // Flex session's RTT scored the HL2 link with nothing on screen to
        // contradict it.
        resetNetworkQualitySession();
    }

    if (stats.rttMs >= 0)
        m_lastPingRtt = stats.rttMs;

    evaluateNetworkQuality();

    // The heartbeat is a statement about the RADIO, not about the timer that
    // asked. Only a tick that saw fresh traffic counts as a beat; a tick on a
    // silent link deliberately says nothing, so MainWindow's miss timer runs
    // out and the indicator goes to its alarm state.
    if (stats.alive)
        emit pingReceived();
}

void RadioModel::teardownBackend()
{
    // Drop the backend and everything it owns (RadioConnection, PanadapterStream
    // and their worker threads). Qt removes any connection whose sender or
    // receiver is destroyed, so the wiring made by setupBackend() goes with it.
    if (!m_backend)
        return;
    if (m_connection)
        QObject::disconnect(m_connection, nullptr, this, nullptr);
    if (m_panStream)
        QObject::disconnect(m_panStream, nullptr, this, nullptr);
    QObject::disconnect(m_backend.get(), nullptr, this, nullptr);
    // Null the transitional alias BEFORE destroying the backend so any status
    // slot running during teardown fails closed instead of dereferencing a
    // backend mid-destruction.
    m_flexBackend = nullptr;
    // Any backend replacement invalidates a WSPR transmit-route claim. Cleared
    // here rather than only in onDisconnected(), because a family switch never
    // reaches that path — see hasWsprTxStream().
    m_wsprTxSeamAudioArmed = false;
    m_backend.reset();
    m_connection = nullptr;
    m_panStream = nullptr;
    // Backend pan ids are only meaningful to the backend that issued them, so
    // the translation table dies with it. Carrying it across a family swap would
    // let a new backend's first pan inherit an index the old one had allocated,
    // and every pan after it would be off by one.
    m_backendPanIndex.clear();
    m_backendPanIdByIndex.clear();
    m_backendPanCenterMhz.clear();
    m_backendPanBandwidthMhz.clear();
    // What the transport could measure was a fact about the backend that just
    // died, not about the app. A Flex arriving after an HL2 must not inherit
    // "this wire has no round trip to time".
    m_backendLinkShape = {};
}

namespace {
// Audio must actually be reaching the filter before a low post-filter level
// means anything. Silence reads the meter floor (-150 dBFS on a FLEX-6700);
// a live tone measured -12 dBFS at SC_MIC and -9.8 at SC_FILT_1. -60 sits
// far above the floor and far below any real transmit audio.
constexpr float kTxAudioPresentFloorDbfs = -60.0f;
// How far SC_FILT_2 must sit below SC_FILT_1 before we call the audio
// "removed". Chosen from a tone sweep across the filter skirt on a FLEX-6700
// with a 100-2900 Hz passband (#4649), because the corner is soft and a
// too-eager threshold produces a confidently wrong message:
//
//   tone   SC_FILT_1 -> SC_FILT_2   delta    forward power
//   2800     -7.76      -5.56       -2.2 dB    43.66 W   (in band)
//   2900     -7.70     -11.31        3.6 dB    38.9  W   (at the cut, still fine)
//   3000     -8.30     -38.95       30.7 dB    17.73 W   (attenuated, NOT dead)
//   3100     -8.88     -64.98       56.1 dB     5.82 W   (effectively dead)
//   3300     -9.16     -55.85       46.7 dB     3.08 W
//
// At 30 dB this fires while the radio is still making 17.7 W -- 40% of full
// output -- which would be reported to the operator as a dead transmitter.
// 40 dB sits between the 30.7 dB case we must NOT flag and the 46.7 dB case we
// must, and only fires once output has collapsed to a few percent.
constexpr float kTxFilterKillDeltaDb = 40.0f;
// Consecutive qualifying meter packets before we speak. The chain ramps at
// key-down -- SC_MIC rises before SC_FILT_2 settles -- so a single sample is
// not evidence. Meters arrive at roughly 10-20 Hz, so this is ~0.3 s.
constexpr int kTxFilterKillConfirmSamples = 5;
// The two filter taps publish at 20 fps and 10 fps. Emission is tied to the
// slower one, so in steady state the partner is ~50 ms old; anything much
// beyond that means one tap has stalled and the pair no longer describes the
// same moment.
constexpr qint64 kTxFilterMaxTapSkewMs = 150;
}  // namespace

// A TX low/high cut that excludes the transmit audio produces no RF, and nothing
// else in the client can tell the operator their filter is the cause (#4649).
// The radio publishes both sides of the filter, so this MEASURES the loss rather
// than inferring it from the cut values -- which is what makes it safe to state
// plainly to the operator instead of as a guess.
void RadioModel::evaluateTxFilterAudioLoss(float scFilt1, float scFilt2)
{
    // Our own transmission only. isOperatorTransmitting() is deliberately NOT
    // used here: it is false for DAX transmits, which is exactly the digital
    // path this defect is reported on. txOwnedByUs() is derived from the
    // interlock's tx_client_handle, so it covers DAX and TCI alike.
    if (!m_radioTransmitting || !m_txOwnedByUs) {
        m_txFilterKillSamples = 0;
        m_txFilterKillReported = false;
        return;
    }
    // Both filter taps must have produced a real sample; see
    // hasTxFilterLevels(). They also publish at different rates, so only
    // compare them while the pair is close in time.
    if (!m_meterModel.hasTxFilterLevels())
        return;
    const qint64 skewMs = m_meterModel.txFilterLevelSkewMs();
    if (skewMs < 0 || skewMs > kTxFilterMaxTapSkewMs) {
        m_txFilterKillSamples = 0;
        return;
    }

    // CW never routes operator audio through the TX filter, so a low SC_FILT_2
    // there carries no information. Excluded explicitly rather than trusting
    // SC_MIC to sit at the floor during CW -- that has not been measured.
    const SliceModel* tx = txSlice();
    if (tx && tx->mode().trimmed().toUpper().startsWith(QStringLiteral("CW"))) {
        m_txFilterKillSamples = 0;
        m_txFilterKillReported = false;
        return;
    }

    // Measure ACROSS THE FILTER STAGES, not from the microphone tap.
    // SC_MIC -> SC_FILT_2 spans mic gain, the equaliser, the speech
    // processor and ALC, so anything upstream collapsing -- a closed DEXP
    // gate, mic gain at zero, a processor fault -- would be reported to the
    // operator as a TX-filter problem, which is a confident wrong answer.
    // SC_FILT_1 sits immediately before the stage the operator's low/high
    // cut actually controls, so this pair isolates it. Measured (#4649):
    // audio inside the passband ran -7.73 -> -5.23 dBFS across the two
    // filter taps, audio outside it -9.79 -> -68.18 -- a wider separation
    // than the mic-referenced comparison, and immune to everything before.
    // Gate on SC_FILT_1 ALONE, never SC_MIC. SC_MIC is the PC/remote-audio
    // entry point only: a hardware mic (BAL/LINE/ACC) joins the chain later,
    // through the CODEC ADC, and never registers there
    // (docs/architecture/tx-audio-signal-path.md). Requiring SC_MIC would
    // silently switch this whole check off for every operator on a real
    // microphone -- the people running tight voice filters in the first
    // place. SC_FILT_1 proves audio reached the filter whatever path it took.
    const bool audioReachedFilter = scFilt1 > kTxAudioPresentFloorDbfs;
    const bool filterRemoved      = scFilt2 < scFilt1 - kTxFilterKillDeltaDb;
    if (!audioReachedFilter || !filterRemoved) {
        m_txFilterKillSamples = 0;
        return;
    }
    if (++m_txFilterKillSamples < kTxFilterKillConfirmSamples)
        return;
    if (m_txFilterKillReported)
        return;   // once per transmission, not once per meter packet
    m_txFilterKillReported = true;

    // Name the actual cut values and the measured attenuation. Deliberately
    // NOT the forward-power reading: this fires ~0.3 s after key-down, before
    // the smoothed power meter has settled, so it reported "0.0 W" on a
    // transmission actually making 2.3 W. The attenuation is the quantity the
    // comparison is built from, so it is settled by construction and cannot
    // contradict the reason the message appeared.
    // State the OBSERVATION, not a culprit. This measures that the audio and
    // the passband do not overlap; it cannot tell whether the cuts were moved
    // or the audio's pitch was. Blaming the filter outright sends an operator
    // whose cuts are fine looking in the wrong place -- which is exactly how
    // the first screenshot of this feature was misread.
    emit txFilterBlockingAudio(
        tr("Almost no transmit audio is getting through the TX filter"),
        tr("Passband %1-%2 Hz is cutting it by %3 dB, so almost no RF is "
           "leaving the radio. Check your TX Low/High Cut, and the pitch of "
           "your transmit audio.")
            .arg(m_transmitModel.txFilterLow())
            .arg(m_transmitModel.txFilterHigh())
            .arg(qRound(scFilt1 - scFilt2)),
        tx ? tx->panId() : QString());
}

RadioModel::RadioModel(QObject* parent)
    : QObject(parent)
{
    // Register the typed seam-delta payloads so IRadioBackend's normalized
    // signals survive a queued connection. Today decode*Status runs synchronously
    // on this thread (AutoConnection → DirectConnection, no metatype needed), but
    // if a backend is ever moved to a worker thread the connection becomes queued;
    // without registration Qt would log "Cannot queue arguments of type …" and
    // silently drop the emit. Idempotent + cheap. (#4071 review.)
    qRegisterMetaType<SliceDelta>();
    qRegisterMetaType<TransmitDelta>();
    qRegisterMetaType<MeterDef>();
    qRegisterMetaType<RadioDelta>();
    qRegisterMetaType<GpsDelta>();
    qRegisterMetaType<MemoryDelta>();

    // Watch both sides of the TX filter so a cut that silences the transmit
    // audio can be reported to the operator instead of failing silently (#4649).
    connect(&m_meterModel, &MeterModel::txFilterLevelsChanged,
            this, &RadioModel::evaluateTxFilterAudioLoss);
    // Arm/disarm on the TX edge, NOT from the meter path. A radio with
    // met_in_rx off publishes no TX- meters between transmissions, so a latch
    // cleared only inside the meter handler would never clear at all -- the
    // card would appear on the first bad transmission of a session and never
    // again.
    connect(this, &RadioModel::radioTransmittingChanged, this, [this](bool) {
        m_txFilterKillSamples = 0;
        m_txFilterKillReported = false;
    });
    qRegisterMetaType<ProfileDelta>();
    qRegisterMetaType<AmpDelta>();
    qRegisterMetaType<TunerDelta>();

    // Publish the host-side memory bank once the event loop turns. Deferred
    // rather than done here because MainWindow connects to memoryChanged AFTER
    // constructing this model — emitting inside the ctor would reach nobody and
    // the browse panel would sit empty until the first connect. With no radio
    // attached yet, usesLocalMemoryBank() is true, so a Flex-only operator with
    // an empty bank file publishes nothing and sees no change.
    QTimer::singleShot(0, this, [this]() { publishLocalMemories(); });

    DigitalVoiceWaveformProcess& digitalVoiceProcess =
        DigitalVoiceWaveformProcess::instance();
    connect(&digitalVoiceProcess, &DigitalVoiceWaveformProcess::metricsChanged,
            this, &RadioModel::digitalVoiceWaveformMetricsChanged);
    connect(&digitalVoiceProcess, &DigitalVoiceWaveformProcess::healthChanged,
            this, &RadioModel::digitalVoiceWaveformHealthChanged);
    connect(&digitalVoiceProcess, &DigitalVoiceWaveformProcess::degradationStarted,
            this, &RadioModel::digitalVoiceWaveformDegradationStarted);
    connect(&digitalVoiceProcess,
            &DigitalVoiceWaveformProcess::sliceRestoreRequested,
            this,
            [this](int sliceId, const QString& previousMode) {
        SliceModel* controlledSlice = slice(sliceId);
        if (!controlledSlice
            || !DigitalVoiceModeRegistry::modeForRadioMode(
                    controlledSlice->mode()).has_value()) {
            return;
        }
        QString restoreMode = previousMode.trimmed().toUpper();
        if (restoreMode.isEmpty()
            || DigitalVoiceModeRegistry::modeForRadioMode(restoreMode).has_value()) {
            restoreMode = DigitalVoiceModeRegistry::descriptor(
                DigitalVoiceModeId::DStar).underlyingMode;
        }
        controlledSlice->setMode(restoreMode);
    });
    connect(&digitalVoiceProcess, &DigitalVoiceWaveformProcess::stateChanged,
            this, [this](DigitalVoiceWaveformProcess::State state) {
        m_lastDigitalVoiceTxSelectionKey.clear();
        if (state == DigitalVoiceWaveformProcess::State::Running) {
            m_dstarRuntimeConfigurationPending = true;
            syncDigitalVoiceTxSelection(true);
            applyPendingDStarRuntimeConfiguration();
        }
    });
    connect(&DigitalVoiceModeRegistry::instance(),
            &DigitalVoiceModeRegistry::activeSliceChanged,
            this,
            [this](int) {
        m_lastDigitalVoiceTxSelectionKey.clear();
        syncDigitalVoiceTxSelection(true);
        applyPendingDStarRuntimeConfiguration();
    }, Qt::QueuedConnection);

    const QString digitalVoiceDir =
        QFileInfo(AppSettings::instance().filePath()).absolutePath()
        + QStringLiteral("/digital-voice");
    m_dstarModel.setTrafficPersistencePath(
        digitalVoiceDir + QStringLiteral("/dstar-traffic.json"));
    connect(&m_flexWaveformModel, &FlexWaveformModel::genericStatusReceived,
            &m_dstarModel, &DStarModel::handleWaveformStatus);
    connect(&m_dstarModel, &DStarModel::configurationChanged,
            this, &RadioModel::scheduleDStarRuntimeConfiguration);
    connect(&m_transmitModel, &TransmitModel::transmittingChanged,
            this, [this](bool transmitting) {
        if (!transmitting) {
            applyPendingDStarRuntimeConfiguration();
        }
    });
    connect(this, &RadioModel::radioTransmittingChanged,
            this, [this](bool transmitting) {
        if (!transmitting) {
            applyPendingDStarRuntimeConfiguration();
        }
    });

    // ── Model-lifetime wiring ───────────────────────────────────────────────
    //
    // Every connection in THIS BLOCK (down to the aetherd RFC 2.2b note) has
    // RadioModel on BOTH ends: `this` as receiver, and as sender either `this`
    // or a RadioModel value member. The rest of the constructor is ordinary
    // model wiring and carries no such claim. Nothing here
    // dies with the backend, so nothing here may live in the rerunnable
    // setupBackend() — installed there, these survived teardownBackend() and
    // accumulated one live copy per family switch for the rest of the session
    // (#4599). They follow whichever backend is current all the same, because
    // each lambda re-reads m_backend / m_family at call time.
    //
    // Installed BEFORE the first setupBackend() below so no connection edge can
    // ever precede its own callback. Nothing emits during setupBackend() today;
    // this makes that a property of the code rather than of an audit.

    // Tell the transmit model whether the HOST modulates. It drives the
    // mic-source list: a backend that modulates here has no physical input jacks
    // to choose between, so "PC" is the only truthful answer.
    connect(this, &RadioModel::connectionStateChanged, this,
            [this](bool connected) { publishCapabilities(connected); });

    // A host-modulating backend owns a local drive register and must receive
    // the cached value when it is constructed. A radio-authoritative backend
    // such as Icom must first read its per-band setting; pushing the model's
    // stale pre-connect value here overwrote the radio before its readback.
    connect(this, &RadioModel::connectionStateChanged, this,
            [this](bool connected) {
        if (connected && m_backend && backendCapabilities().hostModulates) {
            m_backend->setTxPower(m_transmitModel.rfPower());
        }
    });

    // RF power to a backend that owns a drive register. Flex takes it as a text
    // command from TransmitModel and ignores this.
    connect(&m_transmitModel, &TransmitModel::rfPowerCommandIssued, this,
            [this](int percent) {
        if (m_backend)
            m_backend->setTxPower(percent);
    });

    // The CW pitch to a backend that owns its own demodulator.
    //
    // Not a transmit setting for these radios. When we demodulate, the pitch IS
    // the receiver's BFO — the offset that turns a signal sitting on the marker
    // into an audible tone — so a backend that never learns it has no way to
    // put its CW passband where the operator is looking. See
    // IRadioBackend::setCwPitch().
    //
    // cwPitchChanged, not phoneStateChanged: the latter is the bulk-update
    // signal and fires on every unrelated TX edit, which would re-push the
    // pitch (and with it a passband and shift rebuild on every CW receiver)
    // for a mic-gain change.
    //
    // Gated off the Flex command plane like every other seam setter here: a
    // Flex already has `cw pitch` as text from TransmitModel and applies the
    // BFO on-radio.
    connect(&m_transmitModel, &TransmitModel::cwPitchChanged, this,
            [this](int hz) {
        if (m_backend && !usesFlexCommandPlane())
            m_backend->setCwPitch(hz);
    });

    // And on connect, for the same reason the power push above exists:
    // cwPitchChanged fires on edges, so a session that never touches the pitch
    // would leave a host-demodulating backend on its own construction-time
    // default. Those agree today (both 600 Hz), which is precisely why the
    // disagreement would be invisible the first time either one moves.
    connect(this, &RadioModel::connectionStateChanged, this,
            [this](bool connected) {
        if (connected && m_backend && !usesFlexCommandPlane())
            m_backend->setCwPitch(m_transmitModel.cwPitch());
    });

    // The speech processor to a backend that owns its own compressor.
    //
    // HERE, not in setupBackend(): m_transmitModel is a RadioModel value member,
    // so this connection outlives every backend and installing it in the
    // rerunnable path would accumulate one live copy per family switch — the
    // exact defect #4599 fixed, where N switches turn one operator press into N
    // commands.
    //
    // OPERATOR INTENT ONLY — speechProcessorCommandIssued, never micStateChanged.
    // The distinction is the one TransmitModel documents at the declaration: a
    // state-mirroring signal would hand the radio's own echo back as a fresh
    // command. Flex takes this as text and ignores the seam; a host-modulating
    // backend runs the compressor in our DSP and also ignores it.
    // VOX and the ATU: the wire text these also emit is a Flex command and
    // reaches nothing on any other family, so the intent has to cross the seam.
    connect(&m_transmitModel, &TransmitModel::voxCommandIssued, this,
            [this](bool on, int level, int delayMs) {
        if (m_backend) m_backend->setVox(on, level, delayMs);
    });
    connect(&m_transmitModel, &TransmitModel::monitorCommandIssued, this,
            [this](bool on, int level) {
        if (m_backend && !usesFlexCommandPlane())
            m_backend->setTxMonitor(on, level);
    });
    connect(&m_transmitModel, &TransmitModel::atuCommandIssued, this,
            [this](bool start) {
        if (m_backend) m_backend->setAtu(start);
    });
    connect(&m_transmitModel, &TransmitModel::speechProcessorCommandIssued, this,
            [this](bool on, int level) {
        if (m_backend && !m_flexBackend)
            m_backend->setSpeechProcessor(on, level);
    });

    // Keying and tune from the GUI.
    //
    // These paths never reached a non-Flex backend: the MOX button goes through
    // TransmitModel::setMox, which emits the Flex text command "xmit 1" and
    // nothing else, and TUNE emits "transmit tune 1" the same way. Only the
    // automation bridge's key verb went through setTransmit() and therefore
    // through the seam — which is exactly why keying worked under test and did
    // nothing when the operator pressed MOX.
    //
    // Gated to non-Flex families ON PURPOSE: for Flex the text command above
    // already keys the radio, and routing this as well would send "xmit 1"
    // twice.
    connect(&m_transmitModel, &TransmitModel::moxCommandIssued, this,
            [this](bool on) {
        if (m_backend && m_family != QLatin1String("flex")) {
            if (on && !refuseKeyOnTransmitIncapableBackend())
                return;
            m_backend->setKeying(on);
            // The MOX button and the PTT coordinator key here, NOT through
            // setTransmit(), so the raw-TX edge has to be published on this path
            // too — otherwise a TCI client watching an operator-initiated
            // transmit sees nothing. See publishBackendTransmitEdge().
            publishBackendTransmitEdge(on);
        }
    });
    connect(&m_transmitModel, &TransmitModel::tuneCommandIssued, this,
            [this](bool on) {
        if (m_backend && m_family != QLatin1String("flex")) {
            if (on && !refuseKeyOnTransmitIncapableBackend()) {
                // Un-latch TUNE as well. m_tune was already set optimistically
                // (TransmitModel::startTune), and the button's toggle reads it,
                // so leaving it set would strand TUNE "on" against a radio that
                // never keyed. The re-entry through this same slot carries
                // on=false and so skips the guard.
                m_transmitModel.stopTune();
                return;
            }
            // Hand the backend the operator's TUNE power. A host-modulated
            // backend has no other path to it — see IRadioBackend::setTune().
            m_backend->setTune(on, m_transmitModel.tunePower());
            // A tune carrier is a transmission. Hl2Backend::setTune() calls
            // setKeying(), so the radio is on the air — the raw-TX edge has to
            // be published here for the same reason it is on the MOX path, and
            // for one more that is specific to tune: TciServer's
            // "already transmitting" guard reads isRadioTransmitting(), so
            // leaving it false let a TCI client key ON TOP of a live tune
            // carrier, and that client's unkey then dropped the key while tune
            // still believed it owned it. A TCI-driven amplifier also never saw
            // trx:true for the carrier operators most often tune INTO an amp.
            publishBackendTransmitEdge(on);
        }
    });

    // aetherd RFC step 2.2b: the radio-facing seam owns the wire objects. The
    // FlexBackend creates the RadioConnection and PanadapterStream on their
    // worker threads (in the load-bearing #502 order — panStream first) and
    // tears them down. RadioModel keeps non-owning pointers, obtained here, so
    // all the signal wiring and command/WAN orchestration below is byte-for-byte
    // as before — the move is ownership-only.
    //
    // Note: the threads start at THIS call rather than adjacent to their signal
    // wiring below. (It used to be the ctor's first statement; the model-lifetime
    // block above and the DV/meter wiring before it now precede it, and none of
    // that touches the wire.) Safe because RadioConnection::
    // init()/PanadapterStream::init() only allocate sockets/timers and neither
    // auto-connects nor emits — so there is no lost-signal window before our
    // statusReceived/etc. connections are made. Keep that true if init() grows.
    // Default to Flex; the backend is swapped in connectToRadio() to match
    // whichever radio the operator picks in the connection manager.
    setupBackend(QStringLiteral("flex"));

    // #4142 — single owner of the deferred pan-write replay. Armed by the
    // defer path (armProfileLoadPanWriteFlush), hold-relative, self-re-arming;
    // the flush re-checks the hold before sending a byte.
    m_profileLoadPanWriteFlushTimer.setSingleShot(true);
    connect(&m_profileLoadPanWriteFlushTimer, &QTimer::timeout,
            this, &RadioModel::flushPendingProfileLoadPanWrites);

    // Route tuner relay intents to the radio through the backend seam (#4092).
    // The model emits neutral intents; FlexBackend translates them to the SmartSDR
    // "tgxl …" wire, resolving the TGXL handle from its own decode-side state
    // (#4198) — the intent carries no Flex identifier. The direct port-9010
    // fast-path stays inside TunerModel and never reaches here.
    connect(&m_tunerModel, &TunerModel::operateRequested, this, [this](bool on){
        if (m_backend)
            m_backend->invokeExtension(QStringLiteral("flex"), QStringLiteral("tuner.operate"), 0,
                                       QVariantMap{{QStringLiteral("on"), on}});
    });
    connect(&m_tunerModel, &TunerModel::bypassRequested, this, [this](bool on){
        if (m_backend)
            m_backend->invokeExtension(QStringLiteral("flex"), QStringLiteral("tuner.bypass"), 0,
                                       QVariantMap{{QStringLiteral("on"), on}});
    });
    connect(&m_tunerModel, &TunerModel::autotuneRequested, this, [this](){
        // TX interlock gate (was a commandReady string-sniff on "tgxl autotune").
        if (transmitStartBlockedByInhibit(QStringLiteral("tgxl-autotune")))
            return;
        applyTuneInhibit();
        if (m_backend)
            m_backend->invokeExtension(QStringLiteral("flex"), QStringLiteral("tuner.autotune"), 0);
    });

    // Forward DAX IQ commands to the radio
    connect(&m_daxIqModel, &DaxIqModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    // Route amplifier (PGXL) operate intent to the radio through the backend seam
    // (#4094). FlexBackend relays "amplifier set … operate=" to the amp (the path
    // that works remote/SmartLink), resolving the amp handle from its own
    // decode-side state (#4198) — the intent carries no Flex identifier.
    connect(&m_amplifier, &AmpModel::operateRequested, this, [this](bool on){
        if (m_backend)
            m_backend->invokeExtension(QStringLiteral("flex"), QStringLiteral("amp.operate"), 0,
                                       QVariantMap{{QStringLiteral("on"), on}});
    });
    // Protocol-log breadcrumb on amp detection, symmetric with the "amplifier
    // removed" log — kept here so AmpModel stays logging-category-free. #4099.
    connect(&m_amplifier, &AmpModel::presenceChanged, this, [this](bool present){
        if (present)
            qCDebug(lcProtocol) << "RadioModel: power amplifier detected, model="
                                << m_amplifier.modelName() << "ip=" << m_amplifier.ip();
    });

    m_transmitModel.setPttPreflight([this](TransmitModel::PttSource source) {
        m_pendingTransmitPreflightSource = source;
        return localPttInterlockMessage(source);
    });
    connect(&m_transmitModel, &TransmitModel::pttBlocked,
            this, [this](const QString& message) {
        m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
        const QString panId = txSlice() ? txSlice()->panId() : QString();
        emitInterlockNotification(
            message,
            QStringLiteral("local-ptt:%1:%2").arg(panId, message),
            panId);
    });

    // The TX passband reaches a host-modulating backend through the seam, not
    // through the Flex verb next to it. Operator intent only — see the signal's
    // note — so this cannot echo radio state back as a command.
    connect(&m_transmitModel, &TransmitModel::txFilterCommandIssued, this,
            [this](int lowHz, int highHz) {
        if (m_backend && !usesFlexCommandPlane())
            m_backend->setTxFilter(lowHz, highHz);
    });

    // Mic gain reaches a host-modulating backend the same way and under the same
    // rule: operator intent only, and only where the Flex verb cannot land.
    connect(&m_transmitModel, &TransmitModel::micLevelCommandIssued, this,
            [this](int level) {
        if (m_backend && !usesFlexCommandPlane())
            m_backend->setMicGain(level);
    });

    // Forward transmit model commands to the radio
    connect(&m_transmitModel, &TransmitModel::commandReady, this, [this](const QString& cmd){
        const QString trimmed = cmd.trimmed();
        if (trimmed.startsWith(QStringLiteral("transmit set "), Qt::CaseInsensitive)) {
            const QMap<QString, QString> kvs =
                CommandParser::parseKVs(trimmed.mid(QStringLiteral("transmit set ").size()));
            if (kvs.contains(QStringLiteral("filter_low"))
                && kvs.contains(QStringLiteral("filter_high"))) {
                const QString message = txFilterFrequencyLimitMessage(
                    kvs.value(QStringLiteral("filter_low")).toInt(),
                    kvs.value(QStringLiteral("filter_high")).toInt());
                if (!message.isEmpty()) {
                    emitInterlockNotification(
                        message,
                        QStringLiteral("tx-filter:%1:%2")
                            .arg(kvs.value(QStringLiteral("filter_low")),
                                 kvs.value(QStringLiteral("filter_high"))));
                }
            }
        }

        static const QRegularExpression xmitRe(R"(^xmit\s+([01])\s*$)", QRegularExpression::CaseInsensitiveOption);
        const auto match = xmitRe.match(trimmed);
        if (match.hasMatch()) {
            const bool tx = (match.captured(1) == "1");
            if (tx) {
                if (transmitStartBlockedByInhibit(QStringLiteral("xmit"))) {
                    m_pendingTransmitPreflightSource =
                        TransmitModel::PttSource::Mox;
                    m_txRequested = false;
                    return;
                }
                armInterlockNotification(m_pendingTransmitPreflightSource);
                m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
            }
            m_txRequested = tx;
            if (!tx && m_txAudioGate) {
                m_txAudioGate = false;
                emit txAudioGateChanged(false);
            }
        }
        if (cmd == "transmit tune 1" || cmd == "atu start") {
            if (transmitStartBlockedByInhibit(QStringLiteral("tune-start"))) {
                return;
            }
            if (cmd == "atu start") {
                // Attribute the whole interlock cycle before dispatch. ATU and
                // interlock statuses are independent asynchronous planes, so
                // gating only on TUNE_IN_PROGRESS can briefly start (or
                // prematurely resume) the operator-over timer when those
                // statuses arrive out of order.
                //
                // Tag here, AFTER the inhibit gate above, and NOT in
                // TransmitModel::atuStart() (which — unlike startTune — has no
                // runPttPreflight of its own). Tagging before the gate would
                // leave a stale Atu source on a blocked ATU, which a following
                // bare hardware/VOX key (no source-bearing entry point) would
                // inherit and be wrongly excluded from the operator TX timer.
                m_transmitModel.noteActivePttSource(
                    TransmitModel::PttSource::Atu);
            }
            armInterlockNotification(m_pendingTransmitPreflightSource);
            m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
            applyTuneInhibit();
        }
        sendCmd(cmd);
    });

    // Forward equalizer model commands to the radio
    connect(&m_equalizerModel, &EqualizerModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    // Forward TNF intents to the backend, which decides whether a notch is a
    // radio feature (Flex: `tnf …` on the command plane) or host DSP (HL2:
    // WDSP's notched bandpass). Previously this forwarded SmartSDR text
    // straight to sendCmd, which is why the notch controls did nothing at all
    // on any non-Flex radio while still being offered.
    connect(&m_tnfModel, &TnfModel::notchCreateRequested, this,
            [this](double centerHz, double widthHz){
        // Connected only, and create is the one that needs saying so. The notch
        // controls stay live while disconnected (a capability is a fact about
        // the radio, not about the link), but the BACKEND survives a disconnect
        // — so on a host-DSP backend a create here would mint an id and report a
        // marker the operator can see, against a receiver set that no longer
        // exists. The next connect then wipes the backend's record and leaves
        // that marker on the panadapter with nothing behind it: the mirror image
        // of the state-outliving-the-session bug, arriving from the other side.
        if (m_backend && isConnected())
            m_backend->createNotch(centerHz, widthHz);
    });
    connect(&m_tnfModel, &TnfModel::notchChangeRequested, this,
            [this](int id, const NotchDelta& delta){
        if (m_backend)
            m_backend->setNotch(id, delta);
    });
    connect(&m_tnfModel, &TnfModel::notchRemoveRequested, this, [this](int id){
        if (m_backend)
            m_backend->removeNotch(id);
    });
    connect(&m_tnfModel, &TnfModel::notchesEnabledRequested, this, [this](bool on){
        if (m_backend)
            m_backend->setNotchesEnabled(on);
    });
    connect(&m_cwxModel, &CwxModel::commandReady, this, [this](const QString& cmd){
        // Track CWX send state so the interlock handler recognises local
        // CWX TX and doesn't force the audio gate off. (#2047, #2097)
        if (cmd.startsWith("cwx send") || cmd.startsWith("cwx macro send"))
            m_cwxActive = true;
        else if (cmd.startsWith("cwx clear")) {
            m_cwxActive = false;
            m_cwxDrainArmed = false;  // ESC/clear aborts the drain watch (#3949)
        }
        sendCmd(cmd);
    });
    // Final cwx send of each macro/text block goes via replyCommandReady so we
    // can capture the radio_index from the reply.  CwxModel::handleSendReply
    // stores it; applyStatus fires queueEmpty() when cwx sent= reaches it.
    // This replaces the broken cwx queue= path — firmware never sends it
    // (observed on FLEX-6500 fw 4.2.20.41343; the 8600 target runs 4.2.18). (#3949)
    connect(&m_cwxModel, &CwxModel::replyCommandReady, this, [this](const QString& cmd, int epoch, int nChars){
        m_cwxActive = true;
        // Arm the drain-release latch. Unlike m_cwxActive (which the interlock
        // handler clears on every TRANSMITTING→READY flicker during a macro),
        // m_cwxDrainArmed is owned solely by the CWX send/drain lifecycle, so
        // the queueEmpty release below survives QSK break-in flicker. (#3949)
        m_cwxDrainArmed = true;
        sendCmd(cmd, [this, epoch, nChars](int respVal, const QString& body){
            m_cwxModel.handleSendReply(respVal, body, epoch, nChars);
        });
    });
    // When the radio signals its CWX buffer is drained, release TX. (#2450)
    // The radio's break-in timer fires but sync_cwx=1 still requires an
    // explicit xmit 0 from the client — without it the radio holds TX for
    // its full hardware interlock timeout (~60 s).
    //
    // Gated on m_cwxDrainArmed, NOT m_cwxActive: setMox(false) unconditionally
    // emits `xmit 0`, so the release must only fire for a CWX batch we armed,
    // but the gate must not be the interlock-flicker-vulnerable m_cwxActive or
    // the release would be skipped mid-macro and TX would stick. queueEmpty()
    // itself only fires from CwxModel's armed watch (or a legacy queue= that
    // firmware never sends), so this pairing is the drain-release authority. (#3949)
    connect(&m_cwxModel, &CwxModel::queueEmpty, this, [this]() {
        if (!m_cwxDrainArmed) return;
        m_cwxDrainArmed = false;
        m_cwxActive = false;
        m_transmitModel.setMox(false);
    });
    // DVK commands are reply-aware (#3377): capture the verb + slot id so
    // the response code routes back to DvkModel, which forwards non-zero
    // responses to DvkPanel as commandFailed.  Before #3377 these were
    // fire-and-forget — the REC button toggled "checked" while the radio
    // had refused rec_start, leaving the user with no feedback.
    connect(&m_dvkModel, &DvkModel::replyCommandReady, this,
            [this](const QString& cmd, const QString& verb, int id){
        sendCmd(cmd, [this, verb, id](int respVal, const QString& body){
            m_dvkModel.handleCommandResponse(verb, id, static_cast<uint>(respVal), body);
        });
    });
    connect(&m_flexWaveformModel, &FlexWaveformModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });
    connect(&m_navtexModel, &NavtexModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });
    connect(&m_navtexModel, &NavtexModel::replyCommandReady, this, [this](const QString& cmd, int seq){
        sendCmd(cmd, [this, seq](int respVal, const QString& body){
            m_navtexModel.handleSendResponse(seq, static_cast<uint>(respVal), body);
        });
    });
    connect(&m_usbCableModel, &UsbCableModel::commandReady, this, [this](const QString& cmd){
        sendCmd(cmd);
    });

    // Tune PA inhibit: restore TX outputs when tune completes
    connect(&m_transmitModel, &TransmitModel::tuneChanged, this, [this](bool tuning) {
        if (!tuning && m_tuneInhibitActive && m_tuneInhibitBandId >= 0)
            restoreTuneInhibit();
    });

    // Drive the status-bar operator TX timer from actual transmit-state edges
    // (optimistic MOX/PTT plus interlock-driven VOX/footswitch/CW). The source
    // gate inside updateOperatorTransmit() keeps TUNE/ATU/TCI/DAX transmits out.
    connect(&m_transmitModel, &TransmitModel::transmittingChanged, this,
            [this](bool) { updateOperatorTransmit(); });
    // Also recompute when the TX-slice mode changes (phone↔CW) or first resolves
    // after connect: updateOperatorTransmit() gates on modeIsCw, which a
    // transmittingChanged edge alone can't catch mid-over. Idempotent — the
    // extra trigger only emits operatorTransmitChanged on a real edge. (#4131)
    connect(&m_transmitModel, &TransmitModel::txSliceModeChanged, this,
            [this](const QString&) { updateOperatorTransmit(); });

    m_reconnectTimer.setInterval(5000);
    // RFC #4603 PR 3: one settings write per burst of operating-state
    // changes — trailing debounce plus a max-wait so continuous tuning still
    // persists periodically instead of restarting the window forever
    // (PR #4619 review).
    m_operatingStateSaveTimer.setSingleShot(true);
    m_operatingStateSaveTimer.setInterval(2000);
    connect(&m_operatingStateSaveTimer, &QTimer::timeout, this,
            [this] { persistOperatingState(false); });
    m_operatingStateMaxWaitTimer.setSingleShot(true);
    m_operatingStateMaxWaitTimer.setInterval(10000);
    connect(&m_operatingStateMaxWaitTimer, &QTimer::timeout, this,
            [this] { persistOperatingState(false); });
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_intentionalDisconnect && !m_lastInfo.address.isNull()) {
            qCDebug(lcProtocol) << "RadioModel: auto-reconnecting to" << m_lastInfo.address.toString();
            // A retry is an attempt too (#4912). This path does NOT go through
            // connectToRadio() — it re-drives the connection/backend directly —
            // so the flag has to be set here as well or an armed reconnect
            // reads as "nothing is happening".
            m_connectAttemptActive = true;
            clearAutomationSliceFixtures();
            if (m_connection) {
                QMetaObject::invokeMethod(m_connection, [this] {
                    m_connection->connectToRadio(m_lastInfo);
                });
            } else if (m_backend) {
                // Non-Flex backend: re-drive the connect through the seam.
                RadioConnectRequest req;
                req.host   = m_lastInfo.address.toString();
                req.port   = m_lastInfo.port;
                req.serial = m_lastInfo.serial;
                // The RECONNECT path needs these too. Populating only the
                // initial connect gives a session that authenticates once and
                // then fails every automatic retry.
                populateFamilyParams(req, m_family);
                handRestoredStateToBackend(req.serial);
                m_backend->connectRadio(req);
            }
        } else {
            m_reconnectTimer.stop();
        }
    });

}

RadioModel::~RadioModel()
{
    // Disconnect RadioModel's own connections to the wire objects BEFORE they
    // are torn down, to prevent use-after-free (ASAN). (#502) The objects are
    // still alive here — the backend owns them and destroys them next. The WAN
    // connection also delivers statusReceived → handlePanadapterStatus / the
    // waterfall handler, both of which now deref m_flexBackend — sever it too so
    // a late WAN status can't reach a half-destroyed backend. (#4065 review)
    if (m_connection)                   // absent on a self-IQ backend
        QObject::disconnect(m_connection, nullptr, this, nullptr);
    if (m_panStream)                    // likewise
        QObject::disconnect(m_panStream, nullptr, this, nullptr);
    if (m_wanConn) {
        QObject::disconnect(m_wanConn, nullptr, this, nullptr);
    }

    // Destroy the backend, which owns the RadioConnection + PanadapterStream and
    // their worker threads: ~FlexBackend runs the exact #502 teardown ordering
    // (BlockingQueued disconnect/stop → deleteLater → thread quit/wait) that
    // used to live here. (aetherd 2.2b)
    ShutdownTrace trace("radio.backend.destroy");
    teardownBackend();
}

const DigitalVoiceWaveformMetrics& RadioModel::digitalVoiceWaveformMetrics() const
{
    return DigitalVoiceWaveformProcess::instance().metrics();
}

int RadioModel::rawModeOccurrenceCount(const QString& mode) const
{
    int count = 0;
    for (const QString& rawList : m_rawSliceModeLists) {
        for (const QString& rawMode : rawList.split(QLatin1Char(','),
                                                   Qt::SkipEmptyParts)) {
            if (rawMode.trimmed().compare(mode, Qt::CaseInsensitive) == 0) {
                ++count;
            }
        }
    }
    return count;
}

DigitalVoiceWaveformHealth RadioModel::digitalVoiceWaveformHealth() const
{
    return DigitalVoiceWaveformProcess::instance().health();
}

QString RadioModel::digitalVoiceWaveformHealthName() const
{
    return DigitalVoiceWaveformProcess::healthName(digitalVoiceWaveformHealth());
}

QString RadioModel::digitalVoiceWaveformHealthDetail() const
{
    return DigitalVoiceWaveformProcess::instance().healthDetail();
}

bool RadioModel::isConnected() const
{
    // Whoever carries the link reports its state: a RadioConnection when one
    // exists (Flex, and the demo's synthetic wire), otherwise the backend behind
    // the seam (HL2). Mirrors both the connect dispatch in connectToRadio() and
    // the lifecycle wiring in setupBackend().
    //
    // A previous revision keyed this on "!m_flexBackend" so the demo would answer
    // from SimBackend instead — because `sim disconnect` used to drop the backend's
    // state while leaving the synthetic connection "Connected", so this reported
    // connected=true forever with dead audio. That is now fixed at the source:
    // SimBackend::disconnectRadio() tears the synthetic connection down, so the
    // connection is truthful for the demo too.
    //
    // Keying on the connection also preserves teardownBackend()'s fail-closed
    // ordering: it nulls m_flexBackend BEFORE destroying the backend precisely so
    // a status slot running during teardown cannot dereference a half-destroyed
    // backend — which the "!m_flexBackend" form inverted into doing exactly that.
    if (m_connection)
        return m_connection->isConnected() || (m_wanConn && m_wanConn->isConnected());
    return m_backend && m_backend->isConnected();
}

int RadioModel::maxSlicesForModel(const QString& model)
{
    // Slice capacity per model comes from the FlexLib-sourced ModelCapabilities
    // table (SliceList size, Principle I) — the single source of model truth,
    // shared with maxPanadapters(), diversity, and extended-DSP gating.  This is
    // only the pre-connection estimate; the radio's live "slices=N" status
    // overrides m_maxSlices once connected.
    return capabilitiesFor(model).maxSlices;
}

SliceModel* RadioModel::slice(int id) const
{
    for (SliceModel* s : m_slices) {
        if (s && s->sliceId() == id) {
            return s;
        }
    }
    return nullptr;
}

bool RadioModel::isSlotOurs(int sliceId) const
{
    return slice(sliceId) != nullptr;
}

bool RadioModel::isSlotForeign(int sliceId) const
{
    return m_foreignSliceOwners.contains(sliceId);
}

QString RadioModel::foreignSliceOwnerStation(int sliceId) const
{
    auto it = m_foreignSliceOwners.constFind(sliceId);
    if (it == m_foreignSliceOwners.constEnd()) return {};
    return m_clientStations.value(it.value(), {});
}

void RadioModel::clearAutomationSliceFixtures()
{
    const QSet<int> fixtures = m_automationSliceFixtures;
    for (int sliceId : fixtures) {
        handleSliceStatus(sliceId, QMap<QString, QString>{}, true);
        m_ownedSliceIds.remove(sliceId);
        m_foreignSliceOwners.remove(sliceId);
    }
    m_automationSliceFixtures.clear();
    restoreAutomationSliceFixtureBaseline();
}

void RadioModel::restoreAutomationSliceFixtureBaseline()
{
    if (!m_automationSliceFixtureBaselineActive
        || !m_automationSliceFixtures.isEmpty()) {
        return;
    }

    bool changed = false;
    if (m_model != m_automationSliceFixtureBaselineModel) {
        m_model = m_automationSliceFixtureBaselineModel;
        changed = true;
    }
    if (m_maxSlices != m_automationSliceFixtureBaselineMaxSlices) {
        m_maxSlices = m_automationSliceFixtureBaselineMaxSlices;
        changed = true;
    }

    m_automationSliceFixtureBaselineActive = false;
    m_automationSliceFixtureBaselineModel.clear();
    m_automationSliceFixtureBaselineMaxSlices = 4;

    if (changed) {
        emit infoChanged();
    }
}

bool RadioModel::automationApplySliceFixture(int sliceId,
                                             const QString& radioLetter,
                                             QString* error)
{
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (isConnected()) {
        return fail(QStringLiteral("slice fixture is only available while disconnected"));
    }
    if (sliceId < 0 || sliceId >= 8) {
        return fail(QStringLiteral("slice fixture id must be 0..7"));
    }

    const QString trimmedLetter = radioLetter.trimmed();
    if (trimmedLetter.size() > 1) {
        return fail(QStringLiteral("slice fixture letter must be a single A..H letter"));
    }

    QString letter = trimmedLetter.toUpper();
    if (letter.isEmpty()) {
        letter = QString(QChar(static_cast<ushort>('A' + sliceId)));
    }
    const ushort letterCode = letter.at(0).unicode();
    if (letterCode < 'A' || letterCode > 'H') {
        return fail(QStringLiteral("slice fixture letter must be A..H"));
    }

    // Refuse to hijack a real slice (#4122 review). m_slices deliberately
    // survives an unexpected disconnect so the session can be reclaimed on
    // reconnect — a fixture applied over that id would decode fixture kvs
    // into the user's real SliceModel (visibly retuning it) and the eventual
    // fixture clear would DESTROY it, breaking reconnect continuity. Same for
    // a staged stale slice, which the create path would silently reclaim.
    if (!m_automationSliceFixtures.contains(sliceId)
        && (slice(sliceId) || m_staleSlices.contains(sliceId))) {
        return fail(QStringLiteral(
            "slice %1 already exists from the previous session — "
            "fixtures may not overwrite reclaimable slices").arg(sliceId));
    }

    if (!m_automationSliceFixtureBaselineActive) {
        m_automationSliceFixtureBaselineModel = m_model;
        m_automationSliceFixtureBaselineMaxSlices = m_maxSlices;
        m_automationSliceFixtureBaselineActive = true;
    }

    bool infoChangedNeeded = false;
    if (m_model.isEmpty() || maxSlicesForModel(m_model) <= sliceId) {
        m_model = QStringLiteral("FLEX-6700");
        infoChangedNeeded = true;
    }
    const int modelMaxSlices = maxSlicesForModel(m_model);
    if (m_maxSlices < modelMaxSlices) {
        m_maxSlices = modelMaxSlices;
        infoChangedNeeded = true;
    }
    if (m_maxSlices <= sliceId) {
        return fail(QStringLiteral("model %1 supports only %2 slices")
                        .arg(m_model)
                        .arg(m_maxSlices));
    }
    if (infoChangedNeeded) {
        emit infoChanged();
    }

    QMap<QString, QString> kvs;
    kvs.insert(QStringLiteral("in_use"), QStringLiteral("1"));
    kvs.insert(QStringLiteral("pan"), QStringLiteral("0x40000000"));
    kvs.insert(QStringLiteral("index_letter"), letter);
    kvs.insert(QStringLiteral("RF_frequency"), QStringLiteral("14.225000"));
    kvs.insert(QStringLiteral("mode"), QStringLiteral("USB"));
    kvs.insert(QStringLiteral("filter_lo"), QStringLiteral("100"));
    kvs.insert(QStringLiteral("filter_hi"), QStringLiteral("2700"));
    kvs.insert(QStringLiteral("active"), QStringLiteral("1"));
    kvs.insert(QStringLiteral("tx"), QStringLiteral("0"));
    kvs.insert(QStringLiteral("audio_level"), QStringLiteral("50"));
    kvs.insert(QStringLiteral("audio_pan"), QStringLiteral("50"));
    kvs.insert(QStringLiteral("audio_mute"), QStringLiteral("0"));
    kvs.insert(QStringLiteral("rxant"), QStringLiteral("ANT1"));
    kvs.insert(QStringLiteral("txant"), QStringLiteral("ANT1"));
    kvs.insert(QStringLiteral("rxant_list"), QStringLiteral("ANT1,ANT2"));
    kvs.insert(QStringLiteral("txant_list"), QStringLiteral("ANT1,ANT2"));

    m_ownedSliceIds.insert(sliceId);
    m_foreignSliceOwners.remove(sliceId);
    handleSliceStatus(sliceId, kvs, false);
    if (!slice(sliceId)) {
        m_ownedSliceIds.remove(sliceId);
        restoreAutomationSliceFixtureBaseline();  // self-guards on live fixtures
        return fail(QStringLiteral("slice fixture did not create slice %1").arg(sliceId));
    }
    m_automationSliceFixtures.insert(sliceId);

    // Deactivate sibling fixtures only after the new one verifiably exists —
    // deactivating first would leave no active slice if creation failed
    // (#4122 review). Mirrors the radio's single-active semantics.
    for (int existingId : std::as_const(m_automationSliceFixtures)) {
        if (existingId == sliceId) {
            continue;
        }
        handleSliceStatus(existingId,
                          {{QStringLiteral("active"), QStringLiteral("0")}},
                          false);
    }
    return true;
}

bool RadioModel::automationApplyGpsFixture(const GpsDelta& delta,
                                           const QString& referenceState,
                                           const QString& referenceSetting,
                                           bool referenceLocked,
                                           const QString& ntpServerAddress,
                                           QString* error)
{
    if (isConnected()) {
        if (error) {
            *error = QStringLiteral(
                "GPS fixture is only available while disconnected");
        }
        return false;
    }
    applyGpsChanges(delta);
    m_oscState = referenceState;
    m_oscSetting = referenceSetting;
    m_oscLocked = referenceLocked;
    m_automationGpsNtpServerAddress = ntpServerAddress;
    emit oscillatorChanged();
    return true;
}

bool RadioModel::automationRemoveSliceFixture(int sliceId, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (isConnected()) {
        return fail(QStringLiteral("slice fixture is only available while disconnected"));
    }
    if (sliceId < 0 || sliceId >= 8) {
        return fail(QStringLiteral("slice fixture id must be 0..7"));
    }
    if (!m_automationSliceFixtures.contains(sliceId)) {
        return fail(QStringLiteral("no slice fixture with id %1").arg(sliceId));
    }

    handleSliceStatus(sliceId, QMap<QString, QString>{}, true);
    m_ownedSliceIds.remove(sliceId);
    m_foreignSliceOwners.remove(sliceId);
    m_automationSliceFixtures.remove(sliceId);
    restoreAutomationSliceFixtureBaseline();
    return true;
}

int RadioModel::activeTxSliceNum() const
{
    for (auto* s : m_slices) {
        if (s && s->isTxSlice())
            return s->sliceId();
    }
    return -1;
}

void RadioModel::wireRxDemodAudioBus()
{
    // Exactly one producer, ever. Drop the previous binding first: on a family
    // swap the old PanadapterStream is usually destroyed (which would drop its
    // connection anyway), but a swap BETWEEN two seam backends re-enters here
    // with the same `this` on both ends and nothing would be dropped for us.
    QObject::disconnect(m_rxDemodBusConn);
    m_rxDemodBusConn = {};

    if (m_backend && m_backend->ownsRxAudio()) {
        // Seam-native audio (HL2 in-process demod, the sim's real demo audio).
        // Chained off backendAudioFrameReady rather than the backend's own
        // signal so both relays cross the thread boundary identically.
        m_rxDemodBusConn = connect(this, &RadioModel::backendAudioFrameReady,
                                   this, &RadioModel::rxDemodAudioReady);
        return;
    }
    if (m_panStream) {
        // Flex: the VITA-49 slice audio, unchanged and still feeding the engine
        // by its own existing connection. This is an ADDITIONAL subscriber to
        // the same signal, so the audible path is untouched.
        m_rxDemodBusConn = connect(m_panStream, &PanadapterStream::audioDataReady,
                                   this, &RadioModel::rxDemodAudioReady);
    }
}

// See the header for why this falls back and why the key is station-wide.
QString RadioModel::callsign() const
{
    const QString fromRadio = m_callsign.trimmed();
    if (!fromRadio.isEmpty())
        return fromRadio;
    return AppSettings::instance()
        .value(QStringLiteral("StationCallsign"), QString())
        .toString()
        .trimmed();
}

void RadioModel::setStationCallsign(const QString& callsign)
{
    const QString wanted = callsign.trimmed().toUpper();
    const QString before = this->callsign();
    AppSettings::instance().setValue(QStringLiteral("StationCallsign"), wanted);
    // Commit now rather than relying on the shutdown save: the whole point of
    // this setting is that it survives, and an operator who types a callsign
    // and then force-quits has done nothing wrong. Mirrors the nickname write
    // in RadioSetupDialog.
    AppSettings::instance().save();
    const QString after = this->callsign();
    if (after != before)
        emit callsignChanged(after);
}

QString RadioModel::antennaAliasRadioKey() const
{
    QString key = m_chassisSerial.trimmed();
    if (key.isEmpty())
        key = serial().trimmed();
    if (key.isEmpty())
        key = m_model.trimmed();
    if (key.isEmpty())
        key = m_name.trimmed();
    if (key.isEmpty())
        key = m_nickname.trimmed();
    return key.isEmpty() ? QStringLiteral("unconnected") : key;
}

bool RadioModel::reloadAntennaAliases() const
{
    const QString key = antennaAliasRadioKey();
    if (key == m_antennaAliasRadioKey)
        return false;

    const QMap<QString, QString> aliases = AntennaAliasStore::load(key);
    const bool changed = aliases != m_antennaAliases
        || key != m_antennaAliasRadioKey;
    m_antennaAliasRadioKey = key;
    m_antennaAliases = aliases;
    return changed;
}

QString RadioModel::antennaAlias(const QString& token) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::alias(m_antennaAliases, token);
}

QString RadioModel::antennaDisplayName(const QString& token,
                                       bool includeTokenForDisambiguation) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::displayName(
        m_antennaAliases, token, includeTokenForDisambiguation);
}

QString RadioModel::antennaShortDisplayName(const QString& token, int maxChars) const
{
    reloadAntennaAliases();
    return AntennaAliasStore::shortDisplayName(m_antennaAliases, token, maxChars);
}

QMap<QString, QString> RadioModel::antennaAliases() const
{
    reloadAntennaAliases();
    return m_antennaAliases;
}

bool RadioModel::antennaAliasNeedsDisambiguation(const QString& token,
                                                 const QStringList& tokens) const
{
    reloadAntennaAliases();
    const QString a = AntennaAliasStore::alias(m_antennaAliases, token);
    if (a.isEmpty())
        return false;

    int count = 0;
    for (const QString& other : tokens) {
        if (AntennaAliasStore::alias(m_antennaAliases, other) == a)
            ++count;
    }
    return count > 1;
}

void RadioModel::setAntennaAlias(const QString& token, const QString& alias)
{
    if (token.isEmpty())
        return;
    reloadAntennaAliases();

    const QString trimmedAlias = alias.trimmed();
    if (trimmedAlias.isEmpty()) {
        clearAntennaAlias(token);
        return;
    }

    if (m_antennaAliases.value(token) == trimmedAlias)
        return;

    m_antennaAliases.insert(token, trimmedAlias);
    AntennaAliasStore::save(m_antennaAliasRadioKey, m_antennaAliases);
    emit antennaAliasesChanged();
}

void RadioModel::clearAntennaAlias(const QString& token)
{
    if (token.isEmpty())
        return;
    reloadAntennaAliases();
    if (!m_antennaAliases.remove(token))
        return;

    AntennaAliasStore::save(m_antennaAliasRadioKey, m_antennaAliases);
    emit antennaAliasesChanged();
}

QStringList RadioModel::knownAntennaTokens() const
{
    reloadAntennaAliases();
    QStringList tokens;
    for (const QString& ant : m_antList)
        appendUniqueAntennaToken(tokens, ant);
    for (SliceModel* s : m_slices) {
        if (!s)
            continue;
        appendUniqueAntennaToken(tokens, s->rxAntenna());
        appendUniqueAntennaToken(tokens, s->txAntenna());
        for (const QString& ant : s->rxAntennaList())
            appendUniqueAntennaToken(tokens, ant);
        for (const QString& ant : s->txAntennaList())
            appendUniqueAntennaToken(tokens, ant);
    }
    for (auto it = m_antennaAliases.constBegin(); it != m_antennaAliases.constEnd(); ++it)
        appendUniqueAntennaToken(tokens, it.key());
    return tokens;
}

SliceModel* RadioModel::txSlice() const
{
    for (auto* s : m_slices) {
        if (s && s->isTxSlice())
            return s;
    }
    return nullptr;
}

void RadioModel::setPanTransmitInhibited(const QString& panId,
                                         bool inhibited,
                                         const QString& reason)
{
    const QString trimmedPanId = panId.trimmed();
    if (trimmedPanId.isEmpty()) {
        return;
    }

    if (!inhibited) {
        m_panTransmitInhibitReasons.remove(trimmedPanId);
        const bool hadRestoreSlice =
            m_panTransmitInhibitedTxSlices.contains(trimmedPanId);
        const int restoreSliceId = hadRestoreSlice
            ? m_panTransmitInhibitedTxSlices.take(trimmedPanId)
            : -1;
        if (hadRestoreSlice) {
            SliceModel* restoreSlice = slice(restoreSliceId);
            SliceModel* currentTxSlice = txSlice();
            const int currentTxSliceId = currentTxSlice
                ? currentTxSlice->sliceId()
                : -1;
            if (restoreSlice
                && TransmitInhibitPolicy::shouldRestoreInhibitedTxSlice(
                    trimmedPanId, restoreSlice->panId(),
                    sliceMayBelongToUs(restoreSliceId), restoreSliceId,
                    currentTxSliceId)) {
                sendSliceCommand(restoreSlice,
                                 QStringLiteral("slice set %1 tx=1")
                                     .arg(restoreSliceId));
            }
        }
        return;
    }

    const QString trimmedReason = reason.trimmed().isEmpty()
        ? QStringLiteral("Transmit is disabled because this panadapter is displaying receive-only data.")
        : reason.trimmed();
    if (!m_panTransmitInhibitedTxSlices.contains(trimmedPanId)) {
        if (SliceModel* currentTxSlice = txSlice();
            currentTxSlice && currentTxSlice->panId() == trimmedPanId
            && sliceMayBelongToUs(currentTxSlice->sliceId())) {
            m_panTransmitInhibitedTxSlices.insert(
                trimmedPanId, currentTxSlice->sliceId());
        }
    }
    if (m_panTransmitInhibitReasons.value(trimmedPanId) == trimmedReason) {
        return;
    }
    m_panTransmitInhibitReasons.insert(trimmedPanId, trimmedReason);
    enforceTransmitInhibitForPan(trimmedPanId);
}

bool RadioModel::panTransmitInhibited(const QString& panId) const
{
    return m_panTransmitInhibitReasons.contains(panId.trimmed());
}

QString RadioModel::panTransmitInhibitReason(const QString& panId) const
{
    return m_panTransmitInhibitReasons.value(panId.trimmed());
}

QString RadioModel::transmitInhibitMessageForSlice(const SliceModel* slice) const
{
    if (!slice) {
        return QString();
    }
    return panTransmitInhibitReason(slice->panId()).trimmed();
}

QString RadioModel::transmitInhibitMessageForTxSlice() const
{
    return transmitInhibitMessageForSlice(txSlice());
}

void RadioModel::enforceTransmitInhibitForPan(const QString& panId)
{
    if (!panTransmitInhibited(panId)) {
        return;
    }

    for (SliceModel* slice : m_slices) {
        if (slice && slice->panId() == panId
            && sliceMayBelongToUs(slice->sliceId())) {
            enforceTransmitInhibitForSlice(slice);
        }
    }
}

void RadioModel::enforceTransmitInhibitForSlice(SliceModel* slice)
{
    if (!slice || !slice->isTxSlice()
        || !sliceMayBelongToUs(slice->sliceId())) {
        return;
    }

    const QString message = transmitInhibitMessageForSlice(slice);
    if (message.isEmpty()) {
        return;
    }

    emitInterlockNotification(
        message,
        QStringLiteral("pan-tx-inhibit:%1").arg(slice->panId()),
        slice->panId());
    sendCmd(QStringLiteral("slice set %1 tx=0").arg(slice->sliceId()));
}

void RadioModel::selectSoleValidTxAntennaIfNeeded(SliceModel* slice, bool txAntennaStatusReceived)
{
    if (!slice || !txAntennaStatusReceived || !sliceMayBelongToUs(slice->sliceId())) {
        return;
    }

    const QStringList txAntennaList = slice->txAntennaList();
    if (txAntennaList.size() != 1) {
        return;
    }

    const QString allowedAntenna = txAntennaList.first().trimmed();
    const QString currentAntenna = slice->txAntenna().trimmed();
    if (allowedAntenna.isEmpty()
        || currentAntenna.isEmpty()
        || currentAntenna.compare(allowedAntenna, Qt::CaseInsensitive) == 0) {
        return;
    }

    qCInfo(lcProtocol) << "RadioModel: correcting invalid TX antenna for slice"
                       << slice->sliceId()
                       << "from" << currentAntenna
                       << "to sole allowed antenna" << allowedAntenna;
    slice->setTxAntenna(allowedAntenna);
}

bool RadioModel::transmitStartBlockedByInhibit(const QString& key)
{
    SliceModel* target = txSlice();
    const QString message = transmitInhibitMessageForSlice(target);
    if (message.isEmpty()) {
        return false;
    }

    const QString panId = target ? target->panId() : QString();
    emitInterlockNotification(
        message,
        QStringLiteral("pan-tx-inhibit:%1:%2").arg(panId, key),
        panId);
    m_transmitModel.setTransmitting(false);
    if (m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }
    return true;
}

void RadioModel::noteLocalTxSliceEnableIntent(int sliceId)
{
    if (sliceId < 0) {
        return;
    }

    for (auto it = m_panTransmitInhibitedTxSlices.begin();
         it != m_panTransmitInhibitedTxSlices.end();) {
        if (it.value() == sliceId) {
            ++it;
            continue;
        }

        it = m_panTransmitInhibitedTxSlices.erase(it);
    }
}

void RadioModel::sendSliceCommand(SliceModel* slice, const QString& cmd)
{
    const TransmitInhibitPolicy::SliceTxCommand txCommand =
        TransmitInhibitPolicy::parseSliceTxCommand(cmd);
    if (txCommand.valid && txCommand.txEnabled) {
        SliceModel* target = slice && slice->sliceId() == txCommand.sliceId
            ? slice
            : this->slice(txCommand.sliceId);
        const QString message = transmitInhibitMessageForSlice(target);
        if (!message.isEmpty()) {
            emitInterlockNotification(
                message,
                QStringLiteral("pan-tx-inhibit:%1").arg(target->panId()),
                target->panId());
            return;
        }
        noteLocalTxSliceEnableIntent(txCommand.sliceId);
    }

    sendCmd(cmd);
}

QString RadioModel::localPttInterlockMessage(TransmitModel::PttSource source) const
{
    // Capability first, ahead of every other test including the DAX bypass
    // below: a backend that reports it cannot transmit refuses the key under
    // the seam, so letting the request run its optimistic path (Quindar intro,
    // TX state, tune latch, the raw-TX edge) only builds a TX state the radio
    // never entered. TransmitModel consults this from runPttPreflight(), which
    // covers requestPttOn() — MOX, TCI hardware PTT, WSPR — and startTune().
    // Guarded on m_backend so a torn-down model behaves exactly as before;
    // setupBackend() otherwise guarantees one exists, and FlexBackend always
    // reports canTransmit=true, so no Flex path changes.
    if (m_backend && !backendCapabilities().canTransmit) {
        return tr("This radio is receive-only and cannot transmit.");
    }

    auto* s = txSlice();
    if (const QString message = transmitInhibitMessageForSlice(s);
        !message.isEmpty()) {
        return message;
    }

    // CAT/DAX PTT callers acknowledge the request before the asynchronous
    // model path runs, so legacy local voice-mode preflight must not silently
    // eat their PTT. Pan-level receive-only TX inhibits above still apply.
    // Otherwise let the radio be authoritative and report any interlock.
    if (source == TransmitModel::PttSource::Dax) {
        return QString();
    }

    if (!s) {
        return QStringLiteral("No transmit slice is assigned.");
    }

    const QString mode = s->mode().toUpper();
    const bool nonVoiceSource = (source == TransmitModel::PttSource::Tune
                              || source == TransmitModel::PttSource::TciHardware
                              || source == TransmitModel::PttSource::Dax
                              || source == TransmitModel::PttSource::Wspr
                              || s->sliceId() == m_digitalVoiceTxSliceId);
    if (!nonVoiceSource
        && (mode == QStringLiteral("DIGU") || mode == QStringLiteral("DIGL"))) {
        return QStringLiteral("You cannot transmit voice in DIGU/DIGL mode.");
    }

    if (source == TransmitModel::PttSource::Tune)
        return QString();

    return txFilterFrequencyLimitMessage(m_transmitModel.txFilterLow(),
                                         m_transmitModel.txFilterHigh());
}

QString RadioModel::txFilterFrequencyLimitMessage(int lowHz, int highHz) const
{
    auto* s = txSlice();
    if (!s)
        return QString();

    const double carrierMhz = s->frequency();
    if (carrierMhz <= 0.0)
        return QString();

    const QString bandName = BandSettings::bandForFrequency(carrierMhz);
    if (bandName == QStringLiteral("GEN") || bandName == QStringLiteral("WWV"))
        return QString();

    const BandDef& band = BandSettings::bandDef(bandName);
    if (band.lowMhz <= 0.0 || band.highMhz <= band.lowMhz)
        return QString();

    lowHz = qBound(0, lowHz, 9950);
    highHz = qBound(lowHz + 50, highHz, 10000);

    const QString mode = s->mode().toUpper();
    double txLowMhz = carrierMhz;
    double txHighMhz = carrierMhz;
    if (mode == QStringLiteral("LSB") || mode == QStringLiteral("DIGL")) {
        txLowMhz = carrierMhz - highHz / 1.0e6;
        txHighMhz = carrierMhz - lowHz / 1.0e6;
    } else if (mode == QStringLiteral("USB") || mode == QStringLiteral("DIGU")) {
        txLowMhz = carrierMhz + lowHz / 1.0e6;
        txHighMhz = carrierMhz + highHz / 1.0e6;
    } else if (mode == QStringLiteral("AM") || mode == QStringLiteral("SAM")) {
        txLowMhz = carrierMhz - highHz / 1.0e6;
        txHighMhz = carrierMhz + highHz / 1.0e6;
    } else {
        return QString();
    }

    constexpr double kEdgeToleranceMhz = 0.0000005; // 0.5 Hz
    if (txLowMhz < band.lowMhz - kEdgeToleranceMhz
        || txHighMhz > band.highMhz + kEdgeToleranceMhz) {
        return QStringLiteral("Your TX filter overlaps your frequency limits.");
    }

    return QString();
}

QString RadioModel::radioInterlockNotificationMessage(const QMap<QString, QString>& kvs) const
{
    const QString reason = kvs.value(QStringLiteral("reason")).toUpper();
    const QString state = kvs.value(QStringLiteral("state")).toUpper();

    auto withDebugName = [reason, state](const QString& message) {
        const QString debugName = reason.isEmpty() ? state : reason;
        return debugName.isEmpty()
            ? message
            : QStringLiteral("%1 (%2)").arg(message, debugName);
    };

    if (reason == QStringLiteral("OUT_OF_PA_RANGE")) {
        if (auto* s = txSlice()) {
            const QString txAnt = s->txAntenna().trimmed();
            const QStringList txAntList = s->txAntennaList();
            const bool selectedAntIsAllowed =
                txAnt.isEmpty()
                || std::any_of(txAntList.cbegin(), txAntList.cend(),
                               [&txAnt](const QString& candidate) {
                                   return candidate.compare(txAnt, Qt::CaseInsensitive) == 0;
                               });

            if (!txAnt.isEmpty() && !txAntList.isEmpty() && !selectedAntIsAllowed) {
                const QString validAntennas = txAntList.join(QStringLiteral(", "));
                return withDebugName(
                    QStringLiteral("%1 cannot transmit on this frequency. Allowed TX antennas: %2.")
                        .arg(txAnt, validAntennas));
            }
        }

        return withDebugName(
            QStringLiteral("The selected TX antenna cannot transmit on this frequency."));
    }

    if (reason == QStringLiteral("OUT_OF_BAND")
        || reason == QStringLiteral("TUNED_TOO_FAR")
        || reason == QStringLiteral("XVTR_RX_ONLY")) {
        return withDebugName(QStringLiteral("You cannot transmit on this frequency."));
    }

    if (reason == QStringLiteral("BAD_MODE")) {
        if (auto* s = txSlice()) {
            const QString mode = s->mode().toUpper();
            const bool nonVoiceSource =
                (m_interlockNotificationSource == TransmitModel::PttSource::Tune
                 || m_interlockNotificationSource == TransmitModel::PttSource::TciHardware
                 || m_interlockNotificationSource == TransmitModel::PttSource::Dax
                 || m_interlockNotificationSource == TransmitModel::PttSource::Wspr
                 || s->sliceId() == m_digitalVoiceTxSliceId);
            if (!nonVoiceSource
                && (mode == QStringLiteral("DIGU") || mode == QStringLiteral("DIGL"))) {
                return withDebugName(
                    QStringLiteral("You cannot transmit voice in DIGU/DIGL mode."));
            }
        }
        return withDebugName(QStringLiteral("You cannot transmit in this mode."));
    }

    if (reason == QStringLiteral("CLIENT_TX_INHIBIT"))
        return withDebugName(QStringLiteral("Transmit is inhibited for this band."));
    if (reason == QStringLiteral("NO_TX_ASSIGNED"))
        return withDebugName(QStringLiteral("No transmit slice is assigned."));
    if (reason == QStringLiteral("RCA_TXREQ"))
        return withDebugName(QStringLiteral("External RCA TX request is holding transmit."));
    if (reason == QStringLiteral("ACC_TXREQ"))
        return withDebugName(QStringLiteral("External ACC TX request is holding transmit."));
    if (reason == QStringLiteral("AMP:TG") || reason.contains(QStringLiteral("PG-XL")))
        return withDebugName(QStringLiteral("Amplifier interlock is blocking transmit."));

    if (state == QStringLiteral("TIMEOUT"))
        return withDebugName(QStringLiteral("Transmit timed out."));
    if (state == QStringLiteral("STUCK_INPUT"))
        return withDebugName(QStringLiteral("PTT input is stuck active."));
    if (state == QStringLiteral("TX_FAULT"))
        return withDebugName(QStringLiteral("Transmit interlock fault."));

    return QString();
}

void RadioModel::armInterlockNotification(TransmitModel::PttSource source)
{
    m_interlockNotificationArmedUntilMs = QDateTime::currentMSecsSinceEpoch() + 6000;
    m_interlockNotificationSource = source;
}

bool RadioModel::interlockNotificationArmed() const
{
    return QDateTime::currentMSecsSinceEpoch() <= m_interlockNotificationArmedUntilMs;
}

void RadioModel::emitInterlockNotification(const QString& message,
                                           const QString& key,
                                           const QString& panId)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty())
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString effectiveKey = key.isEmpty() ? trimmed : key;
    if (effectiveKey == m_lastInterlockNotificationKey
        && now - m_lastInterlockNotificationMs < 5000) {
        return;
    }

    m_lastInterlockNotificationKey = effectiveKey;
    m_lastInterlockNotificationMs = now;
    emit interlockNotificationRequested(trimmed, effectiveKey, panId.trimmed());
}

// ─── Actions ──────────────────────────────────────────────────────────────────

void RadioModel::connectToRadio(const RadioInfo& info)
{
    // aetherd Gap B: the backend follows the radio the operator selected. A Flex
    // and a Hermes-Lite 2 need different backends, and the picker is where that
    // choice is actually made — so swap the backend here rather than pinning the
    // family for the whole process. Same-family reconnects rebuild nothing.
    const QString wantFamily = info.family.isEmpty() ? QStringLiteral("flex")
                                                     : info.family.toLower();
    if (wantFamily != m_family) {
        qCInfo(lcProtocol) << "RadioModel: switching backend family" << m_family
                           << "->" << wantFamily << "for" << info.address.toString();
        // F1 (#4448): a family switch is a hard radio change — drop every live and
        // staged slice/pan model so none can be reclaimed as a model of the new
        // family with mismatched (or missing) command/TX/DV wiring.
        dropAllSessionModelsForFamilySwitch();
        teardownBackend();
        setupBackend(wantFamily);
        emit backendRebuilt();
    }

    // An attempt is in flight from here until it lands, fails, or is abandoned
    // (#4912). Set after the family switch above so a backend rebuild — which
    // tears the old backend down and can emit a disconnect — cannot clear the
    // flag we are about to set.
    m_connectAttemptActive = true;

    clearAutomationSliceFixtures();
    m_automationGpsNtpServerAddress.clear();

    // RFC #4288 Route A: the demo is selected by its FAMILY ("sim") in the block
    // above, exactly like HL2 — no separate demo path here. An earlier revision
    // kept a parallel rebuildBackendForTarget(bool) alongside the family switch;
    // the two disagreed (the demo RadioInfo carried the default family "flex"),
    // so the family switch built a FlexBackend for the demo and the bool then
    // early-returned believing the target was already correct. It also emitted
    // only backendChanged, never backendRebuilt, leaving the rebuilt
    // PanadapterStream with no sinks bound after a demo↔real swap. One selector.

    m_wanConn = nullptr;  // LAN mode
    m_lastInfo = info;
    m_intentionalDisconnect = false;
    m_forcedDisconnectInProgress = false;
    // Note: m_rebootInProgress is NOT cleared here — connectToRadio() runs
    // again from the reconnect timer during a reboot, and we want to keep
    // suppressing toasts until onConnected() actually fires.
    m_announcedClientConnections.clear();
    m_reconnectTimer.stop();
    m_name    = info.name;
    m_model   = info.model;
    m_version = info.version;
    m_versionLabel = info.versionLabel;
    // Seed nickname/callsign from the discovery packet so the status-bar station
    // label is correct the instant onConnectionStateChanged(true) reads it. These
    // were previously only set later from the async "info" reply, so on connect
    // the label showed a STALE m_nickname — blank on the first connect, or the
    // PREVIOUSLY connected radio's name (it is never cleared on disconnect). The
    // async reply still refreshes them if they differ.
    m_nickname = info.nickname;
    m_callsign = info.callsign;
    m_declaredBands = parseDeclaredBands(info.bands);   // empty for real Flex
    m_maxSlices = maxSlicesForModel(m_model);
    if (reloadAntennaAliases())
        emit antennaAliasesChanged();
    setKnownGuiClients(info.guiClientHandles,
                       info.guiClientPrograms,
                       info.guiClientStations,
                       info.guiClientIps,
                       info.guiClientHosts);
    // The demo and Flex both go through the RadioConnection: the demo's
    // connection (owned by SimBackend, RFC #4288 Route A hybrid) detects the demo
    // serial in connectToRadio() and runs its synthetic-demo connect locally
    // instead of dialing a socket, while a real radio dials as before. Families
    // that own no RadioConnection (HL2) take the seam path below.
    if (m_connection) {
        QMetaObject::invokeMethod(m_connection, [conn = m_connection, info] {
            conn->connectToRadio(info);
        });
    } else if (m_backend) {
        // aetherd Gap B (Step 2b): non-Flex families have no RadioConnection; they
        // connect through the neutral IRadioBackend seam. The backend emits
        // connected()/disconnected() (wired above for the m_connection-null case).
        if (!info.family.isEmpty() && info.family != QLatin1String("flex"))
            qCInfo(lcProtocol) << "RadioModel: connecting" << info.family
                               << "radio via IRadioBackend seam at"
                               << info.address.toString();
        RadioConnectRequest req;
        req.host   = info.address.toString();
        req.port   = info.port;
        req.serial = info.serial;
        populateFamilyParams(req, info.family);
        handRestoredStateToBackend(req.serial);
        m_backend->connectRadio(req);
    }
}

void RadioModel::connectViaWan(WanConnection* wan, const QString& publicIp, quint16 udpPort)
{
    qCDebug(lcProtocol) << "RadioModel: connectViaWan publicIp=" << publicIp
             << "udpPort=" << udpPort
             << "wanHandle=0x" << QString::number(wan->clientHandle(), 16);

    m_connectAttemptActive = true;  // see connectToRadio() (#4912)

    clearAutomationSliceFixtures();
    m_automationGpsNtpServerAddress.clear();

    // Disconnect any stale signal connections from a previous WAN session
    if (m_wanConn)
        m_wanConn->disconnect(this);

    m_wanConn = wan;
    m_wanPublicIp = publicIp;
    m_wanUdpPort = udpPort;
    m_intentionalDisconnect = false;
    m_forcedDisconnectInProgress = false;
    // Note: m_rebootInProgress is NOT cleared here — connectToRadio() runs
    // again from the reconnect timer during a reboot, and we want to keep
    // suppressing toasts until onConnected() actually fires.
    m_announcedClientConnections.clear();
    m_reconnectTimer.stop();

    // Wire WAN connection signals (same as RadioConnection)
    connect(wan, &WanConnection::connected, this, &RadioModel::onConnected);
    connect(wan, &WanConnection::disconnected, this, &RadioModel::onDisconnected);
    connect(wan, &WanConnection::errorOccurred, this, &RadioModel::onConnectionError);
    connect(wan, &WanConnection::certFingerprintMismatch,
            this, &RadioModel::certFingerprintMismatch);
    connect(wan, &WanConnection::versionReceived, this, &RadioModel::onVersionReceived);
    connect(wan, &WanConnection::messageReceived, this, &RadioModel::onMessageReceived);
    connect(wan, &WanConnection::statusReceived, this, &RadioModel::onStatusReceived);
    connect(wan, &WanConnection::pingRttMeasured, this, [this](int ms) {
        m_pingMissCount = 0;
        m_lastPingRtt = ms;
        evaluateNetworkQuality();
        emit pingReceived();
    });

    // The WAN connection is already established (TLS + wan validate done)
    // and has already received V/H. Trigger onConnected manually.
    if (wan->isConnected()) {
        qCDebug(lcProtocol) << "RadioModel: WAN already connected, triggering onConnected";
        onConnected();
    } else {
        qCDebug(lcProtocol) << "RadioModel: WAN not yet connected, waiting for connected signal";
    }
}

void RadioModel::setPendingClientDisconnects(const QList<quint32>& handles)
{
    m_pendingClientDisconnects.clear();
    for (quint32 handle : handles) {
        if (handle != 0 && !m_pendingClientDisconnects.contains(handle))
            m_pendingClientDisconnects.append(handle);
    }
}

bool RadioModel::disconnectClient(quint32 handle)
{
    if (handle == 0 || handle == clientHandle())
        return false;

    disconnectClientHandlesThen({handle});
    return true;
}

void RadioModel::setKnownGuiClients(const QStringList& handles,
                                    const QStringList& programs,
                                    const QStringList& stations,
                                    const QStringList& ips,
                                    const QStringList& hosts)
{
    applyKnownGuiClients(handles, programs, stations, ips, hosts, true);
}

void RadioModel::mergeKnownGuiClients(const QStringList& handles,
                                      const QStringList& programs,
                                      const QStringList& stations,
                                      const QStringList& ips,
                                      const QStringList& hosts)
{
    applyKnownGuiClients(handles, programs, stations, ips, hosts, false);
}

void RadioModel::applyKnownGuiClients(const QStringList& handles,
                                      const QStringList& programs,
                                      const QStringList& stations,
                                      const QStringList& ips,
                                      const QStringList& hosts,
                                      bool replaceExisting)
{
    if (replaceExisting) {
        m_clientStations.clear();
        m_clientInfoMap.clear();
        m_startupClientConnections.clear();
        // Foreign-slot dimming markers belong to per-handle state; clear
        // them alongside so a re-sync doesn't carry stale Multi-Flex
        // occupancy across the reset (#2606).
        m_foreignSliceOwners.clear();
    }

    for (int i = 0; i < handles.size(); ++i) {
        const quint32 handle = parseClientHandle(handles[i]);
        if (handle == 0)
            continue;
        if (replaceExisting)
            m_startupClientConnections.insert(handle);

        const QString program = i < programs.size()
            ? cleanClientText(programs[i])
            : QStringLiteral("Unknown");
        const QString station = i < stations.size()
            ? cleanClientText(stations[i])
            : program;
        QString source = i < ips.size()
            ? cleanClientText(ips[i])
            : QString();
        if (source.isEmpty() && i < hosts.size())
            source = cleanClientText(hosts[i]);

        ClientInfo client = m_clientInfoMap.value(handle);
        if (!station.isEmpty())
            client.station = station;
        if (!program.isEmpty() && program != QStringLiteral("Unknown"))
            client.program = program;
        if (!source.isEmpty())
            client.source = source;

        m_clientStations[handle] = client.station.isEmpty() ? client.program : client.station;
        m_clientInfoMap[handle] = client;
    }
}

void RadioModel::armClientConnectionNoticeSuppression()
{
    m_clientConnectionNoticeTimer.restart();
}

bool RadioModel::clientConnectionNoticeSuppressionActive() const
{
    return m_clientConnectionNoticeTimer.isValid()
        && m_clientConnectionNoticeTimer.elapsed() < CLIENT_CONNECTION_STARTUP_SUPPRESS_MS;
}

bool RadioModel::shouldSuppressRadioMessageNotice(const QString& text, MessageSeverity severity) const
{
    return severity == MessageSeverity::Info
        && clientConnectionNoticeSuppressionActive()
        && isRoutineClientConnectionInfo(text);
}

bool RadioModel::shouldSuppressClientConnectionNotice(quint32 handle)
{
    if (handle == 0 || handle == clientHandle())
        return true;

    if (m_startupClientConnections.remove(handle)) {
        m_announcedClientConnections.insert(handle);
        return true;
    }

    if (clientConnectionNoticeSuppressionActive()) {
        m_announcedClientConnections.insert(handle);
        return true;
    }

    return false;
}

void RadioModel::announceClientConnection(quint32 handle,
                                          const QString& source,
                                          const QString& station,
                                          const QString& program)
{
    if (handle == clientHandle() || m_announcedClientConnections.contains(handle))
        return;

    m_announcedClientConnections.insert(handle);
    QTimer::singleShot(750, this, [this, handle, source, station, program] {
        if (!m_clientInfoMap.contains(handle))
            return;

        const auto client = m_clientInfoMap.value(handle);
        QString latestSource = client.source.isEmpty() ? source : client.source;
        QString latestStation = client.station.isEmpty() ? station : client.station;
        QString latestProgram = client.program.isEmpty() ? program : client.program;

        if (m_wanConn && (latestSource.isEmpty() || latestSource == QStringLiteral("SmartLink"))) {
            QTimer::singleShot(1250, this, [this, handle, latestSource, latestStation, latestProgram] {
                if (!m_clientInfoMap.contains(handle))
                    return;

                const auto client = m_clientInfoMap.value(handle);
                emit clientConnected(handle,
                                     client.source.isEmpty() ? latestSource : client.source,
                                     client.station.isEmpty() ? latestStation : client.station,
                                     client.program.isEmpty() ? latestProgram : client.program);
            });
            return;
        }

        emit clientConnected(handle, latestSource, latestStation, latestProgram);
    });
}

void RadioModel::disconnectFromRadio()
{
    m_intentionalDisconnect = true;
    m_rebootInProgress = false;
    m_connectAttemptActive = false;  // the operator abandoned it (#4912)
    m_reconnectTimer.stop();
    m_pingTimer.stop();
    if (m_wanConn) {
        WanConnection* wan = m_wanConn;
        wan->disconnect(this);  // remove stale signal connections before adding the one-shot teardown (#224)
        connect(wan, &WanConnection::disconnected, this, [this, wan]() {
            if (m_wanConn == wan) {
                onDisconnected();
            }
        }, Qt::SingleShotConnection);
        wan->disconnectFromRadio();
        if (wan->isSocketIdle() && m_wanConn == wan) {
            onDisconnected();
        }
    } else if (!m_connection) {
        // aetherd Gap B (Step 2b): non-Flex families have no RadioConnection; tear
        // down through the neutral seam. The backend emits disconnected() -> our
        // onDisconnected (wired for the m_connection-null case in the ctor).
        if (m_backend)
            m_backend->disconnectRadio();
    } else if (m_connection->isConnected()) {
        // Graceful disconnect: remove our stream and wait for the radio reply
        // before closing. Self "client disconnect" is rejected by the radio.
        quint32 handle = clientHandle();
        QString streamId = RadioStatusOwnership::streamCommandId(m_rxAudio.streamId);
        const quint32 streamRemoveSeq = streamId.isEmpty() ? 0 : m_seqCounter.fetch_add(1);
        QMetaObject::invokeMethod(m_connection, [this, handle, streamId,
                                                 streamRemoveSeq]() {
            m_connection->gracefulDisconnect(handle, streamId, streamRemoveSeq);
        }, Qt::BlockingQueuedConnection);
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
    }
}

void RadioModel::acceptPresentedWanCert()
{
    if (m_wanConn)
        m_wanConn->acceptPresentedCert();
}

void RadioModel::rejectPresentedWanCert()
{
    if (m_wanConn)
        m_wanConn->rejectPresentedCert();
}

void RadioModel::forceDisconnect()
{
    // Close TCP/TLS without setting m_intentionalDisconnect so the UI can
    // start the normal unexpected-disconnect reconnect path.
    m_connectAttemptActive = false;  // this attempt is over; the retry re-arms it (#4912)
    if (m_wanConn) {
        m_wanConn->disconnectFromRadio();
    } else if (!m_connection) {
        // F3 (#4448): a non-Flex backend has no RadioConnection; tear down through
        // the seam. Without this guard the m_connection->isConnected() below
        // null-derefs (~250 ms after a reboot request, or on any forced drop).
        if (m_backend)
            m_backend->disconnectRadio();
    } else if (m_connection->isConnected()) {
        quint32 handle = clientHandle();
        QMetaObject::invokeMethod(m_connection, [conn = m_connection, handle]() {
            conn->gracefulDisconnect(handle, QString(), 0);
        });
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio);
    }
}

void RadioModel::rebootRadio()
{
    // Gate on isConnected() (which already covers WAN/SmartLink sessions), not
    // the LAN socket alone — sendCommand() already routes through m_wanConn
    // for WAN, so a SmartLink user clicking Reboot should send the command
    // and tear the link down the same way as a LAN user.
    if (!isConnected()) {
        return;
    }
    // F3 (#4448): "radio reboot" is a SmartSDR command. A backend that does not
    // support a client-triggered reboot (HL2) leaves canReboot=false; refuse so
    // we neither send a meaningless command nor schedule the forceDisconnect that
    // would follow. The UI also disables the button on this capability.
    if (!backendCapabilities().canReboot) {
        return;
    }
    m_rebootInProgress = true;
    sendCommand(QStringLiteral("radio reboot"));
    // Give the TCP write a brief moment to flush before tearing down the
    // socket, then drop into the unexpected-disconnect path so the existing
    // reconnect timer brings us back when the radio is up again.
    QTimer::singleShot(250, this, &RadioModel::forceDisconnect);
    // Fail-open safety: if the reboot wedges the radio's network stack, the
    // reconnect timer keeps firing "connection refused" forever and the user
    // sees no toasts at all because m_rebootInProgress is gating them. Time
    // the suppression out after 60s so a stuck radio surfaces real errors
    // instead of silently retrying forever. 60s comfortably covers a healthy
    // 6000/8600 boot.
    QTimer::singleShot(60'000, this, [this] {
        if (m_rebootInProgress) {
            m_rebootInProgress = false;
        }
    });
}

RadioCapabilities RadioModel::backendCapabilities() const
{
    return m_backend ? m_backend->capabilities() : RadioCapabilities{};
}

bool RadioModel::hasExtendedDspFilters() const
{
    // Connected: the backend's declaration wins. For a Flex this is bit-for-bit
    // the previous answer — FlexBackend::capabilities() computes
    // hasExtendedDsp as capabilitiesFor(model).hasExtendedDsp(), against the
    // same model string this object holds (it IS m_model, handed over by the
    // setModelProvider lambda in setupBackend). Same table, same key, same
    // result; what changes is only that the value now travels through the seam.
    if (m_backend && isConnected()) {
        return backendCapabilities().hasExtendedDsp;
    }
    // Disconnected or unknown: fall back to the model-name table, so a session
    // restored from settings still shows the right filters before a backend has
    // reported anything.
    return capabilitiesFor(m_model).hasExtendedDsp();
}

bool RadioModel::hasRadioSideDsp() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasRadioSideDsp;
}

bool RadioModel::hasLmsNoiseFilters() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasLmsNoiseFilters;
}

bool RadioModel::hasManualNotch() const
{
    // NOT permissive — see the header. A button nothing has claimed stays off.
    if (!m_backend || !isConnected()) {
        return false;
    }
    return backendCapabilities().hasManualNotch;
}

bool RadioModel::hasHostNoiseBlanker() const
{
    // NOT permissive, for the same reason hasManualNotch() is not: this flag
    // can only ADD the NB button, so answering true with no backend attached
    // would show it on a family that never claims it.
    if (!m_backend || !isConnected()) {
        return false;
    }
    return backendCapabilities().hasHostNoiseBlanker;
}

QList<int> RadioModel::radioFilterWidthsHz() const
{
    if (!m_backend || !isConnected()) {
        return {};
    }
    return backendCapabilities().rxFilterWidthsHz;
}

bool RadioModel::hasRadioSideWaterfallAutoBlack() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasRadioSideWaterfallAutoBlack;
}

bool RadioModel::hasRadioSideCwKeyer() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasRadioSideCwKeyer;
}

bool RadioModel::hasVoiceKeyer() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasVoiceKeyer;
}

bool RadioModel::hasDaxStreams() const
{
    if (!m_backend || !isConnected()) {
        return true;   // nothing attached — assume present, see the header
    }
    return backendCapabilities().hasDaxStreams;
}

// The single fan-out point for "what this radio says it can do".
//
// Everything capability-driven — model-side flags pushed into TransmitModel, and
// the capabilitiesChanged relay the GUI binds to — is published from here, so
// the connect edge and a mid-session revision by the backend take identical
// paths. Adding a capability means adding one line here, not another
// connect-time lambda.
void RadioModel::publishCapabilities(bool connected)
{
    const RadioCapabilities caps = backendCapabilities();

    // Capability-driven, not family()!="flex": only a backend that both
    // host-modulates and may transmit collapses the mic source to PC. (#4449)
    m_transmitModel.setHostModulation(connected
                                      && caps.hostModulates && caps.canTransmit);
    // Whether an antenna tuner exists at all. Same shape and the same
    // reason: the capability is the backend's to report, and the widgets
    // that need it only see the model.
    //
    // Restored to TRUE on disconnect rather than left false — with no radio
    // connected there is nothing to be honest ABOUT, and leaving the ATU
    // greyed out after unplugging an HL2 would look like a fault. Every
    // capability below follows the same `!connected || caps.x` shape.
    m_transmitModel.setHasTuner(!connected || caps.hasTuner);

    emit capabilitiesChanged(connected, caps);
}

IRadioBackend::HealthSnapshot RadioModel::backendHealthSnapshot() const
{
    return m_backend ? m_backend->healthSnapshot()
                     : IRadioBackend::HealthSnapshot{};
}

// Shared key-on guard for the paths that do NOT go through setTransmit().
//
// setTransmit() refuses a key on a backend reporting canTransmit=false before
// touching any state; MOX and TUNE reach the seam through
// mox/tuneCommandIssued instead and had no such test. On an HL2 with the
// automation TX gate closed, Hl2Backend::setKeying() logged "key refused" and
// returned while this side still flipped m_radioTransmitting — so TciServer
// broadcast trx:...,true; for a transmission that never happened, and its
// "already transmitting" guard then rejected the next genuine TCI key. Nothing
// on the air, and TCI keying dead until the flag cleared.
//
// Returns true when keying may proceed. On refusal it rolls the optimistic
// transmit state back and raises the same interlock notification setTransmit()
// uses, so the operator sees one consistent reason on every path.
bool RadioModel::refuseKeyOnTransmitIncapableBackend()
{
    if (backendCapabilities().canTransmit)
        return true;

    emitInterlockNotification(
        tr("This radio is receive-only and cannot transmit."),
        QStringLiteral("rx-only-tx"),
        txSlice() ? txSlice()->panId() : QString());
    m_transmitModel.setTransmitting(false);
    return false;
}

void RadioModel::setTransmit(bool tx, TransmitModel::PttSource source)
{
    if (tx) {
        // F2 (#4448): refuse keying on a backend that cannot transmit. The
        // guard is a capability test, not a family test — HL2 is TX-capable
        // now (see the transmit gate in Hl2Backend), and what still trips this
        // is any backend reporting canTransmit=false: an RX-only receiver, or
        // an HL2 whose transmit gate is closed. This must come BEFORE any
        // optimistic state mutation or command, so such a radio can never be
        // driven into a fake TX state (TX indicator, audio gate, waveform) by a
        // PTT/MOX/DAX/TCI edge. Unkey (tx=false) is always allowed — it only
        // ever clears state.
        if (!backendCapabilities().canTransmit) {
            emitInterlockNotification(
                tr("This radio is receive-only and cannot transmit."),
                QStringLiteral("rx-only-tx"),
                txSlice() ? txSlice()->panId() : QString());
            m_transmitModel.setTransmitting(false);
            return;
        }
        const QString message = localPttInterlockMessage(source);
        if (!message.isEmpty()) {
            const QString panId = txSlice() ? txSlice()->panId() : QString();
            emitInterlockNotification(
                message,
                QStringLiteral("local-ptt:%1:%2").arg(panId, message),
                panId);
            m_transmitModel.setTransmitting(false);
            return;
        }
        armInterlockNotification(source);
        // Record who initiated this key-up so the status-bar TX timer can tell
        // an operator MOX/PTT from a TCI-hardware or DAX transmit (#tx-timer).
        m_transmitModel.noteActivePttSource(source);
    }

    // Track local intent so we can keep TX gating aligned with user/PTT edges
    // while radio interlock transitions through intermediate states.
    m_txRequested = tx;

    // Optimistic edge gating:
    // - TX on: start immediately to keep modem waveform aligned with PTT edge.
    // - TX off: stop immediately to avoid "stuck TX tail" during UNKEY_REQUESTED.
    m_transmitModel.setTransmitting(tx);
    if (!tx && m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }

    if (tx) {
        // The waveform helper is a separate radio client, so its slice tx=
        // field is not a reliable view of this GUI client's TX assignment.
        // Put the radio-authoritative selection ahead of xmit on our command
        // stream so D-STAR is emitted only when that selected slice is DSTR.
        syncDigitalVoiceTxSelection(true);
    }
    // Key through the SEAM, not with a raw Flex command. This used to be
    // sendCmd("xmit N"), which meant IRadioBackend::setKeying had no callers at
    // all and no non-Flex backend could ever be keyed -- the verb existed and
    // was wired to nothing.
    //
    // Behaviour for Flex is unchanged: FlexBackend::setKeying sends the exact
    // same "xmit N" through the same sink, and the only extra gate on that path
    // matches "display pan set ", not "xmit".
    if (m_backend)
        m_backend->setKeying(tx);

    publishBackendTransmitEdge(tx);
}

// Publish the raw TX edge for a backend that has no interlock status plane.
//
// m_radioTransmitting is otherwise decoded exclusively from Flex `interlock`
// status, so on such a backend it stays false forever. That is not a cosmetic
// gap: TciServer waits on radioTransmittingChanged to CONFIRM a TCI key and
// gives up 1250 ms later — WSJT-X's transmission was being unkeyed a second and
// a quarter in, every time.
//
// Our own command is the authoritative edge here precisely because there is no
// status plane to be authoritative instead: no other client shares this radio,
// and the backend's own transmit gate has already been consulted before either
// caller reaches this point. If a backend later reports hardware PTT or an
// interlock of its own, it should drive this through
// IRadioBackend::transmitChanged and this fallback should yield to it.
//
// Flex is excluded: there the edge is decoded from interlock status, and a
// second source would race it.
void RadioModel::publishBackendTransmitEdge(bool tx)
{
    if (!m_backend || m_flexBackend || m_radioTransmitting == tx)
        return;
    m_radioTransmitting = tx;
    emit radioTransmittingChanged(tx);
}

void RadioModel::updateOperatorTransmit()
{
    // On a full unkey, forget the remembered PTT source. A subsequent
    // hardware-mic PTT, footswitch, or VOX key never flows through a
    // source-bearing entry point (the radio just starts transmitting), so
    // without this reset it would inherit a stale ATU/TCI/DAX tag and be
    // wrongly excluded from the operator TX timer.
    if (!m_transmitModel.isTransmitting()
        && m_transmitModel.activePttSource() != TransmitModel::PttSource::Mox) {
        m_transmitModel.noteActivePttSource(TransmitModel::PttSource::Mox);
    }

    // TransmitModel::isTransmitting() already tracks only owned mic/manual TX —
    // the interlock handler forces it false for DAX and other-client TX. The
    // only owned path we must additionally exclude is TCI-hardware PTT, which
    // the radio reports as source=SW and so is indistinguishable at the
    // interlock level; the remembered source disambiguates it. m_daxTxActive is
    // a belt-and-suspenders guard for the optimistic DAX key edge.
    const TransmitModel::PttSource src = m_transmitModel.activePttSource();
    // CW (incl. any CWU/CWL variant) is excluded — see operatorTransmitActive.
    // Prefer the TX slice's live mode; fall back to the mode the radio echoes in
    // its transmit status when there is no resolvable TX slice.
    const SliceModel* ts = txSlice();
    const QString txMode = (ts ? ts->mode() : m_transmitModel.txSliceMode())
                               .trimmed().toUpper();
    const bool modeIsCw = txMode.startsWith(QStringLiteral("CW"));
    const bool sourceIsTuneCarrier =
        src == TransmitModel::PttSource::Tune
        || src == TransmitModel::PttSource::Atu;
    const bool op = RadioStatusOwnership::operatorTransmitActive(
        m_transmitModel.isTransmitting(),
        m_daxTxActive,
        src == TransmitModel::PttSource::TciHardware,
        src == TransmitModel::PttSource::Dax,
        sourceIsTuneCarrier,
        modeIsCw);

    if (op == m_operatorTransmitting)
        return;
    m_operatorTransmitting = op;
    emit operatorTransmitChanged(op);
}

void RadioModel::setDigitalVoiceTxSlice(int sliceId)
{
    m_digitalVoiceTxSliceId = sliceId;
}

void RadioModel::scheduleDStarRuntimeConfiguration()
{
    m_dstarRuntimeConfigurationPending = true;
    applyPendingDStarRuntimeConfiguration();
}

void RadioModel::applyPendingDStarRuntimeConfiguration()
{
    if (!m_dstarRuntimeConfigurationPending
        || !m_flexBackend
        || !isConnected()
        || m_transmitModel.isTransmitting()
        || m_radioTransmitting) {
        return;
    }

    const DigitalVoiceWaveformProcess& process =
        DigitalVoiceWaveformProcess::instance();
    const std::optional<DigitalVoiceModeId> activeMode =
        DigitalVoiceModeRegistry::instance().activeMode();
    if (process.state() != DigitalVoiceWaveformProcess::State::Running
        || !process.registrationVerified()
        || !activeMode.has_value()
        || activeMode.value() != DigitalVoiceModeId::DStar) {
        return;
    }

    const int sliceId = DigitalVoiceModeRegistry::instance().activeSliceId();
    if (sliceId < 0) {
        return;
    }
    SliceModel* controlledSlice = slice(sliceId);
    if (!controlledSlice
        || controlledSlice->mode().compare(QStringLiteral("DSTR"),
                                            Qt::CaseInsensitive) != 0) {
        return;
    }

    const DStarConfiguration config = m_dstarModel.configuration(callsign());
    if (!m_dstarModel.configurationError(config, callsign()).isEmpty()) {
        return;
    }

    QString command = DStarModel::runtimeSetCommand(config);
    const quint32 owner = clientHandle();
    if (owner != 0U) {
        command += QStringLiteral(" owner=0x%1")
            .arg(owner, 8, 16, QLatin1Char('0'));
    }
    m_flexBackend->sendSliceWaveformCommand(sliceId, command);
    m_dstarRuntimeConfigurationPending = false;
}

void RadioModel::syncDigitalVoiceTxSelection(bool force)
{
    DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
    const std::optional<DigitalVoiceModeId> activeMode =
        DigitalVoiceModeRegistry::instance().activeMode();
    if (!isConnected()
        || process.state() != DigitalVoiceWaveformProcess::State::Running
        || !process.registrationVerified()
        || !activeMode.has_value()) {
        m_lastDigitalVoiceTxSelectionKey.clear();
        return;
    }

    const DigitalVoiceModeDescriptor& descriptor =
        DigitalVoiceModeRegistry::descriptor(activeMode.value());
    const int controlledSliceId =
        DigitalVoiceModeRegistry::instance().activeSliceId();
    SliceModel* commandSlice = controlledSliceId >= 0
        ? slice(controlledSliceId)
        : nullptr;
    if (!commandSlice
        || commandSlice->mode().compare(descriptor.radioMode,
                                        Qt::CaseInsensitive) != 0) {
        m_lastDigitalVoiceTxSelectionKey.clear();
        return;
    }

    SliceModel* selectedTxSlice = txSlice();
    const bool selectedModeActive = selectedTxSlice
        && selectedTxSlice->sliceId() == controlledSliceId
        && selectedTxSlice->mode().compare(descriptor.radioMode,
                                           Qt::CaseInsensitive) == 0;
    const QString selectedToken = selectedTxSlice
        ? QString::number(selectedTxSlice->sliceId())
        : QStringLiteral("none");
    const QString selectionKey = QStringLiteral("%1:%2:%3:%4")
        .arg(descriptor.radioMode)
        .arg(commandSlice->sliceId())
        .arg(selectedToken)
        .arg(selectedModeActive ? 1 : 0);
    if (!force && m_lastDigitalVoiceTxSelectionKey == selectionKey) {
        return;
    }

    m_lastDigitalVoiceTxSelectionKey = selectionKey;
    QString command = QStringLiteral("tx_select %1 %2")
        .arg(selectedToken)
        .arg(selectedModeActive ? 1 : 0);
    const quint32 owner = clientHandle();
    if (owner != 0U) {
        command += QStringLiteral(" owner=0x%1")
            .arg(owner, 8, 16, QLatin1Char('0'));
    }
    if (!m_flexBackend) {
        m_lastDigitalVoiceTxSelectionKey.clear();
        return;
    }
    m_flexBackend->sendSliceWaveformCommand(commandSlice->sliceId(), command);
    applyPendingDStarRuntimeConfiguration();
}

QString RadioModel::audioCompressionParam() const
{
    QString setting = AppSettings::instance().value("AudioCompression", "None").toString();
    if (setting == "Opus") return "opus";
    if (setting == "None") return "none";
    // Auto: use Opus on WAN, uncompressed on LAN
    return isWan() ? "opus" : "none";
}

void RadioModel::sendCwKey(bool down, const QString& debugSource,
                           quint64 debugTraceId, quint64 debugSourceMs)
{
    const bool prev = m_cwKeyActive;
    m_cwKeyActive = down;
    // Send only the key edge — the radio's break-in setting decides whether
    // it transmits.  With break_in=1 (QSK), `cw key 1` triggers TX and
    // break_in_delay holds the relay between elements.  With break_in=0,
    // the radio queues the key but doesn't transmit until the operator
    // explicitly asserts CW PTT (Space PTT, MOX, or hardware PTT) — the
    // standard semi-break-in workflow per FlexLib Radio.cs:8890–8965.
    sendNetCwCommand(QString("cw key %1").arg(down ? 1 : 0),
                     debugSource, debugTraceId, debugSourceMs);
    if (prev != down)
        emit cwKeyDownChanged(down);
}

void RadioModel::sendCwPaddle(bool dit, bool dah, const QString& debugSource,
                              quint64 debugTraceId, quint64 debugSourceMs)
{
    // The radio's CW protocol does NOT accept a 2-arg paddle form like
    // `cw key dit dah` — FlexLib only ever sends `cw key 1` or `cw key 0`
    // (single state) and expects the client to do iambic timing locally.
    // Treat any paddle press as a straight-key down so this path still
    // works when the local iambic keyer is disabled.  When the keyer IS
    // running it intercepts upstream and uses sendCwPtt + sendCwKeyEdge
    // directly, bypassing this method.
    sendCwKey(dit || dah, debugSource, debugTraceId, debugSourceMs);
}

void RadioModel::sendCwPtt(bool on, const QString& debugSource,
                           quint64 debugTraceId, quint64 debugSourceMs)
{
    sendNetCwCommand(on ? QStringLiteral("cw ptt 1") : QStringLiteral("cw ptt 0"),
                     debugSource, debugTraceId, debugSourceMs);
}

void RadioModel::sendCwKeyEdge(bool down, const QString& debugSource,
                               quint64 debugTraceId, quint64 debugSourceMs)
{
    const bool prev = m_cwKeyActive;
    m_cwKeyActive = down;
    sendNetCwCommand(QString("cw key %1").arg(down ? 1 : 0),
                     debugSource, debugTraceId, debugSourceMs);
    if (prev != down)
        emit cwKeyDownChanged(down);
}

// ── NetCW stream — VITA-49 UDP delivery with redundant sends ────────────────

QByteArray RadioModel::buildNetCwPacket(const QByteArray& payload)
{
    // VITA-49 header (28 bytes) + ASCII command payload.  Working Maestro
    // captures show the payload null-padded to a 32-bit word boundary; keep
    // the datagram length consistent with the VRT packet_size field.
    const int payloadBytes = payload.size();
    const int paddedPayloadBytes = (payloadBytes + 3) & ~3;
    const int packetWords = static_cast<int>(std::ceil(payloadBytes / 4.0) + 7); // 7 header words
    const int packetBytes = 28 + paddedPayloadBytes;

    QByteArray pkt(packetBytes, '\0');
    auto* w = reinterpret_cast<quint32*>(pkt.data());

    // Word 0: ExtDataWithStream, C=1, T=0, TSI=3(Other), TSF=1(SampleCount)
    static int pktCount = 0;
    quint32 hdr = (0x3u << 28)     // pkt_type = ExtDataWithStream
                | (1u << 27)       // C = 1 (class ID present)
                | (0x3u << 22)     // TSI = 3 (Other)
                | (0x1u << 20)     // TSF = 1 (SampleCount)
                | ((pktCount & 0x0F) << 16)
                | (packetWords & 0xFFFF);
    pktCount = (pktCount + 1) & 0x0F;

    w[0] = qToBigEndian(hdr);
    w[1] = qToBigEndian(m_netCwStreamId);
    w[2] = qToBigEndian<quint32>(0x00001C2D);      // OUI (FlexRadio)
    w[3] = qToBigEndian<quint32>(0x534C03E3);       // ICC=0x534C, PCC=0x03E3
    w[4] = 0; w[5] = 0; w[6] = 0;                  // timestamps

    // Payload: ASCII command string
    memcpy(pkt.data() + 28, payload.constData(), payloadBytes);

    return pkt;
}

void RadioModel::sendNetCwCommand(const QString& baseCmd, const QString& debugSource,
                                  quint64 debugTraceId, quint64 debugSourceMs)
{
    if (m_netCwStreamId == 0) {
        // No netcw stream — fall back to TCP immediate
        const QString fallbackCmd = baseCmd.contains("cw key")
            ? QString(baseCmd).replace("cw key", "cw key immediate")
            : baseCmd;
        if (lcCw().isDebugEnabled()) {
            const quint64 now = cwTraceNowMs();
            qCDebug(lcCw).noquote().nospace()
                << "CW netcw fallback trace=" << debugTraceId
                << " t=" << now << "ms"
                << " sinceSourceMs=" << (debugSourceMs ? static_cast<qint64>(now - debugSourceMs) : -1)
                << " source=" << (debugSource.isEmpty() ? QStringLiteral("unknown") : debugSource)
                << " cmd=\"" << fallbackCmd << "\"";
        }
        sendCmd(fallbackCmd);
        return;
    }

    // Build the full command with timing metadata and dedup index
    // FlexLib format: "cw key 1 time=0x<hex_ms> index=<N> client_handle=0x<handle>".
    // The time value is a 16-bit relative millisecond counter, not an epoch
    // timestamp.  Flex clients reset it after a short idle gap; the radio
    // accepts 0x0000 as a timing resync marker.
    constexpr qint64 kNetCwIdleResetMs = 3000;
    quint16 timeMs = 0;
    if (!m_netCwClock.isValid()
        || m_netCwLastSendMs < 0
        || (m_netCwClock.elapsed() - m_netCwLastSendMs) > kNetCwIdleResetMs) {
        if (m_netCwClock.isValid())
            m_netCwClock.restart();
        else
            m_netCwClock.start();
        m_netCwLastSendMs = 0;
    } else {
        const qint64 elapsed = m_netCwClock.elapsed();
        timeMs = static_cast<quint16>(elapsed & 0xFFFF);
        m_netCwLastSendMs = elapsed;
    }
    int index = m_netCwIndex++;

    // FlexLib formats hex values UPPERCASE (C# ToString("X")), and the
    // radio's status messages do too (e.g. `S23A59BDF|...`) — the netcw
    // parser appears to be case-sensitive on `client_handle`.  Match that
    // by formatting both hex values uppercase explicitly.
    const QString tsHex = QString("%1").arg(timeMs, 4, 16, QChar('0')).toUpper();
    const QString chHex = QString("%1").arg(clientHandle(), 0, 16).toUpper();
    QString fullCmd = QString("%1 time=0x%2 index=%3 client_handle=0x%4")
        .arg(baseCmd, tsHex, QString::number(index), chHex);

    QByteArray payload = fullCmd.toLatin1();

    // Redundant sends via UDP: 0ms, 5ms, 10ms, 15ms.  The radio dedupes by the
    // ASCII `index=N` field, but each datagram needs a UNIQUE VITA-49
    // packet_count — FlexLib's NetCWStream.AddTXData increments packet_count
    // after each ToBytesTX(), so the four redundant copies arrive with counts
    // N, N+1, N+2, N+3.  Reusing a single buffer (same packet_count on all
    // four) makes the radio's VITA stream layer drop them as duplicates.
    QByteArray packet0 = buildNetCwPacket(payload);
    QByteArray packet1 = buildNetCwPacket(payload);
    QByteArray packet2 = buildNetCwPacket(payload);
    QByteArray packet3 = buildNetCwPacket(payload);
    const quint64 scheduledMs = cwTraceNowMs();
    const QString source = debugSource.isEmpty() ? QStringLiteral("unknown") : debugSource;

    if (lcCw().isDebugEnabled()) {
        qCDebug(lcCw).noquote().nospace()
            << "CW netcw schedule trace=" << debugTraceId
            << " t=" << scheduledMs << "ms"
            << " sinceSourceMs=" << (debugSourceMs ? static_cast<qint64>(scheduledMs - debugSourceMs) : -1)
            << " source=" << source
            << " stream=0x" << QString::number(m_netCwStreamId, 16).toUpper()
            << " index=" << index
            << " time=0x" << tsHex
            << " cmd=\"" << baseCmd << "\""
            << " payloadBytes=" << payload.size()
            << " packetBytes=" << packet0.size()
            << " udpCopies=4 tcpBackstop=1";
    }

    auto logUdpSend = [debugTraceId, scheduledMs, source](int copy, int delayMs, int bytes) {
        if (!lcCw().isDebugEnabled())
            return;
        const quint64 now = cwTraceNowMs();
        qCDebug(lcCw).noquote().nospace()
            << "CW netcw udp-send trace=" << debugTraceId
            << " t=" << now << "ms"
            << " source=" << source
            << " copy=" << copy
            << " delayMs=" << delayMs
            << " actualDelayMs=" << static_cast<qint64>(now - scheduledMs)
            << " timerSlipMs=" << (static_cast<qint64>(now - scheduledMs) - delayMs)
            << " bytes=" << bytes;
    };

    QMetaObject::invokeMethod(m_panStream, [this, packet0, logUdpSend]() {
        logUdpSend(0, 0, packet0.size());
        m_panStream->sendToRadio(packet0);
    }, Qt::QueuedConnection);

    QTimer::singleShot(5, this, [this, packet1, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet1, logUdpSend]() {
            logUdpSend(1, 5, packet1.size());
            m_panStream->sendToRadio(packet1);
        }, Qt::QueuedConnection);
    });
    QTimer::singleShot(10, this, [this, packet2, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet2, logUdpSend]() {
            logUdpSend(2, 10, packet2.size());
            m_panStream->sendToRadio(packet2);
        }, Qt::QueuedConnection);
    });
    QTimer::singleShot(15, this, [this, packet3, logUdpSend]() {
        QMetaObject::invokeMethod(m_panStream, [this, packet3, logUdpSend]() {
            logUdpSend(3, 15, packet3.size());
            m_panStream->sendToRadio(packet3);
        }, Qt::QueuedConnection);
    });

    // FlexLib sends the same decorated netcw command over TCP after the UDP
    // copies.  With the 16-bit timestamp format above, the radio can dedupe
    // by index=N and the TCP path provides a reliable delivery backstop.
    sendCmd(fullCmd);
}

void RadioModel::cwAutoTune(int sliceId, bool intermittent)
{
    if (intermittent) {
        sendCmd(QString("slice auto_tune %1 int=1").arg(sliceId));
    } else {
        // int=0 stops the autotune engine (FlexLib: isIntermittent=false)
        sendCmd(QString("slice auto_tune %1 int=0").arg(sliceId));
    }
}

void RadioModel::cwAutoTuneOnce(int sliceId)
{
    // One-shot autotune (FlexLib: isIntermittent=null)
    sendCmd(QString("slice auto_tune %1").arg(sliceId));
}

void RadioModel::addSlice()
{
    if (m_activePanId.isEmpty()) {
        qCWarning(lcProtocol) << "RadioModel::addSlice: no panadapter, cannot create slice";
        return;
    }

    // Create a new slice offset from existing slices so VFO flags deconflict.
    // Use pan center, but if an existing slice is within 5 kHz, offset by
    // 20% of the visible bandwidth.
    auto* pan = activePanadapter();
    double newFreq = pan ? pan->centerMhz() : 14.1;
    const double offsetMhz = (pan ? pan->bandwidthMhz() : 0.2) * 0.2;  // 20% of visible BW
    for (auto* s : m_slices) {
        if (std::abs(s->frequency() - newFreq) < 0.005) {  // within 5 kHz
            newFreq += offsetMhz;
            break;
        }
    }
    const QString freq = QString::number(newFreq, 'f', 6);
    const QString cmd = QString("slice create pan=%1 freq=%2").arg(m_activePanId, freq);

    qCDebug(lcProtocol) << "RadioModel::addSlice:" << cmd;
    sendCmd(cmd, [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel: slice create failed, code"
                       << Qt::hex << code << "body:" << body;
            emit sliceCreateFailed(maxSlices(), m_model);
        } else {
            qCDebug(lcProtocol) << "RadioModel: new slice created, index =" << body;
        }
    });
}

void RadioModel::addSliceOnPan(const QString& panId)
{
    if (panId.isEmpty()) { addSlice(); return; }

    auto* pan = panadapter(panId);
    double newFreq = pan ? pan->centerMhz() : 14.1;
    const double offsetMhz = (pan ? pan->bandwidthMhz() : 0.2) * 0.2;
    for (auto* s : m_slices) {
        if (std::abs(s->frequency() - newFreq) < 0.005) {
            newFreq += offsetMhz;
            break;
        }
    }
    addSliceOnPan(panId, newFreq);
}

void RadioModel::addSliceOnPan(const QString& panId, double freqMhz)
{
    if (panId.isEmpty()) {
        qCWarning(lcProtocol) << "RadioModel::addSliceOnPan: no panadapter, cannot create slice";
        return;
    }
    if (!std::isfinite(freqMhz)) {
        qCWarning(lcProtocol) << "RadioModel::addSliceOnPan: invalid frequency" << freqMhz;
        return;
    }

    const QString freq = QString::number(freqMhz, 'f', 6);
    const QString cmd = QString("slice create pan=%1 freq=%2").arg(panId, freq);

    qCDebug(lcProtocol) << "RadioModel::addSliceOnPan:" << cmd;
    sendCmd(cmd, [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel: slice create failed, code"
                       << Qt::hex << code << "body:" << body;
            emit sliceCreateFailed(maxSlices(), m_model);
        } else {
            qCDebug(lcProtocol) << "RadioModel: new slice created, index =" << body;
        }
    });
}

void RadioModel::createPanadapter()
{
    // A backend that owns its own receivers creates them at the seam. The Flex
    // wire text below (`display panafall create`) goes nowhere on such a radio,
    // which is why "Add Panadapter" did nothing on an HL2 and the only way to
    // get a second receiver was a persisted count applied at connect.
    //
    // Checked BEFORE the limit below, because that limit is a FLEX MODEL-STRING
    // TABLE (capabilitiesFor(m_model)) and has nothing to say about this radio.
    // Applying it to an HL2 would refuse the add against a number derived from
    // a FlexLib platform lookup that never heard of the board.
    //
    // The backend is the only thing that knows the real limits — the receiver
    // count the board reported at discovery, and the link budget at the span
    // currently running — so it decides, and says which limit it hit.
    //
    // The new pan is reported through panCenterBandwidthChanged, which
    // materialises the model exactly as it does for the pans that exist at
    // connect — so there is ONE path that creates a pane, not two.
    if (!m_flexBackend && m_backend) {
        if (!m_backend->createPanadapter()) {
            qCWarning(lcProtocol) << "RadioModel::createPanadapter: backend declined";
            emit panadapterLimitReached(m_backend->capabilities().maxPanadapters, m_model);
        }
        return;
    }

    int limit = maxPanadapters();
    if (static_cast<int>(m_panadapters.size()) >= limit) {
        qCWarning(lcProtocol) << "RadioModel::createPanadapter: limit of" << limit
                              << "panadapters reached for model" << m_model;
        emit panadapterLimitReached(limit, m_model);
        return;
    }
    const auto handleCreatedPan = [this](const QString& source, int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel:" << source << "failed, code"
                                  << Qt::hex << code << "body:" << body;
            emit panadapterLimitReached(maxPanadapters(), m_model);
            return;
        }
        const QString panId = parsePanadapterCreateId(body);

        qCDebug(lcProtocol) << "RadioModel: new panadapter created, pan_id =" << panId;

        if (!panId.isEmpty()) {
            ensureOwnedPanadapter(panId);
            QTimer::singleShot(200, this, [this, panId]() {
                sendCmd(QString("display pan set %1 xpixels=1024 ypixels=700").arg(panId));
                sendCmd(QString("display pan set %1 min_dbm=-130 max_dbm=-40").arg(panId));
            });
        }
    };

    qCDebug(lcProtocol) << "RadioModel::createPanadapter: sending display panafall create";
    sendCmd("display panafall create x=100 y=100", [this, handleCreatedPan](int code, const QString& body) {
        if (code == 0) {
            handleCreatedPan(QStringLiteral("display panafall create"), code, body);
            return;
        }

        qCWarning(lcProtocol) << "RadioModel: display panafall create failed, code"
                              << Qt::hex << code << "body:" << body
                              << "- trying legacy panadapter create";
        sendCmd("panadapter create",
                [handleCreatedPan](int legacyCode, const QString& legacyBody) {
            handleCreatedPan(QStringLiteral("panadapter create"), legacyCode, legacyBody);
        });
    });
}

void RadioModel::removePanadapter(const QString& panId)
{
    // A panafall (display panafall create) allocates BOTH a panadapter stream
    // and a waterfall stream, and the radio does NOT free the waterfall when
    // only the panadapter is removed. So the FlexLib-correct teardown is the
    // pair Panadapter.Close() + Waterfall.Close() — "display pan remove" AND
    // "display panafall remove" (FlexLib v4.2.18). Sending only the first (or
    // the bogus "display pan close", which is not a command at all) leaves the
    // waterfall stream alive on the radio. (#3843)
    //
    // Capture the waterfall id BEFORE sending: the "display pan <id> removed"
    // echo deletes the PanadapterModel in onStatusReceived, so reading it
    // afterwards would race the teardown.
    const PanadapterModel* pan = m_panadapters.value(panId, nullptr);
    const QString wfId = pan ? pan->waterfallId() : QString();
    qCDebug(lcProtocol) << "RadioModel::removePanadapter:" << panId
                        << "waterfall:" << (wfId.isEmpty() ? QStringLiteral("(none)") : wfId);

    // Same reasoning as createPanadapter(): on a backend that owns its
    // receivers this is a seam verb, and the Flex teardown pair below would go
    // nowhere. The model pane is dropped when the backend confirms with
    // panRemoved — never optimistically, or a refused close (the last receiver)
    // would take the pane away while the receiver kept streaming into nothing.
    if (!m_flexBackend && m_backend) {
        if (!m_backend->removePanadapter(backendPanIdFor(panId)))
            qCWarning(lcProtocol) << "RadioModel::removePanadapter: backend declined" << panId;
        return;
    }
    sendCommand(QStringLiteral("display pan remove ") + panId);
    if (!wfId.isEmpty())
        sendCommand(QStringLiteral("display panafall remove ") + wfId);
    // Radio will send "display pan <id> removed" → handled in onStatusReceived
}

// ── Pan accessor implementations ──────────────────────────────────────────────

PanadapterModel* RadioModel::activePanadapter() const
{
    return m_panadapters.value(m_activePanId, nullptr);
}

PanadapterModel* RadioModel::panadapter(const QString& panId) const
{
    return m_panadapters.value(panId, nullptr);
}

PanadapterModel* RadioModel::resolvePan(const QString& panId) const
{
    // Single source of the pan-addressing policy: the addressed pan, else the
    // active one. Used by the aetherd RFC 2.3 backend-signal handlers so a future
    // change (e.g. don't fall back for MultiFlex-owned pans) lands in one place.
    auto* p = m_panadapters.value(panId, nullptr);
    return p ? p : activePanadapter();
}

DisplayInventory::Report RadioModel::displayInventoryReport() const
{
    DisplayInventory::Inputs in;
    in.ourHandle = clientHandle();
    for (auto it = m_radioDisplayPans.cbegin(); it != m_radioDisplayPans.cend(); ++it)
        in.radioPans.push_back({it.key(), it.value().clientHandle});
    for (auto it = m_radioDisplayWaterfalls.cbegin();
         it != m_radioDisplayWaterfalls.cend(); ++it)
        in.radioWaterfalls.push_back({it.key(), it.value().clientHandle,
                                      it.value().parentPanId});
    // Owned sets — normalize the waterfall id so it compares equal to the
    // 0x-prefixed inventory keys regardless of hex case (#3856 review).
    // Include m_stalePanadapters: during the reconnect reclaim window our own
    // pans live there (not yet moved to m_panadapters), and the radio re-dumps
    // their status — without this they'd transiently report as orphan.
    const auto addOwned = [&in](const QMap<QString, PanadapterModel*>& m) {
        for (auto it = m.cbegin(); it != m.cend(); ++it) {
            in.ownedPanIds.insert(normalizePanadapterId(it.key()));
            if (it.value() && !it.value()->waterfallId().isEmpty())
                in.ownedWaterfallIds.insert(normalizePanadapterId(it.value()->waterfallId()));
        }
    };
    addOwned(m_panadapters);
    addOwned(m_stalePanadapters);
    return DisplayInventory::classify(in);
}

bool RadioModel::resyncDisplayInventory()
{
    if (!isConnected()) return false;
    // Re-subscribing to the pan domain makes the radio re-send the status of
    // every currently-allocated panadapter + waterfall. Those replies flow
    // through the same `display pan`/`display panafall` status parser that
    // maintains m_radioDisplayPans/Waterfalls, so the Layer-B inventory
    // refreshes to the radio's authoritative current set — the only way to
    // observe a resource-level lingering waterfall that no longer emits UDP
    // (#3856). We intentionally do NOT clear the maps first: a re-dump can
    // only re-add/confirm objects, so a no-op on firmware that doesn't re-dump
    // leaves the inventory intact rather than wiping it.
    sendCmd("sub pan all");
    return true;
}

double RadioModel::panCenterMhz() const
{
    auto* p = activePanadapter();
    return p ? p->centerMhz() : 14.1;
}

double RadioModel::panBandwidthMhz() const
{
    auto* p = activePanadapter();
    return p ? p->bandwidthMhz() : 0.2;
}

void RadioModel::setPanBandwidth(double bandwidthMhz)
{
    if (m_activePanId.isEmpty()) return;
    // User-intent pan write: must go through the defer queue like its sibling
    // setPanCenter() (#4142). A raw sendCmd() here would be silently dropped
    // during the profile-load hold and trip the routed-pan-field backstop.
    requestPanBandwidth(m_activePanId, bandwidthMhz);
}

void RadioModel::setPanCenter(double centerMhz)
{
    if (m_activePanId.isEmpty()) return;
    if (PanadapterModel* pan = panadapter(m_activePanId)) {
        // Clamp so the pan's low edge stays >= 0 Hz, matching the spectrum
        // pan-drag path (MainWindow_Wiring wirePanadapter). Without it an
        // out-of-range center would be optimistically stored and advertised via
        // TCI dds: even though the radio rejects it.
        centerMhz = std::max(centerMhz, pan->bandwidthMhz() / 2.0);
    }
    requestPanCenter(m_activePanId, centerMhz);
}

namespace {

// Log-line rendering of a voided/flushed entry: exactly the fields that were
// pending, in wire syntax, so the log names what was (or would have been)
// destroyed.
QString describePanWrites(const AetherSDR::PanWrites& writes)
{
    QStringList parts;
    if (writes.bandKey) {
        parts << QStringLiteral("band=%1").arg(*writes.bandKey);
    }
    if (writes.centerMhz) {
        parts << QStringLiteral("center=%1").arg(*writes.centerMhz, 0, 'f', 6);
    }
    if (writes.bandwidthMhz) {
        parts << QStringLiteral("bandwidth=%1").arg(*writes.bandwidthMhz, 0, 'f', 6);
    }
    return parts.join(QLatin1Char(' '));
}

} // namespace

bool RadioModel::requestPanCenter(const QString& panId,
                                  double centerMhz,
                                  double bandwidthMhz,
                                  IRadioBackend::PanCenterIntent intent)
{
    if (panId.isEmpty()) {
        return false;
    }

    const bool wantsBandwidth = bandwidthMhz > 0.0;

    // A pan center is profile-owned radio state, so sendCmd() would DROP this
    // while a profile load is rebuilding the radio's topology. Defer it instead:
    // queue the REQUESTED value and replay it once the hold lifts.
    //
    // Gate on exactly the predicate sendCmd() guards on, so "if sendCmd would
    // drop it, we queue it" is an identity rather than an approximation — two
    // separate clocks could skew and re-open the same silent drop.
    if (profileLoadRadioStateWritesHeld()) {
        const auto pendingCenter =
            m_pendingProfileLoadPanWrites.pendingCenter(panId);
        const auto pendingBandwidth =
            m_pendingProfileLoadPanWrites.pendingBandwidth(panId);

        // Dedupe against EFFECTIVE state. Equal to what is already pending →
        // nothing new to record; the request stays deferred.
        const bool equalsPending =
            pendingCenter && qFuzzyCompare(*pendingCenter, centerMhz)
            && (!wantsBandwidth
                || (pendingBandwidth
                    && qFuzzyCompare(*pendingBandwidth, bandwidthMhz)));
        if (equalsPending) {
            return false;
        }

        // Equal to the MODEL — which keeps tracking radio status during the
        // hold, so this is radio truth. If a different value was pending, the
        // user corrected back: cancel exactly the requested fields instead of
        // replaying the superseded value later. Either way the radio is
        // already where the caller asked — report success.
        PanadapterModel* pan = panadapter(panId);
        const bool equalsModel =
            pan && qFuzzyCompare(pan->centerMhz(), centerMhz)
            && (!wantsBandwidth
                || qFuzzyCompare(pan->bandwidthMhz(), bandwidthMhz));
        if (equalsModel) {
            if (pendingCenter || (wantsBandwidth && pendingBandwidth)) {
                m_pendingProfileLoadPanWrites.supersedeCenter(panId);
                if (wantsBandwidth) {
                    m_pendingProfileLoadPanWrites.supersedeBandwidth(panId);
                }
                qCDebug(lcProtocol).noquote()
                    << "RadioModel: cancelled pending pan write (user corrected"
                    << "back to the radio's state)"
                    << QStringLiteral("pan=%1").arg(panId)
                    << QStringLiteral("center=%1").arg(centerMhz, 0, 'f', 6);
            }
            return true;
        }

        // Deliberately do NOT touch PanadapterModel here. The optimistic local
        // update is the second half of #4142: with the command dropped, a client
        // that advanced its own center claimed a center the radio never took.
        // Honest VITA-49 tiles (each carrying its own FrameLowFreq/BinBandwidth)
        // were then projected into a view that lied about its span, and the
        // non-overlapping region rendered black — permanently, into history.
        // Local state may only advance when a command actually reaches the wire.
        if (wantsBandwidth) {
            m_pendingProfileLoadPanWrites.deferCenterBandwidth(panId, centerMhz,
                                                               bandwidthMhz);
        } else {
            m_pendingProfileLoadPanWrites.deferCenter(panId, centerMhz);
        }
        qCDebug(lcProtocol).noquote()
            << "RadioModel: deferring pan center during profile load"
            << QStringLiteral("pan=%1").arg(panId)
            << QStringLiteral("center=%1").arg(centerMhz, 0, 'f', 6);

        // Whoever defers a write owns scheduling its replay. Do NOT rely on the
        // profile-load ACK to schedule the flush: the hold is armed when the
        // profile-load command is SENT, but MainWindow's recovery pass (and its
        // flush timers) only runs on profileLoadCompleted, which is emitted on
        // ACK. A large topology (8 pans / 8 slices, verified on a 6700) can stall
        // the radio long enough that it misses pings and the client force-
        // disconnects BEFORE the ACK ever arrives — in which case no flush would
        // ever have been scheduled and this request would be stranded forever.
        armProfileLoadPanWriteFlush();
        return false;
    }

    // Immediate path. Supersede exactly the fields this write carries FIRST,
    // so a stale deferred value can never replay over the newer wire state
    // (hold-expiry-to-flush is a real window: the flush runs at hold+100 ms).
    m_pendingProfileLoadPanWrites.supersedeCenter(panId);
    if (wantsBandwidth) {
        m_pendingProfileLoadPanWrites.supersedeBandwidth(panId);
    }
    return dispatchPanCenterBandwidth(panId, centerMhz, bandwidthMhz, intent);
}

RadioModel::DataLiveness RadioModel::dataLiveness() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    DataLiveness l;
    if (m_lastSpectrumMs > 0)
        l.spectrumMs = now - m_lastSpectrumMs;
    if (m_lastAudioMs > 0)
        l.audioMs = now - m_lastAudioMs;
    l.meterMs = m_meterModel.newestValueAgeMs();
    return l;
}

bool RadioModel::requestPanBandwidth(const QString& panId, double bandwidthMhz)
{
    if (panId.isEmpty() || bandwidthMhz <= 0.0) {
        return false;
    }

    if (profileLoadRadioStateWritesHeld()) {
        const auto pendingBandwidth =
            m_pendingProfileLoadPanWrites.pendingBandwidth(panId);
        if (pendingBandwidth && qFuzzyCompare(*pendingBandwidth, bandwidthMhz)) {
            return false;
        }

        PanadapterModel* pan = panadapter(panId);
        if (pan && qFuzzyCompare(pan->bandwidthMhz(), bandwidthMhz)) {
            if (pendingBandwidth) {
                m_pendingProfileLoadPanWrites.supersedeBandwidth(panId);
                qCDebug(lcProtocol).noquote()
                    << "RadioModel: cancelled pending pan bandwidth (user"
                    << "corrected back to the radio's state)"
                    << QStringLiteral("pan=%1").arg(panId)
                    << QStringLiteral("bandwidth=%1").arg(bandwidthMhz, 0, 'f', 6);
            }
            return true;
        }

        m_pendingProfileLoadPanWrites.deferBandwidth(panId, bandwidthMhz);
        qCDebug(lcProtocol).noquote()
            << "RadioModel: deferring pan bandwidth during profile load"
            << QStringLiteral("pan=%1").arg(panId)
            << QStringLiteral("bandwidth=%1").arg(bandwidthMhz, 0, 'f', 6);
        armProfileLoadPanWriteFlush();
        return false;
    }

    m_pendingProfileLoadPanWrites.supersedeBandwidth(panId);
    return dispatchPanCenterBandwidth(
        panId, std::numeric_limits<double>::quiet_NaN(), bandwidthMhz);
}

bool RadioModel::requestPanDisplayRates(const QString& panId, int fps,
                                        int wfRate)
{
    if (panId.isEmpty())
        return false;

    PanadapterModel* pan = panadapter(panId);

    // A backend that streams raw spectra shapes them in onBackendSpectrumFrame,
    // and the pan model is where that shaper reads its target. Applying locally
    // is correct rather than optimistic here: with no radio-side display engine,
    // the client IS the authority for these two values, so there is no echo to
    // wait for and nothing that could later contradict it.
    if (shapesDisplayRatesLocally()) {
        if (!pan)
            return false;
        pan->setDisplayRates(fps, wfRate);
        // The FPS half goes DOWN to the backend, which caps its own frame
        // production. Only the waterfall's line_duration is paced up here —
        // see onBackendSpectrumFrame for why the two live in different places.
        if (fps > 0)
            m_backend->setPanFrameRate(backendPanIdFor(panId), fps);
        return true;
    }

    bool sent = false;
    if (fps > 0) {
        sent = sendCommand(QString("display pan set %1 fps=%2").arg(panId).arg(fps))
               || sent;
    }
    if (wfRate > 0 && pan && !pan->waterfallId().isEmpty()) {
        // The wire parameter keeps Flex's name; the value is the rate.
        sent = sendCommand(QString("display panafall set %1 line_duration=%2")
                               .arg(pan->waterfallId())
                               .arg(wfRate))
               || sent;
    }
    return sent;
}

bool RadioModel::requestPanBand(const QString& panId, const QString& bandKey)
{
    if (panId.isEmpty() || bandKey.isEmpty()) {
        return false;
    }

    if (profileLoadRadioStateWritesHeld()) {
        const auto pendingBand = m_pendingProfileLoadPanWrites.pendingBand(panId);
        if (pendingBand && *pendingBand == bandKey) {
            return false;
        }

        // No model-side dedupe: the client holds no band-stack state to compare
        // against — the radio owns it and reports the outcome via status.
        m_pendingProfileLoadPanWrites.deferBand(panId, bandKey);
        qCDebug(lcProtocol).noquote()
            << "RadioModel: deferring pan band during profile load"
            << QStringLiteral("pan=%1").arg(panId)
            << QStringLiteral("band=%1").arg(bandKey);
        armProfileLoadPanWriteFlush();
        return false;
    }

    m_pendingProfileLoadPanWrites.supersedeBand(panId);
    return dispatchPanBand(panId, bandKey);
}

// Sanity ceiling for a CAT-commanded tune (MHz). Well above the highest amateur
// allocation (~250 GHz, incl. microwave/transverter), so a real tune is never
// rejected, while an absurd literal — a fat-fingered set, a parse that ran away
// — is caught before it broadcasts a bogus frequencyChanged. The radio enforces
// its actual band limits; this only rejects the physically impossible.
//
// Because it must clear 250 GHz, it does NOT bound step arithmetic in the UP
// direction: the step count parses as an int, so the largest reachable UP target
// is ~215 GHz at the usual 100 Hz step and no step count can trip this. The DN
// direction is bounded, but by the mhz > 0 term below rather than by this
// ceiling. Don't lower it to catch runaway steps — that would reject legitimate
// microwave work; a step count worth bounding should be bounded where it is
// parsed.
static constexpr double kMaxCatTuneMhz = 1.0e6; // 1 THz

bool RadioModel::isPlausibleCatTuneMhz(double mhz)
{
    return std::isfinite(mhz) && mhz > 0.0 && mhz < kMaxCatTuneMhz;
}

bool RadioModel::tuneSliceForCat(SliceModel* slice, double mhz)
{
    // This mutates SliceModel, which lives on this model's (GUI) thread. The
    // GUI-thread invariant is upheld by construction at both call sites — not by
    // the assert below (which is a no-op in release): SmartCAT/CatPort calls this
    // directly and already runs on the GUI thread, while rigctld only ever reaches
    // here inside a QueuedConnection invocation delivered on the GUI thread. The
    // assert is a debug-only backstop that catches a future off-thread caller
    // before it can silently race SliceModel, rather than the guarantee itself.
    Q_ASSERT(QThread::currentThread() == thread());
    if (!slice) {
        return false;
    }
    // Boundary validation (Principle VII): this is the single seam every CAT /
    // rigctld retune funnels through, so reject an implausible frequency here
    // once rather than in each caller. A non-positive, non-finite, or absurdly
    // high target (a client sending FA-5000;, a multi-step DN/UP that under- or
    // overflows, a parse that yielded NaN/inf) would otherwise be pushed
    // optimistically into the model and broadcast via frequencyChanged before the
    // radio rejects it. Return false so the caller answers the client with an
    // error rather than acknowledging a tune that was thrown away. (rigctld also
    // pre-validates with this same predicate — its queued tune can't observe this
    // bool; see isPlausibleCatTuneMhz.)
    if (!isPlausibleCatTuneMhz(mhz)) {
        return false;
    }
    // A locked slice refuses the tune outright — SliceModel::setFrequency and
    // tuneAndRecenter both bail on m_locked (and raise the lock feedback), so
    // returning true here would be the same Principle VII lie the check above
    // closes: the client reads success for a tune the radio never made. Test the
    // lock rather than comparing the frequency before and after, because a
    // retune to the frequency the slice already holds is a legitimate no-op that
    // SliceModel also skips — and that case must still report success. (rigctld
    // pre-checks the lock too; its queued tune can't observe this bool.)
    if (slice->isLocked()) {
        // Tell the OPERATOR, not just the CAT client. SliceModel::setFrequency and
        // tuneAndRecenter raise this before bailing on m_locked, so returning here
        // without it would swallow the on-screen lock flash — leaving someone whose
        // WSJT-X has stopped retuning with no indication that the lock is why.
        slice->notifyTuneBlockedByLock();
        return false;
    }
    // Recenter policy: an in-span retune keeps autopan=0 (no yank — external
    // Doppler software like SatPC32 steps every few seconds); an out-of-span or
    // cross-band target uses tuneAndRecenter so the radio recenters/re-bands the
    // panadapter. Both go through SliceModel, which updates the model frequency
    // and emits frequencyChanged — that drives the client-side follow (Center
    // Lock / Pan-Follows-VFO) the radio needs to actually move a centered slice.
    // (A bare command via sendCmdPublic does NOT emit frequencyChanged, so the
    // follow never runs and the tune doesn't stick on real hardware.) SliceModel
    // still guards the no-op case (freq unchanged) itself, so no extra check here.
    // We issue no pan command directly — the client lock logic does, exactly as
    // the GUI tune path does.
    //
    // Deliberately NOT a band-stack preselect: the GUI's own cross-band tune
    // calls preselectBandStackForTune()/requestPanBand() first, which also
    // restores that band's antenna, filters and zoom. CAT does not, by design —
    // the CAT/DAX/TCI planes express intent and let the radio recenter rather
    // than issuing pan/band commands directly, and confirm off the radio's reply
    // instead of a fixed timer. TciServer::tuneSliceAndConfirm behaves the same
    // way, and TCI is the reference we match. Consequence, called out in the PR:
    // a CAT band change follows the pan but does not restore per-band stack
    // state the way clicking the band button does.
    bool inSpan = false;
    if (const PanadapterModel* pan = panadapter(slice->panId())) {
        inSpan = pan->spanContainsMhz(mhz);
    }
    if (inSpan) {
        slice->setFrequency(mhz);
    } else {
        slice->tuneAndRecenter(mhz);
    }
    return true;
}

double RadioModel::effectivePanCenterMhz(const QString& panId) const
{
    if (const auto pending = m_pendingProfileLoadPanWrites.pendingCenter(panId)) {
        return *pending;
    }
    if (const PanadapterModel* pan = panadapter(panId)) {
        return pan->centerMhz();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

double RadioModel::effectivePanBandwidthMhz(const QString& panId) const
{
    if (const auto pending =
            m_pendingProfileLoadPanWrites.pendingBandwidth(panId)) {
        return *pending;
    }
    if (const PanadapterModel* pan = panadapter(panId)) {
        return pan->bandwidthMhz();
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool RadioModel::dispatchPanCenterBandwidth(const QString& panId,
                                            double centerMhz,
                                            double bandwidthMhz,
                                            IRadioBackend::PanCenterIntent intent)
{
    const bool hasCenter = !std::isnan(centerMhz);
    const bool hasBandwidth = bandwidthMhz > 0.0;
    if (!hasCenter && !hasBandwidth) {
        return false;
    }

    PanadapterModel* pan = panadapter(panId);

    if (hasCenter) {
        // Re-clamp against the pan's CURRENT geometry. The request may have
        // been clamped by its caller against geometry a profile load has since
        // replaced — a deferred write is dispatched seconds after it was made.
        // Same rule as every gesture path: the pan's low edge stays >= 0 Hz.
        const double clampBandwidthMhz =
            hasBandwidth ? bandwidthMhz : (pan ? pan->bandwidthMhz() : 0.0);
        const double clamped = std::max(centerMhz, clampBandwidthMhz / 2.0);
        if (clamped > centerMhz) {
            qCDebug(lcProtocol).noquote()
                << "RadioModel: clamping pan center against current geometry"
                << QStringLiteral("pan=%1").arg(panId)
                << QStringLiteral("requested=%1").arg(centerMhz, 0, 'f', 6)
                << QStringLiteral("clamped=%1").arg(clamped, 0, 'f', 6);
            centerMhz = clamped;
        }
    }

    // Center and bandwidth must travel together when both are requested;
    // splitting them produced the P1/P2 waterfall-loss and zoom-drift bugs.
    QString command;
    if (hasCenter && hasBandwidth) {
        command = QString("display pan set %1 center=%2 bandwidth=%3")
                      .arg(panId)
                      .arg(centerMhz, 0, 'f', 6)
                      .arg(bandwidthMhz, 0, 'f', 6);
    } else if (hasCenter) {
        command = QString("display pan set %1 center=%2")
                      .arg(panId)
                      .arg(centerMhz, 0, 'f', 6);
    } else {
        command = QString("display pan set %1 bandwidth=%2")
                      .arg(panId)
                      .arg(bandwidthMhz, 0, 'f', 6);
    }

    // A backend that owns its own pan geometry cannot be driven with Flex wire
    // text — the command would go nowhere and the view would advance against a
    // window the receiver never moved. Route the intent through the seam
    // instead; the backend reports the new centre back via
    // panCenterBandwidthChanged, which drives the model.
    if (!m_flexBackend && m_backend) {
        if (hasCenter) {
            // THE CALLER'S INTENT, FORWARDED — not "did a bandwidth come with
            // it". That inference read every centre-only writer as a drag, and
            // most of them are not: pan-follow, reveal, the band-change
            // recentre and the WFM recentre all send a centre alone. Reveal is
            // the one that bites, because the centre it sends is DELIBERATELY
            // OFFSET from the frequency in question (settle distance in from
            // the edge) — so on a backend where Drag means retune, clicking a
            // signal near the pan edge tuned the radio most of a half-span past
            // it. See IRadioBackend::PanCenterIntent and requestPanCenter().
            m_backend->setPanCenter(backendPanIdFor(panId), centerMhz * 1.0e6,
                                    intent);
        }
        // Bandwidth goes through the seam for the same reason center does. This
        // used to fall straight into the model write below, which is why zooming
        // an HL2 produced black bars: the span the operator asked for became the
        // view's span while the receiver kept sending its old, narrower window,
        // and the honest VITA-49 tiles left the difference unpainted.
        if (hasBandwidth)
            m_backend->setPanBandwidth(backendPanIdFor(panId), bandwidthMhz * 1.0e6);
        if (pan) {
            // Center only. The backend snaps a span REQUEST to a rate it can
            // actually run, so the resulting bandwidth is not ours to predict —
            // it arrives on panCenterBandwidthChanged, which is also what makes
            // the view widen only once the data behind it did.
            pan->setCenterBandwidth(hasCenter ? centerMhz : pan->centerMhz(),
                                    -1.0);
        }
        return true;
    }

    // Wire BEFORE model, gated on the send actually happening. sendCommand()
    // reports the foreign-owner drop, the hold backstop, and a dead WAN
    // session; advancing the model on any of those would re-create the exact
    // model-claims-state-the-radio-never-took lie this fix exists to kill.
    if (!sendCommand(command)) {
        qCWarning(lcProtocol).noquote()
            << "RadioModel: pan write not dispatched — model state unchanged —"
            << command;
        return false;
    }

    if (pan) {
        // Keep the canonical model aligned with the command now on the wire:
        // the radio may ACK without echoing a display status back to the
        // setting client, and TCI dds: follows this center.
        pan->setCenterBandwidth(hasCenter ? centerMhz : pan->centerMhz(),
                                hasBandwidth ? bandwidthMhz : -1.0);
    }
    return true;
}

bool RadioModel::dispatchPanBand(const QString& panId, const QString& bandKey)
{
    const QString command =
        QString("display pan set %1 band=%2").arg(panId, bandKey);

    emit panBandAboutToDispatch(panId);
    if (!sendCommand(command)) {
        // The band stack is a Flex concept. On a backend with no command plane
        // this write was never going to land — that is expected, not the #4142
        // signature the warning exists to catch, and warning on it would fire on
        // every band change the operator makes on an HL2. The failure signal
        // still goes out either way: it restores the KiwiSDR mute handoff that
        // panBandAboutToDispatch just lifted, which is needed precisely BECAUSE
        // the write did not reach the radio.
        if (hasCommandPlane()) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: pan band write not dispatched —" << command;
        } else {
            qCDebug(lcProtocol).noquote()
                << "RadioModel: no command plane for the band stack, skipping" << command;
        }
        emit panBandDispatchFailed(panId);
        return false;
    }

    // No model write: the band-stack swap retunes center/bandwidth/slices on
    // the radio, and that state arrives via radio status like any other
    // radio-initiated change.
    return true;
}

void RadioModel::armProfileLoadPanWriteFlush()
{
    // Re-arm for exactly when the hold lifts. The hold can be EXTENDED after we
    // arm (the ACK pushes it out again, and a second profile load pushes it out
    // further), so the flush re-checks and re-arms itself rather than trusting a
    // single deadline computed up front. stop()/start() keeps a SINGLE live
    // deadline — every defer restarts the one timer instead of adding another.
    const qint64 remainingMs =
        m_profileLoadRadioStateWriteHoldUntilMs - QDateTime::currentMSecsSinceEpoch();
    const int delayMs = static_cast<int>(std::max<qint64>(remainingMs, 0)) + 100;

    m_profileLoadPanWriteFlushTimer.stop();
    m_profileLoadPanWriteFlushTimer.start(delayMs);
}

void RadioModel::flushPendingProfileLoadPanWrites()
{
    if (m_pendingProfileLoadPanWrites.isEmpty()) {
        return;
    }

    if (!isConnected()) {
        // The session these requests belonged to is gone. Their pan ids and the
        // user's intent both died with it, and the radio will rebuild its own
        // topology on reconnect — replaying a pre-disconnect write could land
        // stale state on a pan that is no longer the same pan. Void them
        // loudly rather than stranding or misapplying them. (onDisconnected()
        // normally clears these first; this is the backstop.)
        qCWarning(lcProtocol).noquote()
            << "RadioModel: discarding" << m_pendingProfileLoadPanWrites.size()
            << "deferred pan write(s) — disconnected before the profile load settled";
        m_pendingProfileLoadPanWrites.clear();
        return;
    }

    // THE NON-REGRESSION PROOF, in one branch: while the hold is armed this
    // returns without sending, so a deferred pan write can never put a byte on
    // the wire inside the hold window. Nothing here can reintroduce the
    // missing-slices corruption the hold exists to prevent.
    //
    // Returning WITHOUT clearing is deliberate — we re-arm instead. The hold is
    // still armed only because a further topology-rebuilding profile load pushed
    // it out; dropping the request here would be the exact bug this method exists
    // to fix.
    if (profileLoadRadioStateWritesHeld()) {
        armProfileLoadPanWriteFlush();
        return;
    }

    const QHash<QString, PanWrites> pending =
        m_pendingProfileLoadPanWrites.takeAll();

    int sent = 0;
    int voided = 0;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const QString& panId = it.key();
        const PanWrites& writes = it.value();

        // Backstop only: the removal hook voids a dying pan's writes at
        // removal time, so a vanished pan here means a removal path was
        // missed. Void loudly either way — never silently.
        if (!panadapter(panId)) {
            ++voided;
            qCWarning(lcProtocol).noquote()
                << "RadioModel: voiding deferred pan write(s) — pan vanished"
                << "without a removal void (missed hook?)"
                << QStringLiteral("pan=%1").arg(panId)
                << describePanWrites(writes);
            continue;
        }

        // Band first, then center+bandwidth merged — the same order as the
        // live cross-band path: the band-stack swap lands, then the explicit
        // target overrides the stack's recalled frequency.
        if (writes.bandKey) {
            if (dispatchPanBand(panId, *writes.bandKey)) {
                ++sent;
            } else {
                ++voided;
            }
        }
        if (writes.centerMhz || writes.bandwidthMhz) {
            const double centerMhz = writes.centerMhz
                ? *writes.centerMhz
                : std::numeric_limits<double>::quiet_NaN();
            const double bandwidthMhz =
                writes.bandwidthMhz ? *writes.bandwidthMhz : -1.0;
            if (dispatchPanCenterBandwidth(panId, centerMhz, bandwidthMhz)) {
                ++sent;
            } else {
                ++voided;
            }
        }
    }

    // Always account — a flush that voided everything is exactly the one that
    // must not be invisible in the log.
    qCInfo(lcProtocol).noquote()
        << "RadioModel: deferred profile-load pan write flush"
        << QStringLiteral("sent=%1").arg(sent)
        << QStringLiteral("voided=%1").arg(voided);
}

void RadioModel::voidPendingPanWrites(const QString& panId, const QString& reason)
{
    const auto voided = m_pendingProfileLoadPanWrites.cancel(panId);
    if (!voided) {
        return;
    }
    qCWarning(lcProtocol).noquote()
        << "RadioModel: voiding deferred pan write(s) —" << reason
        << QStringLiteral("pan=%1").arg(panId)
        << describePanWrites(*voided);
}

void RadioModel::setPanDbmRange(float minDbm, float maxDbm)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 min_dbm=%2 max_dbm=%3")
            .arg(m_activePanId)
            .arg(static_cast<double>(minDbm), 0, 'f', 2)
            .arg(static_cast<double>(maxDbm), 0, 'f', 2));
}

void RadioModel::setBinauralRx(bool on)
{
    if (m_binauralRx == on) return;
    m_binauralRx = on;
    sendCmd(QString("radio set binaural_rx=%1").arg(on ? 1 : 0));
}

void RadioModel::setPanWnb(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 wnb=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

void RadioModel::setPanWnbLevel(int level)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 wnb_level=%2").arg(m_activePanId).arg(level));
}

void RadioModel::setPanRfGain(int gain)
{
    setPanRfGainFor(m_activePanId, gain);
}

void RadioModel::setPanRfGainFor(const QString& panId, int gain)
{
    if (panId.isEmpty()) return;
    // A backend that owns its gain in a hardware register cannot be driven with
    // Flex wire text — the same reason center/bandwidth route through the seam.
    // Without this the HL2's RF Gain slider moved, persisted, and changed
    // nothing: lnaGainDb was applied once at connect and never again.
    if (!m_flexBackend && m_backend) {
        m_backend->setPanRfGain(backendPanIdFor(panId), gain);
        return;
    }
    sendCmd(QString("display pan set %1 rfgain=%2").arg(panId).arg(gain));
}

// The discrete front-end stages. No Flex fallback: a Flex publishes no preamp
// or attenuator labels, so its controls never appear and nothing can call
// these. Routing them through the seam unconditionally keeps that true — a
// `display pan set` fallback here would be wire text for a control that does
// not exist.
void RadioModel::setPanPreampFor(const QString& panId, int step)
{
    if (panId.isEmpty() || !m_backend) return;
    m_backend->setPanPreamp(backendPanIdFor(panId), step);
}

void RadioModel::setPanAttenuatorFor(const QString& panId, int step)
{
    if (panId.isEmpty() || !m_backend) return;
    m_backend->setPanAttenuator(backendPanIdFor(panId), step);
}

// ── Display controls — FFT ─────────────────────────────────────────────────

void RadioModel::setPanAverage(int frames)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 average=%2").arg(m_activePanId).arg(frames));
}

void RadioModel::setPanFps(int fps)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 fps=%2").arg(m_activePanId).arg(fps));
}

void RadioModel::setPanWeightedAverage(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 weighted_average=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

// ── Display controls — Waterfall ──────────────────────────────────────────

void RadioModel::setWaterfallColorGain(int gain)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 color_gain=%2").arg(activeWfId()).arg(gain));
}

void RadioModel::setWaterfallBlackLevel(int level)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 black_level=%2").arg(activeWfId()).arg(level));
}

void RadioModel::setWaterfallAutoBlack(bool on)
{
    m_wfAutoBlackOn = on;
    applyWaterfallAutoBlack();
}

void RadioModel::setWaterfallAutoBlackSource(bool radioSide)
{
    m_wfAutoBlackRadioSide = radioSide;
    applyWaterfallAutoBlack();
}

void RadioModel::applyWaterfallAutoBlack()
{
    // The radio only needs to compute and embed its per-tile auto-black level
    // when the user has selected radio-side auto-black AND auto-black is on.
    // Otherwise the client renders the floor from its own estimate, so keep
    // auto_black=0 (radio-authoritative when, and only when, the user asks).
    if (activeWfId().isEmpty()) return;
    // …and only when the RADIO can actually do it. m_wfAutoBlackRadioSide is the
    // operator's stored intent, which deliberately survives a session on a radio
    // that computes no black level (#4606), so the capability has to be ANDed in
    // here too. Belt-and-braces with the GUI's own mask: this is the one place
    // that emits the command, so a caller that reaches it with a stale intent —
    // say a connect-time push that lands before the widget has been told the
    // capability — still cannot ask a radio for a level it will never send.
    const int v = (m_wfAutoBlackOn && m_wfAutoBlackRadioSide
                   && hasRadioSideWaterfallAutoBlack()) ? 1 : 0;
    sendCmd(
        QString("display panafall set %1 auto_black=%2")
            .arg(activeWfId()).arg(v));
}

void RadioModel::setWaterfallLineDuration(int ms)
{
    if (activeWfId().isEmpty()) return;
    sendCmd(
        QString("display panafall set %1 line_duration=%2").arg(activeWfId()).arg(ms));
}

void RadioModel::setPanNoiseFloorPosition(int pos)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 noise_floor_position=%2").arg(m_activePanId).arg(pos));
}

void RadioModel::setPanNoiseFloorEnable(bool on)
{
    if (m_activePanId.isEmpty()) return;
    sendCmd(
        QString("display pan set %1 noise_floor_position_enable=%2").arg(m_activePanId).arg(on ? 1 : 0));
}

// ─── Connection slots ─────────────────────────────────────────────────────────

void RadioModel::onBackendSpectrumFrame(int panId, const QByteArray& frame)
{
    // aetherd Gap B (Step 2): the HL2 data-plane payload is a raw float32 array
    // (Hl2Backend::floatBytes) of DC-centred dBFS bins. Producer and consumer are
    // the same process, so a straight memcpy round-trips the host-endian floats;
    // the documented little-endian contract matters only for the step-4 binary
    // wire format that supersedes this relay cross-machine.
    const int binCount = static_cast<int>(frame.size() / sizeof(float));
    if (binCount <= 0)
        return;
    QVector<float> bins(binCount);
    memcpy(bins.data(), frame.constData(),
           static_cast<std::size_t>(binCount) * sizeof(float));

    // Stream id: prefer the REAL pan's id when this session has a
    // PanadapterModel, and only fall back to the neutral synthetic base when it
    // does not.
    //
    // HL2 has no PanadapterModel yet, so the UI's spectrum handler takes its
    // empty-panadapters fallback and draws into the active pane — enough for
    // first-light, and the neutral id is right for it. Step 3 gives HL2 a real
    // pan + unique id.
    //
    // The demo (RFC #4288 Route A) DOES own a pan — it claims 0x40000000 from
    // its synthetic wire status — so emitting the neutral 0xE1000000 here meant
    // the id never matched any pan, every frame hit the "unmatched" branch, and
    // MainWindow logged `dropped unmatched FFT stream` ~20x/second for the whole
    // session. The panadapter still painted only because wireBackendSeam() draws
    // it directly, which ALSO bypassed the neutral consumers (the adaptive RX
    // filter and the S-history markers), so those received nothing at all.
    quint32 streamId = kNeutralPanStreamIdBase + static_cast<quint32>(panId);
    // The backend's OWN pan for this index first. A backend that claims its
    // pans over a wire (the demo's Route A) keys m_panadapters by the WIRE
    // id ("0x40000001"), which the neutral synthesis below can never
    // produce — so before the multi-pan demo every demo row fell through to
    // resolvePan()'s active-pan fallback, which is wrong the moment a second
    // pan exists: every receiver's rows drew into whichever pane was active.
    // The index↔backend-id pair is allocated by neutralPanIndexFor() when
    // the pan's geometry first crosses the seam, which precedes any row.
    PanadapterModel* pan = nullptr;
    const QString backendPanId = m_backendPanIdByIndex.value(panId);
    if (!backendPanId.isEmpty())
        pan = m_panadapters.value(normalizePanadapterId(backendPanId), nullptr);
    // Else the neutral resolution (HL2 pans; falls back to the active pan).
    if (!pan)
        pan = resolvePan(neutralPanIdString(panId));
    if (pan) {
        if (const quint32 realId = pan->panStreamId())
            streamId = realId;
    }
    const qint64 nowNs = PerfTelemetry::nowNs();

    // The PAN feed is already at the operator's rate — the backend caps its own
    // production at the source (IRadioBackend::setPanFrameRate), where a frame
    // that is not due costs nothing instead of being computed and discarded.
    // So this is a straight pass-through.
    emit panFeedSpectrumReady(streamId, bins, nowNs);

    // Drive the waterfall from the same frames. The backend supplies no separate
    // waterfall plane (Flex gets one from the radio), so the panadapter row IS
    // the waterfall row; it needs real band edges to scale against.
    //
    // Gated once more, because the waterfall rate is a SEPARATE control from
    // the frame rate and normally asks for something slower. That gate is what
    // makes a row actually represent the requested span of time — the
    // calibration the widget's time axis already assumes.
    // Geometry for THIS pan. `panId` here is already the neutral index, which is
    // the same key the geometry handler stores under.
    const double panBandwidthMhz = m_backendPanBandwidthMhz.value(panId, 0.0);
    const double panCenterMhz = m_backendPanCenterMhz.value(panId, 0.0);
    if (panBandwidthMhz > 0.0) {
        // Row PACING (origin/main #4476): emit at the pan's declared
        // waterfallLineDuration rather than once per spectrum frame, advancing BY
        // the interval so the rate does not quantise onto the frame grid.
        // resolvePan(), NOT panadapter(): the latter is an exact-key lookup and
        // the demo's pan is keyed by the id it CLAIMED from its synthetic wire
        // status (0x40000000), not by the neutral string. The exact lookup misses,
        // so the row goes out on the neutral id and MainWindow drops it as
        // unmatched — 343 rows in a 20 s session, measured. resolvePan() falls
        // back to the active pan, which is what the spectrum path above uses and
        // why that side reports zero drops.
        const PanadapterModel* pan = resolvePan(neutralPanIdString(panId));
        // waterfallLineDuration() carries the 1..100 RATE, not milliseconds —
        // low is slow, high is fast (see WaterfallRate.h). Pacing on the raw
        // number ran the control backwards here: rate 1 gated at 1 ms and gave
        // the full 25 fps, rate 100 gated at 100 ms and gave 10 (#4606).
        const int wfRate = (pan && pan->waterfallLineDuration() > 0)
                               ? pan->waterfallLineDuration()
                               : kBackendDefaultWfRate;
        const int wfMs = AetherSDR::WaterfallRate::localRowIntervalMs(wfRate);
        qint64& lastNs = m_backendWfLastRowNs[panId];
        const qint64 dueNs = static_cast<qint64>(wfMs) * 1000000;
        // First row goes out immediately: the interval is the gap BETWEEN rows,
        // not a delay before the first one. A zero interval is the top of the
        // control asking for one row per frame, which this same test gives.
        if (lastNs == 0 || (nowNs - lastNs) >= dueNs) {
            // Advance BY the interval rather than resetting to now, so the row
            // rate does not quantise down onto the frame grid. Clamped to one
            // interval of backlog so a stall cannot produce a catch-up burst.
            lastNs = (lastNs == 0) ? nowNs : lastNs + dueNs;
            if (nowNs - lastNs > dueNs)
                lastNs = nowNs - dueNs;
            const double half = panBandwidthMhz / 2.0;
            // Stream ID (this branch, review finding 8): a session that owns a pan
            // must use ITS waterfall id, or every row is dropped as unmatched and
            // MainWindow logs "dropped unmatched waterfall stream" ~20x/s. The
            // neutral base is correct only for a backend with no PanadapterModel
            // (HL2 today). Kept INSIDE main's pacing gate so both apply.
            quint32 wfId = kNeutralWfStreamIdBase + static_cast<quint32>(panId);
            if (pan) {
                if (const quint32 realWfId = pan->wfStreamId())
                    wfId = realWfId;
            }
            emit panFeedWaterfallRowReady(wfId, bins,
                                          panCenterMhz - half,
                                          panCenterMhz + half,
                                          m_backendWfTimecode++, nowNs);
        }
    }
}


void RadioModel::onConnected()
{
    qCDebug(lcProtocol) << "RadioModel: connected (family=" << m_family << ")";
    m_connectAttemptActive = false;  // the attempt landed (#4912)
    m_reconnectTimer.stop();
    m_rebootInProgress = false;
    // Belt-and-braces (#4122 review): the connect entry points clear fixtures,
    // but isConnected() stays false for the whole Connecting phase, so a
    // fixture applied while the handshake was in flight (seconds on WAN)
    // would otherwise be staged below and "reclaimed" by the real status
    // replay — with the fixture set staying poisoned for the session.
    clearAutomationSliceFixtures();
    stageSessionModelsForReconnect();
    armClientConnectionNoticeSuppression();
    setActivePanResized(false);

    // Inhibit system sleep while connected if the user has opted in (#1420)
    if (AppSettings::instance().value("InhibitSleepWhileConnected", "False").toString() == "True")
        m_sleepInhibitor.acquire("AetherSDR connected to radio");

    emit connectionStateChanged(true);
    // A Flex dumps its memory slots as status during the handshake below. A
    // radio without any has nothing to dump, so the bank is what populates the
    // cache — settle which store owns the session here, on the same edge, so
    // the browse panel and the memory-spot feed come up populated either way.
    syncMemoryStoreForSession();
    // Delay network monitor until after client gui registration
    // (pings sent before registration cause "Malformed command" on WAN)

    // Register as GUI client FIRST — required before subscriptions,
    // especially on WAN/SmartLink where the radio is stricter.
    disconnectPendingClientsThen([this] {
        if (m_wanConn) {
            // On WAN: wait for client ip response before sending client gui.
            // The radio needs time after wan validate to accept GUI registration.
            // multiFLEX conflict on WAN is caught pre-connection via licensedClients.
            sendCmd("client ip", [this](int, const QString& body) {
                qCDebug(lcProtocol) << "RadioModel: client ip ->" << body.trimmed();
                registerAsGuiClient(AppSettings::instance().effectiveGuiClientId());
            });
        } else {
            // On LAN: peek at radio/client status before sending client gui so we
            // can detect a multiFLEX conflict regardless of what discovery provided.
            peekForMultiFlexConflictThen([this] {
                registerAsGuiClient(AppSettings::instance().effectiveGuiClientId());
            });
        }
    });
}

void RadioModel::stageSessionModelsForReconnect()
{
    ++m_sessionModelGeneration;
    // The PREVIOUS session's handle, captured when that session registered
    // (m_ownSessionHandle). clientHandle() is useless here: by stage time the
    // connection has already assigned (or zeroed) the NEW session's handle,
    // so capturing it would make the reclaim-eviction guard dead code. (#3977)
    m_staleSessionOwnHandle = m_ownSessionHandle;
    m_ownSessionHandle = 0;

    for (SliceModel* slice : m_slices) {
        if (slice) {
            m_staleSlices.insert(slice->sliceId(), slice);
        }
    }
    m_slices.clear();

    for (auto it = m_panadapters.cbegin(); it != m_panadapters.cend(); ++it) {
        if (it.value()) {
            it.value()->setResized(false);
            it.value()->setWaterfallConfigured(false);
            it.value()->resetCenterKnownForReconnect();
            m_stalePanadapters.insert(it.key(), it.value());
        }
    }
    m_panadapters.clear();

    m_ownedSliceIds.clear();
    if (!m_rawSliceModeLists.isEmpty()) {
        m_rawSliceModeLists.clear();
        emit rawSliceModeListsChanged();
    }
    m_foreignSliceOwners.clear();
    m_pendingPanStatuses.clear();
    m_panTransmitInhibitReasons.clear();
    m_panTransmitInhibitedTxSlices.clear();
    m_activePanId.clear();

    if (!m_staleSlices.isEmpty() || !m_stalePanadapters.isEmpty()) {
        qCDebug(lcProtocol) << "RadioModel: staged previous session models for reconnect"
                            << "slices=" << m_staleSlices.size()
                            << "pans=" << m_stalePanadapters.size()
                            << "generation=" << m_sessionModelGeneration;
    }

    // Cross-radio guard: staged models are only reclaimable against the radio
    // they came from — slice indexes (0..n) and stream IDs (0x40000000…)
    // collide near-certainly across radios, and a reclaimed SliceModel would
    // drain its queued commands at the wrong radio. On LAN the discovery
    // serial is known here; on WAN it isn't, so registerAsGuiClient() repeats
    // this check when the "info" reply delivers chassis_serial.
    const QString targetSerial = m_wanConn ? QString() : m_lastInfo.serial;
    if (!m_staleSessionSerial.isEmpty() && !targetSerial.isEmpty()
        && targetSerial != m_staleSessionSerial) {
        qCDebug(lcProtocol) << "RadioModel: connect target serial" << targetSerial
                            << "differs from staged session serial" << m_staleSessionSerial
                            << "— dropping previous-session models";
        pruneStaleSessionModels(m_sessionModelGeneration);
    }
}

void RadioModel::pruneStaleSessionModels(quint64 generation)
{
    if (generation != m_sessionModelGeneration || !isConnected()) {
        return;
    }

    if (m_staleSlices.isEmpty() && m_stalePanadapters.isEmpty()) {
        return;
    }

    const QMap<int, SliceModel*> staleSlices = m_staleSlices;
    m_staleSlices.clear();
    for (auto it = staleSlices.cbegin(); it != staleSlices.cend(); ++it) {
        if (!it.value()) {
            continue;
        }
        qCDebug(lcProtocol) << "RadioModel: pruning stale slice after reconnect" << it.key();
        emit sliceRemoved(it.key());
        emit slotOccupancyChanged(it.key());
        it.value()->deleteLater();
    }

    const QMap<QString, PanadapterModel*> stalePans = m_stalePanadapters;
    m_stalePanadapters.clear();
    for (auto it = stalePans.cbegin(); it != stalePans.cend(); ++it) {
        if (!it.value()) {
            continue;
        }
        qCDebug(lcProtocol) << "RadioModel: pruning stale panadapter after reconnect" << it.key();
        emit panadapterRemoved(it.key());
        it.value()->deleteLater();
    }
}

void RadioModel::dropAllSessionModelsForFamilySwitch()
{
    // Live models (present if the switch happens without a clean disconnect).
    const QList<SliceModel*> liveSlices = m_slices;
    m_slices.clear();
    for (SliceModel* s : liveSlices) {
        if (!s) continue;
        emit sliceRemoved(s->sliceId());
        emit slotOccupancyChanged(s->sliceId());
        s->deleteLater();
    }
    const QMap<QString, PanadapterModel*> livePans = m_panadapters;
    m_panadapters.clear();
    for (auto it = livePans.cbegin(); it != livePans.cend(); ++it) {
        if (!it.value()) continue;
        emit panadapterRemoved(it.key());
        it.value()->deleteLater();
    }

    // Staged (stale) models from the previous session — these are what the
    // serial guard would have (failed to) prune.
    const QMap<int, SliceModel*> staleSlices = m_staleSlices;
    m_staleSlices.clear();
    for (auto it = staleSlices.cbegin(); it != staleSlices.cend(); ++it) {
        if (!it.value()) continue;
        emit sliceRemoved(it.key());
        emit slotOccupancyChanged(it.key());
        it.value()->deleteLater();
    }
    const QMap<QString, PanadapterModel*> stalePans = m_stalePanadapters;
    m_stalePanadapters.clear();
    for (auto it = stalePans.cbegin(); it != stalePans.cend(); ++it) {
        if (!it.value()) continue;
        emit panadapterRemoved(it.key());
        it.value()->deleteLater();
    }

    // Reset the session-identity state so nothing from the old family lingers as
    // a reclaim candidate for the new one.
    m_ownedSliceIds.clear();
    m_foreignSliceOwners.clear();
    m_activePanId.clear();
    m_staleSessionSerial.clear();
    m_chassisSerial.clear();
}

void RadioModel::disconnectPendingClientsThen(std::function<void()> continuation)
{
    const QList<quint32> handles = m_pendingClientDisconnects;
    m_pendingClientDisconnects.clear();
    disconnectClientHandlesThen(handles, std::move(continuation));
}

void RadioModel::disconnectClientHandlesThen(const QList<quint32>& requestedHandles,
                                             std::function<void()> continuation)
{
    QList<quint32> handles;
    const quint32 ours = clientHandle();
    for (quint32 handle : requestedHandles) {
        if (handle != 0 && handle != ours && !handles.contains(handle))
            handles.append(handle);
    }
    if (handles.isEmpty()) {
        if (continuation)
            continuation();
        return;
    }

    auto remaining = std::make_shared<QList<quint32>>(handles);
    auto completion = std::make_shared<std::function<void()>>(std::move(continuation));
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, remaining, completion, step]() mutable {
        if (remaining->isEmpty()) {
            if (*completion) {
                QTimer::singleShot(250, this, [completion]() mutable {
                    auto continuation = std::move(*completion);
                    if (continuation)
                        continuation();
                });
            }
            return;
        }

        const quint32 handle = remaining->takeFirst();
        const QString command = QString("client disconnect 0x%1").arg(handle, 0, 16);
        qCDebug(lcProtocol) << "RadioModel: disconnecting occupied client" << Qt::hex << handle;
        sendCmd(command, [handle, step](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcProtocol) << "RadioModel: client disconnect failed for"
                                      << Qt::hex << handle
                                      << "code" << code
                                      << "body:" << body;
            }
            (*step)();
        });
    };

    (*step)();
}

void RadioModel::peekForMultiFlexConflictThen(std::function<void()> continuation)
{
    m_multiFlexContinuation = continuation;

    // Evict stale entries pre-populated from discovery or a previous session.
    // We rebuild the map from scratch using only what the radio confirms via
    // sub client all below, so we never show phantom handles in the dialog.
    const quint32 ours = clientHandle();
    for (auto it = m_clientInfoMap.begin(); it != m_clientInfoMap.end(); ) {
        it = (it.key() != ours) ? m_clientInfoMap.erase(it) : std::next(it);
    }
    m_clientStations.clear();
    // Foreign-slot markers are rebuilt from fresh client/slice statuses
    // after the resub below; clear them so the tab row doesn't show
    // pre-reconnect dim placeholders during the gap (#2606).
    m_foreignSliceOwners.clear();

    // Subscribe to radio and client topics early — before client gui — to get
    // mf_enable and the live connected-client list directly from the radio.
    // 400 ms is enough for the radio's status burst to arrive on a LAN path.
    sendCmd("sub radio all", [this](int, const QString&) {
        sendCmd("sub client all", [this](int, const QString&) {
            resolveLiveGuiClientIdCollision();
            // Fast path: when multiFLEX is enabled the radio explicitly allows
            // multiple GUI clients, so the conflict check below
            // (!m_multiFlexEnabled && hasOthers) can never fire — there is
            // nothing to wait for. mf_enable arrives in the radio status burst
            // triggered by "sub radio all" above, so m_multiFlexEnabled is set
            // by the time this callback runs. Skipping the 400 ms window here
            // shaves it off the connect handshake. When mf is disabled (or its
            // status hasn't arrived yet) we fall through to the original wait.
            if (m_multiFlexEnabled) {
                if (m_multiFlexContinuation) {
                    auto cont = std::move(m_multiFlexContinuation);
                    m_multiFlexContinuation = nullptr;
                    cont();
                }
                return;
            }
            QTimer::singleShot(400, this, [this] {
                // On the non-fast path, client status may arrive during the
                // collection window rather than before the subscription reply.
                resolveLiveGuiClientIdCollision();
                const quint32 ours2 = clientHandle();
                bool hasOthers = false;
                for (auto it = m_clientInfoMap.cbegin(); it != m_clientInfoMap.cend(); ++it) {
                    if (it.key() != ours2) {
                        hasOthers = true;
                        break;
                    }
                }

                // If the map is still empty (only our own handle or nothing) after
                // waiting for the burst, the radio either hasn't sent any client
                // status yet or the burst arrived after the window closed.  Log so
                // field reports of missed conflicts are diagnosable.  The connection
                // proceeds — the alternative is a hang, which is worse than a miss.
                if (m_clientInfoMap.isEmpty() || (m_clientInfoMap.size() == 1 && m_clientInfoMap.contains(ours2)))
                    qCWarning(lcProtocol) << "RadioModel: peek window closed with no client status received —"
                                            " conflict detection may have been missed (busy radio or lossy LAN)";

                if (!m_multiFlexEnabled && hasOthers) {
                    qCDebug(lcProtocol) << "RadioModel: multiFLEX disabled, other clients present — pausing connection";
                    emit multiFlexConflictDetected();
                    return;
                }

                // No conflict — proceed with registration.
                if (m_multiFlexContinuation) {
                    auto cont = std::move(m_multiFlexContinuation);
                    m_multiFlexContinuation = nullptr;
                    cont();
                }
            });
        });
    });
}

void RadioModel::resolveLiveGuiClientIdCollision()
{
    AppSettings& settings = AppSettings::instance();
    const QString effectiveId = settings.effectiveGuiClientId();
    if (effectiveId.isEmpty()) {
        return;
    }
    for (auto it = m_clientInfoMap.cbegin(); it != m_clientInfoMap.cend(); ++it) {
        if (it.key() == clientHandle()
            || it->clientId.compare(effectiveId, Qt::CaseInsensitive) != 0) {
            continue;
        }

        const QString otherStation = it->station;
        const bool selectDistinct = GuiClientIdentityPolicy::shouldSelectDistinctId(
            settings.guiClientIdentityIsTransient(), settings.effectiveStationName(),
            otherStation, it.key() == m_staleSessionOwnHandle);
        // Only a handle captured from THIS process's previous connection may
        // be reclaimed. A same-named station from a fresh process can be a
        // cloned remote profile and must not be evicted by assumption.
        if (!selectDistinct) {
            qCInfo(lcProtocol).noquote()
                << "RadioModel: reclaiming same-station GUI session"
                << QStringLiteral("handle=%1").arg(hexId(it.key()))
                << QStringLiteral("station=%1").arg(otherStation);
            return;
        }
        if (settings.resolveLiveGuiClientIdCollision(otherStation)) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: avoided live duplicate GUI client ID"
                << QStringLiteral("handle=%1").arg(hexId(it.key()))
                << QStringLiteral("station=%1").arg(otherStation)
                << QStringLiteral("new_id=%1").arg(settings.effectiveGuiClientId());
        }
        return;
    }
}

void RadioModel::resolveMultiFlexConflict(quint32 handle)
{
    // Move the continuation out so peekForMultiFlexConflictThen can safely
    // re-assign m_multiFlexContinuation without trampling over it.
    auto continuation = std::move(m_multiFlexContinuation);
    m_multiFlexContinuation = nullptr;

    qCDebug(lcProtocol) << "RadioModel: resolving multiFLEX conflict, disconnecting" << Qt::hex << handle;
    disconnectClientHandlesThen({handle}, [this, continuation = std::move(continuation)]() mutable {
        // After eviction, re-run the full peek rather than jumping straight to the
        // continuation. This catches any remaining clients (e.g., two sessions, or
        // a stale handle that hadn't been cleaned up yet) and re-shows the dialog if
        // needed, preventing phantom slices from a partially-disconnected session.
        if (continuation)
            peekForMultiFlexConflictThen(std::move(continuation));
    });
}

void RadioModel::cancelMultiFlexConflict()
{
    m_multiFlexContinuation = nullptr;
    m_intentionalDisconnect = true;
    QMetaObject::invokeMethod(m_connection, [conn = m_connection] {
        conn->disconnectFromRadio();
    });
}

void RadioModel::handleForcedClientDisconnect()
{
    if (m_forcedDisconnectInProgress)
        return;

    m_forcedDisconnectInProgress = true;
    m_intentionalDisconnect = true;
    m_reconnectTimer.stop();

    qCWarning(lcProtocol) << "RadioModel: this GUI client was force-disconnected by another client";
    emit forcedDisconnectRequested();

    closeConnectionForTerminalDisconnect();
}

// Tear down the transport for a radio-initiated terminal disconnect (forced or
// duplicate-client-id). The radio evicts our GUI-client registration but does
// not guarantee closing the raw TCP/TLS socket — FlexLib fires an event and
// leaves the actual disconnect to the client — so we must close it ourselves,
// or the model is stranded in a half-open "connected" state with dangling
// streams. Callers set m_intentionalDisconnect first so this does not trip the
// auto-reconnect loop.
void RadioModel::closeConnectionForTerminalDisconnect()
{
    if (m_wanConn) {
        m_wanConn->disconnectFromRadio();
        return;
    }

    const quint32 handle = clientHandle();
    const QString streamId = RadioStatusOwnership::streamCommandId(m_rxAudio.streamId);
    if (m_connection->isConnected()) {
        const quint32 streamRemoveSeq = streamId.isEmpty() ? 0 : m_seqCounter.fetch_add(1);
        QMetaObject::invokeMethod(m_connection, [conn = m_connection, handle, streamId,
                                                 streamRemoveSeq]() {
            conn->gracefulDisconnect(handle, streamId, streamRemoveSeq);
        });
    } else {
        QMetaObject::invokeMethod(m_connection, &RadioConnection::disconnectFromRadio);
    }
}

void RadioModel::handleDuplicateClientIdDisconnect()
{
    if (m_forcedDisconnectInProgress) {
        return;
    }
    m_forcedDisconnectInProgress = true;
    m_intentionalDisconnect = true;
    m_reconnectTimer.stop();
    AppSettings::instance().resolveLiveGuiClientIdCollision(
        QStringLiteral("radio-reported duplicate"));
    qCWarning(lcProtocol)
        << "RadioModel: radio disconnected this session because another GUI client used the same ID";
    emit connectionError(tr("Connection stopped: another AetherSDR client was using this "
                            "station identity. A distinct identity has been selected; reconnect "
                            "to continue."));
    // Close the socket ourselves — the radio does not guarantee a TCP close on
    // a duplicate-id eviction, and the error above tells the operator to
    // reconnect, which must start from a fully torn-down connection.
    closeConnectionForTerminalDisconnect();
}

void RadioModel::handleGuiClientRegistrationFailure(
    const GuiClientRegistrationState::Result& result)
{
    m_intentionalDisconnect = true;
    m_reconnectTimer.stop();

    const QString detail = result.detail.isEmpty()
        ? tr("The radio rejected the request without additional detail.")
        : result.detail;
    const QString message =
        tr("GUI client registration failed (%1): %2 "
           "AetherSDR disconnected without retrying. Free a GUI client slot if "
           "needed, then choose Connect to try again.")
            .arg(hexCode(result.resultCode), detail);

    qCWarning(lcProtocol).noquote()
        << "RadioModel: terminal GUI client registration failure"
        << QStringLiteral("code=%1").arg(hexCode(result.resultCode))
        << QStringLiteral("detail=%1").arg(detail);
    emit guiClientRegistrationFailed(message);
    emit connectionError(message);
    closeConnectionForTerminalDisconnect();
}

void RadioModel::registerAsGuiClient(const QString& clientId)
{
    // aetherd Gap B: SmartSDR GUI-client registration is Flex-only. Every step
    // here — the client/sub command batch, client-handle negotiation and the
    // VITA-49 UDP stream bring-up (including the deferred 10 s no-data health
    // check) — speaks the Flex command protocol over a RadioConnection and drives
    // a PanadapterStream. A non-Flex backend has neither, and its data plane is
    // already streaming by the time connected() fires, so the whole flow is
    // inapplicable. Without this the health-check timer fires ~10 s after connect
    // and dereferences the absent stream, crashing shortly after startup.
    if (!m_connection || !m_panStream)
        return;

    // Match FlexLib connect-sequence ordering (Radio.cs:2230-2247):
    //   client program <name>  →  client low_bw_connect  →  client gui
    // The radio's protocol state machine requires client identity (program)
    // BEFORE accepting client-mode configuration (low_bw_connect), and the
    // bandwidth mode must be set BEFORE GUI registration so it is baked
    // into the client session.  Sending these out of order — as we did
    // previously — caused the radio to silently ignore low_bw_connect
    // (#2447: "Low Bandwidth checkbox doesn't do anything meaningful").
    sendCmd("client program AetherSDR");
    if (AppSettings::instance().value("LowBandwidthConnect", "False").toString() == "True")
        sendCmd("client low_bw_connect");

    m_guiClientRegistrationState.begin();
    sendCmd(QString("client gui %1").arg(clientId), [this](int code, const QString& body) {
        armClientConnectionNoticeSuppression();
        if (code != 0) {
            // Commit the rejection before a prompt TCP close can reset the
            // registration state and re-arm automatic reconnect (#4560).
            // RadioConnection preserves protocol-line order, so a preceding
            // fatal M-message has already reached the state when R is handled.
            //
            // What the old singleShot(0) covered and this does not: an M that
            // arrived AFTER the R would have been queued ahead of the timer and
            // picked up as detail. Now it is not, so that case degrades to the
            // generic no-detail text below. That is the trade — a correct
            // terminal transition beats a better string, and the #4481 capture
            // shows MF3000001 arriving BEFORE the R, whose body already carried
            // the reason anyway.
            const GuiClientRegistrationState::Result result =
                m_guiClientRegistrationState.complete(code, body);
            handleGuiClientRegistrationFailure(result);
            return;
        }

        // code == 0: commit the Registered phase and clear any stashed detail.
        // The result is unconditionally ContinueHandshake, so there is nothing
        // to branch on — the return is dropped deliberately, not overlooked.
        m_guiClientRegistrationState.complete(code, body);

        if (!body.trimmed().isEmpty()
            && !AppSettings::instance().guiClientIdentityIsTransient()) {
            // Save our UUID for session persistence across restarts.
            // The radio restores slices/frequencies for a known UUID.
            auto& s = AppSettings::instance();
            s.recordPersistentGuiClientIdReply(body.trimmed());
            qCDebug(lcProtocol) << "RadioModel: saved GUIClientID:" << body.trimmed();
        }

        // #3977: remember THIS session's handle for the next reconnect's
        // reclaim-eviction guard. Captured here (reply time) because the
        // handle is guaranteed assigned once any command round-trips.
        m_ownSessionHandle = clientHandle();

        sendCmd(QString("client station %1").arg(ourStationName()));
        sendCmd("client set send_reduced_bw_dax=1");
        // Set network MTU for VITA-49 packets (matches FlexLib behavior)
        int mtu = AppSettings::instance().value("NetworkMtu", "1450").toInt();
        sendCmd(QString("client set enforce_network_mtu=1 network_mtu=%1").arg(mtu));
        // Enable keepalive (matches FlexLib behavior) — ping timer starts in startNetworkMonitor()
        sendCmd("keepalive enable");
        startNetworkMonitor();

        // Subscriptions are independent topics and the radio processes TCP commands
        // in send order, so fire them back-to-back without round-tripping on each R
        // response — exactly as the second sub batch (sub tnf/dax/codec/…) below
        // already does. The previous one-RTT-per-sub chain serialized ~11 round
        // trips (~0.7 s on a LAN) into the connect handshake for no protocol reason.
        sendCmd("sub slice all");
        sendCmd("sub pan all");
        sendCmd("sub tx all");
        sendCmd("sub atu all");
        sendCmd("sub amplifier all");
        sendCmd("sub meter all");
        sendCmd("sub audio all");
        sendCmd("sub gps all");
        sendCmd("sub apd all");
        // Suppression must be armed BEFORE "sub client all" because that
        // subscription is what triggers the radio to send the client-status
        // burst we want to suppress notices for. The earlier subs in this
        // batch don't generate client-connection notices, so positioning here
        // is safe; the old nested-callback layout made this self-documenting
        // (suppression was armed in the sub apd response callback), the flat
        // layout doesn't — hence the comment.
        armClientConnectionNoticeSuppression();
        sendCmd("sub client all");
        sendCmd("sub xvtr all");
        // Memory status arrives via normal status handler — no subscription needed.
        // "sub memory all" returns 500000A3 (invalid subscription object).
        // Request available mic inputs (comma-separated response: "MIC,BAL,LINE,ACC")
        sendCmd("mic list", [this](int code, const QString& body) {
            if (code == 0) {
                QStringList inputs = body.trimmed().split(',', Qt::SkipEmptyParts);
                m_transmitModel.setMicInputList(inputs);
                qCDebug(lcProtocol) << "RadioModel: mic inputs:" << inputs;
            }
        });

        // Always (re)start on connect — re-binds socket and re-registers
        // UDP port with the radio. start() calls stop() internally if needed. (#561)
        bool streamOk = false;
        if (m_wanConn) {
            QMetaObject::invokeMethod(m_panStream, [this, &streamOk]() {
                streamOk = m_panStream->startWan(QHostAddress(m_wanPublicIp), m_wanUdpPort);
            }, Qt::BlockingQueuedConnection);
        } else {
            QMetaObject::invokeMethod(m_panStream, [this, &streamOk]() {
                streamOk = m_panStream->start(m_connection);
            }, Qt::BlockingQueuedConnection);
        }

        if (!streamOk) {
            qCWarning(lcProtocol) << "RadioModel: UDP stream setup failed — disconnecting gracefully (#894)";
            emit connectionError(tr("UDP stream setup failed. If connecting over VPN, "
                                    "ensure UDP traffic from the radio is routable."));
            QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
            return;
        }

        // Schedule a UDP stream health check: if no VITA-49 data arrives
        // within 10 seconds (e.g. VPN blocks UDP), warn the user. (#894)
        QTimer::singleShot(10000, this, [this]() {
            if (!isConnected()) return;
            // Flex-only check: it asks "did VITA-49 UDP arrive on the pan stream?",
            // which is meaningful solely for a backend whose spectrum RIDES that
            // UDP. The demo's spectrum arrives over the IRadioBackend seam and its
            // pan stream binds no socket, so hasReceivedPackets() is false forever
            // and this fired on every demo connect: a user-visible "No spectrum
            // data received" error, status flipped to Error, and the panadapter
            // connect animation cancelled — while demo spectrum and audio were
            // working fine. (It also guards the m_panStream null deref for any
            // backend that vends no stream at all.)
            if (!m_flexBackend || !m_panStream) return;
            if (!m_wanConn && !m_panStream->hasReceivedPackets()) {
                qCWarning(lcProtocol) << "RadioModel: no VITA-49 UDP data received after 10s"
                                      << "target=" << targetRadioIp()
                                      << "sourceMode=" << selectedSourceMode()
                                      << "sourcePath=" << selectedSourcePath()
                                      << "localTcp=" << localTcpEndpoint()
                                      << "localUdp=" << localUdpEndpoint()
                                      << "udpSeen=" << firstUdpPacketSeen();
                emit connectionError(
                    tr("No spectrum data received from %1. Source mode: %2. TCP: %3. UDP: %4. "
                       "UDP traffic from the radio may be blocked, or the wrong source path may be selected.")
                        .arg(targetRadioIp(), selectedSourceMode(),
                             localTcpEndpoint(), localUdpEndpoint()));
            }
        });

        // On WAN: use "client udp_register" via UDP (not TCP "client udpport").
        // The radio only accepts udp_register on WAN connections.
        if (m_wanConn) {
            QMetaObject::invokeMethod(m_panStream, [this]() {
                m_panStream->startWanUdpRegister(clientHandle());
            });
            qCDebug(lcProtocol) << "RadioModel: WAN — started UDP registration via udp_register";
        }

        const quint16 udpPort = m_panStream->localPort();
        qCInfo(lcProtocol).noquote()
            << "RadioModel: client udpport requested"
            << QStringLiteral("port=%1").arg(udpPort);
        sendCmd(
            QString("client udpport %1").arg(udpPort),
            [this, udpPort](int code2, const QString& body) {
                if (code2 == 0) {
                    qCInfo(lcProtocol).noquote()
                        << "RadioModel: client udpport registered"
                        << QStringLiteral("port=%1").arg(udpPort);
                } else if (shouldRetryLanUdpPortRegistration(m_wanConn != nullptr, code2, body)) {
                    bool rebound = false;
                    QMetaObject::invokeMethod(m_panStream, [this, &rebound]() {
                        rebound = m_panStream->rebindToEphemeralPort(m_connection);
                    }, Qt::BlockingQueuedConnection);

                    if (!rebound) {
                        qCWarning(lcProtocol) << "RadioModel: UDP port" << udpPort
                                              << "is already registered and AetherSDR could not rebind";
                        emit connectionError(tr("UDP port %1 is already in use by another Flex client, "
                                                "and AetherSDR could not switch to a free UDP port.")
                                                 .arg(udpPort));
                        QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
                        return;
                    }

                    const quint16 retryUdpPort = m_panStream->localPort();
                    qCWarning(lcProtocol).noquote()
                        << "RadioModel: client udpport collision"
                        << QStringLiteral("port=%1").arg(udpPort)
                        << QStringLiteral("code=%1").arg(hexCode(code2))
                        << QStringLiteral("retry_port=%1").arg(retryUdpPort);
                    qCInfo(lcProtocol).noquote()
                        << "RadioModel: client udpport requested"
                        << QStringLiteral("port=%1").arg(retryUdpPort);
                    sendCmd(
                        QString("client udpport %1").arg(retryUdpPort),
                        [this, retryUdpPort](int retryCode, const QString& retryBody) {
                            if (retryCode == 0) {
                                qCInfo(lcProtocol).noquote()
                                    << "RadioModel: client udpport retry registered"
                                    << QStringLiteral("port=%1").arg(retryUdpPort);
                            } else {
                                qCWarning(lcProtocol).noquote()
                                    << "RadioModel: client udpport retry failed"
                                    << QStringLiteral("code=%1").arg(hexCode(retryCode))
                                    << QStringLiteral("body=%1").arg(retryBody);
                                emit connectionError(tr("UDP port registration failed after switching to port %1: %2")
                                                         .arg(retryUdpPort)
                                                         .arg(retryBody));
                                QTimer::singleShot(0, this, &RadioModel::disconnectFromRadio);
                            }
                        });
                } else {
                    qCDebug(lcProtocol) << "RadioModel: client udpport returned error" << Qt::hex << code2
                             << "(expected on WAN — using udp_register instead)";
                }

                // Query radio info (region, callsign, options, etc.)
                // Response is comma-separated key=value pairs.
                sendCmd("info",
                    [this](int code, const QString& body) {
                        if (code != 0) return;
                        for (const QString& kv : body.split(',')) {
                            const int eq = kv.indexOf('=');
                            if (eq < 0) continue;
                            const QString key = kv.left(eq).trimmed();
                            const QString val = kv.mid(eq + 1).trimmed()
                                .remove('\\').remove('"');
                            if (key == "callsign") {
                                if (val != m_callsign) {
                                    m_callsign = val;
                                    emit callsignChanged(callsign());   // effective value, not the raw radio field
                                }
                            }
                            else if (key == "name")        m_nickname = val;
                            else if (key == "region")      m_region = val;
                            else if (key == "options")     m_radioOptions = val;
                            else if (key == "model") {
                                m_model = val;
                                m_maxSlices = maxSlicesForModel(m_model);
                            }
                            else if (key == "chassis_serial") m_chassisSerial = val;
                            else if (key == "software_ver")   m_version = val;
                            else if (key == "ip")             m_ip = val;
                            else if (key == "netmask")        m_netmask = val;
                            else if (key == "gateway")        m_gateway = val;
                            else if (key == "mac")            m_mac = val;
                        }
                        qCDebug(lcProtocol) << "RadioModel: info — callsign:" << m_callsign
                                 << "region:" << m_region << "options:" << m_radioOptions;

                        // Cross-radio guard, WAN leg: discovery serial isn't
                        // available at stage time over SmartLink, so drop any
                        // still-staged previous-session models as soon as the
                        // radio identifies itself as a different chassis.
                        if (!m_staleSessionSerial.isEmpty() && !m_chassisSerial.isEmpty()
                            && m_chassisSerial != m_staleSessionSerial) {
                            qCDebug(lcProtocol) << "RadioModel: chassis serial" << m_chassisSerial
                                                << "differs from staged session serial"
                                                << m_staleSessionSerial
                                                << "— dropping previous-session models";
                            pruneStaleSessionModels(m_sessionModelGeneration);
                        }

                        emit infoChanged();
                        if (reloadAntennaAliases())
                            emit antennaAliasesChanged();
                    });

                sendCmd("slice list",
                    [this](int code3, const QString& body) {
                        const quint64 restoreGeneration = m_sessionModelGeneration;
                        QTimer::singleShot(kSessionRestorePruneDelayMs, this, [this, restoreGeneration]() {
                            pruneStaleSessionModels(restoreGeneration);
                        });

                        if (code3 != 0) {
                            qCWarning(lcProtocol) << "RadioModel: slice list failed, code" << Qt::hex << code3;
                            return;
                        }
                        const QStringList ids = body.trimmed().split(' ', Qt::SkipEmptyParts);
                        qCDebug(lcProtocol) << "RadioModel: slice list ->" << (ids.isEmpty() ? "(empty)" : body);

                        if (ids.isEmpty()) {
                            // Radio reports no slices. Reuse an already-restored pan if we
                            // have one; only create a fresh panafall otherwise (#3212).
                            ensureDefaultSlicePreferringRestoredPan();
                        } else if (m_slices.isEmpty()) {
                            // Radio has slices but we haven't matched any to our
                            // client_handle yet (status messages still in flight).
                            // Defer the decision to give status messages time to
                            // arrive and populate m_slices via handleSliceStatus.
                            qCDebug(lcProtocol) << "RadioModel: radio has" << ids.size()
                                     << "slice(s) but none matched yet — deferring 500ms";
                            QTimer::singleShot(500, this, [this]() {
                                if (m_slices.isEmpty() && isConnected()) {
                                    qCDebug(lcProtocol) << "RadioModel: deferred check — still no owned slices, creating default";
                                    ensureDefaultSlicePreferringRestoredPan();
                                } else if (!m_slices.isEmpty()) {
                                    qCDebug(lcProtocol) << "RadioModel: deferred check — adopted"
                                             << m_slices.size() << "existing slice(s)";
                                }
                            });
                        } else {
                            qCDebug(lcProtocol) << "RadioModel: SmartConnect — using our pan"
                                     << m_activePanId << "and" << m_slices.size() << "slice(s)";
                        }

                        for (auto* s : m_slices) {
                            for (const QString& cmd : s->drainPendingCommands())
                                sendSliceCommand(s, cmd);
                        }

                        // Create remote_audio_rx if PC Audio is enabled. Defer briefly so
                        // SmartConnect/restored stream status can be adopted before
                        // we ask the radio for another stream. (#1014, #1051, #1137, #2037)
                        scheduleRxAudioStreamEnsure(QStringLiteral("connect"));

                        // Do not claim a dax_tx stream at GUI attach time. SmartSDR DAX
                        // owns that stream on Windows; AetherSDR creates one lazily only
                        // when its own bridge/TCI path needs to feed DAX TX audio.

                        // Request remote audio TX stream (voice mode, VOX monitoring).
                        // The radio-owned met_in_rx setting controls whether level
                        // meter data is reported while receiving.
                        // Create netcw stream for low-latency CW keying via UDP
                        sendCmd("stream create netcw",
                            [this](int code, const QString& body) {
                                if (code == 0) {
                                    m_netCwStreamId = body.trimmed().toUInt(nullptr, 16);
                                    m_netCwIndex = 1;
                                    m_netCwClock.invalidate();
                                    m_netCwLastSendMs = -1;
                                    qCDebug(lcProtocol) << "RadioModel: netcw stream created, id:"
                                             << Qt::hex << m_netCwStreamId;
                                } else {
                                    qCDebug(lcProtocol) << "RadioModel: netcw stream not supported, code"
                                             << Qt::hex << code << "— using cw key immediate fallback";
                                }
                            });

                        // Radio always forces Opus for remote_audio_tx regardless of
                        // what we request (confirmed by protocol testing, v1.4.0.0).
                        sendCmd(
                            "stream create type=remote_audio_tx compression=opus",
                            [this](int code, const QString& body) {
                                if (code == 0) {
                                    quint32 id = body.trimmed().toUInt(nullptr, 16);
                                    qCDebug(lcProtocol) << "RadioModel: remote_audio_tx stream created, id:"
                                             << Qt::hex << id;
                                    emit remoteTxStreamReady(id);
                                } else {
                                    qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_tx failed, code"
                                               << Qt::hex << code << "body:" << body;
                                }
                            });
                    });
            });
            // Request profile lists/current selections using FlexLib's info command.
            refreshProfiles();
            sendCmd("sub tnf all");
            sendCmd("sub memories all");
            // Additional subscriptions (matches SmartSDR connection sequence)
            sendCmd("sub cwx all");
            sendCmd("sub dax all");
            sendCmd("sub daxiq all");
            sendCmd("sub radio all");
            sendCmd("sub codec all");
            sendCmd("sub dvk all");
            sendCmd("sub navtex all");
            sendCmd("sub usb_cable all");
            sendCmd("sub spot all");
            sendCmd("sub waveform all");
            sendCmd("sub license all");
    }); // client gui
}

int RadioModel::bandIdForFrequency(double freqMhz) const
{
    // Standard amateur HF band ranges → band names matching radio's band_name field
    struct BandRange { double lo; double hi; const char* name; };
    static constexpr BandRange bands[] = {
        {1.8,    2.0,    "160"},
        {3.5,    4.0,    "80"},
        {5.0,    5.5,    "60"},
        {7.0,    7.3,    "40"},
        {10.1,   10.15,  "30"},
        {14.0,   14.35,  "20"},
        {18.068, 18.168, "17"},
        {21.0,   21.45,  "15"},
        {24.89,  24.99,  "12"},
        {28.0,   29.7,   "10"},
        {50.0,   54.0,   "6"},
        {144.0,  148.0,  "2m"},
    };

    for (const auto& b : bands) {
        if (freqMhz >= b.lo && freqMhz <= b.hi) {
            // Find the band ID with this name in m_txBandSettings
            for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it) {
                if (it->bandName == b.name)
                    return it->bandId;
            }
        }
    }
    // Out-of-band or GEN — check for GEN band
    for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it) {
        if (it->bandName == "GEN")
            return it->bandId;
    }
    return -1;
}

void RadioModel::applyTuneInhibit()
{
    auto& s = AppSettings::instance();
    double txFreq = 0.0;
    for (auto* sl : m_slices) {
        if (sl->isTxSlice()) { txFreq = sl->frequency(); break; }
    }
    int bandId = bandIdForFrequency(txFreq);
    if (bandId < 0) return;
    auto it = m_txBandSettings.find(bandId);
    if (it == m_txBandSettings.end()) return;

    QStringList inhibited;
    if (s.value("TuneInhibitAccTx", "False").toString() == "True" && it->accTx) {
        sendCmd(QString("interlock bandset %1 acc_tx_enabled=0").arg(bandId));
        inhibited << "ACC TX";
    }
    if (s.value("TuneInhibitTx1", "False").toString() == "True" && it->tx1) {
        sendCmd(QString("interlock bandset %1 tx1_enabled=0").arg(bandId));
        inhibited << "TX1";
    }
    if (s.value("TuneInhibitTx2", "False").toString() == "True" && it->tx2) {
        sendCmd(QString("interlock bandset %1 tx2_enabled=0").arg(bandId));
        inhibited << "TX2";
    }
    if (s.value("TuneInhibitTx3", "False").toString() == "True" && it->tx3) {
        sendCmd(QString("interlock bandset %1 tx3_enabled=0").arg(bandId));
        inhibited << "TX3";
    }
    if (!inhibited.isEmpty()) {
        m_tuneInhibitBandId = bandId;
        m_tuneInhibitActive = true;
        qDebug() << "Tune PA inhibit: disabled" << inhibited.join(", ")
                 << "on band" << bandId << "before tune";
    }
}

void RadioModel::restoreTuneInhibit()
{
    auto& s = AppSettings::instance();
    int id = m_tuneInhibitBandId;
    QStringList restored;
    if (s.value("TuneInhibitAccTx", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 acc_tx_enabled=1").arg(id));
        restored << "ACC TX";
    }
    if (s.value("TuneInhibitTx1", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx1_enabled=1").arg(id));
        restored << "TX1";
    }
    if (s.value("TuneInhibitTx2", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx2_enabled=1").arg(id));
        restored << "TX2";
    }
    if (s.value("TuneInhibitTx3", "False").toString() == "True") {
        sendCmd(QString("interlock bandset %1 tx3_enabled=1").arg(id));
        restored << "TX3";
    }
    qDebug() << "Tune PA inhibit: restored" << restored.join(", ") << "on band" << id;
    m_tuneInhibitActive = false;
    m_tuneInhibitBandId = -1;
}

void RadioModel::onDisconnected()
{
    qCDebug(lcProtocol) << "RadioModel: disconnected";
    m_guiClientRegistrationState.reset();

    // #4142 — void any pan centers deferred during a profile load. The session
    // they belonged to is gone: the radio rebuilds its topology on reconnect, so
    // a queued center could land a stale frequency on a pan that is no longer the
    // same pan. This is a real path, not a theoretical one — a large profile
    // (8 pans / 8 slices on a 6700) can stall the radio past the ping timeout and
    // force a disconnect BEFORE the profile load is ever ACKed.
    if (!m_pendingProfileLoadPanWrites.isEmpty()) {
        qCWarning(lcProtocol).noquote()
            << "RadioModel: discarding" << m_pendingProfileLoadPanWrites.size()
            << "deferred pan write(s) — disconnected before the profile load settled";
        m_pendingProfileLoadPanWrites.clear();
    }
    m_profileLoadPanWriteFlushTimer.stop();

    // Release sleep inhibition on disconnect (#1420)
    m_sleepInhibitor.release();

    // Safety: restore TX outputs if we were inhibiting during tune
    if (m_tuneInhibitActive && m_tuneInhibitBandId >= 0)
        restoreTuneInhibit();

    m_txRequested = false;
    m_cwKeyActive = false;
    m_cwxActive = false;
    m_cwxDrainArmed = false;
    // Reset the CWX drain watch and bump its epoch so a watch armed mid-macro
    // at disconnect can't wedge the monotonic guard after reconnect. (#3949)
    m_cwxModel.resetDrainWatch();
    m_lastInterlockSource.clear();
    m_lastInterlockNotificationKey.clear();
    m_lastInterlockNotificationMs = 0;
    m_interlockNotificationArmedUntilMs = 0;
    m_pendingTransmitPreflightSource = TransmitModel::PttSource::Mox;
    m_interlockNotificationSource = TransmitModel::PttSource::Mox;
    m_digitalVoiceTxSliceId = -1;
    m_lastDigitalVoiceTxSelectionKey.clear();
    if (m_txAudioGate) {
        m_txAudioGate = false;
        emit txAudioGateChanged(false);
    }
    m_radioTransmitting = false;
    emit radioTransmittingChanged(false);
    if (m_profileDatabaseImporting) {
        m_profileDatabaseImporting = false;
        emit profileDatabaseImportingChanged(false);
    }
    if (m_profileDatabaseExporting) {
        m_profileDatabaseExporting = false;
        emit profileDatabaseExportingChanged(false);
    }
    m_transmitModel.setTransmitting(false);
    m_transmitModel.resetState();
    m_meterModel.clear();
    m_daxStreamDebug.clear();
    m_daxTxStreamId = 0;
    m_daxTxActive = false;
    m_daxTxClientHandle = 0;
    m_daxTxCreatePending = false;
    m_wsprTxOwnershipRequested = false;
    m_wsprTxYieldAfterUse = false;
    m_wsprTxReleaseWhenReady = false;
    // The radio is gone — there is nothing left to hand `transmit dax` back to,
    // and the next connect re-reads it from status.
    m_wsprTxRestoreDax = false;
    m_wsprTxPreviousDax = false;
    m_wsprTxSeamAudioArmed = false;
    m_deadDaxRxSeen.clear();
    m_externalDaxTxSeen.clear();
    m_externalDaxRxSeen.clear();
    m_nudgedDaxStreams.clear();  // re-arm the #1439 nudge one-shot on reconnect (#4383)

    // Reset radio-model-specific state — different radios have different
    // capabilities (APD, max power, pan count, TGXL, amplifier, XVTR, etc.)
    // Must re-derive everything from the new radio's status on next connect. (#359)
    m_tunerModel.setHandle({});       // clear TGXL presence
    m_xvtrList.clear();
    m_amplifier.reset();              // clear PGXL presence/operate (#4094)
    if (m_flexBackend) m_flexBackend->clearExtensionHandles();  // drop cached encode handles (#4198)
    m_fullDuplex = false;
    // Reset to false so the next connect's skip-peek fast path requires the
    // radio's mf_enable status to actually arrive before treating multiFLEX
    // as enabled. Default-true would silently bypass the conflict check if
    // the status burst hadn't been processed yet (#3391 review).
    m_multiFlexEnabled = false;
    // Keep m_maxSlices at the last radio's capacity across disconnect. The DAX
    // combo and the DAX/TCI applet rows follow maxSlices() on
    // connectionStateChanged, and resetting to 4 here shrank a 6700's DAX 5-8
    // to "Off" while disconnected. The next connect re-derives the real value
    // from maxSlicesForModel() (RadioModel.cpp connect path). (#4854 review)
    m_model.clear();
    m_version.clear();
    m_oscState.clear();
    m_oscSetting = QStringLiteral("auto");
    m_oscLocked = false;
    m_extPresent = false;
    m_gpsdoPresent = false;
    m_tcxoPresent = false;
    m_gpsStatus.clear();
    m_gpsTracked = 0;
    m_gpsVisible = 0;
    m_gpsGrid.clear();
    m_gpsAltitude.clear();
    m_gpsLat.clear();
    m_gpsLon.clear();
    m_gpsTime.clear();
    m_gpsSpeed.clear();
    m_gpsTrack.clear();
    m_gpsFreqError.clear();
    m_automationGpsNtpServerAddress.clear();
    emit oscillatorChanged();
    emit gpsStatusChanged(m_gpsStatus, m_gpsTracked, m_gpsVisible,
                          m_gpsGrid, m_gpsAltitude, m_gpsLat, m_gpsLon,
                          m_gpsTime);
    // Cleared beside m_version rather than relying on the next connect to
    // reassign it: this block's contract is that everything here is re-derived
    // from the new radio's status, and a path that reaches a Flex without
    // going through connectToRadio() would otherwise inherit an HL2's word.
    m_versionLabel.clear();
    // Remember which radio the surviving pan/slice models belong to so the
    // next connect can refuse to reclaim them against a different radio.
    // Keep the previous value if this disconnect never learned a serial
    // (e.g. handshake failed before the info reply).
    if (!m_chassisSerial.isEmpty())
        m_staleSessionSerial = m_chassisSerial;
    m_chassisSerial.clear();
    m_callsign.clear();
    // Clear the nickname here too, not just on the connectToRadio() seeding
    // path: connectViaWan() takes no RadioInfo and the LAN auto-reconnect timer
    // calls m_connection->connectToRadio(m_lastInfo) directly, both bypassing
    // RadioModel::connectToRadio(). Clearing on the disconnect side closes all
    // three paths at once, so a reconnect can never show the previous radio's
    // station label while the async info reply is in flight. (#4260 review)
    m_nickname.clear();
    m_region.clear();
    m_rxAudio = {};
    m_netCwStreamId = 0;
    m_netCwIndex = 1;
    m_netCwClock.invalidate();
    m_netCwLastSendMs = -1;
    m_lineoutGain = 50;
    m_headphoneGain = 50;
    // The mutes are the previous radio's state just as much as the gains are.
    // Leaving them set meant a reconnect asserted the OLD radio's mute state
    // until the new one's first `audio` status arrived — the same staleness the
    // nickname clearing above exists to prevent. (#4771)
    m_lineoutMute = false;
    m_headphoneMute = false;
    m_frontSpeakerMute = false;

    stopNetworkMonitor();
    // stop() must run on the network thread (socket lives there). (#561)
    // Guarded: a non-Flex backend (HL2) owns no PanadapterStream.
    if (m_panStream) {
        QMetaObject::invokeMethod(m_panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        m_panStream->clearRegisteredStreams();
    }
    // The radio sends no per-stream "removed" on a hard disconnect, so reset
    // the DAX-IQ stream state + destroy pipes here too (panStream IQ
    // registrations are cleared just above); otherwise stale `exists` makes
    // restoreEnabledChannels() skip persisted channels on reconnect. (#3522)
    m_daxIqModel.handleDisconnect();
    m_pendingPanStatuses.clear();
    m_radioDisplayPans.clear();           // #3856 Layer B — radio re-dumps on reconnect
    m_radioDisplayWaterfalls.clear();

    m_tnfModel.clear();
    m_flexWaveformModel.clear();
    if (!m_licenseFeatures.isEmpty()) {
        m_licenseFeatures.clear();
        emit licenseFeaturesChanged();
    }
    if (!m_memories.isEmpty()) {
        m_memories.clear();
        emit memoriesCleared();
    }
    // The cache is connection-scoped and just went; the bank on disk is not.
    // Write out anything still inside the debounce window before the session
    // that made those edits ends.
    m_localMemories.flush();
    // Then put the bank straight back. Host-side memories are not the radio's
    // to take away — the Memory dialog stays open and fully usable while
    // disconnected (Add, Import and Export are all live there), so leaving the
    // cache empty would show an operator an empty channel list and let them
    // import into a bank they cannot see.
    publishLocalMemories();
    m_clientStations.clear();
    m_clientInfoMap.clear();
    // #3977: handles are radio-boot-scoped and recycled across connections —
    // strikes and eviction marks must not outlive the connection they were
    // observed on, or a recycled handle inherits them (instant eviction of an
    // innocent client / permanent immunity for a real zombie).
    m_foreignPanWrites.clear();
    m_evictedPredecessorHandles.clear();
    m_evictionsInFlight.clear();
    m_announcedClientConnections.clear();
    m_startupClientConnections.clear();
    m_clientConnectionNoticeTimer.invalidate();
    // The radio reaps all our streams on TCP disconnect; drop the DAX channel
    // ownership table without emitting removals (#3305). Consumers re-acquire
    // on their reconnect re-arm paths.
    if (m_panStream)
        m_panStream->resetDaxChannelsForDisconnect();
    emit otherClientsChanged(0, {});
    emit connectionStateChanged(false);
    m_forcedDisconnectInProgress = false;

    if (m_wanConn) {
        qCDebug(lcProtocol) << "RadioModel: WAN disconnected";
        m_wanConn->disconnect(this);
        m_wanConn = nullptr;
    } else if (!m_intentionalDisconnect && !m_lastInfo.address.isNull()) {
        qCDebug(lcProtocol) << "RadioModel: unexpected disconnect — reconnecting in 3s";
        m_reconnectTimer.start();
    }

    // Release memory-slot ownership only when the session is really over. While
    // a reconnect is armed the radio still owns its slots, so the host bank must
    // not start answering writes that reconnect would discard — the phantom
    // channel described on m_sessionRadioOwnsMemories. An intentional disconnect
    // (or no address to return to) genuinely ends the session and hands the bank
    // back, which is the same state the app starts in.
    if (m_intentionalDisconnect || m_lastInfo.address.isNull())
        m_sessionRadioOwnsMemories = false;
}

void RadioModel::onConnectionError(const QString& msg)
{
    qCWarning(lcProtocol) << "RadioModel: connection error:" << msg;
    // The attempt ended (#4912). If the reconnect below arms, its own lambda
    // re-arms the flag when it actually re-drives the connect — so the window
    // between the failure and the retry reads as idle, which is what it is.
    //
    // Deliberately NOT cleared in onDisconnected(): a disconnect emitted while
    // a connect is in flight is part of that connect (a family switch tears the
    // old backend down mid-connectToRadio), and clearing there would report
    // "idle" for a connect that is still very much running.
    m_connectAttemptActive = false;
    if (!m_rebootInProgress) {
        emit connectionError(msg);
    }
    // A refused connect may never emit disconnected, but the radio can recover
    // after expiring a stale session. Keep retrying the same discovered radio.
    if (!m_wanConn && !m_intentionalDisconnect && !m_lastInfo.address.isNull()
            && !m_reconnectTimer.isActive()) {
        qCDebug(lcProtocol) << "RadioModel: connection error — reconnecting in 5s";
        m_reconnectTimer.start();
    }
}

void RadioModel::onVersionReceived(const QString& v)
{
    // The V line from the radio is the protocol version (e.g. "1.4.0.0").
    // We prefer the software version from discovery (e.g. "4.1.5").
    // Only use protocol version as fallback if discovery didn't provide one.
    if (m_version.isEmpty())
        m_version = v;
    m_protocolVersion = v;
    emit infoChanged();
}

// ─── Network quality monitor ─────────────────────────────────────────────────

namespace {
// The loss window and every packet-count getter are int, matching the Flex
// stream's own counters. A backend reports quint64, and an HL2 at 384 kHz puts
// ~3000 EP6 packets a second on the wire — about eight days to reach INT_MAX.
// CLAMP rather than let it wrap: a wrapped negative count turns the loss
// percentage into nonsense and would drive the adaptive throttle off it. At the
// ceiling the deltas simply go to zero, so the readout freezes instead of lying.
int saturatingInt(quint64 v)
{
    constexpr quint64 kMax = static_cast<quint64>(std::numeric_limits<int>::max());
    return static_cast<int>(std::min(v, kMax));
}
}  // namespace

void RadioModel::resetNetworkQualitySession()
{
    // Everything a new scoring session must forget, in ONE place. Two paths
    // start a session — startNetworkMonitor() for the Flex ping timer, and the
    // first backend LinkStats snapshot for a family that has no command plane —
    // and when this was open-coded in both they drifted: the backend path kept
    // the previous session's m_lastPingRtt, which evaluateNetworkQuality() then
    // scored the new link with. A stale WAN RTT against LAN_PING_POOR_MS caps
    // the target score at 45 and engages the adaptive frame-rate throttle on a
    // radio that is streaming perfectly, and hasLinkRtt() hides the number, so
    // nothing on screen contradicts it.
    m_netState = NetState::Excellent;
    m_networkQualityScore = 100.0;
    m_lastPingRtt = 0;
    m_maxPingRtt = 0;
    // Pure state, so it is safe on both paths. The MainWindow-side clear that
    // startNetworkMonitor() emits below is deliberately NOT here: MainWindow
    // already drops m_adaptiveThrottleActive on the disconnect edge, and the
    // handler's !active branch re-asserts every pan's display rate — which on
    // the backend path would fire against LIVE pans (they are published a
    // second before the first link-stats tick), making the client re-assert
    // state the radio owns. Principles II/III.
    m_pendingThrottleLift = false;
    resetNetworkHealthSamples();
}

void RadioModel::startNetworkMonitor()
{
    m_pingTimer.stop();
    m_pingTimer.disconnect();
    resetNetworkQualitySession();
    m_pingMissCount = 0;
    m_pingDisconnectTriggered = false;
    m_lastMultiFlexClientConnectMs = 0;
    m_multiFlexPingGraceUntilMs = 0;
    // Safety: ensure MainWindow's m_adaptiveThrottleActive is cleared even if
    // the connectionStateChanged(false) path was somehow skipped.  Pans are not
    // yet rebuilt at this point so the fps-restore loop in the handler is a no-op.
    emit adaptiveThrottleChanged(false, 0);

    // RTT is read from kernel TCP_INFO (smoothed RTT from TCP ACK timing),
    // completely independent of Qt event loop buffering. Falls back to
    // QElapsedTimer stopwatch if the platform kernel call is unavailable.
    if (m_networkPingConnection) {
        disconnect(m_networkPingConnection);
        m_networkPingConnection = {};
    }
    m_networkPingConnection = connect(m_connection, &RadioConnection::pingRttMeasured, this, [this](int ms) {
        m_pingMissCount = 0;
        m_lastPingRtt = ms;
        evaluateNetworkQuality();
        emit pingReceived();
    });

    connect(&m_pingTimer, &QTimer::timeout, this, [this]() {
        if (!isConnected()) {
            stopNetworkMonitor();
            return;
        }
        ++m_pingMissCount;
        const int missThreshold = (m_netState == NetState::Poor)
                                      ? PING_MISS_DISCONNECT_POOR
                                      : PING_MISS_DISCONNECT;
        if (m_pingMissCount >= missThreshold) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now < m_multiFlexPingGraceUntilMs) {
                qCDebug(lcProtocol) << "RadioModel: deferring ping disconnect during Multi-Flex client-connect grace"
                                    << "misses:" << m_pingMissCount
                                    << "remaining_ms:" << (m_multiFlexPingGraceUntilMs - now);
                sendCmd("ping");
                return;
            }

            // A new Multi-Flex GUI can briefly starve TCP ping replies while
            // the radio replays status and stream ownership. If that burst
            // overlaps the missed-ping window, grant one short grace period
            // before treating the TCP control path as dead.
            const qint64 recentClientConnectWindowMs =
                static_cast<qint64>(missThreshold + 1) * m_pingTimer.interval();
            const bool recentMultiFlexClientConnect =
                m_lastMultiFlexClientConnectMs > 0
                && now >= m_lastMultiFlexClientConnectMs
                && now - m_lastMultiFlexClientConnectMs <= recentClientConnectWindowMs;
            if (recentMultiFlexClientConnect) {
                m_multiFlexPingGraceUntilMs = now + MULTIFLEX_CLIENT_CONNECT_PING_GRACE_MS;
                qCDebug(lcProtocol) << "RadioModel: deferring ping disconnect after recent Multi-Flex client connect"
                                    << "misses:" << m_pingMissCount
                                    << "since_client_connect_ms:" << (now - m_lastMultiFlexClientConnectMs)
                                    << "grace_ms:" << MULTIFLEX_CLIENT_CONNECT_PING_GRACE_MS;
                sendCmd("ping");
                return;
            }

            if (m_pingDisconnectTriggered) {
                return;
            }

            m_pingDisconnectTriggered = true;
            m_pingTimer.stop();
            qCDebug(lcProtocol) << "RadioModel:" << missThreshold
                                << "consecutive pings unanswered — forcing disconnect"
                                << "(state:" << static_cast<int>(m_netState) << ")";
            forceDisconnect();
            return;
        }
        sendCmd("ping");  // RTT measured by RadioConnection::pingRttMeasured
    });
    m_pingTimer.start(1000);
}

void RadioModel::stopNetworkMonitor()
{
    m_pingTimer.stop();
    m_pingTimer.disconnect();
    if (m_networkPingConnection) {
        disconnect(m_networkPingConnection);
        m_networkPingConnection = {};
    }
    m_netState = NetState::Off;
    // Drop the backend transport snapshot on the same edge. Its counters belong
    // to the session that just ended; carrying them into the next one would show
    // the previous link's byte totals against a radio that has sent nothing.
    // Clearing `reported` also puts every getter back on its Flex branch, so a
    // disconnected model answers exactly what it did before this existed.
    m_linkStats = {};

    // ANNOUNCE the reset. m_netState going to Off is not observable on its own:
    // the status-bar field is written only from this signal, and every emitter
    // of it hangs off the ping/transport path that has just been torn down. So
    // the last quality the link ever had stayed on screen after disconnect —
    // a disconnected radio reading "Excellent" — until the next connection
    // happened to overwrite it. Pre-existing on the Flex path too, and fixed
    // for both here rather than only where it was noticed.
    emit networkQualityChanged(networkQuality(), 0);
}

void RadioModel::evaluateNetworkQuality()
{
    // Two sources, one scorer. The Flex VITA-49 stream is consulted first and
    // its behavior is untouched; a backend that reports its own transport is
    // scored by exactly the same thresholds, because "the link is Fair" has to
    // mean the same thing to the operator on either radio.
    int currentErrors = 0;
    int currentPackets = 0;
    if (m_panStream) {
        currentErrors = m_panStream->packetErrorCount();
        currentPackets = m_panStream->packetTotalCount();
    } else if (m_linkStats.reported) {
        currentErrors = saturatingInt(m_linkStats.rxPacketsLost);
        currentPackets = saturatingInt(m_linkStats.rxPackets);
    } else {
        return;   // nothing measures this transport
    }
    recordNetworkHealthSample(currentErrors, currentPackets);
    const int ping = m_lastPingRtt;

    const double targetScore = networkQualityTargetScore(ping);
    const double alpha = targetScore < m_networkQualityScore
                             ? (targetScore <= 45.0 ? 0.45 : 0.30)
                             : 0.12;
    m_networkQualityScore += (targetScore - m_networkQualityScore) * alpha;
    const NetState prevState = m_netState;
    m_netState = networkStateForScore(m_networkQualityScore, m_netState);
    if (ping > m_maxPingRtt) m_maxPingRtt = ping;

    if (m_netState != prevState)
        applyAdaptiveFrameRate(m_netState, prevState);

    // Fire a deferred throttle lift once the min-dwell has elapsed and the
    // state has not re-entered a throttled tier since the engage.
    if (m_pendingThrottleLift
            && m_netState != NetState::Good
            && m_netState != NetState::Fair
            && m_netState != NetState::Poor) {
        if (QDateTime::currentMSecsSinceEpoch() - m_lastThrottleEngageMs >= THROTTLE_MIN_DWELL_MS) {
            m_pendingThrottleLift = false;
            qCDebug(lcProtocol) << "RadioModel: deferred adaptive throttle lift firing after min-dwell";
            emit adaptiveThrottleChanged(false, 0);
        }
    }

    static const char* names[] = {"Off", "Excellent", "Very Good", "Good", "Fair", "Poor"};
    emit networkQualityChanged(names[static_cast<int>(m_netState)], ping);
}

void RadioModel::resetNetworkHealthSamples()
{
    if (m_panStream) {
        m_lastErrorCount = m_panStream->packetErrorCount();
        m_lastPacketCount = m_panStream->packetTotalCount();
    } else if (m_linkStats.reported) {
        m_lastErrorCount = saturatingInt(m_linkStats.rxPacketsLost);
        m_lastPacketCount = saturatingInt(m_linkStats.rxPackets);
    } else {
        m_lastErrorCount = 0;
        m_lastPacketCount = 0;
    }
    for (int i = 0; i < NETWORK_LOSS_WINDOW_SAMPLES; ++i) {
        m_lossSamplePackets[i] = 0;
        m_lossSampleErrors[i] = 0;
    }
    m_lossSampleCursor = 0;
    m_lossSampleCount = 0;
    m_packetLossWindowPackets = 0;
    m_packetLossWindowErrors = 0;
}

void RadioModel::recordNetworkHealthSample(int currentErrors, int currentPackets)
{
    const int deltaErrors = std::max(0, currentErrors - m_lastErrorCount);
    const int deltaPackets = std::max(0, currentPackets - m_lastPacketCount);
    m_lastErrorCount = currentErrors;
    m_lastPacketCount = currentPackets;

    if (m_lossSampleCount < NETWORK_LOSS_WINDOW_SAMPLES) {
        ++m_lossSampleCount;
    } else {
        m_packetLossWindowPackets -= m_lossSamplePackets[m_lossSampleCursor];
        m_packetLossWindowErrors -= m_lossSampleErrors[m_lossSampleCursor];
    }

    m_lossSamplePackets[m_lossSampleCursor] = deltaPackets;
    m_lossSampleErrors[m_lossSampleCursor] = deltaErrors;
    m_packetLossWindowPackets += deltaPackets;
    m_packetLossWindowErrors += deltaErrors;
    m_lossSampleCursor = (m_lossSampleCursor + 1) % NETWORK_LOSS_WINDOW_SAMPLES;
}

double RadioModel::networkQualityTargetScore(int pingMs) const
{
    const bool remote = usesRemoteNetworkThresholds();
    const int fairPingMs = remote ? REMOTE_PING_FAIR_MS : LAN_PING_FAIR_MS;
    const int poorPingMs = remote ? REMOTE_PING_POOR_MS : LAN_PING_POOR_MS;
    const int goodJitterMs = remote ? 45 : 20;
    const int fairJitterMs = remote ? 90 : 45;
    const int poorJitterMs = remote ? 150 : 90;

    double score = 100.0;
    if (pingMs >= poorPingMs) {
        score = std::min(score, 45.0);
    } else if (pingMs >= fairPingMs) {
        score = std::min(score, 70.0);
    } else if (pingMs >= fairPingMs * 2 / 3) {
        score = std::min(score, 84.0);
    }

    if (m_packetLossWindowPackets >= NETWORK_MIN_LOSS_WINDOW_PACKETS) {
        const double lossPct = packetLossPercent();
        if (lossPct >= 3.0) {
            score = std::min(score, 35.0);
        } else if (lossPct >= 1.0) {
            score = std::min(score, 52.0);
        } else if (lossPct >= 0.35) {
            score = std::min(score, 70.0);
        } else if (lossPct >= 0.05) {
            score = std::min(score, 84.0);
        }
    }

    // Skipped rather than scored as 0 when the transport has not produced a
    // delivery-timing window yet: the getter clamps the seam's "not measured"
    // sentinel to 0 for the charts, and a 0 here is the best possible jitter —
    // a free pass on the first tick of every backend session.
    if (hasLinkTiming()) {
        const int jitterMs = audioPacketJitterMs();
        if (jitterMs >= poorJitterMs) {
            score = std::min(score, 42.0);
        } else if (jitterMs >= fairJitterMs) {
            score = std::min(score, 58.0);
        } else if (jitterMs >= goodJitterMs) {
            score = std::min(score, 74.0);
        }
    }

    return score;
}

RadioModel::NetState RadioModel::networkStateForScore(double score, NetState currentState) const
{
    switch (currentState) {
    case NetState::Excellent:
        return score < 89.0 ? NetState::VeryGood : NetState::Excellent;
    case NetState::VeryGood:
        if (score >= 94.0)
            return NetState::Excellent;
        if (score < 76.0)
            return NetState::Good;
        return NetState::VeryGood;
    case NetState::Good:
        if (score >= 83.0)
            return NetState::VeryGood;
        if (score < 60.0)
            return NetState::Fair;
        return NetState::Good;
    case NetState::Fair:
        if (score >= 68.0)
            return NetState::Good;
        if (score < 40.0)
            return NetState::Poor;
        return NetState::Fair;
    case NetState::Poor:
        return score >= 50.0 ? NetState::Fair : NetState::Poor;
    case NetState::Off:
        break;
    }

    if (score >= 92.0)
        return NetState::Excellent;
    if (score >= 80.0)
        return NetState::VeryGood;
    if (score >= 65.0)
        return NetState::Good;
    if (score >= 45.0)
        return NetState::Fair;
    return NetState::Poor;
}

// Single source of truth for the state→fps-cap mapping.
// currentAdaptiveFpsCap(), applyAdaptiveFrameRate(), and any future
// callers must all go through here so adding a new tier (e.g. Critical=2)
// never silently diverges between code paths.
int RadioModel::fpsCapForState(NetState s)
{
    switch (s) {
    case NetState::Poor: return 4;
    case NetState::Fair: return 8;
    case NetState::Good: return 15;
    default:             return 0;
    }
}

int RadioModel::currentAdaptiveFpsCap() const
{
    if (AppSettings::instance().value("AdaptiveThrottleEnabled", "False").toString() != "True")
        return 0;
    return fpsCapForState(m_netState);
}


int RadioModel::adaptiveWfRateForCap(int fpsCap) const
{
    if (fpsCap <= 0) return 0;
    // The caller knows a frame cap; the control speaks the 1..100 rate. Going
    // straight from 1000/fps to the wire treated the rate as milliseconds and
    // inverted the throttle: a Poor-network 4 fps cap produced 250, clamped to
    // 100 — the FASTEST setting — while a mild Good-network 15 fps cap produced
    // 67, about 4.5 rows/s. Worse network, faster waterfall. (#4606)
    //
    // Which law converts depends on who turns the rate into rows: ask a Flex in
    // the units its display engine answers in, and this host in ours.
    if (shapesDisplayRatesLocally()) {
        return AetherSDR::WaterfallRate::localRateForRowsPerSec(
            static_cast<float>(fpsCap));
    }
    return AetherSDR::WaterfallRate::flexRateForMsPerRow(
        1000.0f / static_cast<float>(fpsCap));
}

void RadioModel::sendAdaptiveCapToPan(const QString& panId, int fpsCap)
{
    if (panId.isEmpty() || fpsCap <= 0) return;
    if (profileLoadRadioStateWritesHeld()) return;
    auto* pan = m_panadapters.value(panId, nullptr);
    if (!pan) return;
    // A backend that shapes its own display rate has no Flex command sink, so the
    // wire text below reached nothing and the congestion cap simply never applied
    // to it — on the one backend where the frame cost is paid by THIS host.
    // Route it the same way an operator's slider goes. (#4470)
    if (shapesDisplayRatesLocally()) {
        requestPanDisplayRates(panId, fpsCap, adaptiveWfRateForCap(fpsCap));
        return;
    }
    sendCommand(QString("display pan set %1 fps=%2").arg(panId).arg(fpsCap));
    if (!pan->waterfallId().isEmpty())
        sendCommand(QString("display panafall set %1 line_duration=%2")
                        .arg(pan->waterfallId()).arg(adaptiveWfRateForCap(fpsCap)));
}

void RadioModel::applyAdaptiveFrameRate(NetState newState, NetState oldState)
{
    if (AppSettings::instance().value("AdaptiveThrottleEnabled", "False").toString() != "True")
        return;
    const int newCap = fpsCapForState(newState);
    const int oldCap = fpsCapForState(oldState);
    if (newCap == oldCap)
        return;

    const bool throttling = (newCap > 0);

    if (throttling) {
        m_lastThrottleEngageMs = QDateTime::currentMSecsSinceEpoch();
        m_pendingThrottleLift = false;
        qCDebug(lcProtocol) << "RadioModel: adaptive throttle engaged — fps cap"
                            << newCap << "/ wf rate" << adaptiveWfRateForCap(newCap);
        for (auto it = m_panadapters.cbegin(); it != m_panadapters.cend(); ++it)
            sendAdaptiveCapToPan(it.key(), newCap);
        emit adaptiveThrottleChanged(throttling, newCap);
    } else {
        // Min-dwell guard: if we just engaged, don't lift yet — let the link
        // stabilise before restoring full fps. evaluateNetworkQuality() will
        // fire the deferred lift once THROTTLE_MIN_DWELL_MS has elapsed.
        if (QDateTime::currentMSecsSinceEpoch() - m_lastThrottleEngageMs < THROTTLE_MIN_DWELL_MS) {
            m_pendingThrottleLift = true;
            qCDebug(lcProtocol) << "RadioModel: adaptive throttle lift deferred"
                                << "(min-dwell not reached)";
            return;
        }
        m_pendingThrottleLift = false;
        qCDebug(lcProtocol) << "RadioModel: adaptive throttle lifted — signalling fps restore";
        // Intentionally no fps push here — RadioModel doesn't own the user-configured
        // fps (that lives in each SpectrumWidget). MainWindow restores it via
        // adaptiveThrottleChanged(false, 0). A headless consumer connecting to
        // RadioModel without MainWindow receives the engage but must handle restore itself.
        emit adaptiveThrottleChanged(false, 0);
    }
}

bool RadioModel::usesRemoteNetworkThresholds() const
{
    return m_wanConn != nullptr || m_lastInfo.isRouted;
}

QString RadioModel::networkQuality() const
{
    static const char* names[] = {"Off", "Excellent", "Very Good", "Good", "Fair", "Poor"};
    return names[static_cast<int>(m_netState)];
}

double RadioModel::packetLossPercent() const
{
    if (m_packetLossWindowPackets <= 0)
        return 0.0;
    return (m_packetLossWindowErrors * 100.0) / m_packetLossWindowPackets;
}

// Each of the six below reads the Flex VITA-49 stream when there is one and the
// backend's own transport counters when there is not. The Flex branch is first
// and unchanged in every case; usesBackendLinkStats() is false unless a backend
// has actually reported, so a family that measures nothing still answers the
// same zeros it always did.
int RadioModel::audioPacketGapMs() const
{
    if (m_panStream)
        return m_panStream->audioPacketGapMs();
    return usesBackendLinkStats() ? std::max(0, m_linkStats.gapMs) : 0;
}

int RadioModel::audioPacketGapMaxMs() const
{
    if (m_panStream)
        return m_panStream->audioPacketGapMaxMs();
    return usesBackendLinkStats() ? std::max(0, m_linkStats.gapMaxMs) : 0;
}

int RadioModel::audioPacketJitterMs() const
{
    if (m_panStream)
        return m_panStream->audioPacketJitterMs();
    return usesBackendLinkStats() ? std::max(0, m_linkStats.jitterMs) : 0;
}

int RadioModel::packetDropCount() const
{
    if (m_panStream)
        return m_panStream->packetErrorCount();
    return usesBackendLinkStats() ? saturatingInt(m_linkStats.rxPacketsLost) : 0;
}

int RadioModel::packetTotalCount() const
{
    if (m_panStream)
        return m_panStream->packetTotalCount();
    return usesBackendLinkStats() ? saturatingInt(m_linkStats.rxPackets) : 0;
}

qint64 RadioModel::rxBytes() const
{
    if (m_panStream)
        return m_panStream->totalRxBytes();
    return usesBackendLinkStats() ? m_linkStats.rxBytes : 0;
}

qint64 RadioModel::txBytes() const
{
    if (m_panStream)
        return m_panStream->totalTxBytes();
    return usesBackendLinkStats() ? m_linkStats.txBytes : 0;
}

QString RadioModel::targetRadioIp() const
{
    return m_lastInfo.address.toString();
}

QString RadioModel::selectedSourceMode() const
{
    return m_lastInfo.bindSettings.modeString();
}

QString RadioModel::selectedSourcePath() const
{
    if (m_lastInfo.bindSettings.mode == RadioBindMode::Explicit)
        return m_lastInfo.bindSettings.selectionLabel();

    QHostAddress resolved = m_lastInfo.sessionBindAddress;
    if (resolved.isNull() && m_connection)
        resolved = m_connection->localAddress();
    if (!resolved.isNull() && resolved.protocol() == QAbstractSocket::IPv4Protocol)
        return QStringLiteral("Auto (%1)").arg(resolved.toString());
    return QStringLiteral("Auto");
}

QString RadioModel::localTcpEndpoint() const
{
    if (m_wanConn)
        return QStringLiteral("SmartLink/WAN");

    if (!m_connection) {
        // "Not connected" is true for a family that has no TCP command plane at
        // all, and reads as a fault when the radio is streaming perfectly well.
        // Say which it is.
        if (usesBackendLinkStats())
            return QStringLiteral("None (stream transport only)");
        return QStringLiteral("Not connected");
    }
    const QHostAddress localAddr = m_connection->localAddress();
    const quint16 localPort = m_connection->localTcpPort();
    if (localAddr.isNull() || localPort == 0)
        return QStringLiteral("Not connected");
    return QStringLiteral("%1:%2").arg(localAddr.toString()).arg(localPort);
}

QString RadioModel::localUdpEndpoint() const
{
    if (!m_panStream) {
        if (usesBackendLinkStats() && !m_linkStats.localEndpoint.isEmpty())
            return m_linkStats.localEndpoint;
        return QStringLiteral("Not bound");
    }
    const QHostAddress localAddr = m_panStream->localAddress();
    const quint16 localPort = m_panStream->localPort();
    if (localAddr.isNull() || localPort == 0)
        return QStringLiteral("Not bound");
    return QStringLiteral("%1:%2").arg(localAddr.toString()).arg(localPort);
}

bool RadioModel::firstUdpPacketSeen() const
{
    if (m_panStream)
        return m_panStream->hasReceivedPackets();
    return usesBackendLinkStats() && m_linkStats.rxPackets > 0;
}

PanadapterStream::CategoryStats RadioModel::categoryStats(PanadapterStream::StreamCategory cat) const
{
    if (!m_panStream)
        return {};
    return m_panStream->categoryStats(cat);
}

QVector<PanadapterStream::AudioStreamDiagnostics> RadioModel::audioStreamDiagnostics() const
{
    return m_panStream ? m_panStream->audioStreamDiagnostics()
                       : QVector<PanadapterStream::AudioStreamDiagnostics>{};
}

void RadioModel::resetAudioStreamDiagnostics()
{
    if (m_panStream) {
        QMetaObject::invokeMethod(m_panStream,
                                  &PanadapterStream::resetAudioStreamDiagnostics,
                                  Qt::AutoConnection);
    }
}

void RadioModel::handleMemoryStatus(int index, const QMap<QString, QString>& kvs)
{
    // aetherd RFC 2.3 (RadioModel residual): the Flex memory-slot wire decode
    // moved to FlexBackend::decodeMemoryStatus → memoryChanged → applyMemoryChanges
    // (the model-side MemoryEntry update, text sanitisation, and emits). Thin
    // forwarder behind the seam.
    if (m_flexBackend) {
        m_flexBackend->decodeMemoryStatus(index, kvs);
        return;
    }
    // No Flex backend means no decoder on the other side of the seam, and this
    // used to be a silent no-op. The memory dialog calls it after every
    // successful write to fold the values it just sent into the cache, so on a
    // locally-banked radio dropping it left the UI showing a channel that never
    // updated. Same decoder, same delta path.
    if (usesLocalMemoryBank())
        applyMemoryChanges(MemoryWire::decodeStatus(index, kvs));
}

bool RadioModel::usesLocalMemoryBank() const
{
    // While nothing is connected, no radio owns the slots, so the host bank
    // does. This is not just tidiness: a backend object exists from startup and
    // defaults to the Flex family, so keying off capabilities alone would call
    // the bank unused before the operator has connected to anything — and the
    // Memory dialog is fully usable right there, with Add, Import and Export
    // all live. Connecting to a radio that does own its slots hands ownership
    // back; see syncMemoryStoreForSession().
    // Disconnected: the host bank owns the slots UNLESS this session's radio
    // does and is expected back. Keying on isConnected() alone handed ownership
    // to the bank during any link blip — and a Flex drop does not clear
    // m_slices, so the Memory dialog's Add stayed live, was answered by the
    // bank, reported success, and was then wiped by
    // syncMemoryStoreForSession() on reconnect. The channel never reached the
    // radio and left a stray slot behind that resurfaced on every future
    // disconnect.
    if (!isConnected())
        return !m_sessionRadioOwnsMemories;
    return !backendCapabilities().persistsMemories;
}

std::optional<quint32> RadioModel::tryLocalMemoryCommand(
    const QString& command, const RadioConnection::ResponseCallback& cb)
{
    if (!usesLocalMemoryBank())
        return std::nullopt;
    if (!command.startsWith(QLatin1String("memory ")))
        return std::nullopt;

    const LocalMemoryBank::CommandResult result = m_localMemories.handleCommand(command);
    if (!result.handled)
        return std::nullopt;

    if (result.delta) {
        applyMemoryChanges(*result.delta);
        // applyMemoryChanges owns the space-decode and sanitisation, so read the
        // slot back out of the cache to persist: the file then holds exactly
        // what the UI and a CSV export see, not a second interpretation of the
        // same kv-set.
        if (result.delta->removed) {
            m_localMemories.forget(result.delta->index);
        } else if (const auto it = m_memories.constFind(result.delta->index);
                   it != m_memories.constEnd()) {
            m_localMemories.record(result.delta->index, it.value());
        }
    }

    if (result.recallIndex >= 0)
        recallLocalMemory(result.recallIndex);

    const quint32 seq = m_seqCounter.fetch_add(1);
    if (cb) {
        // Queued, never re-entrant. On the wire a response always arrives on a
        // later turn of the event loop, and the CSV import chains its next
        // record from inside this callback — answering inline would recurse two
        // frames per imported channel and blow the stack on a large file.
        QMetaObject::invokeMethod(this, [cb, code = result.code, body = result.body]() {
            cb(code, body);
        }, Qt::QueuedConnection);
    }
    return seq;
}

void RadioModel::syncMemoryStoreForSession()
{
    // Latch who owns the slots for THIS session, on the connect edge, before the
    // question is asked below. Held across an unexpected drop so a blip cannot
    // silently move ownership to the host bank mid-session.
    m_sessionRadioOwnsMemories = isConnected() && backendCapabilities().persistsMemories;

    if (usesLocalMemoryBank()) {
        publishLocalMemories();
        return;
    }
    // This radio owns its memory slots and is about to dump them. Anything the
    // local bank published while we were disconnected has to go first: both
    // number their slots from 0, so a leftover local slot 0 would sit in the
    // cache pretending to be the radio's slot 0 until the dump overwrote it —
    // and any local slot the radio doesn't have would never be overwritten at
    // all.
    if (!m_memories.isEmpty()) {
        m_memories.clear();
        emit memoriesCleared();
    }
}

void RadioModel::publishLocalMemories()
{
    if (!usesLocalMemoryBank())
        return;

    m_localMemories.load();
    const QMap<int, MemoryEntry>& stored = m_localMemories.entries();
    if (stored.isEmpty())
        return;

    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it) {
        MemoryEntry entry = it.value();
        entry.index = it.key();
        m_memories.insert(it.key(), entry);
        emit memoryChanged(it.key());
    }
    qCInfo(lcProtocol).noquote()
        << "RadioModel: published" << stored.size() << "memories from the local bank";
}

void RadioModel::recallLocalMemory(int index)
{
    const auto it = m_memories.constFind(index);
    if (it == m_memories.constEnd()) {
        qCWarning(lcProtocol) << "RadioModel: local memory recall for unknown slot" << index;
        return;
    }
    // `memory apply` lands on the ACTIVE slice on a Flex, so resolve the same
    // one here. MainWindow has already made its recall target active by the
    // time this runs. The first slice is the fallback for a single-slice
    // backend that never marks one active — better than dropping the recall.
    SliceModel* target = nullptr;
    for (SliceModel* s : m_slices) {
        if (s && s->isActive()) {
            target = s;
            break;
        }
    }
    if (!target && !m_slices.isEmpty())
        target = m_slices.first();
    if (!target) {
        qCWarning(lcProtocol) << "RadioModel: local memory recall with no slice to apply it to";
        return;
    }

    const MemoryEntry& memory = it.value();

    // These are the operator-issue setters, the same ones the panel controls
    // call, so each emits its *CommandIssued signal and reaches the radio
    // through the backend seam. Order matches what `memory apply` does on a
    // Flex: mode first (it resets the filter to the mode default), then the
    // stored filter, then frequency.
    if (!memory.mode.isEmpty())
        target->setMode(memory.mode);
    if (memory.rxFilterLow != 0 || memory.rxFilterHigh != 0)
        target->setFilterWidth(memory.rxFilterLow, memory.rxFilterHigh);

    // FM repeater and tone fields have no seam route today — a non-Flex backend
    // never sees SliceModel's Flex wire text for them. Applying them anyway
    // keeps the model and the UI honest about what the recalled channel is; on
    // a backend that grows FM support they will already be set.
    if (!memory.offsetDir.isEmpty()) {
        target->setRepeaterOffsetDir(memory.offsetDir);
        target->setFmRepeaterOffsetFreq(std::abs(memory.repeaterOffset));
    }
    if (!memory.toneMode.isEmpty()) {
        target->setFmToneMode(memory.toneMode);
        target->setFmToneValue(QString::number(memory.toneValue, 'f', 1));
    }
    target->setSquelch(memory.squelch, memory.squelchLevel);

    // The tuning step, applied directly rather than commanded. On a Flex the
    // panel's follow-up `slice set N step=` round-trips through the radio and
    // comes back as status; on a host-bank backend that command has no command
    // plane to travel on and is dropped, so nothing was setting the step and a
    // recalled channel tuned in whatever increment happened to be current.
    if (memory.step > 0)
        target->applyRecalledStepHz(memory.step);

    if (memory.freq > 0.0)
        target->setFrequency(memory.freq);

    qCInfo(lcProtocol).noquote().nospace()
        << "RadioModel: recalled local memory " << index
        << " onto slice " << target->sliceId()
        << " freq=" << QString::number(memory.freq, 'f', 6)
        << " mode=" << memory.mode;
}

void RadioModel::applyMemoryChanges(const MemoryDelta& d)
{
    if (d.removed) {
        m_memories.remove(d.index);
        emit memoryRemoved(d.index);
        return;
    }

    auto& m = m_memories[d.index];
    m.index = d.index;

    // Decode the protocol space-encoding (0x7f -> ' ') for free-text fields,
    // then strip any NUL/control bytes so corrupt values from the radio (or a
    // previously corrupted memory) never reach the UI, CSV export, or a re-send.
    // This sanitisation stays model-side (MemoryFields is a models/ concern); the
    // backend carries the text raw. Present-only: absent keys keep the prior value.
    auto decodeText = [](const QString& v) {
        return AetherSDR::MemoryFields::sanitizeText(QString(v).replace('\x7f', ' '));
    };
    auto sanitize = [](const QString& v) {
        return AetherSDR::MemoryFields::sanitizeText(v);
    };

    if (d.group)          m.group          = decodeText(*d.group);
    if (d.owner)          m.owner          = decodeText(*d.owner);
    if (d.name)           m.name           = decodeText(*d.name);
    if (d.mode)           m.mode           = sanitize(*d.mode);
    if (d.offsetDir)      m.offsetDir      = sanitize(*d.offsetDir);
    if (d.toneMode)       m.toneMode       = sanitize(*d.toneMode);
    if (d.freq)           m.freq           = *d.freq;
    if (d.repeaterOffset) m.repeaterOffset = *d.repeaterOffset;
    if (d.toneValue)      m.toneValue      = *d.toneValue;
    if (d.step)           m.step           = *d.step;
    if (d.squelch)        m.squelch        = *d.squelch;
    if (d.squelchLevel)   m.squelchLevel   = *d.squelchLevel;
    if (d.rxFilterLow)    m.rxFilterLow    = *d.rxFilterLow;
    if (d.rxFilterHigh)   m.rxFilterHigh   = *d.rxFilterHigh;
    if (d.rttyMark)       m.rttyMark       = *d.rttyMark;
    if (d.rttyShift)      m.rttyShift      = *d.rttyShift;
    if (d.diglOffset)     m.diglOffset     = *d.diglOffset;
    if (d.diguOffset)     m.diguOffset     = *d.diguOffset;

    emit memoryChanged(d.index);
}

// ─── Raw message handler (for meter status with '#' separators) ──────────────

void RadioModel::onMessageReceived(const ParsedMessage& msg)
{
    if (msg.type == MessageType::Handle) {
        // The radio can send routine "Client connected from IP ..." M-messages
        // immediately after H<handle>, before onConnected() is delivered to
        // this object. Arm the startup gate here so our own connect notice stays
        // silent even on that ordering.
        armClientConnectionNoticeSuppression();
        return;
    }

    if (msg.type == MessageType::Message) {
        m_guiClientRegistrationState.noteRadioMessage(msg.object, msg.severity);
        if (shouldSuppressRadioMessageNotice(msg.object, msg.severity)) {
            qCInfo(lcProtocol) << "Radio M-message [Info suppressed during connect]:" << msg.object;
            return;
        }

        // Log everything to the protocol channel at the matching level so the
        // diagnostic trail is uniform.  The user-facing decision (silent log,
        // warning dialog, error dialog) is made in MainWindow::onRadioMessage
        // based on the same severity, so the two paths can't disagree.
        switch (msg.severity) {
        case MessageSeverity::Info:
            qCInfo(lcProtocol) << "Radio M-message [Info]:" << msg.object;
            break;
        case MessageSeverity::Warning:
            qCWarning(lcProtocol) << "Radio M-message [Warning]:" << msg.object;
            break;
        case MessageSeverity::Error:
        case MessageSeverity::Fatal:
            qCCritical(lcProtocol) << "Radio M-message [Error/Fatal]:" << msg.object;
            break;
        }
        emit radioMessageReceived(msg.object, msg.severity);
        return;
    }

    // Meter status uses '#' as KV separator (not spaces), so the normal
    // parseKVs() in CommandParser doesn't handle it.  We intercept the raw
    // status line here and parse it ourselves.
    if (msg.type != MessageType::Status) return;

    // #3977: attribute display-pan writes to their originating client via the
    // S<handle>| source prefix (msg.handle; 0 = the radio itself). A foreign
    // session repeatedly adjusting OUR pan's dBm range is the #3951 zombie
    // signature — detect, log, and (opt-in) evict. Originator semantics were
    // observed live on fw 4.2.18 (FLEX-8400M); FlexLib never parses this
    // token, and 4.1.x is unverified — which is one reason eviction defaults
    // off (see staleSessionEvictionEnabled()).
    noteForeignPanWriteIfAny(msg.object, msg.kvs, msg.handle);

    // Raw line: "S<handle>|meter 7.src=SLC#7.num=0#7.nam=LEVEL#..."
    const QString& raw = msg.raw;
    const int pipe = raw.indexOf('|');
    if (pipe < 0) return;
    const QString body = raw.mid(pipe + 1);
    // Profile status: "profile tx list=Default^..." or "profile mic list=..."
    // Profile names contain spaces, so parseKVs() (which splits on spaces) breaks
    // the list value.  Handle raw here, same pattern as meter status.
    if (body.startsWith("profile tx ")) {
        handleProfileStatusRaw("tx", body.mid(11));  // skip "profile tx "
        return;
    }
    if (body.startsWith("profile mic ")) {
        handleProfileStatusRaw("mic", body.mid(12));  // skip "profile mic "
        return;
    }
    if (body.startsWith("profile global ")) {
        handleProfileStatusRaw("global", body.mid(15));  // skip "profile global "
        return;
    }

    // GPS status: "gps lat=...#lon=...#grid=...#tracked=...#visible=...#status=..."
    if (body.startsWith("gps ")) {
        handleGpsStatus(body.mid(4));  // skip "gps "
        return;
    }

    if (!body.startsWith("meter ")) return;

    handleMeterStatus(body.mid(6));  // skip "meter "
}

// ─── Status dispatch ──────────────────────────────────────────────────────────
//
// Object strings look like:
//   "radio"           → global radio properties
//   "slice 0"         → slice receiver
//   "panadapter 0"    → panadapter (spectrum)
//   "meter 1"         → meter reading (handled by onMessageReceived)
//   "removed=True"    → object was removed

bool RadioModel::backendCanTransmit() const
{
    return m_backend && m_backend->capabilities().canTransmit;
}

void RadioModel::setTxAudioMonitor(bool on)
{
    if (m_backend)
        m_backend->setTxAudioMonitor(on);
}

void RadioModel::submitTxAudio(const QByteArray& int16Stereo, int sampleRateHz,
                               bool clientLeveled)
{
    if (m_backend)
        m_backend->submitTxAudio(int16Stereo, sampleRateHz, clientLeveled);
}

bool RadioModel::sendCommand(const QString& cmd)
{
    // #3977: last-line ownership gate for pan writes. Every UI path that
    // adjusts a pan (auto-floor, band restore, center/bandwidth/zoom/fps)
    // funnels through here; when the radio has told us another client owns
    // the pan, drop the write instead of stomping the rightful owner — the
    // #3951 signature. Fails open when ownership is unknown.
    if (cmd.startsWith(QLatin1String("display pan set "))) {
        const QString panId =
            normalizePanadapterId(cmd.mid(16).section(QLatin1Char(' '), 0, 0));
        if (auto* pan = m_panadapters.value(panId, nullptr);
            pan && !pan->ownedByClient(clientHandle())) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: dropping pan-set for foreign-owned pan"
                << panId << "(owner 0x" + pan->clientHandle() + ") —" << cmd;
            return false;
        }
    }
    qCDebug(lcProtocol) << "RadioModel::sendCommand:" << cmd
             << "connected:" << isConnected() << "wan:" << (m_wanConn != nullptr);
    // sendCmd() reports a drop as sequence 0, before any wire write: the
    // profile-load hold backstop returns 0 directly, and a disconnected WAN
    // session returns 0 from WanConnection::sendCommand(). Both live seq
    // counters start at 1, so seq != 0 is exactly "the command was dispatched".
    return this->sendCmd(cmd) != 0;
}

void RadioModel::sendCmdPublic(const QString& cmd, ResponseCallback cb)
{
    sendCmd(cmd, cb);
}

void RadioModel::requestFileUploadPort(qint64 size, const QString& uploadKind,
                                        ResponseCallback cb)
{
    qCInfo(lcProtocol).noquote()
        << "RadioModel: file upload requested"
        << QStringLiteral("kind=%1").arg(uploadKind)
        << QStringLiteral("bytes=%1").arg(size);
    sendCmd(QStringLiteral("file upload %1 %2").arg(size).arg(uploadKind), cb);
}

void RadioModel::requestFileDownloadPort(const QString& downloadKind, ResponseCallback cb)
{
    qCInfo(lcProtocol).noquote()
        << "RadioModel: file download requested"
        << QStringLiteral("kind=%1").arg(downloadKind);
    sendCmd(QStringLiteral("file download %1").arg(downloadKind), cb);
}

void RadioModel::refreshProfiles()
{
    sendCmd(QStringLiteral("profile global info"));
    sendCmd(QStringLiteral("profile tx info"));
    sendCmd(QStringLiteral("profile mic info"));
}

bool RadioModel::isProfileTransferBlocked() const
{
    return m_radioTransmitting
        || m_txRequested
        || m_transmitModel.isMox()
        || m_transmitModel.isTuning();
}

void RadioModel::requestLocalPtt()
{
    // "enforce_local_ptt" returns 0x50001000; the settable key matches the
    // status key the radio broadcasts: local_ptt.  Firmware v1.4.0.0 quirk.
    sendCmd("client set local_ptt=1", [this](int code, const QString& body) {
        if (code != 0) {
            qCWarning(lcProtocol) << "requestLocalPtt: radio returned error"
                                  << Qt::hex << code << body;
            return;
        }
        // Optimistic update: mark our own entry as having PTT in case the radio
        // doesn't echo a local_ptt=1 status message back to us.
        quint32 ours = clientHandle();
        if (m_clientInfoMap.contains(ours)) {
            m_clientInfoMap[ours].localPtt = true;
            emitOtherClientsChanged();
        }
    });
}


void RadioModel::createRxAudioStream()
{
    if (m_rxAudio.streamId != 0) {
        logRemoteAudioRxSummary(QStringLiteral("create skipped: stream already known"));
        return;
    }
    if (m_rxAudio.createPending) {
        logRemoteAudioRxSummary(QStringLiteral("create skipped: request already pending"));
        return;
    }

    m_rxAudio.createPending = true;
    m_rxAudio.removeRequested = false;
    resetAudioStreamDiagnostics();
    logRemoteAudioRxSummary(QStringLiteral("create requested"));
    // Push our mute preference before opening the stream. Firmware defaults
    // mute_local_audio_when_remote=1, silencing hardware outputs whenever any
    // remote_audio_rx stream exists. Overriding here covers multi-client
    // scenarios where another client (e.g. SmartSDR) has set it to 1. (#1069)
    sendCmd(QString("radio set mute_local_audio_when_remote=%1")
                .arg(m_muteLocalWhenRemote ? 1 : 0));
    sendCmd(QString("stream create type=remote_audio_rx compression=%1").arg(audioCompressionParam()),
        [this](int code, const QString& body) {
            m_rxAudio.createPending = false;
            if (code == 0) {
                const quint32 streamId = RadioStatusOwnership::parseCreateResponseStreamId(body);
                if (streamId == 0) {
                    qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_rx returned unparseable body:"
                                          << body;
                    logRemoteAudioRxSummary(QStringLiteral("create response unparseable"));
                    return;
                }

                if (m_rxAudio.streamId != 0 && m_rxAudio.streamId != streamId) {
                    const quint32 oldStreamId = m_rxAudio.streamId;
                    qCDebug(lcProtocol) << "RadioModel: replacing restored remote_audio_rx"
                                        << RadioStatusOwnership::hexId(oldStreamId)
                                        << "with create response"
                                        << RadioStatusOwnership::hexId(streamId);
                    sendCmd(QString("stream remove %1").arg(RadioStatusOwnership::hexId(oldStreamId)));
                }

                m_rxAudio.streamId = streamId;
                m_rxAudio.clientHandle = clientHandle();
                m_rxAudio.compression = audioCompressionParam();
                resetAudioStreamDiagnostics();
                qCDebug(lcProtocol) << "RadioModel: remote_audio_rx stream created, id:"
                                    << RadioStatusOwnership::hexId(streamId);
                logRemoteAudioRxSummary(QStringLiteral("create response adopted"));
                if (m_rxAudio.removeRequested)
                    removeRxAudioStream();
            } else {
                qCWarning(lcProtocol) << "RadioModel: stream create remote_audio_rx failed, code"
                           << Qt::hex << code << "body:" << body;
                logRemoteAudioRxSummary(QStringLiteral("create failed"));
            }
        });
}

void RadioModel::removeRxAudioStream()
{
    if (m_rxAudio.streamId == 0) {
        if (m_rxAudio.createPending) {
            m_rxAudio.removeRequested = true;
            logRemoteAudioRxSummary(QStringLiteral("remove deferred: create pending"));
        } else {
            logRemoteAudioRxSummary(QStringLiteral("remove skipped: no known stream"));
        }
        return;
    }

    const quint32 streamId = m_rxAudio.streamId;
    // Reassert our mute preference when tearing down. If another client's
    // stream remains open after ours is removed, this prevents the global
    // mute_local_audio_when_remote from silencing hardware outputs. (#1110)
    sendCmd(QString("radio set mute_local_audio_when_remote=%1")
                .arg(m_muteLocalWhenRemote ? 1 : 0));
    sendCmd(QString("stream remove %1").arg(RadioStatusOwnership::hexId(streamId)));
    qCDebug(lcProtocol) << "RadioModel: removed remote_audio_rx stream"
                        << RadioStatusOwnership::hexId(streamId);
    m_rxAudio.streamId = 0;
    m_rxAudio.clientHandle = 0;
    m_rxAudio.statusSeen = false;
    m_rxAudio.removeRequested = false;
    m_rxAudio.compression.clear();
    resetAudioStreamDiagnostics();
    logRemoteAudioRxSummary(QStringLiteral("remove requested"));
}

void RadioModel::scheduleRxAudioStreamEnsure(const QString& reason)
{
    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    if (!pcAudio) {
        qCDebug(lcProtocol) << "RadioModel: PC audio disabled — skipping remote_audio_rx";
        if (m_rxAudio.streamId != 0) {
            qCDebug(lcProtocol) << "RadioModel: removing unexpected owned remote_audio_rx while PC audio is disabled";
            removeRxAudioStream();
        }
        logRemoteAudioRxSummary(QStringLiteral("ensure skipped: not needed"));
        return;
    }

    logRemoteAudioRxSummary(QStringLiteral("ensure scheduled: ") + reason);
    QTimer::singleShot(350, this, [this, reason]() {
        const bool pcAudioNow = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
        if (!isConnected()) {
            logRemoteAudioRxSummary(QStringLiteral("ensure canceled: disconnected"));
            return;
        }
        if (!pcAudioNow) {
            logRemoteAudioRxSummary(QStringLiteral("ensure canceled: no longer needed"));
            return;
        }

        qCDebug(lcProtocol) << "RadioModel: ensuring remote_audio_rx stream after status settle for"
                            << reason;
        createRxAudioStream();
    });
}

bool RadioModel::handleRemoteAudioRxStreamStatus(const QString& object,
                                                 const QMap<QString, QString>& kvs)
{
    const bool allowUnknownOwner = m_clientInfoMap.size() <= 1;
    const auto action = RadioStatusOwnership::applyRemoteAudioRxStatus(
        m_rxAudio, object, kvs, clientHandle(), allowUnknownOwner);

    if (action == RadioStatusOwnership::RemoteAudioRxAction::NotRemoteAudio)
        return false;

    const auto stream = RadioStatusOwnership::parseStreamObject(object);
    const QString streamText = stream.valid
        ? RadioStatusOwnership::hexId(stream.streamId)
        : QStringLiteral("(unknown)");

    switch (action) {
    case RadioStatusOwnership::RemoteAudioRxAction::DeferredUnknownOwner:
        qCDebug(lcProtocol) << "RadioModel: deferred remote_audio_rx status without client_handle"
                            << streamText;
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::IgnoredOtherClient:
        qCDebug(lcProtocol) << "RadioModel: ignored remote_audio_rx for another client"
                            << streamText;
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Adopted:
        qCDebug(lcProtocol) << "RadioModel: adopted owned remote_audio_rx status"
                            << streamText;
        resetAudioStreamDiagnostics();
        logRemoteAudioRxSummary(QStringLiteral("status adopted"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Updated:
        qCDebug(lcProtocol) << "RadioModel: updated owned remote_audio_rx status"
                            << streamText;
        logRemoteAudioRxSummary(QStringLiteral("status updated"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::Removed:
        qCDebug(lcProtocol) << "RadioModel: owned remote_audio_rx removed"
                            << streamText;
        resetAudioStreamDiagnostics();
        logRemoteAudioRxSummary(QStringLiteral("status removed"));
        break;
    case RadioStatusOwnership::RemoteAudioRxAction::NotRemoteAudio:
        break;
    }

    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    if (!pcAudio && m_rxAudio.streamId != 0
        && (action == RadioStatusOwnership::RemoteAudioRxAction::Adopted
            || action == RadioStatusOwnership::RemoteAudioRxAction::Updated)) {
        qCDebug(lcProtocol) << "RadioModel: removing restored remote_audio_rx because PC audio is disabled";
        removeRxAudioStream();
        return true;
    }

    if (m_rxAudio.removeRequested && m_rxAudio.streamId != 0)
        removeRxAudioStream();

    return true;
}

void RadioModel::logRemoteAudioRxSummary(const QString& reason) const
{
    const bool pcAudio = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    const bool autoStartTci = AppSettings::instance().value("AutoStartTCI", "False").toString() == "True";
    const bool ownerKnown = m_rxAudio.clientHandle != 0;
    const bool ownedByUs = ownerKnown && m_rxAudio.clientHandle == clientHandle();

    QStringList fields;
    fields << QStringLiteral("reason=\"%1\"").arg(reason);
    fields << QStringLiteral("stream=%1").arg(
        m_rxAudio.streamId == 0 ? QStringLiteral("(none)")
                                : RadioStatusOwnership::hexId(m_rxAudio.streamId));
    fields << QStringLiteral("owner=%1").arg(
        ownerKnown ? RadioStatusOwnership::hexId(m_rxAudio.clientHandle)
                   : QStringLiteral("(unknown)"));
    fields << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                       : QStringLiteral("?"));
    fields << QStringLiteral("pc_audio=%1").arg(pcAudio ? 1 : 0);
    fields << QStringLiteral("auto_tci=%1").arg(autoStartTci ? 1 : 0);
    fields << QStringLiteral("pending=%1").arg(m_rxAudio.createPending ? 1 : 0);
    fields << QStringLiteral("remove_requested=%1").arg(m_rxAudio.removeRequested ? 1 : 0);
    fields << QStringLiteral("status_seen=%1").arg(m_rxAudio.statusSeen ? 1 : 0);
    if (!m_rxAudio.compression.isEmpty())
        fields << QStringLiteral("compression=%1").arg(m_rxAudio.compression);

    qCInfo(lcProtocol).noquote()
        << "RadioModel: remote_audio_rx summary" << fields.join(QLatin1Char(' '));
}

quint32 RadioModel::sendCmd(const QString& command, ResponseCallback cb)
{
    auto& perf = PerfTelemetry::instance();
    if (perf.enabled()
        && command.startsWith(QStringLiteral("display pan set "))
        && command.contains(QStringLiteral(" center="))) {
        perf.recordPanCenterCommand();
    }

    // Memory commands against a radio with no memory slots are answered by the
    // local bank, before anything else looks at them. This is the single seam
    // that keeps the whole memory feature working off-Flex: the dialog, the
    // browse panel, CSV import/export, the spot feed and the automation verb
    // all reach the radio through these four commands, so answering them here
    // means none of them needed changing. It sits above the profile-load hold
    // deliberately — a local bank edit is not a radio-state write and has
    // nothing to reconcile with a profile load.
    if (const auto seq = tryLocalMemoryCommand(command, cb))
        return *seq;

    const ProfileLoadCommand profileLoad = parseProfileLoadCommand(command);
    if (profileLoad.valid) {
        const bool topologyProfile = profileLoadMayRebuildRadioTopology(profileLoad.type);
        if (topologyProfile) {
            m_profileLoadRadioStateWriteHoldUntilMs =
                std::max(m_profileLoadRadioStateWriteHoldUntilMs,
                         QDateTime::currentMSecsSinceEpoch() + kProfileLoadStateWriteHoldMs);
        }
        emit profileLoadStarted(profileLoad.type, profileLoad.name);

        ResponseCallback originalCallback = std::move(cb);
        cb = [this, profileLoad, topologyProfile, originalCallback = std::move(originalCallback)]
             (int code, const QString& body) mutable {
            if (originalCallback) {
                originalCallback(code, body);
            }

            if (code != 0) {
                qCWarning(lcProtocol).noquote()
                    << "RadioModel: profile load rejected"
                    << QStringLiteral("type=%1").arg(profileLoad.type)
                    << QStringLiteral("name=%1").arg(profileLoad.name)
                    << QStringLiteral("code=%1").arg(hexCode(code))
                    << QStringLiteral("body=%1").arg(body);
                return;
            }

            qCInfo(lcProtocol).noquote()
                << "RadioModel: profile load accepted"
                << QStringLiteral("type=%1").arg(profileLoad.type)
                << QStringLiteral("name=%1").arg(profileLoad.name);
            if (topologyProfile) {
                m_profileLoadRadioStateWriteHoldUntilMs =
                    std::max(m_profileLoadRadioStateWriteHoldUntilMs,
                             QDateTime::currentMSecsSinceEpoch() + kProfileLoadStateWriteHoldMs);
                scheduleRxAudioStreamEnsure(QStringLiteral("profile-load:%1").arg(profileLoad.type));
            }
            QTimer::singleShot(750, this, [this]() {
                if (isConnected()) {
                    refreshProfiles();
                }
            });
            emit profileLoadCompleted(profileLoad.type, profileLoad.name);
        };
    }

    if (!profileLoad.valid
        && profileLoadRadioStateWritesHeld()
        && isProfileOwnedRadioStateWrite(command)) {
        // Defense-in-depth backstop only. A command that reaches here is
        // DROPPED — it never gets a sequence number and never reaches the wire.
        //
        // Warn only for the routed fields (center/bandwidth/band), which is
        // the class carried by requestPanCenter()/requestPanBandwidth()/
        // requestPanBand(): one of those arriving here means a caller bypassed
        // the defer path and a user command is being lost — a real bug (#4142).
        //
        // The other suppressions on this path are model-echo/reconcile writers
        // that #3563 drops BY DESIGN (active-slice and TX-slice reasserts,
        // panafall/waterfall auto-black defaults). Several fire on every profile
        // load, so warning on them would cry wolf and bury the one line that
        // actually means something.
        //
        // " band=" keeps its leading space so it cannot match inside
        // "bandwidth=" — the two are distinct fields with distinct routes.
        const bool routedPanFieldWrite =
            command.startsWith(QStringLiteral("display pan set "))
            && (command.contains(QStringLiteral("center="))
                || command.contains(QStringLiteral("bandwidth="))
                || command.contains(QStringLiteral(" band=")));

        if (routedPanFieldWrite) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: DROPPED a routed pan field write during profile load —"
                << "this should have been deferred via requestPan*()"
                << command;
        } else {
            qCDebug(lcProtocol).noquote()
                << "RadioModel: suppressing profile-load radio-state write"
                << command;
        }
        if (cb) {
            cb(kProfileLoadSuppressedCommandCode,
               QStringLiteral("suppressed during profile load"));
        }
        return 0;
    }

    if (m_wanConn)
        return m_wanConn->sendCommand(command, std::move(cb));

    // A backend that is not Flex or Sim owns no RadioConnection, so there is
    // nothing to write to — invokeMethod() below would dereference null. This
    // is reachable on the memory-recall path (MainWindow follows `memory apply`
    // with a `slice tune`, and the tune is Flex wire text), so fail the way the
    // rest of sendCmd's drops do: sequence 0, meaning "not dispatched".
    if (!hasCommandPlane()) {
        qCDebug(lcProtocol).noquote()
            << "RadioModel: no command plane for this backend, dropping" << command;
        if (cb)
            cb(kNoCommandPlaneCode, QStringLiteral("this radio has no command plane"));
        return 0;
    }

    // Allocate seq on main thread, store callback locally. (#502)
    const quint32 seq = m_seqCounter.fetch_add(1);
    if (cb)
        m_pendingCallbacks.insert(seq, std::move(cb));

    // Queue the socket write on the connection's worker thread.
    QMetaObject::invokeMethod(m_connection, [conn = m_connection, seq, command] {
        conn->writeCommand(seq, command);
    });
    return seq;
}

quint32 RadioModel::clientHandle() const
{
    if (m_wanConn)
        return m_wanConn->clientHandle();
    // No RadioConnection for a non-Flex backend; the Flex client-handle concept
    // does not apply there.
    return m_connection ? m_connection->clientHandle() : 0u;
}

PanadapterModel* RadioModel::resolveBackendPan(const QString& backendPanId)
{
    // Every pan signal from a non-Flex backend has to come through here.
    //
    // A backend labels its pans in its own namespace ("hl2-2"); the models are
    // keyed by the NEUTRAL id ("0xe1000002"). resolvePan() on the raw backend id
    // therefore never matches, and its fallback is the ACTIVE pan — so with one
    // pan everything looked correct (the only pan was the active one), and with
    // four, every pan-addressed update landed on whichever pane happened to be
    // selected. Measured: RF gain reported 20 dB on one pan and 0 on the other
    // three, from a single radio-wide LNA.
    //
    // Flex keeps its existing behaviour: its pan ids ARE the model keys.
    if (m_flexBackend)
        return resolvePan(backendPanId);
    // A non-Flex backend that vends its own RadioConnection (the demo's Route A
    // SimBackend) claims pans from its own synthetic wire status, so — exactly
    // like Flex — its pan ids ARE the model keys. Exact lookup, deliberately NOT
    // resolvePan(): a geometry edge can outrun the wire claim during connect,
    // and the active-pan fallback would re-introduce the wrong-pan hazard this
    // helper exists to prevent. A miss returns null and the caller skips — and on
    // the demo that DROPS the geometry rather than deferring it: the synthetic
    // wire's `display pan center=…` is decoded only through m_flexBackend (see
    // handlePanadapterStatus), which is why RFC #4288 added this seam emit at all.
    // What keeps the ordering safe is SimBackend's 150 ms delay before
    // emitInitialState() (SimBackend.cpp) — do not shorten it. (#4671)
    if (m_connection)
        return panadapter(normalizePanadapterId(backendPanId));
    return panadapter(neutralPanIdString(neutralPanIndexFor(backendPanId)));
}

void RadioModel::wireSliceAudioIntentsToBackend(SliceModel* s)
{
    if (!s)
        return;

    // ONE place, called from EVERY site that constructs a SliceModel.
    //
    // These were originally written inline in the backend's slice-materialising
    // branch, which is the path a real radio takes — and only that path. A slice
    // built any other way (the automation slice fixture, a session restore) got
    // none of them, so its mute, level, balance and TX-slice requests were
    // silently dropped while every other slice's worked. That is the failure
    // mode `wirePanStreamRxAudioSinks()` was retired for in #4537: a sink added
    // at one construction site and missed at another is a dead feature nobody
    // can see is dead.
    //
    // A Flex applies mute/level/pan ON THE RADIO and sends one mixed stream, so
    // these no-op there (m_backend's defaults). A backend that demodulates every
    // receiver on this host has to apply them in its own mixer.
    connect(s, &SliceModel::audioMuteCommandIssued, this,
            [this, s](bool mute) {
        if (m_backend) m_backend->setSliceAudioMute(s->sliceId(), mute);
    });
    connect(s, &SliceModel::audioGainCommandIssued, this,
            [this, s](int gainPercent) {
        if (m_backend) m_backend->setSliceAudioGain(s->sliceId(), gainPercent);
    });
    connect(s, &SliceModel::audioPanCommandIssued, this,
            [this, s](int panPercent) {
        if (m_backend) m_backend->setSliceAudioPan(s->sliceId(), panPercent);
    });
    connect(s, &SliceModel::rxAntennaCommandIssued, this,
            [this, s](const QString& antenna) {
        if (m_backend && !usesFlexCommandPlane())
            m_backend->setSliceRxAntenna(s->sliceId(), antenna);
    });
    // "Make this the transmit slice." On a radio with one transmitter the
    // backend MOVES transmit rather than setting a flag, and republishes both
    // the old and the new slice so the indicator follows — which is why nothing
    // is assumed here about the outcome.
    connect(s, &SliceModel::txSliceCommandIssued, this,
            [this, s]() {
        if (m_backend) m_backend->setTxSlice(s->sliceId());
    });
    // Selecting a slice. Distinct from taking transmit: the operator listens on
    // one slice while transmitting on another routinely, so this must not drag
    // transmit with it. The backend clears the previously active slice, which on
    // a Flex arrives as a status echo and here has no other way of happening.
    connect(s, &SliceModel::activeSliceCommandIssued, this,
            [this, s]() {
        if (m_backend) m_backend->setActiveSlice(s->sliceId());
    });
}

QString RadioModel::neutralPanIdStringForTest(int panIdx)
{
    return neutralPanIdString(panIdx);
}

QString RadioModel::backendPanIdFor(const QString& modelPanId) const
{
    // THE RETURN TRIP. resolveBackendPan() translates backend -> model for every
    // signal coming UP; this translates model -> backend for every command going
    // DOWN, and both directions are required for the pair to be a mapping at all.
    //
    // Without it the app sends the MODEL's id ("0xe1000002") to a backend whose
    // pan ids are its own ("hl2-2"). A backend that ignored the argument (as the
    // single-pan HL2 did) never noticed. One that resolves it — as it must, to
    // know WHICH of four receivers to move — refuses the command outright, and
    // the symptom is a panadapter whose centre never moves while the widget's
    // own drag preview slides over a waterfall still drawn at the old edges.
    //
    // Flex pan ids ARE the model keys, so this is identity there.
    if (m_flexBackend || modelPanId.isEmpty())
        return modelPanId;
    bool ok = false;
    const quint32 id = modelPanId.toUInt(&ok, 0);   // base 0: honours the "0x"
    if (ok && id >= kNeutralPanStreamIdBase) {
        const int idx = static_cast<int>(id - kNeutralPanStreamIdBase);
        const auto it = m_backendPanIdByIndex.constFind(idx);
        if (it != m_backendPanIdByIndex.constEnd())
            return it.value();
    }
    // Not one of ours: hand it back untouched rather than inventing an id. A
    // backend that does not recognise it will refuse, which is the correct
    // outcome for a pan this session never mapped.
    return modelPanId;
}

int RadioModel::neutralPanIndexFor(const QString& backendPanId)
{
    // Backend pan ids are opaque strings. They are assigned neutral indices in
    // FIRST-SEEN order rather than parsed, so that this stays family-agnostic:
    // a backend numbering its pans "hl2-0..hl2-3", "rx1..rx4" or by UUID all
    // work, and no naming convention is load-bearing across the seam.
    //
    // Allocation is stable for the life of a connection, which is what lets the
    // waterfall geometry and the slice->pan association below stay addressed to
    // the same pane across reconnects of the same layout.
    if (backendPanId.isEmpty())
        return 0;   // a backend that names no pan gets the first one
    const auto it = m_backendPanIndex.constFind(backendPanId);
    if (it != m_backendPanIndex.constEnd())
        return it.value();
    // LOWEST FREE index, not size().
    //
    // size() is only collision-free while nothing is ever removed. Closing one
    // pan of four leaves size() == 3 while index 3 is still in use, so the next
    // pan was handed an index that already belonged to a live pane: the new pan
    // resolved to the EXISTING PanadapterModel, no pane was created, and the
    // occupant's geometry and frames were quietly taken over. Observed as
    // pans=3 slices=4 after closing the middle of four and reopening — a slice
    // with no panadapter behind it.
    int assigned = 0;
    while (m_backendPanIdByIndex.contains(assigned))
        ++assigned;
    m_backendPanIndex.insert(backendPanId, assigned);
    // Both directions are filled at the same moment, from the same allocation,
    // so the pair cannot drift. See backendPanIdFor().
    m_backendPanIdByIndex.insert(assigned, backendPanId);
    return assigned;
}

PanadapterModel* RadioModel::ensureOwnedPanadapter(const QString& panId)
{
    const QString normalizedPanId = normalizePanadapterId(panId);
    if (normalizedPanId.isEmpty())
        return nullptr;

    if (auto* existing = m_panadapters.value(normalizedPanId, nullptr))
        return existing;

    bool reclaimed = false;
    PanadapterModel* pan = nullptr;
    if (auto it = m_stalePanadapters.find(normalizedPanId);
        it != m_stalePanadapters.end() && it.value()) {
        pan = it.value();
        m_stalePanadapters.erase(it);
        reclaimed = true;
        // #3977: the stale pan still records the handle of the session we are
        // superseding. If that connection is somehow still alive radio-side
        // (half-open TCP, zombie process), its auto-floor tracker will keep
        // adjusting THIS pan's dBm range under us (#3951) — evict it the way
        // SmartSDR evicts a predecessor on takeover (observed on fw 4.2.18,
        // where the radio also self-heals duplicate client_ids on its own).
        // Only evict when the staged pan still records OUR OWN pre-reconnect
        // handle (captured at registration, consumed at stage time). If
        // status parsing reassigned the pan to another client before we went
        // down, that client is the pan's live rightful owner — evicting it
        // would start an eviction ping-pong between two healthy sessions
        // sharing a client_id.
        const quint32 oldHandle = pan->ownerHandle();
        if (oldHandle != 0 && oldHandle != clientHandle()
            && oldHandle == m_staleSessionOwnHandle) {
            evictStaleSession(oldHandle,
                              QStringLiteral("predecessor recorded on reclaimed "
                                             "pan %1").arg(normalizedPanId));
        }
    } else {
        pan = new PanadapterModel(normalizedPanId, this);
    }
    pan->setClientHandle(QString::number(clientHandle(), 16));
    m_panadapters[normalizedPanId] = pan;
    if (m_activePanId.isEmpty())
        m_activePanId = normalizedPanId;

    // Apply active throttle cap immediately — applyAdaptiveFrameRate only fires
    // on tier transitions, so a pan opened mid-throttle would run at the radio
    // default (~25 fps) until the next state change. currentAdaptiveFpsCap()
    // returns 0 when AdaptiveThrottleEnabled is off, so activeCap > 0 already
    // gates both the push and the deferred lambda.
    // Note: if the pan is created in a healthy state and the network degrades
    // before waterfallId arrives, this connect is never made and the cap is
    // applied by the next applyAdaptiveFrameRate() tier transition instead.
    const int activeCap = currentAdaptiveFpsCap();
    if (activeCap > 0) {
        qCDebug(lcProtocol) << "RadioModel: applying active throttle cap" << activeCap
                            << "to pan" << normalizedPanId;
        sendAdaptiveCapToPan(normalizedPanId, activeCap);
        // waterfallId may not be assigned yet (arrives in a subsequent status
        // message) — re-apply the cap once it lands.
        if (!reclaimed) {
            connect(pan, &PanadapterModel::waterfallIdChanged,
                    this, [this, normalizedPanId]() {
                const int cap = currentAdaptiveFpsCap();
                if (cap > 0) sendAdaptiveCapToPan(normalizedPanId, cap);
            });
        }
    }

    if (!reclaimed) {
        connect(pan, &PanadapterModel::waterfallIdChanged,
                this, &RadioModel::updateStreamFilters);
    }
    updateStreamFilters();

    sendCmd(QString("display pan rfgain_info %1").arg(normalizedPanId),
            [pan](int code, const QString& body) {
        if (code != 0 || body.isEmpty()) return;
        QStringList vals = body.split(',');
        if (vals.size() < 3) return;
        int low = vals[0].trimmed().toInt();
        int high = vals[1].trimmed().toInt();
        int step = vals[2].trimmed().toInt();
        if (step > 0)
            pan->setRfGainInfo(low, high, step);
    });

    qCDebug(lcProtocol) << "RadioModel:" << (reclaimed ? "reclaimed" : "claimed")
                        << "panadapter" << normalizedPanId;
    if (reclaimed) {
        emit panadapterReclaimed(pan);
    } else {
        emit panadapterAdded(pan);
    }

    const auto pending = m_pendingPanStatuses.take(normalizedPanId);
    if (!pending.second.isEmpty()) {
        qCDebug(lcProtocol) << "RadioModel: applying deferred panadapter status for"
                            << normalizedPanId;
        handlePanadapterStatus(normalizedPanId, pending.second);
    }

    return pan;
}

quint32 RadioModel::ourClientHandle() const { return clientHandle(); }

QString RadioModel::ourStationName() const
{
    QString station = AppSettings::instance().effectiveStationName();
    if (station.isEmpty()) {
        station = QSysInfo::machineHostName();
    }
    return station;
}

bool RadioModel::staleSessionEvictionEnabled() const
{
    // #3977: force-disconnecting another radio client is opt-in — detection
    // and forensics always run, the disconnect does not. Feature-owned nested
    // config per Principle V. fw 4.2+ self-heals duplicate-client_id zombies
    // (observed on 4.2.18); the client-side eviction targets older firmware
    // (the #3951 reporter's 8600 runs 4.1.3) where the S<handle> originator
    // semantics below are unverified — hence off by default.
    const QString json = AppSettings::instance()
                             .value(QStringLiteral("StaleSessionDefense"), QString())
                             .toString();
    const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
    return obj.value(QStringLiteral("EvictionEnabled")).toBool(false);
}

void RadioModel::evictStaleSession(quint32 handle, const QString& reason)
{
    if (handle == 0 || handle == clientHandle()) {
        return;
    }
    if (m_evictedPredecessorHandles.contains(handle)
        || m_evictionsInFlight.contains(handle)) {
        return;
    }
    m_evictionsInFlight.insert(handle);
    qCWarning(lcProtocol).noquote()
        << "RadioModel: evicting stale session" << hexId(handle) << "—" << reason;
    sendCmd(QStringLiteral("client disconnect 0x%1").arg(handle, 0, 16),
            [this, handle](int code, const QString& body) {
        m_evictionsInFlight.remove(handle);
        if (code == 0) {
            m_evictedPredecessorHandles.insert(handle);
        } else {
            // Not marked evicted: a refused disconnect must stay retryable,
            // and `get clients` reporting evicted=true for a live zombie
            // would defeat the forensics this exists for. (#3977)
            qCWarning(lcProtocol) << "RadioModel: client disconnect refused for"
                                  << Qt::hex << handle << "code" << code
                                  << "body:" << body;
        }
    });
}

void RadioModel::noteForeignPanWriteIfAny(const QString& object,
                                          const QMap<QString, QString>& kvs,
                                          quint32 sourceHandle)
{
    if (sourceHandle == 0 || sourceHandle == clientHandle()) {
        return;  // radio-originated or our own echo
    }
    if (!object.startsWith(QLatin1String("display pan "))) {
        return;
    }
    if (!kvs.contains(QStringLiteral("min_dbm"))
        && !kvs.contains(QStringLiteral("max_dbm"))) {
        return;
    }

    const QString panId =
        normalizePanadapterId(object.mid(12).section(QLatin1Char(' '), 0, 0));
    auto* pan = m_panadapters.value(panId, nullptr);
    // Fail CLOSED for evidence: only writes to a pan whose radio-confirmed
    // (or claim-time) owner is us count against another client. The fail-open
    // ownedByClient() would let writes to a not-yet-attributed pan frame a
    // legitimate peer. Note applyPanStatus re-stamps the pan when the radio
    // reassigns it, so a rightful new owner's echoes stop counting the moment
    // the radio broadcasts the transfer — that ordering is what prevents two
    // healthy sessions from evicting each other. (#3977)
    if (!pan || pan->ownerHandle() == 0 || pan->ownerHandle() != clientHandle()) {
        return;
    }

    auto& rec = m_foreignPanWrites[sourceHandle];
    rec.count++;
    rec.panId = panId;
    rec.lastMs = QDateTime::currentMSecsSinceEpoch();
    if (rec.count == 1 || rec.count % 25 == 0) {
        qCWarning(lcProtocol).noquote()
            << "RadioModel: foreign client" << hexId(sourceHandle)
            << "is adjusting OUR pan" << panId << "dBm range —"
            << rec.count << "writes so far (#3977)";
    }

    // Evidence-based eviction (#3951): three strikes AND the offender is
    // provably a stale instance of us (same program + station). Anything
    // else — SmartSDR, a differently-named station, a non-GUI client absent
    // from the roster — is the user's business; we log and leave it alone.
    constexpr int kEvictAfterForeignWrites = 3;
    if (rec.count < kEvictAfterForeignWrites
        || m_evictedPredecessorHandles.contains(sourceHandle)) {
        return;
    }
    // Identity must be radio-authoritative on BOTH sides: compare against the
    // station the radio reports for OUR handle, not our local settings (the
    // registered station can differ from the persisted preference).
    const ClientInfo info = m_clientInfoMap.value(sourceHandle);
    const QString ourStation = m_clientInfoMap.value(clientHandle()).station;
    if (info.program != QLatin1String("AetherSDR")
        || ourStation.isEmpty() || info.station != ourStation) {
        return;
    }
    if (!staleSessionEvictionEnabled()) {
        if (rec.count == kEvictAfterForeignWrites) {
            qCWarning(lcProtocol).noquote()
                << "RadioModel: stale AetherSDR session" << hexId(sourceHandle)
                << "(station" << info.station << ") reached" << rec.count
                << "foreign dBm writes to our pan" << panId
                << "— would evict, but eviction is disabled"
                << "(StaleSessionDefense.EvictionEnabled) (#3977/#3951)";
        }
        return;
    }
    evictStaleSession(sourceHandle,
                      QStringLiteral("%1 foreign dBm writes to our pan %2 "
                                     "(station %3) (#3977/#3951)")
                          .arg(rec.count).arg(panId, info.station));
}

bool RadioModel::sliceMayBelongToUs(int sliceId) const
{
    if (m_foreignSliceOwners.contains(sliceId)) {
        return false;
    }
    return m_ownedSliceIds.isEmpty() || m_ownedSliceIds.contains(sliceId);
}

void RadioModel::emitOtherClientsChanged()
{
    quint32 ours = clientHandle();
    QStringList names;
    for (auto it = m_clientStations.cbegin(); it != m_clientStations.cend(); ++it) {
        if (it.key() != ours)
            names << it.value();
    }
    emit otherClientsChanged(names.size(), names);
}

void RadioModel::traceDaxStreamStatus(const QString& object,
                                      const QMap<QString, QString>& kvs)
{
    const auto stream = parseStreamObject(object, QStringLiteral("stream"));
    if (stream.valid) {
        const QString incomingType = kvs.value(QStringLiteral("type"));
        const QString knownType = m_daxStreamDebug.value(stream.streamId).type;
        const bool daxRelated = isDaxStreamType(incomingType)
            || isDaxStreamType(knownType)
            || stream.streamId == m_daxTxStreamId;
        if (!daxRelated)
            return;

        const bool removed = streamStatusRemoved(stream, kvs);
        const QString type = incomingType.isEmpty() ? knownType : incomingType;
        if (removed) {
            qCDebug(lcDax).noquote()
                << "RadioModel: DAX stream removed"
                << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                << QStringLiteral("type=%1").arg(type.isEmpty() ? QStringLiteral("(unknown)") : type)
                << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
            if (stream.streamId == m_daxTxStreamId) {
                m_daxTxStreamId = 0;
                m_daxTxActive = false;
                m_daxTxClientHandle = 0;
                m_daxTxCreatePending = false;
            }
            m_deadDaxRxSeen.remove(stream.streamId);
            m_externalDaxTxSeen.remove(stream.streamId);
            m_externalDaxRxSeen.remove(stream.streamId);
            m_daxStreamDebug.remove(stream.streamId);
            return;
        }

        auto& state = m_daxStreamDebug[stream.streamId];
        if (!incomingType.isEmpty())
            state.type = incomingType;
        if (kvs.contains(QStringLiteral("client_handle")))
            state.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
        if (kvs.contains(QStringLiteral("dax_channel")))
            state.daxChannel = kvs.value(QStringLiteral("dax_channel")).toInt();
        if (kvs.contains(QStringLiteral("daxiq_channel")))
            state.daxIqChannel = kvs.value(QStringLiteral("daxiq_channel")).toInt();
        if (kvs.contains(QStringLiteral("slice")))
            state.sliceId = kvs.value(QStringLiteral("slice")).toInt();
        if (kvs.contains(QStringLiteral("daxiq_rate")))
            state.daxIqRate = kvs.value(QStringLiteral("daxiq_rate")).toInt();
        if (kvs.contains(QStringLiteral("pan")))
            state.panId = kvs.value(QStringLiteral("pan"));
        if (kvs.contains(QStringLiteral("ip")))
            state.ip = kvs.value(QStringLiteral("ip")).trimmed();
        if (kvs.contains(QStringLiteral("active"))) {
            state.active = kvs.value(QStringLiteral("active")) == QStringLiteral("1");
            state.activeKnown = true;
        }
        if (kvs.contains(QStringLiteral("tx"))) {
            state.tx = kvs.value(QStringLiteral("tx")) == QStringLiteral("1");
            state.txKnown = true;
        }

        if (state.type == QStringLiteral("dax_rx") && isDeadOrphanDaxRxStatus(kvs)) {
            if (!m_deadDaxRxSeen.contains(stream.streamId)) {
                m_deadDaxRxSeen.insert(stream.streamId);
                qCWarning(lcDax).noquote()
                    << "RadioModel: ignoring dead DAX RX stream status"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("dax_ch=%1").arg(state.daxChannel)
                    << QStringLiteral("slice=%1").arg(state.sliceId >= 0
                        ? QString::number(state.sliceId)
                        : QStringLiteral("?"))
                    << QStringLiteral("ip=%1").arg(state.ip);
            }
        }

        const bool ownerKnown = state.clientHandle != 0;
        const bool ownedByUs = ownerKnown && state.clientHandle == clientHandle();
        if (state.type == QStringLiteral("dax_tx") && ownerKnown && !ownedByUs) {
            if (!m_externalDaxTxSeen.contains(stream.streamId)) {
                m_externalDaxTxSeen.insert(stream.streamId);
                qCInfo(lcDax).noquote()
                    << "RadioModel: external DAX TX stream observed"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("owner=%1").arg(hexId(state.clientHandle));
            }
        } else if (state.type == QStringLiteral("dax_rx") && ownerKnown && !ownedByUs) {
            if (!m_externalDaxRxSeen.contains(stream.streamId)) {
                m_externalDaxRxSeen.insert(stream.streamId);
                qCInfo(lcDax).noquote()
                    << "RadioModel: external DAX RX stream observed"
                    << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
                    << QStringLiteral("owner=%1").arg(hexId(state.clientHandle))
                    << QStringLiteral("dax_ch=%1").arg(state.daxChannel)
                    << QStringLiteral("slice=%1").arg(state.sliceId >= 0
                        ? QString::number(state.sliceId)
                        : QStringLiteral("?"))
                    << QStringLiteral("ip=%1").arg(state.ip.isEmpty()
                        ? QStringLiteral("?")
                        : state.ip);
            }
        }

        if (state.type == QStringLiteral("dax_tx")
            && daxTxStatusCanUpdateLocalState(stream.streamId, m_daxTxStreamId, kvs, clientHandle())) {
            m_daxTxStreamId = stream.streamId;
            m_daxTxActive = state.tx;
            updateOperatorTransmit();  // DAX TX toggled — refresh the operator
                                       // TX-timer gate promptly (#4131 review)
            m_daxTxClientHandle = state.clientHandle;
            if (ownedByUs)
                m_daxTxCreatePending = false;
        }

        QStringList fields;
        fields << QStringLiteral("stream=%1").arg(hexId(stream.streamId));
        fields << QStringLiteral("type=%1").arg(state.type.isEmpty() ? QStringLiteral("(unknown)") : state.type);
        fields << QStringLiteral("owner=%1").arg(ownerKnown ? hexId(state.clientHandle) : QStringLiteral("(unknown)"));
        fields << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                            : QStringLiteral("?"));
        if (state.daxChannel > 0)
            fields << QStringLiteral("dax_ch=%1").arg(state.daxChannel);
        if (state.daxIqChannel > 0)
            fields << QStringLiteral("daxiq_ch=%1").arg(state.daxIqChannel);
        if (state.sliceId >= 0)
            fields << QStringLiteral("slice=%1").arg(state.sliceId);
        if (state.daxIqRate > 0)
            fields << QStringLiteral("daxiq_rate=%1").arg(state.daxIqRate);
        if (!state.panId.isEmpty())
            fields << QStringLiteral("pan=%1").arg(state.panId);
        if (!state.ip.isEmpty())
            fields << QStringLiteral("ip=%1").arg(state.ip);
        if (state.activeKnown)
            fields << QStringLiteral("active=%1").arg(state.active ? 1 : 0);
        if (state.txKnown)
            fields << QStringLiteral("tx=%1").arg(state.tx ? 1 : 0);
        fields << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));

        qCDebug(lcDax).noquote()
            << "RadioModel: DAX stream status" << fields.join(QLatin1Char(' '));
        return;
    }

    const auto txAudioStream = parseStreamObject(object, QStringLiteral("tx_audio_stream"));
    if (!txAudioStream.valid)
        return;

    const bool removed = streamStatusRemoved(txAudioStream, kvs);
    if (removed) {
        qCDebug(lcDax).noquote()
            << "RadioModel: DAX tx_audio_stream removed"
            << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
            << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
        if (txAudioStream.streamId == m_daxTxStreamId) {
            m_daxTxStreamId = 0;
            m_daxTxActive = false;
            m_daxTxClientHandle = 0;
            m_daxTxCreatePending = false;
        }
        m_daxStreamDebug.remove(txAudioStream.streamId);
        m_externalDaxTxSeen.remove(txAudioStream.streamId);
        return;
    }

    auto& state = m_daxStreamDebug[txAudioStream.streamId];
    state.type = QStringLiteral("dax_tx");
    if (kvs.contains(QStringLiteral("client_handle")))
        state.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
    if (kvs.contains(QStringLiteral("tx"))) {
        state.tx = kvs.value(QStringLiteral("tx")) == QStringLiteral("1");
        state.txKnown = true;
    }
    const bool ownerKnown = state.clientHandle != 0;
    const bool ownedByUs = ownerKnown && state.clientHandle == clientHandle();
    if (ownerKnown && !ownedByUs && !m_externalDaxTxSeen.contains(txAudioStream.streamId)) {
        m_externalDaxTxSeen.insert(txAudioStream.streamId);
        qCInfo(lcDax).noquote()
            << "RadioModel: external DAX TX stream observed"
            << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
            << QStringLiteral("owner=%1").arg(hexId(state.clientHandle));
    }
    if (daxTxStatusCanUpdateLocalState(txAudioStream.streamId, m_daxTxStreamId, kvs, clientHandle())) {
        m_daxTxStreamId = txAudioStream.streamId;
        m_daxTxActive = state.tx;
        updateOperatorTransmit();  // DAX TX toggled (#4131 review)
        m_daxTxClientHandle = state.clientHandle;
        if (ownedByUs)
            m_daxTxCreatePending = false;
    }
    qCDebug(lcDax).noquote()
        << "RadioModel: DAX tx_audio_stream status"
        << QStringLiteral("stream=%1").arg(hexId(txAudioStream.streamId))
        << QStringLiteral("owner=%1").arg(ownerKnown ? hexId(state.clientHandle) : QStringLiteral("(unknown)"))
        << QStringLiteral("ours=%1").arg(ownerKnown ? (ownedByUs ? QStringLiteral("1") : QStringLiteral("0"))
                                                    : QStringLiteral("?"))
        << QStringLiteral("tx=%1").arg(state.txKnown ? (state.tx ? QStringLiteral("1") : QStringLiteral("0"))
                                                     : QStringLiteral("?"))
        << QStringLiteral("keys=%1").arg(kvs.keys().join(QLatin1Char(',')));
}

// Single registration path for dax_rx streams (#3305): every consumer used to
// run its own statusReceived hook (bridge / TCI / RADE) with subtly different
// filtering — RADE's had no client_handle check at all, so an external DAX
// app's broadcast stream status could shadow the channel and suppress our own
// `stream create` (state-machines.md §7.2: stream statuses go to ALL clients).
void RadioModel::handleDaxRxStreamRegistry(const QString& object,
                                           const QMap<QString, QString>& kvs)
{
    const auto stream = parseStreamObject(object, QStringLiteral("stream"));
    if (!stream.valid) return;
    if (streamStatusRemoved(stream, kvs)) {
        // The removed form carries no type= (state-machines.md §7.6) — route
        // by id; a non-dax_rx id is a harmless no-op in the registry.
        m_panStream->unregisterDaxStream(stream.streamId);
        // Re-arm the #1439 nudge one-shot (#4383): a genuine `stream remove`
        // (band switch / re-create) means the next create for a reused id must
        // be allowed to nudge again. The transient unbind echo does NOT reach
        // here (it carries no removed form), so it cannot re-arm.
        m_nudgedDaxStreams.remove(stream.streamId);
        return;
    }
    if (kvs.value(QStringLiteral("type")) != QStringLiteral("dax_rx")) return;
    if (!streamStatusBelongsToUs(kvs, ourClientHandle())) {
        qCDebug(lcDax).noquote()
            << "RadioModel: ignoring foreign DAX RX stream"
            << QStringLiteral("stream=%1").arg(hexId(stream.streamId))
            << QStringLiteral("owner=%1").arg(kvs.value(QStringLiteral("client_handle")));
        return;
    }
    const int ch = kvs.value(QStringLiteral("dax_channel")).toInt();
    if (ch >= 1 && ch <= 8) {
        m_panStream->registerDaxStream(stream.streamId, ch);
        // #1439 client-registration re-assert — LEGACY FALLBACK ONLY, decided
        // here (not in the create reply) because this status is the moment the
        // stream↔slice binding is definitively known: a non-empty slice=<letter>
        // means the radio already auto-bound them (modern firmware, ≥4.2.18) and
        // no nudge is needed. Only when it did NOT auto-bind AND a slice carries
        // this channel do we re-assert `slice set dax=`. Deciding here is
        // race-free across transports (the create reply can precede this status
        // on WAN/SmartLink). This IS a status-echo edge, so on its own it would
        // oscillate — the per-stream one-shot below is what keeps it a genuine
        // fire-once-per-create fallback and holds the #3305 invariant (#4009/#4383).
        const bool autoBound =
            !kvs.value(QStringLiteral("slice")).trimmed().isEmpty();
        // One-shot gate (#4383): a re-assert of a live binding provokes the
        // radio into a transient dax=0/dax=1 unbind→rebind, and during the
        // unbind it re-broadcasts this stream status with an empty slice= —
        // indistinguishable from "never auto-bound". Without a per-stream gate
        // that echo re-enters !autoBound and fires the nudge again, forever
        // (the ~12–15 Hz #4009 storm PR #4017 reintroduced). The stream id is
        // constant across the whole unbind/rebind, so suppress every echo for
        // an id we have already nudged; the set is cleared only on a real
        // `stream remove` above, so a genuine re-create re-arms the fallback.
        const bool alreadyNudged = m_nudgedDaxStreams.contains(stream.streamId);
        if (!autoBound && !alreadyNudged && isConnected()) {
            for (auto* s : slices()) {
                if (s && s->daxChannel() == ch) {
                    qCInfo(lcDax) << "RadioModel: dax_rx ch" << ch
                                  << "not auto-bound — sending #1439 nudge";
                    m_nudgedDaxStreams.insert(stream.streamId);
                    sendCommand(QString("slice set %1 dax=%2")
                                    .arg(s->sliceId()).arg(ch));
                    break;
                }
            }
        }
    }
}

void RadioModel::onStatusReceived(const QString& object,
                                  const QMap<QString, QString>& kvs)
{
    // Relay to listeners (e.g., MemoryDialog)
    emit statusReceived(object, kvs);

    handleRemoteAudioRxStreamStatus(object, kvs);
    traceDaxStreamStatus(object, kvs);
    handleDaxRxStreamRegistry(object, kvs);

    if (object == "radio") {
        handleRadioStatus(kvs);
        return;
    }

    // Client connected/disconnected:
    //   object="client 0x7594C952"       kvs={connected, program=SmartSDR, station=W1AW}
    //   object="client 0x7594C952 disconnected"  kvs={forced=0, ...}
    static const QRegularExpression clientRe(R"(^client\s+(0x[0-9A-Fa-f]+)(?:\s+(\w+))?$)");
    if (object.startsWith("client 0x")) {
        const auto cm = clientRe.match(object);
        if (cm.hasMatch()) {
            quint32 handle = cm.captured(1).toUInt(nullptr, 16);
            QString action = cm.captured(2);  // "disconnected" or empty

            if (action == "disconnected") {
                if (handle == clientHandle()) {
                    if (statusFlagSet(kvs, QStringLiteral("duplicate_client_id"))) {
                        handleDuplicateClientIdDisconnect();
                    } else if (statusFlagSet(kvs, QStringLiteral("forced"))) {
                        handleForcedClientDisconnect();
                    }
                }
                m_clientStations.remove(handle);
                m_clientInfoMap.remove(handle);
                m_announcedClientConnections.remove(handle);
                m_startupClientConnections.remove(handle);
                // Drop any foreign-slot markers tied to the disconnecting
                // handle so the RX-applet tab row stops dimming slots the
                // client no longer owns.  A graceful disconnect usually
                // also produces `slice N removed=1` echoes which would
                // clear these, but hard/abrupt drops don't — covering both
                // paths here is harmless and avoids stale dim markers
                // (#2606).
                auto it = m_foreignSliceOwners.begin();
                while (it != m_foreignSliceOwners.end()) {
                    if (it.value() == handle) {
                        const int sliceId = it.key();
                        it = m_foreignSliceOwners.erase(it);
                        emit slotOccupancyChanged(sliceId);
                    } else {
                        ++it;
                    }
                }
                emitOtherClientsChanged();
            } else if (action == "connected" || kvs.contains("connected")) {
                QString program = cleanClientText(kvs.value("program", "Unknown"));
                QString station = cleanClientText(kvs.value("station", program));
                QString source = clientConnectionSource(kvs);
                auto existing = m_clientInfoMap.constFind(handle);
                if (source.isEmpty() && existing != m_clientInfoMap.cend())
                    source = existing->source;
                if (source.isEmpty() && m_wanConn)
                    source = QStringLiteral("SmartLink");
                bool ptt = kvs.value("local_ptt", "0") == "1";
                m_clientStations[handle] = station;
                ClientInfo client;
                client.clientId = kvs.value(QStringLiteral("client_id")).trimmed();
                client.station = station;
                client.program = program;
                client.source = source;
                client.localPtt = ptt;
                m_clientInfoMap[handle] = client;
                if (handle != clientHandle()) {
                    m_lastMultiFlexClientConnectMs = QDateTime::currentMSecsSinceEpoch();
                    qCDebug(lcProtocol).noquote()
                        << "RadioModel: noted Multi-Flex client connect for ping grace"
                        << QStringLiteral("handle=%1").arg(hexId(handle))
                        << QStringLiteral("program=%1").arg(program)
                        << QStringLiteral("station=%1").arg(station)
                        << QStringLiteral("source=%1").arg(source.isEmpty() ? QStringLiteral("direct") : source);
                }
                emitOtherClientsChanged();
                if (!shouldSuppressClientConnectionNotice(handle))
                    announceClientConnection(handle, source, station, program);
            } else if (kvs.contains("local_ptt") && m_clientInfoMap.contains(handle)) {
                // Partial update: radio echoes local_ptt state change without
                // a full connected message (e.g. after enforce_local_ptt command).
                m_clientInfoMap[handle].localPtt = kvs.value("local_ptt") == "1";
                emitOtherClientsChanged();
            }
        }
        return;
    }

    // XVTR status: "xvtr 0 name=2m rf_freq=144.000000 if_freq=28.000000 ..."
    static const QRegularExpression xvtrRe(R"(^xvtr\s+(\d+)$)");
    if (object.startsWith("xvtr")) {
        const auto m = xvtrRe.match(object);
        if (m.hasMatch()) {
            int idx = m.captured(1).toInt();
            // "in_use=0" means the xvtr was removed
            if (kvs.contains("in_use") && kvs["in_use"] == "0") {
                const auto existing = m_xvtrList.constFind(idx);
                if (existing != m_xvtrList.cend()) {
                    qCDebug(lcProtocol).noquote().nospace()
                        << "RadioModel: xvtr removed idx=" << idx
                        << " name=" << (existing->name.isEmpty() ? QStringLiteral("(unnamed)") : existing->name)
                        << " order=" << existing->order
                        << " is_valid=" << existing->isValid
                        << " has_is_valid=" << existing->hasIsValid;
                } else {
                    qCDebug(lcProtocol).noquote().nospace()
                        << "RadioModel: xvtr removed idx=" << idx
                        << " name=(unknown)";
                }
                m_xvtrList.remove(idx);
                emit infoChanged();
                return;
            }
            auto& x = m_xvtrList[idx];
            x.index = idx;
            if (kvs.contains("order"))     x.order   = kvs["order"].toInt();
            if (kvs.contains("name"))      x.name     = kvs["name"];
            if (kvs.contains("rf_freq"))   x.rfFreq   = kvs["rf_freq"].toDouble();
            if (kvs.contains("if_freq"))   x.ifFreq   = kvs["if_freq"].toDouble();
            if (kvs.contains("lo_error"))  x.loError  = kvs["lo_error"].toDouble();
            if (kvs.contains("rx_gain"))   x.rxGain   = kvs["rx_gain"].toDouble();
            if (kvs.contains("max_power")) x.maxPower = kvs["max_power"].toDouble();
            if (kvs.contains("rx_only"))   x.rxOnly   = kvs["rx_only"] == "1";
            const bool statusHasIsValid = kvs.contains("is_valid");
            if (statusHasIsValid) {
                x.isValid = kvs["is_valid"] == "1";
                x.hasIsValid = true;
            }
            qCDebug(lcProtocol).noquote().nospace()
                << "RadioModel: xvtr status idx=" << x.index
                << " name=" << (x.name.isEmpty() ? QStringLiteral("(unnamed)") : x.name)
                << " order=" << x.order
                << " rf_mhz=" << x.rfFreq
                << " if_mhz=" << x.ifFreq
                << " offset_mhz=" << (x.rfFreq - x.ifFreq)
                << " rx_only=" << x.rxOnly
                << " max_power=" << x.maxPower
                << " is_valid=" << x.isValid
                << " status_has_is_valid=" << statusHasIsValid
                << " has_is_valid=" << x.hasIsValid;
            emit infoChanged();
        }
        return;
    }

    // Filter sharpness: "radio filter_sharpness VOICE level=3 auto_level=0"
    if (object.startsWith("radio filter_sharpness")) {
        int level = kvs.value("level", "-1").toInt();
        bool autoLvl = kvs.value("auto_level", "0") == "1";
        if (object.contains("VOICE"))       { m_filterVoice = level; m_filterVoiceAuto = autoLvl; }
        else if (object.contains("CW"))     { m_filterCw = level; m_filterCwAuto = autoLvl; }
        else if (object.contains("DIGITAL")){ m_filterDigital = level; m_filterDigitalAuto = autoLvl; }
        emit infoChanged();
        return;
    }

    // License info (fw v1.4.0.0): three sub-objects per FlexLib:
    //   "license"              — radio_id, issued, last_refreshed_date, highest_major_version, region
    //   "license subscription" — name=smartsdr+|smartsdr+_early_access, expiration=<date>
    //   "license feature"      — name, enabled, reason (BUILT_IN|LICENSE_FILE|PLUS|EA)
    if (object == "license" && !kvs.contains("name")) {
        if (kvs.contains("radio_id")) {
            m_licenseRadioId = kvs["radio_id"].toUpper();
        }
        if (kvs.contains("highest_major_version")) {
            m_licenseMaxVersion = kvs["highest_major_version"];
        }
        // Base subscription is always "SmartSDR" — upgraded by subscription messages
        if (m_licenseSubscription.isEmpty()) {
            m_licenseSubscription = "SmartSDR";
        }
        emit infoChanged();
        return;
    }
    if (object == "license subscription") {
        // Per FlexLib: name=smartsdr+ or name=smartsdr+_early_access
        // with expiration=<ISO-8601 date>
        QString name = kvs.value("name").toLower();
        QString expStr = kvs.value("expiration");
        QDate expDate = QDate::fromString(expStr.left(10), Qt::ISODate);
        bool active = expDate.isValid() && expDate >= QDate::currentDate();
        if (name == "smartsdr+_early_access" && active) {
            m_licenseSubscription = "SmartSDR+ Early Access";
            m_licenseExpirationDate = expDate.toString("MM/dd/yyyy");
        } else if (name == "smartsdr+" && active) {
            m_licenseSubscription = "SmartSDR+";
            m_licenseExpirationDate = expDate.toString("MM/dd/yyyy");
        }
        emit infoChanged();
        return;
    }
    if (object == "license feature") {
        const QString name = normalizedLicenseFeatureName(kvs.value("name"));
        const QString enabledText = kvs.value("enabled").trimmed();
        const QString reason = kvs.value("reason").trimmed().toLower();
        if (name.isEmpty() || (enabledText != QLatin1String("0") && enabledText != QLatin1String("1"))) {
            qWarning() << "RadioModel: malformed license feature status" << kvs;
            return;
        }

        const LicenseFeatureState next{
            true,
            enabledText == QLatin1String("1"),
            reason.isEmpty() ? QStringLiteral("unknown") : reason
        };
        const LicenseFeatureState current = m_licenseFeatures.value(name);
        const bool changed = !current.seen
            || current.enabled != next.enabled
            || current.reason != next.reason;
        if (changed) {
            m_licenseFeatures.insert(name, next);
            emit licenseFeaturesChanged();
        }
        emit infoChanged();
        return;
    }

    if (object == "radio oscillator") {
        if (kvs.contains("state"))        m_oscState    = kvs["state"];
        if (kvs.contains("setting"))      m_oscSetting  = kvs["setting"];
        if (kvs.contains("locked"))       m_oscLocked   = kvs["locked"] == "1";
        if (kvs.contains("ext_present"))  m_extPresent  = kvs["ext_present"] == "1";
        if (kvs.contains("gpsdo_present") || kvs.contains("gnss_present")) {
            // Firmware generations use either spelling. Treat them as aliases
            // within this status update; do not latch a previous true value
            // when a later update explicitly reports that the source is gone.
            m_gpsdoPresent = kvs.value("gpsdo_present") == QLatin1String("1")
                || kvs.value("gnss_present") == QLatin1String("1");
        }
        if (kvs.contains("tcxo_present")) m_tcxoPresent = kvs["tcxo_present"] == "1";
        emit oscillatorChanged();
        emit infoChanged();
        return;
    }

    if (object == "radio static_net_params") {
        m_staticIp      = kvs.value("ip");
        m_staticNetmask = kvs.value("netmask");
        m_staticGateway = kvs.value("gateway");
        m_hasStaticIp   = !m_staticIp.isEmpty();
        emit infoChanged();
        return;
    }

    static const QRegularExpression sliceWaveformStatusRe(R"(^slice\s+(\d+)\s+waveform_status$)");
    const auto sliceWaveformStatusMatch = sliceWaveformStatusRe.match(object);
    if (sliceWaveformStatusMatch.hasMatch()) {
        QMap<QString, QString> report = kvs;
        report.insert(QStringLiteral("slice"), sliceWaveformStatusMatch.captured(1));
        m_flexWaveformModel.handleGenericStatus(report);
        return;
    }

    static const QRegularExpression sliceRe(R"(^slice\s+(\d+)$)");
    const auto sliceMatch = sliceRe.match(object);
    if (sliceMatch.hasMatch()) {
        const int sliceId = sliceMatch.captured(1).toInt();
        if (kvs.contains(QStringLiteral("mode_list"))) {
            const QString rawModeList = kvs.value(QStringLiteral("mode_list"));
            if (m_rawSliceModeLists.value(sliceId) != rawModeList) {
                m_rawSliceModeLists.insert(sliceId, rawModeList);
                emit rawSliceModeListsChanged();
            }
        }
        if (kvs.contains(QStringLiteral("waveform_status"))) {
            QMap<QString, QString> report = kvs;
            report.insert(QStringLiteral("slice"), sliceMatch.captured(1));
            m_flexWaveformModel.handleGenericStatus(report);
            return;
        }
        // Extract per-client TX info for multiFLEX dashboard before
        // handleSliceStatus filters out other clients' slices
        if (kvs.contains("client_handle") && kvs.value("tx") == "1") {
            quint32 ch = kvs["client_handle"].toUInt(nullptr, 16);
            auto it = m_clientInfoMap.find(ch);
            if (it != m_clientInfoMap.end()) {
                it->txAntenna = kvs.value("txant");
                if (kvs.contains("RF_frequency"))
                    it->txFreqMhz = kvs["RF_frequency"].toDouble();
            }
        }
        const bool removed = kvs.value("in_use") == "0";
        if (removed && m_rawSliceModeLists.remove(sliceId) > 0) {
            emit rawSliceModeListsChanged();
        }
        handleSliceStatus(sliceId, kvs, removed);
        return;
    }

    // Memory channels: "memory <index> key=val ..." or "memory <index> removed"
    // When there are no KV pairs (e.g., "memory 7 removed"), the parser puts
    // everything into the object name. Extract the index from the first token.
    if (object.startsWith("memory ")) {
        const QString rest = object.mid(7);  // "7 removed" or "7"
        const int sp = rest.indexOf(' ');
        const QString idxStr = (sp >= 0) ? rest.left(sp) : rest;
        bool ok;
        int idx = idxStr.toInt(&ok);
        if (ok) {
            // Merge any trailing bare words into kvs
            QMap<QString, QString> merged = kvs;
            if (sp >= 0) {
                const QString extra = rest.mid(sp + 1);
                for (const auto& token : extra.split(' ', Qt::SkipEmptyParts)) {
                    const int eq = token.indexOf('=');
                    if (eq < 0)
                        merged.insert(token, QString{});
                    else
                        merged.insert(token.left(eq), token.mid(eq + 1));
                }
            }
            qCDebug(lcProtocol) << "RadioModel: memory status for index" << idx
                     << "keys:" << merged.keys();
            handleMemoryStatus(idx, merged);
            return;
        }
    }

    // Meter status uses '#'-separated tokens and is handled by onMessageReceived().

    // "display pan 0x40000000 center=14.1 bandwidth=0.2 ..."
    // Only process status for OUR panadapter (matching client_handle or first unclaimed).
    static const QRegularExpression panRe(R"(^display pan\s+(0x[0-9A-Fa-f]+))");
    if (object.startsWith("display pan")) {
        const auto m = panRe.match(object);
        if (m.hasMatch()) {
            const QString panId = m.captured(1);

            // Handle pan removal — "display pan 0x40000001 removed" arrives
            // with no '=' so the parser puts the whole string in 'object'
            if (kvs.contains("removed") || object.endsWith("removed")) {
                // #4142: this pan's deferred writes die with it. Void them
                // loudly NOW — the radio re-uses pan ids across a profile
                // load (observed live on a 6700), so a write left queued here
                // would replay onto a same-id NEWCOMER and override the state
                // the profile just restored. The user's typed-tune-during-
                // rebuild loses both halves consistently: the slice half died
                // in the rebuild too.
                voidPendingPanWrites(
                    panId, QStringLiteral("pan removed during profile load"));
                m_radioDisplayPans.remove(normalizePanadapterId(panId));   // #3856 Layer B inventory
                m_pendingPanStatuses.remove(panId);
                m_panTransmitInhibitReasons.remove(panId);
                m_panTransmitInhibitedTxSlices.remove(panId);
                auto* pan = m_panadapters.take(panId);
                if (!pan) {
                    pan = m_stalePanadapters.take(panId);
                }
                if (pan) {
                    m_panStream->unregisterPanStream(pan->panStreamId());
                    m_panStream->unregisterWfStream(pan->wfStreamId());
                    qCDebug(lcProtocol) << "RadioModel: panadapter removed" << panId;
                    emit panadapterRemoved(panId);
                    pan->deleteLater();
                }
                if (m_activePanId == panId) {
                    m_activePanId = m_panadapters.isEmpty() ? QString()
                                                            : m_panadapters.firstKey();
                }
                return;
            }

            // #3856 Layer B: record into the radio-authoritative pan inventory
            // (presence + owner), independent of whether we end up owning it —
            // foreign and unclaimed pans are tracked too. Pruned on "removed".
            {
                const QString nPanId = normalizePanadapterId(panId);
                auto& e = m_radioDisplayPans[nPanId];
                if (kvs.contains(QStringLiteral("client_handle")))
                    e.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
                // Capture the pan-side waterfall link and stamp it onto the
                // waterfall entry's parentPanId. Some firmware conveys the
                // pan↔waterfall link only here (the pan's `waterfall=` key) and
                // omits `panadapter=` from the waterfall status — without this
                // backfill the waterfall's parent stays unknown, so a leaked
                // waterfall (parent pan removed) could never be flagged. Stamping
                // the persistent waterfall entry means the link survives the
                // pan's removal, which is exactly when the leak shows. (#3856)
                const QString wf =
                    normalizePanadapterId(kvs.value(QStringLiteral("waterfall")));
                if (!wf.isEmpty()) {
                    e.waterfallId = wf;
                    auto wit = m_radioDisplayWaterfalls.find(wf);
                    if (wit != m_radioDisplayWaterfalls.end() && wit->parentPanId.isEmpty())
                        wit->parentPanId = nPanId;
                }
            }

            // Preamp is shared antenna hardware — apply to ALL our pans
            // regardless of which client's pan status this came from.
            if (kvs.contains("pre")) {
                const QString pre = kvs["pre"];
                for (auto* pan : m_panadapters)
                    pan->setPreamp(pre);
            }

            // Staged (previous-session) pans are deliberately NOT treated as
            // known here: a handle-less status for a stale ID must Defer into
            // m_pendingPanStatuses rather than Apply, so reclaim only ever
            // happens on a confirmed client_handle match (Claim below). After
            // a radio reboot the same stream ID can be assigned to another
            // client (SmartSDR), and an incremental status without
            // client_handle must not capture it.
            const bool knownPan = m_panadapters.contains(panId);
            const auto ownershipAction = RadioStatusOwnership::classifyOwnedStatus(
                knownPan, kvs, false, clientHandle());
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Defer) {
                // Stamp the deferred entry and sweep any stale ones (#2228).
                // Entries older than 30 s reflect pans the radio never
                // resolved with a client_handle frame and never marked
                // "removed" — dropping them is never observably wrong
                // because consumption only happens on ownership confirm
                // (which is exactly the missing signal here).
                const qint64 now = QDateTime::currentSecsSinceEpoch();
                m_pendingPanStatuses[panId] = qMakePair(now, kvs);
                constexpr qint64 kPendingPanStatusTtlSec = 30;
                for (auto it = m_pendingPanStatuses.begin();
                     it != m_pendingPanStatuses.end();) {
                    if (now - it.value().first > kPendingPanStatusTtlSec)
                        it = m_pendingPanStatuses.erase(it);
                    else
                        ++it;
                }
                return;  // defer — can't confirm ownership yet
            }
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Ignore) {
                m_pendingPanStatuses.remove(panId);
                PanadapterModel* rejectedPan = nullptr;
                if (auto it = m_stalePanadapters.find(panId);
                    it != m_stalePanadapters.end()) {
                    rejectedPan = it.value();
                    m_stalePanadapters.erase(it);
                }
                if (rejectedPan) {
                    // #4142: any deferred writes targeted the pan the user
                    // saw, not this id's rightful owner — void, loudly.
                    voidPendingPanWrites(panId,
                                         QStringLiteral("pan ownership lost"));
                    m_panTransmitInhibitReasons.remove(panId);
                    m_panTransmitInhibitedTxSlices.remove(panId);
                    m_panStream->unregisterPanStream(rejectedPan->panStreamId());
                    m_panStream->unregisterWfStream(rejectedPan->wfStreamId());
                    qCDebug(lcProtocol) << "RadioModel: panadapter" << panId
                                        << "belongs to another client; removing local stale model";
                    emit panadapterRemoved(panId);
                    rejectedPan->deleteLater();
                    if (m_activePanId == panId) {
                        m_activePanId = m_panadapters.isEmpty() ? QString()
                                                                : m_panadapters.firstKey();
                    }
                } else if (m_panadapters.contains(panId)
                           && kvs.contains(QStringLiteral("client_handle"))) {
                    // A pan we hold reporting a different owner: the radio is
                    // authoritative (Principle II). Keep the pan (don't rip
                    // the user's display down on a transient fragment) but
                    // adopt the radio's verdict on ownership — stamping the
                    // new owner flips ownedByClient() false, which silences
                    // every outbound pan-set gate AND stops the foreign-write
                    // tally from counting the rightful owner's own echoes as
                    // zombie evidence against it. (#3977)
                    PanadapterModel* heldPan = m_panadapters.value(panId);
                    const QString newOwner =
                        kvs.value(QStringLiteral("client_handle"));
                    if (heldPan->ownedByClient(clientHandle())) {
                        qCWarning(lcProtocol)
                            << "RadioModel: panadapter" << panId
                            << "reassigned to client" << newOwner
                            << "— going quiet on it (#3977)";
                    }
                    // #4142: going quiet includes the deferred writes — the
                    // foreign-owner gate would drop them at flush anyway;
                    // void them at the moment ownership actually flips.
                    voidPendingPanWrites(panId,
                                         QStringLiteral("pan ownership lost"));
                    m_panTransmitInhibitReasons.remove(panId);
                    m_panTransmitInhibitedTxSlices.remove(panId);
                    heldPan->setClientHandle(newOwner);
                }
                return;  // not our panadapter, ignore
            }
            if (ownershipAction == RadioStatusOwnership::OwnedStatusAction::Claim)
                ensureOwnedPanadapter(panId);
            handlePanadapterStatus(panId, kvs);
        }
        return;
    }

    // "display waterfall 0x42000000 auto_black=1 ..."
    // Only process status for OUR waterfall (matching client_handle).
    static const QRegularExpression wfRe(R"(^display waterfall\s+(0x[0-9A-Fa-f]+)$)");
    if (object.startsWith("display waterfall")) {
        // #3856 Layer B: prune the radio-side waterfall inventory on removal.
        // Removal arrives in two wire forms (mirroring the pan branch): bare
        // "display waterfall 0x42… removed" (no '=', lands in `object`) and the
        // kv form "display waterfall 0x42… removed=1" (lands in `kvs`). Handle
        // both — the bare form won't match wfRe, and the kv form WOULD match
        // wfRe and be mis-recorded as an add if not caught first.
        if (kvs.contains(QStringLiteral("removed")) || object.endsWith(QLatin1String("removed"))) {
            static const QRegularExpression wfRemovedRe(R"(^display waterfall\s+(0x[0-9A-Fa-f]+))");
            const auto rm = wfRemovedRe.match(object);
            if (rm.hasMatch()) {
                const QString wfId = normalizePanadapterId(rm.captured(1));
                m_radioDisplayWaterfalls.remove(wfId);
                qCDebug(lcProtocol) << "RadioModel: waterfall removed (inventory)" << wfId;
            }
            return;
        }
        const auto m = wfRe.match(object);
        if (m.hasMatch()) {
            const QString wfId = m.captured(1);
            // #3856 Layer B: record into the radio-authoritative waterfall
            // inventory (presence + owner + parent pan), before the ownership
            // early-returns below — a leaked waterfall must be tracked even when
            // it carries no client_handle and we don't own it. Normalize the key
            // so it compares equal to owned/parent ids regardless of hex case.
            {
                const QString nWfId = normalizePanadapterId(wfId);
                auto& e = m_radioDisplayWaterfalls[nWfId];
                if (kvs.contains(QStringLiteral("client_handle")))
                    e.clientHandle = parseClientHandle(kvs.value(QStringLiteral("client_handle")));
                const QString parent =
                    normalizePanadapterId(kvs.value(QStringLiteral("panadapter")));
                if (!parent.isEmpty()) {
                    e.parentPanId = parent;
                } else if (e.parentPanId.isEmpty()) {
                    // Firmware omitted `panadapter=` on the waterfall status:
                    // backfill the parent from the pan that already reported this
                    // waterfall via its `waterfall=` link (handles pan-status-
                    // first ordering; the pan-status path covers the reverse). (#3856)
                    for (auto pit = m_radioDisplayPans.cbegin();
                         pit != m_radioDisplayPans.cend(); ++pit) {
                        if (pit.value().waterfallId == nWfId) {
                            e.parentPanId = pit.key();
                            break;
                        }
                    }
                }
            }
            // Check if this waterfall belongs to one of our panadapters.
            // The waterfallId is set on PanadapterModel by the "display pan" status
            // message which contains "waterfall=0x42xxxxxx".
            bool ours = false;
            PanadapterModel* ownerPan = nullptr;
            for (auto* pan : m_panadapters) {
                if (pan->waterfallId() == wfId) {
                    ours = true;
                    ownerPan = pan;
                    break;
                }
            }
            const QString parentPanId = normalizePanadapterId(kvs.value(QStringLiteral("panadapter")));
            if (!ownerPan && !parentPanId.isEmpty())
                ownerPan = m_panadapters.value(parentPanId, nullptr);
            if (!ours) {
                // Not yet associated via display pan status — check client_handle
                if (!kvs.contains("client_handle"))
                    return;  // defer — can't confirm ownership yet
                quint32 owner = parseClientHandle(kvs["client_handle"]);
                if (owner != clientHandle())
                    return;  // not our waterfall
                if (!ownerPan && !parentPanId.isEmpty())
                    ownerPan = ensureOwnedPanadapter(parentPanId);
                if (ownerPan && ownerPan->waterfallId().isEmpty())
                    ownerPan->setWaterfallId(wfId);
                ours = true;
            }

            // aetherd RFC 2.3: waterfall status decode fully behind the seam.
            // center/bandwidth converge onto the SAME decodePanCenterBandwidth as
            // pan status (single-sourced, the #4063 gap), and line_duration → the
            // universal panWaterfallLineDurationChanged. applyWaterfallStatus is
            // gone — PanadapterModel no longer decodes the wire.
            if (ownerPan && m_flexBackend) {
                m_flexBackend->decodePanCenterBandwidth(ownerPan->panId(), kvs);
                m_flexBackend->decodeWaterfallLineDuration(ownerPan->panId(), kvs);
            }
            if (activeWfId().isEmpty() && ownerPan == activePanadapter())
                ownerPan->setWaterfallId(wfId);
            updateStreamFilters();
            qCDebug(lcProtocol) << "RadioModel: claimed waterfall" << wfId;
            if (ownerPan && !ownerPan->isWaterfallConfigured()
                && !ownerPan->waterfallId().isEmpty() && isConnected()) {
                ownerPan->setWaterfallConfigured(true);
                configureWaterfall(ownerPan->waterfallId());
            }
        }
        return;
    }

    // ATU status: "atu <handle> status=TUNE_SUCCESSFUL atu_enabled=1 ..."
    // Routes to TransmitModel for the TX applet ATU controls.
    // Also forwards to TunerModel if an external TGXL is connected.
    static const QRegularExpression atuRe(R"(^atu\s+(\S+)$)");
    if (object.startsWith("atu")) {
        const auto m = atuRe.match(object);
        if (m.hasMatch() && m_tunerModel.handle().isEmpty())
            m_tunerModel.setHandle(m.captured(1));
        if (m_flexBackend) m_flexBackend->decodeAtuStatus(kvs);   // radio's own ATU → TransmitModel
        if (m_tunerModel.isPresent() && m_flexBackend)
            m_flexBackend->decodeTunerStatus(m_tunerModel.handle(), kvs);  // external TGXL → TunerModel (#4092/#4198)
        return;
    }

    // APD status family.  Forms:
    //   "apd enable=1 configurable=1"               → object "apd"
    //   "apd equalizer_active=1 ant=ANT1 freq=... rfpower=..."  → object "apd"
    //   "apd equalizer_reset"                        → object "apd equalizer_reset"
    //   "apd sampler tx_ant=ANT1 selected_sampler=RX_A valid_samplers=..."
    //                                                → object "apd sampler"
    // Our parser splits object/kvs at the last space before the first '=',
    // so any leading bare flags get absorbed into the object name.
    if (object == "apd sampler") {
        if (m_flexBackend) m_flexBackend->decodeApdSamplerStatus(kvs);
        return;
    }
    if (object == "apd" || object.startsWith("apd ")) {
        QMap<QString, QString> merged = kvs;
        if (object.length() > 3) {
            const QStringList flags = object.mid(4).split(' ', Qt::SkipEmptyParts);
            for (const auto& f : flags) merged.insert(f, QString{});
        }
        if (m_flexBackend) m_flexBackend->decodeApdStatus(merged);
        return;
    }

    // Amplifier status: both TGXL and PGXL report via the amplifier API.
    // FlexLib distinguishes them by the "model" key:
    //   model=TunerGeniusXL  → antenna tuner (TGXL)
    //   model=PowerGeniusXL  → power amplifier (PGXL)
    // "amplifier <handle> model=TunerGeniusXL operate=1 relayC1=20 ..."
    //
    // Removal can arrive in two forms (matches FlexLib Radio.cs:14060/14073
    // which uses substring `s.Contains("removed")`):
    //   1) bare:  "amplifier <handle> removed"        → trailing token, no '='
    //   2) kvs:   "amplifier <handle> ... removed=1"  → key in kvs map
    // Form 1 lands in `object` because our parser treats a body with no '='
    // as all-object-name; form 2 lands in `kvs` as a normal key.
    static const QRegularExpression ampRe(R"(^amplifier\s+(\S+)$)");
    static const QRegularExpression ampRemovedRe(R"(^amplifier\s+(\S+)\s+removed$)");
    if (object.startsWith("amplifier")) {
        // The radio reports both TGXL and PGXL via the "amplifier" API; route
        // TunerGeniusXL to TunerModel and every other (power) amp to AmpModel.
        // (#4094: amp state extracted from RadioModel into AmpModel.)
        const auto rm = ampRemovedRe.match(object);
        if (rm.hasMatch()) {
            const QString handle = rm.captured(1);
            qCDebug(lcProtocol) << "RadioModel: amplifier removed (bare) handle=" << handle;
            if (handle == m_tunerModel.handle())
                m_tunerModel.setHandle({});
            if (m_flexBackend) m_flexBackend->decodeAmplifierStatus(handle, QString(), {}, /*removed=*/true);
            return;
        }
        const auto m = ampRe.match(object);
        if (m.hasMatch()) {
            const QString handle = m.captured(1);
            const QString model = kvs.value("model");
            qCDebug(lcProtocol) << "RadioModel: amplifier status handle=" << handle << "model=" << model;

            // Handle removal (kvs form)
            if (kvs.contains("removed")) {
                if (handle == m_tunerModel.handle())
                    m_tunerModel.setHandle({});
                if (m_flexBackend) m_flexBackend->decodeAmplifierStatus(handle, QString(), {}, /*removed=*/true);
                return;
            }

            // Route TunerGeniusXL to TunerModel
            if (model == "TunerGeniusXL" || handle == m_tunerModel.handle()) {
                // Always update handle — first status may arrive with 0x00000000
                // before the real handle is assigned
                if (handle != "0x00000000" && handle != m_tunerModel.handle()) {
                    m_tunerModel.setHandle(handle);
                    m_meterModel.setTgxlHandle(handle.toUInt(nullptr, 0));
                } else if (m_tunerModel.handle().isEmpty()) {
                    m_tunerModel.setHandle(handle);
                    m_meterModel.setTgxlHandle(handle.toUInt(nullptr, 0));
                }
                if (m_flexBackend) m_flexBackend->decodeTunerStatus(m_tunerModel.handle(), kvs);   // #4092/#4198
            }
            // Power amplifier (PGXL / any non-TGXL amp) → AmpModel. `else` of the
            // tuner branch: a TGXL status is already routed above and would only
            // no-op the amp decode — skip it to avoid the per-status AmpDelta copy.
            else if (m_flexBackend) {
                m_flexBackend->decodeAmplifierStatus(handle, model, kvs, /*removed=*/false);
            }
        }
        return;
    }

    // Transmit status: "transmit rfpower=93 tunepower=38 tune=0 ..."
    if (object == "transmit") {
        if (m_flexBackend) m_flexBackend->decodeTransmitStatus(kvs);
        return;
    }

    // TX profile status: "profile tx list=DAX^Default^..." or "profile tx current=Default"
    if (object.startsWith("profile")) {
        handleProfileStatus(object, kvs);
        return;
    }

    // Per-band TX settings: "transmit band 9 band_name=20 rfpower=100 ..."
    static const QRegularExpression txBandRe(R"(^transmit band\s+(\d+)$)");
    if (object.startsWith("transmit band")) {
        const auto m = txBandRe.match(object);
        if (m.hasMatch()) {
            int id = m.captured(1).toInt();
            auto& b = m_txBandSettings[id];
            b.bandId = id;
            if (kvs.contains("band_name"))    b.bandName  = kvs["band_name"];
            if (kvs.contains("rfpower"))      b.rfPower   = kvs["rfpower"].toInt();
            if (kvs.contains("tunepower"))    b.tunePower = kvs["tunepower"].toInt();
            if (kvs.contains("inhibit"))      b.inhibit   = kvs["inhibit"] == "1";
            if (kvs.contains("hwalc_enabled"))b.hwAlc     = kvs["hwalc_enabled"] == "1";
        }
        return;
    }

    // Per-band interlock: "interlock band 9 band_name=20 acc_txreq_enable=0 ..."
    static const QRegularExpression ilBandRe(R"(^interlock band\s+(\d+)$)");
    if (object.startsWith("interlock band")) {
        const auto m = ilBandRe.match(object);
        if (m.hasMatch()) {
            int id = m.captured(1).toInt();
            auto& b = m_txBandSettings[id];
            b.bandId = id;
            if (kvs.contains("band_name"))       b.bandName = kvs["band_name"];
            if (kvs.contains("acc_txreq_enable"))b.accTxReq = kvs["acc_txreq_enable"] == "1";
            if (kvs.contains("rca_txreq_enable"))b.rcaTxReq = kvs["rca_txreq_enable"] == "1";
            if (kvs.contains("acc_tx_enabled"))  b.accTx    = kvs["acc_tx_enabled"] == "1";
            if (kvs.contains("tx1_enabled"))     b.tx1      = kvs["tx1_enabled"] == "1";
            if (kvs.contains("tx2_enabled"))     b.tx2      = kvs["tx2_enabled"] == "1";
            if (kvs.contains("tx3_enabled"))     b.tx3      = kvs["tx3_enabled"] == "1";
        }
        return;
    }

    // Spot status: "spot 42 callsign=W1AW rx_freq=14.074000 ..."
    //              "spot 42 removed"
    //              "spot 42 triggered pan=0x40000000"
    if (object.startsWith("spot ")) {
        static const QRegularExpression spotRe(R"(^spot\s+(\d+))");
        const auto sm = spotRe.match(object);
        if (sm.hasMatch()) {
            int idx = sm.captured(1).toInt();
            if (kvs.isEmpty() && object.contains("removed")) {
                m_spotModel.removeSpot(idx);
            } else if (kvs.isEmpty() && object.contains("triggered")) {
                // Parse pan= from the object string if present
                static const QRegularExpression panRe2(R"(pan=(0x[0-9A-Fa-f]+))");
                const auto pm = panRe2.match(object);
                emit m_spotModel.spotTriggered(idx, pm.hasMatch() ? pm.captured(1) : QString());
            } else {
                m_spotModel.applySpotStatus(idx, kvs);
            }
        }
        return;
    }

    // USB cable status: "usb_cable FTDI-1234 type=cat enable=1 ..."
    //                   "usb_cable FTDI-1234 bit 0 enable=1 source=active_slice ..."
    //                   "usb_cable FTDI-1234 removed"
    if (object.startsWith("usb_cable ")) {
        QString rest = object.mid(10);  // after "usb_cable "
        // Serial number is the first word
        int spaceIdx = rest.indexOf(' ');
        QString sn = (spaceIdx >= 0) ? rest.left(spaceIdx) : rest;

        if (rest.contains("removed")) {
            m_usbCableModel.handleRemoved(sn);
        } else {
            // Check for bit-level status: remaining object text is "bit <N>"
            // The CommandParser puts extra object words before the KV split.
            // "usb_cable FTDI-1234 bit 3" → object="usb_cable FTDI-1234 bit 3", kvs={enable=1,...}
            QMap<QString, QString> effectiveKvs = kvs;
            if (spaceIdx >= 0) {
                QString afterSn = rest.mid(spaceIdx + 1).trimmed();
                if (afterSn.startsWith("bit ")) {
                    int bitNum = afterSn.mid(4).trimmed().toInt();
                    effectiveKvs["_bit_number"] = QString::number(bitNum);
                }
            }
            m_usbCableModel.applyStatus(sn, effectiveKvs);
        }
        return;
    }

    // CWX status: "cwx sent=0", "cwx wpm=20", "cwx macro1=CQ\u007fCQ"
    if (object == "cwx") {
        m_cwxModel.applyStatus(kvs);
        return;
    }

    // DVK status: "dvk status=idle enabled=1" or "dvk added id=1 name="Recording 1" duration=0"
    if (object.startsWith("dvk")) {
        // Pass both the object string (may contain "added"/"deleted") and KVs
        m_dvkModel.applyStatus(object, kvs);
        return;
    }

    // NAVTEX status (v4.2.18): "navtex status=Active" or "navtex sent idx=1 serial_num=42"
    if (object.startsWith("navtex")) {
        m_navtexModel.parseStatus(object, kvs);
        return;
    }

    // Interlock status: "interlock tx_client_handle=0x... state=TRANSMITTING ..."
    if (object == "interlock") {
        // Track TX ownership — only show TX state if we own the transmitter
        if (kvs.contains("tx_client_handle")) {
            quint32 txOwner = kvs["tx_client_handle"].toUInt(nullptr, 16);
            m_txClientHandle = txOwner;
            m_txOwnedByUs = (txOwner == clientHandle() || txOwner == 0);
        }
        // Parse interlock timing fields into TransmitModel (#498)
        if (m_flexBackend) m_flexBackend->decodeInterlockStatus(kvs);

        // Track PTT source (#2373). The radio reports source=SW for software
        // MOX/CAT/xmit, and source=MIC|ACC|RCA for hardware-keyed PTT (mic
        // PTT line, footswitch via ACC, RCA TXREQ). Field is not always
        // present on every interlock status update, so persist the last
        // seen value. Matches FlexLib v4.2.18 ParsePTTSource (Radio.cs:7932).
        if (kvs.contains("source")) {
            m_lastInterlockSource = kvs["source"].toUpper();
        }

        if (kvs.contains("state")) {
            const QString state = kvs["state"].toUpper();

            // Emit raw radio TX state regardless of ownership — used by DAX
            // passthrough when an external app triggers PTT (#752).
            const bool radioTx = (state == "TRANSMITTING");
            m_radioTransmitting = radioTx;
            emit radioTransmittingChanged(radioTx);

            // Hardware PTT into the radio (mic PTT line, ACC footswitch, RCA
            // TXREQ) keys the radio without us calling setTransmit(), so
            // m_txRequested stays false. Treat hardware sources as a
            // legitimate "we own this TX" path alongside CW key, CWX, and
            // tune. SW source still requires m_txRequested so the optimistic
            // local-unkey behaviour from the TX_SYNC_FIX_REPORT is preserved
            // (a stale state=TRANSMITTING after setTransmit(false) on SW
            // source still falls through to the force-off branch). VOX also
            // keys the radio autonomously (no setTransmit()); when VOX is
            // enabled and we own TX, treat it as an owned-TX path like
            // hardware PTT so the SmartMtr TX meter and audio gate engage
            // (#3861). See RadioStatusOwnership::interlockKeepsLocalTxOn.
            if (!RadioStatusOwnership::interlockKeepsLocalTxOn(
                    m_txOwnedByUs, m_txRequested, m_cwKeyActive, m_cwxActive,
                    m_transmitModel.isTuning(), m_lastInterlockSource,
                    m_transmitModel.voxEnable())) {
                // Another client owns TX, or local unkey requested:
                // force local TX/audio gate off through all interlock states.
                m_transmitModel.setTransmitting(false);
                if (m_txAudioGate) {
                    m_txAudioGate = false;
                    emit txAudioGateChanged(false);
                }
            } else if (state == "TRANSMITTING") {
                // Radio confirms RF is keyed.
                m_transmitModel.setTransmitting(true);
                if (!m_txAudioGate) {
                    m_txAudioGate = true;
                    emit txAudioGateChanged(true);
                }
            } else {
                // Local key requested but radio is still in pre-TX transition
                // (e.g. PTT/TX delay). Keep optimistic TX-on gating for
                // modem/PTT edge alignment.
                const bool transitioningToTx =
                    state.contains("REQUESTED") || state.contains("DELAY");
                if (!transitioningToTx) {
                    m_transmitModel.setTransmitting(false);
                    m_cwxActive = false; // CWX send complete (#2097)
                }
                if (!transitioningToTx && m_txAudioGate) {
                    m_txAudioGate = false;
                    emit txAudioGateChanged(false);
                }
            }

            // Clear persisted PTT source once interlock confirms we're fully
            // out of TX, so a stale hardware-PTT source can't outlive the
            // actual key release if a later status update omits source=.
            // (#2373)
            if (state != "TRANSMITTING" && !state.contains("REQUESTED")
                && !state.contains("DELAY")) {
                m_lastInterlockSource.clear();
            }

            if (state == QStringLiteral("READY") || state == QStringLiteral("RECEIVE")) {
                m_lastInterlockNotificationKey.clear();
                m_lastInterlockNotificationMs = 0;
                m_interlockNotificationArmedUntilMs = 0;
                m_interlockNotificationSource = TransmitModel::PttSource::Mox;
            } else if (interlockNotificationArmed()) {
                // The radio reports interlock state as a sequence: PTT_REQUESTED
                // (with reason=AMP:TG while it waits for the amp/tuner relay
                // chain to settle) → TRANSMITTING (reason cleared, tx_allowed=1)
                // → UNKEY_REQUESTED (reason=AMP:TG again on the way back to
                // RECEIVE).  The PTT_REQUESTED / UNKEY_REQUESTED states are
                // transitional — TX is proceeding, not blocked.  The radio
                // tells us this explicitly via tx_allowed=1.  Only fire the
                // user-facing popup when the radio says TX is actually denied
                // (tx_allowed=0), which is the only case where the operator
                // needs to do something about the interlock.
                const QString txAllowed = kvs.value(QStringLiteral("tx_allowed"));
                const bool txDenied = (txAllowed == QStringLiteral("0"));
                if (txDenied) {
                    const QString message = radioInterlockNotificationMessage(kvs);
                    if (!message.isEmpty()) {
                        emitInterlockNotification(
                            message,
                            QStringLiteral("radio:%1:%2")
                                .arg(state, kvs.value(QStringLiteral("reason")).toUpper()));
                        m_interlockNotificationArmedUntilMs = 0;
                        m_interlockNotificationSource = TransmitModel::PttSource::Mox;
                    }
                }
            }
        }
        // Emit TX ownership state for title bar indicator
        // txOwnerChanged(otherIsTx, stationName) — true when ANOTHER client has TX
        if (!m_txOwnedByUs) {
            QString station = m_clientStations.value(m_txClientHandle, "TX Not Ready");
            emit txOwnerChanged(true, station);  // another client has TX
        } else {
            emit txOwnerChanged(false, {});  // we own TX (or nobody does)
        }
        if (m_flexBackend) m_flexBackend->decodeInterlockStatus(kvs);
        return;
    }

    // EQ status: "eq txsc mode=1 63Hz=0 125Hz=5 ..." or "eq rxsc ..."
    if (object == "eq txsc") {
        m_equalizerModel.applyTxEqStatus(kvs);
        return;
    }
    if (object == "eq rxsc") {
        m_equalizerModel.applyRxEqStatus(kvs);
        return;
    }

    // TNF status: "tnf <id> freq=14.100000 width=100 depth=1 permanent=0"
    static const QRegularExpression tnfRe(R"(^tnf\s+(\d+)$)");
    auto tnfMatch = tnfRe.match(object);
    if (tnfMatch.hasMatch()) {
        int tnfId = tnfMatch.captured(1).toInt();
        m_tnfModel.applyTnfStatus(tnfId, kvs);
        return;
    }

    // Waveform status — three sub-shapes introduced in firmware v4.2.18.
    // CommandParser already disambiguates via the object field; no regex needed.
    // FlexLib Radio.cs ParseWaveformStatus (line 11247). (#2136)
    if (object == QLatin1String("waveform")) {
        m_flexWaveformModel.handleInstalledList(kvs);
        return;
    }
    if (object == QLatin1String("waveform container")) {
        m_flexWaveformModel.handleContainerStatus(kvs);
        return;
    }
    if (object == QLatin1String("waveform wfp_status")) {
        m_flexWaveformModel.handleWfpStatus(kvs);
        return;
    }
    if (object == QLatin1String("waveform status")) {
        m_flexWaveformModel.handleGenericStatus(kvs);
        return;
    }

    // WAN, etc. — informational, ignore for now.
}

QString RadioModel::serial() const
{
    return m_lastInfo.serial;
}

QString RadioModel::gpsNtpServerAddress() const
{
    if (!m_automationGpsNtpServerAddress.isEmpty()) {
        return m_automationGpsNtpServerAddress;
    }
    if (!capabilities().hasNtpServer || isWan() || m_lastInfo.isRouted
        || m_lastInfo.address.isNull()) {
        return {};
    }
    return m_lastInfo.address.toString();
}

LicenseFeatureState RadioModel::licenseFeature(const QString& name) const
{
    return m_licenseFeatures.value(normalizedLicenseFeatureName(name));
}

bool RadioModel::licenseFeatureSeen(const QString& name) const
{
    return licenseFeature(name).seen;
}

bool RadioModel::licenseFeatureEnabled(const QString& name) const
{
    const LicenseFeatureState feature = licenseFeature(name);
    return feature.seen && feature.enabled;
}

QString RadioModel::licenseFeatureReason(const QString& name) const
{
    return licenseFeature(name).reason;
}

void RadioModel::setRemoteOnEnabled(bool on)
{
    m_remoteOnEnabled = on;
    sendCmd(QString("radio set remote_on_enabled=%1").arg(on ? 1 : 0));
    emit infoChanged();
}

void RadioModel::setMultiFlexEnabled(bool on)
{
    m_multiFlexEnabled = on;
    sendCmd(QString("radio set mf_enable=%1").arg(on ? 1 : 0));
    emit infoChanged();
}

void RadioModel::handleRadioStatus(const QMap<QString, QString>& kvs)
{
    // aetherd RFC 2.3 (RadioModel residual): the radio-global wire decode moved
    // to FlexBackend::decodeRadioStatus → radioChanged → applyRadioChanges (the
    // ctor-wired handler). This choke point drives it so live + deferred status
    // both convert.
    if (m_flexBackend) m_flexBackend->decodeRadioStatus(kvs);
}

void RadioModel::applyRadioChanges(const RadioDelta& d)
{
    bool changed = false;
    if (d.model) { m_model = *d.model; m_maxSlices = maxSlicesForModel(m_model); changed = true; }
    if (d.slicesAvailable) {
        // slices=N reports available (unused) slots; total capacity = open + available
        const int available = *d.slicesAvailable;
        const int currentSliceCount = static_cast<int>(m_slices.size());
        const int modelLimit = m_model.isEmpty() ? 0 : maxSlicesForModel(m_model);
        const int reportedTotal = currentSliceCount + available;
        if (modelLimit > 0 && reportedTotal > modelLimit) {
            qCWarning(lcProtocol) << "RadioModel: ignoring impossible slice capacity"
                                  << reportedTotal << "for model" << m_model
                                  << "limit" << modelLimit
                                  << "current slices" << currentSliceCount
                                  << "reported available" << available;
        }
        const int updatedMax = RadioStatusOwnership::boundedSliceCapacity(
            modelLimit,
            m_maxSlices,
            currentSliceCount,
            available);
        if (updatedMax != m_maxSlices) {
            m_maxSlices = updatedMax;
        }
        changed = true;
    }
    if (d.bandsRaw) {
        // Radio-declared band set (gateway/non-Flex hardware; see
        // declaredBands()).  Also accepted on the status path so a radio
        // connected by IP (no discovery packet seen) can still declare. The
        // raw "bands=" string rides through RadioDelta and is validated here
        // (parseDeclaredBands + BandDefs) — a model concern, matching how other
        // text fields decode model-side under aetherd RFC 2.3.
        const QStringList declared = parseDeclaredBands(*d.bandsRaw);
        if (declared != m_declaredBands) {
            m_declaredBands = declared;
            changed = true;
        }
    }
    if (d.callsign) {
        if (*d.callsign != m_callsign) {
            m_callsign = *d.callsign;
            emit callsignChanged(callsign());   // effective value, not the raw radio field
        }
        changed = true;
    }
    if (d.nickname) { m_nickname = *d.nickname; changed = true; }
    if (d.region)   { m_region = *d.region; changed = true; }
    if (d.radioOptions) { m_radioOptions = *d.radioOptions; changed = true; }
    if (d.remoteOnEnabled) { m_remoteOnEnabled = *d.remoteOnEnabled; changed = true; }
    if (d.multiFlexEnabled) { m_multiFlexEnabled = *d.multiFlexEnabled; changed = true; }
    if (d.enforcePrivateIp) { m_enforcePrivateIp = *d.enforcePrivateIp; changed = true; }
    if (d.binauralRx) { m_binauralRx = *d.binauralRx; changed = true; }
    if (d.fullDuplex) { m_fullDuplex = *d.fullDuplex; changed = true; }
    if (d.muteLocalWhenRemote) { m_muteLocalWhenRemote = *d.muteLocalWhenRemote; changed = true; }
    if (d.autoSave) {
        const bool newAutoSave = *d.autoSave;
        if (m_autoSave != newAutoSave) {
            m_autoSave = newAutoSave;
            emit autoSaveChanged(newAutoSave);
            changed = true;
        }
    }
    if (d.freqErrorPpb) { m_freqErrorPpb = *d.freqErrorPpb; changed = true; }
    if (d.calFreqMhz) { m_calFreqMhz = *d.calFreqMhz; changed = true; }
    if (d.lowLatencyDigital) { m_lowLatencyDigital = *d.lowLatencyDigital; changed = true; }
    if (d.rttyMarkDefault) {
        m_rttyMarkDefault = *d.rttyMarkDefault;
        for (SliceModel* s : m_slices)
            s->setRttyMarkDefault(m_rttyMarkDefault);
        changed = true;
    }
    if (d.tnfEnabled) {
        m_tnfModel.applyGlobalEnabled(*d.tnfEnabled);
    }
    // Audio outputs
    bool audioChanged = false;
    if (d.lineoutGain) { m_lineoutGain = *d.lineoutGain; audioChanged = true; }
    if (d.lineoutMute) { m_lineoutMute = *d.lineoutMute; audioChanged = true; }
    if (d.headphoneGain) { m_headphoneGain = *d.headphoneGain; audioChanged = true; }
    if (d.headphoneMute) { m_headphoneMute = *d.headphoneMute; audioChanged = true; }
    if (d.frontSpeakerMute) { m_frontSpeakerMute = *d.frontSpeakerMute; audioChanged = true; }
    if (d.daxiqCapacity)  m_daxIqModel.setCapacity(*d.daxiqCapacity);
    if (d.daxiqAvailable) m_daxIqModel.setAvailable(*d.daxiqAvailable);

    if (audioChanged) emit audioOutputChanged();
    if (changed) emit infoChanged();
}

void RadioModel::setLineoutGain(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_lineoutGain == v) {
        return;
    }
    m_lineoutGain = v;
    qCDebug(lcAudio) << "setLineoutGain:" << v;
    sendCmd(QString("mixer lineout gain %1").arg(v));
    emit audioOutputChanged();
}

// The three mute setters below mirror the command into the model the way the
// gain setters already do. Principle II allows the optimistic write — the
// radio's own `audio` status supersedes it on the next echo — and Flex echoes
// gain and mute alike (FlexBackend.cpp:942-946), so this is the same bargain
// setLineoutGain()/setHeadphoneGain() have always made, not a new one.
//
// Without it these flags are written ONLY by a Flex status, and Flex is their
// only parser. On every other backend they stay pinned to their `false`
// initialisers for the life of the session, so a reconciler reading them
// contradicts what the operator just asked for: mute from the title bar, nudge
// the headphone slider, and setHeadphoneGain()'s audioOutputChanged drives the
// glyph back to unmuted with no command sent to undo it (#4771).
//
// Where these differ from the gain setters: the command is sent
// unconditionally rather than short-circuited on an unchanged value. A mute is
// a request, and suppressing it because the model already believes the radio is
// muted would leave a model that has drifted from the radio unrecoverable from
// the UI — and that drift is precisely what this fixes.
void RadioModel::setLineoutMute(bool m)
{
    qCDebug(lcAudio) << "setLineoutMute:" << m;
    sendCmd(QString("mixer lineout mute %1").arg(m ? 1 : 0));
    if (m_lineoutMute != m) {
        m_lineoutMute = m;
        emit audioOutputChanged();
    }
}

void RadioModel::setHeadphoneGain(int v)
{
    v = std::clamp(v, 0, 100);
    if (m_headphoneGain == v) {
        return;
    }
    m_headphoneGain = v;
    qCDebug(lcAudio) << "setHeadphoneGain:" << v;
    sendCmd(QString("mixer headphone gain %1").arg(v));
    emit audioOutputChanged();
}

// Optimistic mirror — see the rationale on setLineoutMute() above.
void RadioModel::setHeadphoneMute(bool m)
{
    qCDebug(lcAudio) << "setHeadphoneMute:" << m;
    sendCmd(QString("mixer headphone mute %1").arg(m ? 1 : 0));
    if (m_headphoneMute != m) {
        m_headphoneMute = m;
        emit audioOutputChanged();
    }
}

// Optimistic mirror — see the rationale on setLineoutMute() above.
void RadioModel::setFrontSpeakerMute(bool m)
{
    qCDebug(lcAudio) << "setFrontSpeakerMute:" << m;
    sendCmd(QString("mixer front_speaker mute %1").arg(m ? 1 : 0));
    if (m_frontSpeakerMute != m) {
        m_frontSpeakerMute = m;
        emit audioOutputChanged();
    }
}

void RadioModel::handleSliceStatus(int id,
                                    const QMap<QString, QString>& kvs,
                                    bool removed)
{
    // Track slice ownership via client_handle (only present in some messages)
    if (kvs.contains("client_handle")) {
        quint32 owner = kvs["client_handle"].toUInt(nullptr, 16);
        if (owner == clientHandle()) {
            m_ownedSliceIds.insert(id);
            // If this slot was previously foreign (e.g. another client
            // released it and we just got assigned), drop the foreign mark.
            if (m_foreignSliceOwners.remove(id)) {
                emit slotOccupancyChanged(id);
            }
            qCDebug(lcProtocol) << "RadioModel: slice" << id << "is ours (client_handle match)";
        } else if (owner != 0) {
            qCDebug(lcProtocol) << "RadioModel: slice" << id << "belongs to another client"
                     << Qt::hex << owner << ", marking foreign";
            m_ownedSliceIds.remove(id);
            const bool wasForeign = m_foreignSliceOwners.value(id) == owner;
            m_foreignSliceOwners.insert(id, owner);
            // If we already have a SliceModel for this ID, remove it.  We
            // keep the foreign-owner record so UI can dim the slot.
            SliceModel* existing = slice(id);
            if (existing) {
                m_slices.removeOne(existing);
            } else {
                existing = m_staleSlices.take(id);
            }
            if (existing) {
                const bool wasTxSlice = existing->isTxSlice();
                emit sliceRemoved(id);
                existing->deleteLater();
                if (wasTxSlice)
                    m_meterModel.setActiveTxSlice(activeTxSliceNum());
                syncDigitalVoiceTxSelection();
            }
            if (!wasForeign) emit slotOccupancyChanged(id);
            return;  // slice belongs to another client
        }
    }

    // Removal can apply to ours OR a foreign slot — handle before the
    // not-in-owned-set early-out so foreign slot dimming clears when the
    // other client releases their slice.
    SliceModel* s = slice(id);

    if (removed) {
        if (s) {
            const bool wasTxSlice = s->isTxSlice();
            m_slices.removeOne(s);
            m_ownedSliceIds.remove(id);
            emit sliceRemoved(id);
            s->deleteLater();
            if (wasTxSlice)
                m_meterModel.setActiveTxSlice(activeTxSliceNum());
            emit slotOccupancyChanged(id);
        } else if (SliceModel* stale = m_staleSlices.take(id)) {
            emit sliceRemoved(id);
            stale->deleteLater();
            emit slotOccupancyChanged(id);
        } else if (m_foreignSliceOwners.remove(id)) {
            // Foreign client released their slot — clear the dim marker.
            emit slotOccupancyChanged(id);
        }
        syncDigitalVoiceTxSelection();
        return;
    }

    // If we've seen client_handle info and this slice isn't ours, skip it
    if (!m_ownedSliceIds.isEmpty() && !m_ownedSliceIds.contains(id)) {
        qCDebug(lcProtocol) << "RadioModel: ignoring slice" << id << "status (not in owned set)";
        return;
    }

    if (!s) {
        // Only create SliceModel from a full status (has in_use=1 and RF_frequency).
        // Partial statuses (e.g. "slice 3 rit_on=0") arrive early without enough
        // data to initialize the VFO widget correctly.
        if (!kvs.contains("in_use") || kvs["in_use"] != "1")
            return;

        bool reclaimed = false;
        if (auto it = m_staleSlices.find(id);
            it != m_staleSlices.end() && it.value()) {
            s = it.value();
            m_staleSlices.erase(it);
            reclaimed = true;
            qCDebug(lcProtocol) << "RadioModel: reclaimed slice" << id
                                << "from previous session";
        } else {
            s = new SliceModel(id, this);
            // Seed this slice's manual-squelch memory (#3326) from the
            // radio's own status when the frame carries a level: a slice
            // that already existed when we connected — ours from a prior
            // run, or one another Multi-Flex client created — arrives with
            // its own squelch_level, and that's the value to start from
            // (Principle II: the radio is authoritative). When the status
            // is silent, SliceModel's own class default applies — squelch
            // is radio-authoritative (AGENTS.md "do NOT persist"), so there
            // is no client-side last-used value to fall back to (#4592).
            // Either way this is only a starting point, not a live shared
            // value; each slice's own threshold is independent from here on.
            // applyChanges() below maintains the same memory from every
            // subsequent echo (the gate defaults open for a slice with no
            // surface attached), so this seed is the explicit belt to that
            // braces — it pins the value before any UI can observe the slice.
            bool haveRadioLevel = false;
            const int radioLevel =
                kvs.value(QStringLiteral("squelch_level")).toInt(&haveRadioLevel);
            if (haveRadioLevel)
                s->setManualSquelchLevel(radioLevel);
            // Forward slice commands to the radio
            connect(s, &SliceModel::commandReady, this, [this, s](const QString& cmd){
                sendSliceCommand(s, cmd);
            });
            // The per-slice audio and TX-slice intents, wired at EVERY
            // construction site rather than only the backend-materialising one.
            wireSliceAudioIntentsToBackend(s);
            // aetherd RFC 2.3 encode template: mode intent routes through the
            // backend verb, whose output goes through the guarded slice sink.
            // TODO(2.x): route via IRadioBackend once encode is backend-owned —
            // today this no-ops on the wire for any non-Flex backend (m_flexBackend
            // null) while still emitting modeChanged. Fine while Flex is the only
            // backend. (#4063 review)
            connect(s, &SliceModel::modeChangeRequested, this, [this, s](const QString& mode){
                if (m_flexBackend) m_flexBackend->setSliceMode(s->sliceId(), mode);
            });
            connect(s, &SliceModel::digitalVoiceSliceDisplaced,
                    this, [this](int sliceId, const QString& previousMode) {
                SliceModel* displaced = slice(sliceId);
                if (!displaced
                    || !DigitalVoiceModeRegistry::modeForRadioMode(
                            displaced->mode()).has_value()) {
                    return;
                }
                QString restoreMode = previousMode.trimmed().toUpper();
                if (restoreMode.isEmpty()
                    || DigitalVoiceModeRegistry::modeForRadioMode(
                        restoreMode).has_value()) {
                    restoreMode = DigitalVoiceModeRegistry::descriptor(
                        DigitalVoiceModeId::DStar).underlyingMode;
                }
                displaced->setMode(restoreMode);
            });
            connect(s, &SliceModel::txSliceChanged, this, [this](bool) {
                m_meterModel.setActiveTxSlice(activeTxSliceNum());
            });
        }
        s->setRttyMarkDefault(m_rttyMarkDefault);
        m_slices.append(s);
        // aetherd RFC 2.3: decode Flex slice status behind the seam → the
        // synchronous sliceChanged handler applies it to this slice (already in
        // m_slices) before the UI notify below. (populate frequency/mode first.)
        if (m_flexBackend) m_flexBackend->decodeSliceStatus(id, kvs);
        selectSoleValidTxAntennaIfNeeded(s, kvs.contains(QStringLiteral("txant")));
        m_meterModel.setActiveTxSlice(activeTxSliceNum());
        enforceTransmitInhibitForSlice(s);
        syncDigitalVoiceTxSelection();
        if (!reclaimed) {
            emit sliceAdded(s);
        } else if (s->isTxSlice()
                   && transmitInhibitMessageForSlice(s).isEmpty()
                   && QDateTime::currentMSecsSinceEpoch() >= m_profileLoadRadioStateWriteHoldUntilMs) {
            // Re-claim TX after a radio reboot (#145 semantics): the radio
            // recreates a stale slice model with tx=1 but tx_client_handle
            // pointing at our dead pre-reboot handle (or 0). Fresh startup
            // slices are left radio-owned so GUIClient restore can settle.
            sendCmd(QString("slice set %1 tx=1").arg(id));
        }
        emit slotOccupancyChanged(id);  // empty/foreign → ours
        return;   // status already decoded above via decodeSliceStatus; don't
                  // re-run the fall-through decodeSliceStatus at the end
    }

    // aetherd RFC 2.3: Flex slice status decodes in FlexBackend → sliceChanged →
    // applyChanges (synchronous, main-thread) drives this slice.
    if (m_flexBackend) m_flexBackend->decodeSliceStatus(id, kvs);
    selectSoleValidTxAntennaIfNeeded(s, kvs.contains(QStringLiteral("txant")));
    m_meterModel.setActiveTxSlice(activeTxSliceNum());
    enforceTransmitInhibitForSlice(s);
    syncDigitalVoiceTxSelection();

    // Aurora/AU-520: max_internal_pa_power in slice status reports the true
    // system power capability (e.g. 500W) while transmit status max_power_level
    // only reports the exciter limit (100W). Use the higher value. (#484)
    if (kvs.contains("max_internal_pa_power")) {
        int internalMax = kvs["max_internal_pa_power"].toInt();
        if (internalMax > m_transmitModel.maxPowerLevel()) {
            m_transmitModel.setMaxPowerLevel(internalMax);
        }
    }

    // Send any queued commands (e.g. if GUI changed freq before status arrived)
    if (isConnected()) {
        for (const QString& cmd : s->drainPendingCommands())
            sendSliceCommand(s, cmd);
    }
}

void RadioModel::handleMeterStatus(const QString& rawBody)
{
    // aetherd RFC 2.3: the SmartSDR meter-status wire decode moved to
    // FlexBackend::decodeMeterStatus, which emits meterDefined/meterRemoved →
    // the ctor-wired handlers drive the MeterModel. Driving it from this choke
    // point keeps both live and deferred/replayed meter status on the converted
    // path. (MeterModel already held only core state; this removes the last Flex
    // meter wire-decode from RadioModel.)
    if (m_flexBackend) {
        m_flexBackend->decodeMeterStatus(rawBody);
    }
}

void RadioModel::handleGpsStatus(const QString& rawBody)
{
    // aetherd RFC 2.3 (RadioModel residual): the Flex GPS wire decode moved to
    // FlexBackend::decodeGpsStatus → gpsChanged → applyGpsChanges (the model-side
    // member update + gpsStatusChanged emit). Thin forwarder behind the seam.
    if (m_flexBackend) m_flexBackend->decodeGpsStatus(rawBody);
}

void RadioModel::applyGpsChanges(const GpsDelta& d)
{
    // Apply the present fields (absent keys keep their prior value) and always
    // re-emit — the old handler emitted unconditionally on every GPS status.
    if (d.status)    m_gpsStatus    = *d.status;
    if (d.tracked)   m_gpsTracked   = *d.tracked;
    if (d.visible)   m_gpsVisible   = *d.visible;
    if (d.grid)      m_gpsGrid      = *d.grid;
    if (d.altitude)  m_gpsAltitude  = *d.altitude;
    if (d.lat)       m_gpsLat       = *d.lat;
    if (d.lon)       m_gpsLon       = *d.lon;
    if (d.time)      m_gpsTime      = *d.time;
    if (d.speed)     m_gpsSpeed     = *d.speed;
    if (d.track)     m_gpsTrack     = *d.track;
    if (d.freqError) m_gpsFreqError = *d.freqError;

    emit gpsStatusChanged(m_gpsStatus, m_gpsTracked, m_gpsVisible,
                           m_gpsGrid, m_gpsAltitude, m_gpsLat, m_gpsLon,
                           m_gpsTime);
}

void RadioModel::handlePanadapterStatus(const QString& panId, const QMap<QString, QString>& kvs)
{
    // Resolve the addressed pan (fall back to active) for the y_pixels/resize
    // bookkeeping below. All Flex status DECODE now lives in FlexBackend and
    // drives the model via the normalized signals wired in the ctor — so there
    // is no longer an applyPanStatus() call here (PanadapterModel holds no wire
    // decoder). aetherd RFC 2.3: PanadapterModel touchpoint fully converted.
    auto* pan = m_panadapters.value(panId, nullptr);
    const bool panMatchedById = pan != nullptr;
    if (!pan) pan = activePanadapter();  // fallback

    // Drive every converted pan field from this status choke point (not a live
    // statusReceived observer) so both live and deferred/replayed status flow
    // through the backend decode:
    //   - center/bandwidth → panCenterBandwidthChanged → setCenterBandwidth
    //   - min/max dBm      → panRangeChanged → setRange (+ setDbmRange side-effect)
    //   - rfgain / antenna → panRfGainChanged / panRx/AntennaList (universal)
    //   - WNB              → extensionStatus("flex","panWnb")
    //   - wide/loop/fps/pre/daxiq/client_handle/waterfall → …("flex","panState")
    // Each ctor-wired handler applies to the addressed pan and preserves the
    // legacy signals/side-effects the old inline code owned.
    if (m_flexBackend) {
        m_flexBackend->decodePanCenterBandwidth(panId, kvs);
        m_flexBackend->decodePanRange(panId, kvs);
        m_flexBackend->decodePanRfGain(panId, kvs);
        m_flexBackend->decodePanAntenna(panId, kvs);
        m_flexBackend->decodePanExtensions(panId, kvs);
        m_flexBackend->decodePanState(panId, kvs);
    }
    // Track usable ypixels from radio status — the radio encodes FFT bins as
    // pixel Y positions (0..ypixels-1), so PanadapterStream needs this for dBm
    // conversion. Tiny default/reset values are handled below by re-pushing the
    // real widget dimensions; do not feed them to the decoder or most FFT bins
    // clamp into a flat floor until the next dimensions echo.
    if (kvs.contains("y_pixels") && pan && panMatchedById) {
        const int yPix = kvs["y_pixels"].toInt();
        if (yPix > kDefaultPanDimensionThreshold) {
            const bool scaleChanged = pan->setFftYPixels(yPix);
            m_panStream->setYPixels(pan->panStreamId(), yPix);
            if (scaleChanged) {
                emit panadapterFftScaleChanged(pan->panId(), yPix);
            }
        }
    }
    if ((kvs.contains("x_pixels") || kvs.contains("y_pixels")) && pan && panMatchedById) {
        const int xPix = kvs.value("x_pixels", "0").toInt();
        const int yPix = kvs.value("y_pixels", "0").toInt();
        // Radio reset to defaults (profile load, reconnect) — re-push real dimensions
        if ((xPix > 0 && xPix <= kDefaultPanDimensionThreshold)
            || (yPix > 0 && yPix <= kDefaultPanDimensionThreshold)) {
            emit panDimensionsNeeded(pan->panId());
        }
    }
    // (ant_list is now decoded in FlexBackend → panAntennaListChanged, whose
    // ctor handler drives both the pan model AND this m_antList/antListChanged —
    // the old inline dual-parse here is gone. aetherd RFC 2.3.)

    // Configure the panadapter once we know its ID.
    if (pan && !pan->isResized() && isConnected()) {
        pan->setResized(true);
        configurePan(pan->panId());
    }
}

void RadioModel::updateStreamFilters()
{
    // Stream filtering is a Flex VITA-49 concept and lives on PanadapterStream,
    // which a non-Flex backend does not own. Its frames reach the UI through the
    // neutral panFeed signals instead, so there is nothing to register here.
    if (!m_panStream)
        return;
    // Register all known pan/wf stream IDs with PanadapterStream
    for (auto* pan : m_panadapters) {
        if (pan->panStreamId())
            m_panStream->registerPanStream(pan->panStreamId());
        if (pan->wfStreamId())
            m_panStream->registerWfStream(pan->wfStreamId());
    }
}

void RadioModel::configurePan(const QString& panId)
{
    const QString targetPanId = normalizePanadapterId(panId);
    if (targetPanId.isEmpty()) return;

    // Request MainWindow to push actual widget dimensions for this pan.
    // Do NOT hardcode xpixels/ypixels here — MainWindow knows the real sizes.
    emit panDimensionsNeeded(targetPanId);

    // Do not push a default min_dbm/max_dbm here. configurePan() also runs for
    // radio-restored pans on startup/profile recall, and the radio owns saved
    // pan dBm ranges. New user-created pans can still be initialized by the
    // createPanadapter() response path.
}

void RadioModel::configureWaterfall(const QString& waterfallId)
{
    const QString targetWaterfallId = RadioStatusOwnership::normalizedFlexId(waterfallId);
    if (targetWaterfallId.isEmpty()) return;

    // Initialize with radio auto-black OFF (the client renders the floor from
    // its own estimate by default). The persisted auto-black on/off and
    // client-vs-radio source are pushed moments later from the session layer
    // via setWaterfallAutoBlack()/setWaterfallAutoBlackSource(), which raise
    // auto_black=1 only when the user has selected radio-side. black_level is
    // the manual fallback; color_gain is applied client-side via wfHighThresholdRaw.
    // FlexLib uses "display panafall set" addressed to the waterfall stream ID.
    const QString cmd = QString("display panafall set %1 auto_black=0 black_level=15 color_gain=50")
                            .arg(targetWaterfallId);
    sendCmd(cmd, [this, targetWaterfallId](int code, const QString&) {
        if (code != 0) {
            qCDebug(lcProtocol) << "RadioModel: display panafall set waterfall failed, code"
                     << Qt::hex << code << "— trying display waterfall set";
            // Fallback for firmware that doesn't support panafall addressing
            sendCmd(
                QString("display waterfall set %1 auto_black=0 black_level=15 color_gain=50")
                    .arg(targetWaterfallId),
                [](int code2, const QString&) {
                    if (code2 != 0)
                        qCWarning(lcProtocol) << "RadioModel: display waterfall set also failed, code"
                                   << Qt::hex << code2;
                    else
                        qCDebug(lcProtocol) << "RadioModel: waterfall configured via display waterfall set";
                });
        } else {
            qCDebug(lcProtocol) << "RadioModel: waterfall configured (auto_black=0 black_level=15 color_gain=50)";
        }
    });
}

bool RadioModel::profileLoadRadioStateWritesHeld() const
{
    return QDateTime::currentMSecsSinceEpoch() < m_profileLoadRadioStateWriteHoldUntilMs;
}

void RadioModel::ensureDefaultSlicePreferringRestoredPan()
{
    auto& settings = AppSettings::instance();

    SliceRecreatePolicy::Inputs in;
    in.lastFreqMhz = settings.value("LastFrequency", "0").toDouble();
    in.lastMode = settings.value("LastMode", "").toString();

    // If m_activePanId names a pan we already hold, the radio restored it for us
    // (claimed well before the "slice list" query resolved — see #3212). Feed its
    // center to the policy so the recreated slice lands inside the visible span.
    PanadapterModel* restored = (!m_activePanId.isEmpty())
        ? m_panadapters.value(m_activePanId, nullptr)
        : nullptr;
    if (restored) {
        in.hasRestoredPan = true;
        in.restoredPanCenterMhz = restored->centerMhz();
    }

    const SliceRecreatePolicy::Decision d = SliceRecreatePolicy::decide(in);
    const QString freqStr = QString::number(d.freqMhz, 'f', 6);

    if (d.action == SliceRecreatePolicy::Action::ReuseRestoredPan) {
        qCDebug(lcProtocol) << "RadioModel: no slices but pan" << m_activePanId
                 << "already restored — creating slice on it at" << freqStr << d.mode;
        createDefaultSliceOnPan(m_activePanId, freqStr, d.mode, d.antenna);
    } else {
        qCDebug(lcProtocol) << "RadioModel: no slices and no restored pan — creating default panafall + slice";
        createDefaultSlice(freqStr, d.mode, d.antenna);
    }
}

// Standalone mode: create panadapter + slice.
// FlexLib v4.2.18 uses "display panafall create x=100 y=100"; keep the
// legacy "panadapter create" as a fallback for older firmware.

void RadioModel::createDefaultSlice(const QString& freqMhz,
                                     const QString& mode,
                                     const QString& antenna)
{
    qCDebug(lcProtocol) << "RadioModel: standalone mode — creating panadapter + slice"
             << freqMhz << mode << antenna;

    const auto handleCreatedPan =
        [this, freqMhz, mode, antenna](const QString& source,
                                       int code,
                                       const QString& body) -> bool {
        if (code != 0) {
            qCWarning(lcProtocol) << "RadioModel:" << source << "failed, code"
                                  << Qt::hex << code << "body:" << body;
            emit panadapterLimitReached(maxPanadapters(), m_model);
            return false;
        }

        qCDebug(lcProtocol) << "RadioModel:" << source << "response body:" << body;
        const QString panId = parsePanadapterCreateId(body);
        if (panId.isEmpty()) {
            qCWarning(lcProtocol) << "RadioModel:" << source
                                  << "returned empty pan_id";
            return false;
        }

        qCDebug(lcProtocol) << "RadioModel: panadapter created, pan_id =" << panId;
        createDefaultSliceOnPan(panId, freqMhz, mode, antenna);
        return true;
    };

    sendCmd("display panafall create x=100 y=100",
        [this, handleCreatedPan](int code, const QString& body) {
            if (code == 0) {
                handleCreatedPan(QStringLiteral("display panafall create"), code, body);
                return;
            }

            qCWarning(lcProtocol) << "RadioModel: display panafall create failed, code"
                                  << Qt::hex << code << "body:" << body
                                  << "- trying legacy panadapter create";
            sendCmd("panadapter create",
                [handleCreatedPan](int legacyCode, const QString& legacyBody) {
                    handleCreatedPan(QStringLiteral("panadapter create"),
                                     legacyCode,
                                     legacyBody);
                });
        });
}

void RadioModel::createDefaultSliceOnPan(const QString& panId,
                                         const QString& freqMhz,
                                         const QString& mode,
                                         const QString& antenna)
{
    auto* pan = ensureOwnedPanadapter(panId);
    if (!pan) {
        qCWarning(lcProtocol) << "RadioModel: cannot create slice without panadapter id";
        return;
    }

    const QString sliceCmd =
        QString("slice create pan=%1 freq=%2 antenna=%3 mode=%4")
            .arg(pan->panId(), freqMhz, antenna, mode);

    sendCmd(sliceCmd,
        [this, panId = pan->panId()](int code, const QString& body) {
            if (code != 0) {
                qCWarning(lcProtocol) << "RadioModel: slice create failed for pan"
                                      << panId << "code" << Qt::hex << code
                                      << "body:" << body;
                emit sliceCreateFailed(maxSlices(), m_model);
            } else {
                qCDebug(lcProtocol) << "RadioModel: slice created, index =" << body;
                // Radio now emits S|slice N ... status messages;
                // handleSliceStatus() picks them up automatically.
            }
        });
}

void RadioModel::handleProfileStatus(const QString& object,
                                      const QMap<QString, QString>& kvs)
{
    // Profile list/current with space-containing names are handled by
    // handleProfileStatusRaw() via onMessageReceived().  This fallback
    // handles any remaining profile status keys that don't have spaces
    // (e.g. "profile importing=1", "profile exporting=0").
    // aetherd RFC 2.3 (RadioModel residual): the space-free profile-flag decode
    // moved to FlexBackend::decodeProfileFlags → profileChanged → applyProfileChanges.
    Q_UNUSED(object);
    if (m_flexBackend) m_flexBackend->decodeProfileFlags(kvs);
}

void RadioModel::handleProfileStatusRaw(const QString& profileType,
                                         const QString& rawBody)
{
    // aetherd RFC 2.3 (RadioModel residual): the Flex "profile <type> …" wire
    // decode (space-containing list/current values, importing/exporting flags)
    // moved to FlexBackend::decodeProfileStatus → profileChanged →
    // applyProfileChanges. Thin forwarder behind the seam.
    if (m_flexBackend) m_flexBackend->decodeProfileStatus(profileType, rawBody);
}

void RadioModel::applyProfileChanges(const ProfileDelta& d)
{
    // Database import/export flags arrive without a profile type. They never
    // co-occur with a list/current in one delta (the backend emits either a
    // flags delta or a type/list-current delta), but a single flags kv-set may
    // carry both — apply each independently (change-gated), matching the old
    // handleProfileStatus's two separate ifs, then fall through to type routing.
    bool flagHandled = false;
    if (d.importing) {
        if (m_profileDatabaseImporting != *d.importing) {
            m_profileDatabaseImporting = *d.importing;
            emit profileDatabaseImportingChanged(*d.importing);
        }
        flagHandled = true;
    }
    if (d.exporting) {
        if (m_profileDatabaseExporting != *d.exporting) {
            m_profileDatabaseExporting = *d.exporting;
            emit profileDatabaseExportingChanged(*d.exporting);
        }
        flagHandled = true;
    }
    if (flagHandled) return;

    if (d.type == QLatin1String("tx")) {
        if (d.list) {
            m_transmitModel.setProfileList(*d.list);
            qCDebug(lcProtocol) << "RadioModel: TX profiles:" << *d.list;
        } else if (d.current) {
            m_transmitModel.setActiveProfile(*d.current);
            qCDebug(lcProtocol) << "RadioModel: active TX profile:" << *d.current;
        }
    } else if (d.type == QLatin1String("mic")) {
        if (d.list) {
            m_transmitModel.setMicProfileList(*d.list);
            qCDebug(lcProtocol) << "RadioModel: mic profiles:" << *d.list;
        } else if (d.current) {
            m_transmitModel.setActiveMicProfile(*d.current);
            qCDebug(lcProtocol) << "RadioModel: active mic profile:" << *d.current;
        }
    } else if (d.type == QLatin1String("global")) {
        if (d.list) {
            m_globalProfiles = *d.list;
            qCDebug(lcProtocol) << "RadioModel: global profiles:" << m_globalProfiles;
            emit globalProfilesChanged();
        } else if (d.current) {
            m_activeGlobalProfile = *d.current;
            qCDebug(lcProtocol) << "RadioModel: active global profile:" << *d.current;
            emit globalProfilesChanged();
        }
    }
}

void RadioModel::loadGlobalProfile(const QString& name)
{
    sendCmd(QString("profile global load \"%1\"").arg(name));
}

void RadioModel::resetPanState()
{
    setActivePanResized(false);
    setActiveWfConfigured(false);
}

void RadioModel::createAudioStream()
{
    // Remove old audio stream first, then create new one in the callback
    if (m_rxAudio.streamId != 0) {
        const quint32 oldId = m_rxAudio.streamId;
        m_rxAudio = {};
        sendCmd(
            QString("stream remove %1").arg(RadioStatusOwnership::hexId(oldId)),
            [this](int, const QString&) {
                // Old stream removed — now create the new one
                createRxAudioStream();
            });
    } else {
        createRxAudioStream();
    }
}

bool RadioModel::ensureDaxTxStream(DaxTxRequestReason reason)
{
    // A BACKEND THAT TAKES TX AUDIO OVER THE SEAM NEEDS NO DAX STREAM, and
    // asking for one is how TCI transmit died on an Icom: `stream create` went
    // to a radio with no Flex command plane, failed with 0x50000063, and every
    // caller read that as "transmit cannot proceed".
    //
    // TRUE, not false. The contract of this function is "is there a route for
    // transmit audio", and there is — AudioEngine's final-monitor tap into
    // IRadioBackend::submitTxAudio. False would be the same outage with a
    // tidier log.
    if (backendCapabilities().takesTxAudioOverSeam) {
        qCDebug(lcDax).noquote()
            << "RadioModel: DAX TX not needed — this backend takes transmit audio"
            << "over the seam"
            << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
        return true;
    }

    const DaxTxPolicyContext policyContext = currentDaxTxPolicyContext(reason);
    const DaxTxPolicyDecision decision = evaluateDaxTxPolicy(policyContext);
    const quint32 ourHandle = clientHandle();
    qCInfo(lcDax).noquote()
        << "RadioModel: DAX TX policy"
        << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason))
        << QStringLiteral("platform=%1").arg(daxTxPlatformName(policyContext.platform))
        << QStringLiteral("allowed=%1").arg(decision.allowed ? 1 : 0)
        << QStringLiteral("mode=%1").arg(daxTxModeName(policyContext.mode))
        << QStringLiteral("stream=%1").arg(m_daxTxStreamId != 0
            ? hexId(m_daxTxStreamId)
            : QStringLiteral("none"))
        << QStringLiteral("owner=%1").arg(m_daxTxClientHandle != 0
            ? hexId(m_daxTxClientHandle)
            : QStringLiteral("unknown"));

    if (!decision.allowed) {
        qCInfo(lcDax).noquote()
            << "RadioModel: DAX TX stream not created"
            << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason))
            << QStringLiteral("mode=%1").arg(daxTxModeName(policyContext.mode))
            << QStringLiteral("note=%1").arg(decision.note);
        return false;
    }

    const bool existingStreamIsOurs = m_daxTxStreamId != 0
        && (m_daxTxClientHandle == 0 || m_daxTxClientHandle == ourHandle);
    if (existingStreamIsOurs || m_daxTxCreatePending)
        return true;

    m_daxTxCreatePending = true;
    qCInfo(lcDax).noquote()
        << "RadioModel: DAX TX create requested"
        << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
    sendCmd(
        "stream create type=dax_tx",
        [this, reason](int code, const QString& body) {
            m_daxTxCreatePending = false;
            if (code != 0) {
                qCWarning(lcDax).noquote()
                    << "RadioModel: DAX TX create failed"
                    << QStringLiteral("code=%1").arg(hexCode(code))
                    << QStringLiteral("body=%1").arg(body)
                    << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
                return;
            }

            const quint32 id = RadioStatusOwnership::parseCreateResponseStreamId(body);
            if (id == 0) {
                qCWarning(lcDax).noquote()
                    << "RadioModel: DAX TX create failed"
                    << QStringLiteral("code=0x00000000")
                    << QStringLiteral("body=%1").arg(body)
                    << QStringLiteral("reason=%1").arg(daxTxRequestReasonName(reason));
                return;
            }

            const bool statusAlreadyAdoptedStream = m_daxTxStreamId == id;
            m_daxTxStreamId = id;
            // Stream status normally precedes the create reply. Preserve the
            // ownership bit if that status already adopted this exact stream.
            if (!statusAlreadyAdoptedStream) {
                m_daxTxActive = false;
            }
            m_daxTxClientHandle = clientHandle();
            qCInfo(lcDax).noquote()
                << "RadioModel: DAX TX create succeeded"
                << QStringLiteral("stream=%1").arg(hexId(id));
            emit txAudioStreamReady(id);
            if (m_wsprTxOwnershipRequested) {
                sendCmd(QStringLiteral("stream set %1 tx=1").arg(hexId(id)));
            } else if (m_wsprTxReleaseWhenReady) {
                sendCmd(QStringLiteral("stream set %1 tx=0").arg(hexId(id)));
                m_wsprTxReleaseWhenReady = false;
            }
        });
    return true;
}

bool RadioModel::prepareWsprTransmit()
{
    // Fail closed on an RX-only family before borrowing any station state. The
    // UI refuses earlier with an operator-visible reason; this is the backstop
    // so a future caller cannot reach the DAX/PTT path on a backend that has no
    // transmitter. Today it would fail anyway, but only implicitly — the Flex
    // `stream create` below would find no command sink (Principle VIII).
    if (!backendCapabilities().canTransmit) {
        return false;
    }
    // A seam-audio backend needs no Flex DAX stream. HL2 modulates locally and
    // Icom ships the PCM to the radio's own modulator; both paths converge at
    // txFinalMonitorPcmReady → submitTxAudio. There is nothing to create and
    // nothing to borrow, so this arm takes none of the Flex station state below.
    //
    // Nor does it need `transmit dax`: that setting exists to tell a FLEX to
    // take its modulator input from the DAX stream instead of the mic jacks,
    // and a radio with no on-radio modulator has no such choice to make. The
    // mic still has to be kept off the wire — two producers into one modulator
    // would put shack ambience on the WSPR frame — but AudioEngine::
    // startWsprPump() already does that with setDaxTxMode(true), which gates
    // onTxAudioReady() locally on every family.
    // Any backend whose transmit audio leaves through the seam, not only one
    // that modulates locally — the beacon reaches submitTxAudio either way.
    if (backendCapabilities().takesTxAudioOverSeam) {
        m_wsprTxSeamAudioArmed = true;
        return true;
    }
    // Every other non-Flex family: the beacon rides a Flex `dax_tx` stream and
    // nothing else provides one. Check before borrowing any station state —
    // ensureDaxTxStream() below issues `stream create` and returns true
    // optimistically on the pending reply, so on a backend whose command sink
    // drops that command the prepare would "succeed" with a stream that never
    // arrives, leaving `transmit dax` latched until the beacon times out and
    // releases it several minutes later.
    if (m_flexBackend == nullptr) {
        return false;
    }
    // WSPR is generated in-process and sent through our own dax_tx stream.
    // `transmit dax` is a station-wide setting the operator (or SmartSDR DAX2
    // on Windows, #2315) owns, so remember it and hand it back in
    // releaseWsprTransmit(). Leaving dax=1 latched would silently kill the
    // next mic voice TX on every platform where updateDaxTxMode() is compiled
    // out (Windows / Linux without PipeWire). Mirrors the AX.25 TX path.
    m_wsprTxPreviousDax = m_transmitModel.daxOn();
    m_wsprTxRestoreDax = true;
    m_transmitModel.setDax(true);
    m_wsprTxYieldAfterUse = !m_daxTxActive;
    m_wsprTxOwnershipRequested = true;
    m_wsprTxReleaseWhenReady = false;
    if (!ensureDaxTxStream(DaxTxRequestReason::WsprBeacon)) {
        m_wsprTxOwnershipRequested = false;
        m_wsprTxYieldAfterUse = false;
        restoreWsprTransmitDax();
        return false;
    }
    if (m_daxTxStreamId != 0) {
        sendCmd(QStringLiteral("stream set %1 tx=1")
                    .arg(hexId(m_daxTxStreamId)));
    }
    return true;
}

void RadioModel::releaseWsprTransmit()
{
    // Seam audio: nothing was borrowed, so nothing is handed back. Dropping
    // the latch is the whole release — and it must happen before the DAX arm so
    // a stale m_daxTxStreamId from an earlier Flex session in the same process
    // cannot make this path issue `stream set … tx=0` at a radio that has no
    // such stream.
    if (m_wsprTxSeamAudioArmed) {
        m_wsprTxSeamAudioArmed = false;
        return;
    }
    if (m_wsprTxOwnershipRequested && m_wsprTxYieldAfterUse) {
        if (m_daxTxStreamId != 0) {
            sendCmd(QStringLiteral("stream set %1 tx=0")
                        .arg(hexId(m_daxTxStreamId)));
        } else if (m_daxTxCreatePending) {
            m_wsprTxReleaseWhenReady = true;
        }
    }
    m_wsprTxOwnershipRequested = false;
    m_wsprTxYieldAfterUse = false;
    restoreWsprTransmitDax();
}

// Hand `transmit dax` back to whatever owned it before the beacon armed.
// Only writes when the beacon actually changed it, so an operator (or DAX2)
// that already had dax=1 never sees a redundant command.
void RadioModel::restoreWsprTransmitDax()
{
    if (!m_wsprTxRestoreDax)
        return;
    m_wsprTxRestoreDax = false;
    if (m_transmitModel.daxOn() != m_wsprTxPreviousDax)
        m_transmitModel.setDax(m_wsprTxPreviousDax);
}

QJsonObject RadioModel::troubleshootingSnapshot() const
{
    QJsonObject snapshot;
    snapshot["schema_version"] = 1;
    snapshot["captured_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    snapshot["captured_from"] = "AetherSDR in-memory application state";
    snapshot["note"] =
        "This snapshot is built from the app's cached radio, panadapter, slice, "
        "and meter models. It does not query the radio directly.";
    snapshot["privacy"] =
        "Sensitive identifiers are omitted by design, including radio name, "
        "nickname, callsign, serial numbers, MAC/IP addresses, GPS data, and "
        "client station names.";

    QJsonObject app;
    app["name"] = QCoreApplication::applicationName();
    app["version"] = QCoreApplication::applicationVersion();
    app["qt_version"] = qVersion();
    app["os"] = QSysInfo::prettyProductName();
    app["cpu_arch"] = QSysInfo::currentCpuArchitecture();
    snapshot["app"] = app;

    QJsonObject radio;
    radio["connected"] = isConnected();
    radio["transport"] = isWan() ? "WAN" : "LAN";
    radio["model"] = m_model;
    radio["software_version"] = m_version;
    radio["protocol_version"] = m_protocolVersion;
    radio["region"] = m_region;
    radio["radio_options"] = m_radioOptions;
    radio["max_slices"] = m_maxSlices;
    radio["full_duplex_enabled"] = m_fullDuplex;
    radio["binaural_rx"] = m_binauralRx;
    radio["mute_local_audio_when_remote"] = m_muteLocalWhenRemote;
    radio["low_latency_digital_modes"] = m_lowLatencyDigital;
    radio["enforce_private_ip_connections"] = m_enforcePrivateIp;
    radio["remote_on_enabled"] = m_remoteOnEnabled;
    radio["mf_enable"] = m_multiFlexEnabled;
    radio["rtty_mark_default"] = m_rttyMarkDefault;
    radio["antenna_list"] = toJsonArray(m_antList);
    radio["owned_slice_ids"] = toJsonArray(m_ownedSliceIds);
    radio["global_profile_count"] = m_globalProfiles.size();
    radio["active_global_profile_set"] = !m_activeGlobalProfile.trimmed().isEmpty();

    QJsonObject oscillator;
    oscillator["setting"] = m_oscSetting;
    oscillator["locked"] = m_oscLocked;
    oscillator["ext_present"] = m_extPresent;
    oscillator["tcxo_present"] = m_tcxoPresent;
    radio["oscillator"] = oscillator;

    QJsonObject audioOutputs;
    audioOutputs["lineout_gain"] = m_lineoutGain;
    audioOutputs["lineout_mute"] = m_lineoutMute;
    audioOutputs["headphone_gain"] = m_headphoneGain;
    audioOutputs["headphone_mute"] = m_headphoneMute;
    audioOutputs["front_speaker_mute"] = m_frontSpeakerMute;
    radio["audio_outputs"] = audioOutputs;

    const bool pcAudioSetting = AppSettings::instance().value("PcAudioEnabled", "True").toString() == "True";
    QJsonObject remoteAudioRx;
    remoteAudioRx["stream_id"] = m_rxAudio.streamId == 0
        ? QJsonValue()
        : QJsonValue(RadioStatusOwnership::hexId(m_rxAudio.streamId));
    remoteAudioRx["stream_id_known"] = m_rxAudio.streamId != 0;
    remoteAudioRx["create_pending"] = m_rxAudio.createPending;
    remoteAudioRx["remove_requested"] = m_rxAudio.removeRequested;
    remoteAudioRx["status_seen"] = m_rxAudio.statusSeen;
    remoteAudioRx["owner_known"] = m_rxAudio.clientHandle != 0;
    remoteAudioRx["owned_by_us"] = m_rxAudio.clientHandle != 0 && m_rxAudio.clientHandle == ourClientHandle();
    remoteAudioRx["compression"] = m_rxAudio.compression;
    remoteAudioRx["pc_audio_setting"] = pcAudioSetting;
    remoteAudioRx["stream_expected"] = pcAudioSetting;
    remoteAudioRx["routing_note"] = pcAudioSetting
        ? QStringLiteral("PC Audio is enabled; an owned remote_audio_rx stream should exist and the local RX sink should be running.")
        : QStringLiteral("PC Audio is disabled; no remote_audio_rx stream is expected. TCI clients route audio via DAX, not via remote_audio_rx (#1137).");
    radio["remote_audio_rx"] = remoteAudioRx;

    QJsonObject filterSharpness;
    filterSharpness["voice_level"] = m_filterVoice;
    filterSharpness["voice_auto"] = m_filterVoiceAuto;
    filterSharpness["cw_level"] = m_filterCw;
    filterSharpness["cw_auto"] = m_filterCwAuto;
    filterSharpness["digital_level"] = m_filterDigital;
    filterSharpness["digital_auto"] = m_filterDigitalAuto;
    radio["filter_sharpness"] = filterSharpness;

    QJsonObject amplifier;
    amplifier["present"] = m_amplifier.present();
    amplifier["handle"] = m_amplifier.handle();
    amplifier["model"] = m_amplifier.modelName();
    amplifier["operate"] = m_amplifier.operate();
    radio["amplifier"] = amplifier;

    QJsonObject ownership;
    ownership["tx_owned_by_us"] = m_txOwnedByUs;
    QJsonArray clients;
    QSet<quint32> seenHandles;
    const quint32 ourHandle = ourClientHandle();
    for (auto it = m_clientStations.cbegin(); it != m_clientStations.cend(); ++it) {
        clients.append(clientInfoToJson(it.key(), ourHandle, m_txClientHandle,
                                        m_clientInfoMap.value(it.key())));
        seenHandles.insert(it.key());
    }
    for (auto it = m_clientInfoMap.cbegin(); it != m_clientInfoMap.cend(); ++it) {
        if (seenHandles.contains(it.key()))
            continue;
        clients.append(clientInfoToJson(it.key(), ourHandle, m_txClientHandle,
                                        it.value()));
    }
    ownership["clients"] = clients;
    ownership["client_count"] = clients.size();
    ownership["multiple_clients_present"] = clients.size() > 1;
    radio["ownership"] = ownership;

    auto categoryStatsToJson = [this](PanadapterStream::StreamCategory cat) {
        const auto stats = categoryStats(cat);
        QJsonObject obj;
        obj["bytes"] = static_cast<qint64>(stats.bytes);
        obj["packets"] = stats.packets;
        obj["errors"] = stats.errors;
        return obj;
    };

    QJsonObject network;
    network["quality"] = networkQuality();
    network["last_ping_rtt_ms"] = m_lastPingRtt;
    network["max_ping_rtt_ms"] = m_maxPingRtt;
    network["packet_drop_count"] = packetDropCount();
    network["packet_total_count"] = packetTotalCount();
    network["packet_loss_window_seconds"] = packetLossWindowSeconds();
    network["packet_loss_window_drops"] = packetLossWindowDrops();
    network["packet_loss_window_packets"] = packetLossWindowPackets();
    network["packet_loss_window_percent"] = packetLossPercent();
    network["audio_packet_gap_ms"] = audioPacketGapMs();
    network["audio_packet_gap_max_ms"] = audioPacketGapMaxMs();
    network["audio_packet_jitter_ms"] = audioPacketJitterMs();
    network["rx_bytes"] = static_cast<qint64>(rxBytes());
    network["tx_bytes"] = static_cast<qint64>(txBytes());
    // So a test can tell "the RTT is zero" from "this transport has no round
    // trip to time" — the two are indistinguishable in last_ping_rtt_ms alone,
    // and an assertion that reads it without this would pass on a link nothing
    // measured. Same question the GUI readouts ask before printing "< 1 ms".
    network["rtt_measured"] = hasLinkRtt();
    network["timing_measured"] = hasLinkTiming();
    network["stream_categories_measured"] = hasStreamCategoryStats();
    network["source"] = usesBackendLinkStats() ? QStringLiteral("backend_link")
                                               : QStringLiteral("vita49_stream");
    QJsonObject streamCategories;
    streamCategories["audio"] = categoryStatsToJson(PanadapterStream::CatAudio);
    streamCategories["fft"] = categoryStatsToJson(PanadapterStream::CatFFT);
    streamCategories["waterfall"] = categoryStatsToJson(PanadapterStream::CatWaterfall);
    streamCategories["meter"] = categoryStatsToJson(PanadapterStream::CatMeter);
    streamCategories["dax"] = categoryStatsToJson(PanadapterStream::CatDAX);
    network["stream_categories"] = streamCategories;
    radio["network"] = network;

    QJsonObject telemetry;
    telemetry["pa_temp_c"] = m_meterModel.paTemp();
    telemetry["supply_volts"] = m_meterModel.supplyVolts();
    telemetry["tx_forward_power_w"] = m_meterModel.fwdPower();
    // Null rather than a leftover ratio when the TX meters are stale — this
    // snapshot feeds support bundles, and a stale SWR reads as a live antenna
    // fault to whoever opens the report (#4533).
    if (const auto liveSwr = m_meterModel.swrIfLive())
        telemetry["tx_swr"] = *liveSwr;
    else
        telemetry["tx_swr"] = QJsonValue();
    // Whether the TX meter group above is current, so the whole group is
    // interpretable at once.
    //
    // tx_forward_power_w is NOT nulled alongside tx_swr, and the asymmetry is
    // deliberate: forward power is a measured quantity that decays toward zero
    // and reads zero when the carrier drops, so its last value is meaningful on
    // its own. SWR is a RATIO derived from it, and a ratio computed from
    // quantities that are no longer arriving is not a smaller number — it is
    // undefined, which is why it goes null. Without this flag the pair
    // `tx_forward_power_w: 5.0, tx_swr: null` invites the reader to conclude
    // forward power is live and only SWR is missing. (#4533 review)
    telemetry["tx_meters_fresh"] =
        m_meterModel.hasRecentTxMeters(MeterModel::kTxMeterStaleMs);
    // -1 when no TX meter has ever arrived, matching the age convention the
    // automation bridge's meters snapshot already uses.
    const qint64 txMetersAtMs = m_meterModel.txMetersUpdatedAtMs();
    telemetry["tx_meters_age_ms"] =
        txMetersAtMs > 0 ? QDateTime::currentMSecsSinceEpoch() - txMetersAtMs : -1;
    // SliceTroubleshootingDialog renders this with the literal label
    // "HWALC", so keep it pointed at the external Hardware ALC RCA voltage
    // (m_hwAlc) — the gauge in the Phone/CW applet now uses swAlc().
    telemetry["alc"] = m_meterModel.hwAlc();
    telemetry["mic_level_dbfs"] = m_meterModel.micLevel();
    telemetry["mic_peak_dbfs"] = m_meterModel.micPeak();
    telemetry["comp_level_db"] = m_meterModel.compLevel();
    telemetry["comp_peak_db"] = m_meterModel.compPeak();
    radio["telemetry"] = telemetry;

    snapshot["radio"] = radio;

    QJsonObject transmit;

    QJsonObject txPower;
    txPower["rf_power"] = m_transmitModel.rfPower();
    txPower["tune_power"] = m_transmitModel.tunePower();
    txPower["max_power_level"] = m_transmitModel.maxPowerLevel();
    txPower["tune_mode"] = m_transmitModel.tuneMode();
    txPower["show_tx_in_waterfall"] = m_transmitModel.showTxInWaterfall();
    txPower["tuning"] = m_transmitModel.isTuning();
    txPower["mox"] = m_transmitModel.isMox();
    txPower["transmitting"] = m_transmitModel.isTransmitting();
    transmit["power"] = txPower;

    QJsonObject mic;
    mic["selection"] = m_transmitModel.micSelection();
    mic["level"] = m_transmitModel.micLevel();
    mic["mic_acc"] = m_transmitModel.micAcc();
    mic["speech_processor_enable"] = m_transmitModel.speechProcessorEnable();
    mic["speech_processor_level"] = m_transmitModel.speechProcessorLevel();
    mic["compander_on"] = m_transmitModel.companderOn();
    mic["compander_level"] = m_transmitModel.companderLevel();
    mic["dax_on"] = m_transmitModel.daxOn();
    mic["sb_monitor"] = m_transmitModel.sbMonitor();
    mic["mon_gain_sb"] = m_transmitModel.monGainSb();
    mic["mic_boost"] = m_transmitModel.micBoost();
    mic["mic_bias"] = m_transmitModel.micBias();
    mic["met_in_rx"] = m_transmitModel.metInRx();
    mic["sync_cwx"] = m_transmitModel.syncCwx();
    mic["am_carrier_level"] = m_transmitModel.amCarrierLevel();
    mic["dexp_on"] = m_transmitModel.dexpOn();
    mic["dexp_level"] = m_transmitModel.dexpLevel();
    mic["tx_filter_low"] = m_transmitModel.txFilterLow();
    mic["tx_filter_high"] = m_transmitModel.txFilterHigh();
    transmit["mic"] = mic;

    QJsonObject vox;
    vox["enabled"] = m_transmitModel.voxEnable();
    vox["level"] = m_transmitModel.voxLevel();
    vox["delay"] = m_transmitModel.voxDelay();
    transmit["vox"] = vox;

    QJsonObject cw;
    cw["speed_wpm"] = m_transmitModel.cwSpeed();
    cw["pitch_hz"] = m_transmitModel.cwPitch();
    cw["break_in"] = m_transmitModel.cwBreakIn();
    cw["delay_ms"] = m_transmitModel.cwDelay();
    cw["sidetone"] = m_transmitModel.cwSidetone();
    cw["iambic"] = m_transmitModel.cwIambic();
    cw["iambic_mode"] = m_transmitModel.cwIambicMode();
    cw["swap_paddles"] = m_transmitModel.cwSwapPaddles();
    cw["cwl_enabled"] = m_transmitModel.cwlEnabled();
    cw["monitor_gain"] = m_transmitModel.monGainCw();
    transmit["cw"] = cw;

    QJsonObject interlock;
    interlock["acc_tx_delay"] = m_transmitModel.accTxDelay();
    interlock["tx1_delay"] = m_transmitModel.tx1Delay();
    interlock["tx2_delay"] = m_transmitModel.tx2Delay();
    interlock["tx3_delay"] = m_transmitModel.tx3Delay();
    interlock["tx_delay"] = m_transmitModel.txDelay();
    interlock["timeout"] = m_transmitModel.interlockTimeout();
    interlock["acc_tx_req_polarity"] = m_transmitModel.accTxReqPolarity();
    interlock["rca_tx_req_polarity"] = m_transmitModel.rcaTxReqPolarity();
    transmit["interlock"] = interlock;

    QJsonObject atu;
    atu["enabled"] = m_transmitModel.atuEnabled();
    atu["status"] = atuStatusToString(m_transmitModel.atuStatus());
    atu["memories_enabled"] = m_transmitModel.memoriesEnabled();
    atu["using_memory"] = m_transmitModel.usingMemory();
    transmit["atu"] = atu;

    QJsonObject apd;
    apd["enabled"] = m_transmitModel.apdEnabled();
    apd["configurable"] = m_transmitModel.apdConfigurable();
    apd["equalizer_active"] = m_transmitModel.apdEqualizerActive();
    transmit["apd"] = apd;

    QJsonObject profiles;
    profiles["tx_profile_count"] = m_transmitModel.profileList().size();
    profiles["active_tx_profile_set"] = !m_transmitModel.activeProfile().trimmed().isEmpty();
    profiles["mic_profile_count"] = m_transmitModel.micProfileList().size();
    profiles["active_mic_profile_set"] = !m_transmitModel.activeMicProfile().trimmed().isEmpty();
    profiles["mic_inputs"] = toJsonArray(m_transmitModel.micInputList());
    transmit["profiles"] = profiles;

    snapshot["transmit"] = transmit;

    QJsonArray panadapters;
    for (auto it = m_panadapters.cbegin(); it != m_panadapters.cend(); ++it)
        panadapters.append(panToJson(it.value(), m_activePanId));
    snapshot["panadapters"] = panadapters;

    QJsonArray xvtrs;
    for (auto it = m_xvtrList.cbegin(); it != m_xvtrList.cend(); ++it)
        xvtrs.append(xvtrToJson(it.value()));
    snapshot["xvtrs"] = xvtrs;

    auto txBandInfoToJson = [](const TxBandInfo& band) {
        QJsonObject obj;
        obj["band_id"] = band.bandId;
        obj["band_name"] = band.bandName;
        obj["rf_power"] = band.rfPower;
        obj["tune_power"] = band.tunePower;
        obj["inhibit"] = band.inhibit;
        obj["hw_alc"] = band.hwAlc;
        obj["acc_tx_req"] = band.accTxReq;
        obj["rca_tx_req"] = band.rcaTxReq;
        obj["acc_tx"] = band.accTx;
        obj["tx1"] = band.tx1;
        obj["tx2"] = band.tx2;
        obj["tx3"] = band.tx3;
        return obj;
    };

    QJsonArray txBands;
    for (auto it = m_txBandSettings.cbegin(); it != m_txBandSettings.cend(); ++it)
        txBands.append(txBandInfoToJson(it.value()));
    snapshot["tx_band_settings"] = txBands;

    QJsonArray allMeters = m_meterModel.allMeters();
    QJsonArray globalMeters;
    int sliceMeterCount = 0;
    for (const QJsonValue& value : allMeters) {
        const QJsonObject meter = value.toObject();
        if (meter["source"].toString() == "SLC")
            ++sliceMeterCount;
        else
            globalMeters.append(meter);
    }
    snapshot["global_meters"] = globalMeters;

    QList<SliceModel*> sortedSlices = m_slices;
    std::sort(sortedSlices.begin(), sortedSlices.end(), [](SliceModel* lhs, SliceModel* rhs) {
        return lhs->sliceId() < rhs->sliceId();
    });

    QJsonArray slices;
    for (SliceModel* sliceModel : sortedSlices) {
        QJsonObject slice;
        slice["slice_id"] = sliceModel->sliceId();
        slice["pan_id"] = sliceModel->panId();
        slice["frequency_mhz"] = sliceModel->frequency();
        slice["mode"] = sliceModel->mode();
        slice["mode_list"] = toJsonArray(sliceModel->modeList());
        slice["active"] = sliceModel->isActive();
        slice["tx_slice"] = sliceModel->isTxSlice();

        QJsonObject filter;
        filter["low_hz"] = sliceModel->filterLow();
        filter["high_hz"] = sliceModel->filterHigh();
        slice["filter"] = filter;

        QJsonObject audio;
        audio["gain"] = sliceModel->audioGain();
        audio["pan"] = sliceModel->audioPan();
        audio["mute"] = sliceModel->audioMute();
        slice["audio"] = audio;

        slice["rf_gain"] = sliceModel->rfGain();

        QJsonObject antennas;
        antennas["rx"] = sliceModel->rxAntenna();
        antennas["tx"] = sliceModel->txAntenna();
        slice["antennas"] = antennas;

        QJsonObject control;
        control["locked"] = sliceModel->isLocked();
        control["qsk"] = sliceModel->qskOn();
        control["record_on"] = sliceModel->recordOn();
        control["play_on"] = sliceModel->playOn();
        control["play_enabled"] = sliceModel->playEnabled();
        slice["control"] = control;

        QJsonObject dsp;
        dsp["agc_mode"] = sliceModel->agcMode();
        dsp["agc_threshold"] = sliceModel->agcThreshold();
        dsp["nb"] = QJsonObject{{"enabled", sliceModel->nbOn()}, {"level", sliceModel->nbLevel()}};
        dsp["nr"] = QJsonObject{{"enabled", sliceModel->nrOn()}, {"level", sliceModel->nrLevel()}};
        dsp["anf"] = QJsonObject{{"enabled", sliceModel->anfOn()}, {"level", sliceModel->anfLevel()}};
        dsp["lms_nr"] = QJsonObject{{"enabled", sliceModel->nrlOn()}, {"level", sliceModel->nrlLevel()}};
        dsp["speex_nr"] = QJsonObject{{"enabled", sliceModel->nrsOn()}, {"level", sliceModel->nrsLevel()}};
        dsp["rnnoise"] = sliceModel->rnnOn();
        dsp["nrf"] = QJsonObject{{"enabled", sliceModel->nrfOn()}, {"level", sliceModel->nrfLevel()}};
        dsp["lms_anf"] = QJsonObject{{"enabled", sliceModel->anflOn()}, {"level", sliceModel->anflLevel()}};
        dsp["anft"] = sliceModel->anftOn();
        dsp["apf"] = QJsonObject{{"enabled", sliceModel->apfOn()}, {"level", sliceModel->apfLevel()}};
        slice["dsp"] = dsp;

        QJsonObject diversity;
        diversity["enabled"] = sliceModel->diversity();
        diversity["is_parent"] = sliceModel->isDiversityParent();
        diversity["is_child"] = sliceModel->isDiversityChild();
        diversity["index"] = sliceModel->diversityIndex();
        diversity["esc_enabled"] = sliceModel->escEnabled();
        diversity["esc_gain"] = sliceModel->escGain();
        diversity["esc_phase_shift_deg"] = sliceModel->escPhaseShift();
        slice["diversity"] = diversity;

        QJsonObject tuning;
        tuning["squelch_on"] = sliceModel->squelchOn();
        tuning["squelch_level"] = sliceModel->squelchLevel();
        tuning["rit_on"] = sliceModel->ritOn();
        tuning["rit_hz"] = sliceModel->ritFreq();
        tuning["xit_on"] = sliceModel->xitOn();
        tuning["xit_hz"] = sliceModel->xitFreq();
        tuning["step_hz"] = sliceModel->stepHz();
        tuning["step_list"] = toJsonArray(sliceModel->stepList());
        slice["tuning"] = tuning;

        QJsonObject digital;
        digital["dax_channel"] = sliceModel->daxChannel();
        digital["rtty_mark_hz"] = sliceModel->rttyMark();
        digital["rtty_shift_hz"] = sliceModel->rttyShift();
        digital["digl_offset_hz"] = sliceModel->diglOffset();
        digital["digu_offset_hz"] = sliceModel->diguOffset();
        slice["digital"] = digital;

        QJsonObject fm;
        fm["tone_mode"] = sliceModel->fmToneMode();
        fm["tone_value"] = sliceModel->fmToneValue();
        fm["repeater_offset_dir"] = sliceModel->repeaterOffsetDir();
        fm["repeater_offset_mhz"] = sliceModel->fmRepeaterOffsetFreq();
        fm["tx_offset_mhz"] = sliceModel->txOffsetFreq();
        fm["deviation_hz"] = sliceModel->fmDeviation();
        slice["fm"] = fm;

        PanadapterModel* pan = panadapter(sliceModel->panId());
        slice["panadapter_connection_status"] =
            slicePanadapterConnectionStatus(sliceModel->sliceId(),
                                            sliceModel->panId(),
                                            pan != nullptr,
                                            pan && pan->panId() == m_activePanId);
        if (pan)
            slice["panadapter_state"] = panToJson(pan, m_activePanId);

        slice["meters"] = m_meterModel.metersForSource("SLC", sliceModel->sliceId());
        slices.append(slice);
    }
    snapshot["slices"] = slices;

    QJsonArray annotatedPanadapters;
    for (const QJsonValue& value : panadapters) {
        QJsonObject pan = value.toObject();
        pan["slice_connection_status"] = panSliceConnectionStatus(pan, slices);
        annotatedPanadapters.append(pan);
    }
    panadapters = annotatedPanadapters;
    snapshot["panadapters"] = panadapters;

    QJsonObject counts;
    counts["panadapters"] = panadapters.size();
    counts["slices"] = slices.size();
    counts["meters_total"] = allMeters.size();
    counts["global_meters"] = globalMeters.size();
    counts["slice_meters"] = sliceMeterCount;
    snapshot["counts"] = counts;

    return snapshot;
}

bool RadioModel::acquireDaxChannel(int channel, PanadapterStream::DaxConsumer who)
{
    if (!m_panStream) {
        qCDebug(lcDax) << "RadioModel: no PanadapterStream — declining DAX channel"
                       << channel << "for consumer" << static_cast<int>(who);
        return false;
    }
    m_panStream->acquireDaxChannel(channel, who);
    return true;
}

void RadioModel::releaseDaxChannel(int channel, PanadapterStream::DaxConsumer who)
{
    if (!m_panStream)
        return;                       // nothing was ever held
    m_panStream->releaseDaxChannel(channel, who);
}

void RadioModel::releaseAllDaxChannels(PanadapterStream::DaxConsumer who)
{
    if (!m_panStream)
        return;
    m_panStream->releaseAllDaxChannels(who);
}

} // namespace AetherSDR
