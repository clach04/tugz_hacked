// tugz unzip 1.3.0: an Info-ZIP compatible unzip for Windows
// Single-file amalgamation of the tugz sources. Build:
//   $ cc -O2 -nostartfiles -o unzip.exe unzip.c -lmemory
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.
// A copy named zipinfo.exe lists archives as Info-ZIP's zipinfo.

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

// tugz unzip rules: entry names to paths, times, owners, modes, display
//
// No I/O, as in zip.c: the unzip program applies these. Each follows
// Info-ZIP UnZip 6.0's source, named at each function, and the behavior
// of its builds as recorded in test/unziptests.c: macOS's /usr/bin/unzip
// and Debian's (6.0-29, with Unicode, ISO dates, and owner restoring),
// taken where the two differ. Hosts are UnZip's numbers from the upper
// byte of "version made by", where anything past 30 counts as 31.

enum {
    UZ_FAT    = 0,
    UZ_AMIGA  = 1,
    UZ_VMS    = 2,
    UZ_UNIX   = 3,
    UZ_ATARI  = 5,
    UZ_QDOS   = 12,
    UZ_ACORN  = 13,
    UZ_BEOS   = 16,
    UZ_TANDEM = 17,
    UZ_THEOS  = 18,
    UZ_ATHEOS = 30,
    UZ_HOSTS  = 31,  // UnZip's NUM_HOSTS, for any later host
};

#define UZ_EXTRA_PKVMS   0x000c  // PKWARE's VMS
#define UZ_EXTRA_PKUNIX  0x000d  // PKWARE's Unix, laid out as "UX"
#define UZ_EXTRA_ASIUNIX 0x756e  // "nu": ASi's Unix, with a mode
#define UZ_DOS2038       0x74320000u  // UnZip's DOSTIME_2038_01_18

static u32 uz_host(u32 made)
{
    return MIN(made>>8, UZ_HOSTS);
}

// Options for uz_mapname.
enum {
    UZ_WINDOWS = 1 << 0,  // map for Windows file systems
    UZ_JUNK    = 1 << 1,  // -j: keep only the last component
    UZ_KEEPVER = 1 << 2,  // -V: keep a ";N" version suffix
};

// Results of uz_mapname, each a message of UnZip's. The warnings are
// status 1, a failure status 2.
enum {
    // "warning:  %s appears to use backslashes as path separators\n",
    // with the archive's name, once per archive, to stderr
    UZ_BACKSLASH = 1 << 0,
    // "warning:  stripped absolute path spec from %s\n", with
    // uzpath.full, to stderr
    UZ_ABSOLUTE  = 1 << 1,
    // "warning:  skipped \"../\" path component(s) in %s\n", with
    // uzpath.name, to stdout, before the entry's own line
    UZ_DOTDOT    = 1 << 2,
    // A directory entry, whose path, if empty, creates nothing, silently
    UZ_DIR       = 1 << 3,
    // "mapname:  conversion of %s failed\n", with uzpath.name, to
    // stderr: the entry is skipped
    UZ_FAILED    = 1 << 4,
};

typedef struct {
    s8  path;   // relative, '/'-separated, without a trailing '/'
    s8  full;   // the name, its '\' converted (UZ_BACKSLASH)
    s8  name;   // that, without leading '/' (UZ_ABSOLUTE)
    i32 flags;
} uzpath;

// Whether a name, up to any extension and without trailing spaces, is a
// Windows device's (CON, PRN, AUX, NUL, COM0-9 and LPT0-9 with the
// superscript digits too, CONIN$, CONOUT$, CLOCK$), in any case.
static b32 uz_device(u8 const *s, iz len)
{
    iz n = 0;
    for (; n<len && s[n]!='.'; n++) {}
    for (; n>0 && s[n-1]==' '; n--) {}
    u8 up[8] = {0};
    if (n > countof(up)) {
        return 0;
    }
    for (iz i = 0; i < n; i++) {
        up[i] = s[i]>='a' && s[i]<='z' ? (u8)(s[i] - 32) : s[i];
    }
    static u8 const names[][8] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$",
        "COM", "LPT",  // followed by a digit
    };
    for (i32 k = 0; k < countof(names); k++) {
        b32 port = k >= countof(names)-2;
        iz  i    = 0;
        for (; i<countof(up) && names[k][i] && up[i]==names[k][i]; i++) {}
        if (i<countof(up) && names[k][i]) {
            continue;  // a mismatch
        } else if (!port && i==n) {
            return 1;
        } else if (port) {
            b32 digit = n==4 && up[3]>='0' && up[3]<='9';
            b32 super = n==5 && up[3]==0xc2 &&
                        (up[4]==0xb9 || up[4]==0xb2 || up[4]==0xb3);
            if (digit || super) {
                return 1;
            }
        }
    }
    return 0;
}

// Finish a Windows path component, p[beg..*n): drop trailing dots and
// spaces, which Windows would drop, so that a name ending in them cannot
// name another file (tugz's own rule), then put '_' before a device's
// name, as UnZip's maskDOSdevice puts it before a name that the system
// takes for a character device.
static void uz_wincomp(u8 *p, iz beg, iz *n)
{
    for (; *n>beg && (p[*n-1]=='.' || p[*n-1]==' '); (*n)--) {}
    if (*n>beg && uz_device(p+beg, *n-beg)) {
        bytemove(p+beg+1, p+beg, *n-beg);
        p[beg] = '_';
        (*n)++;
    }
}

// Map an entry's name to a relative path to extract it to, with UnZip's
// checks in its order (extract.c, extract_or_test_member), then its
// mapname (unix/unix.c, or win32/win32.c with UZ_WINDOWS):
// 1. Made on MS-DOS (FAT), without a '/', '\' separates (UZ_BACKSLASH).
// 2. Leading '/' are stripped (UZ_ABSOLUTE).
// 3. With UZ_JUNK, only what follows the last '/' is kept.
// 4. Empty and "." components are dropped, as are ".." (UZ_DOTDOT).
// 5. Control characters are dropped: on POSIX all but 0x20-0x7e and
//    0x80-0xfe; on Windows all below 0x20.
// 6. A final ";N", N any digits, is dropped, unless UZ_KEEPVER.
// 7. On POSIX, a final "." becomes "_", and ".." "__".
// 8. On Windows, ':', '\', '<', '>', '|', '"', '?', and '*' become '_';
//    trailing dots and spaces are dropped from components (not UnZip's:
//    UnZip relies on the system's dropping them); and device names get a
//    '_' in front (UnZip asks the system which are devices).
// 9. A file whose final component is left empty fails (UZ_FAILED).
// A name ending in '/' is a directory (UZ_DIR). The path never holds
// ".." components, a leading '/', NUL or control characters, nor on
// Windows drive letters, streams, or device names.
[[maybe_unused]]
static uzpath uz_mapname(s8 name, u32 made, i32 opts, arena *perm)
{
    uzpath r = {0};
    b32 win  = opts & UZ_WINDOWS;

    s8 n = {newstr(perm, name.len), name.len};
    bytecopy(n.s, name.s, n.len);
    b32 slash = 0;
    for (iz i = 0; i < n.len; i++) {
        slash |= n.s[i] == '/';
    }
    if (uz_host(made)==UZ_FAT && !slash) {
        for (iz i = 0; i < n.len; i++) {
            if (n.s[i] == '\\') {
                n.s[i] = '/';
                r.flags |= UZ_BACKSLASH;
            }
        }
    }
    r.full = n;
    iz skip = 0;
    for (; skip<n.len && n.s[skip]=='/'; skip++) {}
    r.flags |= skip ? UZ_ABSOLUTE : 0;
    n.s   += skip;
    n.len -= skip;
    r.name = n;

    b32 dir = n.len && n.s[n.len-1]=='/';
    iz  i   = 0;
    if (opts & UZ_JUNK) {
        for (iz k = 0; k < n.len; k++) {
            i = n.s[k]=='/' ? k+1 : i;
        }
    }

    // Each component grows by at most a '_' before it
    u8 *p    = newstr(perm, 2*(n.len - i) + 1);
    iz  len  = 0;
    iz  beg  = 0;   // of the component being built
    iz  semi = -1;  // its last ';'
    for (; i < n.len; i++) {
        u8 c = n.s[i];
        if (c == '/') {
            s8 comp = {p+beg, len-beg};
            if (comp.len==1 && comp.s[0]=='.') {
                len = beg;
            } else if (comp.len==2 && comp.s[0]=='.' && comp.s[1]=='.') {
                len = beg;
                r.flags |= UZ_DOTDOT;
            } else if (win) {
                uz_wincomp(p, beg, &len);
            }
            if (len > beg) {
                p[len++] = '/';
            }
            beg  = len;
            semi = -1;
        } else if (win && (c==':' || c=='\\' || c=='<' || c=='>' ||
                           c=='|' || c=='"' || c=='?' || c=='*')) {
            p[len++] = '_';
        } else if (c == ';') {
            semi = len;
            p[len++] = c;
        } else if ((c>=0x20 && c<0x7f) || (c>=0x80 && c<0xff) ||
                   (win && c>=0x7f)) {
            p[len++] = c;
        }
    }

    if (dir) {
        len -= len > 0;  // the separator after the last component
        r.path   = (s8){p, len};
        r.flags |= UZ_DIR;
        return r;
    }

    if (!(opts & UZ_KEEPVER) && semi>=0) {
        iz k = semi + 1;
        for (; k<len && p[k]>='0' && p[k]<='9'; k++) {}
        len = k==len ? semi : len;
    }
    s8 comp = {p+beg, len-beg};
    if (win) {
        uz_wincomp(p, beg, &len);
    } else if (comp.len==1 && comp.s[0]=='.') {
        p[beg] = '_';
    } else if (comp.len==2 && comp.s[0]=='.' && comp.s[1]=='.') {
        p[beg] = p[beg+1] = '_';
    }
    if (len == beg) {
        r.flags |= UZ_FAILED;
        return r;
    }
    r.path = (s8){p, len};
    return r;
}

// The fields of an MS-DOS date<<16 | time as UnZip lists them, not
// normalized (list.c): {year 1980-2107, month 0-15, day 0-31, hour 0-31,
// minute 0-63, second 0-62}.
static void uz_dosdate(u32 dostime, i32 tm[6])
{
    tm[0] = (i32)(dostime >> 25) + 1980;
    tm[1] = (i32)(dostime>>21 & 15);
    tm[2] = (i32)(dostime>>16 & 31);
    tm[3] = (i32)(dostime>>11 & 31);
    tm[4] = (i32)(dostime>>5 & 63);
    tm[5] = (i32)(dostime & 31) * 2;
}

// Unix seconds from a broken-down UTC time {year, month 1-12, day, hour,
// minute, second}, the inverse of zip_gmtime. Fields out of range carry
// over, as with mktime: month 13 is January of the next year, day 0 the
// last of the month before, hour 24 midnight of the next day. As UnZip's
// dos_to_unix_time computes it (fileio.c, without mktime), so do the
// DOS times with months 1-13 through 2100 that its builds extract; it
// reads months 0, 14, and 15 from outside its table of days (ydays[]),
// and counts 2100 a leap year for the years after it, a day late.
[[maybe_unused]] static i64 uz_timegm(i32 const tm[6])
{
    i64 m = (i64)tm[1] - 1;
    i64 q = m / 12 - (m%12 < 0);
    i64 y = tm[0] + q;
    m = m - q*12 + 1;
    // Howard Hinnant's days_from_civil
    y -= m <= 2;
    i64 era  = (y >= 0 ? y : y-399) / 400;
    i64 yoe  = y - era*400;
    i64 doy  = (153*(m + (m>2 ? -3 : 9)) + 2)/5 + tm[2] - 1;
    i64 doe  = yoe*365 + yoe/4 - yoe/100 + doy;
    i64 days = era*146097 + doe - 719468;
    return days*86400 + (i64)tm[3]*3600 + (i64)tm[4]*60 + tm[5];
}

// Flags of uz_extra_izux: the times it found, and the owner.
enum {
    UZ_MTIME = 1 << 0,
    UZ_ATIME = 1 << 1,
    UZ_CTIME = 1 << 2,
    UZ_OWNER = 1 << 8,
};

typedef struct {
    i64 mtime;
    i64 atime;
    i64 ctime;
    u32 uid;
    u32 gid;
    i32 flags;
} uzizux;

// Read a 2, 4, or 8-byte owner ID, only if it fits 32 bits, as Debian's
// UnZip reads and restores them (read_ux3_value, close_outfile).
static b32 uz_id(u8 const *p, iz size, u32 *id)
{
    u64 v = 0;
    switch (size) {
    case 2: v = get16(p);  break;
    case 4: v = get32(p);  break;
    case 8: v = get64(p);  break;
    default: return 0;
    }
    *id = (u32)v;
    return v <= 0xffffffff;
}

// The times and owner among an entry's extra fields, local or central,
// as UnZip's ef_scan_for_izux (process.c) scans them:
// - An extended timestamp ("UT") gives the times its flags announce,
//   only the modification time in its central form. It overrides any
//   earlier times, and leaves later old Unix ("UX", or PKWARE's) fields
//   ignored. The last of several wins.
// - "UX" gives modification and access times, and from 12 bytes an
//   owner, if no "UT", "Ux" or "ux" came before it.
// - The newer Unix fields give only an owner: "Ux" two 16-bit IDs, in
//   exactly 4 bytes, unless after a "ux"; "ux", of version 1, IDs of 2,
//   4, or 8 bytes. Each drops any earlier owner, keeping times, as in
//   Debian's build (with IZ_HAVE_UXUIDGID, which UnZip 6.0 leaves out,
//   and fixes: macOS's drops "UX" times after a "Ux"). A "ux" whose IDs
//   run past its end has none (Debian's reads past it), nor one with an
//   ID past 32 bits (Debian's reads it but does not restore it).
// - Times are unsigned 32-bit. One with bit 31 set counts only with a
//   DOS time from 2038-01-18 on: else a modification time voids the
//   field's times, and another time just itself.
// Where UnZip leaves a time announced but unread (the access time of a
// central "UT"), it is not reported here.
static uzizux uz_extra_izux(s8 x, b32 central, u32 dostime)
{
    uzizux r      = {0};
    i32    flags  = 0;  // times in the low byte, as UnZip keeps them
    i32    newer  = 0;  // UnZip's have_new_type_eb: 1 after UT/Ux, 2 ux
    b32    compat = 0;  // high times are unsigned, the DOS time agreeing
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        u8 *d   = x.s + i + 4;
        if (len > x.len-i-4) {
            break;
        }
        i += 4 + len;

        switch (id) {
        case ZIP_EXTRA_TIME:
            flags &= ~0xff;
            newer  = 1;
            if (len < 1) {
                break;
            }
            flags |= d[0];
            iz at = 1;
            if (flags & UZ_MTIME) {
                if (at+4 > len) {
                    flags &= ~UZ_MTIME;
                } else {
                    u32 t = get32(d+at);
                    at += 4;
                    compat = t>>31 && dostime>=UZ_DOS2038;
                    if (t>>31 && !compat) {
                        flags &= ~0xff;
                        break;
                    }
                    r.mtime = t;
                }
            }
            if (central) {
                flags &= ~(UZ_ATIME | UZ_CTIME);
                break;
            }
            i64 *times[] = {&r.atime, &r.ctime};
            i32  bits[]  = {UZ_ATIME, UZ_CTIME};
            for (i32 k = 0; k < 2; k++) {
                if (!(flags & bits[k])) {
                    continue;
                } else if (at+4 > len) {
                    flags &= ~bits[k];
                    continue;
                }
                u32 t = get32(d+at);
                at += 4;
                if (t>>31 && !compat) {
                    flags &= ~bits[k];
                } else {
                    *times[k] = t;
                }
            }
            break;

        case ZIP_EXTRA_UNIX2:  // "Ux"
            newer  = newer ? newer : 1;
            if (newer > 1) {
                break;
            }
            flags &= 0xff;
            if (len == 4) {
                r.uid  = get16(d);
                r.gid  = get16(d+2);
                flags |= UZ_OWNER;
            }
            break;

        case ZIP_EXTRA_UNIX:  // "ux"
            newer  = 2;
            flags &= 0xff;
            if (len>=7 && d[0]==1) {
                iz us = d[1];
                iz gs = 2+us < len ? d[2+us] : len;
                if (3+us+gs<=len && uz_id(d+2, us, &r.uid) &&
                    uz_id(d+3+us, gs, &r.gid)) {
                    flags |= UZ_OWNER;
                }
            }
            break;

        case ZIP_EXTRA_UNIX1:  // "UX"
        case UZ_EXTRA_PKUNIX:
            if (len<8 || newer) {
                break;
            }
            flags |= UZ_MTIME | UZ_ATIME;
            u32 m = get32(d+4);
            u32 a = get32(d);
            compat = m>>31 && dostime>=UZ_DOS2038;
            if (m>>31 && !compat) {
                flags &= ~0xff;
            }
            r.mtime = m;
            if (a>>31 && !compat && (flags & 0xff)) {
                flags &= ~UZ_ATIME;
            } else {
                r.atime = a;
            }
            if (len >= 12) {
                r.uid  = get16(d+8);
                r.gid  = get16(d+10);
                flags |= UZ_OWNER;
            }
            break;
        }
    }
    r.flags = flags & (UZ_MTIME | UZ_ATIME | UZ_CTIME | UZ_OWNER);
    return r;
}

// An entry's mode, as UnZip's mapattr derives it for POSIX
// (unix/unix.c), from the version made by, the external attributes,
// the name, and central extra fields:
// - From Unix-like hosts (Unix, VMS, Acorn, Atari, AtheOS, BeOS, QDOS,
//   Tandem, THEOS), the upper 16 bits; if zero, an ASi Unix field's
//   mode, else PKWARE VMS's or a short ASi field mean DOS attributes.
// - From Amiga, its read, write, and execute bits for all.
// - Otherwise, from the DOS attributes: write unless read-only, and
//   search for a directory (a name ending in '/' is one), for all,
//   unless, from MS-DOS, the upper 16 bits agree with them for the
//   owner: then those.
// - Without keep (-K), set-ID and sticky bits are dropped (filtattr).
// Modes made from DOS or Amiga attributes take the umask (umask set).
// The mode keeps its file type: a link is a link only from the hosts
// uz_symlink_host names, or from MS-DOS with agreeing attributes.
typedef struct {
    u32 mode;     // as st_mode, its type included
    b32 umask;    // apply the process's umask
    b32 symlink;
} uzmode;

static b32 uz_islink(u32 mode)
{
    return (mode & 0170000) == 0120000;
}

static b32 uz_symlink_host(u32 made);

[[maybe_unused]]
static uzmode uz_mode(u32 extattr, u32 made, s8 name, s8 cextra, b32 keep)
{
    uzmode r    = {0};
    u32    host = uz_host(made);
    u32    tmp  = extattr;
    b32    dos  = 0;
    switch (host) {
    case UZ_AMIGA:
        tmp = tmp>>17 & 7;
        r.mode  = tmp<<6 | tmp<<3 | tmp;
        r.umask = 1;
        break;

    case UZ_THEOS:
        tmp &= 0xf1ffffff;
        tmp &= (tmp & 0xf0000000)==0x40000000 ? 0x41ffffff : 0x01ffffff;
        [[fallthrough]];
    case UZ_UNIX: case UZ_VMS: case UZ_ACORN: case UZ_ATARI:
    case UZ_ATHEOS: case UZ_BEOS: case UZ_QDOS: case UZ_TANDEM:
        r.mode = tmp >> 16;
        for (iz i = 0; !r.mode && !dos && cextra.len-i>=4;) {
            u32 id  = get16(cextra.s+i);
            iz  len = get16(cextra.s+i+2);
            if (len > cextra.len-i-4) {
                break;
            }
            if (id==UZ_EXTRA_ASIUNIX && len>=6) {
                r.mode = get16(cextra.s+i+8);
                break;
            }
            dos = id==UZ_EXTRA_ASIUNIX || id==UZ_EXTRA_PKVMS;
            i += 4 + len;
        }
        if (!dos) {
            r.symlink = uz_islink(r.mode) && uz_symlink_host(made);
            break;
        }
        [[fallthrough]];
    default:
        r.mode = host==UZ_FAT || dos ? tmp>>16 : 0;
        if (!(tmp & 0x10) && name.len && name.s[name.len-1]=='/') {
            tmp |= 0x10;
        }
        tmp = (u32)!(tmp & 1)<<1 | (tmp & 0x10)>>4;
        if ((r.mode & 0700) == (0400 | tmp<<6)) {
            r.symlink = uz_islink(r.mode) && host==UZ_FAT;
            break;
        }
        r.mode  = 0444 | tmp<<6 | tmp<<3 | tmp;
        r.umask = 1;
    }
    r.mode &= keep ? 0xffff : 0xffff & ~07000u;
    return r;
}

// Whether a host makes POSIX symbolic links, as UnZip's SYMLINK_HOST:
// Unix, VMS, Atari, BeOS, and AtheOS.
static b32 uz_symlink_host(u32 made)
{
    u32 h = uz_host(made);
    return h==UZ_UNIX || h==UZ_VMS || h==UZ_ATARI || h==UZ_BEOS ||
           h==UZ_ATHEOS;
}

// An entry's Windows file attributes, as UnZip's mapattr derives them
// for Windows (win32/win32.c), from the low byte of the external
// attributes whatever the host, archive set for files, limited to the
// bits it then sets (0x7f): read-only 0x01, hidden 0x02, system 0x04,
// and archive 0x20 among them.
[[maybe_unused]] static u32 uz_dosattr(u32 extattr)
{
    u32 a = extattr | (extattr & 0x10 ? 0 : 0x20);
    return a & 0x7f;
}

// UnZip's ratio (list.c): the compression factor in tenths of a percent,
// negative for growth. Where UnZip's arithmetic overflows, as for a
// Zip64 size with an encrypted entry's compressed size under 12 (less
// its header, wrapped), this computes without overflow and holds the
// growth to what leaves room for rounding in an i32.
static i32 uz_ratio(u64 uc, u64 c)
{
    u64 d = uc>=c ? uc-c : c-uc;
    u64 q = 0;
    if (!uc) {
        return 0;
    } else if (uc > 2000000) {
        u64 denom = uc / 1000;
        q = d/denom + (d%denom + (denom>>1))/denom;
    } else if (d > ((u64)-1 - (uc>>1))/1000) {
        q = (u64)-1;
    } else {
        q = (1000*d + (uc>>1)) / uc;
    }
    q = MIN(q, (u64)0x7fffffff - 5);  // as is any compression, under 1000
    return uc>=c ? (i32)q : -(i32)q;
}

// A name made safe to display, after UnZip's fnfilter (extract.c):
// control characters below 0x20 as '^' and a letter ("^A", "^["), and
// DEL as '?'. Where UnZip relies on the locale for the rest, this takes
// names as UTF-8: C1 controls (U+0080-U+009F) and each byte of anything
// not UTF-8 also become '?'.
static s8 uz_filter(s8 s, arena *perm)
{
    s8 r = {newstr(perm, 2*s.len), 0};
    for (iz i = 0; i < s.len;) {
        u32 c = s.s[i];
        if (c < 0x20) {
            r.s[r.len++] = '^';
            r.s[r.len++] = (u8)(c + 64);
            i++;
            continue;
        } else if (c < 0x7f) {
            r.s[r.len++] = (u8)c;
            i++;
            continue;
        }
        iz n = c>=0xc2 && c<0xe0 ? 2 : c>=0xe0 && c<0xf0 ? 3 :
               c>=0xf0 && c<0xf5 ? 4 : 0;
        b32 ok = n && n<=s.len-i && zip_utf8((s8){s.s+i, n})==1;
        b32 c1 = ok && c==0xc2 && s.s[i+1]<0xa0;
        if (!ok || c1) {
            r.s[r.len++] = '?';
            i += ok ? n : 1;
        } else {
            bytecopy(r.s+r.len, s.s+i, n);
            r.len += n;
            i     += n;
        }
    }
    return r;
}

// tugz zipinfo rules: ZipInfo mode's options, its listings' fields, and
// the text of its verbose report (-v)
//
// No I/O, as in unzip.c: the unzip program's ZipInfo mode (unzip -Z, or
// the program run by the name zipinfo) applies these. Each follows
// Info-ZIP UnZip 6.0's zipinfo.c (ZipInfo 3.00), named at each function,
// and the behavior of its builds as recorded in test/unziptests.c. The
// fields are short, each built in a zistr of its own; -v's longer text,
// a section or a subfield's description at a time, in a zitext.

typedef struct {
    u8 s[32];
    iz len;
} zistr;

static void zi_putc(zistr *z, u8 c)
{
    z->s[z->len++] = c;
}

static void zi_puts(zistr *z, char const *s)
{
    for (; *s; s++) {
        zi_putc(z, (u8)*s);
    }
}

// An unsigned number, as "%wu", or with zero padding, "%0wu".
static void zi_putu(zistr *z, u64 v, iz w, b32 zero)
{
    u8  tmp[20];
    iz  n = 0;
    do {
        tmp[n++] = (u8)('0' + v%10);
    } while (v /= 10);
    for (; w > n; w--) {
        zi_putc(z, zero ? '0' : ' ');
    }
    while (n) {
        zi_putc(z, tmp[--n]);
    }
}

static s8 zi_s8(zistr *z)
{
    return (s8){z->s, z->len};
}

// ZipInfo's options, as zi_opts keeps them (zipinfo.c)
typedef struct {
    i32 lflag;  // the format: 1 -1, 2 -2, 3 -s, 4 -m, 5 -l, 10 -v; or
                // -1 none yet, -2 one negated, 0 only -h or -t
    i32 neg;    // negations ('-') pending, carried into the next word
    b32 hslmv;  // the header for -s, -m, -l, and -v,
    b32 h2;     // and for -2 (and -h or -t alone)
    b32 tslm;   // the totals likewise,
    b32 t2v;    // for -2 and -v
    b32 exph;   // -h given
    b32 expt;   // -t given

    // Once resolved (zi_resolve)
    b32 hflag;  // the header lines
    b32 tflag;  // the totals line
    b32 zflag;  // the archive comment
    b32 Cflag;  // names matched ignoring case
    b32 Tflag;  // times in decimal
} ziopts;

static ziopts zi_new(void)
{
    ziopts z = {0};
    z.lflag = -1;
    z.hslmv = z.tslm = 1;
    return z;
}

// Results of zi_option
enum {
    ZI_OK,
    ZI_BAD,      // unknown: the usage follows (10)
    ZI_REFUSED,  // ZipInfo's, but not tugz's: -M (the pager), -U
};

// Apply an option letter as zi_opts does: '-' negates the letter after
// it, even in the next word; a negated format leaves none chosen; -h and
// -t alone choose the format of neither (0); Z does nothing.
static i32 zi_option(ziopts *z, u8 c)
{
    b32 neg  = z->neg > 0;
    i32 kept = z->neg;
    z->neg = 0;
    switch (c) {
    case '-':
        z->neg = kept + 1;
        return ZI_OK;
    case '1': case '2': case 's': case 'm': case 'l': case 'v':
        z->lflag = neg ? -2 : c=='1' ? 1 : c=='2' ? 2 : c=='s' ? 3 :
                   c=='m' ? 4 : c=='l' ? 5 : 10;
        return ZI_OK;
    case 'h':
        z->hslmv = z->h2 = !neg;
        z->exph |= !neg;
        z->lflag = !neg && z->lflag==-1 ? 0 : z->lflag;
        return ZI_OK;
    case 't':
        z->tslm = z->t2v = !neg;
        z->expt |= !neg;
        z->lflag = !neg && z->lflag==-1 ? 0 : z->lflag;
        return ZI_OK;
    case 'C':
        z->Cflag = !neg;
        return ZI_OK;
    case 'T':
        z->Tflag = !neg;
        return ZI_OK;
    case 'z':
        z->zflag = !neg;
        return ZI_OK;
    case 'Z':
        z->neg = kept;  // ignored, negations and all
        return ZI_OK;
    case 'M': case 'U':
        z->neg = kept;
        return ZI_REFUSED;
    }
    z->neg = kept;
    return ZI_BAD;
}

