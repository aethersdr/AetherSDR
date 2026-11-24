// A key objection registered with RadioModel (RFC #6237) must reach every
// key-on path the model gates, and never an un-key. Socket-free: an injected
// backend records what the model dispatches, and each path is driven the way
// its callers drive it. Each path is run twice on a fresh model: with an
// objection (nothing may reach the backend, and the question must have been
// asked on that path) and without one (the positive control: the same call
// does reach the backend, so "nothing reached it" above is not vacuous).
// No device registers here; the objection is a test lambda.
//
// Run:  ./build/key_objection_gate_test

#include "FlexTransmitServiceStatusTestSupport.h"
#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/TgxlConnection.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/TransmitDelta.h"
#include "core/backends/TunerDelta.h"
#include "models/CwxModel.h"
#include "models/DvkModel.h"
#include "models/NavtexModel.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#include "models/TunerModel.h"
#include "models/TxController.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace AetherSDR {
// Reads the direct link's command counter; no socket is opened.
struct PeripheralConnectionTestAccess {
    static quint32 sequence(const TgxlConnection& connection) { return connection.m_seq; }
};
} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const std::string& name, bool ok, const std::string& detail = {})
{
    std::printf("%s %-84s %s\n", ok ? "[ OK ]" : "[FAIL]", name.c_str(), detail.c_str());
    if (!ok) {
        ++g_failed;
    }
}

std::string joined(const QStringList& list)
{
    return list.join(QLatin1Char('|')).toStdString();
}

const QString kObjection = QStringLiteral("TX failed — Test objection.");

class RecordingBackend final : public IRadioBackend {
public:
    RadioCapabilities caps;
    bool connected{false};
    QStringList* commands;
    TxCoordinator::Operation keyed;

    explicit RecordingBackend(QStringList& record)
        : commands(&record)
    {
        caps.canTransmit = true;
        caps.hasRadioSideCwKeyer = true;
        caps.hasTuner = true;
        caps.hasVoiceKeyer = true;
    }
    RadioCapabilities capabilities() const override { return caps; }
    bool isConnected() const override { return connected; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool on, const TxCoordinator::Operation& operation,
                   const TxCoordinator::Completion&) override
    {
        *commands << (on ? "mox:on" : "mox:off");
        if (on) { keyed = operation; }
    }
    void setTune(bool on, int, const TxCoordinator::Operation& operation,
                 const TxCoordinator::Completion&) override
    {
        *commands << (on ? "tune:on" : "tune:off");
        if (on) { keyed = operation; }
    }
    void setAtu(bool on, const TxCoordinator::Operation& operation,
                const TxCoordinator::Completion&) override
    {
        *commands << (on ? "atu:on" : "atu:off");
        if (on) { keyed = operation; }
    }
    ReceiveDispatch requestCwControl(const CwControlRequest& request,
                                     const TxCoordinator::Command& command) override
    {
        if (request.kind == CwControlRequest::Kind::PttEnvelope) {
            return IRadioBackend::requestCwControl(request, command);
        }
        *commands << (request.down ? "cw:on" : "cw:off");
        if (request.down) { keyed = command.operation; }
        return ReceiveDispatch::Dispatched;
    }
    QString sendCwText(const QString& text, const TxCoordinator::Operation&,
                       const TxCoordinator::Completion&) override
    {
        *commands << "cwx:" + text;
        return {};
    }
    void abortCwText(const TxCoordinator::Operation&, const TxCoordinator::Completion&) override
    {
        *commands << "cwx:abort";
    }
    void invokeExtension(const QString&, const QString& verb, quint64, const QVariant&) override
    {
        *commands << "ext:" + verb;
    }
    ReceiveDispatch requestDvk(const DvkRequest& request, const TransmitServiceCompletion&,
                               const TxCoordinator::Command&, const RadioControlFence&) override
    {
        using Operation = DvkRequest::Operation;
        *commands << (request.operation == Operation::PlaybackStart ? QStringLiteral("dvk-playback:on")
                      : request.operation == Operation::PlaybackStop ? QStringLiteral("dvk-playback:off")
                      : request.operation == Operation::PreviewStart ? QStringLiteral("dvk-preview")
                      : request.operation == Operation::RecordStart ? QStringLiteral("dvk-record")
                                                                     : QStringLiteral("dvk-other"));
        return ReceiveDispatch::Dispatched;
    }
    ReceiveDispatch requestNavtex(const NavtexRequest&, const TransmitServiceCompletion&,
                                  const TxCoordinator::Command&) override
    {
        *commands << QStringLiteral("navtex:on");
        return ReceiveDispatch::Dispatched;
    }
};

