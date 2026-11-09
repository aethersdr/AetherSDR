#ifdef HAVE_WEBSOCKETS
#include "TciProtocol.h"
#include "ReceiveModeVocabulary.h"
#include "backends/IRadioBackend.h"
#include "AppSettings.h"
#include "LogManager.h"
#include "TciRoutingState.h"
#include "TciTrxMap.h"
#include "core/backends/RadioCapabilities.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include "models/TransmitModel.h"
#include "models/EqualizerModel.h"
#include "models/SpotModel.h"
#include "models/DaxIqModel.h"

#include <QList>
#include <QMetaObject>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <QtGlobal>
#include <utility>

namespace AetherSDR {

namespace {

// The level the operator hears, which a control surface's AF dial mirrors;
// RadioModel holds the one rule. Without a model, the PC sink's saved level.
int activeOutputVolumePercent(const RadioModel* model)
{
    return model ? model->activeOutputVolumePercent()
                 : AppSettings::instance()
                       .value(QStringLiteral("MasterVolume"), QStringLiteral("100")).toInt();
}

bool receiveTargetCurrent(const QPointer<RadioModel>& owner, const QPointer<SliceModel>& slice,
                          const RadioControlSession& session, quint64 receiverSession, bool confirmed)
{
    return owner && slice && owner->controlSession() == session
        && slice->receiveSessionGeneration() == receiverSession
        && owner->slice(slice->sliceId()) == slice
        && (!confirmed || owner->controlSessionCurrent(session));
}

}  // namespace

int TciProtocol::tciTrxForSlice(RadioModel* model, const SliceModel* slice)
{
    if (!model || !slice)
        return 0;
    const auto slices = model->slices();
    const int index = slices.indexOf(const_cast<SliceModel*>(slice));
    return index >= 0 ? index : slice->sliceId();
}

bool TciProtocol::transmitPermitted(const RadioModel* model, const SliceModel* slice)
{
    return model && slice && model->isConnected() && model->backendCanTransmit()
        && model->sliceOwnedByUs(slice->sliceId())
        && !modeIsReceiveOnly(model->backendCapabilities(), slice->mode())
        && model->panTransmitInhibitReason(slice->panId()).isEmpty();
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
        {"DSB",  "dsb"},
        {"FM",   "fm"},    {"NFM",  "nfm"},
        {"DFM",  "fm"},    {"FDM",  "fm"},
        {"DIGU", "digu"},  {"DIGL", "digl"},
        {"RTTY", "rtty"},  {"FDV",  "digu"},
        {"FDVU", "digu"},  {"FDVL", "digl"},
    };
    return map.value(mode.toUpper(), "usb");
}

QString TciProtocol::receiveModulationsList(RadioModel* model)
{
    if (!usesReceiveClientFeature(model, ReceiveClientControl::Feature::NativeModes)) {
        return QStringLiteral("modulations_list:usb,lsb,cw,cwr,am,sam,dsb,fm,nfm,digu,digl,rtty;");
    }
    QStringList tokens;
    const auto control = model->backendCapabilities().receiveModeControl;
    if (model->isConnected() && control) {
        QStringList supported;
        for (const SliceModel* slice : model->slices()) {
            for (const QString& mode : declaredReceiveModes(model, slice, false)) {
                if (!supported.contains(mode)) { supported.append(mode); }
            }
        }
        if (model->slices().isEmpty()) { supported = control->modes; }
        for (const QString& mode : supported) {
            const QString token = mode.toLower();
            if (!tokens.contains(token)) { tokens.append(token); }
        }
    }
    return QStringLiteral("modulations_list:%1;").arg(tokens.join(','));
}

QString TciProtocol::receiveModeToTci(const RadioModel* model, const SliceModel* slice)
{
    if (!slice) { return {}; }
    if (!usesDeclaredReceiveModes(model, slice)) { return smartsdrToTci(slice->mode()); }
    const QString mode = slice->mode();
    return declaredReceiveModes(model, slice, false).contains(mode) ? mode.toLower() : QString();
}

QString TciProtocol::tciToSmartSDR(const QString& mode, bool* ok)
{
    static const QMap<QString, QString> map = {
        {"usb",  "USB"},   {"lsb",  "LSB"},
        {"cw",   "CW"},    {"cwr",  "CWL"},
        {"am",   "AM"},    {"sam",  "SAM"},
        {"dsb",  "DSB"},
        {"fm",   "FM"},    {"nfm",  "NFM"},
        {"digu", "DIGU"},  {"digl", "DIGL"},
        {"rtty", "RTTY"},
    };
    const auto it = map.find(mode.toLower());
    if (ok) *ok = (it != map.end());
    return it != map.end() ? it.value() : QStringLiteral("USB");
}

// ── Helpers ────────────────────────────────────────────────────────────────

