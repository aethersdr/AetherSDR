#pragma once

#include <QDialog>
#include <QList>
#include <QString>

namespace AetherSDR {

// Shown when multiFLEX is disabled and another client is already connected.
// Mirrors the SmartSDR "Connected Stations" dialog: lists connected stations
// and lets the user disconnect one before proceeding.
class ConnectedStationsDialog : public QDialog {
public:
    struct Client {
        quint32 handle{0};
        QString program;
        QString station;
    };

    struct RadioMeta {
        QString model;
        QString nickname;
        QString callsign;
    };

    explicit ConnectedStationsDialog(const RadioMeta& radio,
                                     const QList<Client>& clients,
                                     QWidget* parent = nullptr);

    // Returns the handle the user chose to disconnect, or 0 if cancelled.
    quint32 selectedHandle() const { return m_selectedHandle; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    quint32 m_selectedHandle{0};
};

} // namespace AetherSDR
