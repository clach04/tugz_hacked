// tugz 1.3.0: tiny unity gzip, a drop-in gzip for Windows
// Single-file amalgamation of the tugz sources. Build:
//   $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.
// Copies named gunzip.exe or zcat.exe decompress by default.

// tugz core: base types and arena allocator
//
// Everything is a unity build. Each program's entry file
// (platform/*_posix.c, platform/*_windows.c, platform/libtugz.c, and the
// test programs) includes the sources it needs, this one first. The
// hooks os_oom and os_extend come from the program's own layer
// (src/gzipio.c, src/zipcli.c) or from its entry file.

// The release, TUGZ_VERSION, as the programs report it, is set in tugz.h
// alone. (The program amalgamations keep only its version lines here.)
#define TUGZ_VERSION "1.3.0"
#define TUGZ_VERSION_MAJOR 1
#define TUGZ_VERSION_MINOR 3
#define TUGZ_VERSION_PATCH 0

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef   signed int        i32;
typedef unsigned int        u32;
typedef   signed long long  i64;
typedef unsigned long long  u64;
typedef   signed int        b32;
typedef __PTRDIFF_TYPE__    iz;
typedef __SIZE_TYPE__       uz;
typedef __UINTPTR_TYPE__    uptr;
typedef char                byte;

#define assert(c)       while (!(c)) __builtin_unreachable()
#define countof(a)      (iz)(sizeof(a) / sizeof(*(a)))
#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t), 1)
#define newbytes(a, n)  (u8 *)alloc(a, n, 1, 64, 0)  // buffers
#define newstr(a, n)    (u8 *)alloc(a, n, 1, 1, 0)   // strings, packed
#define S8(s)           {(u8 *)s, countof(s)-1}
#define S(s)            (s8)S8(s)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

typedef struct os os;

typedef struct {
    u8 *s;
    iz  len;
} s8;

// Allocates from beg up, or if down, from end down, so that a scratch
// arena may grow toward a permanent one in the same memory.
typedef struct {
    byte *beg;
    byte *end;
    os   *ctx;
    b32   down;
} arena;

// Status codes for compression and decompression.
enum {
    GZ_OK,
    GZ_TRAILING,  // warning: decompressed fine, but junk followed
    GZ_ENOTGZ,
    GZ_EMETHOD,
    GZ_EFLAGS,
    GZ_EHCRC,
    GZ_ETRUNC,
    GZ_EDATA,
    GZ_ECRC,
    GZ_ELEN,
    GZ_EREAD,
    GZ_EWRITE,
    GZ_EHEADER,   // invalid zlib header
    GZ_EUSAGE,    // streaming: input or flush after finishing
    GZ_NEEDIN,    // streaming: all input consumed
    GZ_NEEDOUT,   // streaming: output buffer full
};

// Streaming buffers, advanced as input is consumed and output produced.
typedef struct {
    u8 const *in;
    iz        inlen;
    u8       *out;
    iz        outlen;
} zbuf;

// Null pointers are permitted with zero lengths, as library callers may
// legitimately pass empty buffers that way.
static void *bytecopy(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return len ? __builtin_memcpy(dst, src, (uz)len) : dst;
}

static void *bytemove(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return len ? __builtin_memmove(dst, src, (uz)len) : dst;
}

static void *bytefill(void *dst, i32 c, iz len)
{
    assert(len >= 0);
    return __builtin_memset(dst, c, (uz)len);
}

// Explicit little-endian accesses; each compiles to a single unaligned
// load or store on little-endian targets. Always inlined: GCC would
// otherwise leave load64le a call inside inflate's and deflate's loops.
// store64le names each byte: GCC -O2 merges such stores into one, but
// left a loop over them as eight byte stores in deflate's bit writer.
[[gnu::always_inline]] static inline u64 load64le(u8 const *p)
{
    return (u64)p[0]     | (u64)p[1]<< 8 | (u64)p[2]<<16 | (u64)p[3]<<24 |
           (u64)p[4]<<32 | (u64)p[5]<<40 | (u64)p[6]<<48 | (u64)p[7]<<56;
}

[[gnu::always_inline]] static inline void store64le(u8 *p, u64 v)
{
    p[0] = (u8)(v      );
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
    p[4] = (u8)(v >> 32);
    p[5] = (u8)(v >> 40);
    p[6] = (u8)(v >> 48);
    p[7] = (u8)(v >> 56);
}

// Called when memory runs out.
[[noreturn]] static void os_oom(os *);

// Called when an arena lacks room for need bytes, padding included.
// Makes room, as by committing more memory, while keeping the end from
// which the arena allocates, or exits through os_oom.
static void os_extend(os *, arena *, iz need);

// Padding to align an allocation of len bytes, which must fit unpadded.
static iz alloc_pad(arena *a, iz len, iz align)
{
    uptr p = a->down ? (uptr)(a->end - len) : -(uptr)a->beg;
    return (iz)(p & (uptr)(align - 1));
}

static void *alloc(arena *a, iz count, iz size, iz align, b32 zero)
{
    assert(count >= 0);
    assert(size > 0);
    iz avail = a->end - a->beg;
    if (count>avail/size || alloc_pad(a, count*size, align)>avail-count*size) {
        if (count > ((iz)((uz)-1 >> 1) - align)/size) {
            os_oom(a->ctx);
        }
        os_extend(a->ctx, a, count*size + align-1);  // room for any padding
    }
    iz    len = count * size;
    iz    pad = alloc_pad(a, len, align);
    byte *r   = a->down ? a->end - len - pad : a->beg + pad;
    if (a->down) {
        a->end = r;
    } else {
        a->beg = r + len;
    }
    return zero ? bytefill(r, 0, len) : r;
}

// tugz core: CRC-32 (ISO-HDLC, reflected 0xedb88320)
//
// Slicing-by-8: crc32_table[k][n] is the CRC of byte n followed by k
// zero bytes, so eight table lookups advance the CRC by eight bytes.

static u32 const crc32_table[8][256] = {
    {
        0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
        0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
        0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
        0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
        0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
        0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
        0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
        0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
        0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
        0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
        0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
        0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
        0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
        0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
        0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
        0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
        0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
        0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
        0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
        0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
        0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
        0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
        0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
        0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
        0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
        0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
        0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
        0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
        0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
        0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
        0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
        0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
        0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
        0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
        0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
        0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
        0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
        0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
        0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
        0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
        0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
        0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
        0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
    },
    {
        0x00000000, 0x191b3141, 0x32366282, 0x2b2d53c3, 0x646cc504, 0x7d77f445,
        0x565aa786, 0x4f4196c7, 0xc8d98a08, 0xd1c2bb49, 0xfaefe88a, 0xe3f4d9cb,
        0xacb54f0c, 0xb5ae7e4d, 0x9e832d8e, 0x87981ccf, 0x4ac21251, 0x53d92310,
        0x78f470d3, 0x61ef4192, 0x2eaed755, 0x37b5e614, 0x1c98b5d7, 0x05838496,
        0x821b9859, 0x9b00a918, 0xb02dfadb, 0xa936cb9a, 0xe6775d5d, 0xff6c6c1c,
        0xd4413fdf, 0xcd5a0e9e, 0x958424a2, 0x8c9f15e3, 0xa7b24620, 0xbea97761,
        0xf1e8e1a6, 0xe8f3d0e7, 0xc3de8324, 0xdac5b265, 0x5d5daeaa, 0x44469feb,
        0x6f6bcc28, 0x7670fd69, 0x39316bae, 0x202a5aef, 0x0b07092c, 0x121c386d,
        0xdf4636f3, 0xc65d07b2, 0xed705471, 0xf46b6530, 0xbb2af3f7, 0xa231c2b6,
        0x891c9175, 0x9007a034, 0x179fbcfb, 0x0e848dba, 0x25a9de79, 0x3cb2ef38,
        0x73f379ff, 0x6ae848be, 0x41c51b7d, 0x58de2a3c, 0xf0794f05, 0xe9627e44,
        0xc24f2d87, 0xdb541cc6, 0x94158a01, 0x8d0ebb40, 0xa623e883, 0xbf38d9c2,
        0x38a0c50d, 0x21bbf44c, 0x0a96a78f, 0x138d96ce, 0x5ccc0009, 0x45d73148,
        0x6efa628b, 0x77e153ca, 0xbabb5d54, 0xa3a06c15, 0x888d3fd6, 0x91960e97,
        0xded79850, 0xc7cca911, 0xece1fad2, 0xf5facb93, 0x7262d75c, 0x6b79e61d,
        0x4054b5de, 0x594f849f, 0x160e1258, 0x0f152319, 0x243870da, 0x3d23419b,
        0x65fd6ba7, 0x7ce65ae6, 0x57cb0925, 0x4ed03864, 0x0191aea3, 0x188a9fe2,
        0x33a7cc21, 0x2abcfd60, 0xad24e1af, 0xb43fd0ee, 0x9f12832d, 0x8609b26c,
        0xc94824ab, 0xd05315ea, 0xfb7e4629, 0xe2657768, 0x2f3f79f6, 0x362448b7,
        0x1d091b74, 0x04122a35, 0x4b53bcf2, 0x52488db3, 0x7965de70, 0x607eef31,
        0xe7e6f3fe, 0xfefdc2bf, 0xd5d0917c, 0xcccba03d, 0x838a36fa, 0x9a9107bb,
        0xb1bc5478, 0xa8a76539, 0x3b83984b, 0x2298a90a, 0x09b5fac9, 0x10aecb88,
        0x5fef5d4f, 0x46f46c0e, 0x6dd93fcd, 0x74c20e8c, 0xf35a1243, 0xea412302,
        0xc16c70c1, 0xd8774180, 0x9736d747, 0x8e2de606, 0xa500b5c5, 0xbc1b8484,
        0x71418a1a, 0x685abb5b, 0x4377e898, 0x5a6cd9d9, 0x152d4f1e, 0x0c367e5f,
        0x271b2d9c, 0x3e001cdd, 0xb9980012, 0xa0833153, 0x8bae6290, 0x92b553d1,
        0xddf4c516, 0xc4eff457, 0xefc2a794, 0xf6d996d5, 0xae07bce9, 0xb71c8da8,
        0x9c31de6b, 0x852aef2a, 0xca6b79ed, 0xd37048ac, 0xf85d1b6f, 0xe1462a2e,
        0x66de36e1, 0x7fc507a0, 0x54e85463, 0x4df36522, 0x02b2f3e5, 0x1ba9c2a4,
        0x30849167, 0x299fa026, 0xe4c5aeb8, 0xfdde9ff9, 0xd6f3cc3a, 0xcfe8fd7b,
        0x80a96bbc, 0x99b25afd, 0xb29f093e, 0xab84387f, 0x2c1c24b0, 0x350715f1,
        0x1e2a4632, 0x07317773, 0x4870e1b4, 0x516bd0f5, 0x7a468336, 0x635db277,
        0xcbfad74e, 0xd2e1e60f, 0xf9ccb5cc, 0xe0d7848d, 0xaf96124a, 0xb68d230b,
        0x9da070c8, 0x84bb4189, 0x03235d46, 0x1a386c07, 0x31153fc4, 0x280e0e85,
        0x674f9842, 0x7e54a903, 0x5579fac0, 0x4c62cb81, 0x8138c51f, 0x9823f45e,
        0xb30ea79d, 0xaa1596dc, 0xe554001b, 0xfc4f315a, 0xd7626299, 0xce7953d8,
        0x49e14f17, 0x50fa7e56, 0x7bd72d95, 0x62cc1cd4, 0x2d8d8a13, 0x3496bb52,
        0x1fbbe891, 0x06a0d9d0, 0x5e7ef3ec, 0x4765c2ad, 0x6c48916e, 0x7553a02f,
        0x3a1236e8, 0x230907a9, 0x0824546a, 0x113f652b, 0x96a779e4, 0x8fbc48a5,
        0xa4911b66, 0xbd8a2a27, 0xf2cbbce0, 0xebd08da1, 0xc0fdde62, 0xd9e6ef23,
        0x14bce1bd, 0x0da7d0fc, 0x268a833f, 0x3f91b27e, 0x70d024b9, 0x69cb15f8,
        0x42e6463b, 0x5bfd777a, 0xdc656bb5, 0xc57e5af4, 0xee530937, 0xf7483876,
        0xb809aeb1, 0xa1129ff0, 0x8a3fcc33, 0x9324fd72,
    },
    {
        0x00000000, 0x01c26a37, 0x0384d46e, 0x0246be59, 0x0709a8dc, 0x06cbc2eb,
        0x048d7cb2, 0x054f1685, 0x0e1351b8, 0x0fd13b8f, 0x0d9785d6, 0x0c55efe1,
        0x091af964, 0x08d89353, 0x0a9e2d0a, 0x0b5c473d, 0x1c26a370, 0x1de4c947,
        0x1fa2771e, 0x1e601d29, 0x1b2f0bac, 0x1aed619b, 0x18abdfc2, 0x1969b5f5,
        0x1235f2c8, 0x13f798ff, 0x11b126a6, 0x10734c91, 0x153c5a14, 0x14fe3023,
        0x16b88e7a, 0x177ae44d, 0x384d46e0, 0x398f2cd7, 0x3bc9928e, 0x3a0bf8b9,
        0x3f44ee3c, 0x3e86840b, 0x3cc03a52, 0x3d025065, 0x365e1758, 0x379c7d6f,
        0x35dac336, 0x3418a901, 0x3157bf84, 0x3095d5b3, 0x32d36bea, 0x331101dd,
        0x246be590, 0x25a98fa7, 0x27ef31fe, 0x262d5bc9, 0x23624d4c, 0x22a0277b,
        0x20e69922, 0x2124f315, 0x2a78b428, 0x2bbade1f, 0x29fc6046, 0x283e0a71,
        0x2d711cf4, 0x2cb376c3, 0x2ef5c89a, 0x2f37a2ad, 0x709a8dc0, 0x7158e7f7,
        0x731e59ae, 0x72dc3399, 0x7793251c, 0x76514f2b, 0x7417f172, 0x75d59b45,
        0x7e89dc78, 0x7f4bb64f, 0x7d0d0816, 0x7ccf6221, 0x798074a4, 0x78421e93,
        0x7a04a0ca, 0x7bc6cafd, 0x6cbc2eb0, 0x6d7e4487, 0x6f38fade, 0x6efa90e9,
        0x6bb5866c, 0x6a77ec5b, 0x68315202, 0x69f33835, 0x62af7f08, 0x636d153f,
        0x612bab66, 0x60e9c151, 0x65a6d7d4, 0x6464bde3, 0x662203ba, 0x67e0698d,
        0x48d7cb20, 0x4915a117, 0x4b531f4e, 0x4a917579, 0x4fde63fc, 0x4e1c09cb,
        0x4c5ab792, 0x4d98dda5, 0x46c49a98, 0x4706f0af, 0x45404ef6, 0x448224c1,
        0x41cd3244, 0x400f5873, 0x4249e62a, 0x438b8c1d, 0x54f16850, 0x55330267,
        0x5775bc3e, 0x56b7d609, 0x53f8c08c, 0x523aaabb, 0x507c14e2, 0x51be7ed5,
        0x5ae239e8, 0x5b2053df, 0x5966ed86, 0x58a487b1, 0x5deb9134, 0x5c29fb03,
        0x5e6f455a, 0x5fad2f6d, 0xe1351b80, 0xe0f771b7, 0xe2b1cfee, 0xe373a5d9,
        0xe63cb35c, 0xe7fed96b, 0xe5b86732, 0xe47a0d05, 0xef264a38, 0xeee4200f,
        0xeca29e56, 0xed60f461, 0xe82fe2e4, 0xe9ed88d3, 0xebab368a, 0xea695cbd,
        0xfd13b8f0, 0xfcd1d2c7, 0xfe976c9e, 0xff5506a9, 0xfa1a102c, 0xfbd87a1b,
        0xf99ec442, 0xf85cae75, 0xf300e948, 0xf2c2837f, 0xf0843d26, 0xf1465711,
        0xf4094194, 0xf5cb2ba3, 0xf78d95fa, 0xf64fffcd, 0xd9785d60, 0xd8ba3757,
        0xdafc890e, 0xdb3ee339, 0xde71f5bc, 0xdfb39f8b, 0xddf521d2, 0xdc374be5,
        0xd76b0cd8, 0xd6a966ef, 0xd4efd8b6, 0xd52db281, 0xd062a404, 0xd1a0ce33,
        0xd3e6706a, 0xd2241a5d, 0xc55efe10, 0xc49c9427, 0xc6da2a7e, 0xc7184049,
        0xc25756cc, 0xc3953cfb, 0xc1d382a2, 0xc011e895, 0xcb4dafa8, 0xca8fc59f,
        0xc8c97bc6, 0xc90b11f1, 0xcc440774, 0xcd866d43, 0xcfc0d31a, 0xce02b92d,
        0x91af9640, 0x906dfc77, 0x922b422e, 0x93e92819, 0x96a63e9c, 0x976454ab,
        0x9522eaf2, 0x94e080c5, 0x9fbcc7f8, 0x9e7eadcf, 0x9c381396, 0x9dfa79a1,
        0x98b56f24, 0x99770513, 0x9b31bb4a, 0x9af3d17d, 0x8d893530, 0x8c4b5f07,
        0x8e0de15e, 0x8fcf8b69, 0x8a809dec, 0x8b42f7db, 0x89044982, 0x88c623b5,
        0x839a6488, 0x82580ebf, 0x801eb0e6, 0x81dcdad1, 0x8493cc54, 0x8551a663,
        0x8717183a, 0x86d5720d, 0xa9e2d0a0, 0xa820ba97, 0xaa6604ce, 0xaba46ef9,
        0xaeeb787c, 0xaf29124b, 0xad6fac12, 0xacadc625, 0xa7f18118, 0xa633eb2f,
        0xa4755576, 0xa5b73f41, 0xa0f829c4, 0xa13a43f3, 0xa37cfdaa, 0xa2be979d,
        0xb5c473d0, 0xb40619e7, 0xb640a7be, 0xb782cd89, 0xb2cddb0c, 0xb30fb13b,
        0xb1490f62, 0xb08b6555, 0xbbd72268, 0xba15485f, 0xb853f606, 0xb9919c31,
        0xbcde8ab4, 0xbd1ce083, 0xbf5a5eda, 0xbe9834ed,
    },
    {
        0x00000000, 0xb8bc6765, 0xaa09c88b, 0x12b5afee, 0x8f629757, 0x37def032,
        0x256b5fdc, 0x9dd738b9, 0xc5b428ef, 0x7d084f8a, 0x6fbde064, 0xd7018701,
        0x4ad6bfb8, 0xf26ad8dd, 0xe0df7733, 0x58631056, 0x5019579f, 0xe8a530fa,
        0xfa109f14, 0x42acf871, 0xdf7bc0c8, 0x67c7a7ad, 0x75720843, 0xcdce6f26,
        0x95ad7f70, 0x2d111815, 0x3fa4b7fb, 0x8718d09e, 0x1acfe827, 0xa2738f42,
        0xb0c620ac, 0x087a47c9, 0xa032af3e, 0x188ec85b, 0x0a3b67b5, 0xb28700d0,
        0x2f503869, 0x97ec5f0c, 0x8559f0e2, 0x3de59787, 0x658687d1, 0xdd3ae0b4,
        0xcf8f4f5a, 0x7733283f, 0xeae41086, 0x525877e3, 0x40edd80d, 0xf851bf68,
        0xf02bf8a1, 0x48979fc4, 0x5a22302a, 0xe29e574f, 0x7f496ff6, 0xc7f50893,
        0xd540a77d, 0x6dfcc018, 0x359fd04e, 0x8d23b72b, 0x9f9618c5, 0x272a7fa0,
        0xbafd4719, 0x0241207c, 0x10f48f92, 0xa848e8f7, 0x9b14583d, 0x23a83f58,
        0x311d90b6, 0x89a1f7d3, 0x1476cf6a, 0xaccaa80f, 0xbe7f07e1, 0x06c36084,
        0x5ea070d2, 0xe61c17b7, 0xf4a9b859, 0x4c15df3c, 0xd1c2e785, 0x697e80e0,
        0x7bcb2f0e, 0xc377486b, 0xcb0d0fa2, 0x73b168c7, 0x6104c729, 0xd9b8a04c,
        0x446f98f5, 0xfcd3ff90, 0xee66507e, 0x56da371b, 0x0eb9274d, 0xb6054028,
        0xa4b0efc6, 0x1c0c88a3, 0x81dbb01a, 0x3967d77f, 0x2bd27891, 0x936e1ff4,
        0x3b26f703, 0x839a9066, 0x912f3f88, 0x299358ed, 0xb4446054, 0x0cf80731,
        0x1e4da8df, 0xa6f1cfba, 0xfe92dfec, 0x462eb889, 0x549b1767, 0xec277002,
        0x71f048bb, 0xc94c2fde, 0xdbf98030, 0x6345e755, 0x6b3fa09c, 0xd383c7f9,
        0xc1366817, 0x798a0f72, 0xe45d37cb, 0x5ce150ae, 0x4e54ff40, 0xf6e89825,
        0xae8b8873, 0x1637ef16, 0x048240f8, 0xbc3e279d, 0x21e91f24, 0x99557841,
        0x8be0d7af, 0x335cb0ca, 0xed59b63b, 0x55e5d15e, 0x47507eb0, 0xffec19d5,
        0x623b216c, 0xda874609, 0xc832e9e7, 0x708e8e82, 0x28ed9ed4, 0x9051f9b1,
        0x82e4565f, 0x3a58313a, 0xa78f0983, 0x1f336ee6, 0x0d86c108, 0xb53aa66d,
        0xbd40e1a4, 0x05fc86c1, 0x1749292f, 0xaff54e4a, 0x322276f3, 0x8a9e1196,
        0x982bbe78, 0x2097d91d, 0x78f4c94b, 0xc048ae2e, 0xd2fd01c0, 0x6a4166a5,
        0xf7965e1c, 0x4f2a3979, 0x5d9f9697, 0xe523f1f2, 0x4d6b1905, 0xf5d77e60,
        0xe762d18e, 0x5fdeb6eb, 0xc2098e52, 0x7ab5e937, 0x680046d9, 0xd0bc21bc,
        0x88df31ea, 0x3063568f, 0x22d6f961, 0x9a6a9e04, 0x07bda6bd, 0xbf01c1d8,
        0xadb46e36, 0x15080953, 0x1d724e9a, 0xa5ce29ff, 0xb77b8611, 0x0fc7e174,
        0x9210d9cd, 0x2aacbea8, 0x38191146, 0x80a57623, 0xd8c66675, 0x607a0110,
        0x72cfaefe, 0xca73c99b, 0x57a4f122, 0xef189647, 0xfdad39a9, 0x45115ecc,
        0x764dee06, 0xcef18963, 0xdc44268d, 0x64f841e8, 0xf92f7951, 0x41931e34,
        0x5326b1da, 0xeb9ad6bf, 0xb3f9c6e9, 0x0b45a18c, 0x19f00e62, 0xa14c6907,
        0x3c9b51be, 0x842736db, 0x96929935, 0x2e2efe50, 0x2654b999, 0x9ee8defc,
        0x8c5d7112, 0x34e11677, 0xa9362ece, 0x118a49ab, 0x033fe645, 0xbb838120,
        0xe3e09176, 0x5b5cf613, 0x49e959fd, 0xf1553e98, 0x6c820621, 0xd43e6144,
        0xc68bceaa, 0x7e37a9cf, 0xd67f4138, 0x6ec3265d, 0x7c7689b3, 0xc4caeed6,
        0x591dd66f, 0xe1a1b10a, 0xf3141ee4, 0x4ba87981, 0x13cb69d7, 0xab770eb2,
        0xb9c2a15c, 0x017ec639, 0x9ca9fe80, 0x241599e5, 0x36a0360b, 0x8e1c516e,
        0x866616a7, 0x3eda71c2, 0x2c6fde2c, 0x94d3b949, 0x090481f0, 0xb1b8e695,
        0xa30d497b, 0x1bb12e1e, 0x43d23e48, 0xfb6e592d, 0xe9dbf6c3, 0x516791a6,
        0xccb0a91f, 0x740cce7a, 0x66b96194, 0xde0506f1,
    },
    {
        0x00000000, 0x3d6029b0, 0x7ac05360, 0x47a07ad0, 0xf580a6c0, 0xc8e08f70,
        0x8f40f5a0, 0xb220dc10, 0x30704bc1, 0x0d106271, 0x4ab018a1, 0x77d03111,
        0xc5f0ed01, 0xf890c4b1, 0xbf30be61, 0x825097d1, 0x60e09782, 0x5d80be32,
        0x1a20c4e2, 0x2740ed52, 0x95603142, 0xa80018f2, 0xefa06222, 0xd2c04b92,
        0x5090dc43, 0x6df0f5f3, 0x2a508f23, 0x1730a693, 0xa5107a83, 0x98705333,
        0xdfd029e3, 0xe2b00053, 0xc1c12f04, 0xfca106b4, 0xbb017c64, 0x866155d4,
        0x344189c4, 0x0921a074, 0x4e81daa4, 0x73e1f314, 0xf1b164c5, 0xccd14d75,
        0x8b7137a5, 0xb6111e15, 0x0431c205, 0x3951ebb5, 0x7ef19165, 0x4391b8d5,
        0xa121b886, 0x9c419136, 0xdbe1ebe6, 0xe681c256, 0x54a11e46, 0x69c137f6,
        0x2e614d26, 0x13016496, 0x9151f347, 0xac31daf7, 0xeb91a027, 0xd6f18997,
        0x64d15587, 0x59b17c37, 0x1e1106e7, 0x23712f57, 0x58f35849, 0x659371f9,
        0x22330b29, 0x1f532299, 0xad73fe89, 0x9013d739, 0xd7b3ade9, 0xead38459,
        0x68831388, 0x55e33a38, 0x124340e8, 0x2f236958, 0x9d03b548, 0xa0639cf8,
        0xe7c3e628, 0xdaa3cf98, 0x3813cfcb, 0x0573e67b, 0x42d39cab, 0x7fb3b51b,
        0xcd93690b, 0xf0f340bb, 0xb7533a6b, 0x8a3313db, 0x0863840a, 0x3503adba,
        0x72a3d76a, 0x4fc3feda, 0xfde322ca, 0xc0830b7a, 0x872371aa, 0xba43581a,
        0x9932774d, 0xa4525efd, 0xe3f2242d, 0xde920d9d, 0x6cb2d18d, 0x51d2f83d,
        0x167282ed, 0x2b12ab5d, 0xa9423c8c, 0x9422153c, 0xd3826fec, 0xeee2465c,
        0x5cc29a4c, 0x61a2b3fc, 0x2602c92c, 0x1b62e09c, 0xf9d2e0cf, 0xc4b2c97f,
        0x8312b3af, 0xbe729a1f, 0x0c52460f, 0x31326fbf, 0x7692156f, 0x4bf23cdf,
        0xc9a2ab0e, 0xf4c282be, 0xb362f86e, 0x8e02d1de, 0x3c220dce, 0x0142247e,
        0x46e25eae, 0x7b82771e, 0xb1e6b092, 0x8c869922, 0xcb26e3f2, 0xf646ca42,
        0x44661652, 0x79063fe2, 0x3ea64532, 0x03c66c82, 0x8196fb53, 0xbcf6d2e3,
        0xfb56a833, 0xc6368183, 0x74165d93, 0x49767423, 0x0ed60ef3, 0x33b62743,
        0xd1062710, 0xec660ea0, 0xabc67470, 0x96a65dc0, 0x248681d0, 0x19e6a860,
        0x5e46d2b0, 0x6326fb00, 0xe1766cd1, 0xdc164561, 0x9bb63fb1, 0xa6d61601,
        0x14f6ca11, 0x2996e3a1, 0x6e369971, 0x5356b0c1, 0x70279f96, 0x4d47b626,
        0x0ae7ccf6, 0x3787e546, 0x85a73956, 0xb8c710e6, 0xff676a36, 0xc2074386,
        0x4057d457, 0x7d37fde7, 0x3a978737, 0x07f7ae87, 0xb5d77297, 0x88b75b27,
        0xcf1721f7, 0xf2770847, 0x10c70814, 0x2da721a4, 0x6a075b74, 0x576772c4,
        0xe547aed4, 0xd8278764, 0x9f87fdb4, 0xa2e7d404, 0x20b743d5, 0x1dd76a65,
        0x5a7710b5, 0x67173905, 0xd537e515, 0xe857cca5, 0xaff7b675, 0x92979fc5,
        0xe915e8db, 0xd475c16b, 0x93d5bbbb, 0xaeb5920b, 0x1c954e1b, 0x21f567ab,
        0x66551d7b, 0x5b3534cb, 0xd965a31a, 0xe4058aaa, 0xa3a5f07a, 0x9ec5d9ca,
        0x2ce505da, 0x11852c6a, 0x562556ba, 0x6b457f0a, 0x89f57f59, 0xb49556e9,
        0xf3352c39, 0xce550589, 0x7c75d999, 0x4115f029, 0x06b58af9, 0x3bd5a349,
        0xb9853498, 0x84e51d28, 0xc34567f8, 0xfe254e48, 0x4c059258, 0x7165bbe8,
        0x36c5c138, 0x0ba5e888, 0x28d4c7df, 0x15b4ee6f, 0x521494bf, 0x6f74bd0f,
        0xdd54611f, 0xe03448af, 0xa794327f, 0x9af41bcf, 0x18a48c1e, 0x25c4a5ae,
        0x6264df7e, 0x5f04f6ce, 0xed242ade, 0xd044036e, 0x97e479be, 0xaa84500e,
        0x4834505d, 0x755479ed, 0x32f4033d, 0x0f942a8d, 0xbdb4f69d, 0x80d4df2d,
        0xc774a5fd, 0xfa148c4d, 0x78441b9c, 0x4524322c, 0x028448fc, 0x3fe4614c,
        0x8dc4bd5c, 0xb0a494ec, 0xf704ee3c, 0xca64c78c,
    },
    {
        0x00000000, 0xcb5cd3a5, 0x4dc8a10b, 0x869472ae, 0x9b914216, 0x50cd91b3,
        0xd659e31d, 0x1d0530b8, 0xec53826d, 0x270f51c8, 0xa19b2366, 0x6ac7f0c3,
        0x77c2c07b, 0xbc9e13de, 0x3a0a6170, 0xf156b2d5, 0x03d6029b, 0xc88ad13e,
        0x4e1ea390, 0x85427035, 0x9847408d, 0x531b9328, 0xd58fe186, 0x1ed33223,
        0xef8580f6, 0x24d95353, 0xa24d21fd, 0x6911f258, 0x7414c2e0, 0xbf481145,
        0x39dc63eb, 0xf280b04e, 0x07ac0536, 0xccf0d693, 0x4a64a43d, 0x81387798,
        0x9c3d4720, 0x57619485, 0xd1f5e62b, 0x1aa9358e, 0xebff875b, 0x20a354fe,
        0xa6372650, 0x6d6bf5f5, 0x706ec54d, 0xbb3216e8, 0x3da66446, 0xf6fab7e3,
        0x047a07ad, 0xcf26d408, 0x49b2a6a6, 0x82ee7503, 0x9feb45bb, 0x54b7961e,
        0xd223e4b0, 0x197f3715, 0xe82985c0, 0x23755665, 0xa5e124cb, 0x6ebdf76e,
        0x73b8c7d6, 0xb8e41473, 0x3e7066dd, 0xf52cb578, 0x0f580a6c, 0xc404d9c9,
        0x4290ab67, 0x89cc78c2, 0x94c9487a, 0x5f959bdf, 0xd901e971, 0x125d3ad4,
        0xe30b8801, 0x28575ba4, 0xaec3290a, 0x659ffaaf, 0x789aca17, 0xb3c619b2,
        0x35526b1c, 0xfe0eb8b9, 0x0c8e08f7, 0xc7d2db52, 0x4146a9fc, 0x8a1a7a59,
        0x971f4ae1, 0x5c439944, 0xdad7ebea, 0x118b384f, 0xe0dd8a9a, 0x2b81593f,
        0xad152b91, 0x6649f834, 0x7b4cc88c, 0xb0101b29, 0x36846987, 0xfdd8ba22,
        0x08f40f5a, 0xc3a8dcff, 0x453cae51, 0x8e607df4, 0x93654d4c, 0x58399ee9,
        0xdeadec47, 0x15f13fe2, 0xe4a78d37, 0x2ffb5e92, 0xa96f2c3c, 0x6233ff99,
        0x7f36cf21, 0xb46a1c84, 0x32fe6e2a, 0xf9a2bd8f, 0x0b220dc1, 0xc07ede64,
        0x46eaacca, 0x8db67f6f, 0x90b34fd7, 0x5bef9c72, 0xdd7beedc, 0x16273d79,
        0xe7718fac, 0x2c2d5c09, 0xaab92ea7, 0x61e5fd02, 0x7ce0cdba, 0xb7bc1e1f,
        0x31286cb1, 0xfa74bf14, 0x1eb014d8, 0xd5ecc77d, 0x5378b5d3, 0x98246676,
        0x852156ce, 0x4e7d856b, 0xc8e9f7c5, 0x03b52460, 0xf2e396b5, 0x39bf4510,
        0xbf2b37be, 0x7477e41b, 0x6972d4a3, 0xa22e0706, 0x24ba75a8, 0xefe6a60d,
        0x1d661643, 0xd63ac5e6, 0x50aeb748, 0x9bf264ed, 0x86f75455, 0x4dab87f0,
        0xcb3ff55e, 0x006326fb, 0xf135942e, 0x3a69478b, 0xbcfd3525, 0x77a1e680,
        0x6aa4d638, 0xa1f8059d, 0x276c7733, 0xec30a496, 0x191c11ee, 0xd240c24b,
        0x54d4b0e5, 0x9f886340, 0x828d53f8, 0x49d1805d, 0xcf45f2f3, 0x04192156,
        0xf54f9383, 0x3e134026, 0xb8873288, 0x73dbe12d, 0x6eded195, 0xa5820230,
        0x2316709e, 0xe84aa33b, 0x1aca1375, 0xd196c0d0, 0x5702b27e, 0x9c5e61db,
        0x815b5163, 0x4a0782c6, 0xcc93f068, 0x07cf23cd, 0xf6999118, 0x3dc542bd,
        0xbb513013, 0x700de3b6, 0x6d08d30e, 0xa65400ab, 0x20c07205, 0xeb9ca1a0,
        0x11e81eb4, 0xdab4cd11, 0x5c20bfbf, 0x977c6c1a, 0x8a795ca2, 0x41258f07,
        0xc7b1fda9, 0x0ced2e0c, 0xfdbb9cd9, 0x36e74f7c, 0xb0733dd2, 0x7b2fee77,
        0x662adecf, 0xad760d6a, 0x2be27fc4, 0xe0beac61, 0x123e1c2f, 0xd962cf8a,
        0x5ff6bd24, 0x94aa6e81, 0x89af5e39, 0x42f38d9c, 0xc467ff32, 0x0f3b2c97,
        0xfe6d9e42, 0x35314de7, 0xb3a53f49, 0x78f9ecec, 0x65fcdc54, 0xaea00ff1,
        0x28347d5f, 0xe368aefa, 0x16441b82, 0xdd18c827, 0x5b8cba89, 0x90d0692c,
        0x8dd55994, 0x46898a31, 0xc01df89f, 0x0b412b3a, 0xfa1799ef, 0x314b4a4a,
        0xb7df38e4, 0x7c83eb41, 0x6186dbf9, 0xaada085c, 0x2c4e7af2, 0xe712a957,
        0x15921919, 0xdececabc, 0x585ab812, 0x93066bb7, 0x8e035b0f, 0x455f88aa,
        0xc3cbfa04, 0x089729a1, 0xf9c19b74, 0x329d48d1, 0xb4093a7f, 0x7f55e9da,
        0x6250d962, 0xa90c0ac7, 0x2f987869, 0xe4c4abcc,
    },
    {
        0x00000000, 0xa6770bb4, 0x979f1129, 0x31e81a9d, 0xf44f2413, 0x52382fa7,
        0x63d0353a, 0xc5a73e8e, 0x33ef4e67, 0x959845d3, 0xa4705f4e, 0x020754fa,
        0xc7a06a74, 0x61d761c0, 0x503f7b5d, 0xf64870e9, 0x67de9cce, 0xc1a9977a,
        0xf0418de7, 0x56368653, 0x9391b8dd, 0x35e6b369, 0x040ea9f4, 0xa279a240,
        0x5431d2a9, 0xf246d91d, 0xc3aec380, 0x65d9c834, 0xa07ef6ba, 0x0609fd0e,
        0x37e1e793, 0x9196ec27, 0xcfbd399c, 0x69ca3228, 0x582228b5, 0xfe552301,
        0x3bf21d8f, 0x9d85163b, 0xac6d0ca6, 0x0a1a0712, 0xfc5277fb, 0x5a257c4f,
        0x6bcd66d2, 0xcdba6d66, 0x081d53e8, 0xae6a585c, 0x9f8242c1, 0x39f54975,
        0xa863a552, 0x0e14aee6, 0x3ffcb47b, 0x998bbfcf, 0x5c2c8141, 0xfa5b8af5,
        0xcbb39068, 0x6dc49bdc, 0x9b8ceb35, 0x3dfbe081, 0x0c13fa1c, 0xaa64f1a8,
        0x6fc3cf26, 0xc9b4c492, 0xf85cde0f, 0x5e2bd5bb, 0x440b7579, 0xe27c7ecd,
        0xd3946450, 0x75e36fe4, 0xb044516a, 0x16335ade, 0x27db4043, 0x81ac4bf7,
        0x77e43b1e, 0xd19330aa, 0xe07b2a37, 0x460c2183, 0x83ab1f0d, 0x25dc14b9,
        0x14340e24, 0xb2430590, 0x23d5e9b7, 0x85a2e203, 0xb44af89e, 0x123df32a,
        0xd79acda4, 0x71edc610, 0x4005dc8d, 0xe672d739, 0x103aa7d0, 0xb64dac64,
        0x87a5b6f9, 0x21d2bd4d, 0xe47583c3, 0x42028877, 0x73ea92ea, 0xd59d995e,
        0x8bb64ce5, 0x2dc14751, 0x1c295dcc, 0xba5e5678, 0x7ff968f6, 0xd98e6342,
        0xe86679df, 0x4e11726b, 0xb8590282, 0x1e2e0936, 0x2fc613ab, 0x89b1181f,
        0x4c162691, 0xea612d25, 0xdb8937b8, 0x7dfe3c0c, 0xec68d02b, 0x4a1fdb9f,
        0x7bf7c102, 0xdd80cab6, 0x1827f438, 0xbe50ff8c, 0x8fb8e511, 0x29cfeea5,
        0xdf879e4c, 0x79f095f8, 0x48188f65, 0xee6f84d1, 0x2bc8ba5f, 0x8dbfb1eb,
        0xbc57ab76, 0x1a20a0c2, 0x8816eaf2, 0x2e61e146, 0x1f89fbdb, 0xb9fef06f,
        0x7c59cee1, 0xda2ec555, 0xebc6dfc8, 0x4db1d47c, 0xbbf9a495, 0x1d8eaf21,
        0x2c66b5bc, 0x8a11be08, 0x4fb68086, 0xe9c18b32, 0xd82991af, 0x7e5e9a1b,
        0xefc8763c, 0x49bf7d88, 0x78576715, 0xde206ca1, 0x1b87522f, 0xbdf0599b,
        0x8c184306, 0x2a6f48b2, 0xdc27385b, 0x7a5033ef, 0x4bb82972, 0xedcf22c6,
        0x28681c48, 0x8e1f17fc, 0xbff70d61, 0x198006d5, 0x47abd36e, 0xe1dcd8da,
        0xd034c247, 0x7643c9f3, 0xb3e4f77d, 0x1593fcc9, 0x247be654, 0x820cede0,
        0x74449d09, 0xd23396bd, 0xe3db8c20, 0x45ac8794, 0x800bb91a, 0x267cb2ae,
        0x1794a833, 0xb1e3a387, 0x20754fa0, 0x86024414, 0xb7ea5e89, 0x119d553d,
        0xd43a6bb3, 0x724d6007, 0x43a57a9a, 0xe5d2712e, 0x139a01c7, 0xb5ed0a73,
        0x840510ee, 0x22721b5a, 0xe7d525d4, 0x41a22e60, 0x704a34fd, 0xd63d3f49,
        0xcc1d9f8b, 0x6a6a943f, 0x5b828ea2, 0xfdf58516, 0x3852bb98, 0x9e25b02c,
        0xafcdaab1, 0x09baa105, 0xfff2d1ec, 0x5985da58, 0x686dc0c5, 0xce1acb71,
        0x0bbdf5ff, 0xadcafe4b, 0x9c22e4d6, 0x3a55ef62, 0xabc30345, 0x0db408f1,
        0x3c5c126c, 0x9a2b19d8, 0x5f8c2756, 0xf9fb2ce2, 0xc813367f, 0x6e643dcb,
        0x982c4d22, 0x3e5b4696, 0x0fb35c0b, 0xa9c457bf, 0x6c636931, 0xca146285,
        0xfbfc7818, 0x5d8b73ac, 0x03a0a617, 0xa5d7ada3, 0x943fb73e, 0x3248bc8a,
        0xf7ef8204, 0x519889b0, 0x6070932d, 0xc6079899, 0x304fe870, 0x9638e3c4,
        0xa7d0f959, 0x01a7f2ed, 0xc400cc63, 0x6277c7d7, 0x539fdd4a, 0xf5e8d6fe,
        0x647e3ad9, 0xc209316d, 0xf3e12bf0, 0x55962044, 0x90311eca, 0x3646157e,
        0x07ae0fe3, 0xa1d90457, 0x579174be, 0xf1e67f0a, 0xc00e6597, 0x66796e23,
        0xa3de50ad, 0x05a95b19, 0x34414184, 0x92364a30,
    },
    {
        0x00000000, 0xccaa009e, 0x4225077d, 0x8e8f07e3, 0x844a0efa, 0x48e00e64,
        0xc66f0987, 0x0ac50919, 0xd3e51bb5, 0x1f4f1b2b, 0x91c01cc8, 0x5d6a1c56,
        0x57af154f, 0x9b0515d1, 0x158a1232, 0xd92012ac, 0x7cbb312b, 0xb01131b5,
        0x3e9e3656, 0xf23436c8, 0xf8f13fd1, 0x345b3f4f, 0xbad438ac, 0x767e3832,
        0xaf5e2a9e, 0x63f42a00, 0xed7b2de3, 0x21d12d7d, 0x2b142464, 0xe7be24fa,
        0x69312319, 0xa59b2387, 0xf9766256, 0x35dc62c8, 0xbb53652b, 0x77f965b5,
        0x7d3c6cac, 0xb1966c32, 0x3f196bd1, 0xf3b36b4f, 0x2a9379e3, 0xe639797d,
        0x68b67e9e, 0xa41c7e00, 0xaed97719, 0x62737787, 0xecfc7064, 0x205670fa,
        0x85cd537d, 0x496753e3, 0xc7e85400, 0x0b42549e, 0x01875d87, 0xcd2d5d19,
        0x43a25afa, 0x8f085a64, 0x562848c8, 0x9a824856, 0x140d4fb5, 0xd8a74f2b,
        0xd2624632, 0x1ec846ac, 0x9047414f, 0x5ced41d1, 0x299dc2ed, 0xe537c273,
        0x6bb8c590, 0xa712c50e, 0xadd7cc17, 0x617dcc89, 0xeff2cb6a, 0x2358cbf4,
        0xfa78d958, 0x36d2d9c6, 0xb85dde25, 0x74f7debb, 0x7e32d7a2, 0xb298d73c,
        0x3c17d0df, 0xf0bdd041, 0x5526f3c6, 0x998cf358, 0x1703f4bb, 0xdba9f425,
        0xd16cfd3c, 0x1dc6fda2, 0x9349fa41, 0x5fe3fadf, 0x86c3e873, 0x4a69e8ed,
        0xc4e6ef0e, 0x084cef90, 0x0289e689, 0xce23e617, 0x40ace1f4, 0x8c06e16a,
        0xd0eba0bb, 0x1c41a025, 0x92cea7c6, 0x5e64a758, 0x54a1ae41, 0x980baedf,
        0x1684a93c, 0xda2ea9a2, 0x030ebb0e, 0xcfa4bb90, 0x412bbc73, 0x8d81bced,
        0x8744b5f4, 0x4beeb56a, 0xc561b289, 0x09cbb217, 0xac509190, 0x60fa910e,
        0xee7596ed, 0x22df9673, 0x281a9f6a, 0xe4b09ff4, 0x6a3f9817, 0xa6959889,
        0x7fb58a25, 0xb31f8abb, 0x3d908d58, 0xf13a8dc6, 0xfbff84df, 0x37558441,
        0xb9da83a2, 0x7570833c, 0x533b85da, 0x9f918544, 0x111e82a7, 0xddb48239,
        0xd7718b20, 0x1bdb8bbe, 0x95548c5d, 0x59fe8cc3, 0x80de9e6f, 0x4c749ef1,
        0xc2fb9912, 0x0e51998c, 0x04949095, 0xc83e900b, 0x46b197e8, 0x8a1b9776,
        0x2f80b4f1, 0xe32ab46f, 0x6da5b38c, 0xa10fb312, 0xabcaba0b, 0x6760ba95,
        0xe9efbd76, 0x2545bde8, 0xfc65af44, 0x30cfafda, 0xbe40a839, 0x72eaa8a7,
        0x782fa1be, 0xb485a120, 0x3a0aa6c3, 0xf6a0a65d, 0xaa4de78c, 0x66e7e712,
        0xe868e0f1, 0x24c2e06f, 0x2e07e976, 0xe2ade9e8, 0x6c22ee0b, 0xa088ee95,
        0x79a8fc39, 0xb502fca7, 0x3b8dfb44, 0xf727fbda, 0xfde2f2c3, 0x3148f25d,
        0xbfc7f5be, 0x736df520, 0xd6f6d6a7, 0x1a5cd639, 0x94d3d1da, 0x5879d144,
        0x52bcd85d, 0x9e16d8c3, 0x1099df20, 0xdc33dfbe, 0x0513cd12, 0xc9b9cd8c,
        0x4736ca6f, 0x8b9ccaf1, 0x8159c3e8, 0x4df3c376, 0xc37cc495, 0x0fd6c40b,
        0x7aa64737, 0xb60c47a9, 0x3883404a, 0xf42940d4, 0xfeec49cd, 0x32464953,
        0xbcc94eb0, 0x70634e2e, 0xa9435c82, 0x65e95c1c, 0xeb665bff, 0x27cc5b61,
        0x2d095278, 0xe1a352e6, 0x6f2c5505, 0xa386559b, 0x061d761c, 0xcab77682,
        0x44387161, 0x889271ff, 0x825778e6, 0x4efd7878, 0xc0727f9b, 0x0cd87f05,
        0xd5f86da9, 0x19526d37, 0x97dd6ad4, 0x5b776a4a, 0x51b26353, 0x9d1863cd,
        0x1397642e, 0xdf3d64b0, 0x83d02561, 0x4f7a25ff, 0xc1f5221c, 0x0d5f2282,
        0x079a2b9b, 0xcb302b05, 0x45bf2ce6, 0x89152c78, 0x50353ed4, 0x9c9f3e4a,
        0x121039a9, 0xdeba3937, 0xd47f302e, 0x18d530b0, 0x965a3753, 0x5af037cd,
        0xff6b144a, 0x33c114d4, 0xbd4e1337, 0x71e413a9, 0x7b211ab0, 0xb78b1a2e,
        0x39041dcd, 0xf5ae1d53, 0x2c8e0fff, 0xe0240f61, 0x6eab0882, 0xa201081c,
        0xa8c40105, 0x646e019b, 0xeae10678, 0x264b06e6,
    },
};

