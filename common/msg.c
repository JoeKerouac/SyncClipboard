#include "msg.h"
#include <stdlib.h>

char *sc_msg_build(const char *type, cJSON *body) {
    if (!type) {
        if (body) cJSON_Delete(body);
        return NULL;
    }
    cJSON *root = body ? body : cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "v", 2);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *sc_msg_clipboard(const char *encrypted_b64) {
    if (!encrypted_b64) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "content", encrypted_b64);
    return sc_msg_build("clipboard", o);
}

char *sc_msg_hello(const char *client_type, const char *device_id, const char *app_version) {
    cJSON *o = cJSON_CreateObject();
    if (client_type) cJSON_AddStringToObject(o, "clientType", client_type);
    if (device_id)   cJSON_AddStringToObject(o, "deviceId", device_id);
    if (app_version) cJSON_AddStringToObject(o, "appVersion", app_version);
    cJSON *caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("clipboard"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("file_lan"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("file_nat"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("file_relay"));
    cJSON_AddItemToObject(o, "capabilities", caps);
    return sc_msg_build("hello", o);
}
