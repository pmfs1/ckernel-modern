/*
 * outobj.c	output routines for the assembler to produce
 *		.OBJ object files
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>

#include "as.h"
#include "aslib.h"
#include "stdscan.h"
#include "eval.h"
#include "out/outform.h"
#include "out/outlib.h"

#ifdef OF_OBJ

/*
 * outobj.c is divided into two sections.  The first section is low level
 * routines for creating obj records;  It has nearly zero AS specific
 * code.  The second section is high level routines for processing calls and
 * data structures from the rest of AS into obj format.
 *
 * It should be easy (though not zero work) to lift the first section out for
 * use as an obj file writer for some other assembler or compiler.
 */

/*
 * These routines are built around the ObjRecord data struture.  An ObjRecord
 * holds an object file record that may be under construction or complete.
 *
 * A major function of these routines is to support continuation of an obj
 * record into the next record when the maximum record size is exceeded.  The
 * high level code does not need to worry about where the record breaks occur.
 * It does need to do some minor extra steps to make the automatic continuation
 * work.  Those steps may be skipped for records where the high level knows no
 * continuation could be required.
 *
 * 1) An ObjRecord is allocated and cleared by obj_new, or an existing ObjRecord
 *    is cleared by obj_clear.
 *
 * 2) The caller should fill in .type.
 *
 * 3) If the record is continuable and there is processing that must be done at
 *    the start of each record then the caller should fill in .ori with the
 *    address of the record initializer routine.
 *
 * 4) If the record is continuable and it should be saved (rather than emitted
 *    immediately) as each record is done, the caller should set .up to be a
 *    pointer to a location in which the caller keeps the master pointer to the
 *    ObjRecord.  When the record is continued, the obj_bump routine will then
 *    allocate a new ObjRecord structure and update the master pointer.
 *
 * 5) If the .ori field was used then the caller should fill in the .parm with
 *    any data required by the initializer.
 *
 * 6) The caller uses the routines: obj_byte, obj_word, obj_rword, obj_dword,
 *    obj_x, obj_index, obj_value and obj_name to fill in the various kinds of
 *    data required for this record.
 *
 * 7) If the record is continuable, the caller should call obj_commit at each
 *    point where breaking the record is permitted.
 *
 * 8) To write out the record, the caller should call obj_emit2.  If the
 *    caller has called obj_commit for all data written then he can get slightly
 *    faster code by calling obj_emit instead of obj_emit2.
 *
 * Most of these routines return an ObjRecord pointer.  This will be the input
 * pointer most of the time and will be the new location if the ObjRecord
 * moved as a result of the call.  The caller may ignore the return value in
 * three cases:  It is a "Never Reallocates" routine;  or  The caller knows
 * continuation is not possible;  or  The caller uses the master pointer for the
 * next operation.
 */

#define RECORD_MAX (1024 - 3) /* maximal size of any record except type+reclen */
#define OBJ_PARMS 3           /* maximum .parm used by any .ori routine */

#define FIX_08_LOW 0x8000 /* location type for various fixup subrecords */
#define FIX_16_OFFSET 0x8400
#define FIX_16_SELECTOR 0x8800
#define FIX_32_POINTER 0x8C00
#define FIX_08_HIGH 0x9000
#define FIX_32_OFFSET 0xA400
#define FIX_48_POINTER 0xAC00

enum RecordID
{ /* record ID codes */

  THEADR = 0x80, /* module header */
  COMENT = 0x88, /* comment record */

  LINNUM = 0x94, /* line number record */
  LNAMES = 0x96, /* list of names */

  SEGDEF = 0x98, /* segment definition */
  GRPDEF = 0x9A, /* group definition */
  EXTDEF = 0x8C, /* external definition */
  PUBDEF = 0x90, /* public definition */
  COMDEF = 0xB0, /* common definition */

  LEDATA = 0xA0, /* logical enumerated data */
  FIXUPP = 0x9C, /* fixups (relocations) */
  FIXU32 = 0x9D, /* 32-bit fixups (relocations) */

  MODEND = 0x8A, /* module end */
  MODE32 = 0x8B  /* module end for 32-bit objects */
};

enum ComentID
{ /* ID codes for comment records */

  dEXTENDED = 0xA1, /* tells that we are using translator-specific extensions */
  dLINKPASS = 0xA2, /* link pass 2 marker */
  dTYPEDEF = 0xE3,  /* define a type */
  dSYM = 0xE6,      /* symbol debug record */
  dFILNAME = 0xE8,  /* file name record */
  dCOMPDEF = 0xEA   /* compiler type info */
};

typedef struct ObjRecord ObjRecord;
typedef void ORI(ObjRecord *orp);

struct ObjRecord
{
    ORI *ori;                 /* Initialization routine           */
    int used;                 /* Current data size                */
    int committed;            /* Data size at last boundary       */
    int x_size;               /* (see obj_x)                      */
    unsigned int type;        /* Record type                      */
    ObjRecord *child;         /* Associated record below this one */
    ObjRecord **up;           /* Master pointer to this ObjRecord */
    ObjRecord *back;          /* Previous part of this record     */
    uint32_t parm[OBJ_PARMS]; /* Parameters for ori routine       */
    uint8_t buf[RECORD_MAX + 3];
};

static void obj_fwrite(ObjRecord *orp);
static void ori_ledata(ObjRecord *orp);
static void ori_pubdef(ObjRecord *orp);
static void ori_null(ObjRecord *orp);
static ObjRecord *obj_commit(ObjRecord *orp);

static bool obj_uppercase; /* Flag: all names in uppercase */
static bool obj_use32;     /* Flag: at least one segment is 32-bit */

/*
 * Clear an ObjRecord structure.  (Never reallocates).
 * To simplify reuse of ObjRecord's, .type, .ori and .parm are not cleared.
 */
static ObjRecord *obj_clear(ObjRecord *orp)
{
    orp->used = 0;
    orp->committed = 0;
    orp->x_size = 0;
    orp->child = NULL;
    orp->up = NULL;
    orp->back = NULL;
    return (orp);
}

/*
 * Emit an ObjRecord structure.  (Never reallocates).
 * The record is written out preceeded (recursively) by its previous part (if
 * any) and followed (recursively) by its child (if any).
 * The previous part and the child are freed.  The main ObjRecord is cleared,
 * not freed.
 */
static ObjRecord *obj_emit(ObjRecord *orp)
{
    if (orp->back)
    {
        obj_emit(orp->back);
        as_free(orp->back);
    }

    if (orp->committed)
        obj_fwrite(orp);

    if (orp->child)
    {
        obj_emit(orp->child);
        as_free(orp->child);
    }

    return (obj_clear(orp));
}

/*
 * Commit and Emit a record.  (Never reallocates).
 */
static ObjRecord *obj_emit2(ObjRecord *orp)
{
    obj_commit(orp);
    return (obj_emit(orp));
}

/*
 * Allocate and clear a new ObjRecord;  Also sets .ori to ori_null
 */
static ObjRecord *obj_new(void)
{
    ObjRecord *orp;

    orp = obj_clear(as_malloc(sizeof(ObjRecord)));
    orp->ori = ori_null;
    return (orp);
}

/*
 * Advance to the next record because the existing one is full or its x_size
 * is incompatible.
 * Any uncommited data is moved into the next record.
 */
static ObjRecord *obj_bump(ObjRecord *orp)
{
    ObjRecord *nxt;
    int used = orp->used;
    int committed = orp->committed;

    if (orp->up)
    {
        *orp->up = nxt = obj_new();
        nxt->ori = orp->ori;
        nxt->type = orp->type;
        nxt->up = orp->up;
        nxt->back = orp;
        memcpy(nxt->parm, orp->parm, sizeof(orp->parm));
    }
    else
        nxt = obj_emit(orp);

    used -= committed;
    if (used)
    {
        nxt->committed = 1;
        nxt->ori(nxt);
        nxt->committed = nxt->used;
        memcpy(nxt->buf + nxt->committed, orp->buf + committed, used);
        nxt->used = nxt->committed + used;
    }

    return (nxt);
}

/*
 * Advance to the next record if necessary to allow the next field to fit.
 */
static ObjRecord *obj_check(ObjRecord *orp, int size)
{
    if (orp->used + size > RECORD_MAX)
        orp = obj_bump(orp);

    if (!orp->committed)
    {
        orp->committed = 1;
        orp->ori(orp);
        orp->committed = orp->used;
    }

    return (orp);
}

/*
 * All data written so far is commited to the current record (won't be moved to
 * the next record in case of continuation).
 */
static ObjRecord *obj_commit(ObjRecord *orp)
{
    orp->committed = orp->used;
    return (orp);
}

/*
 * Write a byte
 */
static ObjRecord *obj_byte(ObjRecord *orp, uint8_t val)
{
    orp = obj_check(orp, 1);
    orp->buf[orp->used] = val;
    orp->used++;
    return (orp);
}

/*
 * Write a word
 */
static ObjRecord *obj_word(ObjRecord *orp, unsigned int val)
{
    orp = obj_check(orp, 2);
    orp->buf[orp->used] = val;
    orp->buf[orp->used + 1] = val >> 8;
    orp->used += 2;
    return (orp);
}

/*
 * Write a reversed word
 */
static ObjRecord *obj_rword(ObjRecord *orp, unsigned int val)
{
    orp = obj_check(orp, 2);
    orp->buf[orp->used] = val >> 8;
    orp->buf[orp->used + 1] = val;
    orp->used += 2;
    return (orp);
}

/*
 * Write a dword
 */
