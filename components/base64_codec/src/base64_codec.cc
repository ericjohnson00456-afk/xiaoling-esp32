#include "base64_codec.h"

#include <mbedtls/base64.h>

#include <vector>

namespace Base64Codec {

std::string Encode(std::string_view input) {
    if (input.empty()) {
        return {};
    }

    const size_t capacity = 4 * ((input.size() + 2) / 3) + 1;
    std::string output(capacity, '\0');
    size_t output_size = 0;
    const int ret = mbedtls_base64_encode(
        reinterpret_cast<unsigned char*>(output.data()), output.size(), &output_size,
        reinterpret_cast<const unsigned char*>(input.data()), input.size());
    if (ret != 0) {
        return {};
    }
    output.resize(output_size);
    return output;
}

bool Decode(std::string_view input, std::string& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }

    const auto* encoded = reinterpret_cast<const unsigned char*>(input.data());
    size_t required_size = 0;
    const int size_ret = mbedtls_base64_decode(
        nullptr, 0, &required_size, encoded, input.size());
    if (size_ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || required_size == 0) {
        return false;
    }

    std::vector<unsigned char> decoded(required_size);
    size_t decoded_size = 0;
    const int ret = mbedtls_base64_decode(
        decoded.data(), decoded.size(), &decoded_size, encoded, input.size());
    if (ret != 0) {
        return false;
    }

    output.assign(reinterpret_cast<const char*>(decoded.data()), decoded_size);
    return true;
}

}  // namespace Base64Codec
