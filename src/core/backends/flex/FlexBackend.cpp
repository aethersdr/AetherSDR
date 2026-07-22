#include "core/backends/flex/FlexBackend.h"

#include <limits>

#include <QThread>

#include "core/LogManager.h"
#include "core/RadioConnection.h"
#include "core/PanadapterStream.h"
#include "core/backends/flex/FlexKvCarry.h"
#include "models/ModelCapabilities.h"

namespace AetherSDR {

// Shared present-only + ok-guarded Flex status carriers (#4070), used by the
// pan/slice/meter/transmit decoders below (one overloaded carry() family).
using namespace flexkv;

namespace {

QStringList uniqueCommaList(const QString& value)
{
    QStringList result;
    const QStringList parts = value.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString item = part.trimmed();
        if (!item.isEmpty() && !result.contains(item)) {
            result.append(item);
        }
    }
    return result;
}

} // namespace

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

    // Advertise the "flex" extension namespace: the amp/tuner operate/bypass/
    // autotune verbs are now routed through invokeExtension() (#4092/#4094), and
    // it honors the async contract — an awaited call (requestId != 0) always
    // gets exactly one extensionResult/Error, never a hang.
    caps.extensionNamespaces << QStringLiteral("flex");
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

void FlexBackend::sendSliceWaveformCommand(int sliceId, const QString& command)
{
    if (sliceId < 0 || command.trimmed().isEmpty()) {
        return;
    }
    sendSlice(QStringLiteral("slice waveform_cmd %1 %2")
                  .arg(sliceId)
                  .arg(command));
}

void FlexBackend::setKeying(bool key)
{
    // Keying is only translated here; the interlock/authorization decision is
    // made above the seam (RFC §6). Matches RadioModel::setTransmit's wire form.
    send(QStringLiteral("xmit %1").arg(key ? 1 : 0));
}

