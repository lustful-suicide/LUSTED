# lusted MCP server (`--mcp`)

AI assistants drive the running `lusted` session over stdio with
newline-delimited [JSON-RPC 2.0](https://www.jsonrpc.org/specification).
One JSON object per line on stdin, one JSON object per line on stdout.
No extra dependencies: transport is raw stdio plus the already-vendored
`nlohmann::json`.

Launch: `lusted.exe --mcp`. Shutdown: close stdin (EOF). Log/diagnostic
text goes to stderr; **stdout carries only JSON-RPC frames** (internal
`std::cout` diagnostics are muted while a tool runs).

## Protocol notes

- Request: `{"jsonrpc":"2.0","id":<number|string>,"method":"...","params":{...}}`.
- `id` may be numeric or string and is echoed verbatim in the response.
  Requests without `id` are notifications and get no reply.
- Handshake: `initialize` replies `protocolVersion "2024-11-05"`,
  `capabilities {tools:{}, prompts:{}, resources:{}}`,
  `serverInfo {name:"lusted", version:"1.0"}`. `notifications/initialized`
  gets no reply. `ping` replies `{"result":{}}`.
- `tools/list` replies `{"tools":[{name, description, inputSchema}, ...]}`.
- `tools/call` takes `{"name":"...","arguments":{...}}` and replies with a
  `CallToolResult`: `{"content":[{"type":"text","text":"<JSON payload>"}]}`,
  plus `"isError":true` when the tool itself failed (not attached, read
  failed, script error, ...). Bad tool names/args are JSON-RPC `-32602`.
- Errors are per JSON-RPC: `-32700` parse error, `-32600` invalid request,
  `-32601` method not found, `-32602` invalid params, `-32603` internal.
- Session: `attach` finds `RobloxPlayerBeta` (or uses a given pid),
  `mem.Attach(pid)`, loads `offsets.json` via `OffsetsFetcher`.
  The instance index builds lazily on the first index-dependent call and
  rebuilds when the pid changes. The Luau state (`LuauManager` with
  `ui=nullptr, dr=nullptr`) inits lazily on the first `run_luau`.
- Conventions: addresses are `"0x..."` hex strings (numbers also accepted);
  binary reads/writes are base64.

## Tool table

| Tool | Args | Returns |
| --- | --- | --- |
| `status` | — | `{attached, pid, versionHash, offsets_loaded, offsets_path, index_ready, index_instances, datamodel}` |
| `attach` | `pid?` (number or hex) | `{ok, pid, versionHash, offsets_loaded, offsets_path}` |
| `index_stats` | — | `{pid, ready, instances, stats}` |
| `refresh_index` | `force?`, `deep?` | `{ok, ready, instances, stats}` |
| `get_datamodel` | — | `{address, name, class}` |
| `get_service` | `name` | `{address, name, class}` |
| `get_players` | — | `{count, players:[{address,name,class}]}` |
| `get_children` | `addr` | `{address, count, children:[...], truncated}` (capped at 2000) |
| `get_name` | `addr` | `{address, name}` |
| `get_class` | `addr` | `{address, class}` |
| `get_parent` | `addr` | `{address, parent\|null}` |
| `find_child` | `addr`, `name` | `{address, name, class}` |
| `resolve_path` | `root`, `dotted` | `{address, name, class}` |
| `read_bytes` | `addr`, `length` (1..65536) | `{address, length, base64}` |
| `write_bytes` | `addr`, `data` (base64, ≤65536 decoded bytes) | `{ok, address, bytes_written}` |
| `run_luau` | `code` | `{ok, output}` (`print()` captured; runtime error appended to `output`) |
| `get_camera` | — | `{address, pos[3], rot[9], fov, viewport[2]}` |
| `get_module_base` | `name` | `{name, base, size}` |
| `get_bytecode` | `addr` | `{address, class, name, encoding, byte_length, bytecode_base64}` — fails honestly until the bytecode reader lands; never guesses binary layouts |
| `decompile` | `bytecode` (base64), `filename?` | `{bytes, source, saved_to?}` — LunaUX round trip over any valid chunk |
| `list_scripts` | — | `{count, scripts:[{address,name,class,parent}]}` |
| `decompile_selftest` | — | `{ok, source}` — compiles a snippet locally and decompiles it; proves the integration |

Minimal session example (each line is one frame):

```json
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
{"jsonrpc":"2.0","method":"notifications/initialized"}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"attach","arguments":{}}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"run_luau","arguments":{"code":"print(game.Name)"}}}
```

## opencode client config

```json
{"mcp": {"lusted": {"type": "local", "command": ["C:/Users/real/Desktop/LUSTED/build/lusted.exe", "--mcp"]}}}
```
