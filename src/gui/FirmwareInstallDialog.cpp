#include "FirmwareInstallDialog.h"

#include "CanonIndicators.h"
#include "CanonMessageBox.h"
#include "ComboStyle.h"
#include "Ctr2ProxyPanel.h"
#include "ModemChrome.h"
#include "ScopedChildWidget.h"
#include "models/Ctr2ProxyModel.h"
#include "models/FirmwareInstallModel.h"

#include <QCloseEvent>
#include <QListWidget>
#include <QLocale>
#include <QTabWidget>
#include <QSignalBlocker>
#include <QFileInfo>
#include <QDate>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <unistd.h>
#endif

namespace AetherSDR {

namespace {

QLabel* section(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("SectionLabel"));
    return l;
}

QLabel* wrapped(const QString& text = {})
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    return l;
}

} // namespace

namespace {

// What a row of a firmware list stands for.
constexpr int kReleaseRole = Qt::UserRole;        // int: the release's index
constexpr int kBackupRole = Qt::UserRole + 1;     // QString: the backup's path
constexpr int kFileRole = Qt::UserRole + 2;       // QString: a kept Lynovation firmware
constexpr int kDownloadRole = Qt::UserRole + 3;   // int: a release on lynovation.com, not kept yet

// Whether getting the unit into download mode takes its buttons: unless it
// is there already, or its firmware restarts into it on request (AetherCTR2
// from 0.9.2, stock firmware offering the commands).
bool needsButtons(const FirmwareInstallModel& fw, const FirmwareInstallModel::Target& target)
{
    return target.device != FirmwareInstallModel::Device::Bootloader
           && !fw.restartsIntoDownloadMode(target);
}

// The unit to act on after the download-mode alert, which says to put it in
// download mode now: if the operator did, the one device found there.
FirmwareInstallModel::Target afterAlert(FirmwareInstallModel& fw,
                                        const FirmwareInstallModel::Target& target)
{
    fw.lookAgain();
    const FirmwareInstallModel::Target now = fw.target();
    return now.device == FirmwareInstallModel::Device::Bootloader && fw.loaderCount() == 1
               ? now
               : target;
}

// The selected unit, for a press that waits (on the access question or a
// fresh read of its offer) before acting: a change of selection drops it.
QString unitKey(const FirmwareInstallModel::Target& target)
{
    return target.hidPath + QLatin1Char('|') + target.mac.toUpper();
}

// For a run after an access grant: the unit as it is now, if it is the one
// the operator chose (same MAC, or the only one in download mode when the
// choice had none); else false with the reason.
bool retarget(FirmwareInstallModel& fw, const FirmwareInstallModel::Target& chosen,
              FirmwareInstallModel::Target* now, QString* problem)
{
    fw.lookAgain();
    const FirmwareInstallModel::Target current = fw.target();
    const bool same = chosen.mac.isEmpty()
        ? current.device == FirmwareInstallModel::Device::Bootloader && fw.loaderCount() == 1
        : current.mac.compare(chosen.mac, Qt::CaseInsensitive) == 0;
    if (!same) {
        *problem = FirmwareInstallDialog::tr("The CTR2-Max changed while access was being "
                                             "granted. Press the button again.");
        return false;
    }
    *now = current;
    return true;
}

QListWidgetItem* note(QListWidget* list, const QString& text)
{
    auto* item = new QListWidgetItem(text, list);
    item->setFlags(Qt::NoItemFlags);   // not a choice
    return item;
}

} // namespace

