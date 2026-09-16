/* Copyright (c) 2026 cssxn
 * SPDX-License-Identifier: MIT
 */

#include "HashCommon.h"

#include "UserChoiceLatestHashTables.inc"

namespace UserChoiceLatestHash
{
// Token tags and seeds are format data from the pinned import (see LICENSE),
// not tunable compression parameters. They must agree with the lookup tables.
const U64 kRepeatSentinel = 0xFBFFFFFFFFFFFFFFULL;
const U64 kRepeatTokenA = 0xFE00000000000000ULL;
const U64 kRepeatTokenB = 0xFE00800000000000ULL;
const U64 kRepeatTokenC = 0xFF00000000000000ULL;
const U64 kRepeatTokenD = 0xFE007DFFFFFFFFFFULL;
const U64 kRepeatTokenE = 0xFE00801F7FFFFFFFULL;

static const U8 kProvidedSeedABytes[16] =
{
    0x00, 0x00, 0xFB, 0x69, 0x95, 0xA3, 0xF8, 0x79,
    0x25, 0xEC, 0xE8, 0x3C, 0x05, 0x96, 0xFA, 0x10
};

static const U8 kProvidedSeedBBytes[16] =
{
    0x6D, 0xB9, 0xCE, 0x12, 0x00, 0x00, 0x89, 0x2B,
    0x00, 0x00, 0x9F, 0x5B, 0x01, 0x69, 0x7C, 0x2C
};

DebugData::DebugData()
{
    ZeroMemory(md5_words, sizeof(md5_words));
    ZeroMemory(pair_a, sizeof(pair_a));
    ZeroMemory(pair_b, sizeof(pair_b));
}

EncoderState::EncoderState()
    : entries(4096U, 0ULL),
      count(1ULL),
      last(kRepeatSentinel),
      repeat(0U),
      total_units(0U)
{
    ZeroMemory(seen, sizeof(seen));
    entries[0] = 0ULL;
}

U32 ReadLe32(const U8 *bytes)
{
    return static_cast<U32>(bytes[0])
         | (static_cast<U32>(bytes[1]) << 8)
         | (static_cast<U32>(bytes[2]) << 16)
         | (static_cast<U32>(bytes[3]) << 24);
}

static void LoadSeedWords(const U8 bytes[16], U32 out[4])
{
    for (size_t i = 0U; i < 4U; ++i)
    {
        out[i] = ReadLe32(bytes + (i * 4U));
    }
}

void LoadProvidedSeeds(WorkingSeeds *out)
{
    LoadSeedWords(kProvidedSeedABytes, out->a);
    LoadSeedWords(kProvidedSeedBBytes, out->b);
}

std::wstring ToLowerWide(const std::wstring &value)
{
    // Match the pinned codec's C-locale bytes, independently of the process
    // locale. Unicode-wide casing changes non-ASCII ProgId hashes Windows accepts.
    std::wstring lowered(value);
    for (wchar_t &character : lowered)
    {
        if (character >= L'A' && character <= L'Z')
            character += L'a' - L'A';
    }
    return lowered;
}

static void DecodeHexLiteral(const char *hex, std::vector<U8> *out)
{
    const size_t len = strlen(hex);
    out->assign(len / 2U, 0U);
    for (size_t i = 0U; i < len; i += 2U)
    {
        const char a = hex[i];
        const char b = hex[i + 1U];
        const int hi = (a >= '0' && a <= '9') ? (a - '0') : ((a | 32) - 'a' + 10);
        const int lo = (b >= '0' && b <= '9') ? (b - '0') : ((b | 32) - 'a' + 10);
        (*out)[i / 2U] = static_cast<U8>((hi << 4) | lo);
    }
}

const Tables &GetTables()
{
    // Function-local initialization publishes one complete, immutable table set
    // to concurrent callers; all mutable encoding state belongs to each hash call.
    static const Tables tables = [] {
        Tables tables;
        DecodeHexLiteral(kTokenRegionHex, &tables.token_region);
        DecodeHexLiteral(kLongRewindTableHex, &tables.long_rewind_table);
        DecodeHexLiteral(kFallbackTokenCodeTableHex, &tables.fallback_token_code_table);
        DecodeHexLiteral(kByteClassTableHex, &tables.byte_class_table);
        DecodeHexLiteral(kTokenBitWidthTableHex, &tables.token_bit_width_table);
        DecodeHexLiteral(kShortRewindTableHex, &tables.short_rewind_table);
        return tables;
    }();
    return tables;
}
} // namespace UserChoiceLatestHash
