# 本地插件纵向链验证记录

## 2026-10-23 提交前完整构建与检查

用户最终指定将本地实现提交到 GitHub `womendoushihaoyin/OrcaSlicer` 的 `feature_plugin_system`，保留 `release_2.4.1 @ 1bff8daec7dc9daaf3addec9b4c234e8b74f474c` 基线。提交前重新执行统一 driver，模式为 `build-and-test`，ON/OFF 应用构建和全部 24 阶段通过，包含 82 核心场景、9 生命周期、8 app_info、9 步依赖中断恢复、12 项 wheel 缓存恢复及 SDK。结果为本机 macOS arm64，不代表 Windows/Linux、固定上游宿主、真机或正式签名包已验收。

证据：`release_2.4.1/submit-feature-plugin-system-01/host-ci/results.json`；日志 `submit-feature-plugin-system-01/host-ci.log`。另重新链接并执行 `orca_plugin_runtime_wait_test` 的 `no-app` 和真实 `wxAppConsole` 两分支，均通过并结束解释器；记录位于同目录 `runtime-no-app-中文/results.json`、`runtime-console-中文/results.json`。提交整理只规范 15 个新增文件的 EOF 空行，并纠正当前交付入口和历史状态；不把 App、Python runtime、wheelhouse、账户配置和本机验证输出放入 Git。

## 2026-10-23 六包启动卡死修复（ON39）

已定位并修复选择性 Yield 重入造成的 wx 队列死锁、WebView 同步注册期间提前关闭等待窗，以及动态插件页覆盖未选中首页的问题；页面体验样例更新至 1.0.1。ON39/OFF13、24 阶段门禁、82 核心、9 生命周期、8 app_info、依赖/缓存恢复均通过；新增真实 Runtime 无 App/console 两分支、双 Pages 可见性/代际、六包共存、原生 NSAlert 实际拒绝与正常退出已验证。文件选择器的后台自动化本轮未可靠选中文件，不计作 UI 安装通过；原生安装/替换 API 已实跑。

交付目录：`/Users/leo/CodeProject/Orca_Dev/plugin-development-on39-loadfix`。保留旧 App/用户数据；六个体验插件已预装。具体证据、失败历史与强制 Close(true)/跨端等边界见 [本次事故与修复说明](LOAD_HANG_RECOVERY.md)。下文 ON37 仍作为历史检查点保留，一期外部门禁未因此勾选。


日期：2026-10-22。基线 `release_2.4.1 @ 1bff8daec7dc9daaf3addec9b4c234e8b74f474c`；分支 `codex/plugin-local-phase1-release-2.4.1`。此日期描述的是当时尚未提交的历史检查点；2026-10-23 用户已指定将本地实现整理提交到GitHub `womendoushihaoyin/OrcaSlicer:feature_plugin_system`，远端结果以 Git 记录为准。

## 历史 spike 通过的范围

| 检查 | 实际结果 | 证据 |
| --- | --- | --- |
| 开发应用构建 | macOS arm64、Release、插件开关 ON，完整编译/链接通过 | `release_2.4.1/final-build.log` |
| 原生合同测试 | 10/10；真实嵌入 CPython，含四进程重启 | `release_2.4.1/store-tests-final.log` |
| HTTP 退出回归 | 真实本机 TLS、两个链式请求、六个慢响应取消、取消回调重入、晚 GET/PUT 拒绝、重复 shutdown | `release_2.4.1/http-shutdown-final-tests.log` |
| GUI 首次安装 | 正常设置向导关闭后安装示例、执行 Script、获得真实宿主版本 | smoke-07 `gui-install.json`，PID 28821 |
| GUI 重启加载 | 从持久化安装记录加载并执行，随后禁用 | smoke-07 `gui-restart.json`，PID 28960 |
| GUI 禁用重启 | 一个有效禁用包，零个加载的 Script | smoke-07 `gui-disabled.json`，PID 29020 |
| 管理界面 | 实际按钮启用、运行、禁用、卸载并保留数据、文件选择器安装和信任确认 | smoke-06 `manager-ui-actions.json`、截图、sidecar 快照 |
| 最终管理进程 | 最终二进制再次启用、运行并关闭 | smoke-07 `manager-ui-actions.json`，PID 29089 |
| 退出与隔离 | 四个最终测试进程均消失；解释器已结束；启动/退出探针齐全；路径与未登录状态正确；延迟崩溃报告复核通过 | smoke-07 `verification.json` |
| 上游接口来源 | 五个复制的接口文件与固定上游逐字节一致 | `release_2.4.1/public-interface-check.json` |

所有最终 GUI 运行均使用中文输出路径、App 内 `Contents/Frameworks/python` 和独立 portable `data_dir`。CPython 3.12.13，pybind11 3.0.1。smoke-06 的管理界面源码与最终版哈希一致；该组旧退出测试有 HMS 崩溃，**不计入整体退出通过**。

证据目录：

- 构建及原生测试：`/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/`
- 最终 GUI：`/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/plugin-phase1-smoke-07-中文/`
- 全按钮操作：`/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/plugin-phase1-smoke-06-中文/`
- 源码/二进制哈希：构建证据目录的 `verified-build.json`。

## 验证中发现并修复的问题

1. **过早加载/关闭**：原 `m_post_initialized` 在 `post_init()` 进入前置 true。插件启动安排改到该函数返回后，再等启动模态窗口关闭，防止烟测关闭主窗口后初始化仍调用 `Freeze()`。
2. **异步 HTTP 晚退出**：原 `Http::perform()` 使用脱离线程，可能晚于 OpenSSL 全局清理。请求由登记器持有，退出先停止生产者，再取消并 join；取消标志为原子变量。取消和 join 时不持有登记锁，覆盖回调重入。
3. **HMS 同步查询晚退出**：局部 `boost::thread` 脱离后访问已销毁 `wxApp`。HMS 现持有查询线程，退出先取消并 join；析构再次兜底。PresetUpdater 的既有取消/join 析构也提前到配置和 preset bundle 销毁之前。
4. **测试隔离**：macOS 按应用标识重新激活可能无参数重开。测试 App 邻接独立 portable 目录，运行前校验实际目录。旧 smoke-03 曾误用默认 profile；仅移走字节匹配的测试夹具并保存配置快照，没有删除或回滚用户原有配置，不能宣称原配置完全未变。

失败样本保留用于溯源。`ok` 报告或 `OnExit` 返回值都不能单独证明进程没有崩溃；校验器结合进程存在性与 macOS 延迟诊断报告检查。此处是有限次数回归结果，不代替压力测试、ASan/TSan 或全平台退出验收。

## 历史 spike 当时尚未验收的范围

- 一期全量能力：Pages、PrinterAgent/Connection、SlicingPipeline、ResourceTexture、完整 Host/Model/Mesh 绑定与必要内核补迁。
- wheel、uv、在线依赖下载、离线 wheelhouse、正式权限审计、配置 UI、替换/回滚、中断锁恢复。
- Windows/Linux/macOS x64，以及无系统 Python 的干净环境、正式签名/公证/发行包。
- 插件开关 OFF 的原版对照构建、同一插件跨 Orca/Snapmaker 宿主的完整合同测试。
- File 菜单点击入口尚未单独点验；已编译接入，管理窗口通过专用烟测入口打开。

