/*  wdsp.h

The public interface of WDSP: every entry point exported from
the library, as documented in the WDSP Reference Manual.

GENERATED FILE - do not edit.  Produced by gen_wdsp_h.py, which
scans the library sources for PORT-decorated functions, so that
this header cannot drift from what the DLL actually exports.

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2026 Warren Pratt, NR0V

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

#ifndef _wdsp_h
#define _wdsp_h

#ifdef __cplusplus
extern "C" {
#endif

/*  Callers link against the import library on Windows and against
	the shared object elsewhere, so the decoration differs.  Define
	WDSP_BUILD when compiling the library itself.                */
#ifndef WDSP_API
#if defined(_WIN32) || defined(_WIN64)
#ifdef WDSP_BUILD
#define WDSP_API __declspec (dllexport)
#else
#define WDSP_API __declspec (dllimport)
#endif
#else
#define WDSP_API
#endif
#endif

/*  ---- types used by the interface ----------------------------------

	These are declared here so that this header stands alone.  Nothing
	below requires any other WDSP header, and none of the library's
	internal names are brought into scope.                            */

#include <stddef.h>

/*  Calling-convention keywords.  These appear in function-pointer
	parameters and are Windows-specific; elsewhere they are nothing. */
#if !defined(_WIN32) && !defined(_WIN64)
#ifndef __stdcall
#define __stdcall
#endif
#ifndef __cdecl
#define __cdecl
#endif
#endif

/*  Opaque handles.  The objects they point at are private to the
	library; callers only pass these values back in.               */
#ifndef WDSP_HANDLES_DECLARED
#define WDSP_HANDLES_DECLARED
typedef struct _anb* ANB;
typedef struct _eer* EER;
typedef struct _gain* GAIN;
typedef struct _nob* NOB;
typedef struct _resample* RESAMPLE;
#endif

/*  Sample types.  THESE MUST MATCH THE BINARY.  They are set when
	the library is compiled, and a mismatch does not fail to build -
	it silently passes the wrong sized value.  The concrete types
	below were read from comm.h when this header was generated.    */
typedef float  INREAL;
typedef float  OUTREAL;
typedef double dINREAL;
typedef float  dOUTREAL;

/*  Windows handle types, declared opaquely so this header needs no
	<windows.h>.  They are pointers the caller only passes back in,
	so void* is ABI-identical, and a real handle converts to void*
	implicitly in both C and C++.                                 */

/*  Constants from comm.h that appear in the interface.          */
#ifndef WDSP_CONSTANTS_DECLARED
#define WDSP_CONSTANTS_DECLARED
#define dMAX_M     1
#endif

/*  A Windows DWORD is 32-bit.  `unsigned long` is 32-bit on Windows
	but 64-bit on Linux, so the obvious substitution would change
	the ABI.  Use a fixed width instead.                          */
#ifndef WDSP_DWORD_DECLARED
#define WDSP_DWORD_DECLARED
typedef unsigned int WDSP_DWORD;
#endif


/*  ---- RXA.c ------------------------------------------------------------- */

WDSP_API void RXASetMP (int channel, int mp);
WDSP_API void RXASetNC (int channel, int nc);
WDSP_API void RXASetPassband (int channel, double f_low, double f_high);
WDSP_API void SetRXAMode (int channel, int mode);

/*  ---- TXA.c ------------------------------------------------------------- */

WDSP_API void SetTXABandpassFreqs (int channel, double f_low, double f_high);
WDSP_API void SetTXAFMAFFilter (int channel, double low, double high);
WDSP_API void SetTXAMode (int channel, int mode);
WDSP_API void TXASetMP (int channel, int mp);
WDSP_API void TXASetNC (int channel, int nc);

/*  ---- amd.c ------------------------------------------------------------- */

WDSP_API void SetRXAAMDFadeLevel (int channel, int levelfade);
WDSP_API void SetRXAAMDRun (int channel, int run);
WDSP_API void SetRXAAMDSBMode (int channel, int sbmode);

/*  ---- ammod.c ----------------------------------------------------------- */

WDSP_API void SetTXAAMCarrierLevel (int channel, double c_level);

/*  ---- amsq.c ------------------------------------------------------------ */

WDSP_API void SetRXAAMSQMaxTail (int channel, double tail);
WDSP_API void SetRXAAMSQRun (int channel, int run);
WDSP_API void SetRXAAMSQThreshold (int channel, double threshold);
WDSP_API void SetTXAAMSQMutedGain (int channel, double dBlevel);
WDSP_API void SetTXAAMSQRun (int channel, int run);
WDSP_API void SetTXAAMSQThreshold (int channel, double threshold);

/*  ---- analyzer.c -------------------------------------------------------- */

WDSP_API void CloseBuffer (int disp, int ss, int LO);
WDSP_API void DestroyAnalyzer (int disp);
WDSP_API double GetDetectMaxBin (int disp);
WDSP_API double GetDisplayENB (int disp);
WDSP_API void GetPixels (int disp, int pixout, dOUTREAL *pix, int *flag);
WDSP_API void OpenBuffer (int disp, int ss, int LO, void **Ipointer, void **Qpointer);
WDSP_API void ResetPixelBuffers (int disp);
WDSP_API void SetAnalyzer (int disp, int n_pixout, int n_fft, int typ, int *flp, int sz, int bf_sz, int win_type, double pi, int ovrlp, int clp, double fscLin, double fscHin, int n_pix, int n_stch, int calset, double fmin, double fmax, int max_w);
WDSP_API void SetCalibration (int disp, int set_num, int n_points, double (*cal)[dMAX_M+1]);
WDSP_API void SetDisplayAvBackmult (int disp, int pixout, double mult);
WDSP_API void SetDisplayAverageMode (int disp, int pixout, int mode);
WDSP_API void SetDisplayDetectorMode (int disp, int pixout, int mode);
WDSP_API void SetDisplayNormOneHz (int disp, int pixout, int norm);
WDSP_API void SetDisplayNumAverage (int disp, int pixout, int num);
WDSP_API void SetDisplaySampleRate (int disp, int rate);
WDSP_API void SetupDetectMaxBin (int run, int disp, int ss, int LO, double rate, double fLow, double fHigh, double tau, int frame_rate);
WDSP_API void SnapSpectrum (int disp, int ss, int LO, double *snap_buff);
WDSP_API void SnapSpectrumTimeout (int disp, int ss, int LO, double* snap_buff, WDSP_DWORD timeout, int* flag);
WDSP_API void Spectrum (int disp, int ss, int LO, dINREAL* pI, dINREAL* pQ);
WDSP_API void Spectrum0 (int run, int disp, int ss, int LO, double* pbuff);
WDSP_API void Spectrum2 (int run, int disp, int ss, int LO, dINREAL* pbuff);
WDSP_API void XCreateAnalyzer (int disp, int *success, int m_size, int m_num_fft, int m_stitch, char *app_data_path);

