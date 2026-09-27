#include "WfmPresentationSettings.h"
#include "core/AppSettings.h"
#include <QJsonDocument>
#include <QJsonObject>

namespace AetherSDR {
namespace {
QJsonObject document()
{
    return QJsonDocument::fromJson(AppSettings::instance()
        .value(QStringLiteral("WfmApplet"), QString{}).toString().toUtf8()).object();
}
void save(QJsonObject value, const QJsonObject& ui)
{
    value.insert(QStringLiteral("ui"), ui);
    AppSettings::instance().setValue(QStringLiteral("WfmApplet"),
        QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    AppSettings::instance().save();
}
}
WfmPresentationSettings& WfmPresentationSettings::instance()
{
    static WfmPresentationSettings settings;
    return settings;
}
bool WfmPresentationSettings::broadcastOverlayEnabled() const
{
    return document().value(QStringLiteral("ui")).toObject()
        .value(QStringLiteral("showBroadcastOverlay")).toBool(true);
}
void WfmPresentationSettings::setBroadcastOverlayEnabled(bool enabled)
{
    const bool changed = broadcastOverlayEnabled() != enabled;
    QJsonObject value = document();
    QJsonObject ui = value.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("showBroadcastOverlay"), enabled);
    save(value, ui);
    if (changed) { emit overlayEnabledChanged(enabled); }
}
bool WfmPresentationSettings::showLockScope() const
{
    return document().value(QStringLiteral("ui")).toObject()
        .value(QStringLiteral("showLockScope")).toBool(true);
}
bool WfmPresentationSettings::showDiagnostics() const
{
    return document().value(QStringLiteral("ui")).toObject()
        .value(QStringLiteral("showDiagnostics")).toBool(false);
}
void WfmPresentationSettings::setAppletOptions(bool scope, bool diagnostics)
{
    QJsonObject value = document();
    QJsonObject ui = value.value(QStringLiteral("ui")).toObject();
    ui.insert(QStringLiteral("showLockScope"), scope);
    ui.insert(QStringLiteral("showDiagnostics"), diagnostics);
    save(value, ui);
}
}
