#include "rsx_vertex_compact.h"
#include "rsx_commands.h"
#include "rsx_vertex_formats.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static float rsx_compact_be_f32(const u8* p)
{
    const u32 w = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
                  ((u32)p[2] << 8) | (u32)p[3];
    float f;
    memcpy(&f, &w, sizeof(f));
    return f;
}

static float rsx_compact_be_f16(const u8* p)
{
    const u16 h = (u16)((p[0] << 8) | p[1]);
    const u32 sign = (u32)(h >> 15) << 31;
    const u32 exp = (h >> 10) & 0x1F;
    const u32 man = h & 0x3FF;
    u32 out;
    if (exp == 0)
        out = sign;
    else if (exp == 31)
        out = sign | 0x7F800000u | (man << 13);
    else
        out = sign | ((exp + 112) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof(f));
    return f;
}

int rsx_vertex_decode_element(
    u32 type, u32 size, const u8* source, float out[4])
{
    if (!source || !out)
        return 0;
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (u32 component = 0; component < size && component < 4;
         component++) {
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            out[component] = rsx_compact_be_f32(source + component * 4);
            break;
        case RSX_VTX_TYPE_HALF:
            out[component] = rsx_compact_be_f16(source + component * 2);
            break;
        case RSX_VTX_TYPE_UNORM8:
            out[component] = source[component] / 255.0f;
            break;
        case RSX_VTX_TYPE_UINT8:
            out[component] = (float)source[component];
            break;
        case RSX_VTX_TYPE_SNORM16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = value / 32767.0f;
            break;
        }
        case RSX_VTX_TYPE_SINT16: {
            const s16 value = (s16)(
                (source[component * 2] << 8) | source[component * 2 + 1]);
            out[component] = (float)value;
            break;
        }
        case RSX_VTX_TYPE_CMP32: {
            const u32 word =
                ((u32)source[0] << 24) | ((u32)source[1] << 16) |
                ((u32)source[2] << 8) | (u32)source[3];
            s32 x = (s32)(word & 0x7FF);
            s32 y = (s32)((word >> 11) & 0x7FF);
            s32 z = (s32)((word >> 22) & 0x3FF);
            if (x & 0x400) x -= 0x800;
            if (y & 0x400) y -= 0x800;
            if (z & 0x200) z -= 0x400;
            out[0] = (float)x / 1023.0f;
            out[1] = (float)y / 1023.0f;
            out[2] = (float)z / 511.0f;
            out[3] = 1.0f;
            return 1;
        }
        default:
            return 0;
        }
    }
    return 1;
}

void rsx_vertex_layout_plan_init(
    rsx_vertex_layout_plan* plan, u32 input_mask)
{
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->mask = input_mask & 0xFFFFu;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++)
        plan->slot_by_attr[attr] = -1;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        if (!(plan->mask & (1u << attr)))
            continue;
        const u32 slot = plan->count++;
        plan->attrs[slot] = (u8)attr;
        plan->slot_by_attr[attr] = (s8)slot;
    }
    plan->stride = (u32)plan->count * 16u;
}

void rsx_vertex_fetch_plan_init(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const rsx_vertex_layout_plan* layout,
    rsx_vertex_guest_ptr_fn guest_ptr, void* guest_user)
{
    if (!plan || !rsx || !layout)
        return;
    memset(plan, 0, sizeof(*plan));
    plan->layout = *layout;
    plan->base_offset = rsx_dsp_vertex_data_base_offset(rsx);
    plan->divider_mask = rsx_dsp_reg(rsx, 0x1FC0);
    plan->guest_ptr = guest_ptr;
    plan->guest_user = guest_user;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        rsx_dsp_get_vertex_attr(rsx, attr, &fetch->desc);
        fetch->elem_size =
            rsx_vertex_attrib_size(fetch->desc.type, fetch->desc.size);
        fetch->stride =
            fetch->desc.stride ? fetch->desc.stride : fetch->elem_size;
        rsx_dsp_vertex_default(rsx, attr, fetch->default_value);
        if (attr == 3 &&
            fetch->default_value[0] == 0.0f &&
            fetch->default_value[1] == 0.0f &&
            fetch->default_value[2] == 0.0f &&
            fetch->default_value[3] == 1.0f) {
            fetch->default_value[0] = 1.0f;
            fetch->default_value[1] = 1.0f;
            fetch->default_value[2] = 1.0f;
        }
    }
}

