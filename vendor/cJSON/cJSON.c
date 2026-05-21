/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  SPDX-License-Identifier: MIT
*/
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>
#include "cJSON.h"

typedef struct internal_hooks {
    void *((*allocate)(size_t size));
    void ((*deallocate)(void *pointer));
    void *((*reallocate)(void *pointer, size_t size));
} internal_hooks;

static void *internal_malloc(size_t size) { return malloc(size); }
static void  internal_free(void *ptr)     { free(ptr); }
static void *internal_realloc(void *p, size_t sz) { return realloc(p, sz); }

static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

static char *cJSON_strdup(const char *str, const internal_hooks * const hooks) {
    size_t len;
    char *copy;
    if (!str) return NULL;
    len = strlen(str) + 1;
    if (!(copy = (char*)hooks->allocate(len))) return NULL;
    memcpy(copy, str, len);
    return copy;
}

void cJSON_InitHooks(cJSON_Hooks *hooks) {
    if (!hooks) {
        global_hooks.allocate   = internal_malloc;
        global_hooks.deallocate = internal_free;
        global_hooks.reallocate = internal_realloc;
        return;
    }
    global_hooks.allocate   = hooks->malloc_fn ? hooks->malloc_fn : internal_malloc;
    global_hooks.deallocate = hooks->free_fn   ? hooks->free_fn   : internal_free;
    global_hooks.reallocate = hooks->malloc_fn ? internal_realloc : internal_realloc;
}

void *cJSON_malloc(size_t size) { return global_hooks.allocate(size); }
void  cJSON_free(void *obj)     { global_hooks.deallocate(obj); }

static cJSON *cJSON_New_Item(const internal_hooks * const h) {
    cJSON *node = (cJSON*)h->allocate(sizeof(cJSON));
    if (node) memset(node, 0, sizeof(cJSON));
    return node;
}

void cJSON_Delete(cJSON *item) {
    cJSON *next;
    while (item) {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && item->child)
            cJSON_Delete(item->child);
        if (!(item->type & cJSON_IsReference) && item->valuestring)
            global_hooks.deallocate(item->valuestring);
        if (!(item->type & cJSON_StringIsConst) && item->string)
            global_hooks.deallocate(item->string);
        global_hooks.deallocate(item);
        item = next;
    }
}

typedef struct {
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth;
} parse_buffer;

#define can_read(b, l)  ((b)->offset + (l) <= (b)->length)
#define can_access_at_index(b, i) ((b)->offset + (i) < (b)->length)
#define cannot_access_at_index(b, i) !can_access_at_index(b, i)
#define buffer_at_offset(b) ((b)->content + (b)->offset)

/* printbuffer must be declared before any function using it */
typedef struct printbuffer {
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t depth;
    int noalloc;
    int format;
} printbuffer;

static int parse_value(cJSON * const item, parse_buffer * const buf);
static int print_value(const cJSON * const item, printbuffer * const p);

/* skip whitespace */
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buf) {
    if (!buf || !buf->content) return NULL;
    if (cannot_access_at_index(buf, 0)) return buf;
    while (can_access_at_index(buf, 0) && (buf->content[buf->offset] <= 32))
        buf->offset++;
    if (buf->offset == buf->length)
        buf->offset--;
    return buf;
}

static parse_buffer *skip_utf8_bom(parse_buffer * const buf) {
    if (!buf || !buf->content || buf->offset != 0) return NULL;
    if (can_access_at_index(buf, 4) &&
        (strncmp((const char*)buf->content, "\xEF\xBB\xBF", 3) == 0))
        buf->offset += 3;
    return buf;
}

/* parse string */
static unsigned parse_hex4(const unsigned char * const str) {
    unsigned h = 0;
    for (int i = 0; i < 4; i++) {
        if (str[i] >= '0' && str[i] <= '9') h = (h << 4) + (str[i] - '0');
        else if (str[i] >= 'A' && str[i] <= 'F') h = (h << 4) + 10 + (str[i] - 'A');
        else if (str[i] >= 'a' && str[i] <= 'f') h = (h << 4) + 10 + (str[i] - 'a');
        else return 0;
    }
    return h;
}

