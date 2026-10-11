#pragma once

#include "core/backends/RadioCapabilities.h"
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <memory>

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QPushButton;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class RadioModel;
class SliceModel;
class WfmLockScope;
class WfmMetadataTicker;

// Selected-slice broadcast FM controls. AppletPanel owns the visibility of
// the complete tile and floating window; this widget never hides itself.
class WfmApplet : public QWidget {
    Q_OBJECT
public:
    explicit WfmApplet(QWidget* parent = nullptr);
    ~WfmApplet() override;
    void setRadioModel(RadioModel* model);
    void setSlice(SliceModel* slice);
    bool isAvailable() const { return m_available; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
signals:
    void availabilityChanged(bool available);
private:
    bool ownsWfmSlice() const;
    bool acceptsControl(const QWidget* control, quint64 binding) const;
    void refresh();
    void refreshDiagnostics();
    void refreshHd();
    void appendHdScopeSample();
    bool hasCurrentHdReception() const;
    void appendScopeSample();
    void registerControls();
    void setSettingsExpanded(bool expanded);
    void applyUiPreferences();
    void saveUiPreferences();

    QPointer<RadioModel> m_model;
    QPointer<SliceModel> m_slice;
    quint64 m_modelBinding{0};
    quint64 m_sliceBinding{0};
    quint64 m_audioModeBinding{0};
    QVector<QMetaObject::Connection> m_modelConnections;
    QVector<QMetaObject::Connection> m_sliceConnections;
    std::unique_ptr<ControlAvailabilityRegistry> m_availability;
    RadioCapabilities m_caps;
    bool m_connected{false};
    bool m_available{false};
    QLabel* m_identity{nullptr};
    WfmLockScope* m_scope{nullptr};
    QPushButton* m_audioMode{nullptr};
    QLabel* m_status{nullptr};
    QComboBox* m_hdProgram{nullptr};
    WfmMetadataTicker* m_metadata{nullptr};
    quint64 m_hdSession{0};
    quint64 m_hdEpoch{0};
    quint64 m_hdRevision{0};
    quint64 m_hdSequence{0};
    int m_hdSelectedProgram{-1};
    QComboBox* m_deemphasis{nullptr};
    QComboBox* m_bandwidth{nullptr};
    QPushButton* m_settingsToggle{nullptr};
    QFrame* m_settingsDrawer{nullptr};
    QCheckBox* m_showScope{nullptr};
    QCheckBox* m_showDiagnostics{nullptr};
    QLabel* m_diagnostics{nullptr};
};
} // namespace AetherSDR
