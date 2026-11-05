/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* json.c -- see json.h for why this exists rather than a field scan. */

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

/* --------------------------------------------------------------------------
 * Building the tree
 * ------------------------------------------------------------------------ */

/* Parse position.  Strings are allocated separately because they are the only
 * variable-length part and the only thing an accessor hands out. */
struct build {
    const char *p;
    const char *end;
    const char *error;
};

static struct json *node_new(json_type type) {
    struct json *j = calloc(1, sizeof *j);
    if (j) j->type = type;
    return j;
}

static void skip_ws(struct build *b) {
    while (b->p < b->end &&
           (*b->p == ' ' || *b->p == '\t' || *b->p == '\r' || *b->p == '\n')) {
        b->p++;
    }
}

static struct json *parse_value(struct build *b);

/* A string, with the escapes JSON defines.  \uXXXX is decoded to UTF-8 because
 * control names are not guaranteed to be ASCII.  Surrogate pairs are not paired;
 * nothing in this protocol emits one. */
static char *parse_string_raw(struct build *b) {
    if (b->p >= b->end || *b->p != '"') {
        b->error = "expected a string";
        return NULL;
    }
    b->p++;

    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) {
        b->error = "out of memory";
        return NULL;
    }

    while (b->p < b->end && *b->p != '"') {
        unsigned char c = (unsigned char)*b->p;
        if (len + 5 >= cap) {
            cap *= 2;
            char *grown = realloc(out, cap);
            if (!grown) {
                free(out);
                b->error = "out of memory";
                return NULL;
            }
            out = grown;
        }
        if (c != '\\') {
            out[len++] = (char)c;
            b->p++;
            continue;
        }
        b->p++;
        if (b->p >= b->end) break;
        const char esc = *b->p++;
        switch (esc) {
        case '"':  out[len++] = '"';  break;
        case '\\': out[len++] = '\\'; break;
        case '/':  out[len++] = '/';  break;
        case 'b':  out[len++] = '\b'; break;
        case 'f':  out[len++] = '\f'; break;
        case 'n':  out[len++] = '\n'; break;
        case 'r':  out[len++] = '\r'; break;
        case 't':  out[len++] = '\t'; break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                if (b->p >= b->end || !isxdigit((unsigned char)*b->p)) {
                    free(out);
                    b->error = "truncated \\u escape";
                    return NULL;
                }
                const char h = *b->p++;
                cp = cp * 16 + (unsigned)(h <= '9' ? h - '0'
                                        : (h | 0x20) - 'a' + 10);
            }
            if (cp < 0x80) {
                out[len++] = (char)cp;
            } else if (cp < 0x800) {
                out[len++] = (char)(0xC0 | (cp >> 6));
                out[len++] = (char)(0x80 | (cp & 0x3F));
            } else {
                out[len++] = (char)(0xE0 | (cp >> 12));
                out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[len++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            free(out);
            b->error = "unknown escape";
            return NULL;
        }
    }

    if (b->p >= b->end) {
        free(out);
        b->error = "unterminated string";
        return NULL;
    }
    b->p++;                     /* the closing quote */
    out[len] = '\0';
    return out;
}

/* Append one element/member to a node still being built.  Arrays and objects
 * share a child list; an object also keeps a parallel list of names. */
static int push(struct json *parent, char *key, struct json *child) {
    const size_t n = parent->count;
    struct json **items = realloc(parent->items, (n + 1) * sizeof *items);
    if (!items) return 0;
    parent->items = items;
    parent->items[n] = child;

    if (parent->type == JSON_OBJ) {
        char **keys = realloc(parent->keys, (n + 1) * sizeof *keys);
        if (!keys) return 0;
        parent->keys = keys;
        parent->keys[n] = key;
    }
    parent->count = n + 1;
    return 1;
}

static struct json *parse_object(struct build *b) {
    struct json *j = node_new(JSON_OBJ);
    if (!j) {
        b->error = "out of memory";
        return NULL;
    }
    b->p++;                                     /* '{' */
    skip_ws(b);
    if (b->p < b->end && *b->p == '}') {
        b->p++;
        return j;
    }
    for (;;) {
        skip_ws(b);
        char *key = parse_string_raw(b);
        if (!key) {
            json_free(j);
            return NULL;
        }
        skip_ws(b);
        if (b->p >= b->end || *b->p != ':') {
            free(key);
            b->error = "expected ':' after a member name";
            json_free(j);
            return NULL;
        }
        b->p++;
        struct json *child = parse_value(b);
        if (!child || !push(j, key, child)) {
            free(key);
            json_free(child);
            json_free(j);
            b->error = b->error ? b->error : "out of memory";
            return NULL;
        }
        skip_ws(b);
        if (b->p < b->end && *b->p == ',') {
            b->p++;
            continue;
        }
        if (b->p < b->end && *b->p == '}') {
            b->p++;
            return j;
        }
        b->error = "expected ',' or '}'";
        json_free(j);
        return NULL;
    }
}

