/*
 * eval.c    expression evaluator for the assembler
 */

#include "compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>

#include "as.h"
#include "aslib.h"
#include "eval.h"
#include "labels.h"
#include "float.h"

#define TEMPEXPRS_DELTA 128
#define TEMPEXPR_DELTA 8

static scanner scan;    /* Address of scanner routine */
static efunc error;     /* Address of error reporting routine */
static lfunc labelfunc; /* Address of label routine */

static struct ofmt *outfmt; /* Structure of addresses of output routines */

static expr **tempexprs = NULL;
static int ntempexprs;
static int tempexprs_size = 0;

static expr *tempexpr;
static int ntempexpr;
static int tempexpr_size;

// Remove the global tokval declaration
static int i; /* The t_type of tokval */

static struct location *location; /* Pointer to current line's segment,offset */

extern int in_abs_seg;     /* ABSOLUTE segment flag */
extern int32_t abs_seg;    /* ABSOLUTE segment */
extern int32_t abs_offset; /* ABSOLUTE segment offset */

/*
 * Unimportant cleanup is done to avoid confusing people who are trying
 * to debug real memory leaks
 */
void eval_cleanup(void)
{
    while (ntempexprs)
        as_free(tempexprs[--ntempexprs]);
    as_free(tempexprs);
}

/*
 * Construct a temporary expression.
 */
static void begintemp(void)
{
    tempexpr = NULL;
    tempexpr_size = ntempexpr = 0;
}

static void addtotemp(int32_t type, int64_t value)
{
    while (ntempexpr >= tempexpr_size)
    {
        tempexpr_size += TEMPEXPR_DELTA;
        tempexpr = as_realloc(tempexpr,
                              tempexpr_size * sizeof(*tempexpr));
    }
    tempexpr[ntempexpr].type = type;
    tempexpr[ntempexpr++].value = value;
}

static expr *finishtemp(void)
{
    addtotemp(0L, 0L); /* terminate */
    while (ntempexprs >= tempexprs_size)
    {
        tempexprs_size += TEMPEXPRS_DELTA;
        tempexprs = as_realloc(tempexprs,
                               tempexprs_size * sizeof(*tempexprs));
    }
    return tempexprs[ntempexprs++] = tempexpr;
}

/*
 * Add two vector datatypes. We have some bizarre behaviour on far-
 * absolute segment types: we preserve them during addition _only_
 * if one of the segments is a truly pure scalar.
 */
static expr *add_vectors(expr *p, expr *q)
{
    int preserve;

    preserve = is_really_simple(p) || is_really_simple(q);

    begintemp();

    while (p->type && q->type &&
           p->type < EXPR_SEGBASE + SEG_ABS &&
           q->type < EXPR_SEGBASE + SEG_ABS)
    {
        int lasttype;

        if (p->type > q->type)
        {
            addtotemp(q->type, q->value);
            lasttype = q++->type;
        }
        else if (p->type < q->type)
        {
            addtotemp(p->type, p->value);
            lasttype = p++->type;
        }
        else
        { /* *p and *q have same type */
            int64_t sum = p->value + q->value;
            if (sum)
                addtotemp(p->type, sum);
            lasttype = p->type;
            p++, q++;
        }
        if (lasttype == EXPR_UNKNOWN)
        {
            return finishtemp();
        }
    }
    while (p->type && (preserve || p->type < EXPR_SEGBASE + SEG_ABS))
    {
        addtotemp(p->type, p->value);
        p++;
    }
    while (q->type && (preserve || q->type < EXPR_SEGBASE + SEG_ABS))
    {
        addtotemp(q->type, q->value);
        q++;
    }

    return finishtemp();
}

/*
 * Multiply a vector by a scalar. Strip far-absolute segment part
 * if present.
 *
 * Explicit treatment of UNKNOWN is not required in this routine,
 * since it will silently do the Right Thing anyway.
 *
 * If `affect_hints' is set, we also change the hint type to
 * NOTBASE if a MAKEBASE hint points at a register being
 * multiplied. This allows [eax*1+ebx] to hint EBX rather than EAX
 * as the base register.
 */