struct Fixture {
    // The recorder outlives the model's backend and its shutdown clean-up.
    QStringList commands;
    QStringList interlockKeys;
    QStringList interlockMessages;
    RadioModel radio;
    RecordingBackend* backend{nullptr};
    QObject owner;
    QString objection;
    int asked{0};

    Fixture()
    {
        auto owned = std::make_unique<RecordingBackend>(commands);
        backend = owned.get();
        radio.setBackendForTest(std::move(owned), QStringLiteral("test"));
        if (!radio.automationApplySliceFixture(0, QStringLiteral("A")) || !radio.slice(0)) {
            qFatal("Could not install the slice fixture");
        }
        SliceDelta delta;
        delta.txSlice = true;
        delta.mode = QStringLiteral("USB");
        delta.panId = QStringLiteral("0x40000000");
        radio.slice(0)->applyChanges(delta);
        // The slice fixture installs only on a model that is not connected.
        backend->connected = true;
        radio.transmitModel().setTxModeGetter([] { return QStringLiteral("USB"); });
        QObject::connect(&radio, &RadioModel::interlockNotificationRequested, &owner,
                         [this](const QString& message, const QString& key, const QString&) {
                             interlockMessages << message;
                             interlockKeys << key;
                         });
        commands.clear();
    }

    void addObjection()
    {
        radio.addKeyObjection(&owner, [this]() {
            ++asked;
            return objection;
        });
    }

    bool nothingKeyed() const
    {
        return commands.filter(QStringLiteral(":on")).isEmpty()
               && commands.filter(QStringLiteral("cwx:")).isEmpty()
               && !commands.contains(QStringLiteral("ext:tuner.autotune"));
    }
};

struct Path {
    const char* name;
    std::function<void(Fixture&)> start;
    // What the backend records when nothing objects; empty when this fixture
    // cannot show the dispatch, and only the question is checked.
    const char* dispatched;
};

