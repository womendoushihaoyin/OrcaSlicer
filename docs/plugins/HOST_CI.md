# 本地插件完整宿主 CI 接入

`scripts/test_plugin_host_ci.py` 是独立执行入口，接收同一 checkout 的两个已配置构建目录。
默认编译 GUI 宿主的 `ORCA_PYTHON_PLUGINS=ON/OFF` 两种配置，并运行真实原生测试与 SDK。
它不调用 install/package/release 目标，不启动 GUI，不签名、不上传符号、不发布产物，也不
触发远端 workflow。所有测试使用新证据目录下的隔离 profile；脚本拒绝覆盖已有结果目录。

## 现有 CI 能复用到哪一步

| 现有入口 | 已覆盖内容 | 完整宿主门禁还缺什么 |
| --- | --- | --- |
| `.github/workflows/plugin-runtime-native.yml` | Ubuntu 24.04、Windows 2022、macOS 15 的 Python/uv overlay、搬迁、原生依赖与离线 NumPy | 没有编译本项目 App、原生插件测试或 SDK；其绿灯不能代替完整宿主结果 |
| `build_all.yml` → `build_check_cache.yml` → `build_deps.yml` | 三端正式依赖缓存和发行构建入口 | `build_deps.yml` 会继续调用 `build_orca.yml` 和 `sentry_cli.yml`，不能作为无发布副作用的测试子流程直接复用 |
| `test-linux.yml`、`coverage.yml`、`sanitizer-tests.yml` | Linux 依赖 tarball、`SLIC3R_GUI=OFF` 的内核测试 | 本地插件生产桥在 GUI library 中；这些目标没有覆盖完整宿主 ON/OFF |

可复用依赖缓存的内容和系统依赖安装清单，新增手动测试 job 时应直接 restore 缓存，不调用
上述正式发行 reusable workflow。缓存未命中应明确失败或进入独立的 deps-only 构建 job，不能
静默跳过 ON/OFF。依赖缓存键必须与 checkout 的 `deps/**` 和平台一致；不要用其他系统或
CPU 的 cache 冒充目标环境。

## 三平台准备合同

三个 runner 均须准备当前仓库的 C++ 依赖、CMake 3.28 或更新、Python 3.12+ 脚本解释器、匹配宿主架构的完整 CPython 3.12.13
开发前缀、uv 0.11.16 和 NumPy 2.2.6 的目标平台 wheel。Python 前缀必须有开发头、链接库
和解压后的标准库，不能只给没有开发文件的 embeddable ZIP。可沿用 runtime workflow 的
固定输入取得步骤，但取得输入的联网阶段与本 driver 的离线测试阶段应分开。
干净 ON 配置还会通过 `cmake/OrcaPluginPython.cmake` 获取 pybind11 3.0.1。
若配置阶段也不能联网，应提前保存该文件指定、SHA256 为
`741633da746b7c738bb71f1854f957b9da660bcd2dce68d71949037f0969d0ca` 的源码归档，
校验后解压，并把源码根目录（含 `CMakeLists.txt` 和 `include/pybind11/`）传给
`FETCHCONTENT_SOURCE_DIR_ORCA_PYBIND11`。这个 CMake 本地覆盖会绕过下载及其哈希校验，
因此不能用未经校验的其他版本代替。离线 wheel 安装不意味着依赖下载或干净配置也自动离线。

| 平台 | 当前仓库的依赖前缀 | 配置和执行前提 |
| --- | --- | --- |
| Windows 2022 x64 | `deps/build/OrcaSlicer_dep/usr/local` | VS 2022 x64 developer shell；`dumpbin` 可用；两目录均使用 VS 2022 x64 或同架构 Ninja。传入现 `build_release_vs2022.bat` 使用的 `OPENSSL_ROOT_DIR`、`WIN10SDK_PATH`。driver 额外构建 launcher 与 `COPY_DLLS`，测试进程 PATH 加入指定 Python 和宿主 DLL 目录。 |
| Linux portable | `deps/build/destdir/usr/local` | 安装现 `test-linux.yml` / `build_linux.sh` 要求的 GTK、WebKit、OpenGL 等开发包；Ninja；`ldd` 可用。这里用 `SLIC3R_FHS=OFF`，不声称验证 FHS 安装。 |
| macOS native architecture | `deps/build/<arm64或x86_64>/OrcaSlicer_dep/usr/local` | Xcode clang、libtool、`otool`；Python、所有依赖和 `CMAKE_OSX_ARCHITECTURES` 必须是同一个实际 runner 架构。此 job 不做 universal 合并、签名或公证。 |

