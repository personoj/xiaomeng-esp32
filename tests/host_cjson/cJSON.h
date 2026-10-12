/* Host-test declarations for the public cJSON 1.x ABI, not a JSON implementation.
 * Prefer the dependency's own header when available. This small declaration-only
 * fallback lets offline tests use an already-installed libcjson runtime without
 * downloading or vendoring a parser. No firmware build includes this directory.
 */
#ifndef XY_HOST_CJSON_H
#define XY_HOST_CJSON_H
#include <stddef.h>
typedef int cJSON_bool;
typedef struct cJSON {
    struct cJSON *next, *prev, *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;
const char *cJSON_Version(void);
cJSON *cJSON_ParseWithLength(const char *value, size_t buffer_length);
void cJSON_Delete(cJSON *item);
cJSON *cJSON_CreateObject(void);
cJSON *cJSON_CreateArray(void);
cJSON *cJSON_CreateString(const char *string);
cJSON *cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
char *cJSON_GetStringValue(const cJSON *item);
double cJSON_GetNumberValue(const cJSON *item);
cJSON_bool cJSON_IsString(const cJSON *item);
cJSON_bool cJSON_IsTrue(const cJSON *item);
cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item);
cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name);
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, cJSON_bool boolean);
#define cJSON_ArrayForEach(element, array) \
    for ((element) = (array) != NULL ? (array)->child : NULL; \
         (element) != NULL; (element) = (element)->next)
#endif
