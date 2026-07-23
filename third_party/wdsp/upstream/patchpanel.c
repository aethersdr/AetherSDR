/*  patchpanel.c

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

#include "comm.h"

PANEL create_panel (int channel, int run, int size, double* in, double* out, double gain1, double gain2I, double gain2Q, int inselect, int copy)
{
	PANEL a = (PANEL) malloc0 (sizeof (panel));
	a->channel = channel;
	a->run = run;
	a->size = size;
	a->in = in;
	a->out = out;
	a->gain1 = gain1;
	a->gain2I = gain2I;
	a->gain2Q = gain2Q;
	a->inselect = inselect;
	a->copy = copy;
	return a;
}

void destroy_panel (PANEL a)
{
	_aligned_free (a);
}

void flush_panel (PANEL a)
{

}

void xpanel (PANEL a)
{	
	int i;
	double I, Q;
	double gainI = a->gain1 * a->gain2I;
	double gainQ = a->gain1 * a->gain2Q;
	if (a->run)
	{
		// inselect is either 0(neither), 1(Q), 2(I), or 3(both)
		switch (a->copy)
		{
		case 0:	// no copy
			for (i = 0; i < a->size; i++)
			{
				I = a->in[2 * i + 0] * (a->inselect >> 1);
				Q = a->in[2 * i + 1] * (a->inselect & 1);
				a->out[2 * i + 0] = I;
				a->out[2 * i + 1] = Q;
			}
			break;
		case 1:	// copy I to Q (then Q == I)
			for (i = 0; i < a->size; i++)
			{
				I = a->in[2 * i + 0] * (a->inselect >> 1);
				Q = I;
				a->out[2 * i + 0] = I;
				a->out[2 * i + 1] = Q;
			}
			break;
		case 2:	// copy Q to I (then I == Q)
			for (i = 0; i < a->size; i++)
			{
				Q = a->in[2 * i + 1] * (a->inselect & 1);
				I = Q;
				a->out[2 * i + 0] = I;
				a->out[2 * i + 1] = Q;
			}
			break;
		case 3:	// reverse (I=>Q and Q=>I)
			for (i = 0; i < a->size; i++)
			{
				Q = a->in[2 * i + 0] * (a->inselect >> 1);
				I = a->in[2 * i + 1] * (a->inselect & 1);
				a->out[2 * i + 0] = I;
				a->out[2 * i + 1] = Q;
			}
			break;
		}
	}
	else if (a->out != a->in)
		memcpy(a->out, a->in, a->size * sizeof(complex));
	for (i = 0; i < a->size; i++)
	{
		a->out[2 * i + 0] *= gainI;
		a->out[2 * i + 1] *= gainQ;
	}
}

void setBuffers_panel (PANEL a, double* in, double* out)
{
	a->in = in;
	a->out = out;
}

void setSamplerate_panel (PANEL a, int rate)
{

}

void setSize_panel (PANEL a, int size)
{
	a->size = size;
}

/********************************************************************************************************
*																										*
*											RXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetRXAPanelRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelSelect (int channel, int select)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->inselect = select;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelGain1 (int channel, double gain)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->gain1 = gain;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelGain2 (int channel, double gainI, double gainQ)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->gain2I = gainI;
	rxa[channel].panel.p->gain2Q = gainQ;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelPan (int channel, double pan)
{
	double gain1, gain2;
	EnterCriticalSection (&ch[channel].csDSP);
	if (pan <= 0.5)
	{
		gain1 = 1.0;
		gain2 = sin (pan * PI);
	}
	else
	{
		gain1 = sin (pan * PI);
		gain2 = 1.0;
	}
	rxa[channel].panel.p->gain2I = gain1;
	rxa[channel].panel.p->gain2Q = gain2;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelCopy (int channel, int copy)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->copy = copy;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetRXAPanelBinaural (int channel, int bin)
{
	EnterCriticalSection (&ch[channel].csDSP);
	rxa[channel].panel.p->copy = 1 - bin;
	LeaveCriticalSection (&ch[channel].csDSP);
}

/********************************************************************************************************
*																										*
*											TXA Properties												*
*																										*
********************************************************************************************************/

PORT
void SetTXAPanelRun (int channel, int run)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].panel.p->run = run;
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPanelGain1 (int channel, double gain)
{
	EnterCriticalSection (&ch[channel].csDSP);
	txa[channel].panel.p->gain1 = gain;
	//print_message ("micgainset.txt", "Set MIC Gain to", (int)(100.0 * gain), 0, 0);
	LeaveCriticalSection (&ch[channel].csDSP);
}

PORT
void SetTXAPanelSelect (int channel, int select)
{
	EnterCriticalSection (&ch[channel].csDSP);
	if (select == 1) 
		txa[channel].panel.p->copy = 3;
	else
		txa[channel].panel.p->copy = 0;
	txa[channel].panel.p->inselect = select;
	LeaveCriticalSection (&ch[channel].csDSP);
}