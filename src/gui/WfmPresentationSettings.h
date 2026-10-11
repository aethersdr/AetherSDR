#pragma once
#include <QObject>

namespace AetherSDR {
// Sole owner of the client-only WFM presentation document. Receiver state is
// persisted by its model/backend, never by these preferences.
class WfmPresentationSettings final : public QObject {
    Q_OBJECT
public:
    static WfmPresentationSettings& instance();
    bool broadcastOverlayEnabled() const;
    void setBroadcastOverlayEnabled(bool enabled);
    bool showLockScope() const;
    bool showDiagnostics() const;
    void setAppletOptions(bool scope, bool diagnostics);
signals:
    void overlayEnabledChanged(bool enabled);
private:
    WfmPresentationSettings() = default;
};
}
