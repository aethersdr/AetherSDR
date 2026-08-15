#ifdef HAVE_WEBSOCKETS
#include "TciProtocol.h"
#include "AppSettings.h"
#include "LogManager.h"
#include "TciRoutingState.h"
#include "TciTrxMap.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#include "models/SpotModel.h"
#include "models/DaxIqModel.h"

#include <QList>
#include <QMetaObject>
#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {

int TciProtocol::tciTrxForSlice(RadioModel* model, const SliceModel* slice)
{
    if (!model || !slice)
        return 0;
    const auto slices = model->slices();
    const int index = slices.indexOf(const_cast<SliceModel*>(slice));
    return index >= 0 ? index : slice->sliceId();
}

QString TciProtocol::sanitizeSliceLetter(const QString& letter)
{
    QString clean;
    for (const QChar c : letter) {
        if (c.isLetterOrNumber())
            clean.append(c);
    }
    return clean.left(2);
}

// Reports only what TciServer has pushed. There is deliberately no scan
// fallback: -1 is not exclusively a startup state — TciServer also pushes it
// when the focused slice is removed, and in that window the optimistic-set
// overlap it was assumed to avoid IS possible (SliceModel::setActive() marks
// the incoming slice before the radio echoes active=0 on the outgoing one, so
// a scan can see two). Scanning there would answer a GET, or seed a newly
// connected client's init burst, with a slice that was never broadcast, so
// GET-polling and broadcast-listening clients would disagree about focus.
// Staying silent keeps every path consistent with the broadcast stream.
bool TciProtocol::resolveActiveSlice(int& trx, QString& letter) const
{
    if (m_activeTrx < 0)
        return false;
    trx = m_activeTrx;
    letter = m_activeLetter;
    return true;
}

long long TciProtocol::mhzToHz(double mhz)
{
    return static_cast<long long>(std::round(mhz * 1e6));
}

TciProtocol::TciProtocol(RadioModel* model, TciRoutingState* routingState,
                         const TciTrxMap* trxMap)
    : m_model(model)
    , m_routingState(routingState)
    , m_trxMap(trxMap)
{}

// ── Mode conversion ────────────────────────────────────────────────────────

QString TciProtocol::smartsdrToTci(const QString& mode)
{
    static const QMap<QString, QString> map = {
        {"USB",  "usb"},   {"LSB",  "lsb"},
        {"CW",   "cw"},    {"CWL",  "cwr"},
        {"AM",   "am"},    {"SAM",  "sam"},
        {"FM",   "fm"},    {"NFM",  "nfm"},
        {"DFM",  "fm"},    {"FDM",  "fm"},
        {"DIGU", "digu"},  {"DIGL", "digl"},
        {"RTTY", "rtty"},  {"FDV",  "digu"},
        {"FDVU", "digu"},  {"FDVL", "digl"},
    };
    return map.value(mode.toUpper(), "usb");
}

QString TciProtocol::tciToSmartSDR(const QString& mode, bool* ok)
{
    static const QMap<QString, QString> map = {
        {"usb",  "USB"},   {"lsb",  "LSB"},
        {"cw",   "CW"},    {"cwr",  "CWL"},
        {"am",   "AM"},    {"sam",  "SAM"},
        {"fm",   "FM"},    {"nfm",  "NFM"},
        {"digu", "DIGU"},  {"digl", "DIGL"},
        {"rtty", "RTTY"},
    };
    const auto it = map.find(mode.toLower());
    if (ok) *ok = (it != map.end());
    return it != map.end() ? it.value() : QStringLiteral("USB");
}

// ── Helpers ────────────────────────────────────────────────────────────────

// Argument parsing, in one place (#4867).
//
// QString::toInt() returns 0 on a parse failure, so an unchecked conversion
// does not fail — it turns malformed client input into a valid-looking 0 and
// the command proceeds as if the client had asked for it. That is the hard
// part of this defect class: it does not fail quietly, it *succeeds
// convincingly*. The command applies, a well-formed notification goes out,
// and neither the sender nor a second client watching the wire can tell
// anything went wrong. No crash, no log line, no assertion anywhere in the
// tree that moves. Three separate defects shipped that way and each was
// found by a human noticing odd radio behaviour: a DRIVE read applied as a
// power SET (#4345), `volume:` applied as 100% and `tx_gain:` as a mute
// (#4523).
//
// The trx index is the sharpest case. sliceForTrx() resolves positionally, so
// a malformed trx does not fail to resolve — it resolves to slice 0 and the
// command lands on the wrong slice while broadcasting a notification that
// names slice 0 as though the client had asked for it.
//
// Contract: `out` is left untouched when the argument is missing or
// unparseable, so a caller that pre-seeds a default keeps it. Callers drop
// the command on false — the "ignore silently per TCI spec" posture the
// parser already takes for unrecognised commands, and the remedy #4345 and
// #4523 both settled on. This rejects *unparseable* input only: range clamps
// and the deliberately lenient forms (cmdVolume's non-spec percent branch,
// which three bundled plugins depend on) are the caller's business and are
// unchanged.
//
// Base 10 only, and it must stay that way. QString::toInt()'s base argument
// defaults to 10, which is the only reason "0x1" is rejected rather than
// resolving to slice 1 — pass 0 here for "helpful" auto-base detection and
// `mute:0x1,false` silently starts landing on a slice the client never named.
// The test asserts the hex case; this sentence is why the assertion holds.
static bool argToInt(const QStringList& args, int idx, int& out)
{
    if (idx < 0 || idx >= args.size()) return false;
    bool ok = false;
    const int parsed = args[idx].toInt(&ok);
    if (!ok) return false;
    out = parsed;
    return true;
}

// Booleans are the same defect with a different literal, and there are 20 of
// them: `args[1].toLower() == "true"` turns *everything that is not the word
// "true"* into false, so `mute:0,yes` UNMUTED slice 0 and broadcast a
// well-formed "mute:0,false;" — #4867's signature exactly, with no number
// involved. TCI spells booleans "true"/"false"; this accepts those two
// (case-insensitively, as the old expression did) and reports failure for
// anything else so the caller can drop the command. Every bundled plugin
// sends Python's `str(bool).lower()` or JS's `${bool}`, i.e. exactly these
// two spellings, so nothing shipped is affected.
//
// DELIBERATELY NOT used by the two verbs that key the transmitter. `cmdTune`
// and `cmdKeyer` must fail CLOSED rather than fail silent: dropping an
// unparseable `tune:0,<junk>` would leave a tuning carrier up, and dropping
// `keyer:0,<junk>` would leave the key down. For those two, "not the word
// true" continuing to mean *stop* is the safe reading, and Constitution VI
// makes it the required one. They keep the bare comparison, with a comment
// at each site saying so.
static bool argToBool(const QStringList& args, int idx, bool& out)
{
    if (idx < 0 || idx >= args.size()) return false;
    const QString v = args[idx].trimmed().toLower();
    if (v == QLatin1String("true"))  { out = true;  return true; }
    if (v == QLatin1String("false")) { out = false; return true; }
    return false;
}

static bool argToLongLong(const QStringList& args, int idx, long long& out)
{
    if (idx < 0 || idx >= args.size()) return false;
    bool ok = false;
    const long long parsed = args[idx].toLongLong(&ok);
    if (!ok) return false;
    out = parsed;
    return true;
}

// std::isfinite is part of the check, not extra paranoia: Qt's toDouble()
// parses the literals "inf"/"-inf"/"nan" and reports ok, so the ok flag alone
// lets them through to the arithmetic. That is how `volume:inf` muted the
// radio until #4848 — lround(+inf) is out of long's range and the narrowing
// cast landed on 0. Overflow ("1e400") already clears ok; the literal
// spellings do not, which is exactly why this belongs in the shared helper
// rather than in each caller that remembers.
static bool argToDouble(const QStringList& args, int idx, double& out)
{
    if (idx < 0 || idx >= args.size()) return false;
    bool ok = false;
    const double parsed = args[idx].toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) return false;
    out = parsed;
    return true;
}

SliceModel* TciProtocol::sliceForTrx(int trx) const
{
    if (m_trxMap)
        return m_trxMap->sliceForTrx(m_model, trx);
    return resolveSliceForTrx(m_model, trx);
}

SliceModel* TciProtocol::resolveSliceForTrxStrict(RadioModel* model, int trx)
{
    if (!model || !model->isConnected()) return nullptr;
    const QList<SliceModel*> slices = model->slices();
    if (trx >= 0 && trx < slices.size())
        return slices.at(trx);

    // Compatibility fallback for clients that learned raw Flex slice ids from
    // older AetherSDR builds.
    for (auto* s : slices) {
        if (s && s->sliceId() == trx) return s;
    }
    return nullptr;
}

SliceModel* TciProtocol::resolveSliceForTrx(RadioModel* model, int trx)
{
    if (SliceModel* resolved = resolveSliceForTrxStrict(model, trx))
        return resolved;
    // Fallback: first slice. Read paths keep the guess (tci-receivers.md
    // rule 3); keying paths use the strict resolver instead (#4547).
    if (!model || !model->isConnected()) return nullptr;
    const QList<SliceModel*> slices = model->slices();
    return slices.isEmpty() ? nullptr : slices.first();
}

