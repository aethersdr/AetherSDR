#include "DStarModemPage.h"

#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyle>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr const char* kDStarPageStyle = R"(
QWidget#DStarModemPage {
    background: #07101c;
}
QFrame#DStarHeaderFrame,
QFrame#DStarConfigFrame,
QFrame#DStarTrafficFrame,
QFrame#DStarStatusFrame {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
        stop:0 #111d2c, stop:1 #0a1421);
    border: 1px solid #233246;
    border-radius: 7px;
}
QLabel#DStarPageTitle,
QLabel#DStarPanelTitle {
    color: #d4deea;
    background: transparent;
    font-size: 16px;
    font-weight: 700;
}
QLabel#DStarModeLabel,
QLabel#DStarMuted,
QLabel#DStarTrafficMeta {
    color: #8d99ad;
    background: transparent;
}
QLabel#DStarFieldLabel,
QLabel#DStarFooterLabel {
    color: #8d99ad;
    background: transparent;
    font-size: 11px;
    font-weight: 700;
}
QLabel#DStarServiceDot {
    background: #647187;
    border-radius: 6px;
    min-width: 12px;
    max-width: 12px;
    min-height: 12px;
    max-height: 12px;
}
QLabel#DStarError {
    color: #d2ad74;
    background: #211a10;
    border: 1px solid #4b3920;
    border-radius: 5px;
    padding: 7px 9px;
}
QLabel#DStarRouteSummary,
QLabel#DStarActiveMessage,
QLabel#DStarTrafficMessage {
    color: #c4cedd;
    background: transparent;
    font-family: "SF Mono", "Menlo", "Consolas", monospace;
    font-size: 13px;
}
QToolButton#DStarAdvancedButton {
    color: #aeb9cc;
    background: transparent;
    border: 1px solid transparent;
    padding: 6px 2px;
    text-align: left;
}
QToolButton#DStarAdvancedButton:hover {
    color: #d6dfeb;
}
QListWidget#DStarTrafficList {
    background: #050b13;
    border: 1px solid #1c2a3b;
    border-radius: 5px;
    outline: none;
    padding: 8px;
}
QListWidget#DStarTrafficList::item {
    border: none;
    padding: 0px;
}
QListWidget#DStarTrafficList::item:selected {
    background: #0d1c20;
}
QFrame#DStarTrafficRx {
    background: #0a1724;
    border: 1px solid #31536b;
    border-radius: 6px;
}
QFrame#DStarTrafficTx {
    background: #0c1a20;
    border: 1px solid #315b3b;
    border-radius: 6px;
}
QLabel#DStarDateDivider {
    color: #8d99ad;
    background: transparent;
    font-size: 12px;
}
QFrame#DStarSeparator {
    background: #233246;
    border: none;
    min-height: 1px;
    max-height: 1px;
}
QSplitter::handle {
    background: transparent;
    width: 10px;
}
)";

QLabel* panelTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("DStarPanelTitle"));
    return label;
}

QLabel* fieldLabel(const QString& text, QWidget* buddy, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("DStarFieldLabel"));
    label->setBuddy(buddy);
    return label;
}

QFrame* separator(QWidget* parent)
{
    auto* frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("DStarSeparator"));
    frame->setFrameShape(QFrame::NoFrame);
    return frame;
}

QString displayFrequency(double frequencyMhz)
{
    const qint64 frequencyHz = qRound64(frequencyMhz * 1.0e6);
    const qint64 mhz = frequencyHz / 1000000;
    const qint64 khz = (frequencyHz / 1000) % 1000;
    const qint64 hz = frequencyHz % 1000;
    return QStringLiteral("%1.%2.%3")
        .arg(mhz)
        .arg(khz, 3, 10, QLatin1Char('0'))
        .arg(hz, 3, 10, QLatin1Char('0'));
}

QString routeDescription(const DStarTrafficEntry& entry,
                         const QString& sliceLetter)
{
    return QObject::tr("URCALL %1 | RPT1 %2 | RPT2 %3 | Slice %4")
        .arg(entry.urCall.isEmpty() ? QObject::tr("unknown") : entry.urCall,
             entry.rpt1.isEmpty() ? QObject::tr("unknown") : entry.rpt1,
             entry.rpt2.isEmpty() ? QObject::tr("unknown") : entry.rpt2)
        .arg(sliceLetter);
}

QString trafficPeer(const DStarTrafficEntry& entry)
{
    if (entry.direction == DStarTrafficDirection::Transmit) {
        if (entry.urCall.compare(QStringLiteral("CQCQCQ"),
                                 Qt::CaseInsensitive) == 0) {
            return QObject::tr("CQ");
        }
        if (!entry.urCall.isEmpty()) {
            return entry.urCall;
        }
        return QObject::tr("Unknown destination");
    }
    return entry.myCall.isEmpty()
        ? QObject::tr("Unknown station") : entry.myCall;
}

} // namespace