/*  ---- anf.c ------------------------------------------------------------- */

WDSP_API void SetRXAANFDelay (int channel, int delay);
WDSP_API void SetRXAANFGain (int channel, double gain);
WDSP_API void SetRXAANFLeakage (int channel, double leakage);
WDSP_API void SetRXAANFPosition (int channel, int position);
WDSP_API void SetRXAANFRun (int channel, int run);
WDSP_API void SetRXAANFTaps (int channel, int taps);
WDSP_API void SetRXAANFVals (int channel, int taps, int delay, double gain, double leakage);

/*  ---- anr.c ------------------------------------------------------------- */

WDSP_API void SetRXAANRDelay (int channel, int delay);
WDSP_API void SetRXAANRGain (int channel, double gain);
WDSP_API void SetRXAANRLeakage (int channel, double leakage);
WDSP_API void SetRXAANRPosition (int channel, int position);
WDSP_API void SetRXAANRRun (int channel, int run);
WDSP_API void SetRXAANRTaps (int channel, int taps);
WDSP_API void SetRXAANRVals (int channel, int taps, int delay, double gain, double leakage);

/*  ---- apfshadow.c ------------------------------------------------------- */

WDSP_API void SetRXASPCWBandwidth (int channel, double bandwidth);
WDSP_API void SetRXASPCWFreq (int channel, double f_center);
WDSP_API void SetRXASPCWGain (int channel, double gain);
WDSP_API void SetRXASPCWRun (int channel, int run);
WDSP_API void SetRXASPCWSelection (int channel, int selection);

/*  ---- bandpass.c -------------------------------------------------------- */

WDSP_API void SetRXABandpassFreqs (int channel, double f_low, double f_high);
WDSP_API void SetRXABandpassMP (int channel, int mp);
WDSP_API void SetRXABandpassNC (int channel, int nc);
WDSP_API void SetRXABandpassRun (int channel, int run);
WDSP_API void SetRXABandpassWindow (int channel, int wintype);
WDSP_API void SetTXABandpassMP (int channel, int mp);
WDSP_API void SetTXABandpassNC (int channel, int nc);
WDSP_API void SetTXABandpassRun (int channel, int run);
WDSP_API void SetTXABandpassWindow (int channel, int wintype);

/*  ---- calcc.c ----------------------------------------------------------- */

WDSP_API void GetPSDisp (int channel, double* x, double* ym, double* yc, double* ys, double* xm_cor, double* ym_cor, double* xa_cor, double* ya_cor, int* nsamps_out, int* cpts_out, double* phs_ref_deg_out);
WDSP_API void GetPSHWPeak (int channel, double* peak);
WDSP_API void GetPSInfo (int channel, int *info);
WDSP_API void GetPSMaxTX (int channel, double* maxtx);
WDSP_API void PSRestoreCorr (int channel, char* filename);
WDSP_API void PSSaveCorr (int channel, char* filename);
WDSP_API void SetPSAutomode (int channel, int automode);
WDSP_API void SetPSControl (int channel, int reset, int mancal, int automode, int turnon);
WDSP_API void SetPSFeedbackRate (int channel, int rate);
WDSP_API void SetPSHWPeak (int channel, double peak);
WDSP_API void SetPSLoopDelay (int channel, double delay);
WDSP_API void SetPSMancal (int channel, int mancal);
WDSP_API void SetPSMox (int channel, int mox);
WDSP_API void SetPSMoxDelay (int channel, double delay);
WDSP_API void SetPSReset (int channel, int reset);
WDSP_API void SetPSRunCal (int channel, int run);
WDSP_API double SetPSTXDelay (int channel, double delay);
WDSP_API void SetPSTurnon (int channel, int turnon);
WDSP_API void pscc (int channel, int size, double* tx, double* rx);

/*  ---- cblock.c ---------------------------------------------------------- */

WDSP_API void SetRXACBLRun (int channel, int setit);

/*  ---- cfcomp.c ---------------------------------------------------------- */

WDSP_API void GetTXACFCOMPCompDraw (int channel, double* X, double* Y);
WDSP_API void GetTXACFCOMPDisplayCompression (int channel, double* comp_values, int* ready);
WDSP_API void GetTXACFCOMPPeqDraw (int channel, double* X, double* Y);
WDSP_API void SetTXACFCOMPCompCurve (int channel, int deg, int r, int umethod);
WDSP_API void SetTXACFCOMPCompWeights (int channel, int nfreq, double* weights);
WDSP_API void SetTXACFCOMPEprofile (int channel, int nfreqs, double* F, double* E);
WDSP_API void SetTXACFCOMPGprofile (int channel, int nfreqs, double* F, double* G);
WDSP_API void SetTXACFCOMPPeqCurve (int channel, int deg, int r, int umethod);
WDSP_API void SetTXACFCOMPPeqRun (int channel, int run);
WDSP_API void SetTXACFCOMPPeqWeights (int channel, int nfreq, double* weights);
WDSP_API void SetTXACFCOMPPosition (int channel, int pos);
WDSP_API void SetTXACFCOMPPrePeq (int channel, double prepeq);
WDSP_API void SetTXACFCOMPPrecomp (int channel, double precomp);
WDSP_API void SetTXACFCOMPRun (int channel, int run);
WDSP_API void SetTXACFCOMPprofile (int channel, int nfreqs, double* F, double* G, double *E);

