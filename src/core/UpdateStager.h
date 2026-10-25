#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <atomic>
#include <functional>
#include <memory>

namespace AetherSDR {

class CappedDownload;

// The signed in-app update manifest (RFC #5304), schema 1, as
// docs/architecture/update-manifest.md defines it. parse() is strict: a
// manifest that breaks any rule there is refused whole.
struct UpdateManifest {
    struct Asset {
        QString name;
        QUrl url;
        QString sha256;   // 64 lowercase hex digits
        qint64 size{0};
    };
    struct File {
        QString path;
        bool symlink{false};
        QString target;   // symlink only
        qint64 size{0};
        QString sha256;
        bool executable{false};
    };
    struct Op {
        enum class Kind { Remove, Symlink, Patch, Add };
        Kind kind{Kind::Remove};
        QString path;
        QString target;       // Symlink
        QString member;       // Patch, Add: the bundle member
        int windowLog{0};     // Patch, Add
        QString sha256;       // Patch, Add: the result
        qint64 size{0};       // Patch, Add: the result
        bool executable{false};
        QString fromSha256;   // Patch: the file it applies to
    };
    struct Patch {
        QString from;   // the version it applies to, "26.10.2"
        Asset bundle;
        QList<Op> ops;
    };
    struct Payload {
        QString platform;   // "windows", "linux"
        QString arch;       // QSysInfo::buildCpuArchitecture()'s name
        Asset full;
        QList<File> files;
        QList<Patch> patches;
    };

    int schema{0};
    QString tag;       // "v26.10.3"
    QString version;   // "26.10.3"
    QDateTime generatedAt;
    QString keyId;
    QList<Payload> payloads;

    static bool parse(const QByteArray& json, UpdateManifest* out, QString* problem);
};

// Takes AetherSDR from the running release to a newer one by applying a
// signed manifest's per-file patches into a staging copy of the install.
// Engine only (RFC #5304): it never starts on its own, never touches the
// running install, and swapping the staged tree in is the platform phases'
// job. With Request::fullFallback it stages the full release when no patch
// applies, and every staged release keeps a record beside it
// (stagedRelease(), revalidate()).
//
// stage() fetches the manifest and its signature, verifies the signature
// against the embedded update key before reading a byte of the JSON, refuses
// a manifest that is not newer than this build, is older than one already
// accepted (a replay), or names another release than GitHub's latest (a
// frozen latest/), then downloads this build's bundle, checks its size and
// SHA-256, copies the install into stagingRoot and applies the ops there.
// Every file of the new release is checked before staged() is emitted.
class UpdateStager : public QObject {
    Q_OBJECT

public:
    struct Request {
        QString runningVersion;    // "26.10.2"
        QString platform;          // "windows", "linux"
        QString arch;              // QSysInfo::buildCpuArchitecture()
        QString githubLatestTag;   // releases/latest's tag_name, from UpdateChecker
        QString installRoot;       // the tree the running build was installed as
        QString stagingRoot;       // per-user app data, outside the install
        QUrl manifestUrl;          // kManifestUrl unless a test says otherwise
        // When no patch applies (or one fails), stage the full release
        // instead: the signed portable zip's files plus the installer's own
        // (unins*). Windows: its full download is that tree. Otherwise
        // fullDownloadNeeded() is emitted, as before.
        bool fullFallback = false;
    };

    // An update staged by an earlier run (or this one), waiting for the swap.
    struct StagedRelease {
        QString version;   // empty: nothing usable is staged
        QString dir;       // <stagingRoot>/<version>
        QString from;      // the version it was staged for
    };

    // What choose() decided for a verified manifest.
    enum class Decision { Patch, FullDownload, NotNewer, Replay, NotLatest, NoPayload };

    static const char* const kManifestUrl;
    // Hard ceilings, whatever a manifest claims.
    static constexpr qint64 kMaxManifestBytes = 4 * 1024 * 1024;
    static constexpr qint64 kMaxBundleBytes = 256LL * 1024 * 1024;
    static constexpr qint64 kMaxFullBytes = 512LL * 1024 * 1024;
    static constexpr int kMaxWindowLog = 31;
    static constexpr int kMaxBundleMembers = 4096;

    explicit UpdateStager(QObject* parent = nullptr);
    ~UpdateStager() override;

    void stage(const Request& request);
    void cancel();
    bool isBusy() const;

    // The update signing key built in (Ed25519, 32 raw bytes), from
    // packaging/update-signing.pub.
    static QByteArray builtInKey();
    // builtInKey(), unless a test replaced it.
    static QList<QByteArray> trustedKeys();
    static void setTrustedKeysForTesting(const QList<QByteArray>& keys);
    // Tests: fetch "<base>/<tag>/<asset>" instead of the bundle's GitHub
    // URL (a file:// base keeps them off the network). The manifest's own
    // URLs are still checked as signed.
    static void setDownloadBaseForTesting(const QUrl& base);

