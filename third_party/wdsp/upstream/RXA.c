/*  RXA.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013, 2014, 2015, 2016, 2023, 2025, 2026 Warren Pratt, NR0V

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

The author can be reached by email at  

warren@wpratt.com

*/

#include "comm.h"

struct _rxa rxa[MAX_CHANNELS];

void create_rxa (int channel)
{
	rxa[channel].mode = RXA_LSB;
	rxa[channel].inbuff  = (double *) malloc0 (1 * ch[channel].dsp_insize  * sizeof (complex));
	rxa[channel].outbuff = (double *) malloc0 (1 * ch[channel].dsp_outsize * sizeof (complex));
	rxa[channel].midbuff = (double *) malloc0 (2 * ch[channel].dsp_size    * sizeof (complex));

	// shift to select a slice of spectrum
	rxa[channel].shift.p = create_shift (
		1,												// run
		ch[channel].dsp_insize,							// input buffer size
		rxa[channel].inbuff,							// pointer to input buffer
		rxa[channel].inbuff,							// pointer to output buffer
		ch[channel].in_rate,							// samplerate
		0.0);											// amount to shift (Hz)

	// input resampler
	rxa[channel].rsmpin.p = create_HBResampler (
		ch[channel].in_rate,
		ch[channel].dsp_rate,
		ch[channel].dsp_insize,
		(complex_t *)(void*)rxa[channel].inbuff,
		(complex_t*)(void*)rxa[channel].midbuff);

	// signal generator
	rxa[channel].gen0.p = create_gen (
		0,												// run
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// input buffer
		rxa[channel].midbuff,							// output buffer
		ch[channel].dsp_rate,							// sample rate
		2);												// mode

	// adc (input) meter
	rxa[channel].adcmeter.p = create_meter (
		1,												// run
		0,												// optional pointer to another 'run'
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to buffer
		ch[channel].dsp_rate,							// samplerate
		0.100,											// averaging time constant
		0.100,											// peak decay time constant
		rxa[channel].meter,								// result vector
		rxa[channel].pmtupdate,							// locks for meter access
		RXA_ADC_AV,										// index for average value
		RXA_ADC_PK,										// index for peak value
		-1,												// index for gain value
		0);												// pointer for gain computation

	// notch database
	rxa[channel].ndb.p = create_notchdb (
		0,												// master run for all nbp's
		1024);											// max number of notches

	// notched bandpass
	rxa[channel].nbp0.p = create_nbp (
		1,												// run, always runs
		0,												// run the notches
		0,												// position
		ch[channel].dsp_size,							// buffer size
		max(2048, ch[channel].dsp_size),				// number of coefficients
		0,												// minimum phase flag
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		-4150.0,										// lower filter frequency
		-150.0,											// upper filter frequency
		ch[channel].dsp_rate,							// sample rate
		0,												// wintype
		1.0,											// gain
		1,												// auto-increase notch width
		1025,											// max number of passbands
		&rxa[channel].ndb.p);							// addr of database pointer

	// bandpass for snba
	rxa[channel].bpsnba.p = create_bpsnba (
		0,												// bpsnba run flag
		0,												// run the notches
		0,												// position
		ch[channel].dsp_size,							// size
		max(2048, ch[channel].dsp_size),				// number of filter coefficients
		0,												// minimum phase flag
		rxa[channel].midbuff,							// input buffer
		rxa[channel].midbuff,							// output buffer
		ch[channel].dsp_rate,							// samplerate
		+ 250.0,										// abs value of cutoff nearest zero
		+ 5700.0,										// abs value of cutoff farthest zero
		- 5700.0,										// current low frequency
		- 250.0,										// current high frequency
		0,												// wintype
		1.0,											// gain
		1,												// auto-increase notch width
		1025,											// max number of passbands
		&rxa[channel].ndb.p);							// addr of database pointer

	// send spectrum display
	rxa[channel].sender.p = create_sender (
		channel == 0,									// run
		0,												// flag
		0,												// mode
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to input buffer
		0,												// arg0 <- disp
		1,												// arg1 <- ss
		0,												// arg2 <- LO
		0);												// arg3 <- NOT USED

	// S-meter
	rxa[channel].smeter.p = create_meter (
		1,												// run
		0,												// optional pointer to another 'run'
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to buffer
		ch[channel].dsp_rate,							// samplerate
		0.100,											// averaging time constant
		0.100,											// peak decay time constant
		rxa[channel].meter,								// result vector
		rxa[channel].pmtupdate,							// locks for meter access
		RXA_S_AV,										// index for average value
		RXA_S_PK,										// index for peak value
		-1,												// index for gain value
		0);												// pointer for gain computation

	// AM squelch
	rxa[channel].amsq.p = create_amsq (
		0,												// run
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to signal input buffer used by xamsq
		rxa[channel].midbuff,							// pointer to signal output buffer used by xamsq
		rxa[channel].midbuff,							// pointer to trigger buffer that xamsqcap will capture
		ch[channel].dsp_rate,							// sample rate
		0.010,											// time constant for averaging signal level
		0.070,											// signal up transition time
		0.070,											// signal down transition time
		0.009,											// signal level to initiate tail
		0.010,											// signal level to initiate unmute
		0.000,											// minimum tail length
		1.500,											// maximum tail length
		0.0);											// muted gain

	// AM demod
	rxa[channel].amd.p = create_amd (
		0,												// run - OFF by default
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		0,												// mode:  0->AM, 1->SAM
		1,												// levelfade:  0->OFF, 1->ON
		0,												// sideband mode:  0->OFF
		ch[channel].dsp_rate,							// sample rate
		-2000.0,										// minimum lock frequency
		+2000.0,										// maximum lock frequency
		1.0,											// zeta
		250.0,											// omegaN
		0.02,											// tauR
		1.4);											// tauI

	// WBFM demod
	rxa[channel].wbfm.p = create_wbfm(
		0,												// run
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ch[channel].dsp_rate);							// sample rate

	// FM demod
	rxa[channel].fmd.p = create_fmd (
		0,												// run
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ch[channel].dsp_rate,							// sample rate
		5000.0,											// deviation
		300.0,											// f_low
		3000.0,											// f_high
		-8000.0,										// fmin
		+8000.0,										// fmax
		1.0,											// zeta
		20000.0,										// omegaN
		0.02,											// tau - for dc removal
		0.5,											// audio gain
		1,												// run tone filter
		254.1,											// ctcss frequency
		max(2048, ch[channel].dsp_size),				// # coefs for de-emphasis filter
		0,												// min phase flag for de-emphasis filter
		max(2048, ch[channel].dsp_size),				// # coefs for audio cutoff filter
		0);												// min phase flag for audio cutoff filter

	// FM squelch
	rxa[channel].fmsq.p = create_fmsq (
		0,												// run
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input signal buffer
		rxa[channel].midbuff,							// pointer to output signal buffer
		rxa[channel].fmd.p->audio,						// pointer to trigger buffer
		ch[channel].dsp_rate,							// sample rate
		5000.0,											// cutoff freq for noise filter (Hz)
		&rxa[channel].fmd.p->pllpole,					// pointer to pole frequency of the fmd pll (Hz)
		0.100,											// delay time after channel flush
		0.001,											// tau for noise averaging
		0.100,											// tau for long noise averaging
		0.050,											// signal up transition time
		0.010,											// signal down transition time
		0.750,											// noise level to initiate tail
		0.562,											// noise level to initiate unmute
		0.000,											// minimum tail time
		1.200,											// maximum tail time
		max(2048, ch[channel].dsp_size),				// number of coefficients for noise filter
		0);												// minimum phase flag

	// snba
	rxa[channel].snba.p = create_snba (
		0,												// run
		rxa[channel].midbuff,							// input buffer
		rxa[channel].midbuff,							// output buffer
		ch[channel].dsp_rate,							// input / output sample rate
		12000,											// internal processing sample rate
		ch[channel].dsp_size,							// buffer size
		4,												// overlap factor to use
		256,											// frame size to use; sized for 12K rate
		64,												// asize
		2,												// npasses
		8.0,											// k1
		20.0,											// k2
		10,												// b
		2,												// pre
		2,												// post
		0.5,											// pmultmin
		200.0,											// output resampler low cutoff
		5400.0);										// output resampler high cutoff

	// EQ
	{
	double default_F[11] = {0.0,  32.0,  63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
	//double default_G[11] = {0.0, -12.0, -12.0, -12.0,  -1.0,  +1.0,   +4.0,   +9.0,  +12.0,  -10.0,   -10.0};
	double default_G[11] =   {0.0,   0.0,   0.0,   0.0,   0.0,   0.0,    0.0,    0.0,    0.0,    0.0,     0.0};
	rxa[channel].eqp.p = create_eqp (
		0,												// run - OFF by default
		ch[channel].dsp_size,							// buffer size
		max(16384, ch[channel].dsp_size),				// number of filter coefficients
		1,												// minimum phase flag
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		10,												// number of frequencies
		default_F,										// frequency vector
		default_G,										// gain vector
		0,												// cutoff mode
		2,												// wintype
		ch[channel].dsp_rate);							// sample rate
	}

	// ANF
	rxa[channel].anf.p = create_anf (
		0,												// run - OFF by default
		0,												// position
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ANF_DLINE_SIZE,									// dline_size
		64,												// taps
		16,												// delay
		0.0001,											// two_mu
		0.1,											// gamma
		1.0,											// lidx
		0.0,											// lidx_min
		200.0,											// lidx_max
		6.25e-12,										// ngamma
		6.25e-10,										// den_mult
		1.0,											// lincr
		3.0);											// ldecr

	// ANR
	rxa[channel].anr.p = create_anr (
		0,												// run - OFF by default
		0,												// position
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ANR_DLINE_SIZE,									// dline_size
		64,												// taps
		16,												// delay			
		0.0001,											// two_mu	
		0.1,											// gamma
		120.0,											// lidx
		120.0,											// lidx_min
		200.0,											// lidx_max
		0.001,											// ngamma
		6.25e-10,										// den_mult
		1.0,											// lincr
		3.0);											// ldecr

	
	// EMNR
	rxa[channel].emnr.p = create_emnr (
		0,												// run
		0,												// position
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// input buffer
		rxa[channel].midbuff,							// output buffer
		4096,											// FFT size
		4,												// overlap
		ch[channel].dsp_rate,							// samplerate
		0,												// window type
		1.0,											// gain
		2,												// gain method
		0,												// npe_method
		1);												// ae_run

	// AGC
	rxa[channel].agc.p = create_wcpagc (
		1,												// run
		3,												// mode
		1,												// peakmode = envelope
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ch[channel].dsp_size,							// buffer size
		ch[channel].dsp_rate,							// sample rate
		0.001,											// tau_attack
		0.250,											// tau_decay
		4,												// n_tau
		10000.0,										// max_gain
		1.5,											// var_gain
		1000.0,											// fixed_gain
		1.0,											// max_input
		1.0,											// out_target
		0.250,											// tau_fast_backaverage
		0.005,											// tau_fast_decay
		5.0,											// pop_ratio
		1,												// hang_enable
		0.500,											// tau_hang_backmult
		0.250,											// hangtime
		0.250,											// hang_thresh
		0.100);											// tau_hang_decay

	// agc gain meter
	rxa[channel].agcmeter.p = create_meter (
		1,												// run
		0,												// optional pointer to another 'run'
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to buffer
		ch[channel].dsp_rate,							// samplerate
		0.100,											// averaging time constant
		0.100,											// peak decay time constant
		rxa[channel].meter,								// result vector
		rxa[channel].pmtupdate,							// locks for meter access
		RXA_AGC_AV,										// index for average value
		RXA_AGC_PK,										// index for peak value
		RXA_AGC_GAIN,									// index for gain value
		&rxa[channel].agc.p->gain);						// pointer for gain computation

	// bandpass filter
	rxa[channel].bp1.p = create_bandpass (
		1,												// run - used only with ( AM || ANF || ANR || EMNR)
		0,												// position
		ch[channel].dsp_size,							// buffer size
		max(2048, ch[channel].dsp_size),				// number of coefficients
		0,												// flag for minimum phase
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		-4150.0,										// lower filter frequency
		-150.0,											// upper filter frequency
		ch[channel].dsp_rate,							// sample rate
		1,												// wintype
		1.0);											// gain

	// pull phase & scope display data
	rxa[channel].sip1.p = create_siphon (
		1,												// run - needed only for phase display
		0,												// position
		0,												// mode
		0,												// disp
		ch[channel].dsp_size,							// size of input buffer
		rxa[channel].midbuff,							// input buffer
		4096,											// number of samples to store
		4096,											// fft size for spectrum
		0);												// specmode

	// carrier block
	rxa[channel].cbl.p = create_cbl (
		0,												// run - needed only if set to ON
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		0,												// mode
		ch[channel].dsp_rate,							// sample rate
		0.02);											// tau

	// double-pole CW filter
	rxa[channel].doublepole.p = create_doublepole (
		0,												// run
		0,												// position
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		600.0,											// center frequency
		100.0,											// bandwidth
		ch[channel].dsp_rate,							// sample rate
		2.0,											// gain
		2 );											// mode

	// matched CW filter
	rxa[channel].matched.p = create_matched (
		0,												// run
		0,												// position
		ch[channel].dsp_size,							// buffer size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		600.0,											// center frequency
		100.0,											// bandwidth
		ch[channel].dsp_rate,							// sample rate
		2.0,											// gain
		2 );											// mode

	// gaussian peaking filter
	rxa[channel].gaussian.p = create_gaussian (
		0,												// run
		0,												// position
		ch[channel].dsp_size,							// buffer size
		0,												// number of coefficients
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		600.0,											// center frequency
		100.0,											// bandwidth
		ch[channel].dsp_rate,							// sample rate
		2.0,											// gain
		3.0,											// nsigma
		2 );											// mode

	// bi-quad peaking filter
	rxa[channel].speak.p = create_speak (
		0,												// run
		ch[channel].dsp_size,							// buffer size,
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ch[channel].dsp_rate,							// sample rate
		600.0,											// center frequency
		100.0,											// bandwidth
		2.0,											// gain
		4,												// number of stages
		1);												// design

	// multiple peak filter
	{
		int def_enable[2] = {1, 1};
		double def_freq[2] = {2125.0, 2295.0};
		double def_bw[2] = {75.0, 75.0};
		double def_gain[2] = {1.0, 1.0};
		rxa[channel].mpeak.p = create_mpeak (
			0,											// run
			ch[channel].dsp_size,						// size
			rxa[channel].midbuff,						// pointer to input buffer
			rxa[channel].midbuff,						// pointer to output buffer
			ch[channel].dsp_rate,						// sample rate
			2,											// number of peaking filters
			def_enable,									// enable vector
			def_freq,									// frequency vector
			def_bw,										// bandwidth vector
			def_gain,									// gain vector
			4 );										// number of stages
	}

	// apf_shadow
	rxa[channel].apfshadow.p = create_apfshadow (
		0,												// selection
		0,												// run
		600.0,											// center frequency
		100.0,											// bandwidth
		2.0 );											// gain

	// syllabic squelch
	rxa[channel].ssql.p = create_ssql (
		0,												// run
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		ch[channel].dsp_rate,							// sample rate
		0.070,											// signal up transition time
		0.070,											// signal down transition time
		0.0,											// muted gain
		0.1,											// mute time-constant
		0.1,											// unmute time-constant
		0.08,											// window threshold
		0.8197,											// trigger threshold
		2400,											// ring size for f_to_v converter
		2000.0 );										// max freq for f_to_v converter									

	// patchpanel
	rxa[channel].panel.p = create_panel (
		channel,										// channel number
		1,												// run
		ch[channel].dsp_size,							// size
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].midbuff,							// pointer to output buffer
		4.0,											// gain1
		1.0,											// gain2I
		1.0,											// gain2Q
		3,												// 3 for I and Q
		0);												// no copy

	// resample
	rxa[channel].rsmpout.p = create_resample (
		0,												// run - will be turned ON below if needed
		ch[channel].dsp_size,							// input buffer size			 
		rxa[channel].midbuff,							// pointer to input buffer
		rxa[channel].outbuff,							// pointer to output buffer
		ch[channel].dsp_rate,							// input sample rate
		ch[channel].out_rate,							// output sample rate
		0.0,											// select cutoff automatically
		0,												// select ncoef automatically
		1.0);											// gain

	// turn OFF / ON resamplers as needed
	RXAResCheck (channel);
}

