// Transmit on receiver B, then a same-radio reconnect that opens only
// receiver A at the connect (the default numRx=1) and gets B back from the
// link-up replay: the modulator must run B's mode and the TX NCO sit on B's
// frequency. A fresh backend (a restart) is the control, and a numRx=2
// reconnect checks that transmit stays on B when B is open at the connect.
//
// No socket (Hl2PersistHarness.h). Never keys: the modulator's configured
// mode and the TX NCO command word are read, nothing is transmitted.

#include "Hl2PersistHarness.h"

#include "TestSettingsProfile.h"

#include <QCoreApplication>

using namespace AetherSDR;
using namespace AetherSDR::persist;

namespace {

constexpr double k20m = 14'074'000.0;
constexpr double k40mB = 7'150'000.0;   // receiver B, the transmit receiver

const char* modeName(WdspChannel::Mode m)
{
    switch (m) {
    case WdspChannel::Mode::Lsb: return "LSB";
    case WdspChannel::Mode::Usb: return "USB";
    default: return "other";
    }
}

void report(Session& s, const char* when)
{
    using A = Session::Access;
    const hl2::Hl2TxDsp::Config tc = A::txDspConfig(s.backend);
    const bool ncoOnB = A::txNcoCc(s.backend) == A::expectedTxCc(s.backend, k40mB);
    const bool ncoOnA = A::txNcoCc(s.backend) == A::expectedTxCc(s.backend, k20m);
    std::printf("[INFO] %s: receivers=%d m_txDdc=%d modulator mode=%s passband=%.0f..%.0f "
                "TX NCO on B(7.150)=%d on A(14.074)=%d\n",
                when, A::receiverCount(s.backend), A::txDdc(s.backend), modeName(tc.mode),
                tc.filterLowHz, tc.filterHighHz, ncoOnB, ncoOnA);
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-tx-mode-reconnect"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    g_socketBaseline = openSocketCount();
    using A = Session::Access;

    const QString serial = QStringLiteral("AA:BB:CC:00:20:01");
    const RadioCapabilities caps = hl2::Hl2Backend{}.capabilities();
    RestoredRadioState remembered;

    Session s(serial, RestoredRadioState{}, k20m);
    check(s.backend.createPanadapter(), "setup: a second receiver opens");
    s.settle();
    const int b = A::sliceIdOf(s.backend, 1);
    s.backend.setSliceFrequency(b, k40mB);
    s.backend.setSliceMode(b, QStringLiteral("LSB"));
    s.backend.setTxSlice(b);
    report(s, "before disconnect");
    check(A::txDdc(s.backend) == 1, "setup: transmit is on receiver B");
    check(A::txDspConfig(s.backend).mode == WdspChannel::Mode::Lsb,
          "setup (control): the modulator runs B's LSB");
    check(A::txNcoCc(s.backend) == A::expectedTxCc(s.backend, k40mB),
          "setup (control): the TX NCO is on B's 7.150 MHz");
    remembered = throughDisk(RadioSettingsScope(QStringLiteral("hl2"), serial), caps,
                             s.backend.currentOperatingState());
    check(Session::receiverOf(remembered, 1).value(QStringLiteral("transmit")).toBool(),
          "setup: the remembered document marks B as the transmit receiver");

    // Control: a restart, a new backend that has never seen transmit on B.
    {
        Session fresh(QStringLiteral("AA:BB:CC:00:20:02"),
                      throughDisk(RadioSettingsScope(QStringLiteral("hl2"),
                                                     QStringLiteral("AA:BB:CC:00:20:02")),
                                  caps, remembered),
                      k20m);
        report(fresh, "restart (fresh backend)");
        check(A::receiverCount(fresh.backend) == 2 && A::txDdc(fresh.backend) == 1,
              "restart (control): B is reopened and holds transmit");
        check(A::txDspConfig(fresh.backend).mode == WdspChannel::Mode::Lsb,
              "restart (control): the modulator runs B's LSB");
        check(A::txNcoCc(fresh.backend) == A::expectedTxCc(fresh.backend, k40mB),
              "restart (control): the TX NCO is on B's 7.150 MHz");
    }

    // The case: the same backend reconnects, numRx defaults to 1.
    s.disconnect();
    s.connect(serial, remembered, k20m);
    s.settle();
    report(s, "same-backend reconnect");
    check(A::receiverCount(s.backend) == 2, "reconnect: the replay reopens B");
    check(A::txDdc(s.backend) == 1
              && Session::receiverOf(s.backend.currentOperatingState(), 1)
                     .value(QStringLiteral("transmit")).toBool(),
          "reconnect: B is published as the transmit receiver");
    check(A::txDspConfig(s.backend).mode == WdspChannel::Mode::Lsb,
          QStringLiteral("reconnect: the modulator runs B's LSB, got %1")
              .arg(QString::fromLatin1(modeName(A::txDspConfig(s.backend).mode))));
    check(A::txNcoCc(s.backend) == A::expectedTxCc(s.backend, k40mB),
          "reconnect: the TX NCO is on B's 7.150 MHz");

    // numRx=2 opens B at the connect, so transmit stays on B throughout:
    // the reset must not move it off a receiver that is open.
    {
        const QVariantMap twoRx{{QStringLiteral("numRx"), 2}};
        s.disconnect();
        s.connect(serial, remembered, k20m, twoRx);
        s.settle();
        report(s, "same-backend reconnect, numRx=2");
        const QJsonObject rb = Session::receiverOf(s.backend.currentOperatingState(), 1);
        const double bHz = rb.value(QStringLiteral("freqHz")).toDouble();
        std::printf("[INFO] numRx=2: B is %s on %.0f Hz\n",
                    qPrintable(rb.value(QStringLiteral("mode")).toString()), bHz);
        check(A::receiverCount(s.backend) == 2 && A::txDdc(s.backend) == 1
                  && rb.value(QStringLiteral("transmit")).toBool(),
              "reconnect numRx=2: B holds transmit");
        check(rb.value(QStringLiteral("mode")).toString() == QLatin1String("LSB"),
              QStringLiteral("reconnect numRx=2: B keeps its LSB, got %1")
                  .arg(rb.value(QStringLiteral("mode")).toString()));
        check(A::txDspConfig(s.backend).mode == WdspChannel::Mode::Lsb,
              "reconnect numRx=2: the modulator runs B's LSB");
        check(bHz > 0.0 && A::txNcoCc(s.backend) == A::expectedTxCc(s.backend, bHz),
              "reconnect numRx=2: the TX NCO is on B's frequency");
    }

    std::printf("hl2_tx_mode_reconnect_test: %s\n",
                g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
