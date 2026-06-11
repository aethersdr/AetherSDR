#pragma once

#include "PersistentDialog.h"
#include "core/tnc/AetherAx25LibmodemShim.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QThread>

class QAbstractButton;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QTextEdit;
class QTimer;
class QVBoxLayout;

namespace AetherSDR {

class AprsBeacon;
class AprsMessagesDialog;
class AprsMessenger;
class AprsStationList;
class AudioEngine;
class HeardList;
class KissTncServer;
#ifdef HAVE_MQTT
class MqttClient;
#endif
class PacketActivityWidget;
class PmsMailbox;
class RadioModel;
class SliceModel;
class TncTerminal;

// Persistence for the AetherModem KISS TNC. Per Constitution Principle V,
// new feature configuration lives as a nested JSON blob under one root key
// rather than a stack of flat AppSettings entries. Mirrors the
// CwDecodeSettings pattern.
//
// One-shot migration from the legacy flat keys is in migrateLegacy(); call
// once at startup before any reader runs.
class TncSettings {
public:
    // Defaults — kept as constants for the spinbox/UI to bound against.
    static constexpr int kDefaultPort = 8001;  // Dire Wolf convention
    // Lowest TCP port the spinbox lets the operator pick. Ports below 1024
    // need root on macOS / Linux; the bind would fail silently into
    // m_lastError and the listener would stay off. No reason to expose that
    // foot-gun.
    static constexpr int kMinPort = 1024;
    static constexpr int kMaxPort = 65535;

    static bool enabled()         { return readObj().value("enabled").toString("False") == "True"; }
    static bool startOnStartup()  { return readObj().value("startOnStartup").toString("False") == "True"; }
    static int  port()
    {
        const int p = readObj().value("port").toString(QString::number(kDefaultPort)).toInt();
        if (p < kMinPort || p > kMaxPort) return kDefaultPort;
        return p;
    }

    static void setEnabled(bool on);
    static void setStartOnStartup(bool on);
    static void setPort(int p);

    // One-shot migration from the three legacy flat keys
    // (AetherModemKissTncEnabled / AetherModemKissTncStartOnStartup /
    // AetherModemKissTncPort) into the nested blob. Safe to call repeatedly;
    // returns immediately if the nested blob already exists.
    static void migrateLegacy();

private:
    static QJsonObject readObj();
    static void write(const QJsonObject& o);
};

class Ax25HfPacketDecodeDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                      RadioModel* radio,
                                      SliceModel* initialSlice = nullptr,
                                      QWidget* parent = nullptr);
    ~Ax25HfPacketDecodeDialog() override;

    void setAttachedSlice(SliceModel* slice);
#ifdef HAVE_MQTT
    void setMqttClient(MqttClient* mqtt);
#endif

protected:
    // Command history (Up/Down) on the terminal input line.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setModemProfile(Ax25ModemProfile profile, bool persist);
    void setDecodeEnabled(bool enabled);
    void handleRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate);
    void startAudioCapture();
    void finishAudioCapture(bool save);
    void startTransmitFromUi();
    void startTransmit(const QString& text);
    void beginTransmission(const Ax25TransmitResult& tx, bool fromKiss);
    void beginTransmitWhenReady();
    void paceTransmitAudio();
    void finishTransmit(bool aborted, const QString& reason);

    // APRS client (APRS tab): station table, timed beacon, messaging.
    void buildAprsUi(QWidget* page, QVBoxLayout* pageLayout);
    void applyAprsConfigFromUi(bool persist);
    void refreshAprsStationTable();
    void refreshAprsStationAges();
    void refreshAprsPositionLabel();
    void handleAprsStationMenu(const QPoint& pos);
    void showAprsStationInfo(const QString& call);
    void openAprsMessagesDialog();
    void sendAprsMessageFromUi();
    void updateAprsEnvelopeButton();
    void handleGpsUpdate();

    // Personal Mailbox System (PMS) tab + service wiring.
    QWidget* buildMailboxPage();
    void setPmsEnabled(bool enabled, bool persist);
    void applyPmsConfigFromUi(bool persist);
    void refreshPmsStatus();

    // TNC Terminal tab: connected-mode AX.25 client (call out to a packet BBS).
    QWidget* buildTerminalPage();
    void submitTerminalInput();
    void refreshTerminalStatus();
    void applyTerminalConfigFromUi(bool persist);
    void refreshTerminalHeardCombo();
    // Hide the shared log panel and grow the tab stack on the Terminal tab so the
    // transcript gets the full viewport; restore the chrome on the other tabs.
    void updateTabChrome(int index);

    // KISS TNC tab + TCP server wiring.
    QWidget* buildKissTncPage();
    void setTncEnabled(bool enabled, bool persist);
    void applyTncStartOnStartup();
    void handleKissFrameFromClient(const QByteArray& ax25NoFcs);
    void maybeStartNextKissTx();
    void refreshTncStatus();
    void appendFrame(const Ax25DecodedFrame& frame);
    void updateDiagnostics(const Ax25DecoderDiagnostics& diagnostics);
    void updateHeartbeat();
    void refreshStatus();
    void refreshTransmitControls();
    void setDiagnosticsDebugEnabled(bool enabled, bool persist);
    void logAttachedSliceState(const QString& reason);
    void appendSystemLine(const QString& text);
    void appendTransmitLine(const Ax25TransmitFrame& frame);
    void appendDiagnosticsLine(const Ax25DecoderDiagnostics& diagnostics);
    QString formatTerminalLine(const Ax25DecodedFrame& frame) const;
    QString defaultTransmitSource() const;
    QString transmitSliceSummary() const;
