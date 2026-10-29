#include "UpdateStager.h"

#include "CappedDownload.h"
#include "LogManager.h"
#include "UpdateChecker.h"
#include "UpdateSettings.h"
#include "VersionNumber.h"
#include "ZipArchive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include "monocypher-ed25519.h"
#include "zstd.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;

namespace AetherSDR {

const char* const UpdateStager::kManifestUrl = "https://updates.aethersdr.com/latest/manifest.json";

namespace {

// Every URL a manifest may name: a download of the release it describes, on
// the one repository the app trusts (UpdateChecker::isTrustedDownloadUrl).
const QString kDownloadBase = QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/download/");

// packaging/update-signing.pub's raw Ed25519 key (key_id 788fb6e31f7ecba4).
// update_stager_test checks it against that file.
const unsigned char kUpdateSigningKey[32] = {
    0xe2, 0x73, 0x23, 0x65, 0xae, 0x0a, 0xb4, 0x35, 0x4a, 0x54, 0x05, 0x55, 0x3b, 0x86, 0x93, 0x5e,
    0xc3, 0xc0, 0x82, 0xdd, 0x76, 0xfe, 0x24, 0x8a, 0xc2, 0x3b, 0x19, 0x2d, 0x56, 0xe8, 0x83, 0x84,
};

QList<QByteArray>& testKeys()
{
    static QList<QByteArray> keys;
    return keys;
}
bool& testKeysSet()
{
    static bool set = false;
    return set;
}
QUrl& testDownloadBase()
{
    static QUrl base;
    return base;
}

constexpr int kTarBlock = 512;
constexpr qint64 kChunk = 1 << 20;

const QRegularExpression& hexSha()
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re;
}

// A release version, "26.10.3" or "26.10.3.1": the tag without its "v", the
// name of a staged tree, and the stem of its records.
const QRegularExpression& releaseVersion()
{
    static const QRegularExpression re(QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+(\\.[0-9]+)?$"));
    return re;
}

const QRegularExpression& memberName()
{
    static const QRegularExpression re(QStringLiteral("^[0-9]{6}\\.zst$"));
    return re;
}

// Relative, '/'-separated, inside the tree: no root, drive, backslash, '.',
// '..' or empty segment.
bool safePath(const QString& path)
{
    if (path.isEmpty() || path.size() > 1024 || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\')) || path.contains(QLatin1Char(':'))
        || path.contains(QChar(0))) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString& part : parts) {
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) {
            return false;
        }
    }
    return true;
}

// A link the manifest may carry: relative and resolving, lexically from the
// link's own directory, inside the tree.
bool linkStaysInside(const QString& path, const QString& target)
{
    if (target.isEmpty() || target.startsWith(QLatin1Char('/')) || target.contains(QLatin1Char('\\'))
        || target.contains(QLatin1Char(':'))) {
        return false;
    }
    int depth = path.count(QLatin1Char('/'));
    for (const QString& part : target.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String("..")) {
            if (--depth < 0) {
                return false;
            }
        } else if (part != QLatin1String(".")) {
            ++depth;
        }
    }
    return true;
}

bool readAsset(const QJsonObject& o, const QString& tag, UpdateManifest::Asset* a)
{
    a->name = o.value(QLatin1String("asset")).toString();
    a->url = QUrl(o.value(QLatin1String("url")).toString(), QUrl::StrictMode);
    a->sha256 = o.value(QLatin1String("sha256")).toString();
    a->size = o.value(QLatin1String("size")).toInteger(-1);
    const QString url = a->url.toString();
    return a->url.isValid() && UpdateChecker::isTrustedDownloadUrl(a->url)
           && url.startsWith(kDownloadBase + tag + QLatin1Char('/'))
           && !url.mid(kDownloadBase.size() + tag.size() + 1).contains(QLatin1Char('/'))
           && !a->url.hasQuery() && !a->url.hasFragment() && hexSha().match(a->sha256).hasMatch()
           && a->size >= 0;
}

QString sha256Of(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

QString native(const fs::path& p)
{
    return QString::fromStdU16String(p.u16string());
}

fs::path toPath(const QString& s)
{
    return fs::path(s.toStdU16String());
}

// No component between root and the entry may be a symlink: a write is
// never routed through a link.
bool noLinkOnTheWay(const fs::path& root, const QString& rel)
{
    fs::path at = root;
    const QStringList parts = rel.split(QLatin1Char('/'));
    for (int i = 0; i + 1 < parts.size(); ++i) {
        at /= toPath(parts[i]);
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(at, ec))) {
            return false;
        }
    }
    return true;
}

void setExecutable(const fs::path& p, bool executable)
{
#ifndef Q_OS_WIN
    std::error_code ec;
    const auto x = fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
    fs::permissions(p, x, executable ? fs::perm_options::add : fs::perm_options::remove, ec);
#else
    Q_UNUSED(p);
    Q_UNUSED(executable);
#endif
}

