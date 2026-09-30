--[[
Qobuz, for a paid account you already have.

Qobuz does not publish a third-party API or issue app credentials. This
plugin speaks the same HTTPS catalogue the stock player uses. You supply
the app id and app secret once; they are stored with the session token
under plugin.storage and the password is not saved.

The Stream Media row uses the player's own stream_media/qobuz_row.png, the same
theme-relative icon path Net Radio and Podcasts pass to
plugin.register_stream_media_tile().

Playback goes through plugin.play_remote(). A stream link is only good for
a few minutes, the player cannot ask for a fresh one when a later track
starts, and a remote stream is 16-bit with no seeking. "Play these" therefore
resolves at most 12 tracks and starts them immediately. Tap a single row
when a later track fails to open.

Copy Qobuz.lua to <SD card>/.plugins/ and restart.
]]

plugin.define({
    id = "community.qobuz",
    name = "Qobuz",
    version = "1.1.0",
    api_min = 14,
})

if not plugin.has_capability("playback.remote") then
    plugin.show_toast("Qobuz needs a player build with remote playback")
    return
end

local API = "https://www.qobuz.com/api.json/0.2"
local PAGE = 25
local BROWSE_CAP = 475 -- plus the "Play these" row, within show_list's 500 rows
local SEARCH_CAP = 100
local QUEUE_CAP = 12
local JSON_LIMIT = 524288
local playback_generation = 0
local browse_generation = 0
local auth_generation = 0
local FORMATS = {
    { id = 6,  label = "FLAC CD (16-bit / 44.1 kHz)" },
    { id = 5,  label = "MP3 320" },
    { id = 7,  label = "FLAC up to 24/96" },
    { id = 27, label = "FLAC up to 24/192" },
}

local function toast(message)
    message = tostring(message or "")
    if message == "Logged out of Qobuz" or message == "Nothing in this list can be streamed" then
        plugin.show_toast(message)
    elseif message:match("^Logged in as ") then
        plugin.show_toast("Signed in to Qobuz")
    elseif message == "Getting stream links" or message == "Preparing playback" then
        plugin.show_toast("Preparing playback")
    elseif message == "No tracks" or message == "No albums" or message == "No artists" or message == "No playlists" then
        plugin.show_toast(message)
    elseif message == "Quality updated" then
        plugin.show_toast(message)
    elseif message == "Not in this subscription" then
        plugin.show_toast("This track is unavailable")
    else
        plugin.show_toast("Could not complete the request. Try again.")
    end
end