/*  ---- cfir.c ------------------------------------------------------------ */

WDSP_API void SetTXACFIRCurve (int channel, int DD, int R, int Pairs, int runrate, int cicrate, double cutoff, int xtype, double xbw);
WDSP_API void SetTXACFIRNC (int channel, int nc);
WDSP_API void SetTXACFIRRun (int channel, int run);

/*  ---- channel.c --------------------------------------------------------- */

WDSP_API void CloseChannel (int channel);
WDSP_API void OpenChannel (int channel, int in_size, int dsp_size, int input_samplerate, int dsp_rate, int output_samplerate, int type, int state, double tdelayup, double tslewup, double tdelaydown, double tslewdown, int bfo);
WDSP_API void SetAllRates (int channel, int in_rate, int dsp_rate, int out_rate);
WDSP_API int SetChannelState (int channel, int state, int dmode);
WDSP_API void SetChannelTDelayDown (int channel, double time);
WDSP_API void SetChannelTDelayUp (int channel, double time);
WDSP_API void SetChannelTSlewDown (int channel, double time);
WDSP_API void SetChannelTSlewUp (int channel, double time);
WDSP_API void SetDSPBuffsize (int channel, int dsp_size);
WDSP_API void SetDSPSamplerate (int channel, int dsp_rate);
WDSP_API void SetInputBuffsize (int channel, int in_size);
WDSP_API void SetInputSamplerate (int channel, int in_rate);
WDSP_API void SetOutputSamplerate (int channel, int out_rate);
WDSP_API void SetType (int channel, int type);

/*  ---- compress.c -------------------------------------------------------- */

WDSP_API void SetTXACompressorGain (int channel, double gain);
WDSP_API void SetTXACompressorRun (int channel, int run);

/*  ---- dexp.c ------------------------------------------------------------ */

WDSP_API void GetDEXPPeakSignal (int id, double* peak);
WDSP_API void SendAntiVOXData (int id, int nsamples, double* data);
WDSP_API void SendCBPushDexpVox (int id, void (__stdcall *pushvox)(int id, int active));
WDSP_API void SetAntiVOXDetectorTau (int id, double tau);
WDSP_API void SetAntiVOXGain (int id, double gain);
WDSP_API void SetAntiVOXRate (int id, double rate);
WDSP_API void SetAntiVOXRun (int id, int run);
WDSP_API void SetAntiVOXSize (int id, int size);
WDSP_API void SetDEXPAttackThreshold (int id, double thresh);
WDSP_API void SetDEXPAttackTime (int id, double time);
WDSP_API void SetDEXPAudioDelay (int id, double delay);
WDSP_API void SetDEXPDetectorTau (int id, double tau);
WDSP_API void SetDEXPExpansionRatio (int id, double ratio);
WDSP_API void SetDEXPFilterTaps (int id, int taps);
WDSP_API void SetDEXPHighCut (int id, double highcut);
WDSP_API void SetDEXPHoldTime (int id, double time);
WDSP_API void SetDEXPHysteresisRatio (int id, double ratio);
WDSP_API void SetDEXPIOBuffers (int id, double* in, double* out);
WDSP_API void SetDEXPLowCut (int id, double lowcut);
WDSP_API void SetDEXPRate (int id, double rate);
WDSP_API void SetDEXPReleaseTime (int id, double time);
WDSP_API void SetDEXPRun (int id, int run);
WDSP_API void SetDEXPRunAudioDelay (int id, int run);
WDSP_API void SetDEXPRunSideChannelFilter (int id, int run);
WDSP_API void SetDEXPRunVox (int id, int run);
WDSP_API void SetDEXPSize (int id, int size);
WDSP_API void SetDEXPWindowType (int id, int type);
WDSP_API void create_dexp (int id, int run_dexp, int size, double* in, double* out, int rate, double dettau, double tattack, double tdecay, double thold, double exp_ratio, double hyst_ratio, double attack_thresh, int nc, int wtype, double lowcut, double highcut, int run_filt, int run_vox, int run_audelay, double audelay, void (__stdcall *pushvox)(int id, int active), int antivox_run, int antivox_size, int antivox_rate, double antivox_gain, double antivox_tau);
WDSP_API void destroy_dexp (int id);
WDSP_API void flush_dexp (int id);
WDSP_API void xdexp (int id);

/*  ---- div.c ------------------------------------------------------------- */

WDSP_API void SetEXTDIVBuffsize (int id, int size);
WDSP_API void SetEXTDIVNr (int id, int nr);
WDSP_API void SetEXTDIVOutput (int id, int output);
WDSP_API void SetEXTDIVRotate (int id, int nr, double *Irotate, double *Qrotate);
WDSP_API void SetEXTDIVRun (int id, int run);
WDSP_API void create_divEXT (int id, int run, int nr, int size);
WDSP_API void destroy_divEXT (int id);
WDSP_API void flush_divEXT (int id);
WDSP_API void xdivEXT (int id, int nsamples, double **in, double *out);
WDSP_API void xdivEXTF (int id, int size, float **input, float *Iout, float *Qout);

/*  ---- doublepole.c ------------------------------------------------------ */

WDSP_API void SetRXADoublepoleFreqs (int channel, double f_center, double bandwidth);
WDSP_API void SetRXADoublepoleGain (int channel, double gain);
WDSP_API void SetRXADoublepoleRun (int channel, int run);

/*  ---- eer.c ------------------------------------------------------------- */

