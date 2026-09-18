# Runtime layering

The firmware uses three layers for device interaction:

```text
board hardware drivers
        │  direct Application method calls
        ▼
Application / services
        │  UI_EVENT (status, text, emotion, notification, display mode)
        ▼
UI event handler → model/presenter/view → Display/LVGL
```

Board implementations own GPIO, buses, codecs, panels, network drivers and
their hardware capability objects. A board invokes the appropriate public
method on `Application::GetInstance()` for button and hardware actions. Board
callbacks must not manipulate LVGL objects directly.

`Application` applies device-state rules and publishes `UI_EVENT` through
`UiEvents::Post*` when the screen needs to change. UI code subscribes to
`UI_EVENT` in `UiEventHandler`, marshals the operation through `UiManager`,
and updates models or views on the UI task.

When adding a board input:

1. Call the corresponding public `Application` method from the board callback.
   For the primary button, pass the detected click count to
   `Application::HandleButtonClick(count)`; its business rules live in
   `application_button.cc` and execute on the application task.
2. Add a focused public method to `Application` if no suitable action exists.
3. Publish a UI event from `Application` if visual feedback is required.

This keeps application behavior centralized while preventing LVGL calls from
running in arbitrary GPIO/timer callback contexts.
