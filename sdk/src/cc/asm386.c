#include "cc.h"

#define MAX_OPERANDS 3

typedef struct ASMInstr
{
    uint16_t sym;
    uint16_t opcode;
    uint16_t instr_type;
    uint8_t nb_ops;
    uint8_t op_type[MAX_OPERANDS]; // See OP_xxx
} ASMInstr;

#define OPC_JMP 0x01        // jmp operand
#define OPC_B 0x02          // only used with OPC_WL
#define OPC_WL 0x04         // accepts w, l or no suffix
#define OPC_REG 0x08        // register is added to opcode
#define OPC_MODRM 0x10      // modrm encoding
#define OPC_FWAIT 0x20      // add fwait opcode
#define OPC_TEST 0x40       // test opcodes
#define OPC_SHIFT 0x80      // shift opcodes
#define OPC_D16 0x0100      // generate data16 prefix
#define OPC_ARITH 0x0200    // arithmetic opcodes
#define OPC_SHORTJMP 0x0400 // short jmp operand
#define OPC_FARITH 0x0800   // FPU arithmetic opcodes

#define OPC_BWL (OPC_B | OPC_WL) // accepts b, w, l or no suffix

#define OPC_GROUP_SHIFT 13

// In order to compress the operand type, we use specific operands.
#define OPT_REG8 0  // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_REG16 1 // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_REG32 2 // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_MMX 3   // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_SSE 4   // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_CR 5    // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_TR 6    // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_DB 7    // warning: value is hardcoded from TOK_ASM_xxx
#define OPT_SEG 8
#define OPT_ST 9
#define OPT_IM8 10
#define OPT_IM8S 11
#define OPT_IM16 12
#define OPT_IM32 13
#define OPT_EAX 14   // %al, %ax or %eax register
#define OPT_ST0 15   // %st(0) register
#define OPT_CL 16    // %cl register
#define OPT_DX 17    // %dx register
#define OPT_ADDR 18  // OP_EA with only offset
#define OPT_INDIR 19 // *(expr)
#define OPT_FAR 20

// Composite types
#define OPT_IM 21   // IM8 | IM16 | IM32
#define OPT_REG 22  // REG8 | REG16 | REG32
#define OPT_REGW 23 // REG16 | REG32
#define OPT_IMW 24  // IM16 | IM32

// Can be ored with any OPT_xxx
#define OPT_EA 0x80

typedef struct Operand
{
    uint32_t type;
    int8_t reg;  // register, -1 if none
    int8_t reg2; // second register, -1 if none
    uint8_t shift;
    uint8_t sizehint;
    ExprValue e;
} Operand;

#define OP_REG8 (1 << OPT_REG8)
#define OP_REG16 (1 << OPT_REG16)
#define OP_REG32 (1 << OPT_REG32)
#define OP_MMX (1 << OPT_MMX)
#define OP_SSE (1 << OPT_SSE)
#define OP_CR (1 << OPT_CR)
#define OP_TR (1 << OPT_TR)
#define OP_DB (1 << OPT_DB)
#define OP_SEG (1 << OPT_SEG)
#define OP_ST (1 << OPT_ST)
#define OP_IM8 (1 << OPT_IM8)
#define OP_IM8S (1 << OPT_IM8S)
#define OP_IM16 (1 << OPT_IM16)
#define OP_IM32 (1 << OPT_IM32)
#define OP_EAX (1 << OPT_EAX)
#define OP_ST0 (1 << OPT_ST0)
#define OP_CL (1 << OPT_CL)
#define OP_DX (1 << OPT_DX)
#define OP_ADDR (1 << OPT_ADDR)
#define OP_INDIR (1 << OPT_INDIR)
#define OP_FAR (1 << OPT_FAR)

#define OP_EA 0x40000000
#define OP_REG (OP_REG8 | OP_REG16 | OP_REG32)
#define OP_IM OP_IM32

static const uint8_t reg_to_size[5] = {
    // [OP_REG8] = 0,
    // [OP_REG16] = 1,
    // [OP_REG32] = 2,
    0, 0, 1, 0, 2};

#define WORD_PREFIX_OPCODE 0x66

#define NB_TEST_OPCODES 30

static const uint8_t test_bits[NB_TEST_OPCODES] = {
    0x00, // o
    0x01, // no
    0x02, // b
    0x02, // c
    0x02, // nae
    0x03, // nb
    0x03, // nc
    0x03, // ae
    0x04, // e
    0x04, // z
    0x05, // ne
    0x05, // nz
    0x06, // be
    0x06, // na
    0x07, // nbe
    0x07, // a
    0x08, // s
    0x09, // ns
    0x0a, // p
    0x0a, // pe
    0x0b, // np
    0x0b, // po
    0x0c, // l
    0x0c, // nge
    0x0d, // nl
    0x0d, // ge
    0x0e, // le
    0x0e, // ng
    0x0f, // nle
    0x0f, // g
};

static const uint8_t segment_prefixes[] = {
    0x26, // es
    0x2e, // cs
    0x36, // ss
    0x3e, // ds
    0x64, // fs
    0x65  // gs
};

static const ASMInstr asm_instrs[] = {
#define ALT(x) x
#define DEF_ASM_OP0(name, opcode)
#define DEF_ASM_OP0L(name, opcode, group, instr_type) {TOK_ASM_##name, opcode, (instr_type | group << OPC_GROUP_SHIFT), 0},
#define DEF_ASM_OP1(name, opcode, group, instr_type, op0) {TOK_ASM_##name, opcode, (instr_type | group << OPC_GROUP_SHIFT), 1, {op0}},
#define DEF_ASM_OP2(name, opcode, group, instr_type, op0, op1) {TOK_ASM_##name, opcode, (instr_type | group << OPC_GROUP_SHIFT), 2, {op0, op1}},
#define DEF_ASM_OP3(name, opcode, group, instr_type, op0, op1, op2) {TOK_ASM_##name, opcode, (instr_type | group << OPC_GROUP_SHIFT), 3, {op0, op1, op2}},

#include "opcodes.h"

    // last operation
    {
        0,
    },
};

