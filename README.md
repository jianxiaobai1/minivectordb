# MiniVectorDB

一个从零实现的单机轻量向量数据库：支持 SQL 子集、分页持久化、事务与崩溃恢复，
以及精确 / 近似两种向量 TopK 检索。

赛题：2026 年 OceanBase 数据库大赛 加赛 · 赛题一「完成一个轻量向量数据库」。

---

## 1. 语言与版本

| 项目 | 值 |
| --- | --- |
| 语言 | C++20（`-std=c++20`，无扩展） |
| 编译器 | GCC 13（Ubuntu 24.04 默认 `build-essential` + `cmake`） |
| 构建 | CMake ≥ 3.22，Release 默认 `-O3 -DNDEBUG` |
| 运行环境 | Linux x86_64 |
| 第三方依赖 | **无**。SQL 词法/语法、存储引擎、WAL、IVF 索引、距离内核全部自研 |

按题意允许的通用库只有 libc/libm/libstdc++/libgcc；后两者在 `CMakeLists.txt` 中被静态链接，
所以运行镜像不需要安装任何包。未使用 SQLite、RocksDB 等现成存储引擎，
也未使用 Faiss、VSAG、HNSWlib 等现成向量索引库。

## 2. 镜像构建与启动入口

```bash
docker build -t minivectordb .
docker run -i --rm -v "$PWD/data:/data" minivectordb
```

* `-i` 保持标准输入打开，`-t` 和 `-d` 都**不使用**：进程以前台方式运行，
  通过 stdin 收 SQL、stdout 回结果，日志只写 stderr。
* 不 `EXPOSE`、不做端口映射、不接受 host/port 参数。
* 构建阶段装好全部依赖，正式运行禁止联网也能正常工作。

本地（不经 Docker）直接运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/minivectordb --data-dir=/tmp/mvd
```

## 3. 数据目录

所有持久化文件都写在 `--data-dir`（默认 `/data`）下，重启与恢复复用同一目录：

| 文件 | 内容 |
| --- | --- |
| `catalog.txt` | 表定义、列类型、主键、是否建了向量索引、oid 计数。DDL 提交时原子替换（临时文件 + `rename` + 目录 `fsync`） |
| `t<oid>.heap` | 第 0 页是表头页，第 1..N 页是 slotted 数据页 |
| `mvd.wal` | redo 日志，整页镜像 |
| `t<oid>.pki` | 主键索引 sidecar，可由堆文件重建 |
| `t<oid>.vidx` | 向量索引 sidecar，可由堆文件重建 |

sidecar 头部记录 `row_count` 与 `epoch`，与表头页一致才被认为是有效的；
不一致就扫描堆文件重建。**重建完成前进程不会输出就绪标识**，
即"索引重建成本计入恢复时间"这条要求是被遵守的。

## 4. SQL 支持范围

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32), v VECTOR(128));
CREATE TABLE t (id BIGINT, s VARCHAR(32), v VECTOR(128), PRIMARY KEY (id));
CREATE VECTOR INDEX vi ON t(v);
DROP TABLE t;

INSERT INTO t VALUES (1, 'a', [0.1, 0.2, ...]);
INSERT INTO t (id, s, v) VALUES (1,'a','[0.1,0.2]'), (2,'b','[0.3,0.4]');
SELECT * FROM t WHERE id > 10 AND s = 'a' ORDER BY id DESC LIMIT 5 OFFSET 2;
SELECT id, L2_DISTANCE(v, [1,0,0]) AS d FROM t ORDER BY d ASC LIMIT 10;
UPDATE t SET s = 'b' WHERE id = 1;
DELETE FROM t WHERE id = 2;

BEGIN; ... COMMIT;      -- 也支持 START TRANSACTION / BEGIN WORK / END / COMMIT WORK
ROLLBACK;
SET vector_mode = ann;  -- 或 exact；默认 exact
```

语义要点（与赛题 2.1–2.3 对齐）：

* 类型：`BIGINT`、`VARCHAR(n)`、`VECTOR(n)`；一个 `BIGINT` 主键，每表最多一个向量列。
  `INT/INTEGER` 按 `BIGINT` 处理，`CHAR/TEXT` 按 `VARCHAR` 处理。