static ObjRecord *obj_dword(ObjRecord *orp, uint32_t val)
{
    orp = obj_check(orp, 4);
    orp->buf[orp->used] = val;
    orp->buf[orp->used + 1] = val >> 8;
    orp->buf[orp->used + 2] = val >> 16;
    orp->buf[orp->used + 3] = val >> 24;
    orp->used += 4;
    return (orp);
}

/*
 * All fields of "size x" in one obj record must be the same size (either 16
 * bits or 32 bits).  There is a one bit flag in each record which specifies
 * which.
 * This routine is used to force the current record to have the desired
 * x_size.  x_size is normally automatic (using obj_x), so that this
 * routine should be used outside obj_x, only to provide compatibility with
 * linkers that have bugs in their processing of the size bit.
 */

static ObjRecord *obj_force(ObjRecord *orp, int x)
{
    if (orp->x_size == (x ^ 48))
        orp = obj_bump(orp);
    orp->x_size = x;
    return (orp);
}

/*
 * This routine writes a field of size x.  The caller does not need to worry at
 * all about whether 16-bits or 32-bits are required.
 */
static ObjRecord *obj_x(ObjRecord *orp, uint32_t val)
{
    if (orp->type & 1)
        orp->x_size = 32;
    if (val > 0xFFFF)
        orp = obj_force(orp, 32);
    if (orp->x_size == 32)
    {
        ObjRecord *nxt = obj_dword(orp, val);
        nxt->x_size = 32; /* x_size is cleared when a record overflows */
        return nxt;
    }
    orp->x_size = 16;
    return (obj_word(orp, val));
}

/*
 * Writes an index
 */
static ObjRecord *obj_index(ObjRecord *orp, unsigned int val)
{
    if (val < 128)
        return (obj_byte(orp, val));
    return (obj_word(orp, (val >> 8) | (val << 8) | 0x80));
}

/*
 * Writes a variable length value
 */
static ObjRecord *obj_value(ObjRecord *orp, uint32_t val)
{
    if (val <= 128)
        return (obj_byte(orp, val));
    if (val <= 0xFFFF)
    {
        orp = obj_byte(orp, 129);
        return (obj_word(orp, val));
    }
    if (val <= 0xFFFFFF)
        return (obj_dword(orp, (val << 8) + 132));
    orp = obj_byte(orp, 136);
    return (obj_dword(orp, val));
}

/*
 * Writes a counted string
 */
static ObjRecord *obj_name(ObjRecord *orp, const char *name)
{
    int len = strlen(name);
    uint8_t *ptr;

    orp = obj_check(orp, len + 1);
    ptr = orp->buf + orp->used;
    *ptr++ = len;
    orp->used += len + 1;
    if (obj_uppercase)
        while (--len >= 0)
        {
            *ptr++ = toupper(*name);
            name++;
        }
    else
        memcpy(ptr, name, len);
    return (orp);
}

/*
 * Initializer for an LEDATA record.
 * parm[0] = offset
 * parm[1] = segment index
 * During the use of a LEDATA ObjRecord, parm[0] is constantly updated to
 * represent the offset that would be required if the record were split at the
 * last commit point.
 * parm[2] is a copy of parm[0] as it was when the current record was initted.
 */
static void ori_ledata(ObjRecord *orp)
{
    obj_index(orp, orp->parm[1]);
    orp->parm[2] = orp->parm[0];
    obj_x(orp, orp->parm[0]);
}

/*
 * Initializer for a PUBDEF record.
 * parm[0] = group index
 * parm[1] = segment index
 * parm[2] = frame (only used when both indexes are zero)
 */
static void ori_pubdef(ObjRecord *orp)
{
    obj_index(orp, orp->parm[0]);
    obj_index(orp, orp->parm[1]);
    if (!(orp->parm[0] | orp->parm[1]))
        obj_word(orp, orp->parm[2]);
}

/*
 * Initializer for a LINNUM record.
 * parm[0] = group index
 * parm[1] = segment index
 */
static void ori_linnum(ObjRecord *orp)
{
    obj_index(orp, orp->parm[0]);
    obj_index(orp, orp->parm[1]);
}

/*
 * Initializer for a local vars record.
 */
static void ori_local(ObjRecord *orp)
{
    obj_byte(orp, 0x40);
    obj_byte(orp, dSYM);
}

/*
 * Null initializer for records that continue without any header info
 */
static void ori_null(ObjRecord *orp)
{
    (void)orp; /* Do nothing */
}

/*
 * This concludes the low level section of outobj.c
 */

static char obj_infile[FILENAME_MAX];

static int32_t first_seg;
static bool any_segs;
static int passtwo;
static int arrindex;

#define GROUP_MAX 256  /* we won't _realistically_ have more \
                        * than this many segs in a group */
#define EXT_BLKSIZ 256 /* block size for externals list */

struct Segment; /* need to know these structs exist */
struct Group;

struct LineNumber
{
    struct LineNumber *next;
    struct Segment *segment;
    int32_t offset;
    int32_t lineno;
};

static struct FileName
{
    struct FileName *next;
    char *name;
    struct LineNumber *lnhead, **lntail;
    int index;
} *fnhead, **fntail;

static struct Array
{
    struct Array *next;
    unsigned size;
    int basetype;
} *arrhead, **arrtail;

#define ARRAYBOT 31 /* magic number  for first array index */

static struct Public
{
    struct Public *next;
    char *name;
    int32_t offset;
    int32_t segment; /* only if it's far-absolute */
    int type;        /* only for local debug syms */
} *fpubhead, **fpubtail, *last_defined;

static struct External
{
    struct External *next;
    char *name;
    int32_t commonsize;
    int32_t commonelem; /* element size if FAR, else zero */
    int index;          /* OBJ-file external index */
    enum
    {
        DEFWRT_NONE,    /* no unusual default-WRT */
        DEFWRT_STRING,  /* a string we don't yet understand */
        DEFWRT_SEGMENT, /* a segment */
        DEFWRT_GROUP    /* a group */
    } defwrt_type;
    union
    {
        char *string;
        struct Segment *seg;
        struct Group *grp;
    } defwrt_ptr;
    struct External *next_dws; /* next with DEFWRT_STRING */
} *exthead, **exttail, *dws;

static int externals;

static struct ExtBack
{
    struct ExtBack *next;
    struct External *exts[EXT_BLKSIZ];
} *ebhead, **ebtail;

static struct Segment
{
    struct Segment *next;
    int32_t index;     /* the AS segment id */
    int32_t obj_index; /* the OBJ-file segment index */
    struct Group *grp; /* the group it beint32_ts to */
    uint32_t currentpos;
    int32_t align; /* can be SEG_ABS + absolute addr */
    enum
    {
        CMB_PRIVATE = 0,
        CMB_PUBLIC = 2,
        CMB_STACK = 5,
        CMB_COMMON = 6
    } combine;
    bool use32; /* is this segment 32-bit? */
    struct Public *pubhead, **pubtail, *lochead, **loctail;
    char *name;
    char *segclass, *overlay; /* `class' is a C++ keyword :-) */
    ObjRecord *orp;
} *seghead, **segtail, *obj_seg_needs_update;

static struct Group
{
    struct Group *next;
    char *name;
    int32_t index;     /* AS segment id */
    int32_t obj_index; /* OBJ-file group index */
    int32_t nentries;  /* number of elements... */
    int32_t nindices;  /* ...and number of index elts... */
    union
    {
        int32_t index;
        char *name;
    } segs[GROUP_MAX]; /* ...in this */
} *grphead, **grptail, *obj_grp_needs_update;

static struct ImpDef
{
    struct ImpDef *next;
    char *extname;
    char *libname;
    unsigned int impindex;
    char *impname;
} *imphead, **imptail;

static struct ExpDef
{
    struct ExpDef *next;
    char *intname;
    char *extname;
    unsigned int ordinal;
    int flags;
} *exphead, **exptail;

#define EXPDEF_FLAG_ORDINAL 0x80
#define EXPDEF_FLAG_RESIDENT 0x40
#define EXPDEF_FLAG_NODATA 0x20
#define EXPDEF_MASK_PARMCNT 0x1F

static int32_t obj_entry_seg, obj_entry_ofs;

struct ofmt of_obj;

/* The current segment */
static struct Segment *current_seg;

static int32_t obj_segment(char *, int, int *);
static void obj_write_file(int debuginfo);
static int obj_directive(enum directives, char *, int);

static void obj_init(void)
{
    first_seg = seg_alloc();
    any_segs = false;
    fpubhead = NULL;
    fpubtail = &fpubhead;
    exthead = NULL;
    exttail = &exthead;
    imphead = NULL;
    imptail = &imphead;
    exphead = NULL;
    exptail = &exphead;
    dws = NULL;
    externals = 0;
    ebhead = NULL;
    ebtail = &ebhead;
    seghead = obj_seg_needs_update = NULL;
    segtail = &seghead;
    grphead = obj_grp_needs_update = NULL;
    grptail = &grphead;
    obj_entry_seg = NO_SEG;
    obj_uppercase = false;
    obj_use32 = false;
    passtwo = 0;
    current_seg = NULL;
}

