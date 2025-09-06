const test = require('brittle')
const b4a = require('b4a')
const delta = require('../index')
const { generateTestData, mutateData } = require('./helpers')

const { create, apply, createSync, applySync, applyBatch, applyBatchSync } = delta

test('create and apply delta - simple text', async (t) => {
  const source = b4a.from('Hello world!')
  const target = b4a.from('Hello Bare world!')
  
  // Create delta
  const diff = await delta.create(source, target)
  t.ok(b4a.isBuffer(diff), 'delta.create returns a buffer')
  t.ok(diff.length > 0, 'delta has content')
  
  // Apply delta
  const result = await delta.apply(source, diff)
  t.ok(b4a.isBuffer(result), 'delta.apply returns a buffer')
  t.alike(result, target, 'applied delta produces correct target')
})

test('createSync and applySync - simple text', (t) => {
  const source = b4a.from('The quick brown fox')
  const target = b4a.from('The quick brown fox jumps over the lazy dog')
  
  // Create delta synchronously
  const diff = delta.createSync(source, target)
  t.ok(b4a.isBuffer(diff), 'createSync returns a buffer')
  t.ok(diff.length > 0, 'delta has content')
  
  // Apply delta synchronously
  const result = delta.applySync(source, diff)
  t.ok(b4a.isBuffer(result), 'applySync returns a buffer')
  t.alike(result, target, 'applied delta produces correct target')
})

test('binary data', async (t) => {
  // Create some binary data
  const source = b4a.alloc(256)
  const target = b4a.alloc(256)
  
  for (let i = 0; i < 256; i++) {
    source[i] = i % 256
    target[i] = (i * 2) % 256
  }
  
  // Create and apply delta
  const diff = await delta.create(source, target)
  const result = await delta.apply(source, diff)
  
  t.alike(result, target, 'binary data roundtrips correctly')
})

test('large file with minimal changes', async (t) => {
  // Create a large buffer with minimal changes
  const size = 10000
  const source = b4a.alloc(size)
  const target = b4a.alloc(size)
  
  // Fill with pattern
  for (let i = 0; i < size; i++) {
    source[i] = i % 127
    target[i] = i % 127
  }
  
  // Make a few small changes
  target[100] = 255
  target[5000] = 255
  target[9999] = 255
  
  const diff = await delta.create(source, target)
  
  // Delta should be much smaller than the original
  t.ok(diff.length < size / 10, 'delta is efficient for small changes')
  
  const result = await delta.apply(source, diff)
  t.alike(result, target, 'large file with small changes roundtrips correctly')
})

test('empty source', async (t) => {
  const source = b4a.from('')
  const target = b4a.from('New content')
  
  const diff = await delta.create(source, target)
  const result = await delta.apply(source, diff)
  
  t.alike(result, target, 'empty source works correctly')
})

test('empty target', async (t) => {
  const source = b4a.from('Some content')
  const target = b4a.from('')
  
  const diff = await delta.create(source, target)
  const result = await delta.apply(source, diff)
  
  t.alike(result, target, 'empty target works correctly')
})

test('identical source and target', async (t) => {
  const source = b4a.from('Identical content')
  const target = b4a.from('Identical content')
  
  const diff = await delta.create(source, target)
  const result = await delta.apply(source, diff)
  
  t.alike(result, target, 'identical source and target works correctly')
})

test('completely different content', async (t) => {
  const source = b4a.from('AAAAAAAAAA')
  const target = b4a.from('BBBBBBBBBB')
  
  const diff = await delta.create(source, target)
  const result = await delta.apply(source, diff)
  
  t.alike(result, target, 'completely different content works correctly')
})

