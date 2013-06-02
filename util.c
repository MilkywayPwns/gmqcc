/*
 * Copyright (C) 2012, 2013
 *     Dale Weiler
 *     Wolfgang Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include <stdarg.h>
#include <errno.h>
#include "gmqcc.h"

/* TODO: remove globals ... */
static uint64_t mem_ab = 0;
static uint64_t mem_db = 0;
static uint64_t mem_at = 0;
static uint64_t mem_dt = 0;
static uint64_t mem_pk = 0;
static uint64_t mem_hw = 0;

struct memblock_t {
    const char  *file;
    unsigned int line;
    size_t       byte;
    struct memblock_t *next;
    struct memblock_t *prev;
};

#define PEAK_MEM             \
    do {                     \
        if (mem_hw > mem_pk) \
            mem_pk = mem_hw; \
    } while (0)

static struct memblock_t *mem_start = NULL;

void *util_memory_a(size_t byte, unsigned int line, const char *file) {
    struct memblock_t *info = (struct memblock_t*)malloc(sizeof(struct memblock_t) + byte);
    void              *data = (void*)(info+1);
    if (!info) return NULL;
    info->line = line;
    info->byte = byte;
    info->file = file;
    info->prev = NULL;
    info->next = mem_start;
    if (mem_start)
        mem_start->prev = info;
    mem_start = info;

    mem_at++;
    mem_ab += info->byte;
    mem_hw += info->byte;

    PEAK_MEM;

    return data;
}

void util_memory_d(void *ptrn) {
    struct memblock_t *info = NULL;

    if (!ptrn) return;
    info = ((struct memblock_t*)ptrn - 1);

    mem_db += info->byte;
    mem_hw -= info->byte;
    mem_dt++;

    if (info->prev)
        info->prev->next = info->next;
    if (info->next)
        info->next->prev = info->prev;
    if (info == mem_start)
        mem_start = info->next;

    free(info);
}

void *util_memory_r(void *ptrn, size_t byte, unsigned int line, const char *file) {
    struct memblock_t *oldinfo = NULL;

    struct memblock_t *newinfo;

    if (!ptrn)
        return util_memory_a(byte, line, file);
    if (!byte) {
        util_memory_d(ptrn);
        return NULL;
    }

    oldinfo = ((struct memblock_t*)ptrn - 1);
    newinfo = ((struct memblock_t*)malloc(sizeof(struct memblock_t) + byte));

    /* new data */
    if (!newinfo) {
        util_memory_d(oldinfo+1);
        return NULL;
    }

    /* copy old */
    memcpy(newinfo+1, oldinfo+1, oldinfo->byte);

    /* free old */
    if (oldinfo->prev)
        oldinfo->prev->next = oldinfo->next;
    if (oldinfo->next)
        oldinfo->next->prev = oldinfo->prev;
    if (oldinfo == mem_start)
        mem_start = oldinfo->next;

    /* fill info */
    newinfo->line = line;
    newinfo->byte = byte;
    newinfo->file = file;
    newinfo->prev = NULL;
    newinfo->next = mem_start;
    if (mem_start)
        mem_start->prev = newinfo;
    mem_start = newinfo;

    mem_ab -= oldinfo->byte;
    mem_hw -= oldinfo->byte;
    mem_ab += newinfo->byte;
    mem_hw += newinfo->byte;

    PEAK_MEM;

    free(oldinfo);

    return newinfo+1;
}

static void util_dumpmem(struct memblock_t *memory, uint16_t cols) {
    uint32_t i, j;
    for (i = 0; i < memory->byte + ((memory->byte % cols) ? (cols - memory->byte % cols) : 0); i++) {
        if (i % cols == 0)    con_out("    0x%06X: ", i);
        if (i < memory->byte) con_out("%02X "   , 0xFF & ((char*)(memory + 1))[i]);
        else                  con_out("    ");

        if ((uint16_t)(i % cols) == (cols - 1)) {
            for (j = i - (cols - 1); j <= i; j++) {
                con_out("%c",
                    (j >= memory->byte)
                        ? ' '
                        : (isprint(((char*)(memory + 1))[j]))
                            ? 0xFF & ((char*)(memory + 1)) [j]
                            : '.'
                );
            }
            con_out("\n");
        }
    }
}

/*
 * The following is a VERY tight, efficent, hashtable for integer
 * values and keys, and for nothing more. We could make our existing
 * hashtable support type-genericness through a void * pointer but,
 * ideally that would make things more complicated. We also don't need
 * that much of a bloat for something as basic as this.
 */
typedef struct {
    size_t key;
    size_t value;
} size_entry_t;
#define ST_SIZE 1024

