// Automation `pan close` names why a close was refused (#7583). With the HL2's
// real capability record and the radio keyed, `pan close all` closes every
// other pan and reports the TX pan as refused with reason "transmitting"; a
// backend's own refusal reads "backend-refused". The bridge's line dispatcher
// is called directly against an injected backend: no socket, no radio, and
// the stub's setKeying() has no body.

#include "TestSettingsProfile.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
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

    Bench(bool keyed, bool refuseClose)
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

    QJsonObject closeAll()
    {
        const QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("pan")},
                                  {QStringLiteral("action"), QStringLiteral("close")},
                                  {QStringLiteral("value"), QStringLiteral("all")}};
        return AutomationServerTestAccess::handleLine(
            server, QJsonDocument(request).toJson(QJsonDocument::Compact));
    }
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
