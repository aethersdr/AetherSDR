#include "Ctr2ProxyPanel.h"

#include "CanonMessageBox.h"
#include "Ctr2AccessRule.h"
#include "GuardedSlider.h"
#include "ScopedChildWidget.h"
#include "models/Ctr2ProxyModel.h"

#include <QFile>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QVBoxLayout>

#ifdef Q_OS_LINUX
#include "core/LogManager.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <utility>
#endif

namespace AetherSDR {

namespace {

// A row label (the sheet's SectionLabel), or a value in the given tone.
QLabel* makeLabel(const QString& text, QWidget* parent, const char* tone = nullptr,
                  bool wrap = false)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(wrap);
    if (tone)
        label->setProperty("tone", QString::fromLatin1(tone));
    return label;
}

QLabel* makeRowLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("SectionLabel"));
    return label;
}

void setAvailability(QWidget* w, bool enabled, const QString& reason)
{
    w->setEnabled(enabled);
    w->setAccessibleDescription(enabled ? QString() : reason);
}

} // namespace

Ctr2ProxyPanel::Ctr2ProxyPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ctr2ProxyPanel"));
    setAccessibleName(tr("CTR2 Proxy"));
    buildUi();
    syncConfiguration();
    syncStatus();
    syncStats();
}

void Ctr2ProxyPanel::buildUi()
{
    auto* vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(8);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);

    grid->addWidget(makeRowLabel(tr("Mode"), this), 0, 0);
    m_modeCombo = new GuardedComboBox(this);
    m_modeCombo->setObjectName(QStringLiteral("ctr2ProxyMode"));
    m_modeCombo->setAccessibleName(tr("CTR2 connection mode"));
    m_modeCombo->addItem(tr("Wi-Fi"));
    m_modeCombo->addItem(tr("USB"));
    grid->addWidget(m_modeCombo, 0, 1, 1, 2);

    m_refreshBtn = new QPushButton(tr("Rescan"), this);
    m_refreshBtn->setObjectName(QStringLiteral("ctr2ProxyRescan"));
    m_refreshBtn->setAccessibleName(tr("Rescan local addresses and USB devices"));
    grid->addWidget(m_refreshBtn, 0, 3);

    m_listenRowLabel = makeRowLabel(tr("Listen"), this);
    grid->addWidget(m_listenRowLabel, 1, 0);
    m_listenCombo = new GuardedComboBox(this);
    m_listenCombo->setObjectName(QStringLiteral("ctr2ProxyListenAddress"));
    m_listenCombo->setAccessibleName(tr("CTR2 proxy listen address"));
    grid->addWidget(m_listenCombo, 1, 1);

    m_listenPortEdit = new QLineEdit(this);
    m_listenPortEdit->setObjectName(QStringLiteral("ctr2ProxyListenPort"));
    m_listenPortEdit->setAccessibleName(tr("CTR2 proxy listen port"));
    m_listenPortEdit->setValidator(new QIntValidator(1, 65535, m_listenPortEdit));
    m_listenPortEdit->setFixedWidth(64);
    grid->addWidget(m_listenPortEdit, 1, 2);

    m_usbRowLabel = makeRowLabel(tr("USB"), this);
    grid->addWidget(m_usbRowLabel, 2, 0);
    m_usbCombo = new GuardedComboBox(this);
    m_usbCombo->setObjectName(QStringLiteral("ctr2ProxyUsbDevice"));
    m_usbCombo->setAccessibleName(tr("CTR2 USB device"));
    grid->addWidget(m_usbCombo, 2, 1, 1, 3);

    grid->addWidget(makeRowLabel(tr("Radio"), this), 3, 0);
    // Always the radio AetherSDR is connected to; captured when Start is pressed.
    m_radioLabel = makeLabel(QString(), this);
    m_radioLabel->setObjectName(QStringLiteral("ctr2ProxyRadio"));
    grid->addWidget(m_radioLabel, 3, 1, 1, 2);

    m_startBtn = new QPushButton(tr("Start"), this);
    m_startBtn->setObjectName(QStringLiteral("ctr2ProxyStart"));
    m_startBtn->setAccessibleName(tr("Start CTR2 proxy"));
    grid->addWidget(m_startBtn, 3, 3);

    m_autoStart = new QCheckBox(tr("Start automatically when AetherSDR connects to a radio"), this);
    m_autoStart->setObjectName(QStringLiteral("ctr2ProxyAutoStart"));
    m_autoStart->setToolTip(tr("Starts the relay with the setup it last started with, whenever "
                               "AetherSDR connects to a radio, and keeps trying until the CTR2 "
                               "is plugged in. Stop holds it off until the next connection."));
    grid->addWidget(m_autoStart, 4, 1, 1, 3);

    grid->setColumnStretch(1, 1);
    vbox->addLayout(grid);

    m_problemLabel = makeLabel(QString(), this, "warning", true);
    m_problemLabel->setAccessibleName(tr("CTR2 proxy configuration"));
    vbox->addWidget(m_problemLabel);

    auto* statusPane = new QWidget(this);
    auto* statusBox = new QVBoxLayout(statusPane);
    statusBox->setContentsMargins(0, 0, 0, 0);
    statusBox->setSpacing(4);

    m_stateLabel = makeLabel(QString(), statusPane);
    m_stateLabel->setObjectName(QStringLiteral("ctr2ProxyState"));
    statusBox->addWidget(m_stateLabel);
    // Endpoints and traffic side by side to save vertical space.
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    m_endpointsLabel = makeLabel(QString(), statusPane, "secondary");
    m_endpointsLabel->setObjectName(QStringLiteral("ctr2ProxyEndpoints"));
    // Carries a device's own product string; never render it as rich text.
    m_endpointsLabel->setTextFormat(Qt::PlainText);
    m_endpointsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    // Let long endpoints clip rather than widen the window.
    m_endpointsLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    statusRow->addWidget(m_endpointsLabel, 1);
    m_traffic = new QWidget(statusPane);
    m_traffic->setObjectName(QStringLiteral("ctr2ProxyTraffic"));
    auto* table = new QGridLayout(m_traffic);
    table->setContentsMargins(0, 0, 0, 0);
    table->setHorizontalSpacing(12);
    table->setVerticalSpacing(2);
    // Values right-aligned in columns wide enough for 10 digits, in fixed-
    // width digits: a count gaining a digit grows leftward inside its column.
    const auto value = [this] {
        QLabel* l = makeLabel(QStringLiteral("0"), m_traffic, "readout");
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QFont f = l->font();
        f.setFeature(QFont::Tag("tnum"), 1);
        l->setFont(f);
        return l;
    };
    const auto name = [this](const QString& text) {
        QLabel* l = makeLabel(text, m_traffic, "secondary");
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return l;
    };
    const auto header = [this](const QString& text) {
        QLabel* l = makeLabel(text, m_traffic, "secondary");
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    };
    table->addWidget(header(tr("\u2191 Radio")), 0, 1);
    table->addWidget(header(tr("\u2193 CTR2")), 0, 2);
    table->addWidget(name(tr("Bytes")), 1, 0);
    table->addWidget(m_bytesUp = value(), 1, 1);
    table->addWidget(m_bytesDown = value(), 1, 2);
    table->addWidget(name(tr("Queued")), 2, 0);
    table->addWidget(m_queuedUp = value(), 2, 1);
    table->addWidget(m_queuedDown = value(), 2, 2);
    table->addWidget(m_udpName = name(tr("UDP")), 3, 0);
    table->addWidget(m_udpUp = value(), 3, 1);
    table->addWidget(m_udpDown = value(), 3, 2);
    table->addWidget(m_extraName = name(QString()), 4, 0);
    table->addWidget(m_extraValue = value(), 4, 2);
    const int column = m_bytesUp->fontMetrics().horizontalAdvance(QStringLiteral("0,000,000,000"));
    table->setColumnMinimumWidth(1, column);
    table->setColumnMinimumWidth(2, column);
    statusRow->addWidget(m_traffic, 0, Qt::AlignTop);
    statusBox->addLayout(statusRow);
    m_errorLabel = makeLabel(QString(), statusPane, "danger", true);
    m_errorLabel->setObjectName(QStringLiteral("ctr2ProxyError"));
    // The error can carry a device's own product string; never render it as rich text.
    m_errorLabel->setTextFormat(Qt::PlainText);
    statusBox->addWidget(m_errorLabel);
    vbox->addWidget(statusPane);

    connect(m_autoStart, &QCheckBox::toggled, this, [this](bool on) {
        if (m_model)
            m_model->setAutoStart(on);
    });
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setTransport(idx == 1 ? Ctr2ProxyModel::Transport::Usb
                                           : Ctr2ProxyModel::Transport::Wifi);
        }
    });
    connect(m_listenCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setListenAddress(idx > 0 ? m_listenCombo->itemText(idx) : QString());
        }
    });
    connect(m_usbCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (m_model) {
            m_model->setUsbDevicePath(idx > 0 ? m_usbCombo->itemData(idx).toString() : QString());
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
    connect(m_startBtn, &QPushButton::clicked, this, &Ctr2ProxyPanel::onStartClicked);
}

void Ctr2ProxyPanel::onStartClicked()
{
    if (!m_model || m_installingRule) {
        return;
    }
    if (m_model->isRunning()) {
        m_model->stopByOperator();
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

// True when this build's rule is the one udev uses, locally or as packaged.
// Then a password prompt cannot help: the ACL is missing for another reason.
// An older copy in /etc masks a current packaged one, and is not installed.
bool ruleAlreadyInstalled(const QString& rule)
{
    return udevRuleInEffect(udevRuleDirs(), QStringLiteral("70-aethersdr-ctr2.rules"), rule);
}

} // namespace

// Linux opens USB devices like the CTR2 to root only until a udev rule grants
// the logged-in user access. Offer to install it rather than leave the
// operator with a permissions error.
bool Ctr2ProxyPanel::accessRuleInstalled()
{
    return ruleAlreadyInstalled(loadRule());
}

bool Ctr2ProxyPanel::offerAccessRule(RuleUse use, std::function<void()> afterInstalled)
{
    if (m_installingRule) {
        return false;
    }
    m_afterRule = std::move(afterInstalled);
    return offerUsbAccessRule(use);
}

bool Ctr2ProxyPanel::offerUsbAccessRule(RuleUse use)
{
    const bool firmware = use == RuleUse::DownloadMode;
    const bool relay = use == RuleUse::Relay;
    if (ruleAlreadyInstalled(loadRule())) {
        m_afterRule = nullptr;
        ScopedChildWidget<CanonMessageBox> box(
            tr("CTR2 USB access"),
            firmware ? tr("The CTR2 access rule is already installed, but this login still "
                          "cannot open the CTR2-Max in download mode.")
                     : tr("The CTR2 access rule is already installed, but this login still "
                          "cannot open the CTR2."), this);
        box.get()->setInformativeText(
            firmware ? tr("Press Install again in a moment, or unplug the CTR2-Max and put it "
                          "back in download mode. The rule only opens it to whoever is logged "
                          "in at this computer's own screen, so a remote or background "
                          "session cannot use it.")
                     : (relay ? tr("Unplug the CTR2, plug it back in, then press Start. ")
                              : tr("Unplug the CTR2, plug it back in, then try again. "))
                           + tr("The rule only opens the CTR2 to whoever is logged in at this "
                                "computer's own screen, so a remote or background session "
                                "cannot use it."));
        box.get()->setDefaultButton(box.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept));
        box.get()->exec();
        return false;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty()) {
        m_afterRule = nullptr;
        showManualRuleInstructions(tr("pkexec is not installed"));
        return false;
    }

    ScopedChildWidget<CanonMessageBox> box(
        firmware ? tr("Allow AetherSDR to install firmware on the CTR2-Max?")
                 : tr("Allow AetherSDR to use the CTR2?"),
        firmware ? tr("Linux only lets the administrator open the CTR2-Max's port in "
                      "download mode, so AetherSDR cannot install firmware on it yet.")
                 : tr("Linux only lets the administrator open USB devices such as the CTR2, "
                      "so AetherSDR cannot talk to it yet."),
        this);
    box.get()->setInformativeText(
        tr("AetherSDR can install a small system rule that lets whoever is logged in "
           "at this computer open CTR2 controllers, and the USB serial port of ESP32-S3 "
           "boards, which the CTR2-Max uses in download mode for firmware installs. You "
           "will be asked for your administrator password once; after that it works "
           "every time.\n\nThe rule is written to %1.")
            .arg(kRuleTarget));
    box.get()->addButton(tr("Not now"), CanonMessageBox::Role::Reject);
    QPushButton* install = box.get()->addButton(tr("Install rule"), CanonMessageBox::Role::Accept);
    box.get()->setDefaultButton(install);
    box.get()->exec();
    if (box && box.get()->clickedButton() == install) {
        installUsbAccessRule(use);
        return true;
    }
    m_afterRule = nullptr;
    return false;
}

// No usable polkit: save the rule where the operator can copy it by hand.
void Ctr2ProxyPanel::showManualRuleInstructions(const QString& why)
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
        ScopedChildWidget<CanonMessageBox> box(
            tr("CTR2 USB access"),
            tr("Linux only lets the administrator open USB devices such as the CTR2. "
               "AetherSDR could not ask for permission (%1), and could not save the "
               "access rule to %2 for you to install by hand.").arg(why, saved), this);
        box.get()->setDefaultButton(box.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept));
        box.get()->exec();
        return;
    }
    ScopedChildWidget<CanonMessageBox> box(
        tr("CTR2 USB access"),
        tr("Linux only lets the administrator open USB devices such as the CTR2, "
           "and AetherSDR could not ask for permission: %1.\n\n"
           "To allow it, run these commands in a terminal, then unplug and "
           "replug the CTR2:").arg(why), this);
    box.get()->setInformativeText(
        QStringLiteral("sudo cp '%1' '%2'\n"
                       "sudo udevadm control --reload-rules\n"
                       "sudo udevadm trigger --subsystem-match=hidraw\n"
                       "sudo udevadm trigger --subsystem-match=tty "
                       "--property-match=ID_VENDOR_ID=303a --property-match=ID_VENDOR_ID=2886")
            .arg(saved, kRuleTarget));
    box.get()->setTextSelectable(true);
    box.get()->setDefaultButton(box.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept));
    box.get()->exec();
}

