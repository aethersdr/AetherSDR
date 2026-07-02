#include "WaveformsDialog.h"
#include "core/DStarWaveformProcess.h"
#include "core/DStarWaveformSettings.h"
#include "core/ThemeManager.h"
#include "core/WaveformInstaller.h"
#include "models/FlexWaveformModel.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef HAVE_SERIALPORT
#include <QSerialPortInfo>
#endif

#include <algorithm>

namespace AetherSDR {

namespace {

struct SerialPortOption {
    QString path;
    QString label;
    int score{0};
};

void applyComboHeight(QComboBox* combo)
{
    const int minHeight = qMax(combo->sizeHint().height(),
                               combo->fontMetrics().height() + 12);
    combo->setMinimumHeight(minHeight);
}

void addSerialOption(QList<SerialPortOption>& options,
                     QSet<QString>& seen,
                     const QString& path,
                     const QString& label,
                     int score)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty() || seen.contains(trimmedPath)) {
        return;
    }
    seen.insert(trimmedPath);
    options.push_back({trimmedPath, label.trimmed().isEmpty() ? trimmedPath : label, score});
}

int pathScore(const QString& path)
{
    const QString lower = path.toLower();
    int score = 0;
    if (lower.contains(QStringLiteral("thumbdv"))
            || lower.contains(QStringLiteral("dv3000"))) {
        score += 100;
    }
    if (lower.contains(QStringLiteral("usbserial"))
            || lower.contains(QStringLiteral("usb-serial"))
            || lower.contains(QStringLiteral("ftdi"))) {
        score += 40;
    }
    if (lower.contains(QStringLiteral("usbmodem"))
            || lower.contains(QStringLiteral("ttyusb"))
            || lower.contains(QStringLiteral("ttyacm"))
            || lower.contains(QStringLiteral("wchusbserial"))
            || lower.contains(QStringLiteral("slab_usbtouart"))) {
        score += 20;
    }
    return score;
}

QList<SerialPortOption> detectedSerialPorts()
{
    QList<SerialPortOption> options;
    QSet<QString> seen;

#ifdef HAVE_SERIALPORT
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        QString path = info.systemLocation();
        if (path.isEmpty()) {
            path = info.portName();
        }

        QStringList details;
        if (!info.description().trimmed().isEmpty()) {
            details << info.description().trimmed();
        }
        if (!info.manufacturer().trimmed().isEmpty()
                && !details.contains(info.manufacturer().trimmed())) {
            details << info.manufacturer().trimmed();
        }

        int score = pathScore(path);
        const QString detailText = details.join(QStringLiteral(", "));
        if (detailText.contains(QStringLiteral("thumbdv"), Qt::CaseInsensitive)
                || detailText.contains(QStringLiteral("dv3000"), Qt::CaseInsensitive)) {
            score += 100;
        }
        if (detailText.contains(QStringLiteral("ftdi"), Qt::CaseInsensitive)) {
            score += 40;
        }
        if (info.hasVendorIdentifier() && info.vendorIdentifier() == 0x0403) {
            score += 40;
        }

        const QString label = path;
        addSerialOption(options, seen, path, label, score);
    }
#endif

#if defined(Q_OS_MAC)
    const QStringList patterns {
        QStringLiteral("cu.usbserial*"),
        QStringLiteral("cu.usbmodem*"),
        QStringLiteral("cu.SLAB_USBtoUART*"),
        QStringLiteral("cu.wchusbserial*")
    };
    QDir dev(QStringLiteral("/dev"));
    for (const QString& pattern : patterns) {
        for (const QFileInfo& info : dev.entryInfoList({pattern}, QDir::System | QDir::Files)) {
            addSerialOption(options, seen, info.absoluteFilePath(), info.absoluteFilePath(),
                            pathScore(info.absoluteFilePath()));
        }
    }
#elif defined(Q_OS_LINUX)
    QDir byId(QStringLiteral("/dev/serial/by-id"));
    for (const QFileInfo& info : byId.entryInfoList(QDir::System | QDir::Files | QDir::NoDotAndDotDot)) {
        addSerialOption(options, seen, info.absoluteFilePath(), info.absoluteFilePath(),
                        pathScore(info.absoluteFilePath()) + 30);
    }
    QDir dev(QStringLiteral("/dev"));
    for (const QString& pattern : {QStringLiteral("ttyUSB*"), QStringLiteral("ttyACM*")}) {
        for (const QFileInfo& info : dev.entryInfoList({pattern}, QDir::System | QDir::Files)) {
            addSerialOption(options, seen, info.absoluteFilePath(), info.absoluteFilePath(),
                            pathScore(info.absoluteFilePath()));
        }
    }