SliceModel* TciProtocol::sliceForVfo(int trx, int channel) const
{
    SliceModel* rxSlice = sliceForTrx(trx);
    if (!rxSlice || channel == 0 || !m_model) {
        return rxSlice;
    }

    if (m_routingState && m_routingState->txSliceId() >= 0) {
        if (SliceModel* routed = m_model->slice(m_routingState->txSliceId())) {
            return routed;
        }
    }
    if (SliceModel* txSlice = m_model->txSlice(); txSlice && txSlice != rxSlice) {
        return txSlice;
    }
    return rxSlice;
}

// TRX index of the TX slice for the request/response path — the init burst and
// the DRIVE/TUNE_DRIVE replies. Falls back to 0 when no slice is marked TX,
// because the wire needs a concrete index and 0 is what the burst has always
// used.
//
// TciServer has a near-twin, txTrxIndex(), which returns -1 for "none" and
// resolves that against a cached last-known TX trx. The difference is
// deliberate and worth understanding before merging them: that one serves the
// async broadcast, which is driven by the radio's own power restore during a
// band change — i.e. it fires *inside* the window where the recreated slice has
// not yet regained its TX flag, so without the cache it would reliably mislabel
// (#4161). This path is driven by a client command arriving at an arbitrary
// moment, so it only ever meets that window by coincidence, and a transiently
// 0-labelled reply is the same fallback the burst has always emitted.
int TciProtocol::txSliceTrxOrNone(RadioModel* model)
{
    if (!model) {
        return -1;
    }
    for (SliceModel* slice : model->slices()) {
        if (slice && slice->isTxSlice()) {
            return tciTrxForSlice(model, slice);
        }
    }
    return -1;
}

int TciProtocol::txTrx() const
{
    const int trx = m_trxMap ? m_trxMap->txSliceTrxOrNone(m_model)
                             : txSliceTrxOrNone(m_model);
    return trx < 0 ? 0 : trx;  // request/response wire needs a concrete index
}

// DDS = the IQ-stream center frequency a skimmer (CW Skimmer / SDC) decodes
// against. On FlexRadio the DAX IQ stream is a *panadapter* stream centered on
// the pan, not the slice (FlexLib: DAXIQChannel is a Panadapter property), so
// the center is the panadapter center, not the slice/VFO frequency. Reporting
// the slice freq mis-maps every detected signal by (slice − panCenter) Hz.
// Falls back to the slice frequency if the pan can't be resolved or has not
// received a normalized center update yet. (#3910, #3913 review)
long long TciProtocol::ddsCenterHz(RadioModel* model, const SliceModel* slice)
{
    if (!slice) {
        return 0;
    }
    if (model) {
        PanadapterModel* pan = model->panadapter(slice->panId());
        if (pan && pan->centerKnown()) {
            return mhzToHz(pan->centerMhz());
        }
    }
    return mhzToHz(slice->frequency());
}

// ── Init burst ─────────────────────────────────────────────────────────────

QString TciProtocol::generateInitBurst()
{
    QString burst;

    // ── Phase 1: Initialization commands (spec section 4.1) ───────────
    // Sent in spec order.  READY is NOT sent here: real ExpertSDR3
    // transmits its complete initial state (including the audio/IQ
    // stream parameters) and only then sends READY, and clients written
    // against it (SDC / CW Skimmer, reported by UT4LW) latch their
    // cached settings when READY arrives — an early READY makes them
    // initialize from defaults (notably a wrong iq_samplerate).  The
    // earlier early-READY rationale from #2597 ("strict clients reject
    // non-init commands before READY") does not match the reference
    // parser it cited: eesdr-tci aborts only on *unrecognized* command
    // names, anywhere in the stream, and imposes no pre-READY grammar;
    // the RF2K-S engagement fix was split_enable, not READY placement.
    burst += QStringLiteral("vfo_limits:1000,75000000;");
    burst += QStringLiteral("if_limits:-48000,48000;");

    const auto slices = m_model ? m_model->slices() : QList<SliceModel*>{};
    // #4567: with stable bindings, a transient hole (band-change settle
    // window) must not advertise a count below an index a client is using —
    // the map reports 1 + the highest trx in use instead of the list size.
    int trxCount = m_trxMap ? m_trxMap->trxCount(m_model) : slices.size();
    if (trxCount < 1) trxCount = 1;
    burst += QStringLiteral("trx_count:%1;").arg(trxCount);
    // `channels_count` (plural).  The TCI Protocol PDF spec lists this
    // as `CHANNEL_COUNT` (singular), but the reference implementation
    // (ars-ka0s/eesdr-tci on PyPI, used as the basis for many TCI
    // clients including the RF2K-S amplifier firmware) only recognises
    // the plural form — a singular-form command raises ValueError on
    // the client and aborts handshake parsing.  Implementation wins
    // over the PDF.
    burst += QStringLiteral("channels_count:2;");

    // Identity chosen to bypass WSJT-X's TCI gain-reduction code path.
    // WSJT-X's TCITransceiver halves TX sample amplitude (K2 = 0.499/0x7FFF
    // vs K1 = 0.999/0x7FFF, ~-6 dB) when BOTH conditions hold:
    //   device  == "SunSDR2DX" or "SunSDR2PRO"
    //   protocol != starts-with "ExpertSDR3"
    // Either branch alone keeps the K1 (full-amplitude) path; both branches
    // satisfied makes it doubly safe.
    //   - device: literal "AetherSDR" avoids the SunSDR-specific gain trap
    //     AND avoids the leading-space bug in the older "<name> <model>"
    //     form when the radio's nickname is empty.
    //   - protocol: "ExpertSDR3,1.5" sets WSJT-X's ESDR3 flag, which both
    //     gates the gain reduction off and selects WSJT-X command formats
    //     that AetherSDR already handles correctly (proven in v26.5.1).
    // RF2K-S amp whitelist (which keyed on SunSDR2DX + ExpertSDR2) is
    // regressed by this change; a configurable / adaptive identity is
    // tracked in #2806.  Everything else from #2597 (init-burst order,
    // vfo_limits, if_limits, channels_count, split_enable) is preserved
    // and is what the RF2K-S TCI parser actually needs to engage.
    burst += QStringLiteral("device:AetherSDR;");
    burst += QStringLiteral("receive_only:false;");
    burst += QStringLiteral("modulations_list:usb,lsb,cw,cwr,am,sam,fm,nfm,digu,digl,rtty;");
    burst += QStringLiteral("protocol:ExpertSDR3,1.5;");

    // ── Phase 2: Current state notifications ──────────────────────────
    // Identical in syntax to the events fired by a live tuning
    // operation; clients cache them whether they arrive before or after
    // READY (verified in WSJT-X TCITransceiver.cpp, which parses every
    // command on arrival and uses READY only to advance its own init
    // state machine).
    if (m_model) {
        for (auto* s : slices) {
            int trx = m_trxMap ? m_trxMap->trxForSlice(m_model, s)
                               : tciTrxForSlice(m_model, s);
            const long long hz = mhzToHz(s->frequency());
            burst += QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(hz);
            SliceModel* txVfo = sliceForVfo(trx, 1);
            const long long txHz = mhzToHz(txVfo ? txVfo->frequency() : s->frequency());
            burst += QStringLiteral("vfo:%1,1,%2;").arg(trx).arg(txHz);
            // dds: = IQ-stream center (panadapter center). Skimmers learn the
            // IQ center only from this; without it every spot is mis-mapped by
            // (slice − panCenter) Hz. (#3910)
            burst += QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsCenterHz(m_model, s));
            burst += QStringLiteral("modulation:%1,%2;")
                         .arg(trx).arg(smartsdrToTci(s->mode()));
            burst += QStringLiteral("rx_enable:%1,true;").arg(trx);

            // Filter
            burst += QStringLiteral("rx_filter_band:%1,%2,%3;")
                         .arg(trx).arg(s->filterLow()).arg(s->filterHigh());

            // RIT/XIT
            burst += QStringLiteral("rit_enable:%1,%2;")
                         .arg(trx).arg(s->ritOn() ? "true" : "false");
            burst += QStringLiteral("xit_enable:%1,%2;")
                         .arg(trx).arg(s->xitOn() ? "true" : "false");
            burst += QStringLiteral("rit_offset:%1,%2;")
                         .arg(trx).arg(static_cast<int>(s->ritFreq()));
            burst += QStringLiteral("xit_offset:%1,%2;")
                         .arg(trx).arg(static_cast<int>(s->xitFreq()));

            // Split — explicitly false so single-VFO operation is signalled.
            // The RF2K-S TCI client uses `split_enable:0,false;` as the
            // signal that VFO 0 is the active VFO; without ever receiving
            // it, its `currentPosition` stays None and get_frequency()
            // returns None, causing the amp to fall back to UNIV and show
            // "No TCI available" regardless of how many `vfo:` events it
            // receives.  (Source: rf-kit-gui_v198/operational_interface/
            // tciSupport.py, SplitThreadSafeValue.get() + extract_current_vfo.)
            burst += QStringLiteral("split_enable:%1,%2;")
                         .arg(trx)
                         .arg(m_routingState && m_routingState->splitRequested()
                                 ? QStringLiteral("true")
                                 : QStringLiteral("false"));

            // Lock
            burst += QStringLiteral("lock:%1,%2;")
                         .arg(trx).arg(s->isLocked() ? "true" : "false");

            // SQL
            burst += QStringLiteral("sql_enable:%1,%2;")
                         .arg(trx)
                         .arg(s->receiveSquelchOn() ? "true" : "false");
            burst += QStringLiteral("sql_level:%1,%2;")
                         .arg(trx).arg(s->receiveSquelchLevel());

            // AGC
            burst += QStringLiteral("agc_mode:%1,%2;")
                         .arg(trx).arg(s->receiveAgcMode().toLower());

            // DSP
            burst += QStringLiteral("rx_nb_enable:%1,%2;")
                         .arg(trx).arg(s->nbOn() ? "true" : "false");
            burst += QStringLiteral("rx_nr_enable:%1,%2;")
                         .arg(trx).arg(s->nrOn() ? "true" : "false");
            burst += QStringLiteral("rx_anf_enable:%1,%2;")
                         .arg(trx).arg(s->anfOn() ? "true" : "false");
            burst += QStringLiteral("rx_apf_enable:%1,%2;")
                         .arg(trx).arg(s->apfOn() ? "true" : "false");

            // Mute. Seeded here for the same reason sql/DSP are: without it a
            // connecting client's mute mirror starts at a default guess, and
            // mute was the one flag in this family the burst never carried
            // (#4161). audioMute() is the effective value -- it follows the
            // external-receive source when one is replacing Flex RX audio,
            // which is also what audioMuteChanged carries.
            burst += QStringLiteral("mute:%1,%2;")
                         .arg(trx).arg(s->audioMute() ? "true" : "false");

            // TX
            burst += QStringLiteral("tx_enable:%1,%2;")
                         .arg(trx).arg(s->isTxSlice() ? "true" : "false");
        }

        // Global TX state
        auto& tx = m_model->transmitModel();
        bool isTx = tx.isTransmitting();
        const int txTrxIndex = txTrx();
        // ESDR3 format requires TRX index prefix for drive commands.
        // Without it, WSJT-X/JTDX crash parsing args.at(1) on a 1-element list.
        burst += QStringLiteral("drive:%1,%2;").arg(txTrxIndex).arg(tx.rfPower());
        burst += QStringLiteral("tune_drive:%1,%2;").arg(txTrxIndex).arg(tx.tunePower());
        burst += QStringLiteral("mic_level:%1;").arg(tx.micLevel());
        burst += QStringLiteral("trx:%1,%2;").arg(txTrxIndex).arg(isTx ? "true" : "false");

        // Master AF volume — whole-radio (no trx prefix), same saved value
        // cmdVolume's GET returns, reported in dB per the TCI spec
        // (-60..0; ExpertSDR3 wire scale). Without it the init burst seeds
        // drive/mic_level but not AF, so a client's local mirror starts at
        // a default guess and the first AF-gain step jumps to that guess
        // instead of the radio's real level (Ulanzi/Elgato/StreamController
        // gain steppers).
        burst += QStringLiteral("volume:%1;")
                     .arg(volumeDbFromPercent(
                         AppSettings::instance().value("MasterVolume", "100").toInt()));

        // Which slice holds GUI focus (#4160) — AetherSDR extension. Without
        // it a control surface learns focus only from the next change event,
        // so every dial it owns targets trx 0 until the operator happens to
        // switch slices. Emitted pre-READY with the rest of the state dump.
        int activeTrx = -1;
        QString activeLetter;
        if (resolveActiveSlice(activeTrx, activeLetter)) {
            burst += QStringLiteral("active_slice:%1,%2;")
                         .arg(activeTrx).arg(activeLetter);
        }
    }

    // ── Phase 3: Audio / IQ stream configuration ──────────────────────
    // Last of the settings: clients-of-audio concerns (WSJT-X, CW
    // Skimmer / SDC), irrelevant to control-only clients like
    // amplifiers — which simply cache or ignore them pre-READY.
    burst += QStringLiteral("audio_samplerate:48000;");
    burst += QStringLiteral("audio_stream_sample_type:float32;");
    burst += QStringLiteral("audio_stream_channels:2;");
    burst += QStringLiteral("audio_stream_samples:2048;");
    burst += QStringLiteral("tx_stream_audio_buffering:50;");
    burst += QStringLiteral("iq_samplerate:48000;");

    // READY terminates the settings dump — it must follow EVERY setting
    // (in particular iq_samplerate), because SDC / CW Skimmer reads its
    // cached settings the moment READY arrives.  Matches real
    // ExpertSDR3 behavior and the spec's READY definition ("sent after
    // the initialization commands").  Reported by Yuri UT4LW.
    burst += QStringLiteral("ready;");

    // START is a bidirectional device-state notification. Stream lifecycle
    // commands are client-owned: AUDIO_START and IQ_START require a receiver
    // argument and are sent by the client after READY. Emitting the old
    // argument-less audio_start primer here wedged SDC before it processed
    // START, so its TCI connection never became active and it never requested
    // the IQ stream used by the CW skimmer (#3913 test-build finding).
    burst += QStringLiteral("start;");

    return burst;
}

