/*-------------------------------------------------------------------------
 *
 * adtext.h
 *		Extension API instance
 *
 *
 * Copyright (c) 2021, Jet C.X. ZHANG
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * src/include/utils/adtext.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ADTEXT_H
#define ADTEXT_H

#include "utils/adtextapi.h"

/* ADT Extension global instance */
extern const ADTExtMethod *adtext;

/* Initialize global ADT Extension */
extern void InitADTExt(void);

/* Get Standard ADT Extension instance */
extern const ADTExtMethod *GetStandardADTExt(void);

#endif							/* ADTEXT_H */