void destroy_rxa (int channel)
{
	destroy_resample (rxa[channel].rsmpout.p);
	destroy_panel (rxa[channel].panel.p);
	destroy_ssql (rxa[channel].ssql.p);
	destroy_apfshadow(rxa[channel].apfshadow.p);
	destroy_mpeak (rxa[channel].mpeak.p);
	destroy_speak (rxa[channel].speak.p);
	destroy_gaussian (rxa[channel].gaussian.p);
	destroy_matched (rxa[channel].matched.p);
	destroy_doublepole (rxa[channel].doublepole.p);
	destroy_cbl (rxa[channel].cbl.p);
	destroy_siphon (rxa[channel].sip1.p);
	destroy_bandpass (rxa[channel].bp1.p);
	destroy_meter (rxa[channel].agcmeter.p);
	destroy_wcpagc (rxa[channel].agc.p);
	destroy_emnr (rxa[channel].emnr.p);
	destroy_anr (rxa[channel].anr.p);
	destroy_anf (rxa[channel].anf.p);
	destroy_eqp (rxa[channel].eqp.p);
	destroy_snba (rxa[channel].snba.p);
	destroy_fmsq (rxa[channel].fmsq.p);
	destroy_fmd (rxa[channel].fmd.p);
	destroy_wbfm(rxa[channel].wbfm.p);
	destroy_amd (rxa[channel].amd.p);
	destroy_amsq (rxa[channel].amsq.p);
	destroy_meter (rxa[channel].smeter.p);
	destroy_sender (rxa[channel].sender.p);
	destroy_bpsnba (rxa[channel].bpsnba.p);
	destroy_nbp (rxa[channel].nbp0.p);
	destroy_notchdb (rxa[channel].ndb.p);
	destroy_meter (rxa[channel].adcmeter.p);
	destroy_gen (rxa[channel].gen0.p);
	//destroy_resample (rxa[channel].rsmpin.p);
	destroy_HBResampler(rxa[channel].rsmpin.p);
	destroy_shift (rxa[channel].shift.p);
	_aligned_free (rxa[channel].midbuff);
	_aligned_free (rxa[channel].outbuff);
	_aligned_free (rxa[channel].inbuff);
}

