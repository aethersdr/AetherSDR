#include "KiwiSdrManager.h"

#include "AppSettings.h"
#include "LogManager.h"
#include "ShutdownTrace.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaType>
#include <QSaveFile>
#include <QStringConverter>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace AetherSDR {
namespace {

constexpr const char* kKiwiSdrRxAntennasSettingsKey = "KiwiSdrRxAntennas";
constexpr const char* kVirtualAntennaPrefix = "KIWI:";
constexpr int kRecoverableReconnectDelayMs = 3000;
constexpr int kKiwiSdrProfileNameMaxChars = 16;
constexpr int kKiwiSdrWaterfallRateMax = 4;
constexpr int kKiwiSdrWaterfallMinDbmLimit = -260;
constexpr int kKiwiSdrWaterfallMaxDbmLimit = 30;
constexpr int kKiwiSdrDefaultWaterfallMinDbm = -110;
constexpr int kKiwiSdrDefaultWaterfallMaxDbm = -10;

QString normalizedProfileEndpoint(const QString& endpoint)
{
    return KiwiSdrClient::normalizeEndpoint(endpoint);
}

// ── Receiver-list CSV (#4586) ────────────────────────────────────────────
// A deliberately small, self-contained CSV reader/writer rather than reusing
// MemoryCsvCompat (multi-dialect memory channels) or ShortcutManager's parser
// (keybinding collision semantics) — neither is a good fit for a flat list
// of receiver profiles, and this repo's existing CSV consumers each own a
// parser tailored to their own schema rather than sharing one.

constexpr qsizetype kMaxKiwiCsvBytes = 256 * 1024;
constexpr int kMaxKiwiCsvRows = 2000;
constexpr int kMaxKiwiCsvFieldLength = 512;
constexpr int kKiwiSdrCsvSchemaVersion = 1;

// Only FORMAT_VERSION/NAME/ENDPOINT are required. The remaining columns
// (AUTO_CONNECT, KEEP_AUDIO_DURING_TX, RESUME_AUDIO_AFTER_TX_DELAY,
// WATERFALL_AUTO_SCALE, WATERFALL_MIN_DBM, WATERFALL_MAX_DBM,
// WATERFALL_RATE) are optional on the way in: an absent one falls back to
// the struct's own default, so an older export or a hand-trimmed CSV still
// imports cleanly.
const QStringList kKiwiSdrCsvHeader{
    QStringLiteral("FORMAT_VERSION"),
    QStringLiteral("NAME"),
    QStringLiteral("ENDPOINT"),
};

struct KiwiCsvRow {
    int lineNumber{0};
    QStringList fields;
};

struct ParsedKiwiCsvRow {
    int lineNumber{0};
    QString name;
    QString endpoint;
    bool autoConnect{false};
    bool keepAudioDuringTx{false};
    bool resumeAudioAfterTxDelay{false};
    bool waterfallAutoScale{true};
    int waterfallMinDbm{kKiwiSdrDefaultWaterfallMinDbm};
    int waterfallMaxDbm{kKiwiSdrDefaultWaterfallMaxDbm};
    int waterfallRate{0};
};

QString kiwiCsvEscape(QString value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + value + QLatin1Char('"');
    }
    return value;
}

QString kiwiCsvBool(bool value)
{
    return value ? QStringLiteral("True") : QStringLiteral("False");
}

bool appendKiwiCsvField(KiwiCsvRow& row, QString& field, QStringList& errors)
{
    if (field.size() > kMaxKiwiCsvFieldLength) {
        errors << QStringLiteral("Line %1: a field exceeds %2 characters.")
                      .arg(row.lineNumber)
                      .arg(kMaxKiwiCsvFieldLength);
        return false;
    }
    row.fields << field;
    field.clear();
    return true;
}

// RFC4180-style tokenizer (quotes, embedded commas/newlines) — the same
// mechanics as ShortcutManager's parseCsvRows, sized down for a much smaller
// file (receiver lists run to dozens of rows, not thousands).
QList<KiwiCsvRow> parseKiwiCsvRows(const QByteArray& bytes, QStringList& errors)
{
    QList<KiwiCsvRow> rows;
    if (bytes.size() > kMaxKiwiCsvBytes) {
        errors << QStringLiteral("KiwiSDR receiver CSV exceeds the 256 KiB size limit.");
        return rows;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        errors << QStringLiteral("KiwiSDR receiver CSV is not valid UTF-8.");
        return rows;
    }
    if (!text.isEmpty() && text.front() == QChar(0xfeff)) {
        text.remove(0, 1);
    }

    KiwiCsvRow row;
    row.lineNumber = 1;
    QString field;
    bool inQuotes = false;
    bool afterQuote = false;
    int lineNumber = 1;
    int quoteOpenedLine = 0;

    auto finishRow = [&]() -> bool {
        if (!appendKiwiCsvField(row, field, errors)) {
            return false;
        }
        const bool blank = row.fields.size() == 1 && row.fields.constFirst().isEmpty();
        if (!blank) {
            rows << row;
            if (rows.size() > kMaxKiwiCsvRows + 1) {
                errors << QStringLiteral("KiwiSDR receiver CSV exceeds the %1-row limit.")
                              .arg(kMaxKiwiCsvRows);
                return false;
            }
        }
        row = KiwiCsvRow{};
        afterQuote = false;
        return true;
    };

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (inQuotes) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    inQuotes = false;
                    afterQuote = true;
                }
            } else {
                field += ch;
                if (ch == QLatin1Char('\n')) {
                    ++lineNumber;
                } else if (ch == QLatin1Char('\r')
                           && (i + 1 >= text.size()
                               || text.at(i + 1) != QLatin1Char('\n'))) {
                    ++lineNumber;
                }
            }
            continue;
        }

        if (afterQuote && ch != QLatin1Char(',') && ch != QLatin1Char('\r')
            && ch != QLatin1Char('\n')) {
            errors << QStringLiteral("Line %1: unexpected character after a quoted field.")
                          .arg(lineNumber);
            return {};
        }
        if (ch == QLatin1Char('"')) {
            if (!field.isEmpty() || afterQuote) {
                errors << QStringLiteral("Line %1: unexpected quote in an unquoted field.")
                              .arg(lineNumber);
                return {};
            }
            inQuotes = true;
            quoteOpenedLine = lineNumber;
            continue;
        }
        if (ch == QLatin1Char(',')) {
            if (!appendKiwiCsvField(row, field, errors)) {
                return {};
            }
            afterQuote = false;
            continue;
        }
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            if (!finishRow()) {
                return {};
            }
            if (ch == QLatin1Char('\r') && i + 1 < text.size()
                && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            ++lineNumber;
            row.lineNumber = lineNumber;
            continue;
        }
        field += ch;
    }

    if (inQuotes) {
        errors << QStringLiteral("Line %1: unterminated quoted field.").arg(quoteOpenedLine);
        return {};
    }
    if (!field.isEmpty() || !row.fields.isEmpty()) {
        if (!finishRow()) {
            return {};
        }
    }
    return rows;
}

// Accepts True/False/1/0/Yes/No case-insensitively; anything else is invalid.
bool parseKiwiCsvBool(const QString& text, bool defaultValue, bool& valid)
{
    const QString lower = text.trimmed().toLower();
    if (lower.isEmpty()) {
        valid = true;
        return defaultValue;
    }
    if (lower == QLatin1String("true") || lower == QLatin1String("1")
        || lower == QLatin1String("yes")) {
        valid = true;
        return true;
    }
    if (lower == QLatin1String("false") || lower == QLatin1String("0")
        || lower == QLatin1String("no")) {
        valid = true;
        return false;
    }
    valid = false;
    return defaultValue;
}

