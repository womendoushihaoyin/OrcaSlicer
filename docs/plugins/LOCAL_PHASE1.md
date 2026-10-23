# 本地插件迁移开发入口

当前基线为 `release_2.4.1 @ 1bff8dae`，分支 `codex/plugin-local-phase1-release-2.4.1`。当前实施主线已迁入固定上游生产核心，完整一期尚未验收。具体进展见 [TODO.md](TODO.md) 与 [VERIFICATION.md](VERIFICATION.md)。

接手执行时从 [一期本地实施与验收交接](PHASE1_HANDOFF.md) 开始，按构建、安装、依赖、GUI、回归的顺序推进。

## 当前生产核心构建

在原完整应用的工具链和依赖参数上启用以下参数；两个插件选项不能同时开启：

```sh
-DORCA_PYTHON_PLUGINS=ON
-DORCA_LOCAL_PLUGIN_SPIKE=OFF
-DORCA_PLUGIN_PYTHON_ROOT=/absolute/path/to/cpython-3.12.13
-DORCA_PLUGIN_UV_EXECUTABLE=/absolute/path/to/uv
```

`ORCA_PYTHON_PLUGINS` 默认关闭。使用固定 CPython 3.12.13、pybind11 3.0.1；macOS 优先 App 内 `Contents/Frameworks/python`，uv 优先 App 内 `Contents/MacOS/tools/uv/uv`，开发构建才回退显式配置前缀。解释器不导入系统 site-packages。新增绑定使 GUI 静态库可能超过 4 GiB，macOS 使用 Apple libtool 的 64 位 TOC 归档。

```sh
cmake --build /absolute/path/build-app --target Snapmaker_Orca orca_plugin_core_test --parallel 3
python3 scripts/test_plugin_core.py \
  --binary /absolute/path/build-app/src/slic3r/orca_plugin_core_test \
  --output-dir /absolute/path/new-contract-results \
  --wheelhouse /absolute/path/wheelhouse-for-this-abi --jobs 3
```

结果目录必须不存在。每条合同启动独立原生宿主与独立中文 profile，保存日志和 `results.json`；带 wheelhouse 时覆盖真实 NumPy 原生 wheel。`--jobs` 控制独立进程并发；共享 profile/崩溃恢复链仍按顺序运行。最近一次完整生产检查点为 ON37/OFF13 的 **82/82** 核心场景、9/9 生命周期、8/8 app_info、9步依赖中断恢复、12项wheel缓存恢复及 SDK（`host-ci-native-11`，24阶段通过）。覆盖安装/启停/回滚、模型/网格/NumPy、13切片阶段、三维路径/G-code、精确接缝和可编辑纹理。UV 多选和 Pages 旧实例迟到消息修复已通过实际 GUI；自动混色 actual24 的实际GUI/历史/3MF/G-code通过；投影框矩阵/裁切原生通过，标题栏拖动/关闭保留人工验收。旧 FFF/geometry 350例已执行，349 CTest通过、1原release SSWCP合同失败，原预期失败标签保持不变。全套宿主门禁统一入口见 [HOST_CI.md](HOST_CI.md)，含独立生命周期、ID 初始化及 SDK 检查。

## 本地开发和安装

在独立 `--datadir` 下运行开发 App，从 File 的 Local Plugins 开发菜单进入管理窗口，安装 `.py` 或 `.whl`。PEP 723 / wheel 元数据、包键、能力身份和 sidecar 沿用固定上游；本地操作不需要登录，也不创建云账户。配置按钮支持 JSON 编辑器与沙箱 iframe HTML 配置页；保存、恢复默认和再次保存已在真实 macOS GUI 验证。

依赖设置允许联网下载，或指定离线 wheelhouse。依赖由随包 uv 安装到 `<data_dir>/python/packages/cp312`；已有版本作为约束，先在 staging 中解析和安装，成功后发布，失败保留旧目录。当前仅允许二进制 wheel；离线包必须覆盖该平台/CPU/CPython ABI 的全部传递依赖。OS 文件锁在进程异常退出后自动释放已验证；同名本地包替换已接入 journal 和激活后提交；加载失败及进程崩溃恢复已通过原生与桌面验证。具体范围和未完成项见 [LOCAL_RECOVERY.md](LOCAL_RECOVERY.md)。

示例：