void Ctr2ProxyPanel::installUsbAccessRule(RuleUse use)
{
    m_installingRule = true;
    syncConfiguration();

    auto fail = [this](const QString& detail) {
        m_installingRule = false;
        m_afterRule = nullptr;
        syncConfiguration();
        ScopedChildWidget<CanonMessageBox> box(
            tr("CTR2 USB access"),
            tr("The CTR2 access rule could not be installed."), this);
        if (!detail.isEmpty()) {
            box.get()->setInformativeText(detail);
        }
        box.get()->setDefaultButton(box.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept));
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
        // hidraw nodes carry no ID_VENDOR_ID before systemd 256, so they are
        // all retriggered; ttys only for Espressif and Seeed devices.
        "udevadm trigger --subsystem-match=hidraw; "
        "udevadm trigger --subsystem-match=tty "
        "--property-match=ID_VENDOR_ID=303a --property-match=ID_VENDOR_ID=2886; "
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
    connect(proc, &QProcess::finished, this, [this, proc, fail, use](int code, QProcess::ExitStatus) {
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        if (code == 0 && use != RuleUse::Relay) {
            // Carry on with the CTR2 window's action. On the hardware run the
            // ACL was in place by then; if a port is still denied, the window
            // retries once more before saying anything.
            m_installingRule = false;
            syncConfiguration();
            if (auto next = std::exchange(m_afterRule, nullptr))
                next();
            return;
        }
        if (code == 0) {
            startAfterRuleInstalled(12);
            return;
        }
        qCWarning(lcDevices) << "CTR2: udev rule install failed, code" << code << err;
        m_afterRule = nullptr;
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
void Ctr2ProxyPanel::startAfterRuleInstalled(int attemptsLeft)
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
    if (m_model->transport() != Ctr2ProxyModel::Transport::Usb) {
        return;  // the Start was for USB; never start another transport from here
    }
    if (m_model->usbDeviceNeedsAccessRule()) {
        ScopedChildWidget<CanonMessageBox> box(
            tr("CTR2 USB access"),
            tr("The access rule is installed. Unplug the CTR2, plug it back in, "
               "then press Start."), this);
        box.get()->setDefaultButton(box.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept));
        box.get()->exec();
        return;
    }
    m_model->start();
}
#endif

void Ctr2ProxyPanel::setModel(Ctr2ProxyModel* model)
{
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (m_model) {
        connect(m_model, &Ctr2ProxyModel::listenAddressesChanged, this, &Ctr2ProxyPanel::syncAddresses);
        connect(m_model, &Ctr2ProxyModel::configurationChanged, this, &Ctr2ProxyPanel::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyPanel::syncConfiguration);
        connect(m_model, &Ctr2ProxyModel::stateChanged, this, &Ctr2ProxyPanel::syncStatus);
        connect(m_model, &Ctr2ProxyModel::endpointsChanged, this, &Ctr2ProxyPanel::syncStatus);
        connect(m_model, &Ctr2ProxyModel::lastErrorChanged, this, &Ctr2ProxyPanel::syncStatus);
        connect(m_model, &Ctr2ProxyModel::statsChanged, this, &Ctr2ProxyPanel::syncStats);
        connect(m_model, &Ctr2ProxyModel::autoStartChanged, this, [this] {
            const QSignalBlocker block(m_autoStart);
            m_autoStart->setChecked(m_model->autoStart());
        });
    }
    {
        const QSignalBlocker block(m_autoStart);
        m_autoStart->setChecked(m_model && m_model->autoStart());
        m_autoStart->setEnabled(m_model != nullptr);
    }
    syncAddresses();
    syncConfiguration();
    syncStatus();
    syncStats();
}

void Ctr2ProxyPanel::syncAddresses()
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
    {
        const QSignalBlocker block(m_usbCombo);
        m_usbCombo->clear();
        m_usbCombo->addItem(tr("Select CTR2 USB device"));
        if (m_model) {
            for (const Ctr2HidPort::DeviceInfo& d : m_model->availableUsbDevices()) {
                m_usbCombo->addItem(d.label(), d.path);
            }
            const int idx = m_usbCombo->findData(m_model->usbDevicePath());
            m_usbCombo->setCurrentIndex(idx > 0 ? idx : 0);
            if (idx <= 0 && !m_model->usbDevicePath().isEmpty() && !m_model->isRunning()) {
                m_model->setUsbDevicePath(QString());
            }
        }
    }
}

