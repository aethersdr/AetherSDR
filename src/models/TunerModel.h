#pragma once

#include <QObject>
#include <QMap>
#include <QString>

namespace MasterSDR {

class TgxlConnection;

// State model for a 4o3a Tuner Genius XL (TGXL) connected via the FlexRadio.
//
// Status arrives via TCP as "atu <handle> key=val ..." after "sub atu all".
// Commands use the "tgxl" prefix:
//   tgxl set handle=<H> mode=<0|1>       — operate/standby
//   tgxl set handle=<H> bypass=<0|1>     — bypass on/off
//   tgxl autotune handle=<H>             — initiate auto-tune
//
// Direct TGXL connection (port 9010) enables manual Pi network relay control:
//   tune relay=<0|1|2> move=<+1|-1>      — adjust C1/L/C2 one step
class TunerModel : public QObject {
    Q_OBJECT

public:
    explicit TunerModel(QObject* parent = nullptr);

    // Getters
    QString handle()    const { return m_handle; }
    QString modelName() const { return m_model; }
    QString serialNum() const { return m_serialNum; }
    QString tgxlIp()    const { return m_tgxlIp; }
    bool    isOperate() const { return m_operate; }
    bool    isBypass()  const { return m_bypass; }
    bool    isTuning()  const { return m_tuning; }
    int     relayC1()   const { return m_relayC1; }
    int     relayL()    const { return m_relayL; }
    int     relayC2()   const { return m_relayC2; }
    int     antennaA()  const { return m_antennaA; }  // 0-indexed: 0=ANT1, 1=ANT2, 2=ANT3, -1=unknown
    float   fwdPower()  const { return m_fwdPower; }  // forward power in watts (from direct TGXL status)
    float   swr()       const { return m_swr; }       // SWR ratio (from direct TGXL status)
    bool    hasAntennaSwitch() const { return m_oneByThree; }  // true for TGXL 3x1 model (one_by_three=1)
    bool    isPresent() const { return !m_handle.isEmpty() || m_directPresence; }
    bool    hasDirectConnection() const;

    // Apply key=value pairs from a TCP status message.
    void applyStatus(const QMap<QString, QString>& kvs);

    // Set the tuner handle (extracted from the status object name).
    void setHandle(const QString& handle);

    // Direct TGXL connection for manual relay control (#469)
    void setDirectConnection(TgxlConnection* conn);

    // Manual relay adjustment: relay 0=C1, 1=L, 2=C2; direction +1 or -1
    void adjustRelay(int relay, int direction);

    // Command methods — emit commandReady()
    void setOperate(bool on);
    void setBypass(bool on);
    void autoTune();

    // Antenna switch (TGXL 3x1): ant = 1, 2, or 3 (1-indexed for command)
    void setAntennaA(int ant);

signals:
    void stateChanged();               // any property changed
    void tuningChanged(bool tuning);   // tuning started/stopped
    void antennaAChanged(int antA);    // antenna port changed (0-indexed)
    void metersChanged(float fwdPower, float swr);  // fwd power/SWR from direct TGXL
    void presenceChanged(bool present); // tuner detected / lost
    void directConnectionChanged(bool connected);
    void commandReady(const QString& cmd);

private:
    QString m_handle;
    QString m_model;
    QString m_serialNum;
    QString m_tgxlIp;
    bool    m_operate{false};
    bool    m_bypass{false};
    bool    m_tuning{false};
    int     m_relayC1{0};
    int     m_relayL{0};
    int     m_relayC2{0};
    int     m_antennaA{-1};   // 0-indexed antenna port (-1 = unknown)
    float   m_fwdPower{0.0f};  // forward power in watts (from direct TGXL status)
    float   m_swr{1.0f};      // SWR ratio (from direct TGXL status)
    bool    m_oneByThree{false}; // true for TGXL 3x1 model (from one_by_three=1)

    TgxlConnection* m_directConn{nullptr};
    bool            m_directPresence{false};
};

} // namespace MasterSDR
