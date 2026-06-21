#include "AutomationServer.h"
#include "LogManager.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QPixmap>
#include <QBuffer>
#include <QPoint>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QRegularExpression>

// Best-effort value extraction for common control types.
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QSpinBox>
#include <QProgressBar>

#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#endif

namespace AetherSDR {

namespace {

// Human-meaningful "value" for a control, so an assertion can read state
// without a screenshot. Returns a null QString for widgets that have no
// natural scalar/text value (containers, custom-painted surfaces).
QString widgetValue(const QWidget* w)
{
    if (auto* s = qobject_cast<const QAbstractSlider*>(w))
        return QString::number(s->value());
    if (auto* b = qobject_cast<const QAbstractButton*>(w)) {
        if (b->isCheckable())
            return b->isChecked() ? QStringLiteral("checked")
                                  : QStringLiteral("unchecked");
        return b->text();
    }
    if (auto* cb = qobject_cast<const QComboBox*>(w))
        return cb->currentText();
    if (auto* le = qobject_cast<const QLineEdit*>(w))
        return le->text();
    if (auto* sb = qobject_cast<const QSpinBox*>(w))
        return QString::number(sb->value());
    if (auto* ds = qobject_cast<const QDoubleSpinBox*>(w))
        return QString::number(ds->value());
    if (auto* pb = qobject_cast<const QProgressBar*>(w))
        return QString::number(pb->value());
    if (auto* lb = qobject_cast<const QLabel*>(w))
        return lb->text();
    return QString();  // null -> omitted from snapshot
}

// Short class name without the AetherSDR:: (or any) namespace prefix.
QString shortClassName(const QObject* o)
{
    return QString::fromUtf8(o->metaObject()->className())
        .section(QStringLiteral("::"), -1);
}

QJsonObject describeWidget(const QWidget* w)
{
    QJsonObject o;
    o[QStringLiteral("class")] = QString::fromUtf8(w->metaObject()->className());
    if (!w->objectName().isEmpty())
        o[QStringLiteral("objectName")] = w->objectName();
    if (!w->accessibleName().isEmpty())
        o[QStringLiteral("accessibleName")] = w->accessibleName();
    o[QStringLiteral("enabled")] = w->isEnabled();
    o[QStringLiteral("visible")] = w->isVisible();

    // Geometry in global screen coordinates so a driver can correlate with
    // computer-use / screenshots if it ever needs to.
    const QPoint gp = w->mapToGlobal(QPoint(0, 0));
    QJsonObject geo;
    geo[QStringLiteral("x")] = gp.x();
    geo[QStringLiteral("y")] = gp.y();
    geo[QStringLiteral("w")] = w->width();
    geo[QStringLiteral("h")] = w->height();
    o[QStringLiteral("geometry")] = geo;

    const QString val = widgetValue(w);
    if (!val.isNull())
        o[QStringLiteral("value")] = val;

    QJsonArray kids;
    const QObjectList children = w->children();
    for (const QObject* child : children) {
        if (auto* cw = qobject_cast<const QWidget*>(child))
            kids.append(describeWidget(cw));
    }
    if (!kids.isEmpty())
        o[QStringLiteral("children")] = kids;

    return o;
}

// Depth-first match by class name (full or short) or accessibleName.
QWidget* matchRecursive(QWidget* w, const QString& target)
{
    const QString fullClass = QString::fromUtf8(w->metaObject()->className());
    if (fullClass == target
        || shortClassName(w) == target
        || w->accessibleName() == target) {
        return w;
    }
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            if (QWidget* m = matchRecursive(cw, target))
                return m;
        }
    }
    return nullptr;
}

// Capture a widget to an image. The QRhi panadapter needs a framebuffer
// readback (QWidget::grab() would return empty/garbage for a GPU surface),
// so route QRhiWidget through its own grab().
QImage grabWidget(QWidget* w)
{
#ifdef AETHER_GPU_SPECTRUM
    // QRhiWidget inherits QWidget::grab() (which returns an empty pixmap for a
    // GPU surface); grabFramebuffer() is the real readback and returns a QImage.
    if (auto* rhi = qobject_cast<QRhiWidget*>(w))
        return rhi->grabFramebuffer();
#endif
    return w->grab().toImage();
}

} // namespace

AutomationServer::AutomationServer(QObject* parent)
    : QObject(parent)
{
}

AutomationServer::~AutomationServer()
{
    stop();
}

bool AutomationServer::start(const QString& serverName)
{
    if (m_server)
        return true;

    m_serverName = serverName;
    m_server = new QLocalServer(this);

    // Clear any stale socket left by a crashed run so we can rebind.
    QLocalServer::removeServer(serverName);

    if (!m_server->listen(serverName)) {
        qCWarning(lcAutomation) << "failed to listen on" << serverName << ':'
                                << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QLocalServer::newConnection,
            this, &AutomationServer::onNewConnection);

    // Drop a discovery file so a driver can find the resolved endpoint without
    // knowing the platform-specific socket path. Best-effort; not fatal.
    m_discoveryFile = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                          .filePath(QStringLiteral("aethersdr-automation.json"));
    QJsonObject disc;
    disc[QStringLiteral("socket")]  = fullServerName();
    disc[QStringLiteral("name")]    = serverName;
    disc[QStringLiteral("pid")]     = QCoreApplication::applicationPid();
    disc[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    QFile df(m_discoveryFile);
    if (df.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        df.write(QJsonDocument(disc).toJson(QJsonDocument::Compact));
        df.close();
    } else {
        m_discoveryFile.clear();
    }

    qCInfo(lcAutomation).noquote()
        << "automation bridge listening on" << fullServerName()
        << "(verbs: ping, dumpTree, grab)";
    return true;
}

void AutomationServer::stop()
{
    if (!m_server)
        return;

    for (auto it = m_buffers.constBegin(); it != m_buffers.constEnd(); ++it)
        it.key()->deleteLater();
    m_buffers.clear();

    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;

    if (!m_discoveryFile.isEmpty()) {
        QFile::remove(m_discoveryFile);
        m_discoveryFile.clear();
    }
}

bool AutomationServer::isRunning() const
{
    return m_server && m_server->isListening();
}

QString AutomationServer::fullServerName() const
{
    return m_server ? m_server->fullServerName() : QString();
}

void AutomationServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* sock = m_server->nextPendingConnection();
        m_buffers.insert(sock, QByteArray());
        connect(sock, &QLocalSocket::readyRead,
                this, &AutomationServer::onReadyRead);
        connect(sock, &QLocalSocket::disconnected,
                this, &AutomationServer::onDisconnected);
        qCDebug(lcAutomation) << "client connected;" << m_buffers.size() << "active";
    }
}

