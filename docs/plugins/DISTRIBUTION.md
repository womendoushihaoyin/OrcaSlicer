# 本地插件 runtime、离线依赖与 SDK

基线：Snapmaker Orca `release_2.4.1`，上游 Orca
`14a1aa333b6da3ecadf853a4a6285b0f193b7605`。CPython 固定为 **3.12.13**，
pybind11 为 **3.0.1**；打包工具默认固定 uv **0.11.16**。

这些工具只生成新的 artifact 目录，不修改已安装 App、真实用户 profile、系统 Python
或 `deps/`。runtime 产物是供发行流水线合并的 **overlay**；即使目录名以 `.app`
结尾，它也不是完整 App，不包含 Info.plist、资源、正式签名或公证票据。

## 平台目录合同

以下路径相对 overlay 根目录，与 `PythonInterpreter::find_bundled_python_home()`、
`bundled_uv_path()` 和 `CLI::setup()` 的资源定位吻合：

| 平台 | 宿主可执行文件目录 | Python 前缀 | uv | 资源目录 |
| --- | --- | --- | --- | --- |
| macOS | `Contents/MacOS` | `Contents/Frameworks/python` | `Contents/MacOS/tools/uv/uv` | `Contents/Resources` |
| Windows | 根目录 | `python` | `tools/uv/uv.exe` | `resources` |
| Linux portable / AppImage 内部前缀 | `bin` | `lib/python` | `resources/tools/uv/uv` | `resources` |

Windows 另将 Python 前缀根部的 DLL 复制到宿主可执行文件旁，满足初始化前的 PE loader
依赖。Python 前缀仍保留 DLL、`Lib/encodings` 及扩展模块。不能直接使用只有 zip 标准库、
没有 `Lib/encodings` 的 embeddable ZIP。

宿主的私有 `PluginRuntimePaths.hpp` 统一运行时选择：先应用相对目录，最后才使用显式
编译配置的开发前缀。Linux 从 `<prefix>/bin/host` 查找 `<prefix>/lib/python`，即使
SDK 等命令行工具尚未初始化资源目录也能定位；uv 同时支持
`<prefix>/resources/tools/uv/uv` 和真实资源目录下的 `tools/uv/uv`。空路径和相对路径
不会变成当前工作目录的搜索。Windows 目录还必须有解压后的 encodings 与 Python DLL。

SDK exporter 入口同应用一样初始化 Boost.Nowide 的 UTF-8 文件系统转换，并将 Windows
原生命令行转换为 UTF-8；脚本、输出和隔离 profile 参数不会再把系统 ANSI 编码误当作
UTF-8。依赖安装器在 Windows 显式把 UTF-8 可执行文件及参数转换为宽字符串，令
Boost.Process 使用 `CreateProcessW`。POSIX 保持原始 UTF-8 路径字节。

Linux FHS 安装不在本 overlay 合同内；其 `SLIC3R_FHS_RESOURCES` 是编译期资源路径。
宿主支持可执行文件前缀下的 `lib/python` 和 FHS 资源下的 uv，发行流水线仍需实际安装
这些目录并检查 ELF 依赖。本工具不把 portable 验收当作 FHS 验收。

`ORCA_PYTHON_PLUGINS=ON` 的 `LocalPluginRuntimeCore` 使用上述生产查找逻辑，
不使用 `LocalPluginsDialog::python_home()` 参数。旧的 spike-only 运行时是独立试验路径，
不作为本发行工具的宿主验收对象。

## 生成与搬迁验证

使用 Python 3.12 或更新版本运行打包脚本，运行时前缀仍必须为 3.12.13。
Windows 使用 `python -X utf8`，并将路径各自作为单个参数传入；POSIX 示例的 `\` 续行
不能直接粘贴到 PowerShell/cmd：

```sh
python3 scripts/package_plugin_runtime.py stage \
  --platform macos \
  --python-root /absolute/toolchain/python \
  --uv /absolute/toolchain/uv \
  --host-binary /absolute/build/src/Snapmaker_Orca \
  --output /absolute/new-artifacts/runtime-overlay

# 将整个目录搬到另一个含空格/中文的路径后再运行；不要仅验证原路径。
python3 scripts/package_plugin_runtime.py verify \
  '/absolute/搬迁目录/runtime-overlay' --native
