#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"

#include <QStringList>
#include <memory>
#include <vector>

namespace AetherSDR {
class RadioSetupControlTestAccess {
public:
    static void observe(RadioModel& radio, const RadioDelta& observation)
    { radio.applyRadioChanges(observation); }
    static void band(RadioModel& radio, int id)
    {
        radio.onStatusReceived(QStringLiteral("transmit band %1").arg(id),
            {{QStringLiteral("band_name"), QStringLiteral("20m")},
             {QStringLiteral("rfpower"), QStringLiteral("37")}});
    }
    static void transverter(RadioModel& radio, int id)
    {
        radio.onStatusReceived(QStringLiteral("xvtr %1").arg(id),
            {{QStringLiteral("name"), QStringLiteral("2m")},
             {QStringLiteral("rf_freq"), QStringLiteral("144")},
             {QStringLiteral("if_freq"), QStringLiteral("28")}});
    }
    static void disconnected(RadioModel& radio) { radio.onDisconnected(); }
    static void replaceConnectionGeneration(RadioModel& radio) { ++radio.m_sessionGeneration; }
};

// Recording injection, never a radio peer or an opened transport.
class RadioSetupTestBackend final : public FlexBackend {
public:
    bool live{true};
    bool accept{true};
    QStringList commands;
    std::vector<RadioSetupCompletion> replies;
    bool isConnected() const override { return live; }
    RadioSetupTestBackend()
    {
        setModelProvider([] { return QStringLiteral("FLEX-6700"); });
        setSetupCommandSink([this](const QString& command, const RadioSetupCompletion& completion) {
            if (!live || !accept) { return false; }
            commands.append(command);
            if (completion) { replies.push_back(completion); }
            return true;
        });
    }
};

inline RadioSetupTestBackend* installRadioSetupBackend(RadioModel& radio)
{
    auto owned = std::make_unique<RadioSetupTestBackend>();
    RadioSetupTestBackend* backend = owned.get();
    radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    QObject::connect(backend, &IRadioBackend::radioChanged, &radio,
        [&radio](const RadioDelta& delta) { RadioSetupControlTestAccess::observe(radio, delta); });
    return backend;
}

} // namespace AetherSDR
