#pragma once

#include "AppSettings.h"
#include "NnrControls.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>

namespace AetherSDR {

// NNR configuration as one nested-JSON object under AppSettings["Nnr"] (mirrors
// NvidiaBnrSettings); the older NR methods keep their flat keys. Defaults come
// from NnrControls.h so a fresh install matches the tab's markers.
class NnrSettings {
public:
    // Whether NNR was the selected ADSP method when the session ended.
    static bool enabled() { return readObj().value("enabled").toBool(false); }
    static void setEnabled(bool on)
    {
        QJsonObject o = readObj();
        o["enabled"] = on;
        write(o);
    }

    // 0..100, mapped onto the mask floor's range. See NnrControls.h.
    static int strength()
    {
        // Nnr::kMaskFloorDefaultStrength, not 50: the slider's midpoint is
        // -30 dB, five decibels more aggressive than the -25 dB WDSP starts
        // from, and nothing about "the middle" made that the right default.
        return std::clamp(
            readObj().value("strength").toInt(Nnr::kMaskFloorDefaultStrength),
            0, 100);
    }
    static void setStrength(int v)
    {
        QJsonObject o = readObj();
        o["strength"] = std::clamp(v, 0, 100);
        write(o);
    }

    // 0 = Standard, 1 = Premium.
    static int model()
    {
        return std::clamp(readObj().value("model").toInt(0), 0, 1);
    }
    static void setModel(int slot)
    {
        QJsonObject o = readObj();
        o["model"] = std::clamp(slot, 0, 1);
        write(o);
    }

    // The tuning controls WDSP leaves undocumented. Each defaults to the value
    // WDSP itself starts from, so an untouched install behaves exactly as
    // upstream does.
    static double alpha() { return readDouble("alpha", Nnr::kAlpha); }
    static void setAlpha(double v) { writeDouble("alpha", v, Nnr::kAlpha); }

    static double alphaKnee() { return readDouble("alphaKnee", Nnr::kAlphaKnee); }
    static void setAlphaKnee(double v) { writeDouble("alphaKnee", v, Nnr::kAlphaKnee); }

    static double tau() { return readDouble("tau", Nnr::kTau); }
    static void setTau(double v) { writeDouble("tau", v, Nnr::kTau); }

    static double maxGain() { return readDouble("maxGain", Nnr::kMaxGain); }
    static void setMaxGain(double v) { writeDouble("maxGain", v, Nnr::kMaxGain); }

    static double smoothAttackMs()
    {
        return readDouble("smoothAttackMs", Nnr::kSmoothAttack);
    }
    static void setSmoothAttackMs(double v)
    {
        writeDouble("smoothAttackMs", v, Nnr::kSmoothAttack);
    }

    static double smoothReleaseMs()
    {
        return readDouble("smoothReleaseMs", Nnr::kSmoothRelease);
    }
    static void setSmoothReleaseMs(double v)
    {
        writeDouble("smoothReleaseMs", v, Nnr::kSmoothRelease);
    }

private:
    static double readDouble(const char* key, const Nnr::ControlSpec& spec)
    {
        const double v = readObj().value(QLatin1String(key))
                             .toDouble(spec.defaultValue);
        return std::clamp(v, spec.minimum, spec.maximum);
    }
    static void writeDouble(const char* key, double v,
                            const Nnr::ControlSpec& spec)
    {
        QJsonObject o = readObj();
        o[QLatin1String(key)] = std::clamp(v, spec.minimum, spec.maximum);
        write(o);
    }

    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("Nnr", QString{}).toString();
        if (json.isEmpty()) {
            return {};
        }
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("Nnr",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

}  // namespace AetherSDR