void flush_rxa (int channel)
{
	memset (rxa[channel].inbuff,  0, 1 * ch[channel].dsp_insize  * sizeof (complex));
	memset (rxa[channel].outbuff, 0, 1 * ch[channel].dsp_outsize * sizeof (complex));
	memset (rxa[channel].midbuff, 0, 2 * ch[channel].dsp_size    * sizeof (complex));
	flush_shift (rxa[channel].shift.p);
	//flush_resample (rxa[channel].rsmpin.p);
	flush_HBResampler(rxa[channel].rsmpin.p);
	flush_gen (rxa[channel].gen0.p);
	flush_meter (rxa[channel].adcmeter.p);
	flush_nbp (rxa[channel].nbp0.p);
	flush_bpsnba (rxa[channel].bpsnba.p);
	flush_sender (rxa[channel].sender.p);
	flush_meter (rxa[channel].smeter.p);
	flush_amsq (rxa[channel].amsq.p);
	flush_amd (rxa[channel].amd.p);
	flush_wbfm(rxa[channel].wbfm.p);
	flush_fmd (rxa[channel].fmd.p);
	flush_fmsq (rxa[channel].fmsq.p);
	flush_snba (rxa[channel].snba.p);
	flush_eqp (rxa[channel].eqp.p);
	flush_anf (rxa[channel].anf.p);
	flush_anr (rxa[channel].anr.p);
	flush_emnr (rxa[channel].emnr.p);
	flush_wcpagc (rxa[channel].agc.p);
	flush_meter (rxa[channel].agcmeter.p);
	flush_bandpass (rxa[channel].bp1.p);
	flush_siphon (rxa[channel].sip1.p);
	flush_cbl (rxa[channel].cbl.p);
	flush_doublepole (rxa[channel].doublepole.p);
	flush_matched (rxa[channel].matched.p);
	flush_gaussian (rxa[channel].gaussian.p);
	flush_speak (rxa[channel].speak.p);
	flush_mpeak (rxa[channel].mpeak.p);
	flush_ssql (rxa[channel].ssql.p);
	flush_panel (rxa[channel].panel.p);
	flush_resample (rxa[channel].rsmpout.p);
}

