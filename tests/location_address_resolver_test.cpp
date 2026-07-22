#include "core/LocationAddressResolver.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
} } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    CHECK(LocationAddressResolver::parseDisplayName(
              QByteArrayLiteral("{\"display_name\":\"123 Main St, Example, USA\"}"))
          == QStringLiteral("123 Main St, Example, USA"));
    CHECK(LocationAddressResolver::parseDisplayName(
              QByteArrayLiteral("{\"display_name\":\"  Nearby   mapped  place  \"}"))
          == QStringLiteral("Nearby mapped place"));
    CHECK(LocationAddressResolver::parseDisplayName(
              QByteArrayLiteral("{\"address\":{\"city\":\"Example\"}}"))
          .isEmpty());
    CHECK(LocationAddressResolver::parseDisplayName(QByteArrayLiteral("not json"))
          .isEmpty());
    CHECK(LocationAddressResolver::parseDisplayName(QByteArrayLiteral("[]"))
          .isEmpty());

    return g_failures == 0 ? 0 : 1;
}