WDSP_API void SetEERAMIQ (int id, int amiq);
WDSP_API void SetEERMdelay (int id, double delay);
WDSP_API void SetEERMgain (int id, double gain);
WDSP_API void SetEERPdelay (int id, double delay);
WDSP_API void SetEERPgain (int id, double gain);
WDSP_API void SetEERRun (int id, int run);
WDSP_API void SetEERRunDelays (int id, int run);
WDSP_API void SetEERSamplerate (int id, int rate);
WDSP_API void SetEERSize (int id, int size);
WDSP_API EER create_eer (int run, int size, double* in, double* out, double* outM, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq);
WDSP_API void create_eerEXT (int id, int run, int size, int rate, double mgain, double pgain, int rundelays, double mdelay, double pdelay, int amiq);
WDSP_API void destroy_eer (EER a);
WDSP_API void destroy_eerEXT (int id);
WDSP_API void flush_eer (EER a);
WDSP_API void flush_eerEXT (int id);
WDSP_API void pSetEERAMIQ (EER a, int amiq);
WDSP_API void pSetEERMdelay (EER a, double delay);
WDSP_API void pSetEERMgain (EER a, double gain);
WDSP_API void pSetEERPdelay (EER a, double delay);
WDSP_API void pSetEERPgain (EER a, double gain);
WDSP_API void pSetEERRun (EER a, int run);
WDSP_API void pSetEERRunDelays (EER a, int run);
WDSP_API void pSetEERSamplerate (EER a, int rate);
WDSP_API void pSetEERSize (EER a, int size);
WDSP_API void xeer (EER a);
WDSP_API void xeerEXTF (int id, float* inI, float* inQ, float* outI, float* outQ, float* outMI, float* outMQ, int mox, int size);

/*  ---- emnr.c ------------------------------------------------------------ */

WDSP_API void SetRXAEMNRPosition (int channel, int position);
WDSP_API void SetRXAEMNRRun (int channel, int run);
WDSP_API void SetRXAEMNRaePsi (int channel, double psi);
WDSP_API void SetRXAEMNRaeRun (int channel, int run);
WDSP_API void SetRXAEMNRaeZetaThresh (int channel, double zetathresh);
WDSP_API void SetRXAEMNRgainMethod (int channel, int method);
WDSP_API void SetRXAEMNRnpeMethod (int channel, int method);
WDSP_API void SetRXAEMNRpost2Factor (int channel, double factor);
WDSP_API void SetRXAEMNRpost2Nlevel (int channel, double nlevel);
WDSP_API void SetRXAEMNRpost2Rate (int channel, double tc);
WDSP_API void SetRXAEMNRpost2Run (int channel, int run);
WDSP_API void SetRXAEMNRpost2Taper (int channel, int taper);
WDSP_API void SetRXAEMNRtrainT2 (int channel, double t2);
WDSP_API void SetRXAEMNRtrainZetaThresh (int channel, double thresh);

/*  ---- emph.c ------------------------------------------------------------ */

WDSP_API void SetTXAFMEmphMP (int channel, int mp);
WDSP_API void SetTXAFMEmphNC (int channel, int nc);
WDSP_API void SetTXAFMEmphPosition (int channel, int position);
WDSP_API void SetTXAFMPreEmphFreqs (int channel, double low, double high);

/*  ---- eq.c -------------------------------------------------------------- */

WDSP_API void GetRXAEQDraw (int channel, double* X, double* Y);
WDSP_API void GetTXAEQDraw (int channel, double* X, double* Y);
WDSP_API void SetRXAEQCtfmode (int channel, int mode);
WDSP_API void SetRXAEQCurve (int channel, int deg, int r, int umethod);
WDSP_API void SetRXAEQMP (int channel, int mp);
WDSP_API void SetRXAEQNC (int channel, int nc);
WDSP_API void SetRXAEQProfile (int channel, int nfreqs, double* F, double* G);
WDSP_API void SetRXAEQRun (int channel, int run);
WDSP_API void SetRXAEQWeights (int channel, int nfreq, double* weights);
WDSP_API void SetRXAEQWintype (int channel, int wintype);
WDSP_API void SetRXAGrphEQ (int channel, int *rxeq);
WDSP_API void SetRXAGrphEQ10 (int channel, int *rxeq);
WDSP_API void SetTXAEQCtfmode (int channel, int mode);
WDSP_API void SetTXAEQCurve (int channel, int deg, int r, int umethod);
WDSP_API void SetTXAEQMP (int channel, int mp);
WDSP_API void SetTXAEQNC (int channel, int nc);
WDSP_API void SetTXAEQProfile (int channel, int nfreqs, double* F, double* G);
WDSP_API void SetTXAEQRun (int channel, int run);
WDSP_API void SetTXAEQWeights (int channel, int nfreq, double* weights);
WDSP_API void SetTXAEQWintype (int channel, int wintype);
WDSP_API void SetTXAGrphEQ (int channel, int *txeq);
WDSP_API void SetTXAGrphEQ10 (int channel, int *txeq);

/*  ---- fmd.c ------------------------------------------------------------- */

WDSP_API void SetRXACTCSSFreq (int channel, double freq);
WDSP_API void SetRXACTCSSRun (int channel, int run);
WDSP_API void SetRXAFMAFFilter (int channel, double low, double high);
WDSP_API void SetRXAFMDeviation (int channel, double deviation);
WDSP_API void SetRXAFMLimGain (int channel, double gaindB);
WDSP_API void SetRXAFMLimRun (int channel, int run);
WDSP_API void SetRXAFMMPaud (int channel, int mp);
WDSP_API void SetRXAFMMPde (int channel, int mp);
WDSP_API void SetRXAFMNCaud (int channel, int nc);
WDSP_API void SetRXAFMNCde (int channel, int nc);

/*  ---- fmmod.c ----------------------------------------------------------- */

WDSP_API void SetTXACTCSSFreq (int channel, double freq);
WDSP_API void SetTXACTCSSRun (int channel, int run);
WDSP_API void SetTXAFMAFFreqs (int channel, double low, double high);
WDSP_API void SetTXAFMDeviation (int channel, double deviation);
WDSP_API void SetTXAFMMP (int channel, int mp);
WDSP_API void SetTXAFMNC (int channel, int nc);

/*  ---- fmsq.c ------------------------------------------------------------ */

