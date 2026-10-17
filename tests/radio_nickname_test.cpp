// Socket-free nickname encoding, model admission and production dialog edits.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
#include "core/backends/flex/CommandParser.h"
#include "core/backends/flex/FlexBackend.h"
#include "core/backends/hl2/Hl2Discovery.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QMetaObject>
#include <QObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTest>

#include <cstdio>
#include <initializer_list>
#include <memory>

namespace AetherSDR {
class RadioNicknameTestAccess {
public:
    static void identity(RadioModel& radio, const QString& family,
                         const QString& version)
    {
        radio.m_lastInfo.family = family;
        radio.m_lastInfo.serial = QStringLiteral("nickname-test-radio");
        radio.m_name = QStringLiteral("Discovery name");
        radio.m_version = version;
    }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {
int failures = 0;

void check(bool passed, const char* message)
{
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", message);
    if (!passed) {
        ++failures;
    }
}

class ConnectedFlex final : public FlexBackend {
public:
    bool connected = true;
    bool isConnected() const override { return connected; }
};

class LocalBackend final : public IRadioBackend {
public:
    RadioCapabilities capabilities() const override { return {}; }
    bool isConnected() const override { return true; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool, const TxCoordinator::Operation&,
                   const TxCoordinator::Completion&) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

QLineEdit* nicknameEdit(RadioSetupDialog& dialog)
{
    for (QLineEdit* edit : dialog.findChildren<QLineEdit*>()) {
        if (edit->accessibleName() == QStringLiteral("Radio nickname")) {
            return edit;
        }
    }
    return nullptr;
}

void finishEdit(QLineEdit& edit, const QString& text)
{
    edit.setText(text);
    edit.setModified(true);
    check(QMetaObject::invokeMethod(&edit, "editingFinished", Qt::DirectConnection),
          "production editingFinished handler reached");
}

void encoding()
{
    FlexBackend backend;
    QStringList commands;
    QString version = QStringLiteral("4.3.16.42034");
    check(backend.requestRadioNickname(QString()) == ReceiveDispatch::Unsupported,
          "no command sink refuses the request");
    backend.setCommandSink([&](const QString& command) { commands.append(command); });
    check(backend.requestRadioNickname(QStringLiteral("Unconfirmed")) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "encoding-only command sink cannot dispatch a write requiring confirmation");
    backend.setCommandSink([&](const QString& command, FlexBackend::CommandReply) {
        commands.append(command);
        return true;
    });
    backend.setFirmwareVersionProvider([&] { return version; });

    check(backend.requestRadioNickname(QStringLiteral("Desk 8600_#.,/-"))
              == ReceiveDispatch::Dispatched,
          "nickname request dispatched");
    check(commands == QStringList{QStringLiteral("radio name Desk8600.,/-")},
          "nonempty nickname matches FlexLib front-panel sanitization");
    for (const QString& name : {QString(), QStringLiteral("   "), QStringLiteral("_#@")}) {
        commands.clear();
        backend.requestRadioNickname(name);
        check(commands == QStringList{QStringLiteral("radio clear_name")},
              "empty or sanitized-empty nickname uses clear_name on 4.3");
    }
    commands.clear();
    backend.requestRadioNickname(QStringLiteral("Desk\ntransmit mox 1|"));
    check(commands == QStringList{QStringLiteral("radio name Desktransmitmox1")},
          "framing characters cannot inject a second command");
    for (const QString& old : {QStringLiteral("4.1.5.39794"),
                               QStringLiteral("4.2.20.41343"),
                               QStringLiteral("unknown"), QStringLiteral("4.3garbage"),
                               QStringLiteral("4.3.16.42034junk"), QString()}) {
        version = old;
        commands.clear();
        check(backend.requestRadioNickname(QString()) == ReceiveDispatch::Unsupported
                  && commands.isEmpty(),
              "older, unknown and malformed firmware cannot issue a clear or name query");
        check(backend.requestRadioNickname(QStringLiteral("Desk")) == ReceiveDispatch::Dispatched
                  && commands == QStringList{QStringLiteral("radio name Desk")},
              "ordinary naming remains available without clear support");
    }
    for (const QString& newer : {QStringLiteral("4.3.0.0"), QStringLiteral("5.0.0.0")}) {
        version = newer;
        commands.clear();
        backend.requestRadioNickname(QString());
        check(commands == QStringList{QStringLiteral("radio clear_name")},
              "firmware boundary and newer major versions use clear_name");
    }
    version = QStringLiteral("4.2.20.41343");
    commands.clear();
    check(backend.requestRadioNickname(QString()) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "reused backend reads the current firmware instead of caching a newer version");
}

void modelAndDialog()
{
    RadioModel radio;
    auto owned = std::make_unique<ConnectedFlex>();
    ConnectedFlex* backend = owned.get();
    QStringList commands;
    QList<FlexBackend::CommandReply> replies;
    backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply reply) {
        commands.append(command);
        replies.append(std::move(reply));
        return true;
    });
    backend->setFirmwareVersionProvider([&] { return radio.softwareVersion(); });
    radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                      QStringLiteral("4.3.16.42034"));
    test::SeamThreadAffinityProbe probe(backend);
    test::attachAllSeamSignals(probe);
    {
        backend->connected = false;
        RadioSetupDialog early(&radio);
        QLineEdit* edit = nicknameEdit(early);
        check(edit && edit->text().isEmpty(), "dialog can open before nickname arrives");
        if (!edit) {
            return;
        }
        check(edit->objectName() == QStringLiteral("radioNicknameEdit")
                  && edit->property("aetherAutomationEditCommit").toBool(),
              "production nickname field opts into explicit bridge editing and commit");
        backend->connected = true;
        QSignalSpy finished(edit, &QLineEdit::editingFinished);
        QTest::keyClick(edit, Qt::Key_Return);
        check(finished.size() == 1, "Return finishes an untouched early field");
        check(commands.isEmpty(), "untouched empty field cannot clear a newly connected radio");
        backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("LateName")}});
        check(edit->text() == QStringLiteral("LateName") && !edit->isModified(),
              "late radio identity refreshes untouched field without marking an edit");
        QTest::keyClick(edit, Qt::Key_Return);
        check(commands.isEmpty(), "untouched populated field cannot reassert a stale nickname");
        edit->setText(QStringLiteral("OperatorEdit"));
        edit->setModified(true);
        backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("OtherClient")}});
        check(edit->text() == QStringLiteral("OperatorEdit"),
              "radio status preserves an operator edit in progress");
    }
    backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("Original")}});
    check(radio.nickname() == QStringLiteral("Original"), "radio status seeds nickname");
    QSignalSpy dropped(&radio, &RadioModel::commandDropped);
    {
        RadioSetupDialog dialog(&radio);
        QLineEdit* edit = nicknameEdit(dialog);
        check(edit && edit->text() == QStringLiteral("Original"),
              "dialog shows the confirmed radio nickname");
        if (!edit) {
            return;
        }
        RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                          QStringLiteral("4.2.20.41343"));
        finishEdit(*edit, QString());
        check(commands.isEmpty() && edit->text() == QStringLiteral("Original"),
              "unsupported clear restores the confirmed nickname and sends no query");
        RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                          QStringLiteral("4.3.16.42034"));
        finishEdit(*edit, QString());
        check(commands == QStringList{QStringLiteral("radio clear_name")}
                  && dropped.isEmpty(),
              "dialog clears through backend without a raw model command");
        check(radio.nickname() == QStringLiteral("Original"),
              "request does not overwrite confirmed live state");
        check(!edit->isModified(), "dispatch consumes the operator edit");
        QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
        check(commands.size() == 1, "repeated editingFinished cannot dispatch the same clear twice");
        backend->decodeRadioStatus({{QStringLiteral("model"), QStringLiteral("FLEX-8600")}});
        check(radio.nickname() == QStringLiteral("Original"), "absent status keeps nickname");
        check(edit->text().isEmpty() && !edit->isModified(),
              "unrelated identity refresh cannot replace a pending clear with the old name");
        QSignalSpy identityChanged(&radio, &RadioModel::infoChanged);
        replies.takeFirst()(0, QString());
        check(radio.nickname().isEmpty() && edit->text().isEmpty()
                  && identityChanged.size() == 1,
              "R|0 alone clears model and field and notifies identity consumers");
        backend->decodeRadioStatus({{QStringLiteral("nickname"), QString()}});
        check(radio.nickname().isEmpty() && edit->text().isEmpty(),
              "present empty status clears model and open dialog nickname");
        commands.clear();
        finishEdit(*edit, QStringLiteral("NewName"));
        check(commands == QStringList{QStringLiteral("radio name NewName")},
              "ordinary dialog naming still uses the backend");
        replies.takeFirst()(1, QStringLiteral("refused"));
        check(radio.nickname().isEmpty() && edit->text().isEmpty(),
              "nonzero reply restores the confirmed field and leaves the model unchanged");

        dialog.show();
        edit->setFocus();
        QTest::keyClicks(edit, "Desk Name_");
        QTest::keyClick(edit, Qt::Key_Return);
        check(commands.last() == QStringLiteral("radio name DeskName"),
              "real typed Return dispatches sanitized nickname");
        const qsizetype sent = commands.size();
        backend->decodeRadioStatus({{QStringLiteral("model"), QStringLiteral("FLEX-8600")},
                                    {QStringLiteral("nickname"), QStringLiteral("OldName")}});
        check(edit->text() == QStringLiteral("Desk Name_"),
              "full radio status with the old name preserves the pending operator edit");
        replies.takeFirst()(0, QString());
        check(radio.nickname() == QStringLiteral("DeskName")
                  && edit->text() == QStringLiteral("DeskName") && !edit->isModified(),
              "successful rename applies the sanitized value without a nickname status");
        QTest::keyClick(edit, Qt::Key_Return);
        edit->clearFocus();
        QCoreApplication::processEvents();
        check(commands.size() == sent,
              "Return again and focus loss after an acknowledged edit send no second command");
        finishEdit(*edit, QStringLiteral("Accepted"));
        edit->setText(QStringLiteral("Next edit"));
        edit->setModified(true);
        replies.takeFirst()(0, QString());
        check(radio.nickname() == QStringLiteral("Accepted")
                  && edit->text() == QStringLiteral("Next edit") && edit->isModified(),
              "reply updates model without erasing a newer uncommitted edit");
        finishEdit(*edit, QStringLiteral("LostReply"));
        replies.clear();
        backend->connected = false;
        emit radio.connectionStateChanged(false);
        backend->connected = true;
        emit radio.connectionStateChanged(true);
        backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("Reconnected")}});
        check(edit->text() == QStringLiteral("Reconnected") && !edit->isModified(),
              "disconnect with a discarded reply releases pending field refresh on reconnect");
        finishEdit(*edit, QStringLiteral("AnotherLostReply"));
        replies.clear();
        edit->setText(QStringLiteral("KeepTyping"));
        edit->setModified(true);
        emit radio.connectionStateChanged(false);
        check(edit->text() == QStringLiteral("KeepTyping") && edit->isModified(),
              "disconnect preserves a newer uncommitted operator edit");
        backend->connected = false;
        commands.clear();
        finishEdit(*edit, QString());
        check(commands.isEmpty(), "disconnected dialog cannot dispatch a nickname edit");
        backend->connected = true;
        finishEdit(*edit, QString());
        check(commands == QStringList{QStringLiteral("radio clear_name")},
              "reconnected dialog can dispatch again after a refused edit");
        replies.takeFirst()(0, QString());
    }
    {
        RadioSetupDialog reopened(&radio);
        QLineEdit* edit = nicknameEdit(reopened);
        check(edit && edit->text().isEmpty(),
              "reopened dialog keeps cleared nickname empty instead of discovery fallback");
    }
    check(probe.violations().isEmpty() && probe.afterDisconnect().isEmpty(),
          "nickname status follows the backend seam affinity and lifetime contract");
    commands.clear();
    radio.setBackendForTest(std::make_unique<LocalBackend>(), QStringLiteral("hl2"));
    check(radio.requestRadioNickname(QStringLiteral("WrongRadio")) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "backend replacement cannot reuse the old Flex nickname command sink");
}

