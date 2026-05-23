#pragma once

#include "models/RadioModel.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace MasterSDR {

class MemoryBrowsePanel : public QWidget {
    Q_OBJECT

public:
    explicit MemoryBrowsePanel(QWidget* parent = nullptr);

    void setMemories(const QMap<int, MemoryEntry>& memories);
    void focusClosestToFrequency(double frequencyMhz);

signals:
    void memoryActivated(int memoryIndex);
    void quickAddRequested();

private:
    void populateTable();
    QColor rowBackground(const MemoryEntry& memory, bool highlight) const;
    void scrollToHighlightedRow();

    QMap<int, MemoryEntry> m_memories;
    QLabel* m_emptyLabel{nullptr};
    QTableWidget* m_table{nullptr};
    QPushButton* m_addBtn{nullptr};
    int m_highlightedMemoryIndex{-1};
};

} // namespace MasterSDR
