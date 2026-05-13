#pragma once

#include "core/RadioConnection.h"
#include "core/WanConnection.h"
#include "core/PanadapterStream.h"
#include "core/SleepInhibitor.h"
#include "core/DaxTxPolicy.h"
#include <QThread>
#include "SliceModel.h"
#include "MeterModel.h"
#include "PanadapterModel.h"
#include "TunerModel.h"
#include "TransmitModel.h"
#include "EqualizerModel.h"
#include "TnfModel.h"
#include "SpotModel.h"
#include "CwxModel.h"
#include "DvkModel.h"
#include "UsbCableModel.h"
#include "DaxIqModel.h"
#include "NavtexModel.h"
#include "MemoryEntry.h"
#include "RadioStatusOwnership.h"

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <functional>

#include <QTimer>
#include <QElapsedTimer>

namespace AetherSDR {

// RadioModel is the central data model for a connected radio.
// It owns the RadioConnection, processes incoming status messages,
// and exposes the radio's current state to the GUI via Qt properties/signals.
class RadioModel : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString name        READ name        NOTIFY infoChanged)
    Q_PROPERTY(QString model       READ model       NOTIFY infoChanged)
    Q_PROPERTY(QString version     READ version     NOTIFY infoChanged)
    Q_PROPERTY(bool    connected   READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(float   paTemp      READ paTemp      NOTIFY metersChanged)
    Q_PROPERTY(float   txPower     READ txPower     NOTIFY metersChanged)

public:
    explicit RadioModel(QObject* parent = nullptr);
    ~RadioModel() override;

    // Access the underlying connection and panadapter stream
    RadioConnection*  connection()  { return m_connection; }
    PanadapterStream* panStream()   { return m_panStream; }
    // Sub-models owned by RadioModel (main thread). (#502)
    MeterModel&       meterModel()       { return m_meterModel; }
    TunerModel&       tunerModel()       { return m_tunerModel; }
    TransmitModel&    transmitModel()    { return m_transmitModel; }
    EqualizerModel&   equalizerModel()   { return m_equalizerModel; }
    TnfModel&         tnfModel()         { return m_tnfModel; }
    SpotModel&        spotModel()        { return m_spotModel; }
    CwxModel&         cwxModel()         { return m_cwxModel; }
    DvkModel&         dvkModel()         { return m_dvkModel; }
    NavtexModel&      navtexModel()      { return m_navtexModel; }
    UsbCableModel&    usbCableModel()    { return m_usbCableModel; }
    DaxIqModel&       daxIqModel()       { return m_daxIqModel; }
    bool              hasAmplifier() const { return m_hasAmplifier; }
    bool              ampOperate()   const { return m_ampOperate; }
    QString           ampHandle()    const { return m_ampHandle; }
    QString           ampIp()        const { return m_ampIp; }
    QString           ampModel()     const { return m_ampModel; }

    // Getters
    QString name()    const { return m_name; }
    QString model()   const { return m_model; }
    QString version() const { return m_version; }
    bool isConnected() const;
    bool fullDuplexEnabled() const { return m_fullDuplex; }
    void setFullDuplex(bool on) { m_fullDuplex = on; emit infoChanged(); }
    void setAmpOperate(bool on);
    float paTemp()    const { return m_paTemp; }
    float txPower()   const { return m_txPower; }
    bool  isRadioTransmitting() const { return m_radioTransmitting; }
    QStringList antennaList() const { return m_antList; }
    QString antennaAlias(const QString& token) const;
    QString antennaDisplayName(const QString& token,
                               bool includeTokenForDisambiguation = false) const;
    QString antennaShortDisplayName(const QString& token, int maxChars = 6) const;
    QMap<QString, QString> antennaAliases() const;
    bool antennaAliasNeedsDisambiguation(const QString& token,
                                         const QStringList& tokens) const;
    void setAntennaAlias(const QString& token, const QString& alias);
    void clearAntennaAlias(const QString& token);
    QStringList knownAntennaTokens() const;
    QString serial()       const;
    QString chassisSerial() const { return m_chassisSerial; }
    QString callsign()     const { return m_callsign; }
    QString nickname()     const { return m_nickname; }
    QString region()       const { return m_region; }
    int     rttyMarkDefault() const { return m_rttyMarkDefault; }
    QString radioOptions() const { return m_radioOptions; }
    RadioInfo lastRadioInfo() const { return m_lastInfo; }

    // License info (populated from "sub license all" responses)
    QString licenseRadioId()        const { return m_licenseRadioId; }
    QString licenseExpirationDate() const { return m_licenseExpirationDate; }
    QString licenseMaxVersion()     const { return m_licenseMaxVersion; }
    QString licenseSubscription()   const { return m_licenseSubscription; }

    QString ip()          const { return m_ip; }
    QString netmask()     const { return m_netmask; }
    QString gateway()     const { return m_gateway; }
    QString mac()         const { return m_mac; }
    bool    enforcePrivateIp() const { return m_enforcePrivateIp; }

    // GPS data
    QString gpsStatus()    const { return m_gpsStatus; }
    int     gpsTracked()   const { return m_gpsTracked; }
    int     gpsVisible()   const { return m_gpsVisible; }
    QString gpsGrid()      const { return m_gpsGrid; }
    QString gpsAltitude()  const { return m_gpsAltitude; }
    QString gpsLat()       const { return m_gpsLat; }
    QString gpsLon()       const { return m_gpsLon; }
    QString gpsTime()      const { return m_gpsTime; }
    QString gpsSpeed()     const { return m_gpsSpeed; }
    QString gpsFreqError() const { return m_gpsFreqError; }

    // Max slices reported by radio
    int maxSlices() const { return m_maxSlices; }
    static int maxSlicesForModel(const QString& model);

    // Returns true for BigBend/DragonFire-platform radios (8400, 8600,
    // AU-series, ML-series, CL-series, RT-series) that support the extended
    // firmware DSP filters (NRL, NRS, RNN, NRF).  6000-series radios don't
    // expose these filters and the UI hides them when this returns false. (#2177)
    bool hasExtendedDspFilters() const {
        return m_model.contains("8400") || m_model.contains("8600")
            || m_model.contains("AU-")  || m_model.contains("ML-")
            || m_model.contains("CL-")  || m_model.contains("RT-");
    }

    // Max panadapters supported by this radio model.
    // FLEX-6700: 8 (dual SCU, high-capacity)
    // FLEX-6600 / FLEX-6500 / FLEX-8600 / AU-520: 4 (dual SCU)
    // All single-SCU models (6300, 6400, etc.): 2
    int maxPanadapters() const {
        if (m_model.contains("6700"))
            return 8;
        if (m_model.contains("6600") || m_model.contains("6500")
                || m_model.contains("8600") || m_model.contains("AU-520"))
            return 4;
        return 2;
    }

    // Panadapter bandwidth limits by radio model (MHz).
    // Values from SmartSDR TCP packet captures (#1385).
    // Dual-SCU: 6700, 8600. Single-SCU: 6300, 6400, 6500, 6600, 8400.
    double maxPanBandwidthMhz() const {
        if (m_model.contains("6700") || m_model.contains("8600"))
            return 14.745601;
        if (m_model.contains("6500"))
            return 14.745601;
        // 6300, 6400, 6600, 8400, Aurora: single SCU
        return 5.4;
    }
    double minPanBandwidthMhz() const {
        if (m_model.contains("6700") || m_model.contains("8600"))
            return 0.001230;
        if (m_model.contains("6500"))
            return 0.004920;
        // 6300, 6400, 6600, 8400, Aurora
        return 0.004920;
    }

    // Oscillator / RX settings
    QString oscState()     const { return m_oscState; }
    QString oscSetting()   const { return m_oscSetting; }
    bool    oscLocked()    const { return m_oscLocked; }
    bool    extPresent()   const { return m_extPresent; }
    bool    gpsdoPresent() const { return m_gpsdoPresent; }

    // Returns true for FLEX-8000 class (8400, 8600) and Aurora, which have GPS hardware.
    bool hasGpsHardware() const {
        return m_model.contains("8400") || m_model.contains("8600")
               || m_model.startsWith("AU-");
    }
    bool    tcxoPresent()  const { return m_tcxoPresent; }
    bool    binauralRx()   const { return m_binauralRx; }
    bool    muteLocalWhenRemote() const { return m_muteLocalWhenRemote; }
    bool    autoSave() const { return m_autoSave; }
    int     freqErrorPpb() const { return m_freqErrorPpb; }
    double  calFreqMhz() const { return m_calFreqMhz; }

    // Audio output
    int     lineoutGain()    const { return m_lineoutGain; }
    bool    lineoutMute()    const { return m_lineoutMute; }
    int     headphoneGain()  const { return m_headphoneGain; }
    bool    headphoneMute()  const { return m_headphoneMute; }
    bool    frontSpeakerMute() const { return m_frontSpeakerMute; }
    void setLineoutGain(int v);
    void setLineoutMute(bool m);
    void setHeadphoneGain(int v);
    void setHeadphoneMute(bool m);
    void setFrontSpeakerMute(bool m);
    QHostAddress radioAddress() const { return m_lastInfo.address; }

    int     filterSharpnessVoice()     const { return m_filterVoice; }
    bool    filterSharpnessVoiceAuto() const { return m_filterVoiceAuto; }
    int     filterSharpnessCw()        const { return m_filterCw; }
    bool    filterSharpnessCwAuto()    const { return m_filterCwAuto; }
    int     filterSharpnessDigital()   const { return m_filterDigital; }
    bool    filterSharpnessDigitalAuto() const { return m_filterDigitalAuto; }

    // Global profiles
    QStringList globalProfiles() const { return m_globalProfiles; }
    QString activeGlobalProfile() const { return m_activeGlobalProfile; }
    void loadGlobalProfile(const QString& name);
    void resetPanState();
    void createAudioStream();
    bool ensureDaxTxStream(DaxTxRequestReason reason);
    QJsonObject troubleshootingSnapshot() const;

    // Memory channel cache
    const QMap<int, MemoryEntry>& memories() const { return m_memories; }
    void handleMemoryStatus(int index, const QMap<QString, QString>& kvs);
    bool    lowLatencyDigital()        const { return m_lowLatencyDigital; }
    bool    hasStaticIp()     const { return m_hasStaticIp; }
    QString staticIp()        const { return m_staticIp; }
    QString staticNetmask()   const { return m_staticNetmask; }
    QString staticGateway()   const { return m_staticGateway; }
    bool    remoteOnEnabled() const { return m_remoteOnEnabled; }
    bool    multiFlexEnabled() const { return m_multiFlexEnabled; }
    const QMap<quint32, QString>& clientStations() const { return m_clientStations; }
    quint32 txClientHandle() const { return m_txClientHandle; }
    quint32 ourClientHandle() const;

    struct ClientInfo {
        QString station;
        QString program;
        QString source;
        bool localPtt{false};
        QString txAntenna;
        double txFreqMhz{0};
    };
    const QMap<quint32, ClientInfo>& clientInfoMap() const { return m_clientInfoMap; }
    void    setKnownGuiClients(const QStringList& handles,
                               const QStringList& programs,
                               const QStringList& stations,
                               const QStringList& ips = {},
                               const QStringList& hosts = {});
    void    mergeKnownGuiClients(const QStringList& handles,
                                 const QStringList& programs,
                                 const QStringList& stations,
                                 const QStringList& ips = {},
                                 const QStringList& hosts = {});
    void    setRemoteOnEnabled(bool on);
    void    setMultiFlexEnabled(bool on);
    // Panadapter access (delegates to active pan)
    double panCenterMhz() const;
    double panBandwidthMhz() const;
    QString panId() const { return m_activePanId; }
    void setActivePanId(const QString& id) { m_activePanId = id; }
    PanadapterModel* activePanadapter() const;
    PanadapterModel* panadapter(const QString& panId) const;
    QList<PanadapterModel*> panadapters() const { return m_panadapters.values(); }

    QList<SliceModel*> slices() const { return m_slices; }
    SliceModel* slice(int id) const;
    int activeTxSliceNum() const;

    // High-level actions
    void connectToRadio(const RadioInfo& info);
    void connectViaWan(WanConnection* wan, const QString& publicIp, quint16 udpPort);
    void setPendingClientDisconnects(const QList<quint32>& handles);
    // Called by MainWindow in response to multiFlexConflictDetected().
    // Disconnects handle and resumes the connection sequence.
    void resolveMultiFlexConflict(quint32 handle);
    // Called by MainWindow when the user cancels the conflict dialog.
    void cancelMultiFlexConflict();
    void disconnectFromRadio();
    void forceDisconnect();  // Close TCP but allow auto-reconnect
    bool isWan() const { return m_wanConn != nullptr; }
    void setTransmit(bool tx, TransmitModel::PttSource source = TransmitModel::PttSource::Mox);
    void setDigitalVoiceTxSlice(int sliceId);
    QString audioCompressionParam() const;        // "none" or "opus" based on settings
    void sendCwKey(bool down, const QString& debugSource = {},
                   quint64 debugTraceId = 0, quint64 debugSourceMs = 0); // straight key via netcw stream
    void sendCwPaddle(bool dit, bool dah, const QString& debugSource = {},
                      quint64 debugTraceId = 0, quint64 debugSourceMs = 0); // iambic paddle via netcw stream
    // Lower-level pieces used by the local iambic keyer: PTT and key
    // edges are managed separately so PTT stays asserted across the whole
    // squeeze while key transitions on each element boundary.
    void sendCwPtt(bool on, const QString& debugSource = {},
                   quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    void sendCwKeyEdge(bool down, const QString& debugSource = {},
                       quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    void cwAutoTune(int sliceId, bool intermittent); // int=1 start loop, int=0 stop
    void cwAutoTuneOnce(int sliceId);                // one-shot (no int= param)
    void addSlice();           // Create a new slice on the active panadapter
    void addSliceOnPan(const QString& panId); // Create a new slice on a specific pan
    void createPanadapter();   // Create a new independent panadapter
    void removePanadapter(const QString& panId);
    void setPanBandwidth(double bandwidthMhz);
    void setPanCenter(double centerMhz);
    void setPanDbmRange(float minDbm, float maxDbm);
    void setPanWnb(bool on);
    void setPanWnbLevel(int level);
    void setPanRfGain(int gain);

    // Display controls — FFT (display pan set)
    void setPanAverage(int frames);
    void setPanFps(int fps);
    void setPanWeightedAverage(bool on);

    // Display controls — Waterfall (display panafall set)
    void setWaterfallColorGain(int gain);
    void setWaterfallBlackLevel(int level);
    void setWaterfallAutoBlack(bool on);
    void setWaterfallLineDuration(int ms);

    // Display controls — Noise floor
    void setPanNoiseFloorPosition(int pos);
    void setPanNoiseFloorEnable(bool on);

signals:
    void infoChanged();
    void connectionStateChanged(bool connected);
    // Emitted whenever the local CW key transitions on/off — funnel for
    // serial CTS/DSR, MIDI Gate, TCI key, CWX, and HID encoder sources.
    // Wired to AudioEngine's CwSidetoneGenerator for low-latency local
    // sidetone independent of the radio's own DAX-fed sidetone.
    void cwKeyDownChanged(bool down);
    void sliceAdded(SliceModel* slice);
    void sliceRemoved(int sliceId);
    void metersChanged();
    void connectionError(const QString& msg);
    // Emitted when another GUI client forces this client to disconnect.
    void forcedDisconnectRequested();
    // Emitted when a panadapter's center frequency or bandwidth changes.
    void panadapterInfoChanged(double centerMhz, double bandwidthMhz);
    // Emitted when the radio reports the panadapter's dBm display range.
    void panadapterLevelChanged(float minDbm, float maxDbm);
    void panadapterAdded(PanadapterModel* pan);
    void panadapterRemoved(const QString& panId);
    // Emitted when createPanadapter() is blocked because the radio's pan limit is reached.
    void panadapterLimitReached(int limit, const QString& model);
    // Emitted when the radio rejects a slice create command (e.g. limit reached across
    // all Multi-Flex clients — our local slice count may be below maxSlices()).
    void sliceCreateFailed(int limit, const QString& model);
    // Emitted when a pan needs xpixels/ypixels pushed (after profile change, reconnect, etc.)
    void panDimensionsNeeded(const QString& panId);
    // Emitted when the radio reports its antenna list (e.g. "ANT1,ANT2,RX_A,RX_B").
    void antListChanged(QStringList ants);
    // Local AetherSDR display aliases changed. The radio still uses canonical tokens.
    void antennaAliasesChanged();
    // Emitted when a power amplifier (e.g. PGXL) is detected or lost.
    void amplifierChanged(bool present);
    void ampStateChanged();   // amplifier operate/bypass changed
    // Raw KVS from the radio's amplifier status message (id, vac, meffa, temp, state, …).
    // Emitted on every update so the GUI can refresh telemetry without a direct PGXL connection.
    void ampTelemetryUpdated(const QMap<QString, QString>& kvs);
    void memoryChanged(int index);
    void memoryRemoved(int index);
    void memoriesCleared();
    void audioOutputChanged();
    // Emitted when multiFLEX is disabled and another client is already connected,
    // detected post-TCP-connect before client gui is sent. MainWindow should show
    // ConnectedStationsDialog and call resolveMultiFlexConflict() or cancelMultiFlexConflict().
    void multiFlexConflictDetected();
    // Emitted when TX ownership changes in Multi-Flex (another client transmitting)
    void txOwnerChanged(bool ownedByUs, const QString& otherStation);
    // Emitted when the set of other connected clients changes.
    void otherClientsChanged(int count, const QStringList& names);
    // Emitted when another GUI client logs in after our known client list.
    void clientConnected(quint32 handle,
                         const QString& source,
                         const QString& station,
                         const QString& program);
    // Emitted when GPS status changes (from "sub gps all").
    void gpsStatusChanged(const QString& status, int tracked, int visible,
                          const QString& grid, const QString& altitude,
                          const QString& lat, const QString& lon,
                          const QString& utcTime);
    // Emitted when the radio reports 10 MHz reference oscillator state.
    void oscillatorChanged();
    // Emitted when network quality assessment changes.
    // quality: "Off", "Excellent", "Very Good", "Good", "Fair", "Poor"
    // pingMs: round-trip time in milliseconds
    void networkQualityChanged(const QString& quality, int pingMs);
    // Emitted when the radio assigns a TX audio stream ID (DAX TX).
    void txAudioStreamReady(quint32 streamId);
    // Emitted when the radio assigns a remote audio TX stream ID (voice/VOX).
    void remoteTxStreamReady(quint32 streamId);
    // Audio TX gate for sample pipeline (separate from optimistic MOX UI state).
    void txAudioGateChanged(bool transmitting);
    // Raw interlock TX state (regardless of ownership — for DAX passthrough).
    void radioTransmittingChanged(bool transmitting);
    // Short operator-facing interlock warnings for the panadapter overlay.
    void interlockNotificationRequested(const QString& message);
    // Emitted when global profile list or active profile changes.
    void globalProfilesChanged();
    // Emitted on each successful ping response from the radio.
    void pingReceived();
    // Generic status relay — for dialogs that need to listen for specific objects.
    void statusReceived(const QString& object, const QMap<QString, QString>& kvs);

public:
    // Send a raw command to the radio (for dialogs that need direct protocol access).
    void sendCommand(const QString& cmd);

    // Request local PTT for our station. Sends "client set local_ptt=1" and applies
    // an optimistic update in case the radio doesn't echo the state change.
    void requestLocalPtt();

    // PC Audio: create/remove remote_audio_rx stream
    void createRxAudioStream();
    void removeRxAudioStream();

    // Send a command with a response callback (for firmware uploader, etc.)
    void sendCmdPublic(const QString& cmd, std::function<void(int code, const QString& body)> cb);

    // Radio software version string (from discovery broadcast, e.g. "4.1.5")
    QString softwareVersion() const { return m_version; }
    // SmartSDR protocol version from the V line (e.g. "1.4.0.0"), empty until connected
    QString protocolVersion() const { return m_protocolVersion; }

private slots:
    void onStatusReceived(const QString& object, const QMap<QString, QString>& kvs);
    void onMessageReceived(const ParsedMessage& msg);
    void onConnected();
    void onDisconnected();
    void onConnectionError(const QString& msg);
    void onVersionReceived(const QString& version);

private:
    void handleRadioStatus(const QMap<QString, QString>& kvs);
    void handleSliceStatus(int id, const QMap<QString, QString>& kvs, bool removed);
    void handleMeterStatus(const QString& rawBody);
    void handlePanadapterStatus(const QString& panId, const QMap<QString, QString>& kvs);
    void handleProfileStatus(const QString& object, const QMap<QString, QString>& kvs);
    void handleProfileStatusRaw(const QString& profileType, const QString& rawBody);
    void traceDaxStreamStatus(const QString& object, const QMap<QString, QString>& kvs);
    bool handleRemoteAudioRxStreamStatus(const QString& object,
                                         const QMap<QString, QString>& kvs);
    void scheduleRxAudioStreamEnsure(const QString& reason);
    void logRemoteAudioRxSummary(const QString& reason) const;

    void configurePan();
    void configureWaterfall();
    void registerAsGuiClient(const QString& clientId);
    void disconnectPendingClientsThen(std::function<void()> continuation);
    // LAN-only: subscribe to radio+client topics early, wait 400 ms for
    // the radio status burst, then check for a multiFLEX conflict before
    // sending client gui. Calls continuation() if no conflict is found.
    void peekForMultiFlexConflictThen(std::function<void()> continuation);
    void handleForcedClientDisconnect();
    void applyKnownGuiClients(const QStringList& handles,
                              const QStringList& programs,
                              const QStringList& stations,
                              const QStringList& ips,
                              const QStringList& hosts,
                              bool replaceExisting);
    bool shouldSuppressClientConnectionNotice(quint32 handle);
    void announceClientConnection(quint32 handle,
                                  const QString& source,
                                  const QString& station,
                                  const QString& program);

    // Route command to active connection (LAN or WAN)
    using ResponseCallback = RadioConnection::ResponseCallback;
    quint32 sendCmd(const QString& command, ResponseCallback cb = nullptr);
    quint32 clientHandle() const;
    PanadapterModel* ensureOwnedPanadapter(const QString& panId);
    void updateStreamFilters();
    void handleGpsStatus(const QString& rawBody);
    void emitOtherClientsChanged();
    QString antennaAliasRadioKey() const;
    bool reloadAntennaAliases() const;

    // Standalone mode: create a panadapter then attach a slice to it.
    void createDefaultSlice(const QString& freqMhz = "14.225000",
                            const QString& mode    = "USB",
                            const QString& antenna = "ANT1");
    void createDefaultSliceOnPan(const QString& panId,
                                 const QString& freqMhz,
                                 const QString& mode,
                                 const QString& antenna);

    RadioConnection*  m_connection{nullptr};
    QThread*          m_connThread{nullptr};
    // Sequence counter and callback map — owned by RadioModel on main thread.
    // RadioConnection no longer manages callbacks. (#502)
    std::atomic<quint32> m_seqCounter{1};
    QMap<quint32, ResponseCallback> m_pendingCallbacks;
    PanadapterStream* m_panStream{nullptr};
    QThread*          m_networkThread{nullptr};
    // Sub-models — value members on main thread (#502)
    MeterModel       m_meterModel;
    TunerModel       m_tunerModel;
    TransmitModel    m_transmitModel;
    EqualizerModel   m_equalizerModel;
    TnfModel         m_tnfModel;
    SpotModel        m_spotModel;
    CwxModel         m_cwxModel;
    DvkModel         m_dvkModel;
    NavtexModel      m_navtexModel;
    UsbCableModel    m_usbCableModel;
    DaxIqModel       m_daxIqModel;

    // NetCW stream — VITA-49 UDP delivery for low-latency CW keying
    quint32  m_netCwStreamId{0};
    int      m_netCwIndex{1};           // sequential dedup index
    QElapsedTimer m_netCwClock;          // 16-bit relative ms clock for time=0x....
    qint64   m_netCwLastSendMs{-1};
    void sendNetCwCommand(const QString& cmd, const QString& debugSource = {},
                          quint64 debugTraceId = 0, quint64 debugSourceMs = 0);
    QByteArray buildNetCwPacket(const QByteArray& payload);

    QString     m_name;
    QString     m_model;
    int         m_maxSlices{4};
    QString     m_version;          // software version from discovery (e.g. "4.1.5")
    QString     m_protocolVersion;  // protocol version from V line (e.g. "1.4.0.0")
    float       m_paTemp{0.0f};
    float       m_txPower{0.0f};
    QString     m_chassisSerial;
    QString     m_callsign;
    QString     m_nickname;
    QString     m_region;
    QString     m_radioOptions;
    QString     m_licenseRadioId;
    QString     m_licenseExpirationDate;
    QString     m_licenseMaxVersion;
    QString     m_licenseSubscription;   // e.g. "SmartSDR+", "SmartSDR", "Unknown"
    QString     m_ip;
    QString     m_netmask;
    QString     m_gateway;
    QString     m_mac;
    bool        m_hasStaticIp{false};
    QString     m_staticIp;
    QString     m_staticNetmask;
    QString     m_staticGateway;
    // Oscillator state
    QString     m_oscState;
    QString     m_oscSetting{"auto"};
    bool        m_oscLocked{false};
    bool        m_extPresent{false};
    bool        m_gpsdoPresent{false};
    bool        m_tcxoPresent{false};
    bool        m_binauralRx{false};
    bool        m_muteLocalWhenRemote{false};
    bool        m_autoSave{true};
    int         m_lineoutGain{50};
    bool        m_lineoutMute{false};
    int         m_headphoneGain{50};
    bool        m_headphoneMute{false};
    bool        m_frontSpeakerMute{false};
    int         m_freqErrorPpb{0};
    double      m_calFreqMhz{15.0};
    int         m_filterVoice{2};
    bool        m_filterVoiceAuto{false};
    int         m_filterCw{2};
    bool        m_filterCwAuto{true};
    int         m_filterDigital{2};
    bool        m_filterDigitalAuto{true};
    bool        m_lowLatencyDigital{true};
    bool        m_enforcePrivateIp{true};
    bool        m_remoteOnEnabled{false};
    bool        m_multiFlexEnabled{true};
    bool        m_txRequested{false}; // local MOX command intent (for edge sync)
    bool        m_cwKeyActive{false}; // true while CW key/paddle is held (#1379)
    bool        m_cwxActive{false};   // true while CWX send is in flight (#2047, #2097)
    bool        m_txAudioGate{false}; // actual TX audio gate state
    bool        m_radioTransmitting{false}; // raw interlock TX state, any owner
    QString     m_lastInterlockNotificationKey;
    qint64      m_lastInterlockNotificationMs{0};
    qint64      m_interlockNotificationArmedUntilMs{0};
    TransmitModel::PttSource m_pendingTransmitPreflightSource{TransmitModel::PttSource::Mox};
    TransmitModel::PttSource m_interlockNotificationSource{TransmitModel::PttSource::Mox};
    int         m_digitalVoiceTxSliceId{-1};
    QString     m_lastInterlockSource;   // last seen interlock source= (#2373)
                                         // SW/MIC/ACC/RCA/TUNE per FlexLib
                                         // v4.2.18 ParsePTTSource. Persists
                                         // across status updates that omit
                                         // the field; cleared on non-TX
                                         // interlock states and on disconnect.
    QStringList m_antList;
    mutable QString m_antennaAliasRadioKey;
    mutable QMap<QString, QString> m_antennaAliases;

    QMap<QString, PanadapterModel*> m_panadapters;  // panId → model
    QString m_activePanId;       // currently active panadapter
    QMap<QString, QMap<QString, QString>> m_pendingPanStatuses;

    bool    m_hasAmplifier{false};  // true if a power amp (PGXL) is detected
    QString m_ampHandle;             // amplifier handle for commands
    QString m_ampIp;                 // amplifier IP for direct connection
    QString m_ampModel;              // "PowerGeniusXL"
    bool    m_ampOperate{false};

    // GPS state
    QString m_gpsStatus;           // "Locked", "Present", "Not Present"
    int     m_gpsTracked{0};
    int     m_gpsVisible{0};
    QString m_gpsGrid;
    QString m_gpsAltitude;
    QString m_gpsLat;
    QString m_gpsLon;
    QString m_gpsTime;
    QString m_gpsSpeed;
    QString m_gpsFreqError;

    // Per-band TX settings (from "transmit band" and "interlock band" status)
    struct TxBandInfo {
        int     bandId{0};
        QString bandName;
        int     rfPower{100};
        int     tunePower{10};
        bool    inhibit{false};
        bool    hwAlc{false};
        bool    accTxReq{false};
        bool    rcaTxReq{false};
        bool    accTx{false};
        bool    tx1{false};
        bool    tx2{false};
        bool    tx3{false};
    };
    QMap<int, TxBandInfo> m_txBandSettings;
    int  m_tuneInhibitBandId{-1};  // band ID whose TX outputs were inhibited during tune
    bool m_tuneInhibitActive{false};

    int bandIdForFrequency(double freqMhz) const;  // map TX freq → band ID
    void applyTuneInhibit();    // suppress selected TX outputs before tune
    void restoreTuneInhibit();  // re-enable TX outputs after tune
    SliceModel* txSlice() const;
    QString localPttInterlockMessage(TransmitModel::PttSource source) const;
    QString txFilterFrequencyLimitMessage(int lowHz, int highHz) const;
    QString radioInterlockNotificationMessage(const QMap<QString, QString>& kvs) const;
    void armInterlockNotification(TransmitModel::PttSource source = TransmitModel::PttSource::Mox);
    bool interlockNotificationArmed() const;
    void emitInterlockNotification(const QString& message, const QString& key);

public:
    const QMap<int, TxBandInfo>& txBandSettings() const { return m_txBandSettings; }

    struct XvtrInfo {
        int     index{0};
        int     order{-1};
        QString name;
        double  rfFreq{0.0};
        double  ifFreq{0.0};
        double  loError{0.0};
        double  rxGain{0.0};
        double  maxPower{10.0};
        bool    rxOnly{false};
        bool    isValid{false};
        bool    hasIsValid{false};
    };
    const QMap<int, XvtrInfo>& xvtrList() const { return m_xvtrList; }

private:
    QMap<int, XvtrInfo> m_xvtrList;

    // Backward-compat helpers for active panadapter (Phase 1)
    QString activeWfId() const {
        auto* p = activePanadapter();
        return p ? p->waterfallId() : QString();
    }
    void setActiveWfId(const QString& id) {
        if (auto* p = activePanadapter()) p->setWaterfallId(id);
    }
    bool activePanResized() const {
        auto* p = activePanadapter();
        return p ? p->isResized() : false;
    }
    void setActivePanResized(bool r) {
        if (auto* p = activePanadapter()) p->setResized(r);
    }
    bool activeWfConfigured() const {
        auto* p = activePanadapter();
        return p ? p->isWaterfallConfigured() : false;
    }
    void setActiveWfConfigured(bool c) {
        if (auto* p = activePanadapter()) p->setWaterfallConfigured(c);
    }

private:
    QList<SliceModel*> m_slices;
    QMap<int, MemoryEntry> m_memories;
    QStringList m_globalProfiles;
    QString     m_activeGlobalProfile;
    RadioStatusOwnership::RemoteAudioRxTracking m_rxAudio;
    struct DaxStreamDebugState {
        QString type;
        quint32 clientHandle{0};
        int daxChannel{0};
        int daxIqChannel{0};
        int sliceId{-1};
        int daxIqRate{0};
        QString panId;
        QString ip;
        bool active{false};
        bool tx{false};
        bool activeKnown{false};
        bool txKnown{false};
    };
    QMap<quint32, DaxStreamDebugState> m_daxStreamDebug;
    quint32     m_daxTxStreamId{0};
    bool        m_daxTxActive{false};
    quint32     m_daxTxClientHandle{0};  // Tracked for diagnostics only — not consulted in routing.
    bool        m_daxTxCreatePending{false};
    QSet<quint32> m_deadDaxRxSeen;
    QSet<quint32> m_externalDaxTxSeen;
    QSet<quint32> m_externalDaxRxSeen;
    WanConnection* m_wanConn{nullptr};  // non-null when connected via SmartLink
    QString  m_wanPublicIp;
    quint16  m_wanUdpPort{4991};
    QSet<int>          m_ownedSliceIds;   // slice IDs that belong to our client
    bool               m_txOwnedByUs{true};  // true when tx_client_handle matches our handle
    bool               m_fullDuplex{false};
    int                m_rttyMarkDefault{2125};
    quint32            m_txClientHandle{0};  // handle of the client that owns TX
    QMap<quint32, ClientInfo> m_clientInfoMap; // handle → full client info
    std::function<void()> m_multiFlexContinuation; // saved continuation during conflict pause
    QMap<quint32, QString> m_clientStations;   // handle → station name (legacy, kept in sync)
    QList<quint32> m_pendingClientDisconnects; // handles chosen before connecting
    QSet<quint32> m_announcedClientConnections; // client login notices shown this session
    QSet<quint32> m_startupClientConnections; // clients present before our connect status replay
    QElapsedTimer m_clientConnectionNoticeTimer;
    static constexpr qint64 CLIENT_CONNECTION_STARTUP_SUPPRESS_MS = 5000;

    SleepInhibitor m_sleepInhibitor;     // prevents OS idle sleep while connected
    RadioInfo m_lastInfo;               // stored for auto-reconnect
    bool      m_intentionalDisconnect{false};
    bool      m_forcedDisconnectInProgress{false};
    QTimer    m_reconnectTimer;

    // ── Network quality monitor ──
    void startNetworkMonitor();
    void stopNetworkMonitor();
    void evaluateNetworkQuality();
    void resetNetworkHealthSamples();
    void recordNetworkHealthSample(int currentErrors, int currentPackets);

    enum class NetState { Off, Excellent, VeryGood, Good, Fair, Poor };
    double networkQualityTargetScore(int pingMs) const;
    NetState networkStateForScore(double score, NetState currentState) const;
    bool usesRemoteNetworkThresholds() const;

    static constexpr int LAN_PING_FAIR_MS = 50;
    static constexpr int LAN_PING_POOR_MS = 100;
    static constexpr int REMOTE_PING_FAIR_MS = 180;
    static constexpr int REMOTE_PING_POOR_MS = 350;
    static constexpr int NETWORK_LOSS_WINDOW_SAMPLES = 10;
    static constexpr int NETWORK_MIN_LOSS_WINDOW_PACKETS = 100;

    QTimer        m_pingTimer;           // 1-second interval
    QMetaObject::Connection m_networkPingConnection;
    // RTT now measured by RadioConnection::pingRttMeasured at socket-read time
    int           m_lastPingRtt{0};      // ms
    int           m_maxPingRtt{0};       // max RTT seen this session
    int           m_lastErrorCount{0};   // snapshot for delta
    int           m_lastPacketCount{0};  // snapshot for delta
    int           m_lossSamplePackets[NETWORK_LOSS_WINDOW_SAMPLES]{};
    int           m_lossSampleErrors[NETWORK_LOSS_WINDOW_SAMPLES]{};
    int           m_lossSampleCursor{0};
    int           m_lossSampleCount{0};
    int           m_packetLossWindowPackets{0};
    int           m_packetLossWindowErrors{0};
    double        m_networkQualityScore{100.0};
    NetState      m_netState{NetState::Off};
    int           m_pingMissCount{0};          // consecutive unanswered pings
    bool          m_pingDisconnectTriggered{false};
    static constexpr int PING_MISS_DISCONNECT = 5; // force disconnect after 5 missed pings (~5s)

    // Network diagnostics — byte counters for rate calculation

public:
    // Network diagnostics getters
    int     lastPingRtt()      const { return m_lastPingRtt; }
    int     maxPingRtt()       const { return m_maxPingRtt; }
    QString networkQuality()   const;
    int     packetLossWindowSeconds() const { return NETWORK_LOSS_WINDOW_SAMPLES; }
    int     packetLossWindowDrops() const { return m_packetLossWindowErrors; }
    int     packetLossWindowPackets() const { return m_packetLossWindowPackets; }
    double  packetLossPercent() const;
    int     audioPacketGapMs() const;
    int     audioPacketGapMaxMs() const;
    int     audioPacketJitterMs() const;
    int     packetDropCount()  const;
    int     packetTotalCount() const;
    qint64  rxBytes()          const;
    qint64  txBytes()          const;
    QString targetRadioIp()    const;
    QString selectedSourceMode() const;
    QString selectedSourcePath() const;
    QString localTcpEndpoint() const;
    QString localUdpEndpoint() const;
    bool    firstUdpPacketSeen() const;

    // Per-category stream stats (Audio, FFT, Waterfall, Meter, DAX)
    PanadapterStream::CategoryStats categoryStats(PanadapterStream::StreamCategory cat) const;
};

} // namespace AetherSDR
