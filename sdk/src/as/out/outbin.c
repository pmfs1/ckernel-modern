/*
 * outbin.c output routines for the assembler to produce
 *    flat-form binary files
 */

/* This is the extended version of AS's original binary output
 * format.  It is backward compatible with the original BIN format,
 * and contains support for multiple sections and advanced section
 * ordering.
 *
 * Feature summary:
 *
 * - Users can create an arbitrary number of sections; they are not
 *   limited to just ".text", ".data", and ".bss".
 *
 * - Sections can be either progbits or nobits type.
 *
 * - You can specify that they be aligned at a certian boundary
 *   following the previous section ("align="), or positioned at an
 *   arbitrary byte-granular location ("start=").
 *
 * - You can specify a "virtual" start address for a section, which
 *   will be used for the calculation for all address references
 *   with respect to that section ("vstart=").
 *
 * - The ORG directive, as well as the section/segment directive
 *   arguments ("align=", "start=", "vstart="), can take a critical
 *   expression as their value.  For example: "align=(1 << 12)".
 *
 * - You can generate map files using the 'map' directive.
 *
 */

/* Uncomment the following define if you want sections to adapt
 * their progbits/nobits state depending on what type of
 * instructions are issued, rather than defaulting to progbits.
 * Note that this behavior violates the specification.

#define ABIN_SMART_ADAPT

*/

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "as.h"
#include "aslib.h"
#include "saa.h"
#include "stdscan.h"
#include "labels.h"
#include "eval.h"
#include "out/outform.h"
#include "out/outlib.h"

#ifdef OF_BIN

static FILE *rf = NULL;
static void (*do_output)(void);

/* Section flags keep track of which attributes the user has defined. */
#define START_DEFINED 0x001
#define ALIGN_DEFINED 0x002
#define FOLLOWS_DEFINED 0x004
#define VSTART_DEFINED 0x008
#define VALIGN_DEFINED 0x010
#define VFOLLOWS_DEFINED 0x020
#define TYPE_DEFINED 0x040
#define TYPE_PROGBITS 0x080
#define TYPE_NOBITS 0x100

/* This struct is used to keep track of symbols for map-file generation. */
static struct bin_label
{
    char *name;
    struct bin_label *next;
} * no_seg_labels, **nsl_tail;

static struct Section
{
    char *name;
    struct SAA *contents;
    int64_t length; /* section length in bytes */

    /* Section attributes */
    int flags;            /* see flag definitions above */
    uint64_t align;       /* section alignment */
    uint64_t valign;      /* notional section alignment */
    uint64_t start;       /* section start address */
    uint64_t vstart;      /* section virtual start address */
    char *follows;        /* the section that this one will follow */
    char *vfollows;       /* the section that this one will notionally follow */
    int32_t start_index;  /* AS section id for non-relocated version */
    int32_t vstart_index; /* the AS section id */

    struct bin_label *labels;      /* linked-list of label handles for map output. */
    struct bin_label **labels_end; /* Holds address of end of labels list. */
    struct Section *prev;          /* Points to previous section (implicit follows). */
    struct Section *next;          /* This links sections with a defined start address. */

    /* The extended bin format allows for sections to have a "virtual"
     * start address.  This is accomplished by creating two sections:
     * one beginning at the Load Memory Address and the other beginning
     * at the Virtual Memory Address.  The LMA section is only used to
     * define the section.<section_name>.start label, but there isn't
     * any other good way for us to handle that label.
     */

} * sections, *last_section;

static struct Reloc
{
    struct Reloc *next;
    int32_t posn;
    int32_t bytes;
    int32_t secref;
    int32_t secrel;
    struct Section *target;
} * relocs, **reloctail;

static uint64_t origin;
static int origin_defined;

/* Stuff we need for map-file generation. */
#define MAP_ORIGIN 1
#define MAP_SUMMARY 2
#define MAP_SECTIONS 4
#define MAP_SYMBOLS 8
static int map_control = 0;
static char *infile, *outfile;

extern macros_t bin_stdmac[];

static void add_reloc(struct Section *s, int32_t bytes, int32_t secref,
                      int32_t secrel)
{
    struct Reloc *r;

    r = *reloctail = as_malloc(sizeof(struct Reloc));
    reloctail = &r->next;
    r->next = NULL;
    r->posn = s->length;
    r->bytes = bytes;
    r->secref = secref;
    r->secrel = secrel;
    r->target = s;
}

static struct Section *find_section_by_name(const char *name)
{
    struct Section *s;

    list_for_each(s, sections) if (!strcmp(s->name, name)) break;
    return s;
}

static struct Section *find_section_by_index(int32_t index)
{
    struct Section *s;

    list_for_each(s, sections) if ((index == s->vstart_index) || (index == s->start_index)) break;
    return s;
}

static struct Section *create_section(char *name)
{
    struct Section *s = as_zalloc(sizeof(*s));

    s->prev = last_section;
    s->name = as_strdup(name);
    s->labels_end = &(s->labels);
    s->contents = saa_init(1L);

    /* Register our sections with AS. */
    s->vstart_index = seg_alloc();
    s->start_index = seg_alloc();

    /* TODO: Append to a tail, we need some helper */
    last_section->next = s;
    last_section = s;

    return last_section;
}

