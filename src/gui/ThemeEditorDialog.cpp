#include "ThemeEditorDialog.h"
#include "Theme.h"
#include "ThemeInspector.h"
#include "TokenEditorWidget.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QAction>
#include <QComboBox>
#include <QCursor>
#include <QTimer>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QUrl>
#include <QLineEdit>
#include <QHeaderView>
#include <QTreeWidget>
#include <QTreeWidgetItem>
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
    // Size budget — fits CompactColorPicker (~218×220) on the color
    // page; the gradient page is taller (strip + stop list + picker
    // + angle row).
    setMinimumSize(440, 720);
    applyAppTheme(this);

    // Container declaration — places this dialog under the "dialog"
    // umbrella so step 3's editor UI can navigate from root → dialog →
    // dialog.themeEditor when rendering its own scope tree.
    theme::setContainer(this, QStringLiteral("dialog/themeEditor"));

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_themeLabel = new QLabel(bodyWidget());
    m_themeLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-weight: bold; }"));
    root->addWidget(m_themeLabel);

    // Inline editor — swaps controls based on the currently selected
    // token's namespace + value shape.  Sits above the filter + token
    // list so the operator can see what they're editing without their
    // gaze jumping to a separate window.  Every control writes live to
    // ThemeManager, so the rest of the app re-themes as they edit.
    m_tokenEditor = new TokenEditorWidget(bodyWidget());
    root->addWidget(m_tokenEditor);

    auto* divider = new QFrame(bodyWidget());
    divider->setFrameShape(QFrame::HLine);
    divider->setFrameShadow(QFrame::Sunken);
    root->addWidget(divider);

    // Inspector + scope picker row.  Layout left-to-right:
    //   [Scope: combo] [Inspect btn] [Editing label] [status] [Reset btn]
    // Scope picker sits leftmost so it visually anchors the row as
    // "where this edit will land".  Combo width is clamped to the
    // longest container-path label + 4 px pad on each side; no
    // stretch so it doesn't grow with the dialog.
    auto* inspectRow = new QHBoxLayout;
    inspectRow->addWidget(new QLabel(QStringLiteral("Scope:"), bodyWidget()));
    m_containerCombo = new QComboBox(bodyWidget());
    m_containerCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_containerCombo->setToolTip(QStringLiteral(
        "Container scope to edit.  \"(root)\" is the global namespace.\n"
        "Selecting a nested scope (e.g. applet.tx) routes new overrides\n"
        "into that scope, and only widgets inside that container see them."));
    inspectRow->addWidget(m_containerCombo);

    m_inspectBtn = new QPushButton(QStringLiteral("🎯  Inspect"), bodyWidget());
    m_inspectBtn->setCheckable(true);
    m_inspectBtn->setToolTip(QStringLiteral(
        "Click, then point at any region of the main UI to find the\n"
        "token(s) painting it.  ESC cancels."));
    inspectRow->addWidget(m_inspectBtn);
    // "Editing: <token>" sits left-aligned right after the Inspect
    // button.  Inspector status text takes the leftover stretch so
    // transient messages ("Inspector canceled.") show between the
    // header and the Reset button without nudging either side.
    // Inspector status sits on the inspect row in transient-message
    // territory; Editing label moves down to the filter row.
    m_inspectStatus = new QLabel(bodyWidget());
    m_inspectStatus->setWordWrap(true);
    m_inspectStatus->setStyleSheet(QStringLiteral(
        "QLabel { font-style: italic; }"));
    inspectRow->addWidget(m_inspectStatus, 1);
    // Reset button reparents out of the editor's bottom row and sits
    // right-aligned on the inspect row — keeps the editor's Cancel/OK
    // pair clean, and Reset stays visible without scrolling.
    if (auto* reset = m_tokenEditor->resetButton()) {
        inspectRow->addWidget(reset, 0, Qt::AlignRight);
    }
    root->addLayout(inspectRow);

    // Filter input + "Editing: <token>" header share one row: filter
    // is clamped to ~50% of the body width, header takes the rest so
    // its single-line layout always has the room it needs to render.
    auto* filterRow = new QHBoxLayout;
    filterRow->setSpacing(8);
    m_filterEdit = new QLineEdit(bodyWidget());
    m_filterEdit->setPlaceholderText(QStringLiteral("Filter tokens (e.g. accent, slice, meter)…"));
    m_filterEdit->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    filterRow->addWidget(m_filterEdit, 1);
    if (auto* hdr = m_tokenEditor->headerLabel()) {
        filterRow->addWidget(hdr, 1, Qt::AlignLeft | Qt::AlignVCenter);
    }
    root->addLayout(filterRow);

    m_tokenList = new QTreeWidget(bodyWidget());
    m_tokenList->setIconSize(QSize(18, 18));
    m_tokenList->setAlternatingRowColors(true);
    m_tokenList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tokenList->setRootIsDecorated(false);   // flat-table look
    m_tokenList->setIndentation(0);
    m_tokenList->setUniformRowHeights(true);
    m_tokenList->setSortingEnabled(false);
    // Columns rebuild dynamically as the scope picker changes — see
    // rebuildColumns().  Initial setup with just the Object + Value
    // pair so the widget has something to render before refreshTokenList.
    rebuildColumns();
    root->addWidget(m_tokenList, 1);

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

    refreshContainerCombo();
    refreshTokenList();
    updateTitle();
    m_lastRenderedTheme = ThemeManager::instance().activeTheme();

    // Selection-driven editing — `currentItemChanged` fires for both
    // mouse clicks and keyboard navigation so arrow-key walks through
    // the list also update the inline editor.
    connect(m_tokenList, &QTreeWidget::currentItemChanged,
            this, &ThemeEditorDialog::onTokenRowSelectionChanged);
    // Click on a scope-column cell focuses that scope in the picker AND
    // selects the row's token — the columnar view becomes the primary
    // navigation surface instead of being read-only.
    connect(m_tokenList, &QTreeWidget::itemClicked,
            this, &ThemeEditorDialog::onTokenCellClicked);
    m_tokenList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tokenList, &QTreeWidget::customContextMenuRequested,
            this, &ThemeEditorDialog::onTokenContextMenu);
    connect(m_tokenEditor, &TokenEditorWidget::tokenChanged,
            this, &ThemeEditorDialog::onTokenEditedByEditor);
    // Built-in themes can't be edited in place; the editor stashes
    // its buffered edit and asks us to run Save As, then we restore
    // the snapshot into the new user copy.
    connect(m_tokenEditor, &TokenEditorWidget::requestSaveAsBeforeCommit,
            this, &ThemeEditorDialog::onSaveAsBeforeCommit);
    connect(m_containerCombo, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ThemeEditorDialog::onContainerChanged);

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

