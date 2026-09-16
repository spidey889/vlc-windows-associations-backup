/* Copyright (c) 2026 cssxn
 * SPDX-License-Identifier: MIT
 */

#ifndef USERCHOICELATESTHASH_COMMON_H
#define USERCHOICELATESTHASH_COMMON_H

#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace UserChoiceLatestHash
{
typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned __int64 U64;

// The codec consumes Windows UTF-16 units and uses fixed-width wrapping math.
static_assert(sizeof(wchar_t) == 2 && sizeof(U16) == 2 &&
              sizeof(U32) == 4 && sizeof(U64) == 8,
              "UserChoiceLatest requires Windows UTF-16 and fixed-width integers");

extern const U64 kRepeatSentinel;
extern const U64 kRepeatTokenA;
extern const U64 kRepeatTokenB;
extern const U64 kRepeatTokenC;
extern const U64 kRepeatTokenD;
extern const U64 kRepeatTokenE;
constexpr U32 kSeenModulo = 0x2803U;
// One bit per hash index, rounded up to whole 64-bit words.
constexpr U32 kSeenQwordCount = (kSeenModulo + 63U) / 64U;

struct WorkingSeeds
{
    U32 a[4];
    U32 b[4];
};

struct DebugData
{
    std::vector<U16> packed_words;
    U32 md5_words[4];
    U32 pair_a[2];
    U32 pair_b[2];

    DebugData();
};

struct Tables
{
    std::vector<U8> byte_class_table;
    std::vector<U8> token_region;
    std::vector<U8> fallback_token_code_table;
    std::vector<U8> token_bit_width_table;
    std::vector<U8> short_rewind_table;
    std::vector<U8> long_rewind_table;
};

struct EncoderState
{
    std::vector<U64> entries;
    U64 seen[kSeenQwordCount];
    U64 count;
    U64 last;
    U32 repeat;
    U32 total_units;

    EncoderState();
};

U32 ReadLe32(const U8 *bytes);
bool ComputeMd5Words(const U8 *data, U32 size, U32 out_words[4]);
void LoadProvidedSeeds(WorkingSeeds *out);
std::wstring ToLowerWide(const std::wstring &value);
const Tables &GetTables();

// The caller assembles the class-specific fields; the codec lowercases their
// concatenation and hashes its UTF-16 representation. No registry I/O occurs here.
// lowercase_output controls only the spelling of the final Base64 text; registry
// hashes use false. This is a compatibility option, not Base64 normalization.
// Returns false for encoding/Windows API failures without changing out_hash.
// Allocation failures propagate to the caller. debug_data is optional and may
// contain partial intermediate results on failure; it must not be persisted.
bool ComputeHash(const std::wstring &canonical_input,
                 const WorkingSeeds &seeds,
                 bool lowercase_output,
                 std::wstring *out_hash,
                 DebugData *debug_data);

} // namespace UserChoiceLatestHash

#endif
