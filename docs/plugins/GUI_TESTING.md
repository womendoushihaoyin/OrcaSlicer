# 本地插件 GUI 验证

使用 `scripts/prepare_local_plugin_gui_smoke.py` 创建独立开发 App 和 `data_dir`，由 `cua-driver launch_app` 启动。测试 App 必须匹配生成的 bundle ID、PID 和隔离配置目录；不能复用正在运行的用户应用。正常验证仍需保留真实窗口、交互、结果报告及退出证据，最后运行 `scripts/verify_local_plugin_gui_smoke.py`。

macOS 建议把 `--output-dir` 放在普通开发目录（本机为 `/Users/leo/CodeProject/Orca_Dev/plugin-smoke/`）。每次生成的新 bundle ID 都是新的应用身份；放在 Documents 中可能在 `GUI_App` 构造期间打开配置文件时等待系统文件访问授权，尚未进入 wx 初始化。不要将这种堆栈误认为下面的 Cocoa 事件循环问题，也不要修改 TCC。`/private/tmp` 中的 App 在本机不能由 LaunchServices 按 bundle ID 找到，同样不适合作为启动目录。测试结束后可把报告复制到归档目录。

## macOS Tahoe：启动进程存在但没有窗口

2026-10-22 的 workflow 测试在 macOS 26.4.1 / arm64 出现此问题：`launch_app` 返回进程，但窗口列表为空、没有 `.startup` 报告。主线程采样停在：

```text
wxEntry → wxApp::CallOnInit → [NSApplication run] → nextEventMatchingMask
```

此时应用自己的 `GUI_App::OnInit()` 尚未执行。此前已通过的 slicing GUI 测试也曾采样到相同堆栈，不能据此认定插件业务代码死锁。

wxWidgets 官方已确认这一 macOS 启动问题：[issue #26095](https://github.com/wxWidgets/wxWidgets/issues/26095)。其修复是在 `applicationDidFinishLaunching` 的 `stop` 周围再投递一个 dummy `NSEvent`，使启动循环得以退出：[修复提交](https://github.com/wxWidgets/wxWidgets/commit/2f89cd2)。当前开发构建的 wx 依赖尚无该修复。

本机实际依赖源码根目录（可由 `build-app/CMakeCache.txt` 的 `wxWidgets_CONFIG_EXECUTABLE` 复核）：

```text
/Users/leo/CodeProject/Orca_Dev/Snapmaker_Orca/OrcaSlicer/deps/build/arm64/dep_wxWidgets-prefix/src/dep_wxWidgets
```

- `src/osx/cocoa/utils.mm:70`：`applicationDidFinishLaunching` 调用 `stop`；`:374`：`CallOnInit` 在应用 `OnInit` 前运行 Cocoa 循环。
- `src/osx/carbon/app.cpp:432`：`wxApp::WakeUpIdle` 仅在 `wxEventLoopBase::GetActive()` 非空时唤醒。上述启动阶段直接运行 Cocoa 循环，因此不能把 `wxWakeUpIdle()` 当作可靠修复。
- `src/osx/cocoa/evtloop.mm:350`、`:365`：正式事件循环的停止和唤醒实现说明仅 `stop` 不足，并使用 dummy `NSEvent`。

## 针对已确认卡在启动阶段的测试 PID 唤醒

仅当 bundle/PID 已确认属于本次隔离测试、没有窗口，并且采样符合上述堆栈时，才可使用 `cua-driver` 文档支持的按 PID 键盘事件。将下面的 `TEST_PID` 替换为刚核实的测试 PID：

```text
cua-driver press_key '{"pid":TEST_PID,"key":"escape"}'
```

不传 `window_id` 或 `element_index`，不改变前台应用、不移动用户鼠标，也不使用 `activate`、Dock 点击或 `open`。已有窗口、已有模态对话框或正常运行的用户应用不适用这一步，Escape 可能改变其操作状态。发送一次后重新检查窗口和启动报告；仍无进展时重新诊断，不反复发送按键。

本次历史 PID 71300 在该事件后进入真实 Setup Wizard；此前按原 bundle 重开和发送文件 URL 均未使启动循环退出。没有修改 App 启动代码、wx vendor、TCC 或前台状态。历史 PID 只作证据，不能直接复用。

**这一步只证明启动阻塞已解除，不是插件功能 PASS。** 向导、真实 Plater 工作流、纹理预览/Apply、Undo、3MF 重开、切片结果和正常退出仍须分别完成并保留证据。

## 投影框人工交互验收

2026-10-23 用户确认：留到交接时手工验收。由渲染引擎牵头、几何网格协助，按下列步骤记录结果；此项保持待验，不阻塞本轮本地开发版交付。无需继续用后台自动化反复尝试原生标题栏交互。

当前原生矩阵、裁切和深度测试通过，后台自动化尚未确认原生窗口标题栏交互。请在新隔离 profile 的开发 App 中手工执行以下步骤并保留结果；旧 actual18/19 失败报告不改写为通过。

1. 安装 ResourceTexture 示例，导入可切片模型，在纹理编辑器添加一层图像。
2. 投影方式选 `Project from view`，点击 `Capture view`、`Projection frame`；确认浮窗显示在模型上。
3. 用鼠标拖动原生标题栏，分别从边缘和角部缩放；记录实际前后位置与尺寸。改变透明度，确认纹理图像随框重绘。
4. 点浮窗原生关闭按钮，确认只是隐藏；再次点 `Projection frame`，确认位置尺寸保留，原编辑器仍能操作。
5. 将框覆盖模型局部后点 `Apply frame`，确认浮框隐藏。预览应对应框覆盖范围；执行编辑器 Undo/Redo，检查投影与遮罩共同恢复。
6. 将框完全移到模型外再 Apply，确认错误可见且上一份有效投影/遮罩没有被半更新。最后取消编辑，原工程保持不变。
7. 另开一次编辑，完成有效 Apply → Preview → Bake → 主工程 Undo/Redo → 保存 3MF → 重开 → 切片，正常退出并记录产物。

报告注明 OS/架构、二进制 SHA、隔离 profile、各步结果和失败现象。未执行步骤记为 pending。原生窗口行为需要在 Windows/Linux 环境到位后分别补验；macOS 通过也不能替代另外两端。
