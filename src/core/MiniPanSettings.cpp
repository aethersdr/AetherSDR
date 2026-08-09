#include "core/MiniPanSettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QString>

#include <cmath>

namespace AetherSDR {

namespace {
const QString kMiniPanKey = QStringLiteral("MiniPan");

const QString kGeometryField    = QStringLiteral("geometryBase64");
const QString kOpenField        = QStringLiteral("open");
const QString kSpanField        = QStringLiteral("spanKHz");
const QString kAlwaysOnTopField = QStringLiteral("alwaysOnTop");
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

QByteArray MiniPanSettings::geometryBase64()
{
    return readObj().value(kGeometryField).toString().toUtf8();
}

void MiniPanSettings::setGeometryBase64(const QByteArray& base64)
{
    QJsonObject o = readObj();
    o[kGeometryField] = QString::fromUtf8(base64);
    write(o);
}

bool MiniPanSettings::open()
{
    return readObj().value(kOpenField).toBool(false);
}

void MiniPanSettings::setOpen(bool on)
{
    QJsonObject o = readObj();
    o[kOpenField] = on;
    write(o);
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

bool MiniPanSettings::alwaysOnTop()
{
    return readObj().value(kAlwaysOnTopField).toBool(false);
}

void MiniPanSettings::setAlwaysOnTop(bool on)
{
    QJsonObject o = readObj();
    o[kAlwaysOnTopField] = on;
    write(o);
}

} // namespace AetherSDR
