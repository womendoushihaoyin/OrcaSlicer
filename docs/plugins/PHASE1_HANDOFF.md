# 一期本地实施与验收交接

## 2026-10-23 六包启动卡死修复（ON39）

已定位并修复选择性 Yield 重入造成的 wx 队列死锁、WebView 同步注册期间提前关闭等待窗，以及动态插件页覆盖未选中首页的问题；页面体验样例更新至 1.0.1。ON39/OFF13、24 阶段门禁、82 核心、9 生命周期、8 app_info、依赖/缓存恢复均通过；新增真实 Runtime 无 App/console 两分支、双 Pages 可见性/代际、六包共存、原生 NSAlert 实际拒绝与正常退出已验证。文件选择器的后台自动化本轮未可靠选中文件，不计作 UI 安装通过；原生安装/替换 API 已实跑。

交付目录：`/Users/leo/CodeProject/Orca_Dev/plugin-development-on39-loadfix`。保留旧 App/用户数据；六个体验插件已预装。具体证据、失败历史与强制 Close(true)/跨端等边界见 [本次事故与修复说明](LOAD_HANG_RECOVERY.md)。下文 ON37 仍作为历史检查点保留，一期外部门禁未因此勾选。


本轮按用户确认先完成本地实现、macOS arm64 验证和 Windows/Linux 可交接脚本。设备插件适配按授权暂缓。正式三端发行、固定上游宿主实跑和真机打印是后续环境门槛；本文件不会把这些项目记为通过。

## 本机直接开始

当前修复交付为 [ON39 开发 App](</Users/leo/CodeProject/Orca_Dev/plugin-development-on39-loadfix/Snapmaker Orca Plugin Development.app>)，六个体验插件已预装。[六插件截图教程](</Users/leo/Documents/ChatGPT/技术/切片器插件系统专项调研/Snapmaker Orca 六个体验插件使用教程.html>) 给出逐步操作与成功标准；教程和 App 均为本机交付物，不包含在源码仓库。整个 App 目录一起保留；启动器强制使用同级 `data_dir`，不会回落到日常用户配置。SDK 可按下文统一 driver 重新导出。旧 ON37/homefix 作为历史检查点保留。

历史 ON37 首次向导已选择 U1 / Generic PLA 并跳过体验改进计划，无账户/设备连接，当时无测试插件预装；当前 ON39 已预装六个体验插件。两次真实GUI启动均使用此隔离目录，UI关闭后进程消失并通过延迟崩溃扫描，包内12858项清单未变、完整 `codesign --verify --deep --strict` 仍通过。组装记录与GUI记录分别在 `development-app.json` 和 `gui-acceptance/summary.json`；不伪造OS启动进程的数值退出码。

这是 macOS arm64、ad-hoc签名的本机开发包。完整Python前缀按标准framework组装，保留兼容别名和工具入口；适配在 `release_2.4.1/development-app-staging-01` 的组装脚本，通用runtime overlay不等于完整可签名App。正式发行hook还需采用同样的合规布局并做Developer ID/公证验证。

### 首页打包修复（2026-10-23）

首个ON37开发包遗漏 `snapmaker_control_macos_arm64`，导致网关无法启动，Home页进入 `missing_connection.html`；原Flutter前端文件存在，不能表述为整个前端丢失。旧包保留为失败现场。新homefix包在隔离组装中复用正常release的归档校验/解包流程，显式锁定官方Connect v1.8.3及同归档前端；原仓库v1.8.0发布下载返回404，默认锁未静默改变，正式发布前需处理此锁定版本。

修复包严格验签、15个Mach-O架构/依赖与Python/uv检查通过；真实GUI首页、近期文件空态、创建项目→Prepare→返回首页以及本地首页HTTP200均通过，GUI后包内清单未变。证据为开发目录 `gui-acceptance/homefix-summary.json` 与 `connect-bundle.json`。本次验证不含登录、推荐内容或真机；当前配置来自原开发目录已落盘数据的独立快照。原包运行中的未保存工程不在快照中。

## 1. 从正确分支开始

- 基线：`release_2.4.1 @ 1bff8daec7dc9daaf3addec9b4c234e8b74f474c`。
- 实施分支：`codex/plugin-local-phase1-release-2.4.1`。
- 接口目标：Orca `14a1aa333b6da3ecadf853a4a6285b0f193b7605`。
- 2026-10-23 用户最终指定提交目标：GitHub `womendoushihaoyin/OrcaSlicer` 的 `feature_plugin_system` 分支。该远端分支创建前不存在，因此以已验证的 release_2.4.1 基线提交本次实现；保留远端 main，远端结果以 Git 记录为准。

## 2. 准备工具链并构建

沿用该 release 的 C++ 依赖，额外准备 CPython **3.12.13** 完整开发前缀、pybind11 **3.0.1** 和 uv **0.11.16**。按 [HOST_CI.md](HOST_CI.md) 的配置片段分别创建 ON/OFF 构建目录；开启 `ORCA_PYTHON_PLUGINS`，关闭旧的 `ORCA_LOCAL_PLUGIN_SPIKE`。

先运行统一 driver，构建应用及测试目标并验证核心、生命周期、账户/模块诊断和 SDK。命令中的目录须替换为当前机器的绝对路径，结果目录必须是新的：

```sh
python3 -X utf8 scripts/test_plugin_host_ci.py \
  --on-build /absolute/host-on \
  --off-build /absolute/host-off \
  --wheelhouse /absolute/cp312-platform-wheels \
  --output /absolute/new-host-evidence \
  --jobs 2 --test-jobs 2
```

