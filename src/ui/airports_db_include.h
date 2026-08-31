#pragma once

// Optional generated DB (gitignored). After schema changes, regenerate:
//   python3 tools/generate_airports_db.py
#if __has_include("airports_db.h")
#include "airports_db.h"
#define HAS_AIRPORTS_DB 1
#elif __has_include("ui/airports_db.h")
#include "ui/airports_db.h"
#define HAS_AIRPORTS_DB 1
#elif __has_include("../ui/airports_db.h")
#include "../ui/airports_db.h"
#define HAS_AIRPORTS_DB 1
#else
#define HAS_AIRPORTS_DB 0
#endif

// Stale pre-alias / pre-municipality headers compile-break with a clear regen
// hint instead of opaque "no member named …" errors.
#if HAS_AIRPORTS_DB && !defined(AIRPORTS_DB_HAS_ALIAS)
#error "src/ui/airports_db.h is outdated (missing alias). Run: python3 tools/generate_airports_db.py"
#endif
#if HAS_AIRPORTS_DB && !defined(AIRPORTS_DB_HAS_MUNICIPALITY)
#error "src/ui/airports_db.h is outdated (missing municipality). Run: python3 tools/generate_airports_db.py"
#endif
