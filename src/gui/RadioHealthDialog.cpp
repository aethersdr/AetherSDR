#include "RadioHealthDialog.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
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

}  // namespace

RadioHealthDialog::RadioHealthDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(QStringLiteral("Radio Health"),
                       QStringLiteral("RadioHealthDialogGeometry"), parent)
    , m_model(model)
{
    theme::setContainer(this, QStringLiteral("dialog/radioHealth"));
    setMinimumSize(460, 460);
    resize(520, 620);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setSpacing(8);

    auto* intro = new QLabel(
        tr("Health and status registers reported by the connected radio. "
           "A dash means the radio has not reported that value — which is not "
           "the same as a value of zero."));
    intro->setWordWrap(true);
    ThemeManager::instance().applyStyleSheet(intro,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    root->addWidget(intro);

    m_table = new QTableWidget(0, 2);
    m_table->setHorizontalHeaderLabels({tr("Register"), tr("Value")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    ThemeManager::instance().applyStyleSheet(m_table, "QTableWidget {"
        "  background: {{color.background.0}};"
        "  color: {{color.text.primary}};"
        "  border: 1px solid {{color.background.2}};"
        "  font-size: 11px;"
        "}"
        "QHeaderView::section {"
        "  background: {{color.background.1}};"
        "  color: {{color.text.secondary}};"
        "  border: 0px;"
        "  padding: 4px;"
        "}");
    root->addWidget(m_table, 1);

    auto* buttonRow = new QHBoxLayout;
    m_statusLabel = new QLabel;
    ThemeManager::instance().applyStyleSheet(m_statusLabel,
        "QLabel { color: {{color.text.secondary}}; font-size: 11px; }");
    buttonRow->addWidget(m_statusLabel, 1);
    auto* copyBtn = new QPushButton(tr("Copy"));
    copyBtn->setAutoDefault(false);
    connect(copyBtn, &QPushButton::clicked, this, &RadioHealthDialog::copyToClipboard);
    buttonRow->addWidget(copyBtn);
    auto* closeBtn = new QPushButton(tr("Close"));
    closeBtn->setAutoDefault(false);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    buttonRow->addWidget(closeBtn);
    root->addLayout(buttonRow);

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshIntervalMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &RadioHealthDialog::refresh);
    m_refreshTimer->start();
    refresh();
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
    const IRadioBackend::HealthSnapshot snap = m_model->backendHealthSnapshot();

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