std::vector<Path> keyOnPaths()
{
    using Source = TransmitModel::PttSource;
    using Activity = TxController::Activity;
    return {
        {"MOX, TransmitModel::setMox",
         [](Fixture& f) { f.radio.transmitModel().setMox(true); }, "mox:on"},
        {"PTT, TransmitModel::requestPttOn",
         [](Fixture& f) { f.radio.transmitModel().requestPttOn(Source::Mox); }, "mox:on"},
        {"PTT, RadioModel::setTransmit (serial PTT, legacy)",
         [](Fixture& f) { f.radio.setTransmit(true); }, "mox:on"},
        {"PTT with the DAX source (rigctld, SmartCAT, TCI with audio)",
         [](Fixture& f) { f.radio.setTransmit(true, Source::Dax); }, "mox:on"},
        {"PTT with the TCI hardware source",
         [](Fixture& f) { f.radio.transmitModel().requestPttOn(Source::TciHardware); }, "mox:on"},
        {"PTT with the WSPR source",
         [](Fixture& f) { f.radio.transmitModel().requestPttOn(Source::Wspr); }, "mox:on"},
        {"MOX through a captured input (button, shortcut, MIDI, automation, TCI)",
         [](Fixture& f) {
             (void)f.radio.localTxController()->capture(Activity::Mox).start();
         },
         "mox:on"},
        {"producer PTT, RadioModel::setProducerTransmit (rigctl, SmartCAT, TCI)",
         [](Fixture& f) {
             const TxController::Input input = f.radio.localTxController()->capture(Activity::Mox);
             (void)f.radio.setProducerTransmit(input.request(), true, Source::Dax);
         },
         "mox:on"},
        {"TUNE, TransmitModel::startTune (SWR sweep, TCI tune)",
         [](Fixture& f) { f.radio.transmitModel().startTune(); }, "tune:on"},
        {"two-tone TUNE", [](Fixture& f) { f.radio.transmitModel().startTwoToneTune(); },
         "tune:on"},
        {"TUNE through a captured input (KAT500 tune, TCI)",
         [](Fixture& f) {
             (void)f.radio.localTxController()->capture(Activity::Tune).start();
         },
         "tune:on"},
        {"ATU start", [](Fixture& f) { f.radio.transmitModel().atuStart(); }, "atu:on"},
        {"ATU start through a captured input",
         [](Fixture& f) {
             (void)f.radio.localTxController()->capture(Activity::Atu).start();
         },
         "atu:on"},
        {"CW straight key", [](Fixture& f) { f.radio.sendCwKey(true); }, "cw:on"},
        {"CW PTT", [](Fixture& f) { f.radio.sendCwPtt(true); }, "mox:on"},
        {"CW paddle", [](Fixture& f) { f.radio.sendCwPaddle(true, false); }, "cw:on"},
        {"CW keyer element", [](Fixture& f) { f.radio.sendCwKeyEdge(true); }, "cw:on"},
        {"CW key through a captured input (producer CW)",
         [](Fixture& f) {
             (void)f.radio.localTxController()->capture(Activity::CwKey).start();
         },
         "cw:on"},
        {"CWX text", [](Fixture& f) { f.radio.cwxModel().send(QStringLiteral("CQ")); }, ""},
        {"CWX character", [](Fixture& f) { f.radio.cwxModel().sendChar(QStringLiteral("E")); },
         ""},
        {"CWX macro", [](Fixture& f) { f.radio.cwxModel().sendMacro(1); }, ""},
        {"CWX through a captured input",
         [](Fixture& f) {
             (void)f.radio.localTxController()->capture(Activity::Cwx).send(QStringLiteral("CQ"));
         },
         ""},
        {"producer PTT, RadioModel::requestProducerPttOn (local voice keyer, RX playback, AX.25)",
         [](Fixture& f) {
             const TxController::Input input = f.radio.localTxController()->capture(Activity::Mox);
             (void)f.radio.requestProducerPttOn(input.request(), Source::Dax);
         },
         "mox:on"},
        {"DVK playback on the radio, DvkModel::playbackStart",
         [](Fixture& f) {
             applyDvkStatus(f.radio.dvkModel(), QStringLiteral("dvk"),
                            {{QStringLiteral("status"), QStringLiteral("idle")},
                             {QStringLiteral("enabled"), QStringLiteral("1")}});
             f.radio.dvkModel().playbackStart(1);
         },
         "dvk-playback:on"},
        {"NAVTEX send",
         [](Fixture& f) {
             applyNavtexStatus(f.radio.navtexModel(), QStringLiteral("navtex"),
                               {{QStringLiteral("status"), QStringLiteral("active")}});
             f.radio.navtexModel().sendMessage('A', 'B', QStringLiteral("test"));
         },
         "navtex:on"},
        {"tuner autotune relayed through the radio",
         [](Fixture& f) {
             f.radio.tunerModel().setHandle(QStringLiteral("0x00000001"));
             f.radio.tunerModel().autoTune();
         },
         "ext:tuner.autotune"},
    };
}

void testEveryPath()
{
    for (const Path& path : keyOnPaths()) {
        const std::string name = path.name;
        {
            Fixture f;
            f.objection = kObjection;
            f.addObjection();
            path.start(f);
            report(name + ": the question is asked", f.asked > 0, std::to_string(f.asked));
            report(name + ": an objection lets nothing reach the backend", f.nothingKeyed(),
                   joined(f.commands));
            report(name + ": the objection's own sentence is raised as the interlock notice",
                   f.interlockMessages.contains(kObjection), joined(f.interlockMessages));
            report(name + ": filed under a stable key-objection key, never the sentence",
                   !f.interlockKeys.filter(QStringLiteral("key-objection:")).isEmpty()
                       && f.interlockKeys.filter(kObjection).isEmpty(),
                   joined(f.interlockKeys));
            report(name + ": no transmit or tune state is left behind",
                   !f.radio.transmitModel().isTransmitting()
                       && !f.radio.transmitModel().isTuning()
                       && !f.radio.transmitModel().isMox());
        }
        {
            Fixture f;
            f.addObjection();
            path.start(f);
            const QString expected = QLatin1String(path.dispatched);
            if (expected.isEmpty()) {
                report(name + ": with nothing to object, the question is still asked",
                       f.asked > 0 && !f.interlockMessages.contains(kObjection),
                       std::to_string(f.asked));
            } else {
                report(name + ": positive control, with no objection it reaches the backend",
                       f.asked > 0 && f.commands.contains(expected)
                           && !f.interlockMessages.contains(kObjection),
                       joined(f.commands));
            }
        }
    }
}

