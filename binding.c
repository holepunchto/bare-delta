#include <assert.h>
#include <bare.h>
#include <js.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include <zstd.h>

// Forward declarations for delta.c functions
int delta_create(
  const char *zSrc,      
  size_t lenSrc,         
  const char *zOut,      
  size_t lenOut,         
  char *zDelta          
);

int delta_apply(
  const char *zSrc,      
  size_t lenSrc,         
  const char *zDelta,    
  size_t lenDelta,       
  char *zOut             
);

int delta_output_size(const char *zDelta, size_t lenDelta);

int delta_create_with_options(
  const char *zSrc,
  size_t lenSrc,
  const char *zOut,
  size_t lenOut,
  char *zDelta,
  int nhash,
  int searchLimit
);


// Extract and validate buffer from JavaScript value
static int
extract_buffer(js_env_t *env, js_value_t *value, void **data, size_t *len, const char *name) {
  bool is_typed;
  if (js_is_typedarray(env, value, &is_typed) != 0 || !is_typed) {
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "%s must be a Buffer or TypedArray", name);
    js_throw_type_error(env, NULL, error_msg);
    return -1;
  }
  
  return js_get_typedarray_info(env, value, NULL, data, len, NULL, NULL);
}

// Parse delta options from JavaScript object
static void
parse_delta_options(js_env_t *env, js_value_t *options, int *nhash, int *searchLimit, int *compressed) {
  int err;
  js_value_t *prop;
  
  // Set defaults
  *nhash = 16;  // NHASH_DEFAULT
  *searchLimit = 250;  // SEARCH_LIMIT_DEFAULT
  *compressed = 0;  // No compression by default
  
  // Check if options is null (passed from C code) or JS null/undefined
  if (options == NULL) {
    return;
  }
  
  js_value_type_t type;
  if (js_typeof(env, options, &type) != 0 || type == js_null || type == js_undefined) {
    return;
  }
  
  // hashWindowSize
  if (js_get_named_property(env, options, "hashWindowSize", &prop) == 0) {
    js_value_type_t prop_type;
    if (js_typeof(env, prop, &prop_type) == 0 && prop_type == js_number) {
      int32_t value;
      if (js_get_value_int32(env, prop, &value) == 0 && value > 0 && (value & (value - 1)) == 0) {
        *nhash = value;  // Must be power of 2
      }
    }
  }
  
  // searchDepth
  if (js_get_named_property(env, options, "searchDepth", &prop) == 0) {
    js_value_type_t prop_type;
    if (js_typeof(env, prop, &prop_type) == 0 && prop_type == js_number) {
      int32_t value;
      if (js_get_value_int32(env, prop, &value) == 0 && value > 0) {
        *searchLimit = value;
      }
    }
  }
  
  // compressed
  if (js_get_named_property(env, options, "compressed", &prop) == 0) {
    js_value_type_t prop_type;
    if (js_typeof(env, prop, &prop_type) == 0 && prop_type == js_boolean) {
      bool value;
      if (js_get_value_bool(env, prop, &value) == 0) {
        *compressed = value ? 1 : 0;
      }
    }
  }
}

// Request structure for async operations - following bare-xdiff pattern
typedef struct {
  uv_work_t request;
  js_env_t *env;
  js_ref_t *ctx;
  js_ref_t *callback;
  
  // References to input TypedArrays (keeps them alive during async work)
  js_ref_t *source_ref;
  js_ref_t *target_ref;
  
  // Input buffer pointers (no copy, just pointing to TypedArray data)
  void *buf1;  // source
  size_t len1;
  void *buf2;  // target or delta
  size_t len2;
  
  // Options
  int nhash;
  int search_limit;
  int compressed;
  
  // Output
  char *result;
  size_t result_len;
  int32_t error_code;
  
  // Operation type
  int is_apply; // 0 for create, 1 for apply, 2 for apply_batch
  
  // For batch operations
  void **batch_deltas;  // Array of delta pointers
  size_t *batch_delta_lens; // Array of delta lengths
  size_t batch_count; // Number of deltas
  js_ref_t **batch_refs; // References to delta TypedArrays
  
  js_deferred_teardown_t *teardown;
} bare_delta_request_t;

