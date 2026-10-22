// Guest-space heap allocator (HeapCreate/HeapAlloc family and the engine for GlobalAlloc/LocalAlloc/CRT malloc).
// Size-class free lists over 1 MiB chunks committed from the guest VM; 8-byte header, 16-byte aligned blocks.
#pragma once
#include <cstdint>

namespace dish {

uint32_t heap_create(uint32_t flags, uint32_t initial, uint32_t max);   // returns heap handle (0 on failure)
bool heap_destroy(uint32_t heap);
uint32_t heap_process();                                                // GetProcessHeap
uint32_t heap_alloc(uint32_t heap, uint32_t flags, uint32_t size);      // guest pointer or 0
bool heap_free(uint32_t heap, uint32_t flags, uint32_t ptr);
uint32_t heap_realloc(uint32_t heap, uint32_t flags, uint32_t ptr, uint32_t size);
uint32_t heap_size(uint32_t heap, uint32_t flags, uint32_t ptr);        // 0xFFFFFFFF on error
bool heap_validate(uint32_t heap, uint32_t ptr);
uint64_t heap_bytes_in_use();
void heap_reset();                                                      // tests: forget all heaps (guest memory is not freed)

enum : uint32_t { HEAP_NO_SERIALIZE = 1, HEAP_GENERATE_EXCEPTIONS = 4, HEAP_ZERO_MEMORY = 8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10 };

}  // namespace dish
