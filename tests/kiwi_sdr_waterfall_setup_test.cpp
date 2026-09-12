#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrClient.h"

#include <QCoreApplication>
#include <QDebug>

namespace AetherSDR {

// Inject only the transport boundary; production metadata handling, request
// caching and setup formatting run unchanged. No socket or firmware peer.
class KiwiSdrWaterfallSetupTest final : public KiwiSdrClient {
public:
    bool run()
    {
        using Family = KiwiSdrProtocol::KiwiSdrReceiverFamily;
        setReceiverFamily(Family::Web888);
        setWaterfallView(QStringLiteral("pan0"), 14.104, 0.2);
        sendWaterfallSetupCommands();
        const QString initialView = lastView();
        if (initialView.isEmpty()) {
            return fail("initial setup did not send a view");
        }
        commands.clear();
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG max_camp=4"));
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG freq_offset=0"));
        handleTextMessage(StreamKind::Sound, QStringLiteral("MSG wf_setup"));
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup=1"));
        if (!commands.isEmpty() || m_waterfallSetupResent) {
            return fail("a preamble, sound message or valued token consumed the replay");
        }
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (!m_waterfallSetupResent || lastView() != initialView
            || commands.count(QStringLiteral("SERVER DE CLIENT AetherSDR W/F")) != 1) {
            return fail("wf_setup did not replay the unchanged cached view");
        }
        commands.clear();
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (!commands.isEmpty()) {
            return fail("repeated marker replayed setup twice");
        }

        cleanupSockets();
        commands.clear();
        // Marker first deliberately: the replay must use metadata later in
        // this same message, not the previous connection's default scale.
        handleTextMessage(StreamKind::Waterfall, QStringLiteral(
            "MSG wf_setup center_freq=15360000 bandwidth=30720000 zoom_max=11"));
        const qsizetype greeting = commands.indexOf(
            QStringLiteral("SERVER DE CLIENT AetherSDR W/F"));
        const QString replayView = lastView();
        if (greeting < 0 || commands.lastIndexOf(replayView) <= greeting
            || m_waterfallZoomCap != 11
            || replayView != QStringLiteral("SET zoom=%1 start=%2")
                                 .arg(m_waterfallRequestZoom)
                                 .arg(m_waterfallRequestStart)
            || m_waterfallRequestStart >= (1024u << 11)) {
            return fail("reconnect replay did not use the complete metadata burst");
        }

        cleanupSockets();
        commands.clear();
        connected = false;
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (m_waterfallSetupResent || !commands.isEmpty()) {
            return fail("disconnected input consumed replay");
        }
        connected = true;
        m_monitorMode = true;
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (m_waterfallSetupResent || !commands.isEmpty()) {
            return fail("monitor input emitted setup commands");
        }
        m_monitorMode = false;
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (!m_waterfallSetupResent || lastView().isEmpty()) {
            return fail("ignored input prevented subsequent valid replay");
        }

        cleanupSockets();
        commands.clear();
        setReceiverFamily(Family::Kiwi);
        handleTextMessage(StreamKind::Waterfall, QStringLiteral("MSG wf_setup"));
        if (m_waterfallSetupResent || !commands.isEmpty()) {
            return fail("Kiwi family acquired Web-888 replay behavior");
        }
        return true;
    }

protected:
    bool waterfallTransportConnected() const override { return connected; }
    void sendWaterfallCommand(const QString& command) override
    {
        if (connected) {
            commands.append(command);
        }
    }

private:
    QString lastView() const
    {
        for (auto it = commands.crbegin(); it != commands.crend(); ++it) {
            if (it->startsWith(QLatin1String("SET zoom="))) {
                return *it;
            }
        }
        return {};
    }
    static bool fail(const char* message)
    {
        qCritical() << message;
        return false;
    }
    bool connected{true};
    QStringList commands;
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("kiwi-waterfall-setup"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    AetherSDR::KiwiSdrWaterfallSetupTest test;
    return test.run() ? 0 : 1;
}
