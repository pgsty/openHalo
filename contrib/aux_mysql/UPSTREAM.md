# openHalo 上游同步：2c26131..90fc8d7（36 个提交）

> 2026-11-07。上游 `HaloTech-Co-Ltd/openHalo` master 自我们 fork 的 `2c26131 first commit` 之后新增 36 个提交（到 `90fc8d7`）。
> 上游追平后的 PG14 线：本仓库分支 `pg14`（tag `v1.0-pg14`）= 上游 `90fc8d7` + pgsty 的两个提交。
> aux_mysql 的生成物（语法、aux SQL）从 `pg14` 分支重新生成，C 代码按下表逐项移植（移植时在研究仓库 openhalo-pg18 完成并验证，再并入本树）。

## 重新生成

```bash
# 在本仓库根目录：pg14 分支的源文件导出到临时目录
T=$(mktemp -d)
git show pg14:src/backend/parser/mysql/mys_gram.y > $T/mys_gram.y
mkdir $T/aux_mysql; for f in $(git ls-tree --name-only pg14 contrib/aux_mysql/); do git show pg14:$f > $T/aux_mysql/$(basename $f); done
cd contrib/aux_mysql
python3 -I tools/port_grammar.py $T/mys_gram.y src/parser/mys_gram.y
python3 -I tools/port_aux_sql.py $T/aux_mysql sql/aux_mysql.sql
git show pg14:src/include/parser/mysql/mys_kwlist.h > src/parser/mys_kwlist.h
```

用 `1078669` 的源文件重跑两个脚本，输出与本次同步前提交的 `mys_gram.y` / `aux_mysql.sql` 逐字节相同，所以重新生成带来的差异全部来自上游。
`port_aux_sql.py` 增加了 `aux_mysql--1.4--1.5.sql`；`port_grammar.py` 增加一条替换，消掉上游注释代码后留下的未使用变量。

## 逐个提交

类别：**语法/SQL** = 重新生成；**C** = MySQL 相关 C 代码；**内核** = 只涉及 PG 内核；**文档**。

