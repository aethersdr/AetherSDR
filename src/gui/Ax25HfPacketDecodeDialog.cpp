#include "Ax25HfPacketDecodeDialog.h"

#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/DaxTxPolicy.h"
#include "core/LogManager.h"
#include "core/tnc/Ax25FrameFormatter.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSizePolicy>
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

constexpr auto kPacketDecoderProfileSetting = "Ax25PacketDecoderProfile";
constexpr auto kPacketDecoderPolaritySetting = "Ax25PacketDecoderPolarity";
constexpr auto kPacketDecoderDebugSetting = "Ax25PacketDecoderDiagnosticsDebug";
constexpr int kAudioCaptureSeconds = 180;
constexpr int kTxDaxSettleMs = 150;
constexpr int kTxLeadMs = 200;
constexpr int kTxTailMs = 250;
constexpr int kTxChunkMs = 20;

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
    border-radius: 6px;
    border: 1px solid transparent;
    background: transparent;
    min-height: 40px;
    font-size: 16px;
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

QString polaritySettingsValue(Ax25TonePolarity polarity)
{
    return polarity == Ax25TonePolarity::Inverted
        ? QStringLiteral("Reverse")
        : QStringLiteral("Normal");
}

Ax25TonePolarity polarityFromSettingsValue(const QString& value)
{
    if (value.compare(QStringLiteral("Reverse"), Qt::CaseInsensitive) == 0
        || value.compare(QStringLiteral("Inverted"), Qt::CaseInsensitive) == 0) {
        return Ax25TonePolarity::Inverted;
    }
    return Ax25TonePolarity::Normal;
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
    return button;
}

QPushButton* disabledActionButton(const QString& text, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    button->setEnabled(false);
    button->setMinimumHeight(48);
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

class PacketActivityWidget final : public QWidget {
public:
    explicit PacketActivityWidget(QWidget* parent = nullptr)
        : QWidget(parent)
        , m_levels(68)
    {
        setMinimumHeight(38);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        updateToolTip();
        for (int i = 0; i < m_levels.size(); ++i)
            m_levels[i] = 4 + ((i * 7) % 6);
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

    void recordFrame()
    {
        m_cursor = (m_cursor + 9) % m_levels.size();
        m_levels[m_cursor] = 30;
        if (m_cursor + 3 < m_levels.size())
            m_levels[m_cursor + 3] = 18;
        update();
    }

    void tick(int hdlcCandidates, int acceptedFrames, bool receiveGateOpen)
    {
        if (m_levels.isEmpty())
            return;

        m_cursor = (m_cursor + 1) % m_levels.size();
        int level = receiveGateOpen ? 9 : 3;
        if (hdlcCandidates > 0)
            level = std::max(level, 9 + std::min(14, hdlcCandidates * 3));
        if (acceptedFrames > 0)
            level = 30;
        m_levels[m_cursor] = level;
        update();
    }

    void reset()
    {
        for (int i = 0; i < m_levels.size(); ++i)
            m_levels[i] = 3 + ((i * 5) % 5);
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

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(rect(), QColor(0, 0, 0, 0));

        const int count = m_levels.size();
        if (count <= 0)
            return;

        const int gap = 3;
        const int barWidth = qMax(2, (width() - gap * (count - 1)) / count);
        const int usableHeight = qMax(1, height() - 8);
        const int base = height() - 4;
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_debugEnabled ? QColor(210, 164, 72) : QColor(95, 206, 102));

        for (int i = 0; i < count; ++i) {
            const int level = qBound(2, m_levels[i], usableHeight);
            const int x = i * (barWidth + gap);
            painter.drawRect(QRect(x, base - level, barWidth, level));
            if (m_levels[i] > 5)
                --m_levels[i];
        }

        if (m_debugEnabled) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(210, 164, 72), 1));
            painter.drawRect(rect().adjusted(0, 0, -1, -1));
        }
    }

