#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if HAVE_STRING_H
# include <string.h>
#else
# if HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif

#include "decNumber/decContext.h"
#include "decNumber/decNumber.h"
#include "decNumber/decNumberLocal.h"
#include "decNumber/decimal32.h"
#include "decNumber/decimal64.h"
#include "decNumber/decimal128.h"

#define LINE_MAX_LEN  4000

#define LINE_BUF_MAX_LEN  (LINE_MAX_LEN+3) /* +3 for CR,LF,NUL */

#define CHR_SNG_QUOTE '\''
#define CHR_DBL_QUOTE '"'
#define CHR_COLON ':'

#define STR_COMMENT "--"
#define STR_ARROW "->"
#define STR_COLON ":"

#define WHATEVER_RESULT "?"

#define DBGPRINT(str) fprintf(stderr, "%s:%d:%s", __FILE__, __LINE__, str)
#define DBGPRINTF(fmt, ...) fprintf(stderr, "%s:%d:" fmt, __FILE__, __LINE__, __VA_ARGS__)

typedef enum {
    FALSE,
    TRUE
} bool;

typedef enum {
    FAILURE,
    SUCCESS
} s_or_f;

typedef struct _testfile_t {
    char *filename;
    FILE *fp;
    decContext context;
#if !DECSUBSET
    uint8_t  extended;
#endif
    int test_count;
    int success_count;
    int failure_count;
    int skip_count;
} testfile_t;

#define testfile_context(testfile_ptr) (testfile_ptr->context)

typedef struct _tokens_t {
    int count;
    char **tokens;
} tokens_t;

typedef struct _testcase_t {
    char *id;
    char *operator;
    bool is_using_directive_precision;
    int operand_count;
    char **operands;
    decNumber **operand_numbers;
    decContext *operand_contexts;
    uint32_t expected_status;
    char *expected_string;
    decNumber *expected_number;
    decContext expected_context;
    decContext *context;
    uint32_t actual_status;
    char *actual_string;
    decNumber *actual_number;
} testcase_t;

static s_or_f process_file(char *filename, testfile_t *parent);
static void status_print(uint32_t status);

/*
 * named testcases to skip (>0.5 ulp or flags cases)
 * that Mr. Mike Cowlishaw is aware of.
 */
static char *skip_list[] = {
    "pwsx805", "powx4302", "powx4303", "powx4342", "powx4343", "lnx116",
    "lnx732", NULL
};