typedef size_entry_t **size_table_t;

size_table_t util_st_new() {
    return (size_table_t)memset(
        mem_a(sizeof(size_entry_t*) * ST_SIZE),
        0, ST_SIZE * sizeof(size_entry_t*)
    );
}
void util_st_del(size_table_t table) {
    size_t i = 0;
    for (; i < ST_SIZE; i++) if(table[i]) mem_d(table[i]);
    mem_d(table);
}
size_entry_t *util_st_get(size_table_t table, size_t key) {
    size_t hash = (key % ST_SIZE);
    while (table[hash] && table[hash]->key != key)
        hash = (hash + 1) % ST_SIZE;
    return table[hash];
}
void util_st_put(size_table_t table, size_t key, size_t value) {
    size_t hash = (key % ST_SIZE);
    while (table[hash] && table[hash]->key != key)
        hash = (hash + 1) % ST_SIZE;
    table[hash]        = (size_entry_t*)mem_a(sizeof(size_entry_t));
    table[hash]->key   = key;
    table[hash]->value = value;
}

static uint64_t      strdups           = 0;
static uint64_t      vectors           = 0;
static uint64_t      vector_sizes      = 0;
static uint64_t      hashtables        = 0;
static uint64_t      hashtable_sizes   = 0;
static size_table_t  vector_usage      = NULL;
static size_table_t  hashtable_usage   = NULL;

void util_meminfo() {
    struct memblock_t *info;

    if (OPTS_OPTION_BOOL(OPTION_DEBUG)) {
        for (info = mem_start; info; info = info->next) {
            con_out("lost: %u (bytes) at %s:%u\n",
                info->byte,
                info->file,
                info->line);

            util_dumpmem(info, OPTS_OPTION_U16(OPTION_MEMDUMPCOLS));
        }
    }

    if (OPTS_OPTION_BOOL(OPTION_DEBUG) ||
        OPTS_OPTION_BOOL(OPTION_MEMCHK)) {
        con_out("Memory information:\n\
            Total allocations:   %llu\n\
            Total deallocations: %llu\n\
            Total allocated:     %f (MB)\n\
            Total deallocated:   %f (MB)\n\
            Total peak memory:   %f (MB)\n\
            Total leaked memory: %f (MB) in %llu allocations\n",
                mem_at,
                mem_dt,
                (float)(mem_ab)           / 1048576.0f,
                (float)(mem_db)           / 1048576.0f,
                (float)(mem_pk)           / 1048576.0f,
                (float)(mem_ab -  mem_db) / 1048576.0f,

                /* could be more clever */
                (mem_at -  mem_dt)
        );
    }
    
    if (OPTS_OPTION_BOOL(OPTION_STATISTICS) ||
        OPTS_OPTION_BOOL(OPTION_MEMCHK)) {
        size_t   i         = 0;
        size_t   e         = 1;
        uint64_t vectormem = 0;
        
        con_out("\nAdditional Statistics:\n\
            Total vectors allocated:      %llu\n\
            Total string duplicates:      %llu\n\
            Total hashtables allocated:   %llu\n\
            Total unique vector sizes:    %llu\n",
            vectors,
            strdups,
            hashtables,
            vector_sizes
        );
        
        for (; i < ST_SIZE; i++) {
            size_entry_t *entry;
            
            if (!(entry = vector_usage[i]))
                continue;
            
            con_out("                %2u| # of %4u byte vectors: %u\n",
                (unsigned)e,
                (unsigned)entry->key,
                (unsigned)entry->value
            );
            e++;
            
            vectormem += entry->key * entry->value;
        }

        con_out("\
            Total unique hashtable sizes: %llu\n",
            hashtable_sizes
        );
        
        for (i = 0, e = 1; i < ST_SIZE; i++) {
            size_entry_t *entry;
            
            if (!(entry = hashtable_usage[i]))
                continue;
                
            con_out("                %2u| # of %4u element hashtables: %u\n",
                (unsigned)e,
                (unsigned)entry->key,
                (unsigned)entry->value
            );
            e++;
        }
        
        con_out("            Total vector memory:          %f (MB)\n",
            (float)(vectormem) / 1048576.0f
        );
    }

    if (vector_usage)
        util_st_del(vector_usage);
    if (hashtable_usage)
        util_st_del(hashtable_usage);
}

/*
 * Some string utility functions, because strdup uses malloc, and we want
 * to track all memory (without replacing malloc).
 */
