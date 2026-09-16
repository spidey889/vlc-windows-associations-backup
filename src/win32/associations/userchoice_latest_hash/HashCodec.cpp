/* Copyright (c) 2026 cssxn
 * Copyright (c) 2026 Vinit Rajpurohit (spidey889)
 * SPDX-License-Identifier: MIT
 */

#include "HashCommon.h"

#include <limits>

namespace UserChoiceLatestHash
{
// Encoding and mixing follow the pinned implementation recorded in LICENSE.
// The tables and operation order define the registry hash's byte representation.
static U32 LowMul(U32 a, U32 b)
{
    return static_cast<U32>(static_cast<U64>(a) * static_cast<U64>(b));
}

static bool HashCryptoAlg(ALG_ID alg_id,
                          DWORD provider_type,
                          const BYTE *data,
                          DWORD size,
                          std::vector<U8> *out)
{
    if (out == NULL)
    {
        return false;
    }

    // Allocation below can throw; release both handles on every exit path.
    struct CryptoHandles
    {
        HCRYPTPROV prov = 0;
        HCRYPTHASH hash = 0;
        ~CryptoHandles()
        {
            if (hash != 0)
                CryptDestroyHash(hash);
            if (prov != 0)
                CryptReleaseContext(prov, 0);
        }
    } handles;
    DWORD hash_len = 0;
    DWORD hash_len_size = sizeof(hash_len);
    bool ok = false;

    if (!CryptAcquireContextW(&handles.prov, NULL, NULL, provider_type, CRYPT_VERIFYCONTEXT))
    {
        return false;
    }
    if (!CryptCreateHash(handles.prov, alg_id, 0, 0, &handles.hash))
    {
        return false;
    }
    if (!CryptHashData(handles.hash, data, size, 0))
    {
        return false;
    }
    if (!CryptGetHashParam(handles.hash, HP_HASHSIZE, reinterpret_cast<BYTE *>(&hash_len), &hash_len_size, 0))
    {
        return false;
    }

    out->assign(hash_len, 0U);
    if (hash_len != 0U)
    {
        if (!CryptGetHashParam(handles.hash, HP_HASHVAL, &(*out)[0], &hash_len, 0))
        {
            out->clear();
        }
        else
        {
            ok = true;
        }
    }

    return ok;
}

bool ComputeMd5Words(const U8 *data, U32 size, U32 out_words[4])
{
    std::vector<U8> md5;
    if (!HashCryptoAlg(CALG_MD5, PROV_RSA_FULL, data, size, &md5) || md5.size() != 16U)
    {
        return false;
    }

    for (size_t i = 0; i < 4U; ++i)
    {
        out_words[i] = ReadLe32(&md5[i * 4U]);
    }
    return true;
}

static bool Base64NoCrLf(const BYTE *data, DWORD size, std::wstring *out)
{
    if (out == NULL)
    {
        return false;
    }

    DWORD chars = 0;
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &chars))
    {
        return false;
    }

    std::vector<wchar_t> buffer(chars + 1U, L'\0');
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &buffer[0], &chars))
    {
        return false;
    }

    *out = &buffer[0];
    return true;
}

static U32 HashIndex(U64 value)
{
    return static_cast<U32>(value % kSeenModulo);
}

static bool TestSeen(const EncoderState &state, U64 value)
{
    // This bitmap is only a search filter: modulo collisions are allowed.
    // FindPrevious still compares full tokens in the bounded history window.
    const U32 idx = HashIndex(value);
    return (state.seen[idx >> 6] & (1ULL << (idx & 63U))) != 0ULL;
}

static void MarkSeen(EncoderState &state, U64 value)
{
    const U32 idx = HashIndex(value);
    state.seen[idx >> 6] |= (1ULL << (idx & 63U));
}

static void PushEntry(EncoderState &state, U64 value)
{
    if (state.count >= state.entries.size())
    {
        state.entries.resize(state.entries.size() + 1024U, 0ULL);
    }
    state.entries[static_cast<size_t>(state.count++)] = value;
}

