#include "TxBandDialog.h"

#include "ControlAvailabilityRegistry.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

namespace AetherSDR {
using BandField = TxBandRequest::Field;

TxBandDialog::TxBandDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(
        QStringLiteral("TX Band Settings (Current TX Profile: %1)")
            .arg(model ? model->transmitModel().activeProfile() : QString()),
        QStringLiteral("TxBandDialogGeometry"),
        parent),
      m_model(model)
{
    theme::setContainer(this, QStringLiteral("dialog/txBand"));
    setMinimumSize(700, 450);
    setStyleSheet(QStringLiteral("QDialog { background: #0f0f1a; }"));

    auto* vb = new QVBoxLayout(bodyWidget());
    vb->setSpacing(9);

    auto* gridContainer = new QWidget;
    gridContainer->setStyleSheet(QStringLiteral("background: #506070;"));
    auto* headerGrid = new QGridLayout(gridContainer);
    headerGrid->setContentsMargins(1, 1, 1, 1);
    headerGrid->setSpacing(1);
    const QStringList headers = {"Band", "RF PWR(%)", "Tune PWR(%)", "PTT Inhibit",
                                  "ACC TX", "RCA TX Req", "ACC TX Req",
                                  "RCA TX1", "RCA TX2", "RCA TX3", "HWALC",
                                  "RCA off/TUNE"};
    for (int c = 0; c < headers.size(); ++c) {
        auto* lbl = new QLabel(headers[c]);
        lbl->setStyleSheet(QStringLiteral(
            "QLabel { color: #8aa8c0; font-size: 10px; "
            "font-weight: bold; background: #1a2a3a; "
            "padding: 2px 4px; }"));
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setMinimumSize(lbl->sizeHint());
        headerGrid->addWidget(lbl, 0, c);
    }

    if (!m_model) {
        vb->addWidget(gridContainer);
        vb->addStretch();
        return;
    }

    auto* availability = new ControlAvailabilityRegistry(*m_model, this);
    connect(m_model, &RadioModel::tuneOutputControlChanged, availability,
            &ControlAvailabilityRegistry::refreshEngaged);
    const auto& bands = m_model->txBandSettings();
    const RadioControlSession session = m_model->controlSession();
    QList<int> sortedIds = bands.keys();
    std::sort(sortedIds.begin(), sortedIds.end());

    static const QString kEditStyle = QStringLiteral(
        "QLineEdit { background: #0a0a18; color: #c8d8e8; border: 1px solid #304050; "
        "padding: 2px; font-size: 11px; }");
    static const QString kCbStyle = QStringLiteral(
        "QCheckBox { spacing: 0px; }"
        "QCheckBox::indicator { width: 14px; height: 14px; }");

    int row = 1;
    for (int id : sortedIds) {
        const auto& b = bands[id];
        int col = 0;

        auto* nameLbl = new QLabel(b.bandName);
        nameLbl->setStyleSheet(QStringLiteral(
            "QLabel { color: #c8d8e8; font-size: 11px; "
            "font-weight: bold; background: #0f0f1a; "
            "border: 1px solid #203040; padding: 2px 4px; }"));
        headerGrid->addWidget(nameLbl, row, col++);

        auto* rfEdit = new QLineEdit(QString::number(b.rfPower));
        rfEdit->setStyleSheet(kEditStyle);
        rfEdit->setFixedWidth(50);
        rfEdit->setAlignment(Qt::AlignCenter);
        int bandId = id;
        connect(rfEdit, &QLineEdit::editingFinished, this, [this, rfEdit, bandId, session] {
            bool valid = false;
            const int parsed = rfEdit->text().toInt(&valid);
            if (!valid) { rfEdit->setText(QString::number(m_model->txBandSettings().value(bandId).rfPower)); return; }
            const int percent = qBound(0, parsed, 100);
            rfEdit->setText(QString::number(percent));
            m_model->requestTxBand({bandId, BandField::RfPowerPercent, percent}, session);
        });
        headerGrid->addWidget(rfEdit, row, col++);

        auto* tuneEdit = new QLineEdit(QString::number(b.tunePower));
        tuneEdit->setStyleSheet(kEditStyle);
        tuneEdit->setFixedWidth(50);
        tuneEdit->setAlignment(Qt::AlignCenter);
        connect(tuneEdit, &QLineEdit::editingFinished, this, [this, tuneEdit, bandId, session] {
            bool valid = false;
            const int parsed = tuneEdit->text().toInt(&valid);
            if (!valid) { tuneEdit->setText(QString::number(m_model->txBandSettings().value(bandId).tunePower)); return; }
            const int percent = qBound(0, parsed, 100);
            tuneEdit->setText(QString::number(percent));
            m_model->requestTxBand({bandId, BandField::TunePowerPercent, percent}, session);
        });
        headerGrid->addWidget(tuneEdit, row, col++);

        struct CbDef { bool val; BandField field; };
        CbDef cbs[] = {
            {b.inhibit, BandField::Inhibit},
            {b.accTx, BandField::AccessoryTx},
            {b.rcaTxReq, BandField::RcaTxRequest},
            {b.accTxReq, BandField::AccessoryTxRequest},
            {b.tx1, BandField::Tx1},
            {b.tx2, BandField::Tx2},
            {b.tx3, BandField::Tx3},
            {b.hwAlc, BandField::HardwareAlc},
        };

        for (const auto& cb : cbs) {
            auto* chk = new QCheckBox;
            chk->setChecked(cb.val);
            chk->setStyleSheet(kCbStyle);
            auto* w = new QWidget;
            w->setStyleSheet(QStringLiteral("background: #0f0f1a;"));
            auto* hb = new QHBoxLayout(w);
            hb->setContentsMargins(0, 0, 0, 0);
            hb->setAlignment(Qt::AlignCenter);
            hb->addWidget(chk);
            const BandField field = cb.field;
            connect(chk, &QCheckBox::toggled, this, [this, bandId, field, session](bool on) {
                m_model->requestTxBand({bandId, field, on}, session);
            });
            headerGrid->addWidget(w, row, col++);
        }
        auto* suppression = new QCheckBox;
        suppression->setObjectName(QStringLiteral("tuneOutputSuppressionBand%1").arg(bandId));
        suppression->setAccessibleName(tr("Suppress RCA during TUNE, band %1").arg(b.bandName));
        ThemeManager::instance().applyStyleSheet(suppression, kCbStyle);
        auto* suppressionCell = new QWidget;
        ThemeManager::instance().applyStyleSheet(suppressionCell,
            QStringLiteral("background: {{color.background.0}};"));
        auto* suppressionLayout = new QHBoxLayout(suppressionCell);
        suppressionLayout->setContentsMargins(0, 0, 0, 0);
        suppressionLayout->setAlignment(Qt::AlignCenter);
        suppressionLayout->addWidget(suppression);
        headerGrid->addWidget(suppressionCell, row, col);
        availability->registerWidget(suppression,
            tr("Requires SmartSDR 4.3 or later and this band's radio readback."),
            [this, bandId](bool connected, const RadioCapabilities& caps) {
                const TuneOutputControlState state = m_model->tuneOutputControl(bandId);
                return connected && caps.tuneOutputSuppression.has_value()
                    && state.haveValue && !state.pending;
            }, [this, bandId] {
                const TuneOutputControlState state = m_model->tuneOutputControl(bandId);
                return state.haveValue && state.suppressed;
            }, false);
        const auto syncSuppression = [this, suppression, bandId] {
            const TuneOutputControlState state = m_model->tuneOutputControl(bandId);
            const QSignalBlocker blocker(suppression);
            suppression->setChecked(state.haveValue && state.suppressed);
            const QString description = state.reason.isEmpty()
                ? tr("Suppress TX1, TX2 and TX3 RCA outputs during TUNE for this band. "
                     "Normal PTT and TX REQ inputs are unchanged.")
                : state.reason;
            suppression->setToolTip(description);
            suppression->setAccessibleDescription(description);
        };
        connect(m_model, &RadioModel::tuneOutputControlChanged, suppression, syncSuppression);
        connect(m_model, &RadioModel::capabilitiesChanged, suppression, syncSuppression);
        connect(suppression, &QCheckBox::clicked, this, [this, bandId, syncSuppression](bool checked) {
            m_model->requestTuneOutputSuppression(bandId, checked);
            syncSuppression();
        });
        syncSuppression();
        ++row;
    }

    vb->addWidget(gridContainer);
    auto* explanation = new QLabel(tr("RCA off/TUNE: suppress TX1/TX2/TX3 keying outputs during TUNE "
                                     "on SmartSDR 4.3 or later. ACC TX remains under Inhibit during TUNE."));
    explanation->setWordWrap(true);
    vb->addWidget(explanation);
    vb->addStretch();
    setMinimumSize(minimumSize().expandedTo(minimumSizeHint()));
}

} // namespace AetherSDR