void Ctr2ProxyPanel::syncConfiguration()
{
    const bool haveModel = m_model != nullptr;
    const bool running = haveModel && m_model->isRunning();
    const bool editable = haveModel && !running && !m_installingRule;
    const bool usb = haveModel && m_model->transport() == Ctr2ProxyModel::Transport::Usb;
    const QString frozen = !haveModel     ? tr("Proxy unavailable")
        : m_installingRule ? tr("Waiting for administrator approval")
                           : tr("Stop the proxy to change its settings");

    if (haveModel) {
        const QSignalBlocker block(m_modeCombo);
        m_modeCombo->setCurrentIndex(usb ? 1 : 0);
    }
    setAvailability(m_modeCombo, editable, frozen);
    setAvailability(m_refreshBtn, editable, frozen);
    const QString wifiOnly = tr("Used in Wi-Fi mode only");
    setAvailability(m_listenCombo, editable && !usb, editable ? wifiOnly : frozen);
    setAvailability(m_listenPortEdit, editable && !usb, editable ? wifiOnly : frozen);
    const bool usbAvailable = haveModel && m_model->usbAvailable();
    const QString usbReason = !usbAvailable
        ? tr("This build has no USB HID support (hidapi)")
        : tr("Used in USB mode only");
    const bool usbEditable = editable && usb && usbAvailable;
    setAvailability(m_usbCombo, usbEditable, editable ? usbReason : frozen);
    // Only the row the mode uses is shown; the grid collapses the hidden one.
    m_listenRowLabel->setVisible(!usb);
    m_listenCombo->setVisible(!usb);
    m_listenPortEdit->setVisible(!usb);
    m_usbRowLabel->setVisible(usb);
    m_usbCombo->setVisible(usb);

    if (haveModel) {
        if (m_listenPortEdit->text() != m_model->listenPortText()) {
            m_listenPortEdit->setText(m_model->listenPortText());
        }
    }

    if (haveModel) {
        const QString radio = m_model->aetherRadioLabel();
        m_radioLabel->setText(radio.isEmpty() ? tr("Not connected") : radio);
        m_radioLabel->setAccessibleName(tr("Relay radio: %1").arg(m_radioLabel->text()));
    }

    const QString problem = editable ? m_model->configurationProblem() : QString();
    // While running, the hint that the CTR2 has not answered the link.
    const QString hint = problem.isEmpty() && running ? m_model->noAnswerHint() : problem;
    m_problemLabel->setText(hint);
    m_problemLabel->setAccessibleName(problem.isEmpty() && running ? tr("CTR2 link status")
                                                                   : tr("CTR2 proxy configuration"));
    m_problemLabel->setVisible(!hint.isEmpty());
    m_startBtn->setText(m_installingRule ? tr("Authorizing\u2026")
                                          : (running ? tr("Stop") : tr("Start")));
    m_startBtn->setAccessibleName(m_installingRule ? tr("Authorizing CTR2 USB access")
                                  : (running ? tr("Stop CTR2 proxy") : tr("Start CTR2 proxy")));
    m_startBtn->setEnabled(haveModel && !m_installingRule && (running || problem.isEmpty()));
    m_startBtn->setAccessibleDescription(
        !haveModel ? tr("Proxy unavailable")
        : m_installingRule ? frozen
        : (running ? QString() : problem));
}

