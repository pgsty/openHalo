/*-------------------------------------------------------------------------
 *
 * mys_lock.c
 *	  MySQL user-level locks: GET_LOCK(), RELEASE_LOCK(), IS_FREE_LOCK(),
 *	  IS_USED_LOCK() and RELEASE_ALL_LOCKS(), as in MySQL 5.7.5 and later.
 *
 * A lock is a session-level advisory lock (ExclusiveLock) of the database,
 * keyed by a 64-bit hash of the lower-cased name: MySQL compares lock names
 * case-insensitively, and all MySQL sessions connect to one database
 * (mysql.database).  locktag_field4 is MYS_USER_LOCK_SPACE, so that the
 * locks never collide with the keys of pg_advisory_lock(), which use 1 and 2;
 * in pg_locks they show as advisory locks with that objsubid.
 *
 * As in MySQL, a session can hold several locks, and the same lock several
 * times (RELEASE_LOCK releases one acquisition); transactions do not release
 * them.  The session's locks are released by RELEASE_ALL_LOCKS(), by
 * COM_RESET_CONNECTION, and when the session ends.
 *
 * A wait runs in a subtransaction, with lock_timeout set to the timeout:
 * the timeout error becomes the result 0.  The subtransaction also sets a
 * short deadlock_timeout, so that, as in MySQL, the session that closes a
 * cycle of waits is normally the one that gets the deadlock error
 * (ER_USER_LOCK_DEADLOCK); it keeps the locks it holds.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/xact.h"
#include "common/hashfn.h"
#include "common/unicode_case.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/resowner.h"
#include "utils/varbit.h"

#include "halo_mysql.h"

/* locktag_field4 of aux_mysql's user-level locks ("MY") */
#define MYS_USER_LOCK_SPACE		0x4D59

/* MySQL's NAME_CHAR_LEN: the longest lock name, in characters */
#define MYS_LOCK_NAME_CHARS		64

/* deadlock_timeout (ms) while GET_LOCK() waits */
#define MYS_LOCK_DEADLOCK_TIMEOUT	10

/* locks of this session: how many times each one is held */
typedef struct UserLock
{
	uint64		key;			/* hash key */
	int64		count;
} UserLock;

static HTAB *user_locks = NULL;

PG_FUNCTION_INFO_V1(mys_get_lock);
PG_FUNCTION_INFO_V1(mys_release_lock);
PG_FUNCTION_INFO_V1(mys_is_free_lock);
PG_FUNCTION_INFO_V1(mys_is_used_lock);
PG_FUNCTION_INFO_V1(mys_release_all_locks);

static void
wrong_lock_name(const char *name)
{
	ereport(ERROR,
			(errcode(ERRCODE_MYS_USER_LOCK_WRONG_NAME),
			 errmsg("Incorrect user-level lock name '%s'.", name)));
}

/*
 * The text of an argument of type "any": bytes of a binary string, the
 * output of any other type.  A bit string (the type of a 0x41 literal) is a
 * binary string, as in MySQL: b'101' is the byte 0x05.
 */
static char *
any_arg_bytes(FunctionCallInfo fcinfo, int argno, int *len, bool *binary)
{
	Oid			type = get_fn_expr_argtype(fcinfo->flinfo, argno);
	Oid			base;
	Oid			output;
	bool		varlena;
	char	   *s;

	if (!OidIsValid(type))
		elog(ERROR, "could not determine the type of argument %d", argno + 1);
	base = getBaseType(type);
	*binary = base == BYTEAOID || base == BITOID || base == VARBITOID;
	if (base == BYTEAOID)
	{
		bytea	   *b = PG_GETARG_BYTEA_PP(argno);

		*len = VARSIZE_ANY_EXHDR(b);
		return VARDATA_ANY(b);
	}
	if (*binary)
	{
		VarBit	   *b = PG_GETARG_VARBIT_P(argno);
		int			nbits = VARBITLEN(b);
		bits8	   *in = VARBITS(b);

		*len = (nbits + 7) / 8;
		s = palloc0(*len + 1);
		for (int i = 0; i < nbits; i++)
		{
			int			pos = nbits - 1 - i;	/* bit number from the right */

			if (in[i / 8] & (0x80 >> (i % 8)))
				s[*len - 1 - pos / 8] |= 1 << (pos % 8);
		}
		return s;
	}
	getTypeOutputInfo(type, &output, &varlena);
	s = OidOutputFunctionCall(output, PG_GETARG_DATUM(argno));
	*len = strlen(s);
	return s;
}