上述旧 spike 默认开发开关关闭，仅接受可信、无依赖 `.py`；当前生产核心范围见后续记录。完整任务和团队分工仍以 [TODO.md](TODO.md) 的逐条验收为准。


## 生产核心追加验证：2026-10-22

- `ORCA_PYTHON_PLUGINS=ON`，旧 spike OFF。上游生产核心已链接进真实应用；日志 `core-ui-build-3.log` 通过。macOS 超过 4 GiB 的 GUI 静态库使用 Apple libtool `-toc64`，原 `ar/ranlib` 失败日志保留。
- `core-tests-06/results.json`：12/12；每例独立进程/profile。覆盖 metadata/config/wheel/multicap/audit/failure/paths/dependencies/geometry/lease/lease_out_of_order/numpy。真实离线 NumPy 2.2.6 wheel 校验在 `wheelhouse-macos-arm64-cp312/manifest.json`。
- `lease-out-of-order-before.log` 复现嵌套租约提前释放导致解释器过早结束；共享锁所有权修复后合同通过。lease 是同线程 RAII，不能跨线程转移 shared_mutex 锁。
- `core-tests-03/04` NumPy 失败是测试误用了居中前顶点和；已改为复制快照、释放模型后验证数组内容完全一致。数组 dtype/shape/只读/拷贝独立性均通过。
- 新核心真实 GUI install：`plugin-core-smoke-01-中文`，PID 44524，App 内 Python、Script、解释器关闭及 OnExit 通过；进程退出、延迟崩溃查询通过。
- Pages：`plugin-core-pages-01-中文`，PID 50388。真实 Notebook 页面按钮→Python→JS→配置回传通过；unload 后 tab 消失、解释器关闭、进程退出和延迟崩溃查询通过。截图 page.png / roundtrip.png；报告 gui-pages.json / exit-verification.json。
- 通用 UI：`plugin-core-ui-01-中文` 包含旧二进制，未启动、不算证据；`ui-02` 暴露窗口初始文档空白，未通过；`ui-03` 是事件诊断轮次，不能提前算通过。
- 尚未验收完整一期；三端、全部 Host、切片/设备/纹理实际业务链待完成。


### 2026-10-22：上游核心、Pages 与 Host UI 实测

- `core-tests-08`：13/13 原生合同通过，包括 metadata/config/wheel/multicap/audit/failure/paths/dependencies/geometry/lease/lease_out_of_order/stderr/numpy。
- `plugin-core-pages-02-中文`：PID 69958；共用 WebPanel 的页面真实按钮→Python→页面回传通过，卸载移除页面，解释器结束、OnExit=0、PID 消失且无对应崩溃记录。
- `plugin-core-ui-07-中文`：PID 70618；HTML 窗口、进度框、Prepare 中的 AUI 停靠面板消息往返通过；卸载后注册表无窗口；已排队 UI 请求在关闭时取消，工作线程退出；OnExit=0、PID 消失且无对应崩溃记录。
- 修复过的实际失败：mac WebView 初始 about:blank 导致晚到 bootstrap 覆盖内容；UI Python 回调缺审计身份；测试页索引错误导致停靠等待 Home；嵌套 runtime lease 提前释放；含引号 profile 的 stderr 路径拼接。
- 上述证据只覆盖 macOS arm64 开发包。停靠布局持久化、多页/重启和其余两端仍待验证。新 HostApp/Presets 与配置页面的构建、测试另记，不复用旧 UI 证据。


### 2026-10-22：Host、配置与安装锁收口

- 最新完整应用及测试构建通过：`release_2.4.1/core-host-guard-build.log`。`core-tests-10/results.json` **14/14**，新增预设合同；不将旧 spike 测试数相加。
- `plugin-core-host-01-中文`，PID 79244：真实 10×20×30 模型、当前预设及 GL 信息读取，3MF 副本 ZIP/模型内容有效，副本导出前后工程路径和脏状态保持；错误扩展名与缺失父目录拒绝。该 GUI 使用空耗材保护修复前的版本，正常预设路径一致；空耗材保护由最新原生测试证明。
- 空 FFF 耗材选择曾在 `PresetBundle::full_fff_config()` 访问 `filament_presets[0]` 崩溃（core-tests-09/presets.log）；binding 现在先抛 RuntimeError，再用合法耗材选择验证 full_config。未修改旧内核算法。
- Host 烟测退出时测试立方体仍是未保存工程，真实保存对话框出现；保存到独立 data_dir 后退出。该额外保存发生在副本导出状态检查之后。
- `plugin-core-config-01-中文`，PID 79439：真实沙箱 HTML 配置页，1→保存42→确认恢复默认1→再保存42→关闭，持久化检查通过；保留 saved.png/restored.png。
- `plugin-core-smoke-01-中文`：安装 PID44524、重启 PID82128、禁用后重启 PID82381 的统一 verifier 通过。重启/禁用使用最新二进制，首次安装使用较早核心版本，不能称三个进程来自同一次构建。
- 上述 GUI 的 verification.json 均为通过；解释器结束、OnExit=0、PID 消失和延迟崩溃查询均已校验。其他 UI/Pages 证据沿用上一节，尚无全平台或压力测试证明。
- `WebViewHostDialog::call_web_handler` 的排队投递改为 wxWeakRef，避免闭窗后直接捕获 this；正常 GUI 退出通过，尚无 ASan 结论。
- `orca_plugin_lock_test` + `scripts/test_plugin_file_lock.py` 在 macOS arm64 通过真实双进程排他、强制终止测试子进程后重获锁、持久锁文件以及符号链接拒绝。使用 OS 锁替代目录存在即锁定；Windows/Linux 实现尚未运行验证。
- 证据索引：`release_2.4.1/verified-core-build.json`；未承诺覆盖安装失败自动回滚、safe mode、完整 Host bundle API、切片、纹理或正式三端发行。
- 设备范围按用户授权调整，见 [DEVICE_ADAPTER_ASSESSMENT.md](DEVICE_ADAPTER_ASSESSMENT.md)。本次没有真实设备测试，设备插件暂缓不豁免公共 Http/退出改动的设备回归。


## 2026-10-22 本地替换与切片图推进

- 新核心合同 `release_2.4.1/core-tests-13/results.json` 为 **20/20**。新增包替换（导入、on_load、依赖失败均恢复旧包/配置）、活跃引用拒绝、恢复期间配置写入失败、禁用状态保留、待激活删除、真实双宿主 profile 锁与真实进程中断恢复。`replacement_crash` 的实际退出码是 **82**，预期也是 82；它不是正常退出，而是后续 `replacement_recover` 的故障注入前置。
- 原生 `slicing_graph` 创建真实立方体并 slice，将 live Print/Layer/Surface 交给 Python 修改，再由原生 `Print::process()` 生成围墙，检查修改后几何范围和 bbox 缓存；同时验证原生挤出树遍历。它不是完整 SlicingPipeline 插件调度验收。
- `PluginHostSlicing.cpp` 暂缺 `ExtrusionPath.points()`：固定上游返回 N×3 原生坐标，release 基线是 2D Points。首次编译失败保留于 `core-slicing-data-build.log`，没有用零 Z 或假数据凑接口。测试 bootstrap 的 Print 类型注册问题、const/private 调用问题分别修复，失败日志保留。
- 同名包事务文档见 [LOCAL_RECOVERY.md](LOCAL_RECOVERY.md)。当前方案处理进程崩溃，不声称断电耐久性或撤销外部副作用。完整消费者卸载协议和三端错误组合仍未验收。

