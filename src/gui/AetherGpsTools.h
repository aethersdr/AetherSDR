#pragma once

#include "WaveformInstallGate.h"

#include <QObject>
#include <QPointer>
#include <QString>

namespace AetherSDR {

class RadioModel;
class WaveformInstaller;

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
    // An install, or a removal the radio has not finished, is in progress.
    bool busy() const;
    AetherGpsBlocker blocker() const;
    // Why the checkbox is unavailable, or empty.
    QString blockerReason() const;

    // Each refuses quietly (and emits changed()) when not allowed now.
    void install();
    void remove();

signals:
    // Installed, busy or the blocker may have changed.
    void changed();
    void installProgress(int percent);
    void failed(const QString& message);

private:
    void removalAccepted();

    QPointer<RadioModel> m_model;
    WaveformInstaller* m_installer{nullptr};
    bool m_removing{false};    // the remove command is in flight
    // The radio accepted a removal but still lists the waveform: it takes a
    // few seconds to carry it out. Ends when the row goes, or after 30 s.
    bool m_removalPending{false};
};

}  // namespace AetherSDR
