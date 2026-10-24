#include "gui/FirmwareUpdateCard.h"

#include "core/FirmwareCurrency.h"
#include "core/FirmwareStager.h"
#include "core/FirmwareUploader.h"
#include "gui/CanonPrompt.h"
#include "gui/ScopedChildWidget.h"
#include "models/RadioModel.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

#include <QDateTime>

#include <algorithm>
#include <optional>

namespace AetherSDR {

namespace {

QLabel* plain(const QString& role, QWidget* parent)
{
    auto* l = new QLabel(parent);
    l->setTextFormat(Qt::PlainText);   // versions and messages come from the network and the radio
    l->setProperty("role", role);
    l->setWordWrap(true);
    return l;
}

void restyle(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

// FlexRadio's published list, shared by every window this process opens.
// Fetched at most once a day, the policy FirmwareCurrency states for the
// same index: an operator who connects daily contacts flexradio.com once a
// day, not once per window or reconnect.
struct ReleaseListCache {
    QStringList releases;
    QDateTime fetchedUtc;
    bool fresh() const
    {
        return fetchedUtc.isValid()
            && fetchedUtc.secsTo(QDateTime::currentDateTimeUtc()) < FirmwareCurrency::kPublishedVersionMaxAgeSecs;
    }
};
ReleaseListCache& releaseListCache()
{
    static ReleaseListCache cache;
    return cache;
}

QString displayVersion(const QString& version)
{
    const QVersionNumber release = FirmwareCurrency::releaseOf(version);
    return release.isNull() ? version : release.toString();
}

} // namespace

FirmwareUpdateCard::FirmwareUpdateCard(RadioModel* model, QWidget* parent)
    : QFrame(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("updatesFirmwareCard"));
    m_stager = new FirmwareStager(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 16);
    root->setSpacing(12);

    auto* header = new QHBoxLayout;
    header->setSpacing(10);
    auto* eyebrow = plain(QStringLiteral("eyebrow"), this);
    eyebrow->setText(tr("YOUR RADIO"));
    eyebrow->setWordWrap(false);
    header->addWidget(eyebrow);
    m_radioName = plain(QStringLiteral("radioName"), this);
    m_radioName->setWordWrap(false);
    header->addWidget(m_radioName);
    m_radioDetail = plain(QStringLiteral("radioDetail"), this);
    m_radioDetail->setWordWrap(false);
    header->addWidget(m_radioDetail);
    header->addStretch(1);
    m_pill = plain(QStringLiteral("pill"), this);
    m_pill->setObjectName(QStringLiteral("firmwareStatus"));
    m_pill->setWordWrap(false);
    m_pill->setAccessibleName(tr("Radio firmware status"));
    header->addWidget(m_pill);
    root->addLayout(header);

    m_lanes = new QWidget(this);
    m_lanes->setObjectName(QStringLiteral("firmwareLanes"));
    m_lanesLayout = new QVBoxLayout(m_lanes);
    m_lanesLayout->setContentsMargins(0, 0, 0, 0);
    m_lanesLayout->setSpacing(8);
    m_releaseGroup = new QButtonGroup(this);
    m_releaseGroup->setExclusive(true);
    root->addWidget(m_lanes);

    m_description = plain(QStringLiteral("note"), this);
    m_description->setObjectName(QStringLiteral("firmwareDescription"));
    root->addWidget(m_description);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_install = new QPushButton(tr("Install…"), this);
    m_install->setObjectName(QStringLiteral("updatesSecondary"));
    m_install->setAccessibleName(tr("Install firmware"));
    actions->addWidget(m_install);
    m_openInstaller = new QPushButton(tr("Open Installer…"), this);
    m_openInstaller->setObjectName(QStringLiteral("updatesSecondary"));
    actions->addWidget(m_openInstaller);
    m_notes = new QPushButton(tr("Release notes"), this);
    m_notes->setObjectName(QStringLiteral("updatesGhost"));
    actions->addWidget(m_notes);
    actions->addStretch(1);
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_cancel->setObjectName(QStringLiteral("updatesGhost"));
    m_cancel->hide();
    actions->addWidget(m_cancel);
    root->addLayout(actions);

