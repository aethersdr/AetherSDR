#include "CatControlApplet.h"
#include "core/AppSettings.h"
#include "core/ThemeManager.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace AetherSDR {

namespace {

// Stylesheet templates — tokens are resolved at runtime by ThemeManager.
// Use ThemeManager::applyStyleSheet(widget, kXxx) so widgets re-style automatically
// on theme changes.  The :checked colours on kGreenToggle have no matching token
// (there is no dark-success-background token) so they stay hardcoded.

const char* kGreenToggle =
    "QPushButton { background: {{color.background.1}}; border: 1px solid {{color.background.2}}; border-radius: 3px;"
    " color: {{color.text.primary}}; font-size: 11px; font-weight: bold; padding: 2px 8px; }"
    "QPushButton:hover { background: {{color.background.2}}; }"
    "QPushButton:checked { background: #006040; color: {{color.accent.success}}; border: 1px solid {{color.accent.success}}; }";

const char* kHintBtn =
    "QPushButton { background: transparent; border: none; color: {{color.text.label}};"
    " font-size: 10px; font-style: italic; text-align: right; padding: 0; }"
    "QPushButton:hover { color: {{color.text.secondary}}; }";

const char* kHeaderStyle =
    "QLabel { color: {{color.text.label}}; font-size: 10px; font-weight: bold; }";

const char* kDimLabel =
    "QLabel { color: {{color.text.secondary}}; font-size: 11px; }";

const char* kMonoLabel =
    "QLabel { color: {{color.text.label}}; font-size: 10px; font-family: monospace; }";

const char* kPortEdit =
    "QLineEdit { font-size: 11px; background: {{color.background.0}}; border: 1px solid {{color.border.subtle}};"
    " border-radius: 3px; padding: 0px 3px; color: {{color.text.primary}}; }"
    "QLineEdit:read-only { color: {{color.text.label}}; background: {{color.background.0}}; border-color: {{color.border.subtle}}; }";

const char* kComboStyle =
    "QComboBox { font-size: 11px; background: {{color.background.1}}; border: 1px solid {{color.border.subtle}};"
    " border-radius: 3px; padding: 0px 3px; color: {{color.text.primary}}; }"
    "QComboBox:disabled { color: {{color.text.label}}; background: {{color.background.0}}; }"
    "QComboBox::drop-down { border: none; }"
    "QComboBox QAbstractItemView { background: {{color.background.1}}; color: {{color.text.primary}}; }"
    // Editable combos (VFO A/B) — transparent line edit so it inherits the combo look
    "QComboBox QLineEdit { background: transparent; border: none; color: {{color.text.primary}};"
    " font-size: 11px; padding: 0px; }"
    "QComboBox:disabled QLineEdit { color: {{color.text.label}}; }";

const char* kSepStyle =
    "QFrame { color: {{color.border.subtle}}; }";

constexpr int kMinPort = 1024;
constexpr int kMaxPort = 65535;

QString sliceLetter(int idx)
{
    if (idx < 0 || idx > 25) return "—";
    return QString(QChar('A' + idx));
}

} // namespace

// ── Constructor ──────────────────────────────────────────────────────────────

CatControlApplet::CatControlApplet(QWidget* parent) : QWidget(parent)
{
    setStyleSheet("QWidget { background: transparent; }");

    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(0, 0, 0, 0);
    m_rootLayout->setSpacing(0);

    // Docked page: always present, narrow
    m_dockedPage = new QWidget(this);
    buildDockedView(m_dockedPage);
    m_rootLayout->addWidget(m_dockedPage);

    // Floating page is built lazily on first setFloating(true) — not added here,
    // so it cannot influence the docked sizeHint.

    hide();
}

// ── Docked page ──────────────────────────────────────────────────────────────

