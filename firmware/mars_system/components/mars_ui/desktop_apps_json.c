#include "desktop_apps_json.h"
#include "desktop_dock.h"
#include <stdbool.h>
#include <string.h>

static const char *str(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static void normalize_elements(cJSON *elements, bool *found)
{
    if (!cJSON_IsArray(elements)) return;
    int index = 0;
    cJSON *node;
    while ((node = cJSON_GetArrayItem(elements, index)) != NULL) {
        cJSON *tap = cJSON_GetObjectItemCaseSensitive(node, "onTap");
        const char *action = str(tap, "actionId");
        const bool candidate = strcmp(str(node, "type"), "button") == 0 &&
            strcmp(str(tap, "type"), "invoke") == 0 &&
            (strcmp(action, "app.xiaozhi.open") == 0 ||
             strcmp(action, "app.dyn.0.open") == 0 || strcmp(action, "app.apps.open") == 0);
        if (candidate) {
            if (*found) {
                cJSON_DeleteItemFromArray(elements, index);
                continue;
            }
            {
                cJSON *icon = cJSON_CreateString("apps");
                cJSON *id = cJSON_GetObjectItemCaseSensitive(tap, "actionId");
                if (!icon || !cJSON_SetValuestring(id, "app.apps.open")) {
                    cJSON_Delete(icon);
                    ++index;
                    continue;
                }
                const bool installed = cJSON_HasObjectItem(node, "icon")
                    ? cJSON_ReplaceItemInObjectCaseSensitive(node, "icon", icon)
                    : cJSON_AddItemToObject(node, "icon", icon);
                if (!installed) {
                    cJSON_Delete(icon);
                    ++index;
                    continue;
                }
                cJSON_DeleteItemFromObjectCaseSensitive(node, "text");
            }
            *found = true;
        }
        normalize_elements(cJSON_GetObjectItemCaseSensitive(node, "children"), found);
        ++index;
    }
}

static int number(const cJSON *node,const char *key)
{
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(node,key);
    return cJSON_IsNumber(value)?value->valueint:0;
}
static bool dock_button(const cJSON *node)
{
    if(strcmp(str(node,"type"),"button")||number(node,"y")<180||
       number(node,"width")<8||number(node,"width")>72||number(node,"height")>64)return false;
    const cJSON *tap=cJSON_GetObjectItemCaseSensitive(node,"onTap");
    if(strcmp(str(tap,"type"),"invoke"))return false;
    const char *id=str(tap,"actionId");
    return !strcmp(id,"app.apps.open")||!strcmp(id,"app.clock.open")||
           !strcmp(id,"app.timer.open")||!strcmp(id,"app.fluid_clock.open")||!strcmp(id,"app.system.open");
}
static bool dock_block(const cJSON *node)
{
    return !strcmp(str(node,"id"),"mars-dock")||!strcmp(str(node,"id"),"mars-dock-shadow")||
        (!strcmp(str(node,"type"),"block")&&number(node,"x")==12&&number(node,"y")==198&&
         number(node,"width")==296&&number(node,"height")==36);
}
static void normalize_dock(cJSON *elements)
{
    if(!cJSON_IsArray(elements))return;
    bool apps=false,settings=false,clock=false;
    cJSON *node;
    cJSON_ArrayForEach(node,elements)if(dock_button(node)) {
        const char *id=str(cJSON_GetObjectItemCaseSensitive(node,"onTap"),"actionId");
        apps|=!strcmp(id,"app.apps.open");settings|=!strcmp(id,"app.system.open");
        clock|=!strcmp(id,"app.clock.open")||!strcmp(id,"app.timer.open")||!strcmp(id,"app.fluid_clock.open");
    }
    if(!apps||!settings||!clock)return; /* Leave unrelated/custom layouts alone. */
    cJSON *replacement=cJSON_Parse("[" MARS_NATIVE_DOCK_ELEMENTS "]");
    if(!replacement)return; /* Allocate the full replacement before deleting anything. */
    for(int i=0;(node=cJSON_GetArrayItem(elements,i));) {
        if(dock_button(node)||dock_block(node))cJSON_DeleteItemFromArray(elements,i);
        else ++i;
    }
    while((node=cJSON_DetachItemFromArray(replacement,0)))cJSON_AddItemToArray(elements,node);
    cJSON_Delete(replacement);
}

void desktop_apps_json_normalize(cJSON *root)
{
    bool found = false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version)) return;
    if (version->valueint == 1) {
        cJSON *elements=cJSON_GetObjectItemCaseSensitive(root,"elements");
        normalize_elements(elements, &found);normalize_dock(elements);
    } else if (version->valueint == 2) {
        cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        cJSON *page, *home = NULL, *entry = NULL;
        cJSON_ArrayForEach(page, pages) {
            if (strcmp(str(page, "id"), "home") == 0) home = page;
            if (strcmp(str(page, "id"), str(root, "entryPage")) == 0) entry = page;
        }
        if (!home) home = entry ? entry : cJSON_GetArrayItem(pages, 0);
        cJSON *elements=cJSON_GetObjectItemCaseSensitive(home,"elements");
        normalize_elements(elements, &found);normalize_dock(elements);
    }
}
