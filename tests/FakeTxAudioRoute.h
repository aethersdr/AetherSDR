#pragma once

// A scripted radio for GeneratedAudioTransmitter tests: records every call,
// with the time it happened, and answers the route questions from fields.

#include "core/GeneratedAudioTransmitter.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

namespace AetherSDR {

struct FakeTxAudioRoute : TxAudioRoute {
    // Answers.
    QString refusal;
    bool needs = false;
    bool ready = false;
    bool requestAccepted = true;
    bool waitsRadio = false;
    bool radioKeyed = false;
    bool keyEngages = true;
    // false: the route cannot even request the key (no transmit authority).
    bool keyAccepted = true;
    bool finishQueued = true;
    // -1: never report the barrier; otherwise report it with this drain.
    int drainMs = 0;
    GeneratedAudioTransmitter* tx = nullptr;
    std::function<void()> onKeyOn;   // e.g. refuse synchronously

    // Record.
    QStringList calls;
    QList<qint64> at;
    QByteArray sent;
    int sendCalls = 0;
    qint64 firstSendMs = -1;
    bool admitAccepted = true;
    bool keyed = false;
    QElapsedTimer clock;

    FakeTxAudioRoute() { clock.start(); }

    void note(const QString& c) { calls << c; at << clock.elapsed(); }
    qint64 timeOf(const QString& c) const
    {
        const int i = calls.indexOf(c);
        return i < 0 ? -1 : at[i];
    }

    QString startRefusal() const override { return refusal; }
    bool admitOperation() override { note("admit"); return admitAccepted; }
    bool needsStream() const override { return needs; }
    bool streamReady() const override { return ready; }
    bool requestStream() override { note("requestStream"); return requestAccepted; }
    void claimAudioPath() override { note("claim"); }
    void releaseAudioPath() override { note("release"); }
    bool keyOn() override
    {
        note("keyOn");
        if (!keyAccepted)
            return false;
        keyed = keyEngages;
        if (onKeyOn) onKeyOn();
        return true;
    }
    void keyOff() override { note("keyOff"); keyed = false; }
    bool isKeyed() const override { return keyed; }
    bool waitsForRadioPtt() const override { return waitsRadio; }
    bool isRadioKeyed() const override { return radioKeyed; }
    void sendAudio(const QByteArray& pcm) override
    {
        if (firstSendMs < 0) firstSendMs = clock.elapsed();
        if (sendCalls++ == 0) note("send");
        sent += pcm;
    }
    bool finishAudio(quint64 token) override
    {
        note("finish");
        if (finishQueued && drainMs >= 0 && tx) {
            QTimer::singleShot(0, tx, [t = tx, token, d = drainMs] { t->onAudioFinished(token, d); });
        }
        return finishQueued;
    }
    void clearAudio() override { note("clear"); }
};

} // namespace AetherSDR
