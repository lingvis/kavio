--- Kavio Lua device demonstration.
-- Uses only public SDK APIs for lifecycle, widgets, RX/TX, input, App Channel,
-- configuration, and H.264 capture. The native Image widget owns converted
-- pixels after set_buffer returns; callers release borrowed frame buffers.

local kavio = require("kavio")
local ui = require("kavio.ui")
local proto = require("demo_protocol")

-- Lua uses the App runtime stdout/stderr log stream. The current public Lua
-- binding does not expose the C++/Python app_logger API, so keep this small
-- wrapper rather than introducing a second native logging interface.
local LOG_PREFIX = "[KavioDemoLua]"
local function log_info(event, detail)
    print(string.format("%s [INFO] %s%s", LOG_PREFIX, event,
        detail and (" " .. tostring(detail)) or ""))
end
local function log_warn(event, detail)
    print(string.format("%s [WARN] %s%s", LOG_PREFIX, event,
        detail and (" " .. tostring(detail)) or ""))
end
local function log_error(event, detail)
    print(string.format("%s [ERROR] %s%s", LOG_PREFIX, event,
        detail and (" " .. tostring(detail)) or ""))
end

local app = kavio.App({
    rx_enabled = true,
    rx_frame_sample_interval = 1,  -- Request every available RX frame.
    tx_enabled = true,
})

local backend = ui.LvglBackend()
local device = nil
local current_page = "home"
local status_label = nil
local detail_label = nil
local rx_preview = nil
local sequence = 1
local tx_surface = nil
local h264_stream_id = 0
local next_h264_seq = 0
local h264_file = nil
local tick_count = 0
local rx_reads = 0
local transfer_packets = 0
local protocol_errors = 0
local hello_sent = false

-- RX preview is opt-in. Each UI tick attempts the latest available frame.
-- Use the SDK monotonic clock, as in C++/Python. FPS counts distinct
-- frames successfully submitted to the Image widget per elapsed second.
local PREVIEW_WIDTH = 620  -- Same image-conversion bound as C++ and Python (pixels).
local PREVIEW_HEIGHT = 360 -- Same image-conversion bound as C++ and Python (pixels).
local rx_live = false
local rx_live_button = nil
local rx_fps_label = nil
local rx_fps_count = 0
local rx_fps_window_start = 0
local last_rx_frame_id = nil

-- --- App Channel transport and state reporting ---
--- Update the status label while it belongs to the active page.
-- @param text string Status shown to the user.
local function set_status(text)
    if status_label then
        status_label:set_text(tostring(text))
    end
end