static unsigned char utf16_literal_to_utf8(const unsigned char * const s,
                                            const unsigned char * const e,
                                            unsigned char **o) {
    unsigned int c = 0;
    unsigned int first = parse_hex4(s + 2);
    unsigned char len = 0;
    unsigned char *out = *o;
    if (first >= 0xDC00 && first <= 0xDFFF) return 0;
    if (first >= 0xD800 && first <= 0xDBFF) {
        if (e - s < 6 || s[4] != '\\' || s[5] != 'u') return 0;
        unsigned int second = parse_hex4(s + 6);
        if (second < 0xDC00 || second > 0xDFFF) return 0;
        c = (unsigned int)(((first - 0xD800) << 10) | (second - 0xDC00)) + 0x10000;
        len = 4;
    } else {
        c = first;
        if      (c < 0x80)   len = 1;
        else if (c < 0x800)  len = 2;
        else                 len = 3;
    }
    switch (len) {
        case 4: out[3] = (unsigned char)((c | 0x80) & 0xBF); c >>= 6; /* fall */
        case 3: out[2] = (unsigned char)((c | 0x80) & 0xBF); c >>= 6; /* fall */
        case 2: out[1] = (unsigned char)((c | 0x80) & 0xBF); c >>= 6; /* fall */
        case 1: out[0] = (unsigned char)(c | ((0xF0 << (4 - len)) & 0xFF));
    }
    *o += len;
    return (unsigned char)(len > 0 ? len + (len > 2 ? 4 : 2) : 0);
}

static int parse_string(cJSON * const item, parse_buffer * const buf) {
    const unsigned char *s = buffer_at_offset(buf) + 1;
    const unsigned char *e;
    unsigned char *out;
    unsigned char *out_ptr;
    size_t allocation_length = 0;
    size_t skipped_bytes = 0;

    if (buffer_at_offset(buf)[0] != '\"') return 0;

    /* find end of string */
    for (e = s; e != buf->content + buf->length; e++) {
        if (*e == '\"') break;
        if (*e == '\\' && e + 1 < buf->content + buf->length) {
            skipped_bytes++;
            e++;
            if (*e == 'u') skipped_bytes += 4;
        }
    }
    if (e == buf->content + buf->length) return 0;

    allocation_length = (size_t)(e - s) - skipped_bytes;
    out = (unsigned char*)global_hooks.allocate(allocation_length + sizeof(""));
    if (!out) return 0;

    out_ptr = out;
    while (s < e) {
        if (*s != '\\') {
            *out_ptr++ = *s++;
        } else {
            s++;
            switch (*s) {
                case 'b': *out_ptr++ = '\b'; break;
                case 'f': *out_ptr++ = '\f'; break;
                case 'n': *out_ptr++ = '\n'; break;
                case 'r': *out_ptr++ = '\r'; break;
                case 't': *out_ptr++ = '\t'; break;
                case '\"': case '\\': case '/': *out_ptr++ = *s; break;
                case 'u': {
                    unsigned char seq = utf16_literal_to_utf8(s - 1, e, &out_ptr);
                    if (seq == 0) { global_hooks.deallocate(out); return 0; }
                    s += seq - 1;
                    break;
                }
                default: *out_ptr++ = *s; break;
            }
            s++;
        }
    }
    *out_ptr = '\0';

    item->type = cJSON_String;
    item->valuestring = (char*)out;
    buf->offset = (size_t)(e - buf->content) + 1;
    return 1;
}

static int parse_number(cJSON * const item, parse_buffer * const buf) {
    double num = 0;
    unsigned char *endptr;
    const unsigned char *s = buffer_at_offset(buf);
    char numbuf[64];
    size_t len = 0;

    while (can_access_at_index(buf, len) && len < 63 &&
           (s[len] == '-' || s[len] == '+' || (s[len] >= '0' && s[len] <= '9') ||
            s[len] == '.' || s[len] == 'e' || s[len] == 'E'))
        len++;
    memcpy(numbuf, s, len);
    numbuf[len] = '\0';
    num = strtod(numbuf, (char**)&endptr);

    item->type = cJSON_Number;
    item->valuedouble = num;
    item->valueint = (int)num;
    buf->offset += (size_t)(strlen(numbuf));
    return 1;
}

static int parse_array(cJSON * const item, parse_buffer * const buf);
static int parse_object(cJSON * const item, parse_buffer * const buf);

static const char *ep = NULL;

