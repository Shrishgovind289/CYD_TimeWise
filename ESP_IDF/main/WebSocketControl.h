#ifndef WEBSOCKET_CONTROL_H
#define WEBSOCKET_CONTROL_H

#include "esp_err.h"

esp_err_t websocket_control_start(void);
void websocket_control_stop(void);

#endif /* WEBSOCKET_CONTROL_H */