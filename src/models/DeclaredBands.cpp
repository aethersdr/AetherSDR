#include "DeclaredBands.h"

#include "BandDefs.h"

namespace AetherSDR {

QStringList parseDeclaredBands(const QString& csv)
{
    QStringList out;
    const QStringList parts = csv.split(',', Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const QString name = part.trimmed();
        for (const auto& def : kBands) {
            const QString canon = QString::fromLatin1(def.name);
            if (name.compare(canon, Qt::CaseInsensitive) == 0) {
                if (!out.contains(canon))
                    out.append(canon);
                break;
            }
        }
    }
    return out;
}

} // namespace AetherSDR
