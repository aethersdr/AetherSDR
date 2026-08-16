#ifdef HAVE_WEBSOCKETS
#include "TciServer.h"
#include "TciProtocol.h"
#include "StreamStatus.h"
#include "AudioEngine.h"
#include "AppSettings.h"
#include "Resampler.h"
#include "LogManager.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/PanadapterModel.h"
#include "models/DaxIqModel.h"
#include "models/MeterModel.h"
#include "models/TransmitModel.h"
#include "models/SpotModel.h"

#include <QWebSocketServer>
#include <QWebSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QStringList>
#include <QTimer>
#include <QPointer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

namespace AetherSDR {

namespace {
// Server-side caps closing the unbounded-frame / unbounded-client surface
// flagged in GHSA-7w4w-wfqm-wh93 (M2).  QWebSocket message/frame sizes
// default to 1 GiB in Qt6 — wildly more than any legitimate TCI command
// or audio frame.  64 KiB easily covers the largest legitimate TCI text
// command and is enforced at the framing layer.  Eight concurrent clients
// matches the rigctld cap.
constexpr qint64 kMaxWsMessageBytes = 64 * 1024;
constexpr int    kMaxClients        = 8;
// Grace period before tearing down DAX RX after the last audio client drops.
// A TCP drop is frequently transient (WSJT-X throws on a CAT timeout — e.g. a
// vfo: echo delayed by an ATU tune — then reconnects). Deferring the teardown
// lets the stream survive the blip so audio resumes with no recreate; a
// reconnecting client cancels it. (#3363/#3476 + Tune/ATU)
// Measured drop→audio_start gaps in the field repros: 2.1s / 3.3s / 3.5s —
// and WSJT-X is slowest to reconnect mid-FT8-decode, exactly when these
// throws happen. 10s gives ~3x margin; the cost of lingering after a genuine
// quit is just an unconsumed stream + dax flag for a few extra seconds.
constexpr int    kDaxReleaseGraceMs = 10000;
}

// ── TCI binary audio frame header (per ExpertSDR3 TCI spec v2.0) ────────
// 9 × uint32 = 36 bytes, followed by sample payload
// TCI audio header: 16 × uint32 = 64 bytes
// Per ExpertSDR3 TCI spec v2.0 Stream struct
struct TciAudioHeader {
    quint32 receiver;     // receiver/TRX number
    quint32 sampleRate;   // Hz
    quint32 format;       // 0=int16, 1=int24, 2=int32, 3=float32
    quint32 codec;        // 0 (uncompressed)
    quint32 crc;          // 0 (unused)
    quint32 length;       // number of real samples in data
    quint32 type;         // 0=IQ, 1=RX_AUDIO, 2=TX_AUDIO, 3=TX_CHRONO
    quint32 channels;     // 1 or 2
    quint32 reserved[8];  // zero-filled
};
static_assert(sizeof(TciAudioHeader) == 64, "TCI audio header must be 64 bytes");

namespace {

constexpr int kTxChronoSamples = 2048; // float payload length sent to WSJT-X
constexpr int kTxChronoStereoFrames = kTxChronoSamples / 2;
constexpr qint64 kTxChronoPeriodNs =
    (static_cast<qint64>(kTxChronoStereoFrames) * 1000000000LL) / 48000LL;
constexpr int kTxChronoPollMs = 5;
constexpr qint64 kTxSummaryEveryBlocks = 48;

// Minimum gap between drive:/tune_drive: sends (#4161). Measured on a
// FLEX-6600 over SmartLink: one RF-power slider drag emitted 40 `drive:`
// broadcasts in ~900 ms — without this, every one of those reaches every
// client. With it, the same drag settles to ~20 over 2.8 s.
constexpr int kPowerRateLimitMs = 100;

// parseStatusHandle / streamStatusBelongsToUs  → StreamStatus.h
// trx↔slice mapping                            → TciTrxMap (m_trxMap, #4567)

// txTrxIndex / trxHasLiveSlice (#4161) moved onto TciTrxMap (#4567) — the
// TX-trx scan and the close-vs-recreate discrimination both need the stable
// sliceId→trx bindings, which only the server's map instance holds. The -1
// "no TX slice" sentinel semantics are unchanged (see the broadcastPower
// call site): -1, not 0, because trx 0 is a legitimate TX slice.

} // namespace

TciServer::TciServer(RadioModel* model, QObject* parent)
    : QObject(parent)
    , m_model(model)
{
    // Load per-channel RX gains from persistence (decoupled from DaxRxGain<n>, #1627).
    // Migrate DaxRxGain<n> → TciRxGain<n> on first read so existing users keep
    // their current balance when the applets split.
    {
        auto& s = AppSettings::instance();
        for (int ch = 1; ch <= 8; ++ch) {
            const QString key = QStringLiteral("TciRxGain%1").arg(ch);
            if (!s.contains(key)) {
                const QString legacy = s.value(QStringLiteral("DaxRxGain%1").arg(ch), "0.5").toString();
                s.setValue(key, legacy);
            }
            m_rxChannelGain[ch - 1] = std::clamp(
                s.value(key, "0.5").toString().toFloat(), 0.0f, 1.0f);
        }
        s.save();
    }

    // Cache S-meter values for periodic broadcast (avoid flooding clients)
    if (m_model) {
        connect(&m_model->meterModel(), &MeterModel::sLevelChanged,
                this, [this](int sliceIndex, float dbm) {
            if (sliceIndex >= 0 && sliceIndex < 8)
                m_cachedSLevel[sliceIndex] = dbm;
        });
    }

    // Cache TX meter values
    if (m_model) {
        m_lastRadioTx = m_model->isRadioTransmitting();
        connect(m_model, &RadioModel::radioTransmittingChanged, this,
            &TciServer::onRadioTransmittingChanged);
        connect(&m_model->meterModel(), &MeterModel::txMetersChanged,
                this, [this](float fwd, float swr, bool swrValid) {
            m_cachedFwdPower = fwd;
            // TCI's wire format has no absent marker; 1.0 is what this cache
            // held before any SWR arrived, so absence maps back to it rather
            // than to 0.0 (which is out of the meter's domain).
            m_cachedSwr = swrValid ? swr : 1.0f;
        });
        connect(&m_model->meterModel(), &MeterModel::micMetersChanged,
                this, [this](float micLevel, float, float, float) {
            m_cachedMicLevel = micLevel;
        });
        connect(&m_model->meterModel(), &MeterModel::swAlcChanged,
                this, [this](float dbfs) {
            m_cachedAlc = dbfs;
        });

        // RF/tune power → `drive:` / `tune_drive:` broadcast (#4161). Without
        // this, power was announced only in the init burst and as an echo to
        // the client that set it: a GUI change or the radio's own per-band
        // power restore on QSY stayed invisible to every TCI client until
        // reconnect, leaving control-surface dials showing a stale figure
        // while the operator keyed an amplifier against it (#4310).
        connect(&m_model->transmitModel(), &TransmitModel::rfPowerChanged,
                this, [this](int) { m_drivePending = true; queuePowerBroadcast(); });
        connect(&m_model->transmitModel(), &TransmitModel::tunePowerChanged,
                this, [this](int) { m_tuneDrivePending = true; queuePowerBroadcast(); });
    }

    // Capture DAX RX stream creation responses so we can register them
    // in PanadapterStream for VITA-49 routing (#1331).
    if (m_model) {
        // Stream registration + radio-side-removal recovery now live in the
        // centralized DAX channel manager (RadioModel::handleDaxRxStreamRegistry
        // + PanadapterStream refcounting, #3305). The #3476 "profile load
        // destroyed the stream, never came back" recreate is automatic there.
        // TCI only keeps its channel→trx routing cache truthful (#3669/#3766).
        // The PanadapterStream::daxStreamUnregistered → onDaxStreamUnregistered
        // subscription is made by MainWindow's stream-sink helper (not here) so it
        // is re-established after a backend/family swap destroys the stream (#4448).

        // Re-trigger DAX setup when the radio (re)connects or a slice
        // is added AFTER a TCI client has already requested audio.  Without
        // this, a client that races the radio connect — WSJT-X started
        // before AetherSDR finishes its handshake, or before any slice
        // exists — sets `audioEnabled=true` but ensureDaxForTci()
        // silently no-ops on `!isConnected()` / empty slices, and never
        // gets a second chance.  Result: CAT and TX audio look fine
        // (text channel is alive) but no DAX RX stream is ever created,
        // so the radio sends no audio frames and WSJT-X RX stays silent.
        // (#3270)
        connect(m_model, &RadioModel::connectionStateChanged,
                this, [this](bool connected) {
            if (!connected) {
                // Radio dropped: RadioModel resets the DAX channel manager
                // (the radio reaps our streams server-side, #3305). Drop the
                // routing cache and the slice-assignment bookkeeping: slices
                // are being destroyed with the connection, and a
                // releaseDaxForTci() that runs later (e.g. the debounced grace
                // timer firing after a quick radio reconnect) must not
                // setDaxChannel(0) on the RECREATED slices — that would strip
                // a profile-restored DAX assignment from a slice we no longer
                // manage.
                m_channelTrx.clear();
                m_tciDaxSlices.clear();
                m_trxMap.clear();  // #4567: slices die with the connection
                m_lastDdsCenterHz.clear();
                m_routingState.reset();
                m_pendingVfoBCreate.reset();
                m_pendingTrxRequest.reset();
                m_pendingRouteCommands.clear();
                m_routeTransitionInFlight = false;
                ++m_routeTransitionGeneration;
                m_tciPttRequestedOn = false;
                m_tciPttConfirmedOn = false;
                m_tciPttCancelPending = false;
                m_tciPttWantsAudio = false;
                m_tciPttClient.clear();
                stopTxChrono();
                return;
            }
            for (const auto& cs : m_clients) {
                if (cs.audioEnabled) {
                    qCInfo(lcCat) << "TCI: radio reconnected — re-arming DAX"
                                  << "for pending audio client (#3270)";
                    ensureDaxForTci();
                    return;
                }
            }
        });
        connect(m_model, &RadioModel::sliceAdded,
                this, [this](SliceModel* s) {
            // #4567: bind the receiver number FIRST, before anything below
            // (or any later-connected handler) derives a trx for this slice.
            // A recreate (same Flex slice id, removal < 500 ms ago) reuses
            // its existing binding; a genuinely new slice gets the lowest
            // free number.
            //
            // Bind by walking EVERY live slice in list order, not just the
            // new one (#4577 review): after a reconnect the previous
            // session's slices are reclaimed by the status replay without
            // sliceAdded (RadioModel's !reclaimed guard) while the map was
            // cleared at disconnect — live slices with no binding. Acquiring
            // only the new slice would hand it trx 0 on top of a slice the
            // fallback resolves positionally to 0. The walk is idempotent
            // (acquire reuses existing bindings) and on an empty map
            // reproduces exactly the positional numbering, restoring the
            // invariant that every live slice is bound. The added slice is
            // already in the list here (append precedes the emit).
            if (s) {
                for (SliceModel* live : m_model->slices()) {
                    if (live)
                        m_trxMap.acquire(live->sliceId());
                }
            }
            for (const auto& cs : m_clients) {
                if (cs.audioEnabled) {
                    qCInfo(lcCat) << "TCI: slice added — re-arming DAX"
                                  << "for active audio client (#3270)";
                    ensureDaxForTci();
                    return;
                }
            }
        });
        // A removed slice never fires daxChannelChanged, so without this the
        // Tci hold on its channel stays set forever and the dax_rx stream
        // lingers until the TCI client disconnects (pre-existing orphan,
        // closed alongside #3305 per PR #4017 review item 4). Release any
        // Tci-held channel that no remaining slice carries; the sliceAdded
        // re-arm above re-acquires when a replacement slice appears.
        connect(m_model, &RadioModel::sliceRemoved,
                this, [this](int sliceId) {
            const bool removedTxRoute = sliceId == m_routingState.txSliceId();
            m_routingState.removeSlice(sliceId);
            if (removedTxRoute && (m_tciPttRequestedOn || m_tciPttConfirmedOn)) {
                abortTciPtt();
            }
            m_tciDaxSlices.remove(sliceId);

            // Under the #4567 sticky map a removal does NOT renumber the
            // survivors (that renumbering was #4160's original concern) —
            // publishActiveTrx() still runs because the removed slice may
            // have been the focused one, and the active-trx broadcast must
            // move off the dead index.
            publishActiveTrx();

            // m_lastTxTrx caches the last TX slice's trx so a power change
            // during the band-change slice-recreation gap still labels
            // drive:/tune_drive: correctly (the recreated slice exists but has
            // not regained its TX flag yet). A TX slice that is *closed* —
            // removed with no recreation — would instead leave the cache
            // pointing at a trx no live slice carries, mislabelling a later
            // power change with a dead index. Tell the two apart by deferring
            // past the ~340 ms settle window: a band change re-adds the slice
            // (same id) well within it, so the cache still resolves to a live
            // slice and this is a no-op; a genuine close leaves nothing carrying
            // that trx and resets the cache to the burst's historical default.
            // (A renumber that leaves another live slice at that trx also
            // no-ops; a surviving TX slice refreshes the cache in broadcastPower.)
            if (!m_trxMap.trxHasLiveSlice(m_model, m_lastTxTrx)) {
                QTimer::singleShot(500, this, [this]() {
                    if (m_model && !m_trxMap.trxHasLiveSlice(m_model, m_lastTxTrx)) {
                        m_lastTxTrx = 0;
                    }
                });
            }

            // #4567: release the removed slice's receiver binding only if
            // this is a genuine close. Same settle-window shape as the
            // m_lastTxTrx cache above: a band-change recreate re-adds the
            // same Flex slice id well within 500 ms and reclaims its number
            // via acquire() (so surviving slices never renumber); a genuine
            // close leaves the id dead and frees the number for reuse.
            // The timer also fires during teardown after m_trxMap.clear() —
            // intentional no-op: release() of an absent key does nothing and
            // the liveness guard holds.
            QTimer::singleShot(500, this, [this, sliceId]() {
                if (m_model && !m_model->slice(sliceId)) {
                    m_trxMap.release(sliceId);
                }
            });

            auto* ps = m_model ? m_model->panStream() : nullptr;
            if (!ps) return;
            for (int ch = 1; ch <= 8; ++ch) {
                if (!ps->daxChannelHeldBy(ch, PanadapterStream::DaxConsumer::Tci))
                    continue;
                bool stillWanted = false;
                for (auto* s : m_model->slices()) {
                    if (s && s->daxChannel() == ch) { stillWanted = true; break; }
                }
                if (!stillWanted) {
                    qCInfo(lcCat) << "TCI: releasing DAX channel" << ch
                                  << "after slice" << sliceId << "removal (#3305)";
                    ps->releaseDaxChannel(ch, PanadapterStream::DaxConsumer::Tci);
                    m_channelTrx.remove(ch);
                }
            }
        });

        connect(m_model, &RadioModel::panadapterRemoved, this,
                [this](const QString& panId) {
            m_lastDdsCenterHz.remove(panId);
        });

        // Panadapter recenter → dds: broadcast. The DAX IQ stream a skimmer
        // (CW Skimmer / SDC) decodes is centered on the panadapter, not the
        // slice (FlexLib: DAXIQChannel is a Panadapter property). When a pan
        // scrolls/recenters, every slice on that pan shares the new IQ center,
        // so emit dds:<trx>,<panCenterHz>; for each — mirroring the vfo:
        // broadcast in wireSlice(). Without it a skimmer's spots drift as the
        // pan moves. (#3910)
        auto wirePan = [this](PanadapterModel* pan) {
            if (!pan) {
                return;
            }
            if (pan->centerKnown()) {
                m_lastDdsCenterHz.insert(
                    pan->panId(), TciProtocol::mhzToHz(pan->centerMhz()));
            }
            connect(pan, &PanadapterModel::infoChanged, this,
                    [this, pan](double centerMhz, double /*bwMhz*/) {
                if (!m_model) {
                    return;
                }
                const long long hz = TciProtocol::mhzToHz(centerMhz);
                // infoChanged also fires on bandwidth-only (zoom) changes, so
                // gate on an actual IQ-center move. Update the gate even with
                // no clients so it cannot drift from model state (#3910,
                // #3913 review).
                if (m_lastDdsCenterHz.value(pan->panId(), -1) == hz) {
                    return;
                }
                m_lastDdsCenterHz.insert(pan->panId(), hz);
                if (m_clients.isEmpty()) {
                    return;
                }
                for (auto* s : m_model->slices()) {
                    if (s && s->panId() == pan->panId()) {
                        broadcastSliceFrequencies(s);
                    }
                }
            });
        };
        connect(m_model, &RadioModel::panadapterAdded, this, wirePan);
        for (auto* pan : m_model->panadapters()) {
            wirePan(pan);
        }
    }

    // Periodic status broadcast (200ms — S-meter, TX sensors, TX state)
    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(200);
    connect(m_meterTimer, &QTimer::timeout, this, &TciServer::broadcastStatus);

    // Rate limiter for drive:/tune_drive: — see queuePowerBroadcast().
    m_powerRateTimer = new QTimer(this);
    m_powerRateTimer->setSingleShot(true);
    m_powerRateTimer->setInterval(kPowerRateLimitMs);
    connect(m_powerRateTimer, &QTimer::timeout, this, [this]() {
        if (!m_drivePending && !m_tuneDrivePending) {
            return;  // no trailing change; let the timer lapse so the next
                     // change gets a fresh leading edge
        }
        broadcastPower();
        m_powerRateTimer->start();
    });

    // Debounced DAX RX teardown — see scheduleDaxRelease(). Single-shot; a
    // reconnecting audio client cancels it before it fires.
    m_daxReleaseTimer = new QTimer(this);
    m_daxReleaseTimer->setSingleShot(true);
    connect(m_daxReleaseTimer, &QTimer::timeout, this, [this]() {
        bool anyAudio = false;
        for (const auto& cs : m_clients)
            if (cs.audioEnabled) { anyAudio = true; break; }
        if (anyAudio) {
            qCWarning(lcCat) << "TCI: DAX release grace expired but an audio client is active — keeping DAX RX";
            return;
        }
        qCWarning(lcCat) << "TCI: DAX release grace expired, no audio client returned — releasing DAX RX now";
        releaseDaxForTci();
    });

    // TX_CHRONO timer — sends timing frames to TCI client during TX.
    // WSJT-X only sends TX audio in response to these frames.
    //
    // One TCI TX block is 2048 float samples = 1024 stereo frames at 48 kHz,
    // or 21.333 ms of audio. A fixed 21 ms timer runs ~1.6% fast and warps
    // digital-mode tones, so we poll more frequently and emit frames from a
    // monotonic elapsed-time accumulator.
    m_txChronoTimer = new QTimer(this);
    m_txChronoTimer->setTimerType(Qt::PreciseTimer);
    m_txChronoTimer->setInterval(kTxChronoPollMs);
    connect(m_txChronoTimer, &QTimer::timeout, this, [this]() {
        // Local copy guards against onClientDisconnected nulling the pointer
        // between the check and the send.
        QWebSocket* client = m_txChronoClient;
        if (!client) { m_txChronoTimer->stop(); return; }

        if (!m_txChronoClock.isValid()) {
            m_txChronoClock.start();
            return;
        }

        m_txChronoAccumNs += m_txChronoClock.nsecsElapsed();
        m_txChronoClock.restart();

        while (m_txChronoAccumNs >= kTxChronoPeriodNs) {
            sendTxChronoFrame(client);
            m_txChronoAccumNs -= kTxChronoPeriodNs;
        }
    });
}

TciServer::~TciServer()
{
    stop();
}

bool TciServer::start(quint16 port)
{
    if (m_server)
        return m_server->isListening();

    m_server = new QWebSocketServer(
        QStringLiteral("AetherSDR-TCI"),
        QWebSocketServer::NonSecureMode, this);

    if (!m_server->listen(QHostAddress::Any, port)) {
        qCWarning(lcCat) << "TciServer: failed to listen on port" << port
                         << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QWebSocketServer::newConnection,
            this, &TciServer::onNewConnection);

    m_meterTimer->start();
    qCInfo(lcCat) << "TciServer: listening on port" << m_server->serverPort();
    return true;
}

void TciServer::stop()
{
    m_meterTimer->stop();
    if (m_daxReleaseTimer) m_daxReleaseTimer->stop();  // immediate teardown below
    m_pendingTrxRequest.reset();
    m_pendingRouteCommands.clear();
    m_routeTransitionInFlight = false;
    ++m_routeTransitionGeneration;
    abortTciPtt();
    teardownTciRoute();
    stopTxChrono();

    if (!m_server) return;

    for (auto& cs : m_clients) {
        cs.socket->disconnect(this);   // prevent onClientDisconnected re-entry
        cs.socket->close();
        cs.socket->deleteLater();
        delete cs.protocol;
        qDeleteAll(cs.resamplers);
    }
    m_clients.clear();
    releaseDaxForTci();
    emit clientCountChanged(0);

    m_server->close();
    delete m_server;
    m_server = nullptr;

    qCInfo(lcCat) << "TciServer: stopped";
}

bool TciServer::isRunning() const
{
    return m_server && m_server->isListening();
}

quint16 TciServer::port() const
{
    return m_server ? m_server->serverPort() : 0;
}

void TciServer::broadcastMasterVolume(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    // Wire scale is dB (-60..0) per the TCI spec; pct is the internal
    // 0-100 amplitude from the title bar slider / applyMasterVolume.
    broadcast(QStringLiteral("volume:%1;")
                  .arg(TciProtocol::volumeDbFromPercent(pct)));
}

// Rate-limited entry point for TransmitModel's power signals (#4161).
//
// Leading edge sends immediately, so a client's own SET still echoes in a few
// ms and a band change announces the new per-band power without added latency.
// Anything arriving inside the window is collapsed: one trailing send carries
// whatever the latest value turned out to be. A power-slider drag steps ~40
// times a second (each step is its own `transmit set rfpower=` to the radio),
// and relaying every one floods clients that are often on the far side of a
// SmartLink hop.
void TciServer::queuePowerBroadcast()
{
    // The pending flag for the field that changed is set by the caller. Inside
    // the rate window we do nothing more — the trailing flush picks it up.
    if (m_powerRateTimer->isActive()) {
        return;
    }
    broadcastPower();
    m_powerRateTimer->start();
}

void TciServer::broadcastPower()
{
    if (m_clients.isEmpty() || !m_model) {
        m_drivePending = false;
        m_tuneDrivePending = false;
        // Forget what was last sent. The de-dup below means "the clients
        // already have this value", which is worthless with none attached:
        // power moves while disconnected, a reconnecting client is seeded
        // from the init burst, and a surviving cache would then suppress the
        // next genuine change back to the remembered value — dial stuck on
        // the old figure while the radio keys at the new one (#4161).
        m_lastDriveSent = -1;
        m_lastTuneDriveSent = -1;
        return;
    }
    auto& tx = m_model->transmitModel();
    // Resolve the TX trx, falling back to the last known one when a slice
    // recreation has momentarily cleared every TX flag (#4161). Refresh the
    // cache whenever a real TX slice is found.
    int trx = m_trxMap.txSliceTrxOrNone(m_model);
    if (trx < 0) {
        trx = m_lastTxTrx;
    } else {
        m_lastTxTrx = trx;
    }

    // Only the field that actually changed is sent — sending drive must not
    // drag tune_drive onto the wire (and vice versa). Value de-dup still
    // guards a change that lands back on the last-sent value inside a window.
    if (m_drivePending) {
        m_drivePending = false;
        if (tx.rfPower() != m_lastDriveSent) {
            m_lastDriveSent = tx.rfPower();
            broadcast(QStringLiteral("drive:%1,%2;").arg(trx).arg(m_lastDriveSent));
        }
    }
    if (m_tuneDrivePending) {
        m_tuneDrivePending = false;
        if (tx.tunePower() != m_lastTuneDriveSent) {
            m_lastTuneDriveSent = tx.tunePower();
            broadcast(QStringLiteral("tune_drive:%1,%2;")
                          .arg(trx).arg(m_lastTuneDriveSent));
        }
    }
}

// Recompute the focused TRX and tell clients if it moved (#4160).
//
// Called both when focus changes and when a slice is removed. The removal
// case is the non-obvious one: trx is a positional index, so removing a
// slice renumbers every later slice, but the focused slice itself emits
// nothing — it never lost focus. Without this the tracked trx (and every
// client seeded from it) silently points at the wrong slice.
//
// Unlike vfo:/modulation:, active_slice has no follow-up event that would
// self-correct: once only one slice remains the operator cannot switch
// focus at all, so a stale value would persist indefinitely.
//
// Runs even with no clients connected — focus and slice count both change
// freely before anyone connects, and m_activeTrx seeds each new client's
// init burst.
void TciServer::publishActiveTrx()
{
    int trx = -1;
    QString letter;
    // Resolved from the remembered slice rather than a scan: during a focus
    // switch SliceModel::setActive() sets the incoming slice optimistically
    // while the outgoing one keeps its flag until the radio echoes active=0,
    // so a scan can transiently see two active slices (#3854 review).
    if (m_activeSlice && m_model && m_model->slices().contains(m_activeSlice)) {
        trx = m_trxMap.trxForSlice(m_model, m_activeSlice);
        letter = TciProtocol::sanitizeSliceLetter(m_activeSlice->letter());
    }

    // Letter is part of the dedupe: the radio can relabel a slice without
    // focus moving (MultiFlex reassignment, #2606), and a controller showing
    // "Slice A" must not keep showing it after the radio calls it B.
    if (trx == m_activeTrx && letter == m_activeLetter) return;
    m_activeTrx = trx;
    m_activeLetter = letter;
    for (auto& c : m_clients) {
        if (c.protocol) c.protocol->setActiveSlice(trx, letter);
    }
    // trx < 0 means the focused slice is gone and nothing has claimed focus
    // yet; stay silent rather than announce a slice that does not exist. The
    // radio's next activeChanged brings us back.
    if (trx >= 0 && !m_clients.isEmpty())
        broadcast(QStringLiteral("active_slice:%1,%2;").arg(trx).arg(letter));
}

void TciServer::setTxGain(float gain)
{
    const float clamped = std::clamp(gain, 0.0f, 1.0f);
    if (m_txGain == clamped) return;
    m_txGain = clamped;
    auto& s = AppSettings::instance();
    s.setValue("TciTxGain", QString::number(clamped, 'f', 2));
    s.save();
}

void TciServer::setOverflowMode(int mode)
{
    if (mode < 0 || mode > 2) return;
    auto next = static_cast<OverflowMode>(mode);
    if (m_overflowMode == next) return;
    m_overflowMode = next;
    auto& s = AppSettings::instance();
    s.setValue("TciTxOverflowMode", QString::number(mode));
    s.save();
}

void TciServer::setRxChannelGain(int channel, float gain)
{
    if (channel < 1 || channel > 8) return;
    const float clamped = std::clamp(gain, 0.0f, 1.0f);
    if (m_rxChannelGain[channel - 1] == clamped) return;
    m_rxChannelGain[channel - 1] = clamped;
    auto& s = AppSettings::instance();
    s.setValue(QStringLiteral("TciRxGain%1").arg(channel),
               QString::number(clamped, 'f', 2));
    s.save();
}

float TciServer::rxChannelGain(int channel) const
{
    if (channel < 1 || channel > 8) return 1.0f;
    return m_rxChannelGain[channel - 1];
}

void TciServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        auto* ws = m_server->nextPendingConnection();

