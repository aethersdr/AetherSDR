// The Flex status decoders: wire lines to typed deltas, run by the status
// ingress on its owner's thread for every accepted line (FlexStatusIngress.h).
// Moved from FlexBackend / FlexNtpSourceControls unchanged.
#include "FlexStatusIngress.h"
#include "FlexDisplayStatus.h"

#include "core/LogManager.h"
#include "core/backends/MemoryWireCodec.h"
#include "core/backends/flex/FlexCwControls.h"
#include "core/backends/flex/FlexCwxStatus.h"
#include "core/backends/flex/FlexGraphicEqStatus.h"
#include "core/backends/flex/FlexWaveformStatus.h"
#include "core/backends/flex/FlexKvCarry.h"
#include "core/backends/TuneOutputPolicy.h"
#include "core/backends/flex/FlexStreamStatus.h"
#include "models/RadioStatusOwnership.h"

#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace AetherSDR {

using namespace flexkv;

namespace {

// FlexLib NtpSource.cs SplitStatusTokens: space-separated, a double-quoted
// value (description="two words") is one token, and the quotes are dropped.
QStringList splitStatusTokens(const QString& body)
{
    QStringList tokens;
    QString current;
    bool quoted = false;
    for (const QChar ch : body) {
        if (ch == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (ch == QLatin1Char(' ') && !quoted) {
            if (!current.isEmpty()) { tokens.append(current); current.clear(); }
        } else {
            current.append(ch);
        }
    }
    if (!current.isEmpty()) { tokens.append(current); }
    return tokens;
}

std::optional<bool> parseNtpFlag(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    if (!ok || v < 0 || v > 255) { return std::nullopt; }
    return v == 1;
}

std::optional<int> parseNtpInt(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    return ok ? std::optional<int>(v) : std::nullopt;
}


std::optional<quint32> daxTxIdentity(const QString& text)
{
    if (text.isEmpty() || text.size() > 10) {
        return std::nullopt;
    }
    const bool prefixed = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive);
    const qsizetype first = prefixed ? 2 : 0;
    if (first == text.size()) {
        return std::nullopt;
    }
    bool hexDigit = false;
    for (qsizetype i = first; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) {
            continue;
        }
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'))) {
            hexDigit = true;
            continue;
        }
        return std::nullopt;
    }
    bool ok = false;
    const quint32 value = text.mid(first).toUInt(&ok, prefixed || hexDigit ? 16 : 10);
    return ok && value != 0 ? std::optional<quint32>(value) : std::nullopt;
}

} // namespace

void FlexStatusIngress::decodeLine(const ParsedMessage& line)
{
    if (line.type != MessageType::Status) { return; }
    // Display identity, ownership and pairing are lifecycle, which the Demo
    // needs too; SimBackend drops the values its native producer owns.
    if (line.object.startsWith(QLatin1String("display "))) {
        if (auto display = decodeFlexDisplayStatus(line.object, line.kvs)) {
            display->sourceHandle = line.handle;
            display->statusEpoch = m_epoch;
            emit displayObserved(*display);
        }
        return;
    }
    if (!m_decodeValues) {
        // Identity only (SimBackend): a TGXL's handle, which replayed captured
        // status relies on; nothing else.
        if (line.object.startsWith(QLatin1String("amplifier"))) { routeAmplifierStatus(line.object, line.kvs); }
        return;
    }
    // Raw route: objects whose values carry spaces or '#' separators, read
    // from the line after its S<handle>| prefix.
    const int pipe = line.raw.indexOf(QLatin1Char('|'));
    const QString body = pipe < 0 ? QString() : line.raw.mid(pipe + 1);
    static const struct { const char* prefix; const char* type; } profiles[] = {
        {"profile tx ", "tx"}, {"profile mic ", "mic"},
        {"profile global ", "global"}, {"profile cw ", "cw"}};
    for (const auto& profile : profiles) {
        if (body.startsWith(QLatin1String(profile.prefix))) {
            // A typed profile line is decoded once, flags included.
            decodeProfileStatus(QString::fromLatin1(profile.type),
                                body.mid(int(qstrlen(profile.prefix))));
            return;
        }
    }
    if (pipe >= 0 && decodeRawStatus(body)) { return; }
    if (body.startsWith(QLatin1String("gps "))) { decodeGpsStatus(body.mid(4)); return; }
    if (body.startsWith(QLatin1String("meter "))) { decodeMeterStatus(body.mid(6)); return; }

    // Key/value route.
    const QString& object = line.object;
    const QMap<QString, QString>& kvs = line.kvs;
    // Stream lifecycle first, for every line, as the model saw it before:
    // audio streams, DAX TX ownership, NetCW removal.
    const QPointer<FlexStatusIngress> alive(this);
    decodeStreamStatus(object, kvs);
    if (!alive) { return; }
    if (const auto stream = flexStreamStatus(object, kvs)) {
        emit streamStatusObserved(*stream);
        return;
    }
    if (object.startsWith(QLatin1String("atu"))) {
        decodeAtuStatus(kvs);   // the radio's own ATU → TransmitModel
        if (!alive) { return; }
        // An external TGXL also reports through atu (#4092/#4198): the handle
        // it was last named, else the one this object names. Without either,
        // RadioModel applies it only while a tuner is present (a direct TGXL
        // connection counts).
        static const QRegularExpression atuRe(QStringLiteral(R"(^atu\s+(\S+)$)"));
        QString tunerHandle = m_tunerHandle;
        if (const auto m = atuRe.match(object); m.hasMatch() && tunerHandle.isEmpty()) { tunerHandle = m.captured(1); }
        if (!tunerHandle.isEmpty()) {
            decodeTunerStatus(tunerHandle, kvs);
        } else {
            emit unaddressedTunerStatus(tunerStatus(QString(), kvs));
        }
        return;
    }
    if (object.startsWith(QLatin1String("amplifier"))) { routeAmplifierStatus(object, kvs); return; }
    // "apd sampler …" is its own object; any other leading bare words
    // ("apd equalizer_reset") are flags the parser folded into the object.
    if (object == QLatin1String("apd sampler")) { decodeApdSamplerStatus(kvs); return; }
    if (object == QLatin1String("apd") || object.startsWith(QLatin1String("apd "))) {
        QMap<QString, QString> merged = kvs;
        if (object.length() > 3) {
            const QStringList flags = object.mid(4).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (const auto& f : flags) { merged.insert(f, QString{}); }
        }
        decodeApdStatus(merged);
        return;
    }
    if (object == QLatin1String("transmit")) { decodeTransmitStatus(kvs); return; }
    if (object == QLatin1String("interlock")) { emit interlockStatusObserved(interlockStatus(kvs)); return; }
    static const QRegularExpression txBandRe(QStringLiteral(R"(^transmit band\s+(\d+)$)"));
    if (object.startsWith(QLatin1String("transmit band"))) {
        if (const auto m = txBandRe.match(object); m.hasMatch()) { decodeTxBandStatus(m.captured(1).toInt(), kvs); }
        return;
    }
    static const QRegularExpression interlockBandRe(QStringLiteral(R"(^interlock band\s+(\d+)$)"));
    if (object.startsWith(QLatin1String("interlock band"))) {
        if (const auto m = interlockBandRe.match(object); m.hasMatch()) {
            decodeInterlockBandStatus(m.captured(1).toInt(), kvs);
            if (!alive) { return; }
        }
        decodeTuneOutputStatus(object, kvs);
        return;
    }
    // Untyped profile objects: the space-free flags ("profile importing=1").
    if (object.startsWith(QLatin1String("profile"))) { decodeProfileFlags(kvs); return; }
    if (const auto memory = memoryStatus(object, kvs)) { decodeMemoryStatus(memory->first, memory->second); return; }
    if (object == QLatin1String("cwx")) { emit cwxStatusObserved(flexCwxStatus(kvs)); return; }
    if (object == QLatin1String("waveform") || object.startsWith(QLatin1String("waveform "))) {
        if (const auto waveform = flexWaveformStatus(object, kvs)) { emit waveformStatusObserved(*waveform); }
        return;
    }
    if (object == QLatin1String("eq txsc") || object == QLatin1String("eq rxsc")) {
        emit graphicEqStatusObserved(flexGraphicEqStatus(object == QLatin1String("eq txsc")
            ? GraphicEqRequest::Path::Transmit : GraphicEqRequest::Path::Receive, kvs));
        return;
    }
    // "spot N …", bare "spot N removed", or "spot N triggered pan=…".
    if (object.startsWith(QLatin1String("spot "))) {
        static const QRegularExpression spotRe(QStringLiteral(R"(^spot\s+(\d+))"));
        if (const auto m = spotRe.match(object); m.hasMatch()) {
            SpotStatusDelta spot;
            spot.index = m.captured(1).toInt();
            if (kvs.isEmpty() && object.contains(QLatin1String("removed"))) {
                spot.kind = SpotStatusDelta::Kind::Removed;
            } else if (object.contains(QLatin1String("triggered"))) {
                spot.kind = SpotStatusDelta::Kind::Triggered;
                spot.pan = kvs.value(QStringLiteral("pan"));
            } else {
                spot.record = kvs;
            }
            emit spotStatusObserved(spot);
        }
        return;
    }
    // "tnf N …"; a removal arrives bare ("tnf N removed") or as removed=1.
    static const QRegularExpression tnfRe(QStringLiteral(R"(^tnf\s+(\d+)(?:\s+removed)?$)"));
    if (const auto m = tnfRe.match(object); m.hasMatch()) {
        decodeTnfStatus(m.captured(1).toInt(),
                        kvs.contains(QStringLiteral("removed")) || object.endsWith(QLatin1String("removed")), kvs);
        return;
    }
    if (const auto update = fileUpdateStatus(object, kvs)) { emit fileUpdateReported(*update); }
}

