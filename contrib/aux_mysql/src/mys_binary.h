/* MySQL 1.9 length-aware binary and NUL-safe character strings. */
#ifndef MYS_BINARY_H
#define MYS_BINARY_H
#include "nodes/parsenodes.h"
extern bool mys_binary_types_available;
extern TypeName *mys_binary_type_name(const char *name, int32 typmod, int location);
/* Optional integration: inspect the current statement's INSERT IGNORE flag. */
extern bool (*mys_binary_ignore_hook)(void);
extern bool mys_is_fixed_binary(Oid typid);
extern bool mys_is_hexlit(Oid typid);
extern A_Expr *mys_hexlit_operator(A_Expr_Kind kind, char *name,
								  Node *left, Node *right, int location);
extern Node *mys_binary_func_call(FuncCall *call);
extern Node *mys_mysql_cast(Node *arg, TypeName *type, int location);
#endif
