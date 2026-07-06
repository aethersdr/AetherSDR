#include "core/backends/flex/FlexBackend.h"

#include <limits>

#include <QThread>

#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"
#include "models/ModelCapabilities.h"

namespace AetherSDR {

FlexBackend::FlexBackend(QObject* parent)
    : IRadioBackend(parent)
{
    // Own the wire objects + their worker threads. Order is load-bearing and
    // preserved verbatim from the former RadioModel ctor (#502): PanadapterStream
    // thread FIRST, then RadioConnection thread. Both objects are parentless and
    // moved onto their thread; the thread is this-parented.
    m_networkThread = new QThread(this);
    m_networkThread->setObjectName("PanadapterStream");
    m_panStream = new PanadapterStream;   // no parent — moved to thread
    m_panStream->moveToThread(m_networkThread);
    connect(m_networkThread, &QThread::started, m_panStream, &PanadapterStream::init);
    m_networkThread->start();

    m_connThread = new QThread(this);
    m_connThread->setObjectName("RadioConnection");
    m_connection = new RadioConnection;   // no parent — moved to thread
    m_connection->moveToThread(m_connThread);
    connect(m_connThread, &QThread::started, m_connection, &RadioConnection::init);
    m_connThread->start();

    // Observe wire lifecycle and re-emit as the interface's own signals. Queued
    // (auto) connections: the connection lives on its worker thread.
    connect(m_connection, &RadioConnection::connected,
            this, &IRadioBackend::connected);
    connect(m_connection, &RadioConnection::disconnected,
            this, &IRadioBackend::disconnected);
    connect(m_connection, &RadioConnection::errorOccurred,
            this, &IRadioBackend::connectionError);
}

FlexBackend::~FlexBackend()
{
    // Sever our own lifecycle observation of the connection FIRST — as the old
    // ~RadioModel's earlier m_backend.reset() effectively did (the backend was
    // destroyed, auto-disconnecting these links, before the wire teardown ran).
    // Otherwise disconnectFromRadio below could re-emit connected/disconnected
    // through this half-destroyed backend. (#4058 review)
    if (m_connection) {
        disconnect(m_connection, nullptr, this, nullptr);
    }

    // Teardown in the exact #502 order the former RadioModel dtor used:
    // connection first (BlockingQueued disconnect → deleteLater → thread
    // quit/wait), then panStream (BlockingQueued stop → …).
    if (m_connection && m_connThread && m_connThread->isRunning()) {
        RadioConnection* connection = m_connection;
        QMetaObject::invokeMethod(connection, &RadioConnection::disconnectFromRadio,
                                  Qt::BlockingQueuedConnection);
        connection->deleteLater();
        m_connThread->quit();
        m_connThread->wait(3000);
    } else {
        delete m_connection;
    }
    if (m_connThread && m_connThread->isRunning()) {
        m_connThread->quit();
        m_connThread->wait(3000);
    }
    m_connection = nullptr;

    if (m_panStream && m_networkThread && m_networkThread->isRunning()) {
        PanadapterStream* panStream = m_panStream;
        QMetaObject::invokeMethod(panStream, &PanadapterStream::stop,
                                  Qt::BlockingQueuedConnection);
        panStream->deleteLater();
        m_networkThread->quit();
        m_networkThread->wait(3000);
    } else {
        delete m_panStream;
    }
    if (m_networkThread && m_networkThread->isRunning()) {
        m_networkThread->quit();
        m_networkThread->wait(3000);
    }
    m_panStream = nullptr;
}

void FlexBackend::setCommandSink(std::function<void(const QString&)> sink)
{
    m_sink = std::move(sink);
}

void FlexBackend::setSliceCommandSink(std::function<void(const QString&)> sink)
{
    m_sliceSink = std::move(sink);
}

void FlexBackend::setModelProvider(std::function<QString()> provider)
{
    m_modelProvider = std::move(provider);
}

RadioCapabilities FlexBackend::capabilities() const
{
    RadioCapabilities caps;
    caps.family = QStringLiteral("flex");
    caps.model = m_modelProvider ? m_modelProvider() : QString();

    // Seed from the FlexLib-sourced platform table (Principle I). This is the
    // derived-from-name truth used to *seed* the reported capabilities; a fuller
    // FlexBackend refines these from live radio status as touchpoints convert.
    const ModelCapabilities mc = capabilitiesFor(caps.model);
    caps.maxSlices = mc.maxSlices;
    // approx: pan capacity is not strictly slice count on real Flex hardware;
    // refined from live radio status in a later touchpoint conversion.
    caps.maxPanadapters = mc.maxSlices;
    caps.hasExtendedDsp = mc.hasExtendedDsp();

    // Every current FlexRadio transmits; RX-only WAN/observer nuance is layered
    // in later. Sample rates and TX power range are refined as their touchpoints
    // convert (they are not part of this skeleton).
    caps.canTransmit = true;
    caps.hasTuner = true;

    // Advertise NO extension namespaces yet: no flex verb is routed through the
    // seam, and invokeExtension() can't produce a reply. Advertising "flex"
    // would let a client pre-check the namespace and then hang awaiting an
    // extensionResult/Error that never comes. "flex" is declared here when the
    // first amp/tuner/DAX verb converts.
    return caps;
}

void FlexBackend::connectRadio(const RadioConnectRequest& /*request*/)
{
    // RadioModel still orchestrates connect (RadioInfo assembly, WAN/SmartLink
    // duality, auto-reconnect); the backend owns the objects but not yet the
    // connect flow — that adaptation moves behind the seam in a later increment.
}

void FlexBackend::disconnectRadio()
{
    // RadioModel still orchestrates the staged gracefulDisconnect
    // (handle/streamId/seq). Owned by the backend later.
}

bool FlexBackend::isConnected() const
{
    return m_connection && m_connection->isConnected();
}

void FlexBackend::setSliceFrequency(int sliceId, double hz)
{
    // Matches SliceModel::setFrequency's wire string exactly. Slice verbs use
    // the TX-inhibit-guarded slice sink (§6), not the generic sink.
    sendSlice(QStringLiteral("slice tune %1 %2 autopan=0")
                  .arg(sliceId)
                  .arg(hz / 1'000'000.0, 0, 'f', 6));
}

void FlexBackend::setSliceMode(int sliceId, const QString& mode)
{
    sendSlice(QStringLiteral("slice set %1 mode=%2").arg(sliceId).arg(mode));
}

void FlexBackend::setSliceFilter(int sliceId, int lowHz, int highHz)
{
    sendSlice(QStringLiteral("filt %1 %2 %3").arg(sliceId).arg(lowHz).arg(highHz));
}

void FlexBackend::setKeying(bool key)
{
    // Keying is only translated here; the interlock/authorization decision is
    // made above the seam (RFC §6). Matches RadioModel::setTransmit's wire form.
    send(QStringLiteral("xmit %1").arg(key ? 1 : 0));
}

void FlexBackend::invokeExtension(const QString& /*ns*/, const QString& /*verb*/,
                                  quint64 requestId, const QVariant& /*arg*/)
{
    // No flex extension verbs are routed through the seam yet. Honor the async
    // contract by construction: a caller awaiting a reply (requestId != 0) gets
    // an error, never a hang. Real verbs land with the amp/tuner/DAX touchpoint
    // conversions.
    if (requestId != 0) {
        emit extensionError(requestId,
                            QStringLiteral("flex: no extension verbs implemented"));
    }
}

void FlexBackend::decodePanCenterBandwidth(const QString& panId,
                                           const QMap<QString, QString>& kvs)
{
    // Only emit when the wire carried these fields — matches the old
    // applyPanStatus behavior of touching center/bandwidth only when present.
    if (!kvs.contains(QStringLiteral("center"))
        && !kvs.contains(QStringLiteral("bandwidth"))) {
        return;
    }
    // The radio may send one without the other; carry the current-or-parsed
    // value for the missing one (RadioModel resolves against the model). A
    // sentinel of -1 means "unchanged" for the absent field.
    const double center = kvs.contains(QStringLiteral("center"))
        ? kvs.value(QStringLiteral("center")).toDouble() : -1.0;
    const double bandwidth = kvs.contains(QStringLiteral("bandwidth"))
        ? kvs.value(QStringLiteral("bandwidth")).toDouble() : -1.0;
    emit panCenterBandwidthChanged(panId, center, bandwidth);
}

void FlexBackend::decodePanRange(const QString& panId,
                                 const QMap<QString, QString>& kvs)
{
    // Only emit when the wire carried these fields — matches the old
    // applyPanStatus behavior of touching min/max dBm only when present.
    if (!kvs.contains(QStringLiteral("min_dbm"))
        && !kvs.contains(QStringLiteral("max_dbm"))) {
        return;
    }
    // dBm is signed (-130…-20 typical), so a negative value can't mean
    // "absent" the way it does for center/bandwidth. Carry NaN for the field
    // the radio omitted; the model's setRange() treats NaN as "leave unchanged".
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // Guard the numeric parse: a malformed *present* field must be ignored
    // (carry NaN = "unchanged"), not applied as 0.0 dBm — setRange() only skips
    // NaN, so a bare 0 would collapse the vertical scale via setDbmRange. Matches
    // decodeWaterfallLineDuration's ok-guard + FlexLib's TryParseDouble+continue.
    const auto dbm = [nan](const QString& s) {
        bool ok = false;
        const double v = s.toDouble(&ok);
        return ok ? v : nan;
    };
    const double minDbm = kvs.contains(QStringLiteral("min_dbm"))
        ? dbm(kvs.value(QStringLiteral("min_dbm"))) : nan;
    const double maxDbm = kvs.contains(QStringLiteral("max_dbm"))
        ? dbm(kvs.value(QStringLiteral("max_dbm"))) : nan;
    emit panRangeChanged(panId, minDbm, maxDbm);
}

void FlexBackend::decodePanRfGain(const QString& panId,
                                  const QMap<QString, QString>& kvs)
{
    if (!kvs.contains(QStringLiteral("rfgain"))) {
        return;
    }
    // Guard the parse — a malformed rfgain must be ignored, not emitted as 0
    // (which setRfGain would apply as a real gain). Matches FlexLib's
    // int.TryParse+continue and the sibling decoders' ok-guards.
    bool ok = false;
    const int gain = kvs.value(QStringLiteral("rfgain")).toInt(&ok);
    if (ok) {
        emit panRfGainChanged(panId, gain);
    }
}

void FlexBackend::decodePanAntenna(const QString& panId,
                                   const QMap<QString, QString>& kvs)
{
    // Selected RX antenna and the available list arrive independently — emit
    // each only when its wire key is present (matches the old applyPanStatus).
    if (kvs.contains(QStringLiteral("ant_list"))) {
        const QStringList ants =
            kvs.value(QStringLiteral("ant_list")).split(',', Qt::SkipEmptyParts);
        emit panAntennaListChanged(panId, ants);
    }
    if (kvs.contains(QStringLiteral("rxant"))) {
        emit panRxAntennaChanged(panId, kvs.value(QStringLiteral("rxant")));
    }
}

void FlexBackend::decodeWaterfallLineDuration(const QString& panId,
                                              const QMap<QString, QString>& kvs)
{
    if (!kvs.contains(QStringLiteral("line_duration"))) {
        return;
    }
    // Guard the numeric parse — a malformed line_duration must be ignored, not
    // applied as 0 (the old applyWaterfallStatus used toInt(&ok) + if(ok)).
    bool ok = false;
    const int ms = kvs.value(QStringLiteral("line_duration")).toInt(&ok);
    if (ok) {
        emit panWaterfallLineDurationChanged(panId, ms);
    }
}

void FlexBackend::decodePanState(const QString& panId,
                                 const QMap<QString, QString>& kvs)
{
    // Bundle the remaining Flex-specific display-pan keys onto one namespaced
    // extension event; carry only the keys the wire actually reported so the
    // model applies exactly what changed (present-only, like the WNB group).
    QVariantMap st;
    const auto carry = [&](const char* key) {
        if (kvs.contains(QLatin1String(key))) {
            st.insert(QLatin1String(key), kvs.value(QLatin1String(key)));
        }
    };
    // Raw strings — the model parses each with its existing per-field semantics
    // (bool flags, ok-guarded fps, hex client_handle, waterfall stream-id).
    carry("wide");
    carry("loopa");
    carry("loopb");
    carry("fps");
    carry("pre");
    carry("daxiq_channel");
    carry("client_handle");
    carry("waterfall");
    if (!st.isEmpty()) {
        st.insert(QStringLiteral("panId"), panId);
        emit extensionStatus(QStringLiteral("flex"),
                             QStringLiteral("panState"), st);
    }
}

void FlexBackend::decodePanExtensions(const QString& panId,
                                      const QMap<QString, QString>& kvs)
{
    // WNB (wideband noise blanker) is a Flex-specific pan feature, not core
    // profile — carry only the keys the wire reported, namespaced under "flex".
    QVariantMap wnb;
    if (kvs.contains(QStringLiteral("wnb"))) {
        wnb.insert(QStringLiteral("wnb"),
                   kvs.value(QStringLiteral("wnb")).toInt() != 0);
    }
    if (kvs.contains(QStringLiteral("wnb_level"))) {
        // Guard the numeric parse: a malformed/non-numeric wnb_level must be
        // ignored, not applied as 0. The old inline applyPanStatus path did
        // exactly this (toInt(&ok) + if(ok)), mirroring FlexLib's own
        // uint.TryParse + skip-on-failure (Panadapter.cs:1244). Dropping the
        // guard would silently snap the WNB-level UI to 0 (Principle VII).
        bool ok = false;
        const int level = kvs.value(QStringLiteral("wnb_level")).toInt(&ok);
        if (ok) {
            wnb.insert(QStringLiteral("wnb_level"), level);
        }
    }
    if (kvs.contains(QStringLiteral("wnb_updating"))) {
        // FlexLib v4.2.18 exposes wnb_updating on display pan status while the
        // radio normalizes the SCU-level WNB threshold; it is distinct from the
        // per-pan WNB enable flag ("wnb") above — keep them separate.
        wnb.insert(QStringLiteral("wnb_updating"),
                   kvs.value(QStringLiteral("wnb_updating")).toInt() != 0);
    }
    if (!wnb.isEmpty()) {
        wnb.insert(QStringLiteral("panId"), panId);
        emit extensionStatus(QStringLiteral("flex"),
                             QStringLiteral("panWnb"), wnb);
    }
}

void FlexBackend::decodeMeterStatus(const QString& rawBody)
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

