#!/usr/bin/env python3
"""DML/FK release regressions; disposable schemas, no global setting changes.

MYSQL_PORT/PGPORT select OpenHalo. MYSQL_REFERENCE_PORT optionally repeats the
SQL cases against MySQL 5.7. PG* and MYSQL_* credentials follow ab_regress.py.
"""
import concurrent.futures
import os
import threading
import uuid

import psycopg2
import pymysql


SCHEMA = "review_fix_dml_" + uuid.uuid4().hex[:10]
USER = os.getenv("MYSQL_USER", "dbuser_mysql")
PASSWORD = os.getenv("MYSQL_PASSWORD", "Mysql.123")


def connect(port):
    return pymysql.connect(host=os.getenv("MYSQL_HOST", "127.0.0.1"),
                           port=port, user=USER, password=PASSWORD,
                           autocommit=True, read_timeout=20, write_timeout=20)


def query(c, sql):
    with c.cursor() as q:
        q.execute(sql)
        return q.fetchall() if q.description else q.rowcount


def equal(actual, expected):
    assert actual == expected, (actual, expected)


def rejects(c, sql, code):
    try:
        query(c, sql)
    except pymysql.MySQLError as e:
        equal(e.args[0], code)
    else:
        raise AssertionError("unexpected success: " + sql)


def sql_cases(c):
    query(c, "CREATE TABLE p(id INT PRIMARY KEY,v INT)")
    query(c, "CREATE TABLE ch(pid INT, FOREIGN KEY(pid) REFERENCES p(id) ON DELETE CASCADE)")
    query(c, "INSERT INTO p VALUES(1,10)")
    query(c, "INSERT INTO ch VALUES(1)")
    equal(query(c, "REPLACE INTO p VALUES(1,20)"), 2)
    equal(query(c, "SELECT * FROM ch"), ())

    for action in ("RESTRICT", "NO ACTION"):
        name = "restrict_fk" if action == "RESTRICT" else "noaction_fk"
        query(c, f"CREATE TABLE {name}(pid INT, FOREIGN KEY(pid) REFERENCES p(id) ON DELETE {action})")
        query(c, f"INSERT INTO {name} VALUES(1)")
        rejects(c, "REPLACE INTO p VALUES(1,30)", 1451)
        equal(query(c, "SELECT v FROM p"), ((20,),))
        query(c, f"DROP TABLE {name}")

    query(c, "CREATE TABLE repeated(id INT PRIMARY KEY,v INT)")
    equal(query(c, "INSERT INTO repeated VALUES(1,1),(1,2) ON DUPLICATE KEY UPDATE v=VALUES(v)"), 3)
    equal(query(c, "INSERT INTO repeated VALUES(1,2),(1,2) ON DUPLICATE KEY UPDATE v=VALUES(v)"), 0)
    equal(query(c, "SELECT * FROM repeated"), ((1, 2),))
    equal(query(c, "INSERT INTO repeated SELECT id,v+1 FROM repeated ON DUPLICATE KEY UPDATE v=VALUES(v)"), 2)
    equal(query(c, "SELECT * FROM repeated"), ((1, 3),))

    query(c, "CREATE TABLE multi(a INT PRIMARY KEY,b INT UNIQUE)")
    query(c, "INSERT INTO multi VALUES(1,10),(2,20)")
    equal(query(c, "REPLACE INTO multi VALUES(1,20)"), 3)
    equal(query(c, "SELECT * FROM multi"), ((1, 20),))
    equal(query(c, "INSERT INTO multi VALUES(3,20) ON DUPLICATE KEY UPDATE a=VALUES(a)"), 2)
    equal(query(c, "SELECT * FROM multi"), ((3, 20),))

    query(c, "CREATE TABLE gen(id INT PRIMARY KEY,v INT,g INT AS(v*2) STORED UNIQUE)")
    query(c, "INSERT INTO gen(id,v) VALUES(1,2)")
    equal(query(c, "REPLACE INTO gen(id,v) VALUES(2,2)"), 2)
    equal(query(c, "SELECT * FROM gen"), ((2, 2, 4),))
    equal(query(c, "INSERT INTO gen(id,v) VALUES(3,2) ON DUPLICATE KEY UPDATE id=VALUES(id)"), 2)
    equal(query(c, "SELECT * FROM gen"), ((3, 2, 4),))

    query(c, "CREATE TABLE defaults(id INT PRIMARY KEY,v INT DEFAULT 7)")
    query(c, "INSERT INTO defaults VALUES(1,99)")
    equal(query(c, "REPLACE INTO defaults(id) VALUES(1)"), 2)
    equal(query(c, "SELECT * FROM defaults"), ((1, 7),))
    equal(query(c, "REPLACE INTO defaults(id) VALUES(1)"), 1)

    equal(query(c, "INSERT IGNORE INTO ch VALUES(1),(999),(1)"), 2)
    warnings = query(c, "SHOW WARNINGS")
    equal([(r[0], r[1]) for r in warnings], [("Warning", 1452)])
    equal(query(c, "SELECT * FROM ch"), ((1,), (1,)))
    query(c, "CREATE TABLE self_fk(id INT PRIMARY KEY,pid INT, FOREIGN KEY(pid) REFERENCES self_fk(id))")
    equal(query(c, "INSERT IGNORE INTO self_fk VALUES(1,1),(2,99),(3,1)"), 2)
    equal(query(c, "SELECT * FROM self_fk ORDER BY id"), ((1, 1), (3, 1)))

    query(c, "SET FOREIGN_KEY_CHECKS=0")
    query(c, "TRUNCATE p")
    equal(query(c, "SELECT * FROM ch"), ((1,), (1,)))
    equal(query(c, "INSERT IGNORE INTO ch VALUES(999)"), 1)
    query(c, "SET FOREIGN_KEY_CHECKS=1")
    rejects(c, "INSERT INTO ch VALUES(998)", 1452)
    equal(query(c, "SELECT COUNT(*) FROM ch"), ((3,),))


