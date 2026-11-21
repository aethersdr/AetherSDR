#include "gui/ViewLockSettings.h"

#include <cstdio>

// Round-trip and rejection pins for the persisted Lock view blob. The
// contract: one versioned object, keyed by radio/station then stable pan
// index, deterministic on write, and anything malformed restores nothing
// (a lock that silently engages is worse than one that silently doesn't).

namespace {

int fail(const char* message)
{
    std::fprintf(stderr, "view_lock_settings_test: %s\n", message);
    return 1;
}

namespace VLS = AetherSDR::ViewLockSettings;

int testRoundTrip()
{
    VLS::LockedPans locked;
    locked.insert(QStringLiteral("1234-5678/shack"), {0, 2});
    locked.insert(QStringLiteral("1234-5678/laptop"), {1});

    const QString raw = VLS::serialize(locked);
    const QString expected = QStringLiteral(
        "{\"radios\":{\"1234-5678/laptop\":{\"1\":true},"
        "\"1234-5678/shack\":{\"0\":true,\"2\":true}},\"version\":1}");
    if (raw != expected) {
        std::fprintf(stderr, "got: %s\n", qPrintable(raw));
        return fail("serialize is not the expected deterministic JSON");
    }

    QString error;
    const VLS::LockedPans parsed = VLS::parse(raw, &error);
    if (!error.isEmpty() || parsed != locked) {
        return fail("parse(serialize(x)) must equal x");
    }

    // Two stations on one radio stay independent (MultiFlex).
    if (parsed.value(QStringLiteral("1234-5678/laptop")).contains(0)) {
        return fail("one station's lock must not leak to another");
    }
    return 0;
}

int testEmpty()
{
    if (!VLS::serialize({}).isEmpty()) {
        return fail("nothing locked serializes to empty so the key is removed");
    }
    VLS::LockedPans onlyEmpty;
    onlyEmpty.insert(QStringLiteral("r/s"), {});
    if (!VLS::serialize(onlyEmpty).isEmpty()) {
        return fail("a radio with no locked pans is dropped");
    }

    QString error;
    if (!VLS::parse(QString(), &error).isEmpty() || !error.isEmpty()) {
        return fail("blank input restores nothing and is not an error");
    }
    return 0;
}

int testRejects()
{
    QString error;
    if (!VLS::parse(QStringLiteral("{not json"), &error).isEmpty() || error.isEmpty()) {
        return fail("malformed JSON restores nothing and reports why");
    }

    error.clear();
    if (!VLS::parse(QStringLiteral("[1,2]"), &error).isEmpty() || error.isEmpty()) {
        return fail("a non-object root restores nothing");
    }

    error.clear();
    const QString wrongVersion = QStringLiteral(
        "{\"version\":2,\"radios\":{\"r/s\":{\"0\":true}}}");
    if (!VLS::parse(wrongVersion, &error).isEmpty() || error.isEmpty()) {
        return fail("an unsupported version restores nothing");
    }

    // Bad entries are skipped individually; good ones survive.
    const QString mixed = QStringLiteral(
        "{\"version\":1,\"radios\":{"
        "\"\":{\"0\":true},"
        "\"r/s\":{\"0\":true,\"1\":false,\"-1\":true,\"x\":true,\"3\":\"yes\"},"
        "\"r/t\":5}}");
    error.clear();
    const VLS::LockedPans parsed = VLS::parse(mixed, &error);
    if (!error.isEmpty() || parsed.size() != 1
        || parsed.value(QStringLiteral("r/s")) != QSet<int>{0}) {
        return fail("only well-formed true entries with a valid index survive");
    }
    return 0;
}

} // namespace

int main()
{
    if (int rc = testRoundTrip()) {
        return rc;
    }
    if (int rc = testEmpty()) {
        return rc;
    }
    if (int rc = testRejects()) {
        return rc;
    }
    std::puts("view_lock_settings_test: ok");
    return 0;
}