### 真实 macOS GUI 的替换流程

- `plugin-core-replacement-01-中文`：旧版已恢复，但 `load_installed` 用 `has_error()` 拒绝历史加载失败的描述符，导致无法重新运行。已改为拒绝真正无效的包/sidecar，允许有效旧版重新激活；失败证据保留。
- `plugin-core-replacement-02-中文`：应用采样显示主线程阻塞在读取 Documents 中的测试输入（open），尚未执行插件。测试专用 PID 557 以 SIGTERM 清理，不能算通过。脚本改为启动前复制精确 fixture 到独立测试目录，避免为测试样例请求开发者 Documents 目录访问。
- `plugin-core-replacement-03-中文`：PID **3580**。通过真实桌面 App 的 LocalPluginStore/Runtime 路径，执行旧版 count=1 → 故障新版激活失败 → 旧版/配置恢复并重新运行 → 正常新版执行 count=2。报告包含 `replacement_rollback=true`、`replacement_commit=true`、App 内 CPython、未登录、解释器结束。`.exit` 为 0，进程消失，匹配 PID 的延迟崩溃报告检查通过。
- 目录：`/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/plugin-core-replacement-03-中文`；`verification.json`、`gui-replacement.json`、`.startup`、`.exit` 可核查。测试通过程序内调用真实 GUI 路径自动运行，**不等同于逐个点击插件管理按钮**。
- 该 GUI 包对应 `core-slicing-data-adapter-build.log` 的开发应用；后续切片阶段/恢复边界改动需以各自新构建和合同证据判断，不能将此 GUI 结果标成所有后续 binary 都实测通过。


### 切片阶段迁移中的实测差异

`core-tests-14` 的 6 条新增阶段测试最初失败，原因是预期了 11 次回调。源码复核确认：固定上游与 release 的 `PrintObject::estimate_curled_extrusions()` 都注释了 `set_done(posEstimateCurledExtrusions)`；上游 `hook_after` 只对 DONE 阶段派发，所以该回调实际不可达。当前按这一真实行为保留单独断言和未完成项，未强行增加假完成状态。`posContouring` 和 `psGCodePostProcess` 同样没有占位调用。

内核阶段接缝位于 `SlicingPipeline.hpp` / `Print.cpp`。当前是可执行的 C++ 内核基础：生产 Python SlicingPipeline capability、预设选择和 Manager dispatcher **尚未接入**。阶段合同里的 Python 修改通过真实绑定进入 C++ 图，并不是用户已能安装/选择 SlicingPipeline 插件的证明。

阶段异常/取消时，已完成的 Print 和 PrintObject 状态在 state_mutex 内一起失效；避免重试或导出复用部分修改的数据。由工作线程自行失效时不调用 UI 等待取消回调。调度器租约覆盖整轮 process，卸载等待整轮结束。


### 本轮最终验证结果

- **27/27 通过**：`release_2.4.1/core-tests-16/results.json`。27 项包含旧核心合同、`replacement_journal` 和 6 项阶段合同；崩溃注入的预期退出码仍为 82。
- `slicing_stages`：10 个实际可达阶段的顺序和对象/Print 范围、原生阶段内 Python 修改、热切不重复派发、只改围墙/填充后不重新 slice，修改后的 raw_slices 与 bbox 保留。
- `slicing_shared`：两份等价对象进入真实共享路径，仅源对象派发。初始夹具复制时宿主补 extruder=0，原对象却没有这个键，导致两份对象不等价；`core-tests-15` 和 diagnostic-01 保留失败。明确补齐相同默认值并断言实际共享后通过，未改生产共享算法。
- `slicing_failure` / `slicing_cancel`：在 psSkirtBrim 注入异常/取消，失效整张图、避免自身工作线程调用 UI 取消等待、重试后不叠加旧变形。
- `slicing_drain`：真实后台切片过程停在阶段栅栏时，detach 等待整轮完成，后续阶段使用同一调度器；`slicing_noop`：未安装 hook 与空操作 hook 的原生围墙/填充路径、角色、宽高和流量完全相同。
- `replacement_journal`：损坏 journal 不删文件、恢复有效 journal 后旧配置恢复；未发布 staging 清理；无 journal 的备份保留并拒绝恢复；dangling journal 符号链接拒绝；修复后正常替换仍可用。符号链接子用例在 Windows 暂不运行，仍需平台验证。
- 最终完整应用构建：`core-slicing-stages-build-3.log`；最终原生测试可执行文件构建：`core-slicing-shared-build-2.log`。测试别名 Step 与 STEP 模型类型的冲突已修正，首次编译失败记录保留在 `core-slicing-stages-build.log`。
- `git diff --check`、测试脚本语法检查通过。源码/二进制 SHA256 与 GUI 分组索引更新至 `verified-core-build.json`。未提交、未推送。

这些结果不包含磁盘导入切片缓存的全组合、完整 SDK dispatch、真实 Z contour、G-code 插件后处理、纹理消费链、Windows/Linux 发行包或真实设备回归，**一期尚未完成**。


## 2026-10-22 生产切片插件调度与真实选择器

最新完整应用与原生测试构建：`release_2.4.1/core-slicing-dispatch-build-4.log`，两个目标均完成链接。`core-tests-17/results.json` **30/30 通过**（包含原有27条）。`replacement_crash` 的实际退出码82符合设计，不是普通退出0。前3次构建的失败日志保留：版本头在 release 基线不存在；测试的 fs 别名与 Boost 冲突；配置序列化/严格反序列化 API 适配。修正后重新完整链接并运行测试。

新增3条生产合同：

- `slicing_plugin`：实际安装 PEP723包，注册 SlicingPipeline 类；10个真实可达阶段；Python修改10mm轮廓为5mm，原生围墙消费；热缓存不重复执行；全局配置改变后下一次 process 重建为6mm，同版本能力重载也重建；预设覆盖改成7.5mm；清空选择恢复10mm。验证字段序列化往返，未冒称实际3MF往返。
- `slicing_plugin_errors`：整批能力预检（后面的包缺失时，前面的包也不执行）；缺失/禁用/歧义引用/坏JSON明确中止；Python异常、FatalError及同步UI调用拒绝后图失效，修复配置可重试；runtime无dispatcher但预设仍选择插件时显式失败。
- `slicing_plugin_pin`：真实后台Python回调挂起；卸载/禁用/删除/替换均拒绝；同时修改全局配置不影响本轮所有10个回调的快照；结束后可卸载，无引用泄漏。

真实 macOS arm64 GUI：`/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/plugin-core-slicing-01-中文`，PID30487，使用本次 build-4 的独立 ad-hoc App。实际操作管理窗口的 Slicing plugins、用键盘勾选能力、OK、重新打开，显示 `Contour contract [preset: slicing;;Contour contract]`；关闭后读取该预设选择，后台原生 Print 生成5层、5mm轮廓和原生围墙。`gui-slicing.json`、`.startup`、`.exit` 与 `verification.json` 通过；解释器结束、OnExit=0、进程消失。`selector-interactions.json` 保留实际操作摘要。该测试覆盖真实选择器与后台原生 Print，尚不覆盖 Plater 的完整导出、上传和工程往返。

