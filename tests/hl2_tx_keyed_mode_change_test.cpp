// While the HL2 is keyed, a change to the transmitter's mode, TX passband or
// TX slice is refused: the slice, the Phone applet and the transmitter keep
// what they had, and the operator is told why. Swapping TXA's bandpass
// mid-over (above all USB -> DIGU's minimum- to linear-phase swap) splatters,
// and a held change would leave the slice showing a mode the transmitter is
// not sending. Through the real Hl2Backend and its modulator on the I/O thread,
// socket-free, in the production lifecycle: setKeying() edges, a fresh TX
// operation and context per over, no explicit Hl2TxDsp::reset(). Real-time paced.
//   1. Keyed USB -> DIGU via SliceModel::setMode (the path the mode button,
//      CAT MD and TCI modulation share): refused, the slice stays USB, the
//      channel stays USB minimum phase, the over does not splatter.
//   2. Keyed TX filter change: refused, the echo puts the applet back. A
//      re-send of the pair the applet shows changes nothing and is not told.
//   3. Keyed TX-slice move to an LSB receiver: refused, transmit stays put.
//   4. After the over, an unkeyed USB -> DIGU reaches the channel at once, the
//      published TX passband follows, and the next over transmits DIGU.
//   5. A saved USB cut, then DIGU between overs: a keyed TX filter request is
//      refused and the echo is DIGU's published 150..3000, not the saved cut,
//      which still applies on return to USB.
//   6. Keyed close of the TX-owning pan: refused, transmit, its mode and both
//      receivers stay. Unkeyed, the same close goes through and transmit takes
//      the surviving receiver's mode.

#include "TestSettingsProfile.h"
#include "TxTestAuthority.h"
#include "core/backends/hl2/Hl2Backend.h"
#include "core/backends/hl2/Hl2TxDsp.h"
#include "core/backends/hl2/MetisClient.h"
#include "models/SliceModel.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace AetherSDR;
using namespace AetherSDR::hl2;
using Mode = WdspChannel::Mode;
using Iq = std::vector<std::complex<float>>;

namespace AetherSDR::hl2 {

struct MetisClientTestAccess {
    static void setStreaming(MetisClient& client) { client.m_running = true; }
};

struct Hl2TxKeyedChangeTestAccess {
    // Two receivers, the transmit gate open and a session without a socket.
    static void prepare(Hl2Backend& b)
    {
        b.m_ids.reset(2);
        b.m_rx.assign(2, Hl2Backend::Receiver{});
        b.m_txAllowed = true;
        QMetaObject::invokeMethod(b.m_metis, [m = b.m_metis] {
            m->enableTransmit(true);
            MetisClientTestAccess::setStreaming(*m);
        }, Qt::BlockingQueuedConnection);
    }
    static bool configureTx(Hl2Backend& b)
    {
        bool ok = false;
        QMetaObject::invokeMethod(b.m_txDsp, [&ok, dsp = b.m_txDsp] {
            Hl2TxDsp::Config cfg;
            cfg.mode = Mode::Usb;
            cfg.filterLowHz = 300.0;
            cfg.filterHighHz = 2700.0;
            cfg.alcEnabled = false;   // linear, so a level compares across channels
            std::string err;
            ok = dsp->configure(cfg, &err);
            dsp->setMicGain(1.0);
        }, Qt::BlockingQueuedConnection);
        return ok;
    }
    // Everything already posted to the I/O thread has run when this returns.
    static std::optional<WdspChannel::Config> channel(Hl2Backend& b)
    {
        std::optional<WdspChannel::Config> c;
        QMetaObject::invokeMethod(b.m_txDsp, [&c, dsp = b.m_txDsp] {
            if (const WdspChannel::Config* p = dsp->channelConfig()) {
                c = *p;
            }
        }, Qt::BlockingQueuedConnection);
        return c;
    }
    static Mode configMode(Hl2Backend& b)
    {
        Mode m = Mode::Usb;
        QMetaObject::invokeMethod(b.m_txDsp, [&m, dsp = b.m_txDsp] { m = dsp->config().mode; },
                                  Qt::BlockingQueuedConnection);
        return m;
    }
    static unsigned long long faults(Hl2Backend& b)
    {
        unsigned long long n = 0;
        QMetaObject::invokeMethod(b.m_txDsp, [&n, dsp = b.m_txDsp] { n = dsp->modulatorFaultBlocks(); },
                                  Qt::BlockingQueuedConnection);
        return n;
    }
    static Hl2TxDsp* txDsp(Hl2Backend& b) { return b.m_txDsp; }
    static int txDdc(const Hl2Backend& b) { return b.m_txDdc; }
    static int receivers(const Hl2Backend& b) { return static_cast<int>(b.m_rx.size()); }
    static QString panIdForDdc(const Hl2Backend& b, int ddc)
    {
        const auto* ids = b.m_ids.byDdc(ddc);
        return ids ? ids->panId : QString();
    }
};

}  // namespace AetherSDR::hl2

