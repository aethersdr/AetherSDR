/*  patchpanel.h

This file is part of a program that implements a Software-Defined Radio.

Copyright (C) 2013 Warren Pratt, NR0V

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

#ifndef _patchpanel_h
#define _patchpanel_h

typedef struct _patchpanel
{
	int channel;
	int run;
	int size;
	double* in;
	double* out;
	double gain1;
	double gain2I;
	double gain2Q;
	int inselect;
	int copy;
} panel, *PANEL;

extern PANEL create_panel (int channel, int run, int size, double* in, double* out, double gain1, double gain2I, double gain2Q, int inselect, int copy);

extern void destroy_panel (PANEL a);

extern void flush_panel (PANEL a);

extern void xpanel (PANEL a);

extern void setBuffers_panel (PANEL a, double* in, double* out);

extern void setSamplerate_panel (PANEL a, int rate);

extern void setSize_panel (PANEL a, int size);

// RXA Properties

extern __declspec (dllexport) void SetRXAPanelRun (int channel, int run);

extern __declspec (dllexport) void SetRXAPanelGain1 (int channel, double gain);

extern __declspec (dllexport) void SetRXAPanelGain2 (int channel, double gainI, double gainQ);

extern __declspec (dllexport) void SetRXAPanelPan (int channel, double pan);

extern __declspec (dllexport) void SetRXAPanelCopy (int channel, int copy);

extern __declspec (dllexport) void SetRXAPanelBinaural (int channel, int bin);

// TXA Properties

extern __declspec (dllexport) void SetTXAPanelRun (int channel, int run);

#endif