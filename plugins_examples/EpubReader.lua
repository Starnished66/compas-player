plugin.define({
    id = "example.epub_reader",
    name = "EPUB Reader",
    version = "1.0",
    api_min = 14,
})

-- EPUB reader for unencrypted books stored under <SD card>/Books.
-- Scans Books and immediate subfolders, parses container.xml and the OPF,
-- displays the spine in a list screen with NCX/nav chapter titles,
-- converts chapters to text blocks, and opens paged text reading with
-- persistent resume and continue reading.

-- Last slash, forward only. Lua's pattern matcher backtracks on [^/]+$.
local function basename(path)
    local slash = string.byte("/")
    local last = 0
    local n = #path
    for i = 1, n do
        if path:byte(i) == slash then
            last = i
        end
    end
    if last == 0 or last == n then
        return path
    end
    return path:sub(last + 1)
end

local function percent_decode(s)
    if not s then
        return ""
    end
    return (s:gsub("%%(%x%x)", function(hex)
        return string.char(tonumber(hex, 16))
    end))
end

-- show_list copies a row into a 160-byte buffer. Clip before the spine
-- retains the label, or a slash-free href is kept whole for every itemref.
local MAX_LABEL_BYTES = 160

-- A chapter href names a ZIP entry. The container path, the document
-- directory, and the joined path share this limit. Reject a longer value
-- before it is copied; a repeated spine id or nav label would otherwise
-- keep a copy on every row.
local MAX_HREF_BYTES = 4096

-- Path before '#'. A long fragment is ignored; a path past MAX_HREF_BYTES
-- is rejected. The oversized check reads a short prefix, not the whole value.
local function take_href(href)
    if not href or href == "" then
        return ""
    end
    local hash
    if #href > MAX_HREF_BYTES then
        hash = href:sub(1, MAX_HREF_BYTES + 1):find("#", 1, true)
        if not hash then
            return ""
        end
    else
        hash = href:find("#", 1, true)
    end
    if not hash then
        return href
    end
    if hash == 1 then
        return ""
    end
    return href:sub(1, hash - 1)
end

