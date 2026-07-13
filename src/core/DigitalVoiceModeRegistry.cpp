#include "DigitalVoiceModeRegistry.h"

#include <QMutexLocker>

namespace AetherSDR {

namespace {

const QList<DigitalVoiceModeDescriptor> kSupportedModes {
    {
        DigitalVoiceModeId::DStar,
        QStringLiteral("DStar"),
        QStringLiteral("D-STAR"),
        QStringLiteral("DSTR"),
        QStringLiteral("DFM"),
        QStringLiteral("AetherDStar")
    }
};

} // namespace

DigitalVoiceModeRegistry& DigitalVoiceModeRegistry::instance()
{
    static DigitalVoiceModeRegistry registry;
    return registry;
}

const QList<DigitalVoiceModeDescriptor>& DigitalVoiceModeRegistry::supportedModes()
{
    return kSupportedModes;
}

const DigitalVoiceModeDescriptor& DigitalVoiceModeRegistry::descriptor(
    DigitalVoiceModeId id)
{
    for (const DigitalVoiceModeDescriptor& mode : kSupportedModes) {
        if (mode.id == id) {
            return mode;
        }
    }
    return kSupportedModes.first();
}

std::optional<DigitalVoiceModeId> DigitalVoiceModeRegistry::modeForRadioMode(
    const QString& radioMode)
{
    for (const DigitalVoiceModeDescriptor& mode : kSupportedModes) {
        if (mode.radioMode.compare(radioMode, Qt::CaseInsensitive) == 0) {
            return mode.id;
        }
    }
    return std::nullopt;
}

bool DigitalVoiceModeRegistry::activateMode(DigitalVoiceModeId id, QString* error)
{
    QMutexLocker locker(&m_mutex);
    if (m_activeMode.has_value() && m_activeMode.value() != id) {
        if (error) {
            *error = QStringLiteral("Another digital-voice mode already owns the ThumbDV");
        }
        return false;
    }
    m_activeMode = id;
    return true;
}

std::optional<DigitalVoiceSliceClaim> DigitalVoiceModeRegistry::deactivateMode(
    DigitalVoiceModeId id)
{
    std::optional<DigitalVoiceSliceClaim> claim;
    bool releasedSlice = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_activeMode.has_value() || m_activeMode.value() != id) {
            return std::nullopt;
        }
        if (m_activeSliceId >= 0) {
            claim = DigitalVoiceSliceClaim{id, m_activeSliceId, m_previousMode};
            releasedSlice = true;
        }
        m_activeMode.reset();
        m_activeSliceId = -1;
        m_previousMode.clear();
    }
    if (releasedSlice) {
        emit activeSliceChanged(-1);
    }
    return claim;
}

bool DigitalVoiceModeRegistry::claimSlice(DigitalVoiceModeId id,
                                          int sliceId,
                                          QString* error)
{
    return claimSlice(id, sliceId, descriptor(id).underlyingMode, error);
}

bool DigitalVoiceModeRegistry::claimSlice(DigitalVoiceModeId id,
                                          int sliceId,
                                          const QString& previousMode,
                                          QString* error)
{
    return transferSlice(id, sliceId, previousMode, nullptr, error);
}

bool DigitalVoiceModeRegistry::transferSlice(
    DigitalVoiceModeId id,
    int sliceId,
    const QString& previousMode,
    std::optional<DigitalVoiceSliceClaim>* displaced,
    QString* error)
{
    bool changedSlice = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_activeMode.has_value() || m_activeMode.value() != id) {
            if (error) {
                *error = QStringLiteral("The requested digital-voice mode is not running");
            }
            return false;
        }
        if (m_activeSliceId >= 0 && m_activeSliceId != sliceId) {
            if (displaced == nullptr) {
                if (error) {
                    *error = QStringLiteral("Digital voice is already active on slice %1")
                        .arg(m_activeSliceId);
                }
                return false;
            }
            *displaced = DigitalVoiceSliceClaim{id, m_activeSliceId, m_previousMode};
        } else if (displaced != nullptr) {
            displaced->reset();
        }
        QString restoreMode = previousMode.trimmed().toUpper();
        if (restoreMode.isEmpty() || modeForRadioMode(restoreMode).has_value()) {
            restoreMode = descriptor(id).underlyingMode;
        }
        const bool moved = m_activeSliceId >= 0 && m_activeSliceId != sliceId;
        changedSlice = m_activeSliceId != sliceId;
        m_activeSliceId = sliceId;
        if (m_previousMode.isEmpty() || moved) {
            m_previousMode = restoreMode;
        }
    }
    if (changedSlice) {
        emit activeSliceChanged(sliceId);
    }
    return true;
}

void DigitalVoiceModeRegistry::releaseSlice(int sliceId)
{
    bool releasedSlice = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_activeSliceId == sliceId) {
            m_activeSliceId = -1;
            m_previousMode.clear();
            releasedSlice = true;
        }
    }
    if (releasedSlice) {
        emit activeSliceChanged(-1);
    }
}

std::optional<DigitalVoiceModeId> DigitalVoiceModeRegistry::activeMode() const
{
    QMutexLocker locker(&m_mutex);
    return m_activeMode;
}

int DigitalVoiceModeRegistry::activeSliceId() const
{
    QMutexLocker locker(&m_mutex);
    return m_activeSliceId;
}

std::optional<DigitalVoiceSliceClaim> DigitalVoiceModeRegistry::activeClaim() const
{
    QMutexLocker locker(&m_mutex);
    if (!m_activeMode.has_value() || m_activeSliceId < 0) {
        return std::nullopt;
    }
    return DigitalVoiceSliceClaim{
        m_activeMode.value(),
        m_activeSliceId,
        m_previousMode,
    };
}

} // namespace AetherSDR