static const uint16_t op0_codes[] = {
#define ALT(x)
#define DEF_ASM_OP0(x, opcode) opcode,
#define DEF_ASM_OP0L(name, opcode, group, instr_type)
#define DEF_ASM_OP1(name, opcode, group, instr_type, op0)
#define DEF_ASM_OP2(name, opcode, group, instr_type, op0, op1)
#define DEF_ASM_OP3(name, opcode, group, instr_type, op0, op1, op2)

#include "opcodes.h"
};

static int get_reg_shift(CCState *s1)
{
    int shift, v;

    v = asm_int_expr(s1);
    switch (v)
    {
    case 1:
        shift = 0;
        break;
    case 2:
        shift = 1;
        break;
    case 4:
        shift = 2;
        break;
    case 8:
        shift = 3;
        break;
    default:
        expect("1, 2, 4 or 8 constant");
        shift = 0;
    }
    return shift;
}

static int asm_parse_reg(void)
{
    int reg;
    if (tok != '%')
        goto error_32;
    next();
    if (tok >= TOK_ASM_eax && tok <= TOK_ASM_edi)
    {
        reg = tok - TOK_ASM_eax;
        next();
        return reg;
    }
    else
    {
    error_32:
        expect("32 bit register");
        return 0;
    }
}

static void parse_operand(CCState *s1, Operand *op)
{
    ExprValue e;
    int reg, indir;
    const char *p;

    op->sizehint = 3;
    indir = 0;
    if (tok == '*')
    {
        next();
        indir = OP_INDIR;
    }

    if (tok == '%')
    {
        next();
        if (tok >= TOK_ASM_al && tok <= TOK_ASM_db7)
        {
            reg = tok - TOK_ASM_al;
            op->type = 1 << (reg >> 3); // NB: do not change constant order
            op->reg = reg & 7;
            if ((op->type & OP_REG) && op->reg == TREG_EAX)
            {
                op->type |= OP_EAX;
            }
            else if (op->type == OP_REG8 && op->reg == TREG_ECX)
            {
                op->type |= OP_CL;
            }
            else if (op->type == OP_REG16 && op->reg == TREG_EDX)
            {
                op->type |= OP_DX;
            }
        }
        else if (tok >= TOK_ASM_dr0 && tok <= TOK_ASM_dr7)
        {
            op->type = OP_DB;
            op->reg = tok - TOK_ASM_dr0;
        }
        else if (tok >= TOK_ASM_es && tok <= TOK_ASM_gs)
        {
            op->type = OP_SEG;
            op->reg = tok - TOK_ASM_es;
        }
        else if (tok == TOK_ASM_st)
        {
            op->type = OP_ST;
            op->reg = 0;
            next();
            if (tok == '(')
            {
                next();
                if (tok != TOK_PPNUM)
                    goto reg_error;
                p = tokc.cstr->data;
                reg = p[0] - '0';
                if ((unsigned)reg >= 8 || p[1] != '\0')
                    goto reg_error;
                op->reg = reg;
                next();
                skip(')');
            }
            if (op->reg == 0)
                op->type |= OP_ST0;
            goto no_skip;
        }
        else
        {
        reg_error:
            error("unknown register");
        }
        next();
    no_skip:;
    }
    else if (tok == '$')
    {
        // Constant value
        next();
        asm_expr(s1, &e);
        op->type = OP_IM32;
        op->e.v = e.v;
        op->e.sym = e.sym;
        if (!op->e.sym)
        {
            if (op->e.v == (uint8_t)op->e.v)
                op->type |= OP_IM8;
            if (op->e.v == (int8_t)op->e.v)
                op->type |= OP_IM8S;
            if (op->e.v == (uint16_t)op->e.v)
                op->type |= OP_IM16;
        }
    }
    else
    {
        // Address(reg,reg2,shift) with all variants
        op->type = OP_EA;
        op->reg = -1;
        op->reg2 = -1;
        op->shift = 0;
        if (tok != '(')
        {
            asm_expr(s1, &e);
            op->e.v = e.v;
            op->e.sym = e.sym;
        }
        else
        {
            op->e.v = 0;
            op->e.sym = NULL;
        }
        if (tok == '(')
        {
            next();
            if (tok != ',')
            {
                op->reg = asm_parse_reg();
            }
            if (tok == ',')
            {
                next();
                if (tok != ',')
                {
                    op->reg2 = asm_parse_reg();
                }
                if (tok == ',')
                {
                    next();
                    op->shift = get_reg_shift(s1);
                }
            }
            skip(')');
        }
        if (op->reg == -1 && op->reg2 == -1)
            op->type |= OP_ADDR;
    }
    op->type |= indir;
}

static void masm_logic_or_reg(CCState *s1, Operand *op)
{
    op->reg = -1;
    op->reg2 = -1;
    op->shift = 0;
    op->sizehint = 3;
    op->e.v = 0;
    op->e.sym = NULL;
    if (tok >= TOK_ASM_eax && tok <= TOK_ASM_edi)
    {
        int reg = tok - TOK_ASM_eax;
        next();
        if (tok == '*')
        {
            next();
            op->reg2 = reg;
            op->shift = get_reg_shift(s1);
        }
        else
        {
            op->reg = reg;
        }
    }
    else
    {
        asm_expr_logic(s1, &op->e);
    }
}

