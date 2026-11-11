// Socket-free production UI -> RadioModel -> Kenwood command/readback tests.
// Injected observations prove client behavior, not firmware or audible effects.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeReceiveSettingsWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QJsonDocument>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest>

#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("test-user", "test-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void message(KenwoodBackend& backend, const QByteArray& message)
    { backend.adopt(message); }
};
}

using namespace AetherSDR;
namespace {
const std::array<const char*, 18> kIds{{
    "apf.enabled", "apf.shift", "apf.bandwidth", "apf.gain", "notch.enabled",
    "notch.width", "notch.position", "nr.mode", "nr1.level", "nr2.time",
    "nb1.enabled", "nb1.level", "nb2.enabled", "nb2.level", "nb2.type",
    "nb2.depth", "nb2.width", "bc.mode"}};

QComboBox* editor(QWidget& widget, const char* id)
{
    return widget.findChild<QComboBox*>(QStringLiteral("nativeReceive_") + QString::fromLatin1(id));
}

bool activate(QComboBox& combo, int value)
{
    const int index = combo.findData(value);
    if (index < 0) { return false; }
    // Qt's actual activated signal reaches the production submit slot. No
    // backend setter, copied relay, private widget method or socket is used.
    combo.setCurrentIndex(index);
    return QMetaObject::invokeMethod(&combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}

struct Fixture {
    QList<QByteArray> commands;
    bool acceptWrites = true;
    QVariantMap latestStatus;
    std::function<void(const QVariantMap&)> onStatus;
    std::function<void(const QVariant&)> onResult;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    std::unique_ptr<NativeReceiveSettingsWidget> widget;
    Fixture()
    {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { commands.append(bytes); return acceptWrites; }, {}});
        backend = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& state) {
                if (kind == QLatin1String("receiveControls")) {
                    latestStatus = state;
                    if (onStatus) { onStatus(state); }
                }
            });
        QObject::connect(backend, &IRadioBackend::extensionResult, &model,
            [this](quint64, const QVariant& result) {
                if (onResult) { onResult(result); }
            });
        widget = std::make_unique<NativeReceiveSettingsWidget>(model);
        widget->resize(650, 900);
        widget->show();
    }
    void observe(const QByteArray& bytes)
    { kenwood::KenwoodBackendTestAccess::message(*backend, bytes); }
    void ready(char mode = '3', bool monitorObserved = true)
    {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        observe("ID024"); observe("FR0"); observe("FT0");
        observe("FA00014074000");
        observe(QByteArray("OM0") + mode);
        // Display-mode observations invalidate physical VFO mode authority.
        // The matching SF report must arrive after that notification.
        observe(QByteArray("SF000014074000") + mode);
        observe("TS0"); observe("RX");
        if (monitorObserved) { observe("MO20"); }
        // The injection seam omits setupBackend's session lifecycle wiring.
        emit model.connectionStateChanged(true);
    }
    void controls()
    {
        for (const QByteArray& frame : {QByteArray("AP01"), QByteArray("AP140"), QByteArray("AP20"),
                QByteArray("AP33"), QByteArray("NT1"), QByteArray("NW1"), QByteArray("BP128"),
                QByteArray("NR1"), QByteArray("RL105"), QByteArray("RL204"), QByteArray("NB11"),
                QByteArray("NL1010"), QByteArray("NB21"), QByteArray("NBT1"), QByteArray("NL2006"),
                QByteArray("NBD010"), QByteArray("NBW010"), QByteArray("BC1")}) {
            observe(frame);
        }
    }
};

QVariantMap validState(int value = 0)
{
    return {{"statusRevision", "0"}, {"context", "test-context"}, {"mode", "USB"}, {"editable", true},
        {"controls", QVariantList{QVariantMap{{"id", "nb1.enabled"}, {"label", "Noise blanker 1"},
            {"unit", ""}, {"minimum", 0}, {"maximum", 1}, {"step", 1}, {"value", value},
            {"editable", true}, {"reason", ""}}}}};
}