- [texture_pack](examples/texture_pack/README.md)：标准 wheel 包含 ResourceTexture 和 PNG，示范资源随包安装及构建。
- [local_gcode.py](examples/local_gcode.py)：在最终 G-code 副本追加注释，见 [后处理合同](GCODE_LOCAL.md)。

- [local_slicing.py](examples/local_slicing.py)：切片阶段读取真实层图；安装、选择及限制见 [本地切片插件开发链](SLICING_LOCAL.md)。
- [native_host_hello.py](examples/native_host_hello.py)：Script 与真实宿主信息。
- [local_page.py](examples/local_page.py)：Pages 主窗口页面、双向消息与配置保存；真实 macOS GUI 已通过。
- [local_ui.py](examples/local_ui.py)：HTML 窗口、停靠面板、进度框和通知；窗口/进度/停靠往返、卸载及排队 UI 退出取消已在 macOS GUI 验证。

测试 App 的准备脚本支持 `--uv-executable`，以及 `--mode pages/ui --fixture /absolute/path/example.py`。脚本将测试 fixture 复制到独立测试目录，不自动启动、不覆盖已有 App；只作为开发 ad-hoc 包，资源仍链接源码目录。正式三端布局、签名、公证和干净机器验收尚未完成。

当前已接入 Script、Pages、SlicingPipeline、ResourceTexture 注册与真实 Host App/Presets/Model/Mesh/Geometry；13个切片/G-code阶段、三维 extrusion、ContourZ、预设包管理及纹理烘焙消费链已有生产原生测试。macOS 已有页面/配置/Host/安装替换、双盘保存与失效、纹理实际GL预览/可编辑3MF/烘焙/重切、UV接缝/图块/顶点边多选与图像调整的实际 GUI 证据。精确接缝9项公开符号及真实 G-code专项已通过。自动混色事务与归档重开已通过；投影框原生标题栏交互保留人工门禁，详见验证记录。

设备插件经源码评估按用户授权暂缓，详见 [设备差异评估](DEVICE_ADAPTER_ASSESSMENT.md)，原联机回归保留。Local-Z与Contour累计物理Z及体积组合已通过生产原生验证，ironing/scarf/adaptive PA组合仍显式拒绝；插件/contour不复用旧2D磁盘缓存。进程内审计提供权限判定与归属记录，并非进程级沙箱；Python死循环不能可靠强杀。真实接口差异见 [HOST_API_AUDIT.md](HOST_API_AUDIT.md)，三端发行门禁见 [DISTRIBUTION.md](DISTRIBUTION.md)。用户已确认暂时没有 Windows/Linux 环境：当前先完成本地及可交接验收脚本，两端原生验收保持待实测。

## 阅读与执行顺序

1. 核对本文开头的 release 提交和开发分支；沿用该基线的 C++ 依赖，不复用 main 的构建缓存。
2. 按 [HOST_CI.md](HOST_CI.md) 准备独立 Python/uv/pybind11 和 ON/OFF 构建。首次构建先跑完整应用和原生测试目标。
3. 使用独立 `--datadir` 启动开发 App，从 Local Plugins 安装 Hello，再依次安装 Pages、切片与纹理样例；需要依赖的包按 [DISTRIBUTION.md](DISTRIBUTION.md) 选联网或离线 wheelhouse。
4. 用下文 macOS 烟测准备脚本验证真实 UI。代码成功编译、窗口打开和业务结果通过分别记录。
5. 执行统一 Host CI driver；失败时从结果目录对应阶段日志定位，不覆盖失败证据。
6. 使用 [DUAL_HOST.md](DUAL_HOST.md) 的同哈希包收集当前宿主结果；上游没有运行时保持 pending。
7. 交接时按 [TODO.md](TODO.md) 和 [VERIFICATION.md](VERIFICATION.md) 区分本机通过项、人工交互项、待环境的发行项。

早期最小宿主的命令与限制移至 [SPIKE_HISTORY.md](SPIKE_HISTORY.md)，只作历史参考。

## macOS 开发 GUI 烟测

准备脚本将可执行文件和独立 Python 前缀复制到测试 App，修正宿主的 libpython 相对链接并对宿主做开发用 ad-hoc 签名，资源仍链接仓库目录。它不构成完整签名、公证或正式发行包。输出目录建议放在源码父目录下，使用中文路径覆盖路径编码；每次整套重跑使用新目录。