static int obj_set_info(enum geninfo type, char **val)
{
    (void)type;
    (void)val;

    return 0;
}
static void obj_cleanup(int debuginfo)
{
    obj_write_file(debuginfo);
    of_obj.current_dfmt->cleanup();
    while (seghead)
    {
        struct Segment *segtmp = seghead;
        seghead = seghead->next;
        while (segtmp->pubhead)
        {
            struct Public *pubtmp = segtmp->pubhead;
            segtmp->pubhead = pubtmp->next;
            as_free(pubtmp->name);
            as_free(pubtmp);
        }
        as_free(segtmp->segclass);
        as_free(segtmp->overlay);
        as_free(segtmp);
    }
    while (fpubhead)
    {
        struct Public *pubtmp = fpubhead;
        fpubhead = fpubhead->next;
        as_free(pubtmp->name);
        as_free(pubtmp);
    }
    while (exthead)
    {
        struct External *exttmp = exthead;
        exthead = exthead->next;
        as_free(exttmp);
    }
    while (imphead)
    {
        struct ImpDef *imptmp = imphead;
        imphead = imphead->next;
        as_free(imptmp->extname);
        as_free(imptmp->libname);
        as_free(imptmp->impname); /* as_free won't mind if it's NULL */
        as_free(imptmp);
    }
    while (exphead)
    {
        struct ExpDef *exptmp = exphead;
        exphead = exphead->next;
        as_free(exptmp->extname);
        as_free(exptmp->intname);
        as_free(exptmp);
    }
    while (ebhead)
    {
        struct ExtBack *ebtmp = ebhead;
        ebhead = ebhead->next;
        as_free(ebtmp);
    }
    while (grphead)
    {
        struct Group *grptmp = grphead;
        grphead = grphead->next;
        as_free(grptmp);
    }
}

static void obj_ext_set_defwrt(struct External *ext, char *id)
{
    struct Segment *seg;
    struct Group *grp;

    for (seg = seghead; seg; seg = seg->next)
        if (!strcmp(seg->name, id))
        {
            ext->defwrt_type = DEFWRT_SEGMENT;
            ext->defwrt_ptr.seg = seg;
            as_free(id);
            return;
        }

    for (grp = grphead; grp; grp = grp->next)
        if (!strcmp(grp->name, id))
        {
            ext->defwrt_type = DEFWRT_GROUP;
            ext->defwrt_ptr.grp = grp;
            as_free(id);
            return;
        }

    ext->defwrt_type = DEFWRT_STRING;
    ext->defwrt_ptr.string = id;
    ext->next_dws = dws;
    dws = ext;
}

