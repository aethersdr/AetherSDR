#include "SpeLcdWidget.h"

#include <QPainter>
#include <QPaintEvent>

namespace AetherSDR {

namespace {

using Spe::Lcd::kRows;
using Spe::Lcd::kCols;

constexpr int kCellW = 6;
constexpr int kCellH = 8;
constexpr int kNativeW = kCols * kCellW;  // 240
constexpr int kNativeH = kRows * kCellH;  // 64
constexpr int kBezel = 6;                 // painted frame around the glass
constexpr int kMinScale = 2;

// The SPE Expert's LCD controller font ROM: 256 glyphs, 8 scanlines each,
// 6 bits wide, MSB-left. Extracted from the contributing author's
// field-proven v2 control application (see the branch's provenance note).
const unsigned char kFontRom[256][8] = {
#include "SpeLcdFontRom.inc"
};

// Hardware-depiction palette (QColor components, not theme material — the
// widget renders a physical green LCD, like the analog meter faces keep
// their own face colours).
const QColor kGlassBg(0x10, 0x20, 0x10);
const QColor kGlassFg(0xd6, 0xf5, 0xd6);
const QColor kGlassDim(0x3a, 0x55, 0x3a);
const QColor kBezelCol(0x22, 0x28, 0x22);

}  // namespace

SpeLcdWidget::SpeLcdWidget(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAccessibleName(tr("Amplifier front-panel display"));
    renderFrame();
}

QSize SpeLcdWidget::minimumSizeHint() const
{
    return {kNativeW * kMinScale + kBezel * 2, kNativeH * kMinScale + kBezel * 2};
}

QSize SpeLcdWidget::sizeHint() const
{
    return minimumSizeHint();
}

void SpeLcdWidget::setFrame(const Spe::Lcd::Frame& frame)
{
    m_frame = frame;
    m_hasFrame = true;
    renderFrame();
    update();
}

void SpeLcdWidget::clear()
{
    if (!m_hasFrame)
        return;
    m_frame = {};
    m_hasFrame = false;
    renderFrame();
    update();
}

void SpeLcdWidget::renderFrame()
{
    // 1x render; paintEvent integer-scales it so glyph pixels stay square
    // and crisp at any window size.
    if (m_image.isNull())
        m_image = QImage(kNativeW, kNativeH, QImage::Format_RGB32);
    m_image.fill(kGlassBg);

    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const unsigned char* glyph = kFontRom[m_frame.chars[row][col]];
            const bool inv = m_frame.inverse[row][col];
            const int x0 = col * kCellW;
            const int y0 = row * kCellH;
            for (int y = 0; y < kCellH; ++y) {
                unsigned char scan = glyph[y];
                if (inv)
                    scan = static_cast<unsigned char>(~scan);
                QRgb* line = reinterpret_cast<QRgb*>(m_image.scanLine(y0 + y)) + x0;
                for (int x = 0; x < kCellW; ++x) {
                    const bool on = scan & (1u << (kCellW - 1 - x));
                    line[x] = (on ? kGlassFg : kGlassBg).rgb();
                }
            }
        }
    }
}

void SpeLcdWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);

    // Largest integer scale that fits inside the bezel.
    const int availW = width() - kBezel * 2;
    const int availH = height() - kBezel * 2;
    int scale = qMin(availW / kNativeW, availH / kNativeH);
    if (scale < 1)
        scale = 1;
    const int w = kNativeW * scale;
    const int h = kNativeH * scale;
    const int x = (width() - w) / 2;
    const int y = (height() - h) / 2;

    p.setPen(Qt::NoPen);
    p.setBrush(kBezelCol);
    p.drawRoundedRect(x - kBezel, y - kBezel, w + kBezel * 2, h + kBezel * 2, 4, 4);
    p.setBrush(kGlassBg);
    p.drawRect(x - 2, y - 2, w + 4, h + 4);

    p.drawImage(QRect(x, y, w, h), m_image);

    if (!m_hasFrame) {
        p.setPen(kGlassDim);
        QFont f = p.font();
        f.setPointSize(9);
        p.setFont(f);
        p.drawText(QRect(x, y, w, h), Qt::AlignCenter, tr("waiting for display…"));
    }
}

}  // namespace AetherSDR