    // Three steps, numbered; the current one is lit, finished ones are marked.
    m_steps = new QWidget(this);
    auto* steps = new QHBoxLayout(m_steps);
    steps->setContentsMargins(0, 0, 0, 0);
    steps->setSpacing(8);
    for (const QString& name : {tr("1  Prepare firmware"), tr("2  Upload to radio"), tr("3  Radio confirms")}) {
        auto* tile = plain(QStringLiteral("step"), m_steps);
        tile->setText(name);
        tile->setWordWrap(false);
        tile->setAlignment(Qt::AlignCenter);
        steps->addWidget(tile, 1);
        m_stepTiles.append(tile);
    }
    m_steps->hide();
    root->addWidget(m_steps);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("firmwareProgress"));
    m_progress->setAccessibleName(tr("Firmware install progress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    m_progress->hide();
    root->addWidget(m_progress);

    m_status = plain(QStringLiteral("status"), this);
    m_status->setObjectName(QStringLiteral("firmwareInstallStatus"));
    m_status->setAccessibleName(tr("Firmware install status"));
    m_status->hide();
    root->addWidget(m_status);

    m_caution = plain(QStringLiteral("caution"), this);
    m_caution->setText(tr("⚠ Firmware update from AetherSDR is experimental. FlexRadio's SmartSDR for "
                          "Windows is still the recommended way to update."));
    root->addWidget(m_caution);

    connect(m_releaseGroup, &QButtonGroup::buttonToggled, this, [this](QAbstractButton*, bool on) {
        if (on) {
            syncSelection();
        }
    });
    connect(m_install, &QPushButton::clicked, this, &FirmwareUpdateCard::startInstall);
    connect(m_openInstaller, &QPushButton::clicked, this, &FirmwareUpdateCard::openInstaller);
    connect(m_notes, &QPushButton::clicked, this, [this] {
        if (!m_model) {
            return;
        }
        const auto& source = m_model->backendCapabilities().firmwareUpdateSource;
        const QString version = selectedRelease().isEmpty()
                                    ? displayVersion(m_model->softwareVersion())
                                    : selectedRelease();
        const QString url = source ? FirmwareCurrency::releaseNotesUrl(source->releaseNotesUrlTemplate, version)
                                   : QString();
        if (!url.isEmpty()) {
            QDesktopServices::openUrl(QUrl(url));
        }
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] { confirmStop(window()); });

    connect(m_stager, &FirmwareStager::updateCheckComplete, this,
            [this](const QString&, bool, const QStringList& published) {
        releaseListCache() = {published, QDateTime::currentDateTimeUtc()};
        showReleases(published);
    });
    // The release-list check owns the pill only: it never writes the
    // install's status line or progress bar, so a radio's reconnect
    // after an install cannot overwrite the install's outcome.
    connect(m_stager, &FirmwareStager::updateCheckFailed, this, [this](const QString& error) {
        m_pill->setText(tr("Releases unavailable"));
        m_pill->setToolTip(error);
        m_pill->setAccessibleDescription(error);
        m_pill->setProperty("tone", QString());
        restyle(m_pill);
    });
    connect(m_stager, &FirmwareStager::stageProgress, this, [this](int percent, const QString& status) {
        if (!m_staging) {
            return;   // checkForUpdate() reports on this signal too; listing releases is not an install
        }
        m_progress->setValue(percent);
        setStatus(status, QString());
    });
    connect(m_stager, &FirmwareStager::stageComplete, this, [this](const QString& path, const QString&) {
        m_staging = false;
        // Install confirmed up front on the promise of an MD5 check; when
        // FlexRadio's hash was unavailable that promise was not kept, so ask
        // again and say so. Open Installer always asks before the upload.
        const bool unverifiedDownload = m_uploadAfterStage && !m_stager->stagedVerified();
        const bool confirmFirst = !m_uploadAfterStage || unverifiedDownload;
        m_uploadAfterStage = false;
        uploadStaged(path, confirmFirst, unverifiedDownload);
    });
    connect(m_stager, &FirmwareStager::stageFailed, this, [this](const QString& error) {
        m_staging = false;
        m_uploadAfterStage = false;
        setStep(Step::Idle);
        setStatus(error, QStringLiteral("danger"));
        setBusy(false);
    });

    if (m_model) {
        connect(m_model, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            if (!connected) {
                showReleases({});   // the next radio's installed release is not this one's
            }
            refreshAvailability();
            if (connected) {
                requestReleases();
            }
        });
        connect(m_model, &RadioModel::infoChanged, this, &FirmwareUpdateCard::refreshAvailability);
    }
    refreshAvailability();
    requestReleases();
}

bool FirmwareUpdateCard::isBusy() const
{
    return m_staging || (m_uploader && m_uploader->isUploading());
}

void FirmwareUpdateCard::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    m_cancel->setVisible(busy);
    m_lanes->setEnabled(!busy);
    refreshAvailability();
    emit busyChanged(busy);
}