DStarModemPage::DStarModemPage(RadioModel* radio, QWidget* parent)
    : QWidget(parent)
    , m_radio(radio)
{
    setObjectName(QStringLiteral("DStarModemPage"));
    setAccessibleName(tr("D-STAR modem"));
    setStyleSheet(QString::fromLatin1(kDStarPageStyle));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    buildHeader();

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("DStarWorkspaceSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildConfigurationPanel());
    splitter->addWidget(buildTrafficPanel());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({390, 690});
    root->addWidget(splitter, 1);

    buildFooter();

    if (m_radio) {
        DStarModel& model = m_radio->dstarModel();
        connect(&model, &DStarModel::configurationChanged,
                this, &DStarModemPage::refreshConfiguration);
        connect(&model, &DStarModel::serialDevicesChanged,
                this, &DStarModemPage::refreshSerialDevices);
        connect(&model, &DStarModel::trafficChanged,
                this, &DStarModemPage::refreshTraffic);
        connect(&model, &DStarModel::serviceChanged,
                this, &DStarModemPage::refreshService);
        connect(m_radio, &RadioModel::connectionStateChanged,
                this, &DStarModemPage::refreshService);
        connect(m_radio, &RadioModel::sliceAdded,
                this, [this](SliceModel*) { refreshService(); });
        connect(m_radio, &RadioModel::sliceRemoved,
                this, [this](int) { refreshService(); });
    }

    refreshAll();
}

void DStarModemPage::buildHeader()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("DStarHeaderFrame"));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(16, 10, 16, 10);
    layout->setSpacing(12);

    auto* title = new QLabel(tr("D-STAR"), frame);
    title->setObjectName(QStringLiteral("DStarPageTitle"));
    layout->addWidget(title);

    auto* mode = new QLabel(tr("DV Voice"), frame);
    mode->setObjectName(QStringLiteral("DStarModeLabel"));
    mode->setAccessibleName(tr("D-STAR mode DV Voice"));
    layout->addWidget(mode);
    layout->addStretch(1);

    m_serviceDot = new QLabel(frame);
    m_serviceDot->setObjectName(QStringLiteral("DStarServiceDot"));
    m_serviceDot->setAccessibleName(tr("D-STAR service status"));
    layout->addWidget(m_serviceDot);

    m_serviceState = new QLabel(tr("Stopped"), frame);
    m_serviceState->setObjectName(QStringLiteral("StatusValue"));
    m_serviceState->setMinimumWidth(78);
    layout->addWidget(m_serviceState);

    m_sliceState = new QLabel(tr("No DSTR slice"), frame);
    m_sliceState->setObjectName(QStringLiteral("DStarMuted"));
    m_sliceState->setMinimumWidth(150);
    m_sliceState->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(m_sliceState);

    m_startStopButton = new QPushButton(tr("Start"), frame);
    m_startStopButton->setObjectName(QStringLiteral("dstarServiceStartStop"));
    m_startStopButton->setAccessibleName(tr("Start D-STAR service"));
    m_startStopButton->setFixedWidth(92);
    m_startStopButton->setMinimumHeight(40);
    connect(m_startStopButton, &QPushButton::clicked,
            this, &DStarModemPage::startStopService);
    layout->addWidget(m_startStopButton);

    qobject_cast<QVBoxLayout*>(this->layout())->addWidget(frame);
}