void CatControlApplet::buildDockedView(QWidget* page)
{
    page->setStyleSheet("QWidget { background: transparent; }");
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    auto* row = new QHBoxLayout;
    row->setSpacing(6);

    m_enableBtn = new QPushButton("Enable CAT");
    m_enableBtn->setCheckable(true);
    ThemeManager::instance().applyStyleSheet(m_enableBtn, kGreenToggle);
    m_enableBtn->setFixedSize(100, 22);
    {
        QSignalBlocker b(m_enableBtn);
        m_enableBtn->setChecked(
            AppSettings::instance().value("CatEnabled", "False").toString() == "True");
    }
    row->addWidget(m_enableBtn);
    row->addStretch();
    root->addLayout(row);

    root->addStretch();

    auto* hint = new QPushButton("Pop out for configuration.");
    ThemeManager::instance().applyStyleSheet(hint, kHintBtn);
    hint->setFlat(true);
    hint->setCursor(Qt::PointingHandCursor);
    // Clicking the hint pops out the applet — but the actual pop-out is driven
    // by the ContainerWidget's float button. The hint is informational only.
    root->addWidget(hint);

    connect(m_enableBtn, &QPushButton::toggled, this, [this](bool on) {
        auto& s = AppSettings::instance();
        s.setValue("CatEnabled", on ? "True" : "False");
        s.save();
        emit enableChanged(on);
    });
}

// ── Public API ───────────────────────────────────────────────────────────────

void CatControlApplet::setCatEnabled(bool on)
{
    if (m_enableBtn) {
        QSignalBlocker b(m_enableBtn);
        m_enableBtn->setChecked(on);
    }
    if (m_floatingEnableBtn) {
        QSignalBlocker b(m_floatingEnableBtn);
        m_floatingEnableBtn->setChecked(on);
    }
    for (int i = 0; i < m_rows.size(); ++i)
        updateRowLocked(i);
}

void CatControlApplet::setFloating(bool on)
{
    if (on) {
        // Build the floating page on first use
        if (!m_floatingPage) {
            m_floatingPage = new QWidget(this);
            m_floatingPage->setStyleSheet("QWidget { background: transparent; }");
            auto* floatingRoot = new QVBoxLayout(m_floatingPage);
            floatingRoot->setContentsMargins(4, 4, 4, 4);
            floatingRoot->setSpacing(4);

            // Enable button + note (must enable before ports can be configured)
            auto* enableRow = new QHBoxLayout;
            enableRow->setSpacing(8);
            m_floatingEnableBtn = new QPushButton("Enable CAT");
            m_floatingEnableBtn->setCheckable(true);
            ThemeManager::instance().applyStyleSheet(m_floatingEnableBtn, kGreenToggle);
            m_floatingEnableBtn->setFixedSize(100, 22);
            {
                QSignalBlocker b(m_floatingEnableBtn);
                m_floatingEnableBtn->setChecked(
                    AppSettings::instance().value("CatEnabled", "False").toString() == "True");
            }
            auto* noteLabel = new QLabel("Enable before configuring ports.");
            ThemeManager::instance().applyStyleSheet(noteLabel,
                "QLabel { color: {{color.text.label}}; font-size: 10px; font-style: italic; }");
            enableRow->addWidget(m_floatingEnableBtn);
            enableRow->addWidget(noteLabel);
            enableRow->addStretch();
            floatingRoot->addLayout(enableRow);

            connect(m_floatingEnableBtn, &QPushButton::toggled, this, [this](bool on) {
                auto& s = AppSettings::instance();
                s.setValue("CatEnabled", on ? "True" : "False");
                s.save();
                if (m_enableBtn) {
                    QSignalBlocker b(m_enableBtn);
                    m_enableBtn->setChecked(on);
                }
                emit enableChanged(on);
            });

            auto* sep = new QFrame;
            sep->setFrameShape(QFrame::HLine);
            ThemeManager::instance().applyStyleSheet(sep, kSepStyle);
            floatingRoot->addWidget(sep);

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
            floatingRoot->addWidget(scroll);

            m_rootLayout->addWidget(m_floatingPage);
        }

        if (!m_rowsBuilt && m_portCount > 0)
            buildTableRows();

        m_dockedPage->hide();
        m_floatingPage->show();
    } else {
        if (m_floatingPage) m_floatingPage->hide();
        m_dockedPage->show();
    }
}

void CatControlApplet::setPorts(CatPort** ports, int count)
{
    m_portCount = qMin(count, kMaxPorts);
    for (int i = 0; i < m_portCount; ++i)
        m_ports[i] = ports[i];
    for (int i = m_portCount; i < kMaxPorts; ++i)
        m_ports[i] = nullptr;
    m_rowsBuilt = false;
    // If already floating, rebuild immediately
    if (m_floatingPage && m_floatingPage->isVisible())
        buildTableRows();
}