void FirmwareUpdateCard::refreshAvailability()
{
    const bool connected = m_model && m_model->isConnected();
    const bool capable = connected && m_model->backendCapabilities().canUpgradeFirmware;
    const QString family = connected ? FirmwareStager::modelToFamily(m_model->model()) : QString();

    m_radioName->setText(connected ? (m_model->nickname().isEmpty() ? m_model->model() : m_model->nickname())
                                   : tr("No radio connected"));
    m_radioDetail->setText(connected
                               ? QStringLiteral("%1 · %2").arg(m_model->model(),
                                                               displayVersion(m_model->softwareVersion()))
                               : QString());

    QString reason;
    if (!connected) {
        reason = tr("Connect to a radio to see its firmware.");
    } else if (!capable) {
        reason = tr("AetherSDR cannot update this radio's firmware.");
    } else if (family.isEmpty()) {
        reason = tr("AetherSDR has no firmware installer for the %1.").arg(m_model->model());
    }
    const bool usable = reason.isEmpty() && !m_busy;
    m_lanes->setVisible(capable && !m_releases.isEmpty());
    m_caution->setVisible(capable);
    m_notes->setVisible(capable);
    // A backend that declares no release-notes source leaves the button dead
    // rather than opening a URL invented here.
    const auto& source = connected ? m_model->backendCapabilities().firmwareUpdateSource
                                   : std::optional<FirmwareUpdateSource>{};
    const bool hasNotes = source && !source->releaseNotesUrlTemplate.isEmpty();
    m_notes->setEnabled(hasNotes);
    const QString notesReason = hasNotes ? QString() : tr("This radio's vendor publishes no release-notes link.");
    m_notes->setToolTip(notesReason);
    m_notes->setAccessibleDescription(notesReason);
    m_install->setEnabled(usable && !selectedRelease().isEmpty());
    m_openInstaller->setEnabled(usable);
    // Dimmed controls state why, where a screen reader hears it.
    m_install->setToolTip(reason);
    m_install->setAccessibleDescription(
        reason.isEmpty() ? tr("Downloads the chosen release from FlexRadio and uploads it to the radio.")
                         : reason);
    m_openInstaller->setToolTip(reason);
    m_openInstaller->setAccessibleDescription(
        reason.isEmpty() ? tr("Install from a SmartSDR installer (.msi or .exe) or an extracted .ssdr "
                              "file you downloaded.")
                         : reason);
    if (!reason.isEmpty()) {
        m_description->setText(reason);
        m_pill->setText(connected ? tr("Not updatable here") : tr("No radio"));
        m_pill->setProperty("tone", QString());
        restyle(m_pill);
    } else {
        syncSelection();
    }
}

void FirmwareUpdateCard::requestReleases()
{
    if (!m_model || !m_model->isConnected() || !m_model->backendCapabilities().canUpgradeFirmware
        || isBusy()) {
        return;
    }
    if (releaseListCache().fresh()) {
        showReleases(releaseListCache().releases);
        return;
    }
    m_pill->setText(tr("Checking…"));
    m_stager->checkForUpdate(m_model->softwareVersion());
}