/*
 * Lock tag of the lock named by argument argno; *key gets the hash key.
 * NULL, empty and longer than 64 characters are not lock names.
 */
static void
user_lock_tag(FunctionCallInfo fcinfo, int argno, LOCKTAG *tag, uint64 *key)
{
	char	   *name;
	char	   *lower;
	int			len;
	int			nchars;
	bool		binary;

	if (PG_ARGISNULL(argno))
		wrong_lock_name("NULL");
	name = any_arg_bytes(fcinfo, argno, &len, &binary);
	nchars = binary ? len : pg_mbstrlen_with_len(name, len);
	if (len == 0 || nchars > MYS_LOCK_NAME_CHARS)
		/* MySQL shows up to 192 bytes of the name */
		wrong_lock_name(pnstrdup(name, binary ? Min(len, 192) : pg_mbcliplen(name, len, 192)));

	/* lower case: 'Lock' and 'lock' are one lock, 'Straße' is not 'STRASSE' */
	if (!binary && GetDatabaseEncoding() == PG_UTF8)
	{
		size_t		size = (size_t) len * 2 + 1;
		size_t		n;

		lower = palloc(size);
		n = unicode_strlower(lower, size, name, len, false);
		if (n >= size)
		{
			lower = repalloc(lower, n + 1);
			n = unicode_strlower(lower, n + 1, name, len, false);
		}
		len = n;
	}
	else
	{
		lower = palloc(len);
		for (int i = 0; i < len; i++)
			lower[i] = pg_ascii_tolower((unsigned char) name[i]);
	}

	*key = hash_bytes_extended((const unsigned char *) lower, len, 0);
	SET_LOCKTAG_ADVISORY(*tag, MyDatabaseId, (uint32) (*key >> 32),
						 (uint32) *key, MYS_USER_LOCK_SPACE);
}

/*
 * The timeout argument as MySQL reads an integer: numbers are rounded, a
 * string gives its integer prefix (warning 1292 if anything else follows).
 */
static int64
timeout_arg(FunctionCallInfo fcinfo, int argno)
{
	Oid			type;
	Datum		d = PG_GETARG_DATUM(argno);

	if (PG_ARGISNULL(argno))
		return 0;
	type = getBaseType(get_fn_expr_argtype(fcinfo->flinfo, argno));
	switch (type)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case INT8OID:
			return DatumGetInt64(d);
		case BOOLOID:
			return DatumGetBool(d) ? 1 : 0;
		case FLOAT4OID:
		case FLOAT8OID:
			{
				double		f = type == FLOAT4OID ? DatumGetFloat4(d) : DatumGetFloat8(d);

				if (isnan(f))
					return 0;
				f = rint(f);
				if (f >= (double) PG_INT64_MAX)
					return PG_INT64_MAX;
				if (f <= (double) PG_INT64_MIN)
					return PG_INT64_MIN;
				return (int64) f;
			}
		case NUMERICOID:
			{
				Numeric		n = DatumGetNumeric(d);
				bool		error = false;
				int64		v;

				if (numeric_is_nan(n))
					return 0;
				v = numeric_int8_opt_error(n, &error);
				if (error)
					return DatumGetBool(DirectFunctionCall2(numeric_lt, d,
															NumericGetDatum(int64_to_numeric(0)))) ?
						PG_INT64_MIN : PG_INT64_MAX;
				return v;
			}
		case BITOID:
		case VARBITOID:
			{
				/* 0x0a: a binary string read as a number, big-endian */
				int			len;
				bool		binary;
				const unsigned char *s = (const unsigned char *)
					any_arg_bytes(fcinfo, argno, &len, &binary);
				uint64		v = 0;

				for (int i = 0; i < len; i++)
				{
					if (v > (PG_UINT64_MAX >> 8))
						return PG_INT64_MAX;
					v = (v << 8) | s[i];
				}
				return v > (uint64) PG_INT64_MAX ? PG_INT64_MAX : (int64) v;
			}
		default:
			{
				int			len;
				bool		binary;
				char	   *s = pnstrdup(any_arg_bytes(fcinfo, argno, &len, &binary), len);
				const char *p = s;
				bool		neg = false;
				bool		digits = false;
				uint64		v = 0;

				while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
					p++;
				if (*p == '-' || *p == '+')
					neg = *p++ == '-';
				for (; *p >= '0' && *p <= '9'; p++)
				{
					digits = true;
					if (v <= (uint64) PG_INT64_MAX / 10)
						v = v * 10 + (*p - '0');
					else
						v = (uint64) PG_INT64_MAX + 1;	/* out of range */
				}
				while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
					p++;
				if (!digits || *p != '\0')
					mys_add_warning(1292, psprintf("Truncated incorrect INTEGER value: '%s'", s));
				if (v > (uint64) PG_INT64_MAX)
					return neg ? PG_INT64_MIN : PG_INT64_MAX;
				return neg ? -(int64) v : (int64) v;
			}
	}
}