// Portable CRC update. Start with 0, the CRC of empty input.
static u32 crc32_slice8(u32 crc, u8 const *p, iz len)
{
    u32 const (*t)[256] = crc32_table;
    crc = ~crc;
    for (; len >= 8; p += 8, len -= 8) {
        u32 lo = crc ^ ((u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24);
        u32 hi = (u32)p[4] | (u32)p[5]<<8 | (u32)p[6]<<16 | (u32)p[7]<<24;
        crc = t[7][lo>> 0 & 0xff] ^ t[6][lo>> 8 & 0xff] ^
              t[5][lo>>16 & 0xff] ^ t[4][lo>>24       ] ^
              t[3][hi>> 0 & 0xff] ^ t[2][hi>> 8 & 0xff] ^
              t[1][hi>>16 & 0xff] ^ t[0][hi>>24       ];
    }
    for (; len; p++, len--) {
        crc = t[0][(crc^*p) & 0xff] ^ crc>>8;
    }
    return ~crc;
}

// CPU features that code compiled for them at run time may use (x86,
// where CPUID tells): a mask kept in a codec's state, zero until asked,
// then CPU_ASKED and the features found. Every feature is asked for at
// once, the first time any is needed, and only then, so that a state
// asks at most once, never per update or stream, and a state that needs
// none never asks. CPUID costs about 100 cycles natively, but a
// hypervisor traps it, at around 700 ns to 1 us (WSL2, virtual
// machines, and Windows 11 with virtualization-based security), which a
// library caller's small updates once paid every time. There is no
// global cache: the core keeps no mutable global state.
enum {
    CPU_ASKED  = 1 << 0,  // alone: asked, nothing found (or not x86)
    CPU_PCLMUL = 1 << 1,  // PCLMULQDQ and SSE2: crc32_pclmul
    CPU_BMI2   = 1 << 2,  // BMI2 (shlx, shrx, bzhi): inflate's fast loop
    CPU_VPCLMUL = 1 << 3, // VPCLMULQDQ, AVX2, YMM state: crc32_vpclmul
};

#if __x86_64__ || __i386__
#include <cpuid.h>

// Ask the CPU: leaf 0 for the highest leaf, then leaf 1, and leaf 7
// unless only PCLMULQDQ is wanted (crc32_stateless), so three CPUIDs, or
// two. On i686, __get_cpuid_max also checks that CPUID exists. The ymm
// registers may be used only if the OS saves them: XGETBV, which exists
// when CPUID says OSXSAVE, reads XCR0, whose SSE and AVX bits must both
// be set. (Windows XP never sets them, so it never takes those paths.)
static i32 cpu_ask(i32 want)
{
    i32 r = CPU_ASKED;
    u32 max = __get_cpuid_max(0, 0);
    u32 a, b, c, d;
    b32 ymm = 0;
    if (max >= 1) {
        __cpuid(1, a, b, c, d);
        if ((c & bit_PCLMUL) && (d & bit_SSE2)) {
            r |= CPU_PCLMUL;
            if ((want & CPU_VPCLMUL) && (c & 1<<27) && (c & 1<<28)) {
                u32 lo, hi;  // OSXSAVE and AVX: XCR0's YMM state
                __asm__ volatile ("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
                ymm = (lo & 6) == 6;
            }
        }
    }
    if ((want & (CPU_BMI2|CPU_VPCLMUL)) && max>=7) {
        __cpuid_count(7, 0, a, b, c, d);
        if (b & 1<<8) {
            r |= CPU_BMI2;
        }
        if (ymm && (b & 1<<5) && (c & 1<<10)) {
            r |= CPU_VPCLMUL;  // AVX2 and VPCLMULQDQ
        }
    }
    return r;
}

// The features in *cpu, asking the CPU only if not yet asked.
[[maybe_unused]] static i32 cpu_features(i32 *cpu)
{
    if (!*cpu) {
        *cpu = cpu_ask(-1);
    }
    return *cpu;
}
#endif

// Update a CRC with more data. Start with 0, the CRC of empty input.
// These are CPU feature tests, not platform tests: ARMv8 has instructions
// for exactly this polynomial, and x86 can fold with carry-less multiply.
// (The SSE4.2 crc32 instruction computes CRC-32C, a different CRC.)
//
// Where the CPU must be asked whether it can (x86, by CPUID), *cpu is the
// caller's feature mask (above), so that the question is asked once per
// state, not once per update.
#if __ARM_FEATURE_CRC32
#include <arm_acle.h>
static u32 crc32_update(u32 crc, u8 const *p, iz len, i32 *cpu)
{
    (void)cpu;
    crc = ~crc;
    for (; len >= 8; p += 8, len -= 8) {
        u64 v = (u64)p[0]     | (u64)p[1]<< 8 | (u64)p[2]<<16 | (u64)p[3]<<24 |
                (u64)p[4]<<32 | (u64)p[5]<<40 | (u64)p[6]<<48 | (u64)p[7]<<56;
        crc = __crc32d(crc, v);
    }
    return crc32_slice8(~crc, p, len);
}
#elif __x86_64__ || __i386__
// Folding with carry-less multiplication (Intel, "Fast CRC Computation
// for Generic Polynomials Using PCLMULQDQ"), with the constants for the
// reflected gzip polynomial as used by zlib. Four 128-bit lanes are
// folded 64 bytes at a time, merged, then Barrett-reduced to 32 bits.
// A constant pair folds a lane forward by d bits: x^(d+32) and x^(d-32)
// mod the polynomial, bit-reflected, times x.
//
// No x86 baseline includes PCLMULQDQ, so these functions are compiled
// for it via the target attribute and selected at run time.
#include <immintrin.h>
#define CRC_LOAD(q)         _mm_loadu_si128((__m128i const *)(q))
#define CRC_CLMUL(a, b, i)  _mm_clmulepi64_si128(a, b, i)
#define CRC_FOLD(x, k, y)   _mm_xor_si128(_mm_xor_si128( \
    CRC_CLMUL(x, k, 0x11), CRC_CLMUL(x, k, 0x00)), y)

// Fold in the remaining 16-byte blocks, reduce, then finish the tail.
[[gnu::always_inline]] __attribute((target("pclmul,sse2")))
static inline u32 crc32_pclmul_end(__m128i x1, u8 const *p, iz len)
{
    __m128i k3k4 = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);  // 128
    __m128i k5   = _mm_set_epi64x(0,           0x163cd6124);
    __m128i poly = _mm_set_epi64x(0x1f7011641, 0x1db710641);
    __m128i mask = _mm_setr_epi32(-1, 0, -1, 0);
    for (; len >= 16; p += 16, len -= 16) {
        x1 = CRC_FOLD(x1, k3k4, CRC_LOAD(p));
    }

    // Reduce 128 bits to 64, then Barrett reduction to 32
    __m128i t = CRC_CLMUL(x1, k3k4, 0x10);
    x1 = _mm_xor_si128(_mm_srli_si128(x1, 8), t);
    t  = _mm_srli_si128(x1, 4);
    x1 = _mm_xor_si128(CRC_CLMUL(_mm_and_si128(x1, mask), k5, 0x00), t);
    t  = CRC_CLMUL(_mm_and_si128(x1, mask), poly, 0x10);
    t  = CRC_CLMUL(_mm_and_si128(t, mask), poly, 0x00);
    x1 = _mm_xor_si128(x1, t);
    u32 crc = (u32)_mm_cvtsi128_si32(_mm_srli_si128(x1, 4));
    return crc32_slice8(~crc, p, len);
}

__attribute((target("pclmul,sse2")))
static u32 crc32_pclmul(u32 crc, u8 const *p, iz len)
{
    assert(len >= 64);
    __m128i k1k2 = _mm_set_epi64x(0x1c6e41596, 0x154442bd4);  // 512
    __m128i k3k4 = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);  // 128

    __m128i x1 = _mm_xor_si128(CRC_LOAD(p), _mm_cvtsi32_si128((i32)~crc));
    __m128i x2 = CRC_LOAD(p+16);
    __m128i x3 = CRC_LOAD(p+32);
    __m128i x4 = CRC_LOAD(p+48);
    p += 64;
    len -= 64;

    // Fold 512 bits forward by 512 bits per step
    for (; len >= 64; p += 64, len -= 64) {
        __m128i t1 = CRC_CLMUL(x1, k1k2, 0x00);
        __m128i t2 = CRC_CLMUL(x2, k1k2, 0x00);
        __m128i t3 = CRC_CLMUL(x3, k1k2, 0x00);
        __m128i t4 = CRC_CLMUL(x4, k1k2, 0x00);
        x1 = _mm_xor_si128(_mm_xor_si128(CRC_CLMUL(x1, k1k2, 0x11), t1), CRC_LOAD(p+ 0));
        x2 = _mm_xor_si128(_mm_xor_si128(CRC_CLMUL(x2, k1k2, 0x11), t2), CRC_LOAD(p+16));
        x3 = _mm_xor_si128(_mm_xor_si128(CRC_CLMUL(x3, k1k2, 0x11), t3), CRC_LOAD(p+32));
        x4 = _mm_xor_si128(_mm_xor_si128(CRC_CLMUL(x4, k1k2, 0x11), t4), CRC_LOAD(p+48));
    }

    // Merge the four lanes
    x1 = CRC_FOLD(x1, k3k4, x2);
    x1 = CRC_FOLD(x1, k3k4, x3);
    x1 = CRC_FOLD(x1, k3k4, x4);
    return crc32_pclmul_end(x1, p, len);
}

// The same with VPCLMULQDQ on 256-bit lanes: four of them, 128 bytes a
// step, merged pairwise down to one 128-bit lane. Used only for long
// enough updates (CRC32_YMM_MIN), where the ymm setup and the merge pay.
#define CRC_YLOAD(q)        _mm256_loadu_si256((__m256i const *)(q))
#define CRC_YCLMUL(a, b, i) _mm256_clmulepi64_epi128(a, b, i)
#define CRC_YFOLD(x, k, y)  _mm256_xor_si256(_mm256_xor_si256( \
    CRC_YCLMUL(x, k, 0x11), CRC_YCLMUL(x, k, 0x00)), y)
#define CRC_YK(lo, hi)      _mm256_set_epi64x(hi, lo, hi, lo)
#define CRC32_YMM_MIN 128

__attribute((target("vpclmulqdq,avx2,pclmul")))
static u32 crc32_vpclmul(u32 crc, u8 const *p, iz len)
{
    assert(len >= 128);
    __m256i k1024 = CRC_YK(0x1e88ef372, 0x14a7fe880);
    __m256i k512  = CRC_YK(0x154442bd4, 0x1c6e41596);
    __m256i k256  = CRC_YK(0x0f1da05aa, 0x15a546366);
    __m128i k3k4  = _mm_set_epi64x(0x0ccaa009e, 0x1751997d0);

    __m256i y0 = _mm256_setr_epi32((i32)~crc, 0, 0, 0, 0, 0, 0, 0);
    __m256i y1 = _mm256_xor_si256(CRC_YLOAD(p), y0);
    __m256i y2 = CRC_YLOAD(p+32);
    __m256i y3 = CRC_YLOAD(p+64);
    __m256i y4 = CRC_YLOAD(p+96);
    p += 128;
    len -= 128;

    // Fold 1024 bits forward by 1024 bits per step
    for (; len >= 128; p += 128, len -= 128) {
        y1 = CRC_YFOLD(y1, k1024, CRC_YLOAD(p+ 0));
        y2 = CRC_YFOLD(y2, k1024, CRC_YLOAD(p+32));
        y3 = CRC_YFOLD(y3, k1024, CRC_YLOAD(p+64));
        y4 = CRC_YFOLD(y4, k1024, CRC_YLOAD(p+96));
    }

    // Merge to two lanes, then one, taking in what fits along the way
    y1 = CRC_YFOLD(y1, k512, y3);
    y2 = CRC_YFOLD(y2, k512, y4);
    if (len >= 64) {
        y1 = CRC_YFOLD(y1, k512, CRC_YLOAD(p+ 0));
        y2 = CRC_YFOLD(y2, k512, CRC_YLOAD(p+32));
        p += 64;
        len -= 64;
    }
    y1 = CRC_YFOLD(y1, k256, y2);
    if (len >= 32) {
        y1 = CRC_YFOLD(y1, k256, CRC_YLOAD(p));
        p += 32;
        len -= 32;
    }
    __m128i lo = _mm256_castsi256_si128(y1);
    __m128i hi = _mm256_extracti128_si256(y1, 1);
    return crc32_pclmul_end(CRC_FOLD(lo, k3k4, hi), p, len);
}

// Asks the CPU through *cpu, unless the target always has the
// instructions (-mpclmul, -mvpclmulqdq -mavx2, or a -march with them),
// when there is nothing to ask.
static u32 crc32_update(u32 crc, u8 const *p, iz len, i32 *cpu)
{
    if (len < 64) {
        return crc32_slice8(crc, p, len);
    }
#if __VPCLMULQDQ__ && __AVX2__
    (void)cpu;
    if (len >= CRC32_YMM_MIN) {
        return crc32_vpclmul(crc, p, len);
    }
    return crc32_pclmul(crc, p, len);
#else
#  if __PCLMUL__ && __SSE2__
    if (len < CRC32_YMM_MIN) {
        return crc32_pclmul(crc, p, len);
    }
#  endif
    i32 f = cpu_features(cpu);
    if (len>=CRC32_YMM_MIN && (f & CPU_VPCLMUL)) {
        return crc32_vpclmul(crc, p, len);
    } else if (f & CPU_PCLMUL) {
        return crc32_pclmul(crc, p, len);
    }
#  if __PCLMUL__ && __SSE2__
    return crc32_pclmul(crc, p, len);
#  else
    return crc32_slice8(crc, p, len);
#  endif
#endif
}
#else
static u32 crc32_update(u32 crc, u8 const *p, iz len, i32 *cpu)
{
    (void)cpu;
    return crc32_slice8(crc, p, len);
}
#endif

// Update a CRC for a caller that keeps no state, as the library's
// tugz_crc32 does not, where *cpu must be asked anew each time. It is
// asked only for updates long enough to repay the question even where a
// hypervisor traps it: on an i9-12900 under Windows 11 with
// virtualization-based security CPUID took 1 us, slicing-by-8 0.46 ns
// per byte, and PCLMUL 0.05 (at 4 KiB, asking and folding took 1.6 us,
// the table 1.9 us; at 2 KiB, 1.2 and 1.0). Shorter updates take the
// table, CPU_ASKED alone meaning "none" to crc32_update, unless the
// target always has the instructions (ARMv8 CRC, or PCLMUL as above).
// Longer ones ask only for PCLMULQDQ, which takes one CPUID fewer, up
// to CRC32_ASK_YMM_MIN, from which the third CPUID (and XGETBV) for
// VPCLMULQDQ pays: at 32 KiB, asking for both and folding with ymm took
// 1.37 us, asking for PCLMULQDQ alone and folding 1.49 us.
#define CRC32_ASK_MIN     4096
#define CRC32_ASK_YMM_MIN 32768

[[maybe_unused]] static u32 crc32_stateless(u32 crc, u8 const *p, iz len)
{
    i32 cpu = CPU_ASKED;
#if (__x86_64__ || __i386__) && !(__VPCLMULQDQ__ && __AVX2__)
    if (len >= CRC32_ASK_YMM_MIN) {
        cpu = cpu_ask(CPU_PCLMUL | CPU_VPCLMUL);
#  if !(__PCLMUL__ && __SSE2__)
    } else if (len >= CRC32_ASK_MIN) {
        cpu = cpu_ask(CPU_PCLMUL);
#  endif
    }
#endif
    return crc32_update(crc, p, len, &cpu);
}

// tugz core: Adler-32 checksum (RFC 1950)

#define ADLER_MOD   65521
#define ADLER_NMAX  5552  // most bytes before the sums may overflow 32 bits

static u32 adler32_scalar(u32 adler, u8 const *p, iz len)
{
    u32 a = adler & 0xffff;
    u32 b = adler >> 16;
    while (len) {
        iz n = MIN(len, ADLER_NMAX);
        len -= n;
        for (; n >= 16; n -= 16, p += 16) {
            for (i32 i = 0; i < 16; i++) {
                a += p[i];
                b += a;
            }
        }
        for (; n; n--) {
            a += *p++;
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return a | b<<16;
}

#if __SSE2__
// SSE2, baseline on x86-64 (i686 keeps the scalar loop): 32 bytes per
// step. Over a block of n bytes, a byte k bytes in adds itself to a and
// (n - k) times itself to b, and the old a adds n times to b. psadbw sums
// each step's bytes into a; b gets 32 times the sum of a's lanes before
// each step, plus each position's bytes, summed in 16-bit lanes, times
// its weight, 32 down to 1, by pmaddwd. Those are signed, so a block has
// at most 128 steps (128*255 < 32768), well within ADLER_NMAX.
#include <emmintrin.h>
static u32 adler32_update(u32 adler, u8 const *p, iz len)
{
    u32 a = adler & 0xffff;
    u32 b = adler >> 16;
    __m128i const zero = _mm_setzero_si128();
    __m128i const w1 = _mm_setr_epi16(32, 31, 30, 29, 28, 27, 26, 25);
    __m128i const w2 = _mm_setr_epi16(24, 23, 22, 21, 20, 19, 18, 17);
    __m128i const w3 = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10,  9);
    __m128i const w4 = _mm_setr_epi16( 8,  7,  6,  5,  4,  3,  2,  1);
    while (len >= 32) {
        iz steps = MIN(len/32, 128);
        b += a * (u32)(steps*32);
        __m128i s1 = zero, s2 = zero;
        __m128i c1 = zero, c2 = zero, c3 = zero, c4 = zero;
        for (iz i = 0; i < steps; i++, p += 32) {
            __m128i x = _mm_loadu_si128((__m128i const *)p);
            __m128i y = _mm_loadu_si128((__m128i const *)(p + 16));
            s2 = _mm_add_epi32(s2, s1);
            s1 = _mm_add_epi32(s1, _mm_add_epi32(_mm_sad_epu8(x, zero),
                                                 _mm_sad_epu8(y, zero)));
            c1 = _mm_add_epi16(c1, _mm_unpacklo_epi8(x, zero));
            c2 = _mm_add_epi16(c2, _mm_unpackhi_epi8(x, zero));
            c3 = _mm_add_epi16(c3, _mm_unpacklo_epi8(y, zero));
            c4 = _mm_add_epi16(c4, _mm_unpackhi_epi8(y, zero));
        }
        len -= steps*32;
        s2 = _mm_slli_epi32(s2, 5);
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(c1, w1),
                                             _mm_madd_epi16(c2, w2)));
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(c3, w3),
                                             _mm_madd_epi16(c4, w4)));
        s1 = _mm_add_epi32(s1, _mm_shuffle_epi32(s1, 0x4e));
        s1 = _mm_add_epi32(s1, _mm_shuffle_epi32(s1, 0xb1));
        s2 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, 0x4e));
        s2 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, 0xb1));
        a += (u32)_mm_cvtsi128_si32(s1);
        b += (u32)_mm_cvtsi128_si32(s2);
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return adler32_scalar(a | b<<16, p, len);
}
#else
static u32 adler32_update(u32 adler, u8 const *p, iz len)
{
    return adler32_scalar(adler, p, len);
}
#endif

// tugz core: raw DEFLATE decoder (RFC 1951), and Deflate64
//
// Accepts exactly the streams zlib accepts: incomplete Huffman codes are
// rejected except for a lone 1-bit code, and an empty distance code is
// only an error if a distance is actually decoded.
//
// Decoding is table-driven, with a fast loop that runs while plenty of
// input and output space remain, refilling the bit buffer 64 bits at a
// time. Near buffer edges a careful byte-at-a-time path takes over.
// Output is produced directly into a window that retains 32 KiB of
// history, from which it is handed out as the window fills.
//
// The caller supplies input and output buffers of any size, and decoding
// resumes wherever the previous call stopped.
//
// Deflate64, PKWARE's "Enhanced Deflating" (ZIP method 9), differs only
// in a 64 KiB window, distance codes 30 and 31 (base 32769 and 49153,
// 14 extra bits), and length code 285 taking 16 extra bits over a base
// of 3 (lengths 3 to 65538) rather than meaning 258. Its inflator, from
// inflate64_new, has a window with 64 KiB of history and room for its
// longest match. The fast loop is compiled once for each format, with
// the format a constant, so Deflate's is as if Deflate64 did not exist.
//
// Window invariant: until history first slides, wpos equals the total
// output so far. Afterwards wpos >= the history size, which no distance
// exceeds. So a distance is too far back exactly when it exceeds wpos.

#define INF_HIST      32768
#define INF64_HIST    65536
#define INF_CHUNK     (1 << 18)
#define INF_SLACK     (258 + 32)    // room for one match plus wide-copy overrun
#define INF64_SLACK   (65538 + 32)
#define INF_WINCAP    (INF_HIST + INF_CHUNK + INF_SLACK)
#define INF64_WINCAP  (INF64_HIST + INF_CHUNK + INF64_SLACK)
#define LIT_ROOT      11
#define DIST_ROOT     8
#define LIT_ENOUGH    2342  // zlib's enough for 286 symbols, 11-bit root
#define DIST_ENOUGH   402   // zlib's enough for 30 symbols, 8-bit root

// Deflate64's 32 distance symbols, 8-bit root: the root's 256 entries
// plus subtables. A subtable of 2^k entries holds a complete subtree at
// least k deep, so at least k+1 codes, and k <= 15-8. With 32 codes,
// at most four subtables of 128 entries.
#define DIST64_ENOUGH (256 + 4*128)

enum {
    HUFF_CODELEN,
    HUFF_LITLEN,
    HUFF_DIST,
    HUFF_LITLEN64,  // Deflate64's
    HUFF_DIST64,
};

// Table entry layout:
//   bits  0..4   total bits: code length plus extra bits
//   bit   5      F_HI: add 32768 to the value (Deflate64 distances)
//   bit   6      F_LINK: subtable link
//   bit   7      F_LIT: literal
//   bits  8..11  code length (root bits for a subtable link)
//   bits 12..15  subtable index bits (links only)
//   bits 16..30  value: literal, base length/distance, or subtable offset
//   bit  30      F_EOB: end of block (only with F_SPECIAL)
//   bit  31      F_SPECIAL: end of block or invalid code
// Length and distance entries have no flags, but F_HI for Deflate64's
// distances past 32768. Flags make the common tests single-bit tests.
// Subtable entries hold the full code length, so a lookup never needs to
// consume the root bits separately.
enum {
    ENT_LIT,
    ENT_LEN,  // also distances
    ENT_EOB,
    ENT_SUB,
    ENT_BAD,
};
#define F_HI        (1u << 5)
#define F_LINK      (1u << 6)
#define F_LIT       (1u << 7)
#define F_EOB       (1u << 30)
#define F_SPECIAL   (1u << 31)
#define ENT_FLAGS(kind) \
    ((kind)==ENT_LIT ? F_LIT : (kind)==ENT_SUB ? F_LINK : \
     (kind)==ENT_EOB ? F_SPECIAL|F_EOB : (kind)==ENT_BAD ? F_SPECIAL : 0)
#define ENT(len, kind, extra, val) \
    ((u32)((len) + (extra)) | ENT_FLAGS(kind) | (u32)(len)<<8 | (u32)(val)<<16)
#define ENT_LINK(root, bits, off) \
    (F_LINK | (u32)(root)<<8 | (u32)(bits)<<12 | (u32)(off)<<16)
#define ENT_TOTAL(e)    ((i32)((e) & 31))
#define ENT_CODELEN(e)  ((i32)((e)>>8 & 15))
#define ENT_SUBBITS(e)  ((i32)((e)>>12 & 15))
#define ENT_EXTRA(e)    (ENT_TOTAL(e) - ENT_CODELEN(e))
#define ENT_VAL(e)      ((i32)((e)>>16 & 0x7fff))
#define ENT_VAL64(e)    (ENT_VAL(e) | (i32)((e) & F_HI)<<10)
#define ENT_KIND(e) \
    ((e) & F_LINK ? ENT_SUB : (e) & F_LIT ? ENT_LIT : \
     !((e) & F_SPECIAL) ? ENT_LEN : (e) & F_EOB ? ENT_EOB : ENT_BAD)

typedef struct {
    u32 const *entries;
    u32        mask;
} htable;

enum {
    INF_HEAD,     // next is a block header
    INF_LENS,     // reading a dynamic block's code lengths
    INF_STORED,   // copying stored block data
    INF_SYMBOLS,  // decoding a compressed block
    INF_END,      // the final block has ended
};

// Decoding proceeds in atomic units: a block header (for a dynamic
// block, up to its code lengths), a run of a dynamic block's code
// lengths, or one literal or length/distance pair. When input runs out
// partway through a unit, it is rolled back and its bytes are saved in
// the stash, to be completed by the next call's input. A unit that runs
// out needs at most 10 bytes (a dynamic block header's 74 bits; a run of
// code lengths runs out only in its first), which the stash covers.
// Since the code lengths resume where they stopped, as zlib's do, input
// in tiny pieces costs no more per byte than it otherwise would.
//
// Input invariant: between calls the bit buffer holds fewer than 8
// bits. Whole bytes are always given back to the input, so the stream
// end is reported exactly, and the stash only ever holds bytes of one
// incomplete unit.
#define INF_STASH   32

typedef struct {
    u8 const *in;     // input cursor for the current call
    u8 const *inend;

    u64 bitbuf;   // bits above bitcnt are zero or upcoming input
    i32 bitcnt;
    i32 err;      // sticky error, or GZ_NEEDIN while a unit is starved

    i32 state;
    b32 final;
    b32 d64;      // Deflate64
    b32 wide;     // the block's tables have full roots: see htable_widen
    iz  stored;   // stored block bytes remaining
    htable lt;    // the current block's codes: fixed, or in the entries
    htable dt;

    u8 *win;
    iz  wpos;
    iz  wflushed; // output before this has been handed out

    iz  stashlen;
    u8  stash[INF_STASH];

    // A dynamic block's code descriptions, while being read
    htable cl;      // code length code
    i32 nlit;       // literal/length code lengths, then distance ones
    i32 nlens;      // total
    i32 nread;      // read so far
    u16 lens[286+32];
    u32 cl_entries[128];

    u32  lit_entries[LIT_ENOUGH];
    u32 *dist_entries;  // DIST_ENOUGH, or DIST64_ENOUGH for Deflate64

    // The CPU's features (crc32.c), asked when the fast loop first runs,
    // and kept across resets. A gzip or zlib decoder's check shares it.
    i32 cpu;
} inflator;

