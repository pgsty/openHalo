/*-------------------------------------------------------------------------
 * mys_prefix.h
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *-------------------------------------------------------------------------
 */
/* Exact MySQL prefix indexes; no changes to PostgreSQL's btree opclasses. */
#ifndef MYS_PREFIX_H
#define MYS_PREFIX_H

#include "utils/relcache.h"

#define MYS_PREFIX_AM "mysql_prefix"
extern void mys_prefix_init(void);
extern bool mys_prefix_available(void);
extern bool mys_prefix_type_available(Oid typid);
extern const char *mys_prefix_collation_name(bool binary);
extern int mys_prefix_length(Relation index, int column);
extern bool mys_prefix_is_index(Relation index);

/* Non-PAD SPACE units for substring matching. Resolve the collation once,
 * then stream valid UTF-8 (including NUL) from a character-boundary offset.
 * Each successful next call consumes exactly one character, with no catalog
 * lookup or allocation. Trailing spaces are preserved. False means EOF;
 * malformed/truncated UTF-8 is rejected before reading beyond byte_len.
 * Other collations/encodings require the caller's generic search path. */
typedef enum MysSimpleCollation
{
	MYS_SIMPLE_UNSUPPORTED = 0,
	MYS_SIMPLE_GENERAL_CI = 1,
	MYS_SIMPLE_BIN = 2
} MysSimpleCollation;

extern MysSimpleCollation mys_simple_collation(Oid collid);
extern bool mys_simple_weight_next(MysSimpleCollation kind, const char *data,
								   int byte_len, int *offset, uint32 *weight);

/* Binary type workers: register a bytea-layout type with
 * mysql.register_prefix_binary(regtype). No type OIDs are hard-coded here. */
#endif
