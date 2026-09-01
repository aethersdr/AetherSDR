#include "AetherGateApplet.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHideEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QUrlQuery>
#include <QVariant>
#include <QVBoxLayout>

namespace AetherSDR {

// The gate's --ctl-port default. Not discoverable over the Flex protocol, which
// has no verb to advertise it, so it is a convention shared with the gate.
static constexpr int kGateControlPort = 8731;

// Give up on a gate after this many consecutive failed polls. One miss is a
// dropped packet; a run of them means we are talking to a real Flex (or the
// gate died) and the applet should get out of the way.
static constexpr int kFailuresBeforeAbsent = 3;

static const char* kRowLabelStyle =
    "QLabel { color: #8090a0; font-size: 10px; font-weight: bold; }";

namespace {

QString formatHz(double hz)
{
    if (hz >= 1.0e6)
        return QStringLiteral("%1 MHz").arg(hz / 1.0e6, 0, 'f', 3);
    return QStringLiteral("%1 kHz").arg(hz / 1.0e3, 0, 'f', 1);
}

QString formatBinWidth(double hz)
{
    if (hz >= 1000.0)
        return QStringLiteral("%1 kHz / bin").arg(hz / 1000.0, 0, 'f', 2);
    return QStringLiteral("%1 Hz / bin").arg(hz, 0, 'f', 1);
}

// Repopulate without the refill looking like an operator choice: every control
// here writes to the radio on change, so an unblocked rebuild would re-send the
// value the gate just reported back to it, once per poll.
void setComboItems(QComboBox* combo, const QStringList& items, const QString& current)
{
    const QSignalBlocker block(combo);
    if (combo->count() != items.size()) {
        combo->clear();
        combo->addItems(items);
    }
    const int idx = combo->findText(current);
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}

} // namespace

AetherGateApplet::AetherGateApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/gate"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 8);
    root->setSpacing(6);

    m_status = new QLabel(tr("looking for a gate…"), this);
    m_status->setStyleSheet(kRowLabelStyle);
    root->addWidget(m_status);

    // --- panadapter resolution ------------------------------------------
    // Duplicated with the pan's own zoom on purpose: the zoom is a gesture and
    // this is a readout you can set exactly, and only this one shows what the
    // bin width actually came out as.
    auto* resForm = new QFormLayout;
    resForm->setContentsMargins(0, 0, 0, 0);
    resForm->setSpacing(4);

    m_span = new QComboBox(this);
    m_span->setToolTip(tr("Receiver sample rate. On an SDR the rate IS the "
                          "panadapter span, so a narrower span means finer bins."));
    connect(m_span, &QComboBox::currentIndexChanged, this, [this](int) { sendResolution(); });

    m_bins = new QComboBox(this);
    m_bins->setToolTip(tr("FFT bins across the span. Capped by what one UDP "
                          "datagram can carry."));
    connect(m_bins, &QComboBox::currentIndexChanged, this, [this](int) { sendResolution(); });

    m_binWidth = new QLabel(QStringLiteral("—"), this);

    auto* spanLabel = new QLabel(tr("Span"), this);
    auto* binsLabel = new QLabel(tr("Bins"), this);
    spanLabel->setStyleSheet(kRowLabelStyle);
    binsLabel->setStyleSheet(kRowLabelStyle);
    resForm->addRow(spanLabel, m_span);
    resForm->addRow(binsLabel, m_bins);
    resForm->addRow(QString(), m_binWidth);
    root->addLayout(resForm);

    // --- device controls, built from what the gate reports ---------------
    m_deviceBox = new QWidget(this);
    m_deviceForm = new QFormLayout(m_deviceBox);
    m_deviceForm->setContentsMargins(0, 6, 0, 0);
    m_deviceForm->setSpacing(4);
    m_deviceBox->setVisible(false);
    root->addWidget(m_deviceBox);

    root->addStretch(1);

    m_net = new QNetworkAccessManager(this);
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &AetherGateApplet::poll);
}

void AetherGateApplet::setRadioModel(RadioModel* model)
{
    m_model = model;
    // A new connection may be a different radio entirely, so drop what we know
    // and re-probe rather than showing the previous gate's controls.
    m_controlsFingerprint.clear();
    m_failures = 0;
    setPresent(false);
    // Probe even while hidden.  AppletPanel keeps the GATE button out of the
    // bar until presence is confirmed, so an applet that polled only when
    // visible could never become visible — it would be waiting on the answer to
    // a question it had no way to ask.  poll() stops the timer itself once the
    // question is settled, either way.
    m_timer->start();
    poll();
}

QString AetherGateApplet::baseUrl() const
{
    if (!m_model || m_model->ip().isEmpty())
        return {};
    return QStringLiteral("http://%1:%2").arg(m_model->ip()).arg(kGateControlPort);
}

void AetherGateApplet::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    poll();
    m_timer->start();
}

void AetherGateApplet::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

