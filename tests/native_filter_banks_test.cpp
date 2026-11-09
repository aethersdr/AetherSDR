#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeFilterSettingsWidget.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QApplication>
#include <QAccessible>
#include <QScopeGuard>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStandardItemModel>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& b) {
        b.m_clock.start(); b.m_protocol.start("fixture-user", "fixture-password", false, 0);
        b.m_protocol.receive("##CN1;", 1); b.m_protocol.receive("##ID1;", 2); b.ready();
    }
    static QVariantMap fmStatus(const KenwoodBackend& b) { return b.fmWidthStatus(); }
    static void message(KenwoodBackend& b, const QByteArray& bytes) { b.adopt(bytes); }
};
}
namespace {
using namespace AetherSDR;
std::function<void(QAccessibleEvent*)> accessibilityCallback;
void accessibleUpdate(QAccessibleEvent* event) { if (accessibilityCallback) { accessibilityCallback(event); } }
void require(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
struct Fixture {
    QList<QByteArray> commands;
    QVariantMap lastBankStatus;
    std::function<void(const QString&, const QVariantMap&)> onStatus;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    std::unique_ptr<NativeFilterSettingsWidget> widget;
    Fixture() {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        backend = owned.get();
        QObject::connect(backend, &IRadioBackend::extensionStatus, &model,
            [this](const QString&, const QString& kind, const QVariantMap& state) {
                if (kind == "filterBanks") { lastBankStatus = state; }
                if (onStatus) { onStatus(kind, state); }
            });
        model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        widget = std::make_unique<NativeFilterSettingsWidget>(model);
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::message(*backend, message); }
    void ready() {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT1"), QByteArray("FA00014074000"),
            QByteArray("FB00007074000"), QByteArray("OM03"), QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX"), QByteArray("MO20")}) { observe(message); }
        // setBackendForTest omits setupBackend's actual connection lifecycle relay.
        emit model.connectionStateChanged(true);
    }
    void readbacks(bool fm = false) {
        observe("EX00610 003"); observe("FL001");
        for (int bank = 0; bank < 3; ++bank) {
            observe("FL1" + QByteArray::number(bank) + (fm ? "51500" : "00050"));
            observe("FL2" + QByteArray::number(bank) + (fm ? "3" : "1"));
            observe("FL3" + QByteArray::number(bank) + "1");
        }
        observe("SL010"); observe("SH0005");
    }
    QComboBox* combo(const QString& id) const {
        auto* result = widget->findChild<QComboBox*>(QStringLiteral("nativeFilterBank_") + id);
        require(result != nullptr, "native bank control exists"); return result;
    }
    QPushButton* reset(const QString& id) const { return widget->findChild<QPushButton*>(QStringLiteral("nativeFilterBankReset_") + id); }
    void choose(const QString& id, int value) {
        QComboBox* box = combo(id); const int index = box->findData(value);
        if (index < 0) { std::cerr << "missing choice id=" << id.toStdString() << " value=" << value
            << " count=" << box->count() << " enabled=" << box->isEnabled()
            << " reason=" << box->accessibleDescription().toStdString() << '\n'; }
        require(index >= 0, "native choice remains visible");
        box->setCurrentIndex(index); QMetaObject::invokeMethod(box, "activated", Qt::DirectConnection, Q_ARG(int, index));
    }
};
void pendingBankStatusCannotOutliveTfSetWithdrawal() {
    Fixture f; f.ready(); f.readbacks(); bool nested = false;
    f.onStatus = [&](const QString& kind, const QVariantMap& state) {
        if (kind != "filterBanks" || nested) { return; }
        for (const QVariant& value : state.value("controls").toList()) {
            const QVariantMap row = value.toMap();
            if (row.value("id") == "bank0.afType" && row.value("pending").toBool()) {
                nested = true; f.observe("TS1"); break;
            }
        }
    };
    f.commands.clear(); f.choose("bank0.afType", 0);
    require(nested, "bank ordering probe actually withdraws context from earlier pending-status listener");
    require(!f.commands.contains("FL300;"), "retired bank request cannot dispatch a setter");
    require(f.combo("active")->currentIndex() == -1 && f.combo("bank0.afType")->currentIndex() == -1,
        "new TF-SET context leaves bank observations unknown despite older outer pending push");
}
void bankRevisionValidationAndReadRecovery() {
    Fixture f; f.ready(); f.readbacks(); const QVariantMap old = f.lastBankStatus;
    f.observe("TS1"); emit f.backend->extensionStatus("kenwood", "filterBanks", old);
    require(f.combo("active")->currentIndex() == -1, "older bank push cannot revive TF-SET-retired values");
    f.observe("TS0"); f.readbacks();
    for (const QString& high : QStringList{"1000000", "18446744073709551615"}) {
        QVariantMap bad = f.lastBankStatus; bad["statusRevision"] = high; bad["controls"] = QVariantList{};
        emit f.backend->extensionStatus("kenwood", "filterBanks", bad);
        require(f.combo("active")->currentIndex() == -1, "bad bank body dims without accepting high ordering floor");
        f.commands.clear(); f.widget->findChild<QPushButton*>("nativeFilterBankRefresh")->click(); f.readbacks();
        require(f.commands.contains("FL0;") && f.combo("active")->isEnabled(), "actual bank Read and current lower producer readback recover poisoned-body attempt");
    }
    for (const QVariant& revision : QList<QVariant>{QVariant(1), QVariant(true), QVariant("01"), QVariant("-1"), QVariant("18446744073709551616")}) {
        QVariantMap bad = f.lastBankStatus; bad["statusRevision"] = revision; emit f.backend->extensionStatus("kenwood", "filterBanks", bad);
        require(f.combo("active")->currentIndex() == -1, "malformed bank revision cannot authorize values");
        f.widget->findChild<QPushButton*>("nativeFilterBankRefresh")->click(); f.readbacks(); require(f.combo("active")->isEnabled(), "bank Read recovers malformed ordering metadata");
    }
}
void bankAccessibilityNestedWithdrawal() {
    Fixture f; f.ready(); f.readbacks(); const bool active = QAccessible::isActive();
    const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
    const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
    QAccessible::setActive(true);
    if (!QAccessible::isActive()) { std::cerr << "SKIP bank a11y callback: inactive platform backend\n"; return; }
    bool nested = false;
    accessibilityCallback = [&](QAccessibleEvent* event) {
        if (!nested && event->type() == QAccessible::ValueChanged && event->object() == f.combo("active")) { nested = true; f.observe("TS1"); }
    };
    f.observe("FL000"); require(nested && f.combo("active")->currentIndex() == -1 && f.combo("bank0.afType")->currentIndex() == -1,
        "bank a11y nested context withdrawal prevents stale outer continuation");
}
void everyChoiceAndReset() {
    for (int bank = 0; bank < 3; ++bank) {
        for (int field = 1; field <= 3; ++field) {
            const QString name = field == 1 ? "roofing" : field == 2 ? "ifShape" : "afType";
            const QString id = QStringLiteral("bank%1.%2").arg(bank).arg(name);
            const QByteArray query = "FL" + QByteArray::number(field) + QByteArray::number(bank);
            for (int value = 0; value < (field == 1 ? 6 : 3); ++value) {
                Fixture f; require(!f.combo(id)->isEnabled() && !f.combo(id)->accessibleDescription().isEmpty(), "cold bank control is visible with unavailable reason");
                f.ready(); f.readbacks();
                require(f.combo(id)->isEnabled(), "confirmed native field enables actual UI editor");
                const int old = f.combo(id)->currentData().toInt(); f.commands.clear(); f.choose(id, value);
                require(f.commands == QList<QByteArray>{query + QByteArray::number(value) + ';', query + ';'}, "actual widget/model/backend route emits exact native choice and query");
                require(!f.combo(id)->isEnabled() && f.combo(id)->currentData().toInt() == old, "selection restores confirmed old value and disables while pending");
                f.observe(query + QByteArray::number(value) + (field == 1 ? "0050" : ""));
                require(f.combo(id)->isEnabled() && f.combo(id)->currentData().toInt() == value, "actual radio readback updates native selection");
                if (field == 1) {
                    const QString label = f.widget->findChild<QLabel*>(QStringLiteral("nativeFilterBankBandwidth%1").arg(bank))->text();
                    require(label.contains("500"), "actual roofing bandwidth is shown separately from selected Auto/enum");
                }
            }
            Fixture f; f.ready(); f.readbacks(); f.commands.clear();
            QPushButton* reset = f.reset(id); require(reset && reset->isEnabled(), "native reset remains visible and available for current field");
            const int old = f.combo(id)->currentData().toInt(); reset->click();
            require(f.commands == QList<QByteArray>{query + "9;", query + ';'} && f.combo(id)->currentData().toInt() == old, "all nine actual UI reset routes preserve confirmed value");
            f.observe(query + '2' + (field == 1 ? "0050" : ""));
            require(f.combo(id)->currentData().toInt() == 2, "UI reset adopts actual native readback rather than guessed default");
        }
        Fixture f; f.ready(); f.readbacks(); f.commands.clear(); f.choose("active", bank);
        require(f.commands == QList<QByteArray>{"FL0" + QByteArray::number(bank) + ';', "FL0;"}, "active A/B/C selection uses actual UI route");
        require(f.combo("active")->currentData().toInt() == 0, "active selection retains confirmed bank before report");
        f.observe("FL0" + QByteArray::number(bank) + '1');
        f.observe("SL010"); f.observe("SH0005");
        require(f.combo("active")->currentData().toInt() == bank && f.combo("active")->isEnabled(),
            "all three active bank choices converge through actual radio readback and fresh axes");
    }
}
void explicitReadQueriesEveryBank() {
    Fixture f; f.ready(); f.readbacks(); f.commands.clear();
    QPushButton* read = f.widget->findChild<QPushButton*>(QStringLiteral("nativeFilterBankRefresh"));
    require(read && read->isEnabled(), "actual native bank Read settings action is available");
    read->click();
    QList<QByteArray> expected{"EX00610;", "FL0;"};
    for (int bank = 0; bank < 3; ++bank) {
        for (int field = 1; field <= 3; ++field) {
            expected.append("FL" + QByteArray::number(field) + QByteArray::number(bank) + ';');
        }
    }
    require(f.commands == expected, "explicit UI Read settings queries all nine known bank fields and active/count state without setters");
}
void everyNativeModeRecovers() {
    struct Mode { char code; bool styles; bool shared; bool widthOnly; };
    const QList<Mode> modes{{'1', true, true, false}, {'2', true, true, false}, {'3', false, true, false},
        {'4', false, false, false}, {'5', false, false, false}, {'6', false, false, true}, {'7', false, true, false},
        {'9', false, false, true}, {'A', false, false, true}, {'B', false, false, true}, {'C', true, true, false},
        {'D', true, true, false}, {'E', false, false, false}, {'F', false, false, false}};
    for (const Mode& mode : modes) {
        for (int style = 0; style < (mode.styles ? 2 : 1); ++style) {
            Fixture f; f.ready(); f.readbacks();
            f.observe(QByteArray("OM0") + mode.code); f.observe(QByteArray("SF000014074000") + mode.code);
            f.observe("EX00611 " + QByteArray::number(style).rightJustified(3, '0'));
            f.observe("EX00612 " + QByteArray::number(style).rightJustified(3, '0'));
            f.commands.clear(); f.observe("FL011");
            auto* low = f.widget->findChild<QComboBox*>("nativeFilterLow");
            auto* high = f.widget->findChild<QComboBox*>("nativeFilterHigh");
            require(low && high && !low->isEnabled(), "every native mode dims axis editor after actual bank transition");
            require(f.commands.contains("SL0;") && f.commands.contains("SH0;") != mode.widthOnly,
                    "actual page bank transition queries only existing native axes for mode");
            f.observe("SL000");
            if (!mode.widthOnly) {
                require(!low->isEnabled(), "native two-axis editor waits for complete fresh pair");
                f.observe("SH0000");
            }
            require(low->isEnabled() && high->isEnabled() != mode.widthOnly,
                    "AM/FM/FSK/PSK and all other native editors recover independently of shared edges");
            SliceModel* slice = f.model.slice(0);
            require(slice && (mode.shared || (!slice->receiveObservation().filterLowHz && !slice->receiveObservation().filterHighHz)),
                    "native-only mode model does not invent signed shared edges after qualification");
            f.commands.clear(); low->setCurrentIndex(1);
            QMetaObject::invokeMethod(low, "activated", Qt::DirectConnection, Q_ARG(int, 1));
            require(f.commands == QList<QByteArray>{"SL001;", "SL0;"} && low->currentIndex() == 0,
                    "actual recovered native UI/model/backend route sends exact one-axis request without optimistic selection");
            f.observe("SL001");
            require(low->currentIndex() == 1, "native axis accepts actual changed index readback in every mode/style");
        }
    }
}
void actualPendingAxisWindow() {
    Fixture f; f.ready(); f.readbacks();
    SliceModel* slice = f.model.slice(0);
    require(slice && slice->receiveObservation().filterLowHz && slice->receiveObservation().filterHighHz,
            "production model begins with qualified current bank axes");
    f.commands.clear(); f.choose("active", 0);
    require(!f.combo("bank1.afType")->isEnabled(), "all bank editors dim while active-bank identity is pending");
    require(!slice->receiveObservation().filterLowHz && !slice->receiveObservation().filterHighHz,
            "actual widget same-bank setter retires accepted model edges before radio response");
    f.observe("SL010"); f.observe("SH0005");
    require(!slice->receiveObservation().filterLowHz && !slice->receiveObservation().filterHighHz,
            "pending-window old reports cannot requalify production model axes");
    f.observe("FL001"); f.observe("SL010"); f.observe("SH0005");
    require(slice->receiveObservation().filterLowHz && slice->receiveObservation().filterHighHz,
            "actual unchanged bank confirmation plus fresh pair requalifies model");
}
void boundariesAndLifecycle() {
    Fixture f; f.ready(); f.readbacks();
    f.commands.clear();
    QMetaObject::invokeMethod(f.combo("bank0.afType"), "activated", Qt::DirectConnection, Q_ARG(int, 999));
    QMetaObject::invokeMethod(f.combo("bank0.afType"), "activated", Qt::DirectConnection, Q_ARG(int, -1));
    require(f.commands.isEmpty(), "invalid GUI activation indices cannot coerce into choice zero");
    f.observe("FL000"); f.commands.clear();
    f.choose("bank0.roofing", 1);
    require(f.commands.isEmpty(), "programmatic activation cannot bypass disabled optional270 choice");
    f.observe("EX00610 002"); f.commands.clear(); f.choose("active", 2);
    require(f.commands.isEmpty() && !f.combo("bank2.roofing")->isEnabled(), "bank C stays visible but dim with two-bank menu");
    f.observe("OM04"); f.observe("SF0000140740004"); f.readbacks(true);
    require(!f.combo("bank0.roofing")->isEnabled() && !f.combo("bank0.ifShape")->isEnabled()
        && f.combo("bank0.afType")->isEnabled(), "FM keeps independent AF control but dims roofing and IF shape with reasons");
    f.commands.clear(); f.choose("bank0.roofing", 5); f.reset("bank0.ifShape")->click();
    require(f.commands.isEmpty(), "FM GUI cannot dispatch restricted choice or reset");
    f.observe("OM03"); f.observe("SF0000140740003"); f.readbacks();
    const QVariantMap validLayout = f.lastBankStatus;
    for (int mutation = 0; mutation < 6; ++mutation) {
        QVariantMap invalid = validLayout;
        invalid["statusRevision"] = f.lastBankStatus.value("statusRevision");
        QVariantList controls = invalid.value("controls").toList();
        QVariantMap af = controls[3].toMap();
        QVariantList choices = af.value("choices").toList();
        if (mutation == 0) { invalid.insert("context", -1); }
        if (mutation == 1) { invalid.insert("mode", QString(33, QLatin1Char('X'))); }
        if (mutation == 2) { choices.append(QVariantMap{{"value", 4}, {"label", "invalid AF"}, {"editable", true}, {"reason", ""}}); }
        if (mutation == 3) { choices[0] = choices[1]; }
        if (mutation == 4) { af.insert("value", QStringLiteral("1")); }
        if (mutation == 5) { af.insert("id", QStringLiteral("bank9.afType")); }
        af.insert("choices", choices); controls[3] = af; invalid.insert("controls", controls);
        emit f.backend->extensionStatus("kenwood", "filterBanks", invalid);
        require(!f.combo("bank0.afType")->isEnabled() && f.combo("bank0.afType")->count() == 3,
                "malformed exact-domain layout dims authority but retains visible supported choices");
        f.commands.clear(); f.choose("bank0.afType", 0);
        require(f.commands.isEmpty(), "invalid descriptor cannot dispatch a programmatically activated choice");
        f.widget->findChild<QPushButton*>("nativeFilterBankRefresh")->click(); f.readbacks();
        require(f.combo("bank0.afType")->isEnabled(), "fresh actual readbacks restore layout after invalid metadata");
    }
    QVariantMap malformed{{"context", -1}, {"mode", "CW"}, {"controls", QVariantList{}}};
    emit f.backend->extensionStatus("kenwood", "filterBanks", malformed);
    require(!f.combo("active")->isEnabled() && f.widget->findChild<QLabel*>("nativeFilterBankStatus")->text().contains("invalid"), "malformed layout clears prior editable authority");
    f.widget->findChild<QPushButton*>("nativeFilterBankRefresh")->click(); f.readbacks();
    require(f.combo("bank0.afType")->isEnabled(), "actual Read settings route recovers after malformed or missing status");
    f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
    f.commands.clear(); f.choose("bank0.afType", 2);
    require(f.commands.isEmpty() && !f.combo("bank0.afType")->isEnabled(), "disconnect retires UI authority without replay");
    emit f.backend->extensionStatus("kenwood", "filterBanks", malformed);
    require(!f.combo("active")->isEnabled(), "old disconnected source cannot re-enable controls");
}
void lifetime() {
    Fixture f; bool destroyed = false;
    QObject::connect(f.backend, &IRadioBackend::extensionResult, &f.model, [&](quint64, const QVariant& value) {
        if (!destroyed && value.typeId() == QMetaType::QVariantMap && value.toMap().contains("axes")) {
            destroyed = true; f.widget.reset();
        }
    });
    f.ready();
    require(destroyed && !f.widget && f.backend->isConnected(), "synchronous existing-filter query may delete page before bank query without stale continuation");
}
}
void fmWidthPassiveAndExplicitRead()
{
    Fixture f; f.ready();
    auto* read = f.widget->findChild<QPushButton*>("nativeFmWidthRead");
    auto* apply = f.widget->findChild<QPushButton*>("nativeFmWidthApply");
    auto* left = f.widget->findChild<QLabel*>("nativeFmWidthLeft");
    require(read && apply && left && read->isEnabled() && !apply->isEnabled(), "FM width is visible, initially unknown and explicit-read only");
    require(!f.commands.contains("FW0;") && !f.commands.contains("FW1;") && !f.commands.contains("FW00;") && !f.commands.contains("FW01;"), "page open never reads or writes FW");
    f.commands.clear(); read->click();
    require(f.commands.contains("FR;") && !f.commands.contains("FW01;"), "explicit FM read starts owned context survey without setter");
    auto* cancel = f.widget->findChild<QPushButton*>("nativeFmWidthCancel");
    require(cancel && cancel->isEnabled(), "owned FM read exposes cancel");
    f.commands.clear(); cancel->click();
    require(f.commands.isEmpty(), "FM cancel sends no inverse or native command");
}

void fmWidthAccessibilityReentry()
{
    const bool active = QAccessible::isActive();
    const QAccessible::UpdateHandler previous = QAccessible::installUpdateHandler(accessibleUpdate);
    const auto cleanup = qScopeGuard([previous, active] { accessibilityCallback = {}; QAccessible::installUpdateHandler(previous); QAccessible::setActive(active); });
    QAccessible::setActive(true);
    if (!QAccessible::isActive()) { std::cerr << "SKIP FM a11y callback: inactive platform backend\n"; return; }
    for (bool destroy : {false, true}) {
        Fixture f; f.ready(); f.observe("OM04");
        auto* left = f.widget->findChild<QLabel*>("nativeFmWidthLeft");
        bool nested = false;
        accessibilityCallback = [&](QAccessibleEvent* event) {
            if (nested || event->type() != QAccessible::NameChanged || event->object() != left) { return; }
            nested = true;
            if (destroy) { f.widget.reset(); }
            else { f.observe("FW01"); }
        };
        f.observe("FW00");
        require(nested, "FM observed width emits accessibility event on actual change");
        if (destroy) { require(!f.widget && f.backend->isConnected(), "FM accessibility may delete page without stale continuation"); }
        else { require(left->text().contains("Narrow"), "newer nested FM accessibility observation survives outer render"); }
        accessibilityCallback = {};
    }
}

void fmWidthActualApplyAndAccessibility()
{
    for (char mode : {'4', 'E'}) {
        for (int receive = 0; receive < 2; ++receive) {
            for (int value = 0; value < 2; ++value) {
                Fixture f; f.ready();
                f.observe("FR" + QByteArray::number(receive)); f.observe("FT" + QByteArray::number(1 - receive));
                f.observe("OM0" + QByteArray(1, mode)); f.observe("OM1" + QByteArray(1, mode));
                f.observe("SF000014074000" + QByteArray(1, mode)); f.observe("SF100007074000" + QByteArray(1, mode));
                auto* read = f.widget->findChild<QPushButton*>("nativeFmWidthRead");
                auto* apply = f.widget->findChild<QPushButton*>("nativeFmWidthApply");
                auto* choice = f.widget->findChild<QComboBox*>("nativeFmWidthChoice");
                read->click();
                for (const QByteArray& reply : QList<QByteArray>{"FR" + QByteArray::number(receive), "FT" + QByteArray::number(1 - receive), "TS0", "MV0", "QR00",
                    "SF000014074000" + QByteArray(1, mode), "SF100007074000" + QByteArray(1, mode), "OM0" + QByteArray(1, mode), "OM1" + QByteArray(1, mode), "FW00", "FW11"}) { f.observe(reply); }
                require(choice->isEnabled() && choice->currentIndex() == 0, "actual native Read converges through model/backend to Normal");
                choice->setCurrentIndex(value); QMetaObject::invokeMethod(choice, "activated", Qt::DirectConnection, Q_ARG(int, value));
                require(apply->isEnabled(), "explicit native width draft enables real Apply button");
                const QString qa = qEnvironmentVariable("AETHER_FM_WIDTH_QA_DIR");
                if (!qa.isEmpty() && mode == 'E' && receive == 1 && value == 1) {
                    f.widget->resize(850, 1050); f.widget->show(); QApplication::processEvents();
                    require(QDir().mkpath(qa) && f.widget->grab().save(qa + "/fm-width-ready.png"), "native FM panel capture saved");
                }
                f.commands.clear(); apply->click();
                require(f.commands.contains(value ? "FW01;" : "FW00;") && !apply->isEnabled(), "real widget/model/backend Apply sends exact native width and waits");
                f.observe("TS1");
                require(!choice->isEnabled() && !choice->accessibleDescription().isEmpty()
                    && choice->accessibleDescription() == choice->toolTip(), "unavailable FM reason reaches accessibility as well as tooltip");
            }
        }
    }
}

void fmWidthInvalidRevisionDoesNotPoisonRecovery()
{
    Fixture f; f.ready();
    QVariantMap status = kenwood::KenwoodBackendTestAccess::fmStatus(*f.backend);
    status["statusRevision"] = QStringLiteral("100"); status["revision"] = QStringLiteral("10");
    status["context"] = QStringLiteral("test-current"); status["connected"] = true; status["editable"] = true; status["reason"] = QString{};
    status["refresh"] = QVariantMap{{"token", "1"}, {"busy", false}, {"pendingKey", ""}, {"reason", "Observed"}};
    status["lastWrite"] = QVariantMap{};
    status["displays"] = QVariantList{
        QVariantMap{{"display", 0}, {"eligible", true}, {"known", true}, {"value", 0}, {"reason", "Observed"}},
        QVariantMap{{"display", 1}, {"eligible", true}, {"known", true}, {"value", 1}, {"reason", "Observed"}}};
    emit f.backend->extensionStatus("kenwood", "fmWidth", status);
    auto* choice = f.widget->findChild<QComboBox*>("nativeFmWidthChoice");
    auto* apply = f.widget->findChild<QPushButton*>("nativeFmWidthApply");
    require(choice->isEnabled() && choice->currentIndex() == 0, "strict valid FM status displays Normal");
    QVariantMap invalid = status; invalid["statusRevision"] = QStringLiteral("9999999999"); invalid["editable"] = QStringLiteral("true");
    emit f.backend->extensionStatus("kenwood", "fmWidth", invalid);
    require(!choice->isEnabled(), "malformed high-revision FM frame withdraws editability");
    status["statusRevision"] = QStringLiteral("101"); emit f.backend->extensionStatus("kenwood", "fmWidth", status);
    require(choice->isEnabled(), "malformed higher revision never poisons valid FM recovery floor");
    choice->setCurrentIndex(1); QMetaObject::invokeMethod(choice, "activated", Qt::DirectConnection, Q_ARG(int, 1));
    require(apply->isEnabled(), "explicit FM draft requires Apply");
    status["revision"] = QStringLiteral("11"); status["statusRevision"] = QStringLiteral("102"); emit f.backend->extensionStatus("kenwood", "fmWidth", status);
    require(choice->currentIndex() == 1 && !apply->isEnabled(), "native context change preserves dirty draft but withdraws stale Apply");
}


int main(int argc, char** argv) {
    TestSettingsProfile profile(QStringLiteral("native-filter-banks")); QApplication app(argc, argv);
    require(profile.isValid(), "isolated settings");
    if (app.arguments().contains("--fm-native-qa")) { fmWidthActualApplyAndAccessibility(); fmWidthAccessibilityReentry(); return 0; }
    if (app.arguments().contains("--status-order-probe=banks")) { pendingBankStatusCannotOutliveTfSetWithdrawal(); return 0; }
    fmWidthPassiveAndExplicitRead(); fmWidthInvalidRevisionDoesNotPoisonRecovery(); fmWidthActualApplyAndAccessibility(); fmWidthAccessibilityReentry();
    everyChoiceAndReset(); explicitReadQueriesEveryBank(); everyNativeModeRecovers(); actualPendingAxisWindow(); boundariesAndLifecycle(); lifetime(); pendingBankStatusCannotOutliveTfSetWithdrawal(); bankRevisionValidationAndReadRecovery(); bankAccessibilityNestedWithdrawal(); return 0;
}
