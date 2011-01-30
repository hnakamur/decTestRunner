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
    int operand_count;
    char **operands;
    decNumber **operand_numbers;
    uint32_t expected_status;
    char *expected;
    decContext *context;
    uint32_t actual_status;
    char *actual;
    decNumber *actual_number;
} testcase_t;

static s_or_f process_file(char *filename, testfile_t *parent);

static void print_error(const char *format, ...)
{
	va_list args;

	va_start(args, format);
    vfprintf(stdout, format, args);
    fflush(stdout);
	va_end(args);
}

static void context_print(const decContext *context)
{
    printf("context prec=%d, round=%d, emax=%d, emin=%d, status=%x, traps=%x, clamp=%d, extented=%d\n",
        context->digits, context->round, context->emax, context->emin,
        context->status, context->traps, context->clamp, context->extended);
}

static char *convert_number_to_string(const decNumber *dn)
{
    char *s;
    int buf_len;

    buf_len = dn->digits + 14;
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

    buf_len = dn->digits + 14;
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
    int len;

    quote_count = count_char(s + 1, n - 2, quote);
    len = n - 2 - quote_count / 2;
    dup = (char *)malloc(sizeof(char) * (len + 1));
    for (i = 0; i < len; ++i) {
        dup[i] = s[i + 1];
        if (dup[i] == quote) {
            ++i;
        }
    }
    dup[i] = '\0';
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
        print_error("%s[%d] realloc failed\n", __FILE__, __LINE__);
        return FAILURE;
    }
    tokens->tokens[tokens->count] = unquote_token(text, length);
//    print_error("DEBUG %s[%d] token=%s.\n", __FILE__, __LINE__, tokens->tokens[tokens->count]);
    if (!tokens->tokens[tokens->count]) {
        print_error("%s[%d] unquote_token failed\n", __FILE__, __LINE__);
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
            print_error("%s[%d] tokens_add_token failed\n", __FILE__, __LINE__);
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
    testfile->context.extended = 0;

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
    for (p = s; *p; ++p) {
        if (isdigit(*p)) {
            ++count;
        }
        if (*p == 'e' || *p == 'E') {
            break;
        }
    }
//printf("count_coefficient_digit %s -> %d\n", s, count);
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
    { "Lost_digits",          DEC_Lost_digits },
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
    print_error("%s[%d] error in convert_status_name_to_value. name not found: %s\n", __FILE__, __LINE__, name);
    return FAILURE;
}

static s_or_f tokens_get_conditions(tokens_t *tokens, int offset,
    uint32_t *status)
{
    int i;
    uint32_t flag;

    *status = 0;
    for (i = offset; i < tokens->count; ++i) {
        if (!convert_status_name_to_value(tokens->tokens[i], &flag)) {
            print_error("%s[%d] convert_status_name_to_value failed\n", __FILE__, __LINE__);
            return FAILURE;
        }

        *status |= flag;
    }
    return SUCCESS;
}