class DescriptorBackend final : public IRadioBackend {
public:
    QVariantMap state = validState();
    bool connected = true;
    bool advertised = true;
    bool publishOnDestruction = false;
    int writes = 0;
    ~DescriptorBackend() override
    {
        if (publishOnDestruction) {
            emit extensionStatus(QStringLiteral("test-native"), QStringLiteral("receiveControls"), validState(1));
        }
    }
    RadioCapabilities capabilities() const override
    {
        RadioCapabilities caps;
        if (advertised) {
            caps.extensionNamespaces = {QStringLiteral("test-native")};
            caps.extensions.insert(QStringLiteral("test-native"),
                QVariantMap{{QStringLiteral("nativeReceiveControlsVersion"), 1}});
        }
        return caps;
    }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override { connected = false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&, const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString& verb, quint64 id, const QVariant&) override
    {
        if (verb == QLatin1String("receive.controls.get")) { emit extensionResult(id, state); }
        else { ++writes; }
    }
    void publish(const QVariantMap& next)
    { emit extensionStatus(QStringLiteral("test-native"), QStringLiteral("receiveControls"), next); }
};
}

class NativeReceiveSettingsTest final : public QObject {
    Q_OBJECT
private slots:
    void unknownDspMonitorCannotQualifyApfOrFilterProjection()
    {
        Fixture f; f.ready('3', false); f.controls();
        f.observe("SL010"); f.observe("SH0080");
        QVERIFY(!f.backend->capabilities().hasAudioPeakingFilter);
        QVERIFY(!editor(*f.widget, "apf.enabled")->isEnabled());
        SliceModel* slice = f.model.slice(0);
        QVERIFY(slice);
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        f.observe("MO20");
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        f.observe("AP01"); f.observe("SL010"); f.observe("SH0080");
        QVERIFY(f.backend->capabilities().hasAudioPeakingFilter);
        QVERIFY(slice->receiveObservation().filterLowHz.has_value());
    }

    void unknownDspMonitorRefusesSharedPassbandWrite()
    {
        Fixture f; f.ready('3', false);
        f.commands.clear();
        f.backend->setSliceFilter(0, -250, 250);
        QVERIFY(f.commands.isEmpty());
    }
    void nativeMonitorsUseExplicitReadbackAndNeverReleaseOnClose()
    {
        for (const auto& [id, prefix] : {std::pair{"monitor.rx", "MO1"}, std::pair{"monitor.dsp", "MO2"}}) {
            Fixture f;
            f.ready(); f.controls();
            f.observe("MO10"); f.observe("MO20"); f.observe("AP01");
            QComboBox* combo = editor(*f.widget, id);
            QVERIFY(combo && combo->isEnabled());
            QCOMPARE(combo->count(), 2);
            QPushButton* reset = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveReset_") + id);
            QVERIFY(reset && !reset->isEnabled());
            for (const int value : {1, 0}) {
                f.commands.clear();
                const int before = combo->currentData().toInt();
                QVERIFY(activate(*combo, value));
                QVERIFY(f.commands.contains(QByteArray(prefix) + QByteArray::number(value) + ";"));
                QVERIFY(f.commands.contains(QByteArray(prefix) + ";"));
                QCOMPARE(combo->currentData().toInt(), before);
                QVERIFY(!combo->isEnabled());
                f.observe(QByteArray(prefix) + QByteArray::number(value));
                QCOMPARE(combo->currentData().toInt(), value);
                QVERIFY(combo->isEnabled());
            }
            const QString qaDirectory = qEnvironmentVariable("AETHER_MONITOR_QA_DIR");
            if (!qaDirectory.isEmpty()) {
                QVERIFY(QDir().mkpath(qaDirectory));
                f.widget->resize(850, 1350);
                QApplication::processEvents();
                QVERIFY(f.widget->grab().save(qaDirectory + QLatin1Char('/') + id + QStringLiteral(".png")));
            }
            f.commands.clear();
            f.widget.reset();
            QVERIFY(f.commands.isEmpty());
        }
    }

    void dspMonitorWithdrawsApfAuthorityWithoutInventingOff()
    {
        Fixture f; f.ready(); f.controls(); f.observe("MO20"); f.observe("AP01");
        QComboBox* apf = editor(*f.widget, "apf.enabled");
        QVERIFY(apf && apf->isEnabled());
        QCOMPARE(apf->currentData().toInt(), 2);
        f.commands.clear();
        f.observe("MO21");
        QVERIFY(!apf->isEnabled());
        QVERIFY(!apf->currentData().isValid());
        QVERIFY(f.commands.contains("AP0;"));
        QVERIFY(!f.commands.contains("AP00;"));
        f.observe("AP00");
        QCOMPARE(apf->currentData().toInt(), 1);
        QVERIFY(!apf->isEnabled());
        f.observe("MO20");
        QVERIFY(!apf->currentData().isValid());
        f.observe("AP01");
        QCOMPARE(apf->currentData().toInt(), 2);
        QVERIFY(apf->isEnabled());
    }

