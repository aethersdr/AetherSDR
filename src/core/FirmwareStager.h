#pragma once

#include <QMap>
#include <QStringList>
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace AetherSDR {

// Downloads SmartSDR installer, verifies integrity, extracts .ssdr firmware
// files, and stages them for upload.
//
// Workflow:
//   1. checkForUpdate()  — compare radio version vs latest available
//   2. downloadAndStage() — download installer, verify MD5, extract .ssdr
//   3. stagedFilePath()   — path to extracted .ssdr ready for upload

class FirmwareStager : public QObject {
    Q_OBJECT
public:
    explicit FirmwareStager(QObject* parent = nullptr);

    // Fetch the latest SmartSDR version FlexRadio publishes, and judge nothing.
    // Answers `latestVersionKnown` or `latestVersionUnavailable` exactly once.
    //
    // Separate from checkForUpdate() because the two have different questions:
    // this one asks "what is published?", which needs no radio and is safe to
    // ask before one is connected. checkForUpdate() asks "is THIS radio behind?"
    // and is built on top of it.
    void fetchLatestVersion();

    // Check FlexRadio website for latest version
    void checkForUpdate(const QString& currentVersion);

    // The newest SmartSDR release NAMED ON THE PAGE FOR EACH MAJOR LINE, keyed
    // by major: {2: "2.10.1", 3: "3.10.15", 4: "4.2.20"} on today's index. Empty
    // when the index names none.
    //
    // Grouped rather than reduced to one maximum because FlexRadio offers
    // several lines for download at once, and a radio belongs to exactly one of
    // them. Exposed for tests: the index is the untrusted input this class
    // exists to read (Principle VII), and its parse is the part worth pinning
    // without a network.
    //
    // `json` is FlexRadio's WordPress REST response: an ARRAY of software
    // records. Each release is its own record, so there is no "biggest number
    // on the page" heuristic here and no HTML to spell-match.
    static QMap<int, QString> parsePublishedReleases(const QByteArray& json);

    // EVERY SmartSDR release the index names, newest first: {"4.2.20",
    // "4.2.18", "4.1.5", "3.10.15", "2.10.1"} today. The same records and the
    // same rules as parsePublishedReleases(), which is derived from this --
    // what differs is only that nothing is discarded for being superseded.
    //
    // Radio Setup lists these so an operator can see what FlexRadio offers;
    // the status bar does not use it, because a verdict needs one target per
    // line and not a catalogue.
    static QStringList parseAllReleases(const QByteArray& json);

    // Download installer, verify, extract .ssdr for the given model family
    // modelFamily: "6x00" or "9600"
    void downloadAndStage(const QString& version, const QString& modelFamily);

    // Stage firmware from an installer file the user has already downloaded.
    // Accepts .msi (v4.2+), .exe (v4.1.x and earlier), or .ssdr (no extraction
    // needed; passed straight through to staging). Version is parsed from the
    // filename when present, otherwise stays empty until the radio confirms.
    void stageFromLocalFile(const QString& installerPath, const QString& modelFamily);

    // Cancel in-progress download
    void cancel();

    // Path to the staged .ssdr file (empty if not staged)
    QString stagedFilePath() const { return m_stagedPath; }
    QString stagedVersion()  const { return m_stagedVersion; }
    bool    isStaged()       const { return !m_stagedPath.isEmpty(); }

    // Map radio model string to firmware model family
    static QString modelToFamily(const QString& model);

    // Staging directory
    static QString stagingDir();

signals:
    // Step 0: what does FlexRadio publish? (fetchLatestVersion)
    void publishedReleasesKnown(const QMap<int, QString>& newestByMajor);
    void latestVersionUnavailable(const QString& reason);

    // Step 1: version check
    void updateCheckComplete(const QString& latestVersion, bool updateAvailable,
                             const QStringList& publishedReleases);
    void updateCheckFailed(const QString& error);

    // Steps 2-4: download, verify, extract
    void stageProgress(int percent, const QString& status);
    void stageComplete(const QString& ssdrPath, const QString& version);
    void stageFailed(const QString& error);

private:
    void onInstallerDownloadProgress(qint64 received, qint64 total);
    void onInstallerDownloadFinished();
    void verifyAndExtract();

    // Format-specific extractors. Both produce a .ssdr file at outPath and
    // emit progress/failed signals on the way.
    bool extractFromInnoSetup(const QByteArray& data, const QString& outPath);
    bool extractFromMsi(const QString& msiPath, const QString& outPath);

    // Returns true if the version uses the WiX MSI installer (v4.2+) instead
    // of the older InnoSetup .exe.
    static bool versionUsesMsi(const QString& version);

    // One GET of the software index, one parse, one answer. Both public entry
    // points wrap this so the index is read and validated in exactly one place.
    // `done` receives the parsed version, or an empty string plus a reason.
    void requestPublishedReleases(
        std::function<void(const QMap<int, QString>& newestByMajor,
                           const QStringList& allReleases,
                           const QString& error)> done);

    // Refuse to PARSE an index larger than this. QNetworkReply has already
    // buffered the body by the time this is checked, so it bounds the work we
    // do on a hostile answer, not the bytes we accept -- bounding those needs a
    // streaming read this class does not do.
    //
    // 1 MiB against a real answer of ~2 KB: `_fields` trims each record to its
    // title and slug, and `per_page=100` caps how many there can be.
    static constexpr qint64 kMaxSoftwareIndexBytes = 1 * 1024 * 1024;

    QNetworkAccessManager m_nam;
    QNetworkReply*  m_downloadReply{nullptr};
    QString         m_installerPath;
    QString         m_expectedMd5;
    QString         m_modelFamily;
    QString         m_targetVersion;
    QString         m_stagedPath;
    QString         m_stagedVersion;
    bool            m_cancelled{false};

    // v4.1.x and earlier: InnoSetup self-extracting .exe.
    // v4.2+: WiX 6 MSI (OLE Compound File with embedded LZX-compressed CABs).
    static constexpr const char* INSTALLER_URL_FMT_EXE =
        "https://smartsdr.flexradio.com/SmartSDR_v%1_Installer.exe";
    static constexpr const char* INSTALLER_URL_FMT_MSI =
        "https://smartsdr.flexradio.com/SmartSDR_v%1_x64.msi";
    static constexpr const char* MD5_URL_FMT =
        "https://edge.flexradio.com/www/offload/20251215133656/"
        "SmartSDR-v%1-Installer-MD5-Hash-File.txt";
    // FlexRadio's own WordPress REST index of published software -- what
    // SmartSDR and Maestro read. Structured records rather than the marketing
    // page at /software/, which this used to scrape.
    //
    // `search=SmartSDR` narrows to the SmartSDR records (it matches body text
    // too, so fonts come back and the parse filters by title). `per_page=100`
    // takes the whole set in one request: x-wp-total is 15 today, so there is
    // no pagination to follow. `_fields` trims each record to what is read,
    // which is the difference between a ~2 KB answer and a ~43 KB one.
    static constexpr const char* SOFTWARE_INDEX_URL =
        "https://www.flexradio.com/wp-json/wp/v2/software"
        "?search=SmartSDR&per_page=100&_fields=slug,title";
};

} // namespace AetherSDR
