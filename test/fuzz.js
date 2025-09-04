const test = require('brittle')
const { create, apply, createSync, applySync } = require('..')
const b4a = require('b4a')
const seedrandom = require('math-random-seed')
const process = require('bare-process')

const ITERATIONS = 1000 // Configurable number of fuzz test iterations

// Deterministic random number generator seeded for reproducibility
class SeededRandom {
  constructor (seed) {
    this.seed = seed
    this.rng = seedrandom(seed.toString())
  }

  // Random float between 0 and 1
  random () {
    return this.rng()
  }

  // Random integer between min (inclusive) and max (exclusive)
  randomInt (min, max) {
    return Math.floor(this.random() * (max - min)) + min
  }

  // Random bytes of given length - optimized for large buffers
  randomBytes (length) {
    const bytes = new Uint8Array(length)
    // Fill in chunks of 4 bytes for better performance
    const view = new DataView(bytes.buffer)
    let i = 0
    for (; i < length - 3; i += 4) {
      view.setUint32(i, Math.floor(this.random() * 0xFFFFFFFF), true)
    }
    // Fill remaining bytes
    for (; i < length; i++) {
      bytes[i] = this.randomInt(0, 256)
    }
    return b4a.from(bytes)
  }

  // Pick random element from array
  randomChoice (array) {
    return array[this.randomInt(0, array.length)]
  }

  // Random boolean with given probability (0.0 to 1.0)
  randomBool (probability = 0.5) {
    return this.random() < probability
  }
}

// Generate random test data using seeded randomness - fast and simple
function generateFuzzData (rng, size) {
  // Create buffer filled with random pattern value
  const fillValue = rng.randomInt(0, 256)
  return b4a.allocUnsafe(size).fill(fillValue)
}

// Apply random mutations to data using seeded randomness - fast point mutations only
function applyRandomMutations (rng, original, mutationRate = 0.05) {
  const data = b4a.from(original) // Copy original

  // Fast point mutations only
  const numMutations = Math.max(1, Math.floor(data.length * mutationRate))
  for (let i = 0; i < numMutations; i++) {
    const pos = rng.randomInt(0, data.length)
    data[pos] = rng.randomInt(0, 256)
  }

  return data
}

// Single fuzz test iteration
async function fuzzIteration (rng, iteration) {
  try {
    // Generate random test data
    const fileSize = rng.randomInt(100, 128 * 1024) // 100B to 512KB
    const original = generateFuzzData(rng, fileSize)

    // Apply random mutations
    const mutationRate = rng.random() * 0.3 + 0.01 // 1% to 31% mutation rate
    const modified = applyRandomMutations(rng, original, mutationRate)

    // Test with both sync and async, and with/without compression
    const testConfigs = [
      { async: false, compressed: false },
      { async: false, compressed: true },
      { async: true, compressed: false },
      { async: true, compressed: true }
    ]

    const config = rng.randomChoice(testConfigs)

    let delta, result

    // Time only the actual API calls
    const startTime = process.hrtime.bigint()
    
    if (config.async) {
      // Test async operations
      delta = await create(original, modified, config.compressed ? { compressed: true } : {})
      result = await apply(original, delta, config.compressed ? { compressed: true } : {})
    } else {
      // Test sync operations
      delta = createSync(original, modified, config.compressed ? { compressed: true } : {})
      result = applySync(original, delta, config.compressed ? { compressed: true } : {})
    }
    
    const endTime = process.hrtime.bigint()
    const durationMs = Number(endTime - startTime) / 1000000

    // Verify the result matches the expected modified data
    if (!b4a.equals(result, modified)) {
      throw new Error(`Fuzz test failed: Result does not match expected modified data
        Seed: ${seed}
        Iteration: ${iteration}
        Original size: ${original.length}
        Modified size: ${modified.length}
        Delta size: ${delta.length}
        Result size: ${result.length}
        Config: ${JSON.stringify(config)}
        Mutation rate: ${(mutationRate * 100).toFixed(2)}%`)
    }

    return {
      iteration,
      originalSize: original.length,
      modifiedSize: modified.length,
      deltaSize: delta.length,
      mutationRate,
      config,
      durationMs
    }
  } catch (error) {
    // Enhance error with reproduction info
    error.message += `
    Reproduction info:
      Iteration: ${iteration}`
    throw error
  }
}

test('fuzz testing - deterministic random delta operations', async (t) => {
  const baseSeed = Math.floor(Math.random() * 1000000) // Random base seed for this test run
  console.log(`\nFuzz testing with base seed: ${baseSeed}`)
  console.log(`Running ${ITERATIONS} iterations...`)

  const rng = new SeededRandom(baseSeed) // Single RNG instance for all iterations
  const results = []
  let failures = 0

  for (let i = 0; i < ITERATIONS; i++) {
    const iterStart = process.hrtime.bigint()
    try {
      const result = await fuzzIteration(rng, i)
      results.push(result)
      const iterEnd = process.hrtime.bigint()
      const iterTotalMs = Number(iterEnd - iterStart) / 1000000
      console.log(`  TOTAL ITER ${i}: ${iterTotalMs.toFixed(1)}ms (API: ${result.durationMs.toFixed(1)}ms)`)

      // Progress reporting - show every 10th iteration only
      if ((i + 1) % 10 === 0 || i < 10) {
        const sizeMB = (result.originalSize / 1024 / 1024).toFixed(2)
        const deltaSizeKB = (result.deltaSize / 1024).toFixed(1)
        const config = `${result.config.async ? 'async' : 'sync'}-${result.config.compressed ? 'comp' : 'uncomp'}`
        const duration = `${result.durationMs.toFixed(1)}ms`
        console.log(`  [${(i + 1).toString().padStart(4)}/${ITERATIONS}] ${sizeMB.padStart(6)}MB -> ${deltaSizeKB.padStart(6)}KB (${config}) ${duration.padStart(8)}`)
      }
    } catch (error) {
      failures++
      console.error(`\nFuzz test iteration ${i} failed:`)
      console.error(error.message)

      // Fail the test on first failure for debugging
      t.fail(`Fuzz test failed on iteration ${i}: ${error.message}`)
      return
    }
  }

  // Analyze results
  const avgOriginalSize = results.reduce((sum, r) => sum + r.originalSize, 0) / results.length
  const avgDeltaSize = results.reduce((sum, r) => sum + r.deltaSize, 0) / results.length
  const avgCompressionRatio = (avgDeltaSize / avgOriginalSize) * 100

  const configCounts = results.reduce((counts, r) => {
    const key = `${r.config.async ? 'async' : 'sync'}-${r.config.compressed ? 'compressed' : 'uncompressed'}`
    counts[key] = (counts[key] || 0) + 1
    return counts
  }, {})

  console.log('\nFuzz test completed successfully!')
  console.log(`  Iterations: ${ITERATIONS}`)
  console.log(`  Base seed: ${baseSeed}`)
  console.log(`  Failures: ${failures}`)
  console.log(`  Avg original size: ${avgOriginalSize.toFixed(0)} bytes`)
  console.log(`  Avg delta size: ${avgDeltaSize.toFixed(0)} bytes`)
  console.log(`  Avg compression ratio: ${avgCompressionRatio.toFixed(1)}%`)
  console.log('  Config distribution:', configCounts)

  t.ok(failures === 0, 'All fuzz iterations completed successfully')
  t.ok(results.length === ITERATIONS, `Completed all ${ITERATIONS} iterations`)
  t.ok(avgCompressionRatio < 100, 'Delta compression is effective on average')
})