QList<ParsedKiwiCsvRow> parseKiwiSdrCsv(const QByteArray& bytes, QStringList& errors)
{
    const QList<KiwiCsvRow> rows = parseKiwiCsvRows(bytes, errors);
    if (!errors.isEmpty()) {
        return {};
    }
    if (rows.isEmpty()) {
        errors << QStringLiteral("KiwiSDR receiver CSV is empty.");
        return {};
    }

    QHash<QString, int> columns;
    for (int i = 0; i < rows.constFirst().fields.size(); ++i) {
        const QString name = rows.constFirst().fields.at(i).trimmed().toUpper();
        if (name.isEmpty() || columns.contains(name)) {
            errors << QStringLiteral("Line 1: empty or duplicate column name '%1'.").arg(name);
            return {};
        }
        columns.insert(name, i);
    }
    for (const QString& required : kKiwiSdrCsvHeader) {
        if (!columns.contains(required)) {
            errors << QStringLiteral("Line 1: missing required column %1.").arg(required);
        }
    }
    if (!errors.isEmpty()) {
        return {};
    }

    QList<ParsedKiwiCsvRow> parsed;
    for (int i = 1; i < rows.size(); ++i) {
        const KiwiCsvRow& row = rows.at(i);
        const auto value = [&row, &columns](const QString& name) {
            const int index = columns.value(name, -1);
            return index >= 0 && index < row.fields.size() ? row.fields.at(index).trimmed()
                                                           : QString();
        };

        ParsedKiwiCsvRow parsedRow;
        parsedRow.lineNumber = row.lineNumber;
        parsedRow.name = value(QStringLiteral("NAME"));
        parsedRow.endpoint = value(QStringLiteral("ENDPOINT"));
        if (parsedRow.name.isEmpty()) {
            errors << QStringLiteral("Line %1: NAME is required.").arg(row.lineNumber);
        }
        if (parsedRow.endpoint.isEmpty()) {
            errors << QStringLiteral("Line %1: ENDPOINT is required.").arg(row.lineNumber);
        }

        auto boolField = [&](const QString& column, bool defaultValue) {
            bool fieldOk = true;
            const bool result = parseKiwiCsvBool(value(column), defaultValue, fieldOk);
            if (!fieldOk) {
                errors << QStringLiteral("Line %1: %2 must be True or False.")
                              .arg(row.lineNumber).arg(column);
            }
            return result;
        };
        parsedRow.autoConnect = boolField(QStringLiteral("AUTO_CONNECT"), false);
        parsedRow.keepAudioDuringTx = boolField(QStringLiteral("KEEP_AUDIO_DURING_TX"), false);
        parsedRow.resumeAudioAfterTxDelay =
            boolField(QStringLiteral("RESUME_AUDIO_AFTER_TX_DELAY"), false);
        parsedRow.waterfallAutoScale = boolField(QStringLiteral("WATERFALL_AUTO_SCALE"), true);

        // Out-of-range values are not an error here — updateProfile() clamps
        // them once the row is applied, same as a value typed into the UI
        // dialog would be. Only a non-numeric field fails the import.
        auto intField = [&](const QString& column, int defaultValue) {
            const QString text = value(column);
            if (text.isEmpty()) {
                return defaultValue;
            }
            bool fieldOk = false;
            const int result = text.toInt(&fieldOk);
            if (!fieldOk) {
                errors << QStringLiteral("Line %1: %2 must be an integer.")
                              .arg(row.lineNumber).arg(column);
            }
            return result;
        };
        parsedRow.waterfallMinDbm =
            intField(QStringLiteral("WATERFALL_MIN_DBM"), kKiwiSdrDefaultWaterfallMinDbm);
        parsedRow.waterfallMaxDbm =
            intField(QStringLiteral("WATERFALL_MAX_DBM"), kKiwiSdrDefaultWaterfallMaxDbm);
        parsedRow.waterfallRate = intField(QStringLiteral("WATERFALL_RATE"), 0);

        parsed << parsedRow;
    }
    return parsed;
}

} // namespace

KiwiSdrManager::KiwiSdrManager(
    QObject* parent,
    std::shared_ptr<IKiwiSdrCredentialStore> credentialStore)
    : QObject(parent)
    , m_credentialStore(credentialStore
                            ? std::move(credentialStore)
                            : createDefaultKiwiSdrCredentialStore())
{
    qRegisterMetaType<AetherSDR::KiwiSdrClient::State>(
        "AetherSDR::KiwiSdrClient::State");
    qRegisterMetaType<AetherSDR::KiwiSdrReceiverTelemetry>(
        "AetherSDR::KiwiSdrReceiverTelemetry");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::ReceiverMetadata>(
        "AetherSDR::KiwiSdrProtocol::ReceiverMetadata");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::ProtocolState>(
        "AetherSDR::KiwiSdrProtocol::ProtocolState");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::MeterReading>(
        "AetherSDR::KiwiSdrProtocol::MeterReading");
    qRegisterMetaType<QVector<float>>("QVector<float>");
    qRegisterMetaType<AetherSDR::KiwiSdrPasswordPersistenceState>(
        "AetherSDR::KiwiSdrPasswordPersistenceState");
    loadSettings();
    for (const KiwiSdrAntennaProfile& profile : std::as_const(m_profiles)) {
        loadProfilePassword(profile.id);
    }
}

KiwiSdrManager::~KiwiSdrManager()
{
    {
        ShutdownTrace trace("kiwi.clients.destroy");
        disconnectAll();
        const QStringList ids = m_clients.keys();
        for (const QString& id : ids) {
            destroyClient(id, true);
        }
    }
    if (m_clientThread) {
        ShutdownTrace trace("kiwi.thread.join");
        m_clientThread->quit();
        if (!m_clientThread->wait(3000)) {
            trace.fail("thread_join_timeout");
            qCWarning(lcKiwiSdr)
                << "KiwiSDR client thread did not stop during manager teardown";
            m_clientThread->setParent(nullptr);
            connect(m_clientThread, &QThread::finished,
                    m_clientThread, &QObject::deleteLater);
        }
    }
}

KiwiSdrAntennaProfile KiwiSdrManager::profile(const QString& id) const
{
    const int idx = profileIndex(id);
    return idx >= 0 ? m_profiles[idx] : KiwiSdrAntennaProfile{};
}

bool KiwiSdrManager::hasProfile(const QString& id) const
{
    return profileIndex(id) >= 0;
}

QString KiwiSdrManager::displayName(const QString& id) const
{
    const KiwiSdrAntennaProfile p = profile(id);
    if (!p.name.trimmed().isEmpty()) {
        return p.name.trimmed();
    }
    if (!p.endpoint.isEmpty()) {
        return p.endpoint;
    }
    return tr("KiwiSDR");
}

QString KiwiSdrManager::virtualAntennaToken(const QString& id) const
{
    return QStringLiteral("%1%2").arg(QString::fromLatin1(kVirtualAntennaPrefix), id);
}

QString KiwiSdrManager::profileIdForVirtualAntennaToken(const QString& token) const
{
    return token.startsWith(QString::fromLatin1(kVirtualAntennaPrefix))
        ? token.mid(QString::fromLatin1(kVirtualAntennaPrefix).size())
        : QString();
}

