#include "Ft8ConfigureWindow.h"

#include "CanonIndicators.h"

#include "core/Ft8AgentClient.h"
#include "core/TailnetShimTokenStore.h"
#include "models/RadioModel.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

// The canon dialog style, as the WSPR window: cards on the nested surface,
// keys in muted, values in ink-soft, Apply as the brand-gradient primary.
// Rules are scoped by object name or the fsRole property so the card rule
// never reaches the QLabels inside it.
constexpr const char* kFt8Style =
    "QWidget#ft8Body { background: transparent; }"
    "QLabel { color: {{color.canon.inkSoft}}; background: transparent; border: none; font-size: 12px; }"
    "QLabel#fsTitle { color: {{color.canon.ink}}; font-size: 20px; font-weight: bold; }"
    "QLabel#fsStatusLine { color: {{color.canon.muted}}; font-size: 13px; }"
    "QLabel#fsStatusDot { background: {{color.canon.muted}}; border-radius: 4px;"
    " min-width: 8px; max-width: 8px; min-height: 8px; max-height: 8px; }"
    "QLabel#fsStatusDot[state=\"running\"] { background: {{color.canon.cyan}}; }"
    "QLabel#fsStatusDot[state=\"starting\"] { background: {{color.accent.warning}}; }"
    "QLabel#fsStatusDot[state=\"error\"] { background: {{color.accent.danger}}; }"
    "QFrame#fsRule { background: {{color.canon.line}}; border: none; min-height: 1px; max-height: 1px; }"
    "QFrame[fsRole=\"card\"] { background: {{color.canon.nested}}; border: 1px solid {{color.canon.line}};"
    " border-radius: 12px; }"
    "QLabel[fsRole=\"cardTitle\"] { color: {{color.canon.ink}}; font-size: 13px; font-weight: bold; }"
    "QLabel[fsRole=\"key\"] { color: {{color.canon.muted}}; font-size: 10px; font-weight: bold; }"
    "QLabel[fsRole=\"hint\"] { color: {{color.canon.muted}}; font-size: 11px; }"
    "QLabel[fsRole=\"message\"][tone=\"error\"] { color: {{color.accent.danger}}; }"
    "QLabel[fsRole=\"message\"][tone=\"ok\"] { color: {{color.accent.success}}; }"
    "QLineEdit, QComboBox { background: {{color.canon.control}}; color: {{color.canon.ink}};"
    " border: 1px solid {{color.canon.lineHi}}; border-radius: 4px; padding: 4px 8px; font-size: 12px;"
    " selection-background-color: {{color.canon.cyan}}; selection-color: {{color.canon.onAccent}}; }"
    "QLineEdit:focus, QComboBox:focus, QComboBox:hover { border-color: {{color.canon.aqua}}; }"
    "QLineEdit:disabled, QComboBox:disabled { color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }"
    "QComboBox::drop-down { border: 0; width: 18px; }"
    "QComboBox QAbstractItemView { color: {{color.canon.inkSoft}}; background: {{color.canon.nested}};"
    " border: 1px solid {{color.canon.lineHi}}; selection-background-color: {{color.canon.cyan}};"
    " selection-color: {{color.canon.onAccent}}; }"
    "QPushButton#fsApply { background: {{color.brand.gradient}}; color: {{color.canon.onAccent}};"
    " border: 1px solid transparent; border-radius: 4px; padding: 6px 30px;"
    " font-size: 12px; font-weight: bold; }"
    "QPushButton#fsApply:hover { border-color: {{color.canon.aqua}}; }"
    "QPushButton#fsApply:focus { border: 2px solid {{color.canon.aqua}}; padding: 5px 29px; }"
    "QPushButton#fsApply:disabled { background: transparent; color: {{color.canon.muted}};"
    " border-color: {{color.canon.line}}; }";

void repolish(QWidget* w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
}

QLabel* roleLabel(const QString& text, const char* role, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("fsRole", QString::fromLatin1(role));
    return label;
}

