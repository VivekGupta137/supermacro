#pragma once

#include "sdkconfig.h"

void macro_web_start(void);

#if CONFIG_MACRO_WEB_UI
/** Close all HTTP/WS clients; keep listener (debounced). */
void macro_web_close_active_clients(void);
/** Close every client except keep_fd (immediate; used for WebSocket handshake). */
void macro_web_close_other_clients(int keep_fd);
/** Legacy name: closes clients only (never stops the listener). */
void macro_web_request_listener_restart(void);
#endif
