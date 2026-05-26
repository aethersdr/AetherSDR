#include "TokenEditorWidget.h"
#include "CompactColorPicker.h"
#include "GradientEditorDialog.h"  // re-uses GradientStrip class
#include "core/AppSettings.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFontComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

QString colorToTokenHex(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb)
                            : c.name(QColor::HexArgb);
}

// Paint a 28x24 rounded swatch with the standard 8 px checkerboard
// under a translucent colour fill — mirrors CompactColorPicker's hex
// swatch (so the recent-colors row visually matches that swatch).
QPixmap makeRecentSwatchPixmap(const QColor& c, const QSize& sz)
{
    QPixmap pm(sz);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(
        QRectF(0.5, 0.5, sz.width() - 1.0, sz.height() - 1.0), 3, 3);
    p.save();
    p.setClipPath(clip);
    constexpr int tile = 6;
    for (int y = 0; y < sz.height(); y += tile) {
        for (int x = 0; x < sz.width(); x += tile) {
            const bool dark = ((x / tile + y / tile) & 1) == 0;
            p.fillRect(QRect(x, y, tile, tile),
                       dark ? QColor(0x80, 0x80, 0x80)
                            : QColor(0xc8, 0xc8, 0xc8));
        }
    }
    p.fillRect(pm.rect(), c);
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.drawRoundedRect(
        QRectF(0.5, 0.5, sz.width() - 1.0, sz.height() - 1.0), 3, 3);
    return pm;
}

QIcon stopSwatchIcon(const QColor& c)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0.5, 0.5, 15.0, 15.0), 3, 3);
    p.save();
    p.setClipPath(clip);
    p.fillRect(QRect(0, 0, 16, 16), QColor(0xc8, 0xc8, 0xc8));
    p.fillRect(QRect(0, 0, 8, 8),   QColor(0x80, 0x80, 0x80));
    p.fillRect(QRect(8, 8, 8, 8),   QColor(0x80, 0x80, 0x80));
    p.restore();
    p.setBrush(c);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, 15.0, 15.0), 3, 3);
    return QIcon(pm);
}

} // namespace

TokenEditorWidget::TokenEditorWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    buildHeaderRow(root);
    buildTypeRow(root);

    // Colour + Gradient stops share a single horizontal row so the
    // gradient editor sits to the right of the colour picker — saves
    // vertical real estate and visually couples the two groups since
    // the picker doubles as the selected-stop editor in gradient mode.
    auto* colorRow = new QHBoxLayout;
    colorRow->setSpacing(12);
    // AlignTop on both column widgets ensures their first rows line up
    // against the top of the HBox regardless of how Qt's vertical
    // stretch heuristics distribute the extra height between them.
    colorRow->addWidget(buildColorGroup(), 0, Qt::AlignTop);
    colorRow->addWidget(buildGradientGroup(), 1, Qt::AlignTop);
    root->addLayout(colorRow);

    // Construct the action buttons (Reset / Cancel / OK) before the
    // font row so the font row can embed Cancel + OK at its right end.
    // Reset is reparented out by ThemeEditorDialog (inspect row).
    buildButtonRow(root);
    buildFontGroup(root);

    // Recent-colors swatches are per-theme; when the user switches the
    // active theme (View menu, Save As, etc.) reload them so each
    // theme's history stays distinct.  themeChanged also fires for
    // in-place setColor edits — reloadRecentColorsIfThemeChanged()
    // skips those by comparing the active theme name against the one
    // we last loaded from.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &TokenEditorWidget::reloadRecentColorsIfThemeChanged);

    // Initialise to the "nothing selected" state — every group visible
    // but greyed out so the operator can see the full surface before
    // they pick a token.
    setToken(QString());
}

// ─────────────────────────────────────────────────────────── header row ─

void TokenEditorWidget::buildHeaderRow(QVBoxLayout* root)
{
    Q_UNUSED(root);
    // Label is owned by the editor but placed by the parent
    // ThemeEditorDialog into its inspect row (see headerLabel()) —
    // the editor stays a clean stack of editing controls.
    m_header = new QLabel(this);
    // Word-wrap off so the label width = max line width; the token
    // string is allowed to be long without being broken mid-word.
    // Multi-line layout is produced explicitly via <br/> in setToken().
    m_header->setWordWrap(false);
    m_header->setTextFormat(Qt::RichText);
    m_header->setStyleSheet(QStringLiteral(
        "QLabel { font-weight: bold; }"));
}

// ────────────────────────────────────────────────────── type chooser ────

void TokenEditorWidget::buildTypeRow(QVBoxLayout* root)
{
    auto* row = new QHBoxLayout;
    m_typeLabel = new QLabel(QStringLiteral("Type:"), this);
    row->addWidget(m_typeLabel);
    m_typeFlat     = new QRadioButton(QStringLiteral("Flat color"), this);
    m_typeGradient = new QRadioButton(QStringLiteral("Gradient"),    this);
    m_typeGroup    = new QButtonGroup(this);
    m_typeGroup->addButton(m_typeFlat,     0);
    m_typeGroup->addButton(m_typeGradient, 1);
    row->addWidget(m_typeFlat);
    row->addWidget(m_typeGradient);
    row->addStretch(1);
    root->addLayout(row);

    connect(m_typeGroup, qOverload<int>(&QButtonGroup::idClicked),
            this, &TokenEditorWidget::onColorTypeToggled);
}

// ───────────────────────────────────────────────────────── color group ──

QWidget* TokenEditorWidget::buildColorGroup()
{
    auto* group = new QWidget(this);
    auto* lay = new QVBoxLayout(group);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    // Force the label to the same height as the gradient column's
    // label-row (which contains 22 px tool buttons), so the two
    // headers line up exactly across columns.
    m_colorGroupLabel = new QLabel(QStringLiteral("<b>Color</b>"), group);
    m_colorGroupLabel->setFixedHeight(22);
    lay->addWidget(m_colorGroupLabel);
    m_colorPicker = new CompactColorPicker(group);
    lay->addWidget(m_colorPicker);
    lay->addStretch(1);

    connect(m_colorPicker, &CompactColorPicker::colorChanged,
            this, &TokenEditorWidget::onColorPickerChanged);
    return group;
}

// ────────────────────────────────────────────────────────── font group ──

