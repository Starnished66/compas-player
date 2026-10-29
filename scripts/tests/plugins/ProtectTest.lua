-- Probe plugin for scripts/tests/test_plugin_program_dirs.sh: tries every plugin write path into
-- .apps, .apps-data and .emulators and writes PASS/FAIL lines to <sd>/protect_results.txt.
plugin.define({ id = "test.protect", name = "Protect Test", version = "1.0.0", api_min = 1 })
local root = plugin.sd_root()
local out = {}
local function note(name, ok) out[#out + 1] = (ok and "PASS " or "FAIL ") .. name end
local function why(e) return tostring(e):find("program folders", 1, true) ~= nil end
local function blocked_open(path, mode)
    local f, err = io.open(path, mode)
    if f then f:close() end
    return f == nil and why(err)
end

-- plain writes into the three program folders, any case, any write mode
for _, dir in ipairs({ ".apps", ".APPS", ".apps-data", ".emulators", ".Emulators" }) do
    note("io.open w " .. dir, blocked_open(root .. "/" .. dir .. "/evil.txt", "w"))
end
note("io.open wb", blocked_open(root .. "/.apps/hello/evil.bin", "wb"))
note("io.open a", blocked_open(root .. "/.apps/hello/evil.bin", "a"))
note("io.open r+", blocked_open(root .. "/.apps/hello/app.json", "r+"))
note("io.open w via ..", blocked_open(root .. "/Music/../.apps/evil.txt", "w"))
note("io.open w via ./", blocked_open(root .. "/./.apps//hello/evil.txt", "w"))
note("evil.txt does not exist", io.open(root .. "/.apps/evil.txt", "r") == nil and io.open(root .. "/.emulators/evil.txt", "r") == nil)
local f = io.open(root .. "/.apps/hello/app.json", "r")
note("io.open r still works", f ~= nil)
if f then f:close() end
local ok, e0 = pcall(io.output, root .. "/.apps/out.txt")
note("io.output refused", not ok and why(e0))
local r, e1 = os.remove(root .. "/.apps/hello/app.json")
note("os.remove refused", r == nil and why(e1))
local rn, e2 = os.rename(root .. "/a.txt", root .. "/.apps/a.txt")
note("os.rename into refused", rn == nil and why(e2))
rn, e2 = os.rename(root .. "/.apps/hello/app.json", root .. "/moved.json")
note("os.rename out refused", rn == nil and why(e2))
for _, dir in ipairs({ ".apps/new", ".apps-data/new", ".emulators/new", ".APPS/n2" }) do
    local m, e3 = plugin.mkdir(root .. "/" .. dir)
    note("mkdir " .. dir, m == nil and why(e3))
end
note("mkdir ok elsewhere", plugin.mkdir(root .. "/okdir") == true)
for _, dir in ipairs({ ".apps", ".apps/hello", ".emulators", ".apps-data" }) do
    local c, e4 = pcall(plugin.file_copy, { root .. "/a.txt" }, root .. "/" .. dir)
    note("file_copy into " .. dir, not c and why(e4))
    c, e4 = pcall(plugin.file_move, { root .. "/a.txt" }, root .. "/" .. dir)
    note("file_move into " .. dir, not c and why(e4))
end
local d, e5 = plugin.file_delete({ root .. "/.apps/hello/app.json" })
note("file_delete inside refused", d == nil and why(e5))
d, e5 = plugin.file_move({ root .. "/.emulators" }, root .. "/okdir")
note("file_move of .emulators refused", d == nil and tostring(e5):find("cannot", 1, true) ~= nil)
local dl, e6 = pcall(plugin.download_file_async, "http://127.0.0.1:9/x", root .. "/.apps/dl.bin", function() end)
note("download_file_async refused", not dl and why(e6))
dl, e6 = pcall(plugin.download_file_async, "http://127.0.0.1:9/x", root .. "/.emulators/dl.bin", false, function() end)
note("download_file_async emulators refused", not dl and why(e6))
note("eq_save_profile refused", plugin.eq_save_profile(root .. "/.apps/p.txt") == false)
local g = io.open(root .. "/okfile.txt", "w")
note("io.open w ok elsewhere", g ~= nil)
if g then g:write("x") g:close() end
note("file_copy ok elsewhere", plugin.file_copy({ root .. "/okfile.txt" }, root .. "/okdir") == true)


-- creation through the name of the copy/move result, and FAT name aliases
for _, name in ipairs({ ".apps", ".emulators", ".apps-data" }) do
    local c, e = plugin.file_copy({ root .. "/stage/" .. name }, root)
    note("file_copy of stage/" .. name .. " to the card root", c == nil and why(e))
    c, e = plugin.file_move({ root .. "/stage/" .. name }, root)
    note("file_move of stage/" .. name .. " to the card root", c == nil and why(e))
end
for _, name in ipairs({ ".apps.", ".APPS...", ".emulators.", ".apps-data." }) do
    local m, e = plugin.mkdir(root .. "/" .. name)
    note("mkdir " .. name, m == nil and why(e))
    local r, e2 = os.rename(root .. "/a.txt", root .. "/" .. name)
    note("os.rename to " .. name, r == nil and why(e2))
    note("io.open w " .. name .. "/x", blocked_open(root .. "/" .. name .. "/x", "w"))
end
note("mkdir nested .apps", plugin.mkdir(root .. "/Music/.apps/x") == nil)

-- the approval file and its temporary
note("approval file a", blocked_open("./apps_approved.txt", "a"))
note("approval file w", blocked_open("apps_approved.txt", "w"))
note("approval tmp w", blocked_open("./apps_approved.txt.tmp", "w"))
local rr, ee = os.remove("./apps_approved.txt")
note("approval remove", rr == nil and why(ee))
rr, ee = os.rename("./a.txt", "./apps_approved.txt")
note("approval rename onto", rr == nil and why(ee))
local dlr, dle = pcall(plugin.download_file_async, "http://127.0.0.1:9/x", "./apps_approved.txt", function() end)
note("approval download", not dlr and why(dle))

-- image helpers that write a destination
local function img(name, ...)
    local okc, e = pcall(plugin[name], ...)
    note(name .. " into .apps", not okc and why(e))
end
local nop = function() end
img("zip_image_async", root .. "/a.zip", "e.png", root .. "/.apps/x.bin", 32, 32, nop)
img("image_thumbnail_async", root .. "/a.png", root .. "/.emulators/x.bin", 32, 32, nop)
img("image_wallpaper_async", root .. "/a.png", root .. "/.apps-data/x.bin", nop)
img("image_startup_async", root .. "/a.png", root .. "/.APPS/x.bin", nop)

-- apps API
note("capability apps.list", plugin.has_capability("apps.list") == true)
note("capability apps.launch", plugin.has_capability("apps.launch") == true)
local apps = plugin.list_apps()
local names = {}
for _, a in ipairs(apps) do names[#names + 1] = a.id .. ":" .. a.name .. ":" .. a.version .. ":" .. table.concat(a.opens, ",") end
out[#out + 1] = "apps: " .. table.concat(names, " | ")
local l, reason = plugin.launch_app("hello")
note("launch_app at load refused (" .. tostring(reason) .. ")", l == nil)

-- network API (API 18)
note("api_version >= 18", plugin.api_version() >= 18)
note("capability network.status", plugin.has_capability("network.status") == true)
local ns = plugin.network_status()
note("network_status shape", type(ns) == "table" and type(ns.connected) == "boolean"
  and type(ns.wifi_enabled) == "boolean")
note("plugin.on network_changed accepted", pcall(plugin.on, "network_changed", function() end))

local h = io.open(root .. "/protect_results.txt", "w")
h:write(table.concat(out, "\n"), "\n")
h:close()