QWidget* DStarModemPage::buildConfigurationPanel()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("DStarConfigFrame"));
    frame->setMinimumWidth(340);

    auto* frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(16, 14, 16, 14);
    frameLayout->setSpacing(10);
    frameLayout->addWidget(panelTitle(tr("Station & Route"), frame));

    m_errorLabel = new QLabel(frame);
    m_errorLabel->setObjectName(QStringLiteral("DStarError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    m_errorLabel->setAccessibleName(tr("D-STAR configuration error"));
    frameLayout->addWidget(m_errorLabel);

    auto* scroll = new QScrollArea(frame);
    scroll->setObjectName(QStringLiteral("DStarConfigurationScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* body = new QWidget(scroll);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 4, 0);
    bodyLayout->setSpacing(10);

    auto* identity = new QGridLayout;
    identity->setHorizontalSpacing(12);
    identity->setVerticalSpacing(5);
    m_myCallEdit = new QLineEdit(body);
    m_myCallEdit->setObjectName(QStringLiteral("dstarMyCall"));
    m_myCallEdit->setAccessibleName(tr("D-STAR MYCALL"));
    m_myCallEdit->setMaxLength(8);
    m_myCallEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[A-Za-z0-9]{0,8}$")),
        m_myCallEdit));
    m_suffixEdit = new QLineEdit(body);
    m_suffixEdit->setObjectName(QStringLiteral("dstarMyCallSuffix"));
    m_suffixEdit->setAccessibleName(tr("D-STAR MYCALL suffix"));
    m_suffixEdit->setMaxLength(4);
    m_suffixEdit->setMaximumWidth(100);
    m_suffixEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[A-Za-z0-9]{0,4}$")),
        m_suffixEdit));
    identity->addWidget(fieldLabel(tr("MYCALL"), m_myCallEdit, body), 0, 0);
    identity->addWidget(fieldLabel(tr("Suffix"), m_suffixEdit, body), 0, 1);
    identity->addWidget(m_myCallEdit, 1, 0);
    identity->addWidget(m_suffixEdit, 1, 1);
    identity->setColumnStretch(0, 1);
    bodyLayout->addLayout(identity);

    auto* route = new QGridLayout;
    route->setHorizontalSpacing(10);
    route->setVerticalSpacing(5);
    m_fromCombo = new QComboBox(body);
    m_fromCombo->setObjectName(QStringLiteral("dstarFrom"));
    m_fromCombo->setAccessibleName(tr("D-STAR route from"));
    m_fromCombo->addItem(tr("Direct"),
                         static_cast<int>(DStarRouteOrigin::Direct));
    m_fromCombo->addItem(tr("Repeater"),
                         static_cast<int>(DStarRouteOrigin::Repeater));
    m_toCombo = new QComboBox(body);
    m_toCombo->setObjectName(QStringLiteral("dstarTo"));
    m_toCombo->setAccessibleName(tr("D-STAR route destination type"));
    m_toCombo->addItem(tr("Local CQ"),
                       static_cast<int>(DStarRouteDestination::LocalCq));
    m_toCombo->addItem(tr("Specific station"),
                       static_cast<int>(DStarRouteDestination::Station));
    m_toCombo->addItem(tr("Repeater area"),
                       static_cast<int>(DStarRouteDestination::RepeaterArea));
    m_toCombo->addItem(tr("Custom"),
                       static_cast<int>(DStarRouteDestination::Custom));
    route->addWidget(fieldLabel(tr("From"), m_fromCombo, body), 0, 0);
    route->addWidget(fieldLabel(tr("To"), m_toCombo, body), 0, 1);
    route->addWidget(m_fromCombo, 1, 0);
    route->addWidget(m_toCombo, 1, 1);
    route->setColumnStretch(0, 1);
    route->setColumnStretch(1, 1);
    bodyLayout->addLayout(route);

    m_accessRepeaterLabel = fieldLabel(tr("Access repeater"), nullptr, body);
    bodyLayout->addWidget(m_accessRepeaterLabel);
    m_accessRepeaterRow = new QWidget(body);
    auto* accessRepeaterLayout = new QHBoxLayout(m_accessRepeaterRow);
    accessRepeaterLayout->setContentsMargins(0, 0, 0, 0);
    accessRepeaterLayout->setSpacing(6);
    m_accessRepeaterEdit = new QLineEdit(m_accessRepeaterRow);
    m_accessRepeaterEdit->setObjectName(QStringLiteral("dstarAccessRepeater"));
    m_accessRepeaterEdit->setAccessibleName(tr("D-STAR access repeater callsign"));
    m_accessRepeaterEdit->setPlaceholderText(tr("Repeater callsign"));
    m_accessRepeaterEdit->setMaxLength(7);
    m_accessRepeaterEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[A-Za-z0-9]{0,7}$")),
        m_accessRepeaterEdit));
    accessRepeaterLayout->addWidget(m_accessRepeaterEdit, 1);
    m_accessRepeaterModule = new QComboBox(m_accessRepeaterRow);
    m_accessRepeaterModule->setObjectName(QStringLiteral("dstarAccessRepeaterModule"));
    m_accessRepeaterModule->setAccessibleName(tr("D-STAR access repeater module"));
    for (const QChar module : QStringLiteral("ABCD")) {
        m_accessRepeaterModule->addItem(tr("Module %1").arg(module), module);
    }
    m_accessRepeaterModule->setMinimumWidth(100);
    accessRepeaterLayout->addWidget(m_accessRepeaterModule);
    bodyLayout->addWidget(m_accessRepeaterRow);

    m_destinationLabel = fieldLabel(tr("Destination station"), nullptr, body);
    bodyLayout->addWidget(m_destinationLabel);
    m_destinationRow = new QWidget(body);
    auto* destinationLayout = new QHBoxLayout(m_destinationRow);
    destinationLayout->setContentsMargins(0, 0, 0, 0);
    destinationLayout->setSpacing(6);
    m_destinationEdit = new QLineEdit(m_destinationRow);
    m_destinationEdit->setObjectName(QStringLiteral("dstarDestination"));
    m_destinationEdit->setAccessibleName(tr("D-STAR destination callsign"));
    m_destinationEdit->setMaxLength(8);
    m_destinationEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[A-Za-z0-9 ]{0,8}$")),
        m_destinationEdit));
    destinationLayout->addWidget(m_destinationEdit, 1);
    m_destinationModule = new QComboBox(m_destinationRow);
    m_destinationModule->setObjectName(QStringLiteral("dstarDestinationRepeaterModule"));
    m_destinationModule->setAccessibleName(tr("D-STAR destination repeater module"));
    for (const QChar module : QStringLiteral("ABCD")) {
        m_destinationModule->addItem(tr("Module %1").arg(module), module);
    }
    m_destinationModule->setMinimumWidth(100);
    destinationLayout->addWidget(m_destinationModule);
    bodyLayout->addWidget(m_destinationRow);

    m_routeSummary = new QLabel(body);
    m_routeSummary->setObjectName(QStringLiteral("DStarRouteSummary"));
    m_routeSummary->setWordWrap(true);
    m_routeSummary->setAccessibleName(tr("D-STAR resolved route"));
    bodyLayout->addWidget(m_routeSummary);

    m_advancedButton = new QToolButton(body);
    m_advancedButton->setObjectName(QStringLiteral("DStarAdvancedButton"));
    m_advancedButton->setText(tr("Advanced route details"));
    m_advancedButton->setAccessibleName(tr("Show advanced D-STAR route details"));
    m_advancedButton->setCheckable(true);
    m_advancedButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advancedButton->setArrowType(Qt::RightArrow);
    bodyLayout->addWidget(m_advancedButton);

    m_advancedPanel = new QWidget(body);
    auto* advanced = new QGridLayout(m_advancedPanel);
    advanced->setContentsMargins(0, 0, 0, 0);
    advanced->setHorizontalSpacing(8);
    advanced->setVerticalSpacing(6);
    m_urCallEdit = new QLineEdit(m_advancedPanel);
    m_urCallEdit->setObjectName(QStringLiteral("dstarUrCall"));
    m_urCallEdit->setAccessibleName(tr("D-STAR URCALL"));
    m_urCallEdit->setMaxLength(8);
    m_rpt1Edit = new QLineEdit(m_advancedPanel);
    m_rpt1Edit->setObjectName(QStringLiteral("dstarRpt1"));
    m_rpt1Edit->setAccessibleName(tr("D-STAR RPT1"));
    m_rpt1Edit->setMaxLength(8);
    m_rpt2Edit = new QLineEdit(m_advancedPanel);
    m_rpt2Edit->setObjectName(QStringLiteral("dstarRpt2"));
    m_rpt2Edit->setAccessibleName(tr("D-STAR RPT2"));
    m_rpt2Edit->setMaxLength(8);
    for (QLineEdit* edit : {m_urCallEdit, m_rpt1Edit, m_rpt2Edit}) {
        edit->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("^/?[A-Za-z0-9 ]{0,8}$")), edit));
    }
    advanced->addWidget(fieldLabel(tr("URCALL"), m_urCallEdit, m_advancedPanel), 0, 0);
    advanced->addWidget(m_urCallEdit, 0, 1);
    advanced->addWidget(fieldLabel(tr("RPT1"), m_rpt1Edit, m_advancedPanel), 1, 0);
    advanced->addWidget(m_rpt1Edit, 1, 1);
    advanced->addWidget(fieldLabel(tr("RPT2"), m_rpt2Edit, m_advancedPanel), 2, 0);
    advanced->addWidget(m_rpt2Edit, 2, 1);

    m_executableEdit = new QLineEdit(m_advancedPanel);
    m_executableEdit->setObjectName(QStringLiteral("digitalVoiceWaveformExecutable"));
    m_executableEdit->setAccessibleName(tr("Digital voice service executable"));
    m_executableEdit->setPlaceholderText(tr("Bundled aether-dv-waveform"));
    m_browseButton = new QPushButton(tr("Browse..."), m_advancedPanel);
    m_browseButton->setObjectName(QStringLiteral("dstarExecutableBrowse"));
    m_browseButton->setAccessibleName(tr("Browse for digital voice service executable"));
    advanced->addWidget(fieldLabel(tr("Executable"), m_executableEdit, m_advancedPanel), 3, 0);
    auto* executableRow = new QHBoxLayout;
    executableRow->setContentsMargins(0, 0, 0, 0);
    executableRow->setSpacing(6);
    executableRow->addWidget(m_executableEdit, 1);
    executableRow->addWidget(m_browseButton);
    advanced->addLayout(executableRow, 3, 1);
    advanced->setColumnStretch(1, 1);
    m_advancedPanel->setVisible(false);
    bodyLayout->addWidget(m_advancedPanel);

    bodyLayout->addWidget(separator(body));
    bodyLayout->addWidget(panelTitle(tr("Voice Service"), body));

    auto* vocoderRow = new QHBoxLayout;
    vocoderRow->addWidget(fieldLabel(tr("Vocoder"), nullptr, body));
    auto* vocoder = new QLabel(body);
    vocoder->setObjectName(QStringLiteral("DStarMuted"));
    vocoder->setText(m_radio ? m_radio->dstarModel().vocoderLabel()
                             : tr("ThumbDV / DV3000"));
    vocoder->setAccessibleName(tr("D-STAR vocoder %1").arg(vocoder->text()));
    vocoderRow->addWidget(vocoder, 1);
    bodyLayout->addLayout(vocoderRow);

    bodyLayout->addWidget(fieldLabel(tr("ThumbDV device"), nullptr, body));
    auto* deviceRow = new QHBoxLayout;
    deviceRow->setSpacing(6);
    m_deviceCombo = new QComboBox(body);
    m_deviceCombo->setObjectName(QStringLiteral("dstarThumbDvSerialPort"));
    m_deviceCombo->setAccessibleName(tr("ThumbDV device"));
    m_deviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceCombo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    deviceRow->addWidget(m_deviceCombo, 1);
    m_refreshDevicesButton = new QPushButton(tr("Refresh"), body);
    m_refreshDevicesButton->setObjectName(QStringLiteral("dstarRefreshDevices"));
    m_refreshDevicesButton->setAccessibleName(tr("Refresh ThumbDV devices"));
    deviceRow->addWidget(m_refreshDevicesButton);
    bodyLayout->addLayout(deviceRow);

    m_autoStartCheck = new QCheckBox(tr("Autostart at launch"), body);
    m_autoStartCheck->setObjectName(QStringLiteral("digitalVoiceWaveformAutoStart"));
    m_autoStartCheck->setAccessibleName(tr("Autostart D-STAR service at launch"));
    bodyLayout->addWidget(m_autoStartCheck);
    bodyLayout->addStretch(1);

    scroll->setWidget(body);
    frameLayout->addWidget(scroll, 1);

    connect(m_advancedButton, &QToolButton::toggled, this, [this](bool checked) {
        m_advancedButton->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        m_advancedPanel->setVisible(checked);
    });
    connect(m_myCallEdit, &QLineEdit::editingFinished,
            this, [this] { saveConfiguration(false); });
    connect(m_suffixEdit, &QLineEdit::editingFinished,
            this, [this] { saveConfiguration(false); });
    connect(m_fromCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_updating) {
            return;
        }
        refreshDestinationControl();
        saveConfiguration(false);
    });
    connect(m_toCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (m_updating) {
            return;
        }
        refreshDestinationControl();
        if (m_toCombo->itemData(index).toInt()
            == static_cast<int>(DStarRouteDestination::Custom)) {
            m_advancedButton->setChecked(true);
        }
        saveConfiguration(false);
    });
    connect(m_destinationEdit, &QLineEdit::editingFinished,
            this, [this] { saveConfiguration(false); });
    connect(m_accessRepeaterEdit, &QLineEdit::editingFinished,
            this, [this] { saveConfiguration(false); });
    connect(m_accessRepeaterModule,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (!m_updating) {
            saveConfiguration(false);
        }
    });
    connect(m_destinationModule,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (!m_updating) {
            saveConfiguration(false);
        }
    });
    for (QLineEdit* edit : {m_urCallEdit, m_rpt1Edit, m_rpt2Edit,
                            m_executableEdit}) {
        connect(edit, &QLineEdit::editingFinished,
                this, [this] { saveConfiguration(true); });
    }
    connect(m_browseButton, &QPushButton::clicked,
            this, &DStarModemPage::browseExecutable);
    connect(m_deviceCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (!m_updating) {
            saveConfiguration(false);
        }
    });
    connect(m_refreshDevicesButton, &QPushButton::clicked, this, [this] {
        if (m_radio) {
            m_radio->dstarModel().refreshSerialDevices();
        }
    });
    connect(m_autoStartCheck, &QCheckBox::toggled, this, [this](bool) {
        if (!m_updating) {
            saveConfiguration(false);
        }
    });

    return frame;
}