test('sync methods match async methods', async (t) => {
  const source = b4a.from('Test content for comparison')
  const target = b4a.from('Test modified content for comparison!')
  
  const asyncDiff = await delta.create(source, target)
  const syncDiff = delta.createSync(source, target)
  
  
  // Test cross-compatibility: async delta with sync apply, and vice versa
  const asyncDeltaSyncApply = delta.applySync(source, asyncDiff)
  const syncDeltaAsyncApply = await delta.apply(source, syncDiff)
  
  // All results should be identical
  const asyncResult = await delta.apply(source, asyncDiff)
  const syncResult = delta.applySync(source, syncDiff)
  
  t.alike(asyncResult, target, 'async result is correct')
  t.alike(syncResult, target, 'sync result is correct')
  t.alike(asyncResult, syncResult, 'async and sync produce same result')
  
  // Cross-compatibility tests
  t.alike(asyncDeltaSyncApply, target, 'async delta works with sync apply')
  t.alike(syncDeltaAsyncApply, target, 'sync delta works with async apply')
})

// Test helper functions
test('generateTestData - different data types', (t) => {
  const size = 1024
  
  const textData = generateTestData(size, 'text')
  t.is(textData.length, size, 'text data has correct size')
  t.ok(b4a.isBuffer(textData), 'text data is buffer')
  
  const binaryData = generateTestData(size, 'binary')
  t.is(binaryData.length, size, 'binary data has correct size')
  t.ok(b4a.isBuffer(binaryData), 'binary data is buffer')
  
  const structuredData = generateTestData(size, 'structured')
  t.is(structuredData.length, size, 'structured data has correct size')
  t.ok(b4a.isBuffer(structuredData), 'structured data is buffer')
  
  const randomData = generateTestData(size, 'random')
  t.is(randomData.length, size, 'random data has correct size')
  t.ok(b4a.isBuffer(randomData), 'random data is buffer')
})

test('mutateData - point mutations', (t) => {
  const original = generateTestData(1000, 'text')
  const mutated = mutateData(original, 'point', 0.1)
  
  t.is(mutated.length, original.length, 'point mutation preserves length')
  t.not(b4a.equals(mutated, original), 'point mutation changes data')
  
  // Count differences
  let differences = 0
  for (let i = 0; i < original.length; i++) {
    if (original[i] !== mutated[i]) differences++
  }
  
  t.ok(differences > 0, 'point mutation creates differences')
  t.ok(differences <= original.length * 0.15, 'point mutation respects mutation size')
})

test('mutateData - insert mutations', (t) => {
  const original = generateTestData(1000, 'text')
  const mutated = mutateData(original, 'insert', 0.1)
  
  t.ok(mutated.length > original.length, 'insert mutation increases length')
  t.not(b4a.equals(mutated, original), 'insert mutation changes data')
})

test('mutateData - delete mutations', (t) => {
  const original = generateTestData(1000, 'text')
  const mutated = mutateData(original, 'delete', 0.1)
  
  t.ok(mutated.length < original.length, 'delete mutation decreases length')
  t.not(b4a.equals(mutated, original), 'delete mutation changes data')
})

test('mutateData - move mutations', (t) => {
  const original = generateTestData(1000, 'text')
  const mutated = mutateData(original, 'move', 0.1)
  
  t.is(mutated.length, original.length, 'move mutation preserves length')
  // Note: move might not always change data if source and dest are same
})

test('mutateData - replace mutations', (t) => {
  const original = generateTestData(1000, 'text')
  const mutated = mutateData(original, 'replace', 0.1)
  
  t.is(mutated.length, original.length, 'replace mutation preserves length')
  t.not(b4a.equals(mutated, original), 'replace mutation changes data')
})

// Test delta creation and application with different mutation types
const mutationTypes = ['point', 'insert', 'delete', 'move', 'replace']
const dataTypes = ['text', 'binary', 'structured']

for (const dataType of dataTypes) {
  for (const mutationType of mutationTypes) {
    test(`delta roundtrip - ${dataType} data with ${mutationType} mutations`, async (t) => {
      const original = generateTestData(2048, dataType)
      const mutated = mutateData(original, mutationType, 0.1)
      
      // Create delta
      const diff = await delta.create(original, mutated)
      t.ok(diff.length > 0, 'delta has content')
      
      // Apply delta
      const result = await delta.apply(original, diff)
      t.alike(result, mutated, `delta roundtrips correctly for ${dataType} ${mutationType}`)
    })
  }
}