// ── Command dispatch ───────────────────────────────────────────────────────

QString TciProtocol::handleCommand(const QString& cmd)
{
    m_pendingNotification.clear();
    m_pendingMasterVolume = -1;
    m_pendingTxGain = -1;
    m_vfoRequest.reset();
    m_splitRequest.reset();
    m_trxRequest.reset();

    if (cmd.isEmpty()) return {};

    // Parse: COMMAND_NAME:arg1,arg2,...
    // or just: COMMAND_NAME
    int colonIdx = cmd.indexOf(':');
    QString name = (colonIdx >= 0) ? cmd.left(colonIdx).toLower().trimmed()
                                   : cmd.toLower().trimmed();
    QStringList args;
    if (colonIdx >= 0) {
        QString argStr = cmd.mid(colonIdx + 1).trimmed();
        args = argStr.split(',', Qt::KeepEmptyParts);
        for (auto& a : args) a = a.trimmed();
    }

    // Determine if this is a set or get. TCI convention is: 0-1 args = GET
    // (no args, or trx index only), 2+ args = SET (trx index + value(s)).
    // Computed once up front so all dispatched commands see the correct
    // value — the previous implementation only set this AFTER the first
    // dispatch block, leaving early-dispatched commands (rx_volume,
    // cw_keyer_speed, mon_volume, rx_mute, rx_balance, …) with isSet=false
    // forever and silently treating SETs as GETs. See issue #1764.
    bool isSet = (args.size() >= 2);

    // Commands with special handling
    if (name == "start")            return cmdStart();
    if (name == "stop")             return cmdStop();
    if (name == "tx_enable")        return cmdTxEnable(args);
    if (name == "cw_msg")           return cmdCwMsg(args);
    if (name == "cw_macros")        return cmdCwMacros(args);
    if (name == "cw_macros_stop")   return cmdCwMacrosStop();
    if (name == "spot")             return cmdSpot(args);
    if (name == "spot_delete")      return cmdSpotDelete(args);
    if (name == "spot_clear")       return cmdSpotClear();
    if (name == "iq_start")         return cmdIqStart(args);
    if (name == "iq_stop")          return cmdIqStop(args);
    if (name == "iq_samplerate")    return cmdIqSampleRate(args, args.size() >= 1 && !args[0].isEmpty());
    if (name == "keyer")            return cmdKeyer(args);
    if (name == "cw_keyer_speed")   return cmdCwKeyerSpeed(args, isSet);
    if (name == "cw_macros_delay")  return cmdCwMacrosDelay(args, isSet);
    if (name == "cw_terminal")      return cmdCwTerminal(args, isSet);
    if (name == "dds")              return cmdDds(args, isSet);
    if (name == "if")               return cmdIf(args, isSet);
    if (name == "rx_channel_enable") return cmdRxChannelEnable(args, isSet);
    if (name == "rx_volume")        return cmdRxVolume(args, isSet);
    if (name == "rx_mute")          return cmdRxMute(args, isSet);
    if (name == "rx_balance")       return cmdRxBalance(args, isSet);
    if (name == "mon_enable")       return cmdMonEnable(args, isSet);
    if (name == "mon_volume")       return cmdMonVolume(args, isSet);
    if (name == "rx_nb_param")      return cmdRxNbParam(args, isSet);
    if (name == "rx_bin_enable")    return cmdRxBinEnable(args, isSet);
    if (name == "rx_anc_enable")    return cmdRxAncEnable(args, isSet);
    if (name == "rx_dse_enable")    return cmdRxDseEnable(args, isSet);
    if (name == "rx_nf_enable")     return cmdRxNfEnable(args, isSet);
    if (name == "digl_offset")      return cmdDiglOffset(args, isSet);
    if (name == "digu_offset")      return cmdDiguOffset(args, isSet);
    if (name == "set_in_focus")     return cmdSetInFocus();
    if (name == "tx_frequency")     return cmdTxFrequency();
    if (name == "vfo_limits")       return QStringLiteral("vfo_limits:30000,54000000;");
    if (name == "if_limits")        return QStringLiteral("if_limits:-10000,10000;");
    if (name == "vfo_lock")         return cmdLock(args, isSet);  // alias

    // Bidirectional commands — isSet was already computed at the top of
    // this function, so the dispatch is uniform from here on.

    if (name == "vfo")              return cmdVfo(args, args.size() >= 3);
    if (name == "modulation")       return cmdModulation(args, isSet);
    if (name == "trx")              return cmdTrx(args, isSet);
    if (name == "tune")             return cmdTune(args, isSet);
    if (name == "drive")            return cmdDrive(args, isSet);
    if (name == "tune_drive")       return cmdTuneDrive(args, isSet);
    if (name == "mic_level")        return cmdMicLevel(args, isSet);
    if (name == "rit_enable")       return cmdRitEnable(args, isSet);
    if (name == "xit_enable")       return cmdXitEnable(args, isSet);
    if (name == "rit_offset")       return cmdRitOffset(args, isSet);
    if (name == "xit_offset")       return cmdXitOffset(args, isSet);
    if (name == "split_enable")     return cmdSplitEnable(args, isSet);
    if (name == "rx_filter_band")   return cmdRxFilterBand(args, isSet);
    if (name == "cw_macros_speed")  return cmdCwMacrosSpeed(args, isSet);
    if (name == "lock")             return cmdLock(args, isSet);
    if (name == "sql_enable")       return cmdSqlEnable(args, isSet);
    if (name == "sql_level")        return cmdSqlLevel(args, isSet);
    if (name == "volume")           return cmdVolume(args, isSet);
    if (name == "mute")             return cmdMute(args, isSet);
    if (name == "agc_mode")         return cmdAgcMode(args, isSet);
    if (name == "agc_gain")         return cmdAgcGain(args, isSet);
    if (name == "rx_nb_enable")     return cmdRxNbEnable(args, isSet);
    if (name == "rx_nr_enable")     return cmdRxNrEnable(args, isSet);
    if (name == "rx_anf_enable")    return cmdRxAnfEnable(args, isSet);
    if (name == "rx_apf_enable")    return cmdRxApfEnable(args, isSet);

    // AetherSDR extensions (not in TCI v2.0 spec)
    if (name == "rx_record")        return cmdRxRecord(args, isSet);
    if (name == "rx_play")          return cmdRxPlay(args, isSet);
    if (name == "tx_gain")          return cmdTxGain(args, isSet);
    if (name == "active_slice")     return cmdActiveSlice(args);

    // Unknown command — ignore silently per TCI spec
    return {};
}