        // Refuse new connections once at-capacity (GHSA-7w4w-wfqm-wh93).
        if (m_clients.size() >= kMaxClients) {
            qCWarning(lcCat) << "TciServer: refusing connection from"
                             << ws->peerAddress().toString()
                             << "— at max-clients cap (" << kMaxClients << ")";
            ws->close(QWebSocketProtocol::CloseCodeTooMuchData,
                      QStringLiteral("server at max-clients cap"));
            ws->deleteLater();
            continue;
        }

        // Cap per-message and per-frame size to refuse OOM-by-huge-frame
        // (GHSA-7w4w-wfqm-wh93).  Qt6 default is 1 GiB per message; legit
        // TCI text commands and audio frames are well under 64 KiB.
        ws->setMaxAllowedIncomingMessageSize(kMaxWsMessageBytes);
        ws->setMaxAllowedIncomingFrameSize(kMaxWsMessageBytes);

        auto* protocol = new TciProtocol(m_model, &m_routingState, &m_trxMap);
        // Seed GUI focus so this client's init burst and any `active_slice`
        // GET report the current slice, not a stale scan (#4160). Stays -1
        // if no focus change has been observed yet, in which case the
        // protocol falls back to scanning.
        protocol->setActiveSlice(m_activeTrx, m_activeLetter);

        ClientState cs;
        cs.socket = ws;
        cs.protocol = protocol;
        // Resamplers are created lazily per-channel in onDaxAudioReady()
        // so each DAX channel has its own stateful r8brain instance (#1806).
        m_clients.append(cs);

        connect(ws, &QWebSocket::textMessageReceived,
                this, &TciServer::onTextMessage);
        connect(ws, &QWebSocket::binaryMessageReceived,
                this, &TciServer::onBinaryMessage);
        connect(ws, &QWebSocket::disconnected,
                this, &TciServer::onClientDisconnected);

        qCInfo(lcCat) << "TciServer: client connected from"
                      << ws->peerAddress().toString();
        emit clientCountChanged(m_clients.size());
        emit clientsChanged();

        sendInitBurst(ws);
    }
}

void TciServer::onClientDisconnected()
{
    auto* ws = qobject_cast<QWebSocket*>(sender());
    if (!ws) return;

    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].socket == ws) {
            if (m_pendingTrxRequest && m_pendingTrxRequest->client == ws) {
                m_pendingTrxRequest.reset();
            }
            for (int pendingIndex = m_pendingRouteCommands.size() - 1;
                 pendingIndex >= 0; --pendingIndex) {
                if (m_pendingRouteCommands[pendingIndex].client == ws) {
                    m_pendingRouteCommands.removeAt(pendingIndex);
                }
            }
            // If this client owned TCI PTT/TX audio, fail closed.
            if (ws == m_tciPttClient || ws == m_txChronoClient) {
                abortTciPtt();
            }
            // Clean up IQ stream if this client started one
            if (m_clients[i].iqEnabled && m_model) {
                int ch = m_clients[i].iqChannel + 1;  // TRX 0 → DAX channel 1
                // Only remove if no other client uses the same IQ channel
                bool otherUsing = false;
                for (int j = 0; j < m_clients.size(); ++j) {
                    if (j != i && m_clients[j].iqEnabled &&
                        m_clients[j].iqChannel == m_clients[i].iqChannel) {
                        otherUsing = true;
                        break;
                    }
                }
                if (!otherUsing) {
                    QMetaObject::invokeMethod(m_model, [this, ch]() {
                        m_model->daxIqModel().removeStream(ch);
                    }, Qt::QueuedConnection);
                }
            }
            delete m_clients[i].protocol;
            qDeleteAll(m_clients[i].resamplers);
            m_clients.removeAt(i);

            // Release DAX if no remaining clients want audio (#1331)
            bool anyAudio = false;
            for (const auto& cs : m_clients) {
                if (cs.audioEnabled) { anyAudio = true; break; }
            }
            if (!anyAudio) scheduleDaxRelease();  // debounce: survive transient WSJT-X reconnects
            break;
        }
    }

    ws->deleteLater();
    // DIAG: qCWarning — a TCP-level client drop (WSJT-X threw a rig-control
    // error in do_stop()) is the trigger for the DAX RX teardown above. Always
    // log it so the cause of mid-session RX loss is visible.
    qCWarning(lcCat) << "TciServer: client disconnected (TCP drop),"
                     << m_clients.size() << "remaining";
    emit clientCountChanged(m_clients.size());
    emit clientsChanged();
    if (m_clients.isEmpty()) {
        m_pendingTrxRequest.reset();
        m_pendingRouteCommands.clear();
        m_routeTransitionInFlight = false;
        ++m_routeTransitionGeneration;
        teardownTciRoute();
    } else {
        drainDeferredRoutingAndPtt();
    }
}

QVector<TciClientInfo> TciServer::connectedClients() const
{
    QVector<TciClientInfo> out;
    out.reserve(m_clients.size());
    for (const auto& cs : m_clients) {
        if (!cs.socket)
            continue;
        TciClientInfo info;
        // Normalise the peer address so it is both readable and a STABLE
        // alias key: collapse IPv4-mapped IPv6 (::ffff:a.b.c.d) to plain
        // IPv4, and IPv6 loopback (::1) to 127.0.0.1. Otherwise the same
        // physical client could key its saved Name under two spellings.
        QHostAddress ha = cs.socket->peerAddress();
        bool isV4 = false;
        const quint32 v4 = ha.toIPv4Address(&isV4);
        if (isV4)
            ha = QHostAddress(v4);
        else if (ha.isLoopback())
            ha = QHostAddress(QHostAddress::LocalHost);
        info.peerAddress  = ha.toString();
        info.peerPort     = cs.socket->peerPort();
        info.audio        = cs.audioEnabled;
        info.audioReceiver= cs.audioReceiver;
        info.iq           = cs.iqEnabled;
        info.rxSensors    = cs.rxSensorsEnabled;
        info.txSensors    = cs.txSensorsEnabled;
        out.append(info);
    }
    return out;
}

QJsonObject TciServer::routingSnapshot() const
{
    const auto ownerName = [this]() {
        switch (m_routingState.owner()) {
        case TciRoutingState::TxRouteOwner::External:
            return QStringLiteral("external");
        case TciRoutingState::TxRouteOwner::TciCreated:
            return QStringLiteral("tci-created");
        case TciRoutingState::TxRouteOwner::None:
            return QStringLiteral("none");
        }
        return QStringLiteral("none");
    };

    QJsonArray endpoints;
    if (m_model) {
        const auto slices = m_model->slices();
        for (int trx = 0; trx < slices.size(); ++trx) {
            const SliceModel* slice = slices.at(trx);
            if (!slice) {
                continue;
            }
            endpoints.append(QJsonObject{
                {QStringLiteral("trx"), trx},
                {QStringLiteral("sliceId"), slice->sliceId()},
                {QStringLiteral("panId"), slice->panId()},
                {QStringLiteral("frequencyHz"),
                    static_cast<qint64>(TciProtocol::mhzToHz(slice->frequency()))},
                {QStringLiteral("tx"), slice->isTxSlice()},
            });
        }
    }

    QJsonArray pendingRoutes;
    for (const PendingRouteCommand& pending : m_pendingRouteCommands) {
        QJsonObject item{
            {QStringLiteral("clientConnected"), !pending.client.isNull()},
            {QStringLiteral("kind"),
                pending.kind == PendingRouteCommand::Kind::Vfo
                    ? QStringLiteral("vfo")
                    : QStringLiteral("split")},
        };
        if (pending.kind == PendingRouteCommand::Kind::Vfo) {
            item[QStringLiteral("trx")] = pending.vfo.trx;
            item[QStringLiteral("channel")] = pending.vfo.channel;
            item[QStringLiteral("frequencyHz")]
                = static_cast<qint64>(pending.vfo.frequencyHz);
        } else {
            item[QStringLiteral("trx")] = pending.split.trx;
            item[QStringLiteral("enabled")] = pending.split.enabled;
        }
        pendingRoutes.append(item);
    }

    QJsonObject ptt{
        {QStringLiteral("owned"), !m_tciPttClient.isNull()},
        {QStringLiteral("trx"), m_tciPttTrx},
        {QStringLiteral("wantsAudio"), m_tciPttWantsAudio},
        {QStringLiteral("requestedOn"), m_tciPttRequestedOn},
        {QStringLiteral("confirmedOn"), m_tciPttConfirmedOn},
        {QStringLiteral("cancelPending"), m_tciPttCancelPending},
        {QStringLiteral("generation"), static_cast<qint64>(m_tciPttGeneration)},
    };

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("contractVersion"), 1},
        {QStringLiteral("serverRunning"), isRunning()},
        {QStringLiteral("port"), static_cast<int>(port())},
        {QStringLiteral("clientCount"), m_clients.size()},
        {QStringLiteral("radioConnected"), m_model && m_model->isConnected()},
        {QStringLiteral("radioTransmitting"), m_model && m_model->isRadioTransmitting()},
        {QStringLiteral("splitRequested"), m_routingState.splitRequested()},
        {QStringLiteral("rxSliceId"), m_routingState.rxSliceId()},
        {QStringLiteral("txSliceId"), m_routingState.txSliceId()},
        {QStringLiteral("routeOwner"), ownerName()},
        {QStringLiteral("ownsRoute"), m_routingState.ownsRoute()},
        {QStringLiteral("routeTransitionInFlight"), m_routeTransitionInFlight},
        {QStringLiteral("routeTransitionGeneration"),
            static_cast<qint64>(m_routeTransitionGeneration)},
        {QStringLiteral("pendingVfoBCreate"), m_pendingVfoBCreate.has_value()},
        {QStringLiteral("pendingTrx"), m_pendingTrxRequest.has_value()},
        {QStringLiteral("pendingRoutes"), pendingRoutes},
        {QStringLiteral("lastRouteError"), m_lastRouteError},
        {QStringLiteral("ptt"), ptt},
        {QStringLiteral("endpoints"), endpoints},
    };
}