    void malformedMonitorReportsCannotChangeConfirmedChoice()
    {
        Fixture f; f.ready(); f.controls(); f.observe("MO10"); f.observe("MO20");
        for (const QByteArray& frame : {QByteArray("MO12"), QByteArray("MO19"), QByteArray("MO1x"),
                QByteArray("MO110"), QByteArray("MO2"), QByteArray("MO2-1"), QByteArray("MO29")}) {
            f.observe(frame);
        }
        for (const char* id : {"monitor.rx", "monitor.dsp"}) {
            QComboBox* combo = editor(*f.widget, id);
            QVERIFY(combo && combo->isEnabled());
            QCOMPARE(combo->currentData().toInt(), 0);
        }
    }

    void dspMonitorCannotPublishConfiguredAxesOrApfAsEffective()
    {
        Fixture f; f.ready(); f.controls(); f.observe("MO20"); f.observe("AP01");
        f.observe("SL010"); f.observe("SH0080");
        SliceModel* slice = f.model.slice(0);
        QVERIFY(slice);
        QVERIFY(slice->receiveObservation().filterLowHz.has_value());
        bool publishedApfOn = false;
        QObject::connect(f.backend, &IRadioBackend::sliceChanged, &f.model,
            [&](int, const SliceDelta& delta) { if (delta.apf == true) { publishedApfOn = true; } });
        f.observe("MO21");
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        f.observe("SL010"); f.observe("SH0080"); f.observe("AP01");
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        QVERIFY(!slice->receiveObservation().filterHighHz.has_value());
        QVERIFY(!f.backend->capabilities().hasAudioPeakingFilter);
        QVERIFY(!publishedApfOn);
        f.observe("MO20");
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        f.observe("SL010");
        QVERIFY(!slice->receiveObservation().filterLowHz.has_value());
        f.observe("SH0080");
        QVERIFY(slice->receiveObservation().filterLowHz.has_value());
        QVERIFY(slice->receiveObservation().filterHighHz.has_value());
    }

    void apfReplyDuringDspMonitorCannotReenableSharedAuthority()
    {
        Fixture f; f.ready(); f.controls(); f.observe("MO20"); f.observe("AP01");
        QVERIFY(f.backend->capabilities().hasAudioPeakingFilter);
        bool publishedApfOn = false;
        QObject::connect(f.backend, &IRadioBackend::sliceChanged, &f.model,
            [&](int, const SliceDelta& delta) { if (delta.apf == true) { publishedApfOn = true; } });
        f.observe("MO21"); f.observe("AP01");
        QVERIFY(!f.backend->capabilities().hasAudioPeakingFilter);
        QVERIFY(!publishedApfOn);
        QVERIFY(!editor(*f.widget, "apf.enabled")->isEnabled());
    }

    void unchangedNestedMonitorReplyCannotCancelDependentReadPlan()
    {
        Fixture f; f.ready(); f.controls(); f.observe("MO21");
        bool nested = false;
        QObject::connect(f.backend, &IRadioBackend::sliceChanged, &f.model,
            [&](int, const SliceDelta& delta) {
                if (delta.receiveFilterContextChanged && !nested) {
                    nested = true;
                    f.observe("MO20");
                }
            });
        f.commands.clear();
        f.observe("MO20");
        QVERIFY(nested);
        QVERIFY(f.commands.contains("SL0;"));
        QVERIFY(f.commands.contains("SH0;"));
        QVERIFY(f.commands.contains("AP0;"));
        QVERIFY(!f.model.slice(0)->receiveObservation().filterLowHz.has_value());
        f.observe("SL010"); f.observe("SH0080"); f.observe("AP01");
        QVERIFY(f.model.slice(0)->receiveObservation().filterLowHz.has_value());
        QVERIFY(f.backend->capabilities().hasAudioPeakingFilter);
    }