def trigger_cases(c, pg):
    query(c, "CREATE TABLE trig(id INT PRIMARY KEY,v INT,n INT DEFAULT 0)")
    with pg.cursor() as q:
        q.execute(f"""CREATE FUNCTION {SCHEMA}.bump() RETURNS trigger
          LANGUAGE plpgsql AS $$BEGIN NEW.n=OLD.n+1; RETURN NEW; END$$;
          CREATE TRIGGER bump BEFORE UPDATE ON {SCHEMA}.trig
          FOR EACH ROW EXECUTE FUNCTION {SCHEMA}.bump();""")
    query(c, "INSERT INTO trig VALUES(1,10,0)")
    equal(query(c, "INSERT INTO trig(id,v) VALUES(1,10) ON DUPLICATE KEY UPDATE v=VALUES(v)"), 2)
    equal(query(c, "SELECT n FROM trig"), ((1,),))
    query(c, "CREATE TABLE user_trigger(v INT)")
    with pg.cursor() as q:
        q.execute(f"""CREATE FUNCTION {SCHEMA}.set_value() RETURNS trigger
          LANGUAGE plpgsql AS $$BEGIN NEW.v=99; RETURN NEW; END$$;
          CREATE TRIGGER set_value BEFORE INSERT ON {SCHEMA}.user_trigger
          FOR EACH ROW EXECUTE FUNCTION {SCHEMA}.set_value();""")
    query(c, "SET FOREIGN_KEY_CHECKS=0")
    query(c, "INSERT INTO user_trigger VALUES(1)")
    query(c, "SET FOREIGN_KEY_CHECKS=1")
    equal(query(c, "SELECT v FROM user_trigger"), ((99,),))

    query(c, "CREATE TABLE same_replace(id INT PRIMARY KEY)")
    query(c, "CREATE TABLE insert_audit(id INT)")
    with pg.cursor() as q:
        q.execute(f"""CREATE FUNCTION {SCHEMA}.audit_insert() RETURNS trigger
          LANGUAGE plpgsql AS $$BEGIN INSERT INTO {SCHEMA}.insert_audit VALUES(NEW.id); RETURN NEW; END$$;
          CREATE TRIGGER audit_insert AFTER INSERT ON {SCHEMA}.same_replace
          FOR EACH ROW EXECUTE FUNCTION {SCHEMA}.audit_insert();""")
    query(c, "INSERT INTO same_replace VALUES(1)")
    equal(query(c, "REPLACE INTO same_replace VALUES(1)"), 1)
    equal(query(c, "SELECT COUNT(*) FROM insert_audit"), ((2,),))

    # The native PG executor retains standard cardinality semantics.
    with pg.cursor() as q:
        q.execute(f"CREATE TABLE {SCHEMA}.native_pg(id INT PRIMARY KEY,v INT)")
        try:
            q.execute(f"INSERT INTO {SCHEMA}.native_pg VALUES(1,1),(1,2) ON CONFLICT(id) DO UPDATE SET v=EXCLUDED.v")
        except psycopg2.Error as e:
            equal(e.pgcode, "21000")
        else:
            raise AssertionError("native PostgreSQL cardinality check changed")


