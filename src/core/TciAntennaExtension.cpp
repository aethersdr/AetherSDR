#include "TciAntennaExtension.h"

namespace AetherSDR::TciAntennaExtension {

bool representable(const QString& name)
{
    return !name.isEmpty() && !name.contains(QLatin1Char(':'))
        && !name.contains(QLatin1Char(',')) && !name.contains(QLatin1Char(';'));
}

QStringList wireNames(const QStringList& names)
{
    QStringList out;
    for (const QString& raw : names) {
        const QString name = raw.trimmed();
        if (representable(name) && !out.contains(name)) {
            out << name;
        }
    }
    return out;
}

QString listedName(const QStringList& wireList, const QString& name)
{
    for (const QString& listed : wireList) {
        if (listed.compare(name, Qt::CaseInsensitive) == 0) {
            return listed;
        }
    }
    return {};
}

bool rxOffered(bool connected, bool anyList)
{
    return connected && anyList;
}

bool txOffered(bool connected, bool canTransmit, bool anyList)
{
    return connected && canTransmit && anyList;
}

std::optional<Request> parse(const QStringList& args, bool allowSet)
{
    if (args.isEmpty() || args.size() > (allowSet ? 2 : 1)) {
        return std::nullopt;
    }
    bool ok = false;
    const int trx = args.at(0).toInt(&ok);
    if (!ok || trx < 0) {
        return std::nullopt;
    }
    Request request;
    request.trx = trx;
    if (args.size() == 2) {
        if (args.at(1).isEmpty()) {
            return std::nullopt;
        }
        request.isSet = true;
        request.name = args.at(1);
    }
    return request;
}

QString listLine(const char* command, int trx, const QStringList& wireList)
{
    QString line = QStringLiteral("%1:%2").arg(QLatin1String(command)).arg(trx);
    for (const QString& name : wireList) {
        line += QLatin1Char(',') + name;
    }
    return line + QLatin1Char(';');
}

QString selectionLine(const char* command, int trx, const QString& name)
{
    return QStringLiteral("%1:%2,%3;").arg(QLatin1String(command)).arg(trx).arg(name);
}

} // namespace AetherSDR::TciAntennaExtension
