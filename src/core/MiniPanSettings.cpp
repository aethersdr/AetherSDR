#include "core/MiniPanSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QString>

#include <cmath>

namespace AetherSDR {

namespace {
const QString kMiniPanKey = QStringLiteral("MiniPan");
const QString kSpanField  = QStringLiteral("spanKHz");
} // namespace

QJsonObject MiniPanSettings::readObj()
{
    const QString json =
        AppSettings::instance().value(kMiniPanKey, QString{}).toString();
    if (json.isEmpty())
        return {};
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

void MiniPanSettings::write(const QJsonObject& o)
{
    auto& s = AppSettings::instance();
    s.setValue(kMiniPanKey,
               QString::fromUtf8(
                   QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

double MiniPanSettings::spanKHz()
{
    const double v = readObj().value(kSpanField).toDouble(kSpanNarrowKHz);
    // Only the two spans the UI offers are legal; a hand-edited or
    // future-version value falls back to the narrow default rather than
    // asking the radio for a span it may refuse.
    return (v == kSpanWideKHz) ? kSpanWideKHz : kSpanNarrowKHz;
}

void MiniPanSettings::setSpanKHz(double kHz)
{
    QJsonObject o = readObj();
    o[kSpanField] = (std::isfinite(kHz) && kHz == kSpanWideKHz) ? kSpanWideKHz
                                                                : kSpanNarrowKHz;
    write(o);
}

} // namespace AetherSDR
