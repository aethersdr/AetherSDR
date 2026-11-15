// Which links in the What's New release notes may leave the app
// (src/gui/WhatsNewLinks.h). The notes are fetched from GitHub and rendered
// as Markdown, so a link's scheme is not the operator's choice.

#include "gui/WhatsNewLinks.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

bool opens(const char* link)
{
    return whatsNewLinkTarget(QUrl(QString::fromLatin1(link))).has_value();
}

}  // namespace

int main()
{
    check(opens("https://github.com/aethersdr/AetherSDR/pull/1"), "an https link opens");
    check(opens("http://www.hamsci.org/"), "an http link opens");
    check(opens("HTTPS://example.org/"), "the scheme is compared without case");

    const auto rel = whatsNewLinkTarget(QUrl(QStringLiteral("pull/6402")));
    check(rel && rel->toString() == QStringLiteral("https://github.com/aethersdr/AetherSDR/pull/6402"),
          "a relative link resolves against the repository");

    check(!opens("file:///etc/passwd"), "a file: link is refused");
    check(!opens("javascript:alert(1)"), "a javascript: link is refused");
    check(!opens("smb://host/share"), "an smb: link is refused");
    check(!opens("aethersdr://connect?host=1.2.3.4"), "a custom app scheme is refused");
    check(!opens("mailto:someone@example.org"), "a mailto: link is refused");
    check(!opens("https://"), "an https link without a host is refused");

    if (g_failures == 0)
        std::printf("whats_new_links_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