void FirmwareUpdateCard::showReleases(const QStringList& releases)
{
    m_releases = releases;
    for (QAbstractButton* b : m_releaseGroup->buttons()) {
        m_releaseGroup->removeButton(b);
    }
    while (QLayoutItem* item = m_lanesLayout->takeAt(0)) {
        if (QLayout* l = item->layout()) {
            while (QLayoutItem* child = l->takeAt(0)) {
                delete child->widget();
                delete child;
            }
        }
        delete item->widget();
        delete item;
    }

    const QString installedText = m_model ? m_model->softwareVersion() : QString();
    const QVersionNumber installed = FirmwareCurrency::releaseOf(installedText);
    const QString newest = FirmwareCurrency::newestInLine(releases, installedText);

    const QString preselectText = preselectedRelease(releases, installedText);
    QAbstractButton* preselect = nullptr;
    for (const auto& [major, laneReleases] : firmwareLanes(releases)) {
        auto* lane = new QHBoxLayout;
        lane->setSpacing(6);
        auto* label = plain(QStringLiteral("lane"), m_lanes);
        label->setText(QStringLiteral("v%1").arg(major));
        label->setWordWrap(false);
        label->setFixedWidth(28);
        lane->addWidget(label);
        for (const QString& r : laneReleases) {
            auto* node = new QPushButton(r, m_lanes);
            node->setObjectName(QStringLiteral("firmwareRelease"));
            node->setCheckable(true);
            node->setCursor(Qt::PointingHandCursor);
            QString kind;
            QString description;
            if (!installed.isNull() && QVersionNumber::fromString(r) == installed) {
                kind = QStringLiteral("installed");
                description = tr("Installed on this radio");
            } else if (r == newest) {
                kind = QStringLiteral("newest");
                description = tr("Newest release in this radio's line");
            }
            node->setProperty("kind", kind);
            node->setAccessibleName(tr("SmartSDR %1").arg(r));
            node->setAccessibleDescription(description);
            node->setToolTip(description);
            m_releaseGroup->addButton(node);
            lane->addWidget(node);
            if (r == preselectText) {
                preselect = node;
            }
        }
        lane->addStretch(1);
        m_lanesLayout->addLayout(lane);
    }
    if (preselect) {
        preselect->setChecked(true);
    }

    const bool behind = !newest.isEmpty() && FirmwareCurrency::compareReleases(newest, installedText) > 0;
    m_pill->setText(behind ? QStringLiteral("▲ ") + tr("Behind %1").arg(newest)
                           : (newest.isEmpty() ? tr("Line not published") : QStringLiteral("✓ ") + tr("Current")));
    m_pill->setProperty("tone", behind ? QStringLiteral("warning") : QStringLiteral("current"));
    restyle(m_pill);
    refreshAvailability();
}

QList<QPair<int, QStringList>> FirmwareUpdateCard::firmwareLanes(const QStringList& releasesNewestFirst)
{
    QMap<int, QStringList> byMajor;
    for (const QString& r : releasesNewestFirst) {
        const QVersionNumber v = QVersionNumber::fromString(r);
        if (!v.isNull()) {
            byMajor[v.majorVersion()].prepend(r);
        }
    }
    QList<QPair<int, QStringList>> lanes;
    for (auto it = byMajor.constEnd(); it != byMajor.constBegin();) {
        --it;
        lanes.append({it.key(), it.value()});
    }
    return lanes;
}

QString FirmwareUpdateCard::preselectedRelease(const QStringList& releasesNewestFirst,
                                               const QString& installedVersion)
{
    const QString newest = FirmwareCurrency::newestInLine(releasesNewestFirst, installedVersion);
    return !newest.isEmpty() && FirmwareCurrency::compareReleases(newest, installedVersion) > 0 ? newest
                                                                                                : QString();
}

QString FirmwareUpdateCard::selectedRelease() const
{
    const QAbstractButton* b = m_releaseGroup->checkedButton();
    return b ? b->text() : QString();
}

void FirmwareUpdateCard::syncSelection()
{
    if (!m_model || !m_model->isConnected()) {
        return;
    }
    const QString release = selectedRelease();
    m_install->setText(release.isEmpty() ? tr("Install…") : tr("Install %1…").arg(release));
    m_install->setEnabled(!m_busy && !release.isEmpty()
                          && !FirmwareStager::modelToFamily(m_model->model()).isEmpty());
    m_description->setText(
        release.isEmpty()
            ? tr("Choose a release to install, or open an installer you downloaded.")
            : tr("SmartSDR %1 for the %2: Install downloads the installer from FlexRadio, checks it "
                 "against FlexRadio's MD5 (and asks again if that can't be done), extracts the firmware "
                 "and uploads it. The radio reboots.")
                  .arg(release, m_model->model()));
}

