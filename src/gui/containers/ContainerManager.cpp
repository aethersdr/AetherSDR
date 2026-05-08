#include "ContainerManager.h"
#include "ContainerTitleBar.h"
#include "FloatingContainerWindow.h"
#include "core/AppSettings.h"

#include <QBoxLayout>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace AetherSDR {

namespace {

constexpr const char* kSettingsKey = "ContainerTree";
constexpr int         kSchemaVersion = 1;

QString dockModeToString(ContainerWidget::DockMode m)
{
    return m == ContainerWidget::DockMode::Floating ? "floating" : "panel";
}

ContainerWidget::DockMode dockModeFromString(const QString& s)
{
    return s == "floating" ? ContainerWidget::DockMode::Floating
                           : ContainerWidget::DockMode::PanelDocked;
}

// Build the AppSettings key for a floating container's geometry.
// Stable across launches so saved geometry survives rename-free.
// Sanitize '/' in IDs like "P/CW" — slashes are invalid in the
// AppSettings XML element names and cause silent save/load failures.
QString geometryKeyFor(const QString& id)
{
    QString safe = id;
    safe.replace('/', '_');
    return QStringLiteral("ContainerGeometry_%1").arg(safe);
}

// Parallel helper for the per-container "always on top" preference
// (issue #2430).  Same '/' sanitisation as geometryKeyFor().
QString alwaysOnTopKeyFor(const QString& id)
{
    QString safe = id;
    safe.replace('/', '_');
    return QStringLiteral("ContainerAlwaysOnTop_%1").arg(safe);
}

} // namespace

ContainerManager::ContainerManager(QObject* parent) : QObject(parent) {}

ContainerManager::~ContainerManager()
{
    // Containers are QObject-parented to the manager in
    // createContainer(); Qt's parent-child cleanup tears them down
    // when we go out of scope.  If a caller reparented a container
    // into a layout (very common), QWidget::setParent transferred
    // ownership — Qt still deletes exactly once, via whichever
    // parent is still alive.
    //
    // Floating windows we own explicitly — close + deleteLater so
    // their pending events drain cleanly.
    for (auto* w : m_floatingWindows) {
        if (w) { w->releaseContainer(); w->close(); w->deleteLater(); }
    }
    m_floatingWindows.clear();
}

void ContainerManager::registerContent(const QString& typeId,
                                       ContentFactory factory)
{
    m_factories.insert(typeId, std::move(factory));
}

ContainerWidget* ContainerManager::createContainer(const QString& id,
                                                   const QString& title,
                                                   const QString& contentType,
                                                   const QString& parentId,
                                                   int index)
{
    if (m_containers.contains(id)) return m_containers.value(id).data();

    auto* c = new ContainerWidget(id, title);
    m_containers.insert(id, c);
    m_meta.insert(id, Meta{contentType, parentId, nullptr, -1});

    // Install content widget via factory if one is registered.
    if (!contentType.isEmpty() && m_factories.contains(contentType)) {
        if (QWidget* w = m_factories.value(contentType)(id)) {
            c->setContent(w);
        }
    }

    // If a parent container was specified, insert this new container
    // into its body layout at the requested index.  Top-level
    // containers (empty parentId) must be placed into an external
    // layout by the caller — we don't know where the root lives.
    if (!parentId.isEmpty()) {
        ContainerWidget* parent = m_containers.value(parentId).data();
        if (parent) parent->insertChildWidget(index, c);
    }

    wireContainer(c);
    emit containerCreated(id);
    return c;
}

void ContainerManager::destroyContainer(const QString& id)
{
    if (!m_containers.contains(id)) return;

    // Recursively destroy any child containers first.  This ensures
    // floating-window cleanup happens children-before-parent and
    // reparent-on-dock handlers don't touch a half-destroyed tree.
    for (const QString& childId : childrenOf(id)) {
        destroyContainer(childId);
    }

    if (m_floatingWindows.contains(id)) {
        auto* w = m_floatingWindows.take(id);
        if (w) { w->releaseContainer(); w->deleteLater(); }
    }
    QPointer<ContainerWidget> c = m_containers.take(id);
    m_meta.remove(id);
    if (c) c->deleteLater();
    emit containerDestroyed(id);
}