static bool FindPrevious(const EncoderState &state, U64 value, U64 *distance)
{
    if (distance == NULL || !TestSeen(state, value) || state.count == 0ULL)
    {
        return false;
    }

    const U64 start = (state.count > 0x400ULL) ? (state.count - 0x400ULL) : 0ULL;
    for (U64 idx = state.count - 1ULL;; --idx)
    {
        if (state.entries[static_cast<size_t>(idx)] == value)
        {
            *distance = state.count - idx;
            return true;
        }
        if (idx == start)
        {
            break;
        }
    }
    return false;
}

static U8 ClassFromWord(const Tables &tables, U16 ch)
{
    const U8 hi = static_cast<U8>(ch >> 8);
    if (hi != 0U)
    {
        return static_cast<U8>(tables.byte_class_table[hi] + 8U);
    }
    return tables.byte_class_table[static_cast<U8>(ch & 0x00FFU)];
}

// Tokens reserve the high byte for the table-selected class and pack UTF-16
// units into the remaining bits using that class's widths.
static bool BuildToken(const Tables &tables,
                       const U16 *cursor,
                       const U16 *end,
                       U64 *out_token,
                       U8 *out_kind,
                       U8 *out_len)
{
    if (cursor == NULL || end == NULL || cursor >= end || out_token == NULL || out_kind == NULL || out_len == NULL)
    {
        return false;
    }

    U32 code = tables.token_region[static_cast<size_t>(ClassFromWord(tables, *cursor)) + 256U];
    U32 token_len = tables.token_region[code];
    if (token_len > static_cast<size_t>(end - cursor))
    {
        return false;
    }

    U32 table_base = 168U * code;
    for (;;)
    {
        U32 pos = 0U;
        if (token_len == 0U)
        {
            break;
        }

        const U16 *probe = cursor;
        while (static_cast<U64>(*probe) < (1ULL << tables.token_bit_width_table[table_base + pos]))
        {
            ++pos;
            ++probe;
            if (pos >= token_len)
            {
                goto done;
            }
        }

        ++code;
        table_base = 168U * code;
        token_len = tables.token_region[code];
        if (token_len > static_cast<U32>(end - cursor))
        {
            return false;
        }
    }

done:
    {
        U64 token = static_cast<U64>(static_cast<U8>(code)) << 56;
        int shift = 56;
        for (U32 i = 0U; i < static_cast<U8>(token_len); ++i)
        {
            shift -= tables.token_bit_width_table[table_base + i];
            token |= static_cast<U64>(cursor[i]) << shift;
        }
        *out_token = token;
        *out_kind = static_cast<U8>(code);
        *out_len = static_cast<U8>(token_len);
    }

    return true;
}

// Unlike the first pass's full-token requirement, the tail pass selects a
// fallback code when its lookahead exceeds the remaining UTF-16 units.
static bool BuildTokenSecondPass(const Tables &tables,
                                 const U16 *cursor,
                                 const U16 *end,
                                 U64 *out_token,
                                 U8 *out_kind,
                                 U8 *out_len)
{
    if (cursor == NULL || end == NULL || cursor >= end || out_token == NULL || out_kind == NULL || out_len == NULL)
    {
        return false;
    }

    const U32 remain = static_cast<U32>(end - cursor);
    U32 code = tables.token_region[static_cast<size_t>(ClassFromWord(tables, *cursor)) + 256U];
    U32 token_len = tables.token_region[code];
    if (token_len > remain)
    {
        code = tables.fallback_token_code_table[remain];
        token_len = tables.token_region[code];
    }

    U32 table_base = 168U * code;
retry:
    U32 pos = 0U;
    if (token_len != 0U)
    {
        const U16 *probe = cursor;
        while (probe < end)
        {
            if (static_cast<U64>(*probe) >= (1ULL << tables.token_bit_width_table[table_base + pos]))
            {
                ++code;
                table_base = 168U * code;
                token_len = tables.token_region[code];
                if (token_len <= remain)
                {
                    goto retry;
                }
                break;
            }
            ++pos;
            ++probe;
            if (pos >= token_len)
            {
                break;
            }
        }
    }

    U64 token = static_cast<U64>(static_cast<U8>(code)) << 56;
    int shift = 56;
    for (U32 i = 0U; i < static_cast<U8>(token_len); ++i)
    {
        if (i >= remain)
        {
            break;
        }
        shift -= tables.token_bit_width_table[table_base + i];
        token |= static_cast<U64>(cursor[i]) << shift;
    }

    *out_token = token;
    *out_kind = static_cast<U8>(code);
    *out_len = static_cast<U8>(token_len);
    return true;
}

