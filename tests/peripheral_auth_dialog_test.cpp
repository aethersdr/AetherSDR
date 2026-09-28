#include "TestSettingsProfile.h"
#include "AutomationSensitiveLineEdit.h"
#include "core/TgxlConnection.h"
#include "core/PgxlConnection.h"
#include "gui/RadioSetupDialog.h"
#include "gui/PeripheralAuthStore.h"
#include "gui/PeripheralAuthConnectFlow.h"
#include "models/AntennaGeniusModel.h"
#include "models/RadioModel.h"
#include "PeripheralAuthStoreFake.h"

#include <QApplication>
#include <QByteArray>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <cstdio>
#include <utility>

using namespace AetherSDR;

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("peripheral-auth-dialog-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QApplication app(argc, argv);
    RadioModel model;
    TgxlConnection tgxl;
    PgxlConnection pgxl;
    AntennaGeniusModel ag;
    RadioSetupDialog dialog(&model, nullptr, &tgxl, &pgxl, &ag);
    dialog.show();
    dialog.selectTab(QStringLiteral("Peripherals"));
    QCoreApplication::processEvents();

    QGridLayout* grid = nullptr;
    for (QGridLayout* candidate : dialog.findChildren<QGridLayout*>()) {
        QLayoutItem* item = candidate->itemAtPosition(1, 0);
        QLabel* label = item ? qobject_cast<QLabel*>(item->widget()) : nullptr;
        if (label && label->text().contains(QStringLiteral("Tuner Genius XL"))) {
            grid = candidate;
            break;
        }
    }
    if (!grid) {
        std::fprintf(stderr, "TGXL peripheral row missing\n");
        return 1;
    }
    QLineEdit* code = qobject_cast<QLineEdit*>(grid->itemAtPosition(1, 4)->widget());
    QLabel* status = qobject_cast<QLabel*>(grid->itemAtPosition(1, 7)->widget());
    QPushButton* show = qobject_cast<QPushButton*>(grid->itemAtPosition(1, 5)->widget());
    QPushButton* connectButton = qobject_cast<QPushButton*>(grid->itemAtPosition(1, 3)->widget());
    QPushButton* clearButton = qobject_cast<QPushButton*>(grid->itemAtPosition(1, 6)->widget());
    QLineEdit* ip = qobject_cast<QLineEdit*>(grid->itemAtPosition(1, 1)->widget());
    if (!code || !status || !show || !connectButton || !clearButton || !ip
        || !code->property("aetherSensitiveValue").toBool()) {
        std::fprintf(stderr, "TGXL credential controls missing or not sensitive\n");
        return 1;
    }
    code->setText(QStringLiteral("sample"));
    show->click();
    if (code->echoMode() != QLineEdit::Normal
        || automationLineEditValue(code) != QStringLiteral("<hidden>")) {
        std::fprintf(stderr, "Show exposed the code to automation\n");
        return 1;
    }

    // The invalid-code path rejects the click before opening a socket.
    // The credential store linked here is an in-memory test adapter.
    ip->setText(QStringLiteral("192.0.2.10"));
    code->setText(QStringLiteral("bad code"));
    connectButton->click();
    if (!status->text().contains(QStringLiteral("invalid authorization code"))
        || tgxl.isConnecting()) {
        std::fprintf(stderr, "Connect button did not reject invalid code\n");
        return 1;
    }
    const QString endpoint = PeripheralAuthStore::endpoint(QStringLiteral("192.0.2.10"), 9010);
    PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl, endpoint,
                              QStringLiteral("session-code"), &app);
    clearButton->click();
    QCoreApplication::processEvents();
    PeripheralAuthStore::LoadResult afterClear;
    PeripheralAuthStore::load(PeripheralAuthStore::Device::Tgxl, endpoint, &app,
        [&afterClear](const PeripheralAuthStore::LoadResult& result) { afterClear = result; });
    QCoreApplication::processEvents();
    if (afterClear.status != PeripheralAuthStore::LoadStatus::Missing || !code->text().isEmpty()) {
        std::fprintf(stderr, "Clear code button did not clear saved credential\n");
        return 1;
    }

    // A denied delete must leave a visible, actionable error in the dialog.
    PeripheralAuthStore::save(PeripheralAuthStore::Device::Tgxl, endpoint,
                              QStringLiteral("session-code"), &app);
    FakePeripheralAuthStore::setNextClearResult(false);
    clearButton->click();
    QCoreApplication::processEvents();
    if (!status->text().contains(QStringLiteral("saved code remains in keychain"))) {
        std::fprintf(stderr, "Clear code failure was not shown\n");
        return 1;
    }

    // Simulate the state reached after a newly entered code meets a LAN
    // greeting without AUTH. The status must say why the code was not saved.
    status->setProperty("pendingAuthCode", true);
    tgxl.setAuthCode(QStringLiteral("sample"));
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17")))) {
        return 1;
    }
    if (!status->text().contains(QStringLiteral("code not saved"))
        || status->property("pendingAuthCode").toBool()) {
        std::fprintf(stderr, "unchallenged connection did not explain unsaved code\n");
        return 1;
    }

    // Recovery retires the prior error; a subsequent disconnect must show
    // the actual offline state instead of leaving "Connected" on screen.
    tgxl.connectionFailed(QStringLiteral("temporary connection failure"));
    if (!status->property("credentialError").toBool()) {
        std::fprintf(stderr, "connection failure was not marked\n");
        return 1;
    }
    tgxl.connected();
    if (status->property("credentialError").toBool()
        || status->text() != QStringLiteral("Connected")) {
        std::fprintf(stderr, "recovery retained a stale error\n");
        return 1;
    }
    tgxl.disconnect();
    if (status->text() != QStringLiteral("Not connected")) {
        std::fprintf(stderr, "disconnect left a stale connected status\n");
        return 1;
    }

    // A rejected typed code cannot make a later authenticated connection
    // look like a connection that never requested authentication.
    status->setProperty("pendingAuthCode", true);
    tgxl.connectionFailed(QStringLiteral("temporary socket failure"));
    if (!status->property("pendingAuthCode").toBool()) {
        std::fprintf(stderr, "transient failure discarded pending code state\n");
        return 1;
    }
    if (!QMetaObject::invokeMethod(&tgxl, "beginAttempt", Qt::DirectConnection)) {
        return 1;
    }
    tgxl.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17 AUTH")))
        || !QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("R1|0|Unauthorized")))) {
        return 1;
    }
    if (status->property("pendingAuthCode").toBool()) {
        std::fprintf(stderr, "failed attempt retained pending code state\n");
        return 1;
    }
    if (!QMetaObject::invokeMethod(&tgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V1.2.17")))) {
        return 1;
    }
    if (status->text().contains(QStringLiteral("did not request authentication"))) {
        std::fprintf(stderr, "later connection inherited stale code note\n");
        return 1;
    }
    QLabel* pgxlStatus = qobject_cast<QLabel*>(grid->itemAtPosition(2, 7)->widget());
    if (!pgxlStatus) {
        return 1;
    }
    pgxlStatus->setProperty("pendingAuthCode", true);
    pgxl.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&pgxl, "processLine", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("V3.9.1 AUTH")))
        || !QMetaObject::invokeMethod(&pgxl, "processLine", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("R1|FF|Denied")))) {
        return 1;
    }
    if (pgxlStatus->property("pendingAuthCode").toBool()) {
        std::fprintf(stderr, "PGXL failure retained pending code state\n");
        return 1;
    }
    // Disconnecting before an AUTH reply discards an entered code. A later
    // greeting must not claim that this old code was merely unchallenged.
    for (const auto& item : {std::pair<QLabel*, QObject*>{status, &tgxl},
                             std::pair<QLabel*, QObject*>{pgxlStatus, &pgxl}}) {
        item.first->setProperty("credentialError", false);
        item.first->clear();
        item.first->setProperty("pendingAuthCode", true);
        if (item.second == &tgxl) {
            tgxl.setAuthCode(QStringLiteral("interrupted-code"));
            tgxl.disconnect();
        } else {
            pgxl.setAuthCode(QStringLiteral("interrupted-code"));
            pgxl.disconnect();
        }
        if (item.first->property("pendingAuthCode").toBool()
            || !item.first->property("discardedAuthCode").toBool()
            || !item.first->text().contains(QStringLiteral("discarded before verification"))) {
            std::fprintf(stderr, "discarded TGXL/PGXL code was not explained: %s pending=%d discarded=%d\n",
                         item.first->text().toUtf8().constData(),
                         item.first->property("pendingAuthCode").toBool(),
                         item.first->property("discardedAuthCode").toBool());
            return 1;
        }
    }

    // Set only the model's current-attempt metadata, without opening a socket.
    // The same model backs both rows and retains this metadata on TCP failure.
    AgDeviceInfo& attempt = const_cast<AgDeviceInfo&>(ag.connectedDevice());
    QLabel* agStatus = qobject_cast<QLabel*>(grid->itemAtPosition(3, 7)->widget());
    QLabel* shackSwitchStatus = qobject_cast<QLabel*>(grid->itemAtPosition(4, 7)->widget());
    if (!agStatus || !shackSwitchStatus) {
        std::fprintf(stderr, "Antenna Genius or ShackSwitch status row missing\n");
        return 1;
    }
    attempt.name = QStringLiteral("ShackSwitch");
    ag.connectionError(QStringLiteral("ShackSwitch connection failed"));
    if (!shackSwitchStatus->text().contains(QStringLiteral("ShackSwitch connection failed"))
        || agStatus->text().contains(QStringLiteral("ShackSwitch connection failed"))) {
        std::fprintf(stderr, "ShackSwitch error appeared on the Antenna Genius row\n");
        return 1;
    }
    attempt.name = QStringLiteral("Antenna Genius");
    agStatus->setProperty("pendingAuthCode", true);
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("rejected-code"));
    if (!QMetaObject::invokeMethod(&ag, "processTcpBytes", Qt::DirectConnection,
                                   Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n")))) {
        return 1;
    }
    if (!agStatus->text().contains(QStringLiteral("Connection closed before authorization command"))
        || shackSwitchStatus->text().contains(QStringLiteral("Connection closed before authorization command"))
        || agStatus->property("pendingAuthCode").toBool()) {
        std::fprintf(stderr, "Antenna Genius error routing or pending code state failed\n");
        return 1;
    }
    agStatus->setProperty("credentialError", false);
    agStatus->clear();
    agStatus->setProperty("pendingAuthCode", true);
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.10")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("interrupted-ag-code"));
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9007))
        || agStatus->property("pendingAuthCode").toBool()
        || !agStatus->property("discardedAuthCode").toBool()
        || !agStatus->text().contains(QStringLiteral("discarded before verification"))) {
        std::fprintf(stderr, "discarded AG code was not explained\n");
        return 1;
    }

    // Clearing AG's saved credential must leave the shared model's current
    // ShackSwitch auth block intact.
    attempt.name = QStringLiteral("ShackSwitch");
    if (!QMetaObject::invokeMethod(&ag, "beginAttemptAt", Qt::DirectConnection,
                                   Q_ARG(QString, QStringLiteral("192.0.2.11")), Q_ARG(quint16, 9007))) {
        return 1;
    }
    ag.setAuthCode(QStringLiteral("rejected-switch-code"));
    if (!QMetaObject::invokeMethod(&ag, "processTcpBytes", Qt::DirectConnection,
                                   Q_ARG(QByteArray, QByteArray("V4.0.22 AG AUTH\r\n")))
        || !ag.isAuthBlockedFor(QStringLiteral("192.0.2.11"), 9007)) {
        std::fprintf(stderr, "ShackSwitch block setup failed\n");
        return 1;
    }
    QPushButton* clearAg = qobject_cast<QPushButton*>(grid->itemAtPosition(3, 6)->widget());
    if (!clearAg) {
        return 1;
    }
    clearAg->click();
    QCoreApplication::processEvents();
    if (!ag.isAuthBlockedFor(QStringLiteral("192.0.2.11"), 9007)) {
        std::fprintf(stderr, "AG Clear code reset ShackSwitch auth block\n");
        return 1;
    }

    // Inject a synchronous target-switch discard at the same seam used by
    // each Connect row. This pins ordering without opening a TCP socket.
    QLineEdit replacement;
    QLabel replacementStatus;
    replacement.setText(QStringLiteral("replacement-code"));
    replacementStatus.setProperty("pendingAuthCode", true);
    QString appliedCode;
    bool stalePendingObserved = false;
    connectPeripheralWithCode(&replacement, &replacementStatus,
        QStringLiteral("192.0.2.11"), 9010,
        [&replacementStatus, &stalePendingObserved](const QString&, quint16) {
            stalePendingObserved = replacementStatus.property("pendingAuthCode").toBool();
            if (stalePendingObserved) {
                replacementStatus.setProperty("discardedAuthCode", true);
            }
        },
        [&appliedCode](const QString& value) { appliedCode = value; });
    if (stalePendingObserved || !replacementStatus.property("pendingAuthCode").toBool()
        || replacementStatus.property("discardedAuthCode").toBool()
        || appliedCode != QStringLiteral("replacement-code")) {
        std::fprintf(stderr, "replacement code was discarded during target switch\n");
        return 1;
    }
    return 0;
}
