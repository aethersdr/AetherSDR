#include "AutomationServer.h"
#include "LogManager.h"
#include "AppSettings.h"          // StationName (restore the user's real station name)
#include "DigitalVoiceWaveformProcess.h"
#include "DigitalVoiceWaveformSettings.h"
#include "TxKeyingMarker.h"       // kTxKeyingProperty — authoritative TX-guard marker
#include "AudioEngine.h"
#include "NvidiaBnrSettings.h"   // BNR intensity (in-process AFX, #3902)
#include "ClientTxTestTone.h"     // testtone() verb — client-side TX test tone
#include "QsoRecorder.h"          // record() verb — Client-Side QSO recorder
#include "CallsignLookupService.h" // qrz() verb — QRZ lookup cache/service
#include "CallsignUtils.h"
#include "models/Nr2SettingsModel.h"
#include "models/RadioModel.h"   // RadioModel, SliceModel, PanadapterModel (get())
#include "models/AetherClockModel.h"  // AetherClockModel (get clock)
#include "IConnectionAutomation.h" // gui-free connect/disconnect/dialog hook

#include <QAction>
#include <QLocalServer>
#include <QLocalSocket>
#include <QApplication>
#include <QScreen>
#include <QWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QTabBar>
#include <QEnterEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSysInfo>
#include <QPointer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
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
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QTime>

#include <algorithm>
#include <cstring>
#include <limits>

// Best-effort value extraction for common control types.
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QAbstractItemView>   // invoke selectRow: QTableWidget/QTreeWidget/QListWidget row select
#include <QItemSelectionModel>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QSpinBox>
#include <QProgressBar>
#include <QPushButton>   // doShowMenu: QPushButton::menu()
#include <QToolButton>   // doShowMenu: QToolButton::menu()
#include <QWidgetAction>  // describeAction: header rows (disabled QWidgetAction + QLabel)
#include <QContextMenuEvent>  // doContextMenu: synthesize a right-click menu trigger
#include <QHelpEvent>    // doTooltip: synthesize the same tooltip event as a real hover
#ifdef HAVE_WEBSOCKETS
#include <QWebSocket>         // doTci: in-process TCI client simulator (#3305)
#endif

#include <QScrollArea>   // doScrollTo: ensureWidgetVisible on the ancestor
#include <QScrollBar>    // doScrollTo: echo the resulting scrollbar positions

#ifdef AETHER_GPU_SPECTRUM
#include <QRhiWidget>
#endif

namespace AetherSDR {

namespace {

struct ResolvedAction {
    QPointer<QAction> action;
    QPointer<QMenu> menu;
};

// mark→tail correlation (#3756): doMark needs the seq/mono the log tap assigns
// to the MARK message itself, not whatever m_logSeq/back() read afterward — a
// concurrent logging thread can push between the qCInfo() and the re-lock. The
// tap runs synchronously on doMark's thread, so a thread_local sink lets only
// that thread's own tap call publish the marker's identity. Loggers on other
// threads see a null sink and leave it untouched.
struct MarkCapture {
    quint64 seq{0};
    qint64  monoUs{0};
    bool    set{false};
};
thread_local MarkCapture* g_markSink = nullptr;

QString actionDisplayText(const QAction* action)
{
    QString text = action->text();
    const int shortcutStart = text.indexOf(QLatin1Char('\t'));
    if (shortcutStart >= 0) {
        text.truncate(shortcutStart);
    }

    QString stripped;
    stripped.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) == QLatin1Char('&')) {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('&')) {
                stripped.append(QLatin1Char('&'));
                ++i;
            }
            continue;
        }
        stripped.append(text.at(i));
    }
    return stripped.trimmed();
}

QString actionDataText(const QAction* action)
{
    const QVariant data = action->data();
    if (!data.isValid() || data.isNull()) {
        return QString();
    }
    return data.toString();
}

QString actionValue(const QAction* action)
{
    if (action->isCheckable()) {
        return action->isChecked() ? QStringLiteral("checked")
                                   : QStringLiteral("unchecked");
    }
    return actionDisplayText(action);
}

QJsonObject describeAction(const QAction* action, const QMenu* owner)
{
    QJsonObject o;
    o[QStringLiteral("class")] = QStringLiteral("QAction");
    o[QStringLiteral("role")] = QStringLiteral("action");
    o[QStringLiteral("enabled")] = action->isEnabled();
    o[QStringLiteral("visible")] = action->isVisible();

    if (!action->objectName().isEmpty()) {
        o[QStringLiteral("objectName")] = action->objectName();
    }

    // Section/header rows: a disabled QWidgetAction whose default widget is a
    // QLabel is the app's idiom for a menu section title (QMenu::addSection text
    // doesn't render under the app styling, so the S-meter context menu uses a
    // QLabel instead). actionDisplayText() is empty for a QWidgetAction, so such
    // rows would otherwise serialize blank. Read the label text and tag the row
    // as a header so a driver can assert section titles instead of empty rows
    // (#3858).
    QString headerText;
    if (auto* wa = qobject_cast<const QWidgetAction*>(action)) {
        if (auto* lbl = qobject_cast<const QLabel*>(wa->defaultWidget()))
            headerText = lbl->text();
    }

    const QString text = headerText.isEmpty() ? actionDisplayText(action) : headerText;
    if (!text.isEmpty()) {
        o[QStringLiteral("text")] = text;
        o[QStringLiteral("accessibleName")] = text;
    }
    if (!headerText.isEmpty()) {
        o[QStringLiteral("role")] = QStringLiteral("header");
        o[QStringLiteral("type")] = QStringLiteral("header");
    }

    const QString val = actionValue(action);
    if (!val.isNull()) {
        o[QStringLiteral("value")] = val;
    }

    if (action->isSeparator()) {
        o[QStringLiteral("separator")] = true;
    }
    if (action->isCheckable()) {
        o[QStringLiteral("checkable")] = true;
        o[QStringLiteral("checked")] = action->isChecked();
    }
    if (action->menu()) {
        o[QStringLiteral("hasMenu")] = true;
    }
    if (!action->toolTip().isEmpty()) {
        o[QStringLiteral("toolTip")] = action->toolTip();
    }
    if (!action->statusTip().isEmpty()) {
        o[QStringLiteral("statusTip")] = action->statusTip();
    }

    const QString data = actionDataText(action);
    if (!data.isEmpty()) {
        o[QStringLiteral("data")] = data;
    }

    if (owner && owner->isVisible()) {
        const QRect r = owner->actionGeometry(const_cast<QAction*>(action));
        if (r.isValid() && !r.isEmpty()) {
            const QPoint gp = owner->mapToGlobal(r.topLeft());
            QJsonObject geo;
            geo[QStringLiteral("x")] = gp.x();
            geo[QStringLiteral("y")] = gp.y();
            geo[QStringLiteral("w")] = r.width();
            geo[QStringLiteral("h")] = r.height();
            o[QStringLiteral("geometry")] = geo;
        }
    }

    return o;
}

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
    if (auto* le = qobject_cast<const QLineEdit*>(w)) {
        // Never serialize masked fields (password / PIN / keychain secrets):
        // dumpTree is written to a temp tree.json, so returning the cleartext
        // would exfiltrate credentials. Reporting a placeholder keeps the field
        // assertable (present / non-empty) without leaking the value. (#3646)
        if (le->echoMode() != QLineEdit::Normal)
            return le->text().isEmpty() ? QString() : QStringLiteral("<hidden>");
        return le->text();
    }
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

// Human-readable name for a Qt::CursorShape.  Lets a driver assert hover
// affordance (a clickable field carries PointingHandCursor, a text field an
// IBeam) without observing the live OS cursor, which no widget grab captures.
const char* cursorShapeName(Qt::CursorShape shape)
{
    switch (shape) {
        case Qt::ArrowCursor:        return "arrow";
        case Qt::UpArrowCursor:      return "uparrow";
        case Qt::CrossCursor:        return "cross";
        case Qt::WaitCursor:         return "wait";
        case Qt::IBeamCursor:        return "ibeam";
        case Qt::SizeVerCursor:      return "sizever";
        case Qt::SizeHorCursor:      return "sizehor";
        case Qt::SizeBDiagCursor:    return "sizebdiag";
        case Qt::SizeFDiagCursor:    return "sizefdiag";
        case Qt::SizeAllCursor:      return "sizeall";
        case Qt::BlankCursor:        return "blank";
        case Qt::SplitVCursor:       return "splitv";
        case Qt::SplitHCursor:       return "splith";
        case Qt::PointingHandCursor: return "pointinghand";
        case Qt::ForbiddenCursor:    return "forbidden";
        case Qt::OpenHandCursor:     return "openhand";
        case Qt::ClosedHandCursor:   return "closedhand";
        case Qt::WhatsThisCursor:    return "whatsthis";
        case Qt::BusyCursor:         return "busy";
        case Qt::DragMoveCursor:     return "dragmove";
        case Qt::DragCopyCursor:     return "dragcopy";
        case Qt::DragLinkCursor:     return "draglink";
        default:                     return "other";
    }
}

QJsonObject describeWidget(const QWidget* w)
{
    QJsonObject o;
    o[QStringLiteral("class")] = QString::fromUtf8(w->metaObject()->className());
    if (!w->objectName().isEmpty())
        o[QStringLiteral("objectName")] = w->objectName();
    if (!w->accessibleName().isEmpty())
        o[QStringLiteral("accessibleName")] = w->accessibleName();
    if (!w->accessibleDescription().isEmpty()) {
        o[QStringLiteral("accessibleDescription")] = w->accessibleDescription();
    }
    // Tooltip text — some hints (e.g. the two distinct "Clear" tooltips) carry
    // the only human-meaningful distinction between otherwise-identical controls,
    // so an assertion needs them observable, not just visible on hover (#3646).
    if (!w->toolTip().isEmpty())
        o[QStringLiteral("toolTip")] = w->toolTip();
    o[QStringLiteral("enabled")] = w->isEnabled();
    o[QStringLiteral("visible")] = w->isVisible();

    // Explicitly-set mouse cursor shape — only reported when this widget owns a
    // cursor (WA_SetCursor), so a driver can prove hover affordance (clickable
    // flag fields carry "pointinghand") without observing the live OS cursor,
    // which no screenshot/grab captures (#4036).
    if (w->testAttribute(Qt::WA_SetCursor)) {
        o[QStringLiteral("cursor")] = QLatin1String(cursorShapeName(w->cursor().shape()));
    }

    // Geometry in global screen coordinates so a driver can correlate with
    // computer-use / screenshots if it ever needs to.
    const QPoint gp = w->mapToGlobal(QPoint(0, 0));
    QJsonObject geo;
    geo[QStringLiteral("x")] = gp.x();
    geo[QStringLiteral("y")] = gp.y();
    geo[QStringLiteral("w")] = w->width();
    geo[QStringLiteral("h")] = w->height();
    o[QStringLiteral("geometry")] = geo;

    // Window state for top-level windows — lets a driver assert a maximize /
    // restore / minimize without screenshotting, and prove the `window` verb
    // (resize only ever set explicit geometry, so an un-maximize was previously
    // unverifiable). (#3918)
    if (w->isWindow()) {
        const Qt::WindowStates st = w->windowState();
        const char* ws = "normal";
        if (st & Qt::WindowMinimized)       ws = "minimized";
        else if (st & Qt::WindowFullScreen) ws = "fullscreen";
        else if (st & Qt::WindowMaximized)  ws = "maximized";
        o[QStringLiteral("windowState")] = QLatin1String(ws);
    }

    const QString val = widgetValue(w);
    if (!val.isNull())
        o[QStringLiteral("value")] = val;

    // A checkable button reports its value as "checked"/"unchecked", which hides
    // the label that says *which* control it is (the six DSP method buttons —
    // NR2 … BNR — were indistinguishable without a screenshot). Surface the
    // button text and a boolean check-state so a driver can read both the
    // identity and the on/off state from dumpTree alone (#3856).
    if (auto* b = qobject_cast<const QAbstractButton*>(w); b && b->isCheckable()) {
        if (!b->text().isEmpty())
            o[QStringLiteral("text")] = b->text();
        o[QStringLiteral("checked")] = b->isChecked();
    }

    // Range for numeric controls — lets a driver validate against the real
    // bounds (scale) and detect wrapping/circular sliders without guessing
    // extremes (#3646).
    if (auto* s = qobject_cast<const QAbstractSlider*>(w)) {
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), s->minimum()},
                                                 {QStringLiteral("max"), s->maximum()}};
        o[QStringLiteral("sliderDown")] = s->isSliderDown();
    } else if (auto* sb = qobject_cast<const QSpinBox*>(w)) {
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), sb->minimum()},
                                                 {QStringLiteral("max"), sb->maximum()}};
    } else if (auto* ds = qobject_cast<const QDoubleSpinBox*>(w)) {
        o[QStringLiteral("range")] = QJsonObject{{QStringLiteral("min"), ds->minimum()},
                                                 {QStringLiteral("max"), ds->maximum()}};
    }

    // Full option list for a combo box, so a driver can verify the available
    // choices non-destructively — `value` reports only the active text, which
    // can't prove the rest of the set without stepping (and applying) each one
    // (#3646).
    if (auto* cb = qobject_cast<const QComboBox*>(w)) {
        QJsonArray items;
        for (int i = 0; i < cb->count(); ++i)
            items.append(cb->itemText(i));
        o[QStringLiteral("items")] = items;
        o[QStringLiteral("currentIndex")] = cb->currentIndex();
    }

    // Surface the TX-keying marker so an agent can see which controls invoke()
    // will refuse before trying them (#3646).
    if (w->property(kTxKeyingProperty).toBool())
        o[QStringLiteral("keying")] = true;
    {
        const QVariant sliceId = w->property("sliceId");
        if (sliceId.isValid()) {
            o[QStringLiteral("sliceId")] = sliceId.toInt();
        }
    }
    {
        const QVariant centerLockSliceId = w->property("centerLockSliceId");
        if (centerLockSliceId.isValid()) {
            o[QStringLiteral("centerLockSliceId")] = centerLockSliceId.toInt();
            o[QStringLiteral("centerMhz")] = w->property("centerMhz").toDouble();
            o[QStringLiteral("bandwidthMhz")] = w->property("bandwidthMhz").toDouble();
        }
    }

    // Surface the spectrum's measured FFT noise floor (dBm) when a widget
    // exposes it as a Q_PROPERTY (SpectrumWidget). Read generically via the
    // meta-object so the core bridge stays decoupled from the GUI class. Lets
    // a driver sample post-TX floor recovery numerically (#3804). The sentinel
    // -1000 means "no measurement yet"; emit only a real reading.
    {
        const QVariant nf = w->property("noiseFloorDbm");
        if (nf.isValid() && nf.toDouble() > -500.0) {
            o[QStringLiteral("noiseFloorDbm")] = nf.toDouble();
            const QVariant df = w->property("displayFloorDbm");
            if (df.isValid() && df.toDouble() > -500.0)
                o[QStringLiteral("displayFloorDbm")] = df.toDouble();
            const QVariant pi = w->property("panIndex");
            if (pi.isValid())
                o[QStringLiteral("panIndex")] = pi.toInt();
        }
    }

    // Surface an HGauge's label/value/scale when the widget publishes them as
    // dynamic properties (custom-painted, no Q_OBJECT, so read generically via
    // the meta-object — same decoupled pattern as noiseFloorDbm above). Lets a
    // driver assert the MtrApplet °C/°F range switch and the live numeric
    // overlays (PA Temp, fan RPM) numerically, not by pixel-reading (#3886).
    {
        const QVariant gl = w->property("gaugeLabel");
        if (gl.isValid()) {
            o[QStringLiteral("gaugeLabel")] = gl.toString();
            o[QStringLiteral("gaugeValue")] = w->property("gaugeValue").toDouble();
            QJsonObject range;
            range[QStringLiteral("min")] = w->property("gaugeMin").toDouble();
            range[QStringLiteral("max")] = w->property("gaugeMax").toDouble();
            range[QStringLiteral("redStart")] = w->property("gaugeRedStart").toDouble();
            range[QStringLiteral("yellowStart")] = w->property("gaugeYellowStart").toDouble();
            o[QStringLiteral("gaugeRange")] = range;
            o[QStringLiteral("gaugeTicks")] = w->property("gaugeTicks").toString();
        }
    }

    // The custom-painted analog meters publish their live mechanics as dynamic
    // properties. Surface them generically so bridge validation can prove the
    // standard meter's native SWR filtering and the PWR applet's two calibrated
    // movements without coupling the core automation server to gui/ headers.
    {
        const QVariant txSwrSource = w->property("txSwrSource");
        if (txSwrSource.isValid()) {
            o[QStringLiteral("txSwrSource")] = txSwrSource.toString();
            o[QStringLiteral("txSwr")] = w->property("txSwr").toDouble();
            o[QStringLiteral("txSwrRaw")] = w->property("txSwrRaw").toDouble();
            o[QStringLiteral("txSwrForwardWatts")] =
                w->property("txSwrForwardWatts").toDouble();
            o[QStringLiteral("txSwrPowerEnvelopeWatts")] =
                w->property("txSwrPowerEnvelopeWatts").toDouble();
            o[QStringLiteral("txSwrMinimumForwardWatts")] =
                w->property("txSwrMinimumForwardWatts").toDouble();
            o[QStringLiteral("txSwrHeld")] = w->property("txSwrHeld").toBool();
            o[QStringLiteral("txMode")] = w->property("txMode").toString();
            o[QStringLiteral("transmitting")] =
                w->property("transmitting").toBool();
        }
        const QVariant meterStyle = w->property("meterStyle");
        if (meterStyle.isValid()) {
            o[QStringLiteral("meterStyle")] = meterStyle.toString();
        }
        const QVariant faceTheme = w->property("faceTheme");
        if (faceTheme.isValid()) {
            o[QStringLiteral("faceTheme")] = faceTheme.toString();
        }
        const QVariant designVersion = w->property("geometryDesignVersion");
        if (designVersion.isValid()) {
            o[QStringLiteral("geometryDesignVersion")] = designVersion.toInt();
            o[QStringLiteral("forwardWatts")] =
                w->property("forwardWatts").toDouble();
            o[QStringLiteral("reflectedWatts")] =
                w->property("reflectedWatts").toDouble();
            o[QStringLiteral("reflectedPowerSource")] =
                w->property("reflectedPowerSource").toString();
            o[QStringLiteral("swr")] = w->property("swr").toDouble();
            o[QStringLiteral("rangeMultiplier")] =
                w->property("rangeMultiplier").toDouble();
            o[QStringLiteral("rangeLegendVisible")] =
                w->property("rangeLegendVisible").toBool();
            o[QStringLiteral("transmitting")] =
                w->property("transmitting").toBool();
            o[QStringLiteral("effectiveActive")] =
                w->property("effectiveActive").toBool();
            o[QStringLiteral("automationFixture")] =
                w->property("automationFixture").toBool();
            o[QStringLiteral("forwardAngleRadians")] =
                w->property("forwardAngleRadians").toDouble();
            o[QStringLiteral("reflectedAngleRadians")] =
                w->property("reflectedAngleRadians").toDouble();
            o[QStringLiteral("intersectionX")] =
                w->property("intersectionX").toDouble();
            o[QStringLiteral("intersectionY")] =
                w->property("intersectionY").toDouble();
            o[QStringLiteral("nearestSwrGuide")] =
                w->property("nearestSwrGuide").toString();
            o[QStringLiteral("nearestGuideDistancePx")] =
                w->property("nearestGuideDistancePx").toDouble();
            o[QStringLiteral("displayedForwardWatts")] =
                w->property("displayedForwardWatts").toDouble();
            o[QStringLiteral("displayedReflectedWatts")] =
                w->property("displayedReflectedWatts").toDouble();
            o[QStringLiteral("displayedForwardAngleRadians")] =
                w->property("displayedForwardAngleRadians").toDouble();
            o[QStringLiteral("displayedReflectedAngleRadians")] =
                w->property("displayedReflectedAngleRadians").toDouble();
            o[QStringLiteral("needleAnimationActive")] =
                w->property("needleAnimationActive").toBool();
        }
    }

    QJsonArray kids;
    const QObjectList children = w->children();
    for (const QObject* child : children) {
        if (auto* cw = qobject_cast<const QWidget*>(child))
            kids.append(describeWidget(cw));
    }

    if (auto* menu = qobject_cast<const QMenu*>(w)) {
        QJsonArray actions;
        for (const QAction* action : menu->actions()) {
            const QJsonObject actionNode = describeAction(action, menu);
            actions.append(actionNode);
            kids.append(actionNode);
        }
        if (!actions.isEmpty()) {
            o[QStringLiteral("actions")] = actions;
        }
    }

    if (!kids.isEmpty())
        o[QStringLiteral("children")] = kids;

    return o;
}

QJsonObject describeHitWidget(const QWidget* w)
{
    if (!w) {
        return QJsonObject{};
    }

    QJsonObject o;
    o[QStringLiteral("class")] = shortClassName(w);
    o[QStringLiteral("fullClass")] =
        QString::fromUtf8(w->metaObject()->className());
    if (!w->objectName().isEmpty()) {
        o[QStringLiteral("objectName")] = w->objectName();
    }
    if (!w->accessibleName().isEmpty()) {
        o[QStringLiteral("accessibleName")] = w->accessibleName();
    }
    if (!w->accessibleDescription().isEmpty()) {
        o[QStringLiteral("accessibleDescription")] = w->accessibleDescription();
    }
    o[QStringLiteral("visible")] = w->isVisible();
    o[QStringLiteral("enabled")] = w->isEnabled();

    const QPoint gp = w->mapToGlobal(QPoint(0, 0));
    o[QStringLiteral("geometry")] = QJsonObject{
        {QStringLiteral("x"), gp.x()},
        {QStringLiteral("y"), gp.y()},
        {QStringLiteral("w"), w->width()},
        {QStringLiteral("h"), w->height()},
    };

    QJsonArray ancestors;
    const QWidget* p = w;
    while (p) {
        QJsonObject a;
        a[QStringLiteral("class")] = shortClassName(p);
        if (!p->objectName().isEmpty()) {
            a[QStringLiteral("objectName")] = p->objectName();
        }
        if (!p->accessibleName().isEmpty()) {
            a[QStringLiteral("accessibleName")] = p->accessibleName();
        }
        if (!p->accessibleDescription().isEmpty()) {
            a[QStringLiteral("accessibleDescription")] = p->accessibleDescription();
        }
        ancestors.append(a);
        p = p->parentWidget();
    }
    o[QStringLiteral("ancestors")] = ancestors;
    return o;
}

void collectIdentityMatches(QWidget* w, const QString& target,
                            QList<QWidget*>& out)
{
    const QString fullClass = QString::fromUtf8(w->metaObject()->className());
    if (fullClass == target
        || shortClassName(w) == target
        || w->accessibleName() == target) {
        out.append(w);
    }
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            collectIdentityMatches(cw, target, out);
        }
    }
}

void collectObjectNameMatches(QWidget* w, const QString& target,
                              QList<QWidget*>& out)
{
    if (w->objectName() == target) {
        out.append(w);
    }
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            collectObjectNameMatches(cw, target, out);
        }
    }
}

// Last-resort match by a button's visible text — agents often know a control
// only by its label ("Send", "Transmit"). Lowest priority so an objectName /
// accessibleName / class always wins first.
void collectButtonTextMatches(QWidget* w, const QString& target,
                              QList<QWidget*>& out)
{
    if (auto* b = qobject_cast<QAbstractButton*>(w)) {
        if (b->text() == target) {
            out.append(w);
        }
    }
    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            collectButtonTextMatches(cw, target, out);
        }
    }
}

int widgetResolutionRank(const QWidget* w)
{
    const bool visible = w->isVisible();
    const bool enabled = w->isEnabled();
    if (visible && enabled) {
        return 0;
    }
    if (visible) {
        return 1;
    }
    if (enabled) {
        return 2;
    }
    return 3;
}

QList<QWidget*> preferredWidgetOrder(QList<QWidget*> widgets)
{
    std::stable_sort(widgets.begin(), widgets.end(),
                     [](const QWidget* lhs, const QWidget* rhs) {
                         return widgetResolutionRank(lhs)
                             < widgetResolutionRank(rhs);
                     });
    return widgets;
}

QWidget* preferredWidget(const QList<QWidget*>& widgets)
{
    if (widgets.isEmpty()) {
        return nullptr;
    }

    const auto it = std::min_element(
        widgets.begin(), widgets.end(),
        [](const QWidget* lhs, const QWidget* rhs) {
            return widgetResolutionRank(lhs) < widgetResolutionRank(rhs);
        });
    return it == widgets.end() ? nullptr : *it;
}

QWidget* resolveWithinScopes(const QList<QWidget*>& scopes,
                             const QString& target)
{
    const QList<QWidget*> orderedScopes = preferredWidgetOrder(scopes);
    QList<QWidget*> matches;
    for (QWidget* scope : orderedScopes) {
        collectObjectNameMatches(scope, target, matches);
    }
    if (QWidget* m = preferredWidget(matches)) {
        return m;
    }

    matches.clear();
    for (QWidget* scope : orderedScopes) {
        collectIdentityMatches(scope, target, matches);
    }
    if (QWidget* m = preferredWidget(matches)) {
        return m;
    }

    matches.clear();
    for (QWidget* scope : orderedScopes) {
        collectButtonTextMatches(scope, target, matches);
    }
    return preferredWidget(matches);
}

// Collect every widget whose short class name matches `cls`, anywhere under
// `w`. Used to enumerate all SpectrumWidgets (one per pan) so `grab pan <index>`
// can pick a specific surface instead of the first match. (#3646)
void collectByClass(QWidget* w, const QString& cls, QList<QWidget*>& out)
{
    if (shortClassName(w) == cls)
        out.append(w);
    const QObjectList children = w->children();
    for (QObject* child : children)
        if (auto* cw = qobject_cast<QWidget*>(child))
            collectByClass(cw, cls, out);
}

QList<QWidget*> findWidgetsByClass(const QString& cls)
{
    QList<QWidget*> out;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* tlw : tops)
        collectByClass(tlw, cls, out);
    return out;
}

// Map a UI pan index (SpectrumWidget::panIndex) to the radio stream panId by
// ascending from the matching SpectrumWidget to its PanadapterApplet, which
// carries the panId. Both are read via the meta-object so core/ needs no GUI
// header. Empty if no such pan. (#3646 — `pan close <index>`)
QString panIdForIndex(int index)
{
    const QList<QWidget*> spectra = findWidgetsByClass(QStringLiteral("SpectrumWidget"));
    for (QWidget* sw : spectra) {
        const QVariant pi = sw->property("panIndex");
        if (!pi.isValid() || pi.toInt() != index)
            continue;
        for (QWidget* a = sw; a; a = a->parentWidget()) {
            if (shortClassName(a) == QLatin1String("PanadapterApplet")) {
                const QVariant pid = a->property("panId");
                if (pid.isValid())
                    return pid.toString();
            }
        }
    }
    return QString();
}

QWidget* panSpectrumWidgetForIndex(int index, QJsonArray* available = nullptr)
{
    const QList<QWidget*> spectra = findWidgetsByClass(QStringLiteral("SpectrumWidget"));
    QWidget* match = nullptr;
    for (QWidget* sw : spectra) {
        const QVariant pi = sw->property("panIndex");
        if (!pi.isValid()) {
            continue;
        }
        if (available) {
            available->append(pi.toInt());
        }
        // Prefer a visible surface if duplicate indices ever coexist (e.g. mid
        // float/dock reparent); otherwise the first index match wins below.
        if (pi.toInt() == index && (!match || sw->isVisible())) {
            match = sw;
        }
    }
    return match;
}

QWidget* panadapterAppletForSpectrum(QWidget* spectrum)
{
    for (QWidget* a = spectrum; a; a = a->parentWidget()) {
        if (shortClassName(a) == QLatin1String("PanadapterApplet")) {
            return a;
        }
    }
    return nullptr;
}

QWidget* spectrumForVfoWidget(QWidget* vfo)
{
    for (QWidget* a = vfo; a; a = a->parentWidget()) {
        if (shortClassName(a) == QLatin1String("SpectrumWidget")) {
            return a;
        }
    }
    return nullptr;
}

QString panIdForSpectrumWidget(QWidget* spectrum)
{
    if (QWidget* applet = panadapterAppletForSpectrum(spectrum)) {
        const QVariant pid = applet->property("panId");
        if (pid.isValid()) {
            return pid.toString();
        }
    }
    return QString();
}

QJsonObject widgetGeometryJson(const QWidget* widget)
{
    if (!widget) {
        return QJsonObject{};
    }
    const QPoint global = widget->mapToGlobal(QPoint(0, 0));
    return QJsonObject{
        {QStringLiteral("x"), global.x()},
        {QStringLiteral("y"), global.y()},
        {QStringLiteral("w"), widget->width()},
        {QStringLiteral("h"), widget->height()},
    };
}

QWidget* vfoWidgetForPanIndex(int index)
{
    const QList<QWidget*> vfos = findWidgetsByClass(QStringLiteral("VfoWidget"));
    QWidget* fallback = nullptr;
    for (QWidget* vfo : vfos) {
        for (QWidget* a = vfo; a; a = a->parentWidget()) {
            if (shortClassName(a) != QLatin1String("SpectrumWidget")) {
                continue;
            }
            const QVariant pi = a->property("panIndex");
            if (pi.isValid() && pi.toInt() == index) {
                if (vfo->isVisible()) {
                    return vfo;
                }
                if (!fallback) {
                    fallback = vfo;
                }
            }
            break;
        }
    }
    return fallback;
}

QWidget* vfoWidgetForSliceId(int sliceId)
{
    const QList<QWidget*> vfos = findWidgetsByClass(QStringLiteral("VfoWidget"));
    QWidget* fallback = nullptr;
    for (QWidget* vfo : vfos) {
        const QVariant sid = vfo->property("sliceId");
        if (!sid.isValid() || sid.toInt() != sliceId) {
            continue;
        }
        if (vfo->isVisible()) {
            return vfo;
        }
        if (!fallback) {
            fallback = vfo;
        }
    }
    return fallback;
}

QWidget* resolveVfoSelector(const QString& target)
{
    static const QRegularExpression sliceRe(
        QStringLiteral("^vfo(?:\\s+|:)slice(?:\\s+|:)(\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch sliceMatch = sliceRe.match(target.trimmed());
    if (sliceMatch.hasMatch()) {
        bool ok = false;
        const int sliceId = sliceMatch.captured(1).toInt(&ok);
        return ok ? vfoWidgetForSliceId(sliceId) : nullptr;
    }

    static const QRegularExpression re(
        QStringLiteral("^vfo(?:\\s+|:)(\\d+)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(target.trimmed());
    if (!m.hasMatch()) {
        return nullptr;
    }
    bool ok = false;
    const int index = m.captured(1).toInt(&ok);
    if (!ok) {
        return nullptr;
    }
    return vfoWidgetForPanIndex(index);
}

bool actionMatchesTarget(const QAction* action, const QString& target)
{
    return action->objectName() == target
        || action->text() == target
        || actionDisplayText(action) == target
        || action->toolTip() == target
        || action->statusTip() == target
        || actionDataText(action) == target;
}

ResolvedAction matchMenuAction(QMenu* menu, const QString& target)
{
    for (QAction* action : menu->actions()) {
        if (!action->isVisible()) {
            continue;
        }
        if (actionMatchesTarget(action, target)) {
            return {action, menu};
        }
        if (QMenu* submenu = action->menu(); submenu && submenu->isVisible()) {
            const ResolvedAction match = matchMenuAction(submenu, target);
            if (match.action) {
                return match;
            }
        }
    }
    return {};
}

ResolvedAction matchActionRecursive(QWidget* w, const QString& target)
{
    if (auto* menu = qobject_cast<QMenu*>(w)) {
        if (menu->isVisible()) {
            const ResolvedAction match = matchMenuAction(menu, target);
            if (match.action) {
                return match;
            }
        }
    }

    const QObjectList children = w->children();
    for (QObject* child : children) {
        if (auto* cw = qobject_cast<QWidget*>(child)) {
            const ResolvedAction match = matchActionRecursive(cw, target);
            if (match.action) {
                return match;
            }
        }
    }
    return {};
}

ResolvedAction resolveVisibleAction(const QString& target)
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* tlw : tops) {
        const ResolvedAction match = matchActionRecursive(tlw, target);
        if (match.action) {
            return match;
        }
    }
    return {};
}

// Walk a QMenu's actions WITHOUT the isVisible() gate, descending into
// submenus, so a menu-bar leaf action is reachable even while its menu is
// closed. The owning QMenu is returned so triggerMenuAction() can route through
// the closed-menu (action->trigger()) path. (#3646 fidelity — items 5 + 7)
ResolvedAction matchMenuActionUnconditional(QMenu* menu, const QString& target)
{
    for (QAction* action : menu->actions()) {
        if (action->isSeparator())
            continue;
        if (actionMatchesTarget(action, target))
            return {action, menu};
        if (QMenu* submenu = action->menu()) {
            const ResolvedAction match = matchMenuActionUnconditional(submenu, target);
            if (match.action)
                return match;
        }
    }
    return {};
}

// Resolve a QAction anywhere in any top-level window's menu bar, regardless of
// whether the menu is currently open. This is what lets `invoke` drive
// menu-launched dialogs (AetherControl…/Network…/MQTT…/Radio Setup…/Connect…)
// and View→UI Scale→Zoom without the user first popping the menu.
ResolvedAction resolveMenuBarAction(const QString& target)
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    // 1. Any QMainWindow's menu bar (Windows/Linux, and macOS when the actions
    //    remain on the bar). Searched unconditionally so a CLOSED menu resolves.
    for (QWidget* tlw : tops) {
        auto* mw = qobject_cast<QMainWindow*>(tlw);
        if (!mw)
            continue;
        QMenuBar* mb = mw->menuBar();
        if (!mb)
            continue;
        for (QAction* topAction : mb->actions()) {
            // A top-level menu title (e.g. "Settings") matches but has no owning
            // QMenu to route through; its submenu is what carries the leaves.
            if (actionMatchesTarget(topAction, target))
                return {topAction, nullptr};
            if (QMenu* submenu = topAction->menu()) {
                const ResolvedAction match = matchMenuActionUnconditional(submenu, target);
                if (match.action)
                    return match;
            }
        }
    }
    // 2. macOS NATIVE menu bar: Qt reparents each top menu to a TOP-LEVEL QMenu
    //    that is NOT under a queryable QMenuBar, so the menu-bar walk above finds
    //    nothing. Walk every top-level QMenu unconditionally to reach those. A
    //    transient context-menu/combo-popup QMenu won't carry our dialog/zoom
    //    labels, so the exact-text match stays unambiguous.
    for (QWidget* tlw : tops) {
        auto* menu = qobject_cast<QMenu*>(tlw);
        if (!menu)
            continue;
        const ResolvedAction match = matchMenuActionUnconditional(menu, target);
        if (match.action)
            return match;
    }
    return {};
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

QJsonObject err(const QString& msg)
{
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), msg}};
}

QJsonObject deferredResponse()
{
    return QJsonObject{{QStringLiteral("_deferred"), true}};
}

bool isDeferredResponse(const QJsonObject& response)
{
    return response.value(QStringLiteral("_deferred")).toBool(false);
}

bool writeJsonResponse(QLocalSocket* socket, const QJsonObject& response)
{
    if (!socket || socket->state() == QLocalSocket::UnconnectedState) {
        return false;
    }

    QByteArray payload = QJsonDocument(response).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (socket->write(payload) < 0) {
        qCWarning(lcAutomation) << "failed to write automation response:"
                                << socket->errorString();
        return false;
    }
    socket->flush();
    return true;
}

QJsonObject connectionRadioToJson(const RadioInfo& radio)
{
    QJsonObject o{
        {QStringLiteral("name"), radio.name},
        {QStringLiteral("model"), radio.model},
        {QStringLiteral("serial"), radio.serial},
        {QStringLiteral("version"), radio.version},
        {QStringLiteral("nickname"), radio.nickname},
        {QStringLiteral("callsign"), radio.callsign},
        {QStringLiteral("address"), radio.address.toString()},
        {QStringLiteral("port"), radio.port},
        {QStringLiteral("status"), radio.status},
        {QStringLiteral("inUse"), radio.inUse},
        {QStringLiteral("multiFlexEnabled"), radio.multiFlexEnabled},
        {QStringLiteral("routed"), radio.isRouted},
    };

    QJsonArray stations;
    for (const QString& station : radio.guiClientStations) {
        stations.append(station);
    }
    if (!stations.isEmpty()) {
        o[QStringLiteral("stations")] = stations;
    }

    QJsonArray handles;
    for (const QString& handle : radio.guiClientHandles) {
        handles.append(handle);
    }
    if (!handles.isEmpty()) {
        o[QStringLiteral("clientHandles")] = handles;
    }

    return o;
}

QJsonArray connectionRadioListToJson(const QList<RadioInfo>& radios)
{
    QJsonArray array;
    for (const RadioInfo& radio : radios) {
        array.append(connectionRadioToJson(radio));
    }
    return array;
}

// Parse a textual boolean from an invoke value: 1/true/on/yes/checked → true.
bool parseBool(const QString& v)
{
    const QString s = v.trimmed().toLower();
    return s == QLatin1String("1") || s == QLatin1String("true")
        || s == QLatin1String("on") || s == QLatin1String("yes")
        || s == QLatin1String("checked");
}

// Tokenize an identifier or label into lowercased words, splitting on
// non-alphanumeric separators AND camelCase humps (tuneButton -> [tune, button],
// aprsSvcWXBOT -> [aprs, svc, wxbot], "Auto-Tune" -> [auto, tune]). The TX-guard
// fallback matches a deny-word against a WHOLE token, so a cross-token trigram
// like "cwx" formed by the c in "svc" + "wx" in "wxbot" no longer false-positives
// as the CWX keyer, while genuine keyers (moxButton, pttSend, "Auto-Tune") still
// match. This is the anchored replacement for the old bare contains() blocklist
// that flagged the RX-only APRS weather entry (#3646).
QStringList identifierTokens(const QString& s)
{
    QString spaced;
    spaced.reserve(s.size() * 2);
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        // Break at a lower/digit -> Upper hump (tuneButton -> "tune Button") and
        // at an acronym -> word hump (WXBot -> "WX Bot"); runs of caps stay whole
        // (WXBOT -> "wxbot").
        if (i > 0 && c.isUpper()
            && (s.at(i - 1).isLower() || s.at(i - 1).isDigit()
                || (i + 1 < s.size() && s.at(i + 1).isLower())))
            spaced.append(QLatin1Char(' '));
        spaced.append(c);
    }
    return spaced.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                                  Qt::SkipEmptyParts);
}

// True if any haystack contributes a whole token equal to a deny-word — the
// anchored TX-guard fallback match.
bool matchesTxDenyToken(const QStringList& haystacks, const QStringList& deny)
{
    for (const QString& h : haystacks) {
        const QStringList tokens = identifierTokens(h);
        for (const QString& d : deny)
            if (tokens.contains(d))
                return true;
    }
    return false;
}

// TX-safety guard for invoke(): refuse to drive a control that keys the
// transmitter unless the operator sets AETHER_AUTOMATION_ALLOW_TX. A test bridge
// must never key a live radio by accident.
//
// Authoritative mechanism — a positive marker. Genuinely-keying controls
// (MOX/PTT, TUNE, ATU, CWX send, packet send) are tagged at their creation site
// with markTxKeying() (the "aetherTxKeying" dynamic property). The guard honors
// that property, so a control is blocked because it was *declared* keying, not
// because its label happened to contain a magic word. This closed the holes the
// old substring blocklist missed — notably the CW and packet "Send" buttons,
// which key TX but match no keyword (#3646 review).
//
// Belt-and-suspenders fallback — a button-scoped name heuristic, retained only
// to catch a keying control that predates or forgot the marker. It is *button*
// scoped because only a discrete button action can key (setpoint sliders like
// "Tune power"/"RF power" never transmit by being moved). When the fallback
// fires we log a warning: that control should get an explicit markTxKeying().
bool isTransmitControl(const QWidget* w)
{
    if (w->property(kTxKeyingProperty).toBool())
        return true;  // authoritative positive marker

    const auto* btn = qobject_cast<const QAbstractButton*>(w);
    if (!btn)
        return false;  // sliders / combos / spinboxes can't trigger TX

    if (w->objectName().startsWith(QStringLiteral("panOverlayMessageClose_"))) {
        return false;  // closes an overlay notification, never keys TX.
    }

    // Keep the fallback deny-list narrow and aligned with isTransmitAction():
    // only words that unambiguously mean "keys TX". "tune"/"atu"/"vox" were
    // dropped because they false-positive on RX-only controls — the "Tune Now"
    // button (net/spot retune) and "Tune to <spot>" only move the VFO, and a VOX
    // toggle arms TX rather than keying it. The genuine keying TUNE/ATU buttons
    // (TxApplet, AtuPreTuneDialog) all carry the authoritative markTxKeying()
    // marker, which the positive check above already honors, so removing them
    // here loses no real protection — it just stops blocking RX-only buttons
    // that happen to contain "tune". (#3918 — "Tune Now" false-positive)
    static const QStringList kDeny = {
        QStringLiteral("mox"), QStringLiteral("ptt"),
        QStringLiteral("transmit"), QStringLiteral("cwx"),
    };
    const QStringList hay{w->objectName(), w->accessibleName(), btn->text()};
    if (matchesTxDenyToken(hay, kDeny)) {
        qCWarning(lcAutomation).noquote()
            << "TX guard fell back to name match on" << btn->text()
            << "— add markTxKeying() at its creation site if it keys TX";
        return true;
    }
    return false;
}

bool hasOwnTransmitMarker(const QObject* object)
{
    return object && object->property(kTxKeyingProperty).toBool();
}

bool isTransmitAction(const QAction* action, const QMenu* owner)
{
    if (hasOwnTransmitMarker(action) || hasOwnTransmitMarker(owner)) {
        return true;
    }

    static const QStringList kDeny = {
        QStringLiteral("mox"), QStringLiteral("ptt"),
        QStringLiteral("transmit"), QStringLiteral("cwx"),
    };

    // QAction labels such as "Tune to <spot>" and tooltips like "Next Tune
    // press transmits..." describe RX tuning or future behavior, not this
    // action keying TX. Keep the fallback narrow (whole-token match only); real
    // keying actions should be marked explicitly with kTxKeyingProperty.
    const QStringList hay{action->objectName(), actionDisplayText(action)};
    if (matchesTxDenyToken(hay, kDeny)) {
        qCWarning(lcAutomation).noquote()
            << "TX guard fell back to QAction name match on"
            << actionDisplayText(action)
            << "— add an explicit TX marker at the action/menu creation site if it keys TX";
        return true;
    }
    return false;
}

bool triggerMenuAction(QAction* action, QMenu* menu)
{
    if (!action) {
        return false;
    }

    QPointer<QAction> actionGuard = action;
    QPointer<QMenu> menuGuard = menu;
    const QRect r = menu ? menu->actionGeometry(action) : QRect();
    if (menu && menu->isVisible() && r.isValid() && !r.isEmpty()) {
        const QPoint local = r.center();
        const QPoint global = menu->mapToGlobal(local);
        menu->setActiveAction(action);

        QMouseEvent press(QEvent::MouseButtonPress,
                          QPointF(local),
                          QPointF(local),
                          QPointF(global),
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(menu, &press);
        if (!menuGuard || !actionGuard) {
            return true;
        }

        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(local),
                            QPointF(local),
                            QPointF(global),
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(menu, &release);
        return true;
    }

    action->trigger();
    if (menuGuard && menuGuard->isVisible()) {
        menuGuard->close();
    }
    return true;
}

// Best-effort: activate `win` so a native popup menu shown as a SIDE EFFECT of a
// bridge-driven click/trigger has a valid parent QWindow. Headless automation
// runs the app backgrounded (Role: Background); popping a QMenu while the app is
// inactive can segfault in QWindow::geometry() on a null window (seen from a
// QToolButton popup and an AX.25/APRS dialog menu). Raising/activating the
// window gives Cocoa a realized, active window to anchor the popup to.
//
// OFF by default: activateWindow() really does foreground the app, so doing it
// on every driven click would repeatedly steal focus during a sweep — the
// opposite of headless. Enable AETHER_AUTOMATION_RAISE=1 when driving flows that
// pop native menus from a backgrounded instance. Only invoked from the deferred
// (post-socket-callback) drive path. (#3646 follow-up)
void raiseWindowForPopup(QWidget* win)
{
    static const bool kEnabled = qEnvironmentVariableIsSet("AETHER_AUTOMATION_RAISE");
    if (!kEnabled || !win || !win->isVisible())
        return;          // default: no focus-steal; also don't force-show a hidden window
    win->raise();
    win->activateWindow();
}

// The app's primary top-level QMainWindow — menu-bar actions and the dialogs
// they open belong to it, so it's the window to activate before a menu trigger.
QWidget* primaryTopLevelWindow()
{
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* w : tops)
        if (qobject_cast<QMainWindow*>(w))
            return w;
    return nullptr;
}

// ---- Model snapshots for get(). Hand-built from existing getters so we don't
// have to annotate every model field as a Q_PROPERTY; one call returns the full
// assertable state an agent needs. ----

QJsonObject sliceSnapshot(const SliceModel* s)
{
    return QJsonObject{
        {QStringLiteral("sliceId"),    s->sliceId()},
        {QStringLiteral("letter"),     s->letter()},
        {QStringLiteral("panId"),      s->panId()},
        {QStringLiteral("frequency"),  s->frequency()},   // MHz
        {QStringLiteral("mode"),       s->mode()},
        {QStringLiteral("filterLow"),  s->filterLow()},
        {QStringLiteral("filterHigh"), s->filterHigh()},
        {QStringLiteral("active"),     s->isActive()},
        {QStringLiteral("txSlice"),    s->isTxSlice()},
        {QStringLiteral("rxAntenna"),  s->rxAntenna()},
        {QStringLiteral("txAntenna"),  s->txAntenna()},   // live TX antenna — lets a driver enforce the dummy-load gate before keying (#3646)
        {QStringLiteral("rfGain"),     s->rfGain()},
        {QStringLiteral("audioGain"),  s->audioGain()},
        {QStringLiteral("flexAudioGain"), s->flexAudioGain()},
        {QStringLiteral("audioPan"),   s->audioPan()},
        {QStringLiteral("flexAudioPan"), s->flexAudioPan()},
        {QStringLiteral("audioMute"),  s->audioMute()},
        {QStringLiteral("flexAudioMute"), s->flexAudioMute()},
        {QStringLiteral("locked"),     s->isLocked()},
        {QStringLiteral("diversity"),  s->diversity()},
        {QStringLiteral("diversityParent"), s->isDiversityParent()},
        {QStringLiteral("diversityChild"), s->isDiversityChild()},
        {QStringLiteral("diversityIndex"), s->diversityIndex()},
        {QStringLiteral("nb"),         s->nbOn()},
        {QStringLiteral("nbLevel"),    s->nbLevel()},
        {QStringLiteral("nr"),         s->nrOn()},
        {QStringLiteral("nrLevel"),    s->nrLevel()},
        {QStringLiteral("anf"),        s->anfOn()},
        {QStringLiteral("apf"),        s->apfOn()},
        {QStringLiteral("apfLevel"),   s->apfLevel()},
        {QStringLiteral("externalReceiveReplacement"),
                                     s->externalReceiveReplacementActive()},
        {QStringLiteral("squelch"),    s->squelchOn()},
        {QStringLiteral("squelchLevel"), s->squelchLevel()},
        {QStringLiteral("receiveSquelch"), s->receiveSquelchOn()},
        {QStringLiteral("receiveSquelchLevel"), s->receiveSquelchLevel()},
        {QStringLiteral("flexSquelch"), s->flexSquelchOn()},
        {QStringLiteral("flexSquelchLevel"), s->flexSquelchLevel()},
        {QStringLiteral("agcMode"),    s->agcMode()},
        {QStringLiteral("agcThreshold"), s->agcThreshold()},
        {QStringLiteral("receiveAgcMode"), s->receiveAgcMode()},
        {QStringLiteral("receiveAgcThreshold"), s->receiveAgcThreshold()},
        {QStringLiteral("receiveAgcOffLevel"), s->receiveAgcOffLevel()},
        {QStringLiteral("flexAgcMode"), s->flexAgcMode()},
        {QStringLiteral("flexAgcThreshold"), s->flexAgcThreshold()},
        {QStringLiteral("flexAgcOffLevel"), s->flexAgcOffLevel()},
        // Adaptive RX filter (SSB) — settings plus the live AUTO/active state, so an
        // agent can drive the controls via invoke and assert the result via get slice.
        {QStringLiteral("adaptiveFilterEnabled"), s->adaptiveFilterEnabled()},
        {QStringLiteral("adaptiveMinLowCut"),     s->adaptiveMinLowCut()},
        {QStringLiteral("adaptiveMaxHighCut"),    s->adaptiveMaxHighCut()},
        {QStringLiteral("adaptiveMinSnr"),        s->adaptiveMinSnr()},
        {QStringLiteral("adaptiveResponse"),      s->adaptiveResponse()},
        {QStringLiteral("adaptiveSplatter"),      s->adaptiveSplatter()},
        {QStringLiteral("adaptiveHetReject"),     s->adaptiveHetReject()},
        {QStringLiteral("adaptiveActive"),        s->adaptiveActive()},
    };
}

QJsonObject panSnapshot(const PanadapterModel* p, const RadioModel* radio)
{
    const quint32 ourHandle = radio ? radio->ourClientHandle() : 0;
    const QString panId = p->panId();
    return QJsonObject{
        {QStringLiteral("panId"),        panId},
        // #3977: radio-authoritative owner of this pan; assertions on
        // multi-session tests key off these two fields. Empty string (not
        // "0x") when the radio has not yet attributed the pan.
        {QStringLiteral("clientHandle"),
         p->clientHandle().isEmpty() ? QString()
                                     : QStringLiteral("0x") + p->clientHandle()},
        {QStringLiteral("ownedByUs"),    p->ownedByClient(ourHandle)},
        {QStringLiteral("centerMhz"),    p->centerMhz()},
        {QStringLiteral("bandwidthMhz"), p->bandwidthMhz()},
        {QStringLiteral("minDbm"),       p->minDbm()},
        {QStringLiteral("maxDbm"),       p->maxDbm()},
        {QStringLiteral("rxAntenna"),    p->rxAntenna()},
        {QStringLiteral("rfGain"),       p->rfGain()},
        {QStringLiteral("wide"),         p->wideActive()},
        {QStringLiteral("fps"),          p->fps()},
        {QStringLiteral("transmitInhibited"),
         radio && radio->panTransmitInhibited(panId)},
        {QStringLiteral("transmitInhibitReason"),
         radio ? radio->panTransmitInhibitReason(panId) : QString()},
    };
}

QJsonObject vfoFlagSnapshot(QWidget* vfo, RadioModel* radio)
{
    const int sliceId = vfo->property("sliceId").toInt();
    const SliceModel* slice = radio ? radio->slice(sliceId) : nullptr;
    QWidget* spectrum = spectrumForVfoWidget(vfo);
    const QString attachedPanId = panIdForSpectrumWidget(spectrum);
    const QString expectedPanId = slice ? slice->panId() : QString();

    QJsonObject flag{
        {QStringLiteral("sliceId"), sliceId},
        {QStringLiteral("expectedPanId"), expectedPanId},
        {QStringLiteral("attachedPanId"), attachedPanId},
        {QStringLiteral("attachedToExpectedPan"),
         !expectedPanId.isEmpty() && attachedPanId == expectedPanId},
        {QStringLiteral("visible"), vfo->isVisible()},
        {QStringLiteral("enabled"), vfo->isEnabled()},
        {QStringLiteral("geometry"), widgetGeometryJson(vfo)},
    };
    if (slice) {
        flag[QStringLiteral("letter")] = slice->letter();
    }
    if (spectrum) {
        flag[QStringLiteral("spectrumObjectName")] = spectrum->objectName();
        const QVariant panIndex = spectrum->property("panIndex");
        if (panIndex.isValid()) {
            flag[QStringLiteral("attachedPanIndex")] = panIndex.toInt();
        }
    }
    if (!vfo->objectName().isEmpty()) {
        flag[QStringLiteral("objectName")] = vfo->objectName();
    }
    return flag;
}

QJsonObject radioSnapshot(const RadioModel* r)
{
    // Multi-Flex slot occupancy across the radio's whole slice capacity: each
    // slot is ours / foreign (another client, e.g. a Maestro) / empty. This is
    // why an `slice add` can be refused even when we hold only one slice — the
    // capacity is radio-wide, shared across clients. (#3646)
    QJsonArray slotArr;   // not "slots" — that's a Qt macro
    const int maxSlices = r->maxSlices();
    for (int id = 0; id < maxSlices; ++id) {
        QString state = QStringLiteral("empty");
        if (r->isSlotOurs(id))         state = QStringLiteral("ours");
        else if (r->isSlotForeign(id)) state = QStringLiteral("foreign");
        QJsonObject slot{{QStringLiteral("id"), id}, {QStringLiteral("state"), state}};
        if (state == QLatin1String("foreign"))
            slot[QStringLiteral("owner")] = r->foreignSliceOwnerStation(id);
        slotArr.append(slot);
    }

    return QJsonObject{
        {QStringLiteral("name"),         r->name()},
        {QStringLiteral("model"),        r->model()},
        {QStringLiteral("version"),      r->version()},
        {QStringLiteral("serial"),       r->serial()},
        {QStringLiteral("callsign"),     r->callsign()},
        {QStringLiteral("nickname"),     r->nickname()},
        {QStringLiteral("connected"),    r->isConnected()},
        {QStringLiteral("fullDuplex"),   r->fullDuplexEnabled()},
        {QStringLiteral("transmitting"), r->isRadioTransmitting()},
        {QStringLiteral("txPower"),      r->txPower()},
        {QStringLiteral("paTemp"),       r->paTemp()},
        {QStringLiteral("sliceCount"),   r->slices().size()},
        {QStringLiteral("maxSlices"),    maxSlices},
        {QStringLiteral("slots"),        slotArr},
        {QStringLiteral("panCount"),     r->panadapters().size()},
    };
}

QJsonObject gpsSnapshot(const RadioModel* r)
{
    return QJsonObject{
        {QStringLiteral("available"), r->hasGpsHardware()
             || !r->gpsStatus().isEmpty()},
        {QStringLiteral("status"), r->gpsStatus()},
        {QStringLiteral("tracked"), r->gpsTracked()},
        {QStringLiteral("visible"), r->gpsVisible()},
        {QStringLiteral("grid"), r->gpsGrid()},
        {QStringLiteral("altitude"), r->gpsAltitude()},
        {QStringLiteral("latitude"), r->gpsLat()},
        {QStringLiteral("longitude"), r->gpsLon()},
        {QStringLiteral("utcTime"), r->gpsTime()},
        {QStringLiteral("speed"), r->gpsSpeed()},
        {QStringLiteral("course"), r->gpsTrack()},
        {QStringLiteral("frequencyError"), r->gpsFreqError()},
        {QStringLiteral("ntpServerAddress"), r->gpsNtpServerAddress()},
        {QStringLiteral("referenceSetting"), r->oscSetting()},
        {QStringLiteral("referenceActual"), r->oscState()},
        {QStringLiteral("referenceLocked"), r->oscLocked()},
    };
}

// TX-chain state (TransmitModel) — RF power, mic/processor, VOX/AM/DEXP, CW, ATU
// and APD. Lets a QA scenario assert that a TX/Phone/CW applet control actually
// reached the radio model, not just the widget (#3646 QA finding 2). Read-only:
// keying state (mox/tune/transmitting) is reported but never driven from here.
QString atuStatusName(ATUStatus s)
{
    switch (s) {
    case ATUStatus::None:         return QStringLiteral("none");
    case ATUStatus::NotStarted:   return QStringLiteral("not_started");
    case ATUStatus::InProgress:   return QStringLiteral("in_progress");
    case ATUStatus::Bypass:       return QStringLiteral("bypass");
    case ATUStatus::Successful:   return QStringLiteral("successful");
    case ATUStatus::OK:           return QStringLiteral("ok");
    case ATUStatus::FailBypass:   return QStringLiteral("fail_bypass");
    case ATUStatus::Fail:         return QStringLiteral("fail");
    case ATUStatus::Aborted:      return QStringLiteral("aborted");
    case ATUStatus::ManualBypass: return QStringLiteral("manual_bypass");
    }
    return QStringLiteral("unknown");
}

QJsonObject transmitSnapshot(const TransmitModel* t)
{
    return QJsonObject{
        // power / keying (read-only)
        {QStringLiteral("rfPower"),         t->rfPower()},
        {QStringLiteral("tunePower"),       t->tunePower()},
        {QStringLiteral("tuning"),          t->isTuning()},
        {QStringLiteral("mox"),             t->isMox()},
        {QStringLiteral("transmitting"),    t->isTransmitting()},
        {QStringLiteral("maxPowerLevel"),   t->maxPowerLevel()},
        {QStringLiteral("activeProfile"),   t->activeProfile()},
        // mic / monitor / processor
        {QStringLiteral("micSelection"),    t->micSelection()},
        {QStringLiteral("micLevel"),        t->micLevel()},
        {QStringLiteral("micAcc"),          t->micAcc()},
        {QStringLiteral("speechProc"),      t->speechProcessorEnable()},
        {QStringLiteral("speechProcLevel"), t->speechProcessorLevel()},
        {QStringLiteral("dax"),             t->daxOn()},
        {QStringLiteral("monitor"),         t->sbMonitor()},
        {QStringLiteral("monGainSb"),       t->monGainSb()},
        {QStringLiteral("activeMicProfile"),t->activeMicProfile()},
        // VOX / AM / DEXP / TX filter
        {QStringLiteral("voxEnable"),       t->voxEnable()},
        {QStringLiteral("voxLevel"),        t->voxLevel()},
        {QStringLiteral("voxDelay"),        t->voxDelay()},
        {QStringLiteral("amCarrierLevel"),  t->amCarrierLevel()},
        {QStringLiteral("dexp"),            t->dexpOn()},
        {QStringLiteral("dexpLevel"),       t->dexpLevel()},
        {QStringLiteral("txFilterLow"),     t->txFilterLow()},
        {QStringLiteral("txFilterHigh"),    t->txFilterHigh()},
        // CW
        {QStringLiteral("cwSpeed"),         t->cwSpeed()},
        {QStringLiteral("cwPitch"),         t->cwPitch()},
        {QStringLiteral("cwBreakIn"),       t->cwBreakIn()},
        {QStringLiteral("cwDelay"),         t->cwDelay()},
        {QStringLiteral("cwSidetone"),      t->cwSidetone()},
        {QStringLiteral("cwIambic"),        t->cwIambic()},
        {QStringLiteral("monGainCw"),       t->monGainCw()},
        {QStringLiteral("monPanCw"),        t->monPanCw()},
        // ATU / APD
        {QStringLiteral("atuEnabled"),      t->atuEnabled()},
        {QStringLiteral("atuMemories"),     t->memoriesEnabled()},
        {QStringLiteral("atuStatus"),       atuStatusName(t->atuStatus())},
        {QStringLiteral("apdEnabled"),      t->apdEnabled()},
        {QStringLiteral("showTxInWaterfall"), t->showTxInWaterfall()},
    };
}

// CWX keyer snapshot — the queue-drain watch that the #3949 fix rests on.
// `cwxEndIndex` is the radio_index of the last char in the batch we're waiting
// to drain (-1 = not tracking); `sentIndex` is the radio's live `cwx sent=`
// counter. When sentIndex reaches cwxEndIndex, queueEmpty() fires and TX is
// released. There is no widget exposing this state, so this is the only
// non-hardware-poll way to assert the fix (cf. `get dsp`).
QJsonObject cwxSnapshot(const CwxModel* c, bool active)
{
    return QJsonObject{
        {QStringLiteral("active"),      active},           // TX in flight (RadioModel::cwxActive)
        {QStringLiteral("tracking"),    c->cwxEndIndex() >= 0},
        {QStringLiteral("cwxEndIndex"), c->cwxEndIndex()}, // batch-end radio_index being watched
        {QStringLiteral("sentIndex"),   c->sentIndex()},   // live `cwx sent=` counter
        {QStringLiteral("speed"),       c->speed()},
        {QStringLiteral("speedStep"),   c->speedStep()},
        {QStringLiteral("delay"),       c->delay()},
        {QStringLiteral("qsk"),         c->qskOn()},
        {QStringLiteral("live"),        c->isLive()},
    };
}

QJsonObject audioSnapshot(const AudioEngine* audio)
{
    return QJsonObject{
        {QStringLiteral("muted"), audio->isMuted()},
        {QStringLiteral("rxStreaming"), audio->isRxStreaming()},
        {QStringLiteral("txStreaming"), audio->isTxStreaming()},
        {QStringLiteral("kiwiSdrTransmitMuted"),
            audio->kiwiSdrAudioTransmitMuted()},
        {QStringLiteral("rxBufferBytes"),
            static_cast<qint64>(audio->rxBufferBytes())},
        {QStringLiteral("rxBufferPeakBytes"),
            static_cast<qint64>(audio->rxBufferPeakBytes())},
        {QStringLiteral("rxBufferUnderrunCount"),
            static_cast<double>(audio->rxBufferUnderrunCount())},
        {QStringLiteral("rxBufferSampleRate"),
            audio->rxBufferSampleRate()},
        {QStringLiteral("receivePresentationOutputSignalEmitCount"),
            static_cast<double>(
                audio->receivePresentationOutputSignalEmitCount())},
        {QStringLiteral("receivePresentationOutputSignalSuppressedCount"),
            static_cast<double>(
                audio->receivePresentationOutputSignalSuppressedCount())},
        {QStringLiteral("endpoints"),
            audio->audioEndpointDiagnostics()},
    };
}

QJsonObject audioSnapshotOnObjectThread(AudioEngine* audio, bool* ok)
{
    *ok = false;
    if (!audio) {
        return {};
    }

    if (!audio->thread() || audio->thread() == QThread::currentThread()) {
        *ok = true;
        return audioSnapshot(audio);
    }

    QJsonObject snapshot;
    const bool invoked = QMetaObject::invokeMethod(
        audio,
        [audio, &snapshot]() {
            snapshot = audioSnapshot(audio);
        },
        Qt::BlockingQueuedConnection);
    *ok = invoked;
    return snapshot;
}

// Client-side AetherDSP noise-reduction state (#3856). `get slice` reports the
// radio-side nr/nb/anf; this is the missing model for the six client-side
// AudioEngine modules (NR2 / NR4 / MNR / DFNR / RN2 / BNR) so a driver can
// assert which method is active and read its tuning without a screenshot of the
// AetherDSP applet. The modules are mutually exclusive, so `active` names the one
// enabled module (or "none"). `available` reflects compile-time backend gating —
// the same guards the selector buttons use to dim an unbuildable method.
// Engine-only portion (enable flags + engine-owned tuning getters); the NR2/NR4
// slider params live in AppSettings and are merged in by the caller on the main
// thread. Runs on the AudioEngine thread (m_bnr/m_dfnr are not main-thread safe).
QJsonObject dspEngineSnapshot(const AudioEngine* a)
{
    struct Mod { const char* name; bool enabled; bool available; };
    const Mod mods[] = {
        {"NR2",  a->nr2Enabled(),  true},
        {"NR4",  a->nr4Enabled(),
#ifdef HAVE_SPECBLEACH
                                   true},
#else
                                   false},
#endif
        {"MNR",  a->mnrEnabled(),
#ifdef Q_OS_MAC
                                   true},
#else
                                   false},
#endif
        {"DFNR", a->dfnrEnabled(),
#ifdef HAVE_DFNR
                                   true},
#else
                                   false},
#endif
        {"RN2",  a->rn2Enabled(),  true},
        {"BNR",  a->nvAfxEnabled(),
#ifdef HAVE_NVIDIA_AFX
                                   true},
#else
                                   false},
#endif
    };

    QJsonObject methods;
    QString active = QStringLiteral("none");
    for (const Mod& m : mods) {
        methods[QLatin1String(m.name)] =
            QJsonObject{{QStringLiteral("enabled"), m.enabled},
                        {QStringLiteral("available"), m.available}};
        if (m.enabled) active = QLatin1String(m.name);
    }

    // Engine-owned tuning (the slider params that have live engine getters).
    QJsonObject tuning;
    tuning[QStringLiteral("nr2Runtime")] = a->nr2RuntimeDiagnostics();
    tuning[QStringLiteral("mnr")] =
        QJsonObject{{QStringLiteral("strength"), a->mnrStrength()}};
    tuning[QStringLiteral("dfnr")] =
        QJsonObject{{QStringLiteral("attenLimitDb"), a->dfnrAttenLimit()}};
    // BNR is now the in-process NVIDIA AFX denoiser (#3902): no container, so
    // the old address/connected fields are gone; report the persisted intensity.
    tuning[QStringLiteral("bnr")] =
        QJsonObject{{QStringLiteral("intensity"), NvidiaBnrSettings::intensity()}};

    return QJsonObject{{QStringLiteral("active"), active},
                       {QStringLiteral("methods"), methods},
                       {QStringLiteral("tuning"), tuning}};
}

QJsonObject dspSnapshotOnObjectThread(AudioEngine* audio, bool* ok)
{
    *ok = false;
    if (!audio) return {};
    if (!audio->thread() || audio->thread() == QThread::currentThread()) {
        *ok = true;
        return dspEngineSnapshot(audio);
    }
    QJsonObject snapshot;
    const bool invoked = QMetaObject::invokeMethod(
        audio,
        [audio, &snapshot]() { snapshot = dspEngineSnapshot(audio); },
        Qt::BlockingQueuedConnection);
    *ok = invoked;
    return snapshot;
}

// 8-band graphic EQ (RX + TX). Lets a scenario assert EQ-applet slider changes
// reached the model (#3646). Bands keyed by their short labels (63 … 8k).
QJsonObject equalizerSnapshot(const EqualizerModel* e)
{
    QJsonObject rx, tx;
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        const auto band = static_cast<EqualizerModel::Band>(i);
        const QString key = EqualizerModel::bandLabel(band);
        rx[key] = e->rxBand(band);
        tx[key] = e->txBand(band);
    }
    return QJsonObject{
        {QStringLiteral("rxEnabled"), e->rxEnabled()},
        {QStringLiteral("txEnabled"), e->txEnabled()},
        {QStringLiteral("rx"), rx},
        {QStringLiteral("tx"), tx},
    };
}

// Annotate meters known to be unreliable on the connected radio, mirroring the
// curation the UI already does, so a consumer of the raw `all` table doesn't
// trust a bad reading. Today this is exactly one entry: PACURRENT on the
// FLEX-8000 series, where the declared 10 A meter range is below real PA draw so
// it clips (SMART-11281) — the GUI omits it (see MeterApplet.h), and freshness
// (age_ms) can't catch a fresh-but-clipped value. Keep this list in sync with
// the UI; it is intentionally a small explicit table, not a heuristic. (#3729)
QString unreliableMeterNote(const QString& meterName, const QString& radioModel)
{
    if (meterName == QLatin1String("PACURRENT")
        && radioModel.startsWith(QStringLiteral("FLEX-8"))) {
        return QStringLiteral("clips at the declared 10A cap on FLEX-8000 series "
                              "(SMART-11281); omitted from the UI — do not trust");
    }
    return QString();
}

// Live meter readout. The flat convenience fields are the headline TX meters
// with their freshness age (ms since last update, -1 if never) so a reader can
// reject stale values — critical because some meters (notably PACURRENT) are
// only reported ~1 s into a transmit. `all` carries every defined meter with
// per-meter index/source_index/age_ms so duplicate-named meters (one live, one
// floored) are distinguishable, plus a `reliable:false`+`note` flag on meters
// known-bad for the connected radio. (#3646, #3729)
QJsonObject metersSnapshot(MeterModel* m, const QString& radioModel)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto age = [now](qint64 ts) -> qint64 { return ts > 0 ? now - ts : -1; };

    QJsonArray all = m->allMeters();
    for (int i = 0; i < all.size(); ++i) {
        QJsonObject meter = all[i].toObject();
        const QString note = unreliableMeterNote(meter.value(QStringLiteral("name")).toString(),
                                                 radioModel);
        if (!note.isEmpty()) {
            meter[QStringLiteral("reliable")] = false;
            meter[QStringLiteral("note")]     = note;
            all[i] = meter;
        }
    }

    return QJsonObject{
        {QStringLiteral("fwdPower"),        m->fwdPower()},           // Watts (smoothed)
        {QStringLiteral("fwdPowerInstant"), m->fwdPowerInstant()},    // Watts (peak)
        {QStringLiteral("fwdPowerAgeMs"),   age(m->fwdPowerUpdatedAtMs())},
        {QStringLiteral("reflectedPower"),  m->reflectedPower()},
        {QStringLiteral("reflectedPowerAgeMs"),
         age(m->reflectedPowerUpdatedAtMs())},
        {QStringLiteral("reflectedPowerMeasured"),
         m->hasRecentReflectedPower(500)},
        {QStringLiteral("swr"),             m->swr()},
        {QStringLiteral("swrAgeMs"),        age(m->swrUpdatedAtMs())},
        {QStringLiteral("paTemp"),          m->paTemp()},             // °C
        {QStringLiteral("supplyVolts"),     m->supplyVolts()},        // V
        {QStringLiteral("swAlc"),           m->swAlc()},              // dBFS post-ALC SSB peak
        {QStringLiteral("hwAlc"),           m->hwAlc()},              // dBFS external HW-ALC
        {QStringLiteral("micPeak"),         m->micPeak()},            // dBFS
        {QStringLiteral("micLevel"),        m->micLevel()},           // dBFS
        {QStringLiteral("compPeak"),        m->compPeak()},           // dB compression (peak)
        {QStringLiteral("compLevel"),       m->compLevel()},          // dB compression
        {QStringLiteral("hasCompression"),  m->hasCompressionMeterValue()},
        {QStringLiteral("sLevel"),          m->sLevel()},             // dBm
        {QStringLiteral("txMetersFresh"),   m->hasRecentTxMeters(2000)},
        {QStringLiteral("txMetersAgeMs"),   age(m->txMetersUpdatedAtMs())},
        {QStringLiteral("all"),             all},                     // every meter + age_ms + reliability
    };
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

    // Restrict the socket to the owning user (0600 / per-user pipe ACL). The
    // endpoint can key TX and its path is advertised in a shared-temp discovery
    // file, so another local user must not be able to connect and drive the GUI.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);

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

    // Drop discovery info so a driver can find the resolved endpoint without
    // knowing the platform-specific socket path. Two forms, both best-effort:
    //   1. A per-pid entry in a discovery DIRECTORY, so multiple concurrent
    //      AETHER_AUTOMATION instances each own one file and can be ENUMERATED
    //      (a driver lists the dir, reads {pid,socket,label}, picks the right
    //      one). Each instance removes only its own entry on stop. (#3646)
    //   2. A legacy single fixed file pointing at THIS instance, so existing
    //      single-instance drivers keep working unchanged. On stop it is removed
    //      only if it still points at our pid (so an exiting sibling can't blind
    //      a survivor).
    m_label = qEnvironmentVariable("AETHER_AUTOMATION_LABEL");
    const QString tmp = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QJsonObject disc;
    disc[QStringLiteral("socket")]  = fullServerName();
    disc[QStringLiteral("name")]    = serverName;
    disc[QStringLiteral("pid")]     = static_cast<qint64>(QCoreApplication::applicationPid());
    disc[QStringLiteral("version")] = QCoreApplication::applicationVersion();
    if (!m_label.isEmpty())
        disc[QStringLiteral("label")] = m_label;
    disc[QStringLiteral("startedAt")] = QDateTime::currentMSecsSinceEpoch();
    const QByteArray discJson = QJsonDocument(disc).toJson(QJsonDocument::Compact);

    m_discoveryDir = QDir(tmp).filePath(QStringLiteral("aethersdr-automation"));
    if (QDir().mkpath(m_discoveryDir)) {
        m_discoveryEntry = QDir(m_discoveryDir)
                               .filePath(QString::number(QCoreApplication::applicationPid())
                                         + QStringLiteral(".json"));
        QFile ef(m_discoveryEntry);
        if (ef.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            ef.write(discJson);
            ef.close();
        } else {
            m_discoveryEntry.clear();
        }
    }

    m_discoveryFile = QDir(tmp).filePath(QStringLiteral("aethersdr-automation.json"));
    QFile df(m_discoveryFile);
    if (df.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        df.write(discJson);
        df.close();
    } else {
        m_discoveryFile.clear();
    }

    // TX safety rails (#3646): the watchdog force-unkeys the radio if it stays
    // keyed past a limit — a backstop independent of whatever script drives us.
    // Read the key-time / power-ceiling limits UNCONDITIONALLY so they also
    // apply when TX is enabled later via the GUI toggle (setTxAllowed) — they
    // are watchdog policy, not an env-only feature. Only the watchdog *arming*
    // is gated on TX actually being allowed.
    if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_TX_MAX_MS"))
        m_txMaxKeyMs = qEnvironmentVariableIntValue("AETHER_AUTOMATION_TX_MAX_MS");
    if (qEnvironmentVariableIsSet("AETHER_AUTOMATION_TX_MAX_POWER"))
        m_txMaxPower = qEnvironmentVariableIntValue("AETHER_AUTOMATION_TX_MAX_POWER");
    m_txAllowed = qEnvironmentVariableIsSet("AETHER_AUTOMATION_ALLOW_TX");
    if (m_txAllowed) {
        m_txWatchdog = new QTimer(this);
        m_txWatchdog->setInterval(500);
        connect(m_txWatchdog, &QTimer::timeout, this, &AutomationServer::onTxWatchdog);
        m_txWatchdog->start();
        qCInfo(lcAutomation).noquote()
            << "TX automation ENABLED — watchdog max key" << m_txMaxKeyMs << "ms,"
            << "power ceiling" << (m_txMaxPower < 0 ? QStringLiteral("none")
                                                    : QString::number(m_txMaxPower));
    }

    // Observability suite (#3646): start the monotonic clock, tap the log
    // funnel into a ring buffer, and arm the (idle) drain timer used by
    // `log subscribe`. The tap runs on arbitrary logging threads, so it only
    // touches the mutex-guarded ring — never a socket.
    m_monoClock.start();
    m_logTapId = LogManager::instance().addTap(
        [this](QtMsgType type, const QString& cat, const QString& msg) {
            QMutexLocker lk(&m_logMutex);
            LogEvent e;
            e.seq    = ++m_logSeq;
            e.monoUs = m_monoClock.nsecsElapsed() / 1000;
            e.wall   = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"));
            e.type   = static_cast<int>(type);
            e.cat    = cat;
            e.msg    = msg;
            // If doMark armed a capture sink on this thread, this is the MARK's
            // own tap call (#3756) — publish the seq/mono we just assigned so the
            // caller bracket can't be skewed by a concurrent push afterward.
            if (g_markSink) {
                g_markSink->seq    = e.seq;
                g_markSink->monoUs = e.monoUs;
                g_markSink->set    = true;
            }
            m_logRing.push_back(std::move(e));
            while (m_logRing.size() > static_cast<size_t>(kLogRingMax))
                m_logRing.pop_front();
        });
    m_logDrain = new QTimer(this);
    m_logDrain->setInterval(50);
    connect(m_logDrain, &QTimer::timeout, this, &AutomationServer::onLogDrain);

    // Agent station identity (#3646): announce the agent's name to other
    // MultiFlex clients. Apply now if already connected; on every (re)connect —
    // which re-runs the handshake's own `client station <user>` send — re-apply
    // shortly after so the agent name is what sticks.
    m_agentStation = AppSettings::instance().automationAgentName();
    if (m_agentStation.isEmpty()) {
        m_agentStation = qEnvironmentVariableIsSet("AETHER_AUTOMATION_STATION")
                             ? qEnvironmentVariable("AETHER_AUTOMATION_STATION")
                             : QStringLiteral("Automation");
    }
    if (m_radioModel) {
        connect(m_radioModel, &RadioModel::connectionStateChanged, this,
                [this](bool connected) {
                    if (!connected || m_agentStation.isEmpty())
                        return;
                    QPointer<AutomationServer> self(this);
                    QTimer::singleShot(1000, this, [self]() {
                        if (self) self->applyAgentStation(self->m_agentStation);
                    });
                });
        if (m_radioModel->isConnected())
            applyAgentStation(m_agentStation);
    }

    qCInfo(lcAutomation).noquote()
        << "automation bridge listening on" << fullServerName()
        << QStringLiteral("(verbs: %1)").arg(verbNamesJoined());
    return true;
}

void AutomationServer::stop()
{
    if (!m_server)
        return;

    // A phaseful gesture owns a synthetic left-button press. Release it before
    // clients/widgets are torn down so a slider never remains logically down
    // after the bridge stops.
    cancelGesture(nullptr, QStringLiteral("automation bridge stopping"));

    // Safety: never leave the radio keyed when the bridge shuts down.
    if (m_txAllowed)
        forceUnkey("automation bridge stopping");

    // Restore the user's real station name so live MultiFlex peers stop seeing
    // the agent name immediately (don't wait for the disconnect to drop it).
    restoreStation();
    if (m_txWatchdog) {
        m_txWatchdog->stop();
        m_txWatchdog->deleteLater();
        m_txWatchdog = nullptr;
    }
    // Detach the log tap before teardown so no logging thread can touch us.
    if (m_logTapId >= 0) {
        LogManager::instance().removeTap(m_logTapId);
        m_logTapId = -1;
    }
    if (m_logDrain) {
        m_logDrain->stop();
        m_logDrain->deleteLater();
        m_logDrain = nullptr;
    }
    m_logSubscribers.clear();
    for (const std::shared_ptr<ConnectWait>& wait : m_connectWaits) {
        if (!wait) {
            continue;
        }
        if (wait->timer) {
            wait->timer->stop();
            wait->timer->deleteLater();
            wait->timer = nullptr;
        }
        QObject::disconnect(wait->connection);
        wait->complete = true;
    }
    m_connectWaits.clear();

    for (auto it = m_buffers.constBegin(); it != m_buffers.constEnd(); ++it)
        it.key()->deleteLater();
    m_buffers.clear();

    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;

    // Our own per-pid entry is always ours to remove.
    if (!m_discoveryEntry.isEmpty()) {
        QFile::remove(m_discoveryEntry);
        m_discoveryEntry.clear();
    }
    // The legacy shared pointer: remove it only if it still points at US, so an
    // exiting sibling can't blind a still-running instance that owns it now.
    if (!m_discoveryFile.isEmpty()) {
        QFile lf(m_discoveryFile);
        if (lf.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(lf.readAll()).object();
            lf.close();
            if (o.value(QStringLiteral("pid")).toVariant().toLongLong()
                == QCoreApplication::applicationPid())
                QFile::remove(m_discoveryFile);
        }
        m_discoveryFile.clear();
    }
}

void AutomationServer::setTxAllowed(bool allowed)
{
    if (m_txAllowed == allowed)
        return;  // idempotent
    m_txAllowed = allowed;
    if (allowed) {
        // Arm the force-unkey watchdog (mirrors the start()-time arming). The
        // TX_MAX_MS / TX_MAX_POWER limits are read unconditionally in start(),
        // so the same key-time and power-ceiling policy applies on this GUI
        // path as on the env path.
        if (!m_txWatchdog) {
            m_txWatchdog = new QTimer(this);
            m_txWatchdog->setInterval(500);
            connect(m_txWatchdog, &QTimer::timeout, this, &AutomationServer::onTxWatchdog);
            m_txWatchdog->start();
        }
        qCInfo(lcAutomation).noquote()
            << "TX automation ENABLED by operator — watchdog max key"
            << m_txMaxKeyMs << "ms, power ceiling"
            << (m_txMaxPower < 0 ? QStringLiteral("none") : QString::number(m_txMaxPower));
    } else {
        // Disabling: never leave the radio keyed by a script mid-transmit,
        // then disarm the watchdog. forceUnkey is a safe no-op if not keyed.
        forceUnkey("TX automation disabled by operator");
        if (m_txWatchdog) {
            m_txWatchdog->stop();
            m_txWatchdog->deleteLater();
            m_txWatchdog = nullptr;
        }
        qCInfo(lcAutomation).noquote() << "TX automation DISABLED by operator";
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
    QPointer<QLocalSocket> sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock) {
        return;
    }

    QLocalSocket* socket = sock.data();
    QHash<QLocalSocket*, QByteArray>::iterator it = m_buffers.find(socket);
    if (it == m_buffers.end()) {
        return;
    }
    it.value().append(socket->readAll());

    while (sock) {
        QByteArray line;
        {
            socket = sock.data();
            if (!socket) {
                return;
            }

            it = m_buffers.find(socket);
            if (it == m_buffers.end()) {
                return;
            }

            QByteArray& buf = it.value();
            const int nl = buf.indexOf('\n');
            if (nl < 0) {
                break;
            }
            line = buf.left(nl);
            buf.remove(0, nl + 1);
        }
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const QJsonObject resp = handleLine(line, sock);
        socket = sock.data();
        if (!socket || m_buffers.find(socket) == m_buffers.end()
            || socket->state() == QLocalSocket::UnconnectedState) {
            qCDebug(lcAutomation)
                << "dropping automation response because client disconnected during request";
            return;
        }

        if (isDeferredResponse(resp)) {
            continue;
        }

        if (!writeJsonResponse(socket, resp)) {
            return;
        }
    }
}

void AutomationServer::onDisconnected()
{
    auto* sock = qobject_cast<QLocalSocket*>(sender());
    if (!sock)
        return;
    cancelGesture(sock, QStringLiteral("gesture owner disconnected"));
    m_buffers.remove(sock);
    if (m_logSubscribers.remove(sock) && m_logSubscribers.isEmpty() && m_logDrain)
        m_logDrain->stop();
    const auto newEnd = std::remove_if(
        m_connectWaits.begin(), m_connectWaits.end(),
        [sock](const std::shared_ptr<ConnectWait>& wait) {
            if (!wait || wait->socket != sock) {
                return false;
            }
            if (wait->timer) {
                wait->timer->stop();
                wait->timer->deleteLater();
            }
            QObject::disconnect(wait->connection);
            wait->complete = true;
            return true;
        });
    m_connectWaits.erase(newEnd, m_connectWaits.end());
    sock->deleteLater();
    qCDebug(lcAutomation) << "client disconnected;" << m_buffers.size() << "active";
}

// ── Verb registry (#4174) ────────────────────────────────────────────────────
// One self-contained entry per bridge verb: canonical name, aliases, a
// one-line help summary (surfaced by the `verbs` verb), a bare-line positional
// parser, and the dispatcher. The startup banner and the "unknown command"
// error are DERIVED from this table — never hand-list verbs anywhere else.
// Adding a verb is adding one entry here (plus its doVerb body); nothing else
// to keep in sync. JSON requests normally bypass the parsers (fields map 1:1
// onto VerbArgs in handleLine); the optional `args` field explicitly asks the
// registry to parse the same positional arguments as a bare request.

struct AutomationServer::VerbArgs {
    QString target, path, action, value, model, selector, property;
    QString id, title, detail, tone;
    QString token;                       // shared-secret auth (#3646); JSON `token` field
    int timeoutMs{0};
};

struct AutomationServer::VerbSpec {
    QString name;                                   // canonical spelling
    QStringList aliases;                            // exact-match synonyms
    QString help;                                   // one-line positional summary
    // Fill VerbArgs from the space-split bare line; a non-empty return rejects
    // the request (e.g. `tooltip <t> hide <extras>`).
    std::function<QJsonObject(const QList<QByteArray>&, VerbArgs&)> parse;
    std::function<QJsonObject(AutomationServer&, VerbArgs&, QLocalSocket*)> dispatch;
};

namespace {

// Requests that are pure introspection — allowed even in observe-only mode
// (#4188 area 6). Some diagnostic verbs mix read and write actions, so the
// action must be checked as well as the canonical verb name. Everything else
// (drive/connect/capture/keying) is refused when m_readOnly is set.
bool isReadOnlyRequest(const QString& name, const QString& action)
{
    static const QSet<QString> kSafe = {
        QStringLiteral("ping"),     QStringLiteral("verbs"),
        QStringLiteral("whoami"),   QStringLiteral("dumpTree"),
        QStringLiteral("grab"),     QStringLiteral("get"),
        QStringLiteral("floors"),   QStringLiteral("hitTest"),
    };
    if (kSafe.contains(name)) {
        return true;
    }

    const QString normalizedAction = action.trimmed().toLower();
    if (name == QLatin1String("log")) {
        static const QSet<QString> kSafeLogActions = {
            QString(), QStringLiteral("categories"), QStringLiteral("get"),
            QStringLiteral("tail"), QStringLiteral("subscribe"),
            QStringLiteral("unsubscribe"),
        };
        return kSafeLogActions.contains(normalizedAction);
    }
    if (name == QLatin1String("streams")) {
        static const QSet<QString> kSafeStreamActions = {
            QString(), QStringLiteral("radio"), QStringLiteral("inventory"),
        };
        return kSafeStreamActions.contains(normalizedAction);
    }
    if (name == QLatin1String("gesture")) {
        return normalizedAction == QLatin1String("status");
    }
    return false;
}

QString vtok(const QList<QByteArray>& p, int i)
{
    return QString::fromUtf8(p.value(i));
}

QString vjoin(const QList<QByteArray>& p, int from)
{
    QStringList rest;
    for (int i = from; i < p.size(); ++i)
        rest << vtok(p, i);
    return rest.join(QLatin1Char(' '));
}

// Model names advertised by the `get` verb's required-model error. Kept as a
// list (not a baked string) so the error and any future introspection derive
// from one place.
const QStringList& getModelNames()
{
    static const QStringList kModels{
        QStringLiteral("radio"),      QStringLiteral("transmit"),
        QStringLiteral("meters"),     QStringLiteral("slice"),
        QStringLiteral("slices"),     QStringLiteral("pan"),
        QStringLiteral("pans"),       QStringLiteral("panstats"),
        QStringLiteral("gps"),        QStringLiteral("clock"),
        QStringLiteral("renderstats"),
        QStringLiteral("tracedebug"), QStringLiteral("waveforms"),
        QStringLiteral("kiwi"),
    };
    return kModels;
}

} // namespace

const std::vector<AutomationServer::VerbSpec>& AutomationServer::verbRegistry()
{
    static const std::vector<VerbSpec> kVerbs = [] {
        using A = VerbArgs;

        // ── Shared bare-line parse shapes ───────────────────────────────────
        // Most verbs fit one of these; bespoke parsers live inline in their
        // entry. Local lambdas (not free functions) because VerbArgs is a
        // private nested type.
        auto parseNothing = [](const QList<QByteArray>&, A&) -> QJsonObject {
            return {};
        };
        // Historical default for verbs with no dedicated parse arm ("whoami
        // and friends"): target = tok(1), path = tok(2).
        auto parseTargetPath = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.target = vtok(p, 1);
            a.path = vtok(p, 2);
            return {};
        };
        auto parseTargetOnly = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.target = vtok(p, 1);
            return {};
        };
        auto parseTargetXY = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.target = vtok(p, 1);
            a.value = vtok(p, 2) + QLatin1Char(' ') + vtok(p, 3);
            return {};
        };
        auto parseTargetRest = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.target = vtok(p, 1);
            a.value = vjoin(p, 2);
            return {};
        };
        auto parseActionOnly = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.action = vtok(p, 1);
            return {};
        };
        auto parseActionValue = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.action = vtok(p, 1);
            a.value = vtok(p, 2);
            return {};
        };
        auto parseActionRest = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.action = vtok(p, 1);
            a.value = vjoin(p, 2);
            return {};
        };
        auto parseValueOnly = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.value = vtok(p, 1);
            return {};
        };
        auto parseValueId = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.value = vtok(p, 1);
            a.id = vtok(p, 2);
            return {};
        };
        auto parseValueRest = [](const QList<QByteArray>& p, A& a) -> QJsonObject {
            a.value = vjoin(p, 1);
            return {};
        };

        std::vector<VerbSpec> v;
        auto add = [&v](QString name, QStringList aliases, QString help,
                        std::function<QJsonObject(const QList<QByteArray>&, A&)> parse,
                        std::function<QJsonObject(AutomationServer&, A&, QLocalSocket*)>
                            dispatch) {
            v.push_back({std::move(name), std::move(aliases), std::move(help),
                         std::move(parse), std::move(dispatch)});
        };

        add("ping", {}, "liveness check → app + version + whether a token is required",
            parseNothing,
            [](AutomationServer& self, A&, QLocalSocket*) {
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("app"), QStringLiteral("AetherSDR")},
                    {QStringLiteral("version"), QCoreApplication::applicationVersion()},
                    {QStringLiteral("authRequired"), !self.m_authToken.isEmpty()},
                    {QStringLiteral("readOnly"), self.m_readOnly},
                };
            });

        add("verbs", {}, "list every bridge verb with aliases and help (this table)",
            parseNothing,
            [](AutomationServer&, A&, QLocalSocket*) {
                QJsonArray arr;
                for (const VerbSpec& spec : verbRegistry()) {
                    QJsonObject o{{QStringLiteral("name"), spec.name},
                                  {QStringLiteral("help"), spec.help}};
                    if (!spec.aliases.isEmpty()) {
                        o[QStringLiteral("aliases")] =
                            QJsonArray::fromStringList(spec.aliases);
                    }
                    arr.append(o);
                }
                return QJsonObject{{QStringLiteral("ok"), true},
                                   {QStringLiteral("count"), arr.size()},
                                   {QStringLiteral("verbs"), arr}};
            });

        add("dumpTree", {}, "serialize the full widget tree as JSON",
            parseTargetPath,
            [](AutomationServer& s, A&, QLocalSocket*) { return s.doDumpTree(); });

        add("floors", {}, "per-pan measured noise + display floor (dBm)",
            parseTargetPath,
            [](AutomationServer& s, A&, QLocalSocket*) { return s.doFloors(); });

        add("grab", {}, "grab <target|pan|pan-visible [index]> [path] — PNG capture",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.target = vtok(p, 1);
                if (a.target == QLatin1String("pan")
                    || a.target == QLatin1String("pan-visible")
                    || a.target == QLatin1String("pan-composite")) {
                    a.selector = vtok(p, 2);   // pan index → "grab pan-visible 1 [path]"
                    a.path = vtok(p, 3);
                } else {
                    a.path = vtok(p, 2);       // "grab SpectrumWidget [path]"
                }
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("grab requires a target widget"));
                if (a.target == QLatin1String("pan"))
                    return s.doGrabPan(a.selector, a.path);
                if (a.target == QLatin1String("pan-visible")
                    || a.target == QLatin1String("pan-composite"))
                    return s.doGrabPanVisible(a.selector, a.path);
                return s.doGrab(a.target, a.path);
            });

        add("close", {}, "close <target> — close the target's top-level window",
            parseTargetOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("close requires a target widget/window"));
                return s.doClose(a.target);
            });

        add("hover", {}, "hover <target> [leave] — synthetic mouse hover",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.target = vtok(p, 1);
                a.action = vtok(p, 2);  // optional "leave" → fade after exit
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("hover requires a target widget"));
                return s.doHover(a.target, a.action);
            });

        add("tooltip", {}, "tooltip <target> [hide|text…] — force-show a native tooltip",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.target = vtok(p, 1);
                if (vtok(p, 2) == QLatin1String("hide")) {
                    // "hide" with trailing tokens must not silently become an
                    // override that force-SHOWS a tip reading "hide …" (#4122
                    // review). An override literally starting with "hide" is
                    // available via the JSON form's explicit value field.
                    if (p.size() != 3) {
                        return err(QStringLiteral(
                            "tooltip hide takes no extra arguments"));
                    }
                    a.action = QStringLiteral("hide");
                } else {
                    a.value = vjoin(p, 2);
                }
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("tooltip requires a target widget"));
                return s.doTooltip(a.target, a.action, a.value);
            });

        add("scrollTo", {QStringLiteral("ensureVisible")},
            "scrollTo <target> — scroll a widget into its scroll-area viewport",
            parseTargetPath,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("scrollTo requires a target widget"));
                return s.doScrollTo(a.target);
            });

        add("drag", {QStringLiteral("mouse")},
            "drag <target> <dx> <dy> — synthesize press→move→release",
            parseTargetXY,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("drag requires a target and '<dx> <dy>'"));
                return s.doDrag(a.target, a.value);
            });

        add("dragAt", {},
            "dragAt <target> <x> <y> <dx> <dy> [control|meta|shift|alt,...]",
            parseTargetRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty()) {
                    return err(QStringLiteral("dragAt requires a target and '<x> <y> <dx> <dy>'"));
                }
                return s.doDragAt(a.target, a.value);
            });

        add("gesture", {},
            "gesture <begin|move|end|cancel|status> — phaseful pointer gesture",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.action = vtok(p, 1);
                if (a.action == QLatin1String("begin")) {
                    a.target = vtok(p, 2);
                    a.value = vjoin(p, 3);
                } else {
                    a.value = vjoin(p, 2);
                }
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket* sock) -> QJsonObject {
                return s.doGesture(a.action, a.target, a.value, sock);
            });

        add("showMenu", {QStringLiteral("openMenu")},
            "showMenu <target> — pop a button's drop-down menu",
            parseTargetOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("showMenu requires a target button"));
                return s.doShowMenu(a.target);
            });

        add("contextMenu", {}, "contextMenu <target> [x y] — Qt context-menu path",
            parseTargetXY,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("contextMenu requires a target widget"));
                return s.doContextMenu(a.target, a.value);
            });

        add("rightClick", {}, "rightClick <target> [x y] — mousePressEvent menu path",
            parseTargetXY,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty()) {
                    return err(QStringLiteral("rightClick requires a target widget"));
                }
                return s.doRightClick(a.target, a.value);
            });

        add("hitTest", {QStringLiteral("hittest")},
            "hitTest <target> [x y] — read-only widget-owner probe",
            parseTargetXY,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty())
                    return err(QStringLiteral("hitTest requires a target widget"));
                return s.doHitTest(a.target, a.value);
            });

        add("clickAt", {QStringLiteral("clickat")},
            "clickAt <x> <y> | clickAt <target> <x> <y> — TX-guarded coordinate click",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                // Overloaded: "clickAt <x> <y>" (global) vs "clickAt <target> <x> <y>"
                // (target-local). Disambiguate on whether the first token is numeric.
                bool firstIsNumber = false;
                vtok(p, 1).toInt(&firstIsNumber);
                if (firstIsNumber) {
                    a.value = vtok(p, 1) + QLatin1Char(' ') + vtok(p, 2);
                } else {
                    a.target = vtok(p, 1);
                    a.value = vtok(p, 2) + QLatin1Char(' ') + vtok(p, 3);
                }
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doClickAt(a.target, a.value);
            });

        add("invoke", {}, "invoke <target> <action> [value…] — drive a control (TX-guarded)",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.target = vtok(p, 1);
                a.action = vtok(p, 2);
                a.value = vjoin(p, 3);
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.target.isEmpty() || a.action.isEmpty())
                    return err(QStringLiteral("invoke requires a target and an action"));
                return s.doInvoke(a.target, a.action, a.value);
            });

        add("get", {}, "get <model> [selector] [property] — live model snapshot",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.model = vtok(p, 1);
                a.selector = vtok(p, 2);
                a.property = vtok(p, 3);
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.model.isEmpty())
                    return err(QStringLiteral("get requires a model (")
                               + getModelNames().join(QLatin1Char('|'))
                               + QLatin1Char(')'));
                return s.doGet(a.model, a.selector, a.property);
            });

        add("connect", {}, "connect <list|show|hide|local|ip|wait> [args]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket* sock) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral(
                        "connect requires an action (list|show|hide|local|ip|wait)"));
                }
                return s.doConnect(a.action, a.value, sock);
            });

        add("disconnect", {}, "disconnect from the radio",
            parseTargetPath,
            [](AutomationServer& s, A&, QLocalSocket*) { return s.doDisconnect(); });

        add("txtest", {}, "txtest <twotone|off> — TX-gated test signal",
            parseActionOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral("txtest requires an action (twotone|off)"));
                return s.doTxTest(a.action);
            });

        add("atu", {}, "atu <bypass|start> — antenna tuner (start is TX-gated)",
            parseActionOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral("atu requires an action (bypass|start)"));
                return s.doAtu(a.action);
            });

        add("slice", {}, "slice <action> [args] — slice lifecycle/config (see doSlice)",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral(
                        "slice requires an action (add|remove|select|tx|mode|txant|rxant|rxsource)"));
                return s.doSlice(a.action, a.value);
            });

        add("gps", {},
            "gps <fixture|clearfixture> [6000|8000] — disconnected GPS test data",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral(
                        "gps requires an action (fixture|clearfixture)"));
                }
                return s.doGps(a.action, a.value);
            });

        add("waveform", {},
            "waveform <start|stop|unregister|resync> [args] — digital-voice service",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral(
                        "waveform requires an action (start|stop|unregister|resync)"));
                }
                return s.doWaveform(a.action, a.value);
            });

        add("tune", {}, "tune <mhz> [sliceId] — set a slice frequency (default: the active slice)",
            parseValueId,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.value.isEmpty())
                    return err(QStringLiteral("tune requires a frequency in MHz"));
                return s.doTune(a.value, a.id);
            });

        add("targettune", {},
            "targettune <mhz> — absolute tune through band-stack preselection",
            parseValueOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.value.isEmpty()) {
                    return err(QStringLiteral(
                        "targettune requires a frequency in MHz"));
                }
                return s.doTargetTune(a.value);
            });

        add("memory", {}, "memory activate <index> [panId] — recall a radio memory",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral("memory requires an action (activate)"));
                }
                return s.doMemory(a.action, a.value);
            });

        add("cwx", {}, "cwx <send|speed|stop> [args] — CWX keyer (send is TX-gated)",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doCwx(a.action, a.value);
            });

        add("record", {}, "record <start|stop|status|path|dir> [args]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doRecord(a.action, a.value);
            });

        add("testtone", {}, "testtone <on|off> [freqHz levelDb]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doTestTone(a.action, a.value);
            });

        add("pan", {}, "pan <create|add|remove|close|center> [value]",
            parseActionValue,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral(
                        "pan requires an action (create|add|remove|close|center)"));
                return s.doPan(a.action, a.value);
            });

        add("layout", {}, "layout <rearrange <id>|get> — splitter layout exerciser",
            parseActionValue,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral("layout requires an action (rearrange|get)"));
                }
                return s.doLayout(a.action, a.value);
            });

        add("scale", {}, "scale [pct] — report/persist the UI scale factor",
            parseValueOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doScale(a.value);
            });

        add("panmessage", {},
            "panmessage <add|remove|clear|list> <pan> [id timeout [tone=…] title|detail]",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.action = vtok(p, 1);            // add | remove | clear | list
                a.target = vtok(p, 2);            // pan index | active
                a.id = vtok(p, 3);                // message id for add/remove
                if (a.action == QLatin1String("add")) {
                    bool okTimeout = false;
                    a.timeoutMs = vtok(p, 4).toInt(&okTimeout);
                    int textStart = 5;
                    if (!okTimeout) {
                        // Timeout omitted — tok(4) is the first text token, not a
                        // number; don't swallow it into the failed parse. (#3999 review)
                        a.timeoutMs = 0;
                        textStart = 4;
                    }
                    if (vtok(p, textStart).startsWith(QStringLiteral("tone="),
                                                      Qt::CaseInsensitive)) {
                        a.tone = vtok(p, textStart).section(QLatin1Char('='), 1).trimmed();
                        ++textStart;
                    }
                    const QString text = vjoin(p, textStart);
                    const int sep = text.indexOf(QLatin1Char('|'));
                    if (sep >= 0) {
                        a.title = text.left(sep).trimmed();
                        a.detail = text.mid(sep + 1).trimmed();
                    } else {
                        a.title = text.trimmed();
                        a.detail.clear();
                    }
                }
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral(
                        "panmessage requires an action (add|remove|clear|list)"));
                }
                return s.doPanMessage(a.action, a.target, a.id, a.title, a.detail,
                                      a.timeoutMs, a.tone);
            });

        add("dss", {}, "dss <snapshot|reset|inject|scrollback|live> [pan] [args]",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.action = vtok(p, 1);
                a.target = vtok(p, 2);            // pan index, optional for snapshot
                a.value = vjoin(p, 3);
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty()) {
                    return err(QStringLiteral(
                        "dss requires an action (snapshot|reset|inject|scrollback|live)"));
                }
                return s.doDss(a.action, a.target.isEmpty() ? a.selector : a.target,
                               a.value);
            });

        add("streams", {}, "streams [radio|inventory|resync|refresh|reset] — stream diagnostics",
            parseActionOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doStreams(a.action);
            });

        add("tci", {},
            "tci start|status|stop — in-process TCI client simulator (JSON form only)",
            // Historical quirk, preserved (#4174): tci never had a bare-line
            // parse arm, so the default target/path fill leaves `action` empty
            // and bare "tci start" reports the usage error below. The JSON
            // form supplies action/value explicitly.
            parseTargetPath,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral(
                        "tci requires an action (start [port|sdc [port]] | status | stop [abrupt])"));
                return s.doTci(a.action, a.value);
            });

        add("audioCapture", {},
            "audioCapture <start|stop|status|read|probeNr2Stereo|probeDspStereo> [args]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doAudioCapture(a.action.isEmpty() ? QStringLiteral("status")
                                                           : a.action,
                                        a.value, a.path);
            });

        add("txwaterfall", {}, "txwaterfall <on|off> — show keyed TX in the waterfall",
            parseValueOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                // Radio-authoritative display flag (`transmit set show_tx_in_waterfall`)
                // that gates whether keyed-up TX renders FFT-derived rows in the
                // waterfall. Off by default; enabling it lets a test confirm CWX/tune/ATU
                // energy appears in the waterfall, not just the FFT trace (#3646/#3804).
                if (!s.m_radioModel)
                    return err(QStringLiteral("no radio model available"));
                const QString v = a.value.trimmed().toLower();
                const bool on = (v == QLatin1String("on") || v == QLatin1String("1")
                                 || v == QLatin1String("true") || v == QLatin1String("enable"));
                const bool off = (v == QLatin1String("off") || v == QLatin1String("0")
                                  || v == QLatin1String("false") || v == QLatin1String("disable"));
                if (!on && !off)
                    return err(QStringLiteral("txwaterfall requires on|off"));
                s.m_radioModel->sendCommand(
                    QStringLiteral("transmit set show_tx_in_waterfall=%1").arg(on ? 1 : 0));
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("txwaterfall"), on},
                    {QStringLiteral("note"),
                     QStringLiteral("radio echoes status; re-read with get transmit showTxInWaterfall")}};
            });

        add("key", {}, "key <ptt on|off | mox> — semantic keying (TX-gated)",
            parseActionValue,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.action.isEmpty())
                    return err(QStringLiteral("key requires a name (ptt on|off, mox)"));
                return s.doKey(a.action, a.value);
            });

        add("station", {}, "station <name> — set the GUI-client station name",
            parseValueOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.value.isEmpty())
                    return err(QStringLiteral("station requires a name"));
                return s.doStation(a.value);
            });

        add("resize", {}, "resize <w> <h> [target] — resize a window",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.value = vtok(p, 1) + QLatin1Char(' ') + vtok(p, 2);
                a.target = vtok(p, 3);
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doResize(a.value, a.target);
            });

        add("window", {}, "window <maximize|restore|minimize|fullscreen> [target]",
            [](const QList<QByteArray>& p, A& a) -> QJsonObject {
                a.action = vtok(p, 1);
                a.target = vtok(p, 2);   // optional window target
                return {};
            },
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doWindow(a.action, a.target);
            });

        add("shortcut", {}, "shortcut <id> — fire a ShortcutManager/MIDI action (TX-gated)",
            parseTargetOnly,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doShortcut(a.target.isEmpty() ? a.id : a.target);
            });

        add("midi", {}, "midi cc <0-127> — inject a learned VFO Tune Knob CC event",
            parseActionValue,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doMidi(a.action, a.value);
            });

        add("menu", {}, "menu list | open <name> — menu-bar menus",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doMenu(a.action.isEmpty() ? QStringLiteral("list") : a.action,
                                a.value);
            });

        add("whoami", {}, "bridge instance info: pid, socket, label, station, txAllowed",
            parseTargetPath,
            [](AutomationServer& s, A&, QLocalSocket*) { return s.doWhoami(); });

        add("log", {}, "log <categories|get|set|reset|tail|subscribe|unsubscribe> [args]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket* sock) {
                return s.doLog(a.action.isEmpty() ? QStringLiteral("categories")
                                                  : a.action,
                               a.value, sock);
            });

        add("mark", {}, "mark <text> — timestamped annotation in the log ring",
            parseValueRest,
            [](AutomationServer& s, A& a, QLocalSocket*) -> QJsonObject {
                if (a.value.isEmpty())
                    return err(QStringLiteral("mark requires annotation text"));
                return s.doMark(a.value);
            });

        add("qrz", {}, "qrz <status|cached|lookup|spottext> [args]",
            parseActionRest,
            [](AutomationServer& s, A& a, QLocalSocket*) {
                return s.doQrz(a.action.isEmpty() ? QStringLiteral("status") : a.action,
                               a.value);
            });

        return v;
    }();
    return kVerbs;
}

const AutomationServer::VerbSpec* AutomationServer::findVerb(const QString& cmd)
{
    static const QHash<QString, const VerbSpec*> kIndex = [] {
        QHash<QString, const VerbSpec*> idx;
        for (const VerbSpec& spec : verbRegistry()) {
            idx.insert(spec.name, &spec);
            for (const QString& alias : spec.aliases)
                idx.insert(alias, &spec);
        }
        return idx;
    }();
    return kIndex.value(cmd, nullptr);
}

QString AutomationServer::verbNamesJoined()
{
    QStringList names;
    for (const VerbSpec& spec : verbRegistry())
        names << spec.name;
    return names.join(QStringLiteral(", "));
}

QJsonObject AutomationServer::handleLine(const QByteArray& line, QLocalSocket* sock)
{
    QString cmd;
    VerbArgs a;

    const QByteArray trimmed = line.trimmed();
    if (trimmed.startsWith('{')) {
        // JSON request, e.g.
        //   {"cmd":"invoke","target":"masterVolume","action":"setValue","value":"30"}
        //   {"cmd":"get","model":"slice","selector":"active","property":"frequency"}
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
            return err(QStringLiteral("invalid JSON: ") + perr.errorString());
        const QJsonObject obj = doc.object();
        cmd        = obj.value(QStringLiteral("cmd")).toString();
        a.target   = obj.value(QStringLiteral("target")).toString();
        a.path     = obj.value(QStringLiteral("path")).toString();
        a.action   = obj.value(QStringLiteral("action")).toString();
        // value may be a string, number, or bool — normalize to text.
        const QJsonValue v = obj.value(QStringLiteral("value"));
        if (v.isString())      a.value = v.toString();
        else if (v.isDouble()) a.value = QString::number(v.toDouble());
        else if (v.isBool())   a.value = v.toBool() ? QStringLiteral("true")
                                                    : QStringLiteral("false");
        a.model    = obj.value(QStringLiteral("model")).toString();
        a.selector = obj.value(QStringLiteral("selector")).toString();
        a.property = obj.value(QStringLiteral("property")).toString();
        // id may arrive as a JSON number (e.g. tune's slice id) — normalize
        // like `value` above; a bare .toString() would silently coerce a
        // numeric id to "" and the request would act on the wrong target.
        const QJsonValue idv = obj.value(QStringLiteral("id"));
        if (idv.isString()) {
            a.id = idv.toString();
        } else if (idv.isDouble()) {
            // Keep enough precision for downstream integer validation. The
            // default six significant digits can round 1.0000001 to "1" and
            // silently retarget a request to a real slice.
            a.id = QString::number(idv.toDouble(), 'g',
                                   std::numeric_limits<double>::max_digits10);
        } else if (obj.contains(QStringLiteral("id"))) {
            // An omitted id intentionally selects the active/default target;
            // an explicitly malformed id must not collapse to that sentinel.
            return err(QStringLiteral("id must be a string or number"));
        }
        a.title    = obj.value(QStringLiteral("title")).toString();
        a.detail   = obj.value(QStringLiteral("detail")).toString();
        a.tone     = obj.value(QStringLiteral("tone")).toString();
        a.token    = obj.value(QStringLiteral("token")).toString();
        a.timeoutMs = obj.value(QStringLiteral("timeoutMs")).toInt(0);
        // Authenticated clients cannot use a bare request because the token is
        // a JSON field. Let them keep the registry's positional protocol via
        // {"cmd":"...","args":"...","token":"..."} rather than forcing
        // every generic bridge client to duplicate all verb-specific mappings.
        const QJsonValue positionalArgs = obj.value(QStringLiteral("args"));
        if (!positionalArgs.isUndefined()) {
            if (!positionalArgs.isString()) {
                return err(QStringLiteral("JSON args must be a string"));
            }
            if (const VerbSpec* spec = findVerb(cmd)) {
                QByteArray bareRequest = cmd.toUtf8();
                const QByteArray args = positionalArgs.toString().toUtf8().trimmed();
                if (!args.isEmpty()) {
                    bareRequest.append(' ');
                    bareRequest.append(args);
                }
                const QJsonObject parseError = spec->parse(bareRequest.split(' '), a);
                if (!parseError.isEmpty()) {
                    return parseError;
                }
            }
        }
        // clickAt accepts numeric x/y fields directly (dumpTree geometry is
        // global), folded into `value` as "x y" so both request forms share one
        // code path. Explicit `value` still wins if supplied. Fold ONLY when
        // both fields are present and JSON-numeric: toInt() coerces a missing
        // field or a string-typed number to 0, which would turn a malformed
        // request into a real click at the screen edge (or (0,0)) instead of
        // an error — leave value empty so doClickAt rejects it. Match both
        // alias spellings, like the registry does.
        if ((cmd == QLatin1String("clickAt") || cmd == QLatin1String("clickat"))
            && a.value.isEmpty()
            && obj.value(QStringLiteral("x")).isDouble()
            && obj.value(QStringLiteral("y")).isDouble()) {
            a.value = QString::number(obj.value(QStringLiteral("x")).toInt())
                      + QLatin1Char(' ')
                      + QString::number(obj.value(QStringLiteral("y")).toInt());
        }
    } else {
        // Bare line: positional tokens, parsed by the verb's registry entry
        // (e.g. "invoke <target> <action> [value…]"). Unknown verbs skip the
        // parse and fall through to the shared unknown-command error below.
        const QList<QByteArray> p = trimmed.split(' ');
        cmd = QString::fromUtf8(p.value(0));
        if (const VerbSpec* spec = findVerb(cmd)) {
            const QJsonObject parseError = spec->parse(p, a);
            if (!parseError.isEmpty())
                return parseError;
        }
    }

    qCDebug(lcAutomation) << "request:" << cmd << a.target << a.action << a.model
                          << a.selector;

    const VerbSpec* spec = findVerb(cmd);
    if (!spec)
        return err(QStringLiteral("unknown command: ") + cmd);

    // Shared-secret gate (#3646). When a token is configured, every verb
    // except `ping` must present a matching `token` field. The Unix socket /
    // named pipe only enforces same-user access; the token is what stops a
    // *different* local process (a stray agent) from driving the radio once
    // the operator has opted a specific client in via Radio Setup -> Network.
    // ping stays open (see its registry entry, which reports authRequired) so
    // a client can detect the bridge without holding the secret. Constant-time
    // compare so a wrong token leaks no length/prefix timing signal.
    if (!m_authToken.isEmpty() && cmd != QLatin1String("ping")) {
        const QByteArray want = m_authToken.toUtf8();
        const QByteArray got = a.token.toUtf8();
        // Never index `got` out of range — when it's shorter (or empty),
        // substitute a zero byte rather than reading got[0], which segfaults
        // on an empty QByteArray.
        int diff = static_cast<int>(want.size() ^ got.size());
        for (int i = 0; i < want.size(); ++i) {
            const char g = (i < got.size()) ? got.at(i) : char(0);
            diff |= want.at(i) ^ g;
        }
        if (diff != 0) {
            qCWarning(lcAutomation)
                << "rejected unauthenticated request:" << cmd
                << "(missing or invalid token)";
            return err(QStringLiteral(
                "unauthorized: this bridge requires a token. Set AETHER_MCP_TOKEN "
                "in your MCP client from Radio Setup -> Network -> Agent "
                "Automation."));
        }
    }

    // Observe-only gate (#4188 area 6). When the operator has enabled
    // read-only mode (Radio Setup -> Network -> "Observe only"), refuse any
    // verb that isn't pure introspection — no driving, no connect/capture, no
    // keying. Enforced HERE in the bridge (not the MCP client) so it can't be
    // bypassed by talking to the socket directly. Uses the resolved canonical
    // name so aliases are covered.
    if (m_readOnly && !isReadOnlyRequest(spec->name, a.action)) {
        qCWarning(lcAutomation) << "read-only mode: refused" << spec->name;
        return err(QStringLiteral("read-only mode: '") + spec->name
                   + QStringLiteral("' is blocked. This bridge is observe-only "
                                    "— uncheck \"Observe only\" in Radio Setup "
                                    "-> Network to allow driving."));
    }

    return spec->dispatch(*this, a, sock);
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

// Lightweight read of every spectrum's measured noise floors, keyed by
// panIndex. dumpTree serialises ~2600 widgets (~250 ms) which is far too slow
// to sample a sub-second post-TX transient; this only touches widgets that
// expose the noiseFloorDbm Q_PROPERTY and builds a tiny payload, so a driver
// can poll it at 20+ Hz to trace floor recovery (#3804/#3646).
QJsonObject AutomationServer::doFloors() const
{
    QJsonArray arr;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* tlw : tops) {
        QList<QWidget*> scan = tlw->findChildren<QWidget*>();
        scan.prepend(tlw);
        for (QWidget* w : scan) {
            const QVariant nf = w->property("noiseFloorDbm");
            if (!nf.isValid())
                continue;   // not a spectrum — property absent
            QJsonObject o;
            o[QStringLiteral("panIndex")] = w->property("panIndex").toInt();
            o[QStringLiteral("visible")]  = w->isVisible();
            if (nf.toDouble() > -500.0)
                o[QStringLiteral("noiseFloorDbm")] = nf.toDouble();
            const QVariant df = w->property("displayFloorDbm");
            if (df.isValid() && df.toDouble() > -500.0)
                o[QStringLiteral("displayFloorDbm")] = df.toDouble();
            arr.append(o);
        }
    }
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("floors"), arr}};
}

QJsonObject AutomationServer::doGrab(const QString& target, const QString& path) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("widget not found: ") + target}};
    }
    return saveWidgetGrab(w, target, path);
}

// Capture a specific pan's spectrum surface by SpectrumWidget::panIndex, so a
// driver can grab pan 1 (etc.) in a multi-pan layout instead of always getting
// the first SpectrumWidget. panIndex is read via the meta-object (Q_PROPERTY) so
// core/ stays free of any GUI include. (#3646)
QJsonObject AutomationServer::doGrabPan(const QString& indexStr, const QString& path) const
{
    bool okIdx = false;
    const int index = indexStr.toInt(&okIdx);
    if (!okIdx || index < 0)
        return err(QStringLiteral("grab pan requires a non-negative pan index "
                                  "(e.g. 'grab pan 1')"));

    QJsonArray available;
    QWidget* match = panSpectrumWidgetForIndex(index, &available);
    if (!match) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("no pan with index ") + QString::number(index)},
                           {QStringLiteral("available"), available}};
    }

    QJsonObject r = saveWidgetGrab(match, QStringLiteral("pan") + QString::number(index), path);
    if (r.value(QStringLiteral("ok")).toBool())
        r[QStringLiteral("panIndex")] = index;
    return r;
}

// Capture the whole visible pan applet by index, not just the raw GPU surface.
// This is the screenshot agents usually want for UI overlays: VFO flags,
// side buttons, and child widgets are painted above the SpectrumWidget and are
// therefore absent from `grab pan`, which intentionally returns only the
// QRhiWidget framebuffer.
QJsonObject AutomationServer::doGrabPanVisible(const QString& indexStr,
                                               const QString& path) const
{
    bool okIdx = false;
    const int index = indexStr.toInt(&okIdx);
    if (!okIdx || index < 0) {
        return err(QStringLiteral("grab pan-visible requires a non-negative pan index "
                                  "(e.g. 'grab pan-visible 1')"));
    }

    QJsonArray available;
    QWidget* spectrum = panSpectrumWidgetForIndex(index, &available);
    if (!spectrum) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("no pan with index ") + QString::number(index)},
                           {QStringLiteral("available"), available}};
    }

    QWidget* applet = panadapterAppletForSpectrum(spectrum);
    if (!applet) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("pan ") + QString::number(index)
                                + QStringLiteral(" has no PanadapterApplet ancestor")}};
    }

    QJsonObject r = saveWidgetGrab(applet,
                                   QStringLiteral("pan-visible") + QString::number(index),
                                   path);
    if (r.value(QStringLiteral("ok")).toBool()) {
        r[QStringLiteral("panIndex")] = index;
        r[QStringLiteral("surfaceClass")] = shortClassName(spectrum);
    }
    return r;
}

// Shared tail for grab / grab pan: read the widget back to a PNG (framebuffer
// readback for the GPU panadapter) and describe the result. `label` names the
// default temp file and is echoed as `target`.
QJsonObject AutomationServer::saveWidgetGrab(QWidget* w, const QString& label,
                                             const QString& path) const
{
    const QImage img = grabWidget(w);
    if (img.isNull()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("grab produced an empty image for ") + label}};
    }

    QString outPath = path;
    if (outPath.isEmpty()) {
        QString safe = label;
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
        {QStringLiteral("target"), label},
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

    // VFO shortcuts: "vfo slice 1" resolves the flag for slice 1, and
    // "vfo 1" resolves the first flag inside pan index 1. The slice form is
    // preferred when a pan contains multiple VFOs.
    if (QWidget* vfo = resolveVfoSelector(target)) {
        return vfo;
    }

    static const QRegularExpression panScopeRe(
        QStringLiteral("^pan(?:-visible)?(?:\\s+|:)(\\d+)/(.*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch panScopeMatch =
        panScopeRe.match(target.trimmed());
    if (panScopeMatch.hasMatch()) {
        bool okIndex = false;
        const int panIndex = panScopeMatch.captured(1).toInt(&okIndex);
        const QString inner = panScopeMatch.captured(2);
        if (okIndex && !inner.isEmpty()) {
            if (QWidget* spectrum = panSpectrumWidgetForIndex(panIndex)) {
                if (QWidget* applet = panadapterAppletForSpectrum(spectrum)) {
                    if (QWidget* m = resolveWithinScopes({applet}, inner)) {
                        return m;
                    }
                }
            }
        }
    }

    // 0. Scoped target "<scope>/<name>" disambiguates duplicate accessibleNames
    //    across applets — e.g. "RxApplet/AF gain" vs "PanadapterApplet/AF gain",
    //    which both exist and would otherwise both resolve to whichever comes
    //    first in tree order. <scope> matches an ancestor by objectName, class
    //    (short or full), or accessibleName; <name> is resolved within that
    //    subtree. Falls through to flat resolution if it doesn't resolve, so a
    //    literal '/' in a control name still works.
    const int slash = target.indexOf(QLatin1Char('/'));
    if (slash > 0) {
        const QString scope = target.left(slash);
        const QString inner = target.mid(slash + 1);
        QList<QWidget*> scopes;
        for (QWidget* tlw : tops) {
            collectObjectNameMatches(tlw, scope, scopes);
            collectIdentityMatches(tlw, scope, scopes);
        }

        if (QWidget* m = resolveWithinScopes(scopes, inner)) {
            return m;
        }
    }

    // 1. Exact objectName. Duplicate object names exist in hidden menus and
    //    applet clones, so prefer visible/enabled matches but keep hidden
    //    widgets addressable when no visible match exists.
    QList<QWidget*> matches;
    for (QWidget* tlw : tops) {
        collectObjectNameMatches(tlw, target, matches);
    }
    if (QWidget* m = preferredWidget(matches)) {
        return m;
    }
    // 2. Class name or accessibleName (e.g. "SpectrumWidget" for the panadapter).
    matches.clear();
    for (QWidget* tlw : tops) {
        collectIdentityMatches(tlw, target, matches);
    }
    if (QWidget* m = preferredWidget(matches)) {
        return m;
    }
    // 3. Button visible text, last resort (e.g. "Send", "Transmit").
    matches.clear();
    for (QWidget* tlw : tops) {
        collectButtonTextMatches(tlw, target, matches);
    }
    if (QWidget* m = preferredWidget(matches)) {
        return m;
    }
    return nullptr;
}

QJsonObject AutomationServer::doInvoke(const QString& target, const QString& action,
                                       const QString& value) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        // An open menu wins (it carries live geometry for a real mouse trigger);
        // otherwise reach the action in a CLOSED menu bar so menu-launched
        // dialogs and Zoom are drivable without first popping the menu. (#3646)
        ResolvedAction resolved = resolveVisibleAction(target);
        if (!resolved.action)
            resolved = resolveMenuBarAction(target);
        QAction* menuAction = resolved.action;
        QMenu* menu = resolved.menu;
        if (!menuAction) {
            return err(QStringLiteral("widget or menu action not found: ") + target);
        }

        if (menuAction->isSeparator()) {
            return err(QStringLiteral("action '") + target + QStringLiteral("' is a separator"));
        }
        if (!menuAction->isEnabled()) {
            return err(QStringLiteral("action '") + target + QStringLiteral("' is disabled"));
        }

        if (isTransmitAction(menuAction, menu) && !m_txAllowed) {
            qCWarning(lcAutomation).noquote()
                << "BLOCKED transmit-related QAction invoke on" << target;
            return err(QStringLiteral("blocked: '") + target
                       + QStringLiteral("' is a transmit-keying action (TX-safety guard). "
                                        "Enable \"Allow TX via MCP\" in Radio Setup → Network "
                                        "(or set AETHER_AUTOMATION_ALLOW_TX=1) to override."));
        }

        QPointer<QAction> actionGuard = menuAction;
        const QString text = actionDisplayText(menuAction);
        const QString data = actionDataText(menuAction);
        bool done = false;
        bool deferred = false;
        if (action == QLatin1String("trigger") || action == QLatin1String("click")
            || action == QLatin1String("toggle")) {
            // CRASH-SAFETY: a menu action can open a MODAL dialog (dlg.exec() —
            // Configure Shortcuts, Slice Troubleshooting, Memory, Profile
            // Manager, …). exec() spins a nested event loop; running it
            // synchronously here would re-enter the event loop INSIDE the
            // QLocalSocket read callback (qt_mac_socket_callback ->
            // canReadNotification -> handleLine), corrupting the socket notifier
            // and segfaulting on a later readyRead. It would also block the
            // response until the dialog closed. Defer the trigger to a clean
            // main-loop turn so any nested dialog loop runs on a normal stack.
            // (#3646 fidelity — re-entrancy crash fix)
            QPointer<QAction> ag = menuAction;
            QPointer<QMenu> mg = menu;
            QTimer::singleShot(0, qApp, [ag, mg]() {
                if (!ag) return;
                // Activate the main window first so a menu action that opens a
                // dialog / pops a menu has a valid active window (avoids the
                // backgrounded null-QWindow popup crash). (#3646 follow-up)
                raiseWindowForPopup(primaryTopLevelWindow());
                triggerMenuAction(ag, mg);
            });
            done = true;
            deferred = true;
        } else if (action == QLatin1String("setChecked")) {
            if (!menuAction->isCheckable()) {
                return err(QStringLiteral("action '") + target
                           + QStringLiteral("' is not checkable"));
            }
            menuAction->setChecked(parseBool(value));
            done = true;
        } else {
            return err(QStringLiteral("unknown QAction action: ") + action
                       + QStringLiteral(" (use trigger|click|toggle|setChecked)"));
        }

        if (!done) {
            return err(QStringLiteral("failed to invoke QAction: ") + target);
        }

        qCInfo(lcAutomation).noquote()
            << "invoke" << action << "on" << target << "(QAction)";

        QJsonObject r{
            {QStringLiteral("ok"), true},
            {QStringLiteral("target"), target},
            {QStringLiteral("class"), QStringLiteral("QAction")},
            {QStringLiteral("action"), action},
        };
        if (!text.isEmpty()) {
            r[QStringLiteral("text")] = text;
        }
        if (!data.isEmpty()) {
            r[QStringLiteral("data")] = data;
        }
        if (deferred) {
            // The trigger runs on the next main-loop turn (it may open a modal
            // dialog), so any post-state must be re-read (dumpTree / menu list)
            // rather than trusted from this synchronous reply.
            r[QStringLiteral("deferred")] = true;
        } else if (actionGuard) {
            const QString nv = actionValue(actionGuard);
            if (!nv.isNull()) {
                r[QStringLiteral("newValue")] = nv;
            }
        }
        return r;
    }

    if (!w->isVisible()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: '") + target
                                + QStringLiteral("' is not visible")},
                           {QStringLiteral("hidden"), true},
                           {QStringLiteral("class"), shortClassName(w)}};
    }

    // Refuse to drive a disabled control. Qt's setValue()/setChecked() still
    // mutate a disabled widget, so without this the bridge would report a happy
    // newValue while the radio never sees the change (the control is greyed out
    // for a reason — wrong mode, not connected, etc.). Surfacing it as an error
    // turns a silent no-op into an explicit, assertable signal. (#3646)
    if (!w->isEnabled()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: '") + target
                                + QStringLiteral("' is disabled — the radio won't accept the change")},
                           {QStringLiteral("disabled"), true},
                           {QStringLiteral("class"), shortClassName(w)}};
    }

    // TX-safety guard — never key a live radio from the test bridge unless the
    // operator has explicitly opted in. (#3646 Phase 1 safety requirement.)
    if (isTransmitControl(w) && !m_txAllowed) {
        qCWarning(lcAutomation).noquote()
            << "BLOCKED transmit-related invoke on" << target
            << "(" << shortClassName(w) << ")";
        return err(QStringLiteral("blocked: '") + target
                   + QStringLiteral("' is a transmit-keying control (TX-safety guard). "
                                    "Enable \"Allow TX via MCP\" in Radio Setup → Network "
                                    "(or set AETHER_AUTOMATION_ALLOW_TX=1) to override."));
    }

    // Power-ceiling rail (#3646): clamp RF/Tune power setpoints to the
    // configured max (AETHER_AUTOMATION_TX_MAX_POWER) so automation can't
    // command more power than the connected load can take.
    QString effValue = value;
    if (m_txMaxPower >= 0 && action == QLatin1String("setValue")) {
        const QString an = w->accessibleName();
        if (an == QLatin1String("RF power") || an == QLatin1String("Tune power")) {
            bool okN = false;
            const int n = value.toInt(&okN);
            if (okN && n > m_txMaxPower) {
                effValue = QString::number(m_txMaxPower);
                qCWarning(lcAutomation).noquote()
                    << "power ceiling: clamped" << an << n << "->" << m_txMaxPower;
            }
        }
    }

    bool done = false;
    bool deferred = false;
    QString selectedRowText;   // selectRow: first-column text of the chosen row
    int     selectedRow = -1;
    if (action == QLatin1String("click") || action == QLatin1String("toggle")) {
        // CRASH-SAFETY: a button click can open a popup menu (a QToolButton with
        // a dropdown) or a modal dialog, both of which spin a NESTED event loop.
        // Running that synchronously here re-enters the event loop INSIDE the
        // QLocalSocket read callback (qt_mac_socket_callback -> canReadNotification
        // -> handleLine), corrupting socket/window state — observed SIGSEGV:
        // QToolButton popup -> QMenu::exec -> QWindow::geometry() on a null
        // window (worse when the app is backgrounded). Defer to a clean main-loop
        // turn so any nested loop runs on a normal stack. Mirrors the merged
        // menu-action trigger fix; the widget path was its latent sibling.
        // (#3646 follow-up — re-entrancy crash)
        if (auto* b = qobject_cast<QAbstractButton*>(w)) {
            const bool useToggle =
                action == QLatin1String("toggle") && b->isCheckable();
            QPointer<QAbstractButton> bg = b;
            QPointer<QWidget> win = b->window();
            QTimer::singleShot(0, qApp, [bg, win, useToggle]() {
                if (!bg) return;
                // Activate the button's window first so a popup menu it raises
                // has a valid active window (backgrounded automation otherwise
                // segfaults in QWindow::geometry()). (#3646 follow-up)
                raiseWindowForPopup(win);
                if (useToggle) bg->toggle();
                else           bg->click();
            });
            done = true;
            deferred = true;
        }
    } else if (action == QLatin1String("setChecked")) {
        if (auto* b = qobject_cast<QAbstractButton*>(w); b && b->isCheckable()) {
            b->setChecked(parseBool(value)); done = true;
        }
    } else if (action == QLatin1String("setValue")) {
        bool okNum = false;
        const int n = effValue.toInt(&okNum);
        if (auto* s = qobject_cast<QAbstractSlider*>(w)) {
            if (!okNum) return err(QStringLiteral("setValue needs an integer"));
            s->setValue(n); done = true;
        } else if (auto* sb = qobject_cast<QSpinBox*>(w)) {
            if (!okNum) return err(QStringLiteral("setValue needs an integer"));
            sb->setValue(n); done = true;
        } else if (auto* ds = qobject_cast<QDoubleSpinBox*>(w)) {
            bool okD = false; const double d = effValue.toDouble(&okD);
            if (!okD) return err(QStringLiteral("setValue needs a number"));
            ds->setValue(d); done = true;
        }
    } else if (action == QLatin1String("wheel")) {
        bool okNum = false;
        const int steps = value.isEmpty() ? 1 : value.toInt(&okNum);
        if (!value.isEmpty() && !okNum) {
            return err(QStringLiteral("wheel needs an integer step count"));
        }
        if (steps == 0 || steps < -1 || steps > 1) {
            return err(QStringLiteral("wheel accepts one notch per call (-1 or 1)"));
        }
        const QPoint pos(w->width() / 2, w->height() / 2);
        const QPoint globalPos = w->mapToGlobal(pos);
        QWheelEvent ev(QPointF(pos), QPointF(globalPos),
                       QPoint(), QPoint(0, steps * 120),
                       Qt::NoButton, Qt::NoModifier,
                       Qt::ScrollUpdate, false);
        QCoreApplication::sendEvent(w, &ev);
        done = true;
    } else if (action == QLatin1String("setText")) {
        if (auto* le = qobject_cast<QLineEdit*>(w)) { le->setText(value); done = true; }
    } else if (action == QLatin1String("submit")) {
        // Commit a line edit: optionally set the value, then fire returnPressed —
        // the signal that actually drives the action (a frequency retune, etc.).
        // setText alone is intentionally side-effect-free, because other
        // bridge-reachable fields (SmartLink login, manual-connect host, DX
        // cluster command) attach irreversible actions to returnPressed; commit
        // must be an explicit, opt-in verb. (#3646 fidelity — item 6)
        if (auto* le = qobject_cast<QLineEdit*>(w)) {
            if (!value.isEmpty()) le->setText(value);
            emit le->returnPressed();
            done = true;
        }
    } else if (action == QLatin1String("setCurrentText")) {
        if (auto* cb = qobject_cast<QComboBox*>(w)) { cb->setCurrentText(value); done = true; }
        else if (auto* tb = qobject_cast<QTabBar*>(w)) {
            // Select a tab by its label — the only way to reach deferred
            // setup-dialog tabs (built on first selection) from the bridge.
            for (int i = 0; i < tb->count(); ++i) {
                if (tb->tabText(i).compare(value, Qt::CaseInsensitive) == 0) {
                    tb->setCurrentIndex(i);
                    done = true;
                    break;
                }
            }
            if (!done)
                return err(QStringLiteral("no tab labeled '%1'").arg(value));
        } else if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            QAbstractItemModel* model = view->model();
            if (!model) {
                return err(QStringLiteral("view has no model"));
            }

            std::function<QModelIndex(const QModelIndex&)> findItem;
            findItem = [&](const QModelIndex& parent) -> QModelIndex {
                const int rows = model->rowCount(parent);
                for (int row = 0; row < rows; ++row) {
                    const QModelIndex index = model->index(row, 0, parent);
                    if ((model->flags(index) & Qt::ItemIsSelectable)
                        && model->data(index, Qt::DisplayRole).toString()
                            .compare(value, Qt::CaseInsensitive) == 0) {
                        return index;
                    }
                    const QModelIndex childMatch = findItem(index);
                    if (childMatch.isValid()) {
                        return childMatch;
                    }
                }
                return {};
            };

            const QModelIndex match = findItem({});
            if (!match.isValid()) {
                return err(QStringLiteral("no item labeled '%1'").arg(value));
            }
            view->setCurrentIndex(match);
            view->scrollTo(match, QAbstractItemView::PositionAtCenter);
            done = true;
        }
    } else if (action == QLatin1String("setCurrentIndex")) {
        if (auto* cb = qobject_cast<QComboBox*>(w)) { cb->setCurrentIndex(value.toInt()); done = true; }
        else if (auto* tb = qobject_cast<QTabBar*>(w)) { tb->setCurrentIndex(value.toInt()); done = true; }
    } else if (action == QLatin1String("selectRow")) {
        // Select a whole row in an item view (QTableWidget / QTreeWidget /
        // QListWidget / any QAbstractItemView) so the dialog's row-scoped
        // buttons (Tune / Edit / Remove / Disable) — which read the view's
        // current row or selection — become drivable. invoke click on those
        // buttons was useless without first selecting a row. (#3918)
        if (auto* view = qobject_cast<QAbstractItemView*>(w)) {
            QAbstractItemModel* m = view->model();
            if (!m) return err(QStringLiteral("view has no model"));
            bool okRow = false;
            const int row = value.toInt(&okRow);
            if (!okRow) return err(QStringLiteral("selectRow needs an integer row index"));
            const int rows = m->rowCount();
            if (row < 0 || row >= rows)
                return err(QStringLiteral("row %1 out of range [0,%2)")
                               .arg(row).arg(rows));
            const QModelIndex first = m->index(row, 0);
            const int lastCol = m->columnCount() > 0 ? m->columnCount() - 1 : 0;
            const QModelIndex last = m->index(row, lastCol);
            // Set both the current index and a full-row selection so handlers
            // that read currentRow()/currentItem() AND those that read
            // selectedItems()/selectionModel() all see the choice.
            if (QItemSelectionModel* sm = view->selectionModel())
                sm->select(QItemSelection(first, last),
                           QItemSelectionModel::ClearAndSelect
                               | QItemSelectionModel::Rows);
            view->setCurrentIndex(first);
            view->scrollTo(first);
            selectedRow = row;
            selectedRowText = m->data(first, Qt::DisplayRole).toString();
            done = true;
        }
    } else {
        return err(QStringLiteral("unknown action: ") + action);
    }

    if (!done)
        return err(QStringLiteral("action '") + action + QStringLiteral("' not applicable to ")
                   + shortClassName(w));

    qCInfo(lcAutomation).noquote()
        << "invoke" << action << "on" << target << "(" << shortClassName(w) << ")";

    QJsonObject r{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("action"), action},
    };
    if (deferred) {
        // click/toggle run on the next main-loop turn (they may open a popup or
        // dialog), so any post-state must be re-read (get / dumpTree) rather than
        // trusted from this synchronous reply.
        r[QStringLiteral("deferred")] = true;
    } else if (selectedRow >= 0) {
        // selectRow: echo the chosen row + its first-column text so the driver
        // can confirm the right entry is selected before firing a row action.
        r[QStringLiteral("selectedRow")] = selectedRow;
        if (!selectedRowText.isEmpty())
            r[QStringLiteral("selectedRowText")] = selectedRowText;
    } else {
        const QString nv = widgetValue(w);   // round-trip confirmation
        if (!nv.isNull())
            r[QStringLiteral("newValue")] = nv;
    }
    return r;
}

void AutomationServer::setClockModel(AetherClockModel* model)
{
    m_clockModel = model;
}

namespace {
// AetherClock model snapshot for "get clock" (PRD-A: bridge exposure).
QJsonObject clockSnapshot(const AetherClockModel* m)
{
    return QJsonObject{
        {QStringLiteral("state"), m->state()},
        {QStringLiteral("stateName"), m->stateName()},
        {QStringLiteral("station"), m->station()},
        {QStringLiteral("stationName"), m->stationName()},
        {QStringLiteral("decodedUtc"),
         m->decodedUtc().isValid()
             ? m->decodedUtc().toUTC().toString(Qt::ISODateWithMs)
             : QString{}},
        {QStringLiteral("offsetMs"), m->offsetMs()},
        {QStringLiteral("lockQuality"), m->lockQuality()},
        {QStringLiteral("sliceId"), m->sliceId()},
        {QStringLiteral("gpsTimeAvailable"), m->gpsTimeAvailable()},
        // WS-7 acquisition telemetry (additive — existing consumers see the
        // original keys unchanged). delayEstMs is NaN when the decoder has no
        // estimate; QJsonValue maps NaN to null.
        {QStringLiteral("toneSnrDb"), m->toneSnrDb()},
        {QStringLiteral("pwmContrast"), m->pwmContrast()},
        {QStringLiteral("toneDetected"), m->toneDetected()},
        {QStringLiteral("phaseLocked"), m->phaseLocked()},
        {QStringLiteral("delayEstMs"), m->delayEstMs()},
        {QStringLiteral("anchored"), m->anchored()},
        {QStringLiteral("badFrameStreak"), m->badFrameStreak()},
        {QStringLiteral("classifiedPct"), m->classifiedPct()},
        {QStringLiteral("framesInWindow"), m->framesInWindow()},
        {QStringLiteral("windowSize"), m->windowSize()},
        {QStringLiteral("voteQuality"), m->voteQuality()},
        {QStringLiteral("refusalReason"), m->refusalReason()},
        {QStringLiteral("refusalName"), m->refusalName()},
    };
}
} // namespace

QJsonObject AutomationServer::doGet(const QString& model, const QString& selector,
                                    const QString& property) const
{
    if (model == QLatin1String("audio")) {
        AudioEngine* audio = m_audioEngine;
        if (!audio) {
            return err(QStringLiteral("no audio engine available"));
        }
        bool snapshotOk = false;
        QJsonObject data = audioSnapshotOnObjectThread(audio, &snapshotOk);
        if (!snapshotOk) {
            return err(QStringLiteral("audio engine snapshot unavailable"));
        }
        if (!property.isEmpty()) {
            if (!data.contains(property)) {
                return err(QStringLiteral("unknown property '") + property
                           + QStringLiteral("' for audio"));
            }
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("value"), data.value(property)}};
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("audio"), data}};
    }
    if (model == QLatin1String("dsp")) {
        AudioEngine* audio = m_audioEngine;
        if (!audio)
            return err(QStringLiteral("no audio engine available"));
        bool snapshotOk = false;
        QJsonObject data = dspSnapshotOnObjectThread(audio, &snapshotOk);
        if (!snapshotOk)
            return err(QStringLiteral("dsp snapshot unavailable"));
        // Merge client-side DSP tuning state. NR2 owns one versioned settings
        // object; the remaining DSPs still use their established keys.
        AppSettings& s = AppSettings::instance();
        const Nr2SettingsModel::Config nr2 =
            Nr2SettingsModel::instance().config();
        QJsonObject tuning = data.value(QStringLiteral("tuning")).toObject();
        tuning[QStringLiteral("nr2")] = QJsonObject{
            {QStringLiteral("gainMax"), nr2.gainMax},
            {QStringLiteral("gainFloor"), nr2.gainFloor},
            {QStringLiteral("gainSmooth"), nr2.gainSmooth},
            {QStringLiteral("qspp"), nr2.qspp},
            {QStringLiteral("gainMethod"), nr2.gainMethod},
            {QStringLiteral("npeMethod"), nr2.npeMethod},
            {QStringLiteral("aeFilter"), nr2.aeFilter},
            {QStringLiteral("legacyGeometryAndGainMapping"),
                nr2.legacyGeometryAndGainMapping},
        };
        tuning[QStringLiteral("nr4")] = QJsonObject{
            {QStringLiteral("reductionDb"),  s.value("NR4ReductionAmount", "100").toFloat()},
            {QStringLiteral("smoothing"),    s.value("NR4SmoothingFactor", "0").toFloat()},
            {QStringLiteral("whitening"),    s.value("NR4WhiteningFactor", "0").toFloat()},
            {QStringLiteral("maskingDepth"), s.value("NR4MaskingDepth", "50").toFloat()},
            {QStringLiteral("suppression"),  s.value("NR4SuppressionStrength", "50").toFloat()},
            {QStringLiteral("noiseMethod"),  s.value("NR4NoiseEstimationMethod", "0").toInt()},
            {QStringLiteral("adaptiveNoise"),
                s.value("NR4AdaptiveNoise", "True").toString() == QLatin1String("True")},
        };
        QJsonObject dfnr = tuning.value(QStringLiteral("dfnr")).toObject();
        dfnr[QStringLiteral("postFilterBeta")] =
            s.value("DfnrPostFilterBeta", "0.0").toFloat();
        tuning[QStringLiteral("dfnr")] = dfnr;
        data[QStringLiteral("tuning")] = tuning;
        if (!property.isEmpty()) {
            if (!data.contains(property))
                return err(QStringLiteral("unknown property '") + property
                           + QStringLiteral("' for dsp"));
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("value"), data.value(property)}};
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("dsp"), data}};
    }
    if (model == QLatin1String("clients")) {
        // #3977: the multi-session forensics snapshot — who is connected to
        // the radio, which sessions have written OUR pans' dBm range, and
        // which stale predecessors we have evicted. `get pans` shows the
        // symptom (minDbm drifting); this shows the culprit.
        RadioModel* radio = m_radioModel;
        if (!radio) {
            return err(QStringLiteral("no radio model available"));
        }
        QJsonArray clients;
        const auto& infoMap = radio->clientInfoMap();
        for (auto it = infoMap.cbegin(); it != infoMap.cend(); ++it) {
            clients.append(QJsonObject{
                {QStringLiteral("handle"),
                 QStringLiteral("0x") + QString::number(it.key(), 16)},
                {QStringLiteral("clientId"), it.value().clientId},
                {QStringLiteral("station"), it.value().station},
                {QStringLiteral("program"), it.value().program},
                {QStringLiteral("source"), it.value().source},
                {QStringLiteral("isUs"), it.key() == radio->ourClientHandle()},
            });
        }
        QJsonArray foreign;
        const auto& writes = radio->foreignPanWrites();
        for (auto it = writes.cbegin(); it != writes.cend(); ++it) {
            foreign.append(QJsonObject{
                {QStringLiteral("handle"),
                 QStringLiteral("0x") + QString::number(it.key(), 16)},
                {QStringLiteral("dbmWrites"), it.value().count},
                {QStringLiteral("lastPanId"), it.value().panId},
                {QStringLiteral("lastMs"), it.value().lastMs},
                {QStringLiteral("evicted"),
                 radio->evictedPredecessorHandles().contains(it.key())},
            });
        }
        QJsonArray evicted;
        for (quint32 h : radio->evictedPredecessorHandles()) {
            evicted.append(QStringLiteral("0x") + QString::number(h, 16));
        }
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("model"), model},
            // evictedHandles lists radio-CONFIRMED disconnects only; eviction
            // itself is opt-in (StaleSessionDefense.EvictionEnabled) — when
            // false, strikes are tallied and logged but nothing is disconnected.
            {QStringLiteral("evictionEnabled"), radio->staleSessionEvictionEnabled()},
            {QStringLiteral("ourHandle"),
             QStringLiteral("0x")
                 + QString::number(radio->ourClientHandle(), 16)},
            {QStringLiteral("station"),
             [&] {  // radio-authoritative, falling back to the local intent
                 const QString reported =
                     infoMap.value(radio->ourClientHandle()).station;
                 return reported.isEmpty() ? radio->ourStationName() : reported;
             }()},
            {QStringLiteral("guiClientId"),
             AppSettings::instance().effectiveGuiClientId()},
            {QStringLiteral("guiClientIdTransient"),
             AppSettings::instance().guiClientIdentityIsTransient()},
            {QStringLiteral("clients"), clients},
            {QStringLiteral("foreignPanWrites"), foreign},
            {QStringLiteral("evictedHandles"), evicted}};
    }
    if (model == QLatin1String("dax")) {
        // Centralized DAX RX channel-ownership snapshot (#3305): who holds
        // which channel, the radio-side stream id, and whether a create is in
        // flight — plus each slice's dax assignment. This is the direct
        // assertion surface for DAX/TCI lifecycle tests (storm regression,
        // co-hold survival, grace-window removal) that previously required
        // log-grepping.
        if (!m_radioModel)
            return err(QStringLiteral("no radio model available"));
        auto* ps = m_radioModel->panStream();
        if (!ps)
            return err(QStringLiteral("no panadapter stream available"));
        QJsonArray channels;
        for (const auto& c : ps->daxChannelSnapshot()) {
            channels.append(QJsonObject{
                {QStringLiteral("channel"),       c.channel},
                {QStringLiteral("streamId"),      QStringLiteral("0x")
                     + QString::number(c.streamId, 16)},
                {QStringLiteral("createPending"), c.createPending},
                {QStringLiteral("holders"),       QJsonArray::fromStringList(c.holders)},
            });
        }
        QJsonArray sliceDax;
        for (const SliceModel* s : m_radioModel->slices()) {
            if (!s) continue;
            sliceDax.append(QJsonObject{
                {QStringLiteral("sliceId"),    s->sliceId()},
                {QStringLiteral("daxChannel"), s->daxChannel()},
            });
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("channels"), channels},
                           {QStringLiteral("slices"), sliceDax}};
    }
    if (model == QLatin1String("waveforms")) {
        QJsonArray installed;
        QJsonObject wfp;
        QJsonArray reports;
        if (m_radioModel) {
            const FlexWaveformModel& wf = m_radioModel->flexWaveformModel();
            for (const FlexWaveformEntry& entry : wf.waveforms()) {
                installed.append(QJsonObject{
                    {QStringLiteral("name"), entry.name},
                    {QStringLiteral("version"), entry.version},
                    {QStringLiteral("isContainer"), entry.isContainer},
                    {QStringLiteral("displayName"), entry.displayName()},
                });
            }
            wfp = QJsonObject{
                {QStringLiteral("seen"), wf.wfpStatusSeen()},
                {QStringLiteral("powered"), wf.wfpPowered()},
                {QStringLiteral("ready"), wf.wfpReady()},
                {QStringLiteral("ipAddress"), wf.wfpIpAddress()},
            };
            for (const QMap<QString, QString>& report : wf.statusReports()) {
                QJsonObject obj;
                for (auto it = report.cbegin(); it != report.cend(); ++it) {
                    obj[it.key()] = it.value();
                }
                reports.append(obj);
            }
        }

        const DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();
        const QString radioCallsign = m_radioModel ? m_radioModel->callsign() : QString{};
        const QString configurationError =
            DigitalVoiceWaveformSettings::validationError(radioCallsign);
        const QString executable = DigitalVoiceWaveformProcess::resolveExecutablePath(
            DigitalVoiceWaveformSettings::executablePath());
        const DigitalVoiceWaveformMetrics& metrics = process.metrics();
        const qint64 metricsAgeMs = metrics.valid
            ? std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch()
                                     - metrics.timestampMs)
            : 0;
        const qint64 txMetricsAgeMs = metrics.txValid
            ? std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch()
                                     - metrics.txTimestampMs)
            : 0;

        QJsonArray modes;
        const std::optional<DigitalVoiceModeId> activeMode =
            DigitalVoiceModeRegistry::instance().activeMode();
        for (const DigitalVoiceModeDescriptor& mode
             : DigitalVoiceModeRegistry::supportedModes()) {
            QJsonObject modeSettings;
            if (mode.id == DigitalVoiceModeId::DStar) {
                modeSettings = QJsonObject{
                    {QStringLiteral("myCall"),
                     DigitalVoiceWaveformSettings::effectiveMyCall(radioCallsign)},
                    {QStringLiteral("myCallSuffix"),
                     DigitalVoiceWaveformSettings::myCallSuffix()},
                    {QStringLiteral("urCall"), DigitalVoiceWaveformSettings::urCall()},
                    {QStringLiteral("rpt1"), DigitalVoiceWaveformSettings::rpt1()},
                    {QStringLiteral("rpt2"), DigitalVoiceWaveformSettings::rpt2()},
                    {QStringLiteral("message"), DigitalVoiceWaveformSettings::message()}
                };
            }
            modes.append(QJsonObject{
                {QStringLiteral("id"), mode.settingsId},
                {QStringLiteral("displayName"), mode.displayName},
                {QStringLiteral("radioMode"), mode.radioMode},
                {QStringLiteral("underlyingMode"), mode.underlyingMode},
                {QStringLiteral("waveformName"), mode.waveformName},
                {QStringLiteral("implemented"), true},
                {QStringLiteral("active"), activeMode.has_value()
                     && activeMode.value() == mode.id},
                {QStringLiteral("settings"), modeSettings}
            });
        }

        QJsonArray rawModeLists;
        int maximumDstrOccurrences = 0;
        if (m_radioModel) {
            const QMap<int, QString> rawLists = m_radioModel->rawSliceModeLists();
            for (auto it = rawLists.cbegin(); it != rawLists.cend(); ++it) {
                int dstrOccurrences = 0;
                QJsonArray rawModes;
                for (const QString& rawMode : it.value().split(
                         QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QString normalized = rawMode.trimmed();
                    rawModes.append(normalized);
                    if (normalized.compare(QStringLiteral("DSTR"),
                                           Qt::CaseInsensitive) == 0) {
                        ++dstrOccurrences;
                    }
                }
                maximumDstrOccurrences = std::max(
                    maximumDstrOccurrences, dstrOccurrences);
                rawModeLists.append(QJsonObject{
                    {QStringLiteral("sliceId"), it.key()},
                    {QStringLiteral("raw"), it.value()},
                    {QStringLiteral("modes"), rawModes},
                    {QStringLiteral("dstrOccurrences"), dstrOccurrences}
                });
            }
        }

        QJsonObject dstarSnapshot;
        if (m_radioModel) {
            const DStarModel& dstar = m_radioModel->dstarModel();
            const DStarConfiguration config = dstar.configuration(radioCallsign);
            const DStarRouteRequest route =
                DStarModel::routeRequestForConfiguration(config);
            auto originName = [](DStarRouteOrigin origin) {
                return origin == DStarRouteOrigin::Direct
                    ? QStringLiteral("direct") : QStringLiteral("repeater");
            };
            auto destinationName = [](DStarRouteDestination destination) {
                switch (destination) {
                case DStarRouteDestination::LocalCq:
                    return QStringLiteral("localCq");
                case DStarRouteDestination::Station:
                    return QStringLiteral("station");
                case DStarRouteDestination::RepeaterArea:
                    return QStringLiteral("repeaterArea");
                case DStarRouteDestination::Custom:
                    return QStringLiteral("custom");
                }
                return QStringLiteral("custom");
            };
            auto verificationName = [](DStarSerialDevice::Verification verification) {
                switch (verification) {
                case DStarSerialDevice::Verification::Candidate:
                    return QStringLiteral("candidate");
                case DStarSerialDevice::Verification::Probing:
                    return QStringLiteral("probing");
                case DStarSerialDevice::Verification::Verified:
                    return QStringLiteral("verified");
                case DStarSerialDevice::Verification::Unavailable:
                    return QStringLiteral("unavailable");
                }
                return QStringLiteral("candidate");
            };
            QJsonArray serialDevices;
            for (const DStarSerialDevice& device : dstar.serialDevices()) {
                serialDevices.append(QJsonObject{
                    {QStringLiteral("path"), device.path},
                    {QStringLiteral("label"), device.label},
                    {QStringLiteral("score"), device.score},
                    {QStringLiteral("highConfidence"), device.highConfidence},
                    {QStringLiteral("present"), device.present},
                    {QStringLiteral("verification"),
                     verificationName(device.verification)},
                    {QStringLiteral("detail"), device.detail}
                });
            }
            QJsonArray traffic;
            const QList<DStarTrafficEntry>& entries = dstar.traffic();
            const qsizetype first = std::max<qsizetype>(0, entries.size() - 100);
            for (qsizetype i = first; i < entries.size(); ++i) {
                const DStarTrafficEntry& entry = entries.at(i);
                QString direction;
                switch (entry.direction) {
                case DStarTrafficDirection::Receive:
                    direction = QStringLiteral("rx");
                    break;
                case DStarTrafficDirection::Transmit:
                    direction = QStringLiteral("tx");
                    break;
                case DStarTrafficDirection::System:
                    direction = QStringLiteral("system");
                    break;
                }
                traffic.append(QJsonObject{
                    {QStringLiteral("id"), static_cast<double>(entry.id)},
                    {QStringLiteral("direction"), direction},
                    {QStringLiteral("timestampUtc"),
                     entry.timestampUtc.toString(Qt::ISODateWithMs)},
                    {QStringLiteral("sliceId"), entry.sliceId},
                    {QStringLiteral("myCall"), entry.myCall},
                    {QStringLiteral("myCallSuffix"), entry.myCallSuffix},
                    {QStringLiteral("urCall"), entry.urCall},
                    {QStringLiteral("rpt1"), entry.rpt1},
                    {QStringLiteral("rpt2"), entry.rpt2},
                    {QStringLiteral("message"), entry.message},
                    {QStringLiteral("complete"), entry.complete}
                });
            }
            dstarSnapshot = QJsonObject{
                {QStringLiteral("route"), QJsonObject{
                    {QStringLiteral("origin"), originName(route.origin)},
                    {QStringLiteral("destination"),
                     destinationName(route.destination)},
                    {QStringLiteral("accessRepeaterCallsign"),
                     route.accessRepeaterCallsign},
                    {QStringLiteral("accessRepeaterModule"),
                     QString(route.accessRepeaterModule)},
                    {QStringLiteral("destinationCallsign"),
                     route.destinationCallsign},
                    {QStringLiteral("destinationRepeaterModule"),
                     QString(route.destinationRepeaterModule)},
                    {QStringLiteral("urCall"), config.urCall},
                    {QStringLiteral("rpt1"), config.rpt1},
                    {QStringLiteral("rpt2"), config.rpt2}
                }},
                {QStringLiteral("message"), config.message},
                {QStringLiteral("serialDevices"), serialDevices},
                {QStringLiteral("traffic"), traffic},
                {QStringLiteral("trafficCount"), entries.size()}
            };
        }

        const QJsonObject localDigitalVoice{
            {QStringLiteral("available"), QFileInfo(executable).isExecutable()},
            {QStringLiteral("state"), DigitalVoiceWaveformProcess::stateName(process.state())},
            {QStringLiteral("active"), process.isActive()},
            {QStringLiteral("activeMode"), activeMode.has_value()
                 ? DigitalVoiceModeRegistry::descriptor(activeMode.value()).displayName
                 : QString{}},
            {QStringLiteral("activeSliceId"),
             DigitalVoiceModeRegistry::instance().activeSliceId()},
            {QStringLiteral("status"), process.statusText()},
            {QStringLiteral("lastError"), process.lastError()},
            {QStringLiteral("registrationName"), process.registrationName()},
            {QStringLiteral("registrationVerified"), process.registrationVerified()},
            {QStringLiteral("health"), DigitalVoiceWaveformProcess::healthName(
                 process.health())},
            {QStringLiteral("healthDetail"), process.healthDetail()},
            {QStringLiteral("metricsValid"), metrics.valid},
            {QStringLiteral("txMetricsValid"), metrics.txValid},
            {QStringLiteral("metricsMode"), metrics.mode},
            {QStringLiteral("metricsAgeMs"), metricsAgeMs},
            {QStringLiteral("txMetricsAgeMs"), txMetricsAgeMs},
            {QStringLiteral("rxRateHz"), metrics.rxSampleRateHz},
            {QStringLiteral("vitaGaps"), static_cast<double>(
                 metrics.vitaSequenceGapsTotal)},
            {QStringLiteral("vitaGapsLatest"), static_cast<int>(
                 metrics.vitaSequenceGaps)},
            {QStringLiteral("inferredSourceBlocks"), static_cast<double>(
                 metrics.sourceBlockDeficitsTotal)},
            {QStringLiteral("inferredSourceBlocksLatest"), static_cast<int>(
                 metrics.sourceBlockDeficits)},
            {QStringLiteral("turnaroundMeanUs"), metrics.turnaroundMeanUs},
            {QStringLiteral("turnaroundMaxUs"), static_cast<double>(
                 metrics.turnaroundMaxUs)},
            {QStringLiteral("queueMax"), static_cast<int>(metrics.queueMax)},
            {QStringLiteral("txRateHz"), metrics.txSampleRateHz},
            {QStringLiteral("txVitaGaps"), static_cast<double>(
                 metrics.txVitaSequenceGapsTotal)},
            {QStringLiteral("txVitaGapsLatest"), static_cast<int>(
                 metrics.txVitaSequenceGaps)},
            {QStringLiteral("txNullFrames"), static_cast<double>(
                 metrics.txNullFramesTotal)},
            {QStringLiteral("txNullFramesLatest"), static_cast<int>(
                 metrics.txNullFrames)},
            {QStringLiteral("txPcmClips"), static_cast<double>(
                 metrics.txPcmClipsTotal)},
            {QStringLiteral("txPcmInvalid"), static_cast<double>(
                 metrics.txPcmInvalidTotal)},
            {QStringLiteral("txSendFailures"), static_cast<double>(
                 metrics.txSendFailuresTotal)},
            {QStringLiteral("txQueueMax"), static_cast<int>(metrics.txQueueMax)},
            {QStringLiteral("txTailSamples"), static_cast<int>(
                 metrics.txTailSamples)},
            {QStringLiteral("txTailUs"), static_cast<double>(metrics.txTailUs)},
            {QStringLiteral("txPreRollFrames"), static_cast<int>(
                 metrics.txPreRollFrames)},
            {QStringLiteral("txPreRollDelayMs"), static_cast<int>(
                 metrics.txPreRollDelayMs)},
            {QStringLiteral("txAmbeQueueMax"), static_cast<int>(
                 metrics.txAmbeQueueMax)},
            {QStringLiteral("txAmbeUnderflows"), static_cast<double>(
                 metrics.txAmbeUnderflowsTotal)},
            {QStringLiteral("txAmbeUnderflowsLatest"), static_cast<int>(
                 metrics.txAmbeUnderflows)},
            {QStringLiteral("txAmbeOverflows"), static_cast<double>(
                 metrics.txAmbeOverflowsTotal)},
            {QStringLiteral("txAmbeOverflowsLatest"), static_cast<int>(
                 metrics.txAmbeOverflows)},
            {QStringLiteral("txAmbeSequenceErrors"), static_cast<double>(
                 metrics.txAmbeSequenceErrorsTotal)},
            {QStringLiteral("txAmbeSequenceErrorsLatest"), static_cast<int>(
                 metrics.txAmbeSequenceErrors)},
            {QStringLiteral("txVocoderSubmitFailures"), static_cast<double>(
                 metrics.txVocoderSubmitFailuresTotal)},
            {QStringLiteral("txVocoderSubmitFailuresLatest"), static_cast<int>(
                 metrics.txVocoderSubmitFailures)},
            {QStringLiteral("txVocoderPendingMax"), static_cast<int>(
                 metrics.txVocoderPendingMax)},
            {QStringLiteral("txDrainFrames"), static_cast<int>(
                 metrics.txDrainFrames)},
            {QStringLiteral("txDrainTimeouts"), static_cast<double>(
                 metrics.txDrainTimeoutsTotal)},
            {QStringLiteral("txDrainTimeoutsLatest"), static_cast<int>(
                 metrics.txDrainTimeouts)},
            {QStringLiteral("txDrainDiscardedFrames"), static_cast<double>(
                 metrics.txDrainDiscardedFramesTotal)},
            {QStringLiteral("txDrainDiscardedFramesLatest"), static_cast<int>(
                 metrics.txDrainDiscardedFrames)},
            {QStringLiteral("metricsGeneration"), static_cast<double>(
                 metrics.generation)},
            {QStringLiteral("metricsSequence"), static_cast<double>(
                 metrics.reportSequence)},
            {QStringLiteral("autoStart"), DigitalVoiceWaveformSettings::autoStart()},
            {QStringLiteral("backend"), DigitalVoiceWaveformSettings::backendLabel(
                 DigitalVoiceWaveformSettings::backend())},
            {QStringLiteral("serialPort"), DigitalVoiceWaveformSettings::serialPort()},
            {QStringLiteral("executable"), executable},
            {QStringLiteral("configurationValid"), configurationError.isEmpty()},
            {QStringLiteral("configurationError"), configurationError},
            {QStringLiteral("modes"), modes},
            {QStringLiteral("dstar"), dstarSnapshot}
        };

        QJsonObject data{
            {QStringLiteral("installed"), installed},
            {QStringLiteral("wfp"), wfp},
            {QStringLiteral("statusReports"), reports},
            {QStringLiteral("localDigitalVoice"), localDigitalVoice},
            {QStringLiteral("rawModeLists"), rawModeLists},
            {QStringLiteral("maximumDstrOccurrencesPerSlice"),
             maximumDstrOccurrences},
            {QStringLiteral("lastCommand"), m_lastWaveformCommand}
        };
        const QString requestedProperty = property.isEmpty() ? selector : property;
        if (!requestedProperty.isEmpty()) {
            if (!data.contains(requestedProperty)) {
                return err(QStringLiteral("unknown property '") + requestedProperty
                           + QStringLiteral("' for waveforms"));
            }
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), requestedProperty},
                               {QStringLiteral("value"), data.value(requestedProperty)}};
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("waveforms"), data}};
    }
    if (model == QLatin1String("renderstats")) {
        // One profiling snapshot for all panadapter, waterfall, 3DSS, shared
        // scheduler, and WAVE-scope work. This deliberately reuses the widget
        // snapshots instead of exposing GUI headers through the core bridge.
        // `get renderstats reset` returns the interval and atomically starts a
        // fresh one across every participating widget.
        const bool reset = selector == QLatin1String("reset")
            || property == QLatin1String("reset");
        QJsonArray pans;
        QJsonArray scopes;
        QVariantMap schedulerStats;
        bool haveSchedulerStats = false;
        QSet<QWidget*> seen;

        double fftFramesPerSec = 0.0;
        double gpuFramesPerSec = 0.0;
        double fftIngestMsPerSec = 0.0;
        double gpuFrameMsPerSec = 0.0;
        double softwarePaintMsPerSec = 0.0;
        double nativeWaterfallUpdatesPerSec = 0.0;
        double nativeWaterfallUpdateMsPerSec = 0.0;
        double kiwiWaterfallUpdatesPerSec = 0.0;
        double kiwiWaterfallUpdateMsPerSec = 0.0;
        double hiddenWaterfallUpdatesPerSec = 0.0;
        double dssLiveRowsPerSec = 0.0;
        double dssLiveMsPerSec = 0.0;
        double dssHistoryRowsPerSec = 0.0;
        double dssHistoryMsPerSec = 0.0;
        double hiddenDssLiveRowsPerSec = 0.0;
        double hiddenDssHistoryRowsPerSec = 0.0;
        double waterfallAllocatedBytes = 0.0;
        double dssAllocatedBytes = 0.0;
        int visiblePanCount = 0;

        for (QWidget* w : findWidgetsByClass(QStringLiteral("SpectrumWidget"))) {
            if (seen.contains(w)) {
                continue;
            }
            seen.insert(w);
            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "panstatsSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap),
                                           Q_ARG(bool, reset))) {
                continue;
            }
            pans.append(QJsonObject::fromVariantMap(snap));
            if (snap.value(QStringLiteral("visible")).toBool()) {
                ++visiblePanCount;
            }
            auto number = [&snap](const char* key) {
                return snap.value(QString::fromLatin1(key)).toDouble();
            };
            fftFramesPerSec += number("fftFramesPerSec");
            gpuFramesPerSec += number("gpuFramesPerSec");
            fftIngestMsPerSec += number("ingestMsPerSec");
            gpuFrameMsPerSec += number("gpuFrameMsPerSec");
            softwarePaintMsPerSec += number("paintMsPerSec");
            nativeWaterfallUpdatesPerSec += number("nativeWaterfallUpdatesPerSec");
            nativeWaterfallUpdateMsPerSec += number("nativeWaterfallUpdateMsPerSec");
            kiwiWaterfallUpdatesPerSec += number("kiwiWaterfallUpdatesPerSec");
            kiwiWaterfallUpdateMsPerSec += number("kiwiWaterfallUpdateMsPerSec");
            hiddenWaterfallUpdatesPerSec +=
                number("nativeWaterfallHiddenUpdatesPerSec")
                + number("kiwiWaterfallHiddenUpdatesPerSec");
            dssLiveRowsPerSec += number("dssLiveRowsPerSec");
            dssLiveMsPerSec += number("dssLiveMsPerSec");
            dssHistoryRowsPerSec += number("dssHistoryRowsPerSec");
            dssHistoryMsPerSec += number("dssHistoryMsPerSec");
            hiddenDssLiveRowsPerSec += number("dssHiddenLiveRowsPerSec");
            hiddenDssHistoryRowsPerSec += number("dssHiddenHistoryRowsPerSec");
            waterfallAllocatedBytes += number("waterfallAllocatedBytes");
            dssAllocatedBytes += number("dssAllocatedBytes");

            if (!haveSchedulerStats) {
                QVariantMap scheduler;
                if (QMetaObject::invokeMethod(w, "renderSchedulerStatsSnapshot",
                                              Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariantMap, scheduler),
                                              Q_ARG(bool, reset))) {
                    schedulerStats = scheduler;
                    haveSchedulerStats =
                        scheduler.value(QStringLiteral("enabled")).toBool();
                }
            }
        }

        seen.clear();
        double wavePaintMsPerSec = 0.0;
        double wavePaintsPerSec = 0.0;
        double waveAppendsPerSec = 0.0;
        for (QWidget* w : findWidgetsByClass(QStringLiteral("WaveformWidget"))) {
            if (seen.contains(w)) {
                continue;
            }
            seen.insert(w);
            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "wavestatsSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap),
                                           Q_ARG(bool, reset))) {
                continue;
            }
            scopes.append(QJsonObject::fromVariantMap(snap));
            wavePaintMsPerSec += snap.value(QStringLiteral("paintMsPerSec")).toDouble();
            wavePaintsPerSec += snap.value(QStringLiteral("paintsPerSec")).toDouble();
            waveAppendsPerSec += snap.value(QStringLiteral("appendsPerSec")).toDouble();
        }

        const double measuredMainThreadMsPerSec =
            fftIngestMsPerSec + nativeWaterfallUpdateMsPerSec
            + kiwiWaterfallUpdateMsPerSec + gpuFrameMsPerSec
            + softwarePaintMsPerSec + wavePaintMsPerSec;
        QJsonObject totals{
            {QStringLiteral("panCount"), pans.size()},
            {QStringLiteral("visiblePanCount"), visiblePanCount},
            {QStringLiteral("waveScopeCount"), scopes.size()},
            {QStringLiteral("fftFramesPerSec"), fftFramesPerSec},
            {QStringLiteral("gpuFramesPerSec"), gpuFramesPerSec},
            {QStringLiteral("fftIngestMsPerSec"), fftIngestMsPerSec},
            {QStringLiteral("gpuFrameMsPerSec"), gpuFrameMsPerSec},
            {QStringLiteral("softwarePaintMsPerSec"), softwarePaintMsPerSec},
            {QStringLiteral("nativeWaterfallUpdatesPerSec"), nativeWaterfallUpdatesPerSec},
            {QStringLiteral("nativeWaterfallUpdateMsPerSec"), nativeWaterfallUpdateMsPerSec},
            {QStringLiteral("kiwiWaterfallUpdatesPerSec"), kiwiWaterfallUpdatesPerSec},
            {QStringLiteral("kiwiWaterfallUpdateMsPerSec"), kiwiWaterfallUpdateMsPerSec},
            {QStringLiteral("hiddenWaterfallUpdatesPerSec"), hiddenWaterfallUpdatesPerSec},
            {QStringLiteral("dssLiveRowsPerSec"), dssLiveRowsPerSec},
            {QStringLiteral("dssLiveMsPerSec"), dssLiveMsPerSec},
            {QStringLiteral("dssHistoryRowsPerSec"), dssHistoryRowsPerSec},
            {QStringLiteral("dssHistoryMsPerSec"), dssHistoryMsPerSec},
            {QStringLiteral("hiddenDssLiveRowsPerSec"), hiddenDssLiveRowsPerSec},
            {QStringLiteral("hiddenDssHistoryRowsPerSec"), hiddenDssHistoryRowsPerSec},
            {QStringLiteral("wavePaintsPerSec"), wavePaintsPerSec},
            {QStringLiteral("wavePaintMsPerSec"), wavePaintMsPerSec},
            {QStringLiteral("waveAppendsPerSec"), waveAppendsPerSec},
            {QStringLiteral("measuredMainThreadMsPerSec"), measuredMainThreadMsPerSec},
            {QStringLiteral("waterfallAllocatedBytes"), waterfallAllocatedBytes},
            {QStringLiteral("dssAllocatedBytes"), dssAllocatedBytes},
        };
        QJsonObject out{{QStringLiteral("ok"), true},
                        {QStringLiteral("model"), model},
                        {QStringLiteral("pans"), pans},
                        {QStringLiteral("scopes"), scopes},
                        {QStringLiteral("totals"), totals}};
        if (haveSchedulerStats) {
            out[QStringLiteral("renderScheduler")] =
                QJsonObject::fromVariantMap(schedulerStats);
        }
        return out;
    }
    if (model == QLatin1String("panstats")) {
        // Per-panadapter frame-cost counters from every SpectrumWidget, for
        // before/after rendering-cost proofs without a profiler attach.
        // selector filters by pan index or objectName; property "reset"
        // zeroes the counters after the read so successive reads measure
        // disjoint intervals. GUI-header-free: snapshotted via meta-call.
        const bool reset = property == QLatin1String("reset")
            || selector == QLatin1String("reset");
        const QString effectiveSelector = selector == QLatin1String("reset")
            ? QString() : selector;
        bool selectorIsIndex = false;
        const int wantIndex = effectiveSelector.toInt(&selectorIsIndex);
        QJsonArray pans;
        QVariantMap renderSchedulerStats;
        bool haveRenderSchedulerStats = false;
        // A floated container is reachable from two top-level roots, so the
        // class walk can yield the same widget twice — dedupe by pointer.
        QSet<QWidget*> seen;
        const QList<QWidget*> widgets =
            findWidgetsByClass(QStringLiteral("SpectrumWidget"));
        for (QWidget* w : widgets) {
            if (seen.contains(w))
                continue;
            seen.insert(w);
            if (!effectiveSelector.isEmpty() && !selectorIsIndex
                && w->objectName() != effectiveSelector)
                continue;
            // Read without resetting first: index filtering needs the
            // snapshot's own panIndex (panIndex() is a plain accessor, not a
            // Q_PROPERTY), and a filtered-out pan must keep its counters.
            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "panstatsSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap),
                                           Q_ARG(bool, false)))
                continue;
            if (!effectiveSelector.isEmpty() && selectorIsIndex
                && snap.value(QStringLiteral("panIndex")).toInt() != wantIndex)
                continue;
            if (!haveRenderSchedulerStats) {
                QVariantMap schedulerSnap;
                if (QMetaObject::invokeMethod(w, "renderSchedulerStatsSnapshot",
                                              Qt::DirectConnection,
                                              Q_RETURN_ARG(QVariantMap, schedulerSnap),
                                              Q_ARG(bool, reset && effectiveSelector.isEmpty()))) {
                    renderSchedulerStats = schedulerSnap;
                    haveRenderSchedulerStats =
                        schedulerSnap.value(QStringLiteral("enabled")).toBool();
                }
            }
            if (reset) {
                QVariantMap discard;
                QMetaObject::invokeMethod(w, "panstatsSnapshot",
                                          Qt::DirectConnection,
                                          Q_RETURN_ARG(QVariantMap, discard),
                                          Q_ARG(bool, true));
            }
            pans.append(QJsonObject::fromVariantMap(snap));
        }
        QJsonObject out{{QStringLiteral("ok"), true},
                        {QStringLiteral("model"), model},
                        {QStringLiteral("pans"), pans}};
        if (haveRenderSchedulerStats) {   // only when a scheduler is actually present
            out[QStringLiteral("renderScheduler")] =
                QJsonObject::fromVariantMap(renderSchedulerStats);
        }
        return out;
    }
    if (model == QLatin1String("tracedebug")) {
        // Per-panadapter trace/floor state from SpectrumWidget. This keeps the
        // bridge GUI-header-free while exposing enough state to compare Flex
        // and Kiwi 2D/3D display sources deterministically.
        bool selectorIsIndex = false;
        const int wantIndex = selector.toInt(&selectorIsIndex);
        QJsonArray pans;
        QSet<QWidget*> seen;
        const QList<QWidget*> widgets =
            findWidgetsByClass(QStringLiteral("SpectrumWidget"));
        for (QWidget* w : widgets) {
            if (seen.contains(w)) {
                continue;
            }
            seen.insert(w);
            if (!selector.isEmpty() && !selectorIsIndex
                && w->objectName() != selector) {
                continue;
            }

            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "traceDebugSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap))) {
                continue;
            }
            if (!selector.isEmpty() && selectorIsIndex
                && snap.value(QStringLiteral("panIndex")).toInt() != wantIndex) {
                continue;
            }
            pans.append(QJsonObject::fromVariantMap(snap));
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("pans"), pans}};
    }
    if (model == QLatin1String("rhi")) {
        // Per-panadapter QRhiWidget surface geometry from every SpectrumWidget,
        // so automation can assert the swapchain/color-buffer sizing that the
        // #4091 fix controls (fixedColorBufferSize kept even-aligned vs a
        // fractional QT_SCALE_FACTOR). selector filters by pan index or
        // objectName. GUI-header-free: found by class name, snapshotted via
        // meta-call. Reports gpu:false per pan on non-GPU builds.
        bool selectorIsIndex = false;
        const int wantIndex = selector.toInt(&selectorIsIndex);
        QJsonArray pans;
        const QList<QWidget*> widgets =
            findWidgetsByClass(QStringLiteral("SpectrumWidget"));
        for (QWidget* w : widgets) {
            if (!selector.isEmpty() && !selectorIsIndex
                && w->objectName() != selector) {
                continue;
            }
            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "automationRhiSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap))) {
                continue;
            }
            if (!selector.isEmpty() && selectorIsIndex
                && snap.value(QStringLiteral("panIndex")).toInt() != wantIndex) {
                continue;
            }
            pans.append(QJsonObject::fromVariantMap(snap));
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("pans"), pans}};
    }
    if (model == QLatin1String("wavestats")) {
        // Per-scope paint/append counters from every WaveformWidget
        // instance (sidebar WAVE applet + Aetherial strip panels), for
        // before/after rendering-cost proofs without a profiler attach.
        // selector filters by objectName ("waveAppletScope" /
        // "stripWaveformScope"); property "reset" zeroes the counters
        // after the read so successive reads measure disjoint intervals.
        // GUI-header-free: found by class name, snapshotted via meta-call.
        const bool reset = property == QLatin1String("reset");
        QJsonArray scopes;
        // A floated container is reachable from two top-level roots, so the
        // class walk can yield the same widget twice — dedupe by pointer.
        QSet<QWidget*> seen;
        const QList<QWidget*> widgets =
            findWidgetsByClass(QStringLiteral("WaveformWidget"));
        for (QWidget* w : widgets) {
            if (seen.contains(w))
                continue;
            seen.insert(w);
            if (!selector.isEmpty() && w->objectName() != selector)
                continue;
            QVariantMap snap;
            if (!QMetaObject::invokeMethod(w, "wavestatsSnapshot",
                                           Qt::DirectConnection,
                                           Q_RETURN_ARG(QVariantMap, snap),
                                           Q_ARG(bool, reset)))
                continue;
            scopes.append(QJsonObject::fromVariantMap(snap));
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("scopes"), scopes}};
    }
    if (model == QLatin1String("sync")
        || model == QLatin1String("receiveSync")) {
        if (!m_receiveSyncSnapshotHandler) {
            return err(QStringLiteral("receive sync snapshot unavailable"));
        }
        QJsonObject data = m_receiveSyncSnapshotHandler();
        data[QStringLiteral("ok")] = true;
        data[QStringLiteral("model")] = model;
        return data;
    }
    if (model == QLatin1String("kiwi")
        || model == QLatin1String("kiwisdr")) {
        if (!m_kiwiSdrSnapshotHandler) {
            return err(QStringLiteral("KiwiSDR snapshot unavailable"));
        }
        QJsonObject data = m_kiwiSdrSnapshotHandler();
        if (!property.isEmpty()) {
            if (!data.contains(property)) {
                return err(QStringLiteral("unknown property '") + property
                           + QStringLiteral("' for kiwi"));
            }
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("value"), data.value(property)}};
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("kiwi"), data}};
    }
    if (model == QLatin1String("txtimer")) {
        // Status-bar transmit-timer state (visible/running/holding/fading/
        // elapsedMs/text/opacity). Read off the TitleBar on the GUI thread.
        if (!m_txTimerSnapshotHandler)
            return err(QStringLiteral("tx timer snapshot unavailable"));
        QJsonObject data = m_txTimerSnapshotHandler();
        if (!property.isEmpty()) {
            if (!data.contains(property))
                return err(QStringLiteral("unknown property '") + property
                           + QStringLiteral("' for txtimer"));
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("value"), data.value(property)}};
        }
        data[QStringLiteral("ok")] = true;
        data[QStringLiteral("model")] = model;
        return data;
    }

    if (model == QLatin1String("clock")) {
        // AetherClock time-signal decode state — model exists independently
        // of a radio connection, so it is served before the radio guard.
        AetherClockModel* clock = m_clockModel;
        if (!clock)
            return err(QStringLiteral("no clock model available"));
        QJsonObject data = clockSnapshot(clock);
        if (!property.isEmpty()) {
            if (!data.contains(property))
                return err(QStringLiteral("unknown property '") + property
                           + QStringLiteral("' for clock"));
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("model"), model},
                               {QStringLiteral("property"), property},
                               {QStringLiteral("value"), data.value(property)}};
        }
        data[QStringLiteral("ok")] = true;
        data[QStringLiteral("model")] = model;
        return data;
    }

    RadioModel* radio = m_radioModel;
    if (!radio)
        return err(QStringLiteral("no radio model available"));

    // Build the payload object for the requested model, then optionally narrow
    // to a single property.
    QJsonObject data;

    if (model == QLatin1String("radio")) {
        data = radioSnapshot(radio);
    } else if (model == QLatin1String("gps")) {
        data = gpsSnapshot(radio);
    } else if (model == QLatin1String("transmit")) {
        data = transmitSnapshot(&radio->transmitModel());
    } else if (model == QLatin1String("cwx")) {
        data = cwxSnapshot(&radio->cwxModel(), radio->cwxActive());
    } else if (model == QLatin1String("equalizer") || model == QLatin1String("eq")) {
        data = equalizerSnapshot(&radio->equalizerModel());
    } else if (model == QLatin1String("meters")) {
        data = metersSnapshot(&radio->meterModel(), radio->model());
    } else if (model == QLatin1String("slices")) {
        QJsonArray arr;
        for (const SliceModel* s : radio->slices()) arr.append(sliceSnapshot(s));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slices"), arr}};
    } else if (model == QLatin1String("pans")) {
        QJsonArray arr;
        for (const PanadapterModel* p : radio->panadapters()) {
            arr.append(panSnapshot(p, radio));
        }
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("pans"), arr}};
    } else if (model == QLatin1String("flags")
               || model == QLatin1String("vfoFlags")) {
        if (!property.isEmpty()) {
            return err(QStringLiteral("get flags does not support property narrowing"));
        }
        const QString trimmedSelector = selector.trimmed();
        bool filterBySliceId = false;
        int wantedSliceId = -1;
        if (!trimmedSelector.isEmpty()
            && trimmedSelector != QLatin1String("all")) {
            bool okId = false;
            wantedSliceId = trimmedSelector.toInt(&okId);
            if (!okId) {
                return err(QStringLiteral("flags selector must be a slice id or 'all'"));
            }
            filterBySliceId = true;
        }
        auto selectorMatches = [filterBySliceId, wantedSliceId](int sliceId) {
            return !filterBySliceId || sliceId == wantedSliceId;
        };

        QJsonArray flags;
        QSet<int> seenSliceIds;
        const QList<QWidget*> widgets =
            findWidgetsByClass(QStringLiteral("VfoWidget"));
        for (QWidget* vfo : widgets) {
            if (!vfo) {
                continue;
            }
            const QVariant sid = vfo->property("sliceId");
            if (!sid.isValid()) {
                continue;
            }
            const int sliceId = sid.toInt();
            if (!selectorMatches(sliceId)) {
                continue;
            }
            seenSliceIds.insert(sliceId);
            flags.append(vfoFlagSnapshot(vfo, radio));
        }

        QJsonArray missing;
        for (const SliceModel* s : radio->slices()) {
            if (!s || !selectorMatches(s->sliceId())
                || seenSliceIds.contains(s->sliceId())) {
                continue;
            }
            missing.append(QJsonObject{
                {QStringLiteral("sliceId"), s->sliceId()},
                {QStringLiteral("letter"), s->letter()},
                {QStringLiteral("expectedPanId"), s->panId()},
            });
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("model"), model},
                           {QStringLiteral("flags"), flags},
                           {QStringLiteral("missingSlices"), missing}};
    } else if (model == QLatin1String("slice")) {
        const SliceModel* s = nullptr;
        const QList<SliceModel*> slices = radio->slices();
        if (selector.isEmpty() || selector == QLatin1String("active")) {
            for (SliceModel* c : slices) if (c->isActive()) { s = c; break; }
            if (!s && !slices.isEmpty()) s = slices.first();
        } else if (selector == QLatin1String("tx")) {
            for (SliceModel* c : slices) if (c->isTxSlice()) { s = c; break; }
        } else {
            bool okId = false; const int id = selector.toInt(&okId);
            if (okId) s = radio->slice(id);
        }
        if (!s)
            return err(QStringLiteral("no slice for selector '") + selector + QStringLiteral("'"));
        data = sliceSnapshot(s);
    } else if (model == QLatin1String("pan")) {
        const PanadapterModel* p = nullptr;
        if (selector.isEmpty() || selector == QLatin1String("active"))
            p = radio->activePanadapter();
        else
            p = radio->panadapter(selector);   // by panId, e.g. "0x40000000"
        if (!p)
            return err(QStringLiteral("no panadapter for selector '") + selector + QStringLiteral("'"));
        data = panSnapshot(p, radio);
    } else {
        return err(QStringLiteral("unknown model: ") + model
                   + QStringLiteral(" (use audio|dsp|sync|radio|transmit|cwx|equalizer|meters|slice|slices|pan|pans|flags|panstats|renderstats|tracedebug|clients|kiwi|wavestats|clock)"));
    }

    if (!property.isEmpty()) {
        if (!data.contains(property))
            return err(QStringLiteral("no property '") + property + QStringLiteral("' on ") + model);
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("model"), model},
            {QStringLiteral("property"), property},
            {QStringLiteral("value"), data.value(property)},
        };
    }

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("model"), model},
        {model, data},   // keyed by model name: "radio" / "slice" / "pan"
    };
}

QJsonObject AutomationServer::doWaveform(const QString& action,
                                         const QString& value)
{
    const QString normalizedAction = action.trimmed().toLower();
    DigitalVoiceWaveformProcess& process = DigitalVoiceWaveformProcess::instance();

    if (normalizedAction == QLatin1String("start")) {
        if (!m_radioModel || !m_radioModel->isConnected()) {
            return err(QStringLiteral("no connected radio available"));
        }
        const QString requestedMode = value.trimmed();
        if (!requestedMode.isEmpty()
                && requestedMode.compare(QStringLiteral("dstar"),
                                         Qt::CaseInsensitive) != 0
                && requestedMode.compare(QStringLiteral("d-star"),
                                         Qt::CaseInsensitive) != 0) {
            return err(QStringLiteral("unsupported digital-voice mode '" )
                       + requestedMode + QStringLiteral("'"));
        }
        const bool accepted = process.startForRadio(
            m_radioModel->radioAddress(), m_radioModel->callsign());
        return QJsonObject{
            {QStringLiteral("ok"), accepted},
            {QStringLiteral("state"), DigitalVoiceWaveformProcess::stateName(process.state())},
            {QStringLiteral("status"), process.statusText()},
            {QStringLiteral("registration"), process.registrationName()},
            {QStringLiteral("error"), process.lastError()}
        };
    }

    if (normalizedAction == QLatin1String("stop")) {
        process.stop();
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("state"), DigitalVoiceWaveformProcess::stateName(process.state())}
        };
    }

    if (normalizedAction == QLatin1String("resync")) {
        if (!m_radioModel || !m_radioModel->isConnected()) {
            return err(QStringLiteral("no connected radio available"));
        }
        m_radioModel->sendCommand(QStringLiteral("sub slice all"));
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("pending"), true}};
    }

    if (normalizedAction == QLatin1String("unregister")) {
        if (!m_radioModel || !m_radioModel->isConnected()) {
            return err(QStringLiteral("no connected radio available"));
        }
        const QString name = value.trimmed();
        static const QRegularExpression safeName(
            QStringLiteral(R"(^[A-Za-z0-9_.-]{1,64}$)"));
        if (!safeName.match(name).hasMatch()) {
            return err(QStringLiteral("unregister requires a safe waveform name"));
        }
        if (process.isActive()
                && name.compare(process.registrationName(), Qt::CaseInsensitive) == 0) {
            return err(QStringLiteral("stop the active digital-voice service first"));
        }

        m_lastWaveformCommand = QJsonObject{
            {QStringLiteral("action"), QStringLiteral("unregister")},
            {QStringLiteral("name"), name},
            {QStringLiteral("pending"), true},
            {QStringLiteral("timestampMs"), QDateTime::currentMSecsSinceEpoch()}
        };
        QPointer<AutomationServer> self(this);
        m_radioModel->sendCmdPublic(
            QStringLiteral("waveform remove %1").arg(name),
            [self, name](int code, const QString& body) {
                if (!self) {
                    return;
                }
                self->m_lastWaveformCommand = QJsonObject{
                    {QStringLiteral("action"), QStringLiteral("unregister")},
                    {QStringLiteral("name"), name},
                    {QStringLiteral("pending"), false},
                    {QStringLiteral("code"), code},
                    {QStringLiteral("body"), body},
                    {QStringLiteral("timestampMs"), QDateTime::currentMSecsSinceEpoch()}
                };
                if (code == 0 && self->m_radioModel
                        && self->m_radioModel->isConnected()) {
                    self->m_radioModel->sendCommand(QStringLiteral("sub slice all"));
                }
            });
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("pending"), true},
                           {QStringLiteral("name"), name}};
    }

    return err(QStringLiteral("unknown waveform action '" )
               + action + QStringLiteral("'"));
}

QJsonObject AutomationServer::doConnect(const QString& action,
                                        const QString& arg,
                                        QLocalSocket* sock)
{
    const QString a = action.trimmed().toLower();

    if (a == QLatin1String("wait")) {
        bool ok = false;
        const int requestedTimeoutMs = arg.trimmed().isEmpty()
            ? 30000
            : arg.trimmed().toInt(&ok);
        if (!ok && !arg.trimmed().isEmpty()) {
            return err(QStringLiteral("connect wait requires a timeout in milliseconds"));
        }
        return doConnectWait(requestedTimeoutMs, sock);
    }
    if (a == QLatin1String("show") || a == QLatin1String("hide")) {
        return doConnectDialog(a);
    }
    if (a == QLatin1String("dialog")) {
        const QString dialogAction = arg.trimmed().toLower();
        if (dialogAction == QLatin1String("show") || dialogAction == QLatin1String("hide")) {
            return doConnectDialog(dialogAction);
        }
        return err(QStringLiteral("connect dialog requires show|hide"));
    }

    IConnectionAutomation* conn = connection();
    if (!conn) {
        return err(QStringLiteral("connection panel unavailable"));
    }

    if (a == QLatin1String("list")) {
        const QList<RadioInfo> radios = conn->automationLocalRadios();
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("count"), radios.size()},
            {QStringLiteral("radios"), connectionRadioListToJson(radios)},
        };
    }

    if (m_radioModel && m_radioModel->isConnected()) {
        return err(QStringLiteral("already connected to a radio"));
    }

    if (a == QLatin1String("local")) {
        const QList<RadioInfo> radios = conn->automationLocalRadios();
        if (radios.isEmpty()) {
            return err(QStringLiteral("no local radios have been discovered"));
        }

        const QString selector = arg.trimmed();
        const QString selectorLower = selector.toLower();
        if (selector.isEmpty() || selectorLower == QLatin1String("first")) {
            const RadioInfo selected = radios.first();
            const QString selectedSerial = selected.serial.trimmed();
            if (selectedSerial.isEmpty()) {
                return err(QStringLiteral("first discovered local radio has no serial"));
            }

            QPointer<QObject> guard(conn->asQObject());
            QTimer::singleShot(0, qApp, [guard, conn, selectedSerial] {
                if (!guard) {
                    return;
                }
                QString error;
                if (!conn->automationConnectLocalSerial(selectedSerial, &error)) {
                    qCWarning(lcAutomation).noquote()
                        << "connect local first failed after scheduling:" << error;
                }
            });
            return QJsonObject{
                {QStringLiteral("ok"), true},
                {QStringLiteral("connect"), QStringLiteral("local")},
                {QStringLiteral("selector"), QStringLiteral("first")},
                {QStringLiteral("serial"), selectedSerial},
                {QStringLiteral("requested"), true},
                {QStringLiteral("deferred"), true},
                {QStringLiteral("radio"), connectionRadioToJson(selected)},
            };
        }

        static const QString kSerialPrefix = QStringLiteral("serial ");
        if (selectorLower.startsWith(kSerialPrefix)) {
            const QString serial = selector.mid(kSerialPrefix.size()).trimmed();
            for (const RadioInfo& radio : radios) {
                if (radio.serial.compare(serial, Qt::CaseInsensitive) != 0) {
                    continue;
                }

                QPointer<QObject> guard(conn->asQObject());
                QTimer::singleShot(0, qApp, [guard, conn, serial] {
                    if (!guard) {
                        return;
                    }
                    QString error;
                    if (!conn->automationConnectLocalSerial(serial, &error)) {
                        qCWarning(lcAutomation).noquote()
                            << "connect local serial failed after scheduling:" << error;
                    }
                });
                return QJsonObject{
                    {QStringLiteral("ok"), true},
                    {QStringLiteral("connect"), QStringLiteral("local")},
                    {QStringLiteral("selector"), QStringLiteral("serial")},
                    {QStringLiteral("serial"), serial},
                    {QStringLiteral("requested"), true},
                    {QStringLiteral("deferred"), true},
                    {QStringLiteral("radio"), connectionRadioToJson(radio)},
                };
            }

            return err(QStringLiteral("no discovered local radio has serial '%1'").arg(serial));
        }

        return err(QStringLiteral("connect local requires first or serial <serial>"));
    }

    if (a == QLatin1String("ip")) {
        const QString target = arg.trimmed();
        if (target.isEmpty()) {
            return err(QStringLiteral("connect ip requires a host or IP address"));
        }

        QPointer<QObject> guard(conn->asQObject());
        QTimer::singleShot(0, qApp, [guard, conn, target] {
            if (!guard) {
                return;
            }
            QString error;
            if (!conn->automationConnectByIp(target, &error)) {
                qCWarning(lcAutomation).noquote()
                    << "connect ip failed after scheduling:" << error;
            }
        });
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("connect"), QStringLiteral("ip")},
            {QStringLiteral("target"), target},
            {QStringLiteral("requested"), true},
            {QStringLiteral("deferred"), true},
        };
    }

    return err(QStringLiteral("unknown connect action: ") + action
               + QStringLiteral(" (use list|show|hide|local|ip|wait)"));
}

QJsonObject AutomationServer::doConnectDialog(const QString& action)
{
    const bool show = action == QLatin1String("show");
    const bool hide = action == QLatin1String("hide");
    if (!show && !hide) {
        return err(QStringLiteral("connect dialog requires show|hide"));
    }

    QObject* host = m_connectionDialogHost;
    IConnectionAutomation* conn = connection();
    if (!host && !conn) {
        return err(QStringLiteral("connection dialog unavailable"));
    }

    const bool wasVisible = conn && conn->automationDialogVisible();
    QPointer<QObject> guardedHost = host;
    QPointer<QObject> guard(conn ? conn->asQObject() : nullptr);
    QTimer::singleShot(0, qApp, [guardedHost, guard, conn, show] {
        if (guardedHost) {
            const char* method = show ? "showConnectionDialog" : "hideConnectionDialog";
            if (QMetaObject::invokeMethod(guardedHost, method, Qt::DirectConnection)) {
                return;
            }
            qCWarning(lcAutomation).noquote()
                << "connection dialog host missing invokable" << method
                << "- falling back to direct panel visibility";
        }

        if (!guard) {
            return;
        }
        conn->automationSetDialogVisible(show);
    });

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("connect"), action},
        {QStringLiteral("requested"), true},
        {QStringLiteral("deferred"), true},
        {QStringLiteral("wasVisible"), wasVisible},
    };
}

QJsonObject AutomationServer::doDisconnect()
{
    IConnectionAutomation* conn = connection();
    if (!conn) {
        return err(QStringLiteral("connection panel unavailable"));
    }
    if (!m_radioModel || !m_radioModel->isConnected()) {
        return err(QStringLiteral("not connected to a radio"));
    }

    QPointer<QObject> guard(conn->asQObject());
    QTimer::singleShot(0, qApp, [guard, conn] {
        if (!guard) {
            return;
        }
        QString error;
        if (!conn->automationDisconnect(&error)) {
            qCWarning(lcAutomation).noquote()
                << "disconnect failed after scheduling:" << error;
        }
    });

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("disconnect"), true},
        {QStringLiteral("requested"), true},
        {QStringLiteral("deferred"), true},
    };
}

QJsonObject AutomationServer::doRecord(const QString& action, const QString& value)
{
    // record start|stop|status|path|dir — drives the Client-Side QSO recorder so
    // a live test can capture a WAV and verify SSB + CW/CWX TX are recorded.
    // Not a transmit action, so no ALLOW_TX gate. Runs on the GUI thread (same
    // as the manual record button), matching the recorder's threading.
    if (!m_qsoRecorder)
        return err(QStringLiteral("qso recorder unavailable"));

    const QString a = action.trimmed().toLower();

    if (a == QLatin1String("dir")) {
        if (value.trimmed().isEmpty())
            return err(QStringLiteral("record dir requires a path"));
        m_qsoRecorder->setRecordingDir(value.trimmed());
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("record"), QStringLiteral("dir")},
                           {QStringLiteral("dir"), m_qsoRecorder->recordingDir()}};
    }
    if (a == QLatin1String("start")) {
        m_qsoRecorder->startRecording();
        return QJsonObject{
            {QStringLiteral("ok"), m_qsoRecorder->isRecording()},
            {QStringLiteral("record"), QStringLiteral("start")},
            {QStringLiteral("recording"), m_qsoRecorder->isRecording()},
            {QStringLiteral("path"), m_qsoRecorder->recordingFilePath()},
        };
    }
    if (a == QLatin1String("stop")) {
        const int durationSecs = m_qsoRecorder->recordingDurationSecs();
        m_qsoRecorder->stopRecording();
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("record"), QStringLiteral("stop")},
            {QStringLiteral("recording"), m_qsoRecorder->isRecording()},
            {QStringLiteral("durationSecs"), durationSecs},
            {QStringLiteral("path"), m_qsoRecorder->recordingFilePath()},
        };
    }
    if (a.isEmpty() || a == QLatin1String("status")) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("recording"), m_qsoRecorder->isRecording()},
            {QStringLiteral("durationSecs"), m_qsoRecorder->recordingDurationSecs()},
            {QStringLiteral("path"), m_qsoRecorder->recordingFilePath()},
        };
    }
    if (a == QLatin1String("path")) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("path"), m_qsoRecorder->recordingFilePath()},
        };
    }
    return err(QStringLiteral("record: unknown action '%1' (start|stop|status|path|dir)")
                   .arg(action));
}

QJsonObject AutomationServer::doTestTone(const QString& action, const QString& value)
{
    if (!m_audioEngine || !m_audioEngine->clientTxTestTone())
        return err(QStringLiteral("test tone unavailable"));
    auto* tone = m_audioEngine->clientTxTestTone();
    const QString a = action.trimmed().toLower();

    if (a == QLatin1String("off")) {
        tone->setEnabled(false);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("testtone"), QStringLiteral("off")}};
    }
    if (a == QLatin1String("on")) {
        const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool okF = false, okL = false;
        const float hz = parts.value(0).toFloat(&okF);
        const float db = parts.value(1).toFloat(&okL);
        if (okF) tone->setFrequencyHz(hz);
        if (okL) tone->setLevelDb(db);
        tone->setEnabled(true);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("testtone"), QStringLiteral("on")},
                           {QStringLiteral("freqHz"), okF ? hz : 0.0},
                           {QStringLiteral("levelDb"), okL ? db : 0.0}};
    }
    return err(QStringLiteral("testtone: unknown action '%1' (on [freqHz] [levelDb] | off)")
                   .arg(action));
}

QJsonObject AutomationServer::doConnectWait(int timeoutMs, QLocalSocket* sock)
{
    if (!m_radioModel) {
        return err(QStringLiteral("no radio model available"));
    }
    if (m_radioModel->isConnected()) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("connected"), true},
            {QStringLiteral("elapsedMs"), 0},
            {QStringLiteral("radio"), radioSnapshot(m_radioModel)},
        };
    }
    if (!sock) {
        return err(QStringLiteral("connect wait requires a live automation client"));
    }

    const int boundedTimeoutMs = std::clamp(timeoutMs, 1, 300000);
    auto wait = std::make_shared<ConnectWait>();
    wait->socket = sock;
    wait->timeoutMs = boundedTimeoutMs;
    wait->elapsed.start();
    wait->timer = new QTimer(this);
    wait->timer->setSingleShot(true);

    wait->connection = connect(m_radioModel, &RadioModel::connectionStateChanged,
                               this, [this, wait](bool connected) {
        if (connected) {
            finishConnectWait(wait, false);
        }
    });
    connect(wait->timer, &QTimer::timeout, this, [this, wait] {
        finishConnectWait(wait, true);
    });

    m_connectWaits.push_back(wait);
    wait->timer->start(boundedTimeoutMs);
    return deferredResponse();
}

void AutomationServer::finishConnectWait(const std::shared_ptr<ConnectWait>& wait,
                                         bool timedOut)
{
    if (!wait || wait->complete) {
        return;
    }

    wait->complete = true;
    if (wait->timer) {
        wait->timer->stop();
        wait->timer->deleteLater();
        wait->timer = nullptr;
    }
    QObject::disconnect(wait->connection);

    const auto newEnd = std::remove(m_connectWaits.begin(), m_connectWaits.end(), wait);
    m_connectWaits.erase(newEnd, m_connectWaits.end());

    QLocalSocket* socket = wait->socket;
    if (!socket || m_buffers.find(socket) == m_buffers.end()
        || socket->state() == QLocalSocket::UnconnectedState) {
        qCDebug(lcAutomation)
            << "dropping connect wait response because client disconnected";
        return;
    }

    const bool connected = m_radioModel && m_radioModel->isConnected();
    QJsonObject response{
        {QStringLiteral("ok"), connected},
        {QStringLiteral("connected"), connected},
        {QStringLiteral("elapsedMs"), static_cast<int>(wait->elapsed.elapsed())},
        {QStringLiteral("timeoutMs"), wait->timeoutMs},
    };
    if (connected) {
        response[QStringLiteral("radio")] = radioSnapshot(m_radioModel);
    } else if (timedOut) {
        response[QStringLiteral("timeout")] = true;
        response[QStringLiteral("error")] =
            QStringLiteral("timed out waiting for radio connection");
    } else {
        response[QStringLiteral("error")] =
            QStringLiteral("radio connection did not complete");
    }

    writeJsonResponse(socket, response);
}

// ── TX test-signal control (#3646) ──────────────────────────────────────────
// `txtest twotone` starts the radio's two-tone test (a modulated signal that
// exercises ALC / PEP / linearity meters a steady carrier can't). `txtest off`
// stops it. Keying is gated by AETHER_AUTOMATION_ALLOW_TX, and the TX watchdog
// still backstops it. NOTE: two-tone does not pass through the mic/speech
// processor, so it does not exercise the compression meter — that needs a real
// mic-audio source (DAX TX), which is a separate, larger effort.
QJsonObject AutomationServer::doTxTest(const QString& action)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    auto& tx = m_radioModel->transmitModel();

    if (action == QLatin1String("off") || action == QLatin1String("stop")) {
        tx.stopTune();
        m_txKeyedSinceMs = 0;
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("txtest"), QStringLiteral("off")}};
    }
    if (action == QLatin1String("twotone")) {
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: txtest keys the transmitter — "
                                      "set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        tx.startTwoToneTune();
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();  // arm watchdog window
        qCInfo(lcAutomation) << "txtest two-tone started (ALLOW_TX)";
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("txtest"), QStringLiteral("twotone")}};
    }
    return err(QStringLiteral("unknown txtest action: ") + action + QStringLiteral(" (twotone|off)"));
}

// ── ATU control (#3646) ─────────────────────────────────────────────────────
// `atu bypass` takes the tuner out of circuit (no TX), so meter readings see
// the raw load instead of a recalled antenna match — essential before TX meter
// measurements. `atu start` runs a tune cycle (keys TX → gated by ALLOW_TX).
QJsonObject AutomationServer::doAtu(const QString& action)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    auto& tx = m_radioModel->transmitModel();

    if (action == QLatin1String("bypass")) {
        tx.atuBypass();   // relay switch only — does not transmit
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("atu"), QStringLiteral("bypass")}};
    }
    if (action == QLatin1String("start") || action == QLatin1String("tune")) {
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: atu start keys the transmitter — "
                                      "set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        tx.atuStart();
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("atu"), QStringLiteral("start")}};
    }
    return err(QStringLiteral("unknown atu action: ") + action + QStringLiteral(" (bypass|start)"));
}

// Emergency all-stop: drop tune, two-tone, and MOX immediately. Used by the
// watchdog and stop().
void AutomationServer::forceUnkey(const char* reason)
{
    if (!m_radioModel)
        return;
    auto& tx = m_radioModel->transmitModel();
    tx.stopTune();
    tx.setMox(false);
    // Also abort any in-flight CWX keying: CWX is driven by its own buffer,
    // largely independent of MOX, so setMox(false) alone won't stop a `cwx send`
    // already keying CW. Without this the watchdog would fire repeatedly with no
    // effect until the buffer drained — defeating the all-stop guarantee. (#3646)
    m_radioModel->cwxModel().clearBuffer();
    m_txKeyedSinceMs = 0;
    qCWarning(lcAutomation).noquote() << "TX force-unkey:" << reason;
}

// TX safety watchdog (#3646). Runs only when AETHER_AUTOMATION_ALLOW_TX is set.
// Tracks how long the radio has been continuously keyed and force-unkeys past
// the limit, so a hung or abandoned automation script can never leave a live
// transmitter on. The limit is AETHER_AUTOMATION_TX_MAX_MS (default 20 s).
void AutomationServer::onTxWatchdog()
{
    if (!m_radioModel)
        return;
    const auto& tx = m_radioModel->transmitModel();
    const bool keyed = tx.isTransmitting() || tx.isTuning() || tx.isMox();
    if (!keyed) {
        m_txKeyedSinceMs = 0;
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_txKeyedSinceMs == 0)
        m_txKeyedSinceMs = now;
    else if (now - m_txKeyedSinceMs > m_txMaxKeyMs)
        forceUnkey("max continuous key time exceeded");
}

QJsonObject AutomationServer::doSlice(const QString& action, const QString& arg)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    RadioModel* radio = m_radioModel;

    if (action == QLatin1String("add")) {
        // Pre-check radio-wide slot capacity. Slices are a radio resource shared
        // across MultiFlex clients, so a create is refused when every slot is
        // taken — even if WE hold only one. Surface that synchronously instead
        // of issuing a command the radio will silently reject. (#3646)
        int freeSlots = 0;
        QString occupant;
        for (int id = 0; id < radio->maxSlices(); ++id) {
            if (!radio->isSlotOurs(id) && !radio->isSlotForeign(id)) freeSlots++;
            else if (radio->isSlotForeign(id) && occupant.isEmpty())
                occupant = radio->foreignSliceOwnerStation(id);
        }
        if (freeSlots == 0) {
            QString msg = QStringLiteral("refused: no free slice slot (radio at its ")
                + QString::number(radio->maxSlices()) + QStringLiteral("-slice limit");
            if (!occupant.isEmpty())
                msg += QStringLiteral("; a foreign client '") + occupant + QStringLiteral("' holds a slot");
            return err(msg + QStringLiteral(")"));
        }

        bool okF = false;
        const double freq = arg.toDouble(&okF);
        if (okF && freq > 0)
            radio->addSliceOnPan(radio->panId(), freq);   // specific frequency
        else
            radio->addSlice();                            // default (TX freq / active pan)
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("add")},
                           {QStringLiteral("freq"), okF ? QJsonValue(freq) : QJsonValue()},
                           {QStringLiteral("requested"), true},
                           {QStringLiteral("sliceCount"), radio->slices().size()}};
    }
    if (action == QLatin1String("remove")) {
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId)
            return err(QStringLiteral("slice remove requires a slice id"));
        if (radio->slices().size() <= 1)
            return err(QStringLiteral("refused: cannot remove the last slice"));
        if (!radio->slice(id))
            return err(QStringLiteral("no slice with id ") + arg);
        radio->sendCommand(QStringLiteral("slice remove %1").arg(id));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("remove")},
                           {QStringLiteral("id"), id}};
    }
    if (action == QLatin1String("select")) {
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId || !radio->slice(id))
            return err(QStringLiteral("slice select requires a valid slice id"));
        radio->sendCommand(QStringLiteral("slice set %1 active=1").arg(id));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("select")},
                           {QStringLiteral("id"), id}};
    }
    if (action == QLatin1String("tx")) {
        // Make slice <id> the TX slice — the literal external-split transition
        // (rigctld set_split_vfo 1 VFOB) bottoms out here. Set-only via the same
        // SliceModel chokepoint the CAT split path uses; the radio enforces
        // single-TX and clears the prior TX slice, so we never clear it
        // client-side (radio-authoritative). The txSlice flag flips only when
        // the radio echoes status back, so callers re-poll `get slices`. (#3646)
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId)
            return err(QStringLiteral("slice tx requires a slice id"));
        SliceModel* s = radio->slice(id);
        if (!s)
            return err(QStringLiteral("no slice with id ") + arg);
        if (s->isTxSlice())
            return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("tx")},
                               {QStringLiteral("id"), id}, {QStringLiteral("alreadyTx"), true}};
        s->setTxSlice(true);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), QStringLiteral("tx")},
                           {QStringLiteral("id"), id}, {QStringLiteral("requested"), true}};
    }
    if (action == QLatin1String("diversity")) {
        const QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            return err(QStringLiteral("slice diversity requires '<slice-id> <on|off>'"));
        }
        bool okId = false;
        const int id = parts.at(0).toInt(&okId);
        SliceModel* slice = okId ? radio->slice(id) : nullptr;
        if (!slice) {
            return err(QStringLiteral("slice diversity requires a valid slice id"));
        }
        const QString state = parts.at(1).trimmed().toLower();
        const bool validState = state == QLatin1String("1")
            || state == QLatin1String("true") || state == QLatin1String("on")
            || state == QLatin1String("0") || state == QLatin1String("false")
            || state == QLatin1String("off");
        if (!validState) {
            return err(QStringLiteral("slice diversity state must be on or off"));
        }
        const bool enabled = parseBool(state);
        slice->setDiversity(enabled);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("slice"), QStringLiteral("diversity")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("requested"), true}};
    }
    if (action == QLatin1String("centerlock")) {
        const QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() != 2) {
            return err(QStringLiteral("slice centerlock requires '<slice-id> <on|off>'"));
        }
        bool okId = false;
        const int id = parts.at(0).toInt(&okId);
        if (!okId || !radio->slice(id)) {
            return err(QStringLiteral("slice centerlock requires a valid slice id"));
        }
        const QString state = parts.at(1).trimmed().toLower();
        const bool validState = state == QLatin1String("1")
            || state == QLatin1String("true") || state == QLatin1String("on")
            || state == QLatin1String("0") || state == QLatin1String("false")
            || state == QLatin1String("off");
        if (!validState) {
            return err(QStringLiteral("slice centerlock state must be on or off"));
        }
        if (!m_sliceCenterLockHandler) {
            return err(QStringLiteral("slice centerlock handler is unavailable"));
        }
        return m_sliceCenterLockHandler(id, parseBool(state));
    }
    if (action == QLatin1String("mode")) {
        const QString requestedMode = arg.trimmed().toUpper();
        if (requestedMode.isEmpty()) {
            return err(QStringLiteral("slice mode requires a mode name (e.g. USB or DSTR)"));
        }

        SliceModel* s = nullptr;
        for (SliceModel* candidate : radio->slices()) {
            if (candidate->isActive()) {
                s = candidate;
                break;
            }
        }
        if (!s && !radio->slices().isEmpty()) {
            s = radio->slices().first();
        }
        if (!s) {
            return err(QStringLiteral("no slice available to set mode on"));
        }

        const QStringList modes = s->modeList();
        bool supported = modes.isEmpty();
        for (const QString& mode : modes) {
            if (mode.compare(requestedMode, Qt::CaseInsensitive) == 0) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            return err(QStringLiteral("mode '") + requestedMode
                       + QStringLiteral("' not in mode list: ")
                       + modes.join(QLatin1Char(',')));
        }

        const bool unchanged = s->mode().compare(requestedMode, Qt::CaseInsensitive) == 0;
        if (!unchanged) {
            s->setMode(requestedMode);
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("slice"), QStringLiteral("mode")},
                           {QStringLiteral("id"), s->sliceId()},
                           {QStringLiteral("mode"), requestedMode},
                           {QStringLiteral("unchanged"), unchanged},
                           {QStringLiteral("requested"), !unchanged}};
    }
    if (action == QLatin1String("txant") || action == QLatin1String("rxant")) {
        // Set the transmit/receive antenna port deterministically. The GUI
        // controls are QMenu::exec() popups an invoke() can't drive without
        // blocking the event loop, so route through the same SliceModel
        // setTxAntenna/setRxAntenna chokepoints the menus use. Lets a driver
        // enforce/establish the dummy-load antenna before any TX-safety gate —
        // read back with `get slice tx txAntenna`/`rxAntenna`. (#3646)
        const bool tx = (action == QLatin1String("txant"));
        const QString ant = arg.trimmed();
        if (ant.isEmpty())
            return err(QStringLiteral("slice ") + action + QStringLiteral(" requires an antenna port (e.g. ANT2)"));
        SliceModel* s = nullptr;
        if (tx) for (SliceModel* c : radio->slices()) if (c->isTxSlice())  { s = c; break; }
        if (!s) for (SliceModel* c : radio->slices()) if (c->isActive()) { s = c; break; }
        if (!s && !radio->slices().isEmpty()) s = radio->slices().first();
        if (!s)
            return err(QStringLiteral("no slice available to set antenna on"));
        const QStringList opts = tx ? s->txAntennaList() : s->rxAntennaList();
        if (!opts.isEmpty() && !opts.contains(ant))
            return err(QStringLiteral("antenna '") + ant + QStringLiteral("' not in antenna list: ")
                       + opts.join(QLatin1Char(',')));
        if (tx) s->setTxAntenna(ant); else s->setRxAntenna(ant);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("slice"), action},
                           {QStringLiteral("id"), s->sliceId()},
                           {tx ? QStringLiteral("txAntenna") : QStringLiteral("rxAntenna"), ant},
                           {QStringLiteral("requested"), true}};
    }
    if (action == QLatin1String("rxsource") || action == QLatin1String("source")) {
        if (!m_sliceReceiveSourceHandler) {
            return err(QStringLiteral("slice rxsource is unavailable in this app instance"));
        }
        return m_sliceReceiveSourceHandler(arg);
    }
    if (action == QLatin1String("fixture")) {
        const QStringList parts = arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty()) {
            return err(QStringLiteral("slice fixture requires a slice id"));
        }
        if (parts.size() > 2) {
            return err(QStringLiteral("slice fixture accepts only: <slice id> [letter]"));
        }
        bool okId = false;
        const int id = parts.at(0).toInt(&okId);
        if (!okId) {
            return err(QStringLiteral("slice fixture requires a numeric slice id"));
        }
        const QString letter = parts.value(1);
        QString error;
        if (!radio->automationApplySliceFixture(id, letter, &error)) {
            return err(error);
        }

        QJsonObject response{{QStringLiteral("ok"), true},
                             {QStringLiteral("slice"), QStringLiteral("fixture")},
                             {QStringLiteral("id"), id},
                             {QStringLiteral("sliceCount"), radio->slices().size()}};
        SliceModel* s = radio->slice(id);
        if (s) {
            response[QStringLiteral("letter")] = s->letter();
        }
        return response;
    }
    if (action == QLatin1String("clearfixture")) {
        bool okId = false;
        const int id = arg.toInt(&okId);
        if (!okId) {
            return err(QStringLiteral("slice clearfixture requires a slice id"));
        }
        QString error;
        if (!radio->automationRemoveSliceFixture(id, &error)) {
            return err(error);
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("slice"), QStringLiteral("clearfixture")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("sliceCount"), radio->slices().size()}};
    }
    return err(QStringLiteral("unknown slice action: ") + action
               + QStringLiteral(" (add|remove|select|tx|mode|diversity|centerlock|"
                                "txant|rxant|rxsource|fixture|clearfixture)"));
}

QJsonObject AutomationServer::doGps(const QString& action, const QString& format)
{
    if (!m_radioModel) {
        return err(QStringLiteral("no radio model available"));
    }
    if (m_radioModel->isConnected()) {
        return err(QStringLiteral(
            "gps fixtures are disconnected-only; disconnect from the radio first"));
    }

    GpsDelta delta;
    if (action == QLatin1String("clearfixture")) {
        delta.status = QString();
        delta.tracked = 0;
        delta.visible = 0;
        delta.grid = QString();
        delta.altitude = QString();
        delta.lat = QString();
        delta.lon = QString();
        delta.time = QString();
        delta.speed = QString();
        delta.track = QString();
        delta.freqError = QString();
        QString error;
        if (!m_radioModel->automationApplyGpsFixture(
                delta, QString(), QStringLiteral("auto"), false, QString(),
                &error)) {
            return err(error);
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("gps"), QStringLiteral("clearfixture")}};
    }
    if (action != QLatin1String("fixture")) {
        return err(QStringLiteral(
            "unknown gps action: ") + action
            + QStringLiteral(" (fixture|clearfixture)"));
    }

    const QString profile = format.trimmed().toLower();
    if (profile != QLatin1String("6000")
        && profile != QLatin1String("8000")) {
        return err(QStringLiteral("gps fixture requires 6000 or 8000"));
    }

    delta.status = QStringLiteral("Locked");
    delta.time = QDateTime::currentDateTimeUtc().time()
                     .toString(QStringLiteral("HH:mm:ss'Z'"));
    delta.speed = QStringLiteral("0 kts");
    delta.track = QString();
    if (profile == QLatin1String("6000")) {
        // Live FLEX-6700 GPSDO captures use hemisphere + degrees + decimal
        // minutes. The dashboard intentionally consumes that radio text via
        // the same parseGpsCoordinate() path as production status.
        delta.tracked = 9;
        delta.visible = 12;
        // Use the public Mount Wilson Observatory for shareable visual-test
        // artifacts; the coordinate parser unit test retains the exact live
        // FLEX-6700 capture values.
        delta.grid = QStringLiteral("DM04xf");
        delta.altitude = QStringLiteral("1742 m");
        delta.lat = QStringLiteral("N 34 13.464");
        delta.lon = QStringLiteral("W 118 03.450");
        delta.freqError = QStringLiteral("18 ppb");
    } else {
        // Exercise the decimal-coordinate and course fields from the
        // FLEX-8600 wire format while keeping shareable visual-test artifacts
        // pinned to the public Mount Wilson Observatory. Parser tests retain
        // the exact clean-room firmware 4.2.18 capture values.
        delta.tracked = 8;
        delta.visible = 28;
        delta.grid = QStringLiteral("DM04xf");
        delta.altitude = QStringLiteral("1742 m");
        delta.lat = QStringLiteral("34.224400000");
        delta.lon = QStringLiteral("-118.057500000");
        delta.track = QStringLiteral("273.4");
        delta.freqError = QStringLiteral("274 ppb");
    }
    QString error;
    if (!m_radioModel->automationApplyGpsFixture(
            delta, QStringLiteral("gpsdo"), QStringLiteral("auto"), true,
            profile == QLatin1String("8000")
                ? QStringLiteral("192.0.2.80") : QString(),
            &error)) {
        return err(error);
    }

    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("gps"), QStringLiteral("fixture")},
                       {QStringLiteral("profile"), profile},
                       {QStringLiteral("snapshot"), gpsSnapshot(m_radioModel)}};
}

// ── VFO tuning (#3646) ──────────────────────────────────────────────────────
// Set a slice's frequency (MHz). The most fundamental control the VfoWidget
// couldn't expose (it's custom-painted). Honors the slice lock guard. An
// optional slice id targets a specific slice directly — without it the active
// slice is tuned (the original behavior), which forced external scripts into a
// racy select → tune → restore flap when driving a non-active slice.
QJsonObject AutomationServer::doTune(const QString& value, const QString& id)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    bool okF = false;
    const double mhz = value.toDouble(&okF);
    if (!okF || mhz <= 0)
        return err(QStringLiteral("tune requires a positive frequency in MHz"));

    int sliceId = -1;  // -1 = active slice
    if (!id.isEmpty()) {
        bool okId = false;
        sliceId = id.toInt(&okId);
        if (!okId || sliceId < 0)
            return err(QStringLiteral("tune: sliceId must be a non-negative integer"));
    }

    if (m_tuneHandler) {
        return m_tuneHandler(mhz, sliceId);
    }

    SliceModel* s = nullptr;
    if (sliceId >= 0) {
        for (SliceModel* c : m_radioModel->slices())
            if (c->sliceId() == sliceId) { s = c; break; }
        if (!s)
            return err(QStringLiteral("no slice with id ") + QString::number(sliceId));
        // Mirror the GUI path's Multi-Flex gate (MainWindow::automationTune):
        // a headless caller must not drive another client's slice either.
        if (!m_radioModel->sliceMayBelongToUs(sliceId))
            return err(QStringLiteral("refused: slice ") + QString::number(sliceId)
                       + QStringLiteral(" belongs to another client"));
    } else {
        for (SliceModel* c : m_radioModel->slices())
            if (c->isActive()) { s = c; break; }
        if (!s && !m_radioModel->slices().isEmpty())
            s = m_radioModel->slices().first();
        if (!s)
            return err(QStringLiteral("no slice to tune"));
    }
    if (s->isLocked())
        return err(QStringLiteral("refused: slice ") + s->letter() + QStringLiteral(" is VFO-locked"));

    s->setFrequency(mhz);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("tune"), mhz},
                       {QStringLiteral("sliceId"), s->sliceId()}, {QStringLiteral("letter"), s->letter()}};
}

QJsonObject AutomationServer::doTargetTune(const QString& value)
{
    bool okFrequency = false;
    const double mhz = value.toDouble(&okFrequency);
    if (!okFrequency || mhz <= 0.0) {
        return err(QStringLiteral(
            "targettune requires a positive frequency in MHz"));
    }
    if (!m_targetTuneHandler) {
        return err(QStringLiteral("target tune handler is unavailable"));
    }
    return m_targetTuneHandler(mhz);
}

QJsonObject AutomationServer::doMemory(const QString& action, const QString& arg)
{
    if (action.trimmed().compare(QStringLiteral("activate"), Qt::CaseInsensitive) != 0) {
        return err(QStringLiteral("unknown memory action: ") + action
                   + QStringLiteral(" (activate)"));
    }
    if (!m_memoryActivateHandler) {
        return err(QStringLiteral("memory activation handler is unavailable"));
    }

    const QStringList fields = arg.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
    if (fields.isEmpty() || fields.size() > 2) {
        return err(QStringLiteral("memory activate requires <index> [panId]"));
    }
    bool okIndex = false;
    const int memoryIndex = fields.first().toInt(&okIndex);
    if (!okIndex || memoryIndex < 0) {
        return err(QStringLiteral("memory index must be a non-negative integer"));
    }
    const QString preferredPanId = fields.size() == 2 ? fields.at(1) : QString();
    return m_memoryActivateHandler(memoryIndex, preferredPanId);
}

// ── Semantic transmitter keying (#3646 fidelity — item 3) ───────────────────
// `key ptt on|off` and `key mox` drive RadioModel::setTransmit — the exact
// calls the space-bar PTT event filter (MainWindow_Shortcuts.cpp) and the
// mox_toggle QShortcut make, which `invoke` cannot reach (one is an app-level
// QKeyEvent filter, the other a global QShortcut — neither is a named widget).
// We route to the model rather than synthesizing a QKeyEvent so it is
// deterministic and focus-independent. KEYING is gated by the same
// AETHER_AUTOMATION_ALLOW_TX rail as txtest/atu and arms the force-unkey
// watchdog; UNKEY is always allowed (it only reduces TX risk).
QJsonObject AutomationServer::doKey(const QString& name, const QString& arg)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    auto& tx = m_radioModel->transmitModel();
    const QString n = name.trimmed().toLower();
    const QString a = arg.trimmed().toLower();

    auto keyOn = [&](const QString& what) -> QJsonObject {
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: key '") + what
                       + QStringLiteral("' keys the transmitter — set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        m_radioModel->setTransmit(true);               // == space-bar PTT press (Mox)
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();  // arm watchdog window
        qCInfo(lcAutomation).noquote() << "key" << what << "ON (ALLOW_TX)";
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("key"), what},
                           {QStringLiteral("state"), QStringLiteral("on")}};
    };
    auto keyOff = [&](const QString& what) -> QJsonObject {
        m_radioModel->setTransmit(false);              // == space-bar PTT release
        m_txKeyedSinceMs = 0;
        qCInfo(lcAutomation).noquote() << "key" << what << "OFF";
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("key"), what},
                           {QStringLiteral("state"), QStringLiteral("off")}};
    };

    if (n == QLatin1String("ptt")) {
        if (a == QLatin1String("off") || a == QLatin1String("0")
            || a == QLatin1String("false") || a == QLatin1String("release"))
            return keyOff(QStringLiteral("ptt"));
        if (a.isEmpty() || a == QLatin1String("on") || a == QLatin1String("1")
            || a == QLatin1String("true") || a == QLatin1String("press"))
            return keyOn(QStringLiteral("ptt"));
        return err(QStringLiteral("key ptt requires on|off"));
    }
    if (n == QLatin1String("mox") || n == QLatin1String("mox_toggle")) {
        // mox_toggle is non-idempotent: toggling while keyed UNKEYS (ungated),
        // toggling while idle KEYS (gated). Mirrors the QShortcut handler.
        return tx.isTransmitting() ? keyOff(QStringLiteral("mox"))
                                   : keyOn(QStringLiteral("mox"));
    }
    return err(QStringLiteral("unknown key '") + name
               + QStringLiteral("' (use: ptt on|off, mox)"));
}

// ── CWX keyer (#3646; repro vehicle for #3804) ──────────────────────────────
// `cwx send <text>` keys CW for the string (gated, arms the watchdog), `cwx
// speed <wpm>` sets keyer speed, `cwx stop` aborts the buffer. Routes through
// the same CwxModel chokepoint the CWX panel uses (which emits `cwx send "..."`).
// The slice should be in a CW mode for the radio to emit; we don't enforce it
// here so a caller can stage mode first.
QJsonObject AutomationServer::doCwx(const QString& action, const QString& arg)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    CwxModel& cwx = m_radioModel->cwxModel();
    const QString a = action.trimmed().toLower();

    if (a == QLatin1String("speed") || a == QLatin1String("wpm")) {
        bool ok = false; const int wpm = arg.trimmed().toInt(&ok);
        if (!ok || wpm < 5 || wpm > 100)
            return err(QStringLiteral("cwx speed requires wpm in 5..100"));
        cwx.setSpeed(wpm);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("cwx"), QStringLiteral("speed")},
                           {QStringLiteral("wpm"), wpm}};
    }
    if (a == QLatin1String("stop") || a == QLatin1String("abort") || a == QLatin1String("clear")) {
        cwx.clearBuffer();
        m_txKeyedSinceMs = 0;
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("cwx"), QStringLiteral("stop")}};
    }
    if (a == QLatin1String("send")) {
        const QString text = arg.trimmed();
        if (text.isEmpty())
            return err(QStringLiteral("cwx send requires text"));
        if (!m_txAllowed)
            return err(QStringLiteral("blocked: cwx send keys the transmitter — "
                                      "set AETHER_AUTOMATION_ALLOW_TX=1 to allow"));
        m_txKeyedSinceMs = QDateTime::currentMSecsSinceEpoch();  // arm watchdog
        cwx.send(text);
        qCInfo(lcAutomation).noquote() << "cwx send" << text.length() << "chars (ALLOW_TX)";
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("cwx"), QStringLiteral("send")},
                           {QStringLiteral("chars"), text.length()}};
    }
    return err(QStringLiteral("unknown cwx action '") + action
               + QStringLiteral("' (use: send <text> | speed <wpm> | stop)"));
}

// ── Per-GUI-client station identity (#3646 fidelity — item 1) ───────────────
// Set `client station <name>` so other MultiFlex clients see the agent's name.
// This is the per-GUI-client station identity (FlexLib SetClientStationName),
// orthogonal to the radio-wide callsign — we NEVER write `radio callsign`,
// which is persisted on the radio's front panel. The station name is
// session-scoped and the radio drops it when this client disconnects, so it is
// inherently non-persistent; we additionally restore the user's real name on
// bridge stop so any live peer reverts immediately.
void AutomationServer::applyAgentStation(const QString& name)
{
    if (!m_radioModel || !m_radioModel->isConnected() || name.isEmpty())
        return;
    if (!m_stationApplied) {
        // Capture the user's real station name once, to restore it later. Same
        // fallback the connect handshake uses (AppSettings StationName → host).
        m_priorStationName = AppSettings::instance().value("StationName", "").toString();
        if (m_priorStationName.isEmpty())
            m_priorStationName = QSysInfo::machineHostName();
    }
    m_radioModel->sendCommand(QStringLiteral("client station %1").arg(name));
    m_stationApplied = true;
    qCInfo(lcAutomation).noquote() << "station name set to" << name
                                   << "(other MultiFlex clients will see this)";
}

void AutomationServer::restoreStation()
{
    if (!m_stationApplied || m_priorStationName.isEmpty())
        return;
    if (m_radioModel && m_radioModel->isConnected())
        m_radioModel->sendCommand(QStringLiteral("client station %1").arg(m_priorStationName));
    m_stationApplied = false;
}

QJsonObject AutomationServer::doStation(const QString& name)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    const QString n = name.trimmed();
    if (n.isEmpty())
        return err(QStringLiteral("station requires a name"));
    if (n.contains(QLatin1Char(' ')))
        return err(QStringLiteral("station name must be a single token (no spaces)"));
    if (!m_radioModel->isConnected())
        return err(QStringLiteral("not connected — connect to a radio before setting the station name"));
    m_agentStation = n;
    applyAgentStation(n);
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("station"), n}};
}

// ── QRZ callsign lookup (#3646) ─────────────────────────────────────────────
QJsonObject AutomationServer::doQrz(const QString& action, const QString& value)
{
    auto& svc = CallsignLookupService::instance();

    if (action == QLatin1String("status")) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("enabled"), svc.enabled()},
            {QStringLiteral("hasCredentials"), svc.hasCredentials()},
            {QStringLiteral("cacheEntries"), svc.cacheEntryCount()},
            {QStringLiteral("hasOwnLocation"), svc.hasOwnLocation()},
        };
    }
    if (action == QLatin1String("cached")) {
        const QString call = Callsigns::normalized(value);
        if (call.isEmpty())
            return err(QStringLiteral("qrz cached requires a callsign"));
        const CallsignInfo info = svc.cachedEntry(call);
        if (!info.isValid())
            return QJsonObject{{QStringLiteral("ok"), true},
                               {QStringLiteral("found"), false},
                               {QStringLiteral("call"), call}};
        QJsonObject o = info.toJson();
        o.insert(QStringLiteral("stale"),
                 info.isOlderThan(CallsignLookupService::kCacheTtlSec));
        o.insert(QStringLiteral("photoPath"), svc.photoPathFor(call));
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("found"), true},
                           {QStringLiteral("entry"), o}};
    }
    if (action == QLatin1String("lookup")) {
        const QString call = Callsigns::normalized(value);
        if (call.isEmpty())
            return err(QStringLiteral("qrz lookup requires a callsign"));
        // Shape-gate like the GUI dialog does: the bridge must not let an agent
        // drive unbounded authenticated QRZ queries over arbitrary tokens (#3990).
        if (!Callsigns::isLikelyCallsign(call))
            return err(QStringLiteral("qrz lookup: '") + call
                       + QStringLiteral("' is not a plausible callsign"));
        svc.lookup(call);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("queued"), true},
                           {QStringLiteral("call"), call},
                           {QStringLiteral("note"),
                            QStringLiteral("async — poll `qrz cached %1` for the result").arg(call)}};
    }
    if (action == QLatin1String("spottext")) {
        if (value.trimmed().isEmpty())
            return err(QStringLiteral("qrz spottext requires CW text, e.g. \"CQ CQ DE KI6BCJ KI6BCJ K\""));
        QWidget* mw = topLevelWindowForTarget({});
        QObject* spotter = mw ? mw->findChild<QObject*>(QStringLiteral("cwCallsignSpotter"))
                              : nullptr;
        if (!spotter)
            return err(QStringLiteral("CW callsign spotter not found (main window not up yet?)"));
        const bool ok = QMetaObject::invokeMethod(spotter, "feedText",
                                                  Qt::DirectConnection,
                                                  Q_ARG(QString, value));
        return QJsonObject{{QStringLiteral("ok"), ok},
                           {QStringLiteral("fed"), value}};
    }
    return err(QStringLiteral("qrz requires status|cached|lookup|spottext"));
}

// Resolve the top-level window a window-scoped verb acts on: the target's
// window() when given, else the QMainWindow (or first visible real top-level).
// Skips scroll-area viewports and popup QMenus so the main window wins. Shared by
// doResize and doWindow.
QWidget* AutomationServer::topLevelWindowForTarget(const QString& target)
{
    if (!target.isEmpty()) {
        QWidget* t = resolveWidget(target);
        return t ? t->window() : nullptr;
    }
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* tlw : tops)                 // prefer the QMainWindow
        if (tlw->inherits("QMainWindow")) return tlw;
    for (QWidget* tlw : tops) {               // else first visible real window
        if (tlw->objectName() == QLatin1String("qt_scrollarea_viewport")) continue;
        if (qobject_cast<QMenu*>(tlw)) continue;
        if (tlw->isWindow() && tlw->isVisible()) return tlw;
    }
    return nullptr;
}

// ── Window state (#3918) ─────────────────────────────────────────────────────
// Drive a top-level window's state so an agent can maximize / restore / minimize
// / fullscreen and prove it via dumpTree's `windowState`. resize only set
// explicit geometry, so an un-maximize (restore) was previously unverifiable.
// State changes don't spin a nested event loop, so they're safe to run
// synchronously here (unlike click/menu, which defer).
QJsonObject AutomationServer::doWindow(const QString& action, const QString& target) const
{
    const QString a = action.trimmed().toLower();
    if (a.isEmpty())
        return err(QStringLiteral("window needs an action "
                                  "(maximize|restore|minimize|fullscreen)"));
    if (!target.isEmpty() && !resolveWidget(target))
        return err(QStringLiteral("window not found for target: ") + target);
    QWidget* win = topLevelWindowForTarget(target);
    if (!win)
        return err(QStringLiteral("no top-level window to drive"));

    if (a == QLatin1String("maximize") || a == QLatin1String("max")) {
        win->showMaximized();
    } else if (a == QLatin1String("restore") || a == QLatin1String("normal")
               || a == QLatin1String("unmaximize")) {
        win->showNormal();
    } else if (a == QLatin1String("minimize") || a == QLatin1String("min")) {
        win->showMinimized();
    } else if (a == QLatin1String("fullscreen") || a == QLatin1String("full")) {
        win->showFullScreen();
    } else {
        return err(QStringLiteral("unknown window action: ") + action
                   + QStringLiteral(" (maximize|restore|minimize|fullscreen)"));
    }

    const Qt::WindowStates st = win->windowState();
    const char* ws = "normal";
    if (st & Qt::WindowMinimized)       ws = "minimized";
    else if (st & Qt::WindowFullScreen) ws = "fullscreen";
    else if (st & Qt::WindowMaximized)  ws = "maximized";
    qCInfo(lcAutomation).noquote() << "window" << a << "->" << ws;
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("action"), a},
        {QStringLiteral("windowState"), QLatin1String(ws)},
        {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("w"), win->width()},
                                                 {QStringLiteral("h"), win->height()}}},
    };
}

// ── Fire a ShortcutManager action by id (MIDI/shortcut path) ────────────────
// MIDI controller mappings dispatch by calling the registered ShortcutManager
// action's handler (fireShortcut in MainWindow_Controllers.cpp). Actions with no
// default key sequence and no menu entry — Band Zoom, Segment Zoom, and every
// other MIDI-only trigger — are otherwise unreachable by the bridge, so this
// verb exercises exactly that path.
//
// TX-safety: actions registered keysTx (MOX/TUNE/two-tone/ATU start/PTT hold/CW
// keys — declared at each registerAction site, the same single-source pattern
// as markTxKeying for widgets) are refused unless AETHER_AUTOMATION_ALLOW_TX is
// set. The gate reads the registration flag, not a bridge-side id list that
// can drift (#4057 review: a hand-kept list here missed atu_start on day one).
//
// The handler runs synchronously in the socket callback; today's handlers only
// sendCommand()/toggle model state or defer UI work themselves (go_to_freq
// single-shots into the VFO entry), so no nested event loop. fired:true means
// the handler RAN — handlers validate preconditions (connected, active slice)
// and may no-op; verify effects via get/dumpTree, exactly like a MIDI press.
QJsonObject AutomationServer::doShortcut(const QString& id) const
{
    if (id.isEmpty()) {
        return err(QStringLiteral("shortcut requires an action id, e.g. 'band_zoom'"));
    }

    QWidget* mw = primaryTopLevelWindow();
    if (!mw) {
        return err(QStringLiteral("no main window to dispatch shortcut"));
    }

    const bool allowTx = m_txAllowed;
    int result = -1;
    const bool invoked = QMetaObject::invokeMethod(
        mw, "fireShortcutAction", Qt::DirectConnection,
        Q_RETURN_ARG(int, result), Q_ARG(QString, id), Q_ARG(bool, allowTx));
    if (!invoked) {
        return err(QStringLiteral("fireShortcutAction not invokable on main window"));
    }

    switch (result) {
    case 0:  // MainWindow::ShortcutFireOk
        break;
    case 1:  // ShortcutFireUnknownId
        return err(QStringLiteral("unknown shortcut action id: ") + id);
    case 2:  // ShortcutFireNoDirectHandler
        return err(QStringLiteral("'") + id
                   + QStringLiteral("' exists but is event-filter-driven (momentary "
                                    "key action) — it has no direct handler the "
                                    "bridge can fire"));
    case 3:  // ShortcutFireTxBlocked
        qCWarning(lcAutomation).noquote()
            << "BLOCKED transmit-keying shortcut" << id;
        return err(QStringLiteral("blocked: '") + id
                   + QStringLiteral("' keys the transmitter (TX-safety guard). "
                                    "Set AETHER_AUTOMATION_ALLOW_TX=1 to override."));
    default:
        return err(QStringLiteral("unexpected fireShortcutAction result for '")
                   + id + QStringLiteral("'"));
    }

    qCInfo(lcAutomation).noquote() << "shortcut fired:" << id;
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("shortcut"), id},
        {QStringLiteral("fired"), true},
    };
}

QJsonObject AutomationServer::doMidi(const QString& action, const QString& value) const
{
    if (action.compare(QStringLiteral("cc"), Qt::CaseInsensitive) != 0) {
        return err(QStringLiteral("midi requires 'cc <0-127>'"));
    }

    bool okValue = false;
    const int ccValue = value.toInt(&okValue);
    if (!okValue || ccValue < 0 || ccValue > 127) {
        return err(QStringLiteral("midi cc value must be an integer from 0 to 127"));
    }

    QWidget* mw = primaryTopLevelWindow();
    if (!mw) {
        return err(QStringLiteral("no main window to dispatch MIDI CC"));
    }

    int result = -1;
    const bool invoked = QMetaObject::invokeMethod(
        mw, "injectMidiVfoCcForAutomation", Qt::DirectConnection,
        Q_RETURN_ARG(int, result), Q_ARG(int, ccValue));
    if (!invoked) {
        return err(QStringLiteral("MIDI automation injection is unavailable"));
    }
    if (result == 1) {
        return err(QStringLiteral("MIDI support is unavailable in this build"));
    }
    if (result != 0) {
        return err(QStringLiteral("MIDI CC value was rejected"));
    }

    qCInfo(lcAutomation).noquote() << "MIDI VFO CC injected:" << ccValue;
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("midi"), QStringLiteral("cc")},
        {QStringLiteral("value"), ccValue},
        {QStringLiteral("paramId"), QStringLiteral("rx.tuneKnob")},
        {QStringLiteral("accepted"), true},
    };
}

// ── Headless render size (#3646 fidelity — item 8) ──────────────────────────
// Resize a top-level window so the panadapter x_pixels (== SpectrumWidget
// width) propagates to a realistic value — under QT_QPA_PLATFORM=offscreen the
// window collapses to the tiny virtual screen and x_pixels stalls near the
// floor, so render-size-dependent code (FFT/waterfall stream rate, rhiFlush
// composite cost, burst regressions) is never exercised. We resize the WINDOW
// (not force xpixels via the radio command) so the local FFT decoder, the
// dimensionsChanged debounce, and the radio stay in sync.
QJsonObject AutomationServer::doResize(const QString& value, const QString& target) const
{
    int w = 0, h = 0;
    const QString v = value.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("default") || v == QLatin1String("full")) {
        w = 1920; h = 1080;   // realistic full size → SpectrumWidth ~1873
    } else {
        QString s = value;
        s.replace(QLatin1Char('x'), QLatin1Char(' '));
        s.replace(QLatin1Char('X'), QLatin1Char(' '));
        const QStringList parts = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() >= 2) { w = parts.at(0).toInt(); h = parts.at(1).toInt(); }
        else if (parts.size() == 1) { w = parts.at(0).toInt(); h = w * 9 / 16; }
    }
    if (w <= 0 || h <= 0)
        return err(QStringLiteral("resize requires <width> <height> (e.g. 'resize 1920 1080', or 'full')"));

    if (!target.isEmpty() && !resolveWidget(target))
        return err(QStringLiteral("window not found for target: ") + target);
    QWidget* win = topLevelWindowForTarget(target);
    if (!win)
        return err(QStringLiteral("no top-level window to resize"));

    // The status-bar min-width recompute caps min width to the screen on a small
    // offscreen virtual screen; drop the floor so the resize isn't re-clamped.
    if (win->minimumWidth() > w)
        win->setMinimumWidth(0);
    win->resize(w, h);

    QJsonObject r{
        {QStringLiteral("ok"), true},
        {QStringLiteral("requested"), QJsonObject{{QStringLiteral("w"), w}, {QStringLiteral("h"), h}}},
        {QStringLiteral("actual"), QJsonObject{{QStringLiteral("w"), win->width()},
                                               {QStringLiteral("h"), win->height()}}},
    };
    // Surface the panadapter width — that IS x_pixels, the thing an agent asserts
    // on after the ~300ms dimensionsChanged debounce re-pushes it to the radio.
    if (QWidget* sw = resolveWidget(QStringLiteral("SpectrumWidget")))
        r[QStringLiteral("spectrumWidth")] = sw->width();
    return r;
}

// ── Menu-bar discovery / popup (#3646 fidelity — items 5 + 7) ────────────────
// `menu list` enumerates the menu-bar tree (so an agent can discover exact
// labels to invoke); `menu open <name>` pops a top-level menu non-blocking so a
// follow-up dumpTree/grab can snapshot it. Triggering a leaf is done via
// `invoke <label> trigger` (resolveMenuBarAction), not here.
namespace {
QJsonArray describeMenuActions(QMenu* menu)
{
    QJsonArray arr;
    for (QAction* a : menu->actions()) {
        if (a->isSeparator())
            continue;
        // Section headers (disabled QWidgetAction + QLabel) read their text from
        // the label, not the empty action text — same idiom doDumpTree handles
        // via describeAction (#3858).
        QString headerText;
        if (auto* wa = qobject_cast<const QWidgetAction*>(a)) {
            if (auto* lbl = qobject_cast<const QLabel*>(wa->defaultWidget()))
                headerText = lbl->text();
        }
        QJsonObject o{{QStringLiteral("text"),
                       headerText.isEmpty() ? actionDisplayText(a) : headerText},
                      {QStringLiteral("enabled"), a->isEnabled()}};
        if (!headerText.isEmpty())
            o[QStringLiteral("type")] = QStringLiteral("header");
        if (a->isCheckable()) {
            o[QStringLiteral("checkable")] = true;
            o[QStringLiteral("checked")] = a->isChecked();
        }
        if (QMenu* sub = a->menu())
            o[QStringLiteral("submenu")] = describeMenuActions(sub);
        arr.append(o);
    }
    return arr;
}

// The app's top-level menus, coping with BOTH a regular QMenuBar and the macOS
// NATIVE menu bar (where each menu is reparented to a top-level QMenu widget,
// invisible to QMainWindow::menuBar()->actions()). De-duplicated, in discovery
// order. On macOS this also surfaces submenus as their own entries — harmless.
QList<QPair<QString, QMenu*>> collectTopMenus()
{
    QList<QPair<QString, QMenu*>> out;
    QSet<QMenu*> seen;
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget* tlw : tops) {
        auto* mw = qobject_cast<QMainWindow*>(tlw);
        if (!mw || !mw->menuBar())
            continue;
        for (QAction* a : mw->menuBar()->actions()) {
            if (QMenu* sub = a->menu(); sub && !seen.contains(sub)) {
                seen.insert(sub);
                out.append({actionDisplayText(a), sub});
            }
        }
    }
    for (QWidget* tlw : tops) {
        auto* menu = qobject_cast<QMenu*>(tlw);
        if (!menu || seen.contains(menu))
            continue;
        const QString title = actionDisplayText(menu->menuAction());
        if (title.isEmpty())
            continue;   // a context/combo popup, not a named menu-bar menu
        seen.insert(menu);
        out.append({title, menu});
    }
    return out;
}
} // namespace

QJsonObject AutomationServer::doMenu(const QString& action, const QString& arg) const
{
    const QList<QPair<QString, QMenu*>> menus = collectTopMenus();
    if (menus.isEmpty())
        return err(QStringLiteral("no menus found"));

    if (action == QLatin1String("list")) {
        QJsonArray arr;
        for (const auto& m : menus)
            arr.append(QJsonObject{{QStringLiteral("title"), m.first},
                                   {QStringLiteral("actions"), describeMenuActions(m.second)}});
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("menus"), arr}};
    }
    if (action == QLatin1String("open")) {
        if (arg.isEmpty())
            return err(QStringLiteral("menu open requires a menu name"));
        for (const auto& m : menus) {
            if (m.first.compare(arg, Qt::CaseInsensitive) != 0
                && !actionMatchesTarget(m.second->menuAction(), arg))
                continue;
            // popup() is non-blocking (unlike exec()), so it can't deadlock the
            // socket handler on the GUI thread.
            m.second->popup(QPoint(200, 200));
            return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("menu"), QStringLiteral("open")},
                               {QStringLiteral("title"), m.first}};
        }
        return err(QStringLiteral("menu not found: ") + arg);
    }
    return err(QStringLiteral("unknown menu action: ") + action + QStringLiteral(" (list|open)"));
}

// ── Window close (#3646 fidelity) ───────────────────────────────────────────
// Close the target's top-level window. We call window()->close() rather than
// synthesizing a click on the custom frameless title-bar close QLabel ("Close
// window") — close() is what that QLabel's handler invokes anyway, and it works
// for ANY window (native or frameless), so `invoke … click` is no longer the
// only path. Deferred to a clean main-loop turn because a closeEvent can pop a
// confirm dialog (a nested event loop) which must not re-enter the socket-read
// callback.
QJsonObject AutomationServer::doClose(const QString& target) const
{
    QWidget* w = resolveWidget(target);
    if (!w)
        return err(QStringLiteral("widget not found: ") + target);
    QWidget* win = w->window();
    if (!win)
        return err(QStringLiteral("no top-level window for target: ") + target);

    const QString title = win->windowTitle();
    const QString cls = shortClassName(win);
    QPointer<QWidget> wg = win;
    QTimer::singleShot(0, qApp, [wg]() {
        if (wg) wg->close();
    });
    qCInfo(lcAutomation).noquote() << "close window for" << target << "(" << cls << ")";

    QJsonObject r{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), cls},
        {QStringLiteral("deferred"), true},   // re-read dumpTree to confirm it's gone
    };
    if (!title.isEmpty())
        r[QStringLiteral("title")] = title;
    return r;
}

QJsonObject AutomationServer::pointerSafetyError(const QWidget* widget,
                                                 const QString& target,
                                                 const QString& verb) const
{
    if (!widget->isVisible()) {
        return err(QStringLiteral("refused: '") + target
                   + QStringLiteral("' is not visible"));
    }
    if (!widget->isEnabled()) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("refused: '") + target
                 + QStringLiteral("' is disabled — pointer input would be dropped")},
            {QStringLiteral("disabled"), true},
            {QStringLiteral("class"), shortClassName(widget)},
        };
    }

    if (!m_txAllowed) {
        for (const QWidget* parent = widget; parent; parent = parent->parentWidget()) {
            if (!isTransmitControl(parent)) {
                continue;
            }
            qCWarning(lcAutomation).noquote()
                << "BLOCKED transmit-related" << verb << "on" << target
                << "(keying control in chain:" << shortClassName(parent) << ')';
            return err(QStringLiteral("blocked: '") + target
                       + QStringLiteral("' resolves into a transmit-keying control "
                                        "(TX-safety guard). Enable \"Allow TX via MCP\" "
                                        "in Radio Setup → Network (or set "
                                        "AETHER_AUTOMATION_ALLOW_TX=1) to override."));
        }
    }

    if (m_txMaxPower >= 0) {
        for (const QWidget* parent = widget; parent; parent = parent->parentWidget()) {
            const QString accessibleName = parent->accessibleName();
            if (accessibleName != QLatin1String("RF power")
                && accessibleName != QLatin1String("Tune power")) {
                continue;
            }
            qCWarning(lcAutomation).noquote()
                << "BLOCKED" << verb << "on power slider" << accessibleName
                << "— power ceiling" << m_txMaxPower << "is armed";
            return err(QStringLiteral("blocked: '") + accessibleName
                       + QStringLiteral("' pointer input would bypass the power "
                                        "ceiling (AETHER_AUTOMATION_TX_MAX_POWER). "
                                        "Use `invoke setValue`, which clamps."));
        }
    }

    return {};
}

// ── Mouse-drag gesture synthesis (#3646 fidelity) ───────────────────────────
// `drag <target> <dx> <dy>` synthesizes a press → moves → release so a resize
// grip or slider handle is provable end-to-end, not just via seed + read-back.
// All global coordinates are computed ONCE from the press position; we never
// re-map after a move. That matters for a QSizeGrip, whose parent (and therefore
// the grip itself) shifts as the window resizes — re-mapping mid-drag would feed
// the grip a compounding delta and overshoot the requested size.
QJsonObject AutomationServer::doDrag(const QString& target, const QString& value) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return err(QStringLiteral("widget or window not found: ") + target);
    }
    const QJsonObject safetyError = pointerSafetyError(
        w, target, QStringLiteral("drag"));
    if (!safetyError.isEmpty()) {
        return safetyError;
    }

    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        return err(QStringLiteral("drag requires '<dx> <dy>' in pixels (e.g. 'drag sizeGrip 80 60')"));
    }
    bool okx = false, oky = false;
    const int dx = parts.at(0).toInt(&okx);
    const int dy = parts.at(1).toInt(&oky);
    if (!okx || !oky) {
        return err(QStringLiteral("drag dx/dy must be integers"));
    }

    const QPoint start(w->width() / 2, w->height() / 2);
    const QPoint globalStart = w->mapToGlobal(start);

    QPointer<QWidget> wp = w;
    auto send = [&](QEvent::Type type, const QPoint& off,
                    Qt::MouseButton button, Qt::MouseButtons buttons) -> bool {
        if (!wp)
            return false;
        const QPoint local = start + off;
        const QPoint global = globalStart + off;
        QMouseEvent ev(type, QPointF(local), QPointF(local), QPointF(global),
                       button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(wp, &ev);
        return wp != nullptr;
    };

    // press, then thirds of the travel, then release — fixed-base offsets.
    send(QEvent::MouseButtonPress, QPoint(0, 0), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx / 3, dy / 3), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx * 2 / 3, dy * 2 / 3), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx, dy), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPoint(dx, dy), Qt::LeftButton, Qt::NoButton);

    qCInfo(lcAutomation).noquote()
        << "drag" << target << "by" << dx << dy;

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), wp ? shortClassName(wp) : QStringLiteral("(deleted)")},
        {QStringLiteral("dx"), dx},
        {QStringLiteral("dy"), dy},
    };
}

QJsonObject AutomationServer::doDragAt(const QString& target, const QString& value) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return err(QStringLiteral("widget or window not found: ") + target);
    }
    const QJsonObject safetyError = pointerSafetyError(
        w, target, QStringLiteral("dragAt"));
    if (!safetyError.isEmpty()) {
        return safetyError;
    }

    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 4) {
        return err(QStringLiteral(
            "dragAt requires '<x> <y> <dx> <dy> [modifiers]' in pixels"));
    }

    bool okx = false, oky = false, okdx = false, okdy = false;
    const int x = parts.at(0).toInt(&okx);
    const int y = parts.at(1).toInt(&oky);
    const int dx = parts.at(2).toInt(&okdx);
    const int dy = parts.at(3).toInt(&okdy);
    if (!okx || !oky || !okdx || !okdy) {
        return err(QStringLiteral("dragAt x/y/dx/dy must be integers"));
    }

    const QPoint start(x, y);
    if (!w->rect().contains(start)) {
        return err(QStringLiteral("dragAt start point is outside the target widget"));
    }

    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    if (parts.size() > 4) {
        QString modifierText = parts.mid(4).join(QLatin1Char(','));
        modifierText.replace(QLatin1Char('+'), QLatin1Char(','));
        const QStringList modifierParts =
            modifierText.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& raw : modifierParts) {
            const QString modifier = raw.trimmed().toLower();
            if (modifier == QStringLiteral("control") || modifier == QStringLiteral("ctrl")) {
                modifiers |= Qt::ControlModifier;
            } else if (modifier == QStringLiteral("meta")
                       || modifier == QStringLiteral("command")
                       || modifier == QStringLiteral("cmd")) {
                modifiers |= Qt::MetaModifier;
            } else if (modifier == QStringLiteral("shift")) {
                modifiers |= Qt::ShiftModifier;
            } else if (modifier == QStringLiteral("alt")
                       || modifier == QStringLiteral("option")) {
                modifiers |= Qt::AltModifier;
            } else if (modifier != QStringLiteral("none")) {
                return err(QStringLiteral("dragAt unknown modifier: ") + raw);
            }
        }
    }

    const QPoint globalStart = w->mapToGlobal(start);
    QPointer<QWidget> wp = w;
    auto send = [&](QEvent::Type type, const QPoint& off,
                    Qt::MouseButton button, Qt::MouseButtons buttons) -> bool {
        if (!wp) {
            return false;
        }
        const QPoint local = start + off;
        const QPoint global = globalStart + off;
        QMouseEvent ev(type, QPointF(local), QPointF(local), QPointF(global),
                       button, buttons, modifiers);
        QCoreApplication::sendEvent(wp, &ev);
        return wp != nullptr;
    };

    send(QEvent::MouseButtonPress, QPoint(0, 0), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx / 3, dy / 3), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx * 2 / 3, dy * 2 / 3), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(dx, dy), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPoint(dx, dy), Qt::LeftButton, Qt::NoButton);

    qCInfo(lcAutomation).noquote()
        << "dragAt" << target << "from" << start << "by" << dx << dy
        << "modifiers" << static_cast<int>(modifiers);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), wp ? shortClassName(wp) : QStringLiteral("(deleted)")},
        {QStringLiteral("x"), x},
        {QStringLiteral("y"), y},
        {QStringLiteral("dx"), dx},
        {QStringLiteral("dy"), dy},
        {QStringLiteral("modifiers"), static_cast<int>(modifiers)},
    };
}

// `gesture` keeps the left button down across requests on one QLocalSocket.
// This is deliberately connection-owned: an MCP wrapper can hold that socket
// while ordinary tools use independent short-lived sockets, so queued model or
// radio updates and separate bridge requests get normal main-loop turns while
// QAbstractSlider::isSliderDown() remains true. Losing the owner is the cleanup
// signal; no caller-supplied session id can outlive its transport.
QJsonObject AutomationServer::doGesture(const QString& action,
                                        const QString& target,
                                        const QString& value,
                                        QLocalSocket* sock)
{
    const QString normalizedAction = action.trimmed().toLower();

    auto active = [this]() {
        return m_pointerGesture.owner && m_pointerGesture.widget;
    };
    auto response = [this, sock, &active]() {
        QJsonObject result{
            {QStringLiteral("ok"), true},
            {QStringLiteral("active"), active()},
            {QStringLiteral("leaseMs"), kPointerGestureLeaseMs},
        };
        if (!active()) {
            return result;
        }
        result[QStringLiteral("target")] = m_pointerGesture.target;
        result[QStringLiteral("class")] = shortClassName(m_pointerGesture.widget);
        result[QStringLiteral("dx")] = m_pointerGesture.offset.x();
        result[QStringLiteral("dy")] = m_pointerGesture.offset.y();
        result[QStringLiteral("ownedByCaller")] = m_pointerGesture.owner == sock;
        if (const auto* slider =
                qobject_cast<const QAbstractSlider*>(m_pointerGesture.widget.data())) {
            result[QStringLiteral("sliderDown")] = slider->isSliderDown();
            result[QStringLiteral("value")] = slider->value();
        }
        return result;
    };
    auto parsePoint = [](const QString& text, bool optional, bool coordinates,
                         QPoint* point) -> QString {
        const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (optional && parts.isEmpty()) {
            return {};
        }
        if (parts.size() != 2) {
            return coordinates
                ? QStringLiteral("requires exactly '<x> <y>' integer coordinates")
                : QStringLiteral("requires exactly '<dx> <dy>' integer offsets");
        }
        bool okX = false;
        bool okY = false;
        const int x = parts.at(0).toInt(&okX);
        const int y = parts.at(1).toInt(&okY);
        if (!okX || !okY) {
            return coordinates
                ? QStringLiteral("coordinates must be integers")
                : QStringLiteral("offsets must be integers");
        }
        *point = QPoint(x, y);
        return {};
    };
    auto send = [this](QEvent::Type type, Qt::MouseButton button,
                       Qt::MouseButtons buttons) -> bool {
        if (!m_pointerGesture.widget) {
            return false;
        }
        const QPoint local = m_pointerGesture.startLocal + m_pointerGesture.offset;
        const QPoint global = m_pointerGesture.globalStart + m_pointerGesture.offset;
        QMouseEvent event(type, QPointF(local), QPointF(local), QPointF(global),
                          button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(m_pointerGesture.widget, &event);
        return m_pointerGesture.widget != nullptr;
    };

    if (normalizedAction == QLatin1String("status")) {
        if (!m_pointerGesture.owner || !m_pointerGesture.widget) {
            cancelGesture(nullptr, QStringLiteral("gesture target or owner disappeared"));
        }
        return response();
    }

    if (!sock) {
        return err(QStringLiteral("gesture requires a live client connection"));
    }

    if (normalizedAction == QLatin1String("begin")) {
        if (active()) {
            return err(QStringLiteral("another phaseful gesture is already active"));
        }
        if (target.isEmpty()) {
            return err(QStringLiteral("gesture begin requires a target"));
        }
        QWidget* widget = resolveWidget(target);
        if (!widget) {
            return err(QStringLiteral("widget or window not found: ") + target);
        }
        const QJsonObject safetyError = pointerSafetyError(
            widget, target, QStringLiteral("gesture"));
        if (!safetyError.isEmpty()) {
            return safetyError;
        }

        QPoint start(widget->rect().center());
        if (!value.trimmed().isEmpty()) {
            const QString coordinateError = parsePoint(value, false, true, &start);
            if (!coordinateError.isEmpty()) {
                return err(QStringLiteral("gesture begin ") + coordinateError);
            }
            if (!widget->rect().contains(start)) {
                return err(QStringLiteral("gesture begin point is outside '")
                           + target + QStringLiteral("'"));
            }
        }

        m_pointerGesture.owner = sock;
        m_pointerGesture.widget = widget;
        m_pointerGesture.target = target;
        m_pointerGesture.startLocal = start;
        m_pointerGesture.globalStart = widget->mapToGlobal(start);
        m_pointerGesture.offset = QPoint();

        if (!send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton)) {
            cancelGesture(sock, QStringLiteral("gesture target disappeared during press"));
            return err(QStringLiteral("gesture target disappeared during press"));
        }

        if (!m_pointerGestureTimer) {
            m_pointerGestureTimer = new QTimer(this);
            m_pointerGestureTimer->setSingleShot(true);
            connect(m_pointerGestureTimer, &QTimer::timeout, this, [this]() {
                cancelGesture(nullptr, QStringLiteral("gesture inactivity timeout"));
            });
        }
        m_pointerGestureTimer->start(kPointerGestureLeaseMs);
        qCInfo(lcAutomation).noquote() << "gesture begin" << target << "at" << start;
        return response();
    }

    if (!active() || m_pointerGesture.owner != sock) {
        return err(QStringLiteral("no phaseful gesture is owned by this client"));
    }

    if (normalizedAction == QLatin1String("cancel")) {
        cancelGesture(sock, QStringLiteral("gesture cancelled by client"));
        return response();
    }

    if (normalizedAction != QLatin1String("move")
        && normalizedAction != QLatin1String("end")) {
        cancelGesture(sock, QStringLiteral("invalid gesture continuation"));
        return err(QStringLiteral("gesture action must be begin, move, end, cancel, or status"));
    }

    QPoint offset = m_pointerGesture.offset;
    const bool hasFinalOffset = normalizedAction == QLatin1String("end")
        && !value.trimmed().isEmpty();
    const QString offsetError = parsePoint(
        value, normalizedAction == QLatin1String("end"), false, &offset);
    if (!offsetError.isEmpty()) {
        cancelGesture(sock, QStringLiteral("invalid gesture offset"));
        return err(QStringLiteral("gesture ") + normalizedAction + QLatin1Char(' ')
                   + offsetError + QStringLiteral("; gesture released"));
    }
    m_pointerGesture.offset = offset;

    if (normalizedAction == QLatin1String("move") || hasFinalOffset) {
        if (!send(QEvent::MouseMove, Qt::NoButton, Qt::LeftButton)) {
            cancelGesture(sock, QStringLiteral("gesture target disappeared during move"));
            return err(QStringLiteral("gesture target disappeared during move"));
        }
    }
    if (normalizedAction == QLatin1String("move")) {
        m_pointerGestureTimer->start(kPointerGestureLeaseMs);
        return response();
    }

    const QString endedTarget = m_pointerGesture.target;
    const QString endedClass = shortClassName(m_pointerGesture.widget);
    const QPoint endedOffset = m_pointerGesture.offset;
    cancelGesture(sock, QStringLiteral("gesture ended by client"));
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("active"), false},
        {QStringLiteral("target"), endedTarget},
        {QStringLiteral("class"), endedClass},
        {QStringLiteral("dx"), endedOffset.x()},
        {QStringLiteral("dy"), endedOffset.y()},
    };
}

void AutomationServer::cancelGesture(QLocalSocket* owner, const QString& reason)
{
    if (owner && m_pointerGesture.owner != owner) {
        return;
    }
    if (!m_pointerGesture.owner && !m_pointerGesture.widget) {
        return;
    }

    if (m_pointerGestureTimer) {
        m_pointerGestureTimer->stop();
    }

    const QString target = m_pointerGesture.target;
    if (m_pointerGesture.widget) {
        const QPoint local = m_pointerGesture.startLocal + m_pointerGesture.offset;
        const QPoint global = m_pointerGesture.globalStart + m_pointerGesture.offset;
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(local), QPointF(local), QPointF(global),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_pointerGesture.widget, &release);
    }

    m_pointerGesture = PointerGesture{};
    qCInfo(lcAutomation).noquote()
        << "gesture release" << target << "—" << reason;
}

// hover <target> [leave]: synthesize pointer hover so hover-driven UI is
// provable. Bare form fires QEnterEvent + a no-button QMouseMove at the widget
// centre (mouse tracking is on for hover-aware widgets), which is what the
// HGauge meter readout listens for. The 'leave' form fires QEvent::Leave so a
// driver can watch the value badge fade one second after the pointer exits.
QJsonObject AutomationServer::doHover(const QString& target, const QString& action) const
{
    QWidget* w = resolveWidget(target);
    if (!w)
        return err(QStringLiteral("widget or window not found: ") + target);
    if (!w->isVisible())
        return err(QStringLiteral("refused: '") + target + QStringLiteral("' is not visible"));

    const QPoint center(w->width() / 2, w->height() / 2);
    const QPoint global = w->mapToGlobal(center);
    const bool leave = (action == QLatin1String("leave"));

    const QPointF localF = QPointF(center);
    const QPointF globalF = QPointF(global);
    if (leave) {
        QEvent ev(QEvent::Leave);
        QCoreApplication::sendEvent(w, &ev);
    } else {
        QEnterEvent enter(localF, localF, globalF);
        QCoreApplication::sendEvent(w, &enter);
        QMouseEvent move(QEvent::MouseMove, localF, localF, globalF,
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &move);
    }

    qCInfo(lcAutomation).noquote()
        << "hover" << target << (leave ? "leave" : "enter") << "at" << global;

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("action"), leave ? QStringLiteral("leave")
                                         : QStringLiteral("enter")},
        {QStringLiteral("x"), global.x()},
        {QStringLiteral("y"), global.y()},
    };
}

// tooltip <target> [hide]: explicitly ask the target widget to show its native
// Qt tooltip. Synthetic hover is useful for hover-driven app UI, but platforms
// do not always run the built-in tooltip timer for injected events under
// offscreen automation. Sending QEvent::ToolTip uses the same widget event path
// as a real hover, so a driver can `grab QTipLabel` for PR evidence.
QJsonObject AutomationServer::doTooltip(const QString& target,
                                        const QString& action,
                                        const QString& value) const
{
    if (action == QLatin1String("hide")) {
        // Validate the target like the show path — a typo'd target must not
        // return ok:true (#4122 review). The tip itself is global, so the
        // target is only checked, not used.
        if (!resolveWidget(target)) {
            return err(QStringLiteral("widget or window not found: ") + target);
        }
        bool hidden = false;
        if (QWidget* tip = resolveWidget(QStringLiteral("QTipLabel"))) {
            tip->hide();
            hidden = true;
        }
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("action"), QStringLiteral("hide")},
                           {QStringLiteral("hidden"), hidden}};
    }

    QPointer<QWidget> w = resolveWidget(target);
    if (!w) {
        return err(QStringLiteral("widget or window not found: ") + target);
    }
    if (!w->isVisible()) {
        return err(QStringLiteral("refused: '") + target + QStringLiteral("' is not visible"));
    }

    const QString text = value.isEmpty() ? w->toolTip() : value;
    if (text.isEmpty()) {
        return err(QStringLiteral("target has no tooltip: ") + target);
    }

    const QPoint center(w->width() / 2, w->height() / 2);
    const QPoint global = w->mapToGlobal(center);

    const QString originalToolTip = w->toolTip();
    const bool overrideToolTip = !value.isEmpty() && value != originalToolTip;
    if (overrideToolTip) {
        w->setToolTip(value);
    }

    QHelpEvent event(QEvent::ToolTip, center, global);
    QCoreApplication::sendEvent(w, &event);
    const bool accepted = event.isAccepted();

    // QPointer guard (#4122 review): a ToolTip handler that rebuilds UI can
    // destroy the target during sendEvent — restoring through a raw pointer
    // would be a use-after-free.
    if (!w) {
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("target"), target},
            {QStringLiteral("text"), text},
            {QStringLiteral("accepted"), accepted},
            {QStringLiteral("targetDestroyed"), true},
            {QStringLiteral("grabHint"), QStringLiteral("QTipLabel")},
        };
    }
    if (overrideToolTip) {
        w->setToolTip(originalToolTip);
    }

    qCInfo(lcAutomation).noquote()
        << "tooltip" << target << "at" << global << text;

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("text"), text},
        {QStringLiteral("x"), global.x()},
        {QStringLiteral("y"), global.y()},
        {QStringLiteral("accepted"), accepted},
        {QStringLiteral("grabHint"), QStringLiteral("QTipLabel")},
    };
}

QJsonObject AutomationServer::doScrollTo(const QString& target) const
{
    QWidget* w = resolveWidget(target);
    if (!w)
        return err(QStringLiteral("widget or window not found: ") + target);

    QScrollArea* area = nullptr;
    for (QWidget* p = w->parentWidget(); p; p = p->parentWidget()) {
        if (auto* sa = qobject_cast<QScrollArea*>(p)) {
            area = sa;
            break;
        }
    }
    if (!area)
        return err(QStringLiteral("'") + target
                   + QStringLiteral("' has no QScrollArea ancestor to scroll"));

    area->ensureWidgetVisible(w);

    // Round-trip confirmation: the scrollbar position that resulted and
    // whether the widget's rect now intersects the viewport.
    const int vValue = area->verticalScrollBar()
        ? area->verticalScrollBar()->value() : 0;
    const int hValue = area->horizontalScrollBar()
        ? area->horizontalScrollBar()->value() : 0;
    const QRect vp = area->viewport()->rect()
        .translated(area->viewport()->mapToGlobal(QPoint(0, 0)));
    const QRect wr = w->rect().translated(w->mapToGlobal(QPoint(0, 0)));

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("scrollArea"), shortClassName(area)},
        {QStringLiteral("vScroll"), vValue},
        {QStringLiteral("hScroll"), hValue},
        {QStringLiteral("inViewport"), vp.intersects(wr)},
    };
}

// ── Button drop-down popup (#3646 fidelity) ─────────────────────────────────
// `showMenu <target>` pops a QToolButton/QPushButton drop-down menu. The show is
// POSTED onto the GUI event loop (singleShot(0)) and the owning window is
// raised+activated first — showing the native popup window from inside the
// socket-read callback, or while the app is backgrounded, re-enters Cocoa and
// segfaults in QWindow::geometry(). Raising is unconditional here (unlike the
// gated raiseWindowForPopup used during sweeps) because an explicit showMenu IS
// a request to bring the menu to the foreground.
QJsonObject AutomationServer::doShowMenu(const QString& target) const
{
    QWidget* w = resolveWidget(target);
    if (!w)
        return err(QStringLiteral("widget not found: ") + target);
    if (!w->isVisible())
        return err(QStringLiteral("refused: '") + target + QStringLiteral("' is not visible"));

    QMenu* menu = nullptr;
    if (auto* tb = qobject_cast<QToolButton*>(w))
        menu = tb->menu();
    else if (auto* pb = qobject_cast<QPushButton*>(w))
        menu = pb->menu();
    if (!menu)
        return err(QStringLiteral("'") + target
                   + QStringLiteral("' has no drop-down menu (expected a QToolButton/QPushButton with menu())"));

    QPointer<QMenu> mg = menu;
    QPointer<QWidget> bg = w;
    QPointer<QWidget> win = w->window();
    QTimer::singleShot(0, qApp, [mg, bg, win]() {
        if (!mg || !bg)
            return;
        if (win && win->isVisible()) {   // realize + activate so Cocoa has an anchor
            win->raise();
            win->activateWindow();
        }
        mg->popup(bg->mapToGlobal(QPoint(0, bg->height())));
    });
    qCInfo(lcAutomation).noquote() << "showMenu on" << target;

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("deferred"), true},   // popup runs next turn; dumpTree to read it
    };
}

// ── Custom right-click context menu (#3858) ─────────────────────────────────
// `contextMenu <target> [x y]` triggers a widget's custom right-click menu —
// the kind built on demand in a customContextMenuRequested handler or an
// overridden contextMenuEvent, which showMenu can't reach (it only follows
// QToolButton/QPushButton::menu()). We synthesize a QContextMenuEvent at the
// widget center (or an optional local offset) and route it through the widget's
// event() so Qt dispatches by the widget's contextMenuPolicy automatically:
// CustomContextMenu emits customContextMenuRequested(pos); DefaultContextMenu
// calls the overridden contextMenuEvent(). Sending the event (not calling
// contextMenuEvent() directly) is what makes the CustomContextMenu path fire.
// Like doShowMenu, the trigger is POSTED onto the GUI loop with the owning
// window raised+activated first — the handler usually pops a QMenu that runs its
// own event loop, and showing a native popup from inside the socket-read
// callback re-enters Cocoa and segfaults on a backgrounded macOS instance.
// Inspection + invoke come for free: the popped QMenu is a visible top-level
// menu, which doDumpTree already serializes and invoke already drives by
// text/path.
// Shared scaffolding for the deferred synthetic menu-trigger verbs
// (contextMenu / rightClick): resolve + visibility-check the target, parse an
// optional "<x> <y>" local offset (default: widget center, where a
// position-insensitive handler anchors the menu), then post onto the GUI loop
// with the owning window raised/activated so the native popup has an anchor.
// `verb` names the caller in the error/log text; `send` builds and dispatches
// the concrete event (QContextMenuEvent vs a right-button QMouseEvent) once
// we're back on the event loop. (#4137 review — dedup of the two near-identical
// bodies; behaviour is unchanged for both verbs.)
QJsonObject AutomationServer::postDeferredMenuTrigger(
    const QString& target, const QString& value, const char* verb,
    std::function<void(QWidget*, QPoint, QPoint)> send) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return err(QStringLiteral("widget not found: ") + target);
    }
    if (!w->isVisible()) {
        return err(QStringLiteral("refused: '") + target + QStringLiteral("' is not visible"));
    }
    // Disabled refusal (parity with clickAt / the #4116-review rightClick):
    // a synthetic gesture on a disabled widget is a silent no-op that would
    // otherwise report ok:true.
    if (!w->isEnabled()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: '") + target
                                + QStringLiteral("' is disabled")},
                           {QStringLiteral("disabled"), true},
                           {QStringLiteral("class"), shortClassName(w)}};
    }

    QPoint local = w->rect().center();
    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!parts.isEmpty() && parts.size() != 2) {
        return err(QString::fromLatin1(verb)
                   + QStringLiteral(" requires either no offset or exactly x y"));
    }
    if (parts.size() == 2) {
        bool okx = false, oky = false;
        const int x = parts.at(0).toInt(&okx);
        const int y = parts.at(1).toInt(&oky);
        if (!okx || !oky) {
            return err(QString::fromLatin1(verb)
                       + QStringLiteral(" offset x/y must be integers"));
        }
        local = QPoint(x, y);
        // Bounds refusal (same parity): an out-of-rect point would deliver a
        // press Qt translates onto an ancestor the caller never named.
        if (!w->rect().contains(local)) {
            return err(QString::fromLatin1(verb) + QStringLiteral(": (")
                       + QString::number(x) + QStringLiteral(", ")
                       + QString::number(y) + QStringLiteral(") is outside '")
                       + target + QStringLiteral("' (")
                       + QString::number(w->width()) + QStringLiteral("x")
                       + QString::number(w->height()) + QStringLiteral(")"));
        }
    }

    QPointer<QWidget> wp = w;
    QPointer<QWidget> win = w->window();
    QTimer::singleShot(0, qApp, [wp, win, local, send = std::move(send)]() {
        if (!wp)
            return;
        if (win && win->isVisible()) {   // realize + activate so Cocoa has an anchor
            win->raise();
            win->activateWindow();
        }
        send(wp, local, wp->mapToGlobal(local));
    });
    qCInfo(lcAutomation).noquote() << verb << "on" << target << "at" << local;

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("class"), shortClassName(w)},
        {QStringLiteral("x"), local.x()},
        {QStringLiteral("y"), local.y()},
        {QStringLiteral("deferred"), true},   // popup runs next turn; dumpTree to read it
    };
}

QJsonObject AutomationServer::doContextMenu(const QString& target,
                                            const QString& value) const
{
    // Qt context-menu policy path (contextMenuEvent / customContextMenuRequested).
    return postDeferredMenuTrigger(target, value, "contextMenu",
        [](QWidget* w, QPoint local, QPoint global) {
            QContextMenuEvent ev(QContextMenuEvent::Mouse, local, global);
            QApplication::sendEvent(w, &ev);
        });
}

// ── Real right-button press for mousePressEvent menus (#3646) ────────────────
// Some widgets build context menus directly from mousePressEvent instead of
// Qt's context-menu policy. SpectrumWidget is the important case: its
// panadapter menu is position-sensitive and lives behind a real right-button
// press, so QContextMenuEvent does not reach it. Post a right-button press onto
// the GUI loop and leave the menu's nested event loop to dumpTree/invoke.
QJsonObject AutomationServer::doRightClick(const QString& target,
                                           const QString& value) const
{
    // Real right-button press — for widgets that build their menu directly in
    // mousePressEvent (SpectrumWidget), which a QContextMenuEvent never reaches.
    return postDeferredMenuTrigger(target, value, "rightClick",
        [](QWidget* w, QPoint local, QPoint global) {
            QMouseEvent ev(QEvent::MouseButtonPress,
                           QPointF(local), QPointF(local), QPointF(global),
                           Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            QApplication::sendEvent(w, &ev);
        });
}

QJsonObject AutomationServer::doHitTest(const QString& target,
                                        const QString& value) const
{
    QWidget* w = resolveWidget(target);
    if (!w) {
        return err(QStringLiteral("widget not found: ") + target);
    }
    if (!w->isVisible()) {
        return err(QStringLiteral("refused: '") + target
                   + QStringLiteral("' is not visible"));
    }

    QPoint local = w->rect().center();
    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() == 1) {
        // One coordinate is ambiguous — silently hit-testing rect().center()
        // instead lets a mask/pass-through assertion pass against the wrong
        // point with ok:true. (#3999 review)
        return err(QStringLiteral("hitTest needs both x and y (got one coordinate)"));
    }
    if (parts.size() >= 2) {
        bool okx = false;
        bool oky = false;
        const int x = parts.at(0).toInt(&okx);
        const int y = parts.at(1).toInt(&oky);
        if (!okx || !oky) {
            return err(QStringLiteral("hitTest offset x/y must be integers"));
        }
        local = QPoint(x, y);
    }

    const QPoint global = w->mapToGlobal(local);
    QWidget* child = w->childAt(local);
    QWidget* globalHit = QApplication::widgetAt(global);

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("target"), target},
        {QStringLiteral("targetWidget"), describeHitWidget(w)},
        {QStringLiteral("x"), local.x()},
        {QStringLiteral("y"), local.y()},
        {QStringLiteral("globalX"), global.x()},
        {QStringLiteral("globalY"), global.y()},
        {QStringLiteral("insideTarget"), w->rect().contains(local)},
        {QStringLiteral("childAt"), describeHitWidget(child)},
        {QStringLiteral("widgetAt"), describeHitWidget(globalHit)},
    };
}

// ── clickAt: synthesize a real mouse click at a point (#3461 follow-up) ───────
// Generic fallback for when name/text matching can't reach the widget you want —
// most commonly because several widgets share an accessibleName (e.g. every
// tile's close button is "containerClose") so `invoke` can only ever hit the
// first match. dumpTree reports widget geometry in GLOBAL (screen) coordinates,
// so `clickAt <x> <y>` clicks whatever lives at that global point — pass the
// centre of the target's dumpTree rect and you click exactly that widget. With a
// target, x/y are interpreted LOCAL to that widget instead (like hitTest).
//
// Safety: the click is routed to the deepest child under the point, but the
// TX-keying guard walks the WHOLE ancestor chain from that child to its window.
// Qt re-delivers an unaccepted press to parentWidget() until some ancestor
// accepts it, so guarding only the hit widget would let a click on a passive
// child (a QLabel inside a composite button — see PanLayoutDialog for the live
// pattern) propagate into an unguarded keying parent. Guarding the chain makes
// the check match Qt's delivery semantics — safe by construction, not by the
// accident that today's keying buttons happen to be childless. A coordinate
// click must never be a hole around AETHER_AUTOMATION_ALLOW_TX. (#3646 safety.)
// For the same propagation reason a disabled hit widget is refused outright:
// Qt drops input to disabled widgets, so the click would either silently no-op
// (while we report ok:true) or fall through to an unvetted ancestor.
// Delivery (press+release) is deferred to a clean main-loop turn so any popup
// menu/dialog the click raises runs on a normal stack, mirroring the invoke()
// re-entrancy fix.
QJsonObject AutomationServer::doClickAt(const QString& target,
                                        const QString& value) const
{
    const QStringList parts = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return err(QStringLiteral("clickAt needs both x and y"));
    bool okx = false;
    bool oky = false;
    const int x = parts.at(0).toInt(&okx);
    const int y = parts.at(1).toInt(&oky);
    if (!okx || !oky)
        return err(QStringLiteral("clickAt x/y must be integers"));

    QPoint global;
    QWidget* w = nullptr;
    if (target.isEmpty()) {
        // Global-coordinate form: resolve the widget under the screen point.
        global = QPoint(x, y);
        w = QApplication::widgetAt(global);
        if (!w)
            return err(QStringLiteral("clickAt: no widget at global (")
                       + QString::number(x) + QStringLiteral(", ")
                       + QString::number(y) + QStringLiteral(")"));
    } else {
        // Target-local form: x/y are offsets inside the resolved widget.
        w = resolveWidget(target);
        if (!w)
            return err(QStringLiteral("widget not found: ") + target);
        if (!w->isVisible())
            return err(QStringLiteral("refused: '") + target
                       + QStringLiteral("' is not visible"));
        const QPoint local(x, y);
        // Out-of-bounds local points must not click at all: Qt would translate
        // the press up the parent chain and land it on an ancestor the caller
        // never named (and the TX guard below never saw the reply claim for).
        if (!w->rect().contains(local)) {
            return err(QStringLiteral("clickAt: (") + QString::number(x)
                       + QStringLiteral(", ") + QString::number(y)
                       + QStringLiteral(") is outside '") + target
                       + QStringLiteral("' (") + QString::number(w->width())
                       + QStringLiteral("x") + QString::number(w->height())
                       + QStringLiteral(")"));
        }
        global = w->mapToGlobal(local);
        if (QWidget* child = w->childAt(local))
            w = child;  // route to the deepest child for a faithful click
    }

    // Refuse a disabled hit widget, exactly like invoke(): Qt drops input
    // events to disabled widgets, so the click is a silent no-op that we would
    // otherwise report as ok:true — the control is greyed out for a reason.
    // isEnabled() is effective (false if any ancestor is disabled). (#3646)
    if (!w->isEnabled()) {
        return QJsonObject{{QStringLiteral("ok"), false},
                           {QStringLiteral("error"),
                            QStringLiteral("refused: '") + shortClassName(w)
                                + QStringLiteral("' at the point is disabled — "
                                                 "the click would be dropped")},
                           {QStringLiteral("disabled"), true},
                           {QStringLiteral("class"), shortClassName(w)}};
    }

    // TX-safety guard — a raw coordinate click must honor the same opt-in as
    // invoke(), or it becomes a bypass around the keying gate. Walk the whole
    // ancestor chain (see the function comment): an unaccepted press propagates
    // to parents, so every widget Qt could deliver this click to must pass.
    // (#3646 safety.)
    if (!m_txAllowed) {
        for (const QWidget* p = w; p; p = p->parentWidget()) {
            if (!isTransmitControl(p)) {
                continue;
            }
            qCWarning(lcAutomation).noquote()
                << "BLOCKED transmit-related clickAt on" << shortClassName(w)
                << "(keying control in chain:" << shortClassName(p)
                << ") at global" << global;
            return err(QStringLiteral("blocked: point resolves into '")
                       + shortClassName(p)
                       + QStringLiteral("', a transmit-keying control (TX-safety "
                                        "guard). Enable \"Allow TX via MCP\" in Radio "
                                        "Setup → Network (or set "
                                        "AETHER_AUTOMATION_ALLOW_TX=1) to override."));
        }
    }

    // Power-ceiling rail (#3646): invoke() clamps RF/Tune power setValue to
    // AETHER_AUTOMATION_TX_MAX_POWER, but a groove click on the slider pages
    // the setpoint to an arbitrary value we cannot clamp after the fact. When
    // the rail is armed, refuse the click and point at the clamped path instead
    // of letting a coordinate click walk power past the configured ceiling.
    if (m_txMaxPower >= 0) {
        for (const QWidget* p = w; p; p = p->parentWidget()) {
            const QString an = p->accessibleName();
            if (an == QLatin1String("RF power")
                || an == QLatin1String("Tune power")) {
                qCWarning(lcAutomation).noquote()
                    << "BLOCKED clickAt on power slider" << an
                    << "— power ceiling" << m_txMaxPower
                    << "is armed; use invoke setValue (clamped)";
                return err(QStringLiteral("blocked: '") + an
                           + QStringLiteral("' click would bypass the power "
                                            "ceiling (AETHER_AUTOMATION_TX_MAX_"
                                            "POWER). Use `invoke '") + an
                           + QStringLiteral("' setValue <n>`, which clamps."));
            }
        }
    }

    const QPoint local = w->mapFromGlobal(global);
    QPointer<QWidget> wp = w;
    QPointer<QWidget> win = w->window();
    QTimer::singleShot(0, qApp, [wp, win, local, global]() {
        if (!wp)
            return;
        raiseWindowForPopup(win);  // valid active window for any popup it raises
        const QPointF lf(local);
        const QPointF gf(global);
        QMouseEvent press(QEvent::MouseButtonPress, lf, lf, gf,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(wp, &press);
        if (!wp)
            return;
        QMouseEvent release(QEvent::MouseButtonRelease, lf, lf, gf,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(wp, &release);
    });

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("clicked"), describeHitWidget(w)},
        {QStringLiteral("globalX"), global.x()},
        {QStringLiteral("globalY"), global.y()},
        {QStringLiteral("localX"), local.x()},
        {QStringLiteral("localY"), local.y()},
        {QStringLiteral("deferred"), true},   // press/release run next main-loop turn
    };
}

// ── Panadapter lifecycle (#3646) ────────────────────────────────────────────
// `pan create|add` opens an independent panadapter; `pan center <mhz>` recenters
// the active pan (the band-change lever — a plain `tune` only moves the slice and
// clamps to the pan's RF range, #292); `pan close|remove <panId|index|active|all>`
// tears one down regardless of how it was opened. Close routes through the
// production RadioModel::removePanadapter, which sends the FlexLib-correct pair
// `display pan remove` AND `display panafall remove` (panId + waterfallId), so a
// panafall-created pan closes — waterfall and all — without the slice-removal
// workaround (#3843). Create is async (radio assigns the pan_id), so a caller
// re-reads `get pans`.
QJsonObject AutomationServer::doPan(const QString& action, const QString& arg)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));
    RadioModel* radio = m_radioModel;

    if (action == QLatin1String("create") || action == QLatin1String("add")) {
        const int have = radio->panadapters().size();
        const int limit = radio->maxPanadapters();
        if (have >= limit)
            return err(QStringLiteral("refused: at panadapter limit (%1)").arg(limit));
        radio->createPanadapter();
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("pan"), QStringLiteral("create")},
                           {QStringLiteral("requested"), true}, {QStringLiteral("priorCount"), have}};
    }

    if (action == QLatin1String("center")) {
        bool okF = false; const double mhz = arg.toDouble(&okF);
        if (!okF || mhz <= 0)
            return err(QStringLiteral("pan center requires a positive frequency in MHz"));
        radio->setPanCenter(mhz);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("pan"), QStringLiteral("center")},
                           {QStringLiteral("centerMhz"), mhz}, {QStringLiteral("requested"), true}};
    }

    if (action == QLatin1String("close") || action == QLatin1String("remove")) {
        const QString a = arg.trimmed();
        if (a.isEmpty())
            return err(QStringLiteral("pan close requires <panId|index|active|all>"));

        const bool all = (a.compare(QLatin1String("all"), Qt::CaseInsensitive) == 0);
        QStringList panIds;
        if (all) {
            for (const PanadapterModel* p : radio->panadapters())
                panIds << p->panId();
        } else if (a.compare(QLatin1String("active"), Qt::CaseInsensitive) == 0) {
            if (const PanadapterModel* p = radio->activePanadapter())
                panIds << p->panId();
        } else if (a.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
            panIds << a;   // explicit radio stream id
        } else {
            bool okIdx = false;
            const int idx = a.toInt(&okIdx);
            if (!okIdx)
                return err(QStringLiteral("pan close: '") + a
                           + QStringLiteral("' is not a panId (0x…), index, 'active', or 'all'"));
            const QString pid = panIdForIndex(idx);
            if (pid.isEmpty())
                return err(QStringLiteral("no pan with index ") + a);
            panIds << pid;
        }
        if (panIds.isEmpty())
            return err(QStringLiteral("no matching panadapter to close"));
        // Match the GUI guard for a single targeted close; `all` is an explicit
        // teardown so it is allowed to remove the last pan.
        if (!all && radio->panadapters().size() <= 1)
            return err(QStringLiteral("refused: cannot close the last panadapter"));

        QJsonArray closed;
        for (const QString& pid : panIds) {
            const PanadapterModel* p = radio->panadapter(pid);
            const QString wfId = p ? p->waterfallId() : QString();
            // Single source of truth: drive the production teardown so this verb
            // exercises the exact GUI close path. removePanadapter sends the
            // FlexLib-correct pair "display pan remove" + "display panafall
            // remove" for a panafall, so the waterfall is freed too. (#3843)
            radio->removePanadapter(pid);
            QJsonObject o{{QStringLiteral("panId"), pid},
                          {QStringLiteral("resolved"), p != nullptr}};
            if (!wfId.isEmpty())
                o[QStringLiteral("waterfallId")] = wfId;
            closed.append(o);
            qCInfo(lcAutomation).noquote() << "pan close" << pid
                                           << (wfId.isEmpty() ? QString() : QStringLiteral("+ wf ") + wfId);
        }
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("pan"), QStringLiteral("close")},
            {QStringLiteral("requested"), true},   // radio echoes "… removed"; re-poll get pans
            {QStringLiteral("closed"), closed},
        };
    }

    return err(QStringLiteral("unknown pan action: ") + action
               + QStringLiteral(" (create|add|remove|close|center)"));
}

// ── Panadapter layout (bridge test hook) ────────────────────────────────────
// `layout rearrange <id>` drives PanadapterStack::rearrangeLayout directly, so
// the splitter reparent / GPU-surface path is exercisable regardless of how
// many panadapters the radio has actually granted (MultiFlex capacity caps
// live multi-pan on shared radios, which otherwise makes the add-2nd-pan
// crash path — #4091 — unreachable from the bridge). `layout get` reports the
// saved layout id and current pan counts without changing anything. This does
// NOT persist PanadapterLayout — it is a transient exerciser, not the
// production layout-change path.
QJsonObject AutomationServer::doLayout(const QString& action, const QString& arg)
{
    const QList<QWidget*> stacks =
        findWidgetsByClass(QStringLiteral("PanadapterStack"));
    if (stacks.isEmpty()) {
        return err(QStringLiteral("no PanadapterStack found (connect a radio first)"));
    }

    QString layoutId;   // empty → query-only (the "get" action)
    if (action == QLatin1String("rearrange")) {
        layoutId = arg.trimmed();
        if (layoutId.isEmpty()) {
            return err(QStringLiteral("layout rearrange requires a layout id "
                                      "(1|2v|2h|2h1|12h|3v|2x2|4v|3h2|2x3|4h3|2x4)"));
        }
    } else if (action != QLatin1String("get")) {
        return err(QStringLiteral("unknown layout action: ") + action
                   + QStringLiteral(" (rearrange|get)"));
    }

    QVariantMap snap;
    if (!QMetaObject::invokeMethod(stacks.first(), "automationRearrange",
                                   Qt::DirectConnection,
                                   Q_RETURN_ARG(QVariantMap, snap),
                                   Q_ARG(QString, layoutId))) {
        return err(QStringLiteral("PanadapterStack::automationRearrange failed"));
    }
    // An unknown layout id comes back as an error map — pass it through
    // instead of stamping ok:true over it (#4091 test honesty).
    if (snap.contains(QStringLiteral("error"))) {
        return err(snap.value(QStringLiteral("error")).toString());
    }

    QJsonObject out = QJsonObject::fromVariantMap(snap);
    out[QStringLiteral("ok")] = true;
    out[QStringLiteral("layout")] = action;
    return out;
}

// ── UI scale (report / persist for next launch) ─────────────────────────────
// `scale` reports the effective UI scale so automation can assert the process
// launched at the intended fractional QT_SCALE_FACTOR (pairs with `get rhi`
// to prove the #4091 even-alignment fix). `scale <pct>` persists UiScalePercent
// so a subsequent relaunch reproduces that configuration — QT_SCALE_FACTOR must
// be set before QApplication (main.cpp), so it can only apply on next launch;
// this never mutates the running process's scale. Values match the View → UI
// Scale menu steps.
QJsonObject AutomationServer::doScale(const QString& arg)
{
    AppSettings& s = AppSettings::instance();
    QJsonObject out{{QStringLiteral("ok"), true}, {QStringLiteral("scale"), true}};

    const QByteArray env = qgetenv("QT_SCALE_FACTOR");
    out[QStringLiteral("qtScaleFactorEnv")] =
        env.isEmpty() ? QJsonValue() : QJsonValue(QString::fromUtf8(env));
    out[QStringLiteral("uiScalePercentSaved")] =
        s.value(QStringLiteral("UiScalePercent"), QStringLiteral("100")).toInt();
    if (QScreen* scr = QApplication::primaryScreen()) {
        out[QStringLiteral("primaryScreenDpr")] = scr->devicePixelRatio();
    }

    const QString a = arg.trimmed();
    if (!a.isEmpty()) {
        // Canonical steps duplicate MainWindow.cpp's TU-static kScaleSteps /
        // the View → UI Scale menu (the bridge must not include GUI headers —
        // Engine/UI dependency direction). If the menu grows a step, add it
        // here too; MainWindow.cpp carries the reciprocal note.
        static const QList<int> kScaleSteps = {75, 85, 100, 110, 125, 150, 175, 200};
        bool okI = false;
        const int pct = a.toInt(&okI);
        if (!okI || !kScaleSteps.contains(pct)) {
            return err(QStringLiteral("scale pct must be one of "
                                      "75|85|100|110|125|150|175|200"));
        }
        s.setValue(QStringLiteral("UiScalePercent"), QString::number(pct));
        s.save();
        out[QStringLiteral("uiScalePercentSet")] = pct;
        out[QStringLiteral("appliesOnNextLaunch")] = true;
    }
    return out;
}

QJsonObject AutomationServer::doPanMessage(const QString& action,
                                           const QString& target,
                                           const QString& id,
                                           const QString& title,
                                           const QString& detail,
                                           int timeoutMs,
                                           const QString& tone) const
{
    auto resolveSpectrum = [this, &target]() -> QWidget* {
        const QString trimmed = target.trimmed();
        if (trimmed.isEmpty() || trimmed == QLatin1String("active")) {
            QString activePanId;
            if (m_radioModel && m_radioModel->activePanadapter()) {
                activePanId = m_radioModel->activePanadapter()->panId();
            }
            const QList<QWidget*> spectra =
                findWidgetsByClass(QStringLiteral("SpectrumWidget"));
            if (!activePanId.isEmpty()) {
                for (QWidget* sw : spectra) {
                    for (QWidget* a = sw; a; a = a->parentWidget()) {
                        if (shortClassName(a) == QLatin1String("PanadapterApplet")
                            && a->property("panId").toString() == activePanId) {
                            return sw;
                        }
                    }
                }
            }
            // Active pan unresolved (startup / mid-reconnect / null
            // activePanadapter). Only fall back when there is exactly one
            // panadapter — otherwise silently injecting into pan 0 would target
            // the wrong surface with ok:true. With multiple pans, let the
            // caller surface "no panadapter spectrum for target" (mirrors how
            // `grab pan` errors via panSpectrumWidgetForIndex). (#3999 review)
            if (spectra.size() == 1) {
                return spectra.first();
            }
            return nullptr;
        }

        bool okIndex = false;
        const int index = trimmed.toInt(&okIndex);
        if (okIndex) {
            return panSpectrumWidgetForIndex(index);
        }

        const QList<QWidget*> spectra =
            findWidgetsByClass(QStringLiteral("SpectrumWidget"));
        for (QWidget* sw : spectra) {
            if (sw->objectName() == trimmed) {
                return sw;
            }
            for (QWidget* a = sw; a; a = a->parentWidget()) {
                if (shortClassName(a) == QLatin1String("PanadapterApplet")
                    && a->property("panId").toString() == trimmed) {
                    return sw;
                }
            }
        }
        return nullptr;
    };

    QWidget* spectrum = resolveSpectrum();
    if (!spectrum) {
        return err(QStringLiteral("no panadapter spectrum for target '")
                   + target + QStringLiteral("'"));
    }

    auto snapshot = [spectrum]() {
        QVariantList messages;
        QMetaObject::invokeMethod(spectrum, "overlayMessageSnapshot",
                                  Qt::DirectConnection,
                                  Q_RETURN_ARG(QVariantList, messages));
        QJsonArray arr;
        for (const QVariant& v : messages) {
            arr.append(QJsonObject::fromVariantMap(v.toMap()));
        }
        return arr;
    };

    const QString lower = action.trimmed().toLower();
    if (lower == QLatin1String("list") || lower == QLatin1String("snapshot")) {
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("panmessage"), QStringLiteral("list")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("messages"), snapshot()}};
    }

    if (lower == QLatin1String("clear")) {
        QMetaObject::invokeMethod(spectrum, "automationClearOverlayMessages",
                                  Qt::DirectConnection);
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("panmessage"), QStringLiteral("clear")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("messages"), snapshot()}};
    }

    if (lower == QLatin1String("remove") || lower == QLatin1String("dismiss")) {
        if (id.trimmed().isEmpty()) {
            return err(QStringLiteral("panmessage remove requires an id"));
        }
        bool removed = false;
        QMetaObject::invokeMethod(spectrum, "automationRemoveOverlayMessage",
                                  Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, removed),
                                  Q_ARG(QString, id));
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("panmessage"), QStringLiteral("remove")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("removed"), removed},
                           {QStringLiteral("messages"), snapshot()}};
    }

    if (lower == QLatin1String("add") || lower == QLatin1String("upsert")) {
        if (id.trimmed().isEmpty()) {
            return err(QStringLiteral("panmessage add requires an id"));
        }
        if (title.trimmed().isEmpty() && detail.trimmed().isEmpty()) {
            return err(QStringLiteral("panmessage add requires title or detail"));
        }
        const QString toneLower = tone.trimmed().toLower();
        if (!toneLower.isEmpty()
            && toneLower != QLatin1String("info")
            && toneLower != QLatin1String("warning")) {
            // Reject unknown tones instead of silently mapping them to Info
            // while echoing the bogus value back as honored — a `tone=danger`
            // test would otherwise pass while exercising Info styling. (#3999 review)
            return err(QStringLiteral("panmessage tone must be 'info' or 'warning' (got '")
                       + tone.trimmed() + QStringLiteral("')"));
        }
        bool accepted = false;
        QMetaObject::invokeMethod(spectrum, "automationUpsertOverlayMessage",
                                  Qt::DirectConnection,
                                  Q_RETURN_ARG(bool, accepted),
                                  Q_ARG(QString, id),
                                  Q_ARG(QString, title),
                                  Q_ARG(QString, detail),
                                  Q_ARG(int, timeoutMs),
                                  Q_ARG(QString, tone));
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("panmessage"), QStringLiteral("add")},
                           {QStringLiteral("target"), target},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("timeoutMs"), timeoutMs},
                           {QStringLiteral("tone"), tone.trimmed().isEmpty()
                                ? QStringLiteral("info")
                                : tone.trimmed().toLower()},
                           {QStringLiteral("accepted"), accepted},
                           {QStringLiteral("messages"), snapshot()}};
    }

    return err(QStringLiteral("unknown panmessage action: ") + action
               + QStringLiteral(" (add|remove|clear|list)"));
}

QJsonObject AutomationServer::doDss(const QString& action,
                                    const QString& target,
                                    const QString& value) const
{
    const QString lower = action.trimmed().toLower();
    QStringList args = value.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QString panTarget = target;

    const auto isStreamToken = [](const QString& text) {
        const QString s = text.trimmed().toLower();
        return s == QLatin1String("native")
            || s == QLatin1String("flex")
            || s == QLatin1String("kiwi")
            || s == QLatin1String("kiwisdr");
    };

    bool targetIsInt = false;
    if (!target.isEmpty()) {
        (void)target.toInt(&targetIsInt);
    }
    if (targetIsInt && lower == QLatin1String("inject")
        && (args.size() == 2
            || (args.size() == 3 && isStreamToken(args.value(2)))
            || (args.size() == 5 && isStreamToken(args.value(2))))) {
        args.prepend(target);
        panTarget.clear();
    } else if (targetIsInt
               && (lower == QLatin1String("scrollback")
                   || lower == QLatin1String("pause"))
               && args.isEmpty()) {
        args.prepend(target);
        panTarget.clear();
    }

    bool okIndex = false;
    int panIndex = panTarget.isEmpty() ? 0 : panTarget.toInt(&okIndex);
    if (!panTarget.isEmpty() && !okIndex) {
        args.prepend(panTarget);
        panIndex = 0;
    }

    QJsonArray available;
    QWidget* spectrum = panSpectrumWidgetForIndex(panIndex, &available);
    if (!spectrum) {
        return QJsonObject{
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"),
             QStringLiteral("no pan with index ") + QString::number(panIndex)},
            {QStringLiteral("available"), available},
        };
    }

    const auto parseStream = [](const QString& text, bool* ok) {
        const QString s = text.trimmed().toLower();
        if (s.isEmpty() || s == QLatin1String("native")
            || s == QLatin1String("flex")) {
            *ok = true;
            return false;
        }
        if (s == QLatin1String("kiwi") || s == QLatin1String("kiwisdr")) {
            *ok = true;
            return true;
        }
        *ok = false;
        return false;
    };

    QVariantMap out;
    if (lower == QLatin1String("snapshot") || lower == QLatin1String("status")) {
        if (!QMetaObject::invokeMethod(spectrum, "automationDssSnapshot",
                                       Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariantMap, out))) {
            return err(QStringLiteral("target pan does not expose automationDssSnapshot"));
        }
    } else if (lower == QLatin1String("reset") || lower == QLatin1String("clear")) {
        bool okStream = false;
        const bool kiwiStream = parseStream(args.value(0), &okStream);
        if (!okStream) {
            return err(QStringLiteral("dss reset stream must be native|kiwi"));
        }
        if (!QMetaObject::invokeMethod(spectrum, "automationDssReset",
                                       Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariantMap, out),
                                       Q_ARG(bool, kiwiStream))) {
            return err(QStringLiteral("target pan does not expose automationDssReset"));
        }
    } else if (lower == QLatin1String("inject")) {
        if (args.size() < 3) {
            return err(QStringLiteral(
                "dss inject requires [pan] <count> <firstPeakBin> <stepBin> "
                "[native|kiwi [rowLowMhz rowHighMhz]]"));
        }
        if (args.size() == 5 || args.size() > 6) {
            return err(QStringLiteral(
                "dss inject frame override requires stream plus rowLowMhz rowHighMhz"));
        }
        bool okCount = false;
        bool okPeak = false;
        bool okStep = false;
        const int count = args.value(0).toInt(&okCount);
        const int firstPeakBin = args.value(1).toInt(&okPeak);
        const int stepBin = args.value(2).toInt(&okStep);
        if (!okCount || !okPeak || !okStep) {
            return err(QStringLiteral("dss inject count/firstPeakBin/stepBin must be integers"));
        }
        bool okStream = false;
        const bool kiwiStream = parseStream(args.value(3), &okStream);
        if (!okStream) {
            return err(QStringLiteral("dss inject stream must be native|kiwi"));
        }
        double rowLowMhz = -1.0;
        double rowHighMhz = -1.0;
        if (args.size() == 6) {
            if (!kiwiStream) {
                return err(QStringLiteral("dss inject frame override is only valid for kiwi"));
            }
            bool okLow = false;
            bool okHigh = false;
            rowLowMhz = args.value(4).toDouble(&okLow);
            rowHighMhz = args.value(5).toDouble(&okHigh);
            if (!okLow || !okHigh || rowHighMhz <= rowLowMhz) {
                return err(QStringLiteral(
                    "dss inject rowLowMhz/rowHighMhz must be ascending numbers"));
            }
        }
        if (!QMetaObject::invokeMethod(spectrum, "automationDssInjectRows",
                                       Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariantMap, out),
                                       Q_ARG(int, count),
                                       Q_ARG(int, firstPeakBin),
                                       Q_ARG(int, stepBin),
                                       Q_ARG(bool, kiwiStream),
                                       Q_ARG(double, rowLowMhz),
                                       Q_ARG(double, rowHighMhz))) {
            return err(QStringLiteral("target pan does not expose automationDssInjectRows"));
        }
    } else if (lower == QLatin1String("scrollback")
               || lower == QLatin1String("pause")) {
        bool okOffset = false;
        const int offsetRows = args.value(0).toInt(&okOffset);
        if (!okOffset) {
            return err(QStringLiteral("dss scrollback requires an offset row count"));
        }
        if (!QMetaObject::invokeMethod(spectrum, "automationDssSetScrollback",
                                       Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariantMap, out),
                                       Q_ARG(bool, false),
                                       Q_ARG(int, offsetRows))) {
            return err(QStringLiteral("target pan does not expose automationDssSetScrollback"));
        }
    } else if (lower == QLatin1String("live")) {
        if (!QMetaObject::invokeMethod(spectrum, "automationDssSetScrollback",
                                       Qt::DirectConnection,
                                       Q_RETURN_ARG(QVariantMap, out),
                                       Q_ARG(bool, true),
                                       Q_ARG(int, 0))) {
            return err(QStringLiteral("target pan does not expose automationDssSetScrollback"));
        }
    } else {
        return err(QStringLiteral("unknown dss action: ") + action);
    }

    QJsonObject response = QJsonObject::fromVariantMap(out);
    response[QStringLiteral("cmd")] = QStringLiteral("dss");
    response[QStringLiteral("action")] = action;
    response[QStringLiteral("panIndex")] = panIndex;
    return response;
}

// ── Radio-side display-stream inventory / leak detector (#3856) ──────────────
// `get pans` can never show a radio-side leak: the client tears down its own
// view on the "removed" echo, so it always looks clean. This verb reports two
// independent radio-authoritative views:
//   Layer A (`streams`)      — VITA-49 UDP truth: streams the radio is STILL
//                              transmitting for an id we no longer own (catches
//                              continued-UDP leaks, the #268 class).
//   Layer B (`streams radio`)— status-bookkeeping truth: the radio's full
//                              display-object set classified ours/foreign/orphan,
//                              with leaked waterfalls (parent pan gone) — catches
//                              resource-level lingering that emits no UDP (#3843).
// `streams reset` clears the Layer-A orphan tally to re-baseline a before/after.
// `tci start [port|sdc [port]] | status | stop [abrupt]` — in-process TCI
// client simulator (#3305/#4009/#3913). The default WSJT-X profile negotiates
// RX audio; the SDC profile negotiates 96 kHz IQ and sends `iq_start:0` for a
// CW-skimmer-shaped end-to-end test. `stop abrupt` closes without the matching
// stream-stop command so tests can assert disconnect cleanup.
QJsonObject AutomationServer::doTci(const QString& action, const QString& value)
{
#ifndef HAVE_WEBSOCKETS
    Q_UNUSED(action);
    Q_UNUSED(value);
    return err(QStringLiteral("TCI is not built into this binary (HAVE_WEBSOCKETS off)"));
#else
    const auto status = [this]() {
        QJsonObject o{{QStringLiteral("ok"), true},
                      {QStringLiteral("running"), m_tciSim != nullptr}};
        if (m_tciSim) {
            o[QStringLiteral("connected")]    = m_tciSim->state() == QAbstractSocket::ConnectedState;
            o[QStringLiteral("ready")]        = m_tciSimReady;
            o[QStringLiteral("audioStarted")] = m_tciSimAudioStarted;
            o[QStringLiteral("iqStarted")]    = m_tciSimIqStarted;
            o[QStringLiteral("profile")]      = m_tciSimProfile;
        }
        o[QStringLiteral("binaryFrames")] = m_tciSimBinaryFrames;
        o[QStringLiteral("iqFrames")]     = m_tciSimIqFrames;
        o[QStringLiteral("binaryBytes")]  = m_tciSimBinaryBytes;
        o[QStringLiteral("textMessages")] = m_tciSimTextMsgs;
        o[QStringLiteral("msSinceLastFrame")] =
            (m_tciSimLastFrameMs >= 0 && m_tciSimTimer.isValid())
                ? m_tciSimTimer.elapsed() - m_tciSimLastFrameMs : -1;
        if (!m_tciSimCloseReason.isEmpty())
            o[QStringLiteral("closeReason")] = m_tciSimCloseReason;
        return o;
    };

    if (action == QLatin1String("status"))
        return status();

    if (action == QLatin1String("start")) {
        if (m_tciSim)
            return err(QStringLiteral("tci sim already running — `tci stop` first"));
        const QStringList options = value.simplified().split(
            QLatin1Char(' '), Qt::SkipEmptyParts);
        const bool sdcProfile = !options.isEmpty()
            && options.first().compare(QLatin1String("sdc"), Qt::CaseInsensitive) == 0;
        const QString portText = sdcProfile
            ? (options.size() >= 2 ? options.at(1) : QString())
            : (options.isEmpty() ? QString() : options.first());
        bool okPort = false;
        int port = portText.toInt(&okPort);
        if (!okPort || port <= 0)
            port = AppSettings::instance().value("TciPort", "50001").toInt();
        m_tciSimProfile = sdcProfile ? QStringLiteral("sdc") : QStringLiteral("wsjtx");
        m_tciSimReady = false;
        m_tciSimAudioStarted = false;
        m_tciSimIqStarted = false;
        m_tciSimBinaryFrames = 0;
        m_tciSimIqFrames = 0;
        m_tciSimBinaryBytes = 0;
        m_tciSimTextMsgs = 0;
        m_tciSimLastFrameMs = -1;
        m_tciSimCloseReason.clear();
        m_tciSimTimer.start();
        m_tciSim = new QWebSocket(QStringLiteral("aether-automation-tci-sim"),
                                  QWebSocketProtocol::VersionLatest, this);
        connect(m_tciSim, &QWebSocket::textMessageReceived,
                this, [this](const QString& msg) {
            ++m_tciSimTextMsgs;
            if (m_tciSimReady) return;
            const QStringList cmds = msg.split(QLatin1Char(';'));
            for (const QString& c : cmds) {
                if (c.trimmed() == QLatin1String("ready")) {
                    m_tciSimReady = true;
                    if (m_tciSimProfile == QLatin1String("sdc")) {
                        m_tciSim->sendTextMessage(QStringLiteral("iq_samplerate:96000;"));
                        m_tciSim->sendTextMessage(QStringLiteral("audio_samplerate:24000;"));
                        m_tciSim->sendTextMessage(QStringLiteral("iq_start:0;"));
                        m_tciSimIqStarted = true;
                        qCInfo(lcAutomation)
                            << "tci sim: ready received — SDC IQ negotiation sent";
                    } else {
                        m_tciSim->sendTextMessage(QStringLiteral("audio_samplerate:48000;"));
                        m_tciSim->sendTextMessage(QStringLiteral("audio_start:0;"));
                        m_tciSimAudioStarted = true;
                        qCInfo(lcAutomation)
                            << "tci sim: ready received — WSJT-X audio_start sent";
                    }
                    break;
                }
            }
        });
        connect(m_tciSim, &QWebSocket::binaryMessageReceived,
                this, [this](const QByteArray& b) {
            ++m_tciSimBinaryFrames;
            m_tciSimBinaryBytes += b.size();
            m_tciSimLastFrameMs = m_tciSimTimer.elapsed();
            constexpr int kTciTypeOffset = 6 * static_cast<int>(sizeof(quint32));
            if (b.size() >= kTciTypeOffset + static_cast<int>(sizeof(quint32))) {
                quint32 type = 0;
                std::memcpy(&type, b.constData() + kTciTypeOffset, sizeof(type));
                if (type == 0) {
                    ++m_tciSimIqFrames;
                }
            }
        });
        connect(m_tciSim, &QWebSocket::disconnected, this, [this]() {
            if (m_tciSimCloseReason.isEmpty())
                m_tciSimCloseReason = QStringLiteral("server closed");
            // Tear down so `tci status` reports running=false and a later
            // `tci start` isn't rejected as "already running" after a
            // server/radio-side close (PR #4017 review item 5). The stop path
            // nulls m_tciSim before its own close lands — only reap here when
            // the disconnect came from the socket we still track.
            if (auto* sock = qobject_cast<QWebSocket*>(sender());
                    sock && sock == m_tciSim) {
                m_tciSim->deleteLater();
                m_tciSim = nullptr;
                m_tciSimReady = false;
                m_tciSimAudioStarted = false;
                m_tciSimIqStarted = false;
                qCInfo(lcAutomation) << "tci sim: torn down after server-side close"
                                     << m_tciSimCloseReason;
            }
        });
        m_tciSim->open(QUrl(QStringLiteral("ws://127.0.0.1:%1").arg(port)));
        qCInfo(lcAutomation) << "tci sim: connecting to ws://127.0.0.1:" << port;
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("action"), QStringLiteral("start")},
                           {QStringLiteral("profile"), m_tciSimProfile},
                           {QStringLiteral("port"), port}};
    }

    if (action == QLatin1String("stop")) {
        if (!m_tciSim)
            return err(QStringLiteral("tci sim is not running"));
        const bool abrupt = value.trimmed().compare(QLatin1String("abrupt"),
                                                    Qt::CaseInsensitive) == 0;
        QJsonObject o = status();
        m_tciSimCloseReason = abrupt ? QStringLiteral("client abort (abrupt)")
                                     : QStringLiteral("client stop");
        // Null m_tciSim BEFORE abort()/close(). abort() emits QWebSocket::
        // disconnected synchronously (same-thread direct delivery), re-entering
        // the disconnected lambda; if m_tciSim were still set there it would
        // deleteLater()+null it, and the deleteLater() below would then fire on
        // a dangling pointer. Nulling first makes the lambda's sock==m_tciSim
        // guard fail so it no-ops, and we own the teardown here (#4017).
        QWebSocket* sim = m_tciSim;
        const bool wasAudioStarted = m_tciSimAudioStarted;
        const bool wasIqStarted = m_tciSimIqStarted;
        m_tciSim = nullptr;
        m_tciSimReady = false;
        m_tciSimAudioStarted = false;
        m_tciSimIqStarted = false;
        if (abrupt) {
            sim->abort();
        } else {
            if (wasAudioStarted)
                sim->sendTextMessage(QStringLiteral("audio_stop:0;"));
            if (wasIqStarted)
                sim->sendTextMessage(QStringLiteral("iq_stop:0;"));
            sim->close();
        }
        sim->deleteLater();
        o[QStringLiteral("action")] = QStringLiteral("stop");
        o[QStringLiteral("abrupt")] = abrupt;
        qCInfo(lcAutomation) << "tci sim: stopped" << (abrupt ? "(abrupt)" : "(graceful)");
        return o;
    }

    return err(QStringLiteral(
        "tci requires an action (start [port|sdc [port]] | status | stop [abrupt])"));
#endif
}

QJsonObject AutomationServer::doStreams(const QString& action)
{
    if (!m_radioModel)
        return err(QStringLiteral("no radio model available"));

    const auto hex = [](quint32 id) {
        return QStringLiteral("0x") + QString::number(id, 16);
    };
    const auto ownStr = [](DisplayInventory::Ownership o) {
        switch (o) {
        case DisplayInventory::Ownership::Ours:    return QStringLiteral("ours");
        case DisplayInventory::Ownership::Foreign: return QStringLiteral("foreign");
        default:                                   return QStringLiteral("orphan");
        }
    };

    // Layer B — radio-authoritative display-object inventory.
    if (action.compare(QLatin1String("radio"), Qt::CaseInsensitive) == 0
        || action.compare(QLatin1String("inventory"), Qt::CaseInsensitive) == 0) {
        const DisplayInventory::Report rep = m_radioModel->displayInventoryReport();
        QJsonArray pans;
        for (const auto& p : rep.pans)
            pans.append(QJsonObject{{QStringLiteral("panId"), p.id},
                                    {QStringLiteral("clientHandle"), hex(p.clientHandle)},
                                    {QStringLiteral("ownership"), ownStr(p.ownership)}});
        QJsonArray wfs;
        for (const auto& w : rep.waterfalls) {
            QJsonObject o{{QStringLiteral("waterfallId"), w.id},
                          {QStringLiteral("clientHandle"), hex(w.clientHandle)},
                          {QStringLiteral("ownership"), ownStr(w.ownership)},
                          {QStringLiteral("parentMissing"), w.parentMissing}};
            if (!w.parentPanId.isEmpty())
                o[QStringLiteral("parentPanId")] = w.parentPanId;
            wfs.append(o);
        }
        QJsonArray leaked;
        for (const auto& id : rep.leakedWaterfalls) leaked.append(id);
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("scope"), QStringLiteral("radio")},
            {QStringLiteral("pans"), pans},
            {QStringLiteral("waterfalls"), wfs},
            {QStringLiteral("radioPanCount"), rep.pans.size()},
            {QStringLiteral("radioWaterfallCount"), rep.waterfalls.size()},
            {QStringLiteral("orphanPanCount"), rep.orphanPanCount},
            {QStringLiteral("orphanWaterfallCount"), rep.orphanWfCount},
            {QStringLiteral("foreignPanCount"), rep.foreignPanCount},
            {QStringLiteral("foreignWaterfallCount"), rep.foreignWfCount},
            {QStringLiteral("leakedWaterfalls"), leaked},
            {QStringLiteral("leakCount"), leaked.size()},
        };
    }

    // `streams resync` — force the radio to re-dump its authoritative display
    // set, then re-poll `streams radio` after a moment. Closes the gap where a
    // waterfall lingers as a radio resource but no longer emits UDP (Layer A
    // can't see it) and the client already purged its view (Layer B looked
    // clean). The re-dump is async, so this just triggers and the driver reads
    // the refreshed inventory on the next `streams radio`.
    if (action.compare(QLatin1String("resync"), Qt::CaseInsensitive) == 0
        || action.compare(QLatin1String("refresh"), Qt::CaseInsensitive) == 0) {
        const bool sent = m_radioModel->resyncDisplayInventory();
        if (!sent)
            return err(QStringLiteral("not connected — cannot resync display inventory"));
        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("scope"), QStringLiteral("radio")},
            {QStringLiteral("resync"), QStringLiteral("requested")},
            {QStringLiteral("hint"),
             QStringLiteral("re-poll 'streams radio' after ~500ms for the refreshed set")},
        };
    }

    // Layer A — VITA-49 UDP-orphan detector (needs the stream receiver).
    PanadapterStream* ps = m_radioModel->panStream();
    if (!ps)
        return err(QStringLiteral("no panadapter stream available"));

    if (action.compare(QLatin1String("reset"), Qt::CaseInsensitive) == 0) {
        ps->resetOrphanStreams();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("streams"), QStringLiteral("reset")}};
    }
    if (!action.isEmpty())
        return err(QStringLiteral("unknown streams action: ") + action
                   + QStringLiteral(" (use '' for UDP-orphan, 'radio' for inventory,"
                                    " 'resync' to force a re-dump, or 'reset')"));

    QJsonArray panReg;
    for (quint32 id : ps->registeredPanStreams()) panReg.append(hex(id));
    QJsonArray wfReg;
    for (quint32 id : ps->registeredWfStreams()) wfReg.append(hex(id));

    QJsonArray orphans;
    for (const auto& o : ps->orphanStreams())
        orphans.append(QJsonObject{
            {QStringLiteral("streamId"), hex(o.streamId)},
            {QStringLiteral("kind"), o.waterfall ? QStringLiteral("waterfall")
                                                 : QStringLiteral("panadapter")},
            {QStringLiteral("packets"), static_cast<qint64>(o.packets)},
            {QStringLiteral("age_ms"), o.ageMs},
        });

    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("scope"), QStringLiteral("udp")},
        {QStringLiteral("registeredPanStreams"), panReg},
        {QStringLiteral("registeredWfStreams"), wfReg},
        {QStringLiteral("orphanStreams"), orphans},
        {QStringLiteral("orphanCount"), orphans.size()},
    };
}

QJsonObject AutomationServer::doAudioCapture(const QString& action,
                                             const QString& arg,
                                             const QString& path) const
{
    if (!m_audioEngine) {
        return err(QStringLiteral("no audio engine available"));
    }

    auto compactSnapshot = [this]() {
        QJsonObject snapshot =
            m_audioEngine->automationAudioCaptureSnapshot(false);
        snapshot[QStringLiteral("chunksOmitted")] =
            snapshot.value(QStringLiteral("chunkCount"));
        snapshot.remove(QStringLiteral("chunks"));
        return snapshot;
    };

    const QString normalizedAction = action.trimmed().toLower();
    if (normalizedAction == QLatin1String("start")) {
        const QStringList parts =
            arg.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        int durationMs = 5000;
        int pointStart = 0;
        if (!parts.isEmpty()) {
            bool ok = false;
            const int parsed = parts.constFirst().toInt(&ok);
            if (ok) {
                durationMs = parsed;
                pointStart = 1;
            }
        }

        QStringList points;
        for (int i = pointStart; i < parts.size(); ++i) {
            const QStringList split =
                parts.at(i).split(QLatin1Char(','),
                                  Qt::SkipEmptyParts);
            for (const QString& point : split) {
                points.append(point);
            }
        }
        return m_audioEngine->startAutomationAudioCapture(durationMs, points);
    }

    if (normalizedAction == QLatin1String("stop")) {
        m_audioEngine->stopAutomationAudioCapture();
        return compactSnapshot();
    }

    if (normalizedAction == QLatin1String("status")) {
        return compactSnapshot();
    }

    if (normalizedAction == QLatin1String("read")) {
        const QString outPath =
            !path.trimmed().isEmpty() ? path.trimmed() : arg.trimmed();
        if (outPath.isEmpty()) {
            return compactSnapshot();
        }

        const QJsonObject capture =
            m_audioEngine->automationAudioCaptureSnapshot(true);
        QFile out(outPath);
        const QFileInfo info(outPath);
        if (!info.absoluteDir().exists()
            && !QDir().mkpath(info.absolutePath())) {
            return err(QStringLiteral("failed to create audio capture directory: ")
                       + info.absolutePath());
        }
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return err(QStringLiteral("failed to write audio capture: ")
                       + out.errorString());
        }
        const QByteArray json = QJsonDocument(capture).toJson(
            QJsonDocument::Compact);
        if (out.write(json) != json.size()) {
            return err(QStringLiteral("failed to write complete audio capture"));
        }
        out.close();

        return QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("path"), outPath},
            {QStringLiteral("bytes"), json.size()},
            {QStringLiteral("active"), capture.value(QStringLiteral("active"))},
            {QStringLiteral("capturedBytes"),
             capture.value(QStringLiteral("capturedBytes"))},
            {QStringLiteral("chunkCount"),
             capture.value(QStringLiteral("chunkCount"))},
        };
    }

    if (normalizedAction == QLatin1String("probenr2stereo")) {
        return m_audioEngine->automationNr2StereoProbe();
    }
    if (normalizedAction == QLatin1String("probedspstereo")) {
        return m_audioEngine->automationDspStereoProbe(arg);
    }

    return err(QStringLiteral("audioCapture action must be start, stop, status, read, probeNr2Stereo, or probeDspStereo"));
}

QJsonObject AutomationServer::doWhoami() const
{
    return QJsonObject{
        {QStringLiteral("ok"), true},
        {QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid())},
        {QStringLiteral("name"), m_serverName},
        {QStringLiteral("socket"), fullServerName()},
        {QStringLiteral("label"), m_label},
        {QStringLiteral("station"), m_agentStation},
        {QStringLiteral("agentName"), AppSettings::instance().automationAgentName()},
        {QStringLiteral("automationIdentity"), AppSettings::instance().automationIdentity()},
        {QStringLiteral("guiClientId"), AppSettings::instance().effectiveGuiClientId()},
        {QStringLiteral("guiClientIdTransient"),
         AppSettings::instance().guiClientIdentityIsTransient()},
        {QStringLiteral("txAllowed"), m_txAllowed},
        {QStringLiteral("readOnly"), m_readOnly},
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
    };
}

// ---------------------------------------------------------------------------
// Observability suite (#3646): log control, event ring, markers.
// ---------------------------------------------------------------------------

namespace {

const char* msgTypeName(int t)
{
    switch (t) {
    case QtDebugMsg:    return "D";
    case QtInfoMsg:     return "I";
    case QtWarningMsg:  return "W";
    case QtCriticalMsg: return "C";
    case QtFatalMsg:    return "F";
    default:            return "?";
    }
}

} // namespace

// Serialize one event for the wire. PII is redacted here, on egress, so the
// in-memory ring stays raw (cheap tap) but nothing sensitive ever leaves.
QJsonObject AutomationServer::logEventToJson(const LogEvent& e)
{
    return QJsonObject{
        {QStringLiteral("type"),    QStringLiteral("log")},
        {QStringLiteral("seq"),     static_cast<qint64>(e.seq)},
        {QStringLiteral("mono_us"), e.monoUs},
        {QStringLiteral("t"),       e.wall},
        {QStringLiteral("lvl"),     QString::fromLatin1(msgTypeName(e.type))},
        {QStringLiteral("cat"),     e.cat},
        {QStringLiteral("msg"),     redactPii(e.msg)},
    };
}

QJsonObject AutomationServer::doLog(const QString& action, const QString& arg,
                                    QLocalSocket* sock)
{
    auto& lm = LogManager::instance();

    if (action == QLatin1String("categories")) {
        QJsonArray arr;
        for (const auto& c : lm.categories())
            arr.append(QJsonObject{{QStringLiteral("id"), c.id},
                                   {QStringLiteral("label"), c.label},
                                   {QStringLiteral("enabled"), c.enabled}});
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("categories"), arr}};
    }

    if (action == QLatin1String("get")) {
        if (arg.isEmpty())
            return err(QStringLiteral("log get requires a category id"));
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), arg},
                           {QStringLiteral("enabled"), lm.isEnabled(arg)}};
    }

    if (action == QLatin1String("set")) {
        // "<id> <on|off>" or "<id>=<on|off>"; id "all" toggles every category.
        const QString id = arg.section(QRegularExpression(QStringLiteral("[ =]")), 0, 0).trimmed();
        const QString st = arg.section(QRegularExpression(QStringLiteral("[ =]")), 1).trimmed().toLower();
        if (id.isEmpty() || st.isEmpty())
            return err(QStringLiteral("log set requires '<category> <on|off>'"));
        const bool on = (st == QLatin1String("on") || st == QLatin1String("true")
                         || st == QLatin1String("1") || st == QLatin1String("debug"));
        if (id == QLatin1String("all")) {
            lm.setAllEnabled(on);
            return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
                               {QStringLiteral("enabled"), on}};
        }
        lm.setEnabled(id, on);
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), id},
                           {QStringLiteral("enabled"), lm.isEnabled(id)}};
    }

    if (action == QLatin1String("reset")) {
        lm.loadSettings();  // restore the operator's persisted category prefs
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("reset"), true}};
    }

    if (action == QLatin1String("tail")) {
        // "[n] [since=<seq>]" — newest n events, optionally only seq > since.
        int n = 100;
        quint64 since = 0;
        for (const QString& tokn : arg.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            if (tokn.startsWith(QLatin1String("since=")))
                since = tokn.mid(6).toULongLong();
            else
                n = tokn.toInt();
        }
        if (n <= 0) n = 100;
        QJsonArray arr;
        quint64 curSeq;
        quint64 oldest;
        {
            QMutexLocker lk(&m_logMutex);
            curSeq = m_logSeq;
            // Oldest seq still resident in the ring. A driver can compare its
            // `since` against this to detect eviction: `since < oldest` means
            // earlier matching events were dropped (#3756) and the window is a
            // truncated suffix, not a complete bracket.
            oldest = m_logRing.empty() ? curSeq : m_logRing.front().seq;
            for (const auto& e : m_logRing)
                if (e.seq > since)
                    arr.append(logEventToJson(e));
        }
        while (arr.size() > n)              // keep the newest n
            arr.removeFirst();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("events"), arr},
                           {QStringLiteral("seq"), static_cast<qint64>(curSeq)},
                           {QStringLiteral("oldest"), static_cast<qint64>(oldest)}};
    }

    if (action == QLatin1String("subscribe")) {
        if (!sock)
            return err(QStringLiteral("subscribe requires a connected client"));
        QMutexLocker lk(&m_logMutex);
        m_logSubscribers.insert(sock, m_logSeq);  // stream events from now on
        if (m_logDrain && !m_logDrain->isActive())
            m_logDrain->start();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("subscribed"), true},
                           {QStringLiteral("seq"), static_cast<qint64>(m_logSeq)}};
    }

    if (action == QLatin1String("unsubscribe")) {
        const bool was = sock && m_logSubscribers.remove(sock);
        if (m_logSubscribers.isEmpty() && m_logDrain)
            m_logDrain->stop();
        return QJsonObject{{QStringLiteral("ok"), true},
                           {QStringLiteral("subscribed"), false},
                           {QStringLiteral("was"), was}};
    }

    return err(QStringLiteral("log action must be "
                              "categories|get|set|reset|tail|subscribe|unsubscribe"));
}

QJsonObject AutomationServer::doMark(const QString& text)
{
    // qCInfo runs the installed handler synchronously on this (main) thread, so
    // the tap that assigns the marker its seq fires *inside* the call below.
    // Capture that seq/mono via a thread_local sink (#3756) instead of re-reading
    // m_logSeq/back() afterward — a concurrent logging thread could push in the
    // re-lock gap and hand back a later event's seq, which `log tail since=<seq>`
    // would then skip, defeating the mark→tail bracket.
    MarkCapture cap;
    g_markSink = &cap;
    qCInfo(lcAutomation).noquote() << "MARK" << text;
    g_markSink = nullptr;

    if (!cap.set) {
        // Tap didn't fire (lcAutomation info logging disabled): fall back to the
        // resident tail so callers still get a usable, if approximate, anchor.
        QMutexLocker lk(&m_logMutex);
        cap.seq    = m_logSeq;
        cap.monoUs = m_logRing.empty() ? 0 : m_logRing.back().monoUs;
    }
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("seq"), static_cast<qint64>(cap.seq)},
                       {QStringLiteral("mono_us"), cap.monoUs},
                       {QStringLiteral("text"), text}};
}

void AutomationServer::onLogDrain()
{
    if (m_logSubscribers.isEmpty()) {
        if (m_logDrain) m_logDrain->stop();
        return;
    }

    // Copy just the new tail once, under the lock, then write to sockets
    // outside it so logging threads are never blocked on socket I/O.
    quint64 minLast = std::numeric_limits<quint64>::max();
    for (const quint64 v : m_logSubscribers)
        minLast = std::min(minLast, v);

    std::deque<LogEvent> fresh;
    quint64 curSeq;
    {
        QMutexLocker lk(&m_logMutex);
        curSeq = m_logSeq;
        for (const auto& e : m_logRing)
            if (e.seq > minLast)
                fresh.push_back(e);
    }
    if (fresh.empty())
        return;

    for (auto it = m_logSubscribers.begin(); it != m_logSubscribers.end(); ++it) {
        QLocalSocket* s = it.key();
        const quint64 last = it.value();
        for (const auto& e : fresh) {
            if (e.seq <= last)
                continue;
            s->write(QJsonDocument(logEventToJson(e)).toJson(QJsonDocument::Compact));
            s->write("\n");
        }
        s->flush();
        it.value() = curSeq;
    }
}
} // namespace AetherSDR
