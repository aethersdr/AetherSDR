// **************************************************************** //
// libmodem - APRS modem                                            //
// Version 0.1.0                                                    //
// https://github.com/iontodirel/libmodem                           //
// Copyright (c) 2025 Ion Todirel                                   //
// **************************************************************** //
//
// demodulator.h
//
// MIT License
//
// Copyright (c) 2026 Ion Todirel
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include <cstdint>
#include <vector>

#ifndef LIBMODEM_NAMESPACE
#define LIBMODEM_NAMESPACE libmodem
#endif
#ifndef LIBMODEM_NAMESPACE_BEGIN
#define LIBMODEM_NAMESPACE_BEGIN namespace LIBMODEM_NAMESPACE {
#endif
#ifndef LIBMODEM_NAMESPACE_REFERENCE
#define LIBMODEM_NAMESPACE_REFERENCE libmodem :: 
#endif
#ifndef LIBMODEM_NAMESPACE_END
#define LIBMODEM_NAMESPACE_END }
#endif

LIBMODEM_NAMESPACE_BEGIN

// **************************************************************** //
//                                                                  //
//                                                                  //
// demod_result                                                     //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct demod_result
{
    uint8_t bit = 0;
    double confidence = 0.0;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// hard_limiter                                                     //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct hard_limiter
{
    double process(double sample) noexcept;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// gardner_ted                                                      //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct gardner_ted
{
    gardner_ted() = default;
    gardner_ted(double samples_per_bit, double alpha);

    bool process(double soft) noexcept;
    void reset() noexcept;

private:
    double phase_ = 0;
    double freq_ = 0;
    double freq_nominal_ = 0;
    double alpha_ = 0;
    double mid_soft_ = 0;
    double prev_soft_ = 0;
    double prev_sample_ = 0;
    double decision_soft_ = 0;
    bool mid_captured_ = false;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// truncated_fir_bandpass                                           //
//                                                                  //
// FIR bandpass filter using a rectangular (untapered) window.      //
// Delivers a sharp passband edge at the cost of -13 dB sidelobes,  //
// which is a good trade-off for AFSK, where isolating the mark and //
// space tones matters more than smooth spectral rolloff.           //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct truncated_fir_bandpass
{
    truncated_fir_bandpass() = default;
    truncated_fir_bandpass(double f_mark, double f_space, int bitrate, int sample_rate, double prefilter_baud, double filter_sym_lengths);

    double process(double sample) noexcept;
    void reset() noexcept;

private:
    static constexpr int max_taps = 512;
    double coeffs_[max_taps] = {};
    double buffer_[max_taps] = {};
    int taps_ = 0;
    int pos_ = 0;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// quad_sinc_filter                                                 //
//                                                                  //
// 4-channel truncated sinc (brick-wall) lowpass filter.            //
// Shares one coefficient set across mark I/Q + space I/Q.          //
// bandwidth = cutoff as fraction of symbol rate.                   //
// width = filter length in symbol periods.                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct sinc_filter_result
{
    double mark_i;
    double mark_q;
    double space_i;
    double space_q;
};

struct quad_sinc_filter
{
    quad_sinc_filter() = default;
    quad_sinc_filter(double bandwidth, double width, double samples_per_bit);

    sinc_filter_result process(double in_mark_i, double in_mark_q, double in_space_i, double in_space_q) noexcept;
    void reset() noexcept;

private:
    static constexpr int max_taps = 256;
    double coeffs_[max_taps] = {};
    double buf0_[max_taps] = {};
    double buf1_[max_taps] = {};
    double buf2_[max_taps] = {};
    double buf3_[max_taps] = {};
    int taps_ = 0;
    int pos_ = 0;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// iq_mixer                                                         //
//                                                                  //
// Dual-tone I/Q mixer for mark and space frequencies.              //
// Outputs 4 channels: mark_I, mark_Q, space_I, space_Q.           //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct iq_mix_result
{
    double mark_i;
    double mark_q;
    double space_i;
    double space_q;
};

struct iq_mixer
{
    iq_mixer() = default;
    iq_mixer(double f_mark, double f_space, int sample_rate);

    iq_mix_result process(double sample) noexcept;
    void reset() noexcept;

private:
    double phase_mark_ = 0;
    double phase_space_ = 0;
    double phase_inc_mark_ = 0;
    double phase_inc_space_ = 0;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// dfb_agc                                                          //
//                                                                  //
// Decision-feedback AGC. Tracks mark and space reference levels    //
// independently using the sign of the previous soft decision.      //
// Asymmetric rates: mark tracks faster than space.                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct agc_result
{
    double mark_norm;
    double space_norm;
};

struct dfb_agc
{
    dfb_agc() = default;
    dfb_agc(double alpha_mark, double alpha_space);

    // Normalize mark/space amplitudes. Caller provides last decision for feedback.
    agc_result process(double mark_amp, double space_amp, double last_soft) noexcept;
    void reset() noexcept;

private:
    double mark_ref_ = 0.001;
    double space_ref_ = 0.001;
    double alpha_mark_ = 0.008;
    double alpha_space_ = 0.005;
};

// **************************************************************** //
//                                                                  //
//                                                                  //
// sinc_corr_afsk_demodulator                                       //
//                                                                  //
// Correlator AFSK 1200 demodulator.                                //
// Composed from modular blocks:                                    //
//                                                                  //
//   BPF -> I/Q mixer -> quad LPF -> sqrt -> DFB-AGC -> TED         //
//                                                                  //
//                                                                  //
// **************************************************************** //

struct sinc_corr_afsk_demodulator
{
    sinc_corr_afsk_demodulator() = default;
    sinc_corr_afsk_demodulator(double f_mark, double f_space, int bitrate, int sample_rate,
                               double prefilter_baud, double filter_sym_lengths,
                               double sinc_bw, double sinc_rw,
                               double dfb_alpha_mark, double dfb_alpha_space,
                               double pll_alpha);

    bool try_demodulate(double sample, uint8_t& bit) noexcept;
    bool try_demodulate(double sample, demod_result& result) noexcept;
    void reset() noexcept;

private:
    truncated_fir_bandpass bpf_;
    iq_mixer mixer_;
    quad_sinc_filter lpf_;
    dfb_agc agc_;
    gardner_ted ted_;
    double last_soft_ = 0;
    double ref_amplitude_ = 1.0;
};

LIBMODEM_NAMESPACE_END
