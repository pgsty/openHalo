/*-------------------------------------------------------------------------
 *
 * mys_parser.h
 *	  MySQL raw parser entry point (aux_mysql).
 *
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSER_H
#define MYS_PARSER_H

#include "nodes/pg_list.h"

extern List *mys_raw_parser(const char *str);

#endif							/* MYS_PARSER_H */
