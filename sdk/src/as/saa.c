#include "compiler.h"
#include "aslib.h"
#include "saa.h"

/* Aggregate SAA components smaller than this */
#define SAA_BLKSHIFT    16
#define SAA_BLKLEN    ((size_t)1 << SAA_BLKSHIFT)

struct SAA *saa_init(size_t elem_len) {
    struct SAA *s;
    char *data;

    s = as_zalloc(sizeof(struct SAA));

    if (elem_len >= SAA_BLKLEN)
        s->blk_len = elem_len;
    else
        s->blk_len = SAA_BLKLEN - (SAA_BLKLEN % elem_len);

    s->elem_len = elem_len;
    s->length = s->blk_len;
    data = as_malloc(s->blk_len);
    s->nblkptrs = s->nblks = 1;
    s->blk_ptrs = as_malloc(sizeof(char *));
    s->blk_ptrs[0] = data;
    s->wblk = s->rblk = &s->blk_ptrs[0];

    return s;
}

void saa_free(struct SAA *s) {
    char **p;
    size_t n;

    for (p = s->blk_ptrs, n = s->nblks; n; p++, n--)
        as_free(*p);

    as_free(s->blk_ptrs);
    as_free(s);
}

/* Add one allocation block to an SAA */
static void saa_extend(struct SAA *s) {
    size_t blkn = s->nblks++;

    if (blkn >= s->nblkptrs) {
        size_t rindex = s->rblk - s->blk_ptrs;
        size_t windex = s->wblk - s->blk_ptrs;

        s->nblkptrs <<= 1;
        s->blk_ptrs =
                as_realloc(s->blk_ptrs, s->nblkptrs * sizeof(char *));

        s->rblk = s->blk_ptrs + rindex;
        s->wblk = s->blk_ptrs + windex;
    }

    s->blk_ptrs[blkn] = as_malloc(s->blk_len);
    s->length += s->blk_len;
}

void *saa_wstruct(struct SAA *s) {
    void *p;

    as_assert((s->wpos % s->elem_len) == 0);

    if (s->wpos + s->elem_len > s->blk_len) {
        as_assert(s->wpos == s->blk_len);
        if (s->wptr + s->elem_len > s->length)
            saa_extend(s);
        s->wblk++;
        s->wpos = 0;
    }

    p = *s->wblk + s->wpos;
    s->wpos += s->elem_len;
    s->wptr += s->elem_len;

    if (s->wptr > s->datalen)
        s->datalen = s->wptr;

    return p;
}

void saa_wbytes(struct SAA *s, const void *data, size_t len) {
    const char *d = data;

    while (len) {
        size_t l = s->blk_len - s->wpos;
        if (l > len)
            l = len;
        if (l) {
            if (d) {
                memcpy(*s->wblk + s->wpos, d, l);
                d += l;
            } else
                memset(*s->wblk + s->wpos, 0, l);
            s->wpos += l;
            s->wptr += l;
            len -= l;

            if (s->datalen < s->wptr)
                s->datalen = s->wptr;
        }
        if (len) {
            if (s->wptr >= s->length)
                saa_extend(s);
            s->wblk++;
            s->wpos = 0;
        }
    }
}

void saa_rewind(struct SAA *s) {
    s->rblk = s->blk_ptrs;
    s->rpos = s->rptr = 0;
}

void *saa_rstruct(struct SAA *s) {
    void *p;

    if (s->rptr + s->elem_len > s->datalen)
        return NULL;

    as_assert((s->rpos % s->elem_len) == 0);

    if (s->rpos + s->elem_len > s->blk_len) {
        s->rblk++;
        s->rpos = 0;
    }

    p = *s->rblk + s->rpos;
    s->rpos += s->elem_len;
    s->rptr += s->elem_len;

    return p;
}

const void *saa_rbytes(struct SAA *s, size_t *lenp) {
    const void *p;
    size_t len;

    if (s->rptr >= s->datalen) {
        *lenp = 0;
        return NULL;
    }

    if (s->rpos >= s->blk_len) {
        s->rblk++;
        s->rpos = 0;
    }

    len = *lenp;
    if (len > s->datalen - s->rptr)
        len = s->datalen - s->rptr;
    if (len > s->blk_len - s->rpos)
        len = s->blk_len - s->rpos;

    *lenp = len;
    p = *s->rblk + s->rpos;

    s->rpos += len;
    s->rptr += len;

    return p;
}

void saa_rnbytes(struct SAA *s, void *data, size_t len) {
    char *d = data;

    as_assert(s->rptr + len <= s->datalen);

    while (len) {
        size_t l;
        const void *p;

        l = len;
        p = saa_rbytes(s, &l);

        memcpy(d, p, l);
        d += l;
        len -= l;
    }
}