--- Send one App Channel protocol packet, reusing a reply sequence if supplied.
-- @param message_type integer Packet kind from demo_protocol.
-- @param payload string Optional binary payload.
-- @param request_sequence integer|nil Reply sequence, or nil for a new request.
-- @param checksum_override integer|nil Optional precomputed checksum.
local function send_packet(message_type, payload, request_sequence, checksum_override)
    if not device:is_comm_channel_ready() then
        log_warn("app_channel_send_skipped", "channel_not_ready")
        return
    end

    local packet_sequence = request_sequence
    if packet_sequence == nil then
        packet_sequence = sequence
        sequence = (sequence + 1) & 0xffffffff
    end

    local packet = proto.packet(message_type, packet_sequence, payload or "", checksum_override)
    device:send_comm_data(packet)
    log_info("app_channel_packet_sent", string.format("type=%d sequence=%d bytes=%d",
        message_type, packet_sequence, #packet))
end

--- Build the compact state string consumed by the Windows Host.
local function state_text()
    local runtime = device:runtime_data()
    return string.format(
        "lang=lua;page=%s;usb=%d;rx=%dx%d;tx=%dx%d;rx_reads=%d;transfer=%d;errors=%d",
        current_page,
        device:is_comm_channel_ready() and 1 or 0,
        runtime.rx.width,
        runtime.rx.height,
        runtime.tx.width,
        runtime.tx.height,
        rx_reads,
        transfer_packets,
        protocol_errors
    )
end

local show_page

-- Presentation is shared in spirit with the Python demo; native callbacks stay intact.
-- Theme colors use 0xRRGGBB; dimensions passed to Style are pixels.
-- --- UI theme and widget builders ---
local C = {
    backdrop = 0x0a1220,
    panel = 0x132238,
    row = 0x16243b,
    control = 0x1f4269,
    accent = 0x276bbd,
    white = 0xffffff,
    muted = 0xb4c6e3,
    caption = 0x85a3d0,
}

--- Build a Label widget with optional pixel font size and RGB color.
-- @param text string Display caption.
-- @param size integer|nil Font size in pixels.
-- @param color integer|nil RGB color value.
local function label(text, size, color)
    return ui.Label(text):style(ui.Style({
        font_size = size or 16,
        text_color = color or C.muted,
    }))
end

--- Build a touch-friendly button and register its click handler.
-- @param text string Button caption.
-- @param handler function Zero-argument action callback.
-- @param primary boolean|nil Whether to use the accent color.
local function action(text, handler, primary)
    return ui.Button(text, handler):style(ui.Style({
        width = 290, height = 58, radius = 14, font_size = 18,
        background = primary and C.accent or 0x233756,
        text_color = C.white,
    }))
end

--- Create a complete 720x720 page from a heading and content widgets.
local function shell(title, subtitle, ...)
    local header = ui.Column(
        label("KAVIO  /  DEVICE SDK", 16, 0x6fa3ff),
        label(title, 24, C.white),
        label(subtitle, 16, C.muted)
    ):style(ui.Style({width = 654, padding = 20, gap = 10,
                      background = C.panel, radius = 22}))
    return ui.Column(header, ...):style(ui.Style({
        width = 720, height = 720, padding = 24, gap = 16,
        background = C.backdrop,
    }))
end

--- Build the two-column capability navigation cards.
local function navigation()
    local cards = {}
    local pages = {}
    for _, page in ipairs(proto.pages) do
        if page ~= "home" then pages[#pages + 1] = page end
    end
    for i = 1, #pages, 2 do
        local row = ui.Row()
        for j = i, math.min(i + 1, #pages) do
            local target = pages[j]
            row:add(action(target:gsub("_", " "):upper(), function()
                show_page(target)
            end))
        end
        cards[#cards + 1] = row:style(ui.Style({
            width = 654, padding = 6, gap = 12,
            background = C.row, radius = 16,
        }))
    end
    local column = ui.Column(label("CAPABILITIES", 16, C.caption))
    for _, card in ipairs(cards) do column:add(card) end
    return column:style(ui.Style({
        width = 654, padding = 4, gap = 10, background = C.backdrop,
    }))
end

--- Build a function page with interactive controls and status feedback.
local function page_root(title, ...)
    status_label = label("Ready", 18, C.white)
    detail_label = label("Select an action to inspect this API", 16, C.muted)
    local actions = ui.Column(label("INTERACTIVE CONTROLS", 16, C.caption))
    for _, item in ipairs({...}) do
        -- Button, Slider, Switch all support style(); keep their callbacks.
        if getmetatable(item) == ui.Button then
            item:style(ui.Style({
                width = 590, height = 60, radius = 14, font_size = 18,
                background = C.control, text_color = C.white,
            }))
        elseif getmetatable(item) == ui.Slider then
            item:style(ui.Style({width = 560}))
        end
        actions:add(item)
    end
    actions:style(ui.Style({width = 654, padding = 14, gap = 16,
                            background = C.panel, radius = 22}))
    local feedback = ui.Column(
        label("LIVE FEEDBACK", 16, C.caption),
        status_label, detail_label
    ):style(ui.Style({width = 654, padding = 20, gap = 10,
                      background = C.panel, radius = 22}))
    local back = action("BACK TO OVERVIEW", function()
        show_page("home")
    end, true)
    return shell(title:upper(), "Lua  /  Public SDK capability demo",
                 actions, feedback, back)
end

-- --- HDMI RX image presentation ---
--- Read one HDMI RX sample and update the native Image widget.
-- @param automatic boolean If true, do not report unavailable frames and
--   skip a frame whose identifier matches the last displayed frame.
local function read_rx_frame(automatic)
    local ok, frame = pcall(function()
        return device:read_latest_rx_frame()
    end)
    if not ok then
        if not automatic then
            log_warn("rx_frame_read_failed", frame)
            set_status(frame)
        end
        return
    end

    local info = frame:info()
    local buffer = frame:buffer()
    local _, size = buffer:address()
    if automatic and info.frame_id ~= nil and info.frame_id == last_rx_frame_id then
        buffer:release()
        return
    end
    -- Use pcall so a failed native conversion cannot leak the frame buffer.
    local updated, update_error = pcall(function()
        if rx_preview then rx_preview:set_buffer(frame, PREVIEW_WIDTH, PREVIEW_HEIGHT) end
    end)
    if not updated then
        log_error("rx_frame_display_failed", update_error)
        buffer:release()
        if not automatic then set_status(update_error) end
        return
    end
    last_rx_frame_id = info.frame_id
    rx_reads = rx_reads + 1
    if not automatic then
        log_info("rx_frame_displayed", string.format("frame_id=%s size=%dx%d stride=%d bytes=%d",
            tostring(info.frame_id), info.width, info.height, info.stride, size))
    end
    if automatic then
        rx_fps_count = rx_fps_count + 1
    else
        set_status(string.format(
            "RX %dx%d stride=%d bytes=%d",
            info.width, info.height, info.stride, size
        ))
    end
    buffer:release()
end

-- --- Virtual input and TX surface helpers ---
--- Emit key-down and key-up for a single virtual key.
local function send_key(key_code)
    log_info("keyboard_key_sent", "key_code=" .. tostring(key_code))
    device:send_keyboard(kavio.KeyOption.DOWN, key_code)
    device:send_keyboard(kavio.KeyOption.UP, key_code)
    set_status("Key " .. key_code .. " sent")
end

--- Emit the mouse left-down and left-up pair.
local function left_click()
    log_info("mouse_left_click_sent")
    device:send_mouse(kavio.MouseOption.LEFT_DOWN)
    device:send_mouse(kavio.MouseOption.LEFT_UP)
    set_status("Left click sent")
end

--- Clear the current TX surface without destroying it.
local function clear_tx()
    if tx_surface then
        log_info("tx_surface_clear_requested")
        tx_surface:clear()
        set_status("TX cleared")
    end
end

-- --- H.264 stream lifecycle ---
--- Request H.264 subscription, completed later via the callback.
local function start_h264()
    log_info("h264_subscribe_requested")
    device:subscribe_h264()
    set_status("H.264 subscribe requested")
end

--- Unsubscribe and close the capture output.
local function stop_h264()
    log_info("h264_stop_requested", "stream_id=" .. tostring(h264_stream_id))
    if h264_stream_id ~= 0 then
        device:unsubscribe_h264(h264_stream_id)
        h264_stream_id = 0
    end
    if h264_file then
        h264_file:close()
        h264_file = nil
    end
    set_status("H.264 stopped")
end

--- Update the demonstration configuration through the public JSON API.
local function update_config()
    local json = [[{"default_page":"home","render_fps":30,"show_metrics":true,"transfer_payload_kb":64}]]
    local ok, err = pcall(function()
        device:update_app_config_json(json)
    end)
    set_status(ok and "render_fps updated to 30" or err)
end

-- --- Page navigation and widget trees ---
--- Replace the current widget tree with a page from the shared protocol.
-- @param page_name string One of demo_protocol.pages.
show_page = function(page_name)
    log_info("page_changed", "from=" .. tostring(current_page) .. " to=" .. tostring(page_name))
    rx_live = false
    rx_live_button = nil
    rx_fps_label = nil
    rx_fps_count = 0
    rx_fps_window_start = 0
    last_rx_frame_id = nil
    rx_preview = nil
    current_page = page_name
    local root

    if page_name == "home" then
        root = shell(
            "Explore capabilities", "Lua  /  Interactive hardware API showcase",
            navigation()
        )
    elseif page_name == "render" then
        root = page_root(
            "Render",
            ui.Button("Touch/Widget active", function()
                set_status("Button clicked")
            end),
            ui.Slider(5, 60, 20, function(value)
                set_status("Render FPS " .. value)
            end),
            ui.Switch(true, function(enabled)
                set_status(enabled and "Metrics on" or "Metrics off")
            end)
        )
    elseif page_name == "transfer" then
        root = page_root(
            "Transfer",
            ui.Button("Send Hello", function()
                send_packet(proto.HELLO, "KavioDemo Lua")
            end),
            ui.Button("Send state report", function()
                send_packet(proto.STATE_REPORT, state_text())
            end)
        )
    elseif page_name == "hdmi_rx" then
        rx_preview = ui.Image()
        rx_fps_label = label("FPS: 0.0", 18, 0x78e6bb)
        rx_live_button = ui.Button("START LIVE", function()
            rx_live = not rx_live
            rx_fps_count = 0
            rx_fps_window_start = kavio.monotonic_seconds()
            rx_live_button:set_text(rx_live and "STOP LIVE" or "START LIVE")
            rx_fps_label:set_text("FPS: 0.0")
            set_status(rx_live and "RX live preview on" or "RX live preview paused")
        end)
        root = page_root(
            "HDMI RX",
            ui.Button("Read latest RX frame", function() read_rx_frame(false) end),
            rx_live_button,
            rx_fps_label,
            label("RX FRAME PREVIEW  /  native RGB565", 16, C.caption),
            rx_preview
        )
    elseif page_name == "hdmi_tx" then
        root = page_root(
            "HDMI TX",
            ui.Button("Create TX surface", function()
                device:create_tx_surface(320, 180)
            end),
            ui.Button("Clear TX", clear_tx)
        )
    elseif page_name == "keyboard" then
        root = page_root(
            "Keyboard",
            ui.Button("Send A", function() send_key(kavio.KeyCode.A) end),
            ui.Button("Send Enter", function() send_key(kavio.KeyCode.ENTER) end),
            ui.Button("Send Left", function() send_key(kavio.KeyCode.LEFT) end)
        )
    elseif page_name == "mouse" then
        root = page_root(
            "Mouse",
            ui.Button("Move center", function()
                device:send_mouse(kavio.MouseOption.MOVE, 640, 360)
            end),
            ui.Button("Left click", left_click),
            ui.Button("Wheel +", function()
                device:send_mouse(kavio.MouseOption.WHEEL, 0, 0, 120)
            end)
        )
    elseif page_name == "h264" then
        root = page_root(
            "H.264",
            ui.Button("Start capture", start_h264),
            ui.Button("Stop capture", stop_h264)
        )
    elseif page_name == "schema" then
        root = page_root(
            "App Configuration",
            ui.Button("Read config", function()
                set_status(device:get_app_config_json():sub(1, 160))
            end),
            ui.Button("Set render_fps=30", update_config),
            ui.Button("Reset defaults", function()
                device:reset_app_config()
            end)
        )
    else
        root = page_root(
            "Runtime",
            ui.Button("Refresh runtime", function()
                local runtime = device:runtime_data()
                set_status(string.format(
                    "RX %dx%d TX %dx%d",
                    runtime.rx.width,
                    runtime.rx.height,
                    runtime.tx.width,
                    runtime.tx.height
                ))
            end)
        )
    end

    backend:set_content(root)
end

-- --- Protocol packet dispatch ---
--- Parse and dispatch a received App Channel datagram.
-- @param data string Binary datagram supplied by the SDK.
local function handle_packet(data)
    local message_type, packet_sequence, payload, checksum = proto.parse(data)
    if not message_type then
        protocol_errors = protocol_errors + 1
        log_warn("app_channel_invalid_packet", "errors=" .. tostring(protocol_errors))
        return
    end

    if message_type == proto.HELLO then
        send_packet(proto.HELLO_ACK, "KavioDemo Lua", packet_sequence)
    elseif message_type == proto.PING then
        send_packet(proto.PONG, payload, packet_sequence)
    elseif message_type == proto.NAVIGATE and #payload == 1 then
        local page_index = payload:byte(1) + 1
        if proto.pages[page_index] then
            show_page(proto.pages[page_index])
        end
    elseif message_type == proto.TRANSFER_DATA then
        transfer_packets = transfer_packets + 1
        send_packet(
            proto.TRANSFER_ACK,
            proto.transfer_ack(#payload, checksum),
            packet_sequence
        )
    elseif message_type == proto.QUERY_STATE then
        send_packet(proto.STATE_REPORT, state_text(), packet_sequence)
    end
end

-- --- SDK lifecycle callbacks ---
app:on_ready(function(context)
    log_info("app_ready", "initializing device callbacks")
    device = context.device

    device:set_comm_data_callback(handle_packet)
    log_info("app_channel_callback_registered")
    device:set_tx_surface_callback(function(_, status, surface)
        if status ~= 0 or not surface then
            log_error("tx_surface_create_failed", "status=" .. tostring(status))
            set_status("TX failed " .. status)
            return
        end

        tx_surface = surface
        local info = surface:info()
        local row = string.rep(string.char(32, 120, 240, 255), info.width)
            .. string.rep("\0", math.max(0, info.stride - info.width * 4))
        local rows = {}
        for _ = 1, info.height do
            rows[#rows + 1] = row
        end

        surface:write(
            table.concat(rows),
            info.width,
            info.height,
            info.stride,
            kavio.PixelFormat.BGRA8888,
            0,
            0,
            {
                x = 40,
                y = 30,
                width = math.min(160, info.width - 40),
                height = math.min(100, info.height - 30),
            }
        )
        log_info("tx_surface_ready", string.format("surface_id=%s size=%dx%d",
            tostring(info.surface_id), info.width, info.height))
        set_status("TX surface " .. info.surface_id .. " ready")
    end)

    device:set_h264_stream_callback(function(_, status, info)
        if status ~= 0 then
            log_error("h264_subscribe_failed", "status=" .. tostring(status))
            set_status("H.264 failed " .. status)
            return
        end
        h264_stream_id = info.stream_id
        log_info("h264_stream_ready", "stream_id=" .. tostring(h264_stream_id))
        next_h264_seq = 0
        h264_file = io.open("/tmp/kavio_demo_lua.h264", "wb")
        set_status("H.264 stream " .. h264_stream_id)
    end)

    device:set_h264_invalidated_callback(function()
        log_warn("h264_stream_invalidated")
        set_status("H.264 invalidated")
    end)
    device:set_error_callback(function(event)
        log_error("device_error", event.message)
        set_status("Device error " .. tostring(event.message))
    end)
    device:set_config_changed_callback(function()
        log_info("app_config_changed")
        set_status("Config changed")
    end)

    backend:initialize(context.presentation)
    log_info("ui_backend_initialized")

    local config = device:get_app_config_json()
    local initial = config:match('"default_page"%s*:%s*"([^"]+)"') or "home"
    local valid = false
    for _, name in ipairs(proto.pages) do
        if name == initial then
            valid = true
            break
        end
    end
    show_page(valid and initial or "home")
    log_info("app_initialized", "initial_page=" .. tostring(current_page))
end)

app:on_tick(function()
    backend:process()
    tick_count = tick_count + 1
    if current_page == "hdmi_rx" and rx_live then
        read_rx_frame(true)
        local now = kavio.monotonic_seconds()
        local elapsed = now - rx_fps_window_start
        if elapsed >= 1 then
            local fps = rx_fps_count / elapsed
            rx_fps_count = 0
            rx_fps_window_start = now
            if rx_fps_label then
                rx_fps_label:set_text(string.format("FPS: %.1f", fps))
            end
        end
    end

    if device:is_comm_channel_ready() then
        if not hello_sent then
            send_packet(proto.HELLO, "KavioDemo Lua")
            hello_sent = true
        end
    else
        hello_sent = false
    end

    if current_page == "runtime" and tick_count % 20 == 0 then
        local runtime = device:runtime_data()
        set_status(string.format(
            "RX %dx%d TX %dx%d",
            runtime.rx.width,
            runtime.rx.height,
            runtime.tx.width,
            runtime.tx.height
        ))
    end

    if h264_stream_id ~= 0 and h264_file and tick_count % 4 == 0 then
        local latest = device:latest_h264_packet_seq(h264_stream_id)
        if next_h264_seq == 0 and latest > 0 then
            next_h264_seq = latest
        end

        while next_h264_seq > 0 and next_h264_seq <= latest do
            local ok, packet = pcall(function()
                return device:read_h264_packet(h264_stream_id, next_h264_seq)
            end)
            if not ok then
                break
            end

            local buffer = packet:buffer()
            h264_file:write(buffer:read())
            buffer:release()
            next_h264_seq = next_h264_seq + 1
        end
    end
end)

app:on_stopping(function()
    log_info("app_stopping")
    stop_h264()
    if tx_surface then
        pcall(function()
            tx_surface:request_destroy()
        end)
    end
    backend:shutdown()
    log_info("app_stopped")
end)

log_info("app_starting")
local exit_code = app:run()
log_info("app_exited", "exit_code=" .. tostring(exit_code))
os.exit(exit_code)