static bool is_in_skip_list(const char *id)
{
    char **p;

    for (p = skip_list; *p; ++p) {
        if (strcmp(id, *p) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void context_print(const decContext *context)
{
    printf("context prec=%d, round=%d, emax=%d, emin=%d, status=[",
        context->digits, context->round, context->emax, context->emin);
    status_print(context->status);
    printf("], traps=[");
    status_print(context->traps);
#if DECSUBSET
    printf("], clamp=%d, extended=%d\n", context->clamp, context->extended);
#else
    printf("], clamp=%d\n", context->clamp);
#endif
}

static char *convert_number_to_string(const decNumber *dn)
{
    char *s;
    int buf_len;

    buf_len = dn->digits + 14 + 1;
    s = (char *)malloc(sizeof(char) * buf_len);
    if (!s) {
        return NULL;
    }
    decNumberToString(dn, s);
    return s;
}

static char *convert_number_to_eng_string(const decNumber *dn)
{
    char *s;
    int buf_len;

    buf_len = dn->digits + 14 + 1;
    s = (char *)malloc(sizeof(char) * buf_len);
    if (!s) {
        return NULL;
    }
    decNumberToEngString(dn, s);
    return s;
}

static void tokens_init(tokens_t *tokens)
{
    tokens->count = 0;
    tokens->tokens = NULL;
}

static char *my_strndup(const char *s, size_t n)
{
    char *dup;

    dup = (char *)malloc(sizeof(char) * (n + 1));
    if (dup) {
        strncpy(dup, s, n);
        dup[n] = '\0';
    }
    return dup;
}

static int count_char(const char *s, size_t n, char ch)
{
    int i;
    int count;

    count = 0;
    for (i = 0; i < n; ++i) {
        if (s[i] == ch) {
            ++count;
        }
    }
    return count;
}

static char *unquote_token_helper(const char *s, size_t n, char quote)
{
    char *dup;
    int quote_count;
    int i;
    int j;
    int len;

    quote_count = count_char(s + 1, n - 2, quote);
    len = n - 2 - quote_count / 2;
    dup = (char *)calloc(len + 1, sizeof(char));
    i = 0;
    for (j = 1; j < n - 1; ++j) {
        dup[i++] = s[j];
        if (s[j] == quote && s[j + 1] == quote) {
            ++j;
        }
    }
    dup[len] = '\0';
    return dup;
}

static char *unquote_token(const char *s, size_t n)
{
    return s[0] == CHR_SNG_QUOTE || s[0] == CHR_DBL_QUOTE
        ? unquote_token_helper(s, n, s[0]) : my_strndup(s, n);
}

static s_or_f tokens_add_token(tokens_t *tokens, const char *text,
    size_t length)
{
    int count;

    count = tokens->count + 1;
    tokens->tokens = realloc(tokens->tokens, sizeof(char *) * count);
    if (!tokens->tokens) {
        DBGPRINT("realloc failed\n");
        return FAILURE;
    }
    tokens->tokens[tokens->count] = unquote_token(text, length);
    if (!tokens->tokens[tokens->count]) {
        DBGPRINT("unquote_token failed\n");
        return FAILURE;
    }
    tokens->count = count;
    return SUCCESS;
}

static void tokens_dtor(tokens_t *tokens)
{
    int i;

    for (i = 0; i < tokens->count; ++i) {
        free(tokens->tokens[i]);
    }
    free(tokens->tokens);
}

static void tokens_print(tokens_t *tokens)
{
    int i;

    for (i = 0; i < tokens->count; ++i) {
        if (i > 0) {
            printf(" ");
        }
        printf("%s", tokens->tokens[i]);
    }
    printf("\n");
}

static bool get_next_token_pos(char *line, int offset, int *start, int *end)
{
    int i;
    int len;

    len = strlen(line);

    // skip leading whitespaces.
    for (i = offset; i < len; ++i) {
        if (!isspace(line[i])) {
            break;
        }
    }

    if (i >= len) {
        return FALSE;
    }

    *start = i;
    switch (line[i]) {
    case CHR_SNG_QUOTE:
        for (++i; i < len; ++i) {
            if (line[i] == CHR_SNG_QUOTE
                && !(i + 1 < len && line[i + 1] == CHR_SNG_QUOTE)
            ) {
                ++i;
                break;
            }
        }
        break;
    case CHR_DBL_QUOTE:
        for (++i; i < len; ++i) {
            if (line[i] == CHR_DBL_QUOTE
                && !(i + 1 < len && line[i + 1] == CHR_DBL_QUOTE)
            ) {
                ++i;
                break;
            }
        }
        break;
    default:
        for (++i; i < len; ++i) {
            if (isspace(line[i]) || line[i] == CHR_COLON) {
                break;
            }
        }
        break;
    }
    *end = i;
    return TRUE;
}

static s_or_f tokens_tokenize(tokens_t *tokens, char *line)
{
    int offset;
    int start;
    int end;

    tokens_init(tokens);
    offset = 0;
    for (;;) {
        if (!get_next_token_pos(line, offset, &start, &end)) {
            break;
        }

        // ignore comment
        if (strncmp(line + start, STR_COMMENT, sizeof(STR_COMMENT) - 1) == 0) {
            break;
        }

        if (!tokens_add_token(tokens, line + start, end - start)) {
            DBGPRINT("tokens_add_token failed\n");
            return FAILURE;
        }
        offset = end;
    }

    return SUCCESS;
}

static bool tokens_has_token(tokens_t *tokens, char *str)
{
    int i;

    for (i = 0; i < tokens->count; ++i) {
        if (strcmp(tokens->tokens[i], str) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static inline bool tokens_is_directive(tokens_t *tokens)
{
    return tokens->count == 3 && strcmp(tokens->tokens[1], STR_COLON) == 0;
}

static inline bool tokens_is_empty(tokens_t *tokens)
{
    return tokens->count == 0;
}

static s_or_f testfile_init(testfile_t *testfile, const char* filename)
{
    testfile->filename = strdup(filename);
    testfile->fp = fopen(filename, "r");

    decContextDefault(&testfile->context, DEC_INIT_BASE);
    testfile->context.traps = 0;
#if DECSUBSET
    testfile->context.extended = 0;
#else
    testfile->extended = 0;
#endif

    testfile->test_count = 0;
    testfile->success_count = 0;
    testfile->failure_count = 0;
    testfile->skip_count = 0;

    return testfile->fp != NULL;
}

static void testfile_dtor(testfile_t *testfile)
{
    fclose(testfile->fp);
    free(testfile->filename);
}


static int count_coefficient_digit(const char *s)
{
    const char *p;
    int count;

    count = 0;
    p = s;
    if (*p == '-' || *p == '+') {
        ++p;
    }
    if (strncasecmp(p, "nan", sizeof("nan") - 1) == 0) {
        p += sizeof("nan") - 1;
    } else if (strncasecmp(p, "snan", sizeof("snan") - 1) == 0) {
        p += sizeof("snan") - 1;
    }
    for (; *p; ++p) {
        if (isdigit(*p)) {
            ++count;
        } else if (*p == '.') {
            continue;
        } else if (*p == 'e' || *p == 'E') {
            break;
        }
    }
    return count;
}

static int tokens_count_operands(tokens_t *tokens)
{
    int i;
    int count;

    count = 0;
    for (i = 2; i < tokens->count; ++i) {
        if (strcmp(tokens->tokens[i], STR_ARROW) == 0) {
            break;
        }
        ++count;
    }
    return count;
}

typedef struct _status_map_t {
    char *name;
    uint32_t value;
} status_map_t;

static status_map_t status_maps[] = {
    { "Conversion_syntax",    DEC_Conversion_syntax },
    { "Division_by_zero",     DEC_Division_by_zero },
    { "Division_impossible",  DEC_Division_impossible },
    { "Division_undefined",   DEC_Division_undefined },
    { "Insufficient_storage", DEC_Insufficient_storage },
    { "Inexact",              DEC_Inexact },
    { "Invalid_context",      DEC_Invalid_context },
    { "Invalid_operation",    DEC_Invalid_operation },
#if DECSUBSET
    { "Lost_digits",          DEC_Lost_digits },
#endif
    { "Overflow",             DEC_Overflow },
    { "Clamped",              DEC_Clamped },
    { "Rounded",              DEC_Rounded },
    { "Subnormal",            DEC_Subnormal },
    { "Underflow",            DEC_Underflow },
    { NULL, -1 }
};

static s_or_f convert_status_name_to_value(const char *name, uint32_t *value)
{
    status_map_t *entry;
    for (entry = status_maps; entry->name; ++entry) {
        if (strcasecmp(entry->name, name) == 0) {
            *value = entry->value;
            return SUCCESS;
        }
    }
    DBGPRINTF("error in convert_status_name_to_value. name not found: %s\n", name);
    return FAILURE;
}

static void status_print(uint32_t status)
{
    status_map_t *entry;
    int i;

    i = 0;
    for (entry = status_maps; entry->name; ++entry) {
        if (status & entry->value) {
            if (i > 0) {
                printf(" ");
            }
            printf("%s", entry->name);
            ++i;
        }
    }
}

static s_or_f tokens_get_conditions(tokens_t *tokens, int offset,
    uint32_t *status)
{
    int i;
    uint32_t flag;

    *status = 0;
    for (i = offset; i < tokens->count; ++i) {
        if (!convert_status_name_to_value(tokens->tokens[i], &flag)) {
            DBGPRINT("convert_status_name_to_value failed\n");
            return FAILURE;
        }

        *status |= flag;
    }
    return SUCCESS;
}

static decNumber *alloc_number(int32_t numdigits)
{
    uInt needbytes;
    decNumber *number;

    needbytes = sizeof(decNumber) + (D2U(numdigits) - 1) * sizeof(Unit);
    number = (decNumber *)malloc(needbytes);
    if (!number) {
        DBGPRINT("no more memory in alloc_number.\n");
        return NULL;
    }
    memset(number, 0, needbytes);
    return number;
}

static s_or_f convert_hex_char_to_int(char ch, uint8_t *hex)
{
    switch (ch) {
    case '0':
        *hex = 0;
        break;
    case '1':
        *hex = 1;
        break;
    case '2':
        *hex = 2;
        break;
    case '3':
        *hex = 3;
        break;
    case '4':
        *hex = 4;
        break;
    case '5':
        *hex = 5;
        break;
    case '6':
        *hex = 6;
        break;
    case '7':
        *hex = 7;
        break;
    case '8':
        *hex = 8;
        break;
    case '9':
        *hex = 9;
        break;
    case 'A':
    case 'a':
        *hex = 10;
        break;
    case 'B':
    case 'b':
        *hex = 11;
        break;
    case 'C':
    case 'c':
        *hex = 12;
        break;
    case 'D':
    case 'd':
        *hex = 13;
        break;
    case 'E':
    case 'e':
        *hex = 14;
        break;
    case 'F':
    case 'f':
        *hex = 15;
        break;
    default:
        return FAILURE;
    }
    return SUCCESS;
}

static s_or_f parse_hex(int bytes, uint8_t buf[], const char *s)
{
    int i;
    int j;
    uint8_t hi;
    uint8_t lo;

    if (strlen(s) != bytes * 2) {
        DBGPRINTF("error in parse_hex expected length is %d, but was %d [%s]\n", bytes * 2, strlen(s), s);
        return FAILURE;
    }

    j = 0;
    for (i = bytes - 1; i >= 0; --i, j += 2) {
        if (!convert_hex_char_to_int(s[j], &hi)) {
            return FAILURE;
        }
        if (!convert_hex_char_to_int(s[j + 1], &lo)) {
            return FAILURE;
        }
        buf[i] = hi << 4 | lo;
    }
    return SUCCESS;
}

static s_or_f parse_decimal32_hex(const char *s, decNumber **number,
    decContext *ctx)
{
    decimal32 dec32;

    if (!parse_hex(DECIMAL32_Bytes, dec32.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL32_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal32ToNumber(&dec32, *number);
    decimal32FromNumber(&dec32, *number, ctx);
    decimal32ToNumber(&dec32, *number);
    return SUCCESS;
}

static s_or_f parse_decimal64_hex(const char *s, decNumber **number,
    decContext *ctx)
{
    decimal64 dec64;

    if (!parse_hex(DECIMAL64_Bytes, dec64.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL64_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal64ToNumber(&dec64, *number);
    decimal64FromNumber(&dec64, *number, ctx);
    decimal64ToNumber(&dec64, *number);
    return SUCCESS;
}

static s_or_f parse_decimal128_hex(const char *s, decNumber **number,
    decContext *ctx)
{
    decimal128 dec128;

    if (!parse_hex(DECIMAL128_Bytes, dec128.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL128_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal128ToNumber(&dec128, *number);
    decimal128FromNumber(&dec128, *number, ctx);
    decimal128ToNumber(&dec128, *number);
    return SUCCESS;
}

static s_or_f parse_hex_notation(const char *s, decNumber **number,
    decContext *ctx)
{
    int len;

    len = strlen(s) - 1;
    if (len == 0) {
        *number = NULL;
    } else if (len == 8) {
        if (!parse_decimal32_hex(s + 1, number, ctx)) {
            DBGPRINTF("parse_decimal32_hex failed [%s]\n", s);
            return FAILURE;
        }
    } else if (len == 16) {
        if (!parse_decimal64_hex(s + 1, number, ctx)) {
            DBGPRINTF("parse_decimal32_hex failed [%s]\n", s);
            return FAILURE;
        }
    } else if (len == 32) {
        if (!parse_decimal128_hex(s + 1, number, ctx)) {
            DBGPRINTF("parse_decimal32_hex failed [%s]\n", s);
            return FAILURE;
        }
    } else {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }
    return SUCCESS;
}

static s_or_f parse_decimal32_hex_canonical(const char *s, decNumber **number)
{
    decimal32 dec32;
    decimal32 dec32canonical;

    if (!parse_hex(DECIMAL32_Bytes, dec32.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }
    decimal32Canonical(&dec32canonical, &dec32);

    *number = alloc_number(DECIMAL32_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal32ToNumber(&dec32canonical, *number);
    return SUCCESS;
}

static s_or_f parse_decimal64_hex_canonical(const char *s, decNumber **number)
{
    decimal64 dec64;
    decimal64 dec64canonical;

    if (!parse_hex(DECIMAL64_Bytes, dec64.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }
    decimal64Canonical(&dec64canonical, &dec64);

    *number = alloc_number(DECIMAL64_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal64ToNumber(&dec64canonical, *number);
    return SUCCESS;
}

static s_or_f parse_decimal128_hex_canonical(const char *s, decNumber **number)
{
    decimal128 dec128;
    decimal128 dec128canonical;

    if (!parse_hex(DECIMAL128_Bytes, dec128.bytes, s)) {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }
    decimal128Canonical(&dec128canonical, &dec128);

    *number = alloc_number(DECIMAL128_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal128ToNumber(&dec128canonical, *number);
    return SUCCESS;
}

static s_or_f parse_hex_notation_canonical(const char *s, decNumber **number)
{
    int len;

    len = strlen(s) - 1;
    if (len == 0) {
        *number = NULL;
    } else if (len == 8) {
        if (!parse_decimal32_hex_canonical(s + 1, number)) {
            DBGPRINTF("parse_decimal32_hex_canonical failed [%s]\n", s);
            return FAILURE;
        }
    } else if (len == 16) {
        if (!parse_decimal64_hex_canonical(s + 1, number)) {
            DBGPRINTF("parse_decimal32_hex_canonical failed [%s]\n", s);
            return FAILURE;
        }
    } else if (len == 32) {
        if (!parse_decimal128_hex_canonical(s + 1, number)) {
            DBGPRINTF("parse_decimal32_hex_canonical failed [%s]\n", s);
            return FAILURE;
        }
    } else {
        DBGPRINTF("invalid hex notation [%s]\n", s);
        return FAILURE;
    }
    return SUCCESS;
}

static s_or_f parse_format_dependent_decimal32(const char *s,
    decNumber **number, decContext *ctx)
{
    int32_t digits;
    decimal32 dec32;
    decNumber *tmp;

    tmp = alloc_number(ctx->digits);
    if (!tmp) {
        return FAILURE;
    }
    decNumberFromString(tmp, s, ctx);

    decimal32FromNumber(&dec32, tmp, ctx);
    free(tmp);

    *number = alloc_number(DECIMAL32_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal32ToNumber(&dec32, *number);

    return SUCCESS;
}

static s_or_f parse_format_dependent_decimal64(const char *s,
    decNumber **number, decContext *ctx)
{
    int32_t digits;
    decimal64 dec64;
    decNumber *tmp;

    tmp = alloc_number(ctx->digits);
    if (!tmp) {
        return FAILURE;
    }
    decNumberFromString(tmp, s, ctx);

    decimal64FromNumber(&dec64, tmp, ctx);
    free(tmp);

    *number = alloc_number(DECIMAL64_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal64ToNumber(&dec64, *number);

    return SUCCESS;
}

static s_or_f parse_format_dependent_decimal128(const char *s,
    decNumber **number, decContext *ctx)
{
    int32_t digits;
    decimal128 dec128;
    decNumber *tmp;

    tmp = alloc_number(ctx->digits);
    if (!tmp) {
        return FAILURE;
    }
    decNumberFromString(tmp, s, ctx);

    decimal128FromNumber(&dec128, tmp, ctx);
    free(tmp);

    *number = alloc_number(DECIMAL128_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal128ToNumber(&dec128, *number);

    return SUCCESS;
}

static s_or_f parse_format_dependent_decimal(const char *s, decNumber **number,
    decContext *ctx)
{
    if (strncmp(s, "32#", sizeof("32#") - 1) == 0) {
        if (!parse_format_dependent_decimal32(s + sizeof("32#") - 1, number,
            ctx)
        ) {
            DBGPRINTF("parse_format_dependent_decimal32 failed [%s]\n", s);
            return FAILURE;
        }
    } else if (strncmp(s, "64#", sizeof("64#") - 1) == 0) {
        if (!parse_format_dependent_decimal64(s + sizeof("64#") - 1, number,
            ctx)
        ) {
            DBGPRINTF("parse_format_dependent_decimal64 failed [%s]\n", s);
            return FAILURE;
        }
    } else if (strncmp(s, "128#", sizeof("128#") - 1) == 0) {
        if (!parse_format_dependent_decimal128(s + sizeof("128#") - 1, number,
            ctx)
        ) {
            DBGPRINTF("parse_format_dependent_decimal128 failed [%s]\n", s);
            return FAILURE;
        }
    } else {
        DBGPRINTF("invalid format dependent decimal notation [%s]\n", s);
        return FAILURE;
    }
    return SUCCESS;
}

static void print_operand(testcase_t *testcase, int arg_pos)
{
    decNumber *n;
    char *s;
    int unit_count;
    int j;

    n = testcase->operand_numbers[arg_pos];
    s = convert_number_to_string(n);
    printf("%s [%d] %s -> %s digits=%d, exp=%d, bits=0x%x", testcase->id,
        arg_pos, testcase->operands[arg_pos], s, n->digits, n->exponent,
        n->bits);
    free(s);

    unit_count = D2U(n->digits);
    printf(", lsu=");
    for (j = 0; j < unit_count; ++j) {
        if (j > 0) {
            printf(" ");
        }
        printf("%x", n->lsu[j]);
    }
    printf(", is_using_directive_precision=%d",
        testcase->is_using_directive_precision);

    if (strcmp(testcase->operator, "canonical") == 0) {
        printf("\n");
    } else {
        printf(", ");
        context_print(&testcase->operand_contexts[arg_pos]);
    }
}

static void print_expected(testcase_t *testcase)
{
    decNumber *n;
    char *s;
    int unit_count;
    int j;

    n = testcase->expected_number;
    if (n == NULL) {
        return;
    }

    s = convert_number_to_string(n);
    printf("%s [expected] %s -> %s digits=%d, exp=%d, bits=0x%x", testcase->id,
        testcase->expected_string, s, n->digits, n->exponent, n->bits);
    free(s);

    unit_count = D2U(n->digits);
    printf(", lsu=");
    for (j = 0; j < unit_count; ++j) {
        if (j > 0) {
            printf(" ");
        }
        printf("%x", n->lsu[j]);
    }

    printf(", ");
    context_print(&testcase->expected_context);
}

static s_or_f testcase_convert_operand_to_number(testcase_t *testcase,
    int arg_pos)
{
    decContext *ctx;
    char *s;
    int32_t digits;
    char *p_sharp;

    testcase->operand_contexts[arg_pos] = *testcase->context;
    ctx = &testcase->operand_contexts[arg_pos];
    s = testcase->operands[arg_pos];

    if (!testcase->is_using_directive_precision) {
        digits = count_coefficient_digit(s);
        ctx->digits = digits;
        ctx->emax = INT32_MAX - digits;
        ctx->emin = INT32_MIN + digits;
        ctx->clamp = 0;
    }

    p_sharp = strchr(s, '#');
    if (p_sharp != NULL) {
        if (p_sharp == s) {
            if (strcmp(testcase->operator, "canonical") == 0) {
                if (!parse_hex_notation_canonical(s,
                    &testcase->operand_numbers[arg_pos])
                ) {
                    DBGPRINTF("parse_decimal64_hex_canonical failed for operand %d. [%s]\n", arg_pos, s);
                    return FAILURE;
                }
            } else {
                if (!parse_hex_notation(s, &testcase->operand_numbers[arg_pos],
                    ctx)
                ) {
                    DBGPRINTF("parse_hex_notation failed for operand %d. [%s]\n", arg_pos, s);
                    return FAILURE;
                }
            }
        } else {
            if (!parse_format_dependent_decimal(s,
                &testcase->operand_numbers[arg_pos], ctx)
            ) {
                DBGPRINTF("parse_format_dependent_decimal failed for operand %d. [%s]\n", arg_pos, s);
                return FAILURE;
            }
        }
    } else {
        testcase->operand_numbers[arg_pos] = alloc_number(ctx->digits);
        if (!testcase->operand_numbers[arg_pos]) {
            return FAILURE;
        }
        decNumberFromString(testcase->operand_numbers[arg_pos], s, ctx);
    }

    if (testcase->is_using_directive_precision) {
        testcase->context->status |= ctx->status;
    }

    return SUCCESS;
}

static s_or_f testcase_convert_operands_to_numbers(testcase_t *testcase)
{
    char *op;
    int i;

    op = testcase->operator;

    testcase->operand_numbers = (decNumber **)calloc(testcase->operand_count,
        sizeof(decNumber *));
    if (!testcase->operand_numbers) {
        DBGPRINT("out of memory in testcase_convert_operands_to_numbers\n");
        return FAILURE;
    }
    testcase->operand_contexts = (decContext *)calloc(testcase->operand_count,
        sizeof(decContext));
    if (!testcase->operand_contexts) {
        DBGPRINT("out of memory in testcase_convert_operands_to_numbers\n");
        return FAILURE;
    }

    for (i = 0; i < testcase->operand_count; ++i) {
        if (!testcase_convert_operand_to_number(testcase, i)) {
            return FAILURE;
        }
    }

    return SUCCESS;
}

static s_or_f testcase_convert_result_to_number(testcase_t *testcase)
{
    char *s;
    char *p_sharp;
    int32_t digits;
    decContext *ctx;

    s = testcase->expected_string;
    testcase->expected_context = *testcase->context;
    ctx = &testcase->expected_context;
    digits = count_coefficient_digit(s);
    ctx->digits = digits;
    ctx->emax = INT32_MAX - digits;
    ctx->emin = INT32_MIN + digits;
    ctx->clamp = 0;

    p_sharp = strchr(s, '#');
    if (p_sharp != NULL) {
        // The clamp=1 is only implied when the result is a format-dependent
        // representation (with a # in it).
        ctx->clamp = 1;

        if (p_sharp == s) {
            if (!parse_hex_notation(s, &testcase->expected_number, ctx)) {
                DBGPRINTF("parse_hex_notation failed for result. [%s]\n", s);
                return FAILURE;
            }
        } else {
            if (!parse_format_dependent_decimal(s, &testcase->expected_number,
                ctx)
            ) {
                DBGPRINTF("parse_format_dependent_decimal failed for result. [%s]\n", s);
                return FAILURE;
            }
        }

        if (ctx->status != 0) {
            testcase->context->status |= ctx->status;
        }
    } else {
        testcase->expected_number = alloc_number(ctx->digits);
        if (!testcase->expected_number) {
            return FAILURE;
        }
        decNumberFromString(testcase->expected_number, s, ctx);
    }


    return SUCCESS;
}

static s_or_f testcase_init(testcase_t *testcase, testfile_t *testfile,
    tokens_t *tokens)
{
    int i;
    char *op;

    testcase->id = tokens->tokens[0];
    op = testcase->operator = tokens->tokens[1];
    testcase->is_using_directive_precision = (strcasecmp(op, "apply") == 0
        || strcasecmp(op, "tosci") == 0 || strcasecmp(op, "toeng") == 0);
    testcase->operand_count = tokens_count_operands(tokens);
    testcase->context = &testfile->context;
    testcase->context->traps = 0;
    testcase->context->status = 0;
    testcase->actual_status = 0;
    testcase->actual_string = NULL;
    testcase->actual_number = NULL;
    if (!tokens_get_conditions(tokens, 2 + testcase->operand_count + 2,
        &testcase->expected_status)
    ) {
        DBGPRINT("tokens_get_conditions failed.\n");
        return FAILURE;
    }

    testcase->operand_numbers = NULL;
    testcase->operand_contexts = NULL;
    testcase->operands = (char **)calloc(testcase->operand_count,
        sizeof(char *));
    if (!testcase->operands) {
        DBGPRINT("out of memory in testcase_init\n");
        return FAILURE;
    }
    for (i = 0; i < testcase->operand_count; ++i) {
        testcase->operands[i] = tokens->tokens[i + 2];
    }

    testcase->expected_string = tokens->tokens[2 + testcase->operand_count + 1];
    testcase->expected_number = NULL;
    if (strcasecmp(testcase->operator, "class") != 0
        && strcasecmp(testcase->operator, "tosci") != 0
        && strcasecmp(testcase->operator, "toeng") != 0
    ) {
        if (!testcase_convert_result_to_number(testcase)) {
            return FAILURE;
        }
    }

    return SUCCESS;
}

static bool testcase_has_null_operand(testcase_t *testcase)
{
    int i;

    for (i = 0; i < testcase->operand_count; ++i) {
        if (strcmp(testcase->operands[i], "#") == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static s_or_f testcase_run(testcase_t *testcase)
{
    decNumber **operands;
    decNumber *result;

    if (strlen(testcase->operator) == 0) {
        DBGPRINT("error in testcase_run. operator is empty.\n");
        return FAILURE;
    }

    if (!testcase_convert_operands_to_numbers(testcase)) {
        return FAILURE;
    }
    operands = testcase->operand_numbers;

    testcase->actual_number = alloc_number(testcase->context->digits);
    if (!testcase->actual_number) {
        return FAILURE;
    }
    result = testcase->actual_number;

    switch (tolower(testcase->operator[0])) {
    case 'a':
        if (strcasecmp(testcase->operator, "abs") == 0) {
            decNumberAbs(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "add") == 0) {
            decNumberAdd(result, operands[0], operands[1], testcase->context);
        } else if (strcasecmp(testcase->operator, "and") == 0) {
            decNumberAnd(result, operands[0], operands[1], testcase->context);
        } else if (strcasecmp(testcase->operator, "apply") == 0) {
            decNumberCopy(result, operands[0]);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'c':
        if (strcasecmp(testcase->operator, "canonical") == 0) {
            decNumberCopy(result, operands[0]);
        } else if (strcasecmp(testcase->operator, "class") == 0) {
            enum decClass num_class;
            num_class = decNumberClass(operands[0], testcase->context);
            testcase->actual_string = strdup(decNumberClassToString(num_class));
        } else if (strcasecmp(testcase->operator, "compare") == 0) {
            decNumberCompare(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "comparesig") == 0) {
            decNumberCompareSignal(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "comparetotmag") == 0) {
            decNumberCompareTotalMag(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "comparetotal") == 0) {
            decNumberCompareTotal(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "copy") == 0) {
            decNumberCopy(result, operands[0]);
        } else if (strcasecmp(testcase->operator, "copyabs") == 0) {
            decNumberCopyAbs(result, operands[0]);
        } else if (strcasecmp(testcase->operator, "copynegate") == 0) {
            decNumberCopyNegate(result, operands[0]);
        } else if (strcasecmp(testcase->operator, "copysign") == 0) {
            decNumberCopySign(result, operands[0], operands[1]);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'd':
        if (strcasecmp(testcase->operator, "divide") == 0) {
            decNumberDivide(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "divideint") == 0) {
            decNumberDivideInteger(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'e':
        if (strcasecmp(testcase->operator, "exp") == 0) {
            decNumberExp(result, operands[0], testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'f':
        if (strcasecmp(testcase->operator, "fma") == 0) {
            decNumberFMA(result, operands[0], operands[1], operands[2],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'i':
        if (strcasecmp(testcase->operator, "invert") == 0) {
            decNumberInvert(result, operands[0], testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'l':
        if (strcasecmp(testcase->operator, "ln") == 0) {
            decNumberLn(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "log10") == 0) {
            decNumberLog10(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "logb") == 0) {
            decNumberLogB(result, operands[0], testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'm':
        if (strcasecmp(testcase->operator, "max") == 0) {
            decNumberMax(result, operands[0], operands[1], testcase->context);
        } else if (strcasecmp(testcase->operator, "maxmag") == 0) {
            decNumberMaxMag(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "min") == 0) {
            decNumberMin(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "minmag") == 0) {
            decNumberMinMag(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "minus") == 0) {
            decNumberMinus(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "multiply") == 0) {
            decNumberMultiply(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'n':
        if (strcasecmp(testcase->operator, "nextminus") == 0) {
            decNumberNextMinus(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "nextplus") == 0) {
            decNumberNextPlus(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "nexttoward") == 0) {
            decNumberNextToward(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'o':
        if (strcasecmp(testcase->operator, "or") == 0) {
            decNumberOr(result, operands[0], operands[1], testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'p':
        if (strcasecmp(testcase->operator, "plus") == 0) {
            decNumberPlus(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "power") == 0) {
            decNumberPower(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'q':
        if (strcasecmp(testcase->operator, "quantize") == 0) {
            decNumberQuantize(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'r':
        if (strcasecmp(testcase->operator, "reduce") == 0) {
            decNumberReduce(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "remainder") == 0) {
            decNumberRemainder(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "remaindernear") == 0) {
            decNumberRemainderNear(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "rescale") == 0) {
            decNumberRescale(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "rotate") == 0) {
            decNumberRotate(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 's':
        if (strcasecmp(testcase->operator, "samequantum") == 0) {
            decNumberSameQuantum(result, operands[0], operands[1]);
        } else if (strcasecmp(testcase->operator, "scaleb") == 0) {
            decNumberScaleB(result, operands[0], operands[1],
                testcase->context);
        } else if (strcasecmp(testcase->operator, "shift") == 0) {
            decNumberShift(result, operands[0], operands[1], testcase->context);
        } else if (strcasecmp(testcase->operator, "squareroot") == 0) {
            decNumberSquareRoot(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "subtract") == 0) {
            decNumberSubtract(result, operands[0], operands[1],
                testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 't':
        if (strcasecmp(testcase->operator, "toeng") == 0) {
            testcase->actual_string = convert_number_to_eng_string(operands[0]);
        } else if (strcasecmp(testcase->operator, "tointegral") == 0) {
            decNumberToIntegralValue(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "tointegralx") == 0) {
            decNumberToIntegralExact(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "tosci") == 0) {
            testcase->actual_string = convert_number_to_string(operands[0]);
        } else if (strcasecmp(testcase->operator, "trim") == 0) {
            if (testcase->actual_number->digits < operands[0]->digits) {
                free(testcase->actual_number);
                testcase->actual_number = alloc_number(operands[0]->digits);
                if (!testcase->actual_number) {
                    return FAILURE;
                }
                result = testcase->actual_number;
            }
            decNumberCopy(result, operands[0]);
            decNumberTrim(result);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    case 'x':
        if (strcasecmp(testcase->operator, "xor") == 0) {
            decNumberXor(result, operands[0], operands[1], testcase->context);
        } else {
            DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
                testcase->operator);
            return FAILURE;
        }
        break;
    default:
        DBGPRINTF("error in testcase_run. unknown operator: %s.\n",
            testcase->operator);
        return FAILURE;
    }

    testcase->actual_status = testcase->context->status;

    return SUCCESS;
}

static void testcase_print(testcase_t *testcase)
{
    int i;
    int j;
    char *s;
    decNumber *n;
    int unit_count;

    printf("id=%s  %s ", testcase->id, testcase->operator);
    for (i = 0; i < testcase->operand_count; ++i) {
        if (i > 0) {
            printf(" ");
        }
        printf("%s", testcase->operands[i]);
    }
    printf(" -> ");
    printf("%s", testcase->expected_string);
    printf(" expected_status=[");
    status_print(testcase->expected_status);
    printf("]\n");

    if (testcase->operand_numbers) {
        printf("operand_numbers:\n");
        for (i = 0; i < testcase->operand_count; ++i) {
            print_operand(testcase, i);
        }
    }
    print_expected(testcase);

    fflush(stdout);
}

static bool testcase_check(testcase_t *testcase)
{
    bool value_matched;
    bool status_matched;
    char *actual_string;
    char *expected_string;
    decNumber compare_result;

    if (strcmp(testcase->expected_string, WHATEVER_RESULT) == 0) {
        value_matched = TRUE;
    } else if (testcase->actual_string != NULL) {
        value_matched = (strcmp(testcase->actual_string,
            testcase->expected_string) == 0);
    } else {
        decNumberCompareTotal(&compare_result, testcase->actual_number,
            testcase->expected_number, testcase->context);
        value_matched = decNumberIsZero(&compare_result);
    }

    status_matched = (testcase->actual_status == testcase->expected_status);

    if (value_matched && status_matched) {
        return TRUE;
    }

    testcase_print(testcase);

    if (testcase->actual_string != NULL) {
        actual_string = testcase->actual_string;
        expected_string = testcase->expected_string;
    } else {
        actual_string = convert_number_to_string(testcase->actual_number);
        expected_string = convert_number_to_string(testcase->expected_number);
    }
    printf("value %s\n", (value_matched ? "matched" : "unmatched"));
    printf("   actual_value=[%s]\n", actual_string);
    printf(" expected_value=[%s]\n", expected_string);
    if (testcase->actual_string == NULL) {
        free(actual_string);
        free(expected_string);
    }

    printf("status %s\n", (status_matched ? "matched" : "unmatched"));
    printf("    actual_status=[");
    status_print(testcase->actual_status);
    printf("]\n");
    printf("  expected_status=[");
    status_print(testcase->expected_status);
    printf("]\n");
    context_print(testcase->context);

    return FALSE;
}

static int testcase_dtor(testcase_t *testcase)
{
    int i;

    if (testcase->operands) {
        free(testcase->operands);
    }
    if (testcase->operand_numbers) {
        for (i = 0; i < testcase->operand_count; ++i) {
            if (testcase->operand_numbers[i]) {
                free(testcase->operand_numbers[i]);
            }
        }
        free(testcase->operand_numbers);
    }
    if (testcase->operand_contexts) {
        free(testcase->operand_contexts);
    }

    if (testcase->actual_string) {
        free(testcase->actual_string);
    }
    if (testcase->actual_number) {
        free(testcase->actual_number);
    }

    if (testcase->expected_number) {
        free(testcase->expected_number);
    }
}

static s_or_f testfile_process_test(testfile_t *testfile, tokens_t *tokens)
{
    testcase_t testcase;

    ++testfile->test_count;
#if !DECSUBSET
    if (testfile->extended) {
        ++testfile->skip_count;
        return SUCCESS;
    }
#endif

    if (!testcase_init(&testcase, testfile, tokens)) {
        DBGPRINT("testcase_init failed.\n");
        return FAILURE;
    }

    if (testcase_has_null_operand(&testcase) || is_in_skip_list(testcase.id)) {
        ++testfile->skip_count;
    } else {
        if (!testcase_run(&testcase)) {
            tokens_print(tokens);
            DBGPRINT("testcase_run failed.\n");
            return FAILURE;
        }
        if (testcase_check(&testcase)) {
            ++testfile->success_count;
        } else {
            ++testfile->failure_count;
        }
    }
    testcase_dtor(&testcase);
    return SUCCESS;
}

static s_or_f handle_version(testfile_t *testfile, tokens_t *tokens)
{
    /* no-op */
    return SUCCESS;
}

static s_or_f handle_precision(testfile_t *testfile, tokens_t *tokens)
{
    int precision;

    precision = atoi(tokens->tokens[2]);
    testfile_context(testfile).digits = precision;
    return SUCCESS;
}

typedef struct _rounding_map_t {
    char *name;
    int value;
} rounding_map_t;

static rounding_map_t rounding_maps[] = {
    { "ceiling",   DEC_ROUND_CEILING },
    { "up",        DEC_ROUND_UP },
    { "half_up",   DEC_ROUND_HALF_UP },
    { "half_even", DEC_ROUND_HALF_EVEN },
    { "half_down", DEC_ROUND_HALF_DOWN },
    { "down",      DEC_ROUND_DOWN },
    { "floor",     DEC_ROUND_FLOOR },
    { "05up",      DEC_ROUND_05UP },
    { "max",       DEC_ROUND_MAX },
    { NULL, -1 }
};

static int convert_rounding_name_to_value(const char *name)
{
    rounding_map_t *entry;
    for (entry = rounding_maps; entry->name; ++entry) {
        if (strcasecmp(entry->name, name) == 0) {
            return entry->value;
        }
    }
    return -1;
}

static s_or_f handle_rounding(testfile_t *testfile, tokens_t *tokens)
{
    int rounding;

    rounding = convert_rounding_name_to_value(tokens->tokens[2]);
    if (rounding == -1) {
        DBGPRINT("convert_rounding_name_to_value failed.\n");
        return FAILURE;
    }

    testfile_context(testfile).round = rounding;
    return SUCCESS;
}

static s_or_f handle_max_exponent(testfile_t *testfile, tokens_t *tokens)
{
    int emax;

    emax = atoi(tokens->tokens[2]);
    testfile_context(testfile).emax = emax;
    return SUCCESS;
}

static s_or_f handle_min_exponent(testfile_t *testfile, tokens_t *tokens)
{
    int emin;

    emin = atoi(tokens->tokens[2]);
    testfile_context(testfile).emin = emin;
    return SUCCESS;
}

static s_or_f handle_clamp(testfile_t *testfile, tokens_t *tokens)
{
    int clamp;

    clamp = atoi(tokens->tokens[2]);
    testfile_context(testfile).clamp = clamp;
    return SUCCESS;
}

static s_or_f handle_extended(testfile_t *testfile, tokens_t *tokens)
{
    int extended;

    extended = atoi(tokens->tokens[2]);
#if DECSUBSET
    testfile_context(testfile).extended = extended;
#else
    testfile->extended = extended;
#endif
    return SUCCESS;
}

#define TEST_SUFFIX ".decTest"

static s_or_f handle_dectest(testfile_t *testfile, tokens_t *tokens)
{
    char *p;
    int path_len;
    int dir_len;
    int base_len;
    char *path;
    s_or_f result;

    p = strrchr(testfile->filename, '/');
    dir_len = p ? p - testfile->filename + 1 : 0;
    base_len = strlen(tokens->tokens[2]);
    path_len = dir_len + base_len + sizeof(TEST_SUFFIX);

    path = (char *)malloc(sizeof(char) * path_len);
    if (dir_len) {
        strncpy(path, testfile->filename, dir_len);
    }
    strcpy(path + dir_len, tokens->tokens[2]);
    strcpy(path + dir_len + base_len, TEST_SUFFIX);
    *(path + dir_len + base_len + sizeof(TEST_SUFFIX) - 1) = '\0';
    result = process_file(path, testfile);
    free(path);
    return result;
}

typedef s_or_f (*directive_handler_t)(testfile_t *testfile, tokens_t *tokens);

typedef struct _directive_handler_map_t {
    char *directive;
    directive_handler_t handler;
} directive_handler_map_t;

static directive_handler_map_t directive_handlers[] = {
    { "dectest", handle_dectest },
    { "precision", handle_precision },
    { "rounding", handle_rounding },
    { "maxexponent", handle_max_exponent },
    { "minexponent", handle_min_exponent },
    { "clamp", handle_clamp },
    { "extended", handle_extended },
    { "version", handle_version },
    { NULL, NULL }
};

static directive_handler_t get_directive_handler(const char *directive)
{
    directive_handler_map_t *entry;
    for (entry = directive_handlers; entry->directive; ++entry) {
        if (strcasecmp(entry->directive, directive) == 0) {
            return entry->handler;
        }
    }
    return NULL;
}

static s_or_f testfile_process_directive(testfile_t *testfile, tokens_t *tokens)
{
    directive_handler_t handler;
    handler = get_directive_handler(tokens->tokens[0]);
    if (!handler) {
        DBGPRINT("get_directive_handler failed.\n");
        return FAILURE;
    }
    return handler(testfile, tokens);
}

static s_or_f testfile_process_tokens(testfile_t *testfile, tokens_t *tokens)
{
    if (tokens_has_token(tokens, STR_ARROW)) {
        return testfile_process_test(testfile, tokens);
    } else if (tokens_is_directive(tokens)) {
        return testfile_process_directive(testfile, tokens);
    } else if (tokens_is_empty(tokens)) {
        return SUCCESS;
    }
    DBGPRINT("error in testfile_process_tokens. unsupported line type.\n");
    return FAILURE;
}

static s_or_f process_file(char *filename, testfile_t *parent)
{
    char line_buf[LINE_BUF_MAX_LEN];
    testfile_t testfile;
    tokens_t tokens;
    s_or_f result;

    result = SUCCESS;
    testfile_init(&testfile, filename);
    while (fgets(line_buf, LINE_BUF_MAX_LEN, testfile.fp) != NULL) {
        if (!tokens_tokenize(&tokens, line_buf)) {
            result = FAILURE;
        }
        if (result && !testfile_process_tokens(&testfile, &tokens)) {
            result = FAILURE;
        }
        tokens_dtor(&tokens);
        if (!result) {
            printf("== break because of failure.%s\n", testfile.filename);
            break;
        }
    }
    printf("== %s: tests=%d, success=%d, failure=%d, skip=%d\n",
        testfile.filename, testfile.test_count, testfile.success_count,
        testfile.failure_count, testfile.skip_count);
    if (parent) {
        parent->test_count += testfile.test_count;
        parent->success_count += testfile.success_count;
        parent->failure_count += testfile.failure_count;
        parent->skip_count += testfile.skip_count;
    }

    testfile_dtor(&testfile);
    return result;
}

int
main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s testfile.\n", argv[0]);
        return 1;
    }

    process_file(argv[1], NULL);
    return 0;
}
