/*-------------------------------------------------------------------------
 *
 * mysm_compat.h
 *	  PG14 -> PG18 API compatibility for code ported from openHalo.
 *
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYSM_COMPAT_H
#define MYSM_COMPAT_H

#include "postgres.h"
#include "varatt.h"				/* PG16: VARDATA & co. moved out of postgres.h */
#include "catalog/pg_collation.h"
#include "datatype/timestamp.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "utils/timestamp.h"

/* PG16 removed the Abs() macro */
#ifndef Abs
#define Abs(x)			((x) >= 0 ? (x) : -(x))
#endif

/* PG15 removed scanint8() */
static inline bool
mysm_scanint8(const char *str, bool errorOK, int64 *result)
{
	*result = pg_strtoint64(str);
	return true;
}
#define scanint8(str, errorOK, result) mysm_scanint8((str), (errorOK), (result))

/* PG17 removed lc_collate_is_c() */
static inline bool
mysm_lc_collate_is_c(Oid collation)
{
	if (collation == C_COLLATION_OID)
		return true;
	if (!OidIsValid(collation))
		return false;
	return pg_newlocale_from_collation(collation)->collate_is_c;
}
#define lc_collate_is_c(collation) mysm_lc_collate_is_c(collation)

/* PG15 replaced interval2tm() with interval2itm() */
static inline int
mysm_interval2tm(Interval span, struct pg_tm *tm, fsec_t *fsec)
{
	struct pg_itm itm;

	interval2itm(span, &itm);
	tm->tm_year = itm.tm_year;
	tm->tm_mon = itm.tm_mon;
	tm->tm_mday = itm.tm_mday;
	tm->tm_hour = (int) itm.tm_hour;
	tm->tm_min = itm.tm_min;
	tm->tm_sec = itm.tm_sec;
	*fsec = itm.tm_usec;
	return 0;
}
#define interval2tm(span, tm, fsec) mysm_interval2tm((span), (tm), (fsec))

#endif							/* MYSM_COMPAT_H */
