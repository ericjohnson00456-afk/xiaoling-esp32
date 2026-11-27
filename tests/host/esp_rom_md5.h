#pragma once

#include <openssl/evp.h>

#include <cassert>
#include <cstdint>

// Host implementation of the ROM boundary, backed by the system crypto library.
struct md5_context_t { EVP_MD_CTX* value; };

inline void esp_rom_md5_init(md5_context_t* context) {
    context->value = EVP_MD_CTX_new();
    assert(context->value && EVP_DigestInit_ex(context->value, EVP_md5(), nullptr) == 1);
}
inline void esp_rom_md5_update(md5_context_t* context, const void* bytes, uint32_t size) {
    assert(EVP_DigestUpdate(context->value, bytes, size) == 1);
}
inline void esp_rom_md5_final(uint8_t* digest, md5_context_t* context) {
    unsigned size = 0;
    assert(EVP_DigestFinal_ex(context->value, digest, &size) == 1 && size == 16);
    EVP_MD_CTX_free(context->value);
}