QFrame* makeCard(const QString& title, QWidget* parent, QVBoxLayout** contents)
{
    auto* card = new QFrame(parent);
    card->setProperty("fsRole", QStringLiteral("card"));
    card->setAccessibleName(title);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(16, 14, 16, 16);
    v->setSpacing(10);
    v->addWidget(roleLabel(title, "cardTitle", card));
    *contents = v;
    return card;
}

QString bandDisplay(const QString& band)
{
    // "20m" -> "20 m", as the rest of the app writes bands.
    return band.endsWith(QLatin1Char('m')) ? band.chopped(1) + QStringLiteral(" m") : band;
}

QString stateText(const Ft8Status& st)
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
        // On a FLEX-6000 the kernel never reports a synchronized clock even
        // when the radio's NTP has disciplined it, so say what is missing
        // rather than implying the radio is broken.
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

Ft8ConfigureWindow::Ft8ConfigureWindow(RadioModel* model, QWidget* parent)
    : CanonWindow(tr("FT8 Receiver"), parent)
    , m_model(model)
{
    constexpr int kWidth = 620;
    constexpr int kSide = 28;
    setFixedWidth(kWidth);
    bodyWidget()->setObjectName(QStringLiteral("ft8Body"));
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kFt8Style) + canonIndicatorRules();
    });

    QWidget* body = bodyWidget();
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(kSide, 26, kSide, 22);
    layout->setSpacing(14);

    // Header: title, then a status line led by a dot, then a hairline.
    const QString radioName = m_model ? m_model->nickname() : QString();
    auto* title = new QLabel(radioName.isEmpty() ? tr("FT8 Receiver") : tr("FT8 Receiver — %1").arg(radioName), body);
    title->setObjectName(QStringLiteral("fsTitle"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);
    statusRow->addStretch(1);
    m_statusDot = new QLabel(body);
    m_statusDot->setObjectName(QStringLiteral("fsStatusDot"));
    m_statusDot->setAccessibleName(tr("FT8 receiver status indicator"));
    m_stateLabel = new QLabel(tr("Checking…"), body);
    m_stateLabel->setObjectName(QStringLiteral("fsStatusLine"));
    m_stateLabel->setAccessibleName(tr("FT8 receiver status"));
    statusRow->addWidget(m_statusDot, 0, Qt::AlignVCenter);
    statusRow->addWidget(m_stateLabel);
    statusRow->addStretch(1);
    layout->addLayout(statusRow);

    // An FT8 cycle leaves about two seconds to decode in, and on a 1-core
    // radio that is tight enough to be worth showing: an operator who sees
    // decodes running over the budget knows why spots are being missed.
    m_decodeLabel = roleLabel(QString(), "hint", body);
    m_decodeLabel->setAlignment(Qt::AlignCenter);
    m_decodeLabel->setAccessibleName(tr("Decode time against the cycle budget"));
    layout->addWidget(m_decodeLabel);

    auto* rule = new QFrame(body);
    rule->setObjectName(QStringLiteral("fsRule"));
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
    // The agent CAN band hop -- band_for_cycle() carries WSPR's gray-line
    // schedule, coordinated bands and random permutation across to FT8 -- but
    // this window does not expose the hop lists, so with the settings it can
    // write the receiver stays on one band. Say that, rather than "does not
    // band hop", which was wrong about the receiver and would be read aloud
    // to someone who cannot see the window.
    m_bandCombo->setAccessibleDescription(tr("The band the receiver listens on. It stays there: "
                                             "band hopping is not set up from this window."));
    for (const QString& b : ft8::bands()) {
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
    m_daxCombo = new QComboBox(body);
    m_daxCombo->setAccessibleName(tr("DAX channel"));
    m_daxCombo->setAccessibleDescription(tr("The DAX audio channel the receiver uses for its own slice. "
                                            "It must differ from any other receiver's."));
    for (int ch = 1; ch <= 4; ++ch) {
        m_daxCombo->addItem(QString::number(ch), ch);
    }
    addKey(rgrid, 0, 0, tr("BAND"), m_bandCombo);
    addKey(rgrid, 0, 2, tr("ANTENNA"), m_antennaCombo);
    addKey(rgrid, 1, 0, tr("DAX CHANNEL"), m_daxCombo);
    receive->addLayout(rgrid);
    receive->addWidget(roleLabel(tr("The receiver makes a slice of its own, which never transmits."),
                                 "hint", body));

    // ── Message and Apply ────────────────────────────────────────────────
    m_messageLabel = roleLabel(QString(), "message", body);
    m_messageLabel->setWordWrap(true);
    layout->addWidget(m_messageLabel);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    m_applyButton = new QPushButton(tr("Apply"), body);
    m_applyButton->setObjectName(QStringLiteral("fsApply"));
    m_applyButton->setAccessibleName(tr("Apply the FT8 receiver settings"));
    connect(m_applyButton, &QPushButton::clicked, this, &Ft8ConfigureWindow::apply);
    buttons->addWidget(m_applyButton);
    layout->addLayout(buttons);

    m_client = new Ft8AgentClient(this);
    connect(m_client, &Ft8AgentClient::configReceived, this, &Ft8ConfigureWindow::showConfig);
    connect(m_client, &Ft8AgentClient::statusReceived, this, &Ft8ConfigureWindow::showStatus);
    connect(m_client, &Ft8AgentClient::configSaved, this,
            [this](const Ft8Config& config, const QString& newToken) {
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
            }, TailnetShimTokenStore::Entry::Ft8);
        }
        m_loaded = config;
        showConfig(config, true);
        setMessage(tr("Saved. The receiver applies them on its next cycle."), QStringLiteral("ok"));
    });
    connect(m_client, &Ft8AgentClient::requestFailed, this,
            [this](const QString& operation, const QString& message, bool unauthorized) {
        if (operation == QLatin1String("status")) {
            return;   // a missed poll is not worth a message
        }
        m_busy = false;
        if (unauthorized) {
            m_token.clear();
        }
        setMessage(message, QStringLiteral("error"));
        updateControls();
    });

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(5000);
    connect(m_statusTimer, &QTimer::timeout, this, [this] {
        // ⚠ NOT a `since` in the future, which is what the WSPR window uses
        // to skip the spots it does not need. That filter drops the CYCLES
        // too, and this window shows the last decode time against the budget,
        // so it must see them. One cycle back is enough and keeps the reply
        // small. (Measured: a future `since` returned 0 cycles and 0 spots,
        // which is why the decode line never drew.)
        m_client->fetchStatus(QDateTime::currentSecsSinceEpoch() - kCycleSeconds * 2);
    });

    if (m_model) {
        m_radioSerial = m_model->chassisSerial().trimmed();
        m_client->setRadioAddress(m_model->radioAddress());
    }
    TailnetShimTokenStore::load(m_radioSerial, this, [this](const QString& token) {
        m_token = token;
        updateControls();
    }, TailnetShimTokenStore::Entry::Ft8);
    updateControls();
    load();
}

