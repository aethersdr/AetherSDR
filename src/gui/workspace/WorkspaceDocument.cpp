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

            if (so.contains(QStringLiteral("items"))
                && !so.value(QStringLiteral("items")).isArray()) {
                appendWarning(warnings,
                              QStringLiteral("surface '%1/%2' has a non-array items field")
                                  .arg(ws.id, surface.id));
            }

            QSet<QString> seenItemIds;
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
