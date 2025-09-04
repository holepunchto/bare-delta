#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

// Memory allocation wrappers for fossil functions
void* fossil_malloc(size_t size) {
  return malloc(size);
}

void fossil_free(void* ptr) {
  free(ptr);
}

// Forward declarations for delta.c functions
int delta_create(
  const char *zSrc,      
  unsigned int lenSrc,   
  const char *zOut,      
  unsigned int lenOut,   
  char *zDelta          
);

int delta_apply(
  const char *zSrc,      
  int lenSrc,            
  const char *zDelta,    
  int lenDelta,          
  char *zOut             
);

int delta_output_size(const char *zDelta, int lenDelta);

// Request structure for async operations - following bare-xdiff pattern
typedef struct {
  uv_work_t request;
  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *callback;
  
  // Input buffers
  void *buf1;  // source
  size_t len1;
  void *buf2;  // target or delta
  size_t len2;
  
  // Output
  char *result;
  size_t result_len;
  int32_t error_code;
  
  // Operation type
  int is_apply; // 0 for create, 1 for apply
  
  js_deferred_teardown_t *teardown;
} bare_delta_request_t;

// Worker function for delta_create
static void
bare_delta_create_work(uv_work_t *handle) {
  bare_delta_request_t *request = (bare_delta_request_t *)handle->data;
  
  // Allocate buffer for delta (should be at least 60 bytes larger than target)
  size_t delta_max = request->len2 + 1024;
  request->result = (char *)malloc(delta_max);
  
  if (request->result == NULL) {
    request->error_code = -1;
    return;
  }
  
  // Create the delta
  int delta_len = delta_create(
    (const char *)request->buf1,
    request->len1,
    (const char *)request->buf2, 
    request->len2,
    request->result
  );
  
  if (delta_len < 0) {
    free(request->result);
    request->result = NULL;
    request->error_code = -2;
    return;
  }
  
  request->result_len = delta_len;
  request->error_code = 0;
}

// Worker function for delta_apply
static void
bare_delta_apply_work(uv_work_t *handle) {
  bare_delta_request_t *request = (bare_delta_request_t *)handle->data;
  
  // Get output size from delta
  int output_size = delta_output_size((const char *)request->buf2, request->len2);
  
  if (output_size < 0) {
    request->error_code = -1;
    return;
  }
  
  // Allocate buffer for output
  request->result = (char *)malloc(output_size);
  
  if (request->result == NULL) {
    request->error_code = -2;
    return;
  }
  
  // Apply the delta
  int applied_len = delta_apply(
    (const char *)request->buf1,
    request->len1,
    (const char *)request->buf2,
    request->len2,
    request->result
  );
  
  if (applied_len < 0) {
    free(request->result);
    request->result = NULL;
    request->error_code = -3;
    return;
  }
  
  request->result_len = applied_len;
  request->error_code = 0;
}

// Callback after worker completes - following bare-xdiff pattern exactly
static void
bare_delta_after_work(uv_work_t *handle, int status) {
  int err;
  bare_delta_request_t *request = (bare_delta_request_t *)handle->data;
  
  js_env_t *env = request->env;
  js_handle_scope_t *scope;
  err = js_open_handle_scope(env, &scope);
  assert(err == 0);
  
  js_value_t *ctx;
  err = js_get_reference_value(env, request->ctx, &ctx);
  assert(err == 0);
  
  js_value_t *callback;
  err = js_get_reference_value(env, request->callback, &callback);
  assert(err == 0);
  
  js_value_t *argv[2];
  
  if (status != 0 || request->error_code < 0) {
    // Call callback(error, null)
    js_value_t *message;
    err = js_create_string_utf8(env, (const utf8_t *)"Operation failed", -1, &message);
    assert(err == 0);
    err = js_create_error(env, NULL, message, &argv[0]);
    assert(err == 0);
    
    err = js_get_null(env, &argv[1]);
    assert(err == 0);
  } else {
    // Call callback(null, result)
    err = js_get_null(env, &argv[0]);
    assert(err == 0);
    
    // Create arraybuffer and typedarray for binary data
    js_value_t *arraybuffer;
    void *data;
    err = js_create_arraybuffer(env, request->result_len, &data, &arraybuffer);
    assert(err == 0);
    memcpy(data, request->result, request->result_len);
    
    err = js_create_typedarray(env, js_uint8array, request->result_len, arraybuffer, 0, &argv[1]);
    assert(err == 0);
  }
  
  // Call the callback
  js_call_function(env, ctx, callback, 2, argv, NULL);
  
  err = js_close_handle_scope(env, scope);
  assert(err == 0);
  
  // Clean up
  free(request->buf1);
  free(request->buf2);
  if (request->result) free(request->result);
  
  err = js_delete_reference(env, request->ctx);
  assert(err == 0);
  
  err = js_delete_reference(env, request->callback);
  assert(err == 0);
  
  err = js_finish_deferred_teardown_callback(request->teardown);
  assert(err == 0);
  
  free(request);
}

