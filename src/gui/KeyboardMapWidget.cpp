#include "KeyboardMapWidget.h"
#include "core/ShortcutManager.h"

#include <QPainter>
#include <QMouseEvent>

namespace MasterSDR {

// ─── Category colors ────────────────────────────────────────────────────────

static const QColor kUnboundFill(0x1a, 0x2a, 0x3a);
static const QColor kUnboundBorder(0x30, 0x40, 0x50);

QColor KeyboardMapWidget::categoryColor(const QString& cat) const
{
    if (cat == "Frequency") return QColor(0x00, 0x60, 0x80);
    if (cat == "TX")        return QColor(0x80, 0x00, 0x20);
    if (cat == "Audio")     return QColor(0x00, 0x60, 0x40);
    if (cat == "Display")   return QColor(0x60, 0x60, 0x00);
    if (cat == "Slice")     return QColor(0x40, 0x00, 0x80);
    if (cat == "Mode")      return QColor(0x40, 0x00, 0x80);
    if (cat == "DSP")       return QColor(0x00, 0x40, 0x80);
    if (cat == "Tuning")    return QColor(0x00, 0x60, 0x60);
    if (cat == "AGC")       return QColor(0x00, 0x40, 0x80);
    if (cat == "Filter")    return QColor(0x00, 0x40, 0x80);
    if (cat == "Band")      return QColor(0x00, 0x60, 0x80);
    if (cat == "RIT/XIT")   return QColor(0x00, 0x60, 0x60);
    return kUnboundFill;
}

// ─── Construction ───────────────────────────────────────────────────────────

KeyboardMapWidget::KeyboardMapWidget(ShortcutManager* mgr, QWidget* parent)
    : QWidget(parent), m_mgr(mgr)
{
    setMouseTracking(true);
    buildLayout();
    connect(mgr, &ShortcutManager::bindingsChanged, this, [this]() { update(); });
}

// ─── ANSI keyboard layout ──────────────────────────────────────────────────

void KeyboardMapWidget::buildLayout()
{
    m_keys.clear();

    // Row 0: Function keys
    //   Esc, gap, F1-F4, gap, F5-F8, gap, F9-F12
    m_keys.append({0, 0, 1, 1, Qt::Key_Escape, "ESC", {}});

    m_keys.append({2, 0, 1, 1, Qt::Key_F1, "F1", {}});
    m_keys.append({3, 0, 1, 1, Qt::Key_F2, "F2", {}});
    m_keys.append({4, 0, 1, 1, Qt::Key_F3, "F3", {}});
    m_keys.append({5, 0, 1, 1, Qt::Key_F4, "F4", {}});

    m_keys.append({6.5f, 0, 1, 1, Qt::Key_F5, "F5", {}});
    m_keys.append({7.5f, 0, 1, 1, Qt::Key_F6, "F6", {}});
    m_keys.append({8.5f, 0, 1, 1, Qt::Key_F7, "F7", {}});
    m_keys.append({9.5f, 0, 1, 1, Qt::Key_F8, "F8", {}});

    m_keys.append({11, 0, 1, 1, Qt::Key_F9, "F9", {}});
    m_keys.append({12, 0, 1, 1, Qt::Key_F10, "F10", {}});
    m_keys.append({13, 0, 1, 1, Qt::Key_F11, "F11", {}});
    m_keys.append({14, 0, 1, 1, Qt::Key_F12, "F12", {}});

    // Row 0 right: Print/Scroll/Pause
    m_keys.append({15.5f, 0, 1, 1, Qt::Key_Print, "PRT", {}});
    m_keys.append({16.5f, 0, 1, 1, Qt::Key_ScrollLock, "SCR", {}});
    m_keys.append({17.5f, 0, 1, 1, Qt::Key_Pause, "PAU", {}});

    // Row 1: Number row
    float y1 = 1.25f;
    m_keys.append({0, y1, 1, 1, Qt::Key_QuoteLeft, "`", "~"});
    m_keys.append({1, y1, 1, 1, Qt::Key_1, "1", "!"});
    m_keys.append({2, y1, 1, 1, Qt::Key_2, "2", "@"});
    m_keys.append({3, y1, 1, 1, Qt::Key_3, "3", "#"});
    m_keys.append({4, y1, 1, 1, Qt::Key_4, "4", "$"});
    m_keys.append({5, y1, 1, 1, Qt::Key_5, "5", "%"});
    m_keys.append({6, y1, 1, 1, Qt::Key_6, "6", "^"});
    m_keys.append({7, y1, 1, 1, Qt::Key_7, "7", "&"});
    m_keys.append({8, y1, 1, 1, Qt::Key_8, "8", "*"});
    m_keys.append({9, y1, 1, 1, Qt::Key_9, "9", "("});
    m_keys.append({10, y1, 1, 1, Qt::Key_0, "0", ")"});
    m_keys.append({11, y1, 1, 1, Qt::Key_Minus, "-", "_"});
    m_keys.append({12, y1, 1, 1, Qt::Key_Equal, "=", "+"});
    m_keys.append({13, y1, 2, 1, Qt::Key_Backspace, "BACK", {}});

    m_keys.append({15.5f, y1, 1, 1, Qt::Key_Insert, "INS", {}});
    m_keys.append({16.5f, y1, 1, 1, Qt::Key_Home, "HOM", {}});
    m_keys.append({17.5f, y1, 1, 1, Qt::Key_PageUp, "PGU", {}});

    // Numpad row 1
    m_keys.append({19, y1, 1, 1, Qt::Key_NumLock, "NUM", {}});
    m_keys.append({20, y1, 1, 1, Qt::Key_Slash, "/", {}});
    m_keys.append({21, y1, 1, 1, Qt::Key_Asterisk, "*", {}});
    m_keys.append({22, y1, 1, 1, Qt::Key_Minus, "-", {}});

    // Row 2: QWERTY
    float y2 = 2.25f;
    m_keys.append({0, y2, 1.5f, 1, Qt::Key_Tab, "TAB", {}});
    m_keys.append({1.5f, y2, 1, 1, Qt::Key_Q, "Q", {}});
    m_keys.append({2.5f, y2, 1, 1, Qt::Key_W, "W", {}});
    m_keys.append({3.5f, y2, 1, 1, Qt::Key_E, "E", {}});
    m_keys.append({4.5f, y2, 1, 1, Qt::Key_R, "R", {}});
    m_keys.append({5.5f, y2, 1, 1, Qt::Key_T, "T", {}});
    m_keys.append({6.5f, y2, 1, 1, Qt::Key_Y, "Y", {}});
    m_keys.append({7.5f, y2, 1, 1, Qt::Key_U, "U", {}});
    m_keys.append({8.5f, y2, 1, 1, Qt::Key_I, "I", {}});
    m_keys.append({9.5f, y2, 1, 1, Qt::Key_O, "O", {}});
    m_keys.append({10.5f, y2, 1, 1, Qt::Key_P, "P", {}});
    m_keys.append({11.5f, y2, 1, 1, Qt::Key_BracketLeft, "[", "{"});
    m_keys.append({12.5f, y2, 1, 1, Qt::Key_BracketRight, "]", "}"});
    m_keys.append({13.5f, y2, 1.5f, 1, Qt::Key_Backslash, "\\", "|"});

    m_keys.append({15.5f, y2, 1, 1, Qt::Key_Delete, "DEL", {}});
    m_keys.append({16.5f, y2, 1, 1, Qt::Key_End, "END", {}});
    m_keys.append({17.5f, y2, 1, 1, Qt::Key_PageDown, "PGD", {}});

    // Numpad row 2
    m_keys.append({19, y2, 1, 1, Qt::Key_7, "7", {}});
    m_keys.append({20, y2, 1, 1, Qt::Key_8, "8", {}});
    m_keys.append({21, y2, 1, 1, Qt::Key_9, "9", {}});
    m_keys.append({22, y2, 1, 2, Qt::Key_Plus, "+", {}});

    // Row 3: Home row
    float y3 = 3.25f;
    m_keys.append({0, y3, 1.75f, 1, Qt::Key_CapsLock, "CAPS", {}});
    m_keys.append({1.75f, y3, 1, 1, Qt::Key_A, "A", {}});
    m_keys.append({2.75f, y3, 1, 1, Qt::Key_S, "S", {}});
    m_keys.append({3.75f, y3, 1, 1, Qt::Key_D, "D", {}});
    m_keys.append({4.75f, y3, 1, 1, Qt::Key_F, "F", {}});
    m_keys.append({5.75f, y3, 1, 1, Qt::Key_G, "G", {}});
    m_keys.append({6.75f, y3, 1, 1, Qt::Key_H, "H", {}});
    m_keys.append({7.75f, y3, 1, 1, Qt::Key_J, "J", {}});
    m_keys.append({8.75f, y3, 1, 1, Qt::Key_K, "K", {}});
    m_keys.append({9.75f, y3, 1, 1, Qt::Key_L, "L", {}});
    m_keys.append({10.75f, y3, 1, 1, Qt::Key_Semicolon, ";", ":"});
    m_keys.append({11.75f, y3, 1, 1, Qt::Key_Apostrophe, "'", "\""});
    m_keys.append({12.75f, y3, 2.25f, 1, Qt::Key_Return, "ENTER", {}});

    // Numpad row 3
    m_keys.append({19, y3, 1, 1, Qt::Key_4, "4", {}});
    m_keys.append({20, y3, 1, 1, Qt::Key_5, "5", {}});
    m_keys.append({21, y3, 1, 1, Qt::Key_6, "6", {}});

    // Row 4: Bottom letter row
    float y4 = 4.25f;
    m_keys.append({0, y4, 2.25f, 1, Qt::Key_Shift, "SHIFT", {}});
    m_keys.append({2.25f, y4, 1, 1, Qt::Key_Z, "Z", {}});
    m_keys.append({3.25f, y4, 1, 1, Qt::Key_X, "X", {}});
    m_keys.append({4.25f, y4, 1, 1, Qt::Key_C, "C", {}});
    m_keys.append({5.25f, y4, 1, 1, Qt::Key_V, "V", {}});
    m_keys.append({6.25f, y4, 1, 1, Qt::Key_B, "B", {}});
    m_keys.append({7.25f, y4, 1, 1, Qt::Key_N, "N", {}});
    m_keys.append({8.25f, y4, 1, 1, Qt::Key_M, "M", {}});
    m_keys.append({9.25f, y4, 1, 1, Qt::Key_Comma, ",", "<"});
    m_keys.append({10.25f, y4, 1, 1, Qt::Key_Period, ".", ">"});
    m_keys.append({11.25f, y4, 1, 1, Qt::Key_Slash, "/", "?"});
    m_keys.append({12.25f, y4, 2.75f, 1, Qt::Key_Shift, "SHIFT", {}});

    m_keys.append({16.5f, y4, 1, 1, Qt::Key_Up, "\xE2\x96\xB2", {}});  // ▲

    // Numpad row 4
    m_keys.append({19, y4, 1, 1, Qt::Key_1, "1", {}});
    m_keys.append({20, y4, 1, 1, Qt::Key_2, "2", {}});
    m_keys.append({21, y4, 1, 1, Qt::Key_3, "3", {}});
    m_keys.append({22, y4, 1, 2, Qt::Key_Enter, "ENT", {}});

    // Row 5: Modifier row
    float y5 = 5.25f;
    m_keys.append({0, y5, 1.5f, 1, Qt::Key_Control, "CTRL", {}});
    m_keys.append({1.5f, y5, 1.25f, 1, Qt::Key_Super_L, "WIN", {}});
    m_keys.append({2.75f, y5, 1.25f, 1, Qt::Key_Alt, "ALT", {}});
    m_keys.append({4, y5, 6.25f, 1, Qt::Key_Space, "SPACE", {}});
    m_keys.append({10.25f, y5, 1.25f, 1, Qt::Key_Alt, "ALT", {}});
    m_keys.append({11.5f, y5, 1.25f, 1, Qt::Key_Super_R, "WIN", {}});
    m_keys.append({12.75f, y5, 1.25f, 1, Qt::Key_Menu, "MENU", {}});
    m_keys.append({14, y5, 1, 1, Qt::Key_Control, "CTRL", {}});

    m_keys.append({15.5f, y5, 1, 1, Qt::Key_Left, "\xE2\x97\x80", {}});   // ◀
    m_keys.append({16.5f, y5, 1, 1, Qt::Key_Down, "\xE2\x96\xBC", {}});   // ▼
    m_keys.append({17.5f, y5, 1, 1, Qt::Key_Right, "\xE2\x96\xB6", {}});  // ▶

    // Numpad row 5
    m_keys.append({19, y5, 2, 1, Qt::Key_0, "0", {}});
    m_keys.append({21, y5, 1, 1, Qt::Key_Period, ".", {}});
}

// ─── Geometry helpers ───────────────────────────────────────────────────────

QRectF KeyboardMapWidget::keyRect(const KeyCap& k) const
{
    constexpr float gap = 0.08f;  // gap between keys in key units
    float x = m_originX + k.x * (m_keyUnit + gap * m_keyUnit);
    float y = m_originY + k.y * (m_keyUnit + gap * m_keyUnit);
    float w = k.w * m_keyUnit + (k.w - 1) * gap * m_keyUnit;
    float h = k.h * m_keyUnit + (k.h - 1) * gap * m_keyUnit;
    return {x, y, w, h};
}

int KeyboardMapWidget::hitTest(const QPoint& pos) const
{
    for (int i = 0; i < m_keys.size(); ++i) {
        if (keyRect(m_keys[i]).contains(pos))
            return i;
    }
    return -1;
}

Qt::Key KeyboardMapWidget::selectedKey() const
{
    if (m_selectedIdx >= 0 && m_selectedIdx < m_keys.size())
        return m_keys[m_selectedIdx].qtKey;
    return Qt::Key_unknown;
}

// ─── Painting ───────────────────────────────────────────────────────────────

void KeyboardMapWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x0a, 0x0a, 0x14));

    // Compute key unit size to fit the layout (23 key units wide for main + numpad)
    constexpr float totalKeysW = 23.5f;
    constexpr float totalKeysH = 6.5f;
    constexpr float gap = 0.08f;

    float availW = width() - 20;
    float availH = height() - 20;
    float unitFromW = availW / (totalKeysW * (1 + gap));
    float unitFromH = availH / (totalKeysH * (1 + gap));
    m_keyUnit = std::min(unitFromW, unitFromH);
    m_originX = 10;
    m_originY = 10;

    QFont keyFont;
    keyFont.setPixelSize(std::max(8, static_cast<int>(m_keyUnit * 0.28f)));
    keyFont.setBold(true);

    QFont actionFont;
    actionFont.setPixelSize(std::max(6, static_cast<int>(m_keyUnit * 0.18f)));

    for (int i = 0; i < m_keys.size(); ++i) {
        const auto& k = m_keys[i];
        QRectF r = keyRect(k);

        // Look up bound action
        QKeySequence seq(k.qtKey);
        const auto* act = m_mgr->actionForKey(seq);

        // Determine colors
        QColor fill = kUnboundFill;
        QColor border = kUnboundBorder;
        if (act) {
            fill = categoryColor(act->category);
            border = fill.lighter(140);
        }

        // Hover highlight
        if (i == m_hoverIdx) {
            fill = fill.lighter(130);
            border = border.lighter(130);
        }

        // Selected highlight
        if (i == m_selectedIdx) {
            border = QColor(0x00, 0xb4, 0xd8);
            fill = fill.lighter(150);
        }

        // Draw key background
        p.setPen(QPen(border, 1));
        p.setBrush(fill);
        p.drawRoundedRect(r, 4, 4);

        // Extra label (shift character, top-left)
        if (!k.extraLabel.isEmpty()) {
            p.setFont(actionFont);
            p.setPen(QColor(0x80, 0x90, 0xa0));
            QRectF extraR = r.adjusted(3, 2, -3, -r.height() * 0.6);
            p.drawText(extraR, Qt::AlignLeft | Qt::AlignTop, k.extraLabel);
        }

        // Key label (center-top)
        p.setFont(keyFont);
        p.setPen(Qt::white);
        QRectF labelR = r;
        if (act)
            labelR = r.adjusted(0, 2, 0, -r.height() * 0.4);
        p.drawText(labelR, Qt::AlignCenter, k.label);

        // Action label (bottom, smaller)
        if (act) {
            p.setFont(actionFont);
            p.setPen(QColor(0xc0, 0xd0, 0xe0));
            QRectF actR = r.adjusted(2, r.height() * 0.55, -2, -2);
            QString actionText = act->displayName;
            QFontMetrics fm(actionFont);
            actionText = fm.elidedText(actionText, Qt::ElideRight,
                                       static_cast<int>(actR.width()));
            p.drawText(actR, Qt::AlignCenter, actionText);
        }
    }
}

// ─── Mouse interaction ──────────────────────────────────────────────────────

void KeyboardMapWidget::mousePressEvent(QMouseEvent* ev)
{
    int idx = hitTest(ev->pos());
    if (idx >= 0) {
        m_selectedIdx = idx;
        update();
        emit keySelected(m_keys[idx].qtKey);
    }
    ev->accept();
}

void KeyboardMapWidget::mouseMoveEvent(QMouseEvent* ev)
{
    int idx = hitTest(ev->pos());
    if (idx != m_hoverIdx) {
        m_hoverIdx = idx;
        setCursor(idx >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    ev->accept();
}

void KeyboardMapWidget::leaveEvent(QEvent*)
{
    if (m_hoverIdx >= 0) {
        m_hoverIdx = -1;
        update();
    }
}

} // namespace MasterSDR