static void masm_expr(CCState *s1, Operand *op)
{
    int opr;
    int indir;
    Operand op2;
    indir = 0;
    if (tok == '[')
    {
        indir = 1;
        next();
    }
    masm_logic_or_reg(s1, op);
    for (;;)
    {
        opr = tok;
        if (opr != '+' && opr != '-' && opr != '[')
            break;
        if (opr == '[')
        {
            if (indir)
                break;
            opr = '+';
            indir = 1;
        }
        next();
        masm_logic_or_reg(s1, &op2);

        /* combine registers */
        if (op2.reg != -1 || op2.reg2 != -1)
        {
            if (opr != '+')
                expect("positive offset");
            if (op2.reg != -1)
            {
                if (op->reg == -1)
                {
                    op->reg = op2.reg;
                }
                else if (op->reg2 == -1)
                {
                    op->reg2 = op2.reg;
                    op->shift = 0;
                }
                else
                {
                    error("invalid adressing mode");
                }
            }
            else if (op2.reg2 != -1)
            {
                if (op->reg2 == -1)
                {
                    op->reg2 = op2.reg2;
                    op->shift = op2.shift;
                }
                else
                {
                    error("invalid adressing mode");
                }
            }
        }

        // Combine values
        if (opr == '+')
        {
            op->e.v += op2.e.v;
        }
        else
        {
            op->e.v -= op2.e.v;
        }

        // Combine symbols
        if (opr == '+')
        {
            if (op->e.sym != NULL && op2.e.sym != NULL)
                goto cannot_relocate;
            if (op->e.sym == NULL && op2.e.sym != NULL)
                op->e.sym = op2.e.sym;
        }
        else
        {
            if (!op->e.sym && !op2.e.sym)
            {
                // OK
            }
            else if (op->e.sym && !op2.e.sym)
            {
                // OK
            }
            else if (op->e.sym && op2.e.sym)
            {
                if (op->e.sym == op2.e.sym)
                {
                    // OK
                }
                else if (op->e.sym->r == op2.e.sym->r && op->e.sym->r != 0)
                {
                    // We also accept defined symbols in the same section (TODO)
                    op->e.v += (long)op->e.sym->next - (long)op2.e.sym->next;
                }
                else
                {
                    goto cannot_relocate;
                }
                op->e.sym = NULL; // same symbols can be subtracted from NULL
            }
            else
            {
            cannot_relocate:
                error("invalid operation with label");
            }
        }
    }
    if (indir)
    {
        op->type |= OP_EA | OP_INDIR;
        skip(']');
    }
}

static void parse_masm_operand(CCState *s1, Operand *op)
{
    int reg;
    const char *p;

    op->type = 0;
    op->reg = -1;
    op->reg2 = -1;
    op->shift = 0;
    op->e.v = 0;
    op->e.sym = NULL;
    if (tok >= TOK_ASM_al && tok <= TOK_ASM_db7)
    {
        reg = tok - TOK_ASM_al;
        op->type = 1 << (reg >> 3); // NB: do not change constant order
        op->reg = reg & 7;
        if ((op->type & OP_REG) && op->reg == TREG_EAX)
        {
            op->type |= OP_EAX;
        }
        else if (op->type == OP_REG8 && op->reg == TREG_ECX)
        {
            op->type |= OP_CL;
        }
        else if (op->type == OP_REG16 && op->reg == TREG_EDX)
        {
            op->type |= OP_DX;
        }
        next();
    }
    else if (tok >= TOK_ASM_dr0 && tok <= TOK_ASM_dr7)
    {
        op->type = OP_DB;
        op->reg = tok - TOK_ASM_dr0;
        next();
    }
    else if (tok >= TOK_ASM_es && tok <= TOK_ASM_gs)
    {
        op->type = OP_SEG;
        op->reg = tok - TOK_ASM_es;
        next();
    }
    else if (tok == TOK_ASM_st)
    {
        op->type = OP_ST;
        op->reg = 0;
        next();
        if (tok == '(')
        {
            next();
            if (tok != TOK_PPNUM)
                error("illegal fp register");
            p = tokc.cstr->data;
            reg = p[0] - '0';
            next();
            if ((unsigned)reg >= 8 || p[1] != '\0')
                error("illegal fp register number");
            op->reg = reg;
            next();
            skip(')');
        }
        if (op->reg == 0)
            op->type |= OP_ST0;
    }
    else
    {
        int offset = 0;
        if (tok == TOK_ASM_offset)
        {
            offset = 1;
            next();
        }
        else if (tok == TOK_ASM_far)
        {
            op->type |= OP_FAR;
        }
        if (tok == TOK_ASM_byte || tok == TOK_ASM_word ||
            tok == TOK_ASM_dword || tok == TOK_ASM_fword)
        {
            switch (tok)
            {
            case TOK_ASM_byte:
                op->sizehint = 0;
                break;
            case TOK_ASM_word:
                op->sizehint = 1;
                break;
            case TOK_ASM_dword:
                op->sizehint = 2;
                break;
            case TOK_ASM_fword:
                op->sizehint = 2;
                op->type |= OP_FAR;
                break;
            }
            next();
            op->type |= OP_EA;
            skip(TOK_ASM_ptr);
        }
        masm_expr(s1, op);
        if (op->sizehint == 3 && op->e.sym)
        {
            int align;
            switch (type_size(&op->e.sym->type, &align))
            {
            case 1:
                op->sizehint = 0;
                break;
            case 2:
                op->sizehint = 1;
                break;
            case 4:
                op->sizehint = 2;
                break;
            }
        }
        if (offset)
        {
            if (op->reg != -1 && op->reg2 != -1)
                error("register not allowed in offset");
            op->type &= ~OP_EA;
            op->type |= OP_IM32;
        }
        else if ((op->type & OP_EA) == 0 && op->reg == -1 && op->reg2 == -1 && !op->e.sym)
        {
            // Constant
            op->type = OP_IM32;
            if (op->e.v == (uint8_t)op->e.v)
                op->type |= OP_IM8;
            if (op->e.v == (int8_t)op->e.v)
                op->type |= OP_IM8S;
            if (op->e.v == (uint16_t)op->e.v)
                op->type |= OP_IM16;
        }
        else if (op->reg == -1 && op->reg2 == -1 && op->e.v == 0 &&
                 op->e.sym && (op->e.sym->r & VT_VALMASK) == VT_LOCAL)
        {
            // Local variable
            op->type = OP_EA;
            op->reg = TOK_ASM_ebp - TOK_ASM_eax;
            op->e.v = op->e.sym->c;
            op->e.sym = NULL;
        }
        else
        {
            // address+reg+reg2*n
            op->type |= OP_EA;
            if (op->reg == -1 && op->reg2 == -1)
                op->type |= OP_ADDR;
        }
    }
}

