/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Gerard Braad (Regroove | RGRV) */
/* json.h -- a small JSON parser for reading the panel's door.
 *
 * A field scan is not enough.  A `lamps` snapshot nests objects with the same
 * keys as the message that contains them:
 *
 *     {"t":"lamps","all":{"note:16":{"v":7,"rgb":[255,255,0]}, ...}}
 *
 * A scanner that finds the first `"rgb"` can return another lamp's colour.  This
 * is a recursive-descent parser that builds a tree for the client to walk.
 *
 * It is deliberately standalone, so the client depends only on the protocol and
 * not on the panel's sources.  The tree is owned by the returned node and
 * json_free releases it.  Numbers are doubles; the accessors round.
 */

#ifndef TRACKER_FIRE_JSON_H
#define TRACKER_FIRE_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUM,
    JSON_STR,
    JSON_ARR,
    JSON_OBJ
} json_type;

struct json {
    json_type type;

    double  num;        /* JSON_NUM, and 0/1 for JSON_BOOL */
    char   *str;        /* JSON_STR, decoded (escapes resolved), owned */

    /* JSON_ARR and JSON_OBJ: `items[i]` is the i'th element, and for an object
     * `keys[i]` is its name.  `count` is the shared length. */
    struct json **items;
    char        **keys;
    size_t        count;
};

/* Parse a whole line.  Returns NULL on a syntax error.  Trailing whitespace is
 * allowed; trailing anything else is not. */
struct json *json_parse(const char *text);

void json_free(struct json *j);

/* A member of an object, or NULL. */
const struct json *json_get(const struct json *j, const char *key);

/* An element of an array, or a member of an object (`keys[i]` names it).
 * NULL on anything else, or past the end. */
const struct json *json_at(const struct json *j, size_t i);

/* How many members/elements, and the name of the i'th member of an object (NULL
 * for an array).  This walks the `lamps` snapshot, whose member names are the
 * lamp keys. */
size_t json_count(const struct json *j);
const char *json_key(const struct json *j, size_t i);

/* A string member, or `dflt` when missing or not a string.  The pointer belongs
 * to the tree. */
const char *json_str(const struct json *j, const char *key, const char *dflt);

/* A number member, rounded, or `dflt` when missing or not a number. */
int json_int(const struct json *j, const char *key, int dflt);

/* A member that is an array of numbers, copied into `out` as ints.  Returns how
 * many were copied, up to `max`; 0 when missing, null, or not an array.  This is
 * `rgb`, which is null for an unresolvable palette index. */
int json_numbers(const struct json *j, const char *key, int *out, int max);

#endif /* TRACKER_FIRE_JSON_H */
