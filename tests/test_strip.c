#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <assert.h>

/* Self-contained algorithm validation for strip_words_from_text */
static char *strip_words_test(const char *src, const char *words_to_strip) {
    if (!src || !*src) return strdup("");
    if (!words_to_strip || !*words_to_strip) return strdup(src);

    char *result = strdup(src);
    char *words_copy = strdup(words_to_strip);
    char *token = strtok(words_copy, ",");

    while (token != NULL) {
        while (isspace((unsigned char)*token)) token++;
        char *end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end)) *end-- = '\0';

        size_t tlen = strlen(token);
        if (tlen > 0) {
            char *buf = malloc(strlen(result) + 1);
            size_t bi = 0;
            const char *p = result;
            while (*p != '\0') {
                if (strncasecmp(p, token, tlen) == 0) {
                    p += tlen;
                } else {
                    buf[bi++] = *p++;
                }
            }
            buf[bi] = '\0';
            free(result);
            result = buf;
        }
        token = strtok(NULL, ",");
    }
    free(words_copy);

    /* Collapse whitespace */
    char *clean = malloc(strlen(result) + 1);
    size_t ci = 0;
    int in_space = 0;
    const char *p = result;
    while (isspace((unsigned char)*p)) p++;
    while (*p != '\0') {
        if (isspace((unsigned char)*p)) {
            if (!in_space) {
                clean[ci++] = ' ';
                in_space = 1;
            }
        } else {
            clean[ci++] = *p;
            in_space = 0;
        }
        p++;
    }
    while (ci > 0 && isspace((unsigned char)clean[ci - 1])) ci--;
    clean[ci] = '\0';
    free(result);
    return clean;
}

int main(void) {
    char *r1 = strip_words_test("PU I.2 - Project Saluran", "PU I.2, Project");
    assert(strcmp(r1, "- Saluran") == 0);
    free(r1);

    char *r2 = strip_words_test("FOTO 10 DOKUMENTASI", "foto");
    assert(strcmp(r2, "10 DOKUMENTASI") == 0);
    free(r2);

    char *r3 = strip_words_test("Normal Text Without Match", "foo, bar");
    assert(strcmp(r3, "Normal Text Without Match") == 0);
    free(r3);

    printf("All strip tests passed!\n");
    return 0;
}