    void initialAndMissingReadbacksRemainVisibleUnavailable()
    {
        Fixture f;
        for (const char* id : kIds) {
            QComboBox* combo = editor(*f.widget, id);
            QVERIFY(combo);
            QVERIFY(!combo->isHidden());
            QVERIFY(!combo->isEnabled());
            QVERIFY(!combo->accessibleName().isEmpty());
            QVERIFY(!combo->accessibleDescription().isEmpty());
        }
        f.ready();
        QComboBox* apf = editor(*f.widget, "apf.enabled");
        QVERIFY(!apf->isEnabled());
        QVERIFY(!apf->currentData().isValid());
        QVERIFY(!apf->accessibleDescription().isEmpty());
        f.observe("AP01");
        const QByteArray diagnostic = QJsonDocument::fromVariant(f.latestStatus).toJson(QJsonDocument::Compact)
            + " widget=" + f.widget->findChild<QLabel*>("nativeReceiveStatus")->text().toUtf8()
            + " reason=" + apf->accessibleDescription().toUtf8();
        QVERIFY2(apf->isEnabled(), diagnostic.constData());
        QCOMPARE(apf->currentData().toInt(), 2);
        QLabel* context = f.widget->findChild<QLabel*>("nativeReceiveContext");
        QVERIFY(context && context->text().contains("CW"));
    }

    void everyNativeControlUsesWidgetModelWireAndReadback()
    {
        struct Case { const char* id; int before; int after; const char* command; const char* query; char mode; };
        const std::array<Case, 18> cases{{
            {"apf.enabled", 2, 1, "AP00;", "AP0;", '3'},
            {"apf.shift", 40, 41, "AP141;", "AP1;", '3'},
            {"apf.bandwidth", 0, 1, "AP21;", "AP2;", '3'},
            {"apf.gain", 3, 4, "AP34;", "AP3;", '3'},
            {"notch.enabled", 1, 0, "NT0;", "NT;", '3'},
            {"notch.width", 1, 2, "NW2;", "NW;", '3'},
            {"notch.position", 128, 129, "BP129;", "BP;", '3'},
            {"nr.mode", 1, 2, "NR2;", "NR;", '3'},
            {"nr1.level", 5, 6, "RL106;", "RL1;", '3'},
            {"nr2.time", 4, 5, "RL205;", "RL2;", '3'},
            {"nb1.enabled", 1, 0, "NB10;", "NB1;", '3'},
            {"nb1.level", 10, 11, "NL1011;", "NL1;", '3'},
            {"nb2.enabled", 1, 0, "NB20;", "NB2;", '3'},
            {"nb2.level", 6, 7, "NL2007;", "NL2;", '3'},
            {"nb2.type", 1, 0, "NBT0;", "NBT;", '3'},
            {"nb2.depth", 10, 11, "NBD011;", "NBD;", '3'},
            {"nb2.width", 10, 11, "NBW011;", "NBW;", '3'},
            {"bc.mode", 1, 2, "BC2;", "BC;", '2'},
        }};
        for (const Case& test : cases) {
            Fixture f;
            f.ready(test.mode);
            f.controls();
            if (QString::fromLatin1(test.id) == QLatin1String("nr2.time")) { f.observe("NR2"); f.observe("RL204"); }
            QComboBox* combo = editor(*f.widget, test.id);
            QVERIFY2(combo && combo->isEnabled(), test.id);
            QCOMPARE(combo->currentData().toInt(), test.before);
            f.commands.clear();
            QVERIFY2(activate(*combo, test.after), test.id);
            QCOMPARE(f.commands, (QList<QByteArray>{test.command, test.query}));
            QCOMPARE(combo->currentData().toInt(), test.before);
            QVERIFY(!combo->isEnabled());
            QVERIFY(!combo->accessibleDescription().isEmpty());
            QByteArray echo(test.command);
            echo.chop(1);
            f.observe(echo);
            QCOMPARE(combo->currentData().toInt(), test.after);
            QVERIFY(combo->isEnabled());
        }
    }

    void convertedUnitsMatchOfficialWireOrdinals()
    {
        Fixture f;
        f.ready(); f.controls();
        QComboBox* shift = editor(*f.widget, "apf.shift");
        QCOMPARE(shift->itemText(shift->findData(0)), QStringLiteral("-200 Hz"));
        QCOMPARE(shift->itemText(shift->findData(40)), QStringLiteral("0 Hz"));
        QCOMPARE(shift->itemText(shift->findData(80)), QStringLiteral("200 Hz"));
        QComboBox* width = editor(*f.widget, "apf.bandwidth");
        QVERIFY(width->itemText(width->findData(0)).contains("80 Hz"));
        QVERIFY(width->itemText(width->findData(2)).contains("320 Hz"));
        f.observe("NR2");
        f.observe("RL204");
        QComboBox* time = editor(*f.widget, "nr2.time");
        QCOMPARE(time->itemText(time->findData(0)), QStringLiteral("2 ms"));
        QCOMPARE(time->itemText(time->findData(9)), QStringLiteral("20 ms"));
    }

