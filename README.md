# XP-Harness-Desktop

VC6AI 的极简 Windows 原生桌面版。基于 [bcsongor/vc6ai](https://github.com/bcsongor/vc6ai)，复用其 C 代码、会话管理、单一 `cmd` 工具、cJSON 和随附的 libcurl；增加桌面窗口与 Named Pipe 通信，模型地址由用户配置。

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

- 纯 Win32 聊天窗口、Unicode 输入与回复、工具调用状态。
- 独立 AgentCore，后台请求不会阻塞窗口。
- 发送、新对话、停止、配置入口；Ctrl+Enter 发送。
- 配置沿用 `vc6ai.ini`，用系统记事本编辑。
- `AgentCore.exe` 不带参数仍是上游命令行模式。

对话仅保存在内存。停止会结束本窗口启动的 Core，下次发送创建新会话。Markdown 暂按文本显示。Memory、Skill、DLL 插件、托盘、拖放和 Explorer 集成均不在本次 MVP 内。

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

旧版 `[OpenRouter]` 配置需要改为 `[LLM]` 并填写地址；不再发送 Provider、Effort、DataCollection、ZeroDataRetention 或缓存扩展参数，模型的思考模式使用服务端默认值。可选 `ContextWindow` 仅供 CLI 显示上下文统计。保存后点击 **Stop**，再发送消息以重新载入配置。不要提交含密钥的 INI 文件。

### 当前 Agent 能力

- 多轮对话与多轮工具调用：支持。每次桌面提问最多 24 次模型请求，每个响应最多 16 个工具调用；当前仅有 `cmd` 工具，按顺序执行，结果回传模型。
- 深度思考展示：未实现。目前是非流式请求，仅显示状态、工具命令和最终回答；不展示单独的 reasoning 字段。可展示的思考内容取决于服务端是否公开返回，客户端无法获取模型未提供的内部推理。
- MCP：未实现，没有 MCP 协议客户端、服务器配置或工具发现。

上游附带的 libcurl 使用 OpenSSL；这里保留证书验证与 CA 文件，不改用 Windows 系统 TLS。实际联网能力仍需在目标 XP 机器验证。不要仅凭 `_WIN32_WINNT` 或 PE 子系统版本判断兼容性。

工作目录为应用所在目录；继承的 `cmd` 工具会以当前用户权限执行命令，并非沙箱。聊天文本经 UTF-8 管道传输；目录提示已转换为 UTF-8。工具命令、stdin 和输出仍沿用上游编码，MVP 文件工具仅支持 ASCII 路径与文本命令，建议安装到 `C:\XP-Harness-Desktop`。工具输出捕获上限为 4 MiB，超限结束该命令（exit 122）。

## 代码位置

| 文件 | 职责 |
| --- | --- |
| `src/main.c` | 复用上游模型、工具、会话与 CLI，补充必要校验 |
| `src/agent_mode.inc` | 桌面模式入口，复用同一份 Agent 实现 |
| `src/desktop.c` | 一个 Win32 窗口与后台通信线程 |
| `src/ipc.c` / `src/ipc.h` | 长度前缀、256 KiB 上限、可取消 I/O、当前登录会话 ACL |
| `src/cJSON.*` / `dep/` | 上游依赖 |

IPC 为 4 字节小端长度加 UTF-8 JSON；单连接、单次请求。请求类型为 `chat`（带 `text`）、`reset`、`shutdown`；事件类型为 `status`、`assistant`、`tool`、`error`、`done`。这是内部 MVP 协议，不引入通用 RPC 或插件协议。

## 验证

现代 Windows 开发机可用 Zig 0.13.0 做 x86 宿主机检查：

```powershell
& .\tests\build-host.ps1 -Zig 'C:\tools\zig\zig.exe'
python tests\smoke.py build\host\AgentCore.exe
```

这些测试不需要真实 API Key、不请求云端模型。原生测试覆盖响应校验与 IPC；smoke 测试运行本地 HTTP 模拟服务，检查自定义地址、可选认证、两轮无害 `echo` 工具调用与后续对话历史。`build\host` 是开发验证产物，不是经过认证的 XP 发布包。

当前验证结果见 [VALIDATION.md](VALIDATION.md)。
