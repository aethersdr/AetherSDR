#include "PhoneApplet.h"
#include "GuardedSlider.h"
#include "models/TransmitModel.h"
#include "core/AppSettings.h"

#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QFrame>
#include <QPainter>

namespace AetherSDR {

// ── Triangle step button (reused from RxApplet pattern) ─────────────────────

class PhoneTriBtn : public QPushButton {
public:
    enum Dir { Left, Right };
    explicit PhoneTriBtn(Dir dir, QWidget* parent = nullptr)
        : QPushButton(parent), m_dir(dir)
    {
        setFlat(false);
        setFixedSize(22, 22);
        setStyleSheet(
            "QPushButton { background: #1a2a3a; border: 1px solid #203040; "
            "border-radius: 3px; padding: 0; margin: 0; min-width: 0; min-height: 0; }"
            "QPushButton:hover { background: #203040; }"
            "QPushButton:pressed { background: #00b4d8; }");
    }
protected:
    void paintEvent(QPaintEvent* ev) override {
        QPushButton::paintEvent(ev);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(isDown() ? QColor(0, 0, 0) : QColor(0xc8, 0xd8, 0xe8));
        p.setPen(Qt::NoPen);
        const int cx = width() / 2, cy = height() / 2;
        QPolygon tri;
        if (m_dir == Left)
            tri << QPoint(cx - 5, cy) << QPoint(cx + 4, cy - 5) << QPoint(cx + 4, cy + 5);
        else
            tri << QPoint(cx + 5, cy) << QPoint(cx - 4, cy - 5) << QPoint(cx - 4, cy + 5);
        p.drawPolygon(tri);
    }
private:
    Dir m_dir;
};



// ── Style constants ──────────────────────────────────────────────────────────

static constexpr const char* kSliderStyle =
    "QSlider::groove:horizontal { height: 4px; background: #203040; border-radius: 2px; }"
    "QSlider::handle:horizontal { width: 10px; height: 10px; margin: -3px 0;"
    "background: #00b4d8; border-radius: 5px; }";

static QString percentText(int value)
{
    return QStringLiteral("%1%").arg(value);
}

static const QString kGreenActive =
    "QPushButton:checked { background-color: #006040; color: #00ff88; "
    "border: 1px solid #00a060; }";

static const QString kBlueActive =
    "QPushButton:checked { background-color: #0070c0; color: #ffffff; "
    "border: 1px solid #0090e0; }";

static const QString kBtnBase =
    "QPushButton { background-color: #1a2a3a; color: #c8d8e8; "
    "border: 1px solid #205070; border-radius: 3px; font-size: 11px; "
    "font-weight: bold; padding: 2px 4px; }";

// Small step button style (< > for filter cut)
static const QString kStepBtnStyle =
    "QPushButton { background-color: #1a2a3a; color: #c8d8e8; "
    "border: 1px solid #205070; border-radius: 2px; font-size: 11px; "
    "font-weight: bold; padding: 0px; }";

// ── PhoneApplet ──────────────────────────────────────────────────────────────

PhoneApplet::PhoneApplet(QWidget* parent)
    : QWidget(parent)
{
    buildUI();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setVisible(false);
}

void PhoneApplet::buildUI()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* vbox = new QVBoxLayout;
    vbox->setContentsMargins(4, 2, 8, 4);
    vbox->setSpacing(4);
    outer->addLayout(vbox);

    // ── AM Carrier row ───────────────────────────────────────────────────
    {
        auto* rowW = new QWidget;
        rowW->setFixedHeight(24);
        auto* row = new QHBoxLayout(rowW);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        auto* lbl = new QLabel("AM\nCarrier:");
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        lbl->setFixedWidth(52);
        row->addWidget(lbl);
        row->addSpacing(10);

        m_amCarrierSlider = new GuardedSlider(Qt::Horizontal);
        m_amCarrierSlider->setRange(0, 100);
        static_cast<GuardedSlider*>(m_amCarrierSlider)->setDragValueFormatter(percentText);
        m_amCarrierSlider->setAccessibleName("AM carrier level");
        m_amCarrierSlider->setAccessibleDescription("AM carrier power level, 0 to 100 percent");
        m_amCarrierSlider->setStyleSheet(kSliderStyle);
        connect(m_amCarrierSlider, &QSlider::valueChanged, this, [this](int v) {
            if (!m_updatingFromModel && m_model) m_model->setAmCarrierLevel(v);
            m_amCarrierLabel->setText(QString::number(v));
        });
        row->addWidget(m_amCarrierSlider, 1);

        m_amCarrierLabel = new QLabel("48");
        m_amCarrierLabel->setFixedWidth(26);
        m_amCarrierLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_amCarrierLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
        row->addWidget(m_amCarrierLabel);

        vbox->addWidget(rowW);
    }

