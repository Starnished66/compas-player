--[[
Tidal, for a paid account you already have.

Tidal does not take a password from a third-party player. Sign-in is the
device-code flow: this screen shows a short code, a phone opens
link.tidal.com and enters it, and the player polls until that succeeds.
The app id and app secret are yours to supply. Tidal revokes pairs, and
when sign-in starts failing those two values are the first thing to replace.

The access token is a JWT, renewed from the refresh token about a minute
before it expires. Catalogue calls send it as a bearer token.

The Stream Media row uses the player's own stream_media/tidal.png, the same
theme-relative icon path Net Radio and Podcasts pass to
plugin.register_stream_media_tile().

Lossless is the quality that comes back as one FLAC file, which
plugin.play_remote() can stream. Low and High are offered too; if the
manifest is an MP4 rather than a bare FLAC or ADTS AAC, playback stops
with an explanation. Hi-res arrives as DASH segments, and this player
cannot reassemble those, so that quality is not listed.

Stream links expire in a few minutes, a remote stream does not seek, and
it stays 16-bit. "Play these" resolves at most 12 tracks. Copy Tidal.lua
to <SD card>/.plugins/ and restart.
]]

plugin.define({
    id = "community.tidal",
    name = "Tidal",
    version = "1.1.0",
    api_min = 14,
})

if not plugin.has_capability("playback.remote") then
    plugin.show_toast("Tidal needs a player build with remote playback")
    return
end

local API = "https://api.tidal.com/v1"
local AUTH = "https://auth.tidal.com/v1/oauth2"
local IMAGES = "https://resources.tidal.com/images"
local SCOPE = "r_usr w_usr"
local CLIENT_VERSION = "2025.7.16"
local PAGE = 25
local BROWSE_CAP = 475 -- plus the "Play these" row, within show_list's 500 rows
local SEARCH_CAP = 100
local QUEUE_CAP = 12
local JSON_LIMIT = 524288
local QUALITIES = {
    { id = "LOSSLESS", label = "Lossless FLAC (CD)" },
    { id = "HIGH", label = "High (AAC 320)" },
    { id = "LOW", label = "Low (AAC 96)" },
}

local login = nil
local login_timer = nil
local login_busy = false
local login_starting = false
local login_generation = 0
local session_generation = 0
local playback_generation = 0
local browse_generation = 0

local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function toast(message)
    message = tostring(message or "")
    if message == "Logged out of Tidal" or message == "Nothing in this list can be streamed"
        or message == "No tracks" or message == "No albums" or message == "No artists"
        or message == "No playlists" or message == "No results"
        or message == "Not available to stream" or message == "Still waiting for the phone"
        or message == "Quality updated" or message == "This quality is unavailable. Choose Lossless."
        or message == "Sign-in expired. Open the tile to try again." then
        plugin.show_toast(message)
    elseif message:match("^Code: ") then
        plugin.show_toast(message)
    elseif message:match("^Logged in as ") then
        plugin.show_toast("Signed in to Tidal")
    elseif message == "Preparing playback" then
        plugin.show_toast(message)
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

local function quality()
    local saved = stored("quality")
    for _, item in ipairs(QUALITIES) do
        if item.id == saved then return saved end
    end
    return "LOSSLESS"
end

local function quality_index()
    local current = quality()
    for i, item in ipairs(QUALITIES) do
        if item.id == current then return i end
    end
    return 1
end

local function country()
    return stored("country") or "US"
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

