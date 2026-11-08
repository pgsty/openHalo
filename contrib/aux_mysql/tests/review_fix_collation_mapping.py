#!/usr/bin/env python3
"""Collation fallback and MySQL diagnostic lifecycle checks.

This is an integration test for the MySQL wire endpoint after the parent
task installs the collation fallback change.  It deliberately does not test
the reference MySQL server: the reference keeps the requested collation
names, while this suite checks OpenHalo's explicit closest replacement.

Examples (run from inside ohdev, with the integrated server already running):

    python3 tests/review_fix_collation_mapping.py
    python3 tests/review_fix_collation_mapping.py --mysql-port 3791 \
        --pg-port 5791

The suite creates one unique review_fix_* database, changes no global
settings, and drops the database in finally.  The PostgreSQL check uses only
a temporary table.  It is expected to fail against a server that predates
the parent collation-warning implementation.
"""

import argparse
import json
import os
import sys
import time
import traceback

try:
    import pymysql
    import psycopg2
except ImportError as exc:
    print(json.dumps({"status": "SKIP", "reason": str(exc)}), flush=True)
    sys.exit(2)

from pymysql.constants import CLIENT


WARNING_CODE = 1105
GENERAL = "utf8mb4_general_ci"
BINARY = "utf8mb4_bin"


def mysql_connect(args, port, database=None):
    return pymysql.connect(
        host=args.host,
        port=port,
        user=args.user,
        password=args.password,
        database=database,
        charset="utf8mb4",
        autocommit=True,
        client_flag=CLIENT.MULTI_STATEMENTS,
        binary_prefix=True,
        connect_timeout=10,
        read_timeout=30,
        write_timeout=30,
    )


def warning_count(cursor):
    result = getattr(cursor, "_result", None)
    return int(getattr(result, "warning_count", 0) or 0)


def execute(conn, statement, args=None):
    """Return [(rows, OK/EOF warning_count)] for every result in a packet."""
    results = []
    with conn.cursor() as cursor:
        cursor.execute(statement, args)
        while True:
            rows = cursor.fetchall() if cursor.description else ()
            results.append((rows, warning_count(cursor)))
            if not cursor.nextset():
                break
    return results


def one(conn, statement, args=None):
    results = execute(conn, statement, args)
    assert len(results) == 1, (statement, results)
    return results[0]


def show_warnings(conn):
    return one(conn, "SHOW WARNINGS")[0]


def assert_warning(rows, requested, actual):
    matches = [
        row for row in rows
        if len(row) >= 3 and str(row[0]).lower() == "warning" and row[1] == WARNING_CODE
    ]
    assert len(matches) == 1, (requested, actual, rows)
    message = str(matches[0][2]).lower()
    assert requested.lower() in message, (requested, matches[0])
    assert actual.lower() in message, (actual, matches[0])


def assert_no_warnings(conn):
    rows = show_warnings(conn)
    assert rows == (), rows


def show_create(conn, table):
    rows = one(conn, "SHOW CREATE TABLE " + table)[0]
    assert len(rows) == 1 and len(rows[0]) >= 2, (table, rows)
    return str(rows[0][1])


def assert_actual_collation(conn, table, requested, actual, column=None):
    ddl = show_create(conn, table)
    lowered = ddl.lower()
    assert actual.lower() in lowered, (table, requested, actual, ddl)
    if requested.lower() != actual.lower():
        # The table name in this test deliberately contains some requested
        # aliases (for example exact_utf8_bin), so inspect only the collation
        # clauses rather than rejecting the alias anywhere in SHOW CREATE.
        for marker in (" collate ", " collate=", " default charset="):
            assert marker + requested.lower() not in lowered, (table, requested, ddl)

    if column is None:
        return

    rows = one(conn, "SHOW FULL COLUMNS FROM " + table)[0]
    matching = [row for row in rows if row and str(row[0]).lower() == column.lower()]
    assert len(matching) == 1, (table, column, rows)
    # MySQL's third SHOW FULL COLUMNS field is Collation.  The integrated
    # metadata layer emits the effective portable MySQL name here.
    assert str(matching[0][2]).lower() == actual.lower(), (table, column, matching[0])