void TciServer::onTextMessage(const QString& msg)
{
    auto* ws = qobject_cast<QWebSocket*>(sender());
    if (!ws) return;

    // Find the client state
    int clientIdx = -1;
    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].socket == ws) { clientIdx = i; break; }
    }
    if (clientIdx < 0) return;

    auto& client = m_clients[clientIdx];

    // Raw inbound log — helps diagnose TCI-variant dialects where WSJT-X
    // forks (Improved, Improved Plus, KN4CRD fork…) send commands our
    // parser doesn't match.  Truncate long ones to keep logs readable.
    qCDebug(lcCat) << "TCI rx:" << msg.left(256);
    emit tciMessage(QStringLiteral("rx"), msg);

    // TCI messages are semicolon-terminated; may contain multiple commands
    const QStringList cmds = msg.split(';', Qt::SkipEmptyParts);
    for (const auto& cmd : cmds) {
        QString trimmed = cmd.trimmed().toLower();

        // Handle audio start/stop at server level (affects per-client state)
        if (trimmed.startsWith("audio_start")) {
            int requestedReceiver = -1;
            const int colonIdx2 = trimmed.indexOf(':');
            if (colonIdx2 >= 0) {
                const QString receiverText = trimmed.mid(colonIdx2 + 1)
                                                 .section(QLatin1Char(','), 0, 0)
                                                 .trimmed();
                bool ok = false;
                const int parsedReceiver = receiverText.toInt(&ok);
                if (ok)
                    requestedReceiver = parsedReceiver;
            }
            client.audioEnabled = true;
            client.audioReceiver = requestedReceiver;
            cancelDaxRelease();  // a (re)connecting audio client cancels a pending teardown
            ensureDaxForTci();
            replyText(ws,cmd.trimmed() + ";");
            qCDebug(lcCat) << "TCI: audio started"
                           << "receiver=" << client.audioReceiver
                           << "rate=" << client.audioSampleRate
                           << "channels=" << client.audioChannels
                           << "format=" << client.audioFormat
                           << "peer=" << ws->peerAddress().toString();
            qCInfo(lcCat) << "TCI: audio started for client"
                          << ws->peerAddress().toString()
                          << "rate=" << client.audioSampleRate
                          << "ch=" << client.audioChannels
                          << "fmt=" << client.audioFormat;
            emit clientsChanged();
            continue;
        }
        if (trimmed.startsWith("audio_stop")) {
            client.audioEnabled = false;
            client.audioReceiver = -1;
            // Release DAX if no other clients still want audio
            bool anyAudio = false;
            for (const auto& cs : m_clients) {
                if (cs.audioEnabled) { anyAudio = true; break; }
            }
            if (!anyAudio) scheduleDaxRelease();  // debounce: audio_stop is often followed by a quick audio_start
            replyText(ws,cmd.trimmed() + ";");
            qCWarning(lcCat) << "TCI: audio_stop from client"
                             << ws->peerAddress().toString()
                             << "(anyAudio=" << anyAudio << ")";
            emit clientsChanged();
            continue;
        }

        // Audio format negotiation
        if (trimmed.startsWith("audio_samplerate:")) {
            int colonIdx2 = trimmed.indexOf(':');
            int rate = trimmed.mid(colonIdx2 + 1).toInt();
            if (rate == 8000 || rate == 12000 || rate == 24000 || rate == 48000) {
                client.audioSampleRate = rate;
                // Discard all per-channel resamplers — they were built for
                // the old rate and carry stale filter history.  New instances
                // at the correct rate are lazily created in onDaxAudioReady().
                qDeleteAll(client.resamplers);
                client.resamplers.clear();
                qCInfo(lcCat) << "TCI: audio sample rate set to" << rate
                              << "for" << ws->peerAddress().toString();
            }
            replyText(ws,QStringLiteral("audio_samplerate:%1;")
                                    .arg(client.audioSampleRate));
            continue;
        }
        if (trimmed.startsWith("audio_stream_sample_type:")) {
            int colonIdx2 = trimmed.indexOf(':');
            QString fmtStr = trimmed.mid(colonIdx2 + 1).trimmed();
            int fmt;
            if (fmtStr == "float32")
                fmt = 3;
            else if (fmtStr == "int16")
                fmt = 0;
            else
                fmt = fmtStr.toInt();  // numeric value
            if (fmt == 0 || fmt == 3)  // int16 or float32
                client.audioFormat = fmt;
            replyText(ws,QStringLiteral("audio_stream_sample_type:%1;")
                                    .arg(client.audioFormat));
            continue;
        }
        // Sensor enable/disable
        if (trimmed.startsWith("rx_sensors_enable:")) {
            int colonIdx2 = trimmed.indexOf(':');
            QString val = trimmed.mid(colonIdx2 + 1).split(',').first();
            client.rxSensorsEnabled = (val == "true");
            replyText(ws,QStringLiteral("rx_sensors_enable:%1;")
                                    .arg(client.rxSensorsEnabled ? "true" : "false"));
            qCInfo(lcCat) << "TCI: rx_sensors" << (client.rxSensorsEnabled ? "enabled" : "disabled");
            emit clientsChanged();
            continue;
        }
        if (trimmed.startsWith("tx_sensors_enable:")) {
            int colonIdx2 = trimmed.indexOf(':');
            QString val = trimmed.mid(colonIdx2 + 1).split(',').first();
            client.txSensorsEnabled = (val == "true");
            replyText(ws,QStringLiteral("tx_sensors_enable:%1;")
                                    .arg(client.txSensorsEnabled ? "true" : "false"));
            qCInfo(lcCat) << "TCI: tx_sensors" << (client.txSensorsEnabled ? "enabled" : "disabled");
            emit clientsChanged();
            continue;
        }

        // IQ start/stop — track per-client IQ state, then forward to protocol.
        //
        // The trx is parsed with the ok flag checked for the same reason
        // TciProtocol's argToInt exists (#4867): an unchecked toInt() turns
        // `iq_start:abc;` into trx 0. That used to be merely wrong in the same
        // way on both sides — the protocol also resolved it to 0 and created
        // DAX IQ channel 1, so server bookkeeping and radio state agreed. Now
        // that cmdIqStart rejects it, an unchecked parse here would leave the
        // two DISAGREEING: this client registered as a subscriber on trx 0
        // with no stream ever created for it, receiving another client's
        // channel-1 IQ frames (onIqDataReady) and eligible to tear that
        // client's stream down on disconnect. Drop the command instead, which
        // is also what the protocol does with it half a dozen lines later.
        if (trimmed.startsWith("iq_start:")) {
            const int colonIdx2 = trimmed.indexOf(':');
            bool trxOk = false;
            const int trx = trimmed.mid(colonIdx2 + 1).trimmed().toInt(&trxOk);
            if (!trxOk || trx < 0 || trx > 3)
                continue;
            client.iqEnabled = true;
            client.iqChannel = trx;
            qCInfo(lcCat) << "TCI: IQ started for client"
                          << ws->peerAddress().toString()
                          << "trx=" << trx;
            // Forward to protocol to create DAX IQ stream on the radio
            QString response = client.protocol->handleCommand(cmd.trimmed());
            if (!response.isEmpty())
                replyText(ws,response);
            emit clientsChanged();
            continue;
        }
        if (trimmed.startsWith("iq_stop:")) {
            // Checked for the same reason as iq_start above: an unchecked
            // parse would clear iqEnabled for a client whose channel really
            // is 0 whenever any garbage arrived, while the protocol declined
            // to remove the stream.
            const int colonIdx2 = trimmed.indexOf(':');
            bool trxOk = false;
            const int trx = trimmed.mid(colonIdx2 + 1).trimmed().toInt(&trxOk);
            if (!trxOk || trx < 0 || trx > 3)
                continue;
            if (client.iqChannel == trx)
                client.iqEnabled = false;
            qCInfo(lcCat) << "TCI: IQ stopped for client"
                          << ws->peerAddress().toString()
                          << "trx=" << trx;
            QString response = client.protocol->handleCommand(cmd.trimmed());
            if (!response.isEmpty())
                replyText(ws,response);
            emit clientsChanged();
            continue;
        }

        // Spectrum event subscribe/unsubscribe — enables waterfall row forwarding
        if (trimmed == "spectrum_event:on") {
            client.spectrumEnabled = true;
            qCInfo(lcCat) << "TCI: spectrum_event enabled for client"
                          << ws->peerAddress().toString();
            continue;
        }
        if (trimmed == "spectrum_event:off") {
            client.spectrumEnabled = false;
            qCInfo(lcCat) << "TCI: spectrum_event disabled for client"
                          << ws->peerAddress().toString();
            continue;
        }

        if (trimmed.startsWith("audio_stream_samples:")) {
            // Samples per audio packet — acknowledge but we use fixed packet sizes
            replyText(ws,cmd.trimmed() + ";");
            continue;
        }
        if (trimmed.startsWith("tx_stream_audio_buffering:")) {
            // TX audio buffering in ms — acknowledge
            replyText(ws,cmd.trimmed() + ";");
            continue;
        }
        if (trimmed.startsWith("line_out_start") ||
            trimmed.startsWith("line_out_stop") ||
            trimmed.startsWith("line_out_recorder")) {
            // Line-out recording — not applicable to FlexRadio, acknowledge
            replyText(ws,cmd.trimmed() + ";");
            continue;
        }
        if (trimmed.startsWith("audio_stream_channels:")) {
            int colonIdx2 = trimmed.indexOf(':');
            int ch = trimmed.mid(colonIdx2 + 1).toInt();
            if (ch == 1 || ch == 2)
                client.audioChannels = ch;
            replyText(ws,QStringLiteral("audio_stream_channels:%1;")
                                    .arg(client.audioChannels));
            continue;
        }

        QString response = client.protocol->handleCommand(cmd.trimmed());
        if (!response.isEmpty()) {
            replyText(ws,response);
            qCDebug(lcCat) << "TCI cmd:" << cmd.trimmed()
                           << "-> resp:" << response.left(80).trimmed();
        }

        if (const auto request = client.protocol->takeVfoRequest()) {
            handleVfoRequest(ws, *request);
        }
        if (const auto request = client.protocol->takeSplitRequest()) {
            handleSplitRequest(ws, *request);
        }
        if (const auto request = client.protocol->takeTrxRequest()) {
            handleTrxRequest(ws, *request);
        }

        // If the command changed radio state, broadcast to all other clients
        QString notification = client.protocol->pendingNotification();
        if (!notification.isEmpty()) {
            for (auto& cs : m_clients) {
                if (cs.socket != ws)
                    cs.socket->sendTextMessage(notification);
            }
        }

        // Master volume SET — TciProtocol owns only RadioModel, so it can't
        // touch AudioEngine directly. It stashes the requested level and we
        // forward to MainWindow via signal, mirroring the title bar slider's
        // signal path. The notification (`volume:N;`) was already echoed
        // above to the requesting client and broadcast to others.
        int mvol = client.protocol->pendingMasterVolume();
        if (mvol >= 0) emit masterVolumeRequested(mvol);

        // tx_gain SET — same pattern: TciProtocol can't reach TciServer, so it
        // stashes the 0-100 value and we apply it here via setTxGain().
        int txg = client.protocol->pendingTxGain();
        if (txg >= 0) setTxGain(txg / 100.0f);
    }
}

SliceModel* TciServer::sliceForTrx(int trx) const
{
    return m_trxMap.sliceForTrx(m_model, trx);
}

SliceModel* TciServer::sliceForTrxStrict(int trx) const
{
    // Goes through the SAME trx map as sliceForTrx() above (#4567), not
    // TciProtocol's positional statics. If PTT resolved positionally while
    // every other command followed the stable binding, a band-stack recreate
    // would key whichever slice happened to sit at the requested index — on
    // the one path where being wrong puts RF on the wrong band and antenna.
    return m_trxMap.sliceForTrxStrict(m_model, trx);
}

int TciServer::effectiveTrx(QWebSocket* client, int requestedTrx) const
{
    // Every WSJT-X instance in TCI/ESDR3 mode addresses trx 0, so with two
    // instances on two slices the wire request carries nothing that tells them
    // apart and both resolve to the same receiver (#4547). The one per-client
    // signal that does exist is the receiver declared in `audio_start:<n>` —
    // already parsed and stored per socket — so an instance that started audio
    // on receiver 1 is operating receiver 1 whatever index it puts on the wire.
    //
    // Thetis scopes RX-audio enabled-receiver sets per client while radio state
    // stays global, so reading the declared receiver as the client's identity
    // follows the reference implementation. A client that declares no receiver
    // (`audio_start` with no argument, or control-only) keeps the wire index.
    // Replies still echo the trx the client sent — the binding changes which
    // slice is addressed, never the wire shape.
    //
    // ONLY trx 0 is redirected. Thetis keeps radio state global and scopes only
    // the audio set per client, so the declared receiver is evidence of intent,
    // not an address that outranks one. It is good evidence exactly where the
    // wire has none: every WSJT-X instance addresses trx 0 whatever receiver it
    // operates, so trx 0 carries no client intent to override. A non-zero trx is
    // a deliberate address — a client that declared audio on receiver 0 and then
    // asks for trx 1 means trx 1, and honouring the declaration there would key
    // a slice the client never asked for, on that slice's band and antenna.
    // That is the #4547 defect class, re-entered through its own fix.
    for (const auto& cs : m_clients) {
        if (cs.socket == client) {
            if (cs.audioReceiver < 0 || requestedTrx != 0) {
                return requestedTrx;
            }
            // Log only the divergence. Agreement is the common case and would
            // be one line per key; a redirect is the whole mechanism, and it is
            // otherwise invisible — the reply still echoes the requested trx, so
            // a session transcript cannot show which slice was really addressed.
            if (cs.audioReceiver != requestedTrx) {
                qCDebug(lcCat) << "TCI: PTT bound to declared receiver"
                               << cs.audioReceiver << "over requested trx"
                               << requestedTrx
                               << "peer=" << (cs.socket
                                     ? cs.socket->peerAddress().toString()
                                     : QStringLiteral("<gone>"));
            }
            return cs.audioReceiver;
        }
    }
    return requestedTrx;
}

const char* TciServer::txRouteOwnerName(TciRoutingState::TxRouteOwner owner)
{
    switch (owner) {
    case TciRoutingState::TxRouteOwner::None:
        return "none";
    case TciRoutingState::TxRouteOwner::External:
        return "external";
    case TciRoutingState::TxRouteOwner::TciCreated:
        return "tci-created";
    }
    return "unknown";
}

QString TciServer::sliceTag(int sliceId) const
{
    // Everything a TCI client sees on the wire is a receiver number: trx:,
    // tx_enable:, lock: and rx_filter_band: all carry m_trxMap.trxForSlice().
    // The routing state speaks raw Flex slice ids instead, and since #4567
    // pinned receiver numbers across a slice recreate the two genuinely
    // diverge. A diagnostic that prints one and is read against the other
    // manufactures agreements and disagreements that are not there, so print
    // both wherever a slice id appears.
    if (sliceId < 0) {
        return QStringLiteral("none");
    }
    SliceModel* slice = m_model ? m_model->slice(sliceId) : nullptr;
    if (!slice) {
        // A cached route can outlive its slice; that is a finding, not a gap.
        return QStringLiteral("%1(gone)").arg(sliceId);
    }
    return QStringLiteral("%1(trx%2)").arg(sliceId).arg(m_trxMap.trxForSlice(m_model, slice));
}

QVector<TciSliceEndpoint> TciServer::routingEndpoints() const
{
    QVector<TciSliceEndpoint> endpoints;
    if (!m_model) {
        return endpoints;
    }
    const QList<SliceModel*> slices = m_model->slices();
    endpoints.reserve(slices.size());
    for (SliceModel* slice : slices) {
        if (slice) {
            endpoints.append({ slice->sliceId(), slice->isTxSlice() });
        }
    }
    return endpoints;
}

void TciServer::tuneSliceAndConfirm(
    QWebSocket* client, int trx, int channel, int sliceId, long long frequencyHz)
{
    if (!client || !m_model || frequencyHz <= 0) {
        return;
    }
    SliceModel* slice = m_model->slice(sliceId);
    if (!slice) {
        return;
    }

    const double mhz = static_cast<double>(frequencyHz) / 1.0e6;
    // The in-span test is shared with the CAT/rigctld planes (#4497):
    // PanadapterModel::spanContainsMhz is the single definition, so the three
    // command planes cannot drift apart on what "in span" means. This used to be
    // open-coded here and in RigctlProtocol; the copies were identical until the
    // predicate grew a centerKnown term, which is exactly the drift the shared
    // definition prevents. Behaviour change from the dedupe: before the radio has
    // reported a real centre, PanadapterModel::centerMhz is a placeholder, so a
    // TCI retune in that window now recenters instead of trusting it — the safe
    // direction, and it establishes the centre.
    bool inSpan = false;
    if (const PanadapterModel* pan = m_model->panadapter(slice->panId())) {
        inSpan = pan->spanContainsMhz(mhz);
    }

    // TUNE THROUGH THE MODEL, ON EVERY COMMAND PLANE (#4500, #4493).
    //
    // This was briefly a raw `slice tune` written at the connection, with the
    // radio's command reply used as a barrier to read the settled frequency back
    // out of SliceModel. Both halves of that were wrong on a Flex:
    //
    //   - the raw command bypasses SliceModel, so nothing updates m_frequency;
    //   - the radio does not answer `slice tune` with an RF_frequency status
    //     either (measured: 89 tunes, 0 frequency statuses in one session).
    //
    // So the read-back could only ever observe the PRE-TUNE value, and every
    // confirmation echoed the frequency the slice had already left. WSJT-X's
    // do_frequency() waits on that echo, concluded the radio had not moved, and
    // reported rig-control failure on every band change — while the radio was in
    // fact sitting on the new band, which is how transmissions went out of band.
    //
    // The setter is the command path, not an addition to it: setFrequency()
    // sends the byte-identical "slice tune <id> <mhz> autopan=0" this used to
    // open-code, and additionally updates the model, honours a locked slice, and
    // emits frequencyChanged. There is no second command and no double-tune.
    //
    // The model is the authority here BECAUSE the radio declines to be: with no
    // status to wait for, an optimistic update is the only thing that can make a
    // TCI client's mirror converge. (That the radio never confirms a tune is an
    // older gap than the regression above and wants its own fix; until then this
    // is what masks it, which is exactly why removing it broke so much.)
    if (inSpan)
        slice->setFrequency(mhz);
    else
        slice->tuneAndRecenter(mhz);

    // Confirm what the model ACCEPTED, never what the client asked for.
    //
    // A locked slice refuses the tune outright and a no-op leaves the frequency
    // where it was; in both cases the honest answer is the value the model
    // holds, or a client's mirror drifts away from the radio.
    //
    // Sent unconditionally even though a successful tune also reaches clients
    // via frequencyChanged → broadcastSliceFrequencies(). That duplicates the
    // channel-0 vfo: frame, which is idempotent and harmless — whereas the
    // alternative failure, a client left with NO confirmation, hangs WSJT-X for
    // its full rig-control timeout. Channel 1 is not covered by that automatic
    // path at all unless the routing state happens to be bound, so suppressing
    // this would make split confirmations depend on unrelated state.
    const long long acceptedHz = TciProtocol::mhzToHz(slice->frequency());
    if (acceptedHz > 0) {
        broadcast(QStringLiteral("vfo:%1,%2,%3;").arg(trx).arg(channel).arg(acceptedHz));
    }
}

