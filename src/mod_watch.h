#pragma once

// mod_watch — Diagnose bei Watchdog-Neustarts (1.6.11, Schritt 1)
//
// Loest der Task-Watchdog aus, merkt sich der Stick, welcher Task gerade auf
// welchem Kern lief (RTC-Speicher, uebersteht den Neustart). Nach dem Neustart
// steht es im Syslog. Aendert sonst nichts am Verhalten.

void watch_boot_log();   // nach logs_init() aufrufen