QStringList KiwiSdrManager::virtualAntennaTokens() const
{
    QStringList tokens;
    tokens.reserve(m_profiles.size());
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        tokens.append(virtualAntennaToken(p.id));
    }
    return tokens;
}

QStringList KiwiSdrManager::virtualAntennaLabels() const
{
    QStringList labels;
    labels.reserve(m_profiles.size());
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        labels.append(displayName(p.id));
    }
    return labels;
}

KiwiSdrClient::State KiwiSdrManager::state(const QString& id) const
{
    return m_states.value(id, KiwiSdrClient::State::Disconnected);
}

QString KiwiSdrManager::stateDetail(const QString& id) const
{
    return m_stateDetails.value(id);
}

KiwiSdrReceiverTelemetry KiwiSdrManager::telemetry(const QString& id) const
{
    return m_telemetry.value(id);
}

KiwiSdrProtocol::ReceiverMetadata KiwiSdrManager::receiverMetadata(
    const QString& id) const
{
    return m_telemetry.value(id).metadata;
}

KiwiSdrProtocol::ProtocolState KiwiSdrManager::protocolState(
    const QString& id) const
{
    return m_telemetry.value(id).protocol;
}

bool KiwiSdrManager::waterfallAvailable(const QString& id) const
{
    return m_waterfallAvailable.value(id, true);
}

QString KiwiSdrManager::waterfallDetail(const QString& id) const
{
    return m_waterfallDetails.value(id);
}

KiwiSdrWaterfallDisplayRange KiwiSdrManager::waterfallDisplayRange(
    const QString& id) const
{
    return m_waterfallDisplayRanges.value(id);
}

bool KiwiSdrManager::isConnected(const QString& id) const
{
    return KiwiSdrClient::stateHasReceiveAudio(state(id));
}

bool KiwiSdrManager::reconnectRecommended(const QString& id) const
{
    if (state(id) != KiwiSdrClient::State::Waiting) {
        return false;
    }

    const KiwiSdrProtocol::ReceiverMetadata metadata =
        m_telemetry.value(id).metadata;
    return metadata.hasCampQueueReloadRecommended
        && metadata.campQueueReloadRecommended;
}

QString KiwiSdrManager::assignedProfileForSlice(int sliceId) const
{
    return m_sliceAssignments.value(sliceId);
}

int KiwiSdrManager::assignedSliceForProfile(const QString& id) const
{
    for (auto it = m_sliceAssignments.constBegin(); it != m_sliceAssignments.constEnd(); ++it) {
        if (it.value() == id) {
            return it.key();
        }
    }
    return -1;
}

QString KiwiSdrManager::profilePassword(const QString& id) const
{
    return m_profilePasswords.value(id);
}

bool KiwiSdrManager::isProfilePasswordLoaded(const QString& id) const
{
    return m_loadedProfilePasswords.contains(id);
}

KiwiSdrPasswordPersistenceState
KiwiSdrManager::profilePasswordPersistenceState(const QString& id) const
{
    return m_profilePasswordPersistenceStates.value(
        id, KiwiSdrPasswordPersistenceState::Loading);
}

QString KiwiSdrManager::profilePasswordPersistenceDetail(
    const QString& id) const
{
    return m_profilePasswordPersistenceDetails.value(id);
}

QString KiwiSdrManager::addProfile(const QString& name, const QString& endpoint)
{
    const QString normalizedEndpoint = normalizedProfileEndpoint(endpoint);
    const QString displayName = name.trimmed().left(kKiwiSdrProfileNameMaxChars);
    if (displayName.isEmpty() || normalizedEndpoint.isEmpty()) {
        return QString();
    }

    KiwiSdrAntennaProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.endpoint = normalizedEndpoint;
    profile.name = displayName;
    m_profiles.append(profile);
    saveSettings();
    qCInfo(lcKiwiSdr).noquote()
        << "Profile added" << profile.name << "endpoint=" << profile.endpoint
        << "id=" << profile.id;
    emit profilesChanged();
    return profile.id;
}

void KiwiSdrManager::updateProfile(const KiwiSdrAntennaProfile& profile)
{
    const int idx = profileIndex(profile.id);
    if (idx < 0) {
        return;
    }

    KiwiSdrAntennaProfile updated = profile;
    updated.endpoint = normalizedProfileEndpoint(profile.endpoint);
    updated.name = sanitizedName(profile.name, updated.endpoint);
    updated.waterfallMinDbm = std::clamp(updated.waterfallMinDbm,
                                         kKiwiSdrWaterfallMinDbmLimit,
                                         kKiwiSdrWaterfallMaxDbmLimit - 1);
    updated.waterfallMaxDbm = std::clamp(updated.waterfallMaxDbm,
                                         updated.waterfallMinDbm + 1,
                                         kKiwiSdrWaterfallMaxDbmLimit);
    updated.waterfallRate =
        std::clamp(updated.waterfallRate, 0, kKiwiSdrWaterfallRateMax);
    const QString oldEndpoint = m_profiles[idx].endpoint;
    const bool wasAutoConnect = m_profiles[idx].autoConnect;
    m_profiles[idx] = updated;
    saveSettings();
    const bool endpointChanged = oldEndpoint != updated.endpoint;
    if (endpointChanged) {
        cancelReconnect(updated.id);
        m_waterfallDisplayRanges.remove(updated.id);
        emit profileStreamReset(updated.id);
    }

    if (KiwiSdrClient* c = client(updated.id)) {
        Q_UNUSED(c);
        invokeClient(updated.id, [minDbm = updated.waterfallMinDbm,
                                  maxDbm = updated.waterfallMaxDbm,
                                  autoScale = updated.waterfallAutoScale,
                                  rate = updated.waterfallRate,
                                  endpointChanged,
                                  endpoint = updated.endpoint,
                                  password = profilePassword(updated.id),
                                  reconnect = state(updated.id)
                                      != KiwiSdrClient::State::Disconnected](
                                     KiwiSdrClient* client) {
            client->setWaterfallDisplayRange(minDbm, maxDbm, autoScale);
            client->setWaterfallRateOverride(rate);
            if (endpointChanged && reconnect) {
                client->disconnectFromEndpoint();
                if (!endpoint.isEmpty()) {
                    client->connectToEndpoint(endpoint, password);
                }
            }
        });
    }

    // Turning off auto-connect on a profile that no slice is using leaves it
    // "no longer in use" (no assigned slice, not auto-connect) — release it so
    // it stops squatting the receiver's user slot / per-IP time budget, the same
    // disconnect-when-idle invariant as the slice-transition paths (#3950).
    // Scoped to the true->false transition so plain name/endpoint edits and
    // manually-connected profiles are left untouched.
    if (wasAutoConnect && !updated.autoConnect
        && !shouldMaintainProfileConnection(updated.id)) {
        disconnectProfile(updated.id);
    }

    emit profilesChanged();
}

QByteArray KiwiSdrManager::exportProfilesCsv() const
{
    QStringList lines;
    lines << QStringLiteral(
        "FORMAT_VERSION,NAME,ENDPOINT,AUTO_CONNECT,KEEP_AUDIO_DURING_TX,"
        "RESUME_AUDIO_AFTER_TX_DELAY,WATERFALL_AUTO_SCALE,WATERFALL_MIN_DBM,"
        "WATERFALL_MAX_DBM,WATERFALL_RATE");
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        const QStringList fields{
            QString::number(kKiwiSdrCsvSchemaVersion),
            kiwiCsvEscape(p.name),
            kiwiCsvEscape(p.endpoint),
            kiwiCsvBool(p.autoConnect),
            kiwiCsvBool(p.keepAudioDuringTx),
            kiwiCsvBool(p.resumeAudioAfterTxDelay),
            kiwiCsvBool(p.waterfallAutoScale),
            QString::number(p.waterfallMinDbm),
            QString::number(p.waterfallMaxDbm),
            QString::number(p.waterfallRate),
        };
        lines << fields.join(QLatin1Char(','));
    }
    return lines.join(QStringLiteral("\r\n")).toUtf8() + QByteArray("\r\n");
}

