#pragma once

#include "CanonWindow.h"
#include "models/NetEntry.h"

#include <QList>

#include <functional>

class QPushButton;
class QTableWidget;

namespace AetherSDR {

// Personal net-schedule manager. Mirrors MemoryDialog's table + button-row
// layout and its import/export affordances, but the rows are recurring nets
// (recurrence rule + reminder + tuning preset) persisted client-side as JSON,
// not radio memory slots.
//
// The dialog owns a working copy of the entry list and emits entriesChanged()
// whenever it is mutated so the owner (MainWindow) can persist it and refeed the
// scheduler. "Tune Now" emits tuneRequested() for typed slice corrections,
// without recalling a radio memory slot.
//
// A CanonWindow workspace (style guide, RFC #6226), like AetherRX and AetherTX:
// always frameless, so MainWindow creates it directly rather than through
// showOrRaisePersistent().
class NetSchedulerDialog : public CanonWindow {
    Q_OBJECT

public:
    // capture: returns the current VFO state as a tuning preset (freq/mode/
    // filter/tone) so a net can be seeded from "what I'm listening to now".
    // Returns a freq<=0 MemoryEntry when no slice is available.
    using CaptureFn = std::function<MemoryEntry()>;

    explicit NetSchedulerDialog(QList<NetEntry> entries, CaptureFn capture,
                                QWidget* parent = nullptr);

    QList<NetEntry> entries() const { return m_entries; }

Q_SIGNALS:
    void entriesChanged(const QList<AetherSDR::NetEntry>& entries);
    void tuneRequested(const AetherSDR::NetEntry& entry);

private:
    void populateTable();
    void updateButtons();
    int selectedRow() const;
    void commit();

    void onAdd();
    void onEdit();
    void onRemove();
    void onToggleEnabled();
    void onTuneNow();
    void onImport();
    void onExport();

    QList<NetEntry> m_entries;
    CaptureFn m_capture;
    QTableWidget* m_table{nullptr};
    QPushButton* m_editBtn{nullptr};
    QPushButton* m_removeBtn{nullptr};
    QPushButton* m_toggleBtn{nullptr};
    QPushButton* m_tuneBtn{nullptr};
};

} // namespace AetherSDR
