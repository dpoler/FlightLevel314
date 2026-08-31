#pragma once

// ICAO → compact display-name overrides for detail-card FROM/TO (and similar).
// Applied *instead of* the generic abbreviate + cut-Airport/Aerodrome rule —
// like /etc/hosts over DNS. Keep this list small: only cases where the
// algorithm drops the name people actually use (Schiphol, Boeing Field, …).
//
// Keys are primary OurAirports ICAO (uppercase). Values are ASCII (Montserrat).

struct AirportDisplayOverride {
    const char *icao;
    const char *label;
};

// Sorted by icao for bsearch; keep sorted when editing.
static const AirportDisplayOverride k_airport_display_overrides[] = {
    {"EHAM", "Schiphol"},
    {"EHBD", "Budel"},
    {"EHGG", "Eelde"},
    {"EIKN", "Knock"},
    {"ENBR", "Flesland"},
    {"ENRY", "Rygge"},
    {"ENSB", "Longyear"},
    {"ENTO", "Torp"},
    {"ENZV", "Sola"},
    {"KBFI", "Boeing Field"},
    {"KBTL", "Kellogg Field"},
    {"KCOE", "Pappy Boyington Field"},
    {"KCSV", "Whitson Field"},
    {"KDUA", "Eaker Field"},
    {"KELY", "Yelland Field"},
    {"KFAY", "Grannis Field"},
    {"KFTY", "Brown Field"},
    {"KGGW", "Wokal Field"},
    {"KGYI", "Perrin Field"},
    {"KLBF", "Lee Bird Field"},
    {"KLUK", "Lunken Field"},
    {"KLYH", "Preston Glenn Field"},
    {"KMLS", "Frank Wiley Field"},
    {"KMSS", "Richards Field"},
    {"KOSU", "Don Scott Field"},
    {"KPRC", "Ernest A. Love Field"},
    {"LATI", "Mother Teresa"},
    {"LIRQ", "Peretola"},
};

static const int k_airport_display_override_count =
    (int)(sizeof(k_airport_display_overrides) / sizeof(k_airport_display_overrides[0]));