启动时首次观察到无窗口，采样为 wxApp::CallOnInit 下的 AppKit 事件等待；后来出现设置向导，关闭向导后进入测试。未据此修改应用启动代码，也未将该采样当作崩溃证据。AX对选择器checkbox的Press/Value写入不支持，采用标准键盘Down/Space并通过重新打开预设引用验证结果。

仍缺：3个阶段（curled DONE、真实contouring、G-code后处理）、三维挤出数据、自动GUI重切调度、磁盘缓存组合、完整3MF/预设持久化、完整SDK/纹理消费者、Windows/Linux/正式签名发行与原业务回归。设备插件按用户授权暂缓，原设备回归仍保留。**一期未完成。**


## 2026-10-22 G-code 与切片/导出生命周期

- `core-lifecycle-build-1.log`：完整应用及原生测试目标构建成功。`core-tests-19/results.json` **36/36 通过**，包含原有30项和6项 G-code/生命周期合同；真实崩溃注入退出82仍符合预期。
- `gcode_plugin` / `gcode_plugin_errors` / `gcode_plugin_pin`：传统脚本顺序与改名、连续副本导出、make_copy=false上传路径、BBL参数别名、Skipped继续、RecoverableError/FatalError/Python异常/删文件回滚、缺能力完整预检、runtime缺失拒绝、能力/配置快照与整轮drain。
- `gcode_plugin_export`：真实立方体切片→原生G-code生成→两次插件处理；最终字节等于原生内容加两条有序footer，预览源文件保持原样。不是完整Plater或真机上传验收。
- `gcode_plugin_events` / `gcode_plugin_event_pin`：真实切片/生成事件顺序、模型ID、缓存生成不重复事件、原生文件打开失败发一条Error终态、已有G-code重载、已取消worker不派观察者、观察者异常隔离、后台同步UI拒绝、回调持有包引用。GUI SlicingJobComplete已经接收工作线程的真实状态，交互验收仍待补。
- `core-tests-18`、`gcode-tests-01` 保留两次夹具错误：config_value返回字符串；Python os.environ不会自动同步C++ setenv。依据真实绑定/解释器行为修正夹具后，gcode-tests-02与完整core-tests-19通过，没有为适配错误测试改动公共API。
- 代码入口与使用方式见 [GCODE_LOCAL](GCODE_LOCAL.md)。源码仍在继续并行修改，以上构建证据只对应此次已构建版本，不代表后续内核/纹理/发行改动已验收。

## 2026-10-22 并行批次生产验证（52项）

- `core-parallel-build-5.log` 完整App、core测试、SDK导出器、preset native全部链接成功。`core-tests-21/results.json` 52/52通过；崩溃注入实际82符合预期。`verified-core-build-parallel-52.json`记录App/test SHA，`verified-core-build.json`指向本次证据。
- `plugin-off-build-1.log`插件关闭的完整App成功；`verified-plugin-off-build-1.json`记录SHA。未做OFF GUI或跨平台运行；精确接缝后续批次需另编。
- 本批生产native覆盖路径/轮廓及G-code真实消费者、纹理、配置通知/活动切片失效、先前安装/回滚/后处理/页面Host合同。此前core-tests-20的47/52保留为中间失败，不是最终证据。
- GUI workflow-01：示例误把get_config字符串当字典，切片异常；已修为json.loads。workflow-02：实际切片/G-code及生命周期完成，精确尺寸断言失败；U1基准首层有0.15mm象脚补偿，helper修正测试参数并加入实际层宽诊断，待重编验证。两轮都不算GUI通过。
- 正在补迁precise seam、可编辑纹理Model/Undo/3MF，尚不在52项覆盖范围。Windows/Linux、正式签名/公证与真机/性能未验收。

## 2026-10-22 并行批次8与GUI验收

- `core-parallel-build-8.log` 完整ON与 `core-tests-23/results.json` **55/55**；OFF完整 `plugin-off-build-2.log`。当前源码后续纹理批次9不自动继承这些二进制结论。
- observer_queue原生5/5、preset_view原生3/3。后者初次测试未初始化wx导致线程判断失真，已改真实wxAppConsole启动后通过。
- 静态配置转动态配置漏枚举映射导致两类3MF崩溃；修复构造使用定义创建option后texture_3mf通过。精确接缝另发现Prusa导入白名单丢子类型，已修待专测。
- `/Users/leo/CodeProject/Orca_Dev/plugin-smoke/texture-03-中文/verification.json`：真实wheel资源、GLerror0、12→1082三角、撤销/重做、3MF重开、2564 G-code moves、正常exit0。此为旧单次烘焙界面，新可编辑界面另验。
- `/Users/leo/CodeProject/Orca_Dev/plugin-smoke/preset-02-中文/verification.json`：预设因子0.75、全局仍0.5，实际5层7.5mm轮廓、正常exit0。UI额外确认非法JSON拒绝、取消不保存、继承恢复；关闭textarea自动更正修复macOS弯引号。
- workflow06：双盘隔离、实际三次切片/导出配对与Dock重入销毁业务报告ok；退出被测试修改的预设提示阻断，未声称clean exit。NewProject使用silent夹具和未实际保存的dirty事件覆盖不足已补实际New/Save流程，下一构建重跑。
- SDK05十模块独立校验及固定上游324具名注册符号对照，仅暂缓selected_printer；分发Python10/10、SDK7/7、本机runtime六阶段通过。Windows/Linux完整宿主仍未执行。

### 2026-10-22 第十批并行集成

- 完整 ON build10 / OFF build3 成功；生产核心 core-tests-24 **62/62**。
- workflow-07 实际GUI：NewProject、Save前后、dirty变化、双盘隔离、3次切片/G-code配对、重入Dock销毁、runtime结束、正常exit0全通过。
- texture-04 实际新编辑器：插件图像→Add layer→Paint all→真实GL预览→Bake，12→692三角形；Undo/Redo和3MF层/image/UV/旧paint均通过。最终切片每region必须有perimeter断言失败，仍未记全链通过；进程正常退出。
- precise-seam-native-03：配置enum、Python合同、Prusa3MF通过，BBS dormant extruder被旧通用归一化覆盖，最小修复待下一批。
- runtime-host-relocation-02 严格真实宿主probe通过：移目录后包内stdlib/orca有效，CWD与污染PYTHONPATH不进入sys.path。SDK06真实导出10模块/校验通过。


### 2026-10-22 第十五批：68 场景、真实四色纹理与页面恢复

