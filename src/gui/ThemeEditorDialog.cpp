#include "ThemeEditorDialog.h"
#include "GradientEditorDialog.h"
#include "Theme.h"
#include "ThemeInspector.h"
#include "core/ThemeManager.h"

#include <QAction>
#include <QColorDialog>
#include <QCursor>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMenu>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>

namespace AetherSDR {

namespace {
// Build a tiny coloured square QIcon for use as a swatch on a list row.
// Re-rendered whenever a token's value changes so the swatch tracks the
// live value.  A 2x2 checkerboard sits under the colour fill so the
// operator can see translucency at a glance — opaque colours render
// identically to a solid swatch (the checkerboard is fully covered).
QIcon swatchIcon(const QColor& c)
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

// Token hex serialisation matching ThemeManager's storage convention:
// "#rrggbb" for opaque, "#aarrggbb" for translucent.
QString colorToTokenHex(const QColor& c)
{
    return c.alpha() == 255 ? c.name(QColor::HexRgb)
                            : c.name(QColor::HexArgb);
}

// Mini left-to-right preview of a gradient for the token list row.
// Stops are sampled in declaration order without sorting — matches the
// strip in GradientEditorDialog.
QIcon gradientSwatchIcon(const ThemeGradient& g)
{
    QPixmap pm(32, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath clip;
    clip.addRoundedRect(QRectF(0.5, 0.5, 31.0, 15.0), 3, 3);
    p.save();
    p.setClipPath(clip);
    p.fillRect(QRect(0, 0, 32, 16), QColor(0xc8, 0xc8, 0xc8));
    for (int x = 0; x < 32; x += 8)
        for (int y = 0; y < 16; y += 8)
            if (((x / 8 + y / 8) & 1) == 0)
                p.fillRect(QRect(x, y, 8, 8), QColor(0x80, 0x80, 0x80));

    QLinearGradient lg(QPointF(0.5, 0), QPointF(31.5, 0));
    for (const auto& s : g.stops) lg.setColorAt(std::clamp(s.at, 0.0, 1.0), s.color);
    p.fillRect(QRect(0, 0, 32, 16), QBrush(lg));
    p.restore();

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, 31.0, 15.0), 3, 3);
    return QIcon(pm);
}

// Build the short text shown on a gradient row: "N stops, Xdeg" so the
// list stays scannable without having to open every gradient.
QString gradientRowText(const QString& key, const ThemeGradient& g)
{
    const QString kind = g.type == ThemeGradient::Radial
                             ? QStringLiteral("radial")
                             : QStringLiteral("linear, %1°").arg(
                                   static_cast<int>(std::round(g.angle)));
    return QStringLiteral("%1   %2, %3 stops")
        .arg(key, -36)
        .arg(kind)
        .arg(g.stops.size());
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(QWidget* parent)
    : PersistentDialog(QStringLiteral("Theme Editor"),
                       QStringLiteral("ThemeEditorDialogGeometry"),
                       parent)
{
    // PersistentDialog owns the outer (FramelessWindowTitleBar + bodyWidget)
    // layout and the geometry persistence; we just need to populate
    // bodyWidget() with the actual editor controls.  Subclasses are
    // expected to be non-modal — MainWindow's showOrRaisePersistent
    // wires that up + the frameless chrome from AppSettings.
    setMinimumSize(420, 560);
    applyAppTheme(this);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_themeLabel = new QLabel(bodyWidget());
    m_themeLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-weight: bold; }"));
    root->addWidget(m_themeLabel);

    m_filterEdit = new QLineEdit(bodyWidget());
    m_filterEdit->setPlaceholderText(QStringLiteral("Filter tokens (e.g. accent, slice, meter)…"));
    root->addWidget(m_filterEdit);

    m_tokenList = new QListWidget(bodyWidget());
    m_tokenList->setIconSize(QSize(18, 18));
    m_tokenList->setAlternatingRowColors(true);
    m_tokenList->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_tokenList, 1);