// Keep the emitters separate: the first pass also emits non-repeat history
// references using the next token's class. Both can rewind the cursor, but only
// the second pass has a final repeat flush. These differences affect hash bytes.
static bool EmitFirstPass(EncoderState &state,
                          const Tables &tables,
                          const U16 *&cursor,
                          const U16 *input,
                          const U16 *end,
                          U64 token,
                          U8 token_kind)
{
    if (token == state.last && state.repeat < 0xFFU)
    {
        ++state.repeat;
        return true;
    }

    if (state.repeat != 0U)
    {
        U64 distance = 0ULL;
        if (FindPrevious(state, state.last, &distance))
        {
            const U32 rep_minus_one = state.repeat - 1U;
            U64 entry = 0ULL;
            if (distance >= 0x800ULL)
            {
                const U64 packed = (token >> 17) | (static_cast<U64>(rep_minus_one) << 48) | kRepeatTokenA;
                MarkSeen(state, packed);
                PushEntry(state, packed);
                entry = state.last;
            }
            else
            {
                entry = (token >> 27)
                      | ((static_cast<U64>(((static_cast<U16>(distance) - 1U) & 0x03FFU) | (rep_minus_one << 11)) << 37))
                      | kRepeatTokenB;
            }

            MarkSeen(state, entry);
            PushEntry(state, entry);
            state.last = kRepeatSentinel;
            state.repeat = 0U;
            const U8 rewind = (distance >= 0x800ULL) ? tables.long_rewind_table[token_kind]
                                                   : tables.short_rewind_table[token_kind];
            if (rewind > cursor - input)
                return false;
            cursor -= rewind;
            return true;
        }
    }
    else
    {
        U64 distance = 0ULL;
        if (FindPrevious(state, token, &distance) && cursor < end)
        {
            const U8 next_kind = tables.token_region[static_cast<size_t>(ClassFromWord(tables, *cursor)) + 256U];
            const U64 entry = (distance >= 0x800ULL)
                            ? token
                            : ((token >> 18) | ((distance - 1ULL) << 46) | kRepeatTokenC);
            MarkSeen(state, entry);
            PushEntry(state, entry);
            if (distance < 0x800ULL)
            {
                const U8 rewind = tables.short_rewind_table[static_cast<size_t>(next_kind) + 256U];
                // Lookahead can request more history than was consumed. Reject
                // before forming an out-of-buffer pointer; clamping changes bytes.
                if (rewind > cursor - input)
                    return false;
                cursor -= rewind;
            }
            state.last = token;
            return true;
        }
    }

    MarkSeen(state, token);
    PushEntry(state, token);
    state.last = token;
    return true;
}

