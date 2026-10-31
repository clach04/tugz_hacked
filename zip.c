// tugz zip 1.3.0: an Info-ZIP compatible zip for Windows
// Single-file amalgamation of the tugz sources. Build:
//   $ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.

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

// tugz ZIP format: headers, Zip64, archive parsing, names
//
// No I/O: headers are encoded into and parsed from memory, and the zip
// program (zipcli.c) moves the bytes. Sizes and offsets within archives
// are i64, since archives may exceed the address space on 32-bit hosts.
// Reference: PKWARE APPNOTE.TXT 6.3.

#define ZIP_LOCAL_SIG    0x04034b50u
#define ZIP_CENTRAL_SIG  0x02014b50u
#define ZIP_DESC_SIG     0x08074b50u
#define ZIP_END_SIG      0x06054b50u
#define ZIP_END64_SIG    0x06064b50u
#define ZIP_LOC64_SIG    0x07064b50u
#define ZIP_MAX16        0xffff
#define ZIP_MAX32        0xffffffffll

#define ZIP_LOCAL_LEN    30
#define ZIP_CENTRAL_LEN  46
#define ZIP_END_LEN      22
#define ZIP_END64_LEN    56
#define ZIP_LOC64_LEN    20

enum {
    ZIP_STORE     = 0,
    ZIP_DEFLATE   = 8,
    ZIP_DEFLATE64 = 9,  // read by unzip, never written
};

enum {
    ZIP_FLAG_ENCRYPTED  = 1 << 0,
    ZIP_FLAG_DESCRIPTOR = 1 << 3,
    ZIP_FLAG_UTF8       = 1 << 11,
};

enum {
    ZIP_EXTRA_ZIP64     = 0x0001,
    ZIP_EXTRA_TIME      = 0x5455,  // "UT": Unix times
    ZIP_EXTRA_UPATH     = 0x7075,  // "up": Info-ZIP Unicode path
    ZIP_EXTRA_UNIX      = 0x7875,  // "ux": Unix UID and GID
    ZIP_EXTRA_UNIX1     = 0x5855,  // "UX": Info-ZIP's first, with times
    ZIP_EXTRA_UNIX2     = 0x7855,  // "Ux": Info-ZIP's second, IDs only
};

// Results of parsing an archive's end records.
enum {
    ZIP_OK,
    ZIP_ENOEND,   // no end of central directory record
    ZIP_EMULTI,   // split or spanned archive
    ZIP_EPREFIX,  // data precedes the archive (self-extractor)
    ZIP_EFORMAT,  // inconsistent structure
    ZIP_ECOUNT,   // valid, but for more entries than the directory holds
};

// An entry, one for each of an existing archive's, and so kept small: a
// byte for each flag leaves no padding (112 bytes on 64-bit hosts).
typedef struct {
    s8  name;
    s8  lextra;   // local extra fields, without Zip64
    s8  cextra;   // central extra fields, without Zip64
    s8  comment;
    i64 usize;
    i64 csize;
    i64 offset;   // of the local header
    u32 crc;
    u32 dostime;  // date<<16 | time
    u32 extattr;
    u16 made;     // version made by
    u16 needed;   // minimum version needed to extract, beyond our own
    u16 flags;
    u16 method;
    u16 intattr;
    u8  zip64;    // the local header carries a Zip64 extra
    u8  usize64;  // read: its size came from a Zip64 extra
} zentry;

typedef struct {
    i64 count;
    i64 cdsize;
    i64 cdoff;
    i64 end64;    // offset of the Zip64 end record, or -1
    i64 endpos;   // offset of the end of central directory record
    s8  comment;
    b32 cut;      // the comment runs past the end of the file, cut short
    u32 disk;     // disk numbers from the end record
    u32 cddisk;
    u32 ndisk;    // entries on this disk
} zend;

static u8 *put16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 0);
    p[1] = (u8)(v >> 8);
    return p + 2;
}

static u8 *put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
    return p + 4;
}

static u8 *put64(u8 *p, u64 v)
{
    store64le(p, v);
    return p + 8;
}

static u8 *putbytes(u8 *p, s8 s)
{
    bytecopy(p, s.s, s.len);
    return p + s.len;
}

static u32 get16(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8;
}

static u32 get32(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static u64 get64(u8 const *p)
{
    return load64le(p);
}

static u32 min32(i64 v)
{
    return v<ZIP_MAX32 ? (u32)v : (u32)ZIP_MAX32;
}

// Version needed to extract: 4.5 for Zip64, 2.0 for deflate, otherwise
// 1.0, or more if a copied entry already required it. The version is
// the low byte; a copied entry keeps whatever its writer put above it.
static u16 zip_needed(zentry const *e)
{
    b32 big = e->zip64 || e->offset>=ZIP_MAX32 || e->usize>=ZIP_MAX32 ||
              e->csize>=ZIP_MAX32;
    u16 v   = big ? 45 : e->method==ZIP_DEFLATE ? 20 : 10;
    u16 old = e->needed & 0xff;
    return (u16)(MAX(v, old) | (e->needed & 0xff00));
}

[[maybe_unused]] static iz zip_local_len(zentry const *e)
{
    return ZIP_LOCAL_LEN + e->name.len + (e->zip64 ? 20 : 0) + e->lextra.len;
}

// Encode a local header, zip_local_len bytes. A Zip64 entry records both
// sizes in its extra field, as required.
[[maybe_unused]] static u8 *zip_local(u8 *p, zentry const *e)
{
    p = put32(p, ZIP_LOCAL_SIG);
    p = put16(p, zip_needed(e));
    p = put16(p, e->flags);
    p = put16(p, e->method);
    p = put16(p, e->dostime & 0xffff);
    p = put16(p, e->dostime >> 16);
    p = put32(p, e->crc);
    p = put32(p, e->zip64 ? (u32)ZIP_MAX32 : (u32)e->csize);
    p = put32(p, e->zip64 ? (u32)ZIP_MAX32 : (u32)e->usize);
    p = put16(p, (u32)e->name.len);
    p = put16(p, (u32)(e->lextra.len + (e->zip64 ? 20 : 0)));
    p = putbytes(p, e->name);
    if (e->zip64) {
        p = put16(p, ZIP_EXTRA_ZIP64);
        p = put16(p, 16);
        p = put64(p, (u64)e->usize);
        p = put64(p, (u64)e->csize);
    }
    return putbytes(p, e->lextra);
}

// Data descriptor following an entry's data, at most 24 bytes, for
// entries that keep the descriptor flag.
[[maybe_unused]] static u8 *zip_desc(u8 *p, zentry const *e)
{
    p = put32(p, ZIP_DESC_SIG);
    p = put32(p, e->crc);
    if (e->zip64) {
        p = put64(p, (u64)e->csize);
        return put64(p, (u64)e->usize);
    }
    p = put32(p, (u32)e->csize);
    return put32(p, (u32)e->usize);
}

// A central Zip64 extra, when anything overflows, holds both sizes, with
// their 32-bit fields saturated, and then the offset if it overflows.
// Holding only the fields that overflow, as APPNOTE also allows, trips
// UnZip 6.0: once it has read a size of exactly 0xffffffff from one, it
// expects every later Zip64 extra to begin with that size, and so reads
// an entry's offset as its size.
static iz zip_central64_len(zentry const *e)
{
    b32 off = e->offset >= ZIP_MAX32;
    b32 any = off || e->usize>=ZIP_MAX32 || e->csize>=ZIP_MAX32;
    return any ? 4 + 16 + (off ? 8 : 0) : 0;
}

[[maybe_unused]] static iz zip_central_len(zentry const *e)
{
    return ZIP_CENTRAL_LEN + e->name.len + zip_central64_len(e) +
           e->cextra.len + e->comment.len;
}

// Encode a central directory header, zip_central_len bytes.
[[maybe_unused]] static u8 *zip_central(u8 *p, zentry const *e)
{
    iz z64 = zip_central64_len(e);
    p = put32(p, ZIP_CENTRAL_SIG);
    p = put16(p, e->made);
    p = put16(p, zip_needed(e));
    p = put16(p, e->flags);
    p = put16(p, e->method);
    p = put16(p, e->dostime & 0xffff);
    p = put16(p, e->dostime >> 16);
    p = put32(p, e->crc);
    p = put32(p, z64 ? (u32)ZIP_MAX32 : (u32)e->csize);
    p = put32(p, z64 ? (u32)ZIP_MAX32 : (u32)e->usize);
    p = put16(p, (u32)e->name.len);
    p = put16(p, (u32)(z64 + e->cextra.len));
    p = put16(p, (u32)e->comment.len);
    p = put16(p, 0);  // disk number start
    p = put16(p, e->intattr);
    p = put32(p, e->extattr);
    p = put32(p, min32(e->offset));
    p = putbytes(p, e->name);
    if (z64) {
        p = put16(p, ZIP_EXTRA_ZIP64);
        p = put16(p, (u32)(z64 - 4));
        p = put64(p, (u64)e->usize);
        p = put64(p, (u64)e->csize);
        if (e->offset >= ZIP_MAX32) {
            p = put64(p, (u64)e->offset);
        }
    }
    p = putbytes(p, e->cextra);
    return putbytes(p, e->comment);
}

// Whether an entry's name, extra fields (with Zip64 fields as they would
// be written), and comment fit their 16-bit lengths in both headers.
[[maybe_unused]] static b32 zip_fits(zentry const *e)
{
    return e->name.len<=ZIP_MAX16 && e->comment.len<=ZIP_MAX16 &&
           e->lextra.len+(e->zip64 ? 20 : 0)<=ZIP_MAX16 &&
           e->cextra.len+zip_central64_len(e)<=ZIP_MAX16;
}

static b32 zip_end_needs64(i64 count, i64 cdsize, i64 cdoff)
{
    return count>=ZIP_MAX16 || cdsize>=ZIP_MAX32 || cdoff>=ZIP_MAX32;
}

[[maybe_unused]]
static iz zip_end_len(i64 count, i64 cdsize, i64 cdoff, s8 comment)
{
    b32 z64 = zip_end_needs64(count, cdsize, cdoff);
    return (z64 ? ZIP_END64_LEN+ZIP_LOC64_LEN : 0) + ZIP_END_LEN + comment.len;
}

// Encode the end records for a central directory of count entries and
// cdsize bytes at cdoff: Zip64 records when anything overflows, then the
// end of central directory record. The Zip64 record gives the program's
// version made by, as its entries do.
[[maybe_unused]]
static u8 *zip_end(u8 *p, i64 count, i64 cdsize, i64 cdoff, s8 comment,
                   u16 made)
{
    if (zip_end_needs64(count, cdsize, cdoff)) {
        p = put32(p, ZIP_END64_SIG);
        p = put64(p, ZIP_END64_LEN - 12);
        p = put16(p, made);
        p = put16(p, 45);  // version needed
        p = put32(p, 0);   // this disk
        p = put32(p, 0);   // disk with the central directory
        p = put64(p, (u64)count);
        p = put64(p, (u64)count);
        p = put64(p, (u64)cdsize);
        p = put64(p, (u64)cdoff);

        p = put32(p, ZIP_LOC64_SIG);
        p = put32(p, 0);   // disk with the Zip64 end record
        p = put64(p, (u64)(cdoff + cdsize));
        p = put32(p, 1);   // total disks
    }
    u32 n16 = count<ZIP_MAX16 ? (u32)count : ZIP_MAX16;
    p = put32(p, ZIP_END_SIG);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, n16);
    p = put16(p, n16);
    p = put32(p, min32(cdsize));
    p = put32(p, min32(cdoff));
    p = put16(p, (u32)comment.len);
    return putbytes(p, comment);
}

// Whether the end record's fields defer to a Zip64 end record.
static b32 zip_end_saturated(zend const *e)
{
    return e->count==ZIP_MAX16 || e->ndisk==ZIP_MAX16 ||
           e->disk==ZIP_MAX16 || e->cddisk==ZIP_MAX16 ||
           e->cdsize==ZIP_MAX32 || e->cdoff==ZIP_MAX32;
}

// Whether, by the end record, the central directory ends at it, so that
// there is no room for Zip64 records between, and bytes resembling them
// are by chance (the end of the last entry's comment).
static b32 zip_end_reached(zend const *e)
{
    return e->cdoff+e->cdsize == e->endpos;
}

// Validate the end of central directory record on its own. As Info-ZIP
// does, a split archive is known by its disk numbers, not by a count of
// entries on this disk that differs from the total, which, on a single
// disk, some writers get wrong (where Info-ZIP and UnZip read it). A
// count of more entries than the directory could hold is ZIP_ECOUNT,
// which zip refuses, and unzip reads past, as UnZip reads headers until
// they stop (zar_check).
static i32 zip_check_end32(zend *e)
{
    e->end64 = -1;
    if (e->disk || e->cddisk) {
        return ZIP_EMULTI;
    } else if (!zip_end_reached(e)) {
        return e->cdoff+e->cdsize<e->endpos ? ZIP_EPREFIX : ZIP_EFORMAT;
    }
    return e->count<=e->cdsize/ZIP_CENTRAL_LEN ? ZIP_OK : ZIP_ECOUNT;
}

// Find the end of central directory record among the final n bytes of an
// archive of the given total size. Following it, if e->end64 is not -1,
// read ZIP_END64_LEN bytes there and pass them to zip_parse_end64.
//
// Bytes resembling a Zip64 locator may precede the end record by chance,
// such as in an entry comment, so a Zip64 record is relied upon only if
// it checks out or the end record's fields call for one, or leave room
// for one: then its own faults are reported, not the end record's.
//
// A signature whose comment runs past the end is stray, perhaps within
// the comment, unless no other fits: then the last, nearest the end, is
// taken, as UnZip takes it, its comment cut short (e->cut), which zip
// refuses and unzip warns of.
static i32 zip_find_end(u8 *tail, iz n, i64 size, zend *e)
{
    iz i = -1;
    for (iz j = n-ZIP_END_LEN; j >= 0; j--) {
        if (get32(tail+j) == ZIP_END_SIG) {
            i = i<0 ? j : i;
            if ((iz)get16(tail+j+20) <= n-j-ZIP_END_LEN) {
                i = j;
                break;
            }
        }
    }
    if (i < 0) {
        return ZIP_ENOEND;
    }

    u8 *p    = tail + i;
    iz  clen = get16(p+20);
    e->cut     = clen > n-i-ZIP_END_LEN;
    e->endpos  = size - n + i;
    e->comment = (s8){p+ZIP_END_LEN, MIN(clen, n-i-ZIP_END_LEN)};
    e->disk    = get16(p+4);
    e->cddisk  = get16(p+6);
    e->ndisk   = get16(p+8);
    e->count   = get16(p+10);
    e->cdsize  = get32(p+12);
    e->cdoff   = get32(p+16);
    e->end64   = -1;
    if (!e->count && !e->cdsize && !e->cdoff) {
        // An empty central directory at offset 0, as Info-ZIP writes one
        // wherever it is, as after an emptied self-extractor's stub, and
        // UnZip takes for an empty archive: it is here, and whatever
        // precedes it is a preamble
        e->cdoff = e->endpos;
    }

    if (i>=ZIP_LOC64_LEN && get32(p-ZIP_LOC64_LEN)==ZIP_LOC64_SIG) {
        // On another disk, or one of more than one disk: split. A total
        // of zero disks, as Microsoft's writers (Windows Explorer, .NET)
        // put there, is taken for one, as Debian's UnZip (6.0-29) does,
        // rather than for a split archive
        u8 *loc   = p - ZIP_LOC64_LEN;
        u64 off   = get64(loc+8);
        b32 split = get32(loc+4) || get32(loc+16)>1;
        b32 ok    = !split && e->endpos>=ZIP_LOC64_LEN+ZIP_END64_LEN &&
                    off<=(u64)(e->endpos-ZIP_LOC64_LEN-ZIP_END64_LEN);
        if (ok) {
            e->end64 = (i64)off;
            return ZIP_OK;
        } else if (zip_end_saturated(e) || !zip_end_reached(e)) {
            return split ? ZIP_EMULTI : ZIP_EFORMAT;
        }
    }
    return zip_check_end32(e);
}

static i32 zip_check_end64(u8 const *p, zend *e)
{
    if (get32(p) != ZIP_END64_SIG) {
        return ZIP_EFORMAT;
    }
    u64 recsize = get64(p+4);
    u64 total   = get64(p+32);
    u64 cdsize  = get64(p+40);
    u64 cdoff   = get64(p+48);
    if (get32(p+20)) {
        // As in Info-ZIP, only the disk with the central directory tells,
        // not this disk's number or its count of entries
        return ZIP_EMULTI;
    }

    // The record must sit just before the locator, by its size, and the
    // central directory just before it. Data prepended without adjusting
    // offsets would show here only if the locator had been adjusted: if
    // not, the locator points short of the record, at no record.
    u64 limit = (u64)e->endpos;
    if (recsize<ZIP_END64_LEN-12 || recsize>limit ||
        e->endpos-ZIP_LOC64_LEN-12-(i64)recsize != e->end64) {
        return ZIP_EFORMAT;
    }
    if (cdsize>limit || cdoff>limit || cdoff+cdsize!=(u64)e->end64) {
        b32 prefix = cdoff+cdsize < (u64)e->end64;
        if (cdsize<=limit && cdoff<=limit) {
            // Kept, as zip_check_end32 keeps them, so that a reader may
            // shift the offsets by the data before the archive, as UnZip
            // does, so that the central directory ends at the record, or
            // tell how far short of it the directory ends
            e->count  = (i64)total;
            e->cdsize = (i64)cdsize;
            e->cdoff  = (i64)cdoff;
        }
        return prefix ? ZIP_EPREFIX : ZIP_EFORMAT;
    }
    e->count  = (i64)total;
    e->cdsize = (i64)cdsize;
    e->cdoff  = (i64)cdoff;
    return total<=cdsize/ZIP_CENTRAL_LEN ? ZIP_OK : ZIP_ECOUNT;
}

// Parse the Zip64 end record, ZIP_END64_LEN bytes at e->end64, falling
// back to the end record alone when that suffices: when its fields do
// not defer to Zip64 records, and leave no room for them.
static i32 zip_parse_end64(u8 const *p, zend *e)
{
    zend end = *e;  // as the end record has it
    i32  r   = zip_check_end64(p, e);
    b32  own = r==ZIP_OK || zip_end_saturated(&end) || !zip_end_reached(&end);
    if (!own) {
        *e = end;
        r  = zip_check_end32(e);
    }
    return r;
}

// Copy a kept entry's extra fields, dropping only Zip64, which is
// regenerated as needed. Like Info-ZIP, -X does not apply to kept
// entries, some of which need their fields, such as AES encryption's
// (0x9901). Malformed trailing data is dropped.
static s8 zip_filter_extra(arena *a, s8 x)
{
    s8 r = {newstr(a, x.len), 0};
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id != ZIP_EXTRA_ZIP64) {
            bytecopy(r.s+r.len, x.s+i, 4+len);
            r.len += 4 + len;
        }
        i += 4 + len;
    }
    return r;
}

// Drop the extra fields of one id from x, in place, as a renamed entry
// drops the Unicode path field of its old name. Malformed trailing data
// is dropped too.
[[maybe_unused]] static s8 zip_drop_extra(s8 x, u32 drop)
{
    iz n = 0;
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id != drop) {
            bytemove(x.s+n, x.s+i, 4+len);
            n += 4 + len;
        }
        i += 4 + len;
    }
    x.len = n;
    return x;
}

// The modification time among extra fields as Info-ZIP's -u and -f
// compare it (its ef_scan_ut_time), unsigned as it reads it: that of the
// last extended timestamp ("UT") field, if it has one, else of an old
// Unix ("UX") field, unless after a newer Unix ("Ux") field, which has
// none, or a UT field. Returns false if there is none.
[[maybe_unused]] static b32 zip_extra_mtime(s8 x, i64 *t)
{
    b32 have  = 0;
    b32 newer = 0;  // a UT or Ux field, over any UX field
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        u8 *d   = x.s + i + 4;
        if (len > x.len-i-4) {
            break;
        }
        if (id == ZIP_EXTRA_TIME) {
            newer = 1;
            have  = len>=5 && (d[0] & 1);
            *t    = have ? get32(d+1) : *t;
        } else if (id==ZIP_EXTRA_UNIX2 && !newer) {
            newer = 1;
            have  = 0;
        } else if (id==ZIP_EXTRA_UNIX1 && !newer && len>=8) {
            have = 1;
            *t   = get32(d+4);  // after the access time
        }
        i += 4 + len;
    }
    return have;
}

// The data of the first extra field with an ID, else a null string.
static s8 zip_extra_field(s8 x, u32 id)
{
    for (iz i = 0; x.len-i >= 4;) {
        iz len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        } else if (get16(x.s+i) == id) {
            return (s8){x.s+i+4, len};
        }
        i += 4 + len;
    }
    return (s8){0};
}

// The UTF-8 name in an Info-ZIP Unicode path field among extra fields,
// if, as Info-ZIP and UnZip check, its version is at most 1 and it gives
// crc as the stored name's CRC-32, else a null string. An empty name
// means the stored one is UTF-8.
static s8 zip_extra_upath(s8 x, u32 crc)
{
    s8  f  = zip_extra_field(x, ZIP_EXTRA_UPATH);
    b32 ok = f.len>=5 && f.s[0]<=1 && get32(f.s+1)==crc;
    return ok ? (s8){f.s+5, f.len-5} : (s8){0};
}

// Whether extra fields hold a Unicode path field that Info-ZIP reads but
// finds stale, the stored name having changed since it was made: of a
// version at most 1, without crc, the stored name's CRC-32, or too short
// to hold one.
static b32 zip_extra_upath_stale(s8 x, u32 crc)
{
    s8 f = zip_extra_field(x, ZIP_EXTRA_UPATH);
    return f.s && (!f.len || f.s[0]<=1) && (f.len<5 || get32(f.s+1)!=crc);
}

// Whether a name, not flagged UTF-8, is in an OEM (IBM PC) code page,
// as Info-ZIP's Windows port judges by the host that made it: MS-DOS,
// except PKZIP 2.5, 2.6, and 4.0 for Windows (known by attributes beyond
// DOS's), which use the ANSI code page, OS/2, and WinZip's NTFS 5.0.
static b32 zip_oem_name(zentry const *e)
{
    u32 host  = e->made >> 8;
    u32 ver   = e->made & 0xff;
    b32 pkwin = (e->extattr>>16) && (ver==25 || ver==26 || ver==40);
    return !(e->flags & ZIP_FLAG_UTF8) &&
           ((host==0 && !pkwin) || host==6 || (host==11 && ver==50));
}

// Apply a central header's Zip64 extra field to the entry. Returns false
// if a needed field is missing. Without that extra, saturated sizes and
// offset are literal, as Info-ZIP, which uses Zip64 only past 0xffffffff,
// writes a file of exactly 4 GiB - 1 bytes, and as UnZip, Python, and
// Info-ZIP read it; the bounds checks that follow catch a bogus offset.
// So are saturated sizes beside an extra that holds just a saturated
// offset, as Info-ZIP writes such a file past 4 GiB: APPNOTE would have
// the sizes first, and UnZip and Info-ZIP itself take the offset for
// the uncompressed size, while Python refuses it. A saturated disk
// number still needs the extra.
static b32 zip_apply64(zentry *e, s8 x, b32 diskmax)
{
    b32 want[3] = {e->usize==ZIP_MAX32, e->csize==ZIP_MAX32,
                   e->offset==ZIP_MAX32};
    if (!want[0] && !want[1] && !want[2] && !diskmax) {
        return 1;
    }
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id == ZIP_EXTRA_ZIP64) {
            if (want[2] && len==8 && !diskmax) {
                want[0] = want[1] = 0;  // only the offset: Info-ZIP's
            }
            e->usize64 = (u8)want[0];
            u8 *f = x.s + i + 4;
            i64 *fields[3] = {&e->usize, &e->csize, &e->offset};
            for (i32 k = 0; k < 3; k++) {
                if (!want[k]) {
                    continue;
                }
                if (len < 8) {
                    return 0;
                }
                u64 v = get64(f);
                if (v >> 63) {
                    return 0;
                }
                *fields[k] = (i64)v;
                f += 8;
                len -= 8;
            }
            return !diskmax || (len>=4 && !get32(f));
        }
        i += 4 + len;
    }
    return !diskmax;
}

// Length of a central header's name, extra fields, and comment, or -1 if
// the fixed part, ZIP_CENTRAL_LEN bytes, is not a central header.
static iz zip_central_varlen(u8 const *h)
{
    if (get32(h) != ZIP_CENTRAL_SIG) {
        return -1;
    }
    return (iz)get16(h+28) + (iz)get16(h+30) + (iz)get16(h+32);
}

// Parse the central header beginning the n bytes at h, for an entry whose
// local header lies before cdoff, and given fit, its data too. Its name,
// extra fields (with Zip64, unfiltered), and comment point into h.
// Returns its length, or 0 if malformed. (Unzip lists an entry of any
// compressed size, as UnZip does, but bounds its data before reading.)
static iz zip_parse_header(u8 *h, iz n, i64 cdoff, b32 fit, zentry *e)
{
    iz var = n<ZIP_CENTRAL_LEN ? -1 : zip_central_varlen(h);
    if (var<0 || var>n-ZIP_CENTRAL_LEN) {
        return 0;
    }
    iz nlen = get16(h+28);
    iz xlen = get16(h+30);
    iz clen = get16(h+32);

    *e = (zentry){0};
    e->made    = (u16)get16(h+4);
    e->needed  = (u16)get16(h+6);
    e->flags   = (u16)get16(h+8);
    e->method  = (u16)get16(h+10);
    e->dostime = get16(h+14)<<16 | get16(h+12);
    e->crc     = get32(h+16);
    e->csize   = get32(h+20);
    e->usize   = get32(h+24);
    e->intattr = (u16)get16(h+36);
    e->extattr = get32(h+38);
    e->offset  = get32(h+42);
    e->name    = (s8){h+ZIP_CENTRAL_LEN, nlen};
    e->cextra  = (s8){h+ZIP_CENTRAL_LEN+nlen, xlen};
    e->comment = (s8){h+ZIP_CENTRAL_LEN+nlen+xlen, clen};

    u32 disk = get16(h+34);
    if ((disk && disk!=ZIP_MAX16) ||
        !zip_apply64(e, e->cextra, disk==ZIP_MAX16)) {
        return 0;
    }
    if (e->offset>cdoff-ZIP_LOCAL_LEN || (fit && e->csize>cdoff-e->offset)) {
        return 0;
    }
    return ZIP_CENTRAL_LEN + var;
}