* 比较运算符 `= < <= > >=` 与 `AND`；`ORDER BY`、`LIMIT`、`OFFSET`。
* `VARCHAR(n)` 的长度按 **Unicode 码点数**计算；字符串比较按 **码点顺序**。
* 向量元素必须是可表示为有限 `float32` 的数值，拒绝 `NaN`、`Infinity`、维度不符与转换溢出。
* `L2_DISTANCE` 返回**欧氏距离**（与文档「L2_DISTANCE 返回欧氏距离」及 seekdb/MySQL 一致；
  MySQL 的平方版本另叫 `L2_DISTANCE_SQUARED`）。TopK 内部仍按平方距离排序——
  开方单调不变，所以顺序与并列规则都不受影响，只是输出值取的是真正的距离。
* **带过滤的 TopK 在满足 WHERE 的集合内计算**，不是先取全表 K 条再过滤。
* 结果不足 K 条时返回全部符合条件的记录；距离相同按主键升序；普通 `ORDER BY` 的并列项也按主键升序。
* 精确 / 近似：`SET vector_mode = exact|ann`（默认 exact），
  也可用启动参数 `--vector-mode=`。**即使建了近似索引，exact 模式仍返回精确结果。**
* 主键不可重复；不支持主键更新。
* 未显式 `BEGIN` 时，成功的数据修改语句按自动提交处理。

不支持的语法**返回约定错误而不是静默改语义**：`NULL`、`IS`、`OR`、`NOT`、`IN`、`LIKE`、
`BETWEEN`、`JOIN`、表别名、子查询、`WITH`、聚合、`GROUP BY`、`HAVING`、`DISTINCT`、
`UNION`、`ALTER TABLE`、`TRUNCATE`、外键、`UNIQUE`/`CHECK`/`DEFAULT` 约束、
`SAVEPOINT`（嵌套事务）、`REPLACE`、`INSERT ... ON DUPLICATE KEY`、
`SHOW`/`DESCRIBE`/`EXPLAIN`/`USE`、算术表达式、`NULL` 缺列填充。

## 5. 评测协议

| 方向 | 约定 |
| --- | --- |
| 输入 | stdin，UTF-8。语句以 `;` 结束，可跨多行；支持 `--`、`#`、`/* */` 注释 |
| 输出 | stdout，每条语句响应后立即 flush |
| 结果行 | 制表符分隔，无表头。`BIGINT` 十进制；`VARCHAR` 原样；`VECTOR` 形如 `[1,0.5,0]`（`%.6g`）；距离 `%.6f` |
| 状态行 | `INSERT/UPDATE/DELETE` → `OK <受影响行数>`；DDL / 事务 / `SET` → `OK`；`SELECT` 只有结果行 |
| 错误 | `ERROR <NAME>: <message>`，`--numeric-errors` 改为数字码 |
| 日志 | 全部写 stderr，绝不混入 stdout |
| 就绪 | 默认无额外标记；`--ready-marker=TOKEN` 在每条响应后追加一行 |

题包发布后如果对语句边界、结果集格式、错误码或就绪标识有不同约定，
这些全部集中在 `src/common/config.h` 的 `Config` 与 `src/server/main.cpp` 的 `Output` 里，
改一处即可对齐，不需要动引擎。

## 6. 从空数据目录开始的操作示例

```bash
$ rm -rf /tmp/d && mkdir -p /tmp/d
$ ./build/minivectordb --data-dir=/tmp/d
CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(8), emb VECTOR(4));
OK
INSERT INTO docs VALUES (1,'alpha',[1,0,0,0]), (2,'beta',[0,1,0,0]);
OK 2
SELECT id, tag FROM docs ORDER BY id;            # 普通查询
1	alpha
2	beta
SELECT id, L2_DISTANCE(emb, [1,0,0,0]) AS d FROM docs ORDER BY d LIMIT 1;   # 向量检索
1	0.000000
BEGIN;
OK
INSERT INTO docs VALUES (3,'gamma',[0,0,1,0]);
OK 1
SELECT id FROM docs ORDER BY id;                 # 事务内读到自己的修改
1
2
3
ROLLBACK;                                        # 回滚：记录与向量检索一起恢复
OK
SELECT id FROM docs ORDER BY id;
1
2
COMMIT;                                          # 上面已回滚，这里是空事务
OK
INSERT INTO docs VALUES (3,'gamma',[0,0,1,0]);
COMMIT;                                          # 显式提交，响应发出前已完成落盘
^D                                               # 正常关闭：回滚未提交事务并完成落盘
$ ./build/minivectordb --data-dir=/tmp/d         # 重启，复用同一目录
SELECT id FROM docs ORDER BY id;
1
2
3
```