void FirmwareUpdateCard::startInstall()
{
    const QString release = selectedRelease();
    if (!m_model || release.isEmpty() || isBusy()) {
        return;
    }
    const QPointer<FirmwareUpdateCard> self(this);
    ScopedChildWidget<CanonPrompt> boxOwner(
        tr("Firmware Update"),
        tr("Download SmartSDR %1 from FlexRadio and upload it to %2?\n\n"
           "The radio will reboot after the update.\nDo not disconnect during the upload.")
            .arg(release, m_model->model()),
        tr("Install"), window());
    const int reply = boxOwner.get()->exec();
    if (!self || !boxOwner || reply != QDialog::Accepted || !m_model) {
        return;
    }
    m_staging = true;
    m_uploadAfterStage = true;
    setBusy(true);
    setStep(Step::Prepare);
    m_progress->show();
    m_progress->setValue(0);
    m_stager->downloadAndStage(release, FirmwareStager::modelToFamily(m_model->model()));
}

void FirmwareUpdateCard::openInstaller()
{
    if (!m_model || isBusy()) {
        return;
    }
    const QPointer<FirmwareUpdateCard> self(this);
    const QPointer<RadioModel> model(m_model);
    const QString path = QFileDialog::getOpenFileName(
        window(), tr("Select SmartSDR Installer or Firmware File"), QString(),
        tr("SmartSDR installer or firmware (*.msi *.exe *.ssdr);;MSI installer (*.msi);;"
           "EXE installer (*.exe);;Extracted firmware (*.ssdr);;All files (*)"));
    if (!self || !model || path.isEmpty()) {
        return;
    }
    m_staging = true;
    m_uploadAfterStage = false;
    setBusy(true);
    setStep(Step::Prepare);
    m_progress->show();
    m_progress->setValue(0);
    setStatus(tr("Preparing firmware from %1…").arg(QFileInfo(path).fileName()), QString());
    m_stager->stageFromLocalFile(path, FirmwareStager::modelToFamily(m_model->model()));
}

void FirmwareUpdateCard::uploadStaged(const QString& path, bool confirmFirst, bool unverifiedDownload)
{
    if (!m_model) {
        setBusy(false);
        return;
    }
    if (confirmFirst) {
        const QPointer<FirmwareUpdateCard> self(this);
        const QString question =
            unverifiedDownload
                ? tr("FlexRadio's MD5 hash for this installer could not be downloaded, so the "
                     "firmware's integrity was NOT checked.\n\nUpload %1 to %2 anyway?\n\n"
                     "The radio will reboot after the update.\nDo not disconnect during the upload.")
                : tr("Upload %1 to %2?\n\nThe radio will reboot after the update.\n"
                     "Do not disconnect during the upload.");
        ScopedChildWidget<CanonPrompt> boxOwner(
            tr("Firmware Update"), question.arg(QFileInfo(path).fileName(), m_model->model()),
            tr("Upload"), window());
        const int reply = boxOwner.get()->exec();
        if (!self || !boxOwner) {
            return;
        }
        if (reply != QDialog::Accepted || !m_model) {
            setStep(Step::Idle);
            m_progress->hide();
            setStatus(tr("Firmware is ready; the upload was not started."), QStringLiteral("muted"));
            setBusy(false);
            return;
        }
    }
    ensureUploader();
    setStep(Step::Upload);
    m_progress->setValue(0);
    m_uploader->upload(path);
}

void FirmwareUpdateCard::ensureUploader()
{
    if (m_uploader) {
        return;
    }
    m_uploader = new FirmwareUploader(m_model, this);
    connect(m_uploader, &FirmwareUploader::progressChanged, this, [this](int percent, const QString& status) {
        m_progress->setValue(percent);
        setStatus(status, QString());
        if (m_uploader->phase() == FirmwareUploader::Phase::AwaitingConfirmation) {
            setStep(Step::Confirm);
        }
    });
    connect(m_uploader, &FirmwareUploader::finished, this,
            [this](FirmwareUploader::Outcome outcome, const QString& message) {
        // Three outcomes, not two (#5572): only the radio's own `file update
        // failed=` settles it, and the words say which, not colour alone.
        QString tone;
        switch (outcome) {
        case FirmwareUploader::Outcome::Succeeded:
            m_progress->setValue(100);
            tone = QStringLiteral("current");
            break;
        case FirmwareUploader::Outcome::Unconfirmed:
            // Bytes left the host and the radio is probably applying them; the
            // uploader refuses a retry until reconnect.
            m_progress->setValue(100);
            tone = QStringLiteral("warning");
            break;
        case FirmwareUploader::Outcome::Failed:
            m_progress->hide();
            tone = QStringLiteral("danger");
            break;
        }
        setStep(Step::Idle);
        setStatus(message, tone);
        setBusy(false);
    });
}