// Parse a central directory of n bytes, expected to hold count entries
// whose data lies before cdoff. Names and comments point into p, and
// extra fields, without Zip64, are copied into a. Returns null if
// malformed. (The zip program reads a header at a time instead.)
[[maybe_unused]]
static zentry *zip_parse_central(u8 *p, iz n, i64 count, i64 cdoff,
                                 arena *a)
{
    if (count > n/ZIP_CENTRAL_LEN) {
        return 0;
    }
    zentry *entries = new(a, (iz)count, zentry);
    iz off = 0;
    for (i64 i = 0; i < count; i++) {
        zentry *e   = entries + i;
        iz      len = zip_parse_header(p+off, n-off, cdoff, 1, e);
        if (!len) {
            return 0;
        }
        e->cextra = zip_filter_extra(a, e->cextra);
        off += len;
    }
    return off==n ? entries : 0;
}

// Length of a local header's name and extra fields, or -1 if the fixed
// part, ZIP_LOCAL_LEN bytes, is not a local header.
static iz zip_local_varlen(u8 const *h)
{
    if (get32(h) != ZIP_LOCAL_SIG) {
        return -1;
    }
    return (iz)get16(h+26) + (iz)get16(h+28);
}

// Pack a broken-down time {year, month 1-12, day, hour, minute, second}
// as an MS-DOS date<<16 | time, clamped to its range, 1980 to 2107.
[[maybe_unused]] static u32 zip_dostime(i32 const t[6])
{
    if (t[0] < 1980) {
        return (0<<9 | 1<<5 | 1) << 16;
    } else if (t[0] > 2107) {
        return (127u<<9 | 12<<5 | 31) << 16 | (23<<11 | 59<<5 | 29);
    }
    u32 date = (u32)(t[0]-1980)<<9 | (u32)t[1]<<5 | (u32)t[2];
    u32 time = (u32)t[3]<<11 | (u32)t[4]<<5 | (u32)t[5]>>1;
    return date<<16 | time;
}

// Broken-down UTC time from Unix seconds (proleptic Gregorian calendar).
// Years beyond the range of i32 saturate, so zip_dostime clamps them.
static void zip_gmtime(i64 t, i32 tm[6])
{
    i64 days = t / 86400;
    i64 secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    // Howard Hinnant's civil_from_days
    days += 719468;
    i64 era = (days >= 0 ? days : days-146096) / 146097;
    i64 doe = days - era*146097;
    i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
    i64 mp  = (5*doy + 2) / 153;
    i64 d   = doy - (153*mp + 2)/5 + 1;
    i64 m   = mp<10 ? mp+3 : mp-9;
    i64 y   = yoe + era*400 + (m <= 2);
    tm[0] = (i32)MAX(MIN(y, 0x7fffffff), -0x7fffffff-1);
    tm[1] = (i32)m;
    tm[2] = (i32)d;
    tm[3] = (i32)(secs / 3600);
    tm[4] = (i32)(secs / 60 % 60);
    tm[5] = (i32)(secs % 60);
}

// 0 for ASCII, 1 for valid UTF-8 with non-ASCII, or -1 if not UTF-8
// (including WTF-8 surrogates).
static i32 zip_utf8(s8 s)
{
    i32 r = 0;
    for (iz i = 0; i < s.len;) {
        u32 c = s.s[i++];
        if (c < 0x80) {
            continue;
        }
        r = 1;
        i32 n   = c>=0xc2 && c<0xe0 ? 1 : c>=0xe0 && c<0xf0 ? 2 :
                  c>=0xf0 && c<0xf5 ? 3 : 0;
        u32 min = n==3 ? 0x10000 : n==2 ? 0x800 : 0x80;
        if (!n || n>s.len-i) {
            return -1;
        }
        c &= 0x3f >> n;
        for (i32 k = 0; k < n; k++, i++) {
            if ((s.s[i] & 0xc0) != 0x80) {
                return -1;
            }
            c = c<<6 | (s.s[i] & 0x3f);
        }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) {
            return -1;
        }
    }
    return r;
}

static b32 zip_sep(u8 c, b32 windows)
{
    return c=='/' || (windows && c=='\\');
}

// Length of a UNC prefix "//server/share/" to drop from a name, as
// Info-ZIP does on Unix too, or zero. Without its final separator,
// "//server/share" is kept, though a directory, such as a share root,
// is named with one. On Windows either separator counts, a
// device path "//?/X:/" counts as server "?" and share "X:", and
// "//?/UNC/server/share/" as a whole.
static iz zip_unc(s8 p, b32 windows)
{
    if (p.len<3 || !zip_sep(p.s[0], windows) ||
        !zip_sep(p.s[1], windows) || zip_sep(p.s[2], windows)) {
        return 0;
    }
    iz drop = 0;
    iz i    = 2;
    for (i32 k = 0, parts = 2; k < parts; k++) {
        iz beg = i;
        for (; i<p.len && !zip_sep(p.s[i], windows); i++) {}
        if (i++ == p.len) {
            break;
        }
        b32 dev = windows && k==1 && beg==4 &&
                  (p.s[2]=='?' || p.s[2]=='.');
        if (dev && i-beg==4 && (p.s[beg]|32)=='u' &&
            (p.s[beg+1]|32)=='n' && (p.s[beg+2]|32)=='c') {
            parts = 4;
        }
        drop = k&1 ? i : drop;
    }
    return drop;
}

// Archive name for a path: backslashes become slashes on Windows; a
// drive (Windows) or UNC prefix, leading slashes, and leading ./
// components are dropped; and doubled slashes collapse. Like Info-ZIP,
// ../ components are kept.
[[maybe_unused]] static s8 zip_name(arena *a, s8 path, b32 windows)
{
    s8 r = {newstr(a, path.len), 0};
    iz i = 0;
    u8 drive = path.len>=2 && path.s[1]==':' ? (u8)(path.s[0] | 0x20) : 0;
    if (windows && drive>='a' && drive<='z') {
        i = 2;  // only a letter is a drive: "1:x" is a stream of file "1"
    } else {
        i = zip_unc(path, windows);
    }
    for (b32 lead = 1; i < path.len; i++) {
        u8 c = path.s[i];
        c = windows && c=='\\' ? '/' : c;
        if (c=='/' && (lead || (r.len && r.s[r.len-1]=='/'))) {
            continue;
        }
        if (lead && c=='.') {
            u8 n = i+1<path.len ? path.s[i+1] : '/';
            n = windows && n=='\\' ? '/' : n;
            if (n == '/') {
                i++;  // drop "./"
                continue;
            }
        }
        lead = 0;
        r.s[r.len++] = c;
    }
    return r;
}

