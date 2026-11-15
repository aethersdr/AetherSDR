// Socket/device-free format selection and independent WAV field expectations.
#include "core/QsoRecordingFormat.h"

#include <QCoreApplication>
#include <QDebug>

#include <limits>
#include <type_traits>

using namespace AetherSDR;

static int failures = 0;
static int checks = 0;

static void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        ++failures;
        qCritical() << "FAIL:" << message;
    }
}

static void verifyHeader(const QsoRecordingFormat& format, int rate)
{
    const quint64 payload = static_cast<quint64>(rate) * 4 * 3 + 4;
    const std::optional<QByteArray> header = format.wavHeader(payload);
    check(header && header->size() == 44, "header has exactly 44 bytes");
    if (!header) {
        return;
    }
    // Expected byte layout comes from RIFF/WAVE PCM, not the playback parser.
    check(header->mid(0, 4) == "RIFF" && header->mid(8, 8) == "WAVEfmt "
              && header->mid(36, 4) == "data", "required WAV chunk identifiers");
    const auto u16 = [&header](int offset) {
        return qFromLittleEndian<quint16>(header->constData() + offset);
    };
    const auto u32 = [&header](int offset) {
        return qFromLittleEndian<quint32>(header->constData() + offset);
    };
    check(u32(4) == payload + 36 && u32(40) == payload,
          "RIFF/data sizes equal the accepted payload");
    check(u32(16) == 16 && u16(20) == 1 && u16(22) == 2
              && u16(32) == 4 && u16(34) == 16, "PCM16 stereo block format");
    check(u32(24) == static_cast<quint32>(rate)
              && u32(28) == static_cast<quint32>(rate * 4),
          "sampleRate and byteRate follow immutable file format");
    check(format.durationSecs(payload) == 3,
          "duration uses accepted sample frames, not wall clock");
    check(format.durationSecs(static_cast<quint64>(rate) * 4 - 4) == 0,
          "duration floors incomplete seconds");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    static_assert(!std::is_copy_assignable_v<QsoRecordingFormat>);
    const QsoRecordingFormat early;
    check(early.sampleRateHz() == 24000,
          "start without RX metadata (including TX-first) freezes legacy24");
    verifyHeader(early, 24000);

    PcmProducer producer;
    check(producer.start(), "start legacy24 producer");
    const std::optional<PcmFrame> initial = producer.produce({0.25f, -0.5f});
    check(initial.has_value(), "publish typed24 metadata");
    if (!initial) {
        return 1;
    }
    const QsoRecordingFormat typed24(*initial);
    verifyHeader(typed24, 24000);

    check(producer.setFormat({48000, PcmLayout::Stereo}), "change RX to48");
    const std::optional<PcmFrame> wide = producer.produce({0.25f, -0.5f});
    check(wide.has_value(), "publish typed48 metadata");
    if (!wide) {
        return 1;
    }
    const QsoRecordingFormat typed48(*wide);
    verifyHeader(typed48, 48000);
    check(early.sampleRateHz() == 24000 && typed24.sampleRateHz() == 24000,
          "later RX48 never relabels an early or legacy file");
    check(QsoRecordingFormat(*initial).sampleRateHz() == 24000,
          "revoked observation cannot select a new format");

    check(producer.setFormat({24000, PcmLayout::Mono}), "change RX back to24 mono");
    const std::optional<PcmFrame> narrow = producer.produce({0.25f});
    check(narrow.has_value(), "publish replacement RX24 metadata");
    check(typed48.sampleRateHz() == 48000,
          "24-48-24 epochs cannot change a wide file's format");
    check(QsoRecordingFormat(*wide).sampleRateHz() == 24000,
          "stale48 metadata falls back at next start");
    check(producer.setFormat({48000, PcmLayout::Mono}), "change to48 mono");
    const std::optional<PcmFrame> mono = producer.produce({0.25f});
    check(mono && QsoRecordingFormat(*mono).sampleRateHz() == 48000,
          "live mono48 selects stereo48 file");

    PcmProducer auxiliary;
    check(auxiliary.start(PcmPurpose::Auxiliary, -1, {48000, PcmLayout::Stereo}),
          "start unrelated auxiliary source");
    const std::optional<PcmFrame> aux = auxiliary.produce({0.1f, 0.2f});
    check(aux && QsoRecordingFormat(*aux).sampleRateHz() == 24000,
          "auxiliary metadata cannot select normalized RX recording format");
    producer.invalidate();
    check(mono && QsoRecordingFormat(*mono).sampleRateHz() == 24000,
          "retired producer falls back on TX-first or disconnected start");

    const quint64 maxBytes = 4294967256ULL;
    check(QsoRecordingFormat::kMaxDataBytes == maxBytes,
          "maximum complete stereo payload leaves room for RIFF metadata");
    check(typed48.wavHeader(maxBytes).has_value(), "largest valid RIFF is encoded");
    check(!typed48.wavHeader(maxBytes + 4), "RIFF overflow fails closed");
    check(!typed48.wavHeader(std::numeric_limits<quint64>::max()),
          "huge payload cannot wrap RIFF length");
    check(!typed48.wavHeader(3) && !typed48.durationSecs(3),
          "partial PCM frame cannot become valid container metadata");
    check(typed48.wavHeader(0).has_value() && typed48.durationSecs(0) == 0,
          "initial header and zero captured duration are valid");

    qInfo() << checks << "checks," << failures << "failures";
    return failures == 0 ? 0 : 1;
}