void xrxa (int channel)
{
	xshift (rxa[channel].shift.p);
	//xresample (rxa[channel].rsmpin.p);
	xHBResampler(rxa[channel].rsmpin.p);
	xgen (rxa[channel].gen0.p);
	xmeter (rxa[channel].adcmeter.p);
	xbpsnbain (rxa[channel].bpsnba.p, 0);
	xnbp (rxa[channel].nbp0.p, 0);
	xmeter (rxa[channel].smeter.p);
	xsender (rxa[channel].sender.p);
	xamsqcap (rxa[channel].amsq.p);
	xbpsnbaout (rxa[channel].bpsnba.p, 0);
	xamd (rxa[channel].amd.p);
	xwbfm(rxa[channel].wbfm.p);
	xfmd (rxa[channel].fmd.p);
	xfmsq (rxa[channel].fmsq.p);
	xbpsnbain (rxa[channel].bpsnba.p, 1);
	xbpsnbaout (rxa[channel].bpsnba.p, 1);
	xsnba (rxa[channel].snba.p);
	xeqp (rxa[channel].eqp.p);
	xanf (rxa[channel].anf.p, 0);
	xanr (rxa[channel].anr.p, 0);
	xemnr (rxa[channel].emnr.p, 0);
	xbandpass (rxa[channel].bp1.p, 0);
	xwcpagc (rxa[channel].agc.p);
	xanf (rxa[channel].anf.p, 1);
	xanr (rxa[channel].anr.p, 1);
	xemnr (rxa[channel].emnr.p, 1);
	xbandpass (rxa[channel].bp1.p, 1);
	xmeter (rxa[channel].agcmeter.p);
	xsiphon (rxa[channel].sip1.p, 0);
	xcbl (rxa[channel].cbl.p);
	xdoublepole (rxa[channel].doublepole.p, 0);
	xmatched (rxa[channel].matched.p, 0);
	xgaussian (rxa[channel].gaussian.p, 0);
	xspeak (rxa[channel].speak.p);
	xmpeak (rxa[channel].mpeak.p);
	xssql (rxa[channel].ssql.p);
	xpanel (rxa[channel].panel.p);
	xamsq (rxa[channel].amsq.p);
	xresample (rxa[channel].rsmpout.p);
}

