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


// ── Is this widget a transmit control? One answer for every guard ──────────
// Shared by the automation bridge (AutomationServer.cpp isTransmitControl /
// hasTransmitControlInChain) and the keyboard activation guard
// (gui/TxKeyActivationGuard.h), so the two cannot drift apart.

// Tokenize an identifier or label into lowercased words, splitting on
// non-alphanumeric separators AND camelCase humps (tuneButton -> [tune, button],
// aprsSvcWXBOT -> [aprs, svc, wxbot], "Auto-Tune" -> [auto, tune]). The TX-guard
// fallback matches a deny-word against a WHOLE token, so a cross-token trigram
// like "cwx" formed by the c in "svc" + "wx" in "wxbot" no longer false-positives
// as the CWX keyer, while genuine keyers (moxButton, pttSend, "Auto-Tune") still
// match. This is the anchored replacement for the old bare contains() blocklist
// that flagged the RX-only APRS weather entry (#3646).
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

// The fallback deny-list, kept narrow: only words that unambiguously mean
// "keys TX". "tune"/"atu"/"vox" were dropped because they false-positive on
// RX-only controls — the "Tune Now" button (net/spot retune) and "Tune to
// <spot>" only move the VFO, and a VOX toggle arms TX rather than keying it.
// The genuine keying TUNE/ATU buttons (TxApplet, AtuPreTuneDialog) all carry
// the authoritative markTxKeying() marker, so removing them here loses no real
// protection. (#3918 — "Tune Now" false-positive)
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

// Authoritative: the positive marker. Fallback: a button-scoped name
// heuristic for a keying control that lacks the marker (button-scoped because
// sliders never key). A NameFallback result means the control should get
// markTxKeying(); callers that can log, should.
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
