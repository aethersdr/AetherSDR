// Socket-free nickname encoding, model admission and production dialog edits.
#include "TestSettingsProfile.h"
#include "SeamThreadAffinityProbe.h"
#include "core/AppSettings.h"
#include "core/RadioDiscovery.h"
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
    static void wireStatus(RadioModel& radio, FlexBackend& backend)
    {
        QObject::connect(&backend, &IRadioBackend::radioChanged,
                         &radio, &RadioModel::applyRadioChanges);
    }
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
    backend->setCommandSink([&](const QString& command) { commands.append(command); });
    backend->setFirmwareVersionProvider([&] { return radio.softwareVersion(); });
    radio.setBackendForTest(std::move(owned), QStringLiteral("flex"));
    RadioNicknameTestAccess::identity(radio, QStringLiteral("flex"),
                                      QStringLiteral("4.3.16.42034"));
    RadioNicknameTestAccess::wireStatus(radio, *backend);
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
        backend->decodeRadioStatus({{QStringLiteral("nickname"), QString()}});
        check(radio.nickname().isEmpty() && edit->text().isEmpty(),
              "present empty status clears model and open dialog nickname");
        commands.clear();
        finishEdit(*edit, QStringLiteral("NewName"));
        check(commands == QStringList{QStringLiteral("radio name NewName")},
              "ordinary dialog naming still uses the backend");
        backend->connected = false;
        commands.clear();
        finishEdit(*edit, QString());
        check(commands.isEmpty(), "disconnected dialog cannot dispatch a nickname edit");
        backend->connected = true;
        finishEdit(*edit, QString());
        check(commands == QStringList{QStringLiteral("radio clear_name")},
              "reconnected dialog can dispatch again after a refused edit");
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
    backend->setCommandSink([&](const QString& command) { commands.append(command); });
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
    productionFirmwareProvider();
    statusBarNicknameWiring();
    localNicknames();
    return failures ? 1 : 0;
}