void Ft8ConfigureWindow::load()
{
    if (!m_model || !m_model->isConnected() || !ft8::reachableAddress(m_model->radioAddress())) {
        setStatusLine(tr("Not reachable"), QStringLiteral("error"));
        setMessage(tr("The FT8 receiver can be reached only on the radio's local network, or through "
                      "a tailnet shim that publishes it."),
                   QStringLiteral("error"));
        updateControls();
        return;
    }
    setMessage(tr("Reading the receiver's settings…"));
    m_client->fetchConfig();
    m_client->fetchStatus(QDateTime::currentSecsSinceEpoch() - kCycleSeconds * 2);
    m_statusTimer->start();
}

void Ft8ConfigureWindow::showConfig(const Ft8Config& config, bool claimed)
{
    m_loaded = config;
    m_claimed = claimed;
    m_callsignEdit->setText(config.callsign);
    m_gridEdit->setText(config.grid);
    const int band = m_bandCombo->findData(config.band);
    if (band >= 0) {
        m_bandCombo->setCurrentIndex(band);
    }
    int ant = m_antennaCombo->findData(config.rxant);
    if (ant < 0 && !config.rxant.isEmpty()) {
        // The receiver is on an antenna this radio did not list; keep it
        // rather than silently moving the receiver somewhere else.
        m_antennaCombo->addItem(config.rxant, config.rxant);
        ant = m_antennaCombo->count() - 1;
    }
    m_antennaCombo->setCurrentIndex(ant < 0 ? 0 : ant);
    const int dax = m_daxCombo->findData(config.daxChannel);
    if (dax >= 0) {
        m_daxCombo->setCurrentIndex(dax);
    }
    if (m_messageLabel->text() == tr("Reading the receiver's settings…")) {
        setMessage(QString());
    }
    updateControls();
}

