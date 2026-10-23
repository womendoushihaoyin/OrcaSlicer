# 本地 G-code 插件

安装 [local_gcode.py](examples/local_gcode.py)，在 Local Plugins → Slicing plugins 选中 `Local G-code footer`，再切片并导出。这个能力与几何切片使用同一 `SlicingPipelineCapabilityBase`，通过 `ctx.step` 判断执行阶段；固定上游没有另一套 `post_process_plugin` 选项。

## 执行合同

传统 `post_process` 脚本 → 读取脚本的 `.output_name` → 按预设顺序执行插件 → 成功交付工作文件。

- `ctx.step == psGCodePostProcess`；`ctx.print`、`ctx.object` 为 `None`。
- `ctx.gcode_path` 是可原地编辑的工作文件；`ctx.host` 是目标类型；`ctx.output_name` 是最终目标名，包含传统脚本的改名结果。
- `ctx.config_value(key)` 返回序列化字符串，缺键返回 `None`；不是自动转换的 Python 数字。
- 读取目标名使用 `ctx.output_name`。嵌入解释器的 `os.environ` 在导入时缓存，不能据此假定会同步宿主后来的 `setenv`。
- `Success`、`Skipped` 继续后续能力；`RecoverableError`、`FatalError` 和异常都使本次导出失败。这与几何阶段的 RecoverableError 处理不同。
- 插件必须保留 `gcode_path` 对应文件。插件返回后检查文件仍存在；不要自行改名。此版本与固定上游一致，只有传统脚本的 `.output_name` 改名被消费。
- 整组能力、有效配置在传统脚本开始前解析并锁定；活跃任务期间不能卸载、删除、禁用或替换所用包。后台回调拒绝同步 UI。

## Snapmaker 接入与回滚

公共入口仍为 `libslic3r/GCode/PostProcessor.cpp::run_post_process_scripts`。非 BBL 文件导出用副本，预览继续读取原始生成文件；非 BBL 上传及 BBL 兼容路径要求输入路径保持不变。插件模式下这些路径也先处理副本，成功后替换输入；失败时不发布副本，也不修改传入的目标名。兼容 BBL 调用把源路径和目标名传成同一个字符串的情况。

此事务覆盖宿主工作文件与参数，不能撤销插件主动修改其他文件的副作用，也不代表断电耐久性承诺。外部脚本单独使用时保留既有工作方式。

## 生命周期

`SliceStarted`、`SliceGeometryFinished` 来自实际 `Print::process`；`GCodeExportStarted`、`GCodeExportFinished` 来自原生生成或读取已有 G-code。这些 G-code 事件发生在插件后处理之前。最终 `SlicingJobComplete` 由 GUI 接收到工作线程结果后派发，失败为 Error，取消为 Warn。不能把几何完成或原生 G-code 完成当作上传完成。

## 验证范围

`tests/plugin_core/gcode_plugin.cpp` 覆盖脚本与插件顺序、脚本改名、重复导出、上传路径合同、BBL 参数别名、两类错误/异常/删文件回滚、缺失运行时、活跃引用与关闭等待。另用真实立方体切片、原生 G-code 生成与连续两次后处理验证完整文件内容。生命周期测试覆盖缓存不重复事件、文件打开失败、取消、异常观察者隔离和同步 UI 拒绝。

这些是 macOS arm64 原生集成证据，尚不等于完整 Plater 交互、真实设备上传或三端发行验收；最新记录见 [VERIFICATION](VERIFICATION.md)。
