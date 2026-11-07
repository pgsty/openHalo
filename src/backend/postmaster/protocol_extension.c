/*-------------------------------------------------------------------------
 *
 * protocol_extension.c
 *	  Registry of pluggable wire protocols.
 *
 * Protocol ids are assigned in registration order, starting at 1 (0 is the
 * native protocol).  Since shared_preload_libraries are loaded in the same
 * order in the postmaster and (under EXEC_BACKEND) in every child, ids are
 * stable across processes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/protocol_extension.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/protocol_extension.h"

const ProtocolExtensionConfig *MyProtocolExtension = NULL;

static const ProtocolExtensionConfig *ProtocolExtensions[MAX_PROTOCOL_EXTENSIONS];
static int	num_protocol_extensions = 0;

int
RegisterProtocolExtension(const ProtocolExtensionConfig *config)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("protocol extension \"%s\" must be loaded via \"shared_preload_libraries\"",
						config->name)));

	if (num_protocol_extensions >= MAX_PROTOCOL_EXTENSIONS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many protocol extensions registered")));

	ProtocolExtensions[num_protocol_extensions++] = config;
	return num_protocol_extensions;
}

const ProtocolExtensionConfig *
GetProtocolExtension(int protocol_id)
{
	if (protocol_id <= 0 || protocol_id > num_protocol_extensions)
		return NULL;
	return ProtocolExtensions[protocol_id - 1];
}

int
NumProtocolExtensions(void)
{
	return num_protocol_extensions;
}
