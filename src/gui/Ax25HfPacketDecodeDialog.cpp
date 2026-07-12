#include "Ax25HfPacketDecodeDialog.h"

#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/DaxTxPolicy.h"
#include "core/TxKeyingMarker.h"
#include "core/LogManager.h"
#include "core/MaidenheadLocator.h"
#include "core/ThemeManager.h"
#include "core/aprs/AprsBeacon.h"
#include "core/aprs/AprsMessenger.h"
#include "core/aprs/AprsPacket.h"
#include "core/aprs/AprsSettings.h"
#include "core/aprs/AprsStationList.h"
#include "gui/AprsMessagesDialog.h"
#include "gui/AprsSymbolIcons.h"
#include "core/tnc/Ax25.h"
#include "core/tnc/Ax25FrameFormatter.h"
#include "core/tnc/HeardList.h"
#include "core/tnc/KissTncServer.h"
#include "core/tnc/TncTerminal.h"
#include "core/pms/PmsMailbox.h"
#include "gui/DStarModemPage.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"
#ifdef HAVE_MQTT
#include "core/MqttClient.h"
#include "core/MqttSettings.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#endif

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleValidator>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QTableWidget>
#include <QTimeZone>
#include <QToolButton>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace AetherSDR {

namespace {

constexpr auto kPacketDecoderProfileSetting  = "Ax25PacketDecoderProfile";
constexpr auto kPacketDecoderDebugSetting    = "Ax25PacketDecoderDiagnosticsDebug";
// TNC settings live as nested JSON under "AetherModemKissTnc" — see
// TncSettings class in the header. Legacy flat-key migration in
// TncSettings::migrateLegacy() is run from MainWindow at startup.
constexpr auto kTncSettingsKey   = "AetherModemKissTnc";

// Symmetry with KissTncServer's kMaxWriteBacklogBytes on the RX path: cap
// the TX queue so a misbehaving KISS client pushing frames faster than RF
// can drain them can't grow it without bound. Drop-oldest on overflow
// (oldest is the most-stale; better to lose old data than block new).
constexpr int kMaxKissTxQueueDepth   = 64;
// Maximum 250 ms radio-busy retries per head-of-queue frame before we
// abandon it and try the next one. 60 × 250 ms = 15 s — long enough to
// ride out an ATU tune or a long voice transmission, short enough that a
// stuck-PTT radio doesn't permanently jam the queue.
constexpr int kMaxKissTxBusyRetries  = 60;

// Personal Mailbox System (PMS) settings keys.
// TODO(Principle V): migrate these to a nested-JSON blob alongside TncSettings
// before this becomes the established pattern. Filed as a follow-up to issue #3424.
constexpr auto kPmsEnabledSetting = "AetherModemPmsEnabled";
constexpr auto kPmsListenCallSetting = "AetherModemPmsListenCallsign";
constexpr auto kPmsAliasCallSetting = "AetherModemPmsAliasCallsign";
constexpr auto kPmsWelcomeSetting = "AetherModemPmsWelcome";
constexpr auto kPmsBeaconEnabledSetting = "AetherModemPmsBeaconEnabled";
constexpr auto kPmsBeaconTextSetting = "AetherModemPmsBeaconText";

// TNC Terminal settings keys (persisted in AppSettings).
constexpr auto kTerminalMyCallSetting = "AetherModemTerminalMyCall";
constexpr auto kTerminalLastCallSetting = "AetherModemTerminalLastCall";
constexpr auto kTerminalTxTailSetting = "AetherModemTerminalTxTailMs";
constexpr auto kTerminalRetrySecsSetting = "AetherModemTerminalRetrySecs";
constexpr auto kTerminalMaxTriesSetting = "AetherModemTerminalMaxTries";
constexpr auto kTerminalPaclenSetting = "AetherModemTerminalPaclen";
constexpr auto kTerminalLogSetting = "AetherModemTerminalLogEnabled";
constexpr int kTerminalDefaultRetrySecs = 6;
constexpr int kTerminalDefaultMaxTries = 8;
constexpr int kTerminalDefaultPaclen = 128;

constexpr int kAudioCaptureSeconds = 180;
constexpr int kTxDaxSettleMs = 150;
constexpr int kTxLeadMs = 200;
// Default TX tail: how long PTT stays up after the audio is queued, to flush the
// DAX/radio buffer before unkey. On a half-duplex link this is also dead air the
// peer can't talk over, so it's operator-tunable (Terminal tab, "TX Tail"); the
// runtime value lives in m_txTailMs. Too short clips the end of our frame.
constexpr int kTxTailDefaultMs = 150;
constexpr int kTxChunkMs = 20;
// TX jitter buffer: how far ahead of real time we keep the radio's TX FIFO.
// The pacer runs on the GUI thread, which jitters under RX-decode / diagnostics
// / render load (measured stalls of 40-55 ms). Front-loading this much audio
// and then catch-up pacing keeps the FIFO from underrunning during those
// stalls. Must comfortably exceed the worst pacer gap, and stay within the
// radio's DAX TX buffer depth. Raise if clipping persists; lower if the FIFO
// overflows.
constexpr int kTxLeadBufferMs = 120;

constexpr const char* kAetherModemStyle = R"(
QWidget {
    color: #aeb9cc;
    background: #07101c;
    font-size: 14px;
}
QLabel {
    background: transparent;
}
QFrame#TabsFrame,
QFrame#ControlsFrame,
QFrame#LogFrame,
QFrame#ActionFrame,
QFrame#StatusFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QFrame#TabCell {
    background: transparent;
    border-right: 1px solid #233246;
}
QFrame#ControlCell {
    background: transparent;
    border-right: 1px solid #1c2a3b;
}
QLabel#SectionLabel {
    background: transparent;
    color: #8d99ad;
    font-size: 11px;
    font-weight: 700;
}
QLabel#StatusValue {
    background: transparent;
    color: #b9c4d7;
    font-size: 14px;
    font-weight: 600;
}
QLabel#StatusDot {
    background: #64d36e;
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QRadioButton,
QCheckBox {
    background: transparent;
    color: #aeb9cc;
    spacing: 9px;
}
QRadioButton::indicator {
    width: 20px;
    height: 20px;
    border-radius: 10px;
    border: 2px solid #26374e;
    background: #08111d;
}
QRadioButton::indicator:checked {
    border: 2px solid #65d379;
    background: #132d26;
}
QRadioButton::indicator:checked:hover {
    border-color: #80ed91;
}
QCheckBox::indicator {
    width: 20px;
    height: 20px;
    border-radius: 4px;
    border: 1px solid #34533c;
    background: #0d1a18;
}
QCheckBox::indicator:checked {
    background: #5ebd69;
    border-color: #65d379;
}
QPushButton {
    color: #aeb9cc;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #142235, stop:1 #0b1625);
    border: 1px solid #26374e;
    border-radius: 7px;
    padding: 10px 18px;
    font-weight: 600;
}
QPushButton:hover {
    border-color: #3c526d;
    color: #d6dfeb;
}
QPushButton:disabled {
    color: #6e7a8d;
    border-color: #1d2a3c;
    background: #0b1522;
}
QPushButton#TabButton {
    border-radius: 5px;
    border: 1px solid transparent;
    background: transparent;
    min-height: 20px;
    padding: 4px 12px;
    font-size: 13px;
}
QPushButton#TabButton:checked {
    color: #d4deea;
    border-color: #54c768;
    background: #0d1c20;
}
QPushButton#TabButton:disabled {
    color: #7f8b9e;
}
QComboBox {
    color: #aeb9cc;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 6px 28px 6px 10px;
}
QLineEdit {
    color: #c4cedd;
    background: #050b13;
    border: 1px solid #26374e;
    border-radius: 7px;
    padding: 10px 12px;
    selection-background-color: #1b3650;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
}
QLineEdit:focus {
    border-color: #54c768;
}
QTextEdit {
    color: #c2ccdb;
    background: #050b13;
    border: none;
    selection-background-color: #1b3650;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
}
QScrollBar:vertical {
    background: #07101c;
    width: 12px;
    margin: 8px 2px 8px 2px;
    border-radius: 6px;
}
QScrollBar::handle:vertical {
    background: #25364d;
    border-radius: 5px;
    min-height: 34px;
}
QScrollBar::add-line:vertical,
QScrollBar::sub-line:vertical {
    height: 0px;
}
QLabel#ExperimentalBanner {
    background: #3a2a14;
    color: #e8b977;
    border: 1px solid #6b4a1f;
    border-radius: 6px;
    padding: 8px 12px;
    font-size: 13px;
}
QTableWidget {
    color: #c2ccdb;
    background: #050b13;
    alternate-background-color: #081220;
    border: none;
    gridline-color: #14202f;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
    selection-background-color: #1b3650;
}
QTableWidget::item {
    padding: 2px 10px;
}
QHeaderView::section {
    color: #8d99ad;
    background: #0d1825;
    border: none;
    border-bottom: 1px solid #233246;
    padding: 5px 8px;
    font-size: 11px;
    font-weight: 700;
}
QTableCornerButton::section {
    background: #0d1825;
    border: none;
}
QSpinBox {
    color: #c4cedd;
    background: #0b1625;
    border: 1px solid #26374e;
    border-radius: 5px;
    padding: 6px 8px;
}
QPushButton#EnvelopeButton[hasUnread="true"] {
    color: #80ed91;
    border-color: #54c768;
}
)";

QString profileSettingsValue(Ax25ModemProfile profile)
{
    switch (profile) {
    case Ax25ModemProfile::Hf300:
        return QStringLiteral("Hf300");
    case Ax25ModemProfile::Vhf1200:
        return QStringLiteral("Vhf1200");
    }
    return QStringLiteral("Hf300");
}

Ax25ModemProfile profileFromSettingsValue(const QString& value)
{
    if (value == QStringLiteral("Vhf1200"))
        return Ax25ModemProfile::Vhf1200;
    return Ax25ModemProfile::Hf300;
}

QLabel* sectionLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionLabel"));
    return label;
}

QFrame* panel(const QString& objectName, QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(objectName);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    return frame;
}

QPushButton* tabButton(const QString& text, bool active, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QStringLiteral("TabButton"));
    button->setCheckable(true);
    button->setChecked(active);
    button->setEnabled(active);
    button->setFlat(true);
    button->setMinimumWidth(0);
    button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return button;
}

QFrame* statusPanel(const QString& title, QLabel** dot, QLabel** value, QWidget* parent)
{
    auto* frame = panel(QStringLiteral("StatusFrame"), parent);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(10);
    layout->addWidget(sectionLabel(title, frame));

    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    if (dot) {
        *dot = new QLabel(frame);
        (*dot)->setObjectName(QStringLiteral("StatusDot"));
        row->addWidget(*dot);
    }
    if (value) {
        *value = new QLabel(frame);
        (*value)->setObjectName(QStringLiteral("StatusValue"));
        row->addWidget(*value);
    }
    row->addStretch(1);
    layout->addLayout(row);
    return frame;
}

QString utcClock()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"));
}

QString ax25CapturePath()
{
    const QString dir = QFileInfo(AppSettings::instance().filePath()).absolutePath();
    QDir().mkpath(dir);
    const QString stamp = QDateTime::currentDateTimeUtc()
        .toString(QStringLiteral("yyyyMMdd-HHmmss'Z'"));
    return QDir(dir).filePath(QStringLiteral("ax25-rx-capture-%1-float32.wav").arg(stamp));
}

bool writeMonoFloatWav(const QString& path, const QByteArray& pcm, int sampleRate)
{
    if (sampleRate <= 0 || pcm.isEmpty() || pcm.size() % static_cast<int>(sizeof(float)) != 0)
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    auto writeAscii = [&file](const char* text) {
        file.write(text, 4);
    };
    auto writeU16 = [&file](quint16 value) {
        char bytes[2] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
        };
        file.write(bytes, sizeof(bytes));
    };
    auto writeU32 = [&file](quint32 value) {
        char bytes[4] = {
            static_cast<char>(value & 0xff),
            static_cast<char>((value >> 8) & 0xff),
            static_cast<char>((value >> 16) & 0xff),
            static_cast<char>((value >> 24) & 0xff),
        };
        file.write(bytes, sizeof(bytes));
    };

    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 32;
    constexpr quint16 audioFormatIeeeFloat = 3;
    const quint32 dataBytes = static_cast<quint32>(pcm.size());
    const quint32 byteRate = static_cast<quint32>(sampleRate * channels * sizeof(float));
    const quint16 blockAlign = channels * static_cast<quint16>(sizeof(float));

    writeAscii("RIFF");
    writeU32(36u + dataBytes);
    writeAscii("WAVE");
    writeAscii("fmt ");
    writeU32(16);
    writeU16(audioFormatIeeeFloat);
    writeU16(channels);
    writeU32(static_cast<quint32>(sampleRate));
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);
    writeAscii("data");
    writeU32(dataBytes);
    file.write(pcm);
    return file.error() == QFileDevice::NoError;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// TncSettings — nested-JSON persistence (Constitution Principle V).
// ─────────────────────────────────────────────────────────────────────────

QJsonObject TncSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kTncSettingsKey, QString{}).toString();
    if (json.isEmpty()) return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void TncSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kTncSettingsKey,
               QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

void TncSettings::setEnabled(bool on)
{
    QJsonObject o = readObj();
    o["enabled"] = on ? QStringLiteral("True") : QStringLiteral("False");
    write(o);
}

void TncSettings::setStartOnStartup(bool on)
{
    QJsonObject o = readObj();
    o["startOnStartup"] = on ? QStringLiteral("True") : QStringLiteral("False");
    write(o);
}

void TncSettings::setPort(int p)
{
    if (p < kMinPort || p > kMaxPort) p = kDefaultPort;
    QJsonObject o = readObj();
    o["port"] = QString::number(p);
    write(o);
}

void TncSettings::migrateLegacy()
{
    auto& s = AppSettings::instance();
    if (s.contains(kTncSettingsKey)) return;  // already migrated

    // Read the three legacy flat keys with the same defaults the old code used.
    const QString enabledStr        = s.value("AetherModemKissTncEnabled",        "False").toString();
    const QString startOnStartupStr = s.value("AetherModemKissTncStartOnStartup", "False").toString();
    const int     portInt           = s.value("AetherModemKissTncPort",
                                              QString::number(kDefaultPort)).toString().toInt();

    QJsonObject o;
    o["enabled"]        = enabledStr;
    o["startOnStartup"] = startOnStartupStr;
    o["port"]           = QString::number(
        (portInt >= kMinPort && portInt <= kMaxPort) ? portInt : kDefaultPort);
    write(o);

    // The legacy flat keys are left in place — AppSettings is XML and
    // a future cleanup PR can drop them once we know no other reader
    // still touches them. The nested blob is now authoritative.
}

class PacketActivityWidget final : public QWidget {
public:
    // An EKG-style sweep trace, in the spirit of a hospital heart monitor:
    // a cursor sweeps continuously across the strip and every decoded frame
    // draws a sharp QRS-like spike in green. HDLC candidates that failed the
    // FCS draw smaller amber bumps, and an open receive gate (signal above
    // squelch) lifts and slightly agitates the baseline. The trail fades with
    // age, so the strip reads as "the last few seconds of the channel" — at
    // typical APRS rates of 1-3 packets/s each packet is an individually
    // countable heartbeat, where the old per-second bar graph mushed them
    // into near-identical columns.
    explicit PacketActivityWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        updateToolTip();
        m_sweep.setInterval(kFrameMs);
        connect(&m_sweep, &QTimer::timeout, this, [this] { advanceSweep(); });
        resizeBuffers(220);
    }

    void setDebugEnabled(bool enabled)
    {
        if (m_debugEnabled == enabled)
            return;
        m_debugEnabled = enabled;
        updateToolTip();
        update();
    }

    void setClickHandler(std::function<void()> handler)
    {
        m_clickHandler = std::move(handler);
    }

    // One accepted (FCS-good) frame: queue a QRS complex for the pen to draw
    // as the cursor sweeps over the next ~quarter second.
    void recordFrame()
    {
        // P bump, Q dip, tall R, S undershoot, T recovery.
        static constexpr float kQrs[] = {
            0.10f, 0.16f, 0.10f, -0.10f, 0.55f, 1.00f, 0.45f,
            -0.30f, -0.12f, 0.08f, 0.20f, 0.16f, 0.06f,
        };
        enqueue(kQrs, int(sizeof(kQrs) / sizeof(kQrs[0])), SampleKind::Decode);
        wake();
    }

    // Once-per-second channel health from the decoder diagnostics.
    void tick(int hdlcCandidates, int acceptedFrames, bool receiveGateOpen)
    {
        m_gateOpen = receiveGateOpen;
        m_lastTick.restart();
        // Accepted frames already spiked via recordFrame(); what's left here
        // is candidates that never passed the FCS — the "almost" bumps.
        static constexpr float kBump[] = { 0.12f, 0.30f, 0.45f, 0.30f, 0.12f };
        const int failed = qMin(3, qMax(0, hdlcCandidates - acceptedFrames));
        for (int i = 0; i < failed; ++i)
            enqueue(kBump, int(sizeof(kBump) / sizeof(kBump[0])), SampleKind::Candidate);
        wake();
    }

    void reset()
    {
        m_pending.clear();
        std::fill(m_values.begin(), m_values.end(), 0.0f);
        std::fill(m_kinds.begin(), m_kinds.end(), quint8(SampleKind::Idle));
        m_cursor = 0;
        update();
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && m_clickHandler) {
            m_clickHandler();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        resizeBuffers(width());
    }

    void hideEvent(QHideEvent* event) override
    {
        m_sweep.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        // Scope bed: near-black panel with a faint phosphor baseline.
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(0x23, 0x32, 0x46), 1));
        painter.setBrush(QColor(0x03, 0x08, 0x0e));
        painter.drawRoundedRect(frame, 4, 4);

        const int n = m_values.size();
        if (n < 8)
            return;
        const float base = height() - 6.0f;
        const float usable = height() - 10.0f;
        auto yFor = [&](float v) {
            return qBound(2.0f, base - v * usable, float(height() - 2));
        };

        painter.setClipRect(frame);
        // The trace, oldest-to-newest behind the cursor, alpha fading with
        // age like phosphor afterglow. A short blank gap rides ahead of the
        // cursor, as on a real monitor.
        constexpr int kGapPx = 12;
        for (int age = n - kGapPx; age >= 1; --age) {
            const int i1 = (m_cursor - age + 1 + n) % n;
            const int i0 = (i1 - 1 + n) % n;
            const float fade = 1.0f - float(age) / float(n);
            const int alpha = int(40 + 215 * fade * fade);
            QColor color;
            switch (SampleKind(m_kinds[i1])) {
            case SampleKind::Decode:    color = QColor(0x80, 0xed, 0x91); break;
            case SampleKind::Candidate: color = QColor(0xd2, 0xa4, 0x48); break;
            case SampleKind::Idle:
            default:
                color = m_gateOpen ? QColor(0x4d, 0x86, 0xa8)
                                   : QColor(0x35, 0x4a, 0x63);
                break;
            }
            color.setAlpha(alpha);
            painter.setPen(QPen(color, SampleKind(m_kinds[i1]) == SampleKind::Idle
                                    ? 1.0 : 1.6));
            painter.drawLine(QPointF(i0, yFor(m_values[i0])),
                             QPointF(i1, yFor(m_values[i1])));
        }

        // Cursor head: a bright dot with a soft glow.
        const QPointF head(m_cursor, yFor(m_values[m_cursor]));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x80, 0xed, 0x91, 70));
        painter.drawEllipse(head, 4.0, 4.0);
        painter.setBrush(QColor(0xd6, 0xff, 0xdd));
        painter.drawEllipse(head, 1.6, 1.6);
        painter.setClipping(false);

        if (m_debugEnabled) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(210, 164, 72), 1));
            painter.drawRoundedRect(frame, 4, 4);
        }
    }

