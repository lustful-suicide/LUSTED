# Troubleshooting

## Required Standard Module Not Found

The host loads `helpers.luau`, `ui.luau`, and `drawing.luau` before user scripts. It searches for an `examples` directory beside the executable, one directory above it, then in the current working directory. If a required file is missing, startup reports its path and stops before auto-execution.

The source checkout used to prepare this guide does not currently contain `examples/`. Restore the three bootstrap modules before running the executable. User scripts in `C:/LUSTED/Luas` are not substitutes for these modules because startup loads standard modules first.

## No Live DataModel

If the `game` global is nil, Roblox may not be running, the process may not be attached, or the DataModel scan may not resolve. Start the player and use **Settings > Rescan Roblox**. Instance and memory helpers can return nil or false while unattached.

## Offset Download or Cache

Offsets are stored in `C:/LUSTED/offsets/version-<hash>`. The app reuses valid cached files. If downloads fail, check network access and confirm the running Roblox version hash. An old repository-local `offsets/` cache is not the active root; the cache for the current version was copied to the central path.

## Auto-Run Script Does Not Load

Only direct `.lua` and `.luau` children of `C:/LUSTED/Luas` are discovered. The Settings runner accepts one filename and rejects path separators. Check the console for `[auto] running` and `[luau] runtime` messages.

## UI Control Is Missing

A control is assigned to the tab and section active when it is created. Call `ui_tab` and `ui_section` before adding controls. Settings is always forced to the final tab position. Use `ui_on_pressed` for buttons/keybinds and `ui_on_changed` for value controls.