static UserLock *
user_lock_entry(uint64 key, bool create)
{
	if (user_locks == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(UserLock);
		ctl.hcxt = TopMemoryContext;
		user_locks = hash_create("aux_mysql user locks", 16, &ctl,
								 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	if (create)
	{
		bool		found;
		UserLock   *l = hash_search(user_locks, &key, HASH_ENTER, &found);

		if (!found)
			l->count = 0;
		return l;
	}
	return hash_search(user_locks, &key, HASH_FIND, NULL);
}

/*
 * The entry of a lock this session holds, or NULL.  An entry whose lock is
 * gone (pg_advisory_unlock_all(), DISCARD ALL) is dropped.
 */
static UserLock *
held_user_lock(uint64 key, const LOCKTAG *tag)
{
	UserLock   *l = user_lock_entry(key, false);

	if (l != NULL && !LockHeldByMe(tag, ExclusiveLock, false))
	{
		hash_search(user_locks, &key, HASH_REMOVE, NULL);
		l = NULL;
	}
	return l;
}

/* pid of the session that holds a lock, or 0 */
static int
user_lock_holder(const LOCKTAG *tag)
{
	LockData   *data = GetLockStatusData();
	int			pid = 0;

	for (int i = 0; i < data->nelements; i++)
	{
		LockInstanceData *l = &data->locks[i];

		if ((l->holdMask & LOCKBIT_ON(ExclusiveLock)) &&
			l->locktag.locktag_type == LOCKTAG_ADVISORY &&
			l->locktag.locktag_field1 == tag->locktag_field1 &&
			l->locktag.locktag_field2 == tag->locktag_field2 &&
			l->locktag.locktag_field3 == tag->locktag_field3 &&
			l->locktag.locktag_field4 == tag->locktag_field4)
		{
			pid = l->pid;
			break;
		}
	}
	return pid;
}

/*
 * Wait up to timeout seconds (forever if negative) for a lock that another
 * session holds.  Returns whether the lock was acquired.
 */
static bool
wait_for_user_lock(const LOCKTAG *tag, int64 timeout)
{
	MemoryContext cxt = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	char		buf[32];

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cxt);
	PG_TRY();
	{
		/* GUC_ACTION_SAVE: the subtransaction ends both settings */
		if (timeout < 0 || timeout > INT_MAX / 1000)
			strcpy(buf, "0");
		else
			snprintf(buf, sizeof(buf), "%d", (int) timeout * 1000);
		(void) set_config_option("lock_timeout", buf, PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
		if (DeadlockTimeout > MYS_LOCK_DEADLOCK_TIMEOUT)
		{
			snprintf(buf, sizeof(buf), "%d", MYS_LOCK_DEADLOCK_TIMEOUT);
			(void) set_config_option("deadlock_timeout", buf, PGC_SUSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);
		}
		(void) LockAcquire(tag, ExclusiveLock, true, false);
		/* a lock timeout that coincided with the grant must not leak out */
		CHECK_FOR_INTERRUPTS();
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;
		bool		held;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cxt);
		CurrentResourceOwner = owner;

		/* granted just as the wait ended: the session holds the lock */
		held = LockHeldByMe(tag, ExclusiveLock, false);
		if (edata->sqlerrcode == ERRCODE_LOCK_NOT_AVAILABLE)
			return held;
		if (held)
		{
			/* the statement fails, the session keeps the lock, as in MySQL */
			uint64		key = ((uint64) tag->locktag_field2 << 32) | tag->locktag_field3;

			user_lock_entry(key, true)->count++;
		}
		else if (edata->sqlerrcode == ERRCODE_T_R_DEADLOCK_DETECTED)
			ereport(ERROR,
					(errcode(ERRCODE_MYS_USER_LOCK_DEADLOCK),
					 errmsg("Deadlock found when trying to get user-level lock; try rolling back transaction/releasing locks and restarting lock acquisition.")));
		ReThrowError(edata);
	}
	PG_END_TRY();
	return true;
}

/*
 * GET_LOCK(name, timeout): 1 when the session holds the lock (again),
 * 0 after timeout seconds; a negative timeout waits forever.
 */
Datum
mys_get_lock(PG_FUNCTION_ARGS)
{
	LOCKTAG		tag;
	uint64		key;
	int64		timeout;

	user_lock_tag(fcinfo, 0, &tag, &key);
	timeout = timeout_arg(fcinfo, 1);
	(void) held_user_lock(key, &tag);	/* drop a stale entry */
	if (LockAcquire(&tag, ExclusiveLock, true, true) == LOCKACQUIRE_NOT_AVAIL &&
		(timeout == 0 || !wait_for_user_lock(&tag, timeout)))
		PG_RETURN_INT64(0);
	user_lock_entry(key, true)->count++;
	PG_RETURN_INT64(1);
}

/*
 * RELEASE_LOCK(name): 1 when the session held the lock, 0 if another
 * session holds it, NULL if nobody does.
 */
Datum
mys_release_lock(PG_FUNCTION_ARGS)
{
	LOCKTAG		tag;
	uint64		key;
	UserLock   *l;

	user_lock_tag(fcinfo, 0, &tag, &key);
	l = held_user_lock(key, &tag);
	if (l != NULL)
	{
		LockRelease(&tag, ExclusiveLock, true);
		if (--l->count <= 0)
			hash_search(user_locks, &key, HASH_REMOVE, NULL);
		PG_RETURN_INT64(1);
	}
	if (user_lock_holder(&tag) != 0)
		PG_RETURN_INT64(0);
	PG_RETURN_NULL();
}

/* IS_FREE_LOCK(name): 1 if no session holds the lock, else 0 */
Datum
mys_is_free_lock(PG_FUNCTION_ARGS)
{
	LOCKTAG		tag;
	uint64		key;

	user_lock_tag(fcinfo, 0, &tag, &key);
	if (held_user_lock(key, &tag) != NULL || user_lock_holder(&tag) != 0)
		PG_RETURN_INT64(0);
	PG_RETURN_INT64(1);
}

/* IS_USED_LOCK(name): connection id of the session that holds the lock, or NULL */
Datum
mys_is_used_lock(PG_FUNCTION_ARGS)
{
	LOCKTAG		tag;
	uint64		key;
	int			pid;

	user_lock_tag(fcinfo, 0, &tag, &key);
	if (held_user_lock(key, &tag) != NULL)
		PG_RETURN_INT64(MyProcPid);
	pid = user_lock_holder(&tag);
	if (pid == 0)
		PG_RETURN_NULL();
	PG_RETURN_INT64(pid);
}

/* release every lock of the session; returns the number of acquisitions */
int64
mys_release_user_locks(void)
{
	HASH_SEQ_STATUS seq;
	UserLock   *l;
	int64		n = 0;

	if (user_locks == NULL)
		return 0;
	hash_seq_init(&seq, user_locks);
	while ((l = hash_seq_search(&seq)) != NULL)
	{
		LOCKTAG		tag;

		SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32) (l->key >> 32),
							 (uint32) l->key, MYS_USER_LOCK_SPACE);
		for (; l->count > 0 && LockHeldByMe(&tag, ExclusiveLock, false); l->count--)
		{
			LockRelease(&tag, ExclusiveLock, true);
			n++;
		}
		hash_search(user_locks, &l->key, HASH_REMOVE, NULL);
	}
	return n;
}

/* RELEASE_ALL_LOCKS(): the number of lock acquisitions released */
Datum
mys_release_all_locks(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(mys_release_user_locks());
}