def concurrent_case(c, port, openhalo=True):
    # A known duplicate must be skipped before waiting on an irrelevant FK.
    query(c, "CREATE TABLE lock_parent(id INT PRIMARY KEY)")
    query(c, "CREATE TABLE lock_child(id INT PRIMARY KEY,pid INT,FOREIGN KEY(pid) REFERENCES lock_parent(id))")
    query(c, "INSERT INTO lock_parent VALUES(1),(2)")
    query(c, "INSERT INTO lock_child VALUES(1,1)")
    owner = connect(port)
    try:
        query(owner, f"USE {SCHEMA}")
        query(owner, "BEGIN")
        query(owner, "DELETE FROM lock_parent WHERE id=2")
        if openhalo:
            query(c, "SELECT pg_catalog.set_config('lock_timeout','1s',false)")
        equal(query(c, "INSERT IGNORE INTO lock_child VALUES(1,2)"), 0)
        equal([(r[0], r[1]) for r in query(c, "SHOW WARNINGS")], [("Warning", 1062)])
    finally:
        if openhalo:
            query(c, "SELECT pg_catalog.set_config('lock_timeout','0',false)")
        query(owner, "ROLLBACK")
        owner.close()

    query(c, "CREATE TABLE increments(id INT PRIMARY KEY,v INT)")
    query(c, "INSERT INTO increments VALUES(1,0)")
    gate = threading.Barrier(2)

    def writer(_):
        x = connect(port)
        try:
            query(x, f"USE {SCHEMA}")
            gate.wait(timeout=10)
            for __ in range(30):
                equal(query(x, "INSERT INTO increments VALUES(1,1),(1,1) ON DUPLICATE KEY UPDATE v=v+1"), 4)
        finally:
            x.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        list(pool.map(writer, range(2)))
    equal(query(c, "SELECT v FROM increments"), ((120,),))


def auto_increment_permission_cases(c, pg, x, role):
    query(c, "CREATE TABLE limited_auto(id INT AUTO_INCREMENT PRIMARY KEY,v INT UNIQUE,secret INT DEFAULT 42)")
    with pg.cursor() as q:
        q.execute(f"GRANT INSERT(v) ON {SCHEMA}.limited_auto TO {role}")

    # The protocol needs the generated ID even without SELECT(id) or INSERT(id).
    with x.cursor() as q:
        equal(q.execute("INSERT INTO limited_auto(v) VALUES(7)"), 1)
        insert_id = q.lastrowid
    assert insert_id > 0, insert_id
    equal(query(x, "SELECT LAST_INSERT_ID()"), ((insert_id,),))
    equal(query(c, "SELECT id,v,secret FROM limited_auto"), ((insert_id, 7, 42),))

    # Exempting hidden RETURNING must preserve all actual statement privileges.
    rejects(x, "SELECT id FROM limited_auto", 1142)
    rejects(x, "INSERT INTO limited_auto(secret) VALUES(99)", 1142)
    rejects(x, "INSERT INTO limited_auto(v) SELECT secret FROM limited_auto", 1142)
    rejects(x, "INSERT INTO limited_auto(v) VALUES(7) ON DUPLICATE KEY UPDATE v=7", 1142)
    with pg.cursor() as q:
        q.execute(f"GRANT UPDATE(v) ON {SCHEMA}.limited_auto TO {role}")
    equal(query(x, "INSERT INTO limited_auto(v) VALUES(7) ON DUPLICATE KEY UPDATE v=7"), 0)
    rejects(x, "INSERT INTO limited_auto(v) VALUES(7) ON DUPLICATE KEY UPDATE v=secret", 1142)
    equal(query(c, "SELECT id,v,secret FROM limited_auto"), ((insert_id, 7, 42),))

    # Native explicit RETURNING still requires SELECT on its referenced columns.
    with pg.cursor() as q:
        q.execute(f"SET ROLE {role}")
        try:
            try:
                q.execute(f"INSERT INTO {SCHEMA}.limited_auto(v) VALUES(8) RETURNING id")
            except psycopg2.Error as e:
                equal(e.pgcode, "42501")
            else:
                raise AssertionError("explicit RETURNING bypassed SELECT(id)")
        finally:
            q.execute("RESET ROLE")
        q.execute(f"GRANT SELECT(id) ON {SCHEMA}.limited_auto TO {role}")
        q.execute(f"SET ROLE {role}")
        try:
            q.execute(f"INSERT INTO {SCHEMA}.limited_auto(v) VALUES(8) RETURNING id")
            explicit_id = q.fetchone()[0]
        finally:
            q.execute("RESET ROLE")
    equal(query(c, "SELECT id,v,secret FROM limited_auto WHERE v=8"), ((explicit_id, 8, 42),))