    // Group tokens by meter index.
    QMap<int, QMap<QString, QString>> grouped;
    const QStringList tokens = rawBody.split('#', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const int dot = token.indexOf('.');
        if (dot < 0) continue;
        const int eq = token.indexOf('=', dot);
        if (eq < 0) continue;
        bool ok = false;
        const int idx = token.left(dot).toInt(&ok);
        if (!ok) continue;
        grouped[idx][token.mid(dot + 1, eq - dot - 1)] = token.mid(eq + 1);
    }

    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        const QMap<QString, QString>& f = it.value();
        // Carry only the keys the wire reported, normalized to the MeterDef
        // field names RadioModel reconstructs (present-only — the old parse
        // set MeterDef fields conditionally the same way).
        // Numeric fields are ok-guarded: a malformed present value is dropped,
        // not applied as 0/0.0 (a bad low/hi would otherwise collapse a meter's
        // scale). Consistent with the slice/pan/transmit boundary decoders and
        // FlexLib's TryParse+continue. (#4066 deferred, folded into #PR D.)
        QVariantMap def;
        if (f.contains(QStringLiteral("src")))
            def.insert(QStringLiteral("source"), f.value(QStringLiteral("src")));
        if (f.contains(QStringLiteral("num"))) {
            bool ok = false;
            const int v = f.value(QStringLiteral("num")).toInt(&ok, 0);  // base 0
            if (ok) def.insert(QStringLiteral("sourceIndex"), v);
        }
        if (f.contains(QStringLiteral("nam")))
            def.insert(QStringLiteral("name"), f.value(QStringLiteral("nam")));
        if (f.contains(QStringLiteral("unit")))
            def.insert(QStringLiteral("unit"), f.value(QStringLiteral("unit")));
        if (f.contains(QStringLiteral("low"))) {
            bool ok = false;
            const double v = f.value(QStringLiteral("low")).toDouble(&ok);
            if (ok) def.insert(QStringLiteral("low"), v);
        }
        if (f.contains(QStringLiteral("hi"))) {
            bool ok = false;
            const double v = f.value(QStringLiteral("hi")).toDouble(&ok);
            if (ok) def.insert(QStringLiteral("high"), v);
        }
        if (f.contains(QStringLiteral("desc")))
            def.insert(QStringLiteral("description"), f.value(QStringLiteral("desc")));
        emit meterDefined(it.key(), def);
    }
}