static struct json *parse_array(struct build *b) {
    struct json *j = node_new(JSON_ARR);
    if (!j) {
        b->error = "out of memory";
        return NULL;
    }
    b->p++;                                     /* '[' */
    skip_ws(b);
    if (b->p < b->end && *b->p == ']') {
        b->p++;
        return j;
    }
    for (;;) {
        struct json *child = parse_value(b);
        if (!child || !push(j, NULL, child)) {
            json_free(child);
            json_free(j);
            b->error = b->error ? b->error : "out of memory";
            return NULL;
        }
        skip_ws(b);
        if (b->p < b->end && *b->p == ',') {
            b->p++;
            continue;
        }
        if (b->p < b->end && *b->p == ']') {
            b->p++;
            return j;
        }
        b->error = "expected ',' or ']'";
        json_free(j);
        return NULL;
    }
}

static struct json *parse_value(struct build *b) {
    skip_ws(b);
    if (b->p >= b->end) {
        b->error = "expected a value";
        return NULL;
    }
    switch (*b->p) {
    case '{': return parse_object(b);
    case '[': return parse_array(b);
    case '"': {
        char *s = parse_string_raw(b);
        if (!s) return NULL;
        struct json *j = node_new(JSON_STR);
        if (!j) {
            free(s);
            b->error = "out of memory";
            return NULL;
        }
        j->str = s;
        return j;
    }
    case 't':
        if ((size_t)(b->end - b->p) >= 4 && !strncmp(b->p, "true", 4)) {
            b->p += 4;
            struct json *j = node_new(JSON_BOOL);
            if (j) j->num = 1;
            return j;
        }
        b->error = "expected a value";
        return NULL;
    case 'f':
        if ((size_t)(b->end - b->p) >= 5 && !strncmp(b->p, "false", 5)) {
            b->p += 5;
            return node_new(JSON_BOOL);
        }
        b->error = "expected a value";
        return NULL;
    case 'n':
        if ((size_t)(b->end - b->p) >= 4 && !strncmp(b->p, "null", 4)) {
            b->p += 4;
            return node_new(JSON_NULL);
        }
        b->error = "expected a value";
        return NULL;
    default: {
        char *endp = NULL;
        const double v = strtod(b->p, &endp);
        if (endp == b->p) {
            b->error = "expected a value";
            return NULL;
        }
        b->p = endp;
        struct json *j = node_new(JSON_NUM);
        if (j) j->num = v;
        return j;
    }
    }
}

/* --------------------------------------------------------------------------
 * Public
 * ------------------------------------------------------------------------ */

struct json *json_parse(const char *text) {
    if (!text) return NULL;
    struct build b = { text, text + strlen(text), NULL };
    struct json *j = parse_value(&b);
    if (!j) return NULL;
    skip_ws(&b);
    if (b.p != b.end) {
        /* Trailing bytes mean more than one message, or a truncated one glued
         * to the next.  The built tree is not of the intended line. */
        json_free(j);
        return NULL;
    }
    return j;
}

void json_free(struct json *j) {
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) {
        json_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

const struct json *json_get(const struct json *j, const char *key) {
    if (!j || j->type != JSON_OBJ || !key) return NULL;
    for (size_t i = 0; i < j->count; i++) {
        if (j->keys && j->keys[i] && !strcmp(j->keys[i], key)) return j->items[i];
    }
    return NULL;
}

/* An element of an array, or a member of an object; for the latter `keys[i]` is
 * its name.  The two share one storage, so one walk reads both.  A walk is the
 * only way to read a message whose member names are the data, such as the
 * `lamps` snapshot. */
const struct json *json_at(const struct json *j, size_t i) {
    if (!j || i >= j->count) return NULL;
    if (j->type != JSON_ARR && j->type != JSON_OBJ) return NULL;
    return j->items[i];
}

size_t json_count(const struct json *j) {
    return j ? j->count : 0;
}

const char *json_key(const struct json *j, size_t i) {
    if (!j || j->type != JSON_OBJ || !j->keys || i >= j->count) return NULL;
    return j->keys[i];
}

const char *json_str(const struct json *j, const char *key, const char *dflt) {
    const struct json *v = json_get(j, key);
    if (!v || v->type != JSON_STR) return dflt;
    return v->str;
}

int json_int(const struct json *j, const char *key, int dflt) {
    const struct json *v = json_get(j, key);
    if (!v) return dflt;
    if (v->type == JSON_BOOL) return (int)v->num;
    if (v->type != JSON_NUM) return dflt;
    return (int)lround(v->num);
}

int json_numbers(const struct json *j, const char *key, int *out, int max) {
    const struct json *v = json_get(j, key);
    if (!v || v->type != JSON_ARR) return 0;
    int n = 0;
    for (size_t i = 0; i < v->count && n < max; i++) {
        const struct json *e = v->items[i];
        if (e->type != JSON_NUM) return n;      /* a null stops the run */
        out[n++] = (int)lround(e->num);
    }
    return n;
}