char *_util_Estrdup(const char *s, const char *file, size_t line) {
    size_t  len = 0;
    char   *ptr = NULL;

    /* in case of -DNOTRACK */
    (void)file;
    (void)line;

    if (!s)
        return NULL;

    if ((len = strlen(s)) && (ptr = (char*)mem_af(len+1, line, file))) {
        memcpy(ptr, s, len);
        ptr[len] = '\0';
    }
    strdups++;
    return ptr;
}

char *_util_Estrdup_empty(const char *s, const char *file, size_t line) {
    size_t  len = 0;
    char   *ptr = NULL;

    /* in case of -DNOTRACK */
    (void)file;
    (void)line;

    if (!s)
        return NULL;

    len = strlen(s);
    if ((ptr = (char*)mem_af(len+1, line, file))) {
        memcpy(ptr, s, len);
        ptr[len] = '\0';
    }
    strdups++;
    return ptr;
}

void util_debug(const char *area, const char *ms, ...) {
    va_list  va;
    if (!OPTS_OPTION_BOOL(OPTION_DEBUG))
        return;

    if (!strcmp(area, "MEM") && !OPTS_OPTION_BOOL(OPTION_MEMCHK))
        return;

    va_start(va, ms);
    con_out ("[%s] ", area);
    con_vout(ms, va);
    va_end  (va);
}

/*
 * only required if big endian .. otherwise no need to swap
 * data.
 */
#if PLATFORM_BYTE_ORDER == GMQCC_BYTE_ORDER_BIG
    static GMQCC_INLINE void util_swap16(uint16_t *d, size_t l) {
        while (l--) {
            d[l] = (d[l] << 8) | (d[l] >> 8);
        }
    }

    static GMQCC_INLINE void util_swap32(uint32_t *d, size_t l) {
        while (l--) {
            uint32_t v;
            v = ((d[l] << 8) & 0xFF00FF00) | ((d[l] >> 8) & 0x00FF00FF);
            d[l] = (v << 16) | (v >> 16);
        }
    }

    /* Some strange system doesn't like constants that big, AND doesn't recognize an ULL suffix
     * so let's go the safe way
     */
    static GMQCC_INLINE void util_swap64(uint32_t *d, size_t l) {
        /*
        while (l--) {
            uint64_t v;
            v = ((d[l] << 8) & 0xFF00FF00FF00FF00) | ((d[l] >> 8) & 0x00FF00FF00FF00FF);
            v = ((v << 16) & 0xFFFF0000FFFF0000) | ((v >> 16) & 0x0000FFFF0000FFFF);
            d[l] = (v << 32) | (v >> 32);
        }
        */
        size_t i;
        for (i = 0; i < l; i += 2) {
            uint32_t v1 = d[i];
            d[i] = d[i+1];
            d[i+1] = v1;
            util_swap32(d+i, 2);
        }
    }
#endif

void util_endianswap(void *_data, size_t length, unsigned int typesize) {
#   if PLATFORM_BYTE_ORDER == -1 /* runtime check */
    if (*((char*)&typesize))
        return;
#else
    /* prevent unused warnings */
    (void) _data;
    (void) length;
    (void) typesize;

#   if PLATFORM_BYTE_ORDER == GMQCC_BYTE_ORDER_LITTLE
        return;
#   else
        switch (typesize) {
            case 1: return;
            case 2:
                util_swap16((uint16_t*)_data, length>>1);
                return;
            case 4:
                util_swap32((uint32_t*)_data, length>>2);
                return;
            case 8:
                util_swap64((uint32_t*)_data, length>>3);
                return;

            default: exit(EXIT_FAILURE); /* please blow the fuck up! */
        }
#   endif
#endif
}

/*
 * CRC algorithms vary in the width of the polynomial, the value of said polynomial,
 * the initial value used for the register, weather the bits of each byte are reflected
 * before being processed, weather the algorithm itself feeds input bytes through the
 * register or XORs them with a byte from one end and then straight into the table, as
 * well as (but not limited to the idea of reflected versions) where the final register
 * value becomes reversed, and finally weather the value itself is used to XOR the final
 * register value.  AS such you can already imagine how painfully annoying CRCs are,
 * of course we stand to target Quake, which expects it's certian set of rules for proper
 * calculation of a CRC.
 *
 * In most traditional CRC algorithms on uses a reflected table driven method where a value
 * or register is reflected if it's bits are swapped around it's center.  For example:
 * take the bits 0101 is the 4-bit reflection of 1010, and respectfully 0011 would be the
 * reflection of 1100. Quake however expects a NON-Reflected CRC on the output, but still
 * requires a final XOR on the values (0xFFFF and 0x0000) this is a standard CCITT CRC-16
 * which I respectfully as a programmer don't agree with.
 *
 * So now you know what we target, and why we target it, despite how unsettling it may seem
 * but those are what Quake seems to request.
 */