void TciServer::promoteTxSliceAndContinue(int sliceId, std::function<void(bool)> continuation)
{
    if (!m_model) {
        continuation(false);
        return;
    }
    SliceModel* slice = m_model->slice(sliceId);
    if (!slice || !m_model->panTransmitInhibitReason(slice->panId()).isEmpty()) {
        continuation(false);
        return;
    }
    if (slice->isTxSlice()) {
        continuation(true);
        return;
    }

    // Seam backend (HL2): `slice set N tx=1` is Flex text with no counterpart on
    // this plane, so the sendCmdPublic below would be swallowed AND this
    // continuation would never run. Every caller opens a route transition around
    // it, so a silent drop leaks m_routeTransitionInFlight forever and wedges
    // TCI keying for the rest of the connection.
    //
    // There IS a seam verb now. This used to refuse outright, correctly, because
    // such a radio had exactly one slice and it was already the transmitter —
    // so the only way to reach here was a route that could not be built. With
    // several receivers the request is meaningful: it moves the transmitter.
    //
    // Synchronous, unlike the Flex round trip below: the backend either owns the
    // move or it does not, and there is no radio to wait for. The continuation
    // is invoked either way, which is what keeps the route transition closed.
    if (!m_model->usesFlexCommandPlane()) {
        SliceModel* target = m_model->slice(sliceId);
        if (!target) {
            qCWarning(lcCat) << "TCI: TX-slice selection — no such slice" << sliceId;
            continuation(false);
            return;
        }
        target->setTxSlice(true);
        // Confirm against the MODEL rather than assuming the request took. The
        // backend republishes both the old and the new slice as part of the
        // move, so by here txSlice() is the answer the radio actually gave.
        const bool moved = target->isTxSlice();
        if (!moved) {
            qCWarning(lcCat) << "TCI: TX-slice selection refused by the backend"
                             << "slice=" << sliceId;
        }
        continuation(moved);
        return;
    }

    QPointer<TciServer> self(this);
    m_model->sendCmdPublic(QStringLiteral("slice set %1 tx=1").arg(sliceId),
        [self, sliceId, continuation = std::move(continuation)](
            int code, const QString& body) mutable {
            if (!self) {
                return;
            }
            if (code != 0) {
                qCWarning(lcCat) << "TCI: TX-slice selection rejected"
                                 << "slice=" << sliceId << "code=" << Qt::hex << code
                                 << "body=" << body;
                continuation(false);
                return;
            }
            continuation(true);
        });
}

void TciServer::createTxSliceForVfoB(QWebSocket* client,
    const TciProtocol::VfoRequest& request,
    SliceModel* rxSlice,
    const QString& routeConfirmation,
    bool splitOnly)
{
    if (!client || !m_model || !rxSlice) {
        return;
    }

    // Seam backend (HL2): `slice create` is Flex text this radio does not speak.
    // The command below would be swallowed and its completion callback would
    // never run, so the route transition opened just after it could never be
    // closed -- and handleTrxRequest() defers every subsequent trx:true into
    // m_pendingTrxRequest while a transition is in flight, so WSJT-X could not
    // transmit again for the rest of the connection. That is the failure this
    // guard exists to prevent, and the capacity test below does NOT catch it:
    // maxSlices() is the model-string-derived Flex estimate (2 by default), not
    // the backend's own maxSlices, so a single-slice HL2 looks like it has room.
    //
    // Refusing is also the honest answer, not merely the safe one: WSJT-X's
    // "Split = Rig/Fake It" reaches exactly here, and reportVfoBRouteFailure
    // sends split_enable:...,false; plus the authoritative channel-1 VFO, which
    // is what makes it fall back to single-VFO operation instead of waiting.
    // Seam backend (HL2): `slice create` is Flex text this radio does not speak,
    // and this used to refuse outright — correctly, while such a radio had one
    // receiver and could not make a second.
    //
    // It can now. createPanadapter() brings up another DDC together with its
    // slice, which is exactly what VFO B needs, so split becomes available up to
    // whatever the board and the link budget allow.
    //
    // The shape is different enough from the Flex path below to be written out
    // rather than shared: the seam create is SYNCHRONOUS — the backend either
    // owns the request or it does not, and there is no radio to wait for — so
    // there is no reply to parse, no window in which the requester can leave,
    // and no pending-create record to reconcile. What IS shared is the
    // discipline: one route transition, closed on every exit, and teardown of a
    // slice that gets created but cannot be used.
    if (!m_model->usesFlexCommandPlane()) {
        if (m_model->slices().size() >= m_model->maxSlices()) {
            reportVfoBRouteFailure(client, request,
                QStringLiteral("cannot create VFO B: receiver capacity reached"),
                !routeConfirmation.isEmpty());
            return;
        }

        // Which slice is new is found by DIFFING, not by predicting an id. The
        // backend numbers its slices and is free to skip a retired number after
        // a close, so guessing "the next one" would address the wrong receiver.
        QSet<int> before;
        for (SliceModel* s : m_model->slices())
            if (s) before.insert(s->sliceId());

        const quint64 transitionGeneration = beginRouteTransition();
        m_model->createPanadapter();

        int createdId = -1;
        for (SliceModel* s : m_model->slices()) {
            if (s && !before.contains(s->sliceId())) {
                createdId = s->sliceId();
                break;
            }
        }
        if (createdId < 0) {
            reportVfoBRouteFailure(client, request,
                QStringLiteral("VFO-B receiver could not be created"),
                !routeConfirmation.isEmpty());
            finishRouteTransition(transitionGeneration);
            return;
        }

        const auto tearDown = [this, createdId] {
            if (SliceModel* s = m_model->slice(createdId))
                m_model->removePanadapter(s->panId());
        };

        if (splitOnly && !m_routingState.splitRequested()) {
            tearDown();
            finishRouteTransition(transitionGeneration);
            return;
        }

        m_routingState.bindCreatedRoute(rxSlice->sliceId(), createdId);
        QPointer<TciServer> self(this);
        promoteTxSliceAndContinue(createdId,
            [self, client, request, routeConfirmation, createdId, tearDown,
             transitionGeneration](bool selected) {
            if (!self)
                return;
            if (!client || !selected) {
                tearDown();
                self->m_routingState.clearTciRoute();
                self->reportVfoBRouteFailure(client, request,
                    QStringLiteral("created VFO-B slice could not be selected for TX"),
                    !routeConfirmation.isEmpty());
                self->finishRouteTransition(transitionGeneration);
                return;
            }
            if (!routeConfirmation.isEmpty() && self->m_routingState.splitRequested())
                self->broadcast(routeConfirmation);
            self->tuneSliceAndConfirm(client, request.trx, request.channel,
                                      createdId, request.frequencyHz);
            self->finishRouteTransition(transitionGeneration);
        });
        return;
    }

    if (m_model->slices().size() >= m_model->maxSlices()) {
        reportVfoBRouteFailure(client, request,
            QStringLiteral("cannot create VFO B: radio slice capacity reached"),
            !routeConfirmation.isEmpty());
        return;
    }

    if (m_pendingVfoBCreate) {
        if (m_pendingVfoBCreate->rxSliceId == rxSlice->sliceId()) {
            m_pendingVfoBCreate->client = client;
            m_pendingVfoBCreate->request = request;
            if (!routeConfirmation.isEmpty()) {
                m_pendingVfoBCreate->routeConfirmation = routeConfirmation;
            }
            m_pendingVfoBCreate->splitOnly =
                m_pendingVfoBCreate->splitOnly && splitOnly;
        }
        return;
    }

    const quint64 transitionGeneration = beginRouteTransition();
    m_pendingVfoBCreate = PendingVfoBCreate {
        client, request, rxSlice->sliceId(), routeConfirmation, splitOnly,
        transitionGeneration
    };
    const double mhz = static_cast<double>(request.frequencyHz) / 1.0e6;
    const QString command
        = QStringLiteral("slice create pan=%1 freq=%2").arg(rxSlice->panId()).arg(mhz, 0, 'f', 6);
    QPointer<TciServer> self(this);
    m_model->sendCmdPublic(command, [self](int code, const QString& body) {
        if (!self || !self->m_pendingVfoBCreate) {
            return;
        }
        const PendingVfoBCreate pending = *self->m_pendingVfoBCreate;
        self->m_pendingVfoBCreate.reset();
        if (code != 0) {
            self->reportVfoBRouteFailure(pending.client, pending.request,
                QStringLiteral("VFO-B slice create rejected: code=%1 body=%2")
                    .arg(QString::number(code, 16), body),
                !pending.routeConfirmation.isEmpty());
            self->finishRouteTransition(pending.transitionGeneration);
            return;
        }

        bool idOk = false;
        const int sliceId = body.section(QLatin1Char(','), 0, 0).trimmed().toInt(&idOk);
        if (!idOk || !self->m_model || !self->m_model->slice(sliceId)) {
            self->reportVfoBRouteFailure(pending.client, pending.request,
                QStringLiteral("VFO-B create reply had no settled slice: %1").arg(body),
                !pending.routeConfirmation.isEmpty());
            self->finishRouteTransition(pending.transitionGeneration);
            return;
        }

        const bool clientStillConnected = pending.client
            && std::any_of(self->m_clients.cbegin(), self->m_clients.cend(),
                [&pending](const ClientState& state) {
                    return state.socket == pending.client;
                });
        if (!clientStillConnected) {
            // The asynchronous create completed after its requester left.
            // Reap only the slice created by this reply; never leave an
            // unowned TX route behind.
            self->m_model->sendCommand(QStringLiteral("slice remove %1").arg(sliceId));
            if (!pending.routeConfirmation.isEmpty()) {
                self->m_routingState.setSplitRequested(false);
            }
            self->finishRouteTransition(pending.transitionGeneration);
            return;
        }
        if (pending.splitOnly && !self->m_routingState.splitRequested()) {
            self->m_model->sendCommand(QStringLiteral("slice remove %1").arg(sliceId));
            self->finishRouteTransition(pending.transitionGeneration);
            return;
        }

        self->m_routingState.bindCreatedRoute(pending.rxSliceId, sliceId);
        self->promoteTxSliceAndContinue(sliceId, [self, pending, sliceId](bool selected) {
            if (!self) {
                return;
            }
            if (!pending.client || !selected) {
                if (self->m_model) {
                    self->m_model->sendCommand(
                        QStringLiteral("slice remove %1").arg(sliceId));
                }
                self->m_routingState.clearTciRoute();
                self->reportVfoBRouteFailure(pending.client, pending.request,
                    QStringLiteral("created VFO-B slice could not be selected for TX"),
                    !pending.routeConfirmation.isEmpty());
                self->finishRouteTransition(pending.transitionGeneration);
                return;
            }
            if (!pending.routeConfirmation.isEmpty()
                && self->m_routingState.splitRequested()) {
                self->broadcast(pending.routeConfirmation);
            }
            self->tuneSliceAndConfirm(pending.client, pending.request.trx, pending.request.channel,
                sliceId, pending.request.frequencyHz);
            self->finishRouteTransition(pending.transitionGeneration);
        });
    });
}

void TciServer::reportVfoBRouteFailure(QWebSocket* client,
    const TciProtocol::VfoRequest& request,
    const QString& reason,
    bool rejectSplit)
{
    m_lastRouteError = reason;
    qCWarning(lcCat).noquote() << "TCI:" << reason;

    if (rejectSplit) {
        m_routingState.setSplitRequested(false);
        broadcast(QStringLiteral("split_enable:%1,false;").arg(request.trx));
    }

    // TCI has no standard error frame. Return the authoritative channel-1
    // projection so clients stop waiting for an acknowledgement and can detect
    // that the requested frequency was not accepted.
    if (client) {
        if (SliceModel* fallback = sliceForTrx(request.trx)) {
            replyText(client,
                QStringLiteral("vfo:%1,1,%2;")
                    .arg(request.trx)
                    .arg(TciProtocol::mhzToHz(fallback->frequency())));
        }
    }
}

void TciServer::handleVfoRequest(QWebSocket* client, const TciProtocol::VfoRequest& request)
{
    const bool pttBlocksRouteChange = [&] {
        if (!m_model || !m_model->isRadioTransmitting() || request.channel != 1) {
            return false;
        }
        SliceModel* rxSlice = sliceForTrx(request.trx);
        SliceModel* txSlice = m_model->txSlice();
        return !rxSlice || !txSlice || txSlice == rxSlice;
    }();
    if (client && request.channel == 1
        && (m_routeTransitionInFlight || m_tciPttCancelPending || pttBlocksRouteChange)) {
        if (!m_pendingRouteCommands.isEmpty()) {
            PendingRouteCommand& last = m_pendingRouteCommands.last();
            if (last.kind == PendingRouteCommand::Kind::Vfo
                && last.client == client
                && last.vfo.trx == request.trx
                && last.vfo.channel == request.channel) {
                last.vfo = request;
                return;
            }
        }
        PendingRouteCommand pending;
        pending.kind = PendingRouteCommand::Kind::Vfo;
        pending.client = client;
        pending.vfo = request;
        m_pendingRouteCommands.append(pending);
        return;
    }

    SliceModel* rxSlice = sliceForTrx(request.trx);
    if (!client || !rxSlice) {
        return;
    }
    if (request.channel == 0) {
        tuneSliceAndConfirm(client, request.trx, 0, rxSlice->sliceId(), request.frequencyHz);
        return;
    }

    const TciRoutingState::RouteDecision route
        = m_routingState.resolveVfoB(rxSlice->sliceId(), routingEndpoints());
    if (route.action == TciRoutingState::RouteAction::UseExisting) {
        tuneSliceAndConfirm(client, request.trx, 1, route.txSliceId, request.frequencyHz);
        return;
    }
    if (route.action == TciRoutingState::RouteAction::PromoteExisting) {
        const quint64 transitionGeneration = beginRouteTransition();
        QPointer<TciServer> self(this);
        QPointer<QWebSocket> socket(client);
        promoteTxSliceAndContinue(route.txSliceId,
            [self, socket, request, route, transitionGeneration](bool selected) {
                if (!self) {
                    return;
                }
                if (socket && selected) {
                    self->tuneSliceAndConfirm(
                        socket, request.trx, 1, route.txSliceId, request.frequencyHz);
                }
                self->finishRouteTransition(transitionGeneration);
            });
        return;
    }
    if (route.action == TciRoutingState::RouteAction::Create) {
        createTxSliceForVfoB(client, request, rxSlice);
    }
}

void TciServer::handleSplitRequest(QWebSocket* client, const TciProtocol::SplitRequest& request)
{
    if (!client) {
        return;
    }
    if (m_routeTransitionInFlight || m_tciPttClient || m_tciPttCancelPending
        || (m_model && m_model->isRadioTransmitting())) {
        if (!m_pendingRouteCommands.isEmpty()) {
            PendingRouteCommand& last = m_pendingRouteCommands.last();
            if (last.kind == PendingRouteCommand::Kind::Split
                && last.client == client
                && last.split.trx == request.trx) {
                last.split = request;
                return;
            }
        }
        PendingRouteCommand pending;
        pending.kind = PendingRouteCommand::Kind::Split;
        pending.client = client;
        pending.split = request;
        m_pendingRouteCommands.append(pending);
        return;
    }

    const bool changed = m_routingState.setSplitRequested(request.enabled);
    const QString confirmation = QStringLiteral("split_enable:%1,%2;")
                                     .arg(request.trx)
                                     .arg(request.enabled ? "true" : "false");

    if (request.enabled) {
        SliceModel* rxSlice = sliceForTrx(request.trx);
        if (!rxSlice) {
            m_routingState.setSplitRequested(false);
            return;
        }

        const TciRoutingState::RouteDecision route
            = m_routingState.resolveVfoB(rxSlice->sliceId(), routingEndpoints());
        if (route.action == TciRoutingState::RouteAction::UseExisting) {
            broadcast(confirmation);
            return;
        }
        if (route.action == TciRoutingState::RouteAction::PromoteExisting) {
            const quint64 transitionGeneration = beginRouteTransition();
            QPointer<TciServer> self(this);
            promoteTxSliceAndContinue(route.txSliceId,
                [self, confirmation, transitionGeneration](bool selected) {
                    if (!self) {
                        return;
                    }
                    if (!selected) {
                        self->m_routingState.setSplitRequested(false);
                        self->finishRouteTransition(transitionGeneration);
                        return;
                    }
                    if (self->m_routingState.splitRequested()) {
                        self->broadcast(confirmation);
                    }
                    self->finishRouteTransition(transitionGeneration);
                });
            return;
        }
        if (route.action == TciRoutingState::RouteAction::Create) {
            const TciProtocol::VfoRequest initialTxVfo {
                request.trx, 1, TciProtocol::mhzToHz(rxSlice->frequency())
            };
            createTxSliceForVfoB(
                client, initialTxVfo, rxSlice, confirmation, true);
            return;
        }
        m_routingState.setSplitRequested(false);
        return;
    }

    // A steady false is WSJT-X's compatibility sequence. It must not discard
    // or retarget VFO B. External TX routes are also never reclaimed here.
    if (!changed || !m_routingState.ownsRoute()) {
        broadcast(confirmation);
        return;
    }

    SliceModel* rxSlice = sliceForTrx(request.trx);
    const int createdSliceId = m_routingState.owner() == TciRoutingState::TxRouteOwner::TciCreated
        ? m_routingState.txSliceId()
        : -1;
    if (!rxSlice) {
        return;
    }

    QPointer<TciServer> self(this);
    const quint64 transitionGeneration = beginRouteTransition();
    promoteTxSliceAndContinue(
        rxSlice->sliceId(),
        [self, confirmation, createdSliceId, transitionGeneration](bool selected) {
            if (!self) {
                return;
            }
            if (selected) {
                if (createdSliceId >= 0 && self->m_model) {
                    self->m_model->sendCommand(
                        QStringLiteral("slice remove %1").arg(createdSliceId));
                }
                self->m_routingState.clearTciRoute();
                self->broadcast(confirmation);
            }
            self->finishRouteTransition(transitionGeneration);
        });
}

quint64 TciServer::beginRouteTransition()
{
    m_routeTransitionInFlight = true;
    return ++m_routeTransitionGeneration;
}

void TciServer::finishRouteTransition(quint64 generation)
{
    if (!m_routeTransitionInFlight || generation != m_routeTransitionGeneration) {
        return;
    }
    m_routeTransitionInFlight = false;
    drainDeferredRoutingAndPtt();
}

