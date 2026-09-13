#ifndef WIFIMGR_H
#define WIFIMGR_H

#include <WiFi.h>

// Devuelve true si hay conexión WiFi (STA). Si no la hay, levanta el AP
// "RELOJ2023" con portal web en 192.168.4.1 hasta que el usuario configure
// una red válida. Mientras corre el portal, el timer de barrido del display
// sigue activo (ISR) mostrando el parpadeo de modo AP.
bool wifi_get_connection();

#endif
