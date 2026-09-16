/*****************************************************************************
 * userchoice_hash.cpp: legacy Windows UserChoice hash
 *****************************************************************************
 * Legacy UserChoice hash implementation based on public UserChoice research
 * and the MIT-licensed tools_setfta implementation.
 *
 * Research: https://kolbi.cz/blog/2017/10/25/setuserfta-userchoice-hash-defeated-set-file-type-associations-per-user/
 * Source: https://github.com/mullerdavid/tools_setfta
 *
 * Copyright (c) 2020 David Muller
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 * See LICENSE for the full license text.
 *****************************************************************************/

#include "userchoice_hash.hpp"
#include "userchoice_latest_hash/HashCommon.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace vlc::win32
{
namespace
{
using ByteBuffer = std::vector<std::uint8_t>;
using Md5Words = std::array<UserChoiceLatestHash::U32, 4>;
using MixedHash = std::array<std::uint8_t, 8>;

static_assert(std::numeric_limits<std::uint32_t>::digits == 32,
              "The UserChoice mixer requires 32-bit uint32_t values.");

std::uint32_t read_little_endian_word(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void write_little_endian_word(std::uint32_t word, std::uint8_t *destination)
{
    destination[0] = static_cast<std::uint8_t>(word);
    destination[1] = static_cast<std::uint8_t>(word >> 8u);
    destination[2] = static_cast<std::uint8_t>(word >> 16u);
    destination[3] = static_cast<std::uint8_t>(word >> 24u);
}

// The concatenated fields are intentionally lowercased as ASCII before being
// emitted as UTF-16LE. These Windows identifiers and the experience marker are
// ASCII, so this avoids locale-dependent transformations while matching the
// byte sequence consumed by the UserChoice algorithm.
ByteBuffer build_user_choice_input(std::string_view extension, std::string_view sid,
                                   std::string_view program_id, std::string_view registry_timestamp)
{
    static constexpr std::string_view experience_marker =
        "user choice set via windows user experience "
        "{d18b6dd5-6124-4341-9318-804003bafa0b}";

    std::string text;
    text.reserve(extension.size() + sid.size() + program_id.size() + registry_timestamp.size() +
                 experience_marker.size() + 1u);
    text.append(extension);
    text.append(sid);
    text.append(program_id);
    text.append(registry_timestamp);
    text.append(experience_marker);
    // The UTF-16 terminator is part of the hash input.
    text.push_back('\0');

    ByteBuffer utf16le;
    utf16le.reserve(text.size() * 2u);
    for (char character : text)
    {
        unsigned char code_unit = static_cast<unsigned char>(character);
        if (code_unit >= 'A' && code_unit <= 'Z')
        {
            code_unit = static_cast<unsigned char>(code_unit + ('a' - 'A'));
        }
        utf16le.push_back(static_cast<std::uint8_t>(code_unit));
        utf16le.push_back(0u);
    }
    return utf16le;
}

std::vector<std::uint32_t> make_even_word_sequence(const ByteBuffer &bytes)
{
    // Only complete DWORD pairs enter the mixers; MD5 still uses all bytes.
    const std::uint32_t complete_word_count = static_cast<std::uint32_t>(bytes.size() >> 2u);
    const std::uint32_t even_word_adjustment = (complete_word_count & 1u) < 1u ? 1u : 0u;
    const std::uint32_t even_word_count = even_word_adjustment + complete_word_count - 1u;

    std::vector<std::uint32_t> words(even_word_count);
    for (std::uint32_t index = 0; index < even_word_count; ++index)
    {
        words[index] = read_little_endian_word(bytes.data() + index * 4u);
    }
    return words;
}

// First of the two fixed UserChoice mixing transforms. The constants and the
// sequence of wrapping uint32_t operations are reverse-engineered compatibility
// requirements; they intentionally do not have a higher-level derivation here.
MixedHash run_first_mixing_transform(const ByteBuffer &input, const Md5Words &digest)
{
    MixedHash output{};
    const std::vector<std::uint32_t> words = make_even_word_sequence(input);
    const std::uint32_t word_count = static_cast<std::uint32_t>(words.size());
    if (word_count <= 1u || (word_count & 1u) != 0u)
    {
        return output;
    }

    const std::uint32_t first_key = (digest[0] | 1u) + 0x69FB0000u;
    const std::uint32_t second_key = (digest[1] | 1u) + 0x13DB0000u;

    std::uint32_t rolling_sum = 0u;
    std::uint32_t latest_value = 0u;
    std::uint32_t input_index = 0u;

    const std::uint32_t complete_pairs_before_increment = (word_count - 2u) >> 1u;
    const std::uint32_t pair_count = complete_pairs_before_increment + 1u;
    const std::uint32_t possible_tail_index = pair_count;

    for (std::uint32_t pair = 0; pair < pair_count; ++pair)
    {
        const std::uint32_t first_input = words[input_index] + latest_value;
        input_index += 2u;

        const std::uint32_t first_reduction =
            first_key * first_input - 0x10FA9605u * (first_input >> 16u);
        const std::uint32_t first_expansion =
            0x79F8A395u * first_reduction + 0x689B6B9Fu * (first_reduction >> 16u);
        const std::uint32_t first_mixed =
            0xEA970001u * first_expansion - 0x3C101569u * (first_expansion >> 16u);
        const std::uint32_t sum_before_second_half = first_mixed + rolling_sum;

        const std::uint32_t second_input = words[input_index - 1u] + first_mixed;
        const std::uint32_t second_reduction =
            second_key * second_input - 0x3CE8EC25u * (second_input >> 16u);
        const std::uint32_t second_expansion =
            0x59C3AF2Du * second_reduction - 0x2232E0F1u * (second_reduction >> 16u);
        latest_value = 0x1EC90001u * second_expansion + 0x35BD1EC9u * (second_expansion >> 16u);
        rolling_sum = latest_value + sum_before_second_half;
    }

    // Retain the algorithm's final-word path even though normal evenized input
    // reaches the pair loop without a remainder.
    if (word_count - 2u - 2u * complete_pairs_before_increment == 1u)
    {
        const std::uint32_t tail_input = words[2u * possible_tail_index] + latest_value;
        const std::uint32_t tail_reduction =
            tail_input * first_key - 0x10FA9605u * (tail_input >> 16u);
        const std::uint32_t tail_first_mix =
            0x39646B9Fu * (tail_reduction >> 16u) + 0x28DBA395u * tail_reduction -
            0x3C101569u *
                ((0x689B6B9Fu * (tail_reduction >> 16u) + 0x79F8A395u * tail_reduction) >> 16u);
        const std::uint32_t tail_second_reduction =
            tail_first_mix * second_key - 0x3CE8EC25u * (tail_first_mix >> 16u);
        const std::uint32_t tail_second_expansion =
            0x59C3AF2Du * tail_second_reduction - 0x2232E0F1u * (tail_second_reduction >> 16u);
        latest_value = 0x35BD1EC9u * (tail_second_expansion >> 16u) +
                       0x2A18AF2Du * tail_second_reduction -
                       0xFD6BE0F1u * (tail_second_reduction >> 16u);
        rolling_sum += latest_value + tail_first_mix;
    }

    write_little_endian_word(latest_value, output.data());
    write_little_endian_word(rolling_sum, output.data() + 4u);
    return output;
}

// Second fixed mixing transform. As above, every arithmetic result deliberately
// stays in uint32_t so overflow has the modulo-2^32 behavior the hash expects.
MixedHash run_second_mixing_transform(const ByteBuffer &input, const Md5Words &digest)
{
    MixedHash output{};
    const std::vector<std::uint32_t> words = make_even_word_sequence(input);
    const std::uint32_t word_count = static_cast<std::uint32_t>(words.size());
    if (word_count <= 1u || (word_count & 1u) != 0u)
    {
        return output;
    }

    const std::uint32_t first_seed = digest[0] | 1u;
    const std::uint32_t second_seed = digest[1] | 1u;
    const std::uint32_t first_factor = 0xB1110000u * first_seed;
    const std::uint32_t second_factor = 0x16F50000u * second_seed;

    std::uint32_t rolling_value = 0u;
    std::uint32_t aggregate = 0u;
    std::uint32_t input_index = 0u;

    const std::uint32_t complete_pairs_before_increment = (word_count - 2u) >> 1u;
    const std::uint32_t pair_count = complete_pairs_before_increment + 1u;

    for (std::uint32_t pair = 0; pair < pair_count; ++pair)
    {
        input_index += 2u;

        const std::uint32_t first_input = words[input_index - 2u] + rolling_value;
        const std::uint32_t first_reduction =
            first_input * first_factor - 0x30674EEFu * ((first_seed * first_input) >> 16u);
        const std::uint32_t first_high = first_reduction >> 16u;
        const std::uint32_t first_expansion =
            0xE9B30000u * first_high +
            0x12CEB96Du * ((0x5B9F0000u * first_reduction - 0x78F7A461u * first_high) >> 16u);
        const std::uint32_t first_mixed =
            0x1D830000u * first_expansion + 0x257E1D83u * (first_expansion >> 16u);

        const std::uint32_t second_input = first_mixed + words[input_index - 1u];
        const std::uint32_t second_reduction =
            second_input * second_factor - 0x5D8BE90Bu * ((second_seed * second_input) >> 16u);
        const std::uint32_t second_high = second_reduction >> 16u;
        const std::uint32_t second_mixed =
            (0x96FF0000u * second_reduction - 0x2C7C6901u * second_high) >> 16u;

        rolling_value =
            0xF2310000u * second_mixed -
            0x405B6097u * ((0x7C932B89u * second_mixed - 0x5C890000u * second_high) >> 16u);
        aggregate += rolling_value + first_mixed;
    }

    // This mirrors the transform's specified final-word path for completeness.
    if (word_count - 2u - 2u * complete_pairs_before_increment == 1u)
    {
        const std::uint32_t tail_input =
            words[2u * (complete_pairs_before_increment + 1u)] + rolling_value;
        const std::uint32_t tail_first_reduction = 0xB1110000u * first_seed * tail_input -
                                                   0x30674EEFu * ((first_seed * tail_input) >> 16u);
        const std::uint32_t tail_first_high = tail_first_reduction >> 16u;
        const std::uint32_t tail_expansion_high =
            (0x5B9F0000u * tail_first_reduction - 0x78F7A461u * (tail_first_reduction >> 16u)) >>
            16u;
        const std::uint32_t tail_first_mixed =
            0x257E1D83u *
                ((0xE9B30000u * tail_first_high + 0x12CEB96Du * tail_expansion_high) >> 16u) +
            0x3BC70000u * tail_expansion_high;
        const std::uint32_t tail_second_reduction =
            0x16F50000u * tail_first_mixed * second_seed -
            0x5D8BE90Bu * ((tail_first_mixed * second_seed) >> 16u);
        const std::uint32_t tail_second_high = tail_second_reduction >> 16u;
        const std::uint32_t tail_second_mixed =
            (0x96FF0000u * tail_second_reduction - 0x2C7C6901u * tail_second_high) >> 16u;
        rolling_value =
            0xF2310000u * tail_second_mixed -
            0x405B6097u *
                ((0x7C932B89u * tail_second_mixed - 0x5C890000u * tail_second_high) >> 16u);
        aggregate += rolling_value + tail_first_mixed;
    }

    write_little_endian_word(rolling_value, output.data());
    write_little_endian_word(aggregate, output.data() + 4u);
    return output;
}

MixedHash xor_mixing_results(const MixedHash &first, const MixedHash &second)
{
    MixedHash combined{};
    for (std::size_t index = 0; index < combined.size(); ++index)
    {
        combined[index] = static_cast<std::uint8_t>(first[index] ^ second[index]);
    }
    return combined;
}

std::string encode_base64(const MixedHash &bytes)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string encoded;
    encoded.reserve(((bytes.size() + 2u) / 3u) * 4u);

    for (std::size_t offset = 0; offset < bytes.size(); offset += 3u)
    {
        const std::uint32_t first = bytes[offset];
        const std::uint32_t second = offset + 1u < bytes.size() ? bytes[offset + 1u] : 0u;
        const std::uint32_t third = offset + 2u < bytes.size() ? bytes[offset + 2u] : 0u;
        const std::uint32_t triple = (first << 16u) | (second << 8u) | third;

        encoded.push_back(alphabet[(triple >> 18u) & 0x3fu]);
        encoded.push_back(alphabet[(triple >> 12u) & 0x3fu]);
        encoded.push_back(offset + 1u < bytes.size() ? alphabet[(triple >> 6u) & 0x3fu] : '=');
        encoded.push_back(offset + 2u < bytes.size() ? alphabet[triple & 0x3fu] : '=');
    }
    return encoded;
}
} // namespace

std::string calculateLegacyUserChoiceHash(std::string_view extension, std::string_view sid,
                                          std::string_view program_id,
                                          std::string_view registry_timestamp)
{
    const ByteBuffer input =
        build_user_choice_input(extension, sid, program_id, registry_timestamp);
    if (input.size() > std::numeric_limits<UserChoiceLatestHash::U32>::max())
    {
        throw std::runtime_error("The UserChoice hash input is too large");
    }
    Md5Words digest{};
    if (!UserChoiceLatestHash::ComputeMd5Words(
            input.data(), static_cast<UserChoiceLatestHash::U32>(input.size()), digest.data()))
    {
        throw std::runtime_error("The UserChoice MD5 calculation failed");
    }
    const MixedHash first_mix = run_first_mixing_transform(input, digest);
    const MixedHash second_mix = run_second_mixing_transform(input, digest);
    return encode_base64(xor_mixing_results(first_mix, second_mix));
}
} // namespace vlc::win32
