// Fill-in digipeater (WIDE1-1) decisions. No DSP, no radio.

#include "core/aprs/AprsFillInDigipeater.h"
#include "core/tnc/Ax25.h"

#include <QCoreApplication>
#include <cstdio>

using namespace AetherSDR;
using AetherSDR::ax25::Address;
using AetherSDR::ax25::Frame;

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

static Frame ui(const QString& src, const QString& dest,
                const QVector<QString>& vias, const QByteArray& info)
{
    QVector<Address> via;
    for (const QString& v : vias) {
        auto a = Address::parse(v);
        CHECK(a.has_value(), "via parses");
        if (a)
            via.push_back(*a);
    }
    return Frame::makeUI(*Address::parse(dest), *Address::parse(src), via, info);
}

static void configureDigi(AprsFillInDigipeater& d)
{
    d.setMyAddress(*Address::parse(QStringLiteral("KI6BCJ-7")));
    d.setDupeWindowSecs(30);
    d.setAlsoMyCall(true);
    d.setAlsoRelay(false);
}

static void testWide11FillIn()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")},
                        QByteArray("=4903.50N/07201.75W-hello"));
    const auto dec = d.consider(in, true);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::Repeated, "WIDE1-1 is repeated");
    CHECK(dec.outgoing.has_value(), "outgoing frame");
    if (!dec.outgoing)
        return;
    CHECK(dec.outgoing->via.size() == 2, "path length preserved");
    CHECK(dec.outgoing->via.at(0).toString() == QStringLiteral("KI6BCJ-7"),
          "first hop substituted");
    CHECK(dec.outgoing->via.at(0).hasBeenRepeated, "H-bit set on us");
    CHECK(dec.outgoing->via.at(1).toString() == QStringLiteral("WIDE2-1"),
          "WIDE2 left unused");
    CHECK(!dec.outgoing->via.at(1).hasBeenRepeated, "WIDE2 H-bit clear");
    CHECK(d.stats().repeated == 1, "repeat counted");
}

static void testDoesNotAnswerWide2()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("WIDE2-1")},
                        QByteArray(">status"));
    const auto dec = d.consider(in);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::NoAliasMatch,
          "WIDE2-1 is not a fill-in hop");
    CHECK(!dec.outgoing.has_value(), "no TX");
}

static void testDoesNotAnswerWide12()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("WIDE1-2")},
                        QByteArray(">status"));
    const auto dec = d.consider(in);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::NoAliasMatch,
          "WIDE1-2 is not WIDE1-1");
}

static void testDropsOwn()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("KI6BCJ-7"), QStringLiteral("APRS"),
                        {QStringLiteral("WIDE1-1")},
                        QByteArray(">beacon"));
    const auto dec = d.consider(in);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::Own, "own source dropped");
}

static void testDropsAlreadyInPath()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                  {QStringLiteral("KI6BCJ-7"), QStringLiteral("WIDE2-1")},
                  QByteArray(">x"));
    in.via[0].hasBeenRepeated = true;
    const auto dec = d.consider(in);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::AlreadyHeard,
          "already in path");
}

static void testDuplicateWindow()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("WIDE1-1")},
                        QByteArray(">same"));
    CHECK(d.consider(in).drop == AprsFillInDigipeater::Drop::Repeated, "first copy");
    CHECK(d.consider(in).drop == AprsFillInDigipeater::Drop::Duplicate, "second copy");
}

static void testRelayOptional()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("RELAY")},
                        QByteArray(">old"));
    CHECK(d.consider(in).drop == AprsFillInDigipeater::Drop::NoAliasMatch,
          "RELAY off by default");
    d.setAlsoRelay(true);
    // Different info so it is not a dupe of the previous consider().
    const Frame in2 = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                         {QStringLiteral("RELAY")},
                         QByteArray(">old2"));
    CHECK(d.consider(in2).drop == AprsFillInDigipeater::Drop::Repeated,
          "RELAY on");
}

static void testCallsignDigi()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    const Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                        {QStringLiteral("KI6BCJ-7")},
                        QByteArray(">direct"));
    const auto dec = d.consider(in);
    CHECK(dec.drop == AprsFillInDigipeater::Drop::Repeated, "MYCALL via hop");
}

static void testTnc2()
{
    Frame in = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APZATH"),
                  {QStringLiteral("WIDE1-1"), QStringLiteral("WIDE2-1")},
                  QByteArray(">hi"));
    in.via[0].hasBeenRepeated = true;
    const QString s = AprsFillInDigipeater::tnc2(in);
    CHECK(s == QStringLiteral("N0CALL-9>APZATH,WIDE1-1*,WIDE2-1:>hi"),
          "TNC-2 path stars");
}

static void testNotUi()
{
    AprsFillInDigipeater d;
    configureDigi(d);

    Frame f;
    f.type = ax25::FrameType::SABM;
    f.src = *Address::parse(QStringLiteral("N0CALL"));
    f.dest = *Address::parse(QStringLiteral("KI6BCJ-7"));
    CHECK(d.consider(f).drop == AprsFillInDigipeater::Drop::NotUi, "SABM ignored");
}

static void testDestinationAndPathIdentity()
{
    AprsFillInDigipeater d;
    configureDigi(d);
    Frame first = ui(QStringLiteral("N0CALL-9"), QStringLiteral("APRS"),
                     {QStringLiteral("WIDE1-1")}, QByteArray(">same payload"));
    CHECK(d.consider(first).outgoing.has_value(), "first packet repeated");
    Frame other = first;
    other.dest = *Address::parse(QStringLiteral("APZATH"));
    CHECK(d.consider(other).outgoing.has_value(), "destination distinguishes packet");
    first.via.append(*Address::parse(QStringLiteral("WIDE2-1")));
    CHECK(d.consider(first).drop == AprsFillInDigipeater::Drop::Duplicate,
          "path changes do not defeat duplicate suppression");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDestinationAndPathIdentity();
    testWide11FillIn();
    testDoesNotAnswerWide2();
    testDoesNotAnswerWide12();
    testDropsOwn();
    testDropsAlreadyInPath();
    testDuplicateWindow();
    testRelayOptional();
    testCallsignDigi();
    testTnc2();
    testNotUi();
    if (g_failures) {
        std::fprintf(stderr, "aprs_fill_in_digipeater_test: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::printf("aprs_fill_in_digipeater_test: all tests passed\n");
    return 0;
}
