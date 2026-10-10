#pragma once

#include "core/CallsignInfo.h"

#include <QFrame>

class QLabel;
class QPushButton;
class QGridLayout;

namespace AetherSDR {

// Contact card for one station, shared by every "who is this station" surface
// (CW decoder panel, Callsign Lookup dialog) so the operator learns one visual:
//
//   ┃ ┌────┐  KI6BCJ            Extra
//   ┃ │ 📷 │  Patrick Jensen
//   ┃ └────┘  San Jose, CA, United States
//   ┃          CM97 · Santa Clara
//
// Compact fits the panadapter decoder strip; Large (lookup dialog) has a
// bigger photo and more rows. Colours follow ThemeManager tokens live.
class CallsignCard : public QFrame {
    Q_OBJECT

public:
    enum class Variant { Compact, Large, LiveCw };

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
    void setTextFontPx(int px);

signals:
    void closeRequested();

private:
    void applyTheme();
    void setPlaceholderPhoto();
    int  photoEdge() const;

    Variant m_variant;
    int m_textFontPx{18};
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