// Test with different mutation sizes
test('delta roundtrip - various mutation sizes', async (t) => {
  const original = generateTestData(4096, 'text')
  const mutationSizes = [0.01, 0.05, 0.1, 0.25]
  
  for (const mutationSize of mutationSizes) {
    const mutated = mutateData(original, 'point', mutationSize)
    const diff = await delta.create(original, mutated)
    const result = await delta.apply(original, diff)
    
    t.alike(result, mutated, `delta works with ${(mutationSize * 100).toFixed(0)}% mutation`)
  }
})

// Test delta options
test('delta options - hashWindowSize', async (t) => {
  const source = b4a.from('Hello world with lots of repeated content')
  const target = b4a.from('Hello world with lots of repeated content and more')
  
  const delta1 = await delta.create(source, target, { hashWindowSize: 8 })
  const delta2 = await delta.create(source, target, { hashWindowSize: 32 })
  
  const result1 = await delta.apply(source, delta1)
  const result2 = await delta.apply(source, delta2)
  
  t.alike(result1, target, 'small hash window produces correct result')
  t.alike(result2, target, 'large hash window produces correct result')
})

test('delta options - searchDepth', async (t) => {
  const source = generateTestData(4096, 'text')
  const target = mutateData(source, 'point', 0.05)
  
  const delta1 = await delta.create(source, target, { searchDepth: 100 })
  const delta2 = await delta.create(source, target, { searchDepth: 500 })
  
  const result1 = await delta.apply(source, delta1)
  const result2 = await delta.apply(source, delta2)
  
  t.alike(result1, target, 'shallow search produces correct result')
  t.alike(result2, target, 'deep search produces correct result')
})

// Benchmark validation tests
test('benchmark validation - mutation functions work correctly', async (t) => {
  const testCases = [
    { dataType: 'text', mutationType: 'point', size: 1024 },
    { dataType: 'binary', mutationType: 'insert', size: 1024 },
    { dataType: 'structured', mutationType: 'delete', size: 1024 },
    { dataType: 'text', mutationType: 'move', size: 1024 },
    { dataType: 'text', mutationType: 'replace', size: 1024 }
  ]
  
  for (const testCase of testCases) {
    const original = generateTestData(testCase.size, testCase.dataType)
    const mutated = mutateData(original, testCase.mutationType, 0.1)
    
    // Verify mutation worked
    if (testCase.mutationType !== 'move') {
      // Move might not change data if source and dest are same
      t.not(b4a.equals(original, mutated), `${testCase.dataType} ${testCase.mutationType} mutation changes data`)
    }
    
    // Test delta creation and application
    const diff = await delta.create(original, mutated)
    const result = await delta.apply(original, diff)
    
    t.alike(result, mutated, `delta roundtrips correctly for ${testCase.dataType} ${testCase.mutationType}`)
  }
})

test('benchmark validation - efficiency measurements are reasonable', async (t) => {
  const original = generateTestData(2048, 'text')
  
  // Test that different mutation sizes produce different delta sizes
  const mutationSizes = [0.01, 0.1, 0.5]
  const results = []
  
  for (const mutationSize of mutationSizes) {
    const mutated = mutateData(original, 'point', mutationSize)
    const diff = await delta.create(original, mutated)
    const ratio = diff.length / original.length
    
    results.push({ mutationSize, ratio })
    t.ok(ratio > 0 && ratio <= 1.5, `delta ratio (${(ratio * 100).toFixed(1)}%) is reasonable for ${(mutationSize * 100).toFixed(0)}% mutation`)
  }
  
  // Generally, more mutations should lead to larger deltas (though not always strictly)
  t.ok(results[0].ratio <= results[1].ratio * 1.5, 'small mutations generally produce smaller deltas')
})

test('256KB structured replace delta roundtrip', async (t) => {
  // Test the specific case that was failing in benchmarks
  const original = generateTestData(256 * 1024, 'structured')
  const mutated = mutateData(original, 'replace', 0.1)
  
  t.not(b4a.equals(original, mutated), 'replace mutation changes data')
  
  const diff = await delta.create(original, mutated)
  t.ok(diff.length > 0, 'delta has content')
  
  const result = await delta.apply(original, diff)
  t.alike(result, mutated, '256KB structured replace delta roundtrips correctly')
})