// Info-ZIP's compression percentage, rounded, as its builds with 64-bit
// offsets compute it, exactly (its 256-divisor reduction for large sizes
// is disabled), which no real file size can overflow.
[[maybe_unused]] static i32 zip_percent(i64 n, i64 m)
{
    return n>m ? (i32)((1 + 200*(n - m)/n) / 2) : 0;
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

// tugz archive reading, shared by the zip and unzip programs
//
// An existing archive's central directory and entries, read through a
// read-ahead window, with the string, environment, and path helpers that
// both programs use. Messages and exit statuses are each program's own.

// Platform interface shared by zip and unzip, beyond src/io.c and
// src/dir.c (platform/zipfs_*.c)

// Returns false if the path does not exist or cannot be examined, which
// os_missing then tells apart. With follow, symbolic links are followed;
// otherwise a link (on Windows, or a junction) is FT_LINK.
static b32  os_stat(os *, s8 path, b32 follow, os_info *, arena scratch);
// Whether the last os_stat failed because nothing is there (no such
// file, nor a directory on the way to it), rather than for a reason,
// such as an I/O error, that leaves what is there unknown.
static b32  os_missing(os *);
// As os_stat, for the file that an open descriptor reads.
static b32  os_fstat(os *, i32 fd, os_info *);
// A positioned read of exactly len bytes, which may not be mixed with
// os_read on the same descriptor. Returns 1, or 0 if the file ends
// first, which is no system error, or -1.
static i32  os_readat(os *, i32 fd, u8 *buf, iz len, i64 off);
// Broken-down local time {year, month, day, hour, minute, second}.
static void os_localtime(os *, i64 t, i32 tm[6]);
// A name in the OEM code page, or else the ANSI one, as UTF-8 (Windows),
// or a null string if that code page cannot decode it.
static s8   os_fromcp(os *, s8 name, b32 oem, arena *perm, arena scratch);

typedef struct {
    s8 *data;
    iz  len;
    iz  cap;
} s8s;

static s8 zjoin(arena *a, s8 const *parts, iz n)
{
    iz len = 0;
    for (iz i = 0; i < n; i++) {
        len += parts[i].len;
    }
    s8 r = {newstr(a, len), 0};
    for (iz i = 0; i < n; i++) {
        bytecopy(r.s+r.len, parts[i].s, parts[i].len);
        r.len += parts[i].len;
    }
    return r;
}
#define JOIN(a, ...) \
    zjoin(a, (s8[]){__VA_ARGS__}, countof(((s8[]){__VA_ARGS__})))

static s8 znum(arena *a, i64 v)
{
    u8  tmp[24];
    u8 *e = tmp + countof(tmp);
    u8 *p = e;
    u64 u = v<0 ? -(u64)v : (u64)v;
    do {
        *--p = (u8)('0' + u%10);
    } while (u /= 10);
    if (v < 0) {
        *--p = '-';
    }
    s8 r = {newstr(a, e-p), e-p};
    bytecopy(r.s, p, r.len);
    return r;
}

static b32 zequals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// Whether two strings are equal, ignoring ASCII case given ZIP_FOLD.
[[maybe_unused]] static b32 zfoldeq(s8 a, s8 b, i32 fold)
{
    if (!(fold & ZIP_FOLD)) {
        return zequals(a, b);
    }
    b32 eq = a.len == b.len;
    for (iz i = 0; eq && i < a.len; i++) {
        eq = zip_fold(a.s[i], fold) == zip_fold(b.s[i], fold);
    }
    return eq;
}

static b32 zisspace(u8 c)
{
    return c==' ' || (c>='\t' && c<='\r');
}

// Read an entire descriptor into memory, or return a null string.
static s8 read_all(os *ctx, i32 fd, arena *perm)
{
    s8 r  = {0};
    iz cap = 0;
    for (;;) {
        if (r.len==cap && at_tip(perm, r.s, cap)) {
            newstr(perm, cap);  // grow in place
            cap *= 2;
        } else if (r.len == cap) {
            iz ncap = cap ? 2*cap : 1<<12;
            u8 *buf = newstr(perm, ncap);
            bytecopy(buf, r.s, r.len);
            r.s = buf;
            cap = ncap;
        }
        iz n = os_read(ctx, fd, r.s+r.len, cap-r.len);
        if (n < 0) {
            return (s8){0};
        } else if (!n) {
            r.s = r.s ? r.s : (u8 *)"";
            return r;
        }
        r.len += n;
    }
}

// Split a ZIPOPT or ZIP value into arguments as Info-ZIP's envargs does:
// at whitespace, except that a word starting with a double quote runs to
// the next one, and both are dropped. On POSIX, a backslash within it is
// dropped too, and the byte after it kept, even a quote or a backslash.
// Info-ZIP's Windows port keeps backslashes, its path separators.
static s8s env_args(arena *perm, s8 env, b32 windows)
{
    s8s r   = {0};
    u8 *buf = newstr(perm, env.len);  // for quoted words
    for (iz i = 0;;) {
        for (; i<env.len && zisspace(env.s[i]); i++) {}
        if (i == env.len) {
            return r;
        }
        s8 *arg = push(perm, &r);
        if (env.s[i] == '"') {
            *arg = (s8){buf, 0};
            for (i++; i<env.len && env.s[i]!='"'; i++) {
                if (!windows && env.s[i]=='\\' && ++i==env.len) {
                    break;  // escaping nothing
                }
                arg->s[arg->len++] = env.s[i];
            }
            buf += arg->len;
            i += i < env.len;
        } else {
            iz beg = i;
            for (; i<env.len && !zisspace(env.s[i]); i++) {}
            *arg = (s8){env.s+beg, i-beg};
        }
    }
}

// Whether the directories along an entry's relative path are just that,
// none a link (on Windows, nor a junction), through which its name would
// reach beyond the current directory; a link at its end is the caller's
// (zip's scan_entries). Those of the path checked before, through its
// last slash, are known to be directories, so that a run of entries in
// one directory examines it once, as are those ending before from, the
// length of a prefix the caller trusts (zip trusts none: 0).
[[maybe_unused]]
static b32 linkless(os *ctx, s8 path, iz from, s8 *checked, arena scratch)
{
    for (iz k = 0; k<path.len && k<checked->len; k++) {
        if (path.s[k] != checked->s[k]) {
            break;
        }
        from = path.s[k]=='/' ? MAX(k+1, from) : from;
    }
    iz last = 0;
    for (iz k = 0; k < path.len; k++) {
        if (path.s[k] != '/') {
            continue;
        }
        os_info info = {0};
        if (k>=from && (!os_stat(ctx, (s8){path.s, k}, 0, &info, scratch) ||
                        info.type!=FT_DIR)) {
            return 0;
        }
        last = k + 1;
    }
    *checked = last>from ? (s8){path.s, last} : *checked;
    return 1;
}

// The existing archive

// Its entries are read through a window, so that one read serves the
// headers and data of many small entries, rather than a read for each.
// An archive already in memory (unzip's spooled standard input) is
// instead one window that never moves (zin_memory).
typedef struct {
    os  *ctx;
    i32  fd;
    i64  limit;  // of read-ahead: the file's size, then where entries end
    u8  *buf;
    iz   cap;
    iz   len;    // bytes in the window
    iz   ahead;  // how far the next fill reads
    i64  pos;    // file offset of buf[0]
    b32  mem;    // the whole archive is in buf
} zin;

enum { ZIN_CAP = 1<<20, ZIN_PAGE = 1<<12 };

// Fill the window with the need bytes at off, at most its size, and as
// many more as the read-ahead takes, within its limit: the file as
// examined, and once the central directory is read, the entries. That
// doubles, from a page up to the window's size, with each fill that
// carries on from the last, as when entries are copied in file order,
// and starts over at a jump elsewhere, so that each entry out of order
// costs a page rather than a megabyte. Returns as os_readat: 1, or 0 if
// the file ends before the need bytes, or -1. If the read-ahead fails, as
// when the file has shrunk, the need bytes are read alone, so that it
// fails only where reading just those would.
static i32 zin_fill(zin *r, i64 off, iz need)
{
    b32 onward = off>=r->pos && off-r->pos<=r->len+r->ahead;
    r->ahead = onward ? MIN(2*r->ahead, r->cap) : MIN(ZIN_PAGE, r->cap);
    i64 left = MAX(r->limit-off, 0);
    iz  n    = MAX(need, (iz)MIN(r->ahead, left));
    i32 got  = os_readat(r->ctx, r->fd, r->buf, n, off);
    if (got<=0 && n>need) {
        n   = need;
        got = os_readat(r->ctx, r->fd, r->buf, n, off);
    }
    r->pos = off;
    r->len = got>0 ? n : 0;
    return got;
}

// Read from an archive held in memory, rather than from a descriptor.
[[maybe_unused]] static void zin_memory(zin *r, s8 data)
{
    r->fd    = -1;
    r->buf   = data.s;
    r->len   = data.len;
    r->cap   = MAX(data.len, ZIN_CAP);
    r->limit = data.len;
    r->mem   = 1;
}

// Point *p at the n bytes at off, n at most the window's size, valid
// until its next use. Returns as os_readat.
static i32 zin_get(zin *r, i64 off, iz n, u8 **p)
{
    if (r->mem) {
        // The memory ends where a file would: early, as for a shrunk one
        b32 in = off>=0 && off<=r->len && n<=r->len-off;
        *p = r->buf + (in ? off : 0);
        return in;
    }
    assert(n <= r->cap);
    b32 hit = off>=r->pos && off-r->pos<=r->len-n;
    i32 got = hit ? 1 : zin_fill(r, off, n);
    *p = r->buf + (off - r->pos);
    return got;
}

// Entries' Unicode names, where those differ, by index: few entries have
// them, so these are listed in order rather than given every entry a slot.
typedef struct {
    iz index;
    s8 name;
} zuname;

typedef struct {
    zuname *data;
    iz      len;
    iz      cap;
} zunames;

typedef struct {
    zin     in;
    i64     beg;     // of the first entry, after any preamble
    zend    end;
    zentry *entries;
    zunames unames;
    i64     noname;  // the entry that ZAR_ENONAME refused
    i64     bad;     // the header that ZAR_EFORMAT refused, or -1
    i64     found;   // headers in a directory not of its count, or -1
    i64     shift;   // added to entries' offsets (unzip's extra bytes)
    i64     cdlim;   // entries' local headers and data lie before this,
                     // as offsets read, but for unzip, ZAR_MAXOFF
    iz      maxhdr;  // the longest central header read
    zin     cd;      // the central directory's own window (zar_reread)
    i64    *spans;   // where entries to be read begin, ascending
    iz      nspans;  // (zar_spans), or null: all may reach to cdoff

    // Reading as UnZip does, for unzip (zar_open, zar_check)
    b32     unzip;   // set by the caller: read as UnZip reads
    b32     moved64; // the Zip64 end record was found just before its
                     // locator, not where that says (prepended data)
    b32     nosig64; // no Zip64 end record there either (ZAR_EFORMAT)
    i64     nread;   // central headers read, past the end record's count
    b32     endsig;  // an end signature follows the last header read
    i64     size;    // of the archive, as examined
    b32     wrapped; // offsets wrapped past 4 GiB (zar_unwrap): set by
                     // the caller, kept by zar_check if they add up
    i64     floor;   // given wrapped, where the next entry read begins,
                     // at least (zar_next)
} zarchive;

// Results of reading an archive (zar_open, zar_entries, zar_local)
enum {
    ZAR_OK,
    ZAR_EREAD,    // a read failed, as os_readat's -1
    ZAR_EEOF,     // the file ended early, as os_readat's 0: it has shrunk
    ZAR_ENOEND,   // no end of central directory record
    ZAR_EFORMAT,  // zip file structure invalid
    ZAR_EMULTI,   // split or spanned archive
    ZAR_EPREFIX,  // offsets do not account for data before the archive
    ZAR_ENONAME,  // an entry, noname, has a zero-length name
};

// The result of a failed read, os_readat's got.
static i32 zar_failed(i32 got)
{
    return got<0 ? ZAR_EREAD : ZAR_EEOF;
}

// An entry's Unicode name, if it differs from its stored name, else a
// null string.
static s8 entry_unicode(zarchive *ar, iz i)
{
    zunames *u  = &ar->unames;
    iz       lo = 0;
    for (iz hi = u->len; lo < hi;) {
        iz mid = lo + (hi - lo)/2;
        if (u->data[mid].index < i) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo<u->len && u->data[lo].index==i ? u->data[lo].name : (s8){0};
}

// An entry's name in Unicode, by which Info-ZIP also finds the entry for
// a file, when it differs from the stored name: from a Unicode path field
// (written by Info-ZIP's Windows port, WinZip, 7-Zip) that checks out,
// else on Windows, as Info-ZIP's port reads them, from the OEM code page
// for names made on DOS or Windows, and from the ANSI code page, the
// port's own, for others, such as PKZIP for Windows makes, unless they
// are UTF-8. Returns a null string if none. Where a field does not check
// out, *stale is the name by which Info-ZIP warns of it, else null.
static s8 zar_uname(os *ctx, zentry *e, b32 windows, i32 *crccpu, s8 *stale,
                    arena *perm, arena scratch)
{
    *stale = (s8){0};
    if (e->flags & ZIP_FLAG_UTF8) {
        return (s8){0};
    }
    u32 crc = crc32_update(0, e->name.s, e->name.len, crccpu);
    s8  u   = zip_extra_upath(e->cextra, crc);
    if (!u.s || zip_utf8(u)<0) {
        i32 utf8 = zip_utf8(e->name);
        b32 oem  = zip_oem_name(e);
        b32 cp   = windows && (oem ? utf8 : utf8<0);
        u = cp ? os_fromcp(ctx, e->name, oem, perm, scratch) : (s8){0};
    }
    if (zip_extra_upath_stale(e->cextra, crc)) {
        *stale = u.len ? u : e->name;  // Info-ZIP gives "(null)"
    }
    b32 bad = zip_has(u, 0) || zip_has(e->name, 0);
    return !u.len || bad || zequals(u, e->name) ? (s8){0} : u;
}

// Find the end records of the archive open at fd, size bytes as
// examined, unless it is in memory (zin_memory), through a window in
// perm, claimed, as all else is, before any output. The end record's
// comment, once found, is kept in perm, even should the records then
// prove invalid. Returns a ZAR code: for ZAR_EPREFIX, the records are
// kept as they read, for the caller to shift (zar_entries).
//
// A Zip64 end record that is not where its locator says, but just before
// the locator, is taken there (ar->moved64), as UnZip's find_ecrec64 and
// Info-ZIP's zip look for it: data prepended to the archive without
// adjusting its offsets, which then shift them (ZAR_EPREFIX), as for an
// archive without Zip64 records. Where neither holds the record,
// ar->nosig64. Zip refuses a record found so even when the offsets add
// up, the locator alone being wrong, as it refuses other layouts that do
// not add up.
//
// For unzip (ar->unzip), the end record is read as UnZip reads it: one
// whose comment runs past the end of the file is taken, its comment cut
// short (ar->end.cut), as is one that counts more entries than its
// directory holds (as zar_check reads them). Zip refuses both, the
// latter on reading the directory, to count the headers it holds
// (zar_miscount).
static i32 zar_open(zarchive *ar, os *ctx, i32 fd, i64 size, arena *perm)
{
    zin *in = &ar->in;
    ar->bad   = -1;
    ar->found = -1;
    ar->size  = size;
    in->ctx   = ctx;
    if (!in->mem) {
        in->fd    = fd;
        in->limit = size;
        in->cap   = ZIN_CAP;
        in->buf   = newbytes(perm, in->cap);  // like all else, before output
        in->ahead = ZIN_PAGE;
    }

    // The end records are among the final bytes, read into the window,
    // which for a small archive then holds its central directory too
    iz  n    = (iz)MIN(size, ZIP_END_LEN + ZIP_MAX16 + ZIP_LOC64_LEN);
    u8 *tail = 0;
    i32 got  = zin_get(in, size-n, n, &tail);
    if (got <= 0) {
        return zar_failed(got);
    }
    i32 r = zip_find_end(tail, n, size, &ar->end);
    if (r!=ZIP_ENOEND && ar->end.cut && !ar->unzip) {
        return ZAR_ENOEND;
    } else if (r != ZIP_ENOEND) {
        ar->end.comment = JOIN(perm, ar->end.comment);  // the window moves
    }
    if (r==ZIP_OK && ar->end.end64>=0) {
        zend *e     = &ar->end;
        i64   guess = e->endpos - ZIP_LOC64_LEN - ZIP_END64_LEN;
        u8   *rec   = 0;
        got = zin_get(in, e->end64, ZIP_END64_LEN, &rec);
        if (got <= 0) {
            return zar_failed(got);
        }
        b32 sig = get32(rec) == ZIP_END64_SIG;
        if (!sig && guess!=e->end64) {
            got = zin_get(in, guess, ZIP_END64_LEN, &rec);
            if (got <= 0) {
                return zar_failed(got);
            }
            sig = get32(rec) == ZIP_END64_SIG;
            e->end64 = sig ? guess : e->end64;
            ar->moved64 = sig;
        }
        r = zip_parse_end64(rec, e);
        ar->moved64 &= e->end64 >= 0;
        if (ar->moved64 && !ar->unzip && r==ZIP_OK) {
            r = ZIP_EFORMAT;  // only the locator wrong: zip refuses it
        }
        ar->nosig64  = !sig && e->end64>=0;
    }
    switch (r) {
    case ZIP_OK:
        return ZAR_OK;
    case ZIP_ECOUNT:
        return ZAR_OK;  // zip refuses it in zar_walk, counting the headers
    case ZIP_ENOEND:
        return ZAR_ENOEND;
    case ZIP_EMULTI:
        return ZAR_EMULTI;
    case ZIP_EPREFIX:
        return ZAR_EPREFIX;
    }
    return ZAR_EFORMAT;
}

// Read the central header at off, of a directory ending at cdend, for
// an entry whose local header, and given fit, its data, lies before cdoff
// (as offsets read), through the window, no longer than max bytes. Its
// name, extra fields (with Zip64, unfiltered), and comment point into
// the window, valid until its next use. Returns a ZAR code, and its
// length in *len.
static i32 zar_header(zin *in, i64 off, i64 cdend, i64 cdoff, b32 fit,
                      iz max, zentry *e, iz *len)
{
    // Its fixed part tells its length, at most 192 KiB
    u8 *h   = 0;
    iz  n   = (iz)MIN(cdend-off, ZIP_CENTRAL_LEN);
    i32 got = zin_get(in, off, n, &h);
    if (got>0 && n==ZIP_CENTRAL_LEN) {
        iz var = zip_central_varlen(h);
        n += (iz)MIN(MAX(var, 0), cdend-off-n);
        if (n > max) {
            return ZAR_EFORMAT;
        }
        got = zin_get(in, off, n, &h);
    }
    if (got <= 0) {
        return zar_failed(got);
    }
    *len = zip_parse_header(h, n, cdoff, fit, e);
    if (*len && !e->name.len) {
        // Refused, as by Info-ZIP, rather than kept for readers that
        // cannot name it
        return ZAR_ENONAME;
    }
    return *len ? ZAR_OK : ZAR_EFORMAT;
}

// Refuse a central directory that zar_walk found not of its count, with
// bad the header that failed, but first count the headers there, as
// Info-ZIP's zip does to warn of it: if they parse to its very end, as
// when a writer without Zip64 wraps the count past 65,535 entries, or
// saturates it, ar->found of them. They are read through the window,
// claiming no memory, whatever the count.
static i32 zar_miscount(zarchive *ar, i64 bad)
{
    zin *in    = &ar->in;
    i64  off   = ar->end.cdoff;
    i64  cdend = off + ar->end.cdsize;
    i64  cdoff = ar->end.cdoff - ar->shift;
    i64  n     = 0;
    zentry one = {0};
    for (; off < cdend; n++) {
        iz  len = 0;
        i32 r   = zar_header(in, off, cdend, cdoff, !ar->unzip, in->cap,
                             &one, &len);
        if (r==ZAR_EREAD || r==ZAR_EEOF) {
            return r;
        } else if (r) {
            break;
        }
        off += len;
    }
    ar->bad   = bad;
    ar->found = off==cdend && ar->end.count>=0 && n!=ar->end.count ? n : -1;
    return ZAR_EFORMAT;
}

// How far, for unzip, an entry's local header may lie, read as UnZip
// reads it, wherever its offset says, even past the end of the file, but
// short of overflowing once shifted or unwrapped (zar_unwrap)
#define ZAR_MAXOFF ((i64)1 << 62)

// An archive over 4 GiB written without Zip64, entries each under 4 GiB,
// has its offsets wrapped to 32 bits. Unwrapped as 7-Zip unwraps them, an
// entry's offset, if from its 32-bit field, is the least at or past floor
// that is the same modulo 2^32, floor being where the entry before it in
// the central directory ends, at least (zar_floor). Each of those under
// 4 GiB, the gap to the next is less than 4 GiB, so unwrapped, offsets
// are those written, if entries are in the directory's order.
static i64 zar_unwrap(i64 off, i64 floor)
{
    i64 g = (i64)1 << 32;
    if (off>=g || off>=floor) {
        return off;  // from Zip64, or not wrapped
    }
    i64 r = (floor & -g) | off;
    return r<floor ? r+g : r;
}

// Where the entry after e begins, at least, given wrapped offsets: past
// its local header's fixed part and its data (at most ZAR_MAXOFF).
static i64 zar_floor(zentry *e)
{
    i64 room = ZAR_MAXOFF - ZIP_LOCAL_LEN - e->offset;
    return e->csize<room ? e->offset+ZIP_LOCAL_LEN+e->csize : ZAR_MAXOFF;
}

// Read the central directory that zar_open found, given perm keeping its
// entries there, else only checking them, as unzip does before reading
// them again (zar_reread). Given a shift (ar->shift), the end record's
// offset of the central directory is the caller's, already shifted, and
// entries' offsets are shifted as much. Entries' Unicode names are left
// to the caller (zar_uname). Returns a ZAR code: for ZAR_EFORMAT, the
// header that failed is ar->bad, the count of entries for a directory
// that does not end where it should, and the headers it holds instead,
// ar->found (zar_miscount).
//
// For unzip (ar->unzip), which only checks it, it is read as UnZip reads
// it (extract.c, list.c), not by the end record's count, but header by
// header while they parse, within the directory: ar->nread of them,
// which must be the count, or with no Zip64 end record, as writers
// without Zip64 wrap it past 65,535 entries, the count modulo 65,536.
// Then ar->endsig tells whether an end signature follows them, as UnZip
// checks ("didn't find end-of-central-dir signature"). Otherwise, the
// headers read are the entries, and the first that was not is ar->bad,
// ZAR_EFORMAT (0: none, and the central directory is not there).
static i32 zar_walk(zarchive *ar, arena *perm, arena scratch)
{
    // The central directory is read through the window, whole if it
    // fits, else a window at a time, and parsed a header at a time,
    // keeping only the names, extra fields (without Zip64), and comments,
    // packed, rather than the whole directory.
    zin *in    = &ar->in;
    i64 count  = ar->end.count;
    i64 off    = ar->end.cdoff;
    i64 cdend  = off + ar->end.cdsize;
    i64 cdoff  = ar->end.cdoff - ar->shift;  // as entries' offsets read
    i64 floor  = 0;                          // given ar->wrapped
    u8 *h      = 0;
    i32 got    = zin_get(in, off, (iz)MIN(cdend-off, in->cap), &h);
    if (got <= 0) {
        return zar_failed(got);
    }
    in->ahead = in->cap;

    // Memory for the entries is claimed only for a directory that begins
    // with a header, as a file that only ends like an archive (sparse, or
    // damaged) does not, and is then not touched until each is parsed.
    // A count kept unchecked for the caller to shift (ZAR_EPREFIX) may be
    // anything that a Zip64 record says, even past 2^63 (negative here).
    // Read as UnZip reads it, the count bounds nothing.
    b32 past = ar->unzip && !perm;
    b32 head = cdend-off>=ZIP_CENTRAL_LEN && zip_central_varlen(h)>=0;
    if (past) {
        // headers are read while they parse, whatever the count
    } else if (count<0 || count>ar->end.cdsize/ZIP_CENTRAL_LEN ||
               (count && !head)) {
        return zar_miscount(ar, 0);
    } else if ((u64)count > (uz)-1>>1) {
        os_oom(in->ctx);  // larger than the address space (32-bit hosts)
    }
    ar->cdlim  = ar->unzip ? ZAR_MAXOFF : cdoff;
    ar->maxhdr = 0;
    ar->nread  = 0;
    ar->endsig = 0;
    zentry one = {0};
    if (perm) {
        iz each = sizeof(zentry);
        ar->entries = alloc(perm, (iz)count, each, _Alignof(zentry), 0);
    }
    for (i64 i = 0; past ? off<cdend : i<count; i++) {
        zentry *e   = perm ? ar->entries+i : &one;
        iz      len = 0;
        i32     r   = zar_header(in, off, cdend, ar->cdlim, !ar->unzip,
                                 in->cap, e, &len);
        if (r == ZAR_ENONAME) {
            ar->noname = i;
            return r;
        } else if (r==ZAR_EFORMAT && past) {
            break;
        } else if (r == ZAR_EFORMAT) {
            return zar_miscount(ar, i);
        } else if (r) {
            return r;
        }
        ar->maxhdr = MAX(ar->maxhdr, len);
        if (ar->wrapped) {
            e->offset = zar_unwrap(e->offset, floor);
            floor     = zar_floor(e);
        }
        if (perm) {
            arena tmp = scratch;
            e->offset += ar->shift;
            e->name    = JOIN(perm, e->name);
            e->cextra  = JOIN(perm, zip_filter_extra(&tmp, e->cextra));
            e->comment = JOIN(perm, e->comment);
        }
        off += len;
        ar->nread = i + 1;
    }

    if (past) {
        // Offsets the caller would shift by a multiple of 4 GiB, as if
        // data came before the archive, are instead wrapped if the
        // central directory, its offset unwrapped as the entries' are,
        // is where it is: the entries leave no room for that data
        if (ar->wrapped) {
            ar->wrapped = zar_unwrap(cdoff, floor) == ar->end.cdoff;
            ar->shift   = ar->wrapped ? 0 : ar->shift;
        }

        // UnZip finds the directory missing if its first header is not
        // there, even for a count of zero, unless empty (the caller's)
        i64 n     = ar->nread;
        b32 match = ar->end.end64<0 ? (n & ZIP_MAX16)==count : n==count;
        if (!n || !match) {
            ar->bad = n;
            return ZAR_EFORMAT;
        }
        u8 *sig = 0;
        got = zin_get(in, off, 4, &sig);
        if (got <= 0) {
            return zar_failed(got);
        }
        // With Zip64 records, which UnZip also takes for a Zip64 archive
        // when the end record defers to them, it does not check
        ar->endsig = ar->end.end64>=0 || get32(sig)==ZIP_END_SIG;
    } else if (off != cdend) {
        return zar_miscount(ar, count);
    }
    in->limit = ar->end.cdoff;  // nothing past the entries is read again

    // Offsets may account for data before the first entry, such as a
    // self-extractor's stub after zip -A, or a zipapp's #! line, which
    // Info-ZIP keeps. Without entries, it precedes the central directory.
    ar->beg = ar->end.cdoff;
    for (i64 i = 0; perm && i < ar->end.count; i++) {
        ar->beg = MIN(ar->beg, ar->entries[i].offset);
    }
    return ZAR_OK;
}

// Read the central directory that zar_open found, keeping its entries,
// all in perm, as zar_walk reads it.
static i32 zar_entries(zarchive *ar, arena *perm, arena scratch)
{
    return zar_walk(ar, perm, scratch);
}

// Check the central directory that zar_open found, as zar_entries reads
// it, or for unzip as UnZip reads it (zar_walk), but keeping none of it,
// for zar_next to read it again.
[[maybe_unused]] static i32 zar_check(zarchive *ar)
{
    return zar_walk(ar, 0, (arena){0});
}

// Prepare to read again the central directory that zar_check checked,
// through its own window in perm, of at least window bytes, and no more
// than the directory, unless the archive is in memory, but holding its
// longest header: so read whole, if it fits, else a window at a time.
[[maybe_unused]] static void zar_reread(zarchive *ar, iz window, arena *perm)
{
    zin *cd = &ar->cd;
    *cd = ar->in;  // in memory: the same, a window that never moves
    if (!cd->mem) {
        cd->limit = ar->end.cdoff + ar->end.cdsize;
        cd->cap   = (iz)MIN(ar->end.cdsize, MAX(window, ar->maxhdr));
        cd->buf   = newbytes(perm, cd->cap);
        cd->len   = 0;
        cd->pos   = ar->end.cdoff;
        cd->ahead = cd->cap;  // the first fill reads the whole window
    }
}

// Read again the central header at *off, as zar_check read it, through
// the central directory's window (zar_reread), moving *off past it, its
// extra fields filtered (without Zip64) into a, and its name and comment
// pointing into that window, valid until its next use. Returns a ZAR
// code: anything but ZAR_OK means the archive has changed (or a read
// failed), as does a header longer than the longest read before. Given
// wrapped offsets, they are unwrapped as zar_check unwrapped them, by
// ar->floor, from the first header, else as the caller left it.
[[maybe_unused]] static i32 zar_next(zarchive *ar, i64 *off, zentry *e,
                                     arena *a)
{
    i64 cdend = ar->end.cdoff + ar->end.cdsize;
    iz  len   = 0;
    i32 r     = zar_header(&ar->cd, *off, cdend, ar->cdlim, !ar->unzip,
                           ar->maxhdr, e, &len);
    if (r) {
        return r==ZAR_ENONAME ? ZAR_EFORMAT : r;
    }
    if (ar->wrapped) {
        ar->floor = *off==ar->end.cdoff ? 0 : ar->floor;
        e->offset = zar_unwrap(e->offset, ar->floor);
        ar->floor = zar_floor(e);
    }
    e->offset += ar->shift;
    e->cextra  = zip_filter_extra(a, e->cextra);
    *off += len;
    return ZAR_OK;
}

// Read the central directory of the archive open at fd, size bytes as
// examined, through a window in perm, keeping its end records and its
// entries, all in perm, as all else is claimed, before any output. Its
// entries' Unicode names are left to the caller (zar_uname). Returns a
// ZAR code.
[[maybe_unused]]
static i32 zar_read(zarchive *ar, os *ctx, i32 fd, i64 size, arena *perm,
                    arena scratch)
{
    i32 r = zar_open(ar, ctx, fd, size, perm);
    return r ? r : zar_entries(ar, perm, scratch);
}

// An entry's name for messages: in Unicode, if it has that too.
[[maybe_unused]] static s8 shown_name(zarchive *ar, zentry *e)
{
    s8 u = entry_unicode(ar, e - ar->entries);
    return u.s ? u : e->name;
}

// Sift a[k] down the heap of the first n offsets, largest at the root.
static void zar_sift(i64 *a, iz k, iz n)
{
    for (iz c = 2*k + 1; c < n; k = c, c = 2*k + 1) {
        c += c+1<n && a[c+1]>a[c];
        if (a[k] >= a[c]) {
            return;
        }
        i64 t = a[k];
        a[k]  = a[c];
        a[c]  = t;
    }
}

// The index of the first span that begins past off, or nspans.
static iz zar_after(zarchive *ar, i64 off)
{
    iz lo = 0;
    for (iz hi = ar->nspans; lo < hi;) {
        iz mid = lo + (hi - lo)/2;
        if (ar->spans[mid] <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// Where the data of the entry whose local header is at off must end: the
// next span (zar_spans), else the central directory.
static i64 zar_limit(zarchive *ar, i64 off)
{
    iz k = zar_after(ar, off);
    return k<ar->nspans ? ar->spans[k] : ar->end.cdoff;
}

// Given ar->spans, where each of the nspans entries to be read begins
// (by zip, to copy them, or unzip, to test or extract them), sort them,
// in place, needing no more memory (a heap sort), and from then on bound
// each entry's data by the next one (zar_limit, zar_local). So that an
// entry is not read twice, as by many central headers that point at one
// local header (a zip bomb), each entry must then be checked as
// zar_overlaps checks it, before any is read.
[[maybe_unused]] static void zar_spans(zarchive *ar)
{
    i64 *a = ar->spans;
    iz   n = ar->nspans;
    for (iz k = n/2; k > 0;) {
        zar_sift(a, --k, n);
    }
    for (iz end = n; end > 1;) {
        i64 t  = a[0];
        a[0]   = a[--end];
        a[end] = t;
        zar_sift(a, 0, end);
    }
}

// Whether one of zar_spans's entries, e, overlaps another: they begin at
// once, or it cannot hold the least it takes, a local header and its
// data, before the next.
[[maybe_unused]] static b32 zar_overlaps(zarchive *ar, zentry *e)
{
    iz k = zar_after(ar, e->offset);
    return (k>=2 && ar->spans[k-2]==e->offset) ||
           e->csize > zar_limit(ar, e->offset) - e->offset - ZIP_LOCAL_LEN;
}

// An entry's local header, as zar_local finds it.
typedef struct {
    u8  fixed[ZIP_LOCAL_LEN];
    s8  name;   // in the window, valid until its next use
    s8  extra;  // likewise
    i64 data;   // offset of the entry's data
} zlocal;

// Find an entry's local header and its data, which must lie, at the
// entry's compressed size, before the central directory, and given
// zar_spans, before the next entry. Returns a ZAR code: ZAR_EFORMAT for
// data out of bounds.
static i32 zar_local(zarchive *ar, zentry *e, zlocal *l)
{
    zin *r     = &ar->in;
    u8  *fixed = 0;
    i32  got   = zin_get(r, e->offset, ZIP_LOCAL_LEN, &fixed);
    if (got <= 0) {
        return zar_failed(got);
    }
    bytecopy(l->fixed, fixed, ZIP_LOCAL_LEN);  // before the window moves
    iz  varlen = zip_local_varlen(l->fixed);
    iz  nlen   = get16(l->fixed+26);
    i64 data   = e->offset + ZIP_LOCAL_LEN + varlen;
    i64 limit  = zar_limit(ar, e->offset);
    if (varlen<0 || data>limit || e->csize>limit-data) {
        return ZAR_EFORMAT;
    }
    u8 *var = 0;
    got = zin_get(r, e->offset+ZIP_LOCAL_LEN, varlen, &var);
    if (got <= 0) {
        return zar_failed(got);
    }
    l->name  = (s8){var, nlen};
    l->extra = (s8){var+nlen, varlen-nlen};
    l->data  = data;
    return ZAR_OK;
}

// tugz zip program: Info-ZIP compatible command line and archive driver
//
// Builds the archive in a temporary file beside the target, then renames
// it into place. Entries kept from an existing archive are copied without
// recompressing. Exit statuses follow Info-ZIP, but for the departures
// listed in docs/notes.md ("Departures from Info-ZIP").

enum {
    ZE_OK    = 0,
    ZE_EOF   = 2,   // unexpected end of zip file, as one that has shrunk
    ZE_FORM  = 3,   // zip file structure invalid
    ZE_MEM   = 4,
    ZE_TEMP  = 10,  // temporary file failure, as in creating or closing it
    ZE_READ  = 11,  // could not read the existing archive
    ZE_NONE  = 12,  // nothing to do
    ZE_WRITE = 14,
    ZE_CREAT = 15,  // could not create the archive, or replace it
    ZE_PARMS = 16,
    ZE_OPEN  = 18,  // some input files could not be read
};

// Platform interface for the zip program, beyond src/io.c and src/dir.c

// Target of a symbolic link, or a null string on error. It and any
// temporaries come from one arena.
static s8   os_readlink(os *, s8 path, arena *);
// Positioned writes of exactly len bytes, which may not be mixed
// with os_write on the same descriptor.
static b32  os_writeat(os *, i32 fd, u8 *buf, iz len, i64 off);
static b32  os_truncate(os *, i32 fd, i64 len);
// The path to replace for path, so that symbolic links there survive
// the replacement: path itself unless it is a link, else the file the
// links lead to, which for a dangling link is where it points. Returns
// a null string for a link the system would not follow (a loop), or a
// path that cannot be examined for a reason other than nothing there.
static s8   os_resolve(os *, s8 path, arena *perm, arena scratch);
// Whether an existing file may be replaced as though written: on POSIX,
// that the user may write it; on Windows, that it is not read-only and
// that no other process holds it open in a way that refuses the rename,
// which it then prevents until os_commit.
static b32  os_writable(os *, s8 path, arena scratch);
// Close a created file, temp, and move it over path, keeping it. With
// replace, a file there is replaced by this new one, so its other hard
// links keep the old. Without, a file that has appeared there since
// fails it rather than be replaced (on POSIX, where the file system has
// hard links, as it is linked there; FAT and some network file systems
// have none, and there it is renamed). A deferred write error fails it
// before anything is replaced: on POSIX, one that closing reports
// (there is no fsync, as in Info-ZIP); on Windows, which renames before
// closing, one that flushing reports. On POSIX it takes the replaced
// file's mode, and its group where the user may give it, but not its
// owner or ACL: without the group, the group's permissions are those of
// others, and set-ID and sticky bits stay only with the same owner and
// group. It stays owner-only if the replaced file cannot be examined;
// without one it has a new file's permissions (0666 less the umask, and
// a default ACL if created with OS_DEFPERMS). On Windows it takes the
// replaced file's hidden, system, and not-indexed attributes, but like
// any new file gets its access control from the directory. Returns
// COMMIT_OK, or the step that failed, as os_error then tells why:
// COMMIT_ECLOSE, the deferred write error, or COMMIT_EREPLACE, the move.
// The descriptor is closed even on failure, which discards the file,
// unless whatever refused the move refuses removing it too.
enum { COMMIT_OK, COMMIT_ECLOSE, COMMIT_EREPLACE };
static i32  os_commit(os *, i32 fd, s8 temp, s8 path, b32 replace,
                      arena scratch);
// Whether a standard descriptor is a terminal (console).
static b32  os_isatty(os *, i32 fd);
// Hold the writer of standard output, whose text an interruption that
// ends the program writes first, where it can (POSIX's signal handler).
static void os_holdtext(os *, writer *);
// Why the last failed system call failed, in the words of the C
// library's strerror, as Info-ZIP reports I/O errors, or an empty string
// if unknown. The text may last only until the next call, as the BSDs'
// strerror reuses one buffer, so a reason kept for later is copied.
static s8   os_error(os *);
// A file's full path, past links, which tells files apart where their
// file IDs are unknown (Windows), or a null string.
static s8   os_fullpath(os *, s8 path, arena *perm, arena scratch);

// Standard output's writer, flushed by os_oom, which the platform calls
// with only its context, the reason this is global. Memory is claimed
// before the archive is written, and with it the progress lines, all but
// a failure's reason, copied into perm once a read or write has failed.
static writer *oom_out;

static void os_oom(os *ctx)
{
    s8 msg = S("\nzip error: Out of memory\n");
    if (oom_out) {
        writer_flush(oom_out);
    }
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, ZE_MEM);
}

// The two arenas share one region, perm growing up from its bottom and
// scratch down from its top, so that neither strands what the other
// could use. The platform's os_extend gives each more as it needs it.
typedef struct {
    arena perm;
    arena scratch;  // allocates downward
    s8   *args;     // excluding the program name
    i32   nargs;
    s8    epoch;    // SOURCE_DATE_EPOCH, if set
    s8    zipopt;   // ZIPOPT, if set: options before the arguments
    s8    zipenv;   // ZIP, if set: options when ZIPOPT has none
    b32   windows;  // Windows conventions: names, attributes, wildcards
} zipconfig;

static s8 const zip_usage = S8(
    "tugz zip " TUGZ_VERSION ", a subset of Info-ZIP Zip 3.0\n"
    "usage: zip [-options] archive[.zip] [path ...] [-x pattern ...]\n"
    "  -0..-9  store only, compress faster..better (default 6)\n"
    "  -r      recurse into directories\n"
    "  -q      quiet: no messages or warnings\n"
    "  -X      no extra attributes (times, uid/gid): deterministic output\n"
    "  -@      read paths from standard input, one per line\n"
    "  -j      junk directory names    -D  no directory entries\n"
);
static s8 const zip_usage_posix = S8(
    "  -y      store symbolic links as links\n"
);
static s8 const zip_usage_windows = S8(
    "  -S      include hidden and system files\n"
);
static s8 const zip_usage_tail = S8(
    "  -u      update if newer         -f  freshen: update existing only\n"
    "  -FS     filesync: update changed and delete missing entries\n"
    "  -d      delete entries matching patterns from the archive\n"
    "  -x pattern ...  exclude         -i pattern ...  include only\n"
    "  -nw     no wildcards            --  end of options\n"
    "  -v      version (alone)         -L  license\n"
    "ZIPOPT, or else ZIP, may hold options to apply first.\n"
    "SOURCE_DATE_EPOCH, if set, clamps times and makes them UTC.\n"
);

static s8 const zip_license = S8(
    "This is free and unencumbered software released into the public domain.\n"
    "It is provided \"as is\", without warranty of any kind. For more\n"
    "information, see the UNLICENSE file or <http://unlicense.org/>.\n"
);

enum { MODE_ADD, MODE_UPDATE, MODE_FRESHEN, MODE_SYNC, MODE_DELETE };

typedef struct {
    s8      path;  // on the file system
    s8      name;  // in the archive
    os_info info;
    u32     dostime;
    b32     byentry;  // an entry named it: read through no final link
} zfile;

// Each file is allocated alone, as names are allocated between them, so
// that only the array of pointers moves as it grows.
typedef struct {
    zfile **data;
    iz      len;
    iz      cap;
} zfiles;

typedef struct zmap zmap;
struct zmap {
    zmap *child[4];
    s8    key;
    iz    value;
};

// A file whose name another file also has
typedef struct {
    s8 name;  // in the archive
    s8 path;  // a directory's with a slash, as Info-ZIP names it
} zdup;

typedef struct {
    os     *ctx;
    arena   perm;
    writer *out;  // standard output, flushed before standard error, and
                  // at check points on a terminal (writer_poll)
    i32     level;
    i32     mode;
    b32     quiet;
    b32     recurse;
    b32     noextra;
    b32     junk;
    b32     nodirs;
    b32     symlinks;
    b32     hidden;
    b32     nowild;
    b32     names_stdin;
    b32     filesync;  // -FS, which becomes MODE_SYNC after parsing
    b32     windows;
    b32     haveepoch;
    i64     epoch;
    s8      archive;
    s8      target;  // past links at the archive path; null if unfollowable
    s8s     paths;
    s8s     optargs;    // option arguments, values included, in order
    s8s     nonopts;    // the other arguments: archive, paths, and "--"
    iz      firstpath;  // the index in nonopts of the first path, if any
    s8s     include;
    s8s     exclude;
    zfiles  files;
    zmap   *names;
    zdup    dups[2];  // the repeated name to report, if any: two files
    os_info arcinfo;
    b32     arcexists;
    s8      arcpath;  // full path, if the archive's file ID is unknown
    i32     status;
    iz      nread;     // files and entries read, as Info-ZIP counts them
    i64     bread;
    iz      nskipped;
    i64     bskipped;
    i32     crccpu;  // crc32_update's answer from the CPU
} zip;

// A byte count as Info-ZIP's WriteNumString abbreviates it: in full
// below 1000, else in units of 1024 (K, M, G, T) to three digits
// ("292K"), or one and tenths ("9.8K").
static s8 zbytes(arena *a, i64 v)
{
    i64 n    = v;
    iz  unit = 0;
    for (; n >= 10240; unit++) {
        n >>= 10;
    }
    s8 r = znum(a, n);
    if (n >= 1000) {
        n = n*10 >> 10;
        unit++;
        r = JOIN(a, znum(a, n/10), S("."), znum(a, n%10));
    }
    s8 units = S(" KMGT?");
    s8 u = {units.s + MIN(unit, 5), 1};
    return unit ? JOIN(a, r, u) : r;
}

static u64 zhash(s8 s, i32 fold)
{
    u64 h = 0x100;
    for (iz i = 0; i < s.len; i++) {
        h ^= zip_fold(s.s[i], fold);
        h *= 1111111111111111111u;
    }
    return h;
}

// Find a key's value, inserting it with value -1 if perm is not null.
// Keys are compared ignoring ASCII case given ZIP_FOLD, by which they
// are hashed too, so that the key is the name itself, not a folded copy.
static iz *zmap_upsert(zmap **m, s8 key, i32 fold, arena *perm)
{
    for (u64 h = zhash(key, fold); *m; h <<= 2) {
        if (zfoldeq(key, (*m)->key, fold)) {
            return &(*m)->value;
        }
        m = &(*m)->child[h>>62];
    }
    if (!perm) {
        return 0;
    }
    *m = new(perm, 1, zmap);
    (*m)->key   = key;
    (*m)->value = -1;
    return &(*m)->value;
}

// Write a message to standard output (1), or error (2), first flushing
// standard output, so that the two keep their order on one terminal.
static void say(zip *z, i32 fd, s8 msg)
{
    if (fd == 1) {
        writer_s8(z->out, msg);
    } else {
        writer_flush(z->out);
        os_write(z->ctx, fd, msg.s, msg.len);
    }
}

static void warn(zip *z, s8 msg, s8 arg, arena scratch)
{
    if (!z->quiet) {
        say(z, 2, JOIN(&scratch, S("zip warning: "), msg, arg, S("\n")));
    }
}

// Report a fatal error and return its exit status. Info-ZIP gives an I/O
// error's system reason first, in place of the blank line: why, which
// os_error gave when the error occurred, if it could tell.
static i32 fail_why(zip *z, i32 status, s8 why, s8 msg, s8 arg,
                    arena scratch)
{
    s8 head = why.len ? JOIN(&scratch, S("zip I/O error: "), why) : S("");
    s8 tail = arg.s ? JOIN(&scratch, S(" ("), arg, S(")")) : S("");
    say(z, 2, JOIN(&scratch, head, S("\nzip error: "), msg, tail, S("\n")));
    return status;
}

// Report a fatal error that has just occurred.
static i32 fail(zip *z, i32 status, s8 msg, s8 arg, arena scratch)
{
    b32 io = status==ZE_TEMP  || status==ZE_READ || status==ZE_WRITE ||
             status==ZE_CREAT || status==ZE_OPEN;
    s8 why = io ? os_error(z->ctx) : S("");
    return fail_why(z, status, why, msg, arg, scratch);
}

static i32 badopt(zip *z, s8 kind, s8 opt, arena scratch)
{
    s8 msg = JOIN(&scratch, kind, S(" option '"), opt,
                  S("' not supported"));
    return fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
}

// Append each name in a list (-@, @file) as Info-ZIP's getnam reads it:
// a line ends at any CR or LF, empty lines are skipped, and a name ends
// at a NUL. On Windows, trailing spaces and periods are dropped first.
static void push_lines(zip *z, s8s *list, s8 text)
{
    for (iz i = 0; i < text.len;) {
        iz j = i;
        for (; j<text.len && text.s[j]!='\n' && text.s[j]!='\r'; j++) {}
        s8 line = {text.s+i, j-i};
        i = j + 1;
        if (!line.len) {
            continue;
        }
        while (z->windows && line.len && (line.s[line.len-1]==' ' ||
                                         line.s[line.len-1]=='.')) {
            line.len--;
        }
        iz len = 0;
        for (; len<line.len && line.s[len]; len++) {}
        line.len = len;
        *push(&z->perm, list) = line;
    }
}

// Whether an argument ends a pattern list after its first value: an
// option, or as in Info-ZIP any argument starting with '-', a lone "-"
// and "--" included, or a lone @, which is dropped.
static b32 is_list_end(s8 arg)
{
    return (arg.len && arg.s[0]=='-') || zequals(arg, S("@"));
}

// Add an element of a pattern list: a pattern, or @file, a file of them,
// one per line. Like Info-ZIP, patterns are normalized as names are, so
// that ./ and / prefixes, and Windows backslashes, match.
static i32 add_patterns(zip *z, s8s *list, s8 arg, arena scratch)
{
    iz first = list->len;
    if (!arg.len || arg.s[0]!='@') {
        *push(&z->perm, list) = arg;
    } else if (arg.len == 1) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("missing file after @"), scratch);
    } else {
        // Info-ZIP reads the list through fopen and getc. On POSIX that
        // opens a directory, which getc then fails to read: an empty
        // list. Windows' C runtime refuses to open one, as EACCES.
        s8  path = {arg.s+1, arg.len-1};
        i32 fd   = os_open(z->ctx, path, OS_READ, scratch);
        b32 dir  = fd == OS_EISDIR;
        s8  text = dir && !z->windows ? S("")
                 : fd>=0 ? read_all(z->ctx, fd, &z->perm) : (s8){0};
        s8  why  = text.s ? S("")
                 : dir    ? S("Permission denied")
                 : os_error(z->ctx);
        if (fd >= 0) {
            os_close(z->ctx, fd);
        }
        if (!text.s) {
            s8 opt = list==&z->exclude ? S("x") : S("i");
            s8 msg = JOIN(&scratch, opt, S(" pattern file '"), arg, S("'"));
            return fail_why(z, ZE_OPEN, why,
                            S("File not found or no read permission"), msg,
                            scratch);
        }
        push_lines(z, list, text);
    }
    for (iz i = first; i < list->len; i++) {
        list->data[i] = zip_name(&z->perm, list->data[i], z->windows);
    }
    return 0;
}

// Set the action, -u, -f, or -d, which as in Info-ZIP may be given only
// once. -FS is a flag there, checked against these after parsing.
static i32 set_mode(zip *z, i32 mode, arena scratch)
{
    if (z->mode != MODE_ADD) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("specify just one action"), scratch);
    }
    z->mode = mode;
    return 0;
}

