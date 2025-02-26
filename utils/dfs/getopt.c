/////////////////////////////////////////////////////////////////////
//
// Unix getopt(3)
//
// File:   getopt.c
//

#include <stdio.h>
#include <io.h>
#include <string.h>

/*
 * Here's something you've all been waiting for:  the AT&T public domain
 * source for getopt(3).  It is the code which was given out at the 1985
 * UNIFORUM conference in Dallas.  I obtained it by electronic mail
 * directly from AT&T.  The people there assure me that it is indeed
 * in the public domain.
 *
 * There is no manual page.  That is because the one they gave out at
 * UNIFORUM was slightly different from the current System V Release 2
 * manual page.  The difference apparently involved a note about the
 * famous rules 5 and 6, recommending using white space between an option
 * and its first argument, and not grouping options that have arguments.
 * Getopt itself is currently lenient about both of these things.  White
 * space is allowed, but not mandatory, and the last option in a group can
 * have an argument.  That particular version of the man page evidently
 * has no official existence, and my source at AT&T did not send a copy.
 * The current SVR2 man page reflects the actual behavor of this getopt.
 * However, I am not about to post a copy of anything licensed by AT&T.
 */

#if 0
#define ERR(s, c)                                           \
    if (opterr)                                             \
    {                                                       \
        char errbuf[2];                                     \
        errbuf[0] = (char)c;                                \
        errbuf[1] = '\n';                                   \
        (void)write(2, argv[0], (unsigned)strlen(argv[0])); \
        (void)write(2, s, (unsigned)strlen(s));             \
        (void)write(2, errbuf, 2);                          \
    }
#else
#define ERR(s, c) \
    if (opterr)   \
    printf("%s: %s%c\n", argv[0], s, c)
#endif

int opterr = 1;
int optind = 1;
int optopt = 0;
char *optarg = 0;

int getopt(argc, argv, opts)
int argc;
char **argv, *opts;
{
    static int sp = 1;
    register int c;
    register char *cp;

    if (sp == 1)
        if (optind >= argc ||
            argv[optind][0] != '-' && argv[optind][0] != '/' ||
            argv[optind][1] == '\0')
            return (EOF);
        else if (strcmp(argv[optind], "--") == 0)
        {
            optind++;
            return (EOF);
        }
    optopt = c = argv[optind][sp];
    if (c == ':' || (cp = strchr(opts, c)) == 0)
    {
        ERR(": illegal option -- ", c);
        if (argv[optind][++sp] == '\0')
        {
            optind++;
            sp = 1;
        }
        return ('?');
    }
    if (*++cp == ':')
    {
        if (argv[optind][sp + 1] != '\0')
            optarg = &argv[optind++][sp + 1];
        else if (++optind >= argc)
        {
            ERR(": option requires an argument -- ", c);
            sp = 1;
            return ('?');
        }
        else
            optarg = argv[optind++];
        sp = 1;
    }
    else
    {
        if (argv[optind][++sp] == '\0')
        {
            sp = 1;
            optind++;
        }
        optarg = NULL;
    }
    return (c);
}
