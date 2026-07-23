#include "ShortcutManager.h"
#include "AppSettings.h"
#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringConverter>
#include <QWidget>
#include <QDebug>

namespace AetherSDR {

namespace {

constexpr qsizetype kMaxShortcutCsvBytes = 1024 * 1024;
constexpr int kMaxShortcutCsvRows = 4096;
constexpr int kMaxShortcutCsvFieldLength = 512;
constexpr int kShortcutCsvSchemaVersion = 1;

const QStringList kShortcutCsvHeader{
    QStringLiteral("FORMAT_VERSION"),
    QStringLiteral("ACTION_ID"),
    QStringLiteral("ACTION_NAME"),
    QStringLiteral("CATEGORY"),
    QStringLiteral("SHORTCUT"),
    QStringLiteral("CUSTOMIZED"),
};

struct ShortcutCsvRow {
    int lineNumber{0};
    QStringList fields;
};

struct ParsedShortcutRow {
    int lineNumber{0};
    QString actionId;
    QString actionName;
    QKeySequence shortcut;
    bool customized{false};
};

QString csvEscape(QString value)
{
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"'))
        || value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + value + QLatin1Char('"');
    }
    return value;
}

bool appendCsvField(ShortcutCsvRow& row, QString& field, QStringList& errors)
{
    if (field.size() > kMaxShortcutCsvFieldLength) {
        errors << QStringLiteral("Line %1: a field exceeds %2 characters.")
                      .arg(row.lineNumber)
                      .arg(kMaxShortcutCsvFieldLength);
        return false;
    }
    row.fields << field;
    field.clear();
    return true;
}

QList<ShortcutCsvRow> parseCsvRows(const QByteArray& bytes, QStringList& errors)
{
    QList<ShortcutCsvRow> rows;
    if (bytes.size() > kMaxShortcutCsvBytes) {
        errors << QStringLiteral("Shortcut CSV exceeds the 1 MiB size limit.");
        return rows;
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder.decode(bytes);
    if (decoder.hasError()) {
        errors << QStringLiteral("Shortcut CSV is not valid UTF-8.");
        return rows;
    }
    if (!text.isEmpty() && text.front() == QChar(0xfeff)) {
        text.remove(0, 1);
    }

    ShortcutCsvRow row;
    row.lineNumber = 1;
    QString field;
    bool inQuotes = false;
    bool afterQuote = false;
    int lineNumber = 1;
    // Line the currently-open quote started on, so an unterminated quoted field
    // reports where the runaway '"' was opened rather than where its row began.
    int quoteOpenedLine = 0;

    auto finishRow = [&]() -> bool {
        if (!appendCsvField(row, field, errors)) {
            return false;
        }
        const bool blank = row.fields.size() == 1 && row.fields.constFirst().isEmpty();
        if (!blank) {
            rows << row;
            if (rows.size() > kMaxShortcutCsvRows + 1) {
                errors << QStringLiteral("Shortcut CSV exceeds the %1-row limit.")
                              .arg(kMaxShortcutCsvRows);
                return false;
            }
        }
        row = ShortcutCsvRow{};
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
            if (!appendCsvField(row, field, errors)) {
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

QList<ParsedShortcutRow> parseShortcutCsv(const QByteArray& bytes, QStringList& errors)
{
    const QList<ShortcutCsvRow> rows = parseCsvRows(bytes, errors);
    if (!errors.isEmpty()) {
        return {};
    }
    if (rows.isEmpty()) {
        errors << QStringLiteral("Shortcut CSV is empty.");
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
    for (const QString& required : kShortcutCsvHeader) {
        if (!columns.contains(required)) {
            errors << QStringLiteral("Line 1: missing required column %1.").arg(required);
        }
    }
    if (!errors.isEmpty()) {
        return {};
    }

    QList<ParsedShortcutRow> parsed;
    QHash<QString, int> actionLines;
    QHash<QString, QString> customizedKeyOwners;
    const QRegularExpression actionIdPattern(QStringLiteral("^[A-Za-z0-9_]+$"));
    for (int i = 1; i < rows.size(); ++i) {
        const ShortcutCsvRow& row = rows.at(i);
        const auto value = [&row, &columns](const QString& name) {
            const int index = columns.value(name, -1);
            return index >= 0 && index < row.fields.size() ? row.fields.at(index).trimmed()
                                                           : QString();
        };

        bool schemaOk = false;
        const int schemaVersion = value(QStringLiteral("FORMAT_VERSION")).toInt(&schemaOk);
        const QString actionId = value(QStringLiteral("ACTION_ID"));
        const QString actionName = value(QStringLiteral("ACTION_NAME"));
        const QString shortcutText = value(QStringLiteral("SHORTCUT"));
        const QString customizedText = value(QStringLiteral("CUSTOMIZED")).toLower();
        const bool customized = customizedText == QLatin1String("true")
            || customizedText == QLatin1String("1")
            || customizedText == QLatin1String("yes");
        const bool customizedValid = customized || customizedText == QLatin1String("false")
            || customizedText == QLatin1String("0")
            || customizedText == QLatin1String("no");

        // Independent checks so a row with multiple bad fields surfaces all of
        // them in one pass instead of forcing the user to round-trip the file.
        const bool actionIdValid = actionIdPattern.match(actionId).hasMatch();
        if (!schemaOk || schemaVersion < 1) {
            errors << QStringLiteral("Line %1: FORMAT_VERSION must be a positive integer.")
                          .arg(row.lineNumber);
        }
        if (!actionIdValid) {
            errors << QStringLiteral("Line %1: invalid ACTION_ID '%2'.")
                          .arg(row.lineNumber)
                          .arg(actionId);
        }
        if (actionName.isEmpty()) {
            errors << QStringLiteral("Line %1: ACTION_NAME is required.").arg(row.lineNumber);
        }
        if (!customizedValid) {
            errors << QStringLiteral("Line %1: CUSTOMIZED must be True or False.")
                          .arg(row.lineNumber);
        }
        // Only check for duplicates once the id itself is well-formed; else two
        // rows with empty (misaligned) ids emit a spurious 'duplicate ""' on top
        // of the real 'invalid ACTION_ID' errors.
        if (actionIdValid) {
            if (actionLines.contains(actionId)) {
                errors << QStringLiteral("Line %1: duplicate ACTION_ID '%2' (first on line %3).")
                              .arg(row.lineNumber)
                              .arg(actionId)
                              .arg(actionLines.value(actionId));
            } else {
                actionLines.insert(actionId, row.lineNumber);
            }
        }

        // QKeySequence::fromString(PortableText) returns a count==1 sequence
        // containing Qt::Key_unknown for an unrecognized key name — isEmpty()
        // stays false, so a plain isEmpty() guard misses garbage. Round-trip
        // through toString(PortableText): Key_unknown stringifies as empty,
        // so a non-empty input that round-trips to "" is invalid.
        const QKeySequence shortcut = QKeySequence::fromString(
            shortcutText, QKeySequence::PortableText);
        if (!shortcutText.isEmpty()
            && shortcut.toString(QKeySequence::PortableText).isEmpty()) {
            errors << QStringLiteral("Line %1: invalid portable shortcut '%2'.")
                          .arg(row.lineNumber)
                          .arg(shortcutText);
        }
        if (customized && !shortcut.isEmpty()) {
            const QString canonical = shortcut.toString(QKeySequence::PortableText);
            if (customizedKeyOwners.contains(canonical)) {
                errors << QStringLiteral("Line %1: shortcut '%2' is customized for both %3 and %4.")
                              .arg(row.lineNumber)
                              .arg(canonical, customizedKeyOwners.value(canonical), actionName);
            } else {
                customizedKeyOwners.insert(canonical, actionName);
            }
        }

        parsed << ParsedShortcutRow{row.lineNumber, actionId, actionName,
                                    shortcut, customized};
    }
    return errors.isEmpty() ? parsed : QList<ParsedShortcutRow>{};
}

} // namespace

ShortcutManager::ShortcutManager(QObject* parent)
    : QObject(parent)
{
}

void ShortcutManager::registerAction(const QString& id, const QString& displayName,
                                     const QString& category, const QKeySequence& defaultKey,
                                     std::function<void()> handler,
                                     bool autoRepeat, bool keysTx)
{
    // The id becomes part of the XML element name "Shortcut_<id>". AppSettings
    // silently drops keys that fail ^[A-Za-z_][A-Za-z0-9_]*$ on save (qWarning
    // only), so an id containing '.'/'-'/etc. would be contains()-true in-session
    // yet never written — a cleared binding would resurrect after restart (the
    // exact #3964 failure mode). Keep ids to [A-Za-z0-9_].
    Q_ASSERT_X(QRegularExpression(QStringLiteral("^[A-Za-z0-9_]+$")).match(id).hasMatch(),
               "ShortcutManager::registerAction",
               "action id must match [A-Za-z0-9_]+ (it forms the XML key Shortcut_<id>)");

    // Prevent duplicate registration
    for (const auto& a : m_actions) {
        if (a.id == id) {
            qWarning() << "ShortcutManager: duplicate action registration:" << id;
            return;
        }
    }
    m_actions.append({id, displayName, category, defaultKey, defaultKey,
                      std::move(handler), autoRepeat, /*persisted=*/false, keysTx});
}

void ShortcutManager::setBinding(const QString& actionId, const QKeySequence& key)
{
    auto* a = action(actionId);
    if (!a) return;

    // Clear any existing binding on this key (prevent conflicts)
    if (!key.isEmpty()) {
        for (auto& other : m_actions) {
            if (other.id != actionId && other.currentKey == key)
                other.currentKey = QKeySequence();
        }
    }

    a->currentKey = key;
    a->persisted = true;
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::clearBinding(const QString& actionId)
{
    auto* a = action(actionId);
    if (!a) return;
    a->currentKey = QKeySequence();
    a->persisted = true;   // user intent — an explicit "" must survive restart (#3964)
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::resetToDefaults()
{
    for (auto& a : m_actions) {
        a.currentKey = a.defaultKey;
        a.persisted = false;   // back to default → drop from settings, track future defaults
    }
    saveBindings();
    emit bindingsChanged();
}

void ShortcutManager::loadBindings()
{
    auto& s = AppSettings::instance();
    for (auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        // A present key (even "") is explicit user intent — apply it and mark the
        // action persisted; an absent key keeps the registered default. Presence,
        // not the null-vs-"" value (which the XML round-trip flattens), is the
        // correct sentinel (#3964). We need the presence bit itself to seed
        // `persisted`, so this stays a contains() gate rather than a
        // value(key, default) fallback.
        if (s.contains(key)) {
            a.currentKey = QKeySequence(s.value(key).toString());
            a.persisted = true;
        }
    }

    // Normalize duplicate key bindings for this session only. This clears the
    // losing action's currentKey in memory but leaves its `persisted` flag
    // untouched, so saveBindings() will NOT write the machine-initiated clear:
    // the loser is recomputed from (defaults + persisted customizations) on every
    // load. Persisting it would be indistinguishable from a user clear and would
    // pin the loser to "" forever — suppressing a future release's default once
    // the colliding registration is fixed (#3964 review).
    normalizeDuplicateBindings();
}

void ShortcutManager::normalizeDuplicateBindings()
{
    QHash<QString, int> ownerByKey;
    for (int i = 0; i < m_actions.size(); ++i) {
        auto& action = m_actions[i];
        if (action.currentKey.isEmpty()) continue;

        const QString keyText = action.currentKey.toString();
        auto it = ownerByKey.find(keyText);
        if (it == ownerByKey.end()) {
            ownerByKey.insert(keyText, i);
            continue;
        }

        auto& incumbent = m_actions[*it];
        // Prefer the persisted user intent over a coincidental value match:
        // a user who explicitly bound an action to its default (persisted=true,
        // currentKey==defaultKey) is still "customized" here, and the old
        // `currentKey != defaultKey` test would treat it as a default that any
        // other action could steal in a collision.
        const bool incumbentCustomized = incumbent.persisted && !incumbent.currentKey.isEmpty();
        const bool actionCustomized = action.persisted && !action.currentKey.isEmpty();

        if (actionCustomized && !incumbentCustomized) {
            qWarning() << "ShortcutManager: clearing duplicate default binding"
                       << incumbent.id << "for key" << keyText
                       << "in favor of customized binding" << action.id;
            incumbent.currentKey = QKeySequence();
            *it = i;
        } else {
            qWarning() << "ShortcutManager: clearing duplicate binding"
                       << action.id << "for key" << keyText
                       << "owned by" << incumbent.id;
            action.currentKey = QKeySequence();
        }
    }
}

void ShortcutManager::saveBindings()
{
    auto& s = AppSettings::instance();
    for (const auto& a : m_actions) {
        const QString key = QStringLiteral("Shortcut_%1").arg(a.id);
        // Persist only actions the user explicitly touched. A set or cleared
        // binding is marked persisted and written back — a cleared one round-trips
        // as "" and loadBindings()'s presence gate honors it (#3964). Everything
        // else (fresh at default, reset to default, or cleared only by the
        // load-time duplicate-key normalization) is written absent: a future
        // release's changed default reaches it instead of being pinned, and a
        // machine-initiated normalization clear never sticks.
        if (a.persisted) {
            s.setValue(key, a.currentKey.toString());
        } else {
            s.remove(key);
        }
    }
    s.save();
}

QByteArray ShortcutManager::exportBindingsCsv() const
{
    QStringList lines;
    lines.reserve(m_actions.size() + 1);
    lines << kShortcutCsvHeader.join(QLatin1Char(','));
    for (const Action& action : m_actions) {
        const QStringList fields{
            QString::number(kShortcutCsvSchemaVersion),
            action.id,
            action.displayName,
            action.category,
            action.currentKey.toString(QKeySequence::PortableText),
            action.persisted ? QStringLiteral("True") : QStringLiteral("False"),
        };
        QStringList escaped;
        escaped.reserve(fields.size());
        for (const QString& field : fields) {
            escaped << csvEscape(field);
        }
        lines << escaped.join(QLatin1Char(','));
    }
    return lines.join(QStringLiteral("\r\n")).toUtf8() + QByteArray("\r\n");
}

ShortcutImportResult ShortcutManager::importBindingsCsv(const QByteArray& bytes)
{
    ShortcutImportResult result;
    const QList<ParsedShortcutRow> records = parseShortcutCsv(bytes, result.errors);
    if (!result.ok()) {
        return result;
    }

    struct PendingBinding {
        int actionIndex{-1};
        QKeySequence key;
        bool customized{false};
    };
    QList<PendingBinding> pending;
    QHash<int, int> importedActionLines;

    for (const ParsedShortcutRow& record : records) {
        int actionIndex = -1;
        for (int i = 0; i < m_actions.size(); ++i) {
            if (m_actions.at(i).id == record.actionId) {
                actionIndex = i;
                break;
            }
        }

        // Stable ids are authoritative. The exact human-readable name is a
        // conservative fallback for a release that renamed an internal id;
        // ambiguous display names remain unknown instead of guessing.
        if (actionIndex < 0) {
            int nameMatch = -1;
            for (int i = 0; i < m_actions.size(); ++i) {
                if (m_actions.at(i).displayName != record.actionName) {
                    continue;
                }
                if (nameMatch >= 0) {
                    nameMatch = -1;
                    break;
                }
                nameMatch = i;
            }
            actionIndex = nameMatch;
        }

        if (actionIndex < 0) {
            result.unknownActions << QStringLiteral("%1 (%2)")
                                         .arg(record.actionName, record.actionId);
            continue;
        }
        if (importedActionLines.contains(actionIndex)) {
            result.errors << QStringLiteral(
                "Line %1 maps to the same local action as line %2 (%3).")
                                 .arg(record.lineNumber)
                                 .arg(importedActionLines.value(actionIndex))
                                 .arg(m_actions.at(actionIndex).displayName);
            continue;
        }
        importedActionLines.insert(actionIndex, record.lineNumber);
        pending << PendingBinding{
            actionIndex,
            record.customized ? record.shortcut : m_actions.at(actionIndex).defaultKey,
            record.customized,
        };
    }
    if (!result.ok()) {
        return result;
    }

    // Import is overlay-only: local actions absent from the CSV keep their
    // existing bindings. Use Reset All to Defaults if you want a clean slate.
    // Apply only after the whole file has parsed and resolved successfully so
    // any file-level error leaves every binding unchanged. Imported
    // customizations win collisions with actions absent from an older backup;
    // then run the same duplicate normalization used at startup so a customized
    // binding also wins over a default restored by a later CSV row.
    for (const PendingBinding& binding : pending) {
        if (!binding.customized || binding.key.isEmpty()) {
            continue;
        }
        for (int i = 0; i < m_actions.size(); ++i) {
            if (i == binding.actionIndex) continue;
            if (importedActionLines.contains(i)) continue;
            if (m_actions.at(i).currentKey != binding.key) continue;
            // Reset persisted along with currentKey so this displacement is not
            // written back as an explicit user clear (which loadBindings would
            // later restore as "pinned to empty" per #3964, silently and
            // permanently destroying the user's original binding).
            result.displacedActions << QStringLiteral("%1 (%2)")
                                           .arg(m_actions.at(i).displayName,
                                                m_actions.at(i).id);
            m_actions[i].currentKey = QKeySequence();
            m_actions[i].persisted = false;
        }
    }
    for (const PendingBinding& binding : pending) {
        Action& target = m_actions[binding.actionIndex];
        // Preserve an explicit local clear (persisted=true, currentKey empty)
        // when the CSV row is not customized — a default-adopting row from a
        // machine that never touched this action must not undo the user's
        // deliberate "unbind this key" here (#3964 guarantee).
        if (!binding.customized && target.persisted && target.currentKey.isEmpty()) {
            continue;
        }
        target.currentKey = binding.key;
        target.persisted = binding.customized;
    }
    normalizeDuplicateBindings();
    result.importedCount = pending.size();
    // Nothing changed — skip the settings write and the observer refresh so a
    // no-op import (every row unknown to this release) doesn't churn either.
    if (!pending.isEmpty()) {
        saveBindings();
        emit bindingsChanged();
    }
    return result;
}

ShortcutExportResult ShortcutManager::exportToFile(const QString& path) const
{
    ShortcutExportResult result;
    if (path.trimmed().isEmpty()) {
        result.error = QStringLiteral("No export path was provided.");
        return result;
    }

    const QByteArray csv = exportBindingsCsv();
    QSaveFile file(path);
    // Some network mounts (SMB, WSL DrvFs) can't create the sidecar temp file
    // QSaveFile normally uses — fall back to a direct write so export succeeds
    // where a plain write would.
    file.setDirectWriteFallback(true);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("Couldn't open %1 for writing (%2).")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    if (file.write(csv) != csv.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        result.error = QStringLiteral("Couldn't write the shortcut backup to %1 (%2).")
                           .arg(QDir::toNativeSeparators(path), reason);
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("Couldn't save %1 (%2).")
                           .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    // Count rows actually written rather than trusting m_actions.size() —
    // stays honest if exportBindingsCsv ever filters rows.
    result.exportedCount = m_actions.size();
    return result;
}

ShortcutImportResult ShortcutManager::importFromFile(const QString& path)
{
    ShortcutImportResult result;
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
    // Bound memory before we allocate: the CSV parser rejects anything above
    // kMaxShortcutCsvBytes anyway, but readAll() would already have buffered
    // the whole file in RAM by then. A stat-and-reject up front turns a
    // mispicked 500 MB file into a fast error instead of a giant alloc.
    if (file.size() > kMaxShortcutCsvBytes) {
        result.errors << QStringLiteral(
            "%1 exceeds the %2-byte shortcut backup limit.")
                             .arg(QDir::toNativeSeparators(path))
                             .arg(kMaxShortcutCsvBytes);
        return result;
    }
    const QByteArray bytes = file.readAll();
    // QFile::readAll returns whatever it managed to read without raising, so a
    // partial read (SMB drop, EIO, yanked USB) would otherwise surface as a
    // misleading CSV-parse error further downstream.
    if (file.error() != QFileDevice::NoError) {
        result.errors << QStringLiteral("Couldn't read %1 (%2).")
                             .arg(QDir::toNativeSeparators(path), file.errorString());
        return result;
    }
    return importBindingsCsv(bytes);
}

void ShortcutManager::rebuildShortcuts(QWidget* parent,
                                       std::function<bool()> guardFn)
{
    // Destroy existing shortcuts
    qDeleteAll(m_shortcuts);
    m_shortcuts.clear();

    for (const auto& a : m_actions) {
        if (a.currentKey.isEmpty() || !a.handler) continue;

        auto* sc = new QShortcut(a.currentKey, parent);
        sc->setAutoRepeat(a.autoRepeat);
        auto handler = a.handler;
        connect(sc, &QShortcut::activated, this, [guardFn, handler]() {
            if (guardFn && !guardFn()) return;
            handler();
        });
        m_shortcuts.append(sc);
    }
}

void ShortcutManager::setShortcutsEnabled(bool enabled)
{
    for (auto* sc : m_shortcuts)
        sc->setEnabled(enabled);
}

ShortcutManager::Action* ShortcutManager::action(const QString& id)
{
    for (auto& a : m_actions) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

const ShortcutManager::Action* ShortcutManager::actionForKey(const QKeySequence& key) const
{
    if (key.isEmpty()) return nullptr;
    for (const auto& a : m_actions) {
        if (a.currentKey == key) return &a;
    }
    return nullptr;
}

QString ShortcutManager::conflictCheck(const QKeySequence& key,
                                       const QString& excludeId) const
{
    if (key.isEmpty()) return {};
    for (const auto& a : m_actions) {
        if (a.id != excludeId && a.currentKey == key)
            return a.displayName;
    }
    return {};
}

QStringList ShortcutManager::categories()
{
    return {"Frequency", "Band", "Mode", "TX", "CW", "Audio", "Slice",
            "Filter", "Tuning", "DSP", "AGC", "EQ", "Display", "RIT/XIT"};
}

} // namespace AetherSDR
