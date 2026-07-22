#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QList>

#include <optional>

namespace AetherSDR {

enum class DigitalVoiceModeId {
    DStar
};

struct DigitalVoiceModeDescriptor {
    DigitalVoiceModeId id;
    QString settingsId;
    QString displayName;
    QString radioMode;
    QString underlyingMode;
    QString waveformName;
};

struct DigitalVoiceSliceClaim {
    DigitalVoiceModeId mode{DigitalVoiceModeId::DStar};
    int sliceId{-1};
    QString previousMode;
};

class DigitalVoiceModeRegistry : public QObject
{
    Q_OBJECT

public:
    static DigitalVoiceModeRegistry& instance();

    static const QList<DigitalVoiceModeDescriptor>& supportedModes();
    static const DigitalVoiceModeDescriptor& descriptor(DigitalVoiceModeId id);
    static std::optional<DigitalVoiceModeId> modeForRadioMode(const QString& radioMode);

    bool activateMode(DigitalVoiceModeId id, QString* error = nullptr);
    std::optional<DigitalVoiceSliceClaim> deactivateMode(DigitalVoiceModeId id);
    bool claimSlice(DigitalVoiceModeId id, int sliceId, QString* error = nullptr);
    bool claimSlice(DigitalVoiceModeId id,
                    int sliceId,
                    const QString& previousMode,
                    QString* error = nullptr);
    bool transferSlice(DigitalVoiceModeId id,
                       int sliceId,
                       const QString& previousMode,
                       std::optional<DigitalVoiceSliceClaim>* displaced,
                       QString* error = nullptr);
    void releaseSlice(int sliceId);

    std::optional<DigitalVoiceModeId> activeMode() const;
    int activeSliceId() const;
    std::optional<DigitalVoiceSliceClaim> activeClaim() const;

signals:
    void activeSliceChanged(int sliceId);

private:
    DigitalVoiceModeRegistry() = default;

    mutable QMutex m_mutex;
    std::optional<DigitalVoiceModeId> m_activeMode;
    int m_activeSliceId{-1};
    QString m_previousMode;
};

} // namespace AetherSDR