u32 rsx_vertex_inline_layout(
    const rsx_dispatch* rsx, u32 offsets[RSX_DSP_NUM_VERTEX_ATTR])
{
    u32 packed = 0, declared = 0;
    for (u32 attr = 0; attr < RSX_DSP_NUM_VERTEX_ATTR; attr++) {
        rsx_dsp_vertex_attr desc;
        rsx_dsp_get_vertex_attr(rsx, attr, &desc);
        if (offsets)
            offsets[attr] = packed;
        if (!desc.type || !desc.size)
            continue;
        packed += rsx_vertex_attrib_size(desc.type, desc.size);
        if (desc.stride > declared)
            declared = desc.stride;
    }
    return declared ? declared : packed;
}

void rsx_vertex_fetch_plan_set_inline(
    rsx_vertex_fetch_plan* plan, const rsx_dispatch* rsx,
    const u8* data, u32 bytes)
{
    if (!plan)
        return;
    plan->inline_data = NULL;
    plan->inline_bytes = 0;
    if (!data || !bytes || !rsx)
        return;
    plan->inline_stride = rsx_vertex_inline_layout(rsx, plan->inline_off);
    plan->inline_data = data;
    plan->inline_bytes = bytes;
}

void rsx_vertex_fetch_plan_prepare(
    rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count)
{
    if (!plan)
        return;
    /* Ordinary per-vertex attributes (rsx_vertex_freq_plain) all span the same
     * elements: find them once for the draw instead of once per attribute.
     * The per-attribute walk was 2.2 ms of the RSX thread a frame in
     * inFAMOUS 2's New Marais. */
    int plain_known = 0;
    u32 plain_first = UINT_MAX, plain_last = 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        fetch->stable_base = NULL;
        fetch->stable_first = 0;
        fetch->stable_last = 0;
        if (!refs || !count ||
            !fetch->desc.type || !fetch->desc.size || !fetch->elem_size)
            continue;

        /* INLINE_ARRAY: the stream is already one contiguous, correctly
         * strided array per attribute, which is exactly what stable_base
         * describes -- so it needs no resolver and no bounds fallback, only
         * the right base. */
        if (plan->inline_data) {
            const u32 stride =
                plan->inline_stride ? plan->inline_stride : fetch->stride;
            const u32 off = plan->inline_off[attr];
            if (!stride || off + fetch->elem_size > stride)
                continue;
            const u32 verts = plan->inline_bytes / stride;
            if (!verts)
                continue;
            fetch->stride = stride;
            fetch->stable_base = plan->inline_data + off;
            fetch->stable_first = 0;
            fetch->stable_last = verts - 1;
            continue;
        }

        if (!plan->guest_ptr)
            continue;

        u32 first = UINT_MAX;
        u32 last = 0;
        if (rsx_vertex_freq_plain(fetch->desc.frequency,
                                  (plan->divider_mask >> attr) & 1u)) {
            if (!plain_known) {
                plain_known = 1;
                for (u32 i = 0; i < count; i++) {
                    const u32 element = rsx_vertex_element_index(
                        refs[i].vertex_id, refs[i].base_index, 0, 0);
                    if (element < plain_first) plain_first = element;
                    if (element > plain_last) plain_last = element;
                }
            }
            first = plain_first;
            last = plain_last;
        } else for (u32 i = 0; i < count; i++) {
            const u32 element = rsx_vertex_element_index(
                refs[i].vertex_id, refs[i].base_index,
                fetch->desc.frequency,
                (plan->divider_mask >> attr) & 1u);
            if (element < first) first = element;
            if (element > last) last = element;
        }

        const u64 prefix = rsx_dsp_resolve_vertex_offset(
            plan->base_offset, fetch->desc.offset);
        const u64 start = prefix + (u64)first * fetch->stride;
        const u64 end =
            prefix + (u64)last * fetch->stride + fetch->elem_size;
        if (start > UINT_MAX || end > 0x100000000ull || end <= start ||
            end - start > UINT_MAX)
            continue;

        const u8* base = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, (u32)start,
            (u32)(end - start));
        if (!base)
            continue;
        fetch->stable_base = base;
        fetch->stable_first = first;
        fetch->stable_last = last;
    }
}