private:
    enum class SampleKind : quint8 { Idle = 0, Candidate = 1, Decode = 2 };

    static constexpr int kFrameMs = 33;     // ~30 fps sweep animation
    static constexpr int kPxPerFrame = 2;   // ~60 px/s; a 220 px strip ≈ 3.6 s

    void resizeBuffers(int w)
    {
        const int n = qBound(60, w, 2000);
        if (n == m_values.size())
            return;
        m_values = QVector<float>(n, 0.0f);
        m_kinds = QVector<quint8>(n, quint8(SampleKind::Idle));
        m_cursor = qMin(m_cursor, n - 1);
    }

    void enqueue(const float* shape, int count, SampleKind kind)
    {
        // Bound the backlog so a burst can't lag the pen seconds behind
        // real time; newest data wins, matching the TX queue philosophy.
        if (m_pending.size() > 96)
            m_pending.clear();
        for (int i = 0; i < count; ++i)
            m_pending.append({ shape[i], quint8(kind) });
    }

    void wake()
    {
        if (!m_sweep.isActive() && isVisible())
            m_sweep.start();
    }

    void advanceSweep()
    {
        const int n = m_values.size();
        if (n <= 0)
            return;
        for (int step = 0; step < kPxPerFrame; ++step) {
            m_cursor = (m_cursor + 1) % n;
            if (!m_pending.isEmpty()) {
                const PendingSample s = m_pending.takeFirst();
                m_values[m_cursor] = s.value;
                m_kinds[m_cursor] = s.kind;
            } else {
                // Idle pen: flat when squelched, a restless 1-2 px shimmer
                // while the receive gate is open ("there is RF here").
                const float wiggle = m_gateOpen
                    ? 0.05f + 0.04f * float((m_cursor * 7) % 3)
                    : 0.0f;
                m_values[m_cursor] = wiggle;
                m_kinds[m_cursor] = quint8(SampleKind::Idle);
            }
        }
        // Freeze the trace (and stop repainting) once the modem stops
        // delivering diagnostics and the backlog has drained.
        if (m_pending.isEmpty() && m_lastTick.isValid()
            && m_lastTick.elapsed() > 3000)
            m_sweep.stop();
        update();
    }

    void updateToolTip()
    {
        setToolTip(m_debugEnabled
            ? QStringLiteral("Packet activity debug is on: raw decode log, AX.25 TX "
                             "row and diagnostics are shown. Click to turn it off.")
            : QStringLiteral("Each green heartbeat is a decoded packet; amber bumps "
                             "are frames that failed the FCS. Click to show the raw "
                             "decode log, AX.25 TX row and diagnostics."));
    }

    struct PendingSample {
        float value;
        quint8 kind;
    };

    QVector<float> m_values;
    QVector<quint8> m_kinds;
    QList<PendingSample> m_pending;
    QTimer m_sweep;
    QElapsedTimer m_lastTick;
    int m_cursor{0};
    bool m_gateOpen{false};
    bool m_debugEnabled{false};
    std::function<void()> m_clickHandler;
};

Ax25HfPacketDecodeDialog::Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                                   RadioModel* radio,
                                                   SliceModel* initialSlice,
                                                   QWidget* parent)
    : PersistentDialog(QStringLiteral("AetherModem"),
                       QStringLiteral("Ax25HfPacketDecodeDialogGeometry"),
                       parent)
    , m_audio(audio)
    , m_radio(radio)
{
    theme::setContainer(this, QStringLiteral("dialog/ax25Decode"));
    setMinimumSize(1080, 680);

    m_shim = new AetherAx25LibmodemShim();
    m_shim->moveToThread(&m_shimThread);
    connect(&m_shimThread, &QThread::finished, m_shim, &QObject::deleteLater);
    m_shimThread.start();
    m_kissServer = new KissTncServer(this);
    m_heard = new HeardList(this);
    m_terminal = new TncTerminal(this);
    m_pms = new PmsMailbox(this);
    m_aprsStations = new AprsStationList(this);
    m_aprsMessenger = new AprsMessenger(this);
    m_aprsBeacon = new AprsBeacon(this);

    // The TNC store lives next to the app settings (heard log + session logs).
    const QString tncDir =
        QFileInfo(AppSettings::instance().filePath()).absolutePath()
        + QStringLiteral("/tnc");
    m_heard->setPersistencePath(tncDir + QStringLiteral("/heard.json"));
    m_aprsStations->setPersistencePath(tncDir + QStringLiteral("/aprs-stations.json"));
    m_aprsMessenger->setPersistencePath(tncDir + QStringLiteral("/aprs-messages.json"));
    m_terminal->setHeardList(m_heard);
    m_terminal->setLogDirectory(tncDir + QStringLiteral("/logs"));
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(1000);
    // Free-running (not gated on the modem): updateHeartbeat() also ticks the
    // APRS station-table ages, which must stay honest while the modem is off.
    // The modem-health portion of the heartbeat early-outs when disabled.
    m_heartbeatTimer->start();
    m_txPaceTimer = new QTimer(this);
    m_txPaceTimer->setInterval(kTxChunkMs);
    bodyWidget()->setStyleSheet(QString::fromLatin1(kAetherModemStyle));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);

    auto* tabsFrame = panel(QStringLiteral("TabsFrame"), bodyWidget());
    auto* tabs = new QHBoxLayout(tabsFrame);
    tabs->setContentsMargins(0, 0, 0, 0);
    tabs->setSpacing(0);
    m_ax25Tab = tabButton(QStringLiteral("APRS"), true, tabsFrame);
    m_kissTab = tabButton(QStringLiteral("KISS TNC"), false, tabsFrame);
    m_terminalTab = tabButton(QStringLiteral("Terminal"), false, tabsFrame);
    m_mailboxTab = tabButton(QStringLiteral("Mailbox"), false, tabsFrame);
    m_dstarTab = tabButton(QStringLiteral("D-STAR"), false, tabsFrame);
    m_ax25Tab->setEnabled(true);
    m_kissTab->setEnabled(true);
    m_terminalTab->setEnabled(true);
    m_mailboxTab->setEnabled(true);
    m_dstarTab->setEnabled(true);
    auto* tabGroup = new QButtonGroup(this);
    tabGroup->setExclusive(true);
    tabGroup->addButton(m_ax25Tab, 0);
    tabGroup->addButton(m_kissTab, 1);
    tabGroup->addButton(m_terminalTab, 2);
    tabGroup->addButton(m_mailboxTab, 3);
    tabGroup->addButton(m_dstarTab, 4);
    tabs->addWidget(m_ax25Tab, 1);
    tabs->addWidget(m_kissTab, 1);
    tabs->addWidget(m_terminalTab, 1);
    tabs->addWidget(m_mailboxTab, 1);
    tabs->addWidget(m_dstarTab, 1);
    root->addWidget(tabsFrame);

    m_tabStack = new QStackedWidget(bodyWidget());
    root->addWidget(m_tabStack);
    connect(tabGroup, &QButtonGroup::idClicked, m_tabStack, &QStackedWidget::setCurrentIndex);
    connect(m_tabStack, &QStackedWidget::currentChanged,
            this, &Ax25HfPacketDecodeDialog::updateTabChrome);

    // APRS page: modem config, beacon/messaging controls, and the station
    // table. The raw decode log and status row below the stack are shared by
    // all tabs.
    auto* ax25Page = new QWidget(m_tabStack);
    m_aprsPage = ax25Page;
    auto* ax25PageLayout = new QVBoxLayout(ax25Page);
    ax25PageLayout->setContentsMargins(0, 0, 0, 0);
    ax25PageLayout->setSpacing(10);
    m_tabStack->addWidget(ax25Page);

    auto* controlsFrame = panel(QStringLiteral("ControlsFrame"), ax25Page);
    auto* controls = new QHBoxLayout(controlsFrame);
    controls->setContentsMargins(16, 10, 16, 10);
    controls->setSpacing(20);

    auto* baudCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* baudLayout = new QVBoxLayout(baudCell);
    baudLayout->setContentsMargins(0, 0, 20, 0);
    baudLayout->setSpacing(8);
    baudLayout->addWidget(sectionLabel(QStringLiteral("BAUD RATE"), baudCell));
    auto* baudButtons = new QHBoxLayout;
    baudButtons->setSpacing(34);
    m_hf300Profile = new QRadioButton(QStringLiteral("300 baud"), baudCell);
    m_vhf1200Profile = new QRadioButton(QStringLiteral("1200 baud"), baudCell);
    baudButtons->addWidget(m_hf300Profile);
    baudButtons->addWidget(m_vhf1200Profile);
    baudButtons->addStretch(1);
    baudLayout->addLayout(baudButtons);
    controls->addWidget(baudCell, 2);

    auto* modemCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* modemLayout = new QVBoxLayout(modemCell);
    modemLayout->setContentsMargins(0, 0, 20, 0);
    modemLayout->setSpacing(8);
    modemLayout->addWidget(sectionLabel(QStringLiteral("MODEM"), modemCell));
    auto* modemChecks = new QHBoxLayout;
    modemChecks->setSpacing(20);
    m_enableDecode = new QCheckBox(QStringLiteral("Enable Modem"), modemCell);
    modemChecks->addWidget(m_enableDecode);
    m_modemAutostart = new QCheckBox(QStringLiteral("Autostart at launch"), modemCell);
    m_modemAutostart->setToolTip(QStringLiteral(
        "Enable the modem automatically when AetherSDR starts."));
    m_modemAutostart->setChecked(AprsSettings::modemAutostart());
    modemChecks->addWidget(m_modemAutostart);
    modemChecks->addStretch(1);
    modemLayout->addLayout(modemChecks);
    controls->addWidget(modemCell, 2);
    controls->addStretch(2);

    m_captureButton = new QPushButton(QStringLiteral("Capture 3m"), controlsFrame);
    m_captureButton->setMinimumHeight(42);
    controls->addWidget(m_captureButton);

    m_clearButton = new QPushButton(QStringLiteral("Clear Log"), controlsFrame);
    m_clearButton->setMinimumHeight(42);
    controls->addWidget(m_clearButton);
    ax25PageLayout->addWidget(controlsFrame);

    auto* txFrame = panel(QStringLiteral("ControlsFrame"), ax25Page);
    m_txFrame = txFrame;
    auto* txLayout = new QHBoxLayout(txFrame);
    txLayout->setContentsMargins(16, 12, 16, 12);
    txLayout->setSpacing(12);
    auto* txLabel = sectionLabel(QStringLiteral("TRANSMIT AX.25 UI FRAME"), txFrame);
    txLayout->addWidget(txLabel);
    m_txText = new QLineEdit(txFrame);
    m_txText->setPlaceholderText(QStringLiteral("hello world  or  N0CALL-1>APRS,WIDE1-1:hello world"));
    txLayout->addWidget(m_txText, 1);
    m_txButton = new QPushButton(QStringLiteral("Transmit"), txFrame);
    markTxKeying(m_txButton);   // transmits an AX.25 packet → keys TX (#3646)
    m_txButton->setMinimumHeight(42);
    txLayout->addWidget(m_txButton);

    // APRS client controls (beacon, messaging, station table) — the raw
    // UI-frame transmit row rides along at the bottom of the page.
    buildAprsUi(ax25Page, ax25PageLayout);
    ax25PageLayout->addWidget(txFrame);

    // KISS TNC page (built lazily into the same stack).
    m_tabStack->addWidget(buildKissTncPage());
    // TNC Terminal page (connected-mode AX.25 client).
    m_terminalPage = buildTerminalPage();
    m_tabStack->addWidget(m_terminalPage);
    // Mailbox (PMS) page.
    m_tabStack->addWidget(buildMailboxPage());
    m_dstarPage = new DStarModemPage(m_radio, m_tabStack);
    m_tabStack->addWidget(m_dstarPage);

    auto* logFrame = panel(QStringLiteral("LogFrame"), bodyWidget());
    m_logFrame = logFrame;
    auto* logLayout = new QVBoxLayout(logFrame);
    logLayout->setContentsMargins(12, 10, 12, 10);
    logLayout->setSpacing(0);

    m_log = new QTextEdit(logFrame);
    m_log->setReadOnly(true);
    m_log->document()->setMaximumBlockCount(2000);
    m_log->setLineWrapMode(QTextEdit::NoWrap);
    m_log->setPlaceholderText(QStringLiteral("Decoded AX.25 UI frames will appear here."));
    logLayout->addWidget(m_log);
    root->addWidget(logFrame, 1);

    // Slim status bar: MODEM STATUS, GAIN STAGE and PACKET ACTIVITY inline in a
    // single thin strip rather than three tall stacked panels.
    auto* statusBar = panel(QStringLiteral("StatusFrame"), bodyWidget());
    m_statusBar = statusBar;
    auto* statusBarLayout = new QHBoxLayout(statusBar);
    statusBarLayout->setContentsMargins(14, 6, 14, 6);
    statusBarLayout->setSpacing(10);

    auto statusBarSeparator = [&]() -> QLabel* {
        auto* sep = new QLabel(QStringLiteral("│"), statusBar);
        sep->setStyleSheet(QStringLiteral("color:#233246;"));
        return sep;
    };

    m_modemStatusDot = new QLabel(statusBar);
    m_modemStatusDot->setObjectName(QStringLiteral("StatusDot"));
    statusBarLayout->addWidget(m_modemStatusDot);
    auto* modemTag = sectionLabel(QStringLiteral("MODEM"), statusBar);
    statusBarLayout->addWidget(modemTag);
    m_modemStatusValue = new QLabel(statusBar);
    m_modemStatusValue->setObjectName(QStringLiteral("StatusValue"));
    statusBarLayout->addWidget(m_modemStatusValue);

    statusBarLayout->addWidget(statusBarSeparator());

    m_gainStageDot = new QLabel(statusBar);
    m_gainStageDot->setObjectName(QStringLiteral("StatusDot"));
    m_gainStageDot->setVisible(false); // gain has no dedicated indicator dot
    auto* gainTag = sectionLabel(QStringLiteral("GAIN"), statusBar);
    statusBarLayout->addWidget(gainTag);
    m_gainStageValue = new QLabel(statusBar);
    m_gainStageValue->setObjectName(QStringLiteral("StatusValue"));
    statusBarLayout->addWidget(m_gainStageValue);

    statusBarLayout->addStretch(1);

    m_packetActivityTitle = sectionLabel(QStringLiteral("ACTIVITY"), statusBar);
    statusBarLayout->addWidget(m_packetActivityTitle);
    m_packetActivity = new PacketActivityWidget(statusBar);
    m_packetActivity->setMinimumHeight(18);
    m_packetActivity->setMaximumHeight(20);
    // The EKG sweep is the main at-a-glance channel indicator now that the
    // raw log hides behind the debug toggle — give it a wide strip.
    m_packetActivity->setMinimumWidth(300);
    m_packetActivity->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_packetActivity->setClickHandler([this] {
        setDiagnosticsDebugEnabled(!m_diagnosticsDebugEnabled, true);
    });
    statusBarLayout->addWidget(m_packetActivity);

    root->addWidget(statusBar);

    const Ax25ModemProfile savedProfile = profileFromSettingsValue(
        AppSettings::instance().value(kPacketDecoderProfileSetting, QStringLiteral("Hf300")).toString());
    const bool savedDebug = AppSettings::instance().value(kPacketDecoderDebugSetting, false).toBool();
    m_hf300Profile->setChecked(savedProfile == Ax25ModemProfile::Hf300);
    m_vhf1200Profile->setChecked(savedProfile == Ax25ModemProfile::Vhf1200);
    setDiagnosticsDebugEnabled(savedDebug, false);
    setModemProfile(savedProfile, false);

    connect(m_hf300Profile, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
            setModemProfile(Ax25ModemProfile::Hf300, true);
    });
    connect(m_vhf1200Profile, &QRadioButton::toggled, this, [this](bool checked) {
        if (checked)
            setModemProfile(Ax25ModemProfile::Vhf1200, true);
    });
    connect(m_enableDecode, &QCheckBox::toggled,
            this, &Ax25HfPacketDecodeDialog::setDecodeEnabled);
    connect(m_modemAutostart, &QCheckBox::toggled, this, [](bool on) {
        AprsSettings::setModemAutostart(on);
    });
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        m_log->clear();
        m_frameCount = 0;
        m_lastDecodeUtc = {};
        m_lastActivityHdlc = 0;
        m_lastActivityAccepted = 0;
        if (m_packetActivity)
            m_packetActivity->reset();
        refreshStatus();
    });
    connect(m_captureButton, &QPushButton::clicked, this, [this] {
        if (m_captureActive)
            finishAudioCapture(false);
        else
            startAudioCapture();
    });
    connect(m_txText, &QLineEdit::textChanged,
            this, &Ax25HfPacketDecodeDialog::refreshTransmitControls);
    connect(m_txText, &QLineEdit::returnPressed,
            this, &Ax25HfPacketDecodeDialog::startTransmitFromUi);
    connect(m_txButton, &QPushButton::clicked,
            this, &Ax25HfPacketDecodeDialog::startTransmitFromUi);
    connect(m_txPaceTimer, &QTimer::timeout,
            this, &Ax25HfPacketDecodeDialog::paceTransmitAudio);
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded,
            this, &Ax25HfPacketDecodeDialog::appendFrame);
#ifdef HAVE_MQTT
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded,
            this, &Ax25HfPacketDecodeDialog::publishFrameMqtt);
#endif
    // RX -> KISS clients: forward every decoded frame to connected hosts.
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded, this,
            [this](const Ax25DecodedFrame& frame) {
        if (m_kissServer && m_kissServer->isListening() && !frame.ax25FrameNoFcs.isEmpty()) {
            m_kissServer->broadcastAx25Frame(frame.ax25FrameNoFcs);
            ++m_kissRxCount;
            refreshTncStatus();
        }
    });
    // RX -> Mailbox: feed every decoded frame to the PMS (heard list always;
    // connected-mode handling only for frames addressed to our PMS callsign).
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded, this,
            [this](const Ax25DecodedFrame& frame) {
        if (frame.ax25FrameNoFcs.isEmpty())
            return;
        // Record into the shared heard log once (drives MHEARD + quick-connect),
        // then through the APRS parser into the station roster + messenger.
        if (auto decoded = ax25::Frame::decode(frame.ax25FrameNoFcs)) {
            if (m_heard)
                m_heard->record(*decoded);
            if (auto packet = aprs::parseFrame(*decoded)) {
                if (m_aprsStations)
                    m_aprsStations->record(*packet);
                if (m_aprsMessenger)
                    m_aprsMessenger->onPacket(*packet);
            }
        }
        if (m_pms)
            m_pms->onAirFrame(frame.ax25FrameNoFcs);
        if (m_terminal)
            m_terminal->onAirFrame(frame.ax25FrameNoFcs);
    });
    connect(m_shim, &AetherAx25LibmodemShim::diagnosticsUpdated,
            this, &Ax25HfPacketDecodeDialog::updateDiagnostics);
    connect(m_shim, &AetherAx25LibmodemShim::statusChanged,
            this, &Ax25HfPacketDecodeDialog::refreshStatus);
    connect(m_heartbeatTimer, &QTimer::timeout,
            this, &Ax25HfPacketDecodeDialog::updateHeartbeat);

    if (m_radio) {
        connect(m_radio, &RadioModel::txAudioStreamReady,
                this, [this](quint32 streamId) {
            appendSystemLine(QStringLiteral("DAX TX stream ready: 0x%1.")
                .arg(streamId, 0, 16));
            if (m_txPendingStream)
                beginTransmitWhenReady();
        });
        connect(&m_radio->transmitModel(), &TransmitModel::pttBlocked,
                this, [this](const QString& message) {
            if (m_txActive || m_txPendingStream)
                finishTransmit(true, QStringLiteral("PTT blocked: %1").arg(message));
        });
    }

    if (m_audio) {
        connect(m_audio, &AudioEngine::tncRxAudioReady,
                this, &Ax25HfPacketDecodeDialog::handleRxAudio,
                Qt::QueuedConnection);
    }

    // KISS TNC server wiring.
    connect(m_kissServer, &KissTncServer::ax25FrameFromClient,
            this, &Ax25HfPacketDecodeDialog::handleKissFrameFromClient);
    connect(m_kissServer, &KissTncServer::activity,
            this, &Ax25HfPacketDecodeDialog::appendSystemLine);
    connect(m_kissServer, &KissTncServer::listeningChanged,
            this, [this](bool) { refreshTncStatus(); });
    connect(m_kissServer, &KissTncServer::clientCountChanged,
            this, [this](int) { refreshTncStatus(); });
    connect(m_tncEnable, &QCheckBox::toggled, this, [this](bool on) {
        setTncEnabled(on, true);
    });
    connect(m_tncStartOnStartup, &QCheckBox::toggled, this, [](bool on) {
        TncSettings::setStartOnStartup(on);
    });
    connect(m_tncPort, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
        TncSettings::setPort(value);
        if (m_tncEnable && m_tncEnable->isChecked()) {
            appendSystemLine(QStringLiteral("KISS TNC port changed to %1; restarting listener.")
                .arg(value));
            setTncEnabled(false, false);
            setTncEnabled(true, false);
        }
    });

    // Mailbox (PMS) wiring.
    connect(m_pms, &PmsMailbox::transmitFrame, this, [this](const QByteArray& raw) {
        if (raw.isEmpty() || !m_audio || !m_radio)
            return;
        m_kissTxQueue.enqueue(raw); // shares the one-at-a-time keying/pacing path
        maybeStartNextKissTx();
    });
    connect(m_pms, &PmsMailbox::activity, this, &Ax25HfPacketDecodeDialog::appendSystemLine);
    connect(m_pms, &PmsMailbox::stateChanged, this, [this] { refreshPmsStatus(); });
    connect(m_pmsEnable, &QCheckBox::toggled, this, [this](bool on) {
        setPmsEnabled(on, true);
    });
    connect(m_pmsListenCall, &QLineEdit::editingFinished, this, [this] {
        applyPmsConfigFromUi(true);
        refreshPmsStatus();
    });
    connect(m_pmsAliasCall, &QLineEdit::editingFinished, this, [this] {
        applyPmsConfigFromUi(true);
        refreshPmsStatus();
    });
    connect(m_pmsWelcome, &QLineEdit::editingFinished, this, [this] {
        applyPmsConfigFromUi(true);
    });
    connect(m_pmsBeaconText, &QLineEdit::editingFinished, this, [this] {
        applyPmsConfigFromUi(true);
    });
    connect(m_pmsBeaconEnable, &QCheckBox::toggled, this, [this](bool) {
        applyPmsConfigFromUi(true);
    });

    // APRS client wiring: beacon + messenger share the one-at-a-time modem
    // keying/pacing path with the KISS server, PMS, and terminal.
    auto enqueueAprsTx = [this](const QByteArray& raw) {
        if (raw.isEmpty() || !m_audio || !m_radio)
            return;
        if (m_enableDecode && !m_enableDecode->isChecked()) {
            appendSystemLine(QStringLiteral("Enabling the modem for APRS transmit."));
            m_enableDecode->setChecked(true);
        }
        m_kissTxQueue.enqueue(raw);
        maybeStartNextKissTx();
    };
    connect(m_aprsBeacon, &AprsBeacon::transmitFrame, this, enqueueAprsTx);
    connect(m_aprsMessenger, &AprsMessenger::transmitFrame, this, enqueueAprsTx);
    connect(m_aprsBeacon, &AprsBeacon::activity,
            this, &Ax25HfPacketDecodeDialog::appendSystemLine);
    connect(m_aprsMessenger, &AprsMessenger::activity,
            this, &Ax25HfPacketDecodeDialog::appendSystemLine);
    connect(m_aprsMessenger, &AprsMessenger::unreadCountChanged,
            this, [this](int) { updateAprsEnvelopeButton(); });
    connect(m_aprsStations, &AprsStationList::changed,
            this, &Ax25HfPacketDecodeDialog::refreshAprsStationTable);
    if (m_radio) {
        connect(m_radio, &RadioModel::gpsStatusChanged, this,
                [this](const QString&, int, int, const QString&, const QString&,
                       const QString&, const QString&, const QString&) {
            handleGpsUpdate();
        });
    }

    // TNC Terminal wiring.
    connect(m_terminal, &TncTerminal::transmitFrame, this, [this](const QByteArray& raw) {
        if (raw.isEmpty() || !m_audio || !m_radio)
            return;
        m_kissTxQueue.enqueue(raw); // shares the one-at-a-time keying/pacing path
        maybeStartNextKissTx();
    });
    connect(m_terminal, &TncTerminal::output, this, [this](const QString& text) {
        if (!m_terminalView)
            return;
        m_terminalView->moveCursor(QTextCursor::End);
        m_terminalView->insertPlainText(text);
        m_terminalView->moveCursor(QTextCursor::End);
        if (auto* bar = m_terminalView->verticalScrollBar())
            bar->setValue(bar->maximum());
    });
    // Terminal protocol activity stays out of the shared decode log box (and out
    // of the transcript unless verbose), but it is always written to the support
    // log file so connect/RR/REJ/retransmit traces are available for debugging.
    connect(m_terminal, &TncTerminal::activity, this, [](const QString& line) {
        qCDebug(lcAx25).noquote() << line;
    });
    connect(m_terminal, &TncTerminal::stateChanged, this, [this] { refreshTerminalStatus(); });
    connect(m_heard, &HeardList::changed, this, [this] { refreshTerminalHeardCombo(); });
    // Any outbound connect needs the modem RX tap running, or the BBS's frames
    // are never heard. Turn it on automatically before the link is dialed.
    connect(m_terminal, &TncTerminal::connectRequested, this, [this](const QString& peer) {
        if (m_enableDecode && !m_enableDecode->isChecked()) {
            appendSystemLine(
                QStringLiteral("Enabling the modem for the terminal connection to %1.").arg(peer));
            m_enableDecode->setChecked(true);
        }
    });

    appendSystemLine(QStringLiteral("AetherModem initialized."));
    appendSystemLine(QStringLiteral("Enable Modem to start the RX audio tap."));
    appendSystemLine(QStringLiteral("TX accepts raw payload text or full SRC>DST,path:payload syntax."));
    setAttachedSlice(initialSlice);
    refreshStatus();
    refreshTransmitControls();
    applyTncStartOnStartup();
    refreshTncStatus();

    // Restore mailbox (PMS) state and version SID.