static void bin_cleanup(int debuginfo)
{
    struct Section *g, **gp;
    struct Section *gs = NULL, **gsp;
    struct Section *s, **sp;
    struct Section *nobits = NULL, **nt;
    struct Section *last_progbits;
    struct bin_label *l;
    struct Reloc *r;
    uint64_t pend;
    int h;

    (void)debuginfo; /* placate optimizers */

#ifdef DEBUG
    as_error(ERR_DEBUG,
             "bin_cleanup: Sections were initially referenced in this order:\n");
    for (h = 0, s = sections; s; h++, s = s->next)
        fprintf(stdout, "%i. %s\n", h, s->name);
#endif

    /* Assembly has completed, so now we need to generate the output file.
     * Step 1: Separate progbits and nobits sections into separate lists.
     * Step 2: Sort the progbits sections into their output order.
     * Step 3: Compute start addresses for all progbits sections.
     * Step 4: Compute vstart addresses for all sections.
     * Step 5: Apply relocations.
     * Step 6: Write the sections' data to the output file.
     * Step 7: Generate the map file.
     * Step 8: Release all allocated memory.
     */

    /* To do: Smart section-type adaptation could leave some empty sections
     * without a defined type (progbits/nobits).  Won't fix now since this
     * feature will be disabled.  */

    /* Step 1: Split progbits and nobits sections into separate lists. */

    nt = &nobits;
    /* Move nobits sections into a separate list.  Also pre-process nobits
     * sections' attributes. */
    for (sp = &sections->next, s = sections->next; s; s = *sp)
    { /* Skip progbits sections. */
        if (s->flags & TYPE_PROGBITS)
        {
            sp = &s->next;
            continue;
        }
        /* Do some special pre-processing on nobits sections' attributes. */
        if (s->flags & (START_DEFINED | ALIGN_DEFINED | FOLLOWS_DEFINED))
        { /* Check for a mixture of real and virtual section attributes. */
            if (s->flags & (VSTART_DEFINED | VALIGN_DEFINED |
                            VFOLLOWS_DEFINED))
                as_error(ERR_FATAL | ERR_NOFILE,
                         "cannot mix real and virtual attributes"
                         " in nobits section (%s)",
                         s->name);
            /* Real and virtual attributes mean the same thing for nobits sections. */
            if (s->flags & START_DEFINED)
            {
                s->vstart = s->start;
                s->flags |= VSTART_DEFINED;
            }
            if (s->flags & ALIGN_DEFINED)
            {
                s->valign = s->align;
                s->flags |= VALIGN_DEFINED;
            }
            if (s->flags & FOLLOWS_DEFINED)
            {
                s->vfollows = s->follows;
                s->flags |= VFOLLOWS_DEFINED;
                s->flags &= ~FOLLOWS_DEFINED;
            }
        }
        /* Every section must have a start address. */
        if (s->flags & VSTART_DEFINED)
        {
            s->start = s->vstart;
            s->flags |= START_DEFINED;
        }
        /* Move the section into the nobits list. */
        *sp = s->next;
        s->next = NULL;
        *nt = s;
        nt = &s->next;
    }

    /* Step 2: Sort the progbits sections into their output order. */

    /* In Step 2 we move around sections in groups.  A group
     * begins with a section (group leader) that has a user-
     * defined start address or follows section.  The remainder
     * of the group is made up of the sections that implicitly
     * follow the group leader (i.e., they were defined after
     * the group leader and were not given an explicit start
     * address or follows section by the user). */

    /* For anyone attempting to read this code:
     * g (group) points to a group of sections, the first one of which has
     *   a user-defined start address or follows section.
     * gp (g previous) holds the location of the pointer to g.
     * gs (g scan) is a temp variable that we use to scan to the end of the group.
     * gsp (gs previous) holds the location of the pointer to gs.
     * nt (nobits tail) points to the nobits section-list tail.
     */

    /* Link all 'follows' groups to their proper position.  To do
     * this we need to know three things: the start of the group
     * to relocate (g), the section it is following (s), and the
     * end of the group we're relocating (gs). */
    for (gp = &sections, g = sections; g; g = gs)
    { /* Find the next follows group that is out of place (g). */
        if (!(g->flags & FOLLOWS_DEFINED))
        {
            while (g->next)
            {
                if ((g->next->flags & FOLLOWS_DEFINED) &&
                    strcmp(g->name, g->next->follows))
                    break;
                g = g->next;
            }
            if (!g->next)
                break;
            gp = &g->next;
            g = g->next;
        }
        /* Find the section that this group follows (s). */
        for (sp = &sections, s = sections;
             s && strcmp(s->name, g->follows);
             sp = &s->next, s = s->next)
            ;
        if (!s)
            as_error(ERR_FATAL | ERR_NOFILE, "section %s follows an invalid or"
                                             " unknown section (%s)",
                     g->name, g->follows);
        if (s->next && (s->next->flags & FOLLOWS_DEFINED) &&
            !strcmp(s->name, s->next->follows))
            as_error(ERR_FATAL | ERR_NOFILE, "sections %s and %s can't both follow"
                                             " section %s",
                     g->name, s->next->name, s->name);
        /* Find the end of the current follows group (gs). */
        for (gsp = &g->next, gs = g->next;
             gs && (gs != s) && !(gs->flags & START_DEFINED);
             gsp = &gs->next, gs = gs->next)
        {
            if (gs->next && (gs->next->flags & FOLLOWS_DEFINED) &&
                strcmp(gs->name, gs->next->follows))
            {
                gsp = &gs->next;
                gs = gs->next;
                break;
            }
        }
        /* Re-link the group after its follows section. */
        *gsp = s->next;
        s->next = g;
        *gp = gs;
    }

    /* Link all 'start' groups to their proper position.  Once
     * again we need to know g, s, and gs (see above).  The main
     * difference is we already know g since we sort by moving
     * groups from the 'unsorted' list into a 'sorted' list (g
     * will always be the first section in the unsorted list). */
    for (g = sections, sections = NULL; g; g = gs)
    { /* Find the section that we will insert this group before (s). */
        for (sp = &sections, s = sections; s; sp = &s->next, s = s->next)
            if ((s->flags & START_DEFINED) && (g->start < s->start))
                break;
        /* Find the end of the group (gs). */
        for (gs = g->next, gsp = &g->next;
             gs && !(gs->flags & START_DEFINED);
             gsp = &gs->next, gs = gs->next)
            ;
        /* Re-link the group before the target section. */
        *sp = g;
        *gsp = s;
    }

    /* Step 3: Compute start addresses for all progbits sections. */

    /* Make sure we have an origin and a start address for the first section. */
    if (origin_defined)
    {
        if (sections->flags & START_DEFINED)
        {
            /* Make sure this section doesn't begin before the origin. */
            if (sections->start < origin)
                as_error(ERR_FATAL | ERR_NOFILE, "section %s begins"
                                                 " before program origin",
                         sections->name);
        }
        else if (sections->flags & ALIGN_DEFINED)
        {
            sections->start = ALIGN(origin, sections->align);
        }
        else
        {
            sections->start = origin;
        }
    }
    else
    {
        if (!(sections->flags & START_DEFINED))
            sections->start = 0;
        origin = sections->start;
    }
    sections->flags |= START_DEFINED;

    /* Make sure each section has an explicit start address.  If it
     * doesn't, then compute one based its alignment and the end of
     * the previous section. */
    for (pend = sections->start, g = s = sections; g; g = g->next)
    { /* Find the next section that could cause an overlap situation
       * (has a defined start address, and is not zero length). */
        if (g == s)
            for (s = g->next;
                 s && ((s->length == 0) || !(s->flags & START_DEFINED));
                 s = s->next)
                ;
        /* Compute the start address of this section, if necessary. */
        if (!(g->flags & START_DEFINED))
        { /* Default to an alignment of 4 if unspecified. */
            if (!(g->flags & ALIGN_DEFINED))
            {
                g->align = 4;
                g->flags |= ALIGN_DEFINED;
            }
            /* Set the section start address. */
            g->start = ALIGN(pend, g->align);
            g->flags |= START_DEFINED;
        }
        /* Ugly special case for progbits sections' virtual attributes:
         *   If there is a defined valign, but no vstart and no vfollows, then
         *   we valign after the previous progbits section.  This case doesn't
         *   really make much sense for progbits sections with a defined start
         *   address, but it is possible and we must do *something*.
         * Not-so-ugly special case:
         *   If a progbits section has no virtual attributes, we set the
         *   vstart equal to the start address.  */
        if (!(g->flags & (VSTART_DEFINED | VFOLLOWS_DEFINED)))
        {
            if (g->flags & VALIGN_DEFINED)
                g->vstart = ALIGN(pend, g->valign);
            else
                g->vstart = g->start;
            g->flags |= VSTART_DEFINED;
        }
        /* Ignore zero-length sections. */
        if (g->start < pend)
            continue;
        /* Compute the span of this section. */
        pend = g->start + g->length;
        /* Check for section overlap. */
        if (s)
        {
            if (s->start < origin)
                as_error(ERR_FATAL | ERR_NOFILE, "section %s beings before program origin",
                         s->name);
            if (g->start > s->start)
                as_error(ERR_FATAL | ERR_NOFILE, "sections %s ~ %s and %s overlap!",
                         gs->name, g->name, s->name);
            if (pend > s->start)
                as_error(ERR_FATAL | ERR_NOFILE, "sections %s and %s overlap!",
                         g->name, s->name);
        }
        /* Remember this section as the latest >0 length section. */
        gs = g;
    }

    /* Step 4: Compute vstart addresses for all sections. */

    /* Attach the nobits sections to the end of the progbits sections. */
    for (s = sections; s->next; s = s->next)
        ;
    s->next = nobits;
    last_progbits = s;
    /*
     * Scan for sections that don't have a vstart address.  If we find
     * one we'll attempt to compute its vstart.  If we can't compute
     * the vstart, we leave it alone and come back to it in a
     * subsequent scan.  We continue scanning and re-scanning until
     * we've gone one full cycle without computing any vstarts.
     */
    do
    { /* Do one full scan of the sections list. */
        for (h = 0, g = sections; g; g = g->next)
        {
            if (g->flags & VSTART_DEFINED)
                continue;
            /* Find the section that this one virtually follows.  */
            if (g->flags & VFOLLOWS_DEFINED)
            {
                for (s = sections; s && strcmp(g->vfollows, s->name);
                     s = s->next)
                    ;
                if (!s)
                    as_error(ERR_FATAL | ERR_NOFILE,
                             "section %s vfollows unknown section (%s)",
                             g->name, g->vfollows);
            }
            else if (g->prev != NULL)
                for (s = sections; s && (s != g->prev); s = s->next)
                    ;
            /* The .bss section is the only one with prev = NULL.
           In this case we implicitly follow the last progbits
           section.  */
            else
                s = last_progbits;

            /* If the section we're following has a vstart, we can proceed. */
            if (s->flags & VSTART_DEFINED)
            { /* Default to virtual alignment of four. */
                if (!(g->flags & VALIGN_DEFINED))
                {
                    g->valign = 4;
                    g->flags |= VALIGN_DEFINED;
                }
                /* Compute the vstart address. */
                g->vstart = ALIGN(s->vstart + s->length, g->valign);
                g->flags |= VSTART_DEFINED;
                h++;
                /* Start and vstart mean the same thing for nobits sections. */
                if (g->flags & TYPE_NOBITS)
                    g->start = g->vstart;
            }
        }
    } while (h);

    /* Now check for any circular vfollows references, which will manifest
     * themselves as sections without a defined vstart. */
    for (h = 0, s = sections; s; s = s->next)
    {
        if (!(s->flags & VSTART_DEFINED))
        { /* Non-fatal errors after assembly has completed are generally a
           * no-no, but we'll throw a fatal one eventually so it's ok.  */
            as_error(ERR_NONFATAL, "cannot compute vstart for section %s",
                     s->name);
            h++;
        }
    }
    if (h)
        as_error(ERR_FATAL | ERR_NOFILE, "circular vfollows path detected");

#ifdef DEBUG
    as_error(ERR_DEBUG,
             "bin_cleanup: Confirm final section order for output file:\n");
    for (h = 0, s = sections; s && (s->flags & TYPE_PROGBITS);
         h++, s = s->next)
        fprintf(stdout, "%i. %s\n", h, s->name);
#endif

    /* Step 5: Apply relocations. */

    /* Prepare the sections for relocating. */
    list_for_each(s, sections)
        saa_rewind(s->contents);
    /* Apply relocations. */
    list_for_each(r, relocs)
    {
        uint8_t *p, mydata[8];
        int64_t l2;
        int b;

        as_assert(r->bytes <= 8);

        saa_fread(r->target->contents, r->posn, mydata, r->bytes);
        p = mydata;
        l2 = 0;
        for (b = r->bytes - 1; b >= 0; b--)
            l2 = (l2 << 8) + mydata[b];

        s = find_section_by_index(r->secref);
        if (s)
        {
            if (r->secref == s->start_index)
                l2 += s->start;
            else
                l2 += s->vstart;
        }
        s = find_section_by_index(r->secrel);
        if (s)
        {
            if (r->secrel == s->start_index)
                l2 -= s->start;
            else
                l2 -= s->vstart;
        }

        WRITEADDR(p, l2, r->bytes);
        saa_fwrite(r->target->contents, r->posn, mydata, r->bytes);
    }

    /* Step 6: Write the section data to the output file. */
    do_output();

    /* Step 7: Generate the map file. */

    if (map_control)
    {
        static const char not_defined[] = "not defined";

        /* Display input and output file names. */
        fprintf(rf, "\n- AS Map file ");
        for (h = 63; h; h--)
            fputc('-', rf);
        fprintf(rf, "\n\nSource file:  %s\nOutput file:  %s\n\n",
                infile, outfile);

        if (map_control & MAP_ORIGIN)
        { /* Display program origin. */
            fprintf(rf, "-- Program origin ");
            for (h = 61; h; h--)
                fputc('-', rf);
            fprintf(rf, "\n\n%08" PRIX64 "\n\n", origin);
        }
        /* Display sections summary. */
        if (map_control & MAP_SUMMARY)
        {
            fprintf(rf, "-- Sections (summary) ");
            for (h = 57; h; h--)
                fputc('-', rf);
            fprintf(rf, "\n\nVstart            Start             Stop              "
                        "Length    Class     Name\n");
            list_for_each(s, sections)
            {
                fprintf(rf, "%16" PRIX64 "  %16" PRIX64 "  %16" PRIX64 "  %08" PRIX64 "  ",
                        s->vstart, s->start, s->start + s->length,
                        s->length);
                if (s->flags & TYPE_PROGBITS)
                    fprintf(rf, "progbits  ");
                else
                    fprintf(rf, "nobits    ");
                fprintf(rf, "%s\n", s->name);
            }
            fprintf(rf, "\n");
        }
        /* Display detailed section information. */
        if (map_control & MAP_SECTIONS)
        {
            fprintf(rf, "-- Sections (detailed) ");
            for (h = 56; h; h--)
                fputc('-', rf);
            fprintf(rf, "\n\n");
            list_for_each(s, sections)
            {
                fprintf(rf, "---- Section %s ", s->name);
                for (h = 65 - strlen(s->name); h; h--)
                    fputc('-', rf);
                fprintf(rf, "\n\nclass:     ");
                if (s->flags & TYPE_PROGBITS)
                    fprintf(rf, "progbits");
                else
                    fprintf(rf, "nobits");
                fprintf(rf, "\nlength:    %16" PRIX64 "\nstart:     %16" PRIX64 ""
                            "\nalign:     ",
                        s->length, s->start);
                if (s->flags & ALIGN_DEFINED)
                    fprintf(rf, "%16" PRIX64 "", s->align);
                else
                    fputs(not_defined, rf);
                fprintf(rf, "\nfollows:   ");
                if (s->flags & FOLLOWS_DEFINED)
                    fprintf(rf, "%s", s->follows);
                else
                    fputs(not_defined, rf);
                fprintf(rf, "\nvstart:    %16" PRIX64 "\nvalign:    ", s->vstart);
                if (s->flags & VALIGN_DEFINED)
                    fprintf(rf, "%16" PRIX64 "", s->valign);
                else
                    fputs(not_defined, rf);
                fprintf(rf, "\nvfollows:  ");
                if (s->flags & VFOLLOWS_DEFINED)
                    fprintf(rf, "%s", s->vfollows);
                else
                    fputs(not_defined, rf);
                fprintf(rf, "\n\n");
            }
        }
        /* Display symbols information. */
        if (map_control & MAP_SYMBOLS)
        {
            int32_t segment;
            int64_t offset;

            fprintf(rf, "-- Symbols ");
            for (h = 68; h; h--)
                fputc('-', rf);
            fprintf(rf, "\n\n");
            if (no_seg_labels)
            {
                fprintf(rf, "---- No Section ");
                for (h = 63; h; h--)
                    fputc('-', rf);
                fprintf(rf, "\n\nValue     Name\n");
                list_for_each(l, no_seg_labels)
                {
                    lookup_label(l->name, &segment, &offset);
                    fprintf(rf, "%08" PRIX64 "  %s\n", offset, l->name);
                }
                fprintf(rf, "\n\n");
            }
            list_for_each(s, sections)
            {
                if (s->labels)
                {
                    fprintf(rf, "---- Section %s ", s->name);
                    for (h = 65 - strlen(s->name); h; h--)
                        fputc('-', rf);
                    fprintf(rf, "\n\nReal              Virtual           Name\n");
                    list_for_each(l, s->labels)
                    {
                        lookup_label(l->name, &segment, &offset);
                        fprintf(rf, "%16" PRIX64 "  %16" PRIX64 "  %s\n",
                                s->start + offset, s->vstart + offset,
                                l->name);
                    }
                    fprintf(rf, "\n");
                }
            }
        }
    }

    /* Close the report file. */
    if (map_control && (rf != stdout) && (rf != stderr))
        fclose(rf);

    /* Step 8: Release all allocated memory. */

    /* Free sections, label pointer structs, etc.. */
    while (sections)
    {
        s = sections;
        sections = s->next;
        saa_free(s->contents);
        as_free(s->name);
        if (s->flags & FOLLOWS_DEFINED)
            as_free(s->follows);
        if (s->flags & VFOLLOWS_DEFINED)
            as_free(s->vfollows);
        while (s->labels)
        {
            l = s->labels;
            s->labels = l->next;
            as_free(l);
        }
        as_free(s);
    }

    /* Free no-section labels. */
    while (no_seg_labels)
    {
        l = no_seg_labels;
        no_seg_labels = l->next;
        as_free(l);
    }

    /* Free relocation structures. */
    while (relocs)
    {
        r = relocs->next;
        as_free(relocs);
        relocs = r;
    }
}

