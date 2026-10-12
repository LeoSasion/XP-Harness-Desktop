# XP-Harness-Desktop

VC6AI 的极简 Windows 原生桌面版。左侧管理项目与会话，右侧流式显示思考、回答和工具过程，颜色跟随 Windows。基于 [bcsongor/vc6ai](https://github.com/bcsongor/vc6ai)，复用其 C 代码、单一 `cmd` 工具、cJSON 和随附的 libcurl，模型地址由用户配置。

上游基线：`f99d2f2ec459dff38b5afe89ff0d8a38cf952972`。原 MIT 许可证和第三方 NOTICE 均保留。

```text
XP-Harness-Desktop.exe                 AgentCore.exe
Win32 原生控件  ← JSON →  原 VC6AI Agent 循环
               Named Pipe       ↓
                          libcurl / 自定义 API
                                ↓
                            cmd.exe 工具
```

## MVP 范围

- 纯 Win32 项目/会话树、Unicode 输入与回复、嵌入式流式过程输出。
- 独立 AgentCore，后台请求不会阻塞窗口。
- 创建项目、创建会话、重命名、切换会话、编辑项目记忆；Ctrl+Enter 发送。
- 会话右键导出/删除；拖入文件将引用路径加入输入草稿。
- 会话和项目笔记本地持久化；命令逐次确认，Stop 和崩溃后保留已记录的工具结果。
- HTTP 鉴权、限流、端点和服务故障，以及 DNS、连接、超时、TLS 错误显示处理提示。
- 配置沿用 `vc6ai.ini`，用系统记事本编辑。
- `AgentCore.exe` 不带参数仍可单独使用命令行模式。

桌面程序不会打开外置终端。右侧原生文本控件直接接收 Core 的流式事件；Markdown 暂按文本显示。Skill、DLL 插件、托盘、Explorer 集成和 MCP 不在本次 MVP 内。

## 项目、会话与记忆

**New project** 选择工作文件夹并命名；该项目的工具在这个文件夹执行。左侧树列出项目及各自的会话，**New chat** 在当前项目下创建独立对话，**Rename** 修改选中项的名称。切换会话会恢复过程记录与模型历史，不会把其他会话的对话自动混入。

**Memory** 编辑当前项目共享的长期笔记，例如目标、约定和用户主动保存的结论。笔记在同一项目的各会话中使用，其他项目看不到。它是用户明确维护的记忆，程序不会把临时回答自动当作事实保存。

右键会话（或在树上按 Shift+F10）可 **Export session** 或 **Delete session**。导出通过系统保存对话框写入 UTF-8 JSON，包含完整原始消息、公开思考、工具调用与结果，以及显示记录和最近执行日志；不受 IPC 单帧或显示截取影响。导出采用临时文件、刷新和原子替换，失败保留旧目标文件；应用 `data` 目录不能作为导出目的地。当前没有导入功能。删除默认选择 No，仅删除该会话记录，保留项目文件和共享笔记；删除当前会话切换到可用会话，必要时创建空会话，库中始终保留一个会话。

从 Explorer 拖入一个或多个文件，将带引号的路径追加到输入草稿，再补充指令并发送。拖放本身不读取文件、不发送模型请求；实际读取仍通过 `cmd` 并逐次确认。整批路径超出 MAX_PATH 或输入长度时拒绝，保留原草稿。拖放是路径引用，不包含 Office/PDF 解析器。

桌面数据保存在程序目录的 `data/library.json`，包含项目、会话原始消息、公开思考文本及工具过程。写入采用同目录临时文件、刷新和原子替换；数据目录已被 Git 忽略。一个数据目录只允许一个 Core 写入，损坏的数据不会被自动重置。工具结果与显示记录在同一次检查点保存，再通知桌面。中断轮次保留用户目标、完整工具调用及已记录结果；缺失结果会补上明确的未执行或结果不明标记，维持工具调用与结果配对。整库最多 32 MiB、每会话显示记录最多 4 MiB；普通聊天达到限制会明确报错。工具结果和中断恢复仍能保存：必要时缩短显示记录并标明截取，原始消息和完整工具结果不删减。

发送给模型的是有界上下文：项目系统提示、项目笔记、旧轮次摘录和最近完整轮次。`ContextBytes=65536` 默认限制消息 JSON 的 UTF-8 字节数，可配置为 16384–262144。工具调用和结果按完整轮次保留，旧工具输出可在模型输入中截短，磁盘原始记录不受影响。旧轮次摘录不是语义总结，也不是无限记忆；不会为此额外调用云模型。当前完整轮次无法放入预算时会明确报错。

## XP 环境、工具确认与恢复

系统提示固定以 **Windows XP SP3 x86** 为兼容目标，在现代 Windows 开发机上也不放宽要求。环境信息包含 Windows API 报告的系统版本、实际 cmd.exe 路径、工作目录和 ANSI/OEM 代码页。只列出程序目录中检测到的 BusyBox、jq、curl 的绝对路径；检测仅检查文件存在，不验证版本。不假定 PowerShell、Python、winget、WSL 或现代命令可用，工具缺失时要求模型说明限制。

桌面每次执行有效 `cmd` 调用前显示原生 **Tool permission** 窗口：完整工作目录、命令和 stdin 可滚动查看，提供 **Allow once**、**Deny**、**Stop task**。默认焦点为 Deny，关闭窗口也拒绝。许可只对应本次唯一标识；拒绝、错误/过期标识和非布尔允许值都不会执行命令。命令最多 4000 字节，桌面 stdin 最多 128 KiB；整个确认事件仍受 256 KiB 管道帧上限限制，超限不执行。

当前/最近一轮的 `turn` 检查点与工具状态保存在原 JSON 库内，已有 v1 库可直接读取。命令启动前必须成功保存 `running`，命令结束后立即保存结果，再请求模型；后续 API 失败不会抹掉这些结果。保存失败时停止后续执行，保留磁盘上的上一次成功检查点。重启只恢复本地记录，不自动请求模型或重放命令；在原会话发送“继续”可带着这些记录继续对话，每个新命令仍须重新确认。

Stop 通过 Win32 Event 通知 Core，取消 HTTP、终止当前命令及 Job Object 中的子进程，保存状态；5 秒仍未完成时强制结束本窗口拥有的 Core。stdin 写入与输出读取并行，避免满管道阻塞停止。崩溃时处于 `running` 且没有持久结果的调用标记为 **UNKNOWN**，提示用户核查影响；等待确认的调用标记为未执行。恢复不会撤销已经发生的文件修改，也不保证任意命令恰好执行一次。

这些确认和持久恢复功能用于桌面/管道模式；独立 CLI 保持上游自动执行方式，不使用桌面审批和项目库。

## 构建与运行

目标构建工具为 Visual C++ 6.0 SP6，目标系统为 Windows XP SP3 x86。使用 VC6 命令提示符（先运行其 `VCVARS32.BAT`）：

```bat
build.bat
dist\XP-Harness-Desktop.exe
```

构建脚本生成 `dist\XP-Harness-Desktop.exe`、`dist\AgentCore.exe`，并复制 libcurl、CA 证书、配置样例和上游工具。不需要 MFC、Electron、Node.js 或 Python 运行时。

点击 **Settings** 编辑 `dist\vc6ai.ini`：

```ini
[LLM]
ApiUrl=https://your-server.example/v1/chat/completions
ApiKey=your-key
Model=your-model
```

`ApiUrl` 必须是完整的 HTTP/HTTPS Chat Completions 端点；程序不拼接路径，不内置服务商地址。`ApiKey` 使用 Bearer 认证，不需要认证的本地服务可以留空。模型需支持 `messages`、`tools` 和 `tool_calls`；不直接支持其他协议（例如 Anthropic Messages 或 Responses）。不自动跟随重定向，请填写最终端点。

请求失败时显示 HTTP 状态和处理提示：401 检查密钥，403 检查权限，429 检查限流/额度，404/405/重定向检查完整端点，5xx 提示服务暂不可用。DNS、连接拒绝、超时、TLS 和响应中断也有对应提示；这些 HTTP 和连接提示不回显请求密钥或 URL。修复设置或稍后在原会话发送“继续”可带着已有记录继续，不增加自动重试或命令重放。

旧版 `[OpenRouter]` 配置需要改为 `[LLM]` 并填写地址；不再默认发送 Provider、Effort、DataCollection、ZeroDataRetention 或缓存扩展参数。可选 `ExtraBody` 为一行 JSON 对象，用于服务商的思考开关等参数；不能覆盖 `model`、`messages`、`tools`、`stream`。`Stream=1` 默认开启桌面与 CLI 流式输出；`Stream=0` 使用完整 JSON 响应。可选 `ContextWindow` 仅供 CLI 显示上下文统计。桌面 **Settings** 保存配置后，下次请求重启 Core 应用设置并恢复会话；命令行则退出后重启。不要提交含密钥的 INI 文件。

### 命令行与 DeepSeek

双击根目录 `console.bat`，或进入构建产物目录运行 `AgentCore.exe`。`Stream=1`（默认）启用命令行流式输出，依次显示 `[Request]`、`[Thinking]`、`[Answer]`、`[Tool]` 和 `[Tool result]`，包含完整命令、结果和退出码。Ctrl+C 中断当前请求，`/new` 清空会话，`/exit` 退出。`Stream=0` 使用完整 JSON 响应。流式时按原始文本输出 Markdown；没有额外终端库。

DeepSeek 官方配置示例（密钥自行填写）：

```ini
[LLM]
ApiUrl=https://api.deepseek.com/chat/completions
ApiKey=
Model=deepseek-flash
Stream=1
ExtraBody={"thinking":{"type":"enabled"},"reasoning_effort":"high"}
```

模型名和参数依据 [DeepSeek 官方入门文档](https://api-docs.deepseek.com/) 与 [思考模式文档](https://api-docs.deepseek.com/guides/thinking_mode/)。仅展示 API 实际公开返回的思考文本；简单请求可能没有思考片段。`reasoning_content` 随 assistant 历史保留并回传，以支持 DeepSeek 思考模式下的多轮工具调用。

### 当前 Agent 能力

- 多轮对话与多轮工具调用：支持。每次提问最多 24 次模型请求，每个响应最多 16 个工具调用；当前仅有 `cmd` 工具，按顺序执行，结果回传模型。
- 深度思考展示：桌面与命令行支持流式 `reasoning_content`（也接受文本 `reasoning`），回答和工具参数的碎片会正确拼接。缺失完成标记、中断或错误的响应不会执行其中的工具；桌面保留此前已完整接收的工具调用、公开思考与执行结果，部分流式回答仅作为过程记录保存。桌面右侧依次显示思考、回答、工具命令、stdin、结果和退出码。
- MCP：未实现，没有 MCP 协议客户端、服务器配置或工具发现。

上游附带的 libcurl 使用 OpenSSL；这里保留证书验证与 CA 文件，不改用 Windows 系统 TLS。实际联网能力仍需在目标 XP 机器验证。不要仅凭 `_WIN32_WINNT` 或 PE 子系统版本判断兼容性。

桌面工具工作目录为所选项目文件夹，默认项目与 CLI 使用应用目录；继承的 `cmd` 工具以当前用户权限执行命令，并非沙箱。聊天文本和目录提示使用 UTF-8；桌面将非 UTF-8 工具输出按系统 OEM 代码页转换。工具命令和 stdin 仍沿用上游编码，建议使用 ASCII 路径与文本命令，并安装到 `C:\XP-Harness-Desktop`；无法用系统 ANSI 代码页表示的项目路径会明确拒绝。工具输出捕获上限为 4 MiB，超限结束该命令（exit 122）。

## 代码位置

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | 复用上游模型、工具、会话与 CLI，补充必要校验 |
| `src/agent_mode.inc` | 桌面模式入口，复用同一份 Agent 实现 |
| `src/stream.inc` | 桌面/CLI 共用 SSE 增量解析、思考/回答输出和工具参数拼接 |
| `src/memory.inc` | 本地项目/会话库、原子保存与上下文投影 |
| `src/recovery.inc` | 当前轮次检查点、执行状态校验与中断结果补齐 |
| `src/desktop.c` | 原生项目树、聊天与记忆编辑、后台通信线程 |
| `src/ipc.c` / `src/ipc.h` | 长度前缀、256 KiB 上限、可取消 I/O、当前登录会话 ACL |
| `src/cJSON.*` / `dep/` | 上游依赖 |

IPC 为 4 字节小端长度加 UTF-8 JSON，单连接、请求顺序执行。`library` 列出项目/会话，`open` 恢复会话；`new_project`、`new_chat`、`rename`、`memory` 管理本地数据。`chat` 带会话 `id`、`text` 和 `stream:true`；旧客户端不带 `stream` 时仍用完整 JSON。过程事件包含 `reasoning_delta`、`answer_delta`、`tool`、`tool_stdin`、`tool_result`、`context`、`error`，每个请求以 `done` 结束。`reset` 与 `shutdown` 保留兼容。这是内部 MVP 协议，不引入通用 RPC 或插件协议。

`delete_chat` 带会话 `id`，删除后更新目录并在需要时打开替代会话；`export_chat` 带会话 `id` 和 UTF-8 目标 `path`，Core 直接原子导出 JSON，不经 IPC 传输整份历史。这两个管理操作不请求模型。

新增 `permission` 事件，`text` 为 JSON 对象，包含 `id`（一次性申请标识）、`tool_call_id`、`name`、`cwd`、`command`、`stdin`。客户端仅在收到申请后回复 `{"type":"permission","id":"申请标识","allow":true}` 或 `allow:false`。老管道客户端需要处理这个事件；10 分钟无回复按中断处理。桌面将一个匿名取消 Event 的句柄通过 `--cancel` 参数交给 Core，Core 从指定父进程复制仅等待权限的句柄。

## 验证

现代 Windows 开发机可用 Zig 0.13.0 做 x86 宿主机检查：

```powershell
& .\tests\build-host.ps1 -Zig 'C:\tools\zig\zig.exe'
python tests\smoke.py build\host\AgentCore.exe
python tests\stream_smoke.py build\host\AgentCore.exe
python tests\desktop_memory_smoke.py build\host\AgentCore.exe
python tests\approval_recovery_smoke.py build\host\AgentCore.exe
```

这些测试不需要真实 API Key、不请求云端模型。原生测试覆盖响应校验与 IPC；smoke 测试检查自定义地址、可选认证、两轮无害 `echo` 工具调用与后续历史；stream_smoke 检查实时输出、碎片化 SSE 与错误恢复；desktop_memory_smoke 检查项目/会话与笔记隔离、工作目录、重启和 Stop 恢复、上下文预算、完整历史、长回复跨帧恢复、库锁与损坏/保存失败保护。`build\host` 是开发验证产物，不是经过认证的 XP 发布包。

当前验证结果见 [VALIDATION.md](VALIDATION.md)。
