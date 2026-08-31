/*
 * SX V2 host-reference encoder.
 *
 * This deliberately lives outside the PS1 target.  It packages the best
 * Mac Studio result as two independently described LZMA2 streams:
 *   - 0x000000..0x044c60: lc=2, lp=2, pb=2, 256 KiB dictionary
 *   - 0x044c60..0x080000: lc=3, lp=0, pb=2, 256 KiB dictionary
 *
 * Build on macOS with Homebrew xz/liblzma.  No input bytes are embedded in
 * the repository; --self-test uses generated data only.
 */

#include <lzma.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "sx_format.h"

namespace {

constexpr uint8_t kDictionaryLog2 = 18;

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc ^ 0xffffffffu;
}

struct Profile {
    uint8_t codec;
    uint8_t flags;
    uint8_t lc;
    uint8_t lp;
    uint8_t pb;
    uint8_t dict_log2;
};

Profile profile_for_area(unsigned area) {
    if (area == 0) {
        return {SX_CODEC_LZMA2_MIPS, SX_V2_AREA_FLAG_MIPS_ALIGNED, 2, 2, 2,
                kDictionaryLog2};
    }
    return {SX_CODEC_LZMA2, 0, 3, 0, 2, kDictionaryLog2};
}

lzma_options_lzma make_options(const Profile &profile) {
    lzma_options_lzma options{};
    if (lzma_lzma_preset(&options, LZMA_PRESET_EXTREME | 9u)) {
        throw std::runtime_error("lzma_lzma_preset failed");
    }
    options.dict_size = 1u << profile.dict_log2;
    options.lc = profile.lc;
    options.lp = profile.lp;
    options.pb = profile.pb;
    options.mode = LZMA_MODE_NORMAL;
    options.nice_len = 273;
    options.mf = LZMA_MF_BT4;
    return options;
}

std::vector<uint8_t> raw_lzma2_encode(const uint8_t *input, size_t input_size,
                                      const Profile &profile) {
    lzma_options_lzma options = make_options(profile);
    lzma_filter filters[] = {
        {LZMA_FILTER_LZMA2, &options},
        {LZMA_VLI_UNKNOWN, nullptr},
    };

    std::vector<uint8_t> output(input_size + input_size / 2u + 1024u);
    while (true) {
        size_t output_position = 0;
        const lzma_ret result = lzma_raw_buffer_encode(
            filters, nullptr, input, input_size, output.data(), &output_position,
            output.size());
        if (result == LZMA_OK) {
            output.resize(output_position);
            return output;
        }
        if (result != LZMA_BUF_ERROR || output.size() > input_size * 4u + 65536u) {
            throw std::runtime_error("lzma_raw_buffer_encode failed: " +
                                     std::to_string(static_cast<int>(result)));
        }
        output.resize(output.size() * 2u);
    }
}

std::vector<uint8_t> raw_lzma2_decode(const uint8_t *input, size_t input_size,
                                      size_t expected_size, const Profile &profile) {
    lzma_options_lzma options = make_options(profile);
    lzma_filter filters[] = {
        {LZMA_FILTER_LZMA2, &options},
        {LZMA_VLI_UNKNOWN, nullptr},
    };
    std::vector<uint8_t> output(expected_size);
    size_t input_position = 0;
    size_t output_position = 0;
    const lzma_ret result = lzma_raw_buffer_decode(
        filters, nullptr, input, &input_position, input_size,
        output.data(), &output_position, output.size());
    if (result != LZMA_OK || input_position != input_size ||
        output_position != expected_size) {
        throw std::runtime_error("lzma_raw_buffer_decode failed: " +
                                 std::to_string(static_cast<int>(result)));
    }
    return output;
}