// Parse an int argument (#4867). QString::toInt() returns 0 on failure, which
// turns malformed input into a valid-looking command (#4345, #4523); a bad trx
// would land on slice 0. `out` is untouched on a missing/unparseable arg, and
// callers drop the command on false (the TCI "ignore silently" posture). Range
// clamps and lenient forms remain the caller's. Base 10 only: auto-base would
// make `mute:0x1,false` address slice 1 (tested).
static bool argToInt(const QStringList& args, int idx, int& out)
{
    if (idx < 0 || idx >= args.size()) return false;
    bool ok = false;
    const int parsed = args[idx].toInt(&ok);
    if (!ok) return false;
    out = parsed;
    return true;
}

// TCI booleans are exactly "true"/"false" (case-insensitive); anything else
// returns false so the caller drops the command (#4867), rather than
// `mute:0,yes` unmuting. Not used by cmdTune/cmdKeyer: those must fail closed,
// so any non-"true" there means stop, keeping the bare comparison.
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

// SQL_LEVEL is a threshold in dB, -140..0 (TCI 2.1 p.31). A slice stores a
// 0..100 level; the backend's published squelch scale converts between the
// two. Where the backend publishes no dB place for the slice's mode (HL2 FM,
// RTL, a Kiwi source), Flex's -160 + level map stands in as a nominal scale.
static constexpr double kSqlMinDb = -140.0;

static SquelchLevelScale sqlScaleFor(RadioModel* model, SliceModel* s)
{
    if (model && s) {
        if (auto scale = squelchScaleForMode(
                model->backendCapabilities().squelchLevelScale, s->mode())) {
            return *scale;
        }
    }
    return legacyDbmSquelchScale();
}

static int sqlLevelDb(RadioModel* model, SliceModel* s, int level)
{
    const double db = sqlScaleFor(model, s).thresholdDb(level);
    return static_cast<int>(std::lround(std::clamp(db, kSqlMinDb, 0.0)));
}

int TciProtocol::squelchLevelDb(RadioModel* model, SliceModel* s, int level)
{
    return sqlLevelDb(model, s, level);
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

// TRX of the TX slice for the request/response path (init burst, DRIVE /
// TUNE_DRIVE replies); 0 when none is TX, because the wire needs an index.
// TciServer::txTrxIndex() differs on purpose: it serves async broadcasts fired
// during a band-change slice recreate, before the TX flag returns, so it falls
// back to a cached last TX trx (#4161).
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

bool TciProtocol::txOnOtherSlice(const RadioModel* model, const SliceModel* rx)
{
    const SliceModel* tx = model ? model->txSlice() : nullptr;
    return rx && tx && tx != rx;
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

void TciProtocol::queueReceiveChange(SliceModel* slice, std::function<void()> change)
{
    if (!slice) { return; }
    if (!usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::QueuedIntentFencing)) {
        QMetaObject::invokeMethod(slice, std::move(change), Qt::QueuedConnection);
        return;
    }
    const QPointer<RadioModel> owner(m_model);
    const QPointer<SliceModel> target(slice);
    const RadioControlSession session = m_model->controlSession();
    const quint64 receiverSession = slice->receiveSessionGeneration();
    const bool confirmed = slice->receiveControlsConfirmed();
    QMetaObject::invokeMethod(slice, [owner, target, session, receiverSession, confirmed, change = std::move(change)] {
        if (receiveTargetCurrent(owner, target, session, receiverSession, confirmed)) { change(); }
    }, Qt::QueuedConnection);
}

void TciProtocol::queueReceiveFilterChange(SliceModel* slice, std::function<void()> change)
{
    if (!slice || !usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::QueuedIntentFencing)) { queueReceiveChange(slice, std::move(change)); return; }
    const QPointer<SliceModel> target(slice);
    const quint64 context = slice->receiveFilterContextGeneration();
    queueReceiveChange(slice, [target, context, change = std::move(change)] {
        if (target && target->receiveFilterContextGeneration() == context) { change(); }
    });
}

QString TciProtocol::receiveFilterBandResponse(int trx, const SliceModel* slice) const
{
    if (!slice) { return {}; }
    int low = slice->filterLow();
    int high = slice->filterHigh();
    if (usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::NativeModes)) {
        if (!m_model || m_model->slice(slice->sliceId()) != slice
            || !m_model->controlSessionCurrent(m_model->controlSession())) { return {}; }
        const SliceModel::ReceiveObservation& observed = slice->receiveObservation();
        if (!observed.mode || *observed.mode != slice->mode() || !observed.filterLowHz || !observed.filterHighHz) { return {}; }
        low = *observed.filterLowHz;
        high = *observed.filterHighHz;
        const qint64 width = qint64(high) - qint64(low);
        if (width <= 0 || width > std::numeric_limits<int>::max()) { return {}; }
    }
    return QStringLiteral("rx_filter_band:%1,%2,%3;").arg(trx).arg(low).arg(high);
}

