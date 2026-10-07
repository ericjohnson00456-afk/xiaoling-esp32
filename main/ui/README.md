# UI 架构

`main/ui` 按职责分层。依赖方向从上到下，底层不得反向依赖页面或业务模块。

```text
Application / Board / Cloud
            |
        Ui 命名空间
            |
       UI 命令队列
            |
         UI 专用任务
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

- UI 顶层：`ui.h` 是业务模块的唯一入口，提供 `Ui::Initialize()` 和线程安全的 `Ui::XXX()` 接口。
- `UiManager`：仅供 UI 内部使用，保存显示设备、导航栈，以及 UI 专用任务的 FIFO 命令队列。
- `screens/`：每个页面由 Presenter 保存外部推送的状态，View 创建并直接绘制 LVGL 控件。
- `display/`：显示后端。它负责 panel 句柄、`lvgl_port`、flush、缓冲区、锁和显示能力。
- `themes/`：主题、字体和主题资源选择。
- `utils/`：图片、字体、表情资源和动画等 UI 基础工具。

`display/panels/` 中的 LCD/OLED 类正在从旧版兼容实现迁移为纯面板后端。页面控件应放在
`screens/` 或 `widgets/`；新增代码不得在面板类中增加聊天、业务状态或页面导航逻辑。

所有 LVGL API 必须在 UI 任务或持有显示锁的上下文中调用。网络、音频和闹钟等业务代码只能
调用 `Ui::XXX()`；不得直接访问 `UiManager`、Presenter、View 或 LVGL 控件。尚未迁移的旧板型
代码由兼容层转发，新增板型代码应直接调用 `Ui::XXX()`。

LCD 与 SD 共用 SPI 的板型会把一次完整 LVGL 刷新的全部分段视为同一总线事务，
只允许 SD 在两次刷新之间插入命令，避免动画帧被读卡事务切成可见条带。

UI 初始化完成后，`UiManager` 创建一个专用任务。外部调用会把命令放进同一个 FIFO 队列，
UI 任务按顺序更新 Presenter 和 View。UI 不借用 `Application` 主任务，也不需要业务层注入调度器。

业务命令只在 `ui.cc` 入队一次；Presenter 默认已经运行在 UI 任务中，直接更新状态和 View。
只有定时器、资源下载、事件总线等异步回调需要重新投递到 UI 任务。

`compat/legacy_display_adapter` 仅用于尚未迁移的旧板型代码，将 `Display::SetXXX()` 转成
`Ui::XXX()`；新代码不得继续增加对旧 Display 业务接口的调用。
