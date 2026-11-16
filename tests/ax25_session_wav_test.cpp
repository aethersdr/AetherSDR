#include "Ax25SessionWav.h"

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

QByteArray wav(const QByteArray& payload, bool oddJunk, quint32 rate = 48000)
{
    // WAVE_FORMAT_IEEE_FLOAT, mono, 4-byte blocks, 32 bits.
    QByteArray format = QByteArray::fromHex("0300010080bb000000ee020004002000");
    qToLittleEndian(rate, format.data() + 4);
    qToLittleEndian(rate * 4, format.data() + 8);
    QByteArray body("WAVE", 4);
    body += chunk("fmt ", format);
    if (oddJunk) {
        body += chunk("JUNK", QByteArray("abc"));
    }
    body += chunk("data", payload);
    return chunk("RIFF", body);
}

} // namespace

class Ax25SessionWavTest : public QObject {
    Q_OBJECT

private slots:
    void rejectsPartialSamples_data()
    {
        QTest::addColumn<int>("length");
        for (int length = 1; length <= 11; ++length) {
            if (length % 4 != 0) {
                QTest::newRow(qPrintable(QString::number(length))) << length;
            }
        }
    }

    void rejectsPartialSamples()
    {
        QFETCH(int, length);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("partial.wav"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = wav(QByteArray(length, '\0'), false);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        std::vector<float> samples;
        int sampleRate = 0;
        QString error;
        QVERIFY(!AetherSDR::Ax25SessionWav::loadWav(path, samples, sampleRate, error));
        QCOMPARE(error, QStringLiteral("data chunk length %1 is not a multiple of 4 (float32 sample size)").arg(length));
        QVERIFY(samples.empty());
    }

    void rejectsImplausibleSampleRates_data()
    {
        QTest::addColumn<quint32>("rate");
        QTest::newRow("zero") << quint32(0);
        QTest::newRow("one") << quint32(1);
        QTest::newRow("eight") << quint32(8);
        QTest::newRow("nine") << quint32(9);
        QTest::newRow("below-floor") << quint32(7999);
        QTest::newRow("above-ceiling") << quint32(384001);
    }

    void rejectsImplausibleSampleRates()
    {
        QFETCH(quint32, rate);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("implausible.wav"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = wav(QByteArray(4, '\0'), false, rate);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();

        std::vector<float> samples;
        int sampleRate = 0;
        QString error;
        QVERIFY(!AetherSDR::Ax25SessionWav::loadWav(path, samples, sampleRate, error));
        QCOMPARE(error, QStringLiteral("sample rate %1 Hz is outside the supported 8000-384000 Hz range")
                            .arg(rate));
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
        QVERIFY2(AetherSDR::Ax25SessionWav::loadWav(path, samples, sampleRate, error), qPrintable(error));
        QCOMPARE(sampleRate, 48000);
        QCOMPARE(samples.size(), static_cast<size_t>(count));
        QCOMPARE(std::memcmp(samples.data(), payload.constData(), static_cast<size_t>(payload.size())), 0);
    }
};

QTEST_GUILESS_MAIN(Ax25SessionWavTest)
#include "ax25_session_wav_test.moc"