例如在已经备齐依赖的 runner 中，以下 Python 配置片段使用环境变量指向真实绝对路径，
没有 shell 字符串拼接。Windows 必须从 developer shell 启动。所有输出目录应是新目录。
`PLUGIN_PYBIND11_SOURCE` 是上述已校验源码的可选本地路径；不设置时，ON 首次配置需要联网。

```python
import os, pathlib, platform, subprocess, sys
source = pathlib.Path.cwd().resolve()
deps = pathlib.Path(os.environ["PLUGIN_DEPS_PREFIX"]).resolve(strict=True)
python_root = pathlib.Path(os.environ["PLUGIN_PYTHON_ROOT"]).resolve(strict=True)
uv = pathlib.Path(os.environ["PLUGIN_UV"]).resolve(strict=True)
pybind11 = os.environ.get("PLUGIN_PYBIND11_SOURCE")
if pybind11:
    pybind11 = pathlib.Path(pybind11).resolve(strict=True)
    assert (pybind11 / "CMakeLists.txt").is_file() and (pybind11 / "include/pybind11/pybind11.h").is_file()
common = ["-DCMAKE_BUILD_TYPE=Release", "-DSLIC3R_GUI=ON", "-DSLIC3R_STATIC=ON",
          "-DOPENVDB_USE_STATIC_LIBS=ON", "-DORCA_LOCAL_PLUGIN_SPIKE=OFF",
          "-DSLIC3R_FHS=OFF", "-DSLIC3R_SENTRY=OFF", "-DSLIC3R_PCH=OFF",
          "-DCMAKE_PREFIX_PATH=" + str(deps)]
if os.name == "nt":
    generator = ["-G", "Visual Studio 17 2022", "-A", "x64"]
    sdk_include = pathlib.Path(os.environ["WindowsSdkDir"]) / "Include" / os.environ["WindowsSDKVersion"].rstrip("/\\")
    assert sdk_include.is_dir(), "Open a VS 2022 x64 developer shell with the Windows SDK installed"
    common += ["-DOPENSSL_ROOT_DIR=" + str(deps), "-DWIN10SDK_PATH=" + str(sdk_include)]
else:
    generator = ["-G", "Ninja"]
if sys.platform == "darwin":
    common += ["-DCMAKE_OSX_ARCHITECTURES=" + platform.machine(), "-DCMAKE_MACOSX_BUNDLE=OFF"]
for mode in ("OFF", "ON"):
    build = source / ("host-" + mode.lower())
    assert not build.exists(), "Use fresh ON/OFF directories"
    extra = [] if mode == "OFF" else ["-DORCA_PLUGIN_PYTHON_ROOT=" + str(python_root),
                                      "-DORCA_PLUGIN_UV_EXECUTABLE=" + str(uv)]
    if mode == "ON" and pybind11:
        extra += ["-DFETCHCONTENT_SOURCE_DIR_ORCA_PYBIND11=" + str(pybind11)]
    subprocess.run(["cmake", "-S", str(source), "-B", str(build),
                    *generator, *common, "-DORCA_PYTHON_PLUGINS=" + mode, *extra], check=True)
```

然后执行 driver；命令行中的目录可按 runner 实际路径替换：

```sh
python3 -X utf8 scripts/test_plugin_host_ci.py \
  --on-build host-on --off-build host-off \
  --wheelhouse /absolute/target-wheels \
  --output /absolute/new-host-evidence --jobs 2 --test-jobs 1
```

Windows 可用 `python -X utf8` 启动保存后的配置片段和 driver，并将后面的参数写在同一行，
不要把 POSIX 示例的反斜杠续行照搬到 PowerShell/cmd。带空格、中文的路径应各自作为一个
参数传入；不要在 Python 的路径环境变量值中再包含字面外层引号。脚本的子进程均使用
参数列表，不经 shell 拼接；这保证参数边界，不能代替 Windows 原生执行验收。

