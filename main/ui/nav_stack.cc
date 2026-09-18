#include "nav_stack.h"

#include <stdlib.h>
#include <mutex>

#include <esp_log.h>

#define TAG "ui_nav"

namespace {

struct ScrNode {
    const UiNavScreen* scr;
    ScrNode* next;
};

struct ScrStackNode {
    const UiNavScreen* scr;
    void* data;
    ScrStackNode* next;
};

ScrNode* scr_list_head = nullptr;
ScrStackNode* scr_stack_top = nullptr;
// Navigation callbacks may synchronously request another navigation. A
// recursive mutex keeps the stack consistent while still allowing that
// documented callback pattern.
std::recursive_mutex nav_mutex;

const UiNavScreen* FindScrById(int unique_id) {
    ScrNode* node = scr_list_head;
    while (node) {
        if (node->scr->unique_id == unique_id) {
            return node->scr;
        }
        node = node->next;
    }
    return nullptr;
}

int StackPush(const UiNavScreen* scr, void* data) {
    if (!scr) {
        ESP_LOGE(TAG, "stack push: scr is NULL");
        return -1;
    }

    ScrStackNode* node = static_cast<ScrStackNode*>(malloc(sizeof(ScrStackNode)));
    if (!node) {
        ESP_LOGE(TAG, "stack push: malloc failed");
        return -1;
    }

    node->scr = scr;
    node->next = scr_stack_top;
    node->data = data;
    scr_stack_top = node;

    return 0;
}

const UiNavScreen* StackPop(void** data) {
    if (!scr_stack_top) {
        ESP_LOGE(TAG, "stack pop: stack is empty");
        return nullptr;
    }

    ScrStackNode* node = scr_stack_top;
    const UiNavScreen* scr = node->scr;
    *data = node->data;

    scr_stack_top = node->next;
    free(node);

    return scr;
}

const UiNavScreen* StackTop(void** data) {
    if (!scr_stack_top) {
        ESP_LOGE(TAG, "stack top: stack is empty");
        return nullptr;
    }

    if (data) {
        *data = scr_stack_top->data;
    }

    return scr_stack_top->scr;
}

int StackHasId(int unique_id) {
    ScrStackNode* node = scr_stack_top;
    while (node) {
        if (node->scr && node->scr->unique_id == unique_id) {
            return 1;
        }
        node = node->next;
    }
    return 0;
}

} // namespace

int ui_nav_scr_nav_to(int unique_id) {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    void* from_data = nullptr;
    void* to_data = nullptr;

    const UiNavScreen* from = StackTop(&from_data);
    const UiNavScreen* to = FindScrById(unique_id);

    if (from == nullptr) {
        ESP_LOGE(TAG, "nav_to: from scr not found");
        return -1;
    }

    if (to == nullptr) {
        ESP_LOGE(TAG, "nav_to: to scr not found, id=%d", unique_id);
        return -1;
    }

    if (from->unique_id == unique_id) {
        return 0;
    }

    if (StackHasId(unique_id)) {
        while (scr_stack_top && scr_stack_top->scr && scr_stack_top->scr->unique_id != unique_id) {
            from = StackPop(&from_data);
            if (from == nullptr) {
                ESP_LOGE(TAG, "nav_to: failed to pop stack to id=%d", unique_id);
                return -1;
            }

            if (from->pause) {
                from->pause(from, from_data);
            }

            if (from->close) {
                from->close(from, from_data);
            }
        }

        to = StackTop(&to_data);
        if (to == nullptr) {
            ESP_LOGE(TAG, "nav_to: target scr not found after pop, id=%d", unique_id);
            return -1;
        }
        if (to->resume) {
            to->resume(to, to_data);
        }
        return 0;
    }

    if (to->open == nullptr) {
        return -1;
    }

    if (from->pause) {
        from->pause(from, from_data);
    }

    if (to->open(to, &to_data) != 0) {
        if (from->resume) {
            from->resume(from, from_data);
        }
        return -1;
    }

    if (StackPush(to, to_data) != 0) {
        if (to->close) {
            to->close(to, to_data);
        }
        if (from->resume) {
            from->resume(from, from_data);
        }
        return -1;
    }

    if (to->show) {
        to->show(to, to_data);
    }

    return 0;
}

