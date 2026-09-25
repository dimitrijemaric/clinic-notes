#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "privacy.h"

static pcre2_code *compile(const char *pattern) {
    int error;
    PCRE2_SIZE offset;
    return pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        PCRE2_UTF | PCRE2_UCP | PCRE2_CASELESS, &error, &offset, NULL);
}

static char *replace(const char *text, const char *pattern, const char *replacement) {
    pcre2_code *re = compile(pattern);
    if (!re) return NULL;
    PCRE2_SIZE size = strlen(text) + 64;
    char *output = NULL;
    int result;
    do {
        free(output);
        output = malloc(size + 1);
        if (!output) break;
        PCRE2_SIZE capacity = size;
        result = pcre2_substitute(re, (PCRE2_SPTR)text, PCRE2_ZERO_TERMINATED, 0,
            PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_LITERAL | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
            NULL, NULL, (PCRE2_SPTR)replacement, PCRE2_ZERO_TERMINATED,
            (PCRE2_UCHAR *)output, &capacity);
        size = capacity;
        if (result >= 0) output[size] = 0;
    } while (output && result == PCRE2_ERROR_NOMEMORY);
    pcre2_code_free(re);
    if (!output || result < 0) { free(output); return NULL; }
    return output;
}

static char *literal_pattern(const char *value, int boundary) {
    size_t length = strlen(value);
    char *result = malloc(length * 2 + 64);
    if (!result) return NULL;
    char *p = result;
    if (boundary) { strcpy(p, "(?<![\\p{L}\\p{N}])"); p += strlen(p); }
    for (const char *s = value; *s; s++) {
        if (strchr("\\.^$|?*+()[]{}", *s)) *p++ = '\\';
        *p++ = *s;
    }
    if (boundary) { strcpy(p, "(?![\\p{L}\\p{N}])"); p += strlen(p); }
    *p = 0;
    return result;
}

static int matches(const char *text, const char *pattern) {
    pcre2_code *re = compile(pattern);
    if (!re) return 1; /* fail closed */
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(re, NULL);
    int result = data ? pcre2_match(re, (PCRE2_SPTR)text, strlen(text), 0, 0, data, NULL) : 0;
    pcre2_match_data_free(data);
    pcre2_code_free(re);
    return result != PCRE2_ERROR_NOMATCH;
}

int contains_identity(const char *text, const char *first, const char *last, const char *dob) {
    const char *values[] = { first, last, dob };
    if (!text) return 1;
    for (int i = 0; i < 3; i++) {
        if (!values[i] || !*values[i]) continue;
        char *pattern = literal_pattern(values[i], i != 2);
        if (!pattern) return 1;
        int found = matches(text, pattern);
        free(pattern);
        if (found) return 1;
    }
    return 0;
}

char *anonymize(const char *text, const char *first, const char *last, const char *dob) {
    if (!text || !first || !last || !dob) return NULL;
    size_t n = strlen(first) + strlen(last) + 2;
    char *full = malloc(n);
    if (!full) return NULL;
    snprintf(full, n, "%s %s", first, last);
    const char *values[] = { full, first, last, dob };
    const char *patterns[] = {
        "[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}",
        "https?://[^\\s]+",
        "(?<![\\p{L}\\p{N}])(?:\\+?\\d[\\d .()/\\-]{6,}\\d)(?![\\p{L}\\p{N}])",
        "\\b(?:MRN|medical[ ]+record(?:[ ]+number)?|patient[ ]+id|social[ ]+security|SSN|address|patient[ ]+name|name|ime|prezime|jmbg|adresa|broj[ ]+kartona|datum[ ]+rođenja)[ ]*[:#-]?[ ]*[^\\n,;]+"
    };
    const char *replacements[] = { "[EMAIL REDACTED]", "[URL REDACTED]", "[NUMBER REDACTED]", "[DIRECT IDENTIFIER REDACTED]" };
    char *current = malloc(strlen(text) + 1);
    if (current) strcpy(current, text);
    for (int i = 0; current && i < 4; i++) {
        if (!*values[i]) continue;
        char *pattern = literal_pattern(values[i], i != 3);
        char *next = pattern ? replace(current, pattern, i == 3 ? "[DATE REDACTED]" : "[PATIENT]") : NULL;
        free(pattern); free(current); current = next;
    }
    free(full);
    for (int i = 0; current && i < 4; i++) {
        char *next = replace(current, patterns[i], replacements[i]);
        free(current); current = next;
    }
    if (current && contains_identity(current, first, last, dob)) { free(current); return NULL; }
    return current;
}