// Update scalar_mult prototype to include hint parameter
static expr *scalar_mult(expr *vect, int64_t scalar, int affect_hints, struct eval_hints *hint)
{
    expr *p = vect;

    while (p->type && p->type < EXPR_SEGBASE + SEG_ABS)
    {
        p->value = scalar * (p->value);
        if (hint && hint->type == EAH_MAKEBASE &&
            p->type == hint->base && affect_hints)
            hint->type = EAH_NOTBASE;
        p++;
    }
    p->type = 0;

    return vect;
}

static expr *scalarvect(int64_t scalar)
{
    begintemp();
    addtotemp(EXPR_SIMPLE, scalar);
    return finishtemp();
}

static expr *unknown_expr(void)
{
    begintemp();
    addtotemp(EXPR_UNKNOWN, 1L);
    return finishtemp();
}

/*
 * The SEG operator: calculate the segment part of a relocatable
 * value. Return NULL, as usual, if an error occurs. Report the
 * error too.
 */
static expr *segment_part(expr *e)
{
    int32_t seg;

    if (is_unknown(e))
        return unknown_expr();

    if (!is_reloc(e))
    {
        error(ERR_NONFATAL, "cannot apply SEG to a non-relocatable value");
        return NULL;
    }

    seg = reloc_seg(e);
    if (seg == NO_SEG)
    {
        error(ERR_NONFATAL, "cannot apply SEG to a non-relocatable value");
        return NULL;
    }
    else if (seg & SEG_ABS)
    {
        return scalarvect(seg & ~SEG_ABS);
    }
    else if (seg & 1)
    {
        error(ERR_NONFATAL, "SEG applied to something which"
                            " is already a segment base");
        return NULL;
    }
    else
    {
        int32_t base = outfmt->segbase(seg + 1);

        begintemp();
        addtotemp((base == NO_SEG ? EXPR_UNKNOWN : EXPR_SEGBASE + base),
                  1L);
        return finishtemp();
    }
}

/*
 * Recursive-descent parser. Called with a single boolean operand,
 * which is true if the evaluation is critical (i.e. unresolved
 * symbols are an error condition). Must update the global `i' to
 * reflect the token after the parsed string. May return NULL.
 *
 * evaluate() should report its own errors: on return it is assumed
 * that if NULL has been returned, the error has already been
 * reported.
 */

/*
 * Grammar parsed is:
 *
 * expr  : bexpr [ WRT expr6 ]
 * bexpr : rexp0 or expr0 depending on relative-mode setting
 * rexp0 : rexp1 [ {||} rexp1...]
 * rexp1 : rexp2 [ {^^} rexp2...]
 * rexp2 : rexp3 [ {&&} rexp3...]
 * rexp3 : expr0 [ {=,==,<>,!=,<,>,<=,>=} expr0 ]
 * expr0 : expr1 [ {|} expr1...]
 * expr1 : expr2 [ {^} expr2...]
 * expr2 : expr3 [ {&} expr3...]
 * expr3 : expr4 [ {<<,>>} expr4...]
 * expr4 : expr5 [ {+,-} expr5...]
 * expr5 : expr6 [ {*,/,%,//,%%} expr6...]
 * expr6 : { ~,+,-,SEG } expr6
 *       | (bexpr)
 *       | symbol
 *       | $
 *       | number
 */

