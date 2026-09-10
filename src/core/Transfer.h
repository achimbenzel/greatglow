#pragma once

namespace abglow {

// Encode/decode between the project's working space and linear light.
// sRGB is table driven: the glow touches every pixel, so per-channel pow() is
// too expensive, while interpolated tables stay well inside a 16-bit LSB.
class TransferFunction {
public:
    static const TransferFunction& Srgb();
    static const TransferFunction& Identity();

    bool IsIdentity() const { return identity_; }

    float Decode(float encoded) const;  // working space -> linear
    float Encode(float linear) const;   // linear -> working space

private:
    TransferFunction() = default;
    explicit TransferFunction(bool identity) : identity_(identity) {}

    static constexpr int kDecodeSize = 1025;
    static constexpr int kEncodeSize = 4097;

    bool identity_ = false;
    float decode_[kDecodeSize] = {};
    float encode_[kEncodeSize] = {};  // indexed by sqrt(linear)

    static TransferFunction BuildSrgb();
};

}  // namespace abglow