## 7. 设计

### 7.1 记录布局

固定页大小（默认 4096，`--page-size` 可调；若某张表最宽的一行放不下，
在数据库还没有任何表时会自动放大到 2 的幂）。

```
页:  [PageHeader 12B][slot0 slot1 ...][  free  ][... rec2 rec1 rec0]
     PageHeader{type, slot_count, free_off, used_end, checksum, flags}
     Slot{off, len, flags, pad}                 记录起始地址 8 字节对齐

记录: [ncols u16][pad u16][off0 u32][off1 u32]...[offN u32][payload]
     BIGINT  8B 小端
     VARCHAR 原始 UTF-8 字节，长度由偏移表给出
     VECTOR  dim × 4B 的 float32，直接按偏移表定位
```

偏移表用 4 字节，加上记录起始 8 字节对齐，使向量负载天然 4 字节对齐，
距离内核可以直接对页内字节做 SIMD，不必先反序列化。

表头页（第 0 页）保存 `row_count / next_page / dim / epoch / index_built`，
带 CRC；CRC 覆盖整个结构体，因此所有字节都是具名字段（`HeapHeader::tail` 是显式填充，
否则随机出现的填充字节会让校验失败、把合法表头判成坏页）。

### 7.2 Buffer Pool

全局唯一的 clock 置换池，容量由 `--pool-pages` 或自动从 `memory.max` 推导（默认取限额的 3/5）。
**数据量可以超过内存限额**：扫描按页取，不会把整表读进内存。
活动事务弄脏的页不参与置换——因为 redo 日志只在提交时写，
中途把未提交的页落盘会让 redo-only 的恢复无法撤销它。

`O_DIRECT` 默认开启（文件系统不支持时自动回退，并记录是否真的生效）。
原因是评测用 `memory.peak` 统计**含页缓存**的容器峰值内存，
缓冲 I/O 会把读进来的页算进 M；直连 I/O 让数据页不进页缓存。

### 7.3 向量检索

* **精确**：一次堆扫描，谓词先判定、命中才算距离，维护一个大小为 K 的堆，
  排序键 `(距离, 主键)`。这是 exact 模式的唯一路径，因此"即使建了索引也必须精确"自动成立。
* **近似**：自研 **IVF**（倒排文件）。质心用 farthest-point 播种 + 6 轮 Lloyd，
  训练集上限 4096 条、固定随机种子保证可复现；`nlist ≈ 2·√n`（4..512），
  `nprobe` 默认 8（`--ivf-nprobe`）。查询时先给质心排序，只扫最近的 `nprobe` 个簇。
  增删改通过"每簇 id 数组 + 位置表 + 尾部交换删除"支持，
  建索引后的新增/更新/删除都会正确反映到结果里。
* **带普通列过滤的 TopK 走精确扫描，不走索引**。这不是偷懒，而是成本结构决定的：
  普通列上没有二级索引，判定谓词本身就必须访问每一条记录，
  索引无法省掉这次访问，却会额外引入召回损失与"过滤后不足 K 条"的补扫逻辑。
  谓词先行还能只对命中的记录算距离，比"全表算距离再过滤"更快。
  这条判断写在 `Table::topKExact` 的注释里，并在验证记录里给出实测对比。
* 距离内核用 `target("avx2,fma")` + 运行期 `__builtin_cpu_supports` 分派，
  不支持 AVX2 的机器回退到标量实现。**不用 `-march=native`**：
  评测机 ISA 未知，一条非法指令会让所有用例归零，而分派只损失一点常数。

### 7.4 事务与语句原子性

单进程、单客户端、同一时刻最多一个显式事务，因此不需要并发控制，
但需要三层可见性与两级撤销：