local function resolve_href(opf_dir, href)
    href = take_href(href)
    if href == "" then
        return ""
    end
    href = percent_decode(href)
    local is_dir = (href:sub(-1) == "/")
    local absolute = (href:sub(1, 1) == "/")
    -- take_href never sees the document directory. Reject a long one
    -- before it is copied onto this relative href.
    if not absolute and opf_dir and #opf_dir > MAX_HREF_BYTES then
        return ""
    end

    local segments = {}
    local function add_segments(p)
        for seg in (p .. "/"):gmatch("([^/]*)/") do
            if seg == "" or seg == "." then
                -- drop
            elseif seg == ".." then
                if #segments > 0 then
                    segments[#segments] = nil
                end
            else
                segments[#segments + 1] = seg
            end
        end
    end

    if absolute then
        add_segments(href:sub(2))
    else
        if opf_dir and opf_dir ~= "" then
            add_segments(opf_dir)
        end
        add_segments(href)
    end

    -- Join only after the normalized length fits. table.concat would copy
    -- a path the limit already rejected.
    local result_len = 0
    for i = 1, #segments do
        result_len = result_len + #segments[i]
        if i > 1 then
            result_len = result_len + 1
        end
        if result_len > MAX_HREF_BYTES then
            return ""
        end
    end
    if is_dir and result_len > 0 and result_len + 1 > MAX_HREF_BYTES then
        return ""
    end

    local result = table.concat(segments, "/")
    if is_dir and result ~= "" then
        result = result .. "/"
    end
    return result
end

-- Cache the resolved path on the manifest item. A repeated spine id
-- reuses it instead of resolving the href again.
local function resolved_href(item, base_dir)
    if not item then
        return nil
    end
    local href = item.resolved
    if href == nil then
        href = resolve_href(base_dir, item.href)
        item.resolved = href
    end
    if href == "" then
        return nil
    end
    return href
end

-- Quoted '>' is attribute data, not the tag terminator. XML has no
-- backslash escapes, so a quote ends only at the same quote. One scan
-- for the next of '>', '"' or "'": separate suffix searches are quadratic.
local GT_OR_QUOTE = "[>\"']"

local function find_unquoted_gt(data, p)
    local n = #data
    while p <= n do
        local i = data:find(GT_OR_QUOTE, p)
        if not i then
            return nil
        end
        local q = data:sub(i, i)
        if q == ">" then
            return i
        end
        local close = data:find(q, i + 1, true)
        if not close then
            return nil
        end
        p = close + 1
    end
    return nil
end

local function collapse_whitespace(s)
    if not s then
        return ""
    end
    -- Forward-only: a lazy "(.-)%s*$" is quadratic on long whitespace runs.
    local first = s:find("%S")
    if not first then
        return ""
    end
    return (s:match("^.*%S", first):gsub("%s+", " "))
end

local ENTITIES = { amp = "&", lt = "<", gt = ">", quot = '"', apos = "'", nbsp = "\194\160" }

-- Markup at lt that is not an element tag: a comment, CDATA section,
-- processing instruction or declaration. Returns the position after it and,
-- for CDATA, the content bounds. Returns false when it is never closed: any
-- later search would scan to the end again, so callers stop there. Returns
-- nil for an element start or end tag.
local function skip_special(data, lt)
    if data:sub(lt, lt + 3) == "<!--" then
        local close = data:find("-->", lt + 4, true)
        return close and (close + 3) or false
    elseif data:sub(lt, lt + 8) == "<![CDATA[" then
        local close = data:find("]]>", lt + 9, true)
        if not close then
            return false
        end
        return close + 3, lt + 9, close - 1
    end
    local c = data:sub(lt + 1, lt + 1)
    if c == "?" then
        local close = data:find("?>", lt + 2, true)
        return close and (close + 2) or false
    elseif c == "!" then
        local gt = find_unquoted_gt(data, lt + 2)
        return gt and (gt + 1) or false
    end
    return nil
end

-- One pass, so "&amp;lt;" stays "&lt;".
local function decode_entities(s)
    if not s or not s:find("&", 1, true) then
        return s or ""
    end
    return (
        s:gsub("&(#?%w+);", function(e)
            local named = ENTITIES[e]
            if named then
                return named
            end
            local cp
            if e:sub(1, 2):lower() == "#x" then
                cp = tonumber(e:sub(3), 16)
            elseif e:sub(1, 1) == "#" then
                cp = tonumber(e:sub(2), 10)
            end
            if cp and cp > 0 and cp <= 0x10FFFF and (cp < 0xD800 or cp > 0xDFFF) then
                return utf8.char(cp)
            end
            return nil
        end)
    )
end

-- Text content of markup: tags and comments dropped, entities decoded,
-- CDATA kept literally (it is not entity-decoded).
local function markup_text(s)
    local parts = {}
    local p = 1
    local len = #s
    while p <= len do
        local lt = s:find("<", p, true)
        if not lt then
            parts[#parts + 1] = decode_entities(s:sub(p))
            break
        end
        if lt > p then
            parts[#parts + 1] = decode_entities(s:sub(p, lt - 1))
        end
        -- Comments, CDATA and processing instructions end at their own
        -- terminator, not at a quote or a '>', so skip them before the
        -- quote-aware scan.
        local after, cdata_s, cdata_e = skip_special(s, lt)
        if after == false then
            break
        elseif after then
            if cdata_s then
                parts[#parts + 1] = s:sub(cdata_s, cdata_e)
            end
            p = after
        else
            local gt = find_unquoted_gt(s, lt + 1)
            if not gt then
                break
            end
            p = gt + 1
        end
    end
    return table.concat(parts)
end

-- XML requires quoted attribute values, so unquoted ones are ignored.
-- Forward-only, so a long run of name characters without "=" stays linear.
local function parse_attributes(s)
    local attrs = {}
    local p = 1
    while true do
        local ns, ne = s:find("[%w_:%.%-]+", p)
        if not ns then
            break
        end
        p = ne + 1
        local _, eq = s:find("^%s*=%s*", p)
        if eq then
            p = eq + 1
            local q = s:sub(p, p)
            if q == '"' or q == "'" then
                local close = s:find(q, p + 1, true)
                if not close then
                    break
                end
                attrs[s:sub(ns, ne):lower()] = decode_entities(s:sub(p + 1, close - 1))
                p = close + 1
            end
        end
    end
    return attrs
end

local function has_nav_property(props)
    if not props then
        return false
    end
    for word in props:gmatch("%S+") do
        if word == "nav" then
            return true
        end
    end
    return false
end

local function dir_of(path)
    return path:match("^(.*)/[^/]+$") or ""
end

-- Directory prepended to relative hrefs. A path past the href limit has
-- no usable directory: dir_of would copy it before every name below.
local function document_dir(path)
    if not path or #path > MAX_HREF_BYTES then
        return nil
    end
    return dir_of(path)
end

local function local_name_of(name)
    return name:match(":([^:]+)$") or name
end

-- Returns lt, gt, name, local_name of the next start tag at or after p, skipping
-- comments, CDATA, doctypes, processing instructions and end tags.
local function next_start_tag(data, p)
    local n = #data
    while p <= n do
        local lt = data:find("<", p, true)
        if not lt then
            return nil
        end
        local after = skip_special(data, lt)
        if after == false then
            return nil
        elseif after then
            p = after
        else
            local c = data:sub(lt + 1, lt + 1)
            local gt = find_unquoted_gt(data, lt + 1)
            if not gt then
                return nil
            end
            p = gt + 1
            if c ~= "/" then
                local name = data:match("^[%w_:%.%-]+", lt + 1)
                if name then
                    name = name:lower()
                    return lt, gt, name, local_name_of(name)
                end
            end
        end
    end
    return nil
end

-- Returns the start and end of the next end tag with this local name.
-- Comments, CDATA, processing instructions and quoted attribute values of
-- nested start tags are skipped, so an end tag inside them is ignored.
local function find_close(data, p, local_name)
    local n = #data
    while p <= n do
        local lt = data:find("<", p, true)
        if not lt then
            return nil
        end
        local after = skip_special(data, lt)
        if after == false then
            return nil
        elseif after then
            p = after
        elseif data:sub(lt + 1, lt + 1) == "/" then
            local name = data:match("^[%w_:%.%-]+", lt + 2)
            if name and local_name_of(name:lower()) == local_name then
                return lt, data:find(">", lt + 2, true)
            end
            p = lt + 2
        elseif data:find("^[%a_:]", lt + 1) then
            -- A nested start tag: a quoted "</a>" in its attributes is data.
            local gt = find_unquoted_gt(data, lt + 1)
            if not gt then
                return nil
            end
            p = gt + 1
        else
            p = lt + 1
        end
    end
    return nil
end

-- Text of the element whose start tag ends at gt, and the position after it.
-- unclosed remembers names with no end tag left, so each is searched for once.
local function element_text(data, gt, local_name, unclosed)
    if data:sub(gt - 1, gt - 1) == "/" or unclosed[local_name] then
        return nil, gt + 1
    end
    local close_lt, close_gt = find_close(data, gt + 1, local_name)
    if not close_lt then
        unclosed[local_name] = true
        return nil, gt + 1
    end
    local text = collapse_whitespace(markup_text(data:sub(gt + 1, close_lt - 1)))
    return text, close_gt and (close_gt + 1) or (close_lt + 2)
end

-- titles holds one "" slot per spine chapter (see parse_book). Only those
-- are filled, so a table of contents with thousands of links retains at
-- most the bounded spine's worth of titles.
local function set_title(titles, key, text)
    if key and text and text ~= "" and titles[key] == "" then
        titles[key] = text
    end
end

local function has_titles(titles)
    for _, v in pairs(titles) do
        if v ~= "" then
            return true
        end
    end
    return false
end

-- Resolves a document's links to spine chapters without rebuilding its
-- directory per link. The directory is split once. A path is identified by
-- how many leading directory components it keeps plus its own remaining
-- tail, always taking the longest shared prefix, so every spelling of one
-- file ("../dir/a", "./a", "a") gets the same short key. Spine chapters are
-- keyed the same way up front, so a link costs only its own length and no
-- valid title is ever skipped.
local function new_title_resolver(base_dir, titles)
    local bsegs = {}
    for seg in (base_dir .. "/"):gmatch("([^/]*)/") do
        if seg ~= "" then
            bsegs[#bsegs + 1] = seg
        end
    end
    local nb = #bsegs
    local bnorm = table.concat(bsegs, "/")

    local ids = {}
    for href in pairs(titles) do
        local k, rest = 0, href
        if nb > 0 and href:sub(1, #bnorm + 1) == bnorm .. "/" then
            k, rest = nb, href:sub(#bnorm + 2)
        else
            local pos = 1
            while k < nb do
                local seg = bsegs[k + 1]
                local stop = pos + #seg
                if href:sub(pos, stop - 1) ~= seg or href:sub(stop, stop) ~= "/" then
                    break
                end
                k, pos = k + 1, stop + 1
            end
            rest = href:sub(pos)
        end
        ids[k .. "\0" .. rest] = href
    end

    return function(href)
        local path = take_href(href)
        if path == "" then
            return nil
        end
        path = percent_decode(path)
        if path:sub(-1) == "/" then
            return nil -- a directory, not a chapter file
        end
        local absolute = path:sub(1, 1) == "/"
        local up, hs = 0, {}
        for seg in (path .. "/"):gmatch("([^/]*)/") do
            if seg == "" or seg == "." then
                -- drop
            elseif seg == ".." then
                if #hs > 0 then
                    hs[#hs] = nil
                else
                    up = up + 1
                end
            else
                hs[#hs + 1] = seg
            end
        end
        local k = 0
        if not absolute then
            k = nb - up
            if k < 0 then
                k = 0
            end
        end
        local i = 1
        while i <= #hs and k < nb and hs[i] == bsegs[k + 1] do
            k, i = k + 1, i + 1
        end
        return ids[k .. "\0" .. table.concat(hs, "/", i)]
    end
end

local function scan_epub_files()
    local root = plugin.sd_root() .. "/Books"
    local paths = {}
    local function add(dir, name)
        if #paths < 200 and name:lower():sub(-5) == ".epub" then
            paths[#paths + 1] = dir .. "/" .. name
        end
    end
    for _, e in ipairs(plugin.list_dir(root)) do
        if not e.dir then
            add(root, e.name)
        else
            local sub = root .. "/" .. e.name
            for _, se in ipairs(plugin.list_dir(sub)) do
                if not se.dir then
                    add(sub, se.name)
                end
            end
        end
    end
    table.sort(paths)
    return paths
end

local function find_opf_path(container_xml)
    local p = 1
    while true do
        local lt, gt, _, lname = next_start_tag(container_xml, p)
        if not lt then
            return nil
        end
        p = gt + 1
        if lname == "rootfile" and gt - lt <= MAX_HREF_BYTES * 2 then
            -- Slice the tag only when full-path can still fit. A longer tag
            -- is rejected before the path is copied out of container.xml.
            local raw = parse_attributes(container_xml:sub(lt, gt))["full-path"]
            if raw and #raw <= MAX_HREF_BYTES then
                local val = percent_decode(raw)
                val = val:gsub("^/+", "")
                if val ~= "" then
                    return val
                end
            end
        end
    end
end

local function parse_opf(opf, book_path)
    local unclosed = {}
    local dc_title, fallback_title, creator
    local manifest = {}
    local spine_ids = {}
    local spine_truncated = false
    local ncx_item, nav_item

    local p = 1
    while true do
        local lt, gt, name, lname = next_start_tag(opf, p)
        if not lt then
            break
        end
        p = gt + 1
        if lname == "title" or lname == "creator" then
            local text
            text, p = element_text(opf, gt, lname, unclosed)
            if text and text ~= "" then
                if name == "dc:title" then
                    dc_title = dc_title or text
                elseif lname == "title" then
                    fallback_title = fallback_title or text
                else
                    creator = creator or text
                end
            end
        elseif lname == "item" then
            local attrs = parse_attributes(opf:sub(lt, gt))
            local id = attrs["id"]
            if id and id ~= "" then
                local it = {
                    href = take_href(attrs["href"] or ""),
                    media = attrs["media-type"] or "",
                    properties = attrs["properties"] or "",
                }
                manifest[id] = it
                if it.media:lower() == "application/x-dtbncx+xml" and not ncx_item then
                    ncx_item = it
                end
                if has_nav_property(it.properties) and not nav_item then
                    nav_item = it
                end
            end
        elseif lname == "itemref" then
            local idref = parse_attributes(opf:sub(lt, gt))["idref"]
            if idref and idref ~= "" then
                if #spine_ids < 500 then
                    spine_ids[#spine_ids + 1] = idref
                elseif not spine_truncated then
                    plugin.show_toast("Spine truncated")
                    spine_truncated = true
                end
            end
        end
    end

    return {
        title = dc_title or fallback_title or basename(book_path),
        author = creator,
        manifest = manifest,
        spine_ids = spine_ids,
        ncx_item = ncx_item,
        nav_item = nav_item,
    }
end

-- NCX and nav hrefs are relative to their own document, not the OPF.
local function parse_ncx_titles(ncx, resolve, titles)
    local unclosed = {}
    local pending
    local p = 1
    while true do
        local lt, gt, _, lname = next_start_tag(ncx, p)
        if not lt then
            break
        end
        p = gt + 1
        if lname == "text" then
            local text
            text, p = element_text(ncx, gt, "text", unclosed)
            if text and text ~= "" then
                pending = text
            end
        elseif lname == "content" and pending then
            local src = parse_attributes(ncx:sub(lt, gt))["src"]
            if src then
                set_title(titles, resolve(src), pending)
                pending = nil
            end
        end
    end
end

local function parse_nav_titles(nav, resolve, titles)
    local unclosed = {}
    local p = 1
    while true do
        local lt, gt, _, lname = next_start_tag(nav, p)
        if not lt then
            break
        end
        p = gt + 1
        if lname == "a" then
            local href = parse_attributes(nav:sub(lt, gt))["href"]
            local text
            text, p = element_text(nav, gt, "a", unclosed)
            if href then
                set_title(titles, resolve(href), text)
            end
        end
    end
end

local function parse_book(book_path)
    local container_data, err = plugin.zip_read(book_path, "META-INF/container.xml")
    if not container_data then
        plugin.show_toast(err or "Failed to read container.xml")
        return nil
    end

    local opf_path = find_opf_path(container_data)
    if not opf_path then
        plugin.show_toast("No OPF in container.xml")
        return nil
    end

    local opf_bytes, opf_err = plugin.zip_read(book_path, opf_path)
    if not opf_bytes then
        plugin.show_toast(opf_err or "Failed to read OPF")
        return nil
    end

    local book = parse_opf(opf_bytes, book_path)
    local opf_dir = document_dir(opf_path)
    if opf_dir == nil then
        plugin.show_toast("No OPF in container.xml")
        return nil
    end
    book.opf_dir = opf_dir

    local titles = {}
    for _, idref in ipairs(book.spine_ids) do
        local href = resolved_href(book.manifest[idref], book.opf_dir)
        if href then
            titles[href] = ""
        end
    end
    if book.ncx_item then
        local ncx_path = resolved_href(book.ncx_item, book.opf_dir)
        if ncx_path then
            local ncx_data = plugin.zip_read(book_path, ncx_path)
            if ncx_data then
                local ncx_dir = document_dir(ncx_path)
                if ncx_dir ~= nil then
                    parse_ncx_titles(ncx_data, new_title_resolver(ncx_dir, titles), titles)
                end
            end
        end
    end

    if not has_titles(titles) and book.nav_item then
        local nav_path = resolved_href(book.nav_item, book.opf_dir)
        if nav_path then
            local nav_data = plugin.zip_read(book_path, nav_path)
            if nav_data then
                local nav_dir = document_dir(nav_path)
                if nav_dir ~= nil then
                    parse_nav_titles(nav_data, new_title_resolver(nav_dir, titles), titles)
                end
            end
        end
    end

    book.titles = titles
    return book
end

-- Keys are hashed because storage keys are capped at 128 bytes.
local function pos_key(book_path)
    return "pos:" .. plugin.md5(book_path)
end

local function load_pos(book_path)
    local pos_str = plugin.storage.get(pos_key(book_path))
    if not pos_str then
        return nil, nil
    end
    return tonumber(pos_str:match("^(%d+)")), tonumber(pos_str:match("\n(%d+)$"))
end

local function save_pos(book_path, index, byte_offset)
    plugin.storage.set(pos_key(book_path), tostring(index) .. "\n" .. tostring(byte_offset or 0))
end

local function clip_text(s, max_bytes)
    if not s or #s <= max_bytes then
        return s or ""
    end
    -- Cut on a character boundary.
    local clipped = s:sub(1, max_bytes + 1):gsub("[\192-\255][\128-\191]*$", "")
    return clipped
end

local function chapter_href(book, index)
    local idref = book.spine_ids[index]
    return resolved_href(idref and book.manifest[idref], book.opf_dir)
end

local function chapter_label(book, index)
    local idref = book.spine_ids[index]
    local item = idref and book.manifest[idref]
    if item and item.label ~= nil then
        return item.label
    end
    local href = resolved_href(item, book.opf_dir)
    local label = "(missing)"
    if href then
        label = book.titles[href]
        if not label or label == "" then
            label = basename(href)
        end
        label = clip_text(label, MAX_LABEL_BYTES)
    end
    if item then
        item.label = label
    end
    return label
end

local function open_chapter(book_path, book, index, offset)
    local href = chapter_href(book, index)
    if not href then
        plugin.show_toast("Missing manifest item")
        return
    end

    local chapter_bytes, err = plugin.zip_read(book_path, href)
    if not chapter_bytes then
        plugin.show_toast(err or "Failed to read chapter")
        return
    end

    local blocks, b_err = plugin.html_to_blocks(chapter_bytes)
    if not blocks then
        plugin.show_toast(b_err or "Failed to parse chapter")
        return
    end

    local pieces = {}
    for i = 1, #blocks do
        local b = blocks[i]
        if b.kind == "h" or b.kind == "p" then
            pieces[#pieces + 1] = (b.text or "") .. "\n\n"
        elseif b.kind == "img" then
            pieces[#pieces + 1] = "[image]\n\n"
        elseif b.kind == "hr" then
            pieces[#pieces + 1] = "\n"
        end
    end

    local opts = {
        on_turn = function(_, _, byte_offset)
            save_pos(book_path, index, byte_offset)
        end,
        on_close = function(_, byte_offset)
            save_pos(book_path, index, byte_offset)
        end,
    }
    if offset and offset >= 0 then
        opts.offset = offset
    end

    local ok, tv_err = plugin.show_text_view(chapter_label(book, index), table.concat(pieces), opts)
    if not ok then
        plugin.show_toast(tv_err or "unavailable")
        return
    end
    if plugin.storage.get("continue") ~= book_path then
        plugin.storage.set("continue", book_path)
    end
end

local function open_book(book_path)
    local book = parse_book(book_path)
    if not book then
        return
    end

    if #book.spine_ids == 0 then
        plugin.show_toast("Empty spine")
        return
    end

    local spine_labels = {}
    for i = 1, #book.spine_ids do
        spine_labels[i] = chapter_label(book, i)
    end

    local list_title = book.title
    if book.author and book.author ~= "" then
        list_title = list_title .. " - " .. book.author
    end
    list_title = clip_text(list_title, 80)

    plugin.show_list(list_title, spine_labels, function(chapter_index)
        local saved_chap, saved_off = load_pos(book_path)
        open_chapter(book_path, book, chapter_index, saved_chap == chapter_index and saved_off or nil)
    end)
end

local function open_continue(book_path)
    local book = parse_book(book_path)
    if not book then
        return
    end

    if #book.spine_ids == 0 then
        plugin.show_toast("Empty spine")
        return
    end

    local saved_chap, saved_off = load_pos(book_path)
    if not saved_chap or saved_chap < 1 or saved_chap > #book.spine_ids then
        saved_chap, saved_off = 1, 0
    end

    open_chapter(book_path, book, saved_chap, saved_off)
end

local function open_books_list()
    local paths = scan_epub_files()
    if #paths == 0 then
        plugin.show_toast("No EPUB files in Books")
        return
    end

    local saved_continue = plugin.storage.get("continue")
    local continue_path = nil
    if saved_continue then
        for i = 1, #paths do
            if paths[i] == saved_continue then
                continue_path = saved_continue
                break
            end
        end
    end

    local rows = {}
    if continue_path then
        rows[#rows + 1] = "Continue reading: " .. basename(continue_path)
    end
    for i = 1, #paths do
        rows[#rows + 1] = basename(paths[i])
    end

    plugin.show_list("EPUB Reader", rows, function(index)
        if continue_path and index == 1 then
            open_continue(continue_path)
        else
            local path_idx = continue_path and (index - 1) or index
            local book_path = paths[path_idx]
            if book_path then
                open_book(book_path)
            end
        end
    end)
end

plugin.register_list_item("books", "EPUB Reader", open_books_list)