void AutomationServer::onReadyRead()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock || !m_buffers.contains(sock))
        return;

    QByteArray& buf = m_buffers[sock];
    buf.append(sock->readAll());

    int nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const QByteArray line = buf.left(nl);
        buf.remove(0, nl + 1);
        if (line.trimmed().isEmpty())
            continue;
        const QJsonObject resp = handleLine(line);
        sock->write(QJsonDocument(resp).toJson(QJsonDocument::Compact));
        sock->write("\n");
        sock->flush();
    }
}

void AutomationServer::onDisconnected()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock)
        return;
    m_buffers.remove(sock);
    sock->deleteLater();
    qCDebug(lcAutomation) << "client disconnected;" << m_buffers.size() << "active";
}

QJsonObject AutomationServer::handleLine(const QByteArray& line)
{
    QString cmd;
    QString target;
    QString path;

    const QByteArray trimmed = line.trimmed();
    if (trimmed.startsWith('{')) {
        // JSON request: {"cmd":"grab","target":"...","path":"..."}
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return QJsonObject{{QStringLiteral("ok"), false},
                               {QStringLiteral("error"),
                                QStringLiteral("invalid JSON: ") + err.errorString()}};
        }
        const QJsonObject obj = doc.object();
        cmd    = obj.value(QStringLiteral("cmd")).toString();
        target = obj.value(QStringLiteral("target")).toString();
        path   = obj.value(QStringLiteral("path")).toString();
    } else {
        // Bare line: "grab <target> [path]"
        const QList<QByteArray> parts = trimmed.split(' ');
        cmd = QString::fromUtf8(parts.value(0));
        if (parts.size() > 1) target = QString::fromUtf8(parts.value(1));
        if (parts.size() > 2) path   = QString::fromUtf8(parts.value(2));
    }

    qCDebug(lcAutomation) << "request:" << cmd << target;

    if (cmd == QLatin1String("ping")) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("app"), QStringLiteral("AetherSDR")},
            {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        };
    }
    if (cmd == QLatin1String("dumpTree")) {
        return doDumpTree();
    }
    if (cmd == QLatin1String("grab")) {
        if (target.isEmpty())
            return QJsonObject{{QStringLiteral("ok"), false},
                               {QStringLiteral("error"),
                                QStringLiteral("grab requires a target widget")}};
        return doGrab(target, path);
    }

    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"),
                        QStringLiteral("unknown command: ") + cmd}};
}

QJsonObject AutomationServer::doDumpTree() const
{
    QJsonArray roots;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops) {
        // Skip the transient/internal helper windows Qt creates so the
        // snapshot stays focused on real UI.
        if (w->objectName() == QLatin1String("qt_scrollarea_viewport"))
            continue;
        roots.append(describeWidget(w));
    }
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("roots"), roots},
    };
}

QJsonObject AutomationServer::doGrab(const QString& target, const QString& path) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("widget not found: ") + target}};
    }

    const QImage img = grabWidget(w);
    if (img.isNull()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("grab produced an empty image for ") + target}};
    }

    QString outPath = path;
    if (outPath.isEmpty()) {
        QString safe = target;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                     QStringLiteral("_"));
        outPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("aether-grab-") + safe + QStringLiteral(".png"));
    }

    if (!img.save(outPath, "PNG")) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("failed to write PNG: ") + outPath}};
    }

    const qint64 bytes = QFileInfo(outPath).size();
    qCInfo(lcAutomation).noquote()
        << "grabbed" << shortClassName(w) << "->" << outPath
        << QStringLiteral("(%1x%2, %3 bytes)").arg(img.width()).arg(img.height()).arg(bytes);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("path"), outPath},
        {QStringLiteral("width"), img.width()},
        {QStringLiteral("height"), img.height()},
        {QStringLiteral("bytes"), bytes},
    };
}

QWidget* AutomationServer::resolveWidget(const QString& target)
{
    const QWidgetList tops = QApplication::topLevelWidgets();

    // 1. Exact objectName (cheap, unambiguous).
    for (QWidget* tlw : tops) {
        if (tlw->objectName() == target)
            return tlw;
        if (QWidget* c = tlw->findChild<QWidget*>(target))
            return c;
    }
    // 2. Class name or accessibleName (e.g. "SpectrumWidget" for the panadapter).
    for (QWidget* tlw : tops) {
        if (QWidget* m = matchRecursive(tlw, target))
            return m;
    }
    return nullptr;
}

} // namespace AetherSDR