/* One attribute of one vertex, exactly as rsx_vertex_fetch_one does it. */
static int rsx_vertex_fetch_attr_value(
    const rsx_vertex_fetch_plan* plan, u32 attr,
    const rsx_vertex_ref* ref, float* value)
{
    const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
    if (!fetch->desc.type || !fetch->desc.size) {
        memcpy(value, fetch->default_value, 16);
        return 1;
    }

    const u32 element = rsx_vertex_element_index(
        ref->vertex_id, ref->base_index, fetch->desc.frequency,
        (plan->divider_mask >> attr) & 1u);
    const u8* source = NULL;
    if (fetch->stable_base &&
        element >= fetch->stable_first && element <= fetch->stable_last) {
        source = fetch->stable_base +
            (u64)(element - fetch->stable_first) * fetch->stride;
    } else if (plan->inline_data) {
        /* An inline draw has no vertex array to fall back to: the array
         * offsets still hold whatever the last ordinary draw bound, and
         * reading them would feed this draw another mesh's vertices.
         * An attribute the stream does not supply gets its default,
         * which is what the hardware feeds. */
        source = NULL;
    } else if (plan->guest_ptr) {
        const u32 offset =
            rsx_dsp_resolve_vertex_offset(plan->base_offset,
                                          fetch->desc.offset) +
            element * fetch->stride;
        source = plan->guest_ptr(
            plan->guest_user, fetch->desc.location, offset,
            fetch->elem_size);
    }

    if (source && rsx_vertex_decode_element(
            fetch->desc.type, fetch->desc.size, source, value))
        return 1;
    if (attr == 0)
        return 0;
    memcpy(value, fetch->default_value, 16);
    return 1;
}

int rsx_vertex_fetch_one(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* ref, u8* out)
{
    if (!plan || !ref || (!out && plan->layout.stride))
        return 0;
    for (u32 slot = 0; slot < plan->layout.count; slot++)
        if (!rsx_vertex_fetch_attr_value(plan, plan->layout.attrs[slot], ref,
                                   (float*)(out + slot * 16u)))
            return 0;
    return 1;
}

/* The stable-range loop for one attribute and one vertex type: `decode`
 * reads `src` into `v` (pre-set to 0,0,0,1) for components c < n. Elements
 * outside the stable range take the one-vertex path. */
#define RSX_FETCH_LOOP(decode)                                                  \
    for (u32 i = 0; i < count; i++) {                                           \
        float* v = (float*)(out + (size_t)i * stride + slot * 16u);             \
        const u32 element = plain                                               \
            ? ((refs[i].vertex_id + refs[i].base_index) & 0x000FFFFFu)          \
            : rsx_vertex_element_index(refs[i].vertex_id, refs[i].base_index,   \
                                       fetch->desc.frequency, modulo);          \
        if (element - first > span) {                                          \
            if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i], v)) return 0;      \
            continue;                                                           \
        }                                                                       \
        const u8* src = base + (u64)(element - first) * fstride;                \
        v[0] = v[1] = v[2] = 0.0f; v[3] = 1.0f;                                 \
        for (u32 c = 0; c < n; c++) { decode; }                                 \
    }

