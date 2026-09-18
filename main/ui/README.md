# UI 架构

`main/ui` 按职责分层。依赖方向从上到下，底层不得反向依赖页面或业务模块。

```text
Application / Board / Cloud
            |
          events
            |
       presenters
            |
          views
            |
        display backend
            |
        LVGL / esp_lcd

  resources ---------> views
```

## 目录职责

- `core/`：UI 初始化、任务调度、页面注册和导航栈。
- `events/`：跨任务 UI 事件定义和事件入口。事件处理器不直接持有页面控件。
- `resources/`：可复用的资源仓库（当前为表情动画资源），不保存页面状态，也不创建 LVGL 控件。
- `screens/`：每个页面由 Presenter 保存外部推送的状态，View 创建并直接绘制 LVGL 控件。
- `display/`：显示后端。它负责 panel 句柄、`lvgl_port`、flush、缓冲区、锁和显示能力。
- `themes/`：主题、字体和主题资源选择。
- `tools/`：图片、字体和动画等 UI 基础工具。

`display/panels/` 中的 LCD/OLED 类正在从旧版兼容实现迁移为纯面板后端。页面控件应放在
`screens/` 或 `widgets/`；新增代码不得在面板类中增加聊天、业务状态或页面导航逻辑。

所有 LVGL API 必须在 UI 任务或持有显示锁的上下文中调用。跨任务更新通过
`UiManager::Dispatch()` 或 `UiEvents` 投递，不能从网络、音频或板级任务直接操作控件。
