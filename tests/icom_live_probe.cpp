// IcomCIV live probe — REQUIRES A REAL RADIO, so it is deliberately NOT
// registered with add_test().
//
//   cmake --build build --target icom_live_probe
//   ./build/icom_live_probe <host> <user> <password>
//
// Connects a real IcomSession to a real radio and reports what it actually
// finds. This exists because five questions about the IC-705 cannot be settled
// from any document (icom-oracle.md §12), and every one of them changes code:
//
//   1. The real scope frame rate over WLAN — decides whether setPanFrameRate
//      has anything to do.
//   2. Whether the scope keeps updating during transmit.
//   3. The true dBm offset of the 0..160 display scale, per band and preamp.
//   4. Whether front-panel span changes are echoed or must be polled.
//   5. What the radio does when a second client connects.
//
// This probe answers 1 and 4 directly, and gives 3 the S-meter cross-reference
// it needs. It NEVER transmits: there is no PTT command anywhere below, so 2
// and 5 remain for a session where someone is at the radio.

#include "core/backends/icom/IcomScope.h"
#include "core/backends/icom/IcomSession.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QHostInfo>
#include <QTimer>

#include <cstdio>
#include <map>

using namespace AetherSDR::icom;

namespace {

struct Probe {
    IcomSession session;
    ScopeDecoder scope;

    QElapsedTimer clock;
    int sweeps = 0;
    qint64 firstSweepMs = -1;
    double lastStartHz = 0;
    double lastEndHz = 0;
    ScopeMode lastMode = ScopeMode::Centre;

