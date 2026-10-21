#include "models/Kxpa100Model.h"
#include "core/Kxpa100Maintenance.h"
#include <QCoreApplication>
#include <QPair>
#include <QTcpSocket>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
#include <QSignalSpy>
#include <cstdio>
using namespace AetherSDR::Kxpa100;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
namespace AetherSDR {
class Kxpa100ModelTest {
public:
    struct Harness {
        Kxpa100Model model;
        qint64 now = 100;
        QList<QByteArray> wire;
        Harness() {
            model.m_now = [this] { return now; };
            model.m_write = [this](const QByteArray& bytes) { wire.append(bytes); return true; };
            model.m_connected = model.m_detected = true;
            model.m_lastReply = now; model.m_nextPoll = 100000;
            model.m_status.identity = "KXPA100"; model.m_status.serial = "00031"; model.m_status.firmware = "01.39";
            model.m_status.tuning = false; model.m_status.inputWatts = 0; model.m_status.forwardWatts = 0;
            model.m_status.tunerPresent = true; model.m_status.mode = Mode::Auto; model.m_status.antenna = 1;
            model.m_status.operate = true; model.m_status.faultCode = 'N';
            for (const QString& key : {"^I","^RV","^SN","^TP","^PI","^PF","^TU","^MD","^AN","^OP","^FL"}) { model.m_received[key] = now; }
        }
        void reply(const QByteArray& bytes) { model.tick(); model.receiveBytes(bytes); }
        void preflight() {
            reply("^IKXPA100;"); reply("^RV01.39;"); reply("^SN00031;");
            reply("^TP0;"); reply("^PI0000;"); reply("^PF0000;");
        }
    };
    static int maintenance() {
        // Bytes are injected at the model boundary; this test opens no socket or serial port.
        for (int failure : {0,1,2}) {
            Harness h;
            QByteArray internal(1024, char(0x51)), external(16384, char(0x72));
            const QByteArray originalInternal = internal, originalExternal = external;
            Backup backup{QByteArray(1024,char(0x19)),QByteArray(16384,char(0x28)),"01.39","00031"};
            int applied = 0, consumed = 0;
            QSignalSpy done(&h.model,&Kxpa100Model::operationFinished), failed(&h.model,&Kxpa100Model::operationFailed);
            CHECK(h.model.restoreBackup(backup,true));
            for (int step = 0; step < 5000 && h.model.operationBusy(); ++step) {
                h.model.tick(); h.now += 50;
                if (consumed == h.wire.size()) { continue; }
                const QByteArray wire = h.wire[consumed++];
                if (wire.startsWith("^ER") || wire.startsWith("^EW")) {
                    const bool ext = wire[3] == 'S', write = wire[2] == 'W';
                    const QByteArray bytes = QByteArray::fromHex(wire.mid(4).chopped(1));
                    const int address = quint8(bytes[0])*256 + quint8(bytes[1]), count = quint8(bytes[2]);
                    QByteArray& memory = ext ? external : internal;
                    if (write) {
                        CHECK(address >= (ext ? 0 : 32) && address + count <= (ext ? 0x3800 : 1024));
                        CHECK(address/256 == (address+count-1)/256);
                        memory.replace(address,count,bytes.mid(3,count)); ++applied;
                        if (failure == 1 && applied == 2) { h.now += 4000; continue; }
                    }
                    QByteArray reply = bytes.left(3);
                    if (!write) { reply += (failure == 2 && applied == 2) ? QByteArray(count,'!') : memory.mid(address,count); }
                    int sum = 0; for (char b : reply) { sum += quint8(b); } reply.append(char(-sum));
                    h.model.receiveBytes(wire.left(4) + reply.toHex().toUpper() + ';');
                } else {
                    const QMap<QByteArray,QByteArray> replies{{"^I;","^IKXPA100;"},{"^RV;","^RV01.39;"},{"^SN;","^SN00031;"},
                        {"^TP;","^TP0;"},{"^PI;","^PI0000;"},{"^PF;","^PF0000;"}};
                    CHECK(replies.contains(wire)); h.model.receiveBytes(replies.value(wire));
                }
            }
            CHECK(!h.model.operationBusy());
            CHECK(internal.left(32) == originalInternal.left(32) && external.mid(0x3800) == originalExternal.mid(0x3800));
            if (failure == 0) {
                CHECK(done.size() == 1 && failed.isEmpty() && h.wire.last() == "^RS;");
                CHECK(internal.mid(32) == backup.internal.mid(32) && external.left(0x3800) == backup.external.left(0x3800));
            } else {
                CHECK(done.isEmpty() && failed.size() == 1 && applied == 2 && !h.wire.contains("^RS;"));
            }
        }
        return 0;
    }
    static int transportBoundaries()
    {
        Kxpa100Model model;
        CHECK(model.m_socket->readBufferSize() > 0 && model.m_socket->readBufferSize() <= 4096);
        QSignalSpy failed(&model, &Kxpa100Model::connectionFailed);
        const QList<QString> invalidHosts{QString(), QString(254, QLatin1Char('a')),
            QStringLiteral("bridge/path"), QStringLiteral("bridge host"),
            QStringLiteral("bridge\nhost"), QStringLiteral("bridge") + QChar(0)};
        for (const QString& host : invalidHosts) {
            model.connectToHost(host, 4001);
            CHECK(!model.isConnected() && !model.m_connecting && !model.m_timer.isActive());
            CHECK(model.m_socket->state() == QAbstractSocket::UnconnectedState);
        }
        model.connectToHost(QStringLiteral("bridge"), 0);
        CHECK(failed.size() == invalidHosts.size() + 1 && !model.m_connecting);
#ifdef HAVE_SERIALPORT
        CHECK(model.m_serial && !model.m_serial->isOpen());
        model.connectSerial(QString(), 38400);
        model.connectSerial(QStringLiteral("unused-port"), 12345);
        CHECK(!model.m_serial->isOpen() && !model.isConnected());
#endif
        return 0;
    }
    static int swrPolling()
    {
        const QList<QPair<QByteArray, double>> replies{{"^SW01.3;", 1.3}, {"^SW00.0;", 0.0},
            {"^SW99.9;", 99.9}, {"^SW013;", 1.3}};
        for (const auto& [reply, expected] : replies) {
            Harness h;
            QSignalSpy failed(&h.model, &Kxpa100Model::connectionFailed);
            QSignalSpy disconnected(&h.model, &Kxpa100Model::disconnected);
            QSignalSpy updated(&h.model, &Kxpa100Model::statusUpdated);
            h.model.m_nextPoll = h.now;
            h.model.tick();
            CHECK(h.wire == QList<QByteArray>{"^SW;"} && h.model.m_active);
            const int timeout = h.model.m_active->timeoutMs;
            h.model.receiveBytes("^SW1.3;");
            CHECK(h.model.m_active && !h.model.status().swr && updated.isEmpty());
            h.model.receiveBytes(reply.left(5));
            CHECK(h.model.m_active && updated.isEmpty());
            h.model.receiveBytes(reply.mid(5));
            CHECK(!h.model.m_active && h.model.status().swr == expected);
            CHECK(updated.size() == 1 && h.model.fieldAgeMs(QStringLiteral("^SW")) == 0);
            h.now += timeout + 1;
            h.model.tick();
            CHECK(h.model.isConnected() && h.model.isDetected() && h.model.isResponding());
            CHECK(failed.isEmpty() && disconnected.isEmpty());
            CHECK(h.wire.size() == 2 && h.wire.last() == "^TP;" && h.model.m_active);
            h.model.receiveBytes("^TP0;");
            CHECK(!h.model.m_active && h.model.status().swr == expected);
        }
        return 0;
    }
    static int run() {
        CHECK(transportBoundaries() == 0);
        CHECK(swrPolling() == 0);
        {
            Harness h; h.model.transportOpened();
            CHECK(!h.model.isDetected() && h.wire.isEmpty());
            h.model.tick(); h.model.receiveBytes("^IKXP"); CHECK(!h.model.isDetected());
            h.model.receiveBytes("A100;"); h.reply("^RV01.39;"); CHECK(!h.model.isDetected());
            h.reply("^SN00031;"); CHECK(h.model.isDetected());
            h.model.disconnectDevice(); CHECK(!h.model.isDetected());
            CHECK(!h.wire.contains("^FT;^TP;") && !h.wire.contains("^OP1;^OP;"));
        }
        {
            Harness h; QSignalSpy done(&h.model,&Kxpa100Model::operationFinished);
            CHECK(h.model.setAntenna(2)); CHECK(h.model.status().antenna == 1);
            h.preflight(); h.reply("^AN1;"); CHECK(h.wire.last() == "^AN2;^AN;" && h.model.operationBusy());
            h.now += 100; h.reply("^AN2;");
            CHECK(!h.model.operationBusy() && done.size() == 1 && h.model.status().antenna == 2);
            CHECK(h.wire.count("^AN2;^AN;") == 1);
        }
        {
            Harness h; CHECK(h.model.setAntenna(2)); h.preflight(); h.model.tick();
            h.now += 2500; h.model.receiveBytes("^AN2;");
            CHECK(!h.model.operationBusy() && !h.model.isConnected());
            CHECK(h.wire.count("^AN2;^AN;") == 1);
        }
        {
            Harness h; CHECK(h.model.setAntenna(2)); h.reply("^IKXPA100;"); h.reply("^RV01.39;"); h.reply("^SN00032;");
            CHECK(!h.model.isConnected() && !h.model.operationBusy());
            CHECK(!h.wire.contains("^AN2;^AN;"));
        }
        {
            Harness h; h.model.m_status.tunerPresent = false;
            CHECK(!h.model.setAntenna(2) && !h.model.startTune() && h.wire.isEmpty());
            CHECK(h.model.setOperate(false)); // Optional tuner does not disable the amplifier.
        }
        {
            Harness h; CHECK(h.model.setAntenna(2));
            h.reply("^IKXPA100;"); h.reply("^RV01.39;"); h.reply("^SN00031;");
            h.reply("^TP0;"); h.reply("^PI0010;"); h.reply("^PF0100;");
            CHECK(!h.model.operationBusy() && !h.wire.contains("^AN2;^AN;"));
        }
        {
            Harness h; bool permit = true; CHECK(h.model.startTune([&] { return permit; }));
            h.preflight(); permit = false; h.model.tick();
            CHECK(!h.model.operationBusy() && !h.wire.contains("^FT;^TP;"));
        }
        {
            Harness h; CHECK(h.model.startTune()); h.preflight(); h.reply("^TP1;");
            CHECK(!h.model.operationBusy() && h.model.status().tuning == true);
            CHECK(h.model.cancelTune()); h.reply("^FE;^TP0;");
            CHECK(!h.model.operationBusy() && h.model.status().tuning == false);
        }
        {
            Harness h; h.now += 6000;
            CHECK(!h.model.setAntenna(2) && !h.model.tunerEligible() && !h.model.amplifierMeterEligible());
            h.model.m_lastReply = h.now; // Unrelated replies cannot freshen critical fields.
            CHECK(!h.model.tunerEligible() && !h.model.amplifierMeterEligible());
        }
        {
            Harness h; CHECK(!h.model.restoreBackup({}, true)); CHECK(!h.model.factoryReset(false));
            CHECK(!h.model.eraseMemories(15,0,false) && h.wire.isEmpty());
            CHECK(h.model.setMode(Mode::Manual)); h.model.cancelOperation();
            CHECK(!h.model.operationBusy()); h.model.tick(); CHECK(h.wire.isEmpty());
        }
        {
            Harness h; CHECK(h.model.readConfiguration()); h.reply("^AEA33333333333;");
            CHECK(h.model.configuration() && (*h.model.configuration())[0] == 3);
            Configuration invalid = *h.model.configuration(); invalid[0] = 0;
            CHECK(!h.model.applyConfiguration(invalid));
        }
        {
            Harness h; CHECK(h.model.readConfiguration()); h.reply("^AEA33333333333;");
            Configuration config = *h.model.configuration(); config[0] = 1;
            CHECK(h.model.applyConfiguration(config)); h.preflight();
            QSignalSpy progress(&h.model,&Kxpa100Model::operationProgress);
            QObject::connect(&h.model,&Kxpa100Model::configurationRead,&h.model,[&] { h.model.disconnectDevice(); });
            h.reply("^AEA13333333333;"); CHECK(!h.model.isConnected() && !h.model.operationBusy());
            CHECK(progress.isEmpty());
        }
        CHECK(maintenance() == 0);
        return 0;
    }
};
}
int main(int argc, char** argv) { QCoreApplication app(argc, argv); return AetherSDR::Kxpa100ModelTest::run(); }