std::optional<FirmwareUpdateStatus>
FlexStatusIngress::fileUpdateStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    if (object != QLatin1String("file update")) { return std::nullopt; }
    FirmwareUpdateStatus status;
    // FlexLib treats a parseable failed as terminal (Radio.cs:12626-12630).
    bool parsed = false;
    const int failed = kvs.value(QStringLiteral("failed")).toInt(&parsed);
    if (parsed && (failed == 0 || failed == 1)) { status.failed = failed == 1; }
    status.reason = kvs.value(QStringLiteral("reason")).trimmed();
    const double transfer = kvs.value(QStringLiteral("transfer")).toDouble(&parsed);
    if (parsed && std::isfinite(transfer) && transfer >= 0.0 && transfer <= 1.0) { status.transfer = transfer; }
    return status;
}

std::optional<std::pair<int, QMap<QString, QString>>>
FlexStatusIngress::memoryStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    // "memory 7" or "memory 7 removed": trailing bare words are folded in.
    if (!object.startsWith(QLatin1String("memory "))) { return std::nullopt; }
    const QString rest = object.mid(7);
    const int sp = rest.indexOf(QLatin1Char(' '));
    bool ok = false;
    const int index = ((sp >= 0) ? rest.left(sp) : rest).toInt(&ok);
    if (!ok) { return std::nullopt; }
    QMap<QString, QString> merged = kvs;
    if (sp >= 0) {
        for (const auto& token : rest.mid(sp + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const int eq = token.indexOf(QLatin1Char('='));
            if (eq < 0) { merged.insert(token, QString{}); }
            else { merged.insert(token.left(eq), token.mid(eq + 1)); }
        }
    }
    return std::make_pair(index, merged);
}

