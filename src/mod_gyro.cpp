#include "mod_gyro.h"
#include "mod_sleep.h"
#include "mod_logs.h"
#include <Wire.h>
#include <SPIFFS.h>

// ============================================================
//  mod_gyro - MPU-6050 Erschütterungserkennung
// ============================================================

#define MPU_REG_PWR_MGMT_1  0x6B
#define MPU_REG_ACCEL_XOUT  0x3B
#define MPU_REG_GYRO_XOUT   0x43   // Gyro X/Y/Z (Drehrate °/s)
#define MPU_REG_WHO_AM_I    0x75

// ── Kalibrierung ─────────────────────────────────────────────
// gyro_init(): direkt via I2C (Task läuft noch nicht), 150 × 20ms = 3s
// gyro_recalibrate(): liest g_raw_total vom laufenden Task, 30 × ~100ms = 3s
#define GYRO_CAL_SAMPLES     30      // Samples für Init-Kalibrierung (direkt I2C)
#define GYRO_CAL_SAMPLE_MS   10      // ms zwischen Samples bei direkter Messung
#define GYRO_CAL_MAX_STDDEV  0.015f  // max erlaubte Streuung für "ruhig genug"
#define GYRO_BASELINE_FILE   "/gyro_baseline.txt"

static GyroState  g_state         = GYRO_ERROR;
static float      g_accel         = 0.0f;
static float      g_ax = 0.0f, g_ay = 0.0f, g_az = 1.0f;  // Accel in G (Tilt-Kompensation)
static bool       g_ok            = false;
static uint32_t   g_shake_last_ms = 0;
static int        g_shake_cnt     = 0;
static uint32_t   g_last_shake_ms = 0;
static bool       g_shake_active  = false;
static float      g_threshold     = GYRO_SHAKE_THRESHOLD;
static uint8_t    g_mot_thr       = 4;   // MPU-6050 MOT_THR (1 Einheit = 32mg)
static float      g_baseline      = 1.0f;    // kalibrierte Ruhelage in G
static float      g_wake_accel    = 0.0f;    // Beschleunigung beim Boot (vor Kalibrierung)

// Von gyro_task beschrieben, von gyro_recalibrate() gelesen (cross-task)
static volatile float    g_raw_total = 1.0f; // Roh-Vektorbetrag ohne baseline-Abzug
static volatile float    g_raw_gx = 0, g_raw_gy = 0, g_raw_gz = 0; // Rohe Drehrate (°/s, ohne Bias)
static volatile uint32_t g_task_tick = 0;    // inkrementiert je Task-Zyklus

// Drehrate (Gyroskop) — Z = Yaw (Kurven), X = Pitch-Rate, Y = Roll-Rate
static float g_yaw_dps   = 0.0f;   // °/s gefiltert
static float g_pitch_dps = 0.0f;
static float g_roll_dps  = 0.0f;

// Gyro-Offset (Bias in °/s, bei Kalibrierung gemessen)
static float g_gyro_bias_x = 0.0f;
static float g_gyro_bias_y = 0.0f;
static float g_gyro_bias_z = 0.0f;
#define GYRO_BIAS_FILE  "/gyro_bias.txt"

// ── I2C Hilfsfunktionen ──────────────────────────────────────
static bool mpu_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool mpu_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)MPU6050_ADDR, len);
    if (Wire.available() < len) return false;
    for (int i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

// ── Statistik-Auswertung (gemeinsam für Init + Recalibrate) ─
static void calc_stats(const float* s, int n, float& mean, float& stddev) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += s[i];
    mean = sum / n;
    float var = 0;
    for (int i = 0; i < n; i++) { float d = s[i] - mean; var += d * d; }
    stddev = sqrtf(var / n);
}