    void resetUsesDocumentedSentinelAndWaitsForActualReadback()
    {
        Fixture f;
        f.ready(); f.controls();
        QSet<QString> supported;
        const QSet<QString> expected{QStringLiteral("apf.shift"), QStringLiteral("apf.bandwidth"),
            QStringLiteral("apf.gain"), QStringLiteral("nr1.level"), QStringLiteral("nr2.time"),
            QStringLiteral("nb1.level"), QStringLiteral("nb2.level"), QStringLiteral("nb2.depth"),
            QStringLiteral("nb2.width")};
        for (const char* id : kIds) {
            const QString key = QString::fromLatin1(id);
            QPushButton* reset = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveReset_") + key);
            QVERIFY(reset && !reset->isHidden());
            QVERIFY(!reset->accessibleName().isEmpty());
            if (reset->isEnabled()) { supported.insert(key); }
            if (!expected.contains(key)) {
                QVERIFY(!reset->isEnabled());
                QVERIFY(!reset->accessibleDescription().isEmpty());
            }
        }
        f.observe("NR2");
        f.observe("RL204");
        QPushButton* nr2 = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveReset_nr2.time"));
        QVERIFY(nr2 && nr2->isEnabled());
        supported.insert(QStringLiteral("nr2.time"));
        QCOMPARE(supported, expected);
        QComboBox* shift = editor(*f.widget, "apf.shift");
        QPushButton* reset = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveReset_apf.shift"));
        QCOMPARE(shift->currentData().toInt(), 40);
        f.commands.clear();
        QTest::mouseClick(reset, Qt::LeftButton);
        QCOMPARE(f.commands, (QList<QByteArray>{"AP199;", "AP1;"}));
        QCOMPARE(shift->currentData().toInt(), 40);
        QVERIFY(!shift->isEnabled() && !reset->isEnabled());
        f.observe("AP139");
        QCOMPARE(shift->currentData().toInt(), 39);
        QVERIFY(shift->isEnabled() && reset->isEnabled());
    }

    void everyDocumentedResetUsesWidgetModelWireAndReadback()
    {
        struct Case { const char* id; int before; const char* reset; const char* query; int after; const char* echo; };
        // PC Command Reference reset sentinels are distinct operations, not
        // ordinary selectable values and not assumptions about default values.
        const std::array<Case, 9> cases{{
            {"apf.shift", 40, "AP199;", "AP1;", 39, "AP139"},
            {"apf.bandwidth", 0, "AP29;", "AP2;", 2, "AP22"},
            {"apf.gain", 3, "AP39;", "AP3;", 2, "AP32"},
            {"nr1.level", 5, "RL199;", "RL1;", 4, "RL104"},
            {"nr2.time", 4, "RL299;", "RL2;", 3, "RL203"},
            {"nb1.level", 10, "NL1999;", "NL1;", 9, "NL1009"},
            {"nb2.level", 6, "NL2999;", "NL2;", 5, "NL2005"},
            {"nb2.depth", 10, "NBD999;", "NBD;", 9, "NBD009"},
            {"nb2.width", 10, "NBW999;", "NBW;", 9, "NBW009"},
        }};
        for (const Case& test : cases) {
            Fixture f;
            f.ready(); f.controls();
            if (QString::fromLatin1(test.id) == QLatin1String("nr2.time")) { f.observe("NR2"); f.observe("RL204"); }
            QComboBox* combo = editor(*f.widget, test.id);
            QPushButton* reset = f.widget->findChild<QPushButton*>(
                QStringLiteral("nativeReceiveReset_") + QString::fromLatin1(test.id));
            QVERIFY2(combo && combo->isEnabled() && reset && reset->isEnabled(), test.id);
            QCOMPARE(combo->currentData().toInt(), test.before);
            f.commands.clear();
            QTest::mouseClick(reset, Qt::LeftButton);
            QCOMPARE(f.commands, (QList<QByteArray>{test.reset, test.query}));
            QCOMPARE(combo->currentData().toInt(), test.before);
            QVERIFY(!combo->isEnabled() && !reset->isEnabled());
            QVERIFY(!reset->accessibleDescription().isEmpty());
            f.observe(test.echo);
            QCOMPARE(combo->currentData().toInt(), test.after);
            QVERIFY(combo->isEnabled() && reset->isEnabled());
        }
    }

