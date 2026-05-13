#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>

namespace AetherSDR {

// A "slice" in SmartSDR terminology is an independent receive channel.
// Each slice has its own frequency, mode, filter, and audio settings.
class SliceModel : public QObject {
    Q_OBJECT

    Q_PROPERTY(int    sliceId    READ sliceId)
    Q_PROPERTY(double frequency  READ frequency  WRITE setFrequency  NOTIFY frequencyChanged)
    Q_PROPERTY(QString mode      READ mode       WRITE setMode       NOTIFY modeChanged)
    Q_PROPERTY(int filterLow     READ filterLow  NOTIFY filterChanged)
    Q_PROPERTY(int filterHigh    READ filterHigh NOTIFY filterChanged)
    Q_PROPERTY(bool active       READ isActive   NOTIFY activeChanged)
    Q_PROPERTY(bool txSlice      READ isTxSlice  NOTIFY txSliceChanged)

public:
    explicit SliceModel(int id, QObject* parent = nullptr);

    // Getters
    int     sliceId()    const { return m_id; }
    QString panId()      const { return m_panId; }       // e.g. "0x40000000"
    double  frequency()  const { return m_frequency; }   // MHz
    QString mode()       const { return m_mode; }
    QStringList modeList() const { return m_modeList; }
    int     filterLow()  const { return m_filterLow; }   // Hz offset
    int     filterHigh() const { return m_filterHigh; }
    bool    isActive()   const { return m_active; }
    bool    isTxSlice()  const { return m_txSlice; }
    float   rfGain()     const { return m_rfGain; }
    float   audioGain()  const { return m_audioGain; }
    int     audioPan()   const { return m_audioPan; }

    // Getters — RX DSP state
    QString rxAntenna()   const { return m_rxAntenna; }
    QString txAntenna()   const { return m_txAntenna; }
    QStringList rxAntennaList() const { return m_rxAntennaList; }
    QStringList txAntennaList() const { return m_txAntennaList; }
    bool    isLocked()    const { return m_locked; }
    bool    qskOn()       const { return m_qsk; }
    bool    nbOn()        const { return m_nb; }
    bool    nrOn()        const { return m_nr; }
    bool    anfOn()       const { return m_anf; }
    bool    nrlOn()       const { return m_nrl; }
    bool    nrsOn()       const { return m_nrs; }
    bool    rnnOn()       const { return m_rnn; }
    bool    nrfOn()       const { return m_nrf; }
    bool    anflOn()      const { return m_anfl; }
    bool    anftOn()      const { return m_anft; }
    bool    apfOn()       const { return m_apf; }
    int     apfLevel()    const { return m_apfLevel; }
    int     nbLevel()     const { return m_nbLevel; }
    int     nrLevel()     const { return m_nrLevel; }
    int     anfLevel()    const { return m_anfLevel; }
    int     nrlLevel()    const { return m_nrlLevel; }
    int     nrsLevel()    const { return m_nrsLevel; }
    int     nrfLevel()    const { return m_nrfLevel; }
    int     anflLevel()   const { return m_anflLevel; }
    QString agcMode()      const { return m_agcMode; }
    int     agcThreshold() const { return m_agcThreshold; }
    int     agcOffLevel()  const { return m_agcOffLevel; }
    bool    audioMute()   const { return m_audioMute; }
    bool    squelchOn()   const { return m_squelchOn; }
    int     squelchLevel()const { return m_squelchLevel; }
    bool    ritOn()       const { return m_ritOn; }
    int     ritFreq()     const { return m_ritFreq; }
    bool    xitOn()       const { return m_xitOn; }
    int     xitFreq()     const { return m_xitFreq; }
    int     stepHz()      const { return m_stepHz; }
    QVector<int> stepList() const { return m_stepList; }
    int     daxChannel()  const { return m_daxChannel; }
    int     rttyMark()        const { return m_rttyMark; }
    int     rttyMarkDefault() const { return m_rttyMarkDefault; }
    int     rttyShift()   const { return m_rttyShift; }
    int     diglOffset()  const { return m_diglOffset; }
    int     diguOffset()  const { return m_diguOffset; }

    // Record/playback state (radio-managed)
    bool    recordOn()    const { return m_recordOn; }
    bool    playOn()      const { return m_playOn; }
    bool    playEnabled() const { return m_playEnabled; }

