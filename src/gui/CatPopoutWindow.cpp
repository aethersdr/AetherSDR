#include "CatPopoutWindow.h"
#include "core/AppSettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <QIntValidator>
#include <QFrame>

namespace AetherSDR {

namespace {

const char* kHeaderStyle =
    "QLabel { color: #506070; font-size: 10px; font-weight: bold; }";
const char* kDimLabel =
    "QLabel { color: #8090a0; font-size: 11px; }";
const char* kMonoLabel =
    "QLabel { color: #607080; font-size: 10px; font-family: monospace; }";
const char* kPortEdit =
    "QLineEdit { font-size: 11px; background: #0a0a18; border: 1px solid #1e2e3e;"
    " border-radius: 3px; padding: 0px 3px; color: #c8d8e8; }";
const char* kComboStyle =
    "QComboBox { font-size: 11px; background: #1a2a3a; border: 1px solid #1e2e3e;"
    " border-radius: 3px; padding: 0px 3px; color: #c8d8e8; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: #1a2a3a; color: #c8d8e8; }";

const char* kSep =
    "QFrame { color: #1e2e3e; }";

// Minimum valid port number for CAT (exclude system ports)
constexpr int kMinPort = 1024;
constexpr int kMaxPort = 65535;

QString sliceLetter(int idx)
{
    if (idx < 0 || idx > 25) return "—";
    return QString(QChar('A' + idx));
}

} // namespace

// ── Constructor ──────────────────────────────────────────────────────────────

CatPopoutWindow::CatPopoutWindow(QWidget* parent)
    : PersistentDialog("CAT Configuration", "CatPopoutGeometry", parent)
{
    setMinimumWidth(560);
    buildTable();
}

// ── Public API ───────────────────────────────────────────────────────────────

void CatPopoutWindow::setPorts(CatPort** ports, int count)
{
    m_portCount = qMin(count, kMaxPorts);
    for (int i = 0; i < m_portCount; ++i)
        m_ports[i] = ports[i];
    for (int i = m_portCount; i < kMaxPorts; ++i)
        m_ports[i] = nullptr;
    rebuildRows();
}

void CatPopoutWindow::setMaxSlices(int n)
{
    m_maxSlices = qMax(1, n);
    // Rebuild VFO combos with updated slice count
    for (int row = 0; row < m_rows.size(); ++row) {
        PortRow& r = m_rows[row];
        int savedA = r.vfoACombo->currentIndex();
        int savedB = r.vfoBCombo->currentIndex();

        r.vfoACombo->blockSignals(true);
        r.vfoBCombo->blockSignals(true);

        r.vfoACombo->clear();
        r.vfoBCombo->clear();
        populateVfoCombo(r.vfoACombo, false);
        populateVfoCombo(r.vfoBCombo, true);

        r.vfoACombo->setCurrentIndex(qMin(savedA, r.vfoACombo->count() - 1));
        r.vfoBCombo->setCurrentIndex(qMin(savedB, r.vfoBCombo->count() - 1));

        r.vfoACombo->blockSignals(false);
        r.vfoBCombo->blockSignals(false);
    }
}

void CatPopoutWindow::setMasterEnabled(bool on)
{
    m_masterEnabled = on;
    for (int i = 0; i < m_rows.size(); ++i) {
        bool portRunning = m_ports[i] && m_ports[i]->isRunning();
        updateRowEnabled(i, portRunning);
    }
}

// ── Build the fixed table structure ─────────────────────────────────────────

void CatPopoutWindow::buildTable()
{
    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(4);

    // Column headers
    auto* headerRow = new QHBoxLayout;
    headerRow->setSpacing(0);

    auto addHeader = [&](const QString& text, int fixedW) {
        auto* lbl = new QLabel(text);
        lbl->setStyleSheet(kHeaderStyle);
        lbl->setFixedWidth(fixedW);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        headerRow->addWidget(lbl);
    };

    addHeader("En",      24);
    addHeader("Port",    52);
    addHeader("Dialect", 88);
    addHeader("VFO A",   56);
    addHeader("VFO B",   56);
#ifndef Q_OS_WIN
    addHeader("PTY",    140);
#endif
    addHeader("Clients", 50);
    headerRow->addStretch();
    root->addLayout(headerRow);

    // Separator
    auto* sep = new QFrame;
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(kSep);
    root->addWidget(sep);

    // Scroll area for the rows
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: transparent; }");

