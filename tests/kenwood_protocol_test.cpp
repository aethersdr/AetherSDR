#include "core/backends/kenwood/KnsProtocol.h"
#include "core/backends/kenwood/KenwoodFilter.h"
#include "core/backends/kenwood/KenwoodPowerLimits.h"

#include <QChar>
#include <QCoreApplication>
#include <QtEndian>

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace AetherSDR::kenwood;

namespace {
void require(bool condition, const char* description)
{
    if (!condition) {
        std::cerr << description << '\n';
        std::exit(1);
    }
}
void authenticate(KnsProtocol& protocol)
{
    require(protocol.start("user", "secret", true, 0).outgoing == QList<QByteArray>{"##CN;"}, "greeting");
    require(protocol.receive("##C", 1).outgoing.isEmpty(), "fragment retained");
    require(protocol.receive("N1;", 2).outgoing == QList<QByteArray>{"##ID00406usersecret;"}, "admin login framing");
    const KnsProtocol::Events event = protocol.receive("##ID1;FA00014074000;", 3);
    require(event.authenticated && event.messages == QList<QByteArray>{"FA00014074000"}, "coalesced login and state");
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const QList<QByteArray> inputPairs{"10", "01", "02", "03", "11", "12", "13"};
    require(transmitAudioInputChoices().size() == 7, "all native front/rear combinations are named");
    for (int path = 0; path < 2; ++path) {
        for (int selection = 0; selection < 7; ++selection) {
            const QByteArray expected = "MS" + QByteArray::number(path) + inputPairs[selection];
            require(encodeTransmitAudioInput({path, selection}) == expected, "independent SEND/DATA wire mapping");
            const std::optional<TransmitAudioInput> input = decodeTransmitAudioInput(expected);
            require(input && input->path == path && input->selection == selection, "native audio pair round trip");
        }
    }
    for (const QByteArray& invalid : {QByteArray("MS000"), QByteArray("MS100"), QByteArray("MS099"), QByteArray("MS199"),
         QByteArray("MS203"), QByteArray("MS023"), QByteArray("MS014"), QByteArray("MS1A3"), QByteArray("MS103;"), QByteArray("MS10")}) {
        require(!decodeTransmitAudioInput(invalid), "invalid audio pairs and reset values are rejected");
    }
    require(!encodeTransmitAudioInput({-1, 0}) && !encodeTransmitAudioInput({2, 0})
        && !encodeTransmitAudioInput({0, -1}) && !encodeTransmitAudioInput({1, 7}), "input request bounds");
    KnsProtocol protocol;
    authenticate(protocol);
    const QByteArray memory = QByteArray("MA000000014074000200001000000000000000000")
        + QString::fromUtf8("München").toUtf8();
    const qsizetype split = memory.indexOf(char(0xc3)) + 1;
    require(protocol.receive(memory.first(split), 4).error.isEmpty(), "UTF-8 codepoint may span reads");
    const KnsProtocol::Events text = protocol.receive(memory.sliced(split) + ";FA00007074000;", 5);
    require(text.error.isEmpty() && text.messages == QList<QByteArray>{memory, "FA00007074000"}
        && protocol.state() == KnsProtocol::State::Ready, "UTF-8 status preserves framing and following numeric status");
    for (const QByteArray& invalid : {QByteArray::fromHex("4d41c33b"), QByteArray::fromHex("4d41c0af3b"),
         QByteArray::fromHex("4d41eda0803b"), QByteArray::fromHex("4d41803b"), QByteArray::fromHex("4d417f3b")}) {
        authenticate(protocol);
        require(!protocol.receive(invalid, 4).error.isEmpty(), "malformed or incomplete UTF-8 and ASCII controls are refused");
    }
    authenticate(protocol);
    require(protocol.tick(5003).outgoing == QList<QByteArray>{"PS;"}, "keepalive");
    require(!protocol.tick(15003).error.isEmpty(), "unanswered keepalive times out");
    require(protocol.receive("FA00014074000;", 16000).messages.isEmpty(), "failed session cannot publish");
    authenticate(protocol);
    require(!protocol.receive(QByteArray(4097, 'A'), 4).error.isEmpty(), "oversized frame rejected");
    authenticate(protocol);
    require(!protocol.receive(QByteArray("A\0B;", 4), 4).error.isEmpty(), "binary injection rejected");
    protocol.start("user", "secret", false, 0);
    require(!protocol.receive("##CN0;", 1).error.isEmpty(), "busy radio refused");
    protocol.start("user", "secret", false, 0);
    require(protocol.receive("##CN1;", 1).outgoing == QList<QByteArray>{"##ID10406usersecret;"}, "standard role");
    require(!protocol.receive("##ID0;", 2).error.isEmpty(), "wrong password refused");
    protocol.start("user", "secret", false, 0);
    require(!protocol.tick(5000).error.isEmpty(), "login deadline");
    require(KnsProtocol::credentialsValid("user", "secret")
        && KnsProtocol::credentialsValid(QString(32, QLatin1Char('u')), QString(32, QLatin1Char('p')))
        && !KnsProtocol::credentialsValid("", "secret") && !KnsProtocol::credentialsValid("user", "")
        && !KnsProtocol::credentialsValid("user", "bad;TX1") && !KnsProtocol::credentialsValid("user", "line\nfeed")
        && !KnsProtocol::credentialsValid("user", QString(1, QChar(0x017e)))
        && !KnsProtocol::credentialsValid("user", QString(4096, QLatin1Char('x'))), "credential bounds are shared with connection preflight");
    require(!protocol.start("user", "x;TX1", false, 0).error.isEmpty(), "credential injection refused");
    require(!KnsProtocol::command("FA;TX1"), "command injection refused");
    authenticate(protocol);
    protocol.receive("FA000", 4);
    protocol.reset();
    authenticate(protocol);
    require(protocol.receive("FA00007074000;", 4).messages.size() == 1, "new session retires partial tail");

    require(decodeFrequency("FA00014074000") == 14074000, "frequency parse");
    require(queryPowerLimit(5, 5) == QByteArray("LP1505")
        && encodePowerLimit({5, 5, 10}) == QByteArray("LP1505010"), "documented independent TX-tuning power row");
    for (const qint64 hz : {qint64(1800000), qint64(2000000)}) { require(powerLimitBandForFrequency(hz) == 0, "160m native column endpoints"); }
    require(powerLimitBandForFrequency(5167500) == 2 && !powerLimitBandForFrequency(5167499)
        && !powerLimitBandForFrequency(5167501) && powerLimitBandForFrequency(5250000) == 2
        && powerLimitBandForFrequency(5450000) == 2, "discrete 60m channel and range do not fill the gap");
    require(powerLimitBandForFrequency(4000000) == 1 && powerLimitBandForFrequency(7300000) == 3
        && powerLimitBandForFrequency(10150000) == 4 && powerLimitBandForFrequency(14350000) == 5
        && powerLimitBandForFrequency(18168000) == 6 && powerLimitBandForFrequency(21450000) == 7
        && powerLimitBandForFrequency(24990000) == 8 && powerLimitBandForFrequency(29700000) == 9
        && powerLimitBandForFrequency(54000000) == 10 && powerLimitBandForFrequency(70500000) == 11,
        "documented TX range endpoints identify all native power columns");
    for (const qint64 hz : {qint64(-1), qint64(30000), qint64(2000001), qint64(4999999), qint64(6000000),
         qint64(10150001), qint64(14350001), qint64(54000001), qint64(70500001), std::numeric_limits<qint64>::max()}) {
        require(!powerLimitBandForFrequency(hz), "general coverage and out-of-range frequencies never inherit a nearby power column");
    }
    const PowerLimit power = *decodePowerLimit("LP1505010");
    require(power.type == 5 && power.band == 5 && power.watts == 10
        && powerLimitBands()[5] == "14 MHz" && powerLimitBands()[11] == "70 MHz"
        && powerLimitTypes()[5] == "TX tuning", "power-limit row/column identity is explicit");
    for (int type = 0; type < 6; ++type) {
        for (int band = 0; band < 12; ++band) {
            for (int watts : {5, 100}) {
                const PowerLimit limit = *decodePowerLimit(*encodePowerLimit({type, band, watts}));
                require(limit.type == type && limit.band == band && limit.watts == watts, "all documented power-limit endpoints round trip");
            }
        }
    }
    require(!queryPowerLimit(-1, 0) && !queryPowerLimit(6, 0) && !queryPowerLimit(0, 12)
        && !encodePowerLimit({5, 5, 4}) && !encodePowerLimit({5, 5, 101})
        && !encodePowerLimit({5, 5, 999}), "unassigned rows, bands, values and reset sentinel are not exposed");
    require(!decodePowerLimit("LP1605010") && !decodePowerLimit("LP1512010")
        && !decodePowerLimit("LP1505999") && !decodePowerLimit("LP15050A0")
        && !decodePowerLimit("LP15050100"), "malformed native power reports are bounded before publication");
    require(maximumFrequencyForVariant("") == 60000000
        && maximumFrequencyForVariant("k") == 60000000
        && maximumFrequencyForVariant("e") == 74800000
        && !maximumFrequencyForVariant("unknown"), "explicit regional tuning limits");
    require(decodeFrequency("FA00074800000") == 74800000
        && !decodeFrequency("FA00074800001"), "regional receive endpoint decoding");
    require(encodeFrequency('A', 74800000) == QByteArray("FA00074800000")
        && !encodeFrequency('A', 74800001) && !encodeFrequency('A', 29999),
        "documented receive frequency bounds");
    require(!decodeFrequency("FA00014+74000") && !decodeFrequency("FA99999999999"), "invalid frequency");
    require(encodeFrequency('B', 7074000) == QByteArray("FB00007074000"), "frequency encode");
    require(!encodeFrequency('C', 7074000), "invalid VFO");
    require(decodeMode('D') == QStringLiteral("DIGU") && encodeMode("DIGL") == 'C', "data mode mapping");
    require(!decodeMode('8') && !encodeMode("WFM"), "unknown modes refused");
    const QByteArray status = "IF00021195340      000000000030000000";
    require(decodeStatusTransmit(status) == false, "firmware 1.01 captured IF status reports RX");
    QByteArray transmitting = status;
    transmitting[28] = '1';
    require(decodeStatusTransmit(transmitting) == true, "compatibility state bit reports TX without inventing its source");
    for (qsizetype size = 0; size < status.size(); ++size) {
        require(!decodeStatusTransmit(status.first(size)), "truncated IF cannot qualify receive state");
    }
    require(!decodeStatusTransmit(status + '0'), "overlong IF is not the qualified layout");
    for (qsizetype index = 0; index < status.size(); ++index) {
        QByteArray invalid = status;
        invalid[index] = '?';
        require(!decodeStatusTransmit(invalid), "every IF field is bounded before extracting RX/TX");
    }
    for (char invalidBit : {'2', '9', ' '}) {
        QByteArray invalid = status;
        invalid[28] = invalidBit;
        require(!decodeStatusTransmit(invalid), "unknown state bit does not imply RX");
    }
    const VfoState physicalA = *decodeVfoState("SF0000140740002");
    const VfoState physicalB = *decodeVfoState("SF1000070740007");
    require(physicalA.vfo == 0 && physicalA.frequencyHz == 14074000 && physicalA.mode == "USB"
        && physicalB.vfo == 1 && physicalB.frequencyHz == 7074000 && physicalB.mode == "CWR",
        "SF reports physical VFO identity, frequency and mode");
    for (const QByteArray& malformed : {QByteArray("SF2000140740002"), QByteArray("SF0000140740008"),
             QByteArray("SF000014+740002"), QByteArray("SF0000748000012"), QByteArray("SF000014074000"),
             QByteArray("SF00001407400022")}) {
        require(!decodeVfoState(malformed), "invalid physical VFO report cannot enter live state");
    }
    require(txFilterGroup("USB") == 0 && txFilterGroup("LSB") == 0 && txFilterGroup("AM") == 0
        && txFilterGroup("DIGU") == 1 && txFilterGroup("DIGL") == 1 && txFilterGroup("DAM") == 1,
        "TX filter menu group follows native normal/data transmit mode");
    require(!txFilterGroup("CW") && !txFilterGroup("FM") && !txFilterGroup("RTTY") && !txFilterGroup("PSK")
        && !txFilterGroup("DFM") && !txFilterGroup(""), "unsupported TX modes have no fabricated filter");
    for (int group = 0; group < 2; ++group) {
        for (int low : txFilterLowEdges()) {
            for (int high : txFilterHighEdges()) {
                const QList<QByteArray> encoded = *encodeTxFilter(group, low, high);
                const TxFilterReport lowReport = *decodeTxFilter(encoded[0]);
                const TxFilterReport highReport = *decodeTxFilter(encoded[1]);
                require(lowReport.group == group && !lowReport.high && lowReport.hz == low
                    && highReport.group == group && highReport.high && highReport.hz == high,
                    "all native TX filter edge pairs round trip independently");
            }
        }
    }
    require(encodeTxFilter(1, 10, 4000) == QList<QByteArray>{"EX00608 000", "EX00609 007"}
        && queryTxFilter(0) == QList<QByteArray>{"EX00606", "EX00607"}, "TX filter uses documented EX menu syntax");
    require(!encodeTxFilter(2, 100, 3000) && !encodeTxFilter(0, 0, 3000) && !encodeTxFilter(1, 100, 3400)
        && !decodeTxFilter("EX00606 006") && !decodeTxFilter("EX00607 008")
        && !decodeTxFilter("EX00608 00A") && !decodeTxFilter("EX00609 0000")
        && !decodeTxFilter("EX00610 000"), "unknown TX menu edges and malformed replies are rejected");
    const FilterEdges usb = *decodeFilter("USB", 0, 4, 24);
    require(usb.lowHz == 300 && usb.highHz == 3000, "official SSB high/low table");
    const FilterEdges lsb = *decodeFilter("LSB", 0, 4, 24);
    require(lsb.lowHz == -3000 && lsb.highHz == -300, "LSB passband reflection");
    const FilterEdges cw = *decodeFilter("CW", 0, 16, 80);
    require(cw.lowHz == -750 && cw.highHz == 750, "official CW width index 16 is 1500 Hz");
    require(!decodeFilter("CW", 0, 19, 80), "CW width table bounds");
    require(!decodeFilter("USB", -1, 0, 0), "unknown SSB display style is not guessed");
    const QList<QByteArray> cuts = *encodeFilter("LSB", 0, {-3000, -300});
    require(cuts == QList<QByteArray>{"SL004", "SH0024"}, "LSB filter request uses radio cut indexes");
    require(!encodeFilter("USB", 0, {3000, 300}), "inverted passband refused");
    const NativeFilterLayout am = *nativeFilterLayout("AM", -1);
    require(am.low.valuesHz == QVector<int>{0, 100, 200, 300}
        && am.high.valuesHz.size() == 14 && am.high.valuesHz[11] == 3500
        && am.high.valuesHz.last() == 5000, "official AM audio-cut tables");
    require(nativeFilterLayout("DAM", -1)->high.valuesHz == am.high.valuesHz,
        "AM data uses the documented AM cut table");
    const NativeFilterLayout fm = *nativeFilterLayout("DFM", -1);
    require(fm.low.valuesHz.size() == 12 && fm.low.valuesHz.last() == 1000
        && fm.high.valuesHz.size() == 24 && fm.high.valuesHz[21] == 3400,
        "official FM/data audio-cut tables");
    require(nativeFilterLayout("RTTYR", -1)->low.valuesHz == QVector<int>{250, 300, 350, 400, 450, 500, 1000, 1500}
        && nativeFilterLayout("RTTY", -1)->high.valuesHz.isEmpty(), "FSK is width-only");
    require(nativeFilterLayout("PSK", -1)->low.valuesHz.size() == 27
        && nativeFilterLayout("PSKR", -1)->low.valuesHz[16] == 1200
        && nativeFilterLayout("PSKR", -1)->high.valuesHz.isEmpty(), "official PSK width table");
    require(nativeFilterLayout("CW", -1)->high.valuesHz.size() == 161
        && nativeFilterLayout("CW", -1)->high.valuesHz[80] == 0
        && !nativeFilterLayout("USB", -1) && !nativeFilterLayout("WFM", 0),
        "native layout requires a known mode and SSB style");
    const QByteArray scope = "##DD2" + QByteArray("46").repeated(640);
    require(decodeScope(scope)->levels.size() == 640 && decodeScope(scope)->levels[0] == -50, "official scope midpoint");
    require(decodeScope("##DD2" + QByteArray("00").repeated(640))->levels[0] == 0, "official scope top");
    require(decodeScope("##DD2" + QByteArray("8C").repeated(640))->levels[0] == -100, "official scope floor");
    require(!decodeScope("##DD2" + QByteArray("8D").repeated(640)), "out-of-range scope level refused");
    require(!decodeScope(scope + '0') && !decodeScope("##DD2" + QByteArray(1280, 'Z')), "scope size/hex validation");

    QByteArray audio(652, '\0');
    auto* bytes = reinterpret_cast<uchar*>(audio.data());
    bytes[0] = 0x80;
    bytes[1] = 96;
    qToBigEndian<quint16>(65535, bytes + 2);
    qToLittleEndian<qint16>(-32768, bytes + 12);
    require(decodeAudio(audio)->sequence == 65535 && decodeAudio(audio)->samples[0] == -1, "RX endian and signed encoding");
    require(!decodeAudio(audio.first(11)) && !decodeAudio(audio + 'x'), "truncated/odd audio refused");
    bytes[0] = 0x90;
    bytes[14] = 0xff;
    bytes[15] = 0xff;
    require(!decodeAudio(audio), "oversized RTP extension refused");
    bytes[0] = 0xa0;
    bytes[651] = 0;
    require(!decodeAudio(audio), "invalid RTP padding refused");
    // Variable RTP headers must keep metadata and padding out of PCM.
    for (int csrcCount = 0; csrcCount <= 15; ++csrcCount) {
        for (int extensionWords = 0; extensionWords <= 4; ++extensionWords) {
            for (int padding = 1; padding <= 4; ++padding) {
                const int extensionOffset = 12 + 4 * csrcCount;
                const int payloadOffset = extensionOffset + 4 + extensionWords * 4;
                QByteArray framed(payloadOffset + 4 + padding, char(0x55));
                framed[0] = char(0xb0 | csrcCount);
                framed[1] = char(0xe0); // Marker does not change payload type 96.
                qToBigEndian<quint16>(quint16(extensionWords), framed.data() + extensionOffset + 2);
                qToLittleEndian<qint16>(-16384, framed.data() + payloadOffset);
                qToLittleEndian<qint16>(16384, framed.data() + payloadOffset + 2);
                framed.back() = char(padding);
                const std::optional<AudioPacket> decoded = decodeAudio(framed);
                require(decoded && decoded->samples == QVector<float>{-0.5f, 0.5f},
                    "RTP CSRC, extension and padding cannot enter decoded PCM");
                for (int length = 0; length <= payloadOffset; ++length) {
                    require(!decodeAudio(framed.first(length)), "truncated variable RTP header has no audio");
                }
                qToBigEndian<quint16>(65535, framed.data() + extensionOffset + 2);
                require(!decodeAudio(framed), "extension length cannot escape bounded datagram");
            }
        }
    }
    QVector<float> samples(320, 0);
    const QByteArray tx = *encodeTxAudio(samples, 17, 0.01f);
    require(tx.size() == 652 && qFromBigEndian<quint16>(tx.constData() + 2) == 17
        && qFromBigEndian<quint16>(tx.constData() + 12) == 32768, "TX offset-binary silence");
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    require(!encodeTxAudio(samples, 0, 0.01f), "non-finite TX refused");
    require(!encodeTxAudio(QVector<float>(321), 0, 0.01f), "TX exact packet duration");
    AudioSequence sequence;
    require(sequence.accept(65535, 1, 0) == AudioSequence::Result::First, "sequence first");
    require(sequence.accept(0, 1, 20) == AudioSequence::Result::Continuous, "sequence wraps");
    require(sequence.accept(0, 1, 40) == AudioSequence::Result::Drop, "duplicate discarded");
    require(sequence.accept(2, 1, 60) == AudioSequence::Result::Gap, "loss detected");
    require(sequence.accept(1, 1, 80) == AudioSequence::Result::Drop, "reordered packet discarded");
    require(sequence.accept(3, 1, 100) == AudioSequence::Result::Continuous, "reordering cannot rewind sequence");
    require(sequence.accept(0, 2, 120) == AudioSequence::Result::Restart, "new SSRC restarts");
    require(sequence.accept(0, 2, 2000) == AudioSequence::Result::Restart, "silence permits stream restart");
    AudioSequence restarted;
    require(restarted.accept(1000, 1, 0) == AudioSequence::Result::First, "restart fixture establishes the old stream");
    for (quint16 index = 0; index < 9; ++index) {
        require(restarted.accept(index, 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "a short stale sequence run cannot yet replace live numbering");
    }
    require(restarted.accept(9, 1, 200) == AudioSequence::Result::Restart,
        "same-SSRC sequence reset recovers after bounded sequential proof instead of dropping 49 fresh packets");
    require(restarted.accept(10, 1, 220) == AudioSequence::Result::Continuous,
        "same-SSRC recovery follows the new sequence immediately");

    AudioSequence duplicates;
    require(duplicates.accept(1000, 1, 0) == AudioSequence::Result::First, "duplicate fixture establishes live numbering");
    for (int index = 0; index < 20; ++index) {
        require(duplicates.accept(0, 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "repeated stale duplicates cannot be mistaken for a restarted stream");
    }
    require(duplicates.accept(1001, 1, 420) == AudioSequence::Result::Continuous,
        "duplicate restart candidates cannot rewind the live sequence");
    for (quint16 index = 0; index < 9; ++index) {
        require(duplicates.accept(index, 1, 440 + index * 20) == AudioSequence::Result::Drop,
            "accepted live traffic clears any previous restart proof");
    }
    require(duplicates.accept(9, 1, 620) == AudioSequence::Result::Restart,
        "fresh sustained restart proof remains eligible after duplicate rejection");

    AudioSequence disordered;
    require(disordered.accept(1000, 1, 0) == AudioSequence::Result::First, "disorder fixture establishes live numbering");
    for (int index = 0; index < 20; ++index) {
        require(disordered.accept(index % 2 == 0 ? 0 : 5, 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "nonsequential stale traffic cannot accumulate restart proof");
    }
    require(disordered.accept(1001, 1, 420) == AudioSequence::Result::Continuous,
        "disordered candidates preserve the original expected packet");
    require(disordered.accept(30000, 1, 440) == AudioSequence::Result::Gap,
        "large forward jumps remain accepted discontinuities without waiting for restart proof");
    require(disordered.accept(30001, 1, 460) == AudioSequence::Result::Continuous,
        "large forward jumps establish the next live sequence");

    AudioSequence wrappedRestart;
    require(wrappedRestart.accept(1000, 1, 0) == AudioSequence::Result::First, "wrapped restart fixture establishes live numbering");
    for (int index = 0; index < 9; ++index) {
        require(wrappedRestart.accept(static_cast<quint16>(65530 + index), 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "restart candidates compare sequential values across wrap");
    }
    require(wrappedRestart.accept(3, 1, 200) == AudioSequence::Result::Restart
        && wrappedRestart.accept(4, 1, 220) == AudioSequence::Result::Continuous,
        "bounded restart proof handles a candidate sequence wrapping through zero");
    AudioSequence interrupted;
    require(interrupted.accept(1000, 1, 0) == AudioSequence::Result::First, "interruption fixture establishes live numbering");
    for (quint16 index = 0; index < 9; ++index) {
        require(interrupted.accept(index, 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "partial restart proof remains tentative");
    }
    require(interrupted.accept(1001, 1, 200) == AudioSequence::Result::Continuous,
        "fresh current traffic cancels a nearly complete restart candidate");
    for (quint16 index = 9; index < 18; ++index) {
        require(interrupted.accept(index, 1, 220 + (index - 9) * 20) == AudioSequence::Result::Drop,
            "stale numbering cannot continue proof across accepted live traffic");
    }
    require(interrupted.accept(18, 1, 400) == AudioSequence::Result::Restart,
        "restart proof starts again after live traffic interrupts it");

    AudioSequence sourceChanged;
    require(sourceChanged.accept(1000, 1, 0) == AudioSequence::Result::First, "source fixture establishes live numbering");
    for (quint16 index = 0; index < 9; ++index) {
        require(sourceChanged.accept(index, 1, (index + 1) * 20) == AudioSequence::Result::Drop,
            "source fixture accumulates only tentative restart proof");
    }
    require(sourceChanged.accept(1000, 2, 200) == AudioSequence::Result::Restart,
        "SSRC change starts a separate stream immediately");
    for (quint16 index = 9; index < 18; ++index) {
        require(sourceChanged.accept(index, 2, 220 + (index - 9) * 20) == AudioSequence::Result::Drop,
            "new SSRC cannot inherit the previous source's partial restart proof");
    }
    sourceChanged.reset();
    require(sourceChanged.accept(500, 2, 500) == AudioSequence::Result::First,
        "explicit stream reset starts without a previous sequence epoch");
    std::cout << "Kenwood protocol checks passed\n";
}