FirmwareInstallDialog::FirmwareInstallDialog(Ctr2ProxyModel* model, QWidget* parent)
    : CanonWindow(tr("CTR2"), parent, Kind::Workspace)
    , m_model(model)
    , m_fw(new FirmwareInstallModel(model, this))
{
    setObjectName(QStringLiteral("firmwareInstallDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setGeometryKey(QStringLiteral("Ctr2WindowGeometry"));
    resize(540, 800);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon)
               + QStringLiteral(
                   "QListWidget#firmwareInstallProduct, QListWidget#firmwareInstallFileList { background: {{color.canon.nested}};"
                   " color: {{color.canon.ink}}; border: 1px solid {{color.canon.line}};"
                   " border-radius: 6px; padding: 4px; outline: 0; }"
                   "QListWidget#firmwareInstallProduct::item, QListWidget#firmwareInstallFileList::item { padding: 5px 8px; border-radius: 4px; }"
                   "QListWidget#firmwareInstallProduct::item:hover,"
                   " QListWidget#firmwareInstallFileList::item:hover { background: {{color.canon.raised}}; }"
                   "QListWidget#firmwareInstallProduct::item:selected,"
                   " QListWidget#firmwareInstallFileList::item:selected {"
                   " background: {{color.canon.control}}; color: {{color.canon.cyan}}; }"
                   // Rows that are not a choice: a placeholder, a section's title.
                   "QListWidget#firmwareInstallProduct::item:disabled,"
                   " QListWidget#firmwareInstallFileList::item:disabled {"
                   " color: {{color.canon.muted}}; }"
                   // The status badge; showStatus() picks its tone.
                   "QLabel#firmwareInstallStatus { background: {{color.canon.nested}};"
                   " border: 1px solid {{color.canon.muted}}; color: {{color.canon.muted}};"
                   " border-radius: 11px; padding: 3px 12px; font-weight: 600; }"
                   "QLabel#firmwareInstallStatus[tone=\"cyan\"] { color: {{color.canon.cyan}};"
                   " border-color: {{color.canon.cyan}}; }"
                   "QLabel#firmwareInstallStatus[tone=\"sparkGold\"] {"
                   " color: {{color.canon.sparkGold}}; border-color: {{color.canon.sparkGold}}; }"
                   "QLabel#firmwareInstallStatus[tone=\"aqua\"] { color: {{color.canon.aqua}};"
                   " border-color: {{color.canon.aqua}}; }"
                   // The relay panel (Ctr2ProxyPanel): its combos and value tones.
                   "QComboBox { color: {{color.canon.ink}}; background: {{color.canon.control}};"
                   " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 4px 8px; }"
                   "QComboBox:hover, QComboBox:focus { border-color: {{color.canon.aqua}}; }"
                   "QComboBox:disabled { color: {{color.canon.muted}};"
                   " border-color: {{color.canon.line}}; }"
                   "QComboBox::drop-down { border: 0; width: 18px; }"
                   "QComboBox QAbstractItemView { color: {{color.canon.inkSoft}};"
                   " background: {{color.canon.nested}}; border: 1px solid {{color.canon.lineHi}};"
                   " selection-background-color: {{color.canon.cyan}};"
                   " selection-color: {{color.canon.onAccent}}; }"
                   "QLabel[tone=\"secondary\"] { color: {{color.canon.inkSoft}}; }"
                   "QLabel[tone=\"readout\"] { color: {{color.canon.ink}};"
                   " font-family: \"SF Mono\", \"Menlo\", \"Consolas\", monospace; }"
                   "QLabel[tone=\"warning\"] { color: {{color.canon.sparkGold}}; }"
                   "QLabel[tone=\"danger\"] { color: {{color.accent.danger}}; }"
                   // The firmware tabs, as SpotHub's: text tabs, the selected
                   // one underlined in cyan, over a raised card.
                   "QTabWidget#firmwareInstallTabs::pane { background: {{color.canon.raised}};"
                   " border: 1px solid {{color.canon.line}}; border-radius: 10px; top: -1px; }"
                   "QTabWidget#firmwareInstallTabs QTabBar::tab { color: {{color.canon.muted}};"
                   " background: transparent; border: none; border-bottom: 2px solid transparent;"
                   " padding: 6px 12px; font-weight: 600; }"
                   "QTabWidget#firmwareInstallTabs QTabBar::tab:hover { color: {{color.canon.ink}}; }"
                   "QTabWidget#firmwareInstallTabs QTabBar::tab:selected {"
                   " color: {{color.canon.cyan}}; border-bottom-color: {{color.canon.cyan}}; }"
                   "QTabWidget#firmwareInstallTabs QTabBar::tab:disabled {"
                   " color: {{color.canon.muted}}; }"
                   // The relay's buttons sit in rows of combo boxes: compact,
                   // so they match the rows' height.
                   "#ctr2ProxyPanel QPushButton { padding: 4px 14px; }")
               + comboArrowRule();
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    // The relay first: it is what the window is opened for day to day.
    root->addWidget(makeCanonHeader(tr("CTR2 Relay")));
    m_relay = new Ctr2ProxyPanel;
    m_relay->setModel(model);
    root->addWidget(m_relay);
    root->addSpacing(6);

    root->addWidget(makeCanonHeader(tr("CTR2-Max Firmware")));

    // One tab per firmware: AetherCTR2 from the release list, or the
    // CTR2-Max's own (Lynovation's) from a file the operator downloaded.
    // Each tab holds what applies to it.
    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("firmwareInstallTabs"));
    m_tabs->setAccessibleName(tr("Firmware to install"));
    root->addWidget(m_tabs);

    const auto rowOf = [](QBoxLayout* layout) {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        layout->addLayout(row);
        return row;
    };
    const auto button = [](QHBoxLayout* row, const QString& text, const QString& name,
                           const QString& tip) {
        auto* b = new QPushButton(text);
        b->setObjectName(name);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };

    // AetherCTR2: the releases on aethersdr.com.
    auto* aether = new QWidget;
    auto* aetherBox = new QVBoxLayout(aether);
    aetherBox->setContentsMargins(12, 12, 12, 12);
    aetherBox->setSpacing(8);
    // An inline list, not a drop-down: it stays inside the dialog's sheet.
    m_product = new QListWidget;
    m_product->setObjectName(QStringLiteral("firmwareInstallProduct"));
    m_product->setAccessibleName(tr("AetherCTR2 release to install"));
    m_product->setSelectionMode(QAbstractItemView::SingleSelection);
    m_product->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Room for a release and the unit's backups.
    m_product->setFixedHeight(m_product->fontMetrics().height() * 5 + 46);
    aetherBox->addWidget(m_product);
    m_release = wrapped();
    m_release->setObjectName(QStringLiteral("firmwareInstallRelease"));
    aetherBox->addWidget(m_release);
    QHBoxLayout* aetherActions = rowOf(aetherBox);
    m_install = button(aetherActions, tr("Install…"), QStringLiteral("firmwareInstallInstall"),
                       tr("Install AetherCTR2 beside what the CTR2-Max already has, or update "
                          "it over the USB cable."));
    aetherActions->addStretch(1);
    aetherBox->addStretch(1);
    m_tabs->addTab(aether, tr("AetherCTR2"));

    // Lynovation: the file Lynovation publishes, downloaded by the operator.
    // Laid out as the AetherCTR2 tab: the firmware, what it is, the actions.
    auto* lyn = new QWidget;
    auto* lynBox = new QVBoxLayout(lyn);
    lynBox->setContentsMargins(12, 12, 12, 12);
    lynBox->setSpacing(8);
    m_fileList = new QListWidget;
    m_fileList->setObjectName(QStringLiteral("firmwareInstallFileList"));
    m_fileList->setAccessibleName(tr("CTR2-Max firmware to install"));
    m_fileList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fileList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_fileList->setFixedHeight(m_product->maximumHeight());
    lynBox->addWidget(m_fileList);
    m_fileText = new QLabel;
    m_fileText->setObjectName(QStringLiteral("firmwareInstallFile"));
    m_fileText->setWordWrap(true);
    m_fileText->setTextFormat(Qt::RichText);
    m_fileText->setOpenExternalLinks(true);
    lynBox->addWidget(m_fileText);
    QHBoxLayout* lynActions = rowOf(lynBox);
    m_installFile = button(lynActions, tr("Install…"), QStringLiteral("firmwareInstallInstallFile"),
                           tr("Install this firmware. Only the firmware itself is written: the "
                              "CTR2-Max keeps its settings, its registration and AetherCTR2."));
    m_openFile = button(lynActions, tr("Open File…"), QStringLiteral("firmwareInstallOpenFile"),
                        tr("Open the CTR2-Max firmware you downloaded from lynovation.com (the "
                           "zip, or the .bin in it)."));
    lynActions->addStretch(1);
    lynBox->addStretch(1);
    m_tabs->addTab(lyn, tr("Lynovation"));

    root->addWidget(section(tr("Device")));
    // The device's mode as a badge of its own; details in its tooltip.
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    auto* statusLabel = new QLabel(tr("Status"));
    statusLabel->setObjectName(QStringLiteral("SectionLabel"));
    statusRow->addWidget(statusLabel);
    m_badge = new QLabel;
    m_badge->setObjectName(QStringLiteral("firmwareInstallStatus"));
    m_badge->setAccessibleName(tr("Device status"));
    m_badge->setAlignment(Qt::AlignCenter);
    m_badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    statusRow->addWidget(m_badge);
    statusRow->addStretch(1);
    root->addSpacing(4);
    root->addLayout(statusRow);
    root->addSpacing(4);
    // What the unit starts: the one it runs now is dimmed. The way there is
    // the cheapest the unit allows (see FirmwareInstallModel::restartInto).
    QHBoxLayout* restartRow = rowOf(root);
    auto* restartLabel = new QLabel(tr("Restart into"));
    restartLabel->setObjectName(QStringLiteral("SectionLabel"));
    restartRow->addWidget(restartLabel);
    m_toAetherCtr2 = button(restartRow, tr("AetherCTR2"), QStringLiteral("firmwareInstallToAetherCtr2"),
                            tr("Restart the CTR2-Max into AetherCTR2, already installed on it. "
                               "Only the boot selection is written."));
    m_toStock = button(restartRow, tr("Stock Firmware"), QStringLiteral("firmwareInstallToStock"),
                       tr("Restart the CTR2-Max into its stock firmware. AetherCTR2 stays "
                          "installed."));
    m_toBootloader = button(restartRow, tr("Bootloader"),
                            QStringLiteral("firmwareInstallToBootloader"),
                            tr("Restart the CTR2-Max into download mode."));
    restartRow->addStretch(1);

    m_step = wrapped();
    m_step->setObjectName(QStringLiteral("firmwareInstallStep"));
    m_step->setAccessibleName(tr("Install status"));
    root->addWidget(m_step);
    m_bar = new QProgressBar;
    m_bar->setObjectName(QStringLiteral("firmwareInstallProgress"));
    m_bar->setAccessibleName(tr("Install progress"));
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(8);
    m_bar->hide();
    root->addWidget(m_bar);
    root->addStretch(1);

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setObjectName(QStringLiteral("firmwareInstallCancel"));
    m_cancel->hide();
    closeRow->addWidget(m_cancel);
    auto* close = new QPushButton(tr("Close"));
    close->setObjectName(QStringLiteral("firmwareInstallClose"));
    closeRow->addWidget(close);
    root->addLayout(closeRow);

    connect(m_product, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item && item->data(kReleaseRole).isValid())
            m_fw->chooseRelease(item->data(kReleaseRole).toInt());
        syncText();
    });
    connect(m_fileList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item && item->data(kFileRole).isValid()) {
            QString problem;
            if (!m_fw->chooseStockFirmware(item->data(kFileRole).toString(), &problem))
                m_step->setText(problem);
        }
        syncText();
    });
    connect(m_fw, &FirmwareInstallModel::stockDownloaded, this, [this](const QString& path) {
        m_step->clear();
        syncLists();
        for (int i = 0; i < m_fileList->count(); ++i) {   // the release just kept
            if (m_fileList->item(i)->data(kFileRole).toString() == path)
                m_fileList->setCurrentRow(i);
        }
        // Install pressed on it: carry on with the install.
        if (std::exchange(m_installAfterDownload, false))
            installStockFirmware();
    });
    connect(m_fw, &FirmwareInstallModel::stockDownloadFailed, this, [this](const QString& message) {
        m_installAfterDownload = false;
        m_step->setText(message);
        syncText();
    });
    connect(m_openFile, &QPushButton::clicked, this, &FirmwareInstallDialog::onOpenFile);
    connect(m_installFile, &QPushButton::clicked, this, &FirmwareInstallDialog::installStockFirmware);
    connect(m_install, &QPushButton::clicked, this, &FirmwareInstallDialog::onInstall);
    connect(m_toAetherCtr2, &QPushButton::clicked, this,
            [this] { restartInto(int(FirmwareInstallModel::Firmware::AetherCtr2)); });
    connect(m_toStock, &QPushButton::clicked, this,
            [this] { restartInto(int(FirmwareInstallModel::Firmware::Stock)); });
    connect(m_toBootloader, &QPushButton::clicked, this,
            [this] { restartInto(int(FirmwareInstallModel::Firmware::Bootloader)); });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        // Before cancel(): a wait ends at once, and says so.
        m_actions.invalidate();   // no access retry resumes what was stopped
        m_step->setText(tr("Stopping…"));
        m_fw->cancel();
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(m_fw, &FirmwareInstallModel::releasesChanged, this, &FirmwareInstallDialog::syncLists);
    connect(m_fw, &FirmwareInstallModel::releaseTextChanged, this, &FirmwareInstallDialog::syncText);
    connect(m_fw, &FirmwareInstallModel::deviceChanged, this, &FirmwareInstallDialog::syncDevice);
    connect(m_fw, &FirmwareInstallModel::busyChanged, this, &FirmwareInstallDialog::sync);
    connect(m_fw, &FirmwareInstallModel::connected, this,
            [this](const QString& chip, const QByteArray& mac, quint32 flashSize) {
                showStatus(tr("Bootloader"), "aqua",
                           tr("Connected: %1, %2 MB flash, %3")
                               .arg(chip)
                               .arg(flashSize >> 20)
                               .arg(QString::fromLatin1(mac.toHex(':')).toUpper()));
            });
    connect(m_fw, &FirmwareInstallModel::stepChanged, m_step, &QLabel::setText);
    connect(m_fw, &FirmwareInstallModel::progress, this,
            [this](const QString& step, qint64 done, qint64 total) {
                if (!step.isEmpty())
                    m_step->setText(step);
                m_bar->setValue(total > 0 ? int(done * 100 / total) : 0);
            });
    connect(m_fw, &FirmwareInstallModel::finished, this, &FirmwareInstallDialog::onFinished);
    // The relay's version readout decides whether a link update is offered.
    if (m_model)
        connect(m_model, &Ctr2ProxyModel::firmwareVersionChanged, this,
                &FirmwareInstallDialog::syncDevice);

    // Every button at its full height: squeezed, the sheet's fixed padding
    // clips its label. The window's minimum height follows, so it opens (and
    // restores) tall enough for all of it.
    for (QPushButton* b : findChildren<QPushButton*>())
        b->setMinimumHeight(b->sizeHint().height());
    resize(width(), qMax(height(), sizeHint().height()));

    syncDevice();   // and the lists
    m_fw->begin();
}

