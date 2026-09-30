plugin.define({ id = "example.podcasts", name = "Podcasts", version = "1.0", api_min = 2 })

-- Download-first podcast library stored under <SD>/Podcasts. Put OPML files
-- beside subscriptions.opml to import them from the Podcasts home screen.
-- subscriptions.opml is the export/source list, .catalog caches feeds,
-- .progress.tsv stores resume state, and show folders hold downloaded audio.
-- HTTP playback is limited to MP3 and has no seek or resume; local downloads
-- support the player's formats and resume. The catalog keeps 40 newest
-- episodes per show. Playback speed and chapters are not provided.

local ROOT = plugin.sd_root() .. "/Podcasts"
local CATALOG = ROOT .. "/.catalog"
local SUBSCRIPTIONS = ROOT .. "/subscriptions.opml"
local PROGRESS = ROOT .. "/.progress.tsv"
local USER_AGENT = "CompasPodcasts/1.0"
local MAX_SUBSCRIPTIONS = 300 -- keeps Home well inside show_list's 500-row limit
local MAX_OPML_BYTES = 1048576
local MAX_NEW_PER_SHOW = 5
local subscriptions, catalogs, progress = {}, {}, {}
local fetching, search_running = {}, false
local queue, active_download = {}, nil
local path_to_key, pending_seek = {}, nil
local progress_dirty = false
local cancelled_files = {}
local stream_generation = 0
local last_saved_position, interval_handle = -1, nil
local fetch_feed, feed_response, confirm_unsubscribe, open_episode
local enqueue_download, cancel_download, resolve_redirect
local has_required_capabilities = plugin.has_capability("network.http.async")
    and plugin.has_capability("network.http.download")
    and plugin.has_capability("filesystem.mkdir")

local function trim(s)
    return (tostring(s or ""):gsub("^%s+", ""):gsub("%s+$", ""))
end

local function encode(s)
    return (
        tostring(s or ""):gsub("([^%w%._%- ])", function(c)
            return string.format("%%%02X", string.byte(c))
        end)
    )
end

local function decode(s)
    return ((s or ""):gsub("%%(%x%x)", function(h)
        return string.char(tonumber(h, 16))
    end))
end