static void obj_deflabel(char *name, int32_t segment,
                         int64_t offset, int is_global, char *special)
{
    /*
     * We have three cases:
     *
     * (i) `segment' is a segment-base. If so, set the name field
     * for the segment or group structure it refers to, and then
     * return.
     *
     * (ii) `segment' is one of our segments, or a SEG_ABS segment.
     * Save the label position for later output of a PUBDEF record.
     * (Or a MODPUB, if we work out how.)
     *
     * (iii) `segment' is not one of our segments. Save the label
     * position for later output of an EXTDEF, and also store a
     * back-reference so that we can map later references to this
     * segment number to the external index.
     */
    struct External *ext;
    struct ExtBack *eb;
    struct Segment *seg;
    int i;
    bool used_special = false; /* have we used the special text? */

#if defined(DEBUG) && DEBUG > 2
    as_error(ERR_DEBUG,
             " obj_deflabel: %s, seg=%" PRIx32 ", off=%" PRIx64 ", is_global=%d, %s\n",
             name, segment, offset, is_global, special);
#endif

    /*
     * If it's a special-retry from pass two, discard it.
     */
    if (is_global == 3)
        return;

    /*
     * First check for the double-period, signifying something
     * unusual.
     */
    if (name[0] == '.' && name[1] == '.' && name[2] != '@')
    {
        if (!strcmp(name, "..start"))
        {
            obj_entry_seg = segment;
            obj_entry_ofs = offset;
            return;
        }
        as_error(ERR_NONFATAL, "unrecognised special symbol `%s'", name);
    }

    /*
     * Case (i):
     */
    if (obj_seg_needs_update)
    {
        obj_seg_needs_update->name = name;
        return;
    }
    else if (obj_grp_needs_update)
    {
        obj_grp_needs_update->name = name;
        return;
    }
    if (segment < SEG_ABS && segment != NO_SEG && segment % 2)
        return;

    if (segment >= SEG_ABS || segment == NO_SEG)
    {
        /*
         * SEG_ABS subcase of (ii).
         */
        if (is_global)
        {
            struct Public *pub;

            pub = *fpubtail = as_malloc(sizeof(*pub));
            fpubtail = &pub->next;
            pub->next = NULL;
            pub->name = as_strdup(name);
            pub->offset = offset;
            pub->segment = (segment == NO_SEG ? 0 : segment & ~SEG_ABS);
        }
        if (special)
            as_error(ERR_NONFATAL, "OBJ supports no special symbol features"
                                   " for this symbol type");
        return;
    }

    /*
     * If `any_segs' is still false, we might need to define a
     * default segment, if they're trying to declare a label in
     * `first_seg'.
     */
    if (!any_segs && segment == first_seg)
    {
        int tempint; /* ignored */
        if (segment != obj_segment("__ASDEFSEG", 2, &tempint))
            as_error(ERR_PANIC, "strange segment conditions in OBJ driver");
    }

    for (seg = seghead; seg && is_global; seg = seg->next)
        if (seg->index == segment)
        {
            struct Public *loc = as_malloc(sizeof(*loc));
            /*
             * Case (ii). Maybe MODPUB someday?
             */
            *seg->pubtail = loc;
            seg->pubtail = &loc->next;
            loc->next = NULL;
            loc->name = as_strdup(name);
            loc->offset = offset;

            if (special)
                as_error(ERR_NONFATAL,
                         "OBJ supports no special symbol features"
                         " for this symbol type");
            return;
        }

    /*
     * Case (iii).
     */
    if (is_global)
    {
        ext = *exttail = as_malloc(sizeof(*ext));
        ext->next = NULL;
        exttail = &ext->next;
        ext->name = name;
        /* Place by default all externs into the current segment */
        ext->defwrt_type = DEFWRT_NONE;

/* 28-Apr-2002 - John Coffman
  The following code was introduced on 12-Aug-2000, and breaks fixups
  on code passed thru the MSC 5.1 linker (3.66) and MSC 6.00A linker
  (5.10).  It was introduced after FIXUP32 was added, and may be needed
  for 32-bit segments.  The following will get 16-bit segments working
  again, and maybe someone can correct the 'if' condition which is
  actually needed.
*/
#if 0
        if (current_seg) {
#else
        if (current_seg && current_seg->use32)
        {
            if (current_seg->grp)
            {
                ext->defwrt_type = DEFWRT_GROUP;
                ext->defwrt_ptr.grp = current_seg->grp;
            }
            else
            {
                ext->defwrt_type = DEFWRT_SEGMENT;
                ext->defwrt_ptr.seg = current_seg;
            }
        }
#endif

        if (is_global == 2)
        {
            ext->commonsize = offset;
            ext->commonelem = 1; /* default FAR */
        }
        else
            ext->commonsize = 0;
    }
    else
        return;

    /*
     * Now process the special text, if any, to find default-WRT
     * specifications and common-variable element-size and near/far
     * specifications.
     */
    while (special && *special)
    {
        used_special = true;

        /*
         * We might have a default-WRT specification.
         */
        if (!as_strnicmp(special, "wrt", 3))
        {
            char *p;
            int len;
            special += 3;
            special += strspn(special, " \t");
            p = as_strndup(special, len = strcspn(special, ":"));
            obj_ext_set_defwrt(ext, p);
            special += len;
            if (*special && *special != ':')
                as_error(ERR_NONFATAL, "`:' expected in special symbol"
                                       " text for `%s'",
                         ext->name);
            else if (*special == ':')
                special++;
        }

        /*
         * The NEAR or FAR keywords specify nearness or
         * farness. FAR gives default element size 1.
         */
        if (!as_strnicmp(special, "far", 3))
        {
            if (ext->commonsize)
                ext->commonelem = 1;
            else
                as_error(ERR_NONFATAL,
                         "`%s': `far' keyword may only be applied"
                         " to common variables\n",
                         ext->name);
            special += 3;
            special += strspn(special, " \t");
        }
        else if (!as_strnicmp(special, "near", 4))
        {
            if (ext->commonsize)
                ext->commonelem = 0;
            else
                as_error(ERR_NONFATAL,
                         "`%s': `far' keyword may only be applied"
                         " to common variables\n",
                         ext->name);
            special += 4;
            special += strspn(special, " \t");
        }

        /*
         * If it's a common, and anything else remains on the line
         * before a further colon, evaluate it as an expression and
         * use that as the element size. Forward references aren't
         * allowed.
         */
        if (*special == ':')
            special++;
        else if (*special)
        {
            if (ext->commonsize)
            {
                expr *e;
                struct tokenval tokval;

                stdscan_reset();
                stdscan_set(special);
                tokval.t_type = TOKEN_INVALID;
                e = evaluate(stdscan, NULL, &tokval, NULL, 1, as_error, NULL);
                if (e)
                {
                    if (!is_simple(e))
                        as_error(ERR_NONFATAL, "cannot use relocatable"
                                               " expression as common-variable element size");
                    else
                        ext->commonelem = reloc_value(e);
                }
                special = stdscan_get();
            }
            else
            {
                as_error(ERR_NONFATAL,
                         "`%s': element-size specifications only"
                         " apply to common variables",
                         ext->name);
                while (*special && *special != ':')
                    special++;
                if (*special == ':')
                    special++;
            }
        }
    }

    i = segment / 2;
    eb = ebhead;
    if (!eb)
    {
        eb = *ebtail = as_zalloc(sizeof(*eb));
        eb->next = NULL;
        ebtail = &eb->next;
    }
    while (i >= EXT_BLKSIZ)
    {
        if (eb && eb->next)
            eb = eb->next;
        else
        {
            eb = *ebtail = as_zalloc(sizeof(*eb));
            eb->next = NULL;
            ebtail = &eb->next;
        }
        i -= EXT_BLKSIZ;
    }
    eb->exts[i] = ext;
    ext->index = ++externals;

    if (special && !used_special)
        as_error(ERR_NONFATAL, "OBJ supports no special symbol features"
                               " for this symbol type");
}

/* forward declaration */
static void obj_write_fixup(ObjRecord *orp, int bytes,
                            int segrel, int32_t seg, int32_t wrt,
                            struct Segment *segto);

static void obj_out(int32_t segto, const void *data,
                    enum out_type type, uint64_t size,
                    int32_t segment, int32_t wrt)
{
    const uint8_t *ucdata;
    int32_t ldata;
    struct Segment *seg;
    ObjRecord *orp;

    /*
     * handle absolute-assembly (structure definitions)
     */
    if (segto == NO_SEG)
    {
        if (type != OUT_RESERVE)
            as_error(ERR_NONFATAL, "attempt to assemble code in [ABSOLUTE]"
                                   " space");
        return;
    }

    /*
     * If `any_segs' is still false, we must define a default
     * segment.
     */
    if (!any_segs)
    {
        int tempint; /* ignored */
        if (segto != obj_segment("__ASDEFSEG", 2, &tempint))
            as_error(ERR_PANIC, "strange segment conditions in OBJ driver");
    }

    /*
     * Find the segment we are targetting.
     */
    for (seg = seghead; seg; seg = seg->next)
        if (seg->index == segto)
            break;
    if (!seg)
        as_error(ERR_PANIC, "code directed to nonexistent segment?");

    orp = seg->orp;
    orp->parm[0] = seg->currentpos;

    switch (type)
    {
    case OUT_RAWDATA:
        ucdata = data;
        while (size > 0)
        {
            unsigned int len;
            orp = obj_check(seg->orp, 1);
            len = RECORD_MAX - orp->used;
            if (len > size)
                len = size;
            memcpy(orp->buf + orp->used, ucdata, len);
            orp->committed = orp->used += len;
            orp->parm[0] = seg->currentpos += len;
            ucdata += len;
            size -= len;
        }
        break;

    case OUT_ADDRESS:
    case OUT_REL1ADR:
    case OUT_REL2ADR:
    case OUT_REL4ADR:
    case OUT_REL8ADR:
    {
        int rsize;

        if (segment == NO_SEG && type != OUT_ADDRESS)
            as_error(ERR_NONFATAL, "relative call to absolute address not"
                                   " supported by OBJ format");
        if (segment >= SEG_ABS)
            as_error(ERR_NONFATAL, "far-absolute relocations not supported"
                                   " by OBJ format");

        ldata = *(int64_t *)data;
        if (type != OUT_ADDRESS)
        {
            ldata += size;
            size = realsize(type, size);
            ldata -= size;
        }

        if (size > UINT_MAX)
            size = 0;

        switch ((unsigned int)size)
        {
        default:
            as_error(ERR_NONFATAL, "OBJ format can only handle 16- or "
                                   "32-byte relocations");
            segment = NO_SEG; /* Don't actually generate a relocation */
            break;
        case 2:
            orp = obj_word(orp, ldata);
            break;
        case 4:
            orp = obj_dword(orp, ldata);
            break;
        }

        rsize = size;
        if (segment < SEG_ABS && (segment != NO_SEG && segment % 2) &&
            size == 4)
        {
            /*
             * This is a 4-byte segment-base relocation such as
             * `MOV EAX,SEG foo'. OBJ format can't actually handle
             * these, but if the constant term has the 16 low bits
             * zero, we can just apply a 2-byte segment-base
             * relocation to the low word instead.
             */
            rsize = 2;
            if (ldata & 0xFFFF)
                as_error(ERR_NONFATAL, "OBJ format cannot handle complex"
                                       " dword-size segment base references");
        }
        if (segment != NO_SEG)
            obj_write_fixup(orp, rsize,
                            (type == OUT_ADDRESS ? 0x4000 : 0),
                            segment, wrt, seg);
        seg->currentpos += size;
        break;
    }

    default:
        as_error(ERR_NONFATAL,
                 "Relocation type not supported by output format");
        /* fall through */

    case OUT_RESERVE:
        if (orp->committed)
            orp = obj_bump(orp);
        seg->currentpos += size;
        break;
    }
    obj_commit(orp);
}

static void obj_write_fixup(ObjRecord *orp, int bytes,
                            int segrel, int32_t seg, int32_t wrt,
                            struct Segment *segto)
{
    unsigned locat;
    int method;
    int base;
    int32_t tidx, fidx;
    struct Segment *s = NULL;
    struct Group *g = NULL;
    struct External *e = NULL;
    ObjRecord *forp;

    if (bytes != 2 && bytes != 4)
    {
        as_error(ERR_NONFATAL, "`obj' output driver does not support"
                               " %d-bit relocations",
                 bytes << 3);
        return;
    }

    forp = orp->child;
    if (forp == NULL)
    {
        orp->child = forp = obj_new();
        forp->up = &(orp->child);
        /* We should choose between FIXUPP and FIXU32 record type */
        /* If we're targeting a 32-bit segment, use a FIXU32 record */
        if (segto->use32)
            forp->type = FIXU32;
        else
            forp->type = FIXUPP;
    }

    if (seg % 2)
    {
        base = true;
        locat = FIX_16_SELECTOR;
        seg--;
        if (bytes != 2)
            as_error(ERR_PANIC, "OBJ: 4-byte segment base fixup got"
                                " through sanity check");
    }
    else
    {
        base = false;
        locat = (bytes == 2) ? FIX_16_OFFSET : FIX_32_OFFSET;
        if (!segrel)
            /*
             * There is a bug in tlink that makes it process self relative
             * fixups incorrectly if the x_size doesn't match the location
             * size.
             */
            forp = obj_force(forp, bytes << 3);
    }

    forp = obj_rword(forp, locat | segrel | (orp->parm[0] - orp->parm[2]));

    tidx = fidx = -1, method = 0; /* placate optimisers */

    /*
     * See if we can find the segment ID in our segment list. If
     * so, we have a T4 (LSEG) target.
     */
    for (s = seghead; s; s = s->next)
        if (s->index == seg)
            break;
    if (s)
        method = 4, tidx = s->obj_index;
    else
    {
        for (g = grphead; g; g = g->next)
            if (g->index == seg)
                break;
        if (g)
            method = 5, tidx = g->obj_index;
        else
        {
            int32_t i = seg / 2;
            struct ExtBack *eb = ebhead;
            while (i >= EXT_BLKSIZ)
            {
                if (eb)
                    eb = eb->next;
                else
                    break;
                i -= EXT_BLKSIZ;
            }
            if (eb)
                method = 6, e = eb->exts[i], tidx = e->index;
            else
                as_error(ERR_PANIC,
                         "unrecognised segment value in obj_write_fixup");
        }
    }

    /*
     * If no WRT given, assume the natural default, which is method
     * F5 unless:
     *
     * - we are doing an OFFSET fixup for a grouped segment, in
     *   which case we require F1 (group).
     *
     * - we are doing an OFFSET fixup for an external with a
     *   default WRT, in which case we must honour the default WRT.
     */
    if (wrt == NO_SEG)
    {
        if (!base && s && s->grp)
            method |= 0x10, fidx = s->grp->obj_index;
        else if (!base && e && e->defwrt_type != DEFWRT_NONE)
        {
            if (e->defwrt_type == DEFWRT_SEGMENT)
                method |= 0x00, fidx = e->defwrt_ptr.seg->obj_index;
            else if (e->defwrt_type == DEFWRT_GROUP)
                method |= 0x10, fidx = e->defwrt_ptr.grp->obj_index;
            else
            {
                as_error(ERR_NONFATAL, "default WRT specification for"
                                       " external `%s' unresolved",
                         e->name);
                method |= 0x50, fidx = -1; /* got to do _something_ */
            }
        }
        else
            method |= 0x50, fidx = -1;
    }
    else
    {
        /*
         * See if we can find the WRT-segment ID in our segment
         * list. If so, we have a F0 (LSEG) frame.
         */
        for (s = seghead; s; s = s->next)
            if (s->index == wrt - 1)
                break;
        if (s)
            method |= 0x00, fidx = s->obj_index;
        else
        {
            for (g = grphead; g; g = g->next)
                if (g->index == wrt - 1)
                    break;
            if (g)
                method |= 0x10, fidx = g->obj_index;
            else
            {
                int32_t i = wrt / 2;
                struct ExtBack *eb = ebhead;
                while (i >= EXT_BLKSIZ)
                {
                    if (eb)
                        eb = eb->next;
                    else
                        break;
                    i -= EXT_BLKSIZ;
                }
                if (eb)
                    method |= 0x20, fidx = eb->exts[i]->index;
                else
                    as_error(ERR_PANIC,
                             "unrecognised WRT value in obj_write_fixup");
            }
        }
    }

    forp = obj_byte(forp, method);
    if (fidx != -1)
        forp = obj_index(forp, fidx);
    forp = obj_index(forp, tidx);
    obj_commit(forp);
}

static int32_t obj_segment(char *name, int pass, int *bits)
{
    /*
     * We call the label manager here to define a name for the new
     * segment, and when our _own_ label-definition stub gets
     * called in return, it should register the new segment name
     * using the pointer it gets passed. That way we save memory,
     * by sponging off the label manager.
     */
#if defined(DEBUG) && DEBUG >= 3
    as_error(ERR_DEBUG, " obj_segment: < %s >, pass=%d, *bits=%d\n",
             name, pass, *bits);
#endif
    if (!name)
    {
        *bits = 16;
        current_seg = NULL;
        return first_seg;
    }
    else
    {
        struct Segment *seg;
        struct Group *grp;
        struct External **extp;
        int obj_idx, i, attrs;
        bool rn_error;
        char *p;

        /*
         * Look for segment attributes.
         */
        attrs = 0;
        while (*name == '.')
            name++; /* hack, but a documented one */
        p = name;
        while (*p && !as_isspace(*p))
            p++;
        if (*p)
        {
            *p++ = '\0';
            while (*p && as_isspace(*p))
                *p++ = '\0';
        }
        while (*p)
        {
            while (*p && !as_isspace(*p))
                p++;
            if (*p)
            {
                *p++ = '\0';
                while (*p && as_isspace(*p))
                    *p++ = '\0';
            }

            attrs++;
        }

        obj_idx = 1;
        for (seg = seghead; seg; seg = seg->next)
        {
            obj_idx++;
            if (!strcmp(seg->name, name))
            {
                if (attrs > 0 && pass == 1)
                    as_error(ERR_WARNING, "segment attributes specified on"
                                          " redeclaration of segment: ignoring");
                if (seg->use32)
                    *bits = 32;
                else
                    *bits = 16;
                current_seg = seg;
                return seg->index;
            }
        }

        *segtail = seg = as_malloc(sizeof(*seg));
        seg->next = NULL;
        segtail = &seg->next;
        seg->index = (any_segs ? seg_alloc() : first_seg);
        seg->obj_index = obj_idx;
        seg->grp = NULL;
        any_segs = true;
        seg->name = NULL;
        seg->currentpos = 0;
        seg->align = 1;            /* default */
        seg->use32 = false;        /* default */
        seg->combine = CMB_PUBLIC; /* default */
        seg->segclass = seg->overlay = NULL;
        seg->pubhead = NULL;
        seg->pubtail = &seg->pubhead;
        seg->lochead = NULL;
        seg->loctail = &seg->lochead;
        seg->orp = obj_new();
        seg->orp->up = &(seg->orp);
        seg->orp->ori = ori_ledata;
        seg->orp->type = LEDATA;
        seg->orp->parm[1] = obj_idx;

        /*
         * Process the segment attributes.
         */
        p = name;
        while (attrs--)
        {
            p += strlen(p);
            while (!*p)
                p++;

            /*
             * `p' contains a segment attribute.
             */
            if (!as_stricmp(p, "private"))
                seg->combine = CMB_PRIVATE;
            else if (!as_stricmp(p, "public"))
                seg->combine = CMB_PUBLIC;
            else if (!as_stricmp(p, "common"))
                seg->combine = CMB_COMMON;
            else if (!as_stricmp(p, "stack"))
                seg->combine = CMB_STACK;
            else if (!as_stricmp(p, "use16"))
                seg->use32 = false;
            else if (!as_stricmp(p, "use32"))
                seg->use32 = true;
            else if (!as_stricmp(p, "flat"))
            {
                /*
                 * This segment is an OS/2 FLAT segment. That means
                 * that its default group is group FLAT, even if
                 * the group FLAT does not explicitly _contain_ the
                 * segment.
                 *
                 * When we see this, we must create the group
                 * `FLAT', containing no segments, if it does not
                 * already exist; then we must set the default
                 * group of this segment to be the FLAT group.
                 */
                struct Group *grp2;
                for (grp2 = grphead; grp2; grp2 = grp2->next)
                    if (!strcmp(grp2->name, "FLAT"))
                        break;
                if (!grp2)
                {
                    obj_directive(D_GROUP, "FLAT", 1);
                    for (grp2 = grphead; grp2; grp2 = grp2->next)
                        if (!strcmp(grp2->name, "FLAT"))
                            break;
                    if (!grp2)
                        as_error(ERR_PANIC, "failure to define FLAT?!");
                }
                seg->grp = grp2;
            }
            else if (!as_strnicmp(p, "class=", 6))
                seg->segclass = as_strdup(p + 6);
            else if (!as_strnicmp(p, "overlay=", 8))
                seg->overlay = as_strdup(p + 8);
            else if (!as_strnicmp(p, "align=", 6))
            {
                seg->align = readnum(p + 6, &rn_error);
                if (rn_error)
                {
                    seg->align = 1;
                    as_error(ERR_NONFATAL, "segment alignment should be"
                                           " numeric");
                }
                switch ((int)seg->align)
                {
                case 1:    /* BYTE */
                case 2:    /* WORD */
                case 4:    /* DWORD */
                case 16:   /* PARA */
                case 256:  /* PAGE */
                case 4096: /* PharLap extension */
                    break;
                case 8:
                    as_error(ERR_WARNING,
                             "OBJ format does not support alignment"
                             " of 8: rounding up to 16");
                    seg->align = 16;
                    break;
                case 32:
                case 64:
                case 128:
                    as_error(ERR_WARNING,
                             "OBJ format does not support alignment"
                             " of %d: rounding up to 256",
                             seg->align);
                    seg->align = 256;
                    break;
                case 512:
                case 1024:
                case 2048:
                    as_error(ERR_WARNING,
                             "OBJ format does not support alignment"
                             " of %d: rounding up to 4096",
                             seg->align);
                    seg->align = 4096;
                    break;
                default:
                    as_error(ERR_NONFATAL, "invalid alignment value %d",
                             seg->align);
                    seg->align = 1;
                    break;
                }
            }
            else if (!as_strnicmp(p, "absolute=", 9))
            {
                seg->align = SEG_ABS + readnum(p + 9, &rn_error);
                if (rn_error)
                    as_error(ERR_NONFATAL, "argument to `absolute' segment"
                                           " attribute should be numeric");
            }
        }

        /* We need to know whenever we have at least one 32-bit segment */
        obj_use32 |= seg->use32;

        obj_seg_needs_update = seg;
        if (seg->align >= SEG_ABS)
            define_label(name, NO_SEG, seg->align - SEG_ABS,
                         NULL, false, false);
        else
            define_label(name, seg->index + 1, 0L,
                         NULL, false, false);
        obj_seg_needs_update = NULL;

        /*
         * See if this segment is defined in any groups.
         */
        for (grp = grphead; grp; grp = grp->next)
        {
            for (i = grp->nindices; i < grp->nentries; i++)
            {
                if (!strcmp(grp->segs[i].name, seg->name))
                {
                    as_free(grp->segs[i].name);
                    grp->segs[i] = grp->segs[grp->nindices];
                    grp->segs[grp->nindices++].index = seg->obj_index;
                    if (seg->grp)
                        as_error(ERR_WARNING,
                                 "segment `%s' is already part of"
                                 " a group: first one takes precedence",
                                 seg->name);
                    else
                        seg->grp = grp;
                }
            }
        }

        /*
         * Walk through the list of externals with unresolved
         * default-WRT clauses, and resolve any that point at this
         * segment.
         */
        extp = &dws;
        while (*extp)
        {
            if ((*extp)->defwrt_type == DEFWRT_STRING &&
                !strcmp((*extp)->defwrt_ptr.string, seg->name))
            {
                as_free((*extp)->defwrt_ptr.string);
                (*extp)->defwrt_type = DEFWRT_SEGMENT;
                (*extp)->defwrt_ptr.seg = seg;
                *extp = (*extp)->next_dws;
            }
            else
                extp = &(*extp)->next_dws;
        }

        if (seg->use32)
            *bits = 32;
        else
            *bits = 16;
        current_seg = seg;
        return seg->index;
    }
}

static int obj_directive(enum directives directive, char *value, int pass)
{
    switch (directive)
    {
    case D_GROUP:
    {
        char *p, *q, *v;
        if (pass == 1)
        {
            struct Group *grp;
            struct Segment *seg;
            struct External **extp;
            int obj_idx;

            q = value;
            while (*q == '.')
                q++; /* hack, but a documented one */
            v = q;
            while (*q && !as_isspace(*q))
                q++;
            if (as_isspace(*q))
            {
                *q++ = '\0';
                while (*q && as_isspace(*q))
                    q++;
            }
            /*
             * Here we used to sanity-check the group directive to
             * ensure nobody tried to declare a group containing no
             * segments. However, OS/2 does this as standard
             * practice, so the sanity check has been removed.
             *
             * if (!*q) {
             *     as_error(ERR_NONFATAL,"GROUP directive contains no segments");
             *     return 1;
             * }
             */

            obj_idx = 1;
            for (grp = grphead; grp; grp = grp->next)
            {
                obj_idx++;
                if (!strcmp(grp->name, v))
                {
                    as_error(ERR_NONFATAL, "group `%s' defined twice", v);
                    return 1;
                }
            }

            *grptail = grp = as_malloc(sizeof(*grp));
            grp->next = NULL;
            grptail = &grp->next;
            grp->index = seg_alloc();
            grp->obj_index = obj_idx;
            grp->nindices = grp->nentries = 0;
            grp->name = NULL;

            obj_grp_needs_update = grp;
            define_label(v, grp->index + 1, 0L, NULL, false, false);
            obj_grp_needs_update = NULL;

            while (*q)
            {
                p = q;
                while (*q && !as_isspace(*q))
                    q++;
                if (as_isspace(*q))
                {
                    *q++ = '\0';
                    while (*q && as_isspace(*q))
                        q++;
                }
                /*
                 * Now p contains a segment name. Find it.
                 */
                for (seg = seghead; seg; seg = seg->next)
                    if (!strcmp(seg->name, p))
                        break;
                if (seg)
                {
                    /*
                     * We have a segment index. Shift a name entry
                     * to the end of the array to make room.
                     */
                    grp->segs[grp->nentries++] = grp->segs[grp->nindices];
                    grp->segs[grp->nindices++].index = seg->obj_index;
                    if (seg->grp)
                        as_error(ERR_WARNING,
                                 "segment `%s' is already part of"
                                 " a group: first one takes precedence",
                                 seg->name);
                    else
                        seg->grp = grp;
                }
                else
                {
                    /*
                     * We have an as-yet undefined segment.
                     * Remember its name, for later.
                     */
                    grp->segs[grp->nentries++].name = as_strdup(p);
                }
            }

            /*
             * Walk through the list of externals with unresolved
             * default-WRT clauses, and resolve any that point at
             * this group.
             */
            extp = &dws;
            while (*extp)
            {
                if ((*extp)->defwrt_type == DEFWRT_STRING &&
                    !strcmp((*extp)->defwrt_ptr.string, grp->name))
                {
                    as_free((*extp)->defwrt_ptr.string);
                    (*extp)->defwrt_type = DEFWRT_GROUP;
                    (*extp)->defwrt_ptr.grp = grp;
                    *extp = (*extp)->next_dws;
                }
                else
                    extp = &(*extp)->next_dws;
            }
        }
        return 1;
    }
    case D_UPPERCASE:
        obj_uppercase = true;
        return 1;

    case D_IMPORT:
    {
        char *q, *extname, *libname, *impname;

        if (pass == 2)
            return 1; /* ignore in pass two */
        extname = q = value;
        while (*q && !as_isspace(*q))
            q++;
        if (as_isspace(*q))
        {
            *q++ = '\0';
            while (*q && as_isspace(*q))
                q++;
        }

        libname = q;
        while (*q && !as_isspace(*q))
            q++;
        if (as_isspace(*q))
        {
            *q++ = '\0';
            while (*q && as_isspace(*q))
                q++;
        }

        impname = q;

        if (!*extname || !*libname)
            as_error(ERR_NONFATAL, "`import' directive requires symbol name"
                                   " and library name");
        else
        {
            struct ImpDef *imp;
            bool err = false;

            imp = *imptail = as_malloc(sizeof(struct ImpDef));
            imptail = &imp->next;
            imp->next = NULL;
            imp->extname = as_strdup(extname);
            imp->libname = as_strdup(libname);
            imp->impindex = readnum(impname, &err);
            if (!*impname || err)
                imp->impname = as_strdup(impname);
            else
                imp->impname = NULL;
        }

        return 1;
    }
    case D_EXPORT:
    {
        char *q, *extname, *intname, *v;
        struct ExpDef *export;
        int flags = 0;
        unsigned int ordinal = 0;

        if (pass == 2)
            return 1; /* ignore in pass two */
        intname = q = value;
        while (*q && !as_isspace(*q))
            q++;
        if (as_isspace(*q))
        {
            *q++ = '\0';
            while (*q && as_isspace(*q))
                q++;
        }

        extname = q;
        while (*q && !as_isspace(*q))
            q++;
        if (as_isspace(*q))
        {
            *q++ = '\0';
            while (*q && as_isspace(*q))
                q++;
        }

        if (!*intname)
        {
            as_error(ERR_NONFATAL, "`export' directive requires export name");
            return 1;
        }
        if (!*extname)
        {
            extname = intname;
            intname = "";
        }
        while (*q)
        {
            v = q;
            while (*q && !as_isspace(*q))
                q++;
            if (as_isspace(*q))
            {
                *q++ = '\0';
                while (*q && as_isspace(*q))
                    q++;
            }
            if (!as_stricmp(v, "resident"))
                flags |= EXPDEF_FLAG_RESIDENT;
            else if (!as_stricmp(v, "nodata"))
                flags |= EXPDEF_FLAG_NODATA;
            else if (!as_strnicmp(v, "parm=", 5))
            {
                bool err = false;
                flags |= EXPDEF_MASK_PARMCNT & readnum(v + 5, &err);
                if (err)
                {
                    as_error(ERR_NONFATAL,
                             "value `%s' for `parm' is non-numeric", v + 5);
                    return 1;
                }
            }
            else
            {
                bool err = false;
                ordinal = readnum(v, &err);
                if (err)
                {
                    as_error(ERR_NONFATAL,
                             "unrecognised export qualifier `%s'", v);
                    return 1;
                }
                flags |= EXPDEF_FLAG_ORDINAL;
            }
        }

        export = *exptail = as_malloc(sizeof(struct ExpDef));
        exptail = &export->next;
        export->next = NULL;
        export->extname = as_strdup(extname);
        export->intname = as_strdup(intname);
        export->ordinal = ordinal;
        export->flags = flags;

        return 1;
    }
    default:
        return 0;
    }
}

static void obj_sectalign(int32_t seg, unsigned int value)
{
    struct Segment *s;

    list_for_each(s, seghead)
    {
        if (s->index == seg)
            break;
    }

    /*
     * it should not be too big value
     * and applied on non-absolute sections
     */
    if (!s || !is_power2(value) ||
        value > 4096 || s->align >= SEG_ABS)
        return;

    /*
     * TODO: No code duplication please
     * consider making helper for this
     * mapping since section handler has
     * to do the same
     */
    switch (value)
    {
    case 8:
        value = 16;
        break;
    case 32:
    case 64:
    case 128:
        value = 256;
        break;
    case 512:
    case 1024:
    case 2048:
        value = 4096;
        break;
    }

    if (s->align < (int)value)
        s->align = value;
}

static int32_t obj_segbase(int32_t segment)
{
    struct Segment *seg;

    /*
     * Find the segment in our list.
     */
    for (seg = seghead; seg; seg = seg->next)
        if (seg->index == segment - 1)
            break;

    if (!seg)
    {
        /*
         * Might be an external with a default WRT.
         */
        int32_t i = segment / 2;
        struct ExtBack *eb = ebhead;
        struct External *e;

        while (i >= EXT_BLKSIZ)
        {
            if (eb)
                eb = eb->next;
            else
                break;
            i -= EXT_BLKSIZ;
        }
        if (eb)
        {
            e = eb->exts[i];
            if (!e)
            {
                as_assert(pass0 == 0);
                /* Not available - can happen during optimization */
                return NO_SEG;
            }

            switch (e->defwrt_type)
            {
            case DEFWRT_NONE:
                return segment; /* fine */
            case DEFWRT_SEGMENT:
                return e->defwrt_ptr.seg->index + 1;
            case DEFWRT_GROUP:
                return e->defwrt_ptr.grp->index + 1;
            default:
                return NO_SEG; /* can't tell what it is */
            }
        }

        return segment; /* not one of ours - leave it alone */
    }

    if (seg->align >= SEG_ABS)
        return seg->align; /* absolute segment */
    if (seg->grp)
        return seg->grp->index + 1; /* grouped segment */

    return segment; /* no special treatment */
}

static void obj_filename(char *inname, char *outname)
{
    strcpy(obj_infile, inname);
    standard_extension(inname, outname, ".obj");
}

static void obj_write_file(int debuginfo)
{
    struct Segment *seg, *entry_seg_ptr = 0;
    struct FileName *fn;
    struct LineNumber *ln;
    struct Group *grp;
    struct Public *pub, *loc;
    struct External *ext;
    struct ImpDef *imp;
    struct ExpDef *export;
    int lname_idx;
    ObjRecord *orp;

    /*
     * Write the THEADR module header.
     */
    orp = obj_new();
    orp->type = THEADR;
    obj_name(orp, obj_infile);
    obj_emit2(orp);

    /*
     * Write the AS boast comment.
     */
    orp->type = COMENT;
    obj_rword(orp, 0); /* comment type zero */
    obj_name(orp, as_comment);
    obj_emit2(orp);

    orp->type = COMENT;
    /*
     * Write the IMPDEF records, if any.
     */
    for (imp = imphead; imp; imp = imp->next)
    {
        obj_rword(orp, 0xA0); /* comment class A0 */
        obj_byte(orp, 1);     /* subfunction 1: IMPDEF */
        if (imp->impname)
            obj_byte(orp, 0); /* import by name */
        else
            obj_byte(orp, 1); /* import by ordinal */
        obj_name(orp, imp->extname);
        obj_name(orp, imp->libname);
        if (imp->impname)
            obj_name(orp, imp->impname);
        else
            obj_word(orp, imp->impindex);
        obj_emit2(orp);
    }

    /*
     * Write the EXPDEF records, if any.
     */
    for (export = exphead; export; export = export->next)
    {
        obj_rword(orp, 0xA0); /* comment class A0 */
        obj_byte(orp, 2);     /* subfunction 2: EXPDEF */
        obj_byte(orp, export->flags);
        obj_name(orp, export->extname);
        obj_name(orp, export->intname);
        if (export->flags & EXPDEF_FLAG_ORDINAL)
            obj_word(orp, export->ordinal);
        obj_emit2(orp);
    }

    /* we're using extended OMF if we put in debug info */
    if (debuginfo)
    {
        orp->type = COMENT;
        obj_byte(orp, 0x40);
        obj_byte(orp, dEXTENDED);
        obj_emit2(orp);
    }

    /*
     * Write the first LNAMES record, containing LNAME one, which
     * is null. Also initialize the LNAME counter.
     */
    orp->type = LNAMES;
    obj_byte(orp, 0);
    lname_idx = 1;
    /*
     * Write some LNAMES for the segment names
     */
    for (seg = seghead; seg; seg = seg->next)
    {
        orp = obj_name(orp, seg->name);
        if (seg->segclass)
            orp = obj_name(orp, seg->segclass);
        if (seg->overlay)
            orp = obj_name(orp, seg->overlay);
        obj_commit(orp);
    }
    /*
     * Write some LNAMES for the group names
     */
    for (grp = grphead; grp; grp = grp->next)
    {
        orp = obj_name(orp, grp->name);
        obj_commit(orp);
    }
    obj_emit(orp);

    /*
     * Write the SEGDEF records.
     */
    orp->type = SEGDEF;
    for (seg = seghead; seg; seg = seg->next)
    {
        int acbp;
        uint32_t seglen = seg->currentpos;

        acbp = (seg->combine << 2); /* C field */

        if (seg->use32)
            acbp |= 0x01; /* P bit is Use32 flag */
        else if (seglen == 0x10000L)
        {
            seglen = 0;   /* This special case may be needed for old linkers */
            acbp |= 0x02; /* B bit */
        }

        /* A field */
        if (seg->align >= SEG_ABS)
            /* acbp |= 0x00 */;
        else if (seg->align >= 4096)
        {
            if (seg->align > 4096)
                as_error(ERR_NONFATAL, "segment `%s' requires more alignment"
                                       " than OBJ format supports",
                         seg->name);
            acbp |= 0xC0; /* PharLap extension */
        }
        else if (seg->align >= 256)
        {
            acbp |= 0x80;
        }
        else if (seg->align >= 16)
        {
            acbp |= 0x60;
        }
        else if (seg->align >= 4)
        {
            acbp |= 0xA0;
        }
        else if (seg->align >= 2)
        {
            acbp |= 0x40;
        }
        else
            acbp |= 0x20;

        obj_byte(orp, acbp);
        if (seg->align & SEG_ABS)
        {
            obj_x(orp, seg->align - SEG_ABS); /* Frame */
            obj_byte(orp, 0);                 /* Offset */
        }
        obj_x(orp, seglen);
        obj_index(orp, ++lname_idx);
        obj_index(orp, seg->segclass ? ++lname_idx : 1);
        obj_index(orp, seg->overlay ? ++lname_idx : 1);
        obj_emit2(orp);
    }

    /*
     * Write the GRPDEF records.
     */
    orp->type = GRPDEF;
    for (grp = grphead; grp; grp = grp->next)
    {
        int i;

        if (grp->nindices != grp->nentries)
        {
            for (i = grp->nindices; i < grp->nentries; i++)
            {
                as_error(ERR_NONFATAL, "group `%s' contains undefined segment"
                                       " `%s'",
                         grp->name, grp->segs[i].name);
                as_free(grp->segs[i].name);
                grp->segs[i].name = NULL;
            }
        }
        obj_index(orp, ++lname_idx);
        for (i = 0; i < grp->nindices; i++)
        {
            obj_byte(orp, 0xFF);
            obj_index(orp, grp->segs[i].index);
        }
        obj_emit2(orp);
    }

    /*
     * Write the PUBDEF records: first the ones in the segments,
     * then the far-absolutes.
     */
    orp->type = PUBDEF;
    orp->ori = ori_pubdef;
    for (seg = seghead; seg; seg = seg->next)
    {
        orp->parm[0] = seg->grp ? seg->grp->obj_index : 0;
        orp->parm[1] = seg->obj_index;
        for (pub = seg->pubhead; pub; pub = pub->next)
        {
            orp = obj_name(orp, pub->name);
            orp = obj_x(orp, pub->offset);
            orp = obj_byte(orp, 0); /* type index */
            obj_commit(orp);
        }
        obj_emit(orp);
    }
    orp->parm[0] = 0;
    orp->parm[1] = 0;
    for (pub = fpubhead; pub; pub = pub->next)
    { /* pub-crawl :-) */
        if (orp->parm[2] != (uint32_t)pub->segment)
        {
            obj_emit(orp);
            orp->parm[2] = pub->segment;
        }
        orp = obj_name(orp, pub->name);
        orp = obj_x(orp, pub->offset);
        orp = obj_byte(orp, 0); /* type index */
        obj_commit(orp);
    }
    obj_emit(orp);

    /*
     * Write the EXTDEF and COMDEF records, in order.
     */
    orp->ori = ori_null;
    for (ext = exthead; ext; ext = ext->next)
    {
        if (ext->commonsize == 0)
        {
            if (orp->type != EXTDEF)
            {
                obj_emit(orp);
                orp->type = EXTDEF;
            }
            orp = obj_name(orp, ext->name);
            orp = obj_index(orp, 0);
        }
        else
        {
            if (orp->type != COMDEF)
            {
                obj_emit(orp);
                orp->type = COMDEF;
            }
            orp = obj_name(orp, ext->name);
            orp = obj_index(orp, 0);
            if (ext->commonelem)
            {
                orp = obj_byte(orp, 0x61); /* far communal */
                orp = obj_value(orp, (ext->commonsize / ext->commonelem));
                orp = obj_value(orp, ext->commonelem);
            }
            else
            {
                orp = obj_byte(orp, 0x62); /* near communal */
                orp = obj_value(orp, ext->commonsize);
            }
        }
        obj_commit(orp);
    }
    obj_emit(orp);

    /*
     * Write a COMENT record stating that the linker's first pass
     * may stop processing at this point. Exception is if our
     * MODEND record specifies a start point, in which case,
     * according to some variants of the documentation, this COMENT
     * should be omitted. So we'll omit it just in case.
     * But, TASM puts it in all the time so if we are using
     * TASM debug stuff we are putting it in
     */
    if (debuginfo || obj_entry_seg == NO_SEG)
    {
        orp->type = COMENT;
        obj_byte(orp, 0x40);
        obj_byte(orp, dLINKPASS);
        obj_byte(orp, 1);
        obj_emit2(orp);
    }

    /*
     * 1) put out the compiler type
     * 2) Put out the type info.  The only type we are using is near label #19
     */
    if (debuginfo)
    {
        int i;
        struct Array *arrtmp = arrhead;
        orp->type = COMENT;
        obj_byte(orp, 0x40);
        obj_byte(orp, dCOMPDEF);
        obj_byte(orp, 4);
        obj_byte(orp, 0);
        obj_emit2(orp);

        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x18); /* type # for linking */
        obj_word(orp, 6);    /* size of type */
        obj_byte(orp, 0x2a); /* absolute type for debugging */
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x19); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x24); /* absolute type for debugging */
        obj_byte(orp, 0);    /* near/far specifier */
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x1A); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x24); /* absolute type for debugging */
        obj_byte(orp, 1);    /* near/far specifier */
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x1b); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x23); /* absolute type for debugging */
        obj_byte(orp, 0);
        obj_byte(orp, 0);
        obj_byte(orp, 0);
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x1c); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x23); /* absolute type for debugging */
        obj_byte(orp, 0);
        obj_byte(orp, 4);
        obj_byte(orp, 0);
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x1d); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x23); /* absolute type for debugging */
        obj_byte(orp, 0);
        obj_byte(orp, 1);
        obj_byte(orp, 0);
        obj_emit2(orp);
        obj_byte(orp, 0x40);
        obj_byte(orp, dTYPEDEF);
        obj_word(orp, 0x1e); /* type # for linking */
        obj_word(orp, 0);    /* size of type */
        obj_byte(orp, 0x23); /* absolute type for debugging */
        obj_byte(orp, 0);
        obj_byte(orp, 5);
        obj_byte(orp, 0);
        obj_emit2(orp);

        /* put out the array types */
        for (i = ARRAYBOT; i < arrindex; i++)
        {
            obj_byte(orp, 0x40);
            obj_byte(orp, dTYPEDEF);
            obj_word(orp, i);                /* type # for linking */
            obj_word(orp, arrtmp->size);     /* size of type */
            obj_byte(orp, 0x1A);             /* absolute type for debugging (array) */
            obj_byte(orp, arrtmp->basetype); /* base type */
            obj_emit2(orp);
            arrtmp = arrtmp->next;
        }
    }
    /*
     * write out line number info with a LINNUM record
     * switch records when we switch segments, and output the
     * file in a pseudo-TASM fashion.  The record switch is naive; that
     * is that one file may have many records for the same segment
     * if there are lots of segment switches
     */
    if (fnhead && debuginfo)
    {
        seg = fnhead->lnhead->segment;

        for (fn = fnhead; fn; fn = fn->next)
        {
            /* write out current file name */
            orp->type = COMENT;
            orp->ori = ori_null;
            obj_byte(orp, 0x40);
            obj_byte(orp, dFILNAME);
            obj_byte(orp, 0);
            obj_name(orp, fn->name);
            obj_dword(orp, 0);
            obj_emit2(orp);

            /* write out line numbers this file */

            orp->type = LINNUM;
            orp->ori = ori_linnum;
            for (ln = fn->lnhead; ln; ln = ln->next)
            {
                if (seg != ln->segment)
                {
                    /* if we get here have to flush the buffer and start
                     * a new record for a new segment
                     */
                    seg = ln->segment;
                    obj_emit(orp);
                }
                orp->parm[0] = seg->grp ? seg->grp->obj_index : 0;
                orp->parm[1] = seg->obj_index;
                orp = obj_word(orp, ln->lineno);
                orp = obj_x(orp, ln->offset);
                obj_commit(orp);
            }
            obj_emit(orp);
        }
    }
    /*
     * we are going to locate the entry point segment now
     * rather than wait until the MODEND record, because,
     * then we can output a special symbol to tell where the
     * entry point is.
     *
     */
    if (obj_entry_seg != NO_SEG)
    {
        for (seg = seghead; seg; seg = seg->next)
        {
            if (seg->index == obj_entry_seg)
            {
                entry_seg_ptr = seg;
                break;
            }
        }
        if (!seg)
            as_error(ERR_NONFATAL, "entry point is not in this module");
    }

    /*
     * get ready to put out symbol records
     */
    orp->type = COMENT;
    orp->ori = ori_local;

    /*
     * put out a symbol for the entry point
     * no dots in this symbol, because, borland does
     * not (officially) support dots in label names
     * and I don't know what various versions of TLINK will do
     */
    if (debuginfo && obj_entry_seg != NO_SEG)
    {
        orp = obj_name(orp, "start_of_program");
        orp = obj_word(orp, 0x19); /* type: near label */
        orp = obj_index(orp, seg->grp ? seg->grp->obj_index : 0);
        orp = obj_index(orp, seg->obj_index);
        orp = obj_x(orp, obj_entry_ofs);
        obj_commit(orp);
    }

    /*
     * put out the local labels
     */
    for (seg = seghead; seg && debuginfo; seg = seg->next)
    {
        /* labels this seg */
        for (loc = seg->lochead; loc; loc = loc->next)
        {
            orp = obj_name(orp, loc->name);
            orp = obj_word(orp, loc->type);
            orp = obj_index(orp, seg->grp ? seg->grp->obj_index : 0);
            orp = obj_index(orp, seg->obj_index);
            orp = obj_x(orp, loc->offset);
            obj_commit(orp);
        }
    }
    if (orp->used)
        obj_emit(orp);

    /*
     * Write the LEDATA/FIXUPP pairs.
     */
    for (seg = seghead; seg; seg = seg->next)
    {
        obj_emit(seg->orp);
        as_free(seg->orp);
    }

    /*
     * Write the MODEND module end marker.
     */
    orp->type = obj_use32 ? MODE32 : MODEND;
    orp->ori = ori_null;
    if (entry_seg_ptr)
    {
        orp->type = entry_seg_ptr->use32 ? MODE32 : MODEND;
        obj_byte(orp, 0xC1);
        seg = entry_seg_ptr;
        if (seg->grp)
        {
            obj_byte(orp, 0x10);
            obj_index(orp, seg->grp->obj_index);
        }
        else
        {
            /*
             * the below changed to prevent TLINK crashing.
             * Previous more efficient version read:
             *
             *  obj_byte (orp, 0x50);
             */
            obj_byte(orp, 0x00);
            obj_index(orp, seg->obj_index);
        }
        obj_index(orp, seg->obj_index);
        obj_x(orp, obj_entry_ofs);
    }
    else
        obj_byte(orp, 0);
    obj_emit2(orp);
    as_free(orp);
}