// One zstd frame into outPath, against prefix when given (--patch-from),
// with the window limited to windowLog and the output to exactly size bytes
// hashing to sha256.
bool decode(const QByteArray& frame, const uchar* prefix, size_t prefixSize, int windowLog,
            qint64 size, const QString& sha256, QFileDevice& out, QString* problem)
{
    std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!dctx || ZSTD_isError(ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, windowLog))) {
        *problem = QStringLiteral("zstd could not be set up");
        return false;
    }
    if (prefix && ZSTD_isError(ZSTD_DCtx_refPrefix(dctx.get(), prefix, prefixSize))) {
        *problem = QStringLiteral("zstd refused the base file");
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buf(kChunk, Qt::Uninitialized);
    ZSTD_inBuffer in{frame.constData(), static_cast<size_t>(frame.size()), 0};
    qint64 written = 0;
    size_t ret = 1;
    while (ret != 0) {
        ZSTD_outBuffer o{buf.data(), static_cast<size_t>(buf.size()), 0};
        const size_t inBefore = in.pos;
        ret = ZSTD_decompressStream(dctx.get(), &o, &in);
        if (ZSTD_isError(ret)) {
            *problem = QStringLiteral("corrupt patch data: %1").arg(QString::fromLatin1(ZSTD_getErrorName(ret)));
            return false;
        }
        written += static_cast<qint64>(o.pos);
        if (written > size) {
            *problem = QStringLiteral("patch output exceeds its declared size");
            return false;
        }
        hash.addData(QByteArrayView(buf.constData(), static_cast<qsizetype>(o.pos)));
        if (out.write(buf.constData(), static_cast<qint64>(o.pos)) != static_cast<qint64>(o.pos)) {
            *problem = QStringLiteral("cannot write the new file");
            return false;
        }
        if (ret != 0 && o.pos == 0 && in.pos == inBefore && in.pos == in.size) {
            *problem = QStringLiteral("patch data is cut short");
            return false;
        }
    }
    if (in.pos != in.size) {
        *problem = QStringLiteral("patch data has trailing bytes");
        return false;
    }
    if (!out.flush()) {
        *problem = QStringLiteral("cannot write the new file");
        return false;
    }
    if (written != size || QString::fromLatin1(hash.result().toHex()) != sha256) {
        *problem = QStringLiteral("patched file does not match its hash");
        return false;
    }
    return true;
}

// After the removals, the folders they left empty go, deepest first, each
// parent after its children. A folder that was already empty in the install
// is not the removals' and is left alone.
void pruneEmptiedDirs(const fs::path& root, std::vector<fs::path> touched)
{
    std::sort(touched.begin(), touched.end(), [](const fs::path& a, const fs::path& b) {
        return std::distance(a.begin(), a.end()) > std::distance(b.begin(), b.end());
    });
    std::error_code ec;
    for (fs::path d : touched) {
        // Walk up from each removed entry's folder while the folders empty.
        while (d != root && d.has_relative_path() && fs::is_directory(fs::symlink_status(d, ec))
               && fs::is_empty(d, ec)) {
            fs::remove(d, ec);
            d = d.parent_path();
        }
    }
}

bool applyOps(const fs::path& stage, const QHash<QString, QByteArray>& members,
              const UpdateManifest::Patch& patch, const std::atomic_bool* cancelled, QString* problem)
{
    bool pruned = false;
    std::vector<fs::path> emptied;
    std::error_code ec;
    for (const UpdateManifest::Op& op : patch.ops) {
        if (op.kind != UpdateManifest::Op::Kind::Remove && !pruned) {
            pruneEmptiedDirs(stage, emptied);
            pruned = true;
        }
        if (cancelled && cancelled->load()) {
            *problem = QStringLiteral("cancelled");
            return false;
        }
        const fs::path target = stage / toPath(op.path);
        if (!noLinkOnTheWay(stage, op.path)) {
            *problem = QStringLiteral("%1: a parent folder is a link").arg(op.path);
            return false;
        }
        if (op.kind == UpdateManifest::Op::Kind::Remove) {
            if (!fs::remove(target, ec) || ec) {
                *problem = QStringLiteral("%1: not in the install, or cannot be removed").arg(op.path);
                return false;
            }
            emptied.push_back(target.parent_path());
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (op.kind == UpdateManifest::Op::Kind::Symlink) {
            if (fs::is_symlink(fs::symlink_status(target, ec)) || fs::exists(target, ec)) {
                fs::remove(target, ec);
            }
            fs::create_symlink(toPath(op.target), target, ec);
            if (ec) {
                *problem = QStringLiteral("%1: cannot create the link").arg(op.path);
                return false;
            }
            continue;
        }
        const auto member = members.constFind(op.member);
        if (member == members.cend()) {
            *problem = QStringLiteral("%1: its patch data is missing from the bundle").arg(op.path);
            return false;
        }
        const QByteArray& frame = *member;
        // The new bytes go to a file created here, exclusively and under a
        // random name, never to a predictable one: an unlisted link copied
        // from the install could otherwise be followed out of staging.
        QTemporaryFile out(native(target.parent_path()) + QStringLiteral("/.update-XXXXXX"));
        if (!out.open()) {
            *problem = QStringLiteral("%1: cannot write the new file").arg(op.path);
            return false;
        }
        QString why;
        if (op.kind == UpdateManifest::Op::Kind::Patch) {
            if (fs::is_symlink(fs::symlink_status(target, ec)) || !fs::is_regular_file(target, ec)
                || sha256Of(native(target)) != op.fromSha256) {
                *problem = QStringLiteral("%1: the installed file is not the one this update starts from")
                               .arg(op.path);
                return false;
            }
            QFile base(native(target));
            if (!base.open(QIODevice::ReadOnly)) {
                *problem = QStringLiteral("%1: cannot be read").arg(op.path);
                return false;
            }
            const uchar* mapped = base.size() > 0 ? base.map(0, base.size()) : nullptr;
            if (base.size() > 0 && !mapped) {
                *problem = QStringLiteral("%1: cannot be read").arg(op.path);
                return false;
            }
            const bool ok = decode(frame, mapped, static_cast<size_t>(base.size()), op.windowLog,
                                   op.size, op.sha256, out, &why);
            base.close();
            if (!ok) {
                *problem = op.path + QStringLiteral(": ") + why;
                return false;
            }
        } else if (!decode(frame, nullptr, 0, op.windowLog, op.size, op.sha256, out, &why)) {
            *problem = op.path + QStringLiteral(": ") + why;
            return false;
        }
        out.close();
        if (fs::is_symlink(fs::symlink_status(target, ec))) {
            fs::remove(target, ec);
        }
        fs::rename(toPath(out.fileName()), target, ec);
        if (!ec) {
            out.setAutoRemove(false);   // it is the target now
        }
        if (ec) {
            *problem = QStringLiteral("%1: cannot be replaced").arg(op.path);
            return false;
        }
        setExecutable(target, op.executable);
    }
    if (!pruned) {
        pruneEmptiedDirs(stage, emptied);   // a patch of removals only
    }
    return true;
}

QString recordPath(const QString& stagingRoot, const QString& version, const QString& suffix)
{
    return QDir(stagingRoot).filePath(version + suffix);
}

// The install exists, and it and the staging folder do not overlap: staging
// inside the install would copy into itself; the install inside staging
// would be wiped by a failure.
bool checkRoots(const QString& installRoot, const QString& stagingDir, fs::path* install,
                fs::path* stage, QString* problem)
{
    std::error_code ec;
    *install = fs::weakly_canonical(toPath(installRoot), ec);
    *stage = fs::weakly_canonical(toPath(stagingDir), ec);
    if (installRoot.isEmpty() || !fs::is_directory(*install, ec)) {
        *problem = QStringLiteral("There is no install folder to update.");
        return false;
    }
    const auto inside = [](const fs::path& a, const fs::path& b) {
        const auto m = std::mismatch(b.begin(), b.end(), a.begin(), a.end());
        return m.first == b.end();
    };
    if (inside(*stage, *install) || inside(*install, *stage)) {
        *problem = QStringLiteral("The staging folder and the install overlap.");
        return false;
    }
    return true;
}

bool checkTree(const fs::path& stage, const QList<UpdateManifest::File>& files, QString* problem)
{
    std::error_code ec;
    for (const UpdateManifest::File& f : files) {
        const fs::path p = stage / toPath(f.path);
        if (!noLinkOnTheWay(stage, f.path)) {
            *problem = QStringLiteral("%1: a parent folder is a link").arg(f.path);
            return false;
        }
        const fs::file_status st = fs::symlink_status(p, ec);
        if (f.symlink) {
            if (!fs::is_symlink(st) || QString::fromStdU16String(fs::read_symlink(p, ec).generic_u16string()) != f.target) {
                *problem = QStringLiteral("%1: is not the expected link").arg(f.path);
                return false;
            }
            continue;
        }
        if (!fs::is_regular_file(st) || static_cast<qint64>(fs::file_size(p, ec)) != f.size
            || sha256Of(native(p)) != f.sha256) {
            *problem = QStringLiteral("%1: does not match the new release").arg(f.path);
            return false;
        }
#ifndef Q_OS_WIN
        const bool executable = (st.permissions() & fs::perms::owner_exec) != fs::perms::none;
        if (executable != f.executable) {
            *problem = QStringLiteral("%1: its executable bit does not match the new release").arg(f.path);
            return false;
        }
#endif
    }
    return true;
}

} // namespace