bool FirmwareUpdateCard::confirmStop(QWidget* parent)
{
    // A nested close/reject must not destroy the owner underneath this prompt.
    if (m_closePromptOpen) {
        return false;
    }
    if (m_staging && !(m_uploader && m_uploader->isUploading())) {
        const QPointer<FirmwareUpdateCard> self(this);
        ScopedChildWidget<CanonPrompt> boxOwner(
            tr("Firmware Update In Progress"),
            tr("The firmware is still being prepared. Nothing has been sent to the radio.\n\n"
               "Stop and close?"),
            tr("Stop and Close"), parent);
        m_closePromptOpen = true;
        const int reply = boxOwner.get()->exec();
        if (!self) {
            return false;
        }
        m_closePromptOpen = false;
        if (!boxOwner || reply != QDialog::Accepted) {
            return false;
        }
        m_stager->cancel();
        m_staging = false;
        m_uploadAfterStage = false;
        setStep(Step::Idle);
        m_progress->hide();
        setStatus(tr("Stopped before anything was sent to the radio."), QStringLiteral("muted"));
        setBusy(false);
        return true;
    }
    if (!m_uploader || !m_uploader->isUploading()) {
        return true;
    }
    const QPointer<FirmwareUpdateCard> self(this);
    const QPointer<FirmwareUploader> uploader(m_uploader);
    ScopedChildWidget<CanonPrompt> boxOwner(tr("Firmware Update In Progress"), QString(),
                                            tr("Close Anyway"), parent);
    CanonPrompt* box = boxOwner.get();
    const auto refreshPrompt = [uploader, box] {
        if (!uploader) {
            return;
        }
        switch (uploader->phase()) {
        case FirmwareUploader::Phase::Preparing:
            box->setText(tr("The firmware upload is being prepared. No image bytes have been sent."
                            "\n\nClose this window and cancel the attempt?"));
            break;
        case FirmwareUploader::Phase::Transferring:
            box->setText(tr("A firmware upload is in progress. Closing this window stops the transfer "
                            "and may leave the radio with an incomplete image. The update outcome "
                            "will remain unknown; reconnect before retrying.\n\nClose anyway?"));
            break;
        case FirmwareUploader::Phase::AwaitingConfirmation:
            box->setText(tr("Firmware bytes have left the local write buffer, but the radio has not "
                            "confirmed installation. Closing this window stops waiting for confirmation; "
                            "it does not undo the update.\n\nReconnect to check the firmware version "
                            "before retrying. Close anyway?"));
            break;
        case FirmwareUploader::Phase::Idle:
            box->setText(tr("The firmware upload attempt has ended. Close this window?"));
            break;
        }
    };
    refreshPrompt();
    // The upload can advance or finish while exec() runs its nested event loop.
    connect(uploader, &FirmwareUploader::progressChanged, box, refreshPrompt);
    connect(uploader, &FirmwareUploader::finished, box, refreshPrompt);
    m_closePromptOpen = true;
    const int reply = box->exec();
    if (!self) {
        return false;
    }
    m_closePromptOpen = false;
    if (!boxOwner || reply != QDialog::Accepted) {
        return false;
    }
    if (uploader) {
        // Classify the CURRENT phase; it may have changed inside the prompt.
        // cancel() is a no-op if a terminal radio result already arrived.
        uploader->cancel();
    }
    return !self.isNull();
}

void FirmwareUpdateCard::setStep(Step step)
{
    m_steps->setVisible(step != Step::Idle);
    const int current = static_cast<int>(step) - 1;
    for (int i = 0; i < m_stepTiles.size(); ++i) {
        m_stepTiles[i]->setProperty("state", i < current ? QStringLiteral("done")
                                             : i == current ? QStringLiteral("now")
                                                            : QString());
        restyle(m_stepTiles[i]);
    }
}

void FirmwareUpdateCard::setStatus(const QString& text, const QString& tone)
{
    m_status->setText(text);
    m_status->setAccessibleDescription(text);
    m_status->setProperty("tone", tone);
    m_status->setVisible(!text.isEmpty());
    restyle(m_status);
}

} // namespace AetherSDR
