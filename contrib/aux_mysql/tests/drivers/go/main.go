// Copyright (c) 2026, PGSTY PTE. LTD.
// Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
// go-sql-driver/mysql against aux_mysql, default driver settings unless noted.
//
//	go run . [auth|stmt|txn|all]
package main

import (
	"bytes"
	"context"
	"database/sql"
	"fmt"
	"os"
	"time"

	"github.com/go-sql-driver/mysql"
)

var ctx = context.Background()

// MYSQL_PORT selects the server (default 3306)
var addr = "127.0.0.1:" + port()

func port() string {
	if p := os.Getenv("MYSQL_PORT"); p != "" {
		return p
	}
	return "3306"
}

var dsn = "dbuser_mysql:Mysql.123@tcp(" + addr + ")/test"

var fails int

func check(name string, ok bool, detail any) {
	if ok {
		fmt.Println("  ok  ", name)
	} else {
		fmt.Println("  FAIL", name, " ->", detail)
		fails++
	}
}

func open(params string) *sql.DB {
	d := dsn
	if params != "" {
		d += "?" + params
	}
	db, err := sql.Open("mysql", d)
	if err != nil {
		panic(err)
	}
	return db
}

func auth() {
	fmt.Println("== auth")
	for _, p := range []string{"", "tls=skip-verify", "parseTime=true"} {
		db := open(p)
		var n int
		err := db.QueryRow("select 40 + 2").Scan(&n)
		check(fmt.Sprintf("connect %q", p), err == nil && n == 42, err)
		db.Close()
	}
	db, _ := sql.Open("mysql", "dbuser_mysql:wrong@tcp("+addr+")/test")
	err := db.Ping()
	me, ok := err.(*mysql.MySQLError)
	check("wrong password rejected", ok && me.Number == 1045, err)
	db.Close()
}

// database/sql uses COM_STMT_PREPARE/EXECUTE/CLOSE for every query with args
func stmt() {
	fmt.Println("== server-side prepared statements (driver default)")
	db := open("parseTime=true")
	defer db.Close()
	db.SetMaxOpenConns(1)
	mustExec := func(q string, args ...any) sql.Result {
		r, err := db.Exec(q, args...)
		if err != nil {
			check(q, false, err)
		}
		return r
	}
	mustExec("drop table if exists gps")
	mustExec(`create table gps (id bigint not null auto_increment primary key, i int, u bigint unsigned,
		f float, d double, n decimal(12,4), v varchar(64), b varbinary(16), dt datetime(6), dd date,
		tm time, ok tinyint(1))`)
	// bigint unsigned is an int8 domain in openHalo's type mapping, so values
	// above 2^63-1 are out of range (also over COM_QUERY): a known gap.
	ts := time.Date(2024, 2, 29, 13, 45, 1, 123456000, time.UTC)
	r := mustExec("insert into gps (i, u, f, d, n, v, b, dt, dd, tm, ok) values (?,?,?,?,?,?,?,?,?,?,?)",
		-7, uint64(9223372036854775807), float32(1.5), 2.25, "12345.6789", "a'b\"c\\d", []byte{0, 0x27, 0xff},
		ts, "2024-03-01", "12:34:56", true)
	if r != nil {
		id, _ := r.LastInsertId()
		n, _ := r.RowsAffected()
		check("LastInsertId/RowsAffected", id == 1 && n == 1, fmt.Sprint(id, n))
	}
	mustExec("insert into gps (i, v) values (?, ?)", nil, nil)

	var (
		i    sql.NullInt64
		u    uint64
		f    float32
		d    float64
		n, v string
		b    []byte
		dt   time.Time
		dd   time.Time
		tm   string
		ok   bool
	)
	err := db.QueryRow("select i, u, f, d, n, v, b, dt, dd, tm, ok from gps where id = ?", 1).
		Scan(&i, &u, &f, &d, &n, &v, &b, &dt, &dd, &tm, &ok)
	check("scan row", err == nil, err)
	if err == nil {
		check("int", i.Valid && i.Int64 == -7, i)
		check("bigint unsigned", u == 9223372036854775807, u)
		check("float", f == 1.5, f)
		check("double", d == 2.25, d)
		check("decimal", n == "12345.6789", n)
		check("varchar with quotes", v == "a'b\"c\\d", v)
		check("varbinary", bytes.Equal(b, []byte{0, 0x27, 0xff}), b)
		check("datetime(6)", dt.Equal(ts), dt)
		check("date", dd.Format("2006-01-02") == "2024-03-01", dd)
		check("time", tm == "12:34:56", tm)
		check("bool", ok, ok)
	}
	var cnt int
	err = db.QueryRow("select count(*) from gps where v = ?", "' or '1'='1").Scan(&cnt)
	check("no injection through parameters", err == nil && cnt == 0, fmt.Sprint(cnt, err))
	err = db.QueryRow("select count(*) from gps where i is null and v is null").Scan(&cnt)
	check("null parameters", err == nil && cnt == 1, fmt.Sprint(cnt, err))

	ps, err := db.Prepare("select ? + 1, concat(?, 'x'), ?")
	check("prepare", err == nil, err)
	if err == nil {
		var a int
		var s string
		var z sql.NullString
		err = ps.QueryRow(41, "y", nil).Scan(&a, &s, &z)
		check("expression parameters", err == nil && a == 42 && s == "yx" && !z.Valid, fmt.Sprint(a, s, z, err))
		ps.Close()
	}
	_, err = db.Exec("insert into gps (id) values (?)", 1)
	me, isMy := err.(*mysql.MySQLError)
	check("duplicate key error 1062", isMy && me.Number == 1062, err)
	err = db.QueryRow("select 1").Scan(&cnt)
	check("connection usable after error", err == nil && cnt == 1, err)
	mustExec("drop table gps")
}

func txn() {
	fmt.Println("== transactions")
	db := open("")
	defer db.Close()
	other := open("")
	defer other.Close()
	db.Exec("drop table if exists gtx")
	db.Exec("create table gtx (id int primary key)")
	tx, err := db.Begin()
	check("begin", err == nil, err)
	tx.Exec("insert into gtx values (?)", 1)
	tx.Rollback()
	var n int
	db.QueryRow("select count(*) from gtx").Scan(&n)
	check("rollback", n == 0, n)

	// autocommit=0 on a dedicated connection
	conn, _ := db.Conn(ctx)
	conn.ExecContext(ctx, "set autocommit = 0")
	conn.ExecContext(ctx, "insert into gtx values (2)")
	other.QueryRow("select count(*) from gtx").Scan(&n)
	check("autocommit=0: row invisible before commit", n == 0, n)
	conn.ExecContext(ctx, "commit")
	other.QueryRow("select count(*) from gtx").Scan(&n)
	check("autocommit=0: visible after commit", n == 1, n)
	conn.ExecContext(ctx, "set autocommit = 1")
	conn.Close()
	db.Exec("drop table gtx")
}

func main() {
	what := "all"
	if len(os.Args) > 1 {
		what = os.Args[1]
	}
	if what == "auth" || what == "all" {
		auth()
	}
	if what == "stmt" || what == "all" {
		stmt()
	}
	if what == "txn" || what == "all" {
		txn()
	}
	if fails == 0 {
		fmt.Println("== go: all ok")
	} else {
		fmt.Printf("== go: %d failure(s)\n", fails)
		os.Exit(1)
	}
}