int rsx_vertex_fetch_many(
    const rsx_vertex_fetch_plan* plan, const rsx_vertex_ref* refs, u32 count,
    u8* out)
{
    if (!plan || (!refs && count) || (!out && plan->layout.stride && count))
        return 0;
    const u32 stride = plan->layout.stride;
    for (u32 slot = 0; slot < plan->layout.count; slot++) {
        const u32 attr = plan->layout.attrs[slot];
        const rsx_vertex_fetch_attr* fetch = &plan->attr[attr];
        const u8* base = fetch->stable_base;
        const u32 type = fetch->desc.type;
        if (!type || !fetch->desc.size || !base ||
            type < RSX_VTX_TYPE_SNORM16 || type > RSX_VTX_TYPE_UINT8) {
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            continue;
        }
        const u32 first = fetch->stable_first;
        const u32 span = fetch->stable_last - first;
        const u32 fstride = fetch->stride;
        const u32 modulo = (plan->divider_mask >> attr) & 1u;
        const int plain = rsx_vertex_freq_plain(fetch->desc.frequency, modulo);
        const u32 n = fetch->desc.size < 4 ? fetch->desc.size : 4;
        switch (type) {
        case RSX_VTX_TYPE_FLOAT:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f32(src + c * 4))
            break;
        case RSX_VTX_TYPE_HALF:
            RSX_FETCH_LOOP(v[c] = rsx_compact_be_f16(src + c * 2))
            break;
        case RSX_VTX_TYPE_UNORM8:
            RSX_FETCH_LOOP(v[c] = src[c] / 255.0f)
            break;
        case RSX_VTX_TYPE_UINT8:
            RSX_FETCH_LOOP(v[c] = (float)src[c])
            break;
        case RSX_VTX_TYPE_SNORM16:
            RSX_FETCH_LOOP(v[c] = (s16)((src[c * 2] << 8) | src[c * 2 + 1]) / 32767.0f)
            break;
        case RSX_VTX_TYPE_SINT16:
            RSX_FETCH_LOOP(v[c] = (float)(s16)((src[c * 2] << 8) | src[c * 2 + 1]))
            break;
        default:   /* RSX_VTX_TYPE_CMP32: one packed word, whatever the size */
            for (u32 i = 0; i < count; i++)
                if (!rsx_vertex_fetch_attr_value(plan, attr, &refs[i],
                        (float*)(out + (size_t)i * stride + slot * 16u)))
                    return 0;
            break;
        }
    }
    return 1;
}
#undef RSX_FETCH_LOOP

static u64 rsx_vertex_remap_hash(u64 value)
{
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

/* AVX2 bulk path for rsx_vertex_hash_bytes: eight 64-bit lanes over
 * 64-byte stripes (the XXH3 accumulate step: (d^k).lo32 * (d^k).hi32 plus the
 * lane-swapped input), scrambled every 1 KB, then folded into the scalar
 * hash's state. About 4x the scalar loop's throughput; the vertex cache hashes
 * every draw's vertex bytes each frame. Callers need not care which path ran:
 * one process always takes the same one. */
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__clang__) || defined(__GNUC__))
/* clang-cl's <immintrin.h> leaves out the AVX/AVX2 declarations unless the
 * whole file is built for AVX2. Only the target("avx2") function below uses
 * them, behind a CPUID check, so ask for the declarations alone. */
