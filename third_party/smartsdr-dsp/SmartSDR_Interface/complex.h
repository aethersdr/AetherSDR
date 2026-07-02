/* *****************************************************************************
 * 	complex.h
 *
 *  \date Mar 31, 2012
 *  \author Bob / N4HY
 *
 * *****************************************************************************
 *
 *  Copyright (C) 2012-2014 FlexRadio Systems.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Contact Information:
 *  email: gpl<at>flexradiosystems.com
 *  Mail:  FlexRadio Systems, Suite 1-150, 4616 W. Howard LN, Austin, TX 78728
 *
 * ************************************************************************** */

#ifndef COMPLEX_H_
#define COMPLEX_H_

#include <math.h>

#include "datatypes.h"

typedef struct _complex {
  float real; // left
  float imag; // right
} Complex;

#define CReal(x) ((x.real))
#define CImag(x) ((x.imag))

extern Complex Cplx(float x, float y);

extern Complex ComplexAdd(Complex x, Complex y);

extern Complex ComplexSub(Complex x, Complex y);

extern Complex ComplexMul(Complex x, Complex y);

extern Complex ComplexDiv(Complex x, Complex y);

extern Complex ComplexScl(Complex x, float scl);

extern Complex ComplexCjg(Complex x);

extern float ComplexPwr(Complex x);

extern float ComplexMag(Complex x);

#endif /* COMPLEX_H_ */
