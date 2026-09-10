#include "Transfer.h"

#include <cmath>

#include "Color.h"

namespace abglow {
namespace {

inline float LerpTable(const float* table, int size, float t) {
    const float scaled = t * static_cast<float>(size - 1);
    int i = static_cast<int>(scaled);
    if (i < 0) i = 0;
    if (i > size - 2) i = size - 2;
    const float f = scaled - static_cast<float>(i);
    return table[i] + (table[i + 1] - table[i]) * f;
}

}  // namespace

TransferFunction TransferFunction::BuildSrgb() {
    TransferFunction fn(false);
    for (int i = 0; i < kDecodeSize; ++i) {
        const float encoded = static_cast<float>(i) / static_cast<float>(kDecodeSize - 1);
        fn.decode_[i] = SrgbToLinear(encoded);
    }
    for (int i = 0; i < kEncodeSize; ++i) {
        const float root = static_cast<float>(i) / static_cast<float>(kEncodeSize - 1);
        fn.encode_[i] = LinearToSrgb(root * root);
    }
    return fn;
}

const TransferFunction& TransferFunction::Srgb() {
    static const TransferFunction instance = BuildSrgb();
    return instance;
}

const TransferFunction& TransferFunction::Identity() {
    static const TransferFunction instance(true);
    return instance;
}

float TransferFunction::Decode(float encoded) const {
    if (identity_) return encoded;
    if (encoded <= 0.0f) return encoded < -1.0f ? SrgbToLinear(encoded) : -LerpTable(decode_, kDecodeSize, -encoded);
    if (encoded > 1.0f) return SrgbToLinear(encoded);
    return LerpTable(decode_, kDecodeSize, encoded);
}

float TransferFunction::Encode(float linear) const {
    if (identity_) return linear;
    if (linear <= 0.0f) return linear < -1.0f ? LinearToSrgb(linear) : -LerpTable(encode_, kEncodeSize, std::sqrt(-linear));
    if (linear > 1.0f) return LinearToSrgb(linear);
    return LerpTable(encode_, kEncodeSize, std::sqrt(linear));
}

}  // namespace abglow