// ── Init-Kalibrierung via direktem I2C (kein Task aktiv!) ────
// Kalibriert Accel-Baseline UND Gyro-Bias gleichzeitig.
static bool calibrate_i2c(float& out_mean, float& out_stddev) {
    float samples[GYRO_CAL_SAMPLES];
    float sum_gx = 0, sum_gy = 0, sum_gz = 0;
    int   gyro_cnt = 0;
    for (int i = 0; i < GYRO_CAL_SAMPLES; i++) {
        uint8_t buf[6];
        float total = 1.0f;
        if (mpu_read(MPU_REG_ACCEL_XOUT, buf, 6)) {
            int16_t ax = (int16_t)((buf[0] << 8) | buf[1]);
            int16_t ay = (int16_t)((buf[2] << 8) | buf[3]);
            int16_t az = (int16_t)((buf[4] << 8) | buf[5]);
            float fx = ax / 16384.0f, fy = ay / 16384.0f, fz = az / 16384.0f;
            total = sqrtf(fx*fx + fy*fy + fz*fz);
        }
        samples[i] = total;
        // Gyro-Drehrate mitlesen
        uint8_t gbuf[6];
        if (mpu_read(MPU_REG_GYRO_XOUT, gbuf, 6)) {
            int16_t gx = (int16_t)((gbuf[0] << 8) | gbuf[1]);
            int16_t gy = (int16_t)((gbuf[2] << 8) | gbuf[3]);
            int16_t gz = (int16_t)((gbuf[4] << 8) | gbuf[5]);
            sum_gx += gx / 131.0f;
            sum_gy += gy / 131.0f;
            sum_gz += gz / 131.0f;
            gyro_cnt++;
        }
        delay(GYRO_CAL_SAMPLE_MS);
    }
    calc_stats(samples, GYRO_CAL_SAMPLES, out_mean, out_stddev);

    // Gyro-Bias speichern wenn ruhig genug
    if (out_stddev <= GYRO_CAL_MAX_STDDEV && gyro_cnt > 0) {
        g_gyro_bias_x = sum_gx / gyro_cnt;
        g_gyro_bias_y = sum_gy / gyro_cnt;
        g_gyro_bias_z = sum_gz / gyro_cnt;
        File f = SPIFFS.open(GYRO_BIAS_FILE, "w");
        if (f) { f.printf("%.4f\n%.4f\n%.4f\n", g_gyro_bias_x, g_gyro_bias_y, g_gyro_bias_z); f.close(); }
        { char m[64]; snprintf(m, sizeof(m), "Bias: X=%.2f Y=%.2f Z=%.2f °/s", g_gyro_bias_x, g_gyro_bias_y, g_gyro_bias_z); syslog("GYRO", m); }
    }

    return out_stddev <= GYRO_CAL_MAX_STDDEV;
}

// ── Gyro Task ────────────────────────────────────────────────
static void gyro_task(void*) {
    float fx=0, fy=0, fz=1.0f;
    const float GYRO_ALPHA = 0.3f; // Tiefpass für Drehrate
    while (!g_shutdown) {
        uint8_t buf[6];
        bool accel_ok = mpu_read(MPU_REG_ACCEL_XOUT, buf, 6);
        if (accel_ok) {
            int16_t ax = (int16_t)((buf[0] << 8) | buf[1]);
            int16_t ay = (int16_t)((buf[2] << 8) | buf[3]);
            int16_t az = (int16_t)((buf[4] << 8) | buf[5]);

            // Skalierung: ±2G Bereich = 16384 LSB/G
            fx = ax / 16384.0f;
            fy = ay / 16384.0f;
            fz = az / 16384.0f;
            g_ax = fx; g_ay = fy; g_az = fz;

            float total = sqrtf(fx*fx + fy*fy + fz*fz);
            g_raw_total = total;  // für gyro_recalibrate() bereitstellen
            g_task_tick++;        // Signal: neues Sample vorhanden

            // Abweichung von kalibrierter Ruhelage statt hardcodiertem 1.0f
            float delta = fabsf(total - g_baseline);
            g_accel = delta;

            uint32_t now = millis();
            if (delta >= g_threshold) {
                g_shake_cnt++;
                if (g_shake_cnt >= 3) {   // 3 × 100ms = echte Bewegung, kein Rauschen
                    g_shake_last_ms = now;
                    g_state = GYRO_SHAKE;
                }
            } else {
                g_shake_cnt = 0;          // Rauschen-Spike: Counter zurücksetzen
                if ((now - g_shake_last_ms) < GYRO_HOLD_MS) {
                    g_state = GYRO_SHAKE; // Hold noch aktiv
                } else {
                    g_state = GYRO_STILL;
                }
            }
        } else {
            g_state = GYRO_ERROR;
        }

        // Gyroskop (Drehrate) auslesen — ±250°/s = 131 LSB/(°/s)
        uint8_t gbuf[6];
        if (mpu_read(MPU_REG_GYRO_XOUT, gbuf, 6)) {
            int16_t gx = (int16_t)((gbuf[0] << 8) | gbuf[1]);
            int16_t gy = (int16_t)((gbuf[2] << 8) | gbuf[3]);
            int16_t gz = (int16_t)((gbuf[4] << 8) | gbuf[5]);
            float raw_rx = gx / 131.0f;
            float raw_ry = gy / 131.0f;
            float raw_rz = gz / 131.0f;
            g_raw_gx = raw_rx; g_raw_gy = raw_ry; g_raw_gz = raw_rz;
            float rx = raw_rx - g_gyro_bias_x;  // °/s minus Offset
            float ry = raw_ry - g_gyro_bias_y;
            float rz = raw_rz - g_gyro_bias_z;
            // Tiefpass
            g_pitch_dps += (rx - g_pitch_dps) * GYRO_ALPHA;
            g_roll_dps  += (ry - g_roll_dps)  * GYRO_ALPHA;
            g_yaw_dps   += (rz - g_yaw_dps)   * GYRO_ALPHA;
        }

        uint32_t now = millis();

        if (g_state == GYRO_SHAKE) {
            g_last_shake_ms = now;
            if (!g_shake_active) {
                g_shake_active = true;
                if (ws.count() > 0) ws.textAll("{\"type\":\"gyro\",\"state\":2}");
            }
        } else if (g_shake_active && (now - g_last_shake_ms > 1000)) {
            g_shake_active = false;
            if (ws.count() > 0) ws.textAll("{\"type\":\"gyro\",\"state\":1}");
        }

        // G-Wert + Achsen + Drehrate broadcasten (100ms)
        if (ws.count() > 0) {
            char gjson[160];
            snprintf(gjson, sizeof(gjson),
                     "{\"type\":\"gyro_g\",\"g\":%.4f,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                     "\"gx\":%.1f,\"gy\":%.1f,\"gz\":%.1f}",
                     g_accel, fx, fy, fz, g_pitch_dps, g_roll_dps, g_yaw_dps);
            ws.textAll(gjson);
        }

        vTaskDelay(pdMS_TO_TICKS(GYRO_TASK_MS));
    }
    syslog("GYRO", "Task beendet (Shutdown)");
    vTaskDelete(NULL);
}

