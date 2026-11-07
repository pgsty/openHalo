/*-------------------------------------------------------------------------
 *
 * mys_auth.c
 *	  Server side of the MySQL caching_sha2_password authentication plugin.
 *
 * The packet exchange lives in mysql_proto.c; this file holds the parts that
 * do not touch the socket:
 *
 *	- the scramble check of the fast path:
 *		  client sends  XOR(SHA256(pw), SHA256(SHA256(SHA256(pw)) || nonce))
 *		  server keeps  SHA256(SHA256(pw))       (the "stage2" digest)
 *	- a shared-memory cache user -> stage2 (MySQL's fast-auth cache).
 *	  PostgreSQL stores SCRAM/MD5 verifiers, from which stage2 cannot be
 *	  derived, so -- exactly like MySQL after a restart or FLUSH PRIVILEGES --
 *	  the first login of a user goes through full authentication: the
 *	  password travels over TLS or RSA-encrypted, is checked against the
 *	  regular pg_authid verifier, and stage2 is cached.  Each entry remembers
 *	  a digest of the verifier it was validated against, so ALTER ROLE ...
 *	  PASSWORD invalidates it without any hook.
 *	- the RSA key pair used for the public key exchange on non-TLS
 *	  connections (generated in the data directory on first start, like
 *	  MySQL's auto-generated private_key.pem).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/cryptohash.h"
#include "common/sha2.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/hsearch.h"

#ifdef USE_OPENSSL
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#endif

#include "halo_mysql.h"

#define SHA2_LEN	PG_SHA256_DIGEST_LENGTH

int			halo_mysql_auth_cache_size = 1024;
char	   *halo_mysql_rsa_private_key_file = NULL;

typedef struct MysAuthCacheEntry
{
	char		user[NAMEDATALEN];	/* hash key */
	uint8		stage2[SHA2_LEN];	/* SHA256(SHA256(password)) */
	uint8		verifier[SHA2_LEN]; /* SHA256(pg_authid.rolpassword) */
} MysAuthCacheEntry;

static HTAB *auth_cache = NULL;
static LWLock *auth_cache_lock = NULL;

#define AUTH_CACHE_TRANCHE "aux_mysql auth cache"

/* ----------------------------------------------------------------
 * SHA-256 helpers
 * ----------------------------------------------------------------
 */
static void
sha256(const void *a, size_t alen, const void *b, size_t blen, uint8 *out)
{
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);

	if (ctx == NULL ||
		pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, a, alen) < 0 ||
		(b != NULL && pg_cryptohash_update(ctx, b, blen) < 0) ||
		pg_cryptohash_final(ctx, out, SHA2_LEN) < 0)
	{
		const char *err = ctx ? pg_cryptohash_error(ctx) : "out of memory";

		pg_cryptohash_free(ctx);
		elog(ERROR, "could not compute SHA-256 digest: %s", err);
	}
	pg_cryptohash_free(ctx);
}

/* constant time comparison */
static bool
digest_equal(const uint8 *a, const uint8 *b, int len)
{
	uint8		diff = 0;

	for (int i = 0; i < len; i++)
		diff |= a[i] ^ b[i];
	return diff == 0;
}

/* ----------------------------------------------------------------
 * Shared memory cache
 * ----------------------------------------------------------------
 */
Size
mys_auth_shmem_size(void)
{
	if (halo_mysql_auth_cache_size <= 0)
		return 0;
	return hash_estimate_size(halo_mysql_auth_cache_size, sizeof(MysAuthCacheEntry));
}

void
mys_auth_shmem_request(void)
{
	if (halo_mysql_auth_cache_size > 0)
		RequestNamedLWLockTranche(AUTH_CACHE_TRANCHE, 1);
}

/* caller holds AddinShmemInitLock */
void
mys_auth_shmem_startup(void)
{
	HASHCTL		info;

	if (halo_mysql_auth_cache_size <= 0)
		return;

	auth_cache_lock = &(GetNamedLWLockTranche(AUTH_CACHE_TRANCHE))->lock;
	info.keysize = NAMEDATALEN;
	info.entrysize = sizeof(MysAuthCacheEntry);
	auth_cache = ShmemInitHash("aux_mysql auth cache",
							   halo_mysql_auth_cache_size,
							   halo_mysql_auth_cache_size,
							   &info,
							   HASH_ELEM | HASH_STRINGS | HASH_FIXED_SIZE);
}

static void
cache_key(const char *user, char *key)
{
	memset(key, 0, NAMEDATALEN);
	strlcpy(key, user, NAMEDATALEN);
}

/*
 * Fast path: is "resp" (32 bytes) the right scramble for the cached stage2
 * of this user?  The entry is only trusted while the role still has the
 * verifier it was validated against.
 */
