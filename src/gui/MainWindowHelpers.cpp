#include "MainWindowHelpers.h"

#include "core/PanadapterStream.h"
#include "core/RadioDiscovery.h"
#include "core/SmartLinkClient.h"
#include "models/BandSettings.h"
#include "models/MemoryEntry.h"
#include "models/RadioModel.h"
#include "models/TnfModel.h"

#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
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
// Distinct name (not an overload of formatNetworkSeqErrors): an
// anonymous-namespace overload would hide the namespace-scope two-int
// overload from unqualified lookup inside this TU.
QString formatCategorySeqErrors(const PanadapterStream::CategoryStats& stats)
{
    return formatNetworkSeqErrors(stats.errors, stats.packets);
}
} // namespace

QString buildNetworkTooltip(const RadioModel& model)
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

    QStringList lines;
    lines
        << QString("Network: %1").arg(model.networkQuality())
        << QString("Latency (RTT): %1").arg(formatNetworkMs(model.lastPingRtt()))
        << QString("Max RTT (session): %1").arg(formatNetworkMs(model.maxPingRtt()))
        << QString("Packet loss (%1s): %2")
               .arg(model.packetLossWindowSeconds())
               .arg(formatNetworkSeqErrors(model.packetLossWindowDrops(),
                                           model.packetLossWindowPackets()))
        << QString("Network jitter: %1").arg(formatNetworkMs(model.audioPacketJitterMs()))
        << QString("Audio gap: %1 (max %2)")
               .arg(formatNetworkMs(model.audioPacketGapMs()),
                    formatNetworkMs(model.audioPacketGapMaxMs()))
        << QString("Total sequence gaps: %1")
               .arg(formatNetworkSeqErrors(model.packetDropCount(), model.packetTotalCount()))
        << QString("Audio: %1").arg(formatCategorySeqErrors(audioStats))
        << QString("FFT: %1").arg(formatCategorySeqErrors(fftStats))
        << QString("Waterfall: %1").arg(formatCategorySeqErrors(waterfallStats))
        << QString("Meters: %1").arg(formatCategorySeqErrors(meterStats))
        << QString("DAX: %1").arg(formatCategorySeqErrors(daxStats))
        << QString("UDP RX bytes: %1").arg(QLocale().formattedDataSize(model.rxBytes()))
        << QString("UDP TX bytes: %1").arg(QLocale().formattedDataSize(model.txBytes()))
        << "Double-click for full diagnostics";
    return lines.join('\n');
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