```

`--host-binary` 可选。脚本只复制、修复动态链接并校验这个二进制，**不启动 GUI**。
已有输出目录一律拒绝覆盖。原始输入保持不变，失败的 staging 自动清理。

要验证真正的 embedded 宿主，打包时将 `--host-binary` 指向同批构建的
`orca_plugin_sdk_export`，搬迁后运行：

```sh
python3 scripts/probe_plugin_host_runtime.py \
  --runtime '/absolute/搬迁目录/runtime-overlay' \
  --exporter '/absolute/搬迁目录/runtime-overlay/Contents/MacOS/orca_plugin_sdk_export' \
  --output /absolute/new-artifacts/embedded-host-probe
```

Windows/Linux 使用表中的宿主目录。探针只接受 SDK exporter，创建全新隔离 profile，
在不同 CWD 下运行真实 `PluginManager`/`PythonInterpreter`，检查 `orca` 绑定、包内
stdlib/前缀、受控 sys.path，并用 CWD 中的哨兵模块和污染的 `PYTHONPATH` 验证导入隔离。
它在运行前后验证 overlay 完整性，记录二进制与清单 SHA256。旧二进制未关闭 pybind11
默认 `add_program_dir_to_path` 时应失败；不能只运行独立 Python 来代替这个验收。

- macOS：将指向源 Python 前缀的 install name 改为相对 `@loader_path`，修改后的
  Mach-O 做 ad-hoc 签名以便本机执行；验证每个 Mach-O 的 load command，只接受系统库和
  overlay 内可解析的相对依赖。支持 Python 自带的相对 `LC_RPATH`。
- Linux：识别 PT_DYNAMIC，仅对动态 ELF 使用 `patchelf`（静态 uv 保留不改），将 libpython 依赖和 RPATH 改为相对路径，并用 `ldd` 拒绝缺失、开发机路径依赖及越界 RPATH。
  必须在匹配的 libc/CPU 环境进行 native 验证；脚本不会承诺跨 glibc 兼容性。
- Windows：在对应 OS/CPU 上执行 Python、uv 和 DLL 加载校验。
- `--layout-only` 仅允许准备跨平台目录，明确标记 `relocation: not-run`；它不执行
  目标二进制，不等价于 native 验收。

`plugin-runtime.json` 记录固定版本、上游 commit、uv 源文件 SHA256，以及所有文件的
SHA256/大小/执行位与软链目标。外部或悬空软链拒绝；内部绝对软链转换为相对软链。
校验拒绝修改、遗漏或额外文件。校验清单用于完整性检查，不代替可信发行签名。
`--native` 使用隔离 Python 校验版本、前缀、sys.path、stdlib、共享 libpython 的加载及 uv
版本；这些检查不读取用户 site-packages，不允许自动下载解释器。

发行流水线应先在新 staging 中准备完整应用，然后合并经过验证的 overlay，再进行正式
签名、打包和安装验证。overlay 清单描述 overlay 自身；合并后的完整 App 应生成完整发行
清单并重新签名，不要直接用 overlay 清单宣称整个 App 完整性通过。

## 离线 wheelhouse

先准备目标 OS/CPU/CPython ABI 的 wheels，包括全部传递依赖。输入只接受精确版本
`name==version`（可带 extras），不接受 URL、索引参数或未固定版本。

```sh
python3 scripts/package_plugin_wheelhouse.py \
  --runtime '/absolute/搬迁目录/runtime-overlay' \
  --wheelhouse /absolute/wheels-for-this-platform \
  --requirements /absolute/requirements.in \
  --output /absolute/new-artifacts/offline-bundle \
  --check-import numpy

python3 scripts/package_plugin_wheelhouse.py \
  --runtime '/absolute/搬迁目录/runtime-overlay' \
  --verify-package /absolute/new-artifacts/offline-bundle
```

工具生成 `requirements.lock`（含 hashes）、`wheelhouse/` 和 `wheelhouse.json`，使用
随包 uv 在 `--offline --no-index --no-cache --only-binary :all:` 下解析并安装到临时目录。
缺失传递依赖、平台不兼容和版本冲突会失败，不能由本机缓存掩盖缺包。wheel 的路径越界、
软链和直接 URL 依赖在安装前被拒绝。`--check-import` 可重复指定，用于测试实际加载原生
扩展；它会执行明确指定的模块，应该只用于受信任的发行依赖。

输出绑定到 runtime 清单 SHA256。验证会检查全部 wheel/锁文件 hashes，并再次以空缓存
离线安装，并重跑清单中指定的导入，确认模块来自临时安装目录而非系统或预装包。现有宿主的依赖设置应指向产物中的 `wheelhouse/`；产物不会自动改写真实 profile
的 `.dependency-policy.json`。锁文件用于发行审查和重现；宿主仍按插件依赖声明执行自己的
事务安装与已安装版本约束。

宿主启动 uv 时使用独立环境副本，去除 `UV_*`、`PYTHON*`、`PIP_*` 的外部配置，只影响
子进程。Windows 按大小写无关名称过滤，并直接继承宽字符环境块，避免非 ASCII 值经
ANSI 编码丢失。不能用 `auto environment = boost::this_process::environment()` 充当副本：
这个 API 返回原生环境视图，调用 `erase()` 会删掉宿主进程本身的变量。

## 从真实绑定生成 SDK

`cmake/OrcaPluginSdk.cmake` 提供 `orca_plugin_sdk_export`，与生产 App 链接同一
`libslic3r_gui`、PythonInterpreter、PythonPluginBridge 与宿主适配器。该目标仅按需构建，
不会启动 wxApp、发现插件或使用真实 profile。

```sh
cmake --build /absolute/build --target orca_plugin_sdk_export
python3 scripts/generate_plugin_sdk.py \
  --exporter /absolute/build/src/slic3r/orca_plugin_sdk_export \
  --output /absolute/new-artifacts/plugin-sdk