void gen_expr32(ExprValue *pe)
{
    if (pe->sym)
    {
        greloc(pe->sym, pe->v, 0);
    }
    else
    {
        gen_le32(pe->v);
    }
}

void gen_disp32(ExprValue *pe)
{
    Sym *sym;

    sym = pe->sym;
    if (sym)
    {
        greloc(sym, pe->v - 4, 1);
    }
    else
    {
        // put an empty relocation
        greloc(NULL, pe->v - 4, 1);
    }
}

// Generate the modrm operand
void asm_modrm(int reg, Operand *op)
{
    int mod, reg1, reg2, sib_reg1;

    if (op->type & (OP_REG | OP_MMX | OP_SSE))
    {
        g(0xc0 + (reg << 3) + op->reg);
    }
    else if (op->reg == -1 && op->reg2 == -1)
    {
        // Displacement only
        g(0x05 + (reg << 3));
        gen_expr32(&op->e);
    }
    else
    {
        sib_reg1 = op->reg;
        // Fist compute displacement encoding
        if (sib_reg1 == -1)
        {
            sib_reg1 = 5;
            mod = 0x00;
        }
        else if (op->e.v == 0 && !op->e.sym && op->reg != 5)
        {
            mod = 0x00;
        }
        else if (op->e.v == (int8_t)op->e.v && !op->e.sym)
        {
            mod = 0x40;
        }
        else
        {
            mod = 0x80;
        }

        // Compute if sib byte needed
        reg1 = op->reg;
        if (op->reg2 != -1)
            reg1 = 4;
        g(mod + (reg << 3) + reg1);
        if (reg1 == 4)
        {
            // Add sib byte
            reg2 = op->reg2;
            if (reg2 == -1)
                reg2 = 4; // Indicate no index
            g((op->shift << 6) + (reg2 << 3) + sib_reg1);
        }

        // Add offset
        if (mod == 0x40)
        {
            g(op->e.v);
        }
        else if (mod == 0x80 || op->reg == -1)
        {
            gen_expr32(&op->e);
        }
    }
}