static decNumber *alloc_number(long numdigits)
{
    uInt needbytes;
    decNumber *number;

    needbytes = sizeof(decNumber) + (D2U(numdigits) - 1) * sizeof(Unit);
    number = (decNumber *)malloc(needbytes);
    if (!number) {
        print_error("%s[%d] no more memory in alloc_number.\n", __FILE__, __LINE__);
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
        print_error("%s[%d] error in parse_hex expected length is %d, but was %d [%s]\n", __FILE__, __LINE__, bytes * 2, strlen(s), s);
        return FAILURE;
    }

    j = 0;
    for (i = 0; i < bytes; ++i, j += 2) {
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

static s_or_f parse_decimal32_hex(const char *s, decNumber **number)
{
    decimal32 dec32;

    if (!parse_hex(DECIMAL32_Bytes, dec32.bytes, s)) {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL32_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal32ToNumber(&dec32, *number);
    return SUCCESS;
}

static s_or_f parse_decimal64_hex(const char *s, decNumber **number)
{
    decimal64 dec64;

    if (!parse_hex(DECIMAL64_Bytes, dec64.bytes, s)) {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL64_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal64ToNumber(&dec64, *number);
    return SUCCESS;
}

static s_or_f parse_decimal128_hex(const char *s, decNumber **number)
{
    decimal128 dec128;

    if (!parse_hex(DECIMAL128_Bytes, dec128.bytes, s)) {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
        return FAILURE;
    }

    *number = alloc_number(DECIMAL128_Pmax);
    if (!*number) {
        return FAILURE;
    }
    decimal128ToNumber(&dec128, *number);
    return SUCCESS;
}

static s_or_f parse_hex_notation(const char *s, decNumber **number)
{
    int len;

    len = strlen(s) - 1;
    if (len == 0) {
        *number = NULL;
    } else if (len == 8) {
        if (!parse_decimal32_hex(s + 1, number)) {
            print_error("%s[%d] parse_decimal32_hex failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else if (len == 16) {
        if (!parse_decimal64_hex(s + 1, number)) {
            print_error("%s[%d] parse_decimal32_hex failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else if (len == 32) {
        if (!parse_decimal128_hex(s + 1, number)) {
            print_error("%s[%d] parse_decimal32_hex failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
        return FAILURE;
    }
    return SUCCESS;
}

static s_or_f parse_decimal32_hex_canonical(const char *s, decNumber **number)
{
    decimal32 dec32;
    decimal32 dec32canonical;

    if (!parse_hex(DECIMAL32_Bytes, dec32.bytes, s)) {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
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
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
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
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
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
            print_error("%s[%d] parse_decimal32_hex_canonical failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else if (len == 16) {
        if (!parse_decimal64_hex_canonical(s + 1, number)) {
            print_error("%s[%d] parse_decimal32_hex_canonical failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else if (len == 32) {
        if (!parse_decimal128_hex_canonical(s + 1, number)) {
            print_error("%s[%d] parse_decimal32_hex_canonical failed [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
    } else {
        print_error("%s[%d] invalid hex notation [%s]\n", __FILE__, __LINE__, s);
        return FAILURE;
    }
    return SUCCESS;
}

static s_or_f testcase_convert_operands_to_numbers(testcase_t *testcase)
{
    char *op;
    bool is_using_directive_precision;
    int32_t digits;
    char *s;
    int i;
    int needbytes;
    decContext ctx;

    op = testcase->operator;
    is_using_directive_precision = (strcasecmp(op, "apply") == 0
        || strcasecmp(op, "tosci") == 0 || strcasecmp(op, "toeng") == 0);
    testcase->context->traps = 0;
    testcase->context->status = 0;

    needbytes = sizeof(decNumber *) * testcase->operand_count;
    testcase->operand_numbers = (decNumber **)malloc(needbytes);
    if (!testcase->operand_numbers) {
        print_error("%s[%d] out of memory in testcase_convert_operands_to_numbers\n", __FILE__, __LINE__);
        return FAILURE;
    }
    memset(testcase->operand_numbers, 0, needbytes);

    ctx = *testcase->context;
    for (i = 0; i < testcase->operand_count; ++i) {
        s = testcase->operands[i];
        if (strncmp(s, "#", 1) == 0) {
            if (strcmp(op, "canonical") == 0) {
                if (!parse_hex_notation_canonical(s,
                    &testcase->operand_numbers[i])
                ) {
                    print_error("%s[%d] parse_decimal64_hex_canonical failed for operand %d. [%s]\n", __FILE__, __LINE__, i, s);
                    return FAILURE;
                }
            } else {
                if (!parse_hex_notation(s, &testcase->operand_numbers[i])) {
                    print_error("%s[%d] parse_hex_notation failed for operand %d. [%s]\n", __FILE__, __LINE__, i, s);
                    return FAILURE;
                }
            }
        } else {
            if (!is_using_directive_precision) {
                ctx.digits = count_coefficient_digit(s);
                ctx.emax = DEC_MAX_EMAX;
                ctx.emin = DEC_MIN_EMIN;
            }
            testcase->operand_numbers[i] = alloc_number(ctx.digits);
            if (!testcase->operand_numbers[i]) {
                return FAILURE;
            }
            decNumberFromString(testcase->operand_numbers[i], s, &ctx);
        }
    }
    if (is_using_directive_precision) {
        testcase->context->status = ctx.status;
    }

    return SUCCESS;
}

static s_or_f testcase_init(testcase_t *testcase, testfile_t *testfile,
    tokens_t *tokens)
{
    char *s;
    int i;
    decNumber *n;

    testcase->id = tokens->tokens[0];
    testcase->operator = tokens->tokens[1];
    testcase->operand_count = tokens_count_operands(tokens);
    testcase->context = &testfile->context;
    testcase->actual_status = 0;
    testcase->actual = NULL;
    testcase->actual_number = NULL;
    if (!tokens_get_conditions(tokens, 2 + testcase->operand_count + 2,
        &testcase->expected_status)
    ) {
        print_error("%s[%d] tokens_get_conditions failed.\n", __FILE__, __LINE__);
        return FAILURE;
    }

    testcase->operand_numbers = NULL;
    testcase->operands = (char **)malloc(sizeof(char *) * testcase->operand_count);
    if (!testcase->operands) {
        print_error("%s[%d] out of memory in testcase_init\n", __FILE__, __LINE__);
        return FAILURE;
    }
    for (i = 0; i < testcase->operand_count; ++i) {
        testcase->operands[i] = tokens->tokens[i + 2];
    }

    s = tokens->tokens[2 + testcase->operand_count + 1];
    if (strncmp(s, "#", 1) == 0) {
        if (!parse_hex_notation(s, &n)) {
            print_error("%s[%d] parse_hex_notation failed for result. [%s]\n", __FILE__, __LINE__, s);
            return FAILURE;
        }
        testcase->expected = convert_number_to_string(n);
        free(n);
    } else {
        testcase->expected = strdup(s);
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
        print_error("%s[%d] error in testcase_run. operator is empty.\n", __FILE__, __LINE__);
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
            testcase->actual = convert_number_to_string(operands[0]);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'c':
        if (strcasecmp(testcase->operator, "canonical") == 0) {
            testcase->actual = convert_number_to_string(operands[0]);
        } else if (strcasecmp(testcase->operator, "class") == 0) {
            enum decClass num_class;
            num_class = decNumberClass(operands[0], testcase->context);
            testcase->actual = strdup(decNumberClassToString(num_class));
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'e':
        if (strcasecmp(testcase->operator, "exp") == 0) {
            decNumberExp(result, operands[0], testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'f':
        if (strcasecmp(testcase->operator, "fma") == 0) {
            decNumberFMA(result, operands[0], operands[1], operands[2],
                testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'i':
        if (strcasecmp(testcase->operator, "invert") == 0) {
            decNumberInvert(result, operands[0], testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'o':
        if (strcasecmp(testcase->operator, "or") == 0) {
            decNumberOr(result, operands[0], operands[1], testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'q':
        if (strcasecmp(testcase->operator, "quantize") == 0) {
            decNumberQuantize(result, operands[0], operands[1],
                testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 't':
        if (strcasecmp(testcase->operator, "toeng") == 0) {
            testcase->actual = convert_number_to_eng_string(operands[0]);
        } else if (strcasecmp(testcase->operator, "tointegral") == 0) {
            decNumberToIntegralValue(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "tointegralx") == 0) {
            decNumberToIntegralExact(result, operands[0], testcase->context);
        } else if (strcasecmp(testcase->operator, "tosci") == 0) {
            testcase->actual = convert_number_to_string(operands[0]);
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
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    case 'x':
        if (strcasecmp(testcase->operator, "xor") == 0) {
            decNumberXor(result, operands[0], operands[1], testcase->context);
        } else {
            print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
            return FAILURE;
        }
        break;
    default:
        print_error("%s[%d] error in testcase_run. unknown operator: %s.\n", __FILE__, __LINE__, testcase->operator);
        return FAILURE;
    }

    if (!testcase->actual) {
        testcase->actual = convert_number_to_string(result);
    }
    testcase->actual_status = testcase->context->status;

    return SUCCESS;
}

static void testcase_print(testcase_t *testcase)
{
    int i;

    printf("id=%s, op=%s, opeprands=[", testcase->id, testcase->operator);
    for (i = 0; i < testcase->operand_count; ++i) {
        if (i > 0) {
            printf(", ");
        }
        printf("%s", testcase->operands[i]);
    }
    printf("] -> ");
    printf("%s", testcase->expected);
    printf(" expected_status=%x\n", testcase->expected_status);
    fflush(stdout);
}

static bool testcase_check(testcase_t *testcase)
{
    if (strcmp(testcase->actual, testcase->expected) == 0) {
        if (testcase->actual_status == testcase->expected_status) {
            return TRUE;
        } else {
            testcase_print(testcase);
            printf("status unmatch: actual=%x, expected=%x\n",
                testcase->actual_status, testcase->expected_status);
            context_print(testcase->context);
            return FALSE;
        }
    } else {
        testcase_print(testcase);
        printf("number unmatch: actual=%s,\n", testcase->actual);
        printf("              expected=%s\n", testcase->expected);
        context_print(testcase->context);
        return FALSE;
    }
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

    if (testcase->actual) {
        free(testcase->actual);
    }
    if (testcase->actual_number) {
        free(testcase->actual_number);
    }

    if (testcase->expected) {
        free(testcase->expected);
    }
}

static s_or_f testfile_process_test(testfile_t *testfile, tokens_t *tokens)
{
    testcase_t testcase;

    ++testfile->test_count;
    if (!testcase_init(&testcase, testfile, tokens)) {
        print_error("%s[%d] testcase_init failed.\n", __FILE__, __LINE__);
        return FAILURE;
    }
    if (testcase_has_null_operand(&testcase)) {
        printf("%s: null arg not supported -> skip.\n", testcase.id);
        ++testfile->skip_count;
    } else {
//        testcase_print(&testcase);
        if (!testcase_run(&testcase)) {
            tokens_print(tokens);
            print_error("%s[%d] testcase_run failed.\n", __FILE__, __LINE__);
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
        print_error("%s[%d] convert_rounding_name_to_value failed.\n", __FILE__, __LINE__);
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
    testfile_context(testfile).extended = extended;
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
        print_error("%s[%d] get_directive_handler failed.\n", __FILE__, __LINE__);
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
    print_error("%s[%d] error in testfile_process_tokens. unsupported line type.\n", __FILE__, __LINE__);
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
//        printf(line_buf);
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