static int parse_value(cJSON * const item, parse_buffer * const buf) {
    if (!buf || !buf->content) return 0;
    buffer_skip_whitespace(buf);
    if (cannot_access_at_index(buf, 0)) return 0;

    switch (buffer_at_offset(buf)[0]) {
        case 'n':
            if (can_read(buf, 4) && strncmp((const char*)buffer_at_offset(buf), "null", 4) == 0) {
                item->type = cJSON_NULL;
                buf->offset += 4;
                return 1;
            }
            break;
        case 'f':
            if (can_read(buf, 5) && strncmp((const char*)buffer_at_offset(buf), "false", 5) == 0) {
                item->type = cJSON_False;
                buf->offset += 5;
                return 1;
            }
            break;
        case 't':
            if (can_read(buf, 4) && strncmp((const char*)buffer_at_offset(buf), "true", 4) == 0) {
                item->type = cJSON_True;
                item->valueint = 1;
                buf->offset += 4;
                return 1;
            }
            break;
        case '\"':
            return parse_string(item, buf);
        case '[':
            return parse_array(item, buf);
        case '{':
            return parse_object(item, buf);
        default:
            if (*buffer_at_offset(buf) == '-' ||
                (*buffer_at_offset(buf) >= '0' && *buffer_at_offset(buf) <= '9'))
                return parse_number(item, buf);
            break;
    }
    ep = (const char*)buffer_at_offset(buf);
    return 0;
}

static int parse_array(cJSON * const item, parse_buffer * const buf) {
    cJSON *head = NULL, *current = NULL;

    if (buf->depth >= 512) return 0;
    buf->depth++;

    if (buffer_at_offset(buf)[0] != '[') return 0;
    buf->offset++;
    buffer_skip_whitespace(buf);

    if (can_access_at_index(buf, 0) && buffer_at_offset(buf)[0] == ']') {
        item->type = cJSON_Array;
        buf->offset++;
        buf->depth--;
        return 1;
    }

    while (1) {
        cJSON *new_item = cJSON_New_Item(&global_hooks);
        if (!new_item) goto fail;
        if (!head) { head = current = new_item; }
        else { current->next = new_item; new_item->prev = current; current = new_item; }

        buffer_skip_whitespace(buf);
        if (!parse_value(new_item, buf)) goto fail;
        buffer_skip_whitespace(buf);

        if (!can_access_at_index(buf, 0)) goto fail;
        if (buffer_at_offset(buf)[0] == ',') buf->offset++;
        else if (buffer_at_offset(buf)[0] == ']') break;
        else goto fail;
    }

    buf->offset++;
    item->type = cJSON_Array;
    item->child = head;
    buf->depth--;
    return 1;
fail:
    cJSON_Delete(head);
    buf->depth--;
    return 0;
}

static int parse_object(cJSON * const item, parse_buffer * const buf) {
    cJSON *head = NULL, *current = NULL;

    if (buf->depth >= 512) return 0;
    buf->depth++;

    if (buffer_at_offset(buf)[0] != '{') return 0;
    buf->offset++;
    buffer_skip_whitespace(buf);

    if (can_access_at_index(buf, 0) && buffer_at_offset(buf)[0] == '}') {
        item->type = cJSON_Object;
        buf->offset++;
        buf->depth--;
        return 1;
    }

    while (1) {
        cJSON *new_item = cJSON_New_Item(&global_hooks);
        if (!new_item) goto fail;
        if (!head) { head = current = new_item; }
        else { current->next = new_item; new_item->prev = current; current = new_item; }

        buffer_skip_whitespace(buf);
        if (buffer_at_offset(buf)[0] != '\"') goto fail;
        if (!parse_string(new_item, buf)) goto fail;
        new_item->string = new_item->valuestring;
        new_item->valuestring = NULL;
        buffer_skip_whitespace(buf);
        if (!can_access_at_index(buf, 0) || buffer_at_offset(buf)[0] != ':') goto fail;
        buf->offset++;
        buffer_skip_whitespace(buf);
        if (!parse_value(new_item, buf)) goto fail;
        buffer_skip_whitespace(buf);

        if (!can_access_at_index(buf, 0)) goto fail;
        if (buffer_at_offset(buf)[0] == ',') buf->offset++;
        else if (buffer_at_offset(buf)[0] == '}') break;
        else goto fail;
    }

    buf->offset++;
    item->type = cJSON_Object;
    item->child = head;
    buf->depth--;
    return 1;
fail:
    cJSON_Delete(head);
    buf->depth--;
    return 0;
}

