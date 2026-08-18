#pragma once
#include <cstdint>
#include <cstddef>
namespace wasm {
enum class DecryptResult { Success, NotEncrypted, BadMagic, NoKey, Failed };
DecryptResult decrypt_mpxe(const uint8_t*, std::size_t, uint8_t**, std::size_t*);
}
