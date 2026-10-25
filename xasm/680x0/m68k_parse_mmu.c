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

// 68030 on-chip PMMU instructions

#include <stdbool.h>
#include <string.h>

#include "errors.h"
#include "expression.h"
#include "lexer_context.h"
#include "options.h"
#include "parse.h"
#include "section.h"
#include "tokens.h"
#include "util.h"

#include "m68k_errors.h"
#include "m68k_options.h"
#include "m68k_parse.h"
#include "m68k_tokens.h"

#define MMU_INS 0xF000u

// The 68030 only accepts control alterable modes for all PMMU instructions
#define AM_MMU_EA (AM_AIND | AM_ADISP | AM_AXDISP | AM_WORD | AM_LONG | AM_AXDISP020 | AM_PREINDAXD020 | AM_POSTINDAXD020)

// SIZE_DOUBLE doubles as the quad word size used by CRP and SRP
#define SIZE_QUAD SIZE_DOUBLE

typedef struct {
	ETargetToken token;
	uint16_t code;
	ESize size;
	bool allowFlushDisable;
} SMmuRegister;

static const SMmuRegister g_mmuRegisters[] = {
    {T_68K_REG_TC,    0x4000, SIZE_LONG, true },
    {T_68K_REG_SRP,   0x4800, SIZE_QUAD, true },
    {T_68K_REG_CRP,   0x4C00, SIZE_QUAD, true },
    {T_68K_REG_TT0,   0x0800, SIZE_LONG, true },
    {T_68K_REG_TT1,   0x0C00, SIZE_LONG, true },
    {T_68K_REG_MMUSR, 0x6000, SIZE_WORD, false},
};

static const SMmuRegister*
getMmuRegister(SAddressingMode* mode) {
	if (mode->mode != AM_SYSREG)
		return NULL;

	for (size_t i = 0; i < sizeof(g_mmuRegisters) / sizeof(g_mmuRegisters[0]); ++i) {
		if (g_mmuRegisters[i].token == mode->directRegister)
			return &g_mmuRegisters[i];
	}

	return NULL;
}

static ESize
getSizeSpecifier(void) {
	if (lex_Context->token.id == T_ID && strlen(lex_Context->token.value.string) == 2 &&
	    _strnicmp(lex_Context->token.value.string, ".q", 2) == 0) {
		parse_GetToken();
		return SIZE_QUAD;
	}

	return m68k_GetSizeSpecifier(SIZE_DEFAULT);
}

#define MAX_OPERANDS 4

typedef struct {
	int count;
	SAddressingMode modes[MAX_OPERANDS];
} SOperands;

static bool
getOperands(SOperands* operands) {
	operands->count = 0;
	do {
		if (operands->count == MAX_OPERANDS) {
			err_Error(ERROR_OPERAND);
			return false;
		}

		if (!m68k_GetAddressingMode(&operands->modes[operands->count++], false)) {
			err_Error(ERROR_OPERAND);
			return false;
		}
	} while (lex_Context->token.id == ',' && (parse_GetToken(), true));

	return true;
}

static bool
checkOperandCount(SOperands* operands, int low, int high) {
	if (operands->count < low || operands->count > high) {
		err_Error(ERROR_OPERAND);
		return false;
	}

	return true;
}

static bool
isEffectiveAddress(SAddressingMode* mode) {
	if ((mode->mode & AM_MMU_EA) == 0) {
		err_Error(ERROR_OPERAND);
		return false;
	}

	return true;
}

static SExpression*
getImmediate(SAddressingMode* mode, int32_t low, int32_t high) {
	if (mode->mode != AM_IMM) {
		err_Error(ERROR_OPERAND);
		return NULL;
	}

	SExpression* result = expr_CheckRange(mode->immediateInteger, low, high);
	mode->immediateInteger = NULL;
	return result;
}

// Function code field: SFC, DFC, Dn or #imm
static SExpression*
getFunctionCode(SAddressingMode* mode) {
	if (mode->mode == AM_SYSREG && mode->directRegister == T_68K_REG_SFC)
		return expr_Const(0x00);

	if (mode->mode == AM_SYSREG && mode->directRegister == T_68K_REG_DFC)
		return expr_Const(0x01);

	if (mode->mode == AM_DREG && mode->directRegisterBank == BANK_0)
		return expr_Const(0x08 | mode->directRegister);

	if (mode->mode == AM_IMM) {
		SExpression* fc = getImmediate(mode, 0, 7);
		return fc != NULL ? expr_Or(expr_Const(0x10), fc) : NULL;
	}

	err_Error(ERROR_OPERAND);
	return NULL;
}

static void
outputInstruction(SAddressingMode* ea, SExpression* extension) {
	sect_OutputConst16(MMU_INS | (ea != NULL ? m68k_GetEffectiveAddressField(ea) : 0));
	sect_OutputExpr16(extension);
	if (ea != NULL)
		m68k_OutputExtensionWords(ea);
}

static bool
handlePFLUSHA(SOperands* operands) {
	if (!checkOperandCount(operands, 0, 0))
		return true;

	outputInstruction(NULL, expr_Const(0x2400));
	return true;
}

// PFLUSH fc,#mask[,<ea>]
static bool
handlePFLUSH(SOperands* operands) {
	if (!checkOperandCount(operands, 2, 3))
		return true;

	SAddressingMode* ea = operands->count == 3 ? &operands->modes[2] : NULL;
	if (ea != NULL && !isEffectiveAddress(ea))
		return true;

	SExpression* fc = getFunctionCode(&operands->modes[0]);
	SExpression* mask = getImmediate(&operands->modes[1], 0, 7);
	if (fc == NULL || mask == NULL) {
		expr_Free(fc);
		expr_Free(mask);
		return true;
	}

	SExpression* extension = expr_Or(expr_Const(ea != NULL ? 0x3800 : 0x3000), expr_Or(expr_Asl(mask, expr_Const(5)), fc));
	outputInstruction(ea, extension);
	return true;
}

