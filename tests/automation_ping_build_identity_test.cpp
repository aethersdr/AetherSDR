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
    }

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