static void bin_out(int32_t segto, const void *data,
                    enum out_type type, uint64_t size,
                    int32_t segment, int32_t wrt)
{
    uint8_t *p, mydata[8];
    struct Section *s;

    if (wrt != NO_SEG)
    {
        wrt = NO_SEG; /* continue to do _something_ */
        as_error(ERR_NONFATAL, "WRT not supported by binary output format");
    }

    /* Handle absolute-assembly (structure definitions). */
    if (segto == NO_SEG)
    {
        if (type != OUT_RESERVE)
            as_error(ERR_NONFATAL, "attempt to assemble code in"
                                   " [ABSOLUTE] space");
        return;
    }

    /* Find the segment we are targeting. */
    s = find_section_by_index(segto);
    if (!s)
        as_error(ERR_PANIC, "code directed to nonexistent segment?");

    /* "Smart" section-type adaptation code. */
    if (!(s->flags & TYPE_DEFINED))
    {
        if (type == OUT_RESERVE)
            s->flags |= TYPE_DEFINED | TYPE_NOBITS;
        else
            s->flags |= TYPE_DEFINED | TYPE_PROGBITS;
    }

    if ((s->flags & TYPE_NOBITS) && (type != OUT_RESERVE))
        as_error(ERR_WARNING, "attempt to initialize memory in a"
                              " nobits section: ignored");

    switch (type)
    {
    case OUT_ADDRESS:
        if (segment != NO_SEG && !find_section_by_index(segment))
        {
            if (segment % 2)
                as_error(ERR_NONFATAL, "binary output format does not support"
                                       " segment base references");
            else
                as_error(ERR_NONFATAL, "binary output format does not support"
                                       " external references");
            segment = NO_SEG;
        }
        if (s->flags & TYPE_PROGBITS)
        {
            if (segment != NO_SEG)
                add_reloc(s, size, segment, -1L);
            p = mydata;
            WRITEADDR(p, *(int64_t *)data, size);
            saa_wbytes(s->contents, mydata, size);
        }
        break;

    case OUT_RAWDATA:
        if (s->flags & TYPE_PROGBITS)
            saa_wbytes(s->contents, data, size);
        break;

    case OUT_RESERVE:
        if (s->flags & TYPE_PROGBITS)
        {
            as_error(ERR_WARNING, "uninitialized space declared in"
                                  " %s section: zeroing",
                     s->name);
            saa_wbytes(s->contents, NULL, size);
        }
        break;

    case OUT_REL1ADR:
    case OUT_REL2ADR:
    case OUT_REL4ADR:
    case OUT_REL8ADR:
    {
        int64_t addr = *(int64_t *)data - size;
        size = realsize(type, size);
        if (segment != NO_SEG && !find_section_by_index(segment))
        {
            if (segment % 2)
                as_error(ERR_NONFATAL, "binary output format does not support"
                                       " segment base references");
            else
                as_error(ERR_NONFATAL, "binary output format does not support"
                                       " external references");
            segment = NO_SEG;
        }
        if (s->flags & TYPE_PROGBITS)
        {
            add_reloc(s, size, segment, segto);
            p = mydata;
            WRITEADDR(p, addr - s->length, size);
            saa_wbytes(s->contents, mydata, size);
        }
        break;
    }

    default:
        as_error(ERR_NONFATAL, "unsupported relocation type %d\n", type);
        break;
    }

    s->length += size;
}