// Core delta creation logic - shared by sync and async
static int
delta_create_core(const void *source, size_t source_len, const void *target, size_t target_len,
                  int nhash, int search_limit, int compressed, char **result, size_t *result_len) {
  // Allocate buffer for delta - worst case is target_len + small overhead
  size_t delta_max = target_len + 1024;
  char *delta_buffer = (char *)malloc(delta_max);
  
  if (delta_buffer == NULL) {
    return -1; // Memory allocation failed
  }
  
  // Create the delta
  int delta_len = delta_create_with_options(
    (const char *)source, source_len,
    (const char *)target, target_len,
    delta_buffer, nhash, search_limit
  );
  
  if (delta_len < 0) {
    free(delta_buffer);
    return -2; // Delta creation failed
  }
  
  // CRITICAL: Check for buffer overflow
  if ((size_t)delta_len >= delta_max) {
    free(delta_buffer);
    return -3; // Buffer overflow error
  }
  
  // Apply compression if requested
  if (compressed) {
    size_t compressed_bound = ZSTD_compressBound(delta_len);
    char *compressed_result = (char *)malloc(compressed_bound);
    
    if (compressed_result == NULL) {
      free(delta_buffer);
      return -4; // Compression buffer allocation failed
    }
    
    size_t compressed_size = ZSTD_compress(
      compressed_result, compressed_bound,
      delta_buffer, delta_len, 1
    );
    
    if (ZSTD_isError(compressed_size)) {
      free(delta_buffer);
      free(compressed_result);
      return -5; // Compression failed
    }
    
    free(delta_buffer);
    *result = compressed_result;
    *result_len = compressed_size;
  } else {
    *result = delta_buffer;
    *result_len = delta_len;
  }
  
  return 0; // Success
}

// Core batch delta application logic - applies multiple deltas sequentially
static int
delta_apply_batch_core(const void *source, size_t source_len, 
                      void **deltas, size_t *delta_lens, size_t delta_count,
                      int compressed, char **result, size_t *result_len);

// Core delta application logic - shared by sync and async
static int
delta_apply_core(const void *source, size_t source_len, const void *delta, size_t delta_len,
                 int compressed, char **result, size_t *result_len) {
  const char *delta_data = (const char *)delta;
  size_t final_delta_len = delta_len;
  char *decompressed_delta = NULL;
  
  // Handle decompression if requested
  if (compressed) {
    unsigned long long decompressed_size = ZSTD_getFrameContentSize(delta_data, delta_len);
    
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || 
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
      return -1; // Invalid compressed format
    }
    
    decompressed_delta = (char *)malloc(decompressed_size);
    if (decompressed_delta == NULL) {
      return -2; // Decompression buffer allocation failed
    }
    
    size_t actual_size = ZSTD_decompress(
      decompressed_delta, decompressed_size,
      delta_data, delta_len
    );
    
    if (ZSTD_isError(actual_size)) {
      free(decompressed_delta);
      return -3; // Decompression failed
    }
    
    delta_data = decompressed_delta;
    final_delta_len = actual_size;
  }
  
  // Get output size from delta
  int output_size = delta_output_size(delta_data, final_delta_len);
  if (output_size < 0) {
    if (decompressed_delta) free(decompressed_delta);
    return -4; // Invalid delta format
  }
  
  // Allocate buffer for output + null terminator
  char *output_buffer = (char *)malloc(output_size + 1);
  if (output_buffer == NULL) {
    if (decompressed_delta) free(decompressed_delta);
    return -5; // Output buffer allocation failed
  }
  
  // Apply the delta
  int applied_len = delta_apply(
    (const char *)source, source_len,
    delta_data, final_delta_len,
    output_buffer
  );
  
  if (applied_len < 0) {
    free(output_buffer);
    if (decompressed_delta) free(decompressed_delta);
    return -6; // Delta application failed
  }
  
  if (decompressed_delta) free(decompressed_delta);
  *result = output_buffer;
  *result_len = applied_len;
  return 0; // Success
}

// Core batch delta application logic - applies multiple deltas sequentially
static int
delta_apply_batch_core(const void *source, size_t source_len, 
                      void **deltas, size_t *delta_lens, size_t delta_count,
                      int compressed, char **result, size_t *result_len) {
  if (delta_count == 0) {
    // No deltas to apply, return copy of source
    char *output = (char *)malloc(source_len);
    if (output == NULL) {
      return -1;
    }
    memcpy(output, source, source_len);
    *result = output;
    *result_len = source_len;
    return 0;
  }
  
  // Apply first delta
  char *current_result;
  size_t current_len;
  int err = delta_apply_core(source, source_len, deltas[0], delta_lens[0], 
                             compressed, &current_result, &current_len);
  if (err != 0) {
    return err;
  }
  
  // Apply subsequent deltas
  for (size_t i = 1; i < delta_count; i++) {
    char *next_result;
    size_t next_len;
    
    err = delta_apply_core(current_result, current_len, deltas[i], delta_lens[i],
                          compressed, &next_result, &next_len);
    
    free(current_result); // Free intermediate result
    
    if (err != 0) {
      return err;
    }
    
    current_result = next_result;
    current_len = next_len;
  }
  
  *result = current_result;
  *result_len = current_len;
  return 0;
}