void CatControlApplet::setMaxSlices(int n)
{
    m_maxSlices = qMax(1, n);
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

// ── Build floating table rows ────────────────────────────────────────────────

void CatControlApplet::populateVfoCombo(QComboBox* combo, bool includeNone)
{
    if (includeNone) combo->addItem("—", -1);
    for (int i = 0; i < m_maxSlices; ++i)
        combo->addItem(sliceLetter(i), i);
}

void CatControlApplet::buildTableRows()
{
    // Clear any existing rows
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        if (item->widget()) item->widget()->setParent(nullptr);
        delete item;
    }
    m_rows.clear();

    // Header labels as grid row 0 — same layout as data rows, so columns always align.
    // w=0 means no fixed width (used for the stretching PTY column).
    auto addHdr = [&](const QString& text, int w, int col,
                      Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter) {
        auto* lbl = new QLabel(text);
        ThemeManager::instance().applyStyleSheet(lbl, kHeaderStyle);
        if (w > 0) lbl->setFixedWidth(w);
        lbl->setAlignment(align);
        m_grid->addWidget(lbl, 0, col);
    };
    int hcol = 0;
    addHdr("En",       22, hcol++);
    addHdr("Port",     50, hcol++, Qt::AlignHCenter | Qt::AlignVCenter);
    addHdr("Dialect",  68, hcol++);
    addHdr("VFO A",    42, hcol++, Qt::AlignHCenter | Qt::AlignVCenter);
    addHdr("VFO B",    42, hcol++, Qt::AlignHCenter | Qt::AlignVCenter);
#ifndef Q_OS_WIN
    addHdr("PTY",       0, hcol++);  // no fixed width — column stretches
#endif
    addHdr("Clients",  48, hcol++);

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

        // Port edit
        row.portEdit = new QLineEdit;
        ThemeManager::instance().applyStyleSheet(row.portEdit, kPortEdit);
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
        ThemeManager::instance().applyStyleSheet(row.dialectCombo, kComboStyle);
        row.dialectCombo->setFixedWidth(68);
        row.dialectCombo->addItem("Rigctld",  static_cast<int>(CatDialect::Rigctld));
        row.dialectCombo->addItem("TS-2000",  static_cast<int>(CatDialect::TS2000));
        row.dialectCombo->addItem("Flex",     static_cast<int>(CatDialect::FlexCAT));
        {
            QString d = settings.value(prefix + "Dialect", "Rigctld").toString();
            int idx = (d == "TS2000") ? 1 : (d == "FlexCAT") ? 2 : 0;
            QSignalBlocker b(row.dialectCombo);
            row.dialectCombo->setCurrentIndex(idx);
        }

        // VFO A combo — editable+read-only so we can center the selected-item text
        row.vfoACombo = new QComboBox;
        ThemeManager::instance().applyStyleSheet(row.vfoACombo, kComboStyle);
        row.vfoACombo->setFixedWidth(42);
        populateVfoCombo(row.vfoACombo, false);
        {
            int vfoA = settings.value(prefix + "VfoA", "0").toInt();
            int idx = row.vfoACombo->findData(vfoA);
            QSignalBlocker b(row.vfoACombo);
            row.vfoACombo->setCurrentIndex(qMax(0, idx));
        }
        row.vfoACombo->setEditable(true);
        row.vfoACombo->lineEdit()->setAlignment(Qt::AlignCenter);
        row.vfoACombo->lineEdit()->setReadOnly(true);
        row.vfoACombo->lineEdit()->setFocusPolicy(Qt::NoFocus);

        // VFO B combo
        row.vfoBCombo = new QComboBox;
        ThemeManager::instance().applyStyleSheet(row.vfoBCombo, kComboStyle);
        row.vfoBCombo->setFixedWidth(42);
        populateVfoCombo(row.vfoBCombo, true);
        {
            int vfoB = settings.value(prefix + "VfoB", "-1").toInt();
            int idx = row.vfoBCombo->findData(vfoB);
            QSignalBlocker b(row.vfoBCombo);
            row.vfoBCombo->setCurrentIndex(qMax(0, idx));
        }
        row.vfoBCombo->setEditable(true);
        row.vfoBCombo->lineEdit()->setAlignment(Qt::AlignCenter);
        row.vfoBCombo->lineEdit()->setReadOnly(true);
        row.vfoBCombo->lineEdit()->setFocusPolicy(Qt::NoFocus);

        // PTY label — sizes to the actual path so the column is always wide enough
        row.ptyLabel = new QLabel("—");
#ifndef Q_OS_WIN
        ThemeManager::instance().applyStyleSheet(row.ptyLabel, kMonoLabel);
        // No fixed or minimum width — sizeHint is driven by the path text,
        // so the grid column sizes itself dynamically to the actual path length.
        if (m_ports[i]) {
            QString path = m_ports[i]->ptyPath();
            row.ptyLabel->setText(path.isEmpty() ? "—" : path);
            connect(m_ports[i], &CatPort::ptyPathChanged, this,
                    [this, i](const QString& path) {
                        if (i < m_rows.size())
                            m_rows[i].ptyLabel->setText(path.isEmpty() ? "—" : path);
                    });
        }
        row.ptyLabel->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(row.ptyLabel, &QLabel::customContextMenuRequested, this,
                [this, i](const QPoint& pos) {
                    if (i >= m_rows.size()) return;
                    const QString path = m_rows[i].ptyLabel->text();
                    if (path.isEmpty() || path == "—") return;
                    QMenu menu;
                    QAction* act = menu.addAction("Copy PTY Name");
                    if (menu.exec(m_rows[i].ptyLabel->mapToGlobal(pos)) == act)
                        QGuiApplication::clipboard()->setText(path);
                });
#else
        row.ptyLabel->hide();
#endif

        // Clients label
        row.clientLabel = new QLabel("—");
        ThemeManager::instance().applyStyleSheet(row.clientLabel, kDimLabel);
        row.clientLabel->setFixedWidth(48);
        row.clientLabel->setAlignment(Qt::AlignCenter);
        if (m_ports[i]) {
            auto updateClients = [this, i]() {
                if (i >= m_rows.size()) return;
                int n = m_ports[i]->clientCount();
                m_rows[i].clientLabel->setText(
                    m_ports[i]->isRunning() ? QString::number(n) : "—");
            };
            updateClients();  // initialize — signal alone won't fire until first connect/disconnect
            connect(m_ports[i], &CatPort::clientCountChanged, this, updateClients);
        }

        // Grid layout — data rows start at 1 (row 0 is the header)
        int col = 0;
        m_grid->addWidget(row.enableCheck,  i+1, col++);
        m_grid->addWidget(row.portEdit,     i+1, col++);
        m_grid->addWidget(row.dialectCombo, i+1, col++);
        m_grid->addWidget(row.vfoACombo,    i+1, col++);
        m_grid->addWidget(row.vfoBCombo,    i+1, col++);
#ifndef Q_OS_WIN
        m_grid->addWidget(row.ptyLabel,     i+1, col++);
#endif
        m_grid->addWidget(row.clientLabel,  i+1, col++);

        // Connections
        const int capturedI = i;

        connect(row.enableCheck, &QCheckBox::toggled, this,
                [this, capturedI](bool) {
                    applyRowToSettings(capturedI);
                    updateRowLocked(capturedI);
                    emit configChanged();
                });

        connect(row.portEdit, &QLineEdit::editingFinished, this,
                [this, capturedI]() {
                    applyRowToSettings(capturedI);
                    updateRowLocked(capturedI);
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
        updateRowLocked(i);
    }

#ifndef Q_OS_WIN
    m_grid->setColumnStretch(5, 1);  // PTY column (index 5) expands to fill available width
#endif
    m_grid->setRowStretch(m_portCount + 1, 1);
    m_rowsBuilt = true;
}

// ── Lock controls while port is running ──────────────────────────────────────

void CatControlApplet::updateRowLocked(int row)
{
    if (row >= m_rows.size()) return;
    PortRow& r = m_rows[row];

    bool hasPort  = !r.portEdit->text().isEmpty();
    bool masterOn = AppSettings::instance().value("CatEnabled", "False").toString() == "True";
    // Lock on UI state only — isRunning() lags behind the checkbox by one applyCatPortCount cycle.
    bool locked   = masterOn && r.enableCheck->isChecked() && hasPort;

    r.enableCheck->setEnabled(hasPort || !r.enableCheck->isChecked());
    r.portEdit->setReadOnly(locked);
    r.dialectCombo->setEnabled(!locked);
    r.vfoACombo->setEnabled(!locked);
    r.vfoBCombo->setEnabled(!locked);
}

// ── Persist row settings ─────────────────────────────────────────────────────

void CatControlApplet::applyRowToSettings(int row)
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