    // Inspector toggle row — sits above the filter so it reads as a mode
    // switch ("am I clicking on the main UI to pick a token?") rather than
    // a button buried with Save As / Close.
    auto* inspectRow = new QHBoxLayout;
    m_inspectBtn = new QPushButton(QStringLiteral("🎯  Inspect"), bodyWidget());
    m_inspectBtn->setCheckable(true);
    m_inspectBtn->setToolTip(QStringLiteral(
        "Click, then point at any region of the main UI to find the\n"
        "token(s) painting it.  ESC cancels."));
    inspectRow->addWidget(m_inspectBtn);
    m_inspectStatus = new QLabel(bodyWidget());
    m_inspectStatus->setWordWrap(true);
    m_inspectStatus->setStyleSheet(QStringLiteral(
        "QLabel { font-style: italic; }"));
    inspectRow->addWidget(m_inspectStatus, 1);
    root->addLayout(inspectRow);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_saveAsBtn = new QPushButton(QStringLiteral("Save As…"), bodyWidget());
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), bodyWidget());
    btnRow->addWidget(m_saveAsBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    refreshTokenList();
    updateTitle();
    m_lastRenderedTheme = ThemeManager::instance().activeTheme();

    connect(m_tokenList, &QListWidget::itemClicked,
            this, &ThemeEditorDialog::onTokenRowClicked);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        // Re-evaluate visibility against both the text filter and the
        // last inspector-picked subset (if any) — filterTokensTo() reads
        // the QLineEdit's current text directly.
        filterTokensTo(m_activeSubset);
    });
    connect(m_saveAsBtn, &QPushButton::clicked,
            this, &ThemeEditorDialog::onSaveAsClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    // Active-theme switches outside the editor (e.g. via View → Theme)
    // should refresh both the title and the swatch values so the editor
    // doesn't lie about what it's editing.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ThemeEditorDialog::onActiveThemeChanged);

    // Inspector wiring.  The inspector is owned by the dialog (parent),
    // so it dies when the dialog does and removes its event filter
    // cleanly.
    m_inspector = new ThemeInspector(this, this);
    connect(m_inspectBtn, &QPushButton::toggled,
            this, &ThemeEditorDialog::onInspectToggled);
    connect(m_inspector, &ThemeInspector::widgetPicked,
            this, &ThemeEditorDialog::onInspectorPicked);
    connect(m_inspector, &ThemeInspector::activeChanged,
            this, &ThemeEditorDialog::onInspectorActiveChanged);
    connect(m_inspector, &ThemeInspector::canceled, this, [this]() {
        updateInspectorStatus(QStringLiteral("Inspector canceled."));
    });
}

void ThemeEditorDialog::refreshTokenList()
{
    m_tokenList->clear();
    auto& tm = ThemeManager::instance();
    for (const QString& key : tm.allTokenKeys()) {
        // Color + gradient tokens are both editable through this list;
        // sizing + font get their own editing surfaces in a follow-on PR.
        if (!key.startsWith(QStringLiteral("color.")))
            continue;
        const QBrush br = tm.brush(key);
        if (br.gradient()) {
            // Gradient row — opens GradientEditorDialog on click.
            const ThemeGradient g = tm.gradient(key);
            auto* item = new QListWidgetItem(gradientSwatchIcon(g),
                gradientRowText(key, g));
            item->setData(Qt::UserRole, key);
            m_tokenList->addItem(item);
            continue;
        }

        const QColor c = tm.color(key);
        auto* item = new QListWidgetItem(swatchIcon(c),
            QStringLiteral("%1   %2").arg(key, -36).arg(colorToTokenHex(c)));
        item->setData(Qt::UserRole, key);
        m_tokenList->addItem(item);
    }
}

void ThemeEditorDialog::updateRow(QListWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    auto& tm = ThemeManager::instance();
    if (tm.brush(key).gradient()) {
        const ThemeGradient g = tm.gradient(key);
        item->setIcon(gradientSwatchIcon(g));
        item->setText(gradientRowText(key, g));
        return;
    }
    const QColor c = tm.color(key);
    item->setIcon(swatchIcon(c));
    item->setText(QStringLiteral("%1   %2").arg(key, -36).arg(colorToTokenHex(c)));
}

void ThemeEditorDialog::updateTitle()
{
    const QString name = ThemeManager::instance().activeTheme();
    m_themeLabel->setText(QStringLiteral("Editing: %1").arg(
        name.isEmpty() ? QStringLiteral("(no active theme)") : name));
    setWindowTitle(QStringLiteral("Theme Editor — %1").arg(name));
}