// Synchronous delta_create binding
static js_value_t *
bare_delta_create_sync(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 2;
  js_value_t *argv[2];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  
  if (argc < 2) {
    js_throw_error(env, NULL, "delta.createSync requires 2 arguments (source, target)");
    return NULL;
  }
  
  // Get source buffer
  size_t source_len;
  void *source_data;
  bool is_source_typed;
  js_is_typedarray(env, argv[0], &is_source_typed);
  
  if (!is_source_typed) {
    js_throw_type_error(env, NULL, "source must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[0], NULL, &source_data, &source_len, NULL, NULL);
  
  // Get target buffer
  size_t target_len;
  void *target_data;
  bool is_target_typed;
  js_is_typedarray(env, argv[1], &is_target_typed);
  
  if (!is_target_typed) {
    js_throw_type_error(env, NULL, "target must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[1], NULL, &target_data, &target_len, NULL, NULL);
  
  // Allocate buffer for delta
  size_t delta_max = target_len + 1024;
  js_value_t *arraybuffer;
  void *delta_data;
  err = js_create_arraybuffer(env, delta_max, &delta_data, &arraybuffer);
  assert(err == 0);
  
  // Create the delta
  int delta_len = delta_create(
    (const char *)source_data,
    source_len,
    (const char *)target_data,
    target_len,
    (char *)delta_data
  );
  
  if (delta_len < 0) {
    js_throw_error(env, NULL, "Failed to create delta");
    return NULL;
  }
  
  // Create result buffer as a view of the arraybuffer with the actual length
  js_value_t *result;
  err = js_create_typedarray(env, js_uint8array, delta_len, arraybuffer, 0, &result);
  assert(err == 0);
  
  return result;
}

// Synchronous delta_apply binding
static js_value_t *
bare_delta_apply_sync(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 2;
  js_value_t *argv[2];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  
  if (argc < 2) {
    js_throw_error(env, NULL, "delta.applySync requires 2 arguments (source, delta)");
    return NULL;
  }
  
  // Get source buffer
  size_t source_len;
  void *source_data;
  bool is_source_typed;
  js_is_typedarray(env, argv[0], &is_source_typed);
  
  if (!is_source_typed) {
    js_throw_type_error(env, NULL, "source must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[0], NULL, &source_data, &source_len, NULL, NULL);
  
  // Get delta buffer
  size_t delta_len;
  void *delta_data;
  bool is_delta_typed;
  js_is_typedarray(env, argv[1], &is_delta_typed);
  
  if (!is_delta_typed) {
    js_throw_type_error(env, NULL, "delta must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[1], NULL, &delta_data, &delta_len, NULL, NULL);
  
  // Get output size
  int output_size = delta_output_size((const char *)delta_data, delta_len);
  
  if (output_size < 0) {
    js_throw_error(env, NULL, "Invalid delta format");
    return NULL;
  }
  
  // Allocate buffer for output
  js_value_t *arraybuffer;
  void *output_data;
  err = js_create_arraybuffer(env, output_size, &output_data, &arraybuffer);
  assert(err == 0);
  
  // Apply the delta
  int applied_len = delta_apply(
    (const char *)source_data,
    source_len,
    (const char *)delta_data,
    delta_len,
    (char *)output_data
  );
  
  if (applied_len < 0) {
    js_throw_error(env, NULL, "Failed to apply delta");
    return NULL;
  }
  
  // Create result buffer as a view of the arraybuffer
  js_value_t *result;
  err = js_create_typedarray(env, js_uint8array, applied_len, arraybuffer, 0, &result);
  assert(err == 0);
  
  return result;
}

// Asynchronous delta_create binding
static js_value_t *
bare_delta_create_async(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 3;
  js_value_t *argv[3];
  js_value_t *ctx;
  err = js_get_callback_info(env, info, &argc, argv, &ctx, NULL);
  assert(err == 0);
  
  if (argc < 3) {
    js_throw_error(env, NULL, "delta.create requires 3 arguments (source, target, callback)");
    return NULL;
  }
  
  // Allocate request
  bare_delta_request_t *request = (bare_delta_request_t *)malloc(sizeof(bare_delta_request_t));
  memset(request, 0, sizeof(bare_delta_request_t));
  
  request->env = env;
  request->is_apply = 0;
  
  // Get source buffer
  size_t source_len;
  void *source_data;
  bool is_source_typed;
  js_is_typedarray(env, argv[0], &is_source_typed);
  
  if (!is_source_typed) {
    free(request);
    js_throw_type_error(env, NULL, "source must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[0], NULL, &source_data, &source_len, NULL, NULL);
  request->buf1 = malloc(source_len);
  request->len1 = source_len;
  memcpy(request->buf1, source_data, source_len);
  
  // Get target buffer
  size_t target_len;
  void *target_data;
  bool is_target_typed;
  js_is_typedarray(env, argv[1], &is_target_typed);
  
  if (!is_target_typed) {
    free(request->buf1);
    free(request);
    js_throw_type_error(env, NULL, "target must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[1], NULL, &target_data, &target_len, NULL, NULL);
  request->buf2 = malloc(target_len);
  request->len2 = target_len;
  memcpy(request->buf2, target_data, target_len);
  
  // Store callback reference
  err = js_create_reference(env, argv[2], 1, &request->callback);
  assert(err == 0);
  
  err = js_create_reference(env, ctx, 1, &request->ctx);
  assert(err == 0);
  
  // Start teardown tracking
  err = js_add_deferred_teardown_callback(env, NULL, NULL, &request->teardown);
  assert(err == 0);
  
  // Queue work
  request->request.data = request;
  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);
  uv_queue_work(loop, &request->request, bare_delta_create_work, bare_delta_after_work);
  
  return NULL;
}

// Asynchronous delta_apply binding
static js_value_t *
bare_delta_apply_async(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 3;
  js_value_t *argv[3];
  js_value_t *ctx;
  err = js_get_callback_info(env, info, &argc, argv, &ctx, NULL);
  assert(err == 0);
  
  if (argc < 3) {
    js_throw_error(env, NULL, "delta.apply requires 3 arguments (source, delta, callback)");
    return NULL;
  }
  
  // Get source buffer
  size_t source_len;
  void *source_data;
  bool is_source_typed;
  js_is_typedarray(env, argv[0], &is_source_typed);
  
  if (!is_source_typed) {
    js_throw_type_error(env, NULL, "source must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[0], NULL, &source_data, &source_len, NULL, NULL);
  
  // Get delta buffer
  size_t delta_len;
  void *delta_data;
  bool is_delta_typed;
  js_is_typedarray(env, argv[1], &is_delta_typed);
  
  if (!is_delta_typed) {
    js_throw_type_error(env, NULL, "delta must be a Buffer or TypedArray");
    return NULL;
  }
  
  js_get_typedarray_info(env, argv[1], NULL, &delta_data, &delta_len, NULL, NULL);
  
  // Allocate request
  bare_delta_request_t *request = (bare_delta_request_t *)malloc(sizeof(bare_delta_request_t));
  memset(request, 0, sizeof(bare_delta_request_t));
  
  request->env = env;
  request->is_apply = 1;
  
  request->buf1 = malloc(source_len);
  request->len1 = source_len;
  memcpy(request->buf1, source_data, source_len);
  
  request->buf2 = malloc(delta_len);
  request->len2 = delta_len;
  memcpy(request->buf2, delta_data, delta_len);
  
  // Store callback reference
  err = js_create_reference(env, argv[2], 1, &request->callback);
  assert(err == 0);
  
  err = js_create_reference(env, ctx, 1, &request->ctx);
  assert(err == 0);
  
  // Start teardown tracking
  err = js_add_deferred_teardown_callback(env, NULL, NULL, &request->teardown);
  assert(err == 0);
  
  // Queue work
  request->request.data = request;
  uv_loop_t *loop;
  err = js_get_env_loop(env, &loop);
  assert(err == 0);
  uv_queue_work(loop, &request->request, bare_delta_apply_work, bare_delta_after_work);
  
  return NULL;
}

// Module initialization
static js_value_t *
init(js_env_t *env, js_value_t *exports) {
  js_value_t *create_fn;
  js_create_function(env, "create", -1, bare_delta_create_async, NULL, &create_fn);
  js_set_named_property(env, exports, "create", create_fn);
  
  js_value_t *apply_fn;
  js_create_function(env, "apply", -1, bare_delta_apply_async, NULL, &apply_fn);
  js_set_named_property(env, exports, "apply", apply_fn);
  
  js_value_t *create_sync_fn;
  js_create_function(env, "createSync", -1, bare_delta_create_sync, NULL, &create_sync_fn);
  js_set_named_property(env, exports, "createSync", create_sync_fn);
  
  js_value_t *apply_sync_fn;
  js_create_function(env, "applySync", -1, bare_delta_apply_sync, NULL, &apply_sync_fn);
  js_set_named_property(env, exports, "applySync", apply_sync_fn);
  
  return exports;
}

BARE_MODULE(bare_delta, init)