#include "TxBandDialog.h"

#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QVBoxLayout>
#include <algorithm>

namespace MasterSDR {

TxBandDialog::TxBandDialog(RadioModel* model, QWidget* parent)
    : PersistentDialog(
        QStringLiteral("TX Band Settings (Current TX Profile: %1)")
            .arg(model ? model->transmitModel().activeProfile() : QString()),
        QStringLiteral("TxBandDialogGeometry"),
        parent),
      m_model(model)
{
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
                                  "RCA TX1", "RCA TX2", "RCA TX3", "HWALC"};
    for (int c = 0; c < headers.size(); ++c) {
        auto* lbl = new QLabel(headers[c]);
        lbl->setStyleSheet(QStringLiteral(
            "QLabel { color: #8aa8c0; font-size: 10px; "
            "font-weight: bold; background: #1a2a3a; "
            "padding: 2px 4px; }"));
        lbl->setAlignment(Qt::AlignCenter);
        headerGrid->addWidget(lbl, 0, c);
    }

    if (!m_model) {
        vb->addWidget(gridContainer);
        vb->addStretch();
        return;
    }

    const auto& bands = m_model->txBandSettings();
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
        connect(rfEdit, &QLineEdit::editingFinished, this, [this, rfEdit, bandId] {
            m_model->sendCommand(
                QStringLiteral("transmit bandset %1 rfpower=%2").arg(bandId).arg(rfEdit->text()));
        });
        headerGrid->addWidget(rfEdit, row, col++);

        auto* tuneEdit = new QLineEdit(QString::number(b.tunePower));
        tuneEdit->setStyleSheet(kEditStyle);
        tuneEdit->setFixedWidth(50);
        tuneEdit->setAlignment(Qt::AlignCenter);
        connect(tuneEdit, &QLineEdit::editingFinished, this, [this, tuneEdit, bandId] {
            m_model->sendCommand(
                QStringLiteral("transmit bandset %1 tunepower=%2").arg(bandId).arg(tuneEdit->text()));
        });
        headerGrid->addWidget(tuneEdit, row, col++);

        struct CbDef { bool val; const char* txCmd; const char* ilCmd; };
        CbDef cbs[] = {
            {b.inhibit, "inhibit", nullptr},
            {b.accTx,   nullptr, "acc_tx_enabled"},
            {b.rcaTxReq,nullptr, "rca_txreq_enable"},
            {b.accTxReq,nullptr, "acc_txreq_enable"},
            {b.tx1,     nullptr, "tx1_enabled"},
            {b.tx2,     nullptr, "tx2_enabled"},
            {b.tx3,     nullptr, "tx3_enabled"},
            {b.hwAlc,   "hwalc_enabled", nullptr},
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
            const char* txC = cb.txCmd;
            const char* ilC = cb.ilCmd;
            connect(chk, &QCheckBox::toggled, this, [this, bandId, txC, ilC](bool on) {
                if (txC)
                    m_model->sendCommand(
                        QStringLiteral("transmit bandset %1 %2=%3").arg(bandId).arg(txC).arg(on ? 1 : 0));
                if (ilC)
                    m_model->sendCommand(
                        QStringLiteral("interlock bandset %1 %2=%3").arg(bandId).arg(ilC).arg(on ? 1 : 0));
            });
            headerGrid->addWidget(w, row, col++);
        }
        ++row;
    }

    vb->addWidget(gridContainer);
    vb->addStretch();
}

} // namespace MasterSDR
