//
// Packet buffer management
//
#ifndef PBUF_H
#define PBUF_H

#define PBUF_TRANSPORT_HLEN 20
#define PBUF_IP_HLEN        20
#define PBUF_LINK_HLEN      14

#define PBUF_TRANSPORT      0
#define PBUF_IP             1
#define PBUF_LINK           2
#define PBUF_RAW            3

#define PBUF_RW             0
#define PBUF_RO             1
#define PBUF_POOL           2

// Definitions for the pbuf flag field (these are not the flags that
// are passed to pbuf_alloc()).

#define PBUF_FLAG_RW    0x00    // Flags that pbuf data is read/write.
#define PBUF_FLAG_RO    0x01    // Flags that pbuf data is read-only.
#define PBUF_FLAG_POOL  0x02    // Flags that the pbuf comes from the pbuf pool.

struct pbuf {
    struct pbuf *next;

    unsigned short flags;
    unsigned short ref;
    void *payload;

    int tot_len;                // Total length of buffer + additionally chained buffers.
    int len;                    // Length of this buffer.
    int size;                   // Allocated size of buffer
};

void pbuf_init();

krnlapi struct pbuf *pbuf_alloc(int layer, int size, int type);

krnlapi void pbuf_realloc(struct pbuf *p, int size);

krnlapi int pbuf_header(struct pbuf *p, int header_size);

krnlapi int pbuf_clen(struct pbuf *p);

krnlapi int pbuf_spare(struct pbuf *p);

krnlapi void pbuf_ref(struct pbuf *p);

krnlapi int pbuf_free(struct pbuf *p);

krnlapi void pbuf_chain(struct pbuf *h, struct pbuf *t);

krnlapi struct pbuf *pbuf_dechain(struct pbuf *p);

krnlapi struct pbuf *pbuf_dup(int layer, struct pbuf *p);

krnlapi struct pbuf *pbuf_linearize(int layer, struct pbuf *p);

krnlapi struct pbuf *pbuf_cow(int layer, struct pbuf *p);

#endif
