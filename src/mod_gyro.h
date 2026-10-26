#pragma once
#include "shared.h"

// ============================================================
//  mod_gyro - MPU-6050 Erschütterungserkennung
//
//  Verdrahtung (über P2-Reihe des DS1307 Moduls):
//   VCC → 3.3V (P2)
//   GND → GND  (P2)
//   SDA → GPIO45 (gleicher I2C Bus wie RTC)
//   SCL → GPIO21 (gleicher I2C Bus wie RTC)
//   AD0 → 3.3V  → I2C Adresse 0x69 (NICHT GND - sonst Konflikt mit DS1307 0x68!)
//   INT → GPIO3 (Deep-Sleep Wake-up)
//
//  Ampel-Status:
//   GYRO_STILL    → grün  (keine Bewegung)
//   GYRO_SHAKE    → gelb  (leichte Erschütterung)
//   (rot = kein Sensor / Fehler)
// ============================================================

#define MPU6050_ADDR        0x69   // AD0 an 3.3V
// GYRO_SHAKE_THRESHOLD kommt aus config.h (via shared.h)
#define GYRO_TASK_MS        100    // Abtastrate

enum GyroState {
    GYRO_ERROR = 0,   // rot  - kein Sensor
    GYRO_STILL = 1,   // gruen - ruhig
    GYRO_SHAKE = 2,   // gelb - Erschuetterung
};

void       gyro_init();
GyroState  gyro_get_state();
float      gyro_get_accel_g();      // letzter Gesamtbeschleunigungswert in G
void       gyro_get_accel_xyz(float& ax, float& ay, float& az);  // Rohwerte in G (Tilt-Kompensation)
float      gyro_get_yaw_dps();     // Drehrate Z-Achse (Yaw) in °/s
bool       gyro_ok();               // true wenn MPU-6050 gefunden
void       gyro_set_threshold(float g);  // Schwelle zur Laufzeit ändern (0.005–1.0)
float      gyro_get_threshold();         // aktuelle Schwelle in G
void       gyro_set_mot_threshold(uint8_t v); // Hardware MOT_THR für Deep-Sleep-Wake (1-255, 1=32mg)
uint8_t    gyro_get_mot_threshold();          // aktueller MOT_THR Registerwert
float      gyro_get_wake_accel();            // Beschleunigung beim Boot (vor Kalibrierung)
uint32_t   gyro_last_shake_ms();         // millis() des letzten Erschütterungsmoments (0 = nie)
void       gyro_configure_sleep_int();   // MPU-6050 INT-Latch für Deep-Sleep-Wake konfigurieren

// Neukalibrierung der Ruhelage (Board muss still stehen, ~3s Messung)
// out_baseline / out_stddev: optionale Ausgabeparameter für Web-Response
// Gibt true zurück wenn Board ruhig genug war → g_baseline aktualisiert + SPIFFS gespeichert
bool       gyro_recalibrate(float* out_baseline = nullptr, float* out_stddev = nullptr);