`--preflight-only` 只检查配置合同，结果保留 `passed: false` 和 `preflight_passed: true`，不产生
构建或原生通过声明。`--no-build` 供本机复验已构建产物，结果标记 `mode: existing-artifacts`，
不能把它当作本次重新构建 ON/OFF 的证据。默认模式是 `build-and-test`。

## 实际执行的门禁

driver 校验两个缓存均来自同一源码、GUI 已开、spike 已关、ON/OFF 正确且配置相同。它编译
完整 App 目标；Windows 同时编译 GUI launcher 和复制运行依赖。随后记录所有二进制 SHA256，
用系统原生依赖查看器验证 ON 宿主引用 Python 共享库、OFF 宿主没有该依赖，并拒绝已报告的
缺失依赖。开发树中运行的路径设置不代替已打包 App 的 loader 验收。

原生阶段运行当前全部 core 场景（包括真实 uv 成功/失败安装后的宿主环境保留）、本地预设
bundle、带真实 wx 主线程身份的 preset view、precise seam、真实 wx 队列的 observer，以及
跨进程文件锁。独立 `orca_plugin_unload_overlap_test` 在九个新进程/中文 profile 中检查
单项和批量卸载的重载隔离、shutdown drain、replacement/delete 忙时拒绝及完成后重试、
同线程重入、runtime 析构后的完整 cleanup，以及持 GIL 的 shutdown；每个场景有独立日志，
任一失败即中止，报告额外记录 `lifecycle_scenarios: 9`。`orca_plugin_print_object_id_test`
以无参数独立进程执行 allocator poison 回归，不把其全局分配器替换带进其他测试。
`orca_plugin_app_info_test` 另以新进程和全新 `app-info-中文` profile 执行八项账户字段映射与
真实动态库诊断检查，阶段名为 `native-app-info`。构建目标依赖 good、no_version、debug_mismatch
三个无账户/设备/网络操作的微型共享库；driver 记录测试程序和三库的 SHA256，要求原生日志中
八项结果齐全，缺项、重复项或非零退出均不能通过，结果保存为 `app_info_checks`。
其中四种 Snapmaker/Bambu 布尔组合是纯映射合同；真正加载共享库、缺文件、坏格式、Unicode
路径、源库失败/恢复、缺版本符号、debug/release 不匹配、错误保留/清除及并发快照均走生产
NetworkAgent。它不执行真实账户登录或 GUI，也不把旧 Bambu 身份当作 Snapmaker 账户。

该目标的测试程序和 `plugin-app-info-fixtures` 严格按当前 CMake configuration 查找；VS 或
Ninja Multi-Config 使用其 `Release` 等子目录，单配置使用平铺目录。不会用旧平铺库替代缺失的
当前配置产物，也拒绝指向构建树外的库。profile 已存在（包括悬空符号链接）时明确失败，不删除
旧结果。Windows 必须保留 `orca_plugin_app_info_test.exe` 文件名：旧加载器仅为
`snapmaker-orca.exe` 启用宿主目录 DLL 回退，负向库用例不能被重命名为真实宿主程序运行。
Windows 还预检实际测试库路径能否装入旧加载器的 128 个宽字符缓冲区；过长时应选择较短的
`--output`（例如 `C:\\pci\\run01`），不会把这项现存限制伪装成库加载合同已通过或在本批修改它。
macOS 额外构建 `orca_plugin_dependency_recovery_test` 及其依赖的测试专用
`liborca_plugin_dependency_fault.dylib`，执行 `native-dependency-recovery` 阶段。它先用真实 uv
安装两个离线 wheel，在生产 `std::filesystem::rename` 成功后强制退出，再启动独立宿主进行
恢复；共九个真实进程步骤，覆盖发布前中断、旧版留下的空目录、恢复目标被文件阻塞并重试，
以及已发布的新依赖保持可导入。故障库仅注入播种进程，恢复进程不注入；它不进入应用或发行包。
测试程序和故障库必须位于当前 configuration 的同一原生目标目录，缺失时门禁失败；不会回退
到旧平铺构建。Windows/Linux 不构建或运行这一 macOS dyld/libc++ 注入目标，结果明确写入
`dependency_recovery.status: not-run`、`processes: 0` 和未验证原因，不能写成该场景通过。

