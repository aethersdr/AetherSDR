#include "ThemeEditorDialog.h"
#include "GradientEditorDialog.h"
#include "Theme.h"
#include "ThemeInspector.h"
#include "core/ThemeManager.h"

#include <QAction>
#include <QColorDialog>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFontMetrics>
#include <QMimeData>
#include <QUrl>
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
    // Theme-management dropdown — left side, since it's a destructive
    // group that shouldn't be confused with the Save / Close primary
    // actions on the right.  Built-in themes can't be renamed or
    // deleted; actions are disabled at click time when the active
    // theme is built-in.
    // No trailing ▾ in the label — QPushButton auto-renders a dropdown
    // arrow once setMenu() is called below, and the two collide visually.
    auto* themeActionsBtn = new QPushButton(QStringLiteral("Theme actions"), bodyWidget());
    auto* themeActionsMenu = new QMenu(themeActionsBtn);
    auto* renameAct = themeActionsMenu->addAction(QStringLiteral("Rename theme…"));
    auto* deleteAct = themeActionsMenu->addAction(QStringLiteral("Delete theme…"));
    themeActionsMenu->addSeparator();
    auto* exportAct = themeActionsMenu->addAction(QStringLiteral("Export to file…"));
    auto* importAct = themeActionsMenu->addAction(QStringLiteral("Import from file…"));
    themeActionsBtn->setMenu(themeActionsMenu);
    btnRow->addWidget(themeActionsBtn);

    btnRow->addStretch(1);
    m_saveAsBtn = new QPushButton(QStringLiteral("Save As…"), bodyWidget());
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), bodyWidget());
    btnRow->addWidget(m_saveAsBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(renameAct, &QAction::triggered,
            this, &ThemeEditorDialog::onRenameThemeClicked);
    connect(deleteAct, &QAction::triggered,
            this, &ThemeEditorDialog::onDeleteThemeClicked);
    connect(exportAct, &QAction::triggered,
            this, &ThemeEditorDialog::onExportThemeClicked);
    connect(importAct, &QAction::triggered,
            this, &ThemeEditorDialog::onImportThemeClicked);

    // Accept .aethertheme / .json drag-and-drop on the dialog body — the
    // PersistentDialog base widget gets the drop events as long as the
    // outermost dialog has setAcceptDrops(true).
    setAcceptDrops(true);

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
        // Editable token namespaces — gradient + scalar colours under
        // "color.", string + integer leaves under "font." and "sizing.".
        if (!key.startsWith(QStringLiteral("color."))
            && !key.startsWith(QStringLiteral("font."))
            && !key.startsWith(QStringLiteral("sizing."))) {
            continue;
        }
        auto* item = new QListWidgetItem;
        item->setData(Qt::UserRole, key);
        m_tokenList->addItem(item);
        populateRow(item);
    }
}

void ThemeEditorDialog::populateRow(QListWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    auto& tm = ThemeManager::instance();

    if (key.startsWith(QStringLiteral("color."))) {
        if (tm.brush(key).gradient()) {
            const ThemeGradient g = tm.gradient(key);
            item->setIcon(gradientSwatchIcon(g));
            item->setText(gradientRowText(key, g));
            return;
        }
        const QColor c = tm.color(key);
        item->setIcon(swatchIcon(c));
        item->setText(QStringLiteral("%1   %2").arg(key, -36).arg(colorToTokenHex(c)));
        return;
    }
    if (key.startsWith(QStringLiteral("font.family."))) {
        const QString family = tm.value(key);
        item->setIcon(QIcon());
        item->setText(QStringLiteral("%1   %2").arg(key, -36).arg(family));
        return;
    }
    if (key.startsWith(QStringLiteral("font.size."))
        || key.startsWith(QStringLiteral("sizing."))) {
        const int v = tm.sizing(key);
        item->setIcon(QIcon());
        item->setText(QStringLiteral("%1   %2 px").arg(key, -36).arg(v));
        return;
    }
}