// ── Transmit-family decoders (aetherd RFC 2.3 — TransmitModel touchpoint) ──
// Each translates its Flex status plane into the typed TransmitDelta and emits
// transmitChanged. Numeric parses are ok-guarded (malformed present field is
// dropped, not applied as 0) and clamped to the model's ranges — the wire
// normalization the old TransmitModel decoders did inline.
void FlexStatusIngress::decodeTransmitStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    // Core transmit
    carryClamp(kvs, "rfpower", d.rfPower, 0, 100);
    carryClamp(kvs, "tunepower", d.tunePower, 0, 100);
    carry(kvs, "tune", d.tune);
    carry(kvs, "mox", d.mox);
    carry(kvs, "freq", d.transmitFreq);

    // Mic / monitor / processor
    if (kvs.contains(QStringLiteral("mic_selection")))
        d.micSelection = kvs.value(QStringLiteral("mic_selection")).toUpper();
    carryClamp(kvs, "mic_level", d.micLevel, 0, 100);
    carry(kvs, "mic_acc", d.micAcc);
    carry(kvs, "speech_processor_enable", d.speechProcEnable);
    carryClamp(kvs, "speech_processor_level", d.speechProcLevel, 0, 100);
    carry(kvs, "compander", d.compander);
    carryClamp(kvs, "compander_level", d.companderLevel, 0, 100);
    carry(kvs, "dax", d.dax);
    carry(kvs, "sb_monitor", d.sbMonitor);
    carryClamp(kvs, "mon_gain_sb", d.monGainSb, 0, 100);

    // VOX / phone
    carry(kvs, "vox_enable", d.voxEnable);
    carryClamp(kvs, "vox_level", d.voxLevel, 0, 100);
    carryClamp(kvs, "vox_delay", d.voxDelay, 0, 100);
    carry(kvs, "mic_boost", d.micBoost);
    carry(kvs, "mic_bias", d.micBias);
    carry(kvs, "met_in_rx", d.metInRx);
    // FlexLib 4.3.16.42034 rejects anything but 0/1 (Radio.cs:11151-11163):
    // a malformed value must not confirm "off" or mark the key as reported.
    if (const std::optional<int> v = parseInt(kvs, "ptt_mic_override"); v && (*v == 0 || *v == 1)) {
        d.pttMicOverride = (*v == 1);
    }
    carry(kvs, "synccwx", d.syncCwx);
    carryClamp(kvs, "am_carrier_level", d.amCarrierLevel, 0, 100);
    // dexp / noise_gate_level alias compander / compander_level, but only when
    // the compander key itself is absent (the wire sends one or the other).
    if (kvs.contains(QStringLiteral("dexp")) && !kvs.contains(QStringLiteral("compander")))
        d.compander = kvs.value(QStringLiteral("dexp")) == QLatin1String("1");
    if (kvs.contains(QStringLiteral("noise_gate_level"))
        && !kvs.contains(QStringLiteral("compander_level"))) {
        bool ok = false;
        const int v = kvs.value(QStringLiteral("noise_gate_level")).toInt(&ok);
        if (ok) d.companderLevel = qBound(0, v, 100);
    }
    carryClamp(kvs, "lo", d.txFilterLow, 0, 10000);
    carryClamp(kvs, "hi", d.txFilterHigh, 0, 10000);

    // CW
    carryClamp(kvs, "speed", d.cwSpeed, 5, 100);
    carryClamp(kvs, "pitch", d.cwPitch, 100, 6000);
    carry(kvs, "break_in", d.cwBreakIn);
    carryClamp(kvs, "break_in_delay", d.cwDelay, 0, 2000);
    carry(kvs, "sidetone", d.cwSidetone);
    carry(kvs, "iambic", d.cwIambic);
    carryClamp(kvs, "iambic_mode", d.cwIambicMode, 0, 1);
    carry(kvs, "swap_paddles", d.cwSwapPaddles);
    carry(kvs, "cwl_enabled", d.cwlEnabled);
    carryClamp(kvs, "mon_gain_cw", d.monGainCw, 0, 100);
    carryClamp(kvs, "mon_pan_cw", d.monPanCw, 0, 100);

    // Misc TX
    carry(kvs, "max_power_level", d.maxPowerLevel);
    if (kvs.contains(QStringLiteral("tune_mode")))
        d.tuneMode = kvs.value(QStringLiteral("tune_mode"));
    carry(kvs, "show_tx_in_waterfall", d.showTxInWaterfall);
    if (kvs.contains(QStringLiteral("tx_slice_mode")))
        d.txSliceMode = kvs.value(QStringLiteral("tx_slice_mode"));

    emit transmitChanged(d);
}
void FlexStatusIngress::decodeAtuStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    // Raw ATU status token — the model owns the ATUStatus enum + parse.
    if (kvs.contains(QStringLiteral("status")))
        d.atuStatusRaw = kvs.value(QStringLiteral("status"));
    carry(kvs, "atu_enabled", d.atuEnabled);
    carry(kvs, "memories_enabled", d.memoriesEnabled);
    carry(kvs, "using_mem", d.usingMemory);
    emit transmitChanged(d);
}
void FlexStatusIngress::decodeApdStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    carry(kvs, "enable", d.apdEnabled);
    carry(kvs, "configurable", d.apdConfigurable);
    carry(kvs, "equalizer_active", d.apdEqActive);
    // Bare flag (no `=`): the model clears apdEqActive + emits the reset signal.
    if (kvs.contains(QStringLiteral("equalizer_reset")))
        d.apdEqualizerReset = true;
    // Matched by name, as FlexLib APD.cs does: an unrecognized token stays
    // disengaged rather than guessing a state.
    static const QHash<QString, ApdLifecycleState> kLifecycle{
        {QStringLiteral("OFF"), ApdLifecycleState::Off},
        {QStringLiteral("IDLE"), ApdLifecycleState::Idle},
        {QStringLiteral("TRAINING"), ApdLifecycleState::Training},
        {QStringLiteral("ACTIVE"), ApdLifecycleState::Active},
        {QStringLiteral("HOLD"), ApdLifecycleState::Hold},
        {QStringLiteral("FAULT"), ApdLifecycleState::Fault},
    };
    const auto state = kLifecycle.constFind(kvs.value(QStringLiteral("state")).toUpper());
    if (state != kLifecycle.constEnd())
        d.apdLifecycle = state.value();
    const QString ant = kvs.value(QStringLiteral("ant"));
    if (!ant.isEmpty())
        d.apdEqAntenna = ant;
    carry(kvs, "freq", d.apdEqFrequencyMhz);
    carry(kvs, "rfpower", d.apdEqRfPower);
    emit transmitChanged(d);
}
void FlexStatusIngress::decodeApdSamplerStatus(const QMap<QString, QString>& kvs)
{
    // Keyed by TX antenna; the radio sends one antenna per message. No tx_ant →
    // nothing to route (matches the old early return, no emit).
    const QString txAnt = kvs.value(QStringLiteral("tx_ant")).toUpper();
    if (txAnt.isEmpty()) return;
    TransmitDelta d;
    d.apdSamplerTxAnt = txAnt;
    if (kvs.contains(QStringLiteral("valid_samplers"))) {
        QStringList avail{QStringLiteral("INTERNAL")};
        for (const auto& p : kvs.value(QStringLiteral("valid_samplers"))
                                 .split(',', Qt::SkipEmptyParts)) {
            const QString u = p.trimmed().toUpper();
            if (!u.isEmpty() && !avail.contains(u)) avail.append(u);
        }
        d.apdSamplerAvailable = avail;
    }
    if (kvs.contains(QStringLiteral("selected_sampler")))
        d.apdSamplerSelected = kvs.value(QStringLiteral("selected_sampler")).toUpper();
    emit transmitChanged(d);
}
void FlexStatusIngress::decodeMemoryStatus(int index, const QMap<QString, QString>& kvs)
{
    // Memory-slot status → typed MemoryDelta (aetherd RFC 2.3 — RadioModel
    // residual). The decode itself moved to MemoryWire::decodeStatus so the
    // local memory bank — which decodes the very same kv-set for a radio that
    // has no memory storage of its own — cannot drift from what a Flex reports.
    emit memoryChanged(MemoryWire::decodeStatus(index, kvs));
}
void FlexStatusIngress::decodeProfileStatus(const QString& profileType, const QString& rawBody)
{
    // rawBody is everything after "profile <type> ", e.g.
    //   "list=Default^Default FHM-1^…"  |  "current=Default FHM-1"
    // Values may contain spaces, so parse key=value by hand (verbatim from the
    // old RadioModel::handleProfileStatusRaw). The database importing/exporting
    // flags arrive on this same line regardless of type.
    const int eq = rawBody.indexOf('=');
    if (eq < 0) return;
    const QString key = rawBody.left(eq).trimmed();
    const QString val = rawBody.mid(eq + 1).trimmed();

    ProfileDelta d;
    if (key == QLatin1String("importing")) {
        d.importing = val == QLatin1String("1");
        emit profileChanged(d);
        return;
    }
    if (key == QLatin1String("exporting")) {
        d.exporting = val == QLatin1String("1");
        emit profileChanged(d);
        return;
    }

    // FlexLib 4.3.16 reports the CW profile's dirty flag as
    // unsaved_changes_cw (Radio.cs:12002); the suffix names the profile type.
    static const QLatin1String unsavedPrefix("unsaved_changes_");
    if (key.startsWith(unsavedPrefix) && key.size() > unsavedPrefix.size()) {
        d.type = key.mid(unsavedPrefix.size());
        d.unsavedChanges = val == QLatin1String("1");
        emit profileChanged(d);
        return;
    }

    d.type = profileType;
    if (key == QLatin1String("list"))
        d.list = val.split('^', Qt::SkipEmptyParts);
    else if (key == QLatin1String("current"))
        d.current = val;
    else
        return;   // unknown key for this type → nothing to apply
    emit profileChanged(d);
}
void FlexStatusIngress::decodeProfileFlags(const QMap<QString, QString>& kvs)
{
    // Fallback for profile status keys that arrive space-free through the normal
    // kv-parser (e.g. "profile importing=1"). Only the database flags land here;
    // list/current always route through decodeProfileStatus. Emit nothing when
    // neither flag is present (matches the old handler's no-op path).
    const QPointer<FlexStatusIngress> alive(this);   // a handler may end the backend
    ProfileDelta d;
    carry(kvs, "importing", d.importing);
    carry(kvs, "exporting", d.exporting);
    if (d.importing || d.exporting)
        emit profileChanged(d);
    // Same unsaved_changes_<type> rule as decodeProfileStatus.
    static const QLatin1String unsavedPrefix("unsaved_changes_");
    for (auto it = kvs.cbegin(); it != kvs.cend(); ++it) {
        if (it.key().startsWith(unsavedPrefix) && it.key().size() > unsavedPrefix.size()) {
            ProfileDelta unsaved;
            unsaved.type = it.key().mid(unsavedPrefix.size());
            unsaved.unsavedChanges = it.value() == QLatin1String("1");
            if (!alive) { return; }
            emit profileChanged(unsaved);
        }
    }
}
void FlexStatusIngress::decodeGpsStatus(const QString& rawBody)
{
    // Flex GPS status: '#'-separated key=value tokens, keys case-insensitive.
    //   "status=..#tracked=8#visible=11#grid=..#altitude=644 m#lat=..#lon=..
    //    #time=..#speed=0 kts#track=0.0#freq_error=0 ppb"
    // A token with no '=' (or an empty key, eq<1) is skipped, verbatim from the
    // old RadioModel::handleGpsStatus. We map into a QMap and lean on the shared
    // carriers so the numeric counts are ok-guarded present-only.
    QMap<QString, QString> kvs;
    const QStringList tokens = rawBody.split('#', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const int eq = token.indexOf('=');
        if (eq < 1) continue;
        kvs[token.left(eq).toLower()] = token.mid(eq + 1);
    }

    GpsDelta d;
    carry(kvs, "status", d.status);
    if (kvs.contains(QStringLiteral("fix"))) {
        // Published FlexLib 4.3.16: exact tokens, ignoring case only. A
        // reported unknown value clears the fix; an absent key preserves it.
        const QString fix = kvs.value(QStringLiteral("fix")).toUpper();
        d.fixState = GpsFixState::Unknown;
        if (fix == QLatin1String("3D")) {
            d.fixState = GpsFixState::ThreeD;
        } else if (fix == QLatin1String("2D")) {
            d.fixState = GpsFixState::TwoD;
        } else if (fix == QLatin1String("TIME_ONLY")) {
            d.fixState = GpsFixState::TimeOnly;
        } else if (fix == QLatin1String("NO_FIX")) {
            d.fixState = GpsFixState::NoFix;
        }
    }
    if (kvs.contains(QStringLiteral("status"))) {
        const QString status = kvs.value(QStringLiteral("status")).trimmed().toLower();
        const bool saysLock = status.contains(QLatin1String("lock"));
        const bool saysNoLock = status.contains(QLatin1String("unlock"))
            || status.contains(QLatin1String("no lock"))
            || status.contains(QLatin1String("not lock"))
            || status.contains(QLatin1String("lost"))
            || status.contains(QLatin1String("loss"));
        // Preserve legacy validity for firmware without fix=. RadioModel
        // additionally vetoes it when a reported fix cannot supply position.
        d.positionValid = saysLock && !saysNoLock;
        d.source = QStringLiteral("GPSDO");
    }
    carry(kvs, "tracked", d.tracked);
    carry(kvs, "visible", d.visible);
    carry(kvs, "grid", d.grid);
    carry(kvs, "altitude", d.altitude);
    carry(kvs, "lat", d.lat);
    carry(kvs, "lon", d.lon);
    carry(kvs, "time", d.time);
    carry(kvs, "speed", d.speed);
    // Firmware 4.2.18 includes course-over-ground as `track` even though the
    // current FlexLib GPS property surface ignores it.  This was verified in
    // a clean-room FLEX-8600 status capture; preserving it lets portable and
    // mobile stations see every value the radio actually reports.
    carry(kvs, "track", d.track);
    carry(kvs, "freq_error", d.freqError);
    emit gpsChanged(d);
}
void FlexStatusIngress::decodeMeterStatus(const QString& rawBody)
{
    const QPointer<FlexStatusIngress> alive(this);
    // Meter status body (FlexLib Radio.cs ParseMeterStatus):
    //   Tokens separated by '#', each token is "index.key=value".
    //   e.g. "7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0"
    // Removal: "7 removed". Parsing verbatim from the old RadioModel path.
    if (rawBody.contains(QStringLiteral("removed"))) {
        const QStringList words = rawBody.split(' ', Qt::SkipEmptyParts);
        if (!words.isEmpty()) {
            bool ok = false;
            const int idx = words[0].toInt(&ok);
            if (ok) {
                emit meterRemoved(idx);
            }
        }
        return;
    }

    // Group fields by meter ID, but publish in first-appearance wire order.
    // MeterModel associates a TX waveform block with its preceding SLC block
    // (observed FLEX-8400M fw 4.2.18); sorting IDs can move a reused TX ID
    // ahead of its own SLC context.
    QMap<int, QMap<QString, QString>> grouped;
    QList<int> meterOrder;
    const QStringList tokens = rawBody.split('#', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const int dot = token.indexOf('.');
        if (dot < 0) continue;
        const int eq = token.indexOf('=', dot);
        if (eq < 0) continue;
        bool ok = false;
        const int idx = token.left(dot).toInt(&ok);
        if (!ok) continue;
        if (!grouped.contains(idx)) {
            meterOrder.append(idx);
        }
        grouped[idx][token.mid(dot + 1, eq - dot - 1)] = token.mid(eq + 1);
    }

    for (int index : meterOrder) {
        const auto group = grouped.constFind(index);
        const QMap<QString, QString>& f = group.value();
        // Build the typed MeterDef directly (#4070). Present-only: a field the
        // wire didn't report keeps its MeterDef default. The carry() ok-guard is
        // defensive/consistency only here — a plain MeterDef field's default IS
        // 0/0.0, which is also what an unguarded parse of a malformed value would
        // yield, and meter status is a full definition that defineMeter()
        // full-replaces (so there's no prior value to preserve). The guard has
        // real fail-closed effect only at the std::optional carry() sites
        // (slice/transmit), where a dropped value leaves the field disengaged.
        // (#4075 review.)
        MeterDef def;
        def.index = index;
        carry(f, "src", def.source);
        // num is a uint (FlexLib Meter.SourceIndex): decimal for most meters,
        // 0x%08X for a handle-owned one, which can exceed INT_MAX.
        if (const auto num = parseUInt(f, "num", /*base=*/0))
            def.sourceIndex = static_cast<int>(*num);
        carry(f, "nam", def.name);
        carry(f, "unit", def.unit);
        carry(f, "low", def.low);
        carry(f, "hi", def.high);
        carry(f, "desc", def.description);
        if (!alive) { return; }   // a meter handler may end the backend
        emit meterDefined(def);
    }
}
// FlexLib v4.3.16 Radio.cs:3610 routes `ntp_sources` before its kv split.
bool FlexStatusIngress::decodeRawStatus(const QString& body)
{
    static const QString kNtpSources = QStringLiteral("ntp_sources ");
    if (!body.startsWith(kNtpSources)) { return false; }
    decodeNtpSourceStatus(body.mid(kNtpSources.size()));
    return true;
}
// FlexLib v4.3.16 Radio.cs:14427 ParseNtpSourcesStatus and NtpSource.cs
// StatusUpdate. The first word is always id=<id>; only the word right after it
// can mean removal, since a description could contain "removed".
void FlexStatusIngress::decodeNtpSourceStatus(const QString& rawBody)
{
    const QStringList words = rawBody.split(QLatin1Char(' '));
    if (words.size() < 2 || !words.at(0).startsWith(QLatin1String("id="))) { return; }
    NtpSourceDelta d;
    d.id = words.at(0).mid(3);
    if (d.id.isEmpty()) { return; }
    if (words.at(1) == QLatin1String("removed")) {
        d.removed = true;
        emit ntpSourceChanged(d);
        return;
    }
    for (const QString& token : splitStatusTokens(rawBody.mid(words.at(0).size() + 1))) {
        const qsizetype eq = token.indexOf(QLatin1Char('='));
        if (eq <= 0) { continue; }
        const QString key = token.left(eq).toLower();
        const QString value = token.mid(eq + 1);
        if (key == QLatin1String("address")) { d.address = value; }
        else if (key == QLatin1String("directive")) { d.directive = value; }
        else if (key == QLatin1String("description")) { d.description = value; }
        else if (key == QLatin1String("enabled")) { d.enabled = parseNtpFlag(value); }
        else if (key == QLatin1String("selected")) { d.selected = parseNtpFlag(value); }
        else if (key == QLatin1String("prefer")) { d.prefer = parseNtpFlag(value); }
        else if (key == QLatin1String("state")) { d.state = value; }
        else if (key == QLatin1String("stratum")) { d.stratum = parseNtpInt(value); }
        else if (key == QLatin1String("reach")) { d.reach = parseNtpInt(value); }
    }
    emit ntpSourceChanged(d);
}

