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

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "errors.h"

#include "m68k_errors.h"
#include "m68k_fpconst.h"

// Decimal to binary conversion is done exactly with big integers, so every
// constant is correctly rounded regardless of the host's floating point.

#define BIG_LIMBS      640  // 20480 bits: 10^4970 plus the digits and scaling shifts
#define MAX_DIGITS     400  // significant decimal digits accepted
#define MAX_MAGNITUDE  4970 // |decimal exponent| accepted, past the extended range

typedef struct {
	uint32_t limb[BIG_LIMBS]; // least significant first
	int used;
} SBigInteger;

static void
big_Set(SBigInteger* big, uint32_t value) {
	big->limb[0] = value;
	big->used = value != 0 ? 1 : 0;
}

static bool
big_MultiplyAdd(SBigInteger* big, uint32_t multiplier, uint32_t addend) {
	uint64_t carry = addend;
	for (int i = 0; i < big->used; ++i) {
		uint64_t product = (uint64_t) big->limb[i] * multiplier + carry;
		big->limb[i] = (uint32_t) product;
		carry = product >> 32;
	}
	if (carry != 0) {
		if (big->used == BIG_LIMBS)
			return false;
		big->limb[big->used++] = (uint32_t) carry;
	}
	return true;
}

static int
big_BitLength(const SBigInteger* big) {
	if (big->used == 0)
		return 0;

	uint32_t top = big->limb[big->used - 1];
	int bits = 0;
	while (top != 0) {
		++bits;
		top >>= 1;
	}
	return (big->used - 1) * 32 + bits;
}

static bool
big_ShiftLeft(SBigInteger* big, int bits) {
	if (big->used == 0 || bits == 0)
		return true;

	int limbs = bits / 32;
	int shift = bits % 32;
	int used = big->used + limbs + 1;
	if (used > BIG_LIMBS)
		return false;

	for (int i = used - 1; i >= 0; --i) {
		int from = i - limbs;
		uint32_t high = from < big->used && from >= 0 ? big->limb[from] : 0;
		uint32_t low = from - 1 < big->used && from >= 1 ? big->limb[from - 1] : 0;
		big->limb[i] = shift == 0 ? high : (high << shift) | (low >> (32 - shift));
	}

	big->used = used;
	while (big->used > 0 && big->limb[big->used - 1] == 0)
		--big->used;
	return true;
}

static void
big_ShiftRightOne(SBigInteger* big) {
	for (int i = 0; i < big->used; ++i) {
		uint32_t next = i + 1 < big->used ? big->limb[i + 1] : 0;
		big->limb[i] = (big->limb[i] >> 1) | (next << 31);
	}
	while (big->used > 0 && big->limb[big->used - 1] == 0)
		--big->used;
}

static int
big_Compare(const SBigInteger* a, const SBigInteger* b) {
	if (a->used != b->used)
		return a->used < b->used ? -1 : 1;

	for (int i = a->used - 1; i >= 0; --i) {
		if (a->limb[i] != b->limb[i])
			return a->limb[i] < b->limb[i] ? -1 : 1;
	}
	return 0;
}

// a -= b, where a >= b
static void
big_Subtract(SBigInteger* a, const SBigInteger* b) {
	int64_t borrow = 0;
	for (int i = 0; i < a->used; ++i) {
		int64_t difference = (int64_t) a->limb[i] - (i < b->used ? b->limb[i] : 0) - borrow;
		borrow = difference < 0;
		a->limb[i] = (uint32_t) (difference + (borrow ? ((int64_t) 1 << 32) : 0));
	}
	while (a->used > 0 && a->limb[a->used - 1] == 0)
		--a->used;
}

typedef enum {
	NUMBER_FINITE,
	NUMBER_INFINITY,
	NUMBER_NAN
} ENumberKind;

typedef struct {
	ENumberKind kind;
	bool negative;
	SBigInteger digits; // value = digits * 10^exponent
	int exponent;
	int digitCount;
} SDecimal;

static bool
parseDecimal(const char* text, SDecimal* decimal) {
	const char* p = text;

	while (isspace((unsigned char) *p))
		++p;

	decimal->negative = false;
	if (*p == '+' || *p == '-')
		decimal->negative = *p++ == '-';

	size_t length = strlen(p);
	while (length > 0 && isspace((unsigned char) p[length - 1]))
		--length;

	if ((length == 3 && strncasecmp(p, "INF", 3) == 0) || (length == 8 && strncasecmp(p, "INFINITY", 8) == 0)) {
		decimal->kind = NUMBER_INFINITY;
		return true;
	}
	if (length == 3 && strncasecmp(p, "NAN", 3) == 0) {
		decimal->kind = NUMBER_NAN;
		return true;
	}

	const char* end = p + length;
	decimal->kind = NUMBER_FINITE;
	decimal->exponent = 0;
	decimal->digitCount = 0;
	big_Set(&decimal->digits, 0);

	bool anyDigits = false;
	bool fraction = false;
	for (; p < end; ++p) {
		if (*p == '.' && !fraction) {
			fraction = true;
		} else if (isdigit((unsigned char) *p)) {
			anyDigits = true;
			if (decimal->digitCount == 0 && *p == '0') {
				if (fraction)
					--decimal->exponent;
				continue;
			}
			if (++decimal->digitCount > MAX_DIGITS)
				return false;
			big_MultiplyAdd(&decimal->digits, 10, (uint32_t) (*p - '0'));
			if (fraction)
				--decimal->exponent;
		} else {
			break;
		}
	}

	if (!anyDigits)
		return false;

	if (p < end && (*p == 'e' || *p == 'E')) {
		++p;
		bool negativeExponent = false;
		if (p < end && (*p == '+' || *p == '-'))
			negativeExponent = *p++ == '-';

		if (p == end)
			return false;

		int exponent = 0;
		for (; p < end && isdigit((unsigned char) *p); ++p) {
			exponent = exponent * 10 + (*p - '0');
			if (exponent > 100000)
				return false;
		}
		decimal->exponent += negativeExponent ? -exponent : exponent;
	}

	return p == end;
}