    void pendingContextWithdrawalCannotRestoreOldObservations()
    {
        Fixture f;
        f.ready(); f.controls();
        QComboBox* nb1 = editor(*f.widget, "nb1.enabled");
        QVERIFY(nb1 && nb1->isEnabled());
        QCOMPARE(nb1->currentData().toInt(), 1);
        bool withdrewContext = false;
        f.onStatus = [&f, &withdrewContext](const QVariantMap& state) {
            if (withdrewContext) { return; }
            for (const QVariant& entry : state.value("controls").toList()) {
                const QVariantMap row = entry.toMap();
                if (row.value("id") == QLatin1String("nb1.enabled")
                    && row.value("pending").toBool()) {
                    withdrewContext = true;
                    f.observe("TS1");
                    return;
                }
            }
        };
        f.commands.clear();
        QVERIFY(activate(*nb1, 0));
        QVERIFY(withdrewContext);
        QVERIFY(!f.commands.contains("NB10;"));
        for (const char* id : kIds) {
            QComboBox* combo = editor(*f.widget, id);
            QVERIFY(combo);
            QCOMPARE(combo->currentIndex(), -1);
            QVERIFY(!combo->isEnabled());
            QVERIFY(!combo->accessibleDescription().isEmpty());
        }
    }

    void newerPushedReadbackWinsOlderGetResult()
    {
        Fixture f;
        f.ready(); f.controls();
        QComboBox* nb1 = editor(*f.widget, "nb1.enabled");
        QPushButton* refresh = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveRefresh"));
        QVERIFY(nb1 && nb1->isEnabled());
        QVERIFY(refresh && refresh->isEnabled());
        QCOMPARE(nb1->currentData().toInt(), 1);
        bool injectedReadback = false;
        f.onResult = [&f, &injectedReadback](const QVariant& result) {
            if (!injectedReadback && result.toMap().contains("controls")) {
                injectedReadback = true;
                f.observe("NB10");
            }
        };
        f.commands.clear();
        QTest::mouseClick(refresh, Qt::LeftButton);
        QVERIFY(injectedReadback);
        QVERIFY(!f.commands.contains("NB10;"));
        QCOMPARE(nb1->currentData().toInt(), 0);
        QVERIFY(nb1->isEnabled());
    }

