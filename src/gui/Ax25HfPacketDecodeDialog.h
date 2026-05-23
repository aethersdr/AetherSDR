#pragma once

#include "PersistentDialog.h"
#include "core/tnc/MasterAx25LibmodemShim.h"

#include <QMetaObject>
#include <QPointer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTextEdit;
class QTimer;

namespace MasterSDR {

class AudioEngine;
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
    Ax25TonePolarity selectedTonePolarity() const;
    void setModemProfile(Ax25ModemProfile profile, bool persist);
    void setTonePolarity(Ax25TonePolarity polarity, bool persist);
    void setDecodeEnabled(bool enabled);
    void handleRxAudio(const QByteArray& monoFloat32Pcm, int sampleRate);
    void startAudioCapture();
    void finishAudioCapture(bool save);
    void startTransmitFromUi();
    void beginTransmitWhenReady();
    void paceTransmitAudio();
    void finishTransmit(bool aborted, const QString& reason);
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
    MasterAx25LibmodemShim* m_shim{nullptr};
    QRadioButton* m_hf300Profile{nullptr};
    QRadioButton* m_vhf1200Profile{nullptr};
    QCheckBox* m_enableDecode{nullptr};
    QRadioButton* m_polarityNormal{nullptr};
    QRadioButton* m_polarityReverse{nullptr};
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
    bool m_txActive{false};
    bool m_txPendingStream{false};
    bool m_txRestoreAudioDaxMode{false};
    bool m_txRestoreTransmitDax{false};
    bool m_txPreviousAudioDaxMode{false};
    bool m_txPreviousTransmitDax{false};
};

} // namespace MasterSDR
