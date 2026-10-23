# 本地性能基线的测量边界

L38 尚未完成。该工具提供可重复的 CLI 和真实宿主组件测量，不把原生用例的整段 walltime 当成 GUI 冷启动。固定上游双宿主和 Windows/Linux 真机仍需分别验收。

生产 `src/Snapmaker_Orca.cpp` 的 CLI 分支没有初始化 PluginManager/embedded interpreter。即使把四类包放进 CLI 数据目录，也不能声称测到了插件加载。因此使用两条独立轨道：

| 轨道 | 实际调用 | 能测到 | 不代表 |
|---|---|---|---|
| 正式 ON/OFF CLI | 同一正式可执行文件的 `--help`、同一个3MF的 `--slice 1` | 新进程总耗时、CPU、峰值RSS、真实切片/G-code差异 | 完整 GUI 启动或 Manager 初始化 |
| `orca_plugin_benchmark` | 实际 Manager/Loader/Bridge/registry/Print/mesh bindings | runtime初始化、manifest发现、四包逐个加载、真实Print/G-code、回调批次、网格标量访问 | wxApp/WebView/GL 渲染、完整产品冷启动 |

工具只生成本地记录，不构建、下载、签名、发布或改变用户配置。输入数据只读，所有运行使用隔离 profile。macOS 开发版 CLI 可执行文件按原字节复制进一个私有最小目录布局，Resources 为只读来源的符号链接；这不构成可分发签名包验收。Windows/Linux 应传已有完整运行依赖布局的真实 CLI；当前尚未执行。

## 运行方式

先只生成夹具和待执行计划，不运行宿主：

```sh
python3 scripts/benchmark_plugin_host.py --output /absolute/new-plan --prepare-only
```

准备好 ON/OFF 正式 Release 二进制，以及独立 `orca_plugin_benchmark` target 后，用极小工作负载检查工具连通性：

```sh
python3 scripts/benchmark_plugin_host.py \
  --on-cli /absolute/on/Snapmaker_Orca \
  --off-cli /absolute/off/Snapmaker_Orca \
  --resources /absolute/source/resources \
  --native /absolute/orca_plugin_benchmark \
  --project /absolute/plugin-free-self-contained.3mf \
  --output /absolute/new-validation \
  --validation-only --calls 3 --grid 8
```

`functional_validation` 只验证真实调用/输出，明确不生成性能聚合；存在的原始时长是诊断数据，不能作为基准。可重复指定 `--phase` 限制阶段，例如先只跑 `native-startup` 或 `cli-help`。

等编译、GUI 操作及其他高负载工作结束后才采正式样本：

```sh
python3 scripts/benchmark_plugin_host.py \
  --on-cli /absolute/on/Snapmaker_Orca --off-cli /absolute/off/Snapmaker_Orca \
  --resources /absolute/source/resources --native /absolute/orca_plugin_benchmark \
  --project /absolute/plugin-free-self-contained.3mf --output /absolute/new-baseline \
  --idle-confirmed --warmups 1 --repetitions 20 --calls 1000 --grid 256
```

全部宿主按顺序运行，不启动多进程并行切片。原生 Print::process 限制在 concurrency=1 的 TBB arena 内；CLI 使用输入工程及生产 CLI 的线程策略。每轮交替 ON/OFF/状态的顺序以减少执行顺序偏差；不能据此宣称消除了系统噪声。记录操作系统、架构、CPU数、开始/结束负载、二进制和输入SHA。正式产物更新时必须换 evidence 目录。

## 三种状态与实际阶段

每种状态先在独立 setup 进程准备 profile，安装时间单列且排除出样本。每次样本启动新进程并复制已准备 profile，不复用已加载 Python 对象。

- `none`：无插件，真实 Manager 仍初始化解释器；与整个 feature OFF 是不同概念。
- `disabled`：四个依赖为空的 wheel 已安装、未加载，必须确认 enabled capability 数为0。生产安装默认设置自动加载，因此 setup 会真实加载一次，再通过 Manager 关闭capability并卸载package；这段一次性准备排除出样本，不手写/伪造安装状态。
- `enabled`：四个独立包持久化为启用，正常退出后新进程重新发现及加载。每包一种能力；总4个能力，加载策略明确为逐个等待。

原生 stage 用 `steady_clock` 记录：runtime初始化、manifest发现、逐包加载、Print apply、真实process、G-code导出、Script回调、Pages HTML/双向消息、resource-owned读取与PNG解码、真实网格构造和Python逐元素访问、Manager/解释器关闭。夹具回调不写逐次日志，只在卸载时写计数，避免把磁盘trace开销误算作每次桥接开销。切片夹具是 no-op pipeline，必须确认真实dispatcher确有调用并产出layer/G-code moves。

