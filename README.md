# bare-delta

Binary patch handling for Bare. Provides both asynchronous and synchronous APIs for creating and applying binary deltas using a modified version of Fossil SCM's delta algorithm.

Includes zstd support for working with compressed patches.

```bash
npm install bare-delta
```

## Usage

```js
const original = Buffer.from('Hello world!')
const modified = Buffer.from('Hello Bare world!')

// Async API
const patch = await create(original, modified)
const result = await apply(original, patch)

// Sync API  
const patch = createSync(original, modified)
const result = applySync(original, patch)

// Apply multiple patches
const patches = [patch1, patch2, patch3]
const finalResult = await applyBatch(original, patches)
const finalResultSync = applyBatchSync(original, patches)
```

## API

### `create(original, modified[, options])`

Creates a binary patch between two buffers.

- `original` - Original data (Buffer or Uint8Array)
- `modified` - Modified data (Buffer or Uint8Array)
- `options` - Optional creation options
  - `hashWindowSize` - Hash window size (must be power of 2, default: 16)
  - `searchDepth` - Maximum search depth for matches (default: 250)
  - `compressed` - Whether to compress the patch (default: false)

Returns a `Promise<Buffer>` containing the patch.

### `apply(original, patch[, options])`

Applies a binary patch to reconstruct the modified data.

- `original` - Original data (Buffer or Uint8Array)
- `patch` - Patch created by `create()` (Buffer or Uint8Array)
- `options` - Optional apply options
  - `compressed` - Whether the patch is compressed (default: false)

Returns a `Promise<Buffer>` containing the result.

### `applyBatch(original, patches[, options])`

Applies multiple binary patches sequentially to reconstruct the final result.

- `original` - Original data (Buffer or Uint8Array)
- `patches` - Array of patches to apply in order (Array of Buffer or Uint8Array)
- `options` - Optional apply options
  - `compressed` - Whether all patches are compressed (default: false)

Returns a `Promise<Buffer>` containing the result.

### `createSync(original, modified[, options])`

Synchronous version of `create()`. Returns a `Buffer` directly.

- `original` - Original data (Buffer or Uint8Array)
- `modified` - Modified data (Buffer or Uint8Array)
- `options` - Optional creation options
  - `hashWindowSize` - Hash window size (must be power of 2, default: 16)
  - `searchDepth` - Maximum search depth for matches (default: 250)
  - `compressed` - Whether to compress the patch (default: false)

### `applySync(original, patch[, options])`

Synchronous version of `apply()`. Returns a `Buffer` directly.

- `original` - Original data (Buffer or Uint8Array)
- `patch` - Patch created by `create()` (Buffer or Uint8Array)
- `options` - Optional apply options
  - `compressed` - Whether the patch is compressed (default: false)

### `applyBatchSync(original, patches[, options])`

Synchronous version of `applyBatch()`. Returns a `Buffer` directly.

- `original` - Original data (Buffer or Uint8Array)
- `patches` - Array of patches to apply in order (Array of Buffer or Uint8Array)
- `options` - Optional apply options
  - `compressed` - Whether all patches are compressed (default: false)

## Algorithm Enhancements

This library implements an enhanced version of Fossil SCM's delta compression algorithm with the following optimizations:

### SIMD Acceleration

We've added SIMD (Single Instruction Multiple Data) optimizations to accelerate the core hash computation and chunk matching operations. Our SIMD implementation processes multiple data elements in parallel for these operations on modern processors.

### Compact Encoding

The original Fossil delta format uses a custom base-64 encoding for integers (see `putInt`/`getInt` functions in the reference implementation). We've replaced this with [compact-encoding](https://github.com/compact-encoding) for variable-length integer encoding.

## Performance

Use the sync API for better performance on small to medium files. Use the async API for large files or when you need to avoid blocking the event loop.

## License

Apache 2.0

This project incorporates code from Fossil SCM's delta compression algorithm, which is licensed under the BSD-2-Clause License. Copyright (c) 2006 D. Richard Hipp.
