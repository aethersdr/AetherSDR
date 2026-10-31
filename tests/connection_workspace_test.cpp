#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "models/ConnectionWorkspace.h"
#include "core/backends/flex/SmartLinkClient.h"
#include "gui/ConnectionPanel.h"

#include <QApplication>
#include <QAbstractItemModel>
#include <QJsonDocument>
#include <QListWidget>
#include <QSignalSpy>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QDialog>
#include <QTest>
#include <QMenu>
#include <QAction>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QScrollArea>
#include <QToolButton>
#include <QLabel>
#include <QGroupBox>
#include <QSpinBox>
#include <cstdio>

namespace AetherSDR {
struct SmartLinkClientTestAccess {
    static void seed(SmartLinkClient& client)
    {
        client.m_authenticated = true;
        client.m_idToken = "test-id";
        client.m_refreshToken = "test-refresh";
        client.m_readBuffer = "stale radio list";
        client.m_sessionCredentials["legacy"] = "test-legacy";
    }
    static bool clean(const SmartLinkClient& client)
    {
        return !client.m_authenticated && client.m_idToken.isEmpty()
            && client.m_refreshToken.isEmpty() && client.m_readBuffer.isEmpty();
    }
    static quint64 generation(const SmartLinkClient& client) { return client.m_authGeneration; }
    static bool retained(const SmartLinkClient& client) { return client.m_sessionCredentials.value("legacy") == "test-legacy"; }
};
}

