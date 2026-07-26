#pragma once

#include <QElapsedTimer>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVector>
#include <QWidget>

namespace AetherSDR {

enum class PanadapterOverlayMessageTone {
    Info,
    Warning,
};

struct PanadapterOverlayMessage {
    QString id;
    QString title;
    QString detail;
    int timeoutMs{0};       // <= 0 means persistent until the owner removes it.
    bool dismissible{true};
    // For owner-managed status cards, which are re-asserted from many
    // event-driven sync sites and describe a condition that is still true.
    // Such a card cannot be `dismissible`: a close either lies (the owner
    // re-upserts it seconds later) or, in a quiet state that never re-syncs,
    // permanently hides the only indicator that the pan is stale. Collapsing
    // instead shrinks it to a one-line pill — it stops covering the spectrum
    // but never stops being visible. The collapsed state is held by the
    // overlay per id, so owner re-assertion preserves it. (#4387)
    bool collapsible{false};
    PanadapterOverlayMessageTone tone{PanadapterOverlayMessageTone::Info};
};

class PanadapterMessageOverlay : public QWidget {
    Q_OBJECT

public:
    explicit PanadapterMessageOverlay(QWidget* parent = nullptr);

    bool upsertMessage(PanadapterOverlayMessage message);
    bool removeMessage(const QString& id);
    void clearMessages();
    bool hasMessages() const;
    QVariantList messageSnapshot() const;

    // Collapse/expand a `collapsible` card. Returns false for an unknown id or
    // a card that is not collapsible. The state is keyed by id and survives
    // owner re-assertion via upsertMessage(); only removeMessage() /
    // clearMessages() clear it, because that is the owner saying the condition
    // itself ended and a later re-raise is a new occurrence.
    bool setMessageCollapsed(const QString& id, bool collapsed);
    bool isMessageCollapsed(const QString& id) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    enum class RemoveReason {
        Owner,
        Manual,
        Expired,
    };

    struct Item {
        PanadapterOverlayMessage message;
        QToolButton* closeButton{nullptr};
        QRectF currentRect;
        QRectF targetRect;
        qreal opacity{0.0};
        qreal targetOpacity{1.0};
        qint64 expiresAtMs{-1};
        quint64 sequence{0};
        bool removing{false};
    };

    QString normalizedId(const QString& id) const;
    bool isCollapsed(const PanadapterOverlayMessage& message) const;
    QString collapsedTextFor(const PanadapterOverlayMessage& message) const;
    QSize cardSizeFor(const PanadapterOverlayMessage& message) const;
    QSize collapsedCardSizeFor(const PanadapterOverlayMessage& message) const;
    int indexOf(const QString& id) const;
    QToolButton* ensureCloseButton(Item& item);
    void requestRemove(const QString& id, RemoveReason reason);
    void relayout();
    void tickAnimation();
    void expireDueMessages();
    void scheduleExpiryTimer();
    void updateCloseButtons();
    void updateInputMask();
    QString accessibleSummary() const;
    void updateAccessibilityDescription(bool notify);
    bool pointInsideMessage(const QPoint& point) const;

    QVector<Item> m_items;
    // Ids the user has collapsed. Kept outside Item deliberately: Item is
    // rebuilt by upsertMessage() on every owner re-assertion, and the whole
    // point is that the user's choice outlives those.
    QSet<QString> m_collapsedIds;
    QElapsedTimer m_clock;
    QTimer m_animationTimer;
    QTimer m_expiryTimer;
    QTimer m_countdownTimer;
    quint64 m_nextSequence{1};
};

} // namespace AetherSDR