// Settle the format and which of the header, totals, and comment show,
// as zi_opts does once the options are read, given whether any word
// follows the archive: by default, and with members if only -h or -t
// was given, the short format (-s). Members turn off the header and
// totals of -s, -m, and -l unless they were asked for; -1 shows neither,
// nor the comment.
static void zi_resolve(ziopts *z, b32 members)
{
    if (z->lflag<0 || (members && z->lflag==0)) {
        z->lflag = 3;
    }
    switch (z->lflag) {
    case 0:
    case 2:
        z->hflag = z->h2;
        z->tflag = z->t2v;
        break;
    case 1:
        z->hflag = z->tflag = z->zflag = 0;
        break;
    case 3:
    case 4:
    case 5:
        z->hflag = members && !z->exph ? 0 : z->hslmv;
        z->tflag = members && !z->expt ? 0 : z->tslm;
        break;
    case 10:
        z->hflag = z->hslmv;
        z->tflag = z->t2v;
        break;
    }
}

static u8 zi_lower(u8 c)
{
    return c>='A' && c<='Z' ? (u8)(c + 32) : c;
}

// Whether s begins with prefix, ignoring ASCII case, as STRNICMP.
static b32 zi_prefix(s8 s, char const *prefix)
{
    iz i = 0;
    for (; prefix[i]; i++) {
        if (i>=s.len || zi_lower(s.s[i])!=(u8)prefix[i]) {
            return 0;
        }
    }
    return 1;
}

// Whether the program, by the name it was run as, is ZipInfo: a base
// name starting with "zipinfo" or "ii", in any case, as unzip.c finds it
// (so "ZipInfo.exe" too), after the last '/', or with Windows
// conventions, '/' or '\'.
static b32 zi_named(s8 argv0, b32 windows)
{
    iz beg = 0;
    for (iz i = 0; i < argv0.len; i++) {
        u8 c = argv0.s[i];
        beg = c=='/' || (windows && c=='\\') ? i+1 : beg;
    }
    s8 base = {argv0.s+beg, argv0.len-beg};
    return zi_prefix(base, "zipinfo") || zi_prefix(base, "ii");
}

// The host's three letters, by the upper byte of "version made by", as
// zi_short's os[], hosts past 30 taking the last, "???".
static s8 zi_host(u32 made)
{
    static char const hosts[UZ_HOSTS+1][4] = {
        "fat", "ami", "vms", "unx", "cms", "atr", "hpf", "mac", "zzz",
        "cpm", "t20", "ntf", "qds", "aco", "vft", "mvs", "be ", "nsk",
        "ths", "osx", "???", "???", "???", "???", "???", "???", "???",
        "???", "???", "???", "ath", "???",
    };
    return (s8){(u8 *)hosts[uz_host(made)], 3};
}

// Hosts with attributes of their own (zi_short)
enum {
    ZI_CMS   = 4,
    ZI_HPFS  = 6,
    ZI_NTFS  = 11,
    ZI_VFAT  = 14,
    ZI_MVS   = 15,
};

// An entry's attributes as zi_short shows them, from its external
// attributes, host, and name, with the version that made it: a field of
// 15, the version from column 12, as "-rw-r--r--  3.0":
// - VMS: R, W, E, D for owner, group, and other, from the Unix bits,
//   as "RWED,RWED,RE", the version only where they leave room.
// - Amiga: its type (d, -, ?), then hsparwed.
// - THEOS: its type, then a letter for each bit not set.
// - MS-DOS and the hosts with its attributes (OS/2 HPFS, NTFS, VM/CMS,
//   MVS, Acorn, VFAT): the low byte as d or - (V for a volume label),
//   r, w unless read-only, x for a directory or a name whose last '.'
//   is followed by com, exe, btm, cmd, or bat (in any case), a, h, s.
//   But from MS-DOS, Unix bits that agree with those, for the owner, as
//   an Info-ZIP zip for Unix writes them, show as from Unix.
// - Any other host, Unix's: its type (d - l b c p s, else ?), and rwx,
//   with set-ID (s, S) and sticky (t, T) bits.
static zistr zi_attribs(u32 made, u32 extattr, s8 name)
{
    zistr r     = {0};
    u32   host  = uz_host(made);
    u32   ver   = made & 0xff;
    u32   x     = extattr>>16 & 0xffff;
    u8    a[16] = "               ";
    iz    vms   = -1;  // where VMS's permissions end
    switch (host) {
    case UZ_VMS: {
        u8  w[12] = {0};
        u32 bits[] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
        for (i32 k = 0; k < 3; k++) {
            w[4*k+0] = x & bits[3*k+0] ? 'R' : 0;
            w[4*k+1] = x & bits[3*k+1] ? 'W' : 0;
            w[4*k+2] = x & bits[3*k+2] ? 'E' : 0;
            w[4*k+3] = x & bits[3*k+1] ? 'D' : 0;
        }
        iz p = 0;
        for (i32 k = 0; k < 12; k++) {
            if (w[k]) {
                a[p++] = w[k];
            }
            if (k%4 == 3) {
                a[p++] = ',';
            }
        }
        a[--p] = ' ';
        vms = p;
    } break;

    case UZ_AMIGA:
        a[0] = (x & 06000)==04000 ? 'd' : (x & 06000)==02000 ? '-' : '?';
        for (i32 k = 0; k < 8; k++) {
            a[1+k] = x & 0200u>>k ? (u8)"hsparwed"[k] : '-';
        }
        break;

    case UZ_THEOS: {
        static char const types[] = "??C?DL??SRKI?P23";
        a[0] = (u8)types[x>>12 & 15];
        u32 bits[] = {0x400, 0x800, 0x002, 0x004, 0x200, 0x040, 0x080, 0x100};
        for (i32 k = 0; k < 8; k++) {
            a[1+k] = x & bits[k] ? '.' : (u8)"HMWREXWR"[k];
        }
    } break;

    case UZ_FAT: case ZI_HPFS: case ZI_NTFS: case ZI_CMS: case ZI_MVS:
    case UZ_ACORN: case ZI_VFAT:
        if (host==UZ_FAT && (x & 0700)==(0400 | (u32)!(extattr & 1)<<7 |
                                        (extattr & 0x10)<<2)) {
            goto unixlike;  // as from Unix, by agreeing bits
        }
        x = extattr & 0xff;
        bytecopy(a, ".r.-...", 7);
        a[2] = x & 0x01 ? '-' : 'w';
        a[5] = x & 0x02 ? 'h' : '-';
        a[6] = x & 0x04 ? 's' : '-';
        a[4] = x & 0x20 ? 'a' : '-';
        a[0] = x & 0x10 ? 'd' : '-';
        a[3] = x & 0x10 ? 'x' : a[3];
        if (x & 0x08) {
            a[0] = 'V';
        } else {
            iz dot = -1;
            for (iz i = 0; i < name.len; i++) {
                dot = name.s[i]=='.' ? i : dot;
            }
            s8 ext = {name.s+dot+1, name.len-dot-1};
            if (dot>=0 && (zi_prefix(ext, "com") || zi_prefix(ext, "exe") ||
                           zi_prefix(ext, "btm") || zi_prefix(ext, "cmd") ||
                           zi_prefix(ext, "bat"))) {
                a[3] = 'x';
            }
        }
        break;

    default:
    unixlike:
        switch (x & 0170000) {
        case 0040000: a[0] = 'd'; break;
        case 0100000: a[0] = '-'; break;
        case 0120000: a[0] = 'l'; break;
        case 0060000: a[0] = 'b'; break;
        case 0020000: a[0] = 'c'; break;
        case 0010000: a[0] = 'p'; break;
        case 0140000: a[0] = 's'; break;
        default:      a[0] = '?';
        }
        for (i32 k = 0; k < 3; k++) {
            u32 m = x >> (6 - 3*k);
            a[1+3*k] = m & 4 ? 'r' : '-';
            a[2+3*k] = m & 2 ? 'w' : '-';
        }
        a[3] = x & 0100 ? (x & 04000 ? 's' : 'x') : (x & 04000 ? 'S' : '-');
        a[6] = x & 0010 ? (x & 02000 ? 's' : 'x') : (x & 02000 ? 'S' : '-');
        a[9] = x & 0001 ? (x & 01000 ? 't' : 'x') : (x & 01000 ? 'T' : '-');
    }

    // A version from 10.0 begins a column early, as UnZip's builds show
    // it with Unix's attributes (they abort with the others' instead,
    // their buffer overrun), and with VMS's only where those leave room
    iz at = ver<100 ? 12 : 11;
    if (vms < at) {
        bytecopy(r.s, a, at);
        r.len = at;
        zi_putu(&r, ver/10, 0, 0);
        zi_putc(&r, '.');
        zi_putu(&r, ver%10, 0, 0);
    } else {
        bytecopy(r.s, a, 15);
        r.len = 15;
    }
    return r;
}

// An entry's method as zi_short abbreviates it: "stor", "defN", "i8:3",
// or for one it does not know, "u" and its number, "u099" (UnZip 6.0
// overflows its buffer from u1000, where distributions' patches stop).
// Deflation's level and imploding's dictionary and trees come from
// flag bits 1 and 2.
static zistr zi_method(u32 method, u32 flags)
{
    static struct { u16 id; char name[5]; } const methods[] = {
        {0, "stor"}, {1, "shrk"}, {2, "re:1"}, {3, "re:2"}, {4, "re:3"},
        {5, "re:4"}, {6, "i#:#"}, {7, "tokn"}, {8, "def#"}, {9, "d64#"},
        {10, "dcli"}, {12, "bzp2"}, {14, "lzma"}, {18, "ters"},
        {19, "lz77"}, {97, "wavp"}, {98, "ppmd"},
    };
    zistr r = {0};
    for (i32 k = 0; k < countof(methods); k++) {
        if (methods[k].id == method) {
            zi_puts(&r, methods[k].name);
        }
    }
    if (!r.len) {
        zi_putc(&r, 'u');
        zi_putu(&r, method, 3, 1);
    } else if (method == 6) {
        r.s[1] = flags & 2 ? '8' : '4';
        r.s[3] = flags & 4 ? '3' : '2';
    } else if (method==8 || method==9) {
        r.s[3] = (u8)"NXFS"[flags>>1 & 3];
    }
    return r;
}

// The two characters after an entry's size (zi_short): t for text by
// its internal attributes, else b, in upper case if encrypted; then x
// if it has central extra fields (xlen bytes, Zip64's counted), or, from
// Unix, OS/2 HPFS, or NTFS, external attribute bit 15, the sign of an
// old Unix field kept only in the local header, else -; and in upper
// case, X, or for none, l, with a data descriptor (flag bit 3).
static zistr zi_flags(u32 flags, u32 intattr, u32 extattr, u32 made, iz xlen)
{
    zistr r    = {0};
    u32   host = uz_host(made);
    b32   enc  = flags & 1;
    b32   desc = flags & 8;
    b32   xf   = xlen || ((extattr & 0x8000) &&
                          (host==UZ_UNIX || host==ZI_HPFS || host==ZI_NTFS));
    zi_putc(&r, intattr & 1 ? (enc ? 'T' : 't') : (enc ? 'B' : 'b'));
    zi_putc(&r, xf ? (desc ? 'X' : 'x') : (desc ? 'l' : '-'));
    return r;
}

// An entry's time as zi_time shows it outside -v, from the fields
// {year, month, day, hour, minute, second}, a DOS time's as stored, or
// a local time: "yy-Mon-dd hh:mm", a month out of range as three digits
// ("80-000-00 00:00"); or with -T, "yyyymmdd.hhmmss", the month a
// number however large.
static zistr zi_date(i32 const tm[6], b32 decimal)
{
    static char const months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    zistr r = {0};
    u32   f[6];
    for (i32 k = 0; k < 6; k++) {
        f[k] = (u32)tm[k];
    }
    if (decimal) {
        zi_putu(&r, f[0], 4, 1);
        zi_putu(&r, f[1], 2, 1);
        zi_putu(&r, f[2], 2, 1);
        zi_putc(&r, '.');
        zi_putu(&r, f[3], 2, 1);
        zi_putu(&r, f[4], 2, 1);
        zi_putu(&r, f[5], 2, 1);
        return r;
    }
    zi_putu(&r, f[0]%100, 2, 1);
    zi_putc(&r, '-');
    if (f[1]<1 || f[1]>12) {
        zi_putu(&r, f[1], 3, 1);
    } else {
        zi_puts(&r, months[f[1]-1]);
    }
    zi_putc(&r, '-');
    zi_putu(&r, f[2], 2, 1);
    zi_putc(&r, ' ');
    zi_putu(&r, f[3], 2, 1);
    zi_putc(&r, ':');
    zi_putu(&r, f[4], 2, 1);
    return r;
}

// A signed number, as "%wd".
static void zi_putd(zistr *z, i32 v, iz w)
{
    u32 m = v<0 ? -(u32)v : (u32)v;
    iz  n = 1;
    for (u32 t = m; t >= 10; t /= 10, n++) {}
    for (w -= n + (v<0); w > 0; w--) {
        zi_putc(z, ' ');
    }
    if (v < 0) {
        zi_putc(z, '-');
    }
    zi_putu(z, m, 0, 0);
}

// -m's percentage, from a compression factor in tenths of a percent
// (uz_ratio): "%3d%%" of factor+5 over ten, truncated as C divides
// ("-99%" for -99.5%).
static zistr zi_percent(i32 factor)
{
    zistr r = {0};
    zi_putd(&r, (factor + 5)/10, 3);
    zi_putc(&r, '%');
    return r;
}

// The totals line's factor: "%s%d.%d%%", the sign apart ("-50.0%").
static zistr zi_factor(i32 factor)
{
    zistr r = {0};
    u32   m = factor<0 ? -(u32)factor : (u32)factor;
    if (factor < 0) {
        zi_putc(&r, '-');
    }
    zi_putu(&r, m/10, 0, 0);
    zi_putc(&r, '.');
    zi_putu(&r, m%10, 0, 0);
    zi_putc(&r, '%');
    return r;
}

// -v, the verbose report (zi_end_central, zi_long)

// Text longer than a field: a section's lines, or a subfield's
// description, in room the caller gives, past which it is cut short
// (which nothing here reaches: each part below is bounded).
typedef struct {
    u8 *s;
    iz  len;
    iz  cap;
} zitext;

static zitext zt_new(arena *a, iz cap)
{
    zitext t = {0};
    t.s   = newstr(a, cap);
    t.cap = cap;
    return t;
}

static s8 zt_text(zitext *t)
{
    return (s8){t->s, t->len};
}

static void zt_putc(zitext *t, u8 c)
{
    if (t->len < t->cap) {
        t->s[t->len++] = c;
    }
}

static void zt_puts(zitext *t, char const *s)
{
    for (; *s; s++) {
        zt_putc(t, (u8)*s);
    }
}

static void zt_str(zitext *t, s8 s)
{
    for (iz i = 0; i < s.len; i++) {
        zt_putc(t, s.s[i]);
    }
}

// A number in a base, as "%wu", or zero-padded to w digits, as "%.wX",
// "%.wx", or "%0wo".
static void zt_num(zitext *t, u64 v, u32 base, iz w, b32 zero, b32 upper)
{
    char const *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    u8  tmp[24];
    iz  n = 0;
    do {
        tmp[n++] = (u8)digits[v%base];
    } while (v /= base);
    for (; w > n; w--) {
        zt_putc(t, zero ? '0' : ' ');
    }
    while (n) {
        zt_putc(t, tmp[--n]);
    }
}

static void zt_putu(zitext *t, u64 v)
{
    zt_num(t, v, 10, 0, 0, 0);
}

static void zt_hex(zitext *t, u64 v, iz w, b32 upper)
{
    zt_num(t, v, 16, w, 1, upper);
}

// "%lld"
static void zt_putd(zitext *t, i64 v)
{
    if (v < 0) {
        zt_putc(t, '-');
    }
    zt_putu(t, v<0 ? -(u64)v : (u64)v);
}

// A number in decimal and in hexadecimal, as FmZofft gives offsets and
// sizes: "%wllu (%.16llXh)".
static void zt_both(zitext *t, u64 v, iz w)
{
    zt_num(t, v, 10, w, 0, 0);
    zt_puts(t, " (");
    zt_hex(t, v, 16, 1);
    zt_puts(t, "h)");
}

// What -v's end-record section shows (zi_end_central): the file's size,
// where its end record lies, the Zip64 end record if used, and where the
// central directory says it should, and the end record's fields, as
// tugz takes them, from the 32-bit record or the Zip64 one.
typedef struct {
    i64 size;     // of the file, any data before the archive included
    i64 real;     // offset of the end record, or the Zip64 end record
    u64 cdoff;    // as recorded, not shifted
    u64 cdsize;
    u64 count;    // entries
    u64 ndisk;    // entries on this disk
    u32 disk;     // this disk's number
    u32 cddisk;   // the central directory's first
} ziend;

// The end-record section, after the comment's: the sizes and offsets in
// decimal and hexadecimal, then the central directory, in the words for
// a single-part archive, or for a disk number other than 0, a multi-part
// one. The expected offset wraps, as UnZip's sum does.
static void zi_end_text(zitext *t, ziend const *z)
{
    zt_puts(t, "\nEnd-of-central-directory record:\n"
               "-------------------------------\n\n"
               "  Zip archive file size:               ");
    zt_both(t, (u64)z->size, 11);
    zt_puts(t, "\n  Actual end-cent-dir record offset:   ");
    zt_both(t, (u64)z->real, 11);
    zt_puts(t, "\n  Expected end-cent-dir record offset: ");
    zt_both(t, z->cdoff + z->cdsize, 11);
    zt_puts(t, "\n  (based on the length of the central directory and its"
               " expected offset)\n\n");
    if (!z->disk) {
        zt_puts(t, "  This zipfile constitutes the sole disk of a single-part"
                   " archive; its\n  central directory contains ");
        zt_putu(t, z->count);
        zt_puts(t, z->count==1 ? " entry.\n" : " entries.\n");
        zt_puts(t, "  The central directory is ");
        zt_both(t, z->cdsize, 0);
        zt_puts(t, " bytes long,\n  and its (expected) offset in bytes from"
                   " the beginning of the zipfile\n  is ");
        zt_both(t, z->cdoff, 0);
        zt_puts(t, ".\n\n");
    } else {
        zt_puts(t, "  This zipfile constitutes disk ");
        zt_putu(t, (u64)z->disk + 1);
        zt_puts(t, " of a multi-part archive.  The central\n  directory"
                   " starts on disk ");
        zt_putu(t, (u64)z->cddisk + 1);
        zt_puts(t, " at an offset within that archive part\n  of ");
        zt_both(t, z->cdoff, 0);
        zt_puts(t, " bytes.  The entire\n  central directory is ");
        zt_both(t, z->cdsize, 0);
        zt_puts(t, " bytes long.\n  ");
        zt_putu(t, z->ndisk);
        zt_puts(t, z->ndisk==1 ? " of the archive entries is"
                               : " of the archive entries are");
        zt_puts(t, " contained within this zipfile volume,\n  out of a total"
                   " of ");
        zt_putu(t, z->count);
        zt_puts(t, z->count==1 ? " entry.\n\n" : " entries.\n\n");
    }
}

// The line before an entry's, by which zi_long tells of data between
// entries: "There are an extra N bytes preceding this file.", signed,
// where its offset is not where the entry before it ended, *endprev,
// unless that is unknown (0). Then *endprev is where this one ends, as
// its central header tells: past a local header taken to hold the same
// name and extra fields, and its data, in unsigned arithmetic that
// wraps, as UnZip's does. Its subfields adjust that (zi_subfield,
// zi_oldux).
static void zi_preceding(zitext *t, zentry const *e, u64 *endprev)
{
    u64 off = (u64)e->offset;
    if (off!=*endprev && *endprev) {
        zt_puts(t, "  There are an extra ");
        zt_putd(t, (i64)(off - *endprev));
        zt_puts(t, " bytes preceding this file.\n\n");
    }
    *endprev = off + ZIP_LOCAL_LEN + (u64)e->name.len + (u64)e->cextra.len +
               (u64)e->csize;
}

// A host's long name, by the upper byte of a version, made by or needed
// to extract (zi_long's os[]). Hosts 20 to 29 have none: "made by" shows
// as UnZip's builds print a null string, "(null)"; "needed" as unknown,
// like the hosts past 30.
static void zi_hostlong(zitext *t, u32 host, b32 needed)
{
    static char const *const names[UZ_HOSTS] = {
        "MS-DOS, OS/2 or NT FAT", "Amiga", "VMS", "Unix", "VM/CMS",
        "Atari ST", "OS/2 or NT HPFS", "Macintosh HFS", "Z-System", "CP/M",
        "TOPS-20", "NTFS", "SMS/QDOS", "Acorn RISC OS", "Win32 VFAT", "MVS",
        "BeOS", "Tandem NSK", "Theos", "Mac OS/X (Darwin)",
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        "AtheOS",
    };
    if (host<UZ_HOSTS && names[host]) {
        zt_puts(t, names[host]);
    } else if (host<UZ_HOSTS && !needed) {
        zt_puts(t, "(null)");
    } else {
        zt_puts(t, "unknown (");
        zt_putu(t, host);
        zt_putc(t, ')');
    }
}

// A method's long name (zi_long's method[]), or "unknown (N)".
static void zi_methodlong(zitext *t, u32 method)
{
    static struct { u16 id; char name[24]; } const methods[] = {
        {0, "none (stored)"}, {1, "shrunk"}, {2, "reduced (factor 1)"},
        {3, "reduced (factor 2)"}, {4, "reduced (factor 3)"},
        {5, "reduced (factor 4)"}, {6, "imploded"}, {7, "tokenized"},
        {8, "deflated"}, {9, "deflated (enhanced-64k)"},
        {10, "imploded (PK DCL)"}, {12, "bzipped"}, {14, "LZMA-ed"},
        {18, "tersed (IBM)"}, {19, "LZ77-compressed (IBM)"},
        {97, "WavPacked"}, {98, "PPMd-ed"},
    };
    for (i32 k = 0; k < countof(methods); k++) {
        if (methods[k].id == method) {
            zt_puts(t, methods[k].name);
            return;
        }
    }
    zt_puts(t, "unknown (");
    zt_putu(t, method);
    zt_putc(t, ')');
}

// A time as zi_time shows it under -v, from the fields {year, month,
// day, hour, minute, second}: "%u %s %u %02u:%02u:%02u", a month out of
// range as three digits.
static void zi_vdate(zitext *t, i32 const tm[6])
{
    static char const months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    zt_putu(t, (u32)tm[0]);
    zt_putc(t, ' ');
    if (tm[1]<1 || tm[1]>12) {
        zt_num(t, (u32)tm[1], 10, 3, 1, 0);
    } else {
        zt_puts(t, months[tm[1]-1]);
    }
    zt_putc(t, ' ');
    zt_putu(t, (u32)tm[2]);
    for (i32 k = 3; k < 6; k++) {
        zt_putc(t, k==3 ? ' ' : ':');
        zt_num(t, (u32)tm[k], 10, 2, 1, 0);
    }
}

// A line's label, from its start, and room to column 50, where zi_long's
// values begin.
static void zt_label(zitext *t, iz start, char const *label)
{
    zt_puts(t, label);
    while (t->len-start<50 && t->len<t->cap) {
        zt_putc(t, ' ');
    }
}

