# 本地预设包

插件的 `orca.host.preset.bundles.list_ids()` 与 `reload_local(bundle_id)` 使用当前用户的
`user/<preset_folder>/_local/<bundle_id>`。空的 `preset_folder` 对应 `default`。
列表动态读取磁盘；重载在 UI 线程执行并在成功后刷新预设界面。失败通过 Python 异常报告。

目录示例：

```text
_local/my-local-pack/
  bundle_metadata.json
  process/base/Parent.json
  process/Child.json
  filament/Material.json
  machine/Printer.json
```

`bundle_metadata.json` 必须为 JSON 对象，`id` 必须与目录名一致。例如：

```json
{"id":"my-local-pack","name":"My local pack","version":"1.0","author":"Example"}
```

预设文件沿用宿主的 JSON 格式，须有合法的版本号，`type` 与所在目录一致。名称来自 `name`，
缺失时使用文件名。包 ID 和名称均为单一路径分量，不能为 `.` 或 `..`，不能包含路径分隔符或冒号。
目录和 JSON 不能是软链。解析仅仅读取来源文件，失败不会删除 JSON 或 `.info`。

包内预设注册为 `_local/<bundle_id>/<name>`。包内 `inherits`、`compatible_printers`、
`compatible_prints` 可使用裸名称，加载时解析成完整名称；外部父预设使用宿主中的完整名称。
加载先解析整包，再解析继承关系，支持子预设在父预设之前出现；缺失父级、继承环、重复名称
或不匹配的配置分组会使整个重载失败。启动加载在普通用户预设之前进行，用户副本可继承包预设。

`Preset.bundle_id` 反映实际来源，`Preset.is_from_bundle()` 返回是否来自包。普通保存、写同步
信息与删除入口不会修改包来源文件；原名保存或强制克隆覆盖也被拒绝。另存为普通用户预设时
清除 `bundle_id`，保留对来源预设的继承；选择脱离时同时清除继承。克隆产生独立的用户预设。

重载会保留仍存在的选中预设及其已修改选项。删除或重命名正在编辑的预设会失败；若包外预设
通过继承或兼容性列表依赖这个包，重载也会拒绝，避免留下未重新解析的外部依赖。相同的排序
布局下现有 `Preset` 对象地址保持有效；增加、删除或重排预设仍遵循宿主原有的集合索引语义。
重载不改变耗材槽数量、项目颜色、冲刷矩阵或混色定义。与 `_subscribed` 下已有 ID 冲突的包
不会列为可重载；本实现不提供云端订阅下载、更新或账户状态。

回归目标 `orca_plugin_preset_bundle_test` 接受一个**尚不存在**的临时 profile 路径，不启动 GUI：

```sh
cmake --build /absolute/build --target orca_plugin_preset_bundle_test
/absolute/build/src/slic3r/orca_plugin_preset_bundle_test /absolute/new-test-profile
```

测试覆盖 Unicode 名称、动态列表、路径限制、继承与兼容性映射、编辑保留、失败回滚、外部依赖
拒绝、启动加载，以及真实 save/save_info/delete/Save As/clone API 对原包逐字节保护和副本重启恢复。