static const uint16_t util_crc16_table[] = {
    0x0000,     0x1021,     0x2042,     0x3063,     0x4084,     0x50A5,
    0x60C6,     0x70E7,     0x8108,     0x9129,     0xA14A,     0xB16B,
    0xC18C,     0xD1AD,     0xE1CE,     0xF1EF,     0x1231,     0x0210,
    0x3273,     0x2252,     0x52B5,     0x4294,     0x72F7,     0x62D6,
    0x9339,     0x8318,     0xB37B,     0xA35A,     0xD3BD,     0xC39C,
    0xF3FF,     0xE3DE,     0x2462,     0x3443,     0x0420,     0x1401,
    0x64E6,     0x74C7,     0x44A4,     0x5485,     0xA56A,     0xB54B,
    0x8528,     0x9509,     0xE5EE,     0xF5CF,     0xC5AC,     0xD58D,
    0x3653,     0x2672,     0x1611,     0x0630,     0x76D7,     0x66F6,
    0x5695,     0x46B4,     0xB75B,     0xA77A,     0x9719,     0x8738,
    0xF7DF,     0xE7FE,     0xD79D,     0xC7BC,     0x48C4,     0x58E5,
    0x6886,     0x78A7,     0x0840,     0x1861,     0x2802,     0x3823,
    0xC9CC,     0xD9ED,     0xE98E,     0xF9AF,     0x8948,     0x9969,
    0xA90A,     0xB92B,     0x5AF5,     0x4AD4,     0x7AB7,     0x6A96,
    0x1A71,     0x0A50,     0x3A33,     0x2A12,     0xDBFD,     0xCBDC,
    0xFBBF,     0xEB9E,     0x9B79,     0x8B58,     0xBB3B,     0xAB1A,
    0x6CA6,     0x7C87,     0x4CE4,     0x5CC5,     0x2C22,     0x3C03,
    0x0C60,     0x1C41,     0xEDAE,     0xFD8F,     0xCDEC,     0xDDCD,
    0xAD2A,     0xBD0B,     0x8D68,     0x9D49,     0x7E97,     0x6EB6,
    0x5ED5,     0x4EF4,     0x3E13,     0x2E32,     0x1E51,     0x0E70,
    0xFF9F,     0xEFBE,     0xDFDD,     0xCFFC,     0xBF1B,     0xAF3A,
    0x9F59,     0x8F78,     0x9188,     0x81A9,     0xB1CA,     0xA1EB,
    0xD10C,     0xC12D,     0xF14E,     0xE16F,     0x1080,     0x00A1,
    0x30C2,     0x20E3,     0x5004,     0x4025,     0x7046,     0x6067,
    0x83B9,     0x9398,     0xA3FB,     0xB3DA,     0xC33D,     0xD31C,
    0xE37F,     0xF35E,     0x02B1,     0x1290,     0x22F3,     0x32D2,
    0x4235,     0x5214,     0x6277,     0x7256,     0xB5EA,     0xA5CB,
    0x95A8,     0x8589,     0xF56E,     0xE54F,     0xD52C,     0xC50D,
    0x34E2,     0x24C3,     0x14A0,     0x0481,     0x7466,     0x6447,
    0x5424,     0x4405,     0xA7DB,     0xB7FA,     0x8799,     0x97B8,
    0xE75F,     0xF77E,     0xC71D,     0xD73C,     0x26D3,     0x36F2,
    0x0691,     0x16B0,     0x6657,     0x7676,     0x4615,     0x5634,
    0xD94C,     0xC96D,     0xF90E,     0xE92F,     0x99C8,     0x89E9,
    0xB98A,     0xA9AB,     0x5844,     0x4865,     0x7806,     0x6827,
    0x18C0,     0x08E1,     0x3882,     0x28A3,     0xCB7D,     0xDB5C,
    0xEB3F,     0xFB1E,     0x8BF9,     0x9BD8,     0xABBB,     0xBB9A,
    0x4A75,     0x5A54,     0x6A37,     0x7A16,     0x0AF1,     0x1AD0,
    0x2AB3,     0x3A92,     0xFD2E,     0xED0F,     0xDD6C,     0xCD4D,
    0xBDAA,     0xAD8B,     0x9DE8,     0x8DC9,     0x7C26,     0x6C07,
    0x5C64,     0x4C45,     0x3CA2,     0x2C83,     0x1CE0,     0x0CC1,
    0xEF1F,     0xFF3E,     0xCF5D,     0xDF7C,     0xAF9B,     0xBFBA,
    0x8FD9,     0x9FF8,     0x6E17,     0x7E36,     0x4E55,     0x5E74,
    0x2E93,     0x3EB2,     0x0ED1,     0x1EF0
};

