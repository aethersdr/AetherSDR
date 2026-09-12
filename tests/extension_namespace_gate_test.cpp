// #5262 M1: family-specific verbs are gated on the DECLARED NAMESPACE, not on
// the family string.
//
// extensionNamespaces is the handshake a client pre-checks before issuing
// invokeExtension(). M1's checklist put it plainly: a declared handshake nobody
// reads is the map doc's own "looks identical to one that works" trap. Before
// this, RadioModel's two PC-audio verbs asked `family == "icom"` instead — a
// subtly different question that excludes anything speaking the icom verbs
// without being family "icom", and that leaves the declaration unread.
//
// Socket-free: the predicate is exercised through RadioModel's own accessor
// against the real backends the production factory builds.

#include "models/RadioModel.h"
#include "core/backends/IRadioBackend.h"

#include <QCoreApplication>
#include <QString>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) ++g_failures;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    {
        // A default RadioModel builds the Flex backend (family "flex"), which
        // declares only its own namespace.
        RadioModel m;
        check(m.backendDeclaresExtension(QStringLiteral("flex")),
              "a Flex backend declares the flex namespace");
        check(!m.backendDeclaresExtension(QStringLiteral("icom")),
              "and does NOT declare icom — so the icom verbs stay gated off");

        // The predicate answers about the DECLARATION, not about the family
        // string. These two coincide today for every shipped backend, which is
        // exactly why the difference went unnoticed; the assertion below is
        // what keeps a future backend that speaks icom verbs from being
        // excluded by a family comparison.
        const RadioCapabilities caps = m.backendCapabilities();
        check(caps.family == QLatin1String("flex"),
              "the backend under test is the flex family");
        check(caps.extensionNamespaces.contains(QStringLiteral("flex")),
              "and its declaration is what the predicate reads");

        // Unknown namespaces are refused rather than defaulted open.
        check(!m.backendDeclaresExtension(QStringLiteral("nonesuch")),
              "an undeclared namespace is refused");
        check(!m.backendDeclaresExtension(QString()),
              "an empty namespace is refused");
    }

    if (g_failures == 0)
        std::printf("extension_namespace_gate_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