static i32 usage(zip *z, i32 status)
{
    i32 fd = status ? 2 : 1;
    say(z, fd, zip_usage);
    say(z, fd, z->windows ? zip_usage_windows : zip_usage_posix);
    say(z, fd, zip_usage_tail);
    return status;
}

static void version(zip *z)
{
    s8 line = zip_usage;
    for (line.len = 0; line.s[line.len++] != '\n';) {}
    say(z, 1, line);
}

enum {
    OPT_NEGATE = 1 << 0,  // negatable: only -X, as in Info-ZIP
    OPT_LIST   = 1 << 1,  // takes a pattern list
    OPT_POSIX  = 1 << 2,  // not in Info-ZIP's Windows port
    OPT_WIN    = 1 << 3,  // only in Info-ZIP's Windows port
};

typedef struct {
    s8  name;   // short name, if any
    s8  lname;  // long name, if any
    s8  help;   // Info-ZIP's description, which its errors quote
    i32 flags;
} zoption;

// The supported subset of Info-ZIP's options, under its names, and its
// -mm, which it has only to reject with its own message
static zoption const zip_options[] = {
    {S8("0"),  S8("store"),          S8("store"),                       0},
    {S8("1"),  S8("compress-1"),     S8("compress 1"),                  0},
    {S8("2"),  S8("compress-2"),     S8("compress 2"),                  0},
    {S8("3"),  S8("compress-3"),     S8("compress 3"),                  0},
    {S8("4"),  S8("compress-4"),     S8("compress 4"),                  0},
    {S8("5"),  S8("compress-5"),     S8("compress 5"),                  0},
    {S8("6"),  S8("compress-6"),     S8("compress 6"),                  0},
    {S8("7"),  S8("compress-7"),     S8("compress 7"),                  0},
    {S8("8"),  S8("compress-8"),     S8("compress 8"),                  0},
    {S8("9"),  S8("compress-9"),     S8("compress 9"),                  0},
    {S8("d"),  S8("delete"),         S8("delete entries from archive"), 0},
    {S8("D"),  S8("no-dir-entries"),
               S8("no entries for dirs themselves (-x */)"),            0},
    {S8("FS"), S8("filesync"),
               S8("add/delete entries to make archive match OS"),       0},
    {S8("f"),  S8("freshen"),    S8("freshen existing archive entries"), 0},
    {S8("h"),  S8("help"),           S8("help"),                        0},
    {S8("H"),  S8(""),               S8("help"),                        0},
    {S8("?"),  S8(""),               S8("help"),                        0},
    {S8("h2"), S8("more-help"),      S8("extended help"),               0},
    {S8("i"),  S8("include"),
               S8("include only files matching patterns"),       OPT_LIST},
    {S8("j"),  S8("junk-paths"),
               S8("strip paths and just store file names"),             0},
    {S8("L"),  S8("license"),        S8("display license"),             0},
    {S8("mm"), S8(""),               S8("not used"),                    0},
    {S8("nw"), S8("no-wild"),  S8("no wildcards during add or update"), 0},
    {S8("p"),  S8("paths"),          S8("store paths"),                 0},
    {S8("q"),  S8("quiet"),          S8("quiet"),                       0},
    {S8("r"),  S8("recurse-paths"),  S8("recurse down listed paths"),   0},
    {S8("S"),  S8(""),         S8("include system and hidden"),   OPT_WIN},
    {S8("u"),  S8("update"),
               S8("update existing entries and add new"),               0},
    {S8("v"),  S8("verbose"),  S8("display additional information"),    0},
    {S8(""),   S8("version"),
               S8("(if no other args) show version information"),       0},
    {S8("x"),  S8("exclude"),
               S8("exclude files matching patterns"),            OPT_LIST},
    {S8("X"),  S8("strip-extra"),
               S8("-X- keep all ef, -X strip but critical ef"), OPT_NEGATE},
    {S8("y"),  S8("symlinks"),       S8("store symbolic links"), OPT_POSIX},
    {S8("@"),  S8("names-stdin"),
               S8("get file names from stdin, one per line"),           0},
};

// All of Info-ZIP's long option names, supported or not, for its
// abbreviations: a prefix of exactly one name stands for that name. A
// few exist only in its Unix builds, or only in its Windows port, where
// "--i" is ambiguous.
static char const zip_longnames_posix[]   = " symlinks ";
static char const zip_longnames_windows[] =
    " archive-clear archive-set ignore-case use-privileges ";
static char const zip_longnames[] =
    " store compress-1 compress-2 compress-3 compress-4 compress-5"
    " compress-6 compress-7 compress-8 compress-9 adjust-sfx temp-path"
    " entry-comments delete display-bytes display-counts display-dots"
    " display-globaldots dot-size display-usize display-volume"
    " no-dir-entries difference-archive encrypt fix fixfix fifo filesync"
    " freshen force-descriptors force-zip64 grow help more-help include"
    " junk-paths junk-sfx DOS-names to-crlf from-crlf logfile-path"
    " log-append log-info license move must-match suffixes no-wild"
    " latest-time output-file paths password quiet recurse-paths"
    " recurse-patterns regex split-size split-pause split-verbose"
    " split-bell show-command show-debug show-files show-options"
    " show-unicode show-just-unicode unicode from-date before-date test"
    " unzip-command update copy-entries verbose version wild-stop-dirs"
    " exclude strip-extra archive-comment compression-method names-stdin ";

// Whether an argument has one of Info-ZIP's two-letter short options at
// k, which it matches before single letters, so that each is rejected
// under its own name. Only FS, nw, and h2 are supported. The last three
// exist only in its Windows port.
static b32 two_letter(zip *z, s8 arg, iz k)
{
    s8 opts = S("FSnwh2FFFIDFMMRETTUNdbdcdddgdsdudvfdfzlalflillmmsbscsdsf"
                "sospsusUsvttwsACASic");
    opts.len -= z->windows ? 0 : 6;
    for (iz t = 0; k+1<arg.len && t<opts.len; t += 2) {
        if (arg.s[k]==opts.s[t] && arg.s[k+1]==opts.s[t+1]) {
            return 1;
        }
    }
    return 0;
}

// A supported option by its short or long name, or null.
static zoption const *find_option(zip *z, s8 name, b32 islong)
{
    for (iz i = 0; i < countof(zip_options); i++) {
        zoption const *o = zip_options + i;
        if (name.len && zequals(name, islong ? o->lname : o->name)) {
            b32 absent = o->flags & (z->windows ? OPT_POSIX : OPT_WIN);
            return absent ? 0 : o;
        }
    }
    return 0;
}

// Expand an abbreviated long option name. Returns false, having
// reported it, if it is ambiguous.
static b32 expand_long(zip *z, s8 *name, arena scratch)
{
    s8  found = {0};
    i32 count = 0;
    char const *lists[] = {
        zip_longnames, z->windows ? zip_longnames_windows : zip_longnames_posix
    };
    for (iz l = 0; l < countof(lists); l++) {
        for (char const *p = lists[l]+1; *p && name->len; p++) {
            s8 full = {(u8 *)p, 0};
            for (; p[full.len] != ' '; full.len++) {}
            p += full.len;
            if (zequals(full, *name)) {
                return 1;
            } else if (full.len>name->len &&
                       zequals((s8){full.s, name->len}, *name)) {
                found = full;
                count++;
            }
        }
    }
    if (count > 1) {
        s8 msg = JOIN(&scratch, S("long option '"), *name, S("' ambiguous"));
        fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
        return 0;
    }
    *name = count ? found : *name;
    return 1;
}

static i32 misused(zip *z, s8 opt, zoption const *o, s8 how, arena scratch)
{
    s8 msg = JOIN(&scratch, S("option '"), opt, S("' ("), o->help, S(") "),
                  how);
    return fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
}

// Collect a pattern list option's values as Info-ZIP's get_option does:
// an attached value, even an empty one ("-x=", which matches nothing),
// else the next argument, whatever it is, and those after it up to the
// end of the list.
static i32 take_list(zip *z, zoption const *o, s8 opt, s8s *list, s8 value,
                     s8 *args, i32 nargs, i32 *i, arena scratch)
{
    if (value.s) {
        return add_patterns(z, list, value, scratch);
    } else if (*i+1 == nargs) {
        return misused(z, opt, o, S("requires a value"), scratch);
    }
    i32 err = add_patterns(z, list, args[++*i], scratch);
    for (; !err && *i+1<nargs && !is_list_end(args[*i+1]);) {
        err = add_patterns(z, list, args[++*i], scratch);
    }
    if (!err && *i+1<nargs && zequals(args[*i+1], S("@"))) {
        ++*i;
    }
    return err;
}

// Apply an option, as given by opt, taking a pattern list's values from
// value or the following arguments. Returns an exit status to stop with:
// an error, or -1 for success after an informational option.
static i32 apply_option(zip *z, zoption const *o, s8 opt, b32 negate,
                        s8 value, s8 *args, i32 nargs, i32 *i,
                        arena scratch)
{
    if (negate && !(o->flags & OPT_NEGATE)) {
        return misused(z, opt, o, S("not negatable"), scratch);
    }

    s8 key = o->name.len ? o->name : o->lname;
    if (key.len==1 && key.s[0]>='0' && key.s[0]<='9') {
        z->level = key.s[0] - '0';
        return 0;
    } else if (zequals(key, S("h")) || zequals(key, S("H")) ||
               zequals(key, S("?")) || zequals(key, S("h2"))) {
        return usage(z, 0) - 1;
    } else if (zequals(key, S("version"))) {
        version(z);
        return -1;
    } else if (zequals(key, S("L"))) {
        say(z, 1, zip_license);
        return -1;
    } else if (zequals(key, S("mm"))) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("-mm not supported, Must_Match is -MM"), scratch);
    } else if (o->flags & OPT_LIST) {
        s8s *list = key.s[0]=='x' ? &z->exclude : &z->include;
        return take_list(z, o, opt, list, value, args, nargs, i, scratch);
    }

    b32 on = !negate;
    if (zequals(key, S("q")))  { z->quiet       = on; return 0; }
    if (zequals(key, S("r")))  { z->recurse     = on; return 0; }
    if (zequals(key, S("X")))  { z->noextra     = on; return 0; }
    if (zequals(key, S("j")))  { z->junk        = on; return 0; }
    if (zequals(key, S("D")))  { z->nodirs      = on; return 0; }
    if (zequals(key, S("y")))  { z->symlinks    = on; return 0; }
    if (zequals(key, S("S")))  { z->hidden      = on; return 0; }
    if (zequals(key, S("nw"))) { z->nowild      = on; return 0; }
    if (zequals(key, S("@")))  { z->names_stdin = on; return 0; }
    if (zequals(key, S("FS"))) { z->filesync    = on; return 0; }
    if (zequals(key, S("u")))  { return set_mode(z, MODE_UPDATE,  scratch); }
    if (zequals(key, S("f")))  { return set_mode(z, MODE_FRESHEN, scratch); }
    if (zequals(key, S("d")))  { return set_mode(z, MODE_DELETE,  scratch); }
    return 0;  // -p (store paths, the default) and -v (verbose): no effect
}

static i32 parse_args(zip *z, s8 *args, i32 nargs, arena scratch)
{
    s8  stream  = S("streaming with - not supported");
    b32 options = 1;
    b32 dash    = 0;
    for (i32 i = 0; i < nargs; i++) {
        s8 arg = args[i];
        if (!options || arg.len<2 || arg.s[0]!='-') {
            if (!z->archive.s && zequals(arg, S("-"))) {
                return fail(z, ZE_PARMS, S("Invalid command arguments"),
                            stream, scratch);
            } else if (!z->archive.s) {
                z->archive = arg;
            } else {
                dash |= zequals(arg, S("-"));
                z->firstpath = z->firstpath ? z->firstpath : z->nonopts.len;
                *push(&z->perm, &z->paths) = arg;
            }
            *push(&z->perm, &z->nonopts) = arg;
            continue;
        }

        if (zequals(arg, S("--"))) {
            if (!z->archive.s) {
                return fail(z, ZE_PARMS, S("Invalid command arguments"),
                            S("can't use -- before archive name"), scratch);
            }
            options = 0;
            *push(&z->perm, &z->nonopts) = arg;  // as Info-ZIP orders it
            continue;
        }

        i32 first = i;
        if (arg.s[1] == '-') {
            s8 name  = {arg.s+2, arg.len-2};
            s8 value = {0};
            for (iz k = 0; k < name.len; k++) {
                if (name.s[k] == '=') {
                    value = (s8){name.s+k+1, name.len-k-1};
                    name.len = k;
                    break;
                }
            }
            b32 negate = name.len>1 && name.s[name.len-1]=='-';
            name.len -= negate;
            if (!expand_long(z, &name, scratch)) {
                return ZE_PARMS;
            }
            zoption const *o = find_option(z, name, 1);
            if (!o) {
                return badopt(z, S("long"), name, scratch);
            } else if (value.s && !(o->flags & OPT_LIST)) {
                return misused(z, name, o, S("does not allow a value"),
                               scratch);
            }
            i32 err = apply_option(z, o, name, negate, value, args, nargs,
                                   &i, scratch);
            if (err) {
                return err;
            }
        } else {
            for (iz k = 1; k < arg.len;) {
                s8 opt = {arg.s+k, 1 + two_letter(z, arg, k)};
                k += opt.len;
                zoption const *o = find_option(z, opt, 0);
                if (!o) {
                    return badopt(z, S("short"), opt, scratch);
                }

                // A list's value is the rest of the argument, if any,
                // even "-" or nothing after a leading '=', which is cut
                s8  value  = {0};
                b32 negate = 0;
                if (o->flags & OPT_LIST) {
                    if (k < arg.len) {
                        k += arg.s[k] == '=';
                        value = (s8){arg.s+k, arg.len-k};
                        k = arg.len;
                    }
                } else {
                    negate = k<arg.len && arg.s[k]=='-';
                    k += negate;
                }
                i32 err = apply_option(z, o, opt, negate, value, args, nargs,
                                       &i, scratch);
                if (err) {
                    return err;
                }
            }
        }
        for (; first <= i; first++) {
            *push(&z->perm, &z->optargs) = args[first];
        }
    }

    // A "-" path reads standard input, except that under -d it names
    // the entry that doing so makes, as in Info-ZIP
    if (dash && z->mode!=MODE_DELETE) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"), stream,
                    scratch);
    }
    return 0;
}

// A file's DOS time, in UTC with SOURCE_DATE_EPOCH, and clamped to it
// if clamp, as it is for writing. Odd seconds round up, as in Info-ZIP,
// except where that would pass the epoch from a time at or before it,
// so that a clamped time never exceeds the epoch, and compares equal
// unclamped. Times far outside the DOS range clamp to its ends, give
// or take two days for the time zone, before any arithmetic.
static u32 file_dostime(zip *z, i64 t, b32 clamp)
{
    t = MAX(t, (i64)315532800 - 2*86400);   // 1980-01-01
    t = MIN(t, (i64)4354819200 + 2*86400);  // 2108-01-01
    b32 cap = z->haveepoch && (clamp || t<=z->epoch);
    t = cap ? MIN(t, z->epoch) : t;
    t = (t + 1) & ~(i64)1;
    t -= cap && t>z->epoch ? 2 : 0;
    i32 tm[6];
    if (z->haveepoch) {
        zip_gmtime(t, tm);
    } else {
        os_localtime(z->ctx, t, tm);
    }
    return zip_dostime(tm);
}

// Pattern matching as Info-ZIP's: by bytes, with [sets], on Unix; on
// Windows, by characters, with its DOS rules, ignoring case except
// against archive entries (-d) and when freshening.
static i32 match_flags(zip *z)
{
    i32 flags = z->nowild ? ZIP_NOWILD : 0;
    if (!z->windows) {
        return flags | ZIP_SETS;
    }
    b32 exact = z->mode==MODE_DELETE || z->mode==MODE_FRESHEN;
    return flags | ZIP_DOS | ZIP_UTF8 | (exact ? 0 : ZIP_FOLD);
}

static b32 any_match(zip *z, s8s *patterns, s8 name)
{
    for (iz i = 0; i < patterns->len; i++) {
        if (zip_match(patterns->data[i], name, match_flags(z))) {
            return 1;
        }
    }
    return 0;
}

// Whether a name passes the -i and -x patterns.
static b32 included(zip *z, s8 name)
{
    if (z->include.len && !any_match(z, &z->include, name)) {
        return 0;
    }
    return !any_match(z, &z->exclude, name);
}

static b32 is_dirname(s8 name)
{
    return name.len && name.s[name.len-1]=='/';
}

static b32 selected(zip *z, s8 name)
{
    if (is_dirname(name) && (z->nodirs || z->junk)) {
        return 0;
    }
    return included(z, name);
}

// Whether two files are known to be one: an unknown ID matches nothing.
static b32 same_file(os_info *a, os_info *b)
{
    return (a->ino[0] || a->ino[1]) && a->dev==b->dev &&
           a->ino[0]==b->ino[0] && a->ino[1]==b->ino[1];
}

// Whether a listing tells enough to skip os_stat, saving a handle per
// file on Windows. It lacks identity, which a directory needs to detect
// loops, and a file only if, being the archive's size, it may be that.
static b32 listed(zip *z, os_info *info)
{
    return info->type==FT_FILE &&
           (!z->arcexists || info->size!=z->arcinfo.size);
}

static b32 is_sep(zip *z, u8 c)
{
    return c=='/' || (z->windows && c=='\\');
}

static s8 basename(zip *z, s8 path)
{
    iz i = path.len;
    for (; i>0 && is_sep(z, path.s[i-1]); i--) {}
    path.len = i;
    for (; i>0 && !is_sep(z, path.s[i-1]); i--) {}
    return (s8){path.s+i, path.len-i};
}

// A path as add_file compares paths: without repeated or trailing
// separators, and on Windows, where case does not matter, folded.
static s8 trim_path(zip *z, s8 path, arena *a)
{
    i32 fold = z->windows ? ZIP_FOLD : 0;
    s8  r    = {newstr(a, path.len), 0};
    for (iz i = 0; i < path.len; i++) {
        b32 sep = is_sep(z, path.s[i]);
        if (!sep || !r.len || r.s[r.len-1]!='/') {
            r.s[r.len++] = sep ? '/' : zip_fold(path.s[i], fold);
        }
    }
    r.len -= r.len>1 && r.s[r.len-1]=='/';
    return r;
}

// Whether a file is the archive, by identity, or where the file system
// reports no file IDs, as some network and virtual drives do, by full
// path, so that the archive is still left out (Info-ZIP compares times
// and sizes). Otherwise an unknown ID matches nothing.
static b32 is_archive(zip *z, s8 path, os_info *info, arena scratch)
{
    if (!z->arcexists || info->type!=FT_FILE) {
        return 0;
    } else if (!z->arcpath.s || info->size!=z->arcinfo.size) {
        return same_file(info, &z->arcinfo);
    }
    s8 full = os_fullpath(z->ctx, path, &z->perm, scratch);  // rare, small
    return zequals(full, z->arcpath);
}

// A file's record, its path and name already in perm.
static zfile *new_file(zip *z, s8 path, s8 name, os_info *info)
{
    zfile *f = new(&z->perm, 1, zfile);
    f->path    = path;
    f->name    = name;
    f->info    = *info;
    f->dostime = file_dostime(z, info->mtime, 1);
    return f;
}

// Copy a file's path and name into perm to record it, sharing bytes
// where the name ends the path, as most do ("d/f", "./d/f", -j's "f"),
// or the path begins it, as for a directory ("d" and "d/").
static void keep_names(arena *perm, s8 *path, s8 *name)
{
    iz n = path->len;
    iz m = name->len;
    if (m<=n && zequals((s8){path->s+n-m, m}, *name)) {
        *path = JOIN(perm, *path);
        *name = (s8){path->s+n-m, m};
    } else if (n<=m && zequals((s8){name->s, n}, *path)) {
        *name = JOIN(perm, *name);
        *path = (s8){name->s, n};
    } else {
        *path = JOIN(perm, *path);
        *name = JOIN(perm, *name);
    }
}

// How archive entries, and files to add, are found by name: on Windows,
// ignoring ASCII case, as Info-ZIP's port finds entries (its namecmp).
// It compares files to add by bytes (strcmp), adding both of two names
// that differ only in case, which then collide when extracted there,
// while here they are one name.
static i32 name_fold(zip *z)
{
    return z->windows ? ZIP_FOLD : 0;
}

// Whether a name is the archive's path as given, compared as Info-ZIP
// compares names: ignoring ASCII case on Windows.
static b32 names_archive(zip *z, s8 name)
{
    return zfoldeq(name, z->archive, name_fold(z));
}

// The order of names and paths in which the first repeat is reported:
// by bytes, as Info-ZIP's check_dup sorts them (strcmp), but on Windows,
// where names differing only in case are one name, ignoring case, as
// upper case, as its port's namecmp compares names.
static i32 namecmp(zip *z, s8 a, s8 b)
{
    for (iz i = 0; i<a.len && i<b.len; i++) {
        i32 x = a.s[i];
        i32 y = b.s[i];
        if (z->windows) {
            x -= x>='a' && x<='z' ? 'a'-'A' : 0;
            y -= y>='a' && y<='z' ? 'a'-'A' : 0;
        }
        if (x != y) {
            return x - y;
        }
    }
    return a.len<b.len ? -1 : a.len>b.len;
}

// Note a file whose name an earlier file has. Info-ZIP sorts files by
// path, drops paths given again, then sorts them stably by name, and
// reports only the first repeat, so of the first name repeated in that
// order, the first two different paths are kept. It names a directory
// with a slash.
static void note_repeat(zip *z, zfile *first, s8 path, s8 name, b32 dir,
                        arena scratch)
{
    zdup *r     = z->dups;
    b32   seen  = r[0].name.s != 0;
    i32   cmp   = seen ? namecmp(z, name, r[0].name) : -1;
    b32   slash = dir && path.len && !is_sep(z, path.s[path.len-1]);
    s8    full  = JOIN(&scratch, path, slash ? S("/") : S(""));
    if (cmp > 0) {
        return;
    } else if (!cmp) {
        // This path may be one of the two kept, given again (a ./a ./a)
        s8 p = trim_path(z, path, &scratch);
        if (namecmp(z, full, r[1].path) >= 0 ||
            zequals(p, trim_path(z, r[0].path, &scratch)) ||
            zequals(p, trim_path(z, r[1].path, &scratch))) {
            return;
        }
    } else {
        // A new first repeat: the file that first had the name, and this
        s8 p = first->path;
        slash = first->info.type==FT_DIR && p.len && !is_sep(z, p.s[p.len-1]);
        r[0] = (zdup){first->name, JOIN(&z->perm, p, slash ? S("/") : S(""))};
    }
    r[1] = (zdup){JOIN(&z->perm, name), JOIN(&z->perm, full)};
    if (namecmp(z, r[1].path, r[0].path) < 0) {
        zdup d = r[0];
        r[0] = r[1];
        r[1] = d;
    }
}

// Fail on the repeat noted, warning of it as Info-ZIP does, once all
// paths are scanned and matched: in one warning, whose lines it indents
// by 21 spaces, to line up past its "\tzip warning: ".
static i32 repeated(zip *z, arena scratch)
{
    s8 in  = S("\n                     ");
    s8 j   = JOIN(&scratch, in, S("this may be a result of using -j"));
    s8 msg = JOIN(&scratch,
        S("  first full name: "), z->dups[0].path, in,
        S(" second full name: "), z->dups[1].path, in,
        S("name in zip file repeated: "), z->dups[1].name,
        z->junk ? j : S(""));
    warn(z, msg, S(""), scratch);
    return fail(z, ZE_PARMS, S("Invalid command arguments"),
                S("cannot repeat names in zip file"), scratch);
}