void TokenEditorWidget::buildFontGroup(QVBoxLayout* root)
{
    m_fontGroupLabel = new QLabel(QStringLiteral("<b>Font</b>"), this);
    root->addWidget(m_fontGroupLabel);

    auto* row = new QHBoxLayout;
    row->setSpacing(4);

    m_fontCombo = new QFontComboBox(this);
    m_fontCombo->setEditable(false);
    m_fontCombo->setMaximumWidth(180);
    // Each family in the dropdown is rendered IN that family at the
    // view's font size.  Default is the app's default size, which
    // makes the list visually overwhelming.  Drop by 4px so the
    // list fits more entries and feels less shouty.
    if (auto* view = m_fontCombo->view()) {
        QFont viewFont = view->font();
        const QFontInfo fi(viewFont);
        viewFont.setPixelSize(std::max(8, fi.pixelSize() - 4));
        view->setFont(viewFont);
    }
    row->addWidget(m_fontCombo);

    m_fontSizeCombo = new QComboBox(this);
    // Preset sizes — the 7-20 range covers `font.size.tiny/small/normal/large`
    // (those scalar tokens are still hard-capped at 20 to keep label
    // rows from overflowing).  Larger sizes are for the compound
    // `font.family.<role>.size` field; the freq label defaults to 26
    // and segment displays often go to 36+.  Bump clamps differ per
    // target — see onFontSizeBumpUp/Down().
    for (int sz : {7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20,
                   24, 28, 32, 36, 48, 60, 72}) {
        m_fontSizeCombo->addItem(QStringLiteral("%1 pt").arg(sz),
                                 QVariant(sz));
    }
    m_fontSizeCombo->setFixedWidth(70);
    row->addWidget(m_fontSizeCombo);

    auto makeStyleBtn = [&](const QString& text, const QString& tip,
                            bool checkable) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setFixedSize(28, 28);
        QFont f = b->font();
        if (text == QStringLiteral("B")) f.setBold(true);
        if (text == QStringLiteral("I")) f.setItalic(true);
        if (text == QStringLiteral("U")) f.setUnderline(true);
        if (text == QStringLiteral("S")) f.setStrikeOut(true);
        b->setFont(f);
        row->addWidget(b);
        return b;
    };
    m_fontBoldBtn      = makeStyleBtn(QStringLiteral("B"),
        QStringLiteral("Bold (placeholder — future font.weight tokens)"),
        true);
    m_fontItalicBtn    = makeStyleBtn(QStringLiteral("I"),
        QStringLiteral("Italic (placeholder — future font.style tokens)"),
        true);
    m_fontUnderlineBtn = makeStyleBtn(QStringLiteral("U"),
        QStringLiteral("Underline (placeholder — future tokens)"),
        true);
    m_fontStrikeBtn    = makeStyleBtn(QStringLiteral("S"),
        QStringLiteral("Strikethrough (placeholder — future tokens)"),
        true);

    m_fontSizeUpBtn   = makeStyleBtn(QStringLiteral("A↑"),
        QStringLiteral("Increase size by 1 pt"), false);
    m_fontSizeDownBtn = makeStyleBtn(QStringLiteral("A↓"),
        QStringLiteral("Decrease size by 1 pt"), false);
    m_fontClearBtn    = makeStyleBtn(QStringLiteral("⌧"),
        QStringLiteral("Clear formatting (placeholder — future tokens)"),
        false);

    row->addStretch(1);
    // Cancel / OK live at the right end of the font row so they're
    // always at a predictable position regardless of token kind.
    // Constructed earlier in buildButtonRow().
    row->addWidget(m_cancelBtn);
    row->addWidget(m_okBtn);
    root->addLayout(row);

    connect(m_fontCombo, &QFontComboBox::currentFontChanged,
            this, [this](const QFont& f) { onFontFamilyChanged(f.family()); });
    connect(m_fontSizeCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_settingControlsFromToken) return;
        bool ok = false;
        const int v = m_fontSizeCombo->currentData().toInt(&ok);
        if (ok) onFontSizeChanged(v);
    });
    connect(m_fontSizeUpBtn,   &QToolButton::clicked,
            this, &TokenEditorWidget::onFontSizeBumpUp);
    connect(m_fontSizeDownBtn, &QToolButton::clicked,
            this, &TokenEditorWidget::onFontSizeBumpDown);
}

// ────────────────────────────────────────────────────── gradient group ──

