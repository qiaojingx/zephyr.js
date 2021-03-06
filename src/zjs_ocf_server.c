// Copyright (c) 2016-2017, Intel Corporation.

#ifdef BUILD_MODULE_OCF

#include "oc_api.h"
#include "oc_core_res.h"
#include "port/oc_clock.h"
//#include "port/oc_signal_main_loop.h"

#include "zjs_util.h"
#include "zjs_common.h"

#include "zjs_ocf_server.h"
#include "zjs_ocf_encoder.h"
#include "zjs_ocf_common.h"

#include "zjs_event.h"

struct server_resource {
    /*
     * TODO: cant reference with 'this' because iotivity-constrained callbacks
     *       which trigger events need this. They are not JS API's that have
     *       'this' pointer so we have to save it in C.
     */
    jerry_value_t object;
    u32_t error_code;
    oc_resource_t *res;
    char *device_id;
    char *resource_path;
    char **resource_types;
    char **resource_ifaces;
    u8_t num_types;
    u8_t num_ifaces;
    u8_t flags;
};

typedef struct resource_list {
    struct server_resource *resource;
    struct resource_list *next;
} resource_list_t;

struct ocf_response {
    oc_method_t method;         // Current method being executed
    oc_request_t *request;
    struct server_resource *res;
};

struct ocf_handler {
    oc_request_t *req;
    struct ocf_response *resp;
    struct server_resource *res;
};

static const jerry_object_native_info_t ocf_type_info =
{
   .free_cb = free_handle_nop
};

static resource_list_t *res_list = NULL;

#define FLAG_OBSERVE        1 << 0
#define FLAG_DISCOVERABLE   1 << 1
#define FLAG_SLOW           1 << 2
#define FLAG_SECURE         1 << 3

static struct ocf_handler *new_ocf_handler(struct server_resource *res)
{
    struct ocf_handler *h = zjs_malloc(sizeof(struct ocf_handler));
    if (!h) {
        ERR_PRINT("could not allocate OCF handle, out of memory\n");
        return NULL;
    }
    memset(h, 0, sizeof(struct ocf_handler));
    h->res = res;

    return h;
}

static jerry_value_t make_ocf_error(const char *name, const char *msg,
                                    struct server_resource *res)
{
    if (res) {
        ZVAL ret = jerry_create_object();
        if (name) {
            zjs_obj_add_string(ret, name, "name");
        } else {
            ERR_PRINT("error must have a name\n");
            return ZJS_UNDEFINED;
        }
        if (msg) {
            zjs_obj_add_string(ret, msg, "message");
        } else {
            ERR_PRINT("error must have a message\n");
            return ZJS_UNDEFINED;
        }
        if (res->device_id) {
            zjs_obj_add_string(ret, res->device_id, "deviceId");
        }
        if (res->resource_path) {
            zjs_obj_add_string(ret, res->resource_path, "resourcePath");
        }
        zjs_obj_add_number(ret, (double)res->error_code, "errorCode");

        return jerry_acquire_value(ret);
    } else {
        ERR_PRINT("client resource was NULL\n");
        return ZJS_UNDEFINED;
    }
}

static jerry_value_t request_to_jerry_value(oc_request_t *request)
{
    jerry_value_t props = jerry_create_object();
    oc_rep_t *rep = request->request_payload;
    while (rep != NULL) {
        switch (rep->type) {
        case BOOL:
            zjs_obj_add_boolean(props, rep->value.boolean, oc_string(rep->name));
            break;
        case INT:
            zjs_obj_add_number(props, rep->value.integer, oc_string(rep->name));
            break;
        case BYTE_STRING:
        case STRING:
            zjs_obj_add_string(props, oc_string(rep->value.string), oc_string(rep->name));
            break;
            /*
             * TODO: Implement encoding for complex types
             */
        case STRING_ARRAY:
        case OBJECT:
            ZJS_PRINT("{ Object }\n");
            break;
        default:
            break;
        }
        rep = rep->next;
    }
    return props;
}

