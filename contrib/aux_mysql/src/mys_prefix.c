/*-------------------------------------------------------------------------
 * Exact MySQL prefix indexes on PostgreSQL 18.
 *
 * The physical tree contains bounded, ordered candidate keys. Its entries
 * are NEVER uniqueness evidence. A dirty-snapshot scan fetches the original
 * heap values, evaluates every declared prefix and compares every key part.
 * This also supplies exact conflict TIDs to ExecCheckIndexConstraints.
 *
 * We reuse btree's pages, WAL, splits, vacuum and crash recovery, but disable
 * index-only scans, ordering, deduplication, parallel scans and skip support.
 * A transaction-owned lock on the normalized candidate key closes the
 * check/publish race. It is released after the entry is visible in the tree;
 * dirty-snapshot XID/speculative-token waits then protect uncommitted rows.
 * Bulk inserts therefore do not accumulate one lock per row. Unrelated keys
 * proceed concurrently. Hash collisions only over-serialize, never compare.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "access/amapi.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/relation.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "commands/defrem.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_utilcmd.h"
#include "tcop/utility.h"
#include "storage/lmgr.h"
#include "storage/bulk_write.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "mys_prefix.h"
#include "mys_general_ci_weights.h"

#ifdef MYS_PREFIX_STANDALONE
PG_MODULE_MAGIC;
void _PG_init(void);
void _PG_init(void) { mys_prefix_init(); }
#endif

typedef struct PrefixOptions
{
	int32 vl_len_;
	int length;                 /* zero means the entire column */
} PrefixOptions;

typedef struct PrefixIndexOptions
{
	BTOptions btree;
	int lengths_offset;
} PrefixIndexOptions;

static relopt_kind prefix_relopt_kind;

typedef struct PrefixScan
{
	IndexScanDesc tree;
	MemoryContext context;
	ScanKey original;
	int nkeys;
	IndexInfo *info;
	EState *estate;
	TupleTableSlot *slot;
	struct IndexFetchTableData *fetch;
} PrefixScan;

static post_parse_analyze_hook_type previous_analyze;
#ifdef INDEX_CONSTRAINT_TRANSFORM_HOOK
static index_constraint_transform_hook_type previous_constraint_transform;
#else
static ProcessUtility_hook_type previous_utility;
#endif
#ifdef PG_LOCALE_CREATE_HOOK
static pg_locale_create_hook_type previous_locale_create;
#else
/* Standalone PG18 test fallback; production uses the locale creation hook. */
static ExecutorStart_hook_type previous_executor_start;
#endif
static bool initialized;

/* Resolve on each statement, never cache a missing (or dropped) object. The
 * normal syscache invalidations make an ALTER EXTENSION in another session
 * visible without reconnecting. New binaries can run with the 1.8 SQL. */
bool
mys_prefix_available(void)
{
	return OidIsValid(get_am_oid(MYS_PREFIX_AM, true));
}

bool
mys_prefix_type_available(Oid typid)
{
	Oid am = get_am_oid(MYS_PREFIX_AM, true);
	return OidIsValid(am) && OidIsValid(typid) &&
		OidIsValid(GetDefaultOpClass(getBaseType(typid), am));
}

const char *
mys_prefix_collation_name(bool binary)
{
	const char *name = binary ? "utf8mb4_bin" : "utf8mb4_general_ci";
	Oid collation = get_collation_oid(list_make2(makeString("mysql"),
									 makeString(pstrdup(name))), true);
	return OidIsValid(collation) ? name : binary ? "case_sensitive" : "case_insensitive";
}

#define PREFIX_LIKE_STRATEGY 6
#define PREFIX_PG_LIKE_STRATEGY 7

static uint32
general_weight(uint32 ch)
{
	int lo = 0, hi = lengthof(mys_general_weights);

	if (ch > 0xffff)
		return 0xfffd;
	while (lo < hi)
	{
		int mid = lo + (hi - lo) / 2;
		if (mys_general_weights[mid].code < ch)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < (int) lengthof(mys_general_weights) && mys_general_weights[lo].code == ch ?
		mys_general_weights[lo].weight : ch;
}

/* Fixed-width big-endian weights preserve MySQL's simple character order.
 * Truncate characters BEFORE applying PAD SPACE, including CHAR(n) values. */
static bytea *
general_key(const char *s, int len, int characters, bool pad)
{
	bytea *out = palloc(VARHDRSZ + 2 * len);
	unsigned char *dst = (unsigned char *) VARDATA(out);
	const unsigned char *p = (const unsigned char *) s;
	const unsigned char *end = p + len;
	int n = 0;

	while (p < end && (characters <= 0 || n < characters))
	{
		uint32 weight = general_weight(utf8_to_unicode(p));
		*dst++ = weight >> 8;
		*dst++ = weight & 255;
		p += pg_utf_mblen(p);
		n++;
	}
	if (pad)
		while (dst > (unsigned char *) VARDATA(out) && dst[-2] == 0 && dst[-1] == 32)
			dst -= 2;
	SET_VARSIZE(out, VARHDRSZ + dst - (unsigned char *) VARDATA(out));
	return out;
}

static int
bytes_compare(const bytea *a, const bytea *b)
{
	int alen = VARSIZE_ANY_EXHDR(a), blen = VARSIZE_ANY_EXHDR(b);
	int cmp = memcmp(VARDATA_ANY(a), VARDATA_ANY(b), Min(alen, blen));
	return cmp ? cmp : (alen > blen) - (alen < blen);
}

/* PAD SPACE pads the shorter operand while comparing; merely trimming then
 * comparing lengths gives the wrong order for NUL/control characters. */
static int
pad_compare(const char *a, int alen, const char *b, int blen, int unit)
{
	int common = Min(alen, blen);
	int cmp = memcmp(a, b, common);
	const unsigned char *tail;
	int len, sign;
	if (cmp || alen == blen) return cmp;
	tail = (const unsigned char *) (alen > blen ? a : b);
	len = Max(alen, blen);
	sign = alen > blen ? 1 : -1;
	for (int i = common; i < len; i += unit)
	{
		int value = unit == 2 ? ((int) tail[i] << 8) | tail[i + 1] : tail[i];
		if (value != 32) return sign * (value < 32 ? -1 : 1);
	}
	return 0;
}

static int
general_strncoll(const char *a, ssize_t alen, const char *b, ssize_t blen,
				pg_locale_t locale)
{
	bytea *ak = general_key(a, alen < 0 ? strlen(a) : alen, 0, true);
	bytea *bk = general_key(b, blen < 0 ? strlen(b) : blen, 0, true);
	int cmp = pad_compare(VARDATA(ak), VARSIZE_ANY_EXHDR(ak),
						  VARDATA(bk), VARSIZE_ANY_EXHDR(bk), 2);
	pfree(ak);
	pfree(bk);
	return cmp;
}

/* pg_strnxfrm's result is binary and length-delimited (embedded NUL is OK). */
static size_t
general_strnxfrm(char *dest, size_t size, const char *src, ssize_t len,
				pg_locale_t locale)
{
	bytea *key = general_key(src, len < 0 ? strlen(src) : len, 0, true);
	size_t n = VARSIZE_ANY_EXHDR(key);
	if (size > n)
	{
		memcpy(dest, VARDATA(key), n);
		dest[n] = '\0';
	}
	pfree(key);
	return n;
}

static const struct collate_methods general_methods = {
	general_strncoll, general_strnxfrm, NULL, false
};

static int
mysql_collation_kind(Oid collid)
{
	HeapTuple tuple;
	Form_pg_collation coll;
	int result = 0;
	if (!OidIsValid(collid))
		return false;
	tuple = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(tuple)) return false;
	coll = (Form_pg_collation) GETSTRUCT(tuple);
	if (coll->collnamespace == get_namespace_oid("mysql", true))
	{
		if (strcmp(NameStr(coll->collname), "utf8mb4_general_ci") == 0) result = 1;
		if (strcmp(NameStr(coll->collname), "utf8mb4_bin") == 0) result = 2;
	}
	ReleaseSysCache(tuple);
	return result;
}