- `core-parallel-build-15.log` 完整 ON、`plugin-off-build-5.log` 完整 OFF 成功。`host-ci-native-01/results.json` 为真实已有产物模式：68 核心、preset bundle/view、precise seam、observer queue、OS lock、SDK generate/verify/contracts 全通过，记录全部宿主/测试二进制 SHA256。未自动构建/运行远端 Windows/Linux。
- `texture-06-中文` 与 `texture-color-02-中文` 都有完整 `verification.json`、实际图像、应用业务报告和 exit0。灰度 12→692 三角、2522 G-code moves；RGB 1432 三角全部分配真实 1/2/3/4 耗材，32101 moves 包含工具 0/1/2/3。两组均验证图层/image/UV/MMU 的 Undo、Redo、3MF 重开与切片。使用 build14，不能冒称其后的编辑器新工具已覆盖。
- 保留失败：texture-04 每 region 必有围墙的断言过严，原生对照证实空 fuzzy region 合法；texture-05 因 volume 没有 extruder option 在新色彩对话框构造时空指针。改为安全读取 volume→object→默认1后，两条完整 GUI 重跑通过。texture-color-01 只有一个实际耗材的错误夹具也保留；修正通过真实 PresetBundle 设置四耗材。
- build15 的 `workflow-08-中文/verification.json`：实际双盘/保存/dirty/三次切片与 G-code，正常退出；重入 Dock teardown、真实 AUI 同名身份、尺寸/位置/转义标题保存后重建恢复通过。不是普通关闭后的跨进程 Dock 验收。
- build15 的 `pages-multi-01-中文/verification.json`：PID31057 与 PID33625 两个实际进程；两个独立 Pages 的 HTML↔Python 消息桥、快速 reload 的 generation 隔离、重启后历史数据保留、解释器结束与退出0均通过。并非仅模拟重启。
- 构建及 GUI 产物根分别是本地实施目录的 `release_2.4.1` 和 `/Users/leo/CodeProject/Orca_Dev/plugin-smoke`。新增编辑工具/手工 seam 与 UV 作者能力、Local-Z/Contour 组合另验；Windows/Linux、正式签名/公证、固定上游真实双宿主与设备/性能仍未验收。


#### build15 新编辑操作真实窗口验收

`/Users/leo/CodeProject/Orca_Dev/plugin-smoke/texture-editor-07-中文/verification.json` 通过，PID31158 正常退出。`interaction-evidence.json` 与逐操作 AX 状态/截图保留实际操作：新增层与整面绘制 Undo/Redo、圆形画笔连续拖动得到胶囊形 mask、Shift 拖动擦除、整笔 Undo/Redo、Smart fill 角度60°实际只填当前平面；Model UV 的无效 tile 参数与灰度图的 RGB 开关禁用并解释原因。

UV 窗口显示真实纹理图像背景；实际选中岛、旋转15°与放大1.1，接受后作为一个历史编辑单元，Undo/Redo正确；再次 Reset 后 Cancel 没有产生或替换历史。最终真实 GL 预览与烘焙均2378三角；可编辑图层、图像、UV和MMU的Undo/Redo与3MF重开匹配，最终3328 G-code moves。此证据不包含随后添加的手动 seam、UV顶点/边编辑。

## 2026-10-22：batch16/17 与真实手工 UV 作者工具

- ON build16 / OFF build6 完整通过；host-ci-native-02 的核心70、预设bundle/view、precise seam、observer queue、lock、SDK生成/校验全部通过。二进制身份见实施目录 `release_2.4.1/verified-core-build-parallel-70.json`。这属于macOS arm64本机现有构建验收，非三端远端CI。
- `texture-authoring-08-中文` PID39747 实际操作：手工标记接缝、Clear/Undo恢复红线；LSCM顶点拖拽与Undo/Redo；边拖拽同时改变两端点且可Undo/Redo；UV对话框提交在主历史中为一次操作。Preview/Bake 7328三角形，GL error0，实际Undo/Redo与3MF重开，2992条G-code移动，解释器释放/正常退出0。具体图像与动作证据见该目录 interaction-evidence.json。早期坐标基于错误组合截图的无效拖拽不算通过。
- 上例先Bake；Bake按设计消费手工接缝/UV overrides。因此其烘焙后图层/图像/UV/MMU roundtrip不能作为烘焙前作者数据持久化证据。新增 --texture-authoring fixture 单独覆盖Save editable→Undo/Redo→3MF→reopen→第二次对话框Bake；尚待实际执行。
- app17仅修改Dock测试夹具，真实切换Prepare再请求插件Dock。dock-normal-02首次PID42470保存并正常退出通过；第二次PID43100实际布局/尺寸不一致，明确FAIL而非验收完成，已并行调查并新增双边持久诊断。原workflow-08同进程AUI Save/Load仍为已通过的独立范围。
- 新增笔刷split-tree接缝传播和UV岛选择/组移动/Join/Unjoin已合入，build18/72场景待验证。

## 2026-10-22：batch18 / authoring09 完整编辑工程闭环

ON18/OFF7与host-ci-native-03全部通过：72核心场景、全部专项和SDK。新增brush子边传播在真实TriangleSelector分割树、重合独立表面、T接点和采样/烘焙消费者上验证；UV岛组测试经过真实corner-UV消费。

`texture-authoring-09-中文` PID48580真实窗口完成两点接缝路径、全选/Shift加选/Cmd移除、多岛拖动、Join nearest、清选择后单岛带动整个组、旋转只作用主岛、Unjoin与单独移动、Undo恢复组，及旋转后顶点修改。Save editable触发真实主历史Undo/Redo、3MF写入和重开；逐项比较所有图层/选项（仅忽略writer填写的path_in_3mf）、图像字节、绘制mask、原网格和ModelUV一致。

实际存档 `texture-authoring-editable.3mf` 包含seams `[5,6,1,6]`、UV override键`-12`、groups `[0,0]`及图像blob。重开UV对话框后再次拖动成员，整组仍移动；取消临时移动后Preview/Bake6744三角形、GL0，再Undo/Redo、第二次3MF保存重开与最终3027 G-code移动通过，正常退出0。验收脚本及interaction-evidence.json已保存。

Dock03保留失败证据：第一PID48633配置有2条plugin pane，第二PID50046创建pane前保存布局已无plugin记录。链路为post_init→restore blank project→new_project→reset。新修复只在保存布局时保留尚未创建的插件pane及关联dock_size，已有pane包括显式隐藏以当前为准。真实两进程布局重放与独立原生通过；production GUI重跑在batch19。

## 2026-10-22：batch19 / Dock04 与图像调整闭环

ON19/OFF8完整构建、host-ci-native-04原生73场景及全部专项/SDK通过，二进制身份见verified-core-build-parallel-73.json。Dock04两个正常进程PID55538→56624，保留真实插件pane至主窗口退出；两次exit0、第二次方向/层/行/位置/尺寸/转义标题匹配，修复生产重启链路得到实际确认。

texture-image-10-中文 PID55543真实灰度调整：五控件可见，灰度Hue禁用并解释；亮度/对比度/锐度/模糊触发真实预览，Reset恢复全零，Cancel恢复原692三角预览；亮度0.25另存到User库，外层Undo/Redo一次历史。最终Preview/Bake508三角、GL0、原插件图像字节不变、新图解码像素改变、preview_adjust清零避免重复，Undo/Redo、3MF重开和2608条G-code移动及exit0通过。interaction-evidence.json记录实际动作。RGB图像调整另验。

小尺寸源图预览发现2×2像素不可见，后续build20仅将对话框显示缩放至384×192边界，源图采样保持原尺寸；此显示修复与UV布局工具、Local-Z/Contour合法组合等待下一批生产验证。

## 2026-10-22：batch20 / Local-Z 与 RGB 图像调整

ON20/OFF9完整构建，host-ci-native-05核心77/77及全部专项/SDK通过。新增texture_layout_tools和Local-Z三场景，包括真实writer累计Z/3D体积/相对绝对E/精度限流/上下层高拒绝/异常恢复。ironing、scarf、adaptive PA仍明确不支持；真实打印并未验收。

