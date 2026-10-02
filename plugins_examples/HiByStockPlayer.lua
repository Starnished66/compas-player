-- HiByStockPlayer.lua
--
-- Adds a Player layout that imitates the stock HiBy OS Now Playing screen: a
-- full-width square cover, a bottom panel with the title, artist, progress bar
-- and transport row, and lyrics that replace the cover (white lines, blue
-- current line).
--
-- Install: copy this file and the player_layouts folder (hiby_stock.xml and its
-- 480x720 and 320x480 versions) into <SD card>/.plugins/. Then pick "HiBy
-- Stock" in Settings > Display > Player Layout > Layout, or use the "HiBy Stock
-- Player" row in the same menu to apply it at once (until the next restart).
-- The layout uses the stock theme's own topbar and panel artwork; layouts are
-- explained in docs/PLAYER_LAYOUTS.md.

plugin.define({
    id = "org.example.hiby_stock_player",
    name = "HiBy Stock Player",
    version = "1.0.0",
    api_min = 1,
})

local LAYOUT = { xml = "player_layouts/hiby_stock.xml", name = "HiBy Stock" }

if plugin.has_capability and plugin.has_capability("ui.player_layout_xml") then
    -- Top-level call: only lists the layout in Settings.
    plugin.set_player_layout(LAYOUT)

    plugin.register_list_item("display", "HiBy Stock Player", function()
        -- Called from a callback: selects the layout right away and reloads.
        local ok, err = pcall(plugin.set_player_layout, LAYOUT)
        if ok then
            plugin.show_toast("HiBy Stock layout applied")
        else
            plugin.show_toast("Could not apply the layout: " .. tostring(err))
        end
    end, { group = "player_layout" })
end
-- Older player builds have no XML layouts; the plugin then adds nothing.
