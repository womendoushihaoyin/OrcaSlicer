# Host API 对照

对照固定 Orca commit `14a1aa333b6da3ecadf853a4a6285b0f193b7605`，范围为
`src/slic3r/plugin/host/PluginHost*.cpp` 中注册的 `orca.host` 公开类型、枚举、方法、属性与子模块。
源码差异逐文件审查；生产 SDK 已从当前真实绑定导出为 10 个模块，并独立验证成员清单和文件 hashes。
本表不把返回字典的键混算为 Python 符号。

| 分组 | 当前结果 |
| --- | --- |
| geometry、mesh | 绑定源文件与固定上游相同 |
| slicing | 公开符号齐全；`ExtrusionPath.points()` 返回只读、拥有数据的 Nx3 快照，防止持有数组超过宿主对象生命周期 |
| presets | 公开符号齐全；本地 bundle 加载、来源与重载已由真实实现支撑，预设包 native 回归通过 |
| ui、plugin.storage | 公开符号齐全；本地实现额外检查回调归属、停止状态及线程边界 |
| model | 下列 9 项精确接缝符号及真实模型/切片/3MF 闭包已合入；生产专项通过，最新 host-ci-native-09 再验证 |
| app | `selected_printer()` 缺失，设备代理按本轮范围暂缓；其他函数均存在 |

本轮恢复的精确接缝符号为：

- `orca.host.ModelVolumeType.PreciseSeamCenter`
- `orca.host.ModelVolumeType.PreciseSeamLeft`
- `orca.host.ModelVolumeType.PreciseSeamRight`
- `orca.host.ModelVolumeType.PreciseSeamEnforced`
- `orca.host.ModelVolumeType.PreciseSeamBlocked`
- `orca.host.ModelVolumeType.PreciseSeamNeutral`
- `orca.host.ModelVolume.is_precise_seam()`
- `orca.host.ModelVolume.is_precise_seam_strong()`
- `orca.host.ModelVolume.is_precise_seam_weak()`