#endif

    std::sort(options.begin(), options.end(), [](const SerialPortOption& lhs,
                                                 const SerialPortOption& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.path < rhs.path;
    });
    return options;
}

void showDockerWfpNotReadyDialog(QWidget* parent, const QString& statusText)
{
    PersistentDialog dialog(QObject::tr("Radio WFP Not Ready"), QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/wfpNotReady"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(420);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(
        QObject::tr("AetherSDR can upload a Docker waveform image only after the radio "
                    "reports its Waveform Processor is ON and READY.\n\n"
                    "Current WFP status: %1.\n\n"
                    "Use Install > Legacy Waveform... for .ssdr_waveform packages.")
            .arg(statusText));
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Docker waveform install status"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* okButton = new QPushButton(QObject::tr("OK"));
    okButton->setAccessibleName(QObject::tr("Close Docker waveform status"));
    okButton->setDefault(true);
    QObject::connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    dialog.exec();
}

bool confirmRadioWaveformRemoval(QWidget* parent, const QString& name, bool isContainer)
{
    const QString title = isContainer
        ? QObject::tr("Remove Docker Container")
        : QObject::tr("Uninstall Radio Waveform");
    const QString question = isContainer
        ? QObject::tr("Remove the Docker container \"%1\" from the radio?").arg(name)
        : QObject::tr("Uninstall the waveform \"%1\" from the radio?").arg(name);
    const QString confirmText = isContainer
        ? QObject::tr("Remove")
        : QObject::tr("Uninstall");

    PersistentDialog dialog(title, QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/removeConfirm"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(380);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(question);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Radio waveform removal confirmation"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();

    auto* cancelButton = new QPushButton(QObject::tr("Cancel"));
    cancelButton->setAccessibleName(QObject::tr("Cancel radio waveform removal"));
    cancelButton->setDefault(true);
    QObject::connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttons->addWidget(cancelButton);

    auto* confirmButton = new QPushButton(confirmText);
    confirmButton->setAccessibleName(confirmText);
    QObject::connect(confirmButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(confirmButton);

    root->addLayout(buttons);

    return dialog.exec() == QDialog::Accepted;
}

void showWaveformInstallResultDialog(QWidget* parent,
                                     const QString& title,
                                     const QString& messageText)
{
    PersistentDialog dialog(title, QString(), parent);
    theme::setContainer(&dialog, QStringLiteral("dialog/waveforms/installResult"));
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setModal(true);
    dialog.setMinimumWidth(420);

    auto* root = new QVBoxLayout(dialog.bodyWidget());
    root->setSpacing(10);

    auto* message = new QLabel(messageText);
    message->setTextFormat(Qt::PlainText);
    message->setWordWrap(true);
    message->setAccessibleName(QObject::tr("Waveform install result"));
    root->addWidget(message);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    auto* okButton = new QPushButton(QObject::tr("OK"));
    okButton->setAccessibleName(QObject::tr("Close waveform install result"));
    okButton->setDefault(true);
    QObject::connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    dialog.exec();
}

} // namespace

WaveformsDialog::WaveformsDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(tr("Waveforms"), QStringLiteral("WaveformsDialogGeometry"), parent)
    , m_radioModel(model)
{
    theme::setContainer(this, QStringLiteral("dialog/waveforms"));
    setMinimumSize(740, 420);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(10);
    root->setContentsMargins(12, 10, 12, 12);

    // ── WFP status bar ────────────────────────────────────────────────────────
    auto* statusFrame = new QFrame;
    statusFrame->setFrameShape(QFrame::StyledPanel);
    auto* statusRow = new QHBoxLayout(statusFrame);
    statusRow->setContentsMargins(10, 6, 10, 6);

    m_statusLabel = new QLabel;
    m_statusLabel->setTextFormat(Qt::RichText);
    statusRow->addWidget(m_statusLabel, 1);

    root->addWidget(statusFrame);

    auto* contentRow = new QHBoxLayout;
    contentRow->setSpacing(10);
    root->addLayout(contentRow, 1);

    // ── Local D-STAR waveform process ────────────────────────────────────────
    auto* dstarFrame = new QFrame;
    dstarFrame->setObjectName(QStringLiteral("localDStarPanel"));
    dstarFrame->setFrameShape(QFrame::StyledPanel);
    dstarFrame->setMinimumWidth(320);
    auto* dstarRoot = new QVBoxLayout(dstarFrame);
    dstarRoot->setContentsMargins(10, 8, 10, 10);
    dstarRoot->setSpacing(8);

    auto* dstarHeader = new QHBoxLayout;
    auto* dstarTitle = new QLabel(tr("<b>Local D-STAR</b>"));
    dstarTitle->setTextFormat(Qt::RichText);
    dstarHeader->addWidget(dstarTitle);
    dstarHeader->addStretch();

    m_dstarStartStopBtn = new QPushButton;
    m_dstarStartStopBtn->setObjectName(QStringLiteral("dstarWaveformStartStop"));
    m_dstarStartStopBtn->setAccessibleName(tr("Start or stop D-STAR waveform"));
    connect(m_dstarStartStopBtn, &QPushButton::clicked,
            this, &WaveformsDialog::onDStarStartStopClicked);
    dstarHeader->addWidget(m_dstarStartStopBtn);
    dstarRoot->addLayout(dstarHeader);

    m_dstarStatusLabel = new QLabel;
    m_dstarStatusLabel->setObjectName(QStringLiteral("dstarWaveformStatus"));
    m_dstarStatusLabel->setAccessibleName(tr("D-STAR waveform status"));
    m_dstarStatusLabel->setTextFormat(Qt::RichText);
    dstarRoot->addWidget(m_dstarStatusLabel);

    m_dstarAutoStartCheck = new QCheckBox(tr("Auto-start"));
    m_dstarAutoStartCheck->setObjectName(QStringLiteral("dstarWaveformAutoStart"));
    m_dstarAutoStartCheck->setAccessibleName(tr("Auto-start D-STAR waveform"));
    m_dstarAutoStartCheck->setChecked(DStarWaveformSettings::autoStart());
    connect(m_dstarAutoStartCheck, &QCheckBox::toggled,
            this, &WaveformsDialog::saveDStarSettings);
    dstarRoot->addWidget(m_dstarAutoStartCheck);

    auto* backendRow = new QWidget;
    backendRow->setObjectName(QStringLiteral("dstarVocoderBackend"));
    auto* backendLayout = new QHBoxLayout(backendRow);
    backendLayout->setContentsMargins(0, 0, 0, 0);
    backendLayout->setSpacing(6);
    auto* backendLabel = new QLabel(tr("Vocoder"));
    backendLayout->addWidget(backendLabel);
    auto* backendValue = new QLabel(
        DStarWaveformSettings::backendLabel(DStarWaveformSettings::Backend::ThumbDv));
    backendValue->setAccessibleName(tr("D-STAR vocoder backend"));
    backendValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    backendLayout->addWidget(backendValue, 1);
    dstarRoot->addWidget(backendRow);

    m_dstarSerialLabel = new QLabel(tr("ThumbDV device"));
    dstarRoot->addWidget(m_dstarSerialLabel);

    m_dstarSerialRow = new QWidget;
    auto* serialLayout = new QHBoxLayout(m_dstarSerialRow);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    serialLayout->setSpacing(6);
    m_dstarSerialCombo = new QComboBox;
    m_dstarSerialCombo->setEditable(true);
    m_dstarSerialCombo->setInsertPolicy(QComboBox::NoInsert);
    m_dstarSerialCombo->setObjectName(QStringLiteral("dstarThumbDvSerialPort"));
    m_dstarSerialCombo->setAccessibleName(tr("ThumbDV serial port"));
    applyComboHeight(m_dstarSerialCombo);
    m_dstarSerialLabel->setBuddy(m_dstarSerialCombo);
#if defined(Q_OS_WIN)
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("COM3"));
#elif defined(Q_OS_MAC)
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("/dev/cu.usbserial-*"));
#else
    m_dstarSerialCombo->lineEdit()->setPlaceholderText(tr("/dev/ttyUSB0"));