void TciProtocol::setReceiveNotification(const SliceModel* slice, const QString& notification)
{
    // Confirmed receivers announce accepted state through the model relays,
    // rather than echoing a request which may be refused or retired in queue.
    Q_UNUSED(slice);
    if (!usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)) { m_pendingNotification = notification; }
}

bool TciProtocol::apfStateAvailable(const SliceModel* slice) const
{
    return slice && (!usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)
        || (m_model && m_model->isConnected() && m_model->backendCapabilities().hasAudioPeakingFilter));
}

QString TciProtocol::vfoLimitsResponse(const QString& legacyUnknown) const
{
    if (!usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::FrequencyLimits)) { return legacyUnknown; }
    const RadioCapabilities caps = m_model->backendCapabilities();
    // Zero/zero means unreported in the seam. Keep existing client assumptions
    // for that case; a malformed declared interval must not acquire a fallback.
    if (caps.tuningMinHz == 0.0 && caps.tuningMaxHz == 0.0) { return legacyUnknown; }
    constexpr double kUpperExclusive = -static_cast<double>(std::numeric_limits<qint64>::min());
    if (!std::isfinite(caps.tuningMinHz) || !std::isfinite(caps.tuningMaxHz)
        || caps.tuningMinHz < 0 || caps.tuningMaxHz <= caps.tuningMinHz
        || caps.tuningMaxHz >= kUpperExclusive) { return {}; }
    // TCI 2.1 §4.1 (p.14) carries device frequency limits in Hz. Round inward
    // to integer wire Hz without advertising frequencies beyond the capability.
    const double low = std::ceil(caps.tuningMinHz);
    const double high = std::floor(caps.tuningMaxHz);
    if (low > high) { return {}; }
    return QStringLiteral("vfo_limits:%1,%2;")
        .arg(static_cast<qint64>(low)).arg(static_cast<qint64>(high));
}

// ── Init burst ─────────────────────────────────────────────────────────────

QString TciProtocol::generateInitBurst()
{
    QString burst;

    // Init commands (spec 4.1), in spec order. READY is sent last, after the full
    // state including stream parameters, as ExpertSDR3 does: SDC / CW Skimmer
    // latch settings on READY, so an early READY gives them defaults (notably a
    // wrong iq_samplerate). eesdr-tci imposes no pre-READY grammar.
    burst += vfoLimitsResponse(QStringLiteral("vfo_limits:1000,75000000;"));
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

    // Identity chosen so WSJT-X's TCITransceiver keeps full TX amplitude: it
    // halves samples (K2 = 0.499/0x7FFF vs K1 = 0.999/0x7FFF) only when device is
    // "SunSDR2DX"/"SunSDR2PRO" AND protocol doesn't start with "ExpertSDR3"; we fail
    // both. Any "ExpertSDR3" protocol selects WSJT-X's ESDR3 command formats;
    // WSJT-X, JTDX and eesdr-tci read the name, not the version. The
    // RF2K-S whitelist (SunSDR2DX + ExpertSDR2) is not matched; a configurable
    // identity is #2806.
    burst += QStringLiteral("device:AetherSDR;");
    burst += QStringLiteral("receive_only:false;");
    burst += receiveModulationsList(m_model);
    burst += QStringLiteral("protocol:ExpertSDR3,2.1;");

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
            // GUI split: the TX slice is trx 0's VFO B (see TciServer::publishMainSplit).
            if (trx == 0 && (!txVfo || txVfo == s) && txOnOtherSlice(m_model, s)) {
                txVfo = m_model->txSlice();
            }
            const long long txHz = mhzToHz(txVfo ? txVfo->frequency() : s->frequency());
            burst += QStringLiteral("vfo:%1,1,%2;").arg(trx).arg(txHz);
            // dds: = IQ-stream center (panadapter center). Skimmers learn the
            // IQ center only from this; without it every spot is mis-mapped by
            // (slice − panCenter) Hz. (#3910)
            burst += QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsCenterHz(m_model, s));
            const QString mode = receiveModeToTci(m_model, s);
            if (!mode.isEmpty()) { burst += QStringLiteral("modulation:%1,%2;").arg(trx).arg(mode); }
            burst += QStringLiteral("rx_enable:%1,true;").arg(trx);

            // Filter
            burst += receiveFilterBandResponse(trx, s);

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
                         .arg((m_routingState && m_routingState->splitRequested())
                                      || (trx == 0 && txOnOtherSlice(m_model, s))
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
                         .arg(trx).arg(sqlLevelDb(m_model, s, s->receiveSquelchLevel()));

            // AGC
            burst += QStringLiteral("agc_mode:%1,%2;")
                         .arg(trx).arg(s->receiveAgcMode().toLower());
            burst += QStringLiteral("agc_gain:%1,%2;")
                         .arg(trx).arg(s->receiveAgcThreshold());

            // DSP
            burst += QStringLiteral("rx_nb_enable:%1,%2;")
                         .arg(trx).arg(s->nbOn() ? "true" : "false");
            burst += QStringLiteral("rx_nr_enable:%1,%2;")
                         .arg(trx).arg(s->nrOn() ? "true" : "false");
            burst += QStringLiteral("rx_anf_enable:%1,%2;")
                         .arg(trx).arg(s->anfOn() ? "true" : "false");
            if (apfStateAvailable(s)) {
                burst += QStringLiteral("rx_apf_enable:%1,%2;")
                    .arg(trx).arg(s->apfOn() ? "true" : "false");
            }

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
                         .arg(trx).arg(transmitPermitted(m_model, s) ? "true" : "false");
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
        burst += QStringLiteral("tune:%1,%2;").arg(txTrxIndex).arg(tx.isTuning() ? "true" : "false");

        // Master AF volume — whole-radio (no trx prefix), same saved value
        // cmdVolume's GET returns, reported in dB per the TCI spec
        // (-60..0; ExpertSDR3 wire scale). Without it the init burst seeds
        // drive/mic_level but not AF, so a client's local mirror starts at
        // a default guess and the first AF-gain step jumps to that guess
        // instead of the radio's real level (Ulanzi/Elgato/StreamController
        // gain steppers).
        burst += QStringLiteral("volume:%1;")
                     .arg(volumeDbFromPercent(activeOutputVolumePercent(m_model)));

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
    burst += QStringLiteral("iq_samplerate:%1;").arg(m_iqSampleRate);

    // START belongs in the state dump before READY: WSJT-X gates its
    // frequency/PTT path on the "switched on" flag that only START sets, and checks
    // it at READY (#5007). Stream lifecycle commands (audio_start/iq_start) are
    // still never emitted here (#3913).
    burst += QStringLiteral("start;");

    // READY terminates the settings dump — it must follow EVERY setting
    // (in particular iq_samplerate and start), because SDC / CW Skimmer
    // reads its cached settings the moment READY arrives.  Matches real
    // ExpertSDR3 behavior and the spec's READY definition ("sent after
    // the initialization commands").  Reported by Yuri UT4LW.
    burst += QStringLiteral("ready;");

    return burst;
}

