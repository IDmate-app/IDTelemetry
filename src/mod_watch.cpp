#include "mod_watch.h"
#include "mod_logs.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define WATCH_MAGIC 0x57415431UL   // 'WAT1'

struct WatchRec {
    uint32_t magic;
    uint32_t uptime_s;
    char     task[2][16];
};
static RTC_NOINIT_ATTR WatchRec s_rec;

// Von ESP-IDF im Watchdog-Interrupt aufgerufen, kurz vor dem Neustart
// (schwaches Symbol in esp_system). Nur kopieren, nichts loggen.
extern "C" void esp_task_wdt_isr_user_handler(void) {
    for (int c = 0; c < 2; c++) {
        TaskHandle_t h = xTaskGetCurrentTaskHandleForCPU(c);
        const char*  n = h ? pcTaskGetName(h) : nullptr;
        int i = 0;
        if (n) for (; i < 15 && n[i]; i++) s_rec.task[c][i] = n[i];
        s_rec.task[c][i] = '\0';
    }
    s_rec.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_rec.magic    = WATCH_MAGIC;
}

void watch_boot_log() {
    esp_reset_reason_t r = esp_reset_reason();
    if (s_rec.magic == WATCH_MAGIC && (r == ESP_RST_TASK_WDT || r == ESP_RST_PANIC)) {
        s_rec.task[0][15] = s_rec.task[1][15] = '\0';
        char m[112];
        snprintf(m, sizeof(m), "Task-Watchdog nach %lus Laufzeit: Kern 0 lief \"%s\", Kern 1 \"%s\"",
                 (unsigned long)s_rec.uptime_s, s_rec.task[0], s_rec.task[1]);
        syslog("WATCH", m);
    }
    s_rec.magic = 0;
}
