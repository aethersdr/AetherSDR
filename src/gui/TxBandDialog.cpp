#include "TxBandDialog.h"

#include "CanonIndicators.h"
#include "ControlAvailabilityRegistry.h"
#include "FramelessResizer.h"

#include "core/ThemeManager.h"
#include "models/RadioModel.h"
#include "models/TransmitModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QHideEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <tuple>
#include <utility>

namespace AetherSDR {
using BandField = TxBandRequest::Field;

namespace {

// The table is a grid of cells over a hairline-coloured ground: the 1 px
// spacing between cells draws the rules. The body and everything in it start
// transparent over the canon ground; the cell rules are scoped under #canonBody
// so they outrank that, and popups (a field's right-click menu) are
// descendants too, so they get their panel back.
constexpr const char* kTxBandStyle = R"(
QWidget#canonBody, QWidget#canonBody QWidget {
    background: transparent;
}
QWidget#canonBody QMenu {
    color: {{color.canon.inkSoft}};
    background: {{color.canon.raised}};
    border: 1px solid {{color.canon.lineHi}};
}
QWidget#canonBody QMenu::item {
    padding: 4px 18px;
}
QWidget#canonBody QMenu::item:selected {
    color: {{color.canon.onAccent}};
    background: {{color.canon.cyan}};
}
QWidget#canonBody QMenu::item:disabled {
    color: {{color.control.unavailable}};
}
QWidget#canonBody QMenu::separator {
    height: 1px;
    margin: 4px 8px;
    background: {{color.canon.line}};
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

// The eight bandset check-box columns, in table order: the column header and
// the spoken name each check box carries.
struct ToggleColumn {
    BandField field;
    const char* header;
    const char* name;
};
#define TXB(text) QT_TRANSLATE_NOOP("AetherSDR::TxBandDialog", text)
constexpr ToggleColumn kToggleColumns[] = {
    {BandField::Inhibit, TXB("PTT Inhibit"), TXB("PTT inhibit")},
    {BandField::AccessoryTx, TXB("ACC TX"), TXB("ACC TX")},
    {BandField::RcaTxRequest, TXB("RCA TX Req"), TXB("RCA TX request")},
    {BandField::AccessoryTxRequest, TXB("ACC TX Req"), TXB("ACC TX request")},
    {BandField::Tx1, TXB("RCA TX1"), TXB("RCA TX1")},
    {BandField::Tx2, TXB("RCA TX2"), TXB("RCA TX2")},
    {BandField::Tx3, TXB("RCA TX3"), TXB("RCA TX3")},
    {BandField::HardwareAlc, TXB("HWALC"), TXB("Hardware ALC")},
};
#undef TXB

template <typename Band>
bool toggleValue(const Band& b, BandField field)
{
    switch (field) {
    case BandField::Inhibit: return b.inhibit;
    case BandField::AccessoryTx: return b.accTx;
    case BandField::RcaTxRequest: return b.rcaTxReq;
    case BandField::AccessoryTxRequest: return b.accTxReq;
    case BandField::Tx1: return b.tx1;
    case BandField::Tx2: return b.tx2;
    case BandField::Tx3: return b.tx3;
    case BandField::HardwareAlc: return b.hwAlc;
    default: return false;
    }
}

QString windowTitleFor(const QString& profile)
{
    return QStringLiteral("TX Band Settings (Current TX Profile: %1)").arg(profile);
}

} // namespace

TxBandDialog::TxBandDialog(RadioModel* model, QWidget* parent)
    : CanonWindow(windowTitleFor(model ? model->transmitModel().activeProfile() : QString()),
                  parent, Kind::Workspace),
      m_model(model)
{
    theme::setContainer(this, QStringLiteral("canon/txBand"));
    // Size and position persist under TxBandDialogGeometry, the key earlier
    // releases wrote, so a saved rectangle still restores; it resizes from
    // every edge.
    setGeometryKey(QStringLiteral("TxBandDialogGeometry"));
    FramelessResizer::install(this);
    applyCanonSheet(bodyWidget(), [] {
        return QString::fromLatin1(kTxBandStyle) + canonIndicatorRules();
    });

    m_layout = new QVBoxLayout(bodyWidget());
    m_layout->setContentsMargins(14, 12, 14, 14);
    m_layout->setSpacing(10);
    QWidget* header = makeCanonHeader(windowTitle());
    m_headerTitle = header->findChild<QLabel*>(QStringLiteral("canonHeaderTitle"));
    m_layout->addWidget(header);
    m_layout->addStretch();

    if (m_model) {
        m_profile = m_model->transmitModel().activeProfile();
        // The table belongs to one TX profile and one radio session: a new
        // one rebuilds it. A hidden window is rebuilt when it is next opened.
        connect(&m_model->transmitModel(), &TransmitModel::stateChanged, this, [this] {
            if (isVisible() && m_model->transmitModel().activeProfile() != m_profile) {
                rebuildTable();
            }
        });
        // The model relays each status line before applying it, so the
        // refresh runs once the burst has been applied.
        m_refreshTimer.setSingleShot(true);
        m_refreshTimer.setInterval(0);
        connect(&m_refreshTimer, &QTimer::timeout, this, &TxBandDialog::refreshValues);
        const auto refreshSoon = [this] {
            if (isVisible()) {
                m_refreshTimer.start();
            }
        };
        connect(m_model, &RadioModel::statusReceived, this, refreshSoon);
        connect(m_model, &RadioModel::connectionStateChanged, this, refreshSoon);
    }
    buildTable();
}

