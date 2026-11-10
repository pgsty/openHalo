/*-------------------------------------------------------------------------
 * mys_fk.h
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *-------------------------------------------------------------------------
 */
#ifndef MYS_FK_H
#define MYS_FK_H

#include "nodes/plannodes.h"

typedef struct MysFKUtility MysFKUtility;
extern void mys_fk_init(void);
extern MysFKUtility *mys_fk_before_utility(PlannedStmt **pstmt);
extern void mys_fk_after_utility(MysFKUtility *state);
struct RelationData;
struct TupleTableSlot;
extern void mys_pending_fk_check_insert(struct RelationData *rel, struct TupleTableSlot *slot);

#endif
