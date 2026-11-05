#include "WsprConfigureWindow.h"

#include "CanonIndicators.h"
#include "CanonTableStyle.h"

#include "core/TailnetShimTokenStore.h"
#include "core/ThemeManager.h"
#include "core/WsprAgentClient.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The canon dialog style, as the Remote Access window: cards on the nested
// surface, keys in muted, values in ink-soft, Apply as the brand-gradient
// primary. Rules are scoped by object name or the wsRole property so the
// card rule never reaches the QLabels inside it.
constexpr const char* kWsprStyle =
    "QWidget#wsprBody { background: transparent; }"
    "QLabel { color: {{color.canon.inkSoft}}; background: transparent; border: none; font-size: 12px; }"
    "QLabel#wsTitle { color: {{color.canon.ink}}; font-size: 20px; font-weight: bold; }"
    "QLabel#wsStatusLine { color: {{color.canon.muted}}; font-size: 13px; }"
    "QLabel#wsStatusDot { background: {{color.canon.muted}}; border-radius: 4px;"
    " min-width: 8px; max-width: 8px; min-height: 8px; max-height: 8px; }"
    "QLabel#wsStatusDot[state=\"running\"] { background: {{color.canon.cyan}}; }"
    "QLabel#wsStatusDot[state=\"starting\"] { background: {{color.accent.warning}}; }"
    "QLabel#wsStatusDot[state=\"error\"] { background: {{color.accent.danger}}; }"
    "QFrame#wsRule { background: {{color.canon.line}}; border: none; min-height: 1px; max-height: 1px; }"
    "QFrame[wsRole=\"card\"] { background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}};"
    " border-radius: 12px; }"
    "QLabel[wsRole=\"cardTitle\"] { color: {{color.canon.ink}}; font-size: 13px; font-weight: bold; }"
    "QLabel[wsRole=\"key\"] { color: {{color.canon.muted}}; font-size: 10px; font-weight: bold; }"
    "QLabel[wsRole=\"hint\"] { color: {{color.canon.muted}}; font-size: 11px; }"
    "QLabel[wsRole=\"message\"][tone=\"error\"] { color: {{color.accent.danger}}; }"
    "QLabel[wsRole=\"message\"][tone=\"ok\"] { color: {{color.accent.success}}; }"
    "QLineEdit, QSpinBox, QComboBox { background: {{color.canon.control}}; color: {{color.canon.ink}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 4px 8px; font-size: 12px;"
    " selection-background-color: {{color.canon.cyan}}; selection-color: {{color.canon.onAccent}}; }"
    "QLineEdit:focus, QSpinBox:focus, QComboBox:focus, QComboBox:hover { border-color: {{color.canon.aqua}}; }"
    "QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled { color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }"
    "QComboBox::drop-down { border: 0; width: 18px; }"
    "QComboBox QAbstractItemView { color: {{color.canon.inkSoft}}; background: {{color.canon.nested}};"
    " border: 1px solid {{color.canon.lineHi}}; selection-background-color: {{color.canon.cyan}};"
    " selection-color: {{color.canon.onAccent}}; }"
    "QCheckBox { color: {{color.canon.inkSoft}}; background: transparent; font-size: 12px; spacing: 8px; }"
    "QCheckBox:disabled { color: {{color.canon.muted}}; }"
    "QTableWidget#wsHopTable { background: {{color.canon.control}}; border: 1px solid {{color.canon.line}};"
    " border-radius: 6px; }"
    "QTableWidget#wsHopTable:disabled { color: {{color.canon.muted}}; }"
    "QPushButton { background: {{color.canon.control}}; color: {{color.canon.cyan}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 6px 16px;"
    " font-size: 12px; font-weight: bold; }"
    "QPushButton:hover { background: {{color.canon.nested}}; color: {{color.canon.aqua}}; }"
    "QPushButton:focus { border-color: {{color.canon.aqua}}; }"
    "QPushButton:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }"
    "QPushButton#wsApply { background: {{color.brand.gradient}}; color: {{color.canon.onAccent}};"
    " border: 1px solid transparent; border-radius: 4px; padding: 6px 30px; }"
    "QPushButton#wsApply:hover { border-color: {{color.canon.aqua}}; }"
    "QPushButton#wsApply:focus { border: 2px solid {{color.canon.aqua}}; padding: 5px 29px; }"
    "QPushButton#wsApply:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }";

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