struct server_resource *new_server_resource(char *path)
{
    struct server_resource *resource = zjs_malloc(sizeof(struct server_resource));
    if (!resource) {
        ERR_PRINT("new resource could not be allocated\n");
        return NULL;
    }
    memset(resource, 0, sizeof(struct server_resource));

    u32_t len = strlen(path);
    resource->resource_path = zjs_malloc(len + 1);
    if (!resource->resource_path) {
        ERR_PRINT("resource path could not be allocated\n");
        zjs_free(resource);
        return NULL;
    }
    strncpy(resource->resource_path, path, len + 1);

    return resource;
}

static struct ocf_response *create_response(struct server_resource *resource, oc_method_t method)
{
    struct ocf_response *resp = zjs_malloc(sizeof(struct ocf_response));
    memset(resp, 0, sizeof(struct ocf_response));
    resp->res = resource;
    resp->method = method;

    return resp;
}

static jerry_value_t create_resource(const char *path, jerry_value_t resource_init)
{
    jerry_value_t res = jerry_create_object();

    if (path) {
        zjs_obj_add_string(res, path, "resourcePath");
    }

    ZVAL properties = zjs_get_property(resource_init, "properties");
    zjs_set_property(res, "properties", properties);

    DBG_PRINT("path=%s, obj number=%lu\n", path, res);

    return res;
}

#if 0
static void print_props_data(oc_request_t *data)
{
    int i;
    oc_rep_t *rep = data->request_payload;
    while (rep != NULL) {
        ZJS_PRINT("Type: %u, Key: %s, Value: ", rep->type, oc_string(rep->name));
        switch (rep->type) {
        case BOOL:
            ZJS_PRINT("%d\n", rep->value_boolean);
            break;
        case INT:
            ZJS_PRINT("%ld\n", (s32_t)rep->value_int);
            break;
        case BYTE_STRING:
        case STRING:
            ZJS_PRINT("%s\n", oc_string(rep->value_string));
            break;
        case STRING_ARRAY:
            ZJS_PRINT("[ ");
            for (i = 0; i < oc_string_array_get_allocated_size(rep->value_array); i++) {
                ZJS_PRINT("%s ", oc_string_array_get_item(rep->value_array, i));
            }
            ZJS_PRINT("]\n");
            break;
        case OBJECT:
            ZJS_PRINT("{ Object }\n");
            break;
        default:
            break;
        }
        rep = rep->next;
    }
}
#endif

static ZJS_DECL_FUNC(ocf_respond)
{
    // args: properties object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    jerry_value_t data = argv[0];

    ZJS_GET_HANDLE(this, struct ocf_handler, h, ocf_type_info);

    void *ret;
    // Start the root encoding object
    zjs_rep_start_root_object();
    // Encode all properties from resource (argv[0])
    ret = zjs_ocf_props_setup(data, &g_encoder, true);
    zjs_rep_end_root_object();
    // Free property return handle
    zjs_ocf_free_props(ret);

    /*
     * TODO: Better error handling here. We need to implement a respond
     *       with error as well as checking the return code of previous
     *       OCF calls made.
     */
    if (h->resp->method == OC_PUT || h->resp->method == OC_POST) {
        oc_send_response(h->req, OC_STATUS_CHANGED);
    } else {
        oc_send_response(h->req, OC_STATUS_OK);
    }
    DBG_PRINT("responding to method type=%u, properties=%lu\n", h->resp->method, data);

    jerry_value_t promise = jerry_create_promise();

    jerry_resolve_or_reject_promise(promise, ZJS_UNDEFINED, true);

    return promise;
}

static jerry_value_t create_request(struct server_resource *resource,
                                    oc_method_t method,
                                    struct ocf_handler *handler)
{
    handler->resp = create_response(resource, method);