void Ctr2ProxyPanel::syncStatus()
{
    if (!m_model) {
        m_stateLabel->setText(tr("State: Stopped"));
        m_endpointsLabel->clear();
        m_errorLabel->clear();
        m_errorLabel->hide();
        return;
    }
    m_stateLabel->setText(tr("State: %1").arg(m_model->stateText()));
    m_stateLabel->setAccessibleName(m_stateLabel->text());

    const bool usb = m_model->transport() == Ctr2ProxyModel::Transport::Usb;
    QStringList parts;
    if (!m_model->listenerEndpoint().isEmpty()) {
        parts << tr("Listen %1").arg(m_model->listenerEndpoint());
    }
    if (!m_model->peerEndpoint().isEmpty()) {
        parts << (usb ? tr("USB %1") : tr("CTR2 %1")).arg(m_model->peerEndpoint());
    }
    if (m_model->isRunning() && !m_model->radioEndpoint().isEmpty()) {
        parts << tr("Radio %1").arg(m_model->radioEndpoint());
    }
    m_endpointsLabel->setText(parts.join(QStringLiteral("\n")));
    m_endpointsLabel->setAccessibleName(parts.join(QStringLiteral(", ")));

    const QString err = m_model->lastError();
    m_errorLabel->setText(err.isEmpty() ? QString() : tr("Last error: %1").arg(err));
    m_errorLabel->setVisible(!err.isEmpty());
    m_errorLabel->setAccessibleName(m_errorLabel->text());
}