// ── Manifest ────────────────────────────────────────────────────────────────

bool UpdateManifest::parse(const QByteArray& json, UpdateManifest* out, QString* problem)
{
    const auto bad = [problem](const QString& why) {
        *problem = QStringLiteral("The update manifest is not valid: %1.").arg(why);
        return false;
    };
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return bad(QStringLiteral("not JSON"));
    }
    const QJsonObject root = doc.object();
    UpdateManifest m;
    m.schema = root.value(QLatin1String("schema")).toInt(0);
    if (m.schema != 1) {
        return bad(QStringLiteral("unknown schema %1").arg(m.schema));
    }
    m.tag = root.value(QLatin1String("tag")).toString();
    m.version = root.value(QLatin1String("version")).toString();
    if (!m.tag.startsWith(QLatin1Char('v')) || !releaseVersion().match(m.tag.mid(1)).hasMatch()
        || m.version != m.tag.mid(1)) {
        return bad(QStringLiteral("tag and version"));
    }
    m.generatedAt = QDateTime::fromString(root.value(QLatin1String("generated_at")).toString(), Qt::ISODate);
    m.keyId = root.value(QLatin1String("key_id")).toString();
    if (!m.generatedAt.isValid() || m.keyId.isEmpty()) {
        return bad(QStringLiteral("generated_at and key_id"));
    }
    for (const QJsonValue& pv : root.value(QLatin1String("payloads")).toArray()) {
        const QJsonObject po = pv.toObject();
        UpdateManifest::Payload p;
        p.platform = po.value(QLatin1String("platform")).toString();
        p.arch = po.value(QLatin1String("arch")).toString();
        if (p.platform.isEmpty() || p.arch.isEmpty()
            || !readAsset(po.value(QLatin1String("full")).toObject(), m.tag, &p.full)) {
            return bad(QStringLiteral("a payload's full download"));
        }
        QSet<QString> paths;
        for (const QJsonValue& fv : po.value(QLatin1String("files")).toArray()) {
            const QJsonObject fo = fv.toObject();
            UpdateManifest::File f;
            f.path = fo.value(QLatin1String("path")).toString();
            const QString type = fo.value(QLatin1String("type")).toString();
            if (!safePath(f.path) || paths.contains(f.path)) {
                return bad(QStringLiteral("file path %1").arg(f.path));
            }
            paths.insert(f.path);
            if (type == QLatin1String("symlink")) {
                f.symlink = true;
                f.target = fo.value(QLatin1String("target")).toString();
                if (!linkStaysInside(f.path, f.target)) {
                    return bad(QStringLiteral("link %1 leaves the install").arg(f.path));
                }
            } else if (type == QLatin1String("file")) {
                f.size = fo.value(QLatin1String("size")).toInteger(-1);
                f.sha256 = fo.value(QLatin1String("sha256")).toString();
                f.executable = fo.value(QLatin1String("executable")).toBool();
                if (f.size < 0 || !hexSha().match(f.sha256).hasMatch()) {
                    return bad(QStringLiteral("file %1").arg(f.path));
                }
            } else {
                return bad(QStringLiteral("file type %1").arg(type));
            }
            p.files.append(f);
        }
        for (const QJsonValue& qv : po.value(QLatin1String("patches")).toArray()) {
            const QJsonObject qo = qv.toObject();
            UpdateManifest::Patch q;
            q.from = qo.value(QLatin1String("from")).toString();
            if (VersionNumber::parse(q.from).isNull()
                || !readAsset(qo.value(QLatin1String("bundle")).toObject(), m.tag, &q.bundle)) {
                return bad(QStringLiteral("a patch's bundle"));
            }
            bool writesSeen = false;
            QSet<QString> members;
            for (const QJsonValue& ov : qo.value(QLatin1String("ops")).toArray()) {
                const QJsonObject oo = ov.toObject();
                UpdateManifest::Op op;
                const QString kind = oo.value(QLatin1String("op")).toString();
                op.path = oo.value(QLatin1String("path")).toString();
                if (!safePath(op.path)) {
                    return bad(QStringLiteral("op path %1").arg(op.path));
                }
                if (kind == QLatin1String("remove")) {
                    if (writesSeen) {
                        return bad(QStringLiteral("a removal after a write"));
                    }
                    op.kind = UpdateManifest::Op::Kind::Remove;
                } else if (kind == QLatin1String("symlink")) {
                    op.kind = UpdateManifest::Op::Kind::Symlink;
                    op.target = oo.value(QLatin1String("target")).toString();
                    if (!linkStaysInside(op.path, op.target)) {
                        return bad(QStringLiteral("link %1 leaves the install").arg(op.path));
                    }
                } else if (kind == QLatin1String("patch") || kind == QLatin1String("add")) {
                    op.kind = kind == QLatin1String("patch") ? UpdateManifest::Op::Kind::Patch
                                                             : UpdateManifest::Op::Kind::Add;
                    op.member = oo.value(QLatin1String("member")).toString();
                    op.windowLog = oo.value(QLatin1String("window_log")).toInt(0);
                    op.sha256 = oo.value(QLatin1String("sha256")).toString();
                    op.size = oo.value(QLatin1String("size")).toInteger(-1);
                    op.executable = oo.value(QLatin1String("executable")).toBool();
                    op.fromSha256 = oo.value(QLatin1String("from_sha256")).toString();
                    if (!memberName().match(op.member).hasMatch() || members.contains(op.member)
                        || op.windowLog < 10 || op.windowLog > UpdateStager::kMaxWindowLog
                        || !hexSha().match(op.sha256).hasMatch() || op.size < 0
                        || (op.kind == UpdateManifest::Op::Kind::Patch
                            && !hexSha().match(op.fromSha256).hasMatch())) {
                        return bad(QStringLiteral("op %1").arg(op.path));
                    }
                    members.insert(op.member);
                } else {
                    return bad(QStringLiteral("op kind %1").arg(kind));
                }
                if (op.kind != UpdateManifest::Op::Kind::Remove) {
                    writesSeen = true;
                }
                q.ops.append(op);
            }
            p.patches.append(q);
        }
        m.payloads.append(p);
    }
    if (m.payloads.isEmpty()) {
        return bad(QStringLiteral("no payloads"));
    }
    *out = m;
    return true;
}