void FlexStatusIngress::decodeAudioStreamStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    if (QThread::currentThread() != thread()) { return; }
    // FlexLib StringHelper.TryParseInteger: a 0x-prefixed hex id or decimal.
    static const QRegularExpression pattern(QStringLiteral("\\A(stream|tx_audio_stream) ([^\\s]+)(?: (removed))?\\z"));
    const QRegularExpressionMatch match = pattern.match(object);
    if (!match.hasMatch()) { return; }
    const std::optional<quint32> streamId = daxTxIdentity(match.captured(2));
    if (!streamId) { return; }
    AudioStreamDelta delta;
    delta.streamId = *streamId;
    delta.removed = match.captured(3) == QLatin1String("removed") || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QLatin1String("0");
    const QString type = match.captured(1) == QLatin1String("tx_audio_stream")
        ? QStringLiteral("dax_tx") : kvs.value(QStringLiteral("type"));
    if (type == QLatin1String("remote_audio_rx")) { delta.kind = AudioStreamKind::RemoteReceive; }
    else if (type == QLatin1String("remote_audio_tx")) { delta.kind = AudioStreamKind::RemoteTransmit; }
    else if (type == QLatin1String("dax_rx")) { delta.kind = AudioStreamKind::DaxReceive; }
    else if (type == QLatin1String("dax_tx")) { delta.kind = AudioStreamKind::DaxTransmit; }
    else if (type == QLatin1String("dax_iq")) { delta.kind = AudioStreamKind::Iq; }
    else if (kvs.contains(QStringLiteral("type"))) { delta.removed = true; }
    // client_handle=0x0 leaves the owner unknown, as a missing field does; only
    // a value that does not parse is malformed.
    if (kvs.contains(QStringLiteral("client_handle"))) {
        const QString handle = kvs.value(QStringLiteral("client_handle")).trimmed();
        const quint32 owner = RadioStatusOwnership::parseFlexId(handle);
        const QString digits = handle.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? handle.mid(2) : handle;
        bool parsed = false;
        const bool zero = owner == 0 && digits.toUInt(&parsed, 16) == 0 && parsed;
        if (owner != 0) { delta.clientHandle = owner; }
        else if (!zero) { delta.invalidOwner = true; }
        // 0x0, or an owner that does not parse, at 0.0.0.0 is a dead orphan.
        if (owner == 0) { delta.orphaned = kvs.value(QStringLiteral("ip")).trimmed() == QLatin1String("0.0.0.0"); }
    }
    const bool iqChannel = kvs.contains(QStringLiteral("daxiq_channel"));
    const QString channelKey = iqChannel ? QStringLiteral("daxiq_channel") : QStringLiteral("dax_channel");
    // A channel outside 1..8 (1..4 for IQ) says nothing about who owns the
    // stream; like FlexLib v4.3.16 (DAXRXAudioStream.cs:213), skip the field
    // and keep the known channel.
    if (kvs.contains(channelKey)) {
        bool valid = false;
        const int channel = kvs.value(channelKey).toInt(&valid);
        if (valid && channel >= 1 && channel <= (iqChannel ? 4 : 8)) { delta.channel = channel; }
    }
    if (delta.kind == AudioStreamKind::Iq) {
        bool valid = false;
        const int rate = kvs.value(QStringLiteral("daxiq_rate")).toInt(&valid);
        if (valid) { delta.iqRate = rate; }
        if (kvs.contains(QStringLiteral("pan"))) { delta.pan = kvs.value(QStringLiteral("pan")); }
        if (kvs.contains(QStringLiteral("active"))) { delta.active = kvs.value(QStringLiteral("active")) == QLatin1String("1"); }
    }
    emit audioStreamChanged(delta);
}
void FlexStatusIngress::decodeDaxTxStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    static const QRegularExpression pattern(
        QStringLiteral("\\A(stream|tx_audio_stream) ([^\\s]+)(?: (removed))?\\z"));
    const QRegularExpressionMatch match = pattern.match(object);
    const std::optional<quint32> streamId = match.hasMatch()
        ? daxTxIdentity(match.captured(2)) : std::nullopt;
    if (!streamId) {
        return;
    }
    const bool known = m_daxTxStreams.contains(*streamId);
    const bool legacy = match.captured(1) == QLatin1String("tx_audio_stream");
    const bool otherType = !legacy && kvs.contains(QStringLiteral("type"))
        && kvs.value(QStringLiteral("type")) != QLatin1String("dax_tx");
    DaxTxStreamDelta delta;
    delta.streamId = *streamId;
    delta.removed = !match.captured(3).isEmpty() || kvs.contains(QStringLiteral("removed"))
        || kvs.value(QStringLiteral("in_use")) == QLatin1String("0") || (known && otherType);
    if (delta.removed) {
        ++m_daxTxIncarnation;
        m_daxTxStreams.remove(*streamId);
        emit daxTxStreamChanged(delta);
        return;
    }
    if (otherType || (!known && !legacy
        && kvs.value(QStringLiteral("type")) != QLatin1String("dax_tx"))) {
        return;
    }

    if (!known) {
        DaxTxStreamState state;
        state.incarnation = ++m_daxTxIncarnation;
        m_daxTxStreams.insert(*streamId, state);
    }
    DaxTxStreamState& state = m_daxTxStreams[*streamId];
    if (kvs.contains(QStringLiteral("client_handle"))) {
        delta.clientHandle = daxTxIdentity(kvs.value(QStringLiteral("client_handle")));
        delta.invalidOwner = !delta.clientHandle.has_value();
        if (delta.invalidOwner || state.clientHandle != delta.clientHandle) {
            state.incarnation = ++m_daxTxIncarnation;
            state.tx.reset();
        }
        state.clientHandle = delta.clientHandle;
    }
    if (kvs.contains(QStringLiteral("tx"))) {
        const QString value = kvs.value(QStringLiteral("tx"));
        if (value == QLatin1String("0") || value == QLatin1String("1")) {
            delta.tx = value == QLatin1String("1");
            if (state.clientHandle && !delta.invalidOwner) {
                state.tx = delta.tx;
            }
        } else {
            delta.invalidTx = true;
            state.tx.reset();
            state.incarnation = ++m_daxTxIncarnation;
        }
    }
    emit daxTxStreamChanged(delta);
}
void FlexStatusIngress::decodeStreamStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    const QPointer<FlexStatusIngress> alive(this);
    if (const quint32 netCw = FlexCwControls::removedStreamId(object, kvs)) {
        emit cwStreamRemoved(netCw);
        if (!alive) { return; }
    }
    decodeAudioStreamStatus(object, kvs);
    if (alive) { decodeDaxTxStatus(object, kvs); }
}
void FlexStatusIngress::decodeTxBandStatus(int bandId, const QMap<QString, QString>& kvs)
{
    // Read as RadioModel did: numbers by toInt(), flags by "1".
    TxBandDelta d;
    d.bandId = bandId;
    if (kvs.contains(QStringLiteral("band_name")))     { d.bandName = kvs.value(QStringLiteral("band_name")); }
    if (kvs.contains(QStringLiteral("rfpower")))       { d.rfPower = kvs.value(QStringLiteral("rfpower")).toInt(); }
    if (kvs.contains(QStringLiteral("tunepower")))     { d.tunePower = kvs.value(QStringLiteral("tunepower")).toInt(); }
    if (kvs.contains(QStringLiteral("inhibit")))       { d.inhibit = kvs.value(QStringLiteral("inhibit")) == QLatin1String("1"); }
    if (kvs.contains(QStringLiteral("hwalc_enabled"))) { d.hwAlc = kvs.value(QStringLiteral("hwalc_enabled")) == QLatin1String("1"); }
    emit txBandChanged(d);
}
void FlexStatusIngress::decodeInterlockBandStatus(int bandId, const QMap<QString, QString>& kvs)
{
    // Read as RadioModel did: flags by "1".
    TxBandDelta d;
    d.bandId = bandId;
    const auto flag = [&kvs](const char* key) -> std::optional<bool> {
        if (!kvs.contains(QLatin1String(key))) { return std::nullopt; }
        return kvs.value(QLatin1String(key)) == QLatin1String("1");
    };
    if (kvs.contains(QStringLiteral("band_name"))) { d.bandName = kvs.value(QStringLiteral("band_name")); }
    d.accTxReq = flag("acc_txreq_enable");
    d.rcaTxReq = flag("rca_txreq_enable");
    for (const char* field : {"acc_tx_enabled", "tx1_enabled", "tx2_enabled", "tx3_enabled"}) {
        if (kvs.contains(QLatin1String(field))) { d.reportedOutputs.insert(QLatin1String(field)); }
    }
    d.accTx = flag("acc_tx_enabled");
    d.tx1 = flag("tx1_enabled");
    d.tx2 = flag("tx2_enabled");
    d.tx3 = flag("tx3_enabled");
    emit txBandChanged(d);
}
void FlexStatusIngress::decodeTuneOutputStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    // Subscription readback can precede info's software_ver on reconnect or
    // manual-IP connections. Validate observations now; gate edits on capability.
    static const QRegularExpression bandPattern(QStringLiteral("^interlock band ([0-9]{1,3})$"));
    const QRegularExpressionMatch match = bandPattern.match(object);
    if (!match.hasMatch()) {
        return;
    }
    const int bandId = match.captured(1).toInt();
    const QString value = kvs.value(QStringLiteral("tune_tx_suppress"));
    if (TuneOutputPolicy::validBandId(bandId)
        && (value == QLatin1String("0") || value == QLatin1String("1"))) {
        m_tuneOutputKnownBands.insert(bandId);
        emit tuneOutputSuppressionObserved(bandId, value == QLatin1String("1"));
    }
}
void FlexStatusIngress::decodeTnfStatus(int notchId, bool removed, const QMap<QString, QString>& kvs)
{
    if (removed) {
        emit notchRemoved(notchId);
        return;
    }
    // Read as TnfModel did: freq in MHz, width in Hz (a fractional value is
    // the older MHz form; anything under 10 Hz falls back to 100).
    NotchDelta d;
    if (kvs.contains(QStringLiteral("freq"))) { d.centerHz = kvs.value(QStringLiteral("freq")).toDouble() * 1.0e6; }
    if (kvs.contains(QStringLiteral("width"))) {
        const double rawWidth = kvs.value(QStringLiteral("width")).toDouble();
        int widthHz = rawWidth > 0.0 && rawWidth < 1.0 ? static_cast<int>(rawWidth * 1.0e6 + 0.5)
                                                       : static_cast<int>(rawWidth + 0.5);
        if (widthHz < 10) { widthHz = 100; }
        d.widthHz = widthHz;
    }
    if (kvs.contains(QStringLiteral("depth"))) { d.depthDb = kvs.value(QStringLiteral("depth")).toInt(); }
    if (kvs.contains(QStringLiteral("permanent"))) { d.permanent = kvs.value(QStringLiteral("permanent")) == QLatin1String("1"); }
    emit notchChanged(notchId, d);
}
// Amplifier status: TGXL and PGXL both report via "amplifier"; FlexLib tells
// them apart by model=TunerGeniusXL / PowerGeniusXL. Removal arrives bare
// ("amplifier <handle> removed") or as removed=1 (FlexLib Radio.cs:14060/14073
// matches the substring "removed").
std::optional<StreamStatusDelta> FlexStatusIngress::streamStatus(const QString& object,
                                                                const QMap<QString, QString>& kvs)
{
    return flexStreamStatus(object, kvs);
}