void AetherGateApplet::get(const QString& path,
                           void (AetherGateApplet::*handler)(const QByteArray&))
{
    const QString base = baseUrl();
    if (base.isEmpty())
        return;
    QNetworkRequest req{QUrl(base + path)};
    req.setTransferTimeout(2000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                     QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, handler] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (++m_failures >= kFailuresBeforeAbsent)
                setPresent(false);
            return;
        }
        m_failures = 0;
        setPresent(true);
        (this->*handler)(reply->readAll());
    });
}

void AetherGateApplet::setPresent(bool present)
{
    if (m_present == present)
        return;
    m_present = present;
    if (!present) {
        m_status->setText(tr("no Aether-gate on this radio"));
        m_deviceBox->setVisible(false);
        m_controlsFingerprint.clear();
    }
    emit gatePresenceChanged(present);
}

void AetherGateApplet::poll()
{
    // Off screen the timer exists only to answer "is there a gate on this
    // radio?".  Once that is answered — found, or missed enough times to call
    // it — stop: polling a radio nobody is looking at is pure noise on the
    // network and in the gate's log.  showEvent() restarts it.
    // No radio address yet (wired at startup, before any connect) — nothing to
    // probe, and get() would silently drop the request without ever counting a
    // failure, leaving the timer spinning on a no-op forever.
    if (baseUrl().isEmpty()) {
        m_timer->stop();
        return;
    }
    if (!isVisible() && (m_present || m_failures >= kFailuresBeforeAbsent)) {
        m_timer->stop();
        return;
    }
    get(QStringLiteral("/status"), &AetherGateApplet::applyStatus);
}

void AetherGateApplet::refreshDeviceControls()
{
    get(QStringLiteral("/device"), &AetherGateApplet::applyDeviceControls);
}

void AetherGateApplet::applyStatus(const QByteArray& json)
{
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject res = root.value(QStringLiteral("res")).toObject();
    if (res.isEmpty())
        return;

    const bool connected = root.value(QStringLiteral("connected")).toBool();
    const bool streaming = root.value(QStringLiteral("streaming")).toBool();
    m_status->setText(connected
                          ? tr("gate connected · %1").arg(streaming ? tr("streaming")
                                                                    : tr("idle"))
                          : tr("gate up · waiting for the app"));

    const double spanHz = res.value(QStringLiteral("span_hz")).toDouble();
    const double binHz = res.value(QStringLiteral("bin_hz")).toDouble();
    const int bins = res.value(QStringLiteral("bins")).toInt();
    const int maxBins = res.value(QStringLiteral("max_bins")).toInt(4096);
    m_binWidth->setText(formatBinWidth(binHz));

    // Span list comes from the device, not from us — see the header comment.
    // Each entry carries the raw rate as item data: the label is rounded for
    // reading and must never be what gets sent back.
    const QJsonArray rates = res.value(QStringLiteral("rates")).toArray();
    const bool canSetRate = res.value(QStringLiteral("can_set_rate")).toBool();
    m_span->setEnabled(canSetRate && !rates.isEmpty());
    if (!rates.isEmpty()) {
        const QSignalBlocker block(m_span);
        if (m_span->count() != rates.size()) {
            m_span->clear();
            for (const QJsonValue& v : rates)
                m_span->addItem(formatHz(v.toDouble()), QVariant(v.toDouble()));
        }
        const double running = res.value(QStringLiteral("samp_rate")).toDouble(spanHz);
        const int idx = m_span->findText(formatHz(running));
        if (idx >= 0)
            m_span->setCurrentIndex(idx);
    }

    QStringList binItems;
    for (int n = 1024; n <= 16384; n *= 2) {
        if (n <= maxBins)
            binItems << QString::number(n);
    }
    if (!binItems.contains(QString::number(bins)))
        binItems << QString::number(bins);
    setComboItems(m_bins, binItems, QString::number(bins));

    // The control SET only changes when the device does, so this is fetched
    // once rather than on every one-second poll.
    if (m_controlsFingerprint.isEmpty())
        refreshDeviceControls();
}

void AetherGateApplet::sendResolution()
{
    const QString base = baseUrl();
    if (base.isEmpty() || !m_present)
        return;
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("bins"), m_bins->currentText());
    if (m_span->isEnabled()) {
        const int idx = m_span->currentIndex();
        if (idx >= 0)
            q.addQueryItem(QStringLiteral("rate"),
                           QString::number(m_span->itemData(idx).toDouble(), 'f', 0));
    }
    QUrl url(base + QStringLiteral("/resolution"));
    url.setQuery(q);
    QNetworkRequest req{url};
    req.setTransferTimeout(8000);          // a rate change restarts the stream
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void AetherGateApplet::applyDeviceControls(const QByteArray& json)
{
    buildDeviceControls(QJsonDocument::fromJson(json).object());
}

