// `get dsp` and `get dsp … backend`: the two forms must answer from the same
// object (#5401 review).
//
// The bridge documents `get <model> [selector] [property]` as narrowing: the
// property form returns one field of the snapshot the bare form returns whole.
// The DSP read-back this PR adds was merged into the snapshot AFTER the
// narrowing branch had already returned, so `get dsp` carried `backend` and
// `get dsp … backend` answered "unknown property 'backend' for dsp".
//
// That is worse than a cosmetic gap. assert_state and wait_for — the two tools
// a scenario actually uses to check a value — only ever issue property reads,
// so a field that exists only in the bare form is a field no automation client
// can assert on, which is the entire purpose of the read-back.
//
// So the contract pinned here is not "backend narrows". It is the general one:
// EVERY key of the bare snapshot must resolve through the property form, and
// resolve to the same value. Written that way it fails for `backend` on the
// pre-fix ordering, and it keeps failing for the next field appended below the
// branch by someone who reads only the surrounding lines.
//
// No socket, no radio, no app: the bridge's line dispatcher is called directly
// through its existing test friend, against a stand-in backend that reports a
// fixed chain list. Nothing here can key a transmitter — the stub's setKeying()
// has no body and no wire behind it.

#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "core/AutomationServer.h"
#include "core/backends/IRadioBackend.h"
#include "models/RadioModel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>
#include <memory>
#include <utility>

namespace AetherSDR {

// handleLine() is private; the header already befriends this name for the
// sibling automation tests.
class AutomationServerTestAccess
{
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

} // namespace AetherSDR

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool condition, const char* description)
{
    std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++failures;
    }
}

// The minimum IRadioBackend that can be asked one question. Every override is
// the empty answer except dspChains(), which is the subject.
class StubBackend : public IRadioBackend
{
public:
    explicit StubBackend(QVariantList chains) : m_chains(std::move(chains)) {}

    RadioCapabilities capabilities() const override { return {}; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return false; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    // Deliberately inert. This test never calls it, and there is no wire behind
    // it if it did.
    void setKeying(bool) override {}
    void invokeExtension(const QString&, const QString&, quint64,
                         const QVariant&) override {}

    QVariantList dspChains() const override { return m_chains; }

private:
    QVariantList m_chains;
};

QJsonObject get(AutomationServer& server, const QString& model,
                const QString& property)
{
    QJsonObject request{{QStringLiteral("cmd"), QStringLiteral("get")},
                        {QStringLiteral("model"), model}};
    if (!property.isEmpty()) {
        request[QStringLiteral("property")] = property;
    }
    return AutomationServerTestAccess::handleLine(
        server, QJsonDocument(request).toJson(QJsonDocument::Compact));
}

// Two chains in the shape Hl2Backend::dspChains() really returns, so the test
// exercises a nested array of objects rather than a scalar that would narrow
// even through a broken path.
QVariantList sampleChains()
{
    QVariantMap rx;
    rx[QStringLiteral("chain")] = QStringLiteral("rx-wdsp");
    rx[QStringLiteral("receiver")] = 0;
    rx[QStringLiteral("level")] = QStringLiteral("channel-config");
    rx[QStringLiteral("filterLowHz")] = 150.0;
    rx[QStringLiteral("filterHighHz")] = 3000.0;

    QVariantMap tx;
    tx[QStringLiteral("chain")] = QStringLiteral("hl2-tx");
    tx[QStringLiteral("level")] = QStringLiteral("dsp-config");
    tx[QStringLiteral("filterLowHz")] = 300.0;
    tx[QStringLiteral("filterHighHz")] = 2700.0;

    return QVariantList{rx, tx};
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(
        QStringLiteral("aether-automation-dsp-backend-readback-test"));

    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);
    std::printf("\n  get dsp — the property form must see what the bare form does\n\n");

    check(settingsProfile.isValid(), "isolated settings profile is available");

    AudioEngine engine;
    RadioModel radio;
    AutomationServer server;
    server.setAudioEngine(&engine);
    server.setRadioModel(&radio);

    // ── With no backend at all, both forms agree that there is nothing ──────
    {
        const QJsonObject bare = get(server, QStringLiteral("dsp"), QString());
        check(bare.value(QStringLiteral("ok")).toBool(), "get dsp succeeds with no backend");
        check(!bare.value(QStringLiteral("dsp")).toObject().contains(QStringLiteral("backend")),
              "no backend means no `backend` field — absence is reported, not an empty object");
        const QJsonObject narrowed =
            get(server, QStringLiteral("dsp"), QStringLiteral("backend"));
        check(!narrowed.value(QStringLiteral("ok")).toBool(),
              "and the property form refuses it, agreeing with the bare form");
    }