QWidget* TokenEditorWidget::buildGradientGroup()
{
    auto* group = new QWidget(this);
    auto* lay = new QVBoxLayout(group);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);

    // Header row carries: [Gradient stops] [stretch] [Angle: spin] [+] [−].
    // Putting the angle widget here (rather than in a row of its own
    // below the stop list) reclaims one vertical row in this column and
    // groups every action that affects the gradient as a whole.
    // The row is forced to 22 px tall (matching the Color column's
    // header) so the two column headers stay flush.
    auto* labelRow = new QHBoxLayout;
    labelRow->setSpacing(4);
    labelRow->setContentsMargins(0, 0, 0, 0);
    m_gradGroupLabel = new QLabel(QStringLiteral("<b>Gradient stops</b>"), group);
    m_gradGroupLabel->setFixedHeight(22);
    labelRow->addWidget(m_gradGroupLabel);
    labelRow->addStretch(1);
    auto* angleLabel = new QLabel(QStringLiteral("Angle:"), group);
    labelRow->addWidget(angleLabel);
    m_gradAngleSpin = new QSpinBox(group);
    m_gradAngleSpin->setRange(0, 360);
    m_gradAngleSpin->setSuffix(QStringLiteral("°"));
    m_gradAngleSpin->setSingleStep(15);
    m_gradAngleSpin->setFixedWidth(72);
    labelRow->addWidget(m_gradAngleSpin);
    m_gradAddBtn = new QToolButton(group);
    m_gradAddBtn->setText(QStringLiteral("+"));
    m_gradAddBtn->setFixedSize(22, 22);
    m_gradAddBtn->setToolTip(QStringLiteral(
        "Add a stop between the selected one and the next, or at the "
        "midpoint of the gradient if nothing is selected."));
    m_gradDelBtn = new QToolButton(group);
    m_gradDelBtn->setText(QStringLiteral("−"));
    m_gradDelBtn->setFixedSize(22, 22);
    m_gradDelBtn->setToolTip(QStringLiteral(
        "Remove the selected stop.  Refuses below two stops."));
    labelRow->addWidget(m_gradAddBtn);
    labelRow->addWidget(m_gradDelBtn);
    lay->addLayout(labelRow);

    m_gradStrip = new GradientStrip(group);
    m_gradStrip->setFixedHeight(35);
    lay->addWidget(m_gradStrip);

    m_gradStopList = new QListWidget(group);
    m_gradStopList->setIconSize(QSize(16, 16));
    m_gradStopList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_gradStopList->setFrameShape(QFrame::NoFrame);
    m_gradStopList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_gradStopList->setFixedHeight(164);

    // Wrap the stop list with 8 px horizontal insets so its left/right
    // edges line up with the GradientStrip's painted gradient bar
    // (the strip widget reserves an 8 px kStripMargin on each side
    // before rendering the bar — that's why the unwrapped list looked
    // wider).  Top margin of 4 supplements the column's 4 px default
    // spacing, yielding the requested 8 px gap between strip and list.
    auto* listInset = new QHBoxLayout;
    listInset->setContentsMargins(8, 4, 8, 0);
    listInset->addWidget(m_gradStopList);
    lay->addLayout(listInset);

    // Recent-colors 2×10 grid lives in the gradient column below the
    // stop list — fills the otherwise-empty space and provides quick
    // re-pick of any of the last 20 confirmed colours.  Stays enabled
    // even when the gradient group is disabled (scalar-color tokens).
    buildRecentColorsGrid(lay, group);

    // Soak any leftover vertical space so the gradient column packs
    // tight to the top, matching the Color column's bottom stretch.
    lay->addStretch(1);

    connect(m_gradStrip, &GradientStrip::stopMoved,
            this, &TokenEditorWidget::onGradientStopMoved);
    connect(m_gradStrip, &GradientStrip::stopActivated,
            this, &TokenEditorWidget::onGradientStopActivated);
    connect(m_gradStrip, &GradientStrip::requestNewStop,
            this, &TokenEditorWidget::onGradientRequestNewStop);
    connect(m_gradStrip, &GradientStrip::requestDeleteStop,
            this, &TokenEditorWidget::onGradientRequestDeleteStop);
    connect(m_gradStopList, &QListWidget::itemSelectionChanged,
            this, &TokenEditorWidget::onGradientStopListSelectionChanged);
    connect(m_gradAngleSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &TokenEditorWidget::onGradientAngleChanged);
    connect(m_gradAddBtn, &QToolButton::clicked,
            this, &TokenEditorWidget::onGradientAddStop);
    connect(m_gradDelBtn, &QToolButton::clicked,
            this, &TokenEditorWidget::onGradientDeleteStop);
    return group;
}

// ─────────────────────────────────────────────── recent colors grid ─────

void TokenEditorWidget::buildRecentColorsGrid(QVBoxLayout* lay, QWidget* group)
{
    auto* label = new QLabel(QStringLiteral("<i>Recent colors</i>"), group);
    label->setStyleSheet(QStringLiteral("QLabel { color: #8ea8c0; }"));
    lay->addWidget(label);

    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    grid->setContentsMargins(0, 0, 0, 0);
    m_recentButtons.reserve(20);
    for (int i = 0; i < 20; ++i) {
        auto* b = new QToolButton(group);
        b->setFixedSize(28, 24);
        b->setIconSize(QSize(28, 24));
        b->setAutoRaise(false);
        b->setToolTip(QStringLiteral("Empty slot"));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        // Transparent button frame so the icon (a rounded swatch
        // matching the hex swatch) is the only thing the user sees.
        b->setStyleSheet(QStringLiteral(
            "QToolButton { background: transparent; border: none; padding: 0px; }"
            "QToolButton:hover { background: rgba(0, 188, 212, 40); border-radius: 3px; }"));
        const int row = i / 10;
        const int col = i % 10;
        grid->addWidget(b, row, col);
        m_recentButtons.append(b);
        connect(b, &QToolButton::clicked, this, [this, i]() {
            applyRecentColor(i);
        });
        // Right-click on a populated slot removes that color from the
        // recents list (no confirmation — recents are recoverable by
        // simply re-picking the colour and confirming with OK).
        connect(b, &QToolButton::customContextMenuRequested,
                this, [this, i](const QPoint&) {
            if (i < 0 || i >= m_recentColors.size()) return;
            m_recentColors.removeAt(i);
            saveRecentColors();
            refreshRecentButtons();
        });
    }
    lay->addLayout(grid);

    loadRecentColors();
    refreshRecentButtons();
}

QString TokenEditorWidget::recentColorsKeyFor(const QString& themeName)
{
    // Per-theme bucket — themes that share a name on disk share their
    // recents.  Fall back to a global key when no theme is active so
    // we never write a stray "ThemeEditor.RecentColors/" entry.
    if (themeName.isEmpty()) return QStringLiteral("ThemeEditor.RecentColors");
    return QStringLiteral("ThemeEditor.RecentColors/") + themeName;
}

void TokenEditorWidget::loadRecentColors()
{
    m_recentColors.clear();
    m_recentColorsTheme = ThemeManager::instance().activeTheme();
    const QString blob = AppSettings::instance()
        .value(recentColorsKeyFor(m_recentColorsTheme)).toString();
    if (blob.isEmpty()) return;
    const QStringList parts = blob.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& s : parts) {
        QColor c(s);
        if (c.isValid()) m_recentColors.append(c);
        if (m_recentColors.size() >= 20) break;
    }
}

void TokenEditorWidget::saveRecentColors() const
{
    QStringList parts;
    parts.reserve(m_recentColors.size());
    for (const QColor& c : m_recentColors) {
        parts << (c.alpha() == 255 ? c.name(QColor::HexRgb)
                                   : c.name(QColor::HexArgb));
    }
    // Always write to the same bucket we last loaded from — guarantees
    // a push() that follows a Save As lands in the new theme's bucket
    // (Save As triggers reloadRecentColorsIfThemeChanged() first, which
    // updates m_recentColorsTheme to the new copy).
    AppSettings::instance().setValue(
        recentColorsKeyFor(m_recentColorsTheme),
        parts.join(QLatin1Char(';')));
}

void TokenEditorWidget::reloadRecentColorsIfThemeChanged()
{
    const QString active = ThemeManager::instance().activeTheme();
    if (active == m_recentColorsTheme) return;
    loadRecentColors();
    refreshRecentButtons();
}

