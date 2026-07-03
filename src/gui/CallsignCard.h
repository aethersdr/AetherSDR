#pragma once

#include "core/CallsignInfo.h"

#include <QFrame>

class QLabel;
class QPushButton;
class QGridLayout;

namespace AetherSDR {

// Contact card for one station: colored vertical accent bar, photo on the
// left, callsign / name / location details to the right.  One widget serves
// every surface that shows "who is this station" — the CW decoder panel,
// the Callsign Lookup dialog, and the future SSB voice-callsign decoder —
// so the operator learns a single visual.
//
//   ┃ ┌────┐  KI6BCJ            Extra
//   ┃ │ 📷 │  Patrick Jensen
//   ┃ └────┘  San Jose, CA, United States
//   ┃          CM97 · Santa Clara
//
// Compact fits the decoder strip at the bottom of a panadapter; Large is
// the lookup-dialog variant with a bigger photo and extra detail rows.
// Colors come from ThemeManager tokens and follow live theme switches.
class CallsignCard : public QFrame {
    Q_OBJECT

public:
    enum class Variant { Compact, Large };

    explicit CallsignCard(Variant variant, QWidget* parent = nullptr);

    // "Looking up KI6BCJ…" placeholder while the network round-trip runs.
    void showPending(const QString& call);
    // Fill from a lookup result. `fromCache` adds a subtle "cached" hint.
    void showInfo(const CallsignInfo& info, bool fromCache);
    // "KI6BCJ — not found" (or other provider error) terminal state.
    void showError(const QString& call, const QString& message);
    // Station photo arrived (path into the photo cache).
    void setPhotoPath(const QString& imagePath);

    void clearCard();

    // Callsign currently displayed (pending, error, or filled).
    QString currentCall() const { return m_call; }

    // Compact cards embed a ✕ button; the host hides the card on this.
    void setCloseButtonVisible(bool visible);

signals:
    void closeRequested();

private:
    void applyTheme();
    void setPlaceholderPhoto();
    int  photoEdge() const;

    Variant m_variant;
    QString m_call;

    QWidget*     m_accentBar{nullptr};
    QLabel*      m_photoLabel{nullptr};
    QLabel*      m_callLabel{nullptr};
    QLabel*      m_classChip{nullptr};
    QLabel*      m_cacheHint{nullptr};
    QLabel*      m_nameLabel{nullptr};
    QLabel*      m_locationLabel{nullptr};
    QLabel*      m_metaLabel{nullptr};
    QPushButton* m_closeBtn{nullptr};
};

} // namespace AetherSDR