QLabel* roleLabel(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("wsRole", QString::fromLatin1(role));
    return label;
}

QFrame* makeCard(const QString& title, QWidget* parent, QVBoxLayout** contents)
{
    auto* card = new QFrame(parent);
    card->setProperty("wsRole", QStringLiteral("card"));
    card->setAccessibleName(title);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(16, 14, 16, 16);
    v->setSpacing(10);
    v->addWidget(roleLabel(title, "cardTitle", card));
    *contents = v;
    return card;
}

QString periodName(int period)
{
    switch (period) {
    case WsprConfig::Sunrise: return QObject::tr("Sunrise gray line");
    case WsprConfig::Day: return QObject::tr("Day");
    case WsprConfig::Sunset: return QObject::tr("Sunset gray line");
    case WsprConfig::Night: return QObject::tr("Night");
    default: return {};
    }
}

QString bandDisplay(const QString& band)
{
    // "20m" -> "20 m", as the rest of the app writes bands.
    return band.endsWith(QLatin1Char('m')) ? band.chopped(1) + QStringLiteral(" m") : band;
}

// What the receiver is doing, in words.
QString stateText(const WsprStatus& st)
{
    const QString band = bandDisplay(st.band);
    if (st.state == QLatin1String("capturing")) {
        return QObject::tr("Listening on %1").arg(band);
    }
    if (st.state == QLatin1String("decoding")) {
        return QObject::tr("Decoding %1").arg(band);
    }
    if (st.state == QLatin1String("waiting_cycle")) {
        return QObject::tr("Waiting for the next cycle on %1").arg(band);
    }
    if (st.state == QLatin1String("waiting_clock")) {
        return QObject::tr("Waiting for the radio's clock");
    }
    if (st.state == QLatin1String("starting_audio")) {
        return QObject::tr("Starting the receiver's audio");
    }
    if (st.state == QLatin1String("connecting")) {
        return QObject::tr("Connecting to the radio");
    }
    return QObject::tr("Starting");
}

}  // namespace