#ifdef AETHERSDR_VERSION
    m_pms->setVersionString(QString::fromLatin1(AETHERSDR_VERSION));
#endif
    applyPmsConfigFromUi(false);
    const bool pmsOn = AppSettings::instance()
        .value(kPmsEnabledSetting, QStringLiteral("False")).toString()
            == QStringLiteral("True");
    if (pmsOn && m_pmsEnable)
        m_pmsEnable->setChecked(true); // fires setPmsEnabled() via the toggled connection
    refreshPmsStatus();

    // Restore TNC Terminal state.
    applyTerminalConfigFromUi(false);
    const bool termLogOn = AppSettings::instance()
        .value(kTerminalLogSetting, QStringLiteral("False")).toString()
            == QStringLiteral("True");
    if (termLogOn && m_terminalLogEnable)
        m_terminalLogEnable->setChecked(true); // fires the toggled handler
    refreshTerminalStatus();
    refreshTerminalHeardCombo();

    // Restore APRS client state. Config is pushed into the beacon/messenger
    // without persisting (it just came FROM settings); the beacon enable
    // checkbox fires its toggled handler, which arms the interval timer but
    // never transmits by itself — the first on-air beacon needs the operator
    // (Beacon Now) or the timer.
    applyAprsConfigFromUi(false);
    if (AprsSettings::beaconEnabled() && m_aprsBeaconEnable)
        m_aprsBeaconEnable->setChecked(true);
    handleGpsUpdate();
    refreshAprsStationTable();
    updateAprsEnvelopeButton();

    // Modem autostart: fires the toggled handler, which starts the RX tap.
    // MainWindow constructs this dialog (hidden) at app launch when the
    // setting is on, same as the KISS TNC start-on-startup path.
    if (AprsSettings::modemAutostart() && m_enableDecode)
        m_enableDecode->setChecked(true);

    // No control button may be the dialog's default button — otherwise pressing
    // Return in a text field would trigger it (Connect, Transmit, ...). Combined
    // with the title-bar fix, this guarantees Enter never does anything unwanted
    // in any AetherModem field; each field's own returnPressed still works.
    for (QPushButton* button : bodyWidget()->findChildren<QPushButton*>()) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    // Apply the per-tab chrome (hide the shared log on the Terminal tab) now that
    // the layout is fully built.
    updateTabChrome(m_tabStack->currentIndex());
}

Ax25HfPacketDecodeDialog::~Ax25HfPacketDecodeDialog()
{
    if (m_txActive || m_txPendingStream)
        finishTransmit(true, QStringLiteral("AetherModem window closing"));
    if (m_captureActive)
        finishAudioCapture(false);
    if (m_kissServer)
        m_kissServer->stop();
    if (m_audio)
        m_audio->setTncRxTapEnabled(false);
    m_shimThread.quit();
    m_shimThread.wait();
}