// ── Command dispatch ───────────────────────────────────────────────────────

QString TciProtocol::handleCommand(const QString& cmd)
{
    m_pendingNotification.clear();
    m_pendingMasterVolume = -1;
    m_pendingTxGain = -1;
    m_centerLockRequest.reset();
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
    // iq_start / iq_stop / iq_samplerate are handled entirely in
    // TciServer::onTextMessage, which owns the shared DAX IQ stream lifecycle
    // (per-receiver subscription sets, pan↔channel binding, refcounting across
    // clients). The single-stream implementations that used to live here were
    // removed rather than left unreachable: two implementations of one
    // lifecycle, one of them wrong, is how the wrong one comes back.
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
    if (name == "vfo_limits")       return vfoLimitsResponse(QStringLiteral("vfo_limits:30000,54000000;"));
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

    // Thetis extension, see cmdRxCtunEx
    if (name == "rx_ctun_ex")       return cmdRxCtunEx(args, isSet);

    // Unknown command — ignore silently per TCI spec
    return {};
}

QString TciProtocol::pendingNotification()
{
    QString n = m_pendingNotification;
    m_pendingNotification.clear();
    return n;
}

std::optional<TciProtocol::CenterLockRequest> TciProtocol::takeCenterLockRequest()
{
    std::optional<CenterLockRequest> request = std::move(m_centerLockRequest);
    m_centerLockRequest.reset();
    return request;
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

std::optional<TciProtocol::TuneRequest> TciProtocol::takeTuneRequest()
{
    std::optional<TuneRequest> request = std::move(m_tuneRequest);
    m_tuneRequest.reset();
    return request;
}

// ── Command implementations ────────────────────────────────────────────────

QString TciProtocol::cmdStart()
{
    m_started = true;
    return QStringLiteral("start;");
}

QString TciProtocol::cmdStop()
{
    m_started = false;
    return QStringLiteral("stop;");
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
        const QString mode = receiveModeToTci(m_model, s);
        return mode.isEmpty() ? QString() : QStringLiteral("modulation:%1,%2;").arg(trx).arg(mode);
    }

    if (args.size() < 2) return {};
    bool modOk = false;
    QString sdrMode = tciToSmartSDR(args[1], &modOk);
    if (usesDeclaredReceiveModes(m_model, s)) {
        const QStringList modes = declaredReceiveModes(m_model, s, true);
        const QString exact = args[1].toUpper();
        if (modes.contains(exact)) { sdrMode = exact; modOk = true; }
        else { modOk = modOk && modes.contains(sdrMode); }
    }
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
    queueReceiveChange(s, [s, sdrMode]() {
        s->setMode(sdrMode);
    });

    setReceiveNotification(s, QStringLiteral("modulation:%1,%2;")
                                .arg(trx).arg(args[1].toLower()));
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

    m_trxRequest = parseTrxRequest(args);
    return {};
}

