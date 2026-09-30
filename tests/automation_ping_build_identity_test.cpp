// `ping` reports the build identity captured at build time (#5804).
//
// Socket-free: requests go through the production parser/dispatcher via
// handleLine, the server is never started, and no radio is constructed.
//
// The expected values come from the same generated header AutomationServer.cpp
// compiles against, so this pins the wiring (every field reaches the reply,
// with its JSON type) and the internal consistency of what git describe
// produced. The capture itself -- that the header follows HEAD without a
// re-configure -- is covered by build_identity_capture_test.
//
// Blind spot the live reply cannot close on its own: with no reachable tag
// (CI's fetch-depth 1 checkout) describe and sha are the SAME bare hash, so a
// swap of the two keys changes nothing observable there. The key mapping is
// therefore also pinned against fixed shapes whose fields differ, whatever
// checkout the test happens to be built from.
#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include "AetherBuildIdentity.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

namespace AetherSDR {
class AutomationServerTestAccess
{
public:
    static QJsonObject request(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
    static QJsonObject identity(const QString& describe, const QString& sha,
                                const QString& baseline, int commitsSinceTag, bool dirty)
    {
        return AutomationServer::buildIdentityJson(describe, sha, baseline,
                                                   commitsSinceTag, dirty);
    }
};
}

namespace {
int failures = 0;
void check(bool ok, const char* description)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", description);
    if (!ok) {
        ++failures;
    }
}

// The build object must carry exactly the header's values, typed.
void checkBuild(const QJsonObject& reply, const char* context)
{
    const QJsonValue buildValue = reply.value(QStringLiteral("build"));
    std::printf("-- %s: %s\n", context,
                QJsonDocument(reply).toJson(QJsonDocument::Compact).constData());
    check(buildValue.isObject(), "ping carries a `build` object");
    const QJsonObject build = buildValue.toObject();

    check(build.value(QStringLiteral("describe")).toString()
              == QStringLiteral(AETHER_BUILD_DESCRIBE),
          "build.describe is the captured git describe");
    check(build.value(QStringLiteral("sha")).toString()
              == QStringLiteral(AETHER_BUILD_SHA),
          "build.sha is the captured short hash");
    check(build.value(QStringLiteral("baseline")).toString()
              == QStringLiteral(AETHER_BUILD_BASELINE),
          "build.baseline is the captured tag");
    check(build.value(QStringLiteral("commitsSinceTag")).isDouble()
              && build.value(QStringLiteral("commitsSinceTag")).toInt()
                     == AETHER_BUILD_COMMITS_SINCE_TAG,
          "build.commitsSinceTag is a number and matches the capture");
    check(build.value(QStringLiteral("dirty")).isBool()
              && build.value(QStringLiteral("dirty")).toBool() == AETHER_BUILD_DIRTY,
          "build.dirty is a bool and matches the capture");
    check(build.size() == 5,
          "build carries exactly describe/sha/baseline/commitsSinceTag/dirty "
          "(no branch name: a branch is not a build identity)");

    // Positional, from the reply alone: `sha` is a bare hash (or "unknown")
    // and, except exactly on a tag -- where describe is the tag itself --
    // describe contains it. Holds in every checkout state.
    const QString replySha = build.value(QStringLiteral("sha")).toString();
    check(!replySha.contains(QLatin1Char('-')) && !replySha.contains(QLatin1Char('.')),
          "build.sha is a bare hash, never a describe string or a tag");
    if (build.value(QStringLiteral("commitsSinceTag")).toInt() != 0) {
        check(build.value(QStringLiteral("describe")).toString().contains(replySha),
              "off a tag, build.describe contains build.sha");
    }
}

// The key mapping, driven with values that differ -- in particular the
// no-reachable-tag shapes, whose clean form is the one CI builds.
void checkMapping(const char* shape, const QString& describe, const QString& sha,
                  const QString& baseline, int commitsSinceTag, bool dirty)
{
    const QJsonObject b = AetherSDR::AutomationServerTestAccess::identity(
        describe, sha, baseline, commitsSinceTag, dirty);
    std::printf("-- fixture %s: %s\n", shape,
                QJsonDocument(b).toJson(QJsonDocument::Compact).constData());
    const bool ok = b.size() == 5
        && b.value(QStringLiteral("describe")).toString() == describe
        && b.value(QStringLiteral("sha")).toString() == sha
        && b.value(QStringLiteral("baseline")).toString() == baseline
        && b.value(QStringLiteral("commitsSinceTag")).isDouble()
        && b.value(QStringLiteral("commitsSinceTag")).toInt() == commitsSinceTag
        && b.value(QStringLiteral("dirty")).isBool()
        && b.value(QStringLiteral("dirty")).toBool() == dirty;
    check(ok, shape);
}
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("automation-ping-build-identity"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("0.0.0-test"));