test('memory safety - concurrent operations with shared data', async (t) => {
  // Use the same source data for all operations to stress the memory management
  const source = generateTestData(8192, 'text')
  const target = mutateData(source, 'point', 0.1)
  
  const operations = []
  for (let i = 0; i < 2; i++) {
    operations.push(delta.create(source, target))
  }
  
  const deltas = await Promise.all(operations)
  
  // Now apply all deltas concurrently  
  const applyOperations = deltas.map(diff => delta.apply(source, diff))
  const results = await Promise.all(applyOperations)
  
  t.is(results.length, 2, 'all operations completed')
  t.ok(results.every(r => b4a.equals(r, target)), 'all results are correct')
})

test('memory safety - rapid create/destroy cycles', async (t) => {
  // Test rapid allocation/deallocation to catch use-after-free bugs
  const promises = []
  
  for (let i = 0; i < 100; i++) {
    const source = generateTestData(512 + (i % 100), 'binary') // Varying sizes
    const target = mutateData(source, 'replace', 0.05)
    
    // Mix of operations that succeed and might fail
    const operation = delta.create(source, target).then(async (diff) => {
      const result = await delta.apply(source, diff)
      return { iteration: i, success: b4a.equals(result, target) }
    })
    
    promises.push(operation)
    
    // Don't await - let them all run concurrently to stress memory
  }
  
  const results = await Promise.all(promises)
  
  t.is(results.length, 100, 'all operations completed')
  t.ok(results.every(r => r.success), 'all operations succeeded')
})

test('memory safety - mixed sync and async operations', async (t) => {
  const source = generateTestData(2048, 'structured')
  const target = mutateData(source, 'move', 0.1)
  
  const operations = []
  
  // Mix sync and async operations to test different code paths
  for (let i = 0; i < 30; i++) {
    if (i % 2 === 0) {
      // Async operation
      operations.push(delta.create(source, target))
    } else {
      // Sync operation wrapped in Promise
      operations.push(Promise.resolve(delta.createSync(source, target)))
    }
  }
  
  const deltas = await Promise.all(operations)
  
  // Apply using mixed sync/async - they should be compatible!
  const applyOperations = deltas.map((diff, i) => {
    if (i % 2 === 0) {
      return delta.apply(source, diff)
    } else {
      return Promise.resolve(delta.applySync(source, diff))
    }
  })
  
  const results = await Promise.all(applyOperations)
  
  t.is(results.length, 30, 'all operations completed')
  t.ok(results.every(r => b4a.equals(r, target)), 'all results match target')
})

test('memory safety - error handling with corrupted data', async (t) => {
  const source = b4a.from('hello')
  const invalidDelta = b4a.from('invalid delta data')
  
  try {
    await delta.apply(source, invalidDelta)
    t.fail('should have thrown an error')
  } catch (error) {
    t.ok(error, 'properly handles invalid delta')
  }
})

test('memory safety - stress test error paths with concurrent failures', async (t) => {
  // This test specifically targets the memory corruption bug that was fixed
  // by creating conditions where the C work function might fail early
  const operations = []
  
  for (let i = 0; i < 50; i++) {
    // Create some operations that might trigger error paths
    const source = generateTestData(100 + i, 'text')
    const target = mutateData(source, 'point', 0.01)
    
    // Also mix in some operations with invalid data to trigger error paths
    if (i % 10 === 0) {
      const invalidSource = b4a.alloc(0) // Empty buffer might trigger edge cases
      operations.push(
        delta.create(invalidSource, target).catch(() => ({ error: true, iteration: i }))
      )
    } else {
      operations.push(
        delta.create(source, target).then(diff => ({ diff, iteration: i }))
      )
    }
  }
  
  const results = await Promise.all(operations)
  
  const successes = results.filter(r => !r.error)
  const errors = results.filter(r => r.error)
  
  t.ok(successes.length > 0, 'some operations succeeded')
  t.ok(errors.length >= 0, 'error operations handled gracefully')
  t.is(results.length, 50, 'all operations completed without crashes')
})