static void EmitSecondPass(EncoderState &state,
                           const Tables &tables,
                           const U16 *&cursor,
                           U64 token,
                           U8 token_kind)
{
    if (token == state.last && state.repeat < 0xFFU)
    {
        ++state.repeat;
        return;
    }

    if (state.repeat != 0U)
    {
        U64 distance = 0ULL;
        if (FindPrevious(state, state.last, &distance))
        {
            const U32 rep_minus_one = state.repeat - 1U;
            U64 entry = 0ULL;
            if (distance >= 0x800ULL)
            {
                const U64 packed = (token >> 17) | (static_cast<U64>(rep_minus_one) << 48) | kRepeatTokenA;
                MarkSeen(state, packed);
                PushEntry(state, packed);
                entry = state.last;
            }
            else
            {
                entry = (token >> 27)
                      | ((static_cast<U64>(((static_cast<U16>(distance) - 1U) & 0x03FFU) | (rep_minus_one << 11)) << 37))
                      | kRepeatTokenB;
            }

            MarkSeen(state, entry);
            PushEntry(state, entry);
            state.last = kRepeatSentinel;
            state.repeat = 0U;
            cursor -= (distance >= 0x800ULL) ? tables.long_rewind_table[token_kind] : tables.short_rewind_table[token_kind];
            return;
        }
    }

    MarkSeen(state, token);
    PushEntry(state, token);
    state.last = token;
}

static void FlushSecondPass(EncoderState &state)
{
    if (state.repeat == 0U)
    {
        return;
    }

    U64 entry = 0ULL;
    if (state.last == kRepeatSentinel)
    {
        entry = (static_cast<U64>(state.repeat) << 48) | kRepeatTokenD;
    }
    else
    {
        U64 distance = 0ULL;
        if (FindPrevious(state, state.last, &distance) && distance != 0ULL)
        {
            entry = (static_cast<U64>(((static_cast<U16>(distance) - 1U) & 0x03FFU) | ((state.repeat - 1U) << 11)) << 37)
                  | kRepeatTokenE;
        }
        else
        {
            const U64 repeat_token = (static_cast<U64>(state.repeat) << 48) | kRepeatTokenD;
            MarkSeen(state, repeat_token);
            PushEntry(state, repeat_token);
            entry = state.last;
        }
    }

    MarkSeen(state, entry);
    PushEntry(state, entry);
    state.repeat = 0U;
    state.last = kRepeatSentinel;
}

static bool FirstPass(EncoderState &state,
                      const Tables &tables,
                      const U16 *input,
                      size_t char_count,
                      U32 *consumed_chars)
{
    const U16 *end = input + char_count;
    const U16 *cursor = input;
    U64 result = (2249744775ULL * static_cast<U32>(char_count)) >> 32;

    *consumed_chars = 0U;
    U64 quota = (result + ((char_count - static_cast<size_t>(result)) >> 1)) >> 7;
    while (static_cast<U32>(quota) > 1U)
    {
        const U32 limit = static_cast<U32>(quota) - 1U;
        U32 processed = 0U;
        while (processed < limit)
        {
            U64 token = 0ULL;
            U8 kind = 0U;
            U8 token_len = 0U;
            if (!BuildToken(tables, cursor, end, &token, &kind, &token_len))
            {
                return false;
            }

            // consumed_chars counts token lengths, not net cursor movement:
            // rewinds can revisit units. ComputeHash uses this count as the
            // second-pass slice offset, so keep it within the original input.
            if (token_len == 0U || token_len > char_count - *consumed_chars)
                return false;
            *consumed_chars += token_len;
            cursor += token_len;
            if (!EmitFirstPass(state, tables, cursor, input, end, token, kind))
                return false;
            ++processed;
        }

        result = (2249744775ULL * static_cast<U32>(end - cursor)) >> 32;
        quota = (static_cast<U32>(result) + ((static_cast<U32>(end - cursor) - static_cast<U32>(result)) >> 1)) >> 7;
    }
    return true;
}

static void SecondPass(EncoderState &state,
                       const Tables &tables,
                       const U16 *input,
                       size_t char_count)
{
    const U16 *end = input + char_count;
    const U16 *cursor = input;

    while (cursor < end)
    {
        U64 token = 0ULL;
        U8 kind = 0U;
        U8 token_len = 0U;
        if (!BuildTokenSecondPass(tables, cursor, end, &token, &kind, &token_len))
        {
            break;
        }

        state.total_units += token_len;
        cursor += token_len;
        EmitSecondPass(state, tables, cursor, token, kind);
    }

    FlushSecondPass(state);
}

