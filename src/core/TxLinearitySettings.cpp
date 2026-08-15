#include "core/TxLinearitySettings.h"

#include "core/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace AetherSDR {
namespace {

const QString kRootKey = QStringLiteral("TxLinearity");

constexpr const char* kFieldDaxChannel = "daxChannel";
constexpr const char* kFieldToneSpacing = "toneSpacingHz";
constexpr const char* kFieldTunePower = "tunePowerW";
constexpr const char* kFieldAverages = "averages";
constexpr const char* kFieldTimeout = "timeoutSec";
constexpr const char* kFieldWindow = "window";
constexpr const char* kFieldMaxOrder = "maxOrder";

// Read one field, keeping the caller's default when the key is absent OR
// stored with the wrong JSON type. isDouble()/isString() rather than a bare
// toInt() so a document that has drifted degrades field by field instead of
// silently substituting zero — which for `averages` or `timeoutSec` would be a
// meaningfully wrong configuration, not a cosmetic one.
int readInt(const QJsonObject& obj, const char* field, int fallback)
{
    const QJsonValue v = obj.value(QLatin1String(field));
    return v.isDouble() ? v.toInt(fallback) : fallback;
}

double readDouble(const QJsonObject& obj, const char* field, double fallback)
{
    const QJsonValue v = obj.value(QLatin1String(field));
    return v.isDouble() ? v.toDouble(fallback) : fallback;
}

QString readString(const QJsonObject& obj, const char* field, const QString& fallback)
{
    const QJsonValue v = obj.value(QLatin1String(field));
    return v.isString() ? v.toString() : fallback;
}

}  // namespace

QString TxLinearitySettings::rootKey()
{
    return kRootKey;
}

void TxLinearitySettings::clampToValidRanges()
{
    daxChannel = std::clamp(daxChannel, 1, 4);
    toneSpacingHz = std::clamp(toneSpacingHz, 200.0, 10000.0);
    tunePowerW = std::clamp(tunePowerW, 1, 100);
    averages = std::clamp(averages, 1, 64);
    // Never above the 10 s ceiling §8 fixes, and never zero — "no timeout" is
    // not a configuration that can exist.
    timeoutSec = std::clamp(timeoutSec, 1, 10);
    // Odd orders only, and 7 is the practical limit for a feedback path with
    // ~80 dB of dynamic range.
    if (maxOrder != 3 && maxOrder != 5 && maxOrder != 7) {
        maxOrder = 7;
    }
    // An unrecognised window name falls back to the default rather than
    // leaving the combo with no valid selection; windowKindFromKey() applies
    // the same fallback on the DSP side, so the two agree.
    if (window != QStringLiteral("blackman-harris-7")
        && window != QStringLiteral("blackman-harris-4")
        && window != QStringLiteral("hann") && window != QStringLiteral("flat-top")) {
        window = QStringLiteral("blackman-harris-7");
    }
}

TxLinearitySettings TxLinearitySettings::load()
{
    TxLinearitySettings out;

    const QString json = AppSettings::instance().value(kRootKey, QString{}).toString();
    if (json.isEmpty()) {
        return out;   // never stored: the compiled-in defaults ARE the answer
    }

    const QJsonObject obj = QJsonDocument::fromJson(json.toUtf8()).object();
    out.daxChannel = readInt(obj, kFieldDaxChannel, out.daxChannel);
    out.toneSpacingHz = readDouble(obj, kFieldToneSpacing, out.toneSpacingHz);
    out.tunePowerW = readInt(obj, kFieldTunePower, out.tunePowerW);
    out.averages = readInt(obj, kFieldAverages, out.averages);
    out.timeoutSec = readInt(obj, kFieldTimeout, out.timeoutSec);
    out.window = readString(obj, kFieldWindow, out.window);
    out.maxOrder = readInt(obj, kFieldMaxOrder, out.maxOrder);

    out.clampToValidRanges();
    return out;
}

void TxLinearitySettings::save() const
{
    QJsonObject obj;
    obj[QLatin1String(kFieldDaxChannel)] = daxChannel;
    obj[QLatin1String(kFieldToneSpacing)] = toneSpacingHz;
    obj[QLatin1String(kFieldTunePower)] = tunePowerW;
    obj[QLatin1String(kFieldAverages)] = averages;
    obj[QLatin1String(kFieldTimeout)] = timeoutSec;
    obj[QLatin1String(kFieldWindow)] = window;
    obj[QLatin1String(kFieldMaxOrder)] = maxOrder;

    AppSettings& settings = AppSettings::instance();
    settings.setValue(kRootKey,
                      QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
    settings.save();
}

}  // namespace AetherSDR
