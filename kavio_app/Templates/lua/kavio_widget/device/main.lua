local kavio = require("kavio")
local ui = require("kavio.ui")

local status = ui.State("Ready")

local root = ui.Column(
    ui.Label("{{APP_NAME}}"),
    ui.Label(status),
    ui.Button("Press me", function()
        status:set("Button clicked")
    end),
    ui.Slider(0, 100, 50, function(value)
        status:set("Slider: " .. value)
    end),
    ui.Switch(false, function(checked)
        status:set(checked and "Switch: on" or "Switch: off")
    end)
)

os.exit(kavio.App({content = root}):run())