// Add a file under its archive name, which -i and -x see whole, before
// -j junks its directories, as in Info-ZIP. Also as there, a file whose
// name is the archive's path is left out silently even if it is another
// file, such as one that -j names so (zip -j dist.zip build/dist.zip),
// though here before repeats are looked for, as for excluded files.
static void add_file(zip *z, s8 path, s8 name, os_info *info, arena scratch)
{
    if (is_archive(z, path, info, scratch)) {
        return;  // the archive itself
    } else if (!name.len || !selected(z, name)) {
        return;
    }
    name = z->junk ? basename(z, name) : name;
    if (names_archive(z, name)) {
        return;
    } else if (name.len > ZIP_MAX16) {
        // Possible in deep Windows paths, but not in zip headers
        warn(z, S("name too long for a zip entry: "), path, scratch);
        z->status = ZE_OPEN;
        z->nskipped++;
        z->bskipped += info->type==FT_FILE ? info->size : 0;
        return;
    }

    // The same path reached twice (f f, d d/a, d d/) is skipped, as
    // Info-ZIP does. It compares paths whole, but gives directories
    // their slashes itself, and as tugz collapses doubled slashes in
    // names, it does in paths too. On Windows, names and paths that
    // differ only in case are the same (D/A.txt d/a.txt).
    iz *seen = zmap_upsert(&z->names, name, name_fold(z), 0);
    if (seen) {
        zfile *first = z->files.data[*seen];
        if (!zequals(trim_path(z, first->path, &scratch),
                     trim_path(z, path, &scratch))) {
            note_repeat(z, first, path, name, info->type==FT_DIR, scratch);
        }
        return;
    }

    // Only now, recorded, does the file take any permanent memory, its
    // name its key too
    keep_names(&z->perm, &path, &name);
    *zmap_upsert(&z->names, name, name_fold(z), &z->perm) = z->files.len;
    zfile *f = new_file(z, path, name, info);
    *push(&z->perm, &z->files) = f;
}

// A directory being scanned, within those it is in
typedef struct zdir zdir;
struct zdir {
    zdir       *up;
    s8          path;
    s8          sep;    // to join to path
    s8          name;   // ending in a slash, unless empty
    os_info    *info;
    os_dirent **kids;   // sorted
    iz          nkids;
    iz          next;   // kid to scan next
    arena       base;   // scratch once listed, for each kid to start from
};

// Length of a pattern's part before its first wildcard, which every
// name it matches begins with (ignoring case where it does).
static iz literal_len(zip *z, s8 pat)
{
    b32 sets = !z->nowild && !z->windows;  // and backslash escapes
    iz  k    = 0;
    for (; k < pat.len; k++) {
        u8 c = pat.s[k];
        if (c=='?' || (!z->nowild && c=='*') ||
            (sets && (c=='[' || c=='\\'))) {
            break;
        }
    }
    return k;
}

// Whether two names agree in their first n bytes, as patterns compare.
static b32 same_start(zip *z, s8 a, s8 b, iz n)
{
    i32 flags = match_flags(z);
    for (iz i = 0; i < n; i++) {
        if (zip_fold(a.s[i], flags) != zip_fold(b.s[i], flags)) {
            return 0;
        }
    }
    return 1;
}

// Whether -x or -i leave out every name below a directory, by its name
// ending in a slash, so that nothing is lost by failing to list it: an
// -x pattern of a leading part of the name and then only stars matches
// them all, and an -i pattern whose part before any wildcard parts from
// the name matches none, nor does an empty one (as from "-i="). Other
// patterns may match some or none.
static b32 subtree_out(zip *z, s8 dname)
{
    for (iz i = 0; i < z->exclude.len; i++) {
        s8 x = z->exclude.data[i];
        iz k = literal_len(z, x);
        iz e = k;
        for (; e<x.len && x.s[e]=='*' && !z->nowild; e++) {}
        if (e>k && e==x.len && k<=dname.len && same_start(z, x, dname, k)) {
            return 1;
        }
    }
    for (iz i = 0; i < z->include.len; i++) {
        s8 p = z->include.data[i];
        iz k = MIN(literal_len(z, p), dname.len);
        if (p.len && same_start(z, p, dname, k)) {
            return 0;
        }
    }
    return z->include.len > 0;
}

// Whether -x or -i leave out a path met while recursing that could not
// be examined, so that nothing is lost by passing over it, as Info-ZIP
// does whatever the patterns. A link, as a dangling one, is named as a
// file, and anything else might be a directory.
static b32 unseen_out(zip *z, s8 path, s8 name, arena scratch)
{
    os_info info = {0};
    if (included(z, name)) {
        return 0;
    } else if (os_stat(z->ctx, path, 0, &info, scratch) &&
               info.type==FT_LINK) {
        return 1;
    }
    return subtree_out(z, JOIN(&scratch, name, S("/")));
}

// Add a directory and, with -r, list it, to scan its entries next.
// Returns the innermost directory being scanned.
static zdir *enter(zip *z, zdir *up, s8 path, s8 name, os_info *info,
                   arena *scratch)
{
    s8 dname = name;
    if (name.len && name.s[name.len-1]!='/') {
        dname = JOIN(scratch, name, S("/"));
    }
    add_file(z, path, dname, info, *scratch);
    if (!z->recurse) {
        return up;
    }

    for (zdir *d = up; d; d = d->up) {
        if (same_file(d->info, info)) {
            warn(z, S("skipping directory loop: "), path, *scratch);
            return up;
        }
    }

    iz         n    = 0;
    os_dirent *list = os_listdir(z->ctx, path, z->hidden, &n, scratch);
    if (!list && !subtree_out(z, dname)) {
        warn(z, S("could not read directory: "), path, *scratch);
        z->status = ZE_OPEN;
    }
    if (!list) {
        return up;
    }
    // Entries join the directory's path with a separator, unless it ends
    // in one, but those of "." are named by theirs alone, as Info-ZIP's
    // procname names them, so that "d/a" is a path met again, not "./d/a".
    // Nor does a bare drive (Windows), its current directory, take one,
    // which would join them at its root ("C:" listed, "C:/a" opened).
    u8    drive = path.len==2 && path.s[1]==':' ? (u8)(path.s[0] | 0x20) : 0;
    b32   bare  = z->windows && drive>='a' && drive<='z';
    b32   dot   = zequals(path, S("."));
    b32   join  = !dot && !bare && path.len && !is_sep(z, path.s[path.len-1]);
    zdir *d     = new(scratch, 1, zdir);
    d->up    = up;
    d->path  = dot ? S("") : path;
    d->sep   = join ? S("/") : S("");
    d->name  = dname;
    d->info  = info;
    d->kids  = zsort(list, n, scratch);
    d->nkids = n;
    d->base  = *scratch;
    return d;
}

// The path of a directory's entry, and through name, its name, each
// followed in memory by a slash, which a directory's name takes. Both
// are one string where the name ends the path, as it usually does ("d"
// and "d/", "./d" and "d/"), so that a deep tree's levels each keep one
// copy of their path while their entries are scanned rather than three.
static s8 kid_path(arena *a, zdir *dir, s8 kid, s8 *name)
{
    s8 full = JOIN(a, dir->path, dir->sep, kid, S("/"));
    iz at   = full.len - 1 - kid.len - dir->name.len;  // name's start
    if (at>=0 && zequals((s8){full.s+at, dir->name.len}, dir->name)) {
        *name = (s8){full.s+at, dir->name.len+kid.len};
    } else {
        *name = JOIN(a, dir->name, kid, S("/"));
        name->len--;
    }
    return (s8){full.s, full.len-1};
}

// Warn that a special file is left out, as Info-ZIP words it, but for its
// advice to read a FIFO with -FI, which tugz does not support
static void ignore_special(zip *z, s8 path, os_info *info, arena scratch)
{
    b32 fifo = (info->mode & 0170000) == 0010000;  // S_IFIFO (POSIX)
    s8  msg  = fifo ? S("ignoring FIFO (Named Pipe): ")
             : S("ignoring special file: ");
    warn(z, msg, path, scratch);
}

// Scan a path and, with -r, everything under it, in sorted order. The
// directories being listed form a stack in scratch rather than on the
// call stack, which a deep enough tree would overflow: Windows paths
// reach 32K characters, while its 2 MiB stack held 5,000 levels, not
// 8,000, when this recursed.
static void scan(zip *z, s8 path, s8 name, os_info *info, arena scratch)
{
    zdir *dir = 0;  // innermost
    for (;;) {
        switch (info->type) {
        case FT_FILE:
        case FT_LINK:
            add_file(z, path, name, info, scratch);
            break;
        case FT_DIR:
            dir = enter(z, dir, path, name, info, &scratch);
            break;
        default:
            ignore_special(z, path, info, scratch);
        }

        // Then the next entry of the innermost directory with any left,
        // forgetting what scanning the one before allocated
        for (;;) {
            for (; dir && dir->next==dir->nkids; dir = dir->up) {}
            if (!dir) {
                return;
            }
            scratch = dir->base;
            os_dirent *k = dir->kids[dir->next++];
            path = kid_path(&scratch, dir, k->name, &name);
            info = &k->info;
            if (listed(z, info) ||
                os_stat(z->ctx, path, !z->symlinks, info, scratch)) {
                name.len += info->type == FT_DIR;  // its slash, for enter
                break;
            } else if (!unseen_out(z, path, name, scratch)) {
                warn(z, S("could not open for reading: "), path, scratch);
                z->status = ZE_OPEN;
                z->nskipped++;
            }
        }
    }
}

// Archive name for a path named on the command line. A directory's is
// that of its path with a separator, as Info-ZIP's procname names it, so
// that a share root "//server/share" is named, as "//server/share/" is,
// by nothing, and the names of its entries drop that prefix too.
static s8 arg_name(zip *z, s8 path, os_info *info, arena *a)
{
    path = info->type==FT_DIR ? JOIN(a, path, S("/")) : path;
    return zip_name(a, path, z->windows);
}

// Whether a file is hidden or system (Windows), which Info-ZIP leaves
// out unless -S, even when named. A directory is still scanned.
static b32 hidden_file(zip *z, os_info *info)
{
    return z->windows && !z->hidden && info->type!=FT_DIR &&
           (info->attr & 0x06);
}

// Scan a wildcard argument's match (expand_wild) as an argument: as its
// listing entry describes it, if that does, else once examined. Returns
// whether it was scanned.
static b32 scan_match(void *data, s8 path, os_dirent *entry, arena scratch)
{
    zip    *z    = data;
    os_info info = entry ? entry->info : (os_info){0};
    if ((entry && listed(z, &info)) ||
        os_stat(z->ctx, path, !z->symlinks, &info, scratch)) {
        s8 name = arg_name(z, path, &info, &scratch);
        scan(z, path, name, &info, scratch);
        return 1;
    }
    return 0;
}

// Scan a path on disk under a name: a caller's (zip_call), whose entries
// are named from it, else the one that a path named on the command line
// takes. A hidden or system file named on the command line is left out
// unless -S, as in Info-ZIP, but not one that a caller chose. Returns
// false if it is not on disk.
static b32 scan_named(zip *z, s8 path, s8 name, b32 chosen, arena scratch)
{
    os_info info = {0};
    if (!os_stat(z->ctx, path, !z->symlinks, &info, scratch)) {
        return 0;
    } else if (chosen || !hidden_file(z, &info)) {
        name = name.s ? name : arg_name(z, path, &info, &scratch);
        scan(z, path, name, &info, scratch);
    }
    return 1;
}

// Scan a path argument. Returns false if it is not on disk, for it to
// be matched against the archive's entries, as Info-ZIP's procname
// does. Its Windows port does so only when freshening: otherwise it
// expands wildcards on disk, as Windows shells do not, and stops there.
static b32 scan_arg(zip *z, s8 arg, arena scratch)
{
    if (scan_named(z, arg, (s8){0}, 0, scratch)) {
        return 1;
    } else if (!z->windows || z->mode==MODE_FRESHEN) {
        return 0;
    } else if (!zip_haswild(arg, 0) ||
               !expand_wild(z->ctx, arg, z->hidden,
                            WILD_WINDOWS | (z->nowild ? ZIP_NOWILD : 0),
                            scan_match, z, scratch)) {
        warn(z, S("name not matched: "), arg, scratch);
    }
    return 1;
}

// The existing archive

// An entry's Unicode name, as zar_uname finds it, warning as Info-ZIP
// does of a field that does not check out.
static s8 entry_uname(zip *z, zentry *e, arena *perm, arena scratch)
{
    s8 stale = {0};
    s8 u     = zar_uname(z->ctx, e, z->windows, &z->crccpu, &stale, perm,
                         scratch);
    if (stale.s) {
        warn(z, S("Unicode does not match path - ignoring Unicode: "),
             stale, scratch);
    }
    return u;
}

static i32 not_zip(zip *z, arena scratch)
{
    warn(z, S("missing end signature--probably not a zip file"), S(""),
         scratch);
    return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                scratch);
}

// Fail, just after a read from the archive failed (os_readat's result
// r), as Info-ZIP does: as an I/O error (11), or at its end, which comes
// early only if it has shrunk since it was examined, as an unexpected end
// (2), either naming any entry being copied, else the archive.
static i32 read_failed(zip *z, i32 r, s8 copying, arena scratch)
{
    s8 arg = z->archive;
    if (copying.s) {
        arg = JOIN(&scratch, S("was copying "), copying);
    }
    if (r < 0) {
        return fail(z, ZE_READ, S("Input file read failure"), arg, scratch);
    }
    return fail(z, ZE_EOF, S("Unexpected end of zip file"), arg, scratch);
}

// Returns an exit status on failure, or -1 for an archive that is to be
// taken for a missing one.
static i32 read_archive(zip *z, zarchive *ar, arena scratch)
{
    if (z->arcinfo.type != FT_FILE) {
        // A directory or device, which Info-ZIP reads as an empty file,
        // or a FIFO, on which it waits for data
        return not_zip(z, scratch);
    }

    i32 fd = os_open(z->ctx, z->target, OS_READ|OS_REGULAR, scratch);
    if (fd < 0) {
        // One that cannot be written either is, as in Info-ZIP, taken
        // for a missing archive, which then cannot be written
        s8 why = fd==OS_ERR ? os_error(z->ctx) : S("");
        if (!os_writable(z->ctx, z->target, scratch)) {
            return -1;
        }
        return fail_why(z, ZE_READ, why, S("Could not open archive"),
                        z->archive, scratch);
    }

    switch (zar_read(ar, z->ctx, fd, z->arcinfo.size, &z->perm, scratch)) {
    case ZAR_OK:
        break;
    case ZAR_EREAD:
        return read_failed(z, -1, (s8){0}, scratch);
    case ZAR_EEOF:
        return read_failed(z, 0, (s8){0}, scratch);
    case ZAR_ENOEND:
        return not_zip(z, scratch);
    case ZAR_EFORMAT:
        if (ar->found >= 0) {
            // As Info-ZIP warns of a count that wrapped, or any other
            s8 msg = JOIN(&scratch, S("expected "),
                          znum(&scratch, ar->end.count),
                          S(" entries but found "));
            warn(z, msg, znum(&scratch, ar->found), scratch);
        }
        return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                    scratch);
    case ZAR_EMULTI:
        return fail(z, ZE_FORM, S("Split archives not supported"),
                    z->archive, scratch);
    case ZAR_EPREFIX:
        // As a stub prepended to an archive leaves them, which Info-ZIP
        // finds only on copying an entry ("Did not find entry for")
        warn(z, S("offsets do not account for data before the archive"),
             S(""), scratch);
        return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                    scratch);
    case ZAR_ENONAME:
        warn(z, S("zero-length name for entry #"),
             znum(&scratch, ar->noname+1), scratch);
        return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                    scratch);
    }

    for (i64 i = 0; i < ar->end.count; i++) {
        s8 u = entry_uname(z, ar->entries+i, &z->perm, scratch);
        if (u.s) {
            *push(&z->perm, &ar->unames) = (zuname){(iz)i, u};
        }
    }
    return 0;
}

// Output: buffered positioned writes into the temporary file

typedef struct {
    os     *ctx;
    arena  *perm;  // for why
    i32     fd;
    u8     *buf;
    iz      len;
    iz      cap;
    i64     pos;   // file offset of buf[0]
    b32     err;
    s8      why;   // of the error, as it occurred
    writer *msgs;  // standard output, polled while copying
} zout;

static void zout_writeat(zout *w, u8 *p, iz n, i64 off)
{
    if (!w->err && !os_writeat(w->ctx, w->fd, p, n, off)) {
        w->err = 1;
        w->why = JOIN(w->perm, os_error(w->ctx));
    }
}

static void zout_flush(zout *w)
{
    if (w->len) {
        zout_writeat(w, w->buf, w->len, w->pos);
    }
    w->pos += w->len;
    w->len  = 0;
}

static void zout_write(zout *w, u8 const *p, iz n)
{
    while (n) {
        if (w->len == w->cap) {
            zout_flush(w);
        }
        iz take = MIN(n, w->cap-w->len);
        bytecopy(w->buf+w->len, p, take);
        w->len += take;
        p += take;
        n -= take;
    }
}

static i64 zout_tell(zout *w)
{
    return w->pos + w->len;
}

// Move the write position, such as back to the start of an entry. What
// followed is dropped if still buffered, else overwritten or truncated.
static void zout_seek(zout *w, i64 off)
{
    if (off>=w->pos && off-w->pos<=w->len) {
        w->len = (iz)(off - w->pos);
        return;
    }
    zout_flush(w);
    w->pos = off;
}

// Rewrite bytes already written, such as a local header.
static void zout_patch(zout *w, i64 off, u8 *p, iz n)
{
    if (off>=w->pos && off-w->pos<=w->len-n) {
        bytecopy(w->buf+(off-w->pos), p, n);  // still buffered
        return;
    }
    zout_flush(w);
    zout_writeat(w, p, n, off);
}

// Copy len bytes at off in the existing archive to the output, through
// its window, and so a window at a time when larger. Returns as
// os_readat.
static i32 zin_copy(zin *r, zout *w, i64 off, i64 len)
{
    while (len > 0) {
        writer_poll(w->msgs);
        b32 hit = off>=r->pos && off-r->pos<r->len;
        i32 got = hit ? 1 : zin_fill(r, off, (iz)MIN(len, r->cap));
        if (got <= 0) {
            return got;
        }
        iz at   = (iz)(off - r->pos);
        iz take = (iz)MIN(len, r->len-at);
        zout_write(w, r->buf+at, take);
        off += take;
        len -= take;
    }
    return 1;
}

// Entry data source: a file, or memory (a symbolic link's target)

typedef struct {
    s8       path;
    os_info *info;  // as scanned
    s8       mem;
    iz       off;
    i32      fd;
    b32      err;
    s8       why;      // of the first read error, as it occurred
    b32      changed;  // not opened, being no longer the file scanned
    b32      byentry;  // as its zfile's
} zsrc;

// Open a file to read as the scan found it: not a FIFO swapped in since,
// which would block. Under -y, which stores links as links, not a link
// swapped in either, nor any other file, as through a directory swapped
// for a link since, so that nothing is read through a link swapped in
// after the scan (Info-ZIP examines each file again just before reading
// it). The scan goes by path, as Info-ZIP's does, so one swapped in for
// a directory while the scan is in it still leads it, and this, astray.
// Without -y, links are followed anyway, and a file saved since by
// renaming over it is read as it now is, but for a file that an entry
// names, which the scan refuses as a link (scan_entries): nor is it read
// through one swapped in for it since.
static b32 src_open(zip *z, zsrc *s, arena scratch)
{
    s->off = 0;
    s->err = 0;
    if (s->mem.s) {
        return 1;
    }
    b32 nofollow = z->symlinks || s->byentry;
    i32 mode     = OS_READ | OS_REGULAR | (nofollow ? OS_NOFOLLOW : 0);
    s->fd = os_open(z->ctx, s->path, mode, scratch);
    s->changed = s->fd<0 && s->fd!=OS_ERR;  // a different type
    os_info now  = {0};
    b32     seen = s->info->ino[0] || s->info->ino[1];
    if (s->fd>=0 && z->symlinks && seen &&
        (!os_fstat(z->ctx, s->fd, &now) || !same_file(&now, s->info))) {
        os_close(z->ctx, s->fd);
        s->fd = -1;
        s->changed = 1;
    }
    return s->fd >= 0;
}

static void src_close(zip *z, zsrc *s)
{
    if (!s->mem.s && s->fd>=0) {
        os_close(z->ctx, s->fd);
        s->fd = -1;
    }
}

static iz src_read(zip *z, zsrc *s, u8 *buf, iz cap)
{
    if (s->mem.s) {
        iz n = MIN(cap, s->mem.len-s->off);
        bytecopy(buf, s->mem.s+s->off, n);
        s->off += n;
        return n;
    }
    iz n = os_read(z->ctx, s->fd, buf, cap);
    if (n<0 && !s->err) {
        s->err = 1;
        s->why = JOIN(&z->perm, os_error(z->ctx));
    }
    return n<0 ? 0 : n;
}

typedef struct {
    zout     *out;
    u8       *buf;     // input buffer
    iz        cap;
    deflator *def;     // reset for each entry
    arena     extras;  // new entries' central extra fields
    s8        why;     // of a failed read of an entry's input
} zwork;

static void store_data(zip *z, zwork *k, zsrc *s, u32 *crc, i64 *usize)
{
    if (s->mem.s) {
        // Written directly, as it may be the input buffer itself
        *crc = crc32_update(*crc, s->mem.s, s->mem.len, &z->crccpu);
        *usize += s->mem.len;
        zout_write(k->out, s->mem.s, s->mem.len);
        return;
    }
    for (iz n; (n = src_read(z, s, k->buf, k->cap));) {
        *crc = crc32_update(*crc, k->buf, n, &z->crccpu);
        *usize += n;
        zout_write(k->out, k->buf, n);
        writer_poll(z->out);
    }
}

// Returns whether the first read got all the input, which is therefore
// still in the input buffer.
static b32 deflate_data(zip *z, zwork *k, zsrc *s, u32 *crc, i64 *usize)
{
    deflator *d = k->def;
    deflate_reset(d);
    iz first = -1;
    for (b32 more = 1; more;) {
        iz n = src_read(z, s, k->buf, k->cap);
        more = n > 0;
        first = first<0 ? n : first;
        *crc = crc32_update(*crc, k->buf, n, &z->crccpu);
        *usize += n;
        zbuf b = {k->buf, n, 0, 0};
        for (;;) {
            i32 r = deflate_stream(d, &b, more ? DEF_NONE : DEF_FINISH);
            s8  p = deflate_pending(d);
            zout_write(k->out, p.s, p.len);
            deflate_consume(d, p.len);
            if (r != GZ_NEEDOUT) {
                break;
            }
        }
        writer_poll(z->out);
    }
    return *usize == first;
}

// Unix time as a 32-bit field, as Info-ZIP stores it, but clamped to
// the range UnZip reads: signed when the DOS time is its minimum (1901
// to 1970, read only where time_t is 32 bits), unsigned from 2038 (to
// 2106), where Info-ZIP wraps.
static u8 *put_time(u8 *p, zip *z, i64 t)
{
    t = z->haveepoch ? MIN(t, z->epoch) : t;
    t = MAX(MIN(t, (i64)0xffffffff), -(i64)0x80000000);
    return put32(p, (u32)t);
}

// Info-ZIP's extended timestamp and Unix ownership extra fields: the
// local ones, needed only until the local header is written, from
// scratch, and the central ones from k->extras, set aside before any
// output, at most CEXTRA_MAX bytes for each new entry.
enum { CEXTRA_MAX = 9 + 15 };
static void file_extras(zip *z, zwork *k, zfile *f, zentry *e,
                        arena *scratch)
{
    if (z->noextra) {
        return;
    }
    u8 *l = newstr(scratch, 13+15);
    u8 *c = newstr(&k->extras, z->windows ? 9 : CEXTRA_MAX);
    e->lextra.s = l;
    e->cextra.s = c;

    // Under SOURCE_DATE_EPOCH the access time is the epoch itself, as
    // reading the file may change an earlier one (Linux's relatime)
    l = put16(l, ZIP_EXTRA_TIME);
    l = put16(l, 9);
    *l++ = 3;  // modification and access times
    l = put_time(l, z, f->info.mtime);
    l = put_time(l, z, z->haveepoch ? z->epoch : f->info.atime);
    c = put16(c, ZIP_EXTRA_TIME);
    c = put16(c, 5);
    *c++ = 3;
    c = put_time(c, z, f->info.mtime);

    if (!z->windows) {
        for (i32 i = 0; i < 2; i++) {
            u8 **p = i ? &c : &l;
            *p = put16(*p, ZIP_EXTRA_UNIX);
            *p = put16(*p, 11);
            *(*p)++ = 1;  // version
            *(*p)++ = 4;
            *p = put32(*p, f->info.uid);
            *(*p)++ = 4;
            *p = put32(*p, f->info.gid);
        }
    }
    e->lextra.len = l - e->lextra.s;
    e->cextra.len = c - e->cextra.s;
}

static u32 file_extattr(zip *z, os_info *info)
{
    b32 dir = info->type == FT_DIR;
    if (z->windows) {
        return (info->attr & 0x37) | (dir ? 0x10 : 0);
    }
    b32 ro = !(info->mode & 0200);
    return info->mode<<16 | (dir ? 0x10 : 0) | (ro ? 0x01 : 0);
}

// Version made by: FAT or Unix, Zip 3.0
static u16 made_by(zip *z)
{
    return z->windows ? 0x001e : 0x031e;
}

static u16 level_flags(i32 level)
{
    return level>=8 ? 2 : level<=2 ? 4 : 0;
}

