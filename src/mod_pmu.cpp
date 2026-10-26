#include "mod_pmu.h"
#include "mod_logs.h"
#include "config.h"
#include <Arduino.h>
#include <XPowersLib.h>
#include "esp_sleep.h"

// ============================================================
//  mod_pmu - AXP2101 PMU  (Wire1, SDA=GPIO15, SCL=GPIO7)
//  Verwendet XPowersLib (wie LilyGo AllFunction/power.cpp)
//  Wichtig: DC3 (3000mV) = Modem VDD, BLDO2 (3300mV) = Modem GPS
// ============================================================

static XPowersAXP2101 PMU;
static bool           s_pmu_ok  = false;
static int            s_batt_pct = -1;

void pmu_init() {
    if (!PMU.begin(Wire1, AXP2101_SLAVE_ADDRESS, PMU_SDA_PIN, PMU_SCL_PIN)) {
        syslog("PMU", "FEHLER: AXP2101 nicht gefunden");
        return;
    }
    s_pmu_ok = true;

    // ---- Modem-Stromversorgung aktivieren (wie LilyGo MinimalModemNBIOTExample) ----
    // Beim ersten Boot (kein Deep-Sleep-Wake): DC3 kurz abschalten → sauberer Power-Cycle.
    // Nach Deep Sleep: Modem wurde vor dem Sleep hart per DC3-Off stromlos
    // geschaltet (KEIN AT+CPOF — gibt es im Code nicht, siehe FIXES C.3) → der
    // DC3-Pfad unten reicht; kein zusätzlicher Power-Cycle noetig.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
        PMU.disableDC3();
        delay(200);
        syslog("PMU", "Erster Boot: DC3 Power-Cycle");
    }

    // DC3  = Modem VDD   2700~3400mV → 3000mV
    PMU.setDC3Voltage(3000);
    PMU.enableDC3();
    delay(100);  // Rail stabilisieren vor nächstem
    // BLDO2 = Modem GPS  1400~3700mV → 3300mV
    PMU.setBLDO2Voltage(3300);
    PMU.enableBLDO2();
    // DC1  = VIN1 / QWIIC 3300mV (ESP32S3 Core VDD, default on — explizit setzen)
    PMU.setDC1Voltage(3300);
    PMU.enableDC1();
    delay(100);  // Rail stabilisieren vor nächstem
    // DC5  = Ext. GPS 3300mV
    // Nach Deep Sleep: DC5 NICHT neu setzen! setDC5Voltage() kann einen kurzen
    // Glitch auf der Rail verursachen → M10 sieht Power-Cycle → BBR verloren.
    bool dc5_was_on = PMU.isEnableDC5();
    if (!dc5_was_on) {
        PMU.setDC5Voltage(3300);
        PMU.enableDC5();
    }
    { char m[80]; snprintf(m, sizeof(m), "Power: DC3=3000mV BLDO2=3300mV DC5=3300mV (DC5 %s)",
                           dc5_was_on ? "durchgehend AN" : "neu eingeschaltet"); syslog("PMU", m); }

    // TS-Pin Messung deaktivieren (wie LilyGo-Beispiel — sonst blockiert Laden)
    PMU.disableTSPinMeasure();

    // Nach EXT0-Wake (PMU INT): pending IRQ löschen, damit INT-Pin wieder HIGH geht
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        PMU.clearIrqStatus();
        syslog("PMU", "Wake-IRQ gelöscht (VBUS-Insert)");
    }

    // ---- Lade-Parameter ----
    PMU.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_2000MA);  // PD-Lader liefert genug
    PMU.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_1000MA);    // 2200 mAh Zelle, ~0.5C
    PMU.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);
    PMU.enableCellbatteryCharge();  // Laden aktivieren (war ggf. vor Deep Sleep deaktiviert)

    pmu_update();
}

void pmu_update() {
    if (!s_pmu_ok) return;
    if (!PMU.isBatteryConnect()) { s_batt_pct = -1; return; }

    int    pct = (int)PMU.getBatteryPercent();
    uint16_t mv = PMU.getBattVoltage();  // rohe ADC-Messung, immer zuverlaessig

    // Glitch-Filter: Fuel-Gauge meldet gelegentlich 0% obwohl der Akku voll ist
    // (BAT_PERCENT_DATA Register noch nicht bereit nach Boot/Wake oder I2C-Glitch).
    // Wenn Spannung gesund ist (>3.3V = nicht tiefentladen), aber Prozent sagt 0
    // oder einen implausibel grossen Sprung zum letzten Wert — alten Wert behalten.
    if (pct <= 0 && mv > 3300) {
        // Gauge liefert 0 obwohl Akku OK → verwerfen
        if (s_batt_pct < 0) s_batt_pct = -1;  // initial: unbekannt
        return;
    }
    // Plausibler Sprung: >20% Aenderung in einer Sekunde ist unmoeglich (ausser beim ersten Boot)
    if (s_batt_pct >= 0 && mv > 3300 && abs(pct - s_batt_pct) > 20) {
        return;  // Glitch, ignorieren
    }

    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s_batt_pct = pct;
}

int pmu_batt_pct() { return s_batt_pct; }

bool pmu_ok() { return s_pmu_ok; }   // FIXES B.10: PMU gefunden + initialisiert?

bool pmu_is_vbus_in() {
    if (!s_pmu_ok) return false;
    return PMU.isVbusIn();
}

bool pmu_is_charging() {
    if (!s_pmu_ok) return false;
    return PMU.isCharging();
}

void pmu_set_charging(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.enableCellbatteryCharge();
    } else {
        PMU.disableCellbatteryCharge();
    }
    { char m[32]; snprintf(m, sizeof(m), "Laden %s", on ? "aktiviert" : "deaktiviert"); syslog("PMU", m); }
}

void pmu_enable_vbus_wake() {
    if (!s_pmu_ok) return;
    PMU.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    PMU.clearIrqStatus();
    delay(10);
    // Nur INSERT: beim Sleep-Einstieg ist VBUS weg → REMOVE würde sofort triggern
    PMU.enableIRQ(XPOWERS_AXP2101_VBUS_INSERT_IRQ);
    delay(10);
    PMU.clearIrqStatus();  // Nochmal clearen nach enableIRQ
    syslog("PMU", "VBUS Insert IRQ aktiviert");
}

void pmu_clear_wake_irq() {
    if (!s_pmu_ok) return;
    PMU.clearIrqStatus();
}

void pmu_set_modem_power(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.setDC3Voltage(3000);
        PMU.enableDC3();
    } else {
        PMU.disableDC3();
    }
    { char m[32]; snprintf(m, sizeof(m), "DC3 Modem %s", on ? "an" : "aus"); syslog("PMU", m); }
}

void pmu_set_ext_power(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.setDC5Voltage(3300);
        PMU.enableDC5();
    } else {
        PMU.disableDC5();
    }
    { char m[32]; snprintf(m, sizeof(m), "DC5 ext.GPS %s", on ? "an" : "aus"); syslog("PMU", m); }
}

bool pmu_is_dc5_on() {
    if (!s_pmu_ok) return false;
    return PMU.isEnableDC5();
}

void pmu_set_gps_power(bool on) {
    if (!s_pmu_ok) return;
    if (on) {
        PMU.setBLDO2Voltage(3300);
        PMU.enableBLDO2();
    } else {
        PMU.disableBLDO2();
    }
    { char m[32]; snprintf(m, sizeof(m), "BLDO2 GPS-Ant %s", on ? "an" : "aus"); syslog("PMU", m); }
}