static bool is_general(Oid collid) { return mysql_collation_kind(collid) == 1; }
static bool is_mysql_bin(Oid collid) { return mysql_collation_kind(collid) == 2; }

MysSimpleCollation
mys_simple_collation(Oid collid)
{
	if (GetDatabaseEncoding() != PG_UTF8)
		return MYS_SIMPLE_UNSUPPORTED;
	return (MysSimpleCollation) mysql_collation_kind(collid);
}

bool
mys_simple_weight_next(MysSimpleCollation kind, const char *data, int byte_len,
					   int *offset, uint32 *weight)
{
	const unsigned char *p;
	int width;
	uint32 codepoint;

	if (*offset >= byte_len)
		return false;
	if (*offset < 0 || (kind != MYS_SIMPLE_GENERAL_CI && kind != MYS_SIMPLE_BIN))
		elog(ERROR, "invalid simple collation iterator state");
	p = (const unsigned char *) data + *offset;
	/* PostgreSQL's UTF-8 verifier rejects NUL; text_string permits it. The
	 * remaining input length bounds verification before utf8_to_unicode. */
	width = *p == 0 ? 1 : pg_encoding_verifymbchar(PG_UTF8, (const char *) p,
												byte_len - *offset);
	if (width < 0)
		report_invalid_encoding(PG_UTF8, (const char *) p, byte_len - *offset);
	codepoint = utf8_to_unicode(p);
	*weight = kind == MYS_SIMPLE_GENERAL_CI ? general_weight(codepoint) : codepoint;
	*offset += width;
	return true;
}

static int
binary_strncoll(const char *a, ssize_t alen, const char *b, ssize_t blen,
			   pg_locale_t locale)
{
	if (alen < 0) alen = strlen(a);
	if (blen < 0) blen = strlen(b);
	return pad_compare(a, alen, b, blen, 1);
}

static size_t
binary_strnxfrm(char *dest, size_t size, const char *src, ssize_t len,
				pg_locale_t locale)
{
	if (len < 0) len = strlen(src);
	while (len > 0 && src[len - 1] == ' ') len--;
	if (size > (size_t) len)
	{
		memcpy(dest, src, len);
		dest[len] = '\0';
	}
	return len;
}

static const struct collate_methods binary_methods = {
	binary_strncoll, binary_strnxfrm, NULL, false
};

#ifdef PG_LOCALE_CREATE_HOOK
static void
prefix_locale_create(Oid collid, pg_locale_t locale)
{
	int kind;
	if (previous_locale_create) previous_locale_create(collid, locale);
	kind = mysql_collation_kind(collid);
	if (!kind) return;
	locale->collate = kind == 1 ? &general_methods : &binary_methods;
	locale->deterministic = false;
	locale->collate_is_c = false;
}
#endif

/* Only our NEW, explicitly named collation is affected. Do not alter the
 * existing case_insensitive collation: that would invalidate old indexes.
 * aux_mysql is preloaded in every backend; the analyze hook installs these
 * methods before constant folding, hashing, comparison, or index building.
 * Locale invalidation is handled by installing again for each statement. */
#ifndef PG_LOCALE_CREATE_HOOK
static void
install_collation(void)
{
	static const char *names[] = {"utf8mb4_general_ci", "utf8mb4_bin"};
	if (!IsTransactionState() || !OidIsValid(get_namespace_oid("mysql", true)))
		return;
	for (int i = 0; i < (int) lengthof(names); i++)
	{
		Oid collid = get_collation_oid(list_make2(makeString("mysql"), makeString(pstrdup(names[i]))), true);
		pg_locale_t locale;
		if (!OidIsValid(collid)) continue;
		locale = pg_newlocale_from_collation(collid);
		locale->collate = i == 0 ? &general_methods : &binary_methods;
		locale->deterministic = false;
		locale->collate_is_c = false;
	}
}
#endif

