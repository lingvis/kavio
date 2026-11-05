local kavio = require("kavio")

local app = kavio.App()

local function make_checker_row(width, first, second)
    local pixels = {}
    for x = 0, width - 1 do
        local color = (math.floor(x / 32) % 2 == 0) and first or second
        pixels[#pixels + 1] = string.char(color[1], color[2], color[3], 255)
    end
    return table.concat(pixels)
end

local function draw_background(presentation)
    local frame = presentation:acquire_frame()
    local info = frame:info()
    local row_a = make_checker_row(info.width, {48, 98, 190}, {26, 50, 96})
    local row_b = make_checker_row(info.width, {26, 50, 96}, {48, 98, 190})

    frame:with_buffer(function(buffer)
        for y = 0, info.height - 1 do
            local row = (math.floor(y / 32) % 2 == 0) and row_a or row_b
            buffer:write(y * info.stride, row)
        end
    end)

    frame:present()
end

local function draw_touch_marker(presentation, x, y)
    local frame = presentation:acquire_frame()
    local info = frame:info()
    local x0 = math.max(0, x - 18)
    local y0 = math.max(0, y - 18)
    local x1 = math.min(info.width, x + 18)
    local y1 = math.min(info.height, y + 18)
    local white = string.rep(string.char(255, 255, 255, 255), x1 - x0)

    frame:with_buffer(function(buffer)
        for yy = y0, y1 - 1 do
            buffer:write(yy * info.stride + x0 * 4, white)
        end
    end)

    frame:present({{
        x = x0,
        y = y0,
        width = x1 - x0,
        height = y1 - y0,
    }})
end

app:on_ready(function(context)
    -- Demonstrate a cancelled write transaction before the first published frame.
    local discarded = context.presentation:acquire_frame()
    discarded:discard()

    draw_background(context.presentation)
    context.presentation:set_touch_callback(function(event)
        for _, point in ipairs(event.points or {}) do
            if point.active and point.changed then
                draw_touch_marker(context.presentation, point.x, point.y)
                break
            end
        end
    end)
end)

os.exit(app:run())
