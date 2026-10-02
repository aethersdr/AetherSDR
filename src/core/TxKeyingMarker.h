#pragma once

#include <QRegularExpression>
#include <QStringList>
#include <QWidget>
#include <QPointer>
#include <QVariant>
#include <utility>
#include "models/TxController.h"

namespace AetherSDR {

// Marker for controls that key the transmitter (MOX/PTT, TUNE, ATU tune, CWX
// send, AX.25 send, ...). The automation bridge (#3646) refuses invoke() on a
// marked widget unless AETHER_AUTOMATION_ALLOW_TX is set. Set it at the
// control's creation site; name matching (transmitControlMatch() below, shared
// by the bridge and the keyboard TX activation guard) is only a fallback. Usage:
//     m_moxBtn = new QPushButton("MOX");
//     markTxKeying(m_moxBtn);
inline constexpr char kTxKeyingProperty[] = "aetherTxKeying";

// A control supplies its real controller action, not a label-based model
// shortcut or an ambient "current actor" around QWidget::click(). Preparation
// captures input identity BEFORE the bridge queues invocation. The same UI
// logic remains responsible for toggles, text and per-frequency ATU behavior.
struct TxKeyingAction {
    using Prepared = std::function<void()>;
    using Prepare = std::function<Prepared(const std::shared_ptr<TxController>&,
                                           const QString&, const QString&)>;
    Prepare prepare;
    // Receive controls may optionally establish an automatic-response program.
    // Without a TX controller their prepared action must remain receive-only.
    bool requiresTxPermission{true};
};
inline constexpr char kTxKeyingActionProperty[] = "aetherTxKeyingAction";

inline void markTxKeying(QWidget* w)
{
    if (w)
        w->setProperty(kTxKeyingProperty, true);
}

} // namespace AetherSDR

Q_DECLARE_METATYPE(std::shared_ptr<const AetherSDR::TxKeyingAction>)

namespace AetherSDR {

inline void registerTxKeyingAction(QObject* object, TxKeyingAction::Prepare prepare)
{
    if (!object) {
        return;
    }
    object->setProperty(kTxKeyingProperty, true);
    object->setProperty(kTxKeyingActionProperty,
        QVariant::fromValue(std::make_shared<const TxKeyingAction>(TxKeyingAction{std::move(prepare)})));
}

inline void registerReceiveControlAction(QObject* object, TxKeyingAction::Prepare prepare)
{
    if (!object) { return; }
    object->setProperty(kTxKeyingProperty, true);
    object->setProperty(kTxKeyingActionProperty,
        QVariant::fromValue(std::make_shared<const TxKeyingAction>(
            TxKeyingAction{std::move(prepare), false})));
}

inline bool txActionRequiresPermission(const QObject* object)
{
    const auto endpoint = object ? object->property(kTxKeyingActionProperty)
        .value<std::shared_ptr<const TxKeyingAction>>() : nullptr;
    return !endpoint || endpoint->requiresTxPermission;
}

inline TxKeyingAction::Prepared prepareTxKeyingAction(QObject* object,
    const std::shared_ptr<TxController>& controller, const QString& action, const QString& value)
{
    if (!object || (controller && !controller->valid())) {
        return {};
    }
    const std::shared_ptr<const TxKeyingAction> endpoint =
        object->property(kTxKeyingActionProperty).value<std::shared_ptr<const TxKeyingAction>>();
    if (!endpoint || !endpoint->prepare || (endpoint->requiresTxPermission && !controller)) {
        return {};
    }
    const QPointer<QObject> guard(object);
    TxKeyingAction::Prepared prepared = endpoint->prepare(controller, action, value);
    if (!prepared) {
        return {};
    }
    return [guard, controller, prepared = std::move(prepared)] {
        if (guard && (!controller || controller->valid())) {
            if (const QWidget* widget = qobject_cast<QWidget*>(guard.data()); widget && !widget->isEnabled()) {
                return;
            }
            prepared();
        }
    };
}

// A pointer activation of a TX control uses the same registered action as
// invoke(), with its input captured before delivery. Never send a raw click
// into a native operator callback. Ordinary non-TX widgets still receive Qt
// events through the bridge. Preserve button hit-testing, focus and down state;
// only an explicit release inside may activate, never cancellation/timeout.
class TxPointerAction final {
public:
    static std::shared_ptr<TxPointerAction> prepare(QWidget* hit,
        const std::shared_ptr<TxController>& controller);
    ~TxPointerAction();
    void press(const QPoint& global);
    void move(const QPoint& global);
    void release(const QPoint& global);
    void cancel();
    std::shared_ptr<TxController> controller() const { return m_controller; }

private:
    void clearDownState();
    bool hits(const QPoint& global) const;
    QPointer<QWidget> m_button;
    std::shared_ptr<TxController> m_controller;
    TxKeyingAction::Prepared m_action;
    bool m_started{false};
};

// Lowercased words of an identifier or label, split on separators and camelCase
// humps (aprsSvcWXBOT -> [aprs, svc, wxbot]). The TX-guard fallback matches a
// deny-word against a WHOLE token, so "svc"+"wxbot" never reads as "cwx" (#3646).
inline QStringList identifierTokens(const QString& s)
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
    static const QRegularExpression kSeparators(QStringLiteral("[^a-z0-9]+"));
    return spaced.toLower().split(kSeparators, Qt::SkipEmptyParts);
}

// True if any haystack contributes a whole token equal to a deny-word — the
// anchored TX-guard fallback match.
inline bool matchesTxDenyToken(const QStringList& haystacks, const QStringList& deny)
{
    for (const QString& h : haystacks) {
        const QStringList tokens = identifierTokens(h);
        for (const QString& d : deny)
            if (tokens.contains(d))
                return true;
    }
    return false;
}

// The fallback deny-list: only words that unambiguously mean "keys TX".
// "tune"/"atu"/"vox" also name RX-only controls ("Tune Now", a VOX arm toggle);
// the keying TUNE/ATU buttons carry markTxKeying() instead (#3918).
inline const QStringList& txDenyTokens()
{
    static const QStringList kDeny = {
        QStringLiteral("mox"), QStringLiteral("ptt"),
        QStringLiteral("transmit"), QStringLiteral("cwx"),
    };
    return kDeny;
}

enum class TransmitControlMatch {
    None,          // not a transmit control
    Marker,        // carries the authoritative markTxKeying() marker
    NameFallback,  // unmarked button whose name/label reads as a TX keyer
};

// Is this widget a transmit control? One answer for the automation bridge and
// the keyboard activation guard. The marker is authoritative; the name match is
// a button-only fallback, and a NameFallback control should get markTxKeying().
inline TransmitControlMatch transmitControlMatch(const QWidget* w)
{
    if (!w)
        return TransmitControlMatch::None;
    if (w->property(kTxKeyingProperty).toBool())
        return TransmitControlMatch::Marker;

    // By meta-object name and Q_PROPERTY, not qobject_cast: this header sits in
    // src/core and must not include a QtWidgets class header (engine boundary).
    if (!w->inherits("QAbstractButton"))
        return TransmitControlMatch::None;  // sliders / combos / spinboxes can't trigger TX

    if (w->objectName().startsWith(QStringLiteral("panOverlayMessageClose_")))
        return TransmitControlMatch::None;  // closes an overlay notification, never keys TX.

    const QStringList hay{w->objectName(), w->accessibleName(),
                          w->property("text").toString()};
    return matchesTxDenyToken(hay, txDenyTokens()) ? TransmitControlMatch::NameFallback
                                                   : TransmitControlMatch::None;
}

} // namespace AetherSDR