QString TciProtocol::pendingNotification()
{
    QString n = m_pendingNotification;
    m_pendingNotification.clear();
    return n;
}

std::optional<TciProtocol::VfoRequest> TciProtocol::takeVfoRequest()
{
    std::optional<VfoRequest> request = std::move(m_vfoRequest);
    m_vfoRequest.reset();
    return request;
}

std::optional<TciProtocol::SplitRequest> TciProtocol::takeSplitRequest()
{
    std::optional<SplitRequest> request = std::move(m_splitRequest);
    m_splitRequest.reset();
    return request;
}

std::optional<TciProtocol::TrxRequest> TciProtocol::takeTrxRequest()
{
    std::optional<TrxRequest> request = std::move(m_trxRequest);
    m_trxRequest.reset();
    return request;
}

// ── Command implementations ────────────────────────────────────────────────

QString TciProtocol::cmdStart()
{
    m_started = true;
    return {};
}

QString TciProtocol::cmdStop()
{
    m_started = false;
    return {};
}

// ── VFO: get/set frequency ─────────────────────────────────────────────────

QString TciProtocol::cmdVfo(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx) || trx < 0)
        return {};

    int channel = 0;
    if (args.size() >= 2 && !argToInt(args, 1, channel))
        return {};
    if (channel < 0 || channel > 1)
        return {};
    auto* s = sliceForVfo(trx, channel);

    if (!isSet) {
        if (!s) return {};
        // Get: vfo:trx,channel,freq_hz;
        long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
        return QStringLiteral("vfo:%1,%2,%3;").arg(trx).arg(channel).arg(hz);
    }

    // Set: vfo:trx,channel,freq_hz
    if (args.size() < 3) return {};
    long long hz = 0;
    if (!argToLongLong(args, 2, hz) || hz < 0) return {};
    m_vfoRequest = VfoRequest { trx, channel, hz };
    return {};
}

// ── Modulation: get/set mode ───────────────────────────────────────────────

QString TciProtocol::cmdModulation(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("modulation:%1,%2;")
                   .arg(trx).arg(smartsdrToTci(s->mode()));
    }

    if (args.size() < 2) return {};
    bool modOk = false;
    QString sdrMode = tciToSmartSDR(args[1], &modOk);
    if (!modOk) {
        // Unrecognised modulation name. The server already advertises the
        // accepted set in modulations_list at connect time, so anything
        // outside it is a client error, not something to guess a mode for.
        // Previously this silently fell through to USB and echoed the
        // client's own (wrong) name in one notification while a second
        // notification carried the real "usb" the slice actually became —
        // two conflicting lines a client can't reconcile (#4523). Dropped
        // instead, matching the "ignore silently" posture the parser
        // already takes for unrecognised commands.
        return {};
    }
    QMetaObject::invokeMethod(s, [s, sdrMode]() {
        s->setMode(sdrMode);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("modulation:%1,%2;")
                                .arg(trx).arg(args[1].toLower());
    return {};
}

// ── TRX: get/set TX state ──────────────────────────────────────────────────

QString TciProtocol::cmdTrx(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx) || trx < 0)
        return {};

    if (!isSet) {
        const bool tx = m_model && m_model->isRadioTransmitting();
        return QStringLiteral("trx:%1,%2;").arg(trx).arg(tx ? "true" : "false");
    }

    if (args.size() < 2) return {};
    const QString state = args[1].trimmed().toLower();
    if (state != QStringLiteral("true") && state != QStringLiteral("false")) {
        return {};
    }
    const QString source = args.size() >= 3 ? args[2].trimmed().toLower() : QString();
    m_trxRequest = TrxRequest { trx, state == QStringLiteral("true"), source };
    return {};
}

// ── TX_ENABLE: output-only TX assignment state ─────────────────────────────

QString TciProtocol::cmdTxEnable(const QStringList& args)
{
    Q_UNUSED(args);
    // TCI 2.0 and Thetis define TX_ENABLE as server-to-client state only.
    return {};
}

// ── TUNE: get/set tune state ───────────────────────────────────────────────

QString TciProtocol::cmdTune(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};

    if (!isSet) {
        bool tuning = m_model && m_model->transmitModel().isTuning();
        return QStringLiteral("tune:%1,%2;").arg(trx).arg(tuning ? "true" : "false");
    }

    if (args.size() < 2) return {};
    // Deliberately NOT argToBool, and deliberately not the drop posture the
    // rest of this file uses: this verb keys the transmitter. Dropping an
    // unparseable `tune:0,<junk>` would leave a tuning carrier on the air,
    // so "anything that is not the word true" has to keep meaning STOP.
    // Fail closed, not silent — Constitution VI. (#4867 review)
    const bool tune = (args[1].trimmed().toLower() == QLatin1String("true"));
    QMetaObject::invokeMethod(m_model, [model = m_model, tune]() {
        if (tune)
            model->transmitModel().startTune(TransmitModel::PttSource::Dax);
        else
            model->transmitModel().stopTune();
    }, Qt::QueuedConnection);

    return {};
}

// ── DRIVE: get/set RF power ────────────────────────────────────────────────

// TCI v2.0 defines DRIVE and TUNE_DRIVE as per-transceiver commands:
// one argument reads that TRX; two arguments set TRX,power. Every outbound
// message must retain both fields because ESDR3-mode WSJT-X/JTDX read arg[1]
// after matching arg[0], and a one-field `drive:0;` can crash receiver 0.
// A no-argument read remains as an AetherSDR compatibility extension and
// reports the current TX TRX.
QString TciProtocol::cmdDrive(const QStringList& args, bool /*isSet*/)
{
    int trx = txTrx();
    if (args.size() <= 1) {
        if (!args.isEmpty()) {
            if (!argToInt(args, 0, trx) || trx < 0) return {};
        }
        const int pwr = m_model ? m_model->transmitModel().rfPower() : 0;
        return QStringLiteral("drive:%1,%2;").arg(trx).arg(pwr);
    }
    if (args.size() != 2) return {};

    int pwr = 0;
    if (!argToInt(args, 0, trx) || trx < 0
        || !argToInt(args, 1, pwr) || pwr < 0 || pwr > 100) return {};
    if (m_model) {
        QMetaObject::invokeMethod(m_model, [model = m_model, pwr]() {
            model->transmitModel().setRfPower(pwr);
        }, Qt::QueuedConnection);
    }

    m_pendingNotification = QStringLiteral("drive:%1,%2;").arg(trx).arg(pwr);
    return {};
}

// ── TUNE_DRIVE: get/set tune power ─────────────────────────────────────────

