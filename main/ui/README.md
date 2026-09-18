# UI 架构

`main/ui` 按职责分层。依赖方向从上到下，底层不得反向依赖页面或业务模块。

```text
Application / Board / Cloud
            |
          UiApp
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

- UI 顶层：`UiApp` 负责初始化、跨任务业务入口，`UiManager` 负责任务调度、页面注册和导航栈。
- `screens/`：每个页面由 Presenter 保存外部推送的状态，View 创建并直接绘制 LVGL 控件。
- `display/`：显示后端。它负责 panel 句柄、`lvgl_port`、flush、缓冲区、锁和显示能力。
- `themes/`：主题、字体和主题资源选择。
- `utils/`：图片、字体、表情资源和动画等 UI 基础工具。

`display/panels/` 中的 LCD/OLED 类正在从旧版兼容实现迁移为纯面板后端。页面控件应放在
`screens/` 或 `widgets/`；新增代码不得在面板类中增加聊天、业务状态或页面导航逻辑。

所有 LVGL API 必须在 UI 任务或持有显示锁的上下文中调用。跨任务业务更新通过
`UiApp` 入口调用，UI 内部通过 `UiManager::Dispatch()` 调度；不能从网络、音频或板级任务直接操作控件。
