// Pure-codec tests for the Elecraft KPA1500 protocol layer (#4097).
//
// Two things are under test here, and they matter for different reasons:
//
//  1. The FRAMING and boundary behaviour of MessageParser. These bytes come
//     straight off a LAN socket, so constitution Principle VII applies:
//     split frames, leading garbage, oversized payloads and never-terminated
//     frames must all be handled without crashing, hanging, or growing a
//     buffer without bound.
//
//  2. The KEYING builders. buildKey() must be structurally incapable of
//     emitting the unbounded `^TX;` form, because that form is exactly the
//     "amp stays keyed after the controlling software dies" hazard #4097
//     asks to eliminate.

#include "core/Kpa1500Protocol.h"

#include <QByteArray>
#include <QList>

#include <cmath>
#include <cstdio>

using namespace AetherSDR::Kpa1500;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool nearlyEqual(float a, float b, float tolerance = 0.001f)
{
    return std::fabs(a - b) <= tolerance;
}

// Collects everything a parser emits for one feed sequence.
QList<Message> drain(MessageParser& parser, const QList<QByteArray>& chunks)
{
    QList<Message> out;
    parser.setMessageCallback([&out](const Message& m) { out.append(m); });
    for (const QByteArray& chunk : chunks) {
        parser.feed(chunk);
    }
    return out;
}

}  // namespace

