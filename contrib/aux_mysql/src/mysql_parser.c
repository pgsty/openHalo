/*-------------------------------------------------------------------------
 *
 * mysql_parser.c
 *	  Entry point of the MySQL dialect parser.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parser.h"
#include "parser/scansup.h"
#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "mys_gramparse.h"

#include "halo_mysql.h"
#include "adapter/mysql/systemVar.h"

#ifdef HALO_MYSQL_GRAMMAR
extern List *mys_raw_parser(const char *str);
#endif

/* a \0 escape: the only way a valid query text can put a NUL in a string */
static bool
has_nul_escape(const char *query, int len)
{
	if (mys_sqlMode & MYS_MODE_NO_BACKSLASH_ESCAPES)
		return false;
	for (int i = 0; i + 1 < len; i++)
	{
		if (query[i] == '\\')
		{
			if (query[i + 1] == '0')
				return true;
			i++;				/* the escaped character */
		}
	}
	return false;
}

/*
 * PostgreSQL validates COM_QUERY as text before invoking the parser hook, and
 * a PostgreSQL string cannot hold a NUL.  MySQL keeps the bytes of a string
 * literal as they are, whatever they are, so use our actual lexer to rewrite
 * as X'hex' every string literal that PostgreSQL could not take as text: the
 * _binary ones, and plain ones holding a NUL (raw or \0) or bytes that are not
 * valid in the database encoding (a plain one becomes _binary X'hex', so it
 * reaches a BLOB column unchanged and a TEXT column through the encoding
 * check of the bytea -> text cast, as in MySQL's strict mode).  Comments,
 * identifiers and the other strings still undergo normal validation, and
 * keeping one lexer keeps quoting and SQL mode interpretation identical.
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
	int			prev_token = 0;
	int			end = len;

	/* as in MySQL, a NUL at the end (a C string sent with its terminator) ends the query */
	while (end > 0 && (query[end - 1] == '\0' || scanner_isspace(query[end - 1])))
		end--;
	if (memchr(query + end, '\0', len - end))
		len = end;

	if (halo_mysql_parser == MYS_PARSER_POSTGRES)
	{
		/* PostgreSQL's lexer reads the text; it can only take valid text */
		if (memchr(query, '\0', len))
			ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
							errmsg("NUL in query text; use a binary parameter or hexadecimal literal")));
		return pnstrdup(query, len);
	}
	if (memchr(query, '\0', len) == NULL && !has_nul_escape(query, len) &&
		pg_verify_mbstr(GetDatabaseEncoding(), query, len, true))
		return pnstrdup(query, len);
	memset(&extra, 0, sizeof(extra));
	scanner = mys_scanner_init_len(query, len, &extra.core_yy_extra,
								   &MysScanKeywords, MysScanKeywordTokens);
	extra.normalizing = true;
	initStringInfo(&out);
	for (;;)
	{
		int			token;

		extra.binary_token_end = 0;
		/* a NUL outside a string literal is token 0 as well */
		if ((token = mys_core_yylex(&value, &loc, scanner)) == 0)
			break;
		if (extra.binary_token_end > 0)
		{
			int n = atoi(value.str);

			appendBinaryStringInfo(&out, query + copied, loc - copied);
			/* The introducer may touch the quote: _binary'...' is valid. */
			appendStringInfoString(&out, prev_token == UNDERSCORE_BINARY ? " X'" : " _binary X'");
			enlargeStringInfo(&out, n * 2 + 1);
			out.len += hex_encode(value.str + 16, n, out.data + out.len);
			appendStringInfoChar(&out, '\'');
			copied = extra.binary_token_end;
		}
		prev_token = token;
	}
	mys_scanner_finish(scanner);
	appendBinaryStringInfo(&out, query + copied, len - copied);
	if (memchr(out.data, '\0', out.len))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("NUL in query text outside a string literal")));
	return out.data;
}

List *
mysql_raw_parser(const char *str)
{
#ifdef HALO_MYSQL_GRAMMAR
	/* Syscache-dirty refresh: connections can span ALTER EXTENSION UPDATE. */
	mys_binary_session_init();
	return mys_raw_parser(str);
#else
	/* grammar not ported yet: fall back to the PostgreSQL grammar */
	return raw_parser(str, RAW_PARSE_DEFAULT);
#endif
}
