#pragma once

#include <QObject>
#include <QMap>

#include "core/backends/NotchDelta.h"

namespace AetherSDR {

struct TnfEntry {
    int    id{0};
    double freqMhz{0.0};
    int    widthHz{100};
    int    depthDb{1};       // 1=normal, 2=deep, 3=very deep
    bool   permanent{false};
};

class TnfModel : public QObject {
    Q_OBJECT

public:
    explicit TnfModel(QObject* parent = nullptr);

    // ── Accessors ────────────────────────────────────────────────────────
    const QMap<int, TnfEntry>& tnfs() const { return m_tnfs; }
    const TnfEntry* tnf(int id) const;
    bool globalEnabled() const { return m_globalEnabled; }

    // ── Status parsing (called from RadioModel) ─────────────────────────
    // The Flex path: raw `tnf <id> …` status keys off the command plane.
    void applyTnfStatus(int id, const QMap<QString, QString>& kvs);
    // The typed path, for a backend whose notches live in this process and are
    // reported through IRadioBackend::notchChanged rather than as wire text.
    // Creates the entry if the id is new, which is how an HL2 notch first
    // appears — the backend mints the id, exactly as a Flex radio does.
    void applyNotchDelta(int id, const NotchDelta& delta);
    void removeTnf(int id);
    void applyGlobalEnabled(bool on);

    // ── Commands (emit commandReady) ────────────────────────────────────
    void createTnf(double freqMhz);
    void setTnfFreq(int id, double freqMhz);
    void setTnfWidth(int id, int widthHz);
    void setTnfDepth(int id, int depthDb);
    void setTnfPermanent(int id, bool on);
    void requestRemoveTnf(int id);
    void requestGlobalTnfEnabled(bool on);

    void clear();

signals:
    void tnfChanged(int id);
    void tnfRemoved(int id);
    void globalEnabledChanged(bool on);

    // Typed intents, routed by RadioModel to IRadioBackend (FlexBackend builds the
    // SmartSDR `tnf …` text; a host-DSP backend implements them in software).
    // Create carries no id: the backend assigns it (a Flex mints it in the radio)
    // and the notch arrives back through applyTnfStatus/applyNotchDelta.
    void notchCreateRequested(double centerHz, double widthHz);
    void notchChangeRequested(int id, const NotchDelta& delta);
    void notchRemoveRequested(int id);
    void notchesEnabledRequested(bool on);

private:
    QMap<int, TnfEntry> m_tnfs;
    bool m_globalEnabled{true};
};

} // namespace AetherSDR
