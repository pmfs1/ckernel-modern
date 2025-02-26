/*
 * quote.c
 */

#include "compiler.h"

#include <stdlib.h>

#include "aslib.h"
#include "quote.h"

#define numvalue(c) ((c) >= 'a' ? (c) - 'a' + 10 : (c) >= 'A' ? (c) - 'A' + 10 \
                                                              : (c) - '0')

char *as_quote(char *str, size_t len)
{
    char c, c1, *p, *q, *nstr, *ep;
    unsigned char uc;
    bool sq_ok, dq_ok;
    size_t qlen;

    sq_ok = dq_ok = true;
    ep = str + len;
    qlen = 0; /* Length if we need `...` quotes */
    for (p = str; p < ep; p++)
    {
        c = *p;
        switch (c)
        {
        case '\'':
            sq_ok = false;
            qlen++;
            break;
        case '\"':
            dq_ok = false;
            qlen++;
            break;
        case '`':
        case '\\':
            qlen += 2;
            break;
        default:
            if (c < ' ' || c > '~')
            {
                sq_ok = dq_ok = false;
                switch (c)
                {
                case '\a':
                case '\b':
                case '\t':
                case '\n':
                case '\v':
                case '\f':
                case '\r':
                case 27:
                    qlen += 2;
                    break;
                default:
                    c1 = (p + 1 < ep) ? p[1] : 0;
                    if (c1 >= '0' && c1 <= '7')
                        uc = 0377; /* Must use the full form */
                    else
                        uc = c;
                    if (uc > 077)
                        qlen++;
                    if (uc > 07)
                        qlen++;
                    qlen += 2;
                    break;
                }
            }
            else
            {
                qlen++;
            }
            break;
        }
    }

    if (sq_ok || dq_ok)
    {
        /* Use '...' or "..." */
        nstr = as_malloc(len + 3);
        nstr[0] = nstr[len + 1] = sq_ok ? '\'' : '\"';
        nstr[len + 2] = '\0';
        if (len > 0)
            memcpy(nstr + 1, str, len);
    }
    else
    {
        /* Need to use `...` quoted syntax */
        nstr = as_malloc(qlen + 3);
        q = nstr;
        *q++ = '`';
        for (p = str; p < ep; p++)
        {
            c = *p;
            switch (c)
            {
            case '`':
            case '\\':
                *q++ = '\\';
                *q++ = c;
                break;
            case 7:
                *q++ = '\\';
                *q++ = 'a';
                break;
            case 8:
                *q++ = '\\';
                *q++ = 'b';
                break;
            case 9:
                *q++ = '\\';
                *q++ = 't';
                break;
            case 10:
                *q++ = '\\';
                *q++ = 'n';
                break;
            case 11:
                *q++ = '\\';
                *q++ = 'v';
                break;
            case 12:
                *q++ = '\\';
                *q++ = 'f';
                break;
            case 13:
                *q++ = '\\';
                *q++ = 'r';
                break;
            case 27:
                *q++ = '\\';
                *q++ = 'e';
                break;
            default:
                if (c < ' ' || c > '~')
                {
                    c1 = (p + 1 < ep) ? p[1] : 0;
                    if (c1 >= '0' && c1 <= '7')
                        uc = 0377; /* Must use the full form */
                    else
                        uc = c;
                    *q++ = '\\';
                    if (uc > 077)
                        *q++ = ((unsigned char)c >> 6) + '0';
                    if (uc > 07)
                        *q++ = (((unsigned char)c >> 3) & 7) + '0';
                    *q++ = ((unsigned char)c & 7) + '0';
                    break;
                }
                else
                {
                    *q++ = c;
                }
                break;
            }
        }
        *q++ = '`';
        *q++ = '\0';
        as_assert((size_t)(q - nstr) == qlen + 3);
    }
    return nstr;
}

static char *emit_utf8(char *q, int32_t v)
{
    if (v < 0)
    {
        /* Impossible - do nothing */
    }
    else if (v <= 0x7f)
    {
        *q++ = v;
    }
    else if (v <= 0x000007ff)
    {
        *q++ = 0xc0 | (v >> 6);
        *q++ = 0x80 | (v & 63);
    }
    else if (v <= 0x0000ffff)
    {
        *q++ = 0xe0 | (v >> 12);
        *q++ = 0x80 | ((v >> 6) & 63);
        *q++ = 0x80 | (v & 63);
    }
    else if (v <= 0x001fffff)
    {
        *q++ = 0xf0 | (v >> 18);
        *q++ = 0x80 | ((v >> 12) & 63);
        *q++ = 0x80 | ((v >> 6) & 63);
        *q++ = 0x80 | (v & 63);
    }
    else if (v <= 0x03ffffff)
    {
        *q++ = 0xf8 | (v >> 24);
        *q++ = 0x80 | ((v >> 18) & 63);
        *q++ = 0x80 | ((v >> 12) & 63);
        *q++ = 0x80 | ((v >> 6) & 63);
        *q++ = 0x80 | (v & 63);
    }
    else
    {
        *q++ = 0xfc | (v >> 30);
        *q++ = 0x80 | ((v >> 24) & 63);
        *q++ = 0x80 | ((v >> 18) & 63);
        *q++ = 0x80 | ((v >> 12) & 63);
        *q++ = 0x80 | ((v >> 6) & 63);
        *q++ = 0x80 | (v & 63);
    }
    return q;
}