WsprConfigureWindow::WsprConfigureWindow(RadioModel* model, QWidget* parent)
    : CanonWindow(tr("WSPR Receiver"), parent)
    , m_model(model)
{
    constexpr int kWidth = 760;
    constexpr int kSide = 28;
    setFixedWidth(kWidth);
    bodyWidget()->setObjectName(QStringLiteral("wsprBody"));
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kWsprStyle) + canonIndicatorRules() + canonSpinArrowRules()
             + canonItemCheckRules(QStringLiteral("QTableWidget")) + canonTableRules();
    });

    QWidget* body = bodyWidget();
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(kSide, 26, kSide, 22);
    layout->setSpacing(14);

    // Header: title, then a status line led by a dot (the line always names
    // the state), then a hairline.
    const QString radioName = m_model ? m_model->nickname() : QString();
    auto* title = new QLabel(radioName.isEmpty() ? tr("WSPR Receiver") : tr("WSPR Receiver — %1").arg(radioName), body);
    title->setObjectName(QStringLiteral("wsTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    statusRow->addStretch(1);
    m_statusDot = new QLabel(body);
    m_statusDot->setObjectName(QStringLiteral("wsStatusDot"));
    m_statusDot->setAccessibleName(tr("WSPR receiver status indicator"));
    m_stateLabel = new QLabel(tr("Checking…"), body);
    m_stateLabel->setObjectName(QStringLiteral("wsStatusLine"));
    m_stateLabel->setAccessibleName(tr("WSPR receiver status"));
    statusRow->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_stateLabel);
    statusRow->addStretch(1);
    layout->addLayout(statusRow);
    auto* rule = new QFrame(body);
    rule->setObjectName(QStringLiteral("wsRule"));
    layout->addWidget(rule);

    auto addKey = [body](QGridLayout* grid, int row, int col, const QString& key, QWidget* field) {
        grid->addWidget(roleLabel(key, "key", body), row, col, Qt::AlignLeft | Qt::AlignVCenter);
        grid->addWidget(field, row, col + 1);
    };

    // ── Station ──────────────────────────────────────────────────────────
    QVBoxLayout* station = nullptr;
    layout->addWidget(makeCard(tr("Station"), body, &station));
    auto* sgrid = new QGridLayout;
    sgrid->setHorizontalSpacing(16);
    sgrid->setVerticalSpacing(8);
    sgrid->setColumnStretch(1, 1);
    sgrid->setColumnStretch(3, 1);
    m_callsignEdit = new QLineEdit(body);
    m_callsignEdit->setAccessibleName(tr("Callsign"));
    m_callsignEdit->setAccessibleDescription(tr("Leave empty to use the radio's callsign."));
    m_callsignEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Za-z0-9/]{0,15}")), m_callsignEdit));
    m_gridEdit = new QLineEdit(body);
    m_gridEdit->setAccessibleName(tr("Grid square"));
    m_gridEdit->setAccessibleDescription(tr("Leave empty to use the radio's GPS grid."));
    m_gridEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[A-Ra-r]{2}[0-9]{2}([A-Xa-x]{2})?")), m_gridEdit));
    addKey(sgrid, 0, 0, tr("CALLSIGN"), m_callsignEdit);
    addKey(sgrid, 0, 2, tr("GRID"), m_gridEdit);
    station->addLayout(sgrid);

    // ── Receive ──────────────────────────────────────────────────────────
    QVBoxLayout* receive = nullptr;
    layout->addWidget(makeCard(tr("Receive"), body, &receive));
    auto* rgrid = new QGridLayout;
    rgrid->setHorizontalSpacing(16);
    rgrid->setVerticalSpacing(8);
    rgrid->setColumnStretch(1, 1);
    rgrid->setColumnStretch(3, 1);
    m_bandCombo = new QComboBox(body);
    m_bandCombo->setAccessibleName(tr("Band"));
    m_bandCombo->setAccessibleDescription(tr("The band the receiver stays on when band hopping is off."));
    for (const QString& b : wspr::bands()) {
        m_bandCombo->addItem(bandDisplay(b), b);
    }
    m_antennaCombo = new QComboBox(body);
    m_antennaCombo->setAccessibleName(tr("Receive antenna"));
    m_antennaCombo->addItem(tr("Radio default"), QString());
    if (m_model) {
        for (const QString& ant : m_model->antennaList()) {
            m_antennaCombo->addItem(m_model->antennaDisplayName(ant), ant);
        }
    }
    m_depthCombo = new QComboBox(body);
    m_depthCombo->setAccessibleName(tr("Decode depth"));
    const QStringList depthLabels{tr("Standard"), tr("Fast"), tr("Normal"), tr("Deep")};
    const QStringList depthTips{
        tr("wsprd's own settings. It decoded the most weak signals in testing."),
        tr("WSJT-X's Fast: two passes, the quickest."),
        tr("WSJT-X's Normal: three passes and the ordered-statistics decoder."),
        tr("WSJT-X's Deep: as Normal, with more candidate signals.")};
    for (int i = 0; i < wspr::depths().size(); ++i) {
        m_depthCombo->addItem(depthLabels.value(i), wspr::depths().at(i));
        m_depthCombo->setItemData(i, depthTips.value(i), Qt::ToolTipRole);
    }
    m_daxCombo = new QComboBox(body);
    m_daxCombo->setAccessibleName(tr("DAX channel"));
    m_daxCombo->setAccessibleDescription(tr("The DAX audio channel the receiver uses for its own slice."));
    for (int ch = 1; ch <= 4; ++ch) {
        m_daxCombo->addItem(QString::number(ch), ch);
    }
    addKey(rgrid, 0, 0, tr("BAND"), m_bandCombo);
    addKey(rgrid, 0, 2, tr("ANTENNA"), m_antennaCombo);
    addKey(rgrid, 1, 0, tr("DECODE DEPTH"), m_depthCombo);
    addKey(rgrid, 1, 2, tr("DAX CHANNEL"), m_daxCombo);
    receive->addLayout(rgrid);

    // ── Band hopping ─────────────────────────────────────────────────────
    QVBoxLayout* hopping = nullptr;
    layout->addWidget(makeCard(tr("Band hopping"), body, &hopping));
    auto* hopRow = new QHBoxLayout;
    m_hopCheck = new QCheckBox(tr("Hop bands"), body);
    m_hopCheck->setAccessibleDescription(
        tr("Follow WSJT-X's coordinated band hopping, on the bands ticked for each time of day."));
    hopRow->addWidget(m_hopCheck);
    hopRow->addStretch(1);
    hopRow->addWidget(roleLabel(tr("GRAY TIME"), "key", body));
    m_graySpin = new QSpinBox(body);
    m_graySpin->setRange(1, 120);   // WSJT-X's range
    m_graySpin->setSuffix(tr(" min"));
    m_graySpin->setAccessibleName(tr("Gray line width"));
    m_graySpin->setAccessibleDescription(tr("How long each sunrise and sunset gray line period lasts."));
    hopRow->addWidget(m_graySpin);
    hopping->addLayout(hopRow);
    m_hopTable = new QTableWidget(WsprConfig::PeriodCount, wspr::bands().size(), body);
    m_hopTable->setObjectName(QStringLiteral("wsHopTable"));
    m_hopTable->setAccessibleName(tr("Bands to hop to at each time of day"));
    m_hopTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_hopTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hopTable->setFocusPolicy(Qt::StrongFocus);
    m_hopTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_hopTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_hopTable->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hopTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QStringList bandHeaders;
    for (const QString& b : wspr::bands()) {
        bandHeaders << b.chopped(1);   // the header row reads as metres
    }
    m_hopTable->setHorizontalHeaderLabels(bandHeaders);
    QStringList periodHeaders;
    for (int p = 0; p < WsprConfig::PeriodCount; ++p) {
        periodHeaders << periodName(p);
        for (int c = 0; c < wspr::bands().size(); ++c) {
            auto* item = new QTableWidgetItem;
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
            item->setData(Qt::AccessibleTextRole,
                          tr("%1 at %2").arg(bandDisplay(wspr::bands().at(c)), periodName(p)));
            m_hopTable->setItem(p, c, item);
        }
    }
    m_hopTable->setVerticalHeaderLabels(periodHeaders);
    m_hopTable->setFixedHeight(m_hopTable->horizontalHeader()->sizeHint().height()
                               + WsprConfig::PeriodCount * m_hopTable->verticalHeader()->defaultSectionSize() + 4);
    hopping->addWidget(m_hopTable);
    auto* hopHint = roleLabel(tr("Every two minutes WSJT-X's schedule names one of 160 m to 10 m. The receiver "
                                 "goes there if it is ticked for the time of day, and otherwise to another "
                                 "ticked band. With nothing ticked it stays on its band."),
                              "hint", body);
    hopHint->setWordWrap(true);
    hopping->addWidget(hopHint);

    // ── Upload ───────────────────────────────────────────────────────────
    QVBoxLayout* upload = nullptr;
    layout->addWidget(makeCard(tr("Upload"), body, &upload));
    m_uploadCheck = new QCheckBox(tr("Upload spots to WSPRnet"), body);
    m_uploadCheck->setAccessibleDescription(
        tr("The receiver sends each cycle's spots to wsprnet.org under your callsign and grid."));
    upload->addWidget(m_uploadCheck);
    m_uploadStatus = roleLabel(QString(), "hint", body);
    m_uploadStatus->setAccessibleName(tr("Upload status"));
    upload->addWidget(m_uploadStatus);

    m_messageLabel = roleLabel(QString(), "message", body);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setAccessibleName(tr("WSPR receiver message"));
    layout->addWidget(m_messageLabel);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), body);
    cancel->setAccessibleName(tr("Close without saving"));
    connect(cancel, &QPushButton::clicked, this, &QWidget::close);
    m_applyButton = new QPushButton(tr("Apply"), body);
    m_applyButton->setObjectName(QStringLiteral("wsApply"));
    m_applyButton->setAccessibleName(tr("Save these settings on the radio"));
    m_applyButton->setDefault(true);
    connect(m_applyButton, &QPushButton::clicked, this, &WsprConfigureWindow::apply);
    buttons->addWidget(cancel);
    buttons->addWidget(m_applyButton);
    layout->addLayout(buttons);

    // Any edit re-evaluates whether there is anything to apply.
    auto changed = [this] { updateControls(); };
    connect(m_callsignEdit, &QLineEdit::textChanged, this, changed);
    connect(m_gridEdit, &QLineEdit::textChanged, this, changed);
    for (QComboBox* c : {m_bandCombo, m_antennaCombo, m_depthCombo, m_daxCombo}) {
        connect(c, &QComboBox::currentIndexChanged, this, changed);
    }
    connect(m_hopCheck, &QCheckBox::toggled, this, changed);
    connect(m_graySpin, &QSpinBox::valueChanged, this, changed);
    connect(m_hopTable, &QTableWidget::itemChanged, this, changed);
    connect(m_uploadCheck, &QCheckBox::toggled, this, changed);

    m_client = new WsprAgentClient(this);
    connect(m_client, &WsprAgentClient::configReceived, this, &WsprConfigureWindow::showConfig);
    connect(m_client, &WsprAgentClient::statusReceived, this, &WsprConfigureWindow::showStatus);
    connect(m_client, &WsprAgentClient::configSaved, this,
            [this](const WsprConfig& config, const QString& newToken) {
        m_busy = false;
        if (!newToken.isEmpty()) {
            m_token = newToken;
            m_claimed = true;
            TailnetShimTokenStore::save(m_radioSerial, newToken, this,
                                        [this](bool persisted, const QString& error) {
                if (!persisted) {
                    setMessage(tr("Saved. This computer couldn't store the receiver's admin token (%1), "
                                  "so after a restart it can no longer change these settings.").arg(error),
                               QStringLiteral("error"));
                }
            }, TailnetShimTokenStore::Entry::Wspr);
        }
        showConfig(config, true);
        setMessage(tr("Saved on the radio."), QStringLiteral("ok"));
    });
    connect(m_client, &WsprAgentClient::requestFailed, this,
            [this](const QString& operation, const QString& message, bool unauthorized) {
        if (operation == QLatin1String("status")) {
            setStatusLine(tr("Not answering"), QStringLiteral("error"));
            return;
        }
        m_busy = false;
        updateControls();
        setMessage(unauthorized ? tr("Another computer set this receiver up, so this one can't change its "
                                     "settings. Reinstall the WSPR receiver from the Waveforms window to "
                                     "start over.")
                                : message,
                   QStringLiteral("error"));
    });

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(5000);
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        // Status only: a since in the future leaves the spots out.
        m_client->fetchStatus(QDateTime::currentSecsSinceEpoch() + 3600);
    });

    if (m_model) {
        m_radioSerial = m_model->chassisSerial().trimmed();
        m_client->setRadioAddress(m_model->radioAddress());
    }
    TailnetShimTokenStore::load(m_radioSerial, this, [this](const QString& token) {
        m_token = token;
        updateControls();
    }, TailnetShimTokenStore::Entry::Wspr);
    updateControls();
    load();
}