// The external attributes' lines as zi_long shows them, by host:
// - VMS: "(RWED,RWED,RE,R)" from the Unix bits, for system (as owner),
//   owner, group, and other, with the mode in octal;
// - Amiga: its type, then hsparwed;
// - THEOS: its type's name, then a letter for each bit not set;
// - MS-DOS and the hosts with its attributes (OS/2 HPFS, NTFS, VFAT,
//   Acorn, VM/CMS, MVS): the upper 24 bits in hexadecimal;
// - any other host, Unix's: its type (d - l b c p s, else ?) and rwx,
//   set-ID as s or S, but l for the group's without x, and sticky as t
//   or T;
// then the MS-DOS attributes, the low byte: none, read-only, or words.
static void zi_vattribs(zitext *t, u32 made, u32 extattr)
{
    u32 host  = uz_host(made);
    u32 x     = extattr>>16 & 0xffff;
    u8  a[24];
    iz  n     = 0;
    iz  start = t->len;
    switch (host) {
    case UZ_VMS: {
        u8  w[12] = {0};
        u32 bits[] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
        for (i32 k = 0; k < 3; k++) {
            w[4*k+0] = x & bits[3*k+0] ? 'R' : 0;
            w[4*k+1] = x & bits[3*k+1] ? 'W' : 0;
            w[4*k+2] = x & bits[3*k+2] ? 'E' : 0;
            w[4*k+3] = x & bits[3*k+1] ? 'D' : 0;
        }
        a[n++] = '(';
        for (i32 g = 0; g < 4; g++) {  // system's, the owner's, first
            i32 k = g ? g-1 : 0;
            for (i32 i = 0; i < 4; i++) {
                if (w[4*k+i]) {
                    a[n++] = w[4*k+i];
                }
            }
            a[n++] = g<3 ? ',' : ')';
        }
        zt_puts(t, "  VMS file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
    } break;

    case UZ_AMIGA:
        a[n++] = (x & 06000)==04000 ? 'd' : (x & 06000)==02000 ? '-' : '?';
        for (i32 k = 0; k < 8; k++) {
            a[n++] = x & 0200u>>k ? (u8)"hsparwed"[k] : '-';
        }
        zt_puts(t, "  Amiga file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
        break;

    case UZ_THEOS: {
        static char const types[16][13] = {
            "???         ", "???         ", "???         ", "???         ",
            "Directory   ", "Library     ", "???         ", "???         ",
            "Sequential  ", "Direct      ", "Keyed       ", "Indexed     ",
            "???         ", " 86 program ", "286 program ", "386 program ",
        };
        u32 bits[] = {0x400, 0x800, 0x002, 0x004, 0x200, 0x040, 0x080, 0x100};
        bytecopy(a, types[x>>12], 12);
        n = 12;
        for (i32 k = 0; k < 8; k++) {
            a[n++] = x & bits[k] ? '.' : (u8)"HMWREXWR"[k];
        }
        zt_puts(t, "  Theos file attributes (");
        zt_hex(t, x, 4, 1);
        zt_label(t, start, " hex):");
    } break;

    case UZ_FAT: case ZI_HPFS: case ZI_NTFS: case ZI_VFAT: case UZ_ACORN:
    case ZI_CMS: case ZI_MVS:
        zt_label(t, start, "  non-MSDOS external file attributes:");
        zt_hex(t, extattr>>8, 6, 1);
        zt_puts(t, " hex\n");
        break;

    default:
        switch (x & 0170000) {
        case 0040000: a[0] = 'd'; break;
        case 0100000: a[0] = '-'; break;
        case 0120000: a[0] = 'l'; break;
        case 0060000: a[0] = 'b'; break;
        case 0020000: a[0] = 'c'; break;
        case 0010000: a[0] = 'p'; break;
        case 0140000: a[0] = 's'; break;
        default:      a[0] = '?';
        }
        for (i32 k = 0; k < 3; k++) {
            u32 m = x >> (6 - 3*k);
            a[1+3*k] = m & 4 ? 'r' : '-';
            a[2+3*k] = m & 2 ? 'w' : '-';
        }
        a[3] = x & 0100 ? (x & 04000 ? 's' : 'x') : (x & 04000 ? 'S' : '-');
        a[6] = x & 0010 ? (x & 02000 ? 's' : 'x') : (x & 02000 ? 'l' : '-');
        a[9] = x & 0001 ? (x & 01000 ? 't' : 'x') : (x & 01000 ? 'T' : '-');
        n = 10;
        zt_puts(t, "  Unix file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
    }
    if (n) {
        zt_str(t, (s8){a, n});
        zt_putc(t, '\n');
    }

    u32 d = extattr & 0xff;
    start = t->len;
    zt_puts(t, "  MS-DOS file attributes (");
    zt_hex(t, d, 2, 1);
    zt_label(t, start, " hex):");
    if (d == 0) {
        zt_puts(t, "none");
    } else if (d == 1) {
        zt_puts(t, "read-only");
    } else {
        static char const words[8][5] = {
            "rdo ", "hid ", "sys ", "lab ", "dir ", "arc ", "lnk ", "exe",
        };
        for (i32 k = 0; k < 8; k++) {
            if (d & 1u<<k) {
                zt_puts(t, words[k]);
            }
        }
    }
    zt_putc(t, '\n');
}

// A labeled line of zi_long's.
static void zt_line(zitext *t, char const *label)
{
    zt_label(t, t->len, label);
}

// "%u.%u" of a version's lower byte.
static void zt_version(zitext *t, u32 v)
{
    zt_putu(t, (v & 0xff)/10);
    zt_putc(t, '.');
    zt_putu(t, (v & 0xff)%10);
    zt_putc(t, '\n');
}

// An entry's lines as zi_long shows them, after its name, through its
// attributes: its offset, as recorded, hosts and versions, method (and
// its sub-type), encryption, data descriptor, times, CRC, sizes, lengths,
// disk (always the first, the only one tugz reads), file type, and
// attributes. Its times are its DOS time's fields, and if given (local
// not null), its central extended timestamp's, in local time and UTC.
static void zi_long_text(zitext *t, zentry const *e, i32 const *local,
                         i32 const *utc)
{
    u32 flags = e->flags;
    zt_puts(t, "\n");
    zt_line(t, "  offset of local header from start of archive:");
    zt_putu(t, (u64)e->offset);
    zt_puts(t, "\n");
    zt_line(t, "");
    zt_putc(t, '(');
    zt_hex(t, (u64)e->offset, 16, 1);
    zt_puts(t, "h) bytes\n");
    zt_line(t, "  file system or operating system of origin:");
    zi_hostlong(t, e->made>>8, 0);
    zt_putc(t, '\n');
    zt_line(t, "  version of encoding software:");
    zt_version(t, e->made);
    zt_line(t, "  minimum file system compatibility required:");
    zi_hostlong(t, e->needed>>8, 1);
    zt_putc(t, '\n');
    zt_line(t, "  minimum software version required to extract:");
    zt_version(t, e->needed);
    zt_line(t, "  compression method:");
    zi_methodlong(t, e->method);
    zt_putc(t, '\n');
    if (e->method == 6) {
        zt_line(t, "  size of sliding dictionary (implosion):");
        zt_puts(t, flags & 2 ? "8K\n" : "4K\n");
        zt_line(t, "  number of Shannon-Fano trees (implosion):");
        zt_puts(t, flags & 4 ? "3\n" : "2\n");
    } else if (e->method==8 || e->method==9) {
        static char const sub[4][11] = {
            "normal\n", "maximum\n", "fast\n", "superfast\n",
        };
        zt_line(t, "  compression sub-type (deflation):");
        zt_puts(t, sub[flags>>1 & 3]);
    }
    zt_line(t, "  file security status:");
    zt_puts(t, flags & 1 ? "encrypted\n" : "not encrypted\n");
    zt_line(t, "  extended local header:");
    zt_puts(t, flags & 8 ? "yes\n" : "no\n");

    i32 tm[6];
    uz_dosdate(e->dostime, tm);
    zt_line(t, "  file last modified on (DOS date/time):");
    zi_vdate(t, tm);
    zt_putc(t, '\n');
    for (i32 k = 0; local && k < 2; k++) {
        zt_line(t, "  file last modified on (UT extra field modtime):");
        zi_vdate(t, k ? utc : local);
        zt_puts(t, k ? " UTC\n" : " local\n");
    }

    zt_line(t, "  32-bit CRC value (hex):");
    zt_hex(t, e->crc, 8, 0);
    zt_putc(t, '\n');
    zt_line(t, "  compressed size:");
    zt_putu(t, (u64)e->csize);
    zt_puts(t, " bytes\n");
    zt_line(t, "  uncompressed size:");
    zt_putu(t, (u64)e->usize);
    zt_puts(t, " bytes\n");
    zt_line(t, "  length of filename:");
    zt_putu(t, (u64)e->name.len);
    zt_puts(t, " characters\n");
    zt_line(t, "  length of extra field:");
    zt_putu(t, (u64)e->cextra.len);
    zt_puts(t, " bytes\n");
    zt_line(t, "  length of file comment:");
    zt_putu(t, (u64)e->comment.len);
    zt_puts(t, " characters\n");
    zt_line(t, "  disk number on which file begins:");
    zt_puts(t, "disk 1\n");
    zt_line(t, "  apparent file type:");
    zt_puts(t, e->intattr & 1 ? "text\n" : e->intattr & 2 ? "ebcdic\n" :
                                           "binary\n");
    zi_vattribs(t, e->made, e->extattr);
}

// The next subfield of an extra field, *x, as zi_long's loop takes it:
// false if fewer than the four bytes of a header are left; else its ID,
// its data, and the length its header states, more than the data's
// where that runs past the field's end, and *x moved past it.
static b32 zi_next(s8 *x, u32 *id, s8 *data, iz *stated)
{
    if (x->len < 4) {
        return 0;
    }
    *id     = get16(x->s);
    *stated = get16(x->s+2);
    iz n    = MIN(*stated, x->len-4);
    *data   = (s8){x->s+4, n};
    x->s   += 4 + n;
    x->len -= 4 + n;
    return 1;
}

// zi_long's error for a subfield whose stated length runs past the end
// of the extra field, with left bytes there. It goes to standard error
// (but with totals shown), after a newline if a line is unfinished.
static void zi_cut_text(zitext *t, u32 id, iz stated, iz left)
{
    zt_puts(t, "\n  error: EF data block (type 0x");
    zt_hex(t, id, 4, 0);
    zt_puts(t, ") size ");
    zt_putu(t, (u64)stated);
    zt_puts(t, " exceeds remaining extra field\n         space ");
    zt_putu(t, (u64)left);
    zt_puts(t, "; block length has been truncated.\n");
}

// Mac type and creator codes, eight bytes, as zi_showMacTypeCreator
// shows them: as characters if all are printable (in the C locale), else
// as two numbers.
static void zi_mactype(zitext *t, u8 const *p)
{
    b32 print = 1;
    for (i32 i = 0; i < 8; i++) {
        print &= p[i]>=0x20 && p[i]<0x7f;
    }
    zt_puts(t, ".\n    The associated file has type code `");
    if (print) {
        zt_str(t, (s8){(u8 *)p, 4});
        zt_puts(t, "' and creator code `");
        zt_str(t, (s8){(u8 *)p+4, 4});
    } else {
        u32 type    = (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
        u32 creator = (u32)p[4]<<24 | (u32)p[5]<<16 | (u32)p[6]<<8 | p[7];
        zt_puts(t, "0x");
        zt_num(t, type, 16, 0, 0, 0);
        zt_puts(t, "' and creator code `0x");
        zt_num(t, creator, 16, 0, 0, 0);
    }
    zt_putc(t, '\'');
}

// Text from a subfield, n bytes up to any NUL, shown as names are, where
// UnZip writes it as it is.
static void zi_raw(zitext *t, u8 const *p, iz n, arena scratch)
{
    iz len = 0;
    for (; len<n && p[len]; len++) {}
    zt_str(t, uz_filter((s8){(u8 *)p, len}, &scratch));
}

// Subfield IDs (zi_long)
enum {
    ZI_ZIP64    = 0x0001,
    ZI_OS2      = 0x0009,
    ZI_ACL      = 0x4c41,
    ZI_NTSD     = 0x4453,
    ZI_IZVMS    = 0x4d49,
    ZI_UX       = 0x5855,
    ZI_UX2      = 0x7855,
    ZI_UT       = 0x5455,
    ZI_UPATH    = 0x7075,
    ZI_UCOMMENT = 0x6375,
    ZI_MAC3     = 0x334d,
    ZI_JLMAC    = 0x07c8,
    ZI_ZIPIT    = 0x2605,
    ZI_ZIPIT2   = 0x2705,
    ZI_SMARTZIP = 0x4d63,
    ZI_ATHEOS   = 0x7441,
    ZI_BEOS     = 0x6542,
    ZI_QDOS     = 0xfb4a,
    ZI_AOSVS    = 0x5356,
    ZI_TANDEM   = 0x4154,
    ZI_MD5      = 0x4b46,
};

// A subfield's name, zi_long's ef_fieldname.
static char const *zi_subname(u32 id)
{
    static struct { u16 id; char name[25]; } const names[] = {
        {0x0001, "PKWARE 64-bit sizes"}, {0x0007, "PKWARE AV"},
        {0x0009, "OS/2"}, {0x4c41, "OS/2 ACL"},
        {0x4453, "Security Descriptor"}, {0x000c, "PKWARE VMS"},
        {0x4d49, "Info-ZIP VMS"}, {0x000a, "PKWARE Win32"},
        {0x000d, "PKWARE Unix"}, {0x5855, "old Info-ZIP Unix/OS2/NT"},
        {0x7855, "Unix UID/GID (16-bit)"},
        {0x7875, "Unix UID/GID (any size)"}, {0x5455, "universal time"},
        {0x7075, "UTF8 path name"}, {0x6375, "UTF8 entry comment"},
        {0x334d, "new Info-ZIP Macintosh"},
        {0x07c8, "old Info-ZIP Macintosh"}, {0x2605, "ZipIt Macintosh"},
        {0x2705, "ZipIt Macintosh (short)"}, {0x4d63, "SmartZip Macintosh"},
        {0x4704, "VM/CMS"}, {0x470f, "MVS"}, {0x7441, "AtheOS"},
        {0x6542, "BeOS"}, {0xfb4a, "SMS/QDOS"}, {0x5356, "AOS/VS"},
        {0x4341, "Acorn SparkFS"}, {0x4b46, "Fred Kantor MD5"},
        {0x756e, "ASi Unix"}, {0x4154, "Tandem NSK"}, {0x6854, "Theos"},
    };
    for (i32 k = 0; k < countof(names); k++) {
        if (names[k].id == id) {
            return names[k].name;
        }
    }
    return "unknown";
}

// Whether a subfield's data begins with a signature of four characters.
static b32 zi_magic(u8 const *p, iz n, char const *sig)
{
    return n>=4 && get32(p)==get32((u8 const *)sig);
}

// One subfield of an entry's central extra field, its data d as zi_next
// gives it, described as zi_long describes it: its ID, name, and length,
// then what its decoder makes of its data, if long enough for it, else
// the data in hexadecimal, up to 24 bytes, or the first 20, then a
// period. Subfields that tell how the local header differs adjust where
// the entry ends, *endprev (zi_preceding): Zip64's, for the offset not
// there, if past 4 GiB; old Unix fields, for owners there; the access
// and creation times of a "UT"; Mac and BeOS attributes, unknown where
// compressed (0), as OS/2's and NT's security data are.
//
// Where UnZip writes text from the data as it is (a ZipIt or SmartZip
// name, a QDOS subtype, a VMS version), it is shown as names are; and a
// SmartZip name's length past its 31 bytes, past which UnZip writes,
// counts as 31.
static void zi_subfield(zitext *t, u32 id, s8 d, zentry const *e,
                        u64 *endprev, arena scratch)
{
    u32       host = uz_host(e->made);
    u8 const *p    = d.s;
    iz        n    = d.len;
    switch (id) {
    case ZI_ZIP64:
        if ((u64)e->offset >> 32) {
            *endprev -= n==8 ? 12 : 8;
        }
        break;
    case ZI_UX:
        if (host==UZ_UNIX && *endprev) {
            *endprev += 4;
        }
        break;
    case ZI_UX2:
        if (*endprev) {
            *endprev += 4;
        }
        break;
    }
    zt_puts(t, "\n  - A subfield with ID 0x");
    zt_hex(t, id, 4, 0);
    zt_puts(t, " (");
    zt_puts(t, zi_subname(id));
    zt_puts(t, ") and ");
    zt_putu(t, (u64)n);
    zt_puts(t, " data bytes");

    b32 done = 1;
    switch (id) {
    case ZI_OS2:
    case ZI_ACL:
    case ZI_NTSD:
        if ((done = n >= 4)) {
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, get32(p));
            zt_puts(t, id==ZI_OS2 ? " bytes of OS/2 extended attributes.\n"
                                    "    (May not match OS/2 \"dir\" amount"
                                    " due to storage method)" :
                       id==ZI_ACL ? " bytes of access control list"
                                    " information" :
                                    " bytes of NT security descriptor data");
            *endprev = 0;
        }
        break;

    case ZI_IZVMS:
        if ((done = n >= 8)) {
            static char const comp[4][19] = {
                "stored", "run-length encoded", "deflated", "compressed(?)",
            };
            static struct { char sig[5]; char name[8]; } const blocks[] = {
                {"VFAB", "FAB"}, {"VALL", "XABALL"}, {"VFHC", "XABFHC"},
                {"VDAT", "XABDAT"}, {"VRDT", "XABRDT"}, {"VPRO", "XABPRO"},
                {"VKEY", "XABKEY"}, {"VMSV", "version"},
            };
            char const *name = "unknown";
            for (i32 k = 0; k < countof(blocks); k++) {
                name = zi_magic(p, n, blocks[k].sig) ? blocks[k].name : name;
            }
            zt_puts(t, ".  The extra\n    field is ");
            zt_puts(t, comp[MIN(get16(p+4) & 7, 3)]);
            zt_puts(t, " and has ");
            zt_putu(t, get16(p+6));
            zt_puts(t, " bytes of VMS ");
            zt_puts(t, name);
            zt_puts(t, " information");
            if (zi_magic(p, n, "VMSV") && n>=16) {
                // " (V5.5)", cut short at a NUL, its parenthesis with it
                iz len = 0;
                for (; len<4 && p[12+len]; len++) {}
                zt_puts(t, " (");
                zi_raw(t, p+12, len, scratch);
                if (len == 4) {
                    zt_putc(t, ')');
                }
            }
        }
        break;

    case ZI_UT:
        if (n > 0) {
            static char const names[3][13] = {
                "modification", "access", "creation",
            };
            i32 num = 0;
            for (i32 k = 0; k < 3; k++) {
                if (!(p[0] & 1<<k)) {
                    continue;
                }
                zt_puts(t, num++ ? "/" : ".\n    The local extra field has"
                                         " UTC/GMT ");
                zt_puts(t, names[k]);
                if (k && *endprev) {
                    *endprev += 4;  // in the local "UT", not the central
                }
            }
            if (num) {
                zt_puts(t, num==1 ? " time" : " times");
            }
        }
        break;

    case ZI_UPATH:
    case ZI_UCOMMENT:
        if ((done = n >= 5)) {
            b32 all = n <= 29;
            zt_puts(t, all ? ".\n    The UTF8 data of the extra field (V"
                           : ". The first\n    24 UTF8 bytes in the extra"
                             " field (V");
            zt_putu(t, p[0]);
            zt_puts(t, ", ASCII name CRC `");
            zt_hex(t, get32(p+1), 8, 0);
            zt_puts(t, "') are:\n   ");
            for (iz i = 5; i < (all ? n : 29); i++) {
                zt_putc(t, ' ');
                zt_hex(t, p[i], 2, 0);
            }
        }
        break;

    case ZI_MAC3:
        if ((done = n >= 14)) {
            u32 ucsize = get32(p);
            u32 mflags = get16(p+4);
            b32 uncomp = mflags & 4;
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, ucsize);
            zt_puts(t, uncomp ? " bytes of uncompressed" : " bytes of"
                                                            " compressed");
            zt_puts(t, " Macintosh\n    finder attributes");
            if (!uncomp) {
                *endprev = 0;
            } else if (*endprev) {
                *endprev += ucsize;
            }
            zt_puts(t, ".\n    File is marked as ");
            zt_puts(t, mflags & 1 ? "Data-fork" : "Resource-fork");
            zt_puts(t, mflags & 8 ? ", File Dates are in 64 Bit"
                                  : ", File Dates are in 32 Bit");
            zi_mactype(t, p+6);
        }
        break;

    case ZI_ZIPIT2:
        if ((done = n>=5 && zi_magic(p, n, "ZPIT")) && n>=12) {
            zi_mactype(t, p+4);
        }
        break;

    case ZI_ZIPIT:
        if ((done = n>=5 && zi_magic(p, n, "ZPIT")) && n>=p[4]+13) {
            zt_puts(t, ".\n    The Mac long filename is ");
            zi_raw(t, p+5, p[4], scratch);
            zi_mactype(t, p+5+p[4]);
        }
        break;

    case ZI_JLMAC:
        if ((done = n>=40 && zi_magic(p, n, "JLEE"))) {
            zi_mactype(t, p+4);
            zt_puts(t, p[31] & 1 ? ".\n    File is marked as Data-fork"
                                 : ".\n    File is marked as Resource-fork");
        }
        break;

    case ZI_SMARTZIP:
        if ((done = n==64 && zi_magic(p, n, "dZip"))) {
            zi_mactype(t, p+4);
            zt_puts(t, ".\n    The Mac long filename is ");
            zi_raw(t, p+33, MIN(p[32], 31), scratch);
        }
        break;

    case ZI_ATHEOS:
    case ZI_BEOS:
        if ((done = n >= 5)) {
            u32 ucsize = get32(p);
            b32 uncomp = p[4] & 1;
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, ucsize);
            zt_puts(t, uncomp ? " bytes of uncompressed " : " bytes of"
                                                             " compressed ");
            zt_puts(t, id==ZI_ATHEOS ? "AtheOS file attributes"
                                     : "BeOS file attributes");
            if (!uncomp) {
                *endprev = 0;
            } else if (*endprev) {
                *endprev += ucsize;
            }
        }
        break;

    case ZI_QDOS:
        if ((done = n >= 4)) {
            zt_puts(t, ".\n    The QDOS extra field subtype is `");
            zt_str(t, uz_filter((s8){(u8 *)p, 4}, &scratch));
            zt_putc(t, '\'');
        }
        break;

    case ZI_AOSVS:
        if ((done = n >= 5)) {
            zt_puts(t, ".\n    The AOS/VS extra field revision is ");
            zt_putu(t, p[4]/10);
            zt_putc(t, '.');
            zt_putu(t, p[4]%10);
        }
        break;

    case ZI_TANDEM:
        if ((done = n == 20)) {
            static char const formats[6][16] = {
                "Unstructured", "Relative", "Entry Sequenced",
                "Key Sequenced", "Edit", "Object",
            };
            u32 type = (p[18] & 0x60u) >> 5;
            u32 code = (u32)p[0]<<8 | p[1];  // big-endian
            if (type == 0) {
                type = code==101 ? 4 : code==100 ? 5 : 0;
            }
            zt_puts(t, ".\n    The file was originally a Tandem ");
            zt_puts(t, formats[type]);
            zt_puts(t, " file, with file code ");
            zt_putu(t, code);
        }
        break;

    case ZI_MD5:
        if ((done = n >= 19)) {
            zt_puts(t, ".\n    The 128-bit MD5 signature is ");
            for (i32 i = 15; i >= 0; i--) {
                zt_hex(t, p[i], 2, 0);
            }
        }
        break;

    default:
        done = 0;
    }

    if (!done && n) {
        zt_puts(t, n<=24 ? ":\n   " : ".  The first\n    20 are:  ");
        for (iz i = 0; i < (n<=24 ? n : 20); i++) {
            zt_putc(t, ' ');
            zt_hex(t, p[i], 2, 0);
        }
    }
    zt_putc(t, '.');
}

// The lines after the extra field for an entry whose external attributes
// tell of an old Unix field ("UX") in its local header alone, bit 15
// (and bit 14 for its owner too): from Unix, OS/2 HPFS, or NTFS, it is
// there, its length adjusting where the entry ends (zi_preceding); from
// MS-DOS, it may be, if bit 14 is unset.
static void zi_oldux(zitext *t, zentry const *e, u64 *endprev)
{
    u32 host = uz_host(e->made);
    u32 x    = (e->extattr & 0xc000) >> 12;
    if (!(x & 8)) {
        return;
    } else if (host==UZ_UNIX || host==ZI_HPFS || host==ZI_NTFS) {
        zt_puts(t, "\n  There is a local extra field with ID 0x5855 (old"
                   " Info-ZIP Unix/OS2/NT) and\n  ");
        zt_putu(t, x & 12);
        zt_puts(t, x & 4 ? " data bytes (GMT modification/access times and"
                           " Unix UID/GID).\n"
                         : " data bytes (GMT modification/access times"
                           " only).\n");
        if (*endprev) {
            *endprev += x & 12;
        }
    } else if (host==UZ_FAT && !(x & 4)) {
        zt_puts(t, "\n  There may be a local extra field with ID 0x5855 (old"
                   " Info-ZIP Unix/OS2/NT) and\n  8 data bytes (GMT"
                   " modification/access times only).\n");
    }
}

// tugz unzip program: Info-ZIP UnZip compatible command line and driver
//
// Finds each archive named, reads its central directory (src/zipin.c),
// and lists, tests, or extracts the entries selected, decoding through
// src/inflate.c and applying src/unzip.c's rules. Messages, the streams
// they go to, and exit statuses follow UnZip 6.0's source, cited by file
// at each (unzip.c, process.c, list.c, extract.c, fileio.c), with the
// additions of Debian's build (ISO dates in listings, and its check for
// overlapped components), but for the departures listed in docs/notes.md.
//
// The run is staged so that each archive's memory is claimed before its
// output: options, then per archive the central directory, then the
// mode, which decodes each entry through one inflator claimed at the
// start, or a Deflate64 one claimed with an archive that has entries by
// it. Extraction to disk shares the test's selection, checks, and
// decoding, its paths and the room for what it finishes at the end (links,
// then directories' attributes) planned beforehand. The central directory
// is checked whole, then read again at each stage rather than held, so
// that the memory an entry takes is where it begins, for the overlap
// check, and for a link or directory, its path.
//
// Nothing is written through a link: no directory on an entry's path,
// below the -d directory, may be one, and a file there is replaced by
// removing it and creating the new one exclusively. Between the check
// and the creation, another process could still put a link in the way.

// Exit statuses, UnZip's PK_* (unzip.h)
enum {
    PK_OK     = 0,
    PK_WARN   = 1,
    PK_ERR    = 2,   // an error in an entry; others were processed
    PK_BADERR = 3,   // a severe error in the archive
    PK_MEM    = 4,
    PK_NOZIP  = 9,   // no archive found
    PK_PARAM  = 10,  // bad or unsupported options
    PK_FIND   = 11,  // no matching entries
    PK_BOMB   = 12,  // overlapped components (Debian's)
    PK_DISK   = 50,  // a write failed
    PK_EOF    = 51,  // the archive ended early
    IZ_DIR    = 76,  // the "zipfile" was a directory (internal)
    IZ_UNSUP  = 81,  // entries skipped: unsupported method or encryption
};

// Platform interface for the unzip program, beyond src/io.c, src/dir.c,
// and src/zipin.c

// Whether a standard descriptor is a terminal (console).
static b32  os_isatty(os *, i32 fd);
// Hold the writer of standard output, whose text an interruption that
// ends the program writes first, where it can (POSIX's signal handler).
static void os_holdtext(os *, writer *);
// Why the last failed system call failed, in the words of the C
// library's strerror, or an empty string if unknown.
static s8   os_error(os *);

// Extraction. Paths are those the driver has checked: no directory on
// the way to one, beyond the -d directory, is a link (src/zipin.c's
// linkless), and none of these follows a link at its end.

// Create a directory, with the defaults for a new one (umask, inherited
// access control). Returns false on failure, os_error saying why.
static b32  os_mkdir(os *, s8 path, arena scratch);
// The process's umask (POSIX), or zero.
static u32  os_umask(os *);
// Unix seconds from a broken-down local time {year, month 1-12, day,
// hour, minute, second}, fields out of range carried over, as for an
// MS-DOS time: the inverse of os_localtime, daylight saving time taken
// as the zone had it then.
static i64  os_mktime(os *, i32 const tm[6]);

// Attributes to give an extracted file, directory, or link: those of
// flags, each failure reported by its own flag, with the reason in
// why[], indexed by the flag's bit (OS_AWHY reasons in all).
enum {
    OS_AOWNER = 1 << 0,  // uid and gid (POSIX)
    OS_AMODE  = 1 << 1,  // mode (POSIX) or attributes (Windows)
    OS_ATIMES = 1 << 2,  // modification and access times
    OS_ALINK  = 1 << 3,  // the link itself (os_symlink)
    OS_ASGID  = 1 << 4,  // keep a directory's set-group-ID bit (POSIX)
    OS_ACTIME = 1 << 5,  // with OS_ATIMES, creation time too (Windows)
    OS_AKEEPA = 1 << 6,  // with OS_ATIMES, access time unchanged (Windows)
    OS_AWHY   = 4,
};
typedef struct {
    i64 mtime;
    i64 atime;
    i64 ctime;    // creation time, with OS_ACTIME (failing as OS_ATIMES)
    u32 mode;     // permission bits, 07777, umask already applied
    u32 dosattr;  // read-only, hidden, system, archive (Windows)
    u32 uid;
    u32 gid;
    i32 flags;
} osattrs;
// Give a created file, still open and not yet kept, its attributes: on
// POSIX, its owner, then mode, then times. Returns the flags that
// failed, their reasons copied into the arena.
static i32  os_setattrs(os *, i32 fd, osattrs *, s8 *why, arena *);
// As os_setattrs, for a directory, through a handle that refuses a link
// at its end: its owner, then times, then mode, as UnZip orders them.
static i32  os_setdirattrs(os *, s8 path, osattrs *, s8 *why, arena *);
// Create a symbolic link to target, which must not exist, and, given
// OS_AOWNER, give the link its owner. On Windows, where links need
// privileges, a regular file holding the target, given its attributes
// and times as os_setattrs gives them. Returns the flags that failed,
// OS_ALINK for the link itself.
static i32  os_symlink(os *, s8 target, s8 path, osattrs *, s8 *why,
                       arena *);

static void os_oom(os *ctx)
{
    s8 msg = S("error:  not enough memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, PK_MEM);
}

// The two arenas share one region, perm growing up from its bottom and
// scratch down from its top. The platform's os_extend gives each more.
typedef struct {
    arena perm;
    arena scratch;   // allocates downward
    s8    argv0;     // the program's name, as run: zipinfo* is ZipInfo
    s8   *args;      // excluding the program name
    i32   nargs;
    s8    unzipenv;  // UNZIP, if set: options before the arguments
    s8    unzipopt;  // UNZIPOPT, if set: when UNZIP has none
    s8    zipinfoenv;  // ZIPINFO and ZIPINFOOPT likewise, in ZipInfo mode
    s8    zipinfoopt;
    b32   windows;   // Windows conventions: names, wildcards
} unzipconfig;

static s8 const unzip_usage = S8(
    "tugz unzip " TUGZ_VERSION ", a subset of Info-ZIP UnZip 6.0\n"
    "usage: unzip [-Z] [-opts[modifiers]] file[.zip] [list] [-x xlist] "
    "[-d exdir]\n"
    "  Default action is to extract files in list, except those in xlist,"
    " to exdir;\n"
    "  file[.zip] may be a wildcard, or - for standard input.\n"
    "  -Z => ZipInfo mode (\"unzip -Z\" for usage).\n"
    "\n"
    "  -p  extract files to pipe, no messages     -l  list files"
    " (short format)\n"
    "  -f  freshen existing files, create none    -t  test compressed"
    " archive data\n"
    "  -u  update files, create if necessary      -z  display archive"
    " comment only\n"
    "  -v  list verbosely/show version info       -c  extract files to"
    " stdout, named\n"
    "  -x  exclude files that follow (in xlist)   -d  extract files into"
    " exdir\n"
    "modifiers:\n"
    "  -n  never overwrite existing files         -q  quiet mode"
    " (-qq => quieter)\n"
    "  -o  overwrite files WITHOUT prompting      -j  junk paths"
    " (no directories)\n"
    "  -C  match filenames case-insensitively     -V  retain VMS version"
    " numbers\n"
    "  -D  skip restoring directory times         -DD skip restoring all"
    " times\n"
    "  -y  extract a loose archive into a directory named for the"
    " archive\n"
);
static s8 const unzip_usage_posix = S8(
    "  -X  restore UID/GID info                   -K  keep setuid/setgid/"
    "tacky modes\n"
);
static s8 const unzip_usage_tail = S8(
    "See \"unzip -hh\" for more help.  Examples:\n"
    "  unzip data1 -x joe   => extract all files except joe from zipfile"
    " data1.zip\n"
    "  unzip -p foo | more  => send contents of foo.zip via pipe into"
    " program more\n"
    "  unzip -fo foo ReadMe => quietly replace existing ReadMe if archive"
    " file newer\n"
);

static s8 const unzip_help = S8(
    "Options may come before the archive, as in Info-ZIP UnZip, or after\n"
    "it, as in busybox: an argument after the archive made only of option\n"
    "letters (\"-q\", \"-qo\") is taken for options, not a member name.\n"
    "-d exdir may appear anywhere, and -x starts a list of members to\n"
    "exclude, which runs to the end. A leading - negates the option that\n"
    "follows it (\"--q\" cancels one -q from UNZIP).\n"
    "\n"
    "The archive file[.zip] is tried as given, then with .zip and .ZIP\n"
    "appended. A wildcard (*, ?, [...]) in its name processes every match.\n"
    "An archive of - is read from standard input, and implies -n unless\n"
    "-o is given.\n"
    "\n"
    "Member names in list and xlist are wildcards: * matches any run of\n"
);
static s8 const unzip_help_posix = S8(
    "characters, / included, ? one character, [...] a set, and \\ escapes.\n"
);
static s8 const unzip_help_windows = S8(
    "characters, / included, ? one character, and [...] a set. A \\\n"
    "separates directories, as / does.\n"
);
static s8 const unzip_help_tail = S8(
    "\n"
    "Listings by -l and -v show dates as YYYY-MM-DD (ZipInfo's as\n"
    "Info-ZIP's). Encrypted entries, and entries compressed by methods other\n"
    "than stored, deflated, and Deflate64, are skipped.\n"
    "Options in UNZIP, or if it has none, UNZIPOPT, apply first.\n"
    "\n"
    "Exit status: 0 success, 1 warnings, 2 errors in some entries, 3 a\n"
    "damaged archive, 4 out of memory, 9 no archive found, 10 bad options,\n"
    "11 no matching entries, 12 overlapped entries (a zip bomb), 50 a\n"
    "write error, 51 an archive cut short, 81 entries skipped (unsupported\n"
    "method or encryption).\n"
);

// ZipInfo mode's (unzip -Z, or the program named zipinfo)
static s8 const zipinfo_usage = S8(
    "tugz zipinfo " TUGZ_VERSION ", a subset of Info-ZIP ZipInfo 3.00\n"
    "usage: zipinfo [-12smlvhtTzC] file[.zip] [list...] [-x xlist...]\n"
    "   or: unzip -Z [-12smlvhtTzC] file[.zip] [list...] [-x xlist...]\n"
    "  Lists the entries in list, all by default, except those in xlist,\n"
    "  as \"ls -l\" lists files; file[.zip] may be a wildcard.\n"
    "\n"
    "listing formats:                  -s  short \"ls -l\" (the default)\n"
    "  -1  names only, nothing else    -m  -s with the compression\n"
    "  -2  names, with -h, -t, -z      -l  -s with the compressed size\n"
    "  -v  every field of the records, extra fields decoded\n"
    "other options:\n"
    "  -h  header lines                -t  totals for the entries listed\n"
    "  -z  the archive comment         -T  times as yyyymmdd.hhmmss\n"
    "  -C  match names ignoring case   -x  exclude the members that follow\n"
    "A leading - negates the option after it (\"--h\"). Options in ZIPINFO,\n"
    "or if it has none, ZIPINFOOPT, apply first. unzip run by a name\n"
    "starting with zipinfo or ii (a link or copy named zipinfo, or\n"
    "zipinfo.exe) runs in this mode.\n"
);

// How a message is written (UnZip's MSG_* flags, unzpriv.h)
enum {
    MSG_STDERR = 0x01,  // to standard error, but standard output under -t
    MSG_LNEWLN = 0x20,  // a newline first, unless at the start of a line
    MSG_TNEWLN = 0x40,  // a newline last, unless the message ends one
};

// The output written at a time, as UnZip's slide with Deflate64
enum { UZ_WSIZE = 1 << 16 };

// UnZip's overwrite modes (G.overwrite_mode), for the whole run
enum { OVERWRT_QUERY, OVERWRT_ALWAYS, OVERWRT_NEVER };

// The longest link target held for its link, PATH_MAX on Linux, beyond
// which no system takes one
enum { UZ_LINKMAX = 4096 };

// The least window through which the central directory is read again,
// once checked, for each stage that reads it (planning, the overlap
// check, the entries' processing), rather than held whole: a directory
// that fits is read once more, whole, a larger one a window at a time.
// (The tests make it small, so that every directory is read again.)
#ifndef UZ_CDWIN
#  define UZ_CDWIN (1 << 18)
#endif

// A caller's selection (unzip_call), in place of member patterns: an
// entry found by where its central header is, as unzip reads them, and
// checked by its stored name, then extracted under a name of the
// caller's, relative and '/'-separated, made a path as an entry's own
// name is (uz_mapname), else under its own; marked as it is met, and
// once done: extracted, made, or tested without error.
typedef struct {
    i64 at;
    s8  stored;
    s8  as;      // or null
    b32 met;
    b32 done;
} unzippick;

// The questions of a run, which a caller (unzip_call) may answer in
// place of standard input, a line at a time, as UnZip reads them: to
// replace a file, the answer, then if [r]ename, the new name; to go on
// past a write error
enum { ASK_REPLACE, ASK_NAME, ASK_DISK };

// An entry as a caller lists it (unzip_call's visit), the ordinal'th,
// then, without one (e null), the archive as it was read: its comment,
// cut short (ar->end.cut), its offsets shifted (ar->shift) or wrapped,
// its Zip64 end record moved (ar->moved64), and what ended its central
// directory early (cderr), as a caller may take an archive for one that
// zip, which reads none of these, cannot change.
typedef struct {
    zarchive *ar;
    zentry   *e;
    s8        name;     // decoded, as entry_name decodes it
    i64       at;       // its central header's offset
    i64       ordinal;
    i32       cderr;    // CD_*
    arena    *perm;     // for what the caller keeps
    arena    *scratch;  // for its temporaries, each gone by the next visit
} unzipvisit;

// A link, deferred until the files are extracted, with the empty file
// that holds its place meanwhile, as UnZip's slinkentry with the file
// holding its target
typedef struct {
    s8      path;
    s8      target;
    osattrs attrs;   // the owner, with -X
    u64     dev;     // the placeholder's identity
    u64     ino[2];
    b32     gone;    // its placeholder was replaced by a later entry
    unzippick *pick; // its entry's, to mark done once made (unzip_call)
} xlink;

// The links, and an index of their placeholders' identities: a table
// of 1 + a link's position (0 for none), open addressed, at most half
// full
typedef struct {
    xlink *data;
    iz     len;
    iz     cap;
    iz    *index;
    iz     mask;
} xlinks;

// A directory created by its own entry, to be given its attributes once
// the files are in it, as UnZip's uxdirattr
typedef struct {
    s8      path;
    osattrs attrs;
} xdir;

typedef struct {
    xdir *data;
    iz    len;
    iz    cap;
} xdirs;

typedef struct {
    os     *ctx;
    arena   perm;
    b32     windows;
    i32     crccpu;

    // Options, as UnZip's uO
    i32     vflag;   // -l 1, -v 2 and more
    i32     qflag;   // -q, -qq; -p adds 999
    i32     zflag;
    i32     Dflag;
    i32     Xflag;
    i32     ovall;   // -o
    b32     ovnone;  // -n
    b32     tflag;
    b32     cflag;   // -c, -p
    b32     fflag;
    b32     uflag;
    b32     jflag;
    b32     Cflag;
    b32     Vflag;
    b32     Kflag;
    b32     yflag;   // -y: wrap loose archives in a directory named for one
    i32     showhelp;
    b32     hasexdir;
    s8      exdir;
    b32     extract;  // UnZip's extract_flag: none of -c -l -p -t -v -z
    b32     zinfo;    // ZipInfo mode, its options in zi, the above unset
    ziopts  zi;
    i64     zilast;   // the offset of the last central header it read,
                      // which UnZip (G.crec) carries to the next archive

    s8      zipspec;  // the archive argument
    s8s     fspecs;   // members to process, if any
    s8s     xspecs;   // members to exclude
    b32     xlist;    // -x was given
    b32     allfiles; // UnZip's process_all_files: no list, no -x

    // Output
    writer *out;      // standard output, flushed before standard error,
                      // and at check points on a terminal (writer_poll)
    b32     sol;      // at the start of a line, on either stream
    b32     dup;      // -t errors also to standard error (UnZip's)

    // The run
    inflator *inf;
    inflator *inf64;  // Deflate64's, for an archive with entries by it
    u8       *window; // decoded data, UZ_WSIZE bytes at a time
    s8s     matches;  // wildcard archive names
    b32     noecrec;  // an archive tried had no end record
    s8      zipfn;    // the archive being processed

    // Extraction
    i32     overwrite; // OVERWRT_*: -o, -n, or the prompt's A or N
    reader *answers;   // the prompt's, standard input, unless the archive
    u32     umask;
    s8      root;      // the -d directory and a '/'
    b32     rooted;    // which is there
    s8      checked;   // known to be directories, through its last '/'
    iz      checkcap;  // room for that, the longest path planned
    iz      maxname;   // the longest name planned
    iz      room;      // that extracting an entry takes
    b32     slashed;   // the archive's backslashes were warned of
    xlinks  links;     // links to create once the files are extracted
    xdirs   dirs;      // directories to give attributes after that
    iz      nlinks;    // links, and directories, planned and not yet met
    iz      ndirs;
    u8     *keep;      // room planned for their paths, and links' targets
    iz      keeplen;
    iz      keepcap;

    // The archive being processed
    i64     nentries; // entries read from its central directory
    i32     cderr;    // what ended the reading early: CD_*
    i64     oldshift; // the shift undone to find an entry (UnZip's)

    // A caller's (unzip_call)
    unzippick *picks;  // sorted by at, else null, for member patterns
    iz         npicks;
    unzippick *pick;   // the entry in hand's
    iz       (*ask)(void *data, i32 question, s8 path, u8 *buf, iz cap);
    void     (*visit)(void *data, unzipvisit *);
    void      *data;
} unzip;

// What ended reading a central directory, as UnZip finds it only once
// it gets there
enum {
    CD_OK,
    CD_SIG,   // a header, not the count's, was invalid (or had no name)
    CD_NONAME,
    CD_END,   // a warning: no end signature follows the count's headers
};

// UnZip's uO.tflag: -t, or in ZipInfo mode, the totals line (-t).
static b32 tflagged(unzip *u)
{
    return u->zinfo ? u->zi.tflag : u->tflag;
}

// Write a message as UnZip's UzpMessagePrnt does (fileio.c): with flags
// MSG_*, to standard output, or to standard error, unless testing (-t),
// when everything goes to standard output, so that redirecting it keeps
// the whole report, but errors are repeated on standard error when that
// alone is a terminal. Whether the last message ended a line is tracked
// across both streams. In ZipInfo mode, its totals flag is -t's, so that
// with totals shown, messages for standard error go to standard output.
static void info(unzip *u, i32 flags, s8 msg)
{
    b32 t   = tflagged(u);
    b32 err = (flags & MSG_STDERR) && !t;
    b32 dup = (flags & MSG_STDERR) && t && u->dup;
    u8  nl  = '\n';
    if (err || dup) {
        writer_flush(u->out);
    }
    if ((flags & MSG_LNEWLN) && !u->sol) {
        if (err) {
            os_write(u->ctx, 2, &nl, 1);
        } else {
            writer_byte(u->out, nl);
        }
        if (dup) {
            os_write(u->ctx, 2, &nl, 1);
        }
        u->sol = 1;
    }
    b32 trail = (flags & MSG_TNEWLN) &&
                (msg.len ? msg.s[msg.len-1]!='\n' : !u->sol);
    for (i32 k = 0; k < 2; k++) {
        s8 part = k ? (trail ? (s8){&nl, 1} : (s8){0}) : msg;
        if (!part.len) {
            continue;
        } else if (err) {
            os_write(u->ctx, 2, part.s, part.len);
        } else {
            writer_s8(u->out, part);
        }
        if (dup) {
            os_write(u->ctx, 2, part.s, part.len);
        }
        u->sol = part.s[part.len-1] == '\n';
    }
}

// Formatting, as UnZip's printf formats

// s left-justified in a field of width w, as "%-ws".
static s8 lj(arena *a, s8 s, iz w)
{
    if (s.len >= w) {
        return s;
    }
    s8 r = {newstr(a, w), w};
    bytecopy(r.s, s.s, s.len);
    bytefill(r.s+s.len, ' ', w-s.len);
    return r;
}

// s right-justified in a field of width w, as "%ws".
static s8 rj(arena *a, s8 s, iz w)
{
    if (s.len >= w) {
        return s;
    }
    s8 r = {newstr(a, w), w};
    bytefill(r.s, ' ', w-s.len);
    bytecopy(r.s+w-s.len, s.s, s.len);
    return r;
}

// An unsigned number, as "%wu", or with zero padding, "%0wu".
static s8 unum(arena *a, u64 v, iz w, b32 zero)
{
    u8  tmp[24];
    u8 *e = tmp + countof(tmp);
    u8 *p = e;
    do {
        *--p = (u8)('0' + v%10);
    } while (v /= 10);
    for (; e-p < w && zero; *--p = '0') {}
    s8 r = {newstr(a, e-p), e-p};
    bytecopy(r.s, p, r.len);
    return rj(a, r, w);
}

static s8 hex8(arena *a, u32 v)
{
    s8 r = {newstr(a, 8), 8};
    for (i32 i = 7; i >= 0; i--, v >>= 4) {
        r.s[i] = "0123456789abcdef"[v & 15];
    }
    return r;
}

// "s" after a count other than one.
static s8 plural(u64 n)
{
    return n==1 ? S("") : S("s");
}

// The compression factor as list.c shows it: "%c%d%%", or "100%".
static s8 cfactor(arena *a, u64 uc, u64 c)
{
    i32 f = uz_ratio(uc, c);
    u8  sgn = f<0 ? '-' : ' ';
    f = f<0 ? (-f + 5)/10 : (f + 5)/10;
    if (f == 100) {
        return S("100%");
    }
    return JOIN(a, (s8){&sgn, 1}, unum(a, (u64)f, 0, 0), S("%"));
}

static s8 cstr8(char const *z)
{
    s8 r = {(u8 *)z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

// A name or other string from the archive, made safe to display.
static s8 shown(unzip *u, s8 name, arena *a)
{
    (void)u;
    return uz_filter(name, a);
}

// Display text from the archive, its comment or an entry's, as UnZip's
// do_string (fileio.c) does (DISPLAY): up to any NUL, without carriage
// returns or ^S, and with an escape shown as "^[". Other control
// characters, and what is not UTF-8, are shown as in names (uz_filter),
// where UnZip writes them raw. A newline follows unless it ends one, or
// the text was cut short by the end of the file, where do_string stops.
static void show_text(unzip *u, s8 text, b32 cut, arena scratch)
{
    s8 t = {newstr(&scratch, text.len), 0};
    for (iz i = 0; i < text.len && text.s[i]; i++) {
        u8 c = text.s[i];
        if (c!='\r' && c!=0x13) {
            t.s[t.len++] = c;
        }
    }
    // Each piece between newlines and tabs shown as it is made, in room
    // used again for the next, so as to take memory and time in
    // proportion to the text
    for (iz i = 0; i < t.len;) {
        arena tmp = scratch;
        iz    j   = i;
        for (; j<t.len && t.s[j]!='\n' && t.s[j]!='\t'; j++) {}
        info(u, 0, shown(u, (s8){t.s+i, j-i}, &tmp));
        info(u, 0, j<t.len ? (s8){t.s+j, 1} : S(""));
        i = j + 1;
    }
    if (!cut) {
        info(u, MSG_TNEWLN, S(""));
    }
}

static s8 const end_sig_msg = S8(
    "\nnote:  didn't find end-of-central-dir signature at end of central"
    " dir.\n"
);

static s8 const report_msg = S8(
    "  (please check that you have transferred or created the zipfile in"
    " the\n"
    "  appropriate BINARY mode and that you have compiled UnZip properly)\n"
);

// Options

static i32 show_usage(unzip *u, b32 error)
{
    s8 msg = JOIN(&u->perm, unzip_usage,
                  u->windows ? S("") : unzip_usage_posix, unzip_usage_tail);
    info(u, error ? MSG_STDERR : 0, msg);
    return error ? PK_PARAM : PK_OK;
}

static void show_version(unzip *u)
{
    s8 line = unzip_usage;
    for (line.len = 0; line.s[line.len++] != '\n';) {}
    info(u, 0, line);
}

static s8 const must_exdir =
    S8("error:  must specify directory to which to extract with -d option\n");

// Option letters that UnZip takes but tugz does not, refused
static b32 unsupported(unzip *u, u8 c)
{
    s8 refused = S("aBEFiIJLMNOPQsSTUWY$:^2/");
    return zip_has(refused, c) || (u->windows && (c=='K' || c=='X'));
}

// Option letters taken after the archive, as busybox takes them: those
// supported, but for -d and -x, which there are Info-ZIP's.
static b32 late_option(unzip *u, s8 arg)
{
    s8 letters = S("-bcCDefhjKlnopqtuvVXzy");
    if (arg.len<2 || arg.s[0]!='-') {
        return 0;
    }
    for (iz i = 1; i < arg.len; i++) {
        if (!zip_has(letters, arg.s[i]) || unsupported(u, arg.s[i])) {
            return 0;
        }
    }
    return 1;
}

static i32 minus(i32 v, i32 by)
{
    return MAX(v-by, 0);
}

// Apply an argument of option letters, as UnZip's uz_opts (unzip.c),
// where -d may take the next argument, args[*i+1]. Unknown letters set
// *error, for the usage to follow. Returns 0 or an exit status.
static i32 apply_options(unzip *u, s8 *args, i32 nargs, i32 *i, b32 *error,
                         arena scratch)
{
    s8  arg = args[*i];
    i32 neg = 0;
    for (iz k = 1; k < arg.len; k++) {
        u8 c = arg.s[k];
        if (unsupported(u, c)) {  // even -K and -X, on Windows
            info(u, MSG_STDERR, JOIN(&scratch, S("error:  -"),
                 (s8){&arg.s[k], 1}, S(" option not supported\n")));
            return PK_PARAM;
        }
        switch (c) {
        case '-':
            neg++;
            break;
        case 'b':
            neg = 0;  // -b, the default, does nothing
            break;
        case 'c':
            u->cflag = !neg;
            neg = 0;
            break;
        case 'C':
            u->Cflag = !neg;
            neg = 0;
            break;
        case 'd':
            if (neg) {
                info(u, MSG_STDERR, must_exdir);
                return PK_PARAM;
            } else if (u->hasexdir) {
                info(u, MSG_STDERR, S("error:  -d option used more than once "
                                      "(only one exdir allowed)\n"));
                return PK_PARAM;
            }
            u->exdir = (s8){arg.s+k+1, arg.len-k-1};
            if (!u->exdir.len) {
                if (*i+1>=nargs || (args[*i+1].len && args[*i+1].s[0]=='-')) {
                    info(u, MSG_STDERR, must_exdir);
                    return PK_PARAM;
                }
                u->exdir = args[++*i];
            }
            u->hasexdir = 1;
            k = arg.len;
            break;
        case 'D':
            u->Dflag = neg ? minus(u->Dflag, neg) : u->Dflag+1;
            neg = 0;
            break;
        case 'e':
        case 'x':
            break;  // extraction, the default
        case 'f':
            u->fflag = u->uflag = !neg;
            neg = 0;
            break;
        case 'h':
            if (!u->showhelp) {
                u->showhelp = k+1<arg.len && arg.s[k+1]=='h' ? 2 : 1;
            }
            break;
        case 'j':
            u->jflag = !neg;
            neg = 0;
            break;
        case 'K':
            u->Kflag = !neg;
            neg = 0;
            break;
        case 'l':
            u->vflag = neg ? minus(u->vflag, neg) : u->vflag+1;
            neg = 0;
            break;
        case 'n':
            u->ovnone = !neg;
            neg = 0;
            break;
        case 'o':
            u->ovall = neg ? minus(u->ovall, neg) : u->ovall+1;
            neg = 0;
            break;
        case 'p':
            u->cflag = !neg;
            u->qflag = neg ? minus(u->qflag, 999) : u->qflag+999;
            neg = 0;
            break;
        case 'q':
            u->qflag = neg ? minus(u->qflag, neg) : u->qflag+1;
            neg = 0;
            break;
        case 't':
            u->tflag = !neg;
            neg = 0;
            break;
        case 'u':
            u->uflag = !neg;
            neg = 0;
            break;
        case 'v':
            u->vflag = neg ? minus(u->vflag, neg) : u->vflag ? u->vflag+1 : 2;
            neg = 0;
            break;
        case 'V':
            u->Vflag = !neg;
            neg = 0;
            break;
        case 'X':
            u->Xflag = neg ? minus(u->Xflag, neg) : u->Xflag+1;
            neg = 0;
            break;
        case 'y':
            u->yflag = !neg;
            neg = 0;
            break;
        case 'z':
            u->zflag = neg ? minus(u->zflag, neg) : u->zflag+1;
            neg = 0;
            break;
        case 'Z':
            info(u, MSG_STDERR, S("error:  -Z must be first option for "
                                  "ZipInfo mode (check UNZIP variable?)\n"));
            *error = 1;
            break;
        default:
            *error = 1;
        }
    }
    return 0;
}

// Read what follows the archive, a[i] on: Info-ZIP's -d exdir anywhere,
// -x then a list running to the end, and members, and given late,
// busybox's options. (Without late, as in Info-ZIP, a -x after -x is a
// pattern; with it, ignored.) Returns 0 or an exit status.
static i32 rest_args(unzip *u, s8 *a, i32 n, i32 i, b32 late, b32 *error,
                     arena scratch)
{
    for (; i < n; i++) {
        s8 arg = a[i];
        if (!u->hasexdir && arg.len>=2 && arg.s[0]=='-' && arg.s[1]=='d') {
            u->exdir = (s8){arg.s+2, arg.len-2};
            if (!u->exdir.len) {
                if (i+1 >= n) {
                    info(u, MSG_STDERR, must_exdir);
                    return PK_PARAM;
                }
                u->exdir = a[++i];
            }
            u->hasexdir = 1;
        } else if (zequals(arg, S("-x")) && (late || !u->xlist)) {
            u->xlist = 1;
        } else if (late && late_option(u, arg)) {
            i32 r = apply_options(u, a, n, &i, error, scratch);
            if (r) {
                return r;
            }
        } else {
            // With Windows conventions, '\' separates, as UnZip's port
            // takes it in member names (unzip.c), so escapes nothing
            if (u->windows && zip_has(arg, '\\')) {
                arg = JOIN(&u->perm, arg);
                for (iz k = 0; k < arg.len; k++) {
                    arg.s[k] = arg.s[k]=='\\' ? '/' : arg.s[k];
                }
            }
            *push(&u->perm, u->xlist ? &u->xspecs : &u->fspecs) = arg;
        }
    }
    u->allfiles = !u->fspecs.len && !u->xlist;
    return 0;
}

// Parse the arguments: options, the archive, then members, -x and its
// list, -d, and options again (busybox's), as UnZip's uz_opts and unzip
// do (unzip.c), with the environment's options first. Returns -1 to go
// on, or an exit status.
static i32 unzip_args(unzip *u, unzipconfig *conf, arena scratch)
{
    // UNZIP, or if it has none, UNZIPOPT (envargs.c)
    s8s args = env_args(&u->perm, conf->unzipenv, u->windows);
    if (!args.len) {
        args = env_args(&u->perm, conf->unzipopt, u->windows);
    }
    for (i32 i = 0; i < conf->nargs; i++) {
        *push(&u->perm, &args) = conf->args[i];
    }
    s8 *a = args.data;
    i32 n = (i32)args.len;

    b32 error = 0;
    i32 i     = 0;
    for (; i<n && a[i].len && a[i].s[0]=='-' && !zequals(a[i], S("-")); i++) {
        i32 r = apply_options(u, a, n, &i, &error, scratch);
        if (r) {
            return r;
        }
    }
    b32 zipfile = i < n;
    if (zipfile) {
        u->zipspec = a[i++];
    }
    i32 r = rest_args(u, a, n, i, 1, &error, scratch);
    if (r) {
        return r;
    }

    if (u->showhelp) {
        if (u->showhelp == 2) {
            info(u, 0, JOIN(&u->perm, unzip_usage, S("\n"), unzip_help,
                            u->windows ? unzip_help_windows : unzip_help_posix,
                            unzip_help_tail));
            return PK_OK;
        }
        return show_usage(u, 0);
    }
    if ((u->cflag && (u->tflag || u->uflag)) || (u->tflag && u->uflag) ||
        (u->fflag && u->ovnone)) {
        info(u, MSG_STDERR, S("error:  -fn or any combination of -c, -l, -p,"
                              " -t, -u and -v options invalid\n"));
        error = 1;
    }
    if (u->ovall && u->ovnone) {
        info(u, MSG_STDERR,
             S("caution:  both -n and -o specified; ignoring -o\n"));
        u->ovall = 0;
    }
    if (!zipfile || error) {
        if (u->vflag>=2 && !zipfile) {
            show_version(u);  // "unzip -v": tugz's version, in a line
            return PK_OK;
        }
        return show_usage(u, conf->nargs || error);
    }
    u->extract = !u->cflag && !u->tflag && !u->vflag && !u->zflag;
    if (u->hasexdir && !u->extract) {
        info(u, MSG_STDERR, S("caution:  not extracting; -d ignored\n"));
    }
    if (zequals(u->zipspec, S("-")) && !u->ovall) {
        u->ovnone = 1;  // busybox's: no prompting, stdin being the archive
    }
    return -1;
}

// Parse the arguments in ZipInfo mode, as UnZip's zi_opts and unzip do
// (zipinfo.c, unzip.c), with ZIPINFO's options, or if it has none,
// ZIPINFOOPT's, first: every argument starting with '-' until the
// archive is options, then members, -x and its list, and -d (ignored),
// as in unzip mode, but for busybox's options. Returns -1 to go on, or
// an exit status.
static i32 zipinfo_args(unzip *u, unzipconfig *conf, arena scratch)
{
    s8s args = env_args(&u->perm, conf->zipinfoenv, u->windows);
    if (!args.len) {
        args = env_args(&u->perm, conf->zipinfoopt, u->windows);
    }
    for (i32 i = 0; i < conf->nargs; i++) {
        *push(&u->perm, &args) = conf->args[i];
    }
    s8 *a = args.data;
    i32 n = (i32)args.len;

    u->zinfo = 1;
    u->zi    = zi_new();
    b32 error = 0;
    i32 i     = 0;
    for (; i<n && a[i].len && a[i].s[0]=='-'; i++) {
        for (iz k = 1; k < a[i].len; k++) {
            u8  c = a[i].s[k];
            i32 r = zi_option(&u->zi, c);
            if (r == ZI_REFUSED) {
                info(u, MSG_STDERR, JOIN(&scratch, S("error:  -"),
                     (s8){&a[i].s[k], 1}, S(" option not supported\n")));
                return PK_PARAM;
            }
            error |= r == ZI_BAD;
        }
    }
    if (i==n || error) {
        // (to standard error after an error: the totals flag is unset)
        info(u, error ? MSG_STDERR : 0, zipinfo_usage);
        return error ? PK_PARAM : PK_OK;
    }
    u->zipspec = a[i++];
    zi_resolve(&u->zi, i < n);
    u->Cflag = u->zi.Cflag;
    i32 r = rest_args(u, a, n, i, 0, &error, scratch);
    if (r) {
        return r;
    }
    if (u->hasexdir) {
        info(u, MSG_STDERR, S("caution:  not extracting; -d ignored\n"));
    }
    return -1;
}

// Selection

static i32 member_flags(unzip *u)
{
    return ZIP_SETS | (u->Cflag ? ZIP_FOLD : 0);
}

// Whether an entry, by its name, is selected by the member list and not
// excluded by -x, as extract.c and list.c match, noting in fm and xm,
// if given, the patterns that matched.
static b32 wanted(unzip *u, s8 name, u8 *fm, u8 *xm)
{
    if (u->allfiles) {
        return 1;
    }
    i32 flags = member_flags(u);
    b32 hit   = !u->fspecs.len;
    for (iz i = 0; !hit && i < u->fspecs.len; i++) {
        if (zip_match(u->fspecs.data[i], name, flags)) {
            hit = 1;
            if (fm) {
                fm[i] = 1;
            }
        }
    }
    for (iz i = 0; hit && i < u->xspecs.len; i++) {
        if (zip_match(u->xspecs.data[i], name, flags)) {
            hit = 0;
            if (xm) {
                xm[i] = 1;
            }
        }
    }
    return hit;
}

// The caller's pick of an entry (unzip_call), by where its central
// header is, if its stored name is the pick's too, else null.
static unzippick *pick_of(unzip *u, i64 at, zentry *e)
{
    iz lo = 0;
    for (iz hi = u->npicks; lo < hi;) {
        iz mid = lo + (hi - lo)/2;
        if (u->picks[mid].at < at) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    unzippick *p = lo<u->npicks && u->picks[lo].at==at ? u->picks+lo : 0;
    return p && zequals(p->stored, e->name) ? p : 0;
}

// Whether an entry is the caller's pick, as every entry is without
// picks.
static b32 picked(unzip *u, i64 at, zentry *e)
{
    return !u->picks || pick_of(u, at, e);
}

// The name by which an entry's path is made: the caller's for it, if
// any, else its own.
static s8 path_name(unzip *u, i64 at, zentry *e, s8 name)
{
    unzippick *p = u->picks ? pick_of(u, at, e) : 0;
    return p && p->as.s ? p->as : name;
}

// The entry in hand is done, if it is the caller's pick.
static void pick_done(unzip *u)
{
    if (u->pick) {
        u->pick->done = 1;
    }
}

// Report the members, then the -x patterns, that matched no entry (none
// noted in fm and xm), as UnZip does once it has been through an
// archive. Returns whether a member matched none.
static b32 unmatched(unzip *u, u8 *fm, u8 *xm, arena scratch)
{
    b32 r = 0;
    for (iz i = 0; i < u->fspecs.len; i++) {
        if (!fm[i]) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("caution: filename not matched:"
                                             "  "), u->fspecs.data[i],
                                     S("\n")));
            r = 1;
        }
    }
    for (iz i = 0; i < u->xspecs.len; i++) {
        if (!xm[i]) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("caution: excluded filename not "
                                             "matched:  "), u->xspecs.data[i],
                                     S("\n")));
        }
    }
    return r;
}