RGB image11实际PID61748：色相60°+亮度0.10临时预览后取消；重开0值，再以10°+0.10另存User副本，外层Undo/Redo，重开调整值全零而图像为新像素，避免重复处理。小图显示192×192，原采样尺寸不变。最终1122三角形、GL0、原插件原图不变/新像素嵌入、Undo/Redo、3MF重开和32561移动/四实际工具、正常exit0均通过。ModelUV禁用Average/Auto-connect的实际状态也通过。

检查工具getAXState在原进程已退出后自动重启测试App；已取消后起的独立检查进程，未将其计入成功。原PID61748完整报告在覆盖前保留于verified-first-process-61748，最后取消实例在inspection-relaunch，interaction-evidence.json记录该操作边界；主验证结果仍严格指向原成功PID。

### layout12 真正的 UV 布局操作

PID63042/build20：六面LSCM展开，关闭自动连接保持布局；一岛放大三步后Average实际等大小，Undo/Redo恢复不等/均值；手动偏移后Auto-connect恢复接边，Undo/Redo同时恢复布局和checkbox。开关true时手动再移岛、确认并重开，偏移保留；临时Reset/Cancel不替换外层历史，外层Undo/Redo为一次编辑。最终3006三角、GL0、Undo/Redo、烘焙后3MF重开、2116移动与正常exit0通过。此例未在烘焙前保存工程，完整pre-bake authoring另见authoring09。

## 2026-10-23：batch21–23 / Cut15 修复与 Result 参数实际消费者

ON21核心79、ON22核心80、ON23核心81及各批全部专项/SDK通过；最新完整产物为ON23/OFF11，host-ci-native-08/results.json与verified-core-build-parallel-81.json记录身份。新增经典Result消费者以真实网格验证边界固定/位移、平滑强度与迭代、外环保护、零迭代不改几何；可见性原生覆盖同朝向遮挡面、非均匀/镜像变换、薄壁与实际烘焙。真实GUI可见性验收另列。

Cut13保留失败：旋转/缩放后的六岛模型首次Cut会因显式接缝取代自动角度边界而合并其他岛，安全检查拒绝且原数据未改。修复冻结现有岛之间的真实原边边界，再添加新切割边；不存在原边祖先的内部细分边仍明确拒绝。Cut15/build22/PID79469实际重复原失败步骤后成功：15°/1.1倍方形岛切成两个可独立拖动的三角岛，其他五岛保持位置；Undo恢复完整方形，Redo恢复两片；外层一次Undo/Redo、Save editable及原生主历史与3MF重开保持所有作者数据。重开UV后临时Reset/Cancel不替换保存布局。Preview/Bake3252三角、GL0、再次Undo/Redo与第二次3MF重开、2104条G-code移动、exit0通过，证据位于texture-cut-15-中文/verification.json及interaction-evidence.json。

Result14/build21/PID71004实际验证边界/平滑五个公开选项的编辑、开关保留数值、Undo与工程存储，V2下Preview/Bake692三角/2522移动全链通过；这只证明参数保存，不能证明V2支持这些效果。随后核对固定上游及本地真实消费者，V2分支直接返回且不使用displace_border/smooth_*。build23禁用V2下五项并显示经典管线适用说明；关闭Refine mesh才开放，数值仍保留。真实经典几何效果由texture_editor_result原生场景验证。上游隐藏的relocate/regularize开发控件未擅自公开。

### visibility16 / Result 适用条件真实窗口

build23/PID81698：V2禁用五项经典参数并说明；关闭Refine后可编辑平滑40%、3次，切回V2禁用且保留。Cube真实可见面捕获、orbit观察未选面、替换而非累加、Undo/Redo、关闭可见限制后Capture仅改投影、Paint visible独立替换均通过。Preview/Bake12640三角、GL0、实际Undo/Redo、完整图层与options的3MF往返、1591移动及exit0通过。证据texture-visibility-16-中文/verification.json和interaction-evidence.json。凸cube只证明朝向与mask替换；同朝向前后墙的遮挡由texture_visibility真实几何消费者独立验证。此例先Bake，不冒充该mask的烘焙前3MF GUI验收。

## 2026-10-23：UV 顶点/边多选与投影框原生验证

- ON build24 的 `texture_authoring` 扩展场景通过：顶点别名仅在同一图块合并，边按端点对保存，移除相邻边时保留仍被选中边引用的公共点；真实 LSCM 逐角 UV、移动与持久化均覆盖。
- `texture-multiselect-17-中文`（PID88881）真实 GUI 通过 Shift 追加、Cmd 取消、同时拖动、切缝副本隔离、两级 Undo/Redo；保存可编辑 3MF 后重新打开的独立三角布局保持。最终烘焙 3090 三角形，重开切片 2112 条移动，OpenGL error=0，正常退出，verifier PASS。交互证据在同目录 `interaction-evidence.json`。
- ON build25 新增投影框后的 `texture_visibility` 原生场景通过：有限画框裁切、背面/空框、Retina 比例、框移动、非均匀缩放下真实经典烘焙深度及 12 元素矩阵存取。实际编辑器的框拖动/缩放/透明度与界面链尚待验证；原生支持投影矩阵不等于界面已提供透视相机。
- 插件生命周期两项确定失败（旧卸载覆盖新配置、shutdown 早于在途卸载完成）及修复后的九场景证据保留在 `lifecycle-audit-staging-01`；修复已合入源码，正式新构建尚未跑，不能替代整体验证。

### Projector18 的实际失败与修复依据

`texture-projector-18-中文`（ON25，PID93767）真实编辑器显示浮框，但标题拖动、缩放、Close 和 Escape 均无效果；两次只读窗口记录位置尺寸相同。sample 显示调用仍在 EditableTextureDialog→wxDialog::ShowModal→wxModalEventLoop。保留 `interaction-evidence.json`、两次窗口记录和 sample；证据采集后终止隔离测试进程，不计正常退出通过。

本机实际 wx 3.1.5 Cocoa 源码 `src/osx/cocoa/nonownedwnd.mm:774–778` 根据 `wxFRAME_TOOL_WINDOW` 创建 NSPanel，`:881–906` 只对 NSPanel 设置 worksWhenModal；FLOAT_ON_PARENT 单独仅改层级。源代码加入此标志，维持上游 Show/Hide/Destroy 所有权。MSW 的同一标志映射 WS_EX_TOOLWINDOW，GTK 映射 UTILITY，仅为源码核查；修复后的真实窗口输入与退出仍须重验。


## 2026-10-23：生产生命周期与 Pages 旧实例消息

