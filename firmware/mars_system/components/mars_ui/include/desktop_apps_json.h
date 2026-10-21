#pragma once
#include "cJSON.h"

/* Normalize legacy home entries in memory; keep the saved document intact. */
void desktop_apps_json_normalize(cJSON *root);