private:
    void updateToolTip()
    {
        setToolTip(m_debugEnabled
            ? QStringLiteral("Packet diagnostics debug is on. Click to turn it off.")
            : QStringLiteral("Packet diagnostics debug is off. Click to turn it on."));
    }

    QVector<int> m_levels;
    int m_cursor{0};
    bool m_debugEnabled{false};
    std::function<void()> m_clickHandler;
};

Ax25HfPacketDecodeDialog::Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                                   RadioModel* radio,
                                                   SliceModel* initialSlice,
                                                   QWidget* parent)
    : PersistentDialog(QStringLiteral("AetherModem - Packet Decoder (Experimental)"),
                       QStringLiteral("Ax25HfPacketDecodeDialogGeometry"),
                       parent)
    , m_audio(audio)
    , m_radio(radio)
{
    setMinimumSize(1080, 680);

    m_shim = new AetherAx25LibmodemShim(this);
    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(1000);
    m_txPaceTimer = new QTimer(this);
    m_txPaceTimer->setInterval(kTxChunkMs);
    bodyWidget()->setStyleSheet(QString::fromLatin1(kAetherModemStyle));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);

    auto* experimentalBanner = new QLabel(
        QStringLiteral("<b>Experimental \xE2\x80\x94 AX.25 modem bring-up.</b> "
                       "300 baud HF RX/TX is active; 1200 baud VHF remains "
                       "receive-focused while timing work continues."),
        bodyWidget());
    experimentalBanner->setObjectName(QStringLiteral("ExperimentalBanner"));
    experimentalBanner->setWordWrap(true);
    experimentalBanner->setTextFormat(Qt::RichText);
    root->addWidget(experimentalBanner);

    auto* tabsFrame = panel(QStringLiteral("TabsFrame"), bodyWidget());
    auto* tabs = new QHBoxLayout(tabsFrame);
    tabs->setContentsMargins(0, 0, 0, 0);
    tabs->setSpacing(0);
    tabs->addWidget(tabButton(QStringLiteral("AX.25"), true, tabsFrame), 1);
    auto* terminalTab = tabButton(QStringLiteral("Terminal"), false, tabsFrame);
    terminalTab->setVisible(false);
    tabs->addWidget(terminalTab, 1);
    auto* mailboxTab = tabButton(QStringLiteral("Mailbox"), false, tabsFrame);
    mailboxTab->setVisible(false);
    tabs->addWidget(mailboxTab, 1);
    root->addWidget(tabsFrame);

    auto* controlsFrame = panel(QStringLiteral("ControlsFrame"), bodyWidget());
    auto* controls = new QHBoxLayout(controlsFrame);
    controls->setContentsMargins(16, 14, 16, 14);
    controls->setSpacing(20);

    auto* baudCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* baudLayout = new QVBoxLayout(baudCell);
    baudLayout->setContentsMargins(0, 0, 20, 0);
    baudLayout->setSpacing(12);
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
    modemLayout->setSpacing(12);
    modemLayout->addWidget(sectionLabel(QStringLiteral("MODEM"), modemCell));
    m_enableDecode = new QCheckBox(QStringLiteral("Enable Modem"), modemCell);
    modemLayout->addWidget(m_enableDecode);
    controls->addWidget(modemCell, 1);

    auto* polarityCell = panel(QStringLiteral("ControlCell"), controlsFrame);
    auto* polarityLayout = new QVBoxLayout(polarityCell);
    polarityLayout->setContentsMargins(0, 0, 20, 0);
    polarityLayout->setSpacing(12);
    polarityLayout->addWidget(sectionLabel(QStringLiteral("TONE POLARITY"), polarityCell));
    auto* polarityButtons = new QHBoxLayout;
    polarityButtons->setSpacing(34);
    m_polarityNormal = new QRadioButton(QStringLiteral("Normal"), polarityCell);
    m_polarityReverse = new QRadioButton(QStringLiteral("Reverse"), polarityCell);
    m_polarityNormal->setChecked(true);
    polarityButtons->addWidget(m_polarityNormal);
    polarityButtons->addWidget(m_polarityReverse);
    polarityButtons->addStretch(1);
    polarityLayout->addLayout(polarityButtons);
    controls->addWidget(polarityCell, 2);

    m_captureButton = new QPushButton(QStringLiteral("Capture 3m"), controlsFrame);
    m_captureButton->setMinimumHeight(42);
    controls->addWidget(m_captureButton);

    m_clearButton = new QPushButton(QStringLiteral("Clear Log"), controlsFrame);
    m_clearButton->setMinimumHeight(42);
    controls->addWidget(m_clearButton);
    root->addWidget(controlsFrame);

    auto* txFrame = panel(QStringLiteral("ControlsFrame"), bodyWidget());
    auto* txLayout = new QHBoxLayout(txFrame);
    txLayout->setContentsMargins(16, 12, 16, 12);
    txLayout->setSpacing(12);
    auto* txLabel = sectionLabel(QStringLiteral("TRANSMIT AX.25 UI FRAME"), txFrame);
    txLayout->addWidget(txLabel);
    m_txText = new QLineEdit(txFrame);
    m_txText->setPlaceholderText(QStringLiteral("hello world  or  N0CALL-1>APRS,WIDE1-1:hello world"));
    txLayout->addWidget(m_txText, 1);
    m_txButton = new QPushButton(QStringLiteral("Transmit"), txFrame);
    m_txButton->setMinimumHeight(42);
    txLayout->addWidget(m_txButton);
    root->addWidget(txFrame);

    auto* logFrame = panel(QStringLiteral("LogFrame"), bodyWidget());
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

    auto* actionRowFrame = new QWidget(bodyWidget());
    auto* actionRow = new QHBoxLayout(actionRowFrame);
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    actionRow->addWidget(disabledActionButton(QStringLiteral("Send SMS"), actionRowFrame), 1);
    actionRow->addWidget(disabledActionButton(QStringLiteral("Send APRS Msg..."), actionRowFrame), 1);
    auto* positionFrame = panel(QStringLiteral("ActionFrame"), actionRowFrame);
    auto* positionLayout = new QHBoxLayout(positionFrame);
    positionLayout->setContentsMargins(18, 8, 18, 8);
    positionLayout->setSpacing(12);
    auto* positionButton = new QPushButton(QStringLiteral("Send APRS Position"), positionFrame);
    positionButton->setEnabled(false);
    positionButton->setFlat(true);
    positionLayout->addWidget(positionButton, 1);
    auto* intervalLabel = new QLabel(QStringLiteral("Interval:"), positionFrame);
    intervalLabel->setObjectName(QStringLiteral("StatusValue"));
    positionLayout->addWidget(intervalLabel);
    auto* interval = new QComboBox(positionFrame);
    interval->addItems({QStringLiteral("5 min"), QStringLiteral("10 min"), QStringLiteral("30 min")});
    interval->setEnabled(false);
    positionLayout->addWidget(interval);
    actionRow->addWidget(positionFrame, 2);
    actionRow->addWidget(disabledActionButton(QStringLiteral("Connect BBS"), actionRowFrame), 1);
    root->addWidget(actionRowFrame);
    actionRowFrame->setVisible(false);

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    statusRow->addWidget(statusPanel(QStringLiteral("MODEM STATUS"),
                                     &m_modemStatusDot,
                                     &m_modemStatusValue,
                                     bodyWidget()), 1);
    statusRow->addWidget(statusPanel(QStringLiteral("GAIN STAGE"),
                                     &m_gainStageDot,
                                     &m_gainStageValue,
                                     bodyWidget()), 1);

    auto* activityFrame = panel(QStringLiteral("StatusFrame"), bodyWidget());
    auto* activityLayout = new QHBoxLayout(activityFrame);
    activityLayout->setContentsMargins(16, 12, 16, 12);
    activityLayout->setSpacing(18);
    m_packetActivityTitle = sectionLabel(QStringLiteral("PACKET ACTIVITY"), activityFrame);
    activityLayout->addWidget(m_packetActivityTitle);
    m_packetActivity = new PacketActivityWidget(activityFrame);
    m_packetActivity->setClickHandler([this] {
        setDiagnosticsDebugEnabled(!m_diagnosticsDebugEnabled, true);
    });
    activityLayout->addWidget(m_packetActivity, 1);
    statusRow->addWidget(activityFrame, 2);
    root->addLayout(statusRow);

    const Ax25ModemProfile savedProfile = profileFromSettingsValue(
        AppSettings::instance().value(kPacketDecoderProfileSetting, QStringLiteral("Hf300")).toString());
    const Ax25TonePolarity savedPolarity = polarityFromSettingsValue(
        AppSettings::instance().value(kPacketDecoderPolaritySetting, QStringLiteral("Normal")).toString());
    const bool savedDebug = AppSettings::instance().value(kPacketDecoderDebugSetting, false).toBool();
    m_hf300Profile->setChecked(savedProfile == Ax25ModemProfile::Hf300);
    m_vhf1200Profile->setChecked(savedProfile == Ax25ModemProfile::Vhf1200);
    m_polarityNormal->setChecked(savedPolarity == Ax25TonePolarity::Normal);
    m_polarityReverse->setChecked(savedPolarity == Ax25TonePolarity::Inverted);
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
    connect(m_polarityNormal, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked)
            return;
        setTonePolarity(Ax25TonePolarity::Normal, true);
    });
    connect(m_polarityReverse, &QRadioButton::toggled, this, [this](bool checked) {
        if (!checked)
            return;
        setTonePolarity(Ax25TonePolarity::Inverted, true);
    });
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded,
            this, &Ax25HfPacketDecodeDialog::appendFrame);
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

    appendSystemLine(QStringLiteral("AetherModem initialized."));
    appendSystemLine(QStringLiteral("Enable Modem to start the RX audio tap."));
    appendSystemLine(QStringLiteral("TX accepts raw payload text or full SRC>DST,path:payload syntax."));
    setAttachedSlice(initialSlice);
    refreshStatus();
    refreshTransmitControls();
}

