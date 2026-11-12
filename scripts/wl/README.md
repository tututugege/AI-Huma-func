# 非 SPEC 工作负载流水线（rv32imab）

给 GAPBS、PolyBench/C、Ligra、Olden 等负载做 Linux 镜像、BBV、SimPoint 和检查点，流程和 SPEC 流水线（[../spec/README.md](../spec/README.md)）相同，数据目录布局也相同，所以 trace-model 的 `scripts/pf/run_point.sh` 设 `SPEC=$WLDATA` 就能直接跑这些负载。EXP-022（sdip / Breadcrumbs 预取器）的 GAP / PolyBench 数据用的就是它。

**只支持 rv32imab**：二进制、Linux 内核都是 rv32 版，默认数据目录名里也带 `rv32imab`。rv64 的负载流程还没有做。

## 依赖

- `../spec/env.sh`：提供 `FM`（function-model 的 `a.out`，要用 `XLEN=32` 构建）、`SIMPOINT`、`MKIMAGE`、`LLM4TRACE`（rv32 Linux 内核 / OpenSBI / rootfs 所在，用 `linux_rv32/`）。
- 交叉编译好的静态 rv32imab 二进制：在 Lima 虚拟机 `rvbuild` 里用 GCC 13.3 编出，放在 `$WLDATA/bin/`；GAP 的图文件（`g20.sg`、`g20.wsg`、`g18.sg`）和 Ligra 的 `rmatb.*` 放在 `$WLDATA/` 下。
- `bsdtar`（打 cpio）和 SimPoint 3.2。
- `WLDATA`：默认 `~/personal/AI-Huma-data/wl-rv32imab`，里面是 `bin/ img/ bbv/ sp/ ckpt/ status/`。

## 脚本

| 脚本 | 作用 |
|---|---|
| `mk_wl.sh [负载...]` | 为每个负载生成 `$WLDATA/img/<负载>.bin`：把二进制、参数和图文件塞进 rootfs，启动后自动运行并 `poweroff`，控制台打印 `RUN <负载> rc=N secs=S`。负载名和参数在脚本里的 `cmd()` 表里；不带参数 = 6 个 GAP 内核 + `$WLDATA/bin` 里所有 `pb_*` |
| `run_bbv.sh [负载...]` | 用函数模型 `--mode bbv` 跑每个镜像，间隔 1000 万条指令，`P` 个并行（默认 8），输出 `$WLDATA/bbv/<负载>.{bbv,log}` 和 `bbv/done.txt` |
| `pipeline.sh <负载>` | 单个负载：先检查 BBV 日志（有 `SBI shutdown requested`、没有超时 / Oops / panic、有 `RUN <负载> rc=0`），再跑 SimPoint（`MAXK` 默认 30），最后用 `--mode ckpt` 生成检查点（预热 = 1 个间隔）；状态写到 `$WLDATA/status/<负载>.txt` |

## 典型用法

```bash
cd function-model && make XLEN=32            # a.out
bash scripts/wl/mk_wl.sh gap_bfs gap_pr      # 镜像
P=4 bash scripts/wl/run_bbv.sh gap_bfs gap_pr
bash scripts/wl/pipeline.sh gap_bfs          # SimPoint + 检查点
```

## 注意

- 检查点的间隔 1 号点没有预热间隔，会被跳过，`pipeline.sh` 会给出 `WARN`。
- `ol_*`（Olden）负载在 `cmd()` 里有定义，但目前没有成功跑通的记录（`ol_voronoi` 曾因没有 `SBI shutdown` 行失败）。
