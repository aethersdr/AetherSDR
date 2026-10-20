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
    static auto productionSetupSink(RadioModel& radio, const RadioModel::SetupCommandWriter& writer)
    { return radio.setupCommandSink(writer); }
    static void closing(RadioModel& radio, bool value) { radio.m_txSessionClosing = value; }
    static void expiring(RadioModel& radio, bool value) { radio.m_expiringPendingCallbacks = value; }
    static void retireBackend(RadioModel& radio) { radio.teardownBackend(); }
    static PanadapterModel* addPan(RadioModel& radio, const QString& id)
    {
        PanadapterModel* pan = new PanadapterModel(id, &radio);
        radio.m_panadapters[id] = pan;
        return pan;
    }
    static SliceModel* addSlice(RadioModel& radio, int id)
    {
        SliceModel* slice = new SliceModel(id, &radio);
        // Keep retired fixtures parent-owned so stale-object guards can be tested.
        if (SliceModel* old = radio.slice(id)) { radio.m_slices.removeOne(old); }
        radio.m_slices.append(slice);
        return slice;
    }
    static void pendingDisconnectState(RadioModel& radio)
    {
        radio.m_multiFlexContinuation = [] {};
        radio.m_pendingProfileLoadPanWrites.deferCenter(QStringLiteral("0x40000000"), 14.1);
    }
    static bool disconnectStateRetired(const RadioModel& radio)
    { return !radio.m_multiFlexContinuation && radio.m_pendingProfileLoadPanWrites.isEmpty(); }
};

// Recording injection, never a radio peer or an opened transport.
class RadioSetupTestBackend final : public FlexBackend {
public:
    bool live{true};
    bool accept{true};
    QStringList commands;
    std::vector<RadioSetupCompletion> replies;
    std::shared_ptr<bool> retired;
    bool isConnected() const override { return live; }
    ~RadioSetupTestBackend() override
    {
        if (retired) { *retired = true; }
    }
    void replyLast(const RadioSetupReply& reply)
    {
        if (replies.empty()) { return; }
        const RadioSetupCompletion callback = replies.back();
        callback(reply);
    }
    void acceptCalibrationPrelude()
    {
        replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
        replyLast({RadioSetupReply::Kind::Accepted, 0, {}});
    }
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