static u16 const inf_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static u8 const inf_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
// Distance codes 30 and 31 are Deflate64's alone
static u16 const inf_dist_base[32] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
    32769, 49153
};
static u8 const inf_dist_extra[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13, 14, 14
};
static u8 const inf_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Decoding tables for the fixed codes (RFC 1951, 3.2.6), exactly as
// htable_build and htable_widen make them from the fixed code lengths:
// 11-bit and 8-bit roots, for the fast loop's constant masks, the latter
// with distance codes 30 and 31 invalid. No fixed code is longer than 9
// or 5 bits, so each is its first 512 or 32 entries over again. As
// constants they cost an inflator nothing to set up. test_tables in
// test/tests.c checks them against htable_build and htable_widen.
#define INF_FIXLIT9 \
    0xc0000707, 0x00500888, 0x00100888, 0x0073080c, 0x001f0709, 0x00700888, \
    0x00300888, 0x00c00989, 0x000a0707, 0x00600888, 0x00200888, 0x00a00989, \
    0x00000888, 0x00800888, 0x00400888, 0x00e00989, 0x00060707, 0x00580888, \
    0x00180888, 0x00900989, 0x003b070a, 0x00780888, 0x00380888, 0x00d00989, \
    0x00110708, 0x00680888, 0x00280888, 0x00b00989, 0x00080888, 0x00880888, \
    0x00480888, 0x00f00989, 0x00040707, 0x00540888, 0x00140888, 0x00e3080d, \
    0x002b070a, 0x00740888, 0x00340888, 0x00c80989, 0x000d0708, 0x00640888, \
    0x00240888, 0x00a80989, 0x00040888, 0x00840888, 0x00440888, 0x00e80989, \
    0x00080707, 0x005c0888, 0x001c0888, 0x00980989, 0x0053070b, 0x007c0888, \
    0x003c0888, 0x00d80989, 0x00170709, 0x006c0888, 0x002c0888, 0x00b80989, \
    0x000c0888, 0x008c0888, 0x004c0888, 0x00f80989, 0x00030707, 0x00520888, \
    0x00120888, 0x00a3080d, 0x0023070a, 0x00720888, 0x00320888, 0x00c40989, \
    0x000b0708, 0x00620888, 0x00220888, 0x00a40989, 0x00020888, 0x00820888, \
    0x00420888, 0x00e40989, 0x00070707, 0x005a0888, 0x001a0888, 0x00940989, \
    0x0043070b, 0x007a0888, 0x003a0888, 0x00d40989, 0x00130709, 0x006a0888, \
    0x002a0888, 0x00b40989, 0x000a0888, 0x008a0888, 0x004a0888, 0x00f40989, \
    0x00050707, 0x00560888, 0x00160888, 0x80000808, 0x0033070a, 0x00760888, \
    0x00360888, 0x00cc0989, 0x000f0708, 0x00660888, 0x00260888, 0x00ac0989, \
    0x00060888, 0x00860888, 0x00460888, 0x00ec0989, 0x00090707, 0x005e0888, \
    0x001e0888, 0x009c0989, 0x0063070b, 0x007e0888, 0x003e0888, 0x00dc0989, \
    0x001b0709, 0x006e0888, 0x002e0888, 0x00bc0989, 0x000e0888, 0x008e0888, \
    0x004e0888, 0x00fc0989, 0xc0000707, 0x00510888, 0x00110888, 0x0083080d, \
    0x001f0709, 0x00710888, 0x00310888, 0x00c20989, 0x000a0707, 0x00610888, \
    0x00210888, 0x00a20989, 0x00010888, 0x00810888, 0x00410888, 0x00e20989, \
    0x00060707, 0x00590888, 0x00190888, 0x00920989, 0x003b070a, 0x00790888, \
    0x00390888, 0x00d20989, 0x00110708, 0x00690888, 0x00290888, 0x00b20989, \
    0x00090888, 0x00890888, 0x00490888, 0x00f20989, 0x00040707, 0x00550888, \
    0x00150888, 0x01020808, 0x002b070a, 0x00750888, 0x00350888, 0x00ca0989, \
    0x000d0708, 0x00650888, 0x00250888, 0x00aa0989, 0x00050888, 0x00850888, \
    0x00450888, 0x00ea0989, 0x00080707, 0x005d0888, 0x001d0888, 0x009a0989, \
    0x0053070b, 0x007d0888, 0x003d0888, 0x00da0989, 0x00170709, 0x006d0888, \
    0x002d0888, 0x00ba0989, 0x000d0888, 0x008d0888, 0x004d0888, 0x00fa0989, \
    0x00030707, 0x00530888, 0x00130888, 0x00c3080d, 0x0023070a, 0x00730888, \
    0x00330888, 0x00c60989, 0x000b0708, 0x00630888, 0x00230888, 0x00a60989, \
    0x00030888, 0x00830888, 0x00430888, 0x00e60989, 0x00070707, 0x005b0888, \
    0x001b0888, 0x00960989, 0x0043070b, 0x007b0888, 0x003b0888, 0x00d60989, \
    0x00130709, 0x006b0888, 0x002b0888, 0x00b60989, 0x000b0888, 0x008b0888, \
    0x004b0888, 0x00f60989, 0x00050707, 0x00570888, 0x00170888, 0x80000808, \
    0x0033070a, 0x00770888, 0x00370888, 0x00ce0989, 0x000f0708, 0x00670888, \
    0x00270888, 0x00ae0989, 0x00070888, 0x00870888, 0x00470888, 0x00ee0989, \
    0x00090707, 0x005f0888, 0x001f0888, 0x009e0989, 0x0063070b, 0x007f0888, \
    0x003f0888, 0x00de0989, 0x001b0709, 0x006f0888, 0x002f0888, 0x00be0989, \
    0x000f0888, 0x008f0888, 0x004f0888, 0x00fe0989, 0xc0000707, 0x00500888, \
    0x00100888, 0x0073080c, 0x001f0709, 0x00700888, 0x00300888, 0x00c10989, \
    0x000a0707, 0x00600888, 0x00200888, 0x00a10989, 0x00000888, 0x00800888, \
    0x00400888, 0x00e10989, 0x00060707, 0x00580888, 0x00180888, 0x00910989, \
    0x003b070a, 0x00780888, 0x00380888, 0x00d10989, 0x00110708, 0x00680888, \
    0x00280888, 0x00b10989, 0x00080888, 0x00880888, 0x00480888, 0x00f10989, \
    0x00040707, 0x00540888, 0x00140888, 0x00e3080d, 0x002b070a, 0x00740888, \
    0x00340888, 0x00c90989, 0x000d0708, 0x00640888, 0x00240888, 0x00a90989, \
    0x00040888, 0x00840888, 0x00440888, 0x00e90989, 0x00080707, 0x005c0888, \
    0x001c0888, 0x00990989, 0x0053070b, 0x007c0888, 0x003c0888, 0x00d90989, \
    0x00170709, 0x006c0888, 0x002c0888, 0x00b90989, 0x000c0888, 0x008c0888, \
    0x004c0888, 0x00f90989, 0x00030707, 0x00520888, 0x00120888, 0x00a3080d, \
    0x0023070a, 0x00720888, 0x00320888, 0x00c50989, 0x000b0708, 0x00620888, \
    0x00220888, 0x00a50989, 0x00020888, 0x00820888, 0x00420888, 0x00e50989, \
    0x00070707, 0x005a0888, 0x001a0888, 0x00950989, 0x0043070b, 0x007a0888, \
    0x003a0888, 0x00d50989, 0x00130709, 0x006a0888, 0x002a0888, 0x00b50989, \
    0x000a0888, 0x008a0888, 0x004a0888, 0x00f50989, 0x00050707, 0x00560888, \
    0x00160888, 0x80000808, 0x0033070a, 0x00760888, 0x00360888, 0x00cd0989, \
    0x000f0708, 0x00660888, 0x00260888, 0x00ad0989, 0x00060888, 0x00860888, \
    0x00460888, 0x00ed0989, 0x00090707, 0x005e0888, 0x001e0888, 0x009d0989, \
    0x0063070b, 0x007e0888, 0x003e0888, 0x00dd0989, 0x001b0709, 0x006e0888, \
    0x002e0888, 0x00bd0989, 0x000e0888, 0x008e0888, 0x004e0888, 0x00fd0989, \
    0xc0000707, 0x00510888, 0x00110888, 0x0083080d, 0x001f0709, 0x00710888, \
    0x00310888, 0x00c30989, 0x000a0707, 0x00610888, 0x00210888, 0x00a30989, \
    0x00010888, 0x00810888, 0x00410888, 0x00e30989, 0x00060707, 0x00590888, \
    0x00190888, 0x00930989, 0x003b070a, 0x00790888, 0x00390888, 0x00d30989, \
    0x00110708, 0x00690888, 0x00290888, 0x00b30989, 0x00090888, 0x00890888, \
    0x00490888, 0x00f30989, 0x00040707, 0x00550888, 0x00150888, 0x01020808, \
    0x002b070a, 0x00750888, 0x00350888, 0x00cb0989, 0x000d0708, 0x00650888, \
    0x00250888, 0x00ab0989, 0x00050888, 0x00850888, 0x00450888, 0x00eb0989, \
    0x00080707, 0x005d0888, 0x001d0888, 0x009b0989, 0x0053070b, 0x007d0888, \
    0x003d0888, 0x00db0989, 0x00170709, 0x006d0888, 0x002d0888, 0x00bb0989, \
    0x000d0888, 0x008d0888, 0x004d0888, 0x00fb0989, 0x00030707, 0x00530888, \
    0x00130888, 0x00c3080d, 0x0023070a, 0x00730888, 0x00330888, 0x00c70989, \
    0x000b0708, 0x00630888, 0x00230888, 0x00a70989, 0x00030888, 0x00830888, \
    0x00430888, 0x00e70989, 0x00070707, 0x005b0888, 0x001b0888, 0x00970989, \
    0x0043070b, 0x007b0888, 0x003b0888, 0x00d70989, 0x00130709, 0x006b0888, \
    0x002b0888, 0x00b70989, 0x000b0888, 0x008b0888, 0x004b0888, 0x00f70989, \
    0x00050707, 0x00570888, 0x00170888, 0x80000808, 0x0033070a, 0x00770888, \
    0x00370888, 0x00cf0989, 0x000f0708, 0x00670888, 0x00270888, 0x00af0989, \
    0x00070888, 0x00870888, 0x00470888, 0x00ef0989, 0x00090707, 0x005f0888, \
    0x001f0888, 0x009f0989, 0x0063070b, 0x007f0888, 0x003f0888, 0x00df0989, \
    0x001b0709, 0x006f0888, 0x002f0888, 0x00bf0989, 0x000f0888, 0x008f0888, \
    0x004f0888, 0x00ff0989
static u32 const inf_fixlit[2048] = {
    INF_FIXLIT9, INF_FIXLIT9, INF_FIXLIT9, INF_FIXLIT9,
};
#undef INF_FIXLIT9
#define INF_FIXDIST5 \
    0x00010505, 0x0101050c, 0x00110508, 0x10010510, 0x00050506, 0x0401050e, \
    0x0041050a, 0x40010512, 0x00030505, 0x0201050d, 0x00210509, 0x20010511, \
    0x00090507, 0x0801050f, 0x0081050b, 0x80000505, 0x00020505, 0x0181050c, \
    0x00190508, 0x18010510, 0x00070506, 0x0601050e, 0x0061050a, 0x60010512, \
    0x00040505, 0x0301050d, 0x00310509, 0x30010511, 0x000d0507, 0x0c01050f, \
    0x00c1050b, 0x80000505
static u32 const inf_fixdist[256] = {
    INF_FIXDIST5, INF_FIXDIST5, INF_FIXDIST5, INF_FIXDIST5,
    INF_FIXDIST5, INF_FIXDIST5, INF_FIXDIST5, INF_FIXDIST5,
};
#undef INF_FIXDIST5

static u32 sym_entry(i32 sym, i32 len, i32 kind)
{
    switch (kind) {
    case HUFF_CODELEN:
        return ENT(len, ENT_LIT, 0, sym);
    case HUFF_LITLEN64:
        if (sym == 285) {
            return ENT(len, ENT_LEN, 16, 3);
        }
        [[fallthrough]];
    case HUFF_LITLEN:
        if (sym < 256) {
            return ENT(len, ENT_LIT, 0, sym);
        } else if (sym == 256) {
            return ENT(len, ENT_EOB, 0, 0);
        } else if (sym < 286) {
            sym -= 257;
            return ENT(len, ENT_LEN, inf_len_extra[sym], inf_len_base[sym]);
        }
        break;
    case HUFF_DIST:
    case HUFF_DIST64:
        if (sym < (kind==HUFF_DIST ? 30 : 32)) {
            u32 base = inf_dist_base[sym];
            return ENT(len, ENT_LEN, inf_dist_extra[sym], base & 0x7fff) |
                   (base>>15) * F_HI;
        }
        break;
    }
    return ENT(len, ENT_BAD, 0, 0);
}

// The bit-reversed code after a bit-reversed code of len bits.
static u32 huff_next(u32 huff, i32 len)
{
    u32 incr = 1u << (len - 1);
    for (; huff & incr; incr >>= 1) {}
    return incr ? (huff & (incr - 1)) + incr : 0;
}

// Build a decoding table from code lengths, returning false if the
// lengths do not describe a code that zlib would accept: a root table
// indexed by the low bits, with subtables for longer codes, adapted
// from zlib's inflate_table. The root is rootbits wide, or narrower
// when every code is shorter (see htable_widen).
//
// Codes no longer than the root are laid out as libdeflate lays them
// out: each code's entry is stored once, in a table as wide as the code
// is long, and the table doubles, copying itself, before each longer
// length, so that the copies fill every entry that begins with a code,
// rather than each code's entries being stored one at a time.
static b32 htable_build(htable *t, u32 *entries, iz cap, u16 const *lens,
                        i32 n, i32 kind, i32 rootbits)
{
    u16 count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        count[lens[i]]++;
    }
    i32 max = 15;
    for (; max && !count[max]; max--) {}
    t->entries = entries;

    if (!max) {
        // Nothing can be decoded, which is only allowed for distances.
        // Like zlib, an empty code length code decodes every length as a
        // 1-bit zero, failing later on the missing end-of-block code.
        t->mask = 1;
        entries[0] = entries[1] = kind==HUFF_CODELEN ? ENT(1, ENT_LIT, 0, 0)
                                                     : ENT(1, ENT_BAD, 0, 0);
        return kind!=HUFF_LITLEN && kind!=HUFF_LITLEN64;
    }

    i32 left = 1;
    for (i32 len = 1; len < 16; len++) {
        left = (left << 1) - count[len];
        if (left < 0) {
            return 0;  // over-subscribed
        }
    }
    if (left>0 && (kind==HUFF_CODELEN || max!=1)) {
        return 0;  // incomplete
    }

    u16 offs[16];
    offs[1] = 0;
    for (i32 len = 1; len < 15; len++) {
        offs[len+1] = (u16)(offs[len] + count[len]);
    }
    u16 work[288];
    for (i32 i = 0; i < n; i++) {
        if (lens[i]) {
            work[offs[lens[i]]++] = (u16)i;
        }
    }

    i32 ncodes = n - count[0];
    i32 root = MIN(rootbits, max);
    t->mask = (1u << root) - 1;

    // Codes up to the root, in entries [0, width) so far. An entry that a
    // longer code will take may meanwhile hold another's copy, or garbage,
    // which that code's own store, and the copies after it, replace.
    i32 len = 1;
    for (; !count[len]; len++) {}
    i32 sym   = 0;
    iz  width = (iz)1 << len;
    u32 huff  = 0;  // bit-reversed code of the current symbol
    for (; sym<ncodes && (len = lens[work[sym]])<=root; sym++) {
        for (; width < (iz)1<<len; width *= 2) {
            bytecopy(entries+width, entries, width*(iz)sizeof(u32));
        }
        entries[huff] = sym_entry(work[sym], len, kind);
        count[len]--;
        huff = huff_next(huff, len);
    }
    if (huff && sym==ncodes) {
        // An incomplete code is a lone 1-bit code with an unused codeword
        entries[huff] = ENT(len, ENT_BAD, 0, 0);
    }
    for (; width < (iz)1<<root; width *= 2) {
        bytecopy(entries+width, entries, width*(iz)sizeof(u32));
    }

    // Longer codes, in subtables as zlib builds them: codes that share
    // their low root bits share a subtable, as wide as they need, which
    // their root entry links, and each fills its entries there.
    u32 *next = entries;
    u32  size = 1u << root;  // the current (sub)table's
    i32  curr = root;
    u32  low  = (u32)-1;
    iz   used = (iz)1 << root;
    for (; sym < ncodes; sym++) {
        len = lens[work[sym]];
        if ((huff & t->mask) != low) {
            // A new subtable, as long as the codes that share its root
            // entry need
            next += size;
            curr = len - root;
            i32 avail = 1 << curr;
            for (; curr+root < max; curr++, avail <<= 1) {
                avail -= count[curr + root];
                if (avail <= 0) {
                    break;
                }
            }
            size = 1u << curr;
            used += size;
            assert(used <= cap);
            low = huff & t->mask;
            entries[low] = ENT_LINK(root, curr, next - entries);
        }
        u32 here = sym_entry(work[sym], len, kind);
        u32 incr = 1u << (len - root);
        u32 fill = size;
        do {
            fill -= incr;
            next[(huff >> root) + fill] = here;
        } while (fill);
        count[len]--;
        huff = huff_next(huff, len);
    }
    return 1;
}

// Widen a table's root to rootbits, doubling it by copies, so that the
// fast loop's masks may be constants. A root narrower than that has no
// subtables, all its codes being shorter, so the copies are all there
// is. A small block's codes are short, and its tables would cost more
// to widen than its symbols save, so only blocks whose literal/length
// codes reach the root are widened (and Deflate64's always); the rest
// take a loop with their masks (decode_narrow).
static void htable_widen(htable *t, u32 *entries, i32 rootbits)
{
    assert(t->entries == entries);
    iz width = (iz)t->mask + 1;
    for (; width < (iz)1<<rootbits; width *= 2) {
        bytecopy(entries+width, entries, width*(iz)sizeof(u32));
    }
    t->mask = (1u << rootbits) - 1;
}

// Memory needed by inf_new, including alignment padding.
static iz inf_memsize(b32 d64)
{
    iz ndist  = d64 ? DIST64_ENOUGH : DIST_ENOUGH;
    iz wincap = d64 ? INF64_WINCAP : INF_WINCAP;
    return (iz)sizeof(inflator) + _Alignof(inflator) +
           ndist*(iz)sizeof(u32) + _Alignof(u32) + wincap + 64;
}

// Memory needed by inflate_new, including alignment padding.
[[maybe_unused]] static iz inflate_memsize(void)
{
    return inf_memsize(0);
}

// Memory needed by inflate64_new, including alignment padding.
[[maybe_unused]] static iz inflate64_memsize(void)
{
    return inf_memsize(1);
}

// Prepare to decode a new stream.
static void inflate_reset(inflator *s)
{
    s->in       = s->inend = 0;
    s->bitbuf   = 0;
    s->bitcnt   = 0;
    s->err      = 0;
    s->state    = INF_HEAD;
    s->final    = 0;
    s->stored   = 0;
    s->wpos     = 0;
    s->wflushed = 0;
    s->stashlen = 0;
}

// Only the fields inflate_reset sets, and cpu, need a value: each block
// header sets lt, dt and wide (a dynamic one through its code lengths,
// which it starts) before any symbol is decoded, every code length is
// written before it is read, and the stash is filled before it is read.
// So the state starts uncleared, and costs no more to set up than to
// reset. The CPU is not asked here, but when the fast loop first runs.
static inflator *inf_new(arena *a, b32 d64)
{
    inflator *s = alloc(a, 1, sizeof(inflator), _Alignof(inflator), 0);
    s->d64 = d64;
    s->dist_entries = alloc(a, d64 ? DIST64_ENOUGH : DIST_ENOUGH,
                            sizeof(u32), _Alignof(u32), 0);
    s->win = newbytes(a, d64 ? INF64_WINCAP : INF_WINCAP);
    s->cpu = 0;
    inflate_reset(s);
    return s;
}

// A DEFLATE inflator.
[[maybe_unused]] static inflator *inflate_new(arena *a)
{
    return inf_new(a, 0);
}

// A Deflate64 inflator, otherwise used as inflate_new's.
[[maybe_unused]] static inflator *inflate64_new(arena *a)
{
    return inf_new(a, 1);
}

// Note that the current unit cannot complete with the input at hand.
static void inf_starve(inflator *s)
{
    if (!s->err) {
        s->err = GZ_NEEDIN;
    }
}

// Make at least n bits available, one byte at a time. Returns false if
// input ran out first, in which case fewer bits are available.
static b32 inf_need(inflator *s, i32 n)
{
    assert(n <= 32);
    while (s->bitcnt < n) {
        if (s->in == s->inend) {
            return 0;
        }
        s->bitbuf |= (u64)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    return 1;
}

static void inf_drop(inflator *s, i32 n)
{
    s->bitbuf >>= n;
    s->bitcnt -= n;
}

static u32 inf_bits(inflator *s, i32 n)
{
    if (!inf_need(s, n)) {
        inf_starve(s);
        return 0;
    }
    u32 v = (u32)s->bitbuf & ((1u << n) - 1);
    inf_drop(s, n);
    return v;
}

// Return whole bytes in the bit buffer to the input.
static void inf_giveback(inflator *s)
{
    s->in -= s->bitcnt >> 3;
    s->bitcnt &= 7;
    s->bitbuf &= ((u64)1 << s->bitcnt) - 1;
}

// The entry for a code longer than the root, from the root's link.
[[gnu::always_inline]] static inline u32 sublookup(u32 const *t, u32 link,
                                                   u64 bb)
{
    u32 sub = (u32)(bb >> ENT_CODELEN(link)) & ((1u << ENT_SUBBITS(link)) - 1);
    return t[ENT_VAL(link) + sub];
}

// Look up the entry for the next code, resolving subtables. Always
// inlined: GCC would otherwise call it for every symbol in the hot loop.
[[gnu::always_inline]] static inline u32 lookup(u32 const *t, u32 mask, u64 bb)
{
    u32 e = t[bb & mask];
    if (e & F_LINK) {
        e = sublookup(t, e, bb);
    }
    return e;
}

// Decode one code the careful way, consuming only the code itself. Like
// zlib, an invalid code is only reported once all its bits are present.
// Returns a BAD entry on error. The unit must not be starved already:
// the lookup would then begin at bits of the unfinished field, and an
// invalid entry there (a lone or empty code has 1-bit ones) would turn
// truncation into an error.
static u32 inf_decode(inflator *s, htable const *t)
{
    assert(!s->err);
    inf_need(s, 15);
    u32 e = lookup(t->entries, t->mask, s->bitbuf);
    if (ENT_CODELEN(e) > s->bitcnt) {
        inf_starve(s);
        return ENT(0, ENT_BAD, 0, 0);
    } else if (ENT_KIND(e) == ENT_BAD) {
        s->err = GZ_EDATA;
    } else {
        inf_drop(s, ENT_CODELEN(e));
    }
    return e;
}

// Make room for at least one maximum-length match, sliding history to
// the front once all output has been handed out. Returns false if
// undelivered output is in the way.
static b32 inf_room(inflator *s)
{
    iz hist = s->d64 ? INF64_HIST : INF_HIST;
    iz lim  = s->d64 ? INF64_WINCAP-INF64_SLACK : INF_WINCAP-INF_SLACK;
    if (s->wpos <= lim) {
        return 1;
    } else if (s->wflushed < s->wpos) {
        return 0;
    }
    bytemove(s->win, s->win+s->wpos-hist, hist);
    s->wpos = s->wflushed = hist;
    return 1;
}

// Decoded output not yet handed out.
static s8 inflate_pending(inflator *s)
{
    s8 r = {s->win + s->wflushed, s->wpos - s->wflushed};
    return r;
}

// Mark the first n bytes of pending output as handed out.
static void inflate_consume(inflator *s, iz n)
{
    assert(n>=0 && n<=s->wpos-s->wflushed);
    s->wflushed += n;
}

static u64 load64(u8 const *p)
{
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static void store64(u8 *p, u64 v)
{
    __builtin_memcpy(p, &v, 8);
}

// Copy a match of len bytes from dist back, writing up to 31 bytes past
// the end (requires slack). Inlined into each format's fast loop, as it
// would be into one.
[[gnu::always_inline]] static inline u8 *copy_match(u8 *out, iz dist, iz len)
{
    u8 *src = out - dist;
    u8 *end = out + len;
    if (dist >= 16) {
        do {
            __builtin_memcpy(out+ 0, src+ 0, 16);
            __builtin_memcpy(out+16, src+16, 16);
            out += 32;
            src += 32;
        } while (out < end);
    } else if (dist >= 8) {
        do {
            store64(out, load64(src));
            out += 8;
            src += 8;
        } while (out < end);
    } else if (dist == 1) {
        u64 v = 0x0101010101010101u * src[0];
        do {
            store64(out+0, v);
            store64(out+8, v);
            out += 16;
        } while (out < end);
    } else {
        // Short period: expand it into a pattern buffer, then store 8
        // bytes at a time, rotating the phase within the period.
        u8 pat[16];
        for (i32 i = 0, j = 0; i < 16; i++, j = j+1==dist ? 0 : j+1) {
            pat[i] = src[j];
        }
        iz step  = 8 % dist;
        iz phase = 0;
        do {
            store64(out, load64(pat + phase));
            out += 8;
            phase += step;
            phase -= phase>=dist ? dist : 0;
        } while (out < end);
    }
    return end;
}

// Fast loop: runs while at least 16 input bytes and room for a full
// match remain. Returns true at end of block, false to fall back to the
// careful path (or on error). Always inlined into decode_fast,
// decode_narrow and decode_fast64, so d64 and wide are constants and
// each has its own loop: with wide, the tables' roots are full and
// their masks constants (see htable_widen).
[[gnu::always_inline]] static inline b32 decode_fast_as(inflator *s, b32 d64,
                                                        b32 wide)
{
    htable const *lt = &s->lt;
    htable const *dt = &s->dt;
    u8 const *in    = s->in;
    u8 const *inend = s->inend;
    u8 *win    = s->win;
    u8 *out    = win + s->wpos;
    u8 *outlim = win + (d64 ? INF64_WINCAP-INF64_SLACK : INF_WINCAP-INF_SLACK);
    u64 bb = s->bitbuf;
    u32 bc = (u32)s->bitcnt;  // only its low 6 bits count: see CONSUME
    u32 const *lte = lt->entries;
    u32 const *dte = dt->entries;
    u32 lmask = wide ? (1u << LIT_ROOT) - 1 : lt->mask;
    u32 dmask = wide ? (1u << DIST_ROOT) - 1 : dt->mask;
    b32 eob = 0;
    assert(lt->mask==lmask && dt->mask==dmask);

    // Each iteration begins with a refill, after which all 64 buffered
    // bits are real input (bitcnt counts only whole bytes, at least 56).
    // An iteration consumes at most 48 bits: three literals (3*15), or a
    // length and distance (15+5+15+13). So at least 16 real bits remain,
    // enough to look up the next code early, overlapping the lookup with
    // the match copy. Refilling only appends above existing bits, so the
    // preloaded entry stays valid. An iteration reads at most 8 bytes.
    //
    // Deflate64's distances take up to 14 extra bits (15+5+15+14 = 49,
    // leaving 15, still enough), and its length code 285 takes 16, after
    // which the buffer refills again before the distance. Its iterations
    // then read at most 16 bytes, which the loop's test leaves.
    //
    // Literal/length entries are looked up early in the root alone, so
    // the literal path never tests for a link. A link, for a code longer
    // than the root, rarely needed, fails the tests for literals and for
    // lengths alike, and its entry is then looked up at the top of an
    // iteration, which takes it as any other, with no bits yet consumed
    // since the refill.
    //
    // Consuming an entry subtracts the whole entry from bc, as libdeflate
    // does, so only bc's low 6 bits count. An entry's low 6 bits are its
    // total, bit 5 being clear but in Deflate64's distances (F_HI), and a
    // 64-bit shift ignores the rest of its count on x86-64 and ARM64. So
    // no mask lies between loading an entry and the shift that consumes
    // it, on the path from one code's lookup to the next. Deflate64's
    // distances take the mask (CONSUME31).
    #define REFILL() \
        bb |= load64le(in) << (bc & 63); \
        in += 7 - (bc>>3 & 7); \
        bc |= 56
    #define CONSUME(e) \
        bb >>= (e) & 63; \
        bc -= (e)
    #define CONSUME31(e) \
        bb >>= ENT_TOTAL(e); \
        bc -= (u32)ENT_TOTAL(e)
    #define EXTRA(e) \
        (iz)(((u32)bb & ((1u << ENT_TOTAL(e)) - 1)) >> ENT_CODELEN(e))

    if (inend-in>=16 && out<=outlim) {
        REFILL();
        u32 e = lte[bb & lmask];
        for (;;) {
            if (e & F_LIT) {
                CONSUME(e);
                *out++ = (u8)ENT_VAL(e);
                e = lte[bb & lmask];
                if (e & F_LIT) {
                    CONSUME(e);
                    *out++ = (u8)ENT_VAL(e);
                    e = lte[bb & lmask];
                    if (e & F_LIT) {
                        CONSUME(e);
                        *out++ = (u8)ENT_VAL(e);
                        e = lte[bb & lmask];
                    }
                }
            } else if (!(e & (F_SPECIAL|F_LINK))) {
                iz  len    = ENT_VAL(e) + EXTRA(e);
                b32 refill = d64 && ENT_TOTAL(e)>15+5;
                CONSUME(e);
                if (refill) {
                    REFILL();
                }

                e = dte[bb & dmask];
                if (e & (F_SPECIAL|F_LINK)) {
                    if (e & F_LINK) {
                        e = sublookup(dte, e, bb);
                    }
                    if (e & F_SPECIAL) {
                        s->err = GZ_EDATA;
                        break;
                    }
                }
                iz dist = (d64 ? ENT_VAL64(e) : ENT_VAL(e)) + EXTRA(e);
                if (d64) {
                    CONSUME31(e);
                } else {
                    CONSUME(e);
                }
                if (dist > out-win) {
                    s->err = GZ_EDATA;
                    break;
                }

                e = lte[bb & lmask];
                out = copy_match(out, dist, len);
            } else if (e & F_LINK) {
                e = sublookup(lte, e, bb);
                continue;  // never a link, and no bits consumed: no refill
            } else {
                eob = !!(e & F_EOB);
                if (!eob) {
                    s->err = GZ_EDATA;
                    break;
                }
                CONSUME(e);
                break;
            }

            if (inend-in<16 || out>outlim) {
                break;
            }
            REFILL();
        }
    }
    #undef EXTRA
    #undef CONSUME31
    #undef CONSUME
    #undef REFILL

    s->in = in;
    s->wpos = out - win;
    s->bitbuf = bb;
    s->bitcnt = (i32)(bc & 63);
    inf_giveback(s);
    return eob;
}

static b32 decode_fast(inflator *s)
{
    return decode_fast_as(s, 0, 1);
}

static b32 decode_narrow(inflator *s)
{
    return decode_fast_as(s, 0, 0);
}

static b32 decode_fast64(inflator *s)
{
    return decode_fast_as(s, 1, 1);
}

// On x86-64 the DEFLATE loops are compiled again for BMI2, chosen at run
// time unless the target always has it. BMI2's shifts (shlx, shrx) take
// any register for the count, leave the flags alone, and need no copy
// of the source, and bzhi takes the low bits in one instruction, where
// without BMI2 a shift's count must be in cl. Deflate64, rare, keeps
// its one loop.
#if __x86_64__ && !__BMI2__
#define INF_BMI2 1
__attribute((target("bmi2"))) static b32 decode_fast_bmi2(inflator *s)
{
    return decode_fast_as(s, 0, 1);
}

__attribute((target("bmi2"))) static b32 decode_narrow_bmi2(inflator *s)
{
    return decode_fast_as(s, 0, 0);
}
#endif

// Run the fast loop for the block's format and tables, and the CPU.
[[gnu::always_inline]] static inline b32 inf_fast(inflator *s)
{
    if (s->d64) {
        return decode_fast64(s);
    }
#if INF_BMI2
    if (cpu_features(&s->cpu) & CPU_BMI2) {
        return s->wide ? decode_fast_bmi2(s) : decode_narrow_bmi2(s);
    }
#endif
    return s->wide ? decode_fast(s) : decode_narrow(s);
}

// Careful path: decode one literal, length/distance pair, or end of
// block. Output is only written once the whole unit has been decoded.
// Like zlib, each field is finished before the next begins, so input
// running out anywhere in the unit is truncation.
static void inf_symbol(inflator *s)
{
    u32 e = inf_decode(s, &s->lt);
    if (s->err) {
        return;
    } else if (e & F_LIT) {
        s->win[s->wpos++] = (u8)ENT_VAL(e);
        return;
    } else if (e & F_SPECIAL) {
        s->state = s->final ? INF_END : INF_HEAD;  // end of block
        return;
    }
    iz len = ENT_VAL(e) + inf_bits(s, ENT_EXTRA(e));
    if (s->err) {
        return;
    }
    e = inf_decode(s, &s->dt);
    if (s->err) {
        return;
    }
    iz dist = ENT_VAL64(e) + inf_bits(s, ENT_EXTRA(e));  // F_HI: Deflate64
    if (s->err) {
        return;
    } else if (dist > s->wpos) {
        s->err = GZ_EDATA;
        return;
    }
    u8 *out = s->win + s->wpos;
    for (iz i = 0; i < len; i++) {
        out[i] = out[i-dist];
    }
    s->wpos += len;
}

// Read a dynamic block's code counts and code length code, which
// describes the code lengths that follow (inf_lens).
static void inf_dynamic(inflator *s)
{
    i32 hlit  = (i32)inf_bits(s, 5) + 257;
    i32 hdist = (i32)inf_bits(s, 5) + 1;
    i32 hclen = (i32)inf_bits(s, 4) + 4;
    if (s->err) {
        return;
    } else if (hlit>286 || hdist>(s->d64 ? 32 : 30)) {
        s->err = GZ_EDATA;
        return;
    }

    u16 cllens[19] = {0};
    for (i32 i = 0; i < hclen; i++) {
        cllens[inf_cl_order[i]] = (u16)inf_bits(s, 3);
    }
    if (s->err) {
        return;
    } else if (!htable_build(&s->cl, s->cl_entries, countof(s->cl_entries),
                             cllens, 19, HUFF_CODELEN, 7)) {
        s->err = GZ_EDATA;
        return;
    }
    s->nlit  = hlit;
    s->nlens = hlit + hdist;
    s->nread = 0;
}

// Read a dynamic block's code lengths, one per symbol (with its repeat),
// and once all are read, build the block's tables into lt and dt. Only
// the first can run out of input: each takes at most 14 bits, and the
// next is read only with 2 more bytes at hand. Errors are found as zlib
// finds them, the tables' once the last length is read.
static void inf_lens(inflator *s)
{
    i32  total = s->nlens;
    i32  n     = s->nread;
    u16 *lens  = s->lens;
    do {
        u32 e = inf_decode(s, &s->cl);
        if (s->err) {
            return;
        }
        i32 sym = ENT_VAL(e);
        if (sym < 16) {
            lens[n++] = (u16)sym;
            continue;
        }

        // Like zlib, read the extra bits before validating the repeat
        u16 fill = 0;
        i32 repeat = 0;
        switch (sym) {
        case 16:
            repeat = 3 + (i32)inf_bits(s, 2);
            fill = n ? lens[n-1] : 0;
            break;
        case 17:
            repeat = 3 + (i32)inf_bits(s, 3);
            break;
        case 18:
            repeat = 11 + (i32)inf_bits(s, 7);
            break;
        }
        if (s->err) {
            return;
        } else if ((sym==16 && !n) || repeat>total-n) {
            s->err = GZ_EDATA;
            return;
        }
        for (; repeat; repeat--) {
            lens[n++] = fill;
        }
    } while (n<total && s->inend-s->in>=2);
    s->nread = n;
    if (n < total) {
        return;
    }

    b32 d64 = s->d64;
    if (!lens[256] ||
        !htable_build(&s->lt, s->lit_entries, countof(s->lit_entries),
                      lens, s->nlit, d64 ? HUFF_LITLEN64 : HUFF_LITLEN,
                      LIT_ROOT) ||
        !htable_build(&s->dt, s->dist_entries,
                      d64 ? DIST64_ENOUGH : DIST_ENOUGH, lens+s->nlit,
                      total-s->nlit, d64 ? HUFF_DIST64 : HUFF_DIST,
                      DIST_ROOT)) {
        s->err = GZ_EDATA;
        return;
    }
    s->wide = d64 || s->lt.mask==(1u<<LIT_ROOT)-1;
    if (s->wide) {
        htable_widen(&s->lt, s->lit_entries, LIT_ROOT);
        htable_widen(&s->dt, s->dist_entries, DIST_ROOT);
    }
    s->state = INF_SYMBOLS;
}

// Build Deflate64's fixed codes, the same code lengths as DEFLATE's but
// a different length code 285 and valid distance codes 30 and 31, into
// the dynamic codes' tables. Rare enough not to need constant tables.
static void inf_fixed64(inflator *s)
{
    u16 *lens = s->lens;
    for (i32 i = 0; i < 288; i++) {
        lens[i] = i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8;
    }
    htable_build(&s->lt, s->lit_entries, countof(s->lit_entries), lens, 288,
                 HUFF_LITLEN64, LIT_ROOT);
    htable_widen(&s->lt, s->lit_entries, LIT_ROOT);
    for (i32 i = 0; i < 32; i++) {
        lens[i] = 5;
    }
    htable_build(&s->dt, s->dist_entries, DIST64_ENOUGH, lens, 32,
                 HUFF_DIST64, DIST_ROOT);
    htable_widen(&s->dt, s->dist_entries, DIST_ROOT);
}

// Read a block header, including a stored block's lengths or the start
// of a dynamic block's code descriptions.
static void inf_header(inflator *s)
{
    b32 final = (b32)inf_bits(s, 1);
    i32 type  = (i32)inf_bits(s, 2);
    if (s->err) {
        return;
    }
    switch (type) {
    case 0: {
        inf_drop(s, s->bitcnt & 7);
        u32 len  = inf_bits(s, 16);
        u32 nlen = inf_bits(s, 16);
        if (s->err) {
            return;
        } else if (len != (~nlen & 0xffff)) {
            s->err = GZ_EDATA;
            return;
        }
        inf_giveback(s);  // the bit buffer is now empty
        s->stored = len;
        s->state = INF_STORED;
    } break;
    case 1:
        if (s->d64) {
            inf_fixed64(s);
        } else {
            s->lt = (htable){inf_fixlit, countof(inf_fixlit) - 1};
            s->dt = (htable){inf_fixdist, countof(inf_fixdist) - 1};
        }
        s->wide = 1;
        s->state = INF_SYMBOLS;
        break;
    case 2:
        inf_dynamic(s);
        if (s->err) {
            return;
        }
        s->state = INF_LENS;
        break;
    default:
        s->err = GZ_EDATA;
        return;
    }
    s->final = final;
}

// Run one atomic unit. If input runs out, roll it back and return false.
static b32 inf_unit(inflator *s)
{
    u8 const *in = s->in;
    u64 bitbuf = s->bitbuf;
    i32 bitcnt = s->bitcnt;
    switch (s->state) {
    case INF_HEAD: inf_header(s); break;
    case INF_LENS: inf_lens(s);   break;
    default:       inf_symbol(s);
    }
    if (s->err == GZ_NEEDIN) {
        s->in = in;
        s->bitbuf = bitbuf;
        s->bitcnt = bitcnt;
        s->err = 0;
        return 0;
    }
    return 1;
}

// Decode until the input runs out, undelivered output fills the window,
// or the stream ends. Returns a GZ_* status.
static i32 inf_run(inflator *s)
{
    for (;;) {
        if (s->err) {
            return s->err;
        } else if (s->state == INF_END) {
            return GZ_OK;
        } else if (!inf_room(s)) {
            return GZ_NEEDOUT;
        }

        switch (s->state) {
        case INF_STORED: {
            if (!s->stored) {
                s->state = s->final ? INF_END : INF_HEAD;
                continue;
            }
            iz n = MIN(s->stored, s->inend - s->in);
            n = MIN(n, (s->d64 ? INF64_WINCAP : INF_WINCAP) - s->wpos);
            if (!n) {
                return GZ_NEEDIN;
            }
            bytecopy(s->win + s->wpos, s->in, n);
            s->wpos += n;
            s->in += n;
            s->stored -= n;
        } continue;
        case INF_SYMBOLS:
            if (s->inend-s->in >= 16) {
                b32 eob = inf_fast(s);
                if (eob) {
                    s->state = s->final ? INF_END : INF_HEAD;
                }
                continue;
            }
            break;
        }

        if (!inf_unit(s)) {
            return GZ_NEEDIN;
        }
    }
}

// Copy pending output into the caller's buffer.
static void inf_drain(inflator *s, zbuf *b)
{
    s8 p = inflate_pending(s);
    iz n = MIN(p.len, b->outlen);
    if (n) {
        bytecopy(b->out, p.s, n);
        b->out += n;
        b->outlen -= n;
        inflate_consume(s, n);
    }
}

// Status of a call that has delivered what output fits: GZ_NEEDOUT while
// decoded output remains to be delivered, else r.
static i32 inf_held(inflator *s, i32 r)
{
    return s->wflushed<s->wpos ? GZ_NEEDOUT : r;
}

// Decode from b->in into b->out, advancing both. Returns GZ_NEEDOUT while
// decoded output remains to be delivered, which is only once b->out is
// full. Otherwise all output decoded so far has been delivered, and it
// returns GZ_OK at the end of the stream, with b->in just past it,
// GZ_NEEDIN when all input has been consumed, or an error. Decoding runs
// ahead of b->out into the window, so without this a caller would see
// the input run out, or an error, with output still held. An error thus
// waits for the output before it, as with zlib, which decodes no further
// than its output buffer.
//
// Output may also be taken without copying through inflate_pending and
// inflate_consume, in which case b->out may be empty, and GZ_NEEDOUT
// asks the caller to take the pending output and call again.
static i32 inf_stream(inflator *s, zbuf *b)
{
    if (s->err) {
        inf_drain(s, b);
        return inf_held(s, s->err);
    }

    if (s->stashlen) {
        // Complete the unit begun by an earlier call. All stashed bytes
        // belong to it, so whatever remains afterward came from b->in.
        iz n = MIN(b->inlen, INF_STASH - s->stashlen);
        if (n) {
            bytecopy(s->stash + s->stashlen, b->in, n);
        }
        s->stashlen += n;
        b->in += n;
        b->inlen -= n;
        s->in = s->stash;
        s->inend = s->stash + s->stashlen;
        if (!inf_unit(s)) {
            assert(s->stashlen < INF_STASH);
            inf_drain(s, b);
            return inf_held(s, GZ_NEEDIN);
        }
        inf_giveback(s);
        iz left = s->inend - s->in;
        b->in -= left;
        b->inlen += left;
        s->stashlen = 0;
    }

    s->in = b->in;
    s->inend = b->in + b->inlen;
    i32 r;
    do {
        r = inf_run(s);
        inf_drain(s, b);
    } while (r==GZ_NEEDOUT && s->wflushed==s->wpos);

    inf_giveback(s);
    if (r == GZ_NEEDIN) {
        iz n = s->inend - s->in;
        assert(n < INF_STASH);
        bytecopy(s->stash, s->in, n);
        s->stashlen = n;
        s->in = s->inend;
    }
    b->in = s->in;
    b->inlen = s->inend - s->in;

    if (r == GZ_OK) {
        s->bitbuf = 0;  // padding in the final byte
        s->bitcnt = 0;
    }
    return inf_held(s, r);
}

// As inf_stream, but also for a null b->in with no input, which callers
// pass naturally (unzip for an empty entry, the library for a call that
// only takes output): offsetting a null pointer, even by zero, is
// undefined, so an empty array stands in for it.
static i32 inflate_stream(inflator *s, zbuf *b)
{
    if (b->in) {
        return inf_stream(s, b);
    }
    static u8 const none[1];
    zbuf t = *b;
    t.in = none;
    i32 r = inf_stream(s, &t);
    b->out    = t.out;
    b->outlen = t.outlen;
    return r;
}

// tugz core: raw DEFLATE encoder (RFC 1951)
//
// Input accumulates in a sliding window and is parsed into tokens once
// the window fills (or at a flush), so output depends only on the input
// and flush points, not on how input and output buffers are split
// across calls. Hash chains store window-relative positions plus one
// (zero is "none") plus a stamp (see forget), rebased whenever the
// window slides, except that the levels that search deepest link their
// chains by distances (see chain_link), and the fastest levels keep a
// bucket of recent positions per hash instead of chains (see
// parse_bucket_with).
//
// Output is staged in a buffer from which the caller drains it. Each
// emission step (one block, a window slide, or a flush) requires
// DEF_STAGE_NEED bytes of staging room, which bounds its output: a block
// holds at most TOK_CAP tokens of at most 48 bits each, a stored block
// is only chosen when no larger than that, and held-back stored data is
// under 64 KiB. Parsing pauses when a block is ready but there is no
// room, and resumes once the caller has drained output.

#define DEF_WSIZE     32768
#define DEF_WMASK     (DEF_WSIZE - 1)
#define MIN_MATCH     3
#define MAX_MATCH     258
#define HASH_LEN      4
#define HASH_BITS     16
#define HASH_SIZE     (1 << HASH_BITS)
#define D3_MAX        4096
#define LOOKAHEAD     (MAX_MATCH + 32)
#define WIN_CAP       ((1 << 20) + DEF_WSIZE)
#define TOK_CAP       65535
#define TOK_LIMIT     (TOK_CAP - 3)  // headroom for one lazy parse step
#define DEF_STAGE_NEED (TOK_CAP*6 + 2*(65535+5) + 1024)
#define DEF_STAGE     (DEF_STAGE_NEED + (1 << 16))
#define NLIT          286
#define NDIST         30
#define NCL           19
#define NOBS          10    // block split observation categories
#define OBS_BATCH     512   // observations between block split checks
#define MIN_BLOCK     10000 // minimum block size in bytes for splitting
#define FORGET_REHASH 1024  // longest history to clear by rehashing
#define BLOCK_CAP     65532 // bucket levels' block size in bytes
#define NARROW_LEVEL  5     // levels from here on link by distances
#define STAMP_STEP    (1u << 21)  // stamps' unit; see forget
#define STAMP_LAST    (0u - STAMP_STEP)

_Static_assert(WIN_CAP + DEF_WSIZE <= STAMP_STEP,
               "a stamp step puts older entries beyond any match");
_Static_assert(STAMP_STEP%DEF_WSIZE == 0, "stamps keep chain slots");

// Flush modes, matching the library's
enum {
    DEF_NONE,
    DEF_SYNC,    // byte-align output with an empty stored block
    DEF_FULL,    // also forget history
    DEF_FINISH,  // end the stream
};

typedef struct {
    u16 litlen;
    u16 dist;  // zero for literals
} token;

typedef struct {
    i32 depth;   // hash chain search depth
    i32 nice;    // stop searching at a match this long
    i32 lazy;    // lazy evaluation steps (0 for greedy)
    i32 good;    // skip lazy evaluation, and shorten search, past this
    i32 depth3;  // 3-byte chain search depth
    i32 insert;  // insert positions inside matches up to this long
    i32 bucket;  // entries per bucket of the bucket finder, or 0 (chains)
} deflate_level;

// Chosen from a sweep over Silesia: each level beats zlib's compression
// ratio at the same level while running faster. Levels 1 and 2 find
// matches in buckets of one and two recent positions per hash (see
// parse_bucket_with), the rest in hash chains; the bucket levels use
// only nice and insert besides (nice, which stops a search, only with
// two entries). Level 6, the default, came from a later sweep: its
// former setting (depth 32, nice 64, one lazy step, good 16) fell just
// short of zlib's ratio. A shallower walk that keeps its full depth,
// and evaluates lazily, for matches up to 128 bytes rather than 16
// beats zlib's ratio and runs faster too. (Two lazy steps on a still
// shallower walk ran faster yet, but their output decoded 1% slower.)
static deflate_level const deflate_levels[10] = {
    // depth nice lazy good depth3 insert bucket
    {   0,    0,  0,   0,    0,   0, 0},
    {   0,    0,  0,   0,    0,  16, 1},
    {   0,   32,  0,   0,    0,  16, 2},
    {   8,   32,  0,   8,    8,  32, 0},
    {   8,   32,  1,   8,    8, 258, 0},
    {  16,   64,  1,  16,   16, 258, 0},
    {  22,  128,  1, 128,   32, 258, 0},
    {  64,  128,  2,  32,   32, 258, 0},
    { 256,  258,  2,  64,   64, 258, 0},
    {1024,  258,  2, 258,  256, 258, 0},
};

typedef struct {
    u8  len[288];
    u16 code[288];
} htree;

// A dynamic block's codes and their run-length encoded description.
// Kept in the deflator rather than on the stack, whose frame would
// otherwise exceed a page (a stack probe on Windows).
typedef struct {
    htree lt, dt, cl;
    i32   hlit, hdist, hclen;
    i32   nrle;
    u8    rsym[NLIT+NDIST];
    u8    rextra[NLIT+NDIST];
} dynblock;

typedef struct {
    deflate_level lvl;

    u8 *obuf;     // staged output: [ooff, olen) is pending
    iz  olen;
    iz  ooff;
    b32 blkready; // a block is complete and should be emitted
    i32 flushing; // flush due once a call's input is in, or DEF_NONE
    b32 flushed;  // flushing's output is staged, awaiting drain
    b32 finq;     // a FINISH is due after flushing and those queued
    u64 nqueued;  // SYNC/FULL flushes due after flushing, alternating

    u8 *win;
    iz  win_len;
    u64 base;     // stream offset of win[0]
    iz  pos;      // next position to parse
    iz  ins;      // next position to insert into hash chains
    b32 sampled;
    b32 use3;

    u32 *head;    // or the buckets (see parse_bucket_with)
    u32 *prev;
    u32 *head3;   // follows head
    u32 *prev3;
    u32 *seen;    // sample's bitmap of hashes, zero between samples
    u32  stamp;   // added to hash table entries; see forget

    token *toks;
    iz     ntok;

    u64 blk_start;
    u64 blk_len;
    u64 pend_start;
    iz  pend_len;

    // Block splitting statistics: coarse symbol categories observed in
    // the current block, and in the latest batch of observations.
    u32 obs[NOBS];
    u32 newobs[NOBS];
    u32 nobs;
    u32 nnewobs;

    u32 lit_freq[NLIT];
    u32 dist_freq[NDIST];

    u64 bitbuf;
    i32 bitcnt;

    htree fixlit;
    htree fixdist;
    dynblock dyn;

    u8 lcode[MAX_MATCH+1];  // length -> length code index
    u8 dcode[512];          // see dcode_of
    u32 lenbits[MAX_MATCH+1];  // block's length codes, see emit_tokens
    b32 narrow;   // the level links chains by distances (see chain_link)
} deflator;

static u16 const def_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static u8 const def_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static u16 const def_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static u8 const def_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};
static u8 const def_cl_order[NCL] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Length code index (0..28) for a match length in 3..258.
static i32 length_code(i32 len)
{
    assert(len>=MIN_MATCH && len<=MAX_MATCH);
    u32 x = (u32)len - 3;
    if (x < 8) {
        return (i32)x;
    } else if (len == 258) {
        return 28;
    }
    i32 b = 31 - __builtin_clz(x);
    return 4*(b - 1) + (i32)(x>>(b - 2) & 3);
}

// Distance code (0..29) for a distance in 1..32768.
static i32 dist_code(i32 dist)
{
    assert(dist>=1 && dist<=DEF_WSIZE);
    u32 x = (u32)dist - 1;
    if (x < 4) {
        return (i32)x;
    }
    i32 b = 31 - __builtin_clz(x);
    return 2*b + (i32)(x>>(b - 1) & 1);
}

static i32 lcode_of(deflator *d, i32 len)
{
    return d->lcode[len];
}

static i32 dcode_of(deflator *d, i32 dist)
{
    return d->dcode[dist<=256 ? dist-1 : 256 + ((dist-1)>>7)];
}

static u32 bit_reverse(u32 v, i32 n)
{
    u32 r = 0;
    for (i32 i = 0; i < n; i++, v >>= 1) {
        r = r<<1 | (v&1);
    }
    return r;
}

// Assign canonical codes from lengths already stored in t.
static void huff_codes(htree *t, i32 n)
{
    u32 count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        count[t->len[i]]++;
    }
    count[0] = 0;
    u32 next[16];
    u32 code = 0;
    for (i32 b = 1; b < 16; b++) {
        code = (code + count[b-1]) << 1;
        next[b] = code;
    }
    for (i32 i = 0; i < n; i++) {
        i32 l = t->len[i];
        t->code[i] = l ? (u16)bit_reverse(next[l]++, l) : 0;
    }
}