struct Area {
    uint32_t offset;
    Profile profile;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> build_container(const std::vector<uint8_t> &input) {
    if (input.size() != SX_BIOS_SIZE) {
        throw std::runtime_error("input must be exactly 524288 bytes");
    }

    const uint32_t split = std::min<uint32_t>(SX_V2_MIPS_AREA_END, input.size());
    const uint32_t offsets[] = {0, split};
    const uint32_t sizes[] = {split, static_cast<uint32_t>(input.size() - split)};
    std::vector<Area> areas;
    for (unsigned i = 0; i < 2; ++i) {
        const Profile profile = profile_for_area(i);
        areas.push_back({offsets[i], profile,
                         raw_lzma2_encode(input.data() + offsets[i], sizes[i], profile)});
    }

    const size_t table_size = areas.size() * sizeof(sx_v2_area_header_t);
    const size_t payload_size = areas[0].payload.size() + areas[1].payload.size();
    const size_t stored_size = table_size + payload_size;
    const size_t total_size = sizeof(sx_v2_header_t) + stored_size;
    if (stored_size > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("container too large");
    }

    sx_v2_header_t header{};
    header.magic = SX_MAGIC;
    header.version = SX_VERSION_V2;
    header.header_size = sizeof(header);
    header.original_size = static_cast<uint32_t>(input.size());
    header.stored_size = static_cast<uint32_t>(stored_size);
    header.image_crc32 = crc32(input.data(), input.size());
    header.area_count = static_cast<uint16_t>(areas.size());
    header.flags = SX_V2_FLAG_AREA_SPLIT;
    header.area_header_size = sizeof(sx_v2_area_header_t);
    header.header_crc32 = 0;
    header.header_crc32 = crc32(reinterpret_cast<const uint8_t *>(&header), sizeof(header));

    std::vector<uint8_t> output(total_size);
    std::memcpy(output.data(), &header, sizeof(header));
    size_t at = sizeof(header);
    for (unsigned i = 0; i < areas.size(); ++i) {
        const Area &area = areas[i];
        const uint32_t original_size = i == 0 ? split : static_cast<uint32_t>(input.size() - split);
        sx_v2_area_header_t area_header{};
        area_header.index = static_cast<uint16_t>(i);
        area_header.codec = area.profile.codec;
        area_header.flags = area.profile.flags;
        area_header.original_offset = area.offset;
        area_header.original_size = original_size;
        area_header.stored_size = static_cast<uint32_t>(area.payload.size());
        area_header.original_crc32 = crc32(input.data() + area.offset, original_size);
        area_header.stored_crc32 = crc32(area.payload.data(), area.payload.size());
        area_header.lc = area.profile.lc;
        area_header.lp = area.profile.lp;
        area_header.pb = area.profile.pb;
        area_header.dict_log2 = kDictionaryLog2;
        std::memcpy(output.data() + at, &area_header, sizeof(area_header));
        at += sizeof(area_header);
    }
    for (const Area &area : areas) {
        std::memcpy(output.data() + at, area.payload.data(), area.payload.size());
        at += area.payload.size();
    }
    if (at != output.size()) {
        throw std::runtime_error("container size accounting error");
    }
    return output;
}

void verify_container(const std::vector<uint8_t> &container,
                      const std::vector<uint8_t> &expected) {
    if (container.size() < sizeof(sx_v2_header_t)) {
        throw std::runtime_error("V2 container too short");
    }
    sx_v2_header_t header{};
    std::memcpy(&header, container.data(), sizeof(header));
    const uint32_t sent_header_crc = header.header_crc32;
    header.header_crc32 = 0;
    if (header.magic != SX_MAGIC || header.version != SX_VERSION_V2 ||
        header.header_size != sizeof(header) ||
        header.original_size != expected.size() ||
        header.area_header_size != sizeof(sx_v2_area_header_t) ||
        header.area_count != 2 ||
        header.flags != SX_V2_FLAG_AREA_SPLIT ||
        header.stored_size + sizeof(header) != container.size() ||
        crc32(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) != sent_header_crc) {
        throw std::runtime_error("V2 header validation failed");
    }

    std::vector<uint8_t> restored(expected.size());
    size_t table_at = sizeof(header);
    size_t payload_at = table_at + header.area_count * sizeof(sx_v2_area_header_t);
    if (payload_at > container.size()) {
        throw std::runtime_error("V2 area table truncated");
    }
    for (unsigned i = 0; i < header.area_count; ++i) {
        sx_v2_area_header_t area{};
        std::memcpy(&area, container.data() + table_at, sizeof(area));
        table_at += sizeof(area);
        const Profile expected_profile = profile_for_area(i);
        if (area.index != i || area.codec != expected_profile.codec ||
            area.flags != expected_profile.flags || area.lc != expected_profile.lc ||
            area.lp != expected_profile.lp || area.pb != expected_profile.pb ||
            area.dict_log2 != expected_profile.dict_log2 ||
            area.original_offset + area.original_size > restored.size() ||
            payload_at + area.stored_size > container.size() ||
            crc32(container.data() + payload_at, area.stored_size) != area.stored_crc32) {
            throw std::runtime_error("V2 area header or stored CRC failed");
        }
        const Profile profile{area.codec, area.flags, area.lc, area.lp, area.pb,
                              area.dict_log2};
        const std::vector<uint8_t> decoded = raw_lzma2_decode(
            container.data() + payload_at, area.stored_size, area.original_size, profile);
        if (crc32(decoded.data(), decoded.size()) != area.original_crc32) {
            throw std::runtime_error("V2 area restored CRC failed");
        }
        std::copy(decoded.begin(), decoded.end(), restored.begin() + area.original_offset);
        payload_at += area.stored_size;
    }
    if (payload_at != container.size() || restored != expected ||
        crc32(restored.data(), restored.size()) != header.image_crc32) {
        throw std::runtime_error("V2 image validation failed");
    }
}

std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input: " + path);
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) throw std::runtime_error("cannot stat input: " + path);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) file.read(reinterpret_cast<char *>(data.data()), size);
    if (!file) throw std::runtime_error("cannot read input: " + path);
    return data;
}

