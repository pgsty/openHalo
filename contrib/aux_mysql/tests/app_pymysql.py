import os
import pymysql, datetime, decimal

conn = pymysql.connect(host="127.0.0.1", port=int(os.environ.get("MYSQL_PORT", "3306")), user="dbuser_mysql",
                       password="Mysql.123", database="test", charset="utf8mb4", autocommit=True)
cur = conn.cursor(pymysql.cursors.DictCursor)
print("server:", conn.get_server_info())

cur.execute("DROP TABLE IF EXISTS orders")
cur.execute("""
CREATE TABLE orders (
  id int unsigned NOT NULL AUTO_INCREMENT,
  user_id int NOT NULL,
  amount decimal(12,2) NOT NULL DEFAULT '0.00',
  status tinyint NOT NULL DEFAULT 0,
  note varchar(255) DEFAULT NULL,
  created_at datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_user (user_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""")

# parameterized insert (client-side escaping) + lastrowid
cur.execute("INSERT INTO orders (user_id, amount, note) VALUES (%s, %s, %s)", (1, decimal.Decimal("99.90"), "it's \"quoted\""))
print("lastrowid:", cur.lastrowid)
n = cur.executemany("INSERT INTO orders (user_id, amount, status) VALUES (%s, %s, %s)",
                    [(2, 10, 1), (2, 20, 1), (3, 30, 0)])
print("executemany rows:", n, "lastrowid:", cur.lastrowid)

cur.execute("SELECT user_id, COUNT(*) AS cnt, SUM(amount) AS total FROM orders GROUP BY user_id ORDER BY user_id")
for r in cur.fetchall():
    print("  agg:", r)

cur.execute("SELECT * FROM orders WHERE id = %s", (1,))
row = cur.fetchone()
print("row:", {k: (type(v).__name__, v) for k, v in row.items()})

# transaction
conn.begin()
cur.execute("UPDATE orders SET status = 2 WHERE user_id = %s", (2,))
print("updated:", cur.rowcount)
conn.rollback()
cur.execute("SELECT COUNT(*) AS c FROM orders WHERE status = 2")
print("after rollback status=2:", cur.fetchone()["c"])

cur.execute("SELECT id, note FROM orders WHERE note LIKE %s", ("%quoted%",))
print("like:", cur.fetchall())
cur.execute("SELECT NOW() AS now, CURDATE() AS today, VERSION() AS v")
print("misc:", cur.fetchone())
try:
    cur.execute("SELECT * FROM no_such_table")
except pymysql.err.ProgrammingError as e:
    print("error ok:", e.args)
conn.close()
