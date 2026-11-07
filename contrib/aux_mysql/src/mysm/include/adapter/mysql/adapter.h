/*-------------------------------------------------------------------------
 *
 * adapter.h
 *	  aux_mysql replacement for openHalo's core adapter/mysql/adapter.h.
 *
 * openHalo kept these MySQL session globals in its modified backend; in
 * aux_mysql they live in the extension (mys_compat.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef ADAPTER_H
#define ADAPTER_H

#include "postgres.h"

extern char *halo_mysql_version;
extern bool halo_mysql_explicit_defaults_for_timestamp;
extern bool halo_mysql_auto_rollback_tx_on_error;
extern int	moreResultsFlag;
extern int	warnings;
extern unsigned long foundRows;
extern long long affectedRows;
extern unsigned long lastInsertID;
extern bool isStrictTransTablesOn;
extern Oid	caseInsensitiveId;
extern bool needCommitTrx;
extern bool needStartNewTrx;

extern Oid	getCaseInsensitiveId(void);

#endif							/* ADAPTER_H */