// Moffat & Katajainen in-place minimum redundancy code. On input, w holds
// n weights in ascending order. On output it holds code lengths, which
// are therefore in descending order.
static void min_redundancy(u32 *w, i32 n)
{
    if (n == 1) {
        w[0] = 1;
        return;
    }

    w[0] += w[1];
    i32 root = 0;
    i32 leaf = 2;
    for (i32 next = 1; next < n-1; next++) {
        if (leaf>=n || w[root]<w[leaf]) {
            w[next] = w[root];
            w[root++] = (u32)next;
        } else {
            w[next] = w[leaf++];
        }
        if (leaf>=n || (root<next && w[root]<w[leaf])) {
            w[next] += w[root];
            w[root++] = (u32)next;
        } else {
            w[next] += w[leaf++];
        }
    }

    w[n-2] = 0;
    for (i32 next = n-3; next >= 0; next--) {
        w[next] = w[w[next]] + 1;
    }

    i32 avail = 1;
    i32 used  = 0;
    u32 depth = 0;
    root = n - 2;
    for (i32 next = n-1; avail > 0; depth++) {
        for (; root>=0 && w[root]==depth; root--) {
            used++;
        }
        for (; avail > used; avail--) {
            w[next--] = depth;
        }
        avail = 2 * used;
        used = 0;
    }
}

// Build a complete, length-limited Huffman code. At least two symbols
// always get codes, as zlib does, so every code is complete.
static void huff_build(htree *t, u32 const *freq, i32 n, i32 maxlen)
{
    assert(n <= 288);
    u16 sa[288];
    u16 sb[288];
    u32 w[288];
    i32 m = 0;

    for (i32 i = 0; i < n; i++) {
        t->len[i] = 0;
        if (freq[i]) {
            sa[m++] = (u16)i;
        }
    }
    for (i32 i = 0; m < 2; i++) {
        if (!freq[i]) {
            sa[m++] = (u16)i;
        }
    }
    if (sa[0] > sa[1]) {
        u16 s = sa[0];  // a filler below the one used symbol
        sa[0] = sa[1];
        sa[1] = s;
    }

    // Sort by weight (unused fillers weigh 1), ties by symbol: symbols
    // start in order, and an LSD radix sort on the weight's bytes, being
    // stable, keeps ties so. O(n) rather than an insertion sort's O(n^2).
    #define HUFF_W(s) (freq[s] ? freq[s] : 1)
    u32 maxw = 0;
    for (i32 i = 0; i < m; i++) {
        maxw = MAX(maxw, HUFF_W(sa[i]));
    }
    u16 *syms = sa;
    u16 *tmp  = sb;
    for (i32 shift = 0; shift<32 && maxw>>shift; shift += 8) {
        u16 cnt[256] = {0};
        for (i32 i = 0; i < m; i++) {
            cnt[HUFF_W(syms[i])>>shift & 255]++;
        }
        u16 sum = 0;
        for (i32 i = 0; i < 256; i++) {
            u16 c = cnt[i];
            cnt[i] = sum;
            sum = (u16)(sum + c);
        }
        for (i32 i = 0; i < m; i++) {
            tmp[cnt[HUFF_W(syms[i])>>shift & 255]++] = syms[i];
        }
        u16 *x = syms;
        syms = tmp;
        tmp = x;
    }
    for (i32 i = 0; i < m; i++) {
        w[i] = HUFF_W(syms[i]);
    }
    #undef HUFF_W

    min_redundancy(w, m);

    // Clamp to maxlen, then restore the Kraft equality by lengthening
    // shorter codes until the overflow is absorbed.
    u32 count[16] = {0};
    for (i32 i = 0; i < m; i++) {
        count[MIN(w[i], (u32)maxlen)]++;
    }
    u32 total = 0;
    for (i32 b = 1; b <= maxlen; b++) {
        total += count[b] << (maxlen - b);
    }
    for (; total != 1u<<maxlen; total--) {
        count[maxlen]--;
        for (i32 b = maxlen-1; b > 0; b--) {
            if (count[b]) {
                count[b]--;
                count[b+1] += 2;
                break;
            }
        }
    }

    // Lowest weights first, so assign longest lengths first
    i32 k = 0;
    for (i32 b = maxlen; b > 0; b--) {
        for (u32 j = 0; j < count[b]; j++) {
            t->len[syms[k++]] = (u8)b;
        }
    }
    huff_codes(t, n);
}

// Append n bits, n <= 32. Whole bytes spill into the staging buffer
// 8 bytes at a time once 32 or more bits accumulate. Always inlined:
// GCC would otherwise call it for every code it writes (emit_tokens,
// which writes most of them, keeps this state in locals instead).
[[gnu::always_inline]] static inline void bw_put(deflator *d, u64 v, i32 n)
{
    assert(n<=32 && !(v>>n));
    d->bitbuf |= v << d->bitcnt;
    d->bitcnt += n;
    if (d->bitcnt >= 32) {
        assert(DEF_STAGE-d->olen >= 8);
        store64le(d->obuf+d->olen, d->bitbuf);
        d->olen += d->bitcnt >> 3;
        d->bitbuf >>= d->bitcnt & ~7;
        d->bitcnt &= 7;
    }
}

// Pad to a byte boundary and spill all bits.
static void bw_align(deflator *d)
{
    d->bitcnt = (d->bitcnt + 7) & ~7;
    for (; d->bitcnt; d->bitcnt -= 8) {
        assert(d->olen < DEF_STAGE);
        d->obuf[d->olen++] = (u8)d->bitbuf;
        d->bitbuf >>= 8;
    }
}

// Append whole bytes, such as a container header or trailer, at a byte
// boundary.
static void deflate_bytes(deflator *d, u8 const *p, iz len)
{
    assert(!d->bitcnt && len<=DEF_STAGE-d->olen);
    bytecopy(d->obuf+d->olen, p, len);
    d->olen += len;
}

static i32 cl_extra_bits(i32 s)
{
    return s==16 ? 2 : s==17 ? 3 : s==18 ? 7 : 0;
}

// Run-length encode code lengths into code length code symbols.
static i32 rle_lengths(u8 const *lens, i32 n, u8 *sym, u8 *extra)
{
    i32 m = 0;
    for (i32 i = 0; i < n;) {
        i32 l = lens[i];
        i32 j = i + 1;
        for (; j<n && lens[j]==l; j++) {}
        i32 run = j - i;
        i = j;

        if (!l) {
            for (; run >= 11; m++) {
                i32 r = MIN(run, 138);
                sym[m] = 18;
                extra[m] = (u8)(r - 11);
                run -= r;
            }
            for (; run >= 3; m++) {
                i32 r = MIN(run, 10);
                sym[m] = 17;
                extra[m] = (u8)(r - 3);
                run -= r;
            }
        } else {
            sym[m] = (u8)l;
            extra[m++] = 0;
            run--;
            for (; run >= 3; m++) {
                i32 r = MIN(run, 6);
                sym[m] = 16;
                extra[m] = (u8)(r - 3);
                run -= r;
            }
        }
        for (; run > 0; run--, m++) {
            sym[m] = (u8)l;
            extra[m] = 0;
        }
    }
    return m;
}

static void build_dyn(deflator *d, dynblock *b)
{
    u32 lf[NLIT];
    for (i32 i = 0; i < NLIT; i++) {
        lf[i] = d->lit_freq[i];
    }
    lf[256] = 1;
    huff_build(&b->lt, lf, NLIT, 15);
    huff_build(&b->dt, d->dist_freq, NDIST, 15);

    i32 hlit = NLIT;
    for (; hlit>257 && !b->lt.len[hlit-1]; hlit--) {}
    i32 hdist = NDIST;
    for (; hdist>1 && !b->dt.len[hdist-1]; hdist--) {}
    b->hlit  = hlit;
    b->hdist = hdist;

    u8 lens[NLIT+NDIST];
    bytecopy(lens, b->lt.len, hlit);
    bytecopy(lens+hlit, b->dt.len, hdist);
    b->nrle = rle_lengths(lens, hlit+hdist, b->rsym, b->rextra);

    u32 clf[NCL] = {0};
    for (i32 i = 0; i < b->nrle; i++) {
        clf[b->rsym[i]]++;
    }
    huff_build(&b->cl, clf, NCL, 7);

    i32 hclen = NCL;
    for (; hclen>4 && !b->cl.len[def_cl_order[hclen-1]]; hclen--) {}
    b->hclen = hclen;
}

// Size of the block's symbols under the given codes, from frequencies.
static u64 cost_tokens(deflator *d, htree *lt, htree *dt)
{
    u64 bits = lt->len[256];
    for (i32 i = 0; i < 256; i++) {
        bits += (u64)d->lit_freq[i] * lt->len[i];
    }
    for (i32 i = 0; i < 29; i++) {
        bits += (u64)d->lit_freq[257+i] * (lt->len[257+i] + def_len_extra[i]);
    }
    for (i32 i = 0; i < NDIST; i++) {
        bits += (u64)d->dist_freq[i] * (dt->len[i] + def_dist_extra[i]);
    }
    return bits;
}

static u64 cost_dyn(deflator *d, dynblock *b)
{
    u64 bits = 3 + 5 + 5 + 4 + 3*(u64)b->hclen;
    for (i32 i = 0; i < b->nrle; i++) {
        bits += b->cl.len[b->rsym[i]] + (u64)cl_extra_bits(b->rsym[i]);
    }
    return bits + cost_tokens(d, &b->lt, &b->dt);
}

static u64 cost_stored(deflator *d)
{
    u64 n = d->blk_len;
    u64 nchunks = (n + 65534) / 65535;
    u64 pad = (8 - ((u64)d->bitcnt + 3)%8) % 8;
    return 3 + pad + 32 + 8*n + (nchunks - 1)*40;
}

static void emit_stored_chunk(deflator *d, iz start, iz len, b32 final)
{
    assert(len>=0 && len<=65535);
    bw_put(d, (u32)final, 1);
    bw_put(d, 0, 2);
    bw_align(d);
    bw_put(d, (u32)len | (~(u32)len & 0xffff)<<16, 32);
    bw_align(d);
    deflate_bytes(d, d->win+start, len);
}

static void flush_pending(deflator *d, b32 final)
{
    iz start = (iz)(d->pend_start - d->base);
    iz len = d->pend_len;
    d->pend_len = 0;
    if (!len && final) {
        emit_stored_chunk(d, 0, 0, 1);
    }
    for (iz off = 0; off < len;) {
        iz n = MIN(len-off, 65535);
        emit_stored_chunk(d, start+off, n, final && off+n==len);
        off += n;
    }
}

static void emit_stored(deflator *d, b32 final)
{
    u64 start = d->blk_start;
    iz  len   = (iz)d->blk_len;
    if (d->pend_len) {
        if (d->pend_start+(u64)d->pend_len == start) {
            start = d->pend_start;
            len += d->pend_len;
            d->pend_len = 0;
        } else {
            flush_pending(d, 0);
        }
    }

    // Hold back the final partial chunk so that it may merge with the
    // next stored block.
    iz off = 0;
    for (; len-off > 65535; off += 65535) {
        emit_stored_chunk(d, (iz)(start-d->base)+off, 65535, 0);
    }
    d->pend_start = start + (u64)off;
    d->pend_len = len - off;
    if (final) {
        flush_pending(d, 1);
    }
}

// Emit the block's tokens. The bit buffer, its count and the output
// position live in locals for the whole loop (stores to the output would
// otherwise force reloads of the deflator's fields), and whole bytes
// spill once per match or per up to three literals. After a spill at
// most 7 bits remain, and a match adds at most 15+5+15+13 = 48 bits and
// three literals at most 45, so the 64-bit buffer never overflows. Each
// spill stores 8 bytes but advances only by the whole bytes, which stay
// within the TOK_CAP*6 bytes of DEF_STAGE_NEED (plus those bw_put held);
// DEF_STAGE's slack beyond that absorbs the store's overhang.
static void emit_tokens(deflator *d, htree *lt, htree *dt)
{
    u64    bb   = d->bitbuf;
    i32    bc   = d->bitcnt;
    u8    *out  = d->obuf + d->olen;
    token *toks = d->toks;
    iz     ntok = d->ntok;
    assert(DEF_STAGE-d->olen >= ntok*6 + 16);

    // Each length's code and extra bits under this block's code (at most
    // 15+5 bits) above the low 8 bits, which hold their count
    for (i32 len = MIN_MATCH; len <= MAX_MATCH; len++) {
        i32 lc = lcode_of(d, len);
        i32 ln = lt->len[257+lc];
        u32 lx = (u32)(len - def_len_base[lc]);
        u32 v  = lt->code[257+lc] | lx<<ln;
        d->lenbits[len] = v<<8 | (u32)(ln + def_len_extra[lc]);
    }

    // Spill first, as bw_put may have left up to 31 bits
    store64le(out, bb);
    out += bc >> 3;
    bb >>= bc & ~7;
    bc &= 7;

    #define EMIT_LIT(c) do { \
        bb |= (u64)lt->code[c] << bc; \
        bc += lt->len[c]; \
    } while (0)

    for (iz i = 0; i < ntok;) {
        token t = toks[i++];
        if (!t.dist) {
            EMIT_LIT(t.litlen);
            if (i<ntok && !toks[i].dist) {
                EMIT_LIT(toks[i].litlen);
                i++;
                if (i<ntok && !toks[i].dist) {
                    EMIT_LIT(toks[i].litlen);
                    i++;
                }
            }
        } else {
            // The length's bits from the table, then the distance's code
            // plus extra bits: at most 15+13 bits
            u32 le = d->lenbits[t.litlen];
            i32 dc = dcode_of(d, t.dist);
            u64 dx = (u32)(t.dist - def_dist_base[dc]);
            i32 dn = dt->len[dc];
            bb |= (u64)(le >> 8) << bc;
            bc += (i32)(le & 0xff);
            bb |= ((u64)dt->code[dc] | dx<<dn) << bc;
            bc += dn + def_dist_extra[dc];
        }
        assert(bc <= 7+48);
        store64le(out, bb);
        out += bc >> 3;
        bb >>= bc & ~7;
        bc &= 7;
    }
    #undef EMIT_LIT

    d->bitbuf = bb;
    d->bitcnt = bc;
    d->olen   = out - d->obuf;
    bw_put(d, lt->code[256], lt->len[256]);
}

static void emit_dynamic(deflator *d, dynblock *b)
{
    bw_put(d, 2, 2);
    bw_put(d, (u32)(b->hlit - 257), 5);
    bw_put(d, (u32)(b->hdist - 1), 5);
    bw_put(d, (u32)(b->hclen - 4), 4);
    for (i32 i = 0; i < b->hclen; i++) {
        bw_put(d, b->cl.len[def_cl_order[i]], 3);
    }
    for (i32 i = 0; i < b->nrle; i++) {
        i32 s = b->rsym[i];
        bw_put(d, b->cl.code[s], b->cl.len[s]);
        bw_put(d, b->rextra[i], cl_extra_bits(s));
    }
    emit_tokens(d, &b->lt, &b->dt);
}

// Empty the token buffer and its statistics for the next block.
static void clear_block(deflator *d)
{
    d->ntok = 0;
    d->blk_len = 0;
    bytefill(d->lit_freq, 0, sizeof(d->lit_freq));
    bytefill(d->dist_freq, 0, sizeof(d->dist_freq));
    bytefill(d->obs, 0, sizeof(d->obs));
    bytefill(d->newobs, 0, sizeof(d->newobs));
    d->nobs = d->nnewobs = 0;
}

static void flush_block(deflator *d, b32 final)
{
    if (!d->ntok) {
        if (final) {
            if (d->pend_len) {
                flush_pending(d, 1);
            } else {
                bw_put(d, 1, 1);
                bw_put(d, 1, 2);
                bw_put(d, d->fixlit.code[256], d->fixlit.len[256]);
            }
        }
        return;
    }

    dynblock *b = &d->dyn;
    build_dyn(d, b);

    u64 cd = cost_dyn(d, b);
    u64 cf = 3 + cost_tokens(d, &d->fixlit, &d->fixdist);
    // Always true, as a block ends before a slide would discard its data
    // (see slide_ends_block), but checked, as storing reads the window.
    b32 can_store = d->blk_start>=d->base &&
                    d->blk_start-d->base+d->blk_len <= (u64)d->win_len;
    u64 cs = can_store ? cost_stored(d) : (u64)-1;

    if (cs<=cd && cs<=cf) {
        emit_stored(d, final);
    } else {
        if (d->pend_len) {
            flush_pending(d, 0);
        }
        bw_put(d, (u32)final, 1);
        if (cf <= cd) {
            bw_put(d, 1, 2);
            emit_tokens(d, &d->fixlit, &d->fixdist);
        } else {
            emit_dynamic(d, b);
        }
    }

    clear_block(d);
}

// Decide whether the latest batch of observations differs enough from
// the block so far that a new block should start, so that its Huffman
// codes can adapt. The idea follows libdeflate: compare the batch's
// category distribution to the block's, scaling both to a common total,
// with a bias toward longer blocks that grows as the block does.
static b32 should_split(deflator *d)
{
    b32 split = 0;
    if (d->nobs) {
        u64 delta = 0;
        for (i32 i = 0; i < NOBS; i++) {
            u64 expect = (u64)d->obs[i] * d->nnewobs;
            u64 actual = (u64)d->newobs[i] * d->nobs;
            delta += actual>expect ? actual-expect : expect-actual;
        }
        u64 total  = (u64)d->nobs + d->nnewobs;
        u64 cutoff = (u64)d->nnewobs * 200/512 * d->nobs;
        if (d->blk_len<MIN_BLOCK && total<8192) {
            cutoff += cutoff * (8192 - total) / 8192;
        }
        split = d->blk_len>=MIN_BLOCK &&
                delta + d->blk_len/4096*d->nobs >= cutoff;
    }
    for (i32 i = 0; i < NOBS; i++) {
        d->obs[i] += d->newobs[i];
        d->newobs[i] = 0;
    }
    d->nobs += d->nnewobs;
    d->nnewobs = 0;
    return split;
}

// The token functions run once per token in parse. Always inlined (with
// should_split left out of line): GCC would otherwise call tok_end for
// every token, and tok_lit too once more of parse is inlined.
[[gnu::always_inline]] static inline void tok_end(deflator *d)
{
    if (!d->blkready && (d->ntok>=TOK_LIMIT ||
                         (++d->nnewobs==OBS_BATCH && should_split(d)))) {
        d->blkready = 1;
    }
}

[[gnu::always_inline]] static inline void tok_lit(deflator *d, iz p)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    u8 c = d->win[p];
    d->blk_len++;
    d->toks[d->ntok++] = (token){c, 0};
    d->lit_freq[c]++;
    d->newobs[(c>>5 & 6) | (c & 1)]++;
    tok_end(d);
}

[[gnu::always_inline]]
static inline void tok_match(deflator *d, iz p, i32 len, i32 dist)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    d->blk_len += (u64)len;
    d->toks[d->ntok++] = (token){(u16)len, (u16)dist};
    d->lit_freq[257+lcode_of(d, len)]++;
    d->dist_freq[dcode_of(d, dist)]++;
    d->newobs[8 + (len >= 9)]++;
    tok_end(d);
}

// The match finder's state, which parse keeps in a local: stores to the
// hash tables and to the block's statistics, all u32 arrays, might alias
// the deflator's fields, which GCC would then reload after every
// insertion and token. (See also parse_with.)
typedef struct {
    u8  *win;
    u32 *head;
    u32 *prev;
    u32 *head3;
    u32 *prev3;
    iz   win_len;
    iz   lim;    // positions below have HASH_LEN bytes to hash
    iz   ins;    // next position to insert into hash chains
    u32  h;      // hashes of ins, when below lim, computed ahead
    u32  h3;
    u32  base;   // low bits of the stream offset of win[0]
    u32  stamp;
    b32  use3;
    deflate_level lvl;
    b32  narrow;
} finder;

// Index into prev tables for window position p.
static u32 chain_slot(finder *f, iz p)
{
    return (f->base + (u32)p) & DEF_WMASK;
}

// Index into prev tables for a hash table entry's position. Stamps are
// multiples of the window size, so they drop out.
static u32 entry_slot(finder *f, u32 entry)
{
    return (f->base + entry - 1) & DEF_WMASK;
}

// A chain link as the levels from NARROW_LEVEL on store it, in the first
// half of the link tables viewed as u16: the distance from a new entry
// back to the one it displaces from its head, saturated at LINK_NONE,
// beyond any match (an empty head, zero, gives one back past position
// 0). Their long walks of chains gain more from the halved footprint in
// the cache than the saturation costs each insertion, which the faster
// levels would not. Such links need no rebasing as the window slides.
#define LINK_NONE 0xffff
static u16 chain_link(u32 entry, u32 old)
{
    return (u16)MIN(entry - old, LINK_NONE);
}

