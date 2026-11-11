// Characterization harness for the FlexStatusIngress move (scaffolding, not a
// regression test): feeds a corpus of wire lines through the production
// RadioConnection::processLine on its own thread into a RadioModel with a real
// FlexBackend. Run with QtTest's -vs to trace every signal emitted; the
// normalized trace (tools/flex_ingress_trace.py, corpus in
// tests/data/flex_ingress_corpus.txt) is diffed before and after each stage.
// No socket is opened: lines enter below the socket.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/flex/RadioConnection.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
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
    }
};
}

using namespace AetherSDR;

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
            ++fed;
        }
        qInfo("fed %d lines", fed);
        QVERIFY(fed > 0);
    }
};

QTEST_GUILESS_MAIN(FlexIngressCharacterizationTest)
#include "flex_ingress_characterization_test.moc"