QWidget* DStarModemPage::buildTrafficPanel()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("DStarTrafficFrame"));
    frame->setMinimumWidth(460);
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* header = new QHBoxLayout;
    header->addWidget(panelTitle(tr("D-STAR Traffic"), frame));
    header->addStretch(1);
    m_stationFilter = new QComboBox(frame);
    m_stationFilter->setObjectName(QStringLiteral("dstarTrafficStationFilter"));
    m_stationFilter->setAccessibleName(
        tr("Filter D-STAR traffic by station or destination"));
    m_stationFilter->setMinimumWidth(130);
    header->addWidget(m_stationFilter);
    m_searchEdit = new QLineEdit(frame);
    m_searchEdit->setObjectName(QStringLiteral("dstarTrafficSearch"));
    m_searchEdit->setAccessibleName(tr("Search D-STAR traffic"));
    m_searchEdit->setPlaceholderText(tr("Search traffic"));
    m_searchEdit->setMaximumWidth(180);
    header->addWidget(m_searchEdit);
    m_clearTrafficButton = new QToolButton(frame);
    m_clearTrafficButton->setObjectName(QStringLiteral("dstarClearTraffic"));
    m_clearTrafficButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_clearTrafficButton->setToolTip(tr("Clear D-STAR traffic history"));
    m_clearTrafficButton->setAccessibleName(tr("Clear D-STAR traffic history"));
    m_clearTrafficButton->setMinimumSize(38, 38);
    header->addWidget(m_clearTrafficButton);
    layout->addLayout(header);

    m_trafficList = new QListWidget(frame);
    m_trafficList->setObjectName(QStringLiteral("DStarTrafficList"));
    m_trafficList->setAccessibleName(tr("D-STAR traffic history"));
    m_trafficList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_trafficList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_trafficList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_trafficList->setSpacing(5);
    layout->addWidget(m_trafficList, 1);

    layout->addWidget(separator(frame));
    auto* messageHeader = new QHBoxLayout;
    messageHeader->addWidget(fieldLabel(tr("TX MESSAGE"), nullptr, frame));
    messageHeader->addStretch(1);
    m_messageCounter = new QLabel(tr("0 / 20"), frame);
    m_messageCounter->setObjectName(QStringLiteral("DStarMuted"));
    m_messageCounter->setAccessibleName(tr("D-STAR message character count"));
    messageHeader->addWidget(m_messageCounter);
    layout->addLayout(messageHeader);

    auto* composer = new QHBoxLayout;
    composer->setSpacing(8);
    m_messageEdit = new QLineEdit(frame);
    m_messageEdit->setObjectName(QStringLiteral("dstarMessageDraft"));
    m_messageEdit->setAccessibleName(tr("D-STAR TX message"));
    m_messageEdit->setMaxLength(20);
    m_messageEdit->setPlaceholderText(tr("20-character DV message"));
    composer->addWidget(m_messageEdit, 1);
    m_setMessageButton = new QPushButton(tr("Set TX Message"), frame);
    m_setMessageButton->setObjectName(QStringLiteral("dstarSetTxMessage"));
    m_setMessageButton->setAccessibleName(tr("Set D-STAR message for the next transmission"));
    m_setMessageButton->setMinimumHeight(42);
    m_setMessageButton->setFixedWidth(150);
    composer->addWidget(m_setMessageButton);
    layout->addLayout(composer);

    auto* activeRow = new QHBoxLayout;
    activeRow->addWidget(fieldLabel(tr("ACTIVE"), nullptr, frame));
    m_activeMessage = new QLabel(tr("None"), frame);
    m_activeMessage->setObjectName(QStringLiteral("DStarActiveMessage"));
    m_activeMessage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_activeMessage->setAccessibleName(tr("Active D-STAR TX message"));
    activeRow->addWidget(m_activeMessage, 1);
    layout->addLayout(activeRow);

    connect(m_messageEdit, &QLineEdit::textChanged,
            this, &DStarModemPage::refreshMessageCounter);
    connect(m_messageEdit, &QLineEdit::returnPressed,
            this, &DStarModemPage::setTxMessage);
    connect(m_setMessageButton, &QPushButton::clicked,
            this, &DStarModemPage::setTxMessage);
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &DStarModemPage::refreshTraffic);
    connect(m_stationFilter, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) { refreshTraffic(); });
    connect(m_clearTrafficButton, &QToolButton::clicked, this, [this] {
        if (m_radio) {
            m_radio->dstarModel().clearTraffic();
        }
    });

    return frame;
}