/* Non - Reflected */
uint16_t util_crc16(uint16_t current, const char *k, size_t len) {
    register uint16_t h = current;
    for (; len; --len, ++k)
        h = util_crc16_table[(h>>8)^((unsigned char)*k)]^(h<<8);
    return h;
}
/* Reflective Varation (for reference) */
#if 0
uint16_t util_crc16(const char *k, int len, const short clamp) {
    register uint16_t h= (uint16_t)0xFFFFFFFF;
    for (; len; --len, ++k)
        h = util_crc16_table[(h^((unsigned char)*k))&0xFF]^(h>>8);
    return (~h)%clamp;
}
#endif

size_t util_strtocmd(const char *in, char *out, size_t outsz) {
    size_t sz = 1;
    for (; *in && sz < outsz; ++in, ++out, ++sz)
        *out = (*in == '-') ? '_' : (isalpha(*in) && !isupper(*in)) ? *in + 'A' - 'a': *in;
    *out = 0;
    return sz-1;
}

size_t util_strtononcmd(const char *in, char *out, size_t outsz) {
    size_t sz = 1;
    for (; *in && sz < outsz; ++in, ++out, ++sz)
        *out = (*in == '_') ? '-' : (isalpha(*in) && isupper(*in)) ? *in + 'a' - 'A' : *in;
    *out = 0;
    return sz-1;
}

/* TODO: rewrite ... when I redo the ve cleanup */
void _util_vec_grow(void **a, size_t i, size_t s) {
    vector_t     *d = vec_meta(*a);
    size_t        m = 0;
    size_entry_t *e = NULL;
    void         *p = NULL;
    
    if (*a) {
        m = 2 * d->allocated + i;
        p = mem_r(d, s * m + sizeof(vector_t));
    } else {
        m = i + 1;
        p = mem_a(s * m + sizeof(vector_t));
        ((vector_t*)p)->used = 0;
        vectors++;
    }
    
    if (!vector_usage)
        vector_usage = util_st_new();

    if ((e = util_st_get(vector_usage, s))) {
        e->value ++;
    } else {
        util_st_put(vector_usage, s, 1); /* start off with 1 */
        vector_sizes++;
    }

    *a = (vector_t*)p + 1;
    vec_meta(*a)->allocated = m;
}

/*
 * Hash table for generic data, based on dynamic memory allocations
 * all around.  This is the internal interface, please look for
 * EXPOSED INTERFACE comment below
 */
typedef struct hash_node_t {
    char               *key;   /* the key for this node in table */
    void               *value; /* pointer to the data as void*   */
    struct hash_node_t *next;  /* next node (linked list)        */
} hash_node_t;

