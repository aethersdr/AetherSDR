#include "Ctr2ProxyApplet.h"

#include "ComboStyle.h"
#include "GuardedSlider.h"
#include "ScopedChildWidget.h"
#include "core/ThemeManager.h"
#include "models/Ctr2ProxyModel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#ifdef Q_OS_LINUX
#include "core/LogManager.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#endif

namespace AetherSDR {

namespace {

const QString kFieldStyle = QStringLiteral(
    "QLineEdit { background: {{color.background.1}}; "
    "border: 1px solid {{color.border.subtle}}; border-radius: 3px; "
    "padding: 2px 4px; color: {{color.text.primary}}; font-size: 10px; }"
    "QLineEdit:disabled { color: {{color.text.disabled}}; }");

const QString kButtonStyle = QStringLiteral(
    "QPushButton { background: {{color.toggle.background}}; "
    "border: 1px solid {{color.toggle.border}}; border-radius: 3px; "
    "padding: 2px 8px; font-size: 10px; font-weight: bold; "
    "color: {{color.toggle.foreground}}; }"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:disabled { background: {{color.button.background.disabled}}; "
    "color: {{color.button.foreground.disabled}}; "
    "border: 1px solid {{color.button.border.disabled}}; }");

const QString kComboExtra = QStringLiteral("QComboBox { font-size: 10px; }");

// High-contrast indicator, as in the CAT applet's per-port enables.
const QString kCheckStyle = QStringLiteral(
    "QCheckBox { color: {{color.text.primary}}; font-size: 10px; }"
    "QCheckBox:disabled { color: {{color.text.disabled}}; }"
    "QCheckBox::indicator { width: 13px; height: 13px; border-radius: 3px;"
    " border: 1px solid {{color.text.secondary}}; background: {{color.background.0}}; }"
    "QCheckBox::indicator:hover { border-color: {{color.accent}}; }"
    "QCheckBox::indicator:checked { border: 1px solid {{color.accent}};"
    " background: {{color.accent}}; }"
    "QCheckBox::indicator:disabled { border-color: {{color.border.subtle}};"
    " background: {{color.background.1}}; }");

QLabel* makeLabel(const QString& text, const QString& colorToken, QWidget* parent,
                  bool wrap = false)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(wrap);
    ThemeManager::instance().applyStyleSheet(
        label, QStringLiteral("QLabel { color: {{%1}}; font-size: 10px; }").arg(colorToken));
    return label;
}

QString formatBytes(quint64 bytes)
{
    return QLocale::c().toString(bytes) + QStringLiteral(" B");
}

void setAvailability(QWidget* w, bool enabled, const QString& reason)
{
    w->setEnabled(enabled);
    w->setAccessibleDescription(enabled ? QString() : reason);
}

} // namespace

Ctr2ProxyApplet::Ctr2ProxyApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/ctr2proxy"));
    setAccessibleName(tr("CTR2 Proxy"));
    buildUi();
    syncConfiguration();
    syncStatus();
}

