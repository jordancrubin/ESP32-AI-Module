#ifndef FRONTEND_MEM_H_
#define FRONTEND_MEM_H_

#include <stdlib.h>

#if defined(ESP32) || defined(ESP_PLATFORM) || defined(USE_ESP32) || defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
static inline void *frontend_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(size);
    }
    return ptr;
}
static inline void *frontend_calloc(size_t count, size_t size) {
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = calloc(count, size);
    }
    return ptr;
}
static inline void frontend_free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}
#else
static inline void *frontend_malloc(size_t size) {
    return malloc(size);
}
static inline void *frontend_calloc(size_t count, size_t size) {
    return calloc(count, size);
}
static inline void frontend_free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}
#endif

#endif  // FRONTEND_MEM_H_