- ON26 `orca_plugin_unload_overlap_test` 由当前 host driver 顺序启动九个独立中文 profile，**9/9 PASS**，所有进程退出 0，运行前后二进制 SHA 一致。覆盖单/批卸载重叠和停机、替换、删除、回调重入、析构后正常清理、持真实 GIL 的停机。完整日志、脚本与二进制 SHA：`release_2.4.1/lifecycle-production-01`。
- ON26 `print_object_id` 生产回归通过。先用 allocator-poison 在旧 ON23 稳定复现未初始化值；构造器初始化为 0 后验证默认值及显式 `set_id(42)` 均正确。`print-object-id-production-01.log`；性能 fixture 自身固定 ID 的先前样本不能替代这个生产修复测试。
- Pages baseline03（ON27，PID7237）先建立两个真实 WebView 并完成 JS 往返，再在 GUI observer 队列未处理卸载时完成新实例加载。向旧 WebView 派发同类 script-message 事件后，实际出现 `Late old Page message overwrote new generation configuration`。该进程正常 OnExit 0；这是确定的竞态失败证据。
- Pages fixed04（首次 ON28 PID10490，重启 ON29 PID10875）增加 manager capability pin 与精确实例校验后，同一步骤验证旧消息无效、新实例 sentinel 不变。两页独立往返、快速重载、图标 callback（正常 SVG 和故意缺失图标）、跨进程配置恢复全部通过，两次正常退出 0。新 verifier 强制检查迟到消息与图标字段。目录 `plugin-smoke/pages-late-fixed-04-中文`；baseline03 保留不覆盖。图标回退通过不等于已完成大量页面的溢出布局设计。
- 自动混色 actual20（ON27，PID8830）两次真实预览均为 196 三角；首次取消前监测 372 次，模型、完整配置、sidebar 与历史均未变。烘焙后夹具错误要求 volume ID 不变，实际 geometry bake 按既有逻辑分配新 ID，此断言与已有 native 测试相矛盾。修正夹具为新 ID 非零且不同于旧 ID，后续 actual21 重验；不将 actual20 计为全链通过。
- 投影框 actual19（ON26，PID4008）确认工具窗口样式已生效，但 Cua 标题拖动后窗口 bounds 不变、AX close 未隐藏；按隔离失败测试 SIGTERM 结束。无正常退出通过声明，继续诊断实际模态输入与自动化路径。原生矩阵/裁切 PASS 不替代这项交互验收。


## 2026-10-23：完整宿主检查点与旧业务回归

- `host-ci-native-09`：ON29/OFF12 已有生产产物运行 **21/21 阶段 PASS**，包含82/82核心、9/9生命周期、预设bundle/view、精确接缝、observer队列、PrintObject allocator-poison、跨进程锁、SDK10模块导出与核验。driver用 `--no-build --test-jobs 2`，120.271秒；源码与9个native target、ON/OFF主程序SHA前后一致。`results.json`、`invocation.json`、109份证据SHA清单均保存。此后仅GUI夹具修正的构建不冒充重新跑过整个检查点。
- `legacy-fff-geometry-01`：离线固定Catch2 v3.7.1，FFF33/33、libslic3r316/317，总350默认case、349 CTest PASS、1普通失败、0skip。349中含原1 mayfail和2 shouldfail，并非所有断言通过。全部测试/fixture/kernel SHA前后一致；原隐藏及条件编译用例范围在报告列明。
- 唯一普通失败：SSWCP cached flow不完整时实现清空direct flows，而头文件/测试期望保留。实现/头文件/测试在release、当前和旧review源码逐文件SHA相同；隔离复制旧程序又复现同一断言。故可确认源合同冲突原已存在，但旧程序缺完整构建manifest，未把它冒充全新固定baseline重建。没有改代码或白名单；旧tree bridge在本次当前suite通过。
- `distribution-handoff-02`：pinned Python3.12.13实跑11+18+9脚本合同全通过，含启动失败日志、timeout与输出路径别名保护；现有macOS搬迁包原生runtime+空缓存离线NumPy重装通过、输入SHA不变。Windows/Linux仅完成可交接脚本及操作说明，未声明原生实测。


## 自动混色 actual21 / actual22 后续证据

- actual21（ON29，PID13671）实际生成 stable_id=1、mixed paint ID=5、196三角、sidebar从4到5行、project/print definitions完全一致。第二对象导入调用低层LoadStrategy API没有GUI入口的TakeSnapshot，Undo越过Bake，触发夹具错误。已改用原GUI的wxArrayString overload及正常尺寸模型；不修改生产历史逻辑以迎合该夹具。
- actual22（ON31，PID20005）通过预览/Cancel、真实Bake、选中第二对象后Undo删除、跨Bake多步UndoRedo、改变物理色后的普通Undo保持新色、分支后旧redo清理与实际redo无效果。后续在真实3MF重开发生 `History changed the expected mesh`，保留 `真实自动混色.3mf`，诊断中。尚未运行最终G-code检查，不能称全链通过。
- 夹具的分支检查按本分支UndoRedo栈真实语义修正：topmost继承Action会让 `has_redo_snapshot()` 在栈顶也为true，故改查旧时间戳消失、active位于末端，并实际执行redo验证time/model/palette不变。


## 投影框输入诊断边界

独立 `texture-projector-input-probe-01` 复用实际 wx 库、模态父窗及生产 TOOL/FLOAT flags。PID15887 经真实 Open 按钮显示后记录 `wxNSPanel`、`worksWhenModal=true`、`wx_enabled=true`、`capture_id=0`；但 Cua 窗口枚举标记 overlay 不在当前可见 Space，截图仍为父模态窗。本次没有完成标题拖动/原生关闭验证，正常 Finish 后退出。AX 按钮可绕过 NSEvent local monitor，不能用没有 monitor 事件推断投递失败。结论为自动化交互未确认；不再叠加无根因的产品改动，保留为实际人工交互验收项。已向用户询问是否现在协助手工检查，其余工作继续。

## 2026-10-23 混色 actual23 与归档遮罩缓存

`texture-mix-23-中文`（ON32，PID24825）经真实鼠标 Preview/Cancel、Preview/Bake，历史和混色事务检查通过；实际3MF的100顶点、196三角形精确符合BBS居中，补偿后的实例矩阵也符合已知床面归一化。世界坐标最大float差为2.384185791015625e-7 mm。之后因 `Layer mask changed` 失败，正常OnExit 0，PID已消失；保留失败报告，不记完整通过。

独立诊断 `texture-mix-mask-review-01` 使用该实际3MF和生产 paint→set→reset 复现：slot0的mapping/bitstream均为空，但旧 `FacetsAnnotation::reset()` 留下 `used_states[1]=true`；3MF仅保存绘制编码，重开后派生缓存全false。真实遮罩没有丢失。归档夹具只将expected缓存从编码重建，loaded完整结构仍必须exact；live Undo仍要求全部exact。6项阴性（包括真实新增绘制和伪造loaded缓存）、非空遮罩正例和编码bit损坏拒绝均通过，完整helper语法通过。未改全局reset或放宽实际绘制比较，等待重新真实GUI验收。

## 2026-10-23 app_info 语义与诊断收尾（生产验证进行中）

已合入 `app-info-staging-01`：`is_signed_in`读取现有Snapmaker Gateway最近已收账户快照，`is_bambu_signed_in`独立读取旧Bambu账户。断线继续保留原快照；不等于二期插件云授权，也不暴露账户标识或token。网络版本/错误来自实际模块状态，错误经历失败cleanup保留、成功重载清除。模块生命周期/诊断互斥不扩张为所有旧设备业务方法线程安全承诺。

私有真实动态库8组、3个GUI/绑定TU语法通过；新增统一Host CI阶段及18组工具合同通过。生产ON33因fixture刻意匹配C符号名+C++string返回ABI的Clang警告失败，仅对3个测试库加定点warning例外后重启ON34，OFF13同步构建中；生产代码未关闭该warning。真实新GUI十字段及完整native门禁仍待本批结束后运行。


### 2026-10-23：ON35/OFF13、完整driver与自动混色actual24

