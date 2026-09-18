#pragma once

#include <stdint.h>

/*
 * 页面导航栈（移植自 lisa_ui 的 lisa_ui_nav_scr.c）。
 *
 * 每个页面是一个静态的 UiNavScreen 描述符（通常放全局/静态存储），通过 5 个
 * 生命周期回调与导航栈交互：
 *   open   - 页面第一次进入时调用，分配页面私有数据并写入 *data
 *   show   - 页面成为栈顶并显示时调用
 *   pause  - 页面被新页面覆盖（仍在栈中）时调用
 *   resume - 覆盖页面退出、本页面重新回到栈顶时调用
 *   close  - 页面被弹出栈时调用，释放 open 分配的私有数据
 *
 * 纯数据结构 + 单向链表实现，不依赖 LVGL / FreeRTOS，线程安全由调用方保证
 * （通常由 UiManager 在持有 display lock 时调用）。
 */
struct UiNavScreen {
    int unique_id;
    int (*open)(const UiNavScreen* scr, void** data);
    int (*show)(const UiNavScreen* scr, void* data);
    int (*pause)(const UiNavScreen* scr, void* data);
    int (*resume)(const UiNavScreen* scr, void* data);
    int (*close)(const UiNavScreen* scr, void* data);
};

/* 注册页面描述符，之后可按 unique_id 导航。重复注册同一 id 后者会先生效。 */
int ui_nav_scr_add(const UiNavScreen* scr);

/* 清空整个栈并把 scr 设为栈底默认页（依次 open -> push -> show） */
int ui_nav_scr_default_set(const UiNavScreen* scr);
int ui_nav_scr_default_set_by_id(int id);

/*
 * 导航到指定页面：
 * - 目标已在栈中：把它之上的页面全部 pause+close 弹出，然后 resume 目标页
 * - 目标不在栈中：pause 当前页 -> open 目标页 -> 压栈 -> show
 */
int ui_nav_scr_nav_to(int unique_id);

/* 返回上一页：栈顶页面 pause -> 下一页 resume -> 栈顶 close 弹出。栈底页不可退出。 */
int ui_nav_scr_nav_back();

/* 一路退出到栈底默认页（中间页面全部 pause+close，默认页 resume） */
int ui_nav_scr_nav_default();

/* 当前栈顶页面 id，栈空返回 -1 */
int ui_nav_scr_get_top_id();
