#include "TestSettingsProfile.h"
#include "core/backends/flex/CommandParser.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>

using namespace AetherSDR;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) {
        ++failures;
    }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radiomodel-spot-status"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    RadioModel radio;
    SpotModel& spots = radio.spotModel();
    int triggered = 0;
    int lastIndex = -1;
    QString lastPan;
    int added = 0;
    int updated = 0;
    QObject::connect(&spots, &SpotModel::spotTriggered, &radio,
        [&](int index, const QString& pan) {
            ++triggered;
            lastIndex = index;
            lastPan = pan;
        });
    QObject::connect(&spots, &SpotModel::spotAdded, &radio,
        [&](const SpotData&) { ++added; });
    QObject::connect(&spots, &SpotModel::spotUpdated, &radio,
        [&](const SpotData&) { ++updated; });

    // Parse captured Flex status syntax before entering the production router;
    // no radio, transport, or TCI listener is opened.
    const auto status = [&](const QString& body) {
        const ParsedMessage message = CommandParser::parseLine(
            QStringLiteral("S0|spot ") + body);
        radio.handleStatusForTest(message.object, message.kvs);
    };
    status(QStringLiteral("42 callsign=W1AW rx_freq=14.025000 source=DXCluster"));
    check(spots.spots().contains(42) && added == 1 && triggered == 0,
          "ordinary status adds a spot without a click");

    // Nonzero and zero handles are observed in #2366. Keep bare-status
    // compatibility as well; a zero handle must not become an empty string.
    const QStringList pans{QStringLiteral("0x40000000"),
                           QStringLiteral("0x00000000"), QString()};
    for (const QString& pan : pans) {
        const QString suffix = pan.isEmpty() ? QString() : QStringLiteral(" pan=") + pan;
        const int before = triggered;
        status(QStringLiteral("42 triggered") + suffix);
        check(triggered == before + 1 && lastIndex == 42 && lastPan == pan,
              "known spot trigger preserves its index and exact pan handle");
        check(added == 1 && updated == 0 && spots.spots().size() == 1
                  && spots.spots().value(42).callsign == QStringLiteral("W1AW"),
              "trigger does not upsert or alter the known spot");

        status(QStringLiteral("99 triggered") + suffix);
        check(triggered == before + 2 && lastIndex == 99 && lastPan == pan,
              "unknown spot trigger still routes its notification");
        check(!spots.spots().contains(99) && added == 1 && updated == 0,
              "unknown trigger never creates a blank spot");
    }

    status(QStringLiteral("42 callsign=K6OZY comment=triggered rx_freq=14.026000"));
    check(updated == 1 && triggered == 6
              && spots.spots().value(42).callsign == QStringLiteral("K6OZY"),
          "ordinary update containing triggered as field data is not a click");
    status(QStringLiteral("42 removed"));
    check(spots.spots().isEmpty() && triggered == 6,
          "removed status still removes the spot without a click");
    status(QStringLiteral("99 removed"));
    check(spots.spots().isEmpty(), "removing an unknown spot creates nothing");

    return failures == 0 ? 0 : 1;
}
