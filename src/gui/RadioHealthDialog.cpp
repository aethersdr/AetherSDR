#include "RadioHealthDialog.h"

#include "CanonIndicators.h"
#include "CanonScrollBarStyle.h"
#include "CanonTableStyle.h"
#include "FramelessResizer.h"
#include "core/ThemeManager.h"
#include "core/backends/HealthSnapshotMerge.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Fast enough to watch a FIFO move or an ADC overload blink, slow enough that
// the table is readable. The underlying telemetry only arrives at 10 Hz
// anyway (MetisClient::kTelemetryMinIntervalMs), so polling faster than this
// would re-render identical values.
constexpr int kRefreshIntervalMs = 500;

// The window's labels and button on the canon palette; the register table
// takes the shared canonTableRules() after it.
constexpr const char* kRadioHealthStyle = R"(
QLabel {
    background: transparent;
    color: {{color.canon.inkSoft}};
}
QLabel#radioHealthIntro,
QLabel#radioHealthStatus {
    color: {{color.canon.muted}};
}
QPushButton {
    color: {{color.canon.cyan}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 5px 12px;
    font-weight: 600;
}
QPushButton:hover {
    background: {{color.canon.nested}};
    color: {{color.canon.aqua}};
}
QPushButton:focus {
    border-color: {{color.canon.aqua}};
}
)";

// Canon ink on the selection band (canonTableRules() sets no text colour) and
// the table's paddings.
constexpr const char* kRadioHealthTableStyle = R"(
QTableView::item:selected {
    color: {{color.canon.ink}};
}
QTableView::item {
    padding: 2px 6px;
}
QHeaderView::section {
    padding: 5px 6px;
}
)";

}  // namespace

RadioHealthDialog::RadioHealthDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(QStringLiteral("Radio Health"), parent, Kind::Workspace)
    , m_model(model)
{
    setObjectName(QStringLiteral("RadioHealthDialog"));
    theme::setContainer(this, QStringLiteral("canon/radioHealth"));
    // Size and position persist under the key it used as a PersistentDialog,
    // so saved geometry carries over; it resizes from every edge.
    setGeometryKey(QStringLiteral("RadioHealthDialogGeometry"));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kRadioHealthStyle) + canonTableRules()
             + QString::fromLatin1(kRadioHealthTableStyle) + canonScrollBarRules();
    });

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(14, 12, 14, 14);
    root->setSpacing(10);
    root->addWidget(makeCanonHeader(QStringLiteral("Radio Health")));

    auto* intro = new QLabel(
        tr("Health and status registers reported by the connected radio. "
           "A dash means the radio has not reported that value — which is not "
           "the same as a value of zero."));
    intro->setObjectName(QStringLiteral("radioHealthIntro"));
    intro->setWordWrap(true);
    root->addWidget(intro);

    m_table = new QTableWidget(0, 2);
    m_table->setObjectName(QStringLiteral("radioHealthTable"));
    m_table->setAccessibleName(tr("Radio health registers"));
    m_table->setHorizontalHeaderLabels({tr("Register"), tr("Value")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    root->addWidget(m_table, 1);

    auto* buttonRow = new QHBoxLayout;
    m_statusLabel = new QLabel;
    m_statusLabel->setObjectName(QStringLiteral("radioHealthStatus"));
    m_statusLabel->setTextFormat(Qt::PlainText);
    buttonRow->addWidget(m_statusLabel, 1);
    auto* copyBtn = new QPushButton(tr("Copy"));
    copyBtn->setObjectName(QStringLiteral("radioHealthCopy"));
    copyBtn->setAutoDefault(false);
    connect(copyBtn, &QPushButton::clicked, this, &RadioHealthDialog::copyToClipboard);
    buttonRow->addWidget(copyBtn);
    root->addLayout(buttonRow);

    // 40 px taller than before for the canon header, and never narrower than
    // the layout needs.
    setMinimumSize(QSize(460, 500).expandedTo(minimumSizeHint()));
    resize(520, 660);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshIntervalMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &RadioHealthDialog::refresh);
}

bool RadioHealthDialog::isRefreshing() const
{
    return m_refreshTimer->isActive();
}