void TokenEditorWidget::pushRecentColor(const QColor& c)
{
    if (!c.isValid()) return;
    // Move-to-front: drop any existing entry with the same ARGB.
    for (int i = m_recentColors.size() - 1; i >= 0; --i) {
        if (m_recentColors[i].rgba() == c.rgba()) m_recentColors.removeAt(i);
    }
    m_recentColors.prepend(c);
    while (m_recentColors.size() > 20) m_recentColors.removeLast();
    saveRecentColors();
    refreshRecentButtons();
}

void TokenEditorWidget::refreshRecentButtons()
{
    static const QSize kSwatchSz(28, 24);
    for (int i = 0; i < m_recentButtons.size(); ++i) {
        QToolButton* b = m_recentButtons[i];
        if (i < m_recentColors.size()) {
            const QColor& c = m_recentColors[i];
            b->setIcon(QIcon(makeRecentSwatchPixmap(c, kSwatchSz)));
            b->setToolTip(colorToTokenHex(c));
            b->setEnabled(true);
        } else {
            // Empty slot — same rounded outline as the populated
            // swatches but filled with a dim background tone.
            const QColor empty(0x1a, 0x22, 0x30);
            b->setIcon(QIcon(makeRecentSwatchPixmap(empty, kSwatchSz)));
            b->setToolTip(QStringLiteral("Empty slot"));
            b->setEnabled(false);
        }
    }
}

void TokenEditorWidget::applyRecentColor(int idx)
{
    if (idx < 0 || idx >= m_recentColors.size()) return;
    const QColor c = m_recentColors[idx];
    if (m_target == TargetScalarColor) {
        m_bufferColor = c;
        QSignalBlocker block(m_colorPicker);
        m_colorPicker->setColor(c);
        markDirty();
    } else if (m_target == TargetGradient) {
        const int si = selectedGradientStopIndex();
        if (si < 0 || si >= m_gradientBuf.stops.size()) return;
        m_gradientBuf.stops[si].color = c;
        QSignalBlocker block(m_colorPicker);
        m_colorPicker->setColor(c);
        syncGradientStripAndList();
        selectGradientStop(si);
        markDirty();
    }
}

// ─────────────────────────────────────────────────────── button row ─────

void TokenEditorWidget::buildButtonRow(QVBoxLayout* root)
{
    Q_UNUSED(root);
    // Action buttons are owned by this widget but placed elsewhere:
    //   Reset  → inspect row (ThemeEditorDialog::resetButton())
    //   Cancel → font row    (buildFontGroup())
    //   OK     → font row    (buildFontGroup())
    // No layout is added here; this function exists only to keep the
    // construction + signal wiring of all three buttons in one place.
    m_resetBtn = new QPushButton(QStringLiteral("Reset to default"), this);
    m_resetBtn->setEnabled(false);
    m_resetBtn->setToolTip(QStringLiteral(
        "Restore this token's controls to its canonical value from the "
        "bundled Default Dark theme.  Buffered locally — click OK to "
        "commit, Cancel to back out."));
    m_cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    m_okBtn     = new QPushButton(QStringLiteral("OK"),     this);
    m_okBtn->setDefault(true);
    m_cancelBtn->setEnabled(false);
    m_okBtn->setEnabled(false);
    m_cancelBtn->setToolTip(QStringLiteral(
        "Discard local edits and reload from the active theme's saved values."));
    m_okBtn->setToolTip(QStringLiteral(
        "Commit the current values to the theme.  The rest of the app "
        "re-themes once when you click this, not on every drag."));

    connect(m_resetBtn,  &QPushButton::clicked, this, &TokenEditorWidget::onResetClicked);
    connect(m_okBtn,     &QPushButton::clicked, this, &TokenEditorWidget::onOkClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &TokenEditorWidget::onCancelClicked);
}

// ────────────────────────────────────────────── enable / disable groups ─

void TokenEditorWidget::applyEnableState()
{
    // Type radios — only meaningful for color tokens.
    const bool colorTok = (m_target == TargetScalarColor
                           || m_target == TargetGradient);
    m_typeLabel->setEnabled(colorTok);
    m_typeFlat->setEnabled(colorTok);
    m_typeGradient->setEnabled(colorTok);

    // Color group — picker is enabled for scalar tokens (binds to buffer),
    // gradient tokens (binds to selected stop), AND font.family.*
    // compound tokens (binds to the compound's `color` field so family +
    // size + color are all edited in one view).  Disabled for plain
    // numeric / nothing-selected.
    const bool colorEnabled =
        m_target == TargetScalarColor ||
        (m_target == TargetGradient && selectedGradientStopIndex() >= 0) ||
        m_target == TargetFontFamily;
    m_colorGroupLabel->setEnabled(m_target == TargetScalarColor
                                  || m_target == TargetGradient
                                  || m_target == TargetFontFamily);
    m_colorPicker->setEnabled(colorEnabled);

    // Font group — the row serves two distinct edits depending on
    // token type:
    //   * font.family.*           → family combo edits family
    //   * font.size.* / sizing.*  → size combo + A↑/A↓ edit the
    //                                 integer; family combo is dim
    const bool fontFamily  = (m_target == TargetFontFamily);
    const bool sizeTok     = (m_target == TargetNumeric);
    const bool fontGroup   = fontFamily || sizeTok;
    m_fontGroupLabel->setEnabled(fontGroup);
    m_fontCombo->setEnabled(fontFamily);
    m_fontSizeCombo->setEnabled(sizeTok || fontFamily);
    m_fontSizeUpBtn->setEnabled(sizeTok || fontFamily);
    m_fontSizeDownBtn->setEnabled(sizeTok || fontFamily);
    // Bold / italic / underline / strike / clear are placeholders — no
    // font.weight / font.style tokens exist yet, so they always stay
    // disabled (the tooltip explains).
    for (QToolButton* b : {m_fontBoldBtn, m_fontItalicBtn,
                           m_fontUnderlineBtn, m_fontStrikeBtn,
                           m_fontClearBtn}) {
        b->setEnabled(false);
    }

    // Gradient group.
    const bool gradEnabled = (m_target == TargetGradient);
    m_gradGroupLabel->setEnabled(gradEnabled);
    m_gradStrip->setEnabled(gradEnabled);
    m_gradStopList->setEnabled(gradEnabled);
    m_gradAddBtn->setEnabled(gradEnabled);
    m_gradDelBtn->setEnabled(gradEnabled);
    m_gradAngleSpin->setEnabled(gradEnabled);

    refreshResetButton();
}