bool
mys_sha2_fast_auth(const char *user, const char *shadow_pass,
				   const char *nonce, int nonce_len,
				   const uint8 *resp, int resp_len)
{
	char		key[NAMEDATALEN];
	MysAuthCacheEntry *e;
	uint8		verifier[SHA2_LEN];
	uint8		stage2[SHA2_LEN];
	uint8		xor_key[SHA2_LEN];
	uint8		stage1[SHA2_LEN];
	uint8		check[SHA2_LEN];
	bool		found = false;

	if (auth_cache == NULL || shadow_pass == NULL || resp_len != SHA2_LEN)
		return false;

	cache_key(user, key);
	sha256(shadow_pass, strlen(shadow_pass), NULL, 0, verifier);

	LWLockAcquire(auth_cache_lock, LW_SHARED);
	e = hash_search(auth_cache, key, HASH_FIND, NULL);
	if (e && digest_equal(e->verifier, verifier, SHA2_LEN))
	{
		memcpy(stage2, e->stage2, SHA2_LEN);
		found = true;
	}
	LWLockRelease(auth_cache_lock);

	if (!found)
		return false;

	/* stage1 = resp XOR SHA256(stage2 || nonce);  SHA256(stage1) == stage2 ? */
	sha256(stage2, SHA2_LEN, nonce, nonce_len, xor_key);
	for (int i = 0; i < SHA2_LEN; i++)
		stage1[i] = resp[i] ^ xor_key[i];
	sha256(stage1, SHA2_LEN, NULL, 0, check);
	explicit_bzero(stage1, sizeof(stage1));

	return digest_equal(check, stage2, SHA2_LEN);
}

/* Remember SHA256(SHA256(password)) after a successful full authentication */
void
mys_sha2_cache_store(const char *user, const char *shadow_pass, const char *password)
{
	char		key[NAMEDATALEN];
	uint8		stage1[SHA2_LEN];
	uint8		stage2[SHA2_LEN];
	uint8		verifier[SHA2_LEN];
	MysAuthCacheEntry *e;

	if (auth_cache == NULL || shadow_pass == NULL)
		return;

	cache_key(user, key);
	sha256(password, strlen(password), NULL, 0, stage1);
	sha256(stage1, SHA2_LEN, NULL, 0, stage2);
	explicit_bzero(stage1, sizeof(stage1));
	sha256(shadow_pass, strlen(shadow_pass), NULL, 0, verifier);

	LWLockAcquire(auth_cache_lock, LW_EXCLUSIVE);
	e = hash_search(auth_cache, key, HASH_ENTER_NULL, NULL);
	if (e == NULL)
	{
		/* cache full: evict an arbitrary entry */
		HASH_SEQ_STATUS seq;
		MysAuthCacheEntry *victim;

		hash_seq_init(&seq, auth_cache);
		victim = hash_seq_search(&seq);
		if (victim)
		{
			char		vkey[NAMEDATALEN];

			memcpy(vkey, victim->user, NAMEDATALEN);
			hash_seq_term(&seq);
			hash_search(auth_cache, vkey, HASH_REMOVE, NULL);
		}
		e = hash_search(auth_cache, key, HASH_ENTER_NULL, NULL);
	}
	if (e)
	{
		memcpy(e->stage2, stage2, SHA2_LEN);
		memcpy(e->verifier, verifier, SHA2_LEN);
	}
	LWLockRelease(auth_cache_lock);
}

/* mysql.flush_auth_cache(): what FLUSH PRIVILEGES does to the cache */
PG_FUNCTION_INFO_V1(mys_flush_auth_cache);
Datum
mys_flush_auth_cache(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS seq;
	MysAuthCacheEntry *e;
	List	   *keys = NIL;
	int64		n = 0;
	ListCell   *lc;

	if (auth_cache == NULL)
		PG_RETURN_INT64(0);

	LWLockAcquire(auth_cache_lock, LW_EXCLUSIVE);
	hash_seq_init(&seq, auth_cache);
	while ((e = hash_seq_search(&seq)) != NULL)
		keys = lappend(keys, pnstrdup(e->user, NAMEDATALEN));
	foreach(lc, keys)
	{
		char		key[NAMEDATALEN];

		cache_key(lfirst(lc), key);
		if (hash_search(auth_cache, key, HASH_REMOVE, NULL))
			n++;
	}
	LWLockRelease(auth_cache_lock);
	PG_RETURN_INT64(n);
}

/* ----------------------------------------------------------------
 * RSA key pair (public key exchange on non-TLS connections)
 * ----------------------------------------------------------------
 */
#ifdef USE_OPENSSL
static EVP_PKEY *rsa_key = NULL;
static char *rsa_public_pem = NULL;	/* malloc'd, inherited by backends */
static int	rsa_public_pem_len = 0;

static const char *
ssl_errmsg(void)
{
	unsigned long e = ERR_get_error();
	static char buf[128];

	if (e == 0)
		return "no SSL error reported";
	ERR_error_string_n(e, buf, sizeof(buf));
	return buf;
}