local function b64decode(data)
    data = tostring(data or ""):gsub("%s", ""):gsub("=", "")
    if data == "" then return nil end
    local map = {}
    for i = 1, #B64 do map[B64:sub(i, i)] = i - 1 end
    local out = {}
    local acc = 0
    local bits = 0
    for i = 1, #data do
        local value = map[data:sub(i, i)]
        if not value then return nil end
        acc = acc * 64 + value
        bits = bits + 6
        if bits >= 8 then
            bits = bits - 8
            local weight = 2 ^ bits
            out[#out + 1] = string.char(math.floor(acc / weight) % 256)
            acc = acc % weight
        end
    end
    return table.concat(out)
end

local function image_url(uuid, pixels)
    if not uuid or uuid == "" then return "" end
    local slashed = {}
    for piece in tostring(uuid):gmatch("[^-]+") do slashed[#slashed + 1] = piece end
    if #slashed == 0 then return "" end
    return IMAGES .. "/" .. table.concat(slashed, "/") .. "/" .. pixels .. "x" .. pixels .. ".jpg"
end

local function api_message(body, status)
    local data = body and select(1, plugin.json_decode(body))
    if type(data) == "table" then
        if data.userMessage and data.userMessage ~= "" then return tostring(data.userMessage) end
        if data.error_description and data.error_description ~= "" then return tostring(data.error_description) end
        if data.error and data.error ~= "" then return tostring(data.error) end
    end
    return "Tidal answered HTTP " .. tostring(status)
end

local function form_post(url, form, callback)
    local handle = plugin.http_request({
        url = url,
        method = "POST",
        body = form,
        content_type = "application/x-www-form-urlencoded",
        total_timeout_ms = 20000,
        max_response_bytes = 262144,
    }, function(status, body, err)
        if err then
            callback(nil, nil, err)
            return
        end
        local data = nil
        if body and body ~= "" then data = select(1, plugin.json_decode(body)) end
        callback(status, data, nil, body)
    end)
    if not handle then callback(nil, nil, "request unavailable") end
end

local function save_tokens(data, keep_refresh)
    if not data or not data.access_token or data.access_token == "" then return false end
    plugin.storage.set("access_token", data.access_token)
    if data.refresh_token and data.refresh_token ~= "" then
        plugin.storage.set("refresh_token", data.refresh_token)
    elseif not keep_refresh then
        return false
    end
    local expires = tonumber(data.expires_in) or 0
    plugin.storage.set("expires_at", tostring(expires > 0 and (os.time() + expires) or 0))
    return true
end

local function save_user(user)
    if type(user) ~= "table" then return false end
    local id = user.userId or user.user_id
    if id == nil or tostring(id) == "" or tostring(id) == "0" then return false end
    plugin.storage.set("user_id", tostring(id))
    if user.countryCode and user.countryCode ~= "" then
        plugin.storage.set("country", tostring(user.countryCode))
    end
    local name = user.username or user.nickname or user.email or "Tidal"
    plugin.storage.set("display_name", tostring(name))
    return true
end

local function clear_session()
    session_generation = session_generation + 1
    browse_generation = browse_generation + 1
    plugin.storage.delete("access_token")
    plugin.storage.delete("refresh_token")
    plugin.storage.delete("expires_at")
    plugin.storage.delete("user_id")
    plugin.storage.delete("display_name")
end

local function token_fresh()
    local token = stored("access_token")
    if not token then return false end
    local expires = tonumber(stored("expires_at") or "0") or 0
    if expires == 0 then return true end
    return os.time() + 60 < expires
end

local function auth_headers()
    return {
        Authorization = "Bearer " .. (stored("access_token") or ""),
        ["x-tidal-client-version"] = CLIENT_VERSION,
    }
end

local refresh_token
local tidal_get

refresh_token = function(callback)
    local refresh = stored("refresh_token")
    local client_id = stored("client_id")
    local client_secret = stored("client_secret")
    if not refresh or not client_id or not client_secret then
        callback(false, "Not signed in")
        return
    end
    local generation = session_generation
    local form = "grant_type=refresh_token&refresh_token=" .. url_encode(refresh)
        .. "&client_id=" .. url_encode(client_id)
        .. "&client_secret=" .. url_encode(client_secret)
    form_post(AUTH .. "/token", form, function(status, data, err, body)
        if generation ~= session_generation then return end
        if err then callback(false, err) return end
        if status ~= 200 or not save_tokens(data, true) then
            local oauth_error = type(data) == "table" and data.error or ""
            if status == 400 or status == 401 or status == 403 then
                if oauth_error == "invalid_grant" or status == 401 or status == 403 then clear_session() end
            end
            callback(false, api_message(body, status))
            return
        end
        callback(true)
    end)
end

tidal_get = function(path, callback, refreshed)
    local generation = session_generation
    local function send()
        local handle = plugin.http_request({
            url = API .. path,
            headers = auth_headers(),
            total_timeout_ms = 20000,
            max_response_bytes = JSON_LIMIT,
        }, function(status, body, err)
            if generation ~= session_generation then return end
            if err then callback(nil, err) return end
            if status == 401 and not refreshed then
                refresh_token(function(ok, why)
                    if generation ~= session_generation then return end
                    if not ok then callback(nil, why or "Session expired") return end
                    tidal_get(path, callback, true)
                end)
                return
            end
            if status ~= 200 then
                callback(nil, api_message(body, status))
                return
            end
            local data, decode_err = plugin.json_decode(body, { max_input_bytes = JSON_LIMIT })
            if type(data) ~= "table" then
                callback(nil, decode_err or "Tidal sent an unreadable reply")
                return
            end
            callback(data)
        end)
        if not handle then callback(nil, "request unavailable") end
    end

    if token_fresh() then
        send()
    else
        refresh_token(function(ok, why)
            if not ok then callback(nil, why or "Session expired") return end
            send()
        end)
    end
end

local function unwrap(entry)
    if type(entry) == "table" and type(entry.item) == "table" then return entry.item end
    return entry
end

local function items_of(node)
    if type(node) ~= "table" then return {} end
    local source = node.items or node
    if type(source) ~= "table" then return {} end
    local out = {}
    for _, entry in ipairs(source) do out[#out + 1] = unwrap(entry) end
    return out
end

local function person_name(obj)
    if type(obj) ~= "table" then return "" end
    if type(obj.artist) == "table" and obj.artist.name then return obj.artist.name end
    if type(obj.artists) == "table" and type(obj.artists[1]) == "table" then
        return obj.artists[1].name or ""
    end
    return ""
end

local function album_from(obj)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    return {
        id = tostring(obj.id),
        title = obj.title or "Unknown album",
        artist = person_name(obj),
        cover = image_url(obj.cover, 320),
        count = tonumber(obj.numberOfTracks) or 0,
    }
end

local function track_from(obj, album_hint)
    if type(obj) ~= "table" or obj.id == nil then return nil end
    local hint = album_hint or {}
    local album = type(obj.album) == "table" and obj.album or nil
    local album_id = ""
    if album and album.id ~= nil then album_id = tostring(album.id) end
    if album_id == "" then album_id = hint.id or "" end
    local cover = ""
    if album and album.cover then cover = image_url(album.cover, 320) end
    if cover == "" then cover = hint.cover or "" end
    local streamable = true
    if obj.allowStreaming == false or obj.streamReady == false then streamable = false end
    return {
        id = tostring(obj.id),
        title = obj.title or "Unknown title",
        artist = person_name(obj) ~= "" and person_name(obj) or (hint.artist or "Unknown artist"),
        album = (album and album.title) or hint.title or "",
        album_id = album_id,
        cover = cover,
        duration = tonumber(obj.duration) or 0,
        streamable = streamable,
    }
end

local function playlist_from(obj)
    if type(obj) ~= "table" or not obj.uuid then return nil end
    local cover = ""
    if type(obj.squareImage) == "string" then cover = image_url(obj.squareImage, 320) end
    return {
        id = tostring(obj.uuid),
        name = obj.title or "Playlist",
        count = tonumber(obj.numberOfTracks) or 0,
        cover = cover,
    }
end

local show_list

local function show_rows(title, rows, on_pick)
    local labels = {}
    for i, row in ipairs(rows) do
        labels[i] = clip(row.label, 140)
    end
    return show_list(title, labels, function(index, handle)
        if rows[index] then on_pick(rows[index], handle) end
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

local function codec_for(mime, codecs)
    mime = tostring(mime or ""):lower()
    codecs = tostring(codecs or ""):lower()
    if mime:find("flac") or codecs:find("flac") then return "flac" end
    if mime:find("aac") or codecs:find("aac") then return "aac" end
    if mime:find("mpeg") or mime:find("mp3") then return "mp3" end
    if mime:find("mp4") then return nil end
    return nil
end

local function stream_for(track, callback)
    local path = "/tracks/" .. url_encode(track.id)
        .. "/playbackinfopostpaywall?countryCode=" .. url_encode(country())
        .. "&audioquality=" .. quality()
        .. "&playbackmode=STREAM&assetpresentation=FULL"
    tidal_get(path, function(data, err)
        if not data then callback(nil, err) return end
        local manifest_mime = tostring(data.manifestMimeType or "")
        if manifest_mime:find("dash") then
            callback(nil, "This quality is unavailable. Choose Lossless.")
            return
        end
        local decoded = b64decode(data.manifest or "")
        local manifest = decoded and select(1, plugin.json_decode(decoded))
        if type(manifest) ~= "table" then
            callback(nil, "Tidal sent an unreadable manifest")
            return
        end
        if manifest.encryptionType and manifest.encryptionType ~= "" and manifest.encryptionType ~= "NONE" then
            callback(nil, "This track is encrypted")
            return
        end
        local url = type(manifest.urls) == "table" and manifest.urls[1] or nil
        if not url or url == "" then
            callback(nil, "No stream link")
            return
        end
        local stream_mime = tostring(manifest.mimeType or manifest_mime):lower()
        if stream_mime:find("mp4") then
            callback(nil, "This quality is unavailable. Choose Lossless.")
            return
        end
        local codec = codec_for(stream_mime, manifest.codecs)
        if not codec then
            callback(nil, "This quality is unavailable. Choose Lossless.")
            return
        end
        callback({
            provider = "tidal",
            track_id = track.id,
            stream_url = url,
            title = clip_utf8(track.title, 127),
            artist = clip_utf8(track.artist, 127),
            album = clip_utf8(track.album, 127),
            duration_ms = math.floor((track.duration or 0) * 1000),
            artwork_url = track.cover,
            codec = codec,
            sample_rate = tonumber(data.sampleRate) or 0,
            bit_depth = tonumber(data.bitDepth) or 16,
        })
    end)
end

local function play_one(track)
    if track.streamable == false then
        toast("Not available to stream")
        return
    end
    playback_generation = playback_generation + 1
    local generation = playback_generation
    stream_for(track, function(remote, err)
        if generation ~= playback_generation then return end
        if not remote then toast(err) return end
        plugin.play_remote(remote)
    end)
end

local function play_these(tracks)
    playback_generation = playback_generation + 1
    local generation = playback_generation
    local pending = {}
    for _, track in ipairs(tracks) do
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
        stream_for(pending[index], function(remote)
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
    if #rows == 0 then toast("No tracks") return end
    local shown = { { label = "Play these", play_all = true } }
    for _, row in ipairs(rows) do shown[#shown + 1] = row end
    show_rows(title, shown, function(row)
        if row.play_all then play_these(rows) else play_one(row) end
    end)
end

local function open_album(album, offset, depth, parent)
    offset = offset or 0
    local generation, all = browse_generation, {}
    local function fetch(page_offset)
        tidal_get("/albums/" .. url_encode(album.id) .. "/tracks?countryCode=" .. url_encode(country())
            .. "&limit=" .. PAGE .. "&offset=" .. page_offset, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of(data)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < BROWSE_CAP then fetch(page_offset + PAGE)
            else show_tracks(album.title, track_rows(all, album)) end
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

local function open_playlist(playlist, offset, depth, parent)
    offset = offset or 0
    local generation, all = browse_generation, {}
    local function fetch(page_offset)
        tidal_get("/playlists/" .. url_encode(playlist.id) .. "/tracks?countryCode=" .. url_encode(country())
            .. "&limit=" .. PAGE .. "&offset=" .. page_offset, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of(data)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < BROWSE_CAP then fetch(page_offset + PAGE)
            else show_tracks(playlist.name, track_rows(all)) end
        end)
    end
    fetch(offset)
end

-- Fetches up to cap items in pages of PAGE into one list, so paging never
-- adds a screen. done(items, data) runs only while parent is still in front.
local function fetch_pages(path_for, items_of_page, start, cap, parent, done)
    local generation, all = browse_generation, {}
    local function fetch(offset)
        tidal_get(path_for(offset), function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of_page(data)
            for _, obj in ipairs(page) do all[#all + 1] = obj end
            if #page == PAGE and #all < cap then fetch(offset + PAGE) else done(all) end
        end)
    end
    fetch(start)
end

local function search_kind(kind, query, offset, build, title, open, depth, parent)
    depth = depth or 2
    fetch_pages(function(page_offset)
        return "/search/" .. kind .. "?countryCode=" .. url_encode(country())
            .. "&query=" .. url_encode(query) .. "&limit=" .. PAGE .. "&offset=" .. page_offset
    end, function(data)
        return items_of(type(data[kind]) == "table" and data[kind] or data)
    end, offset, SEARCH_CAP, parent, function(list)
        local rows = build(list)
        if #rows == 0 then toast("No results"); return end
        if open then
            show_rows(title, rows, function(row, handle)
                if depth < 4 then open(row, depth + 1, handle) end
            end)
        else
            show_tracks(title, rows)
        end
    end)
end

local function ask_query(title, go, parent)
    -- The text input is still in front while this runs; the search checks
    -- parent when its results arrive.
    local ok, err = plugin.show_text_input(title, nil, false, function(query)
        if not query or query == "" then return end
        go(query, 0, parent)
    end)
    if ok == false then toast(err or "Text input is busy") end
end

local function favorites_path(kind)
    return function(offset)
        return "/users/" .. url_encode(stored("user_id")) .. "/favorites/" .. kind .. "?countryCode="
            .. url_encode(country()) .. "&limit=" .. PAGE .. "&offset=" .. offset
            .. "&order=DATE&orderDirection=DESC"
    end
end

local function favorite_tracks(depth, parent)
    if not stored("user_id") then toast("This account has no user id"); return end
    fetch_pages(favorites_path("tracks"), items_of, 0, BROWSE_CAP, parent, function(list)
        show_tracks("Favorite tracks", track_rows(list))
    end)
end

local function favorite_albums(depth, parent)
    if not stored("user_id") then toast("This account has no user id"); return end
    fetch_pages(favorites_path("albums"), items_of, 0, BROWSE_CAP, parent, function(list)
        local albums = {}
        for _, obj in ipairs(list) do
            local album = album_from(obj)
            if album then albums[#albums + 1] = album end
        end
        show_albums("Favorite albums", albums, depth)
    end)
end

local function show_playlists(depth, parent)
    local user = stored("user_id")
    if not user then toast("This account has no user id"); return end
    fetch_pages(function(offset)
        return "/users/" .. url_encode(user) .. "/playlists?countryCode=" .. url_encode(country())
            .. "&limit=" .. PAGE .. "&offset=" .. offset
    end, items_of, 0, BROWSE_CAP, parent, function(list)
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

local function open_artist(artist, offset, depth, parent)
    depth = depth or 3
    local generation, albums = browse_generation, {}
    local function fetch(page_offset)
        tidal_get("/artists/" .. url_encode(artist.id) .. "/albums?countryCode="
            .. url_encode(country()) .. "&limit=" .. PAGE .. "&offset=" .. page_offset, function(data, err)
            if generation ~= browse_generation or not parent_showing(parent) then return end
            if not data then toast(err); return end
            local page = items_of(data)
            for _, obj in ipairs(page) do
                local album = album_from(obj)
                if album then albums[#albums + 1] = album end
            end
            if #page == PAGE and #albums < BROWSE_CAP then fetch(page_offset + PAGE)
            else show_albums(artist.name, albums, depth) end
        end)
    end
    fetch(offset or 0)
end

local function choose_quality(parent)
    if not parent_showing(parent) then return end
    local labels = {}
    for i, item in ipairs(QUALITIES) do labels[i] = item.label end
    show_list("Quality", labels, function(index)
        plugin.storage.set("quality", QUALITIES[index].id)
        toast("Quality updated")
    end, { selected = quality_index() })
end

local function stop_login(message)
    login_generation = login_generation + 1
    login_starting = false
    if login_timer then
        plugin.clear_interval(login_timer)
        login_timer = nil
    end
    login = nil
    login_busy = false
    if message then toast(message) end
end

local function poll_login()
    if not login or login_busy then return end
    if os.time() > login.deadline then
        stop_login("Sign-in expired. Open the tile to try again.")
        return
    end
    login_busy = true
    local generation = login_generation
    local form = "client_id=" .. url_encode(stored("client_id"))
        .. "&client_secret=" .. url_encode(stored("client_secret"))
        .. "&device_code=" .. url_encode(login.device_code)
        .. "&grant_type=" .. url_encode("urn:ietf:params:oauth:grant-type:device_code")
        .. "&scope=" .. url_encode(SCOPE)
    form_post(AUTH .. "/token", form, function(status, data, err, body)
        if generation ~= login_generation then return end
        login_busy = false
        if not login then return end
        if err then
            stop_login(err)
            return
        end
        if status == 200 and type(data) == "table" then
            if not save_tokens(data, false) or not save_user(data.user) then
                stop_login("Tidal signed in, but the account id was missing")
                return
            end
            session_generation = session_generation + 1
            browse_generation = browse_generation + 1
            local name = stored("display_name") or "Tidal"
            stop_login("Logged in as " .. name)
            return
        end
        local reason = type(data) == "table" and data.error or ""
        local sub = type(data) == "table" and tonumber(data.sub_status or data.subStatus) or 0
        if reason == "authorization_pending" or sub == 1002 then return end
        if reason == "expired_token" or reason == "expired" then
            stop_login("Sign-in expired. Open the tile to try again.")
            return
        end
        stop_login(api_message(body, status))
    end)
end

local show_home -- defined below, used by earlier screens

local function begin_login()
    if login_timer or login_starting then
        toast("Still waiting for the phone")
        return
    end
    stop_login()
    login_starting = true
    login_generation = login_generation + 1
    local generation = login_generation
    local form = "client_id=" .. url_encode(stored("client_id")) .. "&scope=" .. url_encode(SCOPE)
    form_post(AUTH .. "/device_authorization", form, function(status, data, err)
        if generation ~= login_generation then return end
        login_starting = false
        if err then toast(err) return end
        if status ~= 200 or type(data) ~= "table" or not data.deviceCode or not data.userCode then
            toast(type(data) == "table" and (data.error_description or data.error) or "Tidal did not give a code")
            return
        end
        local where = data.verificationUriComplete or data.verificationUri or "link.tidal.com"
        if where:sub(1, 4) ~= "http" then where = "https://" .. where end
        local interval = tonumber(data.interval) or 2
        if interval < 2 then interval = 2 end
        login = {
            device_code = data.deviceCode,
            deadline = os.time() + (tonumber(data.expiresIn) or 300),
        }
        local shown = plugin.show_text_view("Sign in to Tidal",
            "Code: " .. tostring(data.userCode) .. "\n\nOn a phone, open " .. where
                .. " and enter this code.\n\nWaiting for the phone.")
        if shown == false then
            stop_login("Could not show sign-in instructions. Try again.")
            return
        end
        login_timer = plugin.set_interval(interval, poll_login)
    end)
end

local prompt_keys

show_home = function()
    show_list("Tidal", {
        "Search tracks",
        "Search albums",
        "Search artists",
        "Favorite tracks",
        "Favorite albums",
        "Playlists",
        "Quality",
        "Change app credentials",
        "Sign in or log out",
    }, function(index, handle)
        if index >= 1 and index <= 6 and not stored("refresh_token") then
            begin_login()
            return
        end
        if index == 1 then
            ask_query("Search tracks", function(query, offset, parent)
                search_kind("tracks", query, offset, function(list) return track_rows(list) end, "Tracks", nil, 2, parent)
            end, handle)
        elseif index == 2 then
            ask_query("Search albums", function(query, offset, parent)
                search_kind("albums", query, offset, function(list)
                    local rows = {}
                    for _, obj in ipairs(list) do
                        local album = album_from(obj)
                        if album then
                            album.label = album.title .. (album.artist ~= "" and (" - " .. album.artist) or "")
                            rows[#rows + 1] = album
                        end
                    end
                    return rows
                end, "Albums", function(album, depth, list_handle) open_album(album, 0, depth, list_handle) end, 2, parent)
            end, handle)
        elseif index == 3 then
            ask_query("Search artists", function(query, offset, parent)
                search_kind("artists", query, offset, function(list)
                    local rows = {}
                    for _, obj in ipairs(list) do
                        if type(obj) == "table" and obj.id ~= nil then
                            rows[#rows + 1] = {
                                id = tostring(obj.id),
                                name = obj.name or "Unknown artist",
                                label = obj.name or "Unknown artist",
                            }
                        end
                    end
                    return rows
                end, "Artists", function(artist, depth, list_handle) open_artist(artist, 0, depth, list_handle) end, 2, parent)
            end, handle)
        elseif index == 4 then favorite_tracks(2, handle)
        elseif index == 5 then favorite_albums(2, handle)
        elseif index == 6 then show_playlists(2, handle)
        elseif index == 7 then choose_quality(handle)
        elseif index == 8 then prompt_keys()
        else
            if stored("refresh_token") then
                login_generation = login_generation + 1
                login_starting = false
                playback_generation = playback_generation + 1
                clear_session()
                toast("Logged out of Tidal")
            else
                begin_login()
            end
        end
    end)
end

prompt_keys = function(show_root)
    plugin.show_text_input("Tidal client id", stored("client_id"), false, function(client_id)
        if not client_id or client_id == "" then return end
        plugin.show_text_input("Tidal client secret", nil, true, function(secret)
            if not secret or secret == "" then return end
            stop_login()
            plugin.storage.set("client_id", client_id)
            plugin.storage.set("client_secret", secret)
            playback_generation = playback_generation + 1
            clear_session()
            if show_root then show_home() end
            begin_login()
        end)
    end)
end

local function open_tidal()
    if not stored("client_id") or not stored("client_secret") then
        toast("Tidal does not issue third-party keys. Enter the pair you use.")
        prompt_keys(true)
        return
    end
    if login_timer or login_starting then
        toast("Still waiting for the phone")
        return
    end
    if not stored("refresh_token") then
        show_home()
        begin_login()
        return
    end
    show_home()
end

plugin.register_stream_media_tile("Tidal", open_tidal, "stream_media/tidal.png")
