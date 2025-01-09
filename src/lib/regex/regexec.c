//
// Matching for regular expression library
//

//
// This file includes engine.c *twice*, after muchos fiddling with the
// macros that code uses.  This lets the same code operate on two different
// representations for state sets.
//

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <regex.h>

#include "regex2.h"

#ifdef _MSC_VER
#pragma warning(disable: 4018)
#endif

static int nope = 0;    // for use in asserts; shuts lint up

// Macros for manipulating states, small version
#define states  unsigned
#define states1 unsigned  // for later use in regexec() decision
#define CLEAR(v)  ((v) = 0)
#define SET0(v, n)  ((v) &= ~((unsigned)1 << (n)))
#define SET1(v, n)  ((v) |= (unsigned)1 << (n))
#define ISSET(v, n) ((v) & ((unsigned)1 << (n)))
#define ASSIGN(d, s)  ((d) = (s))
#define EQ(a, b)  ((a) == (b))
#define STATEVARS int dummy // dummy version
#define STATESETUP(m, n)  // nothing
#define STATETEARDOWN(m)  // nothing
#define SETUP(v)  ((v) = 0)
#define onestate  unsigned
#define INIT(o, n)  ((o) = (unsigned)1 << (n))
#define INC(o)  ((o) <<= 1)
#define ISSTATEIN(v, o) ((v) & (o))
// Some abbreviations; note that some of these know variable names!
// do "if I'm here, I can also be there" etc without branches
#define FWD(dst, src, n)  ((dst) |= ((unsigned)(src) & (here)) << (n))
#define BACK(dst, src, n) ((dst) |= ((unsigned)(src) & (here)) >> (n))
#define ISSETBACK(v, n) ((v) & ((unsigned)here >> (n)))
// Function names
#define SNAMES      // engine.c looks after details

#include "engine.c"

// Now undo things
#undef  states
#undef  CLEAR
#undef  SET0
#undef  SET1
#undef  ISSET
#undef  ASSIGN
#undef  EQ
#undef  STATEVARS
#undef  STATESETUP
#undef  STATETEARDOWN
#undef  SETUP
#undef  onestate
#undef  INIT
#undef  INC
#undef  ISSTATEIN
#undef  FWD
#undef  BACK
#undef  ISSETBACK
#undef  SNAMES

// Macros for manipulating states, large version
#define states  char *
#define CLEAR(v)  memset(v, 0, m->g->nstates)
#define SET0(v, n)  ((v)[n] = 0)
#define SET1(v, n)  ((v)[n] = 1)
#define ISSET(v, n) ((v)[n])
#define ASSIGN(d, s)  memcpy(d, s, m->g->nstates)
#define EQ(a, b)  (memcmp(a, b, m->g->nstates) == 0)
#define STATEVARS int vn; char *space
#define STATESETUP(m, nv) { (m)->space = malloc((nv)*(m)->g->nstates); \
        if ((m)->space == NULL) return(REG_ESPACE); \
        (m)->vn = 0; }
#define STATETEARDOWN(m)  { free((m)->space); }
#define SETUP(v)  ((v) = &m->space[m->vn++ * m->g->nstates])
#define onestate  int
#define INIT(o, n)  ((o) = (n))
#define INC(o)  ((o)++)
#define ISSTATEIN(v, o) ((v)[o])
// Some abbreviations; note that some of these know variable names!
// do "if I'm here, I can also be there" etc without branches
#define FWD(dst, src, n)  ((dst)[here + (n)] |= (src)[here])
#define BACK(dst, src, n) ((dst)[here - (n)] |= (src)[here])
#define ISSETBACK(v, n) ((v)[here - (n)])
// Function names
#define LNAMES      /* flag */

#include "engine.c"

#ifdef REDEBUG
#define GOODFLAGS(f) (f)
#else
#define GOODFLAGS(f) ((f) & (REG_NOTBOL | REG_NOTEOL | REG_STARTEND))
#endif

//
// regexec - interface for matching
//
// We put this here so we can exploit knowledge of the state representation
// when choosing which matcher to call.  Also, by this point the matchers
// have been prototyped.
//
int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[], int eflags) {
    struct re_guts *g = preg->re_g;

    if (preg->re_magic != MAGIC1 || g->magic != MAGIC2) return REG_BADPAT;
    assert(!(g->iflags & BAD));
    if (g->iflags & BAD) return REG_BADPAT; // backstop for no-debug case

    eflags = GOODFLAGS(eflags);

    if (g->nstates <= CHAR_BIT * sizeof(states1) && !(eflags & REG_LARGE)) {
        return smatcher(g, (char *) string, nmatch, pmatch, eflags);
    } else {
        return lmatcher(g, (char *) string, nmatch, pmatch, eflags);
    }
}