PG_FUNCTION_INFO_V1(mys_prefix_options);
Datum
mys_prefix_options(PG_FUNCTION_ARGS)
{
	local_relopts *opts = (local_relopts *) PG_GETARG_POINTER(0);
	init_local_reloptions(opts, sizeof(PrefixOptions));
	add_local_int_reloption(opts, "mysql_prefix", "MySQL prefix length (characters or bytes)",
							0, 0, INT_MAX, offsetof(PrefixOptions, length));
	PG_RETURN_VOID();
}

bool
mys_prefix_is_index(Relation index)
{
	char *name = get_am_name(index->rd_rel->relam);
	bool result = name && strcmp(name, MYS_PREFIX_AM) == 0;
	if (name) pfree(name);
	return result;
}

int
mys_prefix_length(Relation index, int column)
{
	PrefixOptions *opts;
	PrefixIndexOptions *indexopts = (PrefixIndexOptions *) index->rd_options;
	if (indexopts && indexopts->lengths_offset)
	{
		const char *p = (char *) indexopts + indexopts->lengths_offset;
		for (int i = 0; i < column && p; i++)
		{
			p = strchr(p, ',');
			if (p) p++;
		}
		if (p) return atoi(p);
	}
	/* Force loading opclass options through the public support-proc API. */
	(void) index_getprocinfo(index, column + 1, BTORDER_PROC);
	opts = index->rd_opcoptions ? (PrefixOptions *) index->rd_opcoptions[column] : NULL;
	return opts ? opts->length : 0;
}

static void
validate_lengths(const char *value)
{
	const char *p = value;
	if (!p || !*p) return;
	for (;;)
	{
		char *end;
		long n = strtol(p, &end, 10);
		if (end == p || n < 0 || n > INT_MAX || (*end && *end != ','))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid prefix_lengths")));
		if (!*end) return;
		p = end + 1;
	}
}

static bytea *
prefix_reloptions(Datum options, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(PrefixIndexOptions, btree.fillfactor)},
		{"vacuum_cleanup_index_scale_factor", RELOPT_TYPE_REAL,
			offsetof(PrefixIndexOptions, btree.vacuum_cleanup_index_scale_factor)},
		{"deduplicate_items", RELOPT_TYPE_BOOL, offsetof(PrefixIndexOptions, btree.deduplicate_items)},
		{"prefix_lengths", RELOPT_TYPE_STRING, offsetof(PrefixIndexOptions, lengths_offset)}
	};
	PrefixIndexOptions *result = (PrefixIndexOptions *) build_reloptions(options, validate,
		prefix_relopt_kind | RELOPT_KIND_BTREE, sizeof(PrefixIndexOptions), tab, lengthof(tab));
	if (result) result->btree.deduplicate_items = false;
	return (bytea *) result;
}

static bool
string_part(Relation index, int col)
{
	/* Our string opclasses alone use STORAGE bytea and an options proc. */
	return OidIsValid(index_getprocid(index, col + 1, BTOPTIONS_PROC));
}

static bytea *
string_key(Relation index, int col, Datum value, bool physical, bool pattern)
{
	struct varlena *v = PG_DETOAST_DATUM_PACKED(value);
	const char *data = VARDATA_ANY(v);
	int len = VARSIZE_ANY_EXHDR(v);
	int n = mys_prefix_length(index, col);
	Oid collid = index->rd_indcollation[col];
	bytea *key;

	if (is_general(collid))
		key = general_key(data, len, n, !pattern);
	else if (is_mysql_bin(collid))
	{
		if (n > 0) len = pg_mbcharcliplen(data, len, n);
		if (!pattern) while (len > 0 && data[len - 1] == ' ') len--;
		key = palloc(VARHDRSZ + len);
		SET_VARSIZE(key, VARHDRSZ + len);
		memcpy(VARDATA(key), data, len);
	}
	else if (OidIsValid(collid))
	{
		pg_locale_t locale = pg_newlocale_from_collation(collid);
		size_t needed;
		if (n > 0)
			len = pg_mbcharcliplen(data, len, n);
		if (locale->collate_is_c || locale->deterministic)
		{
			key = palloc(VARHDRSZ + len);
			SET_VARSIZE(key, VARHDRSZ + len);
			memcpy(VARDATA(key), data, len);
		}
		else
		{
			needed = pg_strnxfrm(NULL, 0, data, len, locale);
			key = palloc(VARHDRSZ + needed + 1);
			pg_strnxfrm(VARDATA(key), needed + 1, data, len, locale);
			SET_VARSIZE(key, VARHDRSZ + needed);
		}
	}
	else
	{
		if (n > 0) len = Min(len, n);
		key = palloc(VARHDRSZ + len);
		SET_VARSIZE(key, VARHDRSZ + len);
		memcpy(VARDATA(key), data, len);
	}
	if (physical)
	{
		/* Leave room for headers, null bitmap, alignment and fixed columns.
		 * Every string part gets the same safe share; no parts are dropped. */
		int limit = 1536 / IndexRelationGetNumberOfKeyAttributes(index);
		int kind = mysql_collation_kind(collid);
		if (kind)
		{
			int unit = kind == 1 ? 2 : 4;
			limit = Min(limit, 192);
			if (n > 0 && n < limit / unit) limit = n * unit;
			if (kind == 1) limit -= limit % 2;
		}
		/* Arbitrary ICU collations can contract/expand characters across a
		 * prefix boundary. They have no safe finite prefix interval. Use a
		 * broad candidate bucket, preserving exact equality and rechecking
		 * the original query, rather than losing matches. */
		if (OidIsValid(collid) && !mysql_collation_kind(collid) &&
			!pg_newlocale_from_collation(collid)->collate_is_c)
			limit = 0;
		if ((int) VARSIZE_ANY_EXHDR(key) > limit)
			SET_VARSIZE(key, VARHDRSZ + limit);
		if (kind && !pattern && (int) VARSIZE_ANY_EXHDR(key) < limit)
		{
			int oldlen = VARSIZE_ANY_EXHDR(key);
			key = repalloc(key, VARHDRSZ + limit);
			for (int i = oldlen; i < limit; i++)
				VARDATA(key)[i] = kind == 1 && i % 2 == 0 ? 0 : 32;
			SET_VARSIZE(key, VARHDRSZ + limit);
		}
	}
	if ((Pointer) v != DatumGetPointer(value)) pfree(v);
	return key;
}