static int
bitLength128(unsigned __int128 value) {
	int bits = 0;
	while (value != 0) {
		++bits;
		value >>= 1;
	}
	return bits;
}

// Round a finite, non-zero decimal to precision bits. The result is
// mantissa * 2^(exponent - (precision - 1)) with mantissa < 2^precision; it is
// normalized when the top bit of the mantissa is set, otherwise denormalized
// with exponent == minExponent.
static bool
roundDecimal(const SDecimal* decimal, int precision, int minExponent, uint64_t* outMantissa, int* outExponent) {
	static SBigInteger numerator;
	static SBigInteger denominator;
	static SBigInteger divisor;

	numerator = decimal->digits;
	big_Set(&denominator, 1);

	SBigInteger* scaled = decimal->exponent >= 0 ? &numerator : &denominator;
	for (int i = abs(decimal->exponent); i > 0; --i) {
		if (!big_MultiplyAdd(scaled, 10, 0))
			return false;
	}

	// Scale so the quotient has precision + 2 or precision + 3 bits
	int shift = (precision + 2) - (big_BitLength(&numerator) - big_BitLength(&denominator));
	if (!big_ShiftLeft(shift > 0 ? &numerator : &denominator, abs(shift)))
		return false;

	divisor = denominator;
	if (!big_ShiftLeft(&divisor, precision + 3))
		return false;

	unsigned __int128 quotient = 0;
	for (int bit = precision + 3; bit >= 0; --bit) {
		quotient <<= 1;
		if (big_Compare(&numerator, &divisor) >= 0) {
			big_Subtract(&numerator, &divisor);
			quotient |= 1;
		}
		big_ShiftRightOne(&divisor);
	}

	bool sticky = numerator.used != 0;
	int length = bitLength128(quotient);
	int exponent = length - 1 - shift;
	int drop = length - precision;

	if (exponent < minExponent) {
		drop += minExponent - exponent;
		exponent = minExponent;
	}

	unsigned __int128 mantissa = 0;
	if (drop < 127) {
		unsigned __int128 half = (unsigned __int128) 1 << (drop - 1);
		unsigned __int128 remainder = quotient & ((half << 1) - 1);
		mantissa = quotient >> drop;
		if (remainder > half || (remainder == half && (sticky || (mantissa & 1))))
			++mantissa;
	}

	if (mantissa >> precision) {
		mantissa >>= 1;
		++exponent;
	}

	*outMantissa = (uint64_t) mantissa;
	*outExponent = exponent;
	return true;
}

static void
putBits(uint8_t* out, size_t bytes, uint64_t value) {
	for (size_t i = 0; i < bytes; ++i)
		out[i] = (uint8_t) (value >> (8 * (bytes - 1 - i)));
}

static size_t
operandBytes(ESize size) {
	switch (size) {
		case SIZE_BYTE:
		case SIZE_WORD:
			return 2;
		case SIZE_LONG:
		case SIZE_SINGLE:
			return 4;
		case SIZE_DOUBLE:
			return 8;
		case SIZE_EXTENDED:
		case SIZE_PACKED:
			return 12;
		default:
			return 0;
	}
}

static size_t
encodeBitPattern(const char* hex, size_t bytes, uint8_t* out) {
	size_t digits = strlen(hex);
	if (digits == 0 || digits > bytes * 2)
		return 0;

	memset(out, 0, bytes);
	for (size_t i = 0; i < digits; ++i) {
		char c = hex[digits - 1 - i];
		if (!isxdigit((unsigned char) c))
			return 0;

		uint8_t nibble = (uint8_t) (isdigit((unsigned char) c) ? c - '0' : toupper((unsigned char) c) - 'A' + 10);
		out[bytes - 1 - i / 2] |= (uint8_t) (nibble << (4 * (i % 2)));
	}
	return bytes;
}