FirmwareInstallDialog::~FirmwareInstallDialog() = default;

void FirmwareInstallDialog::closeEvent(QCloseEvent* event)
{
    if (m_fw->busy()) {
        m_step->setText(tr("Wait for this step to finish, or press Cancel, before closing."));
        event->ignore();
        return;
    }
    CanonWindow::closeEvent(event);
}

void FirmwareInstallDialog::reject()
{
    if (m_fw->busy()) {
        m_step->setText(tr("Wait for this step to finish, or press Cancel, before closing."));
        return;
    }
    CanonWindow::reject();
}

QString FirmwareInstallDialog::selectedBackup(const QListWidget* list) const
{
    const QListWidgetItem* item = list->currentItem();
    return item ? item->data(kBackupRole).toString() : QString();
}

QString FirmwareInstallDialog::backupText(const QString& path) const
{
    for (const FirmwareInstallModel::Backup& b : m_fw->backups()) {
        if (b.path == path)
            return tr("Restore: puts back this CTR2-Max's whole flash as it was on %1 (both "
                      "firmwares, its settings and its registration). Anything since is lost.")
                .arg(QLocale().toString(b.made, QLocale::ShortFormat));
    }
    return {};
}

void FirmwareInstallDialog::syncLists()
{
    // Each list: its firmware, then the connected unit's backups, which
    // install as a restore. The choice made is kept across a refill.
    const QList<FirmwareInstallModel::Backup> backups = m_fw->backups();
    const bool severalUnits = m_fw->deviceMac().isEmpty();
    const auto addBackups = [&](QListWidget* list) {
        if (backups.isEmpty())
            return;
        note(list, severalUnits ? tr("Backups") : tr("Backups of this CTR2-Max"));
        for (const FirmwareInstallModel::Backup& b : backups) {
            QString text = tr("Backup %1").arg(QLocale().toString(b.made, QLocale::ShortFormat));
            if (severalUnits)
                text += QStringLiteral("  (%1)").arg(b.mac);
            auto* item = new QListWidgetItem(text, list);
            item->setData(kBackupRole, b.path);
        }
    };
    const auto keep = [](QListWidget* list) {
        const QListWidgetItem* item = list->currentItem();
        return item ? std::pair<QVariant, QVariant>(item->data(kReleaseRole),
                                                    item->data(kBackupRole))
                    : std::pair<QVariant, QVariant>();
    };
    const auto restore = [](QListWidget* list, const std::pair<QVariant, QVariant>& was) {
        for (int i = 0; i < list->count(); ++i) {
            const QListWidgetItem* item = list->item(i);
            if ((was.first.isValid() && item->data(kReleaseRole) == was.first)
                || (was.second.isValid() && item->data(kBackupRole) == was.second)) {
                list->setCurrentRow(i);
                return;
            }
        }
    };

    {
        const auto was = keep(m_product);
        const QSignalBlocker block(m_product);
        m_product->clear();
        const QStringList releases = m_fw->releases();
        if (releases.isEmpty()) {
            note(m_product, m_fw->catalogProblem().isEmpty() ? tr("Looking for firmware…")
                                                             : m_fw->catalogProblem());
        }
        for (int i = 0; i < releases.size(); ++i)
            (new QListWidgetItem(releases[i], m_product))->setData(kReleaseRole, i);
        addBackups(m_product);
        restore(m_product, was);
    }
    {
        // The Lynovation firmware kept from the files opened here, newest
        // first: an earlier one installs as a downgrade.
        const auto was = keep(m_fileList);
        const QSignalBlocker block(m_fileList);
        m_fileList->clear();
        // With Lynovation's own list of releases (lynovation.com), merged
        // by version: one not kept yet downloads when chosen.
        const QList<FirmwareInstallModel::StoredFirmware> kept = m_fw->storedFirmware();
        const QList<Ctr2MaxStockCatalog::Release> published = m_fw->stockReleases();
        struct Row {
            QString version, text;
            QVariant file, download;
        };
        QList<Row> rows;
        for (const FirmwareInstallModel::StoredFirmware& f : kept)
            rows.append({f.version, f.name, f.path, {}});
        for (int i = 0; i < published.size(); ++i) {
            const bool have = std::any_of(kept.cbegin(), kept.cend(), [&](const auto& f) {
                return f.version == published[i].version;
            });
            if (!have)
                rows.append({published[i].version,
                             tr("CTR2-Max firmware %1  (lynovation.com)").arg(published[i].version),
                             {}, i});
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return Ctr2MaxStockFirmware::newer(a.version, b.version);
        });
        if (rows.isEmpty())
            note(m_fileList, tr("No firmware file opened"));
        for (const Row& r : std::as_const(rows)) {
            auto* item = new QListWidgetItem(r.text, m_fileList);
            item->setData(kFileRole, r.file);
            item->setData(kDownloadRole, r.download);
            if (!was.second.isValid() && r.file.isValid()
                && r.file.toString() == m_fw->stockFirmwarePath())
                m_fileList->setCurrentItem(item);
        }
        addBackups(m_fileList);
        restore(m_fileList, was);
    }
    syncText();
}

