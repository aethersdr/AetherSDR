/* *****************************************************************************
 *	resampler.h														2014 AUG 23
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


#ifndef RESAMPLER_H_
#define RESAMPLER_H_


#define RESAMPLES	384		// Must be divisible by 3

void fdmdv_8_to_24(float out24k[], float in8k[], int n);
void fdmdv_24_to_8(float out8k[], float in24k[], int n);


#endif /* RESAMPLER_H_ */

// EoF =====-----