Windows 在 VS 2022 x64 developer shell 中用 `python -X utf8`，参数写成一行或使用对应 shell 的续行语法。`--preflight-only` 只检查配置；`--no-build` 只复验已有产物。两者的结果不能当作新构建证据。

## 3. 跑通第一个本地插件

用独立 `--datadir` 启动开发应用，从 File → Local Plugins 进入管理窗口，先安装 [Hello Script](examples/native_host_hello.py)。确认安装、执行、禁用、重启不加载、重新启用和卸载。卸载默认保留插件数据，替换失败回滚规则见 [LOCAL_RECOVERY.md](LOCAL_RECOVERY.md)。

然后依次验证四种能力：

| 能力 | 可安装样例 | 应观察到的结果 |
| --- | --- | --- |
| Script | [native_host_hello.py](examples/native_host_hello.py) | 在真实宿主中执行并读取当前应用信息 |
| Pages | [local_page.py](examples/local_page.py) | 主窗口页面的 JavaScript → Python → JavaScript 往返，重启保持配置 |
| SlicingPipeline | [local_slicing.py](examples/local_slicing.py)、[local_gcode.py](examples/local_gcode.py) | 选择插件后读取真实切片数据，并在最终 G-code 出口执行后处理 |
| ResourceTexture | [texture_pack](examples/texture_pack/README.md) | 资源库中可选，真实预览、烘焙、Undo/Redo、工程重开与切片有实际效果 |

示例的成功不能代替同包兼容矩阵。[DUAL_HOST.md](DUAL_HOST.md) 提供固定五包及七种生命周期形态；两宿主必须使用同一组字节和哈希。真实GUI三态回归步骤见 [LEGACY_STATES.md](LEGACY_STATES.md)，批量NumPy与持续内存采样见 [PERFORMANCE.md](PERFORMANCE.md)。

## 4. 验证在线与离线依赖

无依赖插件不需要登录或插件云。带依赖插件可由随包 uv 下载二进制 wheel；离线时使用匹配 OS、CPU 和 CPython ABI 的 wheelhouse。按 [DISTRIBUTION.md](DISTRIBUTION.md) 生成清单、校验哈希并做空缓存安装；只复制插件 wheel 而漏掉其传递依赖不算完整离线包。

runtime overlay 需要合入完整应用后再签名和打包；overlay 本身不是可交付安装包。macOS 开发烟测使用 ad-hoc 签名，也不代表正式签名、公证和 universal 验收。

## 5. 保留真实 GUI 与旧业务证据

[GUI_TESTING.md](GUI_TESTING.md) 说明隔离 App、启动诊断和结果校验。[VERIFICATION.md](VERIFICATION.md) 保留每轮成功及失败的二进制/进程/产物边界。不要覆盖失败目录，也不要用一次源码修复自动覆盖旧的失败记录。

本机当前完整检查点为 ON37/OFF13、`host-ci-native-11`：82 核心、9 生命周期、8 app_info、24 阶段及 SDK 通过。自动混色actual24完整GUI/归档/G-code通过；ON36 actual02真实Plater三态4切片/3Save/1重开及严格输出比较通过。依赖中断恢复9步和wheel缓存恢复12项已通过最终生产回归；同哈希五包在Snapmaker侧56项矩阵已收齐，上游宿主对比仍待执行。

旧 FFF/geometry 默认 350 例已全部执行：349 CTest 通过、1 个 release 原有的 SSWCP 合同失败，含 3 个原有预期/允许失败标签。该失败未屏蔽，也没有为使测试变绿而修改现有设备语义。真实设备上传/打印仍需打印工程用目标机器验证。

## 6. 按责任田接手

| 团队 | 主要代码与入口 | 后续验收责任 |
| --- | --- | --- |
| 平台工程 | `src/slic3r/plugin/`、`LocalPluginsDialog`、Host/Pages、runtime/SDK/CI 脚本 | 安装、依赖、替换恢复、账户字段、异常、卸载退出、三端分发 |
| 切片算法 | `SlicingPipeline.hpp`、Print/PrintObject、ContourZ/Polyline3、插件选择与失效 | 13 阶段真实语义、缓存/取消、无插件算法差异和明确不支持的组合 |
| 打印工程 | G-code 公共后处理出口、原 Snapmaker 设备链路 | 本地成品、重导出、上传文件一致性与真实打印；设备插件另行恢复 |
| 渲染引擎 | Pages/UI 载体、EditableTextureDialog、GL 预览、投影框 | 多页面消息、关闭/重载、真实图形交互与 GPU 资源生命周期 |
| 几何网格 | 模型/NumPy 绑定、TextureEditSession、UV/涂绘/烘焙/3MF | 所有权、拓扑、撤销重做、归档语义、批量数据及持续内存 |

投影框的裁切/矩阵原生测试已过；2026-10-23 用户确认原生标题栏拖动/缩放/关闭留到交接时手工验收，不阻塞本轮本地开发版交付。渲染引擎牵头、几何网格协助，按 [人工交互验收步骤](GUI_TESTING.md#投影框人工交互验收) 记录移动缩放、关闭重开、Apply、Undo/Redo及归档结果。Windows/Linux 原生结果、macOS universal 正式包、固定 Orca 上游宿主结果及真机打印都保持待验。

本地验收结果随实现一并提交，供团队评审；SDK 的跨端冻结仍以对应门禁为准。二期才接 Snapmaker 自有插件云、账户授权、发布分发和自动版本管理；一期已经包含本地版本元数据、手动替换和失败回退。