void AetherGateApplet::buildDeviceControls(const QJsonObject& dev)
{
    // Fingerprint the SHAPE (which controls exist), not the values — rebuilding
    // widgets under the operator's cursor every time a value changed would make
    // the panel unusable.
    QStringList shape;
    const QJsonObject ant = dev.value(QStringLiteral("antenna")).toObject();
    if (!ant.isEmpty())
        shape << QStringLiteral("antenna");
    const QJsonArray settings = dev.value(QStringLiteral("settings")).toArray();
    for (const QJsonValue& v : settings)
        shape << v.toObject().value(QStringLiteral("key")).toString();
    const QString fingerprint = shape.join(QLatin1Char('|'));

    if (fingerprint != m_controlsFingerprint) {
        m_controlsFingerprint = fingerprint;
        m_settingWidgets.clear();
        m_antenna = nullptr;
        while (m_deviceForm->count() > 0) {
            QLayoutItem* item = m_deviceForm->takeAt(0);
            if (QWidget* w = item->widget())
                w->deleteLater();
            delete item;
        }

        if (!ant.isEmpty()) {
            m_antenna = new QComboBox(m_deviceBox);
            for (const QJsonValue& o : ant.value(QStringLiteral("options")).toArray())
                m_antenna->addItem(o.toString());
            connect(m_antenna, &QComboBox::currentTextChanged, this,
                    [this](const QString& text) {
                        QUrlQuery q;
                        q.addQueryItem(QStringLiteral("antenna"), text);
                        QUrl url(baseUrl() + QStringLiteral("/device/set"));
                        url.setQuery(q);
                        QNetworkReply* r = m_net->get(QNetworkRequest(url));
                        connect(r, &QNetworkReply::finished, r, &QNetworkReply::deleteLater);
                    });
            auto* label = new QLabel(tr("Antenna"), m_deviceBox);
            label->setStyleSheet(kRowLabelStyle);
            m_deviceForm->addRow(label, m_antenna);
        }

        for (const QJsonValue& v : settings) {
            const QJsonObject so = v.toObject();
            const QString key = so.value(QStringLiteral("key")).toString();
            const QString name = so.value(QStringLiteral("name")).toString(key);
            const QString type = so.value(QStringLiteral("type")).toString();
            const QJsonArray options = so.value(QStringLiteral("options")).toArray();

            // A write goes out on change and the read-back arrives with the
            // reply, so the control always ends up showing what the DEVICE
            // took rather than what we asked for.
            auto push = [this, key](const QString& value) {
                QUrlQuery q;
                q.addQueryItem(QStringLiteral("key"), key);
                q.addQueryItem(QStringLiteral("value"), value);
                QUrl url(baseUrl() + QStringLiteral("/device/set"));
                url.setQuery(q);
                QNetworkReply* r = m_net->get(QNetworkRequest(url));
                connect(r, &QNetworkReply::finished, this, [this, r] {
                    r->deleteLater();
                    if (r->error() == QNetworkReply::NoError)
                        applyDeviceControls(r->readAll());
                });
            };

            QWidget* w = nullptr;
            if (!options.isEmpty()) {
                auto* combo = new QComboBox(m_deviceBox);
                for (const QJsonValue& o : options)
                    combo->addItem(o.toString());
                connect(combo, &QComboBox::currentTextChanged, this, push);
                w = combo;
            } else if (type == QLatin1String("0")) {          // Soapy ArgInfo BOOL
                auto* check = new QCheckBox(m_deviceBox);
                connect(check, &QCheckBox::toggled, this, [push](bool on) {
                    push(on ? QStringLiteral("true") : QStringLiteral("false"));
                });
                w = check;
            } else {
                auto* spin = new QSpinBox(m_deviceBox);
                spin->setRange(-1000, 1000);
                spin->setKeyboardTracking(false);   // one write per committed edit
                connect(spin, &QSpinBox::valueChanged, this, [push](int v) {
                    push(QString::number(v));
                });
                w = spin;
            }
            m_settingWidgets.insert(key, w);
            auto* label = new QLabel(name, m_deviceBox);
            label->setStyleSheet(kRowLabelStyle);
            m_deviceForm->addRow(label, w);
        }
        m_deviceBox->setVisible(!shape.isEmpty());
    }

    // Values, every time — blocked so a refresh never re-sends what it reads.
    if (m_antenna && !ant.isEmpty()) {
        const QSignalBlocker block(m_antenna);
        const int idx = m_antenna->findText(ant.value(QStringLiteral("value")).toString());
        if (idx >= 0)
            m_antenna->setCurrentIndex(idx);
    }
    for (const QJsonValue& v : settings) {
        const QJsonObject so = v.toObject();
        QWidget* w = m_settingWidgets.value(so.value(QStringLiteral("key")).toString());
        if (!w)
            continue;
        const QString value = so.value(QStringLiteral("value")).toString();
        if (auto* combo = qobject_cast<QComboBox*>(w)) {
            const QSignalBlocker block(combo);
            const int idx = combo->findText(value);
            if (idx >= 0)
                combo->setCurrentIndex(idx);
        } else if (auto* check = qobject_cast<QCheckBox*>(w)) {
            const QSignalBlocker block(check);
            check->setChecked(value == QLatin1String("true"));
        } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
            const QSignalBlocker block(spin);
            spin->setValue(value.toInt());
        }
    }
}

} // namespace AetherSDR
