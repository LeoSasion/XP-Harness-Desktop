# 验证记录

日期：2026-10-10。基线：bcsongor/vc6ai `f99d2f2`。

## 已完成

- Zig 0.13.0，`x86-windows-gnu`，编译 AgentCore、Win32 桌面与测试程序成功。
- `tests/native_tests.c`：118 项检查通过、0 失败。覆盖缺失/异常 API 字段、可选 usage、统计累计、无效工具参数、IPC 连续帧、超时与取消后的重用、零长/超长/NUL/截断帧。
- `tests/smoke.py`：启动实际 AgentCore 子进程，验证分段传输、Unicode、reset、缺失 API 地址、未知请求、shutdown、异常 JSON、超长帧；全部通过。新增本地 HTTP 服务验证：完整自定义路径和查询串、有/无 Bearer 认证、无服务商专用请求字段、连续两轮 `echo` 工具调用与后续追问历史；全部通过。
- `tests/stream_smoke.py`：真实 CLI 子进程连接本地 SSE 模拟服务，验证结束前已输出思考文本、碎片化 UTF-8/JSON/CRLF、工具参数拼接与 `echo` 结果、思考内容跨工具和追问回传、ExtraBody 保留核心字段、流截断/无效工具索引/HTTP 401 后不执行工具且可继续对话；全部通过。
- `tests/desktop_memory_smoke.py`：真实 Core 管道连接本地 HTTP/SSE 服务；项目与会话创建/重命名、共享笔记隔离、各项目 AGENTS.md 和目录提示隔离、提前到达的流式思考、工具工作目录/stdin/结果、DeepSeek 思考历史配对、失败和 Stop 后恢复、16 KiB 上下文硬预算及完整磁盘历史、270 KiB 回复跨帧重放均通过。并发库锁、截断/结构损坏库保护、注入原子保存失败时零 API 请求且保留旧文件均通过。
- DeepSeek 官方真实联调：`/models` 鉴权 HTTP 200；`deepseek-flash` 思考模式下流式回复成功，约 0.5 秒出现首段思考文本；数学回答、一次 `echo XP-HARNESS-OK` 工具调用及后续追问成功。密钥与联调日志仅存本地忽略文件。
- 新桌面窗口可启动并加载持久库，Computer Use 能读取项目/会话树、Memory 入口、工作目录、上下文预算、输入框、按钮与 Ready 状态。纯 C89 编译与 `-Wall -Wextra` 检查无警告。
- Git 差异检查通过（使用 `core.whitespace=cr-at-eol`，保留上游 Windows CRLF）。

自动化测试不发送云端模型请求；本地模拟测试只执行无害 `echo`。DeepSeek 真实联调另外执行一次无害 `echo`，不读取用户文件。

## 尚未完成

- 当前环境没有 VC6/MFC 工具链和 XP 虚拟机，`build.bat` 的 VC6 编译与 XP SP3 实机运行尚未验证。MVP 本身不依赖 MFC。
- DeepSeek 已在现代 Windows 开发机联调；其他服务商与目标 XP 机器的联网兼容性尚未验证。
- 本次 Computer Use 窗口截图连续超时，模拟点击报告坐标几何不可用；窗口按钮、键盘快捷键、视觉布局和系统主题切换尚未完成交互验证。控件树可读取，底层 IPC 功能由集成测试覆盖。

## 产物与边界

`build/host/XP-Harness-Desktop.exe` 为 86,016 字节，`AgentCore.exe` 为 114,176 字节；不含上游 libcurl 和命令行工具。这些是现代 Windows 开发测试产物，不能作为 XP 兼容性证明。

目录提示已转为 UTF-8；上游工具命令和 stdin 仍沿用原编码，MVP 文件工具建议使用 ASCII 路径与文本命令。桌面模式将非 UTF-8 工具输出按系统 OEM 代码页转成 UTF-8，无法用系统 ANSI 代码页表示的项目路径明确拒绝。工具输出捕获最多 4 MiB，超限结束命令。同步 stdin 写入长时间阻塞可用 Stop 结束本窗口拥有的 Core；已完成历史保留在本地，下次继续原会话。对话窗口暂显示原始 Markdown 文本。

存储为可读 JSON，没有数据库或向量检索依赖。整库最多 32 MiB、每会话过程记录最多 4 MiB、项目笔记最多 16 KiB；达到限制会报错，不会自动删除旧记录。旧轮次记忆采用有界摘录，并非云端语义总结。模型输入仍受所配置服务的实际上下文限制，ContextBytes 不是 tokenizer 的 token 计数。MCP 未实现。

下一步验收：在 VC6 SP6 环境运行 `build.bat`，将 `dist` 整目录放入 XP SP3，填写模型配置，验证中文对话、一次无害工具调用、停止和退出清理。