/*
 * Do an *in-place* dequoting of the specified string, returning the
 * resulting length (which may be containing embedded nulls.)
 *
 * In-place replacement is possible since the unquoted length is always
 * shorter than or equal to the quoted length.
 *
 * *ep points to the final quote, or to the null if improperly quoted.
 */
size_t as_unquote(char *str, char **ep)
{
    char bq;
    char *p, *q;
    char *escp = NULL;
    char c;
    enum unq_state
    {
        st_start,
        st_backslash,
        st_hex,
        st_oct,
        st_ucs,
    } state;
    int ndig = 0;
    int32_t nval = 0;

    p = q = str;

    bq = *p++;
    if (!bq)
        return 0;

    switch (bq)
    {
    case '\'':
    case '\"':
        /* '...' or "..." string */
        while ((c = *p) && c != bq)
        {
            p++;
            *q++ = c;
        }
        *q = '\0';
        break;

    case '`':
        /* `...` string */
        state = st_start;

        while ((c = *p))
        {
            p++;
            switch (state)
            {
            case st_start:
                switch (c)
                {
                case '\\':
                    state = st_backslash;
                    break;
                case '`':
                    p--;
                    goto out;
                default:
                    *q++ = c;
                    break;
                }
                break;

            case st_backslash:
                state = st_start;
                escp = p; /* Beginning of argument sequence */
                nval = 0;
                switch (c)
                {
                case 'a':
                    *q++ = 7;
                    break;
                case 'b':
                    *q++ = 8;
                    break;
                case 'e':
                    *q++ = 27;
                    break;
                case 'f':
                    *q++ = 12;
                    break;
                case 'n':
                    *q++ = 10;
                    break;
                case 'r':
                    *q++ = 13;
                    break;
                case 't':
                    *q++ = 9;
                    break;
                case 'u':
                    state = st_ucs;
                    ndig = 4;
                    break;
                case 'U':
                    state = st_ucs;
                    ndig = 8;
                    break;
                case 'v':
                    *q++ = 11;
                    break;
                case 'x':
                case 'X':
                    state = st_hex;
                    ndig = 2;
                    break;
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                    state = st_oct;
                    ndig = 2; /* Up to two more digits */
                    nval = c - '0';
                    break;
                default:
                    *q++ = c;
                    break;
                }
                break;

            case st_oct:
                if (c >= '0' && c <= '7')
                {
                    nval = (nval << 3) + (c - '0');
                    if (!--ndig)
                    {
                        *q++ = nval;
                        state = st_start;
                    }
                }
                else
                {
                    p--; /* Process this character again */
                    *q++ = nval;
                    state = st_start;
                }
                break;

            case st_hex:
                if ((c >= '0' && c <= '9') ||
                    (c >= 'A' && c <= 'F') ||
                    (c >= 'a' && c <= 'f'))
                {
                    nval = (nval << 4) + numvalue(c);
                    if (!--ndig)
                    {
                        *q++ = nval;
                        state = st_start;
                    }
                }
                else
                {
                    p--; /* Process this character again */
                    *q++ = (p > escp) ? nval : escp[-1];
                    state = st_start;
                }
                break;

            case st_ucs:
                if ((c >= '0' && c <= '9') ||
                    (c >= 'A' && c <= 'F') ||
                    (c >= 'a' && c <= 'f'))
                {
                    nval = (nval << 4) + numvalue(c);
                    if (!--ndig)
                    {
                        q = emit_utf8(q, nval);
                        state = st_start;
                    }
                }
                else
                {
                    p--; /* Process this character again */
                    if (p > escp)
                        q = emit_utf8(q, nval);
                    else
                        *q++ = escp[-1];
                    state = st_start;
                }
                break;
            }
        }
        switch (state)
        {
        case st_start:
        case st_backslash:
            break;
        case st_oct:
            *q++ = nval;
            break;
        case st_hex:
            *q++ = (p > escp) ? nval : escp[-1];
            break;
        case st_ucs:
            if (p > escp)
                q = emit_utf8(q, nval);
            else
                *q++ = escp[-1];
            break;
        }
    out:
        break;

    default:
        /* Not a quoted string, just return the input... */
        p = q = strchr(str, '\0');
        break;
    }

    if (ep)
        *ep = p;
    return q - str;
}

/*
 * Find the end of a quoted string; returns the pointer to the terminating
 * character (either the ending quote or the null character, if unterminated.)
 */
char *as_skip_string(char *str)
{
    char bq;
    char *p;
    char c;
    enum unq_state
    {
        st_start,
        st_backslash,
    } state;

    bq = str[0];
    if (bq == '\'' || bq == '\"')
    {
        /* '...' or "..." string */
        for (p = str + 1; *p && *p != bq; p++)
            ;
        return p;
    }
    else if (bq == '`')
    {
        /* `...` string */
        p = str + 1;
        state = st_start;

        while ((c = *p++))
        {
            switch (state)
            {
            case st_start:
                switch (c)
                {
                case '\\':
                    state = st_backslash;
                    break;
                case '`':
                    return p - 1; /* Found the end */
                default:
                    break;
                }
                break;

            case st_backslash:
                /*
                 * Note: for the purpose of finding the end of the string,
                 * all successor states to st_backslash are functionally
                 * equivalent to st_start, since either a backslash or
                 * a backquote will force a return to the st_start state.
                 */
                state = st_start;
                break;
            }
        }
        return p; /* Unterminated string... */
    }
    else
    {
        return str; /* Not a string... */
    }
}