// ── Init ─────────────────────────────────────────────────────
void gyro_init() {
    // I2C Reset: Wire neu initialisieren mit kurzem Timeout.
    // MPU im Cycle-Mode nach Deep Sleep kann den Bus hängen lassen.
    Wire.end();
    delay(5);
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    Wire.setTimeOut(50);  // 50ms (Default) — genug für RTC + MPU auf gleichem Bus
    delay(10);

    // WHO_AM_I prüfen — Retry weil MPU nach Deep Sleep im Cycle-Mode kurz schläft
    uint8_t who = 0;
    bool found = false;
    for (int i = 0; i < 5 && !found; i++) {
        if (mpu_read(MPU_REG_WHO_AM_I, &who, 1)) found = true;
        else delay(30);
    }
    if (!found) {
        syslog("GYRO", "MPU-6050 nicht gefunden — überspringe Init");
        g_ok = false;
        g_state = GYRO_ERROR;
        return;
    }

    // MPU aufwecken (aus Cycle-Mode: PWR_MGMT_1=0x20 → 0x00)
    for (int retry = 0; retry < 5; retry++) {
        if (mpu_write(MPU_REG_PWR_MGMT_1, 0x00)) break;
        delay(30);
    }
    // Alle Achsen aktivieren (Accel + Gyro) — nach Deep Sleep steht hier 0xC7
    mpu_write(0x6C, 0x00);
    delay(50);  // MPU vollständig hochfahren lassen

    // Beschleunigung beim Boot auslesen (vor Kalibrierung)
    {
        uint8_t abuf[6];
        if (mpu_read(MPU_REG_ACCEL_XOUT, abuf, 6)) {
            int16_t ax = (int16_t)((abuf[0] << 8) | abuf[1]);
            int16_t ay = (int16_t)((abuf[2] << 8) | abuf[3]);
            int16_t az = (int16_t)((abuf[4] << 8) | abuf[5]);
            float fx = ax / 16384.0f, fy = ay / 16384.0f, fz = az / 16384.0f;
            g_wake_accel = sqrtf(fx*fx + fy*fy + fz*fz);
            // Boot-Accel wird in sleep_log_wake() per syslog ausgegeben
        }
    }

    // Motion-Detection-Interrupt für GPIO3-Wake konfigurieren
    // MOT_THR aus SPIFFS laden (persistente Aufwachschwelle)
    {
        File fm = SPIFFS.open("/gyro_mot.cfg", "r");
        if (fm) {
            int saved = fm.readStringUntil('\n').toInt();
            fm.close();
            if (saved >= 1 && saved <= 255) g_mot_thr = (uint8_t)saved;
        }
    }
    mpu_write(0x1F, g_mot_thr);
    // MOT_DUR (0x20): Mindestdauer 40ms → kein Rauschen
    mpu_write(0x20, 40);
    // INT_PIN_CFG (0x37): active-high, push-pull, INT_RD_CLEAR=1 (Normal-Betrieb)
    mpu_write(0x37, 0x10);
    // INT_ENABLE (0x38): Motion-Detection-Interrupt aktivieren (Bit 6)
    mpu_write(0x38, 0x40);

    // ── Baseline + Bias immer aus SPIFFS laden (manuelle Kalibrierung via "gyro cal") ──
    {
        File f = SPIFFS.open(GYRO_BASELINE_FILE, "r");
        if (f) {
            float saved = f.readStringUntil('\n').toFloat();
            f.close();
            if (saved >= 0.5f && saved <= 1.5f) {
                g_baseline = saved;
            } else {
                g_baseline = 1.0f;
            }
        } else {
            g_baseline = 1.0f;
            syslog("GYRO", "Keine Kalibrierung gespeichert — 'gyro cal' ausfuehren!");
        }
        File fb = SPIFFS.open(GYRO_BIAS_FILE, "r");
        if (fb) {
            g_gyro_bias_x = fb.readStringUntil('\n').toFloat();
            g_gyro_bias_y = fb.readStringUntil('\n').toFloat();
            g_gyro_bias_z = fb.readStringUntil('\n').toFloat();
            fb.close();
        }
    }

    // ─────────────────────────────────────────────────────────────────

    g_ok    = true;
    g_state = GYRO_STILL;

    // Gespeicherte Schwelle aus SPIFFS laden
    File f = SPIFFS.open("/gyro.cfg", "r");
    if (f) {
        String s = f.readStringUntil('\n');
        f.close();
        float saved = s.toFloat();
        if (saved >= 0.005f && saved <= 1.0f) {
            g_threshold = saved;
        }
    }

    { char m[80]; snprintf(m, sizeof(m), "MPU-6050 OK · baseline=%.4f thr=%.4fG MOT=%d (%dmg)",
                           g_baseline, g_threshold, g_mot_thr, g_mot_thr * 32);
      syslog("GYRO", m); }

    xTaskCreatePinnedToCore(gyro_task, "GYRO", 4096, NULL, 1, NULL, 0);
}