void ThemeEditorDialog::rebuildColumns()
{
    if (!m_tokenList) return;
    // Column layout: Object | <root, seg0, seg0/seg1, …, leaf> | Value
    QStringList scopeLevels;       // canonical paths for each header column
    QStringList headerLabels = { QStringLiteral("Object") };
    if (m_activeContainerPath.isEmpty()) {
        // No scope selected → just Object + Value.  Skip the "root"
        // column since it would duplicate the Value column.
    } else {
        scopeLevels.append(QString());          // root
        headerLabels.append(QStringLiteral("root"));
        const QStringList segs = m_activeContainerPath.split(QLatin1Char('/'),
                                                             Qt::SkipEmptyParts);
        QString running;
        for (const QString& s : segs) {
            running = running.isEmpty() ? s : running + QLatin1Char('/') + s;
            scopeLevels.append(running);
            headerLabels.append(s);
        }
    }
    headerLabels.append(QStringLiteral("Value"));

    m_tokenList->setColumnCount(headerLabels.size());
    m_tokenList->setHeaderLabels(headerLabels);
    // The Object + Value columns size to content; intermediate scope
    // columns get reasonable fixed widths so the table doesn't lurch
    // every time the user picks a different leaf.
    auto* hdr = m_tokenList->header();
    if (hdr) {
        hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        for (int i = 1; i < headerLabels.size() - 1; ++i) {
            hdr->setSectionResizeMode(i, QHeaderView::Stretch);
        }
        hdr->setSectionResizeMode(headerLabels.size() - 1,
                                  QHeaderView::ResizeToContents);
        hdr->setStretchLastSection(false);
    }
    // Stash scope paths on the header column index so populateRow()
    // can use them to look up the override value at each level.
    for (int i = 0; i < scopeLevels.size(); ++i) {
        // header data slot per column: Qt::UserRole carries the path
        m_tokenList->headerItem()->setData(i + 1, Qt::UserRole, scopeLevels.at(i));
    }
}

