#include "miniapp_actions.h"

#include <math.h>
#include <string.h>

static bool text_valid(const char *text, size_t maximum, bool nonempty) {
    return text && (!nonempty || *text) && strlen(text) <= maximum;
}

/* Reject duplicate members instead of validating one value and executing another. */
static bool object_valid(const cJSON *object, unsigned maximum) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    unsigned count = 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        if (++count > maximum || !text_valid(item->string, MINIAPP_ACTION_NAME_MAX, true)) {
            return false;
        }
        for (const cJSON *previous = object->child; previous != item; previous = previous->next) {
            if (!strcmp(previous->string, item->string)) {
                return false;
            }
        }
    }
    return true;
}

static bool type_matches(const cJSON *value, const char *type) {
    if (!strcmp(type, "string")) {
        return cJSON_IsString(value);
    }
    if (!strcmp(type, "boolean")) {
        return cJSON_IsBool(value);
    }
    if (!strcmp(type, "number")) {
        return cJSON_IsNumber(value) && isfinite(value->valuedouble);
    }
    if (!strcmp(type, "integer")) {
        return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
               floor(value->valuedouble) == value->valuedouble &&
               fabs(value->valuedouble) <= 9007199254740991.0;
    }
    return false;
}

static bool value_valid(const cJSON *value, const cJSON *schema) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (!type_matches(value, type->valuestring)) {
        return false;
    }
    const cJSON *min = cJSON_GetObjectItemCaseSensitive(schema, "minimum");
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(schema, "maximum");
    if (min && value->valuedouble < min->valuedouble) {
        return false;
    }
    if (max && value->valuedouble > max->valuedouble) {
        return false;
    }
    const cJSON *options = cJSON_GetObjectItemCaseSensitive(schema, "enum");
    if (options) {
        for (const cJSON *option = options->child; option; option = option->next) {
            if (cJSON_Compare(value, option, true)) {
                return true;
            }
        }
        return false;
    }
    return true;
}

static bool parameter_valid(const cJSON *schema) {
    if (!object_valid(schema, 6)) {
        return false;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(schema, "type");
    if (!cJSON_IsString(type)) {
        return false;
    }
    bool numeric = !strcmp(type->valuestring, "integer") || !strcmp(type->valuestring, "number");
    if (!numeric && strcmp(type->valuestring, "string") && strcmp(type->valuestring, "boolean")) {
        return false;
    }
    for (const cJSON *field = schema->child; field; field = field->next) {
        if (!strcmp(field->string, "type")) {
            continue;
        }
        if (!strcmp(field->string, "description")) {
            if (!cJSON_IsString(field) ||
                !text_valid(field->valuestring, MINIAPP_ACTION_DESCRIPTION_MAX, false)) {
                return false;
            }
        } else if (!strcmp(field->string, "required")) {
            if (!cJSON_IsBool(field)) {
                return false;
            }
        } else if (!strcmp(field->string, "minimum") || !strcmp(field->string, "maximum")) {
            if (!numeric || !cJSON_IsNumber(field) || !isfinite(field->valuedouble)) {
                return false;
            }
        } else if (!strcmp(field->string, "enum")) {
            if (!cJSON_IsArray(field) || !field->child || cJSON_GetArraySize(field) > 32) {
                return false;
            }
            for (const cJSON *option = field->child; option; option = option->next) {
                if (!type_matches(option, type->valuestring)) {
                    return false;
                }
            }
        } else {
            return false; /* Never advertise a constraint we cannot enforce. */
        }
    }
    const cJSON *min = cJSON_GetObjectItemCaseSensitive(schema, "minimum");
    const cJSON *max = cJSON_GetObjectItemCaseSensitive(schema, "maximum");
    return !min || !max || min->valuedouble <= max->valuedouble;
}

bool miniapp_actions_valid(const cJSON *actions) {
    if (!object_valid(actions, MINIAPP_ACTIONS_MAX)) {
        return false;
    }
    for (const cJSON *action = actions->child; action; action = action->next) {
        if (!object_valid(action, 2) || cJSON_GetArraySize(action) != 2) {
            return false;
        }
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(action, "description");
        const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(action, "parameters");
        if (!cJSON_IsString(description) ||
            !text_valid(description->valuestring, MINIAPP_ACTION_DESCRIPTION_MAX, true) ||
            !object_valid(parameters, MINIAPP_ACTION_PARAMS_MAX)) {
            return false;
        }
        for (const cJSON *parameter = parameters->child; parameter; parameter = parameter->next) {
            if (!parameter_valid(parameter)) {
                return false;
            }
        }
    }
    return true;
}

const char *miniapp_action_validate(const cJSON *actions, const char *name,
                                    const cJSON *arguments) {
    if (!text_valid(name, MINIAPP_ACTION_NAME_MAX, true)) {
        return "invalid action name";
    }
    if (!object_valid(arguments, MINIAPP_ACTION_PARAMS_MAX)) {
        return "arguments must be an object with unique parameter names";
    }
    size_t bytes = 8;
    for (const cJSON *value = arguments->child; value; value = value->next) {
        bytes += 8 + strlen(value->string);
        if (cJSON_IsString(value))
            bytes += strlen(value->valuestring);
        if (bytes > MINIAPP_ACTION_ARGS_BYTES_MAX)
            return "action arguments exceed byte budget";
    }
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(actions, name);
    if (!action) {
        return "unsupported action";
    }
    const cJSON *parameters = cJSON_GetObjectItemCaseSensitive(action, "parameters");
    for (const cJSON *value = arguments->child; value; value = value->next) {
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(parameters, value->string);
        if (!schema) {
            return "unknown action parameter";
        }
        if (!value_valid(value, schema)) {
            return "action parameter has invalid type, range or enum value";
        }
        if (cJSON_IsString(value) && strlen(value->valuestring) > MINIAPP_ACTION_ARGS_BYTES_MAX) {
            return "action parameter is too large";
        }
    }
    for (const cJSON *schema = parameters->child; schema; schema = schema->next) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(schema, "required")) &&
            !cJSON_GetObjectItemCaseSensitive(arguments, schema->string)) {
            return "missing required action parameter";
        }
    }
    return NULL;
}