void Ctr2ProxyApplet::buildUi()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(3);

    m_wifiCheck = new QCheckBox(tr("Wi-Fi"), this);
    m_wifiCheck->setObjectName(QStringLiteral("ctr2ProxyWifi"));
    m_wifiCheck->setAccessibleName(tr("Relay CTR2s over Wi-Fi"));
    ThemeManager::instance().applyStyleSheet(m_wifiCheck, kCheckStyle);
    grid->addWidget(m_wifiCheck, 0, 0);
    m_listenCombo = new GuardedComboBox(this);
    m_listenCombo->setObjectName(QStringLiteral("ctr2ProxyListenAddress"));
    m_listenCombo->setAccessibleName(tr("CTR2 proxy listen address"));
    applyComboStyle(m_listenCombo, kComboExtra);
    m_listenCombo->setFixedHeight(20);
    grid->addWidget(m_listenCombo, 0, 1);

    m_listenPortEdit = new QLineEdit(this);
    m_listenPortEdit->setObjectName(QStringLiteral("ctr2ProxyListenPort"));
    m_listenPortEdit->setAccessibleName(tr("CTR2 proxy listen port"));
    m_listenPortEdit->setValidator(new QIntValidator(1, 65535, m_listenPortEdit));
    m_listenPortEdit->setFixedWidth(48);
    ThemeManager::instance().applyStyleSheet(m_listenPortEdit, kFieldStyle);
    grid->addWidget(m_listenPortEdit, 0, 2);

    m_refreshBtn = new QPushButton(tr("Rescan"), this);
    m_refreshBtn->setObjectName(QStringLiteral("ctr2ProxyRescan"));
    m_refreshBtn->setAccessibleName(tr("Rescan local addresses and USB devices"));
    ThemeManager::instance().applyStyleSheet(m_refreshBtn, kButtonStyle);
    grid->addWidget(m_refreshBtn, 0, 3);

    auto* usbLabel = makeLabel(tr("USB"), QStringLiteral("color.text.label"), this);
    grid->addWidget(usbLabel, 1, 0, Qt::AlignTop);
    // One checkbox per HID device, rebuilt on every rescan.
    m_usbList = new QWidget(this);
    m_usbList->setObjectName(QStringLiteral("ctr2ProxyUsbDevices"));
    m_usbList->setAccessibleName(tr("CTR2 USB devices to relay"));
    m_usbListLayout = new QVBoxLayout(m_usbList);
    m_usbListLayout->setContentsMargins(0, 0, 0, 0);
    m_usbListLayout->setSpacing(2);
    m_usbEmptyLabel = makeLabel(QString(), QStringLiteral("color.text.secondary"), m_usbList, true);
    m_usbListLayout->addWidget(m_usbEmptyLabel);
    grid->addWidget(m_usbList, 1, 1, 1, 3);

    grid->addWidget(makeLabel(tr("Radio"), QStringLiteral("color.text.label"), this), 2, 0);
    // Always the radio AetherSDR is connected to; captured when Start is pressed.
    m_radioLabel = makeLabel(QString(), QStringLiteral("color.text.primary"), this);
    m_radioLabel->setObjectName(QStringLiteral("ctr2ProxyRadio"));
    grid->addWidget(m_radioLabel, 2, 1, 1, 2);

    m_startBtn = new QPushButton(tr("Start"), this);
    m_startBtn->setObjectName(QStringLiteral("ctr2ProxyStart"));
    m_startBtn->setAccessibleName(tr("Start CTR2 proxy"));
    ThemeManager::instance().applyStyleSheet(m_startBtn, kButtonStyle);
    grid->addWidget(m_startBtn, 2, 3);
    grid->setColumnStretch(1, 1);
    vbox->addLayout(grid);

    m_problemLabel = makeLabel(QString(), QStringLiteral("color.accent.warning"), this, true);
    m_problemLabel->setAccessibleName(tr("CTR2 proxy configuration"));
    vbox->addWidget(m_problemLabel);

    // Status readouts in their own pane with a 1 px minimum, so a window sized
    // below the default clips the status from the bottom before the layout
    // starts squeezing the controls above.
    auto* statusPane = new QWidget(this);
    statusPane->setMinimumHeight(1);
    auto* statusBox = new QVBoxLayout(statusPane);
    statusBox->setContentsMargins(0, 0, 0, 0);
    statusBox->setSpacing(4);

    m_stateLabel = makeLabel(QString(), QStringLiteral("color.text.primary"), statusPane);
    m_stateLabel->setObjectName(QStringLiteral("ctr2ProxyState"));
    statusBox->addWidget(m_stateLabel);
    // One row per link: the Wi-Fi listener, each CTR2 on it, each USB device.
    m_linksPane = new QWidget(statusPane);
    m_linksPane->setObjectName(QStringLiteral("ctr2ProxyLinks"));
    m_linksLayout = new QVBoxLayout(m_linksPane);
    m_linksLayout->setContentsMargins(0, 0, 0, 0);
    m_linksLayout->setSpacing(4);
    statusBox->addWidget(m_linksPane);
    m_errorLabel = makeLabel(QString(), QStringLiteral("color.accent.danger"), statusPane, true);
    m_errorLabel->setObjectName(QStringLiteral("ctr2ProxyError"));
    statusBox->addWidget(m_errorLabel);
    statusBox->addStretch(1);
    vbox->addWidget(statusPane, 1);

    connect(m_wifiCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model) {
            m_model->setWifiEnabled(on);
        }
    });
    connect(m_listenCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setListenAddress(idx > 0 ? m_listenCombo->itemText(idx) : QString());
        }
    });
    connect(m_listenPortEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (m_model) {
            m_model->setListenPortText(text);
        }
    });
    connect(m_refreshBtn, &QPushButton::clicked, this, [this] {
        if (m_model) {
            m_model->refreshDevices();
        }
    });
    connect(m_startBtn, &QPushButton::clicked, this, &Ctr2ProxyApplet::onStartClicked);
}

