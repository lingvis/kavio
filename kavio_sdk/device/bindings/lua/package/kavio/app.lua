-- Application lifecycle: create an App, register callbacks, and call run().
-- Callbacks receive the current device and presentation via context.
local native = require("kavio.native")

local App = {}
App.__index = App

setmetatable(App, {
    __call = function(_, options)
        return App.new(options)
    end,
})

function App.new(options)
    options = options or {}
    local handle = native.app_create(options)
    local self = setmetatable({
        _handle = handle,
        presentation = handle:presentation(),
        device = handle:device(),
        _ready_handlers = {},
        _tick_handlers = {},
        _stopping_handlers = {},
        _content = options.content,
        _ui_backend = nil,
    }, App)
    self.context = {
        presentation = self.presentation,
        device = self.device,
    }
    return self
end

function App:on_ready(handler)
    assert(type(handler) == "function", "handler must be a function")
    table.insert(self._ready_handlers, handler)
    return self
end

function App:on_tick(handler)
    assert(type(handler) == "function", "handler must be a function")
    table.insert(self._tick_handlers, handler)
    return self
end

function App:on_stopping(handler)
    assert(type(handler) == "function", "handler must be a function")
    table.insert(self._stopping_handlers, handler)
    return self
end

function App:set_content(content)
    self._content = content
    return self
end

function App:request_stop()
    self._handle:request_stop()
end

function App:last_status()
    return self._handle:last_status()
end

function App:_ready()
    if self._content ~= nil then
        local ui = require("kavio.ui")
        self._ui_backend = ui.LvglBackend()
        self._ui_backend:initialize(self.presentation)
        self._ui_backend:set_content(self._content)
    end
    for _, handler in ipairs(self._ready_handlers) do
        handler(self.context)
    end
end

function App:_tick()
    if self._ui_backend ~= nil then
        self._ui_backend:process()
    end
    for _, handler in ipairs(self._tick_handlers) do
        handler(self.context)
    end
end

function App:_stopping()
    local ok, err = pcall(function()
        for _, handler in ipairs(self._stopping_handlers) do
            handler(self.context)
        end
    end)
    if self._ui_backend ~= nil then
        self._ui_backend:shutdown()
        self._ui_backend = nil
    end
    if not ok then
        error(err, 0)
    end
end

function App:run()
    self._handle:set_ready_callback(function() self:_ready() end)
    self._handle:set_tick_callback(function() self:_tick() end)
    self._handle:set_stopping_callback(function() self:_stopping() end)
    return self._handle:run()
end

return App
