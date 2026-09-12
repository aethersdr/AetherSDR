// zoom_cap is not zoom_max. A KiwiSDR v1.900+ shared waterfall (rx8.wf3,
// wf_share=1) advertises "zoom_max=14" and then "zoom_cap=11" in the same
// W/F setup burst (rx/rx_waterfall.cpp: "MSG ... zoom_max=%d zoom_cap=%d ...
// wf_share=%d", MAX_ZOOM, ZOOM_CAP). The server's start fixed-point scale is
// WF_WIDTH << MAX_ZOOM (HZperStart = ui_srate_Hz / (WF_WIDTH << MAX_ZOOM));
// zoom_cap only bounds the zoom a client may request. Treating zoom_cap as
// the scale re-encoded a 14.153 MHz request as start=989353 on a 2^21 scale,
// which the server read on 2^24 as 1.769 MHz, and the frame header decoded
// with the same 2^21 scale labeled that noise as 14.153 MHz: a waterfall of
// even noise with no signals (kphsdr.com:8075, 2026-09-12, start 7914826 ->
// 989353 after "zoom_cap = 11"). Socket-free: the transport seam is injected,
// production metadata handling and request encoding run unchanged.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrClient.h"
#include "core/KiwiSdrProtocol.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QVector>

#include <cmath>