void TciServer::drainDeferredRoutingAndPtt()
{
    const auto radioIsTransmitting = [this] {
        return m_model && m_model->isRadioTransmitting();
    };
    if (m_routeTransitionInFlight || m_tciPttClient || m_tciPttCancelPending
        || radioIsTransmitting()) {
        return;
    }

    while (!m_routeTransitionInFlight && !m_tciPttClient && !m_tciPttCancelPending
        && !radioIsTransmitting()
        && !m_pendingRouteCommands.isEmpty()) {
        const PendingRouteCommand pending = m_pendingRouteCommands.takeFirst();
        if (!pending.client) {
            continue;
        }
        if (pending.kind == PendingRouteCommand::Kind::Vfo) {
            handleVfoRequest(pending.client, pending.vfo);
        } else {
            handleSplitRequest(pending.client, pending.split);
        }
    }
    if (m_routeTransitionInFlight || m_tciPttClient || m_tciPttCancelPending
        || radioIsTransmitting()) {
        return;
    }

    if (!m_pendingTrxRequest) {
        return;
    }

    const PendingTrxRequest pending = *m_pendingTrxRequest;
    m_pendingTrxRequest.reset();
    if (pending.client) {
        handleTrxRequest(pending.client, pending.request);
    }
}

void TciServer::handleTrxRequest(QWebSocket* client, const TciProtocol::TrxRequest& request)
{
    if (!client || !m_model) {
        return;
    }
    if (request.transmitting && (m_routeTransitionInFlight || m_tciPttCancelPending)) {
        m_pendingTrxRequest = PendingTrxRequest { client, request };
        return;
    }
    if (!request.transmitting) {
        if (m_pendingTrxRequest && m_pendingTrxRequest->client == client) {
            m_pendingTrxRequest.reset();
        }
        for (int i = m_pendingRouteCommands.size() - 1; i >= 0; --i) {
            if (m_pendingRouteCommands.at(i).client == client) {
                m_pendingRouteCommands.removeAt(i);
            }
        }
        // A client may only release a transmit session it owns. In particular,
        // never let a TCI "trx:false" unkey an operator, VOX, or another client.
        if (!m_tciPttClient || m_tciPttClient != client) {
            replyText(client,
                QStringLiteral("trx:%1,%2;")
                    .arg(request.trx)
                    .arg(m_model->isRadioTransmitting() ? "true" : "false"));
            return;
        }
        if (m_tciPttRequestedOn && !m_tciPttConfirmedOn) {
            // The radio may still accept the queued key-up after this release.
            // Keep a short fail-closed barrier so that late edge is unkeyed
            // rather than exposed as a new external transmit session.
            broadcastActualTxState(false);
            abortTciPtt();
            return;
        }
        ++m_tciPttGeneration;
        m_tciPttRequestedOn = false;
        requestTciPttOff();
        if (!m_model->isRadioTransmitting()) {
            broadcastActualTxState(false);
            stopTxChrono();
            m_tciPttConfirmedOn = false;
            m_tciPttWantsAudio = false;
            m_tciPttClient.clear();
            drainDeferredRoutingAndPtt();
        }
        return;
    }

    if (m_tciPttClient && m_tciPttClient != client) {
        // #4547 fix list item 4. TCI PTT has one global owner, so a second
        // client's key is refused while the first holds it — and refusing it
        // in silence is what WSJT-X surfaces as "TCI failed to set ptt" with
        // no cause. Report the actual false state, like every other refusal
        // path below. This gets more reachable with the routing fix, not less:
        // binding each client to its own slice is precisely what lets two of
        // them genuinely contend, where before both were routed onto one slice.
        qCWarning(lcCat) << "TCI PTT: trx" << request.trx
                         << "declined - another client holds TCI PTT"
                         << "peer=" << client->peerAddress().toString();
        replyText(client, QStringLiteral("trx:%1,false;").arg(request.trx));
        return;
    }
    if (m_tciPttClient == client && m_tciPttRequestedOn) {
        return;
    }
    if (m_model->isRadioTransmitting()) {
        if (m_tciPttClient == client) {
            replyText(client, QStringLiteral("trx:%1,true;").arg(request.trx));
        }
        return;
    }

    // Strict resolution: this path keys the radio, so an unresolvable receiver
    // must decline rather than fall back to slices[0] and transmit on a slice
    // the client never addressed (#4547). Report the actual false state — the
    // PTT-rejection invariant — so the client gets a cause instead of the
    // silence WSJT-X surfaces as "TCI failed to set ptt".
    const int boundTrx = effectiveTrx(client, request.trx);
    SliceModel* rxSlice = sliceForTrxStrict(boundTrx);
    if (!rxSlice) {
        // Two very different situations reach here and they want different
        // operator responses. TciTrxMap holds a receiver's binding across a
        // band-change recreate and answers null rather than re-pointing it at
        // whichever slice now sits at that index (#4577) — transient, and the
        // next request succeeds. With no slices at all the session cannot key
        // anything. Naming them apart is the whole point of logging this.
        const bool haveSlices = m_model->isConnected() && !m_model->slices().isEmpty();
        qCWarning(lcCat) << "TCI PTT: receiver" << boundTrx
                         << "(requested trx" << request.trx << ")"
                         << (haveSlices
                                    ? "maps to no live slice (unknown receiver, or its slice is"
                                      " mid-recreate) - request declined"
                                    : "cannot be resolved - radio not connected, or no slices"
                                      " - request declined");
        replyText(client, QStringLiteral("trx:%1,false;").arg(request.trx));
        return;
    }
    const QVector<TciSliceEndpoint> endpoints = routingEndpoints();
    const int liveTx = TciRoutingState::currentTxSlice(endpoints);
    // Sample the cached route BEFORE resolving. resolvePttSlice() writes the
    // live TX assignment through to the cache on the external-TX branch, so
    // reading these afterwards would always show the cache agreeing with
    // liveTx - erasing exactly the disagreement this line exists to catch.
    const int cachedTx = m_routingState.txSliceId();
    const int cachedRx = m_routingState.rxSliceId();
    const char* const cachedOwner = txRouteOwnerName(m_routingState.owner());
    const int txSliceId = m_routingState.resolvePttSlice(rxSlice->sliceId(), endpoints);

    // Why did transmit land where it did?  Every TCI routing fault reported so
    // far reduces to one of three things, and all three are invisible without
    // this line: the requested trx resolved away, a cached route outliving the
    // live TX assignment, or two clients addressing the same trx.  Log the
    // whole decision - request, live state, cached state, result - so a report
    // can be diagnosed from a log instead of a reproduction.
    //
    // source= is client-supplied and goes out last. simplified() collapses any
    // embedded newline, because .noquote() means whatever a client puts in that
    // field lands in the log verbatim — and a forged "TCI PTT route:" line in
    // the evidence a reporter attaches is a worse failure than no line at all.
    qCInfo(lcCat).nospace().noquote()
        << "TCI PTT route: trx=" << request.trx
        << (m_trxMap.trxForSlice(m_model, rxSlice) == request.trx ? "" : " [trx fallback]")
        << " rxSlice=" << sliceTag(rxSlice->sliceId())
        << " -> txSlice=" << sliceTag(txSliceId)
        << (txSliceId == rxSlice->sliceId() ? " (the requested slice)"
                                            : " (NOT the requested slice)")
        << " | liveTx=" << sliceTag(liveTx)
        << " cachedTx=" << sliceTag(cachedTx)
        << " cachedRx=" << sliceTag(cachedRx)
        << " owner=" << cachedOwner
        << " split=" << (m_routingState.splitRequested() ? "true" : "false")
        << " source="
        << (request.source.isEmpty() ? QStringLiteral("(none)")
                                     : request.source.simplified().left(32));

    SliceModel* txSlice = m_model->slice(txSliceId);
    if (!txSlice) {
        qCWarning(lcCat).noquote() << "TCI PTT: resolved tx slice" << sliceTag(txSliceId)
                                   << "does not exist - request declined";
        replyText(client, QStringLiteral("trx:%1,false;").arg(request.trx));
        return;
    }
    const QString inhibitReason = m_model->panTransmitInhibitReason(txSlice->panId());
    if (!inhibitReason.isEmpty()) {
        // simplified() for the same reason as source= above, and because a
        // reason that is one grep-able line is worth more in a bug report
        // than one that wraps: this is a translated sentence, not a token.
        qCWarning(lcCat).noquote() << "TCI PTT: slice" << sliceTag(txSliceId)
                                   << "is transmit-inhibited -" << inhibitReason.simplified();
        replyText(client, QStringLiteral("trx:%1,false;").arg(request.trx));
        return;
    }

    // Stated as intent, and only once the drop paths above are behind us: the
    // promote below is asynchronous and can still come back unselected, so
    // this is the request that survived every guard, not a completed move.
    if (liveTx >= 0 && txSliceId != liveTx) {
        qCInfo(lcCat).noquote() << "TCI PTT: transmit will move from slice" << sliceTag(liveTx)
                                << "to slice" << sliceTag(txSliceId);
    }

    const QString mode = txSlice->mode().trimmed().toUpper();
    const bool digitalMode = mode == QStringLiteral("DIGU") || mode == QStringLiteral("DIGL")
        || mode == QStringLiteral("RTTY") || mode == QStringLiteral("FDV")
        || mode == QStringLiteral("FDVU") || mode == QStringLiteral("FDVL");
    const bool wantsAudio = request.source == QStringLiteral("dax")
        || request.source == QStringLiteral("tci") || (request.source.isEmpty() && digitalMode);

    const quint64 transitionGeneration = beginRouteTransition();
    QPointer<TciServer> self(this);
    QPointer<QWebSocket> socket(client);
    promoteTxSliceAndContinue(txSliceId,
        [self, socket, request, wantsAudio, transitionGeneration](bool selected) {
        if (!self) {
            return;
        }
        if (!socket || !selected || !self->m_model) {
            // A refused promote used to be near-unreachable: resolvePttSlice()
            // returned the slice that already held TX, so promoteTxSlice took
            // its isTxSlice() early return. Honouring the requested slice
            // (#4547) means real promotions are now attempted, so this path is
            // live on both planes — a Flex `slice set N tx=1` rejection, and an
            // HL2 transmitter move the backend declines. Report the actual
            // false state rather than going silent; silence is what WSJT-X
            // surfaces as "TCI failed to set ptt" with no cause.
            if (socket) {
                self->replyText(socket,
                    QStringLiteral("trx:%1,false;").arg(request.trx));
            }
            self->finishRouteTransition(transitionGeneration);
            return;
        }
        self->m_tciPttClient = socket;
        self->m_tciPttTrx = request.trx;
        self->m_tciPttWantsAudio = wantsAudio;
        self->m_tciPttRequestedOn = true;
        self->m_tciPttConfirmedOn = false;
        const quint64 generation = ++self->m_tciPttGeneration;

        if (wantsAudio) {
            self->prepareTxAudio();
            self->m_model->setTransmit(true, TransmitModel::PttSource::Dax);
        } else {
            // Hardware-style TCI PTT shares the same preflight and Quindar
            // coordinator as local controls. This remains a single xmit path:
            // TciProtocol no longer keys independently.
            self->m_model->transmitModel().requestPttOn(
                TransmitModel::PttSource::TciHardware);
        }

        QTimer::singleShot(1250, self, [self, socket, generation, request]() {
            if (!self || generation != self->m_tciPttGeneration || !self->m_tciPttRequestedOn
                || self->m_tciPttConfirmedOn) {
                return;
            }
            self->abortTciPtt();
            if (socket) {
                self->replyText(socket, QStringLiteral("trx:%1,false;").arg(request.trx));
            }
        });
        self->finishRouteTransition(transitionGeneration);
    });
}

// ── Binary message handler (TX audio from TCI client) ───────────────────

void TciServer::onBinaryMessage(const QByteArray& data)
{
    if (!m_audio) return;
    if (data.size() < static_cast<int>(sizeof(TciAudioHeader))) return;

    // Parse header
    TciAudioHeader hdr;
    std::memcpy(&hdr, data.constData(), sizeof(hdr));

    // Only accept TX_AUDIO_STREAM (type 2)
    if (hdr.type != 2) return;

    const int payloadBytes = data.size() - static_cast<int>(sizeof(TciAudioHeader));
    if (payloadBytes <= 0) return;

    const char* payload = data.constData() + sizeof(TciAudioHeader);

    // ── Convert TX audio to float32 stereo ─────────────────────────────────
    // WSJT-X channels field is garbage (FIFO reuse). readAudioData() writes
    // hdr.length floats to data[0..length-1]. Take the first hdr.length floats.
    QByteArray pcm;

    if (hdr.format == 3) {
        int validFloats = static_cast<int>(hdr.length);
        int availFloats = payloadBytes / static_cast<int>(sizeof(float));
        if (validFloats > availFloats) validFloats = availFloats;
        if (validFloats <= 0) return;

        pcm = QByteArray(payload,
                         validFloats * static_cast<int>(sizeof(float)));
    } else if (hdr.format == 0) {
        int validSamples = static_cast<int>(hdr.length);
        int availSamples = payloadBytes / static_cast<int>(sizeof(qint16));
        if (validSamples > availSamples) validSamples = availSamples;
        if (validSamples <= 0) return;

        auto* src = reinterpret_cast<const qint16*>(payload);
        pcm.resize(validSamples * static_cast<int>(sizeof(float)));
        auto* dst = reinterpret_cast<float*>(pcm.data());
        for (int i = 0; i < validSamples; ++i)
            dst[i] = src[i] / 32768.0f;
    }

    if (pcm.isEmpty()) return;

    int inputFramesSrcRate = 0;   // input frames at the client-declared rate (#3914)
    bool duplicatedStereo = false;

    // ─── TX resampling: client-declared rate → 24kHz (radio native DAX) ──
    // Resample from the rate the client declared in THIS frame (hdr.sampleRate),
    // not a hardcoded 48k. WSJT-X sends 48 kHz — the common path, unchanged — but
    // a client that negotiated 8/12/24 kHz (audio_samplerate) sends at that rate
    // and must be resampled from it, or every tone is mis-pitched and digital
    // decodes fail (#3306). Rebuild the per-session resampler only if the
    // declared rate changes (rare, mid-stream); a 24 kHz client gets a 1:1
    // resampler so the mono/stereo canonicalization below still runs.
    {
        const int declaredRate = static_cast<int>(hdr.sampleRate);
        const int txSrcRate = (declaredRate == 8000 || declaredRate == 12000
                               || declaredRate == 24000 || declaredRate == 48000)
                                  ? declaredRate
                                  : 48000;   // default/garbage -> WSJT-X-compatible 48k
        if (!m_txResampler
            || static_cast<int>(m_txResampler->srcRate()) != txSrcRate) {
            m_txResampler = std::make_unique<Resampler>(
                static_cast<double>(txSrcRate), 24000.0, 4096);
        }
    }

    // Detect mono vs stereo from payload layout.
    //
    // WSJT-X's TCI modulator writes the first `hdr.length` floats as duplicated
    // stereo pairs (L=R), even though the payload buffer it allocates is larger.
    // Treating those `hdr.length` floats as true mono doubles the apparent
    // duration of every block and destroys digital-mode tones.
    if (m_txResampler) {
        int totalFloats = pcm.size() / static_cast<int>(sizeof(float));
        int declaredSamples = static_cast<int>(hdr.length);
        const auto* fSrc = reinterpret_cast<const float*>(pcm.constData());

        if (hdr.format == 3 && totalFloats >= 2 && (totalFloats % 2) == 0) {
            const int pairsToCheck = std::min(totalFloats / 2, 128);
            int duplicatedPairs = 0;
            for (int i = 0; i < pairsToCheck; ++i) {
                if (std::fabs(fSrc[i * 2] - fSrc[i * 2 + 1]) < 1.0e-6f)
                    ++duplicatedPairs;
            }
            duplicatedStereo = duplicatedPairs >= (pairsToCheck * 9) / 10;
        }

        if (duplicatedStereo) {
            // WSJT-X fills `length` floats as stereo pairs in-place.
            int stereoFrames = totalFloats / 2;
            inputFramesSrcRate = stereoFrames;
            pcm = m_txResampler->processStereoToStereo(fSrc, stereoFrames);
        } else if (totalFloats <= declaredSamples) {
            // True mono: upmix to stereo then resample.
            int monoFrames = totalFloats;
            inputFramesSrcRate = monoFrames;
            pcm = m_txResampler->processMonoToStereo(fSrc, monoFrames);
        } else {
            // Explicit stereo: resample directly.
            int stereoFrames = totalFloats / 2;
            inputFramesSrcRate = stereoFrames;
            pcm = m_txResampler->processStereoToStereo(fSrc, stereoFrames);
        }
        if (pcm.isEmpty()) return;
    }

    auto* dst = reinterpret_cast<float*>(pcm.data());
    const int outputStereoFrames = pcm.size() / (2 * static_cast<int>(sizeof(float)));
    const int outputSamples = pcm.size() / static_cast<int>(sizeof(float));
    double sumSq = 0.0;
    float peak = 0.0f;
    qint64 clipSamples = 0;
    // Three overflow regimes selectable via right-click on the TCI TX slider:
    //   Clip     — saturating clamp at ±1.0; cheap defensive limiter,
    //              introduces harmonics on overshoots but protects the
    //              radio float→int16 stage from out-of-range input.
    //   NaNGuard — pass everything except NaN/Inf (which the radio can't
    //              digest); preserves bit-exact tones for well-formed
    //              digital clients, accepts that a malformed >1.0 client
    //              will reach the radio.
    //   Measure  — pure bypass: count overshoots for telemetry but never
    //              touch sample data.  100% client-side passthrough.
    switch (m_overflowMode) {
    case OverflowMode::Clip:
        for (int i = 0; i < outputSamples; ++i) {
            float v = dst[i] * m_txGain;
            if (v > 1.0f) { v = 1.0f; ++clipSamples; }
            else if (v < -1.0f) { v = -1.0f; ++clipSamples; }
            dst[i] = v;
            peak = std::max(peak, std::abs(v));
            sumSq += static_cast<double>(v) * static_cast<double>(v);
        }
        break;
    case OverflowMode::NaNGuard:
        for (int i = 0; i < outputSamples; ++i) {
            float v = dst[i] * m_txGain;
            if (!std::isfinite(v)) { v = 0.0f; ++clipSamples; }
            else if (std::abs(v) > 1.0f) ++clipSamples;
            dst[i] = v;
            peak = std::max(peak, std::abs(v));
            sumSq += static_cast<double>(v) * static_cast<double>(v);
        }
        break;
    case OverflowMode::Measure:
        for (int i = 0; i < outputSamples; ++i) {
            const float v = dst[i] * m_txGain;
            dst[i] = v;
            if (!std::isfinite(v) || std::abs(v) > 1.0f) ++clipSamples;
            const float absV = std::isfinite(v) ? std::abs(v) : 0.0f;
            peak = std::max(peak, absV);
            sumSq += std::isfinite(v)
                       ? static_cast<double>(v) * static_cast<double>(v)
                       : 0.0;
        }
        break;
    }

    ++m_txAudioBlocks;
    m_txInputFrames += inputFramesSrcRate;
    m_txOutputFrames += outputStereoFrames;
    m_txClipSamples += clipSamples;
    m_txAudioSampleCount += outputSamples;
    m_txAudioSumSq += sumSq;
    m_txAudioPeak = std::max(m_txAudioPeak, peak);
    m_txSawDuplicatedStereo = m_txSawDuplicatedStereo || duplicatedStereo;

    if (outputSamples > 0) {
        emit txLevel(std::sqrt(static_cast<float>(sumSq / outputSamples)));
    }

    if ((m_txAudioBlocks % kTxSummaryEveryBlocks) == 0)
        logTxAudioSummary("running");

    QMetaObject::invokeMethod(m_audio, "feedDaxTxAudio",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, pcm));
}

