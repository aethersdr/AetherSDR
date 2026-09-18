#include "AetherRxSettingsDialog.h"
#include "ModemChrome.h"
#include "core/AetherRxProfiles.h"
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
    "AetherRX profile (*.json);;All files (*)";
}

AetherRxSettingsDialog::AetherRxSettingsDialog(AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_profiles(new AetherRxProfiles(audio, this))
{
    setWindowTitle(tr("AetherRX Settings"));
    setObjectName(QStringLiteral("aetherRxSettingsDialog"));
    ThemeManager::instance().applyStyleSheet(
        this, ModemChrome::styleSheet(ModemChrome::Scale::Dialog));
    resize(460, 380);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto* heading = new QLabel(tr("Profiles"));
    heading->setObjectName(QStringLiteral("SectionLabel"));
    root->addWidget(heading);

    auto* blurb = new QLabel(tr(
        "A profile is the receive chain: every stage's settings, which of them "
        "are switched on, the order they run in, and the noise reduction "
        "method. Loading one leaves the transmit side alone."));
    blurb->setWordWrap(true);
    root->addWidget(blurb);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("aetherRxProfileList"));
    m_list->setAccessibleName(tr("Saved AetherRX profiles"));
    root->addWidget(m_list, 1);

    // Library actions, then the two that cross the filesystem.
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    const auto addBtn = [&](const QString& text, const QString& objName,
                            const QString& tip, void (AetherRxSettingsDialog::*slot)()) {
        auto* b = new QPushButton(text);
        b->setObjectName(objName);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, slot);
        row->addWidget(b);
        return b;
    };
    m_saveBtn = addBtn(tr("Save…"), QStringLiteral("aetherRxProfileSave"),
                       tr("Store the receive chain as it is now under a name."),
                       &AetherRxSettingsDialog::onSave);
    m_loadBtn = addBtn(tr("Load"), QStringLiteral("aetherRxProfileLoad"),
                       tr("Apply the selected profile to the receive chain."),
                       &AetherRxSettingsDialog::onLoad);
    m_deleteBtn = addBtn(tr("Delete"), QStringLiteral("aetherRxProfileDelete"),
                         tr("Remove the selected profile from the library."),
                         &AetherRxSettingsDialog::onDelete);
    root->addLayout(row);

    auto* fileRow = new QHBoxLayout;
    fileRow->setSpacing(6);
    row = fileRow;
    m_importBtn = addBtn(tr("Import…"), QStringLiteral("aetherRxProfileImport"),
                         tr("Read a profile from a JSON file and save it under "
                            "a name you choose."),
                         &AetherRxSettingsDialog::onImport);
    m_exportBtn = addBtn(tr("Export…"), QStringLiteral("aetherRxProfileExport"),
                         tr("Write the selected profile to a JSON file."),
                         &AetherRxSettingsDialog::onExport);
    fileRow->addStretch(1);
    root->addLayout(fileRow);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto* closeRow = new QHBoxLayout;
    closeRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setObjectName(QStringLiteral("aetherRxSettingsClose"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    closeRow->addWidget(closeBtn);
    root->addLayout(closeRow);

    connect(m_list, &QListWidget::itemSelectionChanged,
            this, &AetherRxSettingsDialog::updateButtonStates);
    connect(m_list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onLoad(); });
    connect(m_profiles, &AetherRxProfiles::profilesChanged,
            this, &AetherRxSettingsDialog::refreshList);

    refreshList();
}

void AetherRxSettingsDialog::refreshList()
{
    const QString keep = selectedName();
    m_list->clear();
    m_list->addItems(m_profiles->profileNames());
    if (!keep.isEmpty()) {
        const auto hits = m_list->findItems(keep, Qt::MatchExactly);
        if (!hits.isEmpty()) m_list->setCurrentItem(hits.constFirst());
    }
    if (m_list->count() == 0) {
        m_status->setText(tr("No profiles yet — Save… stores the chain you "
                             "have set up now."));
    }
    updateButtonStates();
}

QString AetherRxSettingsDialog::selectedName() const
{
    auto* item = m_list->currentItem();
    return item ? item->text() : QString();
}

void AetherRxSettingsDialog::updateButtonStates()
{
    const bool has = !selectedName().isEmpty();
    m_loadBtn->setEnabled(has);
    m_deleteBtn->setEnabled(has);
    m_exportBtn->setEnabled(has);
}

void AetherRxSettingsDialog::onSave()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save profile"), tr("Name for this receive chain:"),
        QLineEdit::Normal, selectedName(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;

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

void AetherRxSettingsDialog::onLoad()
{
    const QString name = selectedName();
    if (name.isEmpty()) return;
    if (!m_profiles->loadProfile(name)) {
        m_status->setText(tr("Could not load “%1”.").arg(name));
        return;
    }
    m_status->setText(tr("Loaded “%1”.").arg(name));
    emit profileApplied();
}

void AetherRxSettingsDialog::onDelete()
{
    const QString name = selectedName();
    if (name.isEmpty()) return;
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

void AetherRxSettingsDialog::onExport()
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

    if (!m_profiles->exportToFile(name, path)) {
        m_status->setText(tr("Could not write %1.").arg(path));
        return;
    }
    m_status->setText(tr("Exported “%1” to %2.").arg(name, QFileInfo(path).fileName()));
}

void AetherRxSettingsDialog::onImport()
{
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import profile"), dir, QLatin1String(kFileFilter));
    if (path.isEmpty()) return;

    QString suggested;
    QString error;
    const QJsonObject profile =
        AetherRxProfiles::readFile(path, &suggested, &error);
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