local function split_tabs(line)
    local out = {}
    for value in (line .. "\t"):gmatch("(.-)\t") do
        out[#out + 1] = value
    end
    return out
end

-- Writes through a checked writer: a failed write (a full card) must never
-- replace good state with a truncated file.
local function atomic_write(path, writer)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then
        return false
    end
    local checked = {
        write = function(_, ...)
            local ok, err = f:write(...)
            if not ok then
                error(err or "write failed")
            end
        end,
    }
    local ok = pcall(writer, checked)
    local closed = f:close()
    if not ok or not closed then
        os.remove(tmp)
        return false
    end
    if os.rename(tmp, path) then
        return true
    end
    os.remove(tmp)
    return false
end

local function valid_url(url)
    return type(url) == "string"
        and #url <= 2047 -- the native request APIs refuse 2048 bytes and more
        and url:match("^https?://") ~= nil
        and not url:find("[%c%s]")
end

local function remove_dot_segments(path)
    local segments, out = {}, {}
    for segment in (path:sub(2) .. "/"):gmatch("(.-)/") do
        segments[#segments + 1] = segment
    end
    for _, segment in ipairs(segments) do
        if segment == ".." then
            if #out > 0 then
                table.remove(out)
            end
        elseif segment ~= "." then
            out[#out + 1] = segment
        end
    end
    local result = "/" .. table.concat(out, "/")
    local last = segments[#segments]
    if (last == "." or last == "..") and result:sub(-1) ~= "/" then
        result = result .. "/"
    end
    return result
end

-- A Location header resolved against the URL that returned it (RFC 3986
-- section 5.2, for the forms servers send: absolute, //host, /path, ?query,
-- and relative paths with dot segments).
local function resolve_reference(base, reference)
    local ref = trim(reference):gsub("#.*$", "")
    if ref:match("^[Hh][Tt][Tt][Pp][Ss]?://") then
        return ref
    end
    local scheme, authority, path = base:match("^(https?)://([^/?#]*)([^?#]*)")
    if not scheme then
        return nil
    end
    if ref:sub(1, 2) == "//" then
        return scheme .. ":" .. ref
    end
    if path == "" then
        path = "/"
    end
    if ref == "" then
        return (base:gsub("#.*$", ""))
    end
    if ref:sub(1, 1) == "?" then
        return scheme .. "://" .. authority .. path .. ref
    end
    local ref_path, ref_query = ref:match("^([^?]*)(.*)$")
    local merged = ref_path:sub(1, 1) == "/" and ref_path or ((path:match("^(.*/)") or "/") .. ref_path)
    return scheme .. "://" .. authority .. remove_dot_segments(merged) .. ref_query
end

local function cap(s, n)
    s = tostring(s or "")
    if #s <= n then
        return s
    end
    return s:sub(1, n)
end

local function xml_escape(s)
    return (
        tostring(s or "")
            :gsub("&", "&amp;")
            :gsub("<", "&lt;")
            :gsub(">", "&gt;")
            :gsub('"', "&quot;")
            :gsub("'", "&apos;")
    )
end

local function utf8_char(n)
    if n < 0 or n > 1114111 or (n >= 55296 and n <= 57343) then
        return ""
    end
    if n < 128 then
        return string.char(n)
    end
    if n < 2048 then
        return string.char(192 + math.floor(n / 64), 128 + n % 64)
    end
    if n < 65536 then
        return string.char(224 + math.floor(n / 4096), 128 + math.floor(n / 64) % 64, 128 + n % 64)
    end
    return string.char(
        240 + math.floor(n / 262144),
        128 + math.floor(n / 4096) % 64,
        128 + math.floor(n / 64) % 64,
        128 + n % 64
    )
end

local ENTITIES = { amp = "&", lt = "<", gt = ">", quot = '"', apos = "'" }

-- One pass, so "&amp;lt;" becomes the literal "&lt;" rather than "<".
local function decode_entities(s)
    return (
        (s or ""):gsub("&(#?[%w]+);", function(entity)
            local hex = entity:match("^#[xX](%x+)$")
            if hex then
                return #hex <= 6 and utf8_char(tonumber(hex, 16)) or ""
            end
            local dec = entity:match("^#(%d+)$")
            if dec then
                return #dec <= 7 and utf8_char(tonumber(dec)) or ""
            end
            return ENTITIES[entity] or ("&" .. entity .. ";")
        end)
    )
end

-- Linear: each "<" and ">" is looked for once, and an unterminated "<"
-- ends the scan (a pattern such as "<[^>]*>" rescans on every "<").
local function strip_markup(s)
    local out, pos = {}, 1
    while true do
        local a = s:find("<", pos, true)
        if not a then
            out[#out + 1] = s:sub(pos)
            break
        end
        out[#out + 1] = s:sub(pos, a - 1)
        local b = s:find(">", a + 1, true)
        if not b then
            out[#out + 1] = s:sub(a)
            break
        end
        out[#out + 1] = " "
        pos = b + 1
    end
    return table.concat(out)
end

-- Raw element text is cut before cleanup; every field shown is far shorter.
local MAX_FIELD_BYTES = 8192

-- Element text: markup is removed before entities are decoded, so escaped
-- text such as "&lt;Bonus&gt;" survives; CDATA sections are taken as-is
-- apart from the HTML tags podcast feeds put in them.
local function clean_text(s)
    s = s or ""
    local out, pos = {}, 1
    while true do
        local a, b = s:find("<![CDATA[", pos, true)
        out[#out + 1] = decode_entities(strip_markup(s:sub(pos, a and a - 1 or #s)))
        if not a then
            break
        end
        local c = s:find("]]>", b + 1, true)
        out[#out + 1] = decode_entities(strip_markup(s:sub(b + 1, c and c - 1 or #s)))
        if not c then
            break
        end
        pos = c + 3
    end
    return trim((table.concat(out):gsub("[%c%s]+", " ")))
end

-- The next <name ...> tag at or after pos (name may be "/item" for a closing
-- tag), skipping CDATA sections and comments, where show notes may quote
-- tags and a user may comment one out. One forward pass over each "<".
-- While a feed is parsed, every "<" visited counts against this budget, so
-- a feed built to make the lookups rescan (say, a megabyte of "<") fails
-- cleanly instead of running past the callback time limit.
local scan_budget = nil

local function find_tag(body, pos, name)
    local wanted = "<" .. name
    while true do
        local i = body:find("<", pos, true)
        if not i then
            return nil
        end
        if scan_budget then
            scan_budget = scan_budget - 1
            if scan_budget < 0 then
                error("Feed is too complex")
            end
        end
        if body:sub(i, i + 8) == "<![CDATA[" then
            local e = body:find("]]>", i + 9, true)
            if not e then
                return nil
            end
            pos = e + 3
        elseif body:sub(i, i + 3) == "<!--" then
            local e = body:find("-->", i + 4, true)
            if not e then
                return nil
            end
            pos = e + 3
        elseif body:sub(i, i + #wanted - 1) == wanted then
            local after = body:sub(i + #wanted, i + #wanted)
            if after == ">" or after:match("%s") then
                return i
            end
            pos = i + #wanted
        else
            pos = i + 1
        end
    end
end

-- Plain searches for the exact tag name: scanning every tag with a pattern
-- is too slow for items that carry long HTML show notes.
local function tag_text(block, name)
    local a = find_tag(block, 1, name)
    if not a then
        return ""
    end
    local open_end = block:find(">", a + #name + 1, true)
    if not open_end then
        return ""
    end
    local close_start = find_tag(block, open_end + 1, "/" .. name)
    if not close_start then
        return ""
    end
    return clean_text(block:sub(open_end + 1, math.min(close_start - 1, open_end + MAX_FIELD_BYTES)))
end

-- Removes <name ...>...</name> blocks (show notes) before field lookups.
local function strip_blocks(block, name)
    local parts, pos = {}, 1
    while true do
        -- find_tag skips comments and CDATA for both ends, so a quoted tag
        -- neither ends a block early nor starts one.
        local a = find_tag(block, pos, name)
        if not a then
            break
        end
        local b = find_tag(block, a + #name + 1, "/" .. name)
        if not b then
            break
        end
        local e = block:find(">", b, true)
        if not e then
            break
        end
        parts[#parts + 1] = block:sub(pos, a - 1)
        pos = e + 1
    end
    parts[#parts + 1] = block:sub(pos)
    return table.concat(parts)
end

-- End of the tag opened at `start`: the first ">" outside quotes (a quoted
-- attribute value may contain ">"), or nil past MAX_TAG_BYTES.
local function tag_end(s, start)
    local pos, limit = start, start + 16384
    while pos <= limit do
        local i = s:find("[\"'>]", pos)
        if not i or i > limit then
            return nil
        end
        local c = s:sub(i, i)
        if c == ">" then
            return i
        end
        local close = s:find(c, i + 1, true)
        if not close then
            return nil
        end
        pos = close + 1
    end
    return nil
end

-- Linear scan of name=value pairs: an anchored or single-class pattern at
-- each step, never one that can backtrack over a long hostile tag.
local MAX_TAG_BYTES = 16384

local function attr(tag, name)
    if #tag > MAX_TAG_BYTES then
        return nil
    end
    local wanted, i, n = name:lower(), 1, #tag
    while i <= n do
        if scan_budget then
            scan_budget = scan_budget - 1
            if scan_budget < 0 then
                error("File is too complex")
            end
        end
        local s, e = tag:find("[%w_:%-]+", i)
        if not s then
            return nil
        end
        local key = tag:sub(s, e):lower()
        local _, ws = tag:find("^%s*", e + 1)
        local j = ws + 1
        if tag:sub(j, j) ~= "=" then
            i = e + 1
        else
            _, ws = tag:find("^%s*", j + 1)
            j = ws + 1
            local q = tag:sub(j, j)
            local value, next_i
            if q == '"' or q == "'" then
                local close = tag:find(q, j + 1, true)
                if not close then
                    return nil
                end
                value, next_i = tag:sub(j + 1, close - 1), close + 1
            else
                -- Unquoted (invalid XML, but seen): runs to a space or ">",
                -- less the "/" of a self-closing tag.
                local stop = tag:find("[%s>]", j) or (n + 1)
                value, next_i = tag:sub(j, stop - 1), stop
                if tag:sub(stop, stop) == ">" and value:sub(-1) == "/" then
                    value = value:sub(1, -2)
                end
            end
            if key == wanted then
                return decode_entities(value)
            end
            i = math.max(next_i, e + 1)
        end
    end
    return nil
end

local function date_epoch(year, month, day, hour, minute, second, zone)
    local base = os.time({
        year = tonumber(year),
        month = tonumber(month),
        day = tonumber(day),
        hour = tonumber(hour),
        min = tonumber(minute),
        sec = tonumber(second),
        isdst = false,
    })
    local utc_fields = os.date("!*t", base)
    local local_offset = base - os.time(utc_fields)
    local zones = {
        GMT = 0,
        UTC = 0,
        UT = 0,
        EST = -5,
        EDT = -4,
        CST = -6,
        CDT = -5,
        MST = -7,
        MDT = -6,
        PST = -8,
        PDT = -7,
        CET = 1,
        CEST = 2,
        BST = 1,
        JST = 9,
        AEST = 10,
        AEDT = 11,
        NZST = 12,
        NZDT = 13,
    }
    local zone_offset = 0
    local sign, zh, zm = zone:match("^([%+%-])(%d%d):?(%d%d)$")
    if sign then
        zone_offset = (tonumber(zh) * 60 + tonumber(zm)) * 60
        if sign == "-" then
            zone_offset = -zone_offset
        end
    else
        zone_offset = (zones[zone:upper()] or 0) * 3600
    end
    return base + local_offset - zone_offset
end

local MONTHS = {
    jan = 1,
    feb = 2,
    mar = 3,
    apr = 4,
    may = 5,
    jun = 6,
    jul = 7,
    aug = 8,
    sep = 9,
    oct = 10,
    nov = 11,
    dec = 12,
}

local function parse_date(s)
    s = trim(s)
    local y, mo, d, h, mi, se, iso_zone = s:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)[Tt ](%d%d):(%d%d):(%d%d)(.*)$")
    if y then
        iso_zone = trim(iso_zone):gsub("^%.%d+", "")
        if iso_zone == "" or iso_zone == "Z" or iso_zone == "z" then
            iso_zone = "UTC"
        end
        if iso_zone:match("^[%+%-]%d%d:?%d%d$") or iso_zone == "UTC" then
            return date_epoch(y, mo, d, h, mi, se, iso_zone)
        end
        return nil
    end
    -- RFC 822: the weekday and the seconds are both optional.
    s = s:gsub("^%a+,?%s*", "")
    local day, mon, year, hour, minute, rest =
        s:match("^(%d%d?)%s+(%a+)%s+(%d%d%d?%d?)%s+(%d%d?):(%d%d)(.*)$")
    if not day then
        return nil
    end
    local second, zone = rest:match("^:(%d%d)%s*(%S*)")
    if not second then
        second, zone = "0", rest:match("^%s*(%S*)")
    end
    local month = MONTHS[mon:lower():sub(1, 3)]
    if not month then
        return nil
    end
    year = tonumber(year)
    if year < 100 then
        year = year < 50 and year + 2000 or year + 1900
    end
    return date_epoch(year, month, day, hour, minute, second, zone ~= "" and zone or "GMT")
end

local function format_duration(seconds)
    seconds = math.max(0, math.floor(seconds or 0))
    local h, m, s = math.floor(seconds / 3600), math.floor(seconds / 60) % 60, seconds % 60
    if h > 0 then
        return string.format("%d:%02d:%02d", h, m, s)
    end
    return string.format("%d:%02d", m, s)
end

local MAX_DURATION = 100 * 3600

-- Seconds, mm:ss or hh:mm:ss; anything non-finite or absurd counts as unknown.
local function parse_duration(s)
    s = trim(s):gsub(",", ".")
    local value
    local h, m, sec = s:match("^(%d+):(%d%d):(%d%d)")
    if h then
        value = tonumber(h) * 3600 + tonumber(m) * 60 + tonumber(sec)
    else
        m, sec = s:match("^(%d+):(%d%d)")
        if m then
            value = tonumber(m) * 60 + tonumber(sec)
        else
            value = tonumber(s:match("^%d+%.?%d*$") or "")
        end
    end
    if not value or value ~= value or value < 0 or value > MAX_DURATION then
        return 0
    end
    return math.floor(value)
end

local function audio_ext(url, mime)
    local path = (url or ""):match("^https?://[^/]+([^?#]*)") or url or ""
    local ext = path:match("%.([%w]+)$")
    ext = ext and ext:lower()
    local known = { mp3 = true, m4a = true, aac = true, ogg = true, opus = true, flac = true }
    if known[ext] then
        return ext
    end
    local types = {
        ["audio/mpeg"] = "mp3",
        ["audio/mp4"] = "m4a",
        ["audio/x-m4a"] = "m4a",
        ["audio/m4a"] = "m4a",
        ["audio/aac"] = "aac",
        ["audio/ogg"] = "ogg",
        ["audio/opus"] = "opus",
        ["audio/flac"] = "flac",
    }
    return types[(mime or ""):lower()] or "mp3"
end

local function is_mp3_url(url)
    local lower = (url or ""):lower()
    return lower:match("%.mp3$") ~= nil or lower:match("%.mp3[?#]") ~= nil
end

-- Episode identity. A long GUID or URL is hashed whole rather than cut
-- short, so two episodes sharing a long prefix never merge their state.
local function episode_identity(s)
    if #s <= 200 then
        return s
    end
    return "md5:" .. plugin.md5(s)
end

local function parse_rss(body, feed_url)
    if body:sub(1, 2) == string.char(31, 139) then
        return nil, "Feed is compressed"
    end
    local prefix = body:sub(1, 8192):lower()
    if
        prefix:find("<feed", 1, true)
        and not prefix:find("<rss", 1, true)
        and not prefix:find("<channel", 1, true)
    then
        return nil, "Atom feeds are not supported"
    end
    -- An HTML login or error page must not replace a show's episodes.
    if not prefix:find("<rss", 1, true) and not prefix:find("<channel", 1, true) then
        return nil, "Not a podcast feed"
    end
    local function find_item(start)
        return find_tag(body, start, "item")
    end
    local first_item = find_item(1) or (#body + 1)
    local channel = strip_blocks(body:sub(1, first_item - 1), "image")
    local title = tag_text(channel, "title")
    local author = tag_text(channel, "itunes:author")
    if author == "" then
        author = tag_text(channel, "author")
    end
    local items, pos = {}, 1
    -- Items without audio do not count toward the 40, so the number looked at
    -- is capped too.
    local inspected = 0
    while #items < 40 and inspected < 400 do
        inspected = inspected + 1
        local a = find_item(pos)
        if not a then
            break
        end
        local open_end = body:find(">", a, true)
        if not open_end then
            break
        end
        local close_a = find_tag(body, open_end + 1, "/item")
        if not close_a then
            break
        end
        local close_end = body:find(">", close_a, true)
        if not close_end then
            break
        end
        local block = body:sub(a, close_end)
        block = strip_blocks(block, "content:encoded")
        block = strip_blocks(block, "description")
        block = strip_blocks(block, "itunes:summary")
        local item_title = tag_text(block, "title")
        if item_title == "" then
            item_title = tag_text(block, "itunes:title")
        end
        local item = { title = cap(item_title, 200) }
        item.pub_date = tag_text(block, "pubDate")
        item.epoch = parse_date(item.pub_date)
        item.date = item.epoch and os.date("!%Y-%m-%d", item.epoch) or ""
        item.duration = parse_duration(tag_text(block, "itunes:duration"))
        if item.duration == 0 then
            item.duration = parse_duration(tag_text(block, "duration"))
        end
        local guid = tag_text(block, "guid")
        local enc_pos = 1
        while true do
            local ea = find_tag(block, enc_pos, "enclosure")
            if not ea then
                break
            end
            local eb = tag_end(block, ea)
            if not eb then
                break
            end
            local etag = block:sub(ea, eb)
            local url, mime = attr(etag, "url"), attr(etag, "type")
            if mime == "" then
                mime = nil
            end
            local ext = audio_ext(url, mime)
            local enclosure_ext = url
                and ((url:match("^https?://[^/]+([^?#]*)") or url):lower():match("%.([%w]+)$"))
            local audio_extensions =
                { mp3 = true, m4a = true, aac = true, ogg = true, opus = true, flac = true }
            local is_audio = (mime and mime:lower():match("^audio/"))
                or (not mime and audio_extensions[enclosure_ext])
            if valid_url(url) and is_audio then
                item.url, item.mime = url, cap(mime, 128)
                item.length = tonumber(attr(etag, "length")) or 0
                item.ext = ext
                break
            end
            enc_pos = eb + 1
        end
        if item.url then
            item.guid = episode_identity(guid ~= "" and guid or item.url)
            item.key = plugin.md5(item.guid)
            item.feed_url = feed_url
            item.title = item.title ~= "" and item.title or "Untitled episode"
            items[#items + 1] = item
        end
        pos = close_end + 1
    end
    table.sort(items, function(a, b)
        if not a.epoch then
            return false
        end
        if not b.epoch then
            return true
        end
        return a.epoch > b.epoch
    end)
    return { title = cap(title, 200), author = cap(author, 200), feed_url = feed_url, items = items }
end

local function show_key(url)
    return plugin.md5(url):sub(1, 16)
end
local function catalog_path(key)
    return CATALOG .. "/" .. key .. ".tsv"
end

local function save_catalog(show)
    return atomic_write(catalog_path(show.key), function(f)
        f:write(
            "S\t",
            encode(show.title),
            "\t",
            encode(show.author),
            "\t",
            encode(show.feed_url),
            "\t",
            tostring(os.time()),
            "\n"
        )
        for i = 1, math.min(40, #show.items) do
            local e = show.items[i]
            f:write(
                "E\t",
                encode(e.guid),
                "\t",
                encode(e.title),
                "\t",
                encode(e.url),
                "\t",
                encode(e.mime),
                "\t",
                tostring(e.length or 0),
                "\t",
                tostring(e.duration or 0),
                "\t",
                tostring(e.epoch or 0),
                "\t",
                encode(e.date),
                "\n"
            )
        end
    end)
end

local function load_catalog(key, title, url)
    local f = io.open(catalog_path(key), "r")
    if not f then
        return nil
    end
    local show, count = nil, 0
    for line in f:lines() do
        if #line > 10000 then
            break
        end
        local p = split_tabs(line)
        if p[1] == "S" then
            show = {
                key = key,
                title = decode(p[2]),
                author = decode(p[3]),
                feed_url = decode(p[4]),
                refreshed = tonumber(p[5]) or 0,
                items = {},
            }
        elseif p[1] == "E" and show and count < 40 then
            count = count + 1
            local urlv = decode(p[4])
            if valid_url(urlv) then
                show.items[#show.items + 1] = {
                    guid = decode(p[2]),
                    title = decode(p[3]),
                    url = urlv,
                    mime = decode(p[5]),
                    length = tonumber(p[6]) or 0,
                    duration = tonumber(p[7]) or 0,
                    epoch = tonumber(p[8]) or nil,
                    date = decode(p[9]),
                    ext = audio_ext(urlv, decode(p[5])),
                    key = plugin.md5(decode(p[2])),
                }
            end
        end
    end
    f:close()
    if show then
        show.title = show.title ~= "" and show.title or title
        show.feed_url = show.feed_url ~= "" and show.feed_url or url
    end
    return show
end

local function save_progress()
    local stateless = {}
    for k, p in pairs(progress) do
        if not (p.position ~= 0 or p.played or p.new or p.path ~= "" or p.last_played ~= 0) then
            stateless[#stateless + 1] = k
        end
    end
    for _, k in ipairs(stateless) do
        progress[k] = nil
    end
    local ok = atomic_write(PROGRESS, function(f)
        for k, p in pairs(progress) do
            do
                f:write(
                    "P\t",
                    encode(k),
                    "\t",
                    tostring(math.floor(p.position or 0)),
                    "\t",
                    tostring(math.floor(p.duration or 0)),
                    "\t",
                    p.played and "1" or "0",
                    "\t",
                    p.new and "1" or "0",
                    "\t",
                    encode(p.path),
                    "\t",
                    tostring(p.last_played or 0),
                    "\t",
                    encode(p.title),
                    "\t",
                    encode(p.date),
                    "\t",
                    tostring(p.epoch or 0),
                    "\n"
                )
            end
        end
    end)
    -- The index follows memory, not the file, and a failed write (a full
    -- card) is retried from the tick.
    path_to_key = {}
    for k, p in pairs(progress) do
        if p.path and p.path ~= "" then
            path_to_key[p.path] = k
        end
    end
    progress_dirty = not ok
    return ok
end

local function load_progress()
    local f = io.open(PROGRESS, "r")
    if not f then
        return
    end
    for line in f:lines() do
        if #line < 8192 then
            local p = split_tabs(line)
            if p[1] == "P" and p[2] then
                local k = decode(p[2])
                progress[k] = {
                    position = tonumber(p[3]) or 0,
                    duration = tonumber(p[4]) or 0,
                    played = p[5] == "1",
                    new = p[6] == "1",
                    path = decode(p[7]),
                    last_played = tonumber(p[8]) or 0,
                    title = decode(p[9]),
                    date = decode(p[10]),
                    epoch = tonumber(p[11]) or 0,
                }
            end
        end
    end
    f:close()
    for k, p in pairs(progress) do
        if p.path ~= "" then
            path_to_key[p.path] = k
        end
    end
end

local function save_subscriptions()
    return atomic_write(SUBSCRIPTIONS, function(f)
        f:write(
            '<?xml version="1.0" encoding="UTF-8"?>\n<opml version="2.0"><head><title>Podcasts</title></head><body>\n'
        )
        for _, s in ipairs(subscriptions) do
            f:write(
                '<outline type="rss" text="',
                xml_escape(s.title),
                '" xmlUrl="',
                xml_escape(s.url),
                '"/>\n'
            )
        end
        f:write("</body></opml>\n")
    end)
end

local function load_subscriptions()
    local f = io.open(SUBSCRIPTIONS, "r")
    if not f then
        return
    end
    local body = f:read(MAX_OPML_BYTES) or ""
    f:close()
    scan_budget = 400000
    local ok = pcall(function()
        local seen, pos = {}, 1
        while #subscriptions < MAX_SUBSCRIPTIONS do
            local a = find_tag(body, pos, "outline")
            if not a then
                break
            end
            local b = tag_end(body, a)
            if not b then
                break
            end
            local tag = body:sub(a, b)
            local url = attr(tag, "xmlUrl") or attr(tag, "xmlurl")
            local title = attr(tag, "text") or attr(tag, "title")
            if valid_url(url) and not seen[url] then
                seen[url] = true
                subscriptions[#subscriptions + 1] =
                    { url = url, title = cap(title or url, 200), key = show_key(url) }
            end
            pos = b + 1
        end
    end)
    scan_budget = nil
    if not ok then
        -- Keep whatever loaded before the limit; the file is rewritten on
        -- the next change.
        print("[Podcasts] subscriptions.opml is too complex; loaded " .. #subscriptions .. " shows")
    end
end

local function sort_subscriptions()
    table.sort(subscriptions, function(a, b)
        return a.title:lower() < b.title:lower()
    end)
end

local function find_subscription(url)
    for _, s in ipairs(subscriptions) do
        if s.url == url then
            return s
        end
    end
end

local function show_by_key(key)
    for _, s in ipairs(subscriptions) do
        if s.key == key then
            return s
        end
    end
end

local function progress_key(show, episode)
    return show.key .. "|" .. episode.guid
end

local function episode_progress(show, episode)
    local k = progress_key(show, episode)
    progress[k] = progress[k]
        or {
            position = 0,
            duration = episode.duration or 0,
            played = false,
            new = false,
            path = "",
            last_played = 0,
        }
    return progress[k], k
end

-- A single safe path component. Leading and trailing dots and spaces go
-- too, so feed data can never produce "." or ".." (or a name FAT rejects).
local function sanitized(s, fallback)
    s = tostring(s or ""):gsub('[/\\:*?"<>|]', " "):gsub("%c", " "):gsub("%s+", " ")
    s = s:gsub("^[%s%.]+", ""):gsub("[%s%.]+$", "")
    while #s > 80 do
        s = s:sub(1, #s - 1)
        while #s > 0 and s:byte(-1) >= 128 and s:byte(-1) < 192 do
            s = s:sub(1, -2)
        end
        if #s > 0 and s:byte(-1) >= 192 then
            s = s:sub(1, -2)
        end
    end
    s = s:gsub("[%s%.]+$", "")
    return s ~= "" and s or fallback
end

local function downloaded(show, episode)
    local p = progress[progress_key(show, episode)]
    if p and p.path and p.path ~= "" then
        local f = io.open(p.path, "rb")
        if f then
            f:close()
            return p.path
        end
    end
end

local function queued(show, episode)
    local key = progress_key(show, episode)
    if active_download and active_download.key == key then
        return true
    end
    for _, item in ipairs(queue) do
        if item.key == key then
            return true
        end
    end
    return false
end

local function request_options(url, extra)
    local headers = { ["User-Agent"] = USER_AGENT }
    if extra then
        for k, v in pairs(extra) do
            headers[k] = v
        end
    end
    return {
        url = url,
        headers = headers,
        verify_tls = true,
        connect_timeout_ms = 10000,
        read_timeout_ms = 20000,
        total_timeout_ms = 60000,
    }
end

-- Starts an async request; argument errors raise in the native API, so they
-- come back here as (nil, message) like a full request pool does.
local function start_request(fn, ...)
    local ok, handle, err = pcall(fn, ...)
    if not ok then
        return nil, tostring(handle)
    end
    return handle, err
end

local function pool_busy(err)
    return type(err) == "string" and err:find("too many active", 1, true) ~= nil
end

-- Work that must wait for the next tick: a request started inside another
-- request's callback cannot use the pool slot that request still holds.
local deferred = {}

local function defer(fn)
    deferred[#deferred + 1] = fn
end

local function run_deferred()
    local jobs = deferred
    deferred = {}
    for _, fn in ipairs(jobs) do
        fn()
    end
end

-- Starts a request now or, while the shared pool is full, from later ticks
-- (a finished request frees its slot only after its callback returns).
local function request_with_retry(opts, on_done, on_fail, tries)
    local handle, err = start_request(plugin.http_request, opts, on_done)
    if handle then
        return
    end
    tries = tries or 0
    if pool_busy(err) and tries < 30 then
        defer(function()
            request_with_retry(opts, on_done, on_fail, tries + 1)
        end)
    else
        on_fail(err)
    end
end

-- A few catalogs stay in memory; the rest are read from the card when opened.
local CATALOG_CACHE_SIZE = 4
local catalog_order = {}

local function remember_catalog(key, catalog)
    for i, k in ipairs(catalog_order) do
        if k == key then
            table.remove(catalog_order, i)
            break
        end
    end
    catalogs[key] = catalog
    if catalog then
        catalog_order[#catalog_order + 1] = key
        while #catalog_order > CATALOG_CACHE_SIZE do
            catalogs[table.remove(catalog_order, 1)] = nil
        end
    end
end

local function get_catalog(show)
    local catalog = catalogs[show.key] or load_catalog(show.key, show.title, show.url)
    remember_catalog(show.key, catalog)
    return catalog
end

local function open_show(show)
    local catalog = get_catalog(show)
    if catalog then
        catalog.title = show.title
        catalog.url = show.url
        catalog.key = show.key
    end
    if catalog then
        local labels, episodes = { "Refresh episodes" }, {}
        for i, episode in ipairs(catalog.items) do
            episodes[i] = episode
            local p = progress[progress_key(show, episode)]
            local mark = p and p.played and "v "
                or (
                    p and p.position > 0 and "> " .. format_duration(p.position) .. " "
                    or (p and p.new and "* " or "")
                )
            local suffix = downloaded(show, episode) and " [saved]"
                or (queued(show, episode) and " [downloading]" or "")
            labels[#labels + 1] = cap(mark .. episode.title .. suffix, 200)
        end
        labels[#labels + 1] = "Unsubscribe"
        plugin.show_list(show.title, labels, function(index)
            if index == 1 then
                fetch_feed(show, false)
            elseif index == #labels then
                confirm_unsubscribe(show)
            else
                open_episode(show, episodes[index - 1])
            end
        end)
        return
    end
    fetch_feed(show, true)
end

local function header_value(headers, name)
    for k, v in pairs(headers or {}) do
        if k:lower() == name then
            return v
        end
    end
end

-- Every redirect is followed here, one hop at a time, so each Location is
-- resolved against the URL that sent it (the callback does not say which
-- URL the native client ended on). A feed over 1 MiB is retried once as
-- a Range request for its first 1 MiB (a chunked body reports io_error
-- instead of response_too_large when it overflows).
local function request_feed(show, first_open)
    if fetching[show.key] then
        return
    end
    fetching[show.key] = true
    local function failed(start_err)
        fetching[show.key] = nil
        plugin.show_toast(start_err or "Could not start feed request")
    end
    local function fetch(url, hops, ranged)
        local opts = request_options(url, ranged and { Range = "bytes=0-1048575" } or nil)
        opts.redirect_limit, opts.max_response_bytes = 0, 1048576
        request_with_retry(opts, function(status, body, err, headers)
            local location = status and status >= 300 and status < 400 and header_value(headers, "location")
            if location then
                local next_url = resolve_reference(url, location)
                if hops >= 8 then
                    fetching[show.key] = nil
                    plugin.show_toast("Too many redirects")
                    return
                end
                if url:match("^https://") and next_url and next_url:match("^http://") then
                    fetching[show.key] = nil
                    plugin.show_toast("Refused a redirect from https to http")
                    return
                end
                if valid_url(next_url) then
                    defer(function()
                        fetch(next_url, hops + 1, ranged)
                    end)
                    return
                end
            end
            if not ranged and (err == "response_too_large" or err == "io_error") then
                defer(function()
                    fetch(url, hops, true)
                end)
                return
            end
            fetching[show.key] = nil
            feed_response(show, first_open, status, body, err)
        end, failed)
    end
    fetch(show.url, 0, false)
end

feed_response = function(show, first_open, status, body, err)
    if err then
        plugin.show_toast(err)
        return
    end
    if status < 200 or status >= 300 then
        plugin.show_toast("HTTP " .. tostring(status))
        return
    end
    scan_budget = 400000
    local ok, result, parse_err = pcall(parse_rss, body or "", show.url)
    scan_budget = nil
    body = nil
    collectgarbage("step")
    if not ok then
        plugin.show_toast(
            tostring(result):find("too complex", 1, true) and "Feed is too complex" or "Could not parse feed"
        )
        return
    end
    if not result then
        plugin.show_toast(parse_err or "Could not parse feed")
        return
    end
    local prior = get_catalog(show)
    if #result.items == 0 and prior and #prior.items > 0 then
        plugin.show_toast("The feed returned no episodes; kept the saved list")
        return
    end
    local old = {}
    if prior then
        for _, e in ipairs(prior.items) do
            old[e.guid] = true
        end
    end
    result.key, result.url = show.key, show.url
    if result.title == "" then
        result.title = show.title
    end
    local present, added = {}, 0
    local new_marked = 0
    for _, e in ipairs(result.items) do
        local p = progress[progress_key(show, e)]
        if p and p.new then
            new_marked = new_marked + 1
        end
    end
    -- Progress records are created only for episodes that get state.
    for i, e in ipairs(result.items) do
        present[e.guid] = true
        local is_new = prior and not old[e.guid] or (not prior and i <= 3)
        if is_new then
            if prior then
                added = added + 1
            end
            -- At most the newest few per show carry the flag, so hundreds of
            -- shows cannot fill memory with new-episode records.
            if new_marked < MAX_NEW_PER_SHOW then
                episode_progress(show, e).new = true
                new_marked = new_marked + 1
            end
        end
    end
    local prefix = show.key .. "|"
    for k, p in pairs(progress) do
        if k:sub(1, #prefix) == prefix then
            local guid = k:sub(#prefix + 1)
            if not present[guid] then
                -- Out of the catalog: no longer counted as new, and forgotten
                -- unless it is downloaded or partly heard.
                p.new = false
                local kept_file = p.path and p.path ~= ""
                if not kept_file and (p.played or (p.position or 0) == 0) then
                    progress[k] = nil
                end
            end
        end
    end
    remember_catalog(show.key, result)
    show.title = result.title
    for _, sub in ipairs(subscriptions) do
        if sub.key == show.key then
            sub.title = show.title
        end
    end
    save_catalog(result)
    save_subscriptions()
    save_progress()
    if first_open then
        open_show(show)
    else
        plugin.show_toast(added > 0 and (added .. " new episodes") or "No new episodes")
    end
end

-- Feed completion callbacks deliberately stay on screen only for the first open.
fetch_feed = function(show, first_open)
    plugin.show_toast("Loading...")
    request_feed(show, first_open)
end

local function ask_unsubscribe(show)
    plugin.show_list("Unsubscribe?", { "Unsubscribe " .. cap(show.title, 180), "Cancel" }, function(index)
        if index ~= 1 then
            return
        end
        local removed_at, removed
        for i, s in ipairs(subscriptions) do
            if s.key == show.key then
                removed_at, removed = i, table.remove(subscriptions, i)
                break
            end
        end
        if not save_subscriptions() then
            if removed then
                table.insert(subscriptions, removed_at, removed)
            end
            plugin.show_toast("Could not save subscriptions; the card may be full")
            return
        end
        -- The cache goes only once the removal is on the card.
        os.remove(catalog_path(show.key))
        remember_catalog(show.key, nil)
        plugin.show_toast("Unsubscribed; downloads kept")
    end)
end

confirm_unsubscribe = ask_unsubscribe

open_episode = function(show, episode)
    local p, key = episode_progress(show, episode)
    p.new = false
    save_progress()
    -- Saving drops records without state, so every action takes a fresh one.
    local function state()
        return episode_progress(show, episode)
    end
    local labels, actions = {}, {}
    local path = downloaded(show, episode)
    local is_queued = queued(show, episode)
    local function add(label, fn)
        labels[#labels + 1] = label
        actions[#actions + 1] = fn
    end
    if path then
        add("Play", function()
            local current = state()
            -- A replay of a finished episode starts over and can be resumed.
            if current.played then
                current.played = false
                current.position = 0
            end
            current.last_played = os.time()
            save_progress()
            plugin.play_file(path)
        end)
    end
    if not path and not is_queued and episode.url ~= "" then
        add("Download", function()
            enqueue_download(show, episode)
        end)
    end
    if is_queued then
        add("Cancel download", function()
            cancel_download(key)
        end)
    end
    if
        not path
        and episode.url ~= ""
        and ((episode.mime or ""):lower() == "audio/mpeg" or is_mp3_url(episode.url))
    then
        add("Stream", function()
            -- Any newer stream or track makes this request stale.
            stream_generation = stream_generation + 1
            local generation = stream_generation
            local function stale()
                return generation ~= stream_generation
            end
            resolve_redirect(episode.url, function(url, err)
                if stale() then
                    return
                end
                if err then
                    plugin.show_toast(err)
                else
                    plugin.play_file(url)
                end
            end, nil, stale)
        end)
    end
    local mark_played = not p.played
    add(mark_played and "Mark as played" or "Mark as unplayed", function()
        local current = state()
        current.played = mark_played
        current.position = 0
        save_progress()
        plugin.show_toast(current.played and "Marked as played" or "Marked as unplayed")
    end)
    if path then
        add("Delete download", function()
            os.remove(path)
            local current = state()
            current.path = ""
            current.position = 0
            save_progress()
            plugin.show_toast("Download deleted")
        end)
    end
    labels[#labels + 1] = (episode.date ~= "" and episode.date or "Undated")
        .. " - "
        .. format_duration(episode.duration)
    actions[#labels] = function() end
    plugin.show_list(cap(episode.title, 200), labels, function(index)
        actions[index]()
    end)
end

-- defer_save lets an import add many shows and write the list once.
local function add_subscription(url, title, defer_save)
    if not valid_url(url) then
        plugin.show_toast("Enter an http(s) feed URL")
        return nil
    end
    local exists = find_subscription(url)
    if exists then
        return exists, true
    end
    if #subscriptions >= MAX_SUBSCRIPTIONS then
        plugin.show_toast("Subscription limit reached (" .. MAX_SUBSCRIPTIONS .. ")")
        return nil
    end
    local show = { url = url, title = cap(title ~= "" and title or url, 200), key = show_key(url) }
    subscriptions[#subscriptions + 1] = show
    if not defer_save then
        sort_subscriptions()
        if not save_subscriptions() then
            for i, s in ipairs(subscriptions) do
                if s == show then
                    table.remove(subscriptions, i)
                    break
                end
            end
            plugin.show_toast("Could not save subscriptions; the card may be full")
            return nil
        end
    end
    return show, false
end

local function open_added(show, already)
    if already then
        plugin.show_toast("Already subscribed")
    end
    open_show(show)
end

local function parse_attrs_for_opml(xml)
    local found, seen, pos = {}, {}, 1
    while #found < MAX_SUBSCRIPTIONS do
        local a = find_tag(xml, pos, "outline")
        if not a then
            break
        end
        local b = tag_end(xml, a)
        if not b then
            break
        end
        local tag = xml:sub(a, b)
        local url = attr(tag, "xmlUrl") or attr(tag, "xmlurl")
        local title = attr(tag, "text") or attr(tag, "title") or url
        if valid_url(url) and not seen[url] then
            seen[url] = true
            found[#found + 1] = { url = url, title = cap(title, 200) }
        end
        pos = b + 1
    end
    return found
end

local function import_file(path)
    local f = io.open(path, "r")
    if not f then
        plugin.show_toast("Could not read OPML")
        return
    end
    local xml = f:read(MAX_OPML_BYTES) or ""
    f:close()
    scan_budget = 400000
    local parsed, imported = pcall(parse_attrs_for_opml, xml)
    scan_budget = nil
    xml = nil
    if not parsed then
        plugin.show_toast("Could not import: the OPML file is too complex")
        return
    end
    local n = 0
    local before = #subscriptions
    for _, s in ipairs(imported) do
        if #subscriptions >= MAX_SUBSCRIPTIONS then
            break
        end
        local existed = find_subscription(s.url) ~= nil
        local added = add_subscription(s.url, s.title, true)
        if added and not existed then
            n = n + 1
        end
    end
    if not save_subscriptions() then
        -- Nothing was written: drop the additions so memory matches the card.
        for i = #subscriptions, before + 1, -1 do
            table.remove(subscriptions, i)
        end
        plugin.show_toast("Could not save subscriptions; the card may be full")
        return
    end
    sort_subscriptions()
    plugin.show_toast("Imported " .. n .. " podcasts")
end

local function pick_import()
    local files = {}
    for _, e in ipairs(plugin.list_dir(ROOT)) do
        if not e.dir and e.name:lower():match("%.opml$") and e.name ~= "subscriptions.opml" then
            files[#files + 1] = e.name
        end
    end
    table.sort(files)
    if #files == 0 then
        plugin.show_toast("No OPML files found in Podcasts")
        return
    end
    plugin.show_list("Import OPML", files, function(index)
        import_file(ROOT .. "/" .. files[index])
    end)
end

local function url_encode(s)
    return (
        tostring(s):gsub("([^%w%-_%.~])", function(c)
            return string.format("%%%02X", string.byte(c))
        end)
    )
end

local function show_search_results(results)
    if #results == 0 then
        plugin.show_toast("No podcasts found")
        return
    end
    local labels = {}
    for i, r in ipairs(results) do
        labels[i] = cap(r.title .. (r.author ~= "" and (" - " .. r.author) or ""), 200)
    end
    plugin.show_list("Podcast Search", labels, function(index)
        local r = results[index]
        local show, already = add_subscription(r.url, r.title)
        if show then
            open_added(show, already)
        end
    end)
end

local function search_url(query, fallback)
    local url
    if fallback then
        url = "https://api.fyyd.de/0.2/search/podcast?term=" .. url_encode(query) .. "&count=10"
    else
        url = "https://itunes.apple.com/search?term="
            .. url_encode(query)
            .. "&media=podcast&entity=podcast&limit=15"
    end
    -- The fyyd fallback always starts from a later tick (see defer).
    local function fall_back()
        defer(function()
            search_url(query, true)
        end)
    end
    request_with_retry(request_options(url), function(status, body, err)
        if err or status < 200 or status >= 300 then
            if not fallback then
                fall_back()
            else
                search_running = false
                plugin.show_toast(err or ("HTTP " .. tostring(status)))
            end
            return
        end
        local ok, data = pcall(plugin.json_decode, body or "")
        body = nil
        collectgarbage("step")
        local results = {}
        local list = ok and type(data) == "table" and (fallback and data.data or data.results) or nil
        if type(list) == "table" then
            for i = 1, math.min(#list, 20) do
                local row = list[i]
                if type(row) == "table" then
                    local u = fallback and row.xmlURL or row.feedUrl
                    local title = fallback and row.title or row.collectionName
                    local author = fallback and row.author or row.artistName
                    if valid_url(u) then
                        results[#results + 1] = {
                            url = u,
                            title = cap(clean_text(cap(type(title) == "string" and title or u, 2048)), 200),
                            author = cap(
                                clean_text(cap(type(author) == "string" and author or "", 2048)),
                                200
                            ),
                        }
                    end
                end
            end
        end
        if #results == 0 and not fallback then
            fall_back()
            return
        end
        search_running = false
        show_search_results(results)
    end, function(start_err)
        if fallback then
            search_running = false
            plugin.show_toast(start_err or "Could not start search")
        else
            fall_back()
        end
    end)
end

local function start_search()
    if search_running then
        return
    end
    local opened = plugin.show_text_input("Search podcasts", nil, false, function(query)
        query = trim(query)
        if query == "" then
            return
        end
        if search_running then
            return
        end
        search_running = true
        search_url(query, false)
    end)
    if opened == false then
        plugin.show_toast("Text input busy")
    end
end

local function add_feed_input()
    local opened = plugin.show_text_input("Add feed URL", "https://", false, function(url)
        url = trim(url)
        if not valid_url(url) then
            plugin.show_toast("Enter an http(s) feed URL")
            return
        end
        local show, already = add_subscription(url, url)
        if show then
            open_added(show, already)
        end
    end)
    if opened == false then
        plugin.show_toast("Text input busy")
    end
end

-- Follows the redirect chain one request per tick: HEAD, or a one-byte GET
-- where HEAD is refused. A full request pool retries the same step from the
-- URL already reached.
-- is_cancelled, when given, stops the chain before any further request.
resolve_redirect = function(url, callback, on_handle, is_cancelled)
    local current, hops, busy_tries = url, 0, 0
    local step
    local function start(use_get)
        if is_cancelled and is_cancelled() then
            return
        end
        local opts = request_options(current, use_get and { Range = "bytes=0-0" } or nil)
        opts.method = use_get and "GET" or "HEAD"
        opts.redirect_limit = 0
        opts.max_response_bytes = 4096
        local handle, err = start_request(plugin.http_request, opts, function(code, _, request_err, headers)
            step(code, request_err, headers, use_get)
        end)
        if handle then
            busy_tries = 0
            if on_handle then
                on_handle(handle)
            end
        elseif pool_busy(err) and busy_tries < 30 then
            busy_tries = busy_tries + 1
            defer(function()
                start(use_get)
            end)
        else
            callback(nil, err or "Could not start redirect request")
        end
    end
    step = function(code, request_err, headers, from_get)
        if is_cancelled and is_cancelled() then
            return
        end
        -- A body too big for the probe means this URL serves the file (a
        -- chunked body reports io_error instead); the transfer itself will
        -- report a real network failure.
        if from_get and (request_err == "response_too_large" or request_err == "io_error") then
            callback(current)
            return
        end
        if request_err or code == 405 or code == 403 or code == 501 then
            if from_get then
                callback(nil, request_err or ("HTTP " .. tostring(code)))
            else
                defer(function()
                    start(true)
                end)
            end
            return
        end
        local location
        for k, v in pairs(headers or {}) do
            if k:lower() == "location" then
                location = v
                break
            end
        end
        if code >= 300 and code < 400 and location then
            local next_url = resolve_reference(current, location)
            if not valid_url(next_url) then
                callback(nil, "Invalid redirect URL")
                return
            end
            hops = hops + 1
            if hops > 8 then
                callback(nil, "Too many redirects")
                return
            end
            current = next_url
            defer(function()
                start(false)
            end)
            return
        end
        if (code >= 200 and code < 300) or code == 206 then
            callback(current)
        else
            callback(nil, "HTTP " .. tostring(code))
        end
    end
    if not valid_url(url) then
        callback(nil, "Invalid URL")
    else
        start(false)
    end
end

-- Every file name carries a short hash of the episode identity, so two
-- episodes with the same date and title never share (or delete) one file.
local function download_dest(show, episode)
    local folder = sanitized(show.title, show.key)
    local id = plugin.md5(show.key .. "|" .. episode.guid):sub(1, 8)
    local name = sanitized((episode.date ~= "" and episode.date .. " " or "") .. episode.title, "Episode")
    return ROOT .. "/" .. folder,
        ROOT .. "/" .. folder .. "/" .. name .. " [" .. id .. "]." .. audio_ext(episode.url, episode.mime)
end

-- One download at a time, in two steps: resolve the redirect chain, then
-- start the transfer from a later tick. A request's pool slot is freed only
-- after its callback returns, so starting the next request inside that
-- callback could never use the last free slot.
local function fail_download(item)
    if active_download == item then
        active_download = nil
    end
    plugin.show_toast("Download failed: " .. item.episode.title)
end

-- A full shared pool is transient: keep the item and try again next tick.
local function busy_retry(item)
    item.retries = (item.retries or 0) + 1
    if item.retries > 30 then
        fail_download(item)
        return
    end
    item.waiting = true
end

local function begin_transfer(item)
    item.waiting = false
    local handle, request_err = start_request(
        plugin.download_file_async,
        item.final_url,
        item.dest,
        true,
        function(path, download_err)
            if active_download ~= item then
                return
            end
            active_download = nil
            if download_err then
                plugin.show_toast("Download failed: " .. item.episode.title)
                return
            end
            local p = episode_progress(item.show, item.episode)
            -- Kept with the file so it stays listed after it leaves the 40-episode catalog.
            p.path, p.position = path, 0
            p.title, p.date, p.epoch = item.episode.title, item.episode.date, item.episode.epoch or 0
            if save_progress() then
                plugin.show_toast("Saved " .. item.episode.title)
            else
                plugin.show_toast("Saved " .. item.episode.title .. ", but the card may be full")
            end
        end
    )
    item.handle = handle
    if not handle then
        if pool_busy(request_err) then
            busy_retry(item)
        else
            fail_download(item)
        end
    end
end

local function begin_resolve(item)
    item.waiting = false
    resolve_redirect(item.episode.url, function(final_url, err)
        if active_download ~= item then
            return
        end
        if err then
            fail_download(item)
            return
        end
        item.final_url = final_url
        item.waiting = true -- the transfer starts from the next tick
    end, function(handle)
        item.handle = handle
    end, function()
        return active_download ~= item
    end)
end

-- Called from the 1 s tick: advances the active item or starts the next one.
local function service_downloads()
    local item = active_download
    if item then
        if item.waiting then
            if item.final_url then
                begin_transfer(item)
            else
                begin_resolve(item)
            end
        end
        return
    end
    item = table.remove(queue, 1)
    if not item then
        return
    end
    active_download = item
    local dir, dest = download_dest(item.show, item.episode)
    item.dest = dest
    -- A file already there (say, a download whose state was lost) is never
    -- removed by cancel cleanup; the native downloader keeps it too.
    local existing = io.open(dest, "rb")
    item.dest_existed = existing ~= nil
    if existing then
        existing:close()
    end
    if not plugin.mkdir(dir) then
        fail_download(item)
        return
    end
    begin_resolve(item)
end

enqueue_download = function(show, episode)
    if queued(show, episode) or downloaded(show, episode) then
        return
    end
    queue[#queue + 1] = { show = show, episode = episode, key = progress_key(show, episode) }
    service_downloads()
end

cancel_download = function(key)
    for i, item in ipairs(queue) do
        if item.key == key then
            table.remove(queue, i)
            plugin.show_toast("Download cancelled")
            return
        end
    end
    if active_download and active_download.key == key then
        if active_download.handle then
            plugin.cancel(active_download.handle)
        end
        active_download.cancelled = true
        -- The transfer may have finished just before the cancel; its file is
        -- removed from the tick unless something claims it meanwhile.
        if active_download.dest and not active_download.dest_existed then
            cancelled_files[#cancelled_files + 1] = { path = active_download.dest, ticks = 0 }
        end
        active_download = nil
        plugin.show_toast("Download cancelled")
    end
end

-- Every downloaded episode, from progress state rather than the catalogs, so
-- older episodes and those of unsubscribed shows stay reachable.
local function downloaded_entries()
    local list = {}
    for k, p in pairs(progress) do
        if p.path and p.path ~= "" then
            local f = io.open(p.path, "rb")
            if f then
                f:close()
                local key, guid = k:match("^([^|]+)|(.*)$")
                if key then
                    local show = show_by_key(key) or { key = key, title = "Unsubscribed", url = "" }
                    local episode
                    local catalog = catalogs[key]
                    for _, e in ipairs(catalog and catalog.items or {}) do
                        if e.guid == guid then
                            episode = e
                            break
                        end
                    end
                    episode = episode
                        or {
                            guid = guid,
                            title = (p.title ~= "" and p.title) or p.path:match("([^/]+)$"),
                            url = "",
                            mime = "",
                            duration = p.duration or 0,
                            date = p.date or "",
                            epoch = p.epoch,
                        }
                    list[#list + 1] = { show = show, episode = episode, path = p.path, progress = p }
                end
            end
        end
    end
    return list
end

local MAX_LIST_ROWS = 500

local function show_download_list(title, list, with_show)
    table.sort(list, function(a, b)
        return (a.episode.epoch or 0) > (b.episode.epoch or 0)
    end)
    local labels = {}
    for i = 1, math.min(#list, MAX_LIST_ROWS) do
        local row = list[i]
        labels[i] = cap((with_show and (row.show.title .. ": ") or "") .. row.episode.title, 200)
    end
    plugin.show_list(title, labels, function(index)
        open_episode(list[index].show, list[index].episode)
    end)
end

-- A flat list while it fits one screen; beyond that, one row per show.
local function open_downloads()
    local list = downloaded_entries()
    if #list <= MAX_LIST_ROWS then
        show_download_list("Downloads (" .. #list .. ")", list, true)
        return
    end
    -- One group per show; a show with more than a screenful is split by
    -- year, keeping the same screen depth (the list pool is four deep).
    local per_show = {}
    for _, row in ipairs(list) do
        per_show[row.show.key] = (per_show[row.show.key] or 0) + 1
    end
    local by_show, keys = {}, {}
    for _, row in ipairs(list) do
        local key, title = row.show.key, row.show.title
        if per_show[key] > MAX_LIST_ROWS then
            local year = (row.episode.date or ""):match("^(%d%d%d%d)") or "Undated"
            key, title = key .. "|" .. year, title .. " " .. year
        end
        if not by_show[key] then
            by_show[key] = { title = title, rows = {} }
            keys[#keys + 1] = key
        end
        table.insert(by_show[key].rows, row)
    end
    -- Any group still over a screenful is cut into numbered parts.
    local split_keys = {}
    for _, key in ipairs(keys) do
        local group = by_show[key]
        if #group.rows <= MAX_LIST_ROWS then
            split_keys[#split_keys + 1] = key
        else
            table.sort(group.rows, function(a, b)
                return (a.episode.epoch or 0) > (b.episode.epoch or 0)
            end)
            for first = 1, #group.rows, MAX_LIST_ROWS do
                local part_key = key .. "#" .. first
                local part =
                    { title = group.title .. " part " .. (math.floor(first / MAX_LIST_ROWS) + 1), rows = {} }
                for i = first, math.min(first + MAX_LIST_ROWS - 1, #group.rows) do
                    part.rows[#part.rows + 1] = group.rows[i]
                end
                by_show[part_key] = part
                split_keys[#split_keys + 1] = part_key
            end
        end
    end
    keys = split_keys
    table.sort(keys, function(a, b)
        return by_show[a].title:lower() < by_show[b].title:lower()
    end)
    local labels = {}
    for i, key in ipairs(keys) do
        labels[i] = cap(by_show[key].title .. " (" .. #by_show[key].rows .. ")", 200)
    end
    plugin.show_list("Downloads (" .. #list .. ")", labels, function(index)
        local group = by_show[keys[index]]
        show_download_list(group.title, group.rows, false)
    end)
end

local function continue_episode()
    local best, best_time
    for _, row in ipairs(downloaded_entries()) do
        local p = row.progress
        if p.position > 0 and not p.played and (not best_time or p.last_played > best_time) then
            best = { show = row.show, episode = row.episode, path = row.path, position = p.position }
            best_time = p.last_played
        end
    end
    return best
end

local function open_home()
    local rows, actions = {}, {}
    local cont = continue_episode()
    if cont then
        rows[#rows + 1] = "Continue: " .. cont.episode.title
        actions[#actions + 1] = function()
            -- track_started resumes from the position saved at that moment,
            -- not the one this screen was built with.
            pending_seek = nil
            plugin.play_file(cont.path)
        end
    end
    rows[#rows + 1] = "Search podcasts"
    actions[#actions + 1] = start_search
    rows[#rows + 1] = "Add feed URL"
    actions[#actions + 1] = add_feed_input
    rows[#rows + 1] = "Import OPML"
    actions[#actions + 1] = pick_import
    rows[#rows + 1] = "Export OPML"
    actions[#actions + 1] = function()
        if save_subscriptions() then
            plugin.show_toast("Exported " .. SUBSCRIPTIONS)
        else
            plugin.show_toast("Could not write " .. SUBSCRIPTIONS)
        end
    end
    local count = #downloaded_entries()
    if count > 0 then
        rows[#rows + 1] = "Downloads (" .. count .. ")"
        actions[#actions + 1] = open_downloads
    end
    if #subscriptions == 0 then
        rows[#rows + 1] = "Subscribe with Search or Add feed URL"
        actions[#actions + 1] = function() end
    else
        sort_subscriptions()
        -- New counts come from progress state, so Home never loads every catalog.
        local new_counts = {}
        for k, p in pairs(progress) do
            if p.new then
                local key = k:match("^([^|]+)|")
                if key then
                    new_counts[key] = (new_counts[key] or 0) + 1
                end
            end
        end
        for _, show in ipairs(subscriptions) do
            local new_count = new_counts[show.key] or 0
            rows[#rows + 1] = show.title .. (new_count > 0 and (" (" .. new_count .. " new)") or "")
            actions[#actions + 1] = function()
                open_show(show)
            end
        end
    end
    plugin.show_list("Podcasts", rows, function(index)
        actions[index]()
    end)
end

local function save_playback(force)
    if not force and (not plugin.is_playing() or plugin.is_paused()) then
        return
    end
    local path = plugin.get_current_track_path()
    local k = path and path_to_key[path]
    if not k then
        return
    end
    -- Until the resume seek lands, the reported position is not ours to save.
    if pending_seek and pending_seek.path == path then
        return
    end
    local position, duration = plugin.get_position(), plugin.get_duration()
    if not force and math.abs(position - last_saved_position) < 1 then
        return
    end
    local p = progress[k]
    p.position, p.duration, p.last_played = position, duration, os.time()
    if duration > 0 and (position >= duration - 30 or position / duration >= 0.95) then
        p.played = true
        p.position = 0
    end
    last_saved_position = position
    save_progress()
end

load_subscriptions()
sort_subscriptions()
load_progress()
if has_required_capabilities then
    plugin.mkdir(ROOT)
    plugin.mkdir(CATALOG)
    local ignore = io.open(ROOT .. "/database.ignore", "r")
    if not ignore then
        local f = io.open(ROOT .. "/database.ignore", "w")
        if f then
            f:close()
        end
    else
        ignore:close()
    end
end

-- The track can still be opening when track_started fires, and a seek then
-- is silently dropped, so a resume seek is retried on the 1 s tick until the
-- position shows it took effect (or it is given up on).
local function service_pending_seek()
    if not pending_seek then
        return
    end
    pending_seek.tries = pending_seek.tries + 1
    if pending_seek.tries > 15 then
        pending_seek = nil
        return
    end
    if plugin.get_current_track_path() ~= pending_seek.path or plugin.get_duration() <= 0 then
        return
    end
    -- Only a position read after our own seek counts, and one far past the
    -- target is the previous track's, still being reported.
    local position = plugin.get_position()
    if
        pending_seek.seeked
        and position >= pending_seek.position - 3
        and position <= pending_seek.position + 30
    then
        pending_seek = nil
        last_saved_position = -1
        return
    end
    plugin.seek(pending_seek.position)
    pending_seek.seeked = true
end

plugin.on("track_started", function()
    stream_generation = stream_generation + 1
    last_saved_position = -1
    local path = plugin.get_current_track_path()
    local k = path and path_to_key[path]
    if pending_seek and pending_seek.path ~= path then
        pending_seek = nil
    end
    if k and not pending_seek then
        local p = progress[k]
        if p and p.position > 5 and not p.played then
            pending_seek = { path = path, position = p.position, tries = 0 }
        end
    end
    -- No save here: the position and duration can still be the previous
    -- track's. The tick saves once the new one is really playing.
    service_pending_seek()
end)
plugin.on("paused", function()
    save_playback(true)
end)
plugin.on("stopped", function()
    save_playback(true)
end)
local ticks = 0
interval_handle = plugin.set_interval(1, function()
    service_pending_seek()
    run_deferred()
    service_downloads()
    for i = #cancelled_files, 1, -1 do
        local entry = cancelled_files[i]
        entry.ticks = entry.ticks + 1
        local reclaimed = path_to_key[entry.path] or (active_download and active_download.dest == entry.path)
        if reclaimed or entry.ticks > 30 then
            table.remove(cancelled_files, i)
        elseif entry.ticks >= 3 then
            local f = io.open(entry.path, "rb")
            if f then
                f:close()
                os.remove(entry.path)
                table.remove(cancelled_files, i)
            end
        end
    end
    ticks = ticks + 1
    if ticks % 10 == 0 then
        save_playback(false)
        if progress_dirty then
            save_progress()
        end
    end
end)

plugin.register_stream_media_tile("Podcasts", function()
    if not has_required_capabilities then
        plugin.show_toast("Podcasts needs a newer player build")
        return
    end
    open_home()
end, "stream_media/podcasts_row.png")