void FirmwareInstallDialog::syncText()
{
    const QString aetherBackup = selectedBackup(m_product);
    if (!aetherBackup.isEmpty())
        m_release->setText(backupText(aetherBackup));
    else if (m_product->currentItem())
        m_release->setText(m_fw->releaseText());
    else
        m_release->clear();

    const QString fileBackup = selectedBackup(m_fileList);
    const QListWidgetItem* fileItem = m_fileList->currentItem();
    if (fileItem && fileItem->data(kDownloadRole).isValid()) {
        m_fileText->setText(
            (m_fw->downloadingStockRelease()
                 ? tr("Downloading %1 from lynovation.com…")
                 : tr("%1 is on lynovation.com: Install downloads it from there first, and keeps "
                      "it."))
                .arg(fileItem->text().section(QStringLiteral("  ("), 0, 0))
                .toHtmlEscaped());
    } else if (!fileBackup.isEmpty()) {
        m_fileText->setText(backupText(fileBackup).toHtmlEscaped());
    } else if (fileItem && m_fw->haveStockFirmware()) {
        m_fileText->setText(tr("Ready: %1. Install writes only the firmware itself: the CTR2-Max "
                               "keeps its settings, its registration and AetherCTR2.")
                                .arg(m_fw->stockFirmwareName())
                                .toHtmlEscaped());
    } else if (!m_fw->storedFirmware().isEmpty()) {
        m_fileText->setText(tr("Choose the firmware to install, or open a newer one from <a "
                               "href=\"%1\">lynovation.com</a> with Open File…")
                                .arg(QStringLiteral(
                                    "https://ctr2.lynovation.com/ctr2-max-firmware/")));
    } else {
        m_fileText->setText(
            tr("Download the CTR2-Max firmware from <a href=\"%1\">lynovation.com</a>, then "
               "open it here with Open File…")
                .arg(QStringLiteral("https://ctr2.lynovation.com/ctr2-max-firmware/")));
    }
    sync();
}

