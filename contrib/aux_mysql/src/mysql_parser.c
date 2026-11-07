/*-------------------------------------------------------------------------
 *
 * mysql_parser.c
 *	  Entry point of the MySQL dialect parser.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parser.h"

#include "halo_mysql.h"

#ifdef HALO_MYSQL_GRAMMAR
extern List *mys_raw_parser(const char *str);
#endif

List *
mysql_raw_parser(const char *str)
{
#ifdef HALO_MYSQL_GRAMMAR
	return mys_raw_parser(str);
#else
	/* grammar not ported yet: fall back to the PostgreSQL grammar */
	return raw_parser(str, RAW_PARSE_DEFAULT);
#endif
}