// Both mixers consume pairs of DWORDs with unsigned 32-bit wrapping arithmetic.
// Constants, truncation points and operation order are part of the imported
// format; algebraic rewrites must preserve these intermediate results as well.
static bool MixA(const U32 *words,
                 int dword_count,
                 const U32 md5_words[4],
                 const U32 seed[4],
                 U32 out_pair[2])
{
    if ((dword_count & 1) != 0)
    {
        return false;
    }

    const U32 md0 = md5_words[0] | 1U;
    const U32 md1 = (md5_words[1] | 1U) + 0x13DB0000U;
    const U32 seed0 = seed[0];
    const U32 seed1 = seed[1];
    const U32 seed2 = seed[2];
    const U32 seed3 = seed[3];

    U32 sum = 0U;
    U32 acc = 0U;
    const U32 *p = words;
    int remaining = dword_count;

    if ((((static_cast<U32>(remaining) - 2U) >> 1) + 1U) != 0U)
    {
        U32 loops = ((static_cast<U32>(remaining) - 2U) >> 1) + 1U;
        U32 mul = md0 + seed0;
        while (loops-- != 0U)
        {
            acc += p[0];
            remaining -= 2;

            const U32 x = acc;
            const U32 hi = x >> 16;
            p += 2;

            const U32 t = LowMul(x, mul) - LowMul(hi, seed3);
            mul = md0 + seed0;

            const U32 s = LowMul(t, seed1) + LowMul(t >> 16, 0x689B6B9FU);
            const U32 part1 = LowMul(s, 0xEA970001U) - LowMul(s >> 16, 0x3C101569U);

            const U32 w = p[-1] + part1;
            const U32 t2 = LowMul(w, md1) - LowMul(w >> 16, seed2);
            const U32 temp = LowMul(t2, 0x59C3AF2DU) - LowMul(t2 >> 16, 0x2232E0F1U);
            acc = LowMul(temp >> 16, 0x35BD1EC9U) + LowMul(temp, 0x1EC90001U);
            sum += part1 + acc;
        }
    }

    if (remaining == 1)
    {
        acc += p[0];
        const U32 t = LowMul(acc, md0 + seed0) - LowMul(acc >> 16, seed3);
        U32 part = LowMul(t, seed1) - LowMul(t >> 16, 0x689B6B9FU);
        part += LowMul(t >> 16, 0x39646B9FU);

        const U32 t2 = LowMul(part, md1) - LowMul(part >> 16, seed2);
        const U32 temp = LowMul(t2, 0x59C3AF2DU) - LowMul(t2 >> 16, 0x2232E0F1U);
        const U32 extra = LowMul(t2, 0x2A18AF2DU)
                        + LowMul(t2 >> 16, 0x02941F0FU)
                        + LowMul(temp >> 16, 0x35BD1EC9U);
        acc = extra;
        sum += part + acc;
    }

    out_pair[0] = acc;
    out_pair[1] = sum;
    return true;
}

