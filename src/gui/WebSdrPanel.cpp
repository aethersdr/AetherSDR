#include "WebSdrPanel.h"

#include "WebSdrWaterfallView.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QButtonGroup>
#include <QFont>

namespace AetherSDR {

namespace {
const char* kStateNames[] = {"Disconnected", "Connecting", "Connected", "Streaming", "Error"};

// Standard themed button (applet-style-guide §Buttons), token-based.
const char* kBtnStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; "
    "padding: 2px 6px; }"
    "QPushButton:hover { background: {{color.background.1}}; }";

// As above but with an accent-filled checked state (Blue-active role).
const char* kToggleBtnStyle =
    "QPushButton { background: {{color.background.2}}; border: 1px solid {{color.background.2}}; "
    "border-radius: 3px; color: {{color.text.primary}}; font-size: 10px; font-weight: bold; "
    "padding: 2px 6px; }"
    "QPushButton:hover { background: {{color.background.1}}; }"
    "QPushButton:checked { background: {{color.accent}}; color: {{color.background.0}}; "
    "border: 1px solid {{color.accent}}; }";

// Dark input fields (host / frequency) to match the themed surfaces.
const char* kInputStyle =
    "QLineEdit, QAbstractSpinBox { background: {{color.background.1}}; color: {{color.text.primary}}; "
    "border: 1px solid {{color.border.subtle}}; border-radius: 2px; padding: 2px 4px; font-size: 11px; }"
    "QLineEdit:focus, QAbstractSpinBox:focus { border: 1px solid {{color.accent}}; }";
}

WebSdrPanel::WebSdrPanel(QWidget* parent)
    : QDockWidget(tr("WebSDR"), parent)
{
    setObjectName(QStringLiteral("WebSdrPanel"));
    setFeatures(QDockWidget::DockWidgetMovable
              | QDockWidget::DockWidgetFloatable
              | QDockWidget::DockWidgetClosable);

    auto* content = new QWidget(this);
    QFont pf = content->font();
    pf.setPixelSize(11);                     // compact applet-style text (style guide §Typography)
    content->setFont(pf);

    auto* root = new QVBoxLayout(content);
    root->setContentsMargins(4, 4, 4, 4);    // tight applet-style margins (style guide §Layout)
    root->setSpacing(4);

    auto& theme = ThemeManager::instance();

    // Row 1 — server host + connect.
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(4);
    m_host = new QLineEdit(content);
    m_host->setPlaceholderText(QStringLiteral("host:port  (e.g. example.org:8901)"));
    m_host->setAccessibleName(tr("WebSDR server host and port"));
    theme.applyStyleSheet(m_host, kInputStyle);
    m_connectBtn = new QPushButton(tr("Connect"), content);
    m_connectBtn->setAccessibleName(tr("Connect or disconnect the WebSDR"));
    theme.applyStyleSheet(m_connectBtn, kBtnStyle);
    row1->addWidget(m_host, 1);
    row1->addWidget(m_connectBtn);
    root->addLayout(row1);

    // Row 2 — frequency + audio toggle.
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(4);
    m_freq = new QDoubleSpinBox(content);
    m_freq->setRange(0.0, 30000000.0);       // kHz, up to 30 GHz headroom
    m_freq->setDecimals(3);
    m_freq->setSuffix(QStringLiteral(" kHz"));
    m_freq->setValue(3557.0);
    m_freq->setAccessibleName(tr("WebSDR frequency in kHz"));
    theme.applyStyleSheet(m_freq, kInputStyle);
    m_audioToggle = new QPushButton(tr("WebSDR Audio"), content);
    m_audioToggle->setCheckable(true);
    m_audioToggle->setAccessibleName(tr("Route speaker audio to the WebSDR"));
    theme.applyStyleSheet(m_audioToggle, kToggleBtnStyle);
    row2->addWidget(m_freq, 1);
    row2->addWidget(m_audioToggle);
    root->addLayout(row2);

    // Row 3 — per-slice follow buttons.
    auto* row3 = new QHBoxLayout();
    row3->setSpacing(4);
    row3->addWidget(new QLabel(tr("Follow:"), content));
    m_sliceRow = new QWidget(content);
    auto* sliceLay = new QHBoxLayout(m_sliceRow);
    sliceLay->setContentsMargins(0, 0, 0, 0);
    sliceLay->setSpacing(2);
    row3->addWidget(m_sliceRow);
    m_sliceGroup = new QButtonGroup(this);
    m_sliceGroup->setExclusive(false);       // allow clicking the active one to stop
    row3->addStretch(1);
    root->addLayout(row3);

    // Status row.
    auto* statusRow = new QHBoxLayout();
    m_stateLabel = new QLabel(tr("Disconnected"), content);
    m_bandLabel  = new QLabel(QString(), content);
    m_bandLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    statusRow->addWidget(m_stateLabel);
    statusRow->addWidget(m_bandLabel, 1);
    root->addLayout(statusRow);

    m_waterfall = new WebSdrWaterfallView(content);
    m_waterfall->setOnClick([this](double freqKHz) {
        m_freq->setValue(freqKHz);     // canvas reports an absolute kHz (crop-aware)
        tuneNow();
    });
    root->addWidget(m_waterfall, 1);

    setWidget(content);
    loadSettings();

    connect(m_connectBtn, &QPushButton::clicked, this, &WebSdrPanel::onConnectClicked);
    connect(m_freq, &QDoubleSpinBox::editingFinished, this, &WebSdrPanel::onTuneChanged);
    connect(m_audioToggle, &QPushButton::toggled, this, &WebSdrPanel::audioToWebSdrToggled);
    connect(m_sliceGroup, &QButtonGroup::idClicked, this, [this](int id) {
        QAbstractButton* btn = m_sliceGroup->button(id);
        if (btn && btn->isChecked()) {
            for (QAbstractButton* b : m_sliceGroup->buttons()) {
                if (m_sliceGroup->id(b) != id) {
                    b->setChecked(false);
                }
            }
            m_followingId = id;
        } else {
            m_followingId = -1;
        }
        const bool following = (m_followingId >= 0);
        m_freq->setEnabled(!following);   // manual tuning is driven by the slice while following
        emit followSliceChanged(m_followingId);
    });
    tuneNow();   // initialise the marker from the default freq/mode
}

void WebSdrPanel::onConnectClicked()
{
    if (!m_connected) {
        const QString host = m_host->text().trimmed();
        if (host.isEmpty()) {
            return;
        }
        saveSettings();
        emit connectRequested(host);
        tuneNow();
    } else {
        emit disconnectRequested();
    }
}

void WebSdrPanel::onTuneChanged()
{
    tuneNow();
    saveSettings();
}

void WebSdrPanel::passbandFor(const QString& mode, double& loKHz, double& hiKHz) const
{
    const QString m = mode.toLower();
    if      (m == "usb") { loKHz =  0.0; hiKHz =  2.7; }
    else if (m == "lsb") { loKHz = -2.7; hiKHz =  0.0; }
    else if (m == "cw")  { loKHz = -0.2; hiKHz =  0.2; }
    else if (m == "cwu") { loKHz =  0.2; hiKHz =  0.7; }
    else if (m == "cwl") { loKHz = -0.7; hiKHz = -0.2; }
    else if (m == "am")  { loKHz = -4.5; hiKHz =  4.5; }
    else if (m == "fm")  { loKHz = -6.0; hiKHz =  6.0; }
    else                 { loKHz =  0.0; hiKHz =  2.7; }
}

void WebSdrPanel::tuneNow()
{
    double lo, hi;
    passbandFor(m_mode, lo, hi);
    m_curLoKHz = lo;
    m_curHiKHz = hi;
    if (m_waterfall) {
        m_waterfall->setTuning(m_freq->value(), lo, hi);
    }
    emit tuneRequested(m_freq->value(), m_mode, lo, hi);
}

void WebSdrPanel::applyExternalTune(double freqKHz, const QString& mode,
                                    double loKHz, double hiKHz)
{
    if (qAbs(freqKHz - m_freq->value()) < 1e-6 &&
        mode.compare(m_mode, Qt::CaseInsensitive) == 0 &&
        qAbs(loKHz - m_curLoKHz) < 1e-6 && qAbs(hiKHz - m_curHiKHz) < 1e-6) {
        return;   // unchanged — avoid command spam while following
    }
    QSignalBlocker bf(m_freq);
    m_freq->setValue(freqKHz);
    m_mode = mode;
    m_curLoKHz = loKHz;
    m_curHiKHz = hiKHz;
    if (m_waterfall) {
        m_waterfall->setTuning(freqKHz, loKHz, hiKHz);
    }
    emit tuneRequested(freqKHz, mode, loKHz, hiKHz);
}

void WebSdrPanel::setSourceState(int state, const QString& detail)
{
    m_connected = (state == 2 /*Connected*/ || state == 3 /*Streaming*/);
    QString label = (state >= 0 && state <= 4) ? QString::fromLatin1(kStateNames[state])
                                               : QStringLiteral("?");
    if (!detail.isEmpty()) {
        label += QStringLiteral(" (%1)").arg(detail);
    }
    m_stateLabel->setText(label);
    m_connectBtn->setText(m_connected || state == 1 ? tr("Disconnect") : tr("Connect"));
    if (!m_connected && state != 1) {
        // dropped: revert the audio toggle so Flex regains the speaker
        if (m_audioToggle->isChecked()) {
            m_audioToggle->setChecked(false);
        }
    }
}

void WebSdrPanel::setSliceButtons(const QList<int>& ids, const QStringList& labels,
                                  const QStringList& colors)
{
    if (!m_sliceRow || !m_sliceGroup) {
        return;
    }
    for (QAbstractButton* b : m_sliceGroup->buttons()) {
        m_sliceGroup->removeButton(b);
        b->deleteLater();
    }
    auto* lay = qobject_cast<QHBoxLayout*>(m_sliceRow->layout());
    for (int i = 0; i < ids.size(); ++i) {
        auto* btn = new QPushButton(i < labels.size() ? labels[i] : QString::number(ids[i]), m_sliceRow);
        btn->setCheckable(true);
        btn->setFixedSize(22, 22);   // standard button height (style guide §Layout)
        const QString hex = (i < colors.size() && !colors[i].isEmpty()) ? colors[i]
                                                                        : QStringLiteral("#00d4ff");
        // Outlined in the slice colour; filled with it when selected (following).
        // 10px bold matches the standard button text (style guide §Typography).
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { border: 1px solid %1; border-radius: 3px; color: %1;"
            " background: transparent; padding: 0px; font-size: 10px; font-weight: bold; }"
            "QPushButton:checked { background: %1; color: #000; }").arg(hex));
        btn->setToolTip(tr("Follow Flex slice %1 (read-only)").arg(btn->text()));
        btn->setAccessibleName(tr("Follow slice %1").arg(btn->text()));
        if (ids[i] == m_followingId) {
            btn->setChecked(true);
        }
        if (lay) {
            lay->addWidget(btn);
        }
        m_sliceGroup->addButton(btn, ids[i]);
    }
    if (m_followingId >= 0 && !ids.contains(m_followingId)) {   // followed slice gone
        m_followingId = -1;
        m_freq->setEnabled(true);
        emit followSliceChanged(-1);
    }
}