void ThemeEditorDialog::onTokenRowClicked(QListWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    auto& tm = ThemeManager::instance();

    // Every color token can be either a flat colour or a gradient.  Show
    // a small chooser menu at the cursor so the operator can decide each
    // time — the "current" type stays as the first action, and the
    // other appears as a "Convert to…" follow-up so an unintentional
    // type-switch needs an explicit click.
    const bool isGradient = tm.brush(key).gradient() != nullptr;

    QMenu menu(this);
    QAction* flatAct = nullptr;
    QAction* gradAct = nullptr;
    if (isGradient) {
        gradAct = menu.addAction(QStringLiteral("Edit gradient…"));
        menu.addSeparator();
        flatAct = menu.addAction(QStringLiteral("Convert to flat colour…"));
    } else {
        flatAct = menu.addAction(QStringLiteral("Edit flat colour…"));
        menu.addSeparator();
        gradAct = menu.addAction(QStringLiteral("Convert to gradient…"));
    }
    QAction* chosen = menu.exec(QCursor::pos());
    if (chosen == flatAct)      editTokenAsFlat(key, item);
    else if (chosen == gradAct) editTokenAsGradient(key, item);
    // else: dismissed without picking — no-op.
}

void ThemeEditorDialog::editTokenAsFlat(const QString& key, QListWidgetItem* item)
{
    auto& tm = ThemeManager::instance();
    // ThemeManager::color() returns the first-stop colour when the token
    // is currently a gradient — exactly the right seed for "I want this
    // flat now, starting from the dominant gradient colour."
    const QColor current = tm.color(key);
    const QColor chosen = QColorDialog::getColor(current, this,
        QStringLiteral("Edit %1").arg(key),
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    tm.setColor(key, chosen);   // overwrites any previous gradient entry
    updateRow(item);
}

void ThemeEditorDialog::editTokenAsGradient(const QString& key, QListWidgetItem* item)
{
    auto& tm = ThemeManager::instance();
    ThemeGradient before = tm.gradient(key);
    if (before.stops.isEmpty()) {
        // Token is currently flat — seed a 2-stop linear gradient with
        // the scalar colour at both ends.  The initial render matches
        // the previous flat output exactly, so opening the editor
        // doesn't perturb anything until the operator makes a deliberate
        // edit.  Angle 0 (bottom→top) matches the canonical convention
        // used by the meter and waterfall gradients.
        const QColor c = tm.color(key);
        before.type   = ThemeGradient::Linear;
        before.angle  = 0.0;
        before.stops  = { {0.0, c}, {1.0, c} };
    }
    GradientEditorDialog dlg(key, before, this);
    if (dlg.exec() == QDialog::Accepted) {
        tm.setGradient(key, dlg.currentGradient());  // overwrites any prior scalar
        updateRow(item);
    }
}

void ThemeEditorDialog::onSaveAsClicked()
{
    bool ok = false;
    auto& tm = ThemeManager::instance();
    QString suggestion = tm.activeTheme();
    if (!suggestion.startsWith(QStringLiteral("My ")))
        suggestion = QStringLiteral("My %1").arg(suggestion);

    const QString name = QInputDialog::getText(this,
        QStringLiteral("Save Theme As"),
        QStringLiteral("Theme name:"),
        QLineEdit::Normal, suggestion, &ok).trimmed();
    if (!ok || name.isEmpty()) return;

    // Disallow overwriting a built-in (bundled :/themes/*.json) — that
    // would silently shadow it from the user dir which is confusing.
    // User-dir themes can be overwritten freely.
    const QStringList existing = tm.availableThemes();
    if (existing.contains(name)) {
        const auto reply = QMessageBox::question(this,
            QStringLiteral("Theme exists"),
            QStringLiteral("A theme named \"%1\" already exists. Overwrite it?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    if (!tm.saveCurrentThemeAs(name)) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
            QStringLiteral("Could not write the theme file. Check that "
                           "~/.config/AetherSDR/themes/ is writable."));
        return;
    }
    // saveCurrentThemeAs() makes the new theme active; onActiveThemeChanged
    // will fire from themeChanged and refresh the title.
}

void ThemeEditorDialog::onInspectToggled(bool on)
{
    if (!m_inspector) return;
    if (on) {
        m_activeSubset.clear();
        filterTokensTo({});                       // clear any prior subset
        m_filterEdit->clear();
        updateInspectorStatus(QStringLiteral(
            "Move the cursor over the main UI and click a region.  "
            "ESC cancels."));
        m_inspector->start();
    } else {
        m_inspector->stop();
    }
}

void ThemeEditorDialog::onInspectorActiveChanged(bool active)
{
    // Keep the toggle button's checked state in lock-step with the
    // inspector — covers self-deactivation paths (post-pick, ESC).
    if (m_inspectBtn && m_inspectBtn->isChecked() != active) {
        QSignalBlocker sb(m_inspectBtn);
        m_inspectBtn->setChecked(active);
    }
}

void ThemeEditorDialog::onInspectorPicked(QWidget* target, QPoint localPos)
{
    if (!target) return;
    auto& tm = ThemeManager::instance();

    // Walk up the parent chain until we find a widget with a declared
    // token list — that's how a click on a deep child (e.g. the QLabel
    // inside a QPushButton inside a themed panel) still surfaces the
    // panel-level tokens.  Use tokensAtPoint() instead of plain
    // tokensForWidget() so custom-paint widgets that declared sub-regions
    // (panadapter trace vs waterfall vs slice triangles) get the narrowed
    // hit-list rather than every token they paint with.  The point is
    // mapped from the original target's coordinate space into each
    // ancestor's space as we walk.
    QStringList tokens;
    QWidget* w = target;
    QString hitName = target->metaObject()->className();
    QPoint scanPos = localPos;
    while (w && tokens.isEmpty()) {
        tokens = tm.tokensAtPoint(w, scanPos);
        if (!tokens.isEmpty()) break;
        QWidget* parent = w->parentWidget();
        if (!parent) break;
        scanPos = parent->mapFromGlobal(w->mapToGlobal(scanPos));
        w = parent;
    }

    if (tokens.isEmpty()) {
        updateInspectorStatus(QStringLiteral(
            "%1: no theme tokens registered for this region yet.  Use the "
            "filter or browse the full token list below.").arg(hitName));
        m_activeSubset.clear();
        filterTokensTo({});
        return;
    }

    m_activeSubset = tokens;
    filterTokensTo(tokens);

    const QString matchedClass = w ? w->metaObject()->className() : hitName;
    if (w && w != target) {
        updateInspectorStatus(QStringLiteral(
            "%1 (via %2): %3 token%4.").arg(matchedClass).arg(hitName)
            .arg(tokens.size()).arg(tokens.size() == 1 ? "" : "s"));
    } else {
        updateInspectorStatus(QStringLiteral(
            "%1: %2 token%3.").arg(matchedClass)
            .arg(tokens.size()).arg(tokens.size() == 1 ? "" : "s"));
    }

    // Convenience: if exactly one color token matched, open the picker
    // straight away — that's the "click ugly thing → fix it" flow.
    if (tokens.size() == 1) {
        for (int i = 0; i < m_tokenList->count(); ++i) {
            auto* item = m_tokenList->item(i);
            if (item->isHidden()) continue;
            if (item->data(Qt::UserRole).toString() == tokens.first()) {
                m_tokenList->setCurrentItem(item);
                onTokenRowClicked(item);
                break;
            }
        }
    }
}

void ThemeEditorDialog::updateInspectorStatus(const QString& text)
{
    if (m_inspectStatus) m_inspectStatus->setText(text);
}

void ThemeEditorDialog::filterTokensTo(const QStringList& subset)
{
    // Empty subset → unhide everything that the text filter doesn't
    // already exclude (i.e. fall back to the QLineEdit's current
    // textChanged behaviour).
    const QString needle = m_filterEdit
                               ? m_filterEdit->text().trimmed().toLower()
                               : QString();
    const QSet<QString> wanted(subset.begin(), subset.end());
    for (int i = 0; i < m_tokenList->count(); ++i) {
        auto* item = m_tokenList->item(i);
        const QString key = item->data(Qt::UserRole).toString();
        bool hidden = false;
        if (!subset.isEmpty() && !wanted.contains(key)) hidden = true;
        if (!hidden && !needle.isEmpty() && !key.contains(needle)) hidden = true;
        item->setHidden(hidden);
    }
}

void ThemeEditorDialog::onActiveThemeChanged()
{
    // themeChanged fires for BOTH "user switched active theme" (View →
    // Theme menu, Save As) AND "user edited one token in-place"
    // (setColor / setSizing).  We must only rebuild the row list for
    // the first case; for the second, the caller in onTokenRowClicked
    // is still holding a pointer to the row it clicked and will run
    // updateRow() on it — destroying the list mid-edit causes a SEGV
    // in QPixmapIconEngine when the now-dangling row tries to repaint.
    //
    // Distinguish the two by comparing active-theme names.
    const QString current = ThemeManager::instance().activeTheme();
    updateTitle();
    if (current != m_lastRenderedTheme) {
        m_lastRenderedTheme = current;
        refreshTokenList();
    }
}

} // namespace AetherSDR