// ── Stager ──────────────────────────────────────────────────────────────────

UpdateStager::UpdateStager(QObject* parent)
    : QObject(parent)
    , m_download(new CappedDownload(this))
{
    connect(m_download, &CappedDownload::finished, this, &UpdateStager::onDownloaded);
    connect(m_download, &CappedDownload::failed, this, [this](const QString& reason) {
        // The manifest was trusted; only a release file could not be had
        // (gone, stalled, larger than signed). The full release may still work.
        if (m_stage == Stage::Bundle && m_payload) {
            qCWarning(lcUpdater).noquote() << "update bundle not downloaded:" << reason;
            fallBack(reason);
            return;
        }
        if (m_stage == Stage::Full && m_payload) {
            qCWarning(lcUpdater).noquote() << "full update not downloaded:" << reason;
            m_stage = Stage::Idle;
            emit fullDownloadNeeded(reason, m_payload->full, m_manifest.version);
            return;
        }
        fail(reason);
    });
    connect(m_download, &CappedDownload::progress, this, [this](qint64 done, qint64 total) {
        if (m_stage == Stage::Bundle || m_stage == Stage::Full) {
            emit progress(tr("Downloading the update"), done, total);
        }
    });
}

QByteArray UpdateStager::builtInKey()
{
    return QByteArray(reinterpret_cast<const char*>(kUpdateSigningKey), sizeof(kUpdateSigningKey));
}

QList<QByteArray> UpdateStager::trustedKeys()
{
    if (testKeysSet()) {
        return testKeys();
    }
    QList<QByteArray> keys{builtInKey()};
#if AETHER_UPDATE_DEV_OVERRIDES
    for (const QByteArray& hex : qgetenv("AETHER_UPDATE_TRUST_KEYS").split(',')) {
        const QByteArray key = QByteArray::fromHex(hex.trimmed());
        if (key.size() == 32) {
            qCWarning(lcUpdater).noquote() << "AETHER_UPDATE_TRUST_KEYS: also trusting update key"
                                           << QString::fromLatin1(key.toHex());
            keys << key;
        }
    }
#endif
    return keys;
}

void UpdateStager::setTrustedKeysForTesting(const QList<QByteArray>& keys)
{
    testKeys() = keys;
    testKeysSet() = true;
}

void UpdateStager::setDownloadBaseForTesting(const QUrl& base)
{
    testDownloadBase() = base;
}

bool UpdateStager::verify(const QByteArray& manifest, const QByteArray& signature)
{
    if (signature.size() != 64) {
        return false;
    }
    for (const QByteArray& key : trustedKeys()) {
        if (key.size() == 32
            && crypto_ed25519_check(reinterpret_cast<const uint8_t*>(signature.constData()),
                                    reinterpret_cast<const uint8_t*>(key.constData()),
                                    reinterpret_cast<const uint8_t*>(manifest.constData()),
                                    static_cast<size_t>(manifest.size()))
                   == 0) {
            return true;
        }
    }
    return false;
}

