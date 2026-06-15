// soh3d_oracle — decode a raw PICA (3DS) texture blob with AZAHAR'S OWN decoder
// (Pica::Texture::LookupTexture, the emulator's ground truth) and write it as a
// PPM. The SoH3D toolchain decodes the same CMB texture bytes with its own Python
// decoder (tools/pica_texture.py); diffing the two PPMs validates the converter's
// decode data-driven, with the real emulator as oracle — no emulator run / no
// in-game navigation needed.
//
// Usage: soh3d_oracle <raw.bin> <width> <height> <pica_format> <out.ppm>
//   pica_format: integer of Pica::TexturingRegs::TextureFormat
//     0=RGBA8 1=RGB8 2=RGB5A1 3=RGB565 4=RGBA4 5=IA8 6=RG8 7=I8
//     8=A8 9=IA4 10=I4 11=A4 12=ETC1 13=ETC1A4
//
// Part of the SoH3D Azahar oracle. Built as a standalone target that links only
// citra_common + Azahar's texture_decode/etc1 sources (no GL/Vulkan/core).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/common_types.h"
#include "video_core/pica/regs_texturing.h"
#include "video_core/texture/texture_decode.h"

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: %s <raw.bin> <width> <height> <pica_format> <out.ppm>\n", argv[0]);
        return 2;
    }
    const char* raw_path = argv[1];
    const u32 width = static_cast<u32>(std::atoi(argv[2]));
    const u32 height = static_cast<u32>(std::atoi(argv[3]));
    const int fmt = std::atoi(argv[4]);
    const char* out_path = argv[5];

    std::FILE* f = std::fopen(raw_path, "rb");
    if (!f) {
        std::fprintf(stderr, "soh3d_oracle: cannot open %s\n", raw_path);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<u8> data(static_cast<size_t>(n));
    if (std::fread(data.data(), 1, static_cast<size_t>(n), f) != static_cast<size_t>(n)) {
        std::fprintf(stderr, "soh3d_oracle: short read on %s\n", raw_path);
        return 1;
    }
    std::fclose(f);

    Pica::Texture::TextureInfo info{};
    info.physical_address = 0;
    info.width = width;
    info.height = height;
    info.format = static_cast<Pica::TexturingRegs::TextureFormat>(fmt);
    info.is_shadow_source = false;
    info.SetDefaultStride();

    std::FILE* o = std::fopen(out_path, "wb");
    if (!o) {
        std::fprintf(stderr, "soh3d_oracle: cannot write %s\n", out_path);
        return 1;
    }
    std::fprintf(o, "P6\n%u %u\n255\n", width, height);
    // Emit rows in LookupTexture's NATIVE (x,y) order (no flip), so a direct,
    // unambiguous diff against the converter's pica_texture.decode is possible.
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const auto c = Pica::Texture::LookupTexture(data.data(), x, y, info, false);
            std::fputc(c.r(), o);
            std::fputc(c.g(), o);
            std::fputc(c.b(), o);
        }
    }
    std::fclose(o);
    std::fprintf(stderr, "soh3d_oracle: wrote %s (%ux%u, pica fmt %d)\n", out_path, width, height, fmt);
    return 0;
}
