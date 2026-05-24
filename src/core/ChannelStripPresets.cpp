#include "ChannelStripPresets.h"

#include "AudioEngine.h"
#include "ClientComp.h"
#include "ClientDeEss.h"
#include "ClientEq.h"
#include "ClientFinalLimiter.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientReverb.h"
#include "ClientTube.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kSchemaVersion = 1;

// ── Stage enum/string mapping ─────────────────────────────────────
// Local copy (the AudioEngine.cpp helpers are file-static); kept
// trivial enough to maintain alongside the canonical map there.

QString stageEnumToName(AudioEngine::TxChainStage s)
{
    switch (s) {
        case AudioEngine::TxChainStage::Gate:   return "Gate";
        case AudioEngine::TxChainStage::Eq:     return "Eq";
        case AudioEngine::TxChainStage::DeEss:  return "DeEss";
        case AudioEngine::TxChainStage::Comp:   return "Comp";
        case AudioEngine::TxChainStage::Tube:   return "Tube";
        case AudioEngine::TxChainStage::Enh:    return "Enh";
        case AudioEngine::TxChainStage::Reverb: return "Reverb";
        case AudioEngine::TxChainStage::None:   return "";
    }
    return "";
}

AudioEngine::TxChainStage stageNameToEnum(const QString& n)
{
    if (n == "Gate")   return AudioEngine::TxChainStage::Gate;
    if (n == "Eq")     return AudioEngine::TxChainStage::Eq;
    if (n == "DeEss")  return AudioEngine::TxChainStage::DeEss;
    if (n == "Comp")   return AudioEngine::TxChainStage::Comp;
    if (n == "Tube")   return AudioEngine::TxChainStage::Tube;
    if (n == "Enh")    return AudioEngine::TxChainStage::Enh;
    if (n == "Reverb") return AudioEngine::TxChainStage::Reverb;
    return AudioEngine::TxChainStage::None;
}

// RX-side enum↔name mapping (#2425).  Only stages that exist in
// RxChainStage; the pre-existing TX-only stages (Enh / Reverb) have no
// RX equivalent and aren't in the enum.
QString rxStageEnumToName(AudioEngine::RxChainStage s)
{
    switch (s) {
        case AudioEngine::RxChainStage::Gate:  return "Gate";
        case AudioEngine::RxChainStage::Eq:    return "Eq";
        case AudioEngine::RxChainStage::DeEss: return "DeEss";
        case AudioEngine::RxChainStage::Comp:  return "Comp";
        case AudioEngine::RxChainStage::Tube:  return "Tube";
        case AudioEngine::RxChainStage::Pudu:  return "Pudu";
        case AudioEngine::RxChainStage::None:  return "";
    }
    return "";
}

AudioEngine::RxChainStage rxStageNameToEnum(const QString& n)
{
    if (n == "Gate")  return AudioEngine::RxChainStage::Gate;
    if (n == "Eq")    return AudioEngine::RxChainStage::Eq;
    if (n == "DeEss") return AudioEngine::RxChainStage::DeEss;
    if (n == "Comp")  return AudioEngine::RxChainStage::Comp;
    if (n == "Tube")  return AudioEngine::RxChainStage::Tube;
    if (n == "Pudu")  return AudioEngine::RxChainStage::Pudu;
    return AudioEngine::RxChainStage::None;
}

// ── Per-module enum string maps ───────────────────────────────────

QString eqFilterTypeName(ClientEq::FilterType t)
{
    switch (t) {
        case ClientEq::FilterType::Peak:      return "Peak";
        case ClientEq::FilterType::LowShelf:  return "LowShelf";
        case ClientEq::FilterType::HighShelf: return "HighShelf";
        case ClientEq::FilterType::LowPass:   return "LowPass";
        case ClientEq::FilterType::HighPass:  return "HighPass";
    }
    return "Peak";
}

ClientEq::FilterType eqFilterTypeFromName(const QString& n)
{
    if (n == "LowShelf")  return ClientEq::FilterType::LowShelf;
    if (n == "HighShelf") return ClientEq::FilterType::HighShelf;
    if (n == "LowPass")   return ClientEq::FilterType::LowPass;
    if (n == "HighPass")  return ClientEq::FilterType::HighPass;
    return ClientEq::FilterType::Peak;
}

QString eqFilterFamilyName(ClientEq::FilterFamily f)
{
    switch (f) {
        case ClientEq::FilterFamily::Butterworth: return "Butterworth";
        case ClientEq::FilterFamily::Chebyshev:   return "Chebyshev";
        case ClientEq::FilterFamily::Bessel:      return "Bessel";
        case ClientEq::FilterFamily::Elliptic:    return "Elliptic";
    }
    return "Butterworth";
}

ClientEq::FilterFamily eqFilterFamilyFromName(const QString& n)
{
    if (n == "Chebyshev") return ClientEq::FilterFamily::Chebyshev;
    if (n == "Bessel")    return ClientEq::FilterFamily::Bessel;
    if (n == "Elliptic")  return ClientEq::FilterFamily::Elliptic;
    return ClientEq::FilterFamily::Butterworth;
}

