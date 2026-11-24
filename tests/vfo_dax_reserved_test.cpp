// The VFO's DAX list names a channel held by a program on the radio, such as
// the WSPR receiver (#7139), and still offers it. Socket-free: statuses are
// injected into a real RadioModel; no connect, peer or radio.
#include "FlexSliceStatusTestSupport.h"
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/VfoWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QComboBox>
#include <QtTest>

using namespace AetherSDR;

class VfoDaxReservedTest : public QObject
{
    Q_OBJECT
private slots:
    void reservedChannelIsNamedAndSelectable()
    {
        RadioModel radio;
        VfoWidget vfo;
        vfo.setRadioModel(&radio);
        auto* dax = vfo.findChild<QComboBox*>(QStringLiteral("vfoDaxChannel"));
        QVERIFY(dax);
        QVERIFY(dax->count() >= 3);
        QCOMPARE(dax->itemText(2), QStringLiteral("2"));

        radio.reserveSlice(3, QStringLiteral("WSPR receiver"));
        applyFlexSliceStatus(radio, 3, {{QStringLiteral("dax"), QStringLiteral("2")}});
        QCOMPARE(dax->itemText(2), QStringLiteral("2 (WSPR receiver)"));
        QCOMPARE(dax->itemText(1), QStringLiteral("1"));
        const QModelIndex held = dax->model()->index(2, 0);
        QVERIFY(dax->model()->flags(held).testFlag(Qt::ItemIsEnabled));
        QVERIFY(dax->model()->flags(held).testFlag(Qt::ItemIsSelectable));

        // The program moves to another channel, then the radio removes its slice.
        applyFlexSliceStatus(radio, 3, {{QStringLiteral("dax"), QStringLiteral("1")}});
        QCOMPARE(dax->itemText(1), QStringLiteral("1 (WSPR receiver)"));
        QCOMPARE(dax->itemText(2), QStringLiteral("2"));
        applyFlexSliceStatus(radio, 3, {{QStringLiteral("in_use"), QStringLiteral("0")}}, true);
        QCOMPARE(dax->itemText(1), QStringLiteral("1"));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("vfo-dax-reserved"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    VfoDaxReservedTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "vfo_dax_reserved_test.moc"