QString TciProtocol::cmdTuneDrive(const QStringList& args, bool /*isSet*/)
{
    int trx = txTrx();
    if (args.size() <= 1) {
        if (!args.isEmpty()) {
            if (!argToInt(args, 0, trx) || trx < 0) return {};
        }
        const int pwr = m_model ? m_model->transmitModel().tunePower() : 0;
        return QStringLiteral("tune_drive:%1,%2;").arg(trx).arg(pwr);
    }
    if (args.size() != 2) return {};

    int pwr = 0;
    if (!argToInt(args, 0, trx) || trx < 0
        || !argToInt(args, 1, pwr) || pwr < 0 || pwr > 100) return {};
    if (m_model) {
        QMetaObject::invokeMethod(m_model, [model = m_model, pwr]() {
            model->transmitModel().setTunePower(pwr);
        }, Qt::QueuedConnection);
    }

    m_pendingNotification = QStringLiteral("tune_drive:%1,%2;").arg(trx).arg(pwr);
    return {};
}

// ── MIC_LEVEL: get/set mic input gain ─────────────────────────────────────

// mic_level bridges TransmitModel::setMicLevel() (the existing radio-side
// setter wired into FlexLib via commandReady) to TCI clients.  Single arg
// 0-100 (percent), global — mic is whole-radio, not per-trx. Accepts the
// legacy TRX-prefixed form
// `mic_level:0,N;` for forward compatibility with strict-spec clients (the
// trx index is ignored since mic is global).
//
// Added 2026-05-27 to unblock the aethersdr-ulanzi-plugin Mic Gain ▲▼ keys,
// which already send the verb but currently land on a missing dispatcher
// entry → silently ignored.
QString TciProtocol::cmdMicLevel(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int lvl = m_model ? m_model->transmitModel().micLevel() : 0;
        return QStringLiteral("mic_level:%1;").arg(lvl);
    }
    if (!m_model) return {};
    int lvl = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, lvl)) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, lvl]() {
        model->transmitModel().setMicLevel(lvl);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("mic_level:%1;").arg(lvl);
    return {};
}

// ── TX_GAIN: get/set TCI TX audio gain (AetherSDR extension) ────────────────

// tx_gain is an AetherSDR extension — not in the TCI v2.0 spec. It controls
// TciServer's outbound TX gain (m_txGain), applied to WSJT-X/JTDX audio before
// the radio. Global command, 0-100 (percent of unity), matching the
// drive/tune_drive convention. Like cmdVolume, TciProtocol cannot reach
// TciServer directly, so a SET stashes the value in m_pendingTxGain and
// TciServer applies it via setTxGain() after handleCommand() returns.
QString TciProtocol::cmdTxGain(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        // GET — current TCI TX gain (0-100). TciServer persists the 0.0-1.0
        // value to TciTxGain whenever it changes.
        float g = AppSettings::instance().value("TciTxGain", "1.00").toFloat();
        return QStringLiteral("tx_gain:%1;").arg(qBound(0, qRound(g * 100.0f), 100));
    }
    // Same malformed-input shape as cmdVolume above, and the third instance
    // of it in this file after #4345's DRIVE read: "tx_gain:" splits to a
    // single empty string rather than an empty arg list, so it reaches the
    // SET branch, and an unchecked toInt() turns that into 0 — silently
    // muting the WSJT-X/JTDX TX audio path and broadcasting a well-formed
    // "tx_gain:0;" that a second client cannot tell from a real change.
    // Listed as item 2 of #4523's triage plan alongside cmdVolume; dropped
    // rather than guessed, matching the "ignore silently" posture the parser
    // already takes for unrecognised commands.
    int raw = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, raw)) {
        return {};
    }
    const int pct = qBound(0, raw, 100);
    m_pendingTxGain = pct;
    m_pendingNotification = QStringLiteral("tx_gain:%1;").arg(pct);
    return {};
}

// ── RIT ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdRitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rit_enable:%1,%2;")
                   .arg(trx).arg(s->ritOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    int hz = s->ritFreq();
    QMetaObject::invokeMethod(s, [s, on, hz]() { s->setRit(on, hz); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rit_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRitOffset(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rit_offset:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->ritFreq()));
    }

    if (args.size() < 2) return {};
    int offset = 0;
    if (!argToInt(args, 1, offset)) return {};
    bool on = s->ritOn();
    QMetaObject::invokeMethod(s, [s, on, offset]() {
        s->setRit(on, offset);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rit_offset:%1,%2;")
                                .arg(trx).arg(offset);
    return {};
}

// ── XIT ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdXitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("xit_enable:%1,%2;")
                   .arg(trx).arg(s->xitOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    int hz = s->xitFreq();
    QMetaObject::invokeMethod(s, [s, on, hz]() { s->setXit(on, hz); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("xit_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdXitOffset(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("xit_offset:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->xitFreq()));
    }

    if (args.size() < 2) return {};
    int offset = 0;
    if (!argToInt(args, 1, offset)) return {};
    bool on = s->xitOn();
    QMetaObject::invokeMethod(s, [s, on, offset]() {
        s->setXit(on, offset);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("xit_offset:%1,%2;")
                                .arg(trx).arg(offset);
    return {};
}

// ── SPLIT ──────────────────────────────────────────────────────────────────

QString TciProtocol::cmdSplitEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx) || trx < 0)
        return {};
    auto* s = sliceForTrx(trx);
    if (!isSet) {
        bool split = m_routingState && m_routingState->splitRequested();

        if (!m_routingState && s && m_model) {
            for (auto* sl : m_model->slices()) {
                if (sl->isTxSlice() && sl != s) {
                    split = true;
                    break;
                }
            }
        }
        return QStringLiteral("split_enable:%1,%2;").arg(trx).arg(split ? "true" : "false");
    }

    if (args.size() < 2)
        return {};
    const QString state = args[1].trimmed().toLower();
    if (state != QStringLiteral("true") && state != QStringLiteral("false")) {
        return {};
    }
    m_splitRequest = SplitRequest { trx, state == QStringLiteral("true") };
    return {};
}

// ── RX Filter ──────────────────────────────────────────────────────────────

QString TciProtocol::cmdRxFilterBand(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_filter_band:%1,%2,%3;")
                   .arg(trx).arg(s->filterLow()).arg(s->filterHigh());
    }

    if (args.size() < 3) return {};
    int lo = 0;
    int hi = 0;
    if (!argToInt(args, 1, lo) || !argToInt(args, 2, hi)) return {};
    QMetaObject::invokeMethod(s, [s, lo, hi]() {
        s->setFilterWidth(lo, hi);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_filter_band:%1,%2,%3;")
                                .arg(trx).arg(lo).arg(hi);
    return {};
}

// ── CW ─────────────────────────────────────────────────────────────────────

// GLOBAL commands (no trx) re-derive GET/SET from the argument list rather
// than trusting the dispatcher's `isSet`, and read the value from the last
// argument. handleCommand() computes `isSet = (args.size() >= 2)` from the
// trx-prefixed shape every per-slice verb uses, which is wrong for a global
// one: it makes the spec form `cw_macros_speed:25;` a READ that silently
// discards the value, and leaves the 2-arg form reading the trx position as
// the value (`cw_macros_speed:0,25;` set the speed to 0). cmdVolume,
// cmdMicLevel and cmdTxGain already do it this way — these four verbs were
// simply missed. Found while adding #4867's tests; no bundled plugin sends
// any of them, so nothing shipped depended on the broken shape.
QString TciProtocol::cmdCwMacrosSpeed(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int wpm = m_model ? m_model->transmitModel().cwSpeed() : 25;
        return QStringLiteral("cw_macros_speed:%1;").arg(wpm);
    }

    if (!m_model) return {};
    int wpm = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, wpm)) return {};
    if (wpm < 5 || wpm > 100) return {};
    QString cmd = QStringLiteral("cw wpm %1").arg(wpm);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("cw_macros_speed:%1;").arg(wpm);
    return {};
}

QString TciProtocol::cmdCwMsg(const QStringList& args)
{
    if (args.isEmpty()) return {};
    // cw_msg:text — send CW macro text
    QString text = args.join(',');  // rejoin in case text had commas
    if (text.isEmpty()) return {};
    QString cmd = QStringLiteral("cwx send \"%1\"").arg(text);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        // Capability check runs HERE, on the model's thread, not in the caller:
        // this method is driven by the TCI client socket and RadioModel state is
        // not ours to read from it.
        if (!model->hasRadioSideCwKeyer()) {
            qCWarning(lcCat) << "TCI: cw_msg ignored \u2014 radio has no radio-side "
                                "CW keyer";
            return;
        }
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return {};
}

// ── Lock ───────────────────────────────────────────────────────────────────

