#pragma once

#include "core/backends/DspParameter.h"

#include <QMetaObject>
#include <QPointer>
#include <QWidget>
#include <cstddef>
#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class RadioModel;
class SliceModel;

// Each edit is a request tied to the observation and session it started from.
class DspAdvancedControls final : public QWidget {
    Q_OBJECT
public:
    explicit DspAdvancedControls(QWidget* parent = nullptr);
    void setContext(RadioModel* radio, SliceModel* slice);

signals:
    void contentChanged();

private:
    struct Edit {
        QPointer<RadioModel> radio;
        QPointer<SliceModel> slice;
        quint64 session{0};
        quint64 observation{0};
        int value{0};
    };
    struct Row {
        DspParameter parameter;
        QWidget* container{nullptr};
        QSpinBox* spin{nullptr};
        QComboBox* choices{nullptr};
        QPushButton* apply{nullptr};
        bool available{false};
        bool canApply{false};
        std::optional<Edit> edit;
        std::optional<Edit> pressed;
    };
    void edited(std::size_t index, int value);
    void apply(std::size_t index);
    void refresh(bool discardEdits = false, const QString& notice = {}, bool forceLayout = false);
    bool current(const Edit& edit) const;

    QPointer<RadioModel> m_radio;
    QPointer<SliceModel> m_slice;
    QComboBox* m_algorithm{nullptr};
    QLabel* m_reason{nullptr};
    QLabel* m_notice{nullptr};
    std::vector<Row> m_rows;
    std::vector<QMetaObject::Connection> m_connections;
    ControlAvailabilityRegistry* m_availability{nullptr};
    bool m_refreshing{false};
    quint64 m_binding{0};
};
} // namespace AetherSDR