    AetherSDR::AutomationServer server; // Never start(): no listener or radio.
    const auto ping = [&]() {
        return AetherSDR::AutomationServerTestAccess::request(
            server, QByteArrayLiteral("{\"cmd\":\"ping\"}"));
    };

    // ---- The header itself is self-consistent ------------------------------
    const QString describe = QStringLiteral(AETHER_BUILD_DESCRIBE);
    const QString sha = QStringLiteral(AETHER_BUILD_SHA);
    const QString baseline = QStringLiteral(AETHER_BUILD_BASELINE);
    std::printf("-- captured: describe=%s sha=%s baseline=%s commits=%d dirty=%d\n",
                qPrintable(describe), qPrintable(sha), qPrintable(baseline),
                AETHER_BUILD_COMMITS_SINCE_TAG, AETHER_BUILD_DIRTY ? 1 : 0);
    check(!describe.isEmpty() && !sha.isEmpty() && !baseline.isEmpty(),
          "no captured field is empty (\"unknown\" stands in for missing)");
    check(describe.endsWith(QStringLiteral("-dirty")) == AETHER_BUILD_DIRTY,
          "dirty agrees with describe's -dirty suffix");
    if (AETHER_BUILD_COMMITS_SINCE_TAG > 0) {
        check(describe.startsWith(baseline + QLatin1Char('-'))
                  && describe.contains(QStringLiteral("-g") + sha),
              "past a tag: describe is <baseline>-<n>-g<sha>");
    } else if (AETHER_BUILD_COMMITS_SINCE_TAG == 0) {
        check(describe.startsWith(baseline) && sha != QStringLiteral("unknown"),
              "on a tag: describe is the tag and the sha is still known");
    } else {
        check(baseline == QStringLiteral("unknown"),
              "no reachable tag (or no git): baseline is unknown");
        // The degenerate case, stated rather than left silent: describe IS the
        // bare hash (plus -dirty), or both are "unknown" outside a checkout.
        check(describe == sha || describe == sha + QStringLiteral("-dirty"),
              "no reachable tag (or no git): describe is the sha itself");
    }

    // ---- The mapping, independent of this checkout ------------------------
    checkMapping("fixture past a tag, dirty: every key carries its own field",
                 QStringLiteral("v1.2.3-68-g7e841682-dirty"), QStringLiteral("7e841682"),
                 QStringLiteral("v1.2.3"), 68, true);
    checkMapping("fixture on a tag: every key carries its own field",
                 QStringLiteral("v1.2.3"), QStringLiteral("0a1b2c3d"),
                 QStringLiteral("v1.2.3"), 0, false);
    checkMapping("fixture no reachable tag, dirty: describe and sha stay apart",
                 QStringLiteral("0a1b2c3d-dirty"), QStringLiteral("0a1b2c3d"),
                 QStringLiteral("unknown"), -1, true);
    checkMapping("fixture no reachable tag, clean (CI): describe == sha, by design",
                 QStringLiteral("0a1b2c3d"), QStringLiteral("0a1b2c3d"),
                 QStringLiteral("unknown"), -1, false);

    // ---- ping reports it --------------------------------------------------
    const QJsonObject open = ping();
    check(open.value(QStringLiteral("ok")).toBool(), "ping answers ok");
    check(open.value(QStringLiteral("version")).toString()
              == QStringLiteral("0.0.0-test"),
          "version is still the release string, unchanged in meaning");
    checkBuild(open, "open bridge");

    // ping is in the read-only safe set and stays open under a token; the
    // build identity must be there in both, since a harness asks before auth.
    server.setReadOnly(true);
    checkBuild(ping(), "read-only bridge");
    server.setReadOnly(false);

    server.setAuthToken(QStringLiteral("secret"));
    const QJsonObject gated = ping();
    check(gated.value(QStringLiteral("authRequired")).toBool(),
          "with a token set, ping reports authRequired");
    checkBuild(gated, "token-gated bridge");

    return failures == 0 ? 0 : 1;
}
