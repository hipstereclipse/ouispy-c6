/*
 * OUI-Spy C6 — HTTP + WebSocket server
 * SPDX-License-Identifier: MIT
 */
#pragma once

void web_server_start(void);
void web_server_stop(void);
void web_server_broadcast(const char *json);
