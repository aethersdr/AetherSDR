#include "gui/workspace/WorkspaceMigration.h"

#include "core/AppSettings.h"
#include "gui/workspace/ClassicLayout.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace AetherSDR {

namespace {

// AppSettings stores booleans as the strings "True"/"False" throughout the
// legacy layout keys.  Anything else is treated as absent and falls back to
// the caller's default, rather than being coerced to false — an unreadable
// value must not silently mean "off".
bool legacyBool(const QString& key, bool fallback)
{
    const QString raw = AppSettings::instance().value(key).toString();
    if (raw.compare(QLatin1String("True"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (raw.compare(QLatin1String("False"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    return fallback;
}

// Container ids that ContainerTree says are floating right now.  These are
// the applets migration must leave alone.
QSet<QString> floatingContainerIds()
{
    QSet<QString> floating;

    const QString raw =
        AppSettings::instance().value(QStringLiteral("ContainerTree")).toString();
    if (raw.isEmpty()) {
        return floating;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // A corrupt container tree is not a reason to fail the migration:
        // Classic without float information is still a usable Classic, and
        // the legacy keys are untouched either way.
        return floating;
    }

    const QJsonObject containers =
        doc.object().value(QStringLiteral("containers")).toObject();
    for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
        if (it.value().toObject().value(QStringLiteral("mode")).toString()
            == QLatin1String("floating")) {
            floating.insert(it.key());
        }
    }
    return floating;
}

}  // namespace

QString classicWorkspaceId()
{
    return QStringLiteral("classic");
}

LegacyLayoutState readLegacyLayoutState(const QStringList& knownAppletIds)
{
    auto& settings = AppSettings::instance();

    LegacyLayoutState state;

    const QString layoutId =
        settings.value(QStringLiteral("PanadapterLayout")).toString().trimmed();
    if (!layoutId.isEmpty() && panCountForLayout(layoutId) > 0) {
        state.panLayoutId = layoutId;
    }

    state.floatingPanIds =
        settings.value(QStringLiteral("FloatingPanIds")).toString()
            .split(QLatin1Char(','), Qt::SkipEmptyParts);

    state.appletsLeft = legacyBool(QStringLiteral("AppletPanelDockedLeft"), false);
    state.appletPanelVisible = legacyBool(QStringLiteral("AppletPanelVisible"), true);

    // Column order: AppletOrder first, then anything the operator has never
    // reordered, in the caller's canonical order.  A saved order that names
    // an applet this build no longer has is skipped rather than trusted.
    const QStringList savedOrder =
        settings.value(QStringLiteral("AppletOrder")).toString()
            .split(QLatin1Char(','), Qt::SkipEmptyParts);

    QStringList ordered;
    for (const QString& id : savedOrder) {
        if (knownAppletIds.contains(id) && !ordered.contains(id)) {
            ordered.append(id);
        }
    }
    for (const QString& id : knownAppletIds) {
        if (!ordered.contains(id)) {
            ordered.append(id);
        }
    }

    const QSet<QString> floating = floatingContainerIds();

    for (const QString& id : ordered) {
        // Applet_<ID> is absent until the operator toggles that applet, and
        // its default differs per applet.  AppletPanel owns those defaults,
        // so an unset key here means "not known to be open" and the applet is
        // simply not placed — Classic under-populating is recoverable, and
        // guessing wrong would put windows on screen nobody asked for.
        if (!settings.contains(QStringLiteral("Applet_") + id)) {
            continue;
        }
        if (!legacyBool(QStringLiteral("Applet_") + id, false)) {
            continue;
        }
        if (floating.contains(id)) {
            state.floatingAppletIds.append(id);   // pop-out stays; not placed
            continue;
        }
        state.openAppletIds.append(id);
    }

    return state;
}

WorkspaceDocument buildClassicDocument(const LegacyLayoutState& legacy,
                                       const QStringList& panIds)
{
    // A hidden applet panel means Classic has no column — the pans take the
    // whole surface, which is exactly what the operator sees today.
    const QStringList appletIds =
        legacy.appletPanelVisible ? legacy.openAppletIds : QStringList{};

    WorkspaceSurface main;
    main.id    = WorkspaceSurface::kMainId;
    main.items = composeClassic(panIds, appletIds, legacy.panLayoutId,
                                legacy.appletsLeft);

    Workspace classic;
    classic.id    = classicWorkspaceId();
    classic.label = QStringLiteral("Classic");
    classic.surfaces.append(main);

    WorkspaceDocument doc;
    doc.workspaces.append(classic);
    doc.activeWorkspace = classic.id;
    return doc;
}

}  // namespace AetherSDR
