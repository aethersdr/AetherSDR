#include "models/AntennaAnalyzerModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QtTest>

#include <cmath>

namespace AetherSDR {
class AntennaAnalyzerModelTestAccess {
public:
    static void identityQuiet(AntennaAnalyzerModel& model)
    {
        model.m_identityQuiet.stop();
        QMetaObject::invokeMethod(&model.m_identityQuiet, "timeout", Qt::DirectConnection);
    }
    static void expire(AntennaAnalyzerModel& model) { model.deadlineExpired(); }
};
}

using namespace AetherSDR;

class InjectedHid final : public RigExpertHidTransport {
public:
    using RigExpertHidTransport::RigExpertHidTransport;
    QList<QByteArray> commands;
    QByteArray final;
    bool receipts{true};
    bool closeImmediately{true};
    void start(const QString&) override { emit opened(); }
    void send(const QByteArray& report) override
    {
        commands.append(report.mid(2, static_cast<unsigned char>(report.at(1))));
        if (receipts) { emit reportWritten(report); }
    }
    void finish(const QByteArray& report) override
    {
        final = report;
        if (!report.isEmpty()) { emit reportWritten(report); }
        if (closeImmediately) { emit closed(); }
    }
    void feed(const QByteArray& data)
    {
        // Incoming reports may be short; do not depend on zero-filled padding.
        QByteArray report;
        report.append(0x07);
        report.append(static_cast<char>(data.size()));
        report.append(data);
        emit reportReceived(report);
    }
};