void FlexBackend::invokeExtension(const QString& ns, const QString& verb,
                                  quint64 requestId, const QVariant& arg)
{
    // Translate a neutral amp/tuner intent (#4092/#4094) into the SmartSDR relay
    // wire. The device object handle is a Flex detail resolved from this backend's
    // own decode-side state (m_ampHandle/m_tunerHandle, #4198) — the intent no
    // longer carries it. Async contract: an awaited call (requestId != 0) gets
    // exactly one reply, never a hang.
    const auto fail = [&](const QString& why) {
        if (requestId != 0)
            emit extensionError(requestId, why);
    };
    if (ns != QLatin1String("flex")) {
        fail(QStringLiteral("unknown extension namespace: %1").arg(ns));
        return;
    }

    const bool on = arg.toMap().value(QStringLiteral("on")).toBool();
    QString cmd;
    if (verb == QLatin1String("amp.operate")) {
        if (m_ampHandle.isEmpty()) { fail(QStringLiteral("flex amp.operate: no amp handle")); return; }
        cmd = QStringLiteral("amplifier set %1 operate=%2").arg(m_ampHandle).arg(on ? 1 : 0);
    } else if (verb == QLatin1String("tuner.operate")) {
        if (m_tunerHandle.isEmpty()) { fail(QStringLiteral("flex tuner.operate: no tuner handle")); return; }
        cmd = QStringLiteral("tgxl set handle=%1 mode=%2").arg(m_tunerHandle).arg(on ? 1 : 0);
    } else if (verb == QLatin1String("tuner.bypass")) {
        if (m_tunerHandle.isEmpty()) { fail(QStringLiteral("flex tuner.bypass: no tuner handle")); return; }
        cmd = QStringLiteral("tgxl set handle=%1 bypass=%2").arg(m_tunerHandle).arg(on ? 1 : 0);
    } else if (verb == QLatin1String("tuner.autotune")) {
        if (m_tunerHandle.isEmpty()) { fail(QStringLiteral("flex tuner.autotune: no tuner handle")); return; }
        cmd = QStringLiteral("tgxl autotune handle=%1").arg(m_tunerHandle);
    } else {
        fail(QStringLiteral("unknown flex verb: %1").arg(verb));
        return;
    }

    send(cmd);
    // Fire-and-forget on the wire: the real device state returns asynchronously
    // via the amplifier/tgxl status decode. Acknowledge dispatch so an awaiting
    // caller (requestId != 0) completes; our own RadioModel routes pass 0.
    if (requestId != 0)
        emit extensionResult(requestId, QVariant(true));
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
    // Raw strings via the shared flexkv map-target carry — the model parses each
    // with its existing per-field semantics (bool flags, ok-guarded fps, hex
    // client_handle, waterfall stream-id).
    QVariantMap st;
    carry(kvs, "wide", st);
    carry(kvs, "loopa", st);
    carry(kvs, "loopb", st);
    carry(kvs, "fps", st);
    carry(kvs, "average", st);
    carry(kvs, "weighted_average", st);
    carry(kvs, "pre", st);
    carry(kvs, "daxiq_channel", st);
    carry(kvs, "client_handle", st);
    carry(kvs, "waterfall", st);
    // Radio-owned zoom-mode flags (#4057); the model mirrors FlexLib's
    // uint-parse + >1-invalid semantics (Panadapter.cs 933/1159).
    carry(kvs, "band_zoom", st);
    carry(kvs, "segment_zoom", st);
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
    // All three keys mirror FlexLib's guarded parses (#4147 audit): a malformed
    // or out-of-range value is dropped from the carry (the model keeps
    // last-known-good), never coerced to false/0.
    QVariantMap wnb;
    if (kvs.contains(QStringLiteral("wnb"))) {
        // FlexLib Panadapter.cs:1226 — uint.TryParse + reject > 1, skip on
        // failure. Bare toInt() != 0 turned "wnb=bogus" into false and could
        // silently switch the noise blanker indicator off.
        bool ok = false;
        const uint v = kvs.value(QStringLiteral("wnb")).toUInt(&ok);
        if (ok && v <= 1) {
            wnb.insert(QStringLiteral("wnb"), v != 0);
        } else {
            qCDebug(lcProtocol) << "FlexBackend: invalid wnb value"
                                << kvs.value(QStringLiteral("wnb"));
        }
    }
    if (kvs.contains(QStringLiteral("wnb_level"))) {
        // FlexLib Panadapter.cs:1244 — uint.TryParse (negatives fail to parse)
        // + reject > 100, skip on failure: an out-of-range level keeps the last
        // known-good value rather than being clamped into range (the old signed
        // toInt(&ok) accepted negatives and left > 100 to a model-side clamp,
        // fabricating levels FlexLib refuses; Principle VII).
        bool ok = false;
        const uint level = kvs.value(QStringLiteral("wnb_level")).toUInt(&ok);
        if (ok && level <= 100) {
            wnb.insert(QStringLiteral("wnb_level"), int(level));
        } else {
            qCDebug(lcProtocol) << "FlexBackend: invalid wnb_level value"
                                << kvs.value(QStringLiteral("wnb_level"));
        }
    }
    if (kvs.contains(QStringLiteral("wnb_updating"))) {
        // FlexLib v4.2.18 exposes wnb_updating on display pan status while the
        // radio normalizes the SCU-level WNB threshold; it is distinct from the
        // per-pan WNB enable flag ("wnb") above — keep them separate. Same
        // guarded parse as wnb (Panadapter.cs:1262, uint.TryParse + > 1 reject).
        bool ok = false;
        const uint v = kvs.value(QStringLiteral("wnb_updating")).toUInt(&ok);
        if (ok && v <= 1) {
            wnb.insert(QStringLiteral("wnb_updating"), v != 0);
        } else {
            qCDebug(lcProtocol) << "FlexBackend: invalid wnb_updating value"
                                << kvs.value(QStringLiteral("wnb_updating"));
        }
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
        def.index = it.key();
        carry(f, "src", def.source);
        carry(f, "num", def.sourceIndex, /*base=*/0);
        carry(f, "nam", def.name);
        carry(f, "unit", def.unit);
        carry(f, "low", def.low);
        carry(f, "hi", def.high);
        carry(f, "desc", def.description);
        emit meterDefined(def);
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
    // The shared flexkv carriers (overloaded carry() / carryClamp() / splitList,
    // in scope via the file-scope `using namespace flexkv`) are called directly.

    // Identity / tuning
    carry(kvs, "pan", d.panId);
    carry(kvs, "index_letter", d.letter);
    carry(kvs, "RF_frequency", d.frequency);
    carry(kvs, "mode", d.mode);
    carry(kvs, "filter_lo", d.filterLow);
    carry(kvs, "filter_hi", d.filterHigh);
    if (kvs.contains(QStringLiteral("mode_list"))) {
        d.modeList = uniqueCommaList(kvs.value(QStringLiteral("mode_list")));
    }

    // Core state
    carry(kvs, "active", d.active);
    carry(kvs, "tx", d.txSlice);
    carry(kvs, "rfgain", d.rfGain);
    carry(kvs, "audio_level", d.audioGain);
    carry(kvs, "audio_pan", d.audioPan);
    carry(kvs, "audio_mute", d.audioMute);
    carry(kvs, "in_use", d.inUse);
    carry(kvs, "lock", d.locked);
    carry(kvs, "qsk", d.qsk);

    // Diversity group
    carry(kvs, "diversity_child", d.diversityChild);
    carry(kvs, "diversity_parent", d.diversityParent);
    carry(kvs, "diversity", d.diversity);
    carry(kvs, "diversity_index", d.diversityIndex);

    // ESC (diversity beamforming — "1"/"on" → true)
    if (kvs.contains(QStringLiteral("esc"))) {
        const QString v = kvs.value(QStringLiteral("esc"));
        d.esc = v == QLatin1String("1") || v == QLatin1String("on");
    }
    carry(kvs, "esc_gain", d.escGain);
    carry(kvs, "esc_phase_shift", d.escPhaseShift);

    // Antennas (rx_ant_list takes precedence over ant_list, then split+trim)
    if (kvs.contains(QStringLiteral("rx_ant_list")) || kvs.contains(QStringLiteral("ant_list")))
        d.rxAntennaList = splitList(kvs.value(QStringLiteral("rx_ant_list"),
                                              kvs.value(QStringLiteral("ant_list"))));
    if (kvs.contains(QStringLiteral("tx_ant_list")))
        d.txAntennaList = splitList(kvs.value(QStringLiteral("tx_ant_list")));
    carry(kvs, "rxant", d.rxAntenna);
    carry(kvs, "txant", d.txAntenna);

    // DSP toggles
    carry(kvs, "nb", d.nb);
    carry(kvs, "nr", d.nr);
    carry(kvs, "anf", d.anf);
    carry(kvs, "nrl", d.nrl);
    carry(kvs, "nrs", d.nrs);
    carry(kvs, "rnn", d.rnn);
    carry(kvs, "nrf", d.nrf);
    carry(kvs, "anfl", d.anfl);
    carry(kvs, "anft", d.anft);
    carry(kvs, "apf", d.apf);
    // DSP levels
    carry(kvs, "apf_level", d.apfLevel);
    carry(kvs, "nb_level", d.nbLevel);
    carry(kvs, "nr_level", d.nrLevel);
    carry(kvs, "anf_level", d.anfLevel);
    carry(kvs, "lms_nr_level", d.nrlLevel);
    carry(kvs, "speex_nr_level", d.nrsLevel);
    carry(kvs, "nrf_level", d.nrfLevel);
    carry(kvs, "lms_anf_level", d.anflLevel);

    // AGC / squelch / RIT / XIT
    carry(kvs, "agc_mode", d.agcMode);
    carry(kvs, "agc_threshold", d.agcThreshold);
    carry(kvs, "agc_off_level", d.agcOffLevel);
    carry(kvs, "squelch", d.squelchOn);
    carry(kvs, "squelch_level", d.squelchLevel);
    carry(kvs, "rit_on", d.ritOn);
    carry(kvs, "rit_freq", d.ritFreq);
    carry(kvs, "xit_on", d.xitOn);
    carry(kvs, "xit_freq", d.xitFreq);

    // DAX / RTTY / DIG offsets
    carry(kvs, "dax", d.daxChannel);
    carry(kvs, "rtty_mark", d.rttyMark);
    carry(kvs, "rtty_shift", d.rttyShift);
    carry(kvs, "digl_offset", d.diglOffset);
    carry(kvs, "digu_offset", d.diguOffset);

    // Record / playback (play is 3-state disabled/1/0 — carry raw, model interprets)
    carry(kvs, "record", d.recordOn);
    carry(kvs, "play", d.play);

    // FM duplex/repeater (lowercase normalization stays wire-side)
    if (kvs.contains(QStringLiteral("fm_tone_mode")))
        d.fmToneMode = kvs.value(QStringLiteral("fm_tone_mode")).toLower();
    carry(kvs, "fm_tone_value", d.fmToneValue);  // model formats to 1 decimal
    if (kvs.contains(QStringLiteral("repeater_offset_dir")))
        d.repeaterOffsetDir = kvs.value(QStringLiteral("repeater_offset_dir")).toLower();
    carry(kvs, "fm_repeater_offset_freq", d.fmRepeaterOffsetFreq);
    carry(kvs, "tx_offset_freq", d.txOffsetFreq);
    carry(kvs, "fm_deviation", d.fmDeviation);

    // Step (step_list carried raw — model builds the QVector<int>)
    carry(kvs, "step", d.step);
    carry(kvs, "step_list", d.stepList);

    emit sliceChanged(sliceId, d);
}

// ── Transmit-family decoders (aetherd RFC 2.3 — TransmitModel touchpoint) ──
// Each translates its Flex status plane into the typed TransmitDelta and emits
// transmitChanged. Numeric parses are ok-guarded (malformed present field is
// dropped, not applied as 0) and clamped to the model's ranges — the wire
// normalization the old TransmitModel decoders did inline.
void FlexBackend::decodeTransmitStatus(const QMap<QString, QString>& kvs)
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

void FlexBackend::decodeInterlockStatus(const QMap<QString, QString>& kvs)
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
    emit transmitChanged(d);
}