void Ctr2ProxyPanel::syncStats()
{
    const TcpByteProxy::Stats s = m_model ? m_model->stats() : TcpByteProxy::Stats{};
    const bool usb = m_model && m_model->transport() == Ctr2ProxyModel::Transport::Usb;
    const QLocale locale;
    const auto n = [&locale](quint64 v) { return locale.toString(v); };
    m_bytesUp->setText(n(s.toUpstream));
    m_bytesDown->setText(n(s.toDownstream));
    m_queuedUp->setText(n(static_cast<quint64>(qMax<qint64>(0, s.queuedToUpstream))));
    m_queuedDown->setText(n(static_cast<quint64>(qMax<qint64>(0, s.queuedToDownstream))));
    // The rows depend only on the mode, never on the counts.
    m_udpName->setVisible(usb);
    m_udpUp->setVisible(usb);
    m_udpDown->setVisible(usb);
    m_udpUp->setText(n(s.datagramsToRadio));
    m_udpDown->setText(n(s.datagramsToDevice));
    m_extraName->setText(usb ? tr("Dropped") : tr("Rejected"));
    m_extraValue->setText(n(usb ? s.datagramsDropped : s.rejectedClients));

    QStringList spoken{
        tr("To radio %1 bytes, %2 queued").arg(n(s.toUpstream),
                                               n(static_cast<quint64>(qMax<qint64>(0, s.queuedToUpstream)))),
        tr("To CTR2 %1 bytes, %2 queued").arg(n(s.toDownstream),
                                              n(static_cast<quint64>(qMax<qint64>(0, s.queuedToDownstream))))};
    if (usb) {
        spoken << tr("UDP: %1 to radio, %2 to CTR2, %3 dropped")
                      .arg(n(s.datagramsToRadio), n(s.datagramsToDevice), n(s.datagramsDropped));
    } else {
        spoken << tr("Rejected extra clients: %1").arg(n(s.rejectedClients));
    }
    m_traffic->setAccessibleName(spoken.join(QStringLiteral(", ")));
}

} // namespace AetherSDR
