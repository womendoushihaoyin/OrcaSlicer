# 本地插件加载卡死：事故与修复说明

日期：2026-10-23。范围为本机 macOS arm64 开发包、六个体验插件及相关原生/GUI 回归；保留所有历史失败证据。

## 事故与根因证据

用户安装体验插件后，Local Plugin Page 显示半屏并伴随主进程卡死。用户进程 **86018** 的 [sample](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/user-process-86018.sample.txt) 显示：主线程在 gateway WebKit 消息的 `CallAfter → QueueEvent` 等待 handler 锁；加载 worker 已持有该锁，等待 wx 全局待处理事件列表锁。原 ON37 六包隔离基线 **88640** 重现相同界面与等待链，见 [截图](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/baseline-window.png)、[首次 sample](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/baseline-88640.sample.txt) 和 [后续 sample](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/baseline-after-startup-88640.sample.txt)。这不是已证明的 Python GIL 死锁，也不能仅归咎于页面样例。

[wx-repro](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/wx-repro/README.md) 使用现有真实 wxBase 静态库、无 GUI：选择性 Yield 形成 delayed 列表时重入 `ProcessPendingEvents`，其锁内 `wxCHECK_RET` 提前返回，漏释放一层递归全局锁。worker/main 的 TryEnter 对照及主线程释放一层后 worker 立即完成，证明隔离进程中的锁归属；实测死锁栈与现场相同。无重入和 CATEGORY_ALL 重入两个对照正常。Release 库的检查会静默返回，缺少断言日志不能排除此机制。**现场首次造成 delayed 重入的具体回调未被事前记录，不宣称完整还原。**

第一版 modal 候选 build01、进程 **93009** 又出现另一问题：[sample](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/candidate-second.sample.txt) 位于 `AddScriptMessageHandler → RunScriptSync → wxYield`。timer 此时 `EndModal`，原生 modal session 中止，后续 Yield 不再派发 JavaScript completion，形成忙循环。[独立真实 WKWebView 复现](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/modal-webview-repro/README.md) 已运行：unsafe 预期 watchdog **70**；guarded **0**、实际拒绝提前退出 **30** 次后注册返回；baseline **0**。对应 [unsafe](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/modal-webview-repro/unsafe.json)、[guarded](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/modal-webview-repro/guarded.json)、[baseline](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/modal-webview-repro/baseline.json)。该诊断程序使用直接退出隔离机制，不能算正常宿主退出验收。

半屏/盖住首页还包含独立的可见性问题：Snapmaker 的带图标 `AddPage(..., false)` 未隐藏新面板。插件页面尚未被选中时仍可能覆盖当前首页。此问题与加载死锁分别修复。

## ON39 修复

- GUI 加载改为专用正常 modal 循环；首个有效 timer 才启动加载，排除 caller loop，移除手动选择性 Yield。busy atomic/RAII 拒绝递归加载；无 GUI 与真实 wxAppConsole 分支使用条件变量等待，并在返回前排空 worker 槽。
- 等待窗无隐式父窗口（`wxDIALOG_NO_PARENT`），弱引用监听主窗口关闭；只有 worker 完成、处于本等待循环、没有其它 modal、且 `!IsYielding()` 时结束。临时 `wxModalDialogHook` 跟踪不切换 wx 循环的原生 NSAlert，Exit/弱引用清理嵌套状态。
- Pages adapter 创建页面后先 `Hide()`，再加入 notebook，避免未选中页面遮住首页。
- 页面体验样例更新至 **1.0.1**，使用主题变量及 `min-height:100vh`。修复版体验包保留其它五包内容；原体验 ZIP 和原用户 profile 未修改。见 [包校验](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/experience-kit-loadfix-validation.json)。

## 已完成的验证

| 验证 | 实际结果与证据 |
| --- | --- |
| ON39 host CI | [24 个阶段全部通过](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/host-ci-on39/results.json)：82 core、9 lifecycle、8 app_info、9 个依赖恢复进程、12 个 wheel-cache 恢复进程，以及 SDK 等检查。该 driver 自身不声称 GUI/搬迁验收。 |
| 正式 CMake runtime wait target | [no-app 与真实 wxAppConsole 两分支均 exit 0](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/runtime-wait-cmake/summary.json)，不是仅私有链接测试。 |
| Pages 实际 GUI | [pages_multi 报告](/Users/leo/CodeProject/Orca_Dev/plugin-smoke/loadfix-pages-on39/gui-pages_multi.json) `ok=true`：未选中页面隐藏、双页面独立通信、快速 reload 代际及旧页迟到消息拒绝均通过；解释器停止并正常退出。此报告 `persisted_across_restart=false`，不将它计作本轮跨重启持久化验证。 |
| 管理器实际 GUI | [六包 manager 报告](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/manager-first-six-gui-manager.json) `ok=true`，6 包 loaded、3 个 Script；解释器停止并正常退出。见 [六包界面](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/manager-six-loaded.png)。 |
| 新普通开发 App | `/Users/leo/CodeProject/Orca_Dev/plugin-development-on39-loadfix/Snapmaker Orca Plugin Development.app` 已实际启动，[首页无遮挡截图](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/on39-home-startup.png) 已保存，主线程确认一次退出操作完成。普通 launcher 不带 smoke 自动执行。 |