// Worker function - delegates to core logic
static void
bare_delta_work(uv_work_t *handle) {
  bare_delta_request_t *request = (bare_delta_request_t *)handle->data;
  
  if (!request) {
    return;
  }
  
  if (request->is_apply == 2) {
    // Batch apply
    request->error_code = delta_apply_batch_core(
      request->buf1, request->len1,
      request->batch_deltas, request->batch_delta_lens, request->batch_count,
      request->compressed,
      &request->result, &request->result_len
    );
  } else if (request->is_apply == 1) {
    // Single apply
    request->error_code = delta_apply_core(
      request->buf1, request->len1,
      request->buf2, request->len2,
      request->compressed,
      &request->result, &request->result_len
    );
  } else {
    // Create
    request->error_code = delta_create_core(
      request->buf1, request->len1,
      request->buf2, request->len2,
      request->nhash, request->search_limit, request->compressed,
      &request->result, &request->result_len
    );
  }
}

// Callback after worker completes
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
    if (request->result && request->result_len > 0) {
      js_value_t *arraybuffer;
      void *data;
      err = js_create_arraybuffer(env, request->result_len, &data, &arraybuffer);
      assert(err == 0);
      memcpy(data, request->result, request->result_len);
      
      err = js_create_typedarray(env, js_uint8array, request->result_len, arraybuffer, 0, &argv[1]);
      assert(err == 0);
    } else {
      // Handle edge case where result is empty but not an error
      js_value_t *arraybuffer;
      void *data;
      err = js_create_arraybuffer(env, 0, &data, &arraybuffer);
      assert(err == 0);
      
      err = js_create_typedarray(env, js_uint8array, 0, arraybuffer, 0, &argv[1]);
      assert(err == 0);
    }
  }
  
  // Call the callback
  err = js_call_function(env, ctx, callback, 2, argv, NULL);
  if (err != 0) {
    // Callback failed - this would cause hanging Promises
    // Continue with cleanup to prevent resource leaks
  }
  
  err = js_close_handle_scope(env, scope);
  assert(err == 0);
  
  // Clean up
  if (request->source_ref) {
    err = js_delete_reference(env, request->source_ref);
    assert(err == 0);
  }
  if (request->target_ref) {
    err = js_delete_reference(env, request->target_ref);
    assert(err == 0);
  }
  
  // Clean up batch references if present
  if (request->batch_refs) {
    for (size_t i = 0; i < request->batch_count; i++) {
      if (request->batch_refs[i]) {
        err = js_delete_reference(env, request->batch_refs[i]);
        assert(err == 0);
      }
    }
    free(request->batch_refs);
  }
  if (request->batch_deltas) free(request->batch_deltas);
  if (request->batch_delta_lens) free(request->batch_delta_lens);
  
  if (request->result) free(request->result);
  
  err = js_delete_reference(env, request->ctx);
  if (err != 0) {
    // Critical: Reference deletion failed - this causes resource leaks
    // Continue with cleanup to prevent worse issues
  }
  
  err = js_delete_reference(env, request->callback);  
  if (err != 0) {
    // Critical: Callback reference deletion failed - this causes hanging Promises
    // Continue with cleanup
  }
  
  err = js_finish_deferred_teardown_callback(request->teardown);
  if (err != 0) {
    // Critical: Teardown callback failed - this could cause hangs
    // Still free the request to prevent memory leaks
  }
  
  free(request);
}

