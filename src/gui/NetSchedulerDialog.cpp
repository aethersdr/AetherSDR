#include "NetSchedulerDialog.h"

#include "core/NetRecurrence.h"
#include "core/NetScheduleStore.h"
#include "core/DigitalVoiceFeature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTimeEdit>
#include <QTimeZone>
#include <QVBoxLayout>

namespace AetherSDR {

namespace {

const char* const kWeekdayCodes[7] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
const char* const kWeekdayShort[7] = {"M", "T", "W", "T", "F", "S", "S"};
const char* const kWeekdayLong[7] = {"Monday",   "Tuesday", "Wednesday", "Thursday",
                                     "Friday",   "Saturday", "Sunday"};

QStringList commonModes()
{
    return filterUnavailableDigitalVoiceModes(
        {"LSB", "USB", "CW", "CWL", "CWU", "AM", "SAM",
         "FM", "NFM", "DFM", "DSTR", "DIGL", "DIGU", "RTTY", "FDV"});
}

// Friendly one-line preview of the next firing, e.g.
// "Next: Tue Jun 23, 8:00 PM CDT" — used in the editor and the table.
QString formatNext(const NetEntry& entry)
{
    const QDateTime occ = NetRecurrence::nextOccurrence(entry, QDateTime::currentDateTimeUtc());
    if (!occ.isValid())
        return QStringLiteral("No upcoming dates");
    const QString when = occ.toString("ddd MMM d, h:mm AP");
    const QString abbr = occ.timeZoneAbbreviation();
    return abbr.isEmpty() ? when : QString("%1 %2").arg(when, abbr);
}

// --- editor -------------------------------------------------------------
//
// Modal add/edit sheet with the friendly recurrence pattern from the UX
// research: a Repeats preset + interval, conditional weekday chips / monthly
// ordinal, a lead-time preset, and a live "Next: ..." confirmation that updates
// as the user types — so casual operators never see a raw RRULE.

struct EditorControls {
    QLineEdit* name{nullptr};
    QComboBox* freqUnit{nullptr};  // Once/Daily/Weekly/Monthly
    QDateEdit* date{nullptr};      // one-time date (Once)
    QLabel* dateLabel{nullptr};
    QSpinBox* interval{nullptr};
    QLabel* intervalUnit{nullptr};
    QFrame* weekdayRow{nullptr};
    QPushButton* weekdayChips[7]{};
    QFrame* monthlyRow{nullptr};
    QComboBox* monthlyOrdinal{nullptr};
    QComboBox* monthlyWeekday{nullptr};
    QTimeEdit* time{nullptr};
    QComboBox* timezone{nullptr};
    QComboBox* lead{nullptr};
    QDoubleSpinBox* freq{nullptr};
    QComboBox* mode{nullptr};
    QSpinBox* filterLow{nullptr};
    QSpinBox* filterHigh{nullptr};
    QLineEdit* notes{nullptr};
    QLabel* nextLabel{nullptr};
};

QString buildRrule(const EditorControls& c)
{
    const int kind = c.freqUnit->currentIndex();  // 0 once, 1 daily, 2 weekly, 3 monthly
    const int interval = c.interval->value();
    const QString intervalPart = interval > 1 ? QString(";INTERVAL=%1").arg(interval) : QString();

    if (kind == 0)
        return QString();  // one-time / "Repeat = Never"

    if (kind == 1)
        return QString("FREQ=DAILY%1").arg(intervalPart);

    if (kind == 2) {
        QStringList codes;
        for (int i = 0; i < 7; ++i) {
            if (c.weekdayChips[i]->isChecked())
                codes << kWeekdayCodes[i];
        }
        if (codes.isEmpty())
            codes << kWeekdayCodes[0];  // default Monday if no chip is picked
        return QString("FREQ=WEEKLY%1;BYDAY=%2").arg(intervalPart, codes.join(','));
    }

    // Monthly: ordinal combo maps 0..4 -> 1..5, index 5 -> -1 (last).
    const int ordIdx = c.monthlyOrdinal->currentIndex();
    const int ordinal = (ordIdx == 5) ? -1 : ordIdx + 1;
    const int wdIdx = c.monthlyWeekday->currentIndex();  // 0..6 = Mon..Sun
    return QString("FREQ=MONTHLY%1;BYDAY=%2%3")
        .arg(intervalPart)
        .arg(ordinal)
        .arg(kWeekdayCodes[wdIdx]);
}

void loadRuleIntoControls(EditorControls& c, const QString& rrule)
{
    const auto rule = NetRecurrence::parseRule(rrule);
    c.interval->setValue(rule.interval > 0 ? rule.interval : 1);

    switch (rule.freq) {
    case NetRecurrence::ParsedRule::Freq::Daily:
        c.freqUnit->setCurrentIndex(1);
        break;
    case NetRecurrence::ParsedRule::Freq::Weekly:
        c.freqUnit->setCurrentIndex(2);
        for (int i = 0; i < 7; ++i)
            c.weekdayChips[i]->setChecked(rule.weekdays.contains(i + 1));
        break;
    case NetRecurrence::ParsedRule::Freq::Monthly: {
        c.freqUnit->setCurrentIndex(3);
        const int ordIdx = (rule.monthlyOrdinal == -1) ? 5 : (rule.monthlyOrdinal - 1);
        c.monthlyOrdinal->setCurrentIndex(qBound(0, ordIdx, 5));
        if (rule.monthlyWeekday >= 1 && rule.monthlyWeekday <= 7)
            c.monthlyWeekday->setCurrentIndex(rule.monthlyWeekday - 1);
        break;
    }
    case NetRecurrence::ParsedRule::Freq::Invalid:
        c.freqUnit->setCurrentIndex(2);
        break;
    }
}

int leadMinutesFromIndex(int idx)
{
    static const int kMinutes[] = {0, 5, 10, 15, 30, 60};
    if (idx < 0 || idx >= int(sizeof(kMinutes) / sizeof(int)))
        return 10;
    return kMinutes[idx];
}

int leadIndexFromMinutes(int minutes)
{
    static const int kMinutes[] = {0, 5, 10, 15, 30, 60};
    for (int i = 0; i < int(sizeof(kMinutes) / sizeof(int)); ++i) {
        if (kMinutes[i] == minutes)
            return i;
    }
    return 2;  // default 10 min
}

bool editNetEntry(QWidget* parent, NetEntry& entry,
                  const NetSchedulerDialog::CaptureFn& capture, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(isNew ? QStringLiteral("Add Net") : QStringLiteral("Edit Net"));
    dlg.setModal(true);
    dlg.setMinimumWidth(440);

    EditorControls c;
    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(8);
    root->addLayout(form);

    c.name = new QLineEdit(&dlg);
    c.name->setPlaceholderText(QStringLiteral("e.g. Tuesday County ARES Net"));
    form->addRow(QStringLiteral("Name"), c.name);

    // Repeats row.
    auto* repeatRow = new QHBoxLayout();
    c.freqUnit = new QComboBox(&dlg);
    c.freqUnit->addItems({QStringLiteral("Once (no repeat)"), QStringLiteral("Daily"),
                          QStringLiteral("Weekly"), QStringLiteral("Monthly")});
    c.freqUnit->setCurrentIndex(2);
    repeatRow->addWidget(c.freqUnit);
    auto* everyLabel = new QLabel(QStringLiteral("every"), &dlg);
    repeatRow->addWidget(everyLabel);
    c.interval = new QSpinBox(&dlg);
    c.interval->setRange(1, 52);
    c.interval->setValue(1);
    repeatRow->addWidget(c.interval);
    c.intervalUnit = new QLabel(QStringLiteral("week(s)"), &dlg);
    repeatRow->addWidget(c.intervalUnit);
    repeatRow->addStretch(1);
    form->addRow(QStringLiteral("Repeats"), repeatRow);

    // One-time date (Once only).
    c.date = new QDateEdit(QDate::currentDate(), &dlg);
    c.date->setCalendarPopup(true);
    c.date->setDisplayFormat(QStringLiteral("ddd MMM d, yyyy"));
    c.dateLabel = new QLabel(QStringLiteral("On date"), &dlg);
    form->addRow(c.dateLabel, c.date);

    // Weekday chips (weekly only).
    c.weekdayRow = new QFrame(&dlg);
    auto* chipRow = new QHBoxLayout(c.weekdayRow);
    chipRow->setContentsMargins(0, 0, 0, 0);
    chipRow->setSpacing(4);
    for (int i = 0; i < 7; ++i) {
        auto* chip = new QPushButton(QString::fromLatin1(kWeekdayShort[i]), c.weekdayRow);
        chip->setCheckable(true);
        chip->setFixedWidth(34);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setAccessibleName(QString::fromLatin1(kWeekdayLong[i]));
        c.weekdayChips[i] = chip;
        chipRow->addWidget(chip);
    }
    chipRow->addStretch(1);
    form->addRow(QStringLiteral("On"), c.weekdayRow);

    // Monthly ordinal/weekday (monthly only).
    c.monthlyRow = new QFrame(&dlg);
    auto* monthRow = new QHBoxLayout(c.monthlyRow);
    monthRow->setContentsMargins(0, 0, 0, 0);
    monthRow->setSpacing(6);
    monthRow->addWidget(new QLabel(QStringLiteral("on the"), c.monthlyRow));
    c.monthlyOrdinal = new QComboBox(c.monthlyRow);
    c.monthlyOrdinal->addItems({QStringLiteral("first"), QStringLiteral("second"),
                                QStringLiteral("third"), QStringLiteral("fourth"),
                                QStringLiteral("fifth"), QStringLiteral("last")});
    monthRow->addWidget(c.monthlyOrdinal);
    c.monthlyWeekday = new QComboBox(c.monthlyRow);
    for (int i = 0; i < 7; ++i)
        c.monthlyWeekday->addItem(QString::fromLatin1(kWeekdayLong[i]));
    c.monthlyWeekday->setCurrentIndex(6);  // Sunday default
    monthRow->addWidget(c.monthlyWeekday);
    monthRow->addStretch(1);
    form->addRow(QString(), c.monthlyRow);

    // Time + timezone.
    auto* timeRow = new QHBoxLayout();
    c.time = new QTimeEdit(&dlg);
    c.time->setDisplayFormat(QStringLiteral("h:mm AP"));
    c.time->setTime(QTime(20, 0));
    timeRow->addWidget(c.time);
    c.timezone = new QComboBox(&dlg);
    c.timezone->setEditable(true);
    c.timezone->setInsertPolicy(QComboBox::NoInsert);
    {
        const QByteArray sys = QTimeZone::systemTimeZoneId();
        c.timezone->addItem(QString::fromUtf8(sys));
        c.timezone->addItem(QStringLiteral("Etc/UTC"));
        const auto ids = QTimeZone::availableTimeZoneIds();
        for (const QByteArray& id : ids) {
            const QString s = QString::fromUtf8(id);
            if (c.timezone->findText(s) < 0)
                c.timezone->addItem(s);
        }
        c.timezone->setCurrentIndex(0);
    }
    timeRow->addWidget(c.timezone, 1);
    form->addRow(QStringLiteral("At"), timeRow);

    // Reminder lead.
    c.lead = new QComboBox(&dlg);
    c.lead->addItems({QStringLiteral("At start time"), QStringLiteral("5 minutes before"),
                      QStringLiteral("10 minutes before"), QStringLiteral("15 minutes before"),
                      QStringLiteral("30 minutes before"), QStringLiteral("1 hour before")});
    c.lead->setCurrentIndex(2);
    form->addRow(QStringLiteral("Remind me"), c.lead);

    // Separator.
    auto* sep = new QFrame(&dlg);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    // Tuning preset.
    auto* tuneForm = new QFormLayout();
    tuneForm->setLabelAlignment(Qt::AlignRight);
    tuneForm->setSpacing(8);
    root->addLayout(tuneForm);

    auto* freqRow = new QHBoxLayout();
    c.freq = new QDoubleSpinBox(&dlg);
    c.freq->setDecimals(6);
    c.freq->setRange(0.0, 10000.0);
    c.freq->setSuffix(QStringLiteral(" MHz"));
    c.freq->setValue(146.520000);
    freqRow->addWidget(c.freq, 1);
    c.mode = new QComboBox(&dlg);
    c.mode->addItems(commonModes());
    c.mode->setCurrentText(QStringLiteral("FM"));
    freqRow->addWidget(c.mode);
    auto* captureBtn = new QPushButton(QStringLiteral("Capture current VFO"), &dlg);
    captureBtn->setCursor(Qt::PointingHandCursor);
    freqRow->addWidget(captureBtn);
    tuneForm->addRow(QStringLiteral("Frequency"), freqRow);

    auto* filterRow = new QHBoxLayout();
    c.filterLow = new QSpinBox(&dlg);
    c.filterLow->setRange(-12000, 12000);
    c.filterLow->setSuffix(QStringLiteral(" Hz"));
    c.filterHigh = new QSpinBox(&dlg);
    c.filterHigh->setRange(-12000, 12000);
    c.filterHigh->setSuffix(QStringLiteral(" Hz"));
    filterRow->addWidget(c.filterLow);
    filterRow->addWidget(new QLabel(QStringLiteral("to"), &dlg));
    filterRow->addWidget(c.filterHigh);
    filterRow->addStretch(1);
    tuneForm->addRow(QStringLiteral("Filter"), filterRow);

    c.notes = new QLineEdit(&dlg);
    c.notes->setPlaceholderText(QStringLiteral("Net control, NetLogger name, etc."));
    tuneForm->addRow(QStringLiteral("Notes"), c.notes);

    // Live next-occurrence preview.
    c.nextLabel = new QLabel(&dlg);
    c.nextLabel->setStyleSheet("color: palette(highlight); font-weight: 600;");
    root->addWidget(c.nextLabel);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    // --- behaviour ---
    auto snapshotEntry = [&c, &entry]() {
        NetEntry e = entry;
        e.name = c.name->text();
        e.rrule = buildRrule(c);
        e.timeOfDay = c.time->time().toString("HH:mm");
        e.timezone = c.timezone->currentText().trimmed();
        if (e.rrule.isEmpty())  // one-time: anchor to the chosen date
            e.startDate = c.date->date().toString("yyyy-MM-dd");
        e.reminderLeadMinutes = leadMinutesFromIndex(c.lead->currentIndex());
        e.notes = c.notes->text();
        e.preset.freq = c.freq->value();
        e.preset.mode = c.mode->currentText();
        e.preset.rxFilterLow = c.filterLow->value();
        e.preset.rxFilterHigh = c.filterHigh->value();
        return e;
    };

    auto refresh = [&c, everyLabel, snapshotEntry]() {
        const int kind = c.freqUnit->currentIndex();  // 0 once,1 daily,2 weekly,3 monthly
        const bool once = (kind == 0);
        c.intervalUnit->setText(kind == 1   ? QStringLiteral("day(s)")
                                : kind == 2 ? QStringLiteral("week(s)")
                                            : QStringLiteral("month(s)"));
        everyLabel->setVisible(!once);
        c.interval->setVisible(!once);
        c.intervalUnit->setVisible(!once);
        c.dateLabel->setVisible(once);
        c.date->setVisible(once);
        c.weekdayRow->setVisible(kind == 2);
        c.monthlyRow->setVisible(kind == 3);
        c.nextLabel->setText(QStringLiteral("Next: ") + formatNext(snapshotEntry()));
    };

    QObject::connect(c.freqUnit, &QComboBox::currentIndexChanged, &dlg, [refresh](int) { refresh(); });
    QObject::connect(c.interval, &QSpinBox::valueChanged, &dlg, [refresh](int) { refresh(); });
    QObject::connect(c.monthlyOrdinal, &QComboBox::currentIndexChanged, &dlg, [refresh](int) { refresh(); });
    QObject::connect(c.monthlyWeekday, &QComboBox::currentIndexChanged, &dlg, [refresh](int) { refresh(); });
    QObject::connect(c.time, &QTimeEdit::timeChanged, &dlg, [refresh](const QTime&) { refresh(); });
    QObject::connect(c.date, &QDateEdit::dateChanged, &dlg, [refresh](const QDate&) { refresh(); });
    QObject::connect(c.timezone, &QComboBox::currentTextChanged, &dlg, [refresh](const QString&) { refresh(); });
    QObject::connect(c.lead, &QComboBox::currentIndexChanged, &dlg, [refresh](int) { refresh(); });
    for (int i = 0; i < 7; ++i)
        QObject::connect(c.weekdayChips[i], &QPushButton::toggled, &dlg, [refresh](bool) { refresh(); });

    QObject::connect(captureBtn, &QPushButton::clicked, &dlg, [&c, &capture, &dlg, refresh]() {
        if (!capture) {
            return;
        }
        const MemoryEntry m = capture();
        if (m.freq <= 0.0) {
            QMessageBox::information(&dlg, QStringLiteral("Capture current VFO"),
                                     QStringLiteral("Open a slice on a connected radio first."));
            return;
        }
        c.freq->setValue(m.freq);
        if (!m.mode.isEmpty())
            c.mode->setCurrentText(m.mode);
        c.filterLow->setValue(m.rxFilterLow);
        c.filterHigh->setValue(m.rxFilterHigh);
        refresh();
    });

    // Seed controls from the entry being edited.
    c.name->setText(entry.name);
    if (entry.rrule.trimmed().isEmpty()) {
        if (isNew) {
            // New nets default to Weekly on today's weekday, not Once.
            c.freqUnit->setCurrentIndex(2);
            const int todayDow = QDate::currentDate().dayOfWeek();
            if (todayDow >= 1 && todayDow <= 7)
                c.weekdayChips[todayDow - 1]->setChecked(true);
        } else {
            c.freqUnit->setCurrentIndex(0);  // saved one-time net
        }
    } else {
        loadRuleIntoControls(c, entry.rrule);
    }
    {
        const QDate seedDate = QDate::fromString(entry.startDate, "yyyy-MM-dd");
        c.date->setDate(seedDate.isValid() ? seedDate : QDate::currentDate());
    }
    {
        const QTime t = QTime::fromString(entry.timeOfDay, "HH:mm");
        c.time->setTime(t.isValid() ? t : QTime(20, 0));
    }
    if (!entry.timezone.isEmpty()) {
        const int idx = c.timezone->findText(entry.timezone);
        if (idx >= 0)
            c.timezone->setCurrentIndex(idx);
        else
            c.timezone->setCurrentText(entry.timezone);
    }
    c.lead->setCurrentIndex(leadIndexFromMinutes(entry.reminderLeadMinutes));
    c.notes->setText(entry.notes);
    if (entry.preset.freq > 0.0)
        c.freq->setValue(entry.preset.freq);
    if (!entry.preset.mode.isEmpty())
        c.mode->setCurrentText(entry.preset.mode);
    c.filterLow->setValue(entry.preset.rxFilterLow);
    c.filterHigh->setValue(entry.preset.rxFilterHigh);

    // On a brand-new net, prefill the preset from the current VFO if available.
    if (isNew && capture) {
        const MemoryEntry m = capture();
        if (m.freq > 0.0) {
            c.freq->setValue(m.freq);
            if (!m.mode.isEmpty())
                c.mode->setCurrentText(m.mode);
            c.filterLow->setValue(m.rxFilterLow);
            c.filterHigh->setValue(m.rxFilterHigh);
        }
    }

    refresh();

    if (dlg.exec() != QDialog::Accepted)
        return false;

    NetEntry result = snapshotEntry();
    // Anchor a recurring rule's INTERVAL phasing to today if unset; a one-time
    // net already carries its chosen date in startDate.
    if (!result.rrule.isEmpty() && result.startDate.isEmpty())
        result.startDate = QDate::currentDate().toString("yyyy-MM-dd");
    if (result.name.trimmed().isEmpty())
        result.name = QStringLiteral("Unnamed Net");
    entry = result;
    return true;
}

} // namespace

NetSchedulerDialog::NetSchedulerDialog(QList<NetEntry> entries, CaptureFn capture, QWidget* parent)
    : PersistentDialog(QStringLiteral("Net Scheduler"),
                       QStringLiteral("NetSchedulerDialogGeometry"), parent)
    , m_entries(std::move(entries))
    , m_capture(std::move(capture))
{
    setMinimumSize(640, 380);

    auto* root = new QVBoxLayout(bodyWidget());
    root->setContentsMargins(9, 9, 9, 9);
    root->setSpacing(9);

    auto* intro = new QLabel(
        QStringLiteral("Schedule recurring nets and get a reminder with one-click tuning."),
        bodyWidget());
    intro->setStyleSheet("color: palette(mid);");
    root->addWidget(intro);

    m_table = new QTableWidget(bodyWidget());
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("On"), QStringLiteral("Name"), QStringLiteral("Repeats"),
         QStringLiteral("Next"), QStringLiteral("Frequency"), QStringLiteral("Mode")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setAccessibleName(QStringLiteral("Scheduled nets"));
    root->addWidget(m_table, 1);

    auto* btnRow = new QHBoxLayout();
    auto* addBtn = new QPushButton(QStringLiteral("Add…"), bodyWidget());
    m_editBtn = new QPushButton(QStringLiteral("Edit…"), bodyWidget());
    m_removeBtn = new QPushButton(QStringLiteral("Remove"), bodyWidget());
    m_toggleBtn = new QPushButton(QStringLiteral("Disable"), bodyWidget());
    m_tuneBtn = new QPushButton(QStringLiteral("Tune Now"), bodyWidget());
    btnRow->addWidget(addBtn);
    btnRow->addWidget(m_editBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addWidget(m_toggleBtn);
    btnRow->addWidget(m_tuneBtn);
    btnRow->addStretch(1);
    auto* importBtn = new QPushButton(QStringLiteral("Import…"), bodyWidget());
    auto* exportBtn = new QPushButton(QStringLiteral("Export…"), bodyWidget());
    btnRow->addWidget(importBtn);
    btnRow->addWidget(exportBtn);
    root->addLayout(btnRow);

    connect(addBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onEdit);
    connect(m_removeBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onRemove);
    connect(m_toggleBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onToggleEnabled);
    connect(m_tuneBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onTuneNow);
    connect(importBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onImport);
    connect(exportBtn, &QPushButton::clicked, this, &NetSchedulerDialog::onExport);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &NetSchedulerDialog::updateButtons);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int) { onEdit(); });

    populateTable();
    updateButtons();
}

