/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  SPDX-License-Identifier: MIT
*/
#ifndef CJSON_H
#define CJSON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define cJSON_Invalid  (0)
#define cJSON_False    (1 << 0)
#define cJSON_True     (1 << 1)
#define cJSON_NULL     (1 << 2)
#define cJSON_Number   (1 << 3)
#define cJSON_String   (1 << 4)
#define cJSON_Array    (1 << 5)
#define cJSON_Object   (1 << 6)
#define cJSON_Raw      (1 << 7)
#define cJSON_IsReference    256
#define cJSON_StringIsConst  512

typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

typedef struct cJSON_Hooks {
    void *(*malloc_fn)(size_t sz);
    void  (*free_fn)(void *ptr);
} cJSON_Hooks;

#define cJSON_IsInvalid(item) (((item) == NULL) || ((item)->type & 0xFF) == cJSON_Invalid)
#define cJSON_IsFalse(item)   (((item) != NULL) && ((item)->type & 0xFF) == cJSON_False)
#define cJSON_IsTrue(item)    (((item) != NULL) && ((item)->type & 0xFF) == cJSON_True)
#define cJSON_IsBool(item)    (((item) != NULL) && (((item)->type & 0xFF) == cJSON_True || ((item)->type & 0xFF) == cJSON_False))
#define cJSON_IsNull(item)    (((item) != NULL) && ((item)->type & 0xFF) == cJSON_NULL)
#define cJSON_IsNumber(item)  (((item) != NULL) && ((item)->type & 0xFF) == cJSON_Number)
#define cJSON_IsString(item)  (((item) != NULL) && ((item)->type & 0xFF) == cJSON_String)
#define cJSON_IsArray(item)   (((item) != NULL) && ((item)->type & 0xFF) == cJSON_Array)
#define cJSON_IsObject(item)  (((item) != NULL) && ((item)->type & 0xFF) == cJSON_Object)
#define cJSON_IsRaw(item)     (((item) != NULL) && ((item)->type & 0xFF) == cJSON_Raw)

void        cJSON_InitHooks(cJSON_Hooks *hooks);
cJSON      *cJSON_Parse(const char *value);
cJSON      *cJSON_ParseWithLength(const char *value, size_t buffer_length);
char       *cJSON_Print(const cJSON *item);
char       *cJSON_PrintUnformatted(const cJSON *item);
char       *cJSON_PrintBuffered(const cJSON *item, int prebuffer, int fmt);
void        cJSON_Delete(cJSON *item);

int         cJSON_GetArraySize(const cJSON *array);
cJSON      *cJSON_GetArrayItem(const cJSON *array, int index);
cJSON      *cJSON_GetObjectItem(const cJSON *object, const char *string);
cJSON      *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
int         cJSON_HasObjectItem(const cJSON *object, const char *string);

const char *cJSON_GetErrorPtr(void);
char       *cJSON_GetStringValue(const cJSON *item);
double      cJSON_GetNumberValue(const cJSON *item);

cJSON *cJSON_CreateNull(void);
cJSON *cJSON_CreateTrue(void);
cJSON *cJSON_CreateFalse(void);
cJSON *cJSON_CreateBool(int boolean);
cJSON *cJSON_CreateNumber(double num);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_CreateRaw(const char *raw);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateStringReference(const char *string);
cJSON *cJSON_CreateObjectReference(const cJSON *child);
cJSON *cJSON_CreateArrayReference(const cJSON *child);
cJSON *cJSON_CreateIntArray(const int *numbers, int count);
cJSON *cJSON_CreateFloatArray(const float *numbers, int count);
cJSON *cJSON_CreateDoubleArray(const double *numbers, int count);
cJSON *cJSON_CreateStringArray(const char **strings, int count);

int   cJSON_AddItemToArray(cJSON *array, cJSON *item);
int   cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
int   cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
int   cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
int   cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

cJSON *cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item);
cJSON *cJSON_DetachItemFromArray(cJSON *array, int which);
void  cJSON_DeleteItemFromArray(cJSON *array, int which);
cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string);
cJSON *cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
void  cJSON_DeleteItemFromObject(cJSON *object, const char *string);
void  cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

int   cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);
int   cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON *replacement);
int   cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
int   cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);
int   cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem);

cJSON *cJSON_Duplicate(const cJSON *item, int recurse);
int    cJSON_Compare(const cJSON * const a, const cJSON * const b, const int case_sensitive);
void   cJSON_Minify(char *json);

#define cJSON_AddNullToObject(object,name)      cJSON_AddItemToObject(object, name, cJSON_CreateNull())
#define cJSON_AddTrueToObject(object,name)      cJSON_AddItemToObject(object, name, cJSON_CreateTrue())
#define cJSON_AddFalseToObject(object,name)     cJSON_AddItemToObject(object, name, cJSON_CreateFalse())
#define cJSON_AddBoolToObject(object,name,b)    cJSON_AddItemToObject(object, name, cJSON_CreateBool(b))
#define cJSON_AddNumberToObject(object,name,n)  cJSON_AddItemToObject(object, name, cJSON_CreateNumber(n))
#define cJSON_AddStringToObject(object,name,s)  cJSON_AddItemToObject(object, name, cJSON_CreateString(s))
#define cJSON_AddRawToObject(object,name,s)     cJSON_AddItemToObject(object, name, cJSON_CreateRaw(s))

#define cJSON_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number), (object)->valueint : 0)
#define cJSON_SetNumberValue(object, number) ((object) ? (object)->valuedouble = (number), (object)->valueint = (int)(number), (number) : 0)

#define cJSON_ArrayForEach(element, array) \
    for (element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

void *cJSON_malloc(size_t size);
void  cJSON_free(void *object);

#ifdef __cplusplus
}
#endif

#endif /* CJSON_H */
