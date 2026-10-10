# 验证记录

日期：2026-10-10。基线：bcsongor/vc6ai `f99d2f2`。

## 已完成

- Zig 0.13.0，`x86-windows-gnu`，编译 AgentCore、Win32 桌面与测试程序成功。
- `tests/native_tests.c`：118 项检查通过、0 失败。覆盖缺失/异常 API 字段、可选 usage、统计累计、无效工具参数、IPC 连续帧、超时与取消后的重用、零长/超长/NUL/截断帧。
- `tests/smoke.py`：启动实际 AgentCore 子进程，验证分段传输、Unicode、reset、缺失 API 地址、未知请求、shutdown、异常 JSON、超长帧；全部通过。新增本地 HTTP 服务验证：完整自定义路径和查询串、有/无 Bearer 认证、无服务商专用请求字段、连续两轮 `echo` 工具调用与后续追问历史；全部通过。
- `tests/stream_smoke.py`：真实 CLI 子进程连接本地 SSE 模拟服务，验证结束前已输出思考文本、碎片化 UTF-8/JSON/CRLF、工具参数拼接与 `echo` 结果、思考内容跨工具和追问回传、ExtraBody 保留核心字段、流截断/无效工具索引/HTTP 401 后不执行工具且可继续对话；全部通过。
- DeepSeek 官方真实联调：`/models` 鉴权 HTTP 200；`deepseek-flash` 思考模式下流式回复成功，约 0.5 秒出现首段思考文本；数学回答、一次 `echo XP-HARNESS-OK` 工具调用及后续追问成功。密钥与联调日志仅存本地忽略文件。
- 本地窗口可启动且响应正常，Computer Use 能读取聊天记录、输入框、按钮与状态栏的原生控件树。
- Git 差异检查通过（使用 `core.whitespace=cr-at-eol`，保留上游 Windows CRLF）。

自动化测试不发送云端模型请求；本地模拟测试只执行无害 `echo`。DeepSeek 真实联调另外执行一次无害 `echo`，不读取用户文件。

## 尚未完成

- 当前环境没有 VC6/MFC 工具链和 XP 虚拟机，`build.bat` 的 VC6 编译与 XP SP3 实机运行尚未验证。MVP 本身不依赖 MFC。
- DeepSeek 已在现代 Windows 开发机联调；其他服务商与目标 XP 机器的联网兼容性尚未验证。
- Computer Use 截图返回 `CreateForMonitor: 0x80070057`，输入返回 `GetCursorPos: 0x80070005`；窗口按钮、键盘快捷键、视觉布局尚未完成交互验证。

## 产物与边界

`build/host/XP-Harness-Desktop.exe` 为 75,776 字节，`AgentCore.exe` 为 91,648 字节；不含上游 libcurl 和命令行工具。这些是现代 Windows 开发测试产物，不能作为 XP 兼容性证明。

目录提示已转为 UTF-8；上游工具命令、stdin 和输出仍沿用原编码，MVP 文件工具仅支持 ASCII 路径与文本命令。工具输出捕获最多 4 MiB，超限结束命令。同步 stdin 写入长时间阻塞可用 Stop 结束本窗口的 Core；关闭/停止会丢失内存会话。对话窗口暂显示原始 Markdown 文本。

下一步验收：在 VC6 SP6 环境运行 `build.bat`，将 `dist` 整目录放入 XP SP3，填写模型配置，验证中文对话、一次无害工具调用、停止和退出清理。