void testUnkeyIsNeverAsked()
{
    {
        Fixture f;
        f.addObjection();
        TransmitModel& tx = f.radio.transmitModel();
        tx.setMox(true);
        report("fixture: keyed with nothing objecting",
               f.commands.contains(QStringLiteral("mox:on")) && tx.isTransmitting());

        // The objection appears during the transmission.
        f.objection = kObjection;
        f.asked = 0;
        f.commands.clear();
        tx.requestPttOn(TransmitModel::PttSource::Mox);
        f.radio.setTransmit(true);
        report("a request while this client transmits is not a key-on: nothing is asked",
               f.asked == 0, std::to_string(f.asked));
        report("the transmission on the air is not rolled back",
               tx.isTransmitting() && !f.commands.contains(QStringLiteral("mox:off"))
                   && !f.interlockMessages.contains(kObjection),
               joined(f.commands));
    }
    {
        Fixture f;
        f.addObjection();
        TransmitModel& tx = f.radio.transmitModel();
        tx.setMox(true);
        f.objection = kObjection;
        f.asked = 0;
        f.commands.clear();
        tx.setMox(false);
        report("the un-key goes through with the objection standing, and is not asked about",
               f.asked == 0 && f.commands.contains(QStringLiteral("mox:off"))
                   && !tx.isTransmitting(),
               joined(f.commands));
        f.commands.clear();
        tx.setMox(true);
        report("the next key-on is asked about and refused",
               f.asked > 0 && f.commands.isEmpty() && !tx.isTransmitting(), joined(f.commands));

        f.asked = 0;
        f.commands.clear();
        tx.setMox(false);
        tx.atuBypass();
        tx.stopTune();
        f.radio.setTransmit(false);
        f.radio.sendCwKey(false);
        f.radio.sendCwPtt(false);
        f.radio.cwxModel().clearBuffer();
        tx.setVoxEnable(false);
        f.radio.tunerModel().abortTune();
        report("key-up, bypass, VOX off, abort and clear stay available, unasked",
               f.asked == 0 && f.commands.filter(QStringLiteral(":on")).isEmpty(),
               joined(f.commands));
    }
}

void testRegistration()
{
    {
        Fixture f;
        f.radio.addKeyObjection(nullptr, [] { return kObjection; });
        f.radio.addKeyObjection(&f.owner, RadioModel::KeyObjection());
        f.radio.transmitModel().setMox(true);
        report("an objection without an owner, or an empty one, is not registered",
               f.commands.contains(QStringLiteral("mox:on")));
    }
    {
        Fixture f;
        auto owner = std::make_unique<QObject>();
        int asked = 0;
        f.radio.addKeyObjection(owner.get(), [&asked] {
            ++asked;
            return kObjection;
        });
        f.radio.transmitModel().setMox(true);
        report("fixture: an owned objection refuses", asked > 0 && f.commands.isEmpty());
        owner.reset();
        asked = 0;
        f.radio.transmitModel().setMox(true);
        report("with its owner gone the objection is gone, and is not called",
               asked == 0 && f.commands.contains(QStringLiteral("mox:on")));
    }
    {
        Fixture f;
        QObject second;
        f.objection = QStringLiteral("   ");
        f.addObjection();
        f.radio.addKeyObjection(&second, [] { return QStringLiteral(" second objection. "); });
        f.radio.transmitModel().atuStart();
        report("blank is no objection; the first one with something to say is shown, trimmed",
               f.commands.isEmpty()
                   && f.interlockMessages.contains(QStringLiteral("second objection.")),
               joined(f.interlockMessages));
    }
    {
        Fixture f;
        f.objection = kObjection;
        f.addObjection();
        f.radio.setPanTransmitInhibited(QStringLiteral("0x40000000"), true,
                                        QStringLiteral("pan inhibit"));
        f.radio.transmitModel().atuStart();
        report("the radio's own pan inhibit is reported ahead of an objection",
               f.commands.isEmpty() && f.interlockMessages.contains(QStringLiteral("pan inhibit"))
                   && !f.interlockMessages.contains(kObjection),
               joined(f.interlockMessages));
    }
}

// --- the paths that key from the device side -------------------------------