QString ContainerManager::parentOf(const QString& id) const
{
    return m_meta.value(id).parentId;
}

QStringList ContainerManager::childrenOf(const QString& id) const
{
    // Walk the meta map to find every container whose parentId
    // matches.  For small N (< ~50 containers) the linear scan is
    // cheaper than maintaining a reverse index that could drift.
    QStringList out;
    for (auto it = m_meta.constBegin(); it != m_meta.constEnd(); ++it) {
        if (it.value().parentId == id) out.append(it.key());
    }
    return out;
}

void ContainerManager::reparentContainer(const QString& id,
                                         const QString& newParentId,
                                         int index)
{
    ContainerWidget* c = m_containers.value(id).data();
    if (!c) return;
    auto& meta = m_meta[id];

    // Remove from current layout parent (be it a container or an
    // external widget).  Floating containers don't have a layout
    // parent right now — just update the logical parentId.
    if (!c->isFloating()) {
        if (!meta.parentId.isEmpty()) {
            ContainerWidget* oldParent = m_containers.value(meta.parentId).data();
            if (oldParent) oldParent->removeChildWidget(c);
        } else if (c->parentWidget()) {
            if (auto* box = qobject_cast<QBoxLayout*>(c->parentWidget()->layout())) {
                box->removeWidget(c);
            }
            c->setParent(nullptr);
        }

        if (!newParentId.isEmpty()) {
            ContainerWidget* newParent = m_containers.value(newParentId).data();
            if (newParent) newParent->insertChildWidget(index, c);
        }
        // If newParentId is empty, caller is responsible for
        // placing the container into an external layout.
    }

    meta.parentId = newParentId;
}

ContainerWidget* ContainerManager::container(const QString& id) const
{
    return m_containers.value(id).data();
}

QList<ContainerWidget*> ContainerManager::allContainers() const
{
    QList<ContainerWidget*> out;
    out.reserve(m_containers.size());
    for (const auto& p : m_containers) {
        if (p) out.append(p.data());
    }
    return out;
}

int ContainerManager::containerCount() const
{
    return m_containers.size();
}

void ContainerManager::floatContainer(const QString& id)
{
    ContainerWidget* c = m_containers.value(id).data();
    if (!c || c->isFloating()) return;

    // Remember the container's current slot so re-dock restores it.
    // Two cases: nested (parentId points at another container) and
    // top-level (no parent container, sitting in an external layout).
    auto& meta = m_meta[id];
    meta.originalParent = nullptr;
    meta.originalIndex = -1;

    if (!meta.parentId.isEmpty()) {
        ContainerWidget* parent = m_containers.value(meta.parentId).data();
        if (parent) {
            meta.originalIndex = parent->indexOfChildWidget(c);
            parent->removeChildWidget(c);
        }
    } else if (c->parentWidget()) {
        meta.originalParent = c->parentWidget();
        if (auto* box = qobject_cast<QBoxLayout*>(meta.originalParent->layout())) {
            meta.originalIndex = box->indexOf(c);
            box->removeWidget(c);
        }
        c->setParent(nullptr);
    }

    // Parent the popped-out window to the main application window so it
    // stays on top of MainWindow without becoming WindowStaysOnTopHint
    // (which would float above all apps, including others when the user
    // alt-tabs away).  Qt::Window inside FloatingContainerWindow keeps
    // it as a separate top-level — the parent only affects z-order and
    // lifetime.
    QWidget* parentWindow = nullptr;
    if (auto* pw = qobject_cast<QWidget*>(parent())) parentWindow = pw->window();
    auto* win = new FloatingContainerWindow(parentWindow);
    win->setGeometryKey(geometryKeyFor(id));
    win->takeContainer(c);
    connect(win, &FloatingContainerWindow::dockRequested,
            this, &ContainerManager::onFloatingWindowDock);

    m_floatingWindows.insert(id, win);
    win->setWindowTitle(c->title());
    win->restoreAndEnsureVisible(meta.originalParent);

    // Apply the persisted always-on-top preference *after*
    // restoreAndEnsureVisible(): toggling Qt::WindowStaysOnTopHint
    // recreates the native window, which would otherwise clobber the
    // geometry we just restored.
    const bool aot = AppSettings::instance()
        .value(alwaysOnTopKeyFor(id), false).toBool();
    if (aot) win->setAlwaysOnTop(true);
    if (auto* tb = c->titleBar()) tb->setAlwaysOnTopState(aot);

    win->show();
    saveState();
}

