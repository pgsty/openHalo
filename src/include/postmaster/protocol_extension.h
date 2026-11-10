/*-------------------------------------------------------------------------
 *
 * protocol_extension.h
 *	  Pluggable wire protocol interface.
 *
 * An extension loaded via shared_preload_libraries can register an
 * alternative wire protocol (e.g. MySQL, TDS).  The postmaster opens extra
 * listen sockets on behalf of the extension; connections accepted on those
 * sockets are served by regular backends, but every point where the backend
 * talks to its client is routed through the callbacks below instead of the
 * native PostgreSQL frontend/backend protocol.
 *
 * The callback set is modeled after the Babelfish "ProtocolExtensionConfig"
 * so that existing protocol implementations can be adapted with little
 * effort.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * src/include/postmaster/protocol_extension.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROTOCOL_EXTENSION_H
#define PROTOCOL_EXTENSION_H

#include "lib/stringinfo.h"
#include "nodes/pg_list.h"
#include "tcop/cmdtag.h"
#include "tcop/dest.h"

struct Port;
struct ErrorData;
struct TupleTableSlot;
struct RelationData;

/* Maximum number of protocol extensions that can be registered */
#define MAX_PROTOCOL_EXTENSIONS 8

typedef struct ProtocolExtensionConfig
{
	/* Short protocol name, used in log messages */
	const char *name;

	/*
	 * Postmaster: open listen sockets.  Called once during postmaster
	 * startup, after the native sockets are created.  The callback is
	 * expected to call ListenServerPortForProtocol().
	 */
	void		(*listen_init) (void);

	/*
	 * Backend, before InitProcess(): perform the protocol handshake and fill
	 * in port->user_name and port->database_name.  Shared memory is not
	 * available yet.  Returns STATUS_OK or STATUS_ERROR.
	 */
	int			(*start) (struct Port *port);

	/*
	 * Backend, inside InitPostgres(): authenticate the client.  Replaces
	 * ClientAuthentication(); must ereport(FATAL) on failure.
	 */
	void		(*authenticate) (struct Port *port);

	/* Send an error/notice to the client; replaces send_message_to_frontend */
	void		(*send_message) (struct ErrorData *edata);

	/* Called at the end of each command cycle; replaces ReadyForQuery */
	void		(*send_ready_for_query) (CommandDest dest);

	/*
	 * Read the next command from the client; replaces SocketBackend().
	 * Returns a message type byte (or EOF).  Returning PqMsg_Query with a
	 * NUL-terminated query string in inBuf makes the regular simple-query
	 * path execute it.
	 */
	int			(*read_command) (StringInfo inBuf);

	/*
	 * Process a command that is not handled by the regular main loop.
	 * Return true if the message was consumed.
	 */
	bool		(*process_command) (int firstchar, StringInfo inBuf);

	/*
	 * Parse a client-supplied query string (simple or extended query) into a
	 * list of RawStmt.  SQL function bodies and other internal parsing keep
	 * using the native grammar.  NULL means use the native parser.
	 */
	List	   *(*parse_query) (const char *query_string);

	/* DestReceiver for client-bound tuples (DestRemote/DestRemoteExecute) */
	DestReceiver *(*create_dest_receiver) (CommandDest dest);

	/* Command completion / empty query; replace EndCommand/NullCommand */
	void		(*end_command) (const QueryCompletion *qc, CommandDest dest);
	void		(*null_command) (CommandDest dest);

	/* Report a GUC_REPORT parameter change; NULL means do not report */
	void		(*report_param_status) (const char *name, const char *value);

	/* Reset protocol state during error recovery (optional) */
	void		(*comm_reset) (void);

	/* Optional SQL dialect semantics; native sessions never use these. */
	bool		(*foreign_key_checks) (void);
	void		(*foreign_key_insert) (struct RelationData *relation,
								  struct TupleTableSlot *slot);
	void		(*sequence_write) (struct RelationData *sequence);
	void		(*on_conflict_row) (struct TupleTableSlot *oldslot,
								struct TupleTableSlot *newslot, uint64 deleted);
} ProtocolExtensionConfig;

/*
 * Protocol serving the current backend, or NULL for the native protocol.
 */
extern PGDLLIMPORT const ProtocolExtensionConfig *MyProtocolExtension;

/*
 * Register a protocol extension.  Must be called from _PG_init() of a
 * library listed in shared_preload_libraries.  Returns the protocol id to
 * pass to ListenServerPortForProtocol().
 */
extern int	RegisterProtocolExtension(const ProtocolExtensionConfig *config);
extern const ProtocolExtensionConfig *GetProtocolExtension(int protocol_id);
extern int	NumProtocolExtensions(void);

/* Postmaster: open a listen socket served by the given protocol */
extern int	ListenServerPortForProtocol(int protocol_id, int family,
										const char *hostName,
										unsigned short portNumber,
										const char *unixSocketDir);

#endif							/* PROTOCOL_EXTENSION_H */