void ThemeEditorDialog::updateRow(QListWidgetItem* item)
{
    populateRow(item);
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

    // Click-menu adapts to the token's namespace + current type:
    //   * color.*   — flat ↔ gradient chooser
    //   * font.family.* — font family picker
    //   * font.size.* / sizing.* — numeric picker
    // A "Reset to default" entry shows up at the bottom whenever the
    // token has a factory baseline (every bundled token does).
    QMenu menu(this);
    QAction* flatAct   = nullptr;
    QAction* gradAct   = nullptr;
    QAction* familyAct = nullptr;
    QAction* numAct    = nullptr;

    if (key.startsWith(QStringLiteral("color."))) {
        const bool isGradient = tm.brush(key).gradient() != nullptr;
        if (isGradient) {
            gradAct = menu.addAction(QStringLiteral("Edit gradient…"));
            menu.addSeparator();
            flatAct = menu.addAction(QStringLiteral("Convert to flat colour…"));
        } else {
            flatAct = menu.addAction(QStringLiteral("Edit flat colour…"));
            menu.addSeparator();
            gradAct = menu.addAction(QStringLiteral("Convert to gradient…"));
        }
    } else if (key.startsWith(QStringLiteral("font.family."))) {
        familyAct = menu.addAction(QStringLiteral("Edit font family…"));
    } else if (key.startsWith(QStringLiteral("font.size."))
               || key.startsWith(QStringLiteral("sizing."))) {
        numAct = menu.addAction(QStringLiteral("Edit size…"));
    }

    QAction* resetAct = nullptr;
    if (tm.hasFactoryValue(key)) {
        if (!menu.isEmpty()) menu.addSeparator();
        resetAct = menu.addAction(QStringLiteral("Reset to default"));
    }

    QAction* chosen = menu.exec(QCursor::pos());
    if (!chosen) return;
    if      (chosen == flatAct)   editTokenAsFlat(key, item);
    else if (chosen == gradAct)   editTokenAsGradient(key, item);
    else if (chosen == familyAct) editTokenFontFamily(key, item);
    else if (chosen == numAct)    editTokenSizing(key, item);
    else if (chosen == resetAct)  resetTokenToFactory(key, item);
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

void ThemeEditorDialog::editTokenFontFamily(const QString& key,
                                            QListWidgetItem* item)
{
    auto& tm = ThemeManager::instance();
    const QString currentFamily = tm.value(key);
    QFont seed;
    if (!currentFamily.isEmpty()) seed.setFamily(currentFamily);

    bool ok = false;
    const QFont chosen = QFontDialog::getFont(&ok, seed, this,
        QStringLiteral("Edit %1").arg(key));
    if (!ok) return;
    tm.setString(key, chosen.family());
    updateRow(item);
}

void ThemeEditorDialog::editTokenSizing(const QString& key,
                                        QListWidgetItem* item)
{
    auto& tm = ThemeManager::instance();
    const int current = tm.sizing(key);
    // Integer tokens span both font.size.* and sizing.*; allow a generous
    // range and let the QSpinBox handle clamping.  Range chosen to cover
    // realistic font sizes (4–96 px) and panel paddings (0–48 px) in one
    // input — at the cost of letting a user dial a 96-px panel padding
    // if they really want one.
    bool ok = false;
    const int chosen = QInputDialog::getInt(this,
        QStringLiteral("Edit %1").arg(key),
        QStringLiteral("Value (px):"),
        current, 0, 96, 1, &ok);
    if (!ok) return;
    tm.setSizing(key, chosen);
    updateRow(item);
}

void ThemeEditorDialog::onRenameThemeClicked()
{
    auto& tm = ThemeManager::instance();
    const QString current = tm.activeTheme();
    if (tm.isBuiltInTheme(current)) {
        QMessageBox::information(this, QStringLiteral("Cannot rename"),
            QStringLiteral("\"%1\" is a built-in theme and can't be renamed. "
                           "Use Save As… to create an editable copy under a "
                           "different name first.").arg(current));
        return;
    }
    bool ok = false;
    const QString newName = QInputDialog::getText(this,
        QStringLiteral("Rename theme"),
        QStringLiteral("New name for \"%1\":").arg(current),
        QLineEdit::Normal, current, &ok).trimmed();
    if (!ok || newName.isEmpty() || newName == current) return;

    if (!tm.renameTheme(current, newName)) {
        QMessageBox::warning(this, QStringLiteral("Rename failed"),
            QStringLiteral("Could not rename \"%1\" to \"%2\".  A theme with "
                           "that name may already exist, or the theme file "
                           "may be unwriteable.").arg(current, newName));
    }
}

void ThemeEditorDialog::onExportThemeClicked()
{
    auto& tm = ThemeManager::instance();
    const QString current = tm.activeTheme();
    if (current.isEmpty()) return;

    // Suggest a filename matching the theme name + the `.aethertheme`
    // extension so shared files are recognisable on sight.  The OS file
    // picker remembers the last directory across invocations.
    const QString suggested = QDir::homePath()
                              + QLatin1Char('/')
                              + current + QStringLiteral(".aethertheme");
    const QString path = QFileDialog::getSaveFileName(this,
        QStringLiteral("Export theme"), suggested,
        QStringLiteral("AetherSDR themes (*.aethertheme *.json)"));
    if (path.isEmpty()) return;

    QString err;
    if (!tm.exportThemeToFile(current, path, &err)) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
            QStringLiteral("Could not write \"%1\":\n\n%2").arg(path, err));
        return;
    }
}

