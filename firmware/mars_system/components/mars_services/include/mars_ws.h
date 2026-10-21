#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MARS_WS_MAX_MESSAGE 8192
enum { MARS_WS_CONNECTED = 1, MARS_WS_TEXT, MARS_WS_BINARY,
       MARS_WS_DISCONNECTED, MARS_WS_ERROR };
typedef struct { uint32_t type; uint32_t length; int32_t error; } mars_ws_event_t;
/* Credentials stay in the system, loaded at startup on an internal-RAM stack.
 * Queries/opens use that immutable snapshot; NVS edits take effect after reboot.
 * Returns 0 if configured, -2 if absent. */
int mars_ws_saved_config(uint32_t *protocol_version);
/* Async connect using existing websocket/board NVS. Poll for CONNECTED.
 * Certificate bundle verification and WSS are mandatory. */
int mars_ws_open_saved(void);
int mars_ws_send_text(const char *data, size_t len);
int mars_ws_send_binary(const void *data, size_t len);
int mars_ws_receive(void *buffer, size_t capacity, mars_ws_event_t *event,
                    uint32_t timeout_ms);
/* Synchronous barrier: joins transport before freeing queues. */
int mars_ws_close(void);
void mars_ws_init(void);

#ifdef __cplusplus
}
#endif
