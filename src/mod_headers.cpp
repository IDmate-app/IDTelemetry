#include "mod_headers.h"
#include "mod_pmu.h"
#include "mod_gyro.h"
#include "mod_rtc.h"
#include "mod_can.h"  // can_hw_ok()
#include <Arduino.h>

void headers_apply(AsyncWebServerResponse* resp) {
    if (!resp) return;  // Response kann NULL sein bei RAM-Mangel oder abgebrochenem Client
    char buf[24];

    // FIXES B.16: KEIN X-GPS-Location-Header mehr. Lieferte die exakte
    // Fahrzeugposition ungeschützt auf JEDER Response (/status, /api/*, /gps) an
    // jeden, der den AP erreicht. Position nur noch über die dedizierten
    // (künftig per C.1 authentifizierten) /status- bzw. /gps-Endpoints.

    // Akku — FIXES B.17: gecachten Wert lesen, KEIN pmu_update() (I2C) hier.
    // headers_apply() läuft im async_tcp-Task bei JEDER HTTP-Response; ein
    // I2C-Read pro Response ist teuer und kollidiert mit dem PMU-Update im
    // TELEM-Task. pmu_batt_pct() gibt den zuletzt gecachten Wert zurück.
    snprintf(buf, sizeof(buf), "%d", pmu_batt_pct());
    resp->addHeader("X-Battery", buf);

    // RTC-Zeit
    resp->addHeader("X-RTC-Time", rtc_now_str());

    // CAN-Status (echter TWAI-Hardware-Zustand, nicht nur Treiber-Flag)
    resp->addHeader("X-CAN-Ok", can_hw_ok() ? "1" : "0");

    // Uptime in Sekunden
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    resp->addHeader("X-Uptime", buf);
}