Ctr2ProxyApplet::LinkRow& Ctr2ProxyApplet::linkRow(int index)
{
    while (static_cast<int>(m_linkRows.size()) <= index) {
        LinkRow r;
        r.row = new QWidget(m_linksPane);
        auto* box = new QVBoxLayout(r.row);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(1);
        r.title = makeLabel(QString(), QStringLiteral("color.text.primary"), r.row);
        // Docked width is tight: let endpoints clip rather than widen the panel.
        r.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        r.traffic = makeLabel(QString(), QStringLiteral("color.text.secondary"), r.row);
        r.error = makeLabel(QString(), QStringLiteral("color.accent.danger"), r.row, true);
        box->addWidget(r.title);
        box->addWidget(r.traffic);
        box->addWidget(r.error);
        const int n = static_cast<int>(m_linkRows.size());
        r.row->setObjectName(QStringLiteral("ctr2ProxyLink%1").arg(n));
        m_linksLayout->addWidget(r.row);
        m_linkRows.push_back(r);
    }
    return m_linkRows[index];
}

void Ctr2ProxyApplet::onStartClicked()
{
    if (!m_model || m_installingRule) {
        return;
    }
    if (m_model->isRunning()) {
        m_model->stop();
        return;
    }
#ifdef Q_OS_LINUX
    if (m_model->usbDeviceNeedsAccessRule()) {
        offerUsbAccessRule();
        return;
    }
#endif
    m_model->start();
}