KiwiSdrCsvImportResult KiwiSdrManager::importProfilesCsv(const QByteArray& bytes)
{
    KiwiSdrCsvImportResult result;
    const QList<ParsedKiwiCsvRow> rows = parseKiwiSdrCsv(bytes, result.errors);
    if (!result.ok()) {
        return result;
    }

    for (const ParsedKiwiCsvRow& row : rows) {
        const QString normalizedEndpoint = normalizedProfileEndpoint(row.endpoint);
        if (normalizedEndpoint.isEmpty() || row.name.trimmed().isEmpty()) {
            result.errors << QStringLiteral(
                "Line %1: NAME and ENDPOINT are required.").arg(row.lineNumber);
            continue;
        }

        int existingIdx = -1;
        for (int i = 0; i < m_profiles.size(); ++i) {
            if (m_profiles.at(i).endpoint == normalizedEndpoint) {
                existingIdx = i;
                break;
            }
        }

        // Merge on a matching endpoint (#4586): update the existing profile
        // in place (keeping its id and password) instead of creating a
        // duplicate, so re-importing the same file — e.g. after syncing
        // between machines — is idempotent. A brand-new id is always minted
        // for a new row via the same addProfile() path the UI's "Add
        // receiver" button uses, so validation/truncation/logging match.
        QString id;
        if (existingIdx >= 0) {
            id = m_profiles.at(existingIdx).id;
        } else {
            id = addProfile(row.name, row.endpoint);
            if (id.isEmpty()) {
                result.errors << QStringLiteral(
                    "Line %1: '%2' has no usable name/endpoint.")
                    .arg(row.lineNumber).arg(row.name);
                continue;
            }
        }

        KiwiSdrAntennaProfile updated = profile(id);
        updated.name = row.name;
        updated.endpoint = row.endpoint;
        updated.autoConnect = row.autoConnect;
        updated.keepAudioDuringTx = row.keepAudioDuringTx;
        updated.resumeAudioAfterTxDelay = row.resumeAudioAfterTxDelay;
        updated.waterfallAutoScale = row.waterfallAutoScale;
        updated.waterfallMinDbm = row.waterfallMinDbm;
        updated.waterfallMaxDbm = row.waterfallMaxDbm;
        updated.waterfallRate = row.waterfallRate;
        updateProfile(updated); // clamps waterfall fields, persists, emits

        if (existingIdx >= 0) {
            ++result.mergedCount;
        } else {
            ++result.addedCount;
        }
    }
    return result;
}

KiwiSdrCsvExportResult KiwiSdrManager::exportToFile(const QString& path) const
{
    KiwiSdrCsvExportResult result;
    if (path.trimmed().isEmpty()) {
        result.error = QStringLiteral("No export path was provided.");
        return result;
    }

    const QByteArray csv = exportProfilesCsv();
    QSaveFile file(path);
    // Some network mounts (SMB, WSL DrvFs) can't create the sidecar temp file
    // QSaveFile normally uses — fall back to a direct write so export
    // succeeds where a plain write would (matches ShortcutManager).
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Couldn't open %1 for writing (%2).")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    if (file.write(csv) != csv.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        result.error = QStringLiteral("Couldn't write the receiver list to %1 (%2).")
                           .arg(QDir::toNativeSeparators(path), reason);
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("Couldn't save %1 (%2).")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    result.exportedCount = m_profiles.size();
    return result;
}

KiwiSdrCsvImportResult KiwiSdrManager::importFromFile(const QString& path)
{
    KiwiSdrCsvImportResult result;
    if (path.trimmed().isEmpty()) {
        result.errors << QStringLiteral("No import path was provided.");
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors << QStringLiteral("Couldn't open %1 for reading (%2).")
                             .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    return importProfilesCsv(file.readAll());
}

void KiwiSdrManager::setProfilePassword(const QString& id,
                                        const QString& password)
{
    if (!hasProfile(id)) {
        return;
    }

    const bool changed = m_profilePasswords.value(id) != password;
    const bool retryAfterError =
        profilePasswordPersistenceState(id)
        == KiwiSdrPasswordPersistenceState::Error;
    if (!changed && m_loadedProfilePasswords.contains(id)
        && !retryAfterError) {
        return;
    }

    m_profilePasswordRevisions.insert(
        id, m_profilePasswordRevisions.value(id) + 1);
    m_profilePasswords.insert(id, password);
    m_loadedProfilePasswords.insert(id);
    m_loadingProfilePasswords.remove(id);
    queueProfilePasswordStore(id, password, false);

    emit profilePasswordChanged(id);
    if (m_pendingPasswordConnects.remove(id)) {
        QTimer::singleShot(0, this, [this, id]() { connectProfile(id); });
    }
    if (!changed || state(id) == KiwiSdrClient::State::Disconnected) {
        return;
    }

    cancelReconnect(id);
    const QString endpoint = profile(id).endpoint;
    invokeClient(id, [endpoint, password](KiwiSdrClient* client) {
        client->disconnectFromEndpoint();
        if (!endpoint.isEmpty()) {
            client->connectToEndpoint(endpoint, password);
        }
    });
}

void KiwiSdrManager::removeProfile(const QString& id)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    qCInfo(lcKiwiSdr).noquote() << "Profile removed" << displayName(id) << "id=" << id;
    cancelReconnect(id);
    disconnectProfile(id);
    destroyClient(id);
    if (QTimer* timer = m_reconnectTimers.take(id)) {
        timer->deleteLater();
    }

    const QList<int> assignedSlices = m_sliceAssignments.keys(id);
    for (int sliceId : assignedSlices) {
        m_sliceAssignments.remove(sliceId);
        emit audioSourceEnabledChanged(id, false);
        emit sliceAssignmentChanged(sliceId, QString());
    }

    m_stateDetails.remove(id);
    m_states.remove(id);
    m_clientHasTrackedSlice.remove(id);
    m_telemetry.remove(id);
    m_waterfallAvailable.remove(id);
    m_waterfallDetails.remove(id);
    m_waterfallDisplayRanges.remove(id);
    deleteProfilePassword(id);
    m_profiles.removeAt(idx);
    saveSettings();
    // The client deletion is already scheduled above, so no further audio will
    // be fed for this id; free its audio-engine source state (disable alone
    // leaves the per-source entry allocated — #3668 review).
    emit audioSourceRemoved(id);
    emit profilesChanged();
}