WDSP_API void SetRXAFMSQMP (int channel, int mp);
WDSP_API void SetRXAFMSQNC (int channel, int nc);
WDSP_API void SetRXAFMSQRun (int channel, int run);
WDSP_API void SetRXAFMSQThreshold (int channel, double threshold);

/*  ---- gain.c ------------------------------------------------------------ */

WDSP_API GAIN create_gain (int run, int* prun, int size, double* in, double* out, double Igain, double Qgain);
WDSP_API void destroy_gain (GAIN a);
WDSP_API void flush_gain (GAIN a);
WDSP_API void pSetTXOutputLevel (GAIN a, double level);
WDSP_API void pSetTXOutputLevelRun (GAIN a, int run);
WDSP_API void pSetTXOutputLevelSize (GAIN a, int size);
WDSP_API void xgain (GAIN a);

/*  ---- gaussian.c -------------------------------------------------------- */

WDSP_API void SetRXAGaussianFreqs (int channel, double f_center, double bandwidth);
WDSP_API void SetRXAGaussianGain (int channel, double gain);
WDSP_API void SetRXAGaussianNC (int channel, int nc);
WDSP_API void SetRXAGaussianRun (int channel, int run);

/*  ---- gen.c ------------------------------------------------------------- */

WDSP_API void SetRXAPreGenMode (int channel, int mode);
WDSP_API void SetRXAPreGenNoiseMag (int channel, double mag);
WDSP_API void SetRXAPreGenRun (int channel, int run);
WDSP_API void SetRXAPreGenSweepFreq (int channel, double freq1, double freq2);
WDSP_API void SetRXAPreGenSweepMag (int channel, double mag);
WDSP_API void SetRXAPreGenSweepRate (int channel, double rate);
WDSP_API void SetRXAPreGenToneFreq (int channel, double freq);
WDSP_API void SetRXAPreGenToneMag (int channel, double mag);
WDSP_API void SetTXAPostGenMode (int channel, int mode);
WDSP_API void SetTXAPostGenPulseDutyCycle (int channel, double dc);
WDSP_API void SetTXAPostGenPulseFreq (int channel, double freq);
WDSP_API void SetTXAPostGenPulseIQout (int channel, int IQout);
WDSP_API void SetTXAPostGenPulseMag (int channel, double mag);
WDSP_API void SetTXAPostGenPulseToneFreq (int channel, double freq);
WDSP_API void SetTXAPostGenPulseTransition (int channel, double transtime);
WDSP_API void SetTXAPostGenRun (int channel, int run);
WDSP_API void SetTXAPostGenSweepFreq (int channel, double freq1, double freq2);
WDSP_API void SetTXAPostGenSweepMag (int channel, double mag);
WDSP_API void SetTXAPostGenSweepRate (int channel, double rate);
WDSP_API void SetTXAPostGenTTFreq (int channel, double freq1, double freq2);
WDSP_API void SetTXAPostGenTTMag (int channel, double mag1, double mag2);
WDSP_API void SetTXAPostGenTTPulseDutyCycle (int channel, double dc);
WDSP_API void SetTXAPostGenTTPulseFreq (int channel, double freq);
WDSP_API void SetTXAPostGenTTPulseIQout (int channel, int IQout);
WDSP_API void SetTXAPostGenTTPulseMag (int channel, double mag1, double mag2);
WDSP_API void SetTXAPostGenTTPulseToneFreq (int channel, double freq1, double freq2);
WDSP_API void SetTXAPostGenTTPulseTransition (int channel, double transtime);
WDSP_API void SetTXAPostGenToneFreq (int channel, double freq);
WDSP_API void SetTXAPostGenToneMag (int channel, double mag);
WDSP_API void SetTXAPreGenMode (int channel, int mode);
WDSP_API void SetTXAPreGenNoiseMag (int channel, double mag);
WDSP_API void SetTXAPreGenPulseDutyCycle (int channel, double dc);
WDSP_API void SetTXAPreGenPulseFreq (int channel, double freq);
WDSP_API void SetTXAPreGenPulseMag (int channel, double mag);
WDSP_API void SetTXAPreGenPulseToneFreq (int channel, double freq);
WDSP_API void SetTXAPreGenPulseTransition (int channel, double transtime);
WDSP_API void SetTXAPreGenRun (int channel, int run);
WDSP_API void SetTXAPreGenSawtoothFreq (int channel, double freq);
WDSP_API void SetTXAPreGenSawtoothMag (int channel, double mag);
WDSP_API void SetTXAPreGenSweepFreq (int channel, double freq1, double freq2);
WDSP_API void SetTXAPreGenSweepMag (int channel, double mag);
WDSP_API void SetTXAPreGenSweepRate (int channel, double rate);
WDSP_API void SetTXAPreGenToneFreq (int channel, double freq);
WDSP_API void SetTXAPreGenToneMag (int channel, double mag);
WDSP_API void SetTXAPreGenTriangleFreq (int channel, double freq);
WDSP_API void SetTXAPreGenTriangleMag (int channel, double mag);

/*  ---- iir.c ------------------------------------------------------------- */

WDSP_API void SetRXABiQuadBandwidth (int channel, double bw);
WDSP_API void SetRXABiQuadFreq (int channel, double freq);
WDSP_API void SetRXABiQuadGain (int channel, double gain);
WDSP_API void SetRXABiQuadRun (int channel, int run);
WDSP_API void SetRXAmpeakFilBw (int channel, int fil, double bw);
WDSP_API void SetRXAmpeakFilEnable (int channel, int fil, int enable);
WDSP_API void SetRXAmpeakFilFreq (int channel, int fil, double freq);
WDSP_API void SetRXAmpeakFilGain (int channel, int fil, double gain);
WDSP_API void SetRXAmpeakNpeaks (int channel, int npeaks);
WDSP_API void SetRXAmpeakRun (int channel, int run);

/*  ---- impulse_cache.c --------------------------------------------------- */

WDSP_API void destroy_impulse_cache (void);
WDSP_API void init_impulse_cache (int use);
WDSP_API int read_impulse_cache (const char* path);
WDSP_API int save_impulse_cache (const char* path);
WDSP_API void use_impulse_cache (int use);

