#include "core/UpdateChecker.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>

// UpdateChecker's release reading against the real v26.10.3 release JSON
// (tests/fixtures/release/), plus hostile inputs: the tagline and chapters
// the Updates window shows, the installer picked per platform,
// and the download-URL allowlist. Pure: no network, no settings.

using namespace AetherSDR;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { qCritical() << "FAIL:" << label; ++failures; }
    };

    QFile fixture(QStringLiteral(UPDATE_RELEASE_FIXTURE));
    if (!fixture.open(QIODevice::ReadOnly)) {
        qCritical() << "cannot open" << fixture.fileName();
        return 1;
    }
    const ReleaseInfo r = UpdateChecker::parseRelease(fixture.readAll());

    check(r.isValid() && r.version == QLatin1String("26.10.3"), "version is the parsed tag");
    check(r.tagline == QLatin1String("SmartSDR 4.3 firmware support and the DeepCW AI CW decoder"),
          "tagline is the release name after the dash");
    check(r.published == QDateTime::fromString(QStringLiteral("2026-10-18T17:09:31Z"), Qt::ISODate),
          "published date");

    check(r.chapters.size() == 10, "ten chapters: Contributors and Downloads are boilerplate");
    check(r.chapters.value(0) == QLatin1String("SmartSDR 4.3 firmware support and FlexRadio fixes"),
          "first chapter");
    check(r.chapters.value(1) == QLatin1String("DeepCW AI CW decoder and neural CW reception"),
          "second chapter");
    check(!r.chapters.contains(QStringLiteral("Contributors"))
              && !r.chapters.contains(QStringLiteral("Downloads")),
          "no boilerplate chapter");

    check(r.assets.size() == 20, "every real asset URL passes the allowlist");
    auto picked = [&](const char* os, const char* arch, bool portable) {
        const int i = UpdateChecker::pickAsset(r.assets, QString::fromLatin1(os),
                                               QString::fromLatin1(arch), portable);
        return i >= 0 ? r.assets[i].name : QString();
    };
    check(picked("windows", "x86_64", false) == QLatin1String("AetherSDR-v26.10.3-Windows-x64-setup.exe"),
          "Windows installer");
    check(picked("windows", "x86_64", true) == QLatin1String("AetherSDR-v26.10.3-Windows-x64-portable.zip"),
          "Windows portable");
    check(picked("macos", "arm64", false) == QLatin1String("AetherSDR-v26.10.3-macOS-apple-silicon.dmg"),
          "macOS Apple silicon");
    check(picked("linux", "x86_64", false) == QLatin1String("AetherSDR-v26.10.3-x86_64.AppImage"),
          "Linux x86_64 AppImage, not its .asc or symbols");
    check(picked("linux", "arm64", false) == QLatin1String("AetherSDR-v26.10.3-aarch64.AppImage"),
          "Linux arm64 AppImage");
    check(picked("macos", "x86_64", false).isEmpty(), "no Intel Mac build: release page instead");
    check(picked("windows", "arm64", false).isEmpty(), "no Windows on ARM build: release page instead");

    // The allowlist.
    auto trusted = [](const char* url) {
        return UpdateChecker::isTrustedDownloadUrl(QUrl(QString::fromLatin1(url), QUrl::StrictMode));
    };
    check(trusted("https://github.com/aethersdr/AetherSDR/releases/download/v26.10.3/AetherSDR-v26.10.3-x86_64.AppImage"),
          "a real download URL is trusted");
    check(!trusted("http://github.com/aethersdr/AetherSDR/releases/download/v1/a.exe"), "http refused");
    check(!trusted("https://evil.example/aethersdr/AetherSDR/releases/download/v1/a.exe"), "other host refused");
    check(!trusted("https://github.com.evil.example/aethersdr/AetherSDR/releases/download/v1/a.exe"),
          "lookalike host refused");
    check(!trusted("https://github.com/someone/AetherSDR/releases/download/v1/a.exe"), "other repo refused");
    check(!trusted("https://github.com/aethersdr/AetherSDR/releases/download/v1/../../x/a.exe"), "dot-dot refused");
    check(!trusted("https://github.com/aethersdr/AetherSDR/releases/download/v1/a.exe?x=1"), "query refused");
    check(!trusted("https://user@github.com/aethersdr/AetherSDR/releases/download/v1/a.exe"), "userinfo refused");
    check(!trusted("https://github.com:8443/aethersdr/AetherSDR/releases/download/v1/a.exe"), "port refused");
    check(!trusted("https://github.com/aethersdr/AetherSDR/releases/download/v1/sub/a.exe"), "extra segment refused");
    check(!trusted("javascript:alert(1)"), "script URL refused");

    // Hostile and degenerate input.
    const ReleaseInfo hostile = UpdateChecker::parseRelease(QByteArrayLiteral(R"({
        "tag_name": "v99.0.0<b>x",
        "name": "AetherSDR v99 — <img src=x onerror=alert(1)> **bold** `code`\u0007",
        "body": "- **Before any chapter.** ignored.\n### <b>Chap</b> [one](https://evil)\n- **Lead (#1, #2).** First. Second.\n### Downloads\n- **Never.** x.",
        "assets": [
          {"name": "AetherSDR-v99-x86_64.AppImage", "browser_download_url": "https://evil.example/a", "size": 1},
          {"name": "AetherSDR-v99-x86_64.AppImage", "browser_download_url": "https://github.com/aethersdr/AetherSDR/releases/download/v99/AetherSDR-v99-x86_64.AppImage", "size": 1}
        ]
    })"));
    check(hostile.version == QLatin1String("99.0.0"), "hostile tag parses to digits");
    check(hostile.tagline == QLatin1String("<img src=x onerror=alert(1)> bold code"),
          "tagline loses markdown and control characters (markup stays text, shown as plain text)");
    check(hostile.chapters == QStringList{QStringLiteral("<b>Chap</b> one")}, "links reduced to their text");
    check(hostile.assets.size() == 1 && hostile.assets[0].url.host() == QLatin1String("github.com"),
          "an untrusted asset URL is dropped");

    check(!UpdateChecker::parseRelease(QByteArrayLiteral("not json")).isValid(), "garbage is invalid");
    check(!UpdateChecker::parseRelease(QByteArrayLiteral(R"({"tag_name":"latest"})")).isValid(),
          "a non-version tag is invalid");
    QByteArray longName = QByteArrayLiteral(R"({"tag_name":"v1.2.3","name":"AetherSDR v1.2.3 — )");
    longName.append(QByteArray(5000, 'x'));
    longName.append(QByteArrayLiteral(R"("})"));
    check(UpdateChecker::parseRelease(longName).tagline.size() <= 160, "tagline is capped");

    check(UpdateChecker::releasePageUrl(QStringLiteral("26.10.3"))
              == QUrl(QStringLiteral("https://github.com/aethersdr/AetherSDR/releases/tag/v26.10.3")),
          "release page from digits");
    check(UpdateChecker::releasePageUrl(QStringLiteral("<x>"))
              == QUrl(QString::fromLatin1(UpdateChecker::kReleasesPageUrl)),
          "unusable version falls back to the latest-release page");

    return failures ? 1 : 0;
}
