#include "KiwiPublicReceiverPicker.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// Session-scoped cache of the last successful directory fetch, shared across
// every picker instance opened this run.  Being a process-static, it is NOT
// persisted to disk — it dies with the app, so a new session always starts
// fresh.
//
// The client only ever talks to AetherSDR's own CDN, which is built to be hit,
// so repeat "Browse public…" opens may refresh — but no faster than the mirror
// itself updates (cache-control: max-age=1800).  Inside that window we re-serve
// the cache; past it the next open fetches again.  "Refresh list" always
// fetches and overwrites.
QVector<KiwiPublicReceiver> g_sessionCache;
bool g_haveSessionCache = false;
QDateTime g_cacheFetchedAt;    // mirror's own fetched_at for the cached list
QDateTime g_cacheReceivedAt;   // when WE last pulled it (local UTC clock)

bool cacheIsFresh()
{
    if (!g_haveSessionCache || !g_cacheReceivedAt.isValid())
        return false;
    const qint64 age = g_cacheReceivedAt.secsTo(QDateTime::currentDateTimeUtc());
    return age >= 0 && age < KiwiPublicDirectory::kMinRefreshSeconds;
}

enum Column {
    ReceiverColumn,
    LocationColumn,
    UsersColumn,
    ApiColumn,
    LimitsColumn,
    ColumnCount,
};

// "http://host:port" -> "host:port" (what KiwiSdrClient::normalizeEndpoint wants).
QString endpointFromUrl(const QString& url)
{
    const QUrl u(url);
    QString ep = u.host();
    if (u.port() > 0)
        ep += QStringLiteral(":") + QString::number(u.port());
    return ep.isEmpty() ? url : ep;
}
} // namespace

KiwiPublicReceiverPicker::KiwiPublicReceiverPicker(QWidget* parent)
    : PersistentDialog(tr("Browse public KiwiSDR receivers"),
                       QStringLiteral("KiwiPublicReceiverPickerGeometry"), parent)
    , m_dir(new KiwiPublicDirectory(this))
{
    resize(760, 460);

    auto* outer = new QVBoxLayout(bodyWidget());

    auto* topRow = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Filter by name, location, or host…"));
    m_search->setClearButtonEnabled(true);
    topRow->addWidget(m_search, 1);
    m_refresh = new QPushButton(tr("Refresh list"));
    m_refresh->setToolTip(tr("Re-fetch the public directory from the network."));
    topRow->addWidget(m_refresh);
    outer->addLayout(topRow);

    m_table = new QTableWidget(0, ColumnCount, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Receiver"), tr("Location"), tr("Users"), tr("API"), tr("Limits")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(ReceiverColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(LocationColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(UsersColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ApiColumn, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(LimitsColumn, QHeaderView::ResizeToContents);
    outer->addWidget(m_table, 1);

    m_status = new QLabel(tr("Loading public receivers…"));
    m_status->setStyleSheet("QLabel { color: #8ea8c0; }");
    outer->addWidget(m_status);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    m_ok = buttons->button(QDialogButtonBox::Ok);
    m_ok->setText(tr("Add selected"));
    m_ok->setEnabled(false);
    outer->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &KiwiPublicReceiverPicker::acceptCurrentRow);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_table, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem*) { acceptCurrentRow(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            [this] { m_ok->setEnabled(!m_table->selectedItems().isEmpty()); });
    connect(m_search, &QLineEdit::textChanged, this, &KiwiPublicReceiverPicker::applyFilter);
    connect(m_refresh, &QPushButton::clicked, this, &KiwiPublicReceiverPicker::startFetch);

    connect(m_dir, &KiwiPublicDirectory::ready, this,
            [this](const QVector<KiwiPublicReceiver>& receivers, const QDateTime& fetchedAt) {
                g_cacheReceivedAt = QDateTime::currentDateTimeUtc();
                onReady(receivers, fetchedAt);
            });
    connect(m_dir, &KiwiPublicDirectory::failed, this, [this](const QString& err) {
        m_refresh->setEnabled(true);
        // No origin fallback by design (see KiwiPublicDirectory's good-citizen
        // contract): show whatever list we already have, or say plainly that we
        // have none.
        if (g_haveSessionCache) {
            m_fromCache = true;
            // Held in a member, not appended to the label: applyFilter()
            // rebuilds the status from scratch on every keystroke.
            m_refreshError = err;
            onReady(g_sessionCache, g_cacheFetchedAt);
        } else {
            m_status->setText(tr("Receiver directory unavailable (%1) — "
                                 "try again later.").arg(err));
        }
    });

    // Re-serve the session cache while it is still inside the mirror's own
    // 30-minute refresh window; otherwise fetch.  "Refresh list" always fetches.
    if (cacheIsFresh()) {
        m_fromCache = true;
        onReady(g_sessionCache, g_cacheFetchedAt);
    } else {
        startFetch();
    }
}

void KiwiPublicReceiverPicker::startFetch()
{
    m_fromCache = false;
    m_refreshError.clear();
    m_status->setText(tr("Loading public receivers…"));
    m_refresh->setEnabled(false);
    m_dir->fetch();
}