// Reading the central directory again failed, or found it changed since
// it was checked: the end of the run, as UnZip's readbyte ends it.
static void reread_failed(unzip *u)
{
    writer_flush(u->out);
    info(u, MSG_STDERR, S("error:  zipfile read error\n"));
    writer_flush(u->out);
    os_exit(u->ctx, PK_BADERR);
}

// The entry whose central header is at *off, read again (zar_next), its
// extra fields in a, and *off moved to the next.
static void next_entry(unzip *u, zarchive *ar, i64 *off, zentry *e, arena *a)
{
    if (zar_next(ar, off, e, a) != ZAR_OK) {
        reread_failed(u);
    }
}

// The entry whose central header is at *off, read again as next_entry
// reads it, but raw, as ZipInfo shows it: its extra fields as they are,
// Zip64's among them, and its offset neither shifted nor unwrapped,
// pointing into the central directory's window, valid until its next
// use.
static void next_raw(unzip *u, zarchive *ar, i64 *off, zentry *e)
{
    i64 cdend = ar->end.cdoff + ar->end.cdsize;
    iz  len   = 0;
    if (zar_header(&ar->cd, *off, cdend, ar->cdlim, !ar->unzip, ar->maxhdr,
                   e, &len) != ZAR_OK) {
        reread_failed(u);
    }
    *off += len;
}

