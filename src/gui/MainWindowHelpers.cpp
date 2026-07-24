#include "MainWindowHelpers.h"

#include "SpectrumWidget.h"
#include "core/PanadapterStream.h"
#include "core/RadioDiscovery.h"
#include "core/SmartLinkClient.h"
#include "models/BandSettings.h"
#include "models/MemoryEntry.h"
#include "models/RadioModel.h"
#include "models/XvtrPolicy.h"
#include "models/TnfModel.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
#include <QMap>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

// ─── Platform checks ─────────────────────────────────────────────────────────

bool macDaxDriverInstalled()
{
#ifdef Q_OS_MAC
    const QFileInfo driverBundle("/Library/Audio/Plug-Ins/HAL/AetherSDRDAX.driver");
    if (!driverBundle.exists() || !driverBundle.isDir())
        return false;

    const QString bundlePath = driverBundle.absoluteFilePath();
    const QFileInfo driverExec(bundlePath + "/Contents/MacOS/AetherSDRDAX");
    const QFileInfo infoPlist(bundlePath + "/Contents/Info.plist");
    return driverExec.exists() && driverExec.isFile() && infoPlist.exists() && infoPlist.isFile();
#else
    return true;
#endif
}

// ─── Network diagnostics tooltip ─────────────────────────────────────────────

QString formatNetworkMs(int ms)
{
    return ms < 1 ? "< 1 ms" : QString("%1 ms").arg(ms);
}

QString formatNetworkSeqErrors(int errors, int packets)
{
    if (packets == 0) {
        return "0 / 0 packets";
    }

    const double pct = (errors * 100.0) / packets;
    return QString("%1 / %2 packets (%3%)")
        .arg(errors)
        .arg(packets)
        .arg(pct, 0, 'f', 2);
}