static void bin_deflabel(char *name, int32_t segment, int64_t offset,
                         int is_global, char *special)
{
    (void)segment; /* Don't warn that this parameter is unused */
    (void)offset;  /* Don't warn that this parameter is unused */

    if (special)
        as_error(ERR_NONFATAL, "binary format does not support any"
                               " special symbol types");
    else if (name[0] == '.' && name[1] == '.' && name[2] != '@')
        as_error(ERR_NONFATAL, "unrecognised special symbol `%s'", name);
    else if (is_global == 2)
        as_error(ERR_NONFATAL, "binary output format does not support common"
                               " variables");
    else
    {
        struct Section *s;
        struct bin_label ***ltp;

        /* Remember label definition so we can look it up later when
         * creating the map file. */
        s = find_section_by_index(segment);
        if (s)
            ltp = &(s->labels_end);
        else
            ltp = &nsl_tail;
        (**ltp) = as_malloc(sizeof(struct bin_label));
        (**ltp)->name = name;
        (**ltp)->next = NULL;
        *ltp = &((**ltp)->next);
    }
}

/* These constants and the following function are used
 * by bin_secname() to parse attribute assignments. */

enum
{
    ATTRIB_START,
    ATTRIB_ALIGN,
    ATTRIB_FOLLOWS,
    ATTRIB_VSTART,
    ATTRIB_VALIGN,
    ATTRIB_VFOLLOWS,
    ATTRIB_NOBITS,
    ATTRIB_PROGBITS
};