// ── Getter / Setter ──────────────────────────────────────────
GyroState gyro_get_state()     { return g_state; }
float     gyro_get_accel_g()   { return g_accel; }
void      gyro_get_accel_xyz(float& ax, float& ay, float& az) { ax = g_ax; ay = g_ay; az = g_az; }
float     gyro_get_yaw_dps()  { return g_yaw_dps; }
bool      gyro_ok()            { return g_ok; }
uint32_t  gyro_last_shake_ms() { return g_last_shake_ms; }

// Vor Deep Sleep: INT-Latch aktivieren + MPU in Cycle-Mode (Stromspar)
void gyro_configure_sleep_int() {
    // 1. MPU aufwecken (falls im Cycle-Mode) — sauberer Ausgangszustand
    mpu_write(0x6B, 0x00);          // PWR_MGMT_1: SLEEP=0, CYCLE=0, interner Takt
    delay(10);                       // Stabilisierung

    // 2. Ausstehenden Interrupt löschen (INT_STATUS lesen → INT-Pin geht LOW)
    uint8_t tmp;
    mpu_read(0x3A, &tmp, 1);

    // 3. ACCEL_CONFIG (0x1C): ±2G (AFS_SEL=00), DHPF=On 5Hz (Bits[2:0]=001)
    //    DHPF filtert Schwerkraft heraus — Motion-Detection erkennt nur echte Bewegung!
    //    OHNE DHPF ist die Referenz undefiniert und der INT feuert sofort oder nie.
    mpu_write(0x1C, 0x01);

    // 4. Motion-Threshold setzen (für Wake-up Empfindlichkeit)
    mpu_write(0x1F, g_mot_thr);     // MOT_THR: 1 Einheit = 32mg
    mpu_write(0x20, 1);             // MOT_DUR: 1ms (minimale Dauer, Threshold reicht)

    // 5. MOT_DETECT_CTRL (0x69): Accel-On-Delay=1ms, Zähler-Dekrement für Motion
    //    Nötig damit Accel im Cycle-Mode vor Vergleich stabil ist
    mpu_write(0x69, 0x15);

    // 6. INT_PIN_CFG (0x37): active-high, push-pull, LATCH_INT_EN=1
    //    INT-Pin bleibt HIGH bis INT_STATUS gelesen → sicheres Wake-up
    mpu_write(0x37, 0x20);

    // 7. INT_ENABLE (0x38): Motion-Detection-Interrupt aktivieren (Bit 6)
    //    OHNE diesen Schritt feuert der INT-Pin nie!
    mpu_write(0x38, 0x40);

    // 8. DHPF Referenz einschwingen lassen (wichtig: aktuellen Ruhezustand einlernen)
    delay(50);

    // 9. Nochmal INT_STATUS lesen → sauberer Zustand, INT-Pin LOW
    mpu_read(0x3A, &tmp, 1);

    // 10. PWR_MGMT_2 (0x6C): LP_WAKE_CTRL=11 → 40Hz Abtastrate im Cycle-Mode
    //     STBY_XG/YG/ZG=1 → Gyro-Achsen in Standby (Accel bleibt aktiv)
    mpu_write(0x6C, 0xC7);

    // 11. PWR_MGMT_1 (0x6B): CYCLE=1 → periodische Accel-Messung im Stromspar
    mpu_write(0x6B, 0x20);

    // 12. Kurz warten, nochmal INT_STATUS prüfen
    delay(20);
    mpu_read(0x3A, &tmp, 1);
    { char m[64]; snprintf(m, sizeof(m), "Sleep-Mode: MOT_THR=%d (%dmg) Cycle aktiv", g_mot_thr, g_mot_thr * 32); syslog("GYRO", m); }
}

