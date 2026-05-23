#include "models/PanadapterModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QString>
#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    auto a_ = (actual); auto e_ = (expected); \
    if (a_ != e_) { \
        const QString a_str = QString("%1").arg(a_); \
        const QString e_str = QString("%1").arg(e_); \
        std::fprintf(stderr, "FAIL %s:%d  expected %s, got %s\n", \
                     __FILE__, __LINE__, \
                     e_str.toUtf8().constData(), \
                     a_str.toUtf8().constData()); \
        ++g_failures; \
    } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    PanadapterModel pan(QStringLiteral("0x40000000"));
    QSignalSpy spy(&pan, &PanadapterModel::rxAntennaChanged);

    pan.applyPanStatus({{QStringLiteral("rxant"), QStringLiteral("ANT1")}});
    EXPECT_EQ(pan.rxAntenna(), QStringLiteral("ANT1"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("ANT1"));

    pan.applyPanStatus({{QStringLiteral("rxant"), QStringLiteral("ANT1")}});
    EXPECT_EQ(spy.count(), 0);

    pan.applyPanStatus({{QStringLiteral("rxant"), QStringLiteral("RX_A")}});
    EXPECT_EQ(pan.rxAntenna(), QStringLiteral("RX_A"));
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), QStringLiteral("RX_A"));

    QSignalSpy gainSpy(&pan, &PanadapterModel::rfGainChanged);
    pan.applyPanStatus({{QStringLiteral("rfgain"), QStringLiteral("20")}});
    EXPECT_EQ(pan.rfGain(), 20);
    EXPECT_EQ(gainSpy.count(), 1);
    EXPECT_EQ(gainSpy.takeFirst().at(0).toInt(), 20);

    pan.applyPanStatus({{QStringLiteral("rfgain"), QStringLiteral("20")}});
    EXPECT_EQ(gainSpy.count(), 0);

    pan.applyPanStatus({{QStringLiteral("rfgain"), QStringLiteral("-8")}});
    EXPECT_EQ(pan.rfGain(), -8);
    EXPECT_EQ(gainSpy.count(), 1);
    EXPECT_EQ(gainSpy.takeFirst().at(0).toInt(), -8);

    if (g_failures == 0) {
        std::printf("panadapter_model_rx_antenna_test: all checks passed\n");
        return 0;
    }
    std::printf("panadapter_model_rx_antenna_test: %d failure(s)\n", g_failures);
    return 1;
}
