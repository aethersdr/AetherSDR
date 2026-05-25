#include "ThemeEditorDialog.h"
#include "Theme.h"
#include "core/ThemeManager.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {
// Build a tiny coloured square QIcon for use as a swatch on a list row.
// Re-rendered whenever a token's value changes so the swatch tracks the
// live value.
QIcon swatchIcon(const QColor& c)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(QPen(QColor(0, 0, 0, 80), 1));
    p.drawRoundedRect(QRectF(0.5, 0.5, 15.0, 15.0), 3, 3);
    return QIcon(pm);
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Theme Editor"));
    // Modeless tool window — stays open while the operator interacts
    // with the main UI, never blocks input on the rest of the app.
    setWindowFlags(windowFlags() | Qt::Tool);
    setModal(false);
    resize(420, 560);
    applyAppTheme(this);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    m_themeLabel = new QLabel(this);
    m_themeLabel->setStyleSheet(QStringLiteral(
        "QLabel { font-weight: bold; }"));
    root->addWidget(m_themeLabel);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(QStringLiteral("Filter tokens (e.g. accent, slice, meter)…"));
    root->addWidget(m_filterEdit);

    m_tokenList = new QListWidget(this);
    m_tokenList->setIconSize(QSize(18, 18));
    m_tokenList->setAlternatingRowColors(true);
    m_tokenList->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_tokenList, 1);

    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_saveAsBtn = new QPushButton(QStringLiteral("Save As…"), this);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"), this);
    btnRow->addWidget(m_saveAsBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    refreshTokenList();
    updateTitle();
    m_lastRenderedTheme = ThemeManager::instance().activeTheme();

    connect(m_tokenList, &QListWidget::itemClicked,
            this, &ThemeEditorDialog::onTokenRowClicked);
    connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& q) {
        const QString needle = q.trimmed().toLower();
        for (int i = 0; i < m_tokenList->count(); ++i) {
            auto* item = m_tokenList->item(i);
            item->setHidden(!needle.isEmpty()
                            && !item->data(Qt::UserRole).toString().contains(needle));
        }
    });
    connect(m_saveAsBtn, &QPushButton::clicked,
            this, &ThemeEditorDialog::onSaveAsClicked);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    // Active-theme switches outside the editor (e.g. via View → Theme)
    // should refresh both the title and the swatch values so the editor
    // doesn't lie about what it's editing.
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &ThemeEditorDialog::onActiveThemeChanged);
}

void ThemeEditorDialog::refreshTokenList()
{
    m_tokenList->clear();
    auto& tm = ThemeManager::instance();
    for (const QString& key : tm.allTokenKeys()) {
        // Color tokens only in this first cut — gradient + sizing + font
        // get their own editing surfaces in follow-on PRs.
        if (!key.startsWith(QStringLiteral("color.")))
            continue;
        // Skip gradient leaves (waterfall.colormap.default, slice.dim.*
        // group nodes that flatten to gradient values).  The token map
        // stores them as ThemeGradient; color() returns the first stop
        // for them, which would be misleading to edit as a flat colour.
        const QBrush br = tm.brush(key);
        if (br.gradient()) continue;

        const QColor c = tm.color(key);
        auto* item = new QListWidgetItem(swatchIcon(c),
            QStringLiteral("%1   %2").arg(key, -36).arg(c.name()));
        item->setData(Qt::UserRole, key);
        m_tokenList->addItem(item);
    }
}

void ThemeEditorDialog::updateRow(QListWidgetItem* item)
{
    if (!item) return;
    const QString key = item->data(Qt::UserRole).toString();
    const QColor c = ThemeManager::instance().color(key);
    item->setIcon(swatchIcon(c));
    item->setText(QStringLiteral("%1   %2").arg(key, -36).arg(c.name()));
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
    const QColor current = tm.color(key);

    const QColor chosen = QColorDialog::getColor(current, this,
        QStringLiteral("Edit %1").arg(key),
        QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    tm.setColor(key, chosen);
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
