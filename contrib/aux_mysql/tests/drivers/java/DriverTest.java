/*
 * MySQL Connector/J against aux_mysql, default driver settings unless noted.
 *
 *   javac -cp mysql-connector-j.jar DriverTest.java
 *   java -cp mysql-connector-j.jar:. DriverTest [auth|stmt|txn|all]
 */
import java.math.BigDecimal;
import java.sql.*;
import java.util.Arrays;

public class DriverTest {
    static final String URL = "jdbc:mysql://127.0.0.1:"
        + (System.getenv("MYSQL_PORT") != null ? System.getenv("MYSQL_PORT") : "3306") + "/test";
    static int fails = 0;

    static void check(String name, boolean ok, Object detail) {
        System.out.println((ok ? "  ok   " : "  FAIL ") + name + (ok ? "" : "  -> " + detail));
        if (!ok) fails++;
    }

    static Connection conn(String props) throws SQLException {
        return DriverManager.getConnection(props.isEmpty() ? URL : URL + "?" + props,
                                           "dbuser_mysql", "Mysql.123");
    }

    static String one(Connection c, String sql) throws SQLException {
        try (Statement s = c.createStatement(); ResultSet rs = s.executeQuery(sql)) {
            return rs.next() ? rs.getString(1) : null;
        }
    }

    static void auth() {
        System.out.println("== auth");
        String[][] cases = {
            {"default (sslMode=PREFERRED -> TLS)", ""},
            {"sslMode=REQUIRED", "sslMode=REQUIRED"},
            {"sslMode=DISABLED + allowPublicKeyRetrieval (RSA)", "sslMode=DISABLED&allowPublicKeyRetrieval=true"},
            {"sslMode=DISABLED, cached (fast auth)", "sslMode=DISABLED"},
        };
        for (String[] t : cases) {
            try (Connection c = conn(t[1])) {
                String ssl = one(c, "select ssl from pg_stat_ssl where pid = pg_backend_pid()");
                check(t[0] + " ssl=" + ssl, "42".equals(one(c, "select 40 + 2")), "bad result");
            } catch (Exception e) {
                check(t[0], false, e);
            }
        }
        try (Connection c = DriverManager.getConnection(URL, "dbuser_mysql", "wrong")) {
            check("wrong password rejected", false, "connected");
        } catch (SQLException e) {
            check("wrong password rejected (" + e.getErrorCode() + ")", e.getErrorCode() == 1045, e);
        }
    }