QString TciProtocol::cmdLock(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("lock:%1,%2;")
                   .arg(trx).arg(s->isLocked() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setLocked(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("lock:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── Squelch ────────────────────────────────────────────────────────────────

QString TciProtocol::cmdSqlEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("sql_enable:%1,%2;")
                   .arg(trx)
                   .arg(s->receiveSquelchOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    int level = s->receiveSquelchLevel();
    QMetaObject::invokeMethod(s, [s, on, level]() { s->setSquelch(on, level); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("sql_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdSqlLevel(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("sql_level:%1,%2;")
                   .arg(trx).arg(s->receiveSquelchLevel());
    }

    if (args.size() < 2) return {};
    int level = 0;
    if (!argToInt(args, 1, level)) return {};
    bool on = s->receiveSquelchOn();
    QMetaObject::invokeMethod(s, [s, on, level]() { s->setSquelch(on, level); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("sql_level:%1,%2;")
                                .arg(trx).arg(level);
    return {};
}

// ── Volume / Mute ──────────────────────────────────────────────────────────

// VOLUME is a GLOBAL command per TCI v2.0 spec — it controls the master
// output level the operator hears, not a per-slice audio gain. This
// matches the title bar's master volume slider in the GUI. Per-receiver
// volume goes through the separate `rx_volume` command (cmdRxVolume).
//
// Spec form:  GET = `volume;`  (no args)
//             SET = `volume:N;` (1 arg, dB, -60..0; -60 = silence)
//
// Wire scale is dB per TCI Protocol v2.0 ("The range of values is from
// -60 to 0 dB, at a value of -60 dB there is no sound") — real
// ExpertSDR3 sends e.g. `VOLUME:-12;`.  Internally AetherSDR's master
// volume is a 0-100 percent amplitude (title bar slider /
// applyMasterVolume), so the dB<->percent conversion happens here at
// the wire and everything inboard stays percent.
//
// Legacy compat: values >= 1 cannot be dB (the spec range is
// non-positive), so they are accepted as the old AetherSDR percent
// scale — existing percent senders keep working.  `volume:0` is 0 dB =
// full volume per spec (mute belongs to the `mute:` command).
//
// We also accept the legacy TRX-prefixed form `volume:trx,N;` for
// backward compatibility — the trx is ignored since master volume is
// global. Old clients that previously sent this form to set per-slice
// gain were silently broken (the SET path mixed up vol and trx
// indices) — those clients should migrate to `rx_volume:trx,N;`.
//
// For SET, we don't directly call AudioEngine here — TciProtocol owns
// only the RadioModel, not AudioEngine. Instead we stash the requested
// level in m_pendingMasterVolume; TciServer reads it after handleCommand
// and forwards the request to MainWindow via signal, mirroring the path
// taken by the title bar's masterVolumeChanged signal.

int TciProtocol::volumeDbFromPercent(int pct)
{
    if (pct <= 0) return -60;
    if (pct > 100) pct = 100;
    const int db = static_cast<int>(std::lround(20.0 * std::log10(pct / 100.0)));
    return std::clamp(db, -60, 0);
}

int TciProtocol::volumePercentFromDb(double db)
{
    if (db <= -60.0) return 0;
    if (db > 0.0) db = 0.0;
    const int pct = static_cast<int>(std::lround(100.0 * std::pow(10.0, db / 20.0)));
    // Integer percent can't resolve below -40 dB; floor at 1 so only
    // -60 dB (the spec's explicit silence point) maps to mute.
    return std::clamp(pct, 1, 100);
}

QString TciProtocol::cmdVolume(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        // GET — current master volume from saved settings (the same value
        // the title bar slider reads on startup), reported in dB.
        int pct = AppSettings::instance()
                      .value("MasterVolume", "100").toInt();
        return QStringLiteral("volume:%1;").arg(volumeDbFromPercent(pct));
    }

    // SET — accept either spec form (1 arg) or legacy trx-prefixed (2+ args).
    // "volume:" (colon, nothing after it) splits to a single empty string,
    // not an empty arg list, so it reaches here rather than the GET branch
    // above — but neither spec form (VOLUME; / VOLUME:arg1;) is "colon with
    // nothing after it", so it's malformed input. It used to parse via
    // toDouble() (empty string → 0.0, silently) and apply that as 100% (0 dB
    // is the top of the range) — the malformed input landing on the loudest
    // value the command can express (#4523). Checked and dropped instead,
    // matching the "ignore silently" posture already used for unrecognised
    // commands; this also catches the same shape for the legacy 2-arg form
    // (e.g. "volume:0,").
    //
    // argToDouble rejects the empty string AND the non-finite literals
    // ("inf"/"nan", which Qt parses and reports ok for) — see its definition
    // for why the second half is not optional. The percent branch below is
    // deliberately NOT tightened: it is what three bundled plugins send.
    double val = 0.0;
    if (!argToDouble(args, args.size() == 1 ? 0 : 1, val)) {
        return {};
    }
    const int pct = (val >= 1.0)
        ? std::min(static_cast<int>(std::lround(val)), 100)  // legacy percent
        : volumePercentFromDb(val);                          // spec dB
    m_pendingMasterVolume = pct;

    // Echo in dB (round-tripped through the percent store so the echo
    // matches what a subsequent GET would return).
    m_pendingNotification =
        QStringLiteral("volume:%1;").arg(volumeDbFromPercent(pct));
    return {};
}

QString TciProtocol::cmdMute(const QStringList& args, bool isSet)
{
    if (!isSet) {
        int trx = 0;
        if (argToInt(args, 0, trx)) {
            auto* s = sliceForTrx(trx);
            if (s) return QStringLiteral("mute:%1,%2;")
                              .arg(trx).arg(s->audioMute() ? "true" : "false");
        }
        return {};
    }

    if (args.size() < 2) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    bool mute = false;
    if (!argToBool(args, 1, mute)) return {};
    auto* s = sliceForTrx(trx);
    if (s) {
        QMetaObject::invokeMethod(s, [s, mute]() { s->setAudioMute(mute); },
                                  Qt::QueuedConnection);
    }

    m_pendingNotification = QStringLiteral("mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false");
    return {};
}

// ── AGC ────────────────────────────────────────────────────────────────────

QString TciProtocol::cmdAgcMode(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("agc_mode:%1,%2;")
                   .arg(trx).arg(s->receiveAgcMode().toLower());
    }

    if (args.size() < 2) return {};
    QString mode = args[1].toLower();
    // Map TCI AGC names to FlexRadio: off, slow, med, fast
    QMetaObject::invokeMethod(s, [s, mode]() { s->setAgcMode(mode); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("agc_mode:%1,%2;")
                                .arg(trx).arg(mode);
    return {};
}

QString TciProtocol::cmdAgcGain(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("agc_gain:%1,%2;")
                   .arg(trx).arg(s->receiveAgcThreshold());
    }

    if (args.size() < 2) return {};
    int gain = 0;
    if (!argToInt(args, 1, gain)) return {};
    QMetaObject::invokeMethod(s, [s, gain]() { s->setAgcThreshold(gain); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("agc_gain:%1,%2;")
                                .arg(trx).arg(gain);
    return {};
}

// ── DSP toggles ────────────────────────────────────────────────────────────

QString TciProtocol::cmdRxNbEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nb_enable:%1,%2;")
                   .arg(trx).arg(s->nbOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setNb(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_nb_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxNrEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nr_enable:%1,%2;")
                   .arg(trx).arg(s->nrOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setNr(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_nr_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxAnfEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_anf_enable:%1,%2;")
                   .arg(trx).arg(s->anfOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setAnf(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_anf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxApfEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_apf_enable:%1,%2;")
                   .arg(trx).arg(s->apfOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setApf(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_apf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── AetherSDR extensions (DVK record/play) ─────────────────────────────────

QString TciProtocol::cmdRxRecord(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_record:%1,%2;")
                   .arg(trx).arg(s->recordOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setRecordOn(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_record:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxPlay(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_play:%1,%2;")
                   .arg(trx).arg(s->playOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    QMetaObject::invokeMethod(s, [s, on]() { s->setPlayOn(on); },
                              Qt::QueuedConnection);

    m_pendingNotification = QStringLiteral("rx_play:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false");
    return {};
}

// ── Spot injection ─────────────────────────────────────────────────────────

QString TciProtocol::cmdSpot(const QStringList& args)
{
    // spot:callsign,modulation,freq_hz,color,text
    if (!m_model || args.size() < 3) return {};

    QString callsign = args[0].trimmed();
    QString mode = args[1].trimmed();
    double freqHz = 0.0;
    if (!argToDouble(args, 2, freqHz) || callsign.isEmpty()) return {};

    QString color = (args.size() > 3) ? args[3].trimmed() : QString();
    QString comment = (args.size() > 4) ? args[4].trimmed() : QString();

    QMap<QString, QString> kvs;
    kvs["callsign"] = callsign;
    kvs["rx_freq"] = QString::number(freqHz / 1e6, 'f', 6);
    kvs["mode"] = mode;
    kvs["source"] = "TCI";
    if (!color.isEmpty()) kvs["color"] = color;
    if (!comment.isEmpty()) kvs["comment"] = comment;
    kvs["lifetime_seconds"] = "1800";

    QMetaObject::invokeMethod(m_model, [model = m_model, kvs]() {
        static int idx = 10000;
        model->spotModel().applySpotStatus(idx++, kvs);
    }, Qt::QueuedConnection);

    return {};
}

QString TciProtocol::cmdSpotDelete(const QStringList& args)
{
    if (!m_model || args.size() < 2) return {};
    QString callsign = args[0].trimmed();
    double freqHz = 0.0;
    if (!argToDouble(args, 1, freqHz)) return {};
    double freqMhz = freqHz / 1e6;

    QMetaObject::invokeMethod(m_model, [model = m_model, callsign, freqMhz]() {
        auto& sm = model->spotModel();
        for (auto it = sm.spots().begin(); it != sm.spots().end(); ++it) {
            if (it->callsign == callsign &&
                std::abs(it->rxFreqMhz - freqMhz) < 0.0001) {
                sm.removeSpot(it.key());
                break;
            }
        }
    }, Qt::QueuedConnection);

    return {};
}

QString TciProtocol::cmdSpotClear()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        model->spotModel().clear();
    }, Qt::QueuedConnection);
    return {};
}

// ── CW macros ──────────────────────────────────────────────────────────────

QString TciProtocol::cmdCwMacros(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    QString text = args.join(',');
    if (text.isEmpty()) return {};
    QString cmd = QStringLiteral("cwx send \"%1\"").arg(text);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        if (!model->hasRadioSideCwKeyer()) {
            qCWarning(lcCat) << "TCI: cw_macros ignored \u2014 radio has no "
                                "radio-side CW keyer";
            return;
        }
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdCwMacrosStop()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        if (!model->hasRadioSideCwKeyer()) return;
        model->sendCmdPublic("cwx clear", nullptr);
    }, Qt::QueuedConnection);
    return {};
}

// ── IQ streaming ───────────────────────────────────────────────────────────

QString TciProtocol::cmdIqStart(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    // Bound the index BEFORE the arithmetic: argToInt accepts INT_MAX (it
    // parses cleanly — it is "99999999999" that clears ok), and `trx + 1`
    // would then be signed overflow, i.e. UB reachable from any TCI client on
    // the one PR whose whole subject is boundary validation. Checking the
    // index rather than the result also subsumes the old channel range test.
    //
    // No observable behaviour changes: DaxIqModel::createStream() already
    // rejects an out-of-range channel, so this is strictly about keeping the
    // arithmetic defined for the weekly UBSan job. There is deliberately no
    // test asserting the rejection — there would be nothing to observe — only
    // a positive control that an in-range index still creates its stream.
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (trx < 0 || trx > 3) return {};       // DAX IQ channels are 1-4
    const int channel = trx + 1;  // TRX 0 → DAX IQ channel 1
    // Creating the dax_iq stream is not enough to make IQ flow: on FlexRadio
    // `daxiq_channel` is a Panadapter property, so the radio streams nothing
    // (or a stale pan's IQ) until a panadapter is routed to this channel with
    // `display pan set <panId> daxiq_channel=N`. The GUI does this from the
    // spectrum overlay menu, but a TCI skimmer client (SDC / CW Skimmer) has no
    // such hook — so bind the requested TRX's pan here. Mirrors the only other
    // non-GUI IQ consumer, WfmDemodulator, and centers the stream on the same
    // pan whose center is reported to the client via dds: (#3910/#3913).
    const SliceModel* s = sliceForTrx(trx);
    const QString panId = s ? s->panId() : QString();
    QMetaObject::invokeMethod(m_model, [model = m_model, channel, panId]() {
        model->daxIqModel().createStream(channel);
        if (!panId.isEmpty())
            model->sendCommand(QStringLiteral("display pan set %1 daxiq_channel=%2")
                                   .arg(panId).arg(channel));
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdIqStop(const QStringList& args)
{
    if (!m_model || args.isEmpty()) return {};
    // Bounded before the addition, same as cmdIqStart above.
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (trx < 0 || trx > 3) return {};       // DAX IQ channels are 1-4
    const int channel = trx + 1;
    QMetaObject::invokeMethod(m_model, [model = m_model, channel]() {
        model->daxIqModel().removeStream(channel);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdIqSampleRate(const QStringList& args, bool isSet)
{
    if (!isSet) {
        if (m_model) {
            int rate = m_model->daxIqModel().stream(1).sampleRate;
            return QStringLiteral("iq_samplerate:%1;").arg(rate);
        }
        return QStringLiteral("iq_samplerate:48000;");
    }

    if (args.isEmpty()) return {};
    // Deliberately NOT the drop-on-unparseable posture the rest of this file
    // uses: an unparseable rate joins the unsupported-rate branch below rather
    // than returning {}. #3913 made this command reply to a rejection on
    // purpose, because a skimmer (CW Skimmer / SDC) blocks waiting for the
    // confirmation and silence would hang it. Rejecting is still rejecting —
    // it just says so out loud, which is strictly better than dropping here.
    int rate = 0;
    const bool rateParsed = argToInt(args, 0, rate);
    if (!rateParsed
        || (rate != 24000 && rate != 48000 && rate != 96000 && rate != 192000)) {
        // Reject without hanging the requester: report the rate that remains
        // in force. No pending notification is set because other clients saw
        // no state change (#3913 review).
        const int currentRate = m_model
            ? m_model->daxIqModel().stream(1).sampleRate
            : 48000;
        return QStringLiteral("iq_samplerate:%1;").arg(currentRate);
    }
    if (m_model) {
        QMetaObject::invokeMethod(m_model, [model = m_model, rate]() {
            model->daxIqModel().setSampleRate(1, rate);
        }, Qt::QueuedConnection);
    }
    // Echo the achieved rate to BOTH the requester and other clients. The
    // server is authoritative for the rate (it clamps/rejects above), so a
    // skimmer (CW Skimmer / SDC) blocks waiting for this confirmation. The
    // dispatcher sends the return value to the requesting socket and the
    // pendingNotification to all *other* sockets, so each client gets exactly
    // one copy — without the return, the requester got no reply at all. (#3910)
    m_pendingNotification = QStringLiteral("iq_samplerate:%1;").arg(rate);
    return QStringLiteral("iq_samplerate:%1;").arg(rate);
}

// ── CW keyer (straight key via TCI) ────────────────────────────────────────

QString TciProtocol::cmdKeyer(const QStringList& args)
{
    // keyer:trx,state[,duration_ms]
    // state: true=key down, false=key up
    if (!m_model || args.size() < 2) return {};
    // Same fail-closed exemption as cmdTune: an unparseable state must mean
    // KEY UP, never "ignore and leave the key down" (Constitution VI).
    const bool down = (args[1].trimmed().toLower() == QLatin1String("true"));
    QMetaObject::invokeMethod(m_model, [model = m_model, down]() {
        model->sendCwKey(down);
    }, Qt::QueuedConnection);
    return {};
}

// Global command — see the note above cmdCwMacrosSpeed.
QString TciProtocol::cmdCwKeyerSpeed(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int wpm = m_model ? m_model->transmitModel().cwSpeed() : 20;
        return QStringLiteral("cw_keyer_speed:%1;").arg(wpm);
    }
    if (!m_model) return {};
    int wpm = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, wpm)) return {};
    if (wpm < 5 || wpm > 100) return {};
    QString cmd = QStringLiteral("cw wpm %1").arg(wpm);
    QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
        model->sendCmdPublic(cmd, nullptr);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("cw_keyer_speed:%1;").arg(wpm);
    return {};
}

// Global command — see the note above cmdCwMacrosSpeed.
QString TciProtocol::cmdCwMacrosDelay(const QStringList& args, bool /*isSet*/)
{
    // Delay before CW TX starts (ms). FlexRadio has cw_delay.
    static int delay = 0;
    if (args.isEmpty())
        return QStringLiteral("cw_macros_delay:%1;").arg(delay);
    if (!argToInt(args, args.size() == 1 ? 0 : 1, delay)) return {};
    if (m_model) {
        QString cmd = QStringLiteral("cw delay %1").arg(delay);
        QMetaObject::invokeMethod(m_model, [model = m_model, cmd]() {
            model->sendCmdPublic(cmd, nullptr);
        }, Qt::QueuedConnection);
    }
    m_pendingNotification = QStringLiteral("cw_macros_delay:%1;").arg(delay);
    return {};
}

// Global command — see the note above cmdMonEnable. Same boolean shape.
QString TciProtocol::cmdCwTerminal(const QStringList& args, bool /*isSet*/)
{
    static bool terminal = false;
    if (args.isEmpty())
        return QStringLiteral("cw_terminal:%1;").arg(terminal ? "true" : "false");
    // argToBool leaves `terminal` untouched when the argument is unparseable,
    // so a dropped SET keeps the last good value rather than resetting it.
    if (!argToBool(args, args.size() == 1 ? 0 : 1, terminal)) return {};
    return {};
}

// ── DDS / IF ───────────────────────────────────────────────────────────────

QString TciProtocol::cmdDds(const QStringList& args, bool isSet)
{
    // DDS = center frequency of the receiver (panadapter center, not the VFO).
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsCenterHz(m_model, s));
    }

    if (args.size() < 2) return {};
    // DDS set changes the panadapter center, not the VFO — acknowledge only
    return {};
}

QString TciProtocol::cmdIf(const QStringList& args, bool isSet)
{
    // IF offset — RIT equivalent in TCI
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("if:%1,0,%2;")
                   .arg(trx).arg(s->ritOn() ? s->ritFreq() : 0);
    }

    if (args.size() < 3) return {};
    int offset = 0;
    if (!argToInt(args, 2, offset)) return {};
    const bool on = (offset != 0);
    QMetaObject::invokeMethod(s, [s, on, offset]() { s->setRit(on, offset); },
                              Qt::QueuedConnection);
    return {};
}

// ── Per-receiver audio ─────────────────────────────────────────────────────

QString TciProtocol::cmdRxChannelEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (!isSet)
        return QStringLiteral("rx_channel_enable:%1,true;").arg(trx);
    // Always enabled — FlexRadio slices are always active
    return {};
}

QString TciProtocol::cmdRxVolume(const QStringList& args, bool isSet)
{
    // Per-receiver volume — maps to slice audioGain
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_volume:%1,%2;")
                   .arg(trx).arg(static_cast<int>(s->audioGain()));
    }

    if (args.size() < 2) return {};
    int gainPct = 0;
    if (!argToInt(args, 1, gainPct)) return {};
    const float gain = static_cast<float>(gainPct);
    QMetaObject::invokeMethod(s, [s, gain]() { s->setAudioGain(gain); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_volume:%1,%2;")
                                .arg(trx).arg(static_cast<int>(gain));
    return {};
}

QString TciProtocol::cmdRxMute(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_mute:%1,%2;")
                   .arg(trx).arg(s->audioMute() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool mute = false;
    if (!argToBool(args, 1, mute)) return {};
    QMetaObject::invokeMethod(s, [s, mute]() { s->setAudioMute(mute); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false");
    return {};
}

QString TciProtocol::cmdRxBalance(const QStringList& args, bool isSet)
{
    // RX audio balance — maps to slice audioPan (0=left, 50=center, 100=right)
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        // TCI balance: -40 to +40 (0=center)
        // FlexRadio: 0–100 (50=center)
        int tciBalance = s->audioPan() - 50;
        return QStringLiteral("rx_balance:%1,%2;").arg(trx).arg(tciBalance);
    }

    if (args.size() < 2) return {};
    int tciBalance = 0;  // -40 to +40
    if (!argToInt(args, 1, tciBalance)) return {};
    int flexPan = qBound(0, tciBalance + 50, 100);
    QMetaObject::invokeMethod(s, [s, flexPan]() { s->setAudioPan(flexPan); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_balance:%1,%2;")
                                .arg(trx).arg(tciBalance);
    return {};
}

// ── Monitor ────────────────────────────────────────────────────────────────

// Global command — see the note above cmdCwMacrosSpeed. This is the boolean
// half of the same defect: `mon_enable:true;` is one argument, so the
// trx-shaped `isSet` read it as a GET and the enable was silently discarded,
// while `mon_enable:0,true;` took the SET branch and read args[0] — the trx
// slot, "0" — as the state, so it DISABLED the monitor.
QString TciProtocol::cmdMonEnable(const QStringList& args, bool /*isSet*/)
{
    if (!m_model) return {};
    if (args.isEmpty()) {
        return QStringLiteral("mon_enable:%1;")
                   .arg(m_model->transmitModel().sbMonitor() ? "true" : "false");
    }
    bool on = false;
    if (!argToBool(args, args.size() == 1 ? 0 : 1, on)) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, on]() {
        model->transmitModel().setSbMonitor(on);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("mon_enable:%1;")
                                .arg(on ? "true" : "false");
    return {};
}

// Global command — see the note above cmdCwMacrosSpeed.
QString TciProtocol::cmdMonVolume(const QStringList& args, bool /*isSet*/)
{
    if (!m_model) return {};
    if (args.isEmpty()) {
        return QStringLiteral("mon_volume:%1;")
                   .arg(m_model->transmitModel().monGainSb());
    }
    int vol = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, vol)) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, vol]() {
        model->transmitModel().setMonGainSb(vol);
    }, Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("mon_volume:%1;").arg(vol);
    return {};
}

// ── DSP parameters ─────────────────────────────────────────────────────────

QString TciProtocol::cmdRxNbParam(const QStringList& args, bool isSet)
{
    // NB parameter — maps to slice nbLevel (0–100)
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        return QStringLiteral("rx_nb_param:%1,0,%2;")
                   .arg(trx).arg(s->nbLevel());
    }

    if (args.size() < 3) return {};
    int level = 0;
    if (!argToInt(args, 2, level)) return {};
    QMetaObject::invokeMethod(s, [s, level]() { s->setNbLevel(level); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("rx_nb_param:%1,0,%2;")
                                .arg(trx).arg(level);
    return {};
}

QString TciProtocol::cmdRxBinEnable(const QStringList& args, bool isSet)
{
    // Binaural — no direct FlexRadio equivalent, acknowledge only
    static bool binEnabled = false;
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (!isSet)
        return QStringLiteral("rx_bin_enable:%1,%2;")
                   .arg(trx).arg(binEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    if (!argToBool(args, 1, binEnabled)) return {};
    return {};
}

QString TciProtocol::cmdRxAncEnable(const QStringList& args, bool isSet)
{
    // Adaptive Noise Cancellation — no direct FlexRadio equivalent
    static bool ancEnabled = false;
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (!isSet)
        return QStringLiteral("rx_anc_enable:%1,%2;")
                   .arg(trx).arg(ancEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    if (!argToBool(args, 1, ancEnabled)) return {};
    return {};
}

QString TciProtocol::cmdRxDseEnable(const QStringList& args, bool isSet)
{
    // Digital Surround Effect — no direct FlexRadio equivalent
    static bool dseEnabled = false;
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (!isSet)
        return QStringLiteral("rx_dse_enable:%1,%2;")
                   .arg(trx).arg(dseEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    if (!argToBool(args, 1, dseEnabled)) return {};
    return {};
}

QString TciProtocol::cmdRxNfEnable(const QStringList& args, bool isSet)
{
    // Notch filter module — maps to TNF global enable
    // For now, acknowledge only (TNF is per-notch, not a global toggle in the
    // same way)
    static bool nfEnabled = true;
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    if (!isSet)
        return QStringLiteral("rx_nf_enable:%1,%2;")
                   .arg(trx).arg(nfEnabled ? "true" : "false");
    if (args.size() < 2) return {};
    if (!argToBool(args, 1, nfEnabled)) return {};
    return {};
}

// ── Digital mode offsets ───────────────────────────────────────────────────

// Global command — see the note above cmdCwMacrosSpeed. digl_offset takes no
// trx (it hardcodes sliceForTrx(0)), so the dispatcher's trx-shaped `isSet`
// was wrong in both directions here too: `digl_offset:500;` read as a GET and
// discarded the 500, and `digl_offset:0,500;` set the offset to the trx slot
// and broadcast "digl_offset:0;". Fifth and sixth instances of the same
// defect, found in the #4867 review.
QString TciProtocol::cmdDiglOffset(const QStringList& args, bool /*isSet*/)
{
    if (!m_model) return {};
    auto* s = sliceForTrx(0);
    if (!s) return {};

    if (args.isEmpty())
        return QStringLiteral("digl_offset:%1;").arg(s->diglOffset());

    int hz = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, hz)) return {};
    QMetaObject::invokeMethod(s, [s, hz]() { s->setDiglOffset(hz); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("digl_offset:%1;").arg(hz);
    return {};
}

// Global command — see the note above cmdDiglOffset.
QString TciProtocol::cmdDiguOffset(const QStringList& args, bool /*isSet*/)
{
    if (!m_model) return {};
    auto* s = sliceForTrx(0);
    if (!s) return {};

    if (args.isEmpty())
        return QStringLiteral("digu_offset:%1;").arg(s->diguOffset());

    int hz = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, hz)) return {};
    QMetaObject::invokeMethod(s, [s, hz]() { s->setDiguOffset(hz); },
                              Qt::QueuedConnection);
    m_pendingNotification = QStringLiteral("digu_offset:%1;").arg(hz);
    return {};
}

// ── Focus / TX frequency ───────────────────────────────────────────────────

// `active_slice:<trx>;` — AetherSDR extension (#4160). Read-only: reports
// which slice holds GUI focus so a control surface can follow the operator
// instead of hardcoding trx 0.
//
// Not to be confused with `set_in_focus` below — that is the inverse
// direction (a client asking us to raise our window).
//
// SET is deliberately ignored. Focus is GUI-owned; letting a remote client
// steal it would change default UX for every connected surface, which is
// RFC territory (GOVERNANCE.md, "What requires an RFC"). Silent ignore
// matches the TCI convention for commands the server does not honor.
//
// GET/SET follows the same split handleCommand() documents for every other
// command: 0-1 args = GET, 2+ = SET. active_slice has no per-TRX form, so the
// lone argument in `active_slice:0;` is redundant — but that is the shape a
// client written against the rest of this protocol (`rx_volume:0;`,
// `rx_mute:0;`) will send, and answering it is strictly more useful than
// silence. Replying to a would-be setter with the unchanged focus also tells
// them the SET did not take, which silence cannot.
QString TciProtocol::cmdActiveSlice(const QStringList& args)
{
    if (args.size() >= 2) return {};   // SET — ignored, see above
    int trx = -1;
    QString letter;
    if (!resolveActiveSlice(trx, letter)) return {};
    return QStringLiteral("active_slice:%1,%2;").arg(trx).arg(letter);
}

QString TciProtocol::cmdSetInFocus()
{
    // Client requests us to raise our window — emit via signal if needed
    // For now, just acknowledge
    return {};
}

QString TciProtocol::cmdTxFrequency()
{
    if (!m_model) return {};
    // Find TX slice frequency
    for (auto* s : m_model->slices()) {
        if (s->isTxSlice()) {
            long long hz = static_cast<long long>(std::round(s->frequency() * 1e6));
            return QStringLiteral("tx_frequency:%1;").arg(hz);
        }
    }
    return {};
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
