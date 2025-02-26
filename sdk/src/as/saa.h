#ifndef AS_SAA_H
#define AS_SAA_H

#include "compiler.h"

/*
 * Routines to manage a dynamic sequential-access array, under the
 * same restriction on maximum mallocable block. This array may be
 * written to in two ways: a contiguous chunk can be reserved of a
 * given size with a pointer returned OR single-byte data may be
 * written. The array can also be read back in the same two ways:
 * as a series of big byte-data blocks or as a list of structures
 * of a given size.
 */

struct SAA
{
    /*
     * members `end' and `elem_len' are only valid in first link in
     * list; `rptr' and `rpos' are used for reading
     */
    size_t elem_len; /* Size of each element */
    size_t blk_len;  /* Size of each allocation block */
    size_t nblks;    /* Total number of allocated blocks */
    size_t nblkptrs; /* Total number of allocation block pointers */
    size_t length;   /* Total allocated length of the array */
    size_t datalen;  /* Total data length of the array */
    char **wblk;     /* Write block pointer */
    size_t wpos;     /* Write position inside block */
    size_t wptr;     /* Absolute write position */
    char **rblk;     /* Read block pointer */
    size_t rpos;     /* Read position inside block */
    size_t rptr;     /* Absolute read position */
    char **blk_ptrs; /* Pointer to pointer blocks */
};

struct SAA *saa_init(size_t elem_len); /* 1 == byte */
void saa_free(struct SAA *);

void *saa_wstruct(struct SAA *);                     /* return a structure of elem_len */
void saa_wbytes(struct SAA *, const void *, size_t); /* write arbitrary bytes */
void saa_rewind(struct SAA *);                       /* for reading from beginning */
void *saa_rstruct(struct SAA *);                     /* return NULL on EOA */
const void *saa_rbytes(struct SAA *, size_t *);      /* return 0 on EOA */
void saa_rnbytes(struct SAA *, void *, size_t);      /* read a given no. of bytes */
/* random access */
void saa_fread(struct SAA *, size_t, void *, size_t);

void saa_fwrite(struct SAA *, size_t, const void *, size_t);

/* dump to file */
void saa_fpwrite(struct SAA *, FILE *);

/* Write specific-sized values */
void saa_write8(struct SAA *s, uint8_t v);

void saa_write16(struct SAA *s, uint16_t v);

void saa_write32(struct SAA *s, uint32_t v);

void saa_write64(struct SAA *s, uint64_t v);

void saa_wleb128u(struct SAA *, int); /* write unsigned LEB128 value */
void saa_wleb128s(struct SAA *, int); /* write signed LEB128 value */
void saa_writeaddr(struct SAA *, uint64_t, size_t);

#endif /* AS_SAA_H */
