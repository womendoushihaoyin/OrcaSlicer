# 设备联机差异与一期范围调整

日期：2026-10-22。Snapmaker 基线 `release_2.4.1 @ 1bff8daec7dc9daaf3addec9b4c234e8b74f474c`；Orca 参照 `14a1aa333b6da3ecadf853a4a6285b0f193b7605`。

## 决定

用户明确允许：先检查设备联机差异，不能直接适配时先跳过。源码检查表明这部分需要独立的宿主适配，现将 L23–L25 暂缓，L22 保留为已完成初步源码评估、尚未完成合同与 Adapter。设备插件的注册、设备选择、控制和实际打印不计入当前一期交付门禁，也不记为完成；后续可单独恢复。

保留现有 Snapmaker 设备工作流。继续交付本地插件框架、普通 Pages/UI、模型/预设接口、切片/G-code 和纹理链。`host.selected_printer` 中依赖统一 Agent 的部分随设备适配暂缓，其他 Host 接口不随之延期。仅让 Python 类可以导入不构成设备插件已接入。

## 源码证据

下列 Snapmaker 链接指向当前工作区的 release 源码；Orca 链接固定到参照提交，不代表最新主分支。

| 方面 | Orca | Snapmaker release_2.4.1 | 迁移含义 |
| --- | --- | --- | --- |
| 宿主入口 | `PrinterAgentPluginCapability` 直接继承 `IPrinterAgent`；`NetworkAgentFactory` 注册 Python capability，并以 provider ID 缓存 live agent | `PrintHost::get_print_host()` 按 `host_type` 构造对象，默认 `htMoonRaker_mqtt` → `Moonraker_Mqtt` | 不能仅复制 Python trampoline；设备选择、调用端和释放路径也需接工厂 |
| 连接状态所有权 | Agent 工厂缓存实例；Moonraker Agent 持有 selected_machine、device_info、连接 generation 和线程 | `Moonraker_Mqtt` 持有静态 MQTT/MQTTS 客户端、请求回调表、状态/通知回调表和设备 SN | 插件实例生命周期与现有共享会话不同；卸载插件不能顺手释放原设备会话 |
| 传输与身份 | 上游 `SnapmakerPrinterAgent` 继承 `MoonrakerPrinterAgent`，主要增加耗材查询；父类声明 HTTP URL/API key、WebSocket 状态流与 JSON-RPC 方法 | `Moonraker_Mqtt` 有 MQTT 身份协商、MQTTS CA/客户端证书、request/response/status/notification topic 与超时回调 | 上游存在同名 Snapmaker Agent 不等于覆盖本产品协议；认证、错误及重连需要逐项映射 |
| 页面消息 | Agent 对外提供连接、状态、消息及主线程投递回调 | `SSWCP_Instance::process()` 分发 `sw_test_mqtt_moonraker`，另有 WebView 订阅/取消、文件流和设备交互；GUI_App 持有页面弱引用订阅 | 原 Flutter 设备页面不是普通插件 Pages；需要独立 UI 适配，不能将 SSWCP 特权命令直接开放给插件 HTML |
| 打印任务 | `start_print`、`start_local_print`、`start_send_gcode_to_sdcard` 使用 PrintParams 和进度/取消回调 | PrintHost 上传队列与 `Moonraker_Mqtt` 的 start/pause/resume/cancel 异步接口 | 任务 ID、取消、错误、上传后启动及状态回传需要完整合同；只映射函数名称会漏掉真实业务流程 |

Snapmaker 入口：

- [PrintHost.cpp](../../src/slic3r/Utils/PrintHost.cpp)：`get_print_host`、PrintHostJobQueue。
- [MoonRaker.hpp](../../src/slic3r/Utils/MoonRaker.hpp) / [MoonRaker.cpp](../../src/slic3r/Utils/MoonRaker.cpp)：`Moonraker_Mqtt`、静态客户端、connect/disconnect、异步打印与订阅。
- [MQTT.hpp](../../src/slic3r/Utils/MQTT.hpp)：Paho 回调、重连与客户端所有权。
- [SSWCP.cpp](../../src/slic3r/GUI/SSWCP.cpp) / [SSWCP.hpp](../../src/slic3r/GUI/SSWCP.hpp)：消息分发、页面订阅与设备桥。
- [GUI_App.hpp](../../src/slic3r/GUI/GUI_App.hpp)：页面订阅的弱引用和 WebView 登记。

Orca 入口：

- [IPrinterAgent.hpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/Utils/IPrinterAgent.hpp)
- [NetworkAgentFactory.cpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/Utils/NetworkAgentFactory.cpp)
- [PrinterAgentPluginCapability.hpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/plugin/pluginTypes/printerAgent/PrinterAgentPluginCapability.hpp)
- [SnapmakerPrinterAgent.hpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/Utils/SnapmakerPrinterAgent.hpp) / [实现](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/Utils/SnapmakerPrinterAgent.cpp)
- [MoonrakerPrinterAgent.hpp](https://github.com/OrcaSlicer/OrcaSlicer/blob/14a1aa333b6da3ecadf853a4a6285b0f193b7605/src/slic3r/Utils/MoonrakerPrinterAgent.hpp)

## 恢复实施时的路径

1. 平台工程与打印工程确认 provider ID、physical device ID、连接所有者、凭据来源、任务 ID、回调线程与取消语义。当前共享静态客户端不能据此宣称支持多活动设备。
2. 平台工程先加独立的插件 Agent 注册/选择层，保留原 PrintHost 路由；原生 Snapmaker 设备继续由现有链路拥有。必要时做窄 Adapter，而不是替换协议实现。
3. 打印工程将连接、状态、上传、开始/暂停/恢复/取消逐项映射，用模拟端注入断网、超时、取消和晚回调；协议不支持的操作明确报告不支持。
4. 平台工程接设备 UI 与订阅释放。普通 Pages 仍走独立插件消息桥；SSWCP 不作为默认插件 API。
5. 验证切换设备、卸载插件、退出应用后无晚到回调；最后使用目标真机完成一次上传—打印—取消闭环，再恢复设备交付门禁。

## 验证边界

本次是固定源码的结构评估，没有连接真实打印机，也没有证明现有设备全链路已回归通过。没有为这次范围调整更改 PrintHost、MoonRaker、MQTT 或 SSWCP 协议代码。此前公共 Http/应用退出修改仍须纳入原设备回归，不能因设备插件暂缓而豁免。
