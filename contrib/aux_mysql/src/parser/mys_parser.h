/*-------------------------------------------------------------------------
 *
 * mys_parser.h
 *	  MySQL raw parser entry point (aux_mysql).
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSER_H
#define MYS_PARSER_H

#include "nodes/pg_list.h"

extern List *mys_raw_parser(const char *str);

#endif							/* MYS_PARSER_H */