void testDvkPlayback()
{
    const auto idle = [](Fixture& f) {
        applyDvkStatus(f.radio.dvkModel(), QStringLiteral("dvk"),
                       {{QStringLiteral("status"), QStringLiteral("idle")},
                        {QStringLiteral("enabled"), QStringLiteral("1")}});
        TransmitDelta vox;   // recording needs a VOX the radio reported off
        vox.voxEnable = false;
        f.radio.transmitModel().applyChanges(vox);
    };
    Fixture f;
    idle(f);
    f.objection = kObjection;
    f.addObjection();
    QStringList failed;
    QObject::connect(&f.radio.dvkModel(), &DvkModel::commandFailed, &f.owner,
                     [&failed](const QString& verb, int, uint, const QString&) { failed << verb; });
    f.radio.dvkModel().playbackStart(1);
    report("DVK playback: refused under the dvk gate key",
           !f.interlockKeys.filter(QStringLiteral("key-objection:0x40000000:dvk")).isEmpty(),
           joined(f.interlockKeys));
    report("DVK playback: the panel is told, so the pressed button is released",
           failed.contains(QStringLiteral("playback_start")), joined(failed));
    report("DVK playback: a refused start leaves the keyer free to start again",
           f.radio.dvkModel().canStartOperation());
    f.asked = 0;
    f.commands.clear();
    f.radio.dvkModel().playbackStop(1);
    f.radio.dvkModel().previewStart(1);
    report("DVK stop and preview are not asked about, and go out",
           f.asked == 0 && f.commands.contains(QStringLiteral("dvk-playback:off"))
               && f.commands.contains(QStringLiteral("dvk-preview")),
           joined(f.commands));
    Fixture g;
    idle(g);
    g.objection = kObjection;
    g.addObjection();
    g.radio.dvkModel().recStart(1);
    report("DVK record is not asked about, and goes out",
           g.asked == 0 && g.commands.contains(QStringLiteral("dvk-record")), joined(g.commands));
}

// The direct link is made "connected" by its own greeting, fed to its line
// parser; nothing is opened.
bool connectDirectLink(TgxlConnection& link)
{
    return QMetaObject::invokeMethod(&link, "beginAttempt", Qt::DirectConnection)
           && QMetaObject::invokeMethod(&link, "processLine", Qt::DirectConnection,
                                        Q_ARG(QString, QStringLiteral("V1.2.17")))
           && link.isConnected();
}

void testTunerDirectAutotune()
{
    for (const bool objecting : {true, false}) {
        Fixture f;
        if (objecting) {
            f.objection = kObjection;
        }
        f.addObjection();
        TgxlConnection link;
        const bool linked = connectDirectLink(link);
        f.radio.tunerModel().setDirectConnection(&link);
        const quint32 before = PeripheralConnectionTestAccess::sequence(link);
        f.radio.tunerModel().autoTune();
        const quint32 sent = PeripheralConnectionTestAccess::sequence(link) - before;
        if (objecting) {
            report("tuner autotune over its own link: the question is asked",
                   linked && f.asked > 0, std::to_string(f.asked));
            report("tuner autotune over its own link: an objection sends nothing to the tuner",
                   sent == 0 && f.commands.isEmpty(), std::to_string(sent));
            report("tuner autotune over its own link: the objection is the interlock notice",
                   f.interlockMessages.contains(kObjection)
                       && !f.interlockKeys.filter(QStringLiteral("tuner-autotune")).isEmpty(),
                   joined(f.interlockKeys));
        } else {
            report("tuner autotune over its own link, positive control: one command goes out",
                   linked && f.asked > 0 && sent == 1, std::to_string(sent));
        }
        f.radio.tunerModel().setDirectConnection(nullptr);
    }
    {
        // An abort is the same verb on the relayed route, and never refused.
        Fixture f;
        f.addObjection();
        TunerModel& tuner = f.radio.tunerModel();
        tuner.setHandle(QStringLiteral("0x00000001"));
        tuner.autoTune();
        TunerDelta tuning;
        tuning.tuning = true;
        tuner.applyChanges(tuning);
        f.objection = kObjection;
        f.asked = 0;
        f.commands.clear();
        tuner.abortTune();
        report("an autotune under way is aborted with the objection standing, unasked",
               f.asked == 0 && f.commands.contains(QStringLiteral("ext:tuner.autotune")),
               joined(f.commands));
        f.commands.clear();
        tuner.autoTune();
        report("autoTune while tuning is the same abort: not asked, and sent",
               f.asked == 0 && f.commands.contains(QStringLiteral("ext:tuner.autotune")),
               joined(f.commands));
    }
}