void WsprConfigureWindow::load()
{
    if (!m_model || !m_model->isConnected() || !wspr::reachableAddress(m_model->radioAddress())) {
        setStatusLine(tr("Not reachable"), QStringLiteral("error"));
        setMessage(tr("The WSPR receiver can be reached only on the radio's local network or "
                      "through Remote Access."), QStringLiteral("error"));
        return;
    }
    m_client->fetchConfig();
    m_client->fetchStatus(QDateTime::currentSecsSinceEpoch() + 3600);
    m_statusTimer->start();
}

void WsprConfigureWindow::showConfig(const WsprConfig& c, bool claimed)
{
    m_claimed = claimed;
    m_loaded = c;
    const QSignalBlocker b1(m_hopTable);
    m_callsignEdit->setText(c.callsign);
    m_gridEdit->setText(c.grid);
    m_bandCombo->setCurrentIndex(qMax(0, m_bandCombo->findData(c.band)));
    int ant = m_antennaCombo->findData(c.rxant);
    if (ant < 0) {
        // An antenna this radio doesn't list: keep it, so Apply doesn't drop it.
        m_antennaCombo->addItem(c.rxant, c.rxant);
        ant = m_antennaCombo->count() - 1;
    }
    m_antennaCombo->setCurrentIndex(ant);
    m_depthCombo->setCurrentIndex(qMax(0, m_depthCombo->findData(c.depth)));
    m_daxCombo->setCurrentIndex(qMax(0, m_daxCombo->findData(c.daxChannel)));
    m_hopCheck->setChecked(c.hopping);
    m_graySpin->setValue(c.grayMinutes);
    for (int p = 0; p < WsprConfig::PeriodCount; ++p) {
        for (int col = 0; col < wspr::bands().size(); ++col) {
            m_hopTable->item(p, col)->setCheckState(c.hop[p].contains(wspr::bands().at(col)) ? Qt::Checked
                                                                                              : Qt::Unchecked);
        }
    }
    m_uploadCheck->setChecked(c.upload);
    updateControls();
}