    // Getters — FM duplex/repeater
    QString fmToneMode()          const { return m_fmToneMode; }
    QString fmToneValue()         const { return m_fmToneValue; }
    QString repeaterOffsetDir()   const { return m_repeaterOffsetDir; }
    double  fmRepeaterOffsetFreq()const { return m_fmRepeaterOffsetFreq; }
    double  txOffsetFreq()        const { return m_txOffsetFreq; }
    int     fmDeviation()         const { return m_fmDeviation; }

    // Setters (emit signals AND send radio commands)
    void setFrequency(double mhz);           // slice tune autopan=0 — no recenter
    void tuneAndRecenter(double mhz);      // slice tune — recenters pan (band changes)
    void setMode(const QString& mode);
    void setFilterWidth(int low, int high);
    void setAudioGain(float gain);
    void setRfGain(float gain);
    void setAudioPan(int pan);
    void setAudioMute(bool mute);
    void setDiversity(bool on);
    bool diversity() const { return m_diversity; }
    bool isDiversityChild() const { return m_diversityChild; }
    bool isDiversityParent() const { return m_diversityParent; }
    int  diversityIndex() const { return m_diversityIndex; }
    bool escEnabled() const { return m_escEnabled; }
    float escGain() const { return m_escGain; }
    float escPhaseShift() const { return m_escPhaseShift; }
    void setEscEnabled(bool on);
    void setEscGain(float gain);
    void setEscPhaseShift(float deg);
    void setRxAntenna(const QString& ant);
    void setTxAntenna(const QString& ant);
    void setLocked(bool locked);
    void setQsk(bool on);
    void setNb(bool on);
    void setNr(bool on);
    void setAnf(bool on);
    void setNrl(bool on);
    void setNrs(bool on);
    void setRnn(bool on);
    void setNrf(bool on);
    void setAnfl(bool on);
    void setAnft(bool on);
    void setApf(bool on);
    void setApfLevel(int v);
    void setNbLevel(int v);
    void setNrLevel(int v);
    void setAnfLevel(int v);
    void setNrlLevel(int v);
    void setNrsLevel(int v);
    void setNrfLevel(int v);
    void setAnflLevel(int v);
    void setAgcMode(const QString& mode);
    void setAgcThreshold(int value);
    void setAgcOffLevel(int value);
    void setSquelch(bool on, int level);
    void setRit(bool on, int hz);
    void setXit(bool on, int hz);
    void setDaxChannel(int ch);
    void setRttyMark(int hz);
    void setRttyShift(int hz);
    // Called by RadioModel when the radio's rtty_mark_default changes.
    void setRttyMarkDefault(int hz) { m_rttyMarkDefault = hz; }
    void setDiglOffset(int hz);
    void setDiguOffset(int hz);
    void setTxSlice(bool on);
    void setActive(bool on);
    void setRecordOn(bool on);
    void setPlayOn(bool on);

    // Setters — FM duplex/repeater
    void setFmToneMode(const QString& mode);
    void setFmToneValue(const QString& value);
    void setRepeaterOffsetDir(const QString& dir);
    void setFmRepeaterOffsetFreq(double mhz);
    void setTxOffsetFreq(double mhz);
    void setFmDeviation(int hz);

    // Apply a batch of KV pairs from a status message.
    void applyStatus(const QMap<QString, QString>& kvs);

    // Drain pending outgoing commands (called by RadioModel to send them)
    QStringList drainPendingCommands();

signals:
    void frequencyChanged(double mhz);
    void panIdChanged(const QString& panId);
    void modeChanged(const QString& mode);
    void filterChanged(int low, int high);
    void activeChanged(bool active);
    void txSliceChanged(bool tx);
    void audioGainChanged(float gain);
    void audioPanChanged(int pan);
    void rxAntennaChanged(const QString& ant);
    void txAntennaChanged(const QString& ant);
    void rxAntennaListChanged(const QStringList& ants);
    void txAntennaListChanged(const QStringList& ants);
    void lockedChanged(bool locked);
    void qskChanged(bool on);
    void nbChanged(bool on);
    void nrChanged(bool on);
    void anfChanged(bool on);
    void nrlChanged(bool on);
    void nrsChanged(bool on);
    void rnnChanged(bool on);
    void nrfChanged(bool on);
    void anflChanged(bool on);
    void anftChanged(bool on);
    void apfChanged(bool on);
    void apfLevelChanged(int v);
    void nbLevelChanged(int v);
    void nrLevelChanged(int v);
    void anfLevelChanged(int v);
    void nrlLevelChanged(int v);
    void nrsLevelChanged(int v);
    void nrfLevelChanged(int v);
    void anflLevelChanged(int v);
    void agcModeChanged(const QString& mode);
    void agcThresholdChanged(int value);
    void agcOffLevelChanged(int value);
    void audioMuteChanged(bool mute);
    void diversityChanged(bool on);
    void escEnabledChanged(bool on);
    void escGainChanged(float gain);
    void escPhaseShiftChanged(float deg);
    void rfGainChanged(float gain);
    void squelchChanged(bool on, int level);
    void stepChanged(int hz, const QVector<int>& stepList);
    void ritChanged(bool on, int hz);
    void xitChanged(bool on, int hz);
    void daxChannelChanged(int ch);
    void rttyMarkChanged(int hz);
    void rttyShiftChanged(int hz);
    void diglOffsetChanged(int hz);
    void diguOffsetChanged(int hz);