void ThemeEditorDialog::refreshTokenList()
{
    m_tokenList->clear();
    rebuildColumns();
    auto& tm = ThemeManager::instance();
    for (const QString& key : tm.allTokenKeys()) {
        // Editable token namespaces — gradient + scalar colours under
        // "color.", string + integer leaves under "font." and "sizing.".
        if (!key.startsWith(QStringLiteral("color."))
            && !key.startsWith(QStringLiteral("font."))
            && !key.startsWith(QStringLiteral("sizing."))) {
            continue;
        }
        auto* item = new QTreeWidgetItem;
        item->setData(0, Qt::UserRole, key);
        m_tokenList->addTopLevelItem(item);
        populateRow(item);
    }
    // Re-apply the current filter (text + inspector subset) so the
    // user's view doesn't suddenly fall back to "show everything"
    // after a scope change / theme reload.  filterTokensTo reads the
    // QLineEdit's current text directly.
    filterTokensTo(m_activeSubset);
}

void ThemeEditorDialog::populateRow(QTreeWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(0, Qt::UserRole).toString();
    auto& tm = ThemeManager::instance();

    // Column 0 — Object: token name + swatch.
    QIcon swatch;
    if (key.startsWith(QStringLiteral("color."))) {
        if (tm.brush(key).gradient()) {
            swatch = gradientSwatchIcon(tm.gradient(key));
        } else {
            swatch = swatchIcon(tm.color(key));
        }
    }
    item->setIcon(0, swatch);
    item->setText(0, key);

    // Intermediate scope-chain columns (if any).  Each shows either
    // the literal override stored at that scope, or "inherited" in
    // italics when the scope inherits from an ancestor.
    const int nCols = m_tokenList->columnCount();
    const int lastCol = nCols - 1;
    const QFont normalFont = m_tokenList->font();
    QFont inheritedFont = normalFont; inheritedFont.setItalic(true);
    for (int col = 1; col < lastCol; ++col) {
        const QString scopePath = m_tokenList->headerItem()->data(col, Qt::UserRole).toString();
        if (tm.isOverriddenAt(scopePath, key)) {
            // Format the override succinctly: hex for colours, plain
            // number / string for sizings and font families.
            QString text;
            if (key.startsWith(QStringLiteral("color."))) {
                if (tm.brush(key).gradient()) {
                    {
                    const ThemeGradient g = tm.gradientAt(scopePath, key);
                    text = g.type == ThemeGradient::Radial
                        ? QStringLiteral("radial, %1 stops").arg(g.stops.size())
                        : QStringLiteral("linear, %1°, %2 stops")
                              .arg(static_cast<int>(std::round(g.angle)))
                              .arg(g.stops.size());
                }
                } else {
                    text = colorToTokenHex(tm.colorAt(scopePath, key));
                }
            } else if (key.startsWith(QStringLiteral("font.family."))) {
                text = tm.valueAt(scopePath, key);
            } else {
                text = QStringLiteral("%1 px").arg(tm.sizingAt(scopePath, key));
            }
            item->setText(col, text);
            item->setFont(col, normalFont);
            item->setForeground(col, QBrush());
        } else {
            item->setText(col, QStringLiteral("inherited"));
            item->setFont(col, inheritedFont);
            item->setForeground(col, QBrush(QColor(0x60, 0x70, 0x80)));
        }
    }

    // Last column — Value: the resolved value walking the chain from
    // the leaf container (or root, when no scope is selected).
    const QString leafPath = m_activeContainerPath;
    if (key.startsWith(QStringLiteral("color."))) {
        if (tm.brush(key).gradient()) {
            {
                const ThemeGradient g = tm.gradientAt(leafPath, key);
                const QString text = g.type == ThemeGradient::Radial
                    ? QStringLiteral("radial, %1 stops").arg(g.stops.size())
                    : QStringLiteral("linear, %1°, %2 stops")
                          .arg(static_cast<int>(std::round(g.angle)))
                          .arg(g.stops.size());
                item->setText(lastCol, text);
            }
        } else {
            item->setText(lastCol, colorToTokenHex(tm.colorAt(leafPath, key)));
        }
    } else if (key.startsWith(QStringLiteral("font.family."))) {
        item->setText(lastCol, tm.valueAt(leafPath, key));
    } else {
        item->setText(lastCol, QStringLiteral("%1 px").arg(tm.sizingAt(leafPath, key)));
    }
}