```
基线结构 (pk 索引 + IVF 索引)
  └─ 事务叠加层 tx_overlay_     ← 本事务中已成功语句的累积改动
       └─ 语句叠加层 stmt_overlay_  ← 当前语句的改动
```

* 读路径依次查语句叠加层 → 事务叠加层 → 基线，所以"事务内读到自己的修改"成立，
  包括向量检索（叠加层里的新增向量会被并入候选，删除的键被剔除）。
* 语句失败：丢弃 `stmt_overlay_`，并用 `stmt_undo_` 里的页镜像回滚存储。
  因为索引改动只存在于叠加层，丢弃即等于完全撤销，**不会留下部分修改**。
* `ROLLBACK`：丢弃 `tx_overlay_`，并用 `tx_undo_`（事务内首次触碰时的镜像）回滚存储。
* `COMMIT`：见下。
* 自动提交 = 一条语句自成一个事务。

### 7.5 提交持久化与崩溃恢复

redo-only WAL，记录**整页镜像**：

```
提交时：  把本事务弄脏的页写成 redo 记录 → 写 COMMIT 记录 → fsync(WAL) → 才回 OK
```

* 未提交的事务在日志里**没有任何记录**，所以撕裂的日志尾部只可能是未提交事务的残留，
  恢复时直接忽略即可——这正好实现"发出提交请求但未收到成功响应的事务，
  允许完整生效或完整不生效，不能部分生效"。
* 恢复是两趟扫描：第一趟收集有 COMMIT 记录的 xid，第二趟按 LSN 顺序把对应页
  `pwrite` 回数据文件（整页镜像，重放幂等，所以"恢复途中再次被终止"是安全的）。
  之后 WAL 被清空，`next_lsn` 从扫描结果续起。
* **启动顺序很关键**：先打开文件句柄、再重放 WAL、最后才读表头和加载索引。
  反过来会让"新建空表头"的脏页把重放好的数据覆盖掉。
* `checkpoint` 在 WAL 超过 64 MiB（`--checkpoint-mb`）或正常关闭时触发：
  刷脏页 → `fsync` 数据文件 → 写 checkpoint 记录 → 截断 WAL → 落 sidecar。
* 正常关闭（stdin 关闭或收到 `SIGINT`/`SIGTERM`）：回滚未提交事务、刷盘、写 sidecar。

## 8. 已知限制

1. 一行必须能放进一页。默认 4096 字节可容纳 128 维向量 + 若干标量列；
   更高维度在建表时会自动放大页大小（上限 32768），放大后仍放不下则返回约定错误。
2. 删除留下的空闲槽位只在同一页被再次命中时复用；页内碎片不会自动整理，
   大量"删除 + 插入"交替会让堆文件比数据本身大。没有二级索引的普通列上的谓词必须全表扫描。
3. 单个显式事务的工作集会留在内存里（撤销镜像 + 叠加层），
   因此一个事务里改几百万行不是这套实现的目标场景。题面的基准负载是每 100 行一批提交。
4. 距离渲染精度是 `%.6f`、向量分量是 `%.6g`。文档说"输出允许统一的浮点误差"，
   但具体容差与格式由题包的比较器决定；这两处都在 `src/server/main.cpp` 的 `Output` 里，一处可改。
5. 近似索引只支持 IVF；HNSW 未实现（题面不强制）。`nprobe` 是唯一的召回/耗时旋钮。
6. 崩溃模型只覆盖"进程被强制终止"，与题面一致；不模拟断电、磁盘损坏或文件被外部篡改。

## 9. 验证记录