void WsprConfigureWindow::showStatus(const WsprStatus& st)
{
    setStatusLine(stateText(st), st.state == QLatin1String("capturing") || st.state == QLatin1String("decoding")
                                         || st.state == QLatin1String("waiting_cycle")
                                     ? QStringLiteral("running")
                                     : QStringLiteral("starting"));
    // An empty field means the radio's own; say what that is.
    m_callsignEdit->setPlaceholderText(st.callsign.isEmpty() ? tr("The radio's callsign")
                                                             : tr("From the radio: %1").arg(st.callsign));
    m_gridEdit->setPlaceholderText(st.grid.isEmpty() ? tr("The radio's GPS grid")
                                                     : tr("From the radio: %1").arg(st.grid));
    if (!st.upload.enabled) {
        m_uploadStatus->setText(tr("Spots stay on the radio and in SpotHub."));
    } else if (!st.upload.lastError.isEmpty()) {
        m_uploadStatus->setText(tr("%n spot(s) uploaded, %1 waiting. Last problem: %2", nullptr,
                                   static_cast<int>(st.upload.sent))
                                    .arg(st.upload.queued).arg(st.upload.lastError));
    } else {
        m_uploadStatus->setText(tr("%n spot(s) uploaded since the receiver started, %1 waiting.", nullptr,
                                   static_cast<int>(st.upload.sent))
                                    .arg(st.upload.queued));
    }
}