- `core-parallel-build-34.log`、`plugin-off-build-13.log` 均退出0；合入GUI验收夹具/独立bulk target后 `core-parallel-build-35.log` 退出0。`host-ci-native-10` 实际82核心、9生命周期、8 app_info、22阶段、10 SDK模块通过，耗时125.517秒；15个产物与8个driver输入前后SHA一致。详细版本与SHA见其 `summary.json`。
- `texture-mix-24-中文`（实际PID40720）完整通过：真实Preview→Cancel→Preview→Bake，196三角、多步正式Undo/Redo、选中新增对象删除Undo、普通Undo物理颜色保留、分支清理、稳定混色ID和两份配置归档重开；真实U1输出8788移动、7905挤出，工具0/2，正常OnExit0且PID消失，通用GUI验证器退出0。
- actual23的遮罩失败保留。actual24夹具精确比较编码数据/三角映射，归档后派生used_states仅按原有规则从载入值重建；不改生产reset行为，不放宽真实paint数据或实时Undo比较。3MF几何仍精确保留顶点统一居中、三角顺序与补偿变换。
- `dualhost-first-01-中文` 固定包真实Pages/Texture首次两例通过，PID41610/42104均正常退出；collector为2 observed/54 pending。Pages真实JS→Python→DOM回环；Texture真实GL非背景像素76322、GL error0、12→17188三角、实际Undo/Redo与3MF重开。Texture重开遇到自生成3MF的Customized Preset提示，实际Escape关闭后继续；不是预写成功。全量矩阵与上游仍待完成。
- 并行审查新发现依赖发布中途退出与wheel部分解压缓存的恢复风险，正在独立staging复现修复，尚未合入此检查点。该检查点不等于一期发行全部通过。


### 2026-10-23：真实Plater三态actual02通过

`legacy-states-01-中文`严格验证失败保留，原因与修正见 LEGACY_STATES.md。ON36仅修正验收夹具调用正式set_num_filaments入口；产品PresetBundle不改。`legacy-states-02-中文`（PID45704）四次真实切片、三次Save、启用工程重开通过，5层/777 moves逐次成立；不装/安装禁用G-code原件字节相同，启用和重开只允许精确选择器/完整预设差异列表、时间戳及唯一marker差异。全部其余配置/指令、世界坐标模型/绕序、保存3MF选择器和真实生命周期trace通过。正常OnExit0/PID消失及独立延迟crash复查通过。该范围是真实Plater生成+生产final-file入口，不含Export原生保存对话框、设备或Windows/Linux。证据 `legacy-plater-states-02/summary.json`。


### 2026-10-23：ON37 最终安装恢复集成与同包矩阵

`core-parallel-build-37.log` 完整应用及全部显式原生目标退出0；OFF13继续作为不含插件恢复代码的对照。`host-ci-native-11` 共24阶段全部通过：82核心、9生命周期、8 app_info、9步真实uv发布中断/恢复、12项主/依赖wheel缓存恢复以及SDK生成/校验。18个受测产物保留SHA，新增恢复源码前后检查及本批6个生产源码构建前后哈希一致；摘要见 `host-ci-native-11/summary.json`。

修复前真实故障独立保留：旧依赖重命名后退出会在下次启动暴露空目录并可能丢失唯一备份；损坏wheel首次失败留下的残缺缓存可在第二次导入执行；大小写不敏感卷上的同名别名会覆盖先前文件。修复后在CPython前恢复依赖备份，wheel完整解压并记录归档哈希/必要文件大小后才发布；同坏包重试仍拒绝，损坏缓存可恢复，完整缓存热复用不改写。完成记录不是签名或同大小篡改检测。Windows/Linux及大小写敏感卷原生结果未生成。

`dualhost-full-01-中文` ON35生产GUI共27个独立进程正常退出，Pages/Texture的28项真实消费者矩阵已收齐。与既有native28汇总为 `dualhost-snapmaker-aggregate-01`：56 observed、coverage_complete=true。每项保留其原始report/二进制SHA；这只证明Snapmaker侧，固定Orca上游仍pending。全量矩阵中自生成3MF的预设提示由实际UI关闭后继续，没有伪造完成。

ON37追加真实GUI复验 `dualhost-on37-01-中文`：PID55037/55437正常退出，multi Pages/Texture首次加载与同包重载共4项 observed；真实JS消息、GL预览、12→17188三角烘焙、Undo及3MF重开通过，两进程延迟PID/崩溃检查通过。该4项复验单列，不覆盖或替换原56项矩阵的逐case来源。`bulk-production-validation-02` ON37 side32、1 warmup+2测量循环通过；它是功能验证，不更新ON35正式性能样本。

### 2026-10-23：普通开发 App 实际交付

`plugin-development-on37/Snapmaker Orca Plugin Development.app` 包含真实复制资源、Python/uv与许可证，使用独立data_dir。原始 raw Python prefix 在Frameworks下导致非Mach-O脚本签名失败，后续framework顶层BUILD资源也触发失败；两次失败日志保留。组装器使用标准framework/Resources与内部兼容别名保留完整前缀后，15工具合同、完整strict签名、14 Mach-O架构/依赖检查及Python/uv/工具入口验证通过。该修正仅在开发App组装器，不冒充正式发行hook完成。

主线程实际启动PID57246，正常向导选择U1/Generic PLA并点击体验改进计划“忽略”；privacy_policy_isagree仍false。普通主界面显示后点击原生关闭按钮，后续Cua AX读取重新启动PID57725；两次命令行均明确指定同一独立data_dir，第二次无需重新向导。第二次只点击关闭再用PID/崩溃检查确认退出，避免再次自动重开。两进程延迟检查通过；未取得数值exit code，未写成exit0。GUI后完整strict签名与12858项包内inventory对照仍通过。证据 `plugin-development-on37/gui-acceptance/summary.json`。

同级 `SDK` 的26文件从host-ci-native-11生成产物逐字节复制并记录哈希，`开始使用.md` 提供本地开发/安装步骤。代码未提交/推送；Windows/Linux、固定上游宿主、真机与正式发行、投影框原生标题栏手工验收仍待完成。

### 2026-10-23：首页缺失反馈与打包修复

用户反馈原开发包首页丢失。复现时Home已选中，真实WebView却为 `Printer Connection Required`，路径 `/web/orca/missing_connection.html`；配置show_home_page=true、default_page=0。原Resources内Flutter的204个文件存在且复制一致，缺失的是 `snapmaker_control_macos_arm64`。先前普通GUI启动验收没有覆盖真正首页，因此不能用那次启动通过证明首页完整。

正常release会先stage锁定Connect归档；开发薄包装漏做该步。基线v1.8.0资产URL和release API均404，修复使用显式v1.8.3锁文件、官方SHA256SUMS与arm64归档，SHA为d929a769e048036fa6ce9ce763d1f5c42003b511c6699dfefba897e5f0b99c09。仓库默认锁及ON37宿主源码/二进制不改，旧包不覆盖。组装器新增缺CLI/坏哈希/前端缺损/架构不符等失败拒绝，28项合同通过，15个Mach-O及完整strict签名通过。

`plugin-development-on37-homefix`实际首页URL `/web/flutter_web/home`、菜单入口和近期文件空态通过；实际点击创建项目进入Prepare，再点Home返回首页通过；本地首页HTTP200，GUI后strict签名及完整App文件清单仍通过。现有已落盘开发配置复制到独立data_dir，原包/原目录保留。最终GUI证据为 `gui-acceptance/homefix-summary.json`。官方推荐内容、登录及设备连接/打印未验证；本次不重跑未改宿主的原生82核心门禁。