    jerry_value_t object = jerry_create_object();
    ZVAL source = jerry_create_object();
    ZVAL target = jerry_create_object();

    if (resource->res) {
        zjs_obj_add_string(source, oc_string(resource->res->uri),
                           "resourcePath");
        zjs_obj_add_string(target, oc_string(resource->res->uri),
                           "resourcePath");
    }

    // source is the resource requesting the operation
    zjs_set_property(object, "source", source);

    // target is the resource being retrieved
    zjs_set_property(object, "target", target);

    zjs_obj_add_function(object, ocf_respond, "respond");

    jerry_set_object_native_pointer(object, handler, &ocf_type_info);

    return object;
}

static void post_get(void *handler)
{
    // ZJS_PRINT("POST GET\n");
}

static void ocf_get_handler(oc_request_t *request,
                            oc_interface_mask_t interface, void *user_data)
{
    ZJS_PRINT("ocf_get_handler()\n");
    struct ocf_handler *h =
        new_ocf_handler((struct server_resource *)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }

    h->req = request;

    ZVAL request_val = create_request(h->res, OC_GET, h);
    ZVAL flag = jerry_create_boolean(0);

    jerry_value_t argv[2] = {request_val, flag};
    zjs_trigger_event_now(h->res->object, "retrieve", argv, 2, post_get, h);

    zjs_free(h->resp);
    zjs_free(h);
}

static void post_put(void *handler)
{
    // ZJS_PRINT("POST PUT\n");
}

static void ocf_put_handler(oc_request_t *request,
                            oc_interface_mask_t interface, void *user_data)
{
    ZJS_PRINT("ocf_put_handler()\n");
    struct ocf_handler *h = new_ocf_handler((struct server_resource *)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }

    ZVAL request_val = create_request(h->res, OC_PUT, h);
    ZVAL props_val = request_to_jerry_value(request);
    ZVAL resource_val = jerry_create_object();

    zjs_set_property(resource_val, "properties", props_val);
    zjs_set_property(request_val, "resource", resource_val);

    h->req = request;
    zjs_trigger_event_now(h->res->object, "update", &request_val, 1, post_put,
                          h);

    DBG_PRINT("sent PUT response, code=CHANGED\n");

    zjs_free(h->resp);
    zjs_free(h);
}

#ifdef ZJS_OCF_DELETE_SUPPORT
static void post_delete(void *handler)
{
    // ZJS_PRINT("POST DELETE\n");
}

static void ocf_delete_handler(oc_request_t *request, oc_interface_mask_t interface, void *user_data)
{
    struct ocf_handler *h = new_ocf_handler((struct server_resource *)user_data);
    if (!h) {
        ERR_PRINT("handler was NULL\n");
        return;
    }
    zjs_trigger_event_now(h->res->object, "delete", NULL, 0, post_delete, h);

    oc_send_response(request, OC_STATUS_DELETED);

    DBG_PRINT("sent DELETE response, code=OC_STATUS_DELETED\n");
}
#endif

/*
 * TODO: Get resource object and use it to notify
 */
static ZJS_DECL_FUNC(ocf_notify)
{
    // args: resource object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    ZJS_GET_HANDLE(this, struct server_resource, resource, ocf_type_info);

    DBG_PRINT("path=%s\n", resource->resource_path);

    oc_notify_observers(resource->res);

    return ZJS_UNDEFINED;
}