// Whether a path ends in a suffix of Info-ZIP's default -n list, of
// files already compressed, matched as there: ignoring case on Windows.
static b32 store_suffix(zip *z, s8 path)
{
    static s8 const suffixes[] = {
        S8(".Z"), S8(".zip"), S8(".zoo"), S8(".arc"), S8(".lzh"), S8(".arj"),
    };
    i32 fold = z->windows ? ZIP_FOLD : 0;
    for (iz i = 0; i < countof(suffixes); i++) {
        s8  s     = suffixes[i];
        b32 match = path.len >= s.len;
        for (iz j = 0; match && j < s.len; j++) {
            u8 c = path.s[path.len-s.len+j];
            match = zip_fold(c, fold) == zip_fold(s.s[j], fold);
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

enum {
    WRITE_OK,
    WRITE_EOPEN,     // the system refused (os_error tells why)
    WRITE_ECHANGED,  // no longer the file scanned, so not opened
    WRITE_EREAD,     // the system failed a read (zwork's why tells why)
    WRITE_EDIRFILE,
    WRITE_ELINK,     // an entry's file is now a link (without -y)
};

// Compress a new entry. Returns WRITE_OK, or, having left the output
// where it began, why its input could not be read.
static i32 write_file(zip *z, zwork *k, zfile *f, zentry *e, arena scratch)
{
    zout *w = k->out;
    i64 start = zout_tell(w);

    i32 utf8 = zip_utf8(f->name);
    if (utf8 < 0 && z->windows) {
        warn(z, S("name is not valid UTF-8: "), f->path, scratch);
    }
    *e = (zentry){0};  // the slot may hold a failed attempt's fields
    e->name    = f->name;
    e->made    = made_by(z);
    e->flags   = utf8>0 ? ZIP_FLAG_UTF8 : 0;
    e->dostime = f->dostime;
    e->extattr = file_extattr(z, &f->info);
    e->offset  = start;
    e->zip64   = f->info.type==FT_FILE && f->info.size>=ZIP_MAX32;
    file_extras(z, k, f, e, &scratch);

    zsrc src = {0};
    src.path    = f->path;
    src.info    = &f->info;
    src.fd      = -1;
    src.byentry = f->byentry;
    if (f->info.type == FT_DIR) {
        u8 *h = newbytes(&scratch, zip_local_len(e));
        zout_write(w, h, zip_local(h, e)-h);
        return WRITE_OK;
    } else if (f->info.type == FT_LINK) {
        // Read by path, then found to be the link the scan found, as a
        // file is when opened, rather than one swapped in since
        src.mem = os_readlink(z->ctx, f->path, &scratch);
        if (!src.mem.s) {
            return WRITE_EOPEN;
        }
        os_info now  = {0};
        b32     seen = f->info.ino[0] || f->info.ino[1];
        if (seen && (!os_stat(z->ctx, f->path, 0, &now, scratch) ||
                     !same_file(&now, &f->info))) {
            return WRITE_ECHANGED;
        }
    }

    // As in Info-ZIP, only regular files with data may be compressed:
    // links are always stored, and so, below -9, are files named with
    // its default -n suffixes, without trying
    b32 tryz = z->level>0 && f->info.type==FT_FILE && f->info.size &&
               (z->level==9 || !store_suffix(z, f->path));
    if (tryz) {
        e->flags |= level_flags(z->level);
    }

    for (;;) {
        e->method = tryz ? ZIP_DEFLATE : ZIP_STORE;
        e->crc    = 0;
        e->usize  = 0;
        zout_seek(w, start);  // drop an abandoned attempt, even on failure
        if (!src_open(z, &src, scratch)) {
            b32 link = src.fd==OS_ESYMLINK && !z->symlinks;
            return link ? WRITE_ELINK :
                   src.changed ? WRITE_ECHANGED : WRITE_EOPEN;
        }

        iz  hlen = zip_local_len(e);
        u8 *h    = newbytes(&scratch, hlen);
        zip_local(h, e);
        zout_write(w, h, hlen);
        i64 data = zout_tell(w);

        b32 buffered = 0;
        if (tryz) {
            buffered = deflate_data(z, k, &src, &e->crc, &e->usize);
        } else {
            store_data(z, k, &src, &e->crc, &e->usize);
        }
        e->csize = zout_tell(w) - data;
        src_close(z, &src);
        if (src.err) {
            k->why = src.why;
            zout_seek(w, start);
            return WRITE_EREAD;
        }

        if (tryz && e->csize>=e->usize) {
            tryz = 0;  // compression did not help: store instead
            if (buffered) {
                src.mem = (s8){k->buf, (iz)e->usize};  // no need to reread
            }
            continue;
        } else if (!e->zip64 && e->usize>=ZIP_MAX32) {
            e->zip64 = 1;  // grew past 4 GiB while reading
            continue;
        }
        zip_local(h, e);
        zout_patch(w, start, h, hlen);
        return WRITE_OK;
    }
}

// Warn, as Info-ZIP does on copying an entry, of a local header that
// disagrees with the central one, from which the one written is made:
// in its version needed, flags, CRC (unless a descriptor follows), or
// name, in the order and words of Info-ZIP's warnings.
static void check_local(zip *z, u8 const *loc, s8 lname, zentry *e, s8 name,
                        arena scratch)
{
    s8  what[] = {S("Version Needed To Extract"), S("Entry Flag"),
                  S("Entry CRC"), S("Entry name")};
    b32 bad[]  = {get16(loc+4) != e->needed, get16(loc+6) != e->flags,
                  !(e->flags & ZIP_FLAG_DESCRIPTOR) && get32(loc+14)!=e->crc,
                  !zequals(lname, e->name)};
    for (i32 i = 0; i < countof(bad); i++) {
        if (bad[i]) {
            s8 msg = JOIN(&scratch, S("Local "), what[i],
                          S(" does not match CD: "));
            warn(z, msg, name, scratch);
        }
    }
}

// Copy an entry from the existing archive without recompressing it,
// updating it in place for the central directory, and given a rename,
// under that name (zip_call), checked against its local header as it
// was, then flagged UTF-8 if it is UTF-8 and not ASCII, as new names are.
// The Unicode path field of its old name goes, from the local extra
// fields here, as from its central ones when planned (plan_changes).
static i32 copy_entry(zip *z, zarchive *ar, zwork *k, zentry *e, s8 rename,
                      arena scratch)
{
    zout  *w    = k->out;
    zin   *r    = &ar->in;
    s8     name = shown_name(ar, e);
    zlocal loc  = {0};
    switch (zar_local(ar, e, &loc)) {
    case ZAR_OK:
        break;
    case ZAR_EREAD:
        return read_failed(z, -1, name, scratch);
    case ZAR_EEOF:
        return read_failed(z, 0, name, scratch);
    default:
        return fail(z, ZE_FORM, S("Zip file structure invalid"), e->name,
                    scratch);
    }
    check_local(z, loc.fixed, loc.name, e, name, scratch);

    // Its extra fields are kept, as Info-ZIP keeps them even with -X,
    // except that Zip64 fields are made anew. Those must leave room.
    e->lextra = zip_filter_extra(&scratch, loc.extra);
    if (rename.s) {
        u16 utf8 = zip_utf8(rename)>0 ? ZIP_FLAG_UTF8 : 0;
        e->name   = rename;
        e->flags  = (u16)((e->flags & ~ZIP_FLAG_UTF8) | utf8);
        e->lextra = zip_drop_extra(e->lextra, ZIP_EXTRA_UPATH);
    }
    e->offset = zout_tell(w);
    e->zip64  = e->usize>=ZIP_MAX32 || e->csize>=ZIP_MAX32;
    if (!zip_fits(e)) {
        s8 why = JOIN(&scratch, e->name, S(": no room for Zip64 fields"));
        return fail(z, ZE_FORM, S("Zip file structure invalid"), why,
                    scratch);
    }

    // Sizes are now known, so a descriptor is unnecessary, but as in
    // Info-ZIP, any encrypted entry (flag bit 0) keeps it, as traditional
    // encryption checks against the time when it is present (other kinds,
    // such as AES, keep it harmlessly).
    u16  both = ZIP_FLAG_ENCRYPTED | ZIP_FLAG_DESCRIPTOR;
    b32  desc = (e->flags & both) == both;
    if (!desc) {
        e->flags &= ~ZIP_FLAG_DESCRIPTOR;
    }

    u8 *h = newbytes(&scratch, zip_local_len(e));
    zout_write(w, h, zip_local(h, e)-h);
    i32 got = zin_copy(r, w, loc.data, e->csize);
    if (got <= 0) {
        return read_failed(z, got, name, scratch);
    }
    if (desc) {
        u8 d[24];
        zout_write(w, d, zip_desc(d, e)-d);
    }
    e->lextra = (s8){0};  // in scratch, and written
    return 0;
}

// ITEM_RENAME keeps its entry, copied under the name of its file, a
// record of no file on disk (zip_call), so that only a renamed entry
// takes more memory for it.
enum { ITEM_KEEP, ITEM_ADD, ITEM_UPDATE, ITEM_FRESHEN, ITEM_DELETE,
       ITEM_RENAME };

typedef struct {
    i32     kind;
    zentry *old;
    zfile  *file;
} zitem;

typedef struct {
    zitem *data;
    iz     len;
    iz     cap;
} zitems;

// Print a progress line, with the compression result for a new entry.
static void report(zip *z, s8 verb, s8 name, zentry *e, arena scratch)
{
    if (z->quiet) {
        return;
    }
    s8 how = S("");
    if (e) {
        b32 stored = e->method == ZIP_STORE;
        i32 pct = stored ? 0 : zip_percent(e->usize, e->csize);
        how = JOIN(&scratch, S(" ("), stored ? S("stored ") : S("deflated "),
                   znum(&scratch, pct), S("%)"));
    }
    say(z, 1, JOIN(&scratch, verb, name, how, S("\n")));
}

// Info-ZIP's summary of files and entries read, when any were skipped.
static void summarize_reads(zip *z, arena scratch)
{
    if (z->nskipped && !z->quiet) {
        s8 msg = JOIN(&scratch,
            S("\nzip warning: Not all files were readable\n"),
            S("  files/entries read:  "), znum(&scratch, z->nread),
            S(" ("), zbytes(&scratch, z->bread), S(" bytes)"),
            S("  skipped:  "), znum(&scratch, z->nskipped),
            S(" ("), zbytes(&scratch, z->bskipped), S(" bytes)\n"));
        say(z, 2, msg);
    }
}

// Temporary file beside the archive, past any links, so that it can be
// renamed over it, created discarded-on-close. For a new archive it has
// the permissions of a new file from the start. On Windows, beside a
// drive-relative "X:name" is in "X:", that drive's current directory,
// which need not be the current directory.
static i32 create_temp(zip *z, s8 *path, arena scratch)
{
    s8 a     = z->target;
    u8 drive = a.len>=2 && a.s[1]==':' ? (u8)(a.s[0] | 0x20) : 0;
    iz root  = z->windows && drive>='a' && drive<='z' ? 2 : 0;
    iz cut   = a.len;
    for (; cut>root && !is_sep(z, a.s[cut-1]); cut--) {}
    s8 dir = {a.s, cut};
    i32 mode = OS_CREATE | (z->arcinfo.type==FT_NONE ? OS_DEFPERMS : 0);
    for (i32 i = 0; i < 1000000; i++) {
        s8 num = znum(&scratch, 1000000 + i);
        *path = JOIN(&z->perm, dir, S("zi"), (s8){num.s+1, 6});
        i32 fd = os_open(z->ctx, *path, mode, scratch);
        if (fd != OS_EEXIST) {
            return fd;
        }
    }
    return OS_ERR;
}

// Close the temporary file after a failure, which discards it, warning
// should it stay, as where its directory has since refused removing it,
// which Info-ZIP passes over in silence.
static void discard_temp(zip *z, i32 fd, s8 temp, arena scratch)
{
    os_close(z->ctx, fd);
    os_info info = {0};
    if (os_stat(z->ctx, temp, 0, &info, scratch)) {
        warn(z, S("could not remove temporary file: "), temp, scratch);
    }
}

static i32 write_archive(zip *z, zarchive *ar, zitems *items, arena scratch)
{
    // Whatever grows with the number of entries is allocated before any
    // output, so that running out of memory cannot waste the work: the
    // central directory, as pointers to entries, kept and replaced ones
    // updated in place, added ones' entries, new ones' extra fields, and
    // the buffers.
    iz nadd = 0;
    iz nnew = 0;
    for (iz i = 0; i < items->len; i++) {
        i32 kind = items->data[i].kind;
        nadd += kind == ITEM_ADD;
        nnew += kind!=ITEM_KEEP && kind!=ITEM_DELETE && kind!=ITEM_RENAME;
    }
    zentry **cd    = new(&scratch, items->len, zentry *);
    zentry  *fresh = new(&scratch, nadd, zentry);

    // The entries that may be copied (replaced ones too, which are kept
    // should their files fail) must not overlap, as UnZip finds them out
    // (Info-ZIP's zip does not), so that a small archive whose central
    // headers share their data cannot grow by copying it for each. Each
    // must hold its local header and data, as its central header gives
    // its size, before the next begins, and then, as copy_entry finds its
    // local header, what that takes too (zar_local).
    if (ar) {
        ar->spans  = new(&scratch, items->len - nadd, i64);
        ar->nspans = 0;
        for (iz i = 0; i < items->len; i++) {
            zitem *it = items->data + i;
            if (it->old && it->kind!=ITEM_DELETE) {
                ar->spans[ar->nspans++] = it->old->offset;
            }
        }
        zar_spans(ar);
        for (iz i = 0; i < items->len; i++) {
            zentry *old  = items->data[i].old;
            b32     kept = old && items->data[i].kind!=ITEM_DELETE;
            if (kept && zar_overlaps(ar, old)) {
                warn(z, S("entry overlaps another (possible zip bomb): "),
                     shown_name(ar, old), scratch);
                return fail(z, ZE_FORM, S("Zip file structure invalid"),
                            z->archive, scratch);
            }
        }
    }

    zout w = {0};
    w.ctx  = z->ctx;
    w.perm = &z->perm;
    w.msgs = z->out;
    w.cap  = 1 << 20;
    w.buf  = newbytes(&scratch, w.cap);

    zwork k = {0};
    k.out    = &w;
    k.cap    = 1 << 18;
    k.buf    = newbytes(&scratch, k.cap);
    k.extras = subarena(&scratch, z->noextra ? 0 : nnew*CEXTRA_MAX);
    if (z->level && nnew) {  // not to delete or copy only
        arena a = subarena(&scratch, deflate_memsize());
        k.def = deflate_new(&a, z->level);
    }

    // Then room for any one entry's headers, names, and messages, which
    // each is done with before the next, and for replacing the archive
    arena room = scratch;
    newbytes(&room, 1<<20);

    // As in Info-ZIP, a temporary file that cannot be created to replace
    // an archive is a temporary file failure, naming that file, and for a
    // new archive, which Info-ZIP writes in place, a failure to create it
    s8  temp = {0};
    i32 fd   = create_temp(z, &temp, scratch);
    if (fd<0 && ar) {
        return fail(z, ZE_TEMP, S("Temporary file failure"), temp, scratch);
    } else if (fd < 0) {
        return fail(z, ZE_CREAT, S("Could not create output file"),
                    z->archive, scratch);
    }
    w.fd = fd;

    // The preamble comes first, so that offsets stay absolute
    i32 got = ar ? zin_copy(&ar->in, &w, 0, ar->beg) : 1;
    if (got <= 0) {
        i32 err = read_failed(z, got, (s8){0}, scratch);  // before closing
        discard_temp(z, fd, temp, scratch);
        return err;
    }

    // Once a write fails, as Info-ZIP stops there, nothing more is done
    iz count = 0;
    for (iz i = 0, n = 0; i<items->len && !w.err; i++) {
        zitem  *it     = items->data + i;
        zentry *copy   = 0;
        s8      rename = {0};
        writer_poll(z->out);  // as each entry is added, copied, or deleted
        switch (it->kind) {
        case ITEM_DELETE:
            report(z, S("deleting: "), shown_name(ar, it->old), 0, scratch);
            break;
        case ITEM_RENAME:
            rename = it->file->name;
            copy   = it->old;
            break;
        case ITEM_KEEP:
            copy = it->old;
            break;
        default: {
            // A replacement is written over its entry, restored if the
            // file cannot be read, so that only added files take more
            zentry *old  = it->old;
            zentry  was  = old ? *old : (zentry){0};
            zentry *e    = old ? old : fresh + n++;
            zfile  *f    = it->file;
            s8      verb = it->kind==ITEM_ADD    ? S("  adding: ") :
                           it->kind==ITEM_UPDATE ? S("updating: ") :
                                                   S("freshening: ");
            // An entry selected by name may have changed between file
            // and directory, which Info-ZIP reports, keeping the entry
            i32 r = WRITE_EDIRFILE;
            if (is_dirname(f->name) == (f->info.type==FT_DIR)) {
                r = write_file(z, &k, f, e, scratch);
            }
            if (w.err) {
                // Info-ZIP stops at the write that fails, its progress
                // line begun, as here without the result
                report(z, verb, f->name, 0, scratch);
                break;
            } else if (r == WRITE_OK) {
                // A replaced entry keeps its comment, as in Info-ZIP
                e->comment = was.comment;
                report(z, verb, e->name, e, scratch);
                if (!z->windows && f->info.type==FT_FILE &&
                    e->usize!=f->info.size) {
                    // Warn, as Info-ZIP's Unix port does (not its Windows
                    // port), of data whose size differs from the file's
                    // as examined: there just before reading it, here in
                    // the scan, from which the entry's time comes
                    warn(z, S(" file size changed while zipping "), f->path,
                         scratch);
                }
                z->nread++;
                z->bread += e->usize;
                e->lextra = (s8){0};  // in write_file's scratch, and written
                cd[count++] = e;
                break;
            } else if (r==WRITE_ELINK && old) {
                // A link swapped in since the scan: the entry is left as
                // the scan leaves it for a link, as for a missing file,
                // but kept under -FS too (scan_entries)
                *old = was;
                s8 shown = f->path;  // without the ./ of Windows' lookup
                if (shown.len>2 && shown.s[0]=='.' && shown.s[1]=='/') {
                    shown = (s8){shown.s+2, shown.len-2};
                }
                warn(z, S("not following link that an entry names: "),
                     shown, scratch);
                copy = old;
                break;
            }

            // As Info-ZIP does, give the progress line, then the system's
            // reason, as its perror words it, even under -q, and warn
            // under the entry's name
            if (old) {
                *old = was;
            }
            s8 reason = r==WRITE_EOPEN ? os_error(z->ctx) :
                        r==WRITE_EREAD ? k.why : S("");
            report(z, verb, f->name, 0, scratch);
            if (reason.len) {
                arena tmp = scratch;
                s8    who = old && r==WRITE_EOPEN ? f->name : S("zip warning");
                say(z, 2, JOIN(&tmp, who, S(": "), reason, S("\n")));
            }
            s8 why = S("could not open for reading: ");
            why = r==WRITE_EREAD    ? S("could not read input file: ") :
                  r==WRITE_EDIRFILE ?
                      S("file and directory with the same name: ")  : why;
            warn(z, why, f->name, scratch);
            z->status = ZE_OPEN;
            i64 size = f->info.type==FT_DIR ? 0 : f->info.size;
            if (old) {
                // Keep the entry it was to replace, which Info-ZIP counts
                // as read
                warn(z, S("will just copy entry over: "),
                     shown_name(ar, old), scratch);
                copy = old;
                z->nread++;
                z->bread += size;
            } else {
                z->nskipped++;
                z->bskipped += size;
            }
        }
        }
        if (copy) {
            i32 err = copy_entry(z, ar, &k, copy, rename, scratch);
            if (err) {
                discard_temp(z, fd, temp, scratch);
                return err;
            }
            cd[count++] = copy;
        }
    }

    if (!w.err) {
        // As Info-ZIP, the files read and skipped, before the directory
        // (and so before "zip file empty")
        summarize_reads(z, scratch);

        i64 cdoff = zout_tell(&w);
        for (iz i = 0; i < count; i++) {
            arena tmp = scratch;  // each header is forgotten once written
            u8   *h   = newbytes(&tmp, zip_central_len(cd[i]));
            zout_write(&w, h, zip_central(h, cd[i])-h);
        }
        i64 cdsize  = zout_tell(&w) - cdoff;
        // Empty, its offset is 0, as Info-ZIP writes it, for UnZip, which
        // otherwise takes a preamble (a stub left by deleting all the
        // entries of a self-extractor) for a misplaced directory
        cdoff = count ? cdoff : 0;
        s8  comment = ar ? ar->end.comment : (s8){0};
        iz  len     = zip_end_len(count, cdsize, cdoff, comment);
        u8 *end     = newbytes(&scratch, len);
        u8 *fin     = zip_end(end, count, cdsize, cdoff, comment, made_by(z));
        zout_write(&w, end, fin-end);
        zout_flush(&w);
    }

    if (ar) {
        os_close(z->ctx, ar->in.fd);  // before replacing it
        ar->in.fd = -1;
    }
    if (w.err || !os_truncate(z->ctx, fd, zout_tell(&w))) {
        // Info-ZIP's words for a failed write, in whichever entry
        s8  why = w.err ? w.why : os_error(z->ctx);
        i32 err = fail_why(z, ZE_WRITE, why, S("Output file write failure"),
                           S("write error on zip file"), scratch);
        discard_temp(z, fd, temp, scratch);
        return err;
    }
    if (!count) {
        warn(z, S("zip file empty"), S(""), scratch);
    }
    // A new archive replaces nothing: should a file have appeared at its
    // path meanwhile, that is not for zip to lose
    switch (os_commit(z->ctx, fd, temp, z->target, z->arcexists, scratch)) {
    case COMMIT_OK:
        return 0;
    case COMMIT_ECLOSE:
        // A deferred write error, which Info-ZIP finds on closing its
        // temporary file
        return fail(z, ZE_TEMP, S("Temporary file failure"), temp, scratch);
    }
    // Not replaced, as Info-ZIP reports it, which keeps its temporary
    // file and warns so, where zip removes it, warning only if it cannot
    s8      why  = os_error(z->ctx);
    os_info info = {0};
    if (os_stat(z->ctx, temp, 0, &info, scratch)) {
        warn(z, S("new zip file left as: "), temp, scratch);
    }
    return fail_why(z, ZE_CREAT, why, S("Could not create output file"),
                    S("was replacing the original zip file"), scratch);
}

static b32 has_extension(zip *z, s8 path)
{
    for (iz i = path.len; i > 0; i--) {
        u8 c = path.s[i-1];
        if (c == '.') {
            return 1;
        } else if (is_sep(z, c) || (z->windows && c==':')) {
            return 0;
        }
    }
    return 0;
}

// SOURCE_DATE_EPOCH: any decimal integer, negative too, for times
// before 1970. Magnitudes saturate far beyond what entries can hold, as
// times clamp to the DOS and UT ranges anyway.
static i32 parse_epoch(zip *z, s8 s, arena scratch)
{
    if (!s.s) {
        return 0;
    }
    b32 neg = s.len && s.s[0]=='-';
    i64 v   = 0;
    for (iz i = neg; i < s.len; i++) {
        if (s.s[i]<'0' || s.s[i]>'9') {
            return fail(z, ZE_PARMS, S("Invalid SOURCE_DATE_EPOCH"), s,
                        scratch);
        }
        v = MIN(v*10 + (s.s[i] - '0'), (i64)1<<40);
    }
    if (s.len == neg) {
        return fail(z, ZE_PARMS, S("Invalid SOURCE_DATE_EPOCH"), s, scratch);
    }
    z->haveepoch = 1;
    z->epoch     = neg ? -v : v;
    return 0;
}

// An entry's name as Info-ZIP matches patterns and filters against it,
// and finds its file by it: the stored name, except on Windows, where
// its port decodes names, the Unicode name if there is one.
static s8 port_name(zip *z, zarchive *ar, iz i)
{
    s8 u = z->windows ? entry_unicode(ar, i) : (s8){0};
    return u.s ? u : ar->entries[i].name;
}

// Existing entries by name (see zip_main), names that differ only in
// ASCII case being one on Windows: an open-addressed hash table of entry
// indexes, at most half full, so 8 to 16 bytes an entry, where a hash
// trie would take a 56-byte node for each.
typedef struct {
    zarchive *ar;
    u32      *slots;  // 2*index + 1, plus 1 if by Unicode name; 0 if empty
    i32       exp;
    i32       fold;
    b32       dups;   // some names found no slot of their own
} zindex;

static u64 zindex_hash(zindex *t, s8 name)
{
    u64 h = 0x100;
    for (iz i = 0; i < name.len; i++) {
        h ^= zip_fold(name.s[i], t->fold);
        h *= 1111111111111111111u;
    }
    return h;
}

// The slot holding the entry found by name, else the empty slot for it.
static u32 *zindex_slot(zindex *t, s8 name)
{
    u64 h    = zindex_hash(t, name);
    u32 mask = ((u32)1 << t->exp) - 1;
    u32 step = (u32)(h >> (64 - t->exp)) | 1;
    for (u32 i = (u32)h;;) {
        i = (i + step) & mask;
        u32 v = t->slots[i];
        if (!v) {
            return t->slots + i;
        }
        iz e    = (iz)((v - 1) >> 1);
        s8 have = (v-1)&1 ? entry_unicode(t->ar, e) : t->ar->entries[e].name;
        b32 eq  = have.len == name.len;
        for (iz k = 0; eq && k < name.len; k++) {
            eq = zip_fold(have.s[k], t->fold) == zip_fold(name.s[k], t->fold);
        }
        if (eq) {
            return t->slots + i;
        }
    }
}

// The index of the entry found by name, or -1 if none.
static iz zindex_find(zindex *t, s8 name)
{
    u32 v = *zindex_slot(t, name);
    return v ? (iz)((v - 1) >> 1) : -1;
}

// The index of the entry whose stored name it is, or -1 if none.
static iz zindex_stored(zindex *t, s8 name)
{
    u32 v = *zindex_slot(t, name);
    return v && !((v - 1) & 1) ? (iz)((v - 1) >> 1) : -1;
}

// The entry that a file replaces, found by name, or -1 if none, but not
// by Unicode name one that another file names by its stored name, as
// marked in bystored: that file replaces it, and this one is added, so
// that both are kept, where Info-ZIP lets the last replace it.
static iz file_entry(zindex *t, s8 name, b32 *bystored)
{
    iz i = zindex_find(t, name);
    return i>=0 && bystored[i] && zindex_stored(t, name)<0 ? -1 : i;
}

static zindex zindex_new(zip *z, zarchive *ar, arena *a)
{
    iz      n      = ar ? (iz)ar->end.count : 0;
    zunames unames = ar ? ar->unames : (zunames){0};
    zindex  t      = {0};
    t.ar   = ar;
    t.fold = z->windows ? ZIP_FOLD : 0;
    t.exp  = 1;
    for (iz total = n + unames.len; (iz)1<<(t.exp-1) < total; t.exp++) {
        if (t.exp == 31) {
            os_oom(z->ctx);  // past what 32-bit slots can index
        }
    }
    t.slots = new(a, (iz)1<<t.exp, u32);
    for (i32 pass = 0; pass < 2; pass++) {
        for (iz j = 0; j < (pass ? unames.len : n); j++) {
            iz   i    = pass ? unames.data[j].index : j;
            s8   name = pass ? unames.data[j].name  : ar->entries[i].name;
            u32 *slot = zindex_slot(&t, name);
            if (!*slot) {
                *slot = (u32)(2*i + 1 + pass);
            } else if ((iz)((*slot - 1) >> 1) != i) {
                t.dups = 1;
            }
        }
    }
    return t;
}

// The entries a pattern can match, so that a name without wildcards,
// as when deleting files gone from disk, is looked up, not matched with
// every entry: those found by it, and on Windows, as DOS rules match "f"
// to "f.", by it less a final period. But if some names share a slot,
// as entries of one name, or by case or Unicode names, they could hide
// a match. Returns how many, or -1 if every entry must be tried.
static iz candidates(zip *z, zindex *t, s8 pattern, iz found[2])
{
    if (!pattern.s || t->dups) {
        return -1;
    }
    for (iz i = 0; i < pattern.len; i++) {
        u8 c = pattern.s[i];
        if (c=='*' || c=='?' || c=='[' || c=='\\') {
            return -1;
        }
    }
    iz n = 0;
    iz v = zindex_find(t, pattern);
    found[n] = v;
    n += v >= 0;
    if (z->windows && pattern.len && pattern.s[pattern.len-1]=='.') {
        v = zindex_find(t, (s8){pattern.s, pattern.len-1});
        found[n] = v;
        n += v >= 0;
    }
    return n;
}

// Whether a pattern matches an entry's name for Info-ZIP's port, or its
// stored name, which on Windows tugz matches too.
static b32 pattern_hit(zip *z, s8 pattern, zarchive *ar, iz i)
{
    s8s one  = {&pattern, 1, 1};
    s8  name = ar->entries[i].name;
    s8  port = port_name(z, ar, i);
    return any_match(z, &one, name) ||
           (port.s!=name.s && any_match(z, &one, port));
}

// Mark the entries matching a -d pattern that pass -i and -x, which
// Info-ZIP applies to deletions too. Returns whether any entry matched,
// marked or not.
static b32 mark_deletes(zip *z, zarchive *ar, iz n, zindex *old, s8 pattern,
                        b32 *hit)
{
    b32 any = 0;
    iz  found[2];
    iz  m   = candidates(z, old, pattern, found);
    for (iz k = 0; k < (m<0 ? n : m); k++) {
        iz i = m<0 ? k : found[k];
        if (pattern_hit(z, pattern, ar, i)) {
            hit[i] |= included(z, port_name(z, ar, i));
            any = 1;
        }
    }
    return any;
}

// Mark the entry for a -d name on disk, which Info-ZIP takes literally,
// wildcards and all: a directory names its "dir/" entry, unless -D, and
// a hidden or system file (Windows) nothing, unless -S, nor does a
// special file (FIFO, device). Under -j, as Info-ZIP names a file to
// add, and as -u and -f do, a file names the entry of its last part,
// and a directory none, while -i and -x see the whole name.
static void mark_named(zip *z, zindex *old, s8 path, os_info *info,
                       b32 *hit, arena scratch)
{
    s8 name = arg_name(z, path, info, &scratch);
    if (info->type == FT_OTHER) {
        ignore_special(z, path, info, scratch);
        return;
    } else if (info->type==FT_DIR && (z->nodirs || z->junk || !name.len)) {
        return;
    } else if (hidden_file(z, info)) {
        return;
    }
    iz v = zindex_find(old, z->junk ? basename(z, name) : name);
    if (v>=0 && included(z, name)) {
        hit[v] = 1;
    }
}

// Whether a file is newer than its entry, for -u and -f, as Info-ZIP
// decides: by the Unix time in the entry's extra fields if they have one
// (see zip_extra_mtime), so that the time zone does not matter, else by
// DOS times. The file's time is not clamped to SOURCE_DATE_EPOCH, which
// would hide changes: past the epoch, a file is newer than any entry
// made with it.
static b32 is_newer(zip *z, zfile *f, zentry *e)
{
    i64 t = 0;
    if (zip_extra_mtime(e->cextra, &t)) {
        return f->info.mtime > t;
    }
    return file_dostime(z, f->info.mtime, 0) > e->dostime;
}

// Whether a file differs from its entry, for -FS, as Info-ZIP decides:
// by DOS time, unclamped as above (so the time zone matters), or size.
static b32 differs(zip *z, zfile *f, zentry *e)
{
    return file_dostime(z, f->info.mtime, 0) != e->dostime ||
           (f->info.type==FT_FILE && f->info.size!=e->usize);
}

// The path by which an entry names its file, or a null string if its
// name might reach beyond the current directory, as an untrusted
// archive's could: the name must be one zip would make (not absolute,
// nor on Windows with a drive or backslashes), but for leading ./ such
// as bsdtar and Windows' tar write, and have no .. components. A name of
// only ./ names the current directory.
static s8 entry_path(zip *z, s8 name, arena scratch)
{
    for (iz i = 0; i < name.len; i++) {
        if (!name.s[i]) {
            return (s8){0};
        }
    }
    s8 path = name;
    while (path.len>=2 && path.s[0]=='.' && path.s[1]=='/') {
        iz skip = 1;
        for (; skip<path.len && path.s[skip]=='/'; skip++) {}
        path = (s8){path.s+skip, path.len-skip};
    }
    if (!zequals(zip_name(&scratch, path, z->windows), path)) {
        return (s8){0};
    }
    for (iz i = 0, end; i < path.len; i = end + 1) {
        for (end = i; end<path.len && path.s[end]!='/'; end++) {}
        if (end-i==2 && path.s[i]=='.' && path.s[i+1]=='.') {
            return (s8){0};
        }
    }
    path.len -= is_dirname(path);
    return path.len ? path : S(".");
}

// Select existing entries as Info-ZIP's procname does for a path not on
// disk, matching it as a pattern against their names, and as it does
// for -u and -f without paths, taking every entry (a null pattern). The
// file that a selected entry names is examined, without recursion, -D,
// or -j, if the name passes -i and -x, no path named it already, and it
// reaches no further than the current directory: through no link (on
// Windows, nor junction), not even one at its end, which unlike Info-ZIP
// is not followed, but only stored, with -y. On Windows, it is looked up
// by a ./ path, so a name like COM1 never opens a DOS device, unlike in
// Info-ZIP (a path argument so named still does). A missing file, or such a
// link (with a warning), leaves its entry as it is (deleted, under -FS).
// As in Info-ZIP, which looks up a path's entry by name, but examines
// the file of every entry it selects, every entry of a name is so
// refreshed: the first by a file recorded to add, the others by files
// of their own, in also. Returns whether any entry matched.
static b32 scan_entries(zip *z, zarchive *ar, iz n, zindex *old, s8 pattern,
                        b32 *taken, zfile **also, arena scratch)
{
    b32 any     = 0;
    s8  checked = {0};  // directories known to be no links
    iz  found[2];
    iz  m       = candidates(z, old, pattern, found);
    for (iz k = 0; k < (m<0 ? n : m); k++) {
        iz i = m<0 ? k : found[k];
        if (pattern.s && !pattern_hit(z, pattern, ar, i)) {
            continue;
        }
        any = 1;

        arena iter = scratch;
        s8    name = port_name(z, ar, i);
        if (taken[i] || !included(z, name)) {
            continue;
        }
        s8  path   = entry_path(z, name, iter);
        s8  shown  = path;  // in messages
        b32 dotted = path.s && z->windows && !zequals(path, S("."));
        if (dotted) {
            // A file in the current directory, never the DOS device that
            // a bare name like COM1 or NUL would be, at the end or as a
            // directory along the way
            path = JOIN(&iter, S("./"), path);
        }
        if (!path.s || !linkless(z->ctx, path, 0, &checked, iter)) {
            continue;
        }
        taken[i] = 1;

        os_info info = {0};
        if (!os_stat(z->ctx, path, 0, &info, iter) ||
            is_archive(z, path, &info, iter)) {
            continue;
        } else if (info.type==FT_LINK && !z->symlinks) {
            // Not followed, unlike Info-ZIP, nor stored without -y: its
            // entry is left as for a missing file
            warn(z, S("not following link that an entry names: "), shown,
                 iter);
            continue;
        } else if (info.type == FT_OTHER) {
            // Left out as a special file named or met while recursing is,
            // its entry left as for a missing file (Info-ZIP would block
            // reading a FIFO)
            ignore_special(z, shown, &info, iter);
            continue;
        }
        path = dotted ? JOIN(&z->perm, path) : path;  // kept, as name is
        if (zindex_find(old, name) != i) {
            also[i] = new_file(z, path, name, &info);
            also[i]->byentry = 1;
            continue;
        }
        *zmap_upsert(&z->names, name, name_fold(z), &z->perm) = z->files.len;
        zfile *f = new_file(z, path, name, &info);
        f->byentry = 1;  // nor read through a link swapped in later
        *push(&z->perm, &z->files) = f;
    }
    return any;
}

// Mark an entry's item to be replaced by a file of its name, if -u and
// -f find the file newer, or -FS changed, keeping its name. Returns
// whether the file replaces it.
static b32 refresh_entry(zip *z, zarchive *ar, zitem *it, iz i, zfile *f,
                         arena scratch)
{
    zentry *e       = it->old;
    b32     replace = 0;
    switch (z->mode) {
    case MODE_ADD:
        replace = 1;
        break;
    case MODE_UPDATE:
    case MODE_FRESHEN:
        replace = is_newer(z, f, e);
        break;
    case MODE_SYNC:
        replace  = differs(z, f, e);
        it->kind = ITEM_KEEP;
        if (!replace) {
            // Current, which Info-ZIP counts as read
            z->nread++;
            z->bread += f->info.type==FT_DIR ? 0 : f->info.size;
        }
        break;
    }
    if (!replace) {
        return 0;
    }

    // The entry keeps its name, in Unicode if the file matched that,
    // which is then written as UTF-8
    b32 stored = zfoldeq(f->name, e->name, name_fold(z));
    s8  name   = stored ? e->name : entry_unicode(ar, i);
    if (name.len > ZIP_MAX16) {
        // Decoded from the OEM code page (Windows), as an entry selected
        // it, a Unicode name can outgrow its stored name and a header: as
        // a long path is, it is skipped, and the entry kept
        warn(z, S("name too long for a zip entry: "), f->path, scratch);
        z->status = ZE_OPEN;
        z->nskipped++;
        z->bskipped += f->info.type==FT_FILE ? f->info.size : 0;
        return 0;
    }
    it->kind = z->mode==MODE_FRESHEN ? ITEM_FRESHEN : ITEM_UPDATE;
    it->file = f;
    f->name  = name;
    return 1;
}

// Fail with nothing to do, naming the archive, or, as Info-ZIP does when
// recursing into paths without patterns, suggesting the command that
// takes them as -i patterns of the current directory: its arguments,
// options first, as Info-ZIP moves them there, and the paths after
// "-i" ("try: zip -r a.zip . -i src").
static i32 nothing_to_do(zip *z, arena scratch)
{
    if (!z->recurse || !z->firstpath || z->mode==MODE_DELETE ||
        z->include.len || z->exclude.len) {
        return fail(z, ZE_NONE, S("Nothing to do!"), z->archive, scratch);
    }
    s8s parts = {0};
    *push(&scratch, &parts) = S("try: zip");
    for (iz i = 0; i < z->optargs.len; i++) {
        *push(&scratch, &parts) = S(" ");
        *push(&scratch, &parts) = z->optargs.data[i];
    }
    for (iz i = 0; i < z->nonopts.len; i++) {
        if (i == z->firstpath) {
            *push(&scratch, &parts) = S(" . -i");
        }
        *push(&scratch, &parts) = S(" ");
        *push(&scratch, &parts) = z->nonopts.data[i];
    }
    s8 hint = zjoin(&scratch, parts.data, parts.len);
    return fail(z, ZE_NONE, S("Nothing to do!"), hint, scratch);
}

// The existing archive, as it is opened to be changed: read, if there,
// with its entries indexed by name, in the memory of the caller's frame,
// to which the index points.
typedef struct {
    zarchive  arc;
    zarchive *ar;          // &arc, or null for a new archive
    iz        nold;        // its entries
    zindex    old;
    s8        unfollowed;  // why links at its path could not be followed
} zold;

// Find the archive past any links at its path, and read it if it is
// there, as Info-ZIP does before any work, its index of names in scratch.
// Returns a status, zero to go on.
static i32 open_archive(zip *z, zold *o, arena *scratch)
{
    // The archive is replaced past any links at its path, so that they
    // survive, as Info-ZIP updates it through them. A link that cannot
    // be followed leaves no archive to read, and as in Info-ZIP, once
    // there is something to do, none can be written, for the reason
    // found now.
    z->target     = os_resolve(z->ctx, z->archive, &z->perm, *scratch);
    o->unfollowed = z->target.s ? S("") : JOIN(&z->perm, os_error(z->ctx));

    // Whatever is at the archive path must be a zip file, as Info-ZIP
    // finds before any work: an empty file, or a directory, is not
    z->arcexists = z->target.s &&
                   os_stat(z->ctx, z->target, 1, &z->arcinfo, *scratch);
    if (z->target.s && !z->arcexists && !os_missing(z->ctx)) {
        // Only a missing archive is new: one that could not be examined,
        // as after an I/O error, may hold entries that replacing it loses
        return fail(z, ZE_READ, S("Could not open archive"), z->archive,
                    *scratch);
    }
    if (z->arcexists && !z->arcinfo.ino[0] && !z->arcinfo.ino[1]) {
        z->arcpath = os_fullpath(z->ctx, z->target, &z->perm, *scratch);
    }
    if (z->arcexists) {
        i32 err = read_archive(z, &o->arc, *scratch);
        if (err > 0) {
            return err;
        }
        o->ar = err ? 0 : &o->arc;
    }
    o->nold = o->ar ? (iz)o->ar->end.count : 0;
    if (!o->nold && z->mode!=MODE_ADD && z->mode!=MODE_SYNC) {
        // Info-ZIP warns, then goes on with the arguments
        warn(z, z->archive, S(" not found or empty"), *scratch);
    }

    // Existing entries by name, the first of any duplicates, and failing
    // that by Unicode name, as Info-ZIP looks them up. On Windows, a file
    // then replaces an entry whose name differs only in case, as in
    // Info-ZIP, and the entry keeps its name.
    o->old = zindex_new(z, o->ar, scratch);
    return 0;
}

// Plan the items for the files scanned (z->files): one for each entry,
// kept, deleted (-FS), or refreshed by a file of its name, then one for
// each file that adds an entry. Paths not on disk, missing, select
// entries as patterns, and all, as -u and -f without paths, selects
// every entry, each refreshed from the file it names. Returns a status,
// zero to go on.
static i32 plan_files(zip *z, zold *o, s8s missing, b32 all, zitems *items,
                      b32 *changed, iz *found, arena scratch)
{
    iz      nold    = o->nold;
    zentry *entries = o->ar ? o->ar->entries : 0;

    // Then paths not on disk select entries, as do -u and -f without
    // paths, except entries that paths on disk already selected
    b32    *taken    = new(&scratch, nold, b32);
    b32    *bystored = new(&scratch, nold, b32);
    zfile **also     = new(&scratch, nold, zfile *);
    iz      nadd     = 0;  // files with no entry, as entries select none
    for (iz i = 0; i < z->files.len; i++) {
        iz v = zindex_stored(&o->old, z->files.data[i]->name);
        if (v >= 0) {
            bystored[v] = 1;
        }
    }
    for (iz i = 0; i < z->files.len; i++) {
        iz v = file_entry(&o->old, z->files.data[i]->name, bystored);
        if (v >= 0) {
            taken[v] = 1;
        } else {
            nadd++;
        }
    }
    for (iz p = 0; p < missing.len; p++) {
        s8 pattern = zip_name(&scratch, missing.data[p], z->windows);
        if (!scan_entries(z, &o->arc, nold, &o->old, pattern, taken, also,
                          scratch)) {
            warn(z, S("name not matched: "), missing.data[p], scratch);
        }
    }
    if (all) {
        scan_entries(z, &o->arc, nold, &o->old, (s8){0}, taken, also,
                     scratch);
    }
    if (z->dups[0].name.s) {
        return repeated(z, scratch);
    }

    if (z->mode==MODE_SYNC && !z->files.len) {
        // Rather than delete every entry, as for a misspelled path
        return nothing_to_do(z, scratch);
    }

    // An item for each entry, and one for each file to add
    items->cap  = nold + (z->mode==MODE_FRESHEN ? 0 : nadd);
    items->data = new(&z->perm, items->cap, zitem);
    for (iz i = 0; i < nold; i++) {
        zitem *it = push(&z->perm, items);
        it->kind = z->mode==MODE_SYNC ? ITEM_DELETE : ITEM_KEEP;
        it->old  = entries + i;
        it->file = 0;
    }

    *found = z->files.len;
    for (iz i = 0; i < z->files.len; i++) {
        zfile *f = z->files.data[i];
        iz     v = file_entry(&o->old, f->name, bystored);
        if (v >= 0) {
            *changed |= refresh_entry(z, o->ar, items->data+v, v, f,
                                      scratch);
        } else if (z->mode != MODE_FRESHEN) {
            zitem *it = push(&z->perm, items);
            it->kind = ITEM_ADD;
            it->old  = 0;
            it->file = f;
            *changed = 1;
        }
    }
    for (iz i = 0; i < nold; i++) {
        if (also[i]) {
            (*found)++;
            *changed |= refresh_entry(z, o->ar, items->data+i, i, also[i],
                                      scratch);
        }
        *changed |= items->data[i].kind == ITEM_DELETE;
    }
    return 0;
}

// Write the archive anew, once there is something to do, else say why
// not, as Info-ZIP does: -FS reports an archive already current, while
// -u and -f say nothing. Returns a status.
static i32 finish(zip *z, zold *o, zitems *items, b32 changed, iz found,
                  arena scratch)
{
    if (!changed) {
        b32 refresh = z->mode==MODE_UPDATE || z->mode==MODE_FRESHEN;
        b32 none    = z->mode==MODE_FRESHEN && !o->nold;  // nothing to freshen
        if (found && !none) {
            // Already up to date, which only -FS reports, as in Info-ZIP
            if (z->mode==MODE_SYNC && !z->quiet) {
                say(z, 1, S("Archive is current\n"));
            }
            return z->status;
        } else if (refresh) {
            return ZE_NONE;  // silently, as in Info-ZIP
        }
        return nothing_to_do(z, scratch);
    }

    // Once there is something to do, and before doing it, Info-ZIP opens
    // the archive for writing, which a read-only one refuses: a guard
    // against changing it, though replacing it needs no such permission
    b32 readonly = z->arcexists && !os_writable(z->ctx, z->target, scratch);
    if (!z->target.s || readonly) {
        s8 why = readonly ? os_error(z->ctx) : o->unfollowed;
        return fail_why(z, ZE_CREAT, why, S("Could not create output file"),
                        z->archive, scratch);
    }
    i32 err = write_archive(z, o->ar, items, scratch);
    return err ? err : z->status;
}

static i32 zip_run(zip *z, zipconfig *conf)
{
    arena scratch = conf->scratch;

    // Without arguments, Info-ZIP streams standard input to standard
    // output, unless that is a terminal, which gets the usage instead
    if (!conf->nargs && os_isatty(z->ctx, 1)) {
        return usage(z, 0);
    }

    // Options from the environment precede the arguments, as in Info-ZIP:
    // those of ZIPOPT or, if it has none, of ZIP
    s8s env = env_args(&z->perm, conf->zipopt, z->windows);
    env = env.len ? env : env_args(&z->perm, conf->zipenv, z->windows);
    for (i32 i = 0; i < conf->nargs; i++) {
        *push(&z->perm, &env) = conf->args[i];
    }
    s8 *args  = env.data;
    i32 nargs = (i32)env.len;

    if (nargs==1 && zequals(args[0], S("-v"))) {
        version(z);
        return 0;
    }
    i32 err = parse_args(z, args, nargs, scratch);
    if (err) {
        return err<0 ? 0 : err;
    }
    err = parse_epoch(z, conf->epoch, scratch);
    if (err) {
        return err;
    }
    // As Info-ZIP finds first, -x and -i patterns need something to
    // select from: paths, or archive entries for -u and -f to refresh
    b32 patterns = z->include.len || z->exclude.len;
    b32 refresh  = z->mode==MODE_UPDATE || z->mode==MODE_FRESHEN;
    s8  nothing  = S("nothing to select from");
    if (!z->archive.s) {
        s8 why = patterns ? nothing
               : os_isatty(z->ctx, 1) ? S("cannot write zip file to terminal")
               : S("streaming to standard output not supported");
        return fail(z, ZE_PARMS, S("Invalid command arguments"), why, scratch);
    }
    if (!has_extension(z, z->archive)) {
        z->archive = JOIN(&z->perm, z->archive, S(".zip"));
    }
    if (z->names_stdin) {
        // A directory there holds no names, as Info-ZIP's getc fails at
        // once to read one
        os_info in   = {0};
        s8      text = read_all(z->ctx, 0, &z->perm);
        s8      why  = text.s ? S("") : os_error(z->ctx);
        if (!text.s && os_fstat(z->ctx, 0, &in) && in.type==FT_DIR) {
            text = S("");
        } else if (!text.s) {
            return fail_why(z, ZE_READ, why, S("Could not read names"),
                            S("standard input"), scratch);
        }
        // These names come before the arguments, as in Info-ZIP
        s8s names = {0};
        push_lines(z, &names, text);
        for (iz i = 0; i < z->paths.len; i++) {
            *push(&z->perm, &names) = z->paths.data[i];
        }
        z->paths = names;
    }
    if (patterns && !z->paths.len && !refresh) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"), nothing,
                    scratch);
    }
    if (z->mode==MODE_DELETE && (z->recurse || !z->level)) {
        warn(z, S("invalid option(s) used with -d; ignored."), S(""),
             scratch);
    }
    if (z->filesync && z->mode!=MODE_ADD) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("can't use -d, -f, -u, -U, or -g with filesync -FS"),
                    scratch);
    }
    z->mode = z->filesync ? MODE_SYNC : z->mode;

    zold   o       = {0};
    zitems items   = {0};
    b32    changed = 0;
    iz     found   = 0;  // files found to add or refresh
    err = open_archive(z, &o, &scratch);
    if (err) {
        return err;
    }
    if (z->mode == MODE_DELETE) {
        zentry *entries = o.ar ? o.ar->entries : 0;
        b32    *hit     = new(&scratch, o.nold, b32);
        for (iz p = 0; p < z->paths.len; p++) {
            s8 arg = z->paths.data[p];
            os_info info = {0};
            if (zequals(arg, S("-"))) {
                // Standard input's entry, by name alone, as Info-ZIP's
                // procname takes it, never looking on disk nor warning
                iz v = zindex_find(&o.old, arg);
                if (v>=0 && included(z, arg)) {
                    hit[v] = 1;
                }
            } else if (os_stat(z->ctx, arg, !z->symlinks, &info, scratch)) {
                // Info-ZIP does not warn about names on disk
                mark_named(z, &o.old, arg, &info, hit, scratch);
            } else {
                s8 pattern = zip_name(&scratch, arg, z->windows);
                if (!mark_deletes(z, &o.arc, o.nold, &o.old, pattern, hit)) {
                    warn(z, S("name not matched: "), arg, scratch);
                }
            }
        }
        items.cap  = o.nold;
        items.data = new(&z->perm, items.cap, zitem);
        for (iz i = 0; i < o.nold; i++) {
            zitem *it = push(&z->perm, &items);
            it->kind = hit[i] ? ITEM_DELETE : ITEM_KEEP;
            it->old  = entries + i;
            it->file = 0;
            changed |= hit[i];
        }
    } else {
        s8s missing = {0};
        for (iz p = 0; p < z->paths.len; p++) {
            if (!scan_arg(z, z->paths.data[p], scratch)) {
                *push(&scratch, &missing) = z->paths.data[p];
            }
        }
        err = plan_files(z, &o, missing, refresh && !z->paths.len, &items,
                         &changed, &found, scratch);
        if (err) {
            return err;
        }
    }
    return finish(z, &o, &items, changed, found, scratch);
}