def permission_cases(c, pg, port):
    role = SCHEMA + "_writer"
    with pg.cursor() as q:
        q.execute(f"CREATE ROLE {role} LOGIN PASSWORD %s", (PASSWORD,))
        q.execute(f"GRANT USAGE ON SCHEMA {SCHEMA},mysql TO {role}")
        q.execute(f"CREATE TABLE {SCHEMA}.limited(id INT PRIMARY KEY,v INT,secret TEXT)")
        q.execute(f"INSERT INTO {SCHEMA}.limited VALUES(1,1,'private')")
        q.execute(f"GRANT INSERT(id,v),UPDATE(v) ON {SCHEMA}.limited TO {role}")
        q.execute(f"CREATE TABLE {SCHEMA}.guarded(id INT PRIMARY KEY,v INT)")
        q.execute(f"INSERT INTO {SCHEMA}.guarded VALUES(1,1)")
        q.execute(f"ALTER TABLE {SCHEMA}.guarded ENABLE ROW LEVEL SECURITY")
        q.execute(f"CREATE POLICY ins ON {SCHEMA}.guarded FOR INSERT WITH CHECK(true)")
        q.execute(f"CREATE POLICY del ON {SCHEMA}.guarded FOR DELETE USING(id<>1)")
        q.execute(f"GRANT INSERT,DELETE ON {SCHEMA}.guarded TO {role}")
    x = None
    try:
        x = pymysql.connect(host=os.getenv("MYSQL_HOST", "127.0.0.1"), port=port,
                            user=role, password=PASSWORD, autocommit=True)
        query(x, f"USE {SCHEMA}")
        equal(query(x, "INSERT INTO limited(id,v) VALUES(1,2) ON DUPLICATE KEY UPDATE v=VALUES(v)"), 2)
        equal(query(c, "SELECT v,secret FROM limited"), ((2, 'private'),))
        rejects(x, "REPLACE INTO guarded VALUES(1,2)", 1142)
        equal(query(c, "SELECT * FROM guarded"), ((1, 1),))
        auto_increment_permission_cases(c, pg, x, role)
    finally:
        if x:
            x.close()
        with pg.cursor() as q:
            q.execute(f"DROP OWNED BY {role}")
            q.execute(f"DROP ROLE {role}")


def main():
    port = int(os.getenv("MYSQL_PORT", "3306"))
    pg = psycopg2.connect(host=os.getenv("PGHOST", "127.0.0.1"),
                          port=int(os.getenv("PGPORT", "5432")),
                          user=os.getenv("PGUSER", "postgres"),
                          password=os.getenv("PGPASSWORD", ""),
                          dbname=os.getenv("PGDATABASE", "postgres"))
    pg.autocommit = True
    ports = [port]
    if os.getenv("MYSQL_REFERENCE_PORT"):
        ports.append(int(os.environ["MYSQL_REFERENCE_PORT"]))
    try:
        for target in ports:
            c = connect(target)
            try:
                query(c, f"CREATE DATABASE {SCHEMA}")
                query(c, f"USE {SCHEMA}")
                sql_cases(c)
                concurrent_case(c, target, target == port)
                if target == port:
                    trigger_cases(c, pg)
                    permission_cases(c, pg, target)
                print(f"PASS release DML/FK port={target}", flush=True)
            finally:
                query(c, "SET FOREIGN_KEY_CHECKS=0")
                query(c, f"DROP DATABASE IF EXISTS {SCHEMA}")
                c.close()
    finally:
        pg.close()


if __name__ == "__main__":
    main()
