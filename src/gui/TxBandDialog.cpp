#include "TxBandDialog.h"

#include "CanonIndicators.h"
#include "ControlAvailabilityRegistry.h"
#include "FramelessResizer.h"

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

namespace {

// The table is a grid of cells over a hairline-coloured ground: the 1 px
// spacing between cells draws the rules. The body and everything in it start
// transparent over the canon ground; the cell rules are scoped under #canonBody
// so they outrank that.
constexpr const char* kTxBandStyle = R"(
QWidget#canonBody, QWidget#canonBody QWidget {
    background: transparent;
}
QWidget#canonBody {
    color: {{color.canon.inkSoft}};
}
QWidget#canonBody QWidget#txBandGrid {
    background: {{color.canon.line}};
}
QWidget#canonBody QLabel[txBandCell="header"] {
    color: {{color.canon.cyan}};
    background: {{color.canon.raised}};
    font-size: 10px;
    font-weight: 700;
    padding: 3px 5px;
}
QWidget#canonBody QLabel[txBandCell="band"] {
    color: {{color.canon.ink}};
    background: {{color.canon.nested}};
    font-size: 11px;
    font-weight: 700;
    padding: 2px 6px;
}
QWidget#canonBody QWidget[txBandCell="value"] {
    background: {{color.canon.nested}};
}
QWidget#canonBody QLineEdit {
    color: {{color.canon.ink}};
    background: {{color.canon.control}};
    border: 1px solid {{color.canon.lineHi}};
    border-radius: 4px;
    padding: 2px;
    font-size: 11px;
}
QWidget#canonBody QLineEdit:focus {
    border-color: {{color.canon.aqua}};
}
QWidget#canonBody QCheckBox {
    spacing: 0px;
}
QWidget#canonBody QLabel[txBandCell="note"] {
    color: {{color.canon.muted}};
    font-size: 11px;
}
)";

// A cell that centres one widget, painted as a table cell.
QWidget* valueCell(QWidget* content)
{
    auto* cell = new QWidget;
    cell->setProperty("txBandCell", QStringLiteral("value"));
    auto* layout = new QHBoxLayout(cell);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(content);
    return cell;
}

} // namespace

TxBandDialog::TxBandDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(
        QStringLiteral("TX Band Settings (Current TX Profile: %1)")
            .arg(model ? model->transmitModel().activeProfile() : QString()),
        parent, Kind::Workspace),
      m_model(model)
{
    theme::setContainer(this, QStringLiteral("canon/txBand"));
    // Size and position persist under TxBandDialogGeometry, the key earlier
    // releases wrote, so a saved rectangle still restores; it resizes from
    // every edge.
    setGeometryKey(QStringLiteral("TxBandDialogGeometry"));
    setMinimumSize(700, 450);
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kTxBandStyle) + canonIndicatorRules();
    });

    auto* vb = new QVBoxLayout(bodyWidget());
    vb->setContentsMargins(14, 12, 14, 14);
    vb->setSpacing(10);
    vb->addWidget(makeCanonHeader(windowTitle()));

    // Parented now so each header label is polished by the body's sheet before
    // it is measured: its minimum is the sheet's bold 10 px text, not the
    // default font's.
    auto* gridContainer = new QWidget(bodyWidget());
    gridContainer->setObjectName(QStringLiteral("txBandGrid"));
    auto* headerGrid = new QGridLayout(gridContainer);
    headerGrid->setContentsMargins(1, 1, 1, 1);
    headerGrid->setSpacing(1);
    const QStringList headers = {"Band", "RF PWR(%)", "Tune PWR(%)", "PTT Inhibit",
                                  "ACC TX", "RCA TX Req", "ACC TX Req",
                                  "RCA TX1", "RCA TX2", "RCA TX3", "HWALC",
                                  "RCA off/TUNE"};
    for (int c = 0; c < headers.size(); ++c) {
        auto* lbl = new QLabel(headers[c]);
        lbl->setProperty("txBandCell", QStringLiteral("header"));
        lbl->setAlignment(Qt::AlignCenter);
        headerGrid->addWidget(lbl, 0, c);
        lbl->ensurePolished();
        lbl->setMinimumSize(lbl->sizeHint());
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

    int row = 1;
    for (int id : sortedIds) {
        const auto& b = bands[id];
        int col = 0;

        auto* nameLbl = new QLabel(b.bandName);
        nameLbl->setProperty("txBandCell", QStringLiteral("band"));
        headerGrid->addWidget(nameLbl, row, col++);

        auto* rfEdit = new QLineEdit(QString::number(b.rfPower));
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
        headerGrid->addWidget(valueCell(rfEdit), row, col++);

        auto* tuneEdit = new QLineEdit(QString::number(b.tunePower));
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
        headerGrid->addWidget(valueCell(tuneEdit), row, col++);

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
            const BandField field = cb.field;
            connect(chk, &QCheckBox::toggled, this, [this, bandId, field, session](bool on) {
                m_model->requestTxBand({bandId, field, on}, session);
            });
            headerGrid->addWidget(valueCell(chk), row, col++);
        }
        auto* suppression = new QCheckBox;
        suppression->setObjectName(QStringLiteral("tuneOutputSuppressionBand%1").arg(bandId));
        suppression->setAccessibleName(tr("Suppress RCA during TUNE, band %1").arg(b.bandName));
        headerGrid->addWidget(valueCell(suppression), row, col);
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
    explanation->setProperty("txBandCell", QStringLiteral("note"));
    explanation->setWordWrap(true);
    vb->addWidget(explanation);
    vb->addStretch();
    setMinimumSize(minimumSize().expandedTo(minimumSizeHint()));
}

} // namespace AetherSDR