    radio.setBackendForTest(std::make_unique<StubBackend>(sampleChains()),
                            QStringLiteral("hl2"));

    // ── The bare form carries the read-back ────────────────────────────────
    const QJsonObject bare = get(server, QStringLiteral("dsp"), QString());
    check(bare.value(QStringLiteral("ok")).toBool(), "get dsp succeeds");
    const QJsonObject data = bare.value(QStringLiteral("dsp")).toObject();
    check(data.contains(QStringLiteral("backend")),
          "the bare snapshot contains the backend read-back");
    const QJsonObject backend = data.value(QStringLiteral("backend")).toObject();
    check(backend.value(QStringLiteral("family")).toString() == QLatin1String("hl2"),
          "it names the family it came from");
    check(backend.value(QStringLiteral("chains")).toArray().size() == 2,
          "and carries both chains the backend reported");

    // ── THE REGRESSION. Narrowing to it must work, and give the same value ──
    //
    // This is the assertion that fails on the pre-fix ordering, where the
    // property branch returned before `backend` was merged: the reply was
    // {ok:false, error:"unknown property 'backend' for dsp"}.
    {
        const QJsonObject narrowed =
            get(server, QStringLiteral("dsp"), QStringLiteral("backend"));
        check(narrowed.value(QStringLiteral("ok")).toBool(),
              "get dsp property=backend succeeds (pre-fix: unknown property)");
        check(narrowed.value(QStringLiteral("property")).toString()
                  == QLatin1String("backend"),
              "the reply names the property it narrowed to");
        check(narrowed.value(QStringLiteral("value")).toObject() == backend,
              "and its value is the SAME object the bare form reported");
    }

    // ── The general contract: every bare key resolves through the property ──
    //
    // Stated over the whole snapshot rather than over `backend` alone, because
    // the defect was an ORDERING one and ordering breaks the next field added
    // below the branch just as silently.
    {
        int keys = 0;
        int resolved = 0;
        int identical = 0;
        for (const QString& key : data.keys()) {
            ++keys;
            const QJsonObject narrowed = get(server, QStringLiteral("dsp"), key);
            if (!narrowed.value(QStringLiteral("ok")).toBool()) {
                std::printf("       unresolvable key: %s\n", qPrintable(key));
                continue;
            }
            ++resolved;
            if (narrowed.value(QStringLiteral("value")) == data.value(key)) {
                ++identical;
            }
        }
        check(keys > 1, "the bare snapshot has fields to narrow to");
        check(resolved == keys,
              "EVERY field of the bare snapshot resolves through the property form");
        check(identical == keys,
              "and narrows to the identical value");
    }

    // ── Narrowing still refuses what is genuinely absent ────────────────────
    {
        const QJsonObject narrowed =
            get(server, QStringLiteral("dsp"), QStringLiteral("noSuchProperty"));
        check(!narrowed.value(QStringLiteral("ok")).toBool(),
              "an absent property is still refused");
        check(narrowed.value(QStringLiteral("error")).toString()
                  .contains(QLatin1String("noSuchProperty")),
              "and the refusal names it");
    }

    // ── A backend that reports no chains is not a backend field ─────────────
    //
    // The absence is deliberate: `dspChains()` returns empty both for a backend
    // that does not implement it and for a gather that could not be made, and
    // an empty object would read as "asked, and the answer is nothing".
    {
        radio.setBackendForTest(std::make_unique<StubBackend>(QVariantList{}),
                                QStringLiteral("flex"));
        const QJsonObject empty = get(server, QStringLiteral("dsp"), QString());
        check(!empty.value(QStringLiteral("dsp")).toObject()
                   .contains(QStringLiteral("backend")),
              "a backend reporting no chains adds no backend field");
        const QJsonObject narrowed =
            get(server, QStringLiteral("dsp"), QStringLiteral("backend"));
        check(!narrowed.value(QStringLiteral("ok")).toBool(),
              "and the property form agrees, rather than answering an empty object");
    }

    if (failures > 0) {
        std::printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("\nall checks passed\n");
    return 0;
}