void replyOrdering()
{
    auto backend = std::make_unique<FlexBackend>();
    QList<FlexBackend::CommandReply> replies;
    backend->setCommandSink([&](const QString&, FlexBackend::CommandReply reply) {
        replies.append(std::move(reply));
        return true;
    });
    QSignalSpy changes(backend.get(), &IRadioBackend::radioChanged);
    backend->requestRadioNickname(QStringLiteral("First"));
    backend->requestRadioNickname(QStringLiteral("Second"));
    replies[1](0, QString());
    replies[0](0, QString());
    check(changes.size() == 1
              && qvariant_cast<RadioDelta>(changes.first().first()).nickname == QStringLiteral("Second"),
          "an older success cannot overwrite a newer successful nickname reply");
    changes.clear();
    backend->requestRadioNickname(QStringLiteral("RaceName"));
    backend->decodeRadioStatus({{QStringLiteral("slices"), QStringLiteral("0")},
                                {QStringLiteral("panadapters"), QStringLiteral("1")},
                                {QStringLiteral("nickname"), QStringLiteral("OldName")}});
    replies.last()(0, QString());
    check(changes.size() == 2
              && qvariant_cast<RadioDelta>(changes.last().first()).nickname == QStringLiteral("RaceName"),
          "R|0 confirms rename after an intervening full radio status carrying the old name");
    backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("OtherClient")}});
    check(changes.size() == 3
              && qvariant_cast<RadioDelta>(changes.last().first()).nickname == QStringLiteral("OtherClient"),
          "nickname status arriving after confirmation remains authoritative");
    changes.clear();
    backend->setFirmwareVersionProvider([] { return QStringLiteral("4.3.16.42034"); });
    backend->requestRadioNickname(QString());
    backend->decodeRadioStatus({{QStringLiteral("nickname"), QStringLiteral("OldName")}});
    replies.last()(0, QString());
    check(changes.size() == 2
              && qvariant_cast<RadioDelta>(changes.last().first()).nickname == QString(),
          "R|0 confirms clear after an intervening status carrying the old name");
    changes.clear();
    bool accepted = true;
    backend->requestRadioNickname(QStringLiteral("OldSession"), [&](bool ok) { accepted = ok; });
    backend->clearExtensionHandles();
    replies.last()(0, QString());
    check(changes.isEmpty() && !accepted, "a late reply from an ended session cannot publish identity");
    accepted = true;
    int completions = 0;
    backend->requestRadioNickname(QStringLiteral("Destroyed"), [&](bool ok) {
        accepted = ok;
        ++completions;
    });
    backend.reset();
    replies.last()(0, QString());
    check(!accepted && completions == 1,
          "retained reply callback reports failure exactly once after backend destruction");
}

