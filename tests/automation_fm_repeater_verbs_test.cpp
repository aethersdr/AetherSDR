// FM repeater verbs: slice tone / slice offset, transmit rfpower / tunepower,
// and the single shared slice-action list (#5102).
//
// #5102 was filed reporting squelch as unreachable from the bridge. It was not:
// `slice dsp squelch <on|off> [level]` had implemented it all along. The report
// happened because two hand-maintained copies of the slice-action list had
// drifted, and the one a caller actually hits — `unknown slice action:` — omitted
// filter, agc and dsp. So the list is now derived from one function, and the row
// below that pins both messages to it is the regression guard that matters most
// here: it is the defect that manufactured a false bug report.
//
// What this file pins is BOUNDARY behaviour, which is what a radio-less CI run
// can assert. Validation must happen before any slice is resolved (Principle
// VII), and the fixture makes that observable by carrying a RadioModel with NO
// slices: a well-formed request reports "no slice available", a malformed one
// reports its own error. If validation drifts back behind slice resolution the
// two collapse into one message and these rows fail.
//
// The APPLY path is asserted too, at the end, through the repo's own
// `slice fixture` action — a disconnected-only synthetic slice. That is what
// catches a verb that accepts a request, answers ok, and writes only two of
// the three fields a duplex split needs. A live FLEX-6500 exercised the same
// path against a real repeater; the fixture rows are what keep it honest in
// CI, where there is no radio.

#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok, const QString& detail = QString())
{
    std::printf("%s %-58s %s\n", ok ? "[ OK ]" : "[FAIL]", name, qPrintable(detail));
    if (!ok)
        ++g_failed;
}

QJsonObject request(QLocalSocket& socket, const QByteArray& line)
{
    socket.write(line + '\n');
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000 && !response.contains('\n')) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        response.append(socket.readAll());
        if (!response.contains('\n'))
            QThread::msleep(1);
    }
    if (!response.contains('\n'))
        return QJsonObject{{QStringLiteral("testError"), QStringLiteral("timeout")}};

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(response.trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonObject{{QStringLiteral("testError"), error.errorString()}};
    return doc.object();
}

QString errorOf(const QJsonObject& o)
{
    return o.value(QStringLiteral("error")).toString();
}

