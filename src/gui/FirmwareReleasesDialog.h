#pragma once

#include "PersistentDialog.h"

#include <QString>
#include <QStringList>

class QListWidget;
class QPushButton;

namespace AetherSDR {

// The list behind Radio Setup's "Check for Update": every SmartSDR release
// FlexRadio currently publishes, with the radio's own release marked and the
// newest one in its line called out.
//
// A list rather than a single "update available" line because FlexRadio offers
// several lines for download at once. "Newer firmware exists" is true for a v3
// radio and nearly useless to it: what that operator needs is the newest v3,
// which is a different answer from the newest overall.
//
// READ-ONLY. It names releases and links their notes; staging and uploading
// stay where they are, behind "Select Installer..." in Radio Setup.
class FirmwareReleasesDialog : public PersistentDialog {
    Q_OBJECT
public:
    // `releases` is newest-first, as FirmwareStager::parseAllReleases() returns
    // it. `installedVersion` is what the radio reports and may carry a build
    // component; it is matched on major.minor.patch. `releaseNotesUrlTemplate`
    // comes from the backend's firmware-update capability record -- empty when
    // the backend declares none, and then no row offers a link.
    FirmwareReleasesDialog(const QStringList& releases,
                           const QString& installedVersion,
                           const QString& releaseNotesUrlTemplate,
                           QWidget* parent = nullptr);

private:
    void openReleaseNotesFor(const QString& version);
    QString selectedRelease() const;

    QString      m_releaseNotesUrlTemplate;
    QListWidget* m_list{nullptr};
    QPushButton* m_notesBtn{nullptr};
};

} // namespace AetherSDR