void Ax25HfPacketDecodeDialog::setAttachedSlice(SliceModel* slice)
{
    if (m_attachedSlice == slice) {
        logAttachedSliceState(QStringLiteral("slice state refresh"));
        refreshStatus();
        return;
    }

    if (m_sliceSquelchConnection)
        disconnect(m_sliceSquelchConnection);
    if (m_sliceModeConnection)
        disconnect(m_sliceModeConnection);
    m_sliceSquelchConnection = {};
    m_sliceModeConnection = {};

    m_attachedSlice = slice;
    m_attachedSliceId = slice ? slice->sliceId() : -1;

    if (slice) {
        m_sliceSquelchConnection = connect(slice, &SliceModel::squelchChanged,
                                           this, [this](bool on, int level) {
            const int sliceId = m_attachedSlice ? m_attachedSlice->sliceId() : m_attachedSliceId;
            appendSystemLine(QStringLiteral("Slice %1 squelch changed: %2, level %3.")
                .arg(sliceId)
                .arg(on ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(level));
            refreshStatus();
        });
        m_sliceModeConnection = connect(slice, &SliceModel::modeChanged,
                                        this, [this](const QString& mode) {
            logAttachedSliceState(QStringLiteral("slice mode changed to %1").arg(mode));
            refreshStatus();
        });
    }

    logAttachedSliceState(slice ? QStringLiteral("attached slice") : QStringLiteral("no slice attached"));
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setModemProfile(Ax25ModemProfile profile, bool persist)
{
    m_shimConfig = ax25DemodConfigForProfile(profile, Ax25TonePolarity::Normal);
    QMetaObject::invokeMethod(m_shim, [shim = m_shim, cfg = m_shimConfig]() {
        shim->configure(cfg);
    }, Qt::QueuedConnection);
    m_lastDiagnostics = {};
    m_lastDiagnosticsUtc = {};

    if (persist) {
        AppSettings::instance().setValue(kPacketDecoderProfileSetting, profileSettingsValue(profile));
        AppSettings::instance().save();
    }

    if (m_log)
        appendSystemLine(QStringLiteral("Configured %1.").arg(ax25DemodDescription(m_shimConfig)));
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setDecodeEnabled(bool enabled)
{
    if (enabled) {
        QMetaObject::invokeMethod(m_shim, &AetherAx25LibmodemShim::reset, Qt::QueuedConnection);
        m_lastDiagnostics = {};
        m_enabledUtc = QDateTime::currentDateTimeUtc();
        m_lastDiagnosticsUtc = {};
        m_lastNoAudioNoticeUtc = {};
        m_lastActivityHdlc = 0;
        m_lastActivityAccepted = 0;
        if (m_audio)
            m_audio->setTncRxTapEnabled(true);
        appendSystemLine(QStringLiteral(
            "Modem enabled. RX tap requested; waiting for 24 kHz PC RX audio."));
    } else {
        if (m_captureActive)
            finishAudioCapture(false);
        if (m_audio)
            m_audio->setTncRxTapEnabled(false);
        QMetaObject::invokeMethod(m_shim, &AetherAx25LibmodemShim::reset, Qt::QueuedConnection);
        m_lastDiagnostics = {};
        m_lastDiagnosticsUtc = {};
        m_lastActivityHdlc = 0;
        m_lastActivityAccepted = 0;
        appendSystemLine(QStringLiteral("Modem disabled. RX tap stopped."));
    }
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::handleRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate)
{
    if (m_captureActive && !monoFloat32Pcm.isEmpty()) {
        if (m_captureSampleRate == 0) {
            m_captureSampleRate = sampleRate;
            m_captureTargetBytes = static_cast<qsizetype>(sampleRate)
                * kAudioCaptureSeconds
                * static_cast<qsizetype>(sizeof(float));
            appendSystemLine(QStringLiteral("Audio capture armed: %1 seconds at %2 Hz.")
                .arg(kAudioCaptureSeconds)
                .arg(sampleRate));
        }

        if (sampleRate != m_captureSampleRate) {
            appendSystemLine(QStringLiteral("Audio capture cancelled: sample rate changed from %1 to %2 Hz.")
                .arg(m_captureSampleRate)
                .arg(sampleRate));
            finishAudioCapture(false);
        } else {
            const qsizetype remaining = m_captureTargetBytes - m_capturePcm.size();
            if (remaining > 0)
                m_capturePcm.append(monoFloat32Pcm.constData(),
                                    static_cast<qsizetype>(std::min<qsizetype>(remaining, monoFloat32Pcm.size())));
            if (m_capturePcm.size() >= m_captureTargetBytes)
                finishAudioCapture(true);
        }
    }

    QMetaObject::invokeMethod(m_shim, [shim = m_shim, pcm = monoFloat32Pcm, sr = sampleRate]() {
        shim->feedAudio(pcm, sr);
    }, Qt::QueuedConnection);
}

void Ax25HfPacketDecodeDialog::startAudioCapture()
{
    if (!m_enableDecode || !m_enableDecode->isChecked()) {
        appendSystemLine(QStringLiteral("Enable the modem before starting an RX audio capture."));
        return;
    }

    m_capturePcm.clear();
    m_captureSampleRate = 0;
    m_captureTargetBytes = 0;
    m_captureActive = true;
    QMetaObject::invokeMethod(m_shim, &AetherAx25LibmodemShim::reset, Qt::QueuedConnection);
    m_lastDiagnostics = {};
    m_lastDiagnosticsUtc = {};
    m_lastActivityHdlc = 0;
    m_lastActivityAccepted = 0;
    if (m_packetActivity)
        m_packetActivity->reset();
    if (m_captureButton)
        m_captureButton->setText(QStringLiteral("Cancel Capture"));
    appendSystemLine(QStringLiteral("Decoder state reset for RX audio capture."));
    appendSystemLine(QStringLiteral("Starting %1 second RX audio capture; transmit several packets now.")
        .arg(kAudioCaptureSeconds));
}

void Ax25HfPacketDecodeDialog::finishAudioCapture(bool save)
{
    const QByteArray capture = m_capturePcm;
    const int sampleRate = m_captureSampleRate;
    m_capturePcm.clear();
    m_captureSampleRate = 0;
    m_captureTargetBytes = 0;
    m_captureActive = false;
    if (m_captureButton)
        m_captureButton->setText(QStringLiteral("Capture 3m"));

    if (!save) {
        appendSystemLine(QStringLiteral("RX audio capture cancelled."));
        return;
    }

    const QString path = ax25CapturePath();
    if (!writeMonoFloatWav(path, capture, sampleRate)) {
        appendSystemLine(QStringLiteral("RX audio capture failed: could not write %1.")
            .arg(path));
        return;
    }

    appendSystemLine(QStringLiteral("RX audio capture saved: %1.")
        .arg(path));
}

void Ax25HfPacketDecodeDialog::startTransmitFromUi()
{
    if (!m_txText)
        return;
    startTransmit(m_txText->text());
}

void Ax25HfPacketDecodeDialog::startTransmit(const QString& text)
{
    if (m_txActive || m_txPendingStream) {
        appendSystemLine(QStringLiteral("TX already in progress."));
        return;
    }
    if (!m_audio || !m_radio) {
        appendSystemLine(QStringLiteral("TX unavailable: audio engine or radio model is not ready."));
        return;
    }
    if (m_radio->isRadioTransmitting() || m_radio->transmitModel().isTransmitting()) {
        appendSystemLine(QStringLiteral("TX unavailable: radio is already transmitting."));
        return;
    }

    Ax25TransmitResult tx = ax25BuildTransmitAudio(m_shimConfig, text, defaultTransmitSource());
    if (!tx.ok) {
        appendSystemLine(QStringLiteral("TX packetization failed: %1.").arg(tx.error));
        qCWarning(lcAx25).noquote() << "AX.25 TX packetization failed:" << tx.error;
        return;
    }
    beginTransmission(tx, false);
}

void Ax25HfPacketDecodeDialog::beginTransmission(const Ax25TransmitResult& tx, bool fromKiss)
{
    m_txFromKiss = fromKiss;
    m_pendingTx = tx;
    m_txPcm = tx.stereoFloat32Pcm;
    m_txOffsetBytes = 0;
    m_txChunkIndex = 0;
    const qsizetype chunkBytes = static_cast<qsizetype>(tx.sampleRate)
        * kTxChunkMs / 1000
        * 2
        * static_cast<qsizetype>(sizeof(float));
    m_txChunkCount = chunkBytes > 0
        ? static_cast<int>((m_txPcm.size() + chunkBytes - 1) / chunkBytes)
        : 0;

    appendSystemLine(QStringLiteral(
        "TX packetized (%1): %2 > %3%4, %5 payload bytes, %6 frame bytes, %7 bits, %8 s, RMS %9 dBFS, peak %10 dBFS.")
        .arg(fromKiss ? QStringLiteral("KISS") : QStringLiteral("text"),
             tx.frame.source.isEmpty() ? QStringLiteral("?") : tx.frame.source,
             tx.frame.destination.isEmpty() ? QStringLiteral("?") : tx.frame.destination,
             tx.frame.path.isEmpty()
                 ? QString()
                 : QStringLiteral(" via %1").arg(tx.frame.path.join(QStringLiteral(","))))
        .arg(tx.frame.payload.size())
        .arg(tx.frameBytes)
        .arg(tx.bitCount)
        .arg(tx.durationSeconds, 0, 'f', 2)
        .arg(tx.rmsDbfs, 0, 'f', 1)
        .arg(tx.peakDbfs, 0, 'f', 1));

    if (m_audio->txStreamId() == 0) {
        m_txPendingStream = true;
        refreshTransmitControls();
        appendSystemLine(QStringLiteral("Requesting DAX TX stream for AetherModem TX."));
        qCInfo(lcAx25) << "AX.25 TX requesting DAX TX stream";
        if (!m_radio->ensureDaxTxStream(DaxTxRequestReason::AetherModemAx25Tx))
            finishTransmit(true, QStringLiteral("DAX TX stream policy rejected stream creation"));
        return;
    }

    beginTransmitWhenReady();
}

#ifdef HAVE_MQTT
void Ax25HfPacketDecodeDialog::setMqttClient(MqttClient* mqtt)
{
    if (m_mqtt == mqtt)
        return;
    if (m_mqtt)
        disconnect(m_mqtt, &MqttClient::messageReceived,
                   this, &Ax25HfPacketDecodeDialog::handleMqttMessage);
    m_mqtt = mqtt;
    if (m_mqtt)
        connect(m_mqtt, &MqttClient::messageReceived,
                this, &Ax25HfPacketDecodeDialog::handleMqttMessage);
}

void Ax25HfPacketDecodeDialog::publishFrameMqtt(const Ax25DecodedFrame& frame)
{
    if (!m_mqtt)
        return;
    if (!isMqttTopicEnabled(QString::fromLatin1(kAx25RxTopic)))
        return;
    QString display = frame.source + QStringLiteral(">") + frame.destination;
    if (!frame.path.isEmpty())
        display += QStringLiteral(",") + frame.path.join(QStringLiteral(","));
    display += QStringLiteral(":")
        + (frame.payloadText.isEmpty() ? frame.payloadHex : frame.payloadText);
    QJsonObject obj;
    obj[QStringLiteral("timestamp")] = frame.timestampUtc.toString(Qt::ISODateWithMs);
    obj[QStringLiteral("source")]    = frame.source;
    obj[QStringLiteral("dest")]      = frame.destination;
    if (!frame.path.isEmpty())
        obj[QStringLiteral("path")] = QJsonArray::fromStringList(frame.path);
    obj[QStringLiteral("payload")]   = frame.payloadText.isEmpty() ? frame.payloadHex : frame.payloadText;
    obj[QStringLiteral("display")]   = display;
    obj[QStringLiteral("confidence")] = frame.confidenceOrQuality;
    m_mqtt->publish(QString::fromLatin1(kAx25RxTopic),
                    QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Ax25HfPacketDecodeDialog::handleMqttMessage(const QString& topic, const QByteArray& payload)
{
    if (topic != QString::fromLatin1(kAx25TxTopic))
        return;
    if (!isMqttTopicEnabled(QString::fromLatin1(kAx25TxTopic)))
        return;
    startTransmit(QString::fromUtf8(payload).trimmed());
}
#endif


void Ax25HfPacketDecodeDialog::beginTransmitWhenReady()
{
    if (m_txPcm.isEmpty())
        return;
    if (!m_audio || !m_radio) {
        finishTransmit(true, QStringLiteral("audio engine or radio model disappeared before TX"));
        return;
    }
    if (m_audio->txStreamId() == 0) {
        m_txPendingStream = true;
        refreshTransmitControls();
        return;
    }

    auto& txModel = m_radio->transmitModel();
    if (m_attachedSlice && !m_attachedSlice->isTxSlice()) {
        appendSystemLine(QStringLiteral("Selecting attached slice %1 for AX.25 TX.")
            .arg(m_attachedSlice->sliceId()));
        m_attachedSlice->setTxSlice(true);
    }
    m_txPendingStream = false;
    m_txActive = true;
    m_txPreviousAudioDaxMode = m_audio->isDaxTxMode();
    m_txPreviousTransmitDax = txModel.daxOn();
    m_txRestoreAudioDaxMode = true;
    m_txRestoreTransmitDax = true;

    m_audio->setDaxTxMode(true);
    txModel.setDax(true);
    appendSystemLine(QStringLiteral("Keying transmitter for AX.25 TX on %1; DAX TX stream 0x%2.")
        .arg(transmitSliceSummary())
        .arg(m_audio->txStreamId(), 0, 16));
    qCInfo(lcAx25).noquote()
        << QStringLiteral("AX.25 TX start stream=0x%1 %2 chunks=%3 daxSettleMs=%4 leadMs=%5 tailMs=%6")
            .arg(m_audio->txStreamId(), 0, 16)
            .arg(transmitSliceSummary())
            .arg(m_txChunkCount)
            .arg(kTxDaxSettleMs)
            .arg(kTxLeadMs)
            .arg(m_txTailMs);

    refreshTransmitControls();
    QTimer::singleShot(kTxDaxSettleMs, this, [this] {
        if (!m_txActive)
            return;
        if (!m_radio) {
            finishTransmit(true, QStringLiteral("radio model disappeared before PTT"));
            return;
        }
        auto& txModel = m_radio->transmitModel();
        txModel.requestPttOn(TransmitModel::PttSource::Dax);
        if (!m_txActive)
            return;
        if (!txModel.isTransmitting()) {
            finishTransmit(true, QStringLiteral("PTT did not engage"));
            return;
        }

        appendTransmitLine(m_pendingTx.frame);
        QTimer::singleShot(kTxLeadMs, this, [this] {
            if (!m_txActive)
                return;
            appendSystemLine(QStringLiteral("Sending AX.25 AFSK audio: %1 chunks at %2 ms.")
                .arg(m_txChunkCount)
                .arg(kTxChunkMs));
            m_txPaceClock.restart();
            m_txPaceLastChunkMs = -1;
            m_txPaceMaxGapMs = 0;
            m_txPaceLateChunks = 0;
            paceTransmitAudio();
            if (m_txActive && m_txPaceTimer)
                m_txPaceTimer->start();
        });
    });
}

void Ax25HfPacketDecodeDialog::paceTransmitAudio()
{
    if (!m_txActive || !m_audio)
        return;

    // Measure scheduling gap between pacer ticks. The pacer wants to fire every
    // kTxChunkMs; a much larger gap means the GUI thread stalled (heartbeat,
    // 1 Hz diagnostics, RX decode, render) and the radio TX FIFO likely
    // underran — the suspected cause of periodic AFSK corruption.
    const qint64 nowMs = m_txPaceClock.isValid() ? m_txPaceClock.elapsed() : 0;
    if (m_txPaceLastChunkMs >= 0) {
        const qint64 gapMs = nowMs - m_txPaceLastChunkMs;
        m_txPaceMaxGapMs = std::max(m_txPaceMaxGapMs, gapMs);
        if (gapMs > 2 * kTxChunkMs)
            ++m_txPaceLateChunks;
    }
    m_txPaceLastChunkMs = nowMs;

    const qsizetype frameBytes = 2 * static_cast<qsizetype>(sizeof(float)); // stereo float32
    const qsizetype bytesPerMs = static_cast<qsizetype>(m_pendingTx.sampleRate) * frameBytes / 1000;
    if (bytesPerMs <= 0) {
        finishTransmit(true, QStringLiteral("invalid TX pacing chunk size"));
        return;
    }

    if (m_txOffsetBytes >= m_txPcm.size()) {
        if (m_txPaceTimer)
            m_txPaceTimer->stop();

        // Pacing health summary. With catch-up pacing, stretch <= ~1.0 means we
        // kept up with real time and the radio FIFO stayed fed; stretch >> 1.0
        // means even catch-up could not keep up (FIFO would underrun). maxGap /
        // lateChunks still report raw GUI-thread jitter, but gaps smaller than
        // kTxLeadBufferMs are absorbed by the lead cushion and are harmless.
        const double audioMs = m_pendingTx.durationSeconds * 1000.0;
        const qint64 wallMs = m_txPaceClock.isValid() ? m_txPaceClock.elapsed() : 0;
        const double stretch = audioMs > 0.0 ? static_cast<double>(wallMs) / audioMs : 0.0;
        qCInfo(lcAx25).noquote()
            << QStringLiteral("AX.25 TX pacing summary: baud=%1 chunks=%2 audioMs=%3 wallMs=%4 "
                              "stretch=%5x maxChunkGapMs=%6 lateChunks=%7 nominalChunkMs=%8")
                .arg(m_shimConfig.baud)
                .arg(m_txChunkIndex)
                .arg(audioMs, 0, 'f', 0)
                .arg(wallMs)
                .arg(stretch, 0, 'f', 2)
                .arg(m_txPaceMaxGapMs)
                .arg(m_txPaceLateChunks)
                .arg(kTxChunkMs);
        appendSystemLine(QStringLiteral(
            "TX pacing: %1 chunks, audio %2 ms vs wall %3 ms (%4x), max gap %5 ms, late %6.")
            .arg(m_txChunkIndex)
            .arg(audioMs, 0, 'f', 0)
            .arg(wallMs)
            .arg(stretch, 0, 'f', 2)
            .arg(m_txPaceMaxGapMs)
            .arg(m_txPaceLateChunks));
        appendSystemLine(QStringLiteral("AX.25 TX audio queued; waiting %1 ms before unkey.")
            .arg(m_txTailMs));
        QTimer::singleShot(m_txTailMs, this, [this] {
            finishTransmit(false, QStringLiteral("AX.25 TX complete"));
        });
        return;
    }

    // Catch-up pacing: keep the radio's TX FIFO filled to (real time elapsed +
    // kTxLeadBufferMs). When a tick lands late this ships a larger chunk to
    // refill the cushion; when we are already ahead it ships nothing and waits
    // for real time to advance. This holds the average rate at real time (no
    // chronic lag) while absorbing GUI-thread stalls up to the lead buffer.
    const qsizetype targetBytes = bytesPerMs * (nowMs + kTxLeadBufferMs);
    if (targetBytes <= m_txOffsetBytes)
        return; // FIFO is far enough ahead; wait for the next tick.
    qsizetype sendBytes = std::min<qsizetype>(targetBytes - m_txOffsetBytes,
                                              m_txPcm.size() - m_txOffsetBytes);
    sendBytes -= sendBytes % frameBytes; // keep stereo-frame aligned
    if (sendBytes <= 0)
        return;
    const QByteArray chunk = m_txPcm.mid(m_txOffsetBytes, sendBytes);
    m_txOffsetBytes += sendBytes;
    ++m_txChunkIndex;

    QPointer<AudioEngine> audio = m_audio;
    QMetaObject::invokeMethod(m_audio, [audio, chunk]() {
        if (audio)
            audio->sendModemTxAudio(chunk);
    }, Qt::QueuedConnection);

    if (m_diagnosticsDebugEnabled
        && (m_txChunkIndex == 1 || m_txChunkIndex == m_txChunkCount
            || (m_txChunkIndex % std::max(1, 1000 / kTxChunkMs)) == 0)) {
        qCDebug(lcAx25).noquote()
            << QStringLiteral("AX.25 TX chunk %1/%2 bytes=%3 offset=%4/%5")
                .arg(m_txChunkIndex)
                .arg(m_txChunkCount)
                .arg(sendBytes)
                .arg(m_txOffsetBytes)
                .arg(m_txPcm.size());
    }
}

void Ax25HfPacketDecodeDialog::finishTransmit(bool aborted, const QString& reason)
{
    if (m_txPaceTimer)
        m_txPaceTimer->stop();

    const bool hadTx = m_txActive || m_txPendingStream || !m_txPcm.isEmpty();
    m_txActive = false;
    m_txPendingStream = false;

    if (m_radio) {
        auto& txModel = m_radio->transmitModel();
        if (txModel.isTransmitting())
            txModel.requestPttOff(TransmitModel::PttSource::Dax);
        if (m_txRestoreTransmitDax)
            txModel.setDax(m_txPreviousTransmitDax);
    }
    if (m_audio) {
        if (m_txRestoreAudioDaxMode)
            m_audio->setDaxTxMode(m_txPreviousAudioDaxMode);
        m_audio->clearTxAccumulators();
    }

    if (hadTx) {
        appendSystemLine(aborted
            ? QStringLiteral("AX.25 TX aborted: %1.").arg(reason)
            : QStringLiteral("%1.").arg(reason));
        qCInfo(lcAx25).noquote()
            << QStringLiteral("AX.25 TX %1 reason=%2 chunks=%3/%4 bytes=%5/%6")
                .arg(aborted ? QStringLiteral("aborted") : QStringLiteral("finished"),
                     reason)
                .arg(m_txChunkIndex)
                .arg(m_txChunkCount)
                .arg(m_txOffsetBytes)
                .arg(m_txPcm.size());
    }

    m_txPcm.clear();
    m_pendingTx = {};
    m_txOffsetBytes = 0;
    m_txChunkIndex = 0;
    m_txChunkCount = 0;
    m_txRestoreAudioDaxMode = false;
    m_txRestoreTransmitDax = false;
    m_txFromKiss = false;
    refreshTransmitControls();

    // Drain any queued KISS transmits. On a clean finish, kick the next one on a
    // deferred (queued) call so we never re-enter the TX path within finish; on
    // an abort, drop the backlog so a broken radio can't spin the queue.
    if (aborted) {
        if (!m_kissTxQueue.isEmpty()) {
            appendSystemLine(QStringLiteral("Dropping %1 queued KISS TX frame(s) after abort.")
                .arg(m_kissTxQueue.size()));
            m_kissTxQueue.clear();
        }
    } else if (!m_kissTxQueue.isEmpty()) {
        QTimer::singleShot(0, this, [this] { maybeStartNextKissTx(); });
    }
}

void Ax25HfPacketDecodeDialog::appendFrame(const Ax25DecodedFrame& frame)
{
    if (!frame.fcsOk)
        return;
    ++m_frameCount;
    m_lastDecodeUtc = frame.timestampUtc;
    m_log->append(formatTerminalLine(frame));
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    if (m_packetActivity)
        m_packetActivity->recordFrame();
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::updateDiagnostics(const Ax25DecoderDiagnostics& diagnostics)
{
    const bool firstAudio = !m_lastDiagnosticsUtc.isValid();
    m_lastDiagnostics = diagnostics;
    m_lastDiagnosticsUtc = QDateTime::currentDateTimeUtc();
    if (firstAudio) {
        appendSystemLine(QStringLiteral("RX audio stream detected: %1 Hz, %2 samples/window.")
            .arg(diagnostics.sampleRate)
            .arg(diagnostics.audioSamples));
    }
    if (m_diagnosticsDebugEnabled)
        appendDiagnosticsLine(diagnostics);
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::updateHeartbeat()
{
    // Station-table ages tick whether or not the modem is running — the
    // roster persists across sessions and "how stale is this row" should
    // stay honest while the modem is idle.
    refreshAprsStationAges();

    if (!m_enableDecode || !m_enableDecode->isChecked())
        return;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_packetActivity) {
        const quint64 hdlc = m_lastDiagnostics.plausibleAx25Candidates;
        const quint64 accepted = m_lastDiagnostics.framesAccepted;
        const int hdlcDelta = hdlc >= m_lastActivityHdlc
            ? static_cast<int>(std::min<quint64>(hdlc - m_lastActivityHdlc, 32))
            : 0;
        const int acceptedDelta = accepted >= m_lastActivityAccepted
            ? static_cast<int>(std::min<quint64>(accepted - m_lastActivityAccepted, 8))
            : 0;
        m_packetActivity->tick(hdlcDelta, acceptedDelta, m_lastDiagnostics.receiveGateOpen);
        m_lastActivityHdlc = hdlc;
        m_lastActivityAccepted = accepted;
    }

    if (!m_lastDiagnosticsUtc.isValid()) {
        const int waited = m_enabledUtc.isValid() ? static_cast<int>(m_enabledUtc.secsTo(now)) : 0;
        if (waited >= 2
            && (!m_lastNoAudioNoticeUtc.isValid() || m_lastNoAudioNoticeUtc.secsTo(now) >= 5)) {
            appendSystemLine(QStringLiteral(
                "Waiting for RX audio blocks. Confirm PC Audio is enabled, a slice is active, and AetherSDR is receiving the packet audio stream."));
            m_lastNoAudioNoticeUtc = now;
        }
    } else if (m_lastDiagnosticsUtc.secsTo(now) >= 4
               && (!m_lastNoAudioNoticeUtc.isValid() || m_lastNoAudioNoticeUtc.secsTo(now) >= 5)) {
        appendSystemLine(QStringLiteral(
            "No fresh RX audio diagnostics for %1 s. The tap is enabled, but audio may be paused or PC Audio may be off.")
            .arg(m_lastDiagnosticsUtc.secsTo(now)));
        m_lastNoAudioNoticeUtc = now;
    }

    refreshStatus();
}

void Ax25HfPacketDecodeDialog::refreshStatus()
{
    const bool enabled = m_enableDecode && m_enableDecode->isChecked();
    const bool haveAudio = m_lastDiagnosticsUtc.isValid();
    const int audioAge = haveAudio
        ? static_cast<int>(m_lastDiagnosticsUtc.secsTo(QDateTime::currentDateTimeUtc()))
        : -1;
    QString status;
    if (enabled && haveAudio && audioAge < 4) {
        status = QStringLiteral("Running | %1 | AX.25 %2 OK %3")
            .arg(m_lastDiagnostics.receiveGateOpen ? QStringLiteral("gate open") : QStringLiteral("listening"))
            .arg(m_lastDiagnostics.plausibleAx25Candidates)
            .arg(m_lastDiagnostics.framesAccepted);
    } else if (enabled && haveAudio) {
        status = QStringLiteral("Audio stalled | %1 s").arg(audioAge);
    } else if (enabled) {
        status = QStringLiteral("Waiting for RX audio");
    } else {
        status = m_attachedSliceId >= 0 ? QStringLiteral("Standby") : QStringLiteral("No slice attached");
    }

    if (m_modemStatusValue) {
        const QString stateText = m_lastDiagnostics.inFrame
            ? QStringLiteral("frame")
            : m_lastDiagnostics.inPreamble ? QStringLiteral("preamble") : QStringLiteral("search");
        const QString squelchText = m_attachedSlice
            ? QStringLiteral("%1, level %2")
                .arg(m_attachedSlice->squelchOn() ? QStringLiteral("on") : QStringLiteral("off"))
                .arg(m_attachedSlice->squelchLevel())
            : QStringLiteral("-");
        m_modemStatusValue->setText(status);
        m_modemStatusValue->setToolTip(QStringLiteral(
            "%1\nSlice: %2\nSquelch: %3\nFrames: %4\nLast decode: %5\nDecode lanes: %6\nHDLC starts: %7\nHDLC candidates: %8\nAX.25-like candidates: %9\nAccepted: %10\nRejected: %11\nToo short: %12\nBad FCS: %13\nMalformed: %14\nLast reject: %15\nState: %16, bits: %17, ones: %18%\nReceive gate: %19, rms %20 dBFS, floor %21 dBFS, resets %22")
            .arg(ax25DemodDescription(m_shimConfig))
            .arg(m_attachedSliceId >= 0 ? QString::number(m_attachedSliceId) : QStringLiteral("-"))
            .arg(squelchText)
            .arg(m_frameCount)
            .arg(m_lastDecodeUtc.isValid()
                 ? m_lastDecodeUtc.toUTC().toString(Qt::ISODate)
                 : QStringLiteral("-"))
            .arg(m_lastDiagnostics.decodeLanes)
            .arg(m_lastDiagnostics.hdlcFrameStarts)
            .arg(m_lastDiagnostics.hdlcFrameCandidates)
            .arg(m_lastDiagnostics.plausibleAx25Candidates)
            .arg(m_lastDiagnostics.framesAccepted)
            .arg(m_lastDiagnostics.decodeRejected)
            .arg(m_lastDiagnostics.rejectTooShort)
            .arg(m_lastDiagnostics.rejectBadFcs)
            .arg(m_lastDiagnostics.rejectMalformed)
            .arg(m_lastDiagnostics.lastRejectReason.isEmpty()
                 ? QStringLiteral("-")
                 : m_lastDiagnostics.lastRejectReason)
            .arg(stateText)
            .arg(m_lastDiagnostics.currentFrameBits)
            .arg(m_lastDiagnostics.onesPercent, 0, 'f', 1)
            .arg(m_lastDiagnostics.receiveGateOpen ? QStringLiteral("open") : QStringLiteral("idle"))
            .arg(m_lastDiagnostics.receiveGateRmsDbfs, 0, 'f', 1)
            .arg(m_lastDiagnostics.receiveGateFloorDbfs, 0, 'f', 1)
            .arg(m_lastDiagnostics.receiveGateResets));
    }
    if (m_modemStatusDot) {
        const QString color = enabled && haveAudio && audioAge < 4
            ? QStringLiteral("#64d36e")
            : enabled ? QStringLiteral("#d2a448") : QStringLiteral("#506174");
        m_modemStatusDot->setStyleSheet(
            QStringLiteral("QLabel#StatusDot { background: %1; border-radius: 6px; "
                           "min-width: 12px; max-width: 12px; min-height: 12px; max-height: 12px; }")
                .arg(color));
    }
    if (m_gainStageValue)
        m_gainStageValue->setText(haveAudio
            ? QStringLiteral("RMS %1 dBFS / pk %2")
                .arg(m_lastDiagnostics.rmsDbfs, 0, 'f', 1)
                .arg(m_lastDiagnostics.peakDbfs, 0, 'f', 1)
            : QStringLiteral("No audio yet"));
    refreshTransmitControls();
}

void Ax25HfPacketDecodeDialog::refreshTransmitControls()
{
    if (!m_txButton)
        return;

    const bool hasText = m_txText && !m_txText->text().trimmed().isEmpty();
    const bool ready = hasText && !m_txActive && !m_txPendingStream;
    m_txButton->setEnabled(ready);
    if (m_txActive) {
        m_txButton->setText(QStringLiteral("Transmitting..."));
    } else if (m_txPendingStream) {
        m_txButton->setText(QStringLiteral("Preparing..."));
    } else {
        m_txButton->setText(QStringLiteral("Transmit"));
    }

    if (m_txText) {
        m_txText->setEnabled(!m_txActive && !m_txPendingStream);
        m_txText->setToolTip(
            QStringLiteral("Transmit a %1 AX.25 UI frame. Raw text uses %2>APRS; full SRC>DST,path:payload syntax is also accepted.")
                .arg(ax25ModemProfileName(m_shimConfig.profile), defaultTransmitSource()));
    }
}

void Ax25HfPacketDecodeDialog::setDiagnosticsDebugEnabled(bool enabled, bool persist)
{
    if (m_diagnosticsDebugEnabled == enabled && persist)
        return;

    m_diagnosticsDebugEnabled = enabled;
    if (m_shim)
        QMetaObject::invokeMethod(m_shim, [shim = m_shim, enabled]() {
            shim->setDiagnosticsLoggingEnabled(enabled);
        }, Qt::QueuedConnection);
    if (m_terminal)
        m_terminal->setVerbose(enabled); // echo protocol detail inline in the terminal
    if (m_packetActivity)
        m_packetActivity->setDebugEnabled(enabled);
    if (m_packetActivityTitle) {
        m_packetActivityTitle->setText(enabled
            ? QStringLiteral("PACKET ACTIVITY DEBUG")
            : QStringLiteral("PACKET ACTIVITY"));
    }
    // The raw decode log, raw AX.25 TX row, and the capture/clear-log
    // buttons on the APRS tab are all diagnostics chrome — they follow the
    // debug flag. Refresh so they appear/disappear immediately.
    if (m_captureButton)
        m_captureButton->setVisible(enabled);
    if (m_clearButton)
        m_clearButton->setVisible(enabled);
    if (m_tabStack)
        updateTabChrome(m_tabStack->currentIndex());

    if (persist) {
        AppSettings::instance().setValue(kPacketDecoderDebugSetting, enabled);
        AppSettings::instance().save();
        appendSystemLine(enabled
            ? QStringLiteral("Packet diagnostics debug enabled.")
            : QStringLiteral("Packet diagnostics debug disabled."));
    }
}

void Ax25HfPacketDecodeDialog::logAttachedSliceState(const QString& reason)
{
    if (!m_attachedSlice) {
        appendSystemLine(QStringLiteral("%1.").arg(reason));
        return;
    }

    appendSystemLine(QStringLiteral(
        "%1: slice %2 mode=%3 squelch=%4 level=%5 AF=%6 AGC=%7/%8.")
        .arg(reason)
        .arg(m_attachedSlice->sliceId())
        .arg(m_attachedSlice->mode())
        .arg(m_attachedSlice->squelchOn() ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(m_attachedSlice->squelchLevel())
        .arg(m_attachedSlice->audioGain(), 0, 'f', 0)
        .arg(m_attachedSlice->agcMode())
        .arg(m_attachedSlice->agcThreshold()));
}

void Ax25HfPacketDecodeDialog::appendSystemLine(const QString& text)
{
    if (!m_log)
        return;
    qCDebug(lcAx25).noquote() << text;
    m_log->append(QStringLiteral(
        "<span style=\"color:#63d47a;\">%1</span>&nbsp;&nbsp;"
        "<span style=\"color:#8190a3;\">MODEM</span>&nbsp;&nbsp;"
        "<span style=\"color:#9aa7ba;\">%2</span>")
        .arg(utcClock().toHtmlEscaped(), text.toHtmlEscaped()));
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void Ax25HfPacketDecodeDialog::appendTransmitLine(const Ax25TransmitFrame& frame)
{
    if (!m_log)
        return;

    QString route = frame.source + QStringLiteral(" > ") + frame.destination;
    if (!frame.path.isEmpty())
        route += QStringLiteral(",") + frame.path.join(QStringLiteral(","));
    const QString payload = frame.payloadText.isEmpty()
        ? QStringLiteral("[%1]").arg(frame.payloadHex)
        : frame.payloadText;

    m_log->append(QStringLiteral(
        "<span style=\"color:#63d47a;\">%1</span>&nbsp;&nbsp;"
        "<span style=\"color:#74df87;\">TX</span>&nbsp;&nbsp;"
        "<span style=\"color:#c9d3e2;\">%2:</span>&nbsp;&nbsp;"
        "<span style=\"color:#b5bfce;\">%3</span>")
        .arg(utcClock().toHtmlEscaped(),
             route.toHtmlEscaped(),
             payload.toHtmlEscaped()));
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

void Ax25HfPacketDecodeDialog::appendDiagnosticsLine(const Ax25DecoderDiagnostics& diagnostics)
{
    if (!m_log || !m_diagnosticsDebugEnabled)
        return;

    const QString state = diagnostics.inFrame
        ? QStringLiteral("frame")
        : diagnostics.inPreamble ? QStringLiteral("preamble") : QStringLiteral("search");
    const QString dominantTone = std::abs(diagnostics.markMinusSpaceDb) < 3.0
        ? QStringLiteral("mixed")
        : diagnostics.markMinusSpaceDb > 0.0 ? QStringLiteral("mark") : QStringLiteral("space");
    QString line = QStringLiteral(
        "rms=%1 dBFS pk=%2 dBFS clip=%3% tone%4=%5 dBFS tone%6=%7 dBFS dTone=%8 dB dom=%9 gate=%10 gateRms=%11 floor=%12 lanes=%13 symbols=%14 conf=%15 ones=%16% state=%17 bits=%18 starts=%19 hdlc=%20 ax25=%21 ok=%22 reject=%23")
        .arg(diagnostics.rmsDbfs, 0, 'f', 1)
        .arg(diagnostics.peakDbfs, 0, 'f', 1)
        .arg(diagnostics.clippedPercent, 0, 'f', 2)
        .arg(diagnostics.markToneHz, 0, 'f', 0)
        .arg(diagnostics.markToneDbfs, 0, 'f', 1)
        .arg(diagnostics.spaceToneHz, 0, 'f', 0)
        .arg(diagnostics.spaceToneDbfs, 0, 'f', 1)
        .arg(diagnostics.markMinusSpaceDb, 0, 'f', 1)
        .arg(dominantTone)
        .arg(diagnostics.receiveGateOpen ? QStringLiteral("open") : QStringLiteral("idle"))
        .arg(diagnostics.receiveGateRmsDbfs, 0, 'f', 1)
        .arg(diagnostics.receiveGateFloorDbfs, 0, 'f', 1)
        .arg(diagnostics.decodeLanes)
        .arg(diagnostics.demodSymbols)
        .arg(diagnostics.averageConfidence, 0, 'f', 2)
        .arg(diagnostics.onesPercent, 0, 'f', 1)
        .arg(state)
        .arg(diagnostics.currentFrameBits)
        .arg(diagnostics.hdlcFrameStarts)
        .arg(diagnostics.hdlcFrameCandidates)
        .arg(diagnostics.plausibleAx25Candidates)
        .arg(diagnostics.framesAccepted)
        .arg(diagnostics.decodeRejected);
    line += QStringLiteral(" short=%1 badFcs=%2 malformed=%3")
        .arg(diagnostics.rejectTooShort)
        .arg(diagnostics.rejectBadFcs)
        .arg(diagnostics.rejectMalformed);
    if (!diagnostics.lastRejectReason.isEmpty()) {
        line += QStringLiteral(" last=%1 bytes=%2 bits=%3 fcs=%4/%5 head=%6")
            .arg(diagnostics.lastRejectReason)
            .arg(diagnostics.lastRejectFrameBytes)
            .arg(diagnostics.lastRejectFrameBits)
            .arg(diagnostics.lastRejectActualFcs.isEmpty()
                 ? QStringLiteral("-")
                 : diagnostics.lastRejectActualFcs)
            .arg(diagnostics.lastRejectExpectedFcs.isEmpty()
                 ? QStringLiteral("-")
                 : diagnostics.lastRejectExpectedFcs)
            .arg(diagnostics.lastRejectPreviewHex);
    }
    qCDebug(lcAx25).noquote() << line;
    m_log->append(QStringLiteral(
        "<span style=\"color:#63d47a;\">%1</span>&nbsp;&nbsp;"
        "<span style=\"color:#8ea0b8;\">DIAG</span>&nbsp;&nbsp;"
        "<span style=\"color:#9aa7ba;\">%2</span>")
        .arg(utcClock().toHtmlEscaped(), line.toHtmlEscaped()));
    m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
}

QString Ax25HfPacketDecodeDialog::defaultTransmitSource() const
{
    if (m_radio) {
        const QString callsign = m_radio->callsign().trimmed().toUpper();
        if (!callsign.isEmpty())
            return callsign;
    }
    return QStringLiteral("NOCALL");
}

QString Ax25HfPacketDecodeDialog::transmitSliceSummary() const
{
    if (!m_radio)
        return QStringLiteral("no radio");

    for (auto* slice : m_radio->slices()) {
        if (!slice || !slice->isTxSlice())
            continue;
        return QStringLiteral("slice %1 %2 MHz %3")
            .arg(slice->sliceId())
            .arg(slice->frequency(), 0, 'f', 6)
            .arg(slice->mode());
    }
    if (m_attachedSlice) {
        return QStringLiteral("attached slice %1 %2 MHz %3")
            .arg(m_attachedSlice->sliceId())
            .arg(m_attachedSlice->frequency(), 0, 'f', 6)
            .arg(m_attachedSlice->mode());
    }
    return QStringLiteral("no TX slice");
}

QString Ax25HfPacketDecodeDialog::formatTerminalLine(const Ax25DecodedFrame& frame) const
{
    const QString time = frame.timestampUtc.toUTC().toString(QStringLiteral("HH:mm:ss"));
    QString route = frame.source + QStringLiteral(" > ") + frame.destination;
    if (!frame.path.isEmpty())
        route += QStringLiteral(",") + frame.path.join(QStringLiteral(","));

    const QString payload = frame.payloadText.isEmpty()
        ? QStringLiteral("[%1]").arg(frame.payloadHex)
        : frame.payloadText;

    return QStringLiteral(
        "<span style=\"color:#63d47a;\">%1</span>&nbsp;&nbsp;"
        "<span style=\"color:#9dd6dc;\">RX</span>&nbsp;&nbsp;"
        "<span style=\"color:#c9d3e2;\">%2:</span>&nbsp;&nbsp;"
        "<span style=\"color:#b5bfce;\">%3</span>")
        .arg(time.toHtmlEscaped(),
             route.toHtmlEscaped(),
             payload.toHtmlEscaped());
}

QWidget* Ax25HfPacketDecodeDialog::buildKissTncPage()
{
    auto* page = new QWidget(m_tabStack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* controlsFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* controls = new QHBoxLayout(controlsFrame);
    controls->setContentsMargins(16, 14, 16, 14);
    controls->setSpacing(20);

    auto* serverCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* serverLayout = new QVBoxLayout(serverCell);
    serverLayout->setContentsMargins(0, 0, 20, 0);
    serverLayout->setSpacing(12);
    serverLayout->addWidget(sectionLabel(QStringLiteral("KISS TNC SERVER"), serverCell));
    m_tncEnable = new QCheckBox(QStringLiteral("Enable TNC"), serverCell);
    serverLayout->addWidget(m_tncEnable);
    m_tncStartOnStartup = new QCheckBox(QStringLiteral("Start TNC on Startup"), serverCell);
    serverLayout->addWidget(m_tncStartOnStartup);
    controls->addWidget(serverCell, 2);

    auto* portCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* portLayout = new QVBoxLayout(portCell);
    portLayout->setContentsMargins(0, 0, 20, 0);
    portLayout->setSpacing(12);
    portLayout->addWidget(sectionLabel(QStringLiteral("TCP PORT"), portCell));
    m_tncPort = new QSpinBox(portCell);
    m_tncPort->setRange(TncSettings::kMinPort, TncSettings::kMaxPort);
    m_tncPort->setValue(TncSettings::kDefaultPort);
    m_tncPort->setMaximumWidth(140);
    portLayout->addWidget(m_tncPort);
    controls->addWidget(portCell, 1);
    controls->addStretch(2);
    layout->addWidget(controlsFrame);

    auto* statusFrame = statusPanel(QStringLiteral("TNC STATUS"),
                                    &m_tncStatusDot, &m_tncStatusValue, page);
    layout->addWidget(statusFrame);

    auto* help = new QLabel(
        QStringLiteral("Point a KISS-over-TCP client (Xastir, YAAC, APRSdroid, UISS, Dire Wolf "
                       "clients, terminal/packet programs, …) at this host and TCP port. Decoded "
                       "frames are pushed to every connected client; frames a client sends are "
                       "keyed onto the air using the baud profile selected on the AX.25 tab. "
                       "The modem must be enabled with a slice attached for the TNC to carry "
                       "traffic — enabling the TNC turns the modem on for you."),
        page);
    help->setObjectName(QStringLiteral("StatusValue"));
    help->setWordWrap(true);
    layout->addWidget(help);
    layout->addStretch(1);

    // Seed control values from settings (before signals are wired in the ctor).
    m_tncPort->setValue(TncSettings::port());
    m_tncStartOnStartup->setChecked(TncSettings::startOnStartup());

    return page;
}

void Ax25HfPacketDecodeDialog::setTncEnabled(bool enabled, bool persist)
{
    if (persist) {
        TncSettings::setEnabled(enabled);
    }

    if (enabled) {
        // The TNC needs the modem RX tap running to forward decodes to clients.
        if (m_enableDecode && !m_enableDecode->isChecked()) {
            appendSystemLine(QStringLiteral("Enabling the modem for the KISS TNC."));
            m_enableDecode->setChecked(true);
        }
        const quint16 port = static_cast<quint16>(
            m_tncPort ? m_tncPort->value() : TncSettings::kDefaultPort);
        if (!m_kissServer->start(port) && m_tncEnable) {
            QSignalBlocker blocker(m_tncEnable);
            m_tncEnable->setChecked(false);
        }
    } else {
        m_kissServer->stop();
    }
    refreshTncStatus();
}

void Ax25HfPacketDecodeDialog::applyTncStartOnStartup()
{
    if (TncSettings::startOnStartup() && m_tncEnable) {
        appendSystemLine(QStringLiteral("KISS TNC: start-on-startup enabled; starting listener."));
        m_tncEnable->setChecked(true); // fires setTncEnabled() via the toggled connection
    }
}

void Ax25HfPacketDecodeDialog::handleKissFrameFromClient(const QByteArray& ax25NoFcs)
{
    if (ax25NoFcs.isEmpty())
        return;
    if (!m_audio || !m_radio) {
        appendSystemLine(QStringLiteral("KISS TX dropped: audio engine or radio not ready."));
        qCWarning(lcAx25).noquote()
            << "KISS TX dropped: audio engine or radio not ready (queue size:"
            << m_kissTxQueue.size() << ").";
        return;
    }
    // Cap the queue to prevent a misbehaving KISS client (or a stalled
    // PTT-deny on the radio) from growing it without bound. Drop the
    // oldest pending frame — newer data is more useful than stale
    // backlog. Symmetric with KissTncServer::kMaxWriteBacklogBytes on
    // the RX path.
    while (m_kissTxQueue.size() >= kMaxKissTxQueueDepth) {
        m_kissTxQueue.dequeue();
        appendSystemLine(QStringLiteral(
            "KISS TX queue full (%1 frames); dropping oldest pending frame.")
            .arg(kMaxKissTxQueueDepth));
        qCWarning(lcAx25).noquote()
            << "KISS TX queue full; dropping oldest pending frame. cap="
            << kMaxKissTxQueueDepth;
    }
    m_kissTxQueue.enqueue(ax25NoFcs);
    ++m_kissTxCount;
    refreshTncStatus();
    maybeStartNextKissTx();
}

void Ax25HfPacketDecodeDialog::maybeStartNextKissTx()
{
    if (m_kissTxQueue.isEmpty())
        return;
    if (m_txActive || m_txPendingStream)
        return; // finishTransmit() re-drains when the current TX completes
    if (!m_audio || !m_radio) {
        const int dropped = m_kissTxQueue.size();
        m_kissTxQueue.clear();
        m_kissTxBusyRetries = 0;
        if (dropped > 0) {
            appendSystemLine(QStringLiteral(
                "KISS TX backlog (%1 frames) dropped: audio engine or radio went away.")
                .arg(dropped));
            qCWarning(lcAx25).noquote()
                << "KISS TX backlog dropped — audio/radio not ready. frames="
                << dropped;
        }
        return;
    }
    if (m_radio->isRadioTransmitting() || m_radio->transmitModel().isTransmitting()) {
        ++m_kissTxBusyRetries;
        if (m_kissTxBusyRetries > kMaxKissTxBusyRetries) {
            // Give up on the head-of-queue frame and move on. A stuck PTT
            // shouldn't permanently jam every subsequent frame behind it.
            m_kissTxQueue.dequeue();
            const int retries = m_kissTxBusyRetries;
            m_kissTxBusyRetries = 0;
            appendSystemLine(QStringLiteral(
                "KISS TX abandoned head frame: radio stayed transmitting for "
                "%1 retries (~%2 s); trying next.")
                .arg(retries).arg(retries / 4));
            qCWarning(lcAx25).noquote()
                << "KISS TX abandoned head-of-queue frame after radio-busy "
                   "retries. retries=" << retries
                << "cap=" << kMaxKissTxBusyRetries;
            QTimer::singleShot(0, this, [this] { maybeStartNextKissTx(); });
            return;
        }
        QTimer::singleShot(250, this, [this] { maybeStartNextKissTx(); }); // radio busy; retry
        return;
    }

    const QByteArray frame = m_kissTxQueue.dequeue();
    m_kissTxBusyRetries = 0;
    Ax25TransmitResult tx = ax25BuildTransmitAudioFromFrame(m_shimConfig, frame);
    if (!tx.ok) {
        appendSystemLine(QStringLiteral("KISS TX packetization failed: %1.").arg(tx.error));
        qCWarning(lcAx25).noquote() << "KISS TX packetization failed:" << tx.error;
        QTimer::singleShot(0, this, [this] { maybeStartNextKissTx(); }); // skip to next frame
        return;
    }
    beginTransmission(tx, true);
}

void Ax25HfPacketDecodeDialog::refreshTncStatus()
{
    if (!m_tncStatusValue)
        return;
    const bool listening = m_kissServer && m_kissServer->isListening();
    if (listening) {
        m_tncStatusValue->setText(QStringLiteral("Listening on %1  |  %2 client(s)  |  RX %3  TX %4")
            .arg(m_kissServer->port())
            .arg(m_kissServer->clientCount())
            .arg(m_kissRxCount)
            .arg(m_kissTxCount));
    } else {
        m_tncStatusValue->setText(QStringLiteral("Stopped"));
    }
    if (m_tncStatusDot) {
        m_tncStatusDot->setFixedSize(12, 12);
        m_tncStatusDot->setStyleSheet(listening
            ? QStringLiteral("background:#5fce66;border-radius:6px;")
            : QStringLiteral("background:#8190a3;border-radius:6px;"));
    }
}

// ---------------------------------------------------------------------------
// TNC Terminal tab (connected-mode AX.25 client)
// ---------------------------------------------------------------------------

QWidget* Ax25HfPacketDecodeDialog::buildTerminalPage()
{
    auto* page = new QWidget(m_tabStack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // --- Connect controls -----------------------------------------------------
    auto* controlsFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* controls = new QHBoxLayout(controlsFrame);
    controls->setContentsMargins(16, 14, 16, 14);
    controls->setSpacing(20);

    auto* callCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* callLayout = new QVBoxLayout(callCell);
    callLayout->setContentsMargins(0, 0, 20, 0);
    callLayout->setSpacing(12);
    callLayout->addWidget(sectionLabel(QStringLiteral("MY CALLSIGN"), callCell));
    m_terminalMyCall = new QLineEdit(callCell);
    m_terminalMyCall->setPlaceholderText(QStringLiteral("e.g. N0CALL-7"));
    m_terminalMyCall->setToolTip(QStringLiteral(
        "Your station callsign-SSID. Outbound connects originate from this address."));
    m_terminalMyCall->setMaximumWidth(200);
    callLayout->addWidget(m_terminalMyCall);
    controls->addWidget(callCell);

    auto* targetCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* targetLayout = new QVBoxLayout(targetCell);
    targetLayout->setContentsMargins(0, 0, 20, 0);
    targetLayout->setSpacing(12);
    targetLayout->addWidget(sectionLabel(QStringLiteral("CONNECT TO"), targetCell));
    auto* targetRow = new QHBoxLayout;
    targetRow->setSpacing(10);
    m_terminalTarget = new QLineEdit(targetCell);
    m_terminalTarget->setPlaceholderText(QStringLiteral("e.g. KX9X-1"));
    m_terminalTarget->setMaximumWidth(180);
    targetRow->addWidget(m_terminalTarget);
    m_terminalConnectButton = new QPushButton(QStringLiteral("Connect"), targetCell);
    m_terminalConnectButton->setMinimumHeight(36);
    targetRow->addWidget(m_terminalConnectButton);
    m_terminalCmdButton = new QPushButton(QStringLiteral("Cmd Mode"), targetCell);
    m_terminalCmdButton->setMinimumHeight(36);
    m_terminalCmdButton->setToolTip(QStringLiteral(
        "Return to the command prompt without disconnecting (the escape action)."));
    targetRow->addWidget(m_terminalCmdButton);
    targetLayout->addLayout(targetRow);

    // Quick-connect: a dropdown of recently-heard stations fills the target box.
    auto* heardRow = new QHBoxLayout;
    heardRow->setSpacing(10);
    m_terminalHeardCombo = new QComboBox(targetCell);
    m_terminalHeardCombo->setMinimumWidth(220);
    m_terminalHeardCombo->setToolTip(QStringLiteral(
        "Stations heard on frequency. Pick one to fill the target callsign."));
    heardRow->addWidget(m_terminalHeardCombo);
    m_terminalMheardButton = new QPushButton(QStringLiteral("MHeard"), targetCell);
    m_terminalMheardButton->setMinimumHeight(36);
    m_terminalMheardButton->setToolTip(QStringLiteral(
        "Print the full heard list (callsign, last heard, last beacon) to the terminal."));
    heardRow->addWidget(m_terminalMheardButton);
    heardRow->addStretch(1);
    targetLayout->addLayout(heardRow);
    controls->addWidget(targetCell, 1);

    // Link parameters (forwarded to the data link) + session logging.
    auto* paramCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* paramLayout = new QVBoxLayout(paramCell);
    paramLayout->setContentsMargins(0, 0, 0, 0);
    paramLayout->setSpacing(12);
    paramLayout->addWidget(sectionLabel(QStringLiteral("LINK PARAMETERS"), paramCell));
    auto* paramRow = new QHBoxLayout;
    paramRow->setSpacing(14);
    auto addSpin = [&](const QString& label, int lo, int hi, int def, const QString& tip) {
        auto* col = new QVBoxLayout;
        col->setSpacing(4);
        auto* cap = new QLabel(label, paramCell);
        cap->setObjectName(QStringLiteral("StatusValue"));
        col->addWidget(cap);
        auto* spin = new QSpinBox(paramCell);
        spin->setRange(lo, hi);
        spin->setValue(def);
        spin->setToolTip(tip);
        col->addWidget(spin);
        paramRow->addLayout(col);
        return spin;
    };
    m_terminalRetrySecs = addSpin(QStringLiteral("Retry s"), 1, 60, kTerminalDefaultRetrySecs,
        QStringLiteral("T1 retransmit timeout in seconds."));
    m_terminalMaxTries = addSpin(QStringLiteral("Tries"), 1, 20, kTerminalDefaultMaxTries,
        QStringLiteral("N2 — retransmit attempts before the link is declared dead."));
    m_terminalPaclen = addSpin(QStringLiteral("Paclen"), 16, 256, kTerminalDefaultPaclen,
        QStringLiteral("Max bytes per I-frame."));
    m_terminalTxTail = addSpin(QStringLiteral("TX Tail ms"), 0, 500, kTxTailDefaultMs,
        QStringLiteral("PTT tail (ms) held after the TX audio before unkey. Lower = we hear "
                       "the peer's next frame sooner on a half-duplex link; too low clips the "
                       "end of our transmission so the peer can't decode it."));
    paramLayout->addLayout(paramRow);
    m_terminalLogEnable = new QCheckBox(QStringLiteral("Log session to file"), paramCell);
    m_terminalLogEnable->setToolTip(QStringLiteral(
        "Tee the transcript to a timestamped file under the TNC store."));
    paramLayout->addWidget(m_terminalLogEnable);
    controls->addWidget(paramCell);
    controls->addStretch(1);
    layout->addWidget(controlsFrame);

    // --- Status ---------------------------------------------------------------
    layout->addWidget(statusPanel(QStringLiteral("TERMINAL"),
                                  &m_terminalStatusDot, &m_terminalStatusValue, page));

    // --- Transcript -----------------------------------------------------------
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(12);

    auto* viewFrame = panel(QStringLiteral("LogFrame"), page);
    auto* viewLayout = new QVBoxLayout(viewFrame);
    viewLayout->setContentsMargins(12, 10, 12, 10);
    viewLayout->setSpacing(0);
    m_terminalView = new QTextEdit(viewFrame);
    m_terminalView->setReadOnly(true);
    m_terminalView->document()->setMaximumBlockCount(5000);
    m_terminalView->setLineWrapMode(QTextEdit::WidgetWidth);
    m_terminalView->setFont(mono);
    m_terminalView->setPlaceholderText(QStringLiteral(
        "Set MY CALLSIGN, enter a target call, and press Connect.  Type HELP for commands."));
    // Right-click menu: Clear the screen, and Command Mode (same as the '~' key).
    m_terminalView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_terminalView, &QTextEdit::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QMenu* menu = m_terminalView->createStandardContextMenu();
        menu->addSeparator();
        QAction* clear = menu->addAction(QStringLiteral("Clear"));
        connect(clear, &QAction::triggered, this, [this] {
            m_terminalView->clear();
            if (m_terminal)
                m_terminal->noteScreenCleared();
        });
        QAction* cmd = menu->addAction(QStringLiteral("Command Mode"));
        cmd->setEnabled(m_terminal && m_terminal->mode() == TncTerminal::Mode::Converse);
        connect(cmd, &QAction::triggered, this, [this] {
            if (m_terminal)
                m_terminal->enterCommandMode();
            m_terminalInput->setFocus();
        });
        menu->exec(m_terminalView->viewport()->mapToGlobal(pos));
        menu->deleteLater();
    });
    viewLayout->addWidget(m_terminalView);
    layout->addWidget(viewFrame, 1);

    // --- Input ----------------------------------------------------------------
    auto* inputFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* inputRow = new QHBoxLayout(inputFrame);
    inputRow->setContentsMargins(16, 12, 16, 12);
    inputRow->setSpacing(12);
    inputRow->addWidget(sectionLabel(QStringLiteral("INPUT"), inputFrame));
    m_terminalInput = new QLineEdit(inputFrame);
    m_terminalInput->setFont(mono);
    m_terminalInput->setPlaceholderText(QStringLiteral(
        "Command mode — type CONNECT <call>, HELP, ..."));
    m_terminalInput->installEventFilter(this); // Up/Down command history
    inputRow->addWidget(m_terminalInput, 1);
    m_terminalSendButton = new QPushButton(QStringLiteral("Send"), inputFrame);
    markTxKeying(m_terminalSendButton);   // sends a packet → keys TX; "Send" matches no keyword (#3646 review)
    m_terminalSendButton->setMinimumHeight(36);
    inputRow->addWidget(m_terminalSendButton);
    layout->addWidget(inputFrame);

    // --- Wiring ---------------------------------------------------------------
    connect(m_terminalInput, &QLineEdit::returnPressed,
            this, &Ax25HfPacketDecodeDialog::submitTerminalInput);
    connect(m_terminalSendButton, &QPushButton::clicked,
            this, &Ax25HfPacketDecodeDialog::submitTerminalInput);
    connect(m_terminalConnectButton, &QPushButton::clicked, this, [this] {
        const QString target = m_terminalTarget->text().trimmed();
        if (target.isEmpty()) {
            m_terminalInput->setFocus();
            return;
        }
        m_terminal->submitLine(QStringLiteral("CONNECT %1").arg(target));
        m_terminalInput->setFocus();
    });
    connect(m_terminalCmdButton, &QPushButton::clicked, this, [this] {
        m_terminal->enterCommandMode();
        m_terminalInput->setFocus();
    });
    connect(m_terminalMyCall, &QLineEdit::editingFinished, this, [this] {
        applyTerminalConfigFromUi(true);
    });
    connect(m_terminalMheardButton, &QPushButton::clicked, this, [this] {
        m_terminal->printMheard();
        m_terminalInput->setFocus();
    });
    connect(m_terminalHeardCombo, qOverload<int>(&QComboBox::activated), this, [this](int) {
        const QString call = m_terminalHeardCombo->currentData().toString();
        if (!call.isEmpty())
            m_terminalTarget->setText(call);
    });
    for (QSpinBox* spin : {m_terminalRetrySecs, m_terminalMaxTries, m_terminalPaclen,
                           m_terminalTxTail}) {
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            applyTerminalConfigFromUi(true);
        });
    }
    connect(m_terminalLogEnable, &QCheckBox::toggled, this, [this](bool on) {
        m_terminal->setLogging(on);
        AppSettings::instance().setValue(kTerminalLogSetting,
            on ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
        // Logging may fail to start (e.g. unwritable dir); reflect reality.
        const QSignalBlocker block(m_terminalLogEnable);
        m_terminalLogEnable->setChecked(m_terminal->isLogging());
    });

    // Restore persisted config.
    m_terminalMyCall->setText(
        AppSettings::instance().value(kTerminalMyCallSetting, QString()).toString());
    m_lastDialedCall =
        AppSettings::instance().value(kTerminalLastCallSetting, QString()).toString();
    m_terminalTarget->setText(m_lastDialedCall); // last BBS, persisted across restarts
    m_terminalRetrySecs->setValue(AppSettings::instance()
        .value(kTerminalRetrySecsSetting, kTerminalDefaultRetrySecs).toInt());
    m_terminalMaxTries->setValue(AppSettings::instance()
        .value(kTerminalMaxTriesSetting, kTerminalDefaultMaxTries).toInt());
    m_terminalPaclen->setValue(AppSettings::instance()
        .value(kTerminalPaclenSetting, kTerminalDefaultPaclen).toInt());
    m_terminalTxTail->setValue(AppSettings::instance()
        .value(kTerminalTxTailSetting, kTxTailDefaultMs).toInt());

    refreshTerminalHeardCombo();
    return page;
}

void Ax25HfPacketDecodeDialog::submitTerminalInput()
{
    if (!m_terminalInput || !m_terminal)
        return;
    const QString line = m_terminalInput->text();
    m_terminalInput->clear();
    if (!line.trimmed().isEmpty()
        && (m_terminalHistory.isEmpty() || m_terminalHistory.last() != line)) {
        m_terminalHistory.append(line);
        if (m_terminalHistory.size() > 100)
            m_terminalHistory.removeFirst();
    }
    m_terminalHistoryIndex = m_terminalHistory.size();
    m_terminal->submitLine(line);
}

void Ax25HfPacketDecodeDialog::applyTerminalConfigFromUi(bool persist)
{
    if (!m_terminal || !m_terminalMyCall)
        return;
    const QString call = m_terminalMyCall->text().trimmed();
    m_terminal->setMyCall(call);
    if (m_terminalRetrySecs)
        m_terminal->setRetryTimeoutMs(m_terminalRetrySecs->value() * 1000);
    if (m_terminalMaxTries)
        m_terminal->setMaxRetries(m_terminalMaxTries->value());
    if (m_terminalPaclen)
        m_terminal->setPaclen(m_terminalPaclen->value());
    if (m_terminalTxTail)
        m_txTailMs = m_terminalTxTail->value(); // applies to the next transmission
    if (persist) {
        AppSettings::instance().setValue(kTerminalMyCallSetting, call);
        if (m_terminalRetrySecs)
            AppSettings::instance().setValue(kTerminalRetrySecsSetting,
                QString::number(m_terminalRetrySecs->value()));
        if (m_terminalMaxTries)
            AppSettings::instance().setValue(kTerminalMaxTriesSetting,
                QString::number(m_terminalMaxTries->value()));
        if (m_terminalPaclen)
            AppSettings::instance().setValue(kTerminalPaclenSetting,
                QString::number(m_terminalPaclen->value()));
        if (m_terminalTxTail)
            AppSettings::instance().setValue(kTerminalTxTailSetting,
                QString::number(m_terminalTxTail->value()));
        AppSettings::instance().save();
    }
    refreshTerminalStatus();
}

void Ax25HfPacketDecodeDialog::refreshTerminalHeardCombo()
{
    if (!m_terminalHeardCombo || !m_heard)
        return;
    const QString keep = m_terminalHeardCombo->currentData().toString();
    const QSignalBlocker block(m_terminalHeardCombo);
    m_terminalHeardCombo->clear();
    m_terminalHeardCombo->addItem(QStringLiteral("— heard stations —"), QString());
    int restore = 0;
    const auto stations = m_heard->stations(50);
    for (const auto& s : stations) {
        const QString call = s.station.toString();
        QString label = QStringLiteral("%1   %2")
            .arg(call, s.utc.toString(QStringLiteral("MM/dd HH:mm")));
        m_terminalHeardCombo->addItem(label, call);
        if (call == keep)
            restore = m_terminalHeardCombo->count() - 1;
    }
    m_terminalHeardCombo->setCurrentIndex(restore);
}

void Ax25HfPacketDecodeDialog::refreshTerminalStatus()
{
    if (!m_terminalStatusValue || !m_terminal)
        return;
    const bool connected = m_terminal->isConnected();
    const bool connecting = m_terminal->isConnecting();
    const bool converse = connected && m_terminal->mode() == TncTerminal::Mode::Converse;

    m_terminalStatusValue->setText(QStringLiteral("%1   |   %2")
        .arg(m_terminal->statusSummary(), m_terminal->linkStats()));
    if (m_terminalStatusDot) {
        m_terminalStatusDot->setFixedSize(12, 12);
        const QString color = connected ? QStringLiteral("#5fce66")
            : (connecting ? QStringLiteral("#e0b341") : QStringLiteral("#8190a3"));
        m_terminalStatusDot->setStyleSheet(
            QStringLiteral("background:%1;border-radius:6px;").arg(color));
    }
    if (m_terminalConnectButton)
        m_terminalConnectButton->setEnabled(!connected && !connecting);
    if (m_terminalCmdButton)
        m_terminalCmdButton->setEnabled(converse);
    if (m_terminalInput) {
        m_terminalInput->setPlaceholderText(converse
            ? QStringLiteral("Connected — type a line to send (or '%1' alone to return to commands)")
                  .arg(m_terminal->escapeChar())
            : QStringLiteral("Command mode — type CONNECT <call>, HELP, ..."));
    }

    // Persist the last BBS we dialed so it pre-fills the target after a restart.
    if (connected || connecting) {
        const QString peer = m_terminal->peerCall();
        if (!peer.isEmpty() && peer != m_lastDialedCall) {
            m_lastDialedCall = peer;
            if (m_terminalTarget)
                m_terminalTarget->setText(peer);
            AppSettings::instance().setValue(kTerminalLastCallSetting, peer);
            AppSettings::instance().save();
        }
    }
}

void Ax25HfPacketDecodeDialog::updateTabChrome(int index)
{
    // The Terminal tab (stack index 2) wants the whole window: hide the shared
    // decode-log panel and give the tab stack the vertical stretch so the
    // transcript fills the viewport. On the APRS tab (index 0) the raw decode
    // log and the raw AX.25 TX row are debug chrome — shown only while Packet
    // Activity Debug is on (click the activity trace to toggle), so the
    // station table gets the full viewport in normal operation. Other tabs
    // keep the log panel below their controls.
    QWidget* page = m_tabStack ? m_tabStack->widget(index) : nullptr;
    const bool terminal = page == m_terminalPage;
    const bool dstar = page == m_dstarPage;
    const bool aprs = page == m_aprsPage;
    const bool logVisible = !terminal && !dstar
        && (!aprs || m_diagnosticsDebugEnabled);
    if (m_logFrame)
        m_logFrame->setVisible(logVisible);
    if (m_txFrame)
        m_txFrame->setVisible(!dstar && m_diagnosticsDebugEnabled);
    if (m_statusBar)
        m_statusBar->setVisible(!dstar);
    if (auto* root = qobject_cast<QVBoxLayout*>(bodyWidget()->layout())) {
        root->setStretchFactor(m_tabStack, !logVisible ? 1 : (aprs ? 3 : 0));
        if (m_logFrame)
            root->setStretchFactor(m_logFrame, logVisible ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// APRS client (APRS tab)
// ---------------------------------------------------------------------------

namespace {

// Compact "how long ago" for the station table: "42s", "5m", "3h 12m", "2d".
QString aprsAgeText(const QDateTime& utc)
{
    if (!utc.isValid())
        return QStringLiteral("—");
    const qint64 secs = qMax<qint64>(0, utc.secsTo(QDateTime::currentDateTimeUtc()));
    if (secs < 60)
        return QStringLiteral("%1s").arg(secs);
    if (secs < 3600)
        return QStringLiteral("%1m").arg(secs / 60);
    if (secs < 86400)
        return QStringLiteral("%1h %2m").arg(secs / 3600).arg((secs % 3600) / 60);
    return QStringLiteral("%1d").arg(secs / 86400);
}

// Station-table column order. TIME is the operator's wall clock (local),
// AGE keeps ticking so staleness is visible without doing date math.
enum AprsColumn {
    kColTime = 0,
    kColCall,
    kColSymbol,
    kColAge,
    kColPackets,
    kColGrid,
    kColDist,
    kColCrsSpd,
    kColText,
    kColCount,
};

// Item data roles for the station table. Qt::UserRole carries the payload
// the rest of the dialog reads (callsign / lastHeard msecs); the rest are
// internal to sorting, age-fading, and icon refresh.
constexpr int kAprsSortRole = Qt::UserRole + 1; // numeric sort key (double)
constexpr int kAprsFadeRole = Qt::UserRole + 2; // last applied fade step
constexpr int kAprsSymRole = Qt::UserRole + 3;  // "S:<table><code>" or "W:<n>"

// QTableWidgetItem sorts by display text; give numeric columns (time, age,
// packets, distance, speed) a real sort key instead.
class AprsSortItem final : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& other) const override
    {
        const QVariant a = data(kAprsSortRole);
        const QVariant b = other.data(kAprsSortRole);
        if (a.isValid() && b.isValid())
            return a.toDouble() < b.toDouble();
        return QTableWidgetItem::operator<(other);
    }
};

// Age-fade: rows at full brightness for 5 minutes, then a linear fade down
// to 50% at 30 minutes, where they stay. Active stations pop; the roster's
// overnight tail recedes without disappearing.
constexpr qint64 kAprsFadeStartSecs = 5 * 60;
constexpr qint64 kAprsFadeEndSecs = 30 * 60;

void applyAprsRowFade(QTableWidget* table, int row)
{
    auto* ageItem = table->item(row, kColAge);
    if (!ageItem)
        return;
    const qint64 secs =
        (QDateTime::currentMSecsSinceEpoch()
         - ageItem->data(Qt::UserRole).toLongLong()) / 1000;
    qreal opacity = 1.0;
    if (secs >= kAprsFadeEndSecs) {
        opacity = 0.5;
    } else if (secs > kAprsFadeStartSecs) {
        opacity = 1.0 - 0.5 * qreal(secs - kAprsFadeStartSecs)
                            / qreal(kAprsFadeEndSecs - kAprsFadeStartSecs);
    }
    // Quantize to 5% steps so the per-second tick only repaints rows whose
    // fade bucket actually changed.
    const int step = qRound(opacity * 20.0);
    if (ageItem->data(kAprsFadeRole).toInt() == step)
        return;
    ageItem->setData(kAprsFadeRole, step);

    QColor text(0xc2, 0xcc, 0xdb);
    text.setAlphaF(step / 20.0);
    for (int col = 0; col < kColCount; ++col) {
        if (auto* item = table->item(row, col))
            item->setForeground(text);
    }
    if (auto* sym = table->item(row, kColSymbol)) {
        const QString spec = sym->data(kAprsSymRole).toString();
        QColor stroke(0xae, 0xb9, 0xcc);
        stroke.setAlphaF(step / 20.0);
        if (spec.startsWith(QLatin1String("W:"))) {
            sym->setIcon(aprsicons::weatherIcon(spec.mid(2).toInt(), stroke));
        } else if (spec.startsWith(QLatin1String("S:")) && spec.size() == 4) {
            sym->setIcon(aprsicons::symbolIcon(spec.at(2).toLatin1(),
                                               spec.at(3).toLatin1(), stroke));
        }
    }
}

// The on-air-common symbols offered for our own beacon; data() carries the
// two-character table+code pair.
struct AprsSymbolChoice { const char* pair; const char* name; };
constexpr AprsSymbolChoice kAprsSymbolChoices[] = {
    { "/-",  "Home" },
    { "/y",  "Yagi at QTH" },
    { "/>",  "Car" },
    { "/j",  "Jeep" },
    { "/k",  "Truck" },
    { "/u",  "Truck (18-wheeler)" },
    { "/v",  "Van" },
    { "/R",  "RV" },
    { "/<",  "Motorcycle" },
    { "/b",  "Bicycle" },
    { "/[",  "Jogger" },
    { "/s",  "Power boat" },
    { "/Y",  "Sailboat" },
    { "/'",  "Small aircraft" },
    { "/O",  "Balloon" },
    { "/r",  "Repeater" },
    { "/_",  "Weather station" },
};

} // namespace

void Ax25HfPacketDecodeDialog::buildAprsUi(QWidget* page, QVBoxLayout* pageLayout)
{
    // ── Station + beacon configuration ────────────────────────────────────
    auto* configFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* config = new QGridLayout(configFrame);
    config->setContentsMargins(16, 12, 16, 12);
    config->setHorizontalSpacing(12);
    config->setVerticalSpacing(8);

    config->addWidget(sectionLabel(QStringLiteral("MY CALLSIGN"), configFrame), 0, 0);
    m_aprsMyCall = new QLineEdit(configFrame);
    m_aprsMyCall->setPlaceholderText(QStringLiteral("N0CALL-9"));
    m_aprsMyCall->setMaximumWidth(140);
    m_aprsMyCall->setText(AprsSettings::myCall());
    config->addWidget(m_aprsMyCall, 1, 0);

    config->addWidget(sectionLabel(QStringLiteral("SYMBOL"), configFrame), 0, 1);
    m_aprsSymbol = new QComboBox(configFrame);
    m_aprsSymbol->setIconSize(QSize(16, 16));
    for (const AprsSymbolChoice& choice : kAprsSymbolChoices) {
        m_aprsSymbol->addItem(aprsicons::symbolIcon(choice.pair[0], choice.pair[1]),
                              QString::fromLatin1(choice.name),
                              QString::fromLatin1(choice.pair));
    }
    const int symbolIndex = m_aprsSymbol->findData(AprsSettings::symbol());
    m_aprsSymbol->setCurrentIndex(qMax(0, symbolIndex));
    config->addWidget(m_aprsSymbol, 1, 1);

    config->addWidget(sectionLabel(QStringLiteral("PATH"), configFrame), 0, 2);
    m_aprsPath = new QLineEdit(configFrame);
    m_aprsPath->setMaximumWidth(180);
    m_aprsPath->setText(AprsSettings::path());
    m_aprsPath->setToolTip(QStringLiteral(
        "Digipeater path for beacons and messages (comma-separated, e.g. WIDE1-1,WIDE2-1)."));
    config->addWidget(m_aprsPath, 1, 2);

    config->addWidget(sectionLabel(QStringLiteral("BEACON"), configFrame), 0, 3, 1, 4);
    m_aprsBeaconEnable = new QCheckBox(QStringLiteral("Every"), configFrame);
    config->addWidget(m_aprsBeaconEnable, 1, 3);
    m_aprsBeaconInterval = new QSpinBox(configFrame);
    m_aprsBeaconInterval->setRange(1, 24 * 60);
    m_aprsBeaconInterval->setSuffix(QStringLiteral(" min"));
    m_aprsBeaconInterval->setValue(AprsSettings::beaconIntervalMinutes());
    config->addWidget(m_aprsBeaconInterval, 1, 4);
    m_aprsBeaconText = new QLineEdit(configFrame);
    m_aprsBeaconText->setPlaceholderText(QStringLiteral("Beacon status text"));
    m_aprsBeaconText->setText(AprsSettings::beaconText());
    config->addWidget(m_aprsBeaconText, 1, 5);
    m_aprsBeaconNow = new QPushButton(QStringLiteral("Beacon Now"), configFrame);
    markTxKeying(m_aprsBeaconNow);   // transmits an APRS beacon → keys TX (#3646)
    config->addWidget(m_aprsBeaconNow, 1, 6);
    config->setColumnStretch(5, 1);

    // Position source row: GPS readout plus the manual fallback fields.
    auto* posRow = new QHBoxLayout;
    posRow->setSpacing(10);
    posRow->addWidget(sectionLabel(QStringLiteral("POSITION"), configFrame));
    m_aprsPositionValue = new QLabel(configFrame);
    m_aprsPositionValue->setObjectName(QStringLiteral("StatusValue"));
    posRow->addWidget(m_aprsPositionValue, 1);
    posRow->addWidget(sectionLabel(QStringLiteral("GRID"), configFrame));
    m_aprsManualGrid = new QLineEdit(configFrame);
    m_aprsManualGrid->setPlaceholderText(QStringLiteral("JN48Qm"));
    m_aprsManualGrid->setMaximumWidth(90);
    m_aprsManualGrid->setToolTip(
        QStringLiteral("Maidenhead grid locator — fills the LAT/LON fields"));
    posRow->addWidget(m_aprsManualGrid);
    posRow->addWidget(sectionLabel(QStringLiteral("MANUAL LAT"), configFrame));
    m_aprsManualLat = new QLineEdit(configFrame);
    m_aprsManualLat->setPlaceholderText(QStringLiteral("48.2700"));
    m_aprsManualLat->setMaximumWidth(110);
    m_aprsManualLat->setValidator(
        new QDoubleValidator(-90.0, 90.0, 6, m_aprsManualLat));
    m_aprsManualLat->setText(AprsSettings::manualLat());
    posRow->addWidget(m_aprsManualLat);
    posRow->addWidget(sectionLabel(QStringLiteral("LON"), configFrame));
    m_aprsManualLon = new QLineEdit(configFrame);
    m_aprsManualLon->setPlaceholderText(QStringLiteral("-116.5600"));
    m_aprsManualLon->setMaximumWidth(110);
    m_aprsManualLon->setValidator(
        new QDoubleValidator(-180.0, 180.0, 6, m_aprsManualLon));
    m_aprsManualLon->setText(AprsSettings::manualLon());
    posRow->addWidget(m_aprsManualLon);

    // Lat/lon is the persisted source of truth; derive the grid box from it on
    // open so it round-trips instead of showing blank. Nothing extra is stored.
    {
        bool initLatOk = false, initLonOk = false;
        const double initLat = m_aprsManualLat->text().toDouble(&initLatOk);
        const double initLon = m_aprsManualLon->text().toDouble(&initLonOk);
        if (initLatOk && initLonOk)
            m_aprsManualGrid->setText(MaidenheadLocator::toMaidenhead(initLat, initLon));
    }

    config->addLayout(posRow, 2, 0, 1, 7);
    pageLayout->addWidget(configFrame);

    // ── Messaging row ──────────────────────────────────────────────────────
    auto* msgFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* msgLayout = new QHBoxLayout(msgFrame);
    msgLayout->setContentsMargins(16, 12, 16, 12);
    msgLayout->setSpacing(12);
    msgLayout->addWidget(sectionLabel(QStringLiteral("MESSAGE"), msgFrame));
    m_aprsMsgTo = new QLineEdit(msgFrame);
    m_aprsMsgTo->setPlaceholderText(QStringLiteral("To (N0CALL-7)"));
    m_aprsMsgTo->setMaximumWidth(150);
    msgLayout->addWidget(m_aprsMsgTo);

    // Message-services picker (#3569): a caret button next to the To field that
    // addresses well-known APRS gateways (SMS, email, Winlink, weather) and
    // seeds the matching command template, so operators don't have to memorize
    // callsigns and syntax. The ISS/satellite entry is routing-only — it shows a
    // path hint rather than mutating the To field or the TX path, which is more
    // surprising to undo (see the issue's "path coupling" note). The body must
    // NOT be uppercased anywhere on the send path: EMAIL-2 addresses and SMSGTE
    // aliases are case-sensitive (sendAprsMessageFromUi only uppercases the
    // addressee).
    struct AprsServiceEntry {
        const char* section;    // category header; empty reuses the prior one
        const char* label;      // menu item text
        const char* objectName; // stable id for the agent automation bridge (#3646)
        const char* addressee;  // To callsign; empty = routing-only (hint only)
        const char* body;       // seed for the message field; cursor lands at end
        const char* hint;       // one-line "what this does" + char limit
    };
    // objectName scheme is the full word "aprsService…" (not an "aprsSvc…"
    // abbreviation) on purpose: the agent automation bridge's TX-safety guard
    // (#3646) does an unanchored substring match for the CW-keying token "cwx",
    // and "aprsSvc" + "Wx…" spells "…svcWX…" → a false "cwx" hit that would block
    // the (RX-only) weather entry. The spelled-out prefix avoids that collision.
    static const AprsServiceEntry kAprsServices[] = {
        {"SMS", "SMS — text an SMS to a phone", "aprsServiceSms",
         "SMS", "@",
         "SMS (NA7Q gateway): \"@<10-digit-number> <message>\" (~67 chars). Replies "
         "return as APRS. Make an alias with \"#alias #add <name> <number>\"."},
        {"Email", "EMAIL-2 — send an email", "aprsServiceEmail2",
         "EMAIL-2", "",
         "EMAIL-2: \"<email@address> <message>\" (~67 chars). Send your own address "
         "once to register; \"get\" to EMAIL-2 retrieves held replies (~24h)."},
        {"Winlink", "WLNK-1 — APRSLink help (?)", "aprsServiceWlnkHelp",
         "WLNK-1", "?",
         "WLNK-1 APRSLink: send \"?\" for the command list (L, R#, SP, SMS, …)."},
        {"", "WLNK-1 — one-line email/SMS", "aprsServiceWlnkSms",
         "WLNK-1", "SMS ",
         "WLNK-1: \"SMS <email-or-callsign> <message>\" sends a one-line message "
         "over the Winlink radio-email bridge."},
        {"Weather", "WXBOT — forecast / METAR", "aprsServiceWxbot",
         "WXBOT", "",
         "WXBOT: \"Boston,MA\", \"KJFK\" (ICAO→METAR), a Maidenhead grid, or blank "
         "for your last-heard position. Comma required for City,ST."},
        {"Satellite / ISS", "ISS / ARISS digipeater — path hint", "aprsServiceIss",
         "", "",
         "ISS/ARISS digi (145.825 MHz): set TX PATH to \"ARISS\" (RS0ISS), not "
         "WIDE1-1,WIDE2-1. Keep messages ≤~60 chars; best on passes >30°."},
    };

    auto* serviceMenu = new QMenu(msgFrame);
    QString lastSection;
    for (const AprsServiceEntry& e : kAprsServices) {
        const QString section = QString::fromLatin1(e.section);
        if (!section.isEmpty() && section != lastSection) {
            serviceMenu->addSection(section);
            lastSection = section;
        }
        QAction* act = serviceMenu->addAction(QString::fromLatin1(e.label));
        act->setObjectName(QString::fromLatin1(e.objectName));
        act->setToolTip(QString::fromLatin1(e.hint));
        const QString addressee = QString::fromLatin1(e.addressee);
        const QString body = QString::fromLatin1(e.body);
        const QString hint = QString::fromLatin1(e.hint);
        connect(act, &QAction::triggered, this,
                [this, addressee, body, hint] { seedAprsService(addressee, body, hint); });
    }

    m_aprsServiceButton = new QToolButton(msgFrame);
    m_aprsServiceButton->setObjectName(QStringLiteral("AprsServiceMenuButton"));
    m_aprsServiceButton->setAccessibleName(QStringLiteral("APRS message services"));
    m_aprsServiceButton->setText(QStringLiteral("▾"));
    m_aprsServiceButton->setToolTip(QStringLiteral(
        "Address a common APRS service: SMS, email, Winlink, weather, ISS"));
    m_aprsServiceButton->setMenu(serviceMenu);
    m_aprsServiceButton->setPopupMode(QToolButton::InstantPopup);
    // A human mouse press opens the menu via Qt's built-in InstantPopup path.
    // We deliberately do NOT wire clicked()→open for the agent automation bridge
    // (#3646): a synthetic click arrives inside the bridge's socket-read callback,
    // and showing the menu's native popup window from there re-enters the macOS
    // GUI stack and segfaults intermittently (QMenu::popup → QCocoaWindow::
    // setVisible → QWindow::geometry). Driving this dropdown is a documented
    // bridge gap; the menu actions still carry stable objectNames so the bridge
    // can verify the seeded fields structurally.
    msgLayout->addWidget(m_aprsServiceButton);

    m_aprsMsgText = new QLineEdit(msgFrame);
    m_aprsMsgText->setPlaceholderText(
        QStringLiteral("Message text (sent with ack request, retries until acked)"));
    msgLayout->addWidget(m_aprsMsgText, 1);
    m_aprsMsgSend = new QPushButton(QStringLiteral("Send APRS Msg"), msgFrame);
    markTxKeying(m_aprsMsgSend);   // transmits an APRS message → keys TX (#3646)
    m_aprsMsgSend->setMinimumHeight(42);
    msgLayout->addWidget(m_aprsMsgSend);
    m_aprsEnvelope = new QPushButton(QStringLiteral("✉"), msgFrame);
    m_aprsEnvelope->setObjectName(QStringLiteral("EnvelopeButton"));
    m_aprsEnvelope->setMinimumHeight(42);
    m_aprsEnvelope->setToolTip(QStringLiteral("View APRS messages"));
    msgLayout->addWidget(m_aprsEnvelope);
    pageLayout->addWidget(msgFrame);

    // ── Station table ──────────────────────────────────────────────────────
    auto* tableFrame = panel(QStringLiteral("LogFrame"), page);
    auto* tableLayout = new QVBoxLayout(tableFrame);
    tableLayout->setContentsMargins(8, 6, 8, 6);
    tableLayout->setSpacing(0);
    m_aprsTable = new QTableWidget(tableFrame);
    m_aprsTable->setColumnCount(kColCount);
    m_aprsTable->setHorizontalHeaderLabels({
        QStringLiteral("TIME"), QStringLiteral("STATION"),
        QStringLiteral("SYMBOL"), QStringLiteral("AGE"),
        QStringLiteral("PKTS"), QStringLiteral("GRID"),
        QStringLiteral("DIST"), QStringLiteral("CRS/SPD"),
        QStringLiteral("STATUS / COMMENT"),
    });
    m_aprsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_aprsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_aprsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_aprsTable->verticalHeader()->setVisible(false);
    m_aprsTable->verticalHeader()->setDefaultSectionSize(24);
    m_aprsTable->setAlternatingRowColors(true);
    m_aprsTable->setIconSize(QSize(16, 16));
    m_aprsTable->setShowGrid(false);
    m_aprsTable->setWordWrap(false);
    m_aprsTable->horizontalHeader()->setSectionResizeMode(kColText, QHeaderView::Stretch);
    m_aprsTable->setMinimumHeight(160);
    // Sortable headers; default order is most-recently-heard first. The
    // rebuild re-applies whatever column/order the operator last clicked.
    m_aprsTable->setSortingEnabled(true);
    m_aprsTable->horizontalHeader()->setSortIndicator(kColTime, Qt::DescendingOrder);
    m_aprsTable->horizontalHeader()->setSortIndicatorShown(true);
    m_aprsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    tableLayout->addWidget(m_aprsTable);
    pageLayout->addWidget(tableFrame, 1);

    // ── Control wiring ─────────────────────────────────────────────────────
    connect(m_aprsMyCall, &QLineEdit::editingFinished,
            this, [this] { applyAprsConfigFromUi(true); });
    connect(m_aprsSymbol, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { applyAprsConfigFromUi(true); });
    connect(m_aprsPath, &QLineEdit::editingFinished,
            this, [this] { applyAprsConfigFromUi(true); });
    connect(m_aprsBeaconEnable, &QCheckBox::toggled,
            this, [this](bool) { applyAprsConfigFromUi(true); });
    connect(m_aprsBeaconInterval, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) { applyAprsConfigFromUi(true); });
    connect(m_aprsBeaconText, &QLineEdit::editingFinished,
            this, [this] { applyAprsConfigFromUi(true); });
    connect(m_aprsManualGrid, &QLineEdit::editingFinished,
            this, [this] {
        const QString grid = m_aprsManualGrid->text().trimmed();
        if (grid.isEmpty())
            return;
        double lat = 0.0, lon = 0.0;
        if (!MaidenheadLocator::toLatLon(grid, lat, lon)) {
            m_aprsManualGrid->setStyleSheet(
                QStringLiteral("QLineEdit { color: #c04040; }"));
            return;
        }
        m_aprsManualGrid->setStyleSheet(QString());
        m_aprsManualLat->setText(QString::number(lat, 'f', 4));
        m_aprsManualLon->setText(QString::number(lon, 'f', 4));
        applyAprsConfigFromUi(true);
    });
    connect(m_aprsManualLat, &QLineEdit::editingFinished,
            this, [this] { applyAprsConfigFromUi(true); });
    connect(m_aprsManualLon, &QLineEdit::editingFinished,
            this, [this] { applyAprsConfigFromUi(true); });
    connect(m_aprsBeaconNow, &QPushButton::clicked, this, [this] {
        applyAprsConfigFromUi(false); // pick up anything typed but not committed
        m_aprsBeacon->sendNow();
    });
    connect(m_aprsMsgSend, &QPushButton::clicked,
            this, &Ax25HfPacketDecodeDialog::sendAprsMessageFromUi);
    connect(m_aprsMsgText, &QLineEdit::returnPressed,
            this, &Ax25HfPacketDecodeDialog::sendAprsMessageFromUi);
    connect(m_aprsEnvelope, &QPushButton::clicked,
            this, &Ax25HfPacketDecodeDialog::openAprsMessagesDialog);
    connect(m_aprsTable, &QTableWidget::customContextMenuRequested,
            this, &Ax25HfPacketDecodeDialog::handleAprsStationMenu);
    connect(m_aprsTable, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) {
        if (auto* item = m_aprsTable->item(row, kColCall)) {
            m_aprsMsgTo->setText(item->data(Qt::UserRole).toString());
            m_aprsMsgText->setFocus();
        }
    });
}

void Ax25HfPacketDecodeDialog::applyAprsConfigFromUi(bool persist)
{
    const QString call = m_aprsMyCall->text().trimmed().toUpper();
    const auto myAddr = ax25::Address::parse(call);
    const ax25::Address addr = myAddr.value_or(ax25::Address{});
    m_aprsMessenger->setMyAddress(addr);
    m_aprsBeacon->setMyAddress(addr);

    QString symbol = m_aprsSymbol->currentData().toString();
    if (symbol.size() != 2)
        symbol = QStringLiteral("/-");
    m_aprsBeacon->setSymbol(symbol.at(0).toLatin1(), symbol.at(1).toLatin1());

    QVector<ax25::Address> path;
    const QStringList hops =
        m_aprsPath->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& hop : hops) {
        if (path.size() >= 8)
            break;
        if (const auto a = ax25::Address::parse(hop.trimmed().toUpper()))
            path.append(*a);
    }
    m_aprsBeacon->setPath(path);
    m_aprsMessenger->setPath(path);

    m_aprsBeacon->setStatusText(m_aprsBeaconText->text());
    m_aprsBeacon->setIntervalMinutes(m_aprsBeaconInterval->value());
    m_aprsBeacon->setEnabled(m_aprsBeaconEnable->isChecked());

    bool latOk = false, lonOk = false;
    const double manualLat = m_aprsManualLat->text().toDouble(&latOk);
    const double manualLon = m_aprsManualLon->text().toDouble(&lonOk);
    const bool manualValid = latOk && lonOk
        && manualLat >= -90.0 && manualLat <= 90.0
        && manualLon >= -180.0 && manualLon <= 180.0
        && (manualLat != 0.0 || manualLon != 0.0);
    m_aprsBeacon->setManualPosition(manualLat, manualLon, manualValid);

    if (persist) {
        AprsSettings::setMyCall(call);
        AprsSettings::setSymbol(symbol);
        AprsSettings::setPath(m_aprsPath->text());
        AprsSettings::setBeaconEnabled(m_aprsBeaconEnable->isChecked());
        AprsSettings::setBeaconIntervalMinutes(m_aprsBeaconInterval->value());
        AprsSettings::setBeaconText(m_aprsBeaconText->text());
        AprsSettings::setManualPosition(m_aprsManualLat->text(),
                                        m_aprsManualLon->text());
    }
    refreshAprsPositionLabel();
    refreshAprsStationTable(); // distance column tracks our own position
}

void Ax25HfPacketDecodeDialog::handleGpsUpdate()
{
    if (!m_radio || !m_aprsBeacon)
        return;
    // The GPSDO reports "N 33 33.484" (hemisphere, degrees, decimal
    // minutes), not decimal degrees; parseGpsCoordinate accepts both forms.
    double lat = 0.0, lon = 0.0;
    const bool latOk = aprs::parseGpsCoordinate(m_radio->gpsLat(), lat);
    const bool lonOk = aprs::parseGpsCoordinate(m_radio->gpsLon(), lon);
    // "Fine Lock" / "Coarse Lock" mean the fix is real; "Present" /
    // "Not Present" mean no usable position.
    const bool locked =
        m_radio->gpsStatus().contains(QStringLiteral("Lock"), Qt::CaseInsensitive);
    const bool valid = latOk && lonOk && locked && (lat != 0.0 || lon != 0.0);
    m_aprsBeacon->setGpsPosition(lat, lon, valid);
    refreshAprsPositionLabel();
}

void Ax25HfPacketDecodeDialog::refreshAprsPositionLabel()
{
    if (!m_aprsPositionValue)
        return;
    double lat = 0.0, lon = 0.0;
    if (m_aprsBeacon->currentPosition(lat, lon)) {
        m_aprsPositionValue->setText(QStringLiteral("%1  %2  %3, %4")
            .arg(m_aprsBeacon->usingGps() ? QStringLiteral("GPS")
                                          : QStringLiteral("Manual"),
                 aprs::gridSquare(lat, lon),
                 QString::number(lat, 'f', 4),
                 QString::number(lon, 'f', 4)));
    } else {
        m_aprsPositionValue->setText(QStringLiteral(
            "none — no GPS lock; enter a manual Lat/Lon to beacon"));
    }
}

void Ax25HfPacketDecodeDialog::refreshAprsStationTable()
{
    if (!m_aprsTable || !m_aprsStations)
        return;

    QString selectedCall;
    if (const auto* current = m_aprsTable->item(m_aprsTable->currentRow(), kColCall))
        selectedCall = current->data(Qt::UserRole).toString();

    double myLat = 0.0, myLon = 0.0;
    const bool haveMyPos =
        m_aprsBeacon && m_aprsBeacon->currentPosition(myLat, myLon);

    const QVector<AprsStationList::Station> stations = m_aprsStations->stations();
    m_aprsTable->setUpdatesEnabled(false);
    // Populating with sorting live makes rows re-sort mid-insert; pause it
    // and re-apply the operator's chosen column/order at the end.
    const int sortColumn = m_aprsTable->horizontalHeader()->sortIndicatorSection();
    const Qt::SortOrder sortOrder = m_aprsTable->horizontalHeader()->sortIndicatorOrder();
    m_aprsTable->setSortingEnabled(false);
    m_aprsTable->setRowCount(stations.size());
    for (int i = 0; i < stations.size(); ++i) {
        const AprsStationList::Station& s = stations.at(i);
        auto set = [&](int col, const QString& text) {
            auto* item = new AprsSortItem(text);
            m_aprsTable->setItem(i, col, item);
            return item;
        };
        const double heardMs = double(s.lastHeard.toMSecsSinceEpoch());
        auto* timeItem = set(kColTime,
            s.lastHeard.toLocalTime().toString(QStringLiteral("MM/dd HH:mm:ss")));
        timeItem->setData(kAprsSortRole, heardMs);
        set(kColCall, s.call)->setData(Qt::UserRole, s.call);

        QTableWidgetItem* symItem = nullptr;
        if (s.isWeather) {
            static const char* kWxNames[] = {
                "Weather", "Weather (rain)", "Weather (windy)" };
            const int cond = qBound(0, s.wxCondition, 2);
            symItem = set(kColSymbol, QString::fromLatin1(kWxNames[cond]));
            symItem->setIcon(aprsicons::weatherIcon(cond));
            symItem->setData(kAprsSymRole, QStringLiteral("W:%1").arg(cond));
        } else if (s.hasPosition) {
            symItem = set(kColSymbol,
                          aprs::symbolDescription(s.symbolTable, s.symbolCode));
            symItem->setIcon(aprsicons::symbolIcon(s.symbolTable, s.symbolCode));
            symItem->setData(kAprsSymRole,
                             QStringLiteral("S:%1%2")
                                 .arg(QLatin1Char(s.symbolTable))
                                 .arg(QLatin1Char(s.symbolCode)));
        } else {
            set(kColSymbol, aprs::packetTypeName(s.lastType));
        }

        auto* ageItem = set(kColAge, aprsAgeText(s.lastHeard));
        ageItem->setData(Qt::UserRole, s.lastHeard.toMSecsSinceEpoch());
        ageItem->setData(kAprsSortRole, -heardMs); // ascending age = newest first
        set(kColPackets, QString::number(s.packets))
            ->setData(kAprsSortRole, double(s.packets));
        set(kColGrid, s.hasPosition ? aprs::gridSquare(s.latitude, s.longitude)
                                    : QString());
        QString dist;
        double distKey = 1e12; // unknown distances sort to the bottom
        if (s.hasPosition && haveMyPos) {
            const double miles =
                aprs::distanceMiles(myLat, myLon, s.latitude, s.longitude);
            dist = QStringLiteral("%1 mi %2°")
                .arg(miles, 0, 'f', 1)
                .arg(qRound(aprs::bearingDeg(myLat, myLon,
                                             s.latitude, s.longitude)));
            distKey = miles;
        }
        set(kColDist, dist)->setData(kAprsSortRole, distKey);
        QString crsSpd;
        double speedKey = -1.0;
        if (s.speedKnots >= 0.0) {
            const double mph = s.speedKnots * 1.15078;
            crsSpd = QStringLiteral("%1 mph").arg(qRound(mph));
            if (s.courseDeg >= 0.0)
                crsSpd += QStringLiteral(" %1°").arg(qRound(s.courseDeg));
            speedKey = mph;
        }
        set(kColCrsSpd, crsSpd)->setData(kAprsSortRole, speedKey);
        QString text = !s.comment.isEmpty() ? s.comment : s.status;
        if (!s.comment.isEmpty() && !s.status.isEmpty())
            text = s.comment + QStringLiteral("  |  ") + s.status;
        set(kColText, text);
    }
    m_aprsTable->setSortingEnabled(true);
    m_aprsTable->sortItems(sortColumn, sortOrder);
    m_aprsTable->resizeColumnsToContents();
    // resizeColumnsToContents() packs columns shoulder-to-shoulder; pad each
    // one so the table breathes. The last column stretches regardless.
    for (int col = 0; col < kColText; ++col)
        m_aprsTable->setColumnWidth(col, m_aprsTable->columnWidth(col) + 18);
    m_aprsTable->horizontalHeader()->setSectionResizeMode(kColText, QHeaderView::Stretch);
    for (int row = 0; row < m_aprsTable->rowCount(); ++row) {
        applyAprsRowFade(m_aprsTable, row);
        if (!selectedCall.isEmpty()) {
            const auto* callItem = m_aprsTable->item(row, kColCall);
            if (callItem && callItem->data(Qt::UserRole).toString() == selectedCall)
                m_aprsTable->selectRow(row);
        }
    }
    m_aprsTable->setUpdatesEnabled(true);
}

void Ax25HfPacketDecodeDialog::refreshAprsStationAges()
{
    if (!m_aprsTable)
        return;
    for (int row = 0; row < m_aprsTable->rowCount(); ++row) {
        if (auto* item = m_aprsTable->item(row, kColAge)) {
            // QTimeZone::utc() rather than the Qt 6.5+ QTimeZone::UTC constant —
            // the Linux CI image builds against an older Qt 6.
            const QDateTime lastHeard = QDateTime::fromMSecsSinceEpoch(
                item->data(Qt::UserRole).toLongLong(), QTimeZone::utc());
            item->setText(aprsAgeText(lastHeard));
        }
        applyAprsRowFade(m_aprsTable, row);
    }
}

void Ax25HfPacketDecodeDialog::handleAprsStationMenu(const QPoint& pos)
{
    QMenu menu(m_aprsTable);

    // Station-specific actions only when the click landed on a row; the
    // roster-wide actions below are available from anywhere in the table.
    QString call;
    if (auto* hit = m_aprsTable->itemAt(pos)) {
        if (const auto* callItem = m_aprsTable->item(hit->row(), kColCall))
            call = callItem->data(Qt::UserRole).toString();
    }
    if (!call.isEmpty()) {
        menu.addAction(QStringLiteral("Send Message to %1...").arg(call),
                       this, [this, call] {
            m_aprsMsgTo->setText(call);
            m_aprsMsgText->setFocus();
        });
        menu.addAction(QStringLiteral("Station Info..."), this, [this, call] {
            showAprsStationInfo(call);
        });
        menu.addAction(QStringLiteral("View on aprs.fi"), this, [call] {
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://aprs.fi/info/a/%1").arg(call)));
        });
        menu.addSeparator();
    }
    QAction* clearAll =
        menu.addAction(QStringLiteral("Clear All Stations..."), this, [this] {
        const int count = m_aprsStations->size();
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Clear APRS Stations"),
            QStringLiteral("Remove all %1 heard station%2? "
                           "This also clears the saved roster on disk.")
                .arg(count)
                .arg(count == 1 ? QString() : QStringLiteral("s")));
        if (answer == QMessageBox::Yes)
            m_aprsStations->clear();
    });
    clearAll->setEnabled(m_aprsStations && m_aprsStations->size() > 0);

    menu.exec(m_aprsTable->viewport()->mapToGlobal(pos));
}

void Ax25HfPacketDecodeDialog::showAprsStationInfo(const QString& call)
{
    const auto station = m_aprsStations->find(call);
    if (!station)
        return;
    const AprsStationList::Station& s = *station;

    QStringList lines;
    auto add = [&lines](const QString& key, const QString& value) {
        if (!value.isEmpty())
            lines << QStringLiteral("%1  %2").arg(key.leftJustified(12), value);
    };
    add(QStringLiteral("First heard"),
        s.firstHeard.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")));
    add(QStringLiteral("Last heard"),
        QStringLiteral("%1  (%2 ago)")
            .arg(s.lastHeard.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")),
                 aprsAgeText(s.lastHeard)));
    add(QStringLiteral("Packets"), QString::number(s.packets));
    add(QStringLiteral("Via"), s.via);
    if (s.hasPosition) {
        add(QStringLiteral("Position"),
            QStringLiteral("%1, %2  (%3)")
                .arg(QString::number(s.latitude, 'f', 4),
                     QString::number(s.longitude, 'f', 4),
                     aprs::gridSquare(s.latitude, s.longitude)));
        add(QStringLiteral("Symbol"),
            aprs::symbolDescription(s.symbolTable, s.symbolCode));
        double myLat = 0.0, myLon = 0.0;
        if (m_aprsBeacon && m_aprsBeacon->currentPosition(myLat, myLon)) {
            add(QStringLiteral("Distance"),
                QStringLiteral("%1 mi, bearing %2°")
                    .arg(aprs::distanceMiles(myLat, myLon,
                                             s.latitude, s.longitude),
                         0, 'f', 1)
                    .arg(qRound(aprs::bearingDeg(myLat, myLon,
                                                 s.latitude, s.longitude))));
        }
        if (s.speedKnots >= 0.0) {
            add(QStringLiteral("Speed"),
                QStringLiteral("%1 mph").arg(qRound(s.speedKnots * 1.15078)));
        }
        if (s.courseDeg >= 0.0)
            add(QStringLiteral("Course"),
                QStringLiteral("%1°").arg(qRound(s.courseDeg)));
        if (s.hasAltitude)
            add(QStringLiteral("Altitude"),
                QStringLiteral("%1 ft").arg(qRound(s.altitudeFeet)));
    }
    add(QStringLiteral("Status"), s.status);
    add(QStringLiteral("Comment"), s.comment);
    add(QStringLiteral("Last packet"), s.lastInfo);

    QMessageBox box(this);
    box.setWindowTitle(call);
    box.setText(QStringLiteral("<pre>%1</pre>")
                    .arg(lines.join(QLatin1Char('\n')).toHtmlEscaped()));
    box.setTextFormat(Qt::RichText);
    box.exec();
}

void Ax25HfPacketDecodeDialog::openAprsMessagesDialog()
{
    if (!m_aprsMessagesDialog) {
        m_aprsMessagesDialog = new AprsMessagesDialog(m_aprsMessenger, this);
        connect(m_aprsMessagesDialog, &AprsMessagesDialog::replyRequested,
                this, [this](const QString& callsign) {
            m_aprsMsgTo->setText(callsign);
            raise();
            activateWindow();
            m_aprsMsgText->setFocus();
        });
    }
    m_aprsMessagesDialog->show();
    m_aprsMessagesDialog->raise();
    m_aprsMessagesDialog->activateWindow();
}

void Ax25HfPacketDecodeDialog::sendAprsMessageFromUi()
{
    applyAprsConfigFromUi(false); // pick up a freshly typed MY CALLSIGN
    if (!m_aprsMessenger->myAddress().isValid()) {
        appendSystemLine(QStringLiteral(
            "APRS message not sent: set MY CALLSIGN first."));
        return;
    }
    const QString to = m_aprsMsgTo->text().trimmed().toUpper();
    const QString text = m_aprsMsgText->text().trimmed();
    if (to.isEmpty() || text.isEmpty())
        return;
    if (!m_aprsMessenger->sendMessage(to, text)) {
        appendSystemLine(QStringLiteral(
            "APRS message not sent: \"%1\" is not a valid callsign.").arg(to));
        return;
    }
    m_aprsMsgText->clear();
}

void Ax25HfPacketDecodeDialog::seedAprsService(const QString& addressee,
                                               const QString& body,
                                               const QString& hint)
{
    // Routing-only entries (ISS/satellite) carry no addressee: leave the To and
    // message fields untouched and just surface the hint, so we don't clobber
    // whatever the operator was composing.
    if (!addressee.isEmpty()) {
        m_aprsMsgTo->setText(addressee);
        m_aprsMsgText->setText(body);
        m_aprsMsgText->setFocus();
        m_aprsMsgText->setCursorPosition(body.length());
    }
    if (!hint.isEmpty())
        appendSystemLine(hint);
}

void Ax25HfPacketDecodeDialog::updateAprsEnvelopeButton()
{
    if (!m_aprsEnvelope || !m_aprsMessenger)
        return;
    const int unread = m_aprsMessenger->unreadCount();
    m_aprsEnvelope->setText(unread > 0
        ? QStringLiteral("✉ %1").arg(unread)
        : QStringLiteral("✉"));
    m_aprsEnvelope->setToolTip(unread > 0
        ? QStringLiteral("View APRS messages (%1 unread)").arg(unread)
        : QStringLiteral("View APRS messages"));
    m_aprsEnvelope->setProperty("hasUnread", unread > 0);
    m_aprsEnvelope->style()->unpolish(m_aprsEnvelope);
    m_aprsEnvelope->style()->polish(m_aprsEnvelope);
}

// ---------------------------------------------------------------------------
// Personal Mailbox System (PMS) tab
// ---------------------------------------------------------------------------

QWidget* Ax25HfPacketDecodeDialog::buildMailboxPage()
{
    auto* page = new QWidget(m_tabStack);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* controlsFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* controls = new QHBoxLayout(controlsFrame);
    controls->setContentsMargins(16, 14, 16, 14);
    controls->setSpacing(20);

    auto* mboxCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* mboxLayout = new QVBoxLayout(mboxCell);
    mboxLayout->setContentsMargins(0, 0, 20, 0);
    mboxLayout->setSpacing(12);
    mboxLayout->addWidget(sectionLabel(QStringLiteral("MAILBOX (PMS)"), mboxCell));
    m_pmsEnable = new QCheckBox(QStringLiteral("Enable Mailbox (PMS)"), mboxCell);
    mboxLayout->addWidget(m_pmsEnable);
    m_pmsBeaconEnable = new QCheckBox(QStringLiteral("Send hourly beacon"), mboxCell);
    mboxLayout->addWidget(m_pmsBeaconEnable);
    controls->addWidget(mboxCell, 1);

    auto* callCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* callLayout = new QVBoxLayout(callCell);
    callLayout->setContentsMargins(0, 0, 20, 0);
    callLayout->setSpacing(12);
    callLayout->addWidget(sectionLabel(QStringLiteral("LISTEN CALLSIGN"), callCell));
    m_pmsListenCall = new QLineEdit(callCell);
    m_pmsListenCall->setPlaceholderText(QStringLiteral("e.g. KI6BCJ-10"));
    m_pmsListenCall->setToolTip(QStringLiteral(
        "Full callsign-SSID the mailbox answers on. AX.25 limits a callsign to "
        "6 characters plus an optional -SSID (0-15)."));
    m_pmsListenCall->setMaximumWidth(220);
    callLayout->addWidget(m_pmsListenCall);
    callLayout->addWidget(sectionLabel(QStringLiteral("VANITY ALIAS (OPTIONAL)"), callCell));
    m_pmsAliasCall = new QLineEdit(callCell);
    m_pmsAliasCall->setPlaceholderText(QStringLiteral("e.g. AETBBS (max 6 chars)"));
    m_pmsAliasCall->setToolTip(QStringLiteral(
        "Optional second callsign the mailbox also answers on. AX.25 limits a "
        "callsign to 6 characters plus an optional -SSID."));
    m_pmsAliasCall->setMaximumWidth(220);
    callLayout->addWidget(m_pmsAliasCall);
    controls->addWidget(callCell, 1);
    controls->addStretch(1);
    layout->addWidget(controlsFrame);

    auto* welcomeFrame = panel(QStringLiteral("ControlsFrame"), page);
    auto* welcomeLayout = new QVBoxLayout(welcomeFrame);
    welcomeLayout->setContentsMargins(16, 12, 16, 12);
    welcomeLayout->setSpacing(8);
    welcomeLayout->addWidget(sectionLabel(QStringLiteral("WELCOME / PTEXT"), welcomeFrame));
    m_pmsWelcome = new QLineEdit(welcomeFrame);
    m_pmsWelcome->setPlaceholderText(
        QStringLiteral("Shown to callers after they connect (optional)."));
    welcomeLayout->addWidget(m_pmsWelcome);
    welcomeLayout->addWidget(sectionLabel(QStringLiteral("BEACON TEXT"), welcomeFrame));
    m_pmsBeaconText = new QLineEdit(welcomeFrame);
    m_pmsBeaconText->setPlaceholderText(
        QStringLiteral("Hourly AX.25 beacon announcing the mailbox is online."));
    welcomeLayout->addWidget(m_pmsBeaconText);
    layout->addWidget(welcomeFrame);

    auto* statusFrame = statusPanel(QStringLiteral("MAILBOX STATUS"),
                                    &m_pmsStatusDot, &m_pmsStatusValue, page);
    layout->addWidget(statusFrame);

    // Statistics on the left, Last Callers on the right — each its own panel so
    // the row fills the width evenly.
    auto* infoRow = new QHBoxLayout;
    infoRow->setSpacing(8);

    auto* statsFrame = panel(QStringLiteral("StatusFrame"), page);
    auto* statsLayout = new QVBoxLayout(statsFrame);
    statsLayout->setContentsMargins(16, 12, 16, 12);
    statsLayout->setSpacing(8);
    statsLayout->addWidget(sectionLabel(QStringLiteral("STATISTICS"), statsFrame));
    m_pmsStatsValue = new QLabel(statsFrame);
    m_pmsStatsValue->setObjectName(QStringLiteral("StatusValue"));
    m_pmsStatsValue->setWordWrap(true);
    m_pmsStatsValue->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    statsLayout->addWidget(m_pmsStatsValue, 1);
    infoRow->addWidget(statsFrame, 1);

    auto* callersFrame = panel(QStringLiteral("StatusFrame"), page);
    auto* callersLayout = new QVBoxLayout(callersFrame);
    callersLayout->setContentsMargins(16, 12, 16, 12);
    callersLayout->setSpacing(8);
    callersLayout->addWidget(sectionLabel(QStringLiteral("LAST CALLERS"), callersFrame));
    m_pmsCallersValue = new QLabel(callersFrame);
    m_pmsCallersValue->setObjectName(QStringLiteral("StatusValue"));
    m_pmsCallersValue->setWordWrap(true);
    m_pmsCallersValue->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    callersLayout->addWidget(m_pmsCallersValue, 1);
    infoRow->addWidget(callersFrame, 1);

    layout->addLayout(infoRow);
    layout->addStretch(1);

    // Seed control values from settings (before signals are wired in the ctor).
    // No defaults for the callsign fields — the operator must set them.
    m_pmsListenCall->setText(AppSettings::instance()
        .value(kPmsListenCallSetting, QString()).toString());
    m_pmsAliasCall->setText(AppSettings::instance()
        .value(kPmsAliasCallSetting, QString()).toString());
    m_pmsWelcome->setText(AppSettings::instance()
        .value(kPmsWelcomeSetting, QString()).toString());
    m_pmsBeaconText->setText(AppSettings::instance()
        .value(kPmsBeaconTextSetting,
               QStringLiteral("AetherMailbox online - connect for messages")).toString());
    m_pmsBeaconEnable->setChecked(AppSettings::instance()
        .value(kPmsBeaconEnabledSetting, QStringLiteral("False")).toString()
            == QStringLiteral("True"));

    return page;
}

void Ax25HfPacketDecodeDialog::applyPmsConfigFromUi(bool persist)
{
    if (!m_pms)
        return;
    if (m_pmsListenCall)
        m_pms->setListenCallsign(m_pmsListenCall->text());
    if (m_pmsAliasCall)
        m_pms->setAliasCallsign(m_pmsAliasCall->text());
    if (m_pmsWelcome)
        m_pms->setWelcomeText(m_pmsWelcome->text());
    if (m_pmsBeaconText)
        m_pms->setBeaconText(m_pmsBeaconText->text());
    if (m_pmsBeaconEnable)
        m_pms->setBeaconEnabled(m_pmsBeaconEnable->isChecked());

    if (persist) {
        auto& s = AppSettings::instance();
        if (m_pmsListenCall)
            s.setValue(kPmsListenCallSetting, m_pmsListenCall->text().trimmed().toUpper());
        if (m_pmsAliasCall)
            s.setValue(kPmsAliasCallSetting, m_pmsAliasCall->text().trimmed().toUpper());
        if (m_pmsWelcome)
            s.setValue(kPmsWelcomeSetting, m_pmsWelcome->text());
        if (m_pmsBeaconText)
            s.setValue(kPmsBeaconTextSetting, m_pmsBeaconText->text());
        if (m_pmsBeaconEnable)
            s.setValue(kPmsBeaconEnabledSetting,
                       m_pmsBeaconEnable->isChecked() ? QStringLiteral("True")
                                                      : QStringLiteral("False"));
        s.save();
    }
}

void Ax25HfPacketDecodeDialog::setPmsEnabled(bool enabled, bool persist)
{
    if (persist) {
        AppSettings::instance().setValue(kPmsEnabledSetting,
            enabled ? QStringLiteral("True") : QStringLiteral("False"));
        AppSettings::instance().save();
    }

    if (enabled) {
        // The mailbox needs the modem RX tap running to receive callers.
        if (m_enableDecode && !m_enableDecode->isChecked()) {
            appendSystemLine(QStringLiteral("Enabling the modem for the mailbox (PMS)."));
            m_enableDecode->setChecked(true);
        }
        applyPmsConfigFromUi(false);
        if (!m_pms->hasValidAddress()) {
            appendSystemLine(QStringLiteral(
                "Mailbox: enter a valid listen callsign (e.g. KI6BCJ-10) before enabling the PMS."));
            if (m_pmsEnable) {
                QSignalBlocker blocker(m_pmsEnable);
                m_pmsEnable->setChecked(false);
            }
            refreshPmsStatus();
            return;
        }
        m_pms->setEnabled(true);
        appendSystemLine(QStringLiteral("Mailbox (PMS) listening as %1.")
            .arg(m_pms->localAddress().toString()));
    } else {
        m_pms->setEnabled(false);
        appendSystemLine(QStringLiteral("Mailbox (PMS) disabled."));
    }
    refreshPmsStatus();
}

void Ax25HfPacketDecodeDialog::refreshPmsStatus()
{
    if (!m_pmsStatusValue || !m_pms)
        return;

    const bool enabled = m_pms->isEnabled();
    QString status;
    if (!enabled) {
        status = QStringLiteral("Disabled");
    } else if (m_pms->isCallerConnected()) {
        status = QStringLiteral("Connected: %1").arg(m_pms->connectedCaller());
    } else {
        status = QStringLiteral("Listening as %1").arg(m_pms->localAddress().toString());
    }
    m_pmsStatusValue->setText(status);
    if (m_pmsStatusDot) {
        m_pmsStatusDot->setFixedSize(12, 12);
        m_pmsStatusDot->setStyleSheet(enabled
            ? QStringLiteral("background:#5fce66;border-radius:6px;")
            : QStringLiteral("background:#8190a3;border-radius:6px;"));
    }

    if (m_pmsCallersValue) {
        const QStringList callers = m_pms->lastCallers(5);
        m_pmsCallersValue->setText(callers.isEmpty()
            ? QStringLiteral("(no callers yet)")
            : callers.join(QStringLiteral("\n")));
    }

    if (m_pmsStatsValue) {
        const qint64 freeBytes = m_pms->freeDiskBytes();
        auto humanBytes = [](qint64 bytes) -> QString {
            const char* units[] = {"B", "KB", "MB", "GB", "TB"};
            double value = static_cast<double>(bytes);
            int unit = 0;
            while (value >= 1024.0 && unit < 4) {
                value /= 1024.0;
                ++unit;
            }
            return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
        };
        m_pmsStatsValue->setText(QStringLiteral(
            "%1 message(s)  |  %2 caller(s) logged  |  %3 station(s) heard  |  %4 free")
            .arg(m_pms->messageCount())
            .arg(m_pms->callerCount())
            .arg(m_pms->heardSummary(100000).size())
            .arg(humanBytes(freeBytes)));
    }
}

bool Ax25HfPacketDecodeDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_terminalInput && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Up) {
            if (m_terminalHistoryIndex > 0) {
                --m_terminalHistoryIndex;
                m_terminalInput->setText(m_terminalHistory.value(m_terminalHistoryIndex));
            }
            return true;
        }
        if (key->key() == Qt::Key_Down) {
            if (m_terminalHistoryIndex < m_terminalHistory.size()) {
                ++m_terminalHistoryIndex;
                m_terminalInput->setText(m_terminalHistoryIndex < m_terminalHistory.size()
                    ? m_terminalHistory.value(m_terminalHistoryIndex)
                    : QString());
            }
            return true;
        }
    }
    return PersistentDialog::eventFilter(watched, event);
}

} // namespace AetherSDR