void DStarModemPage::buildFooter()
{
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("DStarStatusFrame"));
    auto* layout = new QHBoxLayout(frame);
    layout->setContentsMargins(14, 7, 14, 7);
    layout->setSpacing(10);

    auto* dot = new QLabel(frame);
    dot->setObjectName(QStringLiteral("DStarServiceDot"));
    layout->addWidget(dot);
    layout->addWidget(fieldLabel(tr("MODEM"), nullptr, frame));
    m_footerState = new QLabel(tr("STOPPED"), frame);
    m_footerState->setObjectName(QStringLiteral("StatusValue"));
    layout->addWidget(m_footerState);

    auto* separatorOne = new QLabel(QStringLiteral("|"), frame);
    separatorOne->setObjectName(QStringLiteral("DStarMuted"));
    layout->addWidget(separatorOne);
    layout->addWidget(fieldLabel(tr("ROUTE"), nullptr, frame));
    m_footerRoute = new QLabel(tr("DIRECT"), frame);
    m_footerRoute->setObjectName(QStringLiteral("StatusValue"));
    layout->addWidget(m_footerRoute);

    auto* separatorTwo = new QLabel(QStringLiteral("|"), frame);
    separatorTwo->setObjectName(QStringLiteral("DStarMuted"));
    layout->addWidget(separatorTwo);
    layout->addWidget(fieldLabel(tr("ACTIVITY"), nullptr, frame));
    m_footerActivity = new QLabel(tr("Waiting"), frame);
    m_footerActivity->setObjectName(QStringLiteral("DStarMuted"));
    m_footerActivity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_footerActivity, 1);

    qobject_cast<QVBoxLayout*>(this->layout())->addWidget(frame);
}

void DStarModemPage::refreshAll()
{
    refreshSerialDevices();
    refreshConfiguration();
    refreshService();
    refreshTraffic();
    refreshMessageCounter();
}

void DStarModemPage::refreshService()
{
    if (!m_radio) {
        return;
    }
    DStarModel& model = m_radio->dstarModel();
    const bool active = model.serviceActive();
    const bool stopping = model.serviceStopping();
    const QString state = model.serviceStateName();
    const QString detail = !model.serviceHealthDetail().isEmpty()
        ? model.serviceHealthDetail()
        : model.serviceStatusText();

    m_serviceState->setText(state);
    m_serviceState->setToolTip(detail);
    m_serviceDot->setToolTip(detail.isEmpty() ? state : detail);
    if (state == QLatin1String("Running")) {
        m_serviceDot->setStyleSheet(QStringLiteral("background:#64d36e;"));
    } else if (state == QLatin1String("Failed")) {
        m_serviceDot->setStyleSheet(QStringLiteral("background:#d2ad74;"));
    } else {
        m_serviceDot->setStyleSheet(QStringLiteral("background:#647187;"));
    }

    m_startStopButton->setText(active ? tr("Stop") : tr("Start"));
    m_startStopButton->setAccessibleName(
        active ? tr("Stop D-STAR service") : tr("Start D-STAR service"));
    m_startStopButton->setEnabled(!stopping
        && (active || (m_radio->isConnected() && model.helperAvailable())));

    const bool lockServiceFields = active || stopping;
    m_deviceCombo->setEnabled(!lockServiceFields && m_deviceCombo->count() > 0
                              && !m_deviceCombo->currentData().toString().isEmpty());
    m_refreshDevicesButton->setEnabled(!lockServiceFields);
    m_executableEdit->setReadOnly(lockServiceFields);
    m_browseButton->setEnabled(!lockServiceFields);

    const int sliceId = model.activeSliceId();
    SliceModel* slice = sliceId >= 0 ? m_radio->slice(sliceId) : nullptr;
    if (slice) {
        m_sliceState->setText(tr("Slice %1  %2 MHz")
            .arg(slice->letter())
            .arg(displayFrequency(slice->frequency())));
    } else {
        m_sliceState->setText(tr("No DSTR slice"));
    }
    m_footerState->setText(state.toUpper());
}