Ax25HfPacketDecodeDialog::~Ax25HfPacketDecodeDialog()
{
    if (m_txActive || m_txPendingStream)
        finishTransmit(true, QStringLiteral("AetherModem window closing"));
    if (m_captureActive)
        finishAudioCapture(false);
    if (m_audio)
        m_audio->setTncRxTapEnabled(false);
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

Ax25TonePolarity Ax25HfPacketDecodeDialog::selectedTonePolarity() const
{
    return m_polarityReverse && m_polarityReverse->isChecked()
        ? Ax25TonePolarity::Inverted
        : Ax25TonePolarity::Normal;
}

void Ax25HfPacketDecodeDialog::setModemProfile(Ax25ModemProfile profile, bool persist)
{
    const auto polarity = selectedTonePolarity();
    m_shim->configure(ax25DemodConfigForProfile(profile, polarity));
    m_lastDiagnostics = {};
    m_lastDiagnosticsUtc = {};

    if (persist) {
        AppSettings::instance().setValue(kPacketDecoderProfileSetting, profileSettingsValue(profile));
        AppSettings::instance().save();
    }

    if (m_log)
        appendSystemLine(QStringLiteral("Configured %1.").arg(m_shim->demodDescription()));
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setTonePolarity(Ax25TonePolarity polarity, bool persist)
{
    auto cfg = m_shim->config();
    if (cfg.polarity == polarity && persist)
        return;

    cfg.polarity = polarity;
    m_shim->configure(cfg);
    m_lastDiagnostics = {};
    m_lastDiagnosticsUtc = {};

    if (persist) {
        AppSettings::instance().setValue(kPacketDecoderPolaritySetting, polaritySettingsValue(polarity));
        AppSettings::instance().save();
    }

    appendSystemLine(QStringLiteral("Tone polarity changed to %1. Configured %2.")
        .arg(polaritySettingsValue(polarity), m_shim->demodDescription()));
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setDecodeEnabled(bool enabled)
{
    if (enabled) {
        m_shim->reset();
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
        m_heartbeatTimer->start();
    } else {
        if (m_captureActive)
            finishAudioCapture(false);
        if (m_audio)
            m_audio->setTncRxTapEnabled(false);
        m_shim->reset();
        m_lastDiagnostics = {};
        m_lastDiagnosticsUtc = {};
        m_lastActivityHdlc = 0;
        m_lastActivityAccepted = 0;
        m_heartbeatTimer->stop();
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

    m_shim->feedAudio(monoFloat32Pcm, sampleRate);
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
    m_shim->reset();
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
    if (m_txActive || m_txPendingStream) {
        appendSystemLine(QStringLiteral("TX already in progress."));
        return;
    }
    if (!m_txText)
        return;
    if (!m_hf300Profile || !m_hf300Profile->isChecked()) {
        appendSystemLine(QStringLiteral("TX is enabled for the 300 baud HF profile in this pass."));
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

    const QString text = m_txText->text();
    Ax25TransmitResult tx = m_shim->buildTransmitAudio(text, defaultTransmitSource());
    if (!tx.ok) {
        appendSystemLine(QStringLiteral("TX packetization failed: %1.").arg(tx.error));
        qCWarning(lcAx25).noquote() << "AX.25 TX packetization failed:" << tx.error;
        return;
    }

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
        "TX packetized: %1 > %2%3, %4 payload bytes, %5 frame bytes, %6 bits, %7 s, RMS %8 dBFS, peak %9 dBFS.")
        .arg(tx.frame.source,
             tx.frame.destination,
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
            .arg(kTxTailMs);

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

    const qsizetype bytesPerChunk = static_cast<qsizetype>(m_pendingTx.sampleRate)
        * kTxChunkMs / 1000
        * 2
        * static_cast<qsizetype>(sizeof(float));
    if (bytesPerChunk <= 0) {
        finishTransmit(true, QStringLiteral("invalid TX pacing chunk size"));
        return;
    }

    if (m_txOffsetBytes >= m_txPcm.size()) {
        if (m_txPaceTimer)
            m_txPaceTimer->stop();
        appendSystemLine(QStringLiteral("AX.25 TX audio queued; waiting %1 ms before unkey.")
            .arg(kTxTailMs));
        QTimer::singleShot(kTxTailMs, this, [this] {
            finishTransmit(false, QStringLiteral("AX.25 TX complete"));
        });
        return;
    }

    const qsizetype sendBytes = std::min<qsizetype>(bytesPerChunk, m_txPcm.size() - m_txOffsetBytes);
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
    refreshTransmitControls();
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
            .arg(m_shim->demodDescription())
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

    const bool hfTx = m_hf300Profile && m_hf300Profile->isChecked();
    const bool hasText = m_txText && !m_txText->text().trimmed().isEmpty();
    const bool ready = hfTx && hasText && !m_txActive && !m_txPendingStream;
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
        m_txText->setToolTip(hfTx
            ? QStringLiteral("Transmit a 300 baud HF AX.25 UI frame. Raw text uses %1>APRS; full SRC>DST,path:payload syntax is also accepted.")
                .arg(defaultTransmitSource())
            : QStringLiteral("TX is enabled for 300 baud HF in this pass."));
    }
}

void Ax25HfPacketDecodeDialog::setDiagnosticsDebugEnabled(bool enabled, bool persist)
{
    if (m_diagnosticsDebugEnabled == enabled && persist)
        return;

    m_diagnosticsDebugEnabled = enabled;
    if (m_shim)
        m_shim->setDiagnosticsLoggingEnabled(enabled);
    if (m_packetActivity)
        m_packetActivity->setDebugEnabled(enabled);
    if (m_packetActivityTitle) {
        m_packetActivityTitle->setText(enabled
            ? QStringLiteral("PACKET ACTIVITY DEBUG")
            : QStringLiteral("PACKET ACTIVITY"));
    }

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

} // namespace AetherSDR