// Poll only while shown. Esc and Ctrl+W close through QDialog::done(), which
// hides without a close event, so the hide is where polling stops.
void RadioHealthDialog::showEvent(QShowEvent* event)
{
    CanonWindow::showEvent(event);
    refresh();
    m_refreshTimer->start();
}

void RadioHealthDialog::hideEvent(QHideEvent* event)
{
    m_refreshTimer->stop();
    CanonWindow::hideEvent(event);
}

QString RadioHealthDialog::formatValue(const QVariant& v)
{
    if (!v.isValid())
        return QStringLiteral("—");
    // Booleans FIRST: QVariant::Bool would otherwise fall through to the
    // numeric branch and render an ADC overload as "1", which reads like a
    // count rather than a flag.
    if (v.typeId() == QMetaType::Bool)
        return v.toBool() ? QObject::tr("Yes") : QObject::tr("No");
    if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float)
        return QString::number(v.toDouble(), 'f', 2);
    return v.toString();
}

void RadioHealthDialog::refresh()
{
    if (!m_model)
        return;
    // Same merge as the bridge (backends/HealthSnapshotMerge.h): offline rows
    // are the base and in-band rows win. The backend blanks in-band rows when
    // the stream is not delivering, so unmerged they would show dashes exactly
    // when the offline source matters. offlineHealthRows() is non-const on
    // purpose: reading rows is the poller's demand signal.
    const IRadioBackend::HealthSnapshot snap =
        m_model->hasOfflineHealth()
            ? mergeHealthSnapshots(m_model->offlineHealthRows(),
                                   m_model->backendHealthSnapshot())
            : m_model->backendHealthSnapshot();

    if (snap.isEmpty()) {
        m_table->setRowCount(0);
        m_currentKeys.clear();
        m_statusLabel->setText(
            m_model->isConnected()
                ? tr("This radio reports no health registers.")
                : tr("Not connected."));
        return;
    }
    m_statusLabel->setText(tr("Updating every %1 ms").arg(kRefreshIntervalMs));

    // Rebuild the rows only when the set of keys changes. On a steady snapshot
    // this runs once and every later refresh just rewrites the value column,
    // which is what keeps the operator's scroll position and selection intact
    // while they are reading.
    if (snap.order != m_currentKeys) {
        m_currentKeys = snap.order;
        m_table->setRowCount(0);
        int row = 0;
        for (const QString& key : snap.order) {
            if (const auto sectionIt = snap.sections.constFind(key);
                sectionIt != snap.sections.constEnd()) {
                m_table->insertRow(row);
                auto* header = new QTableWidgetItem(sectionIt.value());
                QFont f = header->font();
                f.setBold(true);
                header->setFont(f);
                // Section headers carry no key, so the value-column writer
                // below can tell them apart from real rows and skip them.
                m_table->setItem(row, 0, header);
                m_table->setItem(row, 1, new QTableWidgetItem(QString()));
                ++row;
            }
            m_table->insertRow(row);
            auto* label = new QTableWidgetItem(snap.labels.value(key, key));
            label->setData(Qt::UserRole, key);
            m_table->setItem(row, 0, label);
            m_table->setItem(row, 1, new QTableWidgetItem(QString()));
            ++row;
        }
    }

    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* label = m_table->item(row, 0);
        if (!label)
            continue;
        const QString key = label->data(Qt::UserRole).toString();
        if (key.isEmpty())
            continue;                    // section header
        m_table->item(row, 1)->setText(formatValue(snap.values.value(key)));
    }
}

void RadioHealthDialog::copyToClipboard()
{
    QStringList lines;
    lines << QStringLiteral("AetherSDR — Radio Health");
    for (int row = 0; row < m_table->rowCount(); ++row) {
        QTableWidgetItem* label = m_table->item(row, 0);
        QTableWidgetItem* value = m_table->item(row, 1);
        if (!label)
            continue;
        if (label->data(Qt::UserRole).toString().isEmpty())
            lines << QString() << label->text();          // section heading
        else
            lines << QStringLiteral("  %1: %2")
                         .arg(label->text(), value ? value->text() : QString());
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
    m_statusLabel->setText(tr("Copied to clipboard"));
}

} // namespace AetherSDR