/* Comparator receives STORAGE bytea values, independent of input type. */
PG_FUNCTION_INFO_V1(mys_prefix_compare);
Datum
mys_prefix_compare(PG_FUNCTION_ARGS)
{
	bytea *a = PG_GETARG_BYTEA_PP(0), *b = PG_GETARG_BYTEA_PP(1);
	int cmp = bytes_compare(a, b);
	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);
	PG_RETURN_INT32(cmp);
}

static Oid
physical_operator(StrategyNumber strategy)
{
	switch (strategy)
	{
		case BTLessStrategyNumber: return F_BYTEALT;
		case BTLessEqualStrategyNumber: return F_BYTEALE;
		case BTEqualStrategyNumber: return F_BYTEAEQ;
		case BTGreaterEqualStrategyNumber: return F_BYTEAGE;
		case BTGreaterStrategyNumber: return F_BYTEAGT;
	}
	elog(ERROR, "invalid mysql_prefix strategy %d", strategy);
	return InvalidOid;
}

/* Literal prefix before the first unescaped wildcard. */
static text *
pattern_prefix(Datum value)
{
	text *pattern = DatumGetTextPP(value);
	const char *src = VARDATA_ANY(pattern), *end = src + VARSIZE_ANY_EXHDR(pattern);
	text *result = palloc(VARHDRSZ + VARSIZE_ANY_EXHDR(pattern));
	char *dst = VARDATA(result);
	while (src < end && *src != '%' && *src != '_')
	{
		int n;
		if (*src == '\\' && src + 1 < end) src++;
		n = pg_mblen(src);
		memcpy(dst, src, n);
		dst += n; src += n;
	}
	SET_VARSIZE(result, VARHDRSZ + dst - VARDATA(result));
	if ((Pointer) pattern != DatumGetPointer(value)) pfree(pattern);
	return result;
}

static IndexScanDesc
prefix_beginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	PrefixScan *state = palloc0(sizeof(*state));
	state->context = AllocSetContextCreate(CurrentMemoryContext, "MySQL prefix scan", ALLOCSET_DEFAULT_SIZES);
	state->original = palloc0(sizeof(ScanKeyData) * Max(nkeys, 1));
	state->nkeys = nkeys;
	state->tree = btbeginscan(index, 2 * nkeys, 0);
	state->tree->parallel_scan = NULL;
	state->tree->xs_temp_snap = false;
	scan->opaque = state;
	return scan;
}

static void
prefix_rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	PrefixScan *state = scan->opaque;
	MemoryContext old;
	ScanKey converted;
	int count = 0;

	state->tree->heapRelation = scan->heapRelation;
	state->tree->xs_snapshot = scan->xs_snapshot;
	/* btree's old keys can point into the context until btrescan resets it. */
	btrescan(state->tree, NULL, 0, NULL, 0);
	MemoryContextReset(state->context);
	old = MemoryContextSwitchTo(state->context);
	if (keys) memcpy(state->original, keys, sizeof(ScanKeyData) * nkeys);
	keys = state->original;
	nkeys = state->nkeys;
	converted = palloc0(sizeof(ScanKeyData) * Max(2 * nkeys, 1));
	for (int i = 0; i < nkeys; i++)
	{
		ScanKey dst = &converted[count++];
		int col = keys[i].sk_attno - 1;
		*dst = keys[i];
		if (!string_part(scan->indexRelation, col) || (dst->sk_flags & SK_ISNULL))
			continue;
		if (dst->sk_strategy == PREFIX_LIKE_STRATEGY || dst->sk_strategy == PREFIX_PG_LIKE_STRATEGY)
		{
			Oid collid = scan->indexRelation->rd_indcollation[col];
			text *prefix = pattern_prefix(dst->sk_argument);
			bytea *upper, *lower;
			int len;
			/* Only simple character weights admit a safe prefix interval.
			 * Other collations retain their original LIKE filter. */
			if (!VARSIZE_ANY_EXHDR(prefix) ||
				(OidIsValid(collid) && !mysql_collation_kind(collid) &&
				 !pg_newlocale_from_collation(collid)->collate_is_c))
			{
				count--;
				continue;
			}
			upper = string_key(scan->indexRelation, col, PointerGetDatum(prefix), true, true);
			lower = string_key(scan->indexRelation, col, PointerGetDatum(prefix), true, true);
			/* All continuations, including NULs smaller than the PAD SPACE
			 * terminator, belong in a LIKE prefix range. The unpadded key is
			 * a lexicographic lower bound for every physical continuation. */
			dst->sk_strategy = BTGreaterEqualStrategyNumber;
			dst->sk_argument = PointerGetDatum(lower);
			dst->sk_collation = InvalidOid;
			dst->sk_subtype = InvalidOid;
			fmgr_info(F_BYTEAGE, &dst->sk_func);
			/* [prefix, successor(prefix)); truncate to the incremented byte.
			 * The bound may land in a physically truncated bucket: include
			 * it and let the original LIKE condition recheck the row. */
			len = VARSIZE_ANY_EXHDR(upper);
			while (len > 0 && (unsigned char) VARDATA(upper)[len - 1] == 255) len--;
			if (len)
			{
				VARDATA(upper)[len - 1]++;
				SET_VARSIZE(upper, VARHDRSZ + len);
				dst = &converted[count++];
				*dst = converted[count - 2];
				dst->sk_argument = PointerGetDatum(upper);
				dst->sk_strategy = BTLessEqualStrategyNumber;
				fmgr_info(F_BYTEALE, &dst->sk_func);
			}
			continue;
		}
		dst->sk_argument = PointerGetDatum(string_key(scan->indexRelation, col,
											 dst->sk_argument, true, false));
		/* Truncation can make unequal values share a physical key. Strict
		 * boundaries must include the boundary bucket, then heap recheck. */
		if (dst->sk_strategy == BTLessStrategyNumber) dst->sk_strategy = BTLessEqualStrategyNumber;
		if (dst->sk_strategy == BTGreaterStrategyNumber) dst->sk_strategy = BTGreaterEqualStrategyNumber;
		dst->sk_collation = InvalidOid;
		dst->sk_subtype = InvalidOid;
		fmgr_info(physical_operator(dst->sk_strategy), &dst->sk_func);
	}
	state->tree->numberOfKeys = count;
	state->tree->heapRelation = scan->heapRelation;
	state->tree->xs_snapshot = scan->xs_snapshot;
	btrescan(state->tree, converted, count, NULL, 0);
	MemoryContextSwitchTo(old);
}

