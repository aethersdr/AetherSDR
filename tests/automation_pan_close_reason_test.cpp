// Automation `pan close` names why a close was refused (#7583). With the HL2's
// real capability record and the radio keyed, `pan close all` closes every
// other pan and reports the TX pan as refused with reason "transmitting"; a
// backend's own refusal reads "backend-refused". The bridge's line dispatcher
// is called directly against an injected backend: no socket, no radio, and
// the stub's setKeying() has no body.

#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cstdio>
#include <memory>

namespace AetherSDR {
class AutomationServerTestAccess {
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};
// RadioModel's display-observation friend, reused here only to open an
// observation session so injected display reports apply.
class FlexDisplayStatusTestAccess {
public:
    static quint64 openDisplays(RadioModel& radio)
    {
        radio.openObservationSession(radio.m_observationEpoch);
        return radio.m_observationEpoch;
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
}

class NativeBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool refuseClose{false};
    int removeCalls{0};
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
    bool removePanadapter(const QString&) override { ++removeCalls; return !refuseClose; }
};

struct Bench {
    RadioModel radio;
    NativeBackend* backend{nullptr};
    AutomationServer server;
    QString firstPan;
    QString txPan;
    bool ready{false};

    Bench(bool keyed, bool refuseClose, bool removeSupported = true)
    {
        RadioModel specimen;
        if (!specimen.rebuildBackendForTest(QStringLiteral("hl2"))) {
            return;
        }
        auto owned = std::make_unique<NativeBackend>();
        backend = owned.get();
        backend->caps = specimen.backendCapabilities();
        backend->caps.maxPanadapters = 2;
        backend->refuseClose = refuseClose;
        if (!removeSupported && backend->caps.panLifecycleControl) {
            backend->caps.panLifecycleControl->actions.remove(PanLifecycleAction::Remove);
        }
        radio.setBackendForTest(std::move(owned), QStringLiteral("hl2"));
        emit backend->panCenterBandwidthChanged(QStringLiteral("native-first"), 14.1, 0.384);
        emit backend->panCenterBandwidthChanged(QStringLiteral("native-tx"), 7.1, 0.384);
        SliceDelta delta;
        delta.inUse = true;
        delta.frequency = 7.1;
        delta.mode = QStringLiteral("USB");
        delta.filterLow = 300;
        delta.filterHigh = 2700;
        delta.panId = QStringLiteral("native-tx");
        delta.txSlice = true;
        emit backend->sliceChanged(0, delta);
        firstPan = RadioModel::neutralPanIdStringForTest(0);
        txPan = RadioModel::neutralPanIdStringForTest(1);
        TransmitDelta transmit;
        transmit.mox = keyed;
        radio.applyBackendTransmitDeltaForTest(transmit);
        server.setRadioModel(&radio);
        const SliceModel* tx = radio.slice(0);
        ready = tx && tx->isTxSlice() && tx->panId() == txPan && radio.panadapters().size() == 2
            && radio.isRadioTransmitting() == keyed;
        if (!ready) {
            std::printf("  setup: tx=%d pan=%s pans=%lld keyed=%d\n", tx != nullptr,
                        tx ? qPrintable(tx->panId()) : "-",
                        static_cast<long long>(radio.panadapters().size()), radio.isRadioTransmitting());
        }
    }

    QJsonObject close(const QString& value)
    {
        const QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("pan")},
                                  {QStringLiteral("action"), QStringLiteral("close")},
                                  {QStringLiteral("value"), value}};
        return AutomationServerTestAccess::handleLine(
            server, QJsonDocument(request).toJson(QJsonDocument::Compact));
    }

    QJsonObject closeAll() { return close(QStringLiteral("all")); }
};

QJsonObject entryFor(const QJsonObject& reply, const QString& panId)
{
    for (const QJsonValue& value : reply.value(QStringLiteral("closed")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("panId")).toString() == panId) {
            return entry;
        }
    }
    return {};
}