WsprConfig WsprConfigureWindow::collect() const
{
    WsprConfig c = m_loaded.value_or(WsprConfig{});
    c.callsign = m_callsignEdit->text().trimmed().toUpper();
    c.grid = m_gridEdit->text().trimmed();
    // A half-typed grid isn't one: keep what the receiver has.
    if (!c.grid.isEmpty() && c.grid.size() != 4 && c.grid.size() != 6) {
        c.grid = m_loaded ? m_loaded->grid : QString();
    }
    c.band = m_bandCombo->currentData().toString();
    c.rxant = m_antennaCombo->currentData().toString();
    c.depth = m_depthCombo->currentData().toString();
    c.daxChannel = m_daxCombo->currentData().toInt();
    c.hopping = m_hopCheck->isChecked();
    c.grayMinutes = m_graySpin->value();
    for (int p = 0; p < WsprConfig::PeriodCount; ++p) {
        c.hop[p].clear();
        for (int col = 0; col < wspr::bands().size(); ++col) {
            if (m_hopTable->item(p, col)->checkState() == Qt::Checked) {
                c.hop[p] << wspr::bands().at(col);
            }
        }
    }
    c.upload = m_uploadCheck->isChecked();
    return c;
}

void WsprConfigureWindow::apply()
{
    if (!m_loaded || m_busy) {
        return;
    }
    if (m_claimed && m_token.isEmpty()) {
        setMessage(tr("Another computer set this receiver up, so this one can't change its settings. "
                      "Reinstall the WSPR receiver from the Waveforms window to start over."),
                   QStringLiteral("error"));
        return;
    }
    m_busy = true;
    updateControls();
    setMessage(tr("Saving…"));
    m_client->saveConfig(collect(), m_token);
}

void WsprConfigureWindow::setMessage(const QString& text, const QString& tone)
{
    m_messageLabel->setText(text);
    m_messageLabel->setProperty("tone", tone);
    repolish(m_messageLabel);
}

void WsprConfigureWindow::setStatusLine(const QString& text, const QString& state)
{
    m_stateLabel->setText(text);
    m_statusDot->setProperty("state", state);
    m_statusDot->setAccessibleDescription(text);
    repolish(m_statusDot);
}

void WsprConfigureWindow::updateControls()
{
    const bool loaded = m_loaded.has_value();
    for (QWidget* w : std::initializer_list<QWidget*>{m_callsignEdit, m_gridEdit, m_bandCombo, m_antennaCombo,
                                                      m_depthCombo, m_daxCombo, m_hopCheck, m_uploadCheck}) {
        w->setEnabled(loaded && !m_busy);
    }
    const bool hop = loaded && !m_busy && m_hopCheck->isChecked();
    m_graySpin->setEnabled(hop);
    m_hopTable->setEnabled(hop);
    const bool dirty = loaded && collect() != *m_loaded;
    m_applyButton->setEnabled(dirty && !m_busy);
    m_applyButton->setAccessibleDescription(!loaded   ? tr("The receiver's settings haven't been read yet.")
                                            : m_busy  ? tr("Saving.")
                                            : !dirty  ? tr("Nothing has changed.")
                                                      : QString());
}

}  // namespace AetherSDR