/*  ---- iobuffs.c --------------------------------------------------------- */

WDSP_API void fexchange0 (int channel, double* in, double* out, int* error);
WDSP_API void fexchange2 (int channel, INREAL *Iin, INREAL *Qin, OUTREAL *Iout, OUTREAL *Qout, int* error);

/*  ---- matchedCW.c ------------------------------------------------------- */

WDSP_API void SetRXAMatchedFreqs (int channel, double f_center, double bandwidth);
WDSP_API void SetRXAMatchedGain (int channel, double gain);
WDSP_API void SetRXAMatchedRun (int channel, int run);

/*  ---- meter.c ----------------------------------------------------------- */

WDSP_API double GetRXAMeter (int channel, int mt);
WDSP_API double GetTXAMeter (int channel, int mt);

/*  ---- nbp.c ------------------------------------------------------------- */

WDSP_API int RXANBPAddNotch (int channel, int notch, double fcenter, double fwidth, int active);
WDSP_API int RXANBPDeleteNotch (int channel, int notch);
WDSP_API int RXANBPEditNotch (int channel, int notch, double fcenter, double fwidth, int active);
WDSP_API void RXANBPGetMinNotchWidth (int channel, double* minwidth);
WDSP_API int RXANBPGetNotch (int channel, int notch, double* fcenter, double* fwidth, int* active);
WDSP_API void RXANBPGetNumNotches (int channel, int* nnotches);
WDSP_API void RXANBPSetAutoIncrease (int channel, int autoincr);
WDSP_API void RXANBPSetFreqs (int channel, double flow, double fhigh);
WDSP_API void RXANBPSetMP (int channel, int mp);
WDSP_API void RXANBPSetNC (int channel, int nc);
WDSP_API void RXANBPSetNotchesRun (int channel, int run);
WDSP_API void RXANBPSetRun (int channel, int run);
WDSP_API void RXANBPSetShiftFrequency (int channel, double shift);
WDSP_API void RXANBPSetTuneFrequency (int channel, double tunefreq);
WDSP_API void RXANBPSetWindow (int channel, int wintype);

/*  ---- nnet.c ------------------------------------------------------------ */

WDSP_API void SetNNRModelPath (const char* path);
WDSP_API void SetNNRModelPathSlot (int slot, const char* path);

/*  ---- nnr.c ------------------------------------------------------------- */

WDSP_API int GetRXANNRModel (int channel);
WDSP_API void SetRXANNRAlpha (int channel, double alpha);
WDSP_API void SetRXANNRAlphaKnee (int channel, double knee_db);
WDSP_API void SetRXANNRMaskFloor (int channel, double floor_db);
WDSP_API void SetRXANNRMaxGain (int channel, double gmax_db);
WDSP_API int SetRXANNRModel (int channel, int slot);
WDSP_API void SetRXANNRPosition (int channel, int position);
WDSP_API void SetRXANNRRun (int channel, int setit);
WDSP_API void SetRXANNRSmooth (int channel, double att_ms, double rel_ms);
WDSP_API void SetRXANNRTau (int channel, double tau);
WDSP_API void SetRXANNRTestMode (int channel, int mode);
WDSP_API void SetRXANNRcmode (int channel, int cmode);

/*  ---- nob.c ------------------------------------------------------------- */

WDSP_API void SetEXTANBAdvtime (int id, double time);
WDSP_API void SetEXTANBBacktau (int id, double tau);
WDSP_API void SetEXTANBBuffsize (int id, int size);
WDSP_API void SetEXTANBHangtime (int id, double time);
WDSP_API void SetEXTANBRun (int id, int run);
WDSP_API void SetEXTANBSamplerate (int id, int rate);
WDSP_API void SetEXTANBTau (int id, double tau);
WDSP_API void SetEXTANBThreshold (int id, double thresh);
WDSP_API ANB create_anb (int run, int buffsize, double* in, double* out, double samplerate, double tau, double hangtime, double advtime, double backtau, double threshold);
WDSP_API void create_anbEXT (int id, int run, int buffsize, double samplerate, double tau, double hangtime, double advtime, double backtau, double threshold);
WDSP_API void destroy_anb (ANB a);
WDSP_API void destroy_anbEXT (int id);
WDSP_API void flush_anb (ANB a);
WDSP_API void flush_anbEXT (int id);
WDSP_API void pSetRCVRANBAdvtime (ANB a, double time);
WDSP_API void pSetRCVRANBBacktau (ANB a, double tau);
WDSP_API void pSetRCVRANBBuffsize (ANB a, int size);
WDSP_API void pSetRCVRANBHangtime (ANB a, double time);
WDSP_API void pSetRCVRANBRun (ANB a, int run);
WDSP_API void pSetRCVRANBSamplerate (ANB a, int rate);
WDSP_API void pSetRCVRANBTau (ANB a, double tau);
WDSP_API void pSetRCVRANBThreshold (ANB a, double thresh);
WDSP_API void xanb (ANB a);
WDSP_API void xanbEXT (int id, double* in, double* out);
WDSP_API void xanbEXTF (int id, float *I, float *Q);

/*  ---- nobII.c ----------------------------------------------------------- */