void DStarModemPage::refreshConfiguration()
{
    if (!m_radio) {
        return;
    }
    m_updating = true;
    const DStarConfiguration config =
        m_radio->dstarModel().configuration(m_radio->callsign());
    m_myCallEdit->setText(config.myCall);
    m_suffixEdit->setText(config.myCallSuffix);
    m_urCallEdit->setText(config.urCall);
    m_rpt1Edit->setText(config.rpt1);
    m_rpt2Edit->setText(config.rpt2);
    m_executableEdit->setText(config.executablePath);
    m_autoStartCheck->setChecked(config.autoStart);

    const DStarRouteRequest route =
        DStarModel::routeRequestForConfiguration(config);
    m_fromCombo->setCurrentIndex(m_fromCombo->findData(
        static_cast<int>(route.origin)));
    m_toCombo->setCurrentIndex(m_toCombo->findData(
        static_cast<int>(route.destination)));
    m_accessRepeaterEdit->setText(route.accessRepeaterCallsign);
    const int accessModule = m_accessRepeaterModule->findData(
        route.accessRepeaterModule);
    m_accessRepeaterModule->setCurrentIndex(accessModule >= 0 ? accessModule : 0);
    m_destinationEdit->setText(route.destinationCallsign);
    const int destinationModule = m_destinationModule->findData(
        route.destinationRepeaterModule);
    m_destinationModule->setCurrentIndex(
        destinationModule >= 0 ? destinationModule : 0);
    if (route.destination == DStarRouteDestination::Custom) {
        m_advancedButton->setChecked(true);
    }
    if (!m_messageEdit->isModified()) {
        m_messageEdit->setText(config.message);
        m_messageEdit->setModified(false);
    }
    m_activeMessage->setText(config.message.isEmpty() ? tr("None") : config.message);
    m_updating = false;

    refreshDestinationControl();
    refreshSerialDevices();
    refreshRouteSummary();
    refreshMessageCounter();
    setError({});
}

void DStarModemPage::refreshSerialDevices()
{
    if (!m_radio || !m_deviceCombo) {
        return;
    }
    const QString configured =
        m_radio->dstarModel().configuration(m_radio->callsign()).serialPort;
    const QList<DStarSerialDevice> devices = m_radio->dstarModel().serialDevices();

    const bool wasUpdating = m_updating;
    m_updating = true;
    const QSignalBlocker blocker(m_deviceCombo);
    m_deviceCombo->clear();
    int selected = -1;
    if (configured.isEmpty()) {
        m_deviceCombo->addItem(tr("Select a ThumbDV device"), QString());
        m_deviceCombo->setItemData(
            0, tr("Select a device to verify it before starting D-STAR."),
            Qt::ToolTipRole);
        selected = 0;
    }
    for (const DStarSerialDevice& device : devices) {
        QString label = device.label;
        if (device.verification == DStarSerialDevice::Verification::Probing) {
            label += tr(" (checking)");
        } else if (device.verification == DStarSerialDevice::Verification::Unavailable) {
            label += device.present ? tr(" (not verified)") : tr(" (not connected)");
        }
        m_deviceCombo->addItem(label, device.path);
        const int index = m_deviceCombo->count() - 1;
        m_deviceCombo->setItemData(
            index,
            device.detail.isEmpty()
                ? device.path
                : QStringLiteral("%1\n%2").arg(device.path, device.detail),
            Qt::ToolTipRole);
        if (DStarModel::serialPathsEquivalent(device.path, configured)) {
            selected = index;
        }
    }
    if (!configured.isEmpty() && selected < 0) {
        m_deviceCombo->insertItem(0, tr("%1 (not connected)").arg(configured), configured);
        m_deviceCombo->setItemData(0, configured, Qt::ToolTipRole);
        selected = 0;
    }
    if (m_deviceCombo->count() == 0) {
        m_deviceCombo->addItem(tr("No ThumbDV candidates found"), QString());
        selected = 0;
    }
    m_deviceCombo->setCurrentIndex(selected);
    m_updating = wasUpdating;
    refreshService();
}

void DStarModemPage::refreshDestinationControl()
{
    const DStarRouteOrigin origin = static_cast<DStarRouteOrigin>(
        m_fromCombo->currentData().toInt());
    DStarRouteDestination destination = static_cast<DStarRouteDestination>(
        m_toCombo->currentData().toInt());
    const bool fromRepeater = origin == DStarRouteOrigin::Repeater;

    if (auto* model = qobject_cast<QStandardItemModel*>(m_toCombo->model())) {
        const int repeaterAreaIndex = m_toCombo->findData(
            static_cast<int>(DStarRouteDestination::RepeaterArea));
        if (QStandardItem* item = model->item(repeaterAreaIndex)) {
            item->setEnabled(fromRepeater);
        }
    }
    if (!fromRepeater
        && destination == DStarRouteDestination::RepeaterArea) {
        const QSignalBlocker blocker(m_toCombo);
        m_toCombo->setCurrentIndex(m_toCombo->findData(
            static_cast<int>(DStarRouteDestination::LocalCq)));
        destination = DStarRouteDestination::LocalCq;
    }

    m_accessRepeaterLabel->setVisible(fromRepeater);
    m_accessRepeaterRow->setVisible(fromRepeater);

    const bool station = destination == DStarRouteDestination::Station;
    const bool repeaterArea =
        destination == DStarRouteDestination::RepeaterArea;
    const bool needsDestination = station || repeaterArea;
    m_destinationLabel->setVisible(needsDestination);
    m_destinationRow->setVisible(needsDestination);
    m_destinationEdit->setEnabled(needsDestination);
    m_destinationEdit->setMaxLength(repeaterArea ? 6 : 8);
    m_destinationLabel->setText(
        repeaterArea ? tr("Destination repeater") : tr("Destination station"));
    m_destinationEdit->setPlaceholderText(
        repeaterArea ? tr("Repeater callsign") : tr("Station callsign"));
    m_destinationModule->setVisible(repeaterArea);
}

