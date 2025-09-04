const binding = require('./binding')
const b4a = require('b4a')

/**
 * Creates a binary delta between source and target buffers.
 * 
 * @param {Buffer|Uint8Array} source - The source/original buffer
 * @param {Buffer|Uint8Array} target - The target/modified buffer
 * @returns {Promise<Buffer>} A Promise that resolves with the delta buffer
 */
async function create(source, target) {
  return new Promise((resolve, reject) => {
    binding.create(source, target, (err, result) => {
      if (err) reject(err)
      else resolve(b4a.toBuffer(result))
    })
  })
}

/**
 * Applies a binary delta to a source buffer to recreate the target.
 * 
 * @param {Buffer|Uint8Array} source - The source/original buffer
 * @param {Buffer|Uint8Array} delta - The delta buffer created by create()
 * @returns {Promise<Buffer>} A Promise that resolves with the target buffer
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
 * @param {Buffer|Uint8Array} source - The source/original buffer
 * @param {Buffer|Uint8Array} target - The target/modified buffer
 * @returns {Buffer} The delta buffer
 */
function createSync(source, target) {
  return b4a.toBuffer(binding.createSync(source, target))
}

/**
 * Applies a binary delta to a source buffer to recreate the target (synchronous).
 * 
 * @param {Buffer|Uint8Array} source - The source/original buffer
 * @param {Buffer|Uint8Array} delta - The delta buffer created by create()
 * @returns {Buffer} The target buffer
 */
function applySync(source, delta) {
  return b4a.toBuffer(binding.applySync(source, delta))
}

module.exports = {
  create,
  apply,
  createSync,
  applySync
}