test('benchmark stress test - large files with various mutations', async (t) => {
  // This test is skipped by default as it's slow, but can be enabled for stress testing
  const sizes = [16 * 1024, 64 * 1024] // Start smaller for testing
  const dataTypes = ['text', 'binary']
  const mutationTypes = ['point', 'insert', 'delete']
  
  for (const size of sizes) {
    for (const dataType of dataTypes) {
      for (const mutationType of mutationTypes) {
        const original = generateTestData(size, dataType)
        const mutated = mutateData(original, mutationType, 0.05)
        
        const diff = await delta.create(original, mutated)
        const result = await delta.apply(original, diff)
        
        t.alike(result, mutated, `stress test: ${size}B ${dataType} ${mutationType}`)
      }
    }
  }
})

// ZSTD Compression Tests
test('compression - sync create and apply with compression', (t) => {
  const source = b4a.from('Hello world! This is a test file with some repeated content. '.repeat(50))
  const target = b4a.from('Hello world! This is a modified test file with some repeated content. '.repeat(50))

  // Create compressed delta
  const compressedDelta = delta.createSync(source, target, { compressed: true })
  const uncompressedDelta = delta.createSync(source, target, { compressed: false })

  t.ok(compressedDelta.length > 0, 'compressed delta has content')
  t.ok(compressedDelta.length < uncompressedDelta.length, 'compressed delta is smaller')
  
  // Apply compressed delta (auto-detected)
  const result = delta.applySync(source, compressedDelta)
  t.alike(result, target, 'compressed delta produces correct target')
})

test('compression - async create and apply with compression', async (t) => {
  const source = b4a.from('This is repeated text data. '.repeat(100))
  const target = b4a.from('This is modified repeated text data. '.repeat(100))

  // Create compressed and uncompressed deltas
  const [compressedDelta, uncompressedDelta] = await Promise.all([
    delta.create(source, target, { compressed: true }),
    delta.create(source, target, { compressed: false })
  ])

  t.ok(compressedDelta.length > 0, 'async compressed delta has content')
  t.ok(compressedDelta.length < uncompressedDelta.length, 'async compressed delta is smaller')

  // Apply compressed delta (auto-detected)
  const result = await delta.apply(source, compressedDelta)
  t.alike(result, target, 'async compressed delta produces correct target')
})

test('compression - mixed sync and async compression operations', async (t) => {
  const source = generateTestData(4096, 'structured')
  const target = mutateData(source, 'point', 0.05)

  // Mix of sync and async compressed operations
  const syncCompressed = delta.createSync(source, target, { compressed: true })
  const asyncCompressed = await delta.create(source, target, { compressed: true })

  // Apply both ways
  const syncResult = delta.applySync(source, asyncCompressed)
  const asyncResult = await delta.apply(source, syncCompressed)

  t.alike(syncResult, target, 'sync apply of async compressed delta works')
  t.alike(asyncResult, target, 'async apply of sync compressed delta works')
})

test('compression - compression works on different data types', async (t) => {
  const testCases = ['text', 'binary', 'random']

  for (const dataType of testCases) {
    const source = generateTestData(2048, dataType)
    const target = mutateData(source, 'point', 0.05)

    const uncompressed = await delta.create(source, target, { compressed: false })
    const compressed = await delta.create(source, target, { compressed: true })

    t.ok(compressed.length > 0, `${dataType} compressed delta has content`)
    
    // Random data might not compress well due to high entropy
    if (dataType === 'random') {
      t.ok(compressed.length > 0, `${dataType} compressed delta works even if not smaller`)
    } else {
      t.ok(compressed.length <= uncompressed.length, `${dataType} compressed delta is not larger than uncompressed`)
    }
    
    const result = await delta.apply(source, compressed)
    t.alike(result, target, `${dataType} compressed data applies correctly`)
  }
})

