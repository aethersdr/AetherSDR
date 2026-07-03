#pragma once

#include <QElapsedTimer>
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
    QSize cardSizeFor(const PanadapterOverlayMessage& message) const;
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
    QElapsedTimer m_clock;
    QTimer m_animationTimer;
    QTimer m_expiryTimer;
    QTimer m_countdownTimer;
    quint64 m_nextSequence{1};
};

} // namespace AetherSDR
