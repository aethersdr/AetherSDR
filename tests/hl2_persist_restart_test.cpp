// What a Hermes-Lite 2 comes back with after a restart, driven through the
// operator's own setters and read back through the production store and load.
//
// Four things are pinned that the neighbouring tests only pin in halves:
//
//  1. Every mode the menu offers, with the passband the operator left it on,
//     survives capture -> RadioStateMemory store -> load -> a NEW backend ->
//     connectRadio() and the link-up handler. hl2_mode_vocabulary_test checks
//     the restore boundary with a hand-built document, and
//     hl2_state_restore_test checks the capture shape; neither sends a mode the
//     operator selected round the whole loop.
//
//  2. Band changes, per-band drive and per-band LNA gain across a restart: the
//     last band is where the session comes back, each visited band keeps its
//     own drive and gain, a hop in the restarted session applies the band's
//     own values rather than the previous band's, and a band never visited
//     takes the baseline rather than a neighbour's value.
//
//  3. A second receiver's own frequency, mode and passband across a restart,
//     reopened by the link-up replay as a restart does, and opened AT the
//     connect by the automation-only numRx param, and across a same-radio
//     reconnect whose receivers kept their live mode, with transmit on A and
//     on B.
//
//  4. The pan centre across a same-radio reconnect: an off-centre slice keeps
//     its centre while it still listens inside the usable window at the new
//     rate, and re-centres otherwise; another radio and a restart take theirs
//     from the document.
//
// No socket: see Hl2PersistHarness.h. The radio is never keyed (the drive is a
// setpoint; the transmit gate stays shut). The settings profile is isolated.
//
// NOT COVERED: the wire. That the restored mode, passband, drive and gain
// reach a real radio is the bench half.

#include "Hl2PersistHarness.h"

#include "TestSettingsProfile.h"
#include "core/backends/hl2/Hl2ModeVocabulary.h"

#include <QCoreApplication>

#include <cmath>

using namespace AetherSDR;
using namespace AetherSDR::persist;