static int bin_read_attribute(char **line, int *attribute,
                              uint64_t *value)
{
    expr *e;
    int attrib_name_size;
    struct tokenval tokval;
    char *exp;

    /* Skip whitespace. */
    while (**line && as_isspace(**line))
        (*line)++;
    if (!**line)
        return 0;

    /* Figure out what attribute we're reading. */
    if (!as_strnicmp(*line, "align=", 6))
    {
        *attribute = ATTRIB_ALIGN;
        attrib_name_size = 6;
    }
    else
    {
        if (!as_strnicmp(*line, "start=", 6))
        {
            *attribute = ATTRIB_START;
            attrib_name_size = 6;
        }
        else if (!as_strnicmp(*line, "follows=", 8))
        {
            *attribute = ATTRIB_FOLLOWS;
            *line += 8;
            return 1;
        }
        else if (!as_strnicmp(*line, "vstart=", 7))
        {
            *attribute = ATTRIB_VSTART;
            attrib_name_size = 7;
        }
        else if (!as_strnicmp(*line, "valign=", 7))
        {
            *attribute = ATTRIB_VALIGN;
            attrib_name_size = 7;
        }
        else if (!as_strnicmp(*line, "vfollows=", 9))
        {
            *attribute = ATTRIB_VFOLLOWS;
            *line += 9;
            return 1;
        }
        else if (!as_strnicmp(*line, "nobits", 6) &&
                 (as_isspace((*line)[6]) || ((*line)[6] == '\0')))
        {
            *attribute = ATTRIB_NOBITS;
            *line += 6;
            return 1;
        }
        else if (!as_strnicmp(*line, "progbits", 8) &&
                 (as_isspace((*line)[8]) || ((*line)[8] == '\0')))
        {
            *attribute = ATTRIB_PROGBITS;
            *line += 8;
            return 1;
        }
        else
            return 0;
    }

    /* Find the end of the expression. */
    if ((*line)[attrib_name_size] != '(')
    {
        /* Single term (no parenthesis). */
        exp = *line += attrib_name_size;
        while (**line && !as_isspace(**line))
            (*line)++;
        if (**line)
        {
            **line = '\0';
            (*line)++;
        }
    }
    else
    {
        char c;
        int pcount = 1;

        /* Full expression (delimited by parenthesis) */
        exp = *line += attrib_name_size + 1;
        while (1)
        {
            (*line) += strcspn(*line, "()'\"");
            if (**line == '(')
            {
                ++(*line);
                ++pcount;
            }
            if (**line == ')')
            {
                ++(*line);
                --pcount;
                if (!pcount)
                    break;
            }
            if ((**line == '"') || (**line == '\''))
            {
                c = **line;
                while (**line)
                {
                    ++(*line);
                    if (**line == c)
                        break;
                }
                if (!**line)
                {
                    as_error(ERR_NONFATAL,
                             "invalid syntax in `section' directive");
                    return -1;
                }
                ++(*line);
            }
            if (!**line)
            {
                as_error(ERR_NONFATAL, "expecting `)'");
                return -1;
            }
        }
        *(*line - 1) = '\0'; /* Terminate the expression. */
    }

    /* Check for no value given. */
    if (!*exp)
    {
        as_error(ERR_WARNING, "No value given to attribute in"
                              " `section' directive");
        return -1;
    }

    /* Read and evaluate the expression. */
    stdscan_reset();
    stdscan_set(exp);
    tokval.t_type = TOKEN_INVALID;
    e = evaluate(stdscan, NULL, &tokval, NULL, 1, as_error, NULL);
    if (e)
    {
        if (!is_really_simple(e))
        {
            as_error(ERR_NONFATAL, "section attribute value must be"
                                   " a critical expression");
            return -1;
        }
    }
    else
    {
        as_error(ERR_NONFATAL, "Invalid attribute value"
                               " specified in `section' directive.");
        return -1;
    }
    *value = (uint64_t)reloc_value(e);
    return 1;
}

static void bin_sectalign(int32_t seg, unsigned int value)
{
    struct Section *s = find_section_by_index(seg);

    if (!s || !is_power2(value))
        return;

    if (value > s->align)
        s->align = value;

    if (!(s->flags & ALIGN_DEFINED))
        s->flags |= ALIGN_DEFINED;
}

