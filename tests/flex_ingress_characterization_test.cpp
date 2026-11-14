// Characterization harness for the FlexStatusIngress move (scaffolding, not a
// regression test): feeds a corpus of wire lines through the production
// RadioConnection::processLine on its own thread into a RadioModel with a real
// FlexBackend. Run with QtTest's -vs -maxwarnings 0 (the default limit of
// 2000 messages silently drops the rest) to trace every signal emitted; the
// normalized trace (tools/flex_ingress_trace.py, corpus in
// tests/data/flex_ingress_corpus.txt) is diffed before and after each stage.
// No socket is opened: lines enter below the socket.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/AmpModel.h"
#include "models/CwxModel.h"
#include "models/DvkModel.h"
#include "models/EqualizerModel.h"
#include "models/FlexWaveformModel.h"
#include "models/NavtexModel.h"
#include "models/PanadapterModel.h"
#include "models/SliceModel.h"
#include "models/RadioModel.h"
#include "models/SpotModel.h"
#include "models/TnfModel.h"
#include "models/TransmitModel.h"
#include "models/TunerModel.h"

// The typed firmware report exists only once the ingress does; the harness
// still builds on a tree without it, for a before/after run.
#if __has_include("core/backends/flex/FlexStatusIngress.h")
#define AETHER_HAS_FLEX_STATUS_INGRESS 1
#endif

#include <QCoreApplication>
#include <algorithm>
#include <QFile>
#include <QtTest>

namespace AetherSDR {
class RadioConnectionSessionTestAccess {
public:
    static void process(RadioConnection& connection, const QString& line) { connection.processLine(line); }
    // Our handle without the H line, whose connected() would run the full
    // session bootstrap (sleep inhibitor, network lookups).
    static void identity(RadioConnection& connection, quint32 handle)
    {
        connection.resetSessionState();   // a live session, as a connect starts
        connection.m_handle.store(handle);
        // Connected, without the H line's connected() bootstrap; every command
        // the model writes is logged instead of reaching a socket.
        connection.m_state.store(ConnectionState::Connected);
        connection.m_commandSinkForTest = [](quint32, const QString& command) {
            qInfo("WRITE %s", qPrintable(command));
        };
    }
};
}

using namespace AetherSDR;