// ── Neukalibrierung (Task läuft) ─────────────────────────────
// Liest g_raw_total vom laufenden gyro_task — kein direkter I2C-Zugriff, kein Konflikt.
// Wartet je Sample auf neuen Task-Tick (~100ms) → 30 Samples ≈ 3s.
bool gyro_recalibrate(float* out_baseline, float* out_stddev) {
    if (!g_ok) return false;

    const int N = 30;
    float samples[N];
    float sum_gx = 0, sum_gy = 0, sum_gz = 0;
    for (int i = 0; i < N; i++) {
        uint32_t last_tick = g_task_tick;
        uint32_t t0 = millis();
        while (g_task_tick == last_tick && (millis() - t0) < 200) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        samples[i] = g_raw_total;
        sum_gx += g_raw_gx;
        sum_gy += g_raw_gy;
        sum_gz += g_raw_gz;
    }

    float mean = 0.0f, sd = 0.0f;
    calc_stats(samples, N, mean, sd);

    if (out_baseline) *out_baseline = mean;
    if (out_stddev)   *out_stddev   = sd;

    if (sd <= GYRO_CAL_MAX_STDDEV) {
        g_baseline = mean;
        File f = SPIFFS.open(GYRO_BASELINE_FILE, "w");
        if (f) { f.printf("%.6f\n", mean); f.close(); }

        // Gyro-Bias aktualisieren
        g_gyro_bias_x = sum_gx / N;
        g_gyro_bias_y = sum_gy / N;
        g_gyro_bias_z = sum_gz / N;
        File fb = SPIFFS.open(GYRO_BIAS_FILE, "w");
        if (fb) { fb.printf("%.4f\n%.4f\n%.4f\n", g_gyro_bias_x, g_gyro_bias_y, g_gyro_bias_z); fb.close(); }

        { char m[80]; snprintf(m, sizeof(m), "Rekal OK: baseline=%.4f stddev=%.4f bias=%.2f/%.2f/%.2f", mean, sd, g_gyro_bias_x, g_gyro_bias_y, g_gyro_bias_z); syslog("GYRO", m); }
        return true;
    }
    { char m[48]; snprintf(m, sizeof(m), "Rekal fehlgeschlagen (stddev=%.4f)", sd); syslog("GYRO", m); }
    return false;
}

void gyro_set_threshold(float g) {
    if (g < 0.005f || g > 1.0f) return;
    g_threshold = g;
    File f = SPIFFS.open("/gyro.cfg", "w");
    if (f) { f.printf("%.4f\n", g); f.close(); }
}
float gyro_get_threshold() { return g_threshold; }

void gyro_set_mot_threshold(uint8_t v) {
    if (v < 1) v = 1;
    g_mot_thr = v;
    mpu_write(0x1F, v);   // Sofort ins Hardware-Register schreiben
    File f = SPIFFS.open("/gyro_mot.cfg", "w");
    if (f) { f.printf("%u\n", v); f.close(); }
}
uint8_t gyro_get_mot_threshold() { return g_mot_thr; }
float   gyro_get_wake_accel()    { return g_wake_accel; }