```sh
python3 scripts/prepare_local_plugin_gui_smoke.py \
  --binary /absolute/path/build-app/src/Snapmaker_Orca \
  --python-root /absolute/path/to/cpython-3.12.13 \
  --output-dir /absolute/path/gui-smoke \
  --mode install
```

脚本打印 bundle ID 与 `--datadir` 启动参数，同时在 App 邻接位置创建独立 `data_dir`，覆盖 macOS 无参数重开的 portable 路径。测试执行前还会校验实际数据目录，路径不符时拒绝安装和执行。不要使用按 bundle ID 激活的命令反复重开已退出的测试 App。按本机 GUI 自动化工具启动；等待 `gui-install.json`，确认 `ok`、`ui_thread`、`interpreter_stopped` 均为 true，并单独确认进程退出。随后使用同一输出目录分别准备并启动 `restart` 与 `disabled`，前者重新加载并执行后持久化禁用，后者确认一个有效禁用包且没有 Script 被加载。`manager` 模式打开同一管理窗口用于按钮交互验证；它的 `ok` 只表示窗口关闭/报告写出，不能代替每个按钮的 UI 验证。

这些环境开关只存在于显式启用的开发构建。测试脚本拒绝覆盖已有测试 App，防止覆盖正在运行的可执行文件；重跑整套测试时换一个输出目录。

报告包含独立数据目录、App 内 Python 路径、真实进程 PID 和宿主信息。`.startup` 证明初始化完成且没有模态窗口；`.exit` 仅证明走到 `OnExit`，不能单独证明没有晚退出崩溃。关闭测试进程后执行：

```sh
python3 scripts/verify_local_plugin_gui_smoke.py /absolute/path/gui-smoke
```

校验器同时检查进程已消失、不同进程重启、无登录状态和 macOS 崩溃报告。崩溃报告可能延迟写出，最终验收需在后续 UI 测试结束后补查。`manager` 模式的按钮证据单独保留截图与 sidecar 状态。

## HTTP 退出回归

GUI 快速退出暴露了原有后台 HTTP 线程与 OpenSSL 清理之间的竞态。`Http::shutdown_async()` 在停止生产者后取消并 join 已登记请求；屏障关闭后拒绝新异步请求，不调用其回调。只能从应用退出线程调用，不能从 HTTP 回调调用；同步请求的所有者仍负责停止自己的线程。HMS 查询现持有后台线程，并在 `OnExit` 取消、等待；PresetUpdater 的已有取消/join 析构也提前到配置与 preset bundle 销毁之前。两者停止后再关闭异步 HTTP 登记器。

启用开发开关后可构建并运行真实 libcurl/HTTPS 回归：

```sh
cmake --build /absolute/path/build-app --target orca_http_shutdown_test
python3 tests/plugin_runtime_spike/http_shutdown.py \
  /absolute/path/build-app/src/slic3r/orca_http_shutdown_test
```

测试使用临时本机 TLS 服务，覆盖回调内链式请求、六个并行慢响应的取消、重复退出以及退出后的 GET/PUT 拒绝。无需外部网络，运行调度脚本需 Python 和 `openssl` CLI。


## 当前 Host 与配置页开发入口（2026-10-22）

- `docs/plugins/examples/local_host.py`：真实预设/模型读取、OpenGL 信息、中文路径 3MF 副本导出及失败后状态检查。先导入模型，再运行 Script。
- `docs/plugins/examples/local_config.py`：打开管理界面的 Config，按原 Orca HTML 配置协议保存/恢复；无自定义 HTML 时回退到 JSON 编辑器。
- `scripts/prepare_local_plugin_gui_smoke.py --mode host|config --fixture <对应样例>` 创建独立测试应用；`verify_local_plugin_gui_smoke.py <目录> --modes host|config` 检查报告、profile、解释器、退出与崩溃。
- `orca_plugin_lock_test` + `scripts/test_plugin_file_lock.py --binary <二进制>` 验证跨进程互斥和崩溃后恢复。

当前预设绑定已补齐 `preset.bundles`、`Preset.bundle_id/is_from_bundle`，来源/继承/事务/写保护/另存与重启已通过生产原生验证。`app_info`账户映射、模块版本/错误与并发快照八项生产原生检查已通过；`selected_printer` 随设备适配按授权暂缓。样例通过不代替固定上游双宿主全量验收。
