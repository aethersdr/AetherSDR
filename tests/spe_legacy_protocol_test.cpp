#include "core/SpeLegacyProtocol.h"
#include "core/SpeProtocol.h"

#include <QByteArray>
#include <QList>

#include <cmath>
#include <cstdio>

using namespace AetherSDR::Spe;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

// Wraps a payload the way the 1K-FA does: 0xAA x3, CNT, data, 1-byte
// sum. Built independently of the parser's own math so the two check
// each other.
QByteArray ampFrame(const QByteArray& payload)
{
    int sum = 0;
    for (char c : payload) {
        sum += static_cast<quint8>(c);
    }
    QByteArray f(3, static_cast<char>(0xAA));
    f.append(static_cast<char>(payload.size()));
    f.append(payload);
    f.append(static_cast<char>(sum & 0xFF));
    return f;
}

void putLe16(QByteArray& d, int offset, int value)
{
    d[offset] = static_cast<char>(value & 0xFF);
    d[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
}

// A 30-byte Status payload with the fields this test reads; offsets per the
// Rev 2.0 table (packet index minus the 4-byte header).
QByteArray statusPayload(quint8 flags, quint8 displayCtx, quint8 bandInput, quint8 catAnt,
                         int swrOrGain, int temp, int paOut10, int rev10, int volt10,
                         int amp10)
{
    QByteArray d(Legacy::kStatusDataLength, '\0');
    d[0] = static_cast<char>(0xA0);
    d[1] = static_cast<char>(flags);
    d[2] = static_cast<char>(displayCtx);
    d[14] = static_cast<char>(bandInput);
    d[18] = static_cast<char>(catAnt);
    putLe16(d, 19, swrOrGain);
    d[21] = static_cast<char>(temp);
    putLe16(d, 22, paOut10);
    putLe16(d, 24, rev10);
    putLe16(d, 26, volt10);
    putLe16(d, 28, amp10);
    return d;
}

}  // namespace