static ZJS_DECL_FUNC(ocf_register)
{
    // args: resource object
    ZJS_VALIDATE_ARGS(Z_OBJECT);

    struct server_resource *resource;
    int i;

    // Required
    ZVAL resource_path_val = zjs_get_property(argv[0], "resourcePath");
    if (!jerry_value_is_string(resource_path_val)) {
        ERR_PRINT("resourcePath not found\n");
        REJECT("TypeMismatchError", "resourcePath not found");
    }
    ZJS_GET_STRING(resource_path_val, resource_path, OCF_MAX_RES_PATH_LEN);

    ZVAL res_type_array = zjs_get_property(argv[0], "resourceTypes");
    if (!jerry_value_is_array(res_type_array)) {
        ERR_PRINT("resourceTypes array not found\n");
        REJECT("TypeMismatchError", "resourceTypes array not found");
    }

    // Optional
    u8_t flags = 0;
    ZVAL observable_val = zjs_get_property(argv[0], "observable");
    if (jerry_value_is_boolean(observable_val)) {
        if (jerry_get_boolean_value(observable_val)) {
            flags |= FLAG_OBSERVE;
        }
    }

    ZVAL discoverable_val = zjs_get_property(argv[0], "discoverable");
    if (jerry_value_is_boolean(discoverable_val)) {
        if (jerry_get_boolean_value(discoverable_val)) {
            flags |= FLAG_DISCOVERABLE;
        }
    }

    ZVAL slow_val = zjs_get_property(argv[0], "slow");
    if (jerry_value_is_boolean(slow_val)) {
        if (jerry_get_boolean_value(slow_val)) {
            flags |= FLAG_SLOW;
        }
    }

    ZVAL secure_val = zjs_get_property(argv[0], "secure");
    if (jerry_value_is_boolean(secure_val)) {
        if (jerry_get_boolean_value(secure_val)) {
            flags |= FLAG_SECURE;
        }
    }

    resource_list_t *new = zjs_malloc(sizeof(resource_list_t));
    if (!new) {
        REJECT("InternalError", "Could not allocate resource list");
    }
    resource = new_server_resource(resource_path);
    if (!resource) {
        REJECT("InternalError", "Could not allocate resource");
    }
    new->resource = resource;
    new->next = res_list;
    res_list = new;

    resource->flags = flags;

    resource->num_types = jerry_get_array_length(res_type_array);
    resource->resource_types = zjs_malloc(sizeof(char *) * resource->num_types);
    if (!resource->resource_types) {
        REJECT("InternalError", "resourceType alloc failed");
    }

    for (i = 0; i < resource->num_types; ++i) {
        ZVAL type_val = jerry_get_property_by_index(res_type_array, i);
        jerry_size_t size = OCF_MAX_RES_TYPE_LEN;
        resource->resource_types[i] = zjs_alloc_from_jstring(type_val, &size);
        if (!resource->resource_types[i]) {
            REJECT("InternalError", "resourceType alloc failed");
        }
    }

    ZVAL iface_array = zjs_get_property(argv[0], "interfaces");
    if (!jerry_value_is_array(iface_array)) {
        ERR_PRINT("interfaces array not found\n");
        REJECT("TypeMismatchError", "resourceTypes array not found");
    }

    resource->num_ifaces = jerry_get_array_length(iface_array);
    resource->resource_ifaces = zjs_malloc(sizeof(char *) * resource->num_ifaces);
    if (!resource->resource_ifaces) {
        REJECT("InternalError", "interfaces alloc failed");
    }

    for (i = 0; i < resource->num_ifaces; ++i) {
        ZVAL val = jerry_get_property_by_index(iface_array, i);
        jerry_size_t size = OCF_MAX_RES_TYPE_LEN;
        resource->resource_ifaces[i] = zjs_alloc_from_jstring(val, &size);
        if (!resource->resource_ifaces[i]) {
            REJECT("InternalError", "resourceType alloc failed");
        }
    }

    jerry_value_t res = create_resource(resource_path, argv[0]);

    jerry_value_t promise = jerry_create_promise();
    jerry_resolve_or_reject_promise(promise, res, true);

    /*
     * TODO: Add native handle to ensure it gets freed,
     *       check if we can reference with 'this'
     */
    resource->object = this;

    jerry_set_object_native_pointer(res, resource, &ocf_type_info);

    DBG_PRINT("registered resource, path=%s\n", resource_path);

    return promise;
}