static void bin_assign_attributes(struct Section *sec, char *astring)
{
    int attribute, check;
    uint64_t value;
    char *p;

    while (1)
    { /* Get the next attribute. */
        check = bin_read_attribute(&astring, &attribute, &value);
        /* Skip bad attribute. */
        if (check == -1)
            continue;
        /* Unknown section attribute, so skip it and warn the user. */
        if (!check)
        {
            if (!*astring)
                break; /* End of line. */
            else
            {
                p = astring;
                while (*astring && !as_isspace(*astring))
                    astring++;
                if (*astring)
                {
                    *astring = '\0';
                    astring++;
                }
                as_error(ERR_WARNING, "ignoring unknown section attribute:"
                                      " \"%s\"",
                         p);
            }
            continue;
        }

        switch (attribute)
        { /* Handle nobits attribute. */
        case ATTRIB_NOBITS:
            if ((sec->flags & TYPE_DEFINED) && (sec->flags & TYPE_PROGBITS))
                as_error(ERR_NONFATAL,
                         "attempt to change section type"
                         " from progbits to nobits");
            else
                sec->flags |= TYPE_DEFINED | TYPE_NOBITS;
            continue;

            /* Handle progbits attribute. */
        case ATTRIB_PROGBITS:
            if ((sec->flags & TYPE_DEFINED) && (sec->flags & TYPE_NOBITS))
                as_error(ERR_NONFATAL, "attempt to change section type"
                                       " from nobits to progbits");
            else
                sec->flags |= TYPE_DEFINED | TYPE_PROGBITS;
            continue;

            /* Handle align attribute. */
        case ATTRIB_ALIGN:
            if (!value || ((value - 1) & value))
            {
                as_error(ERR_NONFATAL,
                         "argument to `align' is not a power of two");
            }
            else
            {
                /*
                 * Alignment is already satisfied if
                 * the previous align value is greater
                 */
                if ((sec->flags & ALIGN_DEFINED) && (value < sec->align))
                    value = sec->align;

                /* Don't allow a conflicting align value. */
                if ((sec->flags & START_DEFINED) && (sec->start & (value - 1)))
                {
                    as_error(ERR_NONFATAL,
                             "`align' value conflicts with section start address");
                }
                else
                {
                    sec->align = value;
                    sec->flags |= ALIGN_DEFINED;
                }
            }
            continue;

            /* Handle valign attribute. */
        case ATTRIB_VALIGN:
            if (!value || ((value - 1) & value))
                as_error(ERR_NONFATAL, "argument to `valign' is not a"
                                       " power of two");
            else
            { /* Alignment is already satisfied if the previous
               * align value is greater. */
                if ((sec->flags & VALIGN_DEFINED) && (value < sec->valign))
                    value = sec->valign;

                /* Don't allow a conflicting valign value. */
                if ((sec->flags & VSTART_DEFINED) && (sec->vstart & (value - 1)))
                    as_error(ERR_NONFATAL,
                             "`valign' value conflicts "
                             "with `vstart' address");
                else
                {
                    sec->valign = value;
                    sec->flags |= VALIGN_DEFINED;
                }
            }
            continue;

            /* Handle start attribute. */
        case ATTRIB_START:
            if (sec->flags & FOLLOWS_DEFINED)
                as_error(ERR_NONFATAL, "cannot combine `start' and `follows'"
                                       " section attributes");
            else if ((sec->flags & START_DEFINED) && (value != sec->start))
                as_error(ERR_NONFATAL, "section start address redefined");
            else
            {
                sec->start = value;
                sec->flags |= START_DEFINED;
                if (sec->flags & ALIGN_DEFINED)
                {
                    if (sec->start & (sec->align - 1))
                        as_error(ERR_NONFATAL, "`start' address conflicts"
                                               " with section alignment");
                    sec->flags ^= ALIGN_DEFINED;
                }
            }
            continue;

            /* Handle vstart attribute. */
        case ATTRIB_VSTART:
            if (sec->flags & VFOLLOWS_DEFINED)
                as_error(ERR_NONFATAL,
                         "cannot combine `vstart' and `vfollows'"
                         " section attributes");
            else if ((sec->flags & VSTART_DEFINED) && (value != sec->vstart))
                as_error(ERR_NONFATAL,
                         "section virtual start address"
                         " (vstart) redefined");
            else
            {
                sec->vstart = value;
                sec->flags |= VSTART_DEFINED;
                if (sec->flags & VALIGN_DEFINED)
                {
                    if (sec->vstart & (sec->valign - 1))
                        as_error(ERR_NONFATAL, "`vstart' address conflicts"
                                               " with `valign' value");
                    sec->flags ^= VALIGN_DEFINED;
                }
            }
            continue;

            /* Handle follows attribute. */
        case ATTRIB_FOLLOWS:
            p = astring;
            astring += strcspn(astring, " \t");
            if (astring == p)
                as_error(ERR_NONFATAL, "expecting section name for `follows'"
                                       " attribute");
            else
            {
                *(astring++) = '\0';
                if (sec->flags & START_DEFINED)
                    as_error(ERR_NONFATAL,
                             "cannot combine `start' and `follows'"
                             " section attributes");
                sec->follows = as_strdup(p);
                sec->flags |= FOLLOWS_DEFINED;
            }
            continue;

            /* Handle vfollows attribute. */
        case ATTRIB_VFOLLOWS:
            if (sec->flags & VSTART_DEFINED)
                as_error(ERR_NONFATAL,
                         "cannot combine `vstart' and `vfollows'"
                         " section attributes");
            else
            {
                p = astring;
                astring += strcspn(astring, " \t");
                if (astring == p)
                    as_error(ERR_NONFATAL,
                             "expecting section name for `vfollows'"
                             " attribute");
                else
                {
                    *(astring++) = '\0';
                    sec->vfollows = as_strdup(p);
                    sec->flags |= VFOLLOWS_DEFINED;
                }
            }
            continue;
        }
    }
}

static void bin_define_section_labels(void)
{
    static int labels_defined = 0;
    struct Section *sec;
    char *label_name;
    size_t base_len;

    if (labels_defined)
        return;
    list_for_each(sec, sections)
    {
        base_len = strlen(sec->name) + 8;
        label_name = as_malloc(base_len + 8);
        strcpy(label_name, "section.");
        strcpy(label_name + 8, sec->name);

        /* section.<name>.start */
        strcpy(label_name + base_len, ".start");
        define_label(label_name, sec->start_index, 0L, NULL, 0, 0);

        /* section.<name>.vstart */
        strcpy(label_name + base_len, ".vstart");
        define_label(label_name, sec->vstart_index, 0L, NULL, 0, 0);

        as_free(label_name);
    }
    labels_defined = 1;
}