UpdateStager::Decision UpdateStager::choose(const UpdateManifest& manifest, const Request& request,
                                            const QDateTime& acceptedUtc,
                                            const UpdateManifest::Payload** payload,
                                            const UpdateManifest::Patch** patch)
{
    *payload = nullptr;
    *patch = nullptr;
    const VersionNumber running = VersionNumber::parse(request.runningVersion);
    if (!(VersionNumber::parse(manifest.version) > running)) {
        return Decision::NotNewer;
    }
    if (acceptedUtc.isValid() && manifest.generatedAt < acceptedUtc) {
        return Decision::Replay;
    }
    if (manifest.tag != request.githubLatestTag) {
        return Decision::NotLatest;
    }
    for (const UpdateManifest::Payload& p : manifest.payloads) {
        if (p.platform == request.platform && p.arch == request.arch) {
            *payload = &p;
            for (const UpdateManifest::Patch& q : p.patches) {
                if (VersionNumber::parse(q.from) == running) {
                    *patch = &q;
                    return Decision::Patch;
                }
            }
            return Decision::FullDownload;
        }
    }
    return Decision::NoPayload;
}

bool UpdateStager::readBundle(const QByteArray& bundle, QHash<QString, QByteArray>* members,
                              QString* problem)
{
    const auto broken = [problem](const QString& why) {
        *problem = QStringLiteral("The update bundle is damaged: %1.").arg(why);
        return false;
    };
    members->clear();
    qsizetype at = 0;
    while (at + kTarBlock <= bundle.size()) {
        const QByteArray header = bundle.mid(at, kTarBlock);
        if (header.count('\0') == kTarBlock) {
            return true;
        }
        quint32 sum = 0;
        for (int i = 0; i < kTarBlock; ++i) {
            sum += (i >= 148 && i < 156) ? quint32(' ') : quint32(quint8(header[i]));
        }
        bool ok = false;
        const quint32 stored = header.mid(148, 8).trimmed().replace('\0', "").toUInt(&ok, 8);
        if (!ok || stored != sum || !header.mid(257, 5).startsWith("ustar")) {
            return broken(QStringLiteral("bad header"));
        }
        const char type = header[156];
        const QString name = QString::fromLatin1(header.left(100).split('\0').first());
        if ((type != '0' && type != '\0') || header[345] != '\0' || !memberName().match(name).hasMatch()
            || members->contains(name) || members->size() >= kMaxBundleMembers) {
            return broken(QStringLiteral("unexpected member %1").arg(name));
        }
        const qint64 size = header.mid(124, 12).trimmed().replace('\0', "").toLongLong(&ok, 8);
        at += kTarBlock;
        if (!ok || size < 0 || size > bundle.size() - at) {
            return broken(QStringLiteral("cut short"));
        }
        // A view into the bundle, not a copy: the bundle outlives the apply.
        members->insert(name, QByteArray::fromRawData(bundle.constData() + at, static_cast<qsizetype>(size)));
        at += (size + kTarBlock - 1) / kTarBlock * kTarBlock;
    }
    return broken(QStringLiteral("cut short"));
}