#ifdef Q_OS_LINUX
namespace {

const QString kRuleResource = QStringLiteral(":/udev/70-aethersdr-ctr2.rules");
const QString kRuleTarget = QStringLiteral("/etc/udev/rules.d/70-aethersdr-ctr2.rules");

QString loadRule()
{
    QFile file(kRuleResource);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

// True when this build's rule is already in place, locally or as packaged.
// Then a password prompt cannot help: the ACL is missing for another reason.
bool ruleAlreadyInstalled(const QString& rule)
{
    for (const QString& dir : {QStringLiteral("/etc/udev/rules.d"),
                               QStringLiteral("/usr/lib/udev/rules.d"),
                               QStringLiteral("/lib/udev/rules.d")}) {
        QFile file(dir + QStringLiteral("/70-aethersdr-ctr2.rules"));
        if (file.open(QIODevice::ReadOnly) && QString::fromUtf8(file.readAll()) == rule) {
            return true;
        }
    }
    return false;
}

} // namespace

// Linux opens USB devices like the CTR2 to root only until a udev rule grants
// the logged-in user access. Offer to install it rather than leave the
// operator with a permissions error.
void Ctr2ProxyApplet::offerUsbAccessRule()
{
    if (ruleAlreadyInstalled(loadRule())) {
        ScopedChildWidget<QMessageBox> box(
            QMessageBox::Information, tr("CTR2 USB access"),
            tr("The CTR2 access rule is already installed, but this login still cannot "
               "open the CTR2."),
            QMessageBox::Ok, this);
        box.get()->setInformativeText(
            tr("Unplug the CTR2, plug it back in, then press Start. The rule only opens "
               "the CTR2 to whoever is logged in at this computer's own screen, so a "
               "remote or background session cannot use it."));
        box.get()->exec();
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        showManualRuleInstructions(tr("pkexec is not installed"));
        return;
    }

    ScopedChildWidget<QMessageBox> box(
        QMessageBox::Question, tr("Allow AetherSDR to use the CTR2?"),
        tr("Linux only lets the administrator open USB devices such as the CTR2, so "
           "AetherSDR cannot talk to it yet."),
        QMessageBox::NoButton, this);
    box.get()->setInformativeText(
        tr("AetherSDR can install a small system rule that lets whoever is logged in "
           "at this computer open CTR2 controllers, and only CTR2 controllers. You "
           "will be asked for your administrator password once; after that the CTR2 "
           "works every time.\n\nThe rule is written to %1.").arg(kRuleTarget));
    QPushButton* install = box.get()->addButton(tr("Install rule"), QMessageBox::AcceptRole);
    box.get()->addButton(tr("Not now"), QMessageBox::RejectRole);
    box.get()->setDefaultButton(install);
    box.get()->exec();
    if (box && box.get()->clickedButton() == install) {
        installUsbAccessRule();
    }
}

// No usable polkit: save the rule where the operator can copy it by hand.
void Ctr2ProxyApplet::showManualRuleInstructions(const QString& why)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString saved = dir + QStringLiteral("/70-aethersdr-ctr2.rules");
    const QByteArray rule = loadRule().toUtf8();
    QFile out(saved);
    const bool wrote = !rule.isEmpty() && QDir().mkpath(dir)
        && out.open(QIODevice::WriteOnly) && out.write(rule) == rule.size();
    out.close();
    if (!wrote) {
        qCWarning(lcDevices) << "CTR2: could not save the udev rule to" << saved;
        ScopedChildWidget<QMessageBox> box(
            QMessageBox::Warning, tr("CTR2 USB access"),
            tr("Linux only lets the administrator open USB devices such as the CTR2. "
               "AetherSDR could not ask for permission (%1), and could not save the "
               "access rule to %2 for you to install by hand.").arg(why, saved),
            QMessageBox::Ok, this);
        box.get()->exec();
        return;
    }
    ScopedChildWidget<QMessageBox> box(
        QMessageBox::Warning, tr("CTR2 USB access"),
        tr("Linux only lets the administrator open USB devices such as the CTR2, "
           "and AetherSDR could not ask for permission: %1.\n\n"
           "To allow it, run these commands in a terminal, then unplug and "
           "replug the CTR2:").arg(why),
        QMessageBox::Ok, this);
    box.get()->setInformativeText(
        QStringLiteral("sudo cp '%1' '%2'\n"
                       "sudo udevadm control --reload-rules\n"
                       "sudo udevadm trigger --subsystem-match=hidraw")
            .arg(saved, kRuleTarget));
    box.get()->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.get()->exec();
}