// Little-endian, so hashes (and therefore output) match across hosts.
static u32 load32(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static u32 hash4(u32 v)
{
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static u32 hash3(u32 v)
{
    return ((v & 0xffffff) * 2654435761u) >> (32 - HASH_BITS);
}

// Hash position x ahead of its insertion, prefetching its hash table
// entries, which the insertion and the search after it would otherwise
// wait for.
[[gnu::always_inline]] static inline void hash_ahead(finder *f, iz x)
{
    if (x < f->lim) {
        u32 v = load32(f->win + x);
        f->h = hash4(v);
        __builtin_prefetch(f->head + f->h);
        if (f->use3) {
            f->h3 = hash3(v);
            __builtin_prefetch(f->head3 + f->h3);
        }
    }
}

typedef struct {
    u32 c4;  // the chains' previous heads
    u32 c3;  // (zero when not using 3-byte chains)
} heads;

// Insert ins, which must be below lim, and hash the position after it.
[[gnu::always_inline]] static inline heads hash_insert(finder *f)
{
    iz  x  = f->ins++;
    u32 h  = f->h;
    u32 h3 = f->h3;
    hash_ahead(f, x+1);
    u32 slot  = chain_slot(f, x);
    u32 entry = (u32)x + 1 + f->stamp;
    heads r = {f->head[h], 0};
    if (f->narrow) {
        ((u16 *)f->prev)[slot] = chain_link(entry, r.c4);
    } else {
        f->prev[slot] = r.c4;
    }
    f->head[h] = entry;
    if (f->use3) {
        r.c3 = f->head3[h3];
        if (f->narrow) {
            ((u16 *)f->prev3)[slot] = chain_link(entry, r.c3);
        } else {
            f->prev3[slot] = r.c3;
        }
        f->head3[h3] = entry;
    }
    return r;
}

// Insert all positions through q. Always inlined, like hash_insert and
// match_at: GCC would otherwise call this and match_at from parse, and
// hash_insert once they inline.
[[gnu::always_inline]] static inline void ensure_insert(finder *f, iz q)
{
    while (f->ins<=q && f->ins<f->lim) {
        hash_insert(f);
    }
    f->ins = MAX(f->ins, q + 1);
}

[[gnu::always_inline]]
static inline i32 match_len(u8 const *a, u8 const *b, i32 max)
{
    i32 len = 0;
    for (; len+8 <= max; len += 8) {
        u64 x = load64le(a+len) ^ load64le(b+len);
        if (x) {
            return len + (__builtin_ctzll(x) >> 3);  // first differing byte
        }
    }
    for (; len<max && a[len]==b[len]; len++) {}
    return len;
}

typedef struct {
    i32 len;
    i32 dist;
} match;

// find_match for distance links. Candidates are tracked by the low bits
// of their stream offsets (cb), as is p (pb), from which their chain
// slots follow in one step, as from entries. Candidates lie at most a
// window back, and at or after position 0, which the first from an
// empty head does not. A candidate exactly WSIZE back shares p's chain
// slot, where inserting p linked this walk's first candidate: its link
// leads more than WSIZE back, ending the walk before it would revisit
// candidates (find_match makes it the last). Otherwise as find_match.
[[gnu::always_inline]] static inline match
find_match16(finder *f, iz p, u32 cand, u32 cand3, i32 depth, i32 best)
{
    u8  *win  = f->win;
    u16 *prev = (u16 *)f->prev;
    i32 maxlen = (i32)MIN(f->win_len - p, MAX_MATCH);
    assert(maxlen >= MIN_MATCH);  // p+MIN_MATCH <= win_len
    i32 least = best;
    iz  best_pos = 0;
    u32 pb   = f->base + (u32)p;
    u32 kb   = f->base - 1 - f->stamp;  // an entry's offset, less it
    u32 cb   = cand + kb;
    u32 maxd = (u32)MIN(p, DEF_WSIZE);
    if (best >= maxlen) {
        cb = pb - LINK_NONE;  // nothing longer fits
    }
    u32 p4 = pb-cb<=maxd ? load32(win+p) : 0;  // p+HASH_LEN <= win_len

    for (; best<MIN_MATCH && pb-cb<=maxd && depth>0; depth--) {
        iz c = p - (iz)(pb - cb);
        cb -= prev[cb & DEF_WMASK];
        u32 x = load32(win+c) ^ p4;
        if (!(x & 0xffffff)) {
            best = x ? 3 : 4 + match_len(win+c+4, win+p+4, maxlen-4);
            best_pos = c;
            if (best>=f->lvl.nice || best==maxlen) {
                cb = pb - LINK_NONE;
                break;
            }
            if (best>=f->lvl.good && depth>8) {
                depth = 8;
            }
        }
    }

    for (; pb-cb<=maxd && depth>0; depth--) {
        iz c = p - (iz)(pb - cb);
        cb -= prev[cb & DEF_WMASK];
        if (load32(win+c+best-3)==load32(win+p+best-3) &&
            load32(win+c)==p4) {
            i32 len = 4 + match_len(win+c+4, win+p+4, maxlen-4);
            if (len > best) {
                best = len;
                best_pos = c;
                if (best>=f->lvl.nice || best==maxlen) {
                    break;
                }
                if (best>=f->lvl.good && depth>8) {
                    depth = 8;
                }
            }
        }
    }

    u16 *prev3  = (u16 *)f->prev3;
    i32  depth3 = best<MIN_MATCH ? f->lvl.depth3 : 0;
    u32  cb3    = cand3 + kb;
    for (; pb-cb3<=MIN(maxd, D3_MAX) && depth3>0; depth3--) {
        iz c = p - (iz)(pb - cb3);
        if (win[c]==win[p] && win[c+1]==win[p+1] && win[c+2]==win[p+2]) {
            best = MIN_MATCH;
            best_pos = c;
            break;
        }
        cb3 -= prev3[cb3 & DEF_WMASK];
    }

    match r = {0, 0};
    if (best > least) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Candidates are hash table entries, from which distances are measured
// against p's own entry: a forgotten entry is farther than any match.
// A candidate counts only if longer than the best match so far, so once
// there is one it must share p's first 4 bytes and the 4 ending at the
// best length: two 32-bit compares rule out most candidates before
// match_len, which then starts at the fifth byte. Until a first match,
// a candidate (whose 4-byte hash may merely collide) needs 3 bytes.
//
// Only a match longer than best counts, else the result is empty, so
// lazy evaluation searches from the length it has, with the stricter
// filters throughout. It walks the same candidates as from MIN_MATCH-1
// and finds the same match: a match no longer than best would end the
// walk early only at nice, or cap its depth only at good, and good <=
// nice, while lazy evaluation starts only below good; and the 3-byte
// fallback cannot find a longer match than best >= MIN_MATCH.
//
// Always inlined, with match_len: GCC (and clang) would otherwise call
// it for every position searched.
[[gnu::always_inline]] static inline match
find_match(finder *f, iz p, u32 cand, u32 cand3, i32 depth, i32 best)
{
    if (f->narrow) {
        return find_match16(f, p, cand, cand3, depth, best);
    }
    u8 *win = f->win;
    i32 maxlen = (i32)MIN(f->win_len - p, MAX_MATCH);
    assert(maxlen >= MIN_MATCH);  // p+MIN_MATCH <= win_len
    i32 least = best;
    iz  best_pos = 0;
    u32 here = (u32)p + 1 + f->stamp;
    if (best >= maxlen) {
        cand = 0;  // nothing longer fits
    }
    u32 p4 = cand ? load32(win+p) : 0;  // p+HASH_LEN <= win_len

    // Until a first match (see the next loop's comments)
    for (; best<MIN_MATCH && cand && depth>0; depth--) {
        u32 dist = here - cand;
        if (dist > DEF_WSIZE) {
            cand = 0;
            break;
        }
        iz c = p - (iz)dist;
        if (dist == DEF_WSIZE) {
            depth = 1;
        }
        cand = f->prev[entry_slot(f, cand)];
        u32 x = load32(win+c) ^ p4;
        if (!(x & 0xffffff)) {
            best = x ? 3 : 4 + match_len(win+c+4, win+p+4, maxlen-4);
            best_pos = c;
            if (best>=f->lvl.nice || best==maxlen) {
                cand = 0;
                break;
            }
            if (best>=f->lvl.good && depth>8) {
                depth = 8;
            }
        }
    }

    // Once best >= MIN_MATCH
    for (; cand && depth > 0; depth--) {
        u32 dist = here - cand;
        if (dist > DEF_WSIZE) {
            break;
        }
        iz c = p - (iz)dist;
        // A candidate exactly WSIZE back shares p's prev slot, where
        // inserting p linked this walk's first candidate. Revisiting
        // candidates cannot beat best, so make this one the last.
        if (dist == DEF_WSIZE) {
            depth = 1;
        }
        cand = f->prev[entry_slot(f, cand)];
        if (load32(win+c+best-3)==load32(win+p+best-3) &&
            load32(win+c)==p4) {
            i32 len = 4 + match_len(win+c+4, win+p+4, maxlen-4);
            if (len > best) {
                best = len;
                best_pos = c;
                if (best>=f->lvl.nice || best==maxlen) {
                    break;
                }
                if (best>=f->lvl.good && depth>8) {
                    depth = 8;
                }
            }
        }
    }

    // Fall back to short, close 3-byte matches
    i32 depth3 = best<MIN_MATCH ? f->lvl.depth3 : 0;
    for (; cand3 && depth3 > 0; depth3--) {
        u32 dist = here - cand3;
        if (dist > D3_MAX) {
            break;
        }
        iz c = p - (iz)dist;
        if (win[c]==win[p] && win[c+1]==win[p+1] && win[c+2]==win[p+2]) {
            best = MIN_MATCH;
            best_pos = c;
            break;
        }
        cand3 = f->prev3[entry_slot(f, cand3)];
    }

    match r = {0, 0};
    if (best > least) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Find the best match at p longer than best, inserting p, the next
// position to insert, into the hash chains.
[[gnu::always_inline]]
static inline match match_at(finder *f, iz p, i32 depth, i32 best)
{
    assert(p == f->ins);
    if (p < f->lim) {  // p+HASH_LEN <= win_len
        heads c = hash_insert(f);
        return find_match(f, p, c.c4, c.c3, depth, best);
    }
    f->ins = p + 1;
    if (f->use3 && p+MIN_MATCH <= f->win_len) {
        u32 v = (u32)f->win[p] | (u32)f->win[p+1]<<8 | (u32)f->win[p+2]<<16;
        return find_match(f, p, 0, f->head3[hash3(v)], depth, best);
    }
    return (match){0, 0};
}

// Decide whether 3-byte matching is worthwhile by sampling how often
// 4-byte hashes collide in the first part of the input.
static void sample(deflator *d)
{
    d->sampled = 1;
    iz n = MIN(d->win_len, 32768);
    if (n < 64) {
        return;
    }
    u32 *seen = d->seen;
    u32  hits = 0;
    for (iz i = 0; i+4 <= n; i++) {
        u32 h   = hash4(load32(d->win + i));
        u32 bit = (u32)1 << (h & 31);
        hits += (seen[h>>5] & bit) != 0;
        seen[h>>5] |= bit;
    }
    bytefill(seen, 0, HASH_SIZE/8);
    d->use3 = (u64)hits*10 < (u64)(n - 3)*7;
}

// Parse with use3 and lazy (whether the level evaluates lazily) as
// constants: each combination has an instance (below), which carries
// only the state and code it uses (the greedy levels drop lazy
// evaluation), so that GCC can keep more of it in x86-64's few
// registers.
[[gnu::always_inline]]
static inline void
parse_with(deflator *d, iz end, b32 use3, b32 lazy, b32 narrow)
{
    finder f = {
        .win     = d->win,
        .head    = d->head,
        .prev    = d->prev,
        .head3   = d->head3,
        .prev3   = d->prev3,
        .win_len = d->win_len,
        .lim     = d->win_len - (HASH_LEN - 1),
        .ins     = d->ins,
        .base    = (u32)d->base,
        .stamp   = d->stamp,
        .use3    = use3,
        .lvl     = d->lvl,
        .narrow  = narrow,
    };
    if (!lazy) {
        f.lvl.lazy = 0;
    }
    hash_ahead(&f, f.ins);

    iz p = d->pos;
    while (p<end && !d->blkready) {
        match m = match_at(&f, p, f.lvl.depth, MIN_MATCH-1);

        // Lazy matching: prefer a longer match starting one byte later
        for (i32 step = 0; step < f.lvl.lazy; step++) {
            if (m.len<MIN_MATCH || m.len>=f.lvl.good || p+1>=f.win_len) {
                break;
            }
            match m2 = match_at(&f, p+1, f.lvl.depth>>1, m.len);
            if (m2.len <= m.len) {
                break;
            }
            tok_lit(d, p++);
            m = m2;
        }

        if (m.len >= MIN_MATCH) {
            tok_match(d, p, m.len, m.dist);
            if (m.len <= f.lvl.insert) {
                ensure_insert(&f, p + m.len - 1);
            } else {
                f.ins = MAX(f.ins, p + m.len);
                hash_ahead(&f, f.ins);
            }
            p += m.len;
        } else {
            tok_lit(d, p++);
        }
    }
    d->pos = p;
    d->ins = f.ins;
}

// The instances, each a function of its own, which GCC compiles, and
// allocates registers for, alone
[[gnu::noinline]] static void parse_greedy(deflator *d, iz end)
{
    parse_with(d, end, 0, 0, 0);
}

[[gnu::noinline]] static void parse_greedy3(deflator *d, iz end)
{
    parse_with(d, end, 1, 0, 0);
}

[[gnu::noinline]] static void parse_lazy(deflator *d, iz end)
{
    parse_with(d, end, 0, 1, 0);
}

[[gnu::noinline]] static void parse_lazy3(deflator *d, iz end)
{
    parse_with(d, end, 1, 1, 0);
}

[[gnu::noinline]] static void parse_deep(deflator *d, iz end)
{
    parse_with(d, end, 0, 1, 1);
}

[[gnu::noinline]] static void parse_deep3(deflator *d, iz end)
{
    parse_with(d, end, 1, 1, 1);
}

// The bucket finder of levels 1 and 2, after libdeflate's
// ht_matchfinder: each 4-byte hash keeps its nb (1 or 2) most recent
// positions, newest first, in a bucket of nb entries, so a search is at
// most nb compares and an insertion a store or two, with no chain links.
// The table, HASH_SIZE buckets, takes head, and with two entries head3
// too, which follows it in memory; the chain levels' other tables lie
// idle. Entries are positions plus one plus the stamp, as the heads'
// (see forget), so an empty or forgotten entry lies farther back than
// any match. Matches are 4 bytes or longer, found greedily, and the
// positions inside a match are inserted up to the level's insert
// length. Blocks are not split by statistics, which cost these levels
// more time than their ratio was worth: a block ends once it holds
// BLOCK_CAP bytes of input (or before a slide, or at a flush), so it
// holds at most BLOCK_CAP tokens and, but for the last, more than
// MIN_BLOCK bytes, as deflate_bound requires.
_Static_assert(BLOCK_CAP<=TOK_LIMIT && BLOCK_CAP>=MIN_BLOCK, "block cap");

// The bucket finder's state, in a local as the chains' finder is, with
// the block's token pointer and statistics.
typedef struct {
    u8    *win;
    u32   *t;
    token *tok;    // next token
    u32   *lit_freq;
    u32   *dist_freq;
    deflator *d;
    iz     lim;    // positions below have HASH_LEN bytes to hash
    u32    stamp1; // stamp plus one
    u32    h;      // hash of the position in hand, when below lim
    i32    nice;
    i32    ins;
} bfinder;

#define BKT(f, h) ((f)->t + (iz)(h)*nb)

// Search and insert position p, below lim, and emit its token. With
// last, p+1 may be lim (else below), and has no hash. Returns the next
// position, whose hash is then in f->h when below lim. Its bucket, and
// those of positions inserted inside a match, are prefetched a step
// ahead. A candidate is used only if 1 to MIN(p, WSIZE) back, which
// rules out empty and forgotten entries (and a stale one at p itself).
// Always inlined, with match_len, into an instance per entry count.
[[gnu::always_inline]]
static inline iz bkt_step(bfinder *f, iz p, i32 nb, b32 last)
{
    u8  *win  = f->win;
    u32 *b    = BKT(f, f->h);
    u32  c0   = b[0];
    u32  c1   = nb==2 ? b[1] : 0;
    u32  here = (u32)p + f->stamp1;
    if (nb == 2) {
        b[1] = c0;
    }
    b[0] = here;

    u32 hn = 0;
    if (!last || p+1<f->lim) {
        hn = hash4(load32(win+p+1));
        __builtin_prefetch(BKT(f, hn));
    }

    u32 maxd  = (u32)MIN(p, DEF_WSIZE);
    u32 p4    = load32(win+p);
    i32 best  = 0;
    u32 bestd = 0;
    u32 dist  = here - c0;
    i32 maxlen = 0;
    if (dist-1 < maxd && load32(win+p-dist)==p4) {
        maxlen = (i32)MIN(f->d->win_len - p, MAX_MATCH);
        best   = 4 + match_len(win+p-dist+4, win+p+4, maxlen-4);
        bestd  = dist;
    }
    if (nb==2 && best<f->nice) {
        dist = here - c1;
        if (dist-1 < maxd) {
            u8 const *c = win + p - dist;
            if (!best) {
                if (load32(c) == p4) {
                    maxlen = (i32)MIN(f->d->win_len - p, MAX_MATCH);
                    best   = 4 + match_len(c+4, win+p+4, maxlen-4);
                    bestd  = dist;
                }
            } else if (best<maxlen &&
                       load32(c+best-3)==load32(win+p+best-3) &&
                       load32(c)==p4) {
                i32 len = 4 + match_len(c+4, win+p+4, maxlen-4);
                if (len > best) {
                    best  = len;
                    bestd = dist;
                }
            }
        }
    }

    deflator *d = f->d;
    if (!best) {
        *f->tok++ = (token){(u8)p4, 0};
        f->lit_freq[(u8)p4]++;
        f->h = hn;
        return p + 1;
    }

    *f->tok++ = (token){(u16)best, (u16)bestd};
    f->lit_freq[257+lcode_of(d, best)]++;
    f->dist_freq[dcode_of(d, (i32)bestd)]++;
    iz q = p + best;
    u32 hq = 0;
    if (q < f->lim) {
        hq = hash4(load32(win+q));
        __builtin_prefetch(BKT(f, hq));
    }
    if (best <= f->ins) {
        // Insert the match's later positions, hashing ahead
        iz xe = MIN(q, f->lim);
        u32 hx = hn;
        for (iz x = p+1; x < xe; x++) {
            u32 hy = hq;
            if (x+1 < xe) {
                hy = hash4(load32(win+x+1));
                __builtin_prefetch(BKT(f, hy));
            }
            u32 *bx = BKT(f, hx);
            if (nb == 2) {
                bx[1] = bx[0];
            }
            bx[0] = (u32)x + f->stamp1;
            hx = hy;
        }
    }
    f->h = hq;
    return q;
}

[[gnu::always_inline]]
static inline void parse_bucket_with(deflator *d, iz end, i32 nb)
{
    iz p = d->pos;
    assert(d->ins == p);
    assert(d->blk_len < BLOCK_CAP);
    bfinder f = {
        .win       = d->win,
        .t         = d->head,
        .tok       = d->toks + d->ntok,
        .lit_freq  = d->lit_freq,
        .dist_freq = d->dist_freq,
        .d         = d,
        .lim       = d->win_len - (HASH_LEN - 1),
        .stamp1    = d->stamp + 1,
        .nice      = d->lvl.nice,
        .ins       = d->lvl.insert,
    };
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    // Blocks grow by the input parsed, so the cap is a position. The
    // main loop needs no bounds but its own; positions too near the end
    // to hash the next (or themselves) follow.
    iz p0  = p;
    iz cap = p + (iz)(BLOCK_CAP - d->blk_len);
    end = MIN(end, cap);
    iz stop = MIN(end, f.lim-1);  // p+1 has a hash too
    if (p < f.lim) {
        f.h = hash4(load32(f.win+p));
    }
    while (p < stop) {
        p = bkt_step(&f, p, nb, 0);
    }
    while (p < end) {
        if (p < f.lim) {
            p = bkt_step(&f, p, nb, 1);
        } else {
            *f.tok++ = (token){f.win[p], 0};
            f.lit_freq[f.win[p]]++;
            p++;
        }
    }
    d->ntok     = f.tok - d->toks;
    d->blk_len += (u64)(p - p0);
    d->blkready = d->blk_len >= BLOCK_CAP;
    d->pos = p;
    d->ins = p;
}
#undef BKT

[[gnu::noinline]] static void parse_bucket1(deflator *d, iz end)
{
    parse_bucket_with(d, end, 1);
}

[[gnu::noinline]] static void parse_bucket2(deflator *d, iz end)
{
    parse_bucket_with(d, end, 2);
}

static void parse(deflator *d, iz end)
{
    if (d->lvl.bucket) {
        d->sampled = 1;
        d->lvl.bucket==2 ? parse_bucket2(d, end) : parse_bucket1(d, end);
        return;
    }
    if (!d->sampled) {
        sample(d);
    }
    if (d->narrow) {
        d->use3 ? parse_deep3(d, end) : parse_deep(d, end);
    } else if (d->lvl.lazy) {
        d->use3 ? parse_lazy3(d, end) : parse_lazy(d, end);
    } else {
        d->use3 ? parse_greedy3(d, end) : parse_greedy(d, end);
    }
}

// Rebase a table's entries, emptying those whose positions fall out of
// the window or were forgotten, either way at or below lim. Chain links
// that no insertion wrote get rebased too, harmlessly, since no chain
// leads to them, but without a branch on their unknown values, which
// MemorySanitizer would report.
static void slide_table(u32 *t, i32 n, u32 lim, u32 shift)
{
    for (i32 i = 0; i < n; i++) {
        u32 v = t[i];
        t[i] = (v - shift) & -(u32)(v > lim);
    }
}

// The same for a link table, through memcpy, as an earlier stream may
// have left distance links there. (Distance links need nothing.)
static void slide_links(u32 *t, u32 lim, u32 shift)
{
    u8 *b = (u8 *)t;
    for (iz i = 0; i < DEF_WSIZE*(iz)sizeof(u32); i += 4) {
        u32 v;
        __builtin_memcpy(&v, b+i, 4);
        v = (v - shift) & -(u32)(v > lim);
        __builtin_memcpy(b+i, &v, 4);
    }
}

// Whether the next slide would discard data of the block in progress,
// which must then end first, so that every block may be stored. Else a
// block of incompressible data that straddled a slide was coded with
// Huffman codes at a little over 8 bits per byte (on random data, 55
// bytes more than stored, for every other slide), and no simple bound
// on output held. This ends at most one block per slide (about 1 MiB of
// input), and that block is over 32 KiB long.
static b32 slide_ends_block(deflator *d)
{
    iz shift = d->pos - DEF_WSIZE;
    return d->ntok && shift>0 && d->blk_start<d->base+(u64)shift;
}

// Discard window contents more than WSIZE behind the parse position.
static void slide(deflator *d)
{
    iz shift = d->pos - DEF_WSIZE;
    if (shift <= 0) {
        return;
    }
    if (d->pend_len && d->pend_start < d->base+(u64)shift) {
        flush_pending(d, 0);
    }
    bytemove(d->win, d->win+shift, d->win_len-shift);
    d->base    += (u64)shift;
    d->win_len -= shift;
    d->pos     -= shift;
    d->ins     -= shift;

    u32 s = (u32)shift;
    u32 lim = d->stamp + s;
    if (d->lvl.bucket) {  // one or two entries a bucket, from head on
        slide_table(d->head, HASH_SIZE*d->lvl.bucket, lim, s);
        return;
    }
    slide_table(d->head, HASH_SIZE, lim, s);
    if (!d->narrow) {
        slide_links(d->prev, lim, s);
    }
    if (d->use3) {
        slide_table(d->head3, HASH_SIZE, lim, s);
        if (!d->narrow) {
            slide_links(d->prev3, lim, s);
        }
    }
}

// Forget all history by emptying the hash chains. Emptying the heads is
// enough: chains then lead only to positions inserted afterward, whose
// links are written on insertion. Positions inserted since the heads
// were last emptied all lie below ins, still in the window (a slide
// empties the heads of positions it discards), so after a short history
// it is cheapest to rehash them and clear only their slots. Otherwise,
// rather than clear 512 KiB, advance the stamp added to new entries by
// more than any position plus a window: measured from a new entry, as
// find_match does, every older one then lies farther back than any match
// reaches, as good as empty, even once a reset restarts positions at
// zero. (A slide also empties older entries, all being at or below its
// limit.) When the stamps run out, after 2,047 advances, the heads are
// cleared after all. Rehashing clears a position's slots in every layout
// (the heads, the 3-byte heads, and buckets of one and two entries,
// which span both), whatever the level that wrote them and whether it
// sampled for 3-byte matching: a reset may change the level, before or
// after forgetting, and so the layout.
static void forget(deflator *d)
{
    iz n = MIN(d->ins, d->win_len - (HASH_LEN - 1));
    if (n <= FORGET_REHASH) {
        for (iz p = 0; p < n; p++) {
            u32 v = load32(d->win + p);
            u32 h = hash4(v);
            d->head[h] = 0;
            d->head3[hash3(v)] = 0;
            d->head[2*h] = d->head[2*h+1] = 0;  // two-entry buckets
        }
    } else if (d->stamp < STAMP_LAST) {
        d->stamp += STAMP_STEP;
    } else {
        bytefill(d->head, 0, 2*HASH_SIZE*(iz)sizeof(u32));  // and head3
        d->stamp = 0;
    }
}

// Prepare to compress a new stream at the same level, unless changed
// with deflate_setlevel. This costs time in proportion to the previous
// stream, up to clearing the hash heads, and so is far cheaper than
// deflate_new for short streams.
static void deflate_reset(deflator *d)
{
    forget(d);
    d->olen       = d->ooff = 0;
    d->blkready   = d->flushed = 0;
    d->flushing   = DEF_NONE;
    d->finq       = 0;
    d->nqueued    = 0;
    d->win_len    = 0;
    d->base       = 0;
    d->pos        = d->ins = 0;
    d->sampled    = d->use3 = 0;
    d->blk_start  = 0;
    d->pend_start = 0;
    d->pend_len   = 0;
    d->bitbuf     = 0;
    d->bitcnt     = 0;
    clear_block(d);
}

// Levels 1 through 9; others are clamped. Only at the start of a stream.
static void deflate_setlevel(deflator *d, i32 level)
{
    level = MAX(1, MIN(level, 9));
    d->lvl = deflate_levels[level];
    d->narrow = level >= NARROW_LEVEL;
}

// Most bits a stored chunk's header takes: BFINAL and BTYPE, padding to
// a byte boundary, then LEN and NLEN. Padding is 5 bits from a boundary,
// and 7 when 6 or 7 bits were pending.
#define DEF_CHUNK_BITS  (3 + 7 + 32)
#define DEF_SLIDE_MIN   (WIN_CAP - LOOKAHEAD - DEF_WSIZE)  // least shift

// Most bytes of raw output for len bytes of input in a stream with no
// flush but FINISH. Output is 8 bits per input byte, plus at most
// DEF_CHUNK_BITS per "header", plus at most 7 bits of final padding:
// - A stored chunk is its header and its data. Full chunks (65,535
//   bytes) number at most len/65535. Partial ones come only when held
//   back data is flushed: before a Huffman block, at a slide, and at the
//   end (or else an empty final block of 10 bits).
// - A Huffman block is chosen only when its cost is no more than storing
//   its L bytes alone: one header per 65,535 bytes or part, so at most
//   L/65535 + 1 headers and 8L bits. This holds because a block always
//   may be stored (see slide_ends_block).
// - A block ends early, by its statistics (should_split), only once it
//   holds at least MIN_BLOCK bytes, and otherwise at TOK_LIMIT tokens,
//   which is longer, or before a slide, by then over 32 KiB. Of blocks
//   B, all but the last are at least MIN_BLOCK, so B <= len/MIN_BLOCK+1.
// - Slides discard at least DEF_SLIDE_MIN bytes each.
// So headers number at most len/65535 + 2B + len/DEF_SLIDE_MIN + 1.
// Each SYNC or FULL flush adds at most DEF_FLUSH_BOUND bytes: a block it
// ends (at most two headers if Huffman, its own and the held-back data
// before it, else one, flushing its own held-back data), and its empty
// stored block (one more).
#define DEF_FLUSH_BOUND  ((3*DEF_CHUNK_BITS + 7) / 8)
[[maybe_unused]] static u64 deflate_bound(u64 len)
{
    u64 blocks  = len/MIN_BLOCK + 1;
    u64 headers = len/65535 + 2*blocks + len/DEF_SLIDE_MIN + 1;
    return len + (headers*DEF_CHUNK_BITS + 7 + 7)/8;
}

// Memory needed by deflate_new, including alignment padding.
static iz deflate_memsize(void)
{
    return (iz)sizeof(deflator) + WIN_CAP + 2*HASH_SIZE*(iz)sizeof(u32) +
           2*DEF_WSIZE*(iz)sizeof(u32) + TOK_CAP*(iz)sizeof(token) +
           DEF_STAGE + HASH_SIZE/8 + 9*64;
}

// Tokens are written before they are read, and chains lead only to links
// that insertions wrote (see forget and slide_table), so of the large
// tables only the hash heads (and the sampling bitmap) start zeroed. The
// 3-byte heads follow the heads, and two-entry buckets span both.
static deflator *deflate_new(arena *a, i32 level)
{
    deflator *d = new(a, 1, deflator);
    deflate_setlevel(d, level);
    d->obuf  = newbytes(a, DEF_STAGE);
    d->win   = newbytes(a, WIN_CAP);
    d->head  = alloc(a, 2*HASH_SIZE, sizeof(u32), 64, 1);  // and head3
    d->head3 = d->head + HASH_SIZE;
    d->prev  = alloc(a, DEF_WSIZE, sizeof(u32), _Alignof(u32), 0);
    d->seen  = new(a, HASH_SIZE/32, u32);
    d->prev3 = alloc(a, DEF_WSIZE, sizeof(u32), _Alignof(u32), 0);
    d->toks  = alloc(a, TOK_CAP, sizeof(token), _Alignof(token), 0);

    for (i32 i = 0; i < 288; i++) {
        d->fixlit.len[i] = (u8)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
    }
    huff_codes(&d->fixlit, 288);
    for (i32 i = 0; i < NDIST; i++) {
        d->fixdist.len[i] = 5;
    }
    huff_codes(&d->fixdist, NDIST);

    for (i32 len = MIN_MATCH; len <= MAX_MATCH; len++) {
        d->lcode[len] = (u8)length_code(len);
    }
    for (i32 dist = 1; dist <= 256; dist++) {
        d->dcode[dist-1] = (u8)dist_code(dist);
    }
    for (i32 i = 2; i < 256; i++) {
        d->dcode[256+i] = (u8)dist_code((i<<7) + 1);
    }
    deflate_reset(d);
    return d;
}

// Ensure staging room for one emission step, compacting only if that
// makes the room: otherwise a caller taking a little output per call
// would pay on each call to move all that remains.
static b32 def_room(deflator *d)
{
    iz room = DEF_STAGE - d->olen;
    if (room<DEF_STAGE_NEED && room+d->ooff>=DEF_STAGE_NEED) {
        bytemove(d->obuf, d->obuf+d->ooff, d->olen-d->ooff);
        d->olen -= d->ooff;
        d->ooff = 0;
    }
    return DEF_STAGE-d->olen >= DEF_STAGE_NEED;
}

// Staged output not yet handed out.
static s8 deflate_pending(deflator *d)
{
    s8 r = {d->obuf + d->ooff, d->olen - d->ooff};
    return r;
}

// Mark the first n bytes of pending output as handed out.
static void deflate_consume(deflator *d, iz n)
{
    assert(n>=0 && n<=d->olen-d->ooff);
    d->ooff += n;
    if (d->ooff == d->olen) {
        d->ooff = d->olen = 0;
    }
}

static void def_drain(deflator *d, zbuf *b)
{
    s8 p = deflate_pending(d);
    iz n = MIN(p.len, b->outlen);
    if (n) {
        bytecopy(b->out, p.s, n);
        b->out += n;
        b->outlen -= n;
        deflate_consume(d, n);
    }
}

// Emit everything parsed so far for the given flush.
static void def_flush(deflator *d, i32 flush)
{
    switch (flush) {
    case DEF_FINISH:
        flush_block(d, 1);
        bw_align(d);
        break;
    case DEF_SYNC:
    case DEF_FULL:
        flush_block(d, 0);
        flush_pending(d, 0);
        emit_stored_chunk(d, 0, 0, 0);
        if (flush == DEF_FULL) {
            forget(d);
            d->ins = d->pos;
        }
        break;
    }
}

// The mode of the latest flush due, or DEF_NONE if none is.
static i32 def_lastdue(deflator *d)
{
    return d->finq      ? DEF_FINISH :
           d->nqueued&1 ? DEF_SYNC + DEF_FULL - d->flushing :
           d->flushing;
}

// Compress from b->in into b->out, advancing both. With DEF_NONE,
// returns GZ_NEEDIN once all input is consumed, though output may remain
// staged. Otherwise returns GZ_OK once all input is consumed, the flush
// is complete, and all output is delivered. In any mode, returns
// GZ_NEEDOUT, perhaps with input left, when the output buffer is full
// and either staging lacks room for the next emission step or a flush's
// output remains.
//
// A flush falls due once its call has consumed all input, and later
// calls complete it before anything else. While flushes are unfinished,
// a call never consumes input, and one with no input in the mode of the
// latest due flush only completes them (returning GZ_OK once all are
// complete): it is a repeat. A no-input SYNC, FULL, or FINISH in another
// mode falls due at once, queued behind the others, so the stream is
// the one a caller waiting for each flush to complete would get. Any
// other call completes the unfinished flushes, then goes on with its own
// input and mode. After DEF_FINISH falls due, queued or not, only
// repeats are valid.
//
// Repeats make adjacent queued flushes differ in mode, so the queue is
// a run of SYNC and FULL alternating from flushing, perhaps then FINISH:
// a count and a flag hold it exactly.
//
// Output may also be taken without copying through deflate_pending and
// deflate_consume, in which case b->out may be empty.
static i32 deflate_stream(deflator *d, zbuf *b, i32 flush)
{
    if ((d->flushing==DEF_FINISH || d->finq) &&
        (b->inlen || flush!=DEF_FINISH)) {
        return GZ_EUSAGE;
    }

    if (d->flushing && flush && !b->inlen && flush!=def_lastdue(d)) {
        if (flush == DEF_FINISH) {
            d->finq = 1;
        } else {
            d->nqueued++;
        }
    }

    for (;;) {
        def_drain(d, b);
        if (d->flushed) {
            if (d->ooff < d->olen) {
                return GZ_NEEDOUT;
            } else if (d->flushing == DEF_FINISH) {
                return GZ_OK;
            }
            d->flushed = 0;
            if (d->nqueued) {
                d->nqueued--;
                d->flushing = DEF_SYNC + DEF_FULL - d->flushing;
                continue;
            } else if (d->finq) {
                d->finq = 0;
                d->flushing = DEF_FINISH;
                continue;
            }
            b32 repeat = flush==d->flushing && !b->inlen;
            d->flushing = DEF_NONE;
            if (repeat) {
                return GZ_OK;
            }
        }
        if (!d->flushing && !b->inlen) {
            d->flushing = flush;
        }

        if (d->blkready) {
            if (!def_room(d)) {
                return GZ_NEEDOUT;
            }
            flush_block(d, 0);
            d->blkready = 0;
        } else if (d->win_len == WIN_CAP) {
            parse(d, WIN_CAP - LOOKAHEAD);
            if (!d->blkready && slide_ends_block(d)) {
                d->blkready = 1;  // emitted on the next pass, then slide
            } else if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                slide(d);
            }
        } else if (d->flushing) {
            parse(d, d->win_len);
            if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                def_flush(d, d->flushing);
                d->flushed = 1;
            }
        } else if (b->inlen) {
            iz n = MIN(b->inlen, WIN_CAP - d->win_len);
            bytecopy(d->win + d->win_len, b->in, n);
            d->win_len += n;
            b->in += n;
            b->inlen -= n;
        } else {
            return GZ_NEEDIN;
        }
    }
}

// tugz core: zlib (RFC 1950) and gzip (RFC 1952) containers around raw
// DEFLATE, as streaming decoders and encoders
//
// The gzip decoder handles one member at a time: after a member ends,
// the next call with input begins parsing another. Bytes that are not a gzip header
// produce GZ_ENOTGZ, leaving trailing data policy to the caller. As in
// GNU gzip, a header may begin with the old magic, 1f 9e, as well as 1f 8b.

enum {
    FMT_RAW,
    FMT_ZLIB,
    FMT_GZIP,
};

enum {
    FTEXT    = 1 << 0,
    FHCRC    = 1 << 1,
    FEXTRA   = 1 << 2,
    FNAME    = 1 << 3,
    FCOMMENT = 1 << 4,
};

static void put32le(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 get32le(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static u32 get32be(u8 const *p)
{
    return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | (u32)p[3];
}

// The check of more data, with crc32_update's cache of the CPU's answer.
// Kept out of line: with the SSE2 Adler-32 out of line too, GCC inlined
// this into the stream loops, which cost gzip decoding 0.6-0.7%.
[[gnu::noinline]] static u32 check_update(i32 format, u32 check, u8 const *p, iz len,
                        i32 *cpu)
{
    switch (format) {
    case FMT_ZLIB: return adler32_update(check, p, len);
    case FMT_GZIP: return crc32_update(check, p, len, cpu);
    }
    return 0;
}

static u32 check_init(i32 format)
{
    return format==FMT_ZLIB ? 1 : 0;
}

enum {
    DEC_FIXED,    // gzip: first 10 bytes; zlib: 2, and any dictionary ID
    DEC_XLEN,
    DEC_EXTRA,
    DEC_NAME,
    DEC_COMMENT,
    DEC_HCRC,
    DEC_BODY,
    DEC_TRAILER,
    DEC_DONE,
};

typedef struct {
    inflator *inf;
    i32 format;
    i32 state;
    i32 err;      // sticky container error
    u32 check;    // of the output so far
    u64 total;
    u8  buf[10];  // header or trailer bytes
    i32 len;      // bytes in buf
    i32 hpos;     // header bytes consumed, saturated at 2
    u32 hcrc;
    i32 flg;
    i32 xlen;
} decoder;

static iz decoder_memsize(void)
{
    return (iz)sizeof(decoder) + 64 + inflate_memsize();
}

// Start a new stream (or gzip member) in the same format, from any state.
static void decoder_reset(decoder *z)
{
    inflate_reset(z->inf);
    z->state = z->format==FMT_RAW ? DEC_BODY : DEC_FIXED;
    z->check = check_init(z->format);
    z->total = 0;
    z->len   = 0;
    z->hpos  = 0;
    z->hcrc  = 0;
    z->err   = 0;
}

static decoder *decoder_new(arena *a, i32 format)
{
    decoder *z = new(a, 1, decoder);
    z->inf = inflate_new(a);
    z->format = format;
    decoder_reset(z);
    return z;
}

// Memory needed by decoder64_new, including alignment padding.
[[maybe_unused]] static iz decoder64_memsize(void)
{
    return (iz)sizeof(decoder) + 64 + inflate64_memsize();
}

// A raw Deflate64 decoder (no container), otherwise used as decoder_new's.
[[maybe_unused]] static decoder *decoder64_new(arena *a)
{
    decoder *z = new(a, 1, decoder);
    z->inf = inflate64_new(a);
    z->format = FMT_RAW;
    decoder_reset(z);
    return z;
}

// Advance the gzip header state past fields the flags say are absent.
static void gzip_skip_absent(decoder *z)
{
    for (;;) {
        switch (z->state) {
        case DEC_XLEN:    if (z->flg & FEXTRA)   return; break;
        case DEC_EXTRA:   if (z->xlen)           return; break;
        case DEC_NAME:    if (z->flg & FNAME)    return; break;
        case DEC_COMMENT: if (z->flg & FCOMMENT) return; break;
        case DEC_HCRC:    if (z->flg & FHCRC)    return; break;
        default:          return;
        }
        z->state++;
    }
}

// Consume one byte of a fixed-size gzip header field: the first 10
// bytes, XLEN, or the header CRC. The header CRC is computed only when
// FHCRC asks for it to be checked.
static i32 gzip_header_byte(decoder *z, u8 c)
{
    z->hpos += z->hpos < 2;
    switch (z->state) {
    case DEC_FIXED:
        // Each field is validated as soon as it is complete, like zlib.
        // As in GNU gzip, the old magic, 1f 9e, also begins a member.
        z->buf[z->len++] = c;
        if (z->len==2 && (z->buf[0]!=0x1f ||
                          (z->buf[1]!=0x8b && z->buf[1]!=0x9e))) {
            return GZ_ENOTGZ;
        } else if (z->len==4 && z->buf[2]!=8) {
            return GZ_EMETHOD;
        } else if (z->len==4 && (z->buf[3] & 0xe0)) {
            return GZ_EFLAGS;
        } else if (z->len < 10) {
            return GZ_OK;
        }
        z->flg = z->buf[3];
        if (z->flg & FHCRC) {
            z->hcrc = crc32_update(0, z->buf, 10, &z->inf->cpu);
        }
        z->len = 0;
        z->xlen = 0;
        z->state = DEC_XLEN;
        break;
    case DEC_XLEN:
        if (z->flg & FHCRC) {
            z->hcrc = crc32_update(z->hcrc, &c, 1, &z->inf->cpu);
        }
        z->xlen |= c << 8*z->len++;
        if (z->len < 2) {
            return GZ_OK;
        }
        z->len = 0;
        z->state = DEC_EXTRA;
        break;
    case DEC_HCRC:
        z->buf[z->len++] = c;
        if (z->len < 2) {
            return GZ_OK;
        } else if ((z->buf[0] | z->buf[1]<<8) != (i32)(z->hcrc & 0xffff)) {
            return GZ_EHCRC;
        }
        z->state = DEC_BODY;
        break;
    }
    gzip_skip_absent(z);
    return GZ_OK;
}

// Consume as much of a variable gzip header field (the extra field, the
// name, or the comment) as b->in holds, at least a byte, in one span
// rather than byte by byte, as these may be large.
static void gzip_header_span(decoder *z, zbuf *b)
{
    iz  n = 0;
    b32 done;
    if (z->state == DEC_EXTRA) {
        n = MIN(z->xlen, b->inlen);
        z->xlen -= (i32)n;
        done = !z->xlen;
    } else {
        for (; n<b->inlen && b->in[n]; n++) {}
        done = n < b->inlen;
        n += done;  // the terminating zero
    }
    if (z->flg & FHCRC) {
        z->hcrc = crc32_update(z->hcrc, b->in, n, &z->inf->cpu);
    }
    z->hpos = (i32)MIN(z->hpos+MIN(n, 2), 2);  // fields may pass 2^31
    b->in += n;
    b->inlen -= n;
    if (done) {
        z->state++;
        gzip_skip_absent(z);
    }
}

// Consume one zlib header byte. Validation follows zlib's order. Preset
// dictionaries (FDICT) are unsupported, but like zlib, which then asks
// for the dictionary, the decoder first reads its 4-byte ID, so that
// input ending sooner is truncation, as there.
static i32 zlib_header_byte(decoder *z, u8 c)
{
    z->hpos += z->hpos < 2;
    z->buf[z->len++] = c;
    if (z->len != 2) {
        return z->len<6 ? GZ_OK : GZ_EHEADER;  // CMF, or the dictionary ID
    }
    u32 cmf = z->buf[0];
    u32 flg = z->buf[1];
    if ((cmf<<8 | flg) % 31) {
        return GZ_EHEADER;
    } else if ((cmf & 15) != 8) {
        return GZ_EMETHOD;
    } else if ((cmf >> 4) > 7) {
        return GZ_EHEADER;  // window larger than 32 KiB
    } else if (flg & 0x20) {
        return GZ_OK;  // a preset dictionary: its ID follows
    }
    z->len = 0;
    z->state = DEC_BODY;
    return GZ_OK;
}

// Verify each trailer field as soon as it is complete, like zlib.
static i32 trailer_check(decoder *z)
{
    if (z->len != 4) {
        return get32le(z->buf+4)==(u32)z->total ? GZ_OK : GZ_ELEN;
    } else if (z->format == FMT_ZLIB) {
        return get32be(z->buf)==z->check ? GZ_OK : GZ_ECRC;
    }
    return get32le(z->buf)==z->check ? GZ_OK : GZ_ECRC;
}

// Consume only header bytes from b->in, so that a caller can learn
// whether a stream begins as it should before decoding any of it.
// Returns GZ_OK once the body is next, GZ_NEEDIN, or an error, which is
// sticky. decoder_run continues from there.
static i32 decoder_header(decoder *z, zbuf *b)
{
    while (!z->err && z->state<DEC_BODY) {
        if (!b->inlen) {
            return GZ_NEEDIN;
        } else if (z->state>=DEC_EXTRA && z->state<=DEC_COMMENT) {
            gzip_header_span(z, b);  // only gzip has these
            continue;
        }
        u8 c = *b->in++;
        b->inlen--;
        z->err = z->format==FMT_ZLIB ? zlib_header_byte(z, c)
                                     : gzip_header_byte(z, c);
    }
    return z->err;
}

// Decode from b->in into b->out, advancing both. Returns GZ_OK at the
// end of the stream (or gzip member), with b->in just past it, and again
// on later calls, consuming nothing, except that a call with input after
// a gzip member begins the next. So GZ_OK with no input left is a clean
// end, even between gzip members, and GZ_NEEDIN with none means the
// input is truncated. Otherwise returns GZ_NEEDIN, GZ_NEEDOUT, or an
// error, which is sticky.
static i32 decoder_run(decoder *z, zbuf *b)
{
    if (z->err) {
        return z->err;
    } else if (z->state==DEC_DONE && z->format==FMT_GZIP && b->inlen) {
        decoder_reset(z);
    }

    for (;;) {
        switch (z->state) {
        case DEC_DONE:
            return GZ_OK;

        case DEC_BODY: {
            // Lengths come from the counts: both pointers may be null
            u8 *out   = b->out;
            iz  avail = b->outlen;
            i32 r = inflate_stream(z->inf, b);
            iz  n = avail - b->outlen;
            if (z->format != FMT_RAW) {
                z->check = check_update(z->format, z->check, out, n,
                                        &z->inf->cpu);
            }
            z->total += (u64)n;
            if (r != GZ_OK) {
                return r;
            }
            z->state = z->format==FMT_RAW ? DEC_DONE : DEC_TRAILER;
            z->len = 0;
        } break;

        case DEC_TRAILER: {
            i32 need = z->format==FMT_ZLIB ? 4 : 8;
            while (z->len < need) {
                if (!b->inlen) {
                    return GZ_NEEDIN;
                }
                z->buf[z->len++] = *b->in++;
                b->inlen--;
                if (z->len==4 || z->len==8) {
                    z->err = trailer_check(z);
                    if (z->err) {
                        return z->err;
                    }
                }
            }
            z->state = DEC_DONE;
        } break;

        default: {
            i32 r = decoder_header(z, b);
            if (r != GZ_OK) {
                return r;
            }
        } break;
        }
    }
}

typedef struct {
    deflator *def;
    i32 format;
    u32 check;    // of the input so far
    u64 total;
    b32 done;     // trailer written
    i32 cpu;      // for crc32_update, kept across resets
} encoder;

static iz encoder_memsize(void)
{
    return (iz)sizeof(encoder) + 64 + deflate_memsize();
}

// Start a new stream in the same format at a level, which may differ
// from the previous stream's. Like deflate_reset, this costs time in
// proportion to the previous stream.
static void encoder_reset(encoder *e, i32 level)
{
    level = MAX(1, MIN(level, 9));
    deflate_reset(e->def);
    deflate_setlevel(e->def, level);
    e->check = check_init(e->format);
    e->total = 0;
    e->done  = 0;

    switch (e->format) {
    case FMT_GZIP: {
        // XFL marks the fastest and the smallest levels, as GNU gzip and
        // zlib mark them
        u8 header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        header[8] = level==1 ? 4 : level==9 ? 2 : 0;
        deflate_bytes(e->def, header, countof(header));
    } break;
    case FMT_ZLIB: {
        // Same header as zlib for a 32 KiB window at this level
        u32 flevel = level<2 ? 0 : level<6 ? 1 : level==6 ? 2 : 3;
        u32 cmf = 0x78;
        u32 flg = flevel << 6;
        flg |= 31 - (cmf<<8 | flg)%31;
        u8 header[2] = {(u8)cmf, (u8)flg};
        deflate_bytes(e->def, header, countof(header));
    } break;
    }
}

// Most bytes of output for len bytes of input in one stream of the
// format with no flush but FINISH: the raw bound plus the header and
// trailer. Each SYNC or FULL flush may add DEF_FLUSH_BOUND more.
[[maybe_unused]] static u64 encoder_bound(i32 format, u64 len)
{
    u64 wrap = format==FMT_GZIP ? 10+8 : format==FMT_ZLIB ? 2+4 : 0;
    return deflate_bound(len) + wrap;
}

static encoder *encoder_new(arena *a, i32 format, i32 level)
{
    encoder *e = new(a, 1, encoder);
    e->def = deflate_new(a, level);
    e->format = format;
    encoder_reset(e, level);
    return e;
}

// Compress from b->in into b->out with a DEF_* flush mode, appending the
// trailer once the raw stream has finished. Returns as deflate_stream.
//
// The trailer is staged as soon as the final block is, though output may
// remain, so that a caller draining output gets both together: the step
// that staged the block had room for more than a trailer.
static i32 encoder_run(encoder *e, zbuf *b, i32 flush)
{
    deflator *d  = e->def;
    u8 const *in = b->in;
    iz avail = b->inlen;
    i32 r = deflate_stream(d, b, flush);
    iz n = avail - b->inlen;  // not b->in-in: both may be null
    if (e->format != FMT_RAW) {
        e->check = check_update(e->format, e->check, in, n, &e->cpu);
    }
    e->total += (u64)n;

    if (d->flushing==DEF_FINISH && d->flushed && !e->done) {
        u8 trailer[8];
        switch (e->format) {
        case FMT_GZIP:
            put32le(trailer+0, e->check);
            put32le(trailer+4, (u32)e->total);
            deflate_bytes(d, trailer, 8);
            break;
        case FMT_ZLIB:
            for (i32 i = 0; i < 4; i++) {
                trailer[i] = (u8)(e->check >> (24 - 8*i));
            }
            deflate_bytes(d, trailer, 4);
            break;
        }
        e->done = 1;
        r = deflate_stream(d, b, flush);
    }
    return r;
}


// tugz application I/O: platform interface, buffered reader and writer
//
// Only programs include this file. The library layer does not, so the
// codec itself never touches the os_* functions declared here. Each
// program declares its own additional platform functions (gzipio.c,
// zipcli.c), and may leave some of these helpers unused.

// Platform interface. File descriptors 0, 1, and 2 are standard input,
// output, and error. Paths are UTF-8 (WTF-8 on Windows).
enum {
    // Open an existing file for reading. Directories are always refused.
    OS_READ     = 0,
    OS_REGULAR  = 1 << 0,  // refuse anything but a regular file
    OS_NOFOLLOW = 1 << 1,  // refuse symbolic links (and reparse points)
    OS_ONELINK  = 1 << 2,  // refuse files with multiple hard links
    OS_NOSETID  = 1 << 3,  // refuse set-user-ID and set-group-ID (POSIX)
    OS_NOSTICKY = 1 << 4,  // refuse files with the sticky bit (POSIX)

    // Create a file for writing. On POSIX it is created owner-only, for
    // the program to give it permissions later (gzip's os_setmeta, zip's
    // os_commit), or with the defaults for a new file (umask, inherited
    // ACL) given OS_DEFPERMS; on Windows it is opened exclusively and
    // inherits access control either way. It is discarded when closed,
    // or if the process is interrupted, unless first kept (os_keep) or
    // committed (zip's os_commit).
    OS_CREATE   = 1 << 5,  // fail if it exists
    OS_FORCE    = 1 << 6,  // replace if it exists, OS_ERR if it cannot
    OS_DEFPERMS = 1 << 7,
};
enum {
    OS_ERR      = -1,
    OS_EEXIST   = -2,
    OS_EISDIR   = -3,
    OS_ESYMLINK = -4,
    OS_ENOTREG  = -5,
    OS_ELINKS   = -6,
    OS_ESETUID  = -7,
    OS_ESETGID  = -8,
    OS_ESTICKY  = -9,
};

// Returns a non-negative descriptor or a negative OS_E* code.
static i32  os_open(os *, s8 path, i32 mode, arena scratch);
// Returns false if the file could not be closed cleanly, meaning written
// data may be lost.
static b32  os_close(os *, i32 fd);
// Returns the number of bytes read, 0 at end of file, or -1 on error.
static iz   os_read(os *, i32 fd, u8 *buf, iz cap);
// Writes all bytes or returns false.
static b32  os_write(os *, i32 fd, u8 *buf, iz len);
static b32  os_remove(os *, s8 path, arena scratch);
// Keep a created file when it is closed instead of discarding it.
// Returns false if it cannot be kept (on Windows, where the file system
// refuses to cancel its deletion), and closing it still discards it.
static b32  os_keep(os *, i32 fd);
// Exit with a status. A created file not yet kept is discarded. On
// Windows, once a console control event is ending the process, it waits
// for that end instead (see writer_claim).
[[noreturn]] static void os_exit(os *, i32 status);
// Milliseconds on a monotonic clock, from an arbitrary start, as coarse
// as a system timer's tick. It may wrap (Windows, after 49 days).
static i64  os_now(os *);

typedef struct {
    os *ctx;
    u8 *buf;
    iz  len;
    iz  off;
    iz  cap;
    i32 fd;
    b32 eof;
    b32 err;
} reader;

[[maybe_unused]] static reader *newreader(arena *a, i32 fd, iz cap)
{
    reader *r = new(a, 1, reader);
    r->ctx = a->ctx;
    r->buf = newbytes(a, cap);
    r->cap = cap;
    r->fd  = fd;
    return r;
}

// Ensure at least one byte is buffered. Returns false at end of input.
[[maybe_unused]] static b32 reader_fill(reader *r)
{
    if (r->off < r->len) {
        return 1;
    }
    r->off = r->len = 0;
    if (r->eof) {
        return 0;
    }
    iz n = os_read(r->ctx, r->fd, r->buf, r->cap);
    if (n <= 0) {
        r->eof = 1;
        r->err = n < 0;
        return 0;
    }
    r->len = n;
    return 1;
}

// A writer with a negative descriptor discards output. One to a
// terminal (tty), which a program sets for its messages, never its data,
// notes when its buffer last began to fill, for writer_poll.
typedef struct {
    os *ctx;
    u8 *buf;
    iz  len;    // stored with release, for writer_claim
    iz  cap;
    i32 fd;
    b32 err;
    b32 tty;
    i32 state;  // WRITER_*, for writer_claim
    i64 since;  // when the buffered text began to wait
} writer;

// A writer's state: idle, writing its buffer (or a large write past it),
// or claimed by another thread (writer_claim), never to write again.
enum { WRITER_IDLE, WRITER_WRITING, WRITER_CLAIMED };

enum { WRITER_WAIT = 100 };  // milliseconds, writer_poll's

[[maybe_unused]] static writer *newwriter(arena *a, i32 fd, iz cap)
{
    writer *w = new(a, 1, writer);
    w->ctx = a->ctx;
    w->buf = newbytes(a, cap);
    w->cap = cap;
    w->fd  = fd;
    return w;
}

// Mark the writer writing, around each write, unless another thread has
// claimed it (writer_claim): then this one stops, as os_exit waits for
// the process to end.
[[maybe_unused]] static void writer_begin(writer *w)
{
    i32 idle = WRITER_IDLE;
    if (!__atomic_compare_exchange_n(&w->state, &idle, WRITER_WRITING, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        os_exit(w->ctx, 1);
    }
}

[[maybe_unused]] static void writer_end(writer *w)
{
    __atomic_store_n(&w->state, WRITER_IDLE, __ATOMIC_RELEASE);
}

// Claim the writer for a thread of the process's last moments, a console
// control handler on Windows, which writes what it holds as the process
// ends. Returns the length of its text, buf[0, len), or -1 while the
// writer's own thread is writing (try again). Once claimed, that thread
// stops at its next write (writer_begin), never touching that text: it
// only appends past the length taken, whose bytes it stored before it
// stored the length (release, here acquire), and its buffer, error, and
// length are changed back only while writing, which it now cannot.
[[maybe_unused]] static iz writer_claim(writer *w)
{
    i32 idle = WRITER_IDLE;
    if (!__atomic_compare_exchange_n(&w->state, &idle, WRITER_CLAIMED, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        return -1;
    }
    return __atomic_load_n(&w->len, __ATOMIC_ACQUIRE);
}

// The buffer is emptied before it is written, so that a signal handler
// that writes what is buffered (zip's, on POSIX) never writes it twice.
[[maybe_unused]] static b32 writer_flush(writer *w)
{
    iz len = w->len;
    if (len) {
        writer_begin(w);
        w->len = 0;
        if (!w->err && w->fd>=0) {
            w->err = !os_write(w->ctx, w->fd, w->buf, len);
        }
        writer_end(w);
    }
    return !w->err;
}

[[maybe_unused]] static void writer_write(writer *w, u8 const *p, iz len)
{
    if (w->fd < 0) {
        return;
    } else if (len > w->cap-w->len) {
        writer_flush(w);
        if (len >= w->cap) {
            // Large writes bypass the buffer
            writer_begin(w);
            if (!w->err) {
                w->err = !os_write(w->ctx, w->fd, (u8 *)p, len);
            }
            writer_end(w);
            return;
        }
    }
    while (len) {
        if (w->len == w->cap) {
            writer_flush(w);
        }
        if (!w->len && w->tty) {
            w->since = os_now(w->ctx);
        }
        iz take = MIN(len, w->cap-w->len);
        bytecopy(w->buf+w->len, p, take);
        __atomic_store_n(&w->len, w->len+take, __ATOMIC_RELEASE);
        p += take;
        len -= take;
    }
}

[[maybe_unused]] static void writer_s8(writer *w, s8 s)
{
    writer_write(w, s.s, s.len);
}

[[maybe_unused]] static void writer_byte(writer *w, u8 b)
{
    if (w->len == w->cap) {
        writer_flush(w);
    }
    if (!w->len && w->tty) {
        w->since = os_now(w->ctx);
    }
    w->buf[w->len] = b;
    __atomic_store_n(&w->len, w->len+1, __ATOMIC_RELEASE);
}

// A check point, called between pieces of a program's work: text to a
// terminal is flushed once it has waited WRITER_WAIT, as Nagle's
// algorithm holds small packets. Output is otherwise flushed only when
// full, or by the program before a message to standard error, or a
// prompt, and at the end. A line then shows within WRITER_WAIT and a
// piece of work, a flood of lines is written ten times a second, and a
// fast run is written at once, at the end. The clock is read only with
// text waiting, so polling costs nothing for a pipe or file.
[[maybe_unused]] static void writer_poll(writer *w)
{
    if (w->tty && w->len) {
        i64 now = os_now(w->ctx);
        if (now-w->since>=WRITER_WAIT || now<w->since) {  // (or wrapped)
            writer_flush(w);
        }
    }
}

[[maybe_unused]] static b32 s8equals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// Carve out exactly the memory a codec claims to need, as the library
// does, so that every run checks the claim.
[[maybe_unused]] static arena subarena(arena *a, iz size)
{
    arena r = {0};
    r.beg = (byte *)newbytes(a, size);
    r.end = r.beg + size;
    r.ctx = a->ctx;
    return r;
}

// Hand pending output to a descriptor, if any. Returns false on failure.
[[maybe_unused]] static b32 put_pending(os *ctx, i32 fd, s8 p)
{
    return !p.len || fd<0 || os_write(ctx, fd, p.s, p.len);
}

// tugz gzip program I/O: descriptor drivers for the codec

static b32  os_isatty(os *, i32 fd);
// Whether the os_open that just failed (OS_ERR) found no such file.
static b32  os_missing(os *);
// Why the os_* call that just failed did, in the system's words (as C's
// strerror gives them), or an empty string if unknown. The text may last
// only until the next call (the BSDs' strerror reuses one buffer).
static s8   os_error(os *);
// Whether the os_write that just failed found a pipe with no reader.
static b32  os_pipeclosed(os *);
// An open file's permissions, ownership, and timestamps, as far as the
// platform keeps them.
typedef struct osmeta osmeta;
// An input's metadata, taken before reading changes its access time, or
// null if it cannot be read (os_error says why).
static osmeta *os_getmeta(os *, i32 fd, arena *);
// Give an open output file metadata, its ownership only where permitted.
// Returns false if its permissions or timestamps could not be set (with
// os_error saying why), though as much as could be is set.
static b32  os_setmeta(os *, i32 fd, osmeta *);

static void os_oom(os *ctx)
{
    s8 msg = S("gzip: out of memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, 1);
}

// The arena is fixed in size, ample for the codecs.
static void os_extend(os *ctx, arena *a, iz need)
{
    (void)a;
    (void)need;
    os_oom(ctx);
}

// Decoded output not yet handed out, for copy-free delivery.
static s8 decoder_pending(decoder *z)
{
    return inflate_pending(z->inf);
}

static void decoder_consume(decoder *z, iz n)
{
    s8 p = inflate_pending(z->inf);
    if (z->format != FMT_RAW) {
        z->check = check_update(z->format, z->check, p.s, n, &z->inf->cpu);
    }
    z->total += (u64)n;
    inflate_consume(z->inf, n);
}

static s8 encoder_pending(encoder *e)
{
    return deflate_pending(e->def);
}

static void encoder_consume(encoder *e, iz n)
{
    deflate_consume(e->def, n);
}

#define IO_RDBUF  (1 << 18)

// An encoder for gzip_compress, which can reuse it for any number of
// streams in turn.
static encoder *gzip_encoder(arena *perm, i32 level)
{
    arena a = subarena(perm, encoder_memsize());
    return encoder_new(&a, FMT_GZIP, level);
}

// Compress a descriptor into a descriptor as a new stream at a level,
// first resetting the encoder: for small inputs this costs a fraction
// of a new encoder, so one serves every file.
//
// As in GNU gzip, a read or write error stops at once: a stream left
// unfinished by a read error cannot pass for all of the input.
static i32 gzip_compress(encoder *e, i32 in, i32 out, i32 level,
                         arena scratch)
{
    encoder_reset(e, level);
    reader *r = newreader(&scratch, in, IO_RDBUF);
    for (;;) {
        b32 more = reader_fill(r);
        if (r->err) {
            return GZ_EREAD;
        }
        zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
        i32 status = encoder_run(e, &b, more ? DEF_NONE : DEF_FINISH);
        r->off = r->len - b.inlen;
        if (status != GZ_NEEDIN) {
            s8 p = encoder_pending(e);
            if (!put_pending(r->ctx, out, p)) {
                return GZ_EWRITE;
            }
            encoder_consume(e, p.len);
        }
        if (status == GZ_OK) {
            return GZ_OK;
        }
    }
}

// A decoder in a FMT_* format for stream_decompress, which can reuse it
// for any number of streams in turn.
static decoder *stream_decoder(arena *perm, i32 format)
{
    arena a = subarena(perm, decoder_memsize());
    return decoder_new(&a, format);
}

// Settle input that is not a gzip member, of which the decoder took the
// first bytes (fewer than two at the end of the input), following GNU
// gzip. A lone byte may have begun the magic, so it is a truncation
// unless zero. After a member, zero bytes to the end are padding (as on
// tape), and other data is ignored with a warning.
static i32 not_gzip(decoder *z, reader *r, b32 first)
{
    s8 head = {z->buf, z->len};
    if (head.len<2 && (!head.len || head.s[0])) {
        return GZ_ETRUNC;
    } else if (first) {
        return GZ_ENOTGZ;
    }

    b32 zeros = !head.s[0] && (head.len<2 || !head.s[1]);
    while (zeros && reader_fill(r)) {
        for (; zeros && r->off<r->len; r->off++) {
            zeros = !r->buf[r->off];
        }
    }
    if (!zeros) {
        return GZ_TRAILING;
    }
    return r->err ? GZ_EREAD : GZ_OK;
}

// Copy input that is not a gzip member to the output unchanged, from the
// first bytes, which the decoder took.
static i32 pass_through(decoder *z, reader *r, i32 out)
{
    if (!put_pending(r->ctx, out, (s8){z->buf, z->len})) {
        return GZ_EWRITE;
    }
    while (reader_fill(r)) {
        s8 rest = {r->buf+r->off, r->len-r->off};
        if (!put_pending(r->ctx, out, rest)) {
            return GZ_EWRITE;
        }
        r->off = r->len;
    }
    return r->err ? GZ_EREAD : GZ_OK;
}

// A reader for stream_header and stream_decode.
static reader *stream_reader(arena *scratch, i32 in)
{
    return newreader(scratch, in, IO_RDBUF);
}

// Reset the decoder and read only the header of the stream, or of the
// first gzip member, so that a caller can learn that the input is in the
// format before creating an output, as GNU gzip reads the header before
// creating one. Returns whether the body is next. Either way,
// stream_decode continues, and if not, settles the status.
static b32 stream_header(decoder *z, reader *r)
{
    decoder_reset(z);
    i32 status = GZ_NEEDIN;
    while (status==GZ_NEEDIN && reader_fill(r)) {
        zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
        status = decoder_header(z, &b);
        r->off = r->len - b.inlen;
    }
    return status == GZ_OK;
}

// Decompress a stream in the decoder's format, or for gzip all members,
// from a reader into a descriptor, continuing from a decoder that is
// reset (decoder_reset), and perhaps past the header (stream_header).
// One decoder so serves every file. A negative output only verifies.
//
// Following GNU gzip, data after the last gzip member is ignored with a
// warning (GZ_TRAILING), unless it is only zero bytes, which is fine, or
// starts with the gzip magic, in which case it must be a valid member.
// With copy, as for GNU's gzip -cdf (zcat -f), data that is not gzip,
// from the start or after a member, is instead copied unchanged.
//
// As in GNU gzip, a read or write error stops at once.
static i32 stream_decode(decoder *z, reader *r, i32 out, b32 copy)
{
    i32 format = z->format;
    for (b32 first = 1;; first = 0) {
        i32 status = GZ_NEEDIN;
        for (;;) {
            // After GZ_NEEDOUT the decoder has output, or an error, to
            // hand over before it needs input
            b32 more = status==GZ_NEEDOUT || reader_fill(r);
            if (r->err) {
                return GZ_EREAD;
            }
            zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
            status = decoder_run(z, &b);
            r->off = r->len - b.inlen;
            s8 p = decoder_pending(z);
            if (!put_pending(r->ctx, out, p)) {
                return GZ_EWRITE;
            }
            decoder_consume(z, p.len);
            if ((status!=GZ_NEEDIN || !more) && status!=GZ_NEEDOUT) {
                break;
            }
        }

        // Two bytes without the gzip magic, or fewer at the end of input
        b32 ended = status == GZ_NEEDIN;
        if (format==FMT_GZIP && (status==GZ_ENOTGZ || (ended && z->hpos<2))) {
            return copy ? pass_through(z, r, out) : not_gzip(z, r, first);
        } else if (ended) {
            return GZ_ETRUNC;  // inside a stream or member
        } else if (status!=GZ_OK || format!=FMT_GZIP) {
            return status;
        } else if (!reader_fill(r)) {
            return r->err ? GZ_EREAD : GZ_OK;
        }
    }
}

// Decompress from a descriptor, as stream_decode, first resetting the
// decoder.
static i32 stream_decompress(decoder *z, i32 in, i32 out, b32 copy,
                             arena scratch)
{
    decoder_reset(z);
    return stream_decode(z, stream_reader(&scratch, in), out, copy);
}

// tugz core: command line interface
//
// Exit status follows GNU gzip: 0 for success, 1 for errors, 2 for
// warnings (e.g. trailing garbage, file skipped). Errors take precedence.

typedef struct {
    arena perm;
    s8    name;   // program name without directory or extension
    s8   *args;   // excluding the program name
    i32   nargs;
} config;

enum {
    EXIT_OK   = 0,
    EXIT_ERR  = 1,
    EXIT_WARN = 2,
};

typedef struct {
    i32 level;
    b32 decompress;
    b32 test;
    b32 to_stdout;
    b32 force;
    b32 keep;
    b32 quiet;
    b32 stop;      // a read or write error ends the run, as in GNU gzip
    encoder *enc;  // when compressing, shared by every file
    decoder *dec;  // when decompressing or testing, likewise
} options;

// GNU gzip 1.14's --help, its layout and wording, listing only the
// options tugz accepts: none refused, and of the accepted no-ops only
// -n, whose line holds for tugz; --rsyncable, whose doesn't, is hidden
// like GNU's -H and -m. GNU's footer for reporting bugs to its list is
// left out.
static s8 const usage_text = S8(
    "Usage: gzip [OPTION]... [FILE]...\n"
    "Compress or uncompress FILEs "
    "(by default, compress FILES in-place).\n"
    "\n"
    "  -c, --stdout      write on standard output, "
                         "keep original files unchanged\n"
    "  -d, --decompress  decompress\n"
    "  -f, --force       force overwrite of output file and compress links\n"
    "  -h, --help        give this help\n"
    "  -k, --keep        keep (don't delete) input files\n"
    "  -n, --no-name     do not save or restore "
                         "the original name and timestamp\n"
    "  -q, --quiet       suppress all warnings\n"
    "  -t, --test        test compressed file integrity\n"
    "  -V, --version     display version number\n"
    "  -1, --fast        compress faster\n"
    "  -9, --best        compress better\n"
    "\n"
    "With no FILE, or when FILE is -, read standard input.\n"
);

static i32 exit_combine(i32 a, i32 b)
{
    if (a==EXIT_ERR || b==EXIT_ERR) {
        return EXIT_ERR;
    }
    return a==EXIT_WARN || b==EXIT_WARN ? EXIT_WARN : EXIT_OK;
}

static s8 s8concat(arena *a, s8 x, s8 y)
{
    s8 r = {newbytes(a, x.len+y.len), x.len+y.len};
    bytecopy(r.s, x.s, x.len);
    bytecopy(r.s+x.len, y.s, y.len);
    return r;
}

static b32 ascii_iequals(s8 a, s8 b)
{
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        u8 x = a.s[i]>='A' && a.s[i]<='Z' ? a.s[i]+32 : a.s[i];
        u8 y = b.s[i]>='A' && b.s[i]<='Z' ? b.s[i]+32 : b.s[i];
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

// The suffix of a compressed file's name, as written in it (regardless
// of case), or a null string: GNU gzip's list. A suffix alone, or alone
// after a directory separator (/ or \), is not a name with that suffix.
static s8 known_suffix(s8 path)
{
    static s8 const suffixes[] = {
        S8(".gz"), S8(".z"), S8("-gz"), S8("-z"), S8("_z"),
        S8(".tgz"), S8(".taz"),
    };
    for (iz i = 0; i < countof(suffixes); i++) {
        iz len = suffixes[i].len;
        if (path.len <= len) {
            continue;
        }
        s8 tail = {path.s + path.len - len, len};
        u8 prev = tail.s[-1];
        if (prev!='/' && prev!='\\' && ascii_iequals(tail, suffixes[i])) {
            return tail;
        }
    }
    return (s8){0};
}

// Output name for decompression, or a null string if unrecognized: the
// suffix is dropped, except that .tgz and .taz become .tar.
static s8 strip_suffix(arena *a, s8 path)
{
    s8 suffix = known_suffix(path);
    if (!suffix.s) {
        return suffix;
    }
    s8 r = {path.s, path.len - suffix.len};
    if (ascii_iequals(suffix, S(".tgz")) || ascii_iequals(suffix, S(".taz"))) {
        r = s8concat(a, r, S(".tar"));
    }
    return r;
}

static void message(arena scratch, s8 name, s8 msg)
{
    writer *w = newwriter(&scratch, 2, 512);
    writer_s8(w, S("gzip: "));
    if (name.len) {
        writer_s8(w, name);
        writer_s8(w, S(": "));
    }
    writer_s8(w, msg);
    writer_byte(w, '\n');
    writer_flush(w);
}

static s8 status_message(i32 status)
{
    switch (status) {
    case GZ_TRAILING: return S("decompression OK, trailing garbage ignored");
    case GZ_ENOTGZ:   return S("not in gzip format");
    case GZ_ETRUNC:   return S("unexpected end of file");
    case GZ_EDATA:    return S("invalid compressed data--format violated");
    case GZ_ECRC:     return S("invalid compressed data--crc error");
    case GZ_ELEN:     return S("invalid compressed data--length error");
    }
    return S("unknown error");
}

static i32 warn(options *o, s8 name, s8 msg, arena scratch)
{
    if (!o->quiet) {
        message(scratch, name, msg);
    }
    return EXIT_WARN;
}

// A warning that runs on from the name, as some of GNU gzip's do.
static i32 warn_that(options *o, s8 name, s8 msg, arena scratch)
{
    s8 line = s8concat(&scratch, name, msg);
    return warn(o, (s8){0}, line, scratch);
}

static void writer_num(writer *w, u32 v, u32 base, i32 width)
{
    u8  buf[32];
    i32 len = 0;
    for (; v || len<width; v /= base) {
        buf[countof(buf) - ++len] = (u8)"0123456789abcdef"[v % base];
    }
    writer_write(w, buf+countof(buf)-len, len);
}

// Report a bad gzip header as GNU gzip does, from the bytes the decoder
// holds: the method byte, the flags, or the header check.
static void bad_header(decoder *z, s8 name, i32 status, arena scratch)
{
    writer *w = newwriter(&scratch, 2, 512);
    writer_s8(w, S("gzip: "));
    writer_s8(w, name);
    switch (status) {
    case GZ_EMETHOD:
        writer_s8(w, S(": unknown method "));
        writer_num(w, z->buf[2], 10, 1);
        writer_s8(w, S(" -- not supported"));
        break;
    case GZ_EFLAGS:
        if (z->buf[3] & 0x20) {
            writer_s8(w, S(" is encrypted -- not supported"));
        } else {
            writer_s8(w, S(" has flags 0x"));
            writer_num(w, z->buf[3], 16, 1);
            writer_s8(w, S(" -- not supported"));
        }
        break;
    case GZ_EHCRC:
        writer_s8(w, S(": header checksum 0x"));
        writer_num(w, (u32)(z->buf[0] | z->buf[1]<<8), 16, 4);
        writer_s8(w, S(" != computed checksum 0x"));
        writer_num(w, z->hcrc & 0xffff, 16, 4);
        break;
    }
    writer_byte(w, '\n');
    writer_flush(w);
}

// Why an os_* call just failed, as GNU gzip says in the system's words,
// or where those are unknown, a description.
static s8 reason(os *ctx, s8 otherwise)
{
    s8 why = os_error(ctx);
    return why.len ? why : otherwise;
}

// Report a transform's status, from reading in and writing out. As in
// GNU gzip, a read or write error names the file that failed, with the
// system's reason, and ends the run: as an error, or for a closed pipe,
// which the default SIGPIPE would have ended quietly, as a warning.
static i32 report(options *o, s8 in, s8 out, i32 status, arena scratch)
{
    os *ctx = scratch.ctx;
    switch (status) {
    case GZ_OK:
        return EXIT_OK;
    case GZ_TRAILING:
        return warn(o, in, status_message(status), scratch);
    case GZ_EREAD:
        o->stop = 1;
        message(scratch, in, reason(ctx, S("read error")));
        return EXIT_ERR;
    case GZ_EWRITE:
        o->stop = 1;
        if (os_pipeclosed(ctx)) {
            return warn(o, out, reason(ctx, S("broken pipe")), scratch);
        }
        message(scratch, out, reason(ctx, S("write error")));
        return EXIT_ERR;
    case GZ_EMETHOD:
    case GZ_EFLAGS:
    case GZ_EHCRC:
        bad_header(o->dec, in, status, scratch);
        return EXIT_ERR;
    }
    message(scratch, in, status_message(status));
    return EXIT_ERR;
}

// Transform a file to standard output, or test it. As in GNU gzip,
// forced decompression to standard output copies data that is not gzip
// through unchanged (zcat -f), and a forced test passes it.
static i32 transform(options *o, i32 in, i32 out, arena scratch)
{
    b32 copy = o->force;
    if (o->test) {
        return stream_decompress(o->dec, in, -1, copy, scratch);
    } else if (o->decompress) {
        return stream_decompress(o->dec, in, out, copy, scratch);
    }
    return gzip_compress(o->enc, in, out, o->level, scratch);
}

// Like GNU gzip, when using standard input, refuse to write compressed
// data to a terminal or to read it from one, unless forced. Named files
// with -c are not checked. Returns -1 to continue, or an exit status.
static i32 check_terminal(options *o, arena scratch)
{
    os *ctx = scratch.ctx;
    b32 compress = !o->decompress && !o->test;
    if (o->force) {
        return -1;
    } else if (compress && os_isatty(ctx, 1)) {
        message(scratch, (s8){0}, S(
            "compressed data not written to a terminal. "
            "Use -f to force compression."
        ));
        return EXIT_ERR;
    } else if (!compress && os_isatty(ctx, 0)) {
        message(scratch, (s8){0}, S(
            "compressed data not read from a terminal. "
            "Use -f to force decompression."
        ));
        return EXIT_ERR;
    }
    return -1;
}

// Open an input file. As in GNU gzip, decompressing or testing a missing
// name without a suffix tries it with suffixes in turn (zcat foo reads
// foo.gz). The path becomes the name found, or else the first one tried.
static i32 find_input(options *o, s8 *path, i32 mode, arena *scratch)
{
    os *ctx = scratch->ctx;
    i32 in = os_open(ctx, *path, mode, *scratch);
    if (in!=OS_ERR || !(o->decompress || o->test) || !os_missing(ctx) ||
        known_suffix(*path).s) {
        return in;
    }

    static s8 const tries[] = {S8(".gz"), S8(".z"), S8("-z"), S8(".Z")};
    s8 first = s8concat(scratch, *path, tries[0]);
    for (iz i = 0; i < countof(tries); i++) {
        s8 name = i ? s8concat(scratch, *path, tries[i]) : first;
        in = os_open(ctx, name, mode, *scratch);
        if (in!=OS_ERR || !os_missing(ctx)) {
            *path = name;
            return in;
        }
    }
    *path = first;
    return in;
}

static i32 process_file(options *o, s8 path, arena scratch)
{
    os *ctx = scratch.ctx;

    if (s8equals(path, S("-"))) {
        i32 r = check_terminal(o, scratch);
        if (r >= 0) {
            return r;
        }
        i32 status = transform(o, 0, 1, scratch);
        return report(o, S("stdin"), S("stdout"), status, scratch);
    }

    // The input of an in-place operation is deleted afterward, so only
    // plain files qualify. Forcing permits links, which are followed, and
    // the sticky bit, but as in GNU gzip, never set-ID bits.
    b32 in_place = !o->to_stdout && !o->test;
    i32 mode = OS_READ;
    if (in_place) {
        mode |= OS_REGULAR | OS_NOSETID;
        mode |= o->force ? 0 : OS_NOFOLLOW|OS_ONELINK|OS_NOSTICKY;
    }
    i32 in = find_input(o, &path, mode, &scratch);
    switch (in) {
    case OS_EISDIR:
        return warn_that(o, path, S(" is a directory -- ignored"), scratch);
    case OS_ESYMLINK:
        // An error, not a warning, as GNU gzip's refusal (ELOOP) is
        message(scratch, path, S("is a symbolic link -- ignored"));
        return EXIT_ERR;
    case OS_ENOTREG:
        return warn_that(o, path, S(" is not a directory or a regular file "
                                    "- ignored"), scratch);
    case OS_ESETUID:
        return warn_that(o, path, S(" is set-user-ID on execution - ignored"),
                         scratch);
    case OS_ESETGID:
        return warn_that(o, path, S(" is set-group-ID on execution - ignored"),
                         scratch);
    case OS_ESTICKY:
        return warn_that(o, path, S(" has the sticky bit set - file ignored"),
                         scratch);
    case OS_ELINKS:  // which GNU gzip counts
        return warn_that(o, path, S(" has other links -- file ignored"),
                         scratch);
    }
    if (in < 0) {
        message(scratch, path, reason(ctx, S("cannot open for reading")));
        return EXIT_ERR;
    }

    if (!in_place) {
        i32 status = transform(o, in, 1, scratch);
        i32 code = report(o, path, S("stdout"), status, scratch);
        os_close(ctx, in);
        return code;
    }

    // Like GNU gzip, check the name once the input qualifies. Leaving a
    // file named as compressed is no failure, and -f compresses it anyway.
    s8 outpath = {0};
    if (o->decompress) {
        outpath = strip_suffix(&scratch, path);
        if (!outpath.s) {
            // Under -q, GNU gzip leaves even the status at 0
            os_close(ctx, in);
            return o->quiet ? EXIT_OK
                            : warn(o, path, S("unknown suffix -- ignored"),
                                   scratch);
        }
    } else {
        s8 suffix = known_suffix(path);
        if (suffix.s && !o->force) {
            os_close(ctx, in);
            if (!o->quiet) {
                s8 msg = s8concat(&scratch, path, S(" already has "));
                msg = s8concat(&scratch, msg, suffix);
                msg = s8concat(&scratch, msg, S(" suffix -- unchanged"));
                message(scratch, (s8){0}, msg);
            }
            return EXIT_OK;
        }
        outpath = s8concat(&scratch, path, S(".gz"));
    }

    // Take the input's metadata now, as GNU gzip does, before reading
    // changes its access time
    osmeta *meta = os_getmeta(ctx, in, &scratch);
    if (!meta) {
        message(scratch, path, reason(ctx, S("cannot read metadata")));
        os_close(ctx, in);
        return EXIT_ERR;
    }

    // As in GNU gzip, read the header before creating the output, so that
    // input that is not gzip is the error, and never replaces a file
    reader *r = 0;
    if (o->decompress) {
        r = stream_reader(&scratch, in);
        if (!stream_header(o->dec, r)) {
            i32 status = stream_decode(o->dec, r, -1, 0);
            i32 code = report(o, path, outpath, status, scratch);
            os_close(ctx, in);
            return code;
        }
    }

    i32 out = os_open(ctx, outpath, o->force ? OS_FORCE : OS_CREATE, scratch);
    if (out == OS_EEXIST) {
        // A warning in GNU gzip's words, which it gives even under -q
        os_close(ctx, in);
        s8 msg = S(" already exists;\tnot overwritten");
        msg = s8concat(&scratch, outpath, msg);
        message(scratch, (s8){0}, msg);
        return EXIT_WARN;
    } else if (out < 0) {
        message(scratch, outpath, reason(ctx, S("cannot open for writing")));
        os_close(ctx, in);
        return EXIT_ERR;
    }

    // The output is discarded on close unless explicitly kept, so that
    // failures and interruptions never leave a partial file behind. In
    // place, data that is not gzip is an error even when forced.
    i32 status = r ? stream_decode(o->dec, r, out, 0)
                   : gzip_compress(o->enc, in, out, o->level, scratch);
    i32 code = report(o, path, outpath, status, scratch);
    if (status!=GZ_OK && status!=GZ_TRAILING) {
        os_close(ctx, out);
        os_close(ctx, in);
        return code;
    }

    // Failing to give the output the input's permissions or times loses
    // no data, so as in GNU gzip it is only a warning
    if (!os_setmeta(ctx, out, meta)) {
        s8 why = reason(ctx, S("cannot set metadata"));
        code = exit_combine(code, warn(o, outpath, why, scratch));
    }
    // A failed close may mean lost data, and an output that cannot be
    // kept is discarded when closed, so the input must survive either
    b32 kept = os_keep(ctx, out);
    if (!kept || !os_close(ctx, out)) {
        code = report(o, path, outpath, GZ_EWRITE, scratch);
        if (kept) {
            os_remove(ctx, outpath, scratch);
        } else {
            os_close(ctx, out);
        }
        os_close(ctx, in);
        return code;
    }
    os_close(ctx, in);

    // Failing to remove the input loses nothing, so as in GNU gzip it is
    // only a warning
    if (!o->keep && !os_remove(ctx, path, scratch)) {
        s8 why = reason(ctx, S("cannot remove input file"));
        code = exit_combine(code, warn(o, path, why, scratch));
    }
    return code;
}

static void print(arena scratch, i32 fd, s8 s)
{
    writer *w = newwriter(&scratch, fd, 512);
    writer_s8(w, s);
    writer_flush(w);
}

// Report a usage error as GNU gzip does: a message (if any), then a
// pointer to --help.
static i32 usage_error(arena scratch, s8 msg)
{
    writer *w = newwriter(&scratch, 2, 512);
    if (msg.len) {
        writer_s8(w, S("gzip: "));
        writer_s8(w, msg);
        writer_byte(w, '\n');
    }
    writer_s8(w, S("Try `gzip --help' for more information.\n"));
    writer_flush(w);
    return EXIT_ERR;
}

// Options GNU gzip has without a letter
enum {
    OPT_PRESUME = 0x100,
    OPT_SYNCHRONOUS,
    OPT_RSYNCABLE,
};

// GNU gzip's long options, in its order, which ambiguity messages
// follow, whether or not tugz supports them, so that a prefix means
// what it means there. "-presume-input-tty" is GNU's, hidden (---pre).
static struct {
    s8  name;
    i32 which;  // letter, or OPT_*
    b32 arg;    // takes an argument
} const longopts[] = {
    {S8("ascii"),             'a',             0},
    {S8("to-stdout"),         'c',             0},
    {S8("stdout"),            'c',             0},
    {S8("decompress"),        'd',             0},
    {S8("uncompress"),        'd',             0},
    {S8("force"),             'f',             0},
    {S8("help"),              'h',             0},
    {S8("keep"),              'k',             0},
    {S8("list"),              'l',             0},
    {S8("license"),           'L',             0},
    {S8("no-name"),           'n',             0},
    {S8("name"),              'N',             0},
    {S8("-presume-input-tty"), OPT_PRESUME,    0},
    {S8("quiet"),             'q',             0},
    {S8("silent"),            'q',             0},
    {S8("synchronous"),       OPT_SYNCHRONOUS, 0},
    {S8("recursive"),         'r',             0},
    {S8("suffix"),            'S',             1},
    {S8("test"),              't',             0},
    {S8("verbose"),           'v',             0},
    {S8("version"),           'V',             0},
    {S8("fast"),              '1',             0},
    {S8("best"),              '9',             0},
    {S8("lzw"),               'Z',             0},
    {S8("bits"),              'b',             1},
    {S8("rsyncable"),         OPT_RSYNCABLE,   0},
};

// Map a long option (--name, or --name=value) to its letter or code as
// GNU's getopt_long does: an exact name, or else a prefix of names that
// all mean the same option. Sets *name to the option's full name, for
// messages. Returns 0 after reporting an error.
static i32 parse_long(s8 arg, s8 *name, arena scratch)
{
    s8 key = {arg.s+2, 0};
    while (key.len<arg.len-2 && key.s[key.len]!='=') {
        key.len++;
    }
    b32 hasarg = key.len < arg.len-2;

    i32 found = -1;
    b32 ambiguous = 0;
    for (i32 i = 0; i < countof(longopts); i++) {
        s8 full = longopts[i].name;
        s8 head = {full.s, MIN(full.len, key.len)};
        if (full.len<key.len || !s8equals(head, key)) {
            continue;
        } else if (full.len == key.len) {
            found = i;  // an exact match wins
            ambiguous = 0;
            break;
        } else if (found < 0) {
            found = i;
        } else if (longopts[i].which != longopts[found].which) {
            ambiguous = 1;
        }
    }

    if (found < 0) {
        s8 m = s8concat(&scratch, S("unrecognized option '"), arg);
        usage_error(scratch, s8concat(&scratch, m, S("'")));
        return 0;
    } else if (ambiguous) {
        s8 m = s8concat(&scratch, S("option '"), arg);
        m = s8concat(&scratch, m, S("' is ambiguous; possibilities:"));
        i32 which = longopts[found].which;
        for (i32 i = found; i < countof(longopts); i++) {
            s8 full = longopts[i].name;
            s8 head = {full.s, MIN(full.len, key.len)};
            if (full.len>key.len && s8equals(head, key) &&
                (i==found || longopts[i].which!=which)) {
                m = s8concat(&scratch, m, S(" '--"));
                m = s8concat(&scratch, m, full);
                m = s8concat(&scratch, m, S("'"));
            }
        }
        usage_error(scratch, m);
        return 0;
    }

    *name = longopts[found].name;
    if (hasarg && !longopts[found].arg) {
        s8 m = s8concat(&scratch, S("option '--"), *name);
        m = s8concat(&scratch, m, S("' doesn't allow an argument"));
        usage_error(scratch, m);
        return 0;
    }
    return longopts[found].which;
}

// Apply an option, by letter or OPT_* code, given by its letter or else
// by its long name (without "--"). Returns -1 to continue, or exit status.
static i32 apply_option(options *o, i32 c, s8 longname, arena scratch)
{
    u8 letter[2] = {'-', (u8)c};
    s8 name = (s8){letter, 2};
    if (longname.len) {
        name = s8concat(&scratch, S("--"), longname);
    }

    switch (c) {
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
              o->level      = c - '0'; return -1;
    case 'c': o->to_stdout  = 1;       return -1;
    case 'd': o->decompress = 1;       return -1;
    case 'f': o->force      = 1;       return -1;
    case 'k': o->keep       = 1;       return -1;
    case 'm':                          return -1;  // no time: as always
    case 'n':                          return -1;  // nothing to save
    case OPT_RSYNCABLE:                return -1;  // a hint, not a format
    case 'q': o->quiet      = 1;       return -1;
    case 't': o->test       = 1;       return -1;
    case 'h': case 'H':
        print(scratch, 1, usage_text);
        return EXIT_OK;
    case 'V':
        print(scratch, 1, S("gzip (tugz) " TUGZ_VERSION "\n"));
        return EXIT_OK;
    case '?':
        return usage_error(scratch, (s8){0});  // GNU's, without a message
    case 'a': case 'b': case 'l': case 'r': case 'v':
    case 'L': case 'M': case 'N': case 'S': case 'Z':
    case OPT_PRESUME: case OPT_SYNCHRONOUS:
        // GNU gzip's wording for its own -Z
        name = s8concat(&scratch, name, S(" not supported in this version"));
        return usage_error(scratch, name);
    }
    s8 m = s8concat(&scratch, S("invalid option -- '"), (s8){letter+1, 1});
    return usage_error(scratch, s8concat(&scratch, m, S("'")));
}

static b32 ascii_iprefix(s8 s, s8 prefix)
{
    s8 head = {s.s, MIN(s.len, prefix.len)};
    return ascii_iequals(head, prefix);
}

// Like GNU gzip, the program name selects a default mode: gunzip (or
// any un* name) decompresses, and zcat or gzcat decompress to standard
// output.
static void apply_name(options *o, s8 name)
{
    if (ascii_iprefix(name, S("un")) || ascii_iprefix(name, S("gun"))) {
        o->decompress = 1;
    } else if (ascii_iequals(name, S("zcat")) ||
               ascii_iequals(name, S("gzcat"))) {
        o->decompress = 1;
        o->to_stdout = 1;
    }
}

static i32 gzip_main(config *conf)
{
    arena *perm = &conf->perm;
    options o = {0};
    o.level = 6;
    apply_name(&o, conf->name);

    s8 *files = new(perm, conf->nargs+1, s8);
    i32 nfiles = 0;
    b32 endopts = 0;
    for (i32 i = 0; i < conf->nargs; i++) {
        s8 arg = conf->args[i];
        if (endopts || arg.len<2 || arg.s[0]!='-') {
            files[nfiles++] = arg;
        } else if (s8equals(arg, S("--"))) {
            endopts = 1;
        } else if (arg.s[1] == '-') {
            s8 name = {0};
            i32 c = parse_long(arg, &name, *perm);
            if (!c) {
                return EXIT_ERR;
            }
            i32 r = apply_option(&o, c, name, *perm);
            if (r >= 0) {
                return r;
            }
        } else {
            for (iz j = 1; j < arg.len; j++) {
                i32 r = apply_option(&o, arg.s[j], (s8){0}, *perm);
                if (r >= 0) {
                    return r;
                }
            }
        }
    }

    if (!nfiles) {
        files[nfiles++] = S("-");
    }
    if (o.decompress || o.test) {
        o.dec = stream_decoder(perm, FMT_GZIP);
    } else {
        o.enc = gzip_encoder(perm, o.level);
    }

    i32 code = EXIT_OK;
    for (i32 i = 0; i<nfiles && !o.stop; i++) {
        code = exit_combine(code, process_file(&o, files[i], *perm));
    }
    return code;
}

// tugz wildcards: matching names against patterns as Info-ZIP does
//
// For zip's patterns (-x, -i, and paths matched against entries) on
// every platform, and on Windows for wildcard arguments, which zip and
// gzip expand on disk (src/dir.c), as Windows shells do not.

enum {
    ZIP_SETS   = 1 << 0,  // [sets] and backslash escapes, as on Unix
    ZIP_FOLD   = 1 << 1,  // ASCII case-insensitive, for Windows file names
    ZIP_NOWILD = 1 << 2,  // only ? is a wildcard, as with Info-ZIP's -nw
    ZIP_DOS    = 1 << 3,  // a name without a period ends in one (Windows)
    ZIP_UTF8   = 1 << 4,  // ? is a UTF-8 character, not a byte (Windows)
};

static u8 zip_fold(u8 c, i32 flags)
{
    return (flags & ZIP_FOLD) && c>='A' && c<='Z' ? c+('a'-'A') : c;
}

static b32 zip_has(s8 s, u8 c)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i] == c) {
            return 1;
        }
    }
    return 0;
}

// Byte i of a name, past whose end lies ZIP_DOS's implicit period.
static u8 zip_at(s8 s, iz i)
{
    return i<s.len ? s.s[i] : '.';
}

// Length of the character at byte i of a name: of a UTF-8 sequence (or
// WTF-8, whose lone surrogates are characters on Windows), else 1.
static iz zip_charlen(s8 s, iz i)
{
    u8 c = zip_at(s, i);
    iz n = c>=0xc2 && c<0xe0 ? 2 : c>=0xe0 && c<0xf0 ? 3 :
           c>=0xf0 && c<0xf5 ? 4 : 1;
    if (n > s.len-i) {
        return 1;
    }
    for (iz k = 1; k < n; k++) {
        if ((s.s[i+k] & 0xc0) != 0x80) {
            return 1;
        }
    }
    return n;
}

// Whether c is in the set between pat[beg] and pat[end], the bytes
// within the brackets, as Info-ZIP reads one: a backslash escapes, a
// leading - is literal, any other - makes the byte before it the start
// of a range ending at the byte after it, and a byte followed by - is
// only a range start (so [a-] and the a in [a-b-c] match nothing). An
// end of 0xff wraps Info-ZIP's range loop around, matching any byte.
static b32 zip_inset(s8 pat, iz beg, iz end, u8 c, i32 flags)
{
    u8 other = c;
    if (flags & ZIP_FOLD) {
        other = c>='a' && c<='z' ? c-('a'-'A') : zip_fold(c, flags);
    }
    i32 lo  = -1;  // pending range start
    b32 esc = beg<end && pat.s[beg]=='-';
    for (iz k = beg; k < end; k++) {
        u8 b = pat.s[k];
        if (!esc && b=='\\') {
            esc = 1;
        } else if (!esc && b=='-') {
            lo = pat.s[k-1];
        } else {
            if (pat.s[k+1] != '-') {
                u8 l = lo<0 ? b : (u8)lo;
                if (b==0xff || (c>=l && c<=b) || (other>=l && other<=b)) {
                    return 1;
                }
            }
            lo  = -1;
            esc = 0;
        }
    }
    return 0;
}

// Whether a pattern has no wildcards from pat[p] on, by Info-ZIP's
// isshexp, which skips escaped bytes and counts [ even without sets.
static b32 zip_literal(s8 pat, iz p, i32 flags)
{
    for (; p < pat.len; p++) {
        u8 c = pat.s[p];
        if (c=='\\' && (flags & ZIP_SETS) && p+1<pat.len) {
            p++;
        } else if (c=='*' || c=='?' || c=='[') {
            return 0;
        }
    }
    return 1;
}

// Info-ZIP's wildcard match (recmatch) over whole names: * matches any
// run of bytes, including slashes, and ? any single byte. With ZIP_SETS,
// [abc], [a-z], and [!x] or [^x] match a set, and backslash escapes the
// next byte; otherwise brackets and backslashes are literal. ZIP_NOWILD
// keeps only ?, and ZIP_DOS matches a name without a period as if it
// ended in one when the pattern has one, so that *.* matches every name.
// With ZIP_UTF8, ? matches a UTF-8 character, and * a run of them, as
// Info-ZIP's Windows port matches characters of its code page (or wide
// ones), while on Unix it matches bytes. Sets, unused there, match bytes.
//
// Info-ZIP's quirks are kept: an unclosed set or a trailing backslash
// matches nothing; a trailing ** needs at least one more byte; and once
// a * is followed by no more wildcards, the rest of the pattern is
// compared with the end of the name byte for byte, escapes included.
static b32 zip_match(s8 pat, s8 s, i32 flags)
{
    b32 wild = !(flags & ZIP_NOWILD);
    b32 sets = wild && (flags & ZIP_SETS);
    b32 utf8 = flags & ZIP_UTF8;
    b32 dot  = (flags & ZIP_DOS) && zip_has(pat, '.') && !zip_has(s, '.');
    iz  n    = s.len + dot;
    iz p = 0, i = 0;
    iz star = -1, mark = 0;
    while (i < n) {
        if (p < pat.len) {
            u8 c  = pat.s[p];
            u8 sc = zip_at(s, i);
            if (c=='*' && wild) {
                if (zip_literal(pat, ++p, flags)) {
                    iz tail = pat.len - p;
                    if (tail > n-i) {
                        return 0;
                    }
                    for (iz k = 0; k < tail; k++) {
                        u8 a = zip_fold(pat.s[p+k], flags);
                        if (a != zip_fold(zip_at(s, n-tail+k), flags)) {
                            return 0;
                        }
                    }
                    return 1;
                }
                star = p;
                mark = i;
                continue;
            } else if (c == '?') {
                p++;
                i += utf8 ? zip_charlen(s, i) : 1;
                continue;
            } else if (c=='[' && sets) {
                iz  q   = p + 1;
                b32 neg = q<pat.len && (pat.s[q]=='!' || pat.s[q]=='^');
                q += neg;
                iz end = q;  // the first unescaped ]
                for (b32 e = 0; end<pat.len && (e || pat.s[end]!=']'); end++) {
                    e = !e && pat.s[end]=='\\';
                }
                if (end == pat.len) {
                    return 0;  // unclosed: nothing matches
                }
                if (zip_inset(pat, q, end, sc, flags) != neg) {
                    p = end + 1;
                    i++;
                    continue;
                }
            } else {
                if (c=='\\' && sets) {
                    if (++p == pat.len) {
                        return 0;  // nothing to escape: nothing matches
                    }
                    c = pat.s[p];
                }
                if (zip_fold(c, flags) == zip_fold(sc, flags)) {
                    p++;
                    i++;
                    continue;
                }
            }
        }
        if (star < 0) {
            return 0;
        }
        p    = star;
        mark += utf8 ? zip_charlen(s, mark) : 1;
        i    = mark;
    }
    iz stars = 0;
    for (; wild && p<pat.len && pat.s[p]=='*'; p++, stars++) {}
    return p==pat.len && stars<2;
}

static b32 zip_haswild(s8 s, i32 flags)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i]=='*' || s.s[i]=='?' || ((flags & ZIP_SETS) && s.s[i]=='[')) {
            return 1;
        }
    }
    return 0;
}