void replyValidation()
{
    FlexBackend backend;
    FlexBackend::CommandReply reply;
    backend.setCommandSink([&](const QString&, FlexBackend::CommandReply completion) {
        reply = std::move(completion);
        return true;
    });
    QSignalSpy changes(&backend, &IRadioBackend::radioChanged);
    for (const QString& line : {QStringLiteral("R123"), QStringLiteral("R123|"),
                                QStringLiteral("R123||"), QStringLiteral("R123|garbage|"),
                                QStringLiteral("R123|100000000|"), QStringLiteral("R123|+0|"),
                                QStringLiteral("R123|0x0|"), QStringLiteral("R123| 0 |"),
                                QStringLiteral("R123|000000000|")}) {
        const ParsedMessage parsed = CommandParser::parseLine(line);
        check(parsed.resultCode != 0, "malformed reply result cannot be parsed as success");
        bool accepted = true;
        int completions = 0;
        backend.requestRadioNickname(QStringLiteral("Unconfirmed"), [&](bool ok) {
            accepted = ok;
            ++completions;
        });
        reply(parsed.resultCode, parsed.object);
        check(!accepted && completions == 1 && changes.isEmpty(),
              "malformed parsed reply rejects nickname without publishing identity");
    }
    const ParsedMessage fatal = CommandParser::parseLine(QStringLiteral("R123|F3000001|refused"));
    check(static_cast<quint32>(fatal.resultCode) == 0xF3000001U,
          "valid high-bit radio error retains its unsigned wire pattern");
    const ParsedMessage success = CommandParser::parseLine(QStringLiteral("R123|0|"));
    bool accepted = false;
    backend.requestRadioNickname(QStringLiteral("Confirmed"), [&](bool ok) { accepted = ok; });
    reply(success.resultCode, success.object);
    check(accepted && changes.size() == 1
              && qvariant_cast<RadioDelta>(changes.first().first()).nickname == QStringLiteral("Confirmed"),
          "valid zero result still confirms and publishes nickname");
}

