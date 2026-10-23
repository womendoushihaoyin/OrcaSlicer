# 本地切片插件开发链

基线：Snapmaker `release_2.4.1 @ 1bff8dae`，插件接口源自 Orca `14a1aa333b6da3ecadf853a4a6285b0f193b7605`。当前是一期实现中的一个纵向切片；完整接口、完整内核和发行验收仍以 [TODO](TODO.md) 为准。

## 从示例开始

1. 使用 `ORCA_PYTHON_PLUGINS=ON` 的开发 App，指定独立的 `--datadir`。
2. 打开 File → Local Plugins，安装 [local_slicing.py](examples/local_slicing.py)。安装会注册能力，但不会自动影响任何切片。
3. 点击 **Slicing plugins**，选择 `Local slice inspector`。这会修改当前工艺预设；按正常预设流程保存才能持久保存该选择。
4. 导入模型并切片。插件在真实 `posSlice` 后读取层数据，将报告写入该插件的数据目录 `last_slice.json`。
5. 清空选择后，相关几何缓存失效，下一轮从原始模型重新生成。禁用/卸载包而保留选择会明确报错，需要重新启用/安装或清空选择。

开发选择器保留已有选择顺序，新加入能力按列表顺序追加。选择后进入执行顺序与配置窗口：Move up/down 调整顺序，Configure for this preset 编辑当前预设覆盖（复用插件 HTML 配置页或 JSON），Use global configuration 清除此覆盖；最后按 OK 才应用，取消不改变预设或全局配置。引用缺失会显示为预设项，不会悄悄丢失；缺失能力不能打开配置页。新增排序/覆盖界面正待本批实际 GUI 验证。

## 数据流与线程

`工艺预设 → Print::apply → 配置变化失效 → Print::process → 一轮能力解析/引用锁定 → 原生阶段完成 → Python execute(ctx) → 后续原生算法`

- `slicing_pipeline_plugin` 保存有序能力名；`plugins` 保存 `<package-key>;;<capability-name>`（本地）或 `<name>;<uuid>;<capability-name>`（云）。本期不自动安装或恢复云包。
- `print_plugin_config_overrides` 保存固定上游的能力配置数组。它与选择、manifest 都纳入本适配器的静态 PrintConfig，以确保包身份/配置变化确实使 `posSlice` 失效。上游 manifest 是动态字段，这是有意记录的适配差异。
- 开始切片前完整解析所选能力。缺包、未加载、禁用、重复选择、歧义 manifest、无效配置均中止；不采用上游通用 helper 的“记录后跳过”策略。
- 每轮开始时比较能力实例代次和有效配置签名；全局配置改变或同版本包重载，也会在下一次 `process()` 重建几何。GUI 配置通知会排队使引用该能力且未被预设覆盖的盘失效；按本构建自动后台切片开关决定是否重启。逐盘/worker原生合同通过，真实双盘工作流正待本批复验。
- 每轮锁定能力及有效配置快照，整个 `Print::process` 期间有效，包括两个回调之间。管理器拒绝卸载、替换、删除和改变活跃能力的启用状态；当前回调不会读取用户正在编辑的 GUI preset。
- 回调在切片工作线程串行执行，逐能力获取 GIL。同步 UI 请求会抛错以避免 UI 等待切片、切片又等待 UI 的死锁。
- `ctx.print` / `ctx.object` 和其下游引用只在本次 `execute` 内有效。不要存成实例变量或交给其他线程；容器替换也会使旧视图失效。
- Python 异常和 `FatalError` 使本轮失败并失效切片图，重试重建；`RecoverableError` 按固定上游切片合同记录消息后继续。G-code 后处理对两类错误都中止，见 [G-code 插件](GCODE_LOCAL.md)。

## 当前真实阶段

已连接全部13阶段：`posSlice`、`posPerimeters`、`posPrepareInfill`、`posInfill`、`posIroning`、`posSupportMaterial`、`posDetectOverhangsForLift`、`posEstimateCurledExtrusions`、`psWipeTower`、`psSkirtBrim`、`posSimplifyPath`、`posContouring`、`psGCodePostProcess`。回调受真实阶段和缓存状态控制，不保证每次 process 都触发；curled阶段已补真实DONE，Contouring在真实三维几何生成之后。

`psGCodePostProcess` 在公共后处理入口、传统脚本之后执行。`ExtrusionPath.points()` 返回拥有数据且只读的 Nx3 int64 快照；平面路径Z为0，三维路径保留实际相对Z。G-code支持真实XYZ输出、变速分段、scarf、局部裁剪和点彩路径；旧XY擦拭对contour关闭。Local-Z高度覆盖与contour的累计物理Z、体积、逐段限流和异常状态恢复已通过生产原生测试；同时启用ironing、scarf或adaptive pressure advance的组合仍显式拒绝，未声称这些扩展组合兼容。

旧2D磁盘缓存对插件/contour拒绝读写，强制从真实模型重建；普通内存暖缓存不会重复修改几何。这是已测试的兼容边界，并非磁盘缓存格式迁移完成。

## 验证入口及剩余验证

- `tests/plugin_core/slicing_plugin.cpp`：真实安装 → Python 能力 → 原生围墙消费；配置失效、清空恢复、错误回滚及并发包操作保护。
- `tests/plugin_core/slicing_stages.cpp`：原生阶段顺序、增量、共享对象、取消、整轮 drain 和无操作插件输出对照。
- `scripts/prepare_local_plugin_gui_smoke.py --mode slicing --fixture <core-fixture>/slicing.py`：真实开发选择器 → 独立原生 Print 的后台工作线程。它不是 Plater 的完整导出/上传验收。

实际通过记录见 [VERIFICATION](VERIFICATION.md)。预设包及内核3MF测试已补齐；仍需新配置界面、完整 Plater 工作流、可编辑纹理和三端 GUI 验收。当前不能把 INI 字段序列化测试记作 3MF 工程验收。