    auto* rowContainer = new QWidget;
    rowContainer->setStyleSheet("QWidget { background: transparent; }");
    m_grid = new QGridLayout(rowContainer);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(3);

    scroll->setWidget(rowContainer);
    root->addWidget(scroll);
}

// ── Rebuild rows when ports change ──────────────────────────────────────────

void CatPopoutWindow::populateVfoCombo(QComboBox* combo, bool includeNone)
{
    if (includeNone)
        combo->addItem("—", -1);
    for (int i = 0; i < m_maxSlices; ++i)
        combo->addItem(sliceLetter(i), i);
}

void CatPopoutWindow::rebuildRows()
{
    // Remove existing rows from grid
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        if (item->widget()) item->widget()->setParent(nullptr);
        delete item;
    }
    m_rows.clear();

    auto& settings = AppSettings::instance();

    for (int i = 0; i < m_portCount; ++i) {
        PortRow row;
        const QString prefix = QString("CatPort_%1_").arg(i);

        // Enable checkbox
        row.enableCheck = new QCheckBox;
        row.enableCheck->setFixedWidth(22);
        {
            bool en = settings.value(prefix + "Enabled", "False").toString() == "True";
            QSignalBlocker b(row.enableCheck);
            row.enableCheck->setChecked(en);
        }

        // Port line edit
        row.portEdit = new QLineEdit;
        row.portEdit->setStyleSheet(kPortEdit);
        row.portEdit->setFixedWidth(50);
        row.portEdit->setAlignment(Qt::AlignCenter);
        row.portEdit->setValidator(new QIntValidator(kMinPort, kMaxPort, row.portEdit));
        row.portEdit->setPlaceholderText("port");
        {
            QString portStr = settings.value(prefix + "Port", "").toString();
            QSignalBlocker b(row.portEdit);
            row.portEdit->setText(portStr);
        }

        // Dialect combo
        row.dialectCombo = new QComboBox;
        row.dialectCombo->setStyleSheet(kComboStyle);
        row.dialectCombo->setFixedWidth(86);
        row.dialectCombo->addItem("Rigctld",  static_cast<int>(CatDialect::Rigctld));
        row.dialectCombo->addItem("TS-2000",  static_cast<int>(CatDialect::TS2000));
        row.dialectCombo->addItem("FlexCAT",  static_cast<int>(CatDialect::FlexCAT));
        {
            QString d = settings.value(prefix + "Dialect", "Rigctld").toString();
            int idx = 0;
            if (d == "TS2000") idx = 1;
            else if (d == "FlexCAT") idx = 2;
            QSignalBlocker b(row.dialectCombo);
            row.dialectCombo->setCurrentIndex(idx);
        }

        // VFO A combo
        row.vfoACombo = new QComboBox;
        row.vfoACombo->setStyleSheet(kComboStyle);
        row.vfoACombo->setFixedWidth(54);
        populateVfoCombo(row.vfoACombo, false);
        {
            int vfoA = settings.value(prefix + "VfoA", "0").toInt();
            int idx = row.vfoACombo->findData(vfoA);
            QSignalBlocker b(row.vfoACombo);
            row.vfoACombo->setCurrentIndex(qMax(0, idx));
        }

        // VFO B combo
        row.vfoBCombo = new QComboBox;
        row.vfoBCombo->setStyleSheet(kComboStyle);
        row.vfoBCombo->setFixedWidth(54);
        populateVfoCombo(row.vfoBCombo, true);
        {
            int vfoB = settings.value(prefix + "VfoB", "-1").toInt();
            int idx = row.vfoBCombo->findData(vfoB);
            QSignalBlocker b(row.vfoBCombo);
            row.vfoBCombo->setCurrentIndex(qMax(0, idx));
        }

#ifndef Q_OS_WIN
        // PTY path (read-only label)
        row.ptyLabel = new QLabel("—");
        row.ptyLabel->setStyleSheet(kMonoLabel);
        row.ptyLabel->setFixedWidth(138);
        if (m_ports[i]) {
            QString path = m_ports[i]->ptyPath();
            row.ptyLabel->setText(path.isEmpty() ? "—" : path);
            connect(m_ports[i], &CatPort::ptyPathChanged, this,
                    [this, i](const QString& path) {
                        if (i < m_rows.size())
                            m_rows[i].ptyLabel->setText(path.isEmpty() ? "—" : path);
                    });
        }
#else
        row.ptyLabel = new QLabel;  // hidden placeholder on Windows
#endif

        // Client count
        row.clientLabel = new QLabel("—");
        row.clientLabel->setStyleSheet(kDimLabel);
        row.clientLabel->setFixedWidth(48);
        row.clientLabel->setAlignment(Qt::AlignCenter);
        if (m_ports[i]) {
            auto updateClients = [this, i]() {
                if (i >= m_rows.size()) return;
                int n = m_ports[i]->clientCount();
                m_rows[i].clientLabel->setText(
                    m_ports[i]->isRunning() ? QString::number(n) : "—");
            };
            connect(m_ports[i], &CatPort::clientCountChanged,
                    this, updateClients);
        }

        // Add to grid
        int col = 0;
        m_grid->addWidget(row.enableCheck,  i, col++);
        m_grid->addWidget(row.portEdit,     i, col++);
        m_grid->addWidget(row.dialectCombo, i, col++);
        m_grid->addWidget(row.vfoACombo,    i, col++);
        m_grid->addWidget(row.vfoBCombo,    i, col++);
#ifndef Q_OS_WIN
        m_grid->addWidget(row.ptyLabel,     i, col++);
#endif
        m_grid->addWidget(row.clientLabel,  i, col++);

        // ── Connections ──────────────────────────────────────────────────────

        const int capturedI = i;

        connect(row.enableCheck, &QCheckBox::toggled, this,
                [this, capturedI](bool /*on*/) {
                    applyRowToSettings(capturedI);
                    updateRowEnabled(capturedI, false);
                    emit configChanged();
                });

        connect(row.portEdit, &QLineEdit::editingFinished, this,
                [this, capturedI]() {
                    applyRowToSettings(capturedI);
                    emit configChanged();
                });

        connect(row.dialectCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, capturedI](int) {
                    applyRowToSettings(capturedI);
                    emit configChanged();
                });

        connect(row.vfoACombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, capturedI](int) {
                    applyRowToSettings(capturedI);
                    emit configChanged();
                });

        connect(row.vfoBCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, capturedI](int) {
                    applyRowToSettings(capturedI);
                    emit configChanged();
                });

        m_rows.append(row);
        updateRowEnabled(i, m_ports[i] && m_ports[i]->isRunning());
    }

    // Push rows to top
    m_grid->setRowStretch(m_portCount, 1);
}