Pages阶段只测试同一个生产Python消息桥，不含WebView/JS渲染；Texture阶段读取并解码512×512实际PNG，不含GL预览；网格为指定边长grid，逐元素读取所有顶点/三角形并校验，不将此描述成NumPy大数组零拷贝。NumPy bulk及大网格多种规模仍待补测。

## 输出与算法等价性

正式 CLI 必须使用同一个完整、无选中插件的 3MF，记录原文件SHA，并复制为同一 `reference.3mf` 输入。成功退出还必须产出真实G-code移动命令。本分支 GUI 导出的工程版本为2.4.0.1，而CLI声明01.10.01.50；因此两宿主均显式传公开参数 `--allow-newer-file`，记录该策略，保留原始工程字节，不改写版本。

保存原始G-code SHA、规范化SHA和完整diff。主规范化只处理CRLF与生成头里的时间戳；所有坐标、速度、挤出值、工具命令、其他注释/配置仍保留。CLI ON/OFF 的任何剩余不同都标为未解释差异，性能结果不能替代算法审查。

原生启用夹具会产生预期的两项配置注释差异：`plugins`、`slicing_pipeline_plugin`。仅该已验证启用case可将“仅这两项元数据不同”单独分类；原始/完整规范化diff仍保留。任何机器指令或其他配置不同仍失败。不存在一般性的“删掉所有注释/数值容差后认为相同”。CLI和native的等价性分别记录，某一轨道失败不冒充另一轨道发生变化；overall仍要求所有已执行轨道通过。

原生夹具在 `Print.apply()` 后通过公开 `PrintObject::set_id()` 设置顺序稳定ID。当前生产构造器未初始化该字段，仅启用对象排除的导出路径会设置；第一次20轮测量据此发现13份对象标签ID随机变化并保留为失败记录。此问题不通过归一化删除标签解决，修正夹具后必须重新采该轨道。

## 统计与缺口

每组输出 n、min、median、nearest-rank p95、max；默认5次只作探索，并标注p95样本不足，正式建议至少20次。冷文件缓存/OS重启条件不由工具伪造：本工具是新进程、已准备profile、通常暖文件缓存。

POSIX内存由对子进程 `wait4` 的 ru_maxrss 获取，macOS按字节、Linux按KiB换算；这不是各进程内存相加，也不是各stage的独立峰值。Windows缺失这一指标时输出pending而非0。短进程wall等待轮询间隔1ms，native内部阶段使用更细粒度clock。长期内存增长/泄漏趋势尚未测量。

L38 候选 GUI 空插件启动增量≤5%仍待真实GUI测量。CLI无插件切片增量按median单列，≤2%只是待复核候选；输出不等价先解决差异，不能用更快掩盖行为变化。此工具即使全部运行成功，也不完成GUI、冷缓存、Windows/Linux、设备回调或故障恢复耗时门禁。


## NumPy bulk 与持续内存观察的独立补充入口

以下命令用于合入 bulk 补丁后的工作树。新增 `orca_plugin_bulk_benchmark` 是 `EXCLUDE_FROM_ALL` 目标；普通 App 构建不会自动构建它。它与上述三态、新进程测量独立，使用 `scripts/benchmark_plugin_bulk.py` 和真实安装的 Script wheel；不改旧样本、统计或 G-code normalization。

先构建 Release 目标，再执行无需宿主的工具合同：

```sh
cmake --build /absolute/host-on --config Release --target orca_plugin_bulk_benchmark --parallel 1
python3 -X utf8 -B scripts/test_plugin_bulk_tools.py
```

本机实际路径以生成器为准：单配置一般位于 `src/slic3r/`，Visual Studio 等多配置位于相应 `Release/` 子目录，Windows 文件名带 `.exe`。必须使用 `ORCA_PYTHON_PLUGINS=ON`、匹配架构的 CPython 3.12 开发环境和 NumPy 2.2.6 wheelhouse。Windows 使用 `python -X utf8`，将下列参数写为一行或使用对应 shell 的续行语法，不直接复制 POSIX 反斜杠。三个平台使用同一 driver；平台相关原生前提仍见 [HOST_CI.md](HOST_CI.md)。

只生成确定性 wheel 和计划，不运行宿主：

```sh
python3 -X utf8 scripts/benchmark_plugin_bulk.py --mode plan \
  --binary /absolute/orca_plugin_bulk_benchmark \
  --wheelhouse /absolute/cp312-platform-wheels --output /absolute/new-bulk-plan
```

先用小负载验证实际绑定、安装和清理，不能据其耗时宣称性能通过：

```sh
python3 -X utf8 scripts/benchmark_plugin_bulk.py --mode validate \
  --binary /absolute/orca_plugin_bulk_benchmark \
  --wheelhouse /absolute/cp312-platform-wheels --output /absolute/new-bulk-validation \
  --side 32 --warmups 1 --cycles 2
```