def expect_error(conn, statement):
    try:
        one(conn, statement)
    except pymysql.MySQLError as exc:
        return exc
    raise AssertionError("expected MySQL error: " + statement)


def reset_connection(conn):
    # PyMySQL exposes COM_RESET_CONNECTION only through its low-level command
    # helpers.  This is the wire reset, not a SQL RESET statement.
    conn._execute_command(0x1F, b"")
    return conn._read_ok_packet()


def pg_check(args, pg_port):
    conn = psycopg2.connect(
        host=args.pg_host,
        port=pg_port,
        user=args.pg_user,
        password=args.pg_password,
        dbname=args.pg_database,
        connect_timeout=10,
    )
    conn.autocommit = True
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT 'utf8mb4_unicode_ci'::text, 1")
            assert cur.fetchone() == ("utf8mb4_unicode_ci", 1)
            cur.execute(
                'CREATE TEMP TABLE review_fix_pg_collation '
                '(v text COLLATE pg_catalog."C")'
            )
            cur.execute("INSERT INTO review_fix_pg_collation VALUES ('e'), ('E')")
            cur.execute(
                'SELECT count(*) FROM review_fix_pg_collation '
                'WHERE v COLLATE pg_catalog."C" = %s',
                ("e",),
            )
            assert cur.fetchone() == (1,)
    finally:
        conn.close()


