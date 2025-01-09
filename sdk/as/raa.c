#include "aslib.h"
#include "raa.h"

#define LEAFSIZ (sizeof(RAA)-sizeof(RAA_UNION)+sizeof(RAA_LEAF))
#define BRANCHSIZ (sizeof(RAA)-sizeof(RAA_UNION)+sizeof(RAA_BRANCH))

#define LAYERSHIFT(r) ( (r)->layers==0 ? RAA_BLKSHIFT : RAA_LAYERSHIFT )

static struct RAA *real_raa_init(int layers) {
    struct RAA *r;
    int i;

    if (layers == 0) {
        r = as_zalloc(LEAFSIZ);
        r->shift = 0;
    } else {
        r = as_malloc(BRANCHSIZ);
        r->layers = layers;
        for (i = 0; i < RAA_LAYERSIZE; i++)
            r->u.b.data[i] = NULL;
        r->shift =
                (RAA_BLKSHIFT - RAA_LAYERSHIFT) + layers * RAA_LAYERSHIFT;
    }
    return r;
}

struct RAA *raa_init(void) {
    return real_raa_init(0);
}

void raa_free(struct RAA *r) {
    if (r->layers) {
        struct RAA **p;
        for (p = r->u.b.data; p - r->u.b.data < RAA_LAYERSIZE; p++)
            if (*p)
                raa_free(*p);
    }
    as_free(r);
}

int64_t raa_read(struct RAA *r, int32_t posn) {
    if ((uint32_t) posn >= (UINT32_C(1) << (r->shift + LAYERSHIFT(r))))
        return 0;               /* Return 0 for undefined entries */
    while (r->layers > 0) {
        int32_t l = posn >> r->shift;
        posn &= (UINT32_C(1) << r->shift) - 1;
        r = r->u.b.data[l];
        if (!r)
            return 0;           /* Return 0 for undefined entries */
    }
    return r->u.l.data[posn];
}

struct RAA *raa_write(struct RAA *r, int32_t posn, int64_t value) {
    struct RAA *result;

    as_assert(posn >= 0);

    while ((UINT32_C(1) << (r->shift + LAYERSHIFT(r))) <= (uint32_t) posn) {
        /*
         * Must add a layer.
         */
        struct RAA *s;
        int i;

        s = as_malloc(BRANCHSIZ);
        for (i = 0; i < RAA_LAYERSIZE; i++)
            s->u.b.data[i] = NULL;
        s->layers = r->layers + 1;
        s->shift = LAYERSHIFT(r) + r->shift;
        s->u.b.data[0] = r;
        r = s;
    }

    result = r;

    while (r->layers > 0) {
        struct RAA **s;
        int32_t l = posn >> r->shift;
        posn &= (UINT32_C(1) << r->shift) - 1;
        s = &r->u.b.data[l];
        if (!*s)
            *s = real_raa_init(r->layers - 1);
        r = *s;
    }

    r->u.l.data[posn] = value;

    return result;
}