    // ── VOX row: toggle + level slider ───────────────────────────────────
    {
        auto* rowW = new QWidget;
        rowW->setFixedHeight(24);
        auto* row = new QHBoxLayout(rowW);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        m_voxBtn = new QPushButton("VOX");
        m_voxBtn->setCheckable(true);
        m_voxBtn->setFixedSize(52, 22);
        m_voxBtn->setAccessibleName("VOX voice-operated transmit");
        m_voxBtn->setAccessibleDescription("Toggle voice-activated transmit");
        m_voxBtn->setStyleSheet(kBtnBase + kGreenActive);
        connect(m_voxBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_model) m_model->setVoxEnable(on);
        });
        row->addWidget(m_voxBtn);
        row->addSpacing(10);

        m_voxLevelSlider = new GuardedSlider(Qt::Horizontal);
        m_voxLevelSlider->setRange(0, 100);
        static_cast<GuardedSlider*>(m_voxLevelSlider)->setDragValueFormatter(percentText);
        m_voxLevelSlider->setAccessibleName("VOX level");
        m_voxLevelSlider->setAccessibleDescription("VOX activation threshold");
        m_voxLevelSlider->setStyleSheet(kSliderStyle);
        connect(m_voxLevelSlider, &QSlider::valueChanged, this, [this](int v) {
            if (!m_updatingFromModel && m_model) m_model->setVoxLevel(v);
            m_voxLevelLabel->setText(QString::number(v));
        });
        row->addWidget(m_voxLevelSlider, 1);

        m_voxLevelLabel = new QLabel("50");
        m_voxLevelLabel->setFixedWidth(26);
        m_voxLevelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_voxLevelLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
        row->addWidget(m_voxLevelLabel);

        vbox->addWidget(rowW);
    }

    // ── VOX delay row ────────────────────────────────────────────────────
    {
        auto* rowW = new QWidget;
        rowW->setFixedHeight(24);
        auto* row = new QHBoxLayout(rowW);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        auto* lbl = new QLabel("Delay:");
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        lbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        lbl->setFixedWidth(52);
        row->addWidget(lbl);
        row->addSpacing(10);

        m_voxDelaySlider = new GuardedSlider(Qt::Horizontal);
        m_voxDelaySlider->setRange(0, 100);
        m_voxDelaySlider->setAccessibleName("VOX delay");
        m_voxDelaySlider->setAccessibleDescription("VOX hang time before returning to receive");
        m_voxDelaySlider->setStyleSheet(kSliderStyle);
        connect(m_voxDelaySlider, &QSlider::valueChanged, this, [this](int v) {
            if (!m_updatingFromModel && m_model) m_model->setVoxDelay(v);
            m_voxDelayLabel->setText(QString::number(v));
        });
        row->addWidget(m_voxDelaySlider, 1);

        m_voxDelayLabel = new QLabel("50");
        m_voxDelayLabel->setFixedWidth(26);
        m_voxDelayLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_voxDelayLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
        row->addWidget(m_voxDelayLabel);

        vbox->addWidget(rowW);
    }

    // ── DEXP row: toggle + level slider ────────────────────────────────
    // NOTE: DEXP (downward expander / noise gate) commands return error
    // 0x5000002D on firmware v1.4.0.0. UI is present but non-functional
    // until the correct protocol command is identified. See GitHub issue.
    {
        auto* rowW = new QWidget;
        rowW->setFixedHeight(24);
        auto* row = new QHBoxLayout(rowW);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);

        m_dexpBtn = new QPushButton("DEXP");
        m_dexpBtn->setCheckable(true);
        m_dexpBtn->setFixedSize(52, 22);
        m_dexpBtn->setAccessibleName("Downward expander");
        m_dexpBtn->setAccessibleDescription("Toggle downward expander noise gate");
        m_dexpBtn->setStyleSheet(kBtnBase + kBlueActive);
        connect(m_dexpBtn, &QPushButton::toggled, this, [this](bool on) {
            if (!m_updatingFromModel && m_model) {
                m_model->setDexp(on);
                auto& s = AppSettings::instance();
                s.setValue("DexpEnabled", on ? "True" : "False");
                s.save();
            }
        });
        row->addWidget(m_dexpBtn);
        row->addSpacing(10);

        m_dexpSlider = new GuardedSlider(Qt::Horizontal);
        m_dexpSlider->setRange(0, 100);
        static_cast<GuardedSlider*>(m_dexpSlider)->setDragValueFormatter(percentText);
        m_dexpSlider->setAccessibleName("DEXP threshold");
        m_dexpSlider->setAccessibleDescription("Downward expander gate threshold");
        m_dexpSlider->setStyleSheet(kSliderStyle);
        connect(m_dexpSlider, &QSlider::valueChanged, this, [this](int v) {
            if (!m_updatingFromModel && m_model) {
                m_model->setDexpLevel(v);
                auto& s = AppSettings::instance();
                s.setValue("DexpLevel", QString::number(v));
                s.save();
            }
            m_dexpLabel->setText(QString::number(v));
        });
        row->addWidget(m_dexpSlider, 1);

        m_dexpLabel = new QLabel("0");
        m_dexpLabel->setFixedWidth(26);
        m_dexpLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_dexpLabel->setStyleSheet("QLabel { color: #c8d8e8; font-size: 11px; }");
        row->addWidget(m_dexpLabel);

        vbox->addWidget(rowW);
    }

    // ── TX filter section ────────────────────────────────────────────────
    // Two columns: Low Cut (left) and High Cut (right), each with header
    // centered over < value > step buttons.
    {
        auto* grid = new QHBoxLayout;
        grid->setSpacing(0);

        // ── Left column: Low Cut ─────────────────────────────────────────
        auto* lowCol = new QVBoxLayout;
        lowCol->setSpacing(1);

        auto* lowLbl = new QLabel("Low Cut");
        lowLbl->setAlignment(Qt::AlignCenter);
        lowLbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        lowCol->addWidget(lowLbl);

        auto* lowRow = new QHBoxLayout;
        lowRow->setSpacing(2);

        auto* txLbl = new QLabel("TX");
        txLbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; font-weight: bold; }");
        txLbl->setFixedWidth(18);
        lowRow->addWidget(txLbl);

        m_lowCutDown = new PhoneTriBtn(PhoneTriBtn::Left);
        m_lowCutDown->setAccessibleName("TX low cut decrease");
        // Step buttons snap the value to the next multiple of 50 Hz in
        // the chosen direction (rather than the old +/-50 from current).
        // Example: at 87 Hz, ▲ → 100, ▼ → 50.  The radio accepts any
        // integer Hz so this is purely a UI nicety.
        auto lowCutDown = [this]() {
            if (!m_model) return;
            const int v = m_model->txFilterLow();
            const int snapped = ((v - 1) / 50) * 50;
            m_model->setTxFilterLow(qMax(0, snapped));
        };
        auto lowCutUp = [this]() {
            if (!m_model) return;
            const int v = m_model->txFilterLow();
            const int snapped = ((v / 50) + 1) * 50;
            m_model->setTxFilterLow(qMin(m_model->txFilterHigh() - 50, snapped));
        };
        connect(m_lowCutDown, &QPushButton::clicked, this, lowCutDown);
        lowRow->addWidget(m_lowCutDown);

        m_lowCutLabel = new ScrollableLabel("50");
        m_lowCutLabel->setAccessibleName("TX low cut frequency");
        m_lowCutLabel->setFixedWidth(46);
        m_lowCutLabel->setAlignment(Qt::AlignCenter);
        m_lowCutLabel->setStyleSheet(
            "QLabel { font-size: 11px; color: #c8d8e8; background: #0a0a18; "
            "border: 1px solid #1e2e3e; border-radius: 3px; padding: 1px 3px; }");
        connect(m_lowCutLabel, &ScrollableLabel::scrolled, this,
                [lowCutUp, lowCutDown](int dir) {
            if (dir > 0) lowCutUp(); else lowCutDown();
        });
        lowRow->addWidget(m_lowCutLabel);

        m_lowCutUp = new PhoneTriBtn(PhoneTriBtn::Right);
        m_lowCutUp->setAccessibleName("TX low cut increase");
        connect(m_lowCutUp, &QPushButton::clicked, this, lowCutUp);
        lowRow->addWidget(m_lowCutUp);

        lowCol->addLayout(lowRow);
        grid->addLayout(lowCol);

        grid->addStretch();

        // ── Right column: High Cut ───────────────────────────────────────
        auto* highCol = new QVBoxLayout;
        highCol->setSpacing(1);

        auto* highLbl = new QLabel("High Cut");
        highLbl->setAlignment(Qt::AlignCenter);
        highLbl->setStyleSheet("QLabel { color: #8090a0; font-size: 11px; }");
        highCol->addWidget(highLbl);

        auto* highRow = new QHBoxLayout;
        highRow->setSpacing(2);

        m_highCutDown = new PhoneTriBtn(PhoneTriBtn::Left);
        m_highCutDown->setAccessibleName("TX high cut decrease");
        auto highCutDown = [this]() {
            if (!m_model) return;
            const int v = m_model->txFilterHigh();
            const int snapped = ((v - 1) / 50) * 50;
            m_model->setTxFilterHigh(qMax(m_model->txFilterLow() + 50, snapped));
        };
        auto highCutUp = [this]() {
            if (!m_model) return;
            const int v = m_model->txFilterHigh();
            const int snapped = ((v / 50) + 1) * 50;
            m_model->setTxFilterHigh(qMin(10000, snapped));
        };
        connect(m_highCutDown, &QPushButton::clicked, this, highCutDown);
        highRow->addWidget(m_highCutDown);

        m_highCutLabel = new ScrollableLabel("3300");
        m_highCutLabel->setAccessibleName("TX high cut frequency");
        m_highCutLabel->setFixedWidth(46);
        m_highCutLabel->setAlignment(Qt::AlignCenter);
        m_highCutLabel->setStyleSheet(
            "QLabel { font-size: 11px; color: #c8d8e8; background: #0a0a18; "
            "border: 1px solid #1e2e3e; border-radius: 3px; padding: 1px 3px; }");
        connect(m_highCutLabel, &ScrollableLabel::scrolled, this,
                [highCutUp, highCutDown](int dir) {
            if (dir > 0) highCutUp(); else highCutDown();
        });
        highRow->addWidget(m_highCutLabel);

        m_highCutUp = new PhoneTriBtn(PhoneTriBtn::Right);
        m_highCutUp->setAccessibleName("TX high cut increase");
        connect(m_highCutUp, &QPushButton::clicked, this, highCutUp);
        highRow->addWidget(m_highCutUp);

        highCol->addLayout(highRow);
        grid->addLayout(highCol);

        vbox->addLayout(grid);
    }

}