void TokenEditorWidget::refreshResetButton()
{
    if (m_currentToken.isEmpty()) {
        m_resetBtn->setEnabled(false);
        return;
    }
    m_resetBtn->setEnabled(
        ThemeManager::instance().hasFactoryValue(m_currentToken));
}

// ───────────────────────────────────────────────────────── markDirty ─────

void TokenEditorWidget::markDirty()
{
    if (m_settingControlsFromToken) return;
    if (m_currentToken.isEmpty()) return;
    if (m_dirty) return;
    m_dirty = true;
    m_okBtn->setEnabled(true);
    m_cancelBtn->setEnabled(true);
    const QString base = m_header->text();
    if (!base.contains(QStringLiteral("unsaved"))) {
        m_header->setText(base + QStringLiteral(
            "  <span style=\"color:#ffb84d;font-style:italic;font-weight:normal;\">"
            "• unsaved</span>"));
    }
}

// ──────────────────────────────────────────────────── load current token ─

void TokenEditorWidget::loadCurrentTokenIntoBuffers()
{
    auto& tm = ThemeManager::instance();
    const QString& path = m_activeContainerPath;
    m_settingControlsFromToken = true;

    if (m_currentToken.startsWith(QStringLiteral("color."))) {
        // Gradient detection still uses root brush() — gradient tokens
        // are namespace-shaped (color.waterfall.colormap.*) and not yet
        // overridden in nested scopes.  PR step 4 may revisit this.
        const bool isGradient = tm.brush(m_currentToken).gradient() != nullptr;
        if (isGradient) {
            m_target = TargetGradient;
            m_gradientBuf = tm.gradientAt(path, m_currentToken);
            m_bufferColor = m_gradientBuf.stops.isEmpty()
                                ? QColor(255, 255, 255)
                                : m_gradientBuf.stops.first().color;
            QSignalBlocker bAngle(m_gradAngleSpin);
            m_gradAngleSpin->setValue(static_cast<int>(
                std::round(m_gradientBuf.angle)) % 361);
            syncGradientStripAndList();
            QSignalBlocker bRadio(m_typeGroup);
            m_typeGradient->setChecked(true);
        } else {
            m_target = TargetScalarColor;
            m_bufferColor = tm.colorAt(path, m_currentToken);
            QSignalBlocker bRadio(m_typeGroup);
            m_typeFlat->setChecked(true);
            m_gradientBuf = {};
            syncGradientStripAndList();
        }
        QSignalBlocker bPick(m_colorPicker);
        m_colorPicker->setColor(m_bufferColor);
    } else if (m_currentToken.startsWith(QStringLiteral("font.family."))) {
        m_target = TargetFontFamily;
        // Compound: load family + size + color from ThemeFont.  Falls
        // through to legacy bare-string + font.size.normal when the
        // token is still a v1 string (e.g. older imported themes).
        const ThemeFont tf = tm.fontTokenAt(path, m_currentToken);
        m_bufferFontFamily = tf.family;
        QSignalBlocker bCombo(m_fontCombo);
        m_fontCombo->setCurrentFont(QFont(m_bufferFontFamily));
        m_bufferFontSize = tf.size > 0
                              ? tf.size
                              : std::max(1, tm.sizingAt(path, QStringLiteral("font.size.normal")));
        syncFontSizeComboToBuffer(m_bufferFontSize);
        // Surface the embedded colour through the existing CompactColorPicker
        // so the operator can edit it alongside family + size.  Invalid
        // colour (legacy or fresh token) falls back to color.text.primary.
        m_bufferColor = tf.color.isValid()
                            ? tf.color
                            : tm.colorAt(path, QStringLiteral("color.text.primary"));
        QSignalBlocker bPick(m_colorPicker);
        m_colorPicker->setColor(m_bufferColor);
    } else if (m_currentToken.startsWith(QStringLiteral("font.size."))
               || m_currentToken.startsWith(QStringLiteral("sizing."))) {
        m_target = TargetNumeric;
        m_bufferNumeric = tm.sizingAt(path, m_currentToken);
        m_bufferFontSize = std::max(1, m_bufferNumeric);
        syncFontSizeComboToBuffer(m_bufferNumeric);
    } else {
        m_target = TargetNone;
    }

    m_settingControlsFromToken = false;
}

// ─────────────────────────────────────────────────────────── setToken ───

void TokenEditorWidget::setToken(const QString& key)
{
    m_currentToken = key;
    m_dirty = false;
    m_okBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);

    if (key.isEmpty()) {
        m_header->setText(QStringLiteral(
            "<i>Click a token in the list below to edit it here.</i>"));
        m_target = TargetNone;
        // Clear the gradient list so the "Gradient stops" group doesn't
        // show stale rows when nothing is selected.
        m_settingControlsFromToken = true;
        m_gradientBuf = {};
        syncGradientStripAndList();
        m_settingControlsFromToken = false;
        applyEnableState();
        return;
    }

    loadCurrentTokenIntoBuffers();

    QString typeLabel;
    switch (m_target) {
        case TargetScalarColor: typeLabel = QStringLiteral("flat color"); break;
        case TargetGradient:    typeLabel = QStringLiteral("gradient");    break;
        case TargetFontFamily:  typeLabel = QStringLiteral("font family"); break;
        case TargetNumeric:     typeLabel = QStringLiteral("size (px)");   break;
        default:                typeLabel = QStringLiteral("(unknown)");   break;
    }
    // Single-line layout: "Editing:" and the token sit on the same
    // row.  Word-wrap is off so the label keeps its natural width
    // without breaking mid-string.
    // Append the current scope to the header when editing a non-root
    // container, so the operator can see where their commit will land
    // alongside the token name + kind.
    const QString scopeSuffix = m_activeContainerPath.isEmpty()
        ? QString()
        : QStringLiteral(" <span style=\"font-weight:normal;color:#00bcd4;\">"
                         "@ %1</span>").arg(m_activeContainerPath.toHtmlEscaped());
    m_header->setText(QStringLiteral(
        "Editing: %1 <span style=\"font-weight:normal;font-style:italic;\">(%2)</span>%3")
        .arg(key.toHtmlEscaped(), typeLabel, scopeSuffix));
    applyEnableState();
}

void TokenEditorWidget::setActiveContainerPath(const QString& path)
{
    if (path == m_activeContainerPath) return;
    m_activeContainerPath = path;
    // Reload the currently-selected token so the buffer reflects the
    // new scope's resolved value (the user just navigated containers,
    // not tokens — their selection should keep pointing at the same
    // semantic but the controls should now show the new scope's value).
    if (!m_currentToken.isEmpty()) setToken(m_currentToken);
}

