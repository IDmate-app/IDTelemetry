#pragma once
#include <ESPAsyncWebServer.h>

// ============================================================
//  mod_headers - Zentrale HTTP-Response-Header
//
//  Alle Status-Infos (GPS, Akku, RTC, CAN) in einem Aufruf.
//  Aufruf vor jedem r->send(resp) auf HTML-Seiten und /status.
//
//  Gesetzte Header:
//    X-GPS-Location  "51.123456 11.123456"  oder  "no-fix"
//    X-Battery       Akku-% (-1 = kein Akku)
//    X-RTC-Time      "HH:MM:SS"
//    X-CAN-Ok        "1" / "0"
//    X-Uptime        Sekunden seit Boot
// ============================================================

void headers_apply(AsyncWebServerResponse* resp);