cJSON *cJSON_ParseWithLength(const char *value, size_t len) {
    parse_buffer buf = { 0, 0, 0, 0 };
    cJSON *item = cJSON_New_Item(&global_hooks);
    ep = NULL;
    if (!item) return NULL;

    buf.content = (const unsigned char*)value;
    buf.length = len;
    buf.offset = 0;

    skip_utf8_bom(&buf);
    if (!parse_value(item, buffer_skip_whitespace(&buf))) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

cJSON *cJSON_Parse(const char *value) {
    if (!value) return NULL;
    return cJSON_ParseWithLength(value, strlen(value) + sizeof(""));
}

const char *cJSON_GetErrorPtr(void) { return ep; }

char *cJSON_GetStringValue(const cJSON *item) {
    if (!cJSON_IsString(item)) return NULL;
    return item->valuestring;
}

double cJSON_GetNumberValue(const cJSON *item) {
    if (!cJSON_IsNumber(item)) return (double)NAN;
    return item->valuedouble;
}

/* ---- print ---- */

static unsigned char *ensure(printbuffer * const p, size_t needed) {
    unsigned char *newbuffer;
    size_t newsize;
    if (!p || !needed) return NULL;
    if (p->offset + needed <= p->length) return p->buffer + p->offset;
    if (p->noalloc) return NULL;
    newsize = p->length * 2;
    if (newsize <= p->offset + needed) newsize = p->offset + needed + 64;
    if (!(newbuffer = (unsigned char*)global_hooks.reallocate(p->buffer, newsize))) {
        global_hooks.deallocate(p->buffer);
        p->length = 0; p->buffer = NULL;
        return NULL;
    }
    p->length = newsize;
    p->buffer = newbuffer;
    return newbuffer + p->offset;
}

static int print_string_ptr(const unsigned char *str, printbuffer * const p) {
    const unsigned char *ptr;
    unsigned char *out;
    size_t len = 0, flag = 0;
    for (ptr = str; *ptr; ptr++) flag |= ((*ptr > 0 && *ptr < 32) || *ptr == '\"' || *ptr == '\\') ? 1 : 0;

    if (!flag) {
        len = (size_t)(ptr - str);
        out = ensure(p, len + 3);
        if (!out) return 0;
        out[0] = '\"';
        memcpy(out + 1, str, len);
        out[len + 1] = '\"';
        out[len + 2] = '\0';
        p->offset += len + 2;
        return 1;
    }
    /* with escapes */
    char escape_buf[8];
    out = ensure(p, str ? strlen((const char*)str) * 6 + 3 : 3);
    if (!out) return 0;
    ptr = out + 1;
    *out++ = '\"';
    for (; *str; str++) {
        if (*str == '\"' || *str == '\\' || *str == '/') { *out++ = '\\'; *out++ = *str; }
        else if (*str == '\b') { *out++ = '\\'; *out++ = 'b'; }
        else if (*str == '\f') { *out++ = '\\'; *out++ = 'f'; }
        else if (*str == '\n') { *out++ = '\\'; *out++ = 'n'; }
        else if (*str == '\r') { *out++ = '\\'; *out++ = 'r'; }
        else if (*str == '\t') { *out++ = '\\'; *out++ = 't'; }
        else if (*str < 32) { sprintf(escape_buf, "\\u%04x", *str); memcpy(out, escape_buf, 6); out += 6; }
        else *out++ = *str;
    }
    *out++ = '\"';
    *out = '\0';
    p->offset += (size_t)(out - (unsigned char*)p->buffer + p->offset);
    return 1;
}

static int print_value(const cJSON * const item, printbuffer * const p);
static int print_array(const cJSON * const item, printbuffer * const p);
static int print_object(const cJSON * const item, printbuffer * const p);

static int print_number(const cJSON * const item, printbuffer * const p) {
    unsigned char *out;
    double d = item->valuedouble;
    char buf[64];
    if (d != d) sprintf(buf, "null");
    else if (d * 0 != 0) sprintf(buf, "null");
    else if (item->valueint == item->valuedouble) sprintf(buf, "%d", item->valueint);
    else snprintf(buf, sizeof(buf), "%.17g", d);
    size_t len = strlen(buf);
    out = ensure(p, len + 1);
    if (!out) return 0;
    memcpy(out, buf, len + 1);
    p->offset += len;
    return 1;
}

static int print_string(const cJSON * const item, printbuffer * const p) {
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

static int print_array(const cJSON * const item, printbuffer * const p) {
    unsigned char *out;
    out = ensure(p, 1);
    if (!out) return 0;
    *out = '[';
    p->offset++;

    cJSON *child = item->child;
    while (child) {
        if (!print_value(child, p)) return 0;
        if (child->next) {
            out = ensure(p, p->format ? 2 : 1);
            if (!out) return 0;
            *out++ = ',';
            if (p->format) *out++ = ' ';
            p->offset += p->format ? 2 : 1;
        }
        child = child->next;
    }

    out = ensure(p, 2);
    if (!out) return 0;
    *out++ = ']';
    *out = '\0';
    p->offset++;
    return 1;
}

static int print_object(const cJSON * const item, printbuffer * const p) {
    unsigned char *out;
    size_t indent = p->depth;

    out = ensure(p, p->format ? (indent * 2 + 2) : 2);
    if (!out) return 0;
    *out++ = '{';
    if (p->format) { *out++ = '\n'; p->offset += 2; } else p->offset++;

    p->depth++;
    cJSON *child = item->child;
    while (child) {
        if (p->format) {
            out = ensure(p, p->depth * 2 + 1);
            if (!out) return 0;
            memset(out, ' ', p->depth * 2);
            p->offset += p->depth * 2;
        }
        if (!print_string_ptr((unsigned char*)child->string, p)) return 0;
        out = ensure(p, p->format ? 3 : 2);
        if (!out) return 0;
        *out++ = ':';
        if (p->format) *out++ = ' ';
        p->offset += p->format ? 2 : 1;
        if (!print_value(child, p)) return 0;
        if (child->next) {
            out = ensure(p, 2);
            if (!out) return 0;
            *out++ = ',';
            if (p->format) *out++ = '\n';
            p->offset += p->format ? 2 : 1;
        } else if (p->format) {
            out = ensure(p, 2);
            if (!out) return 0;
            *out++ = '\n';
            p->offset++;
        }
        child = child->next;
    }
    p->depth--;

    if (p->format) {
        out = ensure(p, indent * 2 + 2);
        if (!out) return 0;
        memset(out, ' ', indent * 2);
        p->offset += indent * 2;
    }
    out = ensure(p, 2);
    if (!out) return 0;
    *out++ = '}';
    *out = '\0';
    p->offset++;
    return 1;
}

static int print_value(const cJSON * const item, printbuffer * const p) {
    unsigned char *out;
    if (!item || !p) return 0;
    switch (item->type & 0xFF) {
        case cJSON_NULL:   { out = ensure(p, 5); if (out) { memcpy(out, "null", 5); p->offset += 4; } return out ? 1 : 0; }
        case cJSON_False:  { out = ensure(p, 6); if (out) { memcpy(out, "false", 6); p->offset += 5; } return out ? 1 : 0; }
        case cJSON_True:   { out = ensure(p, 5); if (out) { memcpy(out, "true", 5); p->offset += 4; } return out ? 1 : 0; }
        case cJSON_Number: return print_number(item, p);
        case cJSON_Raw:    { size_t l = strlen(item->valuestring); out = ensure(p, l + 1); if (out) { memcpy(out, item->valuestring, l+1); p->offset += l; } return out ? 1 : 0; }
        case cJSON_String: return print_string(item, p);
        case cJSON_Array:  return print_array(item, p);
        case cJSON_Object: return print_object(item, p);
        default: return 0;
    }
}

char *cJSON_Print(const cJSON *item) {
    printbuffer p = { NULL, 0, 0, 0, 0, 1 };
    p.buffer = (unsigned char*)global_hooks.allocate(256);
    if (!p.buffer) return NULL;
    p.length = 256;
    if (!print_value(item, &p)) { global_hooks.deallocate(p.buffer); return NULL; }
    return (char*)p.buffer;
}

char *cJSON_PrintUnformatted(const cJSON *item) {
    printbuffer p = { NULL, 0, 0, 0, 0, 0 };
    p.buffer = (unsigned char*)global_hooks.allocate(256);
    if (!p.buffer) return NULL;
    p.length = 256;
    if (!print_value(item, &p)) { global_hooks.deallocate(p.buffer); return NULL; }
    return (char*)p.buffer;
}

char *cJSON_PrintBuffered(const cJSON *item, int prebuffer, int fmt) {
    printbuffer p = { NULL, 0, 0, 0, 0, fmt };
    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer) return NULL;
    p.length = (size_t)prebuffer;
    if (!print_value(item, &p)) { global_hooks.deallocate(p.buffer); return NULL; }
    return (char*)p.buffer;
}

/* ---- array/object access ---- */
int cJSON_GetArraySize(const cJSON *array) {
    cJSON *c = array ? array->child : NULL;
    size_t i = 0;
    while (c) { i++; c = c->next; }
    return (int)i;
}

cJSON *cJSON_GetArrayItem(const cJSON *array, int index) {
    cJSON *c = array ? array->child : NULL;
    while (c && index-- > 0) c = c->next;
    return c;
}

static cJSON *get_object_item(const cJSON * const object, const char * const name, int case_sensitive) {
    cJSON *c = object ? object->child : NULL;
    while (c) {
        if (c->string && (case_sensitive ? strcmp(c->string, name) : strcasecmp(c->string, name)) == 0)
            return c;
        c = c->next;
    }
    return NULL;
}

cJSON *cJSON_GetObjectItem(const cJSON *obj, const char *str) { return get_object_item(obj, str, 0); }
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *obj, const char *str) { return get_object_item(obj, str, 1); }
int    cJSON_HasObjectItem(const cJSON *obj, const char *str) { return cJSON_GetObjectItem(obj, str) ? 1 : 0; }