void dump(const QJsonObject& reply)
{
    std::printf("  reply: %s\n", QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("automation-pan-close-reason"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);

    {
        Bench bench(true, false);
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        const QJsonObject other = entryFor(reply, bench.firstPan);
        check(bench.ready && reply.value(QStringLiteral("ok")).toBool()
                  && other.value(QStringLiteral("requested")).toBool()
                  && !other.contains(QStringLiteral("reason")),
              "HL2 keyed, pan close all: the other pan is requested, with no reason");
        check(bench.ready && tx.contains(QStringLiteral("requested"))
                  && !tx.value(QStringLiteral("requested")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("transmitting"),
              "HL2 keyed, pan close all: the TX pan is refused with reason \"transmitting\"");
        check(bench.ready && bench.backend->removeCalls == 1 && bench.radio.panadapter(bench.txPan),
              "and the TX pan's close never reaches the backend; the pan stays");
        if (!bench.ready || tx.value(QStringLiteral("reason")).toString() != QStringLiteral("transmitting")) {
            dump(reply);
        }
    }
    {
        Bench bench(true, false);
        const QJsonObject reply = bench.close(bench.txPan);
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && !reply.value(QStringLiteral("ok")).toBool()
                  && !tx.value(QStringLiteral("requested")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("transmitting")
                  && bench.backend->removeCalls == 0 && bench.radio.panadapter(bench.txPan),
              "HL2 keyed, targeted close of the TX pan (Ordinary): refused with reason \"transmitting\"");
        if (tx.value(QStringLiteral("reason")).toString() != QStringLiteral("transmitting")) {
            dump(reply);
        }
    }
    {
        Bench bench(false, false);
        if (PanadapterModel* pan = bench.radio.panadapter(bench.txPan)) {
            pan->setClientHandle(QStringLiteral("7e57c0de"));   // another client's pan
        }
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && !tx.value(QStringLiteral("requested")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("not-owned")
                  && entryFor(reply, bench.firstPan).value(QStringLiteral("requested")).toBool()
                  && bench.backend->removeCalls == 1 && bench.radio.panadapter(bench.txPan),
              "another client's pan: refused with reason \"not-owned\", never sent; ours still closes");
        if (tx.value(QStringLiteral("reason")).toString() != QStringLiteral("not-owned")) {
            dump(reply);
        }
    }
    {
        Bench bench(false, false, false);
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && !reply.value(QStringLiteral("ok")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("remove-not-supported")
                  && entryFor(reply, bench.firstPan).value(QStringLiteral("reason")).toString()
                         == QStringLiteral("remove-not-supported")
                  && bench.backend->removeCalls == 0,
              "a backend without Remove: every entry reads reason \"remove-not-supported\", nothing sent");
        if (tx.value(QStringLiteral("reason")).toString() != QStringLiteral("remove-not-supported")) {
            dump(reply);
        }
    }
    {
        // Over the display-inventory bound (256 waterfalls) the model holds
        // every pan control until reconnect.
        Bench bench(false, false);
        const quint64 epoch = FlexDisplayStatusTestAccess::openDisplays(bench.radio);
        for (int i = 0; i <= 256 && bench.backend; ++i) {
            DisplayObservation observation;
            observation.kind = DisplayObservation::Kind::Waterfall;
            observation.id = QStringLiteral("0x%1").arg(0x42100000 + i, 8, 16, QLatin1Char('0'));
            observation.statusEpoch = epoch;
            emit bench.backend->displayObserved(observation);
        }
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && !reply.value(QStringLiteral("ok")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("inventory-uncertain")
                  && entryFor(reply, bench.firstPan).value(QStringLiteral("reason")).toString()
                         == QStringLiteral("inventory-uncertain")
                  && bench.backend->removeCalls == 0,
              "display inventory overflowed: every entry reads reason \"inventory-uncertain\", nothing sent");
        if (tx.value(QStringLiteral("reason")).toString() != QStringLiteral("inventory-uncertain")) {
            dump(reply);
        }
    }
    {
        Bench bench(false, true);
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && !reply.value(QStringLiteral("ok")).toBool()
                  && !tx.value(QStringLiteral("requested")).toBool()
                  && tx.value(QStringLiteral("reason")).toString() == QStringLiteral("backend-refused")
                  && entryFor(reply, bench.firstPan).value(QStringLiteral("reason")).toString()
                         == QStringLiteral("backend-refused"),
              "unkeyed, the backend refuses: every entry reads reason \"backend-refused\"");
        if (tx.value(QStringLiteral("reason")).toString() != QStringLiteral("backend-refused")) {
            dump(reply);
        }
    }
    {
        Bench bench(false, false);
        const QJsonObject reply = bench.closeAll();
        const QJsonObject tx = entryFor(reply, bench.txPan);
        check(bench.ready && reply.value(QStringLiteral("ok")).toBool()
                  && tx.value(QStringLiteral("requested")).toBool() && !tx.contains(QStringLiteral("reason"))
                  && bench.backend->removeCalls == 2,
              "control: unkeyed, both closes are requested and no entry carries a reason");
    }

    std::printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