void productionFirmwareProvider()
{
    RadioModel radio;
    RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                     QStringLiteral("4.3.16.42034"));
    check(radio.rebuildBackendForTest(QStringLiteral("flex")),
          "production Flex setup constructs backend without connecting");
    auto* backend = qobject_cast<FlexBackend*>(radio.backend());
    check(backend != nullptr, "production setup selects Flex backend");
    if (!backend) {
        return;
    }
    QStringList commands;
    backend->setCommandSink([&](const QString& command, FlexBackend::CommandReply) {
        commands.append(command);
        return true;
    });
    check(backend->requestRadioNickname(QString()) == ReceiveDispatch::Dispatched
              && commands == QStringList{QStringLiteral("radio clear_name")},
          "production setup wires the radio model firmware provider");
    RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                     QStringLiteral("4.2.20.41343"));
    commands.clear();
    check(backend->requestRadioNickname(QString()) == ReceiveDispatch::Unsupported
              && commands.isEmpty(),
          "production firmware provider reads later model changes");
}

void statusBarNicknameWiring()
{
    // MainWindow is not linked into socket-free test targets. Pin only the
    // two production consumers that must pass authoritative empty text through.
    QFile source(QStringLiteral(AETHER_SOURCE_DIR "/src/gui/MainWindow.cpp"));
    check(source.open(QIODevice::ReadOnly), "status-bar wiring source is available");
    const QString text = QString::fromUtf8(source.readAll());
    const auto consumer = [&text](const QString& signal) {
        qsizetype begin = text.indexOf(signal);
        while (begin >= 0) {
            const qsizetype end = text.indexOf(QStringLiteral("\n    });"), begin);
            if (end < 0) {
                return QString();
            }
            const QString block = text.mid(begin, end - begin);
            if (block.contains(QStringLiteral("setStatusBarStationText"))) {
                return block;
            }
            begin = text.indexOf(signal, end);
        }
        return QString();
    };
    const QString identity = consumer(
        QStringLiteral("connect(&m_radioModel, &RadioModel::infoChanged"));
    check(identity.contains(QStringLiteral("setStatusBarStationText"))
              && !identity.contains(QStringLiteral("nickname().isEmpty()")),
          "infoChanged status-bar consumer accepts authoritative empty nickname (source pin)");
    const QString telemetry = consumer(QStringLiteral("&MeterModel::hwTelemetryChanged"));
    check(telemetry.contains(QStringLiteral("setStatusBarStationText"))
              && !telemetry.contains(QStringLiteral("nick.isEmpty()")),
          "telemetry status-bar consumer accepts authoritative empty nickname (source pin)");
}

