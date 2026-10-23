# 历史 spike 验证记录

这些命令只用于早期 `ORCA_LOCAL_PLUGIN_SPIKE`；当前入口是 [LOCAL_PHASE1.md](LOCAL_PHASE1.md)。不要使用本文件的历史限制判断当前生产核心。

## 历史 spike 说明

以下保留早期纵向验证命令，**仅适用于旧 `ORCA_LOCAL_PLUGIN_SPIKE` 构建，不代表当前生产核心的能力或验收结果**。


当前基线：Snapmaker `release_2.4.1 @ 1bff8dae`，工作分支 `codex/plugin-local-phase1-release-2.4.1`。旧 main 工作区保留作历史记录。

完整一期与团队分工见 [TODO.md](TODO.md)。当前已通过原生 Script/存储测试和 macOS arm64 真实 GUI 的安装、重启、启停及退出验证；结果见 [VERIFICATION.md](VERIFICATION.md)。这仍不构成一期交付。公开接口的来源见 [UPSTREAM.txt](UPSTREAM.txt)。

## 第一步：运行独立原生合同测试

准备 CMake、C++17 编译器，以及带头文件、动态库、标准库的 CPython **3.12.13** 独立前缀。不要依赖开发机全局 Python；运行以下命令的 Python 仅作为构建调度器。

```sh
python3 scripts/test_local_plugin_spike.py \
  --python-root /absolute/path/to/cpython-3.12.13 \
  --build-dir /absolute/path/outside/checkout/plugin-spike-build
```

默认从固定版本 URL 下载 pybind11 **3.0.1** 并校验 SHA256。离线开发时传 `--pybind-source /absolute/path/to/pybind11-3.0.1`；此显式本地覆盖目录由开发者保证版本。该构建不下载或安装插件依赖。

测试在真实 C++ 进程嵌入 CPython，导出 `orca` 并调用真实 C++ trampoline；没有使用 fake `orca` 模块。覆盖成功回调、Python 异常、重复能力、依赖提前拒绝、`on_load` 失败，以及安装事务、sidecar 启停持久化、路径边界和卸载重载。跨进程测试依次启动四个 C++ 进程，验证安装 → 重启加载 → 禁用 → 重启不执行；临时目录自动清理。

## 第二步：编译带开发菜单的应用

沿用本仓库的完整应用构建参数，额外添加：

```sh
-DORCA_LOCAL_PLUGIN_SPIKE=ON
-DORCA_PLUGIN_PYTHON_ROOT=/absolute/path/to/cpython-3.12.13
```

默认 `ORCA_LOCAL_PLUGIN_SPIKE=OFF`。这是开发开关；macOS GUI 优先从 App 内 `Contents/Frameworks/python` 定位运行时，找不到时只回退显式编译前缀；Windows/Linux 发行布局仍待实现。不要将开发构建作为正式发行包。

## 第三步：在真实 GUI 中验证

使用独立测试数据目录启动新构建，避免改动日常配置。在 File 菜单选择 `Local Plugins — development`，点击 `Install .py`，选择 [native_host_hello.py](examples/native_host_hello.py) 并确认信任。选中插件后点击 `Run Script`，运行 `Native Host Hello`。可用 `Enable / Disable` 切换包状态、用 `Remove` 卸载；卸载保留 `plugin_data`。

验收应看到真实 Snapmaker 版本、模式和构建号；正常关闭应用时先调用插件 `on_unload`，再释放 Python 对象并结束解释器。独立 CTest 成功不代表此 GUI 验收已经完成。

当前限制：

- 仅支持可信、无依赖的 `.py`。安装位置为 `<data_dir>/orca_plugins/<escaped filename>/<original filename>`，包键为转义后的文件 stem，沿用上游 `.install_state.json` 字段。
- 已安装且启用的包在应用 post_init 完成、初始化模态窗口关闭后自动加载；sidecar 缺失/损坏、多入口或符号链接入口不会自动执行。
- 同包键重复安装会拒绝。手动替换、失败回滚、wheel 和中断锁恢复尚未实现。遇到残留 `.local-install.lock` 会明确报错，需要确认没有安装进程后再人工处理。
- 支持的绑定只有 Script 最小链和 `host.app_info` 的部分字段，其他能力与完整 Host SDK 未迁入。
- 所有操作在初始化线程执行。长耗时 Python 会阻塞 GUI；正式线程/任务协议属于 L08。
- 没有正式审计、权限管理或进程隔离；进程内 Python 不能可靠强杀死循环。
- 插件依赖安装、uv、离线 wheelhouse、Windows/Linux 和 macOS x64 发行验证均待实施。

## 第四步：向正式框架收敛

此小宿主用于提前暴露 CPython/pybind、对象所有权、异常和退出问题。后续按上游 Descriptor/Manager/Loader 的责任划分迁移，保留原生合同测试，收敛或删除 `LocalPluginRuntime`；不要在它上面另造与 Orca 分叉的公共 SDK。

推进顺序是：本地安装/管理 → 完整 Host 与模型/网格 → Pages、切片/G-code、设备、纹理 → 统一生命周期 → 离线依赖/SDK → 三端发行及跨宿主验收。依赖关系、各组责任和每条完成条件以 Todo 为准。
