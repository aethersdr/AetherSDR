#include "AetherTxSettingsDialog.h"
#include "ModemChrome.h"
#include "core/AetherTxProfiles.h"
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
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
constexpr const char* kFileFilter =
    "AetherTX profile (*.json);;All files (*)";
}

AetherTxSettingsDialog::AetherTxSettingsDialog(AudioEngine* audio, QWidget* parent)
    : QDialog(parent)
    , m_profiles(new AetherTxProfiles(audio, this))
{
    setWindowTitle(tr("AetherTX Settings"));
    setObjectName(QStringLiteral("aetherTxSettingsDialog"));
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
        "A profile is the transmit chain: every stage's settings, which of "
        "them are switched on, and the order they run in. Loading one leaves "
        "the receive side alone."));
    blurb->setWordWrap(true);
    root->addWidget(blurb);

    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("aetherTxProfileList"));
    m_list->setAccessibleName(tr("Saved AetherTX profiles"));
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

    // ── The two live controls the chain row used to carry ────────────────
    auto* liveHeading = new QLabel(tr("Chain"));
    liveHeading->setObjectName(QStringLiteral("SectionLabel"));
    root->addWidget(liveHeading);

    auto* liveRow = new QHBoxLayout;
    liveRow->setSpacing(6);

    m_bypassBtn = new QPushButton(tr("BYPASS"));
    m_bypassBtn->setObjectName(QStringLiteral("aetherTxBypass"));
    m_bypassBtn->setCheckable(true);
    m_bypassBtn->setToolTip(
        tr("Suppress every voice stage at once, so the microphone reaches the "
           "radio unprocessed. The docked chain applet carries the same "
           "control if you want it to hand mid-transmission."));
    connect(m_bypassBtn, &QPushButton::toggled,
            this, &AetherTxSettingsDialog::bypassToggled);
    liveRow->addWidget(m_bypassBtn);

    liveRow->addSpacing(16);

    m_monRecBtn = new QPushButton(tr("Record"));
    m_monRecBtn->setObjectName(QStringLiteral("aetherTxMonitorRecord"));
    m_monRecBtn->setCheckable(true);
    m_monRecBtn->setToolTip(
        tr("Record up to 30 s of processed transmit audio (MIC must be set to "
           "PC and DAX off). Click again to stop; playback starts by itself."));
    connect(m_monRecBtn, &QPushButton::clicked,
            this, &AetherTxSettingsDialog::monitorRecordClicked);
    liveRow->addWidget(m_monRecBtn);

    m_monPlayBtn = new QPushButton(tr("Play"));
    m_monPlayBtn->setObjectName(QStringLiteral("aetherTxMonitorPlay"));
    m_monPlayBtn->setCheckable(true);
    m_monPlayBtn->setEnabled(false);
    m_monPlayBtn->setToolTip(
        tr("Play back the captured audio. Click again to cancel."));
    // Why it is greyed out has to reach the accessible channel too, not just
    // the tooltip — a screen-reader user meets a disabled button with no
    // explanation otherwise (#4896).
    m_monPlayBtn->setAccessibleDescription(
        tr("Unavailable until something has been recorded. "
           "Use Record first."));
    connect(m_monPlayBtn, &QPushButton::clicked,
            this, &AetherTxSettingsDialog::monitorPlayClicked);
    liveRow->addWidget(m_monPlayBtn);

    liveRow->addStretch(1);
    root->addLayout(liveRow);

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

QString AetherTxSettingsDialog::selectedName() const
{
    auto* item = m_list->currentItem();
    return item ? item->text() : QString();
}

void AetherTxSettingsDialog::updateButtonStates()
{
    const bool has = !selectedName().isEmpty();
    m_loadBtn->setEnabled(has);
    m_deleteBtn->setEnabled(has);
    m_exportBtn->setEnabled(has);
}

void AetherTxSettingsDialog::onSave()
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save profile"), tr("Name for this transmit chain:"),
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

void AetherTxSettingsDialog::onLoad()
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

void AetherTxSettingsDialog::onDelete()
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

    if (!m_profiles->exportToFile(name, path)) {
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

void AetherTxSettingsDialog::setMonitorRecording(bool on)
{
    if (m_monRecBtn) m_monRecBtn->setChecked(on);
}

void AetherTxSettingsDialog::setMonitorPlaying(bool on)
{
    if (m_monPlayBtn) m_monPlayBtn->setChecked(on);
}

void AetherTxSettingsDialog::setMonitorHasRecording(bool has)
{
    if (!m_monPlayBtn) return;
    m_monPlayBtn->setEnabled(has);
    m_monPlayBtn->setAccessibleDescription(
        has ? tr("Play back the captured audio. Click again to cancel.")
            : tr("Unavailable until something has been recorded. "
                 "Use Record first."));
}

void AetherTxSettingsDialog::setBypassed(bool on)
{
    if (!m_bypassBtn) return;
    QSignalBlocker block(m_bypassBtn);
    m_bypassBtn->setChecked(on);
}

} // namespace AetherSDR