void asm_opcode(CCState *s1, int opcode)
{
    const ASMInstr *pa;
    int i, modrm_index, reg, v, op1, seg_prefix;
    int nb_ops, s, ss;
    Operand ops[MAX_OPERANDS], *pop;
    int op_type[3]; // Decoded op type

    // Process prefixes in masm mode
    if (parse_flags & PARSE_FLAG_MASM)
    {
        while (opcode >= TOK_ASM_lock && opcode <= TOK_ASM_repnz)
        {
            g(op0_codes[opcode - TOK_ASM_pusha]);
            opcode = tok;
            next();
        }
    }

    // Get operands
    pop = ops;
    nb_ops = 0;
    seg_prefix = 0;
    for (;;)
    {
        if (tok == ';' || tok == TOK_LINEFEED || tok == '}')
            break;
        if (nb_ops >= MAX_OPERANDS)
        {
            error("incorrect number of operands");
        }

        if (parse_flags & PARSE_FLAG_MASM)
        {
            parse_masm_operand(s1, pop);
        }
        else
        {
            parse_operand(s1, pop);
        }

        if (tok == ':')
        {
            if (pop->type != OP_SEG || seg_prefix)
            {
                error("incorrect prefix");
            }
            seg_prefix = segment_prefixes[pop->reg];
            next();
            if (parse_flags & PARSE_FLAG_MASM)
            {
                parse_masm_operand(s1, pop);
            }
            else
            {
                parse_operand(s1, pop);
            }
            if (!(pop->type & OP_EA))
            {
                error("segment prefix must be followed by memory reference");
            }
        }

#ifdef ASM_DEBUG
        printf("op%d: type=%x", nb_ops, pop->type);
        if (pop->reg != -1)
            printf(" reg=%x", pop->reg);
        if (pop->reg2 != -1)
            printf(" reg2=%x", pop->reg2);
        if (pop->shift != 0)
            printf(" shift=%d", pop->shift);
        if (pop->sizehint != 3)
            printf(" size=%d", pop->sizehint);
        if (pop->e.v != 0)
            printf(" v=%x", pop->e.v);
        if (pop->e.sym)
            printf(" sym.v=%x sym.r=%x sym.c=%x sym.type=%x", pop->e.sym->v, pop->e.sym->r, pop->e.sym->c, pop->e.sym->type);
        printf("\n");
#endif

        pop++;
        nb_ops++;
        if (tok != ',')
            break;
        next();
    }

    // Swap operands for masm mode
    if (parse_flags & PARSE_FLAG_MASM)
    {
        int a, b;
        for (a = 0, b = nb_ops - 1; a < b; a++, b--)
        {
            Operand tmp;
            tmp = ops[a];
            ops[a] = ops[b];
            ops[b] = tmp;
        }
    }

    s = 0;

    // Optimize matching by using a lookup table (no hashing is needed!)
    for (pa = asm_instrs; pa->sym != 0; pa++)
    {
        s = 0;
        if (pa->instr_type & OPC_FARITH)
        {
            v = opcode - pa->sym;
            if (!((unsigned)v < 8 * 6 && (v % 6) == 0))
                continue;
        }
        else if (pa->instr_type & OPC_ARITH)
        {
            if (!(opcode >= pa->sym && opcode < pa->sym + 8 * 4))
                continue;
            goto compute_size;
        }
        else if (pa->instr_type & OPC_SHIFT)
        {
            if (!(opcode >= pa->sym && opcode < pa->sym + 7 * 4))
                continue;
            goto compute_size;
        }
        else if (pa->instr_type & OPC_TEST)
        {
            if (!(opcode >= pa->sym && opcode < pa->sym + NB_TEST_OPCODES))
                continue;
        }
        else if (pa->instr_type & OPC_B)
        {
            if (!(opcode >= pa->sym && opcode <= pa->sym + 3))
                continue;
        compute_size:
            s = (opcode - pa->sym) & 3;
        }
        else if (pa->instr_type & OPC_WL)
        {
            if (!(opcode >= pa->sym && opcode <= pa->sym + 2))
                continue;
            s = opcode - pa->sym + 1;
        }
        else
        {
            if (pa->sym != opcode)
                continue;
        }
        if (pa->nb_ops != nb_ops)
            continue;

        // Now decode and check each operand
        for (i = 0; i < nb_ops; i++)
        {
            int _op1, op2;
            _op1 = pa->op_type[i];
            op2 = _op1 & 0x1f;
            switch (op2)
            {
            case OPT_IM:
                v = OP_IM8 | OP_IM16 | OP_IM32;
                break;
            case OPT_REG:
                v = OP_REG8 | OP_REG16 | OP_REG32;
                break;
            case OPT_REGW:
                v = OP_REG16 | OP_REG32;
                break;
            case OPT_IMW:
                v = OP_IM16 | OP_IM32;
                break;
            default:
                v = 1 << op2;
            }
            if (_op1 & OPT_EA)
                v |= OP_EA;
            op_type[i] = v;
            if ((ops[i].type & v) == 0)
                goto next;
        }
        // All is matching!
        break;
    next:;
    }

    if (pa->sym == 0)
    {
        if (opcode >= TOK_ASM_pusha && opcode <= TOK_ASM_emms)
        {
            int b;
            b = op0_codes[opcode - TOK_ASM_pusha];
            if (b & 0xff00)
                g(b >> 8);
            g(b);
            return;
        }
        else
        {
            error("unknown opcode '%s'", get_tok_str(opcode, NULL));
        }
    }

    // If the size is unknown, then evaluate it (OPC_B or OPC_WL case)
    if (s == 3)
    {
        for (i = 0; s == 3 && i < nb_ops; i++)
        {
            if ((ops[i].type & OP_REG) && !(op_type[i] & (OP_CL | OP_DX)))
            {
                s = reg_to_size[ops[i].type & OP_REG];
            }
        }
        if (s == 3)
        {
            if ((opcode == TOK_ASM_push || opcode == TOK_ASM_pop) && (ops[0].type & (OP_SEG | OP_IM8S | OP_IM32)))
            {
                s = 2;
            }
            else
            {
                for (i = 0; s == 3 && i < nb_ops; i++)
                    s = ops[i].sizehint;
                if (s == 3)
                    error("cannot infer operand size");
            }
        }
    }

    // Generate data16 prefix if needed
    ss = s;
    if (s == 1 || (pa->instr_type & OPC_D16))
    {
        g(WORD_PREFIX_OPCODE);
    }
    else if (s == 2)
    {
        s = 1;
    }

    // Now generate the operation
    if (pa->instr_type & OPC_FWAIT)
        g(0x9b);
    if (seg_prefix)
        g(seg_prefix);

    v = pa->opcode;
    if ((v == 0x69) != 0)
    {
        // Kludge for imul $im, %reg
        nb_ops = 3;
        ops[2] = ops[1];
    }
    else if (v == 0xcd && ops[0].e.v == 3 && !ops[0].e.sym)
    {
        v--; // int $3 case
        nb_ops = 0;
    }
    else if ((v == 0x06 || v == 0x07))
    {
        if (ops[0].reg >= 4)
        {
            // push/pop %fs or %gs
            v = 0x0fa0 + (v - 0x06) + ((ops[0].reg - 4) << 3);
        }
        else
        {
            v += ops[0].reg << 3;
        }
        nb_ops = 0;
    }
    else if (v <= 0x05)
    {
        // arith case
        v += ((opcode - TOK_ASM_addb) >> 2) << 3;
    }
    else if ((pa->instr_type & (OPC_FARITH | OPC_MODRM)) == OPC_FARITH)
    {
        // fpu arith case
        v += ((opcode - pa->sym) / 6) << 3;
    }

    if (pa->instr_type & OPC_REG)
    {
        for (i = 0; i < nb_ops; i++)
        {
            if (op_type[i] & (OP_REG | OP_ST))
            {
                v += ops[i].reg;
                break;
            }
        }
        // mov $im, %reg case
        if (pa->opcode == 0xb0 && s >= 1)
            v += 7;
    }

    if (pa->instr_type & OPC_B)
        v += s;
    if (pa->instr_type & OPC_TEST)
        v += test_bits[opcode - pa->sym];

    if ((pa->instr_type & OPC_SHORTJMP) && (pa->instr_type & OPC_JMP))
    {
        Sym *sym;

        sym = ops[0].e.sym;
        if (sym && sym->type.t == VT_LABEL)
        {
            // Jump to label
            if (v == 0xeb)
            {
                v = 0;
            }
            else
            {
                v += 0x20;
            }

            if (sym->r)
            {
                // Label defined
                gjmp((long)sym->next, v);
            }
            else
            {
                // Label undefined
                sym->next = (void *)gjmp((long)sym->next, v);
            }
            return;
        }
        else
        {
            // Jumps to external symbols are always long jumps.
            if (v == 0xeb)
            {
                v = 0xe9;
            }
            else
            {
                v += 0x0f10;
            }
        }
    }

    op1 = v >> 8;
    if (op1)
        g(op1);
    g(v);

#ifdef ASM_DEBUG
    printf("v=%x instr_type=%x\n", v, pa->instr_type);
#endif

    // Determine which operand to use for modrm
    modrm_index = 0;
    if (pa->instr_type & OPC_SHIFT)
    {
        reg = (opcode - pa->sym) >> 2;
        if (reg == 6)
            reg = 7;
    }
    else if (pa->instr_type & OPC_ARITH)
    {
        reg = (opcode - pa->sym) >> 2;
    }
    else if (pa->instr_type & OPC_FARITH)
    {
        reg = (opcode - pa->sym) / 6;
    }
    else
    {
        reg = (pa->instr_type >> OPC_GROUP_SHIFT) & 7;
    }

    if (pa->instr_type & OPC_MODRM)
    {
        // First look for an ea operand
        for (i = 0; i < nb_ops; i++)
        {
            if (op_type[i] & OP_EA)
                goto modrm_found;
        }
        // Then if not found, a register or indirection (shift instructions)
        for (i = 0; i < nb_ops; i++)
        {
            if (op_type[i] & (OP_REG | OP_MMX | OP_SSE | OP_INDIR | OP_FAR))
                goto modrm_found;
        }
#ifdef ASM_DEBUG
        error("bad op table");
#endif
    modrm_found:
        modrm_index = i;
        // If a register is used in another operand then it is used instead of group
        for (i = 0; i < nb_ops; i++)
        {
            v = op_type[i];
            if (i != modrm_index && (v & (OP_REG | OP_MMX | OP_SSE | OP_CR | OP_TR | OP_DB | OP_SEG)))
            {
                reg = ops[i].reg;
                break;
            }
        }

        asm_modrm(reg, &ops[modrm_index]);
    }

    // Emit constants
    if (pa->opcode == 0x9a || pa->opcode == 0xea)
    {
        // ljmp or lcall kludge
        gen_expr32(&ops[1].e);
        if (ops[0].e.sym)
            error("cannot relocate");
        gen_le16(ops[0].e.v);
    }
    else
    {
        for (i = 0; i < nb_ops; i++)
        {
            v = op_type[i];
            if (v & (OP_IM8 | OP_IM16 | OP_IM32 | OP_IM8S | OP_ADDR))
            {
                // If multiple sizes are given it means we must look at the op size
                if (v == (OP_IM8 | OP_IM16 | OP_IM32) || v == (OP_IM16 | OP_IM32))
                {
                    if (ss == 0)
                    {
                        v = OP_IM8;
                    }
                    else if (ss == 1)
                    {
                        v = OP_IM16;
                    }
                    else
                    {
                        v = OP_IM32;
                    }
                }
                if (v & (OP_IM8 | OP_IM8S))
                {
                    if (ops[i].e.sym)
                        goto error_relocate;
                    g(ops[i].e.v);
                }
                else if (v & OP_IM16)
                {
                    if (ops[i].e.sym)
                    {
                    error_relocate:
                        error("cannot relocate");
                    }
                    gen_le16(ops[i].e.v);
                }
                else
                {
                    if (pa->instr_type & (OPC_JMP | OPC_SHORTJMP))
                    {
                        gen_disp32(&ops[i].e);
                    }
                    else
                    {
                        gen_expr32(&ops[i].e);
                    }
                }
            }
        }
    }
}