void setInputSamplerate_rxa (int channel)
{
	// buffers
	_aligned_free (rxa[channel].inbuff);
	rxa[channel].inbuff = (double *)malloc0(1 * ch[channel].dsp_insize  * sizeof(complex));
	// shift
	setBuffers_shift (rxa[channel].shift.p, rxa[channel].inbuff, rxa[channel].inbuff);
	setSize_shift (rxa[channel].shift.p, ch[channel].dsp_insize);
	setSamplerate_shift (rxa[channel].shift.p, ch[channel].in_rate);
	// input resampler
	//setBuffers_resample (rxa[channel].rsmpin.p, rxa[channel].inbuff, rxa[channel].midbuff);
	setBuffers_HBResampler(rxa[channel].rsmpin.p, (complex_t*)(void*)rxa[channel].inbuff, (complex_t*)(void*)rxa[channel].midbuff);
	//setSize_resample (rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	setSize_HBResampler(rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	//setInRate_resample (rxa[channel].rsmpin.p, ch[channel].in_rate);
	setInRate_HBResampler(rxa[channel].rsmpin.p, ch[channel].in_rate);
	RXAResCheck (channel);
}

void setOutputSamplerate_rxa (int channel)
{
	// buffers
	_aligned_free (rxa[channel].outbuff);
	rxa[channel].outbuff = (double *)malloc0(1 * ch[channel].dsp_outsize * sizeof(complex));
	// output resampler
	setBuffers_resample (rxa[channel].rsmpout.p, rxa[channel].midbuff, rxa[channel].outbuff);
	setOutRate_resample (rxa[channel].rsmpout.p, ch[channel].out_rate);
	RXAResCheck (channel);
}