    static void stmt() throws SQLException {
        System.out.println("== server-side prepared statements (useServerPrepStmts=true)");
        try (Connection c = conn("useServerPrepStmts=true&cachePrepStmts=true")) {
            Statement s = c.createStatement();
            s.execute("drop table if exists jps");
            s.execute("create table jps (id int not null auto_increment primary key, i bigint, " +
                      "d double, n decimal(10,3), v varchar(64), t text, dt datetime(6), " +
                      "dd date, b blob, f tinyint)");
            PreparedStatement ins = c.prepareStatement(
                "insert into jps (i, d, n, v, t, dt, dd, b, f) values (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                Statement.RETURN_GENERATED_KEYS);
            ins.setLong(1, 9007199254740993L);
            ins.setDouble(2, 3.25);
            ins.setBigDecimal(3, new BigDecimal("1234.567"));
            ins.setString(4, "it's a \\ test -- 'x'");
            ins.setString(5, "中文 text");
            ins.setTimestamp(6, Timestamp.valueOf("2024-02-29 13:45:01.5"));
            ins.setDate(7, Date.valueOf("2024-03-01"));
            ins.setBytes(8, new byte[] {0, 1, (byte) 0xff, 0x27, 0x5c});
            ins.setInt(9, -5);
            check("insert update count", ins.executeUpdate() == 1, "");
            ResultSet gk = ins.getGeneratedKeys();
            check("generated key", gk.next() && gk.getLong(1) == 1, "");
            ins.setNull(1, Types.BIGINT);
            ins.setNull(2, Types.DOUBLE);
            ins.setNull(3, Types.DECIMAL);
            ins.setNull(4, Types.VARCHAR);
            ins.setNull(5, Types.VARCHAR);
            ins.setNull(6, Types.TIMESTAMP);
            ins.setNull(7, Types.DATE);
            ins.setNull(8, Types.BLOB);
            ins.setNull(9, Types.TINYINT);
            check("insert nulls (re-execute)", ins.executeUpdate() == 1, "");

            PreparedStatement sel = c.prepareStatement(
                "select id, i, d, n, v, t, dt, dd, b, f from jps where id = ?");
            check("result metadata at prepare", sel.getMetaData() != null &&
                  sel.getMetaData().getColumnCount() == 10, sel.getMetaData());
            sel.setInt(1, 1);
            ResultSet rs = sel.executeQuery();
            check("row found", rs.next(), "");
            check("bigint", rs.getLong(2) == 9007199254740993L, rs.getString(2));
            check("double", rs.getDouble(3) == 3.25, rs.getString(3));
            check("decimal", new BigDecimal("1234.567").compareTo(rs.getBigDecimal(4)) == 0, rs.getString(4));
            check("varchar w/ quotes", "it's a \\ test -- 'x'".equals(rs.getString(5)), rs.getString(5));
            check("text utf8", "中文 text".equals(rs.getString(6)), rs.getString(6));
            check("datetime", Timestamp.valueOf("2024-02-29 13:45:01.5").equals(rs.getTimestamp(7)), rs.getTimestamp(7));
            check("date", Date.valueOf("2024-03-01").equals(rs.getDate(8)), rs.getDate(8));
            check("blob", Arrays.equals(new byte[] {0, 1, (byte) 0xff, 0x27, 0x5c}, rs.getBytes(9)),
                  Arrays.toString(rs.getBytes(9)));
            check("tinyint", rs.getInt(10) == -5, rs.getString(10));
            check("single row", !rs.next(), "");

            sel.setInt(1, 2);
            rs = sel.executeQuery();
            rs.next();
            rs.getLong(2);
            check("nulls", rs.wasNull() && rs.getString(5) == null && rs.getTimestamp(7) == null, "");

            PreparedStatement inj = c.prepareStatement("select count(*) from jps where v = ?");
            inj.setString(1, "' or '1'='1");
            rs = inj.executeQuery();
            check("no injection through parameters", rs.next() && rs.getInt(1) == 0, rs.getInt(1));

            PreparedStatement batch = c.prepareStatement("insert into jps (i, v) values (?, ?)");
            for (int k = 0; k < 5; k++) {
                batch.setInt(1, k);
                batch.setString(2, "b" + k);
                batch.addBatch();
            }
            int[] counts = batch.executeBatch();
            check("batch", counts.length == 5 && one(c, "select count(*) from jps where v like 'b%'").equals("5"),
                  Arrays.toString(counts));

            PreparedStatement upd = c.prepareStatement("update jps set i = i + ? where v like ?");
            upd.setInt(1, 10);
            upd.setString(2, "b%");
            check("update count", upd.executeUpdate() == 5, "");

            PreparedStatement err = c.prepareStatement("insert into jps (id) values (?)");
            err.setInt(1, 1);
            try {
                err.executeUpdate();
                check("duplicate key error", false, "no error");
            } catch (SQLException e) {
                check("duplicate key error (" + e.getErrorCode() + ")", e.getErrorCode() == 1062, e);
            }
            check("connection usable after error", "1".equals(one(c, "select 1")), "");
            s.execute("drop table jps");
        }
    }

    static void txn() throws SQLException {
        System.out.println("== setAutoCommit(false)");
        try (Connection c = conn(""); Connection other = conn("")) {
            Statement s = c.createStatement();
            s.execute("drop table if exists jtx");
            s.execute("create table jtx (id int primary key)");
            c.setAutoCommit(false);
            s.execute("insert into jtx values (1)");
            check("uncommitted row invisible to others",
                  "0".equals(one(other, "select count(*) from jtx")), "");
            c.rollback();
            check("rollback", "0".equals(one(c, "select count(*) from jtx")), "");
            s.execute("insert into jtx values (2)");
            c.commit();
            check("commit", "1".equals(one(other, "select count(*) from jtx")), "");
            s.execute("insert into jtx values (3)");
            c.setAutoCommit(true);
            check("setAutoCommit(true) commits",
                  "2".equals(one(other, "select count(*) from jtx")), "");
            s.execute("drop table jtx");
        }
    }

    public static void main(String[] args) throws Exception {
        String what = args.length > 0 ? args[0] : "all";
        if (what.equals("auth") || what.equals("all")) auth();
        if (what.equals("stmt") || what.equals("all")) stmt();
        if (what.equals("txn") || what.equals("all")) txn();
        System.out.println(fails == 0 ? "== java: all ok" : "== java: " + fails + " failure(s)");
        System.exit(fails == 0 ? 0 : 1);
    }
}