// A rejection that came from the VERB, not from the radio layer behind it.
void rejectedAtBoundary(QLocalSocket& socket, const char* name,
                        const QByteArray& line, const QString& expectFragment)
{
    const QJsonObject r = request(socket, line);
    const QString e = errorOf(r);
    report(name,
           r.value(QStringLiteral("ok")).toBool() == false
               && e.contains(expectFragment, Qt::CaseInsensitive)
               && !e.contains(QStringLiteral("no slice available")),
           e.isEmpty() ? QStringLiteral("(no error field)") : e);
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir testRoot;
    if (!testRoot.isValid()) {
        std::printf("[FAIL] temporary HOME could not be created\n");
        return 1;
    }
    const QByteArray root = testRoot.path().toUtf8();
    qputenv("HOME", root);
    qputenv("XDG_CONFIG_HOME", root);
    qputenv("TMPDIR", root);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    // Unset: the gate rows below assert TX is refused by default.
    qunsetenv("AETHER_AUTOMATION_ALLOW_TX");
    // start() reads the ceiling unconditionally, so it is in force the moment
    // TX is later permitted at runtime.
    qputenv("AETHER_AUTOMATION_TX_MAX_POWER", "30");

    QGuiApplication app(argc, argv);

    RadioModel radio;   // deliberately no slices — see the header comment
    AutomationServer server;
    server.setRadioModel(&radio);

    const QString serverName = QStringLiteral("aethersdr-fmverbs-test-%1")
                                   .arg(QCoreApplication::applicationPid());
    const bool started = server.start(serverName);
    report("bridge starts", started, server.fullServerName());
    if (!started)
        return 1;

    QLocalSocket socket;
    socket.connectToServer(serverName);
    const bool connected = socket.waitForConnected(2000);
    report("probe connects", connected, socket.errorString());
    if (!connected) {
        server.stop();
        return 1;
    }
    QCoreApplication::processEvents();

    // ── the one shared action list ───────────────────────────────────────
    // THE row this file exists for. Two hand-maintained copies drifted and the
    // divergence caused a false bug report; both messages must now name the
    // same set.
    {
        const QString empty = errorOf(request(socket, QByteArrayLiteral("slice")));
        const QString unknown =
            errorOf(request(socket, QByteArrayLiteral("slice bogusaction")));
        bool sameSet = true;
        for (const char* a : {"add", "remove", "select", "tx", "mode", "filter", "agc",
                              "dsp", "tone", "offset", "diversity", "centerlock",
                              "link", "txant", "rxant", "rxsource", "fixture",
                              "clearfixture"}) {
            const QString act = QString::fromLatin1(a);
            if (empty.contains(act) != unknown.contains(act))
                sameSet = false;
        }
        report("both slice error messages name the same action set", sameSet);
        report("the previously-omitted actions are advertised",
               unknown.contains(QStringLiteral("filter"))
                   && unknown.contains(QStringLiteral("agc"))
                   && unknown.contains(QStringLiteral("dsp")),
               unknown);
        report("the new actions are advertised",
               unknown.contains(QStringLiteral("tone"))
                   && unknown.contains(QStringLiteral("offset")),
               unknown);

        // ...and every advertised action actually DISPATCHES. The rows above
        // pin the two messages to each other, which can only catch someone
        // re-hardcoding one of them. The defect class in #5102 was
        // list ≠ dispatch: an action can be advertised and still fall through
        // to "unknown slice action". So drive each one and assert it does not.
        // Each is sent bare, so most are refused for their own reasons (a
        // missing argument, no slice, no radio) — any of those proves the
        // action was routed. Only falling through to the unknown-action arm
        // fails this row.
        QStringList notDispatched;
        const QStringList advertised =
            unknown.section(QLatin1Char('('), 1).section(QLatin1Char(')'), 0, 0)
                .split(QLatin1Char('|'), Qt::SkipEmptyParts);
        for (const QString& act : advertised) {
            const QString e = errorOf(
                request(socket, ("slice " + act).toUtf8()));
            if (e.contains(QStringLiteral("unknown slice action")))
                notDispatched << act;
        }
        report("every advertised slice action dispatches",
               !advertised.isEmpty() && notDispatched.isEmpty(),
               notDispatched.isEmpty()
                   ? QStringLiteral("%1 actions driven").arg(advertised.size())
                   : QStringLiteral("no handler: ") + notDispatched.join(QLatin1Char(' ')));
    }

    // ── the pre-existing squelch route still works ───────────────────────
    // #5102 reported squelch as missing; `slice dsp squelch` already did it.
    // Nothing here may break that path — reaching the radio layer is the proof
    // the action is still routed.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice dsp squelch off"));
        report("slice dsp squelch still routes (the pre-existing path)",
               errorOf(r).contains(QStringLiteral("no slice available")), errorOf(r));
    }

    // ── control row: a WELL-FORMED request reaches the radio layer ───────
    // Everything below is only meaningful against this.
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice tone ctcss_tx 100.0"));
        report("well-formed tone reaches the radio layer",
               errorOf(r).contains(QStringLiteral("no slice available")), errorOf(r));
    }

    // ── tone ─────────────────────────────────────────────────────────────
    rejectedAtBoundary(socket, "tone with no argument is refused",
                       QByteArrayLiteral("slice tone"), QStringLiteral("requires"));
    rejectedAtBoundary(socket, "tone mode 'dcs' is refused",
                       QByteArrayLiteral("slice tone dcs"), QStringLiteral("off/ctcss_tx"));
    rejectedAtBoundary(socket, "tone freq 9999 is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 9999"), QStringLiteral("CTCSS"));
    rejectedAtBoundary(socket, "tone freq 0 is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 0"), QStringLiteral("CTCSS"));
    // 123.4 sits inside the old 0 < f <= 300 bound and is NOT a CTCSS tone.
    // The verb now validates against core/CtcssTones.h — the same table the
    // operator's dropdown is built from — so the bridge cannot ask the radio
    // for a tone nobody could dial in.
    rejectedAtBoundary(socket, "a non-CTCSS tone inside the old range is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 123.4"),
                       QStringLiteral("CTCSS"));
    rejectedAtBoundary(socket, "67.1 Hz — one tick off a real tone — is refused",
                       QByteArrayLiteral("slice tone ctcss_tx 67.1"),
                       QStringLiteral("CTCSS"));

    // ── offset ───────────────────────────────────────────────────────────
    rejectedAtBoundary(socket, "offset with no argument is refused",
                       QByteArrayLiteral("slice offset"), QStringLiteral("requires"));
    rejectedAtBoundary(socket, "offset direction 'sideways' is refused",
                       QByteArrayLiteral("slice offset sideways"),
                       QStringLiteral("simplex/up/down"));
    rejectedAtBoundary(socket, "offset magnitude 'far' is refused",
                       QByteArrayLiteral("slice offset up far"), QStringLiteral("MHz"));
    rejectedAtBoundary(socket, "offset magnitude 101 MHz is refused",
                       QByteArrayLiteral("slice offset up 101"),
                       QStringLiteral("0..100"));
    rejectedAtBoundary(socket, "a negative offset magnitude is refused",
                       QByteArrayLiteral("slice offset down -5"),
                       QStringLiteral("0..100"));
    rejectedAtBoundary(socket, "a NaN offset magnitude is refused",
                       QByteArrayLiteral("slice offset up nan"),
                       QStringLiteral("frequency"));
    rejectedAtBoundary(socket, "an infinite offset magnitude is refused",
                       QByteArrayLiteral("slice offset up inf"),
                       QStringLiteral("frequency"));

    // ── transmit: gated, then clamped ────────────────────────────────────
    {
        const QJsonObject verbs = request(socket, QByteArrayLiteral("verbs"));
        bool hasTransmit = false;
        for (const QJsonValue& v : verbs.value(QStringLiteral("verbs")).toArray())
            if (v.toObject().value(QStringLiteral("name")).toString()
                == QLatin1String("transmit"))
                hasTransmit = true;
        report("transmit verb is registered", hasTransmit);
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 40"));
        report("transmit rfpower is blocked without ALLOW_TX",
               r.value(QStringLiteral("ok")).toBool() == false
                   && errorOf(r).contains(QStringLiteral("ALLOW_TX")),
               errorOf(r));
    }
    {
        // Principle VI: the gate must not be probeable with nonsense.
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 9999"));
        report("gate reports before value validation on a TX verb",
               errorOf(r).contains(QStringLiteral("ALLOW_TX")), errorOf(r));
    }
    rejectedAtBoundary(socket, "unknown transmit action is refused",
                       QByteArrayLiteral("transmit wattage 5"),
                       QStringLiteral("rfpower|tunepower"));
    rejectedAtBoundary(socket, "transmit with no action is refused",
                       QByteArrayLiteral("transmit"), QStringLiteral("requires an action"));

    // Now permit TX and prove the ceiling still binds. The invoke() power rail
    // is widget-scoped (it keys off accessibleName in the setValue path), so a
    // verb reaching the model directly does NOT inherit it — this row is the
    // reason the clamp is written out explicitly in doTransmit().
    server.setTxAllowed(true);
    QCoreApplication::processEvents();
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 90"));
        report("transmit rfpower is accepted once TX is allowed",
               r.value(QStringLiteral("ok")).toBool(), errorOf(r));
        report("transmit rfpower is clamped to AETHER_AUTOMATION_TX_MAX_POWER",
               r.value(QStringLiteral("clampedTo")).toInt() == 30
                   && r.value(QStringLiteral("requested")).toInt() == 90,
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        report("the clamp is reported, not silent",
               r.contains(QStringLiteral("requested"))
                   && r.contains(QStringLiteral("clampedTo")));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit tunepower 90"));
        report("tunepower is clamped by the same ceiling",
               r.value(QStringLiteral("clampedTo")).toInt() == 30,
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }
    {
        const QJsonObject r = request(socket, QByteArrayLiteral("transmit rfpower 10"));
        report("a request under the ceiling is not annotated",
               r.value(QStringLiteral("ok")).toBool()
                   && !r.contains(QStringLiteral("clampedTo")),
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }
    rejectedAtBoundary(socket, "rfpower 101 is refused even with TX allowed",
                       QByteArrayLiteral("transmit rfpower 101"), QStringLiteral("0..100"));

    // ── the applied duplex, through the repo's own disconnected fixture ──
    //
    // `slice fixture` synthesizes an owned slice through the normal
    // slice-status path with no radio attached, which is what lets these rows
    // assert the APPLY path and not merely the boundary.
    //
    // The bug these exist for: the verb set repeater_offset_dir and
    // fm_repeater_offset_freq and stopped there. tx_offset_freq is a field the
    // radio carries in its own right — FlexBackend decodes it as its own value,
    // and every other writer of duplex in the tree (RxApplet::applyOffsetDir,
    // VfoWidget's spin + applyDir, MemoryRecallPolicy's slice fixup) sends all
    // three. Sending two of three records "down, 5 MHz" on a slice that still
    // transmits on the receive frequency, and no reply field said so.
    {
        const QJsonObject fx = request(socket, QByteArrayLiteral("slice fixture 0"));
        report("disconnected slice fixture is available",
               fx.value(QStringLiteral("ok")).toBool(), errorOf(fx));
    }
    QCoreApplication::processEvents();

    auto offsetApplies = [&socket](const char* name, const QByteArray& line,
                                   double wantTx, const QString& wantDir,
                                   double wantMagnitude) {
        const QJsonObject r = request(socket, line);
        const double tx = r.value(QStringLiteral("txOffsetFreq")).toDouble();
        const QString dir = r.value(QStringLiteral("repeaterOffsetDir")).toString();
        const double mag =
            r.value(QStringLiteral("fmRepeaterOffsetFreq")).toDouble();
        report(name,
               r.value(QStringLiteral("ok")).toBool()
                   && qFuzzyCompare(tx + 1.0, wantTx + 1.0)
                   && dir == wantDir
                   && qFuzzyCompare(mag + 1.0, wantMagnitude + 1.0),
               QString::fromUtf8(
                   QJsonDocument(r).toJson(QJsonDocument::Compact)));
    };

    offsetApplies("down 5 MHz applies tx_offset_freq -5 (the TX split)",
                  QByteArrayLiteral("slice offset down 5"), -5.0,
                  QStringLiteral("down"), 5.0);
    offsetApplies("up 5 MHz applies +5 — the direction carries the sign",
                  QByteArrayLiteral("slice offset up 5"), 5.0,
                  QStringLiteral("up"), 5.0);
    offsetApplies("simplex zeroes the TX offset",
                  QByteArrayLiteral("slice offset simplex"), 0.0,
                  QStringLiteral("simplex"), 5.0);
    // A magnitude change with the direction UNCHANGED. setRepeaterOffsetDir()
    // early-returns when the value has not moved, so a recompute conditioned on
    // the direction changing would leave the old split in place here.
    offsetApplies("down 5 MHz, from simplex",
                  QByteArrayLiteral("slice offset down 5"), -5.0,
                  QStringLiteral("down"), 5.0);
    offsetApplies("a magnitude-only change recomputes the split",
                  QByteArrayLiteral("slice offset down 0.6"), -0.6,
                  QStringLiteral("down"), 0.6);

    {
        // ...and the number is readable, so a session can assert the duplex it
        // applied rather than assume the request took.
        const QJsonObject r = request(socket, QByteArrayLiteral("get slice active"));
        const QJsonObject snap =
            r.value(QStringLiteral("slice")).isObject()
                ? r.value(QStringLiteral("slice")).toObject()
                : r;
        report("the slice snapshot carries txOffsetFreq",
               snap.contains(QStringLiteral("txOffsetFreq"))
                   && qFuzzyCompare(
                       snap.value(QStringLiteral("txOffsetFreq")).toDouble() + 1.0,
                       -0.6 + 1.0),
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }

    // ── trailing operands are refused, and change nothing ────────────────
    //
    // The verbs took "<a> <b>" and read parts[0]/parts[1], so a third token was
    // silently dropped: `slice offset down 5 typo` answered ok and moved the
    // radio. Principle VII wants malformed input refused at the boundary, and
    // "refused" has to mean the model did not move — an error message alone
    // would not have caught this, because the error was never the problem.
    //
    // These rows carry the state assertion for that reason: each sends a
    // malformed command and asserts the slice (and the transmitter) reads
    // exactly as it did before.
    {
        auto sliceState = [&socket]() -> QString {
            const QJsonObject r =
                request(socket, QByteArrayLiteral("get slice active"));
            const QJsonObject sl = r.value(QStringLiteral("slice")).toObject();
            return QStringLiteral("%1/%2/%3/%4/%5")
                .arg(sl.value(QStringLiteral("fmToneMode")).toString(),
                     sl.value(QStringLiteral("fmToneValue")).toString(),
                     sl.value(QStringLiteral("repeaterOffsetDir")).toString())
                .arg(sl.value(QStringLiteral("fmRepeaterOffsetFreq")).toDouble())
                .arg(sl.value(QStringLiteral("txOffsetFreq")).toDouble());
        };
        auto txState = [&socket]() -> QString {
            const QJsonObject r = request(socket, QByteArrayLiteral("get transmit"));
            const QJsonObject t = r.value(QStringLiteral("transmit")).toObject();
            return QStringLiteral("%1/%2")
                .arg(t.value(QStringLiteral("rfPower")).toInt())
                .arg(t.value(QStringLiteral("tunePower")).toInt());
        };

        auto refusedAndUnchanged = [&](const char* name, const QByteArray& line,
                                       bool transmitVerb) {
            const QString before = transmitVerb ? txState() : sliceState();
            const QJsonObject r = request(socket, line);
            const QString after = transmitVerb ? txState() : sliceState();
            const QString e = errorOf(r);
            report(name,
                   r.value(QStringLiteral("ok")).toBool() == false
                       && !e.isEmpty() && after == before,
                   after == before
                       ? (e.isEmpty() ? QStringLiteral("(accepted)") : e)
                       : QStringLiteral("MUTATED %1 -> %2 (%3)")
                             .arg(before, after, e.isEmpty()
                                                     ? QStringLiteral("ok")
                                                     : e));
        };

        refusedAndUnchanged("offset with a trailing operand changes nothing",
                            QByteArrayLiteral("slice offset down 5 typo"), false);
        refusedAndUnchanged("tone with a trailing operand changes nothing",
                            QByteArrayLiteral("slice tone ctcss_tx 100.0 typo"),
                            false);
        // `transmit` drops the tail in its REGISTRY PARSER, before the handler
        // sees it — so this one cannot be caught downstream of parsing.
        refusedAndUnchanged("transmit with a trailing operand changes nothing",
                            QByteArrayLiteral("transmit rfpower 30 90"), true);
        // Both request forms, because they reach the arity check differently:
        // the JSON `args` string is folded into a bare request and re-parsed by
        // the registry parser, while explicit `action`/`value` fields skip the
        // parser entirely and are caught downstream. A fix in only one place
        // leaves the other form able to move the transmitter.
        refusedAndUnchanged("json args form refuses the trailing operand too",
                            QByteArrayLiteral(
                                "{\"cmd\":\"transmit\",\"args\":\"rfpower 30 90\"}"),
                            true);
        refusedAndUnchanged("json action/value form refuses it as well",
                            QByteArrayLiteral(
                                "{\"cmd\":\"transmit\",\"action\":\"rfpower\","
                                "\"value\":\"30 90\"}"),
                            true);

        // The well-formed shapes must keep working — an arity check that is
        // just "too many tokens" is easy to write one token too tight.
        {
            const QJsonObject r =
                request(socket, QByteArrayLiteral("slice offset down 5"));
            report("the well-formed offset still applies",
                   r.value(QStringLiteral("ok")).toBool()
                       && qFuzzyCompare(
                           r.value(QStringLiteral("txOffsetFreq")).toDouble() + 1.0,
                           -5.0 + 1.0),
                   QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        }
        {
            const QJsonObject r =
                request(socket, QByteArrayLiteral("slice offset simplex"));
            report("a one-token offset still applies",
                   r.value(QStringLiteral("ok")).toBool()
                       && r.value(QStringLiteral("repeaterOffsetDir")).toString()
                              == QLatin1String("simplex"),
                   QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        }
        {
            const QJsonObject r =
                request(socket, QByteArrayLiteral("transmit rfpower 25"));
            report("the well-formed transmit still applies",
                   r.value(QStringLiteral("ok")).toBool()
                       && r.value(QStringLiteral("rfPower")).toInt() == 25,
                   QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
        }
    }

    {
        // A legal tone applies, to the same fixture slice, and 123.0 is a real
        // table entry while the neighbouring 123.4 above is not.
        const QJsonObject r =
            request(socket, QByteArrayLiteral("slice tone ctcss_tx 123.0"));
        report("a standard CTCSS tone applies",
               r.value(QStringLiteral("ok")).toBool()
                   && r.value(QStringLiteral("fmToneMode")).toString()
                          == QLatin1String("ctcss_tx")
                   && r.value(QStringLiteral("fmToneValue")).toString()
                          == QLatin1String("123.0"),
               QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact)));
    }

    {
        const QJsonObject r = request(socket, QByteArrayLiteral("slice clearfixture 0"));
        report("slice fixture is removed again",
               r.value(QStringLiteral("ok")).toBool(), errorOf(r));
    }

    socket.disconnectFromServer();
    server.stop();

    std::printf("\n%s\n", g_failed == 0 ? "all rows passed" : "FAILURES PRESENT");
    return g_failed == 0 ? 0 : 1;
}