void testVoxEnable()
{
    const QString on = QStringLiteral("vox 1 ");
    const auto watch = [](Fixture& f, QStringList& issued) {
        QObject::connect(&f.radio.transmitModel(), &TransmitModel::voxCommandIssued, &f.owner,
                         [&issued](bool voxOn, int level, int) {
            issued << QStringLiteral("vox %1 level %2").arg(voxOn ? 1 : 0).arg(level);
        });
    };
    {
        Fixture f;
        f.objection = kObjection;
        f.addObjection();
        QStringList issued;
        watch(f, issued);
        int phone = 0;
        QObject::connect(&f.radio.transmitModel(), &TransmitModel::phoneStateChanged, &f.owner,
                         [&phone]() { ++phone; });
        TransmitModel& tx = f.radio.transmitModel();
        tx.setVoxEnable(true);
        report("VOX on: the question is asked", f.asked > 0, std::to_string(f.asked));
        report("VOX on: an objection leaves VOX off and sends nothing",
               !tx.voxEnable() && issued.filter(on).isEmpty(), joined(issued));
        report("VOX on: the objection is the interlock notice, and the toggle is resynced",
               f.interlockMessages.contains(kObjection) && phone >= 1
                   && !f.interlockKeys.filter(QStringLiteral("vox-enable")).isEmpty(),
               joined(f.interlockKeys));
        f.asked = 0;
        report("VOX on with a ticket (a restore after a recording): refused, no ticket",
               tx.requestVoxEnable(true) == 0 && f.asked > 0 && !tx.voxEnable());
        f.asked = 0;
        tx.setVoxEnable(false);
        tx.setVoxLevel(40);
        report("VOX off and level are not asked about, and go out",
               f.asked == 0 && !issued.isEmpty() && issued.filter(on).isEmpty(), joined(issued));
    }
    {
        Fixture f;
        f.addObjection();
        QStringList issued;
        watch(f, issued);
        f.radio.transmitModel().setVoxEnable(true);
        report("VOX on, positive control: asked, switched on, and the command goes out",
               f.asked > 0 && f.radio.transmitModel().voxEnable() && !issued.filter(on).isEmpty(),
               joined(issued));
    }
}

// --- CW: one question per message -------------------------------------------

// The radio's interlock report, as a Flex sends it while break-in holds TX.
void radioReportsKeyed(Fixture& f, bool keyed)
{
    f.radio.handleStatusForTest(
        QStringLiteral("interlock"),
        {{QStringLiteral("state"), keyed ? QStringLiteral("TRANSMITTING") : QStringLiteral("READY")},
         {QStringLiteral("source"), QStringLiteral("SW")}});
}