local function url_encode(text)
    return (tostring(text):gsub("[^%w%-%.%_%~]", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

local function stored(key)
    local value = plugin.storage.get(key)
    if value == nil or value == "" then return nil end
    return value
end

local function format_id()
    local saved = tonumber(stored("format_id") or "")
    if saved == 5 or saved == 6 or saved == 7 or saved == 27 then return saved end
    return 6
end

local function format_index()
    local current = format_id()
    for i, format in ipairs(FORMATS) do
        if format.id == current then return i end
    end
    return 1
end

local function clip(text, limit)
    text = tostring(text or "")
    if #text > limit then return text:sub(1, limit - 3) .. "..." end
    return text
end

local function clip_utf8(text, limit)
    text = tostring(text or "")
    if #text <= limit then return text end
    local cut = limit - 3
    while cut > 0 do
        local next_byte = text:byte(cut + 1)
        if not next_byte or next_byte < 128 or next_byte >= 192 then break end
        cut = cut - 1
    end
    return text:sub(1, cut) .. "..."
end

local function api_message(body, status)
    local data = body and plugin.json_decode(body)
    if type(data) == "table" and data.message and data.message ~= "" then
        return tostring(data.message)
    end
    return "Qobuz answered HTTP " .. tostring(status)
end

local function qobuz_get(path, extra_headers, callback)
    local headers = { ["X-App-Id"] = stored("app_id") }
    local token = stored("auth_token")
    if token then headers["X-User-Auth-Token"] = token end
    if extra_headers then
        for name, value in pairs(extra_headers) do headers[name] = value end
    end

    local handle = plugin.http_request({
        url = API .. path,
        headers = headers,
        total_timeout_ms = 20000,
        max_response_bytes = JSON_LIMIT,
    }, function(status, body, err)
        if err then
            callback(nil, err)
            return
        end
        if status ~= 200 then
            callback(nil, api_message(body, status))
            return
        end
        local data, decode_err = plugin.json_decode(body, { max_input_bytes = JSON_LIMIT })
        if type(data) ~= "table" then
            callback(nil, decode_err or "Qobuz sent an unreadable reply")
            return
        end
        if data.status == "error" then
            callback(nil, api_message(body, status))
            return
        end
        callback(data)
    end)
    if not handle then callback(nil, "request unavailable") end
end

local function items_of(node)
    if type(node) ~= "table" then return {} end
    if type(node.items) == "table" then return node.items end
    if node[1] ~= nil then return node end
    return {}
end

local function cover_url(image)
    if type(image) ~= "table" then return "" end
    return image.large or image.extralarge or image.small or image.thumbnail or image.mega or ""
end

local function artist_name(obj)
    if type(obj) ~= "table" then return "" end
    if type(obj.performer) == "table" and obj.performer.name then return obj.performer.name end
    if type(obj.artist) == "table" and obj.artist.name then return obj.artist.name end
    if type(obj.album) == "table" and type(obj.album.artist) == "table" then
        return obj.album.artist.name or ""
    end
    return ""
end

local function track_from(obj, album_hint)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    local album = type(obj.album) == "table" and obj.album or nil
    local hint = album_hint or {}
    local cover = album and cover_url(album.image) or ""
    local rate = tonumber(obj.maximum_sampling_rate) or 0
    if rate > 0 and rate < 1000 then rate = math.floor(rate * 1000 + 0.5) end
    return {
        id = tostring(obj.id),
        title = clip_utf8(obj.title or "Unknown title", 127),
        artist = clip_utf8(artist_name(obj) ~= "" and artist_name(obj) or (hint.artist or "Unknown artist"), 127),
        album = clip_utf8((album and album.title) or hint.title or "", 127),
        album_id = (album and album.id and tostring(album.id)) or hint.id or "",
        cover = cover ~= "" and cover or (hint.cover or ""),
        duration = tonumber(obj.duration) or 0,
        sample_rate = rate,
        bit_depth = tonumber(obj.maximum_bit_depth) or 0,
        streamable = obj.streamable ~= false,
    }
end

local function album_from(obj)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    local artist = ""
    if type(obj.artist) == "table" then artist = obj.artist.name or "" end
    return {
        id = tostring(obj.id),
        title = obj.title or "Unknown album",
        artist = artist,
        cover = cover_url(obj.image),
        count = tonumber(obj.tracks_count) or 0,
    }
end

local function artist_from(obj)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    return {
        id = tostring(obj.id),
        name = obj.name or "Unknown artist",
        count = tonumber(obj.albums_count) or 0,
    }
end

local function playlist_from(obj)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    local image = ""
    if type(obj.images300) == "table" then image = obj.images300[1] or "" end
    if image == "" and type(obj.images150) == "table" then image = obj.images150[1] or "" end
    return {
        id = tostring(obj.id),
        name = obj.name or "Playlist",
        count = tonumber(obj.tracks_count) or 0,
        cover = image,
    }
end

local show_list

local function show_rows(title, rows, on_pick)
    local labels = {}
    for i, row in ipairs(rows) do
        labels[i] = clip(row.label, 140)
    end
    return show_list(title, labels, function(index, list_handle)
        if rows[index] then on_pick(rows[index], list_handle) end
    end)
end

local function parent_showing(parent)
    return parent == nil or plugin.is_list_showing(parent)
end

show_list = function(title, labels, on_select, options)
    local handle
    handle = plugin.show_list(title, labels, function(index)
        on_select(index, handle)
    end, options)
    return handle
end

local function file_url(track, callback)
    local ts = tostring(os.time())
    local secret = stored("app_secret") or ""
    local signed = "trackgetFileUrlformat_id" .. tostring(format_id())
        .. "intentstreamtrack_id" .. track.id .. ts .. secret
    local path = "/track/getFileUrl?app_id=" .. url_encode(stored("app_id"))
        .. "&user_auth_token=" .. url_encode(stored("auth_token") or "")
        .. "&request_ts=" .. ts
        .. "&request_sig=" .. plugin.md5(signed)
        .. "&format_id=" .. tostring(format_id())
        .. "&intent=stream&track_id=" .. url_encode(track.id)
    qobuz_get(path, nil, function(data, err)
        if not data then
            callback(nil, err)
            return
        end
        if not data.url or data.url == "" then
            callback(nil, data.streamable == false and "Not in this subscription" or "No stream link")
            return
        end
        local mime = tostring(data.mime_type or "")
        local codec = "flac"
        if mime:find("mpeg") or mime:find("mp3") or format_id() == 5 then codec = "mp3" end
        local rate = tonumber(data.sampling_rate) or track.sample_rate or 0
        if rate > 0 and rate < 1000 then rate = math.floor(rate * 1000 + 0.5) end
        callback({
            provider = "qobuz",
            track_id = track.id,
            stream_url = data.url,
            title = clip_utf8(track.title, 127),
            artist = clip_utf8(track.artist, 127),
            album = clip_utf8(track.album, 127),
            duration_ms = math.floor((tonumber(data.duration) or track.duration or 0) * 1000),
            artwork_url = track.cover,
            codec = codec,
            sample_rate = rate,
            bit_depth = tonumber(data.bit_depth) or track.bit_depth or 16,
        })
    end)
end

local function play_one(track)
    if track.streamable == false then
        toast("Not in this subscription")
        return
    end
    playback_generation = playback_generation + 1
    local generation = playback_generation
    file_url(track, function(remote, err)
        if generation ~= playback_generation then return end
        if not remote then
            toast(err)
            return
        end
        plugin.play_remote(remote)
    end)
end

local function play_these(tracks)
    playback_generation = playback_generation + 1
    local generation = playback_generation
    local pending = {}
    for i, track in ipairs(tracks) do
        if track.streamable ~= false and #pending < QUEUE_CAP then
            pending[#pending + 1] = track
        end
        if #pending >= QUEUE_CAP then break end
    end
    if #pending == 0 then
        toast("Nothing in this list can be streamed")
        return
    end
    toast("Preparing playback")

    local resolved = {}
    local function step(index)
        if generation ~= playback_generation then return end
        if index > #pending then
            if #resolved == 0 then
                toast("None of these tracks returned a stream link")
                return
            end
            plugin.queue_remote_list(resolved, 1)
            return
        end
        file_url(pending[index], function(remote)
            if generation ~= playback_generation then return end
            if remote then resolved[#resolved + 1] = remote end
            step(index + 1)
        end)
    end
    step(1)
end

local function track_rows(list, album_hint)
    local rows = {}
    for _, obj in ipairs(list) do
        local track = track_from(obj, album_hint)
        if track then
            track.label = track.title .. " - " .. track.artist
            rows[#rows + 1] = track
        end
    end
    return rows
end

local function show_tracks(title, rows)
    if #rows == 0 then
        toast("No tracks")
        return
    end
    local shown = { { label = "Play these", play_all = true } }
    for i, row in ipairs(rows) do shown[#shown + 1] = row end
    show_rows(title, shown, function(row)
        if row.play_all then play_these(rows) else play_one(row) end
    end)
end

local function open_album(album, offset, depth, parent)
    offset = offset or 0
    local generation = browse_generation
    local all, hint = {}, album
    local function fetch(page_offset)
        qobuz_get("/album/get?app_id=" .. url_encode(stored("app_id"))
            .. "&album_id=" .. url_encode(album.id) .. "&limit=" .. PAGE .. "&offset=" .. page_offset,
            nil, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            hint = album_from(data) or hint
            local page = items_of(data.tracks)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < BROWSE_CAP
                and ((hint.count or 0) == 0 or page_offset + PAGE < hint.count) then
                fetch(page_offset + PAGE)
            else
                show_tracks(hint.title, track_rows(all, hint))
            end
        end)
    end
    fetch(offset)
end

local function open_playlist(playlist, offset, depth, parent)
    offset = offset or 0
    local generation = browse_generation
    local all = {}
    local function fetch(page_offset)
        qobuz_get("/playlist/get?app_id=" .. url_encode(stored("app_id"))
            .. "&playlist_id=" .. url_encode(playlist.id)
            .. "&extra=tracks&limit=" .. PAGE .. "&offset=" .. page_offset, nil, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of(data.tracks)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < BROWSE_CAP then fetch(page_offset + PAGE)
            else show_tracks(playlist.name, track_rows(all)) end
        end)
    end
    fetch(offset)
end

local function show_albums(title, albums, depth)
    if #albums == 0 then toast("No albums") return end
    local rows = {}
    for _, album in ipairs(albums) do
        album.label = album.title .. (album.artist ~= "" and (" - " .. album.artist) or "")
        rows[#rows + 1] = album
    end
    show_rows(title, rows, function(album, parent)
        if (depth or 2) < 4 then open_album(album, 0, (depth or 2) + 1, parent) end
    end)
end

-- Fetches up to cap items in pages of PAGE into one list, so paging never
-- adds a screen. done(items) runs only while parent is still in front.
local function fetch_pages(path_for, items_of_page, cap, parent, done)
    local generation, all = browse_generation, {}
    local function fetch(offset)
        qobuz_get(path_for(offset), nil, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of_page(data)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < cap then fetch(offset + PAGE) else done(all) end
        end)
    end
    fetch(0)
end

local function albums_from(list)
    local albums = {}
    for _, obj in ipairs(list) do
        local album = album_from(obj)
        if album then albums[#albums + 1] = album end
    end
    return albums
end

local function search_albums(query, depth, parent)
    fetch_pages(function(offset)
        return "/album/search?app_id=" .. url_encode(stored("app_id"))
            .. "&query=" .. url_encode(query) .. "&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.albums) end, SEARCH_CAP, parent, function(list)
        show_albums("Albums", albums_from(list), depth)
    end)
end

local function search_tracks(query, depth, parent)
    fetch_pages(function(offset)
        return "/track/search?app_id=" .. url_encode(stored("app_id"))
            .. "&query=" .. url_encode(query) .. "&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.tracks) end, SEARCH_CAP, parent, function(list)
        show_tracks("Tracks", track_rows(list))
    end)
end

local function open_artist(artist, offset, depth, parent)
    depth = depth or 3
    local generation, albums = browse_generation, {}
    local function fetch(page_offset, page_number)
        qobuz_get("/artist/get?app_id=" .. url_encode(stored("app_id"))
            .. "&artist_id=" .. url_encode(artist.id)
            .. "&extra=albums&limit=" .. PAGE .. "&offset=" .. page_offset, nil, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of(data.albums)
            for _, obj in ipairs(page) do
                local album = album_from(obj)
                if album then albums[#albums + 1] = album end
            end
            if #page == PAGE and page_number < BROWSE_CAP / PAGE then fetch(page_offset + PAGE, page_number + 1)
            else show_albums(artist.name, albums, depth) end
        end)
    end
    fetch(offset or 0, 1)
end

local function search_artists(query, depth, parent)
    fetch_pages(function(offset)
        return "/artist/search?app_id=" .. url_encode(stored("app_id"))
            .. "&query=" .. url_encode(query) .. "&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.artists) end, SEARCH_CAP, parent, function(list)
        local rows = {}
        for _, obj in ipairs(list) do
            local artist = artist_from(obj)
            if artist then artist.label = artist.name; rows[#rows + 1] = artist end
        end
        if #rows == 0 then toast("No artists"); return end
        show_rows("Artists", rows, function(artist, handle)
            if depth < 4 then open_artist(artist, 0, depth + 1, handle) end
        end)
    end)
end

local function ask_query(title, search, parent)
    -- The text input is still in front while this runs; the search checks
    -- parent when its results arrive.
    local ok, err = plugin.show_text_input(title, nil, false, function(query)
        if not query or query == "" then return end
        search(query, 2, parent)
    end)
    if ok == false then toast(err or "Text input is busy") end
end

local function favorite_tracks(depth, parent)
    fetch_pages(function(offset)
        return "/favorite/getUserFavorites?app_id=" .. url_encode(stored("app_id"))
            .. "&type=tracks&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.tracks) end, BROWSE_CAP, parent, function(list)
        show_tracks("Favorite tracks", track_rows(list))
    end)
end

local function favorite_albums(depth, parent)
    fetch_pages(function(offset)
        return "/favorite/getUserFavorites?app_id=" .. url_encode(stored("app_id"))
            .. "&type=albums&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.albums) end, BROWSE_CAP, parent, function(list)
        show_albums("Favorite albums", albums_from(list), depth)
    end)
end

local function show_playlists(depth, parent)
    fetch_pages(function(offset)
        return "/playlist/getUserPlaylists?app_id=" .. url_encode(stored("app_id"))
            .. "&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.playlists) end, BROWSE_CAP, parent, function(list)
        local rows = {}
        for _, obj in ipairs(list) do
            local playlist = playlist_from(obj)
            if playlist then playlist.label = playlist.name; rows[#rows + 1] = playlist end
        end
        if #rows == 0 then toast("No playlists"); return end
        show_rows("Playlists", rows, function(playlist, handle)
            if depth < 4 then open_playlist(playlist, 0, depth + 1, handle) end
        end)
    end)
end

local function show_new(depth, parent)
    fetch_pages(function(offset)
        return "/album/getFeatured?app_id=" .. url_encode(stored("app_id"))
            .. "&type=new-releases&limit=" .. PAGE .. "&offset=" .. offset
    end, function(data) return items_of(data.albums) end, SEARCH_CAP, parent, function(list)
        show_albums("New releases", albums_from(list), depth)
    end)
end

local function choose_quality(parent)
    if not parent_showing(parent) then return end
    local labels = {}
    for i, format in ipairs(FORMATS) do labels[i] = format.label end
    show_list("Quality", labels, function(index)
        plugin.storage.set("format_id", tostring(FORMATS[index].id))
        toast("Quality updated")
    end, { selected = format_index() })
end

local function logout()
    auth_generation = auth_generation + 1
    playback_generation = playback_generation + 1
    browse_generation = browse_generation + 1
    plugin.storage.delete("auth_token")
    plugin.storage.delete("display_name")
    toast("Logged out of Qobuz")
end

local prompt_keys
local prompt_login

local function show_home()
    show_list("Qobuz", {
        "Search tracks",
        "Search albums",
        "Search artists",
        "New releases",
        "Favorite tracks",
        "Favorite albums",
        "Playlists",
        "Quality",
        "Change app credentials",
        "Sign in or log out",
    }, function(index, handle)
        if index >= 1 and index <= 7 and not stored("auth_token") then
            prompt_login()
            return
        end
        if index == 1 then ask_query("Search tracks", search_tracks, handle)
        elseif index == 2 then ask_query("Search albums", search_albums, handle)
        elseif index == 3 then ask_query("Search artists", search_artists, handle)
        elseif index == 4 then show_new(2, handle)
        elseif index == 5 then favorite_tracks(2, handle)
        elseif index == 6 then favorite_albums(2, handle)
        elseif index == 7 then show_playlists(2, handle)
        elseif index == 8 then choose_quality(handle)
        elseif index == 9 then prompt_keys(false, handle)
        else
            if stored("auth_token") then logout() else prompt_login() end
        end
    end)
end

local function finish_login(data, fallback_name)
    if not data.user_auth_token or data.user_auth_token == "" then
        toast("Qobuz did not return a session")
        return
    end
    local name = fallback_name
    if type(data.user) == "table" then
        name = data.user.display_name or data.user.login or data.user.email or name
    end
    browse_generation = browse_generation + 1
    plugin.storage.set("auth_token", data.user_auth_token)
    plugin.storage.set("display_name", name or "Qobuz")
    toast("Logged in as " .. (name or "Qobuz"))
end

local function do_login(email, password)
    -- The password goes in a header, not the query, and is not written down.
    -- device_manufacturer_id carries the app secret; that is the login form
    -- the stock client sends.
    auth_generation = auth_generation + 1
    local generation = auth_generation
    qobuz_get("/user/login?app_id=" .. url_encode(stored("app_id")), {
        username = email,
        password = password,
        device_manufacturer_id = stored("app_secret"),
    }, function(data, err)
        if generation ~= auth_generation then return end
        if not data then toast(err) return end
        finish_login(data, email)
    end)
end

prompt_login = function()
    local ok, err = plugin.show_text_input("Qobuz email", nil, false, function(email)
        if not email or email == "" then return end
        plugin.show_text_input("Qobuz password", nil, true, function(password)
            if not password or password == "" then return end
            do_login(email, password)
        end)
    end)
    if ok == false then toast(err or "Text input is busy") end
end

prompt_keys = function(show_root, parent)
    plugin.show_text_input("Qobuz app id", stored("app_id"), false, function(app_id)
        if not app_id or app_id == "" then return end
        plugin.show_text_input("Qobuz app secret", nil, true, function(secret)
            if not secret or secret == "" then return end
            auth_generation = auth_generation + 1
            browse_generation = browse_generation + 1
            plugin.storage.set("app_id", app_id)
            plugin.storage.set("app_secret", secret)
            playback_generation = playback_generation + 1
            plugin.storage.delete("auth_token")
            plugin.storage.delete("display_name")
            if show_root and parent == nil then show_home() end
            prompt_login()
        end)
    end)
end

local function open_qobuz()
    if not stored("app_id") or not stored("app_secret") then
        toast("Qobuz does not issue third-party keys. Enter the pair you use.")
        prompt_keys(true, nil)
        return
    end
    if not stored("auth_token") then
        show_home()
        prompt_login()
        return
    end
    show_home()
end

plugin.register_stream_media_tile("Qobuz", open_qobuz, "stream_media/qobuz_row.png")
