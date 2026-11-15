// **************************************************************** //
// libmodem - APRS modem                                            //
// Version 0.1.0                                                    //
// https://github.com/iontodirel/libmodem                           //
// Copyright (c) 2025 Ion Todirel                                   //
// **************************************************************** //
//
// demodulator.cpp
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

#include "demodulator.h"

#include <cmath>
#include <cstring>
#include <algorithm>

LIBMODEM_NAMESPACE_BEGIN

// **************************************************************** //
//                                                                  //
//                                                                  //
// hard_limiter                                                     //
//                                                                  //
//                                                                  //
// **************************************************************** //

double hard_limiter::process(double sample) noexcept
{
    // Clips audio to +/-1 (sign function). Removes all amplitude
    // information, leaving only zero-crossing frequency content.

    return (sample >= 0) ? 1.0 : -1.0;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// gardner_ted                                                      //
//                                                                  //
//                                                                  //
// **************************************************************** //

gardner_ted::gardner_ted(double samples_per_bit, double alpha)
{
    freq_ = 1.0 / samples_per_bit;
    freq_nominal_ = freq_;
    alpha_ = alpha;
}

bool gardner_ted::process(double soft) noexcept
{
    // NCO (Numerically Controlled Oscillator): advance symbol phase accumulator
    // by the nominal fractional rate (1/samples_per_bit). phase_ wraps [0, 1)
    // where 0 = start of symbol, 0.5 = mid-symbol, 1.0 = decision point.
    phase_ += freq_;

    // Capture mid-bit soft sample — Gardner TED requires the sample halfway
    // between the previous and current decision points to compute its error.
    if (!mid_captured_ && phase_ >= 0.5)
    {
        mid_soft_ = soft;
        mid_captured_ = true;
    }

    // Not at the decision point yet — keep waiting for NCO to cross 1.0.
    if (phase_ < 1.0)
    {
        prev_sample_ = soft;
        return false;
    }

    // Decision point reached: latch the current sample as the bit decision.
    decision_soft_ = soft;
    phase_ -= 1.0;
    mid_captured_ = false;

    // Sign (slicer output): symbol decisions — +1 for mark, -1 for space.
    // Compared to detect a bit transition where Gardner error is meaningful.
    double prev_sign = (prev_soft_ > 0) ? 1.0 : -1.0;
    double curr_sign = (soft > 0) ? 1.0 : -1.0;

    // TED (Timing Error Detector) — Gardner algorithm:
    //
    //   error = mid_sample × (prev_sign − curr_sign)
    //
    // Only non-zero on bit transitions; sign indicates whether we sampled
    // early (+) or late (−) relative to the true bit boundary. Works on
    // the mid-symbol sample precisely because it straddles the transition.
    if (prev_sign != curr_sign)
    {
        double error = mid_soft_ * (prev_sign - curr_sign);

        // Normalize error by symbol amplitude so gain stays consistent
        // across signal levels (decouples TED from AGC / fading).
        double norm = std::max(std::abs(prev_soft_), std::abs(soft));

        if (norm > 0)
        {
            error /= norm;
        }

        // LF (Loop Filter): first-order integrator on phase. alpha_ controls
        // loop bandwidth / tracking aggressiveness. Pushes the NCO phase
        // toward the true symbol boundary based on TED error.
        phase_ += alpha_ * error;
    }

    // Guard: clamp NCO frequency to ±5% of nominal to prevent runaway
    // if TED produces outliers (noise, weak signals, burst errors).
    double max_dev = freq_nominal_ * 0.05;
    freq_ = std::max(freq_nominal_ - max_dev, std::min(freq_nominal_ + max_dev, freq_));

    prev_soft_ = soft;
    prev_sample_ = soft;

    return true;
}

void gardner_ted::reset() noexcept
{
    phase_ = 0;
    mid_soft_ = 0;
    prev_soft_ = 0;
    prev_sample_ = 0;
    decision_soft_ = 0;
    mid_captured_ = false;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// truncated_fir_bandpass                                           //
//                                                                  //
//                                                                  //
// **************************************************************** //

truncated_fir_bandpass::truncated_fir_bandpass(double f_mark, double f_space, int bitrate, int sample_rate, double prefilter_baud, double filter_sym_lengths)
{
    constexpr double pi = 3.14159265358979323846;
    constexpr double two_pi = 2.0 * pi;

    double samples_per_bit = static_cast<double>(sample_rate) / static_cast<double>(bitrate);

    taps_ = static_cast<int>(filter_sym_lengths * samples_per_bit) | 1;
    if (taps_ > max_taps)
    {
        taps_ = max_taps - 1;
    }
    if (taps_ < 3)
    {
        taps_ = 3;
    }

    int center = (taps_ - 1) / 2;
    double f_lo = f_mark - prefilter_baud * bitrate;
    double f_hi = f_space + prefilter_baud * bitrate;
    double f_center = (f_mark + f_space) / 2.0;

    for (int i = 0; i < taps_; i++)
    {
        double n = static_cast<double>(i - center);
        if (std::abs(n) < 0.5)
        {
            coeffs_[i] = 2.0 * (f_hi - f_lo) / sample_rate;
        }
        else
        {
            coeffs_[i] = (std::sin(two_pi * f_hi * n / sample_rate) - std::sin(two_pi * f_lo * n / sample_rate)) / (pi * n);
        }
    }

    double sum_re = 0;
    for (int i = 0; i < taps_; i++)
    {
        sum_re += coeffs_[i] * std::cos(two_pi * f_center / sample_rate * (i - center));
    }

    if (std::abs(sum_re) > 1e-15)
    {
        for (int i = 0; i < taps_; i++)
        {
            coeffs_[i] /= std::abs(sum_re);
        }
    }
}

double truncated_fir_bandpass::process(double sample) noexcept
{
    buffer_[pos_] = sample;

    double out = 0;

    int i = pos_;
    for (int k = 0; k < taps_; k++)
    {
        out += coeffs_[k] * buffer_[i];
        if (--i < 0)
        {
            i = taps_ - 1;
        }
    }

    pos_ = (pos_ + 1) % taps_;

    return out;
}

void truncated_fir_bandpass::reset() noexcept
{
    std::memset(buffer_, 0, sizeof(buffer_));
    pos_ = 0;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// iq_mixer                                                         //
//                                                                  //
//                                                                  //
// **************************************************************** //

iq_mixer::iq_mixer(double f_mark, double f_space, int sample_rate)
{
    constexpr double two_pi = 2.0 * 3.14159265358979323846;

    phase_inc_mark_ = two_pi * f_mark / sample_rate;
    phase_inc_space_ = two_pi * f_space / sample_rate;
}

iq_mix_result iq_mixer::process(double sample) noexcept
{
    constexpr double two_pi = 2.0 * 3.14159265358979323846;

    iq_mix_result r;

    r.mark_i = sample * std::cos(phase_mark_);
    r.mark_q = sample * std::sin(phase_mark_);
    r.space_i = sample * std::cos(phase_space_);
    r.space_q = sample * std::sin(phase_space_);

    phase_mark_ += phase_inc_mark_;
    phase_space_ += phase_inc_space_;

    if (phase_mark_ >= two_pi)
    {
        phase_mark_ -= two_pi;
    }

    if (phase_space_ >= two_pi)
    {
        phase_space_ -= two_pi;
    }

    return r;
}

void iq_mixer::reset() noexcept
{
    phase_mark_ = 0;
    phase_space_ = 0;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// quad_sinc_filter                                                 //
//                                                                  //
//                                                                  //
// **************************************************************** //

quad_sinc_filter::quad_sinc_filter(double bandwidth, double width, double samples_per_bit)
{
    // Truncated sinc lowpass filter for 4 I/Q channels.
    //
    // The sinc function sin(πx)/(πx) is the impulse response of an
    // ideal brick-wall lowpass filter. Truncating it to a finite
    // number of taps creates a practical FIR with sharp cutoff and
    // -13dB sidelobes (no windowing applied).
    //
    // bandwidth: cutoff as fraction of symbol rate (e.g., 0.80 = 80%)
    // width:     filter length in symbol periods (e.g., 2.0 symbols)
    //
    // Cutoff in normalized frequency: f_c = bandwidth / samples_per_bit
    // Coefficients: h[n] = sin(2π·f_c·n) / (π·n), normalized for
    // unity DC gain.

    constexpr double pi = 3.14159265358979323846;

    taps_ = static_cast<int>(width * samples_per_bit) | 1;
    if (taps_ > max_taps)
    {
        taps_ = max_taps - 1;
    }
    if (taps_ < 3)
    {
        taps_ = 3;
    }

    int center = (taps_ - 1) / 2;
    double cutoff = bandwidth / samples_per_bit;
    double sum = 0;

    for (int j = 0; j < taps_; j++)
    {
        double t = static_cast<double>(j - center);

        if (std::abs(t) < 0.5)
        {
            coeffs_[j] = 2.0 * cutoff;
        }
        else
        {
            coeffs_[j] = std::sin(2.0 * pi * cutoff * t) / (pi * t);
        }

        sum += coeffs_[j];
    }

    for (int j = 0; j < taps_; j++)
    {
        coeffs_[j] /= sum;
    }
}

sinc_filter_result quad_sinc_filter::process(double in_mark_i, double in_mark_q, double in_space_i, double in_space_q) noexcept
{
    buf0_[pos_] = in_mark_i;
    buf1_[pos_] = in_mark_q;
    buf2_[pos_] = in_space_i;
    buf3_[pos_] = in_space_q;

    double mark_i_out = 0;
    double mark_q_out = 0;
    double space_i_out = 0;
    double space_q_out = 0;

    int i = pos_;

    for (int k = 0; k < taps_; k++)
    {
        double c = coeffs_[k];

        mark_i_out += c * buf0_[i];
        mark_q_out += c * buf1_[i];
        space_i_out += c * buf2_[i];
        space_q_out += c * buf3_[i];

        if (--i < 0)
        {
            i = taps_ - 1;
        }
    }

    pos_ = (pos_ + 1) % taps_;

    return {mark_i_out, mark_q_out, space_i_out, space_q_out};
}

void quad_sinc_filter::reset() noexcept
{
    std::memset(buf0_, 0, sizeof(buf0_));
    std::memset(buf1_, 0, sizeof(buf1_));
    std::memset(buf2_, 0, sizeof(buf2_));
    std::memset(buf3_, 0, sizeof(buf3_));
    pos_ = 0;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// dfb_agc                                                          //
//                                                                  //
//                                                                  //
// **************************************************************** //

dfb_agc::dfb_agc(double alpha_mark, double alpha_space) : alpha_mark_(alpha_mark), alpha_space_(alpha_space)
{
}

agc_result dfb_agc::process(double mark_amp, double space_amp, double last_soft) noexcept
{
    // Update the reference level for whichever tone was dominant in the last decision.
    // Only the active tone's reference tracks, the other holds its value.
    // This prevents cross-talk: mark AGC doesn't chase space energy and vice versa.
    if (last_soft > 0)
    {
        // Last decision was mark-dominant: update mark reference with exponential average
        mark_ref_ = alpha_mark_ * mark_amp + (1.0 - alpha_mark_) * mark_ref_;
    }
    else
    {
        // Last decision was space-dominant: update space reference
        space_ref_ = alpha_space_ * space_amp + (1.0 - alpha_space_) * space_ref_;
    }

    // Return normalized amplitudes: each tone divided by its own tracked reference
    return {mark_amp / mark_ref_, space_amp / space_ref_};
}

void dfb_agc::reset() noexcept
{
    mark_ref_ = 0.001;
    space_ref_ = 0.001;
}

// **************************************************************** //
//                                                                  //
//                                                                  //
// sinc_corr_afsk_demodulator                                       //
//                                                                  //
//                                                                  //
// **************************************************************** //

sinc_corr_afsk_demodulator::sinc_corr_afsk_demodulator(
    double f_mark, double f_space, int bitrate, int sample_rate,
    double prefilter_baud, double filter_sym_lengths,
    double sinc_bw, double sinc_rw,
    double dfb_alpha_mark, double dfb_alpha_space,
    double pll_alpha)
{
    double samples_per_bit = static_cast<double>(sample_rate) / static_cast<double>(bitrate);

    bpf_ = truncated_fir_bandpass(f_mark, f_space, bitrate, sample_rate, prefilter_baud, filter_sym_lengths);
    mixer_ = iq_mixer(f_mark, f_space, sample_rate);
    lpf_ = quad_sinc_filter(sinc_bw, sinc_rw, samples_per_bit);
    agc_ = dfb_agc(dfb_alpha_mark, dfb_alpha_space);
    ted_ = gardner_ted(samples_per_bit, pll_alpha);
}

bool sinc_corr_afsk_demodulator::try_demodulate(double sample, demod_result& result) noexcept
{
    // Bandpass filter: isolate mark (1200Hz) and space (2200Hz) tones
    double filtered = bpf_.process(sample);

    // I/Q mixing: multiply by mark and space oscillators to produce 4 baseband channels
    auto mix = mixer_.process(filtered);

    // Matched filter: truncated sinc LPF extracts envelope from each I/Q channel
    auto lp = lpf_.process(mix.mark_i, mix.mark_q, mix.space_i, mix.space_q);

    // Magnitude: sqrt(I² + Q²) for mark and space tone energy
    double mark_amp = std::sqrt(lp.mark_i * lp.mark_i + lp.mark_q * lp.mark_q);
    double space_amp = std::sqrt(lp.space_i * lp.space_i + lp.space_q * lp.space_q);

    // Decision-feedback AGC: normalize mark/space using tracked reference levels
    auto agc_out = agc_.process(mark_amp, space_amp, last_soft_);

    double soft = agc_out.mark_norm - agc_out.space_norm;

    last_soft_ = soft;

    // Symbol timing recovery: Gardner TED fires when a bit boundary is detected
    if (ted_.process(soft))
    {
        result.bit = (soft > 0) ? 1 : 0;

        // Confidence: ratio of soft decision magnitude to running average
        double abs_soft = std::abs(soft);
        ref_amplitude_ = 0.99 * ref_amplitude_ + 0.01 * abs_soft;
        result.confidence = (ref_amplitude_ > 1e-12) ? abs_soft / ref_amplitude_ : 0.0;

        return true;
    }

    return false;
}

bool sinc_corr_afsk_demodulator::try_demodulate(double sample, uint8_t& bit) noexcept
{
    demod_result r;

    if (!try_demodulate(sample, r))
    {
        return false;
    }

    bit = r.bit;

    return true;
}

void sinc_corr_afsk_demodulator::reset() noexcept
{
    bpf_.reset();
    mixer_.reset();
    lpf_.reset();
    agc_.reset();
    ted_.reset();
    last_soft_ = 0;
    ref_amplitude_ = 1.0;
}

LIBMODEM_NAMESPACE_END
