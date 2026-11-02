#pragma once

#include <QWidget>
#include <QVector>
#include <QKeySequence>

namespace AetherSDR {

class ShortcutManager;

class KeyboardMapWidget : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardMapWidget(ShortcutManager* mgr, QWidget* parent = nullptr);

    // As tall as the keys at a width (up to a cap), so a layout gives spare
    // height to its other widgets rather than leaving it empty below the keys.
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize minimumSizeHint() const override { return {900, heightForWidth(900)}; }
    QSize sizeHint() const override { return {1100, heightForWidth(1100)}; }

    int selectedKeyIndex() const { return m_selectedIdx; }
    Qt::Key selectedKey() const;

    QColor categoryColor(const QString& category) const;

signals:
    void keySelected(Qt::Key key);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void leaveEvent(QEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void focusInEvent(QFocusEvent* ev) override;
    void focusOutEvent(QFocusEvent* ev) override;

private:
    struct KeyCap {
        float x, y;       // position in key units
        float w;           // width in key units
        float h{1.0f};     // height in key units (default 1.0)
        Qt::Key qtKey;
        QString label;
        QString extraLabel;  // secondary label (e.g. "!" above "1")
    };

    void buildLayout();
    QRectF keyRect(const KeyCap& k) const;
    int hitTest(const QPoint& pos) const;
    int findKeyVertical(int idx, bool up) const;
    void selectFocusKey();
    void announceKey(int idx);

    ShortcutManager* m_mgr;
    QVector<KeyCap> m_keys;
    int m_selectedIdx{-1};
    int m_hoverIdx{-1};
    int m_focusIdx{0};       // keyboard-navigated key (independent of mouse selection)
    float m_keyUnit{0};     // pixel size of one key unit (computed in paintEvent)
    float m_originX{0};
    float m_originY{0};
};

} // namespace AetherSDR