void FlexStatusIngress::routeAmplifierStatus(const QString& object, const QMap<QString, QString>& kvs)
{
    static const QRegularExpression ampRe(QStringLiteral(R"(^amplifier\s+(\S+)$)"));
    static const QRegularExpression ampRemovedRe(QStringLiteral(R"(^amplifier\s+(\S+)\s+removed$)"));
    if (const auto rm = ampRemovedRe.match(object); rm.hasMatch()) {
        qCDebug(lcProtocol) << "FlexStatusIngress: amplifier removed (bare) handle=" << rm.captured(1);
        decodeAmplifierStatus(rm.captured(1), QString(), {}, /*removed=*/true);
        return;
    }
    const auto m = ampRe.match(object);
    if (!m.hasMatch()) { return; }
    const QString handle = m.captured(1);
    const QString model = kvs.value(QStringLiteral("model"));
    qCDebug(lcProtocol) << "FlexStatusIngress: amplifier status handle=" << handle << "model=" << model;
    if (kvs.contains(QStringLiteral("removed"))) {
        decodeAmplifierStatus(handle, QString(), {}, /*removed=*/true);
        return;
    }
    // The TGXL by its model, or by the handle it was last named (a later
    // status may omit model=); every other amp is a power amplifier.
    if (model == QLatin1String("TunerGeniusXL") || (!m_tunerHandle.isEmpty() && handle == m_tunerHandle)) {
        if (m_decodeValues) {
            decodeTunerStatus(handle, kvs);
        } else if (!handle.isEmpty() && handle != QLatin1String("0x00000000")) {
            m_tunerHandle = handle;
            TunerDelta identity;
            identity.handle = handle;
            emit tunerChanged(identity);
        }
    } else if (m_decodeValues) {
        decodeAmplifierStatus(handle, model, kvs, /*removed=*/false);
    }
}
void FlexStatusIngress::decodeAmplifierStatus(const QString& handle, const QString& model,
                                        const QMap<QString, QString>& kvs, bool removed)
{
    // Translation of the SmartSDR "amplifier <handle> …" wire → AmpDelta
    // (#4094). Placeholder handles are normalized here so the vendor-neutral
    // model never needs SmartSDR sentinel knowledge. The presence latch, operate
    // change-gating, and handle matching are the model's job
    // (AmpModel::applyChanges). Command/encode is the reverse path — invokeExtension("flex",
    // "amp.operate", …) below translates AmpModel's neutral intent (#4094).
    AmpDelta d;
    d.handle = handle;
    if (removed) {
        // Drop the cached handle for the encode path (#4198) when the device it
        // names goes away. TGXL removal also arrives on the amplifier-removed
        // wire (routeAmplifierStatus), so clear whichever handle matches.
        if (handle == m_ampHandle) m_ampHandle.clear();
        if (handle == m_tunerHandle) m_tunerHandle.clear();
        d.removed = true;
        emit amplifierChanged(d);
        return;
    }
    if (handle == QLatin1String("0x00000000")) {
        d.handle.clear();
    }
    // RadioModel routes only power amps (PGXL) into this decode, so the handle is
    // the amp's — cache it for the encode path (#4198). Ignore the placeholder
    // handle a first status can carry before the real one is assigned. Defense in
    // depth (#4203): a pre-existing routing edge — a model-less TGXL status arriving
    // before its handle is known — can fall through to here; refuse to cache a
    // known-tuner handle so a later amp.operate can never mis-target the TGXL.
    if (!d.handle.isEmpty() && d.handle != m_tunerHandle) {
        m_ampHandle = d.handle;
    }
    // A non-empty, non-TGXL model marks a power amp (PGXL); the TunerGeniusXL is
    // the tuner and routes to TunerModel, not here.
    if (!model.isEmpty() && model != QLatin1String("TunerGeniusXL")) {
        d.detectedModel = model;
        if (kvs.contains(QStringLiteral("ip")))
            d.ip = kvs.value(QStringLiteral("ip"));
    }
    // Operate/standby from the wire "state": IDLE/OPERATE/TRANSMIT* → on, else off.
    // Gate on non-empty VALUE (not just key presence) to match the prior
    // AmpModel::applyStatus exactly — a bare "state=" must not flip operate.
    const QString state = kvs.value(QStringLiteral("state")).toUpper();
    if (!state.isEmpty()) {
        d.operate = (state == QLatin1String("IDLE")
                     || state == QLatin1String("OPERATE")
                     || state.startsWith(QLatin1String("TRANSMIT")));
    }
    d.telemetry = kvs;
    emit amplifierChanged(d);
}
void FlexStatusIngress::decodeTunerStatus(const QString& handle, const QMap<QString, QString>& kvs)
{
    // Cache the TGXL handle for the encode path (#4198). A first status can
    // carry 0x00000000 before the real handle is assigned; keep that SmartSDR
    // placeholder out of both the neutral delta and outgoing tuner commands.
    if (!handle.isEmpty() && handle != QLatin1String("0x00000000"))
        m_tunerHandle = handle;
    emit tunerChanged(tunerStatus(handle, kvs));
}