static int32_t bin_secname(char *name, int pass, int *bits)
{
    char *p;
    struct Section *sec;

    /* bin_secname is called with *name = NULL at the start of each
     * pass.  Use this opportunity to establish the default section
     * (default is BITS-16 ".text" segment).
     */
    if (!name)
    { /* Reset ORG and section attributes at the start of each pass. */
        origin_defined = 0;
        list_for_each(sec, sections)
            sec->flags &= ~(START_DEFINED | VSTART_DEFINED |
                            ALIGN_DEFINED | VALIGN_DEFINED);

        /* Define section start and vstart labels. */
        if (pass != 1)
            bin_define_section_labels();

        /* Establish the default (.text) section. */
        *bits = 16;
        sec = find_section_by_name(".text");
        sec->flags |= TYPE_DEFINED | TYPE_PROGBITS;
        return sec->vstart_index;
    }

    /* Attempt to find the requested section.  If it does not
     * exist, create it. */
    p = name;
    while (*p && !as_isspace(*p))
        p++;
    if (*p)
        *p++ = '\0';
    sec = find_section_by_name(name);
    if (!sec)
    {
        sec = create_section(name);
        if (!strcmp(name, ".data"))
            sec->flags |= TYPE_DEFINED | TYPE_PROGBITS;
        else if (!strcmp(name, ".bss"))
        {
            sec->flags |= TYPE_DEFINED | TYPE_NOBITS;
            sec->prev = NULL;
        }
    }

    /* Handle attribute assignments. */
    if (pass != 1)
        bin_assign_attributes(sec, p);

#ifndef ABIN_SMART_ADAPT
    /* The following line disables smart adaptation of
     * PROGBITS/NOBITS section types (it forces sections to
     * default to PROGBITS). */
    if ((pass != 1) && !(sec->flags & TYPE_DEFINED))
        sec->flags |= TYPE_DEFINED | TYPE_PROGBITS;
#endif

    return sec->vstart_index;
}

static int bin_directive(enum directives directive, char *args, int pass)
{
    switch (directive)
    {
    case D_ORG:
    {
        struct tokenval tokval;
        uint64_t value;
        expr *e;

        stdscan_reset();
        stdscan_set(args);
        tokval.t_type = TOKEN_INVALID;
        e = evaluate(stdscan, NULL, &tokval, NULL, 1, as_error, NULL);
        if (e)
        {
            if (!is_really_simple(e))
                as_error(ERR_NONFATAL, "org value must be a critical"
                                       " expression");
            else
            {
                value = reloc_value(e);
                /* Check for ORG redefinition. */
                if (origin_defined && (value != origin))
                    as_error(ERR_NONFATAL, "program origin redefined");
                else
                {
                    origin = value;
                    origin_defined = 1;
                }
            }
        }
        else
            as_error(ERR_NONFATAL, "No or invalid offset specified"
                                   " in ORG directive.");
        return 1;
    }
    case D_MAP:
    {
        /* The 'map' directive allows the user to generate section
         * and symbol information to stdout, stderr, or to a file. */
        char *p;

        if (pass != 1)
            return 1;
        args += strspn(args, " \t");
        while (*args)
        {
            p = args;
            args += strcspn(args, " \t");
            if (*args != '\0')
                *(args++) = '\0';
            if (!as_stricmp(p, "all"))
                map_control |=
                    MAP_ORIGIN | MAP_SUMMARY | MAP_SECTIONS | MAP_SYMBOLS;
            else if (!as_stricmp(p, "brief"))
                map_control |= MAP_ORIGIN | MAP_SUMMARY;
            else if (!as_stricmp(p, "sections"))
                map_control |= MAP_ORIGIN | MAP_SUMMARY | MAP_SECTIONS;
            else if (!as_stricmp(p, "segments"))
                map_control |= MAP_ORIGIN | MAP_SUMMARY | MAP_SECTIONS;
            else if (!as_stricmp(p, "symbols"))
                map_control |= MAP_SYMBOLS;
            else if (!rf)
            {
                if (!as_stricmp(p, "stdout"))
                    rf = stdout;
                else if (!as_stricmp(p, "stderr"))
                    rf = stderr;
                else
                { /* Must be a filename. */
                    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
                    if (fd < 0)
                    {
                        as_error(ERR_WARNING, "unable to open map file `%s'",
                                 p);
                        map_control = 0;
                        return 1;
                    }
                    rf = fdopen(fd, "wt");
                    if (!rf)
                    {
                        close(fd);
                        as_error(ERR_WARNING, "unable to open map file `%s'",
                                 p);
                        map_control = 0;
                        return 1;
                    }
                }
            }
            else
                as_error(ERR_WARNING, "map file already specified");
        }
        if (map_control == 0)
            map_control |= MAP_ORIGIN | MAP_SUMMARY;
        if (!rf)
            rf = stdout;
        return 1;
    }
    default:
        return 0;
    }
}

static void bin_filename(char *inname, char *outname)
{
    standard_extension(inname, outname, "");
    infile = inname;
    outfile = outname;
}

static void ith_filename(char *inname, char *outname)
{
    standard_extension(inname, outname, ".ith");
    infile = inname;
    outfile = outname;
}

static void srec_filename(char *inname, char *outname)
{
    standard_extension(inname, outname, ".srec");
    infile = inname;
    outfile = outname;
}

static int32_t bin_segbase(int32_t segment)
{
    return segment;
}

static int bin_set_info(enum geninfo type, char **val)
{
    (void)type;
    (void)val;
    return 0;
}

struct ofmt of_bin, of_ith, of_srec;
static void binfmt_init(void);
static void do_output_bin(void);
static void do_output_ith(void);
static void do_output_srec(void);

static void bin_init(void)
{
    do_output = do_output_bin;
    binfmt_init();
}

static void ith_init(void)
{
    do_output = do_output_ith;
    binfmt_init();
}

static void srec_init(void)
{
    do_output = do_output_srec;
    binfmt_init();
}

static void binfmt_init(void)
{
    maxbits = 64; /* Support 64-bit Segments */
    relocs = NULL;
    reloctail = &relocs;
    origin_defined = 0;
    no_seg_labels = NULL;
    nsl_tail = &no_seg_labels;

    /* Create default section (.text). */
    sections = last_section = as_zalloc(sizeof(struct Section));
    last_section->name = as_strdup(".text");
    last_section->contents = saa_init(1L);
    last_section->flags = TYPE_DEFINED | TYPE_PROGBITS;
    last_section->labels_end = &(last_section->labels);
    last_section->start_index = seg_alloc();
    last_section->vstart_index = seg_alloc();
}