static bool MixB(const U32 *words,
                 int dword_count,
                 const U32 md5_words[4],
                 const U32 seed[4],
                 U32 out_pair[2])
{
    if ((dword_count & 1) != 0)
    {
        return false;
    }

    const U32 seed0 = seed[0];
    const U32 seed1 = seed[1];
    const U32 seed2 = seed[2];
    const U32 seed3 = seed[3];
    const U32 md1 = md5_words[1] | 1U;
    const U32 md0 = md5_words[0] | 1U;

    U32 sum = 0U;
    U32 acc = 0U;
    const U32 *p = words;
    int remaining = dword_count;

    if ((((static_cast<U32>(remaining) - 2U) >> 1) + 1U) != 0U)
    {
        U32 loops = ((static_cast<U32>(remaining) - 2U) >> 1) + 1U;
        while (loops-- != 0U)
        {
            const U32 x = p[0] + acc;
            remaining -= 2;
            p += 2;

            const U32 t = LowMul(x, md0);
            const U32 a = LowMul(t, 0xB1110000U) - LowMul(t >> 16, 0x30674EEFU);
            const U32 b = LowMul(a, seed2);
            U32 c = b - LowMul(a >> 16, 0x78F7A461U);
            c = LowMul(c >> 16, seed0) - LowMul(a >> 16, 0x164D0000U) - LowMul(b, 0x46930000U);
            const U32 d = LowMul(c >> 16, 0x257E1D83U) + LowMul(c, 0x1D830000U);

            const U32 e = p[-1] + d;
            const U32 f = LowMul(e, md1);
            const U32 g = LowMul(f, 0x16F50000U) - LowMul(f >> 16, 0x5D8BE90BU);
            const U32 h = LowMul(g >> 16, seed3);
            const U32 i = LowMul(g, 0x69010000U) + h;
            const U32 j = LowMul(i, seed1);
            const U32 k = ((0U - h) - LowMul(g, 0x69010000U)) >> 16;
            const U32 m = LowMul(k, 0x7C932B89U) - j;
            U32 n = LowMul(k, 0xF2310000U) - LowMul(m >> 16, 0x405B6097U);
            n += LowMul(j, 0x60970000U);

            acc = n;
            sum += d + acc;
        }
    }

    if (remaining == 1)
    {
        const U32 x = p[0] + acc;
        const U32 t = LowMul(x, md0);
        const U32 a = LowMul(t, 0xB1110000U) - LowMul(t >> 16, 0x30674EEFU);
        const U32 b = LowMul(a, seed2);
        const U32 c = LowMul(a >> 16, 0xE9B30000U) - LowMul(b, 0x46930000U) + LowMul((b - LowMul(a >> 16, 0x78F7A461U)) >> 16, seed0);
        const U32 d = LowMul(c >> 16, 0x257E1D83U) + LowMul(c, 0x1D830000U);

        const U32 e = LowMul(d, md1);
        const U32 f = LowMul(e, 0x16F50000U) - LowMul(e >> 16, 0x5D8BE90BU);
        const U32 h = LowMul(f >> 16, seed3);
        const U32 i = LowMul(f, 0x69010000U) + h;
        const U32 j = LowMul(i, seed1);
        const U32 k = ((0U - h) - LowMul(f, 0x69010000U)) >> 16;
        const U32 m = LowMul(k, 0x7C932B89U) - j;
        U32 n = LowMul(k, 0xF2310000U) - LowMul(m >> 16, 0x405B6097U);
        n += LowMul(j, 0x60970000U);

        acc = n;
        sum += d + acc;
    }

    out_pair[0] = acc;
    out_pair[1] = sum;
    return true;
}

