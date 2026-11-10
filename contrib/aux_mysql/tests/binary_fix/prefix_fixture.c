/*-------------------------------------------------------------------------
 * prefix_fixture.c
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *-------------------------------------------------------------------------
 */
/* Compile the real collation implementation into the private test bundle.
 * Its standalone mode excludes unrelated metadata integration, while this
 * bundle's existing shim supplies PG_MODULE_MAGIC and calls mys_prefix_init.
 * The source is selected through build.py's read-only include path. */
#include "postgres.h"
#include "fmgr.h"
#define MYS_PREFIX_STANDALONE
#undef PG_MODULE_MAGIC
#define PG_MODULE_MAGIC
#define _PG_init review_fix_prefix_module_init
#include "mys_prefix.c"
