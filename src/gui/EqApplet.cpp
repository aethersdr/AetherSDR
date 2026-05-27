#include "EqApplet.h"
#include "GuardedSlider.h"
#include "core/AppSettings.h"

#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include "core/ThemeManager.h"

namespace AetherSDR {

// ── ResetButton: draws a 3/4-circle revert arrow ─────────────────────────────

class ResetButton : public QPushButton {
public:
    explicit ResetButton(QWidget* parent = nullptr)
        : QPushButton(parent)
    {
        setFixedSize(22, 22);
        setCursor(Qt::PointingHandCursor);
        setToolTip("Reset all bands to 0 dB");
        AetherSDR::ThemeManager::instance().applyStyleSheet(this, "QPushButton { background-color: {{color.background.1}}; border: 1px solid {{color.background.2}}; "
            "border-radius: 3px; }"
            "QPushButton:hover { background-color: {{color.background.1}}; }"
            "QPushButton:pressed { background-color: {{color.accent}}; }");
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        QPushButton::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int cx = width() / 2;
        const int cy = height() / 2;
        const int r = 6;

        // 3/4 arc: gap at the bottom (6 o'clock), mirrored for "undo" look.
        // Qt angles: 0 = 3 o'clock, counter-clockwise positive, units 1/16 deg.
        // Start at 225 deg (7:30 position), sweep -270 deg (CW) to -45 deg (4:30).
        QPen pen(QColor(0xc8, 0xd8, 0xe8), 1.5);
        p.setPen(pen);
        p.drawArc(cx - r, cy - r, r * 2, r * 2, 225 * 16, -270 * 16);

        // Arrowhead at the end of the arc (225 deg = lower-left).
        const double angle = 225.0 * M_PI / 180.0;
        const double ex = cx + r * std::cos(angle);
        const double ey = cy - r * std::sin(angle);
        // Arrow points clockwise (toward upper-left)
        p.drawLine(QPointF(ex, ey), QPointF(ex - 4, ey - 1));
        p.drawLine(QPointF(ex, ey), QPointF(ex + 1, ey - 4));
    }
};



// ── Style constants ──────────────────────────────────────────────────────────

static const QString kBtnBase =
    "QPushButton { background-color: #1a2a3a; color: #c8d8e8; "
    "border: 1px solid #205070; border-radius: 3px; font-size: 11px; "
    "font-weight: bold; padding: 2px 4px; }";

static const QString kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

static const QString kBlueActive =
    "QPushButton:checked { background-color: #0070c0; color: #ffffff; "
    "border: 1px solid #0090e0; }";

// Vertical EQ band slider style — thin groove, accent-coloured handle.
// Sizes kept site-local (intentional compact dimensions for the band
// column rhythm); colours routed through color.slider.* so per-applet
// override + live theme switching both light up.  Handle uses the
// foreground (fill) token rather than the canonical handle token
// because the EQ slider's visual idiom puts the accent colour on the
// handle itself (there's no sub-page fill rule here).
static constexpr const char* kVSliderStyle =
    "QSlider::groove:vertical { width: 4px; background: {{color.slider.background}}; border-radius: 2px; }"
    "QSlider::handle:vertical { height: 10px; width: 16px; margin: 0 -6px;"
    "background: {{color.slider.foreground}}; border-radius: 5px; }";

// Drag-popup formatter for EQ band sliders.  Matches the file-scope
// free-function shape used by the other applets (wattsText, percentText,
// panText) so the EQ sliders read consistently with neighbouring code.
static QString dbWithSignText(int v)
{
    return QStringLiteral("%1%2 dB")
        .arg(v > 0 ? QStringLiteral("+") : QString())
        .arg(v);
}

// ── EqApplet ────────────────────────────────────────────────────────────────

EqApplet::EqApplet(QWidget* parent)
    : QWidget(parent)
{
    theme::setContainer(this, QStringLiteral("applet/eq"));
    buildUI();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setVisible(false);
}

void EqApplet::buildUI()
{
    // Restore last-selected RX/TX view from settings (Principle V: nested
    // JSON per feature). Defaults to TX so first-launch behavior is unchanged.
    {
        auto& s = AppSettings::instance();
        const QJsonObject cfg = QJsonDocument::fromJson(
            s.value("EqApplet", "{}").toString().toUtf8()).object();
        m_showTx = cfg.value("showTx").toBool(true);
    }

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* vbox = new QVBoxLayout;
    vbox->setContentsMargins(4, 4, 4, 4);
    vbox->setSpacing(4);
    outer->addLayout(vbox);

    // ── Control row: ON | RX | TX ───────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(4);

        m_onBtn = new QPushButton("ON");
        m_onBtn->setCheckable(true);
        m_onBtn->setFixedSize(36, 22);
        m_onBtn->setAccessibleName("Equalizer enable");
        m_onBtn->setAccessibleDescription("Toggle equalizer on or off");
        m_onBtn->setStyleSheet(kBtnBase + kGreenActive);
        connect(m_onBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_model) {
                if (m_showTx)
                    m_model->setTxEnabled(on);
                else
                    m_model->setRxEnabled(on);
            }
        });
        row->addWidget(m_onBtn);

        auto* resetBtn = new ResetButton;
        m_resetBtn = resetBtn;
        connect(resetBtn, &QPushButton::clicked, this, [this]() {
            if (!m_model) return;
            for (int i = 0; i < EqualizerModel::BandCount; ++i) {
                auto b = static_cast<EqualizerModel::Band>(i);
                if (m_showTx)
                    m_model->setTxBand(b, 0);
                else
                    m_model->setRxBand(b, 0);
            }
        });
        row->addWidget(m_resetBtn);

        row->addStretch();

        m_rxBtn = new QPushButton("RX");
        m_rxBtn->setCheckable(true);
        m_rxBtn->setChecked(!m_showTx);
        m_rxBtn->setFixedSize(36, 22);
        m_rxBtn->setAccessibleName("RX equalizer");
        m_rxBtn->setAccessibleDescription("Show receive equalizer bands");
        m_rxBtn->setStyleSheet(kBtnBase + kBlueActive);
        row->addWidget(m_rxBtn);

        m_txBtn = new QPushButton("TX");
        m_txBtn->setCheckable(true);
        m_txBtn->setChecked(m_showTx);
        m_txBtn->setFixedSize(36, 22);
        m_txBtn->setAccessibleName("TX equalizer");
        m_txBtn->setAccessibleDescription("Show transmit equalizer bands");
        m_txBtn->setStyleSheet(kBtnBase + kBlueActive);
        row->addWidget(m_txBtn);

        // RX/TX are mutually exclusive
        connect(m_rxBtn, &QPushButton::clicked, this, [this]() {
            m_showTx = false;
            m_rxBtn->setChecked(true);
            m_txBtn->setChecked(false);
            persistViewSelection();
            syncFromModel();
        });
        connect(m_txBtn, &QPushButton::clicked, this, [this]() {
            m_showTx = true;
            m_txBtn->setChecked(true);
            m_rxBtn->setChecked(false);
            persistViewSelection();
            syncFromModel();
        });

        vbox->addLayout(row);
    }

    // ── Band label row ──────────────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(0);

        // Left spacer for dB scale alignment
        auto* spacer = new QWidget;
        spacer->setFixedWidth(20);
        row->addWidget(spacer);

        for (int i = 0; i < EqualizerModel::BandCount; ++i) {
            auto* lbl = new QLabel(EqualizerModel::bandLabel(static_cast<EqualizerModel::Band>(i)));
            lbl->setAlignment(Qt::AlignCenter);
            AetherSDR::ThemeManager::instance().applyStyleSheet(lbl, "QLabel { color: {{color.text.secondary}}; font-size: 10px; }");
            row->addWidget(lbl, 1);
        }

        // Right spacer
        auto* spacer2 = new QWidget;
        spacer2->setFixedWidth(20);
        row->addWidget(spacer2);

        vbox->addLayout(row);
    }

    // ── Slider row with dB scale ────────────────────────────────────────────
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(0);

        // Left dB scale
        {
            auto* scaleCol = new QVBoxLayout;
            scaleCol->setSpacing(0);
            auto* topLbl = new QLabel("+10");
            AetherSDR::ThemeManager::instance().applyStyleSheet(topLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            topLbl->setAlignment(Qt::AlignRight | Qt::AlignTop);
            topLbl->setFixedWidth(20);
            scaleCol->addWidget(topLbl);
            scaleCol->addStretch();
            auto* midLbl = new QLabel("0");
            AetherSDR::ThemeManager::instance().applyStyleSheet(midLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            midLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            midLbl->setFixedWidth(20);
            scaleCol->addWidget(midLbl);
            scaleCol->addStretch();
            auto* botLbl = new QLabel("-10");
            AetherSDR::ThemeManager::instance().applyStyleSheet(botLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            botLbl->setAlignment(Qt::AlignRight | Qt::AlignBottom);
            botLbl->setFixedWidth(20);
            scaleCol->addWidget(botLbl);
            row->addLayout(scaleCol);
        }

        // 8 vertical sliders
        for (int i = 0; i < EqualizerModel::BandCount; ++i) {
            auto* col = new QVBoxLayout;
            col->setSpacing(1);
            col->setContentsMargins(0, 0, 0, 0);

            auto* slider = new GuardedSlider(Qt::Vertical);
            slider->setRange(-10, 10);
            slider->setValue(0);
            slider->setDragValueFormatter(dbWithSignText);
            slider->setTickPosition(QSlider::NoTicks);
            AetherSDR::ThemeManager::instance().applyStyleSheet(slider, kVSliderStyle);
            slider->setFixedHeight(100);
            slider->setAccessibleName(
                QString("EQ %1").arg(EqualizerModel::bandLabel(static_cast<EqualizerModel::Band>(i))));
            slider->setAccessibleDescription(
                QString("Equalizer band %1, minus 10 to plus 10 dB").arg(
                    EqualizerModel::bandLabel(static_cast<EqualizerModel::Band>(i))));
            m_sliders[i] = slider;

            col->addWidget(slider, 0, Qt::AlignHCenter);

            auto* valLbl = new QLabel("0");
            valLbl->setAlignment(Qt::AlignCenter);
            AetherSDR::ThemeManager::instance().applyStyleSheet(valLbl, "QLabel { color: {{color.text.primary}}; font-size: 9px; }");
            m_valueLabels[i] = valLbl;
            col->addWidget(valLbl);

            const int bandIdx = i;
            connect(slider, &QSlider::valueChanged, this, [this, bandIdx](int v) {
                m_valueLabels[bandIdx]->setText(QString::number(v));
                if (!m_updatingFromModel && m_model) {
                    auto b = static_cast<EqualizerModel::Band>(bandIdx);
                    if (m_showTx)
                        m_model->setTxBand(b, v);
                    else
                        m_model->setRxBand(b, v);
                }
            });

            row->addLayout(col, 1);
        }

        // Right dB scale
        {
            auto* scaleCol = new QVBoxLayout;
            scaleCol->setSpacing(0);
            auto* topLbl = new QLabel("+10");
            AetherSDR::ThemeManager::instance().applyStyleSheet(topLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            topLbl->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            topLbl->setFixedWidth(20);
            scaleCol->addWidget(topLbl);
            scaleCol->addStretch();
            auto* midLbl = new QLabel("0");
            AetherSDR::ThemeManager::instance().applyStyleSheet(midLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            midLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            midLbl->setFixedWidth(20);
            scaleCol->addWidget(midLbl);
            scaleCol->addStretch();
            auto* botLbl = new QLabel("-10");
            AetherSDR::ThemeManager::instance().applyStyleSheet(botLbl, "QLabel { color: {{color.text.secondary}}; font-size: 9px; }");
            botLbl->setAlignment(Qt::AlignLeft | Qt::AlignBottom);
            botLbl->setFixedWidth(20);
            scaleCol->addWidget(botLbl);
            row->addLayout(scaleCol);
        }

        vbox->addLayout(row);
    }
}

void EqApplet::persistViewSelection()
{
    auto& s = AppSettings::instance();
    QJsonObject cfg = QJsonDocument::fromJson(
        s.value("EqApplet", "{}").toString().toUtf8()).object();
    cfg["showTx"] = m_showTx;
    s.setValue("EqApplet", QString::fromUtf8(
        QJsonDocument(cfg).toJson(QJsonDocument::Compact)));
    s.save();
}

// ── Model binding ────────────────────────────────────────────────────────────

void EqApplet::setEqualizerModel(EqualizerModel* model)
{
    m_model = model;
    if (!model) return;

    connect(model, &EqualizerModel::txStateChanged, this, [this]() {
        if (m_showTx) syncFromModel();
    });
    connect(model, &EqualizerModel::rxStateChanged, this, [this]() {
        if (!m_showTx) syncFromModel();
    });
    syncFromModel();
}

void EqApplet::syncFromModel()
{
    if (!m_model) return;
    m_updatingFromModel = true;

    // ON button reflects current view
    {
        QSignalBlocker b(m_onBtn);
        m_onBtn->setChecked(m_showTx ? m_model->txEnabled() : m_model->rxEnabled());
    }

    // Sliders
    for (int i = 0; i < EqualizerModel::BandCount; ++i) {
        auto band = static_cast<EqualizerModel::Band>(i);
        int val = m_showTx ? m_model->txBand(band) : m_model->rxBand(band);
        QSignalBlocker b(m_sliders[i]);
        m_sliders[i]->setValue(val);
        m_valueLabels[i]->setText(QString::number(val));
    }

    m_updatingFromModel = false;
}

} // namespace AetherSDR