/* Generate binary file output */
static void do_output_bin(void)
{
    struct Section *s;
    uint64_t addr = origin;

    /* Write the progbits sections to the output file. */
    list_for_each(s, sections)
    {
        /* Skip non-progbits sections */
        if (!(s->flags & TYPE_PROGBITS))
            continue;
        /* Skip zero-length sections */
        if (s->length == 0)
            continue;

        /* Pad the space between sections. */
        as_assert(addr <= s->start);
        fwritezero(s->start - addr, ofile);

        /* Write the section to the output file. */
        saa_fpwrite(s->contents, ofile);

        /* Keep track of the current file position */
        addr = s->start + s->length;
    }
}

/* Generate Intel hex file output */
static int write_ith_record(unsigned int len, uint16_t addr,
                            uint8_t type, void *data)
{
    char buf[1 + 2 + 4 + 2 + 255 * 2 + 2 + 2];
    char *p = buf;
    uint8_t csum, *dptr = data;
    unsigned int i;

    as_assert(len <= 255);

    csum = len + addr + (addr >> 8) + type;
    for (i = 0; i < len; i++)
        csum += dptr[i];
    csum = -csum;

    p += sprintf(p, ":%02X%04X%02X", len, addr, type);
    for (i = 0; i < len; i++)
        p += sprintf(p, "%02X", dptr[i]);
    p += sprintf(p, "%02X\n", csum);

    if (fwrite(buf, 1, p - buf, ofile) != (size_t)(p - buf))
        return -1;

    return 0;
}

static void do_output_ith(void)
{
    uint8_t buf[32];
    struct Section *s;
    uint64_t addr, hiaddr, hilba;
    uint64_t length;
    unsigned int chunk;

    /* Write the progbits sections to the output file. */
    hilba = 0;
    list_for_each(s, sections)
    {
        /* Skip non-progbits sections */
        if (!(s->flags & TYPE_PROGBITS))
            continue;
        /* Skip zero-length sections */
        if (s->length == 0)
            continue;

        addr = s->start;
        length = s->length;
        saa_rewind(s->contents);

        while (length)
        {
            hiaddr = addr >> 16;
            if (hiaddr != hilba)
            {
                buf[0] = hiaddr >> 8;
                buf[1] = hiaddr;
                write_ith_record(2, 0, 4, buf);
                hilba = hiaddr;
            }

            chunk = 32 - (addr & 31);
            if (length < chunk)
                chunk = length;

            saa_rnbytes(s->contents, buf, chunk);
            write_ith_record(chunk, (uint16_t)addr, 0, buf);

            addr += chunk;
            length -= chunk;
        }
    }

    /* Write closing record */
    write_ith_record(0, 0, 1, NULL);
}

/* Generate Motorola S-records */
static int write_srecord(unsigned int len, unsigned int alen,
                         uint32_t addr, uint8_t type, void *data)
{
    char buf[2 + 2 + 8 + 255 * 2 + 2 + 2];
    char *p = buf;
    uint8_t csum, *dptr = data;
    unsigned int i;

    as_assert(len <= 255);

    switch (alen)
    {
    case 2:
        addr &= 0xffff;
        break;
    case 3:
        addr &= 0xffffff;
        break;
    case 4:
        break;
    default:
        as_assert(0);
        break;
    }

    csum = (len + alen + 1) + addr + (addr >> 8) + (addr >> 16) + (addr >> 24);
    for (i = 0; i < len; i++)
        csum += dptr[i];
    csum = 0xff - csum;

    p += sprintf(p, "S%c%02X%0*X", type, len + alen + 1, alen * 2, addr);
    for (i = 0; i < len; i++)
        p += sprintf(p, "%02X", dptr[i]);
    p += sprintf(p, "%02X\n", csum);

    if (fwrite(buf, 1, p - buf, ofile) != (size_t)(p - buf))
        return -1;

    return 0;
}

static void do_output_srec(void)
{
    uint8_t buf[32];
    struct Section *s;
    uint64_t addr, maxaddr;
    uint64_t length;
    int alen;
    unsigned int chunk;
    char dtype, etype;

    maxaddr = 0;
    list_for_each(s, sections)
    {
        /* Skip non-progbits sections */
        if (!(s->flags & TYPE_PROGBITS))
            continue;
        /* Skip zero-length sections */
        if (s->length == 0)
            continue;

        addr = s->start + s->length - 1;
        if (addr > maxaddr)
            maxaddr = addr;
    }

    if (maxaddr <= 0xffff)
    {
        alen = 2;
        dtype = '1'; /* S1 = 16-bit data */
        etype = '9'; /* S9 = 16-bit end */
    }
    else if (maxaddr <= 0xffffff)
    {
        alen = 3;
        dtype = '2'; /* S2 = 24-bit data */
        etype = '8'; /* S8 = 24-bit end */
    }
    else
    {
        alen = 4;
        dtype = '3'; /* S3 = 32-bit data */
        etype = '7'; /* S7 = 32-bit end */
    }

    /* Write head record */
    write_srecord(0, 2, 0, '0', NULL);

    /* Write the progbits sections to the output file. */
    list_for_each(s, sections)
    {
        /* Skip non-progbits sections */
        if (!(s->flags & TYPE_PROGBITS))
            continue;
        /* Skip zero-length sections */
        if (s->length == 0)
            continue;

        addr = s->start;
        length = s->length;
        saa_rewind(s->contents);

        while (length)
        {
            chunk = 32 - (addr & 31);
            if (length < chunk)
                chunk = length;

            saa_rnbytes(s->contents, buf, chunk);
            write_srecord(chunk, alen, (uint32_t)addr, dtype, buf);

            addr += chunk;
            length -= chunk;
        }
    }

    /* Write closing record */
    write_srecord(0, alen, 0, etype, NULL);
}

struct ofmt of_bin = {
    "flat-form binary files (e.g. DOS .COM, .SYS)",
    "bin",
    0,
    null_debug_arr,
    &null_debug_form,
    bin_stdmac,
    bin_init,
    bin_set_info,
    bin_out,
    bin_deflabel,
    bin_secname,
    bin_sectalign,
    bin_segbase,
    bin_directive,
    bin_filename,
    bin_cleanup};

struct ofmt of_ith = {
    "Intel hex",
    "ith",
    OFMT_TEXT,
    null_debug_arr,
    &null_debug_form,
    bin_stdmac,
    ith_init,
    bin_set_info,
    bin_out,
    bin_deflabel,
    bin_secname,
    bin_sectalign,
    bin_segbase,
    bin_directive,
    ith_filename,
    bin_cleanup};

struct ofmt of_srec = {
    "Motorola S-records",
    "srec",
    0,
    null_debug_arr,
    &null_debug_form,
    bin_stdmac,
    srec_init,
    bin_set_info,
    bin_out,
    bin_deflabel,
    bin_secname,
    bin_sectalign,
    bin_segbase,
    bin_directive,
    srec_filename,
    bin_cleanup};

#endif /* #ifdef OF_BIN */
