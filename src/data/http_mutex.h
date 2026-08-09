#pragma once
#include <cstdint>

// Global HTTP request serialization -- one shared mutex so concurrent
// fetch/enrichment calls don't hit the network stack at once. Uses
// src/platform/platform.h's mutex primitive. UINT32_MAX means wait
// indefinitely.

void http_mutex_init();
bool http_mutex_acquire(uint32_t timeout_ms = UINT32_MAX);
void http_mutex_release();