static void obj_fwrite(ObjRecord *orp)
{
    unsigned int cksum, len;
    uint8_t *ptr;

    cksum = orp->type;
    if (orp->x_size == 32)
        cksum |= 1;
    fputc(cksum, ofile);
    len = orp->committed + 1;
    cksum += (len & 0xFF) + ((len >> 8) & 0xFF);
    fwriteint16_t(len, ofile);
    fwrite(orp->buf, 1, len - 1, ofile);
    for (ptr = orp->buf; --len; ptr++)
        cksum += *ptr;
    fputc((-cksum) & 0xFF, ofile);
}

extern macros_t obj_stdmac[];

void dbgbi_init(void)
{
    fnhead = NULL;
    fntail = &fnhead;
    arrindex = ARRAYBOT;
    arrhead = NULL;
    arrtail = &arrhead;
}
static void dbgbi_cleanup(void)
{
    struct Segment *segtmp;
    while (fnhead)
    {
        struct FileName *fntemp = fnhead;
        while (fnhead->lnhead)
        {
            struct LineNumber *lntemp = fnhead->lnhead;
            fnhead->lnhead = lntemp->next;
            as_free(lntemp);
        }
        fnhead = fnhead->next;
        as_free(fntemp->name);
        as_free(fntemp);
    }
    for (segtmp = seghead; segtmp; segtmp = segtmp->next)
    {
        while (segtmp->lochead)
        {
            struct Public *loctmp = segtmp->lochead;
            segtmp->lochead = loctmp->next;
            as_free(loctmp->name);
            as_free(loctmp);
        }
    }
    while (arrhead)
    {
        struct Array *arrtmp = arrhead;
        arrhead = arrhead->next;
        as_free(arrtmp);
    }
}