void DStarModemPage::refreshRouteSummary()
{
    DStarConfiguration config;
    QString error;
    if (!configurationFromWidgets(false, &config, &error)) {
        m_routeSummary->setText(error);
        m_routeSummary->setToolTip({});
        m_footerRoute->setText(tr("INCOMPLETE"));
        return;
    }

    const DStarRouteRequest request = routeRequestFromWidgets();
    QString summary;
    switch (request.destination) {
    case DStarRouteDestination::LocalCq:
        summary = request.origin == DStarRouteOrigin::Direct
            ? tr("Direct to local CQ")
            : tr("Local CQ via %1 %2")
                .arg(request.accessRepeaterCallsign,
                     QString(request.accessRepeaterModule));
        break;
    case DStarRouteDestination::Station:
        summary = request.origin == DStarRouteOrigin::Direct
            ? tr("Direct to %1").arg(request.destinationCallsign)
            : tr("%1 via %2 %3 gateway")
                .arg(request.destinationCallsign,
                     request.accessRepeaterCallsign,
                     QString(request.accessRepeaterModule));
        break;
    case DStarRouteDestination::RepeaterArea:
        summary = tr("%1 %2 via %3 %4 gateway")
            .arg(request.destinationCallsign,
                 QString(request.destinationRepeaterModule),
                 request.accessRepeaterCallsign,
                 QString(request.accessRepeaterModule));
        break;
    case DStarRouteDestination::Custom:
        summary = tr("Custom D-STAR route");
        break;
    }
    m_routeSummary->setText(summary);
    m_routeSummary->setToolTip(
        tr("URCALL %1 | RPT1 %2 | RPT2 %3")
            .arg(config.urCall, config.rpt1, config.rpt2));
    m_routeSummary->setAccessibleDescription(m_routeSummary->toolTip());
    m_footerRoute->setText(
        request.origin == DStarRouteOrigin::Direct
            ? tr("DIRECT")
            : tr("%1 %2").arg(request.accessRepeaterCallsign,
                               QString(request.accessRepeaterModule)));
}

void DStarModemPage::refreshMessageCounter()
{
    if (!m_messageEdit || !m_messageCounter) {
        return;
    }
    m_messageCounter->setText(
        tr("%1 / 20").arg(m_messageEdit->text().size()));
}

DStarRouteRequest DStarModemPage::routeRequestFromWidgets() const
{
    DStarRouteRequest request;
    request.origin = static_cast<DStarRouteOrigin>(
        m_fromCombo->currentData().toInt());
    request.destination = static_cast<DStarRouteDestination>(
        m_toCombo->currentData().toInt());
    request.accessRepeaterCallsign = m_accessRepeaterEdit->text();
    request.accessRepeaterModule = m_accessRepeaterModule->currentData().toChar();
    request.destinationCallsign = m_destinationEdit->text();
    request.destinationRepeaterModule =
        m_destinationModule->currentData().toChar();
    request.customUrCall = m_urCallEdit->text();
    request.customRpt1 = m_rpt1Edit->text();
    request.customRpt2 = m_rpt2Edit->text();
    return request;
}

bool DStarModemPage::configurationFromWidgets(bool useRawRoute,
                                               DStarConfiguration* config,
                                               QString* error) const
{
    if (!config) {
        return false;
    }
    *config = m_radio
        ? m_radio->dstarModel().configuration(m_radio->callsign())
        : DStarConfiguration{};
    config->autoStart = m_autoStartCheck->isChecked();
    config->executablePath = m_executableEdit->text();
    config->serialPort = m_deviceCombo->currentData().toString();
    config->myCall = m_myCallEdit->text();
    config->myCallSuffix = m_suffixEdit->text();

    if (useRawRoute) {
        config->urCall = m_urCallEdit->text();
        config->rpt1 = m_rpt1Edit->text();
        config->rpt2 = m_rpt2Edit->text();
        return true;
    }

    DStarResolvedRoute route;
    if (!DStarModel::resolveRoute(routeRequestFromWidgets(), &route, error)) {
        return false;
    }
    config->urCall = route.urCall;
    config->rpt1 = route.rpt1;
    config->rpt2 = route.rpt2;
    return true;
}

bool DStarModemPage::saveConfiguration(bool useRawRoute)
{
    if (m_updating || !m_radio) {
        return false;
    }
    QString error;
    DStarConfiguration config;
    if (!configurationFromWidgets(useRawRoute, &config, &error)) {
        setError(error);
        refreshRouteSummary();
        return false;
    }
    if (!m_radio->dstarModel().setConfiguration(
            config, m_radio->callsign(), &error)) {
        setError(error);
        refreshRouteSummary();
        return false;
    }
    setError({});
    refreshRouteSummary();
    return true;
}

void DStarModemPage::setTxMessage()
{
    if (!m_radio) {
        return;
    }
    DStarConfiguration config =
        m_radio->dstarModel().configuration(m_radio->callsign());
    config.message = m_messageEdit->text();
    QString error;
    if (!m_radio->dstarModel().setConfiguration(
            config, m_radio->callsign(), &error)) {
        setError(error);
        return;
    }
    setError({});
    m_messageEdit->setModified(false);
    m_activeMessage->setText(
        config.message.isEmpty() ? tr("None") : config.message);
}

void DStarModemPage::startStopService()
{
    if (!m_radio) {
        return;
    }
    DStarModel& model = m_radio->dstarModel();
    if (model.serviceActive()) {
        model.stop();
        return;
    }
    if (!saveConfiguration(false)) {
        return;
    }
    const DStarConfiguration config = model.configuration(m_radio->callsign());
    if (config.serialPort.isEmpty()) {
        setError(tr("Connect a ThumbDV and select it before starting D-STAR."));
        return;
    }
    if (!model.start(m_radio->radioAddress(), m_radio->callsign())) {
        setError(model.serviceLastError());
    }
    refreshService();
}

