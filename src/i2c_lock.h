#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
//  Sperre fuer den gemeinsamen I2C-Bus "Wire" (DS1307-RTC + MPU-6050)
//
//  Wire sperrt nur die einzelne Uebertragung. Der Puffer, aus dem
//  Wire.read() danach liest, ist aber fuer alle gleich: liest der
//  Gyro-Task (alle 100 ms) und gleichzeitig der Webserver die RTC,
//  kann der eine die Bytes des anderen bekommen. Die Sperre haelt
//  Schreiben, Anfordern und Auslesen zusammen. (1.7.3, FIXES C.5)
//  Die PMU haengt an Wire1 und ist nicht betroffen.
// ============================================================

class I2cGuard {
public:
    explicit I2cGuard(uint32_t ms = 100) {
        static SemaphoreHandle_t mx = xSemaphoreCreateRecursiveMutex();
        m_mx = mx;
        m_ok = m_mx && xSemaphoreTakeRecursive(m_mx, pdMS_TO_TICKS(ms)) == pdTRUE;
    }
    ~I2cGuard() { if (m_ok) xSemaphoreGiveRecursive(m_mx); }
    bool ok() const { return m_ok; }
private:
    SemaphoreHandle_t m_mx;
    bool m_ok;
};