std::optional<TciProtocol::TrxRequest> TciProtocol::parseTrxRequest(const QStringList& args)
{
    int trx = 0;
    if (args.size() < 2 || !argToInt(args, 0, trx) || trx < 0) { return {}; }
    const QString state = args[1].trimmed().toLower();
    if (state != QStringLiteral("true") && state != QStringLiteral("false")) { return {}; }
    const QString source = args.size() >= 3 ? args[2].trimmed().toLower() : QString();
    return TrxRequest{trx, state == QStringLiteral("true"), source};
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
    if (trx < 0 && tune) {
        // A malformed start keys nothing; it is answered with the real state.
        const bool tuning = m_model && m_model->transmitModel().isTuning();
        return QStringLiteral("tune:%1,%2;").arg(txTrx()).arg(tuning ? "true" : "false");
    }
    if (trx < 0) {
        trx = txTrx();
    }
    // TciServer applies it, so the requester is answered with the state the
    // model reached — a refused start included.
    m_tuneRequest = TuneRequest{trx, tune};
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

// mic_level: global 0-100 % via TransmitModel::setMicLevel() (mic is
// whole-radio). Also accepts `mic_level:0,N;`, ignoring the trx.
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
    queueReceiveChange(s, [s, on, hz]() { s->setRit(on, hz); });

    setReceiveNotification(s, QStringLiteral("rit_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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
    queueReceiveChange(s, [s, on, offset]() {
        s->setRit(on, offset);
    });

    setReceiveNotification(s, QStringLiteral("rit_offset:%1,%2;")
                                .arg(trx).arg(offset));
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

        // A split made in the GUI is just as real; without a routing state
        // every receiver reports it, as before.
        if ((trx == 0 || !m_routingState) && txOnOtherSlice(m_model, s)) {
            split = true;
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
        return receiveFilterBandResponse(trx, s);
    }

    if (args.size() < 3) return {};
    int lo = 0;
    int hi = 0;
    if (!argToInt(args, 1, lo) || !argToInt(args, 2, hi)) return {};
    queueReceiveFilterChange(s, [s, lo, hi]() {
        s->setFilterWidth(lo, hi);
    });

    setReceiveNotification(s, QStringLiteral("rx_filter_band:%1,%2,%3;")
                                .arg(trx).arg(lo).arg(hi));
    return {};
}

// ── CW ─────────────────────────────────────────────────────────────────────

// Global commands (no trx) derive GET/SET from the argument count and read
// the value from the last argument, rather than trusting handleCommand()'s
// `isSet = args.size() >= 2` (per-slice shape): spec `cw_macros_speed:25;` is
// a SET, and `cw_macros_speed:0,25;` sets 25, not 0. Same as cmdVolume,
// cmdMicLevel and cmdTxGain.
QString TciProtocol::cmdCwMacrosSpeed(const QStringList& args, bool /*isSet*/)
{
    if (args.isEmpty()) {
        int wpm = m_model ? m_model->transmitModel().cwSpeed() : 25;
        return QStringLiteral("cw_macros_speed:%1;").arg(wpm);
    }

    if (!m_model) return {};
    int wpm = 0;
    if (!argToInt(args, args.size() == 1 ? 0 : 1, wpm)) return {};
    if (wpm < m_model->cwTextMinWpm() || wpm > m_model->cwTextMaxWpm()) {
        return {};
    }
    QMetaObject::invokeMethod(m_model, [model = m_model, wpm]() {
        model->transmitModel().setCwSpeed(wpm);
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
    QMetaObject::invokeMethod(m_model, [model = m_model, text]() {
        // Capability check runs HERE, on the model's thread, not in the caller:
        // this method is driven by the TCI client socket and RadioModel state is
        // not ours to read from it.
        if (!model->hasRadioSideCwKeyer()) {
            qCWarning(lcCat) << "TCI: cw_msg ignored \u2014 radio has no radio-side "
                                "CW keyer";
            return;
        }
        const QString rejection = model->cwTextValidationError(text);
        if (!rejection.isEmpty()) {
            qCWarning(lcCat) << "TCI: cw_msg ignored:" << rejection;
            return;
        }
        model->cwxModel().send(text);
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
    queueReceiveChange(s, [s, on]() { s->setLocked(on); });

    setReceiveNotification(s, QStringLiteral("lock:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
    return {};
}

// ── RX_CTUN_EX (Thetis extension) ──────────────────────────────────────────
//
// `rx_ctun_ex:<trx>,<bool>;` is Thetis's click-tune switch: true lets the VFO
// move inside a fixed panadapter, false keeps the VFO centred and drags the
// pan along. AetherSDR's equivalent is the inverse, center lock: ctun true =
// center lock off. Additive; TCI 2.1 has no command for it. Clients that
// draw their own spectrum (TCI Remote) send `rx_ctun_ex:0,true;` on connect
// because a locked pan moves under them on every tune.
//
// Center lock is MainWindow state, so a SET is stashed for TciServer and a
// GET is answered through the injected query, if any.

QString TciProtocol::cmdRxCtunEx(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!isSet) {
        if (!m_centerLockQuery) return {};
        return QStringLiteral("rx_ctun_ex:%1,%2;")
                   .arg(trx).arg(m_centerLockQuery(s->sliceId()) ? "false" : "true");
    }

    if (args.size() < 2) return {};
    bool ctun = false;
    if (!argToBool(args, 1, ctun)) return {};
    m_centerLockRequest = CenterLockRequest{trx, s->sliceId(), !ctun};

    setReceiveNotification(s, QStringLiteral("rx_ctun_ex:%1,%2;")
                                .arg(trx).arg(ctun ? "true" : "false"));
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
    auto& pending = (*m_pendingSquelch)[trx];
    if (usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::QueuedIntentFencing)) {
        const RadioControlSession session = m_model->controlSession();
        const quint64 receiverSession = s->receiveSessionGeneration();
        if (pending.source != s || pending.session != session || pending.receiverSession != receiverSession) { pending = {}; }
        pending.source = s;
        pending.session = session;
        pending.receiverSession = receiverSession;
    }
    pending.on = on;
    queueSquelch(s, trx, on, pending.level.value_or(s->receiveSquelchLevel()));

    setReceiveNotification(s, QStringLiteral("sql_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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
                   .arg(trx).arg(sqlLevelDb(m_model, s, s->receiveSquelchLevel()));
    }

    if (args.size() < 2) return {};
    double value = 0.0;
    if (!argToDouble(args, 1, value)) return {};
    // The spec defines dB only. A positive value is outside its range, and is
    // most likely an older client's 0..100 level; read as dB it would close
    // the squelch, so it is ignored (TCI 2.1 p.6). At or below what level 0
    // is reported as (-140 dB, or a higher floor the scale publishes), the
    // squelch opens fully, so level 0 stays reachable and its own echo, sent
    // back, does not move it.
    if (value > 0.0) return {};
    const int level = (value <= sqlLevelDb(m_model, s, 0))
        ? 0
        : sqlScaleFor(m_model, s).levelForThresholdDb(value);
    auto& pending = (*m_pendingSquelch)[trx];
    if (usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::QueuedIntentFencing)) {
        const RadioControlSession session = m_model->controlSession();
        const quint64 receiverSession = s->receiveSessionGeneration();
        if (pending.source != s || pending.session != session || pending.receiverSession != receiverSession) { pending = {}; }
        pending.source = s;
        pending.session = session;
        pending.receiverSession = receiverSession;
    }
    pending.level = level;
    queueSquelch(s, trx, pending.on.value_or(s->receiveSquelchOn()), level);

    setReceiveNotification(s, QStringLiteral("sql_level:%1,%2;")
                                .arg(trx).arg(sqlLevelDb(m_model, s, level)));
    return {};
}

void TciProtocol::queueSquelch(SliceModel* s, int trx, bool on, int level)
{
    if (!usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::QueuedIntentFencing)) {
        QMetaObject::invokeMethod(s, [s, on, level, trx, pending = m_pendingSquelch]() {
            pending->erase(trx);
            s->setSquelch(on, level);
        }, Qt::QueuedConnection);
        return;
    }
    const QPointer<RadioModel> owner(m_model);
    const QPointer<SliceModel> target(s);
    const RadioControlSession session = m_model ? m_model->controlSession() : RadioControlSession{};
    const quint64 receiverSession = s->receiveSessionGeneration();
    const bool confirmed = s->receiveControlsConfirmed();
    QMetaObject::invokeMethod(s, [owner, target, session, receiverSession, confirmed, on, level, trx, pending = m_pendingSquelch]() {
        const auto entry = pending->find(trx);
        if (entry != pending->end() && entry->second.source == target.data() && entry->second.session == session
            && entry->second.receiverSession == receiverSession) {
            pending->erase(entry);
        }
        if (receiveTargetCurrent(owner, target, session, receiverSession, confirmed)) { target->setSquelch(on, level); }
    }, Qt::QueuedConnection);
}

// ── Volume / Mute ──────────────────────────────────────────────────────────

// VOLUME is GLOBAL per TCI v2.0: the master output level (title bar slider);
// per-receiver volume is `rx_volume` (cmdRxVolume).
//   GET = `volume;`   SET = `volume:N;` (dB, -60..0; -60 = silence)
// Internally master volume is 0-100 % amplitude, so dB<->percent converts at
// the wire. Values >= 1 can't be dB and are taken as legacy percent;
// `volume:0` is 0 dB = full volume (muting is `mute:`). `volume:trx,N;` is
// accepted with the trx ignored. SET is stashed in m_pendingMasterVolume
// (TciProtocol doesn't own AudioEngine); TciServer forwards it to MainWindow
// like masterVolumeChanged.

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
        // GET — the level of the output the operator hears, in dB.
        const int pct = activeOutputVolumePercent(m_model);
        return QStringLiteral("volume:%1;").arg(volumeDbFromPercent(pct));
    }

    // SET: spec form (1 arg) or legacy trx-prefixed (2+). An empty value
    // ("volume:" or "volume:0,") is malformed and dropped rather than read as 0 dB
    // = loudest (#4523). argToDouble also rejects "inf"/"nan". The percent branch
    // below stays lenient: bundled plugins send it.
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
        queueReceiveChange(s, [s, mute]() { s->setAudioMute(mute); });
    }

    setReceiveNotification(s, QStringLiteral("mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false"));
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
    queueReceiveChange(s, [s, mode]() { s->setAgcMode(mode); });

    setReceiveNotification(s, QStringLiteral("agc_mode:%1,%2;")
                                .arg(trx).arg(mode));
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
    queueReceiveChange(s, [s, gain]() { s->setAgcThreshold(gain); });

    setReceiveNotification(s, QStringLiteral("agc_gain:%1,%2;")
                                .arg(trx).arg(gain));
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
    queueReceiveChange(s, [s, on]() { s->setNb(on); });

    setReceiveNotification(s, QStringLiteral("rx_nb_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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
    // A radio with no radio-side NR (HL2, ANAN) cannot turn it on. TCI has no
    // error reply, so the refusal is the truth broadcast back: NR is off.
    if (on && m_model && !m_model->radioSideNoiseReductionAvailable()) {
        setReceiveNotification(s, QStringLiteral("rx_nr_enable:%1,false;").arg(trx));
        return {};
    }
    queueReceiveChange(s, [s, on]() { s->setNr(on); });

    setReceiveNotification(s, QStringLiteral("rx_nr_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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
    // Same refusal as rx_nr_enable, where the radio has no auto notch.
    if (on && m_model && !m_model->radioSideAutoNotchAvailable()) {
        setReceiveNotification(s, QStringLiteral("rx_anf_enable:%1,false;").arg(trx));
        return {};
    }
    queueReceiveChange(s, [s, on]() { s->setAnf(on); });

    setReceiveNotification(s, QStringLiteral("rx_anf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
    return {};
}

QString TciProtocol::cmdRxApfEnable(const QStringList& args, bool isSet)
{
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    auto* s = sliceForTrx(trx);
    if (!s) return {};

    if (!apfStateAvailable(s)) { return {}; }
    if (!isSet) {
        return QStringLiteral("rx_apf_enable:%1,%2;")
                   .arg(trx).arg(s->apfOn() ? "true" : "false");
    }

    if (args.size() < 2) return {};
    bool on = false;
    if (!argToBool(args, 1, on)) return {};
    queueReceiveChange(s, [s, on]() { s->setApf(on); });

    setReceiveNotification(s, QStringLiteral("rx_apf_enable:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
    return {};
}

// ── AetherSDR extensions (slice quick-record record/play) ──────────────────

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
    queueReceiveChange(s, [s, on]() { s->setRecordOn(on); });

    setReceiveNotification(s, QStringLiteral("rx_record:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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
    queueReceiveChange(s, [s, on]() { s->setPlayOn(on); });

    setReceiveNotification(s, QStringLiteral("rx_play:%1,%2;")
                                .arg(trx).arg(on ? "true" : "false"));
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

// See the contract on cwMacrosTextFromArgs() in TciProtocol.h for why numeric
// first arguments fail closed while non-numeric ones retain compatibility.
QString TciProtocol::cwMacrosTextFromArgs(const QStringList& args, int trxCount)
{
    if (args.isEmpty()) {
        return {};
    }
    if (trxCount < 1) {
        trxCount = 1;
    }

    // A base-10 integer in the receiver slot is always an address. If it no
    // longer names a live advertised receiver, fail closed rather than
    // reinterpreting it as text and putting the stale index on the air.
    //
    // args.join(',') on the TAIL, not on everything: the join is what lets a
    // message legitimately CONTAIN commas (`cw_macros:0,CQ,CQ` keys "CQ,CQ"),
    // which is why the original code joined at all. Only the index was wrong.
    int trx = 0;
    if (argToInt(args, 0, trx)) {
        if (trx < 0 || trx >= trxCount) {
            return {};
        }
        return args.mid(1).join(',');
    }
    return args.join(',');
}

QString TciProtocol::cmdCwMacros(const QStringList& args)
{
    if (!m_model || args.isEmpty()) {
        return {};
    }

    // TciServer and RadioModel share the GUI thread. Resolve against the
    // receiver map now, while handling the command, so slice churn cannot
    // change how the same wire message is interpreted one event-loop turn
    // later. Only validated text crosses the queued boundary.
    const int trxCount = m_trxMap ? m_trxMap->trxCount(m_model)
                                  : static_cast<int>(m_model->slices().size());
    const QString text = cwMacrosTextFromArgs(args, trxCount);
    if (text.isEmpty()) {
        qCWarning(lcCat) << "TCI: cw_macros ignored \u2014 empty text or "
                            "invalid receiver index";
        return {};
    }

    QMetaObject::invokeMethod(m_model, [model = m_model, text]() {
        if (!model->hasRadioSideCwKeyer()) {
            qCWarning(lcCat) << "TCI: cw_macros ignored \u2014 radio has no "
                                "radio-side CW keyer";
            return;
        }
        const QString rejection = model->cwTextValidationError(text);
        if (!rejection.isEmpty()) {
            qCWarning(lcCat) << "TCI: cw_macros ignored:" << rejection;
            return;
        }
        model->cwxModel().send(text);
    }, Qt::QueuedConnection);
    return {};
}

QString TciProtocol::cmdCwMacrosStop()
{
    if (!m_model) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model]() {
        if (!model->hasRadioSideCwKeyer()) {
            return;
        }
        model->cwxModel().clearBuffer();
    }, Qt::QueuedConnection);
    return {};
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
    if (wpm < m_model->cwTextMinWpm() || wpm > m_model->cwTextMaxWpm()) return {};
    QMetaObject::invokeMethod(m_model, [model = m_model, wpm]() {
        model->transmitModel().setCwSpeed(wpm);
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
    queueReceiveChange(s, [s, gain]() { s->setAudioGain(gain); });
    setReceiveNotification(s, QStringLiteral("rx_volume:%1,%2;")
                                .arg(trx).arg(static_cast<int>(gain)));
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
    queueReceiveChange(s, [s, mute]() { s->setAudioMute(mute); });
    setReceiveNotification(s, QStringLiteral("rx_mute:%1,%2;")
                                .arg(trx).arg(mute ? "true" : "false"));
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
    queueReceiveChange(s, [s, flexPan]() { s->setAudioPan(flexPan); });
    setReceiveNotification(s, QStringLiteral("rx_balance:%1,%2;")
                                .arg(trx).arg(tciBalance));
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
    queueReceiveChange(s, [s, level]() { s->setNbLevel(level); });
    setReceiveNotification(s, QStringLiteral("rx_nb_param:%1,0,%2;")
                                .arg(trx).arg(level));
    return {};
}

QString TciProtocol::cmdRxBinEnable(const QStringList& args, bool isSet)
{
    // Binaural — no direct FlexRadio equivalent, acknowledge only
    static bool binEnabled = false;
    if (args.isEmpty()) return {};
    int trx = 0;
    if (!argToInt(args, 0, trx)) return {};
    const SliceModel* slice = sliceForTrx(trx);
    if (slice && usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)) { return {}; }
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
    const SliceModel* slice = sliceForTrx(trx);
    if (slice && usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)) { return {}; }
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
    const SliceModel* slice = sliceForTrx(trx);
    if (slice && usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)) { return {}; }
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
    const SliceModel* slice = sliceForTrx(trx);
    if (slice && usesReceiveClientFeature(m_model, ReceiveClientControl::Feature::ConfirmedNotifications)) { return {}; }
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
    queueReceiveChange(s, [s, hz]() { s->setDiglOffset(hz); });
    setReceiveNotification(s, QStringLiteral("digl_offset:%1;").arg(hz));
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
    queueReceiveChange(s, [s, hz]() { s->setDiguOffset(hz); });
    setReceiveNotification(s, QStringLiteral("digu_offset:%1;").arg(hz));
    return {};
}

// ── Focus / TX frequency ───────────────────────────────────────────────────

// `active_slice:<trx>;` — AetherSDR extension (#4160), read-only: reports the
// GUI-focused slice so control surfaces can follow it. (`set_in_focus` is the
// opposite direction.) SET (2+ args) is ignored: focus is GUI-owned and
// remote focus-steal would need an RFC. A 1-arg form is answered as a GET,
// since that's what clients shaped like `rx_mute:0;` send.
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