// tugz directories: listings, and wildcards expanded in paths
//
// zip lists directories to recurse into them. On Windows, whose shells
// leave wildcards in arguments to programs, zip and gzip both expand
// them here, as Info-ZIP's Windows port does, and unzip expands
// wildcard archive names on every platform, as UnZip does, matching
// names by src/wild.c. The platform layer provides os_listdir and
// os_upcase.

enum { FT_NONE, FT_FILE, FT_DIR, FT_LINK, FT_OTHER };

typedef struct {
    i32 type;
    i64 size;
    i64 mtime;  // Unix seconds
    i64 atime;
    u32 mode;   // POSIX st_mode (zero on Windows)
    u32 attr;   // DOS attributes (Windows)
    u32 uid;
    u32 gid;
    u64 dev;     // device and file ID identify a file, unless the ID
    u64 ino[2];  // is zero (unknown); 128 bits on Windows
} os_info;

// A directory entry, with what the listing itself tells of the file:
// info as os_stat would report it following links, but with an unknown
// identity, or type FT_NONE where the listing does not tell (always for
// links), in which case os_stat must examine it.
typedef struct {
    s8      name;
    os_info info;
} os_dirent;

typedef struct {
    os_dirent *data;
    iz         len;
    iz         cap;
} os_dirents;