void KiwiSdrManager::connectProfile(const QString& id)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    if (!isProfilePasswordLoaded(id)) {
        m_pendingPasswordConnects.insert(id);
        loadProfilePassword(id);
        return;
    }

    cancelReconnect(id);
    KiwiSdrClient* c = ensureClient(id);
    if (!c || m_profiles[idx].endpoint.isEmpty()) {
        return;
    }
    const KiwiSdrClient::State currentState = state(id);
    const bool waitingReconnect = reconnectRecommended(id);
    if (currentState == KiwiSdrClient::State::Connecting
        || (currentState == KiwiSdrClient::State::Waiting && !waitingReconnect)
        || KiwiSdrClient::stateHasReceiveAudio(currentState)) {
        return;
    }
    invokeClient(id, [callsign = m_operatorCallsign,
                      minDbm = m_profiles[idx].waterfallMinDbm,
                      maxDbm = m_profiles[idx].waterfallMaxDbm,
                      autoScale = m_profiles[idx].waterfallAutoScale,
                      rate = m_profiles[idx].waterfallRate](
                         KiwiSdrClient* client) {
        client->setOperatorCallsign(callsign);
        client->setWaterfallDisplayRange(minDbm, maxDbm, autoScale);
        client->setWaterfallRateOverride(rate);
    });
    if (assignedSliceForProfile(id) < 0) {
        m_clientHasTrackedSlice.insert(id, false);
        invokeClient(id, [](KiwiSdrClient* client) {
            client->setTrackedSlice(-1, 0.0, QString(), 0, 0, QString(),
                                    QString(), 0);
        });
        emit profileNeedsInitialTracking(id);
    }
    if (!m_clientHasTrackedSlice.value(id, false)) {
        return;
    }
    qCInfo(lcKiwiSdr).noquote()
        << "Connecting" << m_profiles[idx].name
        << "->" << m_profiles[idx].endpoint;
    m_states.insert(id, KiwiSdrClient::State::Connecting);
    invokeClient(id, [endpoint = m_profiles[idx].endpoint,
                      password = profilePassword(id)](KiwiSdrClient* client) {
        client->connectToEndpoint(endpoint, password);
    });
}

void KiwiSdrManager::disconnectProfile(const QString& id)
{
    cancelReconnect(id);
    if (KiwiSdrClient* c = client(id)) {
        Q_UNUSED(c);
        qCInfo(lcKiwiSdr).noquote() << "Disconnecting" << displayName(id);
        invokeClient(id, [](KiwiSdrClient* client) {
            client->disconnectFromEndpoint();
        });
    }
    emit audioSourceEnabledChanged(id, false);
    // Releasing a receiver ends its waterfall stream, so drop the per-profile
    // waterfall history the GUI cached for fast switch-back. Without this the
    // SpectrumWidget keeps a full waterfall + history QImage for every distinct
    // Kiwi ever switched to (m_kiwiProfileWaterfallStates), which grows the
    // working set by ~100-200 MB per receiver and is only freed on a full reset
    // — the leak reported in #4199. Profiles that stay assigned/auto-connected
    // are never disconnected here, so their cached history (multi-slice toggle)
    // is preserved. The stream, and its history, rebuild on reconnection.
    emit profileStreamReset(id);
}

void KiwiSdrManager::disconnectAll()
{
    for (QTimer* timer : std::as_const(m_reconnectTimers)) {
        if (timer) {
            timer->stop();
        }
    }
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        invokeClient(it.key(), [](KiwiSdrClient* client) {
            client->disconnectFromEndpoint();
        });
    }
}

void KiwiSdrManager::setOperatorCallsign(const QString& callsign)
{
    m_operatorCallsign = callsign;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        invokeClient(it.key(), [callsign](KiwiSdrClient* client) {
            client->setOperatorCallsign(callsign);
        });
    }
}

void KiwiSdrManager::startAutoConnect()
{
    for (const KiwiSdrAntennaProfile& p : std::as_const(m_profiles)) {
        if (p.autoConnect && !p.endpoint.isEmpty()) {
            connectProfile(p.id);
        }
    }
}

void KiwiSdrManager::primeProfileTracking(const QString& id, int sliceId,
                                          double frequencyMhz,
                                          const QString& mode,
                                          int filterLowHz,
                                          int filterHighHz,
                                          const QString& panId,
                                          double centerMhz,
                                          double bandwidthMhz,
                                          int lineDurationMs,
                                          const QString& bandName,
                                          int cwPitchHz)
{
    if (!hasProfile(id) || sliceId < 0 || frequencyMhz <= 0.0) {
        return;
    }

    if (KiwiSdrClient* c = ensureClient(id)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(id, true);
        invokeClient(id, [sliceId, frequencyMhz, mode, filterLowHz,
                          filterHighHz, panId, lineDurationMs,
                          centerMhz, bandwidthMhz, bandName, cwPitchHz](
                             KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId, bandName, cwPitchHz);
            client->setDisplayWaterfallRate(lineDurationMs);
            if (!panId.isEmpty() && centerMhz > 0.0 && bandwidthMhz > 0.0) {
                client->setWaterfallView(panId, centerMhz, bandwidthMhz);
            }
        });
    }
}

void KiwiSdrManager::assignSliceToProfile(int sliceId, const QString& profileId,
                                          double frequencyMhz,
                                          const QString& mode,
                                          int filterLowHz, int filterHighHz,
                                          const QString& panId,
                                          const QString& bandName,
                                          int cwPitchHz)
{
    if (sliceId < 0 || !hasProfile(profileId)) {
        clearSliceAssignment(sliceId);
        return;
    }

    const QString previousProfile = m_sliceAssignments.value(sliceId);
    if (!previousProfile.isEmpty() && previousProfile != profileId) {
        emit audioSourceEnabledChanged(previousProfile, false);
        if (KiwiSdrClient* previousClient = client(previousProfile)) {
            Q_UNUSED(previousClient);
            invokeClient(previousProfile, [](KiwiSdrClient* client) {
                client->setAudioActive(false);
            });
        }
    }

    const QList<int> otherSlices = m_sliceAssignments.keys(profileId);
    for (int otherSliceId : otherSlices) {
        if (otherSliceId == sliceId) {
            continue;
        }
        m_sliceAssignments.remove(otherSliceId);
        emit sliceAssignmentChanged(otherSliceId, QString());
    }

    m_sliceAssignments.insert(sliceId, profileId);
    qCInfo(lcKiwiSdr).noquote()
        << "Slice" << sliceId << "assigned to" << displayName(profileId)
        << "freq=" << frequencyMhz << "MHz mode=" << mode;
    emit sliceAssignmentChanged(sliceId, profileId);

    // Switching this slice to a different Kiwi: release the previous one if no
    // other slice still uses it and it isn't auto-connect, so we don't squat the
    // receiver's user slot / burn its per-IP time budget (#3950). Must come after
    // the insert() above so shouldMaintainProfileConnection() sees the new map.
    if (!previousProfile.isEmpty() && previousProfile != profileId
        && !shouldMaintainProfileConnection(previousProfile)) {
        disconnectProfile(previousProfile);
    }

    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(profileId, sliceId >= 0 && frequencyMhz > 0.0);
        const bool connected =
            KiwiSdrClient::stateHasReceiveAudio(state(profileId));
        invokeClient(profileId, [sliceId, frequencyMhz, mode, filterLowHz,
                                 filterHighHz, panId, bandName, connected,
                                 cwPitchHz](
                                    KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId, bandName, cwPitchHz);
            client->setAudioActive(connected);
        });
    }
    connectProfile(profileId);
    emit audioSourceEnabledChanged(profileId, true);
}