// An entry's name for matching and display: in Unicode, if it has that
// too, as Debian's UnZip shows it, as zar_uname finds it, decoded (on
// Windows, from a code page) into a, in room apart from the scratch that
// decoding uses: at most three bytes for each byte of the name.
static s8 entry_name(unzip *u, zentry *e, arena *a)
{
    iz    cap   = 3*e->name.len + 1;
    byte *room  = (byte *)newstr(a, cap);
    arena keep  = {room, room+cap, u->ctx, 0};
    s8    stale = {0};
    s8    un    = zar_uname(u->ctx, e, u->windows, &u->crccpu, &stale, &keep,
                            *a);
    return un.s ? un : e->name;
}

// Report an invalid central header, the nth of those UnZip counts.
static void cd_error(unzip *u, i64 n, arena scratch)
{
    if (u->cderr == CD_NONAME) {
        info(u, MSG_STDERR, S(":  bad filename length (central)\n"));
        return;
    }
    info(u, MSG_STDERR, JOIN(&scratch, S("error:  expected central file "
         "header signature not found (file #"), unum(&scratch, (u64)n, 0, 0),
         S(").\n"), report_msg));
}

// Listing (list.c)

// An entry's time as list.c shows it: its central extended timestamp's
// in local time, else its DOS time's fields, unnormalized.
static void entry_time(unzip *u, zentry *e, i32 tm[6])
{
    uzizux x = uz_extra_izux(e->cextra, 1, e->dostime);
    if (x.flags & UZ_MTIME) {
        os_localtime(u->ctx, x.mtime, tm);
    } else {
        uz_dosdate(e->dostime, tm);
    }
}

// Year-month-day hour:minute, as Debian lists dates (DF_YMD).
static s8 iso_time(arena *a, i32 const tm[6])
{
    return JOIN(a, unum(a, (u32)tm[0], 2, 1), S("-"),
                unum(a, (u32)tm[1], 2, 1), S("-"),
                unum(a, (u32)tm[2], 2, 1), S(" "),
                unum(a, (u32)tm[3], 2, 1), S(":"),
                unum(a, (u32)tm[4], 2, 1));
}

// A method's name for -v, as list.c's method[], with deflation's level.
static s8 method_name(arena *a, zentry *e)
{
    static char const *const names[] = {
        "Stored", "Shrunk", "Reduce1", "Reduce2", "Reduce3", "Reduce4",
        "Implode", "Token", "Defl:#", "Def64#", "ImplDCL", "",
        "BZip2", "", "LZMA", "", "", "", "Terse", "IBMLZ77",
    };
    u32 m = e->method;
    s8  r = {0};
    if (m<countof(names) && names[m][0]) {
        r = JOIN(a, cstr8(names[m]));
    } else if (m == 97) {
        return S("WavPack");
    } else if (m == 98) {
        return S("PPMd");
    } else {
        return JOIN(a, S("Unk:"), unum(a, m, 3, 1));
    }
    if (m==ZIP_DEFLATE || m==ZIP_DEFLATE64) {
        r.s[5] = (u8)"NXFS"[e->flags>>1 & 3];
    }
    return r;
}

static i32 list_files(unzip *u, zarchive *ar, arena scratch)
{
    b32 longhdr = u->vflag > 1;
    if (u->qflag < 2) {
        info(u, 0, longhdr
            ? S(" Length   Method    Size  Cmpr    Date    Time   CRC-32   Name\n"
                "--------  ------  ------- ---- ---------- ----- --------  ----\n")
            : S("  Length      Date    Time    Name\n"
                "---------  ---------- -----   ----\n"));
    }

    u64 members = 0;
    u64 tusize  = 0;
    u64 tcsize  = 0;
    i64 off     = ar->end.cdoff;
    for (i64 i = 0; i < u->nentries; i++) {
        arena   tmp  = scratch;
        zentry  ent  = {0};
        zentry *e    = &ent;
        writer_poll(u->out);
        i64     at   = off;
        next_entry(u, ar, &off, e, &tmp);
        s8      name = entry_name(u, e, &tmp);
        if (!wanted(u, name, 0, 0) || !picked(u, at, e)) {
            continue;
        }
        i32 tm[6];
        entry_time(u, e, tm);
        u64 usize = (u64)e->usize;
        u64 csize = (u64)e->csize - (e->flags & ZIP_FLAG_ENCRYPTED ? 12 : 0);
        s8  line  = {0};
        if (longhdr) {
            line = JOIN(&tmp, unum(&tmp, usize, 8, 0), S("  "),
                        lj(&tmp, method_name(&tmp, e), 7),
                        unum(&tmp, csize, 8, 0), S(" "),
                        rj(&tmp, cfactor(&tmp, usize, csize), 4), S(" "),
                        iso_time(&tmp, tm), S(" "), hex8(&tmp, e->crc),
                        S("  "));
        } else {
            line = JOIN(&tmp, unum(&tmp, usize, 9, 0), S("  "),
                        iso_time(&tmp, tm), S("   "));
        }
        info(u, 0, JOIN(&tmp, line, shown(u, name, &tmp), S("\n")));
        if (!u->qflag && e->comment.len) {
            show_text(u, e->comment, 0, tmp);
        }
        tusize += usize;
        tcsize += csize;
        members++;
    }
    if (u->cderr==CD_SIG || u->cderr==CD_NONAME) {
        cd_error(u, u->nentries+1, scratch);
        return PK_BADERR;
    }

    if (u->qflag < 2) {
        arena tmp = scratch;
        s8    s   = plural(members);
        s8    n   = unum(&tmp, members, 0, 0);
        if (longhdr) {
            info(u, 0, JOIN(&tmp,
                S("--------          -------  ---                       "
                  "     -------\n"),
                unum(&tmp, tusize, 8, 0), S("         "),
                unum(&tmp, tcsize, 8, 0), S(" "),
                rj(&tmp, cfactor(&tmp, tusize, tcsize), 4),
                S("                            "), n, S(" file"), s,
                S("\n")));
        } else {
            info(u, 0, JOIN(&tmp,
                S("---------                     -------\n"),
                unum(&tmp, tusize, 9, 0), S("                     "),
                n, S(" file"), s, S("\n")));
        }
    }
    i32 err = PK_OK;
    if (u->cderr == CD_END) {
        info(u, MSG_STDERR, end_sig_msg);
        err = PK_WARN;
    }
    return err<=PK_WARN && !members ? PK_FIND : err;
}

// ZipInfo's listing (zipinfo.c)

// One entry as zi_long reports it under -v, the nth in the central
// directory, after a heading: any data before it, by where the entry
// before it ended (*endprev), its name and fields, its extra field's
// subfields, decoded, then its comment. The text of a subfield, or its
// error, at most a few hundred bytes, takes room only until the next.
static void zipinfo_long(unzip *u, zentry *e, s8 name, i64 n, u64 *endprev,
                         arena scratch)
{
    zitext t = zt_new(&scratch, 4096);
    zt_puts(&t, "\nCentral directory entry #");
    zt_putu(&t, (u64)n);
    zt_puts(&t, ":\n---------------------------\n\n");
    zi_preceding(&t, e, endprev);
    info(u, 0, zt_text(&t));
    info(u, 0, JOIN(&scratch, S("  "), shown(u, name, &scratch), S("\n")));

    // The central extended timestamp's time, in local time and in UTC
    uzizux ux = uz_extra_izux(e->cextra, 1, e->dostime);
    i32    local[6];
    i32    utc[6];
    b32    ut = ux.flags & UZ_MTIME;
    if (ut) {
        os_localtime(u->ctx, ux.mtime, local);
        zip_gmtime(ux.mtime, utc);
    }
    t.len = 0;
    zi_long_text(&t, e, ut ? local : 0, utc);
    info(u, 0, zt_text(&t));

    if (e->cextra.len) {
        info(u, 0, S("\n  The central-directory extra field contains:"));
        s8  x      = e->cextra;
        u32 id     = 0;
        s8  data   = {0};
        iz  stated = 0;
        while (zi_next(&x, &id, &data, &stated)) {
            arena  tmp = scratch;
            zitext s   = zt_new(&tmp, 1024);
            if (stated > data.len) {
                zi_cut_text(&s, id, stated, data.len);
                info(u, MSG_STDERR|MSG_LNEWLN, zt_text(&s));
                s.len = 0;
            }
            zi_subfield(&s, id, data, e, endprev, tmp);
            info(u, 0, zt_text(&s));
        }
        info(u, 0, S("\n"));
    }
    t.len = 0;
    zi_oldux(&t, e, endprev);
    info(u, 0, zt_text(&t));

    if (!e->comment.len) {
        info(u, 0, S("\n  There is no file comment.\n"));
    } else {
        info(u, 0, S("\n------------------------- file comment begins ----"
                     "------------------------\n"));
        show_text(u, e->comment, 0, scratch);
        info(u, 0, S("-------------------------- file comment ends -------"
                     "----------------------\n"));
    }
}

// List the entries as zipinfo does, each by its central header (raw,
// as next_raw reads it): names alone (-1, -2), as zi_short shows them
// (-s, -m, -l), or as zi_long reports them (-v); then the totals, and the
// members and -x patterns that matched nothing. An encrypted entry's
// compressed size counts without its 12-byte header in the totals and
// -m, as in UnZip. For -v, where each entry ends is carried to the next,
// to tell of data between them, but past an entry not listed; and to
// the next archive, from UnZip's last header read, if at offset 4 (the
// signature that begins a split archive's first part).
static i32 zipinfo_files(unzip *u, zarchive *ar, arena scratch)
{
    i32 lflag   = u->zi.lflag;
    u8 *fm      = new(&scratch, u->fspecs.len, u8);
    u8 *xm      = new(&scratch, u->xspecs.len, u8);
    u64 members = 0;
    u64 tusize  = 0;
    u64 tcsize  = 0;  // wrapping, as UnZip's unsigned total
    u64 endprev = u->zilast==4 ? 4 : 0;
    i64 off     = ar->end.cdoff;
    for (i64 i = 0; i < u->nentries; i++) {
        arena   tmp  = scratch;
        zentry  ent  = {0};
        zentry *e    = &ent;
        writer_poll(u->out);
        i64     at   = off;
        next_raw(u, ar, &off, e);
        u->zilast = e->offset;
        s8      name = entry_name(u, e, &tmp);
        if (!wanted(u, name, fm, xm) || !picked(u, at, e)) {
            endprev = 0;
            continue;
        }
        u64 usize = (u64)e->usize;
        u64 csize = (u64)e->csize;
        u64 enc   = e->flags & ZIP_FLAG_ENCRYPTED ? 12 : 0;
        s8  line  = {0};
        if (lflag == 10) {
            zipinfo_long(u, e, name, i+1, &endprev, tmp);
        } else if (lflag >= 3) {
            i32 tm[6];
            entry_time(u, e, tm);
            zistr attr = zi_attribs(e->made, e->extattr, name);
            zistr flag = zi_flags(e->flags, e->intattr, e->extattr, e->made,
                                  e->cextra.len);
            zistr meth = zi_method(e->method, e->flags);
            zistr date = zi_date(tm, u->zi.Tflag);
            zistr pct  = zi_percent(uz_ratio(usize, csize-enc));
            line = JOIN(&tmp, zi_s8(&attr), S(" "), zi_host(e->made), S(" "),
                        unum(&tmp, usize, 8, 0), S(" "), zi_s8(&flag),
                        lflag==4 ? zi_s8(&pct) : S(""),
                        lflag==5 ? S(" ") : S(""),
                        lflag==5 ? unum(&tmp, csize, 8, 0) : S(""), S(" "),
                        zi_s8(&meth), S(" "), zi_s8(&date), S(" "));
        }
        if (lflag && lflag!=10) {  // (-h or -t alone: only those lines)
            info(u, 0, JOIN(&tmp, line, shown(u, name, &tmp), S("\n")));
        }
        tusize += usize;
        tcsize += csize - enc;
        members++;
    }
    if (u->cderr==CD_SIG || u->cderr==CD_NONAME) {
        cd_error(u, u->nentries+1, scratch);
        return PK_BADERR;
    }

    if (u->zi.tflag) {
        arena tmp = scratch;
        zistr f   = zi_factor(uz_ratio(tusize, tcsize));
        info(u, 0, JOIN(&tmp, unum(&tmp, members, 0, 0), S(" file"),
                        plural(members), S(", "), unum(&tmp, tusize, 0, 0),
                        S(" bytes uncompressed, "), unum(&tmp, tcsize, 0, 0),
                        S(" bytes compressed:  "), zi_s8(&f), S("\n")));
    }
    unmatched(u, fm, xm, scratch);  // with some matched, still success
    i32 err = PK_OK;
    if (u->cderr == CD_END) {
        info(u, MSG_STDERR, end_sig_msg);
        err = PK_WARN;
    }
    if (lflag == 10) {
        info(u, 0, S("\n"));
    }
    return !members ? PK_FIND : err;
}

// Testing and extracting (extract.c)

enum { DIR_BLKSIZ = 16384 };  // entries checked, then processed

// UnZip's input buffer, whose start, a multiple of its size, it seeks to
// read an entry's local header
enum { UZ_INBUFSIZ = 8192 };

// The version of the format this reads, UnZip's UNZIP_VERSION with
// Zip64, and VMS_UNZIP_VERSION.
enum { UZ_CANDO = 45, UZ_CANDO_VMS = 42 };

// Whether an entry's method is one unzip decodes.
static b32 decodes(zentry *e)
{
    return e->method==ZIP_STORE || e->method==ZIP_DEFLATE ||
           e->method==ZIP_DEFLATE64;
}

// Whether an entry's version, method, and encryption let it be read.
static b32 readable(zentry *e)
{
    u32 ver  = e->needed & 0xff;
    u32 host = e->needed >> 8;
    u32 can  = host==UZ_VMS ? UZ_CANDO_VMS : UZ_CANDO;
    return ver<=can && !(e->flags & ZIP_FLAG_ENCRYPTED) && decodes(e);
}

// Whether an entry can be decoded, else saying why it is skipped, as
// extract.c's store_info.
static b32 store_info(unzip *u, zentry *e, s8 name, arena scratch)
{
    b32 show = !u->qflag;
    u32 ver  = e->needed & 0xff;
    u32 host = e->needed >> 8;
    s8  sk   = JOIN(&scratch, S("   skipping: "),
                    lj(&scratch, shown(u, name, &scratch), 22), S("  "));
    if ((host==UZ_VMS && ver>UZ_CANDO_VMS) || (host!=UZ_VMS && ver>UZ_CANDO)) {
        if (show) {
            u32 can = host==UZ_VMS ? UZ_CANDO_VMS : UZ_CANDO;
            info(u, MSG_STDERR, JOIN(&scratch, sk, S("need "),
                 host==UZ_VMS ? S("VMS") : S("PK"), S(" compat. v"),
                 unum(&scratch, ver/10, 0, 0), S("."),
                 unum(&scratch, ver%10, 0, 0), S(" (can do v"),
                 unum(&scratch, can/10, 0, 0), S("."),
                 unum(&scratch, can%10, 0, 0), S(")\n")));
        }
        return 0;
    }

    if (!decodes(e)) {
        static struct { u16 id; char const *name; } const methods[] = {
            {0, "store"}, {1, "shrink"}, {2, "reduce"}, {3, "reduce"},
            {4, "reduce"}, {5, "reduce"}, {6, "implode"}, {7, "tokenize"},
            {8, "deflate"}, {9, "deflate64"}, {10, "DCL implode"},
            {12, "bzip2"}, {14, "LZMA"}, {18, "IBM/Terse"},
            {19, "IBM LZ77"}, {97, "WavPack"}, {98, "PPMd"},
        };
        if (show) {
            s8 msg = {0};
            for (i32 k = 0; k < countof(methods); k++) {
                if (methods[k].id == e->method) {
                    msg = JOIN(&scratch, sk, S("`"), cstr8(methods[k].name),
                               S("' method not supported\n"));
                }
            }
            if (!msg.s) {
                msg = JOIN(&scratch, sk, S("unsupported compression method "),
                           unum(&scratch, e->method, 0, 0), S("\n"));
            }
            info(u, MSG_STDERR, msg);
        }
        return 0;
    }

    if (e->flags & ZIP_FLAG_ENCRYPTED) {
        if (show) {
            info(u, MSG_STDERR, JOIN(&scratch, sk,
                                     S("encrypted (not supported)\n")));
        }
        return 0;
    }
    return 1;
}

// Whether an entry's local header lies past the end of the archive, as
// examined, where UnZip, seeking it, finds nothing, reports it, and goes
// on to the next entry: an entry not read, so not among those whose
// overlaps are checked (zar_spans), but reported as it comes (do_member).
// One that begins anywhere before that end, in the central directory
// too, is checked for overlaps, as Debian's UnZip checks it.
static b32 beyond(zarchive *ar, zentry *e)
{
    return e->offset >= ar->size;
}

// The index of the entry to be read that begins at off (zar_spans), or
// -1.
static iz span_index(zarchive *ar, i64 off)
{
    iz k = zar_after(ar, off) - 1;
    return k>=0 && ar->spans[k]==off ? k : -1;
}

static s8 const bomb_msg = S8(
    "error: invalid zip file with overlapped components (possible zip bomb)\n"
);