// ── Model binding ────────────────────────────────────────────────────────────

void PhoneApplet::setTransmitModel(TransmitModel* model)
{
    m_model = model;
    if (!model) return;

    connect(model, &TransmitModel::phoneStateChanged, this, &PhoneApplet::syncFromModel);
    syncFromModel();
}

void PhoneApplet::syncFromModel()
{
    if (!m_model) return;
    m_updatingFromModel = true;

    // AM Carrier
    {
        QSignalBlocker b(m_amCarrierSlider);
        m_amCarrierSlider->setValue(m_model->amCarrierLevel());
        m_amCarrierLabel->setText(QString::number(m_model->amCarrierLevel()));
    }

    // VOX
    {
        QSignalBlocker b(m_voxBtn);
        m_voxBtn->setChecked(m_model->voxEnable());
    }
    {
        QSignalBlocker b(m_voxLevelSlider);
        m_voxLevelSlider->setValue(m_model->voxLevel());
        m_voxLevelLabel->setText(QString::number(m_model->voxLevel()));
    }
    {
        QSignalBlocker b(m_voxDelaySlider);
        m_voxDelaySlider->setValue(m_model->voxDelay());
        m_voxDelayLabel->setText(QString::number(m_model->voxDelay()));
    }

    // DEXP
    {
        QSignalBlocker b(m_dexpBtn);
        m_dexpBtn->setChecked(m_model->dexpOn());
    }
    {
        QSignalBlocker b(m_dexpSlider);
        m_dexpSlider->setValue(m_model->dexpLevel());
        m_dexpLabel->setText(QString::number(m_model->dexpLevel()));
    }

    // TX filter
    m_lowCutLabel->setText(QString::number(m_model->txFilterLow()));
    m_highCutLabel->setText(QString::number(m_model->txFilterHigh()));

    m_updatingFromModel = false;
}

} // namespace AetherSDR