void Ctr2ProxyApplet::installUsbAccessRule()
{
    m_installingRule = true;
    syncConfiguration();

    auto fail = [this](const QString& detail) {
        m_installingRule = false;
        syncConfiguration();
        ScopedChildWidget<QMessageBox> box(
            QMessageBox::Warning, tr("CTR2 USB access"),
            tr("The CTR2 access rule could not be installed."), QMessageBox::Ok, this);
        if (!detail.isEmpty()) {
            box.get()->setInformativeText(detail);
        }
        box.get()->exec();
    };

    const QString rule = loadRule();
    if (rule.isEmpty()) {
        qCWarning(lcDevices) << "CTR2: udev rule resource missing" << kRuleResource;
        fail(tr("This build is missing its copy of the rule."));
        return;
    }

    // Runs as root via polkit. The rule is passed as an argv element, never
    // interpolated into the script. settle waits for the ACL to be applied.
    static const QString kScript = QStringLiteral(
        "set -e; "
        "printf '%s' \"$1\" > \"$2\"; "
        "udevadm control --reload-rules; "
        "udevadm trigger --subsystem-match=hidraw; "
        "udevadm settle --timeout=5 || true");

    auto* proc = new QProcess(this);
    // If pkexec cannot start, finished never fires. A crash emits both
    // signals, so drop the other handler before showing the one dialog.
    connect(proc, &QProcess::errorOccurred, this, [proc, fail](QProcess::ProcessError) {
        if (proc->state() != QProcess::NotRunning) {
            return;
        }
        QObject::disconnect(proc, nullptr, nullptr, nullptr);
        const QString why = proc->errorString();
        proc->deleteLater();
        fail(why);
    });
    connect(proc, &QProcess::finished, this, [this, proc, fail](int code, QProcess::ExitStatus) {
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        if (code == 0) {
            startAfterRuleInstalled(12);
            return;
        }
        qCWarning(lcDevices) << "CTR2: udev rule install failed, code" << code << err;
        if (code == 126) {  // the operator dismissed the polkit dialog
            m_installingRule = false;
            syncConfiguration();
            return;
        }
        if (code == 127) {  // not authorized, or no polkit agent to ask with
            m_installingRule = false;
            syncConfiguration();
            showManualRuleInstructions(err.isEmpty() ? tr("authorization failed") : err);
            return;
        }
        fail(err);
    });
    proc->start(QStandardPaths::findExecutable(QStringLiteral("pkexec")),
                {QStringLiteral("/bin/sh"), QStringLiteral("-c"), kScript,
                 QStringLiteral("aethersdr"), rule, kRuleTarget});
}

// The ACL lands asynchronously after the trigger; wait briefly for it, then
// carry on with the Start the operator asked for.
void Ctr2ProxyApplet::startAfterRuleInstalled(int attemptsLeft)
{
    if (!m_model) {
        m_installingRule = false;
        return;
    }
    if (m_model->usbDeviceNeedsAccessRule() && attemptsLeft > 0) {
        QTimer::singleShot(250, this, [this, attemptsLeft] {
            startAfterRuleInstalled(attemptsLeft - 1);
        });
        return;
    }
    m_installingRule = false;
    syncConfiguration();
    if (m_model->selectedUsbDevices().isEmpty()) {
        return;  // the Start was for USB devices that are no longer checked
    }
    if (m_model->usbDeviceNeedsAccessRule()) {
        ScopedChildWidget<QMessageBox> box(
            QMessageBox::Information, tr("CTR2 USB access"),
            tr("The access rule is installed. Unplug the CTR2, plug it back in, "
               "then press Start."),
            QMessageBox::Ok, this);
        box.get()->exec();
        return;
    }
    m_model->start();
}
#endif

void Ctr2ProxyApplet::setModel(Ctr2ProxyModel* model)
{
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (m_model) {
        connect(m_model, &Ctr2ProxyModel::listenAddressesChanged, this, &Ctr2ProxyApplet::syncAddresses);
        connect(m_model, &Ctr2ProxyModel::configurationChanged, this, &Ctr2ProxyApplet::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyApplet::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::endpointsChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::lastErrorChanged, this, &Ctr2ProxyApplet::syncStatus);
        connect(m_model, &Ctr2ProxyModel::statsChanged, this, &Ctr2ProxyApplet::syncStatus);
    }
    syncAddresses();
    syncStatus();
}

