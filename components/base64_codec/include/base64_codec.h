#ifndef BASE64_CODEC_H
#define BASE64_CODEC_H

#include <string>
#include <string_view>

namespace Base64Codec {

// Encode arbitrary bytes as standard RFC 4648 Base64 text.
std::string Encode(std::string_view input);

// Decode Base64 text into bytes. Returns false for malformed input or an
// allocation/decoder failure; output is unchanged on failure.
bool Decode(std::string_view input, std::string& output);

}  // namespace Base64Codec

#endif  // BASE64_CODEC_H
