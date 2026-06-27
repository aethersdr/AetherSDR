#include "core/KiwiSdrRedirectPolicy.h"

#include <QUrl>

#include <cstdio>

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "kiwi_sdr_redirect_policy_test: %s\n", message);
    return 1;
}

bool allowed(const char* from, const char* to)
{
    return AetherSDR::KiwiSdrRedirectPolicy::isAllowedStatusRedirect(
        QUrl(QString::fromLatin1(from)),
        QUrl(QString::fromLatin1(to)));
}

} // namespace

int main()
{
    using namespace AetherSDR::KiwiSdrRedirectPolicy;

    if (canonicalHost(QStringLiteral("  EXAMPLE.com.  "))
        != QStringLiteral("example.com")) {
        return fail("canonical host normalization should trim, lowercase, and drop trailing dots");
    }

    if (proxyReceiverAlias(QStringLiteral("21042.proxy.kiwisdr.com"))
            != QStringLiteral("21042")
        || proxyReceiverAlias(QStringLiteral("21042.proxy2.kiwisdr.com"))
               != QStringLiteral("21042")
        || !proxyReceiverAlias(QStringLiteral("proxy.kiwisdr.com")).isEmpty()) {
        return fail("Kiwi proxy receiver alias extraction is wrong");
    }

    if (!allowed("http://Example.com.:8073/status",
                 "http://example.com:8074/status")) {
        return fail("same canonical host redirect should be allowed");
    }

    if (!allowed("http://21042.proxy.kiwisdr.com:8073/status",
                 "http://21042.proxy2.kiwisdr.com/status")
        || !allowed("http://21042.proxy2.kiwisdr.com/status",
                    "http://21042.proxy2.kiwisdr.com:8073/status")
        || !allowed("http://21042.proxy2.kiwisdr.com/status",
                    "http://21042.proxy.kiwisdr.com:8073/status")) {
        return fail("matching Kiwi proxy receiver alias redirects should be allowed");
    }

    QString detail;
    if (allowed("http://21042.proxy.kiwisdr.com:8073/status",
                "http://evil.example.com/status")) {
        return fail("cross-domain redirects should be rejected");
    }
    if (allowed("http://21042.proxy.kiwisdr.com:8073/status",
                "http://21043.proxy2.kiwisdr.com/status")) {
        return fail("mismatched Kiwi proxy aliases should be rejected");
    }
    if (isAllowedStatusRedirect(
            QUrl(QStringLiteral("http://21042.proxy.kiwisdr.com/status")),
            QUrl(QStringLiteral("ftp://21042.proxy.kiwisdr.com/status")),
            &detail)
        || !detail.contains(QStringLiteral("unsupported scheme"))) {
        return fail("unsupported redirect schemes should be rejected with a detail");
    }

    std::printf("kiwi_sdr_redirect_policy_test: OK\n");
    return 0;
}
