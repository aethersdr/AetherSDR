/* *****************************************************************************
 *	resampler.c														2014 AUG 23
 *
 *	Upsampler and downsampler by integer 3
 *		for translation to/from 8 ksps from/to 24 ksps
 *
 *	Includes 4 kHz Low Pass filter in each instance
 *
 *	Derived from code released by David Rowe as part of FreeDV/CODEC2 project
 *	Adapted by Graham / KE9H
 *
 * *****************************************************************************
 *
 *	Copyright (C) 2014 FlexRadio Systems.
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *	GNU General Public License for more details.
 *	You should have received a copy of the GNU General Public License
 *	along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 **************************************************************************** */


#include "resampler.h"


// For 8 to 24 kHz sample rate conversion */

#define FDMDV_OS                 3   	// oversampling rate
#define FDMDV_OS_TAPS           48  	// number of OS filter taps



/* *****************************************************************************
 *	Coefficients for 4 kHz low pass filter at 24 ksps for 48 tap FIR filter
 *
 * 	Generated using fir1(47, 1/3) in MatLab (Octave)
 */

const float fdmdv_os24_filter[]= {
	-0.000565842330864509,
	-0.00119184233667459,
	-0.000686550128357081,
	 0.000939738560355487,
	 0.00235824811185176,
	 0.00149083509882116,
	-0.00207002114214581,
	-0.00516284617910486,
	-0.00318858060009128,
	 0.00422846062091092,
	 0.0102371199934064,
	 0.00615820273645780,
	-0.00786127965697296,
	-0.0187253107816201,
	-0.0111560475540299,
	 0.0139752338625282,
	 0.0334879967920482,
	 0.0202917237268834,
	-0.0258029481868858,
	-0.0651503052036609,
	-0.0430343789277145,
	 0.0624453219256916,
	 0.210663786004670,
	 0.318319285594497,
	 0.318319285594497,
	 0.210663786004670,
	 0.0624453219256916,
	-0.0430343789277145,
	-0.0651503052036609,
	-0.0258029481868858,
	 0.0202917237268834,
	 0.0334879967920482,
	 0.0139752338625282,
	-0.0111560475540299,
	-0.0187253107816201,
	-0.00786127965697296,
	 0.00615820273645780,
	 0.0102371199934064,
	 0.00422846062091092,
	-0.00318858060009128,
	-0.00516284617910486,
	-0.00207002114214581,
	 0.00149083509882116,
	 0.00235824811185176,
	 0.000939738560355487,
	-0.000686550128357081,
	-0.00119184233667459,
	-0.000565842330864509
};


/* *****************************************************************************
 *	void fdmdv_8_to_24(float out24k[], float in8k[], int n)
 *
 *	Changes the sample rate of a signal from 8 to 24 kHz.
 *	8 ksps is the native sampling rate of the CODEC2 vocoder.
 *  24 ksps is the native audio/baseband sampling rate for the FLEX-6000 series.
 *
 *	n is the number of samples at the 8 ksps rate.
 *	There are  (FDMDV_OS * n)  samples at the 24 ksps rate.
 *	A memory of FDMDV_OS_TAPS/FDMDV_OS samples is required for in8k[]
 *		(see [David Rowe] t48_8.c unit test as example).
 *
 *	This is a classic polyphase upsampler.  We take the 8 ksps samples
 *	and insert (FDMDV_OS - 1) zeroes between each sample, then
 *	FDMDV_OS_TAPS FIR low pass filter the signal at 4kHz.  As most of
 *	the input samples are zeroes, we only need to multiply non-zero
 *	input samples by filter coefficients.  The zero insertion and
 *	filtering are combined in the code below.
 *
 **************************************************************************** */

void fdmdv_8_to_24(float out24k[], float in8k[], int n)
{
    int i,j,k,l;

    /* make sure n is an integer multiple of the oversampling rate, otherwise
       this function breaks */

    // assert((n % FDMDV_OS) == 0);

    for(i=0; i<n; i++)
    {
		for(j=0; j<FDMDV_OS; j++)
		{
			out24k[i*FDMDV_OS+j] = 0.0;
			for(k=0,l=0; k<FDMDV_OS_TAPS; k+=FDMDV_OS,l++)
			out24k[i*FDMDV_OS+j] += fdmdv_os24_filter[k+j]*in8k[i-l];
			out24k[i*FDMDV_OS+j] *= FDMDV_OS;
		}
    }

    /* update filter memory */
    for(i=-(FDMDV_OS_TAPS/FDMDV_OS); i<0; i++)
	in8k[i] = in8k[i + n];
}


/* *****************************************************************************
 *	void fdmdv_24_to_8(float out8k[], float in24k[], int n)
 *
 *	Changes the sample rate of a signal from 24 to 8 kHz.
 *
 *	n is the number of samples at the 8 kHz rate, there are FDMDV_OS*n
 *	samples at the 24 kHz rate.  As above however a memory of
 *	FDMDV_OS_TAPS samples is reqd for in24k[] (see t48_8.c unit test as example).
 *
 *	Low pass filter the 24 ksps signal at 4 kHz using the same filter as
 *	the upsampler, then just output every FDMDV_OS-th filtered sample.
 *
 **************************************************************************** */

void fdmdv_24_to_8(float out8k[], float in24k[], int n)
{
    int i,j;

    for(i=0; i<n; i++)
    {
		out8k[i] = 0.0;
		for(j=0; j<FDMDV_OS_TAPS; j++)
			out8k[i] += fdmdv_os24_filter[j]*in24k[i*FDMDV_OS-j];
    }

    /* update filter memory */
    for(i=-FDMDV_OS_TAPS; i<0; i++)
	in24k[i] = in24k[i + n*FDMDV_OS];
}


// EoF =====-----
