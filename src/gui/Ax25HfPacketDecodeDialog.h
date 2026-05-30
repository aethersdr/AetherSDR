#pragma once

#include "PersistentDialog.h"
#include "core/tnc/AetherAx25LibmodemShim.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QQueue>

class QAbstractButton;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTextEdit;
class QTimer;

namespace AetherSDR {

class AudioEngine;
class KissTncServer;
class PacketActivityWidget;
class RadioModel;
class SliceModel;

class Ax25HfPacketDecodeDialog : public PersistentDialog {
    Q_OBJECT

public:
    explicit Ax25HfPacketDecodeDialog(AudioEngine* audio,
                                      RadioModel* radio,
                                      SliceModel* initialSlice = nullptr,
                                      QWidget* parent = nullptr);
    ~Ax25HfPacketDecodeDialog() override;

    void setAttachedSlice(SliceModel* slice);

private:
    void setModemProfile(Ax25ModemProfile profile, bool persist);
    void setDecodeEnabled(bool enabled);
    void handleRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate);
    void startAudioCapture();
    void finishAudioCapture(bool save);
    void startTransmitFromUi();
    void beginTransmission(const Ax25TransmitResult& tx, bool fromKiss);
    void beginTransmitWhenReady();
    void paceTransmitAudio();
    void finishTransmit(bool aborted, const QString& reason);

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

    AudioEngine* m_audio{nullptr};
    RadioModel* m_radio{nullptr};
    AetherAx25LibmodemShim* m_shim{nullptr};
    QStackedWidget* m_tabStack{nullptr};
    QAbstractButton* m_ax25Tab{nullptr};
    QAbstractButton* m_kissTab{nullptr};
    QRadioButton* m_hf300Profile{nullptr};
    QRadioButton* m_vhf1200Profile{nullptr};
    QCheckBox* m_enableDecode{nullptr};
    QLineEdit* m_txText{nullptr};
    QPushButton* m_txButton{nullptr};
    QTextEdit* m_log{nullptr};
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
    quint64 m_kissTxCount{0};
    quint64 m_kissRxCount{0};
};

} // namespace AetherSDR