bool UpdateStager::applyPatch(const QString& installRoot, const QByteArray& bundle,
                              const UpdateManifest::Patch& patch,
                              const QList<UpdateManifest::File>& files, const QString& stagingDir,
                              QString* problem, const std::atomic_bool* cancelled)
{
    std::error_code ec;
    fs::path install;
    fs::path stage;
    if (!checkRoots(installRoot, stagingDir, &install, &stage, problem)) {
        return false;
    }
    const auto fail = [&](const QString& why) {
        *problem = why;
        std::error_code ignore;
        fs::remove_all(stage, ignore);
        return false;
    };
    QHash<QString, QByteArray> members;
    if (!readBundle(bundle, &members, problem)) {
        return false;
    }
    fs::remove_all(stage, ec);
    fs::create_directories(stage.parent_path(), ec);
    fs::copy(install, stage, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
    if (ec) {
        return fail(QStringLiteral("The install could not be copied: %1.")
                        .arg(QString::fromStdString(ec.message())));
    }
    QString why;
    if (cancelled && cancelled->load()) {
        return fail(QStringLiteral("cancelled"));
    }
    if (!applyOps(stage, members, patch, cancelled, &why)) {
        return fail(QStringLiteral("The update could not be applied: %1.").arg(why));
    }
    if (!checkTree(stage, files, &why)) {
        return fail(QStringLiteral("The updated files do not match the release: %1.").arg(why));
    }
    return true;
}

bool UpdateStager::applyFull(const QString& installRoot, const QByteArray& zip,
                             const QList<UpdateManifest::File>& files, const QString& stagingDir,
                             QString* problem, const std::atomic_bool* cancelled)
{
    std::error_code ec;
    fs::path install;
    fs::path stage;
    if (!checkRoots(installRoot, stagingDir, &install, &stage, problem)) {
        return false;
    }
    const auto fail = [&](const QString& why) {
        *problem = why;
        std::error_code ignore;
        fs::remove_all(stage, ignore);
        return false;
    };
    // The zip may hold exactly the release's files: no more, no fewer, and
    // nothing larger in all than the manifest says.
    QHash<QString, const UpdateManifest::File*> wanted;
    ZipReadLimits limits;
    limits.maxEntries = 2 * files.size() + 16;   // and the folders' own entries
    limits.maxEntryUncompressedBytes = 0;
    limits.maxTotalUncompressedBytes = 0;
    for (const UpdateManifest::File& f : files) {
        if (f.symlink) {
            *problem = QStringLiteral("The full release has a link, which its zip cannot carry.");
            return false;
        }
        wanted.insert(f.path, &f);
        limits.maxEntryUncompressedBytes = std::max(limits.maxEntryUncompressedBytes, quint64(f.size));
        limits.maxTotalUncompressedBytes += quint64(f.size);
    }
    QString why;
    const QMap<QString, QByteArray> entries = readZipEntries(zip, &why, limits);
    if (entries.isEmpty()) {
        *problem = QStringLiteral("The full release is damaged: %1").arg(why);
        return false;
    }
    QHash<QString, QByteArray> contents;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        // Compress-Archive writes '\'.
        QString name = it.key();
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (name.endsWith(QLatin1Char('/'))) {
            name.chop(1);
            if (!safePath(name) || !it.value().isEmpty()) {
                return fail(QStringLiteral("The full release has an unexpected entry %1.").arg(it.key()));
            }
            continue;   // a folder; its files create it
        }
        if (!wanted.contains(name) || contents.contains(name)) {
            *problem = QStringLiteral("The full release has an unexpected file %1.").arg(it.key());
            return false;
        }
        contents.insert(name, it.value());
    }
    if (contents.size() != wanted.size()) {
        *problem = QStringLiteral("The full release is missing files.");
        return false;
    }
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (ec) {
        return fail(QStringLiteral("The staging folder could not be made."));
    }
    for (auto it = contents.cbegin(); it != contents.cend(); ++it) {
        if (cancelled && cancelled->load()) {
            return fail(QStringLiteral("cancelled"));
        }
        const fs::path target = stage / toPath(it.key());
        if (!noLinkOnTheWay(stage, it.key())) {
            return fail(QStringLiteral("%1: a parent folder is a link").arg(it.key()));
        }
        fs::create_directories(target.parent_path(), ec);
        QFile out(native(target));
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly) || out.write(it.value()) != it.value().size()
            || !out.flush()) {
            return fail(QStringLiteral("%1: cannot write the new file.").arg(it.key()));
        }
        out.close();
        setExecutable(target, wanted.value(it.key())->executable);
    }
    // setup.exe's uninstaller and its record stay with the install: the
    // release's own files never include them.
    // A release file of that name, in any case on Windows, where the
    // install's unins000.dat and the release's Unins000.dat are one file.
    const auto isReleaseFile = [&wanted](const QString& name) {
#ifdef Q_OS_WIN
        for (auto w = wanted.cbegin(); w != wanted.cend(); ++w) {
            if (w.key().compare(name, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        return false;
#else
        return wanted.contains(name);
#endif
    };
    // Every step takes an error code: this runs on a worker, where a
    // throwing increment would end the app.
    fs::directory_iterator it(install, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const fs::path entry = it->path();
        const QString name = native(entry.filename());
        if (!name.startsWith(QLatin1String("unins"), Qt::CaseInsensitive) || isReleaseFile(name)) {
            continue;
        }
        std::error_code probe;
        if (!fs::is_regular_file(fs::symlink_status(entry, probe))) {
            continue;   // a link or a folder is not the installer's file
        }
        fs::copy_file(entry, stage / entry.filename(), ec);
        if (ec) {
            return fail(QStringLiteral("%1: the installer's file could not be kept.").arg(name));
        }
    }
    if (ec) {
        return fail(QStringLiteral("The install could not be read."));
    }
    if (!checkTree(stage, files, &why)) {
        return fail(QStringLiteral("The unpacked files do not match the release: %1.").arg(why));
    }
    return true;
}

namespace {

// An empty path is no install, never the working directory.
bool sameInstall(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QDir(a).absolutePath()).compare(QDir::cleanPath(QDir(b).absolutePath()), cs) == 0;
}

} // namespace

UpdateStager::StagedRelease UpdateStager::stagedRelease(const QString& stagingRoot,
                                                        const QString& runningVersion,
                                                        const QString& installRoot,
                                                        const QString& githubLatestTag,
                                                        const QString& rolledBackVersion)
{
    StagedRelease chosen;
    QDir root(stagingRoot);
    if (stagingRoot.isEmpty() || !root.exists()) {
        return chosen;
    }
    const VersionNumber running = VersionNumber::parse(runningVersion);
    const QStringList records = root.entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& name : records) {
        if (name.endsWith(QLatin1String(".manifest.json"))) {
            continue;
        }
        QFile f(root.filePath(name));
        const QJsonObject r = f.size() <= 64 * 1024 && f.open(QIODevice::ReadOnly)
                                  ? QJsonDocument::fromJson(f.readAll()).object()
                                  : QJsonObject();
        const QString version = r.value(QLatin1String("version")).toString();
        const bool usable = chosen.version.isEmpty() && name == version + QLatin1String(".json")
                            && releaseVersion().match(version).hasMatch()
                            && VersionNumber::parse(version) > running
                            && r.value(QLatin1String("from")).toString() == runningVersion
                            && sameInstall(r.value(QLatin1String("install")).toString(), installRoot)
                            // The tag is the version's own: the record keeps none of
                            // its own to edit, and revalidate() binds the version to
                            // the signed manifest.
                            && (githubLatestTag.isEmpty() || githubLatestTag == QLatin1Char('v') + version)
                            && version != rolledBackVersion && QFileInfo(root.filePath(version)).isDir()
                            && QFileInfo::exists(root.filePath(version + QLatin1String(".manifest.json")))
                            && QFileInfo::exists(root.filePath(version + QLatin1String(".manifest.json.sig")));
        if (usable) {
            chosen = {version, root.filePath(version), runningVersion};
        }
    }
    // Everything else staging wrote is stale: other versions, half-written
    // records, work folders a crashed run left. The folder is shared with
    // the swap helper (UpdateInstaller::stateDir()): its journal, result,
    // repair plan, log and copy are never staging's to remove.
    std::error_code ec;
    const QStringList keep = chosen.version.isEmpty()
                                 ? QStringList()
                                 : QStringList{chosen.version, chosen.version + QLatin1String(".json"),
                                               chosen.version + QLatin1String(".manifest.json"),
                                               chosen.version + QLatin1String(".manifest.json.sig")};
    const QDir::Filters all = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
    // A tree, a record, or a record QSaveFile left half written
    // ("<record>.XXXXXX", where it uses a named temporary file).
    static const QRegularExpression record(
        QStringLiteral("^(.+?)\\.(json|manifest\\.json|manifest\\.json\\.sig)(\\.[A-Za-z0-9]{6})?$"));
    const auto owned = [](const QString& name) {
        const QRegularExpressionMatch m = record.match(name);
        return releaseVersion().match(m.hasMatch() ? m.captured(1) : name).hasMatch();
    };
    for (const QString& name : root.entryList(all)) {
        const bool ours = name.startsWith(QLatin1String(".work-")) || owned(name);
        if (ours && !keep.contains(name)) {
            qCInfo(lcUpdater).noquote() << "discarding stale staged update" << name;
            fs::remove_all(toPath(root.filePath(name)), ec);
        }
    }
    return chosen;
}