python3 scripts/verify_plugin_sdk.py /absolute/new-artifacts/plugin-sdk
```

生成目录包括 `typings/orca/**/__init__.pyi`、`api.json`、一期示例、README 和 SHA256 清单。
`api.json` 保留运行中实际存在的成员与原始 docstring；类型桩使用可解析的 pybind11 签名，
无法可靠表达的地方使用 `Any`。不会用上游设备 API 或源码中的未注册声明补齐接口。
只导出类型信息，不调用宿主操作、实例化宿主类或修改编辑器配置。类型桩需配置到编辑器的
分析路径；独立 Python 无法运行宿主内嵌的 `orca`。
生成时与独立验证时都会逐模块/逐类比较 `.pyi` 声明和 live 成员清单，并验证所有文件的
hashes 与 Python 语法；缺失、额外或篡改的成员会失败。真实绑定的算术与其他公开 Python 协议方法（如 Point.__add__、__repr__）也进入类型桩；不导出 __dict__ 等实现元数据。
`python3 -B scripts/test_plugin_sdk.py` 独立检查重载/嵌套成员清单、缺失或多余成员、文件篡改与
额外清单文件拒绝。测试中的简化 fixture 不作为 SDK 发布；正式 SDK 仍须由真实 exporter 生成。

## 正式构建脚本的显式插件版

三个发行入口仍默认 `ORCA_PYTHON_PLUGINS=OFF`。设置为 `ON` 时，必须同时提供
`ORCA_PLUGIN_PYTHON_ROOT`（CPython 3.12.13 的开发及运行时前缀，含 Python.h、共享库，
Windows 另需 libs/python312.lib）和 `ORCA_PLUGIN_UV_EXECUTABLE`（uv 0.11.16）。
执行打包脚本的 `python3` / `python` 需为 3.12 或更新；C++ 工具链和原有 deps 仍按项目
原有流程准备。本入口不会下载 Python、uv 或依赖 wheel。

macOS 单架构示例（在仓库根目录执行）：

```sh
export ORCA_PYTHON_PLUGINS=ON
export ORCA_PLUGIN_PYTHON_ROOT='/absolute/cpython-3.12.13-macos-aarch64-none'
export ORCA_PLUGIN_UV_EXECUTABLE='/absolute/tools/uv'
./build_release_macos.sh -s -x -a arm64
```

插件版目前明确拒绝 `-a universal`。不能将 arm64 Python、扩展或 uv 配入 universal
宿主。x86_64 需使用相应前缀和 uv，在能原生运行该目标的发行环境执行；本机未验收。
打包后 App 位于 `build/<arch>/Snapmaker_Orca/Snapmaker Orca.app`。运行时合并发生在外层
正式签名和 DMG 制作前，调整过的 Mach-O 仅作 ad-hoc 签名。

Windows（VS 2022 构建环境，路径值不要包含外层引号）：

```bat
set ORCA_PYTHON_PLUGINS=ON
set ORCA_PLUGIN_PYTHON_ROOT=C:\toolchains\cpython-3.12.13
set ORCA_PLUGIN_UV_EXECUTABLE=C:\toolchains\uv.exe
call build_release_vs2022.bat slicer
cd build
cpack --config PluginCPackConfig.cmake -G NSIS
```

部署验证 `Snapmaker_Orca.dll`、`snapmaker-orca.exe` 与 Python/DLL/PYD 的架构，并用
`dumpbin` 递归检查宿主 DLL 依赖。工具从 PATH 或 CMake 已选择的 MSVC linker 同目录查找。
`PluginCPackConfig.cmake` 继承原 NSIS 元数据，打包已验证的 `build/Snapmaker_Orca` 目录。
直接使用原 `CPackConfig.cmake` 会重新执行原安装规则，遗漏后合并的 runtime；因此插件
版必须使用新配置。`build_orca.yml` 的 Windows installer 步骤已按 ON/OFF 选择配置，
其默认流水线仍为 OFF；本轮未启用或触发正式签名/发布 workflow。

Linux（原生目标平台，另需 `patchelf`）：

```sh
export ORCA_PYTHON_PLUGINS=ON
export ORCA_PLUGIN_PYTHON_ROOT='/absolute/cpython-3.12.13-linux-prefix'
export ORCA_PLUGIN_UV_EXECUTABLE='/absolute/tools/uv'
./build_linux.sh -s -i
```

`src/dev-utils/platform/unix/build_linux_image.sh.in` 在 AppImage 封装前部署到 `package/`。
插件模式使用主二进制的独立副本，避免 patchelf 经硬链接改写 build 输出。宿主使用包内
Python 与 orca-runtime 的相对 RPATH；完整 AppImage 依赖审计以清洁环境运行且不能被
`ORCA_SKIP_APPIMAGE_AUDIT` 跳过。包装脚本会核对 CMake cache：例如 ON 构建之后未设置
ON 就单独执行 `-i`，会明确失败，避免产出缺 Python 的插件宿主。

独立实现位于 `scripts/packaging/integrate_plugin_runtime.py`：`prepare` 先在临时目录构造
并原生验证 overlay，生成带确切输入的 CMake cache 片段；`deploy` 核对实际构建 cache，
在应用目录的副本中合并、重定位、检查，再替换原应用目录。失败时保留原目录。已有同名
DLL 必须与 runtime 一致；路径软链父目录、架构不符、未链接 Python 的宿主、未打包依赖
均拒绝。再次部署不同运行时或 Windows 从 ON 切回 OFF 时，应先清理专用安装输出目录，
再运行构建，避免把旧运行时混入新包。不要指向已安装的用户 App。

`plugin-runtime.json` 和 `plugin-deployment.json` 记录签名前的 hashes 与验证结果。
正式签名会改变 Mach-O/PE 字节，须由发行流程另外记录最终安装器 checksum 和平台签名
验收；这些签名前清单不能替代正式签名、公证、干净机器启动或 GUI 验收。

## 当前验证边界

当前没有 Windows/Linux 构建机或 CI，本地一期按 macOS 实测结果推进；相应平台的原生
宿主、安装器和签名验收留给取得环境后的执行者。无需先搭建远端 CI 才能使用以下入口：

| 顺序 | 入口与必需输入 | 可交接的结果 |
| --- | --- | --- |
| 1. 脚本合同 | `python3 -B scripts/test_plugin_distribution.py`、`scripts/test_plugin_release.py`、`scripts/test_plugin_host_ci_contracts.py` | 本机目录/打包/调度合同；不产生其他平台通过声明 |
| 2. 目标原生 runtime | `scripts/test_plugin_runtime_ci.py --python-root ABS --uv ABS --wheelhouse ABS --output NEW_ABS` | 该机器对应的固定 Python/uv、搬迁、原生依赖、空缓存离线 NumPy、归档解包报告 |
| 3. 完整宿主 | `scripts/test_plugin_host_ci.py --on-build ABS --off-build ABS --wheelhouse ABS --output NEW_ABS --jobs 2 --test-jobs 1` | 同 checkout 的 GUI ON/OFF 构建、原生回归、SDK；配置方法见 [HOST_CI.md](HOST_CI.md) |
| 4. 插件版发行包 | 上节对应平台构建脚本及三个 `ORCA_*` 环境变量 | 先验证并合并 runtime，再执行原平台封装；仍须目标平台安装/启动验收 |

Python 脚本用 Python 3.12+ 执行；Windows developer shell 可将示例 `python3` 换为 `python`。
`ABS` 替换为已存在的目标平台绝对输入路径，`NEW_ABS` 必须是新的证据目录。
wheelhouse 必须包含目标 OS/CPU、CPython 3.12 对应的 `numpy==2.2.6` wheel。
HOST_CI 配置示例的 `PLUGIN_PYTHON_ROOT`/`PLUGIN_UV` 是示例输入变量；发行脚本实际读取
`ORCA_PLUGIN_PYTHON_ROOT`/`ORCA_PLUGIN_UV_EXECUTABLE`，两者不要混用。
`--preflight-only` 不编译，`--no-build` 仅复验已有产物；第一次目标平台验收应使用默认
构建模式。上述步骤不需要签名秘密；正式签名、公证及最终安装器 checksum 另记。
现有手动 runtime workflow 调用的就是第 2 步，取得 CI 后可复用；本任务不触发它。

运行 `python3 scripts/test_plugin_distribution.py` 检查目录合同、清单篡改拒绝、软链搬迁、
离线 pin 限制和 wheel 越界拒绝。实际产物验证记录应由本轮 `VERIFICATION.md` 汇总。

`tests/plugin_core/runtime_paths.cpp` 直接使用生产私有路径 helper，创建文件布局检查三平台
选择/搬迁/显式开发回退、FHS 与 CWD 排除；可用 C++17 编译器独立运行，无需完整 App 构建：

```sh
c++ -std=c++17 -Isrc tests/plugin_core/runtime_paths.cpp -o /absolute/new-artifacts/runtime-paths-test
/absolute/new-artifacts/runtime-paths-test /absolute/new-artifacts/runtime-paths-fixture
```

该测试是本机文件系统上的三布局合同测试，不等同于 Windows/Linux 宿主原生验收。

`tests/plugin_core/dependency_environment.cpp` 在真实隔离运行时中调用 uv，分别覆盖成功
安装和失败回滚。它把宿主 `UV_OFFLINE` 设为无效布尔值，确认子进程已过滤该变量，同时
验证宿主的 `UV_OFFLINE`、`PYTHONHOME`、`PIP_INDEX_URL` 全部保留；Windows 用混合大小写
变量名覆盖匹配合同。macOS arm64 的私有原生对照已复现旧代码删除宿主变量，并通过新
实现。SDK exporter 也已用中文/希腊字符的脚本、输出及 profile 路径完成真实运行和逐字节
内容核对。Windows `CreateProcessW` 与 argv 转换的源码路径已核对，但尚无 Windows
宿主执行结果；该测试不能代替相应平台的原生验收。

本机完成的验证限于 macOS arm64：runtime 搬到中文/空格路径后加载，Python 3.12.13、
uv 0.11.16、stdlib 与 libpython 检查，以及 NumPy 2.2.6 的空缓存离线解析、安装和导入。
Windows、Linux、macOS x64 的 native 测试、完整安装器合并、正式代码签名、公证及受控干净
机器验收仍须在相应发行环境完成。脚本产物和 ad-hoc 签名不表示这些门槛已完成。

`.github/workflows/plugin-runtime-native.yml` 提供手动触发的 Ubuntu 24.04、Windows 2022、
macOS 15 native matrix。它在 runner 临时目录取得固定 Python/uv 与 NumPy wheel，然后调用
`scripts/test_plugin_runtime_ci.py` 执行搬迁、stdlib/DLL、空缓存离线安装与篡改拒绝检查。
结果、标明是否完整结束的 summary.json，以及保留软链/执行位的 tar 包上传为 CI artifact。
生成 tar 后会在另一中文/空格路径解包，重新验证 runtime 并完成一次空缓存 NumPy 安装。
超时、启动失败和不符合指定原因的预期拒绝都记录为失败；不能用已通过的前缀检查冒充完成。该 workflow 未在本地任务中触发；没有
远端结果时，Windows/Linux 项目仍保持未验收。workflow 不编译 App、不发布 release，且不
使用正式签名凭证。

手动 runtime matrix 只验证发行 overlay，不编译或链接宿主，也不验证 `ORCA_PYTHON_PLUGINS=ON/OFF`、SDK、完整 App 的动态链接或 FHS 布局。ON/OFF 与完整 GUI/安装器测试必须由应用构建流水线独立执行；本地 ON/OFF 结果不能替代 Windows/Linux 结果。Linux ELF 解析和越界规则有跨平台单元覆盖，真正 patchelf/ldd 执行仍须 Linux runner。

完整宿主的独立 ON/OFF 构建、原生测试与 SDK driver 及三端接入步骤见
[`HOST_CI.md`](HOST_CI.md)。它不调用正式发布 workflow，且将已有产物复验、配置 preflight
与真正本次构建明确区分；当前不声称 Windows/Linux 宿主已经执行。

本轮分发加强后，目录/离线包合同 18 项与发行接入合同 9 项通过；后者包含真实 CPack TGZ
打包，证明已部署的 Python 文件进入产物。runtime driver 共 10 阶段，包含上述合同、搬迁、
空缓存安装、篡改拒绝和归档解包复验。另以 macOS arm64 的真实应用二进制副本执行了正式
部署入口，并验证错误 cache / 未链接 Python 的宿主会失败且保留原目录。这些结果不表示
Windows NSIS、Linux AppImage 或 macOS 正式安装器已经验收。测试不读取真实用户 profile，
不启动 GUI，不向远端触发工作流。
