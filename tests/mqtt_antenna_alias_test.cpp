#include "core/MqttAntennaAlias.h"

#include <iostream>

using namespace AetherSDR;

namespace {

bool expect(bool condition, const char* label)
{
    std::cout << (condition ? "[ OK ] " : "[FAIL] ") << label << '\n';
    return condition;
}

QString aliasFor(const QVector<MqttAntennaAliasUpdate>& updates, const QString& token)
{
    for (const auto& update : updates) {
        if (update.token == token)
            return update.alias;
    }
    return QStringLiteral("__missing__");
}

} // namespace

int main()
{
    bool ok = true;

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/name/ant1"),
            QByteArray("80m Dipole"));
        ok &= expect(updates.size() == 1, "per-token topic produces one update");
        ok &= expect(updates.value(0).token == QStringLiteral("ANT1"),
                     "per-token topic normalizes canonical token");
        ok &= expect(updates.value(0).alias == QStringLiteral("80m Dipole"),
                     "per-token payload becomes alias");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/name/RX_A"),
            QByteArray());
        ok &= expect(updates.size() == 1, "empty per-token payload produces update");
        ok &= expect(updates.value(0).token == QStringLiteral("RX_A"),
                     "empty per-token payload keeps token");
        ok &= expect(updates.value(0).alias.isEmpty(),
                     "empty per-token payload clears alias");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/names"),
            QByteArray("{\"ANT1\":\"80m Dipole\",\"ant2\":\"Hexbeam\",\"RX_A\":null,\"RX_B\":\"\"}"));
        ok &= expect(updates.size() == 4, "bulk JSON object produces string/null updates");
        ok &= expect(aliasFor(updates, QStringLiteral("ANT1")) == QStringLiteral("80m Dipole"),
                     "bulk JSON parses ANT1 alias");
        ok &= expect(aliasFor(updates, QStringLiteral("ANT2")) == QStringLiteral("Hexbeam"),
                     "bulk JSON normalizes ANT2 alias");
        ok &= expect(aliasFor(updates, QStringLiteral("RX_A")).isEmpty(),
                     "bulk JSON null clears alias");
        ok &= expect(aliasFor(updates, QStringLiteral("RX_B")).isEmpty(),
                     "bulk JSON empty string clears alias");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/name/ANT1/extra"),
            QByteArray("bad"));
        ok &= expect(updates.isEmpty(), "nested per-token topic is ignored");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/names"),
            QByteArray("{\"ANT1/extra\":\"bad\",\"ANT 2\":\"bad\",\"ANT3\":\"good\"}"));
        ok &= expect(updates.size() == 1, "bulk JSON rejects malformed token keys");
        ok &= expect(updates.value(0).token == QStringLiteral("ANT3")
                         && updates.value(0).alias == QStringLiteral("good"),
                     "bulk JSON keeps valid token after malformed keys");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("aethersdr/antenna/names"),
            QByteArray("{bad json"));
        ok &= expect(updates.isEmpty(), "malformed bulk JSON is ignored");
    }

    {
        const auto updates = parseMqttAntennaAliasMessage(
            QStringLiteral("station/ant/name/ANT1"),
            QByteArray("Dipole"));
        ok &= expect(updates.isEmpty(), "unrelated topic is ignored");
    }

    {
        MqttAntennaAliasQueue queue;
        auto ready = queue.receive({QStringLiteral("ANT1"), QStringLiteral("80m Dipole")},
                                   true, false);
        ok &= expect(ready.isEmpty(), "unstable radio identity queues alias");
        ok &= expect(queue.pendingCount() == 1, "queued alias is retained");

        ready = queue.receive({QStringLiteral("ANT1"), QStringLiteral("Hexbeam")},
                              true, false);
        ok &= expect(ready.isEmpty(), "unstable duplicate alias stays queued");
        ok &= expect(queue.pendingCount() == 1, "queued alias coalesces by token");

        ready = queue.flush(false);
        ok &= expect(ready.isEmpty(), "flush before stable identity does nothing");
        ok &= expect(queue.pendingCount() == 1, "flush before stable identity keeps queue");

        ready = queue.flush(true);
        ok &= expect(ready.size() == 1, "stable identity flushes queued alias");
        ok &= expect(ready.value(0).token == QStringLiteral("ANT1")
                         && ready.value(0).alias == QStringLiteral("Hexbeam"),
                     "stable identity flush uses latest queued alias");
        ok &= expect(queue.pendingCount() == 0, "stable identity flush clears queue");

        ready = queue.receive({QStringLiteral("ANT2"), QStringLiteral("Vertical")},
                              true, true);
        ok &= expect(ready.size() == 1
                         && ready.value(0).token == QStringLiteral("ANT2")
                         && ready.value(0).alias == QStringLiteral("Vertical"),
                     "stable radio identity applies alias immediately");

        queue.receive({QStringLiteral("RX_A"), QStringLiteral("Beverage")}, true, false);
        queue.clear();
        ready = queue.flush(true);
        ok &= expect(ready.isEmpty(), "disconnect clear prevents cross-radio replay");

        ready = queue.receive({QStringLiteral("ANT1"), QStringLiteral("Disconnected")},
                              false, false);
        ok &= expect(ready.isEmpty(), "disconnected radio ignores alias update");
        ok &= expect(queue.pendingCount() == 0,
                     "disconnected alias update is not queued for next radio");
        ready = queue.flush(true);
        ok &= expect(ready.isEmpty(), "disconnected alias cannot replay to next radio");
    }

    return ok ? 0 : 1;
}