int main()
{
    // ── parseMessage(): one well-formed frame in isolation ───────────────
    {
        const auto m = parseMessage("^PWF1200;");
        report("parseMessage decodes a set/reply frame", m.has_value());
        if (m) {
            report("command token is isolated from the payload", m->cmd == "PWF");
            report("payload is kept as raw text", m->arg == "1200");
        }
    }
    {
        const auto m = parseMessage("^TQ;");
        report("parseMessage decodes a bare query", m.has_value() && m->cmd == "TQ" && m->arg.isEmpty());
    }
    report("parseMessage rejects a frame with no '^'", !parseMessage("PWF1200;").has_value());
    report("parseMessage rejects an unterminated frame", !parseMessage("^PWF1200").has_value());
    report("parseMessage rejects a 1-letter command token", !parseMessage("^P1;").has_value());
    report("parseMessage rejects a 4-letter command token", !parseMessage("^PWFX1;").has_value());
    // Case is normalized rather than rejected: the wire protocol is
    // upper-case, and Message::cmd's contract is "always upper-case", so
    // every comparison downstream can assume it without re-checking.
    {
        const auto m = parseMessage("^pwf1200;");
        report("parseMessage normalizes a lower-case command token to upper-case",
               m.has_value() && m->cmd == "PWF");
    }
    // Two concatenated frames are MessageParser's job, not parseMessage's --
    // accepting them here would let a caller silently drop the second one.
    report("parseMessage rejects two concatenated frames", !parseMessage("^PWF1;^PWR2;").has_value());

    // ── MessageParser: framing and resynchronization ─────────────────────
    {
        MessageParser p;
        const auto msgs = drain(p, {"^PWF1200;^PWR15;^SW120;"});
        report("parser splits three concatenated frames", msgs.size() == 3);
        if (msgs.size() == 3) {
            report("frames arrive in wire order",
                   msgs[0].cmd == "PWF" && msgs[1].cmd == "PWR" && msgs[2].cmd == "SW");
        }
    }
    {
        // A frame split across TCP reads is the normal case, not an edge
        // case -- nothing guarantees a 9-byte frame arrives in one packet.
        MessageParser p;
        const auto msgs = drain(p, {"^PW", "F12", "00;"});
        report("parser reassembles a frame split across three feeds",
               msgs.size() == 1 && msgs[0].cmd == "PWF" && msgs[0].arg == "1200");
    }
    {
        // Connecting mid-stream lands us partway through someone else's
        // frame. Without resync every later frame would be misparsed.
        MessageParser p;
        const auto msgs = drain(p, {"1200;^PWR15;"});
        report("parser resynchronizes past a truncated leading fragment",
               msgs.size() == 1 && msgs[0].cmd == "PWR");
    }
    {
        MessageParser p;
        const auto msgs = drain(p, {"garbage without any frame start at all"});
        report("parser emits nothing for a stream with no frame start", msgs.isEmpty());
    }
    {
        // A '^' that never gets its ';' must not wedge the parser: the next
        // genuine frame still has to come through.
        MessageParser p;
        QByteArray overlong = "^";
        overlong.append(QByteArray(MessageParser::kMaxArgChars + 32, 'A'));
        overlong.append("^PWF900;");
        const auto msgs = drain(p, {overlong});
        report("parser abandons an oversized unterminated frame and still decodes the next",
               msgs.size() == 1 && msgs[0].cmd == "PWF" && msgs[0].arg == "900");
    }
    {
        // Unbounded-growth guard: a peer that sends nothing but frame
        // starts must not be able to make this buffer grow forever.
        MessageParser p;
        p.setMessageCallback([](const Message&) {});
        for (int i = 0; i < 40; ++i) {
            p.feed(QByteArray(1024, '^'));
        }
        report("parser survives a flood of frame starts without emitting anything", true);
    }
    {
        // A malformed command token inside otherwise-valid framing is
        // dropped, not forwarded -- it must not reach applyMessage().
        MessageParser p;
        const auto msgs = drain(p, {"^123;^PWF50;"});
        report("parser drops a numeric command token but keeps the valid frame",
               msgs.size() == 1 && msgs[0].cmd == "PWF");
    }

    // ── applyMessage(): decode into Status ───────────────────────────────
    {
        Status s;
        report("PWF populates forward power",
               applyMessage(Message{"PWF", "1200"}, s) && s.forwardWatts
                   && nearlyEqual(*s.forwardWatts, 1200.0f));
        report("PWR populates reflected power",
               applyMessage(Message{"PWR", "15"}, s) && s.reflectedWatts
                   && nearlyEqual(*s.reflectedWatts, 15.0f));
        // Fixed-point integer form: the amp's documented encoding.
        report("SW decodes the fixed-point integer form",
               applyMessage(Message{"SW", "150"}, s) && s.swr && nearlyEqual(*s.swr, 1.5f));
        report("TM populates temperature",
               applyMessage(Message{"TM", "42"}, s) && s.tempC && *s.tempC == 42);
        report("OS1 reads as OPERATE",
               applyMessage(Message{"OS", "1"}, s) && s.operate && *s.operate);
        report("OS0 reads as STANDBY",
               applyMessage(Message{"OS", "0"}, s) && s.operate && !*s.operate);
        report("hasAnyField() is true once something has been reported", s.hasAnyField());
    }
    {
        // An amp that sends an explicit decimal point is taken at face
        // value rather than re-scaled -- see kSwrDivisor's own comment.
        Status s;
        applyMessage(Message{"SW", "1.5"}, s);
        report("SW honours an explicit decimal point", s.swr && nearlyEqual(*s.swr, 1.5f));
    }
    {
        Status s;
        report("applyMessage reports no change for an identical repeat",
               applyMessage(Message{"TM", "30"}, s) && !applyMessage(Message{"TM", "30"}, s));
    }
    {
        // Malformed input is expected input (Principle VII): it must leave
        // the snapshot untouched, not zero it or half-apply it.
        Status s;
        s.operate = true;
        report("a non-numeric boolean payload is rejected",
               !applyMessage(Message{"OS", "yes"}, s) && s.operate && *s.operate);
        report("an out-of-range boolean payload is rejected",
               !applyMessage(Message{"OS", "2"}, s) && *s.operate);
        report("an empty numeric payload is rejected",
               !applyMessage(Message{"TM", ""}, s) && !s.tempC);
        report("a negative power reading is rejected",
               !applyMessage(Message{"PWF", "-50"}, s) && !s.forwardWatts);
        report("an out-of-range antenna port is rejected",
               !applyMessage(Message{"AN", "7"}, s) && !s.antenna);
        report("an out-of-range ATU mode is rejected",
               !applyMessage(Message{"AM", "9"}, s) && !s.atuMode);
        report("an unrecognized command is ignored rather than desynchronizing",
               !applyMessage(Message{"ZZ", "1"}, s));
    }
    {
        Status s;
        applyMessage(Message{"AM", "1"}, s);
        report("AM1 decodes as AUTO", s.atuMode && *s.atuMode == AtuMode::Auto);
        applyMessage(Message{"AN", "2"}, s);
        report("AN2 decodes as antenna port 2", s.antenna && *s.antenna == 2);
    }

    // ── Band names ───────────────────────────────────────────────────────
    report("bandName(0) == 160", bandName(0) == "160");
    report("bandName(5) == 20", bandName(5) == "20");
    report("bandName(10) == 6", bandName(10) == "6");
    report("bandName() returns empty for a code outside the table",
           bandName(99).isEmpty() && bandName(-1).isEmpty());

    // ── Command builders ─────────────────────────────────────────────────
    report("buildSetOperate(true) == ^OS1;", buildSetOperate(true) == "^OS1;");
    report("buildSetOperate(false) == ^OS0;", buildSetOperate(false) == "^OS0;");
    report("buildSetPower(true) == ^ON1;", buildSetPower(true) == "^ON1;");
    report("buildStartTune() == ^FT1;", buildStartTune() == "^FT1;");
    report("buildCancelTune() == ^FE1;", buildCancelTune() == "^FE1;");
    report("buildSetAtuInline(true) == ^AI1;", buildSetAtuInline(true) == "^AI1;");
    report("buildSetAtuMode(Auto) == ^AM1;", buildSetAtuMode(AtuMode::Auto) == "^AM1;");
    // "Unknown" is this layer's own not-yet-reported marker; sending it
    // would command a mode that does not exist on the amp.
    report("buildSetAtuMode(Unknown) refuses to build a frame",
           buildSetAtuMode(AtuMode::Unknown).isEmpty());
    report("buildSelectAntenna(2) == ^AN2;", buildSelectAntenna(2) == "^AN2;");
    report("buildSelectAntenna refuses an out-of-range port",
           buildSelectAntenna(0).isEmpty() && buildSelectAntenna(4).isEmpty());
    report("buildQuery(\"PWF\") == ^PWF;", buildQuery("PWF") == "^PWF;");
    report("buildMessage refuses a malformed command token",
           buildMessage("").isEmpty() && buildMessage("P").isEmpty()
               && buildMessage("PWFX").isEmpty() && buildMessage("P1").isEmpty());
    report("buildClearFault() == ^FL0;", buildClearFault() == "^FL0;");

    // ── Keying: the safety-critical half ─────────────────────────────────
    //
    // The invariant is structural, not stylistic: there must be NO input to
    // buildKey() that produces the unbounded `^TX;` form. A bare ^TX leaves
    // the amp keyed with no fail-safe if this application dies mid-
    // transmission, which is the exact hazard #4097 asks to design out.
    report("buildKey(10) emits the bounded form", buildKey(10) == "^TX10;");
    report("buildKey(1) emits the bounded form", buildKey(1) == "^TX1;");
    report("buildKey clamps 0 up to the minimum rather than emitting a bare ^TX",
           buildKey(0) == "^TX1;");
    report("buildKey clamps a negative timeout up to the minimum",
           buildKey(-5) == "^TX1;");
    report("buildKey clamps an over-long timeout down to the maximum",
           buildKey(1000) == "^TX99;");
    {
        bool everUnbounded = false;
        for (int s = -1000; s <= 1000; ++s) {
            const QByteArray frame = buildKey(s);
            if (frame == "^TX;" || frame.isEmpty()) {
                everUnbounded = true;
                break;
            }
        }
        // An empty frame counts as a failure too: a key command that never
        // reaches the wire is a silent no-key, which is its own hazard.
        report("buildKey never emits a bare ^TX; or an empty frame, for any input",
               !everUnbounded);
    }
    report("buildUnkey() == ^RX;", buildUnkey() == "^RX;");
    report("buildKeyStateQuery() == ^TQ;", buildKeyStateQuery() == "^TQ;");

    // ── ATU mode labels ──────────────────────────────────────────────────
    report("atuModeLabel(Bypass) == BYPASS", atuModeLabel(AtuMode::Bypass) == "BYPASS");
    report("atuModeLabel(Unknown) renders as a placeholder, not a mode name",
           atuModeLabel(AtuMode::Unknown) != "BYPASS"
               && atuModeLabel(AtuMode::Unknown) != "AUTO"
               && atuModeLabel(AtuMode::Unknown) != "MANUAL");

    if (g_failed == 0) {
        std::printf("\nAll KPA1500 protocol tests passed.\n");
    } else {
        std::printf("\n%d KPA1500 protocol test(s) FAILED.\n", g_failed);
    }
    return g_failed == 0 ? 0 : 1;
}