void FlexBackend::decodeSliceStatus(int sliceId, const QMap<QString, QString>& kvs)
{
    // Translate the Flex slice-status wire kv-set into the normalized, typed
    // SliceDelta. This owns ALL the SmartSDR-specific knowledge — the wire key
    // names, "1"→bool, comma-split lists, lowercase normalization — so
    // SliceModel::applyChanges speaks only the vendor-neutral typed fields.
    // Present-only: each delta field is engaged only when its wire key was
    // reported. Numeric parses are ok-guarded (a malformed *present* field is
    // dropped, not applied as 0/0.0 — this is the Flex slice validation boundary,
    // where a garbled RF_frequency would otherwise retune to 0 Hz; FlexLib itself
    // fails closed via TryParse+continue). #4068 review.
    SliceDelta d;
    const auto oStr = [&](const char* wire, std::optional<QString>& f) {
        if (kvs.contains(QLatin1String(wire))) f = kvs.value(QLatin1String(wire));
    };
    const auto oInt = [&](const char* wire, std::optional<int>& f) {
        if (kvs.contains(QLatin1String(wire))) {
            bool ok = false;
            const int v = kvs.value(QLatin1String(wire)).toInt(&ok);
            if (ok) f = v;
        }
    };
    const auto oReal = [&](const char* wire, std::optional<double>& f) {
        if (kvs.contains(QLatin1String(wire))) {
            bool ok = false;
            const double v = kvs.value(QLatin1String(wire)).toDouble(&ok);
            if (ok) f = v;
        }
    };
    const auto oBool = [&](const char* wire, std::optional<bool>& f) {
        if (kvs.contains(QLatin1String(wire)))
            f = kvs.value(QLatin1String(wire)) == QLatin1String("1");
    };
    const auto splitList = [](const QString& raw) {
        QStringList out;
        for (QString t : raw.split(',', Qt::SkipEmptyParts)) {
            t = t.trimmed();
            if (!t.isEmpty()) out.append(t);
        }
        return out;
    };

    // Identity / tuning
    oStr("pan", d.panId);
    oStr("index_letter", d.letter);
    oReal("RF_frequency", d.frequency);
    oStr("mode", d.mode);
    oInt("filter_lo", d.filterLow);
    oInt("filter_hi", d.filterHigh);
    if (kvs.contains(QStringLiteral("mode_list")))
        d.modeList = kvs.value(QStringLiteral("mode_list")).split(',', Qt::SkipEmptyParts);

    // Core state
    oBool("active", d.active);
    oBool("tx", d.txSlice);
    oReal("rfgain", d.rfGain);
    oReal("audio_level", d.audioGain);
    oInt("audio_pan", d.audioPan);
    oBool("audio_mute", d.audioMute);
    oBool("in_use", d.inUse);
    oBool("lock", d.locked);
    oBool("qsk", d.qsk);

    // Diversity group
    oBool("diversity_child", d.diversityChild);
    oBool("diversity_parent", d.diversityParent);
    oBool("diversity", d.diversity);
    oInt("diversity_index", d.diversityIndex);

    // ESC (diversity beamforming — "1"/"on" → true)
    if (kvs.contains(QStringLiteral("esc"))) {
        const QString v = kvs.value(QStringLiteral("esc"));
        d.esc = v == QLatin1String("1") || v == QLatin1String("on");
    }
    oReal("esc_gain", d.escGain);
    oReal("esc_phase_shift", d.escPhaseShift);

    // Antennas (rx_ant_list takes precedence over ant_list, then split+trim)
    if (kvs.contains(QStringLiteral("rx_ant_list")) || kvs.contains(QStringLiteral("ant_list")))
        d.rxAntennaList = splitList(kvs.value(QStringLiteral("rx_ant_list"),
                                              kvs.value(QStringLiteral("ant_list"))));
    if (kvs.contains(QStringLiteral("tx_ant_list")))
        d.txAntennaList = splitList(kvs.value(QStringLiteral("tx_ant_list")));
    oStr("rxant", d.rxAntenna);
    oStr("txant", d.txAntenna);

    // DSP toggles
    oBool("nb", d.nb);
    oBool("nr", d.nr);
    oBool("anf", d.anf);
    oBool("nrl", d.nrl);
    oBool("nrs", d.nrs);
    oBool("rnn", d.rnn);
    oBool("nrf", d.nrf);
    oBool("anfl", d.anfl);
    oBool("anft", d.anft);
    oBool("apf", d.apf);
    // DSP levels
    oInt("apf_level", d.apfLevel);
    oInt("nb_level", d.nbLevel);
    oInt("nr_level", d.nrLevel);
    oInt("anf_level", d.anfLevel);
    oInt("lms_nr_level", d.nrlLevel);
    oInt("speex_nr_level", d.nrsLevel);
    oInt("nrf_level", d.nrfLevel);
    oInt("lms_anf_level", d.anflLevel);

    // AGC / squelch / RIT / XIT
    oStr("agc_mode", d.agcMode);
    oInt("agc_threshold", d.agcThreshold);
    oInt("agc_off_level", d.agcOffLevel);
    oBool("squelch", d.squelchOn);
    oInt("squelch_level", d.squelchLevel);
    oBool("rit_on", d.ritOn);
    oInt("rit_freq", d.ritFreq);
    oBool("xit_on", d.xitOn);
    oInt("xit_freq", d.xitFreq);

    // DAX / RTTY / DIG offsets
    oInt("dax", d.daxChannel);
    oInt("rtty_mark", d.rttyMark);
    oInt("rtty_shift", d.rttyShift);
    oInt("digl_offset", d.diglOffset);
    oInt("digu_offset", d.diguOffset);

    // Record / playback (play is 3-state disabled/1/0 — carry raw, model interprets)
    oBool("record", d.recordOn);
    oStr("play", d.play);

    // FM duplex/repeater (lowercase normalization stays wire-side)
    if (kvs.contains(QStringLiteral("fm_tone_mode")))
        d.fmToneMode = kvs.value(QStringLiteral("fm_tone_mode")).toLower();
    oReal("fm_tone_value", d.fmToneValue);  // model formats to 1 decimal
    if (kvs.contains(QStringLiteral("repeater_offset_dir")))
        d.repeaterOffsetDir = kvs.value(QStringLiteral("repeater_offset_dir")).toLower();
    oReal("fm_repeater_offset_freq", d.fmRepeaterOffsetFreq);
    oReal("tx_offset_freq", d.txOffsetFreq);
    oInt("fm_deviation", d.fmDeviation);

    // Step (step_list carried raw — model builds the QVector<int>)
    oInt("step", d.step);
    oStr("step_list", d.stepList);

    emit sliceChanged(sliceId, d);
}