// Entries within a directory, excluding . and .., in any order. Unless
// all, hidden and system entries (Windows) are left out, judged by the
// entry itself rather than a link's target. Returns null on error. The
// listing and any temporaries come from one arena.
static os_dirent *os_listdir(os *, s8 path, b32 all, iz *count, arena *);
// A name in upper case as the file system compares names ignoring case
// (Windows), beyond ASCII too, or the name itself. It and any temporaries
// come from one arena.
static s8   os_upcase(os *, s8 name, arena *);

// Append a zeroed slot to a dynamic array, returning a pointer to it.
#define push(a, s) \
    ((s)->len==(s)->cap ? grow(a, (void **)&(s)->data, &(s)->cap, \
                               sizeof(*(s)->data)) : (void)0, \
     (s)->data + (s)->len++)

// Whether data of len bytes ends where the arena's next allocation
// begins, so that it can grow in place.
static b32 at_tip(arena *a, void *data, iz len)
{
    return !a->down && data && (byte *)data+len==a->beg;
}

// Double an array's capacity: in place if it is the last allocation in
// an arena that grows up (perm), else by moving it, which leaves the
// old copy as garbage until the arena is freed (in perm, never). An
// empty one, which doubling would leave empty, moves to a fresh 16.
[[maybe_unused]] static void grow(arena *a, void **data, iz *cap, iz size)
{
    if (*cap && at_tip(a, *data, *cap*size)) {
        alloc(a, *cap, size, 1, 1);
        *cap *= 2;
        return;
    }
    iz   n = *cap ? 2 * *cap : 16;
    void *r = alloc(a, n, size, 16, 1);
    bytecopy(r, *data, *cap*size);
    *data = r;
    *cap  = n;
}

static i32 zcompare(s8 a, s8 b)
{
    iz n = MIN(a.len, b.len);
    i32 r = n ? __builtin_memcmp(a.s, b.s, (uz)n) : 0;
    return r ? r : a.len<b.len ? -1 : a.len>b.len;
}

// A directory listing sorted by name bytes, as pointers to its entries,
// which are large to move: a stable bottom-up merge sort.
static os_dirent **zsort(os_dirent *list, iz n, arena *a)
{
    os_dirent **v = new(a, n, os_dirent *);
    for (iz i = 0; i < n; i++) {
        v[i] = list + i;
    }
    arena       t   = *a;
    os_dirent **tmp = new(&t, n, os_dirent *);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid && j<hi) {
                b32 lt = zcompare(v[j]->name, v[i]->name) < 0;
                tmp[k++] = lt ? v[j++] : v[i++];
            }
            while (i < mid) {
                tmp[k++] = v[i++];
            }
            while (j < hi) {
                tmp[k++] = v[j++];
            }
        }
        bytecopy(v, tmp, n*(iz)sizeof(*v));
    }
    return v;
}

// The path with its bytes from beg to end replaced by name.
static s8 replace_part(arena *a, s8 path, iz beg, iz end, s8 name)
{
    s8 r  = {0};
    r.len = path.len - (end-beg) + name.len;
    r.s   = newstr(a, r.len);
    bytecopy(r.s, path.s, beg);
    bytecopy(r.s+beg, name.s, name.len);
    bytecopy(r.s+beg+name.len, path.s+end, path.len-end);
    return r;
}

static b32 nonascii(s8 s)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i] >= 0x80) {
            return 1;
        }
    }
    return 0;
}

// A match for expand_wild: its path, and its directory entry, if that
// describes it, else null. Returns whether the match counts.
typedef b32 wild_found(void *data, s8 path, os_dirent *entry, arena scratch);

// Flags for expand_wild, beside zip_match's
enum {
    // Names starting with '.' match only patterns that do, as on Unix,
    // where UnZip's '*' and '?' do not match a leading dot
    WILD_NODOTS   = 1 << 16,
    // The ZIP_* flags of Info-ZIP's Windows port, as zip and gzip expand
    // wildcards there: ignoring case, by characters, with DOS rules
    WILD_WINDOWS  = ZIP_FOLD | ZIP_DOS | ZIP_UTF8,
};

// Expand wildcards in a path's components against the file system,
// matching names by zip_match with flags (ZIP_*: with ZIP_SETS, '[' is a
// wildcard too; ZIP_FOLD also ignores case beyond ASCII as the file
// system does; ZIP_DOS takes paths by Windows rules), and WILD_NODOTS.
// Hidden and system entries (Windows) are left out unless all. Each
// match goes to found, in name order. Returns the number of matches that
// found counted.
static iz expand_wild(os *ctx, s8 path, b32 all, i32 flags,
                      wild_found *found, void *data, arena scratch)
{
    // Find the first component with a wildcard. With Windows rules
    // (ZIP_DOS), '\' separates them too, and a drive ends one: "C:*.c"
    // lists the drive's current directory, "C:". A device prefix, "\\?\"
    // or "\\.\" with either slash, is no component: its '?' is no
    // wildcard.
    b32 win   = flags & ZIP_DOS;
    b32 colon = win && path.len>=2 && path.s[1]==':';
    u8  drive = colon ? (u8)(path.s[0] | 0x20) : 0;
    b32 dev   = win && path.len>=4 &&
                (path.s[0]=='/' || path.s[0]=='\\') &&
                (path.s[1]=='/' || path.s[1]=='\\') &&
                (path.s[2]=='?' || path.s[2]=='.') &&
                (path.s[3]=='/' || path.s[3]=='\\');
    iz  beg   = 0;
    iz  end   = 0;
    iz  first = dev ? 4 : drive>='a' && drive<='z' ? 2 : 0;
    for (iz i = first;; i = end + 1) {
        beg = i;
        for (end = i; end<path.len && path.s[end]!='/' &&
                      (!win || path.s[end]!='\\'); end++) {}
        s8 comp = {path.s+beg, end-beg};
        if (zip_haswild(comp, flags)) {
            break;
        } else if (end == path.len) {
            return 0;
        }
    }

    s8 dir  = beg ? (s8){path.s, beg} : S(".");
    s8 pat  = {path.s+beg, end-beg};
    s8 rest = {path.s+end, path.len-end};
    iz         n    = 0;
    os_dirent *list = os_listdir(ctx, dir, all, &n, &scratch);
    if (!list) {
        return 0;
    }
    os_dirent **kids = zsort(list, n, &scratch);

    // Beyond ASCII, which ZIP_FOLD folds, case is ignored by comparing in
    // upper case, as the file system and Info-ZIP's port (by towupper)
    // compare names, where the pattern or the name needs it
    b32 fold  = flags & ZIP_FOLD;
    b32 wide  = fold && nonascii(pat);
    b32 dots  = (flags & WILD_NODOTS) && pat.len && pat.s[0]=='.';
    s8  upat  = wide ? os_upcase(ctx, pat, &scratch) : pat;
    iz  count = 0;
    for (iz i = 0; i < n; i++) {
        arena tmp = scratch;
        s8    kid = kids[i]->name;
        b32   up  = wide || (fold && nonascii(kid));
        if ((flags & WILD_NODOTS) && !dots && kid.len && kid.s[0]=='.') {
            continue;
        } else if (!zip_match(up ? upat : pat,
                              up ? os_upcase(ctx, kid, &tmp) : kid, flags)) {
            continue;
        }
        arena iter = scratch;  // forgets each match's strings
        s8    cand = replace_part(&iter, path, beg, end, kid);
        if (zip_haswild(rest, flags)) {
            count += expand_wild(ctx, cand, all, flags, found, data, iter);
            continue;
        }
        // The listing describes the match only if nothing follows it, and
        // a bare name, like an argument, may instead name a device (NUL)
        os_dirent *entry = beg && !rest.len ? kids[i] : 0;
        if (found(data, cand, entry, iter)) {
            count++;
        }
    }
    return count;
}

// Shared CRT-free Win32 platform code: the os_* file interface of
// src/io.c, directory listings for src/dir.c, path conversion, the
// consoles, error wording, and the command line. Included by each
// Windows program after the sources it needs.

typedef unsigned short c16;
typedef uptr           iptr;

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 size_hi, size_lo;
    u32 reserved[2];
    c16 name[260];
    c16 altname[14];
} find_data;

typedef void (__stdcall *w32proc)(void);

// Only functions that Windows XP has are imported: see winapi for the
// newer ones, looked up at run time.
#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(iptr)   CreateFileW(c16 *, u32, u32, uptr, u32, u32, iptr);
W32(b32)    DeleteFileW(c16 *);
[[noreturn]] W32(void) ExitProcess(u32);
W32(b32)    FindClose(iptr);
W32(iptr)   FindFirstFileExW(c16 *, i32, find_data *, i32, uptr, u32);
W32(b32)    FindNextFileW(iptr, find_data *);
W32(c16 *)  GetCommandLineW(void);
W32(u32)    GetCurrentDirectoryW(u32, c16 *);
W32(u32)    GetFullPathNameW(c16 *, u32, c16 *, c16 **);
W32(u32)    GetFileAttributesW(c16 *);
W32(b32)    GetConsoleMode(iptr, u32 *);
W32(b32)    GetFileInformationByHandle(iptr, void *);
W32(u32)    GetFileType(iptr);
W32(u32)    GetLastError(void);
W32(iptr)   GetModuleHandleW(c16 const *);
W32(b32)    GetNamedPipeHandleStateW(iptr, u32 *, u32 *, u32 *, u32 *, c16 *,
                                     u32);
W32(w32proc) GetProcAddress(iptr, char const *);
W32(iptr)   GetStdHandle(u32);
W32(u32)    GetTickCount(void);
W32(i32)    LCMapStringW(u32, u32, c16 const *, i32, c16 *, i32);
W32(b32)    ReadConsoleW(iptr, c16 *, u32, u32 *, uptr);
W32(b32)    ReadFile(iptr, void *, u32, u32 *, uptr);
W32(b32)    SetConsoleCtrlHandler(b32 (__stdcall *)(u32), b32);
W32(b32)    SetNamedPipeHandleState(iptr, u32 *, u32 *, u32 *);
W32(void)   SetLastError(u32);
W32(void)   Sleep(u32);
W32(void *) VirtualAlloc(uptr, iz, u32, u32);
W32(b32)    WriteConsoleW(iptr, c16 const *, u32, u32 *, uptr);
W32(b32)    WriteFile(iptr, void const *, u32, u32 *, uptr);

#define DELETE                     0x00010000u
#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define FILE_READ_ATTRIBUTES       0x80u
#define FILE_WRITE_ATTRIBUTES      0x100u
#define FILE_SHARE_ALL             7u
#define CREATE_NEW                 1u
#define CREATE_ALWAYS              2u
#define OPEN_EXISTING              3u
#define FILE_ATTRIBUTE_READONLY    0x01u
#define FILE_ATTRIBUTE_HIDDEN      0x02u
#define FILE_ATTRIBUTE_SYSTEM      0x04u
#define FILE_ATTRIBUTE_NORMAL      0x80u
#define FILE_ATTRIBUTE_DIRECTORY   0x10u
#define FILE_ATTRIBUTE_REPARSE     0x400u
#define FILE_FLAG_OPEN_REPARSE     0x00200000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_TYPE_UNKNOWN          0u
#define FILE_TYPE_DISK             1u
#define FIND_FIRST_EX_LARGE_FETCH  2u
#define PIPE_NOWAIT                1u
#define PIPE_READMODE_MESSAGE      2u
#define IO_REPARSE_TAG_SURROGATE   0x20000000u
#define INVALID_FILE_ATTRIBUTES    0xffffffffu
#define INVALID_HANDLE_VALUE       ((iptr)-1)
#define LOCALE_INVARIANT           0x7fu
#define LCMAP_UPPERCASE            0x200u
#define ERROR_INVALID_FUNCTION     1u
#define ERROR_FILE_NOT_FOUND       2u
#define ERROR_PATH_NOT_FOUND       3u
#define ERROR_TOO_MANY_OPEN_FILES  4u
#define ERROR_ACCESS_DENIED        5u
#define ERROR_NO_MORE_FILES        18u
#define ERROR_SHARING_VIOLATION    32u
#define ERROR_NOT_SUPPORTED        50u
#define ERROR_FILE_EXISTS          80u
#define ERROR_INVALID_PARAMETER    87u
#define ERROR_BROKEN_PIPE          109u
#define ERROR_INVALID_NAME         123u
#define ERROR_HANDLE_EOF           38u
#define ERROR_NO_DATA              232u
#define ERROR_MORE_DATA            234u
#define MEM_COMMIT                 0x1000u
#define MEM_RESERVE                0x2000u
#define PAGE_READWRITE             4u

// FILE_INFO_BY_HANDLE_CLASS, of GetFileInformationByHandleEx and
// SetFileInformationByHandle
enum {
    FileBasicInfo        = 0,
    FileRenameInfo       = 3,
    FileDispositionInfo  = 4,
    FileEndOfFileInfo    = 6,
    FileAttributeTagInfo = 9,
    FileIdInfo           = 18,
    FileRenameInfoEx     = 22,
};

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 volume;
    u32 size_hi, size_lo;
    u32 links;
    u32 index_hi, index_lo;
} by_handle_info;

typedef struct {
    i64 created, accessed, written, changed;  // zero means unchanged
    u32 attributes;                           // zero means unchanged
} basic_info;

typedef struct {
    u32 attributes;
    u32 reparse_tag;
} attribute_tag_info;

// Whether a reparse point is a link: its tag a name surrogate, which
// names another file, as a symbolic link, a junction, or a WSL or
// container link does (IsReparseTagNameSurrogate). Others, such as a
// cloud placeholder or a deduplicated file, are the file itself.
static b32 reparse_link(attribute_tag_info tag)
{
    return (tag.attributes & FILE_ATTRIBUTE_REPARSE) &&
           (tag.reparse_tag & IO_REPARSE_TAG_SURROGATE);
}

typedef struct {
    u32  flags;  // FileRenameInfoEx, else a BOOLEAN ReplaceIfExists
    iptr root;
    u32  len;    // bytes
    c16  name[];
} rename_info;

typedef struct {
    uptr status;  // an NTSTATUS, or a pointer
    uptr information;
} io_status;

typedef struct {
    u16  len, cap;  // bytes
    c16 *s;
} unicode_string;

// Functions newer than Windows XP (Vista's), looked up at run time so
// that the programs import only XP's, and where they are missing, the
// native functions of ntdll, which every version has, through which they
// work. Defining TUGZ_FORCE_XP pretends that Vista's are missing, and
// that FindFirstFileExW takes only XP's arguments (find_first), to test
// the fallbacks on a newer Windows. A function not found is null.
typedef struct {
    b32 (__stdcall *getinfo)(iptr, i32, void *, u32);   // ...ByHandleEx
    b32 (__stdcall *setinfo)(iptr, i32, void *, u32);   // ...ByHandle
    u32 (__stdcall *finalpath)(iptr, c16 *, u32, u32);  // ...NameByHandleW
    i32 (__stdcall *ntquery)(iptr, io_status *, void *, u32, i32);
    i32 (__stdcall *ntset)(iptr, io_status *, void *, u32, i32);
    i32 (__stdcall *ntname)(iptr, i32, void *, u32, u32 *);  // NtQueryObject
    u32 (__stdcall *doserror)(i32);  // RtlNtStatusToDosError
    b32 oldfind;  // FindFirstFileExW has refused Windows 7's arguments
} winapi;

#ifdef TUGZ_FORCE_XP
enum { FORCE_XP = 1 };
#else
enum { FORCE_XP = 0 };
#endif

enum { MAX_HANDLES = 8 };

struct os {
    iptr  handles[MAX_HANDLES];
    u32   consoles;    // bit for each handle that is a console
    u8    held[3][4];  // for each output, an incomplete UTF-8 sequence
    u8    nheld[3];
    u8    rest[8];     // console input converted but not yet read
    u8    nrest;
    c16   high;        // a high surrogate read last, for its low one
    b32   wildnames;   // gzip: a name with * or ? is not found (os_open)
    i32   unsure;      // creations refused in a row, each name maybe taken
    iptr  guard;       // zip's archive, held so that it can be replaced
    byte *lo;          // zip: the uncommitted middle of its memory
    byte *hi;
    winapi api;

    // A host running a program's work in its own process (os_call), as
    // the GUI does, rather than the program running alone: os_exit
    // returns to its escape, and its meter sees each path opened, listed,
    // or examined, and each read (of len bytes) and write of a file
    // (descriptors 3 and up), and may end the work there, by os_exit.
    void **escape;     // a __builtin_setjmp buffer, or null
    i32    status;     // os_exit's, for the host
    void (*meter)(os *, s8 path, iz len);
};

// The host's meter, if any, of a path, or of len bytes read (or of a
// write, with neither).
static void meter(os *ctx, s8 path, iz len)
{
    if (ctx->meter) {
        ctx->meter(ctx, path, len);
    }
}

// A console control event's handling (see on_ctrl): the writer it is to
// write, and whether it has begun (1) or is done (2).
static writer *held_text;
static i32     ending;

// The program's thread stops here once the process is ending, for its
// end, rather than make anything more visible.
static void stop_if_ending(void)
{
    if (__atomic_load_n(&ending, __ATOMIC_ACQUIRE)) {
        for (;;) {
            Sleep((u32)-1);  // INFINITE
        }
    }
}

// Look up the functions of winapi, once (os_init).
static void load_api(winapi *w)
{
    iptr k32 = GetModuleHandleW(L"kernel32.dll");
    iptr nt  = GetModuleHandleW(L"ntdll.dll");  // in every process
    if (!FORCE_XP && k32) {
        w->getinfo   = (b32 (__stdcall *)(iptr, i32, void *, u32))
                       GetProcAddress(k32, "GetFileInformationByHandleEx");
        w->setinfo   = (b32 (__stdcall *)(iptr, i32, void *, u32))
                       GetProcAddress(k32, "SetFileInformationByHandle");
        w->finalpath = (u32 (__stdcall *)(iptr, c16 *, u32, u32))
                       GetProcAddress(k32, "GetFinalPathNameByHandleW");
    }
    if (nt) {
        w->ntquery  = (i32 (__stdcall *)(iptr, io_status *, void *, u32, i32))
                      GetProcAddress(nt, "NtQueryInformationFile");
        w->ntset    = (i32 (__stdcall *)(iptr, io_status *, void *, u32, i32))
                      GetProcAddress(nt, "NtSetInformationFile");
        w->ntname   = (i32 (__stdcall *)(iptr, i32, void *, u32, u32 *))
                      GetProcAddress(nt, "NtQueryObject");
        w->doserror = (u32 (__stdcall *)(i32))
                      GetProcAddress(nt, "RtlNtStatusToDosError");
    }
}

// The FILE_INFORMATION_CLASS of a FILE_INFO_BY_HANDLE_CLASS, whose
// structure is the same, or zero where XP has none.
static i32 ntclass(i32 cls)
{
    switch (cls) {
    case FileBasicInfo:        return 4;   // FileBasicInformation
    case FileRenameInfo:       return 10;  // FileRenameInformation
    case FileDispositionInfo:  return 13;  // FileDispositionInformation
    case FileEndOfFileInfo:    return 20;  // FileEndOfFileInformation
    case FileAttributeTagInfo: return 35;  // FileAttributeTagInformation
    }
    return 0;  // FileIdInfo, FileRenameInfoEx: refused, as Windows 7 does
}

// The result of a native call, its status, as the Win32 one would give it.
static b32 ntresult(winapi *w, i32 status)
{
    if (status < 0) {  // an error or a warning, not NT_SUCCESS
        SetLastError(w->doserror ? w->doserror(status)
                                 : ERROR_INVALID_FUNCTION);
        return 0;
    }
    return 1;
}

// GetFileInformationByHandleEx, or as it does, NtQueryInformationFile.
static b32 get_info(os *ctx, iptr h, i32 cls, void *buf, u32 len)
{
    winapi *w = &ctx->api;
    if (w->getinfo) {
        return w->getinfo(h, cls, buf, len);
    } else if (!ntclass(cls) || !w->ntquery) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    io_status io = {0};
    return ntresult(w, w->ntquery(h, &io, buf, len, ntclass(cls)));
}

// SetFileInformationByHandle, or as it does, NtSetInformationFile, which
// renames to an NT path, where it renames to a Win32 one. Each name to
// rename to is a \\?\ path (winpath), which is \??\ in NT.
static b32 set_info(os *ctx, iptr h, i32 cls, void *buf, u32 len)
{
    winapi *w = &ctx->api;
    if (w->setinfo) {
        return w->setinfo(h, cls, buf, len);
    } else if (!ntclass(cls) || !w->ntset) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    c16 *name = 0;
    if (cls == FileRenameInfo) {
        rename_info *ri = buf;
        name = ri->len>=8 && ri->name[0]=='\\' && ri->name[1]=='\\' &&
               ri->name[2]=='?' && ri->name[3]=='\\' ? ri->name : 0;
    }
    if (name) {
        name[1] = '?';
    }
    io_status io = {0};
    i32 status = w->ntset(h, &io, buf, len, ntclass(cls));
    if (name) {
        name[1] = '\\';
    }
    return ntresult(w, status);
}

typedef struct {
    c16 *s;
    iz   len;
} s16;

// Encode UTF-16 as WTF-8, where unpaired surrogates encode like any
// other code point so that arbitrary file names round trip, into d
// unless null. Returns the length, at most three bytes a unit.
static iz wtf8_encode(u8 *d, c16 const *w, iz wlen)
{
    static u8 const lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
    iz len = 0;
    for (iz i = 0; i < wlen; i++) {
        u32 c = w[i];
        if (c>=0xd800 && c<=0xdbff && i+1<wlen &&
            w[i+1]>=0xdc00 && w[i+1]<=0xdfff) {
            c = 0x10000 + ((c - 0xd800)<<10) + (w[++i] - 0xdc00);
        }
        i32 n = c<0x80 ? 1 : c<0x800 ? 2 : c<0x10000 ? 3 : 4;
        for (i32 k = n-1; d && k>0; k--, c >>= 6) {
            d[len+k] = (u8)(0x80 | (c & 63));
        }
        if (d) {
            d[len] = (u8)(lead[n] | c);
        }
        len += n;
    }
    return len;
}

// Convert null-terminated UTF-16 to WTF-8, allocating just its length,
// since an arena that grows down (zip's scratch) could not give back
// the rest of a guess at it, as for every name in a listing.
static s8 towtf8(arena *a, c16 *w)
{
    iz wlen = 0;
    for (; w[wlen]; wlen++) {}
    s8 r  = {0};
    r.len = wtf8_encode(0, w, wlen);
    r.s   = newstr(a, r.len);
    wtf8_encode(r.s, w, wlen);
    return r;
}

// Continuation bytes following a UTF-8 lead byte, or 0 if c is not one.
static i32 wtf8_tail(u32 c)
{
    return c>=0xc2 && c<0xe0 ? 1 : c>=0xe0 && c<0xf0 ? 2 :
           c>=0xf0 && c<0xf5 ? 3 : 0;
}

// Decode a code point from WTF-8 at s[*i], advancing past it. Invalid
// sequences become U+FFFD.
static u32 wtf8_decode(u8 const *s, iz len, iz *i)
{
    u32 c = s[(*i)++];
    if (c >= 0x80) {
        i32 n   = wtf8_tail(c);
        u32 min = n==3 ? 0x10000 : n==2 ? 0x800 : 0x80;
        c = n==3 ? c&7 : n==2 ? c&15 : c&31;
        i32 k = 0;
        for (; k<n && *i<len && (s[*i]&0xc0)==0x80; k++) {
            c = c<<6 | (s[(*i)++] & 63);
        }
        if (!n || k<n || c<min || c>0x10ffff) {
            c = 0xfffd;
        }
    }
    return c;
}

// Append a code point as UTF-16 and return the new length.
static iz utf16_put(c16 *d, iz len, u32 c)
{
    if (c >= 0x10000) {
        c -= 0x10000;
        d[len++] = (c16)(0xd800 | c>>10);
        d[len++] = (c16)(0xdc00 | (c & 0x3ff));
    } else {
        d[len++] = (c16)c;
    }
    return len;
}

// Convert WTF-8 to null-terminated UTF-16. Invalid sequences become
// U+FFFD and slashes become backslashes.
static s16 fromwtf8(arena *a, s8 s)
{
    s16 r = {new(a, s.len+1, c16), 0};
    for (iz i = 0; i < s.len;) {
        u32 c = wtf8_decode(s.s, s.len, &i);
        r.len = utf16_put(r.s, r.len, c=='/' ? '\\' : c);
    }
    r.s[r.len] = 0;
    return r;
}

static c16 *s16cat(arena *a, s16 x, s16 y)
{
    c16 *r = new(a, x.len+y.len+1, c16);
    bytecopy(r, x.s, x.len*(iz)sizeof(c16));
    bytecopy(r+x.len, y.s, y.len*(iz)sizeof(c16));
    return r;
}