namespace {
constexpr int kNetworkTooltipWidthPx = 300;

// Distinct name (not an overload of formatNetworkSeqErrors): an
// anonymous-namespace overload would hide the namespace-scope two-int
// overload from unqualified lookup inside this TU.
QString formatCategorySeqErrors(const PanadapterStream::CategoryStats& stats)
{
    return formatNetworkSeqErrors(stats.errors, stats.packets);
}

QString networkTooltipSection(const QString& title)
{
    return QStringLiteral(
        "<tr><td colspan='2' style='color:#8aa8c0; font-size:8pt; padding-top:5px;'>"
        "%1"
        "</td></tr>")
        .arg(title.toHtmlEscaped());
}

QString networkTooltipRow(const QString& label, const QString& value)
{
    return QStringLiteral(
        "<tr>"
        "<td width='52%' style='color:#8aa8c0; padding-right:18px;'>%1</td>"
        "<td width='48%' align='right' style='color:#c8d8e8;'>%2</td>"
        "</tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}
} // namespace

QString networkQualityColor(const QString& quality)
{
    if (quality == QStringLiteral("Excellent")
            || quality == QStringLiteral("Very Good")) {
        return QStringLiteral("#00cc66");
    }
    if (quality == QStringLiteral("Fair")) {
        return QStringLiteral("#cc9900");
    }
    if (quality == QStringLiteral("Poor")) {
        return QStringLiteral("#cc3333");
    }
    if (quality == QStringLiteral("Good")) {
        return QStringLiteral("#00b4d8");
    }
    return QStringLiteral("#8aa8c0"); // Off / unknown
}

QString buildNetworkTooltip(const RadioModel& model,
                            int adaptiveFpsCap,
                            bool throttleRestorePending)
{
    const PanadapterStream::CategoryStats audioStats =
        model.categoryStats(PanadapterStream::CatAudio);
    const PanadapterStream::CategoryStats fftStats =
        model.categoryStats(PanadapterStream::CatFFT);
    const PanadapterStream::CategoryStats waterfallStats =
        model.categoryStats(PanadapterStream::CatWaterfall);
    const PanadapterStream::CategoryStats meterStats =
        model.categoryStats(PanadapterStream::CatMeter);
    const PanadapterStream::CategoryStats daxStats =
        model.categoryStats(PanadapterStream::CatDAX);

    const QString quality = model.networkQuality();
    QString html = QStringLiteral(
        "<html><body>"
        // Dark background so the fixed light foreground colors below stay
        // legible regardless of the platform/theme tooltip base color (the
        // status-bar tooltip has no global QToolTip palette). Matches the
        // pairing used in PropDashboardDialog.
        "<table width='%1' bgcolor='#102131' cellspacing='0' cellpadding='3'>"
        "<tr>"
        "<td colspan='2'>"
        "<span style='font-size:10pt; font-weight:600; color:#c8d8e8;'>"
        "Network diagnostics"
        "</span>&nbsp;&nbsp;"
        "<span style='color:%2;'>&#9679; %3</span>"
        "</td>"
        "</tr>")
        .arg(kNetworkTooltipWidthPx)
        .arg(networkQualityColor(quality), quality.toHtmlEscaped());

    if (adaptiveFpsCap > 0) {
        const QString throttleStatus = throttleRestorePending
            ? QStringLiteral("Adaptive throttle: %1 fps cap · restoring shortly")
                  .arg(adaptiveFpsCap)
            : QStringLiteral("Adaptive throttle: %1 fps cap").arg(adaptiveFpsCap);
        html += QStringLiteral(
            "<tr><td colspan='2' style='color:#ffc000;'>%1</td></tr>")
                    .arg(throttleStatus.toHtmlEscaped());
    }

    html += networkTooltipSection(QStringLiteral("LINK TIMING"));
    html += networkTooltipRow(QStringLiteral("Current RTT"),
                              formatNetworkMs(model.lastPingRtt()));
    html += networkTooltipRow(QStringLiteral("Session maximum"),
                              formatNetworkMs(model.maxPingRtt()));
    html += networkTooltipRow(QStringLiteral("Network jitter"),
                              formatNetworkMs(model.audioPacketJitterMs()));
    html += networkTooltipRow(
        QStringLiteral("Audio gap"),
        QStringLiteral("%1 · max %2")
            .arg(formatNetworkMs(model.audioPacketGapMs()),
                 formatNetworkMs(model.audioPacketGapMaxMs())));

    html += networkTooltipSection(QStringLiteral("PACKET INTEGRITY"));
    html += networkTooltipRow(
        QStringLiteral("Recent loss (%1 s)").arg(model.packetLossWindowSeconds()),
        formatNetworkSeqErrors(model.packetLossWindowDrops(),
                               model.packetLossWindowPackets()));
    html += networkTooltipRow(
        QStringLiteral("Session sequence gaps"),
        formatNetworkSeqErrors(model.packetDropCount(), model.packetTotalCount()));

    html += networkTooltipSection(QStringLiteral("STREAM SEQUENCE GAPS"));
    html += networkTooltipRow(QStringLiteral("Audio"), formatCategorySeqErrors(audioStats));
    html += networkTooltipRow(QStringLiteral("FFT"), formatCategorySeqErrors(fftStats));
    html += networkTooltipRow(QStringLiteral("Waterfall"),
                              formatCategorySeqErrors(waterfallStats));
    html += networkTooltipRow(QStringLiteral("Meters"), formatCategorySeqErrors(meterStats));
    html += networkTooltipRow(QStringLiteral("DAX"), formatCategorySeqErrors(daxStats));

    html += networkTooltipSection(QStringLiteral("UDP TRAFFIC"));
    html += networkTooltipRow(QStringLiteral("Received"),
                              QLocale().formattedDataSize(model.rxBytes()));
    html += networkTooltipRow(QStringLiteral("Sent"),
                              QLocale().formattedDataSize(model.txBytes()));
    html += QStringLiteral(
        "<tr>"
        "<td colspan='2' style='color:#8aa8c0; padding-top:7px;'>"
        "Double-click to open full diagnostics"
        "</td>"
        "</tr>"
        "</table></body></html>");
    return html;
}

// ─── TNF tooltip ─────────────────────────────────────────────────────────────

long long tnfFrequencyHz(double freqMhz)
{
    return static_cast<long long>(std::llround(freqMhz * 1.0e6));
}

QString formatTnfFrequency(double freqMhz)
{
    const long long hz = tnfFrequencyHz(freqMhz);
    const int mhzPart = static_cast<int>(hz / 1000000);
    const int khzPart = static_cast<int>((hz / 1000) % 1000);
    const int hzPart = static_cast<int>(hz % 1000);
    return QStringLiteral("%1.%2.%3")
        .arg(mhzPart)
        .arg(khzPart, 3, 10, QChar('0'))
        .arg(hzPart, 3, 10, QChar('0'));
}

QString formatTnfDepth(int depthDb)
{
    switch (std::clamp(depthDb, 1, 3)) {
    case 1:
        return QStringLiteral("Normal");
    case 2:
        return QStringLiteral("Deep");
    case 3:
        return QStringLiteral("Very Deep");
    default:
        return QStringLiteral("Normal");
    }
}

QString buildTnfTooltip(const TnfModel& tnfModel)
{
    QString html = QStringLiteral(
        "<html><body style='white-space:nowrap;'>"
        "<div style='font-size:10pt; font-weight:600; color:#c8d8e8; margin-bottom:5px;'>"
        "Tracking Notch Filters — click to toggle"
        "</div>");

    if (tnfModel.tnfs().isEmpty()) {
        html += QStringLiteral(
            "<div style='color:#8aa8c0;'>No TNF filters exist.</div>"
            "</body></html>");
        return html;
    }

    QVector<TnfEntry> filters;
    filters.reserve(tnfModel.tnfs().size());
    for (const TnfEntry& tnf : tnfModel.tnfs()) {
        filters.append(tnf);
    }
    std::sort(filters.begin(), filters.end(), [](const TnfEntry& lhs, const TnfEntry& rhs) {
        const long long lhsHz = tnfFrequencyHz(lhs.freqMhz);
        const long long rhsHz = tnfFrequencyHz(rhs.freqMhz);
        if (lhsHz != rhsHz) {
            return lhsHz < rhsHz;
        }
        return lhs.id < rhs.id;
    });

    html += QStringLiteral(
        "<table cellspacing='0' cellpadding='3'>"
        "<tr style='color:#8aa8c0; font-size:8pt;'>"
        "<th align='left'>Band</th>"
        "<th align='left'>Frequency</th>"
        "<th align='right'>Width</th>"
        "<th align='left'>Depth</th>"
        "<th align='left'>State</th>"
        "</tr>");

    for (const TnfEntry& tnf : filters) {
        const QString band = BandSettings::bandForFrequency(tnf.freqMhz).toHtmlEscaped();
        const QString frequency = formatTnfFrequency(tnf.freqMhz).toHtmlEscaped();
        const QString width = QStringLiteral("%1 Hz").arg(tnf.widthHz).toHtmlEscaped();
        const QString depth = formatTnfDepth(tnf.depthDb).toHtmlEscaped();
        const QString state = tnf.permanent
            ? QStringLiteral("Persistent")
            : QStringLiteral("Temporary");
        const QString stateColor = tnf.permanent
            ? QStringLiteral("#30c030")
            : QStringLiteral("#ffc000");

        html += QStringLiteral(
            "<tr>"
            "<td style='color:#c8d8e8;'>%1</td>"
            "<td style='color:#c8d8e8;'>%2 MHz</td>"
            "<td align='right' style='color:#c8d8e8;'>%3</td>"
            "<td style='color:#c8d8e8;'>%4</td>"
            "<td style='color:%5;'>&#9679; %6</td>"
            "</tr>")
            .arg(band, frequency, width, depth, stateColor, state);
    }

    html += QStringLiteral("</table></body></html>");
    return html;
}

// ─── Memory / passive spot ID math ───────────────────────────────────────────

int memorySpotId(int memoryIndex)
{
    return -(kMemorySpotIdBase + memoryIndex);
}

int memoryIndexFromSpotId(int spotIndex)
{
    if (spotIndex > -kMemorySpotIdBase)
        return -1;
    return -spotIndex - kMemorySpotIdBase;
}

bool isPassiveLocalSpotId(int spotIndex)
{
    return spotIndex <= -kPassiveSpotIdBase;
}

QString memorySpotLabel(const MemoryEntry& memory)
{
    if (!memory.name.trimmed().isEmpty())
        return memory.name.trimmed();
    if (!memory.group.trimmed().isEmpty())
        return memory.group.trimmed();
    return QString("Memory %1").arg(memory.index);
}

QString memorySpotComment(const MemoryEntry& memory)
{
    QStringList parts;
    if (!memory.group.trimmed().isEmpty())
        parts << QString("Group: %1").arg(memory.group.trimmed());
    if (!memory.owner.trimmed().isEmpty())
        parts << QString("Owner: %1").arg(memory.owner.trimmed());
    if (!memory.mode.trimmed().isEmpty())
        parts << QString("Mode: %1").arg(memory.mode.trimmed());
    if (memory.rxFilterLow != 0 || memory.rxFilterHigh != 0) {
        parts << QString("Filter: %1..%2 Hz")
                    .arg(memory.rxFilterLow)
                    .arg(memory.rxFilterHigh);
    }
    return parts.join(" | ");
}

// ─── Pan layout ──────────────────────────────────────────────────────────────

int panCountForLayoutId(const QString& layoutId)
{
    static const QMap<QString, int> kPanCounts = {
        {"1", 1}, {"2v", 2}, {"2h", 2}, {"2h1", 3}, {"12h", 3}, {"3v", 3},
        {"2x2", 4}, {"4v", 4}, {"3h2", 5}, {"2x3", 6}, {"4h3", 7}, {"2x4", 8}
    };
    return kPanCounts.value(layoutId, 1);
}

// ─── XVTR policy summaries (diagnostics) ────────────────────────────────────

QVector<XvtrPolicy::Transverter> xvtrPolicyBandsFrom(
    const QMap<int, RadioModel::XvtrInfo>& xvtrs)
{
    QVector<XvtrPolicy::Transverter> bands;
    bands.reserve(xvtrs.size());
    for (auto it = xvtrs.cbegin(); it != xvtrs.cend(); ++it) {
        const auto& x = it.value();
        bands.append({x.index, x.order, x.name, x.rfFreq, x.ifFreq, x.isValid});
    }
    return bands;
}

QString xvtrSummary(const XvtrPolicy::Transverter& xvtr)
{
    return QStringLiteral("%1[idx=%2 order=%3 valid=%4 rf=%5 if=%6 offset=%7]")
        .arg(xvtr.name.isEmpty() ? QStringLiteral("(unnamed)") : xvtr.name)
        .arg(xvtr.index)
        .arg(xvtr.order)
        .arg(xvtr.isValid ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(xvtr.rfFreqMhz, 0, 'f', 6)
        .arg(xvtr.ifFreqMhz, 0, 'f', 6)
        .arg(xvtr.rfFreqMhz - xvtr.ifFreqMhz, 0, 'f', 6);
}

QString xvtrListSummary(const QVector<XvtrPolicy::Transverter>& xvtrs)
{
    QStringList entries;
    entries.reserve(xvtrs.size());
    for (const auto& xvtr : xvtrs)
        entries << xvtrSummary(xvtr);
    return entries.isEmpty() ? QStringLiteral("(none)") : entries.join(QStringLiteral("; "));
}

QString xvtrForBandSummary(const QString& bandName,
                           const QVector<XvtrPolicy::Transverter>& xvtrs)
{
    for (const auto& xvtr : xvtrs) {
        if (xvtr.name == bandName)
            return xvtrSummary(xvtr);
    }
    return QStringLiteral("(none)");
}

// ─── Pan pixel dimensions ────────────────────────────────────────────────────

namespace {
constexpr int kDefaultPanXpixels = 1024;
constexpr int kDefaultPanYpixels = 700;
constexpr int kMinPanXpixels = 100;
constexpr int kMinPanYpixels = 20;
// FLEX radios cap panadapter FFT frames at a 4096-bin boundary. Requesting
// that boundary (or more) leaves the right edge malformed on firmware
// 4.2.18/4.2.20, so stay one bin below it.
constexpr int kMaxRadioPanXPixels = 4095;
constexpr int kMaxRadioPanYPixels = 8192;

int radioPixelsFor(const SpectrumWidget* spectrum, int logicalPixels, int maximumPixels)
{
    const double ratio = spectrum ? spectrum->devicePixelRatioF() : 1.0;
    const double dpr = std::isfinite(ratio) && ratio > 0.0 ? ratio : 1.0;
    return std::clamp(static_cast<int>(std::lround(logicalPixels * dpr)),
                      1,
                      maximumPixels);
}
} // namespace

int panXpixelsFor(const SpectrumWidget* spectrum)
{
    if (!spectrum || spectrum->width() < kMinPanXpixels) {
        return kDefaultPanXpixels;
    }
    return radioPixelsFor(spectrum, spectrum->width(), kMaxRadioPanXPixels);
}

int panYpixelsFor(const SpectrumWidget* spectrum)
{
    if (!spectrum) {
        return kDefaultPanYpixels;
    }

    const int ypix = spectrum->spectrumPixelHeight();
    if (ypix < kMinPanYpixels) {
        return kDefaultPanYpixels;
    }
    // FFT bins arrive as radio-encoded pixel positions, not full-precision dBm
    // samples. Request render-device pixels and keep the historical 700 px
    // floor so zoomed traces have sub-screen-pixel precision.
    return std::max(radioPixelsFor(spectrum, ypix, kMaxRadioPanYPixels),
                    kDefaultPanYpixels);
}

bool panPixelDimensionsReady(const SpectrumWidget* spectrum)
{
    return spectrum
        && spectrum->width() >= kMinPanXpixels
        && spectrum->spectrumPixelHeight() >= kMinPanYpixels;
}

// ─── Misc UI ─────────────────────────────────────────────────────────────────

QPixmap buildBandStackIndicatorPixmap(bool active)
{
    QPixmap pixmap(10, 22);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(active ? QColor(0x00, 0xb4, 0xd8) : QColor(0x40, 0x48, 0x58));
    painter.drawEllipse(2, 1, 6, 6);
    painter.drawEllipse(2, 8, 6, 6);
    painter.drawEllipse(2, 15, 6, 6);
    return pixmap;
}

QKeySequence shortcutSequenceFromKeyEvent(const QKeyEvent* ev)
{
    if (!ev || ev->key() == Qt::Key_unknown)
        return {};

    const Qt::KeyboardModifiers modifiers =
        ev->modifiers() & (Qt::ShiftModifier
                           | Qt::ControlModifier
                           | Qt::AltModifier
                           | Qt::MetaModifier);
    return QKeySequence(static_cast<int>(modifiers) | ev->key());
}

// ─── Client connection parsing (discovery / multiFLEX) ──────────────────────

QStringList splitClientField(const QString& raw)
{
    QString cleaned = raw;
    cleaned.replace(QChar(0x7f), QLatin1Char(' '));

    QStringList values;
    for (const QString& value : cleaned.split(',', Qt::SkipEmptyParts))
        values << value.trimmed();
    return values;
}

quint32 parseClientHandle(QString text)
{
    text = text.trimmed();
    if (text.startsWith("0x", Qt::CaseInsensitive))
        text = text.mid(2);

    bool ok = false;
    const quint32 handle = text.toUInt(&ok, 16);
    return ok ? handle : 0;
}

QList<ClientDisconnectDialog::Client> buildDisconnectClients(const QStringList& handles,
                                                             const QStringList& programs,
                                                             const QStringList& stations)
{
    QList<ClientDisconnectDialog::Client> clients;
    for (int i = 0; i < handles.size(); ++i) {
        const quint32 handle = parseClientHandle(handles[i]);
        if (handle == 0)
            continue;

        if (std::any_of(clients.cbegin(), clients.cend(), [handle](const auto& client) {
                return client.handle == handle;
            })) {
            continue;
        }

        ClientDisconnectDialog::Client client;
        client.handle = handle;
        if (i < programs.size())
            client.program = programs[i];
        if (i < stations.size())
            client.station = stations[i];
        clients.append(client);
    }
    return clients;
}

QList<ClientDisconnectDialog::Client> buildDisconnectClients(const RadioInfo& info)
{
    return buildDisconnectClients(info.guiClientHandles,
                                  info.guiClientPrograms,
                                  info.guiClientStations);
}

QList<ClientDisconnectDialog::Client> buildDisconnectClients(const WanRadioInfo& info)
{
    return buildDisconnectClients(splitClientField(info.guiClientHandles),
                                  splitClientField(info.guiClientPrograms),
                                  splitClientField(info.guiClientStations));
}

QString cleanClientDisplayText(QString value)
{
    value.replace(QChar(0x7f), QLatin1Char(' '));
    return value.trimmed();
}

QString clientConnectionStatusMessage(quint32 handle,
                                      const QString& source,
                                      const QString& station,
                                      const QString& program)
{
    QString from = cleanClientDisplayText(source);
    const QString stationText = cleanClientDisplayText(station);
    const QString programText = cleanClientDisplayText(program);
    QString detail = stationText;

    if (detail.isEmpty() || detail.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0)
        detail = programText;
    if (detail.compare(QStringLiteral("Unknown"), Qt::CaseInsensitive) == 0)
        detail.clear();

    if (from.isEmpty())
        from = detail;
    if (from.isEmpty())
        from = QStringLiteral("client 0x%1").arg(handle, 8, 16, QChar('0')).toUpper();

    if (!detail.isEmpty() && detail.compare(from, Qt::CaseInsensitive) != 0)
        return QObject::tr("New client connection from %1 (%2)").arg(from, detail);

    return QObject::tr("New client connection from %1").arg(from);
}

} // namespace AetherSDR
