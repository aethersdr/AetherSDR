#include "AetherTxSettingsDialog.h"
#include "CanonIndicators.h"
#include "ModemChrome.h"
#include "core/AetherTxProfiles.h"
#include "core/AetherTxStockProfiles.h"
#include "core/ThemeManager.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr const char* kFileFilter =
    "AetherTX profile (*.json);;All files (*)";
// Each list item carries the profile's real name (its text may add a suffix)
// and whether it is a stock preset.
constexpr int kNameRole  = Qt::UserRole;
constexpr int kStockRole = Qt::UserRole + 1;
}

AetherTxSettingsDialog::AetherTxSettingsDialog(AudioEngine* audio, QWidget* parent,
                                               StockTxFilterHandler txFilterHandler)
    : CanonWindow(tr("AetherTX Settings"), parent)
    , m_profiles(new AetherTxProfiles(audio, this))
    , m_txFilterHandler(std::move(txFilterHandler))
{
    setObjectName(QStringLiteral("aetherTxSettingsDialog"));
    // The canon header adds about 40 px to the old 460x380.
    resize(460, 480);
    applyCanonSheet(bodyWidget(), [] {
        return ModemChrome::styleSheet(ModemChrome::Scale::Dialog, ModemChrome::Look::Canon);
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(8);
    root->addWidget(makeCanonHeader(tr("AetherTX Settings")));

    auto* heading = new QLabel(tr("Profiles"));
    heading->setObjectName(QStringLiteral("SectionLabel"));
    root->addWidget(heading);

    auto* blurb = new QLabel(tr(
        "A profile is the transmit chain: every stage's settings, which of "
        "them are switched on, and the order they run in. Loading one leaves "
        "the receive side alone. The stock presets at the top are read-only "
        "and also set the radio's TX filter; save one under a new name to "
        "change it."));
    blurb->setWordWrap(true);
    root->addWidget(blurb);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("aetherTxProfileList"));
    m_list->setAccessibleName(tr("AetherTX profiles: stock presets, then yours"));
    root->addWidget(m_list, 1);

    // Library actions, then the two that cross the filesystem.
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    const auto addBtn = [&](const QString& text, const QString& objName,
                            const QString& tip, void (AetherTxSettingsDialog::*slot)()) {
        auto* b = new QPushButton(text);
        b->setObjectName(objName);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, slot);
        row->addWidget(b);
        return b;
    };
    m_saveBtn = addBtn(tr("Save…"), QStringLiteral("aetherTxProfileSave"),
                       tr("Store the transmit chain as it is now under a name."),
                       &AetherTxSettingsDialog::onSave);
    m_loadBtn = addBtn(tr("Load"), QStringLiteral("aetherTxProfileLoad"),
                       tr("Apply the selected profile to the transmit chain."),
                       &AetherTxSettingsDialog::onLoad);
    m_deleteBtn = addBtn(tr("Delete"), QStringLiteral("aetherTxProfileDelete"),
                         tr("Remove the selected profile from the library."),
                         &AetherTxSettingsDialog::onDelete);
    root->addLayout(row);

    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(6);
    row = fileRow;
    m_importBtn = addBtn(tr("Import…"), QStringLiteral("aetherTxProfileImport"),
                         tr("Read a profile from a JSON file and save it under "
                            "a name you choose."),
                         &AetherTxSettingsDialog::onImport);
    m_exportBtn = addBtn(tr("Export…"), QStringLiteral("aetherTxProfileExport"),
                         tr("Write the selected profile to a JSON file."),
                         &AetherTxSettingsDialog::onExport);
    fileRow->addStretch(1);
    root->addLayout(fileRow);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setObjectName(QStringLiteral("aetherTxSettingsClose"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    connect(m_list, &QListWidget::itemSelectionChanged,
            this, &AetherTxSettingsDialog::updateButtonStates);
    connect(m_list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onLoad(); });
    connect(m_profiles, &AetherTxProfiles::profilesChanged,
            this, &AetherTxSettingsDialog::refreshList);

    refreshList();
}

void AetherTxSettingsDialog::refreshList()
{
    const QString keep = selectedName();
    const bool keepStock = selectedIsStock();
    m_list->clear();
    for (const QString& name : AetherTxStockProfiles::names()) {
        auto* item = new QListWidgetItem(tr("%1 (stock)").arg(name), m_list);
        item->setData(kNameRole, name);
        item->setData(kStockRole, true);
        QString tip = AetherTxStockProfiles::summary(name);
        if (const auto f = AetherTxStockProfiles::txFilter(name)) {
            tip += QLatin1Char('\n') + tr("Also sets the radio TX filter to %1–%2 Hz.")
                                           .arg(f->lowHz).arg(f->highHz);
        }
        item->setToolTip(tip + QLatin1Char('\n') + tr("Read-only: save it under a new name to change it."));
    }
    for (const QString& name : m_profiles->profileNames()) {
        // Saved under a name before it became a stock preset's: still the
        // operator's, and labelled so the two can be told apart.
        const QString text = AetherTxProfiles::isReservedName(name) ? tr("%1 (yours)").arg(name) : name;
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(kNameRole, name);
        item->setData(kStockRole, false);
    }
    if (!keep.isEmpty()) {
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* item = m_list->item(i);
            if (item->data(kNameRole).toString() == keep && item->data(kStockRole).toBool() == keepStock) {
                m_list->setCurrentItem(item);
                break;
            }
        }
    }
    if (m_profiles->profileNames().isEmpty()) {
        m_status->setText(tr("No profiles yet — Save… stores the chain you "
                             "have set up now."));
    }
    updateButtonStates();
}

