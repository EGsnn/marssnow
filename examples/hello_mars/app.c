/* Apache-2.0. Minimal public Mars application; no cloud or product assets. */
#include <stdint.h>
#include "mars_services.h"

#include "box3_gfx.h"

void app_entry(void)
{
    gfx_clear(0x0863);
    gfx_text(28, 90, "HELLO MARS", 3, 0xffff, 0x0863);
    gfx_present();
    mars_system_delay_ms(1500);
    mars_system_exit_to_desktop();
}
