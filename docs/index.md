# LUSTED Documentation

LUSTED is a Windows C++ application with a Luau runtime, a Dear ImGui control panel, and a transparent GDI drawing overlay. User features are written in Luau and run in the application process through native bindings.

## Build

Requirements:

- Windows 10 or newer
- CMake 3.20 or newer
- A C++20 Windows toolchain. The verified build uses MinGW-w64.
- Network access during the first configure to fetch Luau, nlohmann/json, and Dear ImGui.

From the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Run `build/lusted.exe`. `--help` prints command-line help. `--once` initializes, loads modules, executes auto-run scripts, and then exits. Normal mode stays active until the unload keybind or Settings action is used.

## Startup Sequence

1. Locate the Roblox installation and running process, if any.
2. Select the version hash and fetch or reuse its offsets.
3. Initialize the external overlay windows and attach to the process when available.
4. Create the Luau VM and register native globals.
5. Load the three standard modules: `helpers.luau`, `ui.luau`, and `drawing.luau`.
6. Execute `.lua` and `.luau` files directly inside `C:/LUSTED/Luas`, sorted alphabetically.

Startup stops with a clear error if one of the required standard modules is missing. The source checkout used for this documentation currently has no `examples/` directory, so restore those modules in a location searched by the executable before running it.

## Navigation

See [Runtime Data and Settings](/runtime-data) for folders, configs, hotkeys, rescan, and unload. See [Luau Scripting](/luau-scripting) for globals and the native API. See [UI and Drawing](/ui-and-drawing) for widgets, callbacks, and overlay primitives.