bool ComputeHash(const std::wstring &canonical_input,
                 const WorkingSeeds &seeds,
                 bool lowercase_output,
                 std::wstring *out_hash,
                 DebugData *debug_data)
{
    if (out_hash == NULL || canonical_input.size() > (std::numeric_limits<U32>::max)())
    {
        return false;
    }

    const Tables &lookup_tables = GetTables();
    const std::wstring lowered = ToLowerWide(canonical_input);
    // wchar_t and unsigned short are distinct C++ types, even on Windows.
    // Own the UTF-16 code units instead of relying on strict-aliasing violations.
    std::vector<U16> input_words(lowered.begin(), lowered.end());
    input_words.push_back(0U);
    const U16 *input = input_words.data();
    const size_t char_count = lowered.size();

    EncoderState state;
    U32 first_pass_chars = 0U;
    if (!FirstPass(state, lookup_tables, input, char_count, &first_pass_chars))
        return false;
    // The imported handoff uses the accumulated token lengths, not the first
    // pass's final cursor. Preserve that distinction when handling rewinds.
    SecondPass(state, lookup_tables, input + first_pass_chars, char_count - first_pass_chars);
    state.entries[0] = static_cast<U64>(state.total_units) | (state.count << 32);

    // The first entry carries encoder metadata. Flatten entries low-to-high,
    // omitting zero 16-bit chunks, then append one terminator. Keeping zeros or
    // replacing this with a raw entry dump would change the MD5 input.
    std::vector<U16> packed;
    for (U64 idx = 0ULL; idx < state.count; ++idx)
    {
        const U64 value = state.entries[static_cast<size_t>(idx)];
        for (int shift = 0; shift < 64; shift += 16)
        {
            const U16 part = static_cast<U16>(value >> shift);
            if (part != 0U)
            {
                packed.push_back(part);
            }
        }
    }
    packed.push_back(0U);
    // The mixing routines use signed byte counts; check before narrowing.
    if (packed.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) / sizeof(U16))
        return false;
    if (debug_data != NULL)
    {
        debug_data->packed_words = packed;
    }

    U32 md5_words[4] = { 0U, 0U, 0U, 0U };
    if (!ComputeMd5Words(reinterpret_cast<const U8 *>(&packed[0]),
                         static_cast<U32>(packed.size() * sizeof(U16)),
                         md5_words))
    {
        return false;
    }
    if (debug_data != NULL)
    {
        memcpy(debug_data->md5_words, md5_words, sizeof(md5_words));
    }

    const int byte_len = static_cast<int>(packed.size() * sizeof(U16));
    const int dword_len = byte_len >> 2;
    // MD5 covers the entire packed stream, including its terminator; the mixers
    // cover only complete 8-byte blocks. Zero-filled DWORD assembly below must
    // not extend either hash input or the mixed prefix.
    const int mix_count = ((byte_len & 4) == 0) ? dword_len : (dword_len - 1);
    std::vector<U32> packed_dwords((packed.size() + 1U) / 2U, 0U);
    for (size_t i = 0; i < packed_dwords.size(); ++i)
    {
        const U32 lo = packed[i * 2U];
        const U32 hi = (i * 2U + 1U < packed.size()) ? packed[i * 2U + 1U] : 0U;
        packed_dwords[i] = lo | (hi << 16);
    }

    U32 pair_a[2] = { 0U, 0U };
    U32 pair_b[2] = { 0U, 0U };
    if (mix_count >= 2)
    {
        MixA(&packed_dwords[0], mix_count, md5_words, seeds.a, pair_a);
        MixB(&packed_dwords[0], mix_count, md5_words, seeds.b, pair_b);
    }
    if (debug_data != NULL)
    {
        debug_data->pair_a[0] = pair_a[0];
        debug_data->pair_a[1] = pair_a[1];
        debug_data->pair_b[0] = pair_b[0];
        debug_data->pair_b[1] = pair_b[1];
    }

    BYTE final8[8];
    const U32 low = pair_a[0] ^ pair_b[0];
    const U32 high = pair_a[1] ^ pair_b[1];
    final8[0] = static_cast<BYTE>(low & 0xFFU);
    final8[1] = static_cast<BYTE>((low >> 8) & 0xFFU);
    final8[2] = static_cast<BYTE>((low >> 16) & 0xFFU);
    final8[3] = static_cast<BYTE>((low >> 24) & 0xFFU);
    final8[4] = static_cast<BYTE>(high & 0xFFU);
    final8[5] = static_cast<BYTE>((high >> 8) & 0xFFU);
    final8[6] = static_cast<BYTE>((high >> 16) & 0xFFU);
    final8[7] = static_cast<BYTE>((high >> 24) & 0xFFU);

    std::wstring hash;
    if (!Base64NoCrLf(final8, sizeof(final8), &hash))
    {
        return false;
    }
    if (lowercase_output)
    {
        hash = ToLowerWide(hash);
    }

    *out_hash = hash;
    return true;
}
} // namespace UserChoiceLatestHash