class RigExpertUsbPocTest final : public QObject {
    Q_OBJECT
private:
    InjectedHid* install(AntennaAnalyzerModel& model)
    {
        auto* port = new InjectedHid(&model);
        model.setUsbBackend([] { return QList<RigExpertUsbDevice>{{"injected", "460012345", "Stick Pro"}}; },
                            [port](QObject*) { return port; });
        if (!model.refreshDevices() || !model.connectDevice(0)) { return nullptr; }
        return port;
    }
    void ready(AntennaAnalyzerModel& model, InjectedHid* port)
    {
        port->feed("Stick ");
        port->feed("Pro 1.234 REV 7\r\nSN\t460012345\r\nOK\r\n");
        AntennaAnalyzerModelTestAccess::identityQuiet(model);
    }
private slots:
    void reportBounds()
    {
        QCOMPARE(AntennaAnalyzerModel::commandReport("VER\r\n").toHex().left(14), QByteArray("07055645520d0a"));
        QCOMPARE(AntennaAnalyzerModel::commandReport(QByteArray(62, 'x')).size(), 64);
        QVERIFY(AntennaAnalyzerModel::commandReport(QByteArray(63, 'x')).isEmpty());
        QVERIFY(AntennaAnalyzerModel::commandReport({}).isEmpty());
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        QVERIFY(port);
        emit port->reportReceived(QByteArray::fromHex("073f00"));
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QCOMPARE(port->final, AntennaAnalyzerModel::commandReport("off\r"));
    }
    void stickPro108FullInfoLengthConvention()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        QVERIFY(port);
        port->feed("Stick Pro 108\r\n");
        // Public metadata bytes captured from firmware 108 on macOS. The
        // descriptor and a 256-byte read both confirm a 64-byte API report.
        const QByteArray payload("NAME\tStick Pro\r\nFW\tFirmware ver. 1.8 / 5 Feb 2020\r\nHW\tREU.1230");
        QCOMPARE(payload.size(), 62);
        QByteArray report = AntennaAnalyzerModel::commandReport(payload);
        report[1] = 63;
        emit port->reportReceived(report);
        port->feed("B.1.5\r\nOK\r\n");
        AntennaAnalyzerModelTestAccess::identityQuiet(model);
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Ready);
        QCOMPARE(model.snapshot().value("identity").toString(), QStringLiteral("Stick Pro 108"));
        QVERIFY(model.snapshot().value("fullInfoLines").toArray().contains(
            QStringLiteral("FW\tFirmware ver. 1.8 / 5 Feb 2020")));
        QCOMPARE(model.snapshot().value("identityLengthQuirkReports").toInt(), 1);
        QCOMPARE(port->commands.size(), 2);
    }
    void overclaimedMeasurementRemainsMalformed()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        ready(model, port);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        port->feed("OK\r\n");
        port->feed("OK\r\n");
        QByteArray payload("14,51.4,-1.02\r\n");
        payload.append(QByteArray(62 - payload.size(), ' '));
        QByteArray report = AntennaAnalyzerModel::commandReport(payload);
        report[1] = 63;
        emit port->reportReceived(report);
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QCOMPARE(model.snapshot().value("receivedRows").toInt(), 0);
    }
    void otherFullReportLengthErrorsRemainMalformed()
    {
        for (const int length : {64, 255}) {
            AntennaAnalyzerModel model;
            InjectedHid* port = install(model);
            port->feed("Stick Pro 108\r\n");
            QByteArray report = AntennaAnalyzerModel::commandReport(QByteArray(62, 'x'));
            report[1] = static_cast<char>(length);
            emit port->reportReceived(report);
            QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        }
    }
    void mathAndStrictCsv()
    {
        const QJsonObject match = AntennaAnalyzerModel::impedanceSample("14.180,50,0");
        QCOMPARE(match.value("swr").toDouble(), 1.0);
        QVERIFY(match.value("returnLossDb").isNull());
        QCOMPARE(match.value("quality").toString(), QStringLiteral("perfect-match"));
        const QJsonObject hundred = AntennaAnalyzerModel::impedanceSample("14.180,100,0");
        QVERIFY(std::abs(hundred.value("swr").toDouble() - 2.0) < 1e-10);
        QVERIFY(std::abs(hundred.value("returnLossDb").toDouble() - 9.542425094) < 1e-8);
        const QJsonObject marker = AntennaAnalyzerModel::impedanceSample("14.180,54,0");
        QVERIFY(std::abs(marker.value("swr").toDouble() - 1.08) < 1e-10);
        const QJsonObject reactive = AntennaAnalyzerModel::impedanceSample("14.180,50,50");
        QVERIFY(std::abs(reactive.value("swr").toDouble() - 2.618033989) < 1e-8);
        QVERIFY(AntennaAnalyzerModel::impedanceSample("14.1,nan,0").isEmpty());
        QVERIFY(AntennaAnalyzerModel::impedanceSample("14.1,50x,0").isEmpty());
        QVERIFY(AntennaAnalyzerModel::impedanceSample("14.1,50,0,0").isEmpty());
        QVERIFY(AntennaAnalyzerModel::impedanceSample("1e400,50,0").isEmpty());
        const QJsonObject shorted = AntennaAnalyzerModel::impedanceSample("14.1,0,0");
        QVERIFY(shorted.value("swr").isNull());
        QCOMPARE(shorted.value("quality").toString(), QStringLiteral("total-reflection"));
        const QJsonObject negative = AntennaAnalyzerModel::impedanceSample("14.1,-3,2");
        QCOMPARE(negative.value("rOhm").toDouble(), -3.0);
        QVERIFY(negative.value("swr").isNull());
    }
    void finiteSweepAndNoAutomaticRf()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        QVERIFY(port);
        QCOMPARE(port->commands, QList<QByteArray>{"VER\r\n"});
        ready(model, port);
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Ready);
        QCOMPARE(port->commands, (QList<QByteArray>{"VER\r\n", "FULLINFO\r\n"}));
        QCOMPARE(model.snapshot().value("identity").toString(), QStringLiteral("Stick Pro 1.234 REV 7"));
        QVERIFY(!model.sweep(14000000, 14350000, 2, false));
        QVERIFY(!model.sweep(99999, 14350000, 2, true));
        QVERIFY(!model.sweep(14000000, 14350001, 2, true));
        QVERIFY(!model.sweep(14000000, 14350000, 1001, true));
        QCOMPARE(port->commands.size(), 2);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        QCOMPARE(port->commands.last(), QByteArray("FQ14175000\r"));
        port->feed("OK\r\n");
        QCOMPARE(port->commands.last(), QByteArray("SW350000\r"));
        port->feed("OK\r\n");
        QCOMPARE(port->commands.last(), QByteArray("FRX2\r"));
        port->feed("OK\r\n14,100,0\r\n14.175,50,0\r\n");
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Sweeping);
        port->feed("14.35,75,2\r\n");
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Complete);
        QCOMPARE(model.snapshot().value("receivedRows").toInt(), 3);
        QCOMPARE(port->final, AntennaAnalyzerModel::commandReport("off\r"));
        QVERIFY(!model.snapshot().value("rfOffConfirmed").toBool());
        QVERIFY(!model.sweep(14000000, 14350000, 2, true));
    }
    void cancellationAndOldGeneration()
    {
        AntennaAnalyzerModel model;
        InjectedHid* old = install(model);
        ready(model, old);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        old->closeImmediately = false;
        model.stop();
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Closing);
        QVERIFY(!model.connectDevice(0));
        const qsizetype sent = old->commands.size();
        old->feed("OK\r\n14,50,0\r\n");
        QCOMPARE(old->commands.size(), sent);
        QCOMPARE(model.snapshot().value("receivedRows").toInt(), 0);
        emit old->closed();
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Cancelled);
        InjectedHid* current = install(model);
        old->feed("Stick Pro 9.99\r\nOK\r\n14,50,0\r\n");
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Identifying);
        QCOMPARE(current->commands.size(), 1);
        QCOMPARE(model.snapshot().value("generation").toInt(), 2);
    }
    void malformedOrderingAndDeadline()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        ready(model, port);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        port->feed("OK\r\n");
        port->feed("OK\r\n");
        port->feed("14,50,0\r\n14,51,0\r\n");
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QCOMPARE(model.snapshot().value("receivedRows").toInt(), 1);
        InjectedHid* next = install(model);
        ready(model, next);
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        AntennaAnalyzerModelTestAccess::expire(model);
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QVERIFY(model.lastError().contains("deadline"));
    }
    void acknowledgementRequiresWrite()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        ready(model, port);
        port->receipts = false;
        QVERIFY(model.sweep(14000000, 14350000, 2, true));
        port->feed("OK\r\n");
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QVERIFY(model.lastError().contains("write receipt"));
    }
    void oversizedLineAndWrongIdentity()
    {
        AntennaAnalyzerModel model;
        InjectedHid* port = install(model);
        port->feed("AA-230 ZOOM 1.23\r\n");
        AntennaAnalyzerModelTestAccess::identityQuiet(model);
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Identifying);
        for (int i = 0; i < 9; ++i) { port->feed(QByteArray(62, 'x')); }
        QCOMPARE(model.state(), AntennaAnalyzerModel::State::Error);
        QVERIFY(model.lastError().contains("512"));
    }
};

QTEST_GUILESS_MAIN(RigExpertUsbPocTest)
#include "rigexpert_usb_poc_test.moc"