/* Dirty scans are the executor's unique-conflict probe. Confirm the entire
 * logical key here and set xs_recheck=false: built-in equality would compare
 * whole columns instead of their declared prefixes. */
static bool
exact_candidate(IndexScanDesc scan)
{
	PrefixScan *state = scan->opaque;
	bool again = false, dead = false;
	ItemPointerData tid = state->tree->xs_heaptid;
	Datum values[INDEX_MAX_KEYS];
	bool nulls[INDEX_MAX_KEYS];
	ExprContext *ctx;
	bool match = true;

	if (!state->estate)
	{
		state->estate = CreateExecutorState();
		state->info = BuildIndexInfo(scan->indexRelation);
		state->slot = table_slot_create(scan->heapRelation, NULL);
		state->fetch = table_index_fetch_begin(scan->heapRelation);
	}
	table_index_fetch_reset(state->fetch);
	if (!table_index_fetch_tuple(state->fetch, &tid, scan->xs_snapshot,
								state->slot, &again, &dead))
		return false;
	ctx = GetPerTupleExprContext(state->estate);
	ResetExprContext(ctx);
	ctx->ecxt_scantuple = state->slot;
	FormIndexDatum(state->info, state->slot, state->estate, values, nulls);
	for (int i = 0; i < state->nkeys && match; i++)
	{
		ScanKey key = &state->original[i];
		int col = key->sk_attno - 1;
		if (key->sk_flags & SK_SEARCHNULL)
			match = nulls[col];
		else if (key->sk_flags & SK_SEARCHNOTNULL)
			match = !nulls[col];
		else if (nulls[col] || (key->sk_flags & SK_ISNULL))
			match = false;
		else if (string_part(scan->indexRelation, col))
		{
			bytea *a = string_key(scan->indexRelation, col, values[col], false, false);
			bytea *b = string_key(scan->indexRelation, col, key->sk_argument, false, false);
			int cmp = bytes_compare(a, b);
			switch (key->sk_strategy)
			{
				case 1: match = cmp < 0; break;
				case 2: match = cmp <= 0; break;
				case 3: match = cmp == 0; break;
				case 4: match = cmp >= 0; break;
				case 5: match = cmp > 0; break;
				default: elog(ERROR, "invalid conflict scan strategy");
			}
			pfree(a); pfree(b);
		}
		else
			match = DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation,
												   values[col], key->sk_argument));
	}
	ExecClearTuple(state->slot);
	return match;
}

static bool
prefix_gettuple(IndexScanDesc scan, ScanDirection direction)
{
	PrefixScan *state = scan->opaque;
	bool dirty = scan->xs_snapshot->snapshot_type == SNAPSHOT_DIRTY;
	while (btgettuple(state->tree, direction))
	{
		if (dirty && !exact_candidate(scan)) continue;
		scan->xs_heaptid = state->tree->xs_heaptid;
		scan->xs_recheck = !dirty;
		return true;
	}
	return false;
}

static int64
prefix_getbitmap(IndexScanDesc scan, TIDBitmap *bitmap)
{
	PrefixScan *state = scan->opaque;
	int64 count = 0;
	/* Bitmap scans do not set heapRelation; btree's tuple scan requires it. */
	Relation heap = table_open(scan->indexRelation->rd_index->indrelid, AccessShareLock);
	state->tree->heapRelation = heap;
	while (btgettuple(state->tree, ForwardScanDirection))
	{
		tbm_add_tuples(bitmap, &state->tree->xs_heaptid, 1, true);
		count++;
	}
	table_close(heap, AccessShareLock);
	return count;
}

static void
prefix_endscan(IndexScanDesc scan)
{
	PrefixScan *state = scan->opaque;
	btendscan(state->tree);
	IndexScanEnd(state->tree);
	if (state->estate)
	{
		table_index_fetch_end(state->fetch);
		ExecDropSingleTupleTableSlot(state->slot);
		FreeExecutorState(state->estate);
	}
	MemoryContextDelete(state->context);
	pfree(state->original);
	pfree(state);
}