GMQCC_INLINE size_t util_hthash(hash_table_t *ht, const char *key) {
    const uint32_t       mix   = 0x5BD1E995;
    const uint32_t       rot   = 24;
    size_t               size  = strlen(key);
    uint32_t             hash  = 0x1EF0 /* LICRC TAB */  ^ size;
    uint32_t             alias = 0;
    const unsigned char *data  = (const unsigned char*)key;

    while (size >= 4) {
        alias  = (data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
        alias *= mix;
        alias ^= alias >> rot;
        alias *= mix;

        hash  *= mix;
        hash  ^= alias;

        data += 4;
        size -= 4;
    }

    switch (size) {
        case 3: hash ^= data[2] << 16;
        case 2: hash ^= data[1] << 8;
        case 1: hash ^= data[0];
                hash *= mix;
    }

    hash ^= hash >> 13;
    hash *= mix;
    hash ^= hash >> 15;

    return (size_t) (hash % ht->size);
}

static hash_node_t *_util_htnewpair(const char *key, void *value) {
    hash_node_t *node;
    if (!(node = (hash_node_t*)mem_a(sizeof(hash_node_t))))
        return NULL;

    if (!(node->key = util_strdupe(key))) {
        mem_d(node);
        return NULL;
    }

    node->value = value;
    node->next  = NULL;

    return node;
}

/*
 * EXPOSED INTERFACE for the hashtable implementation
 * util_htnew(size)                             -- to make a new hashtable
 * util_htset(table, key, value, sizeof(value)) -- to set something in the table
 * util_htget(table, key)                       -- to get something from the table
 * util_htdel(table)                            -- to delete the table
 */
hash_table_t *util_htnew(size_t size) {
    hash_table_t *hashtable = NULL;
    size_entry_t *find;
    
    if (size < 1)
        return NULL;
        
    if (!hashtable_usage)
        hashtable_usage = util_st_new();

    if (!(hashtable = (hash_table_t*)mem_a(sizeof(hash_table_t))))
        return NULL;

    if (!(hashtable->table = (hash_node_t**)mem_a(sizeof(hash_node_t*) * size))) {
        mem_d(hashtable);
        return NULL;
    }
    
    if ((find = util_st_get(hashtable_usage, size)))
        find->value++;
    else {
        hashtable_sizes++;
        util_st_put(hashtable_usage, size, 1);
    }

    hashtable->size = size;
    memset(hashtable->table, 0, sizeof(hash_node_t*) * size);

    hashtables++;
    return hashtable;
}

void util_htseth(hash_table_t *ht, const char *key, size_t bin, void *value) {
    hash_node_t *newnode = NULL;
    hash_node_t *next    = NULL;
    hash_node_t *last    = NULL;

    next = ht->table[bin];

    while (next && next->key && strcmp(key, next->key) > 0)
        last = next, next = next->next;

    /* already in table, do a replace */
    if (next && next->key && strcmp(key, next->key) == 0) {
        next->value = value;
    } else {
        /* not found, grow a pair man :P */
        newnode = _util_htnewpair(key, value);
        if (next == ht->table[bin]) {
            newnode->next  = next;
            ht->table[bin] = newnode;
        } else if (!next) {
            last->next = newnode;
        } else {
            newnode->next = next;
            last->next = newnode;
        }
    }
}

void util_htset(hash_table_t *ht, const char *key, void *value) {
    util_htseth(ht, key, util_hthash(ht, key), value);
}

void *util_htgeth(hash_table_t *ht, const char *key, size_t bin) {
    hash_node_t *pair = ht->table[bin];

    while (pair && pair->key && strcmp(key, pair->key) > 0)
        pair = pair->next;

    if (!pair || !pair->key || strcmp(key, pair->key) != 0)
        return NULL;

    return pair->value;
}

void *util_htget(hash_table_t *ht, const char *key) {
    return util_htgeth(ht, key, util_hthash(ht, key));
}

void *code_util_str_htgeth(hash_table_t *ht, const char *key, size_t bin) {
    hash_node_t *pair;
    size_t len, keylen;
    int cmp;

    keylen = strlen(key);

    pair = ht->table[bin];
    while (pair && pair->key) {
        len = strlen(pair->key);
        if (len < keylen) {
            pair = pair->next;
            continue;
        }
        if (keylen == len) {
            cmp = strcmp(key, pair->key);
            if (cmp == 0)
                return pair->value;
            if (cmp < 0)
                return NULL;
            pair = pair->next;
            continue;
        }
        cmp = strcmp(key, pair->key + len - keylen);
        if (cmp == 0) {
            uintptr_t up = (uintptr_t)pair->value;
            up += len - keylen;
            return (void*)up;
        }
        pair = pair->next;
    }
    return NULL;
}

/*
 * Free all allocated data in a hashtable, this is quite the amount
 * of work.
 */
void util_htrem(hash_table_t *ht, void (*callback)(void *data)) {
    size_t i = 0;
    for (; i < ht->size; i++) {
        hash_node_t *n = ht->table[i];
        hash_node_t *p;

        /* free in list */
        while (n) {
            if (n->key)
                mem_d(n->key);
            if (callback)
                callback(n->value);
            p = n;
            n = n->next;
            mem_d(p);
        }

    }
    /* free table */
    mem_d(ht->table);
    mem_d(ht);
}

void util_htrmh(hash_table_t *ht, const char *key, size_t bin, void (*cb)(void*)) {
    hash_node_t **pair = &ht->table[bin];
    hash_node_t *tmp;

    while (*pair && (*pair)->key && strcmp(key, (*pair)->key) > 0)
        pair = &(*pair)->next;

    tmp = *pair;
    if (!tmp || !tmp->key || strcmp(key, tmp->key) != 0)
        return;

    if (cb)
        (*cb)(tmp->value);

    *pair = tmp->next;
    mem_d(tmp->key);
    mem_d(tmp);
}

void util_htrm(hash_table_t *ht, const char *key, void (*cb)(void*)) {
    util_htrmh(ht, key, util_hthash(ht, key), cb);
}

void util_htdel(hash_table_t *ht) {
    util_htrem(ht, NULL);
}

/*
 * Portable implementation of vasprintf/asprintf. Assumes vsnprintf
 * exists, otherwise compiler error.
 *
 * TODO: fix for MSVC ....
 */
int util_vasprintf(char **dat, const char *fmt, va_list args) {
    int   ret;
    int   len;
    char *tmp = NULL;

    /*
     * For visuals tido _vsnprintf doesn't tell you the length of a
     * formatted string if it overflows. However there is a MSVC
     * intrinsic (which is documented wrong) called _vcsprintf which
     * will return the required amount to allocate.
     */
    #ifdef _MSC_VER
        if ((len = _vscprintf(fmt, args)) < 0) {
            *dat = NULL;
            return -1;
        }

        tmp = (char*)mem_a(len + 1);
        if ((ret = _vsnprintf_s(tmp, len+1, len+1, fmt, args)) != len) {
            mem_d(tmp);
            *dat = NULL;
            return -1;
        }
        *dat = tmp;
        return len;
    #else
        /*
         * For everything else we have a decent conformint vsnprintf that
         * returns the number of bytes needed.  We give it a try though on
         * a short buffer, since efficently speaking, it could be nice to
         * above a second vsnprintf call.
         */
        char    buf[128];
        va_list cpy;
        va_copy(cpy, args);
        len = vsnprintf(buf, sizeof(buf), fmt, cpy);
        va_end (cpy);

        if (len < (int)sizeof(buf)) {
            *dat = util_strdup(buf);
            return len;
        }

        /* not large enough ... */
        tmp = (char*)mem_a(len + 1);
        if ((ret = vsnprintf(tmp, len + 1, fmt, args)) != len) {
            mem_d(tmp);
            *dat = NULL;
            return -1;
        }

        *dat = tmp;
        return len;
    #endif
}
int util_asprintf(char **ret, const char *fmt, ...) {
    va_list  args;
    int      read;
    va_start(args, fmt);
    read = util_vasprintf(ret, fmt, args);
    va_end  (args);

    return read;
}

/*
 * These are various re-implementations (wrapping the real ones) of
 * string functions that MSVC consideres unsafe. We wrap these up and
 * use the safe varations on MSVC.
 */
#ifdef _MSC_VER
    static char **util_strerror_allocated() {
        static char **data = NULL;
        return data;
    }

    static void util_strerror_cleanup(void) {
        size_t i;
        char  **data = util_strerror_allocated();
        for (i = 0; i < vec_size(data); i++)
            mem_d(data[i]);
        vec_free(data);
    }

    const char *util_strerror(int num) {
        char         *allocated = NULL;
        static bool   install   = false;
        static size_t tries     = 0;
        char        **vector    = util_strerror_allocated();

        /* try installing cleanup handler */
        while (!install) {
            if (tries == 32)
                return "(unknown)";

            install = !atexit(&util_strerror_cleanup);
            tries ++;
        }

        allocated = (char*)mem_a(4096); /* A page must be enough */
        strerror_s(allocated, 4096, num);
    
        vec_push(vector, allocated);
        return (const char *)allocated;
    }

    int util_snprintf(char *src, size_t bytes, const char *format, ...) {
        int      rt;
        va_list  va;
        va_start(va, format);

        rt = vsprintf_s(src, bytes, format, va);
        va_end  (va);

        return rt;
    }

    char *util_strcat(char *dest, const char *src) {
        strcat_s(dest, strlen(src), src);
        return dest;
    }

    char *util_strncpy(char *dest, const char *src, size_t num) {
        strncpy_s(dest, num, src, num);
        return dest;
    }
#else
    const char *util_strerror(int num) {
        return strerror(num);
    }

    int util_snprintf(char *src, size_t bytes, const char *format, ...) {
        int      rt;
        va_list  va;
        va_start(va, format);
        rt = vsnprintf(src, bytes, format, va);
        va_end  (va);

        return rt;
    }

    char *util_strcat(char *dest, const char *src) {
        return strcat(dest, src);
    }

    char *util_strncpy(char *dest, const char *src, size_t num) {
        return strncpy(dest, src, num);
    }

#endif /*! _MSC_VER */

/*
 * Implementation of the Mersenne twister PRNG (pseudo random numer
 * generator).  Implementation of MT19937.  Has a period of 2^19937-1
 * which is a Mersenne Prime (hence the name).
 *
 * Implemented from specification and original paper:
 * http://www.math.sci.hiroshima-u.ac.jp/~m-mat/MT/ARTICLES/mt.pdf
 *
 * This code is placed in the public domain by me personally
 * (Dale Weiler, a.k.a graphitemaster).
 */

#define MT_SIZE    624
#define MT_PERIOD  397
#define MT_SPACE   (MT_SIZE - MT_PERIOD)

static uint32_t mt_state[MT_SIZE];
static size_t   mt_index = 0;

static GMQCC_INLINE void mt_generate() {
    /*
     * The loop has been unrolled here: the original paper and implemenation
     * Called for the following code:
     * for (register unsigned i = 0; i < MT_SIZE; ++i) {
     *     register uint32_t load;
     *     load  = (0x80000000 & mt_state[i])                 // most  significant 32nd bit
     *     load |= (0x7FFFFFFF & mt_state[(i + 1) % MT_SIZE]) // least significant 31nd bit
     *
     *     mt_state[i] = mt_state[(i + MT_PERIOD) % MT_SIZE] ^ (load >> 1);
     *
     *     if (load & 1) mt_state[i] ^= 0x9908B0DF;
     * }
     *
     * This essentially is a waste: we have two modulus operations, and
     * a branch that is executed every iteration from [0, MT_SIZE).
     *
     * Please see: http://www.quadibloc.com/crypto/co4814.htm for more
     * information on how this clever trick works.
     */
    static const uint32_t matrix[2] = {
        0x00000000,
        0x9908B0Df
    };
    /*
     * This register gives up a little more speed by instructing the compiler
     * to force these into CPU registers (they're counters for indexing mt_state
     * which we can force the compiler to generate prefetch instructions for)
     */
    register uint32_t y;
    register uint32_t i;

    /*
     * Said loop has been unrolled for MT_SPACE (226 iterations), opposed
     * to [0, MT_SIZE)  (634 iterations).
     */
    for (i = 0; i < MT_SPACE; ++i) {
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i + MT_PERIOD] ^ (y >> 1) ^ matrix[y & 1];

        i ++; /* loop unroll */

        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i + MT_PERIOD] ^ (y >> 1) ^ matrix[y & 1];
    }

    /*
     * collapsing the walls unrolled (evenly dividing 396 [632-227 = 396
     * = 2*2*3*3*11])
     */
    i = MT_SPACE;
    while (i < MT_SIZE - 1) {
        /*
         * We expand this 11 times .. manually, no macros are required
         * here. This all fits in the CPU cache.
         */
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
        y           = (0x80000000 & mt_state[i]) | (0x7FFFFFFF & mt_state[i + 1]);
        mt_state[i] = mt_state[i - MT_SPACE] ^ (y >> 1) ^ matrix[y & 1];
        ++i;
    }

    /* i = mt_state[623] */
    y                     = (0x80000000 & mt_state[MT_SIZE - 1]) | (0x7FFFFFFF & mt_state[MT_SIZE - 1]);
    mt_state[MT_SIZE - 1] = mt_state[MT_PERIOD - 1] ^ (y >> 1) ^ matrix[y & 1];
}

