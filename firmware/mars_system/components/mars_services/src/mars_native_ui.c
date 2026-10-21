#include "mars_native_ui.h"
#include <stddef.h>
static const mars_native_ui_v1_t *s_ui;
void mars_native_ui_register(const mars_native_ui_v1_t *ui){s_ui=ui;}
const mars_native_ui_v1_t *mars_native_ui_get(uint32_t version)
{return s_ui&&s_ui->version==version?s_ui:NULL;}