void testCwOncePerMessage()
{
    {
        Fixture f;
        f.addObjection();
        f.radio.transmitModel().setCwDelay(2000);
        f.radio.sendCwKey(true);
        radioReportsKeyed(f, true);  // break-in keys on the first element
        f.radio.sendCwKey(false);
        report("fixture: the first element is asked about and keys",
               f.asked >= 1 && f.commands.count(QStringLiteral("cw:on")) == 1
                   && f.radio.isRadioTransmitting(),
               joined(f.commands));

        f.objection = kObjection;
        f.asked = 0;
        f.radio.sendCwKey(true);
        f.radio.sendCwKey(false);
        f.radio.sendCwPaddle(true, false);
        f.radio.sendCwPaddle(false, false);
        report("an objection appearing mid-message does not stop it: the elements go out",
               f.asked == 0 && f.commands.count(QStringLiteral("cw:on")) == 3
                   && !f.interlockMessages.contains(kObjection),
               joined(f.commands));

        radioReportsKeyed(f, false);  // the break-in delay elapsed: the radio un-keys
        f.commands.clear();
        f.radio.sendCwKey(true);
        report("the next message after the un-key is asked about and refused",
               f.asked > 0 && f.commands.isEmpty() && f.interlockMessages.contains(kObjection),
               joined(f.commands));
        f.radio.sendCwKey(false);
    }
    {
        // A radio that does not report keyed: the break-in delay ends the message.
        Fixture f;
        f.addObjection();
        f.radio.transmitModel().setCwDelay(2000);
        f.radio.sendCwKey(true);
        f.radio.sendCwKey(false);
        f.objection = kObjection;
        f.asked = 0;
        f.radio.sendCwKey(true);
        report("no keyed report, inside the break-in delay: the same message, not asked",
               f.asked == 0 && f.commands.count(QStringLiteral("cw:on")) == 2,
               joined(f.commands));
        f.radio.sendCwKey(false);
        f.radio.transmitModel().setCwDelay(0);
        f.commands.clear();
        f.radio.sendCwKey(true);
        report("no keyed report, the break-in delay passed: a new message, asked and refused",
               f.asked > 0 && f.commands.isEmpty(), joined(f.commands));
    }
    {
        // The radio's keyed state from before the message does not open one.
        Fixture f;
        f.objection = kObjection;
        f.addObjection();
        radioReportsKeyed(f, true);
        f.radio.sendCwKey(true);
        report("a first element is asked about even while the radio reports keyed",
               f.asked > 0 && f.commands.isEmpty(), joined(f.commands));
    }

    {
        // Another client's TX is not this client's break-in: it must not hold
        // our message open, or every later element would go out unasked.
        Fixture f;
        f.addObjection();
        f.radio.transmitModel().setCwDelay(2000);
        f.radio.sendCwKey(true);
        radioReportsKeyed(f, true);
        f.radio.sendCwKey(false);
        f.radio.handleStatusForTest(
            QStringLiteral("interlock"),
            {{QStringLiteral("state"), QStringLiteral("TRANSMITTING")},
             {QStringLiteral("tx_client_handle"), QStringLiteral("0x7F000001")},
             {QStringLiteral("source"), QStringLiteral("SW")}});
        report("fixture: the radio stays keyed, now for another client",
               f.radio.isRadioTransmitting() && !f.radio.txOwnedByUs());
        f.objection = kObjection;
        f.asked = 0;
        f.commands.clear();
        f.radio.sendCwKey(true);
        report("another client holding TX does not continue our message: asked and refused",
               f.asked > 0 && f.commands.isEmpty() && f.interlockMessages.contains(kObjection),
               joined(f.commands));
        f.radio.sendCwKey(false);
    }

    {
        // A host-keyed backend's own keyed report (the HL2's break-in key,
        // TransmitDelta::mox) ends a message the same way.
        Fixture f;
        f.addObjection();
        f.radio.transmitModel().setCwDelay(2000);
        f.radio.sendCwKey(true);
        TransmitDelta keyed;
        keyed.mox = true;
        f.radio.applyBackendTransmitDeltaForTest(keyed);
        f.radio.sendCwKey(false);
        f.objection = kObjection;
        f.asked = 0;
        f.radio.sendCwKey(true);
        f.radio.sendCwKey(false);
        report("backend keyed report: an element inside the message is not asked",
               f.asked == 0 && f.commands.count(QStringLiteral("cw:on")) == 2, joined(f.commands));
        TransmitDelta unkeyed;
        unkeyed.mox = false;
        f.radio.applyBackendTransmitDeltaForTest(unkeyed);
        f.commands.clear();
        f.radio.sendCwKey(true);
        report("backend keyed report: after its un-key the next element is asked and refused",
               f.asked > 0 && f.commands.isEmpty(), joined(f.commands));
    }
}

// --- the preflight route: pttBlocked, under the stable key ------------------