void KiwiSdrManager::clearSliceAssignment(int sliceId)
{
    const QString previousProfile = m_sliceAssignments.take(sliceId);
    if (previousProfile.isEmpty()) {
        return;
    }

    qCInfo(lcKiwiSdr).noquote()
        << "Slice" << sliceId << "cleared from" << displayName(previousProfile);
    emit audioSourceEnabledChanged(previousProfile, false);
    if (KiwiSdrClient* c = client(previousProfile)) {
        Q_UNUSED(c);
        invokeClient(previousProfile, [](KiwiSdrClient* client) {
            client->setAudioActive(false);
        });
    }
    emit sliceAssignmentChanged(sliceId, QString());

    // The slice no longer uses this Kiwi (antenna reverted to Flex, or the slice
    // was closed). Release it once nothing else needs it, so it stops holding the
    // receiver's user slot / per-IP time budget (#3950). take() above already
    // updated the map, so shouldMaintainProfileConnection() reflects reality here.
    if (!shouldMaintainProfileConnection(previousProfile)) {
        disconnectProfile(previousProfile);
    }
}

void KiwiSdrManager::updateSliceTracking(int sliceId, double frequencyMhz,
                                         const QString& mode,
                                         int filterLowHz, int filterHighHz,
                                         const QString& panId,
                                         const QString& bandName,
                                         int cwPitchHz)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }
    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(profileId, sliceId >= 0 && frequencyMhz > 0.0);
        invokeClient(profileId, [sliceId, frequencyMhz, mode,
                                 filterLowHz, filterHighHz, panId,
                                 bandName, cwPitchHz](
                                    KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId, bandName, cwPitchHz);
        });
    }
}

void KiwiSdrManager::updateWaterfallView(int sliceId, const QString& panId,
                                         double centerMhz, double bandwidthMhz,
                                         int lineDurationMs)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }
    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        invokeClient(profileId, [panId, centerMhz, bandwidthMhz,
                                 lineDurationMs](KiwiSdrClient* client) {
            client->setDisplayWaterfallRate(lineDurationMs);
            client->setWaterfallView(panId, centerMhz, bandwidthMhz);
        });
    }
}

void KiwiSdrManager::setReceiverControlsForSlice(
    int sliceId, const KiwiSdrReceiverControls& controls)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }

    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        invokeClient(profileId, [controls](KiwiSdrClient* client) {
            client->setReceiverControls(controls);
        });
    }
}

void KiwiSdrManager::setProfileWaterfallDisplayRange(const QString& id,
                                                     int minDbm,
                                                     int maxDbm,
                                                     bool autoScale,
                                                     int rate)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    KiwiSdrAntennaProfile p = m_profiles[idx];
    p.waterfallMinDbm = std::clamp(minDbm,
                                   kKiwiSdrWaterfallMinDbmLimit,
                                   kKiwiSdrWaterfallMaxDbmLimit - 1);
    p.waterfallMaxDbm = std::clamp(maxDbm,
                                   p.waterfallMinDbm + 1,
                                   kKiwiSdrWaterfallMaxDbmLimit);
    p.waterfallAutoScale = autoScale;
    p.waterfallRate = std::clamp(rate, 0, kKiwiSdrWaterfallRateMax);
    updateProfile(p);
    if (!p.waterfallAutoScale) {
        KiwiSdrWaterfallDisplayRange range;
        range.minDbm = static_cast<float>(p.waterfallMinDbm);
        range.maxDbm = static_cast<float>(p.waterfallMaxDbm);
        range.autoRange = false;
        range.valid = true;
        m_waterfallDisplayRanges.insert(id, range);
        emit waterfallDisplayRangeChanged(id, range.minDbm, range.maxDbm,
                                          range.autoRange);
    }
}

void KiwiSdrManager::requestProfileWaterfallAutoScale(const QString& id)
{
    invokeClient(id, [](KiwiSdrClient* client) {
        client->requestWaterfallAutoScale();
    });
}