// Read a line of standard input as C's fgets reads one into a buffer of
// cap bytes, as UnZip reads answers: up to cap-1 bytes, through a
// newline. Returns its length, or -1 at the end of the input, or where
// the input is the archive. With Windows conventions, as there in text
// mode, a CR before the newline is dropped: a console's lines end in
// CRLF.
static iz read_answer(unzip *u, u8 *buf, iz cap)
{
    reader *r = u->answers;
    iz      n = 0;
    while (r && n<cap-1 && reader_fill(r)) {
        u8 c = r->buf[r->off++];
        if (c=='\n' && n && buf[n-1]=='\r' && u->windows) {
            n--;
        }
        buf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    return n ? n : -1;
}

// The answer to a question about the file at path, as read_answer reads
// it, or the caller's (unzip_call).
static iz answer(unzip *u, i32 question, s8 path, u8 *buf, iz cap)
{
    return u->ask ? u->ask(u->data, question, path, buf, cap)
                  : read_answer(u, buf, cap);
}

// A write to an extracted file failed: ask, as UnZip's disk_error does,
// whether to go on with the next entry.
static b32 disk_error(unzip *u, s8 sname, arena scratch)
{
    info(u, MSG_STDERR|MSG_LNEWLN, JOIN(&scratch, sname,
         S(":  write error (disk full?).  Continue? (y/n/^C) ")));
    u8 buf[10];
    return answer(u, ASK_DISK, sname, buf, countof(buf))>0 && buf[0]=='y';
}

// Where an entry's data goes when extracted to disk.
typedef struct {
    i32 fd;      // a file, or -1
    u8 *target;  // or a link's target, room for its size, or null
    b32 link;
    b32 goon;    // after a failed write, the next entry is wanted
    iz  len;     // the data decoded, up to its size
} xout;

// Whether an entry's data, decoding to out bytes with the CRC crc, more
// than its size, usize, is nonetheless all of it: the size, from its
// 32-bit field rather than a Zip64 extra, is out's modulo 4 GiB, as a
// writer without Zip64 wraps an entry's of 4 GiB or more, and the CRC
// is right. UnZip, checking only the CRC, takes it so.
static b32 size_wrapped(zentry *e, i64 usize, i64 out, u32 crc)
{
    return !e->usize64 && e->method!=ZIP_STORE && crc==e->crc &&
           usize<=ZIP_MAX32 && out>usize && (u32)out==(u32)usize;
}

// Read and decode an entry's data, its local header found at l, to
// standard output with -c and -p, nowhere with -t, or to disk, out,
// as extract.c's extract_or_test_member, name being, on disk, its path.
// That leaves the line naming it unfinished. Returns a status, PK_DISK
// if output failed.
static i32 test_member(unzip *u, zarchive *ar, zentry *e, zlocal *l,
                       i64 usize, s8 name, xout *disk, arena scratch)
{
    s8  sname = shown(u, name, &scratch);
    b32 named = u->qflag != 0;  // the name precedes errors
    b32 store = e->method == ZIP_STORE;
    if (u->tflag) {
        if (!u->qflag) {
            info(u, 0, JOIN(&scratch, S("    testing: "),
                            lj(&scratch, sname, 22), S("  ")));
        }
    } else if (!u->qflag) {
        s8 verb = disk && disk->link && store ? S("    linking: ") :
                  store ? S(" extracting: ") : S("  inflating: ");
        info(u, 0, JOIN(&scratch, verb, lj(&scratch, sname, 22),
                        disk ? S("  ") : S("  \n")));
    }

    // Local extra fields are checked under -t before the data moves the
    // window from them (TestExtraField)
    u32 eflen = 0;
    u32 efrem = 0;
    for (iz i = 0; l->extra.len-i >= 4;) {
        u32 len = get16(l->extra.s+i+2);
        if ((iz)len > l->extra.len-i-4) {
            eflen = len;
            efrem = (u32)(l->extra.len-i-4);
            break;
        }
        i += 4 + len;
    }

    // Output goes out a window at a time, as UnZip flushes its slide, so
    // that an error in a short entry's data leaves none of it written.
    // Beyond its size, it is still decoded to check, but not written,
    // unless the size wrapped (size_wrapped): then the data is decoded
    // again to write the rest, so that nothing beyond the size is written
    // unchecked. (A link's target, held in room for its size, does not
    // wrap.)
    b32 wraps   = !(disk && disk->link);
    i64 lo      = 0;      // output is written from here
    i64 hi      = usize;  // up to here
    i64 out     = 0;
    u32 crc     = 0;
    b32 bad     = 0;  // invalid compressed data
    b32 overrun = 0;  // more than it says it holds
    for (b32 again = 0;; again = 1) {
        zin *in   = &ar->in;
        i64  pos  = l->data;
        i64  left = e->csize;
        i64  had  = out;  // decoded the first time
        u32  sum  = crc;
        zbuf b    = {0};
        inflator *s = e->method==ZIP_DEFLATE64 ? u->inf64 : u->inf;
        inflate_reset(s);
        b.out    = u->window;
        b.outlen = UZ_WSIZE;
        out = crc = 0;
        for (b32 done = 0; !done;) {
            writer_poll(u->out);
            if (!b.inlen && left) {
                u8 *p = 0;
                iz  n = (iz)MIN(left, ZIN_CAP);
                i32 got = zin_get(in, pos, n, &p);
                if (got < 0) {
                    info(u, MSG_STDERR, S("error:  zipfile read error\n"));
                    writer_flush(u->out);
                    os_exit(u->ctx, PK_BADERR);  // as UnZip's readbyte
                } else if (!got) {
                    bad = !store;  // the file has shrunk: as if at its end
                    break;
                }
                b.in    = p;
                b.inlen = n;
                pos  += n;
                left -= n;
            }

            s8 data = {0};
            if (store) {
                data = (s8){(u8 *)b.in, b.inlen};
                b.inlen = 0;
                done = !left;
            } else {
                i32 r = inflate_stream(s, &b);
                if (r == GZ_OK) {
                    done = 1;
                } else if (r==GZ_NEEDIN && !left) {
                    done = bad = 1;  // ends with the data unfinished
                } else if (r!=GZ_NEEDIN && r!=GZ_NEEDOUT) {
                    done = bad = 1;
                }
                if (bad || (!done && b.outlen)) {
                    continue;  // the window not yet full, nor the end
                }
                data = (s8){u->window, b.out - u->window};
                b.out    = u->window;
                b.outlen = UZ_WSIZE;
            }
            if (!data.len) {
                continue;  // as for an empty stored entry, data.s null
            }
            crc = crc32_update(crc, data.s, data.len, &u->crccpu);
            i64 at   = out;
            out += data.len;
            iz  skip = (iz)MIN(MAX(lo-at, 0), data.len);
            iz  keep = (iz)MIN(MAX(hi-at, 0), data.len) - skip;
            u8 *from = data.s + skip;
            if (u->cflag && keep>0) {
                writer_write(u->out, from, keep);
                if (u->out->err) {
                    info(u, MSG_STDERR, JOIN(&scratch, sname,
                         S(":  write error (disk full?).\n")));
                    return PK_DISK;
                }
            } else if (disk && disk->target && keep>0) {
                bytecopy(disk->target+at+skip, from, keep);
            } else if (disk && keep>0 && disk->fd>=0 &&
                       !os_write(u->ctx, disk->fd, from, keep)) {
                disk->goon = disk_error(u, sname, scratch);
                return PK_DISK;
            }
        }

        if (again) {
            bad |= out!=had || crc!=sum;  // the archive changed under it
            break;
        }
        overrun = out > usize;
        b32 wrapped = wraps && !bad && size_wrapped(e, usize, out, crc);
        overrun &= !wrapped;
        if (!wrapped || (!u->cflag && !disk)) {
            break;  // under -t, nothing more to write
        }
        lo = usize;
        hi = out;
    }
    if (u->cflag && !writer_flush(u->out)) {
        // What remained buffered, the entry's last data perhaps, flushed
        // so that its failure, however little was written, is reported
        info(u, MSG_STDERR, JOIN(&scratch, sname,
             S(":  write error (disk full?).\n")));
        return PK_DISK;
    }
    if (disk) {
        disk->len = (iz)MIN(out, usize);  // perhaps less
    }
    bad |= overrun && crc==e->crc;

    if (bad) {
        s8 what = S("invalid compressed data to inflate");
        if (named) {
            info(u, MSG_STDERR, JOIN(&scratch, S("  error:  "), what, S(" "),
                                     sname, S("\n")));
        } else {
            info(u, MSG_STDERR, JOIN(&scratch, S("\n  error:  "), what,
                                     S("\n")));
        }
        return PK_ERR;
    }
    if (crc != e->crc) {
        if (named) {
            info(u, MSG_STDERR, JOIN(&scratch, lj(&scratch, sname, 22),
                                     S(" ")));
        }
        info(u, MSG_STDERR, JOIN(&scratch, S(" bad CRC "), hex8(&scratch, crc),
                                 S("  (should be "), hex8(&scratch, e->crc),
                                 S(")\n")));
        return PK_ERR;
    }
    if (u->tflag) {
        if (eflen) {
            if (u->qflag) {
                info(u, MSG_STDERR, JOIN(&scratch, lj(&scratch, sname, 22),
                                         S(" ")));
            }
            info(u, MSG_STDERR, JOIN(&scratch,
                 S("bad extra-field entry:\n      EF block length ("),
                 unum(&scratch, eflen, 0, 0),
                 S(" bytes) exceeds remaining EF data ("),
                 unum(&scratch, efrem, 0, 0), S(" bytes)\n")));
            return PK_ERR;
        }
        if (!u->qflag) {
            info(u, 0, S(" OK\n"));
        }
    } else if (!u->qflag && !disk) {
        info(u, 0, S("\n"));
    }
    return PK_OK;
}

// Shift the offsets of the entries, as read from now on, of e, read
// already, and of the spans, as UnZip changes its extra_bytes.
static void reshift(zarchive *ar, zentry *e, i64 by)
{
    for (iz i = 0; i < ar->nspans; i++) {
        ar->spans[i] += by;
    }
    e->offset += by;
    ar->shift += by;
}

// Find an entry's local header as extract.c does, reporting a bad
// offset. Where data before the archive was taken to shift its offsets,
// a first entry not found that way is looked for without the shift, and
// should a later one then not be found, with it again. Returns 0 for a
// header found, else a status, with *stop set for one that ends the
// archive's processing.
static i32 find_local(unzip *u, zarchive *ar, zentry *e, i64 filnum,
                      zlocal *l, b32 *stop, arena scratch)
{
    i32 err = PK_OK;
    for (b32 again = 0;; again = 1) {
        s8 why = {0};
        switch (zar_local(ar, e, l)) {
        case ZAR_OK:
            return err;
        case ZAR_EREAD:
            writer_flush(u->out);
            info(u, MSG_STDERR, S("error:  zipfile read error\n"));
            writer_flush(u->out);
            os_exit(u->ctx, PK_BADERR);  // as UnZip's readbyte
        case ZAR_EEOF:
            why = S("EOF");
            break;
        default:
            if (get32(l->fixed) == ZIP_LOCAL_SIG) {
                // Its data reaches into the next entry, or the central
                // directory
                info(u, MSG_STDERR, bomb_msg);
                *stop = 1;
                return PK_BOMB;
            }
            why = S("local header sig");
        }
        info(u, MSG_STDERR, JOIN(&scratch, S("file #"),
             unum(&scratch, (u64)filnum, 0, 0), S(":  bad zipfile offset ("),
             why, S("):  "), znum(&scratch, e->offset), S("\n")));
        b32 retry = (filnum==1 && ar->shift) || (!ar->shift && u->oldshift);
        if (again || zequals(why, S("EOF")) || !retry) {
            return again || why.len==3 ? PK_BADERR : PK_ERR;
        }
        info(u, MSG_STDERR, S("  (attempting to re-compensate)\n"));
        if (ar->shift) {
            u->oldshift = ar->shift;
            reshift(ar, e, -ar->shift);
        } else {
            reshift(ar, e, u->oldshift);
        }
        err = PK_ERR;
    }
}

// Extracting to disk (extract.c's extract_or_test_entrylist, and
// unix/unix.c's mapname, checkdir, and close_outfile)

// Results of making an entry's directories, as UnZip's MPN_*
enum { MPN_OK, MPN_INF_SKIP, MPN_ERR_SKIP };

// Write a message as C's perror writes it, straight to standard error,
// as UnZip's do for a few failures.
static void perror_(unzip *u, s8 what, s8 why, arena scratch)
{
    writer_flush(u->out);
    s8 msg = JOIN(&scratch, what, S(": "), why, S("\n"));
    os_write(u->ctx, 2, msg.s, msg.len);
}

// Make the directories along full, a path below the -d directory: those
// before its last '/', and given all, full itself, as UnZip's checkdir
// makes them (APPEND_DIR), or with create false, find a missing one to
// skip the entry, silently. A link is not a directory, though one leads
// to a directory (a departure: UnZip follows it), so that no entry is
// written through one, but for the -d directory itself. Those found are
// remembered (u->checked), as by linkless, so that a run of entries in
// one directory examines it once. Sets *made if one was made. Returns
// MPN_*, an error reported, naming the entry by name.
static i32 make_dirs(unzip *u, s8 full, b32 all, b32 create, b32 *made,
                     s8 name, arena scratch)
{
    os *ctx  = u->ctx;
    s8  path = all ? JOIN(&scratch, full, S("/")) : full;
    iz  from = u->root.len;
    s8  done = u->checked;
    *made = 0;
    if (!linkless(ctx, path, from, &done, scratch)) {
        for (iz k = 0; k<path.len && k<done.len && path.s[k]==done.s[k]; k++) {
            from = path.s[k]=='/' ? MAX(k+1, from) : from;
        }
        for (iz k = from; k < path.len; k++) {
            if (path.s[k] != '/') {
                continue;
            }
            arena   tmp  = scratch;
            s8      dir  = {path.s, k};
            os_info st   = {0};
            if (os_stat(ctx, dir, 0, &st, tmp)) {
                if (st.type == FT_DIR) {
                    continue;
                }
                info(u, MSG_STDERR, JOIN(&tmp, S("checkdir error:  "),
                     shown(u, dir, &tmp), S(" exists but is not directory\n"
                     "                 unable to process "),
                     shown(u, name, &tmp), S(".\n")));
                return MPN_ERR_SKIP;
            } else if (!create) {
                return MPN_INF_SKIP;  // freshening: nothing there to freshen
            } else if (!os_mkdir(ctx, dir, tmp)) {
                s8 why = JOIN(&tmp, os_error(ctx));
                if (os_stat(ctx, dir, 0, &st, tmp) && st.type==FT_DIR) {
                    continue;  // made meanwhile
                }
                info(u, MSG_STDERR, JOIN(&tmp, S("checkdir error:  cannot "
                     "create "), shown(u, dir, &tmp), S("\n                 "),
                     why, S("\n                 unable to process "),
                     shown(u, name, &tmp), S(".\n")));
                return MPN_ERR_SKIP;
            }
            *made = 1;
        }
        iz last = 0;
        for (iz k = 0; k < path.len; k++) {
            last = path.s[k]=='/' ? k+1 : last;
        }
        done = (s8){path.s, last};
    }

    // Remember them, where the room planned for that holds them
    u->checked.len = 0;
    if (done.len <= u->checkcap) {
        bytemove(u->checked.s, done.s, done.len);
        u->checked.len = done.len;
    }
    return MPN_OK;
}

// An MS-DOS time as Unix seconds, in local time, as UnZip's
// dos_to_unix_time.
static i64 dos_unix(unzip *u, u32 dostime)
{
    i32 tm[6];
    uz_dosdate(dostime, tm);
    return os_mktime(u->ctx, tm);
}

// Give attrs an entry's times, from its local extra fields (ux), else
// its DOS time, the access time defaulting to the modification time, as
// UnZip's get_extattribs (unix.c). With Windows conventions, as the
// port's getNTfiletime (win32.c) has them: the field's times only along
// with its modification time, else the DOS time for both; without its
// access time, the access time left unchanged, as close_outfile and
// set_direc_attribs pass SetFileTime none; and its creation time, which
// only Windows sets, for files and directories alike.
static void entry_times(unzip *u, osattrs *a, uzizux *ux, u32 dost)
{
    i32 have = ux->flags;
    if (u->windows && !(have & UZ_MTIME)) {
        have = 0;
    }
    a->mtime  = have & UZ_MTIME ? ux->mtime : dos_unix(u, dost);
    a->atime  = have & UZ_ATIME ? ux->atime : a->mtime;
    a->flags |= OS_ATIMES;
    if (u->windows && (have & UZ_MTIME) && !(have & UZ_ATIME)) {
        a->flags |= OS_AKEEPA;
    }
    if ((have & UZ_MTIME) && (have & UZ_CTIME)) {
        a->ctime  = ux->ctime;
        a->flags |= OS_ACTIME;
    }
}

// Results of check_for_newer
enum { DOES_NOT_EXIST = -1, EXISTS_AND_OLDER, EXISTS_AND_NEWER };

// Whether a file is at full, and if so, whether it is as new as the
// entry, as fileio.c's check_for_newer: by the entry's local extended
// timestamp, else its DOS time, to which the file's time is rounded up.
// A link, whose time does not count, is older, with a note.
static i32 check_for_newer(unzip *u, s8 full, uzizux *ux, u32 dostime,
                           arena scratch)
{
    os     *ctx  = u->ctx;
    os_info st   = {0};
    os_info lst  = {0};
    b32     note = !u->qflag && u->overwrite!=OVERWRT_ALWAYS;
    if (!os_stat(ctx, full, 1, &st, scratch)) {
        if (!os_stat(ctx, full, 0, &lst, scratch)) {
            return DOES_NOT_EXIST;
        }
        if (note) {
            info(u, 0, JOIN(&scratch, shown(u, full, &scratch),
                 S(" exists and is a symbolic link with no real file.\n")));
        }
        return EXISTS_AND_OLDER;
    }
    if (os_stat(ctx, full, 0, &lst, scratch) && lst.type==FT_LINK) {
        if (note) {
            info(u, 0, JOIN(&scratch, shown(u, full, &scratch),
                            S(" exists and is a symbolic link.\n")));
        }
        return EXISTS_AND_OLDER;
    }
    i64 existing = st.mtime;
    i64 archive  = ux->mtime;
    if (!(ux->flags & UZ_MTIME)) {
        existing += existing & 1;  // to MS-DOS's two seconds
        archive   = dos_unix(u, dostime);
    }
    return existing>=archive ? EXISTS_AND_NEWER : EXISTS_AND_OLDER;
}

// The longest new name read at the prompt, a line of UnZip's FILNAMSIZ
enum { UZ_NEWNAME = 4096 };

// Ask whether to replace the file at full, as extract.c does, reading
// the answer from standard input. Returns 'y' to replace it, 'n' to
// skip the entry, or 'r' with *rename the name to extract it as, read
// into buf (UZ_NEWNAME bytes), or with none read, a null string, for
// the name it had. A or N answers the rest of the run too.
static u8 ask_replace(unzip *u, s8 full, i32 *err, s8 *rename, u8 *buf,
                      arena scratch)
{
    for (;;) {
        arena tmp = scratch;
        info(u, MSG_STDERR, JOIN(&tmp, S("replace "), shown(u, full, &tmp),
             S("? [y]es, [n]o, [A]ll, [N]one, [r]ename: ")));
        u8 reply[10];  // as UnZip's answerbuf, a longer answer split
        iz n = answer(u, ASK_REPLACE, full, reply, countof(reply));
        if (n < 0) {
            info(u, MSG_STDERR, S(" NULL\n(EOF or read error, treating as "
                                  "\"[N]one\" ...)\n"));
            *err = MAX(*err, PK_WARN);  // not extracted: a warning
            reply[0] = 'N';
        }
        switch (reply[0]) {
        case 'r':
        case 'R':
            for (;;) {
                info(u, MSG_STDERR, S("new name: "));
                iz len = answer(u, ASK_NAME, full, buf, UZ_NEWNAME);
                if (len < 0) {
                    *rename = (s8){0};  // UnZip keeps the name it had
                    return 'r';
                }
                len -= buf[len-1] == '\n';
                if (len) {
                    *rename = (s8){buf, len};
                    return 'r';
                }
            }
        case 'A':
            u->overwrite = OVERWRT_ALWAYS;
            return 'y';
        case 'y':
        case 'Y':
            return 'y';
        case 'N':
            u->overwrite = OVERWRT_NEVER;
            return 'n';
        case 'n':
            return 'n';
        }
        s8 bad = {reply, n};
        if (reply[0]=='\n' || reply[0]=='\r') {
            bad = S("{ENTER}");
        } else {
            bad.len -= bad.s[bad.len-1] == '\n';
        }
        info(u, MSG_STDERR, JOIN(&tmp, S("error:  invalid response ["),
                                 shown(u, bad, &tmp), S("]\n")));
    }
}

// Report the attributes that os_setattrs, os_setdirattrs, or os_symlink
// failed to give the file at full, as UnZip words each: within the line
// naming it, with an item that is (inline), else alone. Returns a
// status, a warning for any failure but a file's mode, which UnZip only
// reports.
static i32 attr_failures(unzip *u, i32 failed, s8 full, osattrs *a, s8 *why,
                         b32 dir, b32 inline_, arena scratch)
{
    i32 err  = PK_OK;
    s8  name = shown(u, full, &scratch);
    if (failed & OS_AOWNER) {
        s8 ids = JOIN(&scratch, S("cannot set UID "),
                      unum(&scratch, a->uid, 0, 0), S(" and/or GID "),
                      unum(&scratch, a->gid, 0, 0));
        info(u, MSG_STDERR, inline_
             ? JOIN(&scratch, S(" (warning) "), ids, S("\n          "),
                    why[0])
             : JOIN(&scratch, S("warning:  "), ids, S(" for "), name,
                    S("\n          "), why[0], S("\n")));
        err = dir ? PK_WARN : err;
    }
    if (dir && (failed & OS_ATIMES)) {
        info(u, MSG_STDERR, JOIN(&scratch, S("warning:  cannot set modif./"
             "access times for "), name, S("\n          "), why[2],
             S("\n")));
        err = PK_WARN;
    }
    if (failed & OS_AMODE) {
        if (dir) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning:  cannot set "
                 "permissions for "), name, S("\n          "), why[1],
                 S("\n")));
            err = PK_WARN;
        } else {
            perror_(u, S("fchmod (file attributes) error"), why[1], scratch);
        }
    }
    if (!dir && (failed & OS_ATIMES)) {
        info(u, MSG_STDERR, inline_
             ? JOIN(&scratch, S(" (warning) cannot set modif./access times"
                                "\n          "), why[2])
             : JOIN(&scratch, S("warning:  cannot set modif./access times "
                    "for "), name, S("\n          "), why[2], S("\n")));
    }
    return err;
}

// Room for n bytes kept to the end of the archive, a link's target or
// the path of a link or directory, from that planned for them.
static u8 *kept(unzip *u, iz n)
{
    if (n > u->keepcap-u->keeplen) {
        reread_failed(u);  // more than planned: the archive has changed
    }
    u8 *p = u->keep + u->keeplen;
    u->keeplen += n;
    return p;
}

// Where a placeholder's identity begins its search in the links' index.
static iz link_slot(xlinks *ls, u64 dev, u64 const ino[2])
{
    u64 h = (dev*0x9e3779b97f4a7c15u ^ ino[0]) * 0xbf58476d1ce4e5b9u;
    h = (h ^ ino[1]) * 0x94d049bb133111ebu;
    return (iz)((h ^ h>>32) & (u64)ls->mask);
}

// Index a link by its placeholder's identity.
static void index_link(xlinks *ls, xlink *k)
{
    iz i = link_slot(ls, k->dev, k->ino);
    for (; ls->index[i]; i = (i + 1) & ls->mask) {}
    ls->index[i] = k - ls->data + 1;
}

// A file of that identity, just removed so that a later entry of its
// name can replace it, makes no link of any whose placeholder it was:
// UnZip finds its placeholder replaced, and its link "invalid", as the
// file that a file system may give the same identity would otherwise
// not be (a link replaced by a link then leaves the later). An identity
// unknown (zero, as on some Windows file systems) is no placeholder's.
static void replaced_placeholder(xlinks *ls, os_info *st)
{
    if (!ls->len || st->type!=FT_FILE || st->size ||
        (!st->ino[0] && !st->ino[1])) {
        return;  // no placeholder
    }
    for (iz i = link_slot(ls, st->dev, st->ino); ls->index[i];
         i = (i + 1) & ls->mask) {
        xlink *k = ls->data + ls->index[i] - 1;
        k->gone |= k->dev==st->dev && k->ino[0]==st->ino[0] &&
                   k->ino[1]==st->ino[1];
    }
}

// Extract an entry, its local header found at l, its name as name, as
// extract.c's extract_or_test_entrylist and extract_or_test_member.
// Returns a status, with *stop set when a write failed and the user does
// not go on.
static i32 extract_member(unzip *u, zarchive *ar, zentry *e, s8 name,
                          zlocal *l, i64 usize, b32 *stop, arena scratch)
{
    os    *ctx  = u->ctx;
    i32    err  = PK_OK;
    u32    dost = get32(l->fixed+10);  // the local header's, as UnZip's
    uzizux ux   = uz_extra_izux(l->extra, 0, dost);  // (the window moves)
    uzmode um   = uz_mode(e->extattr, e->made, e->name, e->cextra,
                          u->Kflag);
    b32    link = um.symlink && usize>0;
    i32    opts = (u->windows ? UZ_WINDOWS : 0) | (u->jflag ? UZ_JUNK : 0) |
                  (u->Vflag ? UZ_KEEPVER : 0);
    u32    mask = um.umask ? u->umask : 0;
    s8     as   = u->pick && u->pick->as.s ? u->pick->as : name;
    uzpath mp   = uz_mapname(as, e->made, opts, &scratch);
    s8     full = {0};
    i32    have = DOES_NOT_EXIST;
    b32    renamed = 0;
    if (name.len>u->maxname || u->root.len+mp.path.len+1>u->checkcap) {
        reread_failed(u);  // longer than planned: the archive has changed
    }
    u8    *newname = newstr(&scratch, UZ_NEWNAME);  // from the prompt
    arena  mark    = scratch;  // reset for each new name's path
    for (;; renamed = 1) {
        full = JOIN(&scratch, u->root, mp.path);

        // A name from MS-DOS may separate with '\', and leading '/' are
        // dropped, as extract.c warns
        if ((mp.flags & UZ_BACKSLASH) && !u->slashed) {
            info(u, MSG_STDERR|MSG_LNEWLN, JOIN(&scratch, S("warning:  "),
                 u->zipfn, S(" appears to use backslashes as path "
                             "separators\n")));
            u->slashed = 1;
            err = MAX(err, PK_WARN);
        }
        if (!renamed && (mp.flags & UZ_ABSOLUTE)) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning:  stripped absolute"
                 " path spec from "), shown(u, mp.full, &scratch), S("\n")));
            err = MAX(err, PK_WARN);
        }

        // Its directories, as mapname makes them, then its warning of
        // ".." dropped, then a directory entry is done
        b32 isdir = (mp.flags & UZ_DIR) != 0;
        b32 made  = 0;
        s8  dirs  = full;
        b32 all   = isdir;
        b32 walk  = !isdir || mp.path.len;
        if (mp.flags & UZ_FAILED) {
            // A name that maps to nothing still has its directories
            s8 pre = mp.name;
            for (; pre.len && pre.s[pre.len-1]!='/'; pre.len--) {}
            uzpath dp = uz_mapname(pre, e->made, opts, &scratch);
            dirs = JOIN(&scratch, u->root, dp.path);
            all  = 1;
            walk = dp.path.len > 0;
        }
        if (walk) {
            i32 r = make_dirs(u, dirs, all, !u->fflag || renamed, &made,
                              mp.name, scratch);
            if (r == MPN_ERR_SKIP) {
                return MAX(err, PK_ERR);
            } else if (r == MPN_INF_SKIP) {
                return err;
            }
        }
        if ((mp.flags & UZ_DOTDOT) && !u->qflag) {
            info(u, 0, JOIN(&scratch, S("warning:  skipped \"../\" path "
                 "component(s) in "), shown(u, mp.name, &scratch), S("\n")));
            err = MAX(err, PK_WARN);
        }
        if (isdir) {
            if (!made) {
                pick_done(u);
                return err;  // it was there: nothing to do
            }
            s8 path = {0};
            if (renamed) {
                path = JOIN(&u->perm, full, S("/"));  // kept to the end
            } else if (u->ndirs-- > 0) {
                // In the room planned for it
                path = (s8){kept(u, full.len+1), full.len+1};
                bytecopy(path.s, full.s, full.len);
                path.s[full.len] = '/';
            } else {
                reread_failed(u);  // more than planned: it has changed
            }
            if (!u->qflag) {
                info(u, 0, JOIN(&scratch, S("   creating: "),
                                shown(u, path, &scratch), S("\n")));
            }
            xdir *d = push(&u->perm, &u->dirs);
            d->path = path;
            d->attrs.mode   = um.mode & 07777 & ~mask;
            d->attrs.flags  = OS_AMODE;
            d->attrs.flags |= uz_host(e->made)!=UZ_UNIX ||
                              !(u->Xflag || u->Kflag) ? OS_ASGID : 0;
            if (u->Dflag <= 0) {
                entry_times(u, &d->attrs, &ux, dost);
            }
            if (u->Xflag && (ux.flags & UZ_OWNER)) {
                d->attrs.uid    = ux.uid;
                d->attrs.gid    = ux.gid;
                d->attrs.flags |= OS_AOWNER;
            }
            pick_done(u);
            return err;
        }
        if (mp.flags & UZ_FAILED) {
            info(u, MSG_STDERR, JOIN(&scratch, S("mapname:  conversion of "),
                 shown(u, mp.name, &scratch), S(" failed\n")));
            return MAX(err, PK_ERR);
        }

        // What is there already, and what to do about it
        have = check_for_newer(u, full, &ux, dost, scratch);
        b32 query = 0;
        b32 skip  = 0;
        switch (have) {
        case DOES_NOT_EXIST:
            skip = u->fflag && !renamed;  // freshening creates nothing
            break;
        case EXISTS_AND_OLDER:
            skip  = u->overwrite == OVERWRT_NEVER;
            query = !skip && u->overwrite!=OVERWRT_ALWAYS;
            break;
        case EXISTS_AND_NEWER:
            skip  = u->overwrite==OVERWRT_NEVER || (u->uflag && !renamed);
            query = !skip && u->overwrite!=OVERWRT_ALWAYS;
            break;
        }
        if (query) {
            s8 rename = {0};
            u8 a = ask_replace(u, full, &err, &rename, newname, scratch);
            if (a=='r' && rename.s) {
                // Each new name in the same room, however many are given
                scratch = mark;
                mp = uz_mapname(rename, e->made, opts, &scratch);
                continue;
            } else if (a == 'r') {
                mp.flags &= ~UZ_DOTDOT;  // the name it had, warned of
                continue;
            }
            skip = a == 'n';
        }
        if (skip) {
            return err;
        }
        break;
    }

    // A link's target, held to the end, in the room planned for it
    u8 *target = 0;
    if (link && u->nlinks--<=0) {
        reread_failed(u);  // more than planned: the archive has changed
    } else if (link && usize<=UZ_LINKMAX) {
        target = kept(u, (iz)usize);
    }

    // Replace what is there, removing it, as open_outfile does, so as
    // never to write through a link, then creating the file anew, or for
    // a link, an empty file to hold its place until links are made
    os_info old = {0};
    if (have!=DOES_NOT_EXIST && !os_stat(ctx, full, 0, &old, scratch)) {
        old.type = FT_OTHER;  // gone already, or not to be known
    }
    if (have!=DOES_NOT_EXIST && !os_remove(ctx, full, scratch) &&
        !os_missing(ctx)) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error:  cannot delete old "),
             shown(u, full, &scratch), S("\n        "), os_error(ctx),
             S("\n")));
        return MAX(err, PK_DISK);
    } else if (have != DOES_NOT_EXIST) {
        replaced_placeholder(&u->links, &old);
    }
    i32 fd = os_open(ctx, full, OS_CREATE, scratch);
    if (fd < 0) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error:  cannot create "),
             shown(u, full, &scratch), S("\n        "), os_error(ctx),
             S("\n")));
        return MAX(err, PK_DISK);
    }

    xout out = {fd, target, link, 0, 0};
    i32  r   = test_member(u, ar, e, l, usize, full, &out, scratch);
    if (r > PK_WARN) {
        os_close(ctx, fd);  // discarded
        *stop = r==PK_DISK && !out.goon;
        return MAX(err, r);
    }

    if (link && !out.target) {
        // Refused, where UnZip would make it: a warning (1), though
        // UnZip's own link failures in close_outfile leave the status
        // alone. The message ends the line naming it, as UnZip's do.
        os_close(ctx, fd);
        info(u, MSG_STDERR, JOIN(&scratch, S("warning:  symbolic link ("),
             shown(u, full, &scratch), S(") failed: target too long\n")));
        return MAX(err, PK_WARN);
    } else if (link) {
        // The placeholder is kept, and known by its identity
        os_info id = {0};
        b32 ok = os_fstat(ctx, fd, &id) && os_keep(ctx, fd);
        if (!os_close(ctx, fd) || !ok) {
            os_remove(ctx, full, scratch);
            out.goon = disk_error(u, shown(u, full, &scratch), scratch);
            *stop = !out.goon;
            return MAX(err, PK_DISK);
        }
        s8 target = {out.target, out.len};  // not its size, if less
        if (!u->qflag) {
            info(u, 0, JOIN(&scratch, S("-> "), shown(u, target, &scratch),
                            S(" ")));
        }
        s8 path = full;
        if (renamed) {
            path = JOIN(&u->perm, full);  // kept to the end
        } else {
            path.s = kept(u, full.len);  // in the room planned for it
            bytecopy(path.s, full.s, full.len);
        }
        xlink *k = push(&u->perm, &u->links);
        k->path   = path;
        k->target = target;
        k->dev    = id.dev;
        k->ino[0] = id.ino[0];
        k->ino[1] = id.ino[1];
        k->pick   = u->pick;
        index_link(&u->links, k);
        if (u->Xflag && (ux.flags & UZ_OWNER)) {
            k->attrs.uid   = ux.uid;
            k->attrs.gid   = ux.gid;
            k->attrs.flags = OS_AOWNER;
        }
        if (u->windows) {
            // The file holding the target gets a file's attributes and
            // times, as the port, which makes no links, extracts it as
            // any file (win32.c, close_outfile)
            k->attrs.mode    = um.mode & 07777 & ~mask;
            k->attrs.dosattr = uz_dosattr(e->extattr);
            k->attrs.flags  |= OS_AMODE;
            if (u->Dflag <= 1) {
                entry_times(u, &k->attrs, &ux, dost);
            }
        }
    } else {
        // Its attributes, then it is kept, as close_outfile gives them
        osattrs a = {0};
        a.mode    = um.mode & 07777 & ~mask;
        a.dosattr = uz_dosattr(e->extattr);
        a.flags   = OS_AMODE;
        if (u->Dflag <= 1) {
            entry_times(u, &a, &ux, dost);
        }
        if (u->Xflag && (ux.flags & UZ_OWNER)) {
            a.uid    = ux.uid;
            a.gid    = ux.gid;
            a.flags |= OS_AOWNER;
        }
        s8  why[OS_AWHY] = {0};
        i32 failed = os_setattrs(ctx, fd, &a, why, &scratch);
        attr_failures(u, failed, full, &a, why, 0, !u->qflag, scratch);

        // A failed close may have lost data
        b32 kept = os_keep(ctx, fd);
        if (!kept || !os_close(ctx, fd)) {
            if (kept) {
                os_remove(ctx, full, scratch);
            } else {
                os_close(ctx, fd);
            }
            out.goon = disk_error(u, shown(u, full, &scratch), scratch);
            *stop = !out.goon;
            return MAX(err, PK_DISK);
        }
        pick_done(u);
    }
    if (!u->qflag) {
        info(u, 0, S("\n"));
    }
    return err;
}

