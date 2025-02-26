#include "compiler.h"
#include <inttypes.h>
#include <ctype.h>
#include "aslib.h"
#include "hashtbl.h"
#include "preproc.h"

const char *const pp_directives[108] = {
    "%elif",
    "%elifn",
    "%elifctx",
    "%elifnctx",
    "%elifdef",
    "%elifndef",
    "%elifempty",
    "%elifnempty",
    "%elifenv",
    "%elifnenv",
    "%elifid",
    "%elifnid",
    "%elifidn",
    "%elifnidn",
    "%elifidni",
    "%elifnidni",
    "%elifmacro",
    "%elifnmacro",
    "%elifnum",
    "%elifnnum",
    "%elifstr",
    "%elifnstr",
    "%eliftoken",
    "%elifntoken",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "%if",
    "%ifn",
    "%ifctx",
    "%ifnctx",
    "%ifdef",
    "%ifndef",
    "%ifempty",
    "%ifnempty",
    "%ifenv",
    "%ifnenv",
    "%ifid",
    "%ifnid",
    "%ifidn",
    "%ifnidn",
    "%ifidni",
    "%ifnidni",
    "%ifmacro",
    "%ifnmacro",
    "%ifnum",
    "%ifnnum",
    "%ifstr",
    "%ifnstr",
    "%iftoken",
    "%ifntoken",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "%arg",
    "%assign",
    "%clear",
    "%define",
    "%defstr",
    "%deftok",
    "%depend",
    "%else",
    "%endif",
    "%endm",
    "%endmacro",
    "%endrep",
    "%error",
    "%exitmacro",
    "%exitrep",
    "%fatal",
    "%iassign",
    "%idefine",
    "%idefstr",
    "%ideftok",
    "%imacro",
    "%include",
    "%irmacro",
    "%ixdefine",
    "%line",
    "%local",
    "%macro",
    "%pathsearch",
    "%pop",
    "%push",
    "%rep",
    "%repl",
    "%rmacro",
    "%rotate",
    "%stacksize",
    "%strcat",
    "%strlen",
    "%substr",
    "%undef",
    "%unimacro",
    "%unmacro",
    "%use",
    "%warning",
    "%xdefine",
};
const uint8_t pp_directives_len[108] = {
    5,
    6,
    8,
    9,
    8,
    9,
    10,
    11,
    8,
    9,
    7,
    8,
    8,
    9,
    9,
    10,
    10,
    11,
    8,
    9,
    8,
    9,
    10,
    11,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    3,
    4,
    6,
    7,
    6,
    7,
    8,
    9,
    6,
    7,
    5,
    6,
    6,
    7,
    7,
    8,
    8,
    9,
    6,
    7,
    6,
    7,
    8,
    9,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    4,
    7,
    6,
    7,
    7,
    7,
    7,
    5,
    6,
    5,
    9,
    7,
    6,
    10,
    8,
    6,
    8,
    8,
    8,
    8,
    7,
    8,
    8,
    9,
    5,
    6,
    6,
    11,
    4,
    5,
    4,
    5,
    7,
    7,
    10,
    7,
    7,
    7,
    6,
    9,
    8,
    4,
    8,
    8,
};

enum preproc_token pp_token_hash(const char *token)
{
#define UNUSED 16383
        static const int16_t hash1[128] = {
            UNUSED,
            UNUSED,
            0,
            0,
            0,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            0,
            0,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            -45,
            UNUSED,
            0,
            UNUSED,
            -60,
            0,
            UNUSED,
            UNUSED,
            -42,
            UNUSED,
            UNUSED,
            -49,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            48,
            UNUSED,
            UNUSED,
            49,
            5,
            UNUSED,
            -52,
            65,
            UNUSED,
            UNUSED,
            0,
            0,
            UNUSED,
            38,
            UNUSED,
            30,
            0,
            UNUSED,
            6,
            35,
            UNUSED,
            UNUSED,
            60,
            34,
            UNUSED,
            134,
            UNUSED,
            -86,
            -11,
            41,
            17,
            0,
            129,
            -84,
            UNUSED,
            UNUSED,
            82,
            0,
            UNUSED,
            16,
            97,
            -65,
            -100,
            0,
            -10,
            -76,
            UNUSED,
            UNUSED,
            UNUSED,
            1,
            UNUSED,
            0,
            12,
            UNUSED,
            -145,
            41,
            105,
            UNUSED,
            84,
            UNUSED,
            43,
            85,
            UNUSED,
            22,
            0,
            -13,
            UNUSED,
            UNUSED,
            77,
            -2,
            UNUSED,
            UNUSED,
            11,
            91,
            -6,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            106,
            44,
            UNUSED,
        };
        static const int16_t hash2[128] = {
            UNUSED,
            0,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            80,
            0,
            64,
            UNUSED,
            0,
            0,
            0,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            64,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            121,
            0,
            UNUSED,
            21,
            99,
            63,
            UNUSED,
            114,
            UNUSED,
            178,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            -39,
            UNUSED,
            88,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            87,
            UNUSED,
            42,
            UNUSED,
            UNUSED,
            141,
            UNUSED,
            UNUSED,
            UNUSED,
            102,
            UNUSED,
            46,
            105,
            149,
            UNUSED,
            23,
            53,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            0,
            UNUSED,
            UNUSED,
            UNUSED,
            33,
            0,
            0,
            92,
            UNUSED,
            50,
            72,
            UNUSED,
            7,
            42,
            65,
            UNUSED,
            UNUSED,
            112,
            52,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            UNUSED,
            98,
            100,
            71,
            UNUSED,
            19,
            63,
            32,
            UNUSED,
            UNUSED,
            UNUSED,
            96,
            17,
            84,
            132,
            UNUSED,
            37,
            UNUSED,
            9,
            20,
            UNUSED,
            UNUSED,
            75,
            97,
            UNUSED,
        };
        uint32_t k1, k2;
        uint64_t crc;
        uint16_t ix;

        crc = crc64i(UINT64_C(0xaee7ac5ccabdec91), token);
        k1 = (uint32_t)crc;
        k2 = (uint32_t)(crc >> 32);

        ix = hash1[k1 & 0x7f] + hash2[k2 & 0x7f];
        if (ix >= 108)
                return PP_INVALID;

        if (!pp_directives[ix] || as_stricmp(pp_directives[ix], token))
                return PP_INVALID;

        return ix;
}