QString AetherTxSettingsDialog::selectedName() const
{
    auto* item = m_list->currentItem();
    return item ? item->data(kNameRole).toString() : QString();
}

bool AetherTxSettingsDialog::selectedIsStock() const
{
    auto* item = m_list->currentItem();
    return item && item->data(kStockRole).toBool();
}

void AetherTxSettingsDialog::updateButtonStates()
{
    const bool has = !selectedName().isEmpty();
    m_loadBtn->setEnabled(has);
    m_deleteBtn->setEnabled(has && !selectedIsStock());
    m_exportBtn->setEnabled(has);
}

void AetherTxSettingsDialog::onSave()
{
    bool ok = false;
    const QString suggested = selectedIsStock()
        ? m_profiles->uniqueName(tr("My %1").arg(selectedName()))
        : selectedName();
    const QString name = QInputDialog::getText(
        this, tr("Save profile"), tr("Name for this transmit chain:"),
        QLineEdit::Normal, suggested, &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    if (AetherTxProfiles::isReservedName(name)) {
        m_status->setText(tr("“%1” is a stock preset's name; save it under another name.").arg(name));
        return;
    }
    if (m_profiles->hasProfile(name)) {
        const auto answer = QMessageBox::question(
            this, tr("Replace profile?"),
            tr("“%1” already exists. Replace it?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    if (!m_profiles->saveFromCurrent(name)) {
        m_status->setText(tr("Could not save “%1”.").arg(name));
        return;
    }
    m_status->setText(tr("Saved “%1”.").arg(name));
}

void AetherTxSettingsDialog::onLoad()
{
    const QString name = selectedName();
    if (name.isEmpty()) return;
    if (selectedIsStock()) {
        if (!m_profiles->loadStockProfile(name)) {
            m_status->setText(tr("Could not load “%1”.").arg(name));
            return;
        }
        QString note;
        if (const auto f = AetherTxStockProfiles::txFilter(name)) {
            if (!m_txFilterHandler) {
                note = tr("Set the radio TX filter to %1–%2 Hz to match.").arg(f->lowHz).arg(f->highHz);
            } else {
                m_txFilterHandler(f->lowHz, f->highHz, &note);
            }
        }
        m_status->setText(note.isEmpty() ? tr("Loaded “%1”.").arg(name)
                                         : tr("Loaded “%1”. %2").arg(name, note));
        emit profileApplied();
        return;
    }
    if (!m_profiles->loadProfile(name)) {
        m_status->setText(tr("Could not load “%1”.").arg(name));
        return;
    }
    m_status->setText(tr("Loaded “%1”.").arg(name));
    emit profileApplied();
}

void AetherTxSettingsDialog::onDelete()
{
    const QString name = selectedName();
    if (name.isEmpty() || selectedIsStock()) return;
    const auto answer = QMessageBox::question(
        this, tr("Delete profile?"),
        tr("Delete “%1”? This cannot be undone.").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;
    if (!m_profiles->deleteProfile(name)) {
        m_status->setText(tr("Could not delete “%1”.").arg(name));
        return;
    }
    m_status->setText(tr("Deleted “%1”.").arg(name));
}

void AetherTxSettingsDialog::onExport()
{
    const QString name = selectedName();
    if (name.isEmpty()) return;

    QString suggested = name;
    suggested.replace(QRegularExpression(QStringLiteral("[^\\w \\-]")),
                      QStringLiteral("_"));
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export profile"),
        dir + QLatin1Char('/') + suggested + QStringLiteral(".json"),
        QLatin1String(kFileFilter));
    if (path.isEmpty()) return;

    const bool written = selectedIsStock() ? m_profiles->exportStockToFile(name, path)
                                           : m_profiles->exportToFile(name, path);
    if (!written) {
        m_status->setText(tr("Could not write %1.").arg(path));
        return;
    }
    m_status->setText(tr("Exported “%1” to %2.").arg(name, QFileInfo(path).fileName()));
}

void AetherTxSettingsDialog::onImport()
{
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import profile"), dir, QLatin1String(kFileFilter));
    if (path.isEmpty()) return;

    QString suggested;
    QString error;
    const QJsonObject profile =
        AetherTxProfiles::readFile(path, &suggested, &error);
    if (profile.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"), error);
        m_status->setText(error);
        return;
    }

    // Always ask. The file's own name is only a suggestion, and landing an
    // import on top of a profile that happens to share it would destroy work
    // the operator never offered up.
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Import profile"),
        tr("Save the profile from %1 as:").arg(QFileInfo(path).fileName()),
        QLineEdit::Normal, m_profiles->uniqueName(suggested), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (AetherTxProfiles::isReservedName(name)) {
        m_status->setText(tr("“%1” is a stock preset's name; import it under another name.").arg(name));
        return;
    }

    if (m_profiles->hasProfile(name)) {
        const auto answer = QMessageBox::question(
            this, tr("Replace profile?"),
            tr("“%1” already exists. Replace it?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }
    if (!m_profiles->addProfile(name, profile)) {
        m_status->setText(tr("Could not save the imported profile."));
        return;
    }
    m_status->setText(tr("Imported “%1”.").arg(name));
}

} // namespace AetherSDR