// Create the links deferred (extract.c's set_deferred_symlink), each
// where its placeholder still is, reached through no link. Returns a
// status, which, as UnZip's, failures leave alone.
static i32 finish_links(unzip *u, arena scratch)
{
    os *ctx = u->ctx;
    if (u->links.len && !u->qflag) {
        info(u, 0, S("finishing deferred symbolic links:\n"));
    }
    for (iz i = 0; i < u->links.len; i++) {
        arena   tmp  = scratch;
        xlink  *k    = u->links.data + i;
        s8      none = {0};
        os_info st   = {0};
        b32 ok = !k->gone &&
                 linkless(ctx, k->path, u->root.len, &none, tmp) &&
                 os_stat(ctx, k->path, 0, &st, tmp) && st.type==FT_FILE &&
                 !st.size && st.dev==k->dev && st.ino[0]==k->ino[0] &&
                 st.ino[1]==k->ino[1];
        if (!ok) {
            info(u, MSG_STDERR, JOIN(&tmp, S("warning:  deferred symlink ("),
                 shown(u, k->path, &tmp), S(") failed:\n          invalid "
                 "placeholder file\n")));
            continue;
        }
        os_remove(ctx, k->path, tmp);
        if (!u->qflag) {
            info(u, 0, JOIN(&tmp, S("  "), lj(&tmp, shown(u, k->path, &tmp),
                 22), S(" -> "), shown(u, k->target, &tmp), S("\n")));
        }
        s8  why[OS_AWHY] = {0};
        i32 failed = os_symlink(ctx, k->target, k->path, &k->attrs, why,
                                &tmp);
        if (k->pick && !(failed & OS_ALINK)) {
            k->pick->done = 1;
        }
        if (failed & OS_ALINK) {
            perror_(u, S("symlink error"), why[3], tmp);
        } else if (failed) {
            attr_failures(u, failed, k->path, &k->attrs, why, 0, 0, tmp);
        }
    }
    return PK_OK;
}

// Order paths as strcmp does, descending.
static b32 path_after(s8 a, s8 b)
{
    iz n = MIN(a.len, b.len);
    for (iz i = 0; i < n; i++) {
        if (a.s[i] != b.s[i]) {
            return a.s[i] > b.s[i];
        }
    }
    return a.len > b.len;
}

// Give the directories created by their own entries their attributes,
// deepest first, as extract.c does (SET_DIR_ATTRIB): their paths sorted
// in reverse, each with its '/'. Returns a status.
static i32 finish_dirs(unzip *u, arena scratch)
{
    os   *ctx = u->ctx;
    iz    n   = u->dirs.len;
    xdir *d   = u->dirs.data;
    xdir *t   = new(&scratch, n, xdir);
    for (iz w = 1; w < n; w *= 2) {  // a stable bottom-up merge sort
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid || j<hi) {
                b32 right = i==mid || (j<hi && path_after(d[j].path,
                                                          d[i].path));
                t[k++] = right ? d[j++] : d[i++];
            }
        }
        bytecopy(d, t, n*(iz)sizeof(*d));
    }

    i32 err   = PK_OK;
    u64 nfail = 0;
    for (iz i = 0; i < n; i++) {
        arena tmp  = scratch;
        s8    path = {d[i].path.s, d[i].path.len-1};  // without its '/'
        s8    none = {0};
        s8    why[OS_AWHY] = {0};
        i32   failed = 0;
        if (!linkless(ctx, path, u->root.len, &none, tmp)) {
            failed = d[i].attrs.flags & (OS_AOWNER|OS_AMODE|OS_ATIMES);
            why[0] = why[1] = why[2] = S("Not a directory");
        } else {
            failed = os_setdirattrs(ctx, path, &d[i].attrs, why, &tmp);
        }
        if (failed) {
            i32 r = attr_failures(u, failed, d[i].path, &d[i].attrs, why, 1,
                                  0, tmp);
            err = err ? err : r;
            nfail++;
            info(u, MSG_STDERR, JOIN(&tmp, S("warning:  set times/attribs "
                 "failed for "), shown(u, d[i].path, &tmp), S("\n")));
        }
    }
    if (nfail && !u->qflag) {
        info(u, 0, JOIN(&scratch, S("     failed setting times/attribs for "),
                        unum(&scratch, nfail, 0, 0), S(" dir entries")));
    }
    return err;
}

// Find, check, and process one entry, the filnum'th, named name, as
// extract.c's extract_or_test_entrylist. Returns a status, with *stop set
// for one that ends the archive's processing.
static i32 do_member(unzip *u, zarchive *ar, zentry *e, s8 name, i64 filnum,
                     u8 *read, b32 *stop, arena scratch)
{
    if (beyond(ar, e)) {
        // As UnZip's read fails there: at the 8 KiB block that holds
        // it, if that too lies past the end ("lseek"), else at the
        // header ("EOF"), neither re-compensated
        i64 block = e->offset & -(i64)UZ_INBUFSIZ;
        b32 seek  = block >= ar->size;
        info(u, MSG_STDERR, JOIN(&scratch, S("file #"),
             unum(&scratch, (u64)filnum, 0, 0), S(":  bad zipfile offset ("),
             seek ? S("lseek") : S("EOF"), S("):  "),
             znum(&scratch, seek ? block : e->offset), S("\n")));
        return PK_BADERR;
    }

    // As its central header is read again, it must be one that the
    // overlap check found, and read only once, lest the archive have
    // changed since
    iz k = span_index(ar, e->offset);
    if (k<0 || read[k>>3]>>(k&7) & 1) {
        info(u, MSG_STDERR, bomb_msg);
        *stop = 1;
        return PK_BOMB;
    }
    read[k>>3] |= (u8)(1 << (k&7));

    zlocal l    = {0};
    i32    err  = find_local(u, ar, e, filnum, &l, stop, scratch);
    if (*stop || err>PK_ERR || (err && !l.data)) {
        return err;
    }

    // The names compare as Debian's UnZip compares them, each in Unicode
    // where its own extra fields give that, or, as the central one, its
    // code page (Windows), decoded into room apart from the scratch that
    // decoding uses: at most three bytes for each byte of the name
    arena  lt    = scratch;
    iz     cap   = 3*l.name.len + 1;
    byte  *room  = (byte *)newbytes(&lt, cap);
    arena  keep  = {room, room+cap, u->ctx, 0};
    zentry le    = *e;
    s8     stale = {0};
    le.name   = l.name;
    le.cextra = l.extra;
    s8 lname = zar_uname(u->ctx, &le, u->windows, &u->crccpu, &stale, &keep,
                         lt);
    lname = lname.s ? lname : l.name;
    if (!zequals(lname, name)) {
        arena tmp = lt;  // past the room lname may be in
        info(u, MSG_STDERR, JOIN(&tmp, shown(u, name, &tmp),
             S(":  mismatching \"local\" filename ("),
             shown(u, lname, &tmp), S("),\n         continuing with "
             "\"central\" filename version\n")));
        err = PK_WARN;
    }
    i64 usize = e->usize;
    if (e->method==ZIP_STORE && e->usize!=e->csize) {
        arena tmp = scratch;
        info(u, MSG_STDERR, JOIN(&tmp, shown(u, name, &tmp), S(":  ucsize "),
             znum(&tmp, e->usize), S(" <> csize "), znum(&tmp, e->csize),
             S(" for STORED entry\n         continuing with \"compressed\" "
               "size value\n")));
        usize = e->csize;
        err   = PK_WARN;
    }

    i32 r = 0;
    if (u->extract) {
        r = extract_member(u, ar, e, name, &l, usize, stop, scratch);
        return MAX(err, r);
    }
    r = test_member(u, ar, e, &l, usize, name, 0, scratch);
    *stop = r == PK_DISK;
    if (r == PK_OK) {
        pick_done(u);
    }
    return MAX(err, r);
}

// Plan the test or extraction of the selected entries, before any
// output, reading the central directory again: where each begins, for
// the overlap check, and whether any entry needs Deflate64's inflator;
// to extract them, the longest of their names and paths, and room for
// the paths of links and directories, kept to finish them at the end,
// and for links' targets, and lists for those.
static void plan(unzip *u, zarchive *ar, arena *scratch)
{
    i64    count = u->nentries;
    i32    opts  = (u->windows ? UZ_WINDOWS : 0) |
                   (u->jflag ? UZ_JUNK : 0) | (u->Vflag ? UZ_KEEPVER : 0);
    b32    def64   = 0;
    iz     nlinks  = 0;
    iz     ndirs   = 0;
    iz     longest = 0;
    iz     maxname = 0;
    i64    keep    = 0;
    i64    off     = ar->end.cdoff;
    ar->spans  = new(scratch, (iz)count, i64);
    ar->nspans = 0;
    for (i64 i = 0; i < count; i++) {
        arena  tmp  = *scratch;
        zentry e    = {0};
        i64    at   = off;
        next_entry(u, ar, &off, &e, &tmp);
        s8     name = entry_name(u, &e, &tmp);
        def64 |= e.method == ZIP_DEFLATE64;
        if (!readable(&e) || !wanted(u, name, 0, 0) || !picked(u, at, &e) ||
            beyond(ar, &e)) {
            continue;
        }
        ar->spans[ar->nspans++] = e.offset;
        if (!u->extract) {
            continue;
        }
        s8     as  = path_name(u, at, &e, name);
        uzpath mp  = uz_mapname(as, e.made, opts, &tmp);
        iz     len = u->root.len + mp.path.len;  // its path to make
        maxname = MAX(maxname, name.len);
        longest = MAX(longest, len + 1);
        if (mp.flags & UZ_DIR) {
            if (mp.path.len) {
                keep += len + 1;  // with its '/'
                ndirs++;
            }
            continue;
        } else if (mp.flags & UZ_FAILED) {
            continue;
        }
        uzmode um   = uz_mode(e.extattr, e.made, e.name, e.cextra, u->Kflag);
        i64    size = e.method==ZIP_STORE ? e.csize : e.usize;
        if (um.symlink && size>0) {
            keep += len + (size<=UZ_LINKMAX ? size : 0);
            nlinks++;
        }
    }

    // Deflate64's larger inflator only for an archive that needs it
    u->inf64 = def64 ? inflate64_new(&u->perm) : 0;

    if (u->extract) {
        if (keep > (iz)((uz)-1>>1)) {
            os_oom(u->ctx);  // larger than the address space
        }
        iz slots = 1;
        for (; slots < 2*nlinks; slots *= 2) {}
        u->links    = (xlinks){new(scratch, nlinks, xlink), 0, nlinks,
                               new(scratch, slots, iz), slots-1};
        u->dirs     = (xdirs){new(scratch, ndirs, xdir), 0, ndirs};
        u->nlinks   = nlinks;
        u->ndirs    = ndirs;
        u->keep     = newstr(scratch, (iz)keep);
        u->keeplen  = 0;
        u->keepcap  = (iz)keep;
        u->checked  = (s8){newstr(scratch, longest), 0};
        u->checkcap = longest;
        u->maxname  = maxname;
        u->slashed  = 0;

        // The room that extracting an entry takes, its central header
        // read again, its messages and paths, a new name asked for, and
        // sorting the directories at the end
        u->room = 16*(longest + maxname + ar->maxhdr) + (1<<16) +
                  ndirs*(iz)sizeof(xdir);
    }
}

// Make the -d directory, if not there, as UnZip's checkdir (ROOT) makes
// it, once, in one level, unless freshening (a departure: UnZip then
// freshens the current directory). Returns a status.
static i32 make_root(unzip *u, arena scratch)
{
    s8      dir  = {u->root.s, u->root.len-1};  // as UnZip, without a '/'
    os_info st   = {0};
    if (u->rooted || dir.len<=0 || u->fflag) {
        return PK_OK;
    } else if (!os_stat(u->ctx, dir, 1, &st, scratch) ||
               st.type!=FT_DIR) {
        if (!os_mkdir(u->ctx, dir, scratch)) {
            info(u, MSG_STDERR, JOIN(&scratch, S("checkdir:  cannot create "
                 "extraction directory: "), shown(u, dir, &scratch),
                 S("\n           "), os_error(u->ctx), S("\n")));
            return PK_ERR;
        }
    }
    u->rooted = 1;
    return PK_OK;
}

// The name to wrap an archive's contents under, with -y: the basename of
// the name actually opened, less .zip or .ZIP should it have one, which
// would leave nothing (as .zip alone would) not either.
static s8 zip_dirname(unzip *u)
{
    s8 path = u->zipfn;
    iz base = 0;
    for (iz i = 0; i < path.len; i++) {
        base = path.s[i]=='/' || path.s[i]=='\\' ? i+1 : base;
    }
    s8 name = {path.s+base, path.len-base};
    s8 full  = name;
    if (name.len>4 && name.s[name.len-4]=='.' &&
        zi_lower(name.s[name.len-3])=='z' &&
        zi_lower(name.s[name.len-2])=='i' &&
        zi_lower(name.s[name.len-1])=='p') {
        name.len -= 4;
    }
    return name.len ? name : full;
}

// With -y and no -d, whether an archive's contents are one directory:
// every selected entry mapped, as plan maps them, under the same first
// path component. Should they not be, they are extracted below a
// directory named for the archive. Called once an archive, before plan,
// which sizes its room by the root's length, so that the root must be
// settled here. The root and its made-ness are reset however, so that
// one archive of a run may not extract below another's.
static void wrap_root(unzip *u, zarchive *ar, arena scratch)
{
    if (!u->yflag || !u->extract || u->hasexdir ||
        zequals(u->zipfn, S("-"))) {
        return;
    }
    u->root   = S("");
    u->rooted = 0;
    i32    opts = (u->windows ? UZ_WINDOWS : 0) |
                  (u->jflag ? UZ_JUNK : 0) | (u->Vflag ? UZ_KEEPVER : 0);
    s8     comp = {0};
    b32    one  = 1;  // so far, all entries under one component
    i64    off  = ar->end.cdoff;
    for (i64 i = 0; i < u->nentries && one; i++) {
        arena  tmp  = scratch;
        zentry e    = {0};
        i64    at   = off;
        next_entry(u, ar, &off, &e, &tmp);
        s8     name = entry_name(u, &e, &tmp);
        if (!readable(&e) || !wanted(u, name, 0, 0) || !picked(u, at, &e) ||
            beyond(ar, &e)) {
            continue;
        }
        uzpath mp = uz_mapname(path_name(u, at, &e, name), e.made, opts, &tmp);
        if (mp.flags & UZ_FAILED) {
            continue;
        }
        s8 p = mp.path;
        if (!p.len) {  // an empty name, or a directory entry under -j
            continue;
        }
        iz cut = p.len;  // past the first component, or the whole of it
        for (iz k = 0; k < p.len; k++) {
            if (p.s[k]=='/') {
                cut = k;
                break;
            }
        }
        s8 first = {p.s, cut};
        if (!(mp.flags & UZ_DIR)) {
            one &= cut < p.len;  // a file must sit below a component
        }
        if (!first.len) {
            one = 0;
        } else if (!comp.s) {
            comp = JOIN(&scratch, first);  // kept past each entry's room
        } else {
            one &= zequals(comp, first);
        }
    }
    if (one) {
        return;
    }
    s8 base = zip_dirname(u);
    u->root = JOIN(&u->perm, base, S("/"));
    if (u->qflag < 1) {
        info(u, 0, JOIN(&scratch, S("note:  extracting into "), base,
                        S("/\n")));
    }
}

// Test the selected entries, or extract them, to disk or standard
// output, as extract.c's extract_or_test_files.
static i32 extract_or_test(unzip *u, zarchive *ar, arena scratch)
{
    wrap_root(u, ar, scratch);
    i64 count = u->nentries;
    u8 *fm    = new(&scratch, u->fspecs.len, u8);
    u8 *xm    = new(&scratch, u->xspecs.len, u8);

    // The entries to be read must not overlap, nor reach into the
    // central directory, as Debian's UnZip finds them out: here, before
    // any are read, the least each could take, a local header and its
    // data, and once each local header is read, what it does take. No
    // two begin at once, and each ends by where the next one begins.
    // (One that begins past the end of the file is not read: beyond.)
    plan(u, ar, &scratch);
    zar_spans(ar);
    u8 *read = new(&scratch, (ar->nspans+7)/8, u8);  // a bit for each
    b32 bomb = 0;
    for (iz k = 1; k < ar->nspans; k++) {
        bomb |= ar->spans[k-1] == ar->spans[k];
    }
    i64 off = ar->end.cdoff;
    for (i64 i = 0; i<count && !bomb; i++) {
        arena  tmp  = scratch;
        zentry e    = {0};
        i64    at   = off;
        next_entry(u, ar, &off, &e, &tmp);
        s8     name = entry_name(u, &e, &tmp);
        // (a compressed size, which listings take as it is, may reach
        // anywhere, up to 2^63: zar_overlaps subtracts)
        bomb = readable(&e) && wanted(u, name, 0, 0) && picked(u, at, &e) &&
               !beyond(ar, &e) && zar_overlaps(ar, &e);
    }
    if (bomb) {
        info(u, MSG_STDERR, bomb_msg);
        return PK_BOMB;
    }

    // Extracting an entry also takes room for its local header's name,
    // which lies, with the entry's data, before where the next one
    // begins (or later by the shift that find_local may undo): room to
    // decode it, three bytes a byte, then, should it differ from the
    // central name, its shown form and the message's copy of that, twice
    // its length each, which on Windows, decoded from a code page, may be
    // three times the name's
    i64 span = 0;
    for (iz k = 0; k < ar->nspans; k++) {
        span = MAX(span, zar_limit(ar, ar->spans[k]) - ar->spans[k]);
    }
    iz local = (iz)MIN(MAX(span+ar->shift-ZIP_LOCAL_LEN, 0), ZIP_MAX16);
    u->room += (u->windows ? 3+12 : 3+4)*local + (1<<8);

    i32  err     = PK_OK;
    i64  filnum  = 0;
    u64  skipped = 0;
    i64 *block   = new(&scratch, DIR_BLKSIZ, i64);  // their headers
    i64 *floors  = 0;  // given wrapped offsets, from where each unwraps
    if (ar->wrapped) {
        floors = new(&scratch, DIR_BLKSIZ, i64);
    }
    iz   nblock  = 0;
    b32  stop    = 0;

    // Claimed, the room that extracting each entry takes, so that none is
    // claimed once files are written (but for a new name, from the
    // prompt, for a link or directory, kept to the end), then the -d
    // directory made
    if (u->extract) {
        arena probe = scratch;
        newbytes(&probe, u->room);
        i32 r = make_root(u, scratch);
        if (r) {
            return r;
        }
    }

    off = ar->end.cdoff;
    for (i64 i = 0; i<=count && !stop; i++) {
        if (i==count && (u->cderr==CD_SIG || u->cderr==CD_NONAME)) {
            // As the scan finds it, before the entries of its block
            cd_error(u, nblock + filnum + 1, scratch);
            err = PK_BADERR;
        } else if (i < count) {
            arena  tmp   = scratch;
            zentry e     = {0};
            i64    at    = off;
            i64    floor = ar->floor;
            next_entry(u, ar, &off, &e, &tmp);
            s8     name  = entry_name(u, &e, &tmp);
            if (!wanted(u, name, fm, xm) || !picked(u, at, &e)) {
                continue;
            } else if (u->picks) {
                pick_of(u, at, &e)->met = 1;
            }
            if (!store_info(u, &e, name, tmp)) {
                skipped++;
                continue;
            }
            if (floors) {
                floors[nblock] = floor;
            }
            block[nblock++] = at;
            if (nblock < DIR_BLKSIZ) {
                continue;
            }
        }
        i64 scan = ar->floor;  // where the scan goes on
        for (iz k = 0; k<nblock && !stop; k++) {
            // Read again, as planned, unless the archive has changed
            arena  tmp  = scratch;
            zentry e    = {0};
            i64    at   = block[k];
            ar->floor = floors ? floors[k] : 0;
            next_entry(u, ar, &at, &e, &tmp);
            s8     name = entry_name(u, &e, &tmp);
            if (!readable(&e) || !wanted(u, name, 0, 0) ||
                !picked(u, block[k], &e) ||
                (e.method==ZIP_DEFLATE64 && !u->inf64)) {
                reread_failed(u);
            }
            u->pick = u->picks ? pick_of(u, block[k], &e) : 0;
            writer_poll(u->out);
            i32 r = do_member(u, ar, &e, name, ++filnum, read, &stop, tmp);
            err = MAX(err, r);
        }
        ar->floor = scan;
        nblock = 0;
    }

    // Links, deferred, then directories' attributes, even when stopped
    if (u->extract) {
        i32 r = finish_links(u, scratch);
        err = MAX(err, r);
        r = finish_dirs(u, scratch);
        err = MAX(err, r);
    }
    if (stop) {
        return err;  // no summary, as when UnZip stops early
    }
    if (u->cderr == CD_END) {
        info(u, MSG_STDERR, JOIN(&scratch, end_sig_msg, report_msg));
        err = MAX(err, PK_WARN);
    }

    if (unmatched(u, fm, xm, scratch)) {
        err = err<=PK_WARN ? PK_FIND : err;
    }
    for (iz i = 0; i < u->npicks; i++) {
        // As members are, picks of entries not there, as when the
        // archive has changed since the caller listed it
        if (!u->picks[i].met) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("caution: filename not matched:"
                                             "  "),
                                     shown(u, u->picks[i].stored, &tmp),
                                     S("\n")));
            err = err<=PK_WARN ? PK_FIND : err;
        }
    }

    if (u->tflag && u->qflag<2) {
        arena tmp = scratch;
        if (err) {
            info(u, 0, JOIN(&tmp, S("At least one "),
                            err==PK_WARN ? S("warning-") : S(""),
                            S("error was detected in "), u->zipfn, S(".\n")));
        } else if (!filnum) {
            info(u, 0, JOIN(&tmp, S("Caution:  zero files tested in "),
                            u->zipfn, S(".\n")));
        } else if (u->allfiles && !skipped) {
            info(u, 0, JOIN(&tmp, S("No errors detected in compressed data "
                                    "of "), u->zipfn, S(".\n")));
        } else {
            info(u, 0, JOIN(&tmp, S("No errors detected in "), u->zipfn,
                            S(" for the "), unum(&tmp, (u64)filnum, 0, 0),
                            S(" file"), plural((u64)filnum),
                            S(" tested.\n")));
        }
        if (skipped) {
            info(u, 0, JOIN(&tmp, unum(&tmp, skipped, 0, 0), S(" file"),
                            plural(skipped), S(" skipped because of "
                            "unsupported compression or encoding.\n")));
        }
    }

    if (!filnum && err<=PK_WARN) {
        err = skipped ? IZ_UNSUP : PK_FIND;
    } else if (skipped && err<=PK_WARN) {
        err = IZ_UNSUP;
    }
    return err;
}

// Archives (process.c)

static s8 const no_endsig = S8(
    "  End-of-central-directory signature not found.  Either this file is"
    " not\n"
    "  a zipfile, or it constitutes one disk of a multi-part archive.  In"
    " the\n"
    "  latter case the central directory and zipfile comment will be found"
    " on\n"
    "  the last disk(s) of this archive.\n"
);

static s8 const no_end64sig = S8(
    "fatal error: read failure while seeking for End-of-centdir-64"
    " signature.\n"
    "  This zipfile is corrupt.\n"
);

// The central directory's offset as the end record has it, undoing
// zip_find_end's taking an empty one at offset 0 to be where it is.
static i64 raw_cdoff(zarchive *ar)
{
    zend *e = &ar->end;
    u8   *p = 0;
    if (!e->count && !e->cdsize && e->end64<0 && e->cdoff==e->endpos &&
        zin_get(&ar->in, e->endpos+16, 4, &p)>0) {
        return get32(p);
    }
    return e->cdoff;
}

// ZipInfo's -v comment section (process_zip_cmmnt): the archive comment
// between banners, as far as the file holds it, by its recorded length,
// or that it has none. Returns a status: a warning if it was cut short.
static i32 zipinfo_comment(unzip *u, zarchive *ar, arena scratch)
{
    zend *e   = &ar->end;
    u64   len = (u64)e->comment.len;
    u8   *p   = 0;
    if (e->cut && zin_get(&ar->in, e->endpos+20, 2, &p)>0) {
        len = get16(p);  // as recorded, past the end of the file
    }
    if (!len) {
        info(u, 0, S("There is no zipfile comment.\n"));
        return PK_OK;
    }
    info(u, 0, JOIN(&scratch, S("The zipfile comment is "),
                    unum(&scratch, len, 0, 0), S(" bytes long and contains "
                    "the following text:\n======================== zipfile "
                    "comment begins ==========================\n")));
    show_text(u, e->comment, e->cut, scratch);
    info(u, 0, S("========================= zipfile comment ends ============"
                 "===============\n"));
    if (e->cut) {
        info(u, 0, S("\n  The zipfile comment is truncated.\n"));
        return PK_WARN;
    }
    return PK_OK;
}

// ZipInfo's -v end-record section (zi_end_central), with the end record's
// fields as zar_open found them, before any shift, raw_cdoff's offset.
static void zipinfo_end(unzip *u, zarchive *ar, i64 size, arena scratch)
{
    zend  *e = &ar->end;
    ziend  z = {0};
    z.size   = size;
    z.real   = e->end64>=0 ? e->end64 : e->endpos;
    z.cdoff  = (u64)raw_cdoff(ar);
    z.cdsize = (u64)e->cdsize;
    z.count  = (u64)e->count;
    z.ndisk  = e->ndisk;
    z.disk   = e->disk;
    z.cddisk = e->cddisk;
    zitext t = zt_new(&scratch, 2048);
    zi_end_text(&t, &z);
    info(u, 0, zt_text(&t));
}

// Show the caller each entry (unzip_call's visit) as listings read them,
// then the archive as it was read. Returns a status, as list_files does.
static i32 visit_files(unzip *u, zarchive *ar, arena scratch)
{
    unzipvisit v = {0};
    v.ar   = ar;
    v.perm = &u->perm;
    i64 off = ar->end.cdoff;
    for (i64 i = 0; i < u->nentries; i++) {
        arena  tmp = scratch;
        zentry e   = {0};
        v.at = off;
        next_entry(u, ar, &off, &e, &tmp);
        v.e       = &e;
        v.name    = entry_name(u, &e, &tmp);
        v.ordinal = i;
        v.scratch = &tmp;
        u->visit(u->data, &v);
    }
    v.e       = 0;
    v.name    = (s8){0};
    v.cderr   = u->cderr;
    v.scratch = &scratch;
    u->visit(u->data, &v);
    if (u->cderr==CD_SIG || u->cderr==CD_NONAME) {
        cd_error(u, u->nentries+1, scratch);
        return PK_BADERR;
    } else if (u->cderr == CD_END) {
        info(u, MSG_STDERR, end_sig_msg);
        return PK_WARN;
    }
    return PK_OK;
}