    // A raw 64-byte Ed25519 signature of manifest by a trusted key.
    static bool verify(const QByteArray& manifest, const QByteArray& signature);

    // acceptedUtc: the newest generated_at already accepted (invalid: none).
    static Decision choose(const UpdateManifest& manifest, const Request& request,
                           const QDateTime& acceptedUtc, const UpdateManifest::Payload** payload,
                           const UpdateManifest::Patch** patch);

    // The members of a bundle: a ustar of flat NNNNNN.zst files. The members
    // are views into bundle, which must outlive them.
    static bool readBundle(const QByteArray& bundle, QHash<QString, QByteArray>* members,
                           QString* problem);

    // The full release from its portable zip into stagingDir: exactly the
    // files manifest lists, plus the installer's own files (unins*) copied
    // from installRoot, then every file checked. On any failure, or once
    // cancelled is set, stagingDir is removed.
    static bool applyFull(const QString& installRoot, const QByteArray& zip,
                          const QList<UpdateManifest::File>& files, const QString& stagingDir,
                          QString* problem, const std::atomic_bool* cancelled = nullptr);

    // The release staged in stagingRoot, if it is still the one to install:
    // staged for runningVersion, from installRoot, it is GitHub's latest
    // (githubLatestTag) and not the release that was rolled back. Otherwise
    // an empty StagedRelease. Whatever else staging wrote there is removed
    // (other versions' trees and records, half-written records, the work
    // folders a crashed run left); nothing else in the folder is touched,
    // since it is the swap helper's state folder too. Never call it while a
    // stager isBusy(), whose work folder it would remove. Just after
    // cancel() is fine: isBusy() is false then, and a worker still finishing
    // only removes its own work folder, which may already be gone.
    // An empty githubLatestTag (not known yet) discards nothing on that
    // ground. Cheap: reads the record only. revalidate() checks every file.
    static StagedRelease stagedRelease(const QString& stagingRoot, const QString& runningVersion,
                                       const QString& installRoot, const QString& githubLatestTag,
                                       const QString& rolledBackVersion);
    // Before the swap: the staged tree's signed manifest still verifies and
    // every file still matches it.
    // Removes a staged release and its record: skipped, or its files no
    // longer match. Nothing else in stagingRoot is touched.
    static void discard(const QString& stagingRoot, const QString& version);
    static bool revalidate(const QString& stagingRoot, const StagedRelease& staged,
                           const QString& platform, const QString& arch, QString* problem);

    // Copies installRoot into stagingDir, applies patch's ops there in order
    // and checks every entry of files. On any failure, or once cancelled is
    // set (checked between steps), stagingDir is removed.
    static bool applyPatch(const QString& installRoot, const QByteArray& bundle,
                           const UpdateManifest::Patch& patch,
                           const QList<UpdateManifest::File>& files, const QString& stagingDir,
                           QString* problem, const std::atomic_bool* cancelled = nullptr);

signals:
    void progress(const QString& step, qint64 done, qint64 total);
    // The new release, complete and checked, in stagedDir.
    void staged(const QString& stagedDir, const QString& version);
    // No patch could be applied; the caller may offer the full download.
    void fullDownloadNeeded(const QString& reason, const AetherSDR::UpdateManifest::Asset& full,
                            const QString& version);
    // The running build is already the newest release.
    void upToDate();
    // The manifest could not be trusted or fetched.
    void failed(const QString& reason);

private:
    enum class Stage { Idle, Manifest, Signature, Bundle, Full };

    // Applies (on a worker thread) and, if this request is still current,
    // puts the result in <stagingRoot>/<version> beside its record.
    void runWorker(std::function<QString(const QString& work)> apply);
    // No patch applies or it failed: the full release when the request
    // allows it, otherwise fullDownloadNeeded().
    void fallBack(const QString& reason);

    void onDownloaded(const QUrl& url, const QByteArray& body);
    void fail(const QString& reason);

    CappedDownload* m_download;
    Stage m_stage{Stage::Idle};
    // Every stage() and cancel() starts a new generation: a worker's result
    // is used only if its generation is still the current one.
    quint64 m_generation{0};
    std::shared_ptr<std::atomic_bool> m_cancelled;
    Request m_request;
    QByteArray m_manifestBytes;
    QByteArray m_signature;
    UpdateManifest m_manifest;
    const UpdateManifest::Payload* m_payload{nullptr};
    const UpdateManifest::Patch* m_patch{nullptr};
};

} // namespace AetherSDR