int NetSchedulerDialog::selectedRow() const
{
    return m_table->currentRow();
}

void NetSchedulerDialog::populateTable()
{
    m_table->setRowCount(m_entries.size());
    for (int row = 0; row < m_entries.size(); ++row) {
        const NetEntry& e = m_entries.at(row);

        auto* onItem = new QTableWidgetItem(e.enabled ? QStringLiteral("✓") : QString());
        onItem->setTextAlignment(Qt::AlignCenter);
        onItem->setData(Qt::UserRole, e.id);
        m_table->setItem(row, 0, onItem);

        m_table->setItem(row, 1, new QTableWidgetItem(e.name));
        m_table->setItem(row, 2, new QTableWidgetItem(NetRecurrence::describeRule(e.rrule)));
        m_table->setItem(row, 3, new QTableWidgetItem(e.enabled ? formatNext(e)
                                                                : QStringLiteral("—")));
        m_table->setItem(row, 4,
                         new QTableWidgetItem(QString::number(e.preset.freq, 'f', 4)
                                              + QStringLiteral(" MHz")));
        m_table->setItem(row, 5, new QTableWidgetItem(e.preset.mode));

        if (!e.enabled) {
            for (int col = 0; col < m_table->columnCount(); ++col) {
                if (auto* it = m_table->item(row, col))
                    it->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
            }
        }
    }
}

