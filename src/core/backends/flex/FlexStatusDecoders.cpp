// The Flex status decoders: wire lines to typed deltas, run by the status
// ingress on its owner's thread for every accepted line (FlexStatusIngress.h).
// Moved from FlexBackend / FlexNtpSourceControls unchanged.
#include "FlexStatusIngress.h"

#include "core/backends/MemoryWireCodec.h"
#include "core/backends/flex/FlexKvCarry.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
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

std::optional<bool> parseFlag(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    if (!ok || v < 0 || v > 255) { return std::nullopt; }
    return v == 1;
}

std::optional<int> parseInt(const QString& value)
{
    bool ok = false;
    const int v = value.toInt(&ok);
    return ok ? std::optional<int>(v) : std::nullopt;
}

} // namespace

void FlexStatusIngress::decodeLine(const ParsedMessage& line)
{
    if (line.type != MessageType::Status) { return; }
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
    if (object.startsWith(QLatin1String("atu"))) { decodeAtuStatus(kvs); return; }
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
    // Untyped profile objects: the space-free flags ("profile importing=1").
    if (object.startsWith(QLatin1String("profile"))) { decodeProfileFlags(kvs); return; }
    // "memory 7" or "memory 7 removed": trailing bare words are folded in.
    if (object.startsWith(QLatin1String("memory "))) {
        const QString rest = object.mid(7);
        const int sp = rest.indexOf(QLatin1Char(' '));
        bool ok = false;
        const int index = ((sp >= 0) ? rest.left(sp) : rest).toInt(&ok);
        if (!ok) { return; }
        QMap<QString, QString> merged = kvs;
        if (sp >= 0) {
            for (const auto& token : rest.mid(sp + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const int eq = token.indexOf(QLatin1Char('='));
                if (eq < 0) { merged.insert(token, QString{}); }
                else { merged.insert(token.left(eq), token.mid(eq + 1)); }
            }
        }
        decodeMemoryStatus(index, merged);
    }
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
        else if (key == QLatin1String("enabled")) { d.enabled = parseFlag(value); }
        else if (key == QLatin1String("selected")) { d.selected = parseFlag(value); }
        else if (key == QLatin1String("prefer")) { d.prefer = parseFlag(value); }
        else if (key == QLatin1String("state")) { d.state = value; }
        else if (key == QLatin1String("stratum")) { d.stratum = parseInt(value); }
        else if (key == QLatin1String("reach")) { d.reach = parseInt(value); }
    }
    emit ntpSourceChanged(d);
}

} // namespace AetherSDR
