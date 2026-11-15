#pragma once

#include "AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace AetherSDR {

// Per-feature configuration for BNR (the in-process NVIDIA Maxine AFX
// denoiser), stored as a single nested-JSON object under AppSettings["NvidiaBnr"]
// rather than as loose flat keys — per Principle V (each feature owns its
// configuration as one self-contained object). Mirrors DisplaySettings.
//
// (These keys are new in the BNR feature itself, so there is no shipped flat
// data to migrate; pre-merge testers re-accept the licence / re-set intensity
// once.)
class NvidiaBnrSettings {
public:
    // Denoising strength, 0.0 (passthrough) .. 1.0 (max). Default: full.
    static float intensity()
    {
        return static_cast<float>(readObj().value("intensity").toDouble(1.0));
    }
    static void setIntensity(float v)
    {
        QJsonObject o = readObj();
        o["intensity"] = static_cast<double>(v);
        write(o);
    }

    // Whether the user accepted NVIDIA's software licence for the BNR pack.
    static bool licenseAccepted()
    {
        return readObj().value("licenseAccepted").toBool(false);
    }
    static void setLicenseAccepted(bool on)
    {
        QJsonObject o = readObj();
        o["licenseAccepted"] = on;
        write(o);
    }

private:
    static QJsonObject readObj()
    {
        const QString json =
            AppSettings::instance().value("NvidiaBnr", QString{}).toString();
        if (json.isEmpty())
            return {};
        return QJsonDocument::fromJson(json.toUtf8()).object();
    }
    static void write(const QJsonObject& o)
    {
        auto& s = AppSettings::instance();
        s.setValue("NvidiaBnr",
                   QString::fromUtf8(
                       QJsonDocument(o).toJson(QJsonDocument::Compact)));
        s.save();
    }
};

} // namespace AetherSDR