static bool
prefix_unique(Relation index, Relation heap, Datum *values, bool *nulls,
			  ItemPointer self, IndexUniqueCheck check, LOCKTAG *tag, bool *locked)
{
	ScanKeyData keys[INDEX_MAX_KEYS];
	int n = IndexRelationGetNumberOfKeyAttributes(index);
	SnapshotData dirty;
	IndexScanDesc scan;
	bool conflict;
	TransactionId wait;
	uint32 token;
	uint32 lock_hash = 0;

	for (int i = 0; i < n; i++)
		if (nulls[i] && !index->rd_index->indnullsnotdistinct)
			return true;
	for (int i = 0; i < n; i++)
	{
		uint32 part = 0;
		if (!nulls[i] && string_part(index, i))
		{
			bytea *key = string_key(index, i, values[i], true, false);
			part = hash_bytes((unsigned char *) VARDATA(key), VARSIZE_ANY_EXHDR(key));
			pfree(key);
		}
		else if (!nulls[i])
		{
			TypeCacheEntry *type = lookup_type_cache(index->rd_opcintype[i], TYPECACHE_HASH_PROC_FINFO);
			/* Hashless scalar types deliberately share that part's bucket.
			 * Raw Datum bytes are unsafe (e.g. numeric 1.0 equals 1.00). */
			if (OidIsValid(type->hash_proc_finfo.fn_oid))
				part = DatumGetUInt32(FunctionCall1Coll(&type->hash_proc_finfo,
									  index->rd_indcollation[i], values[i]));
		}
		lock_hash = hash_combine(lock_hash, part);
	}
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, RelationGetRelid(index), lock_hash, 0x5046);
	for (int i = 0; i < n; i++)
	{
		Oid op = get_opfamily_member(index->rd_opfamily[i], index->rd_opcintype[i],
									index->rd_opcintype[i], BTEqualStrategyNumber);
		ScanKeyEntryInitialize(&keys[i], nulls[i] ? SK_ISNULL | SK_SEARCHNULL : 0,
							   i + 1, BTEqualStrategyNumber, InvalidOid,
							   index->rd_indcollation[i], get_opcode(op), values[i]);
	}
retry:
	LockAcquire(tag, ExclusiveLock, false, false);
	*locked = true;
	InitDirtySnapshot(dirty);
	wait = InvalidTransactionId;
	token = 0;
	conflict = false;
	/* AM-internal scan: REINDEX deliberately prohibits public index scans,
	 * but must be able to check the new entries it is currently building. */
	scan = prefix_beginscan(index, n, 0);
	scan->heapRelation = heap;
	scan->xs_snapshot = &dirty;
	scan->parallel_scan = NULL;
	scan->xs_temp_snap = false;
	prefix_rescan(scan, keys, n, NULL, 0);
	while (prefix_gettuple(scan, ForwardScanDirection))
	{
		if (self && ItemPointerEquals(self, &scan->xs_heaptid)) continue;
		conflict = true;
		wait = TransactionIdIsValid(dirty.xmin) ? dirty.xmin : dirty.xmax;
		token = dirty.speculativeToken;
		break;
	}
	prefix_endscan(scan);
	IndexScanEnd(scan);
	if (!conflict) return true;
	if (check == UNIQUE_CHECK_PARTIAL) return false;
	if (TransactionIdIsValid(wait))
	{
		/* As with btree's leaf lock, release before waiting for an XID. The
		 * owner may still need to update another tuple in this same bucket. */
		LockRelease(tag, ExclusiveLock, false);
		*locked = false;
		if (token) SpeculativeInsertionWait(wait, token);
		else XactLockTableWait(wait, heap, self, XLTW_InsertIndex);
		goto retry;
	}
	ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION),
				   errmsg("duplicate key value violates unique constraint \"%s\"",
						  RelationGetRelationName(index)),
				   errtableconstraint(heap, RelationGetRelationName(index))));
	return false;
}

static bool
prefix_insert(Relation index, Datum *values, bool *nulls, ItemPointer tid,
			  Relation heap, IndexUniqueCheck check, bool unchanged, IndexInfo *info)
{
	Datum physical[INDEX_MAX_KEYS];
	bool unique = true;
	bool locked = false;
	LOCKTAG tag;
	MemoryContext temp = AllocSetContextCreate(CurrentMemoryContext, "MySQL prefix insert", ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(temp);
	if (check != UNIQUE_CHECK_NO)
		unique = prefix_unique(index, heap, values, nulls, tid, check, &tag, &locked);
	if (check != UNIQUE_CHECK_EXISTING)
	{
		for (int i = 0; i < info->ii_NumIndexAttrs; i++)
			physical[i] = !nulls[i] && string_part(index, i) ?
				PointerGetDatum(string_key(index, i, values[i], true, false)) : values[i];
		btinsert(index, physical, nulls, tid, heap, UNIQUE_CHECK_NO, unchanged, info);
	}
	if (locked) LockRelease(&tag, ExclusiveLock, false);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(temp);
	return unique;
}

typedef struct PrefixBuild { Relation heap; IndexInfo *info; double tuples; } PrefixBuild;
static void
prefix_build_callback(Relation index, ItemPointer tid, Datum *values, bool *nulls,
					  bool alive, void *opaque)
{
	PrefixBuild *state = opaque;
	prefix_insert(index, values, nulls, tid, state->heap,
				  alive && state->info->ii_Unique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
				  false, state->info);
	state->tuples++;
}

static IndexBuildResult *
prefix_build(Relation heap, Relation index, IndexInfo *info)
{
	IndexBuildResult *result = palloc0(sizeof(*result));
	PrefixBuild state = { heap, info, 0 };
	BulkWriteState *writer = smgr_bulk_start_rel(index, MAIN_FORKNUM);
	BulkWriteBuffer meta = smgr_bulk_get_buf(writer);

	/* Same empty metapage as btbuildempty(), in the main fork. */
	_bt_initmetapage((Page) meta, P_NONE, 0, false);
	smgr_bulk_write(writer, BTREE_METAPAGE, meta, true);
	smgr_bulk_finish(writer);
	result->heap_tuples = table_index_build_scan(heap, index, info, false, true,
												prefix_build_callback, &state, NULL);
	result->index_tuples = state.tuples;
	return result;
}

static void
prefix_cost(PlannerInfo *root, IndexPath *path, double loops, Cost *startup, Cost *total,
			Selectivity *selectivity, double *correlation, double *pages)
{
	GenericCosts costs = {0};
	genericcostestimate(root, path, loops, &costs);
	*startup = costs.indexStartupCost;
	*total = costs.indexTotalCost;
	*selectivity = costs.indexSelectivity;
	*correlation = costs.indexCorrelation;
	*pages = costs.numIndexPages;
}

static bool prefix_validate(Oid opclass) { return true; }

static CompareType
prefix_translate(StrategyNumber strategy, Oid family)
{
	return strategy > BTMaxStrategyNumber ? COMPARE_INVALID : bttranslatestrategy(strategy, family);
}

PG_FUNCTION_INFO_V1(mys_prefix_handler);
Datum
mys_prefix_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = (IndexAmRoutine *) DatumGetPointer(DirectFunctionCall1(bthandler, (Datum) 0));
#ifndef PG_LOCALE_CREATE_HOOK
	install_collation();
#endif
	am->amstrategies = PREFIX_PG_LIKE_STRATEGY;
	am->amcanorder = false;
	am->amconsistentordering = false;
	am->amconsistentequality = false;
	am->amcanreturn = NULL;
	am->amcanparallel = false;
	am->amcanbuildparallel = false;
	am->amcaninclude = false;
	am->amsearcharray = false;
	am->amclusterable = false;
	am->amstorage = true;
	am->amproperty = NULL;
	am->amoptions = prefix_reloptions;
	am->ambuild = prefix_build;
	am->aminsert = prefix_insert;
	am->amcostestimate = prefix_cost;
	am->amvalidate = prefix_validate;
	am->amadjustmembers = NULL;
	am->ambeginscan = prefix_beginscan;
	am->amrescan = prefix_rescan;
	am->amgettuple = prefix_gettuple;
	am->amgetbitmap = prefix_getbitmap;
	am->amendscan = prefix_endscan;
	am->ammarkpos = NULL;
	am->amrestrpos = NULL;
	am->amestimateparallelscan = NULL;
	am->aminitparallelscan = NULL;
	am->amparallelrescan = NULL;
	am->amtranslatestrategy = prefix_translate;
	PG_RETURN_POINTER(am);
}

