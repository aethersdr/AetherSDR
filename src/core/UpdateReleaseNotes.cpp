// UpdateChecker's release reading: the GitHub release JSON into ReleaseInfo,
// the installer for this computer, and the URL allowlist. Kept apart from the
// request lifecycle in UpdateChecker.cpp; every input here is network text.

#include "UpdateChecker.h"
#include "VersionNumber.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>

namespace AetherSDR {

namespace {

constexpr int kMaxTaglineChars = 160;
constexpr int kMaxChapterChars = 80;
constexpr int kMaxChapters = 12;
constexpr int kMaxAssets = 64;

const QString kDownloadPathPrefix = QStringLiteral("/aethersdr/AetherSDR/releases/download/");

// Release-prep boilerplate, not a chapter of what changed.
bool isBoilerplateHeading(const QString& heading)
{
    return heading.compare(QLatin1String("Contributors"), Qt::CaseInsensitive) == 0
        || heading.compare(QLatin1String("Downloads"), Qt::CaseInsensitive) == 0;
}

// Markdown emphasis, code spans and links become their plain text; control
// characters go. The result is shown with Qt::PlainText, never as markup.
QString plainText(QString text, int maxChars)
{
    static const QRegularExpression link(QStringLiteral(R"(\[([^\]]*)\]\([^)]*\))"));
    text.replace(link, QStringLiteral("\\1"));
    text.remove(QStringLiteral("**"));
    text.remove(QLatin1Char('`'));
    QString out;
    out.reserve(text.size());
    for (const QChar c : text) {
        out.append(c.isPrint() ? c : QChar(QLatin1Char(' ')));
    }
    out = out.simplified();
    if (out.size() > maxChars) {
        out = out.left(maxChars - 1).trimmed() + QChar(0x2026);
    }
    return out;
}

} // namespace

ReleaseInfo UpdateChecker::parseRelease(const QByteArray& json)
{
    ReleaseInfo info;
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const VersionNumber version = VersionNumber::parse(root.value(QLatin1String("tag_name")).toString());
    if (version.isNull()) {
        return info;
    }
    info.version = version.toString();

    // "AetherSDR v26.10.3 — SmartSDR 4.3 firmware support and …"
    const QString name = root.value(QLatin1String("name")).toString();
    const qsizetype dash = name.indexOf(QChar(0x2014));
    if (dash >= 0) {
        info.tagline = plainText(name.mid(dash + 1), kMaxTaglineChars);
    }

    info.published = QDateTime::fromString(root.value(QLatin1String("published_at")).toString(),
                                           Qt::ISODate).toUTC();

    const QStringList lines = root.value(QLatin1String("body")).toString().split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("### "))) {
            if (info.chapters.size() >= kMaxChapters) {
                break;
            }
            const QString heading = plainText(line.mid(4), kMaxChapterChars);
            if (heading.isEmpty() || isBoilerplateHeading(heading)) {
                continue;
            }
            info.chapters.append(heading);
        }
    }

    const QJsonArray assets = root.value(QLatin1String("assets")).toArray();
    for (const QJsonValue& v : assets) {
        if (info.assets.size() >= kMaxAssets) {
            break;
        }
        const QJsonObject a = v.toObject();
        ReleaseAsset asset{plainText(a.value(QLatin1String("name")).toString(), 200),
                           QUrl(a.value(QLatin1String("browser_download_url")).toString(),
                                QUrl::StrictMode),
                           a.value(QLatin1String("size")).toInteger()};
        if (!asset.name.isEmpty() && isTrustedDownloadUrl(asset.url)) {
            info.assets.append(asset);
        }
    }
    return info;
}

bool UpdateChecker::isTrustedDownloadUrl(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https")
        || url.host() != QLatin1String("github.com") || url.port() != -1
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) {
        return false;
    }
    const QString path = url.path(QUrl::FullyDecoded);
    if (!path.startsWith(kDownloadPathPrefix) || path.contains(QLatin1String(".."))) {
        return false;
    }
    // .../download/<tag>/<file>: exactly two segments after the prefix.
    const QStringList rest = path.mid(kDownloadPathPrefix.size()).split(QLatin1Char('/'));
    return rest.size() == 2 && !rest[0].isEmpty() && !rest[1].isEmpty();
}

int UpdateChecker::pickAsset(const QList<ReleaseAsset>& assets, const QString& os,
                             const QString& cpuArch, bool portable)
{
    const bool x64 = cpuArch == QLatin1String("x86_64");
    const bool arm64 = cpuArch == QLatin1String("arm64") || cpuArch == QLatin1String("aarch64");
    QString suffix;
    if (os == QLatin1String("windows") && x64) {
        suffix = portable ? QStringLiteral("-Windows-x64-portable.zip")
                          : QStringLiteral("-Windows-x64-setup.exe");
    } else if (os == QLatin1String("macos") && arm64) {
        suffix = QStringLiteral("-macOS-apple-silicon.dmg");
    } else if (os == QLatin1String("linux") && x64) {
        suffix = QStringLiteral("-x86_64.AppImage");
    } else if (os == QLatin1String("linux") && arm64) {
        suffix = QStringLiteral("-aarch64.AppImage");
    } else {
        return -1;
    }
    for (int i = 0; i < assets.size(); ++i) {
        if (assets[i].name.startsWith(QLatin1String("AetherSDR-"))
            && assets[i].name.endsWith(suffix)) {
            return i;
        }
    }
    return -1;
}

ReleaseAsset UpdateChecker::assetForThisComputer(const ReleaseInfo& release)
{
    if (!storeName().isEmpty()) {
        return {};
    }
#if defined(Q_OS_WIN)
    const QString os = QStringLiteral("windows");
    // The installer leaves its uninstaller beside the executable; the
    // portable zip has none.
    const bool portable = !QFile::exists(QCoreApplication::applicationDirPath()
                                         + QStringLiteral("/unins000.exe"));
#elif defined(Q_OS_MACOS)
    const QString os = QStringLiteral("macos");
    const bool portable = false;
#elif defined(Q_OS_LINUX)
    const QString os = QStringLiteral("linux");
    const bool portable = false;
#else
    const QString os;
    const bool portable = false;
#endif
    const int i = pickAsset(release.assets, os,
                            // The build's architecture, not the host's: an x64 build
                            // emulated on ARM is replaced by the x64 installer.
                            QSysInfo::buildCpuArchitecture(), portable);
    return i >= 0 ? release.assets[i] : ReleaseAsset{};
}

QUrl UpdateChecker::releasePageUrl(const QString& version)
{
    const VersionNumber parsed = VersionNumber::parse(version);
    if (parsed.isNull()) {
        return QUrl(QString::fromLatin1(kReleasesPageUrl));
    }
    return QUrl(QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/tag/v")
                + parsed.toString());
}

} // namespace AetherSDR