def run(args):
    schema = "review_fix_collation_" + format(time.time_ns(), "x")
    mysql = None
    failures = []
    checks = 0

    def case(name, callback):
        nonlocal checks
        checks += 1
        try:
            callback()
            print(json.dumps({"case": name, "ok": True}), flush=True)
        except Exception as exc:
            failures.append(name)
            print(
                json.dumps({
                    "case": name,
                    "ok": False,
                    "error": str(exc),
                    "trace": traceback.format_exc(),
                }),
                flush=True,
            )

    try:
        mysql = mysql_connect(args, args.mysql_port)
        one(mysql, "CREATE DATABASE " + schema)
        one(mysql, "USE " + schema)

        def approximate_matrix():
            # These are common 5.7 names: the server default, Laravel's
            # default, and the WordPress 520 upgrade path.  utf8 aliases are
            # included because dumps and older frameworks use both spellings.
            cases = (
                ("latin1_swedish_ci", "map_latin1"),
                ("utf8_unicode_ci", "map_utf8_unicode"),
                ("utf8mb4_unicode_ci", "map_utf8mb4_unicode"),
                ("utf8_unicode_520_ci", "map_utf8_520"),
                ("utf8mb4_unicode_520_ci", "map_utf8mb4_520"),
            )
            for requested, table in cases:
                rows, count = one(
                    mysql,
                    "CREATE TABLE %s (v varchar(64)) COLLATE=%s" %
                    (table, requested),
                )
                assert rows == (), (table, rows)
                assert count == 1, (table, requested, count)
                assert_warning(show_warnings(mysql), requested, GENERAL)
                assert_actual_collation(mysql, table, requested, GENERAL, "v")

        case("common collations warn and map to general_ci", approximate_matrix)

        def exact_matrix():
            for requested, table, actual in (
                ("utf8_general_ci", "exact_utf8_general", GENERAL),
                ("utf8mb4_general_ci", "exact_utf8mb4_general", GENERAL),
                ("utf8_bin", "exact_utf8_bin", BINARY),
                ("utf8mb4_bin", "exact_utf8mb4_bin", BINARY),
            ):
                rows, count = one(
                    mysql,
                    "CREATE TABLE %s (v varchar(64)) COLLATE=%s" %
                    (table, requested),
                )
                assert rows == (), (table, rows)
                assert count == 0, (table, requested, count)
                assert_no_warnings(mysql)
                assert_actual_collation(mysql, table, requested, actual, "v")

        case("dedicated general_ci and bin have no warning", exact_matrix)

        def create_column_and_alter_lifecycle():
            rows, count = one(
                mysql,
                "CREATE TABLE alter_lifecycle "
                "(id int, explicit_col varchar(16) COLLATE utf8mb4_unicode_ci) "
                "COLLATE=utf8mb4_bin",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_ci", GENERAL)
            assert_actual_collation(mysql, "alter_lifecycle", "utf8mb4_unicode_ci", GENERAL, "explicit_col")

            # The table's default was exact bin, so a column without an
            # explicit COLLATE inherits bin and must not invent a warning.
            rows, count = one(
                mysql,
                "ALTER TABLE alter_lifecycle ADD inherited_col varchar(16)",
            )
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)
            assert_actual_collation(mysql, "alter_lifecycle", BINARY, BINARY, "inherited_col")

            rows, count = one(
                mysql,
                "ALTER TABLE alter_lifecycle ADD add_approx varchar(16) "
                "COLLATE utf8mb4_unicode_520_ci",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_520_ci", GENERAL)
            assert_actual_collation(mysql, "alter_lifecycle", "utf8mb4_unicode_520_ci", GENERAL, "add_approx")

            rows, count = one(
                mysql,
                "ALTER TABLE alter_lifecycle MODIFY add_approx varchar(32) "
                "COLLATE latin1_swedish_ci",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "latin1_swedish_ci", GENERAL)
            assert_actual_collation(mysql, "alter_lifecycle", "latin1_swedish_ci", GENERAL, "add_approx")

        case("CREATE and ALTER ADD/MODIFY warn once", create_column_and_alter_lifecycle)

        def like_does_not_repeat():
            rows, count = one(
                mysql,
                "CREATE TABLE like_source (v varchar(16)) "
                "COLLATE=utf8mb4_unicode_ci",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_ci", GENERAL)
            ddl = show_create(mysql, "like_source")
            assert GENERAL in ddl

            rows, count = one(mysql, "CREATE TABLE like_copy LIKE like_source")
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)
            assert_actual_collation(mysql, "like_copy", "utf8mb4_unicode_ci", GENERAL, "v")

            # A copied table's effective default also applies to later ADD
            # and MODIFY operations without replaying the source warning.
            rows, count = one(
                mysql,
                "ALTER TABLE like_copy ADD later_col varchar(16)",
            )
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)
            assert_actual_collation(mysql, "like_copy", "utf8mb4_unicode_ci", GENERAL, "later_col")

        case("CREATE TABLE LIKE preserves effective collation without duplicate warning", like_does_not_repeat)

        def expression_collations():
            rows, count = one(
                mysql,
                "SELECT 'a' COLLATE utf8mb4_unicode_ci",
            )
            assert rows == (("a",),) and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_ci", GENERAL)

            rows, count = one(
                mysql,
                "SELECT 'a' COLLATE utf8mb4_general_ci",
            )
            assert rows == (("a",),) and count == 0, (rows, count)
            assert_no_warnings(mysql)

            rows, count = one(
                mysql,
                "CREATE TABLE expr_ctas AS "
                "SELECT 'a' COLLATE utf8mb4_unicode_520_ci AS v",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_520_ci", GENERAL)
            assert_actual_collation(mysql, "expr_ctas", "utf8mb4_unicode_520_ci", GENERAL, "v")

            rows, count = one(
                mysql,
                "CREATE VIEW expr_view AS "
                "SELECT 'a' COLLATE latin1_swedish_ci AS v",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "latin1_swedish_ci", GENERAL)
            view_ddl = one(mysql, "SHOW CREATE VIEW expr_view")[0][0][1]
            assert GENERAL in str(view_ddl), view_ddl

            rows, count = one(mysql, "CREATE TABLE expr_dml (v varchar(16))")
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)
            for statement, requested in (
                ("INSERT INTO expr_dml VALUES ('a' COLLATE utf8mb4_unicode_ci)",
                 "utf8mb4_unicode_ci"),
                ("UPDATE expr_dml SET v='b' COLLATE utf8mb4_0900_ai_ci",
                 "utf8mb4_0900_ai_ci"),
                ("DELETE FROM expr_dml WHERE v='b' COLLATE made_up_ci",
                 "made_up_ci"),
            ):
                rows, count = one(mysql, statement)
                assert_warning(show_warnings(mysql), requested, GENERAL)

        case("expression COLLATE maps and warns in DML, CTAS and views", expression_collations)

        def sql_prepare_execute():
            prepare_sql = (
                "PREPARE collation_ps FROM "
                "'CREATE TABLE prepared_map (v varchar(16) COLLATE utf8mb4_unicode_ci)'"
            )
            rows, count = one(mysql, prepare_sql)
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)

            rows, count = one(mysql, "EXECUTE collation_ps")
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_ci", GENERAL)
            assert_actual_collation(mysql, "prepared_map", "utf8mb4_unicode_ci", GENERAL, "v")

            rows, count = one(mysql, "DEALLOCATE PREPARE collation_ps")
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)

        case("SQL PREPARE defers warning until EXECUTE", sql_prepare_execute)

        def multi_statement_boundaries():
            results = execute(
                mysql,
                "CREATE TABLE multi_approx (v varchar(16)) "
                "COLLATE=utf8mb4_unicode_ci; SHOW WARNINGS; "
                "CREATE TABLE multi_exact (v varchar(16)) "
                "COLLATE=utf8mb4_general_ci; SHOW WARNINGS",
            )
            assert len(results) == 4, results
            assert results[0][1] == 1, results
            assert_warning(results[1][0], "utf8mb4_unicode_ci", GENERAL)
            assert results[2][1] == 0, results
            assert results[3][0] == (), results
            assert_actual_collation(mysql, "multi_approx", "utf8mb4_unicode_ci", GENERAL, "v")
            assert_actual_collation(mysql, "multi_exact", GENERAL, GENERAL, "v")

        case("multi statement diagnostics do not cross statement boundaries", multi_statement_boundaries)

        def error_and_reset_clear_diagnostics():
            rows, count = one(
                mysql,
                "CREATE TABLE reset_warning (v varchar(16)) "
                "COLLATE=utf8mb4_unicode_520_ci",
            )
            assert rows == () and count == 1, (rows, count)
            assert_warning(show_warnings(mysql), "utf8mb4_unicode_520_ci", GENERAL)

            expect_error(mysql, "SELECT * FROM no_such_review_fix_table")
            rows = show_warnings(mysql)
            assert rows and not any(row[1] == WARNING_CODE for row in rows), rows
            assert any(str(row[0]).lower() == "error" for row in rows), rows

            ok = reset_connection(mysql)
            assert int(getattr(ok, "warning_count", 0) or 0) == 0, ok
            assert_no_warnings(mysql)

            rows, count = one(
                mysql,
                "CREATE TABLE after_reset (v varchar(16)) COLLATE=utf8mb4_bin",
            )
            assert rows == () and count == 0, (rows, count)
            assert_no_warnings(mysql)

        case("errors and COM_RESET_CONNECTION do not leak warnings", error_and_reset_clear_diagnostics)

        def native_pg_session():
            pg_check(args, args.pg_port)

        case("ordinary PostgreSQL session remains unaffected", native_pg_session)

    finally:
        if mysql is not None:
            try:
                one(mysql, "DROP DATABASE IF EXISTS " + schema)
            except Exception as exc:
                print(json.dumps({"cleanup_error": str(exc)}), flush=True)
            mysql.close()

    print(json.dumps({"port": args.mysql_port, "checks": checks, "failures": failures}), flush=True)
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.getenv("MYSQL_HOST", "127.0.0.1"))
    parser.add_argument("--mysql-port", type=int, default=int(os.getenv("MYSQL_PORT", "3790")))
    parser.add_argument("--user", default=os.getenv("MYSQL_USER", "dbuser_mysql"))
    parser.add_argument("--password", default=os.getenv("MYSQL_PASSWORD", "Mysql.123"))
    parser.add_argument("--pg-host", default=os.getenv("PGHOST", "127.0.0.1"))
    parser.add_argument(
        "--pg-port",
        type=int,
        default=int(os.getenv("PGPORT", "0")) or None,
    )
    parser.add_argument("--pg-user", default=os.getenv("PGUSER", "dbuser_mysql"))
    parser.add_argument("--pg-password", default=os.getenv("PGPASSWORD", "Mysql.123"))
    parser.add_argument("--pg-database", default=os.getenv("PGDATABASE", "postgres"))
    args = parser.parse_args()
    if args.pg_port is None:
        args.pg_port = args.mysql_port + 2000
    sys.exit(run(args))


if __name__ == "__main__":
    main()