void ThemeEditorDialog::updateRow(QTreeWidgetItem* item)
{
    populateRow(item);
}

void ThemeEditorDialog::updateTitle()
{
    const QString name = ThemeManager::instance().activeTheme();
    m_themeLabel->setText(QStringLiteral("Profile: %1").arg(
        name.isEmpty() ? QStringLiteral("(no active theme)") : name));
    setWindowTitle(QStringLiteral("Theme Editor — %1").arg(name));
}

void ThemeEditorDialog::onTokenRowSelectionChanged()
{
    // Triggered by either mouse click or keyboard navigation on the
    // token list — load the selected token into the inline editor so
    // its controls reflect the value and the operator can edit in place.
    auto* item = m_tokenList->currentItem();
    if (!item) {
        m_tokenEditor->setToken(QString());
        return;
    }
    m_tokenEditor->setToken(item->data(0, Qt::UserRole).toString());
}

void ThemeEditorDialog::onTokenEditedByEditor(const QString& key)
{
    // The inline editor just wrote a new value for `key` to ThemeManager —
    // refresh the matching list row so its swatch + descriptor reflect
    // the edit.  ThemeManager's themeChanged signal already drove a
    // repaint everywhere else; this just keeps the editor's own list
    // consistent.
    for (int i = 0; i < m_tokenList->topLevelItemCount(); ++i) {
        auto* item = m_tokenList->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == key) {
            updateRow(item);
            break;
        }
    }
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
        return;
    }

    // The per-theme recent-colors bucket in AppSettings outlives the
    // theme file by default; clean it up so deleted themes don't
    // accumulate orphan entries under ThemeEditor.RecentColors/<name>.
    auto& s = AppSettings::instance();
    const QString recentsKey = TokenEditorWidget::recentColorsKeyFor(current);
    if (s.contains(recentsKey)) {
        s.remove(recentsKey);
        s.save();
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

void ThemeEditorDialog::onSaveAsBeforeCommit()
{
    auto& tm = ThemeManager::instance();
    const QString current = tm.activeTheme();
    if (!tm.isBuiltInTheme(current)) {
        // Race / stale signal — nothing to fork, just complete the
        // commit directly.
        m_tokenEditor->completeDeferredCommit();
        return;
    }

    bool ok = false;
    QString suggestion = QStringLiteral("My %1").arg(current);
    const QString name = QInputDialog::getText(this,
        QStringLiteral("Save Theme As"),
        QStringLiteral("\"%1\" is a built-in theme and can't be modified "
                       "directly.\nSave your changes as a new theme:")
            .arg(current),
        QLineEdit::Normal, suggestion, &ok).trimmed();
    if (!ok || name.isEmpty()) return;  // user cancelled — buffer stays uncommitted

    // Disallow accidentally writing to another built-in or overwriting
    // an existing user theme without confirmation.
    if (tm.isBuiltInTheme(name)) {
        QMessageBox::warning(this, QStringLiteral("Reserved name"),
            QStringLiteral("\"%1\" is a built-in theme name and can't be used.")
                .arg(name));
        return;
    }
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
    // saveCurrentThemeAs() has switched the active theme and refreshed
    // the list (clearing the editor's selection in the process).  The
    // editor stashed its buffer in DeferredEdit before emitting; tell
    // it to write that into the new user copy now.
    m_tokenEditor->completeDeferredCommit();

    // Re-select the row that matches the just-committed token so the
    // list highlight matches what the editor is showing.
    const QString committed = m_tokenEditor->currentToken();
    if (!committed.isEmpty()) {
        for (int i = 0; i < m_tokenList->topLevelItemCount(); ++i) {
            auto* it = m_tokenList->topLevelItem(i);
            if (it->data(0, Qt::UserRole).toString() == committed) {
                m_tokenList->setCurrentItem(it);
                break;
            }
        }
    }
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

    // Auto-select the picked widget's container scope so any subsequent
    // edit lands at the right level of the inheritance chain.  Walks
    // `target`'s Qt parent chain to the nearest declared `themeContainer`
    // (the same lookup widget-aware getters use for token resolution).
    const QString pickedScope = tm.containerPathFor(target);
    if (m_containerCombo) {
        const int idx = m_containerCombo->findData(pickedScope);
        if (idx >= 0) m_containerCombo->setCurrentIndex(idx);
    }

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
    const QString scopeSuffix = pickedScope.isEmpty()
        ? QString()
        : QStringLiteral("  ·  scope: %1").arg(pickedScope);
    if (w && w != target) {
        updateInspectorStatus(QStringLiteral(
            "%1 (via %2): %3 token%4.%5").arg(matchedClass).arg(hitName)
            .arg(tokens.size()).arg(tokens.size() == 1 ? "" : "s")
            .arg(scopeSuffix));
    } else {
        updateInspectorStatus(QStringLiteral(
            "%1: %2 token%3.%4").arg(matchedClass)
            .arg(tokens.size()).arg(tokens.size() == 1 ? "" : "s")
            .arg(scopeSuffix));
    }

    // Convenience: if exactly one token matched, select it so the
    // inline editor surfaces it immediately — the "click ugly thing →
    // fix it" flow now happens in the editor above instead of a modal.
    if (tokens.size() == 1) {
        for (int i = 0; i < m_tokenList->topLevelItemCount(); ++i) {
            auto* item = m_tokenList->topLevelItem(i);
            if (item->isHidden()) continue;
            if (item->data(0, Qt::UserRole).toString() == tokens.first()) {
                m_tokenList->setCurrentItem(item);
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
    for (int i = 0; i < m_tokenList->topLevelItemCount(); ++i) {
        auto* item = m_tokenList->topLevelItem(i);
        const QString key = item->data(0, Qt::UserRole).toString();
        bool hidden = false;
        if (!subset.isEmpty() && !wanted.contains(key)) hidden = true;
        if (!hidden && !needle.isEmpty() && !key.contains(needle)) hidden = true;
        item->setHidden(hidden);
    }
}

void ThemeEditorDialog::refreshContainerCombo()
{
    if (!m_containerCombo) return;
    QSignalBlocker block(m_containerCombo);
    m_containerCombo->clear();
    // Friendlier label for the empty (root) path so the dropdown is
    // self-explanatory; userData carries the real path string.
    m_containerCombo->addItem(QStringLiteral("(root)"), QString());
    for (const QString& path : ThemeManager::instance().containerPaths()) {
        if (path.isEmpty()) continue;  // already added as "(root)"
        m_containerCombo->addItem(path, path);
    }
    // Clamp the combo to the longest label + 4 px pad on each side
    // (plus arrow + frame allowance).  Sized once per refresh; combo
    // stays exactly that wide regardless of which item is selected.
    QFontMetrics fm(m_containerCombo->font());
    int maxLabelW = 0;
    for (int i = 0; i < m_containerCombo->count(); ++i) {
        maxLabelW = std::max(maxLabelW,
                             fm.horizontalAdvance(m_containerCombo->itemText(i)));
    }
    // 4 px L + 4 px R text padding (overrides Theme.h's 6/6 default),
    // ~20 px dropdown arrow, ~2 px frame borders.
    m_containerCombo->setStyleSheet(QStringLiteral(
        "QComboBox { padding: 2px 4px; }"));
    m_containerCombo->setFixedWidth(maxLabelW + 4 + 4 + 20 + 2);
    // Restore previous selection by path string, falling back to root.
    int idx = m_containerCombo->findData(m_activeContainerPath);
    if (idx < 0) idx = 0;
    m_containerCombo->setCurrentIndex(idx);
}

void ThemeEditorDialog::onContainerChanged(int)
{
    if (!m_containerCombo) return;
    m_activeContainerPath = m_containerCombo->currentData().toString();
    if (m_tokenEditor) m_tokenEditor->setActiveContainerPath(m_activeContainerPath);
    // Defer the heavy rebuild to the next event-loop tick.  When the
    // inspector triggers a scope change via setCurrentIndex, the slot
    // runs inside the synchronous signal cascade from
    // ThemeInspector::eventFilter → widgetPicked → onInspectorPicked
    // → setCurrentIndex; tearing down QTreeWidgetItems (and their
    // QPixmap-backed icon QVariants) inside that stack has hit a
    // destructor SEGV in the wild.  Deferring breaks the chain — the
    // inspector callback completes, and the rebuild runs from a
    // clean stack on the next iteration.
    QTimer::singleShot(0, this, &ThemeEditorDialog::refreshTokenList);
}

void ThemeEditorDialog::onTokenCellClicked(QTreeWidgetItem* item, int column)
{
    if (!item || !m_tokenList) return;
    if (column == 0) return;                                  // Object column: already wired via currentItemChanged
    if (column == m_tokenList->columnCount() - 1) return;     // Value column: just informational
    // Scope-chain column — switch the active scope to that column's
    // path so the editor's commit lands there.  The user's perspective
    // becomes "I want to override this token at this exact scope".
    const QString scopePath =
        m_tokenList->headerItem()->data(column, Qt::UserRole).toString();
    // Sync the picker, which fires onContainerChanged → updates the
    // editor + columns + repopulates the rows.
    const int idx = m_containerCombo
                        ? m_containerCombo->findData(scopePath)
                        : -1;
    if (idx >= 0) m_containerCombo->setCurrentIndex(idx);
    // After the picker change re-selects the previous current row,
    // explicitly re-select this row so the editor reflects the clicked
    // token (the refresh may have lost the highlight).
    m_tokenList->setCurrentItem(item);
}

void ThemeEditorDialog::onTokenContextMenu(const QPoint& pos)
{
    if (!m_tokenList) return;
    QTreeWidgetItem* item = m_tokenList->itemAt(pos);
    if (!item) return;
    const int column = m_tokenList->columnAt(pos.x());
    // Right-click is only meaningful on a scope-chain column (not
    // Object, not Value) where an actual override could exist.
    if (column <= 0 || column >= m_tokenList->columnCount() - 1) return;
    const QString scopePath =
        m_tokenList->headerItem()->data(column, Qt::UserRole).toString();
    // Root column has no parent to fall back to — "clear" there would
    // delete the token tree-wide.  Reset-to-factory belongs to the
    // dedicated Reset button on the inspect row.
    if (scopePath.isEmpty()) return;
    const QString token = item->data(0, Qt::UserRole).toString();
    auto& tm = ThemeManager::instance();
    if (!tm.isOverriddenAt(scopePath, token)) return;  // nothing to clear

    const QString scopeLabel = scopePath.isEmpty()
                                   ? QStringLiteral("(root)")
                                   : scopePath;
    QMenu menu(this);
    QAction* clearAct = menu.addAction(
        QStringLiteral("Clear override at %1").arg(scopeLabel));
    QAction* picked = menu.exec(m_tokenList->viewport()->mapToGlobal(pos));
    if (picked == clearAct) {
        tm.removeOverride(scopePath, token);
        // Refresh the row so the column flips back to italic "inherited".
        populateRow(item);
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
        refreshContainerCombo();
        refreshTokenList();
    }
}

} // namespace AetherSDR