来源：[固定上游 Model 绑定](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/plugin/host/PluginHostModel.cpp#L34)。

`app_info()` 提供固定上游的 10 个字典键：version、build、mode、language、app_config_version、stealth_mode、is_signed_in、is_bambu_signed_in、network_plugin_version、network_plugin_error，仍只允许 UI 线程调用。
`is_signed_in` 映射为宿主已有的 Snapmaker 账户：`GUI_App::gateway_account().is_login`，来源是 snapmaker_connection 的 `GET /api/account` 与 `notify.account.changed`。这是最近接收的账户快照，尚未收到已登录快照时为 false；网关断线或一次刷新失败时沿用宿主已有的保留快照行为，不把连接状态误当登录状态。它不是二期插件云商店、购买、订阅或 entitlement 判断，也不导出 token、用户标识或资料。
`is_bambu_signed_in` 独立读取旧 `GUI_App::is_user_login()` / Bambu NetworkAgent。两布尔值可独立变化，不能互相代替。
`network_plugin_version` 只在宿主持有实际 Bambu 模块时读取其版本函数；模块不存在、缺版本符号或 debug/release 不匹配时为空，不把旧调用者的 `00.00.00.00` 占位返回值导出为版本。`network_plugin_error` 保存真实 OS 加载/卸载诊断以及实际版本拒绝原因；失败后的卸载清理保留诊断，下一次成功模块初始化清理旧尝试，源模块成功重试只清理自己的失败。空串表示没有观测到上述失败，不表示硬件、云服务或全部设备业务已验证。
生命周期与诊断使用同一递归锁，并不宣称其他 NetworkAgent 业务调用线程安全，也没有改变现有连接、重试、兼容判定或旧调用返回值。`app_config_version` 采用真实原配置版本，缺失或无效时为空。
来源：[固定上游 App 绑定](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/plugin/host/PluginHostApp.cpp#L102)。

## 精确接缝的实现边界

这 9 项属于固定上游的公开 Model API。实现闭包已合入源码，宿主需要从实际项目恢复
辅助体，正确保存它们，并让强/弱接缝控制作用到导出的路径。最小非 GUI 实现闭包如下：

| 文件 | 必需行为 |
| --- | --- |
| Model.hpp/.cpp | 六种类型、强弱谓词、字符串往返、非打印耗材统计、组内优先顺序保持 |
| Print.hpp、PrintObjectSlice.cpp | 按本分支正常模型切片的坐标系切辅助体，保留外轮廓及孔 |
| GCode/PreciseSeam.cpp/.hpp/Internal.hpp | 周界与辅助体交段的真实几何处理；强模式指定点，弱模式修改候选区 |
| GCode/SeamPlacer.hpp/.cpp | 每层缓存、强点优先及对齐后恢复、弱区优先级、全包围/无效果/失败诊断 |
| PrintApply.cpp | 辅助体增删、移动、变型、重排同步与 G-code 缓存失效 |
| Format/bbs_3mf.cpp、Format/3mf.cpp | 两种 3MF 实现的类型元数据及休眠配置往返；旧软件使用普通 modifier 安全降级 |
| GCode.cpp、PrintBase.hpp | 在真实导出步骤汇总并呈现诊断 |
| CMakeLists.txt、PluginHostModel.cpp | 编译新算法并绑定真实类型与谓词 |

这条闭包已从通过基线 hash 和补丁检查的 artifact staging 合入共享源码；
`orca_plugin_precise_seam_test` 已在生产构建实跑通过，最新 host-ci-native-09 再验证。
本分支的 `trafo_centered()`、原有 libigl 接口、二维 `ExtrusionPath::polyline` 与 KD 查询接口须保留，
不能整文件覆盖成上游的新基础设施。GUI 中新增/编辑精确接缝辅助体的菜单、图标与交互由内核代理并行集成；
不应由本表的非 GUI 实现状态推断 GUI 功能已经完成。

验收必须包含：六种类型的 Python 识别、BBS/Prusa 3MF 真实保存与导入、休眠配置和未知模式安全处理、
强模式在多种 seam_position 下改变实际 G-code 外墙起点、弱区 Enforced/Blocked/Neutral 与优先顺序、
辅助体变动后的 `Print::apply()` 失效，以及无辅助体模型的原行为保持。

## 生命周期与句柄行为复核

正常 GUI 中的 `load` 完成早于 wx 观察者派发。为避免把异步延迟变成状态丢失，已将 sidecar 持久化移到激活/显式卸载完成路径。私有 runtime/plugin/capability 代次使旧队列在关闭、重启或卸载重载后失效。窗口卸载只清理当时的资源 ID；页面刷新及每次HTML/消息访问均校验当前 capability 实例，旧WebView迟到消息已在真实应用先复现后验证修复，Dock 回调支持可重入销毁。

`Preset` 的原始借用指针在 collection 排序/删除/不同布局重载后不安全。当前绑定保留公开类名与实时读取语义，使用集合加逻辑来源身份重新解析；配置更新可见，已删除或来源替换时报 `RuntimeError`，弱缓存保留有效别名的 Python identity。逻辑身份含名称、文件、bundle、cloud setting id 与来源标记；内核未新增世代字段，因此两次读取之间完全相同逻辑身份的删除再创建无法区分于同身份更新。

新增 `orca_plugin_observer_queue_test` 使用真实 wxAppConsole 事件队列、已安装 ResourceTexture 包、sidecar 和 consumer registry；`orca_plugin_preset_view_test` 使用真实删除与 bundle 重载 API。独立 C++ 语法与 Python 分发/SDK 回归已通过；统一构建后 observer native 5/5、preset view native 3/3 通过。Dock GUI 行为结果仍须以隔离应用执行记录为准。

`ModelObject`、`ModelVolume`、`ModelInstance` 继续遵循固定上游的借用生命周期合同：模型编辑、删除或替换节点后，不得继续使用先前取得的节点句柄；重新向宿主查询。`TriangleMesh` 是 shared_ptr 持有的不可变几何快照，矩阵与 `ExtrusionPath.points()` 是副本。Slicing graph 与可写 Polygon/Surface 视图仅在本次 `execute(ctx)` 内有效，容器替换、清空或扩容会使其旧引用失效。此轮未把这些既有合同改写为跨编辑安全保证。