using namespace AetherSDR;
namespace {
int failures = 0;
void check(const char* message, bool passed)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed) { ++failures; }
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile sandbox("connection-workspace-test");
    QApplication app(argc, argv);
    AppSettings& settings = AppSettings::instance();
    settings.load();
    settings.setValue("LastConnectedRadioSerial", "saved-flex");
    settings.setValue("SmartLinkEmail", QString::fromLatin1(QByteArray("personal@example.test").toBase64()));
    settings.setValue("AudioInputDeviceId", QByteArray("missing-headset"));
    settings.setValue("RoutedProfilesJson", R"({"radio.example.test":{"identity":{"family":"hl2"},"bind":{"mode":"explicit","interface_id":"vpn"}}})");
    settings.save();
    QJsonObject document = ConnectionWorkspace::load();
    check("default operator has the user-facing name", document.value("operators").toArray().first().toObject().value("name").toString() == "Radio operator");
    const QJsonArray starterStations = document.value("stations").toArray();
    check("first workspace starts at Home with discoverable starter stations", starterStations.size() == 4
        && starterStations[0].toObject().value("name").toString() == "Home"
        && starterStations[1].toObject().value("name").toString() == "POTA"
        && starterStations[2].toObject().value("name").toString() == "Club"
        && starterStations[3].toObject().value("name").toString() == "RV");
    QJsonObject customized = document;
    QJsonArray retained = starterStations;
    retained.removeLast();
    customized["stations"] = retained;
    check("starter stations can be removed permanently", ConnectionWorkspace::save(customized)
        && ConnectionWorkspace::load() == customized);
    ConnectionWorkspace::save(document);
    const QJsonObject imported = document.value("stations").toArray().first().toObject();
    const QJsonArray connections = imported.value("connections").toArray();
    check("migration retains manual and last discovered radio", connections.size() == 2);
    check("migration retains per-address backend and VPN choice",
        connections.first().toObject().value("family").toString() == "hl2"
        && connections.first().toObject().value("route").toObject().value("bind").toObject().value("interface_id").toString() == "vpn");
    check("migration preserves existing single-account key reference",
        document.value("selectedAccount").toString() == "legacy"
        && ConnectionWorkspace::credentialKey("legacy") == "smartlink_refresh_token");
    check("legacy source is retained as downgrade snapshot", settings.contains("RoutedProfilesJson"));
    check("migration is idempotent", ConnectionWorkspace::load() == document);
    const QString secondAccount = ConnectionWorkspace::accountId("club@example.test");
    QJsonArray accounts = document.value("accounts").toArray();
    accounts.append(QJsonObject{{"id", secondAccount}, {"name", "Club"}, {"email", "club@example.test"}});
    document["accounts"] = accounts;
    check("separate account metadata persists", ConnectionWorkspace::save(document));
    check("credential keys cannot collide across accounts",
        ConnectionWorkspace::credentialKey(secondAccount) != ConnectionWorkspace::credentialKey("legacy"));
    check("connection credentials preserve case-sensitive usernames",
        ConnectionWorkspace::connectionCredentialId("radio.example.test", "User")
            != ConnectionWorkspace::connectionCredentialId("radio.example.test", "user"));
    const QString scopedKey = ConnectionWorkspace::credentialKey(secondAccount);
    settings.setValue(scopedKey, "fixture-secret");
    check("scoped credential names cannot become persistent settings", !settings.contains(scopedKey));
    check("refused scoped credentials stay in the session vault", settings.takeSessionCredential(scopedKey) == "fixture-secret");
    QJsonObject bad = document;
    bad["refresh_token"] = "must-not-persist";
    check("credential-bearing configuration is refused", !ConnectionWorkspace::save(bad));
    check("failed validation retains the entire prior document", ConnectionWorkspace::load() == document);
    bad = document;
    QJsonArray profiles = bad.value("operators").toArray();
    QJsonObject duplicate = profiles.first().toObject();
    duplicate["id"] = ConnectionWorkspace::newId();
    profiles.append(duplicate);
    bad["operators"] = profiles;
    check("ambiguous operator names are refused", !ConnectionWorkspace::save(bad));
    {
        QJsonObject legacyWorkspace = document;
        legacyWorkspace.remove("starterStationsCreated");
        QJsonObject custom = starterStations.first().toObject();
        custom["name"] = "Mountain shack";
        legacyWorkspace["stations"] = QJsonArray{custom};
        ConnectionWorkspace::save(legacyWorkspace);
        const QJsonObject upgraded = ConnectionWorkspace::load();
        check("starter migration keeps custom station names and membership", upgraded.value("stations").toArray().size() == 5
            && upgraded.value("stations").toArray().first().toObject().value("name").toString() == "Mountain shack"
            && upgraded.value("stations").toArray().first().toObject().value("connections") == imported.value("connections"));
        ConnectionWorkspace::save(document);
    }

    {
        ConnectionPanel panel;
        QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
        QSignalSpy audioChanges(&panel, &ConnectionPanel::operatorPreferencesRequested);
        QJsonObject next = document;
        QJsonArray stations = next.value("stations").toArray();
        const QString cabin = ConnectionWorkspace::newId();
        stations.append(QJsonObject{{"id", cabin}, {"name", "Cabin"}, {"connections", QJsonArray()}});
        next["stations"] = stations;
        check("additional station persists", ConnectionWorkspace::save(next));
    }
    {
        ConnectionPanel panel;
        QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
        QSignalSpy audioChanges(&panel, &ConnectionPanel::operatorPreferencesRequested);
        const QJsonArray stations = panel.workspaceSnapshot().value("stations").toArray();
        const QString cabin = stations.last().toObject().value("id").toString();
        check("station selection succeeds", panel.selectWorkspaceProfile("stations", cabin));
        check("browsing stations cannot connect or reroute audio", connects.isEmpty() && audioChanges.isEmpty());
        panel.selectWorkspaceProfile("stations", imported.value("id").toString());
        auto* radios = panel.findChild<QListWidget*>("connectionWorkspaceRadios");
        check("offline saved radios remain listed", radios && radios->count() == 2);
        auto* paths = panel.findChild<QComboBox*>("connectionWorkspacePath");
        if (radios && paths) {
            radios->setCurrentRow(0);
            check("choosing a discovered identity updates its access path", paths->currentData().toJsonObject().value("kind").toString() == "local");
            radios->setCurrentRow(1);
            check("returning to a manual radio cannot retain the previous path", paths->currentData().toJsonObject().value("kind").toString() == "manual");
        }
        auto* editStation = panel.findChild<QPushButton*>("connectionEditStation");
        editStation->click();
        auto* stationEditor = panel.findChild<QDialog*>("connectionProfileEditor");
        stationEditor->findChild<QLineEdit*>("connectionProfileName")->setText("My shack");
        QTest::keyClick(stationEditor->findChild<QLineEdit*>("connectionProfileName"), Qt::Key_Return);
        check("Enter saves the station profile", ConnectionWorkspace::find(panel.workspaceSnapshot().value("stations").toArray(),
            imported.value("id").toString()).value("name").toString() == "My shack");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* profilesButton = panel.findChild<QPushButton*>("connectionManageProfiles");
        profilesButton->click();
        auto* create = panel.findChild<QPushButton*>("connectionOperatorCreate");
        create->click();
        auto* editor = panel.findChild<QDialog*>("connectionProfileEditor");
        check("profile creation uses the real editor", editor != nullptr);
        if (editor) {
            editor->findChild<QLineEdit*>("connectionProfileName")->setText("Portable operator");
            QTest::keyClick(editor->findChild<QLineEdit*>("connectionProfileName"), Qt::Key_Enter, Qt::KeypadModifier);
            check("creating a profile saves and selects an independent record",
                panel.workspaceSnapshot().value("operators").toArray().size() == 2
                && ConnectionWorkspace::find(panel.workspaceSnapshot().value("operators").toArray(),
                    panel.workspaceSnapshot().value("selectedOperator").toString()).value("name").toString() == "Portable operator");
            check("creating and selecting preferences never applies them to a radio", connects.isEmpty() && audioChanges.isEmpty());
        }
        panel.setConnected(true);
        check("live radio permits browsing another station", panel.selectWorkspaceProfile("stations", cabin));
        check("browsing does not disturb live radio", connects.isEmpty() && audioChanges.isEmpty());
    }
    {
        ConnectionPanel panel;
        check("created operator survives a fresh panel", panel.workspaceSnapshot().value("operators").toArray().size() == 2);
        QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
        QSignalSpy audioChanges(&panel, &ConnectionPanel::operatorPreferencesRequested);
        const QString importedOperator = document.value("selectedOperator").toString();
        panel.selectWorkspaceProfile("operators", importedOperator);
        panel.addDemoRadio();
        // The public automation path uses the same missing-device guard.
        QString error;
        panel.automationConnectLocalSerial("DEMO-0001", &error);
        QCoreApplication::processEvents();
        check("missing saved microphone blocks connect without silent fallback", connects.isEmpty() && audioChanges.isEmpty());
    }
    {
        SmartLinkClient client;
        SmartLinkClientTestAccess::seed(client);
        const quint64 generation = SmartLinkClientTestAccess::generation(client);
        QSignalSpy lists(&client, &SmartLinkClient::radioListReceived);
        client.setAccountId(secondAccount);
        check("switching accounts clears authenticated state and partial list", SmartLinkClientTestAccess::clean(client));
        check("switching fences pending asynchronous authentication", SmartLinkClientTestAccess::generation(client) > generation);
        check("switching clears the previously published radio list", lists.count() == 1);
        check("switching preserves the other session credential", SmartLinkClientTestAccess::retained(client));
        client.setAccountLocked(true);
        client.setAccountId("legacy");
        check("dependent WAN session prevents account switching", client.accountId() == secondAccount);
        client.logout();
        check("dependent WAN session prevents sign-out", client.accountId() == secondAccount);
    }
    {
        QJsonObject namedDocument = ConnectionWorkspace::load();
        QJsonArray stations = namedDocument.value("stations").toArray();
        QJsonObject home = stations.first().toObject();
        home["connections"] = QJsonArray{
            QJsonObject{{"id", "address"}, {"kind", "manual"}, {"family", "flex"}, {"serial", "same-radio"}, {"host", "192.0.2.1"}, {"name", "192.0.2.1"}},
            QJsonObject{{"id", "named"}, {"kind", "local"}, {"family", "flex"}, {"serial", "same-radio"}, {"name", "Shack radio"}},
            QJsonObject{{"id", "other-address"}, {"kind", "manual"}, {"family", "flex"}, {"host", "192.0.2.2"}, {"name", "192.0.2.2"}}};
        stations[0] = home;
        namedDocument["stations"] = stations;
        namedDocument["selectedStation"] = home.value("id");
        ConnectionWorkspace::save(namedDocument);
        ConnectionPanel panel;
        auto* radios = panel.findChild<QListWidget*>("connectionWorkspaceRadios");
        check("saved names outrank addresses, including alternate paths to one radio", radios->count() == 2
            && radios->item(0)->text().startsWith("Shack radio\n")
            && radios->item(1)->text().startsWith("Unnamed radio\n")
            && radios->item(1)->text().contains("192.0.2.2"));
        for (Qt::Key key : {Qt::Key_Return, Qt::Key_Enter}) {
            const QJsonObject beforeRename = panel.workspaceSnapshot();
            panel.findChild<QPushButton*>("connectionRadioOptions")->click();
            panel.findChild<QAction*>("connectionRadioRename")->trigger();
            panel.findChild<QMenu*>("connectionRadioMenu")->close();
            auto* editor = panel.findChild<QDialog*>("connectionProfileEditor");
            auto* name = editor->findChild<QLineEdit*>("connectionProfileName");
            name->clear();
            QTest::keyClick(name, key);
            check("Enter keeps an invalid rename open", editor->isVisible()
                && panel.workspaceSnapshot() == beforeRename);
            name->setText("Shack radio");
            QTest::keyClick(name, key);
            check("Return and keypad Enter save connection renames", !editor->isVisible()
                && radios->item(0)->text().startsWith("Shack radio\n"));
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        panel.findChild<QPushButton*>("connectionRadioOptions")->click();
        panel.findChild<QAction*>("connectionRadioEdit")->trigger();
        panel.findChild<QMenu*>("connectionRadioMenu")->close();
        auto* connectionEditor = panel.findChild<QDialog*>("connectionProfileEditor");
        QTest::keyClick(connectionEditor->findChild<QLineEdit*>("connectionProfileName"), Qt::Key_Return);
        check("Enter saves the connection editor", !connectionEditor->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel.findChild<QPushButton*>("connectionRadioOptions")->click();
        auto* radioMenu = panel.findChild<QMenu*>("connectionRadioMenu");
        QStringList actions;
        for (QAction* action : radioMenu->actions()) { if (!action->isSeparator()) { actions.append(action->text()); } }
        check("radio context menu has four clear actions", actions == QStringList{"Edit", "Move", "Rename", "Remove"});
        panel.findChild<QAction*>("connectionRadioMove")->trigger();
        radioMenu->close();
        auto* destination = panel.findChild<QComboBox*>("connectionMoveDestination");
        destination->setCurrentText("POTA");
        QTest::keyClick(destination, Qt::Key_Return);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const QJsonObject moved = panel.workspaceSnapshot();
        const QJsonObject target = ConnectionWorkspace::find(moved.value("stations").toArray(), moved.value("selectedStation").toString());
        check("Move carries all paths to the destination location", target.value("name").toString() == "POTA"
            && target.value("connections").toArray().size() == 2
            && moved.value("stations").toArray().first().toObject().value("connections").toArray().size() == 1);
        panel.findChild<QPushButton*>("connectionRadioOptions")->click();
        radioMenu = panel.findChild<QMenu*>("connectionRadioMenu");
        panel.findChild<QAction*>("connectionRadioRemove")->trigger();
        radioMenu->close();
        check("Remove waits for explicit confirmation", panel.workspaceSnapshot() == moved);
        auto* confirmation = panel.findChild<QDialog*>("connectionRemoveConfirmation");
        confirmation->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        check("Cancel keeps all saved radio paths", panel.workspaceSnapshot() == moved);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel.findChild<QPushButton*>("connectionRadioOptions")->click();
        radioMenu = panel.findChild<QMenu*>("connectionRadioMenu");
        panel.findChild<QAction*>("connectionRadioRemove")->trigger();
        radioMenu->close();
        panel.findChild<QPushButton*>("connectionConfirmRemove")->click();
        check("confirmed Remove deletes this location's paths only", ConnectionWorkspace::find(panel.workspaceSnapshot().value("stations").toArray(),
            target.value("id").toString()).value("connections").toArray().isEmpty()
            && panel.workspaceSnapshot().value("stations").toArray().first().toObject().value("connections").toArray().size() == 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto* add = panel.findChild<QPushButton*>("connectionAddRadio");
        add->click();
        panel.show();
        QCoreApplication::processEvents();
        auto* name = panel.findChild<QLineEdit*>("connectionManualName");
        auto* host = panel.findChild<QLineEdit*>("connectionManualIp");
        auto* save = panel.findChild<QPushButton*>("connectionSaveManual");
        auto* connect = panel.findChild<QPushButton*>("connectionManualConnectButton");
        check("connection name precedes the host form", name->mapTo(&panel, QPoint()).y() < host->mapTo(&panel, QPoint()).y());
        check("Save connection shares the Connect by IP action row", save->parentWidget() == connect->parentWidget()
            && qAbs(save->mapTo(&panel, QPoint()).y() - connect->mapTo(&panel, QPoint()).y()) < 5);
        panel.findChild<QPushButton*>("connectionBackToStation")->click();
        auto* heading = panel.findChild<QLabel*>("connectionSelectedRadioHeading");
        auto* group = panel.findChild<QGroupBox*>("connectionSelectedRadioGroup");
        check("Selected radio heading is outside and above the card", group->title().isEmpty()
            && heading->mapTo(&panel, QPoint(0, heading->height())).y() <= group->mapTo(&panel, QPoint()).y());
        panel.findChild<QToolButton*>("connectionPreferencesToggle")->click();
        QTest::qWait(50);
        auto* preferences = panel.findChild<QWidget*>("connectionPreferencesBody");
        auto* scroll = panel.findChild<QScrollArea*>();
        bool allVisible = true;
        for (QCheckBox* box : preferences->findChildren<QCheckBox*>()) {
            allVisible = allVisible && scroll->viewport()->rect().contains(QRect(box->mapTo(scroll->viewport(), QPoint()), box->size()));
        }
        check("expanding preferences exposes every checkbox without manual scrolling", allVisible);
        auto* locations = panel.findChild<QListWidget*>("connectionStationList");
        const QJsonObject beforeReorder = panel.workspaceSnapshot();
        const QJsonArray originalOrder = beforeReorder.value("stations").toArray();
        check("Locations support internal drag reordering", locations->dragDropMode() == QAbstractItemView::InternalMove);
        check("location row can be moved down", locations->model()->moveRows(QModelIndex(), 0, 1, QModelIndex(), locations->count()));
        QCoreApplication::processEvents();
        QJsonArray expectedOrder = originalOrder;
        expectedOrder.removeAt(0);
        expectedOrder.append(originalOrder.first());
        check("drag order persists with selection and radio membership unchanged", panel.workspaceSnapshot().value("stations").toArray() == expectedOrder
            && panel.workspaceSnapshot().value("selectedStation") == beforeReorder.value("selectedStation")
            && locations->currentItem()->data(Qt::UserRole).toString() == beforeReorder.value("selectedStation").toString()
            && ConnectionWorkspace::load().value("stations").toArray() == expectedOrder);
        check("location row can be moved up", locations->model()->moveRows(QModelIndex(), locations->count() - 1, 1, QModelIndex(), 0));
        QCoreApplication::processEvents();
        check("dragging back restores the original order", panel.workspaceSnapshot() == beforeReorder);
    }
    {
        ConnectionPanel panel;
        const auto saveEntry = [&panel](const QString& name, Qt::Key key) {
            auto* editor = panel.findChild<QDialog*>("connectionProfileEditor");
            auto* field = editor->findChild<QLineEdit*>("connectionProfileName");
            field->setText(name);
            QTest::keyClick(field, key);
            const bool saved = !editor->isVisible();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            return saved;
        };
        panel.findChild<QPushButton*>("connectionAddStation")->click();
        check("Enter creates a location", saveEntry("Garden", Qt::Key_Return));
        panel.findChild<QPushButton*>("connectionLocationOptions")->click();
        panel.findChild<QAction*>("connectionLocationRename")->trigger();
        panel.findChild<QMenu*>("connectionLocationMenu")->close();
        check("keypad Enter renames a location", saveEntry("Garden shack", Qt::Key_Enter));
        panel.findChild<QPushButton*>("connectionManageProfiles")->click();
        panel.findChild<QPushButton*>("connectionOperatorEdit")->click();
        check("Enter edits an operator", saveEntry("Portable operator edited", Qt::Key_Return));
        panel.findChild<QPushButton*>("connectionOperatorDuplicate")->click();
        check("keypad Enter duplicates an operator", saveEntry("Guest operator", Qt::Key_Enter));
        panel.findChild<QDialog*>("connectionOperatorProfiles")->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel.findChild<QPushButton*>("connectionManageAccounts")->click();
        panel.findChild<QPushButton*>("connectionAccountAdd")->click();
        auto* accountEditor = panel.findChild<QDialog*>("connectionProfileEditor");
        accountEditor->findChild<QLineEdit*>("connectionProfileName")->setText("Portable account");
        auto* email = accountEditor->findChild<QLineEdit*>("connectionAccountEmail");
        email->setText("portable@example.test");
        QTest::keyClick(email, Qt::Key_Return);
        check("Enter in the email field creates a SmartLink account", !accountEditor->isVisible()
            && ConnectionWorkspace::find(panel.workspaceSnapshot().value("accounts").toArray(),
                ConnectionWorkspace::accountId("portable@example.test")).value("name").toString() == "Portable account");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        panel.findChild<QPushButton*>("connectionAccountRename")->click();
        check("keypad Enter renames a SmartLink account", saveEntry("Club account renamed", Qt::Key_Enter));
        panel.selectWorkspaceProfile("stations", panel.workspaceSnapshot().value("stations").toArray().first().toObject().value("id").toString());
        panel.findChild<QPushButton*>("connectionSaveToStation")->click();
        auto* copyDialog = panel.findChild<QDialog*>("connectionCopyRadio");
        auto* destination = copyDialog->findChild<QComboBox*>("connectionCopyDestination");
        destination->setCurrentText("Garden shack");
        QTest::keyClick(destination, Qt::Key_Enter);
        check("keypad Enter copies the connection to the chosen location", !copyDialog->isVisible()
            && panel.workspaceSnapshot().value("stations").toArray().last().toObject().value("connections").toArray().size() == 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    {
        QJsonObject legacyAccountDocument = ConnectionWorkspace::load();
        QJsonArray accounts = legacyAccountDocument.value("accounts").toArray();
        for (qsizetype i = 0; i < accounts.size(); ++i) {
            QJsonObject account = accounts[i].toObject();
            if (account.value("id").toString() == "legacy") {
                account["email"] = "";
                accounts[i] = account;
            }
        }
        legacyAccountDocument["accounts"] = accounts;
        legacyAccountDocument["selectedAccount"] = "legacy";
        ConnectionWorkspace::save(legacyAccountDocument);
        ConnectionPanel panel;
        panel.findChild<QPushButton*>("connectionAccountRename")->click();
        auto* editor = panel.findChild<QDialog*>("connectionProfileEditor");
        auto* name = editor->findChild<QLineEdit*>("connectionProfileName");
        name->setText("Renamed legacy account");
        QTest::keyClick(name, Qt::Key_Return);
        const QJsonObject renamed = ConnectionWorkspace::find(panel.workspaceSnapshot().value("accounts").toArray(), "legacy");
        check("a legacy account without an email can be renamed without replacing its credential identity",
            !editor->isVisible() && renamed.value("name").toString() == "Renamed legacy account"
            && renamed.value("email").toString().isEmpty()
            && panel.workspaceSnapshot().value("selectedAccount").toString() == "legacy");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    {
        QJsonObject portsDocument = ConnectionWorkspace::load();
        QJsonArray locations = portsDocument.value("stations").toArray();
        for (int i = 0; i < 2; ++i) {
            QJsonObject location = locations[i].toObject();
            const int basePort = i == 0 ? 61001 : 50001;
            location["connections"] = QJsonArray{QJsonObject{
                {"id", QString("icom-port-%1").arg(i)}, {"name", QString("Icom %1").arg(i)},
                {"kind", "manual"}, {"family", "icom"}, {"host", QString("192.0.2.%1").arg(30 + i)},
                {"username", "portable"}, {"credentialId", "requires-relink"},
                {"route", QJsonObject{{"schema_version", 2}, {"identity", QJsonObject{{"family", "icom"}}},
                    {"icom", QJsonObject{{"base_port", basePort}}}, {"bind", QJsonObject{{"mode", "auto"}}}}}}};
            locations[i] = location;
        }
        portsDocument["stations"] = locations;
        portsDocument["selectedStation"] = locations[0].toObject().value("id");
        ConnectionWorkspace::save(portsDocument);
        ConnectionPanel panel;
        QSignalSpy connects(&panel, &ConnectionPanel::connectRequested);
        auto* port = panel.findChild<QSpinBox*>("connectionManualIcomBasePort");
        auto* mode = panel.findChild<QComboBox*>("connectionManualIcomPortMode");
        for (int locationIndex : {0, 1, 0}) {
            panel.selectWorkspaceProfile("stations", locations[locationIndex].toObject().value("id").toString());
            panel.findChild<QPushButton*>("connectionEditSaved")->click();
            const int expectedPort = locationIndex == 0 ? 61001 : 50001;
            check("each saved Icom connection restores its own port triplet", port->value() == expectedPort
                && mode->currentIndex() == (locationIndex == 0 ? 1 : 0));
            port->setValue(62001);
            panel.findChild<QPushButton*>("connectionBackToStation")->click();
        }
        check("editing saved port choices does not connect a radio", connects.isEmpty());
    }
    {
        QJsonObject grouped = ConnectionWorkspace::load();
        const QString home = ConnectionWorkspace::newId();
        const QString pota = ConnectionWorkspace::newId();
        const QString cabin = ConnectionWorkspace::newId();
        grouped["stations"] = QJsonArray{
            QJsonObject{{"id", home}, {"name", "Home"}, {"connections", QJsonArray{
                QJsonObject{{"id", "home-flex"}, {"name", "Shack Flex"}, {"kind", "local"}, {"family", "flex"}, {"serial", "flex-identity"}},
                QJsonObject{{"id", "home-hl2"}, {"name", "Home HL2"}, {"kind", "manual"}, {"family", "hl2"}, {"serial", "shared-board"}, {"host", "192.0.2.10"}}}}},
            QJsonObject{{"id", pota}, {"name", "POTA"}, {"connections", QJsonArray{
                QJsonObject{{"id", "pota-hl2"}, {"name", "Portable HL2"}, {"kind", "manual"}, {"family", "hl2"}, {"serial", "shared-board"}, {"host", "192.0.2.20"}}}}},
            QJsonObject{{"id", cabin}, {"name", "Cabin"}, {"connections", QJsonArray{
                QJsonObject{{"id", "cabin-flex"}, {"name", "Cabin Flex"}, {"kind", "smartlink"}, {"family", "flex"}, {"serial", "remote-flex"}, {"accountId", "legacy"}}}}}};
        grouped["selectedStation"] = cabin;
        check("mixed-family Home, POTA and Cabin paths persist as independent memberships", ConnectionWorkspace::save(grouped)
            && ConnectionWorkspace::load() == grouped);
        const auto homePaths = ConnectionWorkspace::find(grouped.value("stations").toArray(), home).value("connections").toArray();
        const auto potaPaths = ConnectionWorkspace::find(grouped.value("stations").toArray(), pota).value("connections").toArray();
        check("a verified radio identity is shared while each Location retains its own route",
            homePaths[1].toObject().value("serial") == potaPaths[0].toObject().value("serial")
            && homePaths[1].toObject().value("host") != potaPaths[0].toObject().value("host"));
    }
    check("newer settings schema can be staged for guard test",
        settings.setRadioFeature("local", "", "ConnectionWorkspace", 2, document));
    check("newer schema is never loaded as an older document", ConnectionWorkspace::load().isEmpty());
    check("newer schema cannot be overwritten", !ConnectionWorkspace::save(document));
    return failures ? 1 : 0;
}
