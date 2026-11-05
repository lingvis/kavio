from kavio import App, ui


status = ui.State("Ready")
root = ui.Column(
    ui.Label("{{APP_NAME}}"),
    ui.Label(status),
    ui.Button(
        "Press me",
        on_click=lambda: status.set("Button clicked"),
    ),
    ui.Slider(
        0,
        100,
        50,
        on_change=lambda value: status.set(f"Slider: {value}"),
    ),
    ui.Switch(
        False,
        on_change=lambda enabled: status.set(
            "Switch: on" if enabled else "Switch: off"
        ),
    ),
)

raise SystemExit(App(content=root).run())
