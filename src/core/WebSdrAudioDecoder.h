#pragma once

#include <QByteArray>
#include <QVector>
#include <cstdint>

namespace AetherSDR {

// Decoder for the PA3FWM WebSDR audio stream (ws://<host>/~~stream).
//
// Direct C++ port of the server's own websdr-sound.js onmessage handler
// (Copyright 2007-2018 P.T. de Boer, pa3fwm@websdr.org). The reference
// implementation and protocol notes live in the standalone `websdr-client`
// repo (websdr_client.py / PROTOCOL.md).
//
// Stateful: feed() one binary WebSocket message at a time; decoded Int16 mono
// samples (at nativeRate() Hz) accumulate and are drained by takeSamples().
// Not thread-safe — own one instance per stream on its worker thread.
class WebSdrAudioDecoder {
public:
    WebSdrAudioDecoder();

    // Decode one binary WebSocket message, appending samples to the buffer.
    void feed(const QByteArray& frame);

    // Drain and return all samples decoded so far (Int16 mono @ nativeRate()).
    QVector<qint16> takeSamples();

    int  nativeRate() const { return m_D; }   // audio sample rate in Hz (0x81)
    int  sMeter()     const { return m_smeter; }
    bool stopped()    const { return m_stopped; }

private:
    void resetPredictor();
    // Decode a compressed block (128 samples). Updates the byte pointer `n`
    // and bit offset `r`; returns via references.
    void decodeBlock(const quint8* t, int len, int& n, int& r);

    // Predictor / stream state (persists across frames).
    double  m_coeff[20];
    double  m_hist[20];
    double  m_ie    = 0.0;
    int     m_re    = 1;        // gain exponent (sticky)
    int     m_oe    = 1;        // quantiser scale (0x82)
    int     m_ae    = 0;        // filter/mode byte (0x83)
    int     m_ee    = -1;       // active filter index
    int     m_D     = 8000;     // audio sample rate (0x81)
    int     m_smeter = 0;
    bool    m_stopped = false;

    QVector<qint16> m_out;
};

} // namespace AetherSDR
