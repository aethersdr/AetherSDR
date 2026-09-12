// WHO TAGS TRANSMIT AUDIO, AND AS WHAT.
//
// PR "engine-generated audio keeps the level it was generated at" turns on one
// claim: that the WSPR pump, the AX.25 modem and the RADE waveform reach the
// modulator as TxAudioSource::EngineGenerated, while the microphone reaches it
// as Microphone and TCI/DAX as ClientLeveled. Hl2TxDsp's own test proves what
// the DSP DOES with each of those. Nothing proved the audio is tagged right in
// the first place, and a mis-tag is silent: a beacon would simply go out
// 18.58 dB down again, unattended, with every unit test still green.
//
// WHY THIS IS A SOURCE-TEXT TEST. An earlier draft of this comment said
// AudioEngine.cpp is compiled into no test target. THAT WAS WRONG, and the
// correction matters because it was the whole justification. The filename does
// not appear in tests/tests.cmake, but src/core/AudioEngine.cpp is in
// CORE_SOURCES and therefore inside the aethercore library, which several
// registered tests link. icom_identity_test is one: it stands up a real
// AudioEngine with hostModulation() true and already asserts
// source == TxAudioSource::ClientLeveled on a live txFinalMonitorPcmReady.
// One of the three tags has behavioural coverage today.
//
// The EngineGenerated tag is reachable there too, in principle --
// sendModemTxAudio(const QByteArray&) is public and takes the m_hostModulation
// branch into feedDaxTxAudioInternal with markExternalSource false -- so the
// 111.6 s WSPR frame is not what stands between this repository and a
// behavioural check of that branch. The frame argument is real, but it belongs
// to the END-TO-END BEACON leg: a frame keys for 111.6 s and the bench's
// loopback approval class permits a 35 s transmit ceiling, and raising a rail
// to fit a convenience is exactly what that ceiling's own comment forbids
// ("raise the class ceiling deliberately, not the run"). That leg stays open,
// and this test does not close it.
//
// So the claim this file makes for itself is the narrower one: it pins all
// three tags, the metatype, and each of the three call sites in ONE cheap
// place, where the behavioural coverage that exists reaches one tag in one
// backend. It is a reversal tripwire, not the instrument.
//
// SO BE HONEST ABOUT WHAT THIS PROVES. It proves the wiring is WRITTEN as
// claimed, and it fails loudly if someone reverses it. It does NOT prove the
// code runs, and it is not a substitute for the end-to-end beacon leg, which
// remains open. The same idiom, and the same caveat, as
// meter_applet_capability_test's source assertions.

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
    const QString iface = flat(readSource("src/core/backends/IRadioBackend.h"));

    check(!engine.isEmpty(), "AudioEngine.cpp is readable");
    check(!backend.isEmpty(), "Hl2Backend.cpp is readable");
    check(!iface.isEmpty(), "IRadioBackend.h is readable");

    // ── The three states exist and are distinct ──────────────────────────
    check(iface.contains(QLatin1String("enum class TxAudioSource")),
          "TxAudioSource is an enum, not a bool");
    for (const char* state : {"Microphone", "ClientLeveled", "EngineGenerated"})
        check(iface.contains(QLatin1String(state)),
              qPrintable(QStringLiteral("TxAudioSource declares %1").arg(state)));

    // A queued signal carries it across AudioEngine's thread. Without the
    // metatype the connection fails at RUNTIME with a warning and a dropped
    // signal -- no transmit audio and no compile error to catch it.
    check(iface.contains(QLatin1String("Q_DECLARE_METATYPE(AetherSDR::TxAudioSource)")),
          "TxAudioSource is declared as a metatype");
    check(engine.contains(QLatin1String("qRegisterMetaType<TxAudioSource>")),
          "AudioEngine registers the TxAudioSource metatype");

    // ── The microphone is tagged Microphone ──────────────────────────────
    check(engine.contains(QLatin1String(
              "emit txFinalMonitorPcmReady(data, TxAudioSource::Microphone)")),
          "the mic chain emits TxAudioSource::Microphone");

    // ── The engine's own generators are tagged EngineGenerated ───────────
    //
    // Both the WSPR pump and sendModemTxAudio's host-modulation branch reach
    // feedDaxTxAudioInternal with markExternalSource FALSE, and that single
    // emit is what maps false onto EngineGenerated. Pin the mapping and pin
    // both callers, because either half reversing alone is enough to lose it.
    check(engine.contains(QLatin1String(
              "markExternalSource ? TxAudioSource::ClientLeveled "
              ": TxAudioSource::EngineGenerated")),
          "markExternalSource=false maps to EngineGenerated, true to ClientLeveled");
    check(engine.contains(QLatin1String(
              "feedDaxTxAudioInternal(m_wsprFloatScratch, false, true)")),
          "the WSPR pump feeds with markExternalSource=false (EngineGenerated)");
    check(engine.contains(QLatin1String(
              "feedDaxTxAudioInternal(float32pcm, /*markExternalSource=*/false,")),
          "sendModemTxAudio's host-modulation branch feeds false (EngineGenerated)");

    // ── TCI/DAX stays ClientLeveled: #4796 must not regress ──────────────
    check(engine.contains(QLatin1String("feedDaxTxAudioInternal(inPcm, true, false)")),
          "external DAX/TCI feeds with markExternalSource=true (ClientLeveled)");

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