static void dbgbi_linnum(const char *lnfname, int32_t lineno, int32_t segto)
{
    struct FileName *fn;
    struct LineNumber *ln;
    struct Segment *seg;

    if (segto == NO_SEG)
        return;

    /*
     * If `any_segs' is still false, we must define a default
     * segment.
     */
    if (!any_segs)
    {
        int tempint; /* ignored */
        if (segto != obj_segment("__ASDEFSEG", 2, &tempint))
            as_error(ERR_PANIC, "strange segment conditions in OBJ driver");
    }

    /*
     * Find the segment we are targetting.
     */
    for (seg = seghead; seg; seg = seg->next)
        if (seg->index == segto)
            break;
    if (!seg)
        as_error(ERR_PANIC, "lineno directed to nonexistent segment?");

    /*    for (fn = fnhead; fn; fn = fnhead->next) */
    for (fn = fnhead; fn; fn = fn->next) /* fbk - Austin Lunnen - John Fine */
        if (!as_stricmp(lnfname, fn->name))
            break;
    if (!fn)
    {
        fn = as_malloc(sizeof(*fn));
        fn->name = as_malloc(strlen(lnfname) + 1);
        strcpy(fn->name, lnfname);
        fn->lnhead = NULL;
        fn->lntail = &fn->lnhead;
        fn->next = NULL;
        *fntail = fn;
        fntail = &fn->next;
    }
    ln = as_malloc(sizeof(*ln));
    ln->segment = seg;
    ln->offset = seg->currentpos;
    ln->lineno = lineno;
    ln->next = NULL;
    *fn->lntail = ln;
    fn->lntail = &ln->next;
}
static void dbgbi_deflabel(char *name, int32_t segment,
                           int64_t offset, int is_global, char *special)
{
    struct Segment *seg;

    (void)special;

    /*
     * If it's a special-retry from pass two, discard it.
     */
    if (is_global == 3)
        return;

    /*
     * First check for the double-period, signifying something
     * unusual.
     */
    if (name[0] == '.' && name[1] == '.' && name[2] != '@')
    {
        return;
    }

    /*
     * Case (i):
     */
    if (obj_seg_needs_update)
    {
        return;
    }
    else if (obj_grp_needs_update)
    {
        return;
    }
    if (segment < SEG_ABS && segment != NO_SEG && segment % 2)
        return;

    if (segment >= SEG_ABS || segment == NO_SEG)
    {
        return;
    }

    /*
     * If `any_segs' is still false, we might need to define a
     * default segment, if they're trying to declare a label in
     * `first_seg'.  But the label should exist due to a prior
     * call to obj_deflabel so we can skip that.
     */

    for (seg = seghead; seg; seg = seg->next)
        if (seg->index == segment)
        {
            struct Public *loc = as_malloc(sizeof(*loc));
            /*
             * Case (ii). Maybe MODPUB someday?
             */
            last_defined = *seg->loctail = loc;
            seg->loctail = &loc->next;
            loc->next = NULL;
            loc->name = as_strdup(name);
            loc->offset = offset;
        }
}
static void dbgbi_typevalue(int32_t type)
{
    int vsize;
    int elem = TYM_ELEMENTS(type);
    type = TYM_TYPE(type);

    if (!last_defined)
        return;

    switch (type)
    {
    case TY_BYTE:
        last_defined->type = 8; /* uint8_t */
        vsize = 1;
        break;
    case TY_WORD:
        last_defined->type = 10; /* unsigned word */
        vsize = 2;
        break;
    case TY_DWORD:
        last_defined->type = 12; /* unsigned dword */
        vsize = 4;
        break;
    case TY_FLOAT:
        last_defined->type = 14; /* float */
        vsize = 4;
        break;
    case TY_QWORD:
        last_defined->type = 15; /* qword */
        vsize = 8;
        break;
    case TY_TBYTE:
        last_defined->type = 16; /* TBYTE */
        vsize = 10;
        break;
    default:
        last_defined->type = 0x19; /*label */
        vsize = 0;
        break;
    }

    if (elem > 1)
    {
        struct Array *arrtmp = as_malloc(sizeof(*arrtmp));
        int vtype = last_defined->type;
        arrtmp->size = vsize * elem;
        arrtmp->basetype = vtype;
        arrtmp->next = NULL;
        last_defined->type = arrindex++;
        *arrtail = arrtmp;
        arrtail = &(arrtmp->next);
    }
    last_defined = NULL;
}
static void dbgbi_output(int output_type, void *param)
{
    (void)output_type;
    (void)param;
}
static struct dfmt borland_debug_form = {
    "Borland Debug Records",
    "borland",
    dbgbi_init,
    dbgbi_linnum,
    dbgbi_deflabel,
    null_debug_directive,
    dbgbi_typevalue,
    dbgbi_output,
    dbgbi_cleanup,
};

static struct dfmt *borland_debug_arr[3] = {
    &borland_debug_form,
    &null_debug_form,
    NULL};

struct ofmt of_obj = {
    "MS-DOS 16-bit/32-bit OMF object files",
    "obj",
    0,
    borland_debug_arr,
    &borland_debug_form,
    obj_stdmac,
    obj_init,
    obj_set_info,
    obj_out,
    obj_deflabel,
    obj_segment,
    obj_sectalign,
    obj_segbase,
    obj_directive,
    obj_filename,
    obj_cleanup};
#endif /* OF_OBJ */