namespace {

constexpr double k20m = 14'074'000.0;
constexpr double k40m = 7'074'000.0;
constexpr double k10m = 28'074'000.0;
constexpr double k15m = 21'074'000.0;
constexpr double k40mB = 7'150'000.0;   // receiver B's own frequency
constexpr double kPanCentre = 7'150'000.0;   // d265 leg g: pan 7.150, slice 7.162
constexpr double kOffCentre = 7'162'000.0;

QString serialFor(int n)
{
    return QStringLiteral("AA:BB:CC:00:10:%1").arg(n, 2, 16, QLatin1Char('0')).toUpper();
}

// A passband that is not the mode's default and is valid for every mode: the
// default pulled in by a tenth of its width on each edge. It still contains the
// carrier for the modes that straddle it (CW included).
std::pair<int, int> narrowed(double lo, double hi)
{
    const double tenth = (hi - lo) / 10.0;
    return {static_cast<int>(lo + tenth), static_cast<int>(hi - tenth)};
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("aether-hl2-persist-restart"));
    if (!profile.isValid()) {
        std::fprintf(stderr, "FAIL: could not create an isolated settings profile\n");
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    g_socketBaseline = openSocketCount();

    RadioCapabilities caps;

    // ======================================================================
    // 1. Mode and passband, for every mode the menu offers
    // ======================================================================
    {
        const QStringList modes = hl2::publishedModeStrings();
        check(modes.size() >= 8, QStringLiteral("setup: the menu offers %1 modes").arg(modes.size()));

        struct Chosen {
            QString mode;
            RestoredRadioState defaultDoc;   // as stored right after the mode click
            RestoredRadioState customDoc;    // as stored after a passband edit
            int lo = 0, hi = 0;              // the edited passband
        };
        QList<Chosen> chosen;

        {
            Session s(serialFor(0), RestoredRadioState{}, k20m);
            caps = s.backend.capabilities();
            for (const QString& mode : modes) {
                Chosen c;
                c.mode = mode;
                s.backend.setSliceMode(0, mode);
                c.defaultDoc = s.backend.currentOperatingState();
                check(c.defaultDoc.mode == mode,
                      QStringLiteral("capture: the slice holds %1 after the click, got \"%2\"")
                          .arg(mode, c.defaultDoc.mode));
                const auto [lo, hi] = narrowed(c.defaultDoc.filterLowHz, c.defaultDoc.filterHighHz);
                c.lo = lo;
                c.hi = hi;
                s.backend.setSliceFilter(0, lo, hi);
                c.customDoc = s.backend.currentOperatingState();
                check(c.customDoc.filterLowHz == lo && c.customDoc.filterHighHz == hi
                          && c.customDoc.mode == mode,
                      QStringLiteral("capture: %1 holds the edited passband %2..%3")
                          .arg(mode).arg(lo).arg(hi));
                chosen.append(c);
            }
        }

        // The default-passband documents: the mode alone must come back, and
        // the passband with it, with nothing but the stored document to go on.
        for (qsizetype i = 0; i < chosen.size(); ++i) {
            const Chosen& c = chosen.at(i);
            const RadioSettingsScope scope(QStringLiteral("hl2"), serialFor(static_cast<int>(i)));
            bool stored = false;
            const RestoredRadioState back = throughDisk(scope, caps, c.defaultDoc, &stored);
            check(stored, QStringLiteral("%1: the default-passband document is stored").arg(c.mode));
            hl2::Hl2Backend fresh;
            fresh.applyRestoredState(back);
            const RestoredRadioState& kept = fresh.restoredStateForTest();
            check(kept.mode == c.mode
                      && kept.filterLowHz == c.defaultDoc.filterLowHz
                      && kept.filterHighHz == c.defaultDoc.filterHighHz,
                  QStringLiteral("%1 and its default passband survive the disk and the restore boundary")
                      .arg(c.mode));
        }

        // The edited passband, through a real restart: a new backend, a connect
        // and the link-up push. The edit has to beat the mode's default, which
        // the push derives first (the #4484 pair rule).
        for (qsizetype i = 0; i < chosen.size(); ++i) {
            const Chosen& c = chosen.at(i);
            const RadioSettingsScope scope(QStringLiteral("hl2"),
                                           serialFor(static_cast<int>(100 + i)));
            // Control, taken before the restart and independent of what comes
            // back: the document being stored really differs from the default.
            check(c.customDoc.filterLowHz != c.defaultDoc.filterLowHz
                      || c.customDoc.filterHighHz != c.defaultDoc.filterHighHz,
                  QStringLiteral("%1: the stored edited passband differs from the mode's default "
                                 "(control for the restore check below)").arg(c.mode));
            const RestoredRadioState back = throughDisk(scope, caps, c.customDoc);
            Session next(serialFor(static_cast<int>(100 + i)), back, k20m);
            const RestoredRadioState now = next.backend.currentOperatingState();
            check(now.mode == c.mode,
                  QStringLiteral("%1 comes back after a restart, got \"%2\"").arg(c.mode, now.mode));
            check(now.filterLowHz == c.lo && now.filterHighHz == c.hi,
                  QStringLiteral("%1 comes back on the operator's passband %2..%3, not the "
                                 "default %4..%5 — got %6..%7")
                      .arg(c.mode).arg(c.lo).arg(c.hi)
                      .arg(c.defaultDoc.filterLowHz).arg(c.defaultDoc.filterHighHz)
                      .arg(now.filterLowHz).arg(now.filterHighHz));
        }
    }

    // ======================================================================
    // 2. Band changes, per-band drive and per-band LNA gain
    // ======================================================================
    {
        const QString serial = serialFor(200);
        const RadioSettingsScope scope(QStringLiteral("hl2"), serial);

        RestoredRadioState firstSession;
        {
            Session s(serial, RestoredRadioState{}, k20m);
            // 20 m: the first drive the operator sets becomes the baseline.
            s.backend.setTxPower(30);
            s.backend.setPanRfGain(s.panId, -6);
            s.backend.setSliceFrequency(0, k40m);
            s.backend.setTxPower(45);
            s.backend.setPanRfGain(s.panId, 3);
            s.backend.setSliceFrequency(0, k10m);
            s.backend.setTxPower(20);
            s.backend.setPanRfGain(s.panId, 8);
            s.backend.setSliceMode(0, QStringLiteral("USB"));
            firstSession = s.backend.currentOperatingState();
            check(firstSession.rfFrequencyHz == k10m,
                  "setup: the first session ends on 10 m");
            check(Session::driveOf(firstSession, QStringLiteral("20m")) == 30
                      && Session::driveOf(firstSession, QStringLiteral("40m")) == 45
                      && Session::driveOf(firstSession, QStringLiteral("10m")) == 20,
                  "setup: the capture holds each visited band's own drive");
            check(Session::lnaOf(firstSession, QStringLiteral("20m")) == -6
                      && Session::lnaOf(firstSession, QStringLiteral("40m")) == 3
                      && Session::lnaOf(firstSession, QStringLiteral("10m")) == 8,
                  "setup: the capture holds each visited band's own LNA gain");
        }

        bool stored = false;
        const RestoredRadioState onDisk = throughDisk(scope, caps, firstSession, &stored);
        check(stored, "the operating state is stored");

        Session next(serial, onDisk, k20m);   // k20m ignored: the document has a frequency
        const RestoredRadioState up = next.backend.currentOperatingState();
        check(up.rfFrequencyHz == k10m,
              "the restarted session comes back on the band it was left on (10 m)");
        check(Session::driveOf(up, QStringLiteral("20m")) == 30
                  && Session::driveOf(up, QStringLiteral("40m")) == 45
                  && Session::driveOf(up, QStringLiteral("10m")) == 20,
              "every band's drive survives the restart");
        check(Session::lnaOf(up, QStringLiteral("20m")) == -6
                  && Session::lnaOf(up, QStringLiteral("40m")) == 3
                  && Session::lnaOf(up, QStringLiteral("10m")) == 8,
              "every band's LNA gain survives the restart");
        check(up.extension.value(QStringLiteral("txSetpoints")).toObject()
                      .value(QStringLiteral("defaultPercent")).toInt(-1) == 30,
              "the drive baseline (the first drive set) survives the restart");
        check(next.liveLnaDb() == 8,
              "the start band comes up on its own LNA gain, live");

        // Hops in the restarted session apply the band's OWN values.
        next.backend.setSliceFrequency(0, k20m);
        check(next.liveLnaDb() == -6 && next.liveDriveFor(QStringLiteral("20m")) == 30,
              "a hop to 20 m applies 20 m's gain and drive, not 10 m's");
        next.backend.setSliceFrequency(0, k40m);
        check(next.liveLnaDb() == 3 && next.liveDriveFor(QStringLiteral("40m")) == 45,
              "a hop to 40 m applies 40 m's gain and drive");
        next.backend.setSliceFrequency(0, k10m);
        check(next.liveLnaDb() == 8 && next.liveDriveFor(QStringLiteral("10m")) == 20,
              "and back to 10 m applies 10 m's own again");

        // A band never visited: the baseline, never a neighbour's value.
        next.backend.setSliceFrequency(0, k15m);
        check(next.liveDriveFor(QStringLiteral("15m")) == 30,
              "an unvisited band takes the stored drive baseline (30), not the band it came from (20)");
        check(next.liveLnaDb() == hl2::kLnaDefaultGainDb,
              "an unvisited band takes the shipped LNA default, not a visited band's gain");
        check(!Session::hasLna(up, QStringLiteral("15m")) && !Session::hasDrive(up, QStringLiteral("15m")),
              "negative control: the stored document had no 15 m entry to begin with");
    }


    // ======================================================================
    // 3. A second receiver's frequency, mode and passband
    // ======================================================================
    {
        RestoredRadioState captured;
        int bDefaultLo = 0, bDefaultHi = 0;
        {
            Session s(serialFor(201), RestoredRadioState{}, k20m);
            check(s.backend.createPanadapter(), "setup: a second receiver opens");
            s.settle();
            check(Session::Access::receiverCount(s.backend) == 2, "setup: two receivers run");
            const int b = Session::Access::sliceIdOf(s.backend, 1);
            s.backend.setSliceFrequency(b, k40mB);
            s.backend.setSliceMode(b, QStringLiteral("LSB"));
            const QJsonObject atDefault = Session::receiverOf(s.backend.currentOperatingState(), 1);
            bDefaultLo = atDefault.value(QStringLiteral("filterLowHz")).toInt();
            bDefaultHi = atDefault.value(QStringLiteral("filterHighHz")).toInt();
            s.backend.setSliceFilter(b, -2400, -400);
            captured = s.backend.currentOperatingState();
            const QJsonObject rb = Session::receiverOf(captured, 1);
            check(rb.value(QStringLiteral("mode")).toString() == QLatin1String("LSB")
                      && rb.value(QStringLiteral("filterLowHz")).toInt() == -2400
                      && rb.value(QStringLiteral("filterHighHz")).toInt() == -400,
                  "setup: the capture holds receiver B's LSB -2400..-400");
            check(captured.mode == QLatin1String("USB") && captured.filterLowHz != -2400,
                  "setup: receiver A keeps its own USB passband");
        }
        // Control, independent of what comes back: B's edited passband is not
        // what LSB would give it, nor what A has.
        check(bDefaultLo != -2400 || bDefaultHi != -400,
              QStringLiteral("receiver B's edited passband differs from the LSB default %1..%2 "
                             "(control for the checks below)").arg(bDefaultLo).arg(bDefaultHi));

        const auto checkB = [&](const Session& next, const QString& road) {
            check(Session::Access::receiverCount(next.backend) == 2,
                  QStringLiteral("%1: two receivers run after the restart").arg(road));
            const RestoredRadioState up = next.backend.currentOperatingState();
            const QJsonObject ra = Session::receiverOf(up, 0);
            const QJsonObject rb = Session::receiverOf(up, 1);
            check(rb.value(QStringLiteral("mode")).toString() == QLatin1String("LSB"),
                  QStringLiteral("%1: receiver B comes back in LSB, got \"%2\"")
                      .arg(road, rb.value(QStringLiteral("mode")).toString()));
            check(rb.value(QStringLiteral("filterLowHz")).toInt() == -2400
                      && rb.value(QStringLiteral("filterHighHz")).toInt() == -400,
                  QStringLiteral("%1: receiver B comes back on its own passband -2400..-400, "
                                 "got %2..%3")
                      .arg(road)
                      .arg(rb.value(QStringLiteral("filterLowHz")).toInt())
                      .arg(rb.value(QStringLiteral("filterHighHz")).toInt()));
            check(qAbs(rb.value(QStringLiteral("freqHz")).toDouble() - k40mB) < 1.0,
                  QStringLiteral("%1: receiver B comes back on its own frequency").arg(road));
            check(up.mode == QLatin1String("USB")
                      && ra.value(QStringLiteral("filterLowHz")).toInt() == captured.filterLowHz
                      && ra.value(QStringLiteral("filterHighHz")).toInt() == captured.filterHighHz,
                  QStringLiteral("%1: receiver A keeps its own USB passband, not B's").arg(road));
        };

        {
            const RestoredRadioState back =
                throughDisk(RadioSettingsScope(QStringLiteral("hl2"), serialFor(202)), caps, captured);
            Session next(serialFor(202), back, k20m);
            checkB(next, QStringLiteral("replay"));
        }
        {
            // numRx=2 opens B at the connect, before the link-up derivation
            // runs; the replay then has nothing left to reopen.
            const RestoredRadioState back =
                throughDisk(RadioSettingsScope(QStringLiteral("hl2"), serialFor(203)), caps, captured);
            Session next(serialFor(203), back, k20m, {{QStringLiteral("numRx"), 2}});
            checkB(next, QStringLiteral("numRx"));
        }
    }

    // ======================================================================
    // 4. A same-radio reconnect whose receivers kept their live state
    // ======================================================================
    // Same backend, same serial, numRx=2: buildReceivers() carries both
    // receivers, so connectRadio() skips seedOpenReceiversFromMemory() and B
    // keeps its LIVE mode. The link-up derivation then puts B on that mode's
    // default, and the remembered pair applies only where the live mode is the
    // one it was saved with.
    {
        const QString serial = serialFor(204);
        const QVariantMap twoRx{{QStringLiteral("numRx"), 2}};
        Session s(serial, RestoredRadioState{}, k20m, twoRx);
        check(Session::Access::receiverCount(s.backend) == 2,
              "reconnect setup: numRx=2 opens two receivers");
        const int b = Session::Access::sliceIdOf(s.backend, 1);
        s.backend.setSliceFrequency(b, k40mB);
        s.backend.setSliceMode(b, QStringLiteral("LSB"));
        s.backend.setSliceFilter(b, -2400, -400);
        const RestoredRadioState remembered = throughDisk(
            RadioSettingsScope(QStringLiteral("hl2"), serial), caps,
            s.backend.currentOperatingState());
        const QJsonObject rbMem = Session::receiverOf(remembered, 1);
        check(rbMem.value(QStringLiteral("mode")).toString() == QLatin1String("LSB")
                  && rbMem.value(QStringLiteral("filterLowHz")).toInt() == -2400
                  && rbMem.value(QStringLiteral("filterHighHz")).toInt() == -400,
              "reconnect setup: the remembered document holds B's LSB -2400..-400");

        const auto liveB = [&] {
            return Session::receiverOf(s.backend.currentOperatingState(), 1);
        };
        const auto pbOf = [](const QJsonObject& r) {
            return std::pair<int, int>{r.value(QStringLiteral("filterLowHz")).toInt(),
                                       r.value(QStringLiteral("filterHighHz")).toInt()};
        };

        // Live mode differs from the remembered one: B stays in CWU on the
        // passband the click gave it, never the remembered LSB pair.
        s.backend.setSliceMode(b, QStringLiteral("CWU"));
        const auto cwu = pbOf(liveB());
        const QString cwuMode = liveB().value(QStringLiteral("mode")).toString();
        check(cwu != std::pair<int, int>{-2400, -400},
              QStringLiteral("reconnect, modes differ: B's live CWU passband %1..%2 differs "
                             "from the remembered LSB pair (control)")
                  .arg(cwu.first).arg(cwu.second));
        s.disconnect();
        s.connect(serial, remembered, k20m, twoRx);
        {
            const QJsonObject rb = liveB();
            const auto pb = pbOf(rb);
            check(Session::Access::receiverCount(s.backend) == 2
                      && cwuMode != QLatin1String("LSB")
                      && rb.value(QStringLiteral("mode")).toString() == cwuMode,
                  QStringLiteral("reconnect, modes differ: B kept its live mode \"%1\" (the "
                                 "carried road), got \"%2\"")
                      .arg(cwuMode, rb.value(QStringLiteral("mode")).toString()));
            check(pb == cwu,
                  QStringLiteral("reconnect, modes differ: B keeps its live CWU passband %1..%2, "
                                 "not the remembered LSB pair, got %3..%4")
                      .arg(cwu.first).arg(cwu.second).arg(pb.first).arg(pb.second));
        }

        // Live mode matches the remembered one: the remembered pair replaces
        // the live passband (the derivation has already discarded the edit).
        s.backend.setSliceMode(b, QStringLiteral("LSB"));
        const auto lsbDefault = pbOf(liveB());
        s.backend.setSliceFilter(b, -2800, -300);
        const auto edited = pbOf(liveB());
        check(edited == std::pair<int, int>{-2800, -300}
                  && edited != std::pair<int, int>{-2400, -400} && edited != lsbDefault
                  && lsbDefault != std::pair<int, int>{-2400, -400},
              QStringLiteral("reconnect, modes match: the live edit -2800..-300, the remembered "
                             "pair and the LSB default %1..%2 all differ (control)")
                  .arg(lsbDefault.first).arg(lsbDefault.second));
        s.disconnect();
        s.connect(serial, remembered, k20m, twoRx);
        {
            const QJsonObject rb = liveB();
            const auto pb = pbOf(rb);
            check(rb.value(QStringLiteral("mode")).toString() == QLatin1String("LSB")
                      && pb == std::pair<int, int>{-2400, -400},
                  QStringLiteral("reconnect, modes match: B comes back on the remembered LSB "
                                 "pair -2400..-400, got %1 %2..%3")
                      .arg(rb.value(QStringLiteral("mode")).toString())
                      .arg(pb.first).arg(pb.second));
        }
    }

    // ======================================================================
    // 5. The same reconnect with transmit on receiver B
    // ======================================================================
    // m_txDdc survives a disconnect, so on the carried road it still names B
    // at the link-up. The flat fields are receiver A's entry and must land on
    // A; B takes its own entry, mode included.
    {
        const QString serial = serialFor(205);
        const QVariantMap twoRx{{QStringLiteral("numRx"), 2}};
        Session s(serial, RestoredRadioState{}, k20m, twoRx);
        check(Session::Access::receiverCount(s.backend) == 2,
              "tx on B setup: numRx=2 opens two receivers");
        const int b = Session::Access::sliceIdOf(s.backend, 1);
        s.backend.setSliceFrequency(b, k40mB);
        s.backend.setSliceMode(b, QStringLiteral("LSB"));
        s.backend.setSliceFilter(b, -2400, -400);
        s.backend.setTxSlice(b);
        const QString txPassbandBefore =
            s.backend.healthSnapshot().values.value(QStringLiteral("txPassbandHz")).toString();
        const RestoredRadioState remembered = throughDisk(
            RadioSettingsScope(QStringLiteral("hl2"), serial), caps,
            s.backend.currentOperatingState());
        const QJsonObject raMem = Session::receiverOf(remembered, 0);
        const QJsonObject rbMem = Session::receiverOf(remembered, 1);
        check(rbMem.value(QStringLiteral("transmit")).toBool(false)
                  && !raMem.value(QStringLiteral("transmit")).toBool(false),
              "tx on B setup: the remembered document marks B, not A, as the transmit receiver");
        check(rbMem.value(QStringLiteral("mode")).toString() == QLatin1String("LSB")
                  && rbMem.value(QStringLiteral("filterLowHz")).toInt() == -2400
                  && rbMem.value(QStringLiteral("filterHighHz")).toInt() == -400
                  && raMem.value(QStringLiteral("mode")).toString() == QLatin1String("USB"),
              "tx on B setup: the document holds A in USB and B in LSB -2400..-400");
        const std::pair<int, int> aMem{raMem.value(QStringLiteral("filterLowHz")).toInt(),
                                       raMem.value(QStringLiteral("filterHighHz")).toInt()};

        s.disconnect();
        s.connect(serial, remembered, k20m, twoRx);
        const RestoredRadioState up = s.backend.currentOperatingState();
        const QJsonObject ra = Session::receiverOf(up, 0);
        const QJsonObject rb = Session::receiverOf(up, 1);
        check(Session::Access::receiverCount(s.backend) == 2
                  && rb.value(QStringLiteral("transmit")).toBool(false),
              "tx on B, reconnect: two receivers, and transmit is still on B");
        check(rb.value(QStringLiteral("mode")).toString() == QLatin1String("LSB"),
              QStringLiteral("tx on B, reconnect: B keeps its own LSB, not A's mode, got \"%1\"")
                  .arg(rb.value(QStringLiteral("mode")).toString()));
        check(rb.value(QStringLiteral("filterLowHz")).toInt() == -2400
                  && rb.value(QStringLiteral("filterHighHz")).toInt() == -400,
              QStringLiteral("tx on B, reconnect: B comes back on its own passband -2400..-400, "
                             "got %1..%2")
                  .arg(rb.value(QStringLiteral("filterLowHz")).toInt())
                  .arg(rb.value(QStringLiteral("filterHighHz")).toInt()));
        check(ra.value(QStringLiteral("mode")).toString() == QLatin1String("USB")
                  && ra.value(QStringLiteral("filterLowHz")).toInt() == aMem.first
                  && ra.value(QStringLiteral("filterHighHz")).toInt() == aMem.second,
              QStringLiteral("tx on B, reconnect: A comes back on its own USB %1..%2, got %3 %4..%5")
                  .arg(aMem.first).arg(aMem.second)
                  .arg(ra.value(QStringLiteral("mode")).toString())
                  .arg(ra.value(QStringLiteral("filterLowHz")).toInt())
                  .arg(ra.value(QStringLiteral("filterHighHz")).toInt()));
        // The passband alone cannot tell LSB from USB; the modulator's mode can.
        const hl2::Hl2TxDsp::Config tx = Session::Access::txDspConfig(s.backend);
        const QString txPassbandAfter =
            s.backend.healthSnapshot().values.value(QStringLiteral("txPassbandHz")).toString();
        check(tx.mode == WdspChannel::Mode::Lsb && !txPassbandBefore.isEmpty()
                  && txPassbandAfter == txPassbandBefore,
              QStringLiteral("tx on B, reconnect: the modulator runs B's LSB on its TX passband "
                             "%1, got %2 on %3")
                  .arg(txPassbandBefore)
                  .arg(tx.mode == WdspChannel::Mode::Lsb ? QStringLiteral("LSB")
                                                         : QStringLiteral("not LSB"))
                  .arg(txPassbandAfter));
    }

    // ======================================================================
    // 6. The pan centre across a same-radio reconnect
    // ======================================================================
    {
        using A = Session::Access;
        // The centre the backend holds, the initial NCO register, and the
        // centre published for receiver A's pan after the link-up.
        const auto centreIs = [](Session& s, QSignalSpy& pans, double hz, const QString& what) {
            double published = 0.0;
            for (const QList<QVariant>& args : pans) {
                if (args.at(0).toString() == s.panId)
                    published = args.at(1).toDouble() * 1.0e6;
            }
            check(A::ncoHz(s.backend, 0) == hz
                      && A::rxNcoCc(s.backend, 0) == A::expectedRxCc(s.backend, 0, hz)
                      && std::abs(published - hz) < 1.0,
                  QStringLiteral("%1: centre %2 Hz, got NCO %3, published %4")
                      .arg(what).arg(hz, 0, 'f', 0)
                      .arg(A::ncoHz(s.backend, 0), 0, 'f', 0).arg(published, 0, 'f', 0));
        };
        const QString serial = serialFor(206);
        const RadioSettingsScope scope(QStringLiteral("hl2"), serial);
        Session s(serial, RestoredRadioState{}, kPanCentre);
        s.backend.setSliceFrequency(A::sliceIdOf(s.backend, 0), kOffCentre);
        check(A::ncoHz(s.backend, 0) == kPanCentre && A::sampleRateHz(s.backend) == 48'000,
              "pan centre setup: at 48 kHz a 12 kHz tune leaves the centre on 7.150");
        RestoredRadioState remembered =
            throughDisk(scope, caps, s.backend.currentOperatingState());

        // Inside the window: 12 kHz from the centre, usable is 0.8 x 24 kHz.
        s.disconnect();
        {
            QSignalSpy pans(&s.backend, &hl2::Hl2Backend::panCenterBandwidthChanged);
            s.connect(serial, remembered, kPanCentre);
            check(A::sliceHz(s.backend, 0) == kOffCentre,
                  "pan centre, reconnect: the slice comes back on 7.162");
            centreIs(s, pans, kPanCentre, "pan centre, reconnect inside the window");
        }

        // A restart (control): a new backend takes the centre from the document.
        {
            Session fresh(serial, throughDisk(scope, caps, remembered), kPanCentre);
            check(A::ncoHz(fresh.backend, 0) == kPanCentre
                      && A::sliceHz(fresh.backend, 0) == kOffCentre,
                  "pan centre, restart (control): centre 7.150, slice 7.162");
        }

        // Outside the window at the NEW rate: 30 kHz off fits 96 kHz (usable
        // 38.4 kHz) but not 48 kHz (19.2 kHz), so it re-centres on the slice.
        const QVariantMap at96{{QStringLiteral("sampleRateHz"), 96'000}};
        const QVariantMap at48{{QStringLiteral("sampleRateHz"), 48'000}};
        s.disconnect();
        s.connect(serial, remembered, kPanCentre, at96);
        s.backend.setSliceFrequency(A::sliceIdOf(s.backend, 0), kPanCentre + 30'000.0);
        check(A::sampleRateHz(s.backend) == 96'000 && A::ncoHz(s.backend, 0) == kPanCentre,
              "pan centre setup: at 96 kHz a 30 kHz tune leaves the centre on 7.150");
        remembered = throughDisk(scope, caps, s.backend.currentOperatingState());
        s.disconnect();
        {
            QSignalSpy pans(&s.backend, &hl2::Hl2Backend::panCenterBandwidthChanged);
            s.connect(serial, remembered, kPanCentre, at96);
            centreIs(s, pans, kPanCentre, "pan centre, reconnect at the same 96 kHz");
        }
        s.disconnect();
        {
            QSignalSpy pans(&s.backend, &hl2::Hl2Backend::panCenterBandwidthChanged);
            s.connect(serial, remembered, kPanCentre, at48);
            check(A::sampleRateHz(s.backend) == 48'000,
                  "pan centre: the reconnect runs at 48 kHz");
            centreIs(s, pans, kPanCentre + 30'000.0,
                     "pan centre, reconnect at 48 kHz: outside the window, re-centred");
        }

        // Another radio on the same backend takes its own remembered centre.
        const QString other = serialFor(207);
        RestoredRadioState otherDoc;
        {
            Session o(other, RestoredRadioState{}, k20m);
            o.backend.setSliceFrequency(A::sliceIdOf(o.backend, 0), k20m + 6'000.0);
            otherDoc = throughDisk(RadioSettingsScope(QStringLiteral("hl2"), other), caps,
                                   o.backend.currentOperatingState());
        }
        s.disconnect();
        {
            QSignalSpy pans(&s.backend, &hl2::Hl2Backend::panCenterBandwidthChanged);
            s.connect(other, otherDoc, k20m);
            check(A::sliceHz(s.backend, 0) == k20m + 6'000.0,
                  "pan centre, another radio: its slice 14.080");
            centreIs(s, pans, k20m, "pan centre, another radio: its own centre 14.074");
        }

        // A receiver opened at the connect keeps its own centre, on its own
        // NCO register rather than the one start() gives every DDC (A's).
        {
            const QString two = serialFor(208);
            const RadioSettingsScope twoScope(QStringLiteral("hl2"), two);
            const QVariantMap twoRx{{QStringLiteral("numRx"), 2}};
            Session t(two, RestoredRadioState{}, kPanCentre, twoRx);
            const int b = A::sliceIdOf(t.backend, 1);
            t.backend.setSliceFrequency(b, kPanCentre + 30'000.0);   // B re-centres
            t.backend.setSliceFrequency(b, kPanCentre + 15'000.0);   // and stays
            t.backend.setSliceFrequency(A::sliceIdOf(t.backend, 0), kOffCentre);
            check(A::ncoHz(t.backend, 0) == kPanCentre
                      && A::ncoHz(t.backend, 1) == kPanCentre + 30'000.0,
                  "pan centre setup, numRx=2: A centred on 7.150, B on 7.180");
            const RestoredRadioState twoDoc =
                throughDisk(twoScope, caps, t.backend.currentOperatingState());
            t.disconnect();
            t.connect(two, twoDoc, kPanCentre, twoRx);
            std::printf("[INFO] numRx=2 reconnect: B slice %.0f, NCO %.0f\n",
                        A::sliceHz(t.backend, 1), A::ncoHz(t.backend, 1));
            check(A::ncoHz(t.backend, 0) == kPanCentre
                      && A::ncoHz(t.backend, 1) == kPanCentre + 30'000.0,
                  QStringLiteral("pan centre, numRx=2 reconnect: A on 7.150 and B on 7.180, got "
                                 "%1 and %2")
                      .arg(A::ncoHz(t.backend, 0), 0, 'f', 0)
                      .arg(A::ncoHz(t.backend, 1), 0, 'f', 0));
            check(A::rxNcoCc(t.backend, 1)
                      == A::expectedRxCc(t.backend, 1, kPanCentre + 30'000.0),
                  "pan centre, numRx=2 reconnect: B's NCO register is on B's 7.180");
        }
    }

    std::printf("hl2_persist_restart_test: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