// ── RX audio from DAX pipeline → TCI binary frames ─────────────────────

void TciServer::onDaxAudioReady(int channel, const QByteArray& pcm)
{
    // Map DAX channel -> TCI TRX by the slice that owns the channel. Flex
    // slice ids are not necessarily zero-based for this client when another
    // client owns slice 0, but TCI receivers are advertised as 0..N-1.
    int trx = -1;
    int owningSliceId = -1;
    if (m_model) {
        for (auto* s : m_model->slices()) {
            if (s->daxChannel() == channel) {
                trx = m_trxMap.trxForSlice(m_model,s);
                owningSliceId = s->sliceId();
                m_channelTrx[channel] = trx;   // remember the resolved mapping (#3669)
                break;
            }
        }
    }
    if (trx < 0) {
        // The owning slice's DAX binding is transiently 0, so the scan above
        // missed it: the radio re-broadcasts `dax=0` then `dax=1` during a
        // band/mode retune, or when a second client (re)subscribes, and
        // SliceModel zeroes m_daxChannel on the `dax=0`. Route by the last
        // resolved TRX for this channel instead of the positional `channel-1`
        // fallback — in a multi-receiver setup tciTrxForSlice() returns the
        // slice's *index*, which diverges from `channel-1`, so the positional
        // guess trips the `audioReceiver != trx` filter below and silently
        // drops audio for the correctly-bound client (#3669). Cold start (no
        // mapping resolved yet) keeps the legacy positional guess.
        trx = m_channelTrx.value(channel, std::max(0, channel - 1));
    }

    // Check if any client has this receiver's audio enabled. A client that
    // sends audio_start with no receiver keeps the legacy all-receiver behavior.
    int enabledClients = 0;
    for (const auto& cs : m_clients) {
        if (cs.audioEnabled && (cs.audioReceiver < 0 || cs.audioReceiver == trx))
            ++enabledClients;
    }
    if (enabledClients == 0) return;

    ++m_rxAudioPackets;

    const float channelGain = (channel >= 1 && channel <= 8)
        ? m_rxChannelGain[channel - 1] : 1.0f;

    // RMS level meter — post-gain, consistent with DAX meter convention.
    // One emission per DAX packet is cheap at ~187 Hz (128-frame packets /24kHz).
    if (channel >= 1 && channel <= 8) {
        const auto* src = reinterpret_cast<const float*>(pcm.constData());
        const int n = pcm.size() / static_cast<int>(sizeof(float));
        if (n > 0) {
            double sumSq = 0.0;
            for (int i = 0; i < n; ++i) sumSq += static_cast<double>(src[i]) * src[i];
            emit rxLevel(channel, std::sqrt(static_cast<float>(sumSq / n)) * channelGain);
        }
    }

    int sentClients = 0;
    int lastOutputFrames = 0;
    int lastSampleRate = 0;
    int lastChannels = 0;
    int lastFormat = 0;

    // Per-client: accumulate then resample
    for (auto& cs : m_clients) {
        if (!cs.audioEnabled) continue;
        if (cs.audioReceiver >= 0 && cs.audioReceiver != trx) continue;

        // Accumulate DAX packets into a buffer before resampling.
        // DAX delivers ~128-frame packets; r8brain needs larger blocks
        // for clean output without startup transients.
        QByteArray& accumBuf = cs.rxAccumBuf[channel];
        accumBuf.append(pcm);

        int accumFrames = accumBuf.size() / (2 * static_cast<int>(sizeof(float)));

        // Obtain (or lazily create) the per-channel resampler.
        // Each DAX channel needs its own stateful r8brain instance so that
        // filter history from slice A cannot bleed into slice B (#1806).
        // No resampler is needed when the client requested native 24 kHz.
        Resampler* resampler = nullptr;
        if (cs.audioSampleRate != 24000) {
            if (!cs.resamplers.contains(channel))
                cs.resamplers[channel] = new Resampler(24000.0, cs.audioSampleRate, 4096);
            resampler = cs.resamplers[channel];
        }

        // If resampling, wait for enough data to feed r8brain cleanly.
        // Native 24kHz path flushes immediately.
        if (resampler && accumFrames < kAccumMinFrames) {
            continue;
        }

        // Transfer ownership before taking a data pointer.  The native 24 kHz
        // path uses this storage directly, including when it inherits staged
        // samples across a rate change; clearing/squeezing accumBuf first left
        // audioSrc dangling while gain conversion or frame construction still
        // read it (#4744).
        QByteArray accumulated = std::move(accumBuf);
        // Pin the moved-from state for the next packet's append.  The local
        // owner releases the old allocation at the end of this iteration.
        accumBuf.clear();
        const float* audioSrc = reinterpret_cast<const float*>(accumulated.constData());
        int audioFrames = accumFrames;
        QByteArray resampledBuf;

        if (resampler) {
            resampledBuf = resampler->processStereoToStereo(audioSrc, audioFrames);
            audioSrc = reinterpret_cast<const float*>(resampledBuf.constData());
            audioFrames = resampledBuf.size() / (2 * static_cast<int>(sizeof(float)));
        }

        int srcSamples = audioFrames * 2;  // stereo

        // Apply per-channel TCI gain.  Copy into a gained buffer only when the
        // gain is not unity — unity skips the memcpy and keeps audioSrc pointing
        // at the resampler output (or the raw accumulator in the 24kHz path).
        QByteArray gainedBuf;
        if (channelGain != 1.0f) {
            gainedBuf.resize(srcSamples * static_cast<int>(sizeof(float)));
            auto* dst = reinterpret_cast<float*>(gainedBuf.data());
            for (int i = 0; i < srcSamples; ++i) dst[i] = audioSrc[i] * channelGain;
            audioSrc = dst;
        }

        if (cs.audioFormat == 3) {
            // float32 output — pass through directly
            if (cs.audioChannels == 2) {
                const QByteArray frame =
                    buildAudioFrame(trx, 1, cs.audioSampleRate, 2,
                                    audioSrc, audioFrames);
                cs.socket->sendBinaryMessage(frame);
                ++sentClients;
                lastOutputFrames = audioFrames;
                lastSampleRate = cs.audioSampleRate;
                lastChannels = 2;
                lastFormat = cs.audioFormat;
            } else {
                // Mono: average L+R
                QVector<float> monoBuf(audioFrames);
                for (int i = 0; i < audioFrames; ++i)
                    monoBuf[i] = (audioSrc[i*2] + audioSrc[i*2+1]) * 0.5f;
                const QByteArray frame =
                    buildAudioFrame(trx, 1, cs.audioSampleRate, 1,
                                    monoBuf.constData(), audioFrames);
                cs.socket->sendBinaryMessage(frame);
                ++sentClients;
                lastOutputFrames = audioFrames;
                lastSampleRate = cs.audioSampleRate;
                lastChannels = 1;
                lastFormat = cs.audioFormat;
            }
        } else {
            // int16 output — convert float32 → int16
            if (cs.audioChannels == 2) {
                int payloadBytes = srcSamples * static_cast<int>(sizeof(qint16));
                QByteArray frame(sizeof(TciAudioHeader) + payloadBytes, Qt::Uninitialized);
                TciAudioHeader hdr{};
                hdr.receiver = static_cast<quint32>(trx);
                hdr.sampleRate = static_cast<quint32>(cs.audioSampleRate);
                hdr.format = 0;  // int16
                hdr.length = static_cast<quint32>(audioFrames * 2);  // total samples (stereo)
                hdr.type = 1;    // RX_AUDIO
                hdr.channels = 2;
                std::memcpy(frame.data(), &hdr, sizeof(hdr));
                auto* i16dst = reinterpret_cast<qint16*>(frame.data() + sizeof(hdr));
                for (int i = 0; i < srcSamples; ++i) {
                    i16dst[i] = static_cast<qint16>(std::clamp(audioSrc[i] * 32768.0f, -32768.0f, 32767.0f));
                }
                cs.socket->sendBinaryMessage(frame);
                ++sentClients;
                lastOutputFrames = audioFrames;
                lastSampleRate = cs.audioSampleRate;
                lastChannels = 2;
                lastFormat = cs.audioFormat;
            } else {
                // Mono int16
                int payloadBytes = audioFrames * static_cast<int>(sizeof(qint16));
                QByteArray frame(sizeof(TciAudioHeader) + payloadBytes, Qt::Uninitialized);
                TciAudioHeader hdr{};
                hdr.receiver = static_cast<quint32>(trx);
                hdr.sampleRate = static_cast<quint32>(cs.audioSampleRate);
                hdr.format = 0;
                hdr.length = static_cast<quint32>(audioFrames);  // total samples (mono = frames)
                hdr.type = 1;
                hdr.channels = 1;
                std::memcpy(frame.data(), &hdr, sizeof(hdr));
                auto* i16dst = reinterpret_cast<qint16*>(frame.data() + sizeof(hdr));
                for (int i = 0; i < audioFrames; ++i)
                    i16dst[i] = static_cast<qint16>(std::clamp(
                        (audioSrc[i*2] + audioSrc[i*2+1]) * 0.5f * 32768.0f, -32768.0f, 32767.0f));
                cs.socket->sendBinaryMessage(frame);
                ++sentClients;
                lastOutputFrames = audioFrames;
                lastSampleRate = cs.audioSampleRate;
                lastChannels = 1;
                lastFormat = cs.audioFormat;
            }
        }
    }

    if (sentClients > 0)
        m_rxAudioFramesSent += static_cast<qint64>(lastOutputFrames) * sentClients;

    const bool firstLog = !m_rxAudioLogTimer.isValid();
    const bool shouldLog = firstLog || m_rxAudioLogTimer.elapsed() >= 2000;
    if (shouldLog && (sentClients > 0 || firstLog)) {
        qCDebug(lcCat).noquote()
            << "TCI: DAX RX audio"
            << QStringLiteral("dax_ch=%1").arg(channel)
            << QStringLiteral("slice=%1").arg(owningSliceId)
            << QStringLiteral("receiver=%1").arg(trx)
            << QStringLiteral("in_bytes=%1").arg(pcm.size())
            << QStringLiteral("enabled_clients=%1").arg(enabledClients)
            << QStringLiteral("sent_clients=%1").arg(sentClients)
            << QStringLiteral("out_frames=%1").arg(lastOutputFrames)
            << QStringLiteral("rate=%1").arg(lastSampleRate)
            << QStringLiteral("channels=%1").arg(lastChannels)
            << QStringLiteral("format=%1").arg(lastFormat)
            << QStringLiteral("packets=%1").arg(m_rxAudioPackets)
            << QStringLiteral("frames_sent=%1").arg(m_rxAudioFramesSent);
        m_rxAudioLogTimer.restart();
    }
}

// ── Build TCI binary audio frame ────────────────────────────────────────

QByteArray TciServer::buildAudioFrame(int receiver, int type,
                                      int sampleRate, int channels,
                                      const float* samples, int sampleCount)
{
    // sampleCount = number of frames (samples per channel)
    int totalFloats = sampleCount * channels;
    int payloadBytes = totalFloats * static_cast<int>(sizeof(float));

    QByteArray frame(sizeof(TciAudioHeader) + payloadBytes, Qt::Uninitialized);

    // Fill header — length = samples per channel (frames), per TCI v2.0 spec
    TciAudioHeader hdr{};
    hdr.receiver   = static_cast<quint32>(receiver);
    hdr.sampleRate = static_cast<quint32>(sampleRate);
    hdr.format     = 3;  // float32
    hdr.codec      = 0;
    hdr.crc        = 0;
    // length = total number of floats in the data field (not frames).
    // WSJT-X divides this by bytesPerFrame(2) to get stereo frame count.
    hdr.length     = static_cast<quint32>(sampleCount * channels);
    hdr.type       = static_cast<quint32>(type);
    hdr.channels   = static_cast<quint32>(channels);
    std::memset(hdr.reserved, 0, sizeof(hdr.reserved));

    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    std::memcpy(frame.data() + sizeof(hdr), samples, payloadBytes);

    return frame;
}

// ── Wire slice signals for state change broadcasts ──────────────────────

void TciServer::broadcastSliceFrequencies(SliceModel* slice)
{
    if (!slice || !m_model || m_clients.isEmpty()) {
        return;
    }

    const long long vfoHz = TciProtocol::mhzToHz(slice->frequency());
    if (vfoHz <= 0) {
        return;
    }

    const int trx = m_trxMap.trxForSlice(m_model, slice);
    const long long ddsHz = TciProtocol::ddsCenterHz(m_model, slice);
    broadcast(QStringLiteral("vfo:%1,0,%2;").arg(trx).arg(vfoHz));
    broadcast(QStringLiteral("dds:%1,%2;").arg(trx).arg(ddsHz));

    if (slice->sliceId() == m_routingState.txSliceId()) {
        SliceModel* rxSlice = m_model->slice(m_routingState.rxSliceId());
        if (rxSlice) {
            const int rxTrx = m_trxMap.trxForSlice(m_model, rxSlice);
            broadcast(QStringLiteral("vfo:%1,1,%2;").arg(rxTrx).arg(vfoHz));
        }
    }
}