void FirmwareInstallDialog::showStatus(const QString& state, const char* colorToken,
                                       const QString& detail)
{
    // The tone selects a rule in the dialog's sheet (cyan, sparkGold, aqua,
    // or the muted default).
    const QString tone = QString::fromLatin1(colorToken);
    if (m_badge->property("tone").toString() != tone) {
        m_badge->setProperty("tone", tone);
        m_badge->style()->unpolish(m_badge);
        m_badge->style()->polish(m_badge);
    }
    m_badge->setText(state);
    m_badge->setToolTip(detail);
    m_badge->setAccessibleDescription(detail);
}

void FirmwareInstallDialog::syncDevice()
{
    const QString mac = m_fw->deviceMac();
    switch (m_fw->device()) {
    case FirmwareInstallModel::Device::Bootloader: {
        QString detail = tr("%1. Its screen stays dark until it restarts; that is normal.").arg(mac);
        if (m_fw->loaderCount() > 1)
            detail += QLatin1Char(' ')
                      + tr("%1 devices are in download mode; connect only the one to install.")
                            .arg(m_fw->loaderCount());
        showStatus(tr("Bootloader"), "aqua", detail);
        break;
    }
    case FirmwareInstallModel::Device::LinkFirmware: {
        const QString version = m_fw->linkVersion();
        QString detail = mac.isEmpty() ? QString() : mac + QStringLiteral(". ");
        detail += version.isEmpty() ? tr("Running AetherCTR2.")
                                    : tr("Running %1; an AetherCTR2 update goes over the USB "
                                         "cable.")
                                          .arg(version);
        showStatus(m_fw->deviceName(), "cyan", detail);
        break;
    }
    case FirmwareInstallModel::Device::Stock:
        showStatus(tr("Stock firmware"), "sparkGold",
                   (mac.isEmpty() ? QString() : mac + QStringLiteral(". "))
                       + tr("Runs its stock firmware."));
        break;
    case FirmwareInstallModel::Device::None:
        showStatus(tr("No CTR2-Max"), "muted", tr("Connect the CTR2-Max with a USB cable."));
        break;
    }
    syncLists();   // the unit's backups
}

namespace {

// Enables a button, and when it is not, says why to assistive technology.
void offer(QPushButton* b, bool enabled, const QString& why)
{
    b->setEnabled(enabled);
    b->setAccessibleDescription(enabled ? QString() : why);
}

} // namespace

