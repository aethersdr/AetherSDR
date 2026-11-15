#pragma once

#include <QDialog>
#include <QList>
#include <QString>

namespace AetherSDR {

class ClientDisconnectDialog : public QDialog {
public:
    enum class Mode {
        SliceSlotsFull,
        RemoteClientDisconnect
    };

    struct Client {
        quint32 handle{0};
        QString program;
        QString station;
    };

    explicit ClientDisconnectDialog(const QList<Client>& clients,
                                    int maxSlices,
                                    QWidget* parent = nullptr,
                                    Mode mode = Mode::SliceSlotsFull);

    QList<quint32> selectedHandles() const { return m_selectedHandles; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    QString displayName(const Client& client) const;
    void acceptWithHandles(const QList<quint32>& handles);

    QList<Client> m_clients;
    QList<quint32> m_selectedHandles;
};

} // namespace AetherSDR