void write_file(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open output: " + path);
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) throw std::runtime_error("cannot write output: " + path);
}

void self_test() {
    std::vector<uint8_t> input(SX_BIOS_SIZE);
    for (size_t i = 0; i < input.size(); ++i) {
        if (i < SX_V2_MIPS_AREA_END) {
            const uint32_t word = (static_cast<uint32_t>((i / 4) & 31u) << 26) |
                                  (static_cast<uint32_t>((i / 16) & 31u) << 21) |
                                  static_cast<uint32_t>((i * 37u) & 0xffffu);
            input[i] = static_cast<uint8_t>(word >> ((i & 3u) * 8));
        } else {
            input[i] = static_cast<uint8_t>((i * 29u + (i >> 8)) & 0xffu);
        }
    }
    const std::vector<uint8_t> container = build_container(input);
    verify_container(container, input);
    std::cout << "PASS sx_v2_lzma self-test total=" << container.size()
              << " bytes mips_end=0x" << std::hex << SX_V2_MIPS_AREA_END
              << std::dec << "\n";
}

void usage(const char *program) {
    std::cerr << "usage: " << program << " --input BIOS --output CONTAINER\n"
              << "       " << program << " --self-test\n";
}

}  // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            self_test();
            return 0;
        }
        if (argc != 5 || std::string(argv[1]) != "--input" ||
            std::string(argv[3]) != "--output") {
            usage(argv[0]);
            return 2;
        }
        const std::vector<uint8_t> input = read_file(argv[2]);
        const std::vector<uint8_t> container = build_container(input);
        verify_container(container, input);
        write_file(argv[4], container);
        std::cout << "PASS sx_v2_lzma input=" << input.size()
                  << " container=" << container.size()
                  << " saved=" << (input.size() - container.size()) << " bytes\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ERROR " << error.what() << "\n";
        return 1;
    }
}
