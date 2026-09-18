# Runtime layering

The firmware uses three layers for device interaction:

```text
board hardware drivers
        │  direct Application method calls
        ▼
Application / services
        │  UiApp (status, text, emotion, notification, display mode)
        ▼
UiManager dispatch → presenter/view → Display/LVGL
```

Board implementations own GPIO, buses, codecs, panels, network drivers and
their hardware capability objects. A board invokes the appropriate public
method on `Application::GetInstance()` for button and hardware actions. Board
callbacks must not manipulate LVGL objects directly.

`Application` applies device-state rules and calls the corresponding `UiApp`
method when the screen needs to change. `UiApp` marshals the operation through
`UiManager` and updates presenters or views on the application task.

When adding a board input:

1. Call the corresponding public `Application` method from the board callback.
   For the primary button, pass the detected click count to
   `Application::HandleButtonClick(count)`; its business rules live in
   `application_button.cc` and execute on the application task.
2. Add a focused public method to `Application` if no suitable action exists.
3. Call the corresponding `UiApp` method from `Application` if visual feedback
   is required.

This keeps application behavior centralized while preventing LVGL calls from
running in arbitrary GPIO/timer callback contexts.