#endif
    populateDStarSerialPorts(DStarWaveformSettings::serialPort());
    connect(m_dstarSerialCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &WaveformsDialog::saveDStarSettings);
    connect(m_dstarSerialCombo->lineEdit(), &QLineEdit::editingFinished,
            this, &WaveformsDialog::saveDStarSettings);
    serialLayout->addWidget(m_dstarSerialCombo, 1);
    m_dstarSerialMenuBtn = new QToolButton;
    m_dstarSerialMenuBtn->setArrowType(Qt::DownArrow);
    m_dstarSerialMenuBtn->setAccessibleName(tr("Open ThumbDV device choices"));
    m_dstarSerialMenuBtn->setToolTip(tr("Open ThumbDV device choices"));
    m_dstarSerialMenuBtn->setMinimumHeight(m_dstarSerialCombo->minimumHeight());
    m_dstarSerialMenuBtn->setFixedWidth(m_dstarSerialCombo->minimumHeight());
    connect(m_dstarSerialMenuBtn, &QToolButton::clicked,
            m_dstarSerialCombo, qOverload<>(&QComboBox::showPopup));
    serialLayout->addWidget(m_dstarSerialMenuBtn);
    m_dstarSerialRefreshBtn = new QPushButton(tr("Refresh"));
    m_dstarSerialRefreshBtn->setAccessibleName(tr("Refresh ThumbDV serial ports"));
    m_dstarSerialRefreshBtn->setMinimumHeight(m_dstarSerialCombo->minimumHeight());
    connect(m_dstarSerialRefreshBtn, &QPushButton::clicked, this, [this] {
        populateDStarSerialPorts(selectedDStarSerialPort());
        saveDStarSettings();
    });
    serialLayout->addWidget(m_dstarSerialRefreshBtn);
    dstarRoot->addWidget(m_dstarSerialRow);

    m_dstarAdvancedBtn = new QToolButton;
    m_dstarAdvancedBtn->setText(tr("Advanced"));
    m_dstarAdvancedBtn->setAccessibleName(tr("Show advanced D-STAR waveform settings"));
    m_dstarAdvancedBtn->setCheckable(true);
    m_dstarAdvancedBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_dstarAdvancedBtn->setChecked(!DStarWaveformSettings::executablePath().trimmed().isEmpty());
    m_dstarAdvancedBtn->setArrowType(
        m_dstarAdvancedBtn->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    dstarRoot->addWidget(m_dstarAdvancedBtn);

    m_dstarAdvancedPanel = new QWidget;
    auto* advancedLayout = new QVBoxLayout(m_dstarAdvancedPanel);
    advancedLayout->setContentsMargins(0, 0, 0, 0);
    advancedLayout->setSpacing(4);

    auto* exeRow = new QWidget;
    auto* exeLayout = new QHBoxLayout(exeRow);
    exeLayout->setContentsMargins(0, 0, 0, 0);
    exeLayout->setSpacing(6);
    m_dstarExecutableEdit = new QLineEdit(DStarWaveformSettings::executablePath());
    m_dstarExecutableEdit->setObjectName(QStringLiteral("dstarWaveformExecutable"));
    m_dstarExecutableEdit->setAccessibleName(tr("D-STAR waveform executable"));
    const QString defaultExecutablePath = DStarWaveformProcess::defaultExecutablePath();
    const QString defaultExecutableName = QFileInfo(defaultExecutablePath).fileName();
    m_dstarExecutableEdit->setPlaceholderText(
        tr("Default: %1").arg(defaultExecutableName.isEmpty()
            ? defaultExecutablePath
            : defaultExecutableName));
    auto updateExecutableHint = [this, defaultExecutablePath](const QString& path) {
        const QString trimmed = path.trimmed();
        const QString hint = trimmed.isEmpty()
            ? tr("Leave blank to use the default executable: %1").arg(defaultExecutablePath)
            : trimmed;
        m_dstarExecutableEdit->setToolTip(hint);
        m_dstarExecutableEdit->setAccessibleDescription(hint);
    };
    updateExecutableHint(m_dstarExecutableEdit->text());
    m_dstarExecutableEdit->setCursorPosition(m_dstarExecutableEdit->text().size());
    connect(m_dstarExecutableEdit, &QLineEdit::textChanged,
            this, updateExecutableHint);
    connect(m_dstarExecutableEdit, &QLineEdit::editingFinished,
            this, &WaveformsDialog::saveDStarSettings);
    exeLayout->addWidget(m_dstarExecutableEdit, 1);
    m_dstarBrowseBtn = new QPushButton(tr("Browse..."));
    m_dstarBrowseBtn->setAccessibleName(tr("Browse for D-STAR waveform executable"));
    connect(m_dstarBrowseBtn, &QPushButton::clicked,
            this, &WaveformsDialog::onDStarBrowseClicked);
    exeLayout->addWidget(m_dstarBrowseBtn);
    auto* executableLabel = new QLabel(tr("Executable"));
    executableLabel->setBuddy(m_dstarExecutableEdit);
    advancedLayout->addWidget(executableLabel);
    advancedLayout->addWidget(exeRow);
    m_dstarAdvancedPanel->setVisible(m_dstarAdvancedBtn->isChecked());
    connect(m_dstarAdvancedBtn, &QToolButton::toggled, this, [this](bool checked) {
        m_dstarAdvancedBtn->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        m_dstarAdvancedPanel->setVisible(checked);
        if (checked) {
            m_dstarExecutableEdit->setCursorPosition(m_dstarExecutableEdit->text().size());
        }
    });
    dstarRoot->addWidget(m_dstarAdvancedPanel);
    dstarRoot->addStretch();

    contentRow->addWidget(dstarFrame, 0);

    // ── Waveform list (scrollable) ────────────────────────────────────────────
    auto* installedFrame = new QFrame;
    installedFrame->setObjectName(QStringLiteral("installedWaveformsPanel"));
    installedFrame->setFrameShape(QFrame::StyledPanel);
    auto* installedRoot = new QVBoxLayout(installedFrame);
    installedRoot->setContentsMargins(10, 8, 10, 10);
    installedRoot->setSpacing(8);

    auto* installedHeader = new QHBoxLayout;
    auto* installedTitle = new QLabel(tr("<b>Radio Waveforms</b>"));
    installedTitle->setTextFormat(Qt::RichText);
    installedHeader->addWidget(installedTitle);
    installedHeader->addStretch();

    m_installBtn = new QToolButton;
    m_installBtn->setText(tr("Install..."));
    m_installBtn->setAccessibleName(tr("Install radio waveform"));
    m_installBtn->setPopupMode(QToolButton::InstantPopup);
    m_installBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_installBtn->setFixedWidth(104);
    m_installBtn->setEnabled(false);  // updated after installer state is known
    auto* installMenu = new QMenu(m_installBtn);
    installMenu->addAction(tr("Legacy Waveform..."),
                           this, &WaveformsDialog::onInstallLegacyClicked);
    installMenu->addAction(tr("Docker Waveform..."),
                           this, &WaveformsDialog::onInstallDockerClicked);
    m_installBtn->setMenu(installMenu);
    installedHeader->addWidget(m_installBtn);
    installedRoot->addLayout(installedHeader);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    m_listContainer = new QWidget;
    m_listLayout = new QVBoxLayout(m_listContainer);
    m_listLayout->setSpacing(4);
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->addStretch();

    scroll->setWidget(m_listContainer);
    installedRoot->addWidget(scroll, 1);
    contentRow->addWidget(installedFrame, 1);

    // ── Wire model + theme signals ────────────────────────────────────────────
    FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&wfModel, &FlexWaveformModel::wfpStatusChanged,
            this, &WaveformsDialog::updateInstallButtonState);
    connect(&wfModel, &FlexWaveformModel::waveformsChanged,
            this, &WaveformsDialog::refreshWaveformList);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshStatus);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshWaveformList);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &WaveformsDialog::refreshDStarStatus);
    connect(&DStarWaveformProcess::instance(), &DStarWaveformProcess::stateChanged,
            this, &WaveformsDialog::refreshDStarStatus);
    connect(&DStarWaveformProcess::instance(), &DStarWaveformProcess::statusTextChanged,
            this, &WaveformsDialog::refreshDStarStatus);
    connect(m_radioModel, &RadioModel::connectionStateChanged,
            this, &WaveformsDialog::updateDStarControls);
    connect(m_radioModel, &RadioModel::connectionStateChanged,
            this, &WaveformsDialog::updateInstallButtonState);

    refreshStatus();
    updateInstallButtonState();
    refreshWaveformList();
    refreshDStarStatus();
}

