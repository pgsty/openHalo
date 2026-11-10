/*-------------------------------------------------------------------------
 *
 * systemVar.c
 *	  Extend system_var routines
 *
 * 
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 * 版权所有 (c) 2026, PGSTY PTE. LTD. 保留所有权利。
 * Copyright (c) 2026, PGSTY PTE. LTD. All rights reserved.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 * 
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）、openHalo软件的发明人同时也为
 * 知识产权权利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括
 * 但不限于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文
 * 档等）均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识
 * 产权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
 * 产权”是指任何及所有基于 Halo 软件产生的：（a）版权、商标、商号、域名、与
 * 商标和商号相关的商誉、设计和专利；与创新、技术诀窍、商业秘密、保密技术、非
 * 技术信息相关的权利；（b）人身权、掩模作品权、署名权和发表权；以及（c）在
 * 本协议生效之前已存在或此后出现在世界任何地方的其他工业产权、专有权、与“知
 * 识产权”相关的权利，以及上述权利的所有续期和延长，无论此类权利是否已在相
 * 关法域内的相关机构注册。
 *
 * This software and related documentation are provided under a license
 * agreement containing restrictions on use and disclosure and are 
 * protected by intellectual property laws. Except as expressly permitted
 * in your license agreement or allowed by law, you may not use, copy, 
 * reproduce, translate, broadcast, modify, license, transmit, distribute,
 * exhibit, perform, publish, or display any part, in any form, or by any
 * means. Reverse engineering, disassembly, or decompilation of this 
 * software, unless required by law for interoperability, is prohibited.
 * 
 * This software is developed for general use in a variety of
 * information management applications. It is not developed or intended
 * for use in any inherently dangerous applications, including applications
 * that may create a risk of personal injury. If you use this software or
 * in dangerous applications, then you shall be responsible to take all
 * appropriate fail-safe, backup, redundancy, and other measures to ensure
 * its safe use. Halo Corporation and its affiliates disclaim any 
 * liability for any damages caused by use of this software in dangerous
 * applications.
 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/systemVar.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"
#include "halo_mysql.h"

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "adapter/mysql/systemVar.h"

/* The wire/database representation is UTF-8. Keep initialization settings
 * honest about approximated encodings; literal collation propagation is a
 * separate compatibility boundary, not implemented by these setters. */
static char *
connection_charset(char *name)
{
	if (pg_strcasecmp(name, "utf8") == 0 || pg_strcasecmp(name, "utf8mb3") == 0)
		return "utf8";
	if (pg_strcasecmp(name, "utf8mb4") != 0)
		mys_add_warning(1105, psprintf("Character set '%s' mapped to utf8mb4", name));
	return "utf8mb4";
}

static void
set_connection_charset(FunctionCallInfo fcinfo, bool names)
{
	char *charset = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *connection;
	char *collation = NULL;
	char *effective;
	bool binary;
	size_t len;

	if (strcmp(charset, "halo_mysql_system_var_default") == 0)
	{
		getSystemVariableValueForSelect("character_set_server", false, &charset);
		if (names)
			getSystemVariableValueForSelect("collation_server", false, &collation);
	}
	charset = connection_charset(charset);
	connection = charset;
	if (names)
	{
		if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
			collation = text_to_cstring(PG_GETARG_TEXT_PP(1));
	}
	else
	{
		getSystemVariableValueForSelect("character_set_database", true, &connection);
		connection = connection_charset(connection);
		getSystemVariableValueForSelect("collation_database", true, &collation);
	}
	len = collation ? strlen(collation) : 0;
	/* Match the DDL mapper: *_ci uses general_ci, other names use bin. */
	binary = collation && !(len > 3 && pg_strcasecmp(collation + len - 3, "_ci") == 0);
	effective = strcmp(connection, "utf8") == 0 ?
		(binary ? "utf8_bin" : "utf8_general_ci") :
		(binary ? "utf8mb4_bin" : "utf8mb4_general_ci");
	if (collation && pg_strcasecmp(collation, effective) != 0)
		mys_add_warning(1105, psprintf("Collation '%s' mapped to %s", collation, effective));
	setSystemVariableValue("character_set_client", charset, true);
	setSystemVariableValue("character_set_results", charset, true);
	setSystemVariableValue("character_set_connection", connection, true);
	setSystemVariableValue("collation_connection", effective, true);
}

PG_FUNCTION_INFO_V1(mys_set_names);
Datum
mys_set_names(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	set_connection_charset(fcinfo, true);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(mys_set_charset);
Datum
mys_set_charset(PG_FUNCTION_ARGS)
{
	set_connection_charset(fcinfo, false);
	PG_RETURN_VOID();
}


PG_FUNCTION_INFO_V1(setSystemVariable);
Datum
setSystemVariable(PG_FUNCTION_ARGS)
{
    char *varName;
    char *varValue;
    bool isSessionSystemVar;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_BOOL(false);
    }
    if (PG_ARGISNULL(2))
    {
        PG_RETURN_BOOL(false);
    }

    varName = TextDatumGetCString(PG_GETARG_DATUM(0));
    if (!PG_ARGISNULL(1))
    {
        varValue = TextDatumGetCString(PG_GETARG_DATUM(1));
    }
    else 
    {
        varValue = NULL;
    }
    isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(2));
    setSystemVariableValue(varName, varValue, isSessionSystemVar);
    PG_RETURN_BOOL(true);
}


PG_FUNCTION_INFO_V1(getSystemVariable);
Datum
getSystemVariable(PG_FUNCTION_ARGS)
{
    char *varName;
    bool isSessionSystemVar;
    char *varValue;

    varName = TextDatumGetCString(PG_GETARG_DATUM(0));
    isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(1));
    getSystemVariableValueForSelect(varName, 
                                    isSessionSystemVar, 
                                    &varValue);
    if (varValue != NULL)
    {
        PG_RETURN_TEXT_P(cstring_to_text(varValue));
    }
    else 
    {
        PG_RETURN_NULL();
    }
}

/* SHOW VARIABLES must expose the same live settings as @@variables, with
 * SHOW's enum spellings (for example ON/OFF), rather than placeholder values. */
PG_FUNCTION_INFO_V1(mys_show_system_variable);
Datum
mys_show_system_variable(PG_FUNCTION_ARGS)
{
	char *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char *value = NULL;

	if (!getSystemVariableValueForShow(name, PG_GETARG_BOOL(1), &value) ||
		value == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(value));
}


/*
 * mysql.getenv(name): environment variable of the server process (openHalo
 * issue 10, used for @@datadir / @@character_sets_dir at install time).
 *
 * Port note: postmasters started by pg_ctl inherit PGDATA, but Patroni and
 * plain "postgres -D" do not export it; fall back to the data directory so
 * CREATE EXTENSION does not depend on how the server was launched.
 */
PG_FUNCTION_INFO_V1(getENV);
Datum
getENV(PG_FUNCTION_ARGS)
{
    char *varName = NULL;
    char *envValue = NULL;
    if (PG_ARGISNULL(0))
        ereport(ERROR,
                (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                 errmsg("environment variable must not be null")));

    varName = TextDatumGetCString(PG_GETARG_DATUM(0));
    envValue = getenv(varName);
    if (envValue == NULL && strcmp(varName, "PGDATA") == 0 && DataDir != NULL)
        envValue = DataDir;
    if (envValue == NULL)
        elog(ERROR, "The environment variable %s is not set yet.", varName);

    PG_RETURN_TEXT_P(cstring_to_text(envValue));
}