// ─────────────────────────────────────────────────────── color picker ───

void TokenEditorWidget::onColorPickerChanged(const QColor& c)
{
    if (m_settingControlsFromToken || m_currentToken.isEmpty()) return;
    if (!c.isValid()) return;
    if (m_target == TargetScalarColor) {
        m_bufferColor = c;
        markDirty();
    } else if (m_target == TargetFontFamily) {
        // Font-compound editing — the colour field of the ThemeFont is
        // edited through the same picker as scalar colour tokens; the
        // commit pulls it from m_bufferColor.
        m_bufferColor = c;
        markDirty();
    } else if (m_target == TargetGradient) {
        const int sel = selectedGradientStopIndex();
        if (sel < 0 || sel >= m_gradientBuf.stops.size()) return;
        m_gradientBuf.stops[sel].color = c;
        // Refresh the strip + the matching list row swatch + label.
        m_gradStrip->setGradient(m_gradientBuf);
        if (auto* item = m_gradStopList->item(sel)) {
            item->setIcon(stopSwatchIcon(c));
            const auto& s = m_gradientBuf.stops[sel];
            item->setText(QStringLiteral("%1  at %2  %3")
                              .arg(sel, 2, 10, QChar('0'))
                              .arg(QString::number(s.at, 'f', 4), -8)
                              .arg(colorToTokenHex(c)));
        }
        markDirty();
    }
}

void TokenEditorWidget::onColorTypeToggled()
{
    if (m_settingControlsFromToken || m_currentToken.isEmpty()) return;
    if (!m_currentToken.startsWith(QStringLiteral("color."))) return;

    if (m_typeGradient->isChecked() && m_target != TargetGradient) {
        // Flat → gradient: seed a 2-stop ramp with the current scalar
        // at both ends so the initial appearance is identical.
        ThemeGradient g;
        g.type  = ThemeGradient::Linear;
        g.angle = 0.0;
        g.stops = { {0.0, m_bufferColor}, {1.0, m_bufferColor} };
        m_settingControlsFromToken = true;
        m_gradientBuf = g;
        m_target = TargetGradient;
        QSignalBlocker bAngle(m_gradAngleSpin);
        m_gradAngleSpin->setValue(0);
        syncGradientStripAndList();
        m_settingControlsFromToken = false;
        applyEnableState();
        markDirty();
    } else if (m_typeFlat->isChecked() && m_target != TargetScalarColor) {
        // Gradient → flat: take the first stop's colour as the seed.
        const QColor seed = m_gradientBuf.stops.isEmpty()
                                ? m_bufferColor
                                : m_gradientBuf.stops.first().color;
        m_settingControlsFromToken = true;
        m_bufferColor = seed;
        m_target = TargetScalarColor;
        m_gradientBuf = {};
        QSignalBlocker bPick(m_colorPicker);
        m_colorPicker->setColor(seed);
        syncGradientStripAndList();
        m_settingControlsFromToken = false;
        applyEnableState();
        markDirty();
    }
}

// ────────────────────────────────────────────────────────── gradient ────

void TokenEditorWidget::syncGradientStripAndList()
{
    m_gradStrip->setGradient(m_gradientBuf);
    rebuildGradientStopList();
}

void TokenEditorWidget::rebuildGradientStopList()
{
    QSignalBlocker block(m_gradStopList);
    m_gradStopList->clear();
    for (int i = 0; i < m_gradientBuf.stops.size(); ++i) {
        const auto& s = m_gradientBuf.stops[i];
        auto* item = new QListWidgetItem(stopSwatchIcon(s.color),
            QStringLiteral("%1  at %2  %3")
                .arg(i, 2, 10, QChar('0'))
                .arg(QString::number(s.at, 'f', 4), -8)
                .arg(colorToTokenHex(s.color)));
        item->setData(Qt::UserRole, i);
        m_gradStopList->addItem(item);
    }
}

void TokenEditorWidget::selectGradientStop(int index)
{
    if (index < 0 || index >= m_gradientBuf.stops.size()) {
        m_gradStopList->clearSelection();
        m_gradStrip->setSelectedStop(-1);
        return;
    }
    QSignalBlocker block(m_gradStopList);
    m_gradStopList->setCurrentRow(index);
    m_gradStrip->setSelectedStop(index);
}

int TokenEditorWidget::selectedGradientStopIndex() const
{
    const auto sel = m_gradStopList->selectedItems();
    if (sel.isEmpty()) return -1;
    return sel.first()->data(Qt::UserRole).toInt();
}

void TokenEditorWidget::sortGradientStopsByAt()
{
    const int prevSel = selectedGradientStopIndex();
    QColor prevColor;
    qreal  prevAt = -1.0;
    if (prevSel >= 0 && prevSel < m_gradientBuf.stops.size()) {
        prevColor = m_gradientBuf.stops[prevSel].color;
        prevAt    = m_gradientBuf.stops[prevSel].at;
    }
    std::sort(m_gradientBuf.stops.begin(), m_gradientBuf.stops.end(),
              [](const ThemeGradientStop& a, const ThemeGradientStop& b) {
                  return a.at < b.at;
              });
    syncGradientStripAndList();
    if (prevAt >= 0.0) {
        for (int i = 0; i < m_gradientBuf.stops.size(); ++i) {
            if (qFuzzyCompare(m_gradientBuf.stops[i].at, prevAt) &&
                m_gradientBuf.stops[i].color == prevColor) {
                selectGradientStop(i);
                return;
            }
        }
    }
}

void TokenEditorWidget::onGradientStopMoved(int index, qreal newAt)
{
    if (m_settingControlsFromToken) return;
    if (index < 0 || index >= m_gradientBuf.stops.size()) return;
    m_gradientBuf.stops[index].at = newAt;
    m_gradStrip->setGradient(m_gradientBuf);
    if (auto* item = m_gradStopList->item(index)) {
        const auto& s = m_gradientBuf.stops[index];
        item->setText(QStringLiteral("%1  at %2  %3")
                          .arg(index, 2, 10, QChar('0'))
                          .arg(QString::number(s.at, 'f', 4), -8)
                          .arg(colorToTokenHex(s.color)));
    }
    markDirty();
}

void TokenEditorWidget::onGradientStopActivated(int index)
{
    selectGradientStop(index);
    applyEnableState();
}