void DStarModemPage::browseExecutable()
{
    const QString current = m_executableEdit->text().trimmed();
    QString initial = QCoreApplication::applicationDirPath();
    if (!current.isEmpty()) {
        const QFileInfo info(current);
        initial = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Digital Voice Service Executable"), initial);
    if (path.isEmpty()) {
        return;
    }
    m_executableEdit->setText(path);
    m_executableEdit->setCursorPosition(path.size());
    saveConfiguration(true);
}

void DStarModemPage::refreshTraffic()
{
    if (!m_radio || !m_trafficList) {
        return;
    }
    const QList<DStarTrafficEntry>& traffic = m_radio->dstarModel().traffic();
    const QString previousStation = m_stationFilter->currentData().toString();
    QSet<QString> stations;
    for (const DStarTrafficEntry& entry : traffic) {
        const QString peer = trafficPeer(entry);
        if (!peer.isEmpty()) {
            stations.insert(peer);
        }
    }
    QStringList sortedStations = stations.values();
    std::sort(sortedStations.begin(), sortedStations.end());
    {
        const QSignalBlocker blocker(m_stationFilter);
        m_stationFilter->clear();
        m_stationFilter->addItem(tr("All traffic"), QString());
        for (const QString& station : sortedStations) {
            m_stationFilter->addItem(station, station);
        }
        const int previousIndex = m_stationFilter->findData(previousStation);
        m_stationFilter->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    }

    const QString station = m_stationFilter->currentData().toString();
    const QString search = m_searchEdit->text().trimmed();
    const bool wasAtBottom = m_trafficList->verticalScrollBar()->value()
        >= m_trafficList->verticalScrollBar()->maximum() - 8;
    m_trafficList->clear();

    QDate lastDate;
    for (const DStarTrafficEntry& entry : traffic) {
        const QString peer = trafficPeer(entry);
        if (!station.isEmpty()
            && peer.compare(station, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString haystack = QStringLiteral("%1 %2 %3 %4 %5")
            .arg(entry.myCall, entry.message, entry.urCall, entry.rpt1, entry.rpt2);
        if (!search.isEmpty()
            && !haystack.contains(search, Qt::CaseInsensitive)) {
            continue;
        }

        const QDate localDate = entry.timestampUtc.toLocalTime().date();
        if (localDate != lastDate) {
            lastDate = localDate;
            auto* dividerItem = new QListWidgetItem(m_trafficList);
            dividerItem->setFlags(Qt::NoItemFlags);
            auto* divider = new QLabel(
                localDate == QDate::currentDate()
                    ? tr("Today")
                    : QLocale().toString(localDate, QLocale::ShortFormat),
                m_trafficList);
            divider->setObjectName(QStringLiteral("DStarDateDivider"));
            divider->setAlignment(Qt::AlignCenter);
            dividerItem->setSizeHint(QSize(100, 28));
            m_trafficList->setItemWidget(dividerItem, divider);
        }

        const bool outgoing = entry.direction == DStarTrafficDirection::Transmit;
        auto* item = new QListWidgetItem(m_trafficList);
        SliceModel* trafficSlice = m_radio->slice(entry.sliceId);
        const QString sliceLetter = trafficSlice
            ? trafficSlice->letter()
            : entry.sliceId >= 0 && entry.sliceId < 26
                ? QString(QChar('A' + entry.sliceId))
                : tr("unknown");
        const QString description = routeDescription(entry, sliceLetter);
        const QString time = entry.timestampUtc.toLocalTime().toString(
            QStringLiteral("h:mm:ss AP"));
        const QString bodyText = entry.message.isEmpty()
            ? tr("Voice transmission")
            : entry.message;
        item->setText(outgoing
            ? tr("Transmitted to %1 at %2: %3").arg(peer, time, bodyText)
            : tr("Received from %1 at %2: %3").arg(peer, time, bodyText));
        item->setToolTip(description);

        auto* row = new QWidget(m_trafficList);
        row->setAccessibleName(item->text());
        row->setAccessibleDescription(description);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 2, 4, 2);
        rowLayout->setSpacing(8);
        if (outgoing) {
            rowLayout->addStretch(1);
        }

        auto* card = new QFrame(row);
        card->setObjectName(outgoing
            ? QStringLiteral("DStarTrafficTx")
            : QStringLiteral("DStarTrafficRx"));
        card->setMinimumWidth(260);
        card->setMaximumWidth(470);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 8, 12, 8);
        cardLayout->setSpacing(4);
        auto* meta = new QLabel(
            QStringLiteral("%1  %2  %3").arg(
                outgoing ? tr("To %1").arg(peer) : peer,
                QString(QChar(0x00b7)), time), card);
        meta->setObjectName(QStringLiteral("DStarTrafficMeta"));
        meta->setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
        cardLayout->addWidget(meta);
        auto* message = new QLabel(bodyText, card);
        message->setObjectName(entry.message.isEmpty()
            ? QStringLiteral("DStarMuted")
            : QStringLiteral("DStarTrafficMessage"));
        message->setWordWrap(true);
        message->setTextInteractionFlags(Qt::TextSelectableByMouse);
        message->setAlignment(outgoing ? Qt::AlignRight : Qt::AlignLeft);
        cardLayout->addWidget(message);
        if (outgoing) {
            auto* status = new QLabel(tr("Transmitted"), card);
            status->setObjectName(QStringLiteral("DStarMuted"));
            status->setAlignment(Qt::AlignRight);
            cardLayout->addWidget(status);
        }
        rowLayout->addWidget(card);
        if (!outgoing) {
            rowLayout->addStretch(1);
        }
        item->setSizeHint(QSize(100, row->sizeHint().height() + 4));
        m_trafficList->setItemWidget(item, row);
    }

    if (m_trafficList->count() == 0) {
        auto* empty = new QListWidgetItem(tr("No D-STAR traffic yet"), m_trafficList);
        empty->setFlags(Qt::NoItemFlags);
        empty->setTextAlignment(Qt::AlignCenter);
        empty->setForeground(QColor(QStringLiteral("#8d99ad")));
    } else if (wasAtBottom) {
        m_trafficList->scrollToBottom();
    }

    if (traffic.isEmpty()) {
        m_footerActivity->setText(tr("Waiting"));
    } else {
        const DStarTrafficEntry& latest = traffic.constLast();
        const QString direction = latest.direction == DStarTrafficDirection::Transmit
            ? tr("TX") : tr("RX");
        const QString summary = latest.message.isEmpty()
            ? tr("voice") : latest.message;
        m_footerActivity->setText(
            tr("%1 %2  %3").arg(direction,
                trafficPeer(latest),
                summary));
    }
}

void DStarModemPage::setError(const QString& message)
{
    m_errorLabel->setText(message);
    m_errorLabel->setVisible(!message.isEmpty());
}

} // namespace AetherSDR