| # | 提交 | 主题 | 类别 | 处理 |
|---|------|------|------|------|
| 1 | `46f0316` | correct postgresql.conf.sample format | 内核 | 不适用：PG14 配置样例的缩进；OpenHalo 18 的默认配置另写 |
| 2 | `804d66b` | correct environment setup steps | 文档 | README，原样保留 |
| 3 | `c53f1e8` | fix compilation warnings (#4) | 内核 | 不适用：openHalo 改过的 `varchar.c` 只调缩进；PG18 用官方 `varchar.c` |
| 4 | `df498b5` | Update README.md | 文档 | 原样保留 |
| 5 | `11e6313` | fix the issue socket file not deleted when quit | C | 不适用：清理 MySQL 的 Unix socket 文件；aux_mysql 只监听 TCP（Unix socket 是 P1 待办），PG 自己的 socket 由内核清理 |
| 6 | `550037b` | check MySQL listen enabled before on_proc_exit | C | 不适用：同上 |
| 7 | `45a0039` | improve the output of environment variables | SQL + C | 已移植：SQL 重新生成（`@@datadir`、`@@character_sets_dir`）；`getENV` 移到 `src/mysm/systemVar.c`。移植差异：`PGDATA` 未导出时（Patroni、直接 `postgres -D`）回退到 `DataDir`，否则 `CREATE EXTENSION` 会失败；`mysql.getenv()` 收回 PUBLIC 执行权限（`aux_mysql_additions.sql`），它只在安装时使用 |
| 8 | `05ed76e` | format code and improve errno output (#13) | C | 已移植（并入 `getENV`） |
| 9 | `f2a4ed4` | Fix typo and garbled | 语法 + C | 已移植：语法重新生成；`mys_parser.c`、`mys_adtext.c`、`mys_adtext.h` 注释和 `strfuncs.c` 乱码注释同步；`mys_analyze.c`、`mys_parsenodes.h` 的对应内容在 aux_mysql 中不存在 |
| 10 | `f66d311` | Tweak code logic (#22) | C | 已移植（`getENV` 返回值） |
| 11 | `f3e3e15` | Added handling for CMDTAG_ALTER_SYSTEM (#23) | C | 已覆盖：aux_mysql 的 `mys_end_command()` 对所有非结果集命令统一回 OK 包，不需要按命令标签列举；`p5_upstream.sql` 验证 |
| 12 | `c2dbc91` | Minor cleanups (#24) | 内核 | 不适用：删除 PG14 configure 的 SM3 选项；PG18 configure 原样 |
| 13 | `aa679e6` | Adjust the display of data types (#25) | SQL | 已移植（重新生成）：`year`、`binary`、`varbinary`、`bit` 的显示；`p5` 验证 |
| 14 | `e4f58ea` | Some supplementary revisions to #25 (#26) | SQL | 已移植（重新生成） |
| 15 | `3debcfa` | Some environments might require ICU (#27) | 文档 | 原样保留 |
| 16 | `bf7f1c0` | README: remote login with mysql CLI (#28) | 文档 | 原样保留 |
| 17 | `cc0a7d1` | Add CONTRIBUTING.md (#29) | 文档 | 原样保留 |
| 18 | `b223ca4` | The version information indicates adjustments (#39) | C | 已移植：握手版本号本来就取 `mysql.halo_mysql_version`；补上按版本选默认字符集（低于 8.0 报 45 `utf8mb4_general_ci`，否则 255），PyMySQL 实测 |
| 19 | `1b250eb` | CMDTAG_CREATE_ROLE / CMDTAG_DROP_ROLE (#38) | C | 已覆盖（同 11） |
| 20 | `0d621b7` | CREATE USER ... IDENTIFIED BY (#40) | 语法 | 已移植：语法重新生成，`mys_kwlist.h` 同步 `IDENTIFIED`；新建用户可以用 MySQL 协议登录 |
| 21 | `d86ca29` | mysql.uuid() 用 gen_random_uuid()，去掉 uuid-ossp，aux_mysql 1.5 | SQL | 已移植：`port_aux_sql.py` 纳入 `1.4--1.5`，`aux_mysql.control` 去掉 `requires = 'uuid-ossp'` |
| 22 | `936b453` | Adjust comment content (#46) | C | 已移植：`sysvar_core.c`、`uuid_short_core.c`、`systemVar.h`、`uuidShort.h` 的文件头注释 |
| 23 | `58cbe8c` | Merge branch 'master' | — | 合并提交，内容同 22 |
| 24 | `264834a` | SHOW ENGINE INNODB STATUS | 语法 | 已移植（重新生成）；`p5` 验证 |
| 25 | `f3518a1` | Improve show_create_table_impl / show_table_full_columns | SQL | 已移植（重新生成）：Collation 列、`SHOW CREATE TABLE` 的 `COLLATE` 显示 PG 排序规则名（`case_insensitive` 等，与上游一致）；aux_mysql 侧让这些名字可以回放 |
| 26 | `3e43ac3` | fix #47: SHOW CHARACTER SET WHERE (#48) | 语法 | 已移植（重新生成）；`p5` 验证 |
| 27 | `cdbd589` | Support ALTER USER (#50) | C | 已覆盖（同 11） |
| 28 | `6d871d5` | Merge PG-14.18 (#52) | 内核 + C | 内核部分不适用（PG 18.6 已含这些修复）。MySQL 部分：① 删除 `mysql.ci_collation` GUC，大小写只由排序规则决定——aux_mysql 本来如此，同步删掉兼容层里恒为 false 的变量和 `bpchar.c`/`strfuncs.c` 中的分支；② 列排序规则映射从语法移到 `mys_parse_utilcmd.c`（表级 `COLLATE=` 参与判断，MODIFY 不带 COLLATE 回到 CI）——移植为 `mys_rewrite.c` 的列排序规则转换，否则重新生成的语法会让 `COLLATE utf8mb4_bin` 直接报错；同时修掉表级 `COLLATE=` 被当成 reloption 传给 PG 的问题；③ `mys_tablecmds.c` / `mys_execMain.c` 是 PG back-patch 同步进分叉代码，aux_mysql 不分叉这些文件，不适用 |
| 29 | `6602ba8` | Merge PG-14.18 (#53) | — | 合并提交，无独立改动 |
| 30 | `154ce7a` | Fix bugs | SQL + C | ① 原“不适用”说法仅覆盖不断连，不代表功能等价；修复版补 SHOW WARNINGS/ERRORS 诊断区；② `mysql.setval()`：移植为 `mysSetval3Oid`，复用 aux_mysql 现有的单调推进实现（原高水位 + 排他锁实现存在倒退/死锁；现调用核心 SetSequenceValueAtLeast，在原序列页锁/WAL/权限路径中推进），不照搬直接改序列页的写法；③ 空表 `ALTER TABLE ... AUTO_INCREMENT=n` 直接重置计数器：已移植；④ `CREATE TABLE ... AUTO_INCREMENT=` 超过 int4：aux_mysql 已支持，`p5` 验证；⑤ MODIFY 自增列：aux_mysql 原来会报 “is an identity column”，已修复（保留 / 去掉 / 新增 AUTO_INCREMENT 三种情况，新增时从现有最大值之后继续）；⑥ 改排序规则时强制重写表：不适用，PG18 的 ALTER TYPE 自己处理 |
| 31 | `c742044` | Supports compilation on macOS (#60) | C | 已移植：`uuid_short_core.c` 的 macOS 分支；语法里的 `ulong` 重新生成；`bpchar.c` 的 include 调整和 Makefile 的 ddsm 位置不适用（aux_mysql 有自己的 `pgsrc/` 拷贝和 PGXS Makefile） |
| 32 | `e271848` | Update configure (#61) | 内核 | 不适用：PG14 configure（默认端口 1921→5432、删 SM3/C2/HSM 选项、版本串）；PG18 configure 原样 |
| 33 | `ecd1942` | improve detail of transaction (#62) | C | 已覆盖：openHalo 在 COMMIT/ROLLBACK 后没清 `needStartNewTrx`；aux_mysql 的 autocommit 实现（`mys_txn.c`）没有这个状态位，`p3_txn` / `p5` 验证 COMMIT、ROLLBACK 后下一条语句开启新事务 |
| 34 | `b910654` | Add Discord link to README | 文档 | 原样保留 |
| 35 | `eb69c22` | Add acknowledgment for IMT Atlantique student project | 文档 | 原样保留 |
| 36 | `90fc8d7` | Update COPYRIGHT | 文档 | 原样保留 |

## 原移植时记录（不作为发布验收）

- 原记录使用 PG18.6 + 协议补丁；独立审查发现新增与继承的编译警告，不能声称无新增警告。
- 原 `run.sh` 只统计 m 脚本错误、缺驱动会静默跳过，原“全绿”不足以证明全部通过。修复版按错误预期和分组退出码判定，并报告跳过。
- 手工核对：`CREATE USER ... IDENTIFIED BY` 建的用户能用 MySQL 协议登录；握手字符集 `8.0.30`→255、`5.7.32-log`→45；非超级用户执行 `mysql.getenv()` 被拒绝。

## 已知差异（未在本次处理）

- `SHOW WARNINGS` / `SHOW ERRORS` 已补基本诊断区，最多保留64条；不声称完整MySQL诊断系统。
- `SHOW CREATE TABLE` 的 `COLLATE` 显示 PG 排序规则名，直接回放到真 MySQL 不通用。

## 独立审查后的修复范围

上表记录移植来源，不能证明每项边界正确。独立审查确认自增、LIKE、UNSIGNED、
SHOW CREATE、表默认排序规则、协议时间/二进制、连接重置等回归。修复版新增
`tests/ab_regress.py` 并修正 p5 对错误行为的期望；内核除原12文件协议补丁外，
增加 commands/sequence.c、commands/sequence.h 的原子推进接口及 pg_upgrade 的
旧OpenHalo版本拒绝提示。未改变PG节点/catalog布局，ClientSocket安装头布局变化仍在。
当前支持范围及有意未恢复的PG14类型/升级能力见 [MIGRATION.md](MIGRATION.md)。
发布通过数必须使用对应源码状态下的实际测试日志，不再沿用本历史记录的“全绿”。