void WebSdrPanel::setBandSpan(double centerKHz, double srKHz, const QString& name)
{
    if (!name.isEmpty()) {
        m_bandLabel->setText(tr("band: %1").arg(name));
    }
    if (m_waterfall) {
        m_waterfall->setGeometryInfo(centerKHz, srKHz);
    }
}

void WebSdrPanel::addWaterfallRow(const QImage& row)
{
    if (m_waterfall) {
        m_waterfall->addRow(row);
    }
}

void WebSdrPanel::loadSettings()
{
    // Nested JSON under one root key per feature (Constitution Principle V).
    const QJsonObject o = QJsonDocument::fromJson(
        AppSettings::instance().value("WebSdr").toString().toUtf8()).object();
    m_host->setText(o.value("host").toString());   // no default — user enters their WebSDR
    m_freq->setValue(o.value("freqKHz").toDouble(3557.0));
    m_mode = o.value("mode").toString(QStringLiteral("CW"));
}

void WebSdrPanel::saveSettings() const
{
    // Whole config regenerated and written atomically (Principles V + XIV).
    QJsonObject o;
    o["host"]    = m_host->text().trimmed();
    o["freqKHz"] = m_freq->value();
    o["mode"]    = m_mode;
    auto& s = AppSettings::instance();
    s.setValue("WebSdr", QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    s.save();
}

} // namespace AetherSDR
