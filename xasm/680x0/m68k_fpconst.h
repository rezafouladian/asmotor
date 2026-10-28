/*  Copyright 2008-2026 Carsten Elton Sorensen and contributors

    This file is part of ASMotor.

    ASMotor is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ASMotor is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ASMotor.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef XASM_M68K_FPCONST_H_INCLUDED_
#define XASM_M68K_FPCONST_H_INCLUDED_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "m68k_parse.h"

#define FPCONST_MAX_STRING 128
#define FPCONST_MAX_BYTES  12

// Encode an MPW Asm style FPU immediate string, as in FADD.S #"0.25",FP0.
// "$7FC01400" is a bit pattern, right aligned in the operand; anything else is
// a decimal number (or INF or NAN), rounded to nearest even. A decimal
// extended precision constant repeats its sign and exponent word in the
// padding word when mpwExtendedPadding is set, like MPW Asm.
// Returns the number of bytes written to out, or 0 after reporting an error.
extern size_t
m68k_EncodeFloatString(const char* text, ESize size, bool mpwExtendedPadding, uint8_t out[FPCONST_MAX_BYTES]);

// Encode the value of a floating point expression for a single, double or
// extended precision operand. The precision is that of the host's long double.
// Returns the number of bytes written to out, or 0 after reporting an error.
extern size_t
m68k_EncodeFloatValue(long double value, ESize size, bool mpwExtendedPadding, uint8_t out[FPCONST_MAX_BYTES]);

#endif