// Process one archive, at path, or with stdin, standard input, as
// process.c's do_seekable. With lastchance, a missing one is reported.
// Returns a status, or PK_NOZIP or IZ_DIR for none there.
static i32 do_archive(unzip *u, s8 path, b32 lastchance, b32 stdin,
                      arena scratch)
{
    zarchive ar     = {0};
    i32      fd     = -1;
    i64      size   = 0;
    b32      exe    = 0;
    os_info  info_  = {0};
    if (stdin) {
        // Read in place if a regular file, else spooled into memory
        if (os_fstat(u->ctx, 0, &info_) && info_.type==FT_FILE) {
            fd   = 0;
            size = info_.size;
        } else {
            s8 data = read_all(u->ctx, 0, &u->perm);
            if (!data.s) {
                info(u, MSG_STDERR, S("error:  zipfile read error\n"));
                return PK_BADERR;
            }
            zin_memory(&ar.in, data);
            size = data.len;
        }
    } else {
        b32 found = os_stat(u->ctx, path, 1, &info_, scratch);
        if (!found || info_.type==FT_DIR) {
            if (lastchance && u->qflag<3) {
                arena tmp = scratch;
                s8    w   = u->zipspec;
                s8    who = u->zinfo ? S("zipinfo") : S("unzip");
                s8    msg = u->noecrec
                    ? JOIN(&tmp, who, S(":  cannot find zipfile directory in "
                           "one of "), w, S(" or\n        "),
                           u->zinfo ? S("  ") : S(""), w,
                           S(".zip, and cannot find "), path, S(", period.\n"))
                    : JOIN(&tmp, who, S(":  cannot find or open "), w, S(", "),
                           w, S(".zip or "), path, S(".\n"));
                info(u, MSG_STDERR, msg);
            }
            return found ? IZ_DIR : PK_NOZIP;
        }
        exe = (info_.mode & 0100) != 0;
        fd  = os_open(u->ctx, path, OS_READ|OS_REGULAR, scratch);
        if (fd == OS_ENOTREG) {
            fd = -2;  // a device or FIFO: read as an empty file
        } else if (fd < 0) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("error:  cannot open zipfile [ "),
                 path, S(" ]\n        "), os_error(u->ctx), S("\n")));
            return PK_NOZIP;
        } else if (os_fstat(u->ctx, fd, &info_)) {
            size = info_.size;
        }
    }
    u->zipfn = path;

    i32 err = PK_OK;
    if (u->zinfo ? u->zi.hflag : !u->qflag) {
        info(u, 0, JOIN(&scratch, S("Archive:  "), path, S("\n")));
    }

    ar.unzip = 1;  // read as UnZip reads it
    i32 r = fd==-2 ? ZAR_ENOEND : zar_open(&ar, u->ctx, fd, size, &u->perm);
    if (r==ZAR_EREAD || r==ZAR_EEOF) {
        info(u, MSG_STDERR, S("error:  zipfile read error\n"));
        err = r==ZAR_EREAD ? PK_BADERR : PK_EOF;
        goto done;
    }

    // The comment, as find_ecrec shows it, before checking the records,
    // as far as the file holds it, warning if it ends first (in ZipInfo
    // mode, only under -z, or -v's section whatever -z says)
    b32 cut     = r!=ZAR_ENOEND && ar.end.cut;
    b32 show    = u->zinfo ? u->zi.zflag : u->zflag || !u->qflag;
    b32 verbose = u->zinfo && u->zi.lflag==10;  // ZipInfo's -v sections
    if (r!=ZAR_ENOEND && verbose) {
        err = zipinfo_comment(u, &ar, scratch);
    } else if (r!=ZAR_ENOEND && (ar.end.comment.len || cut) && show) {
        show_text(u, ar.end.comment, cut, scratch);
        if (cut) {
            info(u, MSG_STDERR, S("\ncaution:  zipfile comment truncated\n"));
            err = PK_WARN;
        }
    }

    // No end record, or as find_ecrec64 fails, no Zip64 end record where
    // its locator says, nor just before the locator: perhaps the wrong
    // file, as UnZip takes it
    if (r==ZAR_ENOEND || ar.nosig64) {
        if (u->qflag || u->zinfo) {
            info(u, MSG_STDERR, JOIN(&scratch, S("["), path, S("]\n")));
        }
        info(u, MSG_STDERR, r==ZAR_ENOEND ? no_endsig : no_end64sig);
        if (exe) {
            info(u, MSG_STDERR, JOIN(&scratch, S("note:  "), path,
                 S(" may be a plain executable, not an archive\n")));
        }
        if (fd > 0) {  // standard input stays open
            os_close(u->ctx, fd);
        }
        if (lastchance || stdin) {
            return stdin ? PK_NOZIP : PK_ERR;
        }
        u->noecrec = 1;  // perhaps the wrong file: try NAME.zip
        return PK_NOZIP;
    }

    // A Zip64 end record found just before its locator, rather than
    // where that says: its offsets shift by the data before the archive
    if (ar.moved64) {
        if (u->qflag || u->zinfo) {
            info(u, MSG_STDERR, JOIN(&scratch, S("["), path, S("]\n")));
        }
        info(u, MSG_STDERR, S("error: End-of-centdir-64 signature not where "
             "expected (prepended bytes?)\n  (attempting to process "
             "anyway)\n"));
    }
    if (u->zflag) {
        goto done;
    }
    // ZipInfo's header line, as zi_end_central gives it, of the file's
    // size, with any data before the archive, and the count of entries;
    // or -v's section on the end records
    if (verbose) {
        zipinfo_end(u, &ar, size, scratch);
    } else if (u->zinfo && u->zi.hflag) {
        info(u, 0, JOIN(&scratch, S("Zip file size: "), znum(&scratch, size),
                        S(" bytes, number of entries: "),
                        unum(&scratch, (u64)ar.end.count, 0, 0), S("\n")));
    }

    // Data before the archive, unaccounted for by its offsets, shifts
    // them, as UnZip's extra_bytes; offsets past it are an error
    if (r==ZAR_EMULTI && ar.end.end64<0 && (ar.end.disk || ar.end.cddisk)) {
        // Disk numbers in the end record alone, which UnZip takes for
        // the parts of an archive concatenated, warning of the last one;
        // ZipInfo of disk numbers that disagree, refusing an archive
        // whose central directory begins on an earlier disk
        if (u->zinfo && ar.end.disk>ar.end.cddisk) {
            info(u, MSG_STDERR, JOIN(&scratch, S("\n   ["), path,
                 S("]:\n     Zipfile is disk "), unum(&scratch, ar.end.disk,
                 0, 0), S(" of a multi-disk archive, and this is not the "
                 "disk on\n     which the central zipfile directory begins "
                 "(disk "), unum(&scratch, ar.end.cddisk, 0, 0), S(").\n")));
            err = PK_FIND;
            goto done;
        }
        zend end = ar.end;
        end.disk = end.cddisk = 0;
        switch (zip_check_end32(&end)) {
        case ZIP_OK:
        case ZIP_ECOUNT:  // read past, as zar_open takes it
            r = ZAR_OK;
            break;
        case ZIP_EPREFIX:
            r = ZAR_EPREFIX;
            break;
        default:
            r = ZAR_EFORMAT;
        }
        if (u->zinfo && ar.end.disk<ar.end.cddisk) {
            info(u, MSG_STDERR, JOIN(&scratch, S("\nwarning ["), path,
                 S("]:  end-of-central-directory record claims this\n  is "
                   "disk "), unum(&scratch, ar.end.disk, 0, 0), S(" but that "
                 "the central directory starts on disk "),
                 unum(&scratch, ar.end.cddisk, 0, 0), S("; this is a\n  "
                 "contradiction.  Attempting to process anyway.\n")));
            err = PK_WARN;
        } else if (!u->zinfo && ar.end.disk) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                 S("]:  zipfile claims to be last disk of a multi-part "
                   "archive;\n  attempting to process anyway, assuming all "
                   "parts have been concatenated\n  together in order.  "
                   "Expect \"errors\" and warnings...true multi-part "
                   "support\n  doesn't exist yet (coming soon).\n")));
            err = PK_WARN;
        }
        ar.end = end;
    }
    // Fields that defer to a Zip64 end record that could not be used
    // (ZAR_EFORMAT) tell nothing: no distance comes of them
    i64 cdoff = raw_cdoff(&ar);
    i64 real  = ar.end.end64>=0 ? ar.end.end64 : ar.end.endpos;
    b32 sat   = ar.end.end64>=0 &&
                (ar.end.cdsize==ZIP_MAX32 || cdoff==ZIP_MAX32);
    i64 extra = sat ? 0 : real - (cdoff + ar.end.cdsize);
    if (r == ZAR_EMULTI) {
        info(u, MSG_STDERR, JOIN(&scratch, S("\nerror ["), path,
             S("]:  zipfile is part of multi-disk archive\n"
               "  (sorry, not yet supported).\n")));
        err = PK_FIND;
        goto done;
    } else if (r==ZAR_EFORMAT && extra<0) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
             S("]:  missing "), znum(&scratch, -extra),
             S(" bytes in zipfile\n  (attempting to process anyway)\n")));
        err = PK_ERR;
    } else if (extra > 0) {
        if (!cdoff && ar.end.cdsize) {
            info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
                 S("]:  NULL central directory offset\n"
                   "  (attempting to process anyway)\n")));
            ar.end.cdoff = extra;
            err = PK_ERR;
        } else {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                 S("]:  "), znum(&scratch, extra), S(" extra byte"),
                 plural((u64)extra), S(" at beginning or within zipfile\n"
                 "  (attempting to process anyway)\n")));
            ar.shift     = extra;
            ar.end.cdoff = cdoff + extra;
            err = PK_WARN;
            // Or, without Zip64 records, by a multiple of 4 GiB, offsets
            // wrapped to 32 bits, should they add up so (zar_check)
            ar.wrapped = ar.end.end64<0 && !(extra & ZIP_MAX32);
        }
    }
    if (!cdoff && !ar.end.cdsize) {
        if (u->zinfo) {  // ZipInfo's, on stdout
            info(u, 0, verbose ? S("\n  Empty zipfile.\n")
                               : S("Empty zipfile.\n"));
        } else {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                                     S("]:  zipfile is empty\n")));
        }
        err = MAX(err, PK_WARN);
        goto done;
    }

    // Central headers are read while they parse, as UnZip reads them,
    // whatever the count, which they must then match, as a count without
    // Zip64 matches modulo 65,536. A directory invalid past its first
    // entry, or of more or fewer entries than its count, is processed up
    // to there, as UnZip finds it out once it gets there. Checked here,
    // it is read again as needed, rather than held (zar_reread).
    u->nentries = 0;
    u->cderr    = CD_OK;
    u->oldshift = 0;
    r = r==ZAR_EFORMAT ? ZAR_EFORMAT : zar_check(&ar);
    switch (r) {
    case ZAR_OK:
        u->nentries = ar.nread;
        u->cderr    = ar.endsig ? CD_OK : CD_END;
        break;
    case ZAR_EREAD:
    case ZAR_EEOF:
        info(u, MSG_STDERR, S("error:  zipfile read error\n"));
        err = r==ZAR_EREAD ? PK_BADERR : PK_EOF;
        goto done;
    case ZAR_ENONAME:
    case ZAR_EFORMAT:
        u->nentries = r==ZAR_ENONAME ? ar.noname : ar.bad;
        u->cderr    = r==ZAR_ENONAME ? CD_NONAME : CD_SIG;
        if (u->nentries <= 0) {
            info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
                 S("]:  start of central directory not found;\n"
                   "  zipfile corrupt.\n"), report_msg));
            err = PK_BADERR;
            goto done;
        }
    }
    zar_reread(&ar, UZ_CDWIN, &u->perm);

    if (u->visit) {
        r = visit_files(u, &ar, scratch);
    } else if (u->zinfo) {
        r = zipinfo_files(u, &ar, scratch);
    } else if (u->vflag && !u->tflag && !u->cflag) {
        r = list_files(u, &ar, scratch);
    } else {
        r = extract_or_test(u, &ar, scratch);
    }
    err = MAX(err, r);

done:
    if (fd > 0) {
        os_close(u->ctx, fd);
    }
    return err;
}

static b32 add_match(void *data, s8 path, os_dirent *entry, arena scratch)
{
    (void)entry;
    (void)scratch;
    unzip *u = data;
    *push(&u->perm, &u->matches) = JOIN(&u->perm, path);
    return 1;
}

// Process the archives named, as process.c's process_zipfiles: each
// match of a wildcard, or the name as given, then if that alone was not
// there, with .zip and .ZIP appended, summarizing a wildcard's.
static i32 process_zipfiles(unzip *u, arena scratch)
{
    s8 spec = u->zipspec;
    if (zequals(spec, S("-"))) {
        i32 r = do_archive(u, spec, 1, 1, scratch);
        return r==IZ_DIR ? PK_NOZIP : r;
    }

    // On Windows hidden and system files match too, as the port's
    // Readdir (win32/win32.c) lists them, unlike zip's
    b32 wild = zip_haswild(spec, ZIP_SETS);
    if (wild) {
        i32 flags = ZIP_SETS | (u->windows ? WILD_WINDOWS : WILD_NODOTS);
        expand_wild(u->ctx, spec, 1, flags, add_match, u, scratch);
    }
    if (!u->matches.len) {
        *push(&u->perm, &u->matches) = spec;  // tried as it is
    }

    i32  status = 0;
    i32  r      = 0;
    iz   win = 0, lose = 0, warn = 0, dirs = 0, miss = 0;
    s8   last = {0};
    byte *mark = u->perm.beg;
    for (iz i = 0; i < u->matches.len; i++) {
        last = u->matches.data[i];
        if (!u->qflag && r!=PK_NOZIP && r!=IZ_DIR && win+lose+warn+miss) {
            info(u, 0, S("\n"));
        }
        r = do_archive(u, last, 0, 0, scratch);
        u->perm.beg = mark;  // each archive's memory is reused
        if (r == PK_WARN) {
            warn++;
        } else if (r == IZ_DIR) {
            dirs++;
        } else if (r == PK_NOZIP) {
            miss++;
        } else if (r) {
            lose++;
        } else {
            win++;
        }
        status = r!=IZ_DIR ? MAX(status, r) : status;
    }

    if (!win && !warn && !lose && dirs+miss==1) {
        // Again with the suffixes, the last reporting its absence
        s8 zip = JOIN(&u->perm, last, S(".zip"));
        dirs = miss = 0;
        status = PK_OK;
        r = do_archive(u, zip, 0, 0, scratch);
        if (r==PK_NOZIP || r==IZ_DIR) {
            dirs += r == IZ_DIR;
            zip = JOIN(&u->perm, last, S(".ZIP"));
            r = do_archive(u, zip, 1, 0, scratch);
        }
        switch (r) {
        case PK_WARN:
            warn++;
            break;
        case IZ_DIR:
            dirs++;
            r = PK_NOZIP;
            break;
        case PK_NOZIP:
            break;
        default:
            if (r) {
                lose++;
            } else {
                win++;
            }
        }
        status = MAX(status, r);
    }

    if (wild && u->qflag<3) {
        arena tmp = scratch;
        i32   e   = MSG_STDERR;
        if ((miss+lose+warn>0 || win!=1) && !(u->tflag && u->qflag>1)) {
            info(u, e, S("\n"));
        }
        if (win>1 || (win==1 && dirs+miss+lose+warn>0)) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)win, 0, 0), S(" archive"),
                            win==1 ? S(" was") : S("s were"),
                            S(" successfully processed.\n")));
        }
        if (warn) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)warn, 0, 0), S(" archive"),
                            plural((u64)warn),
                            S(" had warnings but no fatal errors.\n")));
        }
        if (lose) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)lose, 0, 0), S(" archive"),
                            plural((u64)lose), S(" had fatal errors.\n")));
        }
        if (miss) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)miss, 0, 0), S(" file"),
                            plural((u64)miss),
                            S(" had no zipfile directory.\n")));
        }
        if (dirs == 1) {
            info(u, e, S("1 \"zipfile\" was a directory.\n"));
        } else if (dirs) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)dirs, 0, 0),
                            S(" \"zipfiles\" were directories.\n")));
        }
        if (!win && !lose && !warn) {
            info(u, e, S("No zipfiles found.\n"));
        }
    }
    return status;
}

// Ready a run once its options are set: the decoders to test or
// extract, and to extract, how to overwrite, and the -d directory (as
// UnZip's checkdir ROOT, dropping a '/').
static void unzip_start(unzip *u)
{
    if (u->tflag || u->cflag || u->extract) {
        u->inf    = inflate_new(&u->perm);
        u->window = newbytes(&u->perm, UZ_WSIZE);
    }
    if (u->extract) {
        u->overwrite = u->ovnone ? OVERWRT_NEVER :
                       u->ovall  ? OVERWRT_ALWAYS : OVERWRT_QUERY;
        u->umask = os_umask(u->ctx);
        s8 dir = u->exdir;
        dir.len -= dir.len && dir.s[dir.len-1]=='/';
        u->root = u->hasexdir && u->exdir.len ? JOIN(&u->perm, dir, S("/"))
                                              : S("");
    }
}

[[maybe_unused]] static i32 unzip_main(unzipconfig *conf)
{
    unzip *u = new(&conf->perm, 1, unzip);
    u->ctx     = conf->perm.ctx;
    u->perm    = conf->perm;
    u->windows = conf->windows;
    u->sol     = 1;
    u->out     = newwriter(&u->perm, 1, 1<<16);
    arena scratch = conf->scratch;

    // ZipInfo mode by the program's name, or a first argument of -Z, as
    // UnZip decides it before reading either's environment (unzip.c)
    b32 zinfo = zi_named(conf->argv0, u->windows) ||
                (conf->nargs && conf->args[0].len>=2 &&
                 conf->args[0].s[0]=='-' && conf->args[0].s[1]=='Z');
    i32 r = zinfo ? zipinfo_args(u, conf, scratch)
                  : unzip_args(u, conf, scratch);
    if (r < 0) {
        u->dup = tflagged(u) && !os_isatty(u->ctx, 1) && os_isatty(u->ctx, 2);
        // On a terminal, output is flushed once it has waited a moment,
        // at check points between entries and between chunks of their
        // data, lest a slow run show nothing for long, then pages at
        // once, while flushing each line, as UnZip's C library does a
        // terminal's, is slow on a Windows console given many small
        // files. Under -c, names are interleaved with the data, which is
        // shown so too, but without them (-p, or -c quietly) the output
        // is data alone, buffered as to a pipe.
        u->out->tty = os_isatty(u->ctx, 1) && !(u->cflag && u->qflag);
        // An interruption writes the messages held, but not entries' data
        // (-p, -c), which stops where it was written, as gzip -c's does,
        // rather than run on by up to a buffer's worth past it
        if (!u->cflag) {
            os_holdtext(u->ctx, u->out);
        }
        // The prompts' answers, unless standard input is the archive
        if (u->extract && !zequals(u->zipspec, S("-"))) {
            u->answers = newreader(&u->perm, 0, 1<<12);
        }
        unzip_start(u);
        r = process_zipfiles(u, scratch);
    }
    writer_flush(u->out);
    os_holdtext(u->ctx, 0);
    return r;
}

// A caller's interface, for runs other than the command line's, as the
// GUI makes them: an archive listed entry by entry (visit), or its picked
// entries, or all, tested, or extracted below a directory under names of
// the caller's, or one's ZipInfo -v report. No options come from the
// environment, the archive's name is neither expanded nor tried with
// ".zip" added, and the caller answers the questions (ask), if it would.
// Messages are unzip's own, and the status is as unzip_main's.
enum { CALL_LIST, CALL_TEST, CALL_EXTRACT, CALL_ZIPINFO };

typedef struct {
    s8         archive;
    i32        mode;       // CALL_*
    s8         exdir;      // to extract below, made one level if missing
    unzippick *picks;      // sorted by at, else null for every entry
    iz         npicks;
    i32        overwrite;  // OVERWRT_*
    iz       (*ask)(void *data, i32 question, s8 path, u8 *buf, iz cap);
    void     (*visit)(void *data, unzipvisit *);
    void      *data;
} unzipcall;

[[maybe_unused]] static i32 unzip_call(unzipconfig *conf, unzipcall *c)
{
    unzip *u = new(&conf->perm, 1, unzip);
    u->ctx      = conf->perm.ctx;
    u->perm     = conf->perm;
    u->windows  = conf->windows;
    u->sol      = 1;
    u->out      = newwriter(&u->perm, 1, 1<<16);
    u->zipspec  = c->archive;
    u->allfiles = !c->picks;
    u->picks    = c->picks;
    u->npicks   = c->npicks;
    u->ask      = c->ask;
    u->visit    = c->visit;
    u->data     = c->data;
    switch (c->mode) {
    case CALL_LIST:
        u->qflag = 1;
        break;
    case CALL_TEST:
        u->tflag = 1;
        break;
    case CALL_EXTRACT:
        u->extract  = 1;
        u->hasexdir = 1;
        u->exdir    = c->exdir;
        u->ovall    = c->overwrite == OVERWRT_ALWAYS;
        u->ovnone   = c->overwrite == OVERWRT_NEVER;
        break;
    case CALL_ZIPINFO:
        u->zinfo = 1;
        u->zi    = zi_new();
        zi_option(&u->zi, 'v');
        zi_resolve(&u->zi, 1);
        break;
    }
    unzip_start(u);
    i32 r = do_archive(u, c->archive, 1, 0, conf->scratch);
    writer_flush(u->out);
    return r==IZ_DIR ? PK_NOZIP : r;
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

// unzip's Win32 platform functions, beyond those of windows.c and
// zipfs_windows.c: directories, attributes, link files, and times for
// extraction. Included after them by platform/unzip_windows.c and by
// platform/gui_windows.c, which holds both programs.

W32(b32) CreateDirectoryW(c16 *, uptr);
W32(b32) TzSpecificLocalTimeToSystemTime(uptr, systemtime *, systemtime *);

#define FILE_ATTRIBUTE_ARCHIVE      0x20u
#define ERROR_CANT_RESOLVE_FILENAME 1921u

static u32 os_umask(os *ctx)
{
    (void)ctx;
    return 0;  // access control is inherited from the directory
}

// By the time zone's rules for that year, as os_localtime applies them,
// rather than the current year's for every year, as Info-ZIP's port's
// C runtime mktime does (win32.c, dos_to_unix_time in fileio.c). Fields
// out of range carry over first, as through uz_timegm. A local time
// skipped by a change to daylight saving time is taken as the system
// takes it, and one repeated, as the first.
static i64 os_mktime(os *ctx, i32 const tm[6])
{
    (void)ctx;
    i64 t = uz_timegm(tm);  // normalized, as though UTC
    i32 n[6];
    zip_gmtime(t, n);
    if (n[0]<1601 || n[0]>30827) {
        return t;
    }
    systemtime loc = {0};
    loc.year   = (u16)n[0];
    loc.month  = (u16)n[1];
    loc.day    = (u16)n[2];
    loc.hour   = (u16)n[3];
    loc.minute = (u16)n[4];
    loc.second = (u16)n[5];
    systemtime utc = {0};
    if (!TzSpecificLocalTimeToSystemTime(0, &loc, &utc)) {
        return t;
    }
    i32 u[6] = {utc.year, utc.month, utc.day, utc.hour, utc.minute,
                utc.second};
    return uz_timegm(u);
}

static b32 os_mkdir(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return 0;
    }
    return CreateDirectoryW(wpath, 0);
}

// A FILETIME from Unix seconds, as a FILE_BASIC_INFO time, where zero
// means unchanged: times before 1601 become its first tick.
static i64 filetime(i64 t)
{
    i64 lo = -11644473600;
    i64 hi = (0x7fffffffffffffff / 10000000) + lo;
    t = t<lo ? lo : t>hi ? hi : t;
    i64 ft = (t - lo) * 10000000;
    return ft ? ft : 1;
}

// The times and attributes of attrs as one FILE_BASIC_INFO, its zero
// fields unchanged. Times include the creation time with OS_ACTIME, and
// leave the access time unchanged with OS_AKEEPA, as the port's
// SetFileTime gives a UT field's (win32.c, close_outfile and
// set_direc_attribs). Attributes, for a file only, are read-only,
// hidden, system, and archive, as Info-ZIP's port sets them (win32.c,
// close_outfile, through mapattr), without the others of the low byte
// that it passes, which files cannot have (directory, volume label).
static basic_info basic(osattrs *attrs, i32 flags)
{
    basic_info info = {0};
    if (flags & OS_ATIMES) {
        info.written  = filetime(attrs->mtime);
        if (!(attrs->flags & OS_AKEEPA)) {
            info.accessed = filetime(attrs->atime);
        }
        if (attrs->flags & OS_ACTIME) {
            info.created = filetime(attrs->ctime);
        }
    }
    if (flags & OS_AMODE) {
        u32 keep = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                   FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
        info.attributes = attrs->dosattr & keep;
        info.attributes = info.attributes ? info.attributes
                                          : FILE_ATTRIBUTE_NORMAL;
    }
    return info;
}

// The reason for the last failure, copied, for each flag in failed.
static void failure(os *ctx, i32 failed, s8 *why, arena *a)
{
    s8 r = JOIN(a, os_error(ctx));
    for (i32 b = 0; b < OS_AWHY; b++) {
        why[b] = failed & 1<<b ? r : why[b];
    }
}

// Times and attributes in one call, on the handle that wrote the file,
// still delete-pending: Windows takes read-only on such a handle, and
// os_keep may still clear the deletion. The mode is ignored, as the
// owner is (unzip refuses -X here). Should the call fail, times and
// attributes are tried apart, to tell which failed.
static i32 os_setattrs(os *ctx, i32 fd, osattrs *attrs, s8 *why, arena *a)
{
    iptr h     = ctx->handles[fd];
    i32  flags = attrs->flags & (OS_AMODE|OS_ATIMES);
    if (!flags) {
        return 0;
    }
    basic_info info = basic(attrs, flags);
    if (set_info(ctx, h, FileBasicInfo, &info, sizeof(info))) {
        return 0;
    }
    i32 failed = 0;
    for (i32 f = OS_AMODE; f <= OS_ATIMES; f <<= 1) {
        info = basic(attrs, f);
        if ((flags & f) &&
            !set_info(ctx, h, FileBasicInfo, &info, sizeof(info))) {
            failure(ctx, f, why, a);
            failed |= f;
        }
    }
    return failed;
}

// A directory's times, as Info-ZIP's port sets them (win32.c,
// set_direc_attribs), which sets no attributes on directories, through
// a handle to the directory itself, refusing one that is a link,
// junction, or any other reparse point, which is no directory made by
// unzip.
static i32 os_setdirattrs(os *ctx, s8 path, osattrs *attrs, s8 *why,
                          arena *a)
{
    i32 flags = attrs->flags & OS_ATIMES;
    if (!flags) {
        return 0;
    }
    arena tmp   = *a;
    c16  *wpath = winpath(&tmp, path);
    iptr  h     = INVALID_HANDLE_VALUE;
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);
    } else {
        h = CreateFileW(wpath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_ALL, 0,
                        OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE, 0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        failure(ctx, flags, why, a);
        return flags;
    }
    attribute_tag_info tag = {0};
    b32 ok = get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag));
    if (ok && (tag.attributes & FILE_ATTRIBUTE_REPARSE)) {
        ok = 0;
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
    } else if (ok && !(tag.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ok = 0;
        SetLastError(ERROR_PATH_NOT_FOUND);
    }
    if (ok) {
        basic_info info = basic(attrs, flags);
        ok = set_info(ctx, h, FileBasicInfo, &info, sizeof(info));
    }
    if (!ok) {
        failure(ctx, flags, why, a);
    }
    CloseHandle(h);
    return ok ? 0 : flags;
}

// Links need privileges on Windows, which Info-ZIP's port does not
// make: as it extracts them, a link becomes a regular file holding its
// target, every byte of it (empty for an empty target), created where
// nothing is, as a link would be, with a file's attributes and times,
// as the port's close_outfile gives them. It is written to completion
// and given them before it is kept, so that a failure to write leaves
// nothing, while a failure to give them is reported, the file kept.
static i32 os_symlink(os *ctx, s8 target, s8 path, osattrs *attrs, s8 *why,
                      arena *a)
{
    i32 fd = os_open(ctx, path, OS_CREATE, *a);
    b32 ok = fd >= 0;
    ok = ok && os_write(ctx, fd, target.s, target.len);
    i32 failed = ok ? os_setattrs(ctx, fd, attrs, why, a) : 0;
    ok = ok && os_keep(ctx, fd);
    if (fd >= 0) {
        u32 err = GetLastError();
        if (!os_close(ctx, fd) && ok) {
            ok = 0;
            os_remove(ctx, path, *a);
            err = GetLastError();
        }
        SetLastError(err);
    }
    if (!ok) {
        failure(ctx, OS_ALINK, why, a);
        return OS_ALINK;
    }
    return failed;
}

// CRT-free Win32 platform layer for tugz unzip
// For 32-bit (i686), add -Wl,--large-address-aware: 4 GB under WOW64.


void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    reserve(&ctx);
    unzipconfig conf = {0};
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};
    conf.windows = 1;

    // Wildcards in member names are unzip's own, matched against entries,
    // and in the archive's name expanded by unzip (expand_wild)
    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    conf.argv0 = argc>0 ? argv[0] : (s8){0};  // a copy named zipinfo.exe
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    conf.unzipenv   = getenv8(&conf.perm, L"UNZIP");
    conf.unzipopt   = getenv8(&conf.perm, L"UNZIPOPT");
    conf.zipinfoenv = getenv8(&conf.perm, L"ZIPINFO");
    conf.zipinfoopt = getenv8(&conf.perm, L"ZIPINFOOPT");
    os_exit(&ctx, unzip_main(&conf));
}