static size_t
encodeInteger(const char* text, ESize size, uint8_t* out) {
	char* end;
	long long value = strtoll(text, &end, 10);
	while (isspace((unsigned char) *end))
		++end;

	if (end == text || *end != 0)
		return err_Error(MERROR_FPU_CONSTANT), 0;

	long long low = size == SIZE_BYTE ? -128 : size == SIZE_WORD ? -32768 : INT32_MIN;
	long long high = size == SIZE_BYTE ? 255 : size == SIZE_WORD ? 65535 : UINT32_MAX;
	if (value < low || value > high)
		return err_Error(ERROR_OPERAND_RANGE), 0;

	size_t bytes = operandBytes(size);
	putBits(out, bytes, (uint64_t) value & (size == SIZE_BYTE ? 0xFF : size == SIZE_WORD ? 0xFFFF : 0xFFFFFFFF));
	return bytes;
}

// IEEE single or double precision
static size_t
encodeIeee(const SDecimal* decimal, int precision, int exponentBits, uint8_t* out) {
	int bias = (1 << (exponentBits - 1)) - 1;
	int fractionBits = precision - 1;
	size_t bytes = (size_t) (1 + exponentBits + fractionBits) / 8;
	uint64_t sign = (uint64_t) decimal->negative << (exponentBits + fractionBits);
	uint64_t maxExponent = ((uint64_t) 1 << exponentBits) - 1;

	if (decimal->kind == NUMBER_INFINITY) {
		putBits(out, bytes, sign | (maxExponent << fractionBits));
		return bytes;
	}
	if (decimal->kind == NUMBER_NAN) {
		putBits(out, bytes, sign | (maxExponent << fractionBits) | ((uint64_t) 1 << (fractionBits - 1)));
		return bytes;
	}
	if (decimal->digits.used == 0) {
		putBits(out, bytes, sign);
		return bytes;
	}

	uint64_t mantissa;
	int exponent;
	if (!roundDecimal(decimal, precision, 1 - bias, &mantissa, &exponent) || exponent > bias)
		return err_Error(ERROR_OPERAND_RANGE), 0;

	bool normal = (mantissa >> fractionBits) & 1;
	uint64_t biased = normal ? (uint64_t) (exponent + bias) : 0;
	putBits(out, bytes, sign | (biased << fractionBits) | (mantissa & (((uint64_t) 1 << fractionBits) - 1)));
	return bytes;
}

// 68881 extended precision: sign and exponent word, padding word, 64 bit mantissa
// with an explicit integer bit
static size_t
encodeExtended(const SDecimal* decimal, bool mpwPadding, uint8_t* out) {
	uint16_t sign = decimal->negative ? 0x8000 : 0;

	memset(out, 0, 12);
	if (decimal->kind != NUMBER_FINITE) {
		putBits(out, 2, sign | 0x7FFF);
		if (decimal->kind == NUMBER_NAN)
			putBits(out + 4, 8, 0xC000000000000000);
		return 12;
	}

	uint16_t exponentWord = sign;
	uint64_t mantissa = 0;
	if (decimal->digits.used != 0) {
		int exponent;
		if (!roundDecimal(decimal, 64, -16382, &mantissa, &exponent) || exponent > 16383)
			return err_Error(ERROR_OPERAND_RANGE), 0;

		if ((mantissa >> 63) == 0)
			return err_Error(MERROR_FPU_CONSTANT_PATTERN), 0;

		exponentWord |= (uint16_t) (exponent + 16383);
	}

	putBits(out, 2, exponentWord);
	putBits(out + 2, 2, mpwPadding ? exponentWord : 0);
	putBits(out + 4, 8, mantissa);
	return 12;
}

size_t
m68k_EncodeFloatString(const char* text, ESize size, bool mpwExtendedPadding, uint8_t out[FPCONST_MAX_BYTES]) {
	size_t bytes = operandBytes(size);
	if (bytes == 0)
		return err_Error(MERROR_INSTRUCTION_SIZE), 0;

	if (text[0] == '$') {
		if (encodeBitPattern(text + 1, size == SIZE_BYTE ? 1 : bytes, size == SIZE_BYTE ? out + 1 : out) == 0)
			return err_Error(MERROR_FPU_CONSTANT), 0;
		if (size == SIZE_BYTE)
			out[0] = 0;
		return bytes;
	}

	if (size == SIZE_BYTE || size == SIZE_WORD || size == SIZE_LONG)
		return encodeInteger(text, size, out);

	static SDecimal decimal;
	if (!parseDecimal(text, &decimal))
		return err_Error(MERROR_FPU_CONSTANT), 0;

	if (decimal.kind == NUMBER_FINITE && decimal.digits.used != 0 &&
	    abs(decimal.exponent + decimal.digitCount) > MAX_MAGNITUDE)
		return err_Error(ERROR_OPERAND_RANGE), 0;

	switch (size) {
		case SIZE_SINGLE:
			return encodeIeee(&decimal, 24, 8, out);
		case SIZE_DOUBLE:
			return encodeIeee(&decimal, 53, 11, out);
		case SIZE_EXTENDED:
			return encodeExtended(&decimal, mpwExtendedPadding, out);
		default:
			// packed decimal constants only as bit patterns
			return err_Error(MERROR_FPU_CONSTANT_PATTERN), 0;
	}
}
