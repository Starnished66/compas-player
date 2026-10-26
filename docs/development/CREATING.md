# Create a plugin or theme

Plugins add Lua features. Themes are plain text files read by the player's built-in theme picker. Both can be edited without rebuilding firmware.

## Plugins

A plugin is one Lua file placed directly in `<SD card>/.plugins/`. Here is a working first plugin:

```lua
plugin.define({
    id = "org.example.hello",
    name = "Hello Player",
    version = "1.0.0",
    api_min = 16,
})

plugin.register_list_item("settings", "Hello Player", function()
    plugin.show_list("Hello Player", { "Hello from Lua!" }, function()
        plugin.show_toast("Your plugin is working!")
    end)
end)
```

Save it as `.plugins/Hello.lua`. Insert the card, then open **Settings > System > Plugin Manager > Refresh Plugins**. The **Hello Player** row appears under **Settings > System > Additional Tools**. Tap it to see the message. Browse **Plugin Store** from Plugin Manager for published plugins.

Use the top level of your script to register rows and callbacks. Files in nested folders are not loaded as plugins. Give each plugin a unique, stable ID. API 16 is the standard for new Compás plugins. Set `api_min = 16` and check capabilities before using optional features. See [PLUGINS.md](../../PLUGINS.md) for the complete API, sandbox rules, and examples.

## Themes

Themes are built into the player; you do not need a Lua plugin to create or apply one. Save a `.theme` file in `<SD card>/Themes/`. The built-in picker scans that folder and subfolders up to two levels deep. For example, save this as `Themes/MyTheme.theme`:

```ini
# A small native theme
name=My Theme
screen=0x101820
card=0x1B2838
list_row=0x223449
text_primary=0xF4F7FA
text_muted=0xAAB8C7
```

All six fields are required. Colors accept decimal or `0x` hexadecimal values from `0x000000` to `0xFFFFFF`. Each setting is a `key=value` line; lines beginning with `#` are comments. From the player, open **More > Themes** and select your theme. Close and reopen the picker after adding a file so it rescans. A PNG preview is optional: place `MyTheme.png` beside `MyTheme.theme` to have it appear in the picker.

The parser's optional settings cover icon folders, icon replacements, Home tile styles, and launcher appearance. See [Writing a `.theme` file in PLUGINS.md](../../PLUGINS.md#writing-a-theme-file) for the full key list and the [published theme package examples](https://github.com/Starnished66/compas-plugins/tree/main/plugins/Themes/Themes).

To publish a theme in Plugin Store, follow the individual theme packages in the [compas-plugins repository](https://github.com/Starnished66/compas-plugins/tree/main/plugins). For example, `ThemeCobaltSignal` includes `store.json`, `CobaltSignal.theme`, a matching `CobaltSignal.png` preview, and a compatibility Lua stub. The store package installs the theme and preview under `Themes/`; use the package's `store.json` as the format example. Current players handle `.theme` files natively, so the compatibility stub is not needed to apply the theme.

Themes change colors, icons, and supported Home styling. To redesign the Now Playing screen, create an XML player layout instead; see [Player Layouts](../reference/PLAYER_LAYOUTS.md) and the XML examples in `assets/theme2/player_layouts/`.
