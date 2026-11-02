#include "Ax25ReplayWav.h"

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <cstring>
#include <vector>

namespace {

QByteArray chunk(const char* id, const QByteArray& payload)
{
    QByteArray result(id, 4);
    const quint32 length = qToLittleEndian(static_cast<quint32>(payload.size()));
    result.append(reinterpret_cast<const char*>(&length), sizeof(length));
    result.append(payload);
    if (payload.size() % 2 != 0) {
        result.append('\0');
    }
    return result;
}

QByteArray wav(const QByteArray& payload, bool oddJunk,
               quint16 format = 3, quint16 channels = 1, quint16 bits = 32)
{
    // WAVE_FORMAT_IEEE_FLOAT, mono, 48000 Hz, 192000 bytes/s, four-byte blocks.
    QByteArray header = QByteArray::fromHex("0300010080bb000000ee020004002000");
    qToLittleEndian(format, header.data());
    qToLittleEndian(channels, header.data() + 2);
    qToLittleEndian(bits, header.data() + 14);
    QByteArray body("WAVE", 4);
    body += chunk("fmt ", header);
    if (oddJunk) {
        body += chunk("JUNK", QByteArray("abc"));
    }
    body += chunk("data", payload);
    return chunk("RIFF", body);
}

} // namespace

class Ax25ReplayWavTest : public QObject {
    Q_OBJECT

private slots:
    void rejectsPartialSamples_data()
    {
        QTest::addColumn<int>("length");
        QTest::addColumn<bool>("oddJunk");
        for (int length = 1; length <= 11; ++length) {
            if (length % 4 != 0) {
                QTest::newRow(qPrintable(QString::number(length))) << length << false;
            }
        }
        QTest::newRow("padded-junk") << 6 << true;
    }

    void rejectsPartialSamples()
    {
        QFETCH(int, length);
        QFETCH(bool, oddJunk);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("partial.wav"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = wav(QByteArray(length, '\0'), oddJunk);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        std::vector<float> samples;
        int sampleRate = 0;
        QString error;
        QVERIFY(!AetherSDR::Ax25ReplayWav::loadWav(path, samples, sampleRate, error));
        QCOMPARE(error, QStringLiteral("data chunk length %1 is not a multiple of 4 (float32 sample size)").arg(length));
        QVERIFY(samples.empty());
    }

    void loadsWholeSamples_data()
    {
        QTest::addColumn<int>("count");
        QTest::addColumn<bool>("oddJunk");
        QTest::newRow("one") << 1 << false;
        QTest::newRow("two") << 2 << false;
        QTest::newRow("three") << 3 << false;
        QTest::newRow("padded-junk") << 3 << true;
    }

    void loadsWholeSamples()
    {
        QFETCH(int, count);
        QFETCH(bool, oddJunk);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("complete.wav"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        // Float32 encodings of 0.5, -0.25, and 1.0, in WAV's little-endian order.
        const QByteArray payload = QByteArray::fromHex("0000003f000080be0000803f").first(count * 4);
        const QByteArray bytes = wav(payload, oddJunk);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        std::vector<float> samples;
        int sampleRate = 0;
        QString error;
        QVERIFY2(AetherSDR::Ax25ReplayWav::loadWav(path, samples, sampleRate, error), qPrintable(error));
        QCOMPARE(sampleRate, 48000);
        QCOMPARE(samples.size(), static_cast<size_t>(count));
        QCOMPARE(std::memcmp(samples.data(), payload.constData(), static_cast<size_t>(payload.size())), 0);
    }

    void rejectsUnsupportedFormats_data()
    {
        QTest::addColumn<quint16>("format");
        QTest::addColumn<quint16>("channels");
        QTest::addColumn<quint16>("bits");
        QTest::newRow("pcm") << quint16(1) << quint16(1) << quint16(32);
        QTest::newRow("stereo") << quint16(3) << quint16(2) << quint16(32);
        QTest::newRow("16-bit") << quint16(3) << quint16(1) << quint16(16);
    }

    void rejectsUnsupportedFormats()
    {
        QFETCH(quint16, format);
        QFETCH(quint16, channels);
        QFETCH(quint16, bits);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("unsupported.wav"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = wav(QByteArray(8, '\0'), false, format, channels, bits);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        std::vector<float> samples;
        int sampleRate = 0;
        QString error;
        QVERIFY(!AetherSDR::Ax25ReplayWav::loadWav(path, samples, sampleRate, error));
        QCOMPARE(error, QStringLiteral("expected mono float32 WAV (got fmt=%1 ch=%2 bits=%3)")
                            .arg(format).arg(channels).arg(bits));
        QVERIFY(samples.empty());
    }
};

QTEST_GUILESS_MAIN(Ax25ReplayWavTest)
#include "ax25_replay_wav_test.moc"