// Return the constraint priority (we allocate first the lowest
// numbered constraints)
int constraint_priority(const char *str)
{
    int priority, c, pr;

    // we take the lowest priority
    priority = 0;
    for (;;)
    {
        c = *str;
        if (c == '\0')
            break;
        str++;
        switch (c)
        {
        case 'A':
            pr = 0;
            break;

        case 'a':
        case 'b':
        case 'c':
        case 'd':
        case 'S':
        case 'D':
            pr = 1;
            break;

        case 'q':
            pr = 2;
            break;

        case 'r':
            pr = 3;
            break;

        case 'N':
        case 'M':
        case 'I':
        case 'i':
        case 'm':
        case 'g':
            pr = 4;
            break;

        default:
            error("unknown constraint '%c'", c);
            pr = 0;
        }
        if (pr > priority)
            priority = pr;
    }
    return priority;
}

const char *skip_constraint_modifiers(const char *p)
{
    while (*p == '=' || *p == '&' || *p == '+' || *p == '%')
        p++;
    return p;
}

#define REG_OUT_MASK 0x01
#define REG_IN_MASK 0x02

#define is_reg_allocated(reg) (regs_allocated[reg] & reg_mask)

void asm_compute_constraints(ASMOperand *operands,
                             int nb_operands, int nb_outputs,
                             const uint8_t *clobber_regs, int *pout_reg)
{
    ASMOperand *op;
    int sorted_op[MAX_ASM_OPERANDS];
    int i, j, k, p1, p2, tmp, reg, c, reg_mask;
    const char *str;
    uint8_t regs_allocated[NB_ASM_REGS];

    // Init fields
    for (i = 0; i < nb_operands; i++)
    {
        op = &operands[i];
        op->input_index = -1;
        op->ref_index = -1;
        op->reg = -1;
        op->is_memory = 0;
        op->is_rw = 0;
    }

    // Compute constraint priority and evaluate references to output
    // constraints if input constraints.
    for (i = 0; i < nb_operands; i++)
    {
        op = &operands[i];
        str = op->constraint;
        str = skip_constraint_modifiers(str);
        if (is_num(*str) || *str == '[')
        {
            // This is a reference to another constraint
            k = find_constraint(operands, nb_operands, str, NULL);
            if (k < 0 || k >= i || i < nb_outputs)
            {
                error("invalid reference in constraint %d ('%s')", i, str);
            }
            op->ref_index = k;
            if (operands[k].input_index >= 0)
            {
                error("cannot reference twice the same operand");
            }
            operands[k].input_index = i;
            op->priority = 5;
        }
        else
        {
            op->priority = constraint_priority(str);
        }
    }

    // Sort operands according to their priority
    for (i = 0; i < nb_operands; i++)
        sorted_op[i] = i;
    for (i = 0; i < nb_operands - 1; i++)
    {
        for (j = i + 1; j < nb_operands; j++)
        {
            p1 = operands[sorted_op[i]].priority;
            p2 = operands[sorted_op[j]].priority;
            if (p2 < p1)
            {
                tmp = sorted_op[i];
                sorted_op[i] = sorted_op[j];
                sorted_op[j] = tmp;
            }
        }
    }

    for (i = 0; i < NB_ASM_REGS; i++)
    {
        if (clobber_regs[i])
        {
            regs_allocated[i] = REG_IN_MASK | REG_OUT_MASK;
        }
        else
        {
            regs_allocated[i] = 0;
        }
    }

    // esp cannot be used
    regs_allocated[4] = REG_IN_MASK | REG_OUT_MASK;
    // ebp cannot be used yet
    regs_allocated[5] = REG_IN_MASK | REG_OUT_MASK;

    // Allocate registers and generate corresponding asm moves
    for (i = 0; i < nb_operands; i++)
    {
        j = sorted_op[i];
        op = &operands[j];
        str = op->constraint;
        // No need to allocate references
        if (op->ref_index >= 0)
            continue;
        // Select if register is used for output, input or both
        if (op->input_index >= 0)
        {
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
        }
        else if (j < nb_outputs)
        {
            reg_mask = REG_OUT_MASK;
        }
        else
        {
            reg_mask = REG_IN_MASK;
        }
    try_next:
        c = *str++;
        switch (c)
        {
        case '=':
            goto try_next;

        case '+':
            op->is_rw = 1;
            // FALL THRU

        case '&':
            if (j >= nb_outputs)
            {
                error("'%c' modifier can only be applied to outputs", c);
            }
            reg_mask = REG_IN_MASK | REG_OUT_MASK;
            goto try_next;

        case 'A':
            // Allocate both eax and edx
            if (is_reg_allocated(TREG_EAX) || is_reg_allocated(TREG_EDX))
                goto try_next;
            op->is_llong = 1;
            op->reg = TREG_EAX;
            regs_allocated[TREG_EAX] |= reg_mask;
            regs_allocated[TREG_EDX] |= reg_mask;
            break;

        case 'a':
            reg = TREG_EAX;
            goto alloc_reg;

        case 'b':
            reg = 3;
            goto alloc_reg;

        case 'c':
            reg = TREG_ECX;
            goto alloc_reg;

        case 'd':
            reg = TREG_EDX;
            goto alloc_reg;

        case 'S':
            reg = 6;
            goto alloc_reg;

        case 'D':
            reg = 7;
        alloc_reg:
            if (is_reg_allocated(reg))
                goto try_next;
            goto reg_found;

        case 'q':
            // eax, ebx, ecx or edx
            for (reg = 0; reg < 4; reg++)
            {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;

        case 'r':
            // Any general register
            for (reg = 0; reg < 8; reg++)
            {
                if (!is_reg_allocated(reg))
                    goto reg_found;
            }
            goto try_next;

        reg_found:
            // Now we can reload in the register
            op->is_llong = 0;
            op->reg = reg;
            regs_allocated[reg] |= reg_mask;
            break;

        case 'i':
            if ((op->vt->r & (VT_VALMASK | VT_LVAL)) != VT_CONST)
                goto try_next;
            break;

        case 'I':
        case 'N':
        case 'M':
            if ((op->vt->r & (VT_VALMASK | VT_LVAL | VT_SYM)) != VT_CONST)
                goto try_next;
            break;

        case 'm':
        case 'g':
            // Nothing special to do because the operand is already in
            // memory, except if the pointer itself is stored in a
            // memory variable (VT_LLOCAL case)
            // TODO: fix constant case
            // If it is a reference to a memory zone, it must lie
            // in a register, so we reserve the register in the
            // input registers and a load will be generated
            // later
            if (j < nb_outputs || c == 'm')
            {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL)
                {
                    // Any general register
                    for (reg = 0; reg < 8; reg++)
                    {
                        if (!(regs_allocated[reg] & REG_IN_MASK))
                            goto reg_found1;
                    }
                    goto try_next;
                reg_found1:
                    // Now we can reload in the register
                    regs_allocated[reg] |= REG_IN_MASK;
                    op->reg = reg;
                    op->is_memory = 1;
                }
            }
            break;

        default:
            error("asm constraint %d ('%s') could not be satisfied", j, op->constraint);
        }

        // If a reference is present for that operand, we assign it too
        if (op->input_index >= 0)
        {
            operands[op->input_index].reg = op->reg;
            operands[op->input_index].is_llong = op->is_llong;
        }
    }

    // Compute out_reg. It is used to store outputs registers to memory
    // locations references by pointers (VT_LLOCAL case)
    *pout_reg = -1;
    for (i = 0; i < nb_operands; i++)
    {
        op = &operands[i];
        if (op->reg >= 0 && (op->vt->r & VT_VALMASK) == VT_LLOCAL && !op->is_memory)
        {
            for (reg = 0; reg < 8; reg++)
            {
                if (!(regs_allocated[reg] & REG_OUT_MASK))
                    goto reg_found2;
            }
            error("could not find free output register for reloading");
        reg_found2:
            *pout_reg = reg;
            break;
        }
    }

#ifdef ASM_DEBUG
    // Print sorted constraints
    for (i = 0; i < nb_operands; i++)
    {
        j = sorted_op[i];
        op = &operands[j];
        printf("%%%d [%s]: \"%s\" r=0x%04x reg=%d\n",
               j,
               op->id ? get_tok_str(op->id, NULL) : "",
               op->constraint,
               op->vt->r,
               op->reg);
    }
    if (*pout_reg >= 0)
        printf("out_reg=%d\n", *pout_reg);
#endif
}