/* ---- create ---- */
static cJSON *cJSON_New(int type) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item) item->type = type;
    return item;
}

cJSON *cJSON_CreateNull(void)   { return cJSON_New(cJSON_NULL); }
cJSON *cJSON_CreateTrue(void)   { cJSON *i = cJSON_New(cJSON_True);  if (i) i->valueint = 1; return i; }
cJSON *cJSON_CreateFalse(void)  { return cJSON_New(cJSON_False); }
cJSON *cJSON_CreateBool(int b)  { return b ? cJSON_CreateTrue() : cJSON_CreateFalse(); }

cJSON *cJSON_CreateNumber(double num) {
    cJSON *i = cJSON_New(cJSON_Number);
    if (i) { i->valuedouble = num; i->valueint = (int)num; }
    return i;
}

cJSON *cJSON_CreateString(const char *s) {
    cJSON *i = cJSON_New(cJSON_String);
    if (i) {
        i->valuestring = cJSON_strdup(s, &global_hooks);
        if (!i->valuestring) { cJSON_Delete(i); return NULL; }
    }
    return i;
}

cJSON *cJSON_CreateRaw(const char *raw) {
    cJSON *i = cJSON_New(cJSON_Raw);
    if (i) {
        i->valuestring = cJSON_strdup(raw, &global_hooks);
        if (!i->valuestring) { cJSON_Delete(i); return NULL; }
    }
    return i;
}