## 可追溯构建与交付

- ON39 未重签源宿主 SHA-256：`3ec60771a1fc52d256cd694a56d3b25487386e7643bc79769142ff0d949a44ab`。
- 普通 App 内重签宿主 SHA-256：`bf36c8584bcd36a37378f34c5bec5e5616cb1b167d46caeb071ef06558804548`。
- Bundle ID：`com.snapmaker.codex.plugin-development.6854a256fc0c`。
- [组装交接记录](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/candidate-on39-handoff.json) 与 [开发包 manifest](/Users/leo/CodeProject/Orca_Dev/plugin-development-on39-loadfix/development-app.json) 记录源文件哈希、完整 runtime、15 个 Mach-O 架构/依赖检查及 `codesign --verify --deep --strict`。重签前后哈希不同为预期，不混用。
- Connect 仍使用明确记录的官方 **v1.8.3 override**，未静默替换仓库 v1.8.0 锁。此包为本地 ad-hoc 开发包，不是 Developer ID/公证发行包。

## 仍需保留的边界

1. 原生权限实际拒绝已通过，详见下节；强制系统关闭仍不属于这一流程的证明。
2. 本轮原生文件选择器自动化中 AX Open 失败，键鼠也未可靠选中，已取消。**不能把管理器 loaded 状态或其它安装 API 回归写成本轮 GUI 选文件安装通过。**相关 [选择器证据](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/install-dialog.png) 保留。
3. `CanVeto=false` 的程序化 `Close(true)` 仍可穿透关闭监听；系统/强制升级关闭路径尚未纳入加载完成后的延期重放。正常可否决关闭的保护不能代表所有强关语义。
4. 本次修复与回归不关闭全项目的 Windows/Linux 原生、固定上游宿主、设备实机、正式签名/发行等外部门禁。

后续若 GUI 选文件安装取得新证据，应追加独立记录，保留本次失败和未完成状态的时间线。

## 原生权限拒绝与样例更新收尾

[真实 NSAlert 拒绝证据](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/native-alert-rejection.json)：对隔离 helper 安装仅尝试读取自建测试文件的 audit_load.py；没有权限测试替换或自动授权。加载 worker 已不在 sample 中，主线程仍处于 Runtime.load → 原生 NSAlert。提示保持 23.943 秒，直到实际点击 No；随后六个正常包继续 loaded，管理器正常退出、解释器停止。此测试覆盖异步审计失败后 worker 已结束、原生框仍应保持的边界；不冒充所有系统提示或强制关闭回归。

[生产安装 API 更新页面](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/final-page-upgrade.json)：使用真实 Manager 安装和 Runtime 加载，将最终开发包独立 profile 的页面更新至 1.0.1；退出后 enabled/capability 状态保持，解释器停止。专用探针另通过占用 OS 锁时拒绝且 profile 字节不变的控制；不手动删除遗留锁文件。此为原生安装 API，不能算 GUI 文件选择器安装通过。

最终普通 App 中新页面已实际显示完整深色视口、清晰标题并返回 `Python replied: received 42`，见[样式与往返截图](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/final-page-replied.png)。原用户 homefix profile 未修改，失败基线与失败候选保留；最终 profile 六包预装，只有页面样例更新为 1.0.1。普通 App 正常关闭后再启动的验收摘要见 [最终验收](/Users/leo/Documents/ChatGPT/技术/SnapmakerOrca插件一期实施/release_2.4.1/page-hang-01/final-acceptance.json)。

新体验 ZIP：`/Users/leo/CodeProject/Orca_Dev/SnapmakerOrca插件体验包-加载修复版.zip`，SHA256 `55f89532f39b514811e497ec9ca9d530cdbb654915d8d9519f05210dc3bf5eb4`。旧 ZIP 保持原字节，另有备份；其余五包与旧 ZIP 完全相同。使用新宿主才能修复队列死锁，单独更新页面样式不能解决它。