[[maybe_unused]] static i32 zip_main(zipconfig *conf)
{
    zip *z = new(&conf->perm, 1, zip);
    z->ctx     = conf->perm.ctx;
    z->perm    = conf->perm;
    z->level   = 6;
    z->windows = conf->windows;

    // Messages to standard output are buffered, as Info-ZIP's C library
    // buffers a pipe or file, rather than written a piece at a time, two
    // or three writes for each entry. A terminal's too, which that
    // library writes a line at a time, but flushed once its text has
    // waited a moment, at check points between entries and between
    // chunks of their data, lest a slow run show nothing for long, then
    // pages at once; a line at a time is slow on a Windows console given
    // many small files. The archive never goes to standard output.
    z->out      = newwriter(&z->perm, 1, 1<<16);
    z->out->tty = os_isatty(z->ctx, 1);
    os_holdtext(z->ctx, z->out);
    oom_out = z->out;
    i32 r = zip_run(z, conf);
    writer_flush(z->out);
    os_holdtext(z->ctx, 0);
    oom_out = 0;
    return r;
}

// A caller's interface, for changes other than the command line's, as
// the GUI makes them: files added under names of the caller's choosing,
// or entries, each found by its ordinal in the central directory and its
// stored name, deleted or renamed, but not both at once. Paths are taken
// as they are, never as patterns or with ".zip" added, and no options
// come from the environment. Messages are zip's own, on standard output
// and error, and the status is as zip_main's.

typedef struct {
    s8 path;  // on the file system
    s8 name;  // in the archive: a directory's names its entries' prefix
} zipsource;

typedef struct {
    iz ordinal;  // of the entry, in the central directory
    s8 stored;   // its name as stored, which the entry's must be
    s8 name;     // its new name, in UTF-8, or null to delete it
} zipchange;

typedef struct {
    s8         archive;
    zipsource *sources;
    iz         nsources;
    zipchange *changes;
    iz         nchanges;
    i32        level;    // 0 to 9
    i32        mode;     // MODE_ADD, MODE_UPDATE, or MODE_FRESHEN
    b32        hidden;   // -S, for files met while recursing
    b32        noextra;  // -X
    b32        nodirs;   // -D
    // Whether a file is to replace the entry it names, asked of each
    // before anything is written, or if null, every one replaces its
    // entry, as on the command line
    b32      (*replace)(void *data, s8 name);
    void      *data;
    i64        total;    // set: the bytes to read, of files and entries
} zipcall;