// PLOADR/PLOADW fc,<ea>
static bool
handlePLOAD(SOperands* operands, bool read) {
	if (!checkOperandCount(operands, 2, 2) || !isEffectiveAddress(&operands->modes[1]))
		return true;

	SExpression* fc = getFunctionCode(&operands->modes[0]);
	if (fc == NULL)
		return true;

	outputInstruction(&operands->modes[1], expr_Or(expr_Const(read ? 0x2200 : 0x2000), fc));
	return true;
}

// PTESTR/PTESTW fc,<ea>,#level[,An]
static bool
handlePTEST(SOperands* operands, bool read) {
	if (!checkOperandCount(operands, 3, 4) || !isEffectiveAddress(&operands->modes[1]))
		return true;

	uint16_t addressRegister = 0;
	if (operands->count == 4) {
		SAddressingMode* reg = &operands->modes[3];
		if (reg->mode != AM_AREG || reg->directRegisterBank != BANK_0) {
			err_Error(ERROR_OPERAND);
			return true;
		}
		addressRegister = 0x0100 | reg->directRegister << 5;
	}

	SExpression* fc = getFunctionCode(&operands->modes[0]);
	SExpression* level = getImmediate(&operands->modes[2], 0, 7);
	if (fc == NULL || level == NULL) {
		expr_Free(fc);
		expr_Free(level);
		return true;
	}

	// Level 0 searches only the ATC, so there is no descriptor address to return
	if (addressRegister != 0 && expr_IsConstant(level) && level->value.integer == 0) {
		err_Error(ERROR_OPERAND);
		expr_Free(fc);
		expr_Free(level);
		return true;
	}

	SExpression* extension = expr_Or(expr_Const(0x8000 | (read ? 0x0200 : 0) | addressRegister),
	                                 expr_Or(expr_Asl(level, expr_Const(10)), fc));
	outputInstruction(&operands->modes[1], extension);
	return true;
}

// PMOVE <ea>,MRn / PMOVE MRn,<ea> / PMOVEFD <ea>,MRn
static bool
handlePMOVE(SOperands* operands, ESize size, bool flushDisable) {
	if (!checkOperandCount(operands, 2, 2))
		return true;

	SAddressingMode* src = &operands->modes[0];
	SAddressingMode* dest = &operands->modes[1];
	const SMmuRegister* mmuRegister;
	SAddressingMode* ea;
	uint16_t toMemory;

	if ((mmuRegister = getMmuRegister(dest)) != NULL) {
		ea = src;
		toMemory = 0;
	} else if ((mmuRegister = getMmuRegister(src)) != NULL && !flushDisable) {
		ea = dest;
		toMemory = 0x0200;
	} else {
		err_Error(ERROR_OPERAND);
		return true;
	}

	if (!isEffectiveAddress(ea))
		return true;

	if (flushDisable && !mmuRegister->allowFlushDisable) {
		err_Error(ERROR_OPERAND);
		return true;
	}

	if (size != SIZE_DEFAULT && size != mmuRegister->size) {
		err_Error(MERROR_INSTRUCTION_SIZE);
		return true;
	}

	outputInstruction(ea, expr_Const(mmuRegister->code | toMemory | (flushDisable ? 0x0100 : 0)));
	return true;
}

static bool
handleToken(ETargetToken token, ESize size, SOperands* operands) {
	if (token != T_68K_PMOVE && token != T_68K_PMOVEFD && size != SIZE_DEFAULT)
		err_Warn(MERROR_IGNORING_SIZE);

	switch (token) {
		case T_68K_PFLUSH:
			return handlePFLUSH(operands);
		case T_68K_PFLUSHA:
			return handlePFLUSHA(operands);
		case T_68K_PLOADR:
			return handlePLOAD(operands, true);
		case T_68K_PLOADW:
			return handlePLOAD(operands, false);
		case T_68K_PMOVE:
			return handlePMOVE(operands, size, false);
		case T_68K_PMOVEFD:
			return handlePMOVE(operands, size, true);
		case T_68K_PTESTR:
			return handlePTEST(operands, true);
		case T_68K_PTESTW:
			return handlePTEST(operands, false);
		default:
			internalerror("Unknown MMU instruction");
	}

	return false;
}

bool
m68k_ParseMmuInstruction(void) {
	if (lex_Context->token.id < T_68K_MMU_FIRST || lex_Context->token.id > T_68K_MMU_LAST) {
		return false;
	}

	ETargetToken token = (ETargetToken) lex_Context->token.id;
	parse_GetToken();

	if (opt_Current->machineOptions->cpu != CPUF_68030) {
		err_Error(MERROR_INSTRUCTION_CPU);
		return true;
	}

	if ((sect_CurrentSize() & 1) != 0) {
		err_Error(MERROR_WORD_ALIGN);
		return true;
	}

	err_Warn(MERROR_INSTRUCTION_PRIV);

	ESize size = getSizeSpecifier();

	SOperands operands = {0};
	bool result = token == T_68K_PFLUSHA || getOperands(&operands);
	if (result)
		result = handleToken(token, size, &operands);

	for (int i = 0; i < operands.count; ++i)
		addrMode_FreeAll(&operands.modes[i]);

	return result;
}