该阶段不仅检查 driver 退出码，还重新检查有序九步结果、真实重命名后的退出 86、恢复成功与
解释器停止、每个原始日志/报告的 SHA256。报告记录测试程序、故障库及生产恢复源码、测试源、
fixture 生成器、CMake 和 driver 的 SHA256；运行期间源码或原生输入发生变化也会失败。
源码哈希表示本次门禁读取的源码快照；`--no-build` 不据此宣称这些源码刚刚被重新编译。
独立重跑可使用 `scripts/test_plugin_dependency_recovery.py --binary ABS --fault-library ABS --output NEW_ABS`；
正常完整门禁会自动执行，不需要手工另列命令。

三平台的目标列表还包含 `orca_plugin_wheel_cache_recovery_test`，通过
`native-wheel-cache-recovery` 阶段执行真实 Manager 导入和 Script 回调。十二个独立进程覆盖
主 wheel 与附带依赖 wheel 的晚期 CRC 错误、不安全路径、重复 entry、文件/目录冲突、旧缓存，
以及大小写路径别名与跨进程完整缓存复用。别名用例先探测实际文件系统是否区分大小写：
不区分时拒绝覆盖，区分时验证两文件保留并正常导入，不根据操作系统名称猜测。仅 `entry_restart` 明确复用先前 `entry_crc` 的
profile，其余 profile 各自独立；这与复用当前进程内的 Python 状态不同。
每项同时检查坏包重试、合法包修复、资源缺失/截断、完成标记损坏、归档更新及真实回调内容，
失败解压不得发布部分缓存，也不能破坏先前完整目录。原生结果必须包含预定的全部 phase、
正确 callback 数据和解释器停止证据，单凭退出 0 或 summary 的 `ok` 字段不能通过。

测试程序严格取当前单/多配置原生目标目录，参数使用列表传递，新证据路径含中文；已有输出
不覆盖。driver 记录程序、相关生产/测试/fixture/接线源码 SHA256，并重新检查实际 fixture
manifest 与每个文件、十二项结果、原始日志哈希和运行期间二进制/源码是否变化。独立重跑为
`scripts/test_plugin_wheel_cache_recovery.py --native ABS --output NEW_ABS`。这一 portable
目标将来在 Windows/Linux runner 上也必须真正执行；本次仅 macOS 原生实测，不能把跨平台
Python 参数合同或预置目标当成 Windows/Linux 通过证据。

SDK 阶段用实际 embedded exporter 生成中文路径的 SDK，再校验成员/类型桩
和完整性，并执行 SDK verifier 自身的契约测试。场景数量从实际 `results.json` 汇总，不能
只根据二进制 exit code 宣称存在某个未接入的场景。

结果与经过过滤的阶段日志写到新证据目录；目录不能经符号链接指向 ON/OFF 构建树或输入
wheelhouse。进程找不到/无法启动、非零退出和超时均记录失败阶段，门禁不会继续运行后续
步骤。编译命令中的定义和明显敏感字段不会写入日志。
CI 应只上传这个证据目录及需要保留的 SDK，不能上传完整 CMakeCache、compdb、未过滤构建
日志或带凭证的环境。没有触发远端执行前，不应把本地脚本测试写成三端通过。

完整发行仍须另跑 `plugin-runtime-native.yml` 的搬迁/离线轮子检查与
`probe_plugin_host_runtime.py` 的搬迁后 embedded host 检查；实际 Plater、GL 预览、交互、
Undo/Redo 和导出重开由 `GUI_TESTING.md` 的真实 GUI 验收覆盖。本 driver 的结果明确标记
`gui_interaction_tested: false`、`packaged_relocation_tested: false`。

## 本轮本机验证

