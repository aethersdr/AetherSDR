#include "core/hfgateway/HfGatewayConfig.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

using namespace AetherSDR::HfGateway;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) {
        ++g_failed;
    }
}

QJsonObject engine(const QString& id)
{
    QJsonObject o;
    o.insert("id", id);
    return o;
}

Config withEngines(const QJsonArray& engines)
{
    QJsonObject root;
    root.insert("engines", engines);
    return fromJson(root);
}

}  // namespace

int main()
{
    // ── Defaults ────────────────────────────────────────────────────────────
    {
        const Config c = fromJson({});
        report("empty document: no engines", c.engines.isEmpty());
        report("empty document: back end on loopback", c.backend.loopbackOnly);
        report("empty document: back-end ports differ", c.backend.commandPort != c.backend.dataPort);
    }

    // ── Round trip ──────────────────────────────────────────────────────────
    {
        Config c;
        EngineConfig m;
        m.id = "mercury";
        m.profile = Profile::Mercury;
        m.label = "Mercury";
        m.commandPort = 8400;
        m.dataPort = 8401;
        EngineConfig v;
        v.id = "legacy-vara";
        v.profile = Profile::VaraCompatible;
        v.host = "localhost";
        c.engines = {m, v};
        c.backend.commandPort = 8600;
        c.backend.dataPort = 8601;
        c.backend.loopbackOnly = false;

        const QJsonObject j = toJson(c);
        report("schema version written", j.value("version").toInt() == Config::kSchemaVersion);
        const Config r = fromJson(j);
        report("round trip: engines", r.engines.size() == 2 && r.engines[0].id == "mercury"
                                          && r.engines[0].profile == Profile::Mercury && r.engines[0].label == "Mercury"
                                          && r.engines[0].commandPort == 8400 && r.engines[0].dataPort == 8401
                                          && r.engines[1].id == "legacy-vara" && r.engines[1].host == "localhost"
                                          && r.engines[1].profile == Profile::VaraCompatible);
        report("round trip: back end", r.backend.commandPort == 8600 && r.backend.dataPort == 8601
                                           && !r.backend.loopbackOnly);
        report("profile names round trip", profileFromString(toString(Profile::Data2gHost)) == Profile::Data2gHost);
        report("unknown profile falls back", profileFromString("frob") == Profile::VaraCompatible);
    }

    // ── Engine validation ───────────────────────────────────────────────────
    {
        const Config c = withEngines({engine("Bad Id"), engine(""), engine("ok-1"), engine("ok-1"), engine("ok-2")});
        report("invalid and duplicate ids dropped", c.engines.size() == 2 && c.engines[0].id == "ok-1"
                                                        && c.engines[1].id == "ok-2");
    }
    {
        QJsonArray many;
        for (int i = 0; i < 10; ++i) {
            many.append(engine(QStringLiteral("e%1").arg(i)));
        }
        report("at most kMaxEngines kept", withEngines(many).engines.size() == Config::kMaxEngines);
    }
    {
        QJsonObject e = engine("e");
        e.insert("commandPort", 80);
        e.insert("dataPort", 70000);
        e.insert("host", "bad host");
        e.insert("label", QStringLiteral("Name\x01With\nControls"));
        const EngineConfig r = withEngines({e}).engines.value(0);
        report("privileged port falls back", r.commandPort == 8300);
        report("out-of-range data port falls back to command + 1", r.dataPort == 8301);
        report("host with whitespace falls back to loopback", r.host == "127.0.0.1");
        report("label control characters stripped", r.label == "NameWithControls");
    }
    {
        QJsonObject e = engine("e");
        e.insert("commandPort", 9000);
        e.insert("dataPort", 9000);
        const EngineConfig r = withEngines({e}).engines.value(0);
        report("equal ports: data becomes command + 1", r.commandPort == 9000 && r.dataPort == 9001);
    }
    {
        QJsonObject e = engine("e");
        e.insert("commandPort", 9000.5);
        e.insert("dataPort", "9001");
        const EngineConfig r = withEngines({e}).engines.value(0);
        report("non-integer and non-number ports fall back", r.commandPort == 8300 && r.dataPort == 8301);
    }
    {
        QJsonObject e = engine("e");
        e.insert("label", QString(200, QLatin1Char('x')));
        report("label capped", withEngines({e}).engines.value(0).label.size() == 64);
    }

    // ── Back-end validation ─────────────────────────────────────────────────
    {
        QJsonObject b;
        b.insert("commandPort", 9100);
        b.insert("dataPort", 9100);
        b.insert("loopbackOnly", "no");
        QJsonObject root;
        root.insert("backend", b);
        const Config c = fromJson(root);
        const BackendConfig d;
        report("equal back-end ports fall back to defaults", c.backend.commandPort == d.commandPort
                                                                  && c.backend.dataPort == d.dataPort);
        report("non-bool loopbackOnly stays on", c.backend.loopbackOnly);
    }

    // ── Helpers ─────────────────────────────────────────────────────────────
    report("loopback hosts", isLoopbackHost("127.0.0.1") && isLoopbackHost("::1") && isLoopbackHost("localhost")
                                 && isLoopbackHost("LOCALHOST"));
    report("non-loopback hosts", !isLoopbackHost("192.168.1.10") && !isLoopbackHost("example.org")
                                     && !isLoopbackHost(""));
    report("port range", isValidPort(1024) && isValidPort(65535) && !isValidPort(1023) && !isValidPort(65536));
    report("engine ids", isValidEngineId("a") && isValidEngineId("mercury-2") && !isValidEngineId("Mercury")
                             && !isValidEngineId(QString(33, QLatin1Char('a'))));

    if (g_failed != 0) {
        std::printf("\n%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
