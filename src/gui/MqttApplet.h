#pragma once

#include <QWidget>
#include <QVector>
#include <QJsonArray>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextEdit;
class QGridLayout;

namespace AetherSDR {

class MqttClient;

struct MqttButtonDef {
    QString label;
    QString topic;
    QString payload;
};

struct MqttTopicDef {
    QString topic;
    bool displayOnPan{false};
};

// Applet for MQTT station device integration (#699).
// Phase 1: subscribe + display. Phase 2: publish buttons + overlay.
class MqttApplet : public QWidget {
    Q_OBJECT

public:
    explicit MqttApplet(QWidget* parent = nullptr);

    void setMqttClient(MqttClient* client);
    QVector<MqttTopicDef> topicConfig() const { return m_topicDefs; }

signals:
    void connectRequested(const QString& host, quint16 port,
                          const QString& user, const QString& pass,
                          const QStringList& topics,
                          bool useTls, const QString& caFile);
    void disconnectRequested();
    void displayValueChanged(const QString& key, const QString& value);
    void displayCleared();
    void antennaAliasRequested(const QString& token, const QString& alias);

private:
    void buildUI();
    void updateStatus(const QString& text, bool ok);
    void onMessageReceived(const QString& topic, const QByteArray& payload);
    void rebuildButtons();
    void saveButtons();
    void loadButtons();
    void editButton(int index);
    void addButton();
    void removeButton(int index);

    // MQTT broker password lives in QKeychain rather than AppSettings
    // (GHSA-mmqp-cm4w-cvpp).  Both calls are async and no-op on builds
    // without HAVE_KEYCHAIN defined.
    void loadPasswordFromKeychain();
    void savePasswordToKeychain(const QString& password);

    MqttClient* m_client{nullptr};
    QLineEdit*   m_hostEdit{nullptr};
    QLineEdit*   m_portEdit{nullptr};
    QLineEdit*   m_userEdit{nullptr};
    QLineEdit*   m_passEdit{nullptr};
    QLineEdit*   m_topicsEdit{nullptr};
    QCheckBox*   m_tlsCheck{nullptr};
    QLineEdit*   m_caFileEdit{nullptr};
    QPushButton* m_enableBtn{nullptr};
    QLabel*      m_statusLabel{nullptr};
    QTextEdit*   m_messageLog{nullptr};

    // Publish buttons
    QGridLayout*          m_buttonGrid{nullptr};
    QVector<MqttButtonDef> m_buttonDefs;
    QVector<QPushButton*>  m_buttons;
    QPushButton*           m_editBtn{nullptr};
    bool                   m_editMode{false};

    // Per-topic display config
    QVector<MqttTopicDef> m_topicDefs;
};

} // namespace AetherSDR
