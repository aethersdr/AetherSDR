// Socket-free production widget -> RadioModel -> backend control/readback.
#include "TestSettingsProfile.h"
#include "core/backends/kenwood/KenwoodBackend.h"
#include "gui/NativeVfoSettingsWidget.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QByteArray>
#include <QList>
#include <QCheckBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QMetaType>
#include <QPair>
#include <QVariantMap>

#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

namespace AetherSDR::kenwood {
struct KenwoodBackendTestAccess {
    static void authenticate(KenwoodBackend& backend)
    {
        backend.m_clock.start();
        backend.m_protocol.start("fixture-user", "fixture-password", false, 0);
        backend.m_protocol.receive("##CN1;", 1);
        backend.m_protocol.receive("##ID1;", 2);
        backend.ready();
    }
    static void message(KenwoodBackend& backend, const QByteArray& bytes) { backend.adopt(bytes); }
};
}
namespace {
using namespace AetherSDR;
void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    QList<QByteArray> commands;
    RadioModel model;
    kenwood::KenwoodBackend* backend = nullptr;
    std::unique_ptr<NativeVfoSettingsWidget> widget;
    Fixture()
    {
        auto owned = std::make_unique<kenwood::KenwoodBackend>(kenwood::KenwoodBackend::Transport{
            [this](const QByteArray& bytes) { commands.append(bytes); return true; }, {}});
        backend = owned.get();
        model.setBackendForTest(std::move(owned), QStringLiteral("kenwood"));
        widget = std::make_unique<NativeVfoSettingsWidget>(model);
    }
    void observe(const QByteArray& message) { kenwood::KenwoodBackendTestAccess::message(*backend, message); }
    void ready()
    {
        kenwood::KenwoodBackendTestAccess::authenticate(*backend);
        for (const QByteArray& message : {QByteArray("ID024"), QByteArray("FR0"), QByteArray("FT1"),
                QByteArray("FA00014074000"), QByteArray("FB00007074000"), QByteArray("OM03"),
                QByteArray("SF0000140740003"), QByteArray("SF1000070740003"), QByteArray("TS0"), QByteArray("RX")}) { observe(message); }
        // The test seam omits production setupBackend's lifecycle relay.
        emit model.connectionStateChanged(true);
    }
    QCheckBox* field(const char* name) const
    { return widget->findChild<QCheckBox*>(QStringLiteral("nativeTuning_") + QString::fromLatin1(name)); }
    void readbacks() { observe("FS00"); observe("MH0"); observe("LK0"); }
};
void actualRoute()
{
    Fixture f;
    for (const char* field : {"receiveFine", "mhzStep", "locked"}) {
        require(f.field(field) && !f.field(field)->isEnabled(), "native tuning controls remain visible and dimmed before connection");
        require(!f.field(field)->accessibleDescription().isEmpty(), "unavailable native tuning states have a stated reason");
    }
    auto* read = f.widget->findChild<QPushButton*>(QStringLiteral("nativeTuningRead"));
    require(read && !read->isEnabled() && !read->accessibleDescription().isEmpty(),
        "explicit tuning read remains visible with disconnected reason");
    f.ready();
    f.readbacks();
    require(read->isEnabled(), "confirmed RX allows explicit tuning refresh");
    f.commands.clear(); read->click();
    require(f.commands == QList<QByteArray>{"FS;", "MH;", "LK;"},
        "actual read button routes one bounded fresh query batch through model/backend");
    f.commands.clear();
    emit f.model.capabilitiesChanged(true, f.model.backendCapabilities());
    emit f.model.capabilitiesChanged(true, f.model.backendCapabilities());
    require(f.commands.isEmpty(), "same-binding capability notifications do not recursively refresh queries");
    f.observe("FS01"); f.observe("MH1"); f.observe("LK1");
    require(!f.field("receiveFine")->isChecked() && f.field("mhzStep")->isChecked()
        && f.field("locked")->isChecked(), "same-context front-panel readbacks update real widget");
    f.readbacks();
    for (const char* field : {"receiveFine", "mhzStep", "locked"}) { require(f.field(field)->isEnabled(), "current native readback enables each tuning field"); }
    for (const QByteArray& bits : {QByteArray("00"), QByteArray("01"), QByteArray("10"), QByteArray("11")}) {
        f.observe("FS" + bits);
        require(f.field("receiveFine")->isChecked() == (bits[0] == '1'), "widget RX FINE follows only the RX bit across all four combined answers");
    }
    f.readbacks();
    const std::array<std::pair<const char*, QByteArray>, 3> fields{{
        {"receiveFine", "FS"}, {"mhzStep", "MH"}, {"locked", "LK"}}};
    for (const auto& [name, command] : fields) {
        QCheckBox* check = f.field(name);
        f.commands.clear();
        check->click();
        require(f.commands == QList<QByteArray>{command + "1;", command + ";"}, "actual clicked signal reaches model extension and exact backend setter/query");
        require(!check->isChecked() && !check->isEnabled(), "pending click restores old readback and dims only pending field");
        require(check->accessibleDescription().contains("readback"), "pending explanation is accessible");
        f.observe(command == "FS" ? QByteArray("FS10") : command + '1');
        require(check->isChecked() && check->isEnabled(), "radio readback completes actual widget state");
        f.commands.clear();
        check->click();
        require(f.commands == QList<QByteArray>{command + "0;", command + ";"},
            "actual On-to-Off toggle dispatches exact one-bit setter and query");
        require(check->isChecked() && !check->isEnabled(), "pending Off request retains confirmed On state");
        f.observe(command == "FS" ? QByteArray("FS01") : command + '0');
        require(!check->isChecked() && check->isEnabled(), "Off readback completes actual native widget toggle");
        require(!f.commands.contains("FS01;") && !f.commands.contains("FS10;") && !f.commands.contains("FS11;"),
            "combined FINE answers never become a TX FINE setter");
    }
    require(f.field("receiveFine")->toolTip().contains("STEP"), "radio FINE is explicitly distinguished from client STEP");
    require(f.field("locked")->toolTip().contains("slice lock"), "native lock is distinguished from client lock");
    const QVariantMap good{{"context", "current-fixture"}, {"editable", true}, {"reason", ""},
        {"receiveFine", false}, {"transmitFine", false}, {"mhzStep", false}, {"locked", false}};
    const QList<QPair<QString, QVariant>> malformed{
        {"context", 1}, {"context", ""}, {"editable", "true"}, {"reason", 1},
        {"receiveFine", "1"}, {"transmitFine", 1}, {"mhzStep", 1}, {"locked", "false"},
        {"pending", "receiveFine"}, {"pending", QVariantMap{{"receiveFine", 1}}},
        {"pending", QVariantMap{{"transmitFine", true}}}};
    for (const auto& [key, value] : malformed) {
        f.readbacks();
        require(f.field("receiveFine")->isEnabled(), "valid readback restores current editor authority");
        QVariantMap bad = good;
        bad.insert(key, value);
        emit f.backend->extensionStatus("kenwood", "tuning", bad);
        f.commands.clear();
        for (const char* field : {"receiveFine", "mhzStep", "locked"}) {
            require(!f.field(field)->isEnabled(), "malformed current status clears prior editable authority");
            f.field(field)->click();
        }
        require(f.commands.isEmpty(), "malformed status cannot retain a writable native editor");
    }
    f.readbacks();
    f.observe("FR1");
    for (const char* field : {"receiveFine", "mhzStep", "locked"}) { require(!f.field(field)->isEnabled(), "physical VFO switch retires native control values immediately"); }
    f.observe("SF1000070740003"); f.readbacks();
    require(f.field("receiveFine")->isEnabled() && !f.field("receiveFine")->isChecked(), "new VFO requires new observed tuning state");
    f.observe("TS1"); f.commands.clear(); f.field("locked")->click();
    require(f.commands.isEmpty() && !f.field("locked")->isEnabled(), "TF-SET dims native controls and prevents UI dispatch");
    f.backend->disconnectRadio(); emit f.model.connectionStateChanged(false);
    f.commands.clear();
    for (const char* field : {"receiveFine", "mhzStep", "locked"}) { f.field(field)->click(); require(!f.field(field)->isEnabled(), "disconnect retains visible unavailable tuning controls"); }
    require(!read->isEnabled(), "explicit read is unavailable after disconnect");
    read->click();
    require(f.commands.isEmpty(), "disconnected widget never writes native controls");
    // A stale endpoint publication cannot re-enable a disconnected widget.
    emit f.backend->extensionStatus("kenwood", "tuning", QVariantMap{{"context", "old"}, {"editable", true}, {"reason", ""}, {"receiveFine", true}, {"mhzStep", true}, {"locked", true}});
    require(!f.field("receiveFine")->isEnabled(), "old source/session status cannot revive tuning UI");
}
void deletionDuringVfoResult()
{
    Fixture f;
    bool deleted = false;
    const QPointer<IRadioBackend> backend(f.backend);
    const QPointer<RadioModel> model(&f.model);
    QObject::connect(f.backend, &IRadioBackend::extensionResult, &f.model,
        [&](quint64, const QVariant& value) {
            if (!deleted && value.typeId() == QMetaType::QVariantMap && value.toMap().contains("txActive")) {
                deleted = true;
                f.widget.reset();
            }
        });
    f.ready();
    require(deleted && !f.widget && backend && model && backend->isConnected(),
        "synchronous VFO result may destroy widget while backend/model remain alive");
}
}
int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("native-vfo-tuning"));
    require(settings.isValid(), "isolated settings profile");
    QApplication app(argc, argv);
    actualRoute();
    deletionDuringVfoResult();
    return 0;
}