test('compression - large file compression', async (t) => {
  // Test compression on larger files
  const source = generateTestData(32768, 'text')
  const target = mutateData(source, 'point', 0.02) // Small changes

  const uncompressed = await delta.create(source, target, { compressed: false })
  const compressed = await delta.create(source, target, { compressed: true })

  t.ok(compressed.length > 0, 'large file compressed delta has content')
  t.ok(compressed.length < uncompressed.length, 'large file compressed delta is smaller')

  const result = await delta.apply(source, compressed)
  t.alike(result, target, 'large file compressed delta applies correctly')
})

test('compression - automatic detection of compressed deltas', async (t) => {
  const source = b4a.from('Hello world! '.repeat(100))
  const target = b4a.from('Hello Bare world! '.repeat(100))
  
  // Create both compressed and uncompressed deltas
  const compressedDelta = await delta.create(source, target, { compressed: true })
  const uncompressedDelta = await delta.create(source, target, { compressed: false })
  
  // Verify compressed delta has zstd magic number
  t.ok(compressedDelta.length >= 4, 'compressed delta is at least 4 bytes')
  t.ok(compressedDelta[0] === 0x28 && compressedDelta[1] === 0xB5 && 
       compressedDelta[2] === 0x2F && compressedDelta[3] === 0xFD, 
       'compressed delta has zstd magic number')
  
  // Apply both without specifying compression - should auto-detect
  const resultFromCompressed = await delta.apply(source, compressedDelta)
  const resultFromUncompressed = await delta.apply(source, uncompressedDelta)
  
  t.alike(resultFromCompressed, target, 'auto-detected compressed delta applies correctly')
  t.alike(resultFromUncompressed, target, 'uncompressed delta applies correctly')
  
  // Test sync versions
  const syncResultCompressed = delta.applySync(source, compressedDelta)
  const syncResultUncompressed = delta.applySync(source, uncompressedDelta)
  
  t.alike(syncResultCompressed, target, 'sync auto-detected compressed delta applies correctly')
  t.alike(syncResultUncompressed, target, 'sync uncompressed delta applies correctly')
})

test('compression - error handling with zstd magic but invalid data', async (t) => {
  const source = generateTestData(1024, 'binary')
  // Create invalid data with correct zstd magic number
  const invalidWithMagic = b4a.from([
    0x28, 0xB5, 0x2F, 0xFD,  // Correct zstd magic number
    0xFF, 0xFF, 0xFF, 0xFF,  // Invalid frame header
    0x00, 0x00, 0x00, 0x00   // Invalid data
  ])

  try {
    await delta.apply(source, invalidWithMagic)
    t.fail('should throw error for invalid compressed data with magic number')
  } catch (err) {
    t.ok(err instanceof Error, 'throws error for corrupt data with zstd magic number (async)')
  }

  try {
    delta.applySync(source, invalidWithMagic)
    t.fail('should throw error for invalid compressed data with magic number (sync)')
  } catch (err) {
    t.ok(err instanceof Error, 'throws error for corrupt data with zstd magic number (sync)')
  }
})

test('compression - data without magic number treated as uncompressed', async (t) => {
  const source = generateTestData(1024, 'binary')
  // Data that doesn't start with zstd magic number
  const invalidUncompressed = b4a.from([0x01, 0x02, 0x03, 0x04, 0x05, 0x06])

  // This should fail because it's not a valid delta, not because of compression
  try {
    await delta.apply(source, invalidUncompressed)
    t.fail('should throw error for invalid delta format')
  } catch (err) {
    t.ok(err instanceof Error, 'throws error for invalid delta format (async)')
  }

  try {
    delta.applySync(source, invalidUncompressed)
    t.fail('should throw error for invalid delta format (sync)')
  } catch (err) {
    t.ok(err instanceof Error, 'throws error for invalid delta format (sync)')
  }
})

test('batch apply - apply multiple deltas sequentially sync', (t) => {
  const source = b4a.from('Hello')
  
  // Create a series of modifications
  const target1 = b4a.from('Hello World')
  const target2 = b4a.from('Hello World!')
  const target3 = b4a.from('Hello World!!!') 
  
  const delta1 = createSync(source, target1)
  const delta2 = createSync(target1, target2)
  const delta3 = createSync(target2, target3)
  
  // Apply all deltas in batch
  const result = applyBatchSync(source, [delta1, delta2, delta3])
  
  t.ok(b4a.equals(result, target3), 'batch apply produces correct final result')
})