void ContainerManager::dockContainer(const QString& id)
{
    ContainerWidget* c = m_containers.value(id).data();
    if (!c || !c->isFloating()) return;

    auto* win = m_floatingWindows.take(id);
    if (!win) return;

    ContainerWidget* released = win->releaseContainer();
    win->close();
    win->deleteLater();
    if (!released) return;

    const auto& meta = m_meta.value(id);

    if (!meta.parentId.isEmpty()) {
        // Nested: re-insert into logical parent container's body.
        // If the parent is itself currently floating, re-docking the
        // child just re-inserts it into the parent's body which
        // happens to live in a floating window — Qt reparenting is
        // transparent to that.
        ContainerWidget* parent = m_containers.value(meta.parentId).data();
        if (parent) {
            const int clamped = std::min(std::max(meta.originalIndex, 0),
                                         parent->childWidgetCount());
            parent->insertChildWidget(clamped, released);
        }
    } else if (meta.originalParent) {
        // Top-level: re-insert into the external layout we came from.
        released->setParent(meta.originalParent);
        if (auto* box = qobject_cast<QBoxLayout*>(meta.originalParent->layout())) {
            const int clamped = std::min(std::max(meta.originalIndex, 0),
                                         box->count());
            box->insertWidget(clamped, released);
        }
        released->show();
    }
    saveState();
}

void ContainerManager::setFramelessMode(bool on)
{
    for (auto* win : m_floatingWindows) {
        if (win) win->setFramelessMode(on);
    }
}

void ContainerManager::prepareShutdown()
{
    saveState();  // commit current floating/visibility state before closing windows
    for (auto* win : m_floatingWindows) {
        win->prepareShutdown();
    }
    m_floatingWindows.clear();
}

void ContainerManager::wireContainer(ContainerWidget* c)
{
    connect(c, &ContainerWidget::floatRequested,
            this, &ContainerManager::onFloatRequested);
    connect(c, &ContainerWidget::dockRequested,
            this, &ContainerManager::onDockRequested);
    connect(c, &ContainerWidget::closeRequested,
            this, &ContainerManager::onCloseRequested);
    connect(c, &ContainerWidget::alwaysOnTopToggled,
            this, &ContainerManager::onAlwaysOnTopToggled);
}

void ContainerManager::onFloatRequested()
{
    auto* c = qobject_cast<ContainerWidget*>(sender());
    if (c) floatContainer(c->id());
}

void ContainerManager::onDockRequested()
{
    auto* c = qobject_cast<ContainerWidget*>(sender());
    if (c) dockContainer(c->id());
}

void ContainerManager::onCloseRequested()
{
    auto* c = qobject_cast<ContainerWidget*>(sender());
    if (!c) return;
    c->setContainerVisible(false);
    saveState();
}

void ContainerManager::onAlwaysOnTopToggled(bool on)
{
    auto* c = qobject_cast<ContainerWidget*>(sender());
    if (!c) return;
    const QString id = c->id();
    AppSettings::instance().setValue(alwaysOnTopKeyFor(id), on);
    if (auto* win = m_floatingWindows.value(id, nullptr)) {
        win->setAlwaysOnTop(on);
    }
}

void ContainerManager::onFloatingWindowDock(ContainerWidget* c)
{
    if (c) dockContainer(c->id());
}