void Ft8ConfigureWindow::showStatus(const Ft8Status& st)
{
    // It answers now, so it may not have when the window opened.
    if (!m_loaded) {
        m_client->fetchConfig();
    }
    const bool running = st.state == QLatin1String("capturing")
                      || st.state == QLatin1String("decoding")
                      || st.state == QLatin1String("waiting_cycle");
    setStatusLine(stateText(st), running ? QStringLiteral("running") : QStringLiteral("starting"));

    // An empty field means the radio's own; say what that is.
    m_callsignEdit->setPlaceholderText(st.callsign.isEmpty() ? tr("The radio's callsign")
                                                             : tr("From the radio: %1").arg(st.callsign));
    m_gridEdit->setPlaceholderText(st.grid.isEmpty() ? tr("The radio's GPS grid")
                                                     : tr("From the radio: %1").arg(st.grid));

    // The newest cycle's decode time against the budget. Over it means the
    // decode did not finish before the next cycle began.
    if (st.cycles.isEmpty()) {
        m_decodeLabel->setText(QString());
        return;
    }
    const Ft8Cycle& c = st.cycles.last();
    m_decodeLabel->setText(c.decodeMs > kDecodeBudgetMs
        ? tr("Last decode %1 ms — over the %2 ms this cycle allows, so decodes are being missed.")
              .arg(c.decodeMs).arg(kDecodeBudgetMs)
        : tr("Last decode %1 ms of the %2 ms this cycle allows.").arg(c.decodeMs).arg(kDecodeBudgetMs));
}

Ft8Config Ft8ConfigureWindow::collect() const
{
    Ft8Config c = m_loaded.value_or(Ft8Config{});
    c.callsign = m_callsignEdit->text().trimmed().toUpper();
    c.grid = m_gridEdit->text().trimmed();
    c.band = m_bandCombo->currentData().toString();
    c.rxant = m_antennaCombo->currentData().toString();
    c.daxChannel = m_daxCombo->currentData().toInt();
    return c;
}

void Ft8ConfigureWindow::apply()
{
    if (!m_loaded || m_busy) {
        return;
    }
    if (m_claimed && m_token.isEmpty()) {
        setMessage(tr("Another computer set this receiver up, so this one can't change its settings. "
                      "Reinstall the FT8 receiver from the Waveforms window to start over."),
                   QStringLiteral("error"));
        return;
    }
    m_busy = true;
    updateControls();
    setMessage(tr("Saving…"));
    m_client->saveConfig(collect(), m_token);
}

void Ft8ConfigureWindow::setMessage(const QString& text, const QString& tone)
{
    m_messageLabel->setText(text);
    m_messageLabel->setProperty("tone", tone);
    repolish(m_messageLabel);
}

void Ft8ConfigureWindow::setStatusLine(const QString& text, const QString& state)
{
    m_stateLabel->setText(text);
    m_statusDot->setProperty("state", state);
    repolish(m_statusDot);
}

void Ft8ConfigureWindow::updateControls()
{
    const bool editable = m_loaded.has_value() && !m_busy;
    m_callsignEdit->setEnabled(editable);
    m_gridEdit->setEnabled(editable);
    m_bandCombo->setEnabled(editable);
    m_antennaCombo->setEnabled(editable);
    m_daxCombo->setEnabled(editable);
    m_applyButton->setEnabled(editable);
}

void Ft8ConfigureWindow::closeEvent(QCloseEvent* event)
{
    m_statusTimer->stop();
    m_client->cancel();
    CanonWindow::closeEvent(event);
}

}  // namespace AetherSDR