void FlexBackend::decodeAtuStatus(const QMap<QString, QString>& kvs)
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

void FlexBackend::decodeAmplifierStatus(const QString& handle, const QString& model,
                                        const QMap<QString, QString>& kvs, bool removed)
{
    // Stateless translation of the SmartSDR "amplifier <handle> …" wire → AmpDelta
    // (#4094). The presence latch, operate change-gating, and handle matching are
    // the model's job (AmpModel::applyChanges) — this only reports what the wire
    // said. Command/encode is the reverse path — invokeExtension("flex",
    // "amp.operate", …) below translates AmpModel's neutral intent (#4094).
    AmpDelta d;
    d.handle = handle;
    if (removed) {
        // Drop the cached handle for the encode path (#4198) when the device it
        // names goes away. TGXL removal also arrives on the amplifier-removed
        // wire (routed here by RadioModel), so clear whichever handle matches.
        if (handle == m_ampHandle) m_ampHandle.clear();
        if (handle == m_tunerHandle) m_tunerHandle.clear();
        d.removed = true;
        emit amplifierChanged(d);
        return;
    }
    // RadioModel routes only power amps (PGXL) into this decode, so the handle is
    // the amp's — cache it for the encode path (#4198). Ignore the placeholder
    // handle a first status can carry before the real one is assigned. Defense in
    // depth (#4203): a pre-existing routing edge — a model-less TGXL status arriving
    // before its handle is known — can fall through to here; refuse to cache a
    // known-tuner handle so a later amp.operate can never mis-target the TGXL.
    if (!handle.isEmpty() && handle != QLatin1String("0x00000000") && handle != m_tunerHandle)
        m_ampHandle = handle;
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

void FlexBackend::decodeTunerStatus(const QString& handle, const QMap<QString, QString>& kvs)
{
    // Cache the TGXL handle for the encode path (#4198). RadioModel passes the
    // handle it already extracted+sanitized (never the 0x00000000 placeholder),
    // so the tuner intents no longer carry a Flex identifier through the seam.
    if (!handle.isEmpty() && handle != QLatin1String("0x00000000"))
        m_tunerHandle = handle;
    // Present-only, strict parity with the prior TunerModel::applyStatus: bools
    // are "1"-equality, ints are unguarded toInt() (matching val.toInt()), text
    // is verbatim. The change-gating / edge signals live in TunerModel::applyChanges.
    TunerDelta d;
    if (kvs.contains(QStringLiteral("serial_num")))
        d.serialNum = kvs.value(QStringLiteral("serial_num"));
    if (kvs.contains(QStringLiteral("model")))
        d.model = kvs.value(QStringLiteral("model"));
    if (kvs.contains(QStringLiteral("ip")))
        d.ip = kvs.value(QStringLiteral("ip"));
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
    emit tunerChanged(d);
}

void FlexBackend::clearExtensionHandles()
{
    // #4198: forget the amp/tuner encode handles on disconnect/reset so a stale
    // handle can't survive into a reconnect (possibly a different radio).
    m_ampHandle.clear();
    m_tunerHandle.clear();
}

void FlexBackend::decodeApdStatus(const QMap<QString, QString>& kvs)
{
    TransmitDelta d;
    carry(kvs, "enable", d.apdEnabled);
    carry(kvs, "configurable", d.apdConfigurable);
    carry(kvs, "equalizer_active", d.apdEqActive);
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

void FlexBackend::decodeRadioStatus(const QMap<QString, QString>& kvs)
{
    // Radio-global status → typed RadioDelta (aetherd RFC 2.3 — RadioModel
    // residual). Present-only, ok-guarded via the shared flexkv carriers; the
    // model-side orchestration (slice-capacity bounding, rtty→slices propagation,
    // TNF/DAX-IQ sub-models, the change-gated emits) stays in applyRadioChanges.
    RadioDelta d;
    // Identity / capability
    carry(kvs, "model", d.model);
    carry(kvs, "slices", d.slicesAvailable);
    carry(kvs, "callsign", d.callsign);
    carry(kvs, "nickname", d.nickname);
    carry(kvs, "region", d.region);
    carry(kvs, "radio_options", d.radioOptions);
    carry(kvs, "bands", d.bandsRaw);   // optional radio-declared bands (gateway/non-Flex); validated in RadioModel
    // Global flags
    carry(kvs, "remote_on_enabled", d.remoteOnEnabled);
    carry(kvs, "mf_enable", d.multiFlexEnabled);
    carry(kvs, "enforce_private_ip_connections", d.enforcePrivateIp);
    carry(kvs, "binaural_rx", d.binauralRx);
    carry(kvs, "full_duplex_enabled", d.fullDuplex);
    carry(kvs, "mute_local_audio_when_remote", d.muteLocalWhenRemote);
    carry(kvs, "auto_save", d.autoSave);
    carry(kvs, "low_latency_digital_modes", d.lowLatencyDigital);
    carry(kvs, "tnf_enabled", d.tnfEnabled);
    // Calibration / defaults
    carry(kvs, "freq_error_ppb", d.freqErrorPpb);
    carry(kvs, "cal_freq", d.calFreqMhz);
    carry(kvs, "rtty_mark_default", d.rttyMarkDefault);
    // Audio outputs
    carry(kvs, "lineout_gain", d.lineoutGain);
    carry(kvs, "lineout_mute", d.lineoutMute);
    carry(kvs, "headphone_gain", d.headphoneGain);
    carry(kvs, "headphone_mute", d.headphoneMute);
    carry(kvs, "front_speaker_mute", d.frontSpeakerMute);
    // DAX-IQ capacity
    carry(kvs, "daxiq_capacity", d.daxiqCapacity);
    carry(kvs, "daxiq_available", d.daxiqAvailable);
    emit radioChanged(d);
}

void FlexBackend::decodeGpsStatus(const QString& rawBody)
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

void FlexBackend::decodeMemoryStatus(int index, const QMap<QString, QString>& kvs)
{
    // Memory-slot status → typed MemoryDelta (aetherd RFC 2.3 — RadioModel
    // residual). Removal: the radio sends "in_use=0" or a bare "removed".
    MemoryDelta d;
    d.index = index;
    if (kvs.value(QStringLiteral("in_use")) == QLatin1String("0")
        || kvs.contains(QStringLiteral("removed"))) {
        d.removed = true;
        emit memoryChanged(d);
        return;
    }

    // Text fields ride raw — the protocol space-encoding (0x7f→' ') and the
    // NUL/control-byte sanitisation (MemoryFields) are a model concern applied in
    // RadioModel::applyMemoryChanges, so the backend keeps no models/ dependency.
    carry(kvs, "group", d.group);
    carry(kvs, "owner", d.owner);
    carry(kvs, "name", d.name);
    carry(kvs, "mode", d.mode);
    carry(kvs, "repeater", d.offsetDir);
    carry(kvs, "tone_mode", d.toneMode);
    // Numeric fields — ok-guarded (a malformed *present* value is dropped, so the
    // model keeps the slot's prior value rather than clobbering it with 0). The
    // old handler applied an unguarded toInt/toDouble (→0); the carry() guard is
    // the same fail-closed improvement made at the slice/transmit sites.
    carry(kvs, "freq", d.freq);
    carry(kvs, "repeater_offset", d.repeaterOffset);
    carry(kvs, "tone_value", d.toneValue);
    carry(kvs, "step", d.step);
    carry(kvs, "squelch", d.squelch);
    carry(kvs, "squelch_level", d.squelchLevel);
    carry(kvs, "rx_filter_low", d.rxFilterLow);
    carry(kvs, "rx_filter_high", d.rxFilterHigh);
    carry(kvs, "rtty_mark", d.rttyMark);
    carry(kvs, "rtty_shift", d.rttyShift);
    carry(kvs, "digl_offset", d.diglOffset);
    carry(kvs, "digu_offset", d.diguOffset);
    emit memoryChanged(d);
}

void FlexBackend::decodeProfileStatus(const QString& profileType, const QString& rawBody)
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

    d.type = profileType;
    if (key == QLatin1String("list"))
        d.list = val.split('^', Qt::SkipEmptyParts);
    else if (key == QLatin1String("current"))
        d.current = val;
    else
        return;   // unknown key for this type → nothing to apply
    emit profileChanged(d);
}

void FlexBackend::decodeProfileFlags(const QMap<QString, QString>& kvs)
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