本地验证在 WSL2 Ubuntu 24.04（GCC 13.3、内核 6.18、cgroup v2、12 核 / 24 GiB）上完成。
正式成绩以平台对锁定版本的评测为准。

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| SQL 子集 / 类型 / 错误 | `tests/` 用例 + 本地驱动 | 见 `tests/README.md` |
| 带过滤 TopK 的正确性 | 构造"前 K 条都不满足过滤"的数据 | 见 `tests/t/vector_filter.test` |
| 事务读己之写 / 回滚 | `bench/crash_test.py` + `tests/t/txn.test` | 通过 |
| 崩溃恢复 | `python3 bench/crash_test.py 6`：mid-ack 与 mid-txn 两类各若干轮 | 6/6 通过：已提交不丢、未提交不残留、无部分生效、恢复后可继续写 |
| 持久化重启 | 同一 `--data-dir` 重启后比对 | 通过 |
| 性能 | `python3 bench/bench.py`（独立 cgroup，按 `io.stat`/`memory.peak` 计量） | 见下表 |
| 自包含性 | `bash scripts/verify-clean.sh`：把分支 clone 到新目录，从那里构建并跑全套 | 10/10 通过，崩溃恢复通过，构建产物不依赖本机路径 |
| Dockerfile 构建步骤 | `bash scripts/verify-dockerfile-steps.sh` | 通过：安装布局与 `ENTRYPOINT` 路径一致；`ldd` 只剩 `libm`/`libc`（libstdc++/libgcc 已静态链入），所以 runtime 阶段不装任何包也能跑；暂存产物运行与同目录重启都正确 |
| **`docker build` 本身** | 见下方说明 | **未验证** |

`docker build` 在本机跑不起来：WSL 到 `registry-1.docker.io:443` 是 `i/o timeout`，
连 `ubuntu:24.04` 基础镜像都拉不下来，所以失败原因是**网络而不是 Dockerfile**。
因此上面那行只能算"构建步骤逐条验证过"，不等于镜像真的构建成功过。
在能访问镜像仓库的机器上先跑一次 `docker build -t minivectordb .` 再提测，
这是当前交付里唯一一处已知的未验证环节。

### 9.1 性能实测

环境：WSL2 Ubuntu 24.04，独立 cgroup v2，`cpu.max=2 核`、`memory.max=2 GiB`、swap 关闭。
负载按赛题 3.3：10,000 条 128 维记录、每批 100 条写入并提交、1,000 条 K=10 的 TopK 查询
（一半带 `cat` 等值过滤）。`E` 以"精确执行器自身"为基线，因此 **E=0.5 表示与基线持平**
（`f(R)=R/(1+R)` 在 R=1 时取 0.5），不是减半。

| 配置 | T_write | B_write | M_write | T_query | B_query | M_query | Recall 无过滤 / 有过滤 | E |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| exact（三轮中位数，提交默认） | 1.011s | 6.1 MiB | 14.8 MiB | 0.357s | 0 | 38.8 MiB | 1.0000 / 1.0000 | 0.500（基线） |
| ann nprobe=8 | 0.983s | 6.2 MiB | 14.8 MiB | 0.184s | 0 | 38.8 MiB | **0.2988** / 1.0000 | 0.568 |
| ann nprobe=32 | — | — | — | 0.233s | — | — | **0.6648** / 1.0000 | 0.540 |
| ann nprobe=64 | — | — | — | 0.344s | — | — | **0.8694** / 1.0000 | 0.502 |
| ann nprobe=96 | — | — | — | 0.417s | — | — | 0.9486 / 1.0000 | 0.481 |
| ann nprobe=128 | — | — | — | 0.401s | — | — | 0.9858 / 1.0000 | 0.487 |
| ann 但未建索引（回退，单轮） | 1.108s | 6.2 MiB | 14.8 MiB | 0.519s | 0 | 14.8 MiB | 1.0000 / 1.0000 | 0.487 |

**这张表得出的结论，和"用了近似索引就更快"是相反的**，所以必须写清楚：

1. 基准数据是 128 维均匀随机向量。均匀分布是 IVF 最不利的情况——真实近邻几乎均匀散布在所有簇里，
   要靠提高 `nprobe` 才能捞回来。实测要到 `nprobe=96`（占 200 个簇的 48%）才勉强够 0.95，
   而那时耗时 0.417s，**已经比精确扫描的 0.357s 更慢**。
2. 赛题规定"无过滤和有过滤两组的平均 Recall@10 均须达到 0.95"，
   任一组不达标性能项直接 0 分。所以低 `nprobe` 换到的加速（nprobe=8 时 0.184s，约 2 倍）
   是用"必然归零的风险"换来的，不值得。
3. 精确扫描这边已经是 1.28 GFMA 在 0.357s 内完成，接近这个数据量的内存带宽下限
   （每轮把 5.1 MB 向量全部过一遍，1000 次共 5.1 GB）。`B_query=0` 说明查询期间没有块设备 I/O，
   数据页常驻缓冲池，O_DIRECT 也让页缓存没有计入 M。
