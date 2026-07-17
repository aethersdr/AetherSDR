/*  main.h

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

#ifndef _mainloop_h
#define _mainloop_h

extern void wdspmain (void *pargs);

extern void create_main (int channel);

extern void destroy_main (int channel);

extern void flush_main (int channel);

extern void setInputSamplerate_main (int channel);

extern void setOutputSamplerate_main (int channel);

extern void setDSPSamplerate_main (int channel);

extern void setDSPBuffsize_main (int channel);

#endif