#ifdef HAVE_MQTT
    void publishFrameMqtt(const Ax25DecodedFrame& frame);
    void handleMqttMessage(const QString& topic, const QByteArray& payload);
#endif

    AudioEngine* m_audio{nullptr};
    RadioModel* m_radio{nullptr};
    AetherAx25LibmodemShim* m_shim{nullptr};
    QThread m_shimThread;
    Ax25DemodConfig m_shimConfig;
    QStackedWidget* m_tabStack{nullptr};
    QAbstractButton* m_ax25Tab{nullptr};
    QAbstractButton* m_kissTab{nullptr};
#ifdef HAVE_MQTT
    QPointer<MqttClient> m_mqtt;
#endif
    QRadioButton* m_hf300Profile{nullptr};
    QRadioButton* m_vhf1200Profile{nullptr};
    QCheckBox* m_enableDecode{nullptr};
    QCheckBox* m_modemAutostart{nullptr};
    QLineEdit* m_txText{nullptr};
    QPushButton* m_txButton{nullptr};
    QWidget* m_txFrame{nullptr};
    QTextEdit* m_log{nullptr};
    QWidget* m_logFrame{nullptr};
    QLabel* m_modemStatusDot{nullptr};
    QLabel* m_modemStatusValue{nullptr};
    QLabel* m_gainStageDot{nullptr};
    QLabel* m_gainStageValue{nullptr};
    QLabel* m_packetActivityTitle{nullptr};
    PacketActivityWidget* m_packetActivity{nullptr};
    QPushButton* m_clearButton{nullptr};
    QPushButton* m_captureButton{nullptr};
    QTimer* m_heartbeatTimer{nullptr};
    QTimer* m_txPaceTimer{nullptr};
    QPointer<SliceModel> m_attachedSlice;
    QMetaObject::Connection m_sliceSquelchConnection;
    QMetaObject::Connection m_sliceModeConnection;
    int m_attachedSliceId{-1};
    int m_frameCount{0};
    QDateTime m_enabledUtc;
    QDateTime m_lastDecodeUtc;
    QDateTime m_lastDiagnosticsUtc;
    QDateTime m_lastNoAudioNoticeUtc;
    Ax25DecoderDiagnostics m_lastDiagnostics;
    quint64 m_lastActivityHdlc{0};
    quint64 m_lastActivityAccepted{0};
    QByteArray m_capturePcm;
    int m_captureSampleRate{0};
    qsizetype m_captureTargetBytes{0};
    bool m_captureActive{false};
    bool m_diagnosticsDebugEnabled{false};
    QByteArray m_txPcm;
    Ax25TransmitResult m_pendingTx;
    qsizetype m_txOffsetBytes{0};
    int m_txChunkIndex{0};
    int m_txChunkCount{0};
    // TX pacing health: detects GUI-thread stalls starving the 20 ms pacer.
    QElapsedTimer m_txPaceClock;
    qint64 m_txPaceLastChunkMs{-1};
    qint64 m_txPaceMaxGapMs{0};
    int m_txPaceLateChunks{0};
    bool m_txActive{false};
    bool m_txPendingStream{false};
    bool m_txRestoreAudioDaxMode{false};
    bool m_txRestoreTransmitDax{false};
    bool m_txPreviousAudioDaxMode{false};
    bool m_txPreviousTransmitDax{false};
    bool m_txFromKiss{false};

    // KISS TNC server (TCP) and its controls.
    KissTncServer* m_kissServer{nullptr};
    QCheckBox* m_tncEnable{nullptr};
    QCheckBox* m_tncStartOnStartup{nullptr};
    QSpinBox* m_tncPort{nullptr};
    QLabel* m_tncStatusDot{nullptr};
    QLabel* m_tncStatusValue{nullptr};
    QQueue<QByteArray> m_kissTxQueue;
    // Number of 250 ms radio-busy retries currently elapsed on the head-of-
    // queue frame. Capped (kMaxKissTxBusyRetries) so a stuck-transmitting
    // radio can't spin maybeStartNextKissTx() forever and starve later
    // frames behind it. Reset on each new dequeue.
    int m_kissTxBusyRetries{0};
    quint64 m_kissTxCount{0};
    quint64 m_kissRxCount{0};

    // Shared station-heard log (feeds the terminal MHEARD + quick-connect).
    HeardList* m_heard{nullptr};

    // APRS client services (APRS tab) and its controls.
    AprsStationList* m_aprsStations{nullptr};
    AprsMessenger* m_aprsMessenger{nullptr};
    AprsBeacon* m_aprsBeacon{nullptr};
    QPointer<AprsMessagesDialog> m_aprsMessagesDialog;
    QTableWidget* m_aprsTable{nullptr};
    QLineEdit* m_aprsMyCall{nullptr};
    QComboBox* m_aprsSymbol{nullptr};
    QLineEdit* m_aprsPath{nullptr};
    QCheckBox* m_aprsBeaconEnable{nullptr};
    QSpinBox* m_aprsBeaconInterval{nullptr};
    QLineEdit* m_aprsBeaconText{nullptr};
    QPushButton* m_aprsBeaconNow{nullptr};
    QLabel* m_aprsPositionValue{nullptr};
    QLineEdit* m_aprsManualLat{nullptr};
    QLineEdit* m_aprsManualLon{nullptr};
    QLineEdit* m_aprsMsgTo{nullptr};
    QLineEdit* m_aprsMsgText{nullptr};
    QPushButton* m_aprsMsgSend{nullptr};
    QPushButton* m_aprsEnvelope{nullptr};

    // TNC Terminal service (connected-mode AX.25 client) and its controls.
    TncTerminal* m_terminal{nullptr};
    QAbstractButton* m_terminalTab{nullptr};
    QLineEdit* m_terminalMyCall{nullptr};
    QLineEdit* m_terminalTarget{nullptr};
    QComboBox* m_terminalHeardCombo{nullptr};
    QPushButton* m_terminalConnectButton{nullptr};
    QPushButton* m_terminalCmdButton{nullptr};
    QPushButton* m_terminalMheardButton{nullptr};
    QSpinBox* m_terminalRetrySecs{nullptr};
    QSpinBox* m_terminalMaxTries{nullptr};
    QSpinBox* m_terminalPaclen{nullptr};
    QSpinBox* m_terminalTxTail{nullptr};
    // PTT tail (ms) after TX audio, before unkey. Operator-tunable to shrink the
    // half-duplex turnaround so we hear the peer's next frame sooner.
    int m_txTailMs{150};
    QCheckBox* m_terminalLogEnable{nullptr};
    QTextEdit* m_terminalView{nullptr};
    QLineEdit* m_terminalInput{nullptr};
    QPushButton* m_terminalSendButton{nullptr};
    QLabel* m_terminalStatusDot{nullptr};
    QLabel* m_terminalStatusValue{nullptr};
    QStringList m_terminalHistory;
    int m_terminalHistoryIndex{0};
    QString m_lastDialedCall; // persisted across restarts (last BBS connected)

    // Personal Mailbox System (PMS) service and its controls.
    PmsMailbox* m_pms{nullptr};
    QAbstractButton* m_mailboxTab{nullptr};
    QCheckBox* m_pmsEnable{nullptr};
    QLineEdit* m_pmsListenCall{nullptr};
    QLineEdit* m_pmsAliasCall{nullptr};
    QLineEdit* m_pmsWelcome{nullptr};
    QCheckBox* m_pmsBeaconEnable{nullptr};
    QLineEdit* m_pmsBeaconText{nullptr};
    QLabel* m_pmsStatusDot{nullptr};
    QLabel* m_pmsStatusValue{nullptr};
    QLabel* m_pmsCallersValue{nullptr};
    QLabel* m_pmsStatsValue{nullptr};
};

} // namespace AetherSDR