cJSON *cJSON_CreateArray(void)  { return cJSON_New(cJSON_Array); }
cJSON *cJSON_CreateObject(void) { return cJSON_New(cJSON_Object); }

cJSON *cJSON_CreateStringReference(const char *s) {
    cJSON *i = cJSON_New(cJSON_String | cJSON_IsReference);
    if (i) i->valuestring = (char*)s;
    return i;
}

cJSON *cJSON_CreateObjectReference(const cJSON *child) {
    cJSON *i = cJSON_New(cJSON_Object | cJSON_IsReference);
    if (i) i->child = (cJSON*)child;
    return i;
}

cJSON *cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *i = cJSON_New(cJSON_Array | cJSON_IsReference);
    if (i) i->child = (cJSON*)child;
    return i;
}

cJSON *cJSON_CreateIntArray(const int *nums, int count) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < count; i++) {
        cJSON *n = cJSON_CreateNumber(nums[i]);
        if (!n || !cJSON_AddItemToArray(a, n)) { cJSON_Delete(a); return NULL; }
    }
    return a;
}

cJSON *cJSON_CreateFloatArray(const float *nums, int count) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < count; i++) {
        cJSON *n = cJSON_CreateNumber((double)nums[i]);
        if (!n || !cJSON_AddItemToArray(a, n)) { cJSON_Delete(a); return NULL; }
    }
    return a;
}