namespace {
// The model state stage 4 can change without a signal, one line per model.
void dumpState(RadioModel& model)
{
    QStringList parts;
    for (auto it = model.txBandSettings().cbegin(); it != model.txBandSettings().cend(); ++it) {
        const auto& b = it.value();
        parts << QStringLiteral("band%1[%2 rf=%3 tune=%4 inh=%5 alc=%6 acc=%7 rca=%8 accTx=%9 tx=%10%11%12]")
            .arg(it.key()).arg(b.bandName).arg(b.rfPower).arg(b.tunePower).arg(b.inhibit).arg(b.hwAlc)
            .arg(b.accTxReq).arg(b.rcaTxReq).arg(b.accTx).arg(b.tx1).arg(b.tx2).arg(b.tx3);
    }
    qInfo("STATE bands %s", qPrintable(parts.join(QLatin1Char(' '))));
    parts.clear();
    for (const TnfEntry& t : model.tnfModel().tnfs()) {
        parts << QStringLiteral("tnf%1[%2 w=%3 d=%4 p=%5]").arg(t.id).arg(t.freqMhz, 0, 'f', 6).arg(t.widthHz).arg(t.depthDb).arg(t.permanent);
    }
    parts << QStringLiteral("tnfOn=%1").arg(model.tnfModel().globalEnabled());
    qInfo("STATE tnf %s", qPrintable(parts.join(QLatin1Char(' '))));
    parts.clear();
    for (const SpotData& sp : model.spotModel().spots()) {
        parts << QStringLiteral("spot%1[%2 rx=%3 tx=%4 %5 %6 %7 %8 %9 life=%10]").arg(sp.index).arg(sp.callsign)
            .arg(sp.rxFreqMhz, 0, 'f', 6).arg(sp.txFreqMhz, 0, 'f', 6).arg(sp.mode, sp.color, sp.source, sp.spotterCallsign, sp.comment)
            .arg(sp.lifetimeSeconds);
    }
    qInfo("STATE spots %s", qPrintable(parts.join(QLatin1Char(' '))));
    parts.clear();
    for (const VoiceKeyerRecording& r : model.dvkModel().recordings()) {
        parts << QStringLiteral("rec%1[%2 %3ms]").arg(r.id).arg(r.name).arg(r.durationMs);
    }
    qInfo("STATE dvk status=%d active=%d enabled=%d %s", int(model.dvkModel().status()), model.dvkModel().activeId(),
          int(model.dvkModel().enabled()), qPrintable(parts.join(QLatin1Char(' '))));
    parts.clear();
    for (const auto& m : model.navtexModel().messages()) {
        parts << QStringLiteral("msg%1[%2 %3 %4]").arg(m.idx).arg(m.serialNum).arg(m.dateTime, m.msgText);
    }
    qInfo("STATE navtex status=%d %s", int(model.navtexModel().status()), qPrintable(parts.join(QLatin1Char(' '))));
    qInfo("STATE cwx sent=%d end=%d step=%d", model.cwxModel().sentIndex(), model.cwxModel().cwxEndIndex(),
          model.cwxModel().speedStep());
    qInfo("STATE eq tx=%d rx=%d", int(model.equalizerModel().txEnabled()), int(model.equalizerModel().rxEnabled()));
    qInfo("STATE waveform wfp=%d ip=%s reports=%d", int(model.flexWaveformModel().wfpStatusSeen()),
          qPrintable(model.flexWaveformModel().wfpIpAddress()), int(model.flexWaveformModel().statusReports().size()));
    qInfo("STATE amp model=%s state=%s fan=%s meffa=%s", qPrintable(model.amplifier().modelName()),
          qPrintable(model.amplifier().stateText()), qPrintable(model.amplifier().fanMode()), qPrintable(model.amplifier().meffa()));
    qInfo("STATE tuner model=%s serial=%s operate=%d present=%d", qPrintable(model.tunerModel().modelName()),
          qPrintable(model.tunerModel().serialNum()), int(model.tunerModel().isOperate()), int(model.tunerModel().isPresent()));
    QStringList pans;
    QList<PanadapterModel*> models = model.panadapters();
    std::sort(models.begin(), models.end(), [](auto* a, auto* b) { return a->panId() < b->panId(); });
    for (const PanadapterModel* pan : models) {
        pans << QStringLiteral("%1[c=%2 bw=%3 dbm=%4..%5 fps=%6 avg=%7 rf=%8 ant=%9 wnb=%10/%11 wf=%12 line=%13 owner=%14]")
            .arg(pan->panId()).arg(pan->centerMhz()).arg(pan->bandwidthMhz()).arg(pan->minDbm()).arg(pan->maxDbm())
            .arg(pan->fps()).arg(pan->average()).arg(pan->rfGain()).arg(pan->rxAntenna()).arg(int(pan->wnbActive()))
            .arg(pan->wnbLevel()).arg(pan->waterfallId()).arg(pan->waterfallLineDuration())
            .arg(pan->ownerHandle(), 0, 16);
    }
    qInfo("STATE pans %s", qPrintable(pans.join(QLatin1Char(' '))));
    parts.clear();
    for (const auto& x : model.xvtrList()) {
        parts << QStringLiteral("xvtr%1[%2 o=%3 rf=%4 if=%5 lo=%6 g=%7 p=%8 rx=%9 v=%10/%11]").arg(x.index).arg(x.name)
            .arg(x.order).arg(x.rfFreq).arg(x.ifFreq).arg(x.loError).arg(x.rxGain).arg(x.maxPower).arg(int(x.rxOnly))
            .arg(int(x.isValid)).arg(int(x.hasIsValid));
    }
    for (const UsbCable& c : model.usbCableModel().cables()) {
        QString bits;
        for (int i = 0; i < 8; ++i) {
            const UsbCableBit& b = c.bits[i];
            bits += QStringLiteral(" b%1[%2 %3 %4 %5 %6 %7 %8 %9 %10]").arg(i).arg(int(b.enabled)).arg(b.source, b.band)
                .arg(b.lowFreqMhz).arg(b.highFreqMhz).arg(int(b.activeHigh)).arg(int(b.pttDependent))
                .arg(b.pttDelayMs).arg(b.txDelayMs);
        }
        parts << QStringLiteral("usb[%1 %2 %3 en=%4 in=%5 log=%6 %7/%8/%9/%10/%11 src=%12 auto=%13 hi=%14 ldpa=%15/%16%17]")
            .arg(c.serialNumber, c.type, c.name).arg(int(c.enabled)).arg(int(c.present)).arg(int(c.loggingEnabled))
            .arg(c.speed).arg(c.dataBits).arg(c.parity).arg(c.stopBits).arg(c.flowControl).arg(c.source)
            .arg(int(c.autoReport)).arg(int(c.activeHigh)).arg(c.ldpaBand).arg(int(c.preamp)).arg(bits);
    }
    qInfo("STATE setup %s sharp=%d/%d,%d/%d,%d/%d", qPrintable(parts.join(QLatin1Char(' '))),
          model.filterSharpnessVoice(), int(model.filterSharpnessVoiceAuto()), model.filterSharpnessCw(),
          int(model.filterSharpnessCwAuto()), model.filterSharpnessDigital(), int(model.filterSharpnessDigitalAuto()));
    parts.clear();
    for (int id = 0; id < 8; ++id) {
        if (const SliceModel* sl = model.slice(id)) {
            parts << QStringLiteral("s%1[%2 %3 %4 %5..%6 act=%7 tx=%8 ant=%9/%10 dax=%11]").arg(id).arg(sl->frequency(), 0, 'f', 6)
                .arg(sl->mode(), sl->panId()).arg(sl->filterLow()).arg(sl->filterHigh()).arg(int(sl->isActive()))
                .arg(int(sl->isTxSlice())).arg(sl->rxAntenna(), sl->txAntenna()).arg(sl->daxChannel());
        } else if (model.isSlotForeign(id)) {
            parts << QStringLiteral("s%1[foreign]").arg(id);
        }
    }
    qInfo("STATE slices %s", qPrintable(parts.join(QLatin1Char(' '))));
    qInfo("STATE license id=%s max=%s sub=%s exp=%s dvk=%d/%d nr=%d/%d", qPrintable(model.licenseRadioId()),
          qPrintable(model.licenseMaxVersion()), qPrintable(model.licenseSubscription()),
          qPrintable(model.licenseExpirationDate()), int(model.licenseFeatureSeen(QStringLiteral("digital_voice_keyer"))),
          int(model.licenseFeatureEnabled(QStringLiteral("digital_voice_keyer"))),
          int(model.licenseFeatureSeen(QStringLiteral("noise_reduction"))),
          int(model.licenseFeatureEnabled(QStringLiteral("noise_reduction"))));
}
} // namespace