/* LIKE is not PAD SPACE. A simple general_ci weight corresponds to exactly
 * one character; '%' and '_' operate on characters, never encoded bytes. */
static bool
prefix_like(FunctionCallInfo fcinfo, bool trim_char)
{
	text *value = PG_GETARG_TEXT_PP(0), *pattern = PG_GETARG_TEXT_PP(1);
	const unsigned char *src = (const unsigned char *) VARDATA_ANY(value);
	const unsigned char *pat = (const unsigned char *) VARDATA_ANY(pattern);
	int slen = VARSIZE_ANY_EXHDR(value), plen = VARSIZE_ANY_EXHDR(pattern);
	uint32 *s = palloc(sizeof(uint32) * Max(slen, 1));
	uint32 *p = palloc(sizeof(uint32) * Max(plen, 1));
	int ns = 0, np = 0, si = 0, pi = 0, star = -1, retry = 0;
	bool general = is_general(PG_GET_COLLATION());
	bool matched;
	if (trim_char) while (slen > 0 && src[slen - 1] == ' ') slen--;
	for (int i = 0; i < slen; i += pg_utf_mblen(src + i))
	{
		uint32 ch = utf8_to_unicode(src + i);
		s[ns++] = general ? general_weight(ch) : ch;
	}
	for (int i = 0; i < plen;)
	{
		uint32 ch = utf8_to_unicode(pat + i);
		bool escaped = false;
		i += pg_utf_mblen(pat + i);
		if (ch == '\\' && i < plen)
		{
			ch = utf8_to_unicode(pat + i);
			i += pg_utf_mblen(pat + i);
			escaped = true;
		}
		p[np++] = !escaped && ch == '%' ? UINT32_MAX :
			!escaped && ch == '_' ? UINT32_MAX - 1 : general ? general_weight(ch) : ch;
	}
	while (si < ns)
	{
		CHECK_FOR_INTERRUPTS();
		if (pi < np && (p[pi] == UINT32_MAX - 1 || p[pi] == s[si])) { si++; pi++; }
		else if (pi < np && p[pi] == UINT32_MAX) { star = pi++; retry = si; }
		else if (star >= 0) { pi = star + 1; si = ++retry; }
		else break;
	}
	while (pi < np && p[pi] == UINT32_MAX) pi++;
	matched = si == ns && pi == np;
	pfree(s); pfree(p);
	PG_FREE_IF_COPY(value, 0); PG_FREE_IF_COPY(pattern, 1);
	return matched;
}

PG_FUNCTION_INFO_V1(mys_prefix_like);
Datum mys_prefix_like(PG_FUNCTION_ARGS) { PG_RETURN_BOOL(prefix_like(fcinfo, false)); }
PG_FUNCTION_INFO_V1(mys_prefix_not_like);
Datum mys_prefix_not_like(PG_FUNCTION_ARGS) { PG_RETURN_BOOL(!prefix_like(fcinfo, false)); }
PG_FUNCTION_INFO_V1(mys_prefix_char_like);
Datum mys_prefix_char_like(PG_FUNCTION_ARGS) { PG_RETURN_BOOL(prefix_like(fcinfo, true)); }
PG_FUNCTION_INFO_V1(mys_prefix_char_not_like);
Datum mys_prefix_char_not_like(PG_FUNCTION_ARGS) { PG_RETURN_BOOL(!prefix_like(fcinfo, true)); }

static bool
rewrite_like(Node *node, void *context)
{
	if (!node) return false;
	if (IsA(node, Query)) return query_tree_walker((Query *) node, rewrite_like, NULL, 0);
	if (IsA(node, OpExpr))
	{
		OpExpr *op = (OpExpr *) node;
		if (list_length(op->args) == 2 && mysql_collation_kind(op->inputcollid))
		{
			char *name = get_opname(op->opno);
			if (name && (strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0))
			{
				Oid replacement = OpernameGetOprid(list_make2(makeString("mysql"),
					makeString(strcmp(name, "~~") == 0 ? "~#~" : "!~#~")),
					exprType(linitial(op->args)), exprType(lsecond(op->args)));
				if (OidIsValid(replacement))
				{
					op->opno = replacement;
					op->opfuncid = get_opcode(replacement);
				}
			}
			if (name) pfree(name);
		}
	}
	return expression_tree_walker(node, rewrite_like, context);
}

