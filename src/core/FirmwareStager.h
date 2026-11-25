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
    // An empty directory uses stagingDir(); callers can supply an isolated one.
    explicit FirmwareStager(QObject* parent = nullptr, const QString& stagingDirectory = {});

    // Fetch the SmartSDR releases FlexRadio publishes, and judge nothing.
    // Answers `publishedReleasesKnown` or `latestVersionUnavailable` exactly once.
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
    // line and not a full list.
    static QStringList parseAllReleases(const QByteArray& json);

    // ─── Finding FlexRadio's MD5 for one release (#7421) ────────────────────
    //
    // FlexRadio uploads each release's hash file into its own timestamped
    // folder and attaches it to that release's software record. Finding it is
    // therefore three reads, each a pure function of untrusted input
    // (Principle VII), exposed so the tests can pin them without a network:
    //
    //   software index --releasePostId()--> post id
    //   media?parent=id --selectMd5HashUrl()--> hash-file URL
    //   hash file --parseMd5HashFile()--> MD5 of the installer we download
    //
    // Every step matches the release EXACTLY and answers empty rather than
    // guess: a release is never verified against another release's hash.

    // The WordPress post id of the record for exactly `version` ("4.3.19"),
    // or 0 when the index names no such release. Uses the same title/slug
    // rules as parseAllReleases(), so "a release" means one thing.
    static qint64 releasePostId(const QByteArray& indexJson, const QString& version);

    // The URL of `version`'s hash file among a release record's media
    // attachments, or empty. `arch` is the installer's architecture tag
    // ("x64"), or empty for pre-4.2 .exe releases, whose hash file has none.
    // The file name must match exactly (case aside: 4.2.18 spells "ARM64"),
    // the URL must be https on flexradio.com, and two different matches are
    // treated as no match.
    static QString selectMd5HashUrl(const QByteArray& mediaJson, const QString& version,
                                    const QString& arch);

    // The lowercase MD5 the hash file records for `installerFileName`, or
    // empty. The file names the installer it covers ("[001/001]
    // SmartSDR_v4.3.19_x64.msi"), and that name must be the one we download,
    // so the hash and the installer cannot drift apart.
    static QString parseMd5HashFile(const QByteArray& body, const QString& installerFileName);

    // The installer file name for `version`, as FlexRadio publishes it and as
    // it is cached in the staging directory.
    static QString installerFileName(const QString& version);

    // Download installer, verify, extract .ssdr for the given model family
    // modelFamily: "6x00" or "9600"
    void downloadAndStage(const QString& version, const QString& modelFamily);

    // Stage firmware from an installer file the user has already downloaded.
    // Accepts .msi (v4.2+), .exe (v4.1.x and earlier), or .ssdr (no extraction
    // needed; its filename is preserved). Version is parsed from the filename
    // when present, otherwise reported as "unknown".
    void stageFromLocalFile(const QString& installerPath, const QString& modelFamily);

    // Cancel in-progress download
    void cancel();

    // Path to the staged .ssdr file (empty if not staged)
    QString stagedFilePath() const { return m_stagedPath; }
    QString stagedVersion()  const { return m_stagedVersion; }
    bool    isStaged()       const { return !m_stagedPath.isEmpty(); }
    // True only when the staged installer's MD5 matched FlexRadio's
    // published hash; false when no hash was available or for a local file.
    bool    stagedVerified() const { return m_stagedVerified; }

    // Map the FlexLib hardware platform to its installer payload name.
    // BigBend (8000/Aurora) uses "9600"; unverified platforms return empty.
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
    QString stagingDirectory() const;
    void onInstallerDownloadProgress(qint64 received, qint64 total);
    void onInstallerDownloadFinished();
    void verifyAndExtract();

    // Format-specific extractors emit progress/failed signals on the way.
    bool extractFromInnoSetup(const QByteArray& data, const QString& outPath);
    // Preserve the MSI FileName and return its staged path after saving.
    bool extractFromMsi(const QString& msiPath, const QString& outputDirectory,
                        QString& extractedPath);

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
    // A real hash file is ~250 bytes.
    static constexpr qint64 kMaxHashFileBytes = 64 * 1024;

    // GET `url` and hand back its body, or an empty body and a reason. Refuses
    // a body over `maxBytes`. Drops the answer if a newer download, or a
    // cancel, has begun since the request was made.
    void getBounded(const QUrl& url, qint64 maxBytes,
                    std::function<void(const QByteArray& body, const QString& error)> done);

    // Steps 2a-2c: index -> media -> hash file. `done` receives the MD5, or
    // an empty string plus the step that failed.
    void fetchExpectedMd5(const QString& version,
                          std::function<void(const QString& md5, const QString& error)> done);
    // Step 2d: use the cached installer if it matches, else download it.
    void downloadInstaller(const QString& version);

    QNetworkAccessManager m_nam;
    QString         m_stagingDirectory;
    QNetworkReply*  m_downloadReply{nullptr};
    QString         m_installerPath;
    QString         m_expectedMd5;
    QString         m_modelFamily;
    QString         m_targetVersion;
    QString         m_stagedPath;
    QString         m_stagedVersion;
    // Why the hash is missing, for the status line; empty when it was found.
    QString         m_md5Unavailable;
    bool            m_cancelled{false};
    bool            m_stagedVerified{false};
    // Bumped by every downloadAndStage() and cancel(), so an answer to an
    // earlier attempt cannot act on a later one.
    quint64         m_downloadGeneration{0};

    // The one architecture the updater downloads. It names the MSI, the cached
    // file and the hash file, so the three cannot drift apart (#7421).
    static constexpr const char* kInstallerArch = "x64";

    // v4.1.x and earlier: InnoSetup self-extracting .exe.
    // v4.2+: WiX 6 MSI (OLE Compound File with embedded LZX-compressed CABs).
    // Both are installerFileName() under this host.
    static constexpr const char* INSTALLER_URL_BASE = "https://smartsdr.flexradio.com/";
    // The media attached to one software record: for v4.3.19 its x64 and
    // arm64 hash files and the FlexLib zip.
    static constexpr const char* MEDIA_URL_FMT =
        "https://www.flexradio.com/wp-json/wp/v2/media"
        "?parent=%1&per_page=100&_fields=source_url";
    // FlexRadio's own WordPress REST index of published software -- what
    // SmartSDR and Maestro read. Structured records rather than the marketing
    // page at /software/, which this used to scrape.
    //
    // `search=SmartSDR` narrows to the SmartSDR records (it matches body text
    // too, so fonts come back and the parse filters by title). `per_page=100`
    // takes the whole set in one request: x-wp-total is 15 today, so there is
    // no pagination to follow. `_fields` trims each record to what is read,
    // which is the difference between a ~2 KB answer and a ~43 KB one. `id`
    // is what the media lookup for a release's hash file needs.
    static constexpr const char* SOFTWARE_INDEX_URL =
        "https://www.flexradio.com/wp-json/wp/v2/software"
        "?search=SmartSDR&per_page=100&_fields=id,slug,title";
};

} // namespace AetherSDR