class FlexIngressCharacterizationTest final : public QObject {
    Q_OBJECT
private slots:
    void corpus()
    {
        const QString path = qEnvironmentVariable("AETHER_INGRESS_CORPUS");
        if (path.isEmpty()) { QSKIP("set AETHER_INGRESS_CORPUS to a corpus file"); }
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        TestSettingsProfile profile(QStringLiteral("flex-ingress-characterization"));
        QVERIFY(profile.isValid());
        AppSettings::instance().setValue("InhibitSleepWhileConnected", "False");
        RadioModel model;
        QVERIFY(model.rebuildBackendForTest(QStringLiteral("flex")));
        RadioConnection* connection = model.connection();
        QVERIFY(connection);
#ifdef AETHER_HAS_FLEX_STATUS_INGRESS
        connect(&model, &RadioModel::firmwareUpdateReported, this, [](const FirmwareUpdateStatus& s) {
            qInfo("STATE firmware failed=%s transfer=%s reason=%s",
                  s.failed ? (*s.failed ? "1" : "0") : "-",
                  s.transfer ? qPrintable(QString::number(*s.transfer)) : "-", qPrintable(s.reason));
        });
#endif
        QMetaObject::invokeMethod(connection, [connection] {
            RadioConnectionSessionTestAccess::identity(*connection, 0x71);
        }, Qt::BlockingQueuedConnection);
        int fed = 0;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char('H'))) { continue; }
            qInfo("LINE %s", qPrintable(line));
            QMetaObject::invokeMethod(connection, [connection, line] {
                RadioConnectionSessionTestAccess::process(*connection, line);
            }, Qt::BlockingQueuedConnection);
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents();
            // Flush the commands the model queued to the connection's thread.
            QMetaObject::invokeMethod(connection, [] {}, Qt::BlockingQueuedConnection);
            QCoreApplication::sendPostedEvents();
            dumpState(model);
            ++fed;
        }
        qInfo("fed %d lines", fed);
        QVERIFY(fed > 0);
    }
};

QTEST_GUILESS_MAIN(FlexIngressCharacterizationTest)
#include "flex_ingress_characterization_test.moc"