void ThemeEditorDialog::onImportThemeClicked()
{
    const QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("Import theme"), QDir::homePath(),
        QStringLiteral("AetherSDR themes (*.aethertheme *.json)"));
    if (path.isEmpty()) return;
    importThemeFromPath(path);
}

void ThemeEditorDialog::importThemeFromPath(const QString& filePath)
{
    auto& tm = ThemeManager::instance();
    QString err;
    const QString name = tm.importThemeFromFile(filePath, &err);
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Import failed"),
            QStringLiteral("Could not import \"%1\":\n\n%2").arg(filePath, err));
        return;
    }
    QMessageBox::information(this, QStringLiteral("Theme imported"),
        QStringLiteral("Installed \"%1\" and made it the active theme.").arg(name));
}

void ThemeEditorDialog::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        PersistentDialog::dragEnterEvent(event);
        return;
    }
    // Only accept drops where every URL is a theme-shaped file — refusing
    // the drop is the cleanest signal that an arbitrary file isn't valid.
    const auto urls = event->mimeData()->urls();
    for (const QUrl& u : urls) {
        if (!u.isLocalFile()) {
            PersistentDialog::dragEnterEvent(event);
            return;
        }
        const QString suffix = QFileInfo(u.toLocalFile()).suffix().toLower();
        if (suffix != QStringLiteral("aethertheme")
            && suffix != QStringLiteral("json")) {
            PersistentDialog::dragEnterEvent(event);
            return;
        }
    }
    event->acceptProposedAction();
}

void ThemeEditorDialog::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasUrls()) {
        PersistentDialog::dropEvent(event);
        return;
    }
    const auto urls = event->mimeData()->urls();
    for (const QUrl& u : urls) {
        if (u.isLocalFile()) importThemeFromPath(u.toLocalFile());
    }
    event->acceptProposedAction();
}

void ThemeEditorDialog::onDeleteThemeClicked()
{
    auto& tm = ThemeManager::instance();
    const QString current = tm.activeTheme();
    if (tm.isBuiltInTheme(current)) {
        QMessageBox::information(this, QStringLiteral("Cannot delete"),
            QStringLiteral("\"%1\" is a built-in theme and can't be deleted.").arg(current));
        return;
    }
    const auto reply = QMessageBox::question(this,
        QStringLiteral("Delete theme"),
        QStringLiteral("Permanently delete \"%1\"?\n\n"
                       "The theme file at "
                       "~/.config/AetherSDR/themes/%1.json will be removed. "
                       "The active theme will switch to Default Dark.")
            .arg(current),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    if (!tm.deleteTheme(current)) {
        QMessageBox::warning(this, QStringLiteral("Delete failed"),
            QStringLiteral("Could not delete \"%1\". The theme file may be "
                           "locked by another process.").arg(current));
    }
}

void ThemeEditorDialog::resetTokenToFactory(const QString& key,
                                            QListWidgetItem* item)
{
    auto& tm = ThemeManager::instance();
    if (!tm.hasFactoryValue(key)) return;

    if (key.startsWith(QStringLiteral("color."))) {
        // Gradients restore through setGradient; scalars through setColor.
        // The factory snapshot lookup goes through factoryGradient first,
        // falls back to factoryColor.  An empty gradient with a valid
        // factoryColor means the factory baseline is a scalar.
        const ThemeGradient g = tm.factoryGradient(key);
        if (!g.stops.isEmpty()) {
            tm.setGradient(key, g);
        } else {
            const QColor c = tm.factoryColor(key);
            if (c.isValid()) tm.setColor(key, c);
        }
    } else if (key.startsWith(QStringLiteral("font.family."))) {
        const QString f = tm.factoryString(key);
        if (!f.isEmpty()) tm.setString(key, f);
    } else if (key.startsWith(QStringLiteral("font.size."))
               || key.startsWith(QStringLiteral("sizing."))) {
        const int v = tm.factorySizing(key);
        if (v >= 0) tm.setSizing(key, v);
    }
    updateRow(item);
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