void FirmwareInstallDialog::sync()
{
    using Device = FirmwareInstallModel::Device;
    const bool running = m_fw->busy();
    const Device device = m_fw->device();
    const QString busy = tr("Unavailable while the current step runs.");
    const QString noDevice = tr("No CTR2-Max is connected.");
    // Every action runs in download mode; the window gets the unit there
    // (or tells the operator how) when it is not.
    const bool any = device != Device::None;
    const bool aetherBackup = !selectedBackup(m_product).isEmpty();
    const bool release = m_fw->haveRelease() && !aetherBackup && m_product->currentItem();
    offer(m_install, !running && any && (release || aetherBackup),
          running ? busy : !any ? noDevice : tr("Choose a release or a backup first."));
    const bool fileBackup = !selectedBackup(m_fileList).isEmpty();
    // The kept firmware chosen (not an earlier choice still loaded), a
    // release to download first, or a backup; not while one downloads.
    const QListWidgetItem* fileItem = m_fileList->currentItem();
    const bool downloading = m_fw->downloadingStockRelease();
    const bool file = !downloading
                      && ((fileItem && fileItem->data(kFileRole).isValid()
                           && fileItem->data(kFileRole).toString() == m_fw->stockFirmwarePath()
                           && m_fw->haveStockFirmware())
                          || (fileItem && fileItem->data(kDownloadRole).isValid()) || fileBackup);
    offer(m_installFile, !running && file && any,
          running ? busy : !file ? tr("Open the firmware file first.") : noDevice);
    // The firmware it runs now is dimmed.
    const QString current = tr("The CTR2-Max runs it now.");
    offer(m_toAetherCtr2, !running && any && device != Device::LinkFirmware,
          running ? busy : !any ? noDevice : current);
    offer(m_toStock, !running && any && device != Device::Stock,
          running ? busy : !any ? noDevice : current);
    offer(m_toBootloader, !running && any && device != Device::Bootloader,
          running ? busy : !any ? noDevice : current);
    m_product->setEnabled(!running);
    m_fileList->setEnabled(!running);
    offer(m_openFile, !running, busy);
    m_tabs->tabBar()->setEnabled(!running);
    m_cancel->setVisible(running);
    m_bar->setVisible(running && !m_fw->waitingForDownloadMode());
}

bool FirmwareInstallDialog::offerAccessFirst(std::function<void()> again)
{
#ifdef Q_OS_LINUX
    using Device = FirmwareInstallModel::Device;
    const FirmwareInstallModel::Target target = m_fw->target();
    if (!m_relay || (target.device != Device::LinkFirmware && target.device != Device::Stock)
        || target.hidPath.isEmpty())
        return false;
    const QByteArray path = target.hidPath.toLocal8Bit();
    if (::access(path.constData(), R_OK | W_OK) == 0 || errno != EACCES)
        return false;
    // A ticket for this press: another action or Cancel meanwhile drops it.
    const Ctr2ActionRetry::Ticket ticket = m_actions.mark(unitKey(target));
    // Declined, or no install could start: the action goes ahead as before.
    return m_relay->offerAccessRule(Ctr2ProxyPanel::RuleUse::Firmware, [this, ticket, again] {
        // What the action can do (restart on request, or the buttons) depends
        // on the firmware's version and commands, which the relay reads once
        // it can open the device: rescan, and wait for them (up to ~5 s).
        if (m_model)
            m_model->refreshDevices();
        waitForFirmwareInfo(20, ticket, again);
    });
#else
    Q_UNUSED(again);
    return false;
#endif
}

bool FirmwareInstallDialog::refreshOfferFirst(std::function<void()> again)
{
    using Device = FirmwareInstallModel::Device;
    if (std::exchange(m_offerRefreshed, false))
        return false;   // the repeat after the read
    const FirmwareInstallModel::Target target = m_fw->target();
    if (!m_model || m_model->isRunning()
        || (target.device != Device::LinkFirmware && target.device != Device::Stock)
        || m_fw->takesCommands(target))
        return false;
    const Ctr2ActionRetry::Ticket ticket = m_actions.mark(unitKey(target));
    m_step->setText(tr("Reading the CTR2-Max's firmware…"));
    m_model->reprobeFirmware();
    waitForFirmwareInfo(8, ticket, [this, again] {
        m_step->clear();
        m_offerRefreshed = true;
        again();
    });
    return true;
}

void FirmwareInstallDialog::waitForFirmwareInfo(int tries, Ctr2ActionRetry::Ticket ticket,
                                                std::function<void()> again)
{
    // Another action, Cancel, or another unit selected meanwhile: drop it.
    if (!m_actions.current(ticket, unitKey(m_fw->target()))) {
        m_step->clear();
        return;
    }
    // Known once the relay has read the device: a version (AetherCTR2), or
    // the restart commands (stock firmware that offers them). Stock firmware
    // without them stays unknown, and gets the buttons after the wait.
    if (tries > 0 && m_fw->linkVersion().isEmpty() && !m_fw->takesCommands(m_fw->target())) {
        QTimer::singleShot(250, this, [this, tries, ticket, again] {
            waitForFirmwareInfo(tries - 1, ticket, again);
        });
        return;
    }
    again();
}


void FirmwareInstallDialog::runJob(const QString& doneText, Ctr2ActionRetry::Job job,
                                   bool retarget, bool retryUsed)
{
    m_actions.started(job, doneText, retryUsed);
    m_offerRefreshed = false;
    m_jobDone = doneText;
    QString problem;
    const bool ok = job(retarget, &problem);
    started(ok, problem, doneText);
}

void FirmwareInstallDialog::started(bool ok, const QString& problem, const QString& doneText)
{
    if (!ok) {
        m_jobDone.clear();
        m_step->setText(problem);
        sync();
        return;
    }
    // An action that finished at once (a device that could not be opened)
    // has already reported through onFinished().
    if (m_fw->busy()) {
        m_jobDone = doneText;
        m_bar->setValue(0);
    }
    sync();
}

bool FirmwareInstallDialog::askDownloadMode(bool buttons)
{
    // Only a unit whose firmware cannot restart into download mode on
    // request needs its buttons (needsButtons()).
    if (!buttons) {
        return true;
    }
    ScopedChildWidget<CanonMessageBox> ask(tr("Put the CTR2-Max in download mode"),
                                           tr("Put the CTR2-Max in download mode now."), this);
    ask.get()->setInformativeText(
        tr("Hold BOOT (through the hole in the back), press and release RESET, then release "
           "BOOT. Its screen goes dark; that is normal. AetherSDR carries on by itself once it "
           "finds it there."));
    ask.get()->addButton(tr("Cancel"), CanonMessageBox::Role::Reject);
    QPushButton* ok = ask.get()->addButton(tr("OK"), CanonMessageBox::Role::Accept);
    ask.get()->setDefaultButton(ok);
    ask.get()->exec();
    return ask && ask.get()->clickedButton() == ok;
}

