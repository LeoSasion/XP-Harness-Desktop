# 验证记录

日期：2026-10-10。基线：bcsongor/vc6ai `f99d2f2`。

## 已完成

- Zig 0.13.0，`x86-windows-gnu`，编译 AgentCore、Win32 桌面与测试程序成功。
- `tests/native_tests.c`：118 项检查通过、0 失败。覆盖缺失/异常 API 字段、可选 usage、统计累计、无效工具参数、IPC 连续帧、超时与取消后的重用、零长/超长/NUL/截断帧。
- `tests/smoke.py`：启动实际 AgentCore 子进程，验证分段传输、Unicode、reset、无密钥错误、未知请求、shutdown、异常 JSON、超长帧；全部通过。
- 本地窗口可启动且响应正常，Computer Use 能读取聊天记录、输入框、按钮与状态栏的原生控件树。
- Git 差异检查通过（使用 `core.whitespace=cr-at-eol`，保留上游 Windows CRLF）。

上述测试不发送模型请求，也不执行工具命令。

## 尚未完成

- 当前环境没有 VC6/MFC 工具链和 XP 虚拟机，`build.bat` 的 VC6 编译与 XP SP3 实机运行尚未验证。MVP 本身不依赖 MFC。
- 没有使用 API Key，真实 OpenRouter 回复与完整工具执行回路尚未联调。
- Computer Use 截图返回 `CreateForMonitor: 0x80070057`，输入返回 `GetCursorPos: 0x80070005`；窗口按钮、键盘快捷键、视觉布局尚未完成交互验证。

## 产物与边界

`build/host/XP-Harness-Desktop.exe` 为 75,776 字节，`AgentCore.exe` 为 90,112 字节；不含上游 libcurl 和命令行工具。这些是现代 Windows 开发测试产物，不能作为 XP 兼容性证明。

目录提示已转为 UTF-8；上游工具命令、stdin 和输出仍沿用原编码，MVP 文件工具仅支持 ASCII 路径与文本命令。工具输出捕获最多 4 MiB，超限结束命令。同步 stdin 写入长时间阻塞可用 Stop 结束本窗口的 Core；关闭/停止会丢失内存会话。对话窗口暂显示原始 Markdown 文本。

下一步验收：在 VC6 SP6 环境运行 `build.bat`，将 `dist` 整目录放入 XP SP3，填写模型配置，验证中文对话、一次无害工具调用、停止和退出清理。