#if defined(_MSC_VER) && !defined(__AVX__)
#define __AVX__ 1
#define RSX_HASH_UNDEF_AVX 1
#endif
#if defined(_MSC_VER) && !defined(__AVX2__)
#define __AVX2__ 1
#define RSX_HASH_UNDEF_AVX2 1
#endif
#include <immintrin.h>
#ifdef RSX_HASH_UNDEF_AVX
#undef __AVX__
#undef RSX_HASH_UNDEF_AVX
#endif
#ifdef RSX_HASH_UNDEF_AVX2
#undef __AVX2__
#undef RSX_HASH_UNDEF_AVX2
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#define RSX_HASH_AVX2 1
static int rsx_hash_have_avx2(void)
{
    static int have = -1;
    if (have < 0) {
        int r[4] = { 0, 0, 0, 0 };
#if defined(_MSC_VER)
        __cpuidex(r, 7, 0);
#else
        unsigned a, b, c, d;
        __cpuid_count(7, 0, a, b, c, d);
        r[1] = (int)b;
#endif
        have = (r[1] >> 5) & 1;
    }
    return have;
}
__attribute__((target("avx2")))
static u64 rsx_hash_avx2_bulk(const u8* p, size_t blocks64, u64 seed)
{
    static const u64 K[8] = {
        0xBE4BA423396CFEB8ull, 0x1CAD21F72C81017Cull, 0xDB979083E96DD4DEull, 0x1F67B3B7A4A44072ull,
        0x78E5C0CC4EE679CBull, 0x2172FFCC7DD05A82ull, 0x8E2443F7744608B8ull, 0x4C263A81E69035E0ull };
    __m256i acc0 = _mm256_set_epi64x((long long)(seed ^ 0x9E3779B185EBCA87ull), (long long)(seed + 0xC2B2AE3D27D4EB4Full),
                                     (long long)(seed ^ 0x165667B19E3779F9ull), (long long)(seed - 0x85EBCA77C2B2AE63ull));
    __m256i acc1 = _mm256_set_epi64x((long long)(seed + 0x27D4EB2F165667C5ull), (long long)(seed ^ 0xFF51AFD7ED558CCDull),
                                     (long long)(seed + 0xC4CEB9FE1A85EC53ull), (long long)(seed ^ 0x9FB21C651E98DF25ull));
    const __m256i k0 = _mm256_loadu_si256((const __m256i*)&K[0]);
    const __m256i k1 = _mm256_loadu_si256((const __m256i*)&K[4]);
    const __m256i prime = _mm256_set1_epi32((int)0x9E3779B1u);
    for (size_t b = 0; b < blocks64; b++) {
        const __m256i d0 = _mm256_loadu_si256((const __m256i*)(p + b * 64));
        const __m256i d1 = _mm256_loadu_si256((const __m256i*)(p + b * 64 + 32));
        const __m256i x0 = _mm256_xor_si256(d0, k0);
        const __m256i x1 = _mm256_xor_si256(d1, k1);
        acc0 = _mm256_add_epi64(acc0, _mm256_mul_epu32(x0, _mm256_srli_epi64(x0, 32)));
        acc1 = _mm256_add_epi64(acc1, _mm256_mul_epu32(x1, _mm256_srli_epi64(x1, 32)));
        acc0 = _mm256_add_epi64(acc0, _mm256_shuffle_epi32(d0, _MM_SHUFFLE(1, 0, 3, 2)));
        acc1 = _mm256_add_epi64(acc1, _mm256_shuffle_epi32(d1, _MM_SHUFFLE(1, 0, 3, 2)));
        if ((b & 15) == 15) {   /* scramble every 1 KB, as XXH3 does per block */
            acc0 = _mm256_xor_si256(_mm256_xor_si256(acc0, _mm256_srli_epi64(acc0, 47)), k1);
            acc1 = _mm256_xor_si256(_mm256_xor_si256(acc1, _mm256_srli_epi64(acc1, 47)), k0);
            acc0 = _mm256_add_epi64(_mm256_mul_epu32(acc0, prime),
                                    _mm256_slli_epi64(_mm256_mul_epu32(_mm256_srli_epi64(acc0, 32), prime), 32));
            acc1 = _mm256_add_epi64(_mm256_mul_epu32(acc1, prime),
                                    _mm256_slli_epi64(_mm256_mul_epu32(_mm256_srli_epi64(acc1, 32), prime), 32));
        }
    }
    u64 l[8];
    _mm256_storeu_si256((__m256i*)&l[0], acc0);
    _mm256_storeu_si256((__m256i*)&l[4], acc1);
    u64 h = seed ^ ((u64)blocks64 * 0x9E3779B185EBCA87ull);
    for (int i = 0; i < 8; i++) {
        h ^= (l[i] ^ K[i]) * 0xC2B2AE3D27D4EB4Full;
        h = (h << 27 | h >> 37) * 0x9E3779B185EBCA87ull + 0x85EBCA77C2B2AE63ull;
    }
    return h;
}
#endif