int ui_nav_scr_nav_back() {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    void* from_data = nullptr;
    void* to_data = nullptr;

    if (!scr_stack_top) {
        ESP_LOGE(TAG, "nav_back: stack is empty");
        return -1;
    }

    if (!scr_stack_top->next) {
        ESP_LOGW(TAG, "nav_back: only one node in stack, cannot go back");
        return -1;
    }

    const UiNavScreen* from = StackPop(&from_data);
    const UiNavScreen* to = StackTop(&to_data);

    if (from == nullptr) {
        ESP_LOGE(TAG, "nav_back: from scr not found");
        return -1;
    }

    if (to == nullptr) {
        ESP_LOGE(TAG, "nav_back: to scr not found");
        return -1;
    }

    if (from->pause) {
        from->pause(from, from_data);
    }

    if (to->resume) {
        to->resume(to, to_data);
    }

    if (from->close) {
        from->close(from, from_data);
    }

    return 0;
}

int ui_nav_scr_nav_default() {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    void* to_data = nullptr;

    if (!scr_stack_top) {
        ESP_LOGE(TAG, "nav_default: stack is empty");
        return -1;
    }

    if (!scr_stack_top->next) {
        ESP_LOGW(TAG, "nav_default: only one node in stack, already at default");
        return 0;
    }

    while (scr_stack_top && scr_stack_top->next) {
        void* temp_data = nullptr;
        const UiNavScreen* temp_scr = StackPop(&temp_data);
        if (temp_scr && temp_scr->pause) {
            temp_scr->pause(temp_scr, temp_data);
        }
        if (temp_scr && temp_scr->close) {
            temp_scr->close(temp_scr, temp_data);
        }
    }

    const UiNavScreen* to = StackTop(&to_data);
    if (to == nullptr) {
        ESP_LOGE(TAG, "nav_default: default scr not found");
        return -1;
    }

    if (to->resume) {
        to->resume(to, to_data);
    }

    return 0;
}

int ui_nav_scr_default_set(const UiNavScreen* scr) {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    if (scr == nullptr) {
        ESP_LOGE(TAG, "default_set: scr is NULL");
        return -1;
    }

    while (scr_stack_top) {
        void* temp_data = nullptr;
        const UiNavScreen* temp_scr = StackPop(&temp_data);
        if (temp_scr && temp_scr->close) {
            temp_scr->close(temp_scr, temp_data);
        }
    }

    void* data = nullptr;

    if (scr->open) {
        scr->open(scr, &data);
    }

    if (StackPush(scr, data) != 0) {
        ESP_LOGE(TAG, "default_set: failed to push scr to stack");
        return -1;
    }

    if (scr->show) {
        scr->show(scr, data);
    }

    return 0;
}

int ui_nav_scr_default_set_by_id(int id) {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    return ui_nav_scr_default_set(FindScrById(id));
}

int ui_nav_scr_add(const UiNavScreen* scr) {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    if (!scr) {
        ESP_LOGE(TAG, "scr is NULL");
        return -1;
    }

    ScrNode* node = static_cast<ScrNode*>(malloc(sizeof(ScrNode)));
    if (!node) {
        ESP_LOGE(TAG, "malloc failed for scr_node");
        return -1;
    }

    node->scr = scr;
    node->next = scr_list_head;
    scr_list_head = node;

    return 0;
}

int ui_nav_scr_get_top_id() {
    std::lock_guard<std::recursive_mutex> lock(nav_mutex);
    const UiNavScreen* scr = StackTop(nullptr);
    if (!scr) {
        ESP_LOGE(TAG, "get_top_id: stack is empty");
        return -1;
    }

    return scr->unique_id;
}