void TciServer::wireSlice(int trx, SliceModel* slice)
{
    if (!slice) return;
    Q_UNUSED(trx);

    connect(slice, &SliceModel::frequencyChanged, this, [this, slice](double) {
        broadcastSliceFrequencies(slice);
    });

    connect(slice, &SliceModel::panIdChanged, this, [this, slice](const QString&) {
        broadcastSliceFrequencies(slice);
    });

    connect(slice, &SliceModel::modeChanged, this, [this, slice](const QString& mode) {
        if (m_clients.isEmpty()) return;
        const int trx = m_trxMap.trxForSlice(m_model,slice);
        broadcast(QStringLiteral("modulation:%1,%2;")
                      .arg(trx).arg(TciProtocol::smartsdrToTci(mode)));
    });

    connect(slice, &SliceModel::filterChanged, this, [this, slice](int lo, int hi) {
        if (m_clients.isEmpty()) return;
        const int trx = m_trxMap.trxForSlice(m_model,slice);
        broadcast(QStringLiteral("rx_filter_band:%1,%2,%3;")
                      .arg(trx).arg(lo).arg(hi));
    });

    connect(slice, &SliceModel::txSliceChanged, this, [this, slice](bool tx) {
        const int trx = m_trxMap.trxForSlice(m_model,slice);
        // Keep the drive:/tune_drive: label cache truthful even with no
        // clients attached, so a later power change resolves the right trx
        // (#4161). Only a slice *gaining* TX updates it; the losing edge
        // leaves the cache pointing at the outgoing slice for the brief
        // band-change gap, which is the value drive should still use.
        if (tx) m_lastTxTrx = trx;
        if (m_clients.isEmpty()) return;
        broadcast(QStringLiteral("tx_enable:%1,%2;")
                      .arg(trx).arg(tx ? "true" : "false"));
    });

    // Seed the TX-trx cache from current state — txSliceChanged only fires on
    // a change, so a slice that is already TX at wire time would never set it.
    if (slice->isTxSlice()) {
        m_lastTxTrx = m_trxMap.trxForSlice(m_model, slice);
    }

    connect(slice, &SliceModel::lockedChanged, this, [this, slice](bool locked) {
        if (m_clients.isEmpty()) return;
        const int trx = m_trxMap.trxForSlice(m_model,slice);
        broadcast(QStringLiteral("lock:%1,%2;")
                      .arg(trx).arg(locked ? "true" : "false"));
    });

    // GUI focus → `active_slice:trx;` broadcast (#4160). Control surfaces
    // (Elgato / StreamController / Ulanzi) otherwise hardcode trx 0 and every
    // dial keeps addressing slice A no matter what the operator selected.
    //
    // Only the true edge is relayed. A slice losing focus also emits
    // activeChanged(false), and the gaining slice's true edge is the
    // authoritative event — relaying the false edge would emit a second,
    // wrong active_slice for the outgoing trx.
    //
    // The focused slice is remembered by identity, not by trx: trx is
    // positional, so a later slice removal renumbers it (see
    // publishActiveTrx()).
    connect(slice, &SliceModel::activeChanged, this, [this, slice](bool active) {
        if (!active) return;
        m_activeSlice = slice;
        publishActiveTrx();
    });

    // The radio can relabel a slice without focus moving (MultiFlex
    // reassignment, #2606). Re-announce so a controller showing "Slice A"
    // does not keep showing it after the radio calls it something else.
    connect(slice, &SliceModel::letterChanged, this, [this, slice](const QString&) {
        if (slice != m_activeSlice) return;
        publishActiveTrx();
    });

    // Seed from current state — the activeChanged edge above is not enough.
    // RadioModel decodes the radio's slice status (applying active=1, which
    // emits activeChanged) BEFORE it emits sliceAdded, and sliceAdded is what
    // triggers this wiring. So for every newly added slice the focus edge has
    // already fired by the time we connect, and nothing re-fires it: adding a
    // slice made it active in the GUI while TCI kept reporting the old one.
    if (slice->isActive()) {
        m_activeSlice = slice;
        publishActiveTrx();
    }

    // Per-slice audioGain → `rx_volume:trx,N;` broadcast. Without this,
    // a GUI change to a slice's audio level was invisible to TCI clients;
    // remote controllers would drift out of sync. Part of issue #1764 fix.
    connect(slice, &SliceModel::audioGainChanged, this, [this, slice](float gain) {
        if (m_clients.isEmpty()) return;
        const int trx = m_trxMap.trxForSlice(m_model, slice);
        broadcast(QStringLiteral("rx_volume:%1,%2;")
                      .arg(trx).arg(static_cast<int>(gain)));
    });

    // DSP / squelch / RIT / XIT flags → per-slice broadcasts (#4161). These
    // had no signal wiring at all, so a flag toggled in AetherSDR's own GUI
    // was invisible to every TCI client, and the client that sent the SET was
    // never told the radio accepted it (the command-echo path excludes the
    // sender).
    //
    // Each relay is a change handler that de-dups repeats, plus a seed that
    // announces the current state after a (re)wire. Both share one baseline
    // (`last`, starting "unsent") so a value is never announced twice. The
    // de-dup is needed because SliceModel's emit discipline is uneven —
    // nb/nr/anf/squelch/rit/xit re-emit on every status refresh whether or not
    // the value moved, while apf/audioMute guard — and squelchChanged/
    // ritChanged/xitChanged carry (flag, value), so spinning a RIT offset would
    // otherwise re-announce an unchanged rit_enable on every step. The trailing
    // int on those three signals is simply dropped: Qt binds a 1-arg slot to a
    // 2-arg signal, so one helper serves both shapes (#4161 is scoped to the
    // *_enable family; sql_level/rit_offset/xit_offset are out of scope).
    //
    // The seed is DEFERRED ~400 ms and reads the *settled* value, exactly like
    // the frequency push below and for the same reason: a Flex band change
    // recreates the slice, and RadioModel decodes the radio's slice status
    // BEFORE it emits sliceAdded (the signal that triggers this wiring), so at
    // wire time the recreated slice still holds pre-settle DSP state. An
    // immediate seed would broadcast that stale value, then the radio's restore
    // (~250-340 ms later) would broadcast the corrected one — flapping every
    // flag on every band change. Deferring past the settle window announces
    // exactly the settled value: if a restore edge lands inside the window the
    // handler announces it and the seed de-dups; if the new band's value equals
    // the recreated default no edge fires and the seed is what announces it (the
    // per-flag analog of the #2824 vfo: case handled by the frequency push).
    //
    // The seed no-ops before any client connects (slices are wired at startup);
    // a client connecting later gets this state from the init burst. QPointer
    // guards a rapid band change that destroys the slice before the timer fires.
    auto emitFlag = [this](SliceModel* s, const char* cmd, bool on) {
        if (m_clients.isEmpty()) {
            return;
        }
        const int trx = m_trxMap.trxForSlice(m_model, s);
        broadcast(QStringLiteral("%1:%2,%3;")
                      .arg(QLatin1String(cmd)).arg(trx)
                      .arg(on ? "true" : "false"));
    };

    auto wireFlag = [this, slice, emitFlag](auto signal, const char* cmd,
                                            std::function<bool()> read) {
        auto last = std::make_shared<int>(-1);  // -1 = nothing announced yet
        // Change handler. A 1-arg slot binds both bool and (bool,int) signals.
        connect(slice, signal, this, [slice, cmd, emitFlag, last](bool on) {
            const int v = on ? 1 : 0;
            if (*last == v) {
                return;
            }
            *last = v;
            emitFlag(slice, cmd, on);
        });
        // Deferred settled seed, sharing `last` so it can't double-announce.
        QPointer<SliceModel> guard(slice);
        QTimer::singleShot(400, this, [this, guard, cmd, emitFlag, last, read]() {
            if (!guard || m_clients.isEmpty()) {
                return;
            }
            const bool on = read();
            const int v = on ? 1 : 0;
            if (*last == v) {
                return;
            }
            *last = v;
            emitFlag(guard, cmd, on);
        });
    };

    wireFlag(&SliceModel::nbChanged,        "rx_nb_enable",  [slice]{ return slice->nbOn(); });
    wireFlag(&SliceModel::nrChanged,        "rx_nr_enable",  [slice]{ return slice->nrOn(); });
    wireFlag(&SliceModel::anfChanged,       "rx_anf_enable", [slice]{ return slice->anfOn(); });
    wireFlag(&SliceModel::apfChanged,       "rx_apf_enable", [slice]{ return slice->apfOn(); });
    wireFlag(&SliceModel::audioMuteChanged, "mute",          [slice]{ return slice->audioMute(); });

    // squelch/rit/xit emit (flag, value); the value is dropped (see above).
    // sql_enable keeps a known KiwiSDR-only quirk: three squelch sources are in
    // play and diverge ONLY when m_externalReceiveAudioReplacement is set — the
    // init burst and this seed report receiveSquelchOn() (effective), while
    // squelchChanged carries squelchOn() (Flex-side). In that mode the seed and
    // the first edge can disagree, producing one spurious sql_enable edge on
    // connect; in normal mode all three are equal. Left as-is deliberately: a
    // real fix aligns all three sources and can only be verified with a KiwiSDR
    // RX source, out of this change's *_enable scope. (The band-change transient
    // that used to compound this is gone now the seed is deferred and settled.)
    wireFlag(&SliceModel::squelchChanged, "sql_enable", [slice]{ return slice->receiveSquelchOn(); });
    wireFlag(&SliceModel::ritChanged,     "rit_enable", [slice]{ return slice->ritOn(); });
    wireFlag(&SliceModel::xitChanged,     "xit_enable", [slice]{ return slice->xitOn(); });

    // State sync on (re)wire, deferred. A Flex band change (display pan set
    // band=) tears down and recreates the slice, so wireSlice() runs again for
    // the new slice. The handlers above only fire on *subsequent* changes; if
    // the radio's restored band frequency equals the recreated slice's init
    // value no frequencyChanged fires and the new band's vfo: is never
    // announced to TCI clients (silent for 160/80/60/17/10m; #2824).
    //
    // Pushing immediately is wrong: the recreated slice briefly holds an
    // intermediate frequency before the radio restores the band-stack value
    // (slices settle in ~250-340 ms observed), so an immediate push emits a
    // transient wrong vfo:. Defer ~400 ms and read the *settled* frequency so
    // every band announces exactly one correct vfo:. QPointer guards rapid
    // band changes that destroy the slice before the timer fires (the new
    // slice schedules its own deferred push, so the final band still wins).
    QPointer<SliceModel> guard(slice);
    QTimer::singleShot(400, this, [this, guard]() {
        if (!guard || m_clients.isEmpty()) return;
        SliceModel* s = guard;
        broadcastSliceFrequencies(s);
    });
}

// ── Wire spot click notifications ───────────────────────────────────────

SliceModel* TciServer::sliceForPanId(const QString& panId) const
{
    if (!m_model || panId.isEmpty())
        return nullptr;

    for (auto* slice : m_model->slices()) {
        if (slice && slice->panId() == panId)
            return slice;
    }

    return nullptr;
}

void TciServer::broadcastSpotClicked(const QString& callsign, long long frequencyHz,
                                     int trx, int channel)
{
    if (m_clients.isEmpty())
        return;

    const QString call = callsign.trimmed();
    if (call.isEmpty() || frequencyHz <= 0)
        return;

    const int safeTrx = std::max(0, trx);
    const int safeChannel = std::max(0, channel);

    // TCI v2 clients may listen for the receiver-qualified form; older
    // Log4OM-style clients commonly use the original two-field message.
    broadcast(QStringLiteral("clicked_on_spot:%1,%2;")
                  .arg(call)
                  .arg(frequencyHz));
    broadcast(QStringLiteral("rx_clicked_on_spot:%1,%2,%3,%4;")
                  .arg(safeTrx)
                  .arg(safeChannel)
                  .arg(call)
                  .arg(frequencyHz));
}

void TciServer::notifySpotClicked(int spotIndex, SliceModel* slice)
{
    if (!m_model)
        return;

    const auto& spots = m_model->spotModel().spots();
    auto it = spots.find(spotIndex);
    if (it == spots.end())
        return;

    SliceModel* resolvedSlice = slice;
    if (!resolvedSlice) {
        // The MainWindow caller always passes a non-null slice; the only path
        // that reaches the fallback is wireSpotModel's sliceForPanId(panId)
        // lookup returning nullptr. In a multi-pan setup that mis-attributes
        // the rx_clicked_on_spot: trx field to slice index 0 — exactly the
        // kind of bug invisible in single-slice testing (#3152).
        const auto slices = m_model->slices();
        if (!slices.isEmpty()) {
            resolvedSlice = slices.first();
            qCWarning(lcCat) << "TciServer::notifySpotClicked falling back to "
                                "first slice; panId lookup failed for spot"
                             << spotIndex;
        }
    }

    const int trx = m_trxMap.trxForSlice(m_model, resolvedSlice);
    const long long hz = static_cast<long long>(std::round(it->rxFreqMhz * 1e6));
    broadcastSpotClicked(it->callsign, hz, trx, 0);
}

void TciServer::wireSpotModel()
{
    if (!m_model) return;
    connect(&m_model->spotModel(), &SpotModel::spotTriggered,
            this, [this](int index, const QString& panId) {
        notifySpotClicked(index, sliceForPanId(panId));
    });
}

void TciServer::sendInitBurst(QWebSocket* client)
{
    if (!client || !m_model) return;

    // Find protocol for this client to generate init burst
    TciProtocol* protocol = nullptr;
    for (auto& cs : m_clients) {
        if (cs.socket == client) { protocol = cs.protocol; break; }
    }
    if (!protocol) return;

    QStringList receiverMap;
    const auto slices = m_model->slices();
    for (auto* s : slices) {
        receiverMap << QStringLiteral("trx%1=slice%2/dax%3")
                           .arg(m_trxMap.trxForSlice(m_model,s))
                           .arg(s->sliceId())
                           .arg(s->daxChannel());
    }
    qCDebug(lcCat).noquote()
        << "TCI: receiver map"
        << (receiverMap.isEmpty() ? QStringLiteral("(none)") : receiverMap.join(QLatin1Char(' ')));

    // TCI protocol requires one command per WebSocket message.
    // Split the concatenated burst into individual messages.
    QString burst = protocol->generateInitBurst();
    const auto commands = burst.split(';', Qt::SkipEmptyParts);
    for (const auto& cmd : commands) {
        // DIAG: log each init-burst command — the startup vfo:/dds: here is what
        // WSJT-X reconciles against on connect; a wrong/late one explains the
        // "TCI failed set rxfreq" some users hit right at WSJT-X startup.
        qCDebug(lcCat).noquote() << "TCI tx→init:" << (cmd + QLatin1Char(';'));
        client->sendTextMessage(cmd + ';');
    }
    qCDebug(lcCat) << "TCI: sent init burst," << commands.size() << "commands";
}

void TciServer::replyText(QWebSocket* ws, const QString& msg)
{
    if (!ws) return;
    // DIAG: per-command echoes (audio_*, vfo:, etc.) bypass the dispatch log
    // at the top of onTextMessage via their early `continue`; log them here so
    // every command's response is visible when chasing CAT timeouts (#tci-diag).
    qCDebug(lcCat).noquote() << "TCI tx→client:" << msg.trimmed();
    ws->sendTextMessage(msg);
}

void TciServer::broadcast(const QString& msg)
{
    // DIAG: every async broadcast (vfo:/trx:/modulation:/lock:/rx_smeter:…).
    // The vfo: echo here is exactly what WSJT-X's do_frequency() waits ≤2s on
    // before it throws "TCI failed set rxfreq" and drops the socket.
    qCDebug(lcCat).noquote() << "TCI tx→all:" << msg.trimmed();
    for (auto& cs : m_clients)
        cs.socket->sendTextMessage(msg);
    emit tciMessage(QStringLiteral("tx"), msg);
}

void TciServer::broadcastBinary(const QByteArray& data)
{
    for (auto& cs : m_clients) {
        if (cs.audioEnabled)
            cs.socket->sendBinaryMessage(data);
    }
}

void TciServer::prepareTxAudio()
{
    if (m_txAudioPrepared) {
        return;
    }
    m_txAudioPrepared = true;

    // TCI always uses the radio-native DAX TX route (dax=1, int16 mono).
    // The legacy DaxTxLowLatency AppSettings key is retired — its only
    // real consumer was RADE mode, which now controls the route itself
    // via AudioEngine::setRadeMode().  Leaving TCI's route here unconditional
    // guarantees every WSJT-X / digital-mode client keeps the path that
    // works on firmware v4.1.5.
    m_txUseRadioRoute = true;
    // TCI has its own TX gain (decoupled from DaxTxGain) so users who split
    // DAX and TCI routing get independent slider control.  On first read,
    // copy DaxTxGain into TciTxGain so upgrading users see no behavior
    // change — later the DAX/TCI applet split supplies separate UI.
    auto& txGainSettings = AppSettings::instance();
    if (!txGainSettings.contains("TciTxGain")) {
        const QString legacy = txGainSettings.value("DaxTxGain", "0.5").toString();
        txGainSettings.setValue("TciTxGain", legacy);
        txGainSettings.save();
    }
    m_txGain = std::clamp(
        txGainSettings.value("TciTxGain", "0.5").toString().toFloat(),
        0.0f, 1.0f);
    {
        bool ok = false;
        int rawMode = txGainSettings.value("TciTxOverflowMode", "0").toString().toInt(&ok);
        if (!ok || rawMode < 0 || rawMode > 2) rawMode = 0;
        m_overflowMode = static_cast<OverflowMode>(rawMode);
    }
    m_txChronoAccumNs = 0;
    m_txChronoRequestedFrames = 0;
    m_txAudioBlocks = 0;
    m_txInputFrames = 0;
    m_txOutputFrames = 0;
    m_txClipSamples = 0;
    m_txAudioSampleCount = 0;
    m_txAudioSumSq = 0.0;
    m_txAudioPeak = 0.0f;
    m_txSawDuplicatedStereo = false;

    // TCI always routes through the radio-native DAX stream (int16 mono,
    // PCC 0x0123) — matches the dax=1 command sent below.
    if (m_audio) {
        m_audio->setDaxTxUseRadioRoute(m_txUseRadioRoute);
        m_audio->setDaxTxMode(true);
    }

    // Create the TX resampler with the 48 kHz default (WSJT-X). onBinaryMessage
    // re-derives the source rate from each frame's hdr.sampleRate and rebuilds
    // this if a client transmits at a non-48k negotiated rate (#3306).
    m_txResampler = std::make_unique<Resampler>(48000.0, 24000.0, 4096);
    // The DAX TX stream is how a Flex radio is told to modulate from the
    // network instead of its mic jack. A host-modulating backend (HL2) has no
    // such stream and no such command set: its modulator is AudioEngine's, so
    // arranging a radio-side route here would send Flex text at a radio that
    // does not speak it. AudioEngine::feedDaxTxAudio routes to the local
    // modulator on that backend and never reaches the VITA-49 packetizers.
    if (m_model && !hostModulatingBackend()) {
        // Always dax=1 for TCI TX. The DaxTxLowLatency flag only controls
        // VITA-49 packet format (PCC 0x03E3 vs 0x0123 in feedDaxTxAudio);
        // both formats require dax=1 so the radio routes the dax_tx stream
        // to the modulator. Sending dax=0 keeps the radio on the physical
        // mic and silently discards every dax_tx packet. — fw v1.4.0.0
        m_model->ensureDaxTxStream(DaxTxRequestReason::TciTxAudio);
        m_model->sendCmdPublic("transmit set dax=1", nullptr);
    }
}

void TciServer::requestTciPttOff()
{
    if (!m_model) {
        return;
    }
    if (m_tciPttWantsAudio) {
        m_model->setTransmit(false, TransmitModel::PttSource::Dax);
    } else {
        m_model->transmitModel().requestPttOff(
            TransmitModel::PttSource::TciHardware);
    }
}

void TciServer::abortTciPtt()
{
    const bool hadSession = m_tciPttClient || m_tciPttRequestedOn || m_tciPttConfirmedOn;
    const bool pendingKeyUp = m_tciPttRequestedOn && !m_tciPttConfirmedOn;
    ++m_tciPttGeneration;
    const quint64 generation = m_tciPttGeneration;
    m_tciPttRequestedOn = false;
    m_tciPttCancelPending = m_tciPttCancelPending || pendingKeyUp;

    // Teardown paths fail closed and bypass optional PTT outro delays.
    if (m_model && hadSession) {
        m_model->setTransmit(false,
            m_tciPttWantsAudio ? TransmitModel::PttSource::Dax
                               : TransmitModel::PttSource::TciHardware);
    }
    stopTxChrono();
    m_tciPttConfirmedOn = false;
    m_tciPttWantsAudio = false;
    m_tciPttClient.clear();

    if (pendingKeyUp) {
        QPointer<TciServer> self(this);
        QTimer::singleShot(1250, this, [self, generation]() {
            if (!self || generation != self->m_tciPttGeneration
                || !self->m_tciPttCancelPending) {
                return;
            }
            self->m_tciPttCancelPending = false;
            if (self->m_model && self->m_model->isRadioTransmitting()) {
                // The fail-closed unkey did not settle inside the bounded
                // barrier. Resume reporting the authoritative radio state;
                // routing remains blocked by raw TX until the radio unkeys.
                self->broadcastActualTxState(true);
            }
            self->drainDeferredRoutingAndPtt();
        });
    }
}

void TciServer::startTxChrono(QWebSocket* client, int trx)
{
    if (!client) {
        return;
    }
    if (m_txChronoClient == client && m_txChronoTimer->isActive()) {
        return;
    }
    if (m_txChronoClient) {
        stopTxChrono();
    }
    prepareTxAudio();
    m_txChronoClient = client;
    m_txChronoTrx = trx;

    m_txChronoClock.start();
    m_txChronoSessionClock.start();
    m_txChronoTimer->start();
    sendTxChronoFrame(client);
    qCInfo(lcCat) << "TCI: TX_CHRONO started for TRX" << trx
                  << "route=" << (m_txUseRadioRoute ? "radio-dax" : "dax-tx-f32")
                  << "gain=" << m_txGain
                  << "poll_ms=" << kTxChronoPollMs
                  << "target_ms=" << (static_cast<double>(kTxChronoPeriodNs) / 1.0e6);
}

void TciServer::stopTxChrono()
{
    if (!m_txChronoTimer->isActive() && !m_txChronoClient && !m_txAudioPrepared) {
        return;
    }

    logTxAudioSummary("stop");
    m_txChronoTimer->stop();
    m_txChronoClient = nullptr;
    m_txChronoClock.invalidate();
    m_txChronoSessionClock.invalidate();
    m_txChronoAccumNs = 0;

    // Do NOT send `transmit set dax=0` here. The radio's status echo
    // flips m_daxTxMode to false via updateDaxTxMode, which blocks the
    // feedDaxTxAudio gate on the next TX cycle. Leave dax=1 active;
    // voice TX will override when needed. — fw v1.4.0.0
    if (m_audio) {
        m_audio->setDaxTxMode(false);
    }

    m_txResampler.reset();
    m_txAudioPrepared = false;

    qCInfo(lcCat) << "TCI: TX_CHRONO stopped";
}

void TciServer::sendTxChronoFrame(QWebSocket* client)
{
    if (!client) return;

    // TX_CHRONO: header-only, no payload (matches Thetis).
    QByteArray frame(sizeof(TciAudioHeader), '\0');
    TciAudioHeader hdr{};
    hdr.receiver   = static_cast<quint32>(m_txChronoTrx);
    hdr.sampleRate = 48000;
    hdr.format     = 3;                // float32
    hdr.length     = kTxChronoSamples; // matches audio_stream_samples
    hdr.type       = 3;                // TX_CHRONO
    hdr.channels   = 2;
    std::memcpy(frame.data(), &hdr, sizeof(hdr));
    client->sendBinaryMessage(frame);
    m_txChronoRequestedFrames += kTxChronoStereoFrames;
}