static u64 rsx_vertex_hash_scalar(const void* data, size_t bytes, u64 seed);

u64 rsx_vertex_hash_bytes(const void* data, size_t bytes, u64 seed)
{
#ifdef RSX_HASH_AVX2
    if (bytes >= 256 && rsx_hash_have_avx2()) {
        const size_t blocks = bytes / 64;
        const u64 h = rsx_hash_avx2_bulk((const u8*)data, blocks, seed);
        /* the tail, and the length, through the scalar path */
        return rsx_vertex_hash_scalar((const u8*)data + blocks * 64, bytes - blocks * 64,
                                      h ^ ((u64)bytes * 0x165667B19E3779F9ull));
    }
#endif
    return rsx_vertex_hash_scalar(data, bytes, seed);
}

static u64 rsx_vertex_hash_scalar(const void* data, size_t bytes, u64 seed)
{
    /* Four independent multiply-xorshift lanes over 32-byte blocks, then a
     * tail and a final avalanche (the xxh64/murmur3 finaliser). */
    const u8* p = (const u8*)data;
    const u64 K1 = 0x9E3779B185EBCA87ull, K2 = 0xC2B2AE3D27D4EB4Full;
    u64 a = seed ^ K1, b = seed + K2, c = seed ^ 0x165667B19E3779F9ull,
        d = seed - K1;
    size_t n = bytes;
    while (n >= 32) {
        u64 w[4];
        memcpy(w, p, 32);
        a = (a ^ (w[0] * K2)) * K1; a ^= a >> 29;
        b = (b ^ (w[1] * K2)) * K1; b ^= b >> 29;
        c = (c ^ (w[2] * K2)) * K1; c ^= c >> 29;
        d = (d ^ (w[3] * K2)) * K1; d ^= d >> 29;
        p += 32; n -= 32;
    }
    u64 h = (a ^ (b << 1) ^ (c << 2) ^ (d << 3)) + (u64)bytes * K2;
    while (n >= 8) {
        u64 w; memcpy(&w, p, 8);
        h = (h ^ (w * K2)) * K1; h ^= h >> 31;
        p += 8; n -= 8;
    }
    if (n) {
        u64 w = 0; memcpy(&w, p, n);
        h = (h ^ ((w + n) * K2)) * K1;
    }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

/* The direct-table pass: same result as the hashed one below (first-use
 * order, refs compacted in place, occurrence map), when every reference
 * shares refs[0].base_index and the largest id is `max_id`. */
static int rsx_vertex_remap_direct(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count, u32 max_id,
    u32* unique_count)
{
    if (max_id >= remap->direct_capacity) {
        u32 cap = remap->direct_capacity ? remap->direct_capacity : 4096u;
        while (cap <= max_id) cap *= 2u;
        u32 (*t)[2] = (u32 (*)[2])realloc(remap->direct, (size_t)cap * sizeof(*t));
        if (!t)
            return 0;
        memset(t, 0, (size_t)cap * sizeof(*t));
        remap->direct = t;
        remap->direct_capacity = cap;
        remap->direct_generation = 0;
    }
    if (++remap->direct_generation == 0) {
        memset(remap->direct, 0, (size_t)remap->direct_capacity * sizeof(*remap->direct));
        remap->direct_generation = 1;
    }
    const u32 gen = remap->direct_generation;
    u32 (*t)[2] = remap->direct;
    u32* occ = remap->occurrence_to_unique;
    u32 unique = 0;
    for (u32 i = 0; i < count; i++) {
        const u32 id = refs[i].vertex_id;
        if (t[id][0] != gen) {
            t[id][0] = gen;
            t[id][1] = unique;
            refs[unique] = refs[i];
            occ[i] = unique++;
        } else {
            occ[i] = t[id][1];
        }
    }
    *unique_count = unique;
    return 1;
}

int rsx_vertex_remap_build(
    rsx_vertex_remap* remap, rsx_vertex_ref* refs, u32 count,
    u32* unique_count)
{
    if (!remap || (!refs && count) || !unique_count)
        return 0;
    *unique_count = count;
    if (!count)
        return 1;

    if (count > remap->occurrence_capacity) {
        u32* occurrences = (u32*)realloc(
            remap->occurrence_to_unique, (size_t)count * sizeof(u32));
        if (!occurrences)
            return 0;
        remap->occurrence_to_unique = occurrences;
        remap->occurrence_capacity = count;
    }

    /* Direct table when it applies and stays small: ids under 1M and not
     * much sparser than the draw (a 16-bit index buffer always qualifies). */
    {
        const u32 base = refs[0].base_index;
        u32 max_id = 0, same = 1;
        for (u32 i = 0; i < count; i++) {
            const u32 id = refs[i].vertex_id;
            if (id > max_id) max_id = id;
            same &= refs[i].base_index == base;
        }
        if (same && max_id < (1u << 20) && max_id <= count * 8u + 65536u &&
            rsx_vertex_remap_direct(remap, refs, count, max_id, unique_count))
            return 1;
    }

    if (count > UINT_MAX / 2u)
        return 0;
    u32 slot_capacity = 16u;
    const u32 required_slots = count * 2u;
    while (slot_capacity < required_slots) {
        if (slot_capacity > UINT_MAX / 2u)
            return 0;
        slot_capacity *= 2u;
    }
    if (slot_capacity > remap->slot_capacity) {
        rsx_vertex_remap_slot* slots = (rsx_vertex_remap_slot*)realloc(
            remap->slots, (size_t)slot_capacity * sizeof(*slots));
        if (!slots)
            return 0;
        remap->slots = slots;
        remap->slot_capacity = slot_capacity;
        memset(remap->slots, 0,
               (size_t)remap->slot_capacity * sizeof(*remap->slots));
        remap->generation = 1u;
    } else {
        remap->generation++;
        if (!remap->generation) {
            memset(remap->slots, 0,
                   (size_t)remap->slot_capacity * sizeof(*remap->slots));
            remap->generation = 1u;
        }
    }

    u32 unique = 0;
    const u32 mask = remap->slot_capacity - 1u;
    for (u32 occurrence = 0; occurrence < count; occurrence++) {
        const rsx_vertex_ref ref = refs[occurrence];
        const u64 key = ((u64)ref.base_index << 32) | ref.vertex_id;
        u32 slot_index = (u32)rsx_vertex_remap_hash(key) & mask;
        for (;;) {
            rsx_vertex_remap_slot* slot = &remap->slots[slot_index];
            if (slot->generation != remap->generation) {
                slot->key = key;
                slot->value = unique;
                slot->generation = remap->generation;
                refs[unique] = ref;
                remap->occurrence_to_unique[occurrence] = unique;
                unique++;
                break;
            }
            if (slot->key == key) {
                remap->occurrence_to_unique[occurrence] = slot->value;
                break;
            }
            slot_index = (slot_index + 1u) & mask;
        }
    }
    *unique_count = unique;
    return 1;
}

u32 rsx_vertex_remap_index(
    const rsx_vertex_remap* remap, u32 occurrence)
{
    return remap && occurrence < remap->occurrence_capacity
        ? remap->occurrence_to_unique[occurrence] : occurrence;
}

void rsx_vertex_remap_destroy(rsx_vertex_remap* remap)
{
    if (!remap)
        return;
    free(remap->occurrence_to_unique);
    free(remap->slots);
    free(remap->direct);
    memset(remap, 0, sizeof(*remap));
}

int rsx_vertex_topology_plan(
    u32 primitive, int refs_remapped, int* indexed)
{
    if (!indexed)
        return 0;
    switch (primitive) {
    case RSX_PRIMITIVE_TRIANGLES:
        *indexed = refs_remapped != 0;
        return 1;
    case RSX_PRIMITIVE_TRIANGLE_STRIP:
    case RSX_PRIMITIVE_TRIANGLE_FAN:
    case RSX_PRIMITIVE_QUADS:
        *indexed = 1;
        return 1;
    default:
        *indexed = 0;
        return 0;
    }
}