void UpdateStager::discard(const QString& stagingRoot, const QString& version)
{
    if (stagingRoot.isEmpty() || !releaseVersion().match(version).hasMatch()) {
        return;
    }
    std::error_code ec;
    for (const QString& suffix : {QString(), QStringLiteral(".json"), QStringLiteral(".manifest.json"),
                                  QStringLiteral(".manifest.json.sig")}) {
        fs::remove_all(toPath(recordPath(stagingRoot, version, suffix)), ec);
    }
    qCInfo(lcUpdater).noquote() << "staged update to v" + version + " discarded";
}

bool UpdateStager::revalidate(const QString& stagingRoot, const StagedRelease& staged,
                              const QString& platform, const QString& arch, QString* problem)
{
    if (staged.version.isEmpty()) {
        *problem = QStringLiteral("No update is staged.");
        return false;
    }
    QFile m(recordPath(stagingRoot, staged.version, QStringLiteral(".manifest.json")));
    QFile s(recordPath(stagingRoot, staged.version, QStringLiteral(".manifest.json.sig")));
    if (m.size() > kMaxManifestBytes || !m.open(QIODevice::ReadOnly) || !s.open(QIODevice::ReadOnly)) {
        *problem = QStringLiteral("The staged update's manifest is missing.");
        return false;
    }
    const QByteArray manifest = m.readAll();
    if (!verify(manifest, s.read(65))) {
        *problem = QStringLiteral("The staged update's manifest is not signed by AetherSDR's update key.");
        return false;
    }
    UpdateManifest parsed;
    if (!UpdateManifest::parse(manifest, &parsed, problem)) {
        return false;
    }
    if (parsed.version != staged.version) {
        *problem = QStringLiteral("The staged update's manifest is for another release.");
        return false;
    }
    for (const UpdateManifest::Payload& p : parsed.payloads) {
        if (p.platform == platform && p.arch == arch) {
            QString why;
            if (!checkTree(toPath(staged.dir), p.files, &why)) {
                *problem = QStringLiteral("The staged update has changed: %1.").arg(why);
                return false;
            }
            return true;
        }
    }
    *problem = QStringLiteral("The staged update's manifest has nothing for this computer.");
    return false;
}

bool UpdateStager::isBusy() const
{
    return m_stage != Stage::Idle;
}

UpdateStager::~UpdateStager()
{
    cancel();
}

void UpdateStager::cancel()
{
    m_download->cancel();
    if (m_cancelled) {
        m_cancelled->store(true);   // a running worker stops at its next step
    }
    ++m_generation;
    m_stage = Stage::Idle;
}

void UpdateStager::stage(const Request& request)
{
    cancel();
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    m_request = request;
    if (m_request.manifestUrl.isEmpty()) {
        m_request.manifestUrl = QUrl(QString::fromLatin1(kManifestUrl));
#if AETHER_UPDATE_DEV_OVERRIDES
        const QByteArray env = qgetenv("AETHER_UPDATE_MANIFEST_URL");
        if (!env.isEmpty()) {
            m_request.manifestUrl = QUrl::fromUserInput(QString::fromUtf8(env));
            qCWarning(lcUpdater) << "AETHER_UPDATE_MANIFEST_URL: using the manifest at"
                                 << m_request.manifestUrl.toString() << "instead of" << kManifestUrl;
        }
#endif
    }
    m_stage = Stage::Manifest;
    m_download->get(m_request.manifestUrl, kMaxManifestBytes, tr("The update manifest is too large."));
}

namespace {

// Where a signed release file is fetched from: its signed URL, unless a test
// (or a dev-overrides build) serves "<base>/<tag>/<asset>" instead. The URL
// the manifest names is still checked as signed.
QUrl fetchUrl(const UpdateManifest::Asset& asset, const QString& tag)
{
    QUrl base = testDownloadBase();
#if AETHER_UPDATE_DEV_OVERRIDES
    if (!base.isValid()) {
        const QByteArray env = qgetenv("AETHER_UPDATE_DOWNLOAD_BASE");
        if (!env.isEmpty()) {
            base = QUrl::fromUserInput(QString::fromUtf8(env));
            qCWarning(lcUpdater) << "AETHER_UPDATE_DOWNLOAD_BASE: fetching" << asset.name << "from"
                                 << base.toString();
        }
    }
#endif
    if (!base.isValid()) {
        return asset.url;
    }
    QString b = base.toString();
    while (b.endsWith(QLatin1Char('/'))) {
        b.chop(1);
    }
    return QUrl(b + QLatin1Char('/') + tag + QLatin1Char('/') + asset.name);
}

} // namespace

void UpdateStager::fallBack(const QString& reason)
{
    const UpdateManifest::Asset& full = m_payload->full;
    if (m_request.fullFallback && m_stage != Stage::Full
        && full.name.endsWith(QLatin1String(".zip"), Qt::CaseInsensitive) && full.size <= kMaxFullBytes) {
        qCInfo(lcUpdater).noquote() << "no patch applied (" + reason + "); staging the full release";
        m_stage = Stage::Full;
        m_download->get(fetchUrl(full, m_manifest.tag), full.size, tr("The update is larger than it said."));
        return;
    }
    m_stage = Stage::Idle;
    emit fullDownloadNeeded(reason, full, m_manifest.version);
}