void TciServer::logTxAudioSummary(const char* reason)
{
    if (m_txChronoRequestedFrames <= 0 && m_txAudioBlocks <= 0)
        return;

    const double elapsedSec = m_txChronoSessionClock.isValid()
        ? static_cast<double>(m_txChronoSessionClock.nsecsElapsed()) / 1.0e9
        : 0.0;
    const double effectiveRate48k = elapsedSec > 0.0
        ? static_cast<double>(m_txChronoRequestedFrames) / elapsedSec
        : 0.0;
    const double rms = m_txAudioSampleCount > 0
        ? std::sqrt(m_txAudioSumSq / static_cast<double>(m_txAudioSampleCount))
        : 0.0;

    qCInfo(lcCat).nospace()
        << "TCI TX summary reason=" << reason
        << " trx=" << m_txChronoTrx
        << " route=" << (m_txUseRadioRoute ? "radio-dax" : "dax-tx-f32")
        << " gain=" << m_txGain
        << " blocks=" << m_txAudioBlocks
        << " requested48k=" << m_txChronoRequestedFrames
        << " inputFramesSrc=" << m_txInputFrames
        << " output24k=" << m_txOutputFrames
        << " effective48k=" << effectiveRate48k
        << " peak=" << m_txAudioPeak
        << " rms=" << rms
        << " clips=" << m_txClipSamples
        << " layout=" << (m_txSawDuplicatedStereo ? "duplicated-stereo" : "mono-or-stereo");
}

void TciServer::broadcastActualTxState(bool transmitting)
{
    if (!m_model) {
        return;
    }
    SliceModel* txSlice = m_model->txSlice();
    int trx = m_tciPttClient ? m_tciPttTrx : m_trxMap.trxForSlice(m_model, txSlice);
    broadcast(QStringLiteral("trx:%1,%2;").arg(trx).arg(transmitting ? "true" : "false"));
    if (transmitting && txSlice) {
        broadcast(
            QStringLiteral("tx_frequency:%1;").arg(TciProtocol::mhzToHz(txSlice->frequency())));
    }
}

void TciServer::onRadioTransmittingChanged(bool transmitting)
{
    const bool changed = transmitting != m_lastRadioTx;
    m_lastRadioTx = transmitting;

    if (transmitting) {
        if (m_tciPttCancelPending) {
            // A cancelled key-up won the command race. Do not publish a
            // transient trx:true that clients could interpret as a new owner.
            // Force the radio back to RX and wait for that authoritative edge.
            if (m_model) {
                m_model->setTransmit(false, TransmitModel::PttSource::TciHardware);
            }
            return;
        }
        if (m_tciPttClient && m_tciPttRequestedOn) {
            m_tciPttConfirmedOn = true;
            m_tciPttRequestedOn = false;
            ++m_tciPttGeneration;
            broadcastActualTxState(true);
            if (m_tciPttWantsAudio) {
                startTxChrono(m_tciPttClient, m_tciPttTrx);
            }
            return;
        }
        if (changed) {
            broadcastActualTxState(true);
        }
        return;
    }

    // Interlock REQUESTED/DELAY states are reported as not-yet-transmitting.
    // Keep a pending explicit TCI key request alive until the confirmation
    // timeout rather than treating an intermediate state as a rejection.
    if (m_tciPttRequestedOn && !m_tciPttConfirmedOn) {
        return;
    }
    const bool cancelledLateKeyUp = m_tciPttCancelPending;
    if (cancelledLateKeyUp) {
        m_tciPttCancelPending = false;
        ++m_tciPttGeneration;
    }
    if ((!cancelledLateKeyUp && changed) || m_tciPttClient) {
        broadcastActualTxState(false);
    }
    if (m_tciPttClient) {
        ++m_tciPttGeneration;
        m_tciPttClient.clear();
        m_tciPttConfirmedOn = false;
        m_tciPttWantsAudio = false;
        stopTxChrono();
    }
    drainDeferredRoutingAndPtt();
}

void TciServer::teardownTciRoute()
{
    if (!m_model) {
        m_routingState.reset();
        return;
    }
    if (m_tciPttClient || m_tciPttRequestedOn || m_tciPttConfirmedOn) {
        abortTciPtt();
    }

    if (!m_routingState.ownsRoute()) {
        m_routingState.reset();
        return;
    }
    const int rxSliceId = m_routingState.rxSliceId();
    const int txSliceId = m_routingState.txSliceId();
    const bool removeCreated = m_routingState.owner() == TciRoutingState::TxRouteOwner::TciCreated;
    m_routingState.reset();

    if (rxSliceId < 0 || !m_model->slice(rxSliceId)) {
        return;
    }
    QPointer<TciServer> self(this);
    promoteTxSliceAndContinue(rxSliceId, [self, txSliceId, removeCreated](bool selected) {
        if (self && selected && removeCreated && self->m_model && txSliceId >= 0) {
            self->m_model->sendCommand(QStringLiteral("slice remove %1").arg(txSliceId));
        }
    });
}

void TciServer::broadcastStatus()
{
    if (m_clients.isEmpty() || !m_model || !m_model->isConnected())
        return;

    // Broadcast S-meter for each owned slice (throttled to 200ms)
    // TCI spec: rx_smeter:receiver,value; (2 args)
    for (auto* s : m_model->slices()) {
        const int trx = m_trxMap.trxForSlice(m_model,s);
        const int meterIndex = s->sliceId();
        if (trx >= 0 && meterIndex >= 0 && meterIndex < 8) {
            float dbm = m_cachedSLevel[meterIndex];
            if (dbm > -200.0f)
                broadcast(QStringLiteral("rx_smeter:%1,%2;")
                              .arg(trx).arg(static_cast<int>(dbm)));
        }
    }

    // Broadcast RX/TX sensor telemetry to clients that enabled them
    for (auto& cs : m_clients) {
        if (cs.rxSensorsEnabled) {
            for (auto* s : m_model->slices()) {
                const int trx = m_trxMap.trxForSlice(m_model,s);
                const int meterIndex = s->sliceId();
                if (trx >= 0 && meterIndex >= 0 && meterIndex < 8) {
                    float dbm = m_cachedSLevel[meterIndex];
                    if (dbm > -200.0f)
                        cs.socket->sendTextMessage(
                            QStringLiteral("rx_channel_sensors:%1,0,%2;")
                                .arg(trx).arg(dbm, 0, 'f', 1));
                }
            }
        }
        if (cs.txSensorsEnabled && m_model->transmitModel().isTransmitting()) {
            // tx_sensors:trx,mic_dbm,fwd_watts,peak_watts,swr,alc_dbfs
            // alc_dbfs (trailing field, AetherSDR extension) is the SW-ALC
            // peak; index-based parsers safely ignore the extra field.
            cs.socket->sendTextMessage(
                QStringLiteral("tx_sensors:0,%1,%2,%3,%4,%5;")
                    .arg(m_cachedMicLevel, 0, 'f', 1)
                    .arg(m_cachedFwdPower, 0, 'f', 1)
                    .arg(m_cachedFwdPower, 0, 'f', 1)  // peak ≈ avg for now
                    .arg(m_cachedSwr, 0, 'f', 1)
                    .arg(m_cachedAlc, 0, 'f', 1));
        }
    }
}

// ── IQ data from DAX IQ stream → TCI binary frames (type=0) ───────────

void TciServer::onDaxStreamUnregistered(int channel, quint32 /*streamId*/)
{
    // The DAX channel's radio-side stream went away; drop its stale channel→TRX
    // routing-cache entry so a re-registration re-resolves cleanly (#3669/#3766).
    m_channelTrx.remove(channel);
}

void TciServer::onIqDataReady(int channel, const QByteArray& rawPayload, int sampleRate)
{
    // Check if any client wants IQ for this channel
    bool anyIq = false;
    int trx = channel - 1;  // DAX IQ channel 1 → TRX 0
    for (const auto& cs : m_clients) {
        if (cs.iqEnabled && cs.iqChannel == trx) { anyIq = true; break; }
    }
    if (!anyIq) return;

    // dax_iq payloads are LITTLE-endian float32 (the radio reports
    // payload_endian=little for this stream type, unlike pan/wf/meter/audio
    // which are big-endian network order). Reading them big-endian byte-reverses
    // every float into a denormal ≈ 0, so the skimmer (SDC / CW Skimmer) sees a
    // dead, flat IQ stream. Read little-endian to native (a no-op on an LE host),
    // matching DaxIqModel::feedRawIqPacket's handling of the same payload.
    const int numFloats = rawPayload.size() / 4;
    QByteArray swapped(rawPayload.size(), Qt::Uninitialized);
    const quint32* src = reinterpret_cast<const quint32*>(rawPayload.constData());
    quint32* dst = reinterpret_cast<quint32*>(swapped.data());
    for (int i = 0; i < numFloats; ++i)
        dst[i] = qFromLittleEndian(src[i]);

    // Build TCI IQ binary frame (type=0, channels=2 for I/Q pair)
    const int iqFrames = numFloats / 2;  // I/Q pairs
    QByteArray frame = buildAudioFrame(trx, 0 /*IQ*/, sampleRate, 2,
                                       reinterpret_cast<const float*>(swapped.constData()),
                                       iqFrames);

    for (auto& cs : m_clients) {
        if (cs.iqEnabled && cs.iqChannel == trx)
            cs.socket->sendBinaryMessage(frame);
    }
}

// ── Waterfall row → TCI binary spectrum frames (type=4) ──────────────────────

void TciServer::onWaterfallRowReady(quint32 streamId, const QVector<float>& binsDbm,
                                    double lowMhz, double highMhz,
                                    quint32 timecode, qint64 emittedNs)
{
    Q_UNUSED(timecode); Q_UNUSED(emittedNs);

    bool anySpectrum = false;
    for (const auto& cs : m_clients) {
        if (cs.spectrumEnabled) { anySpectrum = true; break; }
    }
    if (!anySpectrum) return;

    const int nBins = binsDbm.size();
    if (nBins == 0) return;

    // Resolve waterfall streamId → TRX for multi-pan disambiguation.
    // Waterfall IDs are 0x42xx; each PanadapterModel knows its wfStreamId().
    int trx = 0;
    if (m_model) {
        for (auto* pan : m_model->panadapters()) {
            if (pan->wfStreamId() == streamId) {
                for (auto* s : m_model->slices()) {
                    if (s->panId() == pan->panId()) {
                        trx = m_trxMap.trxForSlice(m_model, s);
                        break;
                    }
                }
                break;
            }
        }
    }

    // TciAudioHeader (64 bytes) + float32 dBm bins.
    // type=4 (SPECTRUM, AetherSDR extension — not in TCI spec v2.0).
    // reserved[0] = low edge in Hz, reserved[1] = high edge in Hz.
    QByteArray frame(static_cast<int>(sizeof(TciAudioHeader)) + nBins * static_cast<int>(sizeof(float)),
                     Qt::Uninitialized);

    TciAudioHeader hdr{};
    hdr.receiver    = static_cast<quint32>(trx);
    hdr.format      = 3;      // float32
    hdr.length      = static_cast<quint32>(nBins);
    hdr.type        = 4;      // SPECTRUM (AetherSDR extension)
    hdr.channels    = 1;
    hdr.reserved[0] = static_cast<quint32>(lowMhz  * 1'000'000.0);
    hdr.reserved[1] = static_cast<quint32>(highMhz * 1'000'000.0);
    std::memcpy(frame.data(), &hdr, sizeof(hdr));

    auto* dst = reinterpret_cast<float*>(frame.data() + sizeof(hdr));
    std::memcpy(dst, binsDbm.constData(), nBins * sizeof(float));

    for (auto& cs : m_clients) {
        if (cs.spectrumEnabled)
            cs.socket->sendBinaryMessage(frame);
    }
}

// ── DAX channel management for TCI audio (#1331) ─────────────────────────────
//
// TCI audio feeds from daxAudioReady (not audioDataReady) so that audio_mute
// doesn't kill TCI audio. We auto-assign a DAX channel to each slice that
// doesn't already have one, and release it when the last TCI audio client
// disconnects.

// True when the connected backend demodulates and modulates in this process
// (Hermes-Lite 2) rather than inside the radio. Such a backend has no DAX /
// VITA-49 data plane at all, so every DAX arrangement in this file is not just
// unnecessary but actively wrong — it would push Flex slice/transmit text at a
// radio that speaks HPSDR. Capability, not a family-name test.
bool TciServer::hostModulatingBackend() const
{
    return m_model && m_model->backendCapabilities().hostModulates;
}

void TciServer::ensureDaxForTci()
{
    if (!m_model || !m_model->isConnected()) return;

    // In-process backend (HL2): there is no DAX plane to arrange. RX audio
    // reaches onDaxAudioReady() on channel 1 straight from the backend's
    // demodulator (MainWindow wires backendAudioFrameReady), and the
    // channel→TRX fallback there maps channel 1 to trx 0 — which is the whole
    // mapping on a single-slice radio. Assigning slice DAX channels here would
    // emit Flex `slice set … dax=` commands into a socket that ignores them.
    if (!m_model->panStream()) return;

    QSet<int> channelsNeeded;

    for (auto* s : m_model->slices()) {
        if (s->daxChannel() == 0) {
            // Slice has no DAX channel — auto-assign one
            QSet<int> used;
            for (auto* sl : m_model->slices()) {
                if (sl->daxChannel() > 0) {
                    used.insert(sl->daxChannel());
                }
            }
            for (int ch = 1; ch <= 8; ++ch) {
                if (!used.contains(ch)) {
                    qCDebug(lcCat) << "TCI: auto-assigning DAX channel" << ch
                                   << "to slice" << s->sliceId();
                    qCInfo(lcCat) << "TCI: auto-assigning DAX channel" << ch
                                  << "to slice" << s->sliceId() << "for TCI audio (#1331)";
                    s->setDaxChannel(ch);
                    m_tciDaxSlices.insert(s->sliceId());
                    channelsNeeded.insert(ch);
                    break;
                }
            }
        } else {
            // Slice already has a DAX channel (from radio profile) —
            // still need to ensure a stream exists for it.
            channelsNeeded.insert(s->daxChannel());
        }
    }

    // Acquire the needed channels from the centralized manager (#3305). It
    // creates the radio-side stream only when the channel gains its FIRST
    // holder — never a duplicate subscription (duplicate streams made
    // daxAudioReady fire twice per period, doubling apparent audio speed) —
    // and reuses anything the DAX bridge or a previous arm already created.
    // Acquire is idempotent, so re-arm paths can call this freely.
    //
    // The #1439 dax_clients re-assert is a one-shot in RadioModel tied to the
    // actual `stream create`. The unconditional re-assert that used to live
    // here re-asserted LIVE bindings, which the radio answers with a transient
    // unbind/rebind dax=0/dax=<ch> pair — the seed of the #4009 storm.
    if (m_model->panStream()) {
        for (int ch : channelsNeeded) {
            m_model->panStream()->acquireDaxChannel(
                ch, PanadapterStream::DaxConsumer::Tci);
        }
    }
}

void TciServer::scheduleDaxRelease()
{
    // Debounce the DAX RX teardown. A TCP client drop is frequently transient:
    // WSJT-X throws a rig-control error (e.g. a vfo: echo delayed past its 2s
    // timeout by an ATU tune, or a profile-load band change) and reconnects
    // within ~2s. Tearing DAX RX down immediately turns that blip into
    // permanent silence (#3363 / #3476 / Tune-ATU). Defer it; a reconnecting
    // client that re-arms audio cancels the timer (cancelDaxRelease()), so the
    // stream survives and audio resumes with no recreate. If the radio actually
    // destroyed the streams meanwhile (profile slice recreate), the centralized
    // manager's removed-status recovery re-creates them (#3305).
    if (!m_daxReleaseTimer) { releaseDaxForTci(); return; }
    qCWarning(lcCat) << "TCI: last audio client gone — deferring DAX RX release"
                     << kDaxReleaseGraceMs << "ms (cancelled if a client reconnects)";
    m_daxReleaseTimer->start(kDaxReleaseGraceMs);
}

void TciServer::cancelDaxRelease()
{
    if (m_daxReleaseTimer && m_daxReleaseTimer->isActive()) {
        m_daxReleaseTimer->stop();
        qCWarning(lcCat) << "TCI: audio client (re)armed — cancelled pending DAX RX release; stream kept alive";
    }
}

void TciServer::rearmDaxForProfileLoad()
{
    if (!m_model || !m_model->isConnected()) {
        return;
    }

    bool hasAudioClient = false;
    for (const auto& cs : m_clients) {
        if (cs.audioEnabled) {
            hasAudioClient = true;
            break;
        }
    }
    if (!hasAudioClient) {
        return;
    }

    // Streams the profile load destroyed radio-side are re-created
    // automatically by the DAX channel manager's removed-status recovery
    // (#3305/#3476); we only need to refresh the routing cache and re-run the
    // slice policy (idempotent acquires).
    m_channelTrx.clear();   // routing cache stale across a profile load (#3669)
    m_tciDaxSlices.clear();

    qCInfo(lcCat) << "TCI: profile load completed - re-arming DAX for active audio client";
    ensureDaxForTci();
}

void TciServer::releaseDaxForTci()
{
    if (!m_model) return;

    // DIAG (qCWarning so it survives default log levels): this is the path that
    // silences WSJT-X RX on a client disconnect / audio_stop. It ran invisibly
    // in the 26.6.2 repro because qCInfo(lcCat) is suppressed below warning.
    qCWarning(lcCat) << "TCI: releaseDaxForTci() releasing DAX RX —"
                     << m_tciDaxSlices.size() << "slice assignment(s);"
                     << "RX audio stops until the next audio_start re-arms it";

    // Release TCI's hold on every channel. The centralized manager removes a
    // radio-side stream only when the LAST holder releases (after a grace
    // window), so a channel the DAX bridge or RADE still uses survives — the
    // old "skip borrowed" bookkeeping, enforced structurally (#3305).
    if (m_model->panStream()) {
        m_model->panStream()->releaseAllDaxChannels(
            PanadapterStream::DaxConsumer::Tci);
    }
    m_channelTrx.clear();   // routing cache stale once the channel holds are dropped (#3669)

    // Release DAX channel assignments we made
    for (int sliceId : m_tciDaxSlices) {
        if (auto* s = m_model->slice(sliceId)) {
            qCWarning(lcCat) << "TCI: releasing DAX channel from slice" << sliceId << "(#1331)";
            s->setDaxChannel(0);
        }
    }
    m_tciDaxSlices.clear();
}

} // namespace AetherSDR

#endif // HAVE_WEBSOCKETS
