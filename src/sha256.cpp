#include "sha256.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace fpkgi {
namespace {

struct Sha256Context {
    uint8_t data[64];
    uint32_t data_len;
    uint64_t bit_len;
    uint32_t state[8];
};

const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void transform(Sha256Context& ctx, const uint8_t data[]) {
    uint32_t m[64];
    for (uint32_t i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (static_cast<uint32_t>(data[j]) << 24) | (static_cast<uint32_t>(data[j+1]) << 16) |
               (static_cast<uint32_t>(data[j+2]) << 8) | static_cast<uint32_t>(data[j+3]);
    for (uint32_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(m[i-15], 7) ^ rotr(m[i-15], 18) ^ (m[i-15] >> 3);
        const uint32_t s1 = rotr(m[i-2], 17) ^ rotr(m[i-2], 19) ^ (m[i-2] >> 10);
        m[i] = m[i-16] + s0 + m[i-7] + s1;
    }
    uint32_t a=ctx.state[0], b=ctx.state[1], c=ctx.state[2], d=ctx.state[3];
    uint32_t e=ctx.state[4], f=ctx.state[5], g=ctx.state[6], h=ctx.state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + s1 + ch + k[i] + m[i];
        const uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx.state[0]+=a; ctx.state[1]+=b; ctx.state[2]+=c; ctx.state[3]+=d;
    ctx.state[4]+=e; ctx.state[5]+=f; ctx.state[6]+=g; ctx.state[7]+=h;
}

void init(Sha256Context& ctx) {
    ctx.data_len = 0; ctx.bit_len = 0;
    ctx.state[0]=0x6a09e667; ctx.state[1]=0xbb67ae85; ctx.state[2]=0x3c6ef372; ctx.state[3]=0xa54ff53a;
    ctx.state[4]=0x510e527f; ctx.state[5]=0x9b05688c; ctx.state[6]=0x1f83d9ab; ctx.state[7]=0x5be0cd19;
}

void update(Sha256Context& ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        ctx.data[ctx.data_len++] = data[i];
        if (ctx.data_len == 64) { transform(ctx, ctx.data); ctx.bit_len += 512; ctx.data_len = 0; }
    }
}

void final(Sha256Context& ctx, uint8_t hash[32]) {
    uint32_t i = ctx.data_len;
    ctx.data[i++] = 0x80;
    if (i > 56) { while (i < 64) ctx.data[i++] = 0; transform(ctx, ctx.data); i = 0; }
    while (i < 56) ctx.data[i++] = 0;
    ctx.bit_len += static_cast<uint64_t>(ctx.data_len) * 8;
    for (int shift = 56; shift >= 0; shift -= 8) ctx.data[i++] = static_cast<uint8_t>(ctx.bit_len >> shift);
    transform(ctx, ctx.data);
    for (int j = 0; j < 8; ++j) {
        for (int byte = 0; byte < 4; ++byte) {
            hash[j * 4 + byte] = static_cast<uint8_t>((ctx.state[j] >> (24 - byte * 8)) & 0xff);
        }
    }
}

} // namespace

bool sha256_stream(FILE* file, std::string& digest, std::string& error) {
    if (!file) {
        error = "cannot hash a null package stream";
        return false;
    }
    Sha256Context ctx;
    init(ctx);
    uint8_t buffer[64 * 1024];
    for (;;) {
        const size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count) update(ctx, buffer, count);
        if (count < sizeof(buffer)) {
            if (ferror(file)) {
                error = "failed while hashing downloaded file";
                return false;
            }
            break;
        }
    }
    uint8_t hash[32];
    final(ctx, hash);
    static const char hex[] = "0123456789abcdef";
    digest.assign(64, '0');
    for (int i = 0; i < 32; ++i) {
        digest[i * 2] = hex[hash[i] >> 4];
        digest[i * 2 + 1] = hex[hash[i] & 0x0f];
    }
    return true;
}

} // namespace fpkgi
