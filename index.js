const binding = require('./binding')
const b4a = require('b4a')

/**
 * Creates a binary delta between source and target buffers.
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array} target - The target/modified buffer  
 * @param {Object} [options] - Optional delta creation options
 * @param {number} [options.hashWindowSize=16] - Hash window size (must be power of 2)
 * @param {number} [options.searchDepth=250] - Maximum search depth for matches
 * @param {boolean} [options.compressed=false] - Whether to compress the delta
 * @returns {Promise<Uint8Array>} A Promise that resolves with the delta buffer
 */
async function create(source, target, options = {}) {
  return new Promise((resolve, reject) => {
    binding.create(source, target, options, (err, result) => {
      if (err) reject(err)
      else resolve(b4a.toBuffer(result))
    })
  })
}

/**
 * Applies a binary delta to a source buffer to recreate the target.
 * Automatically detects if the delta is zstd compressed.
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array} delta - The delta buffer created by create()
 * @returns {Promise<Uint8Array>} A Promise that resolves with the target buffer
 */
async function apply(source, delta) {
  return new Promise((resolve, reject) => {
    binding.apply(source, delta, (err, result) => {
      if (err) reject(err)
      else resolve(b4a.toBuffer(result))
    })
  })
}

/**
 * Creates a binary delta between source and target buffers (synchronous).
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array} target - The target/modified buffer
 * @param {Object} [options] - Optional delta creation options
 * @param {number} [options.hashWindowSize=16] - Hash window size (must be power of 2)
 * @param {number} [options.searchDepth=250] - Maximum search depth for matches
 * @param {boolean} [options.compressed=false] - Whether to compress the delta
 * @returns {Uint8Array} The delta buffer
 */
function createSync(source, target, options = {}) {
  const result = binding.createSync(source, target, options);
  return b4a.toBuffer(result);
}

/**
 * Applies a binary delta to a source buffer to recreate the target (synchronous).
 * Automatically detects if the delta is zstd compressed.
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array} delta - The delta buffer created by create()
 * @returns {Uint8Array} The target buffer
 */
function applySync(source, delta) {
  return b4a.toBuffer(binding.applySync(source, delta))
}

/**
 * Applies multiple binary deltas sequentially to a source buffer.
 * Automatically detects if each delta is zstd compressed.
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array[]} deltas - Array of delta buffers to apply in sequence
 * @returns {Promise<Uint8Array>} A Promise that resolves with the final target buffer
 */
async function applyBatch(source, deltas) {
  return new Promise((resolve, reject) => {
    binding.applyBatch(source, deltas, (err, result) => {
      if (err) reject(err)
      else resolve(b4a.toBuffer(result))
    })
  })
}

/**
 * Applies multiple binary deltas sequentially to a source buffer (synchronous).
 * Automatically detects if each delta is zstd compressed.
 * 
 * @param {Uint8Array} source - The source/original buffer
 * @param {Uint8Array[]} deltas - Array of delta buffers to apply in sequence
 * @returns {Uint8Array} The final target buffer
 */
function applyBatchSync(source, deltas) {
  return b4a.toBuffer(binding.applyBatchSync(source, deltas))
}

module.exports = {
  create,
  apply,
  createSync,
  applySync,
  applyBatch,
  applyBatchSync
}