void Ctr2ProxyApplet::syncAddresses()
{
    {
        const QSignalBlocker block(m_listenCombo);
        m_listenCombo->clear();
        // Placeholder first: the listen address is always an explicit choice.
        m_listenCombo->addItem(tr("Select address"));
        if (m_model) {
            m_listenCombo->addItems(m_model->availableListenAddresses());
            const int idx = m_listenCombo->findText(m_model->listenAddress());
            m_listenCombo->setCurrentIndex(idx > 0 ? idx : 0);
            if (idx <= 0 && !m_model->listenAddress().isEmpty() && !m_model->isRunning()) {
                m_model->setListenAddress(QString());
            }
        }
    }
    for (QCheckBox* check : m_usbChecks) {
        check->hide();
        check->deleteLater();
    }
    m_usbChecks.clear();
    const QList<Ctr2HidPort::DeviceInfo> devices =
        m_model ? m_model->availableUsbDevices() : QList<Ctr2HidPort::DeviceInfo>();
    for (const Ctr2HidPort::DeviceInfo& d : devices) {
        auto* check = new QCheckBox(d.label(), m_usbList);
        check->setObjectName(QStringLiteral("ctr2ProxyUsbDevice%1").arg(m_usbChecks.size()));
        check->setAccessibleName(tr("Relay USB device %1").arg(d.label()));
        check->setToolTip(d.label());   // the docked column clips long names
        ThemeManager::instance().applyStyleSheet(check, kCheckStyle);
        const QString path = d.path;
        connect(check, &QCheckBox::toggled, this, [this, path](bool on) {
            if (m_model) {
                m_model->setUsbDeviceSelected(path, on);
            }
        });
        m_usbListLayout->addWidget(check);
        m_usbChecks.push_back(check);
    }
    m_usbEmptyLabel->setText(!m_model || !m_model->usbAvailable()
                                 ? tr("This build has no USB HID support (hidapi)")
                                 : tr("No USB HID devices found"));
    m_usbEmptyLabel->setVisible(m_usbChecks.empty());
    syncConfiguration();
}

void Ctr2ProxyApplet::syncConfiguration()
{
    const bool haveModel = m_model != nullptr;
    const bool running = haveModel && m_model->isRunning();
    const bool editable = haveModel && !running && !m_installingRule;
    const bool wifi = haveModel && m_model->wifiEnabled();
    const QString frozen = !haveModel     ? tr("Proxy unavailable")
        : m_installingRule ? tr("Waiting for administrator approval")
                           : tr("Stop the proxy to change its settings");

    if (haveModel) {
        const QSignalBlocker block(m_wifiCheck);
        m_wifiCheck->setChecked(wifi);
    }
    setAvailability(m_wifiCheck, editable, frozen);
    setAvailability(m_refreshBtn, editable, frozen);
    const QString wifiOff = tr("Check Wi-Fi to relay CTR2s over the network");
    setAvailability(m_listenCombo, editable && wifi, editable ? wifiOff : frozen);
    setAvailability(m_listenPortEdit, editable && wifi, editable ? wifiOff : frozen);
    const auto devices = haveModel ? m_model->availableUsbDevices()
                                   : QList<Ctr2HidPort::DeviceInfo>();
    for (size_t i = 0; i < m_usbChecks.size() && i < static_cast<size_t>(devices.size()); ++i) {
        QCheckBox* check = m_usbChecks[i];
        const QSignalBlocker block(check);
        check->setChecked(m_model->isUsbDeviceSelected(devices[static_cast<int>(i)].path));
        setAvailability(check, editable, frozen);
    }

    if (haveModel) {
        if (m_listenPortEdit->text() != m_model->listenPortText()) {
            m_listenPortEdit->setText(m_model->listenPortText());
        }
        const QString radio = m_model->aetherRadioLabel();
        m_radioLabel->setText(radio.isEmpty() ? tr("Not connected") : radio);
        m_radioLabel->setAccessibleName(tr("Relay radio: %1").arg(m_radioLabel->text()));
    }

    const QString problem = editable ? m_model->configurationProblem() : QString();
    m_problemLabel->setText(problem);
    m_problemLabel->setVisible(!problem.isEmpty());
    m_startBtn->setText(m_installingRule ? tr("Authorizing…")
                                          : (running ? tr("Stop") : tr("Start")));
    m_startBtn->setAccessibleName(m_installingRule ? tr("Authorizing CTR2 USB access")
                                  : (running ? tr("Stop CTR2 proxy") : tr("Start CTR2 proxy")));
    m_startBtn->setEnabled(haveModel && !m_installingRule && (running || problem.isEmpty()));
    m_startBtn->setAccessibleDescription(
        !haveModel ? tr("Proxy unavailable")
        : m_installingRule ? frozen
        : (running ? QString() : problem));
}

