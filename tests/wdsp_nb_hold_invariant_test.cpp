// #5499 item 3 — the noise-blanker hold, and what can actually be guarded
// about it.
//
// Three comments said the opposite of what the code did:
//
//   * Hl2RxDsp::setAudioMuted — "Holding it freezes the average instead;
//     WdspChannel flushes it on release."
//   * WdspChannel::setNoiseBlankerHold — "Both stores are atomic and the flush
//     they schedule happens inside processIq() itself."
//   * and both of them contradicted WdspChannel::processIq's own hold-release
//     branch, which argues AT LENGTH, with measured impulse ratios, that the
//     stage is DELIBERATELY NOT FLUSHED there and that this is the whole point
//     of holding rather than muting.
//
// The behaviour was the deliberate, well-argued one; the two upstream sentences
// were stale and inverted it. That is a comment fix, and a comment fix has no
// runtime test — nothing in the build can notice a sentence drifting away from
// the code it points at. THIS FILE DOES NOT PRETEND OTHERWISE. What it guards
// is the INVARIANT the corrected sentences now describe, so that if someone
// later "fixes" the code to match the sentences that were wrong, the build
// says so:
//
//   1. There is exactly ONE flush_anbEXT call in WdspChannel.cpp.
//   2. It is inside WdspChannel::setNoiseBlanker, in the `if (on)` branch —
//      i.e. on ENABLE, and nowhere else.
//   3. setNoiseBlankerHold flushes nothing and schedules nothing.
//   4. processIq flushes nothing, on either side of the hold.
//   5. While held, the stage is SKIPPED — the hold branch does not feed
//      xanbEXT. "Skipped, not fed" is the load-bearing half of the corrected
//      comment: it is why the running average survives the transmit and why
//      the blanker is already armed on the first receive sample.
//
// The rest of what those comments claim is UNGUARDABLE from here and is left
// unguarded rather than half-guarded: the 1.000 / 0.43 / 0.52 impulse ratios
// are a measurement against real WDSP and real signal, the ~200 ms arming
// delay follows from backtau inside the library, and "the mitigation is inert
// with the blanker off" is a property of m_nbActive gating m_nbHold that a
// reader can see in four lines and a test could only restate.
//
// Read as TEXT, in the same spirit and for the same stated reason as
// meter_surfaces_test: these facts never meet at compile time, and a
// structural check that proves a call site EXISTS WHERE IT SAYS IT DOES is
// worth more than no check. The limitation is real — it is a grep with a brace
// matcher, not a compiler — which is exactly why the analyser below is a pure
// function run against synthetic sources as well as the real one. Those
// synthetic runs are the POSITIVE CONTROL: they prove this file can actually
// see a flush in the wrong place, rather than reporting health because its
// matcher never matches anything.
//
// WHAT stripComments() BELOW CANNOT PARSE, so that a red here is diagnosed in
// a minute rather than an afternoon: it treats every bare ' outside a string
// or a comment as opening a char literal, and knows nothing about digit
// separators (1'000'000) or raw string literals (R"(...)"). WdspChannel.cpp
// contains neither today. If one appears, the text after it is blanked as
// though it were a literal, one of the four matchers below stops resolving,
// `parsed` goes false and EVERY check in this file goes red at once -- on a
// change that need not have touched the noise blanker at all. Six reds with
// no flush anywhere near them means look here first, not at the blanker.

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <QString>

#include <cstdio>