void subst_asm_operand(CString *add_str, SValue *sv, int modifier)
{
    int r, reg, size, val;
    char buf[64];

    r = sv->r;
    if ((r & VT_VALMASK) == VT_CONST)
    {
        if (!(r & VT_LVAL) && modifier != 'c' && modifier != 'n')
            cstr_ccat(add_str, '$');
        if (r & VT_SYM)
        {
            cstr_cat(add_str, get_tok_str(sv->sym->v, NULL));
            if (sv->c.i != 0)
            {
                cstr_ccat(add_str, '+');
            }
            else
            {
                return;
            }
        }
        val = sv->c.i;
        if (modifier == 'n')
            val = -val;
        snprintf(buf, sizeof(buf), "%d", sv->c.i);
        cstr_cat(add_str, buf);
    }
    else if ((r & VT_VALMASK) == VT_LOCAL)
    {
        snprintf(buf, sizeof(buf), "%d(%%ebp)", sv->c.i);
        cstr_cat(add_str, buf);
    }
    else if (r & VT_LVAL)
    {
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            error("internal compiler error");
        snprintf(buf, sizeof(buf), "(%%%s)", get_tok_str(TOK_ASM_eax + reg, NULL));
        cstr_cat(add_str, buf);
    }
    else
    {
        // Register case
        reg = r & VT_VALMASK;
        if (reg >= VT_CONST)
            error("internal compiler error");

        // Choose register operand size
        if ((sv->type.t & VT_BTYPE) == VT_BYTE)
        {
            size = 1;
        }
        else if ((sv->type.t & VT_BTYPE) == VT_SHORT)
        {
            size = 2;
        }
        else
        {
            size = 4;
        }
        if (size == 1 && reg >= 4)
            size = 4;

        if (modifier == 'b')
        {
            if (reg >= 4)
                error("cannot use byte register");
            size = 1;
        }
        else if (modifier == 'h')
        {
            if (reg >= 4)
                error("cannot use byte register");
            size = -1;
        }
        else if (modifier == 'w')
        {
            size = 2;
        }

        switch (size)
        {
        case -1:
            reg = TOK_ASM_ah + reg;
            break;
        case 1:
            reg = TOK_ASM_al + reg;
            break;
        case 2:
            reg = TOK_ASM_ax + reg;
            break;
        default:
            reg = TOK_ASM_eax + reg;
            break;
        }
        snprintf(buf, sizeof(buf), "%%%s", get_tok_str(reg, NULL));
        cstr_cat(add_str, buf);
    }
}

