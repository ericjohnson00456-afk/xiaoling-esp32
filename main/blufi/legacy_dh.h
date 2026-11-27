#pragma once

// The phone provisioning protocol still sends an arbitrary 1024-bit DH group.
// PSA named FFDH groups cannot represent it. Keep the existing wire format
// using the MPI interface supplied by IDF, as the IDF BluFi example does.
#include <mbedtls/bignum.h>

#include <cstddef>
#include <initializer_list>

namespace blufi_crypto {

struct DhContext {
    mbedtls_mpi prime, generator, peer, secret, public_key, private_key, rr;
    bool private_ready = false;

    DhContext() {
        for (auto* value : {&prime, &generator, &peer, &secret, &public_key,
                            &private_key, &rr}) mbedtls_mpi_init(value);
    }
    ~DhContext() {
        for (auto* value : {&prime, &generator, &peer, &secret, &public_key,
                            &private_key, &rr}) mbedtls_mpi_free(value);
    }
    DhContext(const DhContext&) = delete;
    DhContext& operator=(const DhContext&) = delete;
};

inline size_t GetLen(const DhContext* context) {
    return mbedtls_mpi_size(&context->prime);
}

inline int CheckRange(const mbedtls_mpi* value, const mbedtls_mpi* prime) {
    mbedtls_mpi limit;
    mbedtls_mpi_init(&limit);
    int result = mbedtls_mpi_sub_int(&limit, prime, 1);
    if (result == 0 && (mbedtls_mpi_cmp_int(value, 1) <= 0 ||
                       mbedtls_mpi_cmp_mpi(value, &limit) >= 0)) result = -1;
    mbedtls_mpi_free(&limit);
    return result;
}

inline int ReadParams(DhContext* context, unsigned char** cursor,
                      const unsigned char* end) {
    context->private_ready = false;
    mbedtls_mpi_free(&context->rr);
    mbedtls_mpi_init(&context->rr);
    for (auto* value : {&context->prime, &context->generator, &context->peer}) {
        if (end - *cursor < 2) return -1;
        const size_t size = (size_t((*cursor)[0]) << 8) | (*cursor)[1];
        *cursor += 2;
        if (size == 0 || size > 128 || size > size_t(end - *cursor)) return -1;
        const int result = mbedtls_mpi_read_binary(value, *cursor, size);
        if (result != 0) return result;
        *cursor += size;
    }
    if (*cursor != end || GetLen(context) != 128 ||
        mbedtls_mpi_get_bit(&context->prime, 0) == 0) return -1;
    if (CheckRange(&context->generator, &context->prime) != 0) return -1;
    return CheckRange(&context->peer, &context->prime);
}

inline int MakePublic(DhContext* context, int, unsigned char* output, size_t capacity,
                      int (*random)(void*, unsigned char*, size_t), void* state) {
    if (capacity < GetLen(context) || random == nullptr) return -1;
    mbedtls_mpi limit;
    mbedtls_mpi_init(&limit);
    int result = mbedtls_mpi_sub_int(&limit, &context->prime, 1);
    if (result == 0) {
        result = mbedtls_mpi_random(&context->private_key, 2, &limit, random, state);
    }
    mbedtls_mpi_free(&limit);
    if (result == 0) result = mbedtls_mpi_exp_mod(&context->public_key,
        &context->generator, &context->private_key, &context->prime, &context->rr);
    if (result == 0) result = CheckRange(&context->public_key, &context->prime);
    if (result == 0) result = mbedtls_mpi_write_binary(
        &context->public_key, output, GetLen(context));
    context->private_ready = result == 0;
    return result;
}

inline int CalcSecret(DhContext* context, unsigned char* output, size_t capacity,
                      size_t* size, int (*)(void*, unsigned char*, size_t), void*) {
    if (!context->private_ready || capacity < GetLen(context)) return -1;
    // Each private exponent is used once; the next negotiation generates a new
    // one. This is the first-use path of the former Mbed TLS DHM implementation.
    context->private_ready = false;
    int result = mbedtls_mpi_exp_mod(&context->secret, &context->peer,
        &context->private_key, &context->prime, &context->rr);
    mbedtls_mpi_free(&context->private_key);
    mbedtls_mpi_init(&context->private_key);
    if (result == 0) {
        *size = mbedtls_mpi_size(&context->secret);
        result = mbedtls_mpi_write_binary(&context->secret, output, *size);
    }
    mbedtls_mpi_free(&context->secret);
    mbedtls_mpi_init(&context->secret);
    return result;
}

}  // namespace blufi_crypto
