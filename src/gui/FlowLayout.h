#pragma once

#include <QLayout>
#include <QList>
#include <QRect>

namespace AetherSDR {

// Left-to-right layout that wraps to a new row when it runs out of
// horizontal space, rather than compressing children to illegibility.
// Born file-local in DxClusterDialog for the Spot List band-filter
// checkboxes (#4157); promoted to a shared class when the Demo Noise
// applet hit the same failure mode in the fixed-width applet rail
// (#4518). Use it for any dense control strip that must stay readable
// at the 260 px rail width — the rail's scroll area is widget-resizable
// with horizontal scrolling off, so a plain QHBoxLayout there is handed
// less than its minimum and clips, it never overflows.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(int margin, int hSpacing, int vSpacing)
        : m_hSpace(hSpacing), m_vSpace(vSpacing) {
        setContentsMargins(margin, margin, margin, margin);
    }
    ~FlowLayout() override {
        while (QLayoutItem* item = takeAt(0))
            delete item;
    }

    void addItem(QLayoutItem* item) override { m_items.append(item); }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return doLayout(QRect(0, 0, width, 0), true); }
    int count() const override { return m_items.size(); }
    QLayoutItem* itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem* takeAt(int index) override {
        return (index >= 0 && index < m_items.size()) ? m_items.takeAt(index) : nullptr;
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override {
        QSize size;
        for (QLayoutItem* item : m_items)
            size = size.expandedTo(item->minimumSize());
        const QMargins m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }

private:
    int doLayout(const QRect& rect, bool testOnly) const {
        const QMargins m = contentsMargins();
        QRect effectiveRect = rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = effectiveRect.x();
        int y = effectiveRect.y();
        int lineHeight = 0;
        for (QLayoutItem* item : m_items) {
            int nextX = x + item->sizeHint().width() + m_hSpace;
            if (nextX - m_hSpace > effectiveRect.right() && lineHeight > 0) {
                x = effectiveRect.x();
                y += lineHeight + m_vSpace;
                nextX = x + item->sizeHint().width() + m_hSpace;
                lineHeight = 0;
            }
            if (!testOnly)
                item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
            x = nextX;
            lineHeight = qMax(lineHeight, item->sizeHint().height());
        }
        return y + lineHeight - rect.y() + m.bottom();
    }

    QList<QLayoutItem*> m_items;
    int m_hSpace;
    int m_vSpace;
};

}  // namespace AetherSDR
