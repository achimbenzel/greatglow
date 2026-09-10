#include "TestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Pixel.h"

namespace abglow_test {
namespace {

using abglow::PixelDepth;
using abglow::PixelF;

std::uint32_t Crc32(const unsigned char* data, std::size_t length, std::uint32_t crc = 0xFFFFFFFFu) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    for (std::size_t i = 0; i < length; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

void PushBigEndian32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value >> 24));
    out.push_back(static_cast<unsigned char>(value >> 16));
    out.push_back(static_cast<unsigned char>(value >> 8));
    out.push_back(static_cast<unsigned char>(value));
}

void PushChunk(std::vector<unsigned char>& out, const char tag[4], const std::vector<unsigned char>& payload) {
    PushBigEndian32(out, static_cast<std::uint32_t>(payload.size()));
    std::vector<unsigned char> crc_input;
    crc_input.insert(crc_input.end(), tag, tag + 4);
    crc_input.insert(crc_input.end(), payload.begin(), payload.end());
    out.insert(out.end(), crc_input.begin(), crc_input.end());
    PushBigEndian32(out, Crc32(crc_input.data(), crc_input.size()) ^ 0xFFFFFFFFu);
}

// Uncompressed zlib stream: keeps the test harness dependency free.
std::vector<unsigned char> ZlibStore(const std::vector<unsigned char>& raw) {
    std::vector<unsigned char> out;
    out.push_back(0x78);
    out.push_back(0x01);
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const std::size_t block = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = (offset + block) >= raw.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<unsigned char>(block & 0xFF));
        out.push_back(static_cast<unsigned char>(block >> 8));
        out.push_back(static_cast<unsigned char>(~block & 0xFF));
        out.push_back(static_cast<unsigned char>((~block >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                   raw.begin() + static_cast<std::ptrdiff_t>(offset + block));
        offset += block;
    }
    std::uint32_t s1 = 1;
    std::uint32_t s2 = 0;
    for (unsigned char byte : raw) {
        s1 = (s1 + byte) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    PushBigEndian32(out, (s2 << 16) | s1);
    return out;
}

unsigned char ToByte(float value) {
    const float v = std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f;
    return static_cast<unsigned char>(v);
}

}  // namespace

void TestImage::SetPixel(int x, int y, const PixelF& value) {
    void* row = view_.Row(y);
    switch (view_.depth) {
        case PixelDepth::kBits8: {
            abglow::Pixel8& p = static_cast<abglow::Pixel8*>(row)[x];
            p.a = ToByte(value.a);
            p.r = ToByte(value.r);
            p.g = ToByte(value.g);
            p.b = ToByte(value.b);
            break;
        }
        case PixelDepth::kBits16: {
            abglow::Pixel16& p = static_cast<abglow::Pixel16*>(row)[x];
            auto to16 = [](float v) {
                return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * abglow::kMaxChannel16 + 0.5f);
            };
            p.a = to16(value.a);
            p.r = to16(value.r);
            p.g = to16(value.g);
            p.b = to16(value.b);
            break;
        }
        case PixelDepth::kFloat32:
            static_cast<PixelF*>(row)[x] = value;
            break;
    }
}

PixelF TestImage::GetPixel(int x, int y) const {
    const void* row = view_.ConstRow(y);
    return abglow::ReadHostPixel(view_, row, x);
}

bool WriteZoomedCrop(const std::string& path, const TestImage& image, int x0, int y0, int width, int height,
                     int zoom) {
    TestImage crop(width * zoom, height * zoom, PixelDepth::kBits8);
    for (int y = 0; y < height * zoom; ++y) {
        for (int x = 0; x < width * zoom; ++x) {
            const int sx = std::clamp(x0 + x / zoom, 0, image.View().width - 1);
            const int sy = std::clamp(y0 + y / zoom, 0, image.View().height - 1);
            crop.SetPixel(x, y, image.GetPixel(sx, sy));
        }
    }
    return WritePng(path, crop);
}

int LongestFlatRun(const TestImage& image, int y) {
    int longest = 0;
    int run = 0;
    int previous = -1;
    for (int x = 0; x < image.View().width; ++x) {
        const int value = static_cast<int>(ToByte(image.GetPixel(x, y).g));
        if (value == previous) {
            ++run;
        } else {
            run = 1;
            previous = value;
        }
        longest = std::max(longest, run);
    }
    return longest;
}

bool WritePng(const std::string& path, const TestImage& image) {
    const abglow::HostImage& view = image.View();
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<std::size_t>(view.height) * (static_cast<std::size_t>(view.width) * 4 + 1));
    for (int y = 0; y < view.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < view.width; ++x) {
            const PixelF p = image.GetPixel(x, y);
            raw.push_back(ToByte(p.r));
            raw.push_back(ToByte(p.g));
            raw.push_back(ToByte(p.b));
            raw.push_back(255);
        }
    }

    std::vector<unsigned char> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    PushBigEndian32(ihdr, static_cast<std::uint32_t>(view.width));
    PushBigEndian32(ihdr, static_cast<std::uint32_t>(view.height));
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", ZlibStore(raw));
    PushChunk(png, "IEND", {});

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const std::size_t written = std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    return written == png.size();
}

}  // namespace abglow_test