// Update function prototypes to include tokenval parameter
static expr *rexp0(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *rexp1(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *rexp2(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *rexp3(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr0(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr1(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr2(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr3(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr4(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr5(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);
static expr *expr6(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags);

// Update the function pointer type
static expr *(*bexpr)(int, void *, struct tokenval *, struct eval_hints *, int *);

// Update all evaluation function implementations to include tv parameter
// Here's an example for rexp0, apply similar changes to all other functions:
static expr *rexp0(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = rexp1(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == TOKEN_DBL_OR)
    {
        i = scan(scpriv, tv);
        f = rexp1(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`|' operator may only be applied to"
                                " scalar values");
        }

        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect((int64_t)(reloc_value(e) || reloc_value(f)));
    }
    return e;
}

static expr *rexp1(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = rexp2(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == TOKEN_DBL_XOR)
    {
        i = scan(scpriv, tv);
        f = rexp2(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`^' operator may only be applied to"
                                " scalar values");
        }

        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect((int64_t)(!reloc_value(e) ^ !reloc_value(f)));
    }
    return e;
}

static expr *rexp2(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = rexp3(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;
    while (i == TOKEN_DBL_AND)
    {
        i = scan(scpriv, tv);
        f = rexp3(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`&' operator may only be applied to"
                                " scalar values");
        }
        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect((int64_t)(reloc_value(e) && reloc_value(f)));
    }
    return e;
}

// Update rexp3 to include hint parameter
static expr *rexp3(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;
    int64_t v;

    e = expr0(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == TOKEN_EQ || i == TOKEN_LT || i == TOKEN_GT ||
           i == TOKEN_NE || i == TOKEN_LE || i == TOKEN_GE)
    {
        int j = i;
        i = scan(scpriv, tv);
        f = expr0(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;

        e = add_vectors(e, scalar_mult(f, -1L, false, hint));

        switch (j)
        {
        case TOKEN_EQ:
        case TOKEN_NE:
            if (is_unknown(e))
                v = -1; /* means unknown */
            else if (!is_really_simple(e) || reloc_value(e) != 0)
                v = (j == TOKEN_NE); /* unequal, so return true if NE */
            else
                v = (j == TOKEN_EQ); /* equal, so return true if EQ */
            break;
        default:
            if (is_unknown(e))
                v = -1; /* means unknown */
            else if (!is_really_simple(e))
            {
                error(ERR_NONFATAL,
                      "`%s': operands differ by a non-scalar",
                      (j == TOKEN_LE ? "<=" : j == TOKEN_LT ? "<"
                                          : j == TOKEN_GE   ? ">="
                                                            : ">"));
                v = 0; /* must set it to _something_ */
            }
            else
            {
                int64_t vv = reloc_value(e);
                if (vv == 0)
                    v = (j == TOKEN_LE || j == TOKEN_GE);
                else if (vv > 0)
                    v = (j == TOKEN_GE || j == TOKEN_GT);
                else /* vv < 0 */
                    v = (j == TOKEN_LE || j == TOKEN_LT);
            }
            break;
        }

        if (v == -1)
            e = unknown_expr();
        else
            e = scalarvect(v);
    }
    return e;
}

static expr *expr0(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr1(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == '|')
    {
        i = scan(scpriv, tv);
        f = expr1(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`|' operator may only be applied to"
                                " scalar values");
        }
        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect(reloc_value(e) | reloc_value(f));
    }
    return e;
}

static expr *expr1(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr2(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == '^')
    {
        i = scan(scpriv, tv);
        f = expr2(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`^' operator may only be applied to"
                                " scalar values");
        }
        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect(reloc_value(e) ^ reloc_value(f));
    }
    return e;
}

static expr *expr2(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr3(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == '&')
    {
        i = scan(scpriv, tv);
        f = expr3(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "`&' operator may only be applied to"
                                " scalar values");
        }
        if (is_just_unknown(e) || is_just_unknown(f))
            e = unknown_expr();
        else
            e = scalarvect(reloc_value(e) & reloc_value(f));
    }
    return e;
}

static expr *expr3(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr4(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;

    while (i == TOKEN_SHL || i == TOKEN_SHR)
    {
        int j = i;
        i = scan(scpriv, tv);
        f = expr4(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (!(is_simple(e) || is_just_unknown(e)) ||
            !(is_simple(f) || is_just_unknown(f)))
        {
            error(ERR_NONFATAL, "shift operator may only be applied to"
                                " scalar values");
        }
        else if (is_just_unknown(e) || is_just_unknown(f))
        {
            e = unknown_expr();
        }
        else
            switch (j)
            {
            case TOKEN_SHL:
                e = scalarvect(reloc_value(e) << reloc_value(f));
                break;
            case TOKEN_SHR:
                e = scalarvect(((uint64_t)reloc_value(e)) >>
                               reloc_value(f));
                break;
            }
    }
    return e;
}

static expr *expr4(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr5(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;
    while (i == '+' || i == '-')
    {
        int j = i;
        i = scan(scpriv, tv);
        f = expr5(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        switch (j)
        {
        case '+':
            e = add_vectors(e, f);
            break;
        case '-':
            e = add_vectors(e, scalar_mult(f, -1L, false, hint));
            break;
        }
    }
    return e;
}

static expr *expr5(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    expr *e, *f;

    e = expr6(critical, scpriv, tv, hint, opflags);
    if (!e)
        return NULL;
    while (i == '*' || i == '/' || i == '%' ||
           i == TOKEN_SDIV || i == TOKEN_SMOD)
    {
        int j = i;
        i = scan(scpriv, tv);
        f = expr6(critical, scpriv, tv, hint, opflags);
        if (!f)
            return NULL;
        if (j != '*' && (!(is_simple(e) || is_just_unknown(e)) ||
                         !(is_simple(f) || is_just_unknown(f))))
        {
            error(ERR_NONFATAL, "division operator may only be applied to"
                                " scalar values");
            return NULL;
        }
        if (j != '*' && !is_unknown(f) && reloc_value(f) == 0)
        {
            error(ERR_NONFATAL, "division by zero");
            return NULL;
        }
        switch (j)
        {
        case '*':
            if (is_simple(e))
                e = scalar_mult(f, reloc_value(e), true, hint);
            else if (is_simple(f))
                e = scalar_mult(e, reloc_value(f), true, hint);
            else if (is_just_unknown(e) && is_just_unknown(f))
                e = unknown_expr();
            else
            {
                error(ERR_NONFATAL, "unable to multiply two "
                                    "non-scalar objects");
                return NULL;
            }
            break;
        case '/':
            if (is_just_unknown(e) || is_just_unknown(f))
                e = unknown_expr();
            else
                e = scalarvect(((uint64_t)reloc_value(e)) /
                               ((uint64_t)reloc_value(f)));
            break;
        case '%':
            if (is_just_unknown(e) || is_just_unknown(f))
                e = unknown_expr();
            else
                e = scalarvect(((uint64_t)reloc_value(e)) %
                               ((uint64_t)reloc_value(f)));
            break;
        case TOKEN_SDIV:
            if (is_just_unknown(e) || is_just_unknown(f))
                e = unknown_expr();
            else
                e = scalarvect(((int64_t)reloc_value(e)) /
                               ((int64_t)reloc_value(f)));
            break;
        case TOKEN_SMOD:
            if (is_just_unknown(e) || is_just_unknown(f))
                e = unknown_expr();
            else
                e = scalarvect(((int64_t)reloc_value(e)) %
                               ((int64_t)reloc_value(f)));
            break;
        }
    }
    return e;
}

static expr *eval_floatize(enum floatize type, void *scpriv, struct tokenval *tv)
{
    uint8_t result[16], *p; /* Up to 128 bits */
    static const struct
    {
        int bytes, start, len;
    } formats[] = {
        {1, 0, 1},  /* FLOAT_8 */
        {2, 0, 2},  /* FLOAT_16 */
        {4, 0, 4},  /* FLOAT_32 */
        {8, 0, 8},  /* FLOAT_64 */
        {10, 0, 8}, /* FLOAT_80M */
        {10, 8, 2}, /* FLOAT_80E */
        {16, 0, 8}, /* FLOAT_128L */
        {16, 8, 8}, /* FLOAT_128H */
    };
    int sign = 1;
    int64_t val;
    int j;

    i = scan(scpriv, tv);
    if (i != '(')
    {
        error(ERR_NONFATAL, "expecting `('");
        return NULL;
    }
    i = scan(scpriv, tv);
    if (i == '-' || i == '+')
    {
        sign = (i == '-') ? -1 : 1;
        i = scan(scpriv, tv);
    }
    if (i != TOKEN_FLOAT)
    {
        error(ERR_NONFATAL, "expecting floating-point number");
        return NULL;
    }
    if (!float_const(tv->t_charptr, sign, result,
                     formats[type].bytes, error))
        return NULL;
    i = scan(scpriv, tv);
    if (i != ')')
    {
        error(ERR_NONFATAL, "expecting `)'");
        return NULL;
    }

    p = result + formats[type].start + formats[type].len;
    val = 0;
    for (j = formats[type].len; j; j--)
    {
        p--;
        val = (val << 8) + *p;
    }

    begintemp();
    addtotemp(EXPR_SIMPLE, val);

    i = scan(scpriv, tv);
    return finishtemp();
}

static expr *eval_strfunc(enum strfunc type, void *scpriv, struct tokenval *tv)
{
    char *string;
    size_t string_len;
    int64_t val;
    bool parens, rn_warn;

    parens = false;
    i = scan(scpriv, tv);
    if (i == '(')
    {
        parens = true;
        i = scan(scpriv, tv);
    }
    if (i != TOKEN_STR)
    {
        error(ERR_NONFATAL, "expecting string");
        return NULL;
    }
    string_len = string_transform(tv->t_charptr, tv->t_inttwo,
                                  &string, type);
    if (string_len == (size_t)-1)
    {
        error(ERR_NONFATAL, "invalid string for transform");
        return NULL;
    }

    val = readstrnum(string, string_len, &rn_warn);
    if (parens)
    {
        i = scan(scpriv, tv);
        if (i != ')')
        {
            error(ERR_NONFATAL, "expecting `)'");
            return NULL;
        }
    }

    if (rn_warn)
        error(ERR_WARNING | ERR_PASS1, "character constant too long");

    begintemp();
    addtotemp(EXPR_SIMPLE, val);

    i = scan(scpriv, tv);
    return finishtemp();
}

// Update expr6 to include hint parameter
static expr *expr6(int critical, void *scpriv, struct tokenval *tv, struct eval_hints *hint, int *opflags)
{
    int32_t type;
    expr *e;
    int32_t label_seg;
    int64_t label_ofs;
    int64_t tmpval;
    bool rn_warn;

    switch (i)
    {
    case '-':
        i = scan(scpriv, tv);
        e = expr6(critical, scpriv, tv, hint, opflags);
        if (!e)
            return NULL;
        return scalar_mult(e, -1L, false, hint);

    case '+':
        i = scan(scpriv, tv);
        return expr6(critical, scpriv, tv, hint, opflags);

    case '~':
        i = scan(scpriv, tv);
        e = expr6(critical, scpriv, tv, hint, opflags);
        if (!e)
            return NULL;
        if (is_just_unknown(e))
            return unknown_expr();
        else if (!is_simple(e))
        {
            error(ERR_NONFATAL, "`~' operator may only be applied to"
                                " scalar values");
            return NULL;
        }
        return scalarvect(~reloc_value(e));

    case '!':
        i = scan(scpriv, tv);
        e = expr6(critical, scpriv, tv, hint, opflags);
        if (!e)
            return NULL;
        if (is_just_unknown(e))
            return unknown_expr();
        else if (!is_simple(e))
        {
            error(ERR_NONFATAL, "`!' operator may only be applied to"
                                " scalar values");
            return NULL;
        }
        return scalarvect(!reloc_value(e));

    case TOKEN_SEG:
        i = scan(scpriv, tv);
        e = expr6(critical, scpriv, tv, hint, opflags);
        if (!e)
            return NULL;
        e = segment_part(e);
        if (!e)
            return NULL;
        if (is_unknown(e) && critical)
        {
            error(ERR_NONFATAL, "unable to determine segment base");
            return NULL;
        }
        return e;

    case TOKEN_FLOATIZE:
        return eval_floatize(tv->t_integer, scpriv, tv);

    case TOKEN_STRFUNC:
        return eval_strfunc(tv->t_integer, scpriv, tv);

    case '(':
        i = scan(scpriv, tv);
        e = bexpr(critical, scpriv, tv, hint, opflags);
        if (!e)
            return NULL;
        if (i != ')')
        {
            error(ERR_NONFATAL, "expecting `)'");
            return NULL;
        }
        i = scan(scpriv, tv);
        return e;

    case TOKEN_NUM:
    case TOKEN_STR:
    case TOKEN_REG:
    case TOKEN_ID:
    case TOKEN_INSN: /* Opcodes that occur here are really labels */
    case TOKEN_HERE:
    case TOKEN_BASE:
        begintemp();
        switch (i)
        {
        case TOKEN_NUM:
            addtotemp(EXPR_SIMPLE, tv->t_integer);
            break;
        case TOKEN_STR:
            tmpval = readstrnum(tv->t_charptr, tv->t_inttwo, &rn_warn);
            if (rn_warn)
                error(ERR_WARNING | ERR_PASS1, "character constant too long");
            addtotemp(EXPR_SIMPLE, tmpval);
            break;
        case TOKEN_REG:
            addtotemp(tv->t_integer, 1L);
            if (hint && hint->type == EAH_NOHINT)
                hint->base = tv->t_integer, hint->type = EAH_MAKEBASE;
            break;
        case TOKEN_ID:
        case TOKEN_INSN:
        case TOKEN_HERE:
        case TOKEN_BASE:
            /*
             * If !location->known, this indicates that no
             * symbol, Here or Base references are valid because we
             * are in preprocess-only mode.
             */
            if (!location->known)
            {
                error(ERR_NONFATAL,
                      "%s not supported in preprocess-only mode",
                      (i == TOKEN_HERE ? "`$'" : i == TOKEN_BASE ? "`$$'"
                                                                 : "symbol references"));
                addtotemp(EXPR_UNKNOWN, 1L);
                break;
            }

            type = EXPR_SIMPLE; /* might get overridden by UNKNOWN */
            if (i == TOKEN_BASE)
            {
                label_seg = in_abs_seg ? abs_seg : location->segment;
                label_ofs = 0;
            }
            else if (i == TOKEN_HERE)
            {
                label_seg = in_abs_seg ? abs_seg : location->segment;
                label_ofs = in_abs_seg ? abs_offset : location->offset;
            }
            else
            {
                if (!labelfunc(tv->t_charptr, &label_seg, &label_ofs))
                {
                    char *tmp_scope = local_scope(tv->t_charptr);
                    char *tmp_scope_ptr = NULL;
                    if (tmp_scope)
                    {
                        tmp_scope_ptr = as_strdup(tmp_scope);
                    }

                    if (critical == 2)
                    {
                        error(ERR_NONFATAL, "symbol `%s%s' undefined",
                              tmp_scope_ptr ? tmp_scope_ptr : "", tv->t_charptr);
                        if (tmp_scope_ptr)
                            as_free(tmp_scope_ptr);
                        return NULL;
                    }
                    else if (critical == 1)
                    {
                        error(ERR_NONFATAL,
                              "symbol `%s%s' not defined before use",
                              tmp_scope_ptr ? tmp_scope_ptr : "", tv->t_charptr);
                        if (tmp_scope_ptr)
                            as_free(tmp_scope_ptr);
                        return NULL;
                    }
                    else
                    {
                        if (opflags)
                            *opflags |= 1;
                        type = EXPR_UNKNOWN;
                        label_seg = NO_SEG;
                        label_ofs = 1;
                    }
                    if (tmp_scope_ptr)
                        as_free(tmp_scope_ptr);
                }
                if (opflags && is_extern(tv->t_charptr))
                    *opflags |= OPFLAG_EXTERN;
            }
            addtotemp(type, label_ofs);
            if (label_seg != NO_SEG)
                addtotemp(EXPR_SEGBASE + label_seg, 1L);
            break;
        }
        i = scan(scpriv, tv);
        return finishtemp();

    default:
        error(ERR_NONFATAL, "expression syntax error");
        return NULL;
    }
}

void eval_global_info(struct ofmt *output, lfunc lookup_label,
                      struct location *locp)
{
    outfmt = output;
    labelfunc = lookup_label;
    location = locp;
}

// Update evaluate to include hint parameter
expr *evaluate(scanner sc, void *scprivate, struct tokenval *tv,
               int *fwref, int critical, efunc report_error,
               struct eval_hints *hints)
{
    expr *e;
    expr *f = NULL;

    if (hints)
        hints->type = EAH_NOHINT;

    if (critical & CRITICAL)
    {
        critical &= ~CRITICAL;
        bexpr = rexp0;
    }
    else
        bexpr = expr0;

    scan = sc;
    error = report_error;

    if (tv->t_type == TOKEN_INVALID)
        i = scan(scprivate, tv);
    else
        i = tv->t_type;

    while (ntempexprs) /* initialize temporary storage */
        as_free(tempexprs[--ntempexprs]);

    e = bexpr(critical, scprivate, tv, hints, fwref);
    if (!e)
        return NULL;

    if (i == TOKEN_WRT)
    {
        i = scan(scprivate, tv); /* eat the WRT */
        f = expr6(critical, scprivate, tv, hints, fwref);
        if (!f)
            return NULL;
    }
    e = scalar_mult(e, 1L, false, hints); /* strip far-absolute segment part */
    if (f)
    {
        expr *g;
        if (is_just_unknown(f))
            g = unknown_expr();
        else
        {
            int64_t value;
            begintemp();
            if (!is_reloc(f))
            {
                error(ERR_NONFATAL, "invalid right-hand operand to WRT");
                return NULL;
            }
            value = reloc_seg(f);
            if (value == NO_SEG)
                value = reloc_value(f) | SEG_ABS;
            else if (!(value & SEG_ABS) && !(value % 2) && critical)
            {
                error(ERR_NONFATAL, "invalid right-hand operand to WRT");
                return NULL;
            }
            addtotemp(EXPR_WRT, value);
            g = finishtemp();
        }
        e = add_vectors(e, g);
    }
    return e;
}