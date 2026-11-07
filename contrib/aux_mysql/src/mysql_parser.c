/*-------------------------------------------------------------------------
 *
 * mysql_parser.c
 *	  Entry point of the MySQL dialect parser.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parser.h"
#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"
#include "mys_gramparse.h"

#include "halo_mysql.h"

#ifdef HALO_MYSQL_GRAMMAR
extern List *mys_raw_parser(const char *str);
#endif

/*
 * PostgreSQL validates COM_QUERY as text before invoking the parser hook.
 * Use our actual lexer to hex-encode only _binary string tokens first;
 * comments, identifiers and text strings still undergo normal validation.
 * Keeping one lexer also keeps quoting and SQL mode interpretation identical.
 */
char *
mys_normalize_query(const char *query, int len)
{
	mys_yy_extra_type extra;
	core_yyscan_t scanner;
	core_YYSTYPE value;
	YYLTYPE loc;
	StringInfoData out;
	int copied = 0;

	if (memchr(query, '\0', len))
		ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
						errmsg("NUL in query text; use a binary parameter or hexadecimal literal")));
	if (pg_verify_mbstr(GetDatabaseEncoding(), query, len, true))
		return pnstrdup(query, len);
	memset(&extra, 0, sizeof(extra));
	scanner = mys_scanner_init(query, &extra.core_yy_extra,
							  &MysScanKeywords, MysScanKeywordTokens);
	initStringInfo(&out);
	for (;;)
	{
		extra.binary_token_end = 0;
		if (mys_core_yylex(&value, &loc, scanner) == 0)
			break;
		if (extra.binary_token_end > 0)
		{
			int n = atoi(value.str);
			const unsigned char *bytes = (const unsigned char *) value.str + 16;

			appendBinaryStringInfo(&out, query + copied, loc - copied);
			/* The introducer may touch the quote: _binary'...' is valid. */
			appendStringInfoString(&out, " X'");
			for (int i = 0; i < n; i++)
				appendStringInfo(&out, "%02x", bytes[i]);
			appendStringInfoChar(&out, '\'');
			copied = extra.binary_token_end;
		}
	}
	mys_scanner_finish(scanner);
	appendBinaryStringInfo(&out, query + copied, len - copied);
	return out.data;
}

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