// ── Gray out controls while the port is running ──────────────────────────────

void CatPopoutWindow::updateRowEnabled(int row, bool portRunning)
{
    if (row >= m_rows.size()) return;
    PortRow& r = m_rows[row];

    bool hasPort = !r.portEdit->text().isEmpty();
    // Enable checkbox can only be checked if there's a port number
    r.enableCheck->setEnabled(hasPort || !r.enableCheck->isChecked());

    // All config fields are locked while the port is running
    bool locked = portRunning || (m_masterEnabled && r.enableCheck->isChecked() && hasPort);
    r.portEdit->setEnabled(!locked);
    r.dialectCombo->setEnabled(!locked);
    r.vfoACombo->setEnabled(!locked);
    r.vfoBCombo->setEnabled(!locked);
}

// ── Persist a row's settings ────────────────────────────────────────────────

void CatPopoutWindow::applyRowToSettings(int row)
{
    if (row >= m_rows.size()) return;
    const PortRow& r = m_rows[row];

    auto& s = AppSettings::instance();
    const QString prefix = QString("CatPort_%1_").arg(row);

    s.setValue(prefix + "Enabled", r.enableCheck->isChecked() ? "True" : "False");
    s.setValue(prefix + "Port",    r.portEdit->text());

    int dIdx = r.dialectCombo->currentIndex();
    QString dialect = (dIdx == 1) ? "TS2000" : (dIdx == 2) ? "FlexCAT" : "Rigctld";
    s.setValue(prefix + "Dialect", dialect);

    s.setValue(prefix + "VfoA", QString::number(r.vfoACombo->currentData().toInt()));
    s.setValue(prefix + "VfoB", QString::number(r.vfoBCombo->currentData().toInt()));

    s.save();
}

} // namespace AetherSDR