namespace {

int g_failed = 0;

void check(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

// Comments are where the WRONG claims lived, so they must not be able to
// satisfy — or break — a claim about the code. Blanks // to end of line and
// /* */ spans, plus the insides of string and character literals so that a
// brace or a call name appearing in one cannot reach the matcher. Removed text
// is replaced with spaces rather than deleted, so offsets stay put.
QString stripComments(QString source)
{
    const int n = source.size();
    bool inLine = false;
    bool inBlock = false;
    bool inString = false;
    bool inChar = false;
    for (int i = 0; i < n; ++i) {
        const QChar c = source.at(i);
        const QChar next = (i + 1 < n) ? source.at(i + 1) : QChar();
        if (inLine) {
            if (c == QLatin1Char('\n')) {
                inLine = false;
            } else {
                source[i] = QLatin1Char(' ');
            }
            continue;
        }
        if (inBlock) {
            const bool end = (c == QLatin1Char('*') && next == QLatin1Char('/'));
            if (c != QLatin1Char('\n')) {
                source[i] = QLatin1Char(' ');
            }
            if (end) {
                source[i + 1] = QLatin1Char(' ');
                ++i;
                inBlock = false;
            }
            continue;
        }
        if (inString || inChar) {
            const QChar closer = inString ? QLatin1Char('"') : QLatin1Char('\'');
            if (c == QLatin1Char('\\')) {
                source[i] = QLatin1Char(' ');
                if (i + 1 < n) {
                    source[i + 1] = QLatin1Char(' ');
                }
                ++i;
            } else if (c == closer) {
                inString = false;
                inChar = false;
            } else if (c != QLatin1Char('\n')) {
                source[i] = QLatin1Char(' ');
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inString = true;
        } else if (c == QLatin1Char('\'')) {
            inChar = true;
        } else if (c == QLatin1Char('/') && next == QLatin1Char('/')) {
            inLine = true;
            source[i] = QLatin1Char(' ');
        } else if (c == QLatin1Char('/') && next == QLatin1Char('*')) {
            inBlock = true;
            source[i] = QLatin1Char(' ');
        }
    }
    return source;
}

// The brace-matched block that starts at the first '{' at or after `from`.
// Returns an empty string when the braces do not balance, which every caller
// treats as "could not parse" rather than "nothing found".
QString blockAt(const QString& source, int from)
{
    const int open = source.indexOf(QLatin1Char('{'), from);
    if (open < 0) {
        return {};
    }
    int depth = 0;
    for (int i = open; i < source.size(); ++i) {
        const QChar c = source.at(i);
        if (c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                return source.mid(open, i - open + 1);
            }
        }
    }
    return {};
}

QString bodyOf(const QString& source, const QString& signature)
{
    const int at = source.indexOf(signature);
    return at < 0 ? QString() : blockAt(source, at);
}

int countOf(const QString& haystack, const QRegularExpression& call)
{
    int n = 0;
    QRegularExpressionMatchIterator matches = call.globalMatch(haystack);
    while (matches.hasNext()) {
        matches.next();
        ++n;
    }
    return n;
}

struct NbShape {
    bool parsed{false};                  // every function this needs was found
    int  flushCount{0};                  // flush_anbEXT calls in the whole file
    bool flushOnEnableOnly{false};       // the one call sits in setNoiseBlanker's if (on)
    bool holdSetterIsInert{false};       // setNoiseBlankerHold flushes nothing
    bool processIqNeverFlushes{false};   // no flush on either side of the hold
    bool stageSkippedWhileHeld{false};   // the hold branch does not feed xanbEXT

    bool healthy() const
    {
        return parsed && flushCount == 1 && flushOnEnableOnly && holdSetterIsInert
            && processIqNeverFlushes && stageSkippedWhileHeld;
    }
};

// Comments have already been replaced with whitespace. C++ permits that
// whitespace between the function identifier and its argument list.
const QRegularExpression kFlush(QStringLiteral("\\bflush_anbEXT\\s*\\("));
const QRegularExpression kFeed(QStringLiteral("\\bxanbEXT\\s*\\("));

NbShape analyse(const QByteArray& rawSource)
{
    NbShape shape;
    const QString source = stripComments(QString::fromUtf8(rawSource));

    const QString setter = bodyOf(source, QStringLiteral("::setNoiseBlanker(bool"));
    const QString holdSetter = bodyOf(source, QStringLiteral("::setNoiseBlankerHold(bool"));
    const QString processIq = bodyOf(source, QStringLiteral("::processIq("));
    if (setter.isEmpty() || holdSetter.isEmpty() || processIq.isEmpty()) {
        return shape;
    }

    // The hold branch is the block attached to `if (m_nbHold…)`, inside the
    // m_nbActive gate. Everything after its closing brace is the release side.
    const int holdAt = processIq.indexOf(QStringLiteral("if (m_nbHold"));
    const QString holdBranch = holdAt < 0 ? QString() : blockAt(processIq, holdAt);
    if (holdBranch.isEmpty()) {
        return shape;
    }

    const int onAt = setter.indexOf(QStringLiteral("if (on)"));
    const QString onBranch = onAt < 0 ? QString() : blockAt(setter, onAt);
    if (onBranch.isEmpty()) {
        return shape;
    }

    shape.parsed = true;
    shape.flushCount = countOf(source, kFlush);
    // "Only" in the strong sense: the call is in the enable branch AND the
    // enclosing function holds no other one.
    shape.flushOnEnableOnly = countOf(onBranch, kFlush) == 1
        && countOf(setter, kFlush) == 1;
    shape.holdSetterIsInert = countOf(holdSetter, kFlush) == 0;
    shape.processIqNeverFlushes = countOf(processIq, kFlush) == 0;
    // Skipped, not fed. The stage's running average survives the transmit
    // precisely because nothing is pushed through it while held.
    shape.stageSkippedWhileHeld = countOf(holdBranch, kFeed) == 0
        && countOf(holdBranch, kFlush) == 0;
    return shape;
}

// ── Synthetic sources: the positive control ─────────────────────────────────
//
// Miniatures of the four shapes that matter. If `analyse` reported health for
// these as readily as for the real file, it would be measuring nothing — which
// is the failure this whole issue is about, committed inside its own test.

const char* kHealthyMiniature = R"CPP(
bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_nbOpen) {
        if (on) {
            flush_anbEXT(m_channelId);
        }
        SetEXTANBRun(m_channelId, on ? 1 : 0);
    }
    return true;
}
void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    m_nbHold.store(hold, std::memory_order_relaxed);
}
WdspChannel::ProcessResult WdspChannel::processIq(Span i, Span q) noexcept
{
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
        } else {
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        }
    }
    return ProcessResult::Ok;
}
)CPP";

