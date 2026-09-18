// WHO TAGS TRANSMIT AUDIO, AND AS WHAT -- THE PART A RUNNING TEST CANNOT REACH.
//
// The claim: the WSPR pump reaches the modulator as
// TxAudioSource::EngineGenerated, while the microphone and the AX.25 modem
// reach it as Microphone and TCI/DAX as ClientLeveled. Hl2TxDsp's own test
// proves what the DSP DOES with each. A mis-tag is otherwise silent: a beacon
// goes out 18.58 dB down, unattended, with every unit test still green.
//
// WHAT IS COVERED BEHAVIOURALLY, AND SO IS NOT COVERED HERE. icom_identity_test
// stands up a real AudioEngine with hostModulation() true and asserts the tag
// that arrives on a live txFinalMonitorPcmReady for TWO of the three entry
// points -- feedDaxTxAudio -> ClientLeveled, and sendModemTxAudio -> Microphone.
// AGENTS.md prefers exactly that ("Prefer behavioral seams over source-text
// assertions"), and an earlier revision of this file argued for source text on
// a premise -- "AudioEngine.cpp is compiled into no test target" -- that was
// simply false: it is in CORE_SOURCES and therefore in aethercore.
//
// WHAT IS LEFT, AND WHY IT IS HERE. AudioEngine::startWsprPump() is the only
// EngineGenerated producer, and reaching it behaviourally needs a prepared
// beacon and a timer tick for a frame that keys for 111.6 s -- against a bench
// loopback approval that permits 35 s, and raising a rail to fit a convenience
// is what that ceiling's own comment forbids. So this file pins THAT call site,
// and the metatype, and nothing that already has a behavioural home.
//
// SO BE HONEST ABOUT WHAT THIS PROVES. It proves one call site is WRITTEN as
// claimed and fails loudly if someone reverses it. It does NOT prove the code
// runs, and it does not close the end-to-end beacon leg, which remains open.
//
// AND ABOUT HOW IT CAN LIE. Matching a name anywhere in a file is not the same
// as matching a declaration: the doc comments in these headers spell every
// enumerator in prose, so a file-wide contains() for "EngineGenerated" passes
// with the enum deleted. The assertions below are scoped to the enum body for
// that reason -- the first revision of this file was not, and all three of its
// "declares X" checks passed against a gutted enum.

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition)
        ++failures;
}

QString readSource(const char* relative)
{
    QFile f(QStringLiteral(AETHER_SOURCE_DIR) + QLatin1Char('/')
            + QLatin1String(relative));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// Whitespace inside the source is not the subject of any assertion here, and
// matching it would make this file fail on a reflow that changed nothing.
QString flat(QString s)
{
    return s.simplified();
}

}  // namespace

int main()
{
    const QString engine = flat(readSource("src/core/AudioEngine.cpp"));
    const QString backend = flat(readSource("src/core/backends/hl2/Hl2Backend.cpp"));
    const QString iface = flat(readSource("src/core/backends/TxAudioSource.h"));

    check(!engine.isEmpty(), "AudioEngine.cpp is readable");
    check(!backend.isEmpty(), "Hl2Backend.cpp is readable");
    check(!iface.isEmpty(), "TxAudioSource.h is readable");

    // ── The three states exist and are distinct ──────────────────────────
    const qsizetype enumAt =
        iface.indexOf(QLatin1String("enum class TxAudioSource {"));
    check(enumAt >= 0, "TxAudioSource is an enum, not a bool");
    const QString enumBody = enumAt < 0
        ? QString()
        : iface.mid(enumAt, iface.indexOf(QLatin1Char('}'), enumAt) - enumAt);
    for (const char* state : {"Microphone", "ClientLeveled", "EngineGenerated"})
        check(enumBody.contains(QLatin1String(state)),
              qPrintable(QStringLiteral("TxAudioSource declares %1").arg(state)));

    // A queued signal carries it across AudioEngine's thread. Without the
    // metatype the connection fails at RUNTIME with a warning and a dropped
    // signal -- no transmit audio and no compile error to catch it.
    check(iface.contains(QLatin1String("Q_DECLARE_METATYPE(AetherSDR::TxAudioSource)")),
          "TxAudioSource is declared as a metatype");
    check(engine.contains(QLatin1String("qRegisterMetaType<AetherSDR::TxAudioSource>")),
          "AudioEngine registers the TxAudioSource metatype");

    // ── THE ONE CALL SITE WITHOUT A BEHAVIOURAL HOME ────────────────────
    //
    // AudioEngine::startWsprPump() is the only EngineGenerated producer. The
    // tag is passed explicitly now rather than derived from markExternalSource:
    // deriving it is what swept the AX.25 modem into the beacon's bucket, and
    // an explicit argument is also the thing a reader can check at the call
    // site. If this ever reads Microphone or ClientLeveled, a WSPR beacon goes
    // back to being moved by the mic slider.
    // NOT anchored on the closing paren: the call carries a
    // TxCoordinator::Context too (#5659), and a source-text assertion that pins
    // the whole argument list fires on every signature change while the
    // behaviour it guards is untouched. What must stay true is that THIS call
    // site -- identified by m_wsprFloatScratch, which no other
    // feedDaxTxAudioInternal() caller passes -- names EngineGenerated
    // explicitly. Arguments AFTER the tag are deliberately unconstrained.
    // `engine` is already whitespace-collapsed by flat() above, so a reflow of
    // the call does not fire this either.
    //
    // The tag must still match as a WHOLE TOKEN. A bare prefix match would
    // accept a future sibling enumerator -- EngineGeneratedBeacon, say -- and
    // go green while the WSPR pump's tag had silently changed, which is the one
    // thing this file exists to pin. So require the delimiter that ends the
    // argument, accepting either a following argument or the end of the call.
    // That terminates the token without constraining what comes after it.
    const QLatin1String wsprCall(
        "feedDaxTxAudioInternal(m_wsprFloatScratch, false, true, "
        "TxAudioSource::EngineGenerated");
    check(engine.contains(QString(wsprCall) + QLatin1Char(','))
              || engine.contains(QString(wsprCall) + QLatin1Char(')')),
          "the WSPR pump feeds TxAudioSource::EngineGenerated");

    // NOBODY DERIVES THE TAG FROM markExternalSource AGAIN. That flag means "a
    // TCI/DAX client is feeding" and arms the TCI active-audio timer; it is NOT
    // "not a beacon", and reading it as one is the defect this pins. The two
    // tags it used to produce are asserted behaviourally in icom_identity_test.
    check(!engine.contains(QLatin1String("markExternalSource ? TxAudioSource::")),
          "the tag is never derived from markExternalSource");

    // ── The backend acts on the distinction ──────────────────────────────
    check(backend.contains(QLatin1String("TxAudioSource::EngineGenerated")),
          "Hl2Backend distinguishes EngineGenerated");
    check(backend.contains(QLatin1String(
              "m_txAudioEngineGenerated || (source == TxAudioSource::EngineGenerated)")),
          "Hl2Backend records that a transmission carried engine-generated audio");

    if (failures == 0)
        std::printf("tx_audio_source_wiring_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