void FirmwareInstallDialog::confirmRestore(const QString& path)
{
    FirmwareInstallModel::Target target = m_fw->target();
    ScopedChildWidget<CanonMessageBox> ask(tr("Restore the backup?"), backupText(path), this);
    ask.get()->setInformativeText(
        tr("A backup goes back only onto the CTR2-Max it came from, and only if the file is "
           "unchanged since it was made."));
    // Return keeps what is on the device; restoring takes a click.
    ask.get()->setDefaultButton(ask.get()->addButton(tr("Cancel"), CanonMessageBox::Role::Reject));
    QPushButton* go = ask.get()->addButton(tr("Restore"), CanonMessageBox::Role::Destructive);
    ask.get()->exec();
    if (!ask || ask.get()->clickedButton() != go || !askDownloadMode(needsButtons(*m_fw, target)))
        return;
    target = afterAlert(*m_fw, target);
    const QString done = tr("The backup is restored and the CTR2-Max is restarting.");
    m_step->setText(tr("Connecting…"));
    runJob(done, [this, target, path](bool again, QString* problem) {
        FirmwareInstallModel::Target t = target;
        return (!again || retarget(*m_fw, target, &t, problem)) && m_fw->restoreBackup(t, path, problem);
    });
}

void FirmwareInstallDialog::onInstall()
{
    if (offerAccessFirst([this] { onInstall(); }))
        return;
    if (refreshOfferFirst([this] { onInstall(); }))
        return;
    // The device is the one shown now; the model refuses if it changes
    // while the questions below are open.
    FirmwareInstallModel::Target target = m_fw->target();
    const QString backup = selectedBackup(m_product);
    if (!backup.isEmpty()) {
        confirmRestore(backup);
        return;
    }
    if (target.device == FirmwareInstallModel::Device::LinkFirmware
        && !m_fw->linkVersion().isEmpty()) {
        installOverLink();
        return;
    }
    const QString product = m_fw->releaseProduct();
    const QString version = m_fw->releaseVersion();
    const QString folder = QDir::toNativeSeparators(FirmwareInstallModel::backupFolder());
    ScopedChildWidget<CanonMessageBox> ask(tr("Install %1?").arg(product),
                                           tr("Install %1 %2 on this CTR2-Max?").arg(product, version),
                                           this);
    ask.get()->setInformativeText(
        tr("Its whole flash is backed up first, to %1; that takes about a minute. The install "
           "writes only what the firmware's manifest lists: the stock firmware, its settings and "
           "its registration are kept.")
            .arg(folder));
    ask.get()->addButton(tr("Cancel"), CanonMessageBox::Role::Reject);
    QPushButton* go = ask.get()->addButton(tr("Install"), CanonMessageBox::Role::Accept);
    ask.get()->setDefaultButton(go);
    ask.get()->exec();
    if (!ask || ask.get()->clickedButton() != go || !askDownloadMode(needsButtons(*m_fw, target)))
        return;
    target = afterAlert(*m_fw, target);
    const QString done =
        tr("%1 %2 is installed and starting. The backup is in %3.").arg(product, version, folder);
    m_step->setText(tr("Connecting…"));
    runJob(done, [this, target](bool again, QString* problem) {
        FirmwareInstallModel::Target t = target;
        return (!again || retarget(*m_fw, target, &t, problem)) && m_fw->installRelease(t, problem);
    });
}

void FirmwareInstallDialog::installOverLink()
{
    const FirmwareInstallModel::Target target = m_fw->target();
    QString problem;
    if (!m_fw->canInstallOverLink(&problem)) {
        m_step->setText(problem);
        return;
    }
    const QString product = m_fw->releaseProduct();
    const QString version = m_fw->releaseVersion();
    ScopedChildWidget<CanonMessageBox> ask(
        tr("Install %1?").arg(product),
        tr("Install %1 %2 over the USB cable? The device runs %3 now.")
            .arg(product, version, m_fw->linkVersion()),
        this);
    ask.get()->setInformativeText(
        tr("It takes about three minutes. The device checks the firmware's signature, and keeps "
           "the firmware it has if anything goes wrong."));
    ask.get()->addButton(tr("Cancel"), CanonMessageBox::Role::Reject);
    QPushButton* go = ask.get()->addButton(tr("Install"), CanonMessageBox::Role::Accept);
    ask.get()->setDefaultButton(go);
    ask.get()->exec();
    if (!ask || ask.get()->clickedButton() != go)
        return;
    const QString done = tr("%1 %2 is installed and running.").arg(product, version);
    m_step->setText(tr("Starting the update…"));
    runJob(done, [this, target](bool, QString* why) { return m_fw->installOverLink(target, why); });
}

void FirmwareInstallDialog::onOpenFile()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open the CTR2-Max firmware"), dir,
        tr("CTR2-Max firmware (*.zip *.bin);;All files (*)"));
    if (path.isEmpty())
        return;
    QString problem;
    if (!m_fw->openStockFirmware(path, &problem)) {
        m_step->setText(tr("%1: %2").arg(QFileInfo(path).fileName(), problem));
        return;
    }
    m_step->clear();
    syncLists();
    for (int i = 0; i < m_fileList->count(); ++i) {   // the file just opened
        if (m_fileList->item(i)->data(kFileRole).toString() == m_fw->stockFirmwarePath())
            m_fileList->setCurrentRow(i);
    }
}

