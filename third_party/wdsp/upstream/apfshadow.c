/*  apfshadow.c

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2025 Warren Pratt, NR0V

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

warren@pratt.one
*/

#include "comm.h"

APFSHADOW create_apfshadow (int selection, int run, double f_center, double bandwidth, double gain)
{
	APFSHADOW a = (APFSHADOW)malloc0 (sizeof (apfshadow));
	a->selection = selection;
	a->run = run;
	a->f_center = f_center;
	a->bandwidth = bandwidth;
	a->gain = gain;
	return a;
}

void destroy_apfshadow (APFSHADOW a)
{
	_aligned_free (a);
}

PORT
void SetRXASPCWSelection (int channel, int selection)
{
	APFSHADOW a = rxa[channel].apfshadow.p;
	if (a->selection != selection)
	{
		a->selection = selection;
		switch (a->selection)
		{
		case 0: // Double-pole
			SetRXAMatchedRun (channel, 0);
			SetRXAGaussianRun (channel, 0);
			SetRXABiQuadRun (channel, 0);
			SetRXADoublepoleFreqs (channel, a->f_center, a->bandwidth);
			SetRXADoublepoleGain (channel, a->gain);
			SetRXADoublepoleRun (channel, a->run);
			break;
		case 1: // Matched
			SetRXADoublepoleRun (channel, 0);
			SetRXAGaussianRun (channel, 0);
			SetRXABiQuadRun (channel, 0);
			SetRXAMatchedFreqs (channel, a->f_center, a->bandwidth);
			SetRXAMatchedGain (channel, sqrt (2.0) * a->gain);
			SetRXAMatchedRun (channel, a->run);
			break;
		case 2: // Gaussian
			SetRXADoublepoleRun (channel, 0);
			SetRXAMatchedRun (channel, 0);
			SetRXABiQuadRun (channel, 0);
			SetRXAGaussianFreqs (channel, a->f_center, a->bandwidth);
			SetRXAGaussianGain (channel, sqrt (2.0) * a->gain);
			SetRXAGaussianRun (channel, a->run);
			break;
		case 3: // Bi-quad
			SetRXADoublepoleRun (channel, 0);
			SetRXAMatchedRun (channel, 0);
			SetRXAGaussianRun (channel, 0);
			SetRXABiQuadFreq (channel, a->f_center);
			SetRXABiQuadBandwidth (channel, a->bandwidth);
			SetRXABiQuadGain (channel, a->gain);
			SetRXABiQuadRun (channel, a->run);
			break;
		default:
			break;
		}
	}
}

PORT
void SetRXASPCWRun (int channel, int run)
{
	APFSHADOW a = rxa[channel].apfshadow.p;
	a->run = run;
	switch (a->selection)
	{
	case 0:	// Double-pole
		SetRXADoublepoleRun (channel, a->run);
		break;
	case 1:	// Matched
		SetRXAMatchedRun (channel, a->run);
		break;
	case 2:	// Gaussian
		SetRXAGaussianRun (channel, a->run);
		break;
	case 3:	// Bi-quad
		SetRXABiQuadRun (channel, a->run);
		break;
	default:
		break;
	}
}

PORT
void SetRXASPCWFreq (int channel, double f_center)
{
	APFSHADOW a = rxa[channel].apfshadow.p;
	a->f_center = f_center;
	switch (a->selection)
	{
	case 0:	// Double-pole
		SetRXADoublepoleFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 1:	// Matched
		SetRXAMatchedFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 2:	// Gaussian
		SetRXAGaussianFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 3:	// Bi-quad
		SetRXABiQuadFreq (channel, a->f_center);
		break;
	default:
		break;
	}
}

PORT
void SetRXASPCWBandwidth (int channel, double bandwidth)
{
	APFSHADOW a = rxa[channel].apfshadow.p;
	a->bandwidth = bandwidth;
	switch (a->selection)
	{
	case 0:	// Double-pole
		SetRXADoublepoleFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 1:	// Matched
		SetRXAMatchedFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 2:	// Gaussian
		SetRXAGaussianFreqs (channel, a->f_center, a->bandwidth);
		break;
	case 3:	// Bi-quad
		SetRXABiQuadBandwidth (channel, a->bandwidth);
		break;
	default:
		break;
	}
}

PORT
void SetRXASPCWGain (int channel, double gain)
{
	APFSHADOW a = rxa[channel].apfshadow.p;
	a->gain = gain;
	switch (a->selection)
	{
	case 0:	// Double-pole
		SetRXADoublepoleGain (channel, a->gain);
		break;
	case 1:	// Matched
		SetRXAMatchedGain (channel, sqrt(2.0) * a->gain);
		break;
	case 2:	// Gaussian
		SetRXAGaussianGain (channel, sqrt(2.0) * a->gain);
		break;
	case 3:	// Bi-quad
		SetRXABiQuadGain (channel, a->gain);
		break;
	default:
		break;
	}
}