void NetSchedulerDialog::updateButtons()
{
    const int row = selectedRow();
    const bool has = row >= 0 && row < m_entries.size();
    m_editBtn->setEnabled(has);
    m_removeBtn->setEnabled(has);
    m_toggleBtn->setEnabled(has);
    m_tuneBtn->setEnabled(has);
    if (has)
        m_toggleBtn->setText(m_entries.at(row).enabled ? QStringLiteral("Disable")
                                                       : QStringLiteral("Enable"));
}

void NetSchedulerDialog::commit()
{
    populateTable();
    updateButtons();
    Q_EMIT entriesChanged(m_entries);
}

void NetSchedulerDialog::onAdd()
{
    NetEntry e;
    if (!editNetEntry(this, e, m_capture, /*isNew=*/true))
        return;
    e.id = NetScheduleStore::newId();
    m_entries.append(e);
    commit();
    m_table->setCurrentCell(m_entries.size() - 1, 1);
}

void NetSchedulerDialog::onEdit()
{
    const int row = selectedRow();
    if (row < 0 || row >= m_entries.size())
        return;
    NetEntry e = m_entries.at(row);
    if (!editNetEntry(this, e, m_capture, /*isNew=*/false))
        return;
    m_entries[row] = e;
    commit();
    m_table->setCurrentCell(row, 1);
}

