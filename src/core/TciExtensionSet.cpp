#include "TciExtensionSet.h"

#include <QChar>
#include <QString>

#include <utility>

namespace AetherSDR {

namespace {

const QLatin1String kPrefix("aethersdr_");
const QLatin1String kEnableCommand("aethersdr_ext_enable");
const QLatin1String kAdvertisementCommand("aethersdr_ext");

// TCI reserves `:` `,` `;`; a token or name is one lower-case word.
bool isWord(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar ch : text) {
        const bool allowed = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('_');
        if (!allowed) {
            return false;
        }
    }
    return true;
}

} // namespace

bool TciExtensionSet::registerGroup(Group group)
{
    if (!isWord(group.token) || group.commands.isEmpty() || !group.offered || !group.handle) {
        return false;
    }
    for (const QString& name : std::as_const(group.commands)) {
        if (!isWord(name) || !isExtensionCommand(name) || name == kEnableCommand
            || name == kAdvertisementCommand || group.commands.count(name) != 1) {
            return false;
        }
    }
    for (const Group& existing : std::as_const(m_groups)) {
        if (existing.token == group.token) {
            return false;
        }
        for (const QString& name : std::as_const(group.commands)) {
            if (existing.commands.contains(name)) {
                return false;
            }
        }
    }
    m_groups.append(std::move(group));
    return true;
}

QStringList TciExtensionSet::offeredTokens() const
{
    QStringList tokens;
    for (const Group& group : m_groups) {
        if (group.offered()) {
            tokens.append(group.token);
        }
    }
    return tokens;
}

QString TciExtensionSet::advertisement() const
{
    QStringList items{QString::number(kVersion)};
    items += offeredTokens();
    return QString(kAdvertisementCommand) + QLatin1Char(':')
        + items.join(QLatin1Char(',')) + QLatin1Char(';');
}

QStringList TciExtensionSet::stateLines() const
{
    QStringList lines;
    for (const Group& group : m_groups) {
        if (group.offered() && group.stateLines) {
            lines += group.stateLines();
        }
    }
    return lines;
}

const TciExtensionSet::Group* TciExtensionSet::offeredGroupFor(const QString& name) const
{
    for (const Group& group : m_groups) {
        if (group.commands.contains(name)) {
            return group.offered() ? &group : nullptr;
        }
    }
    return nullptr;
}

bool TciExtensionSet::isExtensionCommand(const QString& lowerName)
{
    return lowerName.startsWith(kPrefix);
}

std::optional<bool> TciExtensionSet::parseBool(const QString& value)
{
    const QString v = value.trimmed().toLower();
    if (v == QLatin1String("true") || v == QLatin1String("1")) {
        return true;
    }
    if (v == QLatin1String("false") || v == QLatin1String("0")) {
        return false;
    }
    return std::nullopt;
}

} // namespace AetherSDR