QString gateModeName(ClientGate::Mode m)
{
    switch (m) {
        case ClientGate::Mode::Expander:  return "Expander";
        case ClientGate::Mode::Gate:      return "Gate";
    }
    return "Expander";
}

ClientGate::Mode gateModeFromName(const QString& n)
{
    if (n == "Gate") return ClientGate::Mode::Gate;
    return ClientGate::Mode::Expander;
}

QString tubeModelName(ClientTube::Model m)
{
    switch (m) {
        case ClientTube::Model::A: return "A";
        case ClientTube::Model::B: return "B";
        case ClientTube::Model::C: return "C";
    }
    return "A";
}

ClientTube::Model tubeModelFromName(const QString& n)
{
    if (n == "B") return ClientTube::Model::B;
    if (n == "C") return ClientTube::Model::C;
    return ClientTube::Model::A;
}

QString puduModeName(ClientPudu::Mode m)
{
    switch (m) {
        case ClientPudu::Mode::Aphex:     return "Aphex";
        case ClientPudu::Mode::Behringer: return "Behringer";
    }
    return "Aphex";
}

ClientPudu::Mode puduModeFromName(const QString& n)
{
    if (n == "Behringer") return ClientPudu::Mode::Behringer;
    return ClientPudu::Mode::Aphex;
}

// ── Helpers for safe JSON reads ───────────────────────────────────

double jnum(const QJsonObject& o, const QString& k, double fallback)
{
    const auto v = o.value(k);
    return v.isDouble() ? v.toDouble() : fallback;
}

bool jbool(const QJsonObject& o, const QString& k, bool fallback)
{
    const auto v = o.value(k);
    return v.isBool() ? v.toBool() : fallback;
}

QString jstr(const QJsonObject& o, const QString& k,
             const QString& fallback = QString())
{
    const auto v = o.value(k);
    return v.isString() ? v.toString() : fallback;
}

} // namespace

// ──────────────────────────────────────────────────────────────────
// Construction
// ──────────────────────────────────────────────────────────────────

ChannelStripPresets::ChannelStripPresets(AudioEngine* engine,
                                         QObject*     parent)
    : QObject(parent)
    , m_engine(engine)
{
    if (!loadFromDisk()) {
        m_root = QJsonObject{
            {"version", kSchemaVersion},
            {"presets", QJsonObject{}},
        };
    }
}

QString ChannelStripPresets::filePath() const
{
    // Sibling of AetherSDR.settings under XDG_CONFIG_HOME.
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/ChannelStrip.settings";
}