// Generate prolog and epilog code for asm statment
void asm_gen_code(ASMOperand *operands, int nb_operands, int nb_outputs, int is_output,
                  uint8_t *clobber_regs, int out_reg)
{
    uint8_t regs_allocated[NB_ASM_REGS];
    ASMOperand *op;
    int i, reg;
    static uint8_t reg_saved[NB_SAVED_REGS] = {3, 6, 7};

    // Mark all used registers
    memcpy(regs_allocated, clobber_regs, sizeof(regs_allocated));
    for (i = 0; i < nb_operands; i++)
    {
        op = &operands[i];
        if (op->reg >= 0)
            regs_allocated[op->reg] = 1;
    }
    if (!is_output)
    {
        // Generate reg save code
        for (i = 0; i < NB_SAVED_REGS; i++)
        {
            reg = reg_saved[i];
            if (regs_allocated[reg])
                g(0x50 + reg);
        }

        // Generate load code
        for (i = 0; i < nb_operands; i++)
        {
            op = &operands[i];
            if (op->reg >= 0)
            {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL && op->is_memory)
                {
                    // Memory reference case (for both input and output cases)
                    SValue sv;
                    sv = *op->vt;
                    sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                    load(op->reg, &sv);
                }
                else if (i >= nb_outputs || op->is_rw)
                {
                    // Load value in register
                    load(op->reg, op->vt);
                    if (op->is_llong)
                    {
                        SValue sv;
                        sv = *op->vt;
                        sv.c.ul += 4;
                        load(TREG_EDX, &sv);
                    }
                }
            }
        }
    }
    else
    {
        // Generate save code
        for (i = 0; i < nb_outputs; i++)
        {
            op = &operands[i];
            if (op->reg >= 0)
            {
                if ((op->vt->r & VT_VALMASK) == VT_LLOCAL)
                {
                    if (!op->is_memory)
                    {
                        SValue sv;
                        sv = *op->vt;
                        sv.r = (sv.r & ~VT_VALMASK) | VT_LOCAL;
                        load(out_reg, &sv);

                        sv.r = (sv.r & ~VT_VALMASK) | out_reg;
                        store(op->reg, &sv);
                    }
                }
                else
                {
                    store(op->reg, op->vt);
                    if (op->is_llong)
                    {
                        SValue sv;
                        sv = *op->vt;
                        sv.c.ul += 4;
                        store(TREG_EDX, &sv);
                    }
                }
            }
        }

        // Generate reg restore code
        for (i = NB_SAVED_REGS - 1; i >= 0; i--)
        {
            reg = reg_saved[i];
            if (regs_allocated[reg])
                g(0x58 + reg);
        }
    }
}

void asm_clobber(uint8_t *clobber_regs, const char *str)
{
    int reg;
    TokenSym *ts;

    if (!strcmp(str, "memory") || !strcmp(str, "cc"))
        return;
    ts = tok_alloc(str, strlen(str));
    reg = ts->tok;
    if (reg >= TOK_ASM_eax && reg <= TOK_ASM_edi)
    {
        reg -= TOK_ASM_eax;
    }
    else if (reg >= TOK_ASM_ax && reg <= TOK_ASM_di)
    {
        reg -= TOK_ASM_ax;
    }
    else
    {
        error("invalid clobber register '%s'", str);
    }
    clobber_regs[reg] = 1;
}