    // FM duplex/repeater signals
    void fmToneModeChanged(const QString& mode);
    void fmToneValueChanged(const QString& value);
    void repeaterOffsetDirChanged(const QString& dir);
    void fmRepeaterOffsetFreqChanged(double mhz);
    void txOffsetFreqChanged(double mhz);
    void fmDeviationChanged(int hz);

    void modeListChanged(const QStringList& modes);
    void recordOnChanged(bool on);
    void playOnChanged(bool on);
    void playEnabledChanged(bool enabled);
    void commandReady(const QString& cmd);  // ready to send to radio

private:
    int     m_id{0};
    QString m_panId;           // panadapter assignment (e.g. "0x40000000")
    double  m_frequency{0.0};
    QString m_mode{"USB"};
    QStringList m_modeList;
    int     m_filterLow{-1500};
    int     m_filterHigh{1500};
    bool    m_active{false};
    bool    m_txSlice{false};
    float   m_rfGain{0.0f};
    float   m_audioGain{50.0f};
    int     m_audioPan{50};

    // Slice control state
    QString m_rxAntenna{"ANT1"};
    QString m_txAntenna{"ANT1"};
    QStringList m_rxAntennaList;
    QStringList m_txAntennaList;
    bool    m_locked{false};
    bool    m_qsk{false};
    bool    m_audioMute{false};
    bool    m_diversity{false};
    bool    m_diversityChild{false};
    bool    m_diversityParent{false};
    int     m_diversityIndex{-1};
    bool    m_escEnabled{false};
    float   m_escGain{1.0f};
    float   m_escPhaseShift{0.0f};
    bool    m_nb{false};
    bool    m_nr{false};
    bool    m_anf{false};
    bool    m_nrl{false};
    bool    m_nrs{false};
    bool    m_rnn{false};
    bool    m_nrf{false};
    bool    m_anfl{false};
    bool    m_anft{false};
    bool    m_apf{false};
    int     m_apfLevel{50};
    int     m_nbLevel{50};
    int     m_nrLevel{50};
    int     m_anfLevel{50};
    int     m_nrlLevel{50};
    int     m_nrsLevel{50};
    int     m_nrfLevel{50};
    int     m_anflLevel{50};
    QString m_agcMode{"med"};
    int     m_agcThreshold{65};
    int     m_agcOffLevel{10};
    bool    m_squelchOn{false};
    int     m_squelchLevel{20};
    int     m_stepHz{100};
    QVector<int> m_stepList;
    bool    m_ritOn{false};
    int     m_ritFreq{0};
    bool    m_xitOn{false};
    int     m_xitFreq{0};
    int     m_daxChannel{0};
    int     m_rttyMark{2125};
    int     m_rttyMarkDefault{2125};
    bool    m_rttyMarkUserOverride{false};
    int     m_rttyShift{170};
    int     m_diglOffset{2210};
    int     m_diguOffset{1500};

    // FM duplex/repeater state
    QString m_fmToneMode{"off"};
    QString m_fmToneValue{"100.0"};
    QString m_repeaterOffsetDir{"simplex"};
    double  m_fmRepeaterOffsetFreq{0.0};
    double  m_txOffsetFreq{0.0};
    int     m_fmDeviation{5000};

    // Record/playback
    bool    m_recordOn{false};
    bool    m_playOn{false};
    bool    m_playEnabled{false};

    void sendCommand(const QString& cmd);

    QStringList m_pendingCommands;
};

} // namespace AetherSDR