bool ChannelStripPresets::loadFromDisk()
{
    QFile f(filePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    m_root = doc.object();
    if (!m_root.contains("presets") || !m_root["presets"].isObject())
        m_root["presets"] = QJsonObject{};
    if (!m_root.contains("version"))
        m_root["version"] = kSchemaVersion;
    return true;
}

bool ChannelStripPresets::saveToDisk() const
{
    QFile f(filePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    return true;
}

// ──────────────────────────────────────────────────────────────────
// Public preset library API
// ──────────────────────────────────────────────────────────────────

QStringList ChannelStripPresets::presetNames() const
{
    QStringList names = m_root.value("presets").toObject().keys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool ChannelStripPresets::hasPreset(const QString& name) const
{
    return m_root.value("presets").toObject().contains(name);
}

bool ChannelStripPresets::loadPreset(const QString& name)
{
    const auto presets = m_root.value("presets").toObject();
    if (!presets.contains(name)) return false;
    applyPresetJson(presets.value(name).toObject());
    return true;
}

bool ChannelStripPresets::savePresetFromCurrent(const QString& name)
{
    if (name.trimmed().isEmpty()) return false;
    auto presets = m_root.value("presets").toObject();
    presets[name] = capturePresetJson();
    m_root["presets"] = presets;
    if (!saveToDisk()) return false;
    emit presetsChanged();
    return true;
}

bool ChannelStripPresets::deletePreset(const QString& name)
{
    auto presets = m_root.value("presets").toObject();
    if (!presets.contains(name)) return false;
    presets.remove(name);
    m_root["presets"] = presets;
    if (!saveToDisk()) return false;
    emit presetsChanged();
    return true;
}

bool ChannelStripPresets::exportPresetToFile(const QString& name,
                                             const QString& path) const
{
    const auto presets = m_root.value("presets").toObject();
    if (!presets.contains(name)) return false;

    // Single-preset shareable format: top-level object IS the preset
    // body, plus a `name` field so importers can recover the name
    // even if the file was renamed on disk.
    QJsonObject out = presets.value(name).toObject();
    out["name"] = name;
    out["schema"] = "AetherSDR ChannelStripPreset v1";

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    return true;
}

bool ChannelStripPresets::exportLibraryToFile(const QString& path) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    return true;
}

bool ChannelStripPresets::exportCurrentToFile(const QString& presetName,
                                              const QString& path) const
{
    QJsonObject out = capturePresetJson();
    out["name"] = presetName;
    out["schema"] = "AetherSDR ChannelStripPreset v1";

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
    return true;
}

QString ChannelStripPresets::importPresetFromFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return QString();

    const QJsonObject root = doc.object();
    auto presets = m_root.value("presets").toObject();

    QString firstImported;

    // Full library form (has "presets" wrapper).
    if (root.contains("presets") && root.value("presets").isObject()) {
        const auto incoming = root.value("presets").toObject();
        for (auto it = incoming.begin(); it != incoming.end(); ++it) {
            if (!it.value().isObject()) continue;
            QString name = it.key();
            // De-collide on import: append " (imported)" if a same-name
            // preset already exists in the local library, then suffix
            // with numbers if needed.
            if (presets.contains(name)) {
                QString base = name + " (imported)";
                QString candidate = base;
                int n = 2;
                while (presets.contains(candidate))
                    candidate = base + QString(" %1").arg(n++);
                name = candidate;
            }
            presets[name] = it.value().toObject();
            if (firstImported.isEmpty()) firstImported = name;
        }
    } else {
        // Single-preset form (top-level object IS the preset body).
        QString name = root.value("name").toString();
        if (name.trimmed().isEmpty()) {
            // Fall back to filename without extension.
            name = QFileInfo(path).completeBaseName();
        }
        if (name.trimmed().isEmpty()) return QString();
        if (presets.contains(name)) {
            QString base = name + " (imported)";
            QString candidate = base;
            int n = 2;
            while (presets.contains(candidate))
                candidate = base + QString(" %1").arg(n++);
            name = candidate;
        }
        QJsonObject body = root;
        body.remove("name");
        body.remove("schema");
        presets[name] = body;
        firstImported = name;
    }

    if (firstImported.isEmpty()) return QString();
    m_root["presets"] = presets;
    if (!saveToDisk()) return QString();
    emit presetsChanged();
    return firstImported;
}

// ──────────────────────────────────────────────────────────────────
// Capture: engine → JSON
// ──────────────────────────────────────────────────────────────────

QJsonObject ChannelStripPresets::capturePresetJson() const
{
    QJsonObject preset;
    preset["createdBy"] = "AetherSDR";
    preset["createdAt"] = QDateTime::currentDateTimeUtc()
        .toString(Qt::ISODate);

    if (!m_engine) return preset;

    // Chain order.
    {
        QJsonArray chain;
        for (auto s : m_engine->txChainStages()) {
            const QString n = stageEnumToName(s);
            if (!n.isEmpty()) chain.append(n);
        }
        preset["chain"] = chain;
    }

    // Gate.
    if (auto* g = m_engine->clientGateTx()) {
        QJsonObject o;
        o["enabled"]      = g->isEnabled();
        o["mode"]         = gateModeName(g->mode());
        o["thresholdDb"]  = g->thresholdDb();
        o["ratio"]        = g->ratio();
        o["attackMs"]     = g->attackMs();
        o["releaseMs"]    = g->releaseMs();
        o["holdMs"]       = g->holdMs();
        o["floorDb"]      = g->floorDb();
        o["returnDb"]     = g->returnDb();
        o["lookaheadMs"]  = g->lookaheadMs();
        preset["gate"] = o;
    }

    // EQ.
    if (auto* e = m_engine->clientEqTx()) {
        QJsonObject o;
        o["enabled"]         = e->isEnabled();
        o["masterGain"]      = e->masterGain();
        o["filterFamily"]    = eqFilterFamilyName(e->filterFamily());
        o["activeBandCount"] = e->activeBandCount();
        QJsonArray bands;
        for (int i = 0; i < ClientEq::kMaxBands; ++i) {
            const auto b = e->band(i);
            QJsonObject bo;
            bo["freqHz"]        = b.freqHz;
            bo["gainDb"]        = b.gainDb;
            bo["q"]             = b.q;
            bo["type"]          = eqFilterTypeName(b.type);
            bo["enabled"]       = b.enabled;
            bo["slopeDbPerOct"] = b.slopeDbPerOct;
            bands.append(bo);
        }
        o["bands"] = bands;
        preset["eq"] = o;
    }

    // Comp.
    if (auto* c = m_engine->clientCompTx()) {
        QJsonObject o;
        o["enabled"]          = c->isEnabled();
        o["thresholdDb"]      = c->thresholdDb();
        o["ratio"]            = c->ratio();
        o["attackMs"]         = c->attackMs();
        o["releaseMs"]        = c->releaseMs();
        o["kneeDb"]           = c->kneeDb();
        o["makeupDb"]         = c->makeupDb();
        o["limiterEnabled"]   = c->limiterEnabled();
        o["limiterCeilingDb"] = c->limiterCeilingDb();
        preset["comp"] = o;
    }

    // De-Esser.
    if (auto* d = m_engine->clientDeEssTx()) {
        QJsonObject o;
        o["enabled"]     = d->isEnabled();
        o["frequencyHz"] = d->frequencyHz();
        o["q"]           = d->q();
        o["thresholdDb"] = d->thresholdDb();
        o["amountDb"]    = d->amountDb();
        o["attackMs"]    = d->attackMs();
        o["releaseMs"]   = d->releaseMs();
        preset["deess"] = o;
    }

    // Tube.
    if (auto* t = m_engine->clientTubeTx()) {
        QJsonObject o;
        o["enabled"]        = t->isEnabled();
        o["model"]          = tubeModelName(t->model());
        o["driveDb"]        = t->driveDb();
        o["biasAmount"]     = t->biasAmount();
        o["tone"]           = t->tone();
        o["outputGainDb"]   = t->outputGainDb();
        o["dryWet"]         = t->dryWet();
        o["envelopeAmount"] = t->envelopeAmount();
        o["attackMs"]       = t->attackMs();
        o["releaseMs"]      = t->releaseMs();
        preset["tube"] = o;
    }

    // Pudu.
    if (auto* p = m_engine->clientPuduTx()) {
        QJsonObject o;
        o["enabled"]        = p->isEnabled();
        o["mode"]           = puduModeName(p->mode());
        o["pooDriveDb"]     = p->pooDriveDb();
        o["pooTuneHz"]      = p->pooTuneHz();
        o["pooMix"]         = p->pooMix();
        o["dooTuneHz"]      = p->dooTuneHz();
        o["dooHarmonicsDb"] = p->dooHarmonicsDb();
        o["dooMix"]         = p->dooMix();
        preset["pudu"] = o;
    }

    // Reverb.
    if (auto* r = m_engine->clientReverbTx()) {
        QJsonObject o;
        o["enabled"]    = r->isEnabled();
        o["size"]       = r->size();
        o["decayS"]     = r->decayS();
        o["damping"]    = r->damping();
        o["preDelayMs"] = r->preDelayMs();
        o["mix"]        = r->mix();
        preset["reverb"] = o;
    }

    // RN2 mic pre-amp (#3054).  Sibling of the per-stage objects so a
    // future NR-cluster addition can extend `rn2` into an object —
    // today we only persist the enable bit because that is the entire
    // user-tunable surface for RN2 TX.  Old preset files without the
    // key leave RN2 state untouched on apply, matching the precedent
    // of the v0.9.8 `rx` block.
    preset["rn2"] = m_engine->rn2TxEnabled();

    // Final brickwall limiter (always present, not in user chain).
    // Test-tone state is intentionally NOT in presets — it's a
    // session-time setup tool, not a "voice mix" parameter.
    // Quindar tones (#2262) are likewise excluded: they're a PTT-time
    // stylistic choice, not voice-shaping, and preset recall shouldn't
    // silently start signing off everyone's transmissions.
    if (auto* lim = m_engine->clientFinalLimiterTx()) {
        QJsonObject o;
        o["enabled"]      = lim->isEnabled();
        o["ceilingDb"]    = lim->ceilingDb();
        o["outputTrimDb"] = lim->outputTrimDb();
        o["dcBlock"]      = lim->dcBlockEnabled();
        preset["finalLimiter"] = o;
    }

    // ── RX-side mirror (#2425) ────────────────────────────────────
    // Stored under a top-level "rx" object so old preset files (no
    // rx key) leave RX state untouched on apply, and so a single
    // schema bump isn't required to ship this.  Same field shapes
    // as the TX modules above, just bound to the *Rx engine getters.
    {
        QJsonObject rx;

        // RX chain order.
        QJsonArray rxChain;
        for (auto s : m_engine->rxChainStages()) {
            const QString n = rxStageEnumToName(s);
            if (!n.isEmpty()) rxChain.append(n);
        }
        rx["chain"] = rxChain;

        if (auto* g = m_engine->clientGateRx()) {
            QJsonObject o;
            o["enabled"]      = g->isEnabled();
            o["mode"]         = gateModeName(g->mode());
            o["thresholdDb"]  = g->thresholdDb();
            o["ratio"]        = g->ratio();
            o["attackMs"]     = g->attackMs();
            o["releaseMs"]    = g->releaseMs();
            o["holdMs"]       = g->holdMs();
            o["floorDb"]      = g->floorDb();
            o["returnDb"]     = g->returnDb();
            o["lookaheadMs"]  = g->lookaheadMs();
            rx["gate"] = o;
        }
        if (auto* e = m_engine->clientEqRx()) {
            QJsonObject o;
            o["enabled"]         = e->isEnabled();
            o["masterGain"]      = e->masterGain();
            o["filterFamily"]    = eqFilterFamilyName(e->filterFamily());
            o["activeBandCount"] = e->activeBandCount();
            QJsonArray bands;
            for (int i = 0; i < ClientEq::kMaxBands; ++i) {
                const auto b = e->band(i);
                QJsonObject bo;
                bo["freqHz"]        = b.freqHz;
                bo["gainDb"]        = b.gainDb;
                bo["q"]             = b.q;
                bo["type"]          = eqFilterTypeName(b.type);
                bo["enabled"]       = b.enabled;
                bo["slopeDbPerOct"] = b.slopeDbPerOct;
                bands.append(bo);
            }
            o["bands"] = bands;
            rx["eq"] = o;
        }
        if (auto* c = m_engine->clientCompRx()) {
            QJsonObject o;
            o["enabled"]          = c->isEnabled();
            o["thresholdDb"]      = c->thresholdDb();
            o["ratio"]            = c->ratio();
            o["attackMs"]         = c->attackMs();
            o["releaseMs"]        = c->releaseMs();
            o["kneeDb"]           = c->kneeDb();
            o["makeupDb"]         = c->makeupDb();
            o["limiterEnabled"]   = c->limiterEnabled();
            o["limiterCeilingDb"] = c->limiterCeilingDb();
            rx["comp"] = o;
        }
        if (auto* d = m_engine->clientDeEssRx()) {
            QJsonObject o;
            o["enabled"]     = d->isEnabled();
            o["frequencyHz"] = d->frequencyHz();
            o["q"]           = d->q();
            o["thresholdDb"] = d->thresholdDb();
            o["amountDb"]    = d->amountDb();
            o["attackMs"]    = d->attackMs();
            o["releaseMs"]   = d->releaseMs();
            rx["deess"] = o;
        }
        if (auto* t = m_engine->clientTubeRx()) {
            QJsonObject o;
            o["enabled"]        = t->isEnabled();
            o["model"]          = tubeModelName(t->model());
            o["driveDb"]        = t->driveDb();
            o["biasAmount"]     = t->biasAmount();
            o["tone"]           = t->tone();
            o["outputGainDb"]   = t->outputGainDb();
            o["dryWet"]         = t->dryWet();
            o["envelopeAmount"] = t->envelopeAmount();
            o["attackMs"]       = t->attackMs();
            o["releaseMs"]      = t->releaseMs();
            rx["tube"] = o;
        }
        if (auto* p = m_engine->clientPuduRx()) {
            QJsonObject o;
            o["enabled"]        = p->isEnabled();
            o["mode"]           = puduModeName(p->mode());
            o["pooDriveDb"]     = p->pooDriveDb();
            o["pooTuneHz"]      = p->pooTuneHz();
            o["pooMix"]         = p->pooMix();
            o["dooTuneHz"]      = p->dooTuneHz();
            o["dooHarmonicsDb"] = p->dooHarmonicsDb();
            o["dooMix"]         = p->dooMix();
            rx["pudu"] = o;
        }
        // RX RN2 — same per-profile semantics as the TX `rn2` field
        // above (#3054).  Lives inside the `rx` block because the RX
        // RN2 toggle is bound to m_rn2Enabled, separate from the
        // TX-side m_rn2TxEnabled atomic.
        rx["rn2"] = m_engine->rn2Enabled();
        preset["rx"] = rx;
    }

    return preset;
}

// ──────────────────────────────────────────────────────────────────
// Apply: JSON → engine
// ──────────────────────────────────────────────────────────────────

void ChannelStripPresets::applyPresetJson(const QJsonObject& preset)
{
    if (!m_engine) return;

    // Chain order.
    if (preset.contains("chain") && preset.value("chain").isArray()) {
        QVector<AudioEngine::TxChainStage> stages;
        for (const auto& v : preset.value("chain").toArray()) {
            const auto s = stageNameToEnum(v.toString());
            if (s != AudioEngine::TxChainStage::None) stages.append(s);
        }
        if (!stages.isEmpty()) m_engine->setTxChainStages(stages);
    }

    // Gate.
    if (auto* g = m_engine->clientGateTx();
        g && preset.contains("gate") && preset.value("gate").isObject()) {
        const auto o = preset.value("gate").toObject();
        g->setEnabled(jbool(o, "enabled", g->isEnabled()));
        g->setMode(gateModeFromName(jstr(o, "mode", gateModeName(g->mode()))));
        g->setThresholdDb(jnum(o, "thresholdDb", g->thresholdDb()));
        g->setRatio(jnum(o, "ratio", g->ratio()));
        g->setAttackMs(jnum(o, "attackMs", g->attackMs()));
        g->setReleaseMs(jnum(o, "releaseMs", g->releaseMs()));
        g->setHoldMs(jnum(o, "holdMs", g->holdMs()));
        g->setFloorDb(jnum(o, "floorDb", g->floorDb()));
        g->setReturnDb(jnum(o, "returnDb", g->returnDb()));
        g->setLookaheadMs(jnum(o, "lookaheadMs", g->lookaheadMs()));
    }

    // EQ.
    if (auto* e = m_engine->clientEqTx();
        e && preset.contains("eq") && preset.value("eq").isObject()) {
        const auto o = preset.value("eq").toObject();
        e->setEnabled(jbool(o, "enabled", e->isEnabled()));
        e->setMasterGain(static_cast<float>(jnum(o, "masterGain", e->masterGain())));
        e->setFilterFamily(eqFilterFamilyFromName(
            jstr(o, "filterFamily", eqFilterFamilyName(e->filterFamily()))));
        const int activeCount = static_cast<int>(jnum(o, "activeBandCount",
                                                       e->activeBandCount()));
        if (o.contains("bands") && o.value("bands").isArray()) {
            const auto bands = o.value("bands").toArray();
            const int n = std::min<int>(bands.size(), ClientEq::kMaxBands);
            for (int i = 0; i < n; ++i) {
                const auto bo = bands.at(i).toObject();
                ClientEq::BandParams p = e->band(i);
                p.freqHz = static_cast<float>(jnum(bo, "freqHz", p.freqHz));
                p.gainDb = static_cast<float>(jnum(bo, "gainDb", p.gainDb));
                p.q      = static_cast<float>(jnum(bo, "q", p.q));
                p.type   = eqFilterTypeFromName(
                    jstr(bo, "type", eqFilterTypeName(p.type)));
                p.enabled = jbool(bo, "enabled", p.enabled);
                p.slopeDbPerOct = static_cast<int>(
                    jnum(bo, "slopeDbPerOct", p.slopeDbPerOct));
                e->setBand(i, p);
            }
        }
        e->setActiveBandCount(activeCount);
    }

    // Comp.
    if (auto* c = m_engine->clientCompTx();
        c && preset.contains("comp") && preset.value("comp").isObject()) {
        const auto o = preset.value("comp").toObject();
        c->setEnabled(jbool(o, "enabled", c->isEnabled()));
        c->setThresholdDb(jnum(o, "thresholdDb", c->thresholdDb()));
        c->setRatio(jnum(o, "ratio", c->ratio()));
        c->setAttackMs(jnum(o, "attackMs", c->attackMs()));
        c->setReleaseMs(jnum(o, "releaseMs", c->releaseMs()));
        c->setKneeDb(jnum(o, "kneeDb", c->kneeDb()));
        c->setMakeupDb(jnum(o, "makeupDb", c->makeupDb()));
        c->setLimiterEnabled(jbool(o, "limiterEnabled", c->limiterEnabled()));
        c->setLimiterCeilingDb(jnum(o, "limiterCeilingDb",
                                    c->limiterCeilingDb()));
    }

    // De-Esser.
    if (auto* d = m_engine->clientDeEssTx();
        d && preset.contains("deess") && preset.value("deess").isObject()) {
        const auto o = preset.value("deess").toObject();
        d->setEnabled(jbool(o, "enabled", d->isEnabled()));
        d->setFrequencyHz(jnum(o, "frequencyHz", d->frequencyHz()));
        d->setQ(jnum(o, "q", d->q()));
        d->setThresholdDb(jnum(o, "thresholdDb", d->thresholdDb()));
        d->setAmountDb(jnum(o, "amountDb", d->amountDb()));
        d->setAttackMs(jnum(o, "attackMs", d->attackMs()));
        d->setReleaseMs(jnum(o, "releaseMs", d->releaseMs()));
    }

    // Tube.
    if (auto* t = m_engine->clientTubeTx();
        t && preset.contains("tube") && preset.value("tube").isObject()) {
        const auto o = preset.value("tube").toObject();
        t->setEnabled(jbool(o, "enabled", t->isEnabled()));
        t->setModel(tubeModelFromName(jstr(o, "model", tubeModelName(t->model()))));
        t->setDriveDb(jnum(o, "driveDb", t->driveDb()));
        t->setBiasAmount(jnum(o, "biasAmount", t->biasAmount()));
        t->setTone(jnum(o, "tone", t->tone()));
        t->setOutputGainDb(jnum(o, "outputGainDb", t->outputGainDb()));
        t->setDryWet(jnum(o, "dryWet", t->dryWet()));
        t->setEnvelopeAmount(jnum(o, "envelopeAmount", t->envelopeAmount()));
        t->setAttackMs(jnum(o, "attackMs", t->attackMs()));
        t->setReleaseMs(jnum(o, "releaseMs", t->releaseMs()));
    }

    // Pudu.
    if (auto* p = m_engine->clientPuduTx();
        p && preset.contains("pudu") && preset.value("pudu").isObject()) {
        const auto o = preset.value("pudu").toObject();
        p->setEnabled(jbool(o, "enabled", p->isEnabled()));
        p->setMode(puduModeFromName(jstr(o, "mode", puduModeName(p->mode()))));
        p->setPooDriveDb(jnum(o, "pooDriveDb", p->pooDriveDb()));
        p->setPooTuneHz(jnum(o, "pooTuneHz", p->pooTuneHz()));
        p->setPooMix(jnum(o, "pooMix", p->pooMix()));
        p->setDooTuneHz(jnum(o, "dooTuneHz", p->dooTuneHz()));
        p->setDooHarmonicsDb(jnum(o, "dooHarmonicsDb", p->dooHarmonicsDb()));
        p->setDooMix(jnum(o, "dooMix", p->dooMix()));
    }

    // Reverb.
    if (auto* r = m_engine->clientReverbTx();
        r && preset.contains("reverb") && preset.value("reverb").isObject()) {
        const auto o = preset.value("reverb").toObject();
        r->setEnabled(jbool(o, "enabled", r->isEnabled()));
        r->setSize(jnum(o, "size", r->size()));
        r->setDecayS(jnum(o, "decayS", r->decayS()));
        r->setDamping(jnum(o, "damping", r->damping()));
        r->setPreDelayMs(jnum(o, "preDelayMs", r->preDelayMs()));
        r->setMix(jnum(o, "mix", r->mix()));
    }

    // Final brickwall limiter.
    if (auto* lim = m_engine->clientFinalLimiterTx();
        lim && preset.contains("finalLimiter")
            && preset.value("finalLimiter").isObject()) {
        const auto o = preset.value("finalLimiter").toObject();
        lim->setEnabled(jbool(o, "enabled", lim->isEnabled()));
        lim->setCeilingDb(jnum(o, "ceilingDb", lim->ceilingDb()));
        lim->setOutputTrimDb(jnum(o, "outputTrimDb", lim->outputTrimDb()));
        lim->setDcBlockEnabled(jbool(o, "dcBlock", lim->dcBlockEnabled()));
    }

    // RN2 mic pre-amp (#3054).  Only applies the key when present —
    // old preset files without `rn2` leave the engine's current RN2
    // state untouched (the v0.9.8 `rx` block uses the same convention).
    // setRn2TxEnabled() routes through the lazy-alloc + persistence
    // path so AppSettings stays in sync with the preset.
    if (preset.contains("rn2")) {
        m_engine->setRn2TxEnabled(jbool(preset, "rn2",
                                        m_engine->rn2TxEnabled()));
    }

    // ── RX-side apply (#2425) ─────────────────────────────────────
    // Reads the optional top-level "rx" block.  Old preset files
    // (no rx key) leave RX state untouched.
    if (preset.contains("rx") && preset.value("rx").isObject()) {
        const auto rx = preset.value("rx").toObject();

        if (rx.contains("chain") && rx.value("chain").isArray()) {
            QVector<AudioEngine::RxChainStage> stages;
            for (const auto& v : rx.value("chain").toArray()) {
                const auto s = rxStageNameToEnum(v.toString());
                if (s != AudioEngine::RxChainStage::None) stages.append(s);
            }
            if (!stages.isEmpty()) m_engine->setRxChainStages(stages);
        }

        if (auto* g = m_engine->clientGateRx();
            g && rx.contains("gate") && rx.value("gate").isObject()) {
            const auto o = rx.value("gate").toObject();
            g->setEnabled(jbool(o, "enabled", g->isEnabled()));
            g->setMode(gateModeFromName(jstr(o, "mode", gateModeName(g->mode()))));
            g->setThresholdDb(jnum(o, "thresholdDb", g->thresholdDb()));
            g->setRatio(jnum(o, "ratio", g->ratio()));
            g->setAttackMs(jnum(o, "attackMs", g->attackMs()));
            g->setReleaseMs(jnum(o, "releaseMs", g->releaseMs()));
            g->setHoldMs(jnum(o, "holdMs", g->holdMs()));
            g->setFloorDb(jnum(o, "floorDb", g->floorDb()));
            g->setReturnDb(jnum(o, "returnDb", g->returnDb()));
            g->setLookaheadMs(jnum(o, "lookaheadMs", g->lookaheadMs()));
        }

        if (auto* e = m_engine->clientEqRx();
            e && rx.contains("eq") && rx.value("eq").isObject()) {
            const auto o = rx.value("eq").toObject();
            e->setEnabled(jbool(o, "enabled", e->isEnabled()));
            e->setMasterGain(static_cast<float>(jnum(o, "masterGain", e->masterGain())));
            e->setFilterFamily(eqFilterFamilyFromName(
                jstr(o, "filterFamily", eqFilterFamilyName(e->filterFamily()))));
            const int activeCount = static_cast<int>(jnum(o, "activeBandCount",
                                                           e->activeBandCount()));
            if (o.contains("bands") && o.value("bands").isArray()) {
                const auto bands = o.value("bands").toArray();
                const int n = std::min<int>(bands.size(), ClientEq::kMaxBands);
                for (int i = 0; i < n; ++i) {
                    const auto bo = bands.at(i).toObject();
                    ClientEq::BandParams p = e->band(i);
                    p.freqHz = static_cast<float>(jnum(bo, "freqHz", p.freqHz));
                    p.gainDb = static_cast<float>(jnum(bo, "gainDb", p.gainDb));
                    p.q      = static_cast<float>(jnum(bo, "q", p.q));
                    p.type   = eqFilterTypeFromName(
                        jstr(bo, "type", eqFilterTypeName(p.type)));
                    p.enabled = jbool(bo, "enabled", p.enabled);
                    p.slopeDbPerOct = static_cast<int>(
                        jnum(bo, "slopeDbPerOct", p.slopeDbPerOct));
                    e->setBand(i, p);
                }
            }
            e->setActiveBandCount(activeCount);
        }

        if (auto* c = m_engine->clientCompRx();
            c && rx.contains("comp") && rx.value("comp").isObject()) {
            const auto o = rx.value("comp").toObject();
            c->setEnabled(jbool(o, "enabled", c->isEnabled()));
            c->setThresholdDb(jnum(o, "thresholdDb", c->thresholdDb()));
            c->setRatio(jnum(o, "ratio", c->ratio()));
            c->setAttackMs(jnum(o, "attackMs", c->attackMs()));
            c->setReleaseMs(jnum(o, "releaseMs", c->releaseMs()));
            c->setKneeDb(jnum(o, "kneeDb", c->kneeDb()));
            c->setMakeupDb(jnum(o, "makeupDb", c->makeupDb()));
            c->setLimiterEnabled(jbool(o, "limiterEnabled", c->limiterEnabled()));
            c->setLimiterCeilingDb(jnum(o, "limiterCeilingDb",
                                        c->limiterCeilingDb()));
        }

        if (auto* d = m_engine->clientDeEssRx();
            d && rx.contains("deess") && rx.value("deess").isObject()) {
            const auto o = rx.value("deess").toObject();
            d->setEnabled(jbool(o, "enabled", d->isEnabled()));
            d->setFrequencyHz(jnum(o, "frequencyHz", d->frequencyHz()));
            d->setQ(jnum(o, "q", d->q()));
            d->setThresholdDb(jnum(o, "thresholdDb", d->thresholdDb()));
            d->setAmountDb(jnum(o, "amountDb", d->amountDb()));
            d->setAttackMs(jnum(o, "attackMs", d->attackMs()));
            d->setReleaseMs(jnum(o, "releaseMs", d->releaseMs()));
        }

        if (auto* t = m_engine->clientTubeRx();
            t && rx.contains("tube") && rx.value("tube").isObject()) {
            const auto o = rx.value("tube").toObject();
            t->setEnabled(jbool(o, "enabled", t->isEnabled()));
            t->setModel(tubeModelFromName(jstr(o, "model", tubeModelName(t->model()))));
            t->setDriveDb(jnum(o, "driveDb", t->driveDb()));
            t->setBiasAmount(jnum(o, "biasAmount", t->biasAmount()));
            t->setTone(jnum(o, "tone", t->tone()));
            t->setOutputGainDb(jnum(o, "outputGainDb", t->outputGainDb()));
            t->setDryWet(jnum(o, "dryWet", t->dryWet()));
            t->setEnvelopeAmount(jnum(o, "envelopeAmount", t->envelopeAmount()));
            t->setAttackMs(jnum(o, "attackMs", t->attackMs()));
            t->setReleaseMs(jnum(o, "releaseMs", t->releaseMs()));
        }

        if (auto* p = m_engine->clientPuduRx();
            p && rx.contains("pudu") && rx.value("pudu").isObject()) {
            const auto o = rx.value("pudu").toObject();
            p->setEnabled(jbool(o, "enabled", p->isEnabled()));
            p->setMode(puduModeFromName(jstr(o, "mode", puduModeName(p->mode()))));
            p->setPooDriveDb(jnum(o, "pooDriveDb", p->pooDriveDb()));
            p->setPooTuneHz(jnum(o, "pooTuneHz", p->pooTuneHz()));
            p->setPooMix(jnum(o, "pooMix", p->pooMix()));
            p->setDooTuneHz(jnum(o, "dooTuneHz", p->dooTuneHz()));
            p->setDooHarmonicsDb(jnum(o, "dooHarmonicsDb", p->dooHarmonicsDb()));
            p->setDooMix(jnum(o, "dooMix", p->dooMix()));
        }

        // RX RN2 (#3054).  Only applies when the key is present — old
        // preset files leave RN2 untouched.  setRn2Enabled() handles
        // the NR cluster's mutual exclusion (turning RN2 on disables
        // NR2/NR4/BNR/DFNR/MNR) and the lazy RNNoise allocation.
        if (rx.contains("rn2")) {
            m_engine->setRn2Enabled(jbool(rx, "rn2",
                                          m_engine->rn2Enabled()));
        }
    }

    // Persist the new engine state back to AppSettings so the values
    // survive an app restart.  Without this, the per-module setters
    // above only update in-memory state — on next launch the engine
    // would reload whatever AppSettings had BEFORE the preset was
    // applied, making it look like the preset never stuck.
    m_engine->saveClientGateSettings();
    m_engine->saveClientEqSettings();
    m_engine->saveClientCompSettings();
    m_engine->saveClientDeEssSettings();
    m_engine->saveClientTubeSettings();
    m_engine->saveClientPuduSettings();
    m_engine->saveClientReverbSettings();
    m_engine->saveClientFinalLimiterSettings();
    // RX-side persistence (#2425).  saveClientEqSettings above already
    // handles both Rx and Tx EQ; the rest are independent.
    m_engine->saveClientGateRxSettings();
    m_engine->saveClientCompRxSettings();
    m_engine->saveClientDeEssRxSettings();
    m_engine->saveClientTubeRxSettings();
    m_engine->saveClientPuduRxSettings();
    m_engine->saveClientRxChainOrder();
}

} // namespace AetherSDR