    void modeChangeDisconnectAndInvalidActivationDoNotSend()
    {
        Fixture f;
        f.ready(); f.controls();
        QComboBox* apf = editor(*f.widget, "apf.enabled");
        QVERIFY(apf->isEnabled());
        f.observe("OM04"); f.observe("SF0000140740004");
        QVERIFY(!apf->isEnabled());
        QVERIFY(!apf->isHidden());
        QVERIFY(!apf->accessibleDescription().isEmpty());
        QComboBox* nr = editor(*f.widget, "nr.mode");
        f.observe("NR1");
        QCOMPARE(nr->findData(2), -1);
        f.commands.clear();
        QVERIFY(QMetaObject::invokeMethod(apf, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
        QVERIFY(QMetaObject::invokeMethod(nr, "activated", Qt::DirectConnection, Q_ARG(int, -1)));
        QVERIFY(f.commands.isEmpty());
        f.model.disconnectFromRadio();
        // setBackendForTest bypasses the production connection-session hook.
        // Supply that model notification after the real disconnect intent.
        emit f.model.connectionStateChanged(false);
        f.commands.clear();
        for (const char* id : kIds) {
            QComboBox* combo = editor(*f.widget, id);
            QVERIFY(!combo->isEnabled());
            QVERIFY(!combo->isHidden());
            QVERIFY(!combo->accessibleDescription().isEmpty());
            QVERIFY(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
        }
        QVERIFY(f.commands.isEmpty());
    }

    void readSettingsRetriesQueriesWithoutReplayingRejectedMutation()
    {
        Fixture f;
        f.ready(); f.controls();
        QComboBox* shift = editor(*f.widget, "apf.shift");
        QPushButton* refresh = f.widget->findChild<QPushButton*>(QStringLiteral("nativeReceiveRefresh"));
        QVERIFY(refresh && refresh->isEnabled());
        f.commands.clear();
        f.acceptWrites = false;
        QVERIFY(activate(*shift, 41));
        QCOMPARE(f.commands, (QList<QByteArray>{"AP141;"}));
        QCOMPARE(shift->currentData().toInt(), 40);
        QVERIFY(!shift->isEnabled());
        QVERIFY(!f.widget->findChild<QLabel*>("nativeReceiveStatus")->text().isEmpty());
        f.acceptWrites = true;
        f.commands.clear();
        QTest::mouseClick(refresh, Qt::LeftButton);
        const QSet<QByteArray> allowedQueries{"AP0;", "AP1;", "AP2;", "AP3;", "NT;", "NW;", "BP;",
            "NR;", "RL1;", "RL2;", "NB1;", "NL1;", "NB2;", "NL2;", "NBT;", "NBD;", "NBW;", "MO1;", "MO2;"};
        QVERIFY(f.commands.contains("AP1;"));
        for (const QByteArray& command : f.commands) {
            QVERIFY2(allowedQueries.contains(command), command.constData());
        }
        QCOMPARE(shift->currentData().toInt(), 40);
        QVERIFY(!shift->isEnabled());
        f.observe("AP140");
        QCOMPARE(shift->currentData().toInt(), 40);
        QVERIFY(shift->isEnabled());
    }

    void malformedLayoutsFailClosedAndRecover()
    {
        RadioModel model;
        auto owned = std::make_unique<DescriptorBackend>();
        DescriptorBackend* source = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("test"));
        NativeReceiveSettingsWidget widget(model);
        QComboBox* combo = editor(widget, "nb1.enabled");
        QVERIFY(combo && combo->isEnabled());
        QList<QVariantMap> malformed;
        QVariantMap bad = validState(); bad["context"] = QString(129, u'x'); malformed.append(bad);
        bad = validState(); bad["context"] = 123; malformed.append(bad);
        bad = validState(); bad["mode"] = QString(129, u'x'); malformed.append(bad);
        bad = validState(); bad["mode"] = 123; malformed.append(bad);
        bad = validState(); bad["editable"] = QStringLiteral("true"); malformed.append(bad);
        for (const QVariant& invalid : {QVariant(0), QVariant(-1), QVariant(0.5),
                QVariant(std::numeric_limits<double>::infinity()), QVariant(QStringLiteral("1"))}) {
            bad = validState();
            QVariantMap row = bad["controls"].toList().first().toMap(); row["step"] = invalid;
            bad["controls"] = QVariantList{row}; malformed.append(bad);
        }
        bad = validState(); bad["controls"] = QVariantList{bad["controls"].toList().first(), bad["controls"].toList().first()}; malformed.append(bad);
        bad = validState();
        QVariantMap unknown = bad["controls"].toList().first().toMap(); unknown["id"] = "unsupported";
        bad["controls"] = QVariantList{unknown}; malformed.append(bad);
        bad = validState();
        QVariantMap outOfRange = bad["controls"].toList().first().toMap(); outOfRange["value"] = 2;
        bad["controls"] = QVariantList{outOfRange}; malformed.append(bad);
        for (const QString& field : {QStringLiteral("id"), QStringLiteral("label"),
                QStringLiteral("unit"), QStringLiteral("reason"), QStringLiteral("pending"), QStringLiteral("resettable")}) {
            bad = validState();
            QVariantMap row = bad["controls"].toList().first().toMap(); row[field] = 1;
            bad["controls"] = QVariantList{row}; malformed.append(bad);
        }
        for (const QString& field : {QStringLiteral("value"), QStringLiteral("pending")}) {
            bad = validState();
            QVariantMap row = bad["controls"].toList().first().toMap();
            if (field == QLatin1String("value")) { row.remove(field); } else { row[field] = true; }
            bad["controls"] = QVariantList{row}; malformed.append(bad);
        }
        for (const QVariantMap& state : malformed) {
            source->publish(state);
            QVERIFY(!combo->isEnabled());
            QVERIFY(!combo->accessibleDescription().isEmpty());
            QVERIFY(widget.findChild<QLabel*>("nativeReceiveStatus")->text().contains("invalid"));
            const int writes = source->writes;
            QVERIFY(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
            QCOMPARE(source->writes, writes);
            source->publish(validState());
            QVERIFY(combo->isEnabled());
            QCOMPARE(combo->currentData().toInt(), 0);
        }
    }

    void olderPushCannotReplaceNewerObservationAndNewBindingResetsFloor()
    {
        RadioModel model;
        auto owned = std::make_unique<DescriptorBackend>();
        DescriptorBackend* source = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("first"));
        NativeReceiveSettingsWidget widget(model);
        QComboBox* combo = editor(widget, "nb1.enabled");
        QVariantMap newer = validState(1); newer["statusRevision"] = "20";
        source->publish(newer);
        QCOMPARE(combo->currentData().toInt(), 1);
        QVariantMap older = validState(0); older["statusRevision"] = "19";
        source->publish(older);
        QCOMPARE(combo->currentData().toInt(), 1);
        auto replacement = std::make_unique<DescriptorBackend>();
        DescriptorBackend* current = replacement.get();
        model.setBackendForTest(std::move(replacement), QStringLiteral("second"));
        emit model.capabilitiesChanged(model.isConnected(), current->capabilities());
        QVERIFY(combo->isEnabled());
        QCOMPARE(combo->currentData().toInt(), 0);
    }

    void exactIntegralNumericDescriptorsRemainSupported()
    {
        RadioModel model;
        auto owned = std::make_unique<DescriptorBackend>();
        DescriptorBackend* source = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("test"));
        NativeReceiveSettingsWidget widget(model);
        QVariantMap state = validState(1);
        QVariantMap row = state["controls"].toList().first().toMap();
        row["minimum"] = 0.0; row["maximum"] = 1.0; row["step"] = 1.0; row["value"] = 1.0;
        state["controls"] = QVariantList{row};
        source->publish(state);
        QComboBox* combo = editor(widget, "nb1.enabled");
        QVERIFY(combo->isEnabled());
        QCOMPARE(combo->currentData().toInt(), 1);
    }

    void malformedHighRevisionCannotPoisonReadRecovery()
    {
        RadioModel model;
        auto owned = std::make_unique<DescriptorBackend>();
        DescriptorBackend* source = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("test"));
        NativeReceiveSettingsWidget widget(model);
        QComboBox* combo = editor(widget, "nb1.enabled");
        QPushButton* refresh = widget.findChild<QPushButton*>("nativeReceiveRefresh");
        QVERIFY(combo && refresh);
        const QVariantList invalidRevisions{QVariant("1000000"), QVariant("18446744073709551615")};
        for (const QVariant& revision : invalidRevisions) {
            QVariantMap bad = validState(); bad["statusRevision"] = revision;
            QVariantMap row = bad["controls"].toList().first().toMap();
            row["value"] = true;
            bad["controls"] = QVariantList{row};
            source->publish(bad);
            QCOMPARE(combo->currentIndex(), -1);
            QVERIFY(!combo->isEnabled());
            QTest::mouseClick(refresh, Qt::LeftButton);
            QVERIFY(combo->isEnabled());
            QCOMPARE(combo->currentData().toInt(), 0);
            QCOMPARE(source->writes, 0);
        }
        for (const QVariant& revision : {QVariant(1), QVariant("01"), QVariant("+1"),
                QVariant("18446744073709551616"), QVariant("-1")}) {
            QVariantMap bad = validState(); bad["statusRevision"] = revision;
            source->publish(bad);
            QVERIFY(!combo->isEnabled());
            QTest::mouseClick(refresh, Qt::LeftButton);
            QVERIFY(combo->isEnabled());
        }
    }