void localNicknames()
{
    RadioModel radio;
    radio.setBackendForTest(std::make_unique<LocalBackend>(), QStringLiteral("hl2"));
    RadioNicknameTestAccess::identity(radio, QStringLiteral("hl2"), QString());
    check(radio.requestRadioNickname(QStringLiteral("Unsupported")) == ReceiveDispatch::Unsupported,
          "backend without radio-owned names refuses the typed operation");
    const RadioInfo info = radio.lastRadioInfo();
    {
        RadioSetupDialog dialog(&radio);
        QLineEdit* edit = nicknameEdit(dialog);
        check(edit != nullptr, "local nickname field exists");
        if (!edit) {
            return;
        }
        finishEdit(*edit, QStringLiteral("Local Desk"));
        check(hl2::Hl2Discovery::effectiveNickname(info.family, info.serial, QString())
                  == QStringLiteral("Local Desk"),
              "non-Flex nickname retains local storage and spaces");
        emit radio.connectionStateChanged(false);
        check(edit->text() == QStringLiteral("Local Desk"),
              "disconnect does not replace a locally stored nickname with radio identity");
        finishEdit(*edit, QString());
        check(!hl2::Hl2Discovery::hasCustomNickname(info.family, info.serial),
              "non-Flex clear removes the local nickname");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-nickname"));
    QApplication app(argc, argv);
    AppSettings::instance().load();
    encoding();
    modelAndDialog();
    replyOrdering();
    replyValidation();
    productionFirmwareProvider();
    statusBarNicknameWiring();
    localNicknames();
    return failures ? 1 : 0;
}
