/*-------------------------------------------------------------------------
 *
 * parserapi.h
 *		API for parser
 *
 * This is the external API for the raw lexing/parsing functions.
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * src/include/parser/parserapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSERHELPER_H
#define PARSERHELPER_H

#include "nodes/parsenodes.h"

/* From gram.y */
extern TypeName *SystemTypeName(char *name);
extern List *SystemFuncName(char *name);

#endif							/* PARSERHELPER_H */