// Synchronous delta_create binding
static js_value_t *
bare_delta_create_sync(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 3;
  js_value_t *argv[3];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  
  if (argc < 2) {
    js_throw_error(env, NULL, "delta.createSync requires at least 2 arguments (source, target[, options])");
    return NULL;
  }
  
  // Extract buffers
  size_t source_len, target_len;
  void *source_data, *target_data;
  
  if (extract_buffer(env, argv[0], &source_data, &source_len, "source") != 0 ||
      extract_buffer(env, argv[1], &target_data, &target_len, "target") != 0) {
    return NULL;
  }
  
  // Parse options
  int nhash, search_limit, compressed;
  parse_delta_options(env, argc > 2 ? argv[2] : NULL, &nhash, &search_limit, &compressed);
  
  // Use core logic
  char *result_data;
  size_t result_len;
  int result_code = delta_create_core(source_data, source_len, target_data, target_len,
                                      nhash, search_limit, compressed, &result_data, &result_len);
  
  if (result_code != 0) {
    js_throw_error(env, NULL, "Failed to create delta");
    return NULL;
  }
  
  // Create JS result
  js_value_t *arraybuffer;
  void *js_data;
  err = js_create_arraybuffer(env, result_len, &js_data, &arraybuffer);
  assert(err == 0);
  memcpy(js_data, result_data, result_len);
  free(result_data);
  
  js_value_t *result;
  err = js_create_typedarray(env, js_uint8array, result_len, arraybuffer, 0, &result);
  assert(err == 0);
  
  return result;
}

// Synchronous delta_apply binding
static js_value_t *
bare_delta_apply_sync(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 3;
  js_value_t *argv[3];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  
  if (argc < 2) {
    js_throw_error(env, NULL, "delta.applySync requires at least 2 arguments (source, delta[, options])");
    return NULL;
  }
  
  // Extract buffers
  size_t source_len, delta_len;
  void *source_data, *delta_data;
  
  if (extract_buffer(env, argv[0], &source_data, &source_len, "source") != 0 ||
      extract_buffer(env, argv[1], &delta_data, &delta_len, "delta") != 0) {
    return NULL;
  }
  
  // Parse options
  int nhash, search_limit, compressed;
  parse_delta_options(env, argc > 2 ? argv[2] : NULL, &nhash, &search_limit, &compressed);
  
  // Use core logic
  char *result_data;
  size_t result_len;
  int result_code = delta_apply_core(source_data, source_len, delta_data, delta_len,
                                     compressed, &result_data, &result_len);
  
  if (result_code != 0) {
    js_throw_error(env, NULL, "Failed to apply delta");
    return NULL;
  }
  
  // Create JS result
  js_value_t *arraybuffer;
  void *js_data;
  err = js_create_arraybuffer(env, result_len, &js_data, &arraybuffer);
  assert(err == 0);
  memcpy(js_data, result_data, result_len);
  free(result_data);
  
  js_value_t *result;
  err = js_create_typedarray(env, js_uint8array, result_len, arraybuffer, 0, &result);
  assert(err == 0);
  
  return result;
}

// Extract buffer and create reference (helper function)
static int
extract_buffer_with_ref(js_env_t *env, js_value_t *value, const char *name,
                        void **data, size_t *len, js_ref_t **ref) {
  if (extract_buffer(env, value, data, len, name) != 0) {
    return -1;
  }
  
  // Create a reference to keep the TypedArray alive
  int err = js_create_reference(env, value, 1, ref);
  if (err != 0) {
    js_throw_error(env, NULL, "Failed to create buffer reference");
    return -1;
  }
  
  return 0;
}

