#pragma once
#include <cstddef>
#include <cstdint>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_SPIRAM 2
extern "C" { void *heap_caps_aligned_alloc(size_t,size_t,uint32_t); void heap_caps_free(void*); size_t heap_caps_get_allocated_size(void*); }
