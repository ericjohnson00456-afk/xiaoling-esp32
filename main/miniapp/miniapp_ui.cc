#include "miniapp.h"
#include "miniapp_platform.h"
#include "ui/ui_manager.h"
#include "ui/ui_page_ids.h"
#include "ui/ui.h"
#include "ui/themes/lvgl_theme.h"
#include "display/lvgl_display.h"
#include <lvgl.h>
#include <mutex>
#include <atomic>

LV_FONT_DECLARE(font_puhui_basic_16_4);

namespace {
std::mutex scene_mutex;
miniapp_scene_t *pending = nullptr;
miniapp_scene_t *visible = nullptr;
lv_obj_t *canvas = nullptr;
std::atomic<bool> scheduled{false};
std::atomic<uint32_t> applied{0};
uint32_t submitted = 0;
const lv_font_t *Font() {
    auto *display = UiManager::GetInstance().display();
    auto *theme = display ? dynamic_cast<LvglTheme *>(display->GetTheme()) : nullptr;
    return theme && theme->text_font() ? theme->text_font()->font() : &font_puhui_basic_16_4;
}
void DrawShape(lv_layer_t *layer, const miniapp_shape_t &s, int x, int y) {
    if (!s.opacity)
        return;
    if (s.kind == MINIAPP_SHAPE_LINE) {
        lv_draw_line_dsc_t d;
        lv_draw_line_dsc_init(&d);
        d.p1 = {static_cast<lv_value_precise_t>(x + s.x), static_cast<lv_value_precise_t>(y + s.y)};
        d.p2 = {static_cast<lv_value_precise_t>(x + s.w), static_cast<lv_value_precise_t>(y + s.h)};
        d.color = lv_color_hex(s.color);
        d.opa = s.opacity;
        d.width = s.width;
        d.round_start = d.round_end = s.rounded;
        lv_draw_line(layer, &d);
    } else if (s.kind == MINIAPP_SHAPE_ARC) {
        if (s.start_angle == s.end_angle)
            return;
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center = {x + s.x, y + s.y};
        d.radius = s.w;
        d.start_angle = s.start_angle;
        d.end_angle = s.end_angle;
        d.color = lv_color_hex(s.color);
        d.opa = s.opacity;
        d.width = s.width;
        d.rounded = s.rounded;
        lv_draw_arc(layer, &d);
    } else {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(s.color);
        d.bg_opa = s.fill ? s.opacity : LV_OPA_TRANSP;
        d.border_color = lv_color_hex(s.border_color);
        d.border_width = s.width;
        d.border_opa = s.opacity;
        d.radius = s.kind == MINIAPP_SHAPE_CIRCLE ? LV_RADIUS_CIRCLE : s.radius;
        lv_area_t a =
            s.kind == MINIAPP_SHAPE_CIRCLE
                ? lv_area_t{x + s.x - s.w, y + s.y - s.w, x + s.x + s.w - 1, y + s.y + s.w - 1}
                : lv_area_t{x + s.x, y + s.y, x + s.x + s.w - 1, y + s.y + s.h - 1};
        lv_draw_rect(layer, &d, &a);
    }
}
void DrawText(lv_layer_t *layer, const miniapp_text_t &t, int x, int y) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = Font();
    d.color = lv_color_hex(t.color);
    d.opa = t.opacity;
    d.text = t.text;
    d.text_local = true;
    static const lv_text_align_t align[] = {LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER,
                                            LV_TEXT_ALIGN_RIGHT};
    d.align = align[t.align];
    lv_point_t size;
    lv_text_get_size(&size, t.text, d.font, 0, 0, t.width ? t.width : LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_area_t a{x + t.x, y + t.y, x + t.x + (t.width ? t.width : size.x) - 1, y + t.y + size.y - 1};
    lv_draw_label(layer, &d, &a);
}
void Draw(lv_event_t *event) {
    if (!visible)
        return;
    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    auto *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(visible->background);
    bg.bg_opa = LV_OPA_COVER;
    lv_draw_rect(layer, &bg, &area);
    unsigned shape = 0, text = 0;
    while (shape < visible->shape_count || text < visible->text_count) {
        if (shape < visible->shape_count &&
            (text == visible->text_count || !visible->ordered ||
             visible->shapes[shape].order < visible->texts[text].order)) {
            DrawShape(layer, visible->shapes[shape++], area.x1, area.y1);
        } else
            DrawText(layer, visible->texts[text++], area.x1, area.y1);
    }
}
int Open(const UiNavScreen *, void **) {
    auto *display = UiManager::GetInstance().lvgl_display();
    if (!display)
        return -1;
    canvas = lv_obj_create(lv_display_get_screen_active(display->GetLvglDisplay()));
    lv_obj_remove_style_all(canvas);
    lv_obj_set_size(canvas, MINIAPP_SCREEN_WIDTH, MINIAPP_SCREEN_HEIGHT);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas, Draw, LV_EVENT_DRAW_MAIN, nullptr);
    return 0;
}
int Show(const UiNavScreen *, void *) {
    if (!canvas)
        return -1;
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(canvas);
    return 0;
}
int Hide(const UiNavScreen *, void *) {
    if (canvas)
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    return 0;
}
int Close(const UiNavScreen *, void *) {
    if (canvas)
        lv_obj_delete(canvas);
    canvas = nullptr;
    miniapp_runtime_ui_closed();
    return 0;
}
} // namespace
extern "C" int miniapp_ui_init() {
    if (!miniapp_has_screen())
        return 0;
    pending = static_cast<miniapp_scene_t *>(miniapp_calloc(1, sizeof(*pending)));
    visible = static_cast<miniapp_scene_t *>(miniapp_calloc(1, sizeof(*visible)));
    if (!pending || !visible) {
        free(pending);
        free(visible);
        pending = visible = nullptr;
        return -1;
    }
    static const UiNavScreen screen{UI_PAGE_MINIAPP, Open, Show, Hide, Show, Close};
    return UiManager::GetInstance().RegisterScreen(&screen);
}
extern "C" int miniapp_ui_open() {
    UiManager::GetInstance().Dispatch(
        []() { UiManager::GetInstance().NavigateTo(UI_PAGE_MINIAPP); });
    return 0;
}
extern "C" int miniapp_ui_present(const miniapp_scene_t *scene) {
    if (!miniapp_has_screen())
        return 0;
    if (!pending || !visible)
        return -1;
    {
        std::lock_guard<std::mutex> lock(scene_mutex);
        miniapp_scene_copy(pending, scene);
        ++submitted;
    }
    if (!scheduled.exchange(true)) {
        UiManager::GetInstance().Dispatch([]() {
            auto *display = UiManager::GetInstance().display();
            DisplayLockGuard guard(display);
            {
                std::lock_guard<std::mutex> lock(scene_mutex);
                miniapp_scene_copy(visible, pending);
                applied = submitted;
                scheduled = false;
            }
            if (guard.locked() && canvas)
                lv_obj_invalidate(canvas);
        });
    }
    return 0;
}
extern "C" int miniapp_ui_wait_frame(uint32_t timeout) {
    // FIFO barrier confirms that the candidate page opened successfully.
    auto done = std::make_shared<std::atomic<int>>(0);
    UiManager::GetInstance().Dispatch([done]() { *done = canvas && applied.load() ? 1 : -1; });
    auto start = xTaskGetTickCount();
    while (!done->load() && xTaskGetTickCount() - start < pdMS_TO_TICKS(timeout))
        vTaskDelay(1);
    return done->load() == 1 ? 0 : -1;
}
extern "C" int miniapp_ui_close() {
    UiManager::GetInstance().Dispatch([]() {
        if (UiManager::GetInstance().TopScreenId() == UI_PAGE_MINIAPP)
            UiManager::GetInstance().NavigateHome();
    });
    return 0;
}