`python3 -B scripts/test_plugin_host_ci_contracts.py` 已通过 32 组输入/日志及调度合同，覆盖错 checkout、
GUI 关闭、spike、错误 ON/OFF、同一构建目录、multi-config、缺失 runtime、错误产物目录及
构建定义过滤，并检查生命周期的完整逐项调度/失败中止及无参数 allocator 测试合同。
新增真实失败子进程、超时、缺失可执行文件、UTF-8/空格/字面 shell 字符参数和缺失 CMake
后的 JSON 失败记录检查；这些测试不启动 CMake 配置或真实编译。
新增 app_info 合同覆盖三平台文件名与单/多配置查找、缺失和外部库拒绝、旧配置拒绝、profile
保留、Windows 宿主名回退与路径长度拒绝，以及八项原生结果完整性。跨平台布局合同是在本机
用临时文件检查调度规则，不是 Windows/Linux 二进制执行。`native-app-info` 已纳入下述 native-10 的真实生产门禁。新增恢复门禁的私有候选证据不能替代
合并后的完整 driver 运行。
macOS arm64 的现有真实 ON/OFF 缓存 preflight 已通过；新增九个生命周期场景已用
ON26 主线生产 `orca_plugin_unload_overlap_test` 实际通过，使用正式 driver 的同一调度函数；
证据为 `release_2.4.1/lifecycle-production-01/results.json`，二进制运行前后 SHA256 一致。
这不替代完整 driver 的其余验收；场景总数始终读取当次结果，不在文档固定旧数量。

最近的完整本机检查点为 `release_2.4.1/host-ci-native-10`：macOS arm64 使用生产 ON/OFF
现有产物执行 `--no-build --test-jobs 2`，82 个 core 场景、9 个生命周期场景、8 个 app_info 检查，
预设 bundle/view、precise seam、observer、allocator poison、文件锁和 SDK 均通过，共 22 阶段。
其 `results.json` 保存模式、场景和产物哈希；该检查点在新增依赖/解压恢复门禁之前，不包含它们。
这是 `existing-artifacts` 复验，没有配置或编译应用，也未启动 GUI；不代表发行搬迁或
Windows/Linux 验收。

新增依赖恢复阶段的 9 步私有 macOS 原生证据在
`release_2.4.1/dependency-recovery-staging-01/formal-driver-01/results.json`，统一 driver 的报告
校验器已经只读核对该实际证据。`host-ci-recovery-staging-01/stage-dispatch-native-01` 又经
统一 driver 的阶段函数实际启动九个新进程并全部通过，仍使用私有已验证候选产物；它不是
合并后的完整 host CI。相关合同还覆盖
macOS/非 macOS 调度、单/多配置路径、故障库缺失/外部链接/旧配置拒绝、重复或缺失步骤、
错误退出、伪空成功、篡改日志/二进制，以及源码哈希。合同中的合成记录仅测试校验器，不是
真实宿主退出或恢复证据。

wheel 恢复接线的合同额外覆盖三平台单/多配置产物、Unicode 参数、旧配置/重复 profile 拒绝、
十二项顺序与唯一允许的跨进程 profile 复用、原始 phase/callback/停止证据、fixture/二进制/
结果篡改与缺失日志。其合成输入只验证报告校验器。私有调度首轮
`host-ci-recovery-staging-01/wheel-stage-dispatch-native-01` 保留了旧二进制对新增大小写别名场景的
真实失败；原十一项通过不能掩盖该缺陷。随后 `wheel-stage-dispatch-native-02` 使用冻结后的私有新候选，经统一 driver 阶段函数实际运行
全十二项并通过；耗时 23.092 秒，程序、driver、wrapper 与测试源的运行前后 SHA256 一致。
这是 macOS 单阶段原生结果，合并后的完整门禁仍需由主线构建完成后另跑。

当前没有 Windows/Linux 构建机或 CI，先交付脚本、参数及上述本地验证即可继续本地一期。
取得目标原生环境后，按准备合同配置两个新构建目录，再执行本 driver 默认构建模式及
发行搬迁门禁。Windows/Linux 完整宿主、GUI 和安装器均仍待实测，远端 workflow 未触发；
preflight、脚本合同或本机 macOS 结果不能填补该验收空缺。