void UpdateStager::runWorker(std::function<QString(const QString& work)> apply)
{
    emit progress(tr("Applying the update"), 0, 0);
    // Each request applies into a folder of its own, so a worker that is
    // still finishing after a cancel never shares files with the next; only
    // the current request's result becomes <stagingRoot>/<version>.
    const QString root = m_request.stagingRoot;
    const QString work = QDir(root).filePath(
        QStringLiteral(".work-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const quint64 generation = m_generation;
    const bool isFull = m_stage == Stage::Full;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, root, work, generation,
                                                                isFull] {
        QString problem = watcher->result();
        watcher->deleteLater();
        std::error_code ec;
        if (generation != m_generation) {
            fs::remove_all(toPath(work), ec);   // a cancelled request's result
            return;
        }
        const QString version = m_manifest.version;
        const QString staging = QDir(root).filePath(version);
        if (problem.isEmpty()) {
            fs::remove_all(toPath(staging), ec);
            fs::rename(toPath(work), toPath(staging), ec);
            if (ec) {
                fs::remove_all(toPath(work), ec);
                problem = QStringLiteral("The staged update could not be put in place.");
            }
        }
        if (!problem.isEmpty()) {
            qCWarning(lcUpdater).noquote() << "update to v" + version + " not staged:" << problem;
            if (isFull) {
                m_stage = Stage::Idle;
                emit fullDownloadNeeded(problem, m_payload->full, version);
            } else {
                fallBack(problem);
            }
            return;
        }
        // The record beside the tree, never in it: what it was staged for,
        // and the signed manifest that vouches for every file.
        QJsonObject record{{QStringLiteral("version"), version},
                           {QStringLiteral("from"), m_request.runningVersion},
                           {QStringLiteral("install"), QDir(m_request.installRoot).absolutePath()}};
        QSaveFile r(recordPath(root, version, QStringLiteral(".json")));
        QSaveFile m(recordPath(root, version, QStringLiteral(".manifest.json")));
        QSaveFile s(recordPath(root, version, QStringLiteral(".manifest.json.sig")));
        const bool saved = r.open(QIODevice::WriteOnly) && r.write(QJsonDocument(record).toJson()) >= 0
                           && r.commit() && m.open(QIODevice::WriteOnly) && m.write(m_manifestBytes) >= 0
                           && m.commit() && s.open(QIODevice::WriteOnly) && s.write(m_signature) >= 0
                           && s.commit();
        m_stage = Stage::Idle;
        if (!saved) {
            fs::remove_all(toPath(staging), ec);
            emit fullDownloadNeeded(tr("The staged update's record could not be saved."), m_payload->full,
                                    version);
            return;
        }
        qCInfo(lcUpdater).noquote() << "update to v" + version + (isFull ? " (full)" : "") + " staged in"
                                    << staging;
        emit staged(staging, version);
    });
    watcher->setFuture(QtConcurrent::run([apply, work] { return apply(work); }));
}

void UpdateStager::onDownloaded(const QUrl&, const QByteArray& body)
{
    switch (m_stage) {
    case Stage::Idle:
        return;
    case Stage::Manifest: {
        m_manifestBytes = body;
        m_stage = Stage::Signature;
        QUrl sig = m_request.manifestUrl;
        sig.setPath(sig.path() + QStringLiteral(".sig"));
        m_download->get(sig, 64, tr("The update signature is not a signature."));
        return;
    }
    case Stage::Signature: {
        // The signature before a byte of the manifest is read.
        if (!verify(m_manifestBytes, body)) {
            fail(tr("The update manifest is not signed by AetherSDR's update key."));
            return;
        }
        m_signature = body;
        QString problem;
        if (!UpdateManifest::parse(m_manifestBytes, &m_manifest, &problem)) {
            fail(problem);
            return;
        }
        const Decision d = choose(m_manifest, m_request, UpdateSettings::acceptedManifestUtc(),
                                  &m_payload, &m_patch);
        switch (d) {
        case Decision::NotNewer:
            m_stage = Stage::Idle;
            emit upToDate();
            return;
        case Decision::Replay:
            fail(tr("The update manifest is older than one already seen."));
            return;
        case Decision::NotLatest:
            fail(tr("The update manifest is not for GitHub's latest release."));
            return;
        case Decision::NoPayload:
            fail(tr("This release has no update for this computer."));
            return;
        case Decision::FullDownload:
        case Decision::Patch:
            break;
        }
        UpdateSettings::setAcceptedManifestUtc(m_manifest.generatedAt);
        if (d == Decision::FullDownload || m_patch->bundle.size > kMaxBundleBytes) {
            fallBack(tr("No update applies to this version."));
            return;
        }
        m_stage = Stage::Bundle;
        m_download->get(fetchUrl(m_patch->bundle, m_manifest.tag), m_patch->bundle.size,
                        tr("The update is larger than it said."));
        return;
    }
    case Stage::Bundle: {
        if (body.size() != m_patch->bundle.size
            || QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex())
                   != m_patch->bundle.sha256) {
            fallBack(tr("The downloaded update does not match its signed hash."));
            return;
        }
        const QString install = m_request.installRoot;
        const UpdateManifest::Patch patch = *m_patch;
        const QList<UpdateManifest::File> files = m_payload->files;
        const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
        runWorker([install, body, patch, files, cancelled](const QString& work) {
            QString problem;
            return applyPatch(install, body, patch, files, work, &problem, cancelled.get()) ? QString()
                                                                                          : problem;
        });
        return;
    }
    case Stage::Full: {
        if (body.size() != m_payload->full.size
            || QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex())
                   != m_payload->full.sha256) {
            m_stage = Stage::Idle;
            emit fullDownloadNeeded(tr("The downloaded update does not match its signed hash."),
                                    m_payload->full, m_manifest.version);
            return;
        }
        const QString install = m_request.installRoot;
        const QList<UpdateManifest::File> files = m_payload->files;
        const std::shared_ptr<std::atomic_bool> cancelled = m_cancelled;
        runWorker([install, body, files, cancelled](const QString& work) {
            QString problem;
            return applyFull(install, body, files, work, &problem, cancelled.get()) ? QString() : problem;
        });
        return;
    }
    }
}

void UpdateStager::fail(const QString& reason)
{
    m_stage = Stage::Idle;
    qCWarning(lcUpdater).noquote() << "update not staged:" << reason;
    emit failed(reason);
}

} // namespace AetherSDR