void setDSPSamplerate_rxa (int channel)
{
	// buffers
	_aligned_free (rxa[channel].inbuff);
	rxa[channel].inbuff = (double *)malloc0(1 * ch[channel].dsp_insize  * sizeof(complex));
	_aligned_free (rxa[channel].outbuff);
	rxa[channel].outbuff = (double *)malloc0(1 * ch[channel].dsp_outsize * sizeof(complex));
	// shift
	setBuffers_shift (rxa[channel].shift.p, rxa[channel].inbuff, rxa[channel].inbuff);
	setSize_shift (rxa[channel].shift.p, ch[channel].dsp_insize);
	// input resampler
	//setBuffers_resample (rxa[channel].rsmpin.p, rxa[channel].inbuff, rxa[channel].midbuff);
	setBuffers_HBResampler(rxa[channel].rsmpin.p, (complex_t*)(void*)rxa[channel].inbuff, (complex_t*)(void*)rxa[channel].midbuff);
	//setSize_resample (rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	setSize_HBResampler(rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	//setOutRate_resample (rxa[channel].rsmpin.p, ch[channel].dsp_rate);
	setOutRate_HBResampler(rxa[channel].rsmpin.p, ch[channel].dsp_rate);
	// dsp_rate blocks
	setSamplerate_gen (rxa[channel].gen0.p, ch[channel].dsp_rate);
	setSamplerate_meter (rxa[channel].adcmeter.p, ch[channel].dsp_rate);
	setSamplerate_nbp (rxa[channel].nbp0.p, ch[channel].dsp_rate);
	setSamplerate_bpsnba (rxa[channel].bpsnba.p, ch[channel].dsp_rate);
	setSamplerate_meter (rxa[channel].smeter.p, ch[channel].dsp_rate);
	setSamplerate_sender (rxa[channel].sender.p, ch[channel].dsp_rate);
	setSamplerate_amsq (rxa[channel].amsq.p, ch[channel].dsp_rate);
	setSamplerate_amd (rxa[channel].amd.p, ch[channel].dsp_rate);
	setSamplerate_wbfm(rxa[channel].wbfm.p, ch[channel].dsp_rate);
	setSamplerate_fmd (rxa[channel].fmd.p, ch[channel].dsp_rate);
	setBuffers_fmsq (rxa[channel].fmsq.p, rxa[channel].midbuff, rxa[channel].midbuff, rxa[channel].fmd.p->audio);
	setSamplerate_fmsq (rxa[channel].fmsq.p, ch[channel].dsp_rate);
	setSamplerate_snba (rxa[channel].snba.p, ch[channel].dsp_rate);
	setSamplerate_eqp (rxa[channel].eqp.p, ch[channel].dsp_rate);
	setSamplerate_anf (rxa[channel].anf.p, ch[channel].dsp_rate);
	setSamplerate_anr (rxa[channel].anr.p, ch[channel].dsp_rate);
	setSamplerate_emnr (rxa[channel].emnr.p, ch[channel].dsp_rate);
	setSamplerate_bandpass (rxa[channel].bp1.p, ch[channel].dsp_rate);
	setSamplerate_wcpagc (rxa[channel].agc.p, ch[channel].dsp_rate);
	setSamplerate_meter (rxa[channel].agcmeter.p, ch[channel].dsp_rate);
	setSamplerate_siphon (rxa[channel].sip1.p, ch[channel].dsp_rate);
	setSamplerate_cbl (rxa[channel].cbl.p, ch[channel].dsp_rate);
	setSamplerate_doublepole (rxa[channel].doublepole.p, ch[channel].dsp_rate);
	setSamplerate_matched (rxa[channel].matched.p, ch[channel].dsp_rate);
	setSamplerate_gaussian (rxa[channel].gaussian.p, ch[channel].dsp_rate);
	setSamplerate_speak (rxa[channel].speak.p, ch[channel].dsp_rate);
	setSamplerate_mpeak (rxa[channel].mpeak.p, ch[channel].dsp_rate);
	setSamplerate_ssql (rxa[channel].ssql.p, ch[channel].dsp_rate);
	setSamplerate_panel (rxa[channel].panel.p, ch[channel].dsp_rate);
	// output resampler
	setBuffers_resample (rxa[channel].rsmpout.p, rxa[channel].midbuff, rxa[channel].outbuff);
	setInRate_resample (rxa[channel].rsmpout.p, ch[channel].dsp_rate);
	RXAResCheck (channel);
}