void FirmwareInstallDialog::installStockFirmware()
{
    if (offerAccessFirst([this] { installStockFirmware(); }))
        return;
    if (refreshOfferFirst([this] { installStockFirmware(); }))
        return;
    using Device = FirmwareInstallModel::Device;
    FirmwareInstallModel::Target target = m_fw->target();
    const QString backup = selectedBackup(m_fileList);
    if (!backup.isEmpty()) {
        confirmRestore(backup);
        return;
    }
    // A release on lynovation.com: download it, then carry on from here.
    const QListWidgetItem* item = m_fileList->currentItem();
    if (item && item->data(kDownloadRole).isValid()) {
        m_installAfterDownload = true;
        m_fw->downloadStockRelease(item->data(kDownloadRole).toInt());
        syncText();
        return;
    }
    const QString name = m_fw->stockFirmwareName();
    const QString folder = QDir::toNativeSeparators(FirmwareInstallModel::backupFolder());
    ScopedChildWidget<CanonMessageBox> ask(tr("Update the CTR2-Max?"),
                                           tr("Install %1 on this CTR2-Max?").arg(name), this);
    QString info = tr("Only the firmware itself is written: the CTR2-Max keeps its settings, its "
                      "registration and AetherCTR2. Its whole flash is backed up first, to %1; "
                      "that takes about a minute.")
                       .arg(folder);
    if (target.device == Device::LinkFirmware)
        info += QStringLiteral("\n\n")
                + tr("AetherCTR2 restarts into download mode for the update; the relay stops "
                     "while it runs.");
    ask.get()->setInformativeText(info);
    ask.get()->addButton(tr("Cancel"), CanonMessageBox::Role::Reject);
    QPushButton* go = ask.get()->addButton(tr("Update"), CanonMessageBox::Role::Accept);
    ask.get()->setDefaultButton(go);
    ask.get()->exec();
    if (!ask || ask.get()->clickedButton() != go || !askDownloadMode(needsButtons(*m_fw, target)))
        return;
    target = afterAlert(*m_fw, target);
    const QString done =
        tr("%1 is installed and starting. The backup is in %2.").arg(name, folder);
    m_step->setText(tr("Connecting…"));
    runJob(done, [this, target](bool again, QString* problem) {
        FirmwareInstallModel::Target t = target;
        return (!again || retarget(*m_fw, target, &t, problem)) && m_fw->installStockFirmware(t, problem);
    });
}

void FirmwareInstallDialog::restartInto(int which)
{
    using Firmware = FirmwareInstallModel::Firmware;
    const auto firmware = static_cast<Firmware>(which);
    if (offerAccessFirst([this, which] { restartInto(which); }))
        return;
    if (refreshOfferFirst([this, which] { restartInto(which); }))
        return;
    FirmwareInstallModel::Target target = m_fw->target();
    // Firmware that cannot restart into download mode takes the unit's buttons: the
    // alert is all there is to it, and Status follows when it gets there.
    if (needsButtons(*m_fw, target) && firmware == Firmware::Bootloader) {
        askDownloadMode(true);
        m_step->clear();
        return;
    }
    // The other firmware: SWITCH where the firmware takes the commands,
    // else a start from download mode.
    if (firmware != Firmware::Bootloader
        && !askDownloadMode(target.device != FirmwareInstallModel::Device::Bootloader
                            && !m_fw->takesCommands(target))) {
        return;
    }
    target = afterAlert(*m_fw, target);
    const QString done = firmware == Firmware::AetherCtr2
                             ? tr("The CTR2-Max is restarting into AetherCTR2.")
                         : firmware == Firmware::Stock
                             ? tr("The CTR2-Max is restarting into its stock firmware.")
                             : tr("The CTR2-Max is restarting into download mode.");
    m_step->setText(tr("Asking the CTR2-Max to restart…"));
    runJob(done, [this, target, firmware](bool again, QString* problem) {
        FirmwareInstallModel::Target t = target;
        return (!again || retarget(*m_fw, target, &t, problem))
               && m_fw->restartInto(t, firmware, problem);
    });
}

void FirmwareInstallDialog::onFinished(bool ok, const QString& message)
{
    QString text = ok ? m_jobDone : message;
    // Done, but the device did not restart by itself: say so, with what to do.
    if (ok && !message.isEmpty())
        text += QLatin1Char(' ')
                + tr("It did not restart by itself (%1): press RESET on the CTR2-Max.").arg(message);
    m_jobDone.clear();
    m_step->setText(text);
    syncLists();   // a new backup, or one fewer
#ifdef Q_OS_LINUX
    // The download-mode port is closed to this login: offer the access rule,
    // then run the same action again. Queued, so the question does not open
    // inside the job's own finished signal.
    if (!ok && m_fw->lastFailureAccessDenied() && m_actions.hasJob() && m_relay) {
        // The action that failed, and only while it is still the operator's
        // latest: another action or Cancel in the meantime drops the retry.
        const Ctr2ActionRetry::Ticket ticket = m_actions.ticket();
        const auto again = [this, ticket] {
            if (!m_actions.current(ticket) || m_fw->busy())
                return;
            m_step->setText(tr("Access granted. Trying again…"));
            runJob(ticket.doneText, ticket.job, true);
        };
        // With the rule in place, a port that has only just appeared may not
        // have its ACL yet: try once more before saying anything.
        if (Ctr2ProxyPanel::accessRuleInstalled() && !m_actions.retried()) {
            QTimer::singleShot(1000, this, [this, ticket] {
                if (!m_actions.current(ticket) || m_fw->busy())
                    return;
                runJob(ticket.doneText, ticket.job, true, true);
            });
            return;
        }
        QTimer::singleShot(0, this, [this, again] {
            m_relay->offerAccessRule(Ctr2ProxyPanel::RuleUse::DownloadMode, again);
        });
    }
#endif
}

} // namespace AetherSDR
