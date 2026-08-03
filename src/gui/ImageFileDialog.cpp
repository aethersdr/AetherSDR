#include "ImageFileDialog.h"

#include <QButtonGroup>
#include <QDialog>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFrame>
#include <QGridLayout>
#include <QIdentityProxyModel>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QListView>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPixmapCache>
#include <QResizeEvent>
#include <QSet>
#include <QSize>
#include <QSizePolicy>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include <algorithm>

namespace AetherSDR {

namespace {

// Mirrors the decompression-bomb guard added for #3990
// (CallsignCard::setPhotoPath) — a small file can still claim a huge pixel
// count, which would freeze/OOM the GUI on decode.
constexpr int kMaxDecodeDimension = 4096;
// 4096 x 4096 x 4 bytes (ARGB32), i.e. the same worst case the dimension
// guard above already allows through when size() is known.
constexpr int kMaxDecodeAllocationMiB = 64;
const QSize kThumbnailDecodeSize(96, 96);
// Caps the preview decode target: the preview label grows with the dialog
// (Expanding size policy), so decoding at its raw size could mean decoding
// at whatever huge size the user resizes the dialog to. QLabel centers the
// (unscaled) pixmap within the available space, so capping here doesn't
// change what's visible for any label smaller than this.
const QSize kPreviewMaxDecodeSize(512, 512);

const QSet<QByteArray>& supportedImageFormats()
{
    static const QSet<QByteArray> formats = [] {
        QSet<QByteArray> s;
        for (const QByteArray& f : QImageReader::supportedImageFormats())
            s.insert(f.toLower());
        return s;
    }();
    return formats;
}

bool isSupportedImage(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return !suffix.isEmpty() && supportedImageFormats().contains(suffix.toUtf8());
}

QString buildImageFilter()
{
    const QSet<QByteArray>& formats = supportedImageFormats();
    if (formats.isEmpty())
        return QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp)");
    QStringList patterns;
    patterns.reserve(formats.size());
    for (const QByteArray& f : formats)
        patterns << QStringLiteral("*.%1").arg(QString::fromLatin1(f));
    patterns.sort();
    return QStringLiteral("Images (%1)").arg(patterns.join(QLatin1Char(' ')));
}

QString cacheKey(const QString& path)
{
    const QFileInfo info(path);
    return QStringLiteral("bgimg:%1:%2")
        .arg(path, QString::number(info.lastModified().toMSecsSinceEpoch()));
}

// Decodes DCT-scaled where the format supports it (e.g. JPEG), so scaling
// down to targetSize keeps this cheap even for large source images.
QImage decodeThumbnail(const QString& path, const QSize& targetSize)
{
    QImageReader reader(path);
    const QSize dim = reader.size();
    if (dim.isValid() && (dim.width() > kMaxDecodeDimension || dim.height() > kMaxDecodeDimension))
        return {};
    reader.setAutoTransform(true);
    const QSize cap = targetSize.expandedTo(QSize(1, 1));
    // Preserve aspect ratio rather than stretching into a square: when the
    // reader can report the source size up front, scale to the largest size
    // that fits targetSize (cheap, since e.g. JPEG decodes DCT-scaled at the
    // computed size).
    if (dim.isValid()) {
        reader.setScaledSize(dim.scaled(cap, Qt::KeepAspectRatio));
        return reader.read();
    }
    // dim invalid: we can't precompute an aspect-correct scaled size, and
    // requesting an arbitrary scaled decode target here wouldn't bound the
    // expensive part anyway -- only JPEG's DCT scaling decodes directly at
    // the requested size; every other format handler still fully decodes at
    // native resolution first and downscales afterward. setAllocationLimit()
    // aborts the decode (returning a null image) once it would exceed this
    // budget, which is what actually guards against a decompression bomb
    // when the pre-decode dimension check above can't run.
    reader.setAllocationLimit(kMaxDecodeAllocationMiB);
    const QImage img = reader.read();
    return img.isNull() ? img : img.scaled(cap, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

void updatePreview(QLabel* label, const QString& path)
{
    if (path.isEmpty() || !isSupportedImage(path)) {
        label->clear();
        return;
    }
    const QImage img = decodeThumbnail(path, label->size().boundedTo(kPreviewMaxDecodeSize));
    if (img.isNull()) {
        label->clear();
        return;
    }
    label->setPixmap(QPixmap::fromImage(img));
}

// Preview pane (#4717): grows with the dialog instead of staying pinned at
// its initial size, matching the platform file picker's resizable preview.
// Re-decoding at the new size on every intermediate frame of a live resize
// drag would stutter, so resizeEvent debounces via a timer rather than
// refreshing immediately; setPreviewPath() (a selection change, not a
// resize) still refreshes right away.
class PreviewLabel : public QLabel
{
public:
    explicit PreviewLabel(QWidget* parent)
        : QLabel(parent)
    {
        m_refreshTimer.setSingleShot(true);
        m_refreshTimer.setInterval(50);
        connect(&m_refreshTimer, &QTimer::timeout, this, [this] { updatePreview(this, m_path); });
    }

    void setPreviewPath(const QString& path)
    {
        m_path = path;
        updatePreview(this, m_path);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        if (!m_path.isEmpty())
            m_refreshTimer.start();
    }

private:
    QString m_path;
    QTimer m_refreshTimer;
};

// Wraps QFileDialog's internal QFileSystemModel to supply real image
// thumbnails for Qt::DecorationRole instead of the generic per-filetype
// icon (#4717). Shared between the dialog's listView and treeView so both
// List and Detail mode show real thumbnails. Decoding happens off the GUI
// thread; results land in a bounded, mtime-keyed QPixmapCache.
class ImageThumbnailProxyModel : public QIdentityProxyModel
{
public:
    explicit ImageThumbnailProxyModel(QObject* parent)
        : QIdentityProxyModel(parent)
    {
    }

    // Gates real-thumbnail decoration on/off. False returns the untouched
    // generic file icon (QIdentityProxyModel::data() passthrough) — the
    // normal small-icon list. True supplies the real decoded photo instead.
    // Kept as an explicit switch rather than relying on icon-size alone:
    // whether the delegate visibly scales a raw QPixmap decoration to a
    // changed view iconSize() is not reliable, so "off" must mean a
    // genuinely different (generic-icon) decoration value, not just a
    // smaller copy of the same thumbnail.
    void setShowThumbnails(bool on)
    {
        if (m_showThumbnails == on)
            return;
        m_showThumbnails = on;
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        // Name is column 0; Size/Type/Date Modified are separate indexes on
        // the same row. Without this check every column got the same
        // thumbnail for Qt::DecorationRole (the path lookup is column-
        // agnostic), crowding the attribute columns' text out entirely.
        if (role != Qt::DecorationRole || index.column() != 0 || !m_showThumbnails)
            return QIdentityProxyModel::data(index, role);

        const QString path = pathForIndex(index);
        if (path.isEmpty())
            return QIdentityProxyModel::data(index, role);

        QPixmap cached;
        if (QPixmapCache::find(cacheKey(path), &cached))
            return cached;

        const_cast<ImageThumbnailProxyModel*>(this)->scheduleDecode(
            path, QPersistentModelIndex(mapToSource(index)));
        return QIdentityProxyModel::data(index, role);
    }

private:
    QString pathForIndex(const QModelIndex& index) const
    {
        const auto* fsModel = qobject_cast<const QFileSystemModel*>(sourceModel());
        if (!fsModel)
            return {};
        const QModelIndex srcIdx = mapToSource(index);
        if (!srcIdx.isValid() || fsModel->isDir(srcIdx))
            return {};
        const QString path = fsModel->filePath(srcIdx);
        return isSupportedImage(path) ? path : QString();
    }

    void scheduleDecode(const QString& path, const QPersistentModelIndex& srcIdx)
    {
        if (m_pending.contains(path))
            return;
        m_pending.insert(path);

        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, path, srcIdx] {
            m_pending.remove(path);
            const QImage img = watcher->result();
            watcher->deleteLater();
            if (img.isNull() || !srcIdx.isValid())
                return;
            QPixmapCache::insert(cacheKey(path), QPixmap::fromImage(img));
            const QModelIndex proxyIdx = mapFromSource(srcIdx);
            if (proxyIdx.isValid())
                emit dataChanged(proxyIdx, proxyIdx, {Qt::DecorationRole});
        });
        watcher->setFuture(QtConcurrent::run(
            [path] { return decodeThumbnail(path, kThumbnailDecodeSize); }));
    }

    QSet<QString> m_pending;
    bool m_showThumbnails = false;
};

} // namespace