// ── Transmit-family decoders (aetherd RFC 2.3 — TransmitModel touchpoint) ──
// Each translates its Flex status plane into the typed TransmitDelta and emits
// transmitChanged. Numeric parses are ok-guarded (malformed present field is
// dropped, not applied as 0) and clamped to the model's ranges — the wire
// normalization the old TransmitModel decoders did inline.
namespace {
// present-only, ok-guarded carriers over a Flex kv-set.
inline void tBool(const QMap<QString, QString>& kvs, const char* wire,
                  std::optional<bool>& f) {
    if (kvs.contains(QLatin1String(wire)))
        f = kvs.value(QLatin1String(wire)) == QLatin1String("1");
}
inline void tInt(const QMap<QString, QString>& kvs, const char* wire,
                 std::optional<int>& f) {
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok);
        if (ok) f = v;
    }
}
inline void tClamp(const QMap<QString, QString>& kvs, const char* wire,
                   std::optional<int>& f, int lo, int hi) {
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const int v = kvs.value(QLatin1String(wire)).toInt(&ok);
        if (ok) f = qBound(lo, v, hi);
    }
}
inline void tReal(const QMap<QString, QString>& kvs, const char* wire,
                  std::optional<double>& f) {
    if (kvs.contains(QLatin1String(wire))) {
        bool ok = false;
        const double v = kvs.value(QLatin1String(wire)).toDouble(&ok);
        if (ok) f = v;
    }
}
}  // namespace