static s16 s16lit(c16 *z)
{
    s16 r = {z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

// A device path, \\?\ or \\.\, after slashes became backslashes.
static b32 isdevice(s16 p)
{
    return p.len>=4 && p.s[0]=='\\' && p.s[1]=='\\' &&
           (p.s[2]=='?' || p.s[2]=='.') && p.s[3]=='\\';
}

// A path written exactly \\?\..., which Win32 takes as it is. It
// resolves any other device path (//?/..., \\.\...) like any path.
static b32 isverbatim(s8 path)
{
    return path.len>=4 && path.s[0]=='\\' && path.s[1]=='\\' &&
           path.s[2]=='?' && path.s[3]=='\\';
}

// Length of an absolute path's root: a drive "X:", a share
// "\\server\share", or a device path through its first component
// ("\\?\X:", "\\?\Volume{...}") or share ("\\?\UNC\server\share").
// Zero for a relative path, and negative for a root missing one of
// these parts ("\\server\", "\\?\"), which names nothing.
static iz rootlen(s16 p)
{
    c16 *s = p.s;
    if (p.len < 2) {
        return 0;
    } else if (s[0]!='\\' || s[1]!='\\') {
        b32 letter = (s[0]>='A' && s[0]<='Z') || (s[0]>='a' && s[0]<='z');
        return letter && s[1]==':' ? 2 : 0;
    }
    iz  i     = 2;
    i32 parts = 2;  // server and share
    if (isdevice(p)) {
        b32 unc = p.len>=8 && (s[4]|32)=='u' && (s[5]|32)=='n' &&
                  (s[6]|32)=='c' && s[7]=='\\';
        i     = unc ? 8 : 4;
        parts = unc ? 2 : 1;
    }
    for (i32 k = 0; k < parts; k++) {
        i += k && i<p.len;  // separator
        iz beg = i;
        for (; i<p.len && s[i]!='\\'; i++) {}
        if (i == beg) {
            return -1;
        }
    }
    return i;
}

// The current directory, or that of a drive given its letter.
static s16 curdir(arena *a, c16 drive)
{
    c16 name[] = {drive, ':', 0};
    u32 cap = drive ? GetFullPathNameW(name, 0, 0, 0)
                    : GetCurrentDirectoryW(0, 0);
    if (!cap) {
        return (s16){0};
    }
    s16 r = {new(a, cap, c16), 0};
    u32 len = drive ? GetFullPathNameW(name, cap, r.s, 0)
                    : GetCurrentDirectoryW(cap, r.s);
    if (!len || len>=cap) {
        return (s16){0};
    }
    r.len = len;
    return r;
}

// The device path of a bare name that Win32 takes for a DOS device (NUL,
// CON, COM1, ..., perhaps with an extension, as GetFullPathNameW decides
// for this version of Windows), or null.
static c16 *dosdevice(arena *a, s16 p)
{
    for (iz i = 0; i < p.len; i++) {
        if (p.s[i] == '\\') {
            return 0;
        }
    }
    if (rootlen(p)) {
        return 0;  // drive-relative
    }
    c16 dev[16];
    u32 len = GetFullPathNameW(p.s, countof(dev), dev, 0);
    if (!len || len>=countof(dev) || !isdevice((s16){dev, len}) ||
        dev[2]!='.') {
        return 0;
    }
    c16 *r = new(a, len+1, c16);
    bytecopy(r, dev, (len+1)*(iz)sizeof(c16));
    return r;
}

// Convert a path to an absolute \\?\ path, lifting the MAX_PATH limit.
// That prefix turns off Win32 path parsing, so first resolve the path as
// Win32 would: against the current directory (or a drive's, or its
// root), dropping "." and ".." components and doubled separators. Unlike
// Win32, keep trailing dots and spaces, so that names from a directory
// listing round trip, and stop ".." at a device path's volume or share.
// A \\.\ path, which Win32 would parse again, becomes \\?\, which names
// the same devices. A path written exactly \\?\ passes through, as Win32
// passes it, and a bare DOS device name (NUL) becomes its device as in
// any Windows program. Within a directory such names stay files, as they
// are in a listing, since other systems make them. Returns null for a
// path that names no file: an empty one, or one with an incomplete root.
static c16 *winpath(arena *a, s8 path)
{
    s16 p = fromwtf8(a, path);
    if (!p.len) {
        return 0;
    } else if (isverbatim(path)) {
        return p.s;
    }
    c16 *dev = dosdevice(a, p);
    if (dev) {
        return dev;
    }

    b32 dirsep  = p.s[p.len-1] == '\\';
    b32 devpath = isdevice(p);
    iz  root    = rootlen(p);
    s16 base    = {0};
    if (root < 0) {
        return 0;
    } else if (!root) {
        base = curdir(a, 0);
        if (base.s && p.s[0]=='\\') {
            base.len = rootlen(base);  // root-relative
        }
        if (base.len <= 0) {
            return 0;
        }
    } else if (p.s[1]==':' && (p.len==2 || p.s[2]!='\\')) {
        base = curdir(a, p.s[0]);  // drive-relative, "X:name"
        if (!base.s) {
            return 0;
        }
        p.s   += 2;
        p.len -= 2;
    }
    if (base.s) {
        c16 *s = new(a, base.len+1+p.len, c16);
        bytecopy(s, base.s, base.len*(iz)sizeof(c16));
        s[base.len] = '\\';
        bytecopy(s+base.len+1, p.s, p.len*(iz)sizeof(c16));
        p.s   = s;
        p.len = base.len + 1 + p.len;
        root  = rootlen(p);
        if (root <= 0) {
            return 0;
        }
    }

    // Prefix the root: "C:" to "\\?\C:", "\\server" to "\\?\UNC\server"
    s16 pre  = s16lit(L"\\\\?\\");
    iz  skip = 0;
    if (isdevice(p)) {
        pre.len = 0;
    } else if (p.s[0] == '\\') {
        pre  = s16lit(L"\\\\?\\UNC");
        skip = 1;
    }
    c16 *r = new(a, pre.len+p.len+2, c16);
    bytecopy(r, pre.s, pre.len*(iz)sizeof(c16));
    bytecopy(r+pre.len, p.s+skip, (root-skip)*(iz)sizeof(c16));
    r[2] = '?';  // \\.\ too
    iz top = pre.len + root - skip;

    iz n = top;
    for (iz i = root, end; i < p.len; i = end + 1) {
        for (end = i; end<p.len && p.s[end]!='\\'; end++) {}
        iz len = end - i;
        if (!len || (len==1 && p.s[i]=='.')) {
            continue;
        } else if (len==2 && p.s[i]=='.' && p.s[i+1]=='.') {
            for (; n>top && r[n-1]!='\\'; n--) {}
            n -= n > top;
            continue;
        }
        r[n++] = '\\';
        bytecopy(r+n, p.s+i, len*(iz)sizeof(c16));
        n += len;
    }
    // A root is a directory only with its separator, but as in Win32, a
    // device path keeps one only if it ends in one: \\?\C: is the volume
    if (dirsep || (n==top && !devpath)) {
        r[n++] = '\\';
    }
    r[n] = 0;
    return r;
}

// Returns zero and a handle, or an OS_E* code.
static i32 open_input(os *ctx, c16 *wpath, i32 mode, iptr *out)
{
    u32 flags = FILE_ATTRIBUTE_NORMAL;
    flags |= mode & OS_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE : 0;
    iptr h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        u32 why  = GetLastError();
        u32 attr = GetFileAttributesW(wpath);
        b32 dir  = attr!=INVALID_FILE_ATTRIBUTES &&
                   (attr & FILE_ATTRIBUTE_DIRECTORY);
        SetLastError(why);  // not the check's, as for a volume (87)
        return dir ? OS_EISDIR : OS_ERR;
    }

    if (mode & OS_NOFOLLOW) {
        // Refuse links, but other reparse points (e.g. cloud placeholder
        // files) are ordinary files: reopen those normally.
        attribute_tag_info tag = {0};
        get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag));
        if (tag.attributes & FILE_ATTRIBUTE_REPARSE) {
            CloseHandle(h);
            if (reparse_link(tag)) {
                return OS_ESYMLINK;
            }
            return open_input(ctx, wpath, mode & ~OS_NOFOLLOW, out);
        }
    }

    // A volume or raw disk is a disk file too, but it has no file
    // information, which tells it apart from a file that failed to give
    // it, as through a network error, which is no file type. Likewise an
    // unknown type is one only if telling it did not fail.
    i32 err  = 0;
    u32 type = FILE_TYPE_DISK;
    if (mode & OS_REGULAR) {
        SetLastError(0);
        type = GetFileType(h);
    }
    by_handle_info info = {0};
    if (type==FILE_TYPE_UNKNOWN && GetLastError()) {
        err = OS_ERR;
    } else if (type != FILE_TYPE_DISK) {
        err = OS_ENOTREG;
    } else if (!GetFileInformationByHandle(h, &info)) {
        u32 why    = GetLastError();
        b32 device = why==ERROR_INVALID_FUNCTION ||
                     why==ERROR_INVALID_PARAMETER || why==ERROR_NOT_SUPPORTED;
        err = !(mode & OS_REGULAR) ? 0 : device ? OS_ENOTREG : OS_ERR;
    } else if ((mode & OS_ONELINK) && info.links>1) {
        err = OS_ELINKS;
    }
    if (err) {
        u32 why = GetLastError();
        CloseHandle(h);
        SetLastError(why);  // for os_error
        return err;
    }
    *out = h;
    return 0;
}

// Delete a file, or a link rather than its target, as POSIX unlink
// does. DeleteFileW refuses two of those. A read-only file: clear the
// attribute through a handle, mark the file deleted, then restore it,
// which the deletion survives, for any other hard links to the file. A
// junction or another directory link (reparse_link), which is a
// directory to DeleteFileW:
// mark it deleted through a handle to the link itself, which removes
// only the link, never its target nor what is in it. Each is checked
// through the handle, so that what is deleted is what was checked,
// whatever the name has become meanwhile, and a directory that is not a
// link, read-only or not, empty or not, is left alone, as is any other
// reparse point that is a directory (a cloud placeholder holds files).
// Returns whether the file was deleted, and if not, leaves why in the
// last error.
static b32 remove_file(os *ctx, c16 *wpath)
{
    if (DeleteFileW(wpath)) {
        return 1;
    }
    u32 why     = GetLastError();
    u32 attr    = GetFileAttributesW(wpath);
    u32 dirlink = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE;
    u32 rofile  = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY;
    b32 link    = attr!=INVALID_FILE_ATTRIBUTES && (attr&dirlink)==dirlink;
    b32 locked  = why==ERROR_ACCESS_DENIED && attr!=INVALID_FILE_ATTRIBUTES &&
                  (attr&rofile)==FILE_ATTRIBUTE_READONLY;
    if (!link && !locked) {
        SetLastError(why);  // as DeleteFileW said
        return 0;
    }

    // Only a directory link opens with backup semantics, so the handle
    // is never to a directory otherwise
    u32  flags = FILE_FLAG_OPEN_REPARSE;
    u32  want  = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    flags |= link ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    iptr h = CreateFileW(wpath, want, FILE_SHARE_ALL, 0, OPEN_EXISTING,
                         flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    attribute_tag_info tag = {0};
    b32 ok = get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag));
    if (link) {
        ok = ok && (tag.attributes & FILE_ATTRIBUTE_DIRECTORY) &&
             reparse_link(tag);
    } else {
        ok = ok && (tag.attributes&rofile)==FILE_ATTRIBUTE_READONLY;
    }
    if (!ok) {
        CloseHandle(h);
        SetLastError(why);
        return 0;
    }

    b32 deleted = 0;
    u32 err     = 0;
    u8  discard = 1;
    basic_info info = {0};
    info.attributes = FILE_ATTRIBUTE_NORMAL;
    if (!(tag.attributes & FILE_ATTRIBUTE_READONLY)) {
        deleted = set_info(ctx, h, FileDispositionInfo, &discard, 1);
        err = GetLastError();
    } else if (set_info(ctx, h, FileBasicInfo, &info, sizeof(info))) {
        deleted = set_info(ctx, h, FileDispositionInfo, &discard, 1);
        err = GetLastError();
        if (!link || !deleted) {
            info.attributes = tag.attributes;
            set_info(ctx, h, FileBasicInfo, &info, sizeof(info));
        }
    } else {
        err = GetLastError();
    }
    CloseHandle(h);
    SetLastError(err);
    return deleted;
}

// Created files are marked delete-pending immediately, so that the file
// system removes them however the process ends, until kept: by os_keep
// (gzip), or by zip's os_commit, which clears it just before renaming.
static i32 open_output(os *ctx, i32 fd, c16 *wpath, i32 mode)
{
    u32 kept = 0;  // why a name to replace could not be removed
    if (mode & OS_FORCE && !remove_file(ctx, wpath)) {
        // Replace rather than write through a link
        kept = GetLastError();
        kept = kept==ERROR_FILE_NOT_FOUND ? 0 : kept;
    }
    iptr h = CreateFileW(wpath, GENERIC_WRITE|DELETE, 0, 0, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, 0);
    i32 unsure = ctx->unsure;
    ctx->unsure = 0;
    if (h == INVALID_HANDLE_VALUE) {
        // Forced, a name that could not be replaced is an error, for
        // the reason it could not be removed. Else a name that another
        // process holds delete-pending (its output not yet kept)
        // refuses access rather than reporting that it exists, and so
        // does checking for it. But where the directory refuses that
        // check for any name (no traverse rights), every name looks
        // taken, so after a run of 64 such names give up rather than
        // let a caller seeking a free name try them all.
        u32 err = GetLastError();
        if (mode & OS_FORCE) {
            if (err==ERROR_FILE_EXISTS && kept) {
                SetLastError(kept);
            }
            return OS_ERR;
        } else if (err==ERROR_ACCESS_DENIED || err==ERROR_SHARING_VIOLATION) {
            b32 found = GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
            if (found) {
                err = ERROR_FILE_EXISTS;
            } else if (GetLastError()==ERROR_ACCESS_DENIED && unsure<64) {
                ctx->unsure = unsure + 1;
                err = ERROR_FILE_EXISTS;
            }
            SetLastError(err);  // why it failed, rather than the check
        }
        return err==ERROR_FILE_EXISTS ? OS_EEXIST : OS_ERR;
    } else if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);  // a device name (NUL): output is only ever a file
        return OS_ERR;
    }
    u8 discard = 1;
    if (!set_info(ctx, h, FileDispositionInfo, &discard, sizeof(discard))) {
        CloseHandle(h);
        DeleteFileW(wpath);
        return OS_ERR;
    }
    ctx->handles[fd] = h;
    return fd;
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    meter(ctx, path, 0);
    i32 fd = 3;
    for (; fd<MAX_HANDLES && ctx->handles[fd]; fd++) {}
    c16 *wpath = winpath(&scratch, path);
    if (fd==MAX_HANDLES || !wpath) {
        // Say why, as a failed CreateFileW would, for os_missing and the
        // like: a path that names nothing is a name no file can have
        SetLastError(wpath ? ERROR_TOO_MANY_OPEN_FILES : ERROR_INVALID_NAME);
        return OS_ERR;
    }

    if (mode & (OS_CREATE|OS_FORCE)) {
        return open_output(ctx, fd, wpath, mode);
    }
    iptr h = 0;
    i32 err = open_input(ctx, wpath, mode, &h);
    if (err) {
        // A wildcard that matched nothing stays as it is, as a POSIX
        // shell leaves it, and as there names no file, though Windows
        // calls a name with one invalid
        if (err==OS_ERR && ctx->wildnames && zip_haswild(path, 0) &&
            GetLastError()==ERROR_INVALID_NAME) {
            SetLastError(ERROR_FILE_NOT_FOUND);
        }
        return err;
    }
    ctx->handles[fd] = h;

    // A console opened by name (CON, CONIN$), a DOS device, is read as
    // standard input is when it is one
    u32 cmode = 0;
    b32 dosdev = wpath[0]=='\\' && wpath[1]=='\\' && wpath[2]=='.';
    ctx->consoles |= (dosdev && GetConsoleMode(h, &cmode) ? 1u : 0u) << fd;
    return fd;
}

static b32 os_close(os *ctx, i32 fd)
{
    b32 ok = CloseHandle(ctx->handles[fd]);
    ctx->handles[fd] = 0;
    ctx->consoles &= ~(1u << fd);
    return ok;
}

// Setting delete-pending just worked on this handle, so clearing it
// should too, but a file system or filter may yet refuse.
[[maybe_unused]] static b32 os_keep(os *ctx, i32 fd)
{
    stop_if_ending();
    u8 keep = 0;
    return set_info(ctx, ctx->handles[fd], FileDispositionInfo, &keep,
                    sizeof(keep));
}

// Read a console as UTF-16, converted to WTF-8, since ReadFile would
// give the bytes in the console's input code page: write_console's
// counterpart, so that names typed for zip -@ are UTF-8 like the rest.
// As ReadFile does, a read that begins with Ctrl+Z, as a line does when
// one ends input, is the end. A high surrogate at the end of a read waits
// for the low one that may follow, and bytes that do not fit for the
// next read.
static iz read_console(os *ctx, i32 fd, u8 *buf, iz cap)
{
    if (ctx->nrest) {
        iz n = MIN(cap, ctx->nrest);
        bytecopy(buf, ctx->rest, n);
        bytemove(ctx->rest, ctx->rest+n, ctx->nrest-n);
        ctx->nrest -= (u8)n;
        return n;
    } else if (cap < countof(ctx->rest)) {
        // Room for any code point and a surrogate held before it
        iz n = read_console(ctx, fd, ctx->rest, countof(ctx->rest));
        ctx->nrest = (u8)MAX(n, 0);
        return n>0 ? read_console(ctx, fd, buf, cap) : n;
    }

    for (;;) {
        c16 w[512];
        iz  n = ctx->high ? 1 : 0;
        w[0] = ctx->high;
        ctx->high = 0;
        u32 max = (u32)MIN(countof(w)-n, (cap - 3*n)/3);  // 3 bytes a unit
        u32 got = 0;
        if (!ReadConsoleW(ctx->handles[fd], w+n, max, &got, 0)) {
            return -1;
        } else if (!n && (!got || w[0]==0x1a)) {
            return 0;
        }
        n += got;
        if (got && w[n-1]>=0xd800 && w[n-1]<=0xdbff) {
            ctx->high = w[--n];
        }
        iz len = wtf8_encode(buf, w, n);
        if (len) {
            return len;
        }
    }
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    if (ctx->consoles>>fd & 1) {
        return read_console(ctx, fd, buf, cap);
    }
    iptr h = ctx->handles[fd];
    for (;;) {
        u32 got = 0;
        if (ReadFile(h, buf, (u32)MIN(cap, 1<<30), &got, 0)) {
            if (fd >= 3) {
                meter(ctx, (s8){0}, got);
            }
            return got;
        }
        u32 err = GetLastError();
        if (err==ERROR_BROKEN_PIPE || err==ERROR_HANDLE_EOF) {
            return 0;
        } else if (err == ERROR_MORE_DATA) {
            return got;  // part of a longer message (message-mode pipe)
        }

        // An inherited pipe left non-blocking (PIPE_NOWAIT), as another
        // program may leave one, has no input yet: wait for input from
        // now on, as POSIX does for O_NONBLOCK, keeping its read mode
        u32 state = 0;
        if (err!=ERROR_NO_DATA ||
            !GetNamedPipeHandleStateW(h, &state, 0, 0, 0, 0, 0) ||
            !(state & PIPE_NOWAIT)) {
            SetLastError(err);
            return -1;
        }
        u32 mode = state & PIPE_READMODE_MESSAGE;  // and PIPE_WAIT
        if (!SetNamedPipeHandleState(h, &mode, 0, 0)) {
            SetLastError(err);
            return -1;
        }
    }
}

// Write UTF-8 (WTF-8) to a console as UTF-16, since WriteFile would take
// the bytes in the console's code page. An incomplete sequence at the
// end is held for the next write, as output may be split anywhere.
static b32 write_console(os *ctx, i32 fd, u8 *buf, iz len)
{
    u8  in[512];
    c16 out[countof(in)];  // at most one unit per byte
    iz  n = ctx->nheld[fd];
    bytecopy(in, ctx->held[fd], n);
    for (;;) {
        iz take = MIN(len, countof(in)-n);
        bytecopy(in+n, buf, take);
        buf += take;
        len -= take;
        n   += take;

        // Find an incomplete sequence at the end
        iz end = n;
        for (iz k = 1; k<=3 && k<=n; k++) {
            if ((in[n-k]&0xc0) != 0x80) {
                end -= wtf8_tail(in[n-k]) >= k ? k : 0;
                break;
            }
        }

        iz m = 0;
        for (iz i = 0; i < end;) {
            m = utf16_put(out, m, wtf8_decode(in, end, &i));
        }
        for (iz i = 0; i < m;) {
            u32 wrote = 0;
            if (!WriteConsoleW(ctx->handles[fd], out+i, (u32)(m-i),
                               &wrote, 0) || !wrote) {
                return 0;
            }
            i += wrote;
        }

        bytemove(in, in+end, n-end);
        n -= end;
        if (!len) {
            break;
        }
    }
    bytecopy(ctx->held[fd], in, n);
    ctx->nheld[fd] = (u8)n;
    return 1;
}

static b32 write_fd(os *ctx, i32 fd, u8 *buf, iz len)
{
    if ((u32)fd<3 && ctx->consoles>>fd & 1) {
        return write_console(ctx, fd, buf, len);
    }
    while (len) {
        u32 wrote = 0;
        u32 n = (u32)MIN(len, 1<<30);
        if (!WriteFile(ctx->handles[fd], buf, n, &wrote, 0) || !wrote) {
            return 0;
        }
        buf += wrote;
        len -= wrote;
    }
    return 1;
}

// A console control event (Ctrl+C, Ctrl+Break, the console closed, a
// logoff or shutdown) ends the process, as on POSIX a signal does, but
// its handler runs on a thread of its own, beside the program's. The
// writer of standard output that zip and unzip hold (os_holdtext) is
// written there as the process ends, as POSIX's signal handler writes it
// (a closed console then takes nothing), and the event is then passed to
// the default handler, which ends the process as it always has
// (STATUS_CONTROL_C_EXIT, 0xC000013A, for each event). Outputs not yet
// kept need no handling, as they are delete-pending.
//
// The handler claims the writer (writer_claim) by an atomic exchange of
// its state, idle to claimed, and writes the text buffered by then. While
// the program's thread is writing (from the buffer or past it), it waits
// for that write to end, up to 2 s (it may block on a full pipe), then
// gives up, writing nothing. Once claimed, the program's thread never
// writes that text, nor touches it: it may go on appending past it, but
// stops at its next write of the writer, which must first exchange the
// state idle to writing. No text is written twice, none torn, and none
// out of order. From the moment the handler begins (ending), the
// program's thread also stops before anything else it would make
// visible: any other write (os_write, zip's os_writeat), a file kept or
// committed, and its exit, so that it neither keeps an output nor ends
// the process with its own status while the handler writes. Its writer's
// own writes go on, as the handler waits for them. The thread stops,
// rather than being suspended, so that it never holds a lock (the
// console's) that the handler needs. A second event's handler waits for
// the first's, for a while too.
static b32 __stdcall on_ctrl(u32 event)
{
    (void)event;
    enum { PATIENCE = 2000 };  // milliseconds
    u32 start = GetTickCount();
    i32 none  = 0;
    if (__atomic_compare_exchange_n(&ending, &none, 1, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        writer *w = __atomic_load_n(&held_text, __ATOMIC_ACQUIRE);
        while (w) {
            iz len = writer_claim(w);
            if (len >= 0) {
                if (len && !w->err && w->fd>=0) {
                    write_fd(w->ctx, w->fd, w->buf, len);
                }
                break;
            } else if (GetTickCount()-start >= PATIENCE) {
                break;  // its write is blocked: give up
            }
            Sleep(10);
        }
        __atomic_store_n(&ending, 2, __ATOMIC_RELEASE);
    } else {
        // Once the first's write has blocked for a while, another event
        // ends the process
        while (__atomic_load_n(&ending, __ATOMIC_ACQUIRE) != 2 &&
               GetTickCount()-start < PATIENCE) {
            Sleep(10);
        }
    }
    return 0;  // to the default handler, which ends the process
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    writer *held = __atomic_load_n(&held_text, __ATOMIC_ACQUIRE);
    if (!held || fd!=held->fd) {
        stop_if_ending();  // (the held writer's stop at writer_begin)
    }
    b32 ok = write_fd(ctx, fd, buf, len);
    if (ok && fd>=3) {
        meter(ctx, (s8){0}, 0);
    }
    return ok;
}

static b32 os_isatty(os *ctx, i32 fd)
{
    return (u32)fd<3 && ctx->consoles>>fd & 1;
}

// Hold w's text, for a console control event's handler to write (see
// on_ctrl), installed the first time.
[[maybe_unused]] static void os_holdtext(os *ctx, writer *w)
{
    (void)ctx;
    static b32 installed;
    __atomic_store_n(&held_text, w, __ATOMIC_RELEASE);
    if (w && !installed) {
        installed = SetConsoleCtrlHandler(on_ctrl, 1);
    }
}

// To the system timer's 10-16 ms, wrapping after 49 days
static i64 os_now(os *ctx)
{
    (void)ctx;
    return GetTickCount();
}

// Nothing there: no such file, nor a directory on the way to it, rather
// than a share or server not found, which one that is down may give, or
// a name that no file can have.
static b32 os_missing(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND;
}

// Why the last system call failed, as the C runtime of Info-ZIP's port,
// and of GNU gzip built for Windows, would say: in the words of its
// strerror for the errno to which its _dosmaperr maps the code (checked
// against msvcrt, and the UCRT, which differs only in mapping 1113 to
// EILSEQ), or an empty string if none failed. Beyond its table, a disk
// full through a handle (39) is a full disk too, and a pipe being closed
// (232), which gzip takes for a closed pipe, a broken one.
static s8 os_error(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    switch (err) {
    case 0:
        return S("");
    case 2: case 3: case 15: case 18: case 53: case 67: case 161: case 206:
        return S("No such file or directory");
    case 4:
        return S("Too many open files");
    case 5: case 16: case 65: case 82: case 83: case 108: case 132:
    case 158: case 167:
        return S("Permission denied");
    case 6: case 114: case 130:
        return S("Bad file descriptor");
    case 7: case 8: case 9: case 1816:
        return S("Not enough space");
    case 10:
        return S("Arg list too long");
    case 11:
        return S("Exec format error");
    case 17:
        return S("Improper link");
    case 39: case 112:
        return S("No space left on device");
    case 80: case 183:
        return S("File exists");
    case 89: case 164: case 215:
        return S("Resource temporarily unavailable");
    case 109: case 232:
        return S("Broken pipe");
    case 128: case 129:
        return S("No child processes");
    case 145:
        return S("Directory not empty");
    }
    return err>=19 && err<=36   ? S("Permission denied")  // sharing, locks
         : err>=188 && err<=202 ? S("Exec format error")
         : S("Invalid argument");
}

// As unlink, even of a read-only file (remove_file).
[[maybe_unused]] static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return 0;
    }
    return remove_file(ctx, wpath);
}

// Unix seconds from a FILETIME, rounding down.
static i64 unixtime(u32 const ft[2])
{
    i64 t = (i64)((u64)ft[1]<<32 | ft[0]) - 116444736000000000;
    return t>=0 ? t/10000000 : -((-t + 9999999)/10000000);
}

// The first entry of a listing, which skips short names, fetching many
// entries at a time, with Windows 7's FindExInfoBasic and
// FIND_FIRST_EX_LARGE_FETCH. Older versions refuse both as an invalid
// parameter: then list as they do, from then on once that works.
static iptr find_first(os *ctx, c16 *pattern, find_data *fd)
{
    if (ctx->api.oldfind) {
        return FindFirstFileExW(pattern, 0, fd, 0, 0, 0);
    }
    iptr h = INVALID_HANDLE_VALUE;
    if (FORCE_XP) {
        SetLastError(ERROR_INVALID_PARAMETER);
    } else {
        h = FindFirstFileExW(pattern, 1, fd, 0, 0, FIND_FIRST_EX_LARGE_FETCH);
    }
    if (h==INVALID_HANDLE_VALUE && GetLastError()==ERROR_INVALID_PARAMETER) {
        h = FindFirstFileExW(pattern, 0, fd, 0, 0, 0);
        ctx->api.oldfind |= h!=INVALID_HANDLE_VALUE ||
                            GetLastError()!=ERROR_INVALID_PARAMETER;
    }
    return h;
}

// Hidden and system entries are judged by the attributes in the listing,
// which are a link's own, as Info-ZIP does, and which need no handle to
// the file (some, like pagefile.sys, cannot be opened at all). Plain
// files and directories are described from the listing too, so that
// scanning opens only those whose identity it needs. Links, and other
// reparse points, are left to os_stat, which follows them. A directory
// entry's size and times can lag for a file changed through another of
// its hard links, as Microsoft documents, where a handle's would not.
// The search pattern, dead once the search begins, is overwritten by the
// listing, so that a deep tree's directories do not each keep theirs.
// Each entry is first packed after the last, its name, a terminator, and
// what the listing tells of it, then entries are made for them all at
// once: an array doubled as it grew would, in an arena that grows down
// (zip's scratch), or in one where names follow it (gzip's), leave each
// smaller copy behind, some 265 bytes a name in all on x86-64, where
// this takes 117 and the name.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    meter(ctx, path, 0);
    arena tmp   = *a;
    c16  *wpath = winpath(&tmp, path);
    if (!wpath) {
        return 0;
    }
    s16 dir = s16lit(wpath);
    b32 sep = dir.len && dir.s[dir.len-1]=='\\';
    c16 *pattern = s16cat(&tmp, dir, s16lit(sep ? L"*" : L"\\*"));

    typedef struct {
        u32 attributes;
        u32 written[2], accessed[2];
        u32 size_hi, size_lo;
    } found;
    u8 *first = (u8 *)(a->down ? a->end : a->beg);  // where entries begin
    u8 *last  = first;                              // and end
    iz  n     = 0;
    find_data fd = {0};
    iptr h = find_first(ctx, pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // Nothing matched: an empty directory without . and .., such as
        // an empty drive's root. A missing directory is PATH_NOT_FOUND.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return 0;
        }
    } else {
        u32 skip = all ? 0 : FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM;
        do {
            c16 *w    = fd.name;
            b32  dots = w[0]=='.' && (!w[1] || (w[1]=='.' && !w[2]));
            if ((fd.attributes & skip) || dots) {
                continue;
            }
            iz wlen = 0;
            for (; w[wlen]; wlen++) {}
            iz    len  = wtf8_encode(0, w, wlen);
            iz    size = len + 1 + (iz)sizeof(found);
            u8   *e    = newstr(a, size);
            found f    = {
                fd.attributes, {fd.written[0], fd.written[1]},
                {fd.accessed[0], fd.accessed[1]}, fd.size_hi, fd.size_lo
            };
            assert(a->down ? e+size==last : e==last);  // packed
            wtf8_encode(e, w, wlen);
            e[len] = 0;
            bytecopy(e+len+1, &f, sizeof(f));
            last = a->down ? e : e+size;
            n++;
        } while (FindNextFileW(h, &fd));
        b32 done = GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(h);
        if (!done) {
            return 0;  // not a partial listing
        }
    }

    os_dirent *list = new(a, n, os_dirent);  // type FT_NONE
    u8        *p    = MIN(first, last);
    for (iz i = 0; i < n; i++) {
        os_dirent *e = list + i;
        e->name.s = p;
        for (; p[e->name.len]; e->name.len++) {}
        found f;
        bytecopy(&f, p+e->name.len+1, sizeof(f));
        p += e->name.len + 1 + (iz)sizeof(f);
        if (!(f.attributes & FILE_ATTRIBUTE_REPARSE)) {
            b32 isdir = f.attributes & FILE_ATTRIBUTE_DIRECTORY;
            e->info.type  = isdir ? FT_DIR : FT_FILE;
            e->info.size  = (i64)((u64)f.size_hi<<32 | f.size_lo);
            e->info.mtime = unixtime(f.written);
            e->info.atime = unixtime(f.accessed);
            e->info.attr  = f.attributes;
        }
    }
    *count = n;
    return list;
}

// Upper case by the system's "file system rules", its default without
// LCMAP_LINGUISTIC_CASING, which map each UTF-16 unit to one, as file
// names are compared ignoring case: the case that the C runtime's
// towupper, which Info-ZIP's port matches wildcards with, gives too.
static s8 os_upcase(os *ctx, s8 name, arena *a)
{
    (void)ctx;
    s16  w = fromwtf8(a, name);
    c16 *u = new(a, w.len+1, c16);
    i32  n = LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, w.s, (i32)w.len,
                          u, (i32)w.len);
    if (n != w.len) {
        return name;
    }
    u[n] = 0;
    return towtf8(a, u);
}

// Created files not kept are delete-pending, so they need no cleanup.
// An incomplete UTF-8 sequence still held for a console ended the
// output, which makes it invalid: write it out as U+FFFD. A host's work
// (os_call) ends instead of the process.
static void os_exit(os *ctx, i32 status)
{
    stop_if_ending();
    if (ctx->escape) {
        ctx->status = status;
        __builtin_longjmp(ctx->escape, 1);
    }
    for (i32 fd = 1; fd < 3; fd++) {
        if (ctx->nheld[fd]) {
            c16 bad = 0xfffd;
            u32 wrote = 0;
            WriteConsoleW(ctx->handles[fd], &bad, 1, &wrote, 0);
            ctx->nheld[fd] = 0;
        }
    }
    ExitProcess((u32)status);
}

// Run a program's work, fn, for a host in its own process, as the GUI
// runs zip and unzip, on the calling thread, returning its status. Should
// the work end by os_exit, as when memory runs out or the host's meter
// stops it, that returns here, rather than ending the process. Either way
// the files it left open are then closed, as the process's end would
// close them, since a program may leave some open as it returns, as zip
// leaves the archive when it fails before writing: those it created and
// did not keep, delete-pending, are gone, and an archive being replaced
// is as it was. The host keeps its state behind ctx, which the escape
// leaves be.
[[maybe_unused]] static i32 os_call(os *ctx, i32 (*fn)(os *, void *),
                                    void *arg)
{
    void *jmp[5];
    ctx->escape = jmp;
    if (!__builtin_setjmp(jmp)) {
        ctx->status = fn(ctx, arg);
    }
    ctx->escape = 0;
    for (i32 fd = 3; fd < MAX_HANDLES; fd++) {
        if (ctx->handles[fd]) {
            os_close(ctx, fd);
        }
    }
    if (ctx->guard) {
        CloseHandle(ctx->guard);  // zip's archive (os_writable)
        ctx->guard = 0;
    }
    return ctx->status;
}

// Initialize standard handles, and find the functions XP lacks.
[[maybe_unused]] static void os_init(os *ctx)
{
    load_api(&ctx->api);
    ctx->handles[0] = GetStdHandle((u32)-10);
    ctx->handles[1] = GetStdHandle((u32)-11);
    ctx->handles[2] = GetStdHandle((u32)-12);
    for (i32 fd = 0; fd < 3; fd++) {
        u32 mode;
        b32 console = GetConsoleMode(ctx->handles[fd], &mode);
        ctx->consoles |= (console ? 1u : 0u) << fd;
    }
}

// Command line arguments as WTF-8, including the program name first.
[[maybe_unused]] static s8 *os_args(arena *a, i32 *argc)
{
    *argc = 0;
    c16 **argv = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!argv) {
        *argc = 0;
    }
    s8 *args = new(a, *argc, s8);
    for (i32 i = 0; i < *argc; i++) {
        args[i] = towtf8(a, argv[i]);
    }
    return args;
}

// CRT-free Win32 platform layer for tugz gzip
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.


// A pipe whose reader has gone, which has no SIGPIPE here: writes fail
// with "the pipe is being closed", or else "the pipe has been ended".
static b32 os_pipeclosed(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_NO_DATA || err==ERROR_BROKEN_PIPE;
}

struct osmeta {
    basic_info info;
};

static osmeta *os_getmeta(os *ctx, i32 fd, arena *a)
{
    osmeta *m = new(a, 1, osmeta);
    b32 ok = get_info(ctx, ctx->handles[fd], FileBasicInfo, &m->info,
                      sizeof(m->info));
    return ok ? m : 0;
}

#define FILE_ATTRIBUTE_ARCHIVE     0x20u
#define FILE_ATTRIBUTE_NOT_INDEXED 0x2000u

// Copy the timestamps, and the attributes that describe a file as the
// mode does on POSIX: read-only, the nearest thing to a missing write
// bit, and as zip keeps a replaced archive's, hidden, system, and not
// indexed, with the archive bit set, as on any new file. Access control
// is inherited from the directory, as new files' would be from GNU gzip.
static b32 os_setmeta(os *ctx, i32 fd, osmeta *m)
{
    u32 kept = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
               FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_INDEXED;
    basic_info set = {0};
    set.accessed = m->info.accessed;
    set.written  = m->info.written;
    if (m->info.attributes & kept) {
        set.attributes = (m->info.attributes & kept) | FILE_ATTRIBUTE_ARCHIVE;
    }
    return set_info(ctx, ctx->handles[fd], FileBasicInfo, &set, sizeof(set));
}

// A match: its path packed after the last, with a terminator, in the
// arena that data is.
static b32 add_arg(void *data, s8 path, os_dirent *entry, arena scratch)
{
    (void)entry;
    (void)scratch;
    u8 *copy = newstr(data, path.len+1);
    bytecopy(copy, path.s, path.len);
    copy[path.len] = 0;
    return 1;
}

// Expand wildcards in arguments, which Windows shells leave to programs,
// as zip expands them (src/dir.c) and as a POSIX shell would: one with *
// or ? becomes the names it matches, in order, but for hidden and system
// files, as a shell leaves out dotfiles, or if none, stays as it is.
// The arguments go to perm, and directory listings to scratch. Matches
// are packed first, then the arguments made all at once, since an array
// doubled as it grew between them would leave each smaller copy behind.
static s8 *expand_args(os *ctx, arena *perm, s8 *args, i32 *nargs,
                       arena scratch)
{
    iz *matches = new(perm, *nargs, iz);
    u8 *p       = (u8 *)perm->beg;  // where they begin
    iz  total   = 0;
    for (i32 i = 0; i < *nargs; i++) {
        if (zip_haswild(args[i], 0)) {
            matches[i] = expand_wild(ctx, args[i], 0, WILD_WINDOWS, add_arg,
                                     perm, scratch);
        }
        total += matches[i] ? matches[i] : 1;
    }

    s8 *r = new(perm, total, s8);
    iz  n = 0;
    for (i32 i = 0; i < *nargs; i++) {
        if (!matches[i]) {
            r[n++] = args[i];
        }
        for (iz k = 0; k < matches[i]; k++, n++) {
            r[n].s = p;
            for (; p[r[n].len]; r[n].len++) {}
            p += r[n].len + 1;
        }
    }
    *nargs = (i32)total;
    return r;
}

void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    ctx.wildnames = 1;  // as after a shell (expand_args)
    iz    cap = (iz)1 << 25;
    byte *mem = VirtualAlloc(0, cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        os_oom(&ctx);
    }
    config conf = {0};
    conf.perm = (arena){mem, mem+cap, &ctx, 0};

    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    if (argc > 0) {
        // Drop the directory and an .exe extension
        s8 name = argv[0];
        for (iz i = name.len; i > 0; i--) {
            if (name.s[i-1]=='/' || name.s[i-1]=='\\' || name.s[i-1]==':') {
                name.s += i;
                name.len -= i;
                break;
            }
        }
        if (name.len > 4) {
            s8 ext = {name.s + name.len - 4, 4};
            name.len -= ascii_iequals(ext, S(".exe")) ? 4 : 0;
        }
        conf.name = name;
    }
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    // Listings take the top three quarters of the memory left, which
    // the expanded arguments, below them, then leave free
    arena scratch  = conf.perm;
    scratch.beg   += (scratch.end - scratch.beg) / 4;
    conf.perm.end  = scratch.beg;
    conf.args      = expand_args(&ctx, &conf.perm, conf.args, &conf.nargs,
                                 scratch);
    conf.perm.end  = scratch.end;
    os_exit(&ctx, gzip_main(&conf));
}
