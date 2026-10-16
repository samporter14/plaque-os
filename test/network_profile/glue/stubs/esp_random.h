#pragma once
#include <cstddef>
#include <cstdint>
void esp_fill_random(void* buffer, size_t length);
uint32_t esp_random();
