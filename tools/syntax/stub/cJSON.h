#pragma once
#include <cstddef>
typedef struct cJSON { struct cJSON *next; char *valuestring; int valueint; double valuedouble; } cJSON;
extern "C" {
cJSON *cJSON_ParseWithLength(const char*, size_t);
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON*, const char*);
int cJSON_IsString(const cJSON*); int cJSON_IsNumber(const cJSON*);
int cJSON_IsTrue(const cJSON*);   int cJSON_IsArray(const cJSON*);
void cJSON_Delete(cJSON*);
}
#define cJSON_ArrayForEach(e,a) for((e)=(a)?(a)->next:0;(e);(e)=(e)->next)
