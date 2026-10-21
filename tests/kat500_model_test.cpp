#include <cstdio>
#include "models/Kat500Model.h"
#include "core/Kat500Protocol.h"
#include "core/Kat500Maintenance.h"
#include <QCoreApplication>
#include <QTcpSocket>
#ifdef HAVE_SERIALPORT
#include <QSerialPort>
#endif
#include <QSignalSpy>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <utility>
using namespace AetherSDR::Kat500;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (false)
namespace AetherSDR {
class Kat500ModelTest {
public:
static int keyBlockReadback() {
    Kat500Model model;
    qint64 now = 100;
    model.m_timer.stop();
    model.m_now = [&] { return now; };
    model.m_write = [](const QByteArray&) { return true; };
    model.m_connected = true;
    model.m_detected = true;
    model.m_lastReply = model.m_lastIo = now;
    model.m_nextPoll = 60000;
    model.m_status.identity = "KAT500";
    model.m_status.powerOn = true;
    model.m_status.tuning = false;
    model.m_status.forwardAdc = 0;
    model.m_received["TP"] = model.m_received["VFWD"] = now;
    CHECK(!model.ampKeyBlockConfirmed());
    CHECK(model.setAmpKeyInterrupted(true));
    model.tick();
    CHECK(!model.ampKeyBlockConfirmed()); // Sending a request is not confirmation.
    model.receiveBytes("AMPI1;");
    CHECK(model.ampKeyBlockConfirmed());
    now += 5001;
    model.m_lastReply = model.m_lastIo = now; // Other replies cannot freshen AMPI.
    CHECK(!model.ampKeyBlockConfirmed());
    model.query("AMPI;"); model.tick(); model.receiveBytes("AMPI1;");
    CHECK(model.ampKeyBlockConfirmed());
    model.query("AMPI;"); model.tick(); model.receiveBytes("AMPI0;");
    CHECK(!model.ampKeyBlockConfirmed());
    model.query("AMPI;"); model.tick(); model.receiveBytes("AMPI1;");
    CHECK(model.ampKeyBlockConfirmed());
    model.m_status.powerOn = false;
    CHECK(!model.ampKeyBlockConfirmed());
    model.m_status.powerOn = true;
    model.transportClosed();
    CHECK(!model.ampKeyBlockConfirmed());
    return 0;
}
static int settlingReadback() {
 Kat500Model model; qint64 now=0; QList<QByteArray> wire;
 model.m_now=[&]{return now;}; model.m_write=[&](const QByteArray& bytes){wire.append(bytes);return true;};
 model.m_connected=true; model.m_detected=true; model.m_lastReply=0; model.m_lastIo=0;
 model.m_status.identity="KAT500"; model.m_status.powerOn=true; model.m_status.tuning=false; model.m_status.forwardAdc=0;
 model.m_received["TP"]=0; model.m_received["VFWD"]=0;
 QSignalSpy finished(&model,&Kat500Model::operationFinished),failed(&model,&Kat500Model::operationFailed);
 CHECK(model.setAntenna(2)); model.tick(); CHECK(wire.last()=="AN2;AN;");
 model.receiveBytes("AN1;"); CHECK(model.operationBusy() && failed.isEmpty() && finished.isEmpty());
 now+=100; model.tick(); CHECK(wire.last()=="AN;"); model.receiveBytes("AN2;");
 CHECK(!model.operationBusy() && finished.size()==1 && failed.isEmpty()); CHECK(wire.count("AN2;AN;")==1);
 now+=200; model.m_active.reset(); model.m_queue.clear(); model.m_lastIo=now;
 CHECK(model.setAntenna(3)); model.tick(); model.receiveBytes("AN2;"); now+=2100; model.tick();
 CHECK(!model.operationBusy() && failed.size()==1 && wire.count("AN3;AN;")==1);
 now+=2100; model.m_lastIo=now; model.m_lastReply=now; model.m_received["TP"]=now;model.m_received["VFWD"]=now;
 CHECK(model.setAntenna(1)); model.tick(); model.receiveBytes("AN2;"); model.cancelOperation();
 const int count=wire.size(); now+=100;model.tick();CHECK(wire.size()==count && !model.operationBusy());
 now+=2100;model.m_lastIo=now;model.m_lastReply=now;model.m_received["TP"]=now;model.m_received["VFWD"]=now;
 CHECK(model.setAntenna(2)); model.tick(); model.receiveBytes("AN1;");
 model.query("VFWD;"); model.tick(); model.receiveBytes("VFWD1;"); now+=100; const int before=wire.size(); model.tick();
 CHECK(!model.operationBusy() && wire.size()==before);
 now+=2100;model.m_lastIo=now;model.m_lastReply=now;model.m_received["TP"]=now;model.m_received["VFWD"]=now;model.m_status.forwardAdc=0;
 CHECK(model.setAntenna(3));model.tick();model.receiveBytes("AN1;");
 now+=1900;model.m_lastIo=now;model.tick();CHECK(wire.last()=="AN;");
 const int beforeLate=wire.size(),beforeFinished=finished.size();now+=150;model.receiveBytes("AN3;");
 CHECK(!model.operationBusy() && finished.size()==beforeFinished && wire.size()==beforeLate);


 return 0;
}

struct Peer {
    Kat500Model model;
    qint64 now = 0;
    QByteArray internal = QByteArray(1024, char(0x51));
    QByteArray external = QByteArray(65536, char(0x72));
    QList<QByteArray> wire;
    QMap<QByteArray,QByteArray> config;
    QList<QByteArray> setters;
    bool rejectSetting = false;
    bool tuning = false;
    int cancelPolls = -1;
    int writes = 0;
    bool protectedWrite = false;
    bool dropWrite = false;
    bool corruptReadback = false;
    bool off = false;
    bool dropRead = false;
    bool wrongMemory = false;
    bool rf = false;
    Peer()
    {
        QFile fixture(QStringLiteral(KAT500_CONFIGURATION_PATH));
        if (fixture.open(QIODevice::ReadOnly)) {
            for (const QJsonValue value : QJsonDocument::fromJson(fixture.readAll()).array()) {
                const QJsonObject row = value.toObject();
                config.insert(row["request"].toString().toLatin1().chopped(1), row["reply"].toString().toLatin1());
            }
        }
        config["AE052"] = "AE0520;"; config["AP05"] = "AP051;";
        model.m_now = [this] { return now; };
        model.m_write = [this](const QByteArray& bytes) {
            wire.append(bytes);
            for (const QByteArray& command : bytes.split(';')) {
                if (command.isEmpty()) { continue; }
                QByteArray reply;
                if (command == "I") { reply = "KAT500;"; }
                else if (command == "RV") { reply = "RV02.16;"; }
                else if (command == "SN") { reply = "SN 222;"; }
                else if (command == "PS") { reply = off ? "PS0;" : "PS1;"; }
                else if (command == "T" || command == "FTNS") { tuning = true; }
                else if (command == "CT") { cancelPolls = 3; }
                else if (command == "TP") {
                    if (cancelPolls > 0 && --cancelPolls == 0) { tuning = false; }
                    reply = tuning ? "TP1;" : "TP0;";
                }
                else if (command == "AN") { reply = "AN1;"; }
                else if (command == "BYP") { reply = "BYPB;"; }
                else if (command == "SIDE") { reply = "SIDET;"; }
                else if (command == "C") { reply = "C00;"; }
                else if (command == "L") { reply = "L00;"; }
                else if (command.startsWith("DM")) { reply = wrongMemory ? "DM 14080-14099;AN2;BYP;VSWRB 1.00;5 UNUSED;" : "DM 14080-14099;AN1;BYP;VSWRB 1.00;5 UNUSED;"; }
                else if (command == "VFWD") { reply = rf ? "VFWD 1;" : "VFWD 0;"; }
                else if (command.startsWith("ER") || command.startsWith("EW")) {
                    const bool ext = command[2] == 'S'; const bool write = command[1] == 'W';
                    const QByteArray header = QByteArray::fromHex(command.mid(3,6));
                    const int address = static_cast<quint8>(header[0])*256 + static_cast<quint8>(header[1]);
                    const int count = static_cast<quint8>(header[2]);
                    if (!write && dropRead && address == 64) { continue; }
                    QByteArray& memory = ext ? external : internal;
                    if (write) {
                        ++writes;
                        protectedWrite |= address < (ext ? 4096 : 32) || address + count > memory.size();
                        memory.replace(address,count,QByteArray::fromHex(command.mid(9,count*2)));
                        if (dropWrite) { continue; }
                    }
                    QByteArray response = header;
                    if (!write) {
                        response += memory.mid(address,count);
                        if (corruptReadback && writes > 0) { response[3] = char(response[3] ^ 0x01); }
                    }
                    unsigned sum = 0; for (const char byte : response) { sum += static_cast<quint8>(byte); }
                    response.append(static_cast<char>(0U-sum));
                    reply = command.left(3) + response.toHex().toUpper() + ';';
                }
                else if (config.contains(command)) { reply = config[command]; }
                else {
                    for (auto it = config.begin(); it != config.end(); ++it) {
                        if (command.startsWith(it.key()) && command.size() > it.key().size()) {
                            setters.append(command);
                            if (!rejectSetting) { it.value() = command + ';'; }
                            break;
                        }
                    }
                }
                if (!reply.isEmpty()) { model.receiveBytes(reply); }
            }
            if (bytes == ";" || bytes.endsWith(";;")) { model.receiveBytes(";"); }
            return true;
        };
        model.transportOpened(); model.m_timer.stop();
        for (int i=0; i<7; ++i) { step(); }
    }
    void step() { now += 100; model.tick(); }
    void complete() { for (int i=0; i<50000 && model.operationBusy(); ++i) { step(); } }
};
// Reproduce MainWindow's availability-to-effective-frequency refresh wiring.
static int followDoesNotRetryAdmittedIntent()
{
    Peer peer;
    qint64 frequency = 14074000;
    const auto transport = peer.model.m_write;
    peer.model.m_write = [&](const QByteArray& bytes) {
        if (bytes.startsWith("F ") || bytes == "F;") {
            peer.wire.append(bytes); peer.model.receiveBytes("F 14000;"); return true;
        }
        return transport(bytes);
    };
    peer.model.setFollowTxFrequency(true);
    QObject::connect(&peer.model, &Kat500Model::availabilityChanged, &peer.model, [&] {
        if (peer.model.followTxFrequency() && peer.model.unavailableReason(true).isEmpty()) {
            peer.model.setEffectiveTxFrequencyHz(frequency);
        }
    });
    CHECK(peer.model.begin(QStringLiteral("Read-only maintenance"), false));
    peer.model.setEffectiveTxFrequencyHz(frequency);
    CHECK(!peer.wire.contains("F 14074;F;"));
    peer.model.finish(QStringLiteral("Read-only maintenance finished"));
    peer.complete();
    for (int i = 0; i < 90; ++i) {
        // Repeated fresh eligibility, as ordinary TP/VFWD polls provide it.
        peer.model.m_received["TP"] = peer.model.m_received["VFWD"] = peer.now;
        peer.model.m_lastReply = peer.now;
        peer.model.m_nextPoll = peer.now + 10000;
        peer.step();
    }
    CHECK(peer.wire.count("F 14074;F;") == 1);
    frequency = 14076000;
    peer.model.setEffectiveTxFrequencyHz(frequency);
    for (int i = 0; i < 5 && !peer.wire.contains("F 14076;F;"); ++i) { peer.step(); }
    CHECK(peer.wire.count("F 14076;F;") == 1);
    peer.model.cancelOperation();
    for (int i = 0; i < 25; ++i) { peer.model.m_nextPoll = peer.now + 10000; peer.step(); }
    CHECK(peer.wire.count("F 14076;F;") == 1);
    peer.model.setFollowTxFrequency(false);
    peer.model.setFollowTxFrequency(true);
    peer.model.m_lastReply = peer.now;
    peer.model.m_received["TP"] = peer.model.m_received["VFWD"] = peer.now;
    for (int i = 0; i < 5 && peer.wire.count("F 14076;F;") < 2; ++i) { peer.step(); }
    CHECK(peer.wire.count("F 14076;F;") == 2);
    return 0;
}
static int lateEepromRepliesStopRestore(bool readback)
{
    Peer peer;
    Backup backup{peer.internal, peer.external, peer.model.status().firmware, peer.model.status().serial};
    const auto transport = peer.model.m_write;
    QByteArray held;
    peer.model.m_write = [&](const QByteArray& bytes) {
        if (held.isEmpty() && (readback ? bytes.startsWith("ERP002020") && peer.writes == 1 : bytes.startsWith("EWP"))) {
            held = bytes; return true;
        }
        return transport(bytes);
    };
    QSignalSpy finished(&peer.model, &Kat500Model::operationFinished);
    QSignalSpy failed(&peer.model, &Kat500Model::operationFailed);
    CHECK(peer.model.restoreBackup(backup, true));
    for (int i = 0; i < 30 && held.isEmpty(); ++i) { peer.step(); }
    CHECK(!held.isEmpty() && peer.model.m_active);
    peer.now = peer.model.m_sentAt + peer.model.m_active->timeoutMs + 1;
    transport(held); // Receive-ready runs before the overdue timer callback.
    CHECK(!peer.model.operationBusy() && failed.size() == 1 && finished.isEmpty());
    for (int i = 0; i < 60; ++i) { peer.step(); }
    CHECK(peer.writes == 1 && finished.isEmpty());
    return 0;
}
static int lateMemoryTerminatorCannotPublish()
{
    Peer peer;
    const auto transport = peer.model.m_write;
    peer.model.m_write = [&](const QByteArray& bytes) {
        if (bytes == "DM14096;") { peer.model.receiveBytes("DM 14080-14099;AN3;BYP;VSWRB 1.00;"); return true; }
        return transport(bytes);
    };
    QSignalSpy published(&peer.model, &Kat500Model::memoriesRead);
    QSignalSpy failed(&peer.model, &Kat500Model::operationFailed);
    CHECK(peer.model.inspectMemories(14096)); peer.step();
    CHECK(peer.model.m_active && peer.model.m_memoryReading);
    peer.now = peer.model.m_sentAt + peer.model.m_active->timeoutMs + 1;
    peer.model.receiveBytes("5 UNUSED;");
    CHECK(published.isEmpty() && failed.size() == 1 && !peer.model.operationBusy());
    CHECK(!peer.model.status().antenna); // AN3 belongs only to the expired memory response.
    peer.now += 2001; peer.model.tick();
    CHECK(peer.model.isConnected()); // The late terminator still completes drain.
    return 0;
}
// Antenna controls must become available from polling, without opening Advanced.
static int antennaPermissionsLoadWhilePolling()
{
    Peer peer;
    int band = 5;
    const QMap<QByteArray,QByteArray> replies{
        {"VRFL;","VRFL0;"}, {"VSWR;","VSWR1.00;"}, {"VSWRB;","VSWRB1.00;"},
        {"F;","F14074;"}, {"MD;","MDA;"}, {"AMPI;","AMPI0;"},
        {"ATTN;","ATTN0;"}, {"FLT;","FLT0;"}, {"FY;","FY 14060-14079;"},
        {"FC;","FC14074;"}, {"FX;","FX14074;"}};
    const auto transport = peer.model.m_write;
    peer.model.m_write = [&](const QByteArray& bytes) {
        if (bytes == "BN;" || replies.contains(bytes)) {
            peer.wire.append(bytes);
            peer.model.receiveBytes(bytes == "BN;" ? "BN" + QByteArray::number(band) + ';' : replies[bytes]);
            return true;
        }
        return transport(bytes);
    };
    QSignalSpy busy(&peer.model, &Kat500Model::operationBusyChanged);
    CHECK(peer.model.configuration().isEmpty());
    for (int i = 0; i < 150; ++i) { peer.step(); }
    CHECK(peer.model.configuration().size() == 3);
    CHECK(peer.model.configuration().value({Setting::EnabledAntenna,5,1},-1) == 1);
    CHECK(peer.model.configuration().value({Setting::EnabledAntenna,5,2},-1) == 0);
    CHECK(peer.model.configuration().value({Setting::EnabledAntenna,5,3},-1) == 1);
    CHECK(!peer.model.setAntenna(2));
    CHECK(peer.setters.isEmpty() && busy.isEmpty());
    band = 3;
    for (int i = 0; i < 150; ++i) { peer.step(); }
    CHECK(peer.model.status().band == 3);
    CHECK(peer.model.configuration().contains({Setting::EnabledAntenna,3,1}));
    CHECK(peer.model.configuration().contains({Setting::EnabledAntenna,3,2}));
    CHECK(peer.model.configuration().contains({Setting::EnabledAntenna,3,3}));
    peer.model.transportClosed();
    CHECK(peer.model.configuration().isEmpty());
    peer.model.transportOpened(); peer.model.m_timer.stop();
    for (int i = 0; i < 150; ++i) { peer.step(); }
    CHECK(peer.model.configuration().size() == 3);
    CHECK(peer.model.configuration().contains({Setting::EnabledAntenna,3,3}));
    CHECK(peer.setters.isEmpty() && busy.isEmpty());
    return 0;
}
static int ordinaryPollLatencyKeepsSafetyFresh()
{
    Peer peer;
    const QMap<QByteArray,QByteArray> extra{
        {"VRFL;","VRFL0;"}, {"VSWR;","VSWR1.00;"}, {"VSWRB;","VSWRB1.00;"},
        {"F;","F14074;"}, {"BN;","BN5;"}, {"MD;","MDA;"}, {"AMPI;","AMPI1;"},
        {"ATTN;","ATTN0;"}, {"FLT;","FLT0;"}, {"FY;","FY 14060-14079;"},
        {"FC;","FC14074;"}, {"FX;","FX14074;"}};
    const auto transport = peer.model.m_write;
    QByteArray pending;
    qint64 replyAt = 0;
    peer.model.m_write = [&](const QByteArray& bytes) {
        pending = bytes; replyAt = peer.now + 300; return true;
    };
    for (int i = 0; i < 1200; ++i) {
        peer.step();
        if (!pending.isEmpty() && peer.now >= replyAt) {
            const QByteArray request = std::exchange(pending, {});
            if (extra.contains(request)) { peer.wire.append(request); peer.model.receiveBytes(extra[request]); }
            else { transport(request); }
        }
        CHECK(peer.model.unavailableReason(true).isEmpty());
        for (const QString& field : {QStringLiteral("VSWR"), QStringLiteral("VRFL"), QStringLiteral("AMPI")}) {
            CHECK(peer.model.fieldAgeMs(field) <= 5000);
            CHECK(peer.now < 5000 || peer.model.fieldAgeMs(field) >= 0);
        }
    }
    CHECK(peer.model.ampKeyBlockConfirmed());
    CHECK(peer.wire.count("SN;") >= 3); // Background round-robin is not starved.
    peer.rf = true;
    for (int i = 0; i < 35; ++i) {
        peer.step();
        if (!pending.isEmpty() && peer.now >= replyAt) {
            const QByteArray request = std::exchange(pending, {});
            if (extra.contains(request)) { peer.model.receiveBytes(extra[request]); } else { transport(request); }
        }
    }
    CHECK(!peer.model.unavailableReason(true).isEmpty());
    return 0;
}
static int rejectNewUnsafeReadback()
{
    const QList<QPair<QByteArray,QByteArray>> cases{{"VFWD;","VFWD 1;"},{"TP;","TP1;"},{"PS;","PS0;"}};
    for (const auto& [query,reply] : cases) {
        Peer peer;
        const auto transport = peer.model.m_write;
        peer.model.m_write = [&](const QByteArray& bytes) {
            return bytes == query ? true : transport(bytes);
        };
        peer.model.query(query); peer.step();
        CHECK(peer.model.m_active && peer.model.m_active->wire == query);
        CHECK(peer.model.setCapacitorMask(4));
        QSignalSpy failed(&peer.model,&Kat500Model::operationFailed);
        peer.model.receiveBytes(reply); peer.step();
        CHECK(!peer.wire.contains("C04;C;") && failed.size() == 1 && !peer.model.operationBusy());
    }
    Peer power;
    power.model.m_status.powerOn = false; power.model.m_status.forwardAdc = 1;
    CHECK(power.model.setPower(true)); power.complete(); CHECK(power.wire.contains("PS1;PS;"));
    Peer cancel;
    cancel.tuning = true; cancel.model.m_status.tuning = true; cancel.model.m_status.forwardAdc = 1;
    CHECK(cancel.model.cancelTune()); cancel.complete();
    CHECK(cancel.wire.contains("CT;TP;") && cancel.model.status().tuning == false);
    return 0;
}
static int doNotExposeFailedBackup()
{
    for (const int failure : {0,1,2,3}) {
        Peer peer;
        bool completeData = false;
        const auto transport = peer.model.m_write;
        peer.model.m_write = [&](const QByteArray& bytes) {
            if (completeData && bytes == "I;") {
                if (failure == 1) { return true; } // Final identity query times out.
                if (failure == 2) { peer.model.receiveBytes("SN 99999;"); return true; }
                if (failure == 3) { peer.model.transportClosed("Final preflight disconnected"); return true; }
                peer.rf = true;
            }
            const bool lastBlock = bytes.startsWith("ERSFFC040");
            const bool result = transport(bytes);
            if (lastBlock) { completeData = true; }
            return result;
        };
        QSignalSpy published(&peer.model,&Kat500Model::backupRead);
        QSignalSpy failed(&peer.model,&Kat500Model::operationFailed);
        CHECK(peer.model.readBackup());
        for (int i=0; i<2000 && !completeData; ++i) { peer.step(); }
        CHECK(completeData && published.isEmpty());
        CHECK(Kat500Model::encodeBackupFile(peer.model.backup()).isEmpty());
        peer.complete();
        CHECK(published.isEmpty() && failed.size() == 1);
        CHECK(Kat500Model::encodeBackupFile(peer.model.backup()).isEmpty());
    }
    return 0;
}
static int cancelResetDuringDiagnostic()
{
    Peer peer;
    QSignalSpy finished(&peer.model,&Kat500Model::operationFinished);
    QObject::connect(&peer.model,&Kat500Model::diagnostic,&peer.model,[&](const QString& text) {
        if (text.contains(QStringLiteral("TX EEINIT;"))) { peer.model.cancelOperation(); }
    });
    CHECK(peer.model.factoryReset(true)); peer.complete();
    for (int i=0; i<50; ++i) { peer.step(); }
    CHECK(peer.wire.contains("EEINIT;") && !peer.wire.contains("RST0;") && finished.isEmpty());
    CHECK(peer.model.isConnected() && !peer.model.operationBusy());
    return 0;
}
static int canceledMemoryDrainsWithoutTelemetry()
{
    for (const bool terminated : {false,true}) {
        Peer peer;
        peer.model.m_status.antenna = 1; peer.model.m_status.capacitorMask = 0; peer.model.m_status.inductorMask = 0;
        const auto transport = peer.model.m_write;
        peer.model.m_write = [&](const QByteArray& bytes) {
            if (bytes == "DM14096;") {
                peer.wire.append(bytes); peer.model.receiveBytes("DM 14080-14099;AN3;SIDEA;"); return true;
            }
            return transport(bytes);
        };
        CHECK(peer.model.inspectMemories(14096)); peer.step();
        CHECK(peer.model.m_memoryReading);
        peer.model.cancelOperation();
        peer.model.receiveBytes("C06;L02;VSWRB 1.33;");
        if (terminated) { peer.model.receiveBytes("5 UNUSED;"); }
        peer.now += 2001; peer.model.tick();
        CHECK(peer.model.status().antenna == 1 && peer.model.status().capacitorMask == 0 && peer.model.status().inductorMask == 0);
        CHECK(peer.model.isConnected() == terminated && !peer.model.operationBusy());
        for (const QByteArray& wire : peer.wire) {
            CHECK(!wire.contains("C06;") && !wire.contains("L02;") && !wire.contains("SM") && !wire.contains("EW"));
        }
    }
    return 0;
}
static int stateful()
{
    Peer peer;
    CHECK(peer.model.status().serial == "00222");
    QSignalSpy done(&peer.model,&Kat500Model::operationFinished);
    QSignalSpy failed(&peer.model,&Kat500Model::operationFailed);
    CHECK(peer.model.readBackup()); peer.complete();
    CHECK(done.size() == 1 && failed.isEmpty());
    CHECK(peer.model.backup().internal == peer.internal && peer.model.backup().external == peer.external);
    // A real stateful memory seam detects ACK-only success and range corruption.
    Backup changed = peer.model.backup(); changed.internal.fill(char(0x17)); changed.external.fill(char(0x38));
    peer.model.m_received["TP"] = peer.model.m_received["VFWD"] = peer.now;
    peer.model.m_lastReply = peer.now;
    CHECK(peer.model.restoreBackup(changed,true)); peer.complete();
    CHECK(done.size() == 2 && failed.isEmpty() && !peer.protectedWrite && peer.writes == 1951);
    CHECK(peer.internal.left(32) == QByteArray(32,char(0x51)) && peer.internal.mid(32) == QByteArray(992,char(0x17)));
    CHECK(peer.external.left(4096) == QByteArray(4096,char(0x72)) && peer.external.mid(4096) == QByteArray(61440,char(0x38)));
    for (const QByteArray& wire : peer.wire) { CHECK(!wire.contains("RST") && !wire.contains("PS1") && !wire.contains("FT;")); }
    Peer missing; missing.dropWrite = true; QSignalSpy uncertain(&missing.model,&Kat500Model::operationFailed);
    changed.serial = "00222"; changed.firmware = "02.16";
    CHECK(missing.model.restoreBackup(changed,true)); missing.complete();
    CHECK(uncertain.size() == 1 && missing.writes == 1);
    Peer mismatch; mismatch.corruptReadback = true; QSignalSpy incorrect(&mismatch.model,&Kat500Model::operationFailed);
    CHECK(mismatch.model.restoreBackup(changed,true)); mismatch.complete();
    CHECK(incorrect.size() == 1 && mismatch.writes == 1);
    Peer transmitting; transmitting.rf = true;
    CHECK(transmitting.model.restoreBackup(changed,true)); transmitting.complete(); CHECK(transmitting.writes == 0);
    Peer cancellation;
    CHECK(cancellation.model.restoreBackup(changed,true));
    for (int i=0; i<9; ++i) { cancellation.step(); }
    cancellation.model.cancelOperation(); const int before = cancellation.writes;
    for (int i=0; i<100; ++i) { cancellation.step(); }
    CHECK(cancellation.writes == before && before > 0);
    Peer signalCancel;
    QObject::connect(&signalCancel.model,&Kat500Model::operationProgress,&signalCancel.model,[&](int progress,int) {
        if (progress >= 32) { signalCancel.model.cancelOperation(); }
    });
    CHECK(signalCancel.model.restoreBackup(changed,true)); signalCancel.complete();
    for (int i=0; i<50; ++i) { signalCancel.step(); }
    CHECK(signalCancel.writes == 1);
    Peer poweredOff; poweredOff.off = true; poweredOff.model.m_status.powerOn = false; poweredOff.model.m_queue.clear();
    poweredOff.wire.clear();
    for (int i=0; i<110; ++i) { poweredOff.step(); }
    int probes = 0;
    for (const QByteArray& wire : poweredOff.wire) { CHECK(wire == ";" || wire == "PS;"); probes += wire == "PS;"; }
    CHECK(probes >= 2 && probes <= 3 && poweredOff.model.isDetected());
    Peer swapped;
    CHECK(swapped.model.restoreBackup(changed,true)); swapped.step();
    swapped.model.receiveBytes("SN 99999;"); CHECK(!swapped.model.isConnected());
    Peer partial; partial.dropRead = true;
    QSignalSpy noPartial(&partial.model,&Kat500Model::backupRead);
    CHECK(partial.model.readBackup()); partial.complete(); CHECK(noPartial.isEmpty());
    Peer history;
    CHECK(history.model.readFaultHistory()); history.complete();
    CHECK(history.model.faults().size() == 256 && history.model.faults().first().slot == 81);
    CHECK(!history.model.status().faultCode && history.model.faults().first().raw.size() == 16);
    Peer memory;
    CHECK(memory.model.memorize(14096)); memory.complete();
    CHECK(memory.model.memories().matches.size() == 1 && memory.wire.contains("SM 14096;DM14096;"));
    Peer wrongMemory; wrongMemory.wrongMemory = true;
    QSignalSpy saveRejected(&wrongMemory.model,&Kat500Model::operationFailed);
    CHECK(wrongMemory.model.memorize(14096)); wrongMemory.complete(); CHECK(saveRejected.size() == 1);
    Peer erased;
    CHECK(!erased.model.eraseMemories(-1,2,false));
    CHECK(erased.model.eraseMemories(-1,2,true)); erased.complete();
    int erasedBands = 0;
    for (const QByteArray& wire : erased.wire) {
        if (wire.startsWith("EM")) {
            CHECK(wire == "EM" + QByteArray::number(erasedBands).rightJustified(2,'0') + "2;RV;");
            ++erasedBands;
        }
    }
    CHECK(erasedBands == 11);
    Peer settings;
    CHECK(settings.model.readConfiguration()); settings.complete();
    CHECK(settings.model.configuration().size() == 103);
    Configuration edits{{{Setting::EnabledAntenna,5,2},1},{{Setting::PreferredAntenna,5},2},{{Setting::EnabledAntenna,5,1},0}};
    CHECK(settings.model.applyConfiguration(edits)); settings.complete();
    CHECK(settings.setters == QList<QByteArray>({"AE0521","AP052","AE0510"}));
    CHECK(settings.model.configuration().value({Setting::PreferredAntenna,5}) == 2);
    settings.setters.clear(); settings.rejectSetting = true;
    CHECK(settings.model.applyConfiguration({{{Setting::SlotsPerBin,5},4},{{Setting::AutoFineTune,5},0}})); settings.complete();
    CHECK(settings.setters.size() == 1 && settings.model.configuration().value({Setting::SlotsPerBin,5}) == 6);
    Peer tuning;
    CHECK(tuning.model.startTune()); tuning.complete(); CHECK(tuning.model.status().tuning == true);
    QSignalSpy tuneTelemetry(&tuning.model, &Kat500Model::telemetryReceived);
    tuning.model.receiveBytes("FT;");
    CHECK(tuneTelemetry.size() == 1 && tuneTelemetry.first().first().toString() == "FT");
    CHECK(tuning.model.cancelTune()); tuning.complete(); CHECK(tuning.model.status().tuning == false);
    Peer reset; QSignalSpy resetDone(&reset.model,&Kat500Model::operationFinished);
    CHECK(reset.model.factoryReset(true)); reset.complete();
    CHECK(reset.wire.contains("EEINIT;") && reset.wire.contains("RST0;") && !reset.model.isConnected());
    CHECK(!reset.wire.contains("PS1;"));
    return 0;
}
static int rejectUnsupportedMemoryFrequency()
{
    Peer peer;
    peer.wire.clear();
    QSignalSpy failed(&peer.model,&Kat500Model::operationFailed);
    for (const int frequency : {1, 1799, 54001, 99999}) {
        CHECK(!peer.model.recallFrequency(frequency));
        CHECK(!peer.model.memorize(frequency));
        CHECK(!peer.model.inspectMemories(frequency));
        CHECK(!peer.model.operationBusy() && peer.model.isConnected());
    }
    CHECK(peer.wire.isEmpty() && failed.size() == 12);
    peer.model.m_status.frequencyKhz = 0;
    CHECK(!peer.model.memorize() && !peer.model.inspectMemories());
    CHECK(peer.wire.isEmpty() && peer.model.isConnected());
    return 0;
}
static int transportBoundaries()
{
    Kat500Model model;
    CHECK(model.m_socket->readBufferSize() > 0 && model.m_socket->readBufferSize() <= 4096);
    QSignalSpy failed(&model, &Kat500Model::connectionFailed);
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
static int run()
{
    CHECK(transportBoundaries() == 0);
    Kat500Model model;
    qint64 now = 0; QList<QByteArray> sent;
    model.m_now = [&] { return now; };
    model.m_write = [&](const QByteArray& bytes) { sent.append(bytes); return true; };
    model.transportOpened(); model.m_timer.stop();
    auto next = [&]() { now += 100; model.tick(); return sent.isEmpty() ? QByteArray{} : sent.takeLast(); };
    CHECK(next() == ";"); CHECK(next() == ";");
    model.receiveBytes(";");
    CHECK(next() == "I;"); model.receiveBytes("KA"); CHECK(!model.isDetected()); model.receiveBytes("T500;");
    CHECK(model.isDetected());
    CHECK(next() == "RV;"); model.receiveBytes("RV02.16;");
    CHECK(next() == "SN;"); model.receiveBytes("SN 222;");
    CHECK(next() == "PS;"); model.receiveBytes("PS1;");
    CHECK(next() == "TP;"); model.receiveBytes("TP0;");
    CHECK(next() == "VFWD;"); model.receiveBytes("VFWD0;");
    CHECK(!model.followTxFrequency());
    model.setEffectiveTxFrequencyHz(14074000); CHECK(sent.isEmpty());
    QSignalSpy failed(&model,&Kat500Model::operationFailed);
    QSignalSpy finished(&model,&Kat500Model::operationFinished);
    CHECK(model.setMode(Mode::Manual));
    CHECK(next() == "MDM;MD;"); model.receiveBytes("MDA;");
    CHECK(failed.isEmpty() && finished.isEmpty() && model.operationBusy() && model.status().mode == Mode::Auto);
    now += 2100; model.tick();
    CHECK(failed.size() == 1 && finished.isEmpty());
    now += 4000; model.tick(); sent.clear();
    model.m_active.reset(); model.m_queue.clear(); model.m_waking = false; model.m_lastIo = now; model.m_lastReply = now;
    CHECK(model.inspectMemories(14096));
    CHECK(next() == "DM14096;");
    model.receiveBytes("DM 14080-14099;\r\nAN1;BYP;VSWRB 1.00;\r\n5 UNUSED;\r\n");
    CHECK(!model.status().antenna && !model.status().bypassed && model.memories().matches.size() == 1);
    model.m_queue.clear();
    CHECK(model.readBackup()); CHECK(next() == "I;");
    model.cancelOperation(); model.receiveBytes("KAT500;ERP00020100FD;");
    CHECK(!model.operationBusy());
    model.transportClosed("test disconnect");
    CHECK(!model.isDetected() && !model.isConnected());
    model.transportOpened(); model.m_timer.stop(); sent.clear();
    CHECK(next() == ";"); CHECK(!sent.contains("MDM;MD;"));
    model.transportClosed();
    const int failures = keyBlockReadback() | rejectUnsupportedMemoryFrequency() | followDoesNotRetryAdmittedIntent() | lateEepromRepliesStopRestore(false) | lateEepromRepliesStopRestore(true) |
        lateMemoryTerminatorCannotPublish() | antennaPermissionsLoadWhilePolling() | ordinaryPollLatencyKeepsSafetyFresh() | settlingReadback() | rejectNewUnsafeReadback() | doNotExposeFailedBackup() |
        cancelResetDuringDiagnostic() | canceledMemoryDrainsWithoutTelemetry();
    return failures ? 1 : stateful();
}
};
}
int main(int argc, char** argv) { QCoreApplication app(argc,argv); return AetherSDR::Kat500ModelTest::run(); }
