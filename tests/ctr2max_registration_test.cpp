// Ctr2MaxRegistration against synthetic stock-firmware volumes: Reg.txt as
// v1.02.00m and later write it (hashed, read back trimmed), a plain-text call
// as earlier versions wrote it (not taken), absent, and no volume at all.

#include "Ctr2MaxRegistrationFixtures.h"
#include "core/Ctr2MaxRegistration.h"

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

QByteArray fixture(const char* base64)
{
    return qUncompress(QByteArray::fromBase64(base64));
}

} // namespace

int main()
{
    using Ctr2MaxRegistration::Result;
    QString call;
    const QByteArray registered = fixture(Ctr2MaxRegistrationFixtures::kRegistered);
    check(registered.size() == 16 * 4096, "the registered fixture decodes");
    check(Ctr2MaxRegistration::check(registered, &call) == Result::Registered,
          "a volume with a hashed Reg.txt (v1.02.00m and later) is registered");
    check(call.size() == 64 && !call.endsWith(QLatin1Char('\n')),
          "its first line is read, without its line ending");

    const QByteArray plain = fixture(Ctr2MaxRegistrationFixtures::kPlainCall);
    check(Ctr2MaxRegistration::check(plain, &call) == Result::NotRegistered,
          "a call in plain text (before v1.02.00m) is not taken as registered");

    const QByteArray unregistered = fixture(Ctr2MaxRegistrationFixtures::kUnregistered);
    check(Ctr2MaxRegistration::check(unregistered) == Result::NotRegistered,
          "a volume without Reg.txt is not registered");

    check(Ctr2MaxRegistration::check(QByteArray(16 * 4096, '\xFF')) == Result::Unreadable,
          "an erased partition is unreadable");
    check(Ctr2MaxRegistration::check(QByteArray(100, '\0')) == Result::Unreadable,
          "a short read is unreadable");

    // Reading never writes: the input is unchanged.
    QByteArray before = registered;
    Ctr2MaxRegistration::check(registered);
    check(registered == before, "the volume is unchanged");

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ctr2max_registration_test: all checks passed\n");
    return 0;
}
