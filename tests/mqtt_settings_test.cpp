#include "core/MqttSettings.h"

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

} // namespace

int main()
{
    bool ok = true;

    {
        const QVector<MqttTopicDef> topics = parseMqttTopicConfig(
            QStringLiteral(" *rotator/pos, station/log, *ant/selected, , plain "));
        ok &= expect(topics.size() == 4, "legacy topic list skips empty entries");
        ok &= expect(topics.value(0).topic == QStringLiteral("rotator/pos")
                         && topics.value(0).displayOnPan,
                     "star-prefixed topic marks display-on-panadapter");
        ok &= expect(topics.value(1).topic == QStringLiteral("station/log")
                         && !topics.value(1).displayOnPan,
                     "plain topic is user subscription only");
        ok &= expect(topics.value(2).topic == QStringLiteral("ant/selected")
                         && topics.value(2).displayOnPan,
                     "later star-prefixed topic keeps display flag");
        ok &= expect(serializeMqttTopicConfig(topics)
                         == QStringLiteral("*rotator/pos, station/log, *ant/selected, plain"),
                     "topic serialization preserves legacy star convention");
    }

    {
        const QStringList internal = internalMqttSubscriptionTopics();
        ok &= expect(internal.contains(QStringLiteral("aethersdr/antenna/name/+")),
                     "internal subscriptions include per-antenna alias wildcard");
        ok &= expect(internal.contains(QStringLiteral("aethersdr/antenna/names")),
                     "internal subscriptions include bulk antenna alias topic");

        const QStringList subscriptions = mqttSubscriptionTopics({
            QStringLiteral("station/log"),
            QStringLiteral("aethersdr/antenna/name/+"),
            QStringLiteral("station/log"),
        });
        ok &= expect(subscriptions.size() == 3,
                     "subscription list deduplicates user and internal topics");
        ok &= expect(subscriptions.value(0) == QStringLiteral("station/log"),
                     "subscription list keeps user topic order");
        ok &= expect(subscriptions.contains(QStringLiteral("aethersdr/antenna/names")),
                     "subscription list appends missing internal topics");
    }

    {
        const QVector<MqttButtonDef> buttons{
            {QStringLiteral("CW"), QStringLiteral("rotator/cmd"), QStringLiteral("CW")},
            {QStringLiteral("Stop"), QStringLiteral("rotator/cmd"), QStringLiteral("STOP")},
        };
        const QVector<MqttButtonDef> roundTrip = mqttButtonsFromJson(mqttButtonsToJson(buttons));
        ok &= expect(roundTrip.size() == 2, "publish button JSON round-trips count");
        ok &= expect(roundTrip.value(0).label == QStringLiteral("CW")
                         && roundTrip.value(0).topic == QStringLiteral("rotator/cmd")
                         && roundTrip.value(0).payload == QStringLiteral("CW"),
                     "publish button JSON preserves first button");
        ok &= expect(roundTrip.value(1).label == QStringLiteral("Stop")
                         && roundTrip.value(1).topic == QStringLiteral("rotator/cmd")
                         && roundTrip.value(1).payload == QStringLiteral("STOP"),
                     "publish button JSON preserves second button");
    }

    return ok ? 0 : 1;
}