void TxBandDialog::rebuildTable()
{
    m_profile = m_model->transmitModel().activeProfile();
    setWindowTitle(windowTitleFor(m_profile));
    if (m_headerTitle) {
        m_headerTitle->setText(windowTitle());
    }
    // Focus returns to the same control in the new table; text typed into the
    // old one belonged to the old profile and is dropped.
    QWidget* focused = QApplication::focusWidget();
    const QString focusedName = focused && m_table && m_table->isAncestorOf(focused)
        ? focused->accessibleName() : QString();
    m_rows.clear();
    delete m_table;
    buildTable();
    if (!focusedName.isEmpty()) {
        for (QWidget* w : m_table->findChildren<QWidget*>()) {
            if (w->accessibleName() == focusedName) {
                w->setFocus();
                break;
            }
        }
    }
}

void TxBandDialog::buildTable()
{
    // Parented to the body before anything is added, so each header label is
    // polished by the body's sheet before it is measured: its minimum is the
    // sheet's bold 10 px text, not the default font's.
    m_table = new QWidget(bodyWidget());
    m_table->setObjectName(QStringLiteral("txBandTable"));
    auto* vb = new QVBoxLayout(m_table);
    vb->setContentsMargins(0, 0, 0, 0);
    vb->setSpacing(10);
    m_layout->insertWidget(1, m_table);

    auto* gridContainer = new QWidget(m_table);
    gridContainer->setObjectName(QStringLiteral("txBandGrid"));
    auto* headerGrid = new QGridLayout(gridContainer);
    headerGrid->setContentsMargins(1, 1, 1, 1);
    headerGrid->setSpacing(1);
    QStringList headers = {tr("Band"), tr("RF PWR(%)"), tr("Tune PWR(%)")};
    for (const ToggleColumn& column : kToggleColumns) {
        headers.append(tr(column.header));
    }
    headers.append(tr("RCA off/TUNE"));
    for (int c = 0; c < headers.size(); ++c) {
        auto* lbl = new QLabel(headers[c]);
        lbl->setProperty("txBandCell", QStringLiteral("header"));
        lbl->setAlignment(Qt::AlignCenter);
        headerGrid->addWidget(lbl, 0, c);
        lbl->ensurePolished();
        lbl->setMinimumSize(lbl->sizeHint());
    }
    vb->addWidget(gridContainer);

    if (!m_model) {
        setMinimumSize(QSize(700, 450).expandedTo(minimumSizeHint()));
        return;
    }

    auto* availability = new ControlAvailabilityRegistry(*m_model, m_table);
    connect(m_model, &RadioModel::tuneOutputControlChanged, availability,
            &ControlAvailabilityRegistry::refreshEngaged);
    const auto& bands = m_model->txBandSettings();
    const RadioControlSession session = m_model->controlSession();
    m_sameSession = [this, session] { return m_model->controlSession() == session; };
    QList<int> sortedIds = bands.keys();
    std::sort(sortedIds.begin(), sortedIds.end());

    int row = 1;
    for (int id : sortedIds) {
        const auto& b = bands[id];
        int col = 0;
        Row cells;
        cells.bandId = id;
        cells.bandName = b.bandName;
        cells.rfPowerShown = b.rfPower;
        cells.tunePowerShown = b.tunePower;

        auto* nameLbl = new QLabel(b.bandName);
        nameLbl->setProperty("txBandCell", QStringLiteral("band"));
        headerGrid->addWidget(nameLbl, row, col++);

        auto* rfEdit = new QLineEdit(QString::number(b.rfPower));
        rfEdit->setAccessibleName(tr("RF power percent, band %1").arg(b.bandName));
        rfEdit->setFixedWidth(50);
        rfEdit->setAlignment(Qt::AlignCenter);
        int bandId = id;
        connect(rfEdit, &QLineEdit::editingFinished, this, [this, rfEdit, bandId, session] {
            bool valid = false;
            const int parsed = rfEdit->text().toInt(&valid);
            if (!valid) { rfEdit->setText(QString::number(m_model->txBandSettings().value(bandId).rfPower)); return; }
            const int percent = qBound(0, parsed, 100);
            rfEdit->setText(QString::number(percent));
            m_model->requestTxBand({bandId, BandField::RfPowerPercent, percent}, session, rfEdit,
                [this, rfEdit, bandId](const RadioSetupReply& reply) {
                    if (reply.kind != RadioSetupReply::Kind::Accepted) {
                        rfEdit->setText(QString::number(m_model->txBandSettings().value(bandId).rfPower));
                    }
                });
        });
        headerGrid->addWidget(valueCell(rfEdit), row, col++);
        cells.rfPower = rfEdit;

        auto* tuneEdit = new QLineEdit(QString::number(b.tunePower));
        tuneEdit->setAccessibleName(tr("Tune power percent, band %1").arg(b.bandName));
        tuneEdit->setFixedWidth(50);
        tuneEdit->setAlignment(Qt::AlignCenter);
        connect(tuneEdit, &QLineEdit::editingFinished, this, [this, tuneEdit, bandId, session] {
            bool valid = false;
            const int parsed = tuneEdit->text().toInt(&valid);
            if (!valid) { tuneEdit->setText(QString::number(m_model->txBandSettings().value(bandId).tunePower)); return; }
            const int percent = qBound(0, parsed, 100);
            tuneEdit->setText(QString::number(percent));
            m_model->requestTxBand({bandId, BandField::TunePowerPercent, percent}, session, tuneEdit,
                [this, tuneEdit, bandId](const RadioSetupReply& reply) {
                    if (reply.kind != RadioSetupReply::Kind::Accepted) {
                        tuneEdit->setText(QString::number(m_model->txBandSettings().value(bandId).tunePower));
                    }
                });
        });
        headerGrid->addWidget(valueCell(tuneEdit), row, col++);
        cells.tunePower = tuneEdit;

        for (const ToggleColumn& column : kToggleColumns) {
            const bool on = toggleValue(b, column.field);
            auto* chk = new QCheckBox;
            chk->setAccessibleName(tr("%1, band %2").arg(tr(column.name), b.bandName));
            chk->setChecked(on);
            const BandField field = column.field;
            connect(chk, &QCheckBox::toggled, this, [this, chk, bandId, field, session](bool on) {
                // A change the radio refuses, or that is never sent, shows
                // the radio's value again.
                m_model->requestTxBand({bandId, field, on}, session, chk,
                    [this, chk, bandId, field](const RadioSetupReply& reply) {
                        if (reply.kind == RadioSetupReply::Kind::Accepted) {
                            return;
                        }
                        const QSignalBlocker blocker(chk);
                        chk->setChecked(toggleValue(m_model->txBandSettings().value(bandId), field));
                    });
            });
            headerGrid->addWidget(valueCell(chk), row, col++);
            cells.toggles.append(chk);
            cells.togglesShown.append(on);
        }
        m_rows.append(cells);
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

    auto* explanation = new QLabel(tr("RCA off/TUNE: suppress TX1/TX2/TX3 keying outputs during TUNE "
                                     "on SmartSDR 4.3 or later. ACC TX remains under Inhibit during TUNE."));
    explanation->setProperty("txBandCell", QStringLiteral("note"));
    explanation->setWordWrap(true);
    vb->addWidget(explanation);
    setMinimumSize(QSize(700, 450).expandedTo(minimumSizeHint()));
}

void TxBandDialog::hideEvent(QHideEvent* event)
{
    // Esc and Ctrl+W hide the window; minimizing is spontaneous and keeps it.
    if (!event->spontaneous()) {
        m_rebuildOnShow = true;
    }
    CanonWindow::hideEvent(event);
}

void TxBandDialog::showEvent(QShowEvent* event)
{
    // Opening it again builds a fresh table, as closing with the corner button
    // and reopening does.
    if (m_rebuildOnShow && m_model) {
        m_rebuildOnShow = false;
        rebuildTable();
    }
    CanonWindow::showEvent(event);
}

void TxBandDialog::refreshValues()
{
    if (!isVisible()) {
        return;
    }
    const auto& bands = m_model->txBandSettings();
    // Rows are the radio's bands by id and name, in id order (QMap's order).
    bool sameBands = bands.size() == m_rows.size();
    for (int i = 0; sameBands && i < m_rows.size(); ++i) {
        const auto it = std::next(bands.constBegin(), i);
        sameBands = it.key() == m_rows[i].bandId && it->bandName == m_rows[i].bandName;
    }
    if (!m_sameSession() || !sameBands) {
        rebuildTable();
        return;
    }
    // Only a value the radio changed since it was last shown is written, so a
    // click or entry the radio has not yet echoed is left as the operator set
    // it, and a field with uncommitted typing keeps it.
    for (Row& row : m_rows) {
        const auto& b = bands[row.bandId];
        for (auto [edit, shown, value] : {std::tuple{row.rfPower, &row.rfPowerShown, b.rfPower},
                                          std::tuple{row.tunePower, &row.tunePowerShown, b.tunePower}}) {
            if (value == *shown || !edit || edit->isModified()) {
                continue;
            }
            *shown = value;
            edit->setText(QString::number(value));
        }
        for (int i = 0; i < row.toggles.size(); ++i) {
            const bool value = toggleValue(b, kToggleColumns[i].field);
            if (value == row.togglesShown[i] || !row.toggles[i]) {
                continue;
            }
            row.togglesShown[i] = value;
            const QSignalBlocker blocker(row.toggles[i]);
            row.toggles[i]->setChecked(value);
        }
    }
}

} // namespace AetherSDR