// Plan a caller's changes to entries, each found by its ordinal and
// checked by its stored name: deleted, or renamed under a name that no
// entry kept has (stored or in Unicode), nor another renamed one, though
// it may be its own in another case. A renamed entry's central Unicode
// path field, of its old name, is dropped (in perm, before any output),
// as copy_entry drops its local one. Returns a status, zero to go on.
static i32 plan_changes(zip *z, zold *o, zipcall *c, zitems *items,
                        b32 *changed, arena scratch)
{
    iz      nold    = o->nold;
    zentry *entries = o->ar ? o->ar->entries : 0;
    items->cap  = nold;
    items->data = new(&z->perm, items->cap, zitem);
    for (iz i = 0; i < nold; i++) {
        zitem *it = push(&z->perm, items);
        it->kind = ITEM_KEEP;
        it->old  = entries + i;
        it->file = 0;
    }

    s8 invalid = S("Invalid command arguments");
    for (iz k = 0; k < c->nchanges; k++) {
        zipchange *ch = c->changes + k;
        iz         i  = ch->ordinal;
        if (i<0 || i>=nold || !zequals(entries[i].name, ch->stored) ||
            items->data[i].kind!=ITEM_KEEP) {
            s8 why = JOIN(&scratch, S("no such entry: "), ch->stored);
            return fail(z, ZE_PARMS, invalid, why, scratch);
        } else if (!ch->name.s) {
            items->data[i].kind = ITEM_DELETE;
            *changed = 1;
            continue;
        }

        b32 nul = 0;
        for (iz n = 0; n < ch->name.len; n++) {
            nul |= !ch->name.s[n];
        }
        if (!ch->name.len || ch->name.len>ZIP_MAX16 || nul ||
            zip_utf8(ch->name)<0) {
            s8 why = JOIN(&scratch, S("invalid name: "), ch->name);
            return fail(z, ZE_PARMS, invalid, why, scratch);
        }
        zfile *f = new(&z->perm, 1, zfile);
        f->name = JOIN(&z->perm, ch->name);
        items->data[i].kind = ITEM_RENAME;
        items->data[i].file = f;
        *changed = 1;
    }

    zmap *taken = 0;
    i32   fold  = name_fold(z);
    for (iz i = 0; i < nold; i++) {
        if (items->data[i].kind == ITEM_KEEP) {
            *zmap_upsert(&taken, entries[i].name, fold, &scratch) = i;
            s8 u = entry_unicode(o->ar, i);
            if (u.s) {
                *zmap_upsert(&taken, u, fold, &scratch) = i;
            }
        }
    }
    for (iz i = 0; i < nold; i++) {
        if (items->data[i].kind == ITEM_RENAME) {
            s8  name = items->data[i].file->name;
            iz *v    = zmap_upsert(&taken, name, fold, &scratch);
            if (*v >= 0) {
                s8 why = JOIN(&scratch, S("name already in archive: "), name);
                return fail(z, ZE_PARMS, invalid, why, scratch);
            }
            *v = i;
            entries[i].cextra = zip_drop_extra(entries[i].cextra,
                                               ZIP_EXTRA_UPATH);
        }
    }
    return 0;
}

static i32 call_run(zip *z, zipcall *c, arena scratch)
{
    if (c->nsources && c->nchanges) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("files to add and entries to change at once"),
                    scratch);
    }

    zold   o       = {0};
    zitems items   = {0};
    b32    changed = 0;
    iz     found   = 0;
    i32    err     = open_archive(z, &o, &scratch);
    if (err) {
        return err;
    } else if (c->nchanges) {
        err = plan_changes(z, &o, c, &items, &changed, scratch);
    } else {
        for (iz i = 0; i < c->nsources; i++) {
            zipsource *s = c->sources + i;
            if (!scan_named(z, s->path, s->name, 1, scratch)) {
                warn(z, S("name not matched: "), s->path, scratch);
            }
        }
        err = plan_files(z, &o, (s8s){0}, 0, &items, &changed, &found,
                         scratch);
        for (iz i = 0; !err && c->replace && i<items.len; i++) {
            zitem *it = items.data + i;
            b32    up = it->kind==ITEM_UPDATE || it->kind==ITEM_FRESHEN;
            if (up && !c->replace(c->data, it->file->name)) {
                it->kind = ITEM_KEEP;
                it->file = 0;
            }
        }
        if (c->replace) {
            changed = 0;
            for (iz i = 0; i < items.len; i++) {
                changed |= items.data[i].kind != ITEM_KEEP;
            }
        }
    }
    if (err) {
        return err;
    }

    c->total = 0;
    for (iz i = 0; i < items.len; i++) {
        zitem *it = items.data + i;
        switch (it->kind) {
        case ITEM_KEEP:
        case ITEM_RENAME:
            c->total += it->old->csize;
            break;
        case ITEM_ADD:
        case ITEM_UPDATE:
        case ITEM_FRESHEN:
            c->total += it->file->info.type==FT_FILE ? it->file->info.size : 0;
            break;
        }
    }
    return finish(z, &o, &items, changed, found, scratch);
}

[[maybe_unused]] static i32 zip_call(zipconfig *conf, zipcall *c)
{
    zip *z = new(&conf->perm, 1, zip);
    z->ctx     = conf->perm.ctx;
    z->perm    = conf->perm;
    z->windows = conf->windows;
    z->archive = c->archive;
    z->level   = c->level;
    z->mode    = c->mode;
    z->recurse = 1;
    z->hidden  = c->hidden;
    z->noextra = c->noextra;
    z->nodirs  = c->nodirs;
    z->out     = newwriter(&z->perm, 1, 1<<16);
    oom_out    = z->out;
    i32 r = call_run(z, c, conf->scratch);
    writer_flush(z->out);
    oom_out = 0;
    return r;
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

// CRT-free Win32 file system functions for tugz zip and unzip: the
// arena's reservation, examining files, positioned reads, code pages,
// the environment, local time. Included by the programs' platform
// layers, after windows.c.

typedef struct {
    uptr internal, internal_high;
    u32  offset, offset_high;
    uptr event;
} overlapped;

typedef struct {
    u16 year, month, weekday, day, hour, minute, second, ms;
} systemtime;

typedef struct {
    u64 volume;
    u64 id[2];  // 128 bits
} file_id_info;

W32(u32)  GetEnvironmentVariableW(c16 *, c16 *, u32);
W32(i32)  MultiByteToWideChar(u32, u32, u8 const *, i32, c16 *, i32);
W32(b32)  SystemTimeToTzSpecificLocalTime(uptr, systemtime *, systemtime *);

#define CP_ACP                     0u
#define CP_OEMCP                   1u
#define MB_ERR_INVALID_CHARS       0x8u
#define ERROR_ENVVAR_NOT_FOUND     203u

// zip's memory is one reserved range of address space, committed a
// chunk at a time as it is used, so that the commit charge grows with
// use: perm from the bottom up, and scratch from the top down. As much
// as the system will reserve, up to 64 GiB (1 GiB in 32-bit processes),
// halving on refusal.
[[maybe_unused]] static void reserve(os *ctx)
{
    iz cap = (iz)1 << (sizeof(void *)==8 ? 36 : 30);
    for (; cap >= (iz)1<<24; cap /= 2) {
        byte *p = VirtualAlloc(0, cap, MEM_RESERVE, PAGE_READWRITE);
        if (p) {
            ctx->lo = p;
            ctx->hi = p + cap;
            return;
        }
    }
    os_oom(ctx);
}

// Commit more of the reservation to perm, from below the middle, or to
// a scratch arena, from above it, a megabyte at a time. Scratch below
// the arena asking is free, since the functions that allocated it have
// returned. Any other arena, such as a codec's exact one, is fixed.
static void os_extend(os *ctx, arena *a, iz need)
{
    if (a->down) {
        a->beg = ctx->hi;
    } else if (a->end != ctx->lo) {
        os_oom(ctx);
    }
    iz want = need - (a->end - a->beg);
    iz room = ctx->hi - ctx->lo;
    if (want > room) {
        os_oom(ctx);
    } else if (want > 0) {
        iz    chunk = (iz)1 << 20;
        iz    take  = MIN((want + chunk - 1) & -chunk, room);
        byte *at    = a->down ? ctx->hi-take : ctx->lo;
        if (!VirtualAlloc((uptr)at, take, MEM_COMMIT, PAGE_READWRITE)) {
            os_oom(ctx);  // the system's commit limit
        }
        if (a->down) {
            ctx->hi = at;
            a->beg  = at;
        } else {
            ctx->lo = at + take;
            a->end  = ctx->lo;
        }
    }
}

static b32 handle_info(os *ctx, iptr h, os_info *info)
{
    by_handle_info bh = {0};
    file_id_info   id = {0};
    b32 ok   = GetFileInformationByHandle(h, &bh);
    b32 isid = get_info(ctx, h, FileIdInfo, &id, sizeof(id));
    u32 type = GetFileType(h);
    if (!ok && (type==FILE_TYPE_DISK || type==FILE_TYPE_UNKNOWN)) {
        return 0;
    }

    // The 64-bit index is not unique on ReFS, so prefer the 128-bit ID,
    // available from Windows 8 where the file system supports it. An ID
    // of all zero or all one bits is unknown.
    if (!isid) {
        id.volume = bh.volume;
        id.id[0]  = (u64)bh.index_hi<<32 | bh.index_lo;
        id.id[1]  = 0;
    }
    if (id.id[0]==(u64)-1 && (id.id[1]==(u64)-1 || !id.id[1])) {
        id.id[0] = id.id[1] = 0;
    }

    // Only files and directories on disk are archived. A device (NUL),
    // even one that answers no queries, or the pipe namespace, is special.
    b32 dir = bh.attributes & FILE_ATTRIBUTE_DIRECTORY;
    info->type   = type!=FILE_TYPE_DISK ? FT_OTHER : dir ? FT_DIR : FT_FILE;
    info->size   = (i64)((u64)bh.size_hi<<32 | bh.size_lo);
    info->mtime  = unixtime(bh.written);
    info->atime  = unixtime(bh.accessed);
    info->mode   = 0;
    info->attr   = bh.attributes;
    info->uid    = 0;
    info->gid    = 0;
    info->dev    = id.volume;
    info->ino[0] = id.id[0];
    info->ino[1] = id.id[1];
    return 1;
}

// Symbolic links and junctions are followed, as Info-ZIP does on Windows
// (there is no -y), unless asked not to, as for the directories along an
// entry's name, when a link or a junction, or any other reparse point
// that names another file (reparse_link), is FT_LINK. Other reparse
// points, such as cloud placeholders, are ordinary files.
static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    meter(ctx, path, 0);
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // no file can have it
        return 0;
    }
    u32 flags = FILE_FLAG_BACKUP_SEMANTICS;
    flags |= follow ? 0 : FILE_FLAG_OPEN_REPARSE;
    iptr h = CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        // The console (CON), unlike other DOS devices (NUL, CONIN$),
        // refuses an open that would neither read nor write, but is
        // there to refuse it: special, as they are, rather than missing
        b32 dosdev = wpath[0]=='\\' && wpath[1]=='\\' && wpath[2]=='.';
        if (!dosdev || GetLastError()!=ERROR_INVALID_PARAMETER) {
            return 0;
        }
        *info = (os_info){0};
        info->type = FT_OTHER;
        return 1;
    }
    b32 ok = handle_info(ctx, h, info);
    attribute_tag_info tag = {0};
    if (ok && !follow && (info->attr & FILE_ATTRIBUTE_REPARSE) &&
        get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        reparse_link(tag)) {
        info->type = FT_LINK;
    }
    CloseHandle(h);
    return ok;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    return handle_info(ctx, ctx->handles[fd], info);
}

static i32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    while (len) {
        overlapped ov = {0};
        ov.offset      = (u32)off;
        ov.offset_high = (u32)((u64)off >> 32);
        u32 got = 0;
        u32 n   = (u32)MIN(len, 1<<30);
        if (!ReadFile(ctx->handles[fd], buf, n, &got, (uptr)&ov)) {
            return GetLastError()==ERROR_HANDLE_EOF ? 0 : -1;
        } else if (!got) {
            return 0;
        }
        meter(ctx, (s8){0}, got);
        buf += got;
        len -= got;
        off += got;
    }
    return 1;
}

// By the system's OEM code page, as Info-ZIP's port converts such names
// with OemToAnsi, or else by its ANSI code page, in which the port keeps
// names. A name has at most 65,535 bytes, each at most one unit. Bytes
// that the code page cannot decode (DBCS, UTF-8) leave it without one.
static s8 os_fromcp(os *ctx, s8 name, b32 oem, arena *perm, arena scratch)
{
    (void)ctx;
    i32  len = (i32)name.len;
    c16 *w   = new(&scratch, len+1, c16);
    u32  cp  = oem ? CP_OEMCP : CP_ACP;
    i32  n   = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, name.s, len, w,
                                   len);
    w[n>0 ? n : 0] = 0;
    return n>0 ? towtf8(perm, w) : (s8){0};
}

// An environment variable as WTF-8, or a null string if it is unset.
[[maybe_unused]] static s8 getenv8(arena *a, c16 *name)
{
    SetLastError(0);  // zero is also the length of an empty value
    u32 cap = GetEnvironmentVariableW(name, 0, 0);  // including the null
    if (!cap) {
        b32 unset = GetLastError() == ERROR_ENVVAR_NOT_FOUND;
        return unset ? (s8){0} : S("");
    }
    c16 *buf = new(a, cap, c16);
    u32  len = GetEnvironmentVariableW(name, buf, cap);
    buf[len<cap ? len : 0] = 0;
    return towtf8(a, buf);
}

// By the time zone's rules for that year, as POSIX localtime and .NET
// apply them, where Info-ZIP's port, by the C runtime's localtime,
// applies the current year's to every year.
static void os_localtime(os *ctx, i64 t, i32 tm[6])
{
    (void)ctx;
    zip_gmtime(t, tm);
    if (tm[0]<1601 || tm[0]>30827) {
        return;
    }
    i64 days = t/86400 - (t%86400 < 0);
    systemtime utc = {0};
    utc.year    = (u16)tm[0];
    utc.month   = (u16)tm[1];
    utc.weekday = (u16)(((days + 4)%7 + 7) % 7);  // 1970-01-01: Thursday
    utc.day     = (u16)tm[2];
    utc.hour    = (u16)tm[3];
    utc.minute  = (u16)tm[4];
    utc.second  = (u16)tm[5];
    systemtime loc = {0};
    if (SystemTimeToTzSpecificLocalTime(0, &utc, &loc)) {
        tm[0] = loc.year;
        tm[1] = loc.month;
        tm[2] = loc.day;
        tm[3] = loc.hour;
        tm[4] = loc.minute;
        tm[5] = loc.second;
    }
}

// zip's Win32 platform functions, beyond those of windows.c and
// zipfs_windows.c: links at the archive's path, positioned writes, and
// replacing the archive. Included after them by platform/zip_windows.c
// and by platform/gui_windows.c, which holds both programs.

W32(b32)  FlushFileBuffers(iptr);

#define FILE_ATTRIBUTE_ARCHIVE     0x20u
#define FILE_ATTRIBUTE_NOT_INDEXED 0x2000u
#define FILE_FLAG_DELETE_ON_CLOSE  0x04000000u
#define FILE_RENAME_REPLACE        1u
#define FILE_RENAME_POSIX          2u

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    (void)ctx;
    (void)path;
    (void)a;
    return (s8){0};  // never asked: links are followed
}

// The final path of an open file, a \\?\ path, with its volume named
// one way: as a drive letter path (DOS), by its GUID, or by its NT
// device. Null if the system has no such name for it. XP, which lacks
// GetFinalPathNameByHandleW, has only the NT one, from the object
// manager (NtQueryObject's ObjectNameInformation): the path past any
// junctions, as GetFinalPathNameByHandleW gives it, but for any short
// names and the case of each name, which stay as they were opened.
enum { VOLUME_NAME_DOS, VOLUME_NAME_GUID, VOLUME_NAME_NT };
static c16 *final_path(os *ctx, iptr h, u32 how, arena *a)
{
    winapi *w = &ctx->api;
    if (w->finalpath) {
        u32  cap = w->finalpath(h, 0, 0, how);  // including the null
        c16 *buf = cap ? new(a, cap, c16) : 0;
        u32  len = buf ? w->finalpath(h, buf, cap, how) : 0;
        return len && len<cap ? buf : 0;
    } else if (how!=VOLUME_NAME_NT || !w->ntname) {
        return 0;
    }

    // A UNICODE_STRING, then the name, at most 65,534 bytes
    u32 cap = (u32)sizeof(unicode_string) + 0x10000;
    unicode_string *name = (unicode_string *)newbytes(a, cap);
    u32 len = 0;
    if (!ntresult(w, w->ntname(h, 1, name, cap, &len)) || !name->len) {
        return 0;
    }
    iz   n = name->len / 2;
    c16 *r = new(a, n+1, c16);
    bytecopy(r, name->s, n*(iz)sizeof(c16));
    r[n] = 0;
    return r;
}

// A link (or junction) at the end of the path is followed by opening the
// file it leads to, which the system finds, and asking for its final
// path. A dangling link is followed as writing through it would be: by
// creating its target, which is discarded at once. Where the system has
// no drive letter path for it (on a volume mounted nowhere, at least),
// its volume is named by GUID, and if the mount manager does not know
// the volume (a RAM disk), by its device, under \\?\GLOBALROOT. Since
// messages name files beside the archive, a target within the directory
// where the user named the archive is named from there, and otherwise,
// a drive or share path loses its \\?\ (winpath restores it). As on
// POSIX, a path that cannot be examined, other than for nothing there,
// cannot be followed, so that zip refuses it before any work, as
// Info-ZIP does on failing to create it then: a name that no file can
// have (<, a component over 255 units), a file that another process
// holds delete-pending, a share or server not found.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    c16 *wpath = winpath(&scratch, path);
    u32  attr  = wpath ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return (s8){0};
    } else if (attr == INVALID_FILE_ATTRIBUTES) {
        return os_missing(ctx) ? path : (s8){0};
    } else if (!(attr & FILE_ATTRIBUTE_REPARSE)) {
        return path;  // no link
    }
    iptr h = CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(wpath, DELETE, FILE_SHARE_ALL, 0, CREATE_NEW,
                        FILE_FLAG_DELETE_ON_CLOSE, 0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return (s8){0};  // e.g. a loop
    }
    u32  how    = VOLUME_NAME_DOS;
    c16 *target = final_path(ctx, h, how, &scratch);
    while (!target && how<VOLUME_NAME_NT) {
        target = final_path(ctx, h, ++how, &scratch);
    }
    CloseHandle(h);
    if (!target) {
        return (s8){0};
    }

    // The directory as named, which for "X:name" is "X:"
    iz cut = path.len;
    for (; cut>0 && path.s[cut-1]!='/' && path.s[cut-1]!='\\'; cut--) {}
    u8 drive = path.len>2 ? (u8)(path.s[0] | 0x20) : 0;
    cut = !cut && drive>='a' && drive<='z' && path.s[1]==':' ? 2 : cut;
    s8   dir  = {path.s, cut};
    c16 *wdir = winpath(&scratch, cut ? dir : S("."));
    iptr dh   = !wdir ? INVALID_HANDLE_VALUE :
                CreateFileW(wdir, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    s16 base = {0};
    if (dh != INVALID_HANDLE_VALUE) {
        c16 *final = final_path(ctx, dh, how, &scratch);
        base = final ? s16lit(final) : base;
        CloseHandle(dh);
    }

    s16 t      = s16lit(target);
    b32 sep    = base.len && base.s[base.len-1]=='\\';  // a root
    iz  len    = base.len + !sep;  // with the separator after it
    b32 within = base.len && len<t.len && (sep || t.s[base.len]=='\\');
    for (iz i = 0; within && i<base.len; i++) {
        within = t.s[i] == base.s[i];
    }
    if (within) {
        return JOIN(perm, dir, towtf8(&scratch, t.s+len));
    } else if (how == VOLUME_NAME_NT) {
        return JOIN(perm, S("\\\\?\\GLOBALROOT"), towtf8(&scratch, t.s));
    } else if (how == VOLUME_NAME_DOS) {
        b32 unc = t.s[4]=='U' && t.s[5]=='N' && t.s[6]=='C' && t.s[7]=='\\';
        t.s += unc ? 6 : 4;  // "\\?\UNC\server" to "\\server"
        t.s[0] = unc ? '\\' : t.s[0];
    }
    return towtf8(perm, t.s);
}

// The file's final path, by which file systems that report no file IDs
// still tell files apart, following links as os_stat does.
static s8 os_fullpath(os *ctx, s8 path, arena *perm, arena scratch)
{
    c16 *wpath = winpath(&scratch, path);
    iptr h     = !wpath ? INVALID_HANDLE_VALUE :
                 CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return (s8){0};
    }
    c16 *full = 0;
    for (u32 how = VOLUME_NAME_DOS; !full && how<=VOLUME_NAME_NT; how++) {
        full = final_path(ctx, h, how, &scratch);
    }
    CloseHandle(h);
    return full ? towtf8(perm, full) : (s8){0};
}

static void release_guard(os *ctx)
{
    if (ctx->guard) {
        CloseHandle(ctx->guard);
        ctx->guard = 0;
    }
}

// Whether the archive may be replaced, found before any work, since the
// rename that replaces it would refuse only after all of it: not if it
// is read-only, which Info-ZIP's port cannot open to update either, nor
// if another process holds it open without sharing delete access, as
// the rename needs, which Info-ZIP's port finds only at the end. Hold it
// with that access, sharing all, until os_commit, so that no process can
// open it so in the meantime. Each fails as that open would.
static b32 os_writable(os *ctx, s8 path, arena scratch)
{
    c16 *wpath = winpath(&scratch, path);
    u32  attr  = wpath ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 1;
    } else if (attr & FILE_ATTRIBUTE_READONLY) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    iptr h = CreateFileW(wpath, DELETE, FILE_SHARE_ALL, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    release_guard(ctx);
    ctx->guard = h;
    return 1;
}

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    stop_if_ending();
    while (len) {
        overlapped ov = {0};
        ov.offset      = (u32)off;
        ov.offset_high = (u32)((u64)off >> 32);
        u32 wrote = 0;
        u32 n     = (u32)MIN(len, 1<<30);
        if (!WriteFile(ctx->handles[fd], buf, n, &wrote, (uptr)&ov) ||
            !wrote) {
            return 0;
        }
        meter(ctx, (s8){0}, 0);
        buf += wrote;
        len -= wrote;
        off += wrote;
    }
    return 1;
}

static b32 os_truncate(os *ctx, i32 fd, i64 len)
{
    return set_info(ctx, ctx->handles[fd], FileEndOfFileInfo, &len,
                    sizeof(len));
}

// Rename by handle while the file is still open, replacing the target
// if asked, so that it is never visible incomplete under its final name
// (the rename refuses a target that is not to be replaced). POSIX
// semantics replace a target that others hold open with delete sharing
// (scanners, indexers), which the classic rename refuses. Flush first,
// while the file is still delete-pending, since deferred write errors
// (network, quotas) may surface only then: closing, the POSIX layer's
// check, comes after the rename. Once renamed, the archive is replaced,
// and closing, after the flush, can lose nothing.
static i32 os_commit(os *ctx, i32 fd, s8 temp, s8 path, b32 replace,
                     arena scratch)
{
    stop_if_ending();
    iptr h = ctx->handles[fd];
    if (!FlushFileBuffers(h)) {
        // A file system that cannot flush (some network and virtual ones)
        // defers nothing
        u32 err = GetLastError();
        if (err!=ERROR_INVALID_FUNCTION && err!=ERROR_NOT_SUPPORTED) {
            os_close(ctx, fd);  // still delete-pending
            release_guard(ctx);
            SetLastError(err);
            return COMMIT_ECLOSE;
        }
    }
    c16 *wpath = winpath(&scratch, path);
    c16 *wtemp = winpath(&scratch, temp);
    if (!wpath || !wtemp) {
        os_close(ctx, fd);
        release_guard(ctx);
        SetLastError(ERROR_INVALID_NAME);
        return COMMIT_EREPLACE;
    }

    // Keep a replaced archive's attributes, as POSIX keeps its mode:
    // hidden, system, and not indexed. Read-only was refused, and the
    // archive bit stays set, as for any changed file. Access control is
    // inherited from the directory, as for any new file, since copying
    // the old would take advapi32.
    u32 old  = GetFileAttributesW(wpath);
    u32 kept = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
               FILE_ATTRIBUTE_NOT_INDEXED;
    if (old!=INVALID_FILE_ATTRIBUTES && (old & kept)) {
        basic_info info = {0};
        info.attributes = (old & kept) | FILE_ATTRIBUTE_ARCHIVE;
        set_info(ctx, h, FileBasicInfo, &info, sizeof(info));
    }

    s16 name = s16lit(wpath);
    iz  size = (iz)sizeof(rename_info) + (name.len+1)*(iz)sizeof(c16);
    rename_info *ri = (rename_info *)newbytes(&scratch, size);
    bytefill(ri, 0, size);
    ri->flags = FILE_RENAME_POSIX | (replace ? FILE_RENAME_REPLACE : 0);
    ri->len   = (u32)(name.len * (iz)sizeof(c16));
    bytecopy(ri->name, name.s, name.len*(iz)sizeof(c16));

    u8 keep = 0;
    if (!set_info(ctx, h, FileDispositionInfo, &keep, 1)) {
        u32 err = GetLastError();
        os_close(ctx, fd);  // still delete-pending
        release_guard(ctx);
        SetLastError(err);
        return COMMIT_EREPLACE;
    }
    if (!set_info(ctx, h, FileRenameInfoEx, ri, (u32)size)) {
        // Older Windows, or a file system without POSIX semantics
        // (FAT, some SMB servers), which also refuses while os_writable
        // holds the archive
        release_guard(ctx);
        ri->flags = !!replace;  // ReplaceIfExists
        if (!set_info(ctx, h, FileRenameInfo, ri, (u32)size)) {
            // Discard it again, else, as when a network session has been
            // lost with the handle, delete it by name once closed
            u32 err     = GetLastError();
            u8  discard = 1;
            b32 marked  = set_info(ctx, h, FileDispositionInfo, &discard, 1);
            os_close(ctx, fd);
            if (!marked) {
                DeleteFileW(wtemp);
            }
            SetLastError(err);
            return COMMIT_EREPLACE;
        }
    }
    release_guard(ctx);
    os_close(ctx, fd);
    return COMMIT_OK;
}

// CRT-free Win32 platform layer for tugz zip
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.


void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    reserve(&ctx);
    zipconfig conf = {0};
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};
    conf.windows = 1;

    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    conf.epoch  = getenv8(&conf.perm, L"SOURCE_DATE_EPOCH");
    conf.zipopt = getenv8(&conf.perm, L"ZIPOPT");
    conf.zipenv = getenv8(&conf.perm, L"ZIP");
    os_exit(&ctx, zip_main(&conf));
}
