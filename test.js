const test = require('brittle')
const b4a = require('b4a')
const delta = require('./index')

test('create and apply delta - simple text', async (t) => {
  const source = b4a.from('Hello world!')
  const target = b4a.from('Hello Bare world!')
  
  // Create delta
  const diff = await delta.create(source, target)
  t.ok(diff instanceof Buffer, 'delta.create returns a buffer')
  t.ok(diff.length > 0, 'delta has content')
  
  // Apply delta
  const result = await delta.apply(source, diff)
  t.ok(result instanceof Buffer, 'delta.apply returns a buffer')
  t.alike(result, target, 'applied delta produces correct target')
})

test('createSync and applySync - simple text', (t) => {
  const source = b4a.from('The quick brown fox')
  const target = b4a.from('The quick brown fox jumps over the lazy dog')
  
  // Create delta synchronously
  const diff = delta.createSync(source, target)
  t.ok(diff instanceof Buffer, 'createSync returns a buffer')
  t.ok(diff.length > 0, 'delta has content')
  
  // Apply delta synchronously
  const result = delta.applySync(source, diff)
  t.ok(result instanceof Buffer, 'applySync returns a buffer')
  t.alike(result, target, 'applied delta produces correct target')
})

test('binary data', async (t) => {
  // Create some binary data
  const source = Buffer.alloc(256)
  const target = Buffer.alloc(256)
  
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
  const source = Buffer.alloc(size)
  const target = Buffer.alloc(size)
  
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
  
  // Deltas might not be exactly identical due to implementation details,
  // but applying them should produce the same result
  const asyncResult = await delta.apply(source, asyncDiff)
  const syncResult = delta.applySync(source, syncDiff)
  
  t.alike(asyncResult, target, 'async result is correct')
  t.alike(syncResult, target, 'sync result is correct')
  t.alike(asyncResult, syncResult, 'async and sync produce same result')
})