void NetSchedulerDialog::onRemove()
{
    const int row = selectedRow();
    if (row < 0 || row >= m_entries.size())
        return;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Remove Net"),
        QStringLiteral("Remove \"%1\" from your schedule?").arg(m_entries.at(row).name));
    if (answer != QMessageBox::Yes)
        return;
    m_entries.removeAt(row);
    commit();
}

void NetSchedulerDialog::onToggleEnabled()
{
    const int row = selectedRow();
    if (row < 0 || row >= m_entries.size())
        return;
    m_entries[row].enabled = !m_entries.at(row).enabled;
    commit();
    m_table->setCurrentCell(row, 1);
}

void NetSchedulerDialog::onTuneNow()
{
    const int row = selectedRow();
    if (row < 0 || row >= m_entries.size())
        return;
    Q_EMIT tuneRequested(m_entries.at(row));
}

void NetSchedulerDialog::onImport()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import Net Schedule"), QString(),
        QStringLiteral("Net schedule (*.json);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import"),
                             QStringLiteral("Could not open the file."));
        return;
    }
    const auto result = NetScheduleStore::parse(file.readAll());
    if (!result.ok()) {
        QMessageBox::warning(this, QStringLiteral("Import"),
                             QStringLiteral("Could not read this file:\n%1")
                                 .arg(result.errors.join('\n')));
        return;
    }
    if (result.nets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Import"),
                                 QStringLiteral("No nets were found in this file."));
        return;
    }

    NetScheduleStore::MergePolicy policy = NetScheduleStore::MergePolicy::Duplicate;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Import Net Schedule"));
    box.setText(QStringLiteral("Importing %1 net(s). How should entries that already "
                               "exist be handled?")
                    .arg(result.nets.size()));
    auto* skipBtn = box.addButton(QStringLiteral("Skip existing"), QMessageBox::AcceptRole);
    auto* overwriteBtn = box.addButton(QStringLiteral("Overwrite"), QMessageBox::AcceptRole);
    auto* dupBtn = box.addButton(QStringLiteral("Import as new"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == skipBtn)
        policy = NetScheduleStore::MergePolicy::Skip;
    else if (box.clickedButton() == overwriteBtn)
        policy = NetScheduleStore::MergePolicy::Overwrite;
    else if (box.clickedButton() == dupBtn)
        policy = NetScheduleStore::MergePolicy::Duplicate;
    else
        return;

    m_entries = NetScheduleStore::merge(m_entries, result.nets, policy);
    commit();
}

void NetSchedulerDialog::onExport()
{
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export Net Schedule"), QStringLiteral("net-schedule.json"),
        QStringLiteral("Net schedule (*.json)"));
    if (path.isEmpty())
        return;

    const QByteArray json = NetScheduleStore::serialize(
        m_entries, QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("Export"),
                             QStringLiteral("Could not write the file."));
        return;
    }
}

} // namespace AetherSDR