功能验证最多允许 side 64、2 次 warmup、3 次 measured cycle，结果标记 `baseline_eligible: false`。其原始耗时及诊断聚合不混入正式样本。每次输出目录必须不存在；driver 建立专用标记 profile，UV 只使用指定离线 wheelhouse，既有用户配置和旧 benchmark 结果保持不变。安装在单独 preparation 进程完成，耗时排除出操作样本。

只有编译、GUI 验收和其他高负载工作结束，并得到明确安静窗口后，才运行正式同进程样本：

```sh
python3 -X utf8 scripts/benchmark_plugin_bulk.py --mode sample \
  --binary /absolute/orca_plugin_bulk_benchmark \
  --wheelhouse /absolute/cp312-platform-wheels --output /absolute/new-bulk-baseline \
  --side 512 --warmups 10 --cycles 100 --idle-confirmed
```

`--idle-confirmed` 是操作者对环境的确认，脚本不会自动判断桌面是否空闲。默认工作负载为 263,169 个顶点、524,288 个三角、262,144 个 polygon 点；网格预先构造一次，NumPy 在整个 session 保持导入。每轮真实 load → Script execute → unload → GC，记录 10 次 warmup 和 100 次后续循环。循环属于同一个进程，彼此相关；它与前文每轮新进程的三态比较不是同一组样本。

### 实际 bulk 操作与数据正确性

- `TriangleMesh.vertices()` / `triangles()`：真实只读 native view，核对 shape、dtype、owner base 和向量化 checksum。
- NumPy-owned 顶点副本：向量化修改副本，并确认原始 native mesh 未变。当前 TriangleMesh API 不提供写回接口，不能把副本修改称为修改宿主网格。
- `face_normals()`：原生计算后返回独立 NumPy 数组，检查全部法向与 dtype。
- `Polygon.set_points()` / `as_array()`：真实 N×2 数组拷入和可写 native view 批量修改；回调返回后由 C++ 独立验证点数、端点和 checksum。

Python 不逐点循环。插件内部用 `perf_counter_ns()` 分别记录上述操作；native 用 `steady_clock` 记录 load、整个 dispatch/execute 和 unload。后者包含实际 capability 调度与正确性检查，不能直接等同于零拷贝 view 创建成本。样本按轮写入 JSONL，不在被测 native 进程累积全部历史记录。

### 当前内存与所有权指标

| 平台 | 字段 | 实际来源与含义 |
| --- | --- | --- |
| macOS | `rss_bytes` | Mach `task_info` 当前 `resident_size` |
| macOS | `phys_footprint_bytes` | Mach `TASK_VM_INFO` 当前 physical footprint |
| Linux | `rss_bytes` | `/proc/self/statm` 当前 resident pages × page size |
| Windows | `rss_bytes` | `GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX)` 的当前 `WorkingSetSize` |
| Windows | `private_commit_bytes` | 同一 API 的 `PrivateUsage`；private commit，不是驻留内存或 macOS footprint |

Windows 目标只在 WIN32 链接 Psapi，使用当前字段，未用 `PeakWorkingSetSize`。其 API 与字段语义见 Microsoft 的 [GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo) 和 [PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)。各平台缺失的不同指标不伪填为 0，也不跨平台直接比较为同一物理量。

每轮在 load 前、load 后、execute 后、unload 后和 GC/原生 polygon 销毁后采样当前内存；它不是 ru_maxrss 或操作期间的瞬时峰值。部分临时数组在 execute 返回时已销毁，最后一项关注清理后的保留量。不会主动清 allocator 缓存或制造内存压力来压低结果。

另外记录 Python `getallocatedblocks()`、GC tracked objects、观察 weakref 的存活数和插件 module 数。每轮要求观察的 module/capability/数组 weakref 全释放、卸载 callback 恰好一次、native capability weak_ptr 过期、manager registry 清空、mesh shared ownership 回到 1。它们验证本夹具观察到的所有权，不覆盖宿主全部全局对象。故意保留一个 mesh view 的独立阴性包必须被检测为失败。

正式 summary 排除 warmup，输出操作时长的 median/p95，以及清理后当前 RSS/footprint/private commit、Python 对象计数的首末值、首末窗口中位数和每循环 OLS 斜率。平稳曲线不能证明无泄漏，增长也可能含 allocator 或 runtime cache 保留；没有任意“斜率小于某值即无泄漏”的门槛。原始逐轮记录和二进制、wheel、NumPy wheel 哈希必须一起保留。

### 当前交付与平台边界

