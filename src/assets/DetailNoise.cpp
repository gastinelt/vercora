#include "assets/DetailNoise.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace vercora {
namespace {

float latticeValue(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h & 0x00ffffffu) / 16777215.0f;
}

unsigned char toByte(float v) {
    return static_cast<unsigned char>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
}

} // namespace

TextureData makeDetailNoiseTexture(uint32_t size, uint32_t cells, uint32_t seed) {
    if (size == 0 || cells == 0 || size % cells != 0 || (size & (size - 1)) != 0) {
        throw std::invalid_argument(
            "makeDetailNoiseTexture : size doit etre une puissance de deux multiple de cells.");
    }

    // --- Niveau 0 : bruit de valeur periodique et ses derivees --------------
    const uint32_t texelsPerCell = size / cells;
    std::vector<float> level(static_cast<size_t>(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            // Centre du texel, en unites de cellule.
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(texelsPerCell);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(texelsPerCell);
            const uint32_t i = static_cast<uint32_t>(std::floor(u));
            const uint32_t j = static_cast<uint32_t>(std::floor(v));
            const float fx = u - static_cast<float>(i);
            const float fy = v - static_cast<float>(j);

            // Raccord periodique : la cellule "cells" est la cellule 0.
            const uint32_t i0 = i % cells, i1 = (i + 1) % cells;
            const uint32_t j0 = j % cells, j1 = (j + 1) % cells;
            const float a = latticeValue(i0, j0, seed);
            const float b = latticeValue(i1, j0, seed);
            const float c = latticeValue(i0, j1, seed);
            const float d = latticeValue(i1, j1, seed);

            const float sx = fx * fx * (3.0f - 2.0f * fx);
            const float sy = fy * fy * (3.0f - 2.0f * fy);
            const float dsx = 6.0f * fx * (1.0f - fx);
            const float dsy = 6.0f * fy * (1.0f - fy);
            const float k = a - b - c + d;

            const float value = a + (b - a) * sx + (c - a) * sy + k * sx * sy;
            const float du = dsx * ((b - a) + k * sy);
            const float dv = dsy * ((c - a) + k * sx);

            float* px = &level[(static_cast<size_t>(y) * size + x) * 4];
            px[0] = value;
            px[1] = 0.5f + 0.5f * du / kDetailNoiseSlopeRange;
            px[2] = 0.5f + 0.5f * dv / kDetailNoiseSlopeRange;
            px[3] = 1.0f;
        }
    }

    // --- Chaine de mipmaps par moyenne 2x2 ------------------------------------
    TextureData data;
    data.width = size;
    data.height = size;
    data.format = TextureFormat::RGBA8;
    data.label = "bruit de detail du sol (Phase 27)";
    data.epsg = 0; // non georeferencee

    uint32_t w = size;
    std::vector<float> current = std::move(level);
    for (;;) {
        TextureMipLevel mip;
        mip.width = w;
        mip.height = w;
        mip.offset = data.pixels.size();
        mip.size = static_cast<uint64_t>(w) * w * 4;
        data.mips.push_back(mip);
        for (float f : current) {
            data.pixels.push_back(toByte(f));
        }
        if (w == 1) {
            break;
        }
        const uint32_t half = w / 2;
        std::vector<float> next(static_cast<size_t>(half) * half * 4);
        for (uint32_t y = 0; y < half; ++y) {
            for (uint32_t x = 0; x < half; ++x) {
                for (uint32_t ch = 0; ch < 4; ++ch) {
                    const auto at = [&](uint32_t xx, uint32_t yy) {
                        return current[(static_cast<size_t>(yy) * w + xx) * 4 + ch];
                    };
                    next[(static_cast<size_t>(y) * half + x) * 4 + ch] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) +
                                 at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                }
            }
        }
        current = std::move(next);
        w = half;
    }
    return data;
}

} // namespace vercora