void util_seed(uint32_t value) {
    /*
     * We seed the mt_state with a LCG (linear congruential generator)
     * We're operating exactly on exactly m=32, so there is no need to
     * use modulus.
     *
     * The multipler of choice is 0x6C07865, also knows as the Borosh-
     * Niederreiter multipler used for modulus 2^32.  More can be read
     * about this in Knuth's TAOCP Volume 2, page 106.
     *
     * If you don't own TAOCP something is wrong with you :-) .. so I
     * also provided a link to the original paper by Borosh and
     * Niederreiter.  It's called "Optional Multipliers for PRNG by The
     * Linear Congruential Method" (1983).
     * http://en.wikipedia.org/wiki/Linear_congruential_generator
     *
     * From said page, it says the following:
     * "A common Mersenne twister implementation, interestingly enough
     *  used an LCG to generate seed data."
     *
     * Remarks:
     * The data we're operating on is 32-bits for the mt_state array, so
     * there is no masking required with 0xFFFFFFFF
     */
    register size_t i;

    mt_state[0] = value;
    for (i = 1; i < MT_SIZE; ++i)
        mt_state[i] = 0x6C078965 * (mt_state[i - 1] ^ mt_state[i - 1] >> 30) + i;
}

uint32_t util_rand() {
    register uint32_t y;

    /*
     * This is inlined with any sane compiler (I checked)
     * for some reason though, SubC seems to be generating invalid
     * code when it inlines this.
     */
    if (!mt_index)
        mt_generate();

    y = mt_state[mt_index];

    /* Standard tempering */
    y ^= y >> 11;              /* +7 */
    y ^= y << 7  & 0x9D2C5680; /* +4 */
    y ^= y << 15 & 0xEFC60000; /* -4 */
    y ^= y >> 18;              /* -7 */

    if(++mt_index == MT_SIZE)
         mt_index = 0;

    return y;
}