void Ctr2ProxyApplet::syncStatus()
{
    const QList<Ctr2ProxyModel::Link> links =
        m_model ? m_model->links() : QList<Ctr2ProxyModel::Link>();
    int relaying = 0;
    for (const Ctr2ProxyModel::Link& link : links) {
        if (link.kind != Ctr2ProxyModel::Link::Kind::WifiListener
            && link.state == TcpByteProxy::State::Relaying) {
            ++relaying;
        }
    }
    const bool running = m_model && m_model->isRunning();
    m_stateLabel->setText(running ? tr("State: Running, %n relaying", nullptr, relaying)
                                  : tr("State: Stopped"));
    m_stateLabel->setAccessibleName(m_stateLabel->text());

    // Compact text for the narrow column; the accessible names spell it out.
    auto direction = [](const QString& label, quint64 sent, qint64 queued) {
        QString line = label.arg(formatBytes(sent));
        if (queued > 0) {
            line += tr(" (+%1 queued)").arg(formatBytes(static_cast<quint64>(queued)));
        }
        return line;
    };
    for (int i = 0; i < links.size(); ++i) {
        const Ctr2ProxyModel::Link& link = links[i];
        const TcpByteProxy::Stats& s = link.stats;
        LinkRow& row = linkRow(i);
        const QString title = tr("%1: %2").arg(link.name, link.stateText);
        QStringList lines;
        QStringList spoken{title};
        if (link.kind == Ctr2ProxyModel::Link::Kind::WifiListener) {
            if (s.rejectedClients > 0) {
                lines << tr("Rejected %1").arg(s.rejectedClients);
                spoken << tr("Rejected extra clients: %1").arg(s.rejectedClients);
            }
        } else {
            lines << direction(tr("↑ Radio %1"), s.toUpstream, s.queuedToUpstream)
                  << direction(tr("↓ CTR2 %1"), s.toDownstream, s.queuedToDownstream);
            spoken << tr("To radio %1, queued %2")
                          .arg(formatBytes(s.toUpstream),
                               formatBytes(static_cast<quint64>(s.queuedToUpstream)))
                   << tr("To CTR2 %1, queued %2")
                          .arg(formatBytes(s.toDownstream),
                               formatBytes(static_cast<quint64>(s.queuedToDownstream)));
        }
        if (link.kind == Ctr2ProxyModel::Link::Kind::Usb) {
            lines << tr("UDP ↑%1 ↓%2 ✕%3")
                         .arg(s.datagramsToRadio).arg(s.datagramsToDevice).arg(s.datagramsDropped);
            spoken << tr("UDP: %1 to radio, %2 to CTR2, %3 dropped")
                          .arg(s.datagramsToRadio).arg(s.datagramsToDevice).arg(s.datagramsDropped);
        }
        const QString error = link.error.isEmpty() ? QString()
                                                   : tr("Last error: %1").arg(link.error);
        if (!error.isEmpty()) {
            spoken << error;
        }
        row.title->setText(title);
        row.traffic->setText(lines.join(QStringLiteral("  ")));
        row.traffic->setVisible(!lines.isEmpty());
        row.error->setText(error);
        row.error->setVisible(!error.isEmpty());
        row.row->setAccessibleName(spoken.join(QStringLiteral(", ")));
        row.title->setAccessibleName(title);
        row.traffic->setAccessibleName(spoken.mid(1).join(QStringLiteral(", ")));
        row.error->setAccessibleName(error);
        row.row->show();
    }
    for (size_t i = links.size(); i < m_linkRows.size(); ++i) {
        m_linkRows[i].row->hide();
    }

    // The links carry their own errors; this is why the relay stopped itself.
    const QString err = m_model && links.isEmpty() ? m_model->lastError() : QString();
    m_errorLabel->setText(err.isEmpty() ? QString() : tr("Last error: %1").arg(err));
    m_errorLabel->setVisible(!err.isEmpty());
    m_errorLabel->setAccessibleName(m_errorLabel->text());
}

} // namespace AetherSDR