// The belief the stale comments carried: a flush on the way OUT of the hold.
const char* kFlushesOnReleaseMiniature = R"CPP(
bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_nbOpen) {
        if (on) {
            flush_anbEXT(m_channelId);
        }
    }
    return true;
}
void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    m_nbHold.store(hold, std::memory_order_relaxed);
}
WdspChannel::ProcessResult WdspChannel::processIq(Span i, Span q) noexcept
{
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
        } else {
            flush_anbEXT(m_channelId);
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        }
    }
    return ProcessResult::Ok;
}
)CPP";

// Held but still fed — the other way to lose the running average.
const char* kFedWhileHeldMiniature = R"CPP(
bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_nbOpen) {
        if (on) {
            flush_anbEXT(m_channelId);
        }
    }
    return true;
}
void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    m_nbHold.store(hold, std::memory_order_relaxed);
}
WdspChannel::ProcessResult WdspChannel::processIq(Span i, Span q) noexcept
{
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        } else {
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        }
    }
    return ProcessResult::Ok;
}
)CPP";

// A flush the hold SETTER performs — the literal reading of the sentence that
// said "the flush they schedule happens inside processIq() itself".
const char* kHoldSetterFlushesMiniature = R"CPP(
bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_nbOpen) {
        if (on) {
            flush_anbEXT(m_channelId);
        }
    }
    return true;
}
void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    flush_anbEXT(m_channelId);
    m_nbHold.store(hold, std::memory_order_relaxed);
}
WdspChannel::ProcessResult WdspChannel::processIq(Span i, Span q) noexcept
{
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
        } else {
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        }
    }
    return ProcessResult::Ok;
}
)CPP";

// A comment claiming a flush must not be able to satisfy — or violate — any of
// the above, because a comment claiming a flush is precisely what was wrong.
const char* kCommentOnlyFlushMiniature = R"CPP(
bool WdspChannel::setNoiseBlanker(bool on, int level) noexcept
{
    if (m_nbOpen) {
        if (on) {
            flush_anbEXT(m_channelId);
        }
    }
    return true;
}
void WdspChannel::setNoiseBlankerHold(bool hold) noexcept
{
    // WdspChannel flushes it on release: flush_anbEXT(m_channelId);
    m_nbHold.store(hold, std::memory_order_relaxed);
}
WdspChannel::ProcessResult WdspChannel::processIq(Span i, Span q) noexcept
{
    if (m_nbActive.load(std::memory_order_relaxed)) {
        if (m_nbHold.load(std::memory_order_relaxed)) {
            /* the stage is skipped, not fed -- no xanbEXT( here */
        } else {
            xanbEXT(m_channelId, m_nbInterleaved.data(), m_nbInterleaved.data());
        }
    }
    return ProcessResult::Ok;
}
)CPP";

} // namespace

