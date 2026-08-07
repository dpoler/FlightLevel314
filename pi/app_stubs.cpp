// Temporary placeholders for subsystems that haven't been ported to Linux
// yet. metar is still ESP32-only. airlines and enrichment are ported
// (platform_linux/airlines_linux.cpp, enrichment_linux.cpp).

#include "../src/data/metar.h"

volatile MetarStatus metar_status = METAR_IDLE;
char metar_raw[128] = {0};
char metar_station[8] = {0};