void setDSPBuffsize_rxa (int channel)
{
	// buffers
	_aligned_free(rxa[channel].inbuff);
	rxa[channel].inbuff = (double *)malloc0(1 * ch[channel].dsp_insize  * sizeof(complex));
	_aligned_free (rxa[channel].midbuff);
	rxa[channel].midbuff = (double *)malloc0(2 * ch[channel].dsp_size * sizeof(complex));
	_aligned_free (rxa[channel].outbuff);
	rxa[channel].outbuff = (double *)malloc0(1 * ch[channel].dsp_outsize * sizeof(complex));
	// shift
	setBuffers_shift (rxa[channel].shift.p, rxa[channel].inbuff, rxa[channel].inbuff);
	setSize_shift (rxa[channel].shift.p, ch[channel].dsp_insize);
	// input resampler
	//setBuffers_resample (rxa[channel].rsmpin.p, rxa[channel].inbuff, rxa[channel].midbuff);
	setBuffers_HBResampler(rxa[channel].rsmpin.p, (complex_t*)(void*)rxa[channel].inbuff, (complex_t*)(void*)rxa[channel].midbuff);
	//setSize_resample (rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	setSize_HBResampler(rxa[channel].rsmpin.p, ch[channel].dsp_insize);
	// dsp_size blocks
	setBuffers_gen (rxa[channel].gen0.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_gen (rxa[channel].gen0.p, ch[channel].dsp_size);
	setBuffers_meter (rxa[channel].adcmeter.p, rxa[channel].midbuff);
	setSize_meter (rxa[channel].adcmeter.p, ch[channel].dsp_size);
	setBuffers_nbp (rxa[channel].nbp0.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_nbp (rxa[channel].nbp0.p, ch[channel].dsp_size);
	setBuffers_bpsnba (rxa[channel].bpsnba.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_bpsnba (rxa[channel].bpsnba.p, ch[channel].dsp_size);
	setBuffers_meter (rxa[channel].smeter.p, rxa[channel].midbuff);
	setSize_meter (rxa[channel].smeter.p, ch[channel].dsp_size);
	setBuffers_sender (rxa[channel].sender.p, rxa[channel].midbuff);
	setSize_sender (rxa[channel].sender.p, ch[channel].dsp_size);
	setBuffers_amsq (rxa[channel].amsq.p, rxa[channel].midbuff, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_amsq (rxa[channel].amsq.p, ch[channel].dsp_size);
	setBuffers_amd (rxa[channel].amd.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_amd (rxa[channel].amd.p, ch[channel].dsp_size);
	setBuffers_wbfm(rxa[channel].wbfm.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_wbfm(rxa[channel].wbfm.p, ch[channel].dsp_size);
	setBuffers_fmd (rxa[channel].fmd.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_fmd (rxa[channel].fmd.p, ch[channel].dsp_size);
	setBuffers_fmsq (rxa[channel].fmsq.p, rxa[channel].midbuff, rxa[channel].midbuff, rxa[channel].fmd.p->audio);
	setSize_fmsq (rxa[channel].fmsq.p, ch[channel].dsp_size);
	setBuffers_snba (rxa[channel].snba.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_snba (rxa[channel].snba.p, ch[channel].dsp_size);
	setBuffers_eqp (rxa[channel].eqp.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_eqp (rxa[channel].eqp.p, ch[channel].dsp_size);
	setBuffers_anf (rxa[channel].anf.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_anf (rxa[channel].anf.p, ch[channel].dsp_size);
	setBuffers_anr (rxa[channel].anr.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_anr (rxa[channel].anr.p, ch[channel].dsp_size);
	setBuffers_emnr (rxa[channel].emnr.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_emnr (rxa[channel].emnr.p, ch[channel].dsp_size);
	setBuffers_bandpass (rxa[channel].bp1.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_bandpass (rxa[channel].bp1.p, ch[channel].dsp_size);
	setBuffers_wcpagc (rxa[channel].agc.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_wcpagc (rxa[channel].agc.p, ch[channel].dsp_size);
	setBuffers_meter (rxa[channel].agcmeter.p, rxa[channel].midbuff);
	setSize_meter (rxa[channel].agcmeter.p, ch[channel].dsp_size);
	setBuffers_siphon (rxa[channel].sip1.p, rxa[channel].midbuff);
	setSize_siphon (rxa[channel].sip1.p, ch[channel].dsp_size);
	setBuffers_cbl (rxa[channel].cbl.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_cbl (rxa[channel].cbl.p, ch[channel].dsp_size);
	setBuffers_doublepole (rxa[channel].doublepole.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_doublepole (rxa[channel].doublepole.p, ch[channel].dsp_size);
	setBuffers_matched (rxa[channel].matched.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_matched (rxa[channel].matched.p, ch[channel].dsp_size);
	setBuffers_gaussian (rxa[channel].gaussian.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_gaussian (rxa[channel].gaussian.p, ch[channel].dsp_size);
	setBuffers_speak (rxa[channel].speak.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_speak (rxa[channel].speak.p, ch[channel].dsp_size);
	setBuffers_mpeak (rxa[channel].mpeak.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_mpeak (rxa[channel].mpeak.p, ch[channel].dsp_size);
	setBuffers_ssql (rxa[channel].ssql.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_ssql (rxa[channel].ssql.p, ch[channel].dsp_size);
	setBuffers_panel (rxa[channel].panel.p, rxa[channel].midbuff, rxa[channel].midbuff);
	setSize_panel (rxa[channel].panel.p, ch[channel].dsp_size);
	// output resampler
	setBuffers_resample (rxa[channel].rsmpout.p, rxa[channel].midbuff, rxa[channel].outbuff);
	setSize_resample (rxa[channel].rsmpout.p, ch[channel].dsp_size);
}

/********************************************************************************************************
*																										*
*										RXA Mode & Filter Controls										*
*																										*
********************************************************************************************************/

PORT
void SetRXAMode (int channel, int mode)
{
	if (rxa[channel].mode != mode)
	{
		int amd_run = (mode == RXA_AM) || (mode == RXA_SAM);
		RXAbpsnbaCheck (channel, mode, rxa[channel].ndb.p->master_run);
		RXAbp1Check (channel, amd_run, rxa[channel].snba.p->run, rxa[channel].emnr.p->run, 
			rxa[channel].anf.p->run, rxa[channel].anr.p->run);
		EnterCriticalSection (&ch[channel].csDSP);
		rxa[channel].mode = mode;
		rxa[channel].amd.p->run   = 0;
		rxa[channel].wbfm.p->run  = 0;
		rxa[channel].fmd.p->run   = 0;
		rxa[channel].agc.p->run   = 1;
		rxa[channel].nbp0.p->run  = 1;
		rxa[channel].panel.p->run = 1;
		switch (mode)
		{
		case RXA_AM:
			rxa[channel].amd.p->run  = 1;
			rxa[channel].amd.p->mode = 0;
			break;
		case RXA_SAM:
			rxa[channel].amd.p->run  = 1;
			rxa[channel].amd.p->mode = 1;
			break;
		case RXA_DSB:
		
			break;
		case RXA_FM:
			rxa[channel].fmd.p->run  = 1;
			rxa[channel].agc.p->run  = 0;
			break;
		case RXA_WBFM:
			rxa[channel].wbfm.p->run  = 1;
			rxa[channel].agc.p->run   = 0;
			rxa[channel].nbp0.p->run  = 0;
			rxa[channel].panel.p->run = 0;
			break;
		default:

			break;
		}
		RXAbp1Set (channel);
		RXAbpsnbaSet (channel);
		LeaveCriticalSection (&ch[channel].csDSP);
	}
}

void RXAResCheck (int channel)
{	
	// HBResampler (rsmpin) turns OFF/ON automatically for this situation.
	RESAMPLE b = rxa[channel].rsmpout.p;
	if (ch[channel].dsp_rate != ch[channel].out_rate)	b->run = 1;
	else												b->run = 0;
}

void RXAbp1Check (int channel, int amd_run, int snba_run, 
	int emnr_run, int anf_run, int anr_run)
{
	BANDPASS a = rxa[channel].bp1.p;
	double gain;
	if (amd_run  ||
		snba_run ||
		emnr_run ||
		anf_run  ||
		anr_run)	gain = 2.0;
	else			gain = 1.0;
	if (a->gain != gain)
		setGain_bandpass (a, gain, 0);
}

void RXAbp1Set (int channel)
{
	BANDPASS a = rxa[channel].bp1.p;
	int old = a->run;
	if ((rxa[channel].amd.p->run  == 1) ||
		(rxa[channel].snba.p->run == 1) ||
		(rxa[channel].emnr.p->run == 1) ||
		(rxa[channel].anf.p->run  == 1) ||
		(rxa[channel].anr.p->run  == 1))	a->run = 1;
	else									a->run = 0;
	if (!old && a->run) flush_bandpass (a);
	setUpdate_fircore (a->p);
}

void RXAbpsnbaCheck (int channel, int mode, int notch_run)
{
	// for BPSNBA: set run, position, freqs, run_notches
	// call this upon change in RXA_mode, snba_run, notch_master_run
	BPSNBA a = rxa[channel].bpsnba.p;
	double f_low = 0.0, f_high = 0.0;
	int run_notches = 0;
	switch (mode)
	{
		case RXA_LSB:
		case RXA_CWL:
		case RXA_DIGL:
			f_low  = -a->abs_high_freq;
			f_high = -a->abs_low_freq;
			run_notches = notch_run;
			break;
		case RXA_USB:
		case RXA_CWU:
		case RXA_DIGU:
			f_low  = +a->abs_low_freq;
			f_high = +a->abs_high_freq;
			run_notches = notch_run;
			break;
		case RXA_AM:
		case RXA_SAM:
		case RXA_DSB:
			f_low  = +a->abs_low_freq;
			f_high = +a->abs_high_freq;
			run_notches = 0;
			break;
		case RXA_FM:
		case RXA_WBFM:
			f_low  = +a->abs_low_freq;
			f_high = +a->abs_high_freq;
			run_notches = 0;
			break;
		case RXA_DRM:
		case RXA_SPEC:
		
			break;
	}
	// 'run' and 'position' are examined at run time; no filter changes required.
	// Recalculate filter if frequencies OR 'run_notches' changed.
	if ((a->f_low       != f_low      ) ||
		(a->f_high      != f_high     ) ||
		(a->run_notches != run_notches))	
	{
		a->f_low  = f_low;
		a->f_high = f_high;
		a->run_notches = run_notches;
		// f_low, f_high, run_notches are needed for the filter recalculation
		recalc_bpsnba_filter (a, 0);
	}
}

void RXAbpsnbaSet (int channel)
{
	// for BPSNBA: set run, position, freqs, run_notches
	// call this upon change in RXA_mode, snba_run, notch_master_run
	BPSNBA a = rxa[channel].bpsnba.p;
	switch (rxa[channel].mode)
	{
		case RXA_LSB:
		case RXA_CWL:
		case RXA_DIGL:
			a->run = rxa[channel].snba.p->run;
			a->position = 0;
			break;
		case RXA_USB:
		case RXA_CWU:
		case RXA_DIGU:
			a->run = rxa[channel].snba.p->run;
			a->position = 0;
			break;
		case RXA_AM:
		case RXA_SAM:
		case RXA_DSB:
			a->run = rxa[channel].snba.p->run;
			a->position = 1;
			break;
		case RXA_FM:
			a->run = rxa[channel].snba.p->run;
			a->position = 1;
			break;
		case RXA_DRM:
		case RXA_SPEC:
			a->run = 0;
			break;
	}
	setUpdate_fircore (a->bpsnba->p);
}

/********************************************************************************************************
*																										*
*												Collectives												*
*																										*
********************************************************************************************************/

PORT
void RXASetPassband (int channel, double f_low, double f_high)
{
	SetRXABandpassFreqs			(channel, f_low, f_high);
	SetRXASNBAOutputBandwidth	(channel, f_low, f_high);
	RXANBPSetFreqs				(channel, f_low, f_high);
}

PORT
void RXASetNC (int channel, int nc)
{
	int oldstate = SetChannelState (channel, 0, 1);
	RXANBPSetNC					(channel, nc);
	RXABPSNBASetNC				(channel, nc);
	SetRXABandpassNC			(channel, nc);
	SetRXAFMSQNC				(channel, nc);
	SetRXAFMNCde				(channel, nc);
	SetRXAFMNCaud				(channel, nc);
	SetChannelState (channel, oldstate, 0);
}

PORT
void RXASetMP (int channel, int mp)
{
	RXANBPSetMP					(channel, mp);
	RXABPSNBASetMP				(channel, mp);
	SetRXABandpassMP			(channel, mp);
	SetRXAFMSQMP				(channel, mp);
	SetRXAFMMPde				(channel, mp);
	SetRXAFMMPaud				(channel, mp);
}