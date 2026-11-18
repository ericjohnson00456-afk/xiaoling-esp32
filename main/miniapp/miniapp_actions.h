#ifdef __cplusplus
extern "C" {
#endif
#ifndef MINIAPP_ACTIONS_H
#define MINIAPP_ACTIONS_H

#include <stdbool.h>
#include "cJSON.h"

#define MINIAPP_ACTIONS_MAX 16u
#define MINIAPP_ACTION_PARAMS_MAX 16u
#define MINIAPP_ACTION_NAME_MAX 63u
#define MINIAPP_ACTION_DESCRIPTION_MAX 512u
#define MINIAPP_ACTIONS_BYTES_MAX 16384u
#define MINIAPP_ACTION_ARGS_BYTES_MAX 8192u
#define MINIAPP_ACTION_STATE_MAX 2048u

bool miniapp_actions_valid(const cJSON *actions);
/* NULL means valid; returned error strings are static. */
const char *miniapp_action_validate(const cJSON *actions, const char *name, const cJSON *arguments);
#endif

#ifdef __cplusplus
}
#endif
