#include "gui/workspace/WorkspaceDocument.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>

namespace AetherSDR {

const QString WorkspaceSurface::kMainId = QStringLiteral("main");

namespace {

QJsonArray rectToJson(const NormRect& r)
{
    return QJsonArray{r.x, r.y, r.w, r.h};
}

// A rect is four finite numbers.  Anything else is not "a rect we can fix" —
// it is a rect we never wrote, so the item carrying it is dropped.
bool rectFromJson(const QJsonValue& v, NormRect* out)
{
    if (!v.isArray()) {
        return false;
    }
    const QJsonArray a = v.toArray();
    if (a.size() != 4) {
        return false;
    }
    for (const QJsonValue& n : a) {
        if (!n.isDouble()) {
            return false;
        }
    }
    NormRect r;
    r.x = a.at(0).toDouble();
    r.y = a.at(1).toDouble();
    r.w = a.at(2).toDouble();
    r.h = a.at(3).toDouble();
    if (!r.isValid()) {
        return false;
    }
    *out = r;
    return true;
}

QJsonObject itemToJson(const CanvasItem& it)
{
    QJsonObject o;
    o[QStringLiteral("id")]   = it.id;
    o[QStringLiteral("rect")] = rectToJson(it.rect);
    o[QStringLiteral("z")]    = it.z;
    if (!it.contentType.isEmpty()) {
        o[QStringLiteral("type")] = it.contentType;
    }
    if (it.closed) {
        o[QStringLiteral("closed")] = true;
    }
    if (it.collapsed) {
        o[QStringLiteral("collapsed")] = true;
    }
    // minimumSize is deliberately NOT persisted: it is a property of the
    // content, not of the operator's arrangement.  Whatever rehydrates the
    // widget re-declares it, so a content change ships a new floor instead of
    // being overridden by a stale one saved months ago.
    return o;
}

void appendWarning(QStringList* warnings, const QString& text)
{
    if (warnings) {
        warnings->append(text);
    }
}

}  // namespace

const WorkspaceSurface* Workspace::surface(const QString& surfaceId) const
{
    for (const WorkspaceSurface& s : surfaces) {
        if (s.id == surfaceId) {
            return &s;
        }
    }
    return nullptr;
}

const Workspace* WorkspaceDocument::workspace(const QString& id) const
{
    for (const Workspace& w : workspaces) {
        if (w.id == id) {
            return &w;
        }
    }
    return nullptr;
}

QStringList WorkspaceDocument::workspaceIds() const
{
    QStringList out;
    out.reserve(workspaces.size());
    for (const Workspace& w : workspaces) {
        out.append(w.id);
    }
    return out;
}

QString WorkspaceDocument::boundWorkspace(const QString& radioProfile) const
{
    const QString id = bindings.value(radioProfile);
    return contains(id) ? id : QString();
}

QString WorkspaceDocument::uniqueWorkspaceId() const
{
    int n = 1;
    while (contains(QStringLiteral("ws-%1").arg(n))) {
        ++n;
    }
    return QStringLiteral("ws-%1").arg(n);
}

QString WorkspaceDocument::uniqueLabel(const QString& base) const
{
    const QString wanted = base.trimmed().isEmpty()
                               ? QStringLiteral("Workspace")
                               : base.trimmed();
    auto taken = [this](const QString& l) {
        for (const Workspace& w : workspaces) {
            if (w.label == l) return true;
        }
        return false;
    };
    if (!taken(wanted)) {
        return wanted;
    }
    int n = 2;
    while (taken(QStringLiteral("%1 (%2)").arg(wanted).arg(n))) {
        ++n;
    }
    return QStringLiteral("%1 (%2)").arg(wanted).arg(n);
}

QString WorkspaceDocument::addDuplicateOf(const QString& sourceId,
                                          const QString& label)
{
    const Workspace* src = workspace(sourceId);
    if (!src) {
        return QString();
    }
    Workspace w = *src;   // deep copy: surfaces and items are value types
    w.id    = uniqueWorkspaceId();
    w.label = uniqueLabel(label);
    workspaces.append(w);
    return w.id;
}

QString WorkspaceDocument::addBlank(const QString& label)
{
    Workspace w;
    w.id    = uniqueWorkspaceId();
    w.label = uniqueLabel(label);
    WorkspaceSurface main;
    main.id = WorkspaceSurface::kMainId;
    w.surfaces.append(main);
    workspaces.append(w);
    return w.id;
}

bool WorkspaceDocument::renameWorkspace(const QString& id, const QString& label)
{
    for (Workspace& w : workspaces) {
        if (w.id == id) {
            // Exclude SELF from the collision scan (review M4): renaming a
            // workspace to its own label used to come back "Label (2)".
            w.label.clear();
            w.label = uniqueLabel(label);
            return true;
        }
    }
    return false;
}

QString WorkspaceDocument::uniqueSurfaceId(const QString& workspaceId) const
{
    const Workspace* w = workspace(workspaceId);
    if (!w) {
        return QString();
    }
    int n = 2;
    while (w->surface(QStringLiteral("canvas%1").arg(n))) {
        ++n;
    }
    return QStringLiteral("canvas%1").arg(n);
}

QString WorkspaceDocument::addSurface(const QString& workspaceId,
                                      const QString& label)
{
    for (Workspace& w : workspaces) {
        if (w.id != workspaceId) continue;
        WorkspaceSurface s;
        s.id = uniqueSurfaceId(workspaceId);
        // Labels are de-duplicated within the workspace, same reasoning as
        // workspace labels: the window-list menu and the bridge address
        // surfaces by label as operator text.
        const QString wanted = label.trimmed().isEmpty()
                                   ? QStringLiteral("Canvas")
                                   : label.trimmed();
        auto taken = [&w](const QString& l) {
            for (const WorkspaceSurface& surf : w.surfaces) {
                if (surf.label == l) return true;
            }
            return false;
        };
        QString unique = wanted;
        int n = 2;
        while (taken(unique)) {
            unique = QStringLiteral("%1 (%2)").arg(wanted).arg(n++);
        }
        s.label = unique;
        w.surfaces.append(s);
        return s.id;
    }
    return QString();
}

bool WorkspaceDocument::removeSurface(const QString& workspaceId,
                                      const QString& surfaceId)
{
    if (surfaceId == WorkspaceSurface::kMainId) {
        return false;
    }
    for (Workspace& w : workspaces) {
        if (w.id != workspaceId) continue;
        int idx = -1;
        for (int i = 0; i < w.surfaces.size(); ++i) {
            if (w.surfaces.at(i).id == surfaceId) { idx = i; break; }
        }
        if (idx < 0) {
            return false;
        }
        const QList<CanvasItem> orphans = w.surfaces.at(idx).items;
        w.surfaces.removeAt(idx);
        for (WorkspaceSurface& s : w.surfaces) {
            if (s.id == WorkspaceSurface::kMainId) {
                // Identity is workspace-wide (the parser enforces it), so a
                // straight append cannot collide.
                s.items.append(orphans);
                break;
            }
        }
        return true;
    }
    return false;
}

bool WorkspaceDocument::renameSurface(const QString& workspaceId,
                                      const QString& surfaceId,
                                      const QString& label)
{
    for (Workspace& w : workspaces) {
        if (w.id != workspaceId) continue;
        for (WorkspaceSurface& s : w.surfaces) {
            if (s.id != surfaceId) continue;
            const QString wanted = label.trimmed().isEmpty()
                                       ? QStringLiteral("Canvas")
                                       : label.trimmed();
            // Exclude self from the collision scan (the workspace-rename
            // M4 lesson, applied on arrival).
            auto taken = [&w, &s](const QString& l) {
                for (const WorkspaceSurface& other : w.surfaces) {
                    if (&other != &s && other.label == l) return true;
                }
                return false;
            };
            QString unique = wanted;
            int n = 2;
            while (taken(unique)) {
                unique = QStringLiteral("%1 (%2)").arg(wanted).arg(n++);
            }
            s.label = unique;
            return true;
        }
        return false;
    }
    return false;
}

bool WorkspaceDocument::removeWorkspace(const QString& id)
{
    if (workspaces.size() <= 1 || !contains(id)) {
        return false;
    }
    for (int i = 0; i < workspaces.size(); ++i) {
        if (workspaces.at(i).id == id) {
            workspaces.removeAt(i);
            break;
        }
    }
    // Bindings to the removed workspace drop — the parser's dangling-target
    // rule, applied at the edit instead of waiting for the next load.
    for (auto it = bindings.begin(); it != bindings.end();) {
        if (it.value() == id) it = bindings.erase(it);
        else ++it;
    }
    if (activeWorkspace == id) {
        activeWorkspace = workspaces.first().id;
    }
    return true;
}

QJsonObject WorkspaceDocument::toJson() const
{
    QJsonArray wsArray;
    for (const Workspace& w : workspaces) {
        QJsonArray surfaceArray;
        for (const WorkspaceSurface& s : w.surfaces) {
            QJsonArray itemArray;
            for (const CanvasItem& it : s.items) {
                itemArray.append(itemToJson(it));
            }

            QJsonObject so;
            so[QStringLiteral("id")]    = s.id;
            so[QStringLiteral("items")] = itemArray;
            if (!s.label.isEmpty()) {
                so[QStringLiteral("label")] = s.label;
            }
            if (!s.windowGeometry.isEmpty()) {
                so[QStringLiteral("windowGeometry")] =
                    QString::fromLatin1(s.windowGeometry.toBase64());
            }
            if (s.hidden) {
                so[QStringLiteral("hidden")] = true;
            }
            surfaceArray.append(so);
        }

        QJsonObject wo;
        wo[QStringLiteral("id")]       = w.id;
        wo[QStringLiteral("surfaces")] = surfaceArray;
        if (!w.label.isEmpty()) {
            wo[QStringLiteral("label")] = w.label;
        }
        wsArray.append(wo);
    }

    QJsonObject bindingsObj;
    for (auto it = bindings.constBegin(); it != bindings.constEnd(); ++it) {
        bindingsObj[it.key()] = it.value();
    }

    QJsonObject root;
    root[QStringLiteral("version")]         = kSchemaVersion;
    root[QStringLiteral("activeWorkspace")] = activeWorkspace;
    root[QStringLiteral("workspaces")]      = wsArray;
    if (canvasEnabled) {
        root[QStringLiteral("canvasEnabled")] = true;
    }
    if (!bindingsObj.isEmpty()) {
        root[QStringLiteral("bindings")] = bindingsObj;
    }
    return root;
}

bool WorkspaceDocument::fromJson(const QJsonObject& root,
                                 WorkspaceDocument* out,
                                 QString* error,
                                 QStringList* warnings)
{
    if (!out) {
        return false;
    }

    const auto fail = [error](const QString& why) {
        if (error) {
            *error = why;
        }
        return false;
    };

    const QJsonValue versionValue = root.value(QStringLiteral("version"));
    if (!versionValue.isDouble()) {
        return fail(QStringLiteral("no schema version"));
    }
    const int version = versionValue.toInt();
    if (version < 1) {
        return fail(QStringLiteral("invalid schema version %1").arg(version));
    }
    if (version > kSchemaVersion) {
        // A newer build wrote this.  Rewriting it from our understanding
        // would silently drop whatever it knows that we do not.
        return fail(QStringLiteral("schema version %1 is newer than %2")
                        .arg(version)
                        .arg(kSchemaVersion));
    }
    if (!root.value(QStringLiteral("workspaces")).isArray()) {
        return fail(QStringLiteral("no workspaces array"));
    }

    WorkspaceDocument doc;

    for (const QJsonValue& wsValue : root.value(QStringLiteral("workspaces")).toArray()) {
        if (!wsValue.isObject()) {
            appendWarning(warnings, QStringLiteral("dropped a non-object workspace"));
            continue;
        }
        const QJsonObject wo = wsValue.toObject();

        Workspace ws;
        ws.id    = wo.value(QStringLiteral("id")).toString();
        ws.label = wo.value(QStringLiteral("label")).toString();
        if (ws.id.isEmpty()) {
            appendWarning(warnings, QStringLiteral("dropped a workspace with no id"));
            continue;
        }
        if (doc.contains(ws.id)) {
            appendWarning(warnings,
                          QStringLiteral("dropped duplicate workspace '%1'").arg(ws.id));
            continue;
        }

        if (wo.contains(QStringLiteral("surfaces"))
            && !wo.value(QStringLiteral("surfaces")).isArray()) {
            appendWarning(warnings,
                          QStringLiteral("workspace '%1' has a non-array surfaces field")
                              .arg(ws.id));
        }
        // Item ids are WORKSPACE-wide identity (phase 7): an item on two
        // surfaces of one workspace would be placed twice by the replay
        // and fight itself.  The dedup set therefore spans the surface
        // loop, not each surface.
        QSet<QString> seenItemIds;
        for (const QJsonValue& sValue : wo.value(QStringLiteral("surfaces")).toArray()) {
            if (!sValue.isObject()) {
                appendWarning(warnings,
                              QStringLiteral("dropped a non-object surface in '%1'")
                                  .arg(ws.id));
                continue;
            }
            const QJsonObject so = sValue.toObject();

            WorkspaceSurface surface;
            surface.id    = so.value(QStringLiteral("id")).toString();
            surface.label = so.value(QStringLiteral("label")).toString();
            if (surface.id.isEmpty()) {
                appendWarning(warnings,
                              QStringLiteral("dropped a surface with no id in '%1'")
                                  .arg(ws.id));
                continue;
            }
            if (ws.surface(surface.id)) {
                appendWarning(warnings,
                              QStringLiteral("dropped duplicate surface '%1' in '%2'")
                                  .arg(surface.id, ws.id));
                continue;
            }

            const QString geometry =
                so.value(QStringLiteral("windowGeometry")).toString();
            if (!geometry.isEmpty()) {
                // Decode strictly.  The default fromBase64() silently ignores
                // invalid characters and returns whatever it managed, so
                // mangled input would become plausible-looking garbage that
                // QWidget::restoreGeometry() rejects later without a word.
                // This is a hint either way — dropping it costs one drag; the
                // value is that it lands in warnings() instead of nowhere.
                const auto decoded = QByteArray::fromBase64Encoding(
                    geometry.toLatin1(),
                    QByteArray::Base64Encoding
                        | QByteArray::AbortOnBase64DecodingErrors);
                if (decoded.decodingStatus == QByteArray::Base64DecodingStatus::Ok) {
                    surface.windowGeometry = *decoded;
                } else {
                    appendWarning(warnings,
                                  QStringLiteral("dropped an unreadable window "
                                                 "geometry hint on '%1/%2'")
                                      .arg(ws.id, surface.id));
                }
            }

            surface.hidden = so.value(QStringLiteral("hidden")).toBool(false);
            if (surface.hidden && surface.id == WorkspaceSurface::kMainId) {
                // The main window's canvas cannot be "closed" — repair.
                surface.hidden = false;
                appendWarning(warnings,
                              QStringLiteral("main surface of '%1' was marked "
                                             "hidden").arg(ws.id));
            }

            if (so.contains(QStringLiteral("items"))
                && !so.value(QStringLiteral("items")).isArray()) {
                appendWarning(warnings,
                              QStringLiteral("surface '%1/%2' has a non-array items field")
                                  .arg(ws.id, surface.id));
            }

            for (const QJsonValue& iValue : so.value(QStringLiteral("items")).toArray()) {
                if (!iValue.isObject()) {
                    appendWarning(warnings,
                                  QStringLiteral("dropped a non-object item in '%1/%2'")
                                      .arg(ws.id, surface.id));
                    continue;
                }
                const QJsonObject io = iValue.toObject();

                CanvasItem item;
                item.id = io.value(QStringLiteral("id")).toString();
                if (item.id.isEmpty()) {
                    appendWarning(warnings,
                                  QStringLiteral("dropped an item with no id in '%1/%2'")
                                      .arg(ws.id, surface.id));
                    continue;
                }
                if (seenItemIds.contains(item.id)) {
                    appendWarning(warnings,
                                  QStringLiteral("dropped duplicate item '%1' in '%2/%3'")
                                      .arg(item.id, ws.id, surface.id));
                    continue;
                }
                if (!rectFromJson(io.value(QStringLiteral("rect")), &item.rect)) {
                    appendWarning(warnings,
                                  QStringLiteral("dropped item '%1' with an invalid rect")
                                      .arg(item.id));
                    continue;
                }

                item.contentType = io.value(QStringLiteral("type")).toString();
                item.z           = io.value(QStringLiteral("z")).toInt();
                item.collapsed   = io.value(QStringLiteral("collapsed")).toBool();
                item.closed      = io.value(QStringLiteral("closed")).toBool();

                seenItemIds.insert(item.id);
                surface.items.append(item);
            }

            ws.surfaces.append(surface);
        }

        // Every workspace has a main surface, even if the stored document
        // forgot one: the main window always has a canvas, so a document that
        // cannot describe it is not usable.
        if (!ws.surface(WorkspaceSurface::kMainId)) {
            WorkspaceSurface main;
            main.id = WorkspaceSurface::kMainId;
            ws.surfaces.prepend(main);
            appendWarning(warnings,
                          QStringLiteral("workspace '%1' had no main surface").arg(ws.id));
        }

        // Labels are identity for humans and for the bridge's switch-by-
        // label: a duplicate parses but leaves the second workspace
        // unreachable by name (review, K6OZY).  Same treatment as ids —
        // repair with a warning rather than drop.
        for (const Workspace& prior : doc.workspaces) {
            if (!ws.label.isEmpty() && prior.label == ws.label) {
                const QString old = ws.label;
                ws.label = doc.uniqueLabel(ws.label);
                appendWarning(warnings,
                              QStringLiteral("workspace '%1' relabelled '%2' "
                                             "(duplicate label '%3')")
                                  .arg(ws.id, ws.label, old));
                break;
            }
        }
        doc.workspaces.append(ws);
    }

    const QJsonObject bindingsObj = root.value(QStringLiteral("bindings")).toObject();
    for (auto it = bindingsObj.constBegin(); it != bindingsObj.constEnd(); ++it) {
        const QString target = it.value().toString();
        if (target.isEmpty() || !doc.contains(target)) {
            appendWarning(warnings,
                          QStringLiteral("dropped binding '%1' -> '%2' (no such workspace)")
                              .arg(it.key(), target));
            continue;
        }
        doc.bindings.insert(it.key(), target);
    }

    doc.canvasEnabled = root.value(QStringLiteral("canvasEnabled")).toBool(false);

    doc.activeWorkspace = root.value(QStringLiteral("activeWorkspace")).toString();
    if (!doc.activeWorkspace.isEmpty() && !doc.contains(doc.activeWorkspace)) {
        appendWarning(warnings,
                      QStringLiteral("active workspace '%1' does not exist")
                          .arg(doc.activeWorkspace));
        doc.activeWorkspace.clear();
    }
    if (doc.activeWorkspace.isEmpty() && !doc.workspaces.isEmpty()) {
        doc.activeWorkspace = doc.workspaces.first().id;
    }

    *out = doc;
    return true;
}

bool WorkspaceDocument::fromStoredJson(const QByteArray& blob,
                                       WorkspaceDocument* out,
                                       QString* error,
                                       QStringList* warnings)
{
    if (blob.isEmpty()) {
        if (error) {
            *error = QStringLiteral("empty document");
        }
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument parsed = QJsonDocument::fromJson(blob, &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
        if (error) {
            *error = parseError.error != QJsonParseError::NoError
                         ? parseError.errorString()
                         : QStringLiteral("document root is not an object");
        }
        return false;
    }
    return fromJson(parsed.object(), out, error, warnings);
}

QByteArray WorkspaceDocument::toStoredJson() const
{
    return QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
}

}  // namespace AetherSDR