// Asynchronous delta_create binding
static js_value_t *
bare_delta_create_async(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 4;
  js_value_t *argv[4];
  js_value_t *ctx;
  err = js_get_callback_info(env, info, &argc, argv, &ctx, NULL);
  assert(err == 0);
  
  if (argc < 3) {
    js_throw_error(env, NULL, "delta.create requires at least 3 arguments (source, target, [options,] callback)");
    return NULL;
  }
  
  // Allocate request
  bare_delta_request_t *request = (bare_delta_request_t *)malloc(sizeof(bare_delta_request_t));
  memset(request, 0, sizeof(bare_delta_request_t));
  
  request->env = env;
  request->is_apply = 0;
  
  // Extract buffers and create references (no copying)
  if (extract_buffer_with_ref(env, argv[0], "source", &request->buf1, &request->len1, &request->source_ref) != 0 ||
      extract_buffer_with_ref(env, argv[1], "target", &request->buf2, &request->len2, &request->target_ref) != 0) {
    if (request->source_ref) js_delete_reference(env, request->source_ref);
    free(request);
    return NULL;
  }
  
  // Parse options and store callback
  js_value_t *callback;
  if (argc == 4) {
    parse_delta_options(env, argv[2], &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[3];
  } else {
    parse_delta_options(env, NULL, &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[2];
  }
  
  // Store callback reference
  err = js_create_reference(env, callback, 1, &request->callback);
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
  uv_queue_work(loop, &request->request, bare_delta_work, bare_delta_after_work);
  
  return NULL;
}

// Asynchronous delta_apply binding
static js_value_t *
bare_delta_apply_async(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 4;
  js_value_t *argv[4];
  js_value_t *ctx;
  err = js_get_callback_info(env, info, &argc, argv, &ctx, NULL);
  assert(err == 0);
  
  if (argc < 3) {
    js_throw_error(env, NULL, "delta.apply requires at least 3 arguments (source, delta, [options,] callback)");
    return NULL;
  }
  
  // Allocate request
  bare_delta_request_t *request = (bare_delta_request_t *)malloc(sizeof(bare_delta_request_t));
  memset(request, 0, sizeof(bare_delta_request_t));
  
  request->env = env;
  request->is_apply = 1;
  
  // Extract buffers and create references (no copying)
  if (extract_buffer_with_ref(env, argv[0], "source", &request->buf1, &request->len1, &request->source_ref) != 0 ||
      extract_buffer_with_ref(env, argv[1], "delta", &request->buf2, &request->len2, &request->target_ref) != 0) {
    if (request->source_ref) js_delete_reference(env, request->source_ref);
    free(request);
    return NULL;
  }
  
  // Parse options and store callback
  js_value_t *callback;
  if (argc == 4) {
    parse_delta_options(env, argv[2], &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[3];
  } else {
    parse_delta_options(env, NULL, &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[2];
  }
  
  // Store callback reference
  err = js_create_reference(env, callback, 1, &request->callback);
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
  uv_queue_work(loop, &request->request, bare_delta_work, bare_delta_after_work);
  
  return NULL;
}

// Synchronous batch delta_apply binding
static js_value_t *
bare_delta_apply_batch_sync(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 3;
  js_value_t *argv[3];
  err = js_get_callback_info(env, info, &argc, argv, NULL, NULL);
  assert(err == 0);
  
  if (argc < 2) {
    js_throw_error(env, NULL, "delta.applyBatchSync requires at least 2 arguments (source, deltas[, options])");
    return NULL;
  }
  
  // Extract source buffer
  size_t source_len;
  void *source_data;
  if (extract_buffer(env, argv[0], &source_data, &source_len, "source") != 0) {
    return NULL;
  }
  
  // Get delta array
  bool is_array;
  err = js_is_array(env, argv[1], &is_array);
  assert(err == 0);
  
  if (!is_array) {
    js_throw_type_error(env, NULL, "deltas must be an array");
    return NULL;
  }
  
  uint32_t delta_count;
  err = js_get_array_length(env, argv[1], &delta_count);
  assert(err == 0);
  
  // Parse options
  int nhash, search_limit, compressed;
  parse_delta_options(env, argc > 2 ? argv[2] : NULL, &nhash, &search_limit, &compressed);
  
  // Extract all deltas
  void **deltas = malloc(sizeof(void *) * delta_count);
  size_t *delta_lens = malloc(sizeof(size_t) * delta_count);
  
  if (!deltas || !delta_lens) {
    if (deltas) free(deltas);
    if (delta_lens) free(delta_lens);
    js_throw_error(env, NULL, "Failed to allocate memory for delta arrays");
    return NULL;
  }
  
  for (uint32_t i = 0; i < delta_count; i++) {
    js_value_t *delta_val;
    err = js_get_element(env, argv[1], i, &delta_val);
    assert(err == 0);
    
    if (extract_buffer(env, delta_val, &deltas[i], &delta_lens[i], "delta") != 0) {
      free(deltas);
      free(delta_lens);
      return NULL;
    }
  }
  
  // Use core batch logic
  char *result_data;
  size_t result_len;
  int result_code = delta_apply_batch_core(source_data, source_len, deltas, delta_lens, delta_count,
                                           compressed, &result_data, &result_len);
  
  free(deltas);
  free(delta_lens);
  
  if (result_code != 0) {
    js_throw_error(env, NULL, "Failed to apply batch deltas");
    return NULL;
  }
  
  // Create JS result
  js_value_t *arraybuffer;
  void *js_data;
  err = js_create_arraybuffer(env, result_len, &js_data, &arraybuffer);
  assert(err == 0);
  memcpy(js_data, result_data, result_len);
  free(result_data);
  
  js_value_t *result;
  err = js_create_typedarray(env, js_uint8array, result_len, arraybuffer, 0, &result);
  assert(err == 0);
  
  return result;
}

// Asynchronous batch delta_apply binding
static js_value_t *
bare_delta_apply_batch_async(js_env_t *env, js_callback_info_t *info) {
  int err;
  size_t argc = 4;
  js_value_t *argv[4];
  js_value_t *ctx;
  err = js_get_callback_info(env, info, &argc, argv, &ctx, NULL);
  assert(err == 0);
  
  if (argc < 3) {
    js_throw_error(env, NULL, "delta.applyBatch requires at least 3 arguments (source, deltas, [options,] callback)");
    return NULL;
  }
  
  // Get delta array
  bool is_array;
  err = js_is_array(env, argv[1], &is_array);
  assert(err == 0);
  
  if (!is_array) {
    js_throw_type_error(env, NULL, "deltas must be an array");
    return NULL;
  }
  
  uint32_t delta_count;
  err = js_get_array_length(env, argv[1], &delta_count);
  assert(err == 0);
  
  // Allocate request
  bare_delta_request_t *request = (bare_delta_request_t *)malloc(sizeof(bare_delta_request_t));
  memset(request, 0, sizeof(bare_delta_request_t));
  
  request->env = env;
  request->is_apply = 2; // Batch apply mode
  request->batch_count = delta_count;
  
  // Extract source buffer and create reference
  if (extract_buffer_with_ref(env, argv[0], "source", &request->buf1, &request->len1, &request->source_ref) != 0) {
    free(request);
    return NULL;
  }
  
  // Allocate arrays for batch operation
  request->batch_deltas = malloc(sizeof(void *) * delta_count);
  request->batch_delta_lens = malloc(sizeof(size_t) * delta_count);
  request->batch_refs = malloc(sizeof(js_ref_t *) * delta_count);
  
  if (!request->batch_deltas || !request->batch_delta_lens || !request->batch_refs) {
    if (request->source_ref) js_delete_reference(env, request->source_ref);
    if (request->batch_deltas) free(request->batch_deltas);
    if (request->batch_delta_lens) free(request->batch_delta_lens);
    if (request->batch_refs) free(request->batch_refs);
    free(request);
    js_throw_error(env, NULL, "Failed to allocate memory for batch arrays");
    return NULL;
  }
  
  memset(request->batch_refs, 0, sizeof(js_ref_t *) * delta_count);
  
  // Extract all deltas and create references
  for (uint32_t i = 0; i < delta_count; i++) {
    js_value_t *delta_val;
    err = js_get_element(env, argv[1], i, &delta_val);
    assert(err == 0);
    
    if (extract_buffer_with_ref(env, delta_val, "delta", 
                                &request->batch_deltas[i], &request->batch_delta_lens[i], 
                                &request->batch_refs[i]) != 0) {
      // Clean up on error
      if (request->source_ref) js_delete_reference(env, request->source_ref);
      for (uint32_t j = 0; j <= i; j++) {
        if (request->batch_refs[j]) js_delete_reference(env, request->batch_refs[j]);
      }
      free(request->batch_deltas);
      free(request->batch_delta_lens);
      free(request->batch_refs);
      free(request);
      return NULL;
    }
  }
  
  // Parse options and store callback  
  js_value_t *callback;
  if (argc == 4) {
    parse_delta_options(env, argv[2], &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[3];
  } else {
    parse_delta_options(env, NULL, &request->nhash, &request->search_limit, &request->compressed);
    callback = argv[2];
  }
  
  // Store callback reference
  err = js_create_reference(env, callback, 1, &request->callback);
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
  uv_queue_work(loop, &request->request, bare_delta_work, bare_delta_after_work);
  
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
  
  js_value_t *apply_batch_fn;
  js_create_function(env, "applyBatch", -1, bare_delta_apply_batch_async, NULL, &apply_batch_fn);
  js_set_named_property(env, exports, "applyBatch", apply_batch_fn);
  
  js_value_t *apply_batch_sync_fn;
  js_create_function(env, "applyBatchSync", -1, bare_delta_apply_batch_sync, NULL, &apply_batch_sync_fn);
  js_set_named_property(env, exports, "applyBatchSync", apply_batch_sync_fn);
  
  return exports;
}

BARE_MODULE(bare_delta, init)
