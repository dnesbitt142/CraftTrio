/* The Warp EXEPACK:2 page stream uses literals, byte runs, and LZ matches.
 * Formats documented by the lxLite implementation of UnpackMethod2.
 * Each page is coded independently, and the LX loader fills the page tail.
 */
#include <stdint.h>
#include <string.h>

static unsigned min(unsigned a, unsigned b)
{
    return a < b ? a : b;
}

static unsigned hash(const uint8_t *p)
{
    return ((p[0] * 251u + p[1]) * 251u + p[2]) & 4095;
}

static void register_positions(const uint8_t *src, unsigned size, unsigned pos,
                               unsigned length, int *head, int *previous)
{
    for (; length && pos + 2 < size; --length, ++pos) {
        unsigned bucket = hash(src + pos);
        previous[pos] = head[bucket];
        head[bucket] = (int)pos;
    }
}

static unsigned put_literals(const uint8_t *src, uint8_t *dst, unsigned count)
{
    uint8_t *out = dst;
    while (count) {
        unsigned chunk = min(count, 63);
        *out++ = chunk << 2;
        memcpy(out, src, chunk);
        out += chunk;
        src += chunk;
        count -= chunk;
    }
    return out - dst;
}

/* dst must have room for 2 * size + 2. Returns zero if packing is not useful. */
unsigned lx_pack_page(const uint8_t *src, unsigned size, uint8_t *dst)
{
    int head[4096], previous[4096];
    uint8_t *out = dst;
    unsigned pos = 0, literal = 0;
    memset(head, 0xff, sizeof(head));
    while (pos + 2 < size) {
        unsigned length = 0, distance = 0, run = 1;
        unsigned limit = min(size - pos, 63);
        unsigned attempts = 0;
        int candidate = head[hash(src + pos)];
        while (candidate >= 0 && attempts++ < 64) {
            unsigned n = 0;
            while (n < limit && src[pos + n] == src[candidate + n]) ++n;
            if (n > length) {
                length = n;
                distance = pos - candidate;
                if (n == limit) break;
            }
            candidate = previous[candidate];
        }
        while (run < min(size - pos, 255) && src[pos + run] == src[pos]) ++run;
        if (run >= 4 && run >= length) {
            out += put_literals(src + literal, out, pos - literal);
            *out++ = 0;
            *out++ = run;
            *out++ = src[pos];
            length = run;
        } else if (length >= 3) {
            unsigned prefix, header, bytes;
            if (length <= 10 && distance < 512) {
                prefix = min(pos - literal, 3);
                header = 1 | prefix << 2 | (length - 3) << 4 | distance << 7;
                bytes = 2;
            } else if (length <= 6) {
                prefix = 0;
                header = 2 | (length - 3) << 2 | distance << 4;
                bytes = 2;
            } else {
                prefix = min(pos - literal, 15);
                header = 3 | prefix << 2 | length << 6 | distance << 12;
                bytes = 3;
            }
            out += put_literals(src + literal, out, pos - literal - prefix);
            *out++ = header;
            *out++ = header >> 8;
            if (bytes == 3) *out++ = header >> 16;
            memcpy(out, src + pos - prefix, prefix);
            out += prefix;
        } else {
            register_positions(src, size, pos, 1, head, previous);
            ++pos;
            continue;
        }
        register_positions(src, size, pos, length, head, previous);
        pos += length;
        literal = pos;
    }
    out += put_literals(src + literal, out, size - literal);
    *out++ = 0;
    *out++ = 0;
    return out - dst < size && out - dst < 4092 ? out - dst : 0;
}

int lx_verify_page(const uint8_t *compressed, unsigned packed_size,
                   const uint8_t *original, unsigned original_size)
{
    uint8_t expanded[4096];
    unsigned in = 0, out = 0;
    while (in < packed_size) {
        unsigned command = compressed[in++];
        unsigned length, prefix = 0, distance = 0;
        if (!command) {
            if (in == packed_size) return 0;
            length = compressed[in++];
            if (!length)
                return in == packed_size && out == original_size &&
                       !memcmp(expanded, original, original_size);
            if (in == packed_size || length > original_size - out) return 0;
            memset(expanded + out, compressed[in++], length);
            out += length;
            continue;
        }
        if (!(command & 3)) {
            prefix = command >> 2;
            length = 0;
        } else {
            if (in == packed_size) return 0;
            command |= (unsigned)compressed[in++] << 8;
            switch (command & 3) {
            case 1:
                length = ((command >> 4) & 7) + 3;
                prefix = (command >> 2) & 3;
                distance = command >> 7;
                break;
            case 2:
                length = ((command >> 2) & 3) + 3;
                distance = command >> 4;
                break;
            default:
                if (in == packed_size) return 0;
                command |= (unsigned)compressed[in++] << 16;
                length = (command >> 6) & 63;
                prefix = (command >> 2) & 15;
                distance = command >> 12;
            }
        }
        if (prefix > packed_size - in || prefix + length > original_size - out)
            return 0;
        memcpy(expanded + out, compressed + in, prefix);
        in += prefix;
        out += prefix;
        if (length && (!distance || distance > out)) return 0;
        while (length--) {
            expanded[out] = expanded[out - distance];
            ++out;
        }
    }
    return 0;
}