void ContainerManager::saveState() const
{
    QJsonObject containers;
    for (auto it = m_containers.constBegin(); it != m_containers.constEnd(); ++it) {
        const QString& id = it.key();
        ContainerWidget* c = it.value().data();
        if (!c) continue;

        QJsonObject entry;
        entry["mode"]        = dockModeToString(c->dockMode());
        entry["visible"]     = c->isContainerVisible();
        entry["contentType"] = m_meta.value(id).contentType;
        entry["parent"]      = m_meta.value(id).parentId;

        // Children in their current body-layout order — used by
        // restoreState to re-insert each child at the correct index.
        if (c->childWidgetCount() > 0) {
            QJsonArray childIds;
            for (int i = 0; i < c->childWidgetCount(); ++i) {
                QWidget* w = c->childWidgetAt(i);
                auto* childContainer = qobject_cast<ContainerWidget*>(w);
                if (childContainer) childIds.append(childContainer->id());
            }
            if (!childIds.isEmpty()) entry["children"] = childIds;
        }

        containers[id] = entry;
    }
    QJsonObject root;
    root["version"]    = kSchemaVersion;
    root["containers"] = containers;

    AppSettings::instance().setValue(
        kSettingsKey,
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void ContainerManager::restoreState()
{
    const QString json = AppSettings::instance()
        .value(kSettingsKey, "").toString();
    if (json.isEmpty()) return;

    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) return;
    const QJsonObject root = doc.object();
    if (root.value("version").toInt() != kSchemaVersion) return;

    const QJsonObject containers = root.value("containers").toObject();

    // First pass: create every container without inserting children.
    // This guarantees parent containers exist before we try to parent
    // a child into them, regardless of the QJsonObject's key order.
    for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
        const QString id = it.key();
        const QJsonObject entry = it.value().toObject();
        const QString contentType = entry.value("contentType").toString();
        const QString parentId    = entry.value("parent").toString();

        ContainerWidget* c = m_containers.value(id).data();
        if (!c) {
            // Don't pass parentId yet — we'll nest in the second pass
            // once every container exists.  This avoids insertion
            // order dependencies.
            c = createContainer(id, id, contentType);
        }
        if (c) {
            // Pre-populate parentId so later lookups work.
            auto& meta = m_meta[id];
            meta.parentId = parentId;
            meta.contentType = contentType;
        }
    }

    // Second pass: nest children into parents in their saved order,
    // then apply visibility + float state.  Parents are handled
    // before their descendants because the QJsonObject iteration is
    // keyed — we walk each container and insert its listed children;
    // missing or stale child IDs are skipped safely.
    for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
        const QString id = it.key();
        const QJsonObject entry = it.value().toObject();
        ContainerWidget* c = m_containers.value(id).data();
        if (!c) continue;

        const QJsonArray childIds = entry.value("children").toArray();
        for (int i = 0; i < childIds.size(); ++i) {
            const QString childId = childIds[i].toString();
            ContainerWidget* child = m_containers.value(childId).data();
            if (!child) continue;
            // Skip children that are currently floating — they live
            // in their own windows and shouldn't be reparented into
            // the body until they dock.
            if (child->isFloating()) continue;
            c->insertChildWidget(i, child);
        }
    }

    // Third pass: apply visibility + floating state.  Floating must
    // come last so the child is already in its parent's body layout,
    // giving floatContainer() a meaningful index to remember.
    for (auto it = containers.constBegin(); it != containers.constEnd(); ++it) {
        const QString id = it.key();
        const QJsonObject entry = it.value().toObject();
        const bool visible = entry.value("visible").toBool(true);
        const auto mode = dockModeFromString(entry.value("mode").toString());
        ContainerWidget* c = m_containers.value(id).data();
        if (!c) continue;

        c->setContainerVisible(visible);
        if (mode == ContainerWidget::DockMode::Floating) {
            floatContainer(id);
        }
    }
}

} // namespace AetherSDR