int main()
{
    // ── The controls first. If these do not discriminate, nothing below means
    // anything, and saying so before the real assertion is the point.
    const NbShape healthy = analyse(QByteArray(kHealthyMiniature));
    check("control: the analyser parses a miniature of the expected shape",
          healthy.parsed);
    check("control: and calls that shape healthy", healthy.healthy());

    const NbShape onRelease = analyse(QByteArray(kFlushesOnReleaseMiniature));
    check("control: a flush on hold RELEASE is seen and rejected",
          onRelease.parsed && !onRelease.processIqNeverFlushes
              && onRelease.flushCount == 2 && !onRelease.healthy());

    const NbShape fedWhileHeld = analyse(QByteArray(kFedWhileHeldMiniature));
    check("control: feeding the stage while held is seen and rejected",
          fedWhileHeld.parsed && !fedWhileHeld.stageSkippedWhileHeld
              && !fedWhileHeld.healthy());

    const NbShape setterFlushes = analyse(QByteArray(kHoldSetterFlushesMiniature));
    check("control: a flush in the hold SETTER is seen and rejected",
          setterFlushes.parsed && !setterFlushes.holdSetterIsInert
              && !setterFlushes.healthy());

    const NbShape commentOnly = analyse(QByteArray(kCommentOnlyFlushMiniature));
    check("control: a flush that exists only in a COMMENT changes nothing",
          commentOnly.healthy());

    const QByteArray separators[]{" ", "\t\n", " /* gap */ ", " // gap\n"};
    for (const QByteArray& separator : separators) {
        const QByteArray flushCall = "flush_anbEXT" + separator + "(";
        const QByteArray feedCall = "xanbEXT" + separator + "(";
        const NbShape spacedHealthy = analyse(QByteArray(kHealthyMiniature)
            .replace("flush_anbEXT(", flushCall).replace("xanbEXT(", feedCall));
        check(("control: legal call separators preserve healthy code: "
               + separator.toHex()).constData(), spacedHealthy.healthy());

        const NbShape spacedRelease = analyse(QByteArray(kFlushesOnReleaseMiniature)
            .replace("flush_anbEXT(", flushCall));
        check(("control: separated release flush is rejected: "
               + separator.toHex()).constData(),
              spacedRelease.parsed && spacedRelease.flushCount == 2
                  && !spacedRelease.processIqNeverFlushes && !spacedRelease.healthy());

        const NbShape spacedSetter = analyse(QByteArray(kHoldSetterFlushesMiniature)
            .replace("flush_anbEXT(", flushCall));
        check(("control: separated hold-setter flush is rejected: "
               + separator.toHex()).constData(),
              spacedSetter.parsed && !spacedSetter.holdSetterIsInert
                  && !spacedSetter.healthy());

        const NbShape spacedFeed = analyse(QByteArray(kFedWhileHeldMiniature)
            .replace("xanbEXT(", feedCall));
        check(("control: separated held feed is rejected: "
               + separator.toHex()).constData(),
              spacedFeed.parsed && !spacedFeed.stageSkippedWhileHeld
                  && !spacedFeed.healthy());
    }

    // ── The real file.
    QFile file(QString::fromLatin1(AETHER_SOURCE_DIR)
               + QStringLiteral("/src/core/dsp/WdspChannel.cpp"));
    const bool opened = file.open(QIODevice::ReadOnly);
    check("WdspChannel.cpp is readable", opened);
    if (!opened) {
        std::printf("FAILURES\n");
        return 1;
    }
    const NbShape real = analyse(file.readAll());

    check("the three noise-blanker functions are all still recognisable",
          real.parsed);
    check("there is exactly one flush_anbEXT call in the file",
          real.flushCount == 1);
    check("and it is on ENABLE, inside setNoiseBlanker's if (on) branch",
          real.flushOnEnableOnly);
    check("setNoiseBlankerHold flushes nothing and schedules nothing",
          real.holdSetterIsInert);
    check("processIq flushes nothing on either side of the hold",
          real.processIqNeverFlushes);
    check("while held the stage is SKIPPED, not fed",
          real.stageSkippedWhileHeld);

    std::printf("%s\n", g_failed == 0 ? "ALL PASS" : "FAILURES");
    return g_failed == 0 ? 0 : 1;
}