void testPreflightRoute()
{
    struct Case {
        const char* name;
        std::function<void(Fixture&)> start;
        const char* gate;
    };
    const std::vector<Case> cases{
        {"MOX", [](Fixture& f) { f.radio.transmitModel().setMox(true); }, "xmit"},
        {"PTT with the WSPR source",
         [](Fixture& f) { f.radio.transmitModel().requestPttOn(TransmitModel::PttSource::Wspr); },
         "xmit"},
        {"TUNE", [](Fixture& f) { f.radio.transmitModel().startTune(); }, "tune-start"},
    };
    for (const Case& c : cases) {
        Fixture f;
        f.objection = kObjection;
        f.addObjection();
        QStringList blocked;
        QObject::connect(&f.radio.transmitModel(), &TransmitModel::pttBlocked, &f.owner,
                         [&blocked](const QString& message) { blocked << message; });
        c.start(f);
        const QString key = QStringLiteral("key-objection:0x40000000:") + QLatin1String(c.gate);
        report(std::string(c.name) + ": refused through pttBlocked with the objection's sentence",
               blocked == QStringList{kObjection}, joined(blocked));
        report(std::string(c.name) + ": filed as " + key.toStdString() + ", not as local-ptt",
               f.interlockKeys.contains(key)
                   && f.interlockKeys.filter(QStringLiteral("local-ptt:")).isEmpty(),
               joined(f.interlockKeys));
    }
    {
        // A plain preflight refusal after an objection keeps its own key.
        Fixture f;
        f.objection = kObjection;
        f.addObjection();
        f.radio.transmitModel().setMox(true);
        f.objection.clear();
        SliceDelta digital;
        digital.mode = QStringLiteral("DIGU");
        f.radio.slice(0)->applyChanges(digital);
        f.interlockKeys.clear();
        f.radio.transmitModel().setMox(true);
        report("the radio's own preflight refusal afterwards is filed as local-ptt again",
               !f.interlockKeys.filter(QStringLiteral("local-ptt:")).isEmpty()
                   && f.interlockKeys.filter(QStringLiteral("key-objection:")).isEmpty(),
               joined(f.interlockKeys));
    }
    {
        // Q3: not asked while this client already transmits.
        Fixture f;
        f.addObjection();
        f.radio.transmitModel().setMox(true);
        f.objection = kObjection;
        f.asked = 0;
        f.radio.transmitModel().startTune();
        report("TUNE during a MOX over is not asked about",
               f.asked == 0 && !f.interlockMessages.contains(kObjection), joined(f.interlockMessages));
    }
    {
        // VOX on outlives the over it is switched on in, so it is asked even
        // then; the refusal leaves the MOX on the air as it was.
        Fixture f;
        f.addObjection();
        TransmitModel& tx = f.radio.transmitModel();
        int refused = 0;
        QObject::connect(&tx, &TransmitModel::voxEnableRefused, &f.owner,
                         [&refused](const QString&) { ++refused; });
        tx.setMox(true);
        report("fixture: MOX keyed with nothing objecting",
               f.commands.contains(QStringLiteral("mox:on")), joined(f.commands));
        const bool moxBefore = tx.isMox();
        const bool transmittingBefore = tx.isTransmitting();
        f.objection = kObjection;
        f.asked = 0;
        f.commands.clear();
        tx.setVoxEnable(true);
        report("VOX on during a MOX over is asked and refused, under vox-enable",
               f.asked > 0 && refused == 1 && !tx.voxEnable()
                   && f.interlockMessages.contains(kObjection)
                   && !f.interlockKeys.filter(QStringLiteral(":vox-enable")).isEmpty(),
               joined(f.interlockKeys));
        report("the VOX refusal during the over leaves the MOX as it was and unkeys nothing",
               tx.isMox() == moxBefore && tx.isTransmitting() == transmittingBefore
                   && f.commands.filter(QStringLiteral(":off")).isEmpty(),
               joined(f.commands));
        f.asked = 0;
        report("a VOX restore with a ticket during the over is refused too",
               tx.requestVoxEnable(true) == 0 && f.asked > 0 && !tx.voxEnable());
    }
    {
        // Positive control: the same over with nothing objecting; VOX on
        // passes the admission (what the backend then does is its own).
        Fixture f;
        f.addObjection();
        TransmitModel& tx = f.radio.transmitModel();
        int refused = 0;
        QObject::connect(&tx, &TransmitModel::voxEnableRefused, &f.owner,
                         [&refused](const QString&) { ++refused; });
        tx.setMox(true);
        f.asked = 0;
        tx.setVoxEnable(true);
        report("VOX on during a MOX over, positive control: asked, and admitted with no objection",
               f.asked > 0 && refused == 0
                   && f.interlockKeys.filter(QStringLiteral(":vox-enable")).isEmpty(),
               joined(f.interlockKeys));
    }
}

// --- VOX: an existing VOX is left alone --------------------------------------

void testExistingVoxLeftAlone()
{
    Fixture f;
    f.addObjection();
    TransmitModel& tx = f.radio.transmitModel();
    tx.setVoxEnable(true);
    report("fixture: VOX switched on with nothing objecting", tx.voxEnable());
    f.objection = kObjection;
    f.asked = 0;
    int offs = 0;
    QObject::connect(&tx, &TransmitModel::voxCommandIssued, &f.owner,
                     [&offs](bool on, int, int) { offs += on ? 0 : 1; });
    tx.setVoxLevel(55);
    tx.setVoxDelay(300);
    report("an objection appearing with VOX on leaves VOX on, unasked, and switches nothing off",
           tx.voxEnable() && f.asked == 0 && offs == 0 && !f.interlockMessages.contains(kObjection));
    tx.setVoxEnable(false);
    tx.setVoxEnable(true);
    report("once off, switching VOX on again (the WSPR beacon's restore) is refused",
           !tx.voxEnable() && f.asked > 0 && f.interlockMessages.contains(kObjection));
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("key-objection-gate"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testEveryPath();
    testUnkeyIsNeverAsked();
    testRegistration();
    testDvkPlayback();
    testTunerDirectAutotune();
    testVoxEnable();
    testCwOncePerMessage();
    testPreflightRoute();
    testExistingVoxLeftAlone();

    std::printf("\n%s (%d failed)\n", g_failed == 0 ? "ALL PASSED" : "FAILURES", g_failed);
    return g_failed == 0 ? 0 : 1;
}