    void replacementCannotRetainOldBackendAuthority()
    {
        RadioModel model;
        auto first = std::make_unique<DescriptorBackend>();
        first->publishOnDestruction = true;
        DescriptorBackend* previous = first.get();
        QPointer<DescriptorBackend> retired(previous);
        model.setBackendForTest(std::move(first), QStringLiteral("first"));
        NativeReceiveSettingsWidget widget(model);
        QComboBox* combo = editor(widget, "nb1.enabled");
        QVERIFY(combo->isEnabled());
        auto second = std::make_unique<DescriptorBackend>();
        second->advertised = false;
        DescriptorBackend* current = second.get();
        model.setBackendForTest(std::move(second), QStringLiteral("second"));
        emit model.capabilitiesChanged(model.isConnected(), current->capabilities());
        QVERIFY(retired.isNull());
        QVERIFY(!combo->isEnabled());
        QVERIFY(!combo->currentData().isValid());
        current->publish(validState(1));
        QVERIFY(!combo->isEnabled());
        QVERIFY(!combo->currentData().isValid());
        QVERIFY(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, 0)));
        QCOMPARE(current->writes, 0);
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("native-receive-settings"));
    if (!profile.isValid()) { return 1; }
    QApplication app(argc, argv);
    AppSettings::instance().load();
    NativeReceiveSettingsTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "native_receive_settings_test.moc"