void zjs_ocf_register_resources(void)
{
    resource_list_t *cur = res_list;
    while (cur) {
        int i;
        struct server_resource *resource = cur->resource;

        resource->res = oc_new_resource(resource->resource_path, resource->num_types, 0);
        if (!resource->res) {
            ERR_PRINT("failed to create new resource\n");
            return;
        }
        for (i = 0; i < resource->num_types; ++i) {
            oc_resource_bind_resource_type(resource->res, resource->resource_types[i]);
        }
        for (i = 0; i < resource->num_ifaces; ++i) {
            if (strcmp(resource->resource_ifaces[i], "/oic/if/rw") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_RW);
                oc_resource_set_default_interface(resource->res, OC_IF_RW);
            } else if (strcmp(resource->resource_ifaces[0], "/oic/if/r") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_R);
                oc_resource_set_default_interface(resource->res, OC_IF_R);
            } else if (strcmp(resource->resource_ifaces[0], "/oic/if/a") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_A);
                oc_resource_set_default_interface(resource->res, OC_IF_A);
            } else if (strcmp(resource->resource_ifaces[0], "/oic/if/s") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_S);
                oc_resource_set_default_interface(resource->res, OC_IF_S);
            } else if (strcmp(resource->resource_ifaces[0], "/oic/if/b") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_B);
                oc_resource_set_default_interface(resource->res, OC_IF_B);
            } else if (strcmp(resource->resource_ifaces[0], "/oic/if/ll") == 0) {
                oc_resource_bind_resource_interface(resource->res, OC_IF_LL);
                oc_resource_set_default_interface(resource->res, OC_IF_LL);
            }
        }
        if (resource->flags & FLAG_DISCOVERABLE) {
            oc_resource_set_discoverable(resource->res, 1);
        }
        if (resource->flags & FLAG_OBSERVE) {
            oc_resource_set_periodic_observable(resource->res, 1);
        }

        oc_resource_set_request_handler(resource->res, OC_GET, ocf_get_handler, resource);
        oc_resource_set_request_handler(resource->res, OC_PUT, ocf_put_handler, resource);
        oc_resource_set_request_handler(resource->res, OC_POST, ocf_put_handler, resource);

        oc_add_resource(resource->res);

        // Get UUID and set it in the ocf.device object
        oc_uuid_t *id = oc_core_get_device_id(resource->res->device);
        char uuid[37];
        oc_uuid_to_str(id, uuid, 37);
        zjs_set_uuid(uuid);

        cur = cur->next;
    }
}

jerry_value_t zjs_ocf_server_init()
{
    jerry_value_t server = jerry_create_object();

    zjs_obj_add_function(server, ocf_register, "register");
    zjs_obj_add_function(server, ocf_notify, "notify");

    zjs_make_event(server, ZJS_UNDEFINED);

    return server;
}

void zjs_ocf_server_cleanup()
{
    if (res_list) {
        resource_list_t *cur = res_list;
        while (cur) {
            int i;
            struct server_resource *resource = cur->resource;
            if (resource->device_id) {
                zjs_free(resource->device_id);
            }
            if (resource->resource_path) {
                zjs_free(resource->resource_path);
            }
            if (resource->resource_types) {
                for (i = 0; i < resource->num_types; ++i) {
                    if (resource->resource_types[i]) {
                        zjs_free(resource->resource_types[i]);
                    }
                }
                zjs_free(resource->resource_types);
            }
            if (resource->resource_ifaces) {
                for (i = 0; i < resource->num_ifaces; ++i) {
                    if (resource->resource_ifaces[i]) {
                        zjs_free(resource->resource_ifaces[i]);
                    }
                }
                zjs_free(resource->resource_ifaces);
            }
            jerry_release_value(resource->object);
            zjs_free(resource);
            res_list = res_list->next;
            zjs_free(cur);
            cur = res_list;
        }
    }
}

#endif // BUILD_MODULE_OCF