void TokenEditorWidget::onGradientRequestNewStop(qreal at)
{
    if (m_settingControlsFromToken) return;
    ThemeGradientStop s;
    s.at = at;
    if (m_gradientBuf.stops.isEmpty()) {
        s.color = QColor(0xff, 0xff, 0xff);
    } else if (m_gradientBuf.stops.size() == 1) {
        s.color = m_gradientBuf.stops.first().color;
    } else {
        ThemeGradient sorted = m_gradientBuf;
        std::sort(sorted.stops.begin(), sorted.stops.end(),
                  [](const auto& a, const auto& b) { return a.at < b.at; });
        const auto& first = sorted.stops.first();
        const auto& last  = sorted.stops.last();
        if (at <= first.at)      s.color = first.color;
        else if (at >= last.at)  s.color = last.color;
        else {
            for (int i = 1; i < sorted.stops.size(); ++i) {
                const auto& a = sorted.stops[i - 1];
                const auto& b = sorted.stops[i];
                if (at >= a.at && at <= b.at) {
                    const qreal span = std::max(1e-9, b.at - a.at);
                    const qreal t = (at - a.at) / span;
                    auto blend = [t](int x, int y) {
                        return static_cast<int>(std::round((1.0 - t) * x + t * y));
                    };
                    s.color = QColor(blend(a.color.red(),   b.color.red()),
                                     blend(a.color.green(), b.color.green()),
                                     blend(a.color.blue(),  b.color.blue()),
                                     blend(a.color.alpha(), b.color.alpha()));
                    break;
                }
            }
        }
    }
    m_gradientBuf.stops.append(s);
    sortGradientStopsByAt();
    for (int i = 0; i < m_gradientBuf.stops.size(); ++i) {
        if (qFuzzyCompare(m_gradientBuf.stops[i].at, s.at) &&
            m_gradientBuf.stops[i].color == s.color) {
            selectGradientStop(i);
            break;
        }
    }
    applyEnableState();
    markDirty();
}

void TokenEditorWidget::onGradientRequestDeleteStop(int index)
{
    if (m_settingControlsFromToken) return;
    if (index < 0 || index >= m_gradientBuf.stops.size()) return;
    if (m_gradientBuf.stops.size() <= 2) {
        QMessageBox::information(this, QStringLiteral("Cannot delete stop"),
            QStringLiteral("A gradient needs at least two stops."));
        return;
    }
    m_gradientBuf.stops.removeAt(index);
    syncGradientStripAndList();
    selectGradientStop(std::min(index,
                                static_cast<int>(m_gradientBuf.stops.size()) - 1));
    applyEnableState();
    markDirty();
}

void TokenEditorWidget::onGradientStopListSelectionChanged()
{
    const int sel = selectedGradientStopIndex();
    m_gradStrip->setSelectedStop(sel);
    // When a stop is now selected, push its colour into the picker so
    // the operator can edit it.  The picker's enable state is gated on
    // having a selection.
    if (m_target == TargetGradient) {
        m_settingControlsFromToken = true;
        if (sel >= 0 && sel < m_gradientBuf.stops.size()) {
            QSignalBlocker block(m_colorPicker);
            m_colorPicker->setColor(m_gradientBuf.stops[sel].color);
        }
        m_settingControlsFromToken = false;
        applyEnableState();
    }
}

void TokenEditorWidget::onGradientAngleChanged(int deg)
{
    if (m_settingControlsFromToken) return;
    m_gradientBuf.angle = static_cast<qreal>(deg);
    markDirty();
}

void TokenEditorWidget::onGradientAddStop()
{
    qreal at = 0.5;
    const int sel = selectedGradientStopIndex();
    if (sel >= 0 && sel < m_gradientBuf.stops.size() - 1) {
        at = (m_gradientBuf.stops[sel].at + m_gradientBuf.stops[sel + 1].at) / 2.0;
    } else if (sel == m_gradientBuf.stops.size() - 1 && sel > 0) {
        at = (m_gradientBuf.stops[sel - 1].at + m_gradientBuf.stops[sel].at) / 2.0;
    }
    onGradientRequestNewStop(at);
}

void TokenEditorWidget::onGradientDeleteStop()
{
    onGradientRequestDeleteStop(selectedGradientStopIndex());
}

// ────────────────────────────────────────────────────────── font ────────

void TokenEditorWidget::onFontFamilyChanged(const QString& family)
{
    if (m_settingControlsFromToken || m_currentToken.isEmpty()) return;
    if (m_target != TargetFontFamily) return;
    if (family.isEmpty()) return;
    m_bufferFontFamily = family;
    markDirty();
}

void TokenEditorWidget::onFontSizeChanged(int v)
{
    if (m_settingControlsFromToken || m_currentToken.isEmpty()) return;
    if (v <= 0) return;
    m_bufferFontSize = v;
    if (m_target == TargetNumeric) {
        // font.size.* / sizing.* token — the size combo IS the editor.
        m_bufferNumeric = v;
        markDirty();
    } else if (m_target == TargetFontFamily) {
        // font.family.* is now a compound (family + size + color), so
        // the size combo IS part of what we commit.  Mark dirty so the
        // OK button activates and commitBufferToThemeManager writes
        // the new ThemeFont with the picked size.
        markDirty();
    }
}

void TokenEditorWidget::onFontSizeBumpUp()
{
    // font.size.*/sizing.* scalar tokens stay capped at 20 — overflowing
    // those breaks status-row labels.  font.family.* compounds can go
    // larger (the freq compound is 26 by default, segment displays
    // commonly run to 36-48).
    const int hi = (m_target == TargetFontFamily) ? 96 : 20;
    m_bufferFontSize = std::clamp(m_bufferFontSize + 1, 7, hi);
    syncFontSizeComboToBuffer(m_bufferFontSize);
    if (m_target == TargetNumeric || m_target == TargetFontFamily) {
        if (m_target == TargetNumeric) m_bufferNumeric = m_bufferFontSize;
        markDirty();
    }
}

void TokenEditorWidget::onFontSizeBumpDown()
{
    const int hi = (m_target == TargetFontFamily) ? 96 : 20;
    m_bufferFontSize = std::clamp(m_bufferFontSize - 1, 7, hi);
    syncFontSizeComboToBuffer(m_bufferFontSize);
    if (m_target == TargetNumeric || m_target == TargetFontFamily) {
        if (m_target == TargetNumeric) m_bufferNumeric = m_bufferFontSize;
        markDirty();
    }
}

