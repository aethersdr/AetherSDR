#include "AetherTxProfiles.h"
#include "AudioEngine.h"
#include "ChannelStripPresets.h"
#include "SettingsPaths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>

#include <algorithm>

namespace AetherSDR {

namespace {

constexpr int kSchemaVersion = 1;

// Nothing here needs the noise-reduction cluster: RN2 on the mic path is a TX
// setting, but it lives with the tube pre-amp toggles rather than in the chain,
// and ChannelStripPresets already carries it.

} // namespace

AetherTxProfiles::AetherTxProfiles(AudioEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
    m_root["version"]  = kSchemaVersion;
    m_root["profiles"] = QJsonObject{};
    loadFromDisk();
    migrateLegacyPresets();
}

QString AetherTxProfiles::filePath() const
{
    // Share the settings store's cross-platform test/profile override.
    const QString dir = SettingsPaths::configDir();
    QDir().mkpath(dir);
    return dir + "/AetherTxProfiles.json";
}

void AetherTxProfiles::migrateLegacyPresets()
{
    static const QString kFlag = QStringLiteral("migratedFromChannelStrip");
    if (m_root.value(kFlag).toBool()) return;

    QFile f(ChannelStripPresets::legacyLibraryPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        // Nothing to import. The flag is set in memory only -- writing it
        // would create an empty library file for every operator who never
        // had a preset, to record an absence. The cost is that a launch
        // with no legacy file re-checks, which is one stat() and is also
        // the behaviour you want if a preset file is later restored from
        // a backup.
        m_root[kFlag] = true;
        return;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) { m_root[kFlag] = true; return; }

    const QJsonObject presets = doc.object().value(QStringLiteral("presets")).toObject();
    QJsonObject candidate = m_root;
    QJsonObject profiles  = candidate.value(QStringLiteral("profiles")).toObject();

    int imported = 0;
    for (auto it = presets.begin(); it != presets.end(); ++it) {
        if (!it.value().isObject()) continue;
        // A profile the operator already has under that name wins: this is a
        // one-way import, not a restore.
        if (profiles.contains(it.key())) continue;
        const QJsonObject preset = it.value().toObject();
        // The transmit half is the preset's top level; "rx" is the other
        // window's business and must not travel with it.
        QJsonObject profile = preset;
        profile.remove(QStringLiteral("rx"));
        profiles[it.key()] = profile;
        ++imported;
    }

    candidate[QStringLiteral("profiles")] = profiles;
    candidate[kFlag] = true;
    if (!writeDocument(candidate)) return;   // try again next launch
    m_root = candidate;
    if (imported > 0) emit profilesChanged();
}

bool AetherTxProfiles::loadFromDisk()
{
    QFile f(filePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    m_root = doc.object();
    if (!m_root.contains("profiles") || !m_root["profiles"].isObject())
        m_root["profiles"] = QJsonObject{};
    if (!m_root.contains("version"))
        m_root["version"] = kSchemaVersion;
    return true;
}

// Atomic, and checked at every step (Constitution XIV). Written this way
// because the store this one replaces is not: ChannelStripPresets::saveToDisk()
// opens the live library WriteOnly|Truncate, which destroys every saved preset
// before a single replacement byte is written — an interrupted save takes the
// lot — and ignores the write result, so a full disk reports success and the
// dialog says "Saved". This file has no "previous version" of its own; that
// sibling is what the comparison is to.
bool AetherTxProfiles::writeDocument(const QJsonObject& root) const
{
    QSaveFile f(filePath());
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    // commit() is the rename; until it returns true the old file is intact.
    return f.commit();
}

QStringList AetherTxProfiles::profileNames() const
{
    QStringList names = m_root.value("profiles").toObject().keys();
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return names;
}

bool AetherTxProfiles::hasProfile(const QString& name) const
{
    return m_root.value("profiles").toObject().contains(name);
}

bool AetherTxProfiles::saveFromCurrent(const QString& name)
{
    if (name.trimmed().isEmpty() || !m_engine) return false;

    QJsonObject p = ChannelStripPresets::captureTxJson(m_engine);
    p["createdBy"] = QStringLiteral("AetherSDR ") + QCoreApplication::applicationVersion();
    p["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return addProfile(name.trimmed(), p);
}

bool AetherTxProfiles::addProfile(const QString& name, const QJsonObject& profile)
{
    if (name.trimmed().isEmpty()) return false;
    QJsonObject profiles = m_root.value("profiles").toObject();
    QJsonObject stored = profile;
    // The name is the key; a copy inside the object would be a second place
    // for it to be wrong after a rename.
    stored.remove(QStringLiteral("name"));
    profiles[name.trimmed()] = stored;

    // Candidate first, publish second: a refused write must leave the
    // in-memory library exactly as the operator last saw it, or the dialog
    // would list a profile that is not on disk.
    QJsonObject candidate = m_root;
    candidate["profiles"] = profiles;
    candidate["version"]  = kSchemaVersion;
    if (!writeDocument(candidate)) return false;
    m_root = candidate;
    emit profilesChanged();
    return true;
}

bool AetherTxProfiles::loadProfile(const QString& name)
{
    const QJsonObject profiles = m_root.value("profiles").toObject();
    if (!profiles.contains(name) || !m_engine) return false;
    const QJsonObject p = profiles.value(name).toObject();

    ChannelStripPresets::applyTxJson(m_engine, p);
    return true;
}

bool AetherTxProfiles::deleteProfile(const QString& name)
{
    QJsonObject profiles = m_root.value("profiles").toObject();
    if (!profiles.contains(name)) return false;
    profiles.remove(name);

    QJsonObject candidate = m_root;
    candidate["profiles"] = profiles;
    if (!writeDocument(candidate)) return false;
    m_root = candidate;
    emit profilesChanged();
    return true;
}

bool AetherTxProfiles::exportToFile(const QString& name, const QString& filePath) const
{
    const QJsonObject profiles = m_root.value("profiles").toObject();
    if (!profiles.contains(name)) return false;

    QJsonObject out = profiles.value(name).toObject();
    // A standalone file says what it is and what it was called, so the
    // importer has something to suggest and a reader can tell at a glance.
    out["name"]    = name;
    out["kind"]    = QStringLiteral("AetherTX profile");
    out["version"] = kSchemaVersion;

    // Same contract as the library write: an export that fails must not leave
    // a truncated file behind, and must not tell the operator it succeeded.
    QSaveFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(out).toJson(QJsonDocument::Indented);
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

QJsonObject AetherTxProfiles::readFile(const QString& path,
                                       QString* suggestedName,
                                       QString* error)
{
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QJsonObject{};
    };

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return fail(QObject::tr("Could not open %1.").arg(path));
    }
    QJsonParseError pe{};
    const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        return fail(QObject::tr("Not valid JSON: %1").arg(pe.errorString()));
    }
    if (!doc.isObject()) {
        return fail(QObject::tr("The file does not contain a JSON object."));
    }
    const QJsonObject root = doc.object();

    // Honour "kind" when it is there. The sniffing below deliberately does not
    // require it, so a hand-written file still imports -- but the two windows'
    // exports overlap almost completely (same stage keys, overlapping chain
    // names), so without this check an AetherRX export imports cleanly here and
    // writes receive-side values into the transmit chain.
    {
        const QString kind = root.value(QStringLiteral("kind")).toString();
        if (!kind.isEmpty() && kind != QStringLiteral("AetherTX profile")) {
            return fail(QObject::tr("%1 holds a \u201c%2\u201d, not an AetherTX profile.")
                            .arg(QFileInfo(path).fileName(), kind));
        }
    }

    // A library: take its first profile.
    if (root.value("profiles").isObject()) {
        const QJsonObject profiles = root.value("profiles").toObject();
        if (profiles.isEmpty()) {
            return fail(QObject::tr("The file contains no profiles."));
        }
        const QString first = profiles.keys().constFirst();
        if (suggestedName) *suggestedName = first;
        return profiles.value(first).toObject();
    }

    // A single exported profile. Recognised by carrying at least one stage,
    // rather than by its "kind", so a hand-written file still imports.
    static const char* kStageKeys[] = { "gate", "eq", "deess", "comp",
                                        "tube", "pudu", "reverb" };
    const bool looksLikeProfile =
        root.contains(QStringLiteral("chain"))
        || std::any_of(std::begin(kStageKeys), std::end(kStageKeys),
                       [&root](const char* k) {
                           return root.value(QLatin1String(k)).isObject();
                       });
    if (!looksLikeProfile) {
        return fail(QObject::tr(
            "This does not look like an AetherTX profile — it has no chain "
            "and no transmit stages in it."));
    }
    if (suggestedName) {
        *suggestedName = root.value("name").toString(
            QFileInfo(path).completeBaseName());
    }
    return root;
}

QString AetherTxProfiles::uniqueName(const QString& desired) const
{
    const QString base = desired.trimmed().isEmpty()
        ? QObject::tr("Imported profile")
        : desired.trimmed();
    if (!hasProfile(base)) return base;
    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(n);
        if (!hasProfile(candidate)) return candidate;
    }
    return base;
}

} // namespace AetherSDR