    std::map<std::uint8_t, int> meters;   // subcommand -> raw value
    bool everConnected = false;
    std::uint8_t reportedCivAddress = 0;
    std::uint64_t frequencyHz = 0;
    QString modeName;
    quint64 audioSamples = 0;
};

const char* modeLabel(ScopeMode m)
{
    switch (m) {
    case ScopeMode::Centre:  return "Centre";
    case ScopeMode::Fixed:   return "Fixed";
    case ScopeMode::ScrollC: return "Scroll-C";
    case ScopeMode::ScrollF: return "Scroll-F";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <host> <user> <password>\n", argv[0]);
        std::fprintf(stderr,
                     "\nOn the radio, first check:\n"
                     "  WLAN set > Remote settings > Network control  = ON\n"
                     "  Connectors > MOD Input > DATA MOD             = WLAN\n"
                     "  Connectors > CI-V > CI-V Address              = A4h\n");
        return 2;
    }

    const QString hostArg = QString::fromLocal8Bit(argv[1]);
    QHostAddress host(hostArg);
    if (host.isNull()) {
        const QHostInfo info = QHostInfo::fromName(hostArg);
        if (info.addresses().isEmpty()) {
            std::fprintf(stderr, "cannot resolve %s\n", argv[1]);
            return 2;
        }
        host = info.addresses().first();
    }

    auto* p = new Probe;
    p->clock.start();

    IcomSession::Params params;
    params.host = host;
    params.username = QString::fromLocal8Bit(argv[2]);
    params.password = QString::fromLocal8Bit(argv[3]);
    // RECEIVE ONLY. Negotiating no TX codec is a stronger guarantee than simply
    // not sending audio — this probe cannot key the radio even by accident.
    params.enableTx = false;

    QObject::connect(&p->session, &IcomSession::connected, &app, [p](const QString& name) {
        p->everConnected = true;
        std::printf("connected to \"%s\" after %lld ms\n", name.toLocal8Bit().constData(),
                    static_cast<long long>(p->clock.elapsed()));
        std::fflush(stdout);

        const std::uint8_t addr = p->session.civAddress();
        p->session.sendCiv(cmdReadId(addr));
        p->session.sendCiv(cmdReadFrequency(addr));
        p->session.sendCiv(cmdReadMode(addr));

        // BOTH scope switches. Enabling only 0x27 0x10 turns the scope on the
        // radio's own screen and sends us nothing — the number-one "black
        // panadapter" cause, and worth demonstrating from a probe.
        p->session.sendCiv(cmdScopeOnOff(addr, true));
        p->session.sendCiv(cmdScopeDataOutput(addr, true));
    });

    QObject::connect(&p->session, &IcomSession::disconnected, &app, [](const QString& why) {
        std::printf("disconnected: %s\n", why.toLocal8Bit().constData());
        std::fflush(stdout);
    });

    QObject::connect(&p->session, &IcomSession::civFrameReady, &app, [p](const CivFrame& f) {
        if (auto sweep = p->scope.feed(f)) {
            if (p->firstSweepMs < 0)
                p->firstSweepMs = p->clock.elapsed();
            ++p->sweeps;
            // Report a geometry change the moment it happens: this is how we
            // learn whether a front-panel span change is ECHOED or has to be
            // polled (oracle §12 q4).
            if (sweep->startHz != p->lastStartHz || sweep->endHz != p->lastEndHz
                || sweep->mode != p->lastMode) {
                std::printf("  scope geometry: %s %.6f .. %.6f MHz (%.1f kHz wide, %zu points)%s\n",
                            modeLabel(sweep->mode), sweep->startHz / 1e6, sweep->endHz / 1e6,
                            sweep->bandwidthHz() / 1e3, sweep->raw.size(),
                            sweep->outOfRange ? "  [OUT OF RANGE]" : "");
                std::fflush(stdout);
                p->lastStartHz = static_cast<double>(sweep->startHz);
                p->lastEndHz = static_cast<double>(sweep->endHz);
                p->lastMode = sweep->mode;
            }
            return;
        }

        switch (f.cmd) {
        case cmd::kReadId:
            if (!f.data.empty()) {
                p->reportedCivAddress = f.data[0];
                std::printf("  CI-V address reported by the radio: 0x%02X\n", f.data[0]);
            }
            break;
        case cmd::kReadFreq:
        case cmd::kSetFreqTrx:
            if (auto hz = decodeFreq(f.data)) {
                p->frequencyHz = *hz;
                std::printf("  frequency: %.6f MHz\n", *hz / 1e6);
            }
            break;
        case cmd::kReadMode:
        case cmd::kSetModeTrx:
            if (!f.data.empty()) {
                const auto m = static_cast<CivMode>(f.data[0]);
                p->modeName = QString::fromStdString(modeToNeutral(m, false));
                std::printf("  mode: %s (wire 0x%02X)%s\n",
                            p->modeName.toLocal8Bit().constData(), f.data[0],
                            f.data.size() > 1 ? "" : "  [no filter byte]");
            }
            break;
        case cmd::kMeter:
            if (f.hasSub) {
                if (auto v = decodeLevel(f.data))
                    p->meters[f.sub] = *v;
            }
            break;
        default:
            break;
        }
        std::fflush(stdout);
    });

    QObject::connect(&p->session, &IcomSession::audioReady, &app,
                     [p](const std::vector<float>& s) { p->audioSamples += s.size(); });

    if (!p->session.start(params)) {
        std::fprintf(stderr, "failed to start the session\n");
        return 1;
    }

    // Poll the meters once a second. Deliberately gentle: metering shares the
    // CI-V stream with everything else, and a probe that floods it would be
    // measuring its own contention rather than the radio.
    auto* meterTimer = new QTimer(&app);
    QObject::connect(meterTimer, &QTimer::timeout, &app, [p] {
        if (!p->session.isConnected())
            return;
        const std::uint8_t addr = p->session.civAddress();
        p->session.sendCiv(cmdReadMeter(addr, meter::kSMeter));
        p->session.sendCiv(cmdReadMeter(addr, meter::kOverflow));
        p->session.sendCiv(cmdReadMeter(addr, meter::kVd));
    });
    meterTimer->start(1000);

    // Report and exit after 20 s.
    QTimer::singleShot(20000, &app, [p, &app] {
        const qint64 elapsed = p->clock.elapsed();
        std::printf("\n---- results after %.1f s ----\n", elapsed / 1000.0);

        if (p->sweeps > 1 && p->firstSweepMs >= 0) {
            const double window = (elapsed - p->firstSweepMs) / 1000.0;
            std::printf("scope: %d sweeps in %.1f s = %.1f sweeps/s   <-- oracle §12 q1\n",
                        p->sweeps, window, window > 0 ? p->sweeps / window : 0.0);
        } else if (!p->everConnected) {
            // Do NOT blame the scope settings for a session that never came up.
            // A diagnostic that points at the wrong subsystem costs more time
            // than no diagnostic at all.
            std::printf("scope: n/a — the session never connected, so nothing was asked of "
                        "the scope.\n");
        } else {
            std::printf("scope: NO SWEEPS.\n"
                        "       Both 0x27 0x10 (scope on) AND 0x27 0x11 (wave data output)\n"
                        "       must be ON. This probe sent both — if nothing arrived, check\n"
                        "       the scope is not disabled in the radio's own menu.\n");
        }

        std::printf("meters:\n");
        for (const auto& [sub, raw] : p->meters) {
            const char* name = sub == meter::kSMeter ? "S-meter"
                             : sub == meter::kOverflow ? "OVF"
                             : sub == meter::kVd ? "Vd" : "?";
            std::printf("  %-8s raw %4d", name, raw);
            // The S-meter is the ONLY calibrated reference we have, and it is
            // what turns the scope's uncalibrated 0..160 into something real
            // (oracle §12 q3). Published: 0 = S0, 120 = S9, 241 = S9+60.
            if (sub == meter::kSMeter) {
                const double s = raw <= 120 ? raw * 9.0 / 120.0 : 9.0;
                const double over = raw > 120 ? (raw - 120) * 60.0 / 121.0 : 0.0;
                if (over > 0)
                    std::printf("  = S9 + %.0f dB", over);
                else
                    std::printf("  = S%.1f", s);
            }
            std::printf("\n");
        }

        std::printf("audio: %llu samples received\n",
                    static_cast<unsigned long long>(p->audioSamples));

        const auto st = p->session.stats();
        std::printf("link:  control rtt %d ms  |  serial rx %llu pkt, %llu lost, %llu retx asked"
                    "  |  audio rx %llu pkt, %llu lost, %llu retx asked\n",
                    st.control.rttMs,
                    static_cast<unsigned long long>(st.serial.rxPackets),
                    static_cast<unsigned long long>(st.serial.rxLost),
                    static_cast<unsigned long long>(st.serial.retransmitsAsked),
                    static_cast<unsigned long long>(st.audio.rxPackets),
                    static_cast<unsigned long long>(st.audio.rxLost),
                    static_cast<unsigned long long>(st.audio.retransmitsAsked));

        if (p->reportedCivAddress != 0 && p->reportedCivAddress != p->session.civAddress())
            std::printf("\nNOTE: the radio reports CI-V address 0x%02X but the session assumed "
                        "0x%02X.\n      Model discovery must drive this, never a literal.\n",
                        p->reportedCivAddress, p->session.civAddress());

        p->session.stop();
        app.quit();
    });

    const int rc = app.exec();
    delete p;
    return rc;
}