WDSP_API void SetEXTNOBAdvtime (int id, double time);
WDSP_API void SetEXTNOBBacktau (int id, double tau);
WDSP_API void SetEXTNOBBuffsize (int id, int size);
WDSP_API void SetEXTNOBHangtime (int id, double time);
WDSP_API void SetEXTNOBMode (int id, int mode);
WDSP_API void SetEXTNOBRun (int id, int run);
WDSP_API void SetEXTNOBSamplerate (int id, int rate);
WDSP_API void SetEXTNOBTau (int id, double tau);
WDSP_API void SetEXTNOBThreshold (int id, double thresh);
WDSP_API NOB create_nob (int run, int buffsize, double* in, double* out, double samplerate, int mode, double advslewtime, double advtime, double hangslewtime, double hangtime, double max_imp_seq_time, double backtau, double threshold);
WDSP_API void create_nobEXT (int id, int run, int mode, int buffsize, double samplerate, double slewtime, double hangtime, double advtime, double backtau, double threshold);
WDSP_API void destroy_nob (NOB a);
WDSP_API void destroy_nobEXT (int id);
WDSP_API void flush_nob (NOB a);
WDSP_API void flush_nobEXT (int id);
WDSP_API void pSetRCVRNOBAdvtime (NOB a, double time);
WDSP_API void pSetRCVRNOBBacktau (NOB a, double tau);
WDSP_API void pSetRCVRNOBBuffsize (NOB a, int size);
WDSP_API void pSetRCVRNOBHangtime (NOB a, double time);
WDSP_API void pSetRCVRNOBMode (NOB a, int mode);
WDSP_API void pSetRCVRNOBRun (NOB a, int run);
WDSP_API void pSetRCVRNOBSamplerate (NOB a, int rate);
WDSP_API void pSetRCVRNOBTau (NOB a, double tau);
WDSP_API void pSetRCVRNOBThreshold (NOB a, double thresh);
WDSP_API void xnob (NOB a);
WDSP_API void xnobEXT (int id, double* in, double* out);
WDSP_API void xnobEXTF (int id, float *I, float *Q);

/*  ---- osctrl.c ---------------------------------------------------------- */

WDSP_API void SetTXAosctrlRun (int channel, int run);

/*  ---- patchpanel.c ------------------------------------------------------ */

WDSP_API void SetRXAPanelBinaural (int channel, int bin);
WDSP_API void SetRXAPanelCopy (int channel, int copy);
WDSP_API void SetRXAPanelGain1 (int channel, double gain);
WDSP_API void SetRXAPanelGain2 (int channel, double gainI, double gainQ);
WDSP_API void SetRXAPanelPan (int channel, double pan);
WDSP_API void SetRXAPanelRun (int channel, int run);
WDSP_API void SetRXAPanelSelect (int channel, int select);
WDSP_API void SetTXAPanelGain1 (int channel, double gain);
WDSP_API void SetTXAPanelRun (int channel, int run);
WDSP_API void SetTXAPanelSelect (int channel, int select);

/*  ---- phrot.c ----------------------------------------------------------- */

WDSP_API void GetTXAPHROTAsymmetry (int channel, double* in_pos, double* in_neg, double* in_ratio, double* out_pos, double* out_neg, double* out_ratio, double* current_fc, double* auto_step);
WDSP_API void SetTXAPHROTAutoMode (int channel, int autoMode);
WDSP_API void SetTXAPHROTAutoReset (int channel);
WDSP_API void SetTXAPHROTCorner (int channel, double frequency);
WDSP_API void SetTXAPHROTNstages (int channel, int nstages);
WDSP_API void SetTXAPHROTReverse (int channel, int reverse);
WDSP_API void SetTXAPHROTRun (int channel, int run);

/*  ---- resample.c -------------------------------------------------------- */

WDSP_API RESAMPLE create_resample (int run, int size, double* in, double* out, int in_rate, int out_rate, double fc, int ncoef, double gain);
WDSP_API void* create_resampleFV (int in_rate, int out_rate);
WDSP_API void* create_resampleV (int in_rate, int out_rate);
WDSP_API void destroy_resample (RESAMPLE a);
WDSP_API void destroy_resampleFV (void* ptr);
WDSP_API void destroy_resampleV (void* ptr);
WDSP_API void flush_resample (RESAMPLE a);
WDSP_API int xresample (RESAMPLE a);
WDSP_API void xresampleFV (float* input, float* output, int numsamps, int* outsamps, void* ptr);
WDSP_API void xresampleV (double* input, double* output, int numsamps, int* outsamps, void* ptr);

/*  ---- rmatch.c ---------------------------------------------------------- */

WDSP_API void* create_rmatchLegacyV (int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize);
WDSP_API void* create_rmatchV (int in_size, int out_size, int nom_inrate, int nom_outrate, int ringsize, double var);
WDSP_API void destroy_rmatchV (void* ptr);
WDSP_API void forceRMatchVar (void* b, int force, double fvar);
WDSP_API void getControlFlag (void* ptr, int* control_flag);
WDSP_API void getRMatchDiags (void* b, int* underflows, int* overflows, double* var, int* ringsize, int* nring);
WDSP_API void resetRMatchDiags (void* b);
WDSP_API void setRMatchFFAlpha (void* ptr, double ff_alpha);
WDSP_API void setRMatchFFRingMax (void* ptr, int ff_ringmax);
WDSP_API void setRMatchFFRingMin (void* ptr, int ff_ringmin);
WDSP_API void setRMatchFeedbackGain (void* b, double feedback_gain);
WDSP_API void setRMatchInsize (void* ptr, int insize);
WDSP_API void setRMatchNomInrate (void* ptr, int nom_inrate);
WDSP_API void setRMatchNomOutrate (void* ptr, int nom_outrate);
WDSP_API void setRMatchOutsize (void* ptr, int outsize);
WDSP_API void setRMatchPropRingMax (void* ptr, int prop_max);
WDSP_API void setRMatchPropRingMin (void* ptr, int prop_min);
WDSP_API void setRMatchRingsize (void* ptr, int ringsize);
WDSP_API void setRMatchSlewTime (void* b, double slew_time);
WDSP_API void setRMatchSlewTime1 (void* b, double slew_time);
WDSP_API void xrmatchIN (void* b, double* in);
WDSP_API void xrmatchOUT (void* b, double* out);

/*  ---- sender.c ---------------------------------------------------------- */

WDSP_API void SetRXASpectrum (int channel, int flag, int disp, int ss, int LO);

/*  ---- shift.c ----------------------------------------------------------- */

WDSP_API void SetRXAShiftFreq (int channel, double fshift);
WDSP_API void SetRXAShiftRun (int channel, int run);

/*  ---- siphon.c ---------------------------------------------------------- */

