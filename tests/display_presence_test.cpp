#include "core/DisplayPresence.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
} } while (0)

// Build a fake /sys/class/drm connector: <root>/<name>/status = <status>\n.
static void makeConnector(const QString& root, const QString& name,
                          const QString& status)
{
    QDir().mkpath(root + QStringLiteral("/") + name);
    QFile f(root + QStringLiteral("/") + name + QStringLiteral("/status"));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(status.toUtf8() + '\n');
        f.close();
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // 1. A connected display -> Connected.
    {
        QTemporaryDir dir;
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-1"),
                      QStringLiteral("connected"));
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-2"),
                      QStringLiteral("disconnected"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Connected);
    }

    // 2. Connectors present, none connected -> Headless (the wayvnc case).
    {
        QTemporaryDir dir;
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-1"),
                      QStringLiteral("disconnected"));
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-2"),
                      QStringLiteral("disconnected"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Headless);
    }

    // 3. Writeback / virtual connectors ("unknown") never count as connected.
    {
        QTemporaryDir dir;
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-1"),
                      QStringLiteral("disconnected"));
        makeConnector(dir.path(), QStringLiteral("card1-Writeback-1"),
                      QStringLiteral("unknown"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Headless);
    }

    // 4. No connector directories at all -> Unknown (do NOT assume headless).
    {
        QTemporaryDir dir;  // empty
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Unknown);
    }

    // 5. Only non-connector entries (no "card*-*" dash form) -> Unknown.
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + QStringLiteral("/card1"));       // no dash suffix
        QDir().mkpath(dir.path() + QStringLiteral("/renderD128"));  // not a card
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Unknown);
    }

    // 6. A connector directory without a status file is skipped, not counted.
    {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + QStringLiteral("/card1-HDMI-A-1"));  // no status
        makeConnector(dir.path(), QStringLiteral("card1-HDMI-A-2"),
                      QStringLiteral("disconnected"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Headless);
    }

    // 7. Trailing newline is trimmed; a second card's connector counts too.
    {
        QTemporaryDir dir;
        makeConnector(dir.path(), QStringLiteral("card0-DP-1"),
                      QStringLiteral("connected"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Connected);
    }

    // 8. A nonexistent root -> Unknown (no crash).
    {
        CHECK(detectDisplayPresence(QStringLiteral("/nonexistent/path/xyz"))
              == DisplayPresence::Unknown);
    }

    // 9. Every connector reports "unknown" (all writeback/virtual, or a panel
    //    that can't hotplug-detect: DPI, composite, some DSI) -> Unknown, NOT
    //    Headless — we cannot tell, so don't push such a display onto XWayland.
    {
        QTemporaryDir dir;
        makeConnector(dir.path(), QStringLiteral("card1-DSI-1"),
                      QStringLiteral("unknown"));
        makeConnector(dir.path(), QStringLiteral("card1-Writeback-1"),
                      QStringLiteral("unknown"));
        CHECK(detectDisplayPresence(dir.path()) == DisplayPresence::Unknown);
    }

    if (g_failures == 0) {
        std::fprintf(stderr, "display_presence_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
