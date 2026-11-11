#pragma once

#include <QMap>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
#include <array>

class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QPushButton;
class QSpinBox;

namespace AetherSDR {
class ControlAvailabilityRegistry;
class IRadioBackend;
class RadioModel;

class NativeScopeSettingsWidget final : public QWidget {
    Q_OBJECT
public:
    explicit NativeScopeSettingsWidget(RadioModel& model, QWidget* parent = nullptr);
    ~NativeScopeSettingsWidget() override;

private:
    struct Control {
        QLabel* observed = nullptr;
        QLabel* unit = nullptr;
        QComboBox* choice = nullptr;
        QSpinBox* number = nullptr;
        QPushButton* apply = nullptr;
        QPushButton* reset = nullptr;
        bool seeded = false;
        bool dirty = false;
        int draft = -1;
        QString context;
        QString revision;
    };
    struct Range {
        QLabel* observed = nullptr;
        QSpinBox* low = nullptr;
        QSpinBox* high = nullptr;
        QPushButton* apply = nullptr;
        QPushButton* reset = nullptr;
        bool seeded = false;
        bool dirty = false;
        QString context;
        QString revision;
    };
    void bindBackend();
    void acceptStatus(const QVariantMap& status);
    void acceptFrame(const QVariantMap& frame);
    void render();
    void renderFrame();
    void refreshAvailability();
    void editControl(const QString& id, int value);
    void editRange(int slot);
    void applyControl(const QString& id, bool reset);
    void applyRange(int slot, bool reset);
    void submit(const QString& verb, const QVariantMap& arguments = {});
    QVariantMap controlStatus(const QString& id) const;
    QVariantMap rangeStatus(int slot) const;
    bool controlAvailable(const QString& id, bool apply) const;
    bool rangeAvailable(int slot, bool reset) const;
    bool actionAvailable(const QString& id) const;
    void parkFrame(const QString& reason);

    RadioModel& m_model;
    ControlAvailabilityRegistry* m_availability;
    QPointer<IRadioBackend> m_backend;
    QString m_namespace;
    QVariantMap m_confirmed;
    QMap<QString, Control> m_controls;
    std::array<Range, 3> m_ranges;
    QMap<QString, QPushButton*> m_actions;
    QMap<quint64, QString> m_requests;
    QMap<quint64, QString> m_requestContexts;
    quint64 m_bindingGeneration = 0;
    quint64 m_receiveSession = 0;
    quint64 m_statusRevision = 0;
    quint64 m_statusSerial = 0;
    quint64 m_getSerial = 0;
    quint64 m_frameSequence = 0;
    quint64 m_frameSerial = 0;
    bool m_haveFrameSequence = false;
    QString m_frameContext;
    QVariantList m_frameValues;
    QString m_frameReason;
    std::array<QMetaObject::Connection, 3> m_connections;
    QPushButton* m_read;
    QLabel* m_context;
    QLabel* m_status;
    QLabel* m_currentBounds;
    QLabel* m_transport;
    QLabel* m_filterStatus;
    QGraphicsView* m_plot;
    QGraphicsScene* m_scene;
};
} // namespace AetherSDR