4. 因此**提交配置默认走 exact**：召回恒为 1.0，速度靠 SIMD + 免解码扫描 + 谓词先行拿到，
   不依赖索引。`ann` 仍然完整可用（`--vector-mode=ann` 或 `SET vector_mode = ann`），
   等题包公布真实数据分布后，如果数据是聚簇的（真实 embedding 通常是），
   再按 `scripts/nprobe-sweep.sh` 重扫一次并决定是否切换。

复现命令：

```bash
scripts/build.sh
scripts/bench-full.sh          # exact 基线 + ann 对比 + 无索引回退
scripts/nprobe-sweep.sh        # nprobe / nlist 扫描，用来定提交配置
scripts/run-crash.sh 6         # 崩溃恢复
bash tests/run_all.sh          # 功能准入用例，当前 10/10
```

### 9.2 已发现并修复的缺陷（保留判断过程，便于复核）

| 缺陷 | 怎么发现的 | 影响面 |
| --- | --- | --- |
| `HeapHeader` 尾部未具名填充导致 CRC 随机失败 | ROLLBACK 后已提交数据消失 | 合法表头被判成坏页并重置为空表 |
| `Wal::scan` 复用 `got` 变量，遇到 0 长度 COMMIT 记录误判为撕裂 | `kill -9` 后 `recovered_pages=0` | 已提交数据全丢，加赛直接 0 分 |
| `Wal::resetTo` 允许 `next_lsn=0` | 同上排查中 | 首条记录 LSN=0 会被 `lsn <= base_lsn` 永久跳过 |
| 启动顺序：重放 WAL 之前就读了表头 | 同上，第一次修复无效后才定位到 | 新建的空表头以脏页覆盖重放结果 |
| `--vector-mode=ann` 从未传进引擎 | 测试套件里独立于基线的 `# expect-ann-active` 断言 | 功能测试全绿，但近似索引完全没被使用 |
| 普通 `ORDER BY` 并列项按插入序而非主键序 | 基线 diff | 违反规则 6，结果比对逐行错位 |
| 空表上 `WHERE` 引用不存在的列返回空集 | 测试套件 `# expect-error` | 违反"不支持的语句不能静默改语义" |
| `-9223372036854775808` 被拒 | `types_and_limits` | BIGINT 最小值存不进去 |
| `L2_DISTANCE` 返回平方距离 | 与文档"返回欧氏距离"及 seekdb/MySQL 语义逐条比对 | 排序不变，但逐值比对距离全部不匹配 |
| `topKAnn` 去重用 `return` 而非 `continue` | 代码审查（被上一条 ann 未生效掩盖） | 一个重复的挂起向量会吞掉后续所有挂起项 |
| 无索引时 ann 回退物化全表再排序 | 基准实测 0.836s > 精确 0.357s | "不开索引"反而比默认更慢 |

### 9.3 尚未做、但已定位的下一步

* **exact 扫描的多线程化**：赛题限制的是"单客户端连接、同一时刻最多一个显式事务"，
  没有限制一条只读查询内部的并行。当前 2 核配额下，把一次全表扫描切成两段并行，
  T_query 有望接近减半。前提是 BufferPool 要先加锁（目前是无锁单线程设计），
  属于有明确收益但有正确性风险的改动，没有和本次交付混在一起做。
* 近似索引换成图索引（HNSW）以在均匀分布上同时拿到召回和速度——题面不强制，收益待题包数据确认。
* 页内碎片整理与删除后的空间复用。

## 10. 目录

```
src/            引擎与 SQL 层
  common/       类型、错误、配置、文件工具
  sql/          词法、AST、递归下降解析器
  storage/      slotted page、buffer pool、WAL
  index/        距离内核与 IVF
  engine.h/.cpp Table + Engine：存储、事务、恢复
  exec.cpp      表达式求值、谓词编译、SELECT/INSERT/UPDATE/DELETE/DDL
  server/       stdin/stdout 会话与输出格式
tests/          mysqltest 风格用例与本地驱动
bench/          按平台口径的性能测量与崩溃恢复检查
agent/          conversation.json（学习证据材料）
Dockerfile
```
