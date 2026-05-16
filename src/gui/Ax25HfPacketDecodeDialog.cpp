#include "Ax25HfPacketDecodeDialog.h"

#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/tnc/Ax25FrameFormatter.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

constexpr auto kPacketDecoderProfileSetting = "Ax25PacketDecoderProfile";

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

QString titleForProfile(Ax25ModemProfile profile)
{
    switch (profile) {
    case Ax25ModemProfile::Hf300:
        return QStringLiteral("Experimental RX-only 300 bps HF AX.25");
    case Ax25ModemProfile::Vhf1200:
        return QStringLiteral("Experimental RX-only 1200 bps VHF AX.25");
    }
    return QStringLiteral("Experimental RX-only AX.25");
}

} // namespace

Ax25HfPacketDecodeDialog::Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                                   int initialSliceId,
                                                   QWidget* parent)
    : PersistentDialog(QStringLiteral("Packet Decoder"),
                       QStringLiteral("Ax25HfPacketDecodeDialogGeometry"),
                       parent)
    , m_audio(audio)
{
    setMinimumSize(720, 420);
    m_attachedSliceId = initialSliceId;

    m_shim = new AetherAx25LibmodemShim(this);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    m_titleLabel = new QLabel(bodyWidget());
    m_titleLabel->setStyleSheet(QStringLiteral("font-weight: 700;"));
    root->addWidget(m_titleLabel);

    auto* controls = new QHBoxLayout;
    controls->addWidget(new QLabel(QStringLiteral("Decoder:"), bodyWidget()));
    m_hf300Profile = new QRadioButton(QStringLiteral("300 baud HF"), bodyWidget());
    m_vhf1200Profile = new QRadioButton(QStringLiteral("1200 baud VHF"), bodyWidget());
    controls->addWidget(m_hf300Profile);
    controls->addWidget(m_vhf1200Profile);
    controls->addSpacing(12);

    m_enableDecode = new QCheckBox(QStringLiteral("Enable decode"), bodyWidget());
    controls->addWidget(m_enableDecode);

    controls->addWidget(new QLabel(QStringLiteral("Tone polarity:"), bodyWidget()));
    m_polarity = new QComboBox(bodyWidget());
    m_polarity->addItem(QStringLiteral("Normal"), static_cast<int>(Ax25TonePolarity::Normal));
    m_polarity->addItem(QStringLiteral("Inverted"), static_cast<int>(Ax25TonePolarity::Inverted));
    controls->addWidget(m_polarity);
    controls->addStretch(1);

    m_clearButton = new QPushButton(QStringLiteral("Clear Log"), bodyWidget());
    controls->addWidget(m_clearButton);
    root->addLayout(controls);

    auto* statusGrid = new QGridLayout;
    statusGrid->setHorizontalSpacing(12);
    statusGrid->setVerticalSpacing(4);
    m_sliceLabel = new QLabel(bodyWidget());
    m_sampleRateLabel = new QLabel(bodyWidget());
    m_frameCountLabel = new QLabel(bodyWidget());
    m_lastDecodeLabel = new QLabel(bodyWidget());
    statusGrid->addWidget(new QLabel(QStringLiteral("Attached:"), bodyWidget()), 0, 0);
    statusGrid->addWidget(m_sliceLabel, 0, 1);
    statusGrid->addWidget(new QLabel(QStringLiteral("Demod:"), bodyWidget()), 0, 2);
    statusGrid->addWidget(m_sampleRateLabel, 0, 3);
    statusGrid->addWidget(new QLabel(QStringLiteral("Frames:"), bodyWidget()), 1, 0);
    statusGrid->addWidget(m_frameCountLabel, 1, 1);
    statusGrid->addWidget(new QLabel(QStringLiteral("Last decode:"), bodyWidget()), 1, 2);
    statusGrid->addWidget(m_lastDecodeLabel, 1, 3);
    statusGrid->setColumnStretch(3, 1);
    root->addLayout(statusGrid);

    m_log = new QPlainTextEdit(bodyWidget());
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setPlaceholderText(QStringLiteral("Decoded AX.25 UI frames will appear here."));
    root->addWidget(m_log, 1);

    const Ax25ModemProfile savedProfile = profileFromSettingsValue(
        AppSettings::instance().value(kPacketDecoderProfileSetting, QStringLiteral("Hf300")).toString());
    m_hf300Profile->setChecked(savedProfile == Ax25ModemProfile::Hf300);
    m_vhf1200Profile->setChecked(savedProfile == Ax25ModemProfile::Vhf1200);
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
        refreshStatus();
    });
    connect(m_polarity, &QComboBox::currentIndexChanged, this, [this] {
        auto cfg = m_shim->config();
        cfg.polarity = static_cast<Ax25TonePolarity>(m_polarity->currentData().toInt());
        m_shim->configure(cfg);
        refreshStatus();
    });
    connect(m_shim, &AetherAx25LibmodemShim::frameDecoded,
            this, &Ax25HfPacketDecodeDialog::appendFrame);
    connect(m_shim, &AetherAx25LibmodemShim::statusChanged,
            this, &Ax25HfPacketDecodeDialog::refreshStatus);

    if (m_audio) {
        connect(m_audio, &AudioEngine::tncRxAudioReady,
                m_shim, &AetherAx25LibmodemShim::feedAudio,
                Qt::QueuedConnection);
    }

    refreshStatus();
}

Ax25HfPacketDecodeDialog::~Ax25HfPacketDecodeDialog()
{
    if (m_audio)
        m_audio->setTncRxTapEnabled(false);
}

void Ax25HfPacketDecodeDialog::setAttachedSlice(int sliceId)
{
    m_attachedSliceId = sliceId;
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setModemProfile(Ax25ModemProfile profile, bool persist)
{
    const auto polarity = m_shim->config().polarity;
    m_shim->configure(ax25DemodConfigForProfile(profile, polarity));
    m_titleLabel->setText(titleForProfile(profile));

    if (persist) {
        AppSettings::instance().setValue(kPacketDecoderProfileSetting, profileSettingsValue(profile));
        AppSettings::instance().save();
    }

    refreshStatus();
}

void Ax25HfPacketDecodeDialog::setDecodeEnabled(bool enabled)
{
    if (m_audio)
        m_audio->setTncRxTapEnabled(enabled);
    if (!enabled)
        m_shim->reset();
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::appendFrame(const Ax25DecodedFrame& frame)
{
    if (!frame.fcsOk)
        return;
    ++m_frameCount;
    m_lastDecodeUtc = frame.timestampUtc;
    m_log->appendPlainText(Ax25FrameFormatter::formatLogLine(frame));
    refreshStatus();
}

void Ax25HfPacketDecodeDialog::refreshStatus()
{
    if (m_attachedSliceId >= 0) {
        m_sliceLabel->setText(QStringLiteral("slice %1 via PC RX audio stream").arg(m_attachedSliceId));
    } else {
        m_sliceLabel->setText(QStringLiteral("no slice attached"));
    }

    m_sampleRateLabel->setText(m_shim->demodDescription());
    m_frameCountLabel->setText(QString::number(m_frameCount));
    m_lastDecodeLabel->setText(
        m_lastDecodeUtc.isValid()
            ? m_lastDecodeUtc.toUTC().toString(Qt::ISODate)
            : QStringLiteral("-"));
}

} // namespace AetherSDR