PG_FUNCTION_INFO_V1(mys_index_key_parts19);
Datum
mys_index_key_parts19(PG_FUNCTION_ARGS)
{
#ifndef MYS_PREFIX_STANDALONE
	extern Datum mys_index_key_parts(PG_FUNCTION_ARGS);
#endif
	Relation index = try_relation_open(PG_GETARG_OID(0), AccessShareLock);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	if (!index || !index->rd_index || !mys_prefix_is_index(index))
	{
		if (index) relation_close(index, AccessShareLock);
#ifndef MYS_PREFIX_STANDALONE
		return mys_index_key_parts(fcinfo);
#else
		InitMaterializedSRF(fcinfo, 0);
		return (Datum) 0;
#endif
	}
	InitMaterializedSRF(fcinfo, 0);
	for (int i = 0; i < index->rd_index->indnkeyatts; i++)
	{
		int length = mys_prefix_length(index, i);
		AttrNumber attnum = index->rd_index->indkey.values[i];
		Datum values[3];
		bool nulls[3] = {false, false, length == 0};
		if (attnum <= 0) continue;
		values[0] = Int32GetDatum(i + 1);
		values[1] = CStringGetTextDatum(get_attname(index->rd_index->indrelid, attnum, false));
		values[2] = Int32GetDatum(length);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	relation_close(index, AccessShareLock);
	return (Datum) 0;
}

static void
prefix_analyze(ParseState *pstate, Query *query, JumbleState *jstate)
{
	if (previous_analyze) previous_analyze(pstate, query, jstate);
#ifndef PG_LOCALE_CREATE_HOOK
	install_collation();
#endif
	query_tree_walker(query, rewrite_like, NULL, 0);
}

/* pg_dump already preserves index reloptions for PRIMARY/UNIQUE constraints.
 * The marker makes the AM self-describing through native dump/restore AND
 * ALTER TYPE's internal constraint rebuild, without rewriting dump text. */
static void
prefix_constraint(Constraint *constraint)
{
#ifdef INDEX_CONSTRAINT_TRANSFORM_HOOK
	if (previous_constraint_transform) previous_constraint_transform(constraint);
#endif
	if ((constraint->contype != CONSTR_PRIMARY && constraint->contype != CONSTR_UNIQUE) ||
		constraint->access_method != NULL)
		return;
	foreach_node(DefElem, option, constraint->options)
		if (strcmp(option->defname, "prefix_lengths") == 0)
		{
			constraint->access_method = MYS_PREFIX_AM;
			return;
		}
}

#ifndef INDEX_CONSTRAINT_TRANSFORM_HOOK
/* Stock PostgreSQL standalone tests. Production must apply the generic
 * transform hook as internal ALTER TYPE rebuilds do not call ProcessUtility. */
static void
prefix_constraint_node(Node *node)
{
	if (!node) return;
	if (IsA(node, Constraint)) prefix_constraint((Constraint *) node);
	else if (IsA(node, ColumnDef))
		foreach_ptr(Node, c, ((ColumnDef *) node)->constraints) prefix_constraint_node(c);
	else if (IsA(node, CreateStmt))
		foreach_ptr(Node, c, ((CreateStmt *) node)->tableElts) prefix_constraint_node(c);
	else if (IsA(node, AlterTableStmt))
		foreach_node(AlterTableCmd, cmd, ((AlterTableStmt *) node)->cmds) prefix_constraint_node(cmd->def);
}

static void
prefix_utility(PlannedStmt *pstmt, const char *query, bool readOnlyTree,
			   ProcessUtilityContext context, ParamListInfo params,
			   QueryEnvironment *env, DestReceiver *dest, QueryCompletion *completion)
{
	if (IsA(pstmt->utilityStmt, CreateStmt) || IsA(pstmt->utilityStmt, AlterTableStmt))
	{
		pstmt = copyObject(pstmt);
		prefix_constraint_node(pstmt->utilityStmt);
		readOnlyTree = false;
	}
	if (previous_utility) previous_utility(pstmt, query, readOnlyTree, context, params, env, dest, completion);
	else standard_ProcessUtility(pstmt, query, readOnlyTree, context, params, env, dest, completion);
}
#endif

#ifndef PG_LOCALE_CREATE_HOOK
static void
prefix_executor_start(QueryDesc *query, int flags)
{
	install_collation();
	if (previous_executor_start) previous_executor_start(query, flags);
	else standard_ExecutorStart(query, flags);
}
#endif

void
mys_prefix_init(void)
{
	if (initialized) return;
	initialized = true;
	prefix_relopt_kind = add_reloption_kind();
	add_string_reloption(prefix_relopt_kind, "prefix_lengths", "Declared MySQL prefixes, in key order",
						 NULL, validate_lengths, AccessExclusiveLock);
	previous_analyze = post_parse_analyze_hook;
	post_parse_analyze_hook = prefix_analyze;
#ifdef INDEX_CONSTRAINT_TRANSFORM_HOOK
	previous_constraint_transform = index_constraint_transform_hook;
	index_constraint_transform_hook = prefix_constraint;
#else
	previous_utility = ProcessUtility_hook;
	ProcessUtility_hook = prefix_utility;
#endif
#ifdef PG_LOCALE_CREATE_HOOK
	previous_locale_create = pg_locale_create_hook;
	pg_locale_create_hook = prefix_locale_create;
#else
	previous_executor_start = ExecutorStart_hook;
	ExecutorStart_hook = prefix_executor_start;
#endif
}
