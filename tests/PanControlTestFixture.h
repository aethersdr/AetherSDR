#pragma once

#include "core/backends/flex/FlexBackend.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/RadioModel.h"

#include <QDateTime>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {

class TxOperationIntegrationTestAccess {
public:
    static void identity(RadioModel& radio, RadioConnection& connection, FlexBackend* backend)
    {
        connection.m_handle = 0x71;
        radio.m_connection = &connection;
        radio.m_flexBackend = backend;
    }
    static void release(RadioModel& radio) { radio.m_connection = nullptr; }
    static void status(RadioModel& radio, const QString& object, const QMap<QString, QString>& values)
    { radio.onStatusReceived(object, values); }
    static int pending(const RadioModel& radio) { return radio.m_pendingPanCreations.size(); }
    static int history(const RadioModel& radio) { return radio.m_displayRetirements.size(); }
    static void retire(RadioModel& radio, const QString& id) { radio.retireDisplayResource(id); }
    static void settle(RadioModel& radio) { radio.settlePanCreations(); }
    static void newSession(RadioModel& radio) { ++radio.m_sessionGeneration; radio.settlePanCreations(); }
    static void hold(RadioModel& radio) { radio.m_profileLoadRadioStateWriteHoldUntilMs = QDateTime::currentMSecsSinceEpoch() + 60000; }
    static void flush(RadioModel& radio)
    { radio.m_profileLoadRadioStateWriteHoldUntilMs = 0; radio.flushPendingProfileLoadPanWrites(); }
};

// Records production encoder requests without binding or connecting a transport.
class RecordingPanBackend final : public FlexBackend {
public:
    struct Write { QString command; RadioControlCompletion completion; };
    std::vector<Write> writes;
    std::function<void(const QString&)> onWrite;
    bool admit{true};
    bool isConnected() const override { return true; }
    RecordingPanBackend()
    {
        setModelProvider([] { return QStringLiteral("FLEX-6700"); });
        setSetupCommandSink([this](const QString& command, const RadioControlCompletion& completion) {
            if (!admit) { return false; }
            writes.push_back({command, completion});
            const auto callback = onWrite;
            if (callback) { callback(command); }
            return true;
        });
    }
    QStringList commands() const
    {
        QStringList result;
        for (const Write& write : writes) { result.append(write.command); }
        return result;
    }
    void answer(size_t index, const RadioControlReply& reply = {RadioControlReply::Kind::Accepted, 0, {}})
    {
        const RadioControlCompletion callback = writes.at(index).completion;
        if (callback) { callback(reply); }
    }
};

struct PanControlFixture {
    RadioConnection identity; // Unopened, used only for the client-handle value.
    RadioModel radio;
    RecordingPanBackend* backend{nullptr};
    PanControlFixture()
    {
        auto owned = std::make_unique<RecordingPanBackend>();
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("flex"), backend->panStream());
        TxOperationIntegrationTestAccess::identity(radio, identity, backend);
        // Injection installs universal receivers. Forward the existing Flex
        // display extension as setupBackend does for a production session.
        QObject::connect(backend, &IRadioBackend::extensionStatus, &radio,
            [this](const QString& ns, const QString& kind, const QVariantMap& fields) {
                PanadapterModel* pan = radio.panadapter(fields.value(QStringLiteral("panId")).toString());
                if (pan && ns == QLatin1String("flex") && kind == QLatin1String("panState")) {
                    pan->applyStateExtension(fields);
                }
            });
    }
    ~PanControlFixture() { TxOperationIntegrationTestAccess::release(radio); }
    void status(const QString& object, const QMap<QString, QString>& values)
    { TxOperationIntegrationTestAccess::status(radio, object, values); }
    PanadapterModel* publish(const QString& id, const QString& waterfall = {})
    {
        QMap<QString, QString> values{{QStringLiteral("client_handle"), QStringLiteral("0x71")},
            {QStringLiteral("center"), QStringLiteral("14.1")}, {QStringLiteral("bandwidth"), QStringLiteral("0.2")}};
        if (!waterfall.isEmpty()) { values.insert(QStringLiteral("waterfall"), waterfall); }
        status(QStringLiteral("display pan %1").arg(id), values);
        return radio.panadapter(id);
    }
    void remove(const QString& id) { status(QStringLiteral("display pan %1 removed").arg(id), {}); }
    void clear() { backend->writes.clear(); backend->onWrite = {}; }
};

} // namespace AetherSDR
