#pragma once

#include <QList>
#include <QString>
#include <QStringView>

namespace AetherSDR {

bool aetherDspModeRequiresDisable(QStringView mode);

struct AetherDspSliceAudioState {
    QString mode;
    bool muted{false};
    float gain{0.0f};
};

bool aetherDspMixRequiresDisable(const QList<AetherDspSliceAudioState>& slices);

class AetherDspModePolicy {
public:
    enum class ActionKind {
        None,
        Disable,
        Enable,
    };

    struct Action {
        ActionKind kind{ActionKind::None};
        QString method;
    };

    Action update(bool restricted, QStringView enabledMethod);
    void noteUserOverride();

    bool restricted() const { return m_restricted; }
    QString methodForPersistence(QStringView enabledMethod) const;

private:
    bool m_restricted{false};
    bool m_userOverride{false};
    QString m_autoDisabledMethod;
};

} // namespace AetherSDR
