#include "miniapp_actions.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    cJSON *actions =
        cJSON_Parse("{\"set\":{\"description\":\"Set "
                    "value\",\"parameters\":{\"n\":{\"type\":\"integer\",\"required\":true,"
                    "\"minimum\":1,\"maximum\":10},\"enabled\":{\"type\":\"boolean\"}}}}");
    assert(miniapp_actions_valid(actions));
    const char *invalid[] = {"{}",
                             "{\"n\":0}",
                             "{\"n\":11}",
                             "{\"n\":1.1}",
                             "{\"n\":\"2\"}",
                             "{\"n\":2,\"extra\":true}",
                             "{\"n\":2,\"n\":3}",
                             "{\"n\":2,\"enabled\":0}",
                             "{\"n\":null}",
                             "[]"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        cJSON *args = cJSON_Parse(invalid[i]);
        assert(miniapp_action_validate(actions, "set", args));
        cJSON_Delete(args);
    }
    cJSON *args = cJSON_Parse("{\"n\":10,\"enabled\":false}");
    assert(!miniapp_action_validate(actions, "set", args));
    assert(miniapp_action_validate(actions, "SET", args));
    cJSON_Delete(args);
    cJSON *number_schema = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(actions, "set"), "parameters");
    number_schema = cJSON_GetObjectItemCaseSensitive(number_schema, "n");
    cJSON_AddItemToObject(number_schema, "enum", cJSON_Parse("[2,4]"));
    args = cJSON_Parse("{\"n\":3}");
    assert(miniapp_action_validate(actions, "set", args));
    cJSON_Delete(args);
    args = cJSON_Parse("{\"n\":4}");
    assert(!miniapp_action_validate(actions, "set", args));
    cJSON_Delete(args);
    cJSON_DeleteItemFromObjectCaseSensitive(number_schema, "enum");
    cJSON *parameter = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(actions, "set"), "parameters");
    cJSON *schema = cJSON_AddObjectToObject(parameter, "text");
    cJSON_AddStringToObject(schema, "type", "string");
    char *text = malloc(8193);
    memset(text, 'x', 8192);
    text[8192] = 0;
    args = cJSON_Parse("{\"n\":1}");
    cJSON_AddStringToObject(args, "text", text);
    assert(miniapp_action_validate(actions, "set", args));
    free(text);
    cJSON_Delete(args);
    cJSON_AddBoolToObject(schema, "unsupported", true);
    assert(!miniapp_actions_valid(actions));
    cJSON_Delete(actions);
    puts("miniapp action validation passed");
}