namespace AetherSDR {

namespace {

// The reporter's session: a 200 kHz view centered on 14.270 MHz over the
// KiwiSDR's 0-30 MHz full band. Zoom 7 is the narrowest row (30 MHz / 2^7 =
// 234.375 kHz) that covers 200 kHz; the row is centered on the view.
constexpr double kFullLowMhz = 0.0;
constexpr double kFullBandwidthMhz = 30.0;
constexpr double kViewCenterMhz = 14.27;
constexpr double kViewBandwidthMhz = 0.2;
constexpr int kExpectedZoom = 7;
constexpr double kRowSpanMhz = kFullBandwidthMhz / (1 << kExpectedZoom);
constexpr double kExpectedRowLowMhz = kViewCenterMhz - kRowSpanMhz * 0.5;
// The start the server echoed back in the 2026-09-12 log for this view:
// round((14.1528125 / 30) * 2^24).
constexpr quint32 kLoggedStart = 7914826u;
// A 1 kHz view wants the finest row; uncapped that is zoom 14.
constexpr double kNarrowViewBandwidthMhz = 0.001;
// KiwiSDR W/F payload bytes encode dBm + 255; 155 is -100 dBm, a quiet floor.
constexpr char kQuietFloorByte = static_cast<char>(155);

double rowSpanMhz(double fullBandwidthMhz, int zoom)
{
    return fullBandwidthMhz / static_cast<double>(1 << zoom);
}

} // namespace

class KiwiSdrWaterfallZoomCapTest final : public KiwiSdrClient {
public:
    bool run()
    {
        using namespace KiwiSdrProtocol;
        const double kiwiScale = waterfallStartFixedPointScale(14);
        const double cappedScale = waterfallStartFixedPointScale(11);
        if (kiwiScale != 16777216.0 || cappedScale != 2097152.0) {
            return fail("scale helper does not match 2^24 / 2^21");
        }
        // What the pre-fix client sent for the same view once zoom_cap=11
        // had replaced the scale: the value observed on the wire.
        const quint32 wrongScaleStart = waterfallStartFixedPoint(
            kFullLowMhz, kFullBandwidthMhz, kExpectedRowLowMhz, cappedScale);
        if (wrongScaleStart != 989353u) {
            qCritical() << "2^21 start" << wrongScaleStart;
            return fail("2^21 encoding of the reporter's view is not 989353");
        }

        setReceiverFamily(KiwiSdrReceiverFamily::Kiwi);
        setWaterfallView(QStringLiteral("pan0"), kViewCenterMhz,
                         kViewBandwidthMhz);
        sendWaterfallSetupCommands();
        if (m_waterfallRequestZoom != kExpectedZoom
            || m_waterfallRequestStart != kLoggedStart
            || zoomLines().size() != 1
            || zoomLines().first() != viewLine(kExpectedZoom, kLoggedStart)) {
            qCritical() << "initial" << zoomLines();
            return fail("initial setup did not request zoom 7 at the logged start");
        }

        // The live v1.900 burst, token order preserved: zoom_max first,
        // zoom_cap after it, both in one MSG.
        commands.clear();
        handleTextMessage(StreamKind::Waterfall, QStringLiteral(
            "MSG center_freq=15000000 bandwidth=30000000 adc_clk_nom=66666600"));
        commands.clear();
        handleTextMessage(StreamKind::Waterfall, QStringLiteral(
            "MSG wf_fft_size=1024 wf_fps=23 wf_fps_max=23 zoom_max=14 "
            "zoom_cap=11 rx_chans=8 wf_chans=3 wf_chans_real=3 wf_share=1 "
            "wf_cal=-11 wf_setup"));
        if (m_waterfallZoomMax != 14 || m_waterfallZoomCap != 11
            || !m_waterfallZoomCapFromServer) {
            return fail("zoom_max/zoom_cap were not recorded separately");
        }
        if (m_waterfallRequestZoom != kExpectedZoom
            || m_waterfallRequestStart != kLoggedStart) {
            qCritical() << "start after zoom_cap:" << m_waterfallRequestStart
                        << "expected" << kLoggedStart;
            return fail("zoom_cap=11 re-encoded the start on the 2^21 scale");
        }
        // Nothing was re-sent: zoom_max matched the scale already in use and
        // zoom_cap did not change the selected zoom, so the request the
        // server already holds (zoom 7 at kLoggedStart) stands.
        if (!zoomLines().isEmpty()) {
            qCritical() << "burst" << zoomLines();
            return fail("the burst re-sent a request although zoom/start did not change");
        }

        // A 1040-byte frame whose 16-byte extended header echoes our start
        // (x_bin_server, zoom 7) must decode on the 2^24 scale too.
        connect(this, &KiwiSdrClient::waterfallRowReady, this,
                [this](const QString&, const QVector<float>&, double low,
                       double high, quint32) {
                    ++rowsSeen;
                    rowLowMhz = low;
                    rowHighMhz = high;
                });
        if (!feedFrame(kLoggedStart, kExpectedZoom)) {
            return fail("no row emitted synchronously for the first frame");
        }
        if (std::fabs(rowLowMhz - kExpectedRowLowMhz) > 1.0e-6
            || std::fabs((rowHighMhz - rowLowMhz) - kRowSpanMhz) > 1.0e-6) {
            qCritical() << "row" << rowLowMhz << rowHighMhz;
            return fail("frame header start decoded on the wrong scale");
        }

        // zoom_cap still caps what we request: a 1 kHz view wants zoom 14
        // on an uncapped server but must stop at 11 here, and the capped
        // row must still cover the view.
        commands.clear();
        setWaterfallView(QStringLiteral("pan0"), kViewCenterMhz,
                         kNarrowViewBandwidthMhz);
        if (m_waterfallRequestZoom != 11 || zoomLines().size() != 1
            || zoomLines().first() != viewLine(11, m_waterfallRequestStart)) {
            qCritical() << "zoom" << m_waterfallRequestZoom << zoomLines();
            return fail("zoom_cap did not bound the requested zoom");
        }
        if (!wireStartCovers(kiwiScale, kFullLowMhz, kFullBandwidthMhz, 11,
                             kViewCenterMhz, kNarrowViewBandwidthMhz)) {
            return fail("capped zoom-11 window does not cover the view");
        }

        // Token order and cap/max relation must not matter. Each burst starts
        // from a fresh connection with the narrow view already requested on
        // the wire; the effective ceiling is min(cap, max), the scale is
        // always zoom_max, and a request is re-sent only when it changed.
        struct Burst {
            const char* message;
            int zoomMax;
            int zoomCap;
            int expectedZoom;
            int expectedLines;
        };
        const Burst bursts[] = {
            {"MSG zoom_max=14 zoom_cap=11", 14, 11, 11, 1},
            {"MSG zoom_cap=11 zoom_max=14", 14, 11, 11, 1},
            {"MSG zoom_max=11 zoom_cap=14", 11, 14, 11, 1},
            {"MSG zoom_max=14", 14, 14, 14, 0},
        };
        for (const Burst& burst : bursts) {
            if (!beginNarrowSession()) {
                return fail("connection reset did not restore defaults");
            }
            handleTextMessage(StreamKind::Waterfall,
                              QString::fromLatin1(burst.message));
            const double scale = waterfallStartFixedPointScale(burst.zoomMax);
            const QStringList lines = zoomLines();
            bool linesMatch = lines.size() == burst.expectedLines;
            for (const QString& line : lines) {
                linesMatch = linesMatch
                    && line == viewLine(burst.expectedZoom,
                                        m_waterfallRequestStart);
            }
            if (m_waterfallZoomMax != burst.zoomMax
                || m_waterfallZoomCap != burst.zoomCap
                || m_waterfallRequestZoom != burst.expectedZoom
                || !linesMatch
                || !wireStartCovers(scale, kFullLowMhz, kFullBandwidthMhz,
                                    burst.expectedZoom, kViewCenterMhz,
                                    kNarrowViewBandwidthMhz)) {
                qCritical() << burst.message << "max" << m_waterfallZoomMax
                            << "cap" << m_waterfallZoomCap << "zoom"
                            << m_waterfallRequestZoom << "start"
                            << m_waterfallRequestStart << lines;
                return fail("burst order or cap/max relation mishandled");
            }
        }

        // The frame-header zoom gate uses the same effective ceiling as the
        // request. A header at the ceiling is accepted and decoded on the
        // zoom_max scale; one above it is not trusted, and since a 1040-byte
        // frame without its 16-byte header has no 1024-byte payload the
        // frame is dropped (no row). The header start is offset from the
        // request so an accepted row is distinguishable from the request.
        struct Gate {
            const char* message;
            int zoomMax;
            int ceiling;
        };
        const Gate gates[] = {
            {"MSG zoom_max=14 zoom_cap=11", 14, 11},
            {"MSG zoom_max=11 zoom_cap=14", 11, 11},
        };
        for (const Gate& gate : gates) {
            if (!beginNarrowSession()) {
                return fail("connection reset did not restore defaults");
            }
            handleTextMessage(StreamKind::Waterfall,
                              QString::fromLatin1(gate.message));
            const double scale = waterfallStartFixedPointScale(gate.zoomMax);
            const quint32 offsetStart = m_waterfallRequestStart + 8192u;
            const double offsetLowMhz = waterfallStartFixedPointToLowMhz(
                kFullLowMhz, kFullBandwidthMhz, offsetStart, scale);
            if (!feedFrame(offsetStart, gate.ceiling)
                || std::fabs(rowLowMhz - offsetLowMhz) > 1.0e-6
                || std::fabs((rowHighMhz - rowLowMhz)
                             - rowSpanMhz(kFullBandwidthMhz, gate.ceiling))
                    > 1.0e-6) {
                qCritical() << gate.message << "row" << rowLowMhz << rowHighMhz
                            << "expected low" << offsetLowMhz;
                return fail("header at the effective ceiling was not decoded on zoom_max");
            }
            if (feedFrame(offsetStart, gate.ceiling + 1)) {
                qCritical() << gate.message << "row" << rowLowMhz << rowHighMhz;
                return fail("header above the effective ceiling was accepted");
            }
        }

        // Reconnecting to a server that stops sending zoom_cap must release
        // the old ceiling: the 1 kHz view goes back to zoom 14.
        if (!resetConnectionState()) {
            return fail("connection reset did not restore defaults");
        }
        handleTextMessage(StreamKind::Waterfall,
                          QStringLiteral("MSG zoom_max=14 zoom_cap=11"));
        cleanupSockets();
        commands.clear();
        handleTextMessage(StreamKind::Waterfall,
                          QStringLiteral("MSG zoom_max=14 wf_setup"));
        if (m_waterfallZoomCapFromServer || m_waterfallZoomCap != 14
            || m_waterfallRequestZoom != 14
            || zoomLines() != QStringList{viewLine(14, m_waterfallRequestStart)}) {
            qCritical() << "cap" << m_waterfallZoomCap << "zoom"
                        << m_waterfallRequestZoom << zoomLines();
            return fail("a stale zoom_cap survived socket teardown");
        }

        // Non-finite zoom values are ignored rather than cast.
        commands.clear();
        handleTextMessage(StreamKind::Waterfall,
                          QStringLiteral("MSG zoom_max=nan zoom_cap=inf"));
        if (m_waterfallZoomMax != 14 || m_waterfallZoomCap != 14
            || !zoomLines().isEmpty()) {
            return fail("non-finite zoom values changed the zoom state");
        }

        // Web-888 regression: zoom_max=11 alone sets both the scale and the
        // ceiling to 11, so its 2^21 encoding is unchanged.
        if (!resetConnectionState()) {
            return fail("connection reset did not restore defaults");
        }
        setReceiverFamily(KiwiSdrReceiverFamily::Web888);
        setWaterfallView(QStringLiteral("pan0"), 14.104, kViewBandwidthMhz);
        commands.clear();
        handleTextMessage(StreamKind::Waterfall, QStringLiteral(
            "MSG center_freq=15360000 bandwidth=30720000 zoom_max=11"));
        if (m_waterfallZoomMax != 11 || m_waterfallZoomCap != 11
            || m_waterfallZoomCapFromServer
            || m_waterfallRequestZoom != 7
            || !wireStartCovers(cappedScale, 0.0, 30.72, 7, 14.104,
                                kViewBandwidthMhz)
            || zoomLines().isEmpty()
            || zoomLines().last() != viewLine(7, m_waterfallRequestStart)) {
            qCritical() << "web888" << m_waterfallRequestZoom
                        << m_waterfallRequestStart << zoomLines();
            return fail("zoom_max=11 alone no longer drives the 2^21 scale");
        }
        return true;
    }

protected:
    bool waterfallTransportConnected() const override { return true; }
    void sendWaterfallCommand(const QString& command) override
    {
        commands.append(command);
    }

private:
    // connectToEndpoint() is the production per-connection reset; it only
    // leaves a status preflight pending, which this test never services.
    bool resetConnectionState()
    {
        connectToEndpoint(QStringLiteral("example.invalid:8073"), QString());
        commands.clear();
        return m_waterfallZoomMax == 14 && m_waterfallZoomCap == 14
            && !m_waterfallZoomCapFromServer && !m_waterfallRequestValid;
    }
    // Fresh connection with the narrow view already requested on the wire,
    // so a burst's SET zoom lines are exactly the re-sends it caused.
    bool beginNarrowSession()
    {
        if (!resetConnectionState()) {
            return false;
        }
        setWaterfallView(QStringLiteral("pan0"), kViewCenterMhz,
                         kNarrowViewBandwidthMhz);
        sendWaterfallViewToServer();
        commands.clear();
        return m_waterfallRequestValid;
    }
    // Decode the start on the wire with the scale the server uses and check
    // that the row it names covers the view. Unlike the client's own
    // request bounds this is not self-consistent: a start encoded on the
    // wrong scale decodes to the wrong band.
    bool wireStartCovers(double scale, double fullLowMhz,
                         double fullBandwidthMhz, int zoom,
                         double centerMhz, double bandwidthMhz) const
    {
        const double lowMhz = KiwiSdrProtocol::waterfallStartFixedPointToLowMhz(
            fullLowMhz, fullBandwidthMhz, m_waterfallRequestStart, scale);
        const double highMhz = lowMhz + rowSpanMhz(fullBandwidthMhz, zoom);
        return std::fabs(lowMhz - m_waterfallRequestLowMhz) <= 1.0e-9
            && lowMhz <= centerMhz - bandwidthMhz * 0.5 + 1.0e-9
            && highMhz >= centerMhz + bandwidthMhz * 0.5 - 1.0e-9;
    }
    // Feed one 1040-byte direct-bin frame with an extended header. No row
    // has been emitted since the gate was cleared: queueWaterfallRow passes a
    // row straight through while m_lastWaterfallRowEmitUtcMs is 0 (only
    // connectToEndpoint/cleanupSockets reset it), so the emit is synchronous.
    bool feedFrame(quint32 start, int zoom)
    {
        QByteArray frame("W/F ", 4);
        appendLittleEndianU32(frame, start);
        appendLittleEndianU32(frame, static_cast<quint32>(zoom));
        appendLittleEndianU32(frame, 1u);
        frame.append(QByteArray(1024, kQuietFloorByte));
        m_lastWaterfallRowEmitUtcMs = 0;
        const int before = rowsSeen;
        rowLowMhz = -1.0;
        rowHighMhz = -1.0;
        handleWaterfallFrame(frame);
        return rowsSeen == before + 1;
    }
    static QString viewLine(int zoom, quint32 start)
    {
        return QStringLiteral("SET zoom=%1 start=%2").arg(zoom).arg(start);
    }
    static void appendLittleEndianU32(QByteArray& out, quint32 value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            out.append(static_cast<char>((value >> shift) & 0xffu));
        }
    }
    QStringList zoomLines() const
    {
        QStringList lines;
        for (const QString& command : commands) {
            if (command.startsWith(QLatin1String("SET zoom="))) {
                lines.append(command);
            }
        }
        return lines;
    }
    static bool fail(const char* message)
    {
        qCritical() << message;
        return false;
    }
    QStringList commands;
    int rowsSeen{0};
    double rowLowMhz{-1.0};
    double rowHighMhz{-1.0};
};

} // namespace AetherSDR

int main(int argc, char** argv)
{
    TestSettingsProfile settings(QStringLiteral("kiwi-waterfall-zoom-cap"));
    if (!settings.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();
    AetherSDR::KiwiSdrWaterfallZoomCapTest test;
    return test.run() ? 0 : 1;
}