KiwiSdrClient* KiwiSdrManager::ensureClient(const QString& id)
{
    if (KiwiSdrClient* existing = client(id)) {
        return existing;
    }

    if (!hasProfile(id)) {
        return nullptr;
    }

    ensureClientThread();
    auto* c = new KiwiSdrClient;
    c->setDecodeAudioWhenInactive(false);
    c->setOperatorCallsign(m_operatorCallsign);
    c->moveToThread(m_clientThread);
    connect(m_clientThread, &QThread::finished, c, &QObject::deleteLater);
    m_clients.insert(id, c);
    m_states.insert(id, KiwiSdrClient::State::Disconnected);
    m_clientHasTrackedSlice.insert(id, false);
    connect(c, &KiwiSdrClient::stateChanged,
            this, [this, id, c](KiwiSdrClient::State state, const QString& detail) {
        if (client(id) != c) {
            return;
        }
        m_states.insert(id, state);
        m_stateDetails.insert(id, detail);
        qCInfo(lcKiwiSdr).noquote()
            << "State" << displayName(id) << "->" << static_cast<int>(state)
            << (detail.isEmpty() ? QString() : QStringLiteral("(") + detail + QStringLiteral(")"));
        if (state == KiwiSdrClient::State::Connecting) {
            m_telemetry.insert(id, {});
            m_waterfallAvailable.insert(id, true);
            m_waterfallDetails.remove(id);
            emit profileTelemetryChanged(id, m_telemetry.value(id));
            emit profileWaterfallAvailabilityChanged(id, true, QString());
        }
        if (KiwiSdrClient::stateHasReceiveAudio(state)) {
            const int idx = profileIndex(id);
            const bool hasAssignedSlice = assignedSliceForProfile(id) >= 0;
            if (idx >= 0 || hasAssignedSlice) {
                const int minDbm = idx >= 0
                    ? m_profiles[idx].waterfallMinDbm
                    : kKiwiSdrDefaultWaterfallMinDbm;
                const int maxDbm = idx >= 0
                    ? m_profiles[idx].waterfallMaxDbm
                    : kKiwiSdrDefaultWaterfallMaxDbm;
                const bool autoScale = idx < 0 || m_profiles[idx].waterfallAutoScale;
                const int rate = idx >= 0 ? m_profiles[idx].waterfallRate : 0;
                const bool normalReceiver =
                    KiwiSdrClient::stateAllowsReceiverControl(state);
                invokeClient(id, [idx, minDbm, maxDbm, autoScale, rate, hasAssignedSlice,
                                  normalReceiver](
                                     KiwiSdrClient* client) {
                    if (idx >= 0 && normalReceiver) {
                        client->setWaterfallDisplayRange(minDbm, maxDbm,
                                                         autoScale);
                        client->setWaterfallRateOverride(rate);
                    }
                    if (hasAssignedSlice) {
                        client->setAudioActive(true);
                    }
                });
            }
            if (hasAssignedSlice) {
                emit audioSourceEnabledChanged(id, true);
            }
        } else if (state != KiwiSdrClient::State::Connecting) {
            emit audioSourceEnabledChanged(id, false);
            emit meterReadingReady(
                id,
                KiwiSdrProtocol::meterUnavailable(
                    KiwiSdrProtocol::MeterSource::Unknown,
                    detail));
        }
        emit profileStateChanged(id, state, detail);
        scheduleWaitingReconnectIfRecommended(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::recoverableDisconnect,
            this, [this, id, c](const QString&) {
        if (client(id) != c) {
            return;
        }
        scheduleReconnect(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::telemetryChanged, this,
            [this, id, c](const KiwiSdrReceiverTelemetry& telemetry) {
        if (client(id) != c) {
            return;
        }
        m_telemetry.insert(id, telemetry);
        emit profileTelemetryChanged(id, telemetry);
        scheduleWaitingReconnectIfRecommended(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::waterfallAvailabilityChanged,
            this, [this, id, c](bool available, const QString& detail) {
        if (client(id) != c) {
            return;
        }
        m_waterfallAvailable.insert(id, available);
        if (detail.isEmpty()) {
            m_waterfallDetails.remove(id);
        } else {
            m_waterfallDetails.insert(id, detail);
        }
        emit profileWaterfallAvailabilityChanged(id, available, detail);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::decodedAudioReady,
            this, [this, id, c](const QByteArray& pcm) {
        if (client(id) != c) {
            return;
        }
        emit decodedAudioReady(id, pcm);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::waterfallRowReady,
            this, [this, id, c](const QString& panId, const QVector<float>& binsDbm,
                             double lowFreqMhz, double highFreqMhz,
                             quint32 timecode) {
        if (client(id) != c) {
            return;
        }
        emit waterfallRowReady(id, panId, binsDbm, lowFreqMhz, highFreqMhz,
                               timecode);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::waterfallDisplayRangeChanged,
            this, [this, id, c](float minDbm, float maxDbm, bool autoRange) {
        if (client(id) != c) {
            return;
        }
        KiwiSdrWaterfallDisplayRange range;
        range.minDbm = minDbm;
        range.maxDbm = maxDbm;
        range.autoRange = autoRange;
        range.valid = true;
        m_waterfallDisplayRanges.insert(id, range);
        emit waterfallDisplayRangeChanged(id, minDbm, maxDbm, autoRange);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::meterReadingReady,
            this, [this, id, c](const KiwiSdrProtocol::MeterReading& reading) {
        if (client(id) != c) {
            return;
        }
        emit meterReadingReady(id, reading);
    }, Qt::QueuedConnection);
    return c;
}

KiwiSdrClient* KiwiSdrManager::client(const QString& id) const
{
    return m_clients.value(id, nullptr);
}

void KiwiSdrManager::ensureClientThread()
{
    if (m_clientThread) {
        return;
    }

    m_clientThread = new QThread(this);
    m_clientThread->setObjectName(QStringLiteral("KiwiSdrClients"));
    m_clientThread->start();
}

void KiwiSdrManager::invokeClient(
    const QString& id,
    std::function<void(KiwiSdrClient*)> fn)
{
    KiwiSdrClient* c = client(id);
    if (!c) {
        return;
    }

    QMetaObject::invokeMethod(c, [c, fn = std::move(fn)]() {
        fn(c);
    }, Qt::QueuedConnection);
}

void KiwiSdrManager::destroyClient(const QString& id, bool blocking)
{
    KiwiSdrClient* c = m_clients.take(id);
    if (!c) {
        return;
    }

    c->disconnect(this);
    m_states.remove(id);
    m_stateDetails.remove(id);
    m_clientHasTrackedSlice.remove(id);
    m_telemetry.remove(id);
    m_waterfallAvailable.remove(id);
    m_waterfallDetails.remove(id);
    m_waterfallDisplayRanges.remove(id);
    const Qt::ConnectionType connectionType =
        blocking && c->thread() != QThread::currentThread()
            ? Qt::BlockingQueuedConnection
            : Qt::QueuedConnection;
    QMetaObject::invokeMethod(c, [c]() {
        c->disconnectFromEndpoint();
        c->deleteLater();
    }, connectionType);
}

int KiwiSdrManager::profileIndex(const QString& id) const
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            return i;
        }
    }
    return -1;
}

void KiwiSdrManager::loadSettings()
{
    m_profiles.clear();
    const QString raw = AppSettings::instance()
        .value(kKiwiSdrRxAntennasSettingsKey, "{}").toString();
    const QJsonObject root = QJsonDocument::fromJson(raw.toUtf8()).object();
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& value : profiles) {
        const QJsonObject obj = value.toObject();
        KiwiSdrAntennaProfile p;
        p.id = obj.value(QStringLiteral("id")).toString();
        if (p.id.isEmpty()) {
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        p.endpoint = normalizedProfileEndpoint(
            obj.value(QStringLiteral("endpoint")).toString());
        p.name = sanitizedName(obj.value(QStringLiteral("name")).toString(),
                               p.endpoint);
        p.autoConnect = obj.value(QStringLiteral("autoConnect")).toBool(false);
        p.keepAudioDuringTx =
            obj.value(QStringLiteral("keepAudioDuringTx")).toBool(false);
        p.resumeAudioAfterTxDelay =
            obj.value(QStringLiteral("resumeAudioAfterTxDelay")).toBool(false);
        if (obj.contains(QStringLiteral("waterfallMinDbm"))
            || obj.contains(QStringLiteral("waterfallMaxDbm"))) {
            p.waterfallMinDbm = std::clamp(
                obj.value(QStringLiteral("waterfallMinDbm"))
                    .toInt(kKiwiSdrDefaultWaterfallMinDbm),
                kKiwiSdrWaterfallMinDbmLimit,
                kKiwiSdrWaterfallMaxDbmLimit - 1);
            p.waterfallMaxDbm = std::clamp(
                obj.value(QStringLiteral("waterfallMaxDbm"))
                    .toInt(kKiwiSdrDefaultWaterfallMaxDbm),
                p.waterfallMinDbm + 1,
                kKiwiSdrWaterfallMaxDbmLimit);
            p.waterfallAutoScale =
                obj.value(QStringLiteral("waterfallAutoScale")).toBool(true);
        } else {
            // Legacy profiles stored ±dB offsets against the previous
            // continuously-running auto range. Those offsets do not map cleanly
            // onto absolute WF Floor/Ceiling, so migrate them back to Auto.
            p.waterfallMinDbm = kKiwiSdrDefaultWaterfallMinDbm;
            p.waterfallMaxDbm = kKiwiSdrDefaultWaterfallMaxDbm;
            p.waterfallAutoScale = true;
        }
        p.waterfallRate = std::clamp(
            obj.value(QStringLiteral("waterfallRate")).toInt(0),
            0,
            kKiwiSdrWaterfallRateMax);
        m_profiles.append(p);
    }
}

void KiwiSdrManager::saveSettings() const
{
    QJsonArray profiles;
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), p.id);
        obj.insert(QStringLiteral("name"), p.name);
        obj.insert(QStringLiteral("endpoint"), normalizedProfileEndpoint(p.endpoint));
        obj.insert(QStringLiteral("autoConnect"), p.autoConnect);
        obj.insert(QStringLiteral("keepAudioDuringTx"), p.keepAudioDuringTx);
        obj.insert(QStringLiteral("resumeAudioAfterTxDelay"),
                   p.resumeAudioAfterTxDelay);
        obj.insert(QStringLiteral("waterfallAutoScale"), p.waterfallAutoScale);
        const int waterfallMinDbm = std::clamp(
            p.waterfallMinDbm,
            kKiwiSdrWaterfallMinDbmLimit,
            kKiwiSdrWaterfallMaxDbmLimit - 1);
        const int waterfallMaxDbm = std::clamp(
            p.waterfallMaxDbm,
            waterfallMinDbm + 1,
            kKiwiSdrWaterfallMaxDbmLimit);
        obj.insert(QStringLiteral("waterfallMinDbm"), waterfallMinDbm);
        obj.insert(QStringLiteral("waterfallMaxDbm"), waterfallMaxDbm);
        obj.insert(QStringLiteral("waterfallRate"),
                   std::clamp(p.waterfallRate, 0, kKiwiSdrWaterfallRateMax));
        profiles.append(obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("profiles"), profiles);
    auto& settings = AppSettings::instance();
    settings.setValue(kKiwiSdrRxAntennasSettingsKey, QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.save();
}

QString KiwiSdrManager::profilePasswordKey(const QString& id)
{
    return QStringLiteral("kiwisdr_password_%1").arg(id);
}

void KiwiSdrManager::loadProfilePassword(const QString& id)
{
    if (!hasProfile(id) || m_loadedProfilePasswords.contains(id)
        || m_loadingProfilePasswords.contains(id)) {
        return;
    }
    m_loadingProfilePasswords.insert(id);
    setProfilePasswordPersistence(
        id, KiwiSdrPasswordPersistenceState::Loading);
    const quint64 revision = m_profilePasswordRevisions.value(id);

    m_credentialStore->read(
        profilePasswordKey(id), this,
        [this, id, revision](const KiwiSdrCredentialResult& result) {
        m_loadingProfilePasswords.remove(id);
        if (!hasProfile(id)) {
            return;
        }
        if (m_profilePasswordRevisions.value(id) != revision) {
            return;
        }
        if (result.code == KiwiSdrCredentialResultCode::Success) {
            m_profilePasswords.insert(id, result.value);
            setProfilePasswordPersistence(
                id, result.value.isEmpty()
                        ? KiwiSdrPasswordPersistenceState::NoPassword
                        : (m_credentialStore->isPersistent()
                               ? KiwiSdrPasswordPersistenceState::Stored
                               : KiwiSdrPasswordPersistenceState::SessionOnly));
        } else if (result.code == KiwiSdrCredentialResultCode::NotFound) {
            m_profilePasswords.remove(id);
            setProfilePasswordPersistence(
                id, KiwiSdrPasswordPersistenceState::NoPassword);
        } else {
            qCWarning(lcKiwiSdr).noquote()
                << "KiwiSDR password keychain read failed"
                << "id=" << id << result.error;
            setProfilePasswordPersistence(
                id, KiwiSdrPasswordPersistenceState::Error,
                tr("Could not read the saved password: %1")
                    .arg(result.error));
        }
        m_loadedProfilePasswords.insert(id);
        emit profilePasswordChanged(id);
        if (result.code != KiwiSdrCredentialResultCode::Error
            && m_pendingPasswordConnects.remove(id)) {
            connectProfile(id);
        } else if (result.code == KiwiSdrCredentialResultCode::Error) {
            m_pendingPasswordConnects.remove(id);
        }
    });
}

void KiwiSdrManager::deleteProfilePassword(const QString& id)
{
    m_profilePasswordRevisions.insert(
        id, m_profilePasswordRevisions.value(id) + 1);
    m_profilePasswords.remove(id);
    m_loadedProfilePasswords.remove(id);
    m_loadingProfilePasswords.remove(id);
    m_pendingPasswordConnects.remove(id);
    queueProfilePasswordStore(id, {}, true);
}

void KiwiSdrManager::queueProfilePasswordStore(
    const QString& id, const QString& password, bool profileRemoval)
{
    const PendingPasswordStore pending{
        m_profilePasswordRevisions.value(id), password, profileRemoval};
    if (!profileRemoval) {
        setProfilePasswordPersistence(
            id, m_credentialStore->isPersistent()
                    ? KiwiSdrPasswordPersistenceState::Saving
                    : (password.isEmpty()
                           ? KiwiSdrPasswordPersistenceState::NoPassword
                           : KiwiSdrPasswordPersistenceState::SessionOnly));
    }
    const auto finished =
        [this, id, pending](const KiwiSdrCredentialResult& result) {
        const bool latest = m_profilePasswordRevisions.value(id)
            == pending.revision;
        if (latest) {
            if (result.code == KiwiSdrCredentialResultCode::Success
                || (pending.password.isEmpty()
                    && result.code == KiwiSdrCredentialResultCode::NotFound)) {
                if (!pending.profileRemoval) {
                    setProfilePasswordPersistence(
                        id, pending.password.isEmpty()
                                ? KiwiSdrPasswordPersistenceState::NoPassword
                                : (m_credentialStore->isPersistent()
                                       ? KiwiSdrPasswordPersistenceState::Stored
                                       : KiwiSdrPasswordPersistenceState::SessionOnly));
                }
            } else {
                const QString action = pending.password.isEmpty()
                    ? tr("delete") : tr("save");
                const QString detail =
                    tr("Could not %1 the KiwiSDR password: %2")
                        .arg(action, result.error);
                qCWarning(lcKiwiSdr).noquote()
                    << "KiwiSDR password credential-store update failed"
                    << "id=" << id << detail;
                setProfilePasswordPersistence(
                    id, KiwiSdrPasswordPersistenceState::Error, detail);
            }
        }

        if (pending.profileRemoval && latest) {
            m_profilePasswordRevisions.remove(id);
            m_profilePasswordPersistenceStates.remove(id);
            m_profilePasswordPersistenceDetails.remove(id);
        }
    };

    if (pending.password.isEmpty()) {
        m_credentialStore->remove(profilePasswordKey(id), this, finished);
    } else {
        m_credentialStore->write(profilePasswordKey(id), pending.password,
                                 this, finished);
    }
}

void KiwiSdrManager::setProfilePasswordPersistence(
    const QString& id, KiwiSdrPasswordPersistenceState state,
    const QString& detail)
{
    if (m_profilePasswordPersistenceStates.value(id) == state
        && m_profilePasswordPersistenceDetails.value(id) == detail) {
        return;
    }
    m_profilePasswordPersistenceStates.insert(id, state);
    if (detail.isEmpty()) {
        m_profilePasswordPersistenceDetails.remove(id);
    } else {
        m_profilePasswordPersistenceDetails.insert(id, detail);
    }
    emit profilePasswordPersistenceChanged(id, state, detail);
}

bool KiwiSdrManager::shouldMaintainProfileConnection(const QString& id) const
{
    const int idx = profileIndex(id);
    if (idx < 0 || m_profiles[idx].endpoint.trimmed().isEmpty()) {
        return false;
    }

    return m_profiles[idx].autoConnect || assignedSliceForProfile(id) >= 0;
}

void KiwiSdrManager::scheduleReconnect(const QString& id)
{
    if (!shouldMaintainProfileConnection(id) && !reconnectRecommended(id)) {
        return;
    }

    QTimer* timer = m_reconnectTimers.value(id, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(kRecoverableReconnectDelayMs);
        m_reconnectTimers.insert(id, timer);
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (shouldMaintainProfileConnection(id)
                || reconnectRecommended(id)) {
                connectProfile(id);
            }
        });
    }

    if (!timer->isActive()) {
        qCInfo(lcKiwiSdr).noquote()
            << "Reconnect scheduled for" << displayName(id)
            << "in" << kRecoverableReconnectDelayMs << "ms";
        timer->start();
    }
}

void KiwiSdrManager::scheduleWaitingReconnectIfRecommended(const QString& id)
{
    if (!reconnectRecommended(id)) {
        return;
    }

    scheduleReconnect(id);
}

void KiwiSdrManager::cancelReconnect(const QString& id)
{
    if (QTimer* timer = m_reconnectTimers.value(id, nullptr)) {
        timer->stop();
    }
}

QString KiwiSdrManager::sanitizedName(const QString& name,
                                      const QString& endpoint)
{
    const QString trimmed = name.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed.left(kKiwiSdrProfileNameMaxChars);
    }
    if (!endpoint.trimmed().isEmpty()) {
        return endpoint.trimmed().left(kKiwiSdrProfileNameMaxChars);
    }
    return QStringLiteral("KiwiSDR");
}

} // namespace AetherSDR
