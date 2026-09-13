# Runtime layering

The firmware uses three one-way layers for device interaction:

```text
board hardware drivers
        │  BOARD_EVENT (button, power, network-switch requests)
        ▼
Application / services
        │  UI_EVENT (status, text, emotion, notification, display mode)
        ▼
UI event handler → model/presenter/view → Display/LVGL
```

Board implementations own GPIO, buses, codecs, panels, network drivers and
their hardware capability objects. A board publishes `BOARD_EVENT` for input
or hardware state changes; it must not call `Application`, `McpServer`, a UI
model, or a display presentation method from a button/timer callback.

`Application` subscribes to board events and applies device-state rules. It
publishes `UI_EVENT` through `UiEvents::Post*` when the screen needs to
change. UI code subscribes to `UI_EVENT` in `UiEventHandler`, marshals the
operation through `UiManager`, and updates models or views on the UI task.

When adding a board input:

1. Add a semantic input to `BoardEvents::Input`.
2. Publish it from the board callback with `BoardEvents::PostInput`.
3. Handle it in `Application::HandleBoardInput`.
4. Publish a UI event if visual feedback is required.

This keeps board code independent of application state and prevents LVGL
calls from running in arbitrary GPIO/timer callback contexts.
