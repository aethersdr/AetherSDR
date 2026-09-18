#include "AetherRxProfiles.h"
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

// Which noise-reduction method is running. The cluster is exclusive, so this
// is one name or none — the per-module tuning stays in AppSettings, because a
// profile is about how the chain is arranged, not a second copy of every
// slider in the app.
QString activeNrMethod(AudioEngine* e)
{
    if (!e) return {};
    if (e->nr2Enabled())    return QStringLiteral("NR2");
    if (e->nr4Enabled())    return QStringLiteral("NR4");
    if (e->mnrEnabled())    return QStringLiteral("MNR");
    if (e->dfnrEnabled())   return QStringLiteral("DFNR");
    if (e->rn2Enabled())    return QStringLiteral("RN2");
    if (e->nvAfxEnabled())  return QStringLiteral("BNR");
    if (e->nnrEnabled())    return QStringLiteral("NNR");
    return QStringLiteral("Off");
}

// Switch the cluster to one method, or all of it off. Each setter handles the
// mutual exclusion itself, so turning the wanted one on is enough — but a
// profile that says "Off" has to say so explicitly.
void applyNrMethod(AudioEngine* e, const QString& method)
{
    if (!e || method.isEmpty()) return;
    const auto off = [e]() {
        if (e->nr2Enabled())   e->setNr2Enabled(false);
        if (e->nr4Enabled())   e->setNr4Enabled(false);
        if (e->mnrEnabled())   e->setMnrEnabled(false);
        if (e->dfnrEnabled())  e->setDfnrEnabled(false);
        if (e->rn2Enabled())   e->setRn2Enabled(false);
        if (e->nvAfxEnabled()) e->setNvAfxEnabled(false);
        if (e->nnrEnabled())   e->setNnrEnabled(false);
    };
    if (method.compare(QLatin1String("Off"), Qt::CaseInsensitive) == 0) {
        off();
        return;
    }
    off();
    if (method == QLatin1String("NR2"))       e->setNr2Enabled(true);
    else if (method == QLatin1String("NR4"))  e->setNr4Enabled(true);
    else if (method == QLatin1String("MNR"))  e->setMnrEnabled(true);
    else if (method == QLatin1String("DFNR")) e->setDfnrEnabled(true);
    else if (method == QLatin1String("RN2"))  e->setRn2Enabled(true);
    else if (method == QLatin1String("BNR"))  e->setNvAfxEnabled(true);
    else if (method == QLatin1String("NNR"))  e->setNnrEnabled(true);
}

} // namespace

AetherRxProfiles::AetherRxProfiles(AudioEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
{
    m_root["version"]  = kSchemaVersion;
    m_root["profiles"] = QJsonObject{};
    loadFromDisk();
}

QString AetherRxProfiles::filePath() const
{
    // Share the settings store's cross-platform test/profile override.
    const QString dir = SettingsPaths::configDir();
    QDir().mkpath(dir);
    return dir + "/AetherRxProfiles.json";
}

bool AetherRxProfiles::loadFromDisk()
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

// Atomic, and checked at every step (Constitution XIV). The previous version
// opened the live library WriteOnly|Truncate, which destroys every saved
// profile before a single replacement byte is written — an interrupted save
// took the lot — and ignored the write result, so a full disk reported
// success and the dialog said "Saved".
bool AetherRxProfiles::writeDocument(const QJsonObject& root) const
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

QStringList AetherRxProfiles::profileNames() const
{
    QStringList names = m_root.value("profiles").toObject().keys();
    std::sort(names.begin(), names.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return names;
}

bool AetherRxProfiles::hasProfile(const QString& name) const
{
    return m_root.value("profiles").toObject().contains(name);
}

bool AetherRxProfiles::saveFromCurrent(const QString& name)
{
    if (name.trimmed().isEmpty() || !m_engine) return false;

    QJsonObject p = ChannelStripPresets::captureRxJson(m_engine);
    p["createdBy"] = QStringLiteral("AetherSDR ") + QCoreApplication::applicationVersion();
    p["createdAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    p["nr"] = QJsonObject{{ QStringLiteral("method"), activeNrMethod(m_engine) }};

    return addProfile(name.trimmed(), p);
}

bool AetherRxProfiles::addProfile(const QString& name, const QJsonObject& profile)
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

bool AetherRxProfiles::loadProfile(const QString& name)
{
    const QJsonObject profiles = m_root.value("profiles").toObject();
    if (!profiles.contains(name) || !m_engine) return false;
    const QJsonObject p = profiles.value(name).toObject();

    ChannelStripPresets::applyRxJson(m_engine, p);
    if (p.value("nr").isObject()) {
        applyNrMethod(m_engine, p.value("nr").toObject()
                                 .value("method").toString());
    }
    return true;
}

bool AetherRxProfiles::deleteProfile(const QString& name)
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

bool AetherRxProfiles::exportToFile(const QString& name, const QString& filePath) const
{
    const QJsonObject profiles = m_root.value("profiles").toObject();
    if (!profiles.contains(name)) return false;

    QJsonObject out = profiles.value(name).toObject();
    // A standalone file says what it is and what it was called, so the
    // importer has something to suggest and a reader can tell at a glance.
    out["name"]    = name;
    out["kind"]    = QStringLiteral("AetherRX profile");
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

QJsonObject AetherRxProfiles::readFile(const QString& path,
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
    static const char* kStageKeys[] = { "gate", "eq", "comp", "tube", "pudu" };
    const bool looksLikeProfile =
        root.contains(QStringLiteral("chain"))
        || std::any_of(std::begin(kStageKeys), std::end(kStageKeys),
                       [&root](const char* k) {
                           return root.value(QLatin1String(k)).isObject();
                       });
    if (!looksLikeProfile) {
        return fail(QObject::tr(
            "This does not look like an AetherRX profile — it has no chain "
            "and no receive stages in it."));
    }
    if (suggestedName) {
        *suggestedName = root.value("name").toString(
            QFileInfo(path).completeBaseName());
    }
    return root;
}

QString AetherRxProfiles::uniqueName(const QString& desired) const
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
