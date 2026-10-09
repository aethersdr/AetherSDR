#pragma once

#include "VoiceKeyer.h"

#include <QLatin1String>
#include <QMap>
#include <QString>
#include <QVector>

#include <functional>

namespace AetherSDR {

// The recording record is shared with the client-side keyer; the historical
// name stays for existing callers.
using DvkRecording = VoiceKeyerRecording;

// The radio-hosted voice keyer: SmartSDR `dvk` verbs, recordings stored on the
// radio (SmartSDR+ required). The panel drives it through VoiceKeyer.
class DvkModel : public VoiceKeyer {
    Q_OBJECT
public:
    explicit DvkModel(QObject* parent = nullptr);

    // The radio keeps slots 1-12 and stops a recording at 10 s (SmartSDR API
    // wiki, TCPIP-dvk). A name is at most 63 bytes including its quotes.
    static constexpr int kSlotCount = 12;
    static constexpr int kMaxRecordingMs = 10'000;
    static constexpr int kMaxNameBytes = 61;

    // State
    Status status() const override { return m_status; }
    int activeId() const override { return m_activeId; }
    bool enabled() const { return m_enabled; }
    // The radio does not refuse overlapping operations, so a new one may only
    // start from idle (Unknown, before any status, fails open), with no start
    // of ours still awaiting its reply and no WAV transfer running.
    bool canStartOperation() const override;
    // The start we sent and are still waiting on, or Unknown for none.
    Status pendingOperation() const override { return m_pending; }
    // DvkWavTransfer reports a running import or export here.
    void setTransferActive(bool active);
    // The radio answered a dvk command with 50004001 (feature not licensed).
    bool licenseRefused() const { return m_licenseRefused; }
    // Latches a 50004001 refusal from any dvk path, including WAV transfers.
    void noteRefusal(uint code);
    // The radio later reported the feature licensed (its status outranks an
    // earlier refusal, e.g. a subscription activated mid-session).
    void clearRefusal();
    // The radio's name for the DVK entitlement in `license feature` status.
    static constexpr QLatin1String kLicenseFeature{"digital_voice_keyer"};
    const QVector<VoiceKeyerRecording>& recordings() const override { return m_recordings; }

    // Per-keyer limits the panel shows and enforces. The radio caps a
    // recording itself and names an empty slot its own way, so these report
    // the radio's values; the statics stay because this class and its tests
    // use them directly.
    int maxRecordingMs() const override { return kMaxRecordingMs; }
    int maxNameBytes() const override { return kMaxNameBytes; }
    QString defaultSlotName(int id) const override { return defaultName(id); }

    static QString defaultName(int id);
    // Drops the characters the radio cannot carry in a quoted name (`"`, `'`,
    // `|`), collapses spaces as the radio does, and trims to kMaxNameBytes.
    static QString sanitizeName(const QString& name);

    // Commands
    void recStart(int id) override;
    void recStop(int id) override;
    void previewStart(int id) override;
    void previewStop(int id) override;
    void playbackStart(int id) override;
    void playbackStop(int id) override;
    void clear(int id) override;
    void remove(int id) override;
    void setName(int id, const QString& name) override;

    // WAV import/export. The transfer itself (DvkWavTransfer) is Flex wire
    // code this model must not reach, so the model asks for it by signal and
    // MainWindow connects the transfer — see wavUploadRequested. The busy
    // probe reports whether that transfer is mid-flight; with none installed,
    // import/export is unavailable.
    void importWav(int id, const QString& path) override;
    void exportWav(int id, const QString& path) override;
    bool canTransferWav() const override { return static_cast<bool>(m_transferBusyProbe); }
    bool isTransferring() const override { return m_transferBusyProbe && m_transferBusyProbe(); }
    void setWavTransferBusyProbe(std::function<bool()> probe) { m_transferBusyProbe = std::move(probe); }

    QString sourceLabel() const override { return QStringLiteral("Radio"); }

    // Status parsing (called from RadioModel)
    void applyStatus(const QString& object, const QMap<QString, QString>& kvs);

    // Connection-scoped state goes with the connection.
    void reset();

    // Called by RadioModel when a reply to a DVK command arrives.  Non-zero
    // codes are forwarded as commandFailed() so the UI can surface them
    // instead of leaving the operation silently rejected. (#3377)
    void handleCommandResponse(const QString& verb, int id, uint code, const QString& body);

    // Map a SmartSDR response code to a human-readable hint.  Known codes
    // come from FlexLib's SsdrErrors enum (Principle I); unknown codes
    // render as bare hex.
    static QString dvkErrorString(uint code);

signals:
    // Emitted for commands that need response correlation.  RadioModel
    // attaches a callback that invokes handleCommandResponse() with the
    // verb + slot id captured here. (#3377)
    void replyCommandReady(const QString& cmd, const QString& verb, int id);
    void licenseRefusedChanged(bool refused);
    // The operator asked to import or export a slot's WAV; MainWindow routes
    // these to DvkWavTransfer.
    void wavUploadRequested(int id, const QString& path);
    void wavDownloadRequested(int id, const QString& path);

private:
    static constexpr uint kNotLicensed = 0x50004001u;

    Status m_status{Unknown};
    int m_activeId{-1};
    bool m_enabled{false};
    bool m_licenseRefused{false};
    Status m_pending{Unknown};
    int m_pendingId{-1};
    bool m_transferActive{false};

    void setPending(Status pending);
    QVector<VoiceKeyerRecording> m_recordings;
    std::function<bool()> m_transferBusyProbe;

    VoiceKeyerRecording* findRecording(int id);
};

} // namespace AetherSDR