void KiwiPublicReceiverPicker::onReady(const QVector<KiwiPublicReceiver>& receivers,
                                       const QDateTime& fetchedAt)
{
    // Populate (or overwrite) the session cache from every successful fetch.
    // Serving from the cache passes the same vector straight back through here,
    // which is a harmless no-op assignment.
    g_sessionCache = receivers;
    g_cacheFetchedAt = fetchedAt;
    g_haveSessionCache = true;
    m_fetchedAt = fetchedAt;

    m_refresh->setEnabled(true);
    m_apiReceivers.clear();
    m_hiddenWebOnly = 0;
    m_hiddenUnknown = 0;
    m_hiddenFlagged = 0;
    // Honor the operator: only receivers that allow the external API are
    // listed. Web-only (ext_api == 0) are excluded entirely, as are receivers
    // that don't publish a policy (we can't confirm API is OK) and entries the
    // origin has flagged. The policy itself lives on KiwiPublicReceiver so the
    // test locks the same code this loop runs.
    for (const auto& r : receivers) {
        switch (r.offerDecision()) {
            case KiwiPublicReceiver::Offer::Yes:                 m_apiReceivers.push_back(r); break;
            case KiwiPublicReceiver::Offer::HiddenFlagged:       ++m_hiddenFlagged; break;
            case KiwiPublicReceiver::Offer::HiddenWebOnly:       ++m_hiddenWebOnly; break;
            case KiwiPublicReceiver::Offer::HiddenPolicyUnknown: ++m_hiddenUnknown; break;
            case KiwiPublicReceiver::Offer::HiddenOffline:       break;
        }
    }
    applyFilter();
}

void KiwiPublicReceiverPicker::applyFilter()
{
    const QString needle = m_search->text().trimmed();
    m_table->setRowCount(0);
    int shown = 0;
    for (const auto& r : m_apiReceivers) {
        if (!needle.isEmpty()
            && !r.name.contains(needle, Qt::CaseInsensitive)
            && !r.location.contains(needle, Qt::CaseInsensitive)
            && !r.url.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto* nameItem = new QTableWidgetItem(r.name.isEmpty() ? r.url : r.name);
        nameItem->setToolTip(r.url);
        // Carry the endpoint + a suggested name on the row's first item.
        nameItem->setData(Qt::UserRole, endpointFromUrl(r.url));
        nameItem->setData(Qt::UserRole + 1,
                          QUrl(r.url).host().left(16));  // short default name
        m_table->setItem(row, ReceiverColumn, nameItem);
        m_table->setItem(row, LocationColumn, new QTableWidgetItem(r.location));
        m_table->setItem(row, UsersColumn, new QTableWidgetItem(
            QStringLiteral("%1/%2").arg(r.users).arg(r.usersMax)));
        m_table->setItem(row, ApiColumn, new QTableWidgetItem(r.apiBadge()));

        auto* limitsItem = new QTableWidgetItem(r.connectionLimitBadge());
        if (r.advertisesConnectionLimit()) {
            limitsItem->setToolTip(tr("This receiver advertises connection limits, "
                                      "but the public directory does not publish "
                                      "the configured duration."));
        } else {
            limitsItem->setToolTip(tr("No connection limit is advertised in the "
                                      "public directory."));
        }
        m_table->setItem(row, LimitsColumn, limitsItem);
        ++shown;
    }
    QString status = tr("%1 receivers allow API access").arg(shown);
    QStringList hiddenParts;
    if (m_hiddenWebOnly > 0)
        hiddenParts << tr("%1 web-only").arg(m_hiddenWebOnly);
    if (m_hiddenUnknown > 0)
        hiddenParts << tr("%1 policy-unknown").arg(m_hiddenUnknown);
    if (m_hiddenFlagged > 0)
        hiddenParts << tr("%1 flagged").arg(m_hiddenFlagged);
    if (!hiddenParts.isEmpty())
        status += tr(" (%1 hidden)").arg(hiddenParts.join(QStringLiteral(", ")));
    if (m_fromCache)
        status += tr("  ·  cached — use “Refresh list” to update");
    // Surface the list's age only once it is genuinely old: the mirror refreshes
    // hourly, so anything inside its own staleness threshold is unremarkable.
    if (m_fetchedAt.isValid()) {
        const qint64 mins = m_fetchedAt.secsTo(QDateTime::currentDateTimeUtc()) / 60;
        if (mins >= KiwiPublicDirectory::kStaleAfterMinutes) {
            status += (mins >= 2 * 60 * 24)
                ? tr("  ·  list is %1 days old").arg(mins / (60 * 24))
                : tr("  ·  list is %1 hours old").arg(mins / 60);
        }
    }
    if (!m_refreshError.isEmpty())
        status += tr("  ·  could not refresh: %1").arg(m_refreshError);
    m_status->setText(status);
    m_ok->setEnabled(!m_table->selectedItems().isEmpty());
}

void KiwiPublicReceiverPicker::acceptCurrentRow()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = m_table->item(row, ReceiverColumn);
    if (!item) return;
    m_selectedEndpoint = item->data(Qt::UserRole).toString();
    m_selectedName = item->data(Qt::UserRole + 1).toString();
    accept();
}

} // namespace AetherSDR