void TokenEditorWidget::syncFontSizeComboToBuffer(int sz)
{
    QSignalBlocker block(m_fontSizeCombo);
    int idx = -1;
    for (int i = 0; i < m_fontSizeCombo->count(); ++i) {
        if (m_fontSizeCombo->itemData(i).toInt() == sz) { idx = i; break; }
    }
    if (idx < 0) {
        // Insert in sorted order so the dropdown stays low→high.
        int insertAt = m_fontSizeCombo->count();
        for (int i = 0; i < m_fontSizeCombo->count(); ++i) {
            if (m_fontSizeCombo->itemData(i).toInt() > sz) { insertAt = i; break; }
        }
        m_fontSizeCombo->insertItem(insertAt,
                                    QStringLiteral("%1 pt").arg(sz),
                                    QVariant(sz));
        idx = insertAt;
    }
    m_fontSizeCombo->setCurrentIndex(idx);
}

// ────────────────────────────────────────────────────────── numeric ─────

void TokenEditorWidget::onNumericValueChanged(int /*v*/)
{
    // Stub — kept so any leftover signal connections compile.  The
    // size combo + A↑/A↓ handle numeric editing through the font row.
}

// ──────────────────────────────────────────────────────── reset / OK ────

void TokenEditorWidget::onResetClicked()
{
    if (m_currentToken.isEmpty()) return;
    auto& tm = ThemeManager::instance();
    if (!tm.hasFactoryValue(m_currentToken)) return;

    m_settingControlsFromToken = true;
    if (m_target == TargetScalarColor) {
        const QColor c = tm.factoryColor(m_currentToken);
        if (c.isValid()) {
            m_bufferColor = c;
            QSignalBlocker block(m_colorPicker);
            m_colorPicker->setColor(c);
        }
    } else if (m_target == TargetGradient) {
        const ThemeGradient g = tm.factoryGradient(m_currentToken);
        if (!g.stops.isEmpty()) {
            m_gradientBuf = g;
            QSignalBlocker bAngle(m_gradAngleSpin);
            m_gradAngleSpin->setValue(static_cast<int>(
                std::round(g.angle)) % 361);
            syncGradientStripAndList();
            if (!m_gradientBuf.stops.isEmpty()) selectGradientStop(0);
        }
    } else if (m_target == TargetFontFamily) {
        const QString f = tm.factoryString(m_currentToken);
        if (!f.isEmpty()) {
            m_bufferFontFamily = f;
            QSignalBlocker block(m_fontCombo);
            m_fontCombo->setCurrentFont(QFont(f));
        }
    } else if (m_target == TargetNumeric) {
        const int v = tm.factorySizing(m_currentToken);
        if (v >= 0) {
            m_bufferNumeric  = v;
            m_bufferFontSize = std::max(1, v);
            syncFontSizeComboToBuffer(v);
        }
    }
    m_settingControlsFromToken = false;
    applyEnableState();
    markDirty();
}

void TokenEditorWidget::commitBufferToThemeManager()
{
    if (m_currentToken.isEmpty()) return;
    auto& tm = ThemeManager::instance();
    const QString& path = m_activeContainerPath;
    // The scope-aware setters short-circuit to the bare-token (root)
    // overload when `path` is empty, so the historical flat-namespace
    // behaviour is preserved for the default selection.
    switch (m_target) {
        case TargetScalarColor:
            tm.setColor(path, m_currentToken, m_bufferColor);
            pushRecentColor(m_bufferColor);
            break;
        case TargetGradient:
            tm.setGradient(path, m_currentToken, m_gradientBuf);
            if (const int si = selectedGradientStopIndex();
                si >= 0 && si < m_gradientBuf.stops.size()) {
                pushRecentColor(m_gradientBuf.stops[si].color);
            }
            break;
        case TargetFontFamily: {
            // Compound write — family + size + color all land in the
            // same ThemeFont so the on-disk shape matches the editor's
            // grouped UI.
            ThemeFont tf;
            tf.family = m_bufferFontFamily;
            tf.size   = m_bufferFontSize;
            tf.color  = m_bufferColor;
            tm.setFontToken(path, m_currentToken, tf);
            if (m_bufferColor.isValid()) pushRecentColor(m_bufferColor);
            break;
        }
        case TargetNumeric:     tm.setSizing(path, m_currentToken, m_bufferNumeric); break;
        default: break;
    }
}

void TokenEditorWidget::onOkClicked()
{
    if (m_currentToken.isEmpty() || !m_dirty) return;

    // Built-in bundled themes ("Default Dark" / "Default Light") are
    // read-only — overwriting them would silently shadow the bundled
    // copy from the user dir.  Defer the commit, hand the dialog a
    // chance to run Save As, and let it call back into
    // completeDeferredCommit() once the active theme is a user copy.
    auto& tm = ThemeManager::instance();
    if (tm.isBuiltInTheme(tm.activeTheme())) {
        m_deferredEdit = DeferredEdit{
            m_currentToken, m_target, m_bufferColor, m_gradientBuf,
            m_bufferFontFamily, m_bufferNumeric, m_bufferFontSize, true};
        emit requestSaveAsBeforeCommit();
        return;
    }

    commitBufferToThemeManager();
    emit tokenChanged(m_currentToken);
    setToken(m_currentToken);
}

void TokenEditorWidget::completeDeferredCommit()
{
    if (!m_deferredEdit.pending) return;
    // Restore the buffer snapshot — Save As's themeChanged signal ran
    // refreshTokenList() which called setToken("") and wiped the live
    // buffer.  After this, the active theme is a fresh user copy with
    // the original token values, so commitBufferToThemeManager() will
    // write our edits to that copy.
    m_currentToken    = m_deferredEdit.token;
    m_target          = m_deferredEdit.target;
    m_bufferColor     = m_deferredEdit.color;
    m_gradientBuf     = m_deferredEdit.gradient;
    m_bufferFontFamily = m_deferredEdit.fontFamily;
    m_bufferNumeric   = m_deferredEdit.numeric;
    m_bufferFontSize  = m_deferredEdit.fontSize;
    m_deferredEdit    = {};
    commitBufferToThemeManager();
    emit tokenChanged(m_currentToken);
    setToken(m_currentToken);
}

void TokenEditorWidget::onCancelClicked()
{
    if (m_currentToken.isEmpty()) return;
    setToken(m_currentToken);
}

} // namespace AetherSDR
