#include "FirmwareReleasesDialog.h"
#include "core/FirmwareCurrency.h"
#include "core/ThemeManager.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace AetherSDR {

namespace {

constexpr int kReleaseRole = Qt::UserRole + 1;

const char* kDialogStyle =
    "QLabel { color: {{color.text.primary}}; background: transparent; }"
    "QLabel#caption { color: {{color.text.secondary}}; font-size: 11px; }"
    "QListWidget {"
    "  background: {{color.background.1}}; color: {{color.text.primary}};"
    "  border: 1px solid {{color.border}}; border-radius: 3px; }"
    "QListWidget::item { padding: 5px 7px; }"
    // No ::item:selected rule: selection comes from the palette, as for the
    // app-wide lists, so it stays readable whatever the theme's accent.
    // `favorites_picker_selection_test` scans src/ and fails the build if an
    // accent-coloured selection creeps back in.
    "QListWidget::item:hover { background: {{color.background.2}}; }"
    "QPushButton {"
    "  background: {{color.background.1}}; color: {{color.text.primary}};"
    "  border: 1px solid {{color.border}}; border-radius: 3px;"
    "  padding: 4px 10px; }"
    "QPushButton:disabled {"
    "  background: {{color.button.background.disabled}};"
    "  color: {{color.button.foreground.disabled}};"
    "  border-color: {{color.button.border.disabled}}; }";

} // namespace

FirmwareReleasesDialog::FirmwareReleasesDialog(const QStringList& releases,
                                               const QString& installedVersion,
                                               const QString& releaseNotesUrlTemplate,
                                               QWidget* parent)
    : PersistentDialog(tr("Available SmartSDR Releases"),
                       QStringLiteral("FirmwareReleasesDialogGeometry"), parent)
    , m_releaseNotesUrlTemplate(releaseNotesUrlTemplate)
{
    setMinimumSize(430, 330);
    ThemeManager::instance().applyStyleSheet(this, kDialogStyle);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(9);

    // The radio's own release, truncated the way every comparison here is: the
    // build component ("4.2.20.41343") is not published, so it has nothing on
    // the other side to meet.
    const QVersionNumber installed = FirmwareCurrency::releaseOf(installedVersion);
    root->addWidget(new QLabel(
        installed.isNull()
            ? tr("This radio has not reported a firmware version.")
            : tr("This radio is running %1.").arg(installed.toString()),
        bodyWidget()));

    // The newest release in the RADIO'S OWN LINE, which is the one worth
    // pointing at. The newest release overall may be a major version this radio
    // is not licensed for, and its notes describe a line the operator is not on.
    const QString newestInLine =
        FirmwareCurrency::newestInLine(releases, installedVersion);

    m_list = new QListWidget(bodyWidget());
    for (const QString& release : releases) {
        QString label = release;
        if (!installed.isNull() && QVersionNumber::fromString(release) == installed)
            label += tr("   — installed on this radio");
        else if (release == newestInLine)
            label += tr("   — newest for this radio");

        auto* item = new QListWidgetItem(label, m_list);
        item->setData(kReleaseRole, release);
    }
    root->addWidget(m_list, 1);

    auto* caption = new QLabel(
        m_releaseNotesUrlTemplate.isEmpty()
            ? tr("To install a release, download its installer from flexradio.com, "
                 "then use \"Select Installer...\".")
            : tr("Select a release to read its notes. To install one, download its "
                 "installer from flexradio.com, then use \"Select Installer...\"."),
        bodyWidget());
    caption->setObjectName(QStringLiteral("caption"));
    caption->setWordWrap(true);
    root->addWidget(caption);

    auto* btnRow = new QHBoxLayout;
    m_notesBtn = new QPushButton(tr("Release Notes"), bodyWidget());
    m_notesBtn->setEnabled(false);
    btnRow->addWidget(m_notesBtn);
    btnRow->addStretch(1);
    auto* closeBtn = new QPushButton(tr("Close"), bodyWidget());
    closeBtn->setDefault(true);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // An empty template means the backend declares no release-notes source, so
    // the button stays dead rather than opening a URL we invented.
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        m_notesBtn->setEnabled(!m_releaseNotesUrlTemplate.isEmpty()
                               && !selectedRelease().isEmpty());
    });
    connect(m_notesBtn, &QPushButton::clicked, this,
            [this] { openReleaseNotesFor(selectedRelease()); });
    connect(m_list, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
        openReleaseNotesFor(item->data(kReleaseRole).toString());
    });
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    // Start on the release this operator most likely opened the list to read.
    if (!newestInLine.isEmpty()) {
        for (int row = 0; row < m_list->count(); ++row) {
            if (m_list->item(row)->data(kReleaseRole).toString() == newestInLine) {
                m_list->setCurrentRow(row);
                break;
            }
        }
    }
}

QString FirmwareReleasesDialog::selectedRelease() const
{
    const QListWidgetItem* item = m_list ? m_list->currentItem() : nullptr;
    return item ? item->data(kReleaseRole).toString() : QString();
}

void FirmwareReleasesDialog::openReleaseNotesFor(const QString& version)
{
    const QString url =
        FirmwareCurrency::releaseNotesUrl(m_releaseNotesUrlTemplate, version);
    if (url.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl(url));
}

} // namespace AetherSDR