QString getBackgroundImagePath(QWidget* parent, const QString& caption)
{
    QFileDialog dlg(parent, caption, QString(), buildImageFilter());
    dlg.setFileMode(QFileDialog::ExistingFile);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setAcceptMode(QFileDialog::AcceptOpen);
    // Detail (name/date modified/type/size columns, small icons) instead of
    // the default, which shows the "Computer" root's drives as large icons —
    // distracting, and inconsistent with how the rest of the list renders.
    // Qt's own view, not the native shell's (DontUseNativeDialog above), so
    // this is identical across Windows/Linux/macOS.
    dlg.setViewMode(QFileDialog::Detail);

    // Best-effort augmentation: if QFileDialog's internal layout/widget
    // names ever change under us, fall back to the plain (wider-filter)
    // dialog rather than crashing.
    auto* grid = qobject_cast<QGridLayout*>(dlg.layout());
    auto* listView = dlg.findChild<QListView*>(QStringLiteral("listView"));
    auto* treeView = dlg.findChild<QTreeView*>(QStringLiteral("treeView"));
    PreviewLabel* previewLabel = nullptr;

    if (grid && listView) {
        // Deliberately not touching QFileDialog's own view/flow/layout
        // (setViewMode, setFlow, setWrapping, setResizeMode,
        // setUniformItemSizes) — that was ruled out on real hardware as the
        // cause of broken drive/folder navigation.
        //
        // The actual cause: QFileDialogPrivate's own navigation slots
        // (_q_enterDirectory and friends) map a clicked view index back to
        // the real QFileSystemModel via a private `proxyModel` pointer that
        // is only populated by QFileDialog::setProxyModel(). Setting
        // listView/treeView's model directly (as this used to do) leaves
        // that private pointer null, so the dialog resolves proxy-model
        // indices against the raw model and silently fails to navigate —
        // clicking anything did nothing. setProxyModel() is the sanctioned
        // API for exactly this (augmenting the model backing the dialog's
        // views) and wires both listView and treeView itself.
        auto* proxy = new ImageThumbnailProxyModel(&dlg);
        dlg.setProxyModel(proxy);

        // QFileSystemModel populates a directory's rows asynchronously —
        // rowCount() reads 0 until its background scan finishes and
        // directoryLoaded() fires. An already-scanned (cached) directory
        // resolves before the view ever paints, so it looks instant; a
        // fresh one can leave the view showing zero rows against a root
        // index that is otherwise perfectly valid, until something else
        // happens to force a re-layout. Re-applying rootIndex is a public,
        // idempotent way to force that re-layout (QAbstractItemView
        // doesn't skip it just because the index value is unchanged).
        if (auto* fsModel = qobject_cast<QFileSystemModel*>(proxy->sourceModel())) {
            QObject::connect(fsModel, &QFileSystemModel::directoryLoaded, &dlg,
                              [listView, treeView](const QString&) {
                                  listView->setRootIndex(listView->rootIndex());
                                  if (treeView)
                                      treeView->setRootIndex(treeView->rootIndex());
                              });
        }

        const QSize defaultIconSize = listView->iconSize();

        // Two mutually-exclusive view buttons (Explorer-style view switcher)
        // rather than one checkable toggle: clicking a button always selects
        // it; clicking the already-active one is a no-op (QButtonGroup
        // exclusivity), so neither button un-selects itself.
        auto* smallIconsButton = new QToolButton(&dlg);
        smallIconsButton->setObjectName(QStringLiteral("imageDialogSmallIconsButton"));
        smallIconsButton->setCheckable(true);
        smallIconsButton->setToolTip(QStringLiteral("Small icons"));
        smallIconsButton->setIcon(dlg.style()->standardIcon(QStyle::SP_FileDialogListView));
        // Tooltips aren't reliably exposed to assistive tech on icon-only
        // buttons; set the accessible name/description explicitly.
        smallIconsButton->setAccessibleName(QStringLiteral("Small icons"));
        smallIconsButton->setAccessibleDescription(
            QStringLiteral("Show the file list with small icons"));
        grid->addWidget(smallIconsButton, 0, grid->columnCount());

        auto* thumbButton = new QToolButton(&dlg);
        thumbButton->setObjectName(QStringLiteral("imageDialogThumbnailToggle"));
        thumbButton->setCheckable(true);
        thumbButton->setToolTip(QStringLiteral("Large thumbnails"));
        thumbButton->setIcon(dlg.style()->standardIcon(QStyle::SP_FileDialogContentsView));
        thumbButton->setAccessibleName(QStringLiteral("Large thumbnails"));
        thumbButton->setAccessibleDescription(
            QStringLiteral("Show the file list with large image thumbnails"));
        grid->addWidget(thumbButton, 0, grid->columnCount());

        auto* viewButtons = new QButtonGroup(&dlg);
        viewButtons->setExclusive(true);
        viewButtons->addButton(smallIconsButton);
        viewButtons->addButton(thumbButton);

        previewLabel = new PreviewLabel(&dlg);
        previewLabel->setObjectName(QStringLiteral("imageDialogPreviewLabel"));
        previewLabel->setMinimumSize(180, 180);
        previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        previewLabel->setAlignment(Qt::AlignCenter);
        previewLabel->setFrameShape(QFrame::StyledPanel);
        previewLabel->setScaledContents(false);
        previewLabel->setAccessibleName(QStringLiteral("Image preview"));
        previewLabel->setAccessibleDescription(
            QStringLiteral("Preview of the currently selected image file"));
        const int previewColumn = grid->columnCount();
        const int previewRowSpan = std::max(1, grid->rowCount() - 1);
        // No alignment argument: passing one (e.g. the old Qt::AlignTop)
        // pins the widget to its size hint within the cell instead of
        // stretching it to fill the cell, which is what let the dialog grow
        // around it without the preview itself growing.
        grid->addWidget(previewLabel, 1, previewColumn, previewRowSpan, 1);
        grid->setColumnStretch(previewColumn, 1);

        auto applyThumbnailMode = [listView, treeView, proxy, defaultIconSize](bool large) {
            proxy->setShowThumbnails(large);
            const QSize size = large ? kThumbnailDecodeSize : defaultIconSize;
            listView->setIconSize(size);
            listView->viewport()->update();
            if (treeView) {
                treeView->setIconSize(size);
                treeView->viewport()->update();
            }
        };

        // Both buttons' toggled(true) fires exactly once per user click (the
        // group's exclusivity fires toggled(false) on the other button too,
        // ignored here) — each branch only acts on becoming the active one.
        QObject::connect(thumbButton, &QToolButton::toggled, &dlg, [applyThumbnailMode](bool on) {
            if (on)
                applyThumbnailMode(true);
        });
        QObject::connect(smallIconsButton, &QToolButton::toggled, &dlg, [applyThumbnailMode](bool on) {
            if (on)
                applyThumbnailMode(false);
        });

        // Always open in Detail + small icons, matching the dialog's
        // pre-#4717 behavior; the view mode isn't persisted across opens,
        // only within the current dialog session via the toggle above.
        smallIconsButton->setChecked(true);
    }

    if (previewLabel) {
        QObject::connect(&dlg, &QFileDialog::currentChanged, &dlg,
                          [previewLabel](const QString& path) { previewLabel->setPreviewPath(path); });
    }

    if (dlg.exec() != QDialog::Accepted)
        return {};
    return dlg.selectedFiles().value(0, QString());
}

} // namespace AetherSDR