cJSON *cJSON_CreateDoubleArray(const double *nums, int count) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < count; i++) {
        cJSON *n = cJSON_CreateNumber(nums[i]);
        if (!n || !cJSON_AddItemToArray(a, n)) { cJSON_Delete(a); return NULL; }
    }
    return a;
}

cJSON *cJSON_CreateStringArray(const char **strs, int count) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; a && i < count; i++) {
        cJSON *s = cJSON_CreateString(strs[i]);
        if (!s || !cJSON_AddItemToArray(a, s)) { cJSON_Delete(a); return NULL; }
    }
    return a;
}

/* ---- add/detach ---- */
static void suffix_object(cJSON *prev, cJSON *item) {
    prev->next = item;
    item->prev = prev;
}

static cJSON *create_reference(const cJSON *item) {
    cJSON *ref = cJSON_New_Item(&global_hooks);
    if (!ref) return NULL;
    memcpy(ref, item, sizeof(*item));
    ref->string = NULL;
    ref->type |= cJSON_IsReference;
    ref->next = ref->prev = NULL;
    return ref;
}

static int add_item_to_array(cJSON *array, cJSON *item) {
    cJSON *child;
    if (!item || !array || array == item) return 0;
    child = array->child;
    if (!child) { array->child = item; item->prev = item; item->next = NULL; }
    else {
        while (child->next) child = child->next;
        suffix_object(child, item);
        array->child->prev = item;
    }
    return 1;
}

int cJSON_AddItemToArray(cJSON *array, cJSON *item) { return add_item_to_array(array, item); }

static int add_item_to_object(cJSON *object, const char *string, cJSON *item, int constant_key) {
    char *new_key;
    int new_type;
    if (!object || !string || !item || object == item) return 0;
    if (constant_key) {
        new_key = (char*)string;
        new_type = item->type | cJSON_StringIsConst;
    } else {
        new_key = cJSON_strdup(string, &global_hooks);
        if (!new_key) return 0;
        new_type = item->type & ~cJSON_StringIsConst;
    }
    if (!(item->type & cJSON_StringIsConst) && item->string)
        global_hooks.deallocate(item->string);
    item->string = new_key;
    item->type = new_type;
    return add_item_to_array(object, item);
}

int cJSON_AddItemToObject(cJSON *obj, const char *str, cJSON *item) { return add_item_to_object(obj, str, item, 0); }
int cJSON_AddItemToObjectCS(cJSON *obj, const char *str, cJSON *item) { return add_item_to_object(obj, str, item, 1); }
int cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item) { return add_item_to_array(array, create_reference(item)); }
int cJSON_AddItemReferenceToObject(cJSON *obj, const char *str, cJSON *item) {
    return add_item_to_object(obj, str, create_reference(item), 0);
}

static cJSON *detach_item_via_pointer(cJSON *parent, cJSON * const item) {
    if (!parent || !item) return NULL;
    if (item != parent->child) {
        item->prev->next = item->next;
    } else {
        parent->child = item->next;
    }
    if (item->next) {
        item->next->prev = item->prev;
    }
    if (item == parent->child ? 0 : 1) {
        if (item->next == NULL && parent->child) parent->child->prev = item->prev;
    }
    item->next = item->prev = NULL;
    return item;
}

cJSON *cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item) { return detach_item_via_pointer(parent, item); }

cJSON *cJSON_DetachItemFromArray(cJSON *array, int which) {
    return detach_item_via_pointer(array, cJSON_GetArrayItem(array, which));
}

void cJSON_DeleteItemFromArray(cJSON *array, int which) { cJSON_Delete(cJSON_DetachItemFromArray(array, which)); }

cJSON *cJSON_DetachItemFromObject(cJSON *obj, const char *str) {
    return detach_item_via_pointer(obj, get_object_item(obj, str, 0));
}

cJSON *cJSON_DetachItemFromObjectCaseSensitive(cJSON *obj, const char *str) {
    return detach_item_via_pointer(obj, get_object_item(obj, str, 1));
}

void cJSON_DeleteItemFromObject(cJSON *obj, const char *str) { cJSON_Delete(cJSON_DetachItemFromObject(obj, str)); }
void cJSON_DeleteItemFromObjectCaseSensitive(cJSON *obj, const char *str) { cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(obj, str)); }

int cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem) {
    cJSON *after = cJSON_GetArrayItem(array, which);
    if (!after) return add_item_to_array(array, newitem);
    newitem->next = after;
    newitem->prev = after->prev;
    after->prev = newitem;
    if (after == array->child) array->child = newitem;
    else newitem->prev->next = newitem;
    return 1;
}

int cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON *replacement) {
    if (!parent || !item || !replacement) return 0;
    if (replacement == item) return 1;
    replacement->next = item->next;
    replacement->prev = item->prev;
    if (replacement->next) replacement->next->prev = replacement;
    if (parent->child == item) parent->child = replacement;
    else replacement->prev->next = replacement;
    item->next = item->prev = NULL;
    cJSON_Delete(item);
    return 1;
}

int cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem) {
    return cJSON_ReplaceItemViaPointer(array, cJSON_GetArrayItem(array, which), newitem);
}

static int replace_item_in_object(cJSON *obj, const char *str, cJSON *newitem, int cs) {
    if (!newitem || !str) return 0;
    if (!(newitem->type & cJSON_StringIsConst) && newitem->string) global_hooks.deallocate(newitem->string);
    newitem->string = cJSON_strdup(str, &global_hooks);
    if (!newitem->string) return 0;
    newitem->type &= ~cJSON_StringIsConst;
    return cJSON_ReplaceItemViaPointer(obj, get_object_item(obj, str, cs), newitem);
}

int cJSON_ReplaceItemInObject(cJSON *obj, const char *str, cJSON *item) { return replace_item_in_object(obj, str, item, 0); }
int cJSON_ReplaceItemInObjectCaseSensitive(cJSON *obj, const char *str, cJSON *item) { return replace_item_in_object(obj, str, item, 1); }

cJSON *cJSON_Duplicate(const cJSON *item, int recurse) {
    cJSON *newitem, *child, *next, *newchild;
    if (!item) return NULL;
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem) return NULL;
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring) {
        newitem->valuestring = cJSON_strdup(item->valuestring, &global_hooks);
        if (!newitem->valuestring) { cJSON_Delete(newitem); return NULL; }
    }
    if (item->string) {
        newitem->string = (item->type & cJSON_StringIsConst) ? item->string : cJSON_strdup(item->string, &global_hooks);
        if (!newitem->string) { cJSON_Delete(newitem); return NULL; }
    }
    if (!recurse) return newitem;
    child = item->child;
    while (child) {
        newchild = cJSON_Duplicate(child, 1);
        if (!newchild) { cJSON_Delete(newitem); return NULL; }
        if (next) { next->next = newchild; newchild->prev = next; next = newchild; }
        else { newitem->child = newchild; next = newchild; }
        child = child->next;
    }
    if (newitem->child) newitem->child->prev = next;
    return newitem;
}

int cJSON_Compare(const cJSON * const a, const cJSON * const b, const int cs) {
    if (!a || !b || (a->type & 0xFF) != (b->type & 0xFF)) return 0;
    switch (a->type & 0xFF) {
        case cJSON_NULL:
        case cJSON_True:
        case cJSON_False: return 1;
        case cJSON_Number: return (fabs(a->valuedouble - b->valuedouble) <= DBL_EPSILON);
        case cJSON_String:
        case cJSON_Raw:
            return (cs ? strcmp(a->valuestring, b->valuestring) : strcasecmp(a->valuestring, b->valuestring)) == 0;
        default: return 0;
    }
}

void cJSON_Minify(char *json) {
    unsigned char *into = (unsigned char*)json;
    if (!json) return;
    while (*json) {
        if (*json == ' ' || *json == '\t' || *json == '\r' || *json == '\n') json++;
        else if (*json == '/' && json[1] == '/') {
            while (*json && *json != '\n') json++;
        } else if (*json == '/' && json[1] == '*') {
            while (*json && !(*json == '*' && json[1] == '/')) json++;
            json += 2;
        } else if (*json == '\"') {
            *into++ = (unsigned char)*json++;
            while (*json && *json != '\"') {
                if (*json == '\\') *into++ = (unsigned char)*json++;
                *into++ = (unsigned char)*json++;
            }
            *into++ = (unsigned char)*json++;
        } else {
            *into++ = (unsigned char)*json++;
        }
    }
    *into = '\0';
}
