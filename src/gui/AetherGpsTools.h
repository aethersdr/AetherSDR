#pragma once

#include "WaveformInstallGate.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

namespace AetherSDR {

class FlexWaveformModel;
class RadioModel;
class WaveformInstaller;

// The radio accepted an AetherGPS install or removal, but its waveform list
// doesn't show the change yet; that takes a few seconds. One per radio,
// shared by the AetherGPS window's checkbox and the Waveforms window's
// Install menu, so neither offers a second upload meanwhile. Ends when the
// list agrees, when the session ends, or after 30 s. GUI thread only.
class AetherGpsSettle : public QObject {
    Q_OBJECT

public:
    enum class State { None, Install, Removal };

    // The radio's one instance, created on first use as a child of `waveforms`.
    static AetherGpsSettle* of(FlexWaveformModel& waveforms);

    State state() const { return m_state; }
    bool active() const { return m_state != State::None; }
    // Waits for the list to show the install or the removal (restarting the
    // 30 s); nothing to wait for when it already does.
    void start(bool installing);
    // The radio refused the change: nothing is coming to wait for.
    void cancel();

signals:
    void changed();

private:
    explicit AetherGpsSettle(FlexWaveformModel& waveforms);
    bool listed() const;

    FlexWaveformModel& m_waveforms;
    State m_state{State::None};
    QTimer m_timer;
};

// The AetherGPS relay on the radio (tools/aether-gps): whether the radio
// lists it, and installing or removing it. FlexRadio only: the relay is a
// waveform and reads the radio's gpsd. This class is the one place the
// shared AetherGPS window reaches the Flex waveform classes, so the window
// itself stays vendor-free (EB3 baseline row, by the maintainer's ruling).
class AetherGpsTools : public QObject {
    Q_OBJECT

public:
    explicit AetherGpsTools(RadioModel* model, QObject* parent = nullptr);

    // Whether this radio installs waveforms at all; the bar hides otherwise.
    bool radioTakesWaveforms() const;
    bool installed() const;
    // An install, or a removal the radio has not finished, is in progress:
    // any upload in this process, or an AetherGPS change from either window
    // that the radio's list doesn't show yet.
    bool busy() const;
    AetherGpsBlocker blocker() const;
    // Why the checkbox is unavailable, or empty.
    QString blockerReason() const;

    // Each refuses quietly (and emits changed()) when not allowed now.
    void install();
    void remove();

    // The Waveforms window installed (`installed`) or is removing AetherGPS:
    // wait for the radio's list to agree, as install() and remove() do.
    // stopWaiting() when the radio refuses the change.
    void awaitListing(bool installed);
    void stopWaiting();
    // An AetherGPS install or removal from either window that the radio
    // hasn't finished: busy() without other windows' uploads.
    bool awaitingRadio() const;

    // Writes the bundled package for a radio that takes the container
    // (`docker`) or the legacy waveform to the cache, for an installer.
    // Empty, with `error` set, on failure. The Waveforms window's Install
    // menu uses it too.
    static QString writePackage(bool docker, QString* error);

signals:
    // Installed, busy or the blocker may have changed.
    void changed();
    void installProgress(int percent);
    void failed(const QString& message);

private:
    QPointer<RadioModel> m_model;
    WaveformInstaller* m_installer{nullptr};
    bool m_removing{false};    // the remove command is in flight
    QPointer<AetherGpsSettle> m_settle;   // the radio's, shared with the Waveforms window
};

}  // namespace AetherSDR