WDSP_API void GetaSipF1EXT (int id, float* out, int size);
WDSP_API void RXAGetaSipF (int channel, float* out, int size);
WDSP_API void RXAGetaSipF1 (int channel, float* out, int size);
WDSP_API void SetSiphonInsize (int id, int size);
WDSP_API void TXAGetSpecF1 (int channel, float* out);
WDSP_API void TXAGetaSipF (int channel, float* out, int size);
WDSP_API void TXAGetaSipF1 (int channel, float* out, int size);
WDSP_API void TXASetSipAllocDisps (int channel, int n_alloc_disps, int* alloc_run, int* alloc_disp);
WDSP_API void TXASetSipDisplay (int channel, int disp);
WDSP_API void TXASetSipMode (int channel, int mode);
WDSP_API void TXASetSipPosition (int channel, int pos);
WDSP_API void TXASetSipSpecmode (int channel, int mode);
WDSP_API void create_siphonEXT (int id, int run, int insize, int sipsize, int fftsize, int specmode);
WDSP_API void destroy_siphonEXT (int id);
WDSP_API void flush_siphonEXT (int id);
WDSP_API void xsiphonEXT (int id, double* buff);

/*  ---- slew.c ------------------------------------------------------------ */

WDSP_API void SetTXAuSlewTime (int channel, double time);

/*  ---- snb.c ------------------------------------------------------------- */

WDSP_API void RXABPSNBASetMP (int channel, int mp);
WDSP_API void RXABPSNBASetNC (int channel, int nc);
WDSP_API void SetRXASNBAOutputBandwidth (int channel, double flow, double fhigh);
WDSP_API void SetRXASNBARun (int channel, int run);
WDSP_API void SetRXASNBAasize (int channel, int size);
WDSP_API void SetRXASNBAbridge (int channel, int bridge);
WDSP_API void SetRXASNBAk1 (int channel, double k1);
WDSP_API void SetRXASNBAk2 (int channel, double k2);
WDSP_API void SetRXASNBAnpasses (int channel, int npasses);
WDSP_API void SetRXASNBAovrlp (int channel, int ovrlp);
WDSP_API void SetRXASNBApmultmin (int channel, double pmultmin);
WDSP_API void SetRXASNBApostsamps (int channel, int postsamps);
WDSP_API void SetRXASNBApresamps (int channel, int presamps);

/*  ---- ssql.c ------------------------------------------------------------ */

WDSP_API void SetRXASSQLRun (int channel, int run);
WDSP_API void SetRXASSQLTauMute (int channel, double tau_mute);
WDSP_API void SetRXASSQLTauUnMute (int channel, double tau_unmute);
WDSP_API void SetRXASSQLThreshold (int channel, double threshold);

/*  ---- utilities.c ------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
WDSP_API void DestroyCriticalSection (void* cs_ptr);
#endif
WDSP_API int create_bfcu (int id, int min_size, int max_size, double rate, double corner, int points);
WDSP_API void destroy_bfcu (int id);
WDSP_API void getFilterCorners (int id, int* lower_index, int* upper_index);
WDSP_API void getFilterCurve (int id, int size, int w_type, int index_low, int index_high, double* segment);
WDSP_API void * malloc0 (int size);
WDSP_API void print_buffer_parameters (const char* filename, int channel);

/*  ---- varsamp.c --------------------------------------------------------- */

WDSP_API void* create_varsampV (int in_rate, int out_rate, int R);
WDSP_API void destroy_varsampV (void* ptr);
WDSP_API void xvarsampV (double* input, double* output, int numsamps, double var, int* outsamps, void* ptr);

/*  ---- version.c --------------------------------------------------------- */

WDSP_API int GetWDSPVersion (void);

/*  ---- wbfm.c ------------------------------------------------------------ */

WDSP_API int GetRXAWBFMStereoIndicator (int channel);
WDSP_API void SetRXAWBFMdmph (int channel, int dmph_run, int dmph_continent);

/*  ---- wcpAGC.c ---------------------------------------------------------- */

WDSP_API void GetRXAAGCHangLevel (int channel, double *hangLevel);
WDSP_API void GetRXAAGCHangThreshold (int channel, int *hangthreshold);
WDSP_API void GetRXAAGCThresh (int channel, double *thresh, double size, double rate);
WDSP_API void GetRXAAGCTop (int channel, double *max_agc);
WDSP_API void SetRXAAGCAttack (int channel, int attack);
WDSP_API void SetRXAAGCDecay (int channel, int decay);
WDSP_API void SetRXAAGCFixed (int channel, double fixed_agc);
WDSP_API void SetRXAAGCHang (int channel, int hang);
WDSP_API void SetRXAAGCHangLevel (int channel, double hangLevel);
WDSP_API void SetRXAAGCHangThreshold (int channel, int hangthreshold);
WDSP_API void SetRXAAGCMaxInputLevel (int channel, double level);
WDSP_API void SetRXAAGCMode (int channel, int mode);
WDSP_API void SetRXAAGCSlope (int channel, int slope);
WDSP_API void SetRXAAGCThresh (int channel, double thresh, double size, double rate);
WDSP_API void SetRXAAGCTop (int channel, double max_agc);
WDSP_API void SetTXAALCAttack (int channel, int attack);
WDSP_API void SetTXAALCDecay (int channel, int decay);
WDSP_API void SetTXAALCHang (int channel, int hang);
WDSP_API void SetTXAALCMaxGain (int channel, double maxgain);
WDSP_API void SetTXAALCSt (int channel, int state);
WDSP_API void SetTXALevelerAttack (int channel, int attack);
WDSP_API void SetTXALevelerDecay (int channel, int decay);
WDSP_API void SetTXALevelerHang (int channel, int hang);
WDSP_API void SetTXALevelerSt (int channel, int state);
WDSP_API void SetTXALevelerTop (int channel, double maxgain);

/*  ---- wisdom.c ---------------------------------------------------------- */

WDSP_API int WDSPwisdom (char* directory);
WDSP_API char* wisdom_get_status (void);

#ifdef __cplusplus
}
#endif

#endif
