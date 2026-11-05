-- Retained UI controls. Use Page/Column/Row to build a widget tree.
-- All UI updates must run on the application UI thread.
local native = require("kavio.ui_native")

local Style = {}
Style.__index = Style
setmetatable(Style, { __call = function(_, values) return Style.new(values) end })
function Style.new(values)
    values = values or {}
    return setmetatable({
        width = values.width,
        height = values.height,
        padding = values.padding,
        gap = values.gap,
        radius = values.radius,
        font_size = values.font_size,
        background = values.background,
        text_color = values.text_color,
        opacity = values.opacity,
    }, Style)
end

local State = {}
State.__index = State
setmetatable(State, { __call = function(_, value) return State.new(value) end })
function State.new(value)
    return setmetatable({ value = value, _subscribers = {} }, State)
end
function State:set(value)
    self.value = value
    for _, fn in ipairs(self._subscribers) do fn(value) end
end
function State:subscribe(fn)
    table.insert(self._subscribers, fn)
    fn(self.value)
    return self
end

local Widget = {}
Widget.__index = Widget
function Widget:_new(handle)
    return setmetatable({ _native = handle, _children = {}, _callbacks = {} }, self)
end
function Widget:add(...)
    for _, child in ipairs({...}) do
        self._native:add(child._native)
        table.insert(self._children, child)
    end
    return self
end
function Widget:style(style)
    self._native:set_style(style or {})
    return self
end

local function class(base)
    local value = {}
    value.__index = value
    return setmetatable(value, { __index = base, __call = function(cls, ...) return cls.new(...) end })
end

local Page = class(Widget)
function Page.new(...)
    local self = Widget._new(Page, native.widget_create(native.PAGE))
    return self:add(...)
end

local Column = class(Widget)
function Column.new(...)
    local self = Widget._new(Column, native.widget_create(native.COLUMN))
    return self:add(...)
end

local Row = class(Widget)
function Row.new(...)
    local self = Widget._new(Row, native.widget_create(native.ROW))
    return self:add(...)
end

local Label = class(Widget)
function Label.new(text)
    local initial = getmetatable(text) == State and text.value or text or ""
    local self = Widget._new(Label, native.widget_create(native.LABEL, tostring(initial)))
    if getmetatable(text) == State then text:subscribe(function(value) self:set_text(value) end) end
    return self
end
function Label:set_text(text) self._native:set_text(tostring(text)); return self end

local Button = class(Widget)
function Button.new(text, on_click)
    local self = Widget._new(Button, native.widget_create(native.BUTTON, tostring(text or "")))
    if on_click then self:on_click(on_click) end
    return self
end
function Button:set_text(text) self._native:set_text(tostring(text)); return self end
function Button:on_click(fn) table.insert(self._callbacks, fn); self._native:set_click_callback(fn); return self end

--- Image widget supporting either an image source or an SDK image frame.
-- set_buffer() copies the pixels; callers may release the frame afterward.
-- Call the setters from the UI thread.
local Image = class(Widget)
function Image.new(source) return Widget._new(Image, native.widget_create(native.IMAGE, tostring(source or ""))) end
--- Replace the current pixel buffer with an image source.
function Image:set_source(source) self._native:set_source(tostring(source)); return self end
--- Copy UYVY, YUYV, or BGRA8888 pixels while preserving the aspect ratio.
function Image:set_buffer(frame, max_width, max_height)
    self._native:set_buffer(frame, max_width or 620, max_height or 320)
    return self
end

local Slider = class(Widget)
function Slider.new(minimum, maximum, value, on_change)
    local self = Widget._new(Slider, native.widget_create(native.SLIDER, "", minimum or 0, maximum or 100, value or 0))
    if on_change then self:on_change(on_change) end
    return self
end
function Slider:value() return self._native:get_value() end
function Slider:set_value(value) self._native:set_value(value); return self end
function Slider:set_range(minimum, maximum) self._native:set_range(minimum, maximum); return self end
function Slider:on_change(fn) table.insert(self._callbacks, fn); self._native:set_value_callback(fn); return self end

local Switch = class(Widget)
function Switch.new(checked, on_change)
    local self = Widget._new(Switch, native.widget_create(native.SWITCH, "", 0, 100, 0, checked and true or false))
    if on_change then self:on_change(on_change) end
    return self
end
function Switch:checked() return self._native:get_checked() end
function Switch:set_checked(checked) self._native:set_checked(checked); return self end
function Switch:on_change(fn) table.insert(self._callbacks, fn); self._native:set_checked_callback(fn); return self end

local LvglBackend = {}
LvglBackend.__index = LvglBackend
setmetatable(LvglBackend, { __call = function(_) return LvglBackend.new() end })
function LvglBackend.new()
    return setmetatable({ _native = native.backend_create(), _presentation = nil, _root = nil }, LvglBackend)
end
function LvglBackend:initialize(presentation) self._native:initialize(presentation); self._presentation = presentation end
function LvglBackend:process() self._native:process() end
function LvglBackend:set_content(root) self._native:set_content(root._native); self._root = root end
function LvglBackend:clear_content() self._native:clear_content(); self._root = nil end
function LvglBackend:shutdown() self._native:shutdown(); self._root = nil; self._presentation = nil end
function LvglBackend:is_ready() return self._native:is_ready() end
function LvglBackend:is_foreground() return self._native:is_foreground() end

return {
    Style = Style,
    State = State,
    Page = Page,
    Column = Column,
    Row = Row,
    Label = Label,
    Button = Button,
    Image = Image,
    Slider = Slider,
    Switch = Switch,
    LvglBackend = LvglBackend,
}