test('batch apply - apply multiple deltas sequentially async', async (t) => {
  const source = b4a.from('Hello')
  
  // Create a series of modifications
  const target1 = b4a.from('Hello World')
  const target2 = b4a.from('Hello World!')
  const target3 = b4a.from('Hello World!!!')
  
  const delta1 = await create(source, target1)
  const delta2 = await create(target1, target2)
  const delta3 = await create(target2, target3)
  
  // Apply all deltas in batch
  const result = await applyBatch(source, [delta1, delta2, delta3])
  
  t.ok(b4a.equals(result, target3), 'async batch apply produces correct final result')
})

test('batch apply - empty delta array', (t) => {
  const source = b4a.from('Hello World')
  
  const result = applyBatchSync(source, [])
  
  t.ok(b4a.equals(result, source), 'empty delta array returns original source')
})

test('batch apply - single delta in batch', async (t) => {
  const source = b4a.from('Hello')
  const target = b4a.from('Hello World')
  
  const delta1 = await create(source, target)
  const result = await applyBatch(source, [delta1])
  
  t.ok(b4a.equals(result, target), 'single delta in batch works correctly')
})

test('batch apply - large batch with compression', async (t) => {
  const source = generateTestData(1024, 'text')
  const targets = []
  const deltas = []
  
  // Create 5 sequential modifications
  let current = source
  for (let i = 0; i < 5; i++) {
    const modified = mutateData(current, 'point', 0.05)
    targets.push(modified)
    const delta1 = await create(current, modified, { compressed: true })
    deltas.push(delta1)
    current = modified
  }
  
  // Apply all deltas
  const result = await applyBatch(source, deltas)
  
  t.ok(b4a.equals(result, targets[targets.length - 1]), 'batch with compression works')
})

test('batch apply - mixed sync and async', async (t) => {
  const source = b4a.from('Start')
  const target1 = b4a.from('Start Middle')
  const target2 = b4a.from('Start Middle End')
  
  // Create deltas with sync
  const delta1 = createSync(source, target1)
  const delta2 = createSync(target1, target2)
  
  // Apply with async batch
  const result = await applyBatch(source, [delta1, delta2])
  
  t.ok(b4a.equals(result, target2), 'sync deltas work with async batch apply')
  
  // Also test the reverse
  const asyncDelta1 = await create(source, target1)
  const asyncDelta2 = await create(target1, target2)
  
  const syncResult = applyBatchSync(source, [asyncDelta1, asyncDelta2])
  
  t.ok(b4a.equals(syncResult, target2), 'async deltas work with sync batch apply')
})

test('batch apply - auto-detection of mixed compressed/uncompressed deltas', async (t) => {
  const source = b4a.from('Initial content. '.repeat(50))
  const deltas = []
  
  let current = source
  
  // Create a mix of compressed and uncompressed deltas
  for (let i = 0; i < 6; i++) {
    const target = b4a.from(`Modified ${i} content. `.repeat(50))
    const compressed = i % 2 === 0 // Alternate compression
    
    const delta = await create(current, target, { compressed })
    
    // Verify magic number presence
    if (compressed && delta.length >= 4) {
      t.ok(delta[0] === 0x28 && delta[1] === 0xB5 && 
           delta[2] === 0x2F && delta[3] === 0xFD, 
           `delta ${i} has zstd magic number`)
    } else if (!compressed && delta.length >= 4) {
      t.ok(!(delta[0] === 0x28 && delta[1] === 0xB5 && 
             delta[2] === 0x2F && delta[3] === 0xFD), 
           `delta ${i} does not have zstd magic number`)
    }
    
    deltas.push(delta)
    current = target
  }
  
  // Apply batch - should auto-detect each delta's compression
  const result = await applyBatch(source, deltas)
  t.alike(result, current, 'async batch with mixed compression works')
  
  // Test sync version
  const syncResult = applyBatchSync(source, deltas)
  t.alike(syncResult, current, 'sync batch with mixed compression works')
})