/* Same as saa_rnbytes, except position the counter first */
void saa_fread(struct SAA *s, size_t posn, void *data, size_t len) {
    size_t ix;

    as_assert(posn + len <= s->datalen);

    if (likely(s->blk_len == SAA_BLKLEN)) {
        ix = posn >> SAA_BLKSHIFT;
        s->rpos = posn & (SAA_BLKLEN - 1);
    } else {
        ix = posn / s->blk_len;
        s->rpos = posn % s->blk_len;
    }
    s->rptr = posn;
    s->rblk = &s->blk_ptrs[ix];

    saa_rnbytes(s, data, len);
}

/* Same as saa_wbytes, except position the counter first */
void saa_fwrite(struct SAA *s, size_t posn, const void *data, size_t len) {
    size_t ix;

    /* Seek beyond the end of the existing array not supported */
    as_assert(posn <= s->datalen);

    if (likely(s->blk_len == SAA_BLKLEN)) {
        ix = posn >> SAA_BLKSHIFT;
        s->wpos = posn & (SAA_BLKLEN - 1);
    } else {
        ix = posn / s->blk_len;
        s->wpos = posn % s->blk_len;
    }
    s->wptr = posn;
    s->wblk = &s->blk_ptrs[ix];

    if (!s->wpos) {
        s->wpos = s->blk_len;
        s->wblk--;
    }

    saa_wbytes(s, data, len);
}

void saa_fpwrite(struct SAA *s, FILE *fp) {
    const char *data;
    size_t len;

    saa_rewind(s);
    while (len = s->datalen, (data = saa_rbytes(s, &len)) != NULL)
        fwrite(data, 1, len, fp);
}

void saa_write8(struct SAA *s, uint8_t v) {
    saa_wbytes(s, &v, 1);
}

#ifdef WORDS_LITTEENDIAN

void saa_write16(struct SAA *s, uint16_t v)
{
    saa_wbytes(s, &v, 2);
}

void saa_write32(struct SAA *s, uint32_t v)
{
    saa_wbytes(s, &v, 4);
}

void saa_write64(struct SAA *s, uint64_t v)
{
    saa_wbytes(s, &v, 8);
}

void saa_writeaddr(struct SAA *s, uint64_t v, size_t len)
{
    saa_wbytes(s, &v, len);
}

#else                           /* not WORDS_LITTLEENDIAN */

void saa_write16(struct SAA *s, uint16_t v) {
    uint8_t b[2];

    b[0] = v;
    b[1] = v >> 8;
    saa_wbytes(s, b, 2);
}

void saa_write32(struct SAA *s, uint32_t v) {
    uint8_t b[4];

    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
    saa_wbytes(s, b, 4);
}

void saa_write64(struct SAA *s, uint64_t v) {
    uint8_t b[8];

    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
    b[4] = v >> 32;
    b[5] = v >> 40;
    b[6] = v >> 48;
    b[7] = v >> 56;
    saa_wbytes(s, b, 8);
}

void saa_writeaddr(struct SAA *s, uint64_t v, size_t len) {
    uint8_t b[8];

    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
    b[4] = v >> 32;
    b[5] = v >> 40;
    b[6] = v >> 48;
    b[7] = v >> 56;

    saa_wbytes(s, &v, len);
}

#endif                          /* WORDS_LITTEENDIAN */

/* write unsigned LEB128 value to SAA */
void saa_wleb128u(struct SAA *psaa, int value) {
    char temp[64], *ptemp;
    uint8_t byte;
    int len;

    ptemp = temp;
    len = 0;
    do {
        byte = value & 127;
        value >>= 7;
        if (value != 0)         /* more bytes to come */
            byte |= 0x80;
        *ptemp = byte;
        ptemp++;
        len++;
    } while (value != 0);
    saa_wbytes(psaa, temp, len);
}

/* write signed LEB128 value to SAA */
void saa_wleb128s(struct SAA *psaa, int value) {
    char temp[64], *ptemp;
    uint8_t byte;
    bool more, negative;
    int size, len;

    ptemp = temp;
    more = 1;
    negative = (value < 0);
    size = sizeof(int) * 8;
    len = 0;
    while (more) {
        byte = value & 0x7f;
        value >>= 7;
        if (negative)
            /* sign extend */
            value |= -(1 << (size - 7));
        /* sign bit of byte is second high order bit (0x40) */
        if ((value == 0 && !(byte & 0x40)) ||
            ((value == -1) && (byte & 0x40)))
            more = 0;
        else
            byte |= 0x80;
        *ptemp = byte;
        ptemp++;
        len++;
    }
    saa_wbytes(psaa, temp, len);
}
