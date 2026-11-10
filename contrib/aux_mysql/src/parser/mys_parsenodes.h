/*-------------------------------------------------------------------------
 *
 * mys_parsenodes.h
 *		Definitions for the "mys" parser (flex and bison phases only)
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * src/include/parser/mysql/mys_parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSENODES_H
#define MYS_PARSENODES_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"

/*
 * MySQL-specific raw parse nodes.  PostgreSQL has no extensible raw parse
 * nodes, so these use node tags outside the core range.  They never reach
 * the core analyzer: aux_mysql rewrites them into standard nodes (or
 * executes them directly) before parse analysis.
 */
#define T_MysNodeBase			((NodeTag) 30000)
#define T_UserVarRef			((NodeTag) (T_MysNodeBase + 1))
#define T_UserVarAssign			((NodeTag) (T_MysNodeBase + 2))
#define T_SysVarRef				((NodeTag) (T_MysNodeBase + 3))
#define T_MysVariableSetStmt	((NodeTag) (T_MysNodeBase + 4))
#define T_MysSelectIntoStmt		((NodeTag) (T_MysNodeBase + 5))
#define T_ByteaString			((NodeTag) (T_MysNodeBase + 6))
#define T_MysDoStmt				((NodeTag) (T_MysNodeBase + 7))

/*
 * MySQL-only variants of core enums, carried inside standard nodes.  They
 * are translated by aux_mysql (mys_rewrite.c / ProcessUtility hook) before
 * the core analyzer or executor could see them.
 */
#define AT_MysBase				1000
#define AT_TableOption			((AlterTableType) (AT_MysBase + 1))
#define AT_ModifyColumn			((AlterTableType) (AT_MysBase + 2))
#define AT_ChangeColumn			((AlterTableType) (AT_MysBase + 3))
#define AT_DropPrimaryKey		((AlterTableType) (AT_MysBase + 4))
#define AT_DropIndex			((AlterTableType) (AT_MysBase + 5))
#define AT_DropForeignKey		((AlterTableType) (AT_MysBase + 6))
#define AT_DropCheck			((AlterTableType) (AT_MysBase + 7))

#define CONSTR_MysBase			1000
#define CONSTR_AUTOINC			((ConstrType) (CONSTR_MysBase + 1))
#define CONSTR_KEY				((ConstrType) (CONSTR_MysBase + 2))

#define ONCONFLICT_REPLACE		((OnConflictAction) 1000)


typedef struct UserVarRef
{
    NodeTag type;
    char *userVarName;
    int location;
} UserVarRef;


typedef struct UserVarAssign
{
    NodeTag type;
    char *userVarName;
    Node *expr;
    int location;
} UserVarAssign;


typedef struct SysVarRef
{
    NodeTag type;
    char *sysVarName;
    int location;
} SysVarRef;


typedef struct MysVariableSetStmt
{
    NodeTag type;
    Node *varSetStmt;   /* 目前是SelectStmt */
} MysVariableSetStmt;


typedef struct MysSelectIntoStmt
{
    NodeTag type;
    List *target;
    Node *expr;   /* 目前是SelectStmt */
} MysSelectIntoStmt;


/* DO expr [, expr] ...: evaluate the expressions, discard the values */
typedef struct MysDoStmt
{
	NodeTag		type;
	List	   *exprs;
} MysDoStmt;


#endif							/* MYS_PARSENODES_H */
