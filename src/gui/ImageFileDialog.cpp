#include "ImageFileDialog.h"

#include "core/AppSettings.h"

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
#include <QSet>
#include <QSize>
#include <QStringList>
#include <QStyle>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include <algorithm>

namespace AetherSDR {

namespace {

// Mirrors the decompression-bomb guard added for #3990
// (CallsignCard::setPhotoPath) — a small file can still claim a huge pixel
// count, which would freeze/OOM the GUI on decode.
constexpr int kMaxDecodeDimension = 4096;
const QSize kThumbnailDecodeSize(96, 96);

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
    reader.setScaledSize(targetSize.expandedTo(QSize(1, 1)));
    return reader.read();
}

void updatePreview(QLabel* label, const QString& path)
{
    if (path.isEmpty() || !isSupportedImage(path)) {
        label->clear();
        return;
    }
    const QImage img = decodeThumbnail(path, label->size());
    if (img.isNull()) {
        label->clear();
        return;
    }
    label->setPixmap(QPixmap::fromImage(img));
}

// Wraps QFileDialog's internal QFileSystemModel to supply real image
// thumbnails for Qt::DecorationRole instead of the generic per-filetype
// icon. QFileDialog::ViewMode has no native thumbnail mode (#4717) — this
// proxy is what makes IconMode on the internal list view show anything
// useful. Decoding happens off the GUI thread; results land in a bounded,
// mtime-keyed QPixmapCache.
class ImageThumbnailProxyModel : public QIdentityProxyModel
{
public:
    explicit ImageThumbnailProxyModel(QObject* parent)
        : QIdentityProxyModel(parent)
    {
    }

    QVariant data(const QModelIndex& index, int role) const override
    {
        if (role != Qt::DecorationRole)
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
};

} // namespace

QString getBackgroundImagePath(QWidget* parent, const QString& caption)
{
    QFileDialog dlg(parent, caption, QString(), buildImageFilter());
    dlg.setFileMode(QFileDialog::ExistingFile);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setAcceptMode(QFileDialog::AcceptOpen);

    // Best-effort augmentation: if QFileDialog's internal layout/widget
    // names ever change under us, fall back to the plain (wider-filter)
    // dialog rather than crashing.
    auto* grid = qobject_cast<QGridLayout*>(dlg.layout());
    auto* listView = dlg.findChild<QListView*>(QStringLiteral("listView"));
    QLabel* previewLabel = nullptr;

    if (grid && listView) {
        auto* proxy = new ImageThumbnailProxyModel(&dlg);
        proxy->setSourceModel(listView->model());
        listView->setModel(proxy);

        auto* thumbButton = new QToolButton(&dlg);
        thumbButton->setObjectName(QStringLiteral("imageDialogThumbnailToggle"));
        thumbButton->setCheckable(true);
        thumbButton->setToolTip(QStringLiteral("Thumbnail view"));
        thumbButton->setIcon(dlg.style()->standardIcon(QStyle::SP_FileDialogContentsView));
        grid->addWidget(thumbButton, 0, grid->columnCount());

        previewLabel = new QLabel(&dlg);
        previewLabel->setObjectName(QStringLiteral("imageDialogPreviewLabel"));
        previewLabel->setFixedSize(180, 180);
        previewLabel->setAlignment(Qt::AlignCenter);
        previewLabel->setFrameShape(QFrame::StyledPanel);
        previewLabel->setScaledContents(false);
        const int previewRowSpan = std::max(1, grid->rowCount() - 1);
        grid->addWidget(previewLabel, 1, grid->columnCount(), previewRowSpan, 1, Qt::AlignTop);

        auto applyViewMode = [&dlg, listView](bool thumbnails) {
            if (thumbnails) {
                dlg.setViewMode(QFileDialog::List);
                listView->setViewMode(QListView::IconMode);
                listView->setIconSize(kThumbnailDecodeSize);
                listView->setGridSize(kThumbnailDecodeSize + QSize(16, 16));
                listView->setWrapping(true);
                listView->setResizeMode(QListView::Adjust);
                listView->setFlow(QListView::LeftToRight);
                listView->setUniformItemSizes(true);
            } else {
                listView->setViewMode(QListView::ListMode);
                listView->setFlow(QListView::TopToBottom);
                listView->setWrapping(false);
                listView->setGridSize(QSize());
            }
        };

        QObject::connect(thumbButton, &QToolButton::toggled, &dlg, [applyViewMode](bool on) {
            applyViewMode(on);
            auto& s = AppSettings::instance();
            s.setValue(QStringLiteral("ImageFileDialog/ThumbnailView"),
                       on ? QStringLiteral("True") : QStringLiteral("False"));
            s.save();
        });

        auto& s = AppSettings::instance();
        const bool persisted = s.value(QStringLiteral("ImageFileDialog/ThumbnailView"),
                                        QStringLiteral("False")).toString()
            == QStringLiteral("True");
        thumbButton->setChecked(persisted); // fires toggled() above when true
    }

    if (previewLabel) {
        QObject::connect(&dlg, &QFileDialog::currentChanged, &dlg,
                          [previewLabel](const QString& path) { updatePreview(previewLabel, path); });
    }

    if (dlg.exec() != QDialog::Accepted)
        return {};
    return dlg.selectedFiles().value(0, QString());
}

} // namespace AetherSDR