私有 macOS Release harness 已完成真实离线 NumPy 2.2.6、side32 / 1 warmup / 2 cycles 功能验证，以及故意 retained-view 的阴性检测。新增 Windows 分支后，便携工具/三平台字段结构合同共 14 项通过；这是 source/结构验证，不能替代 Windows 编译或 Linux/Windows 原生执行。该私有二进制的 source/archive 范围单独记录，不冒充共享 production target 或最新整机 GUI。

本节初始交付时正式 bulk 采样尚未执行；后续 ON35 授权安静窗口的独立结果见下节。此前 CLI/native 数字和 normalization 保持原记录；本节不完成 GUI 冷启动、冷文件缓存、上游双宿主、设备或无界长时间泄漏验收。


## ON35 正式 bulk 样本：2026-10-23

在主线明确确认构建、GUI 和其他原生任务结束后，使用 ON35 生产 `orca_plugin_bulk_benchmark`、side512、10 warmup + 100 measured cycles，执行一次授权安静窗口测量。独立证据目录为 `release_2.4.1/bulk-production-sample-01`：`results.json`、`run-host.json.samples.jsonl`、`summary.json` 和 `README.md` 保留全部 110 轮数据、来源与限制。二进制 SHA256 为 `a0d760a693f8bdb57117cb8c4bdbb9e5ef53341b17ba67d5957bdfaef0ee911d`，与此前 ON35 小规模功能验证一致；五个源码文件哈希也一致。原有 CLI/native 样本和 normalization 未改。

实际负载为 263,169 个顶点、524,288 个三角、262,144 个 polygon 点，原生 mesh 数据 9,449,484 字节。真实离线 NumPy 2.2.6、包加载、Script 调度及绑定操作完成；110/110 轮正确性与观察 owner 检查通过。运行进程退出 0，整个运行 session 含 warmup/清理等开销约 31.192 秒；安装准备另计。以下操作统计只包含后 100 个循环：

| 操作 | Median (ms) | p95 nearest rank (ms) |
| --- | ---: | ---: |
| 真实 package load | 234.724105 | 255.085625 |
| 完整 dispatch/execute | 11.508333 | 12.571959 |
| 真实 package unload | 0.476646 | 0.627792 |
| 只读 mesh view 创建 | 0.017708 | 0.027459 |
| mesh 向量化读取/checksum | 0.581834 | 0.713959 |
| NumPy-owned mesh copy/写入 | 1.063959 | 1.216667 |
| 原生 normals/owned 数组返回 | 2.868813 | 3.239000 |
| NumPy → native Polygon set_points | 1.180500 | 1.325833 |
| native polygon view 向量化写入 | 0.835854 | 0.907584 |

清理后当前 RSS 从 122,634,240 增至 123,142,144 字节（116.953125 → 117.4375 MiB，增加 **496 KiB**）；physical footprint 从 81,347,328 增至 81,855,232 字节，同增 507,904 字节。二者整个测量区间的 OLS 斜率约为 5,645 字节/循环，最后 75 个测量循环的 RSS 不再变化。这是当前保留量，并非 peak RSS。

GC tracked objects 始终为 18,324；每轮观察到的 8 个 weakref 均释放、插件 module 数为 0、unload callback 恰好一次、native capability weak_ptr 过期、mesh shared ownership 从执行后 2 回到清理后 1。但 Python allocated blocks 从 **76,608 增至 77,347（+739）**，OLS 约为 7.254 blocks/循环。增长尚未归因，不能将平稳的 RSS 或已观察 owner 释放转述为“无泄漏”。可能的测量保留、解释器/allocator 缓存与其他未观察对象需要独立诊断，不能在没有证据时选定其中一种解释。

本次是 macOS arm64 单进程的有序、相关样本，NumPy 保持导入；它不提供 feature OFF 差值、冷缓存、其他平台或无界长期增长结论。后续诊断必须使用新目录并标记 diagnostic，不能改写这一正式样本或把诊断时长混入性能表。

另在 `release_2.4.1/bulk-memory-diagnostic-01` 用同一未修改的 ON35 binary 完成了获授权的 25 轮 side8 tracemalloc 诊断，非性能采样。第 5→25 次卸载后 GC snapshot 净增 31,995 traced bytes / 505 blocks，其中诊断自身 snapshot/file I/O 路径为 2,626 bytes / 30 blocks，mesh view 创建调用为 1,865 / 38，importlib 为 630 / 14；其余 26,874 / 423 缺少 Python 分配栈。25 轮观察 owner 检查通过，module/cache 条目数固定，但这不能证明缓存内容固定，也不能确定其他对象的当前持有者。诊断工具改变了内存环境，且工作负载不同，因此不得从正式 +739 中扣除这些数值。正式样本及 binary SHA 均未变；**+739 仍未归因，没有无泄漏结论或据此进行的产品修复**。具体方法、原始 snapshot 与限制见该目录的 `README.md` 和 `analysis.json`。