TunerDelta FlexStatusIngress::tunerStatus(const QString& handle, const QMap<QString, QString>& kvs)
{
    // Present-only, strict parity with the prior TunerModel::applyStatus: bools
    // are "1"-equality, ints are unguarded toInt() (matching val.toInt()), text
    // is verbatim. The change-gating / edge signals live in TunerModel::applyChanges.
    TunerDelta d;
    if (!handle.isEmpty() && handle != QLatin1String("0x00000000")) {
        d.handle = handle;
    }
    if (kvs.contains(QStringLiteral("serial_num")))
        d.serialNum = kvs.value(QStringLiteral("serial_num"));
    if (kvs.contains(QStringLiteral("model")))
        d.model = kvs.value(QStringLiteral("model"));
    if (kvs.contains(QStringLiteral("ip")))
        d.ip = kvs.value(QStringLiteral("ip"));
    // Per-port antenna, "ANT1,ANT2". Split exactly as FlexLib's
    // Tuner.ParseAntenna does: first field is port A, second is port B, a
    // missing second field leaves B empty, and anything past the second is
    // ignored rather than treated as an error.
    if (kvs.contains(QStringLiteral("ant"))) {
        // split() always yields at least one element, so at(0) is safe.
        const QStringList ants = kvs.value(QStringLiteral("ant")).split(QLatin1Char(','));
        d.portAAnt = ants.at(0).trimmed();
        d.portBAnt = ants.size() > 1 ? ants.at(1).trimmed() : QString();
    }

    // PTT-per-port. Casing unconfirmed — FlexLib lower-cases every key before
    // matching, so its "ptta"/"pttb" cases do not pin the wire's spelling.
    // Both are accepted; a string compare is cheaper than a lamp that stays
    // dark with no way to tell why.
    if (kvs.contains(QStringLiteral("pttA")))
        d.pttA = (kvs.value(QStringLiteral("pttA")) == QLatin1String("1"));
    else if (kvs.contains(QStringLiteral("ptta")))
        d.pttA = (kvs.value(QStringLiteral("ptta")) == QLatin1String("1"));
    if (kvs.contains(QStringLiteral("pttB")))
        d.pttB = (kvs.value(QStringLiteral("pttB")) == QLatin1String("1"));
    else if (kvs.contains(QStringLiteral("pttb")))
        d.pttB = (kvs.value(QStringLiteral("pttb")) == QLatin1String("1"));
    if (kvs.contains(QStringLiteral("operate")))
        d.operate = (kvs.value(QStringLiteral("operate")) == QLatin1String("1"));
    if (kvs.contains(QStringLiteral("bypass")))
        d.bypass = (kvs.value(QStringLiteral("bypass")) == QLatin1String("1"));
    if (kvs.contains(QStringLiteral("tuning")))
        d.tuning = (kvs.value(QStringLiteral("tuning")) == QLatin1String("1"));
    if (kvs.contains(QStringLiteral("relayC1")))
        d.relayC1 = kvs.value(QStringLiteral("relayC1")).toInt();
    if (kvs.contains(QStringLiteral("relayC2")))
        d.relayC2 = kvs.value(QStringLiteral("relayC2")).toInt();
    if (kvs.contains(QStringLiteral("relayL")))
        d.relayL = kvs.value(QStringLiteral("relayL")).toInt();
    if (kvs.contains(QStringLiteral("antA")))
        d.antennaA = kvs.value(QStringLiteral("antA")).toInt();
    if (kvs.contains(QStringLiteral("one_by_three")))
        d.oneByThree = (kvs.value(QStringLiteral("one_by_three")) == QLatin1String("1"));
    return d;
}
TransmitDelta FlexStatusIngress::interlockTiming(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    carry(kvs, "acc_tx_delay", d.accTxDelay);
    carry(kvs, "tx1_delay", d.tx1Delay);
    carry(kvs, "tx2_delay", d.tx2Delay);
    carry(kvs, "tx3_delay", d.tx3Delay);
    carry(kvs, "tx_delay", d.txDelay);
    carry(kvs, "timeout", d.interlockTimeout);
    carry(kvs, "acc_txreq_polarity", d.accTxReqPolarity);
    carry(kvs, "rca_txreq_polarity", d.rcaTxReqPolarity);
    return d;
}

InterlockStatusDelta FlexStatusIngress::interlockStatus(const QMap<QString, QString>& kvs)
{
    // Read as RadioModel did: the owner in hex, state and source upper-cased.
    InterlockStatusDelta d;
    if (kvs.contains(QStringLiteral("tx_client_handle"))) {
        d.txClientHandle = kvs.value(QStringLiteral("tx_client_handle")).toUInt(nullptr, 16);
    }
    if (kvs.contains(QStringLiteral("state"))) { d.state = kvs.value(QStringLiteral("state")).toUpper(); }
    if (kvs.contains(QStringLiteral("source"))) { d.source = kvs.value(QStringLiteral("source")).toUpper(); }
    d.txAllowed = kvs.value(QStringLiteral("tx_allowed"));
    d.reason = kvs.value(QStringLiteral("reason")).toUpper();
    d.timing = interlockTiming(kvs);
    return d;
}
} // namespace AetherSDR