namespace {

using Access = Hl2TxKeyedChangeTestAccess;

int g_failures = 0;
void check(bool ok, const std::string& what)
{
    std::fprintf(stderr, "%s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) {
        ++g_failures;
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kFsIn = 24000.0;
constexpr double kFsOut = 48000.0;
constexpr std::size_t kChunk = 240;            // 10 ms at 24 kHz
constexpr std::size_t kChunks = 120;           // 1.2 s per over
constexpr std::size_t kEvent = 60;             // change before 0.6 s of input
constexpr long kMs = 48;                       // output samples per ms
// The output time at which a change could first show; the window is generous.
constexpr long kEventOut = static_cast<long>(kEvent * kChunk * 2);
constexpr std::size_t kSettle = 12288;         // key-up ramp and fill

void pumpFor(double ms)
{
    QElapsedTimer t;
    t.start();
    while (t.nsecsElapsed() < static_cast<qint64>(ms * 1e6)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// Interleaved stereo int16 chunks, as AudioEngine hands them to submitTxAudio().
std::vector<QByteArray> tone(double hz, double amp)
{
    std::vector<QByteArray> chunks;
    for (std::size_t k = 0; k < kChunks; ++k) {
        QByteArray pcm(static_cast<int>(kChunk * 2 * sizeof(qint16)), Qt::Uninitialized);
        auto* s = reinterpret_cast<qint16*>(pcm.data());
        for (std::size_t i = 0; i < kChunk; ++i) {
            const double n = static_cast<double>(k * kChunk + i);
            const auto v = static_cast<qint16>(
                std::lround(32767.0 * amp * std::sin(2.0 * kPi * hz * n / kFsIn)));
            s[2 * i] = v;
            s[2 * i + 1] = v;
        }
        chunks.push_back(pcm);
    }
    return chunks;
}

struct Over {
    Iq iq;
    bool starved = false;
};

// One over in the production lifecycle: a fresh operation and media context,
// setKeying(true), real-time audio, setKeying(false). The unkey's own cleanup
// (Hl2Backend::applyKeying) ends the over; nothing here resets the modulator.
Over runOver(Hl2Backend& backend, TxTestAuthority& auth, const TxCoordinator::Producer& mic,
             const std::vector<QByteArray>& audio, const std::function<void()>& event)
{
    Over o;
    std::mutex lock;
    const QMetaObject::Connection c = QObject::connect(Access::txDsp(backend), &Hl2TxDsp::iqReady,
        Access::txDsp(backend), [&o, &lock](const Iq& iq, const TxCoordinator::Context&) {
            const std::lock_guard<std::mutex> g(lock);
            o.iq.insert(o.iq.end(), iq.begin(), iq.end());
        }, Qt::DirectConnection);
    const TxCoordinator::Operation op =
        auth.coordinator.acquire(auth.actor, TxCoordinator::monotonicMs()).operation;
    const TxCoordinator::Context ctx = auth.coordinator.mediaContext(mic, op);
    const unsigned long long faults0 = Access::faults(backend);
    backend.setKeying(true, op, {});
    for (std::size_t k = 0; k < kChunks; ++k) {
        if (k == kEvent && event) {
            event();
        }
        backend.submitTxAudio(audio[k], 24000, TxAudioSource::Microphone, ctx);
        pumpFor(1000.0 * static_cast<double>(kChunk) / kFsIn);
    }
    pumpFor(50.0);
    o.starved = Access::faults(backend) > faults0;
    backend.setKeying(false, op, {});
    (void)Access::channel(backend);   // the unkey's posted cleanup has run
    (void)auth.coordinator.finishLocalIntent(op);
    QObject::disconnect(c);
    pumpFor(20.0);
    const std::lock_guard<std::mutex> g(lock);
    return o;
}

void fft(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Energy outside |f| in [100, 3200] Hz (either side: the union of USB's and
// DIGU's passbands with their skirts) over energy inside, in dB, 100 ms Hann
// window from `start`.
double oobDbc(const Iq& x, long start)
{
    const long len = 100 * kMs;
    std::size_t n = 1;
    while (n < static_cast<std::size_t>(len)) {
        n <<= 1;
    }
    std::vector<std::complex<double>> a(n, 0.0);
    for (long i = 0; i < len; ++i) {
        const long k = start + i;
        if (k >= 0 && k < static_cast<long>(x.size())) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / (len - 1));
            a[static_cast<std::size_t>(i)] = std::complex<double>(x[static_cast<std::size_t>(k)]) * w;
        }
    }
    fft(a);
    double in = 0.0;
    double out = 0.0;
    for (std::size_t k = 0; k < n; ++k) {
        const double f = (k <= n / 2 ? static_cast<double>(k) : static_cast<double>(k) - static_cast<double>(n))
            * kFsOut / static_cast<double>(n);
        const double af = std::fabs(f);
        ((af >= 100.0 && af <= 3200.0) ? in : out) += std::norm(a[k]);
    }
    return 10.0 * std::log10(std::max(out, 1e-300) / std::max(in, 1e-300));
}

// Worst 100 ms window from the change to 200 ms after it, 10 ms steps.
double worstOob(const Iq& x)
{
    double worst = -400.0;
    for (long w = kEventOut - 50 * kMs; w <= kEventOut + 150 * kMs; w += 10 * kMs) {
        worst = std::max(worst, oobDbc(x, w));
    }
    return worst;
}

double rms(const Iq& x, std::size_t from)
{
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < x.size(); ++i, ++n) {
        acc += std::norm(x[i]);
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

double db(double r) { return 20.0 * std::log10(std::max(r, 1e-30)); }

bool isUsbMinPhase(const std::optional<WdspChannel::Config>& c)
{
    return c && c->mode == Mode::Usb && c->minimumPhase && c->filterLowHz == 300.0
        && c->filterHighHz == 2700.0;
}

bool isDiguLinear(const std::optional<WdspChannel::Config>& c)
{
    return c && c->mode == Mode::Digu && !c->minimumPhase && c->filterLowHz == 150.0
        && c->filterHighHz == 3000.0;
}

std::optional<std::pair<double, double>> publishedTxPassband(Hl2Backend& backend)
{
    backend.settleDspChainsForTest();
    for (const QVariant& v : backend.dspChains()) {
        const QVariantMap e = v.toMap();
        if (e.value(QStringLiteral("chain")).toString() == QLatin1String("hl2-tx")) {
            return std::make_pair(e.value(QStringLiteral("filterLowHz")).toDouble(),
                                  e.value(QStringLiteral("filterHighHz")).toDouble());
        }
    }
    return std::nullopt;
}

// A fresh channel's level for `audio`: the reference for "transmits DIGU".
double freshLevel(Mode mode, double lo, double hi, const std::vector<QByteArray>& audio, bool& starved)
{
    TxTestAuthority auth;
    const TxCoordinator::Producer mic = auth.coordinator.registerProducer(true);
    Hl2TxDsp tx;
    Hl2TxDsp::Config cfg;
    cfg.mode = mode;
    cfg.filterLowHz = lo;
    cfg.filterHighHz = hi;
    cfg.alcEnabled = false;
    std::string err;
    if (!tx.configure(cfg, &err)) {
        check(false, "a reference modulator configures " + err);
        return 0.0;
    }
    tx.setMicGain(1.0);
    Iq iq;
    QObject::connect(&tx, &Hl2TxDsp::iqReady, &tx, [&iq](const Iq& x, const TxCoordinator::Context&) {
        iq.insert(iq.end(), x.begin(), x.end());
    });
    const TxCoordinator::Context ctx = auth.coordinator.mediaContext(mic, auth.operation);
    for (const QByteArray& pcm : audio) {
        const auto* s = reinterpret_cast<const qint16*>(pcm.constData());
        std::vector<float> mono(kChunk);
        for (std::size_t i = 0; i < kChunk; ++i) {
            mono[i] = static_cast<float>(s[2 * i]) / 32768.0f;
        }
        tx.processAudioBlock(mono, TxAudioSource::Microphone, ctx);
        pumpFor(1000.0 * static_cast<double>(kChunk) / kFsIn);
    }
    pumpFor(50.0);
    starved = starved || tx.modulatorFaultBlocks() > 0;
    return rms(iq, kSettle);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("hl2-tx-keyed-mode-change"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "Cannot create isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
#if !AETHER_HL2_TX_TXA
    std::fprintf(stderr, "SKIP: phasing build has no TXA channel\n");
    return 77;
#else
    const std::vector<QByteArray> t1k = tone(1000.0, 0.5);
    const std::vector<QByteArray> t200 = tone(200.0, 0.5);
    bool inconclusive = false;

    const double ref200Digu = freshLevel(Mode::Digu, 150.0, 3000.0, t200, inconclusive);
    const double ref200Usb = freshLevel(Mode::Usb, 300.0, 2700.0, t200, inconclusive);
    std::fprintf(stderr, "200 Hz tone: fresh DIGU %.3f dB, fresh USB %.3f dB\n",
                 db(ref200Digu), db(ref200Usb));
    check(db(ref200Digu) - db(ref200Usb) > 40.0,
          "a 200 Hz tone tells DIGU from USB (USB stops it, DIGU passes it)");

    TxTestAuthority auth;
    (void)auth.coordinator.finishLocalIntent(auth.operation);   // each over acquires its own
    const TxCoordinator::Producer mic = auth.coordinator.registerProducer(true);
    Hl2Backend backend;
    Access::prepare(backend);

    // Wired as RadioModel wires it: both connections direct.
    SliceModel slice(0);
    QObject::connect(&slice, &SliceModel::modeChangeRequested, &backend,
                     [&backend](const QString& m) { backend.setSliceMode(0, m); });
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &slice, [&slice](int id, const SliceDelta& d) {
        if (id == 0) {
            slice.applyChanges(d);
        }
    });
    QString lastAnnounced;
    QObject::connect(&slice, &SliceModel::modeChanged, &slice,
                     [&lastAnnounced](const QString& m) { lastAnnounced = m; });
    QStringList refusals;
    QObject::connect(&backend, &IRadioBackend::transmitChangeRefused, &backend,
                     [&refusals](int id, const QString& m) { refusals << QStringLiteral("%1:%2").arg(id).arg(m); });
    std::optional<std::pair<int, int>> txFilterEcho;
    QObject::connect(&backend, &IRadioBackend::transmitChanged, &backend, [&txFilterEcho](const TransmitDelta& d) {
        if (d.txFilterLow && d.txFilterHigh) {
            txFilterEcho = std::make_pair(*d.txFilterLow, *d.txFilterHigh);
        }
    });
    std::optional<bool> slice1Tx;
    QObject::connect(&backend, &IRadioBackend::sliceChanged, &backend, [&slice1Tx](int id, const SliceDelta& d) {
        if (id == 1 && d.txSlice) {
            slice1Tx = *d.txSlice;
        }
    });

    slice.setMode(QStringLiteral("USB"));
    backend.setSliceMode(1, QStringLiteral("LSB"));
    if (!Access::configureTx(backend)) {
        check(false, "the backend's modulator configures");
        return 1;
    }
    check(isUsbMinPhase(Access::channel(backend)),
          "setup: the TX channel runs USB, minimum phase, 300..2700 Hz");

    // Control over: nothing changes.
    const Over control = runOver(backend, auth, mic, t1k, {});
    inconclusive = inconclusive || control.starved;

    // 1-3. The keyed over: a mode change, a TX filter change and a TX-slice move.
    std::optional<WdspChannel::Config> duringOver;
    bool modeRefused = false;
    bool filterRefused = false;
    bool usbResendQuiet = false;
    bool moveRefused = false;
    bool completionRefused = false;
    QString completionText;
    int completionCode = 0;
    QStringList seen;
    const Over keyed = runOver(backend, auth, mic, t1k, [&] {
        refusals.clear();
        lastAnnounced.clear();
        slice.setMode(QStringLiteral("DIGU"));
        modeRefused = refusals == QStringList{QStringLiteral("0:Mode can't change while transmitting.")};
        seen << refusals;

        refusals.clear();
        txFilterEcho.reset();
        backend.setTxFilter(100, 2900);
        filterRefused = refusals == QStringList{QStringLiteral("0:TX filter can't change while transmitting.")}
            && txFilterEcho == std::make_pair(300, 2700);
        seen << refusals;

        // The pair the applet already shows, sent again (no operator cut yet).
        refusals.clear();
        txFilterEcho.reset();
        backend.setTxFilter(300, 2700);
        usbResendQuiet = refusals.isEmpty() && txFilterEcho == std::make_pair(300, 2700);

        refusals.clear();
        slice1Tx.reset();
        backend.requestTxSlice(1, true, [&](const RadioControlReply& r) {
            completionRefused = r.kind == RadioControlReply::Kind::Refused;
            completionText = r.message;
            completionCode = r.code;
        }, {});
        backend.setTxSlice(1);   // the requestSliceControl path
        moveRefused = refusals.size() == 2
            && refusals.at(0) == QLatin1String("1:TX slice can't change while transmitting.")
            && refusals.at(1) == refusals.at(0)
            && Access::txDdc(backend) == 0 && slice1Tx == false;
        seen << refusals;

        duringOver = Access::channel(backend);
    });
    inconclusive = inconclusive || keyed.starved;
    std::fprintf(stderr, "refusals told to the operator: %s\n", qPrintable(seen.join(QStringLiteral(" | "))));

    check(modeRefused, "a keyed USB -> DIGU is refused and the operator is told "
                       "\"Mode can't change while transmitting.\"");
    check(slice.mode() == QLatin1String("USB"), "the refused change leaves the slice in USB");
    check(lastAnnounced.isEmpty() || lastAnnounced == QLatin1String("USB"),
          "the last modeChanged after the refusal is not DIGU");
    check(isUsbMinPhase(duringOver) && Access::configMode(backend) == Mode::Usb,
          "the transmitter is unchanged while keyed: USB, minimum phase, 300..2700 Hz");
    check(filterRefused,
          "a keyed TX filter change is refused and told; the echo puts the applet back on 300..2700");
    check(usbResendQuiet, "a keyed re-send of the shown 300..2700 is not told as a refusal; the echo is unchanged");
    check(moveRefused, "a keyed TX-slice move is refused on both paths; transmit stays on slice 0");
    check(completionRefused && completionText.contains(QLatin1String("transmitting")),
          "the TX-slice move's completion is answered Refused");
    // #7589: TCI tells a keyed refusal from any other by this code, and asks
    // the declared record before it moves TX itself.
    check(completionCode == RadioControlReply::kRefusedWhileKeyed,
          "the keyed TX-slice refusal carries kRefusedWhileKeyed");
    const auto txSliceControl = backend.capabilities().txSliceControl;
    check(txSliceControl && txSliceControl->changeWhileKeyed
                                == RadioCapabilities::TxSliceControl::ChangeWhileKeyed::Refused,
          "the HL2 declares TxSliceControl::ChangeWhileKeyed::Refused");
    const double worstEv = worstOob(keyed.iq);
    const double worstCtl = worstOob(control.iq);
    std::fprintf(stderr, "keyed over: out-of-band %.1f dBc, unchanged over %.1f dBc\n", worstEv, worstCtl);
    check(worstEv - worstCtl <= 10.0,
          "the keyed over does not splatter (out-of-band within 10 dB of the unchanged over)");
    check(isUsbMinPhase(Access::channel(backend)),
          "after the over the channel is still USB: nothing was held for the unkey");

    // 4. Between overs the change applies at once.
    refusals.clear();
    slice.setMode(QStringLiteral("DIGU"));
    check(refusals.isEmpty() && slice.mode() == QLatin1String("DIGU"), "an unkeyed USB -> DIGU is accepted");
    check(isDiguLinear(Access::channel(backend)),
          "an unkeyed change after a prior over reaches the channel at once: DIGU, linear phase, 150..3000 Hz");
    const auto published = publishedTxPassband(backend);
    check(published && published->first == 150.0 && published->second == 3000.0,
          "the published TX passband follows at once: 150..3000 Hz");
    const Over next = runOver(backend, auth, mic, t200, {});
    inconclusive = inconclusive || next.starved;
    const double lvl = rms(next.iq, kSettle);
    std::fprintf(stderr, "next over, 200 Hz tone: %.3f dB (fresh DIGU %.3f dB)\n", db(lvl), db(ref200Digu));
    check(std::fabs(db(lvl) - db(ref200Digu)) < 0.5, "the next over transmits DIGU");

    // 5. A saved USB cut survives DIGU, and a keyed refusal echoes what DIGU
    //    publishes, not the saved cut.
    slice.setMode(QStringLiteral("USB"));
    backend.setTxFilter(200, 2800);
    const auto usbCut = publishedTxPassband(backend);
    check(usbCut && usbCut->first == 200.0 && usbCut->second == 2800.0,
          "setup: a custom USB cut 200..2800 Hz is published");
    slice.setMode(QStringLiteral("DIGU"));
    const auto diguPublished = publishedTxPassband(backend);
    check(diguPublished && diguPublished->first == 150.0 && diguPublished->second == 3000.0,
          "setup: unkeyed DIGU publishes 150..3000 Hz while the USB cut is kept");

    bool diguFilterRefused = false;
    bool diguResendQuiet = false;
    std::optional<std::pair<int, int>> diguEcho;
    // 6 (keyed half). The TX-owning pan is DDC 0; closing it would hand
    // transmit to the LSB receiver.
    const QString txPan = Access::panIdForDdc(backend, 0);
    QStringList removedPans;
    QObject::connect(&backend, &IRadioBackend::panRemoved, &backend,
                     [&removedPans](const QString& id) { removedPans << id; });
    bool closeReturned = true;
    bool closeRefusalTold = false;
    std::optional<WdspChannel::Config> afterKeyedClose;
    const Over diguOver = runOver(backend, auth, mic, t1k, [&] {
        refusals.clear();
        txFilterEcho.reset();
        backend.setTxFilter(100, 2900);
        diguEcho = txFilterEcho;
        diguFilterRefused = refusals == QStringList{QStringLiteral("0:TX filter can't change while transmitting.")};

        // DIGU's published pair sent again, with the saved USB cut differing.
        refusals.clear();
        txFilterEcho.reset();
        backend.setTxFilter(150, 3000);
        diguResendQuiet = refusals.isEmpty() && txFilterEcho == std::make_pair(150, 3000);

        refusals.clear();
        closeReturned = backend.removePanadapter(txPan);
        closeRefusalTold = refusals == QStringList{
            QStringLiteral("0:TX panadapter can't close while transmitting.")};
        afterKeyedClose = Access::channel(backend);
    });
    inconclusive = inconclusive || diguOver.starved;
    std::fprintf(stderr, "keyed DIGU filter request echo: %d..%d\n",
                 diguEcho ? diguEcho->first : -1, diguEcho ? diguEcho->second : -1);
    check(diguFilterRefused, "a keyed TX filter change in DIGU is refused and told");
    check(diguResendQuiet, "a keyed re-send of DIGU's shown 150..3000 is not told as a refusal, though the saved USB cut differs");
    check(diguEcho == std::make_pair(150, 3000),
          "the refusal echoes DIGU's published 150..3000, not the saved USB cut");
    const auto diguAfter = publishedTxPassband(backend);
    check(diguAfter && diguAfter->first == 150.0 && diguAfter->second == 3000.0,
          "the published TX passband is still 150..3000 after the refusal");

    std::fprintf(stderr, "keyed close of the TX pan %s: returned %d, receivers %d, txDdc %d\n",
                 qPrintable(txPan), closeReturned ? 1 : 0, Access::receivers(backend),
                 Access::txDdc(backend));
    check(!closeReturned && closeRefusalTold,
          "a keyed close of the TX-owning pan is refused and the operator is told");
    check(Access::receivers(backend) == 2 && removedPans.isEmpty() && Access::txDdc(backend) == 0,
          "the refused close removes nothing: both receivers stay and transmit stays on DDC 0");
    check(isDiguLinear(afterKeyedClose) && Access::configMode(backend) == Mode::Digu,
          "the refused close leaves the transmitter in DIGU, 150..3000 Hz");

    // Back to USB between overs: the saved cut applies again.
    slice.setMode(QStringLiteral("USB"));
    const auto usbAgain = publishedTxPassband(backend);
    check(usbAgain && usbAgain->first == 200.0 && usbAgain->second == 2800.0,
          "the saved USB cut 200..2800 is still applied on return to USB");

    // 6 (unkeyed half). The same close goes through between overs, and the
    // surviving LSB receiver takes transmit with its own mode.
    refusals.clear();
    const bool unkeyedClose = backend.removePanadapter(txPan);
    check(unkeyedClose && refusals.isEmpty() && Access::receivers(backend) == 1
              && removedPans == QStringList{txPan} && Access::txDdc(backend) == 0,
          "an unkeyed close of the TX-owning pan goes through; transmit moves to the survivor");
    const auto afterClose = Access::channel(backend);
    check(afterClose && afterClose->mode == Mode::Lsb && Access::configMode(backend) == Mode::Lsb,
          "after the unkeyed close the transmitter runs the surviving receiver's LSB");

    if (g_failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    if (inconclusive) {
        std::fprintf(stderr, "SKIP: the worker was starved (machine load); inconclusive\n");
        return 77;
    }
    std::fprintf(stderr, "all checks passed\n");
    return 0;
#endif
}