static EVP_PKEY *
generate_key(const char *path)
{
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
	EVP_PKEY   *key = NULL;
	int			fd;
	FILE	   *fp;

	if (ctx == NULL || EVP_PKEY_keygen_init(ctx) <= 0 ||
		EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 ||
		EVP_PKEY_keygen(ctx, &key) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		ereport(WARNING,
				(errmsg("aux_mysql: could not generate RSA key pair: %s", ssl_errmsg())));
		return NULL;
	}
	EVP_PKEY_CTX_free(ctx);

	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY, S_IRUSR | S_IWUSR);
	if (fd < 0 || (fp = fdopen(fd, "w")) == NULL)
	{
		if (fd >= 0)
			close(fd);
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("aux_mysql: could not create RSA private key file \"%s\": %m", path)));
		return key;				/* still usable for this run */
	}
	if (PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) != 1)
		ereport(WARNING,
				(errmsg("aux_mysql: could not write RSA private key file \"%s\": %s",
						path, ssl_errmsg())));
	fclose(fp);
	ereport(LOG,
			(errmsg("aux_mysql: generated RSA key pair \"%s\" for caching_sha2_password", path)));
	return key;
}

static void
load_key(void)
{
	const char *path = halo_mysql_rsa_private_key_file;
	FILE	   *fp;
	BIO		   *bio;
	char	   *data;
	long		len;

	if (rsa_key != NULL || path == NULL || path[0] == '\0')
		return;

	fp = AllocateFile(path, "r");
	if (fp != NULL)
	{
		rsa_key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
		FreeFile(fp);
		if (rsa_key == NULL)
		{
			ereport(WARNING,
					(errmsg("aux_mysql: could not load RSA private key \"%s\": %s",
							path, ssl_errmsg())));
			return;
		}
	}
	else if (errno == ENOENT && !IsUnderPostmaster)
		rsa_key = generate_key(path);
	else
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("aux_mysql: could not open RSA private key \"%s\": %m", path)));
		return;
	}
	if (rsa_key == NULL)
		return;

	if (EVP_PKEY_base_id(rsa_key) != EVP_PKEY_RSA)
	{
		ereport(WARNING,
				(errmsg("aux_mysql: \"%s\" is not an RSA key", path)));
		EVP_PKEY_free(rsa_key);
		rsa_key = NULL;
		return;
	}

	/* SubjectPublicKeyInfo PEM ("BEGIN PUBLIC KEY"), as MySQL sends it */
	bio = BIO_new(BIO_s_mem());
	if (bio == NULL || PEM_write_bio_PUBKEY(bio, rsa_key) != 1)
	{
		BIO_free(bio);
		ereport(WARNING,
				(errmsg("aux_mysql: could not encode RSA public key: %s", ssl_errmsg())));
		EVP_PKEY_free(rsa_key);
		rsa_key = NULL;
		return;
	}
	len = BIO_get_mem_data(bio, &data);
	rsa_public_pem = malloc(len + 1);
	if (rsa_public_pem)
	{
		memcpy(rsa_public_pem, data, len);
		rsa_public_pem[len] = '\0';
		rsa_public_pem_len = (int) len;
	}
	BIO_free(bio);
}
#endif							/* USE_OPENSSL */

/* Postmaster startup (data directory is the working directory) */
void
mys_auth_postmaster_init(void)
{
#ifdef USE_OPENSSL
	load_key();
#endif
}

/* PEM of the public key, or NULL if RSA key exchange is not available */
const char *
mys_rsa_public_key(int *len)
{
#ifdef USE_OPENSSL
	if (rsa_key == NULL)
		load_key();				/* EXEC_BACKEND, or key added later */
	if (rsa_key && rsa_public_pem)
	{
		*len = rsa_public_pem_len;
		return rsa_public_pem;
	}
#endif
	*len = 0;
	return NULL;
}

/*
 * Decrypt the client's RSA-OAEP block and undo the scramble XOR.
 * Returns the palloc'd NUL terminated password, or NULL.
 */
char *
mys_rsa_decrypt_password(const uint8 *in, int inlen,
						 const char *nonce, int nonce_len)
{
#ifdef USE_OPENSSL
	EVP_PKEY_CTX *ctx;
	size_t		outlen = 0;
	unsigned char *out;
	char	   *pw;

	if (rsa_key == NULL || inlen <= 0 || nonce_len <= 0)
		return NULL;

	ctx = EVP_PKEY_CTX_new(rsa_key, NULL);
	if (ctx == NULL || EVP_PKEY_decrypt_init(ctx) <= 0 ||
		EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0 ||
		EVP_PKEY_decrypt(ctx, NULL, &outlen, in, inlen) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		return NULL;
	}
	out = palloc(outlen + 1);
	if (EVP_PKEY_decrypt(ctx, out, &outlen, in, inlen) <= 0)
	{
		EVP_PKEY_CTX_free(ctx);
		ERR_clear_error();
		pfree(out);
		return NULL;
	}
	EVP_PKEY_CTX_free(ctx);

	for (size_t i = 0; i < outlen; i++)
		out[i] ^= (unsigned char) nonce[i % nonce_len];
	pw = pnstrdup((char *) out, strnlen((char *) out, outlen));
	explicit_bzero(out, outlen);
	pfree(out);
	return pw;
#else
	return NULL;
#endif
}
