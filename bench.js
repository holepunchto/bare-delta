const { create, apply, createSync, applySync } = require('.')
const b4a = require('b4a')
const process = require('bare-process')
const { generateTestData, mutateData } = require('./test/helpers')

// Extended benchmark with performance analysis
const scenarios = [
  { name: 'Tiny files', size: 512, dataType: 'binary', mutations: 0.01 },
  { name: 'Small files', size: 4 * 1024, dataType: 'binary', mutations: 0.05 },
  { name: 'Medium files', size: 64 * 1024, dataType: 'binary', mutations: 0.05 }, 
  { name: 'Large files', size: 256 * 1024, dataType: 'binary', mutations: 0.05 },
  { name: 'XL files', size: 1024 * 1024, dataType: 'binary', mutations: 0.05 },
  { name: 'Structured low', size: 16 * 1024, dataType: 'structured', mutations: 0.01 },
  { name: 'Structured med', size: 16 * 1024, dataType: 'structured', mutations: 0.05 },
  { name: 'Random data', size: 32 * 1024, dataType: 'random', mutations: 0.05 },
  { name: 'High mutation', size: 64 * 1024, dataType: 'binary', mutations: 0.20 }
]

async function benchmark(scenario, compressed = false) {
  const original = generateTestData(scenario.size, scenario.dataType)
  const modified = mutateData(original, 'point', scenario.mutations)
  
  const options = compressed ? { compressed: true } : {}
  const suffix = compressed ? ' (comp)' : ''
  
  // Time delta creation
  const createStart = process.hrtime.bigint()
  const delta = await create(original, modified, options)
  const createEnd = process.hrtime.bigint()
  
  // Time delta application
  const applyStart = process.hrtime.bigint()
  const result = await apply(original, delta, options)
  const applyEnd = process.hrtime.bigint()
  
  if (!b4a.equals(result, modified)) {
    throw new Error(`VERIFICATION FAILED: ${scenario.name}${suffix}`)
  }
  
  const createTime = Number(createEnd - createStart) / 1000000
  const applyTime = Number(applyEnd - applyStart) / 1000000
  const totalTime = createTime + applyTime
  
  const ratio = (delta.length / original.length) * 100
  const createThroughput = (scenario.size / 1024 / 1024) / (createTime / 1000)
  const applyThroughput = (scenario.size / 1024 / 1024) / (applyTime / 1000)
  
  const mutationRate = scenario.mutations * 100
  const nameWithSuffix = (scenario.name + suffix).padEnd(15)
  
  console.log(`${nameWithSuffix} ${(scenario.size/1024).toFixed(0).padStart(5)}KB  ${mutationRate.toFixed(0).padStart(3)}%  ${createTime.toFixed(1).padStart(5)}ms  ${applyTime.toFixed(1).padStart(4)}ms  ${createThroughput.toFixed(0).padStart(4)}MB/s  ${applyThroughput.toFixed(0).padStart(4)}MB/s  ${ratio.toFixed(1).padStart(5)}%`)
  
  return { delta, createTime, applyTime, ratio }
}

async function run() {
  console.log('Scenario         Size  Mut  Create Apply CreateMB/s ApplyMB/s Delta')
  console.log('===================================================================')
  
  const compressionResults = []
  
  for (const scenario of scenarios) {
    // Run uncompressed benchmark
    const uncompressed = await benchmark(scenario, false)
    
    // Run compressed benchmark
    const compressed = await benchmark(scenario, true)
    
    // Calculate compression effectiveness
    const compressionRatio = (compressed.delta.length / uncompressed.delta.length) * 100
    const createOverhead = ((compressed.createTime / uncompressed.createTime) - 1) * 100
    const applyOverhead = ((compressed.applyTime / uncompressed.applyTime) - 1) * 100
    
    compressionResults.push({
      name: scenario.name,
      size: scenario.size,
      dataType: scenario.dataType,
      compressionRatio,
      createOverhead,
      applyOverhead,
      originalDelta: uncompressed.ratio,
      compressedDelta: compressed.ratio
    })
  }
  
  console.log('\n=== COMPRESSION ANALYSIS ===')
  console.log('Scenario         Size  Type        Compression  CreateOH  ApplyOH   Original  Compressed')
  console.log('====================================================================================')
  
  for (const result of compressionResults) {
    const sizeMB = (result.size / 1024).toFixed(0).padStart(4)
    const type = result.dataType.padEnd(10)
    const compRatio = `${result.compressionRatio.toFixed(1)}%`.padStart(8)
    const createOH = `${result.createOverhead >= 0 ? '+' : ''}${result.createOverhead.toFixed(0)}%`.padStart(8)
    const applyOH = `${result.applyOverhead >= 0 ? '+' : ''}${result.applyOverhead.toFixed(0)}%`.padStart(8)
    const originalDelta = `${result.originalDelta.toFixed(1)}%`.padStart(8)
    const compressedDelta = `${result.compressedDelta.toFixed(1)}%`.padStart(10)
    
    console.log(`${result.name.padEnd(15)} ${sizeMB}KB  ${type} ${compRatio} ${createOH} ${applyOH}  ${originalDelta}  ${compressedDelta}`)
  }
  
  console.log('\nPerformance Analysis:')
  console.log('- Create throughput: Speed of delta generation')  
  console.log('- Apply throughput: Speed of delta application')
  console.log('- Delta %: Size ratio vs original file (lower = better compression)')
  console.log('- Compression: Compressed delta size vs uncompressed delta (lower = better)')
  console.log('- CreateOH/ApplyOH: Performance overhead for compression (lower = better)')
  console.log('- Expected binary diff performance: 50-200 MB/s create, 100-500 MB/s apply')
}

run().catch(err => {
  console.error('BENCHMARK FAILED:', err.message)
  process.exit(1)
})