void FlexBackend::decodeTransmitStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    // Core transmit
    tClamp(kvs, "rfpower", d.rfPower, 0, 100);
    tClamp(kvs, "tunepower", d.tunePower, 0, 100);
    tBool(kvs, "tune", d.tune);
    tBool(kvs, "mox", d.mox);
    tReal(kvs, "freq", d.transmitFreq);

    // Mic / monitor / processor
    if (kvs.contains(QStringLiteral("mic_selection")))
        d.micSelection = kvs.value(QStringLiteral("mic_selection")).toUpper();
    tClamp(kvs, "mic_level", d.micLevel, 0, 100);
    tBool(kvs, "mic_acc", d.micAcc);
    tBool(kvs, "speech_processor_enable", d.speechProcEnable);
    tClamp(kvs, "speech_processor_level", d.speechProcLevel, 0, 100);
    tBool(kvs, "compander", d.compander);
    tClamp(kvs, "compander_level", d.companderLevel, 0, 100);
    tBool(kvs, "dax", d.dax);
    tBool(kvs, "sb_monitor", d.sbMonitor);
    tClamp(kvs, "mon_gain_sb", d.monGainSb, 0, 100);

    // VOX / phone
    tBool(kvs, "vox_enable", d.voxEnable);
    tClamp(kvs, "vox_level", d.voxLevel, 0, 100);
    tClamp(kvs, "vox_delay", d.voxDelay, 0, 100);
    tBool(kvs, "mic_boost", d.micBoost);
    tBool(kvs, "mic_bias", d.micBias);
    tBool(kvs, "met_in_rx", d.metInRx);
    tBool(kvs, "synccwx", d.syncCwx);
    tClamp(kvs, "am_carrier_level", d.amCarrierLevel, 0, 100);
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
    tClamp(kvs, "lo", d.txFilterLow, 0, 10000);
    tClamp(kvs, "hi", d.txFilterHigh, 0, 10000);

    // CW
    tClamp(kvs, "speed", d.cwSpeed, 5, 100);
    tClamp(kvs, "pitch", d.cwPitch, 100, 6000);
    tBool(kvs, "break_in", d.cwBreakIn);
    tClamp(kvs, "break_in_delay", d.cwDelay, 0, 2000);
    tBool(kvs, "sidetone", d.cwSidetone);
    tBool(kvs, "iambic", d.cwIambic);
    tClamp(kvs, "iambic_mode", d.cwIambicMode, 0, 1);
    tBool(kvs, "swap_paddles", d.cwSwapPaddles);
    tBool(kvs, "cwl_enabled", d.cwlEnabled);
    tClamp(kvs, "mon_gain_cw", d.monGainCw, 0, 100);
    tClamp(kvs, "mon_pan_cw", d.monPanCw, 0, 100);

    // Misc TX
    tInt(kvs, "max_power_level", d.maxPowerLevel);
    if (kvs.contains(QStringLiteral("tune_mode")))
        d.tuneMode = kvs.value(QStringLiteral("tune_mode"));
    tBool(kvs, "show_tx_in_waterfall", d.showTxInWaterfall);
    if (kvs.contains(QStringLiteral("tx_slice_mode")))
        d.txSliceMode = kvs.value(QStringLiteral("tx_slice_mode"));

    emit transmitChanged(d);
}

void FlexBackend::decodeInterlockStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    tInt(kvs, "acc_tx_delay", d.accTxDelay);
    tInt(kvs, "tx1_delay", d.tx1Delay);
    tInt(kvs, "tx2_delay", d.tx2Delay);
    tInt(kvs, "tx3_delay", d.tx3Delay);
    tInt(kvs, "tx_delay", d.txDelay);
    tInt(kvs, "timeout", d.interlockTimeout);
    tInt(kvs, "acc_txreq_polarity", d.accTxReqPolarity);
    tInt(kvs, "rca_txreq_polarity", d.rcaTxReqPolarity);
    emit transmitChanged(d);
}

void FlexBackend::decodeAtuStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    // Raw ATU status token — the model owns the ATUStatus enum + parse.
    if (kvs.contains(QStringLiteral("status")))
        d.atuStatusRaw = kvs.value(QStringLiteral("status"));
    tBool(kvs, "atu_enabled", d.atuEnabled);
    tBool(kvs, "memories_enabled", d.memoriesEnabled);
    tBool(kvs, "using_mem", d.usingMemory);
    emit transmitChanged(d);
}

void FlexBackend::decodeApdStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    tBool(kvs, "enable", d.apdEnabled);
    tBool(kvs, "configurable", d.apdConfigurable);
    tBool(kvs, "equalizer_active", d.apdEqActive);
    // Bare flag (no `=`): the model clears apdEqActive + emits the reset signal.
    if (kvs.contains(QStringLiteral("equalizer_reset")))
        d.apdEqualizerReset = true;
    emit transmitChanged(d);
}

void FlexBackend::decodeApdSamplerStatus(const QMap<QString, QString>& kvs)
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

void FlexBackend::send(const QString& cmd)
{
    if (m_sink) {
        m_sink(cmd);
    }
}

void FlexBackend::sendSlice(const QString& cmd)
{
    if (m_sliceSink) {
        m_sliceSink(cmd);
    } else if (m_sink) {
        m_sink(cmd);
    }
}

}  // namespace AetherSDR