int main()
{
    // ── Host -> amp framing ──────────────────────────────────────────────
    report("RCU_ON is 55 55 55 01 80 80 (no CR LF)",
           Legacy::buildRcuOn() == QByteArray::fromHex("555555018080"));
    report("RCU_OFF is 55 55 55 01 81 81",
           Legacy::buildRcuOff() == QByteArray::fromHex("555555018181"));
    report("OPERATE key is KEY_ON 0x10 + 0x1C, checksum 0x2C",
           Legacy::buildKeyCommand(Key::Operate) == QByteArray::fromHex("55555502101c2c"));
    report("TUNE key code is 0x34", Legacy::keyCode(Key::Tune) == 0x34);
    report("POWER maps to the 1K-FA MODE key (HALF/FULL)", Legacy::keyCode(Key::Power) == 0x1A);
    report("SWITCH OFF maps to 0x18", Legacy::keyCode(Key::SwitchOff) == 0x18);
    {
        bool allMapped = true;
        for (int k = static_cast<int>(Key::Input); k <= static_cast<int>(Key::Set); ++k) {
            if (Legacy::keyCode(static_cast<Key>(k)) == 0x00) {
                allMapped = false;
            }
        }
        report("every shared key has a 1K-FA code", allMapped);
    }

    // ── Amp -> host parsing ──────────────────────────────────────────────
    const QByteArray standby = statusPayload(
        /*flags*/ 0x80 | 0x10, /*ctx*/ 0x01, /*band 20m, IN2*/ 0x41, /*ANT3*/ 0x02,
        /*SWR 1.50*/ 150, /*temp*/ 34, /*exciter 25.0 W*/ 250, /*rev*/ 0,
        /*48.3 V*/ 483, /*1.2 A*/ 12);
    {
        QList<Legacy::Frame> frames;
        Legacy::FrameParser p;
        p.setFrameCallback([&](const Legacy::Frame& f) { frames.append(f); });
        const QByteArray stream = QByteArray::fromHex("00ff13")  // line noise
            + ampFrame(QByteArray(1, static_cast<char>(Legacy::kAck)))
            + ampFrame(standby);
        // Byte-at-a-time, as a slow serial read delivers it.
        for (char c : stream) {
            p.feed(QByteArray(1, c));
        }
        report("parser yields ACK then Status from a byte-wise stream after noise",
               frames.size() == 2 && frames.at(0).isReply() && frames.at(1).isStatus());
    }
    {
        QList<Legacy::Frame> frames;
        Legacy::FrameParser p;
        p.setFrameCallback([&](const Legacy::Frame& f) { frames.append(f); });
        QByteArray bad = ampFrame(standby);
        bad[10] = static_cast<char>(bad.at(10) ^ 0x01);  // corrupt one data byte
        p.feed(bad + ampFrame(standby));
        report("a checksum failure drops one frame and resyncs to the next",
               frames.size() == 1 && frames.at(0).data == standby);
    }
    {
        QList<Legacy::Frame> frames;
        Legacy::FrameParser p;
        p.setFrameCallback([&](const Legacy::Frame& f) { frames.append(f); });
        p.feed(QByteArray::fromHex("aaaaaa45") + ampFrame(standby));
        report("an undefined CNT resyncs instead of waiting for 69 bytes",
               frames.size() == 1);
    }

    // ── Status decode ────────────────────────────────────────────────────
    {
        const auto s = Legacy::parseStatus(standby);
        report("STANDBY status decodes", s.has_value());
        if (s) {
            report("model ID is the synthetic 10K", s->id == QLatin1String("10K"));
            report("STANDBY, not transmitting", !s->operate && !s->transmitting);
            report("FULL maps to power level H", s->powerLevel == u'H');
            report("legacy band 4 (20m) maps onto the shared table",
                   bandName(s->bandIndex) == QLatin1String("20m"));
            report("input nibble 1 is IN2", s->input == 2);
            report("antenna nibble 2 is ANT 3", s->txAntenna == 3);
            report("STANDBY SWR comes straight from the field", near(s->swrAnt, 1.5f));
            report("no before-ATU SWR on the 1K-FA", s->swrAtu == 0.0f);
            report("power, voltage, current, temperature scale by 10/10/10/1",
                   near(s->outputPowerW, 25.0f) && near(s->paVoltageV, 48.3f)
                       && near(s->paCurrentA, 1.2f) && s->tempUpper == 34);
            report("no warning or alarm text", s->warningDetail.isEmpty()
                       && s->alarmDetail.isEmpty());
        }
    }
    {
        // OPERATE + TX + HALF, 40m, 400 W out, 16 W reflected -> rho 0.2 -> 1.5:1.
        const QByteArray tx = statusPayload(0x80 | 0x04 | 0x02 | 0x08, 0x17, 0x20, 0x00,
                                            /*gain 13.0 dB*/ 130, 51, 4000, 160, 470, 280);
        const auto s = Legacy::parseStatus(tx);
        report("OPERATE status decodes", s.has_value());
        if (s) {
            report("OPERATE + TX flags", s->operate && s->transmitting);
            report("HALF maps to power level L", s->powerLevel == u'L');
            report("legacy band 2 (40m) skips the shared table's 60m",
                   bandName(s->bandIndex) == QLatin1String("40m"));
            report("OPERATE SWR is derived from forward/reverse, not the gain field",
                   near(s->swrAnt, 1.5f));
            report("display context 0x17 is the temperature warning",
                   s->warningDetail == QLatin1String("Temperature high"));
            report("ALARM bit sets alarm text", !s->alarmDetail.isEmpty());
        }
    }
    {
        const QByteArray inf = statusPayload(0x00, 0x01, 0x90, 0x04, 9999, 30, 50, 0, 480, 0);
        const auto s = Legacy::parseStatus(inf);
        report("STANDBY infinite SWR is capped, 6m and no antenna decode",
               s && near(s->swrAnt, 99.9f) && bandName(s->bandIndex) == QLatin1String("6m")
                   && s->txAntenna == 0);
    }
    report("a short payload is rejected", !Legacy::parseStatus(QByteArray(29, '\0')));

    report("SWR from power: no forward power is 0",
           Legacy::swrFromPower(0.0f, 5.0f) == 0.0f);
    report("SWR from power: full reflection is capped",
           near(Legacy::swrFromPower(100.0f, 100.0f), 99.9f));

    // ── Variant persistence ──────────────────────────────────────────────
    report("variant key round-trips",
           variantFromKey(variantKey(Variant::Legacy1k)) == Variant::Legacy1k
               && variantFromKey(variantKey(Variant::Expert)) == Variant::Expert);
    report("an empty or unknown key reads as the newer family",
           variantFromKey(QString()) == Variant::Expert
               && variantFromKey(QStringLiteral("bogus")) == Variant::Expert);
    report("serial speed: 9600 for the 1K-FA, 115200 otherwise",
           serialBaud(Variant::Legacy1k) == 9600 && serialBaud(Variant::Expert) == 115200);

    std::printf("\n%d SPE legacy protocol test(s) failed.\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