void WaveformsDialog::updateInstallButtonState()
{
    const bool busy  = m_installer && m_installer->isInstalling();
    m_installBtn->setEnabled(!busy && m_radioModel && m_radioModel->isConnected());
}

void WaveformsDialog::onInstallLegacyClicked()
{
    installWaveformFile(
        tr("Select Legacy Waveform Package"),
        tr("SmartSDR Waveforms (*.ssdr_waveform);;All Files (*)"),
        false);
}

void WaveformsDialog::onInstallDockerClicked()
{
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    if (!wfModel.wfpStatusSeen() || !wfModel.wfpPowered() || !wfModel.wfpReady()) {
        const QString powerText = wfModel.wfpPowered() ? tr("ON") : tr("OFF");
        const QString readyText = wfModel.wfpReady() ? tr("READY") : tr("NOT READY");
        const QString statusText = wfModel.wfpStatusSeen()
            ? tr("%1, %2").arg(powerText, readyText)
            : tr("not reported by this radio");
        showDockerWfpNotReadyDialog(this, statusText);
        return;
    }

    installWaveformFile(
        tr("Select Docker Waveform Image"),
        tr("Docker Waveform Images (*.tar *.tar.gz *.tgz);;All Files (*)"),
        true);
}

void WaveformsDialog::installWaveformFile(const QString& title,
                                          const QString& filter,
                                          bool docker)
{
    if (!m_radioModel || !m_radioModel->isConnected()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(
        this,
        title,
        {},
        filter);

    if (path.isEmpty()) {
        return;
    }

    if (!m_installer) {
        m_installer = new WaveformInstaller(m_radioModel, this);
    }

    if (m_installer->isInstalling()) {
        return;
    }

    m_installBtn->setEnabled(false);

    auto* progress = new PersistentDialog(
        docker ? tr("Installing Docker Waveform")
               : tr("Installing Legacy Waveform"),
        QString(),
        this);
    theme::setContainer(progress, QStringLiteral("dialog/waveforms/installProgress"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setModal(true);
    progress->setMinimumWidth(420);

    auto* progressRoot = new QVBoxLayout(progress->bodyWidget());
    progressRoot->setSpacing(10);

    auto* progressLabel = new QLabel(
        docker ? tr("Installing Docker waveform...")
               : tr("Installing legacy waveform..."));
    progressLabel->setTextFormat(Qt::PlainText);
    progressLabel->setWordWrap(true);
    progressLabel->setAccessibleName(tr("Waveform install progress"));
    progressRoot->addWidget(progressLabel);

    auto* progressBar = new QProgressBar;
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setTextVisible(true);
    progressBar->setAccessibleName(tr("Waveform install progress value"));
    progressRoot->addWidget(progressBar);

    auto* progressButtons = new QHBoxLayout;
    progressButtons->addStretch();
    auto* cancelButton = new QPushButton(tr("Cancel"));
    cancelButton->setAccessibleName(tr("Cancel waveform install"));
    progressButtons->addWidget(cancelButton);
    progressRoot->addLayout(progressButtons);

    connect(m_installer, &WaveformInstaller::progressChanged,
            progress, [progressBar, progressLabel](int pct, const QString& msg) {
                progressBar->setValue(pct);
                progressLabel->setText(msg);
            });

    connect(cancelButton, &QPushButton::clicked,
            progress, &QDialog::reject);
    connect(progress, &QDialog::rejected, this, [this] {
        if (m_installer && m_installer->isInstalling()) {
            m_installer->cancel();
        }
    });

    connect(m_installer, &WaveformInstaller::finished,
            this, [this, progress](bool ok, const QString& msg) {
                progress->close();
                progress->deleteLater();
                updateInstallButtonState();
                showWaveformInstallResultDialog(
                    this,
                    ok ? tr("Install Complete") : tr("Install Failed"),
                    msg);
            }, Qt::SingleShotConnection);

    progress->show();

    if (docker) {
        m_installer->installDockerWaveform(path);
    } else {
        m_installer->installLegacyWaveform(path);
    }
}

void WaveformsDialog::onDStarStartStopClicked()
{
    saveDStarSettings();

    DStarWaveformProcess& process = DStarWaveformProcess::instance();
    if (process.isActive()) {
        process.stop();
    } else {
        process.startForRadio(m_radioModel->radioAddress());
    }
    refreshDStarStatus();
}

void WaveformsDialog::onDStarBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Select D-STAR Waveform Executable"),
        m_dstarExecutableEdit->text().trimmed());
    if (path.isEmpty()) {
        return;
    }
    m_dstarExecutableEdit->setText(path);
    m_dstarExecutableEdit->setCursorPosition(path.size());
    m_dstarExecutableEdit->setFocus();
    saveDStarSettings();
}

void WaveformsDialog::refreshStatus()
{
    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    auto& tm = ThemeManager::instance();

    const QString onColor  = tm.color(this, QStringLiteral("color.accent")).name();
    const QString offColor = tm.color(this, QStringLiteral("color.text.secondary")).name();

    if (!wfModel.wfpStatusSeen()) {
        m_statusLabel->setText(
            QStringLiteral("WFP:&nbsp;&nbsp;"
                           "<font color='%1'>&#9679;</font> %2"
                           "&nbsp;&nbsp;&nbsp;"
                           "IP: %3")
                .arg(offColor, tr("unavailable"), tr("none")));
        return;
    }

    const QString powerColor = wfModel.wfpPowered() ? onColor : offColor;
    const QString readyColor = wfModel.wfpReady()   ? onColor : offColor;
    const QString powerText  = wfModel.wfpPowered() ? tr("ON")    : tr("OFF");
    const QString readyText  = wfModel.wfpReady()   ? tr("READY") : tr("NOT READY");

    QString ip = wfModel.wfpIpAddress();
    if (ip.isEmpty()) {
        ip = tr("none");
    }

    m_statusLabel->setText(
        QStringLiteral("WFP:&nbsp;&nbsp;"
                       "<font color='%1'>&#9679;</font> %2"
                       "&nbsp;&nbsp;&nbsp;"
                       "<font color='%3'>&#9679;</font> %4"
                       "&nbsp;&nbsp;&nbsp;"
                       "IP: %5")
            .arg(powerColor,
                 powerText.toHtmlEscaped(),
                 readyColor,
                 readyText.toHtmlEscaped(),
                 ip.toHtmlEscaped()));
}

void WaveformsDialog::refreshDStarStatus()
{
    const DStarWaveformProcess& process = DStarWaveformProcess::instance();
    auto& tm = ThemeManager::instance();
    const QString onColor  = tm.color(this, QStringLiteral("color.accent")).name();
    const QString offColor = tm.color(this, QStringLiteral("color.text.secondary")).name();
    const QString failColor = tm.color(this, QStringLiteral("color.accent.warning")).name();

    QString color = offColor;
    if (process.state() == DStarWaveformProcess::State::Running
            || process.state() == DStarWaveformProcess::State::Starting) {
        color = onColor;
    } else if (process.state() == DStarWaveformProcess::State::Failed) {
        color = failColor;
    }

    const QString text = process.statusText().isEmpty()
        ? DStarWaveformProcess::stateName(process.state())
        : process.statusText();
    m_dstarStatusLabel->setText(
        QStringLiteral("<font color='%1'>&#9679;</font> %2")
            .arg(color, text.toHtmlEscaped()));

    updateDStarControls();
}

void WaveformsDialog::updateDStarControls()
{
    const DStarWaveformProcess& process = DStarWaveformProcess::instance();
    const bool active = process.isActive();
    m_dstarStartStopBtn->setText(active ? tr("Stop") : tr("Start"));
    m_dstarStartStopBtn->setEnabled(active || m_radioModel->isConnected());
    m_dstarExecutableEdit->setEnabled(true);
    m_dstarExecutableEdit->setReadOnly(active);
    m_dstarBrowseBtn->setEnabled(!active);
    m_dstarSerialLabel->setVisible(true);
    m_dstarSerialRow->setVisible(true);
    m_dstarSerialCombo->setEnabled(!active);
    m_dstarSerialMenuBtn->setEnabled(!active);
    m_dstarSerialRefreshBtn->setEnabled(!active);
}

void WaveformsDialog::saveDStarSettings()
{
    DStarWaveformSettings::setAutoStart(m_dstarAutoStartCheck->isChecked());
    DStarWaveformSettings::setBackend(DStarWaveformSettings::Backend::ThumbDv);
    DStarWaveformSettings::setExecutablePath(m_dstarExecutableEdit->text());
    DStarWaveformSettings::setSerialPort(selectedDStarSerialPort());
    updateDStarControls();
}

void WaveformsDialog::populateDStarSerialPorts(const QString& preferredPort)
{
    const QString preferred = preferredPort.trimmed();
    const QSignalBlocker comboBlocker(m_dstarSerialCombo);
    const QSignalBlocker editBlocker(m_dstarSerialCombo->lineEdit());

    m_dstarSerialCombo->clear();
    const QList<SerialPortOption> options = detectedSerialPorts();

    int selectedIndex = -1;
    for (const SerialPortOption& option : options) {
        m_dstarSerialCombo->addItem(option.label, option.path);
        const int index = m_dstarSerialCombo->count() - 1;
        m_dstarSerialCombo->setItemData(index, option.path, Qt::ToolTipRole);
        if (!preferred.isEmpty() && option.path == preferred) {
            selectedIndex = index;
        }
    }

    if (!preferred.isEmpty() && selectedIndex < 0) {
        m_dstarSerialCombo->insertItem(0, preferred, preferred);
        selectedIndex = 0;
    } else if (preferred.isEmpty() && !options.isEmpty()) {
        selectedIndex = 0;
    }

    if (selectedIndex >= 0) {
        m_dstarSerialCombo->setCurrentIndex(selectedIndex);
    } else {
        m_dstarSerialCombo->lineEdit()->clear();
    }
}

QString WaveformsDialog::selectedDStarSerialPort() const
{
    if (!m_dstarSerialCombo) {
        return {};
    }

    const int index = m_dstarSerialCombo->currentIndex();
    const QString editText = m_dstarSerialCombo->currentText().trimmed();
    if (index >= 0 && editText == m_dstarSerialCombo->itemText(index)) {
        const QString path = m_dstarSerialCombo->itemData(index).toString().trimmed();
        if (!path.isEmpty()) {
            return path;
        }
    }
    return editText;
}

void WaveformsDialog::refreshWaveformList()
{
    // Remove all items except the trailing stretch
    while (m_listLayout->count() > 1) {
        QLayoutItem* item = m_listLayout->takeAt(0);
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }

    const FlexWaveformModel& wfModel = m_radioModel->flexWaveformModel();
    const QList<FlexWaveformEntry>& waveforms = wfModel.waveforms();

    if (waveforms.isEmpty()) {
        auto* placeholder = new QLabel(tr("No radio waveforms installed"));
        placeholder->setAlignment(Qt::AlignCenter);
        ThemeManager::instance().applyStyleSheet(placeholder,
            QStringLiteral("QLabel { color: {{color.text.secondary}}; }"));
        m_listLayout->insertWidget(0, placeholder);
        return;
    }

    for (const FlexWaveformEntry& entry : waveforms) {
        const QString name        = entry.name;
        const bool    isContainer = entry.isContainer;

        auto* row = new QFrame;
        row->setFrameShape(QFrame::StyledPanel);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 4, 8, 4);
        rowLayout->setSpacing(8);

        // Name + version
        auto* nameLabel = new QLabel(
            QStringLiteral("<b>%1</b> %2")
                .arg(name.toHtmlEscaped(), entry.version.toHtmlEscaped()));
        nameLabel->setTextFormat(Qt::RichText);
        rowLayout->addWidget(nameLabel, 1);

        // Type badge
        const QString badgeText = isContainer ? tr("Docker") : tr("Legacy");
        auto* typeLabel = new QLabel(QStringLiteral("[%1]").arg(badgeText));
        ThemeManager::instance().applyStyleSheet(typeLabel,
            QStringLiteral("QLabel { color: {{color.text.secondary}}; }"));
        rowLayout->addWidget(typeLabel);

        if (isContainer) {
            auto* restartBtn = new QPushButton(tr("Restart"));
            restartBtn->setFixedWidth(70);
            connect(restartBtn, &QPushButton::clicked, this, [this, name]() {
                m_radioModel->flexWaveformModel().requestRestart(name);
            });
            rowLayout->addWidget(restartBtn);
        }

        // Remove / Uninstall button
        const QString removeLabel = isContainer ? tr("Remove") : tr("Uninstall");
        auto* removeBtn = new QPushButton(removeLabel);
        removeBtn->setFixedWidth(70);
        connect(removeBtn, &QPushButton::clicked, this, [this, name, isContainer]() {
            if (!confirmRadioWaveformRemoval(this, name, isContainer)) {
                return;
            }
            if (isContainer) {
                m_radioModel->flexWaveformModel().requestRemoveContainer(name);
            } else {
                m_radioModel->flexWaveformModel().requestUninstall(name);
            }
        });
        rowLayout->addWidget(removeBtn);

        m_listLayout->insertWidget(m_listLayout->count() - 1, row);
    }
}

} // namespace AetherSDR
