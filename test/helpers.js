const b4a = require('b4a')

// Generate different types of test data to measure delta efficiency
function generateTestData(sizeInBytes, dataType = 'text') {
  switch (dataType) {
    case 'text':
      // Realistic text-like data with repetitive patterns
      const pattern = 'This is line content with some text and numbers 123456789\n'
      const patternBuffer = b4a.from(pattern)
      const numRepeats = Math.ceil(sizeInBytes / patternBuffer.length)
      
      const fullBuffer = b4a.allocUnsafe(numRepeats * patternBuffer.length)
      for (let i = 0; i < numRepeats; i++) {
        patternBuffer.copy(fullBuffer, i * patternBuffer.length)
      }
      return fullBuffer.subarray(0, sizeInBytes)
    
    case 'binary':
      // Binary data with some structure
      const buffer = b4a.allocUnsafe(sizeInBytes)
      for (let i = 0; i < sizeInBytes; i++) {
        buffer[i] = (i * 37) % 256
      }
      return buffer
    
    case 'structured':
      // JSON-like structured data
      const jsonTemplate = {
        id: 'item_PLACEHOLDER_000',
        name: 'Sample Item Name',
        description: 'This is a sample description for testing purposes',
        values: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
        metadata: { version: '1.0.0', created: '2024-01-01', updated: '2024-01-02' }
      }
      const templateStr = JSON.stringify(jsonTemplate)
      const templateBuffer = b4a.from(templateStr)
      
      // Create consistent-sized records by padding the ID field
      const recordSize = templateBuffer.length
      const numRecords = Math.ceil(sizeInBytes / recordSize)
      
      const result = b4a.allocUnsafe(numRecords * recordSize)
      for (let i = 0; i < numRecords; i++) {
        const paddedId = `item_${i.toString().padStart(10, '0')}_000`
        const record = { ...jsonTemplate, id: paddedId }
        const recordStr = JSON.stringify(record)
        const recordBuffer = b4a.from(recordStr)
        
        // Pad or truncate to exact record size
        if (recordBuffer.length >= recordSize) {
          recordBuffer.copy(result, i * recordSize, 0, recordSize)
        } else {
          recordBuffer.copy(result, i * recordSize)
          // Fill remaining bytes with spaces
          result.fill(32, i * recordSize + recordBuffer.length, (i + 1) * recordSize)
        }
      }
      return result.subarray(0, sizeInBytes)
    
    case 'random':
      // Truly random data (worst case for compression)
      const randomBuffer = b4a.allocUnsafe(sizeInBytes)
      for (let i = 0; i < sizeInBytes; i++) {
        randomBuffer[i] = Math.floor(Math.random() * 256)
      }
      return randomBuffer
    
    default:
      throw new Error(`Unknown data type: ${dataType}`)
  }
}

// Different mutation strategies to test delta efficiency
function mutateData(original, mutationType, mutationSize = 0.1) {
  switch (mutationType) {
    case 'point': {
      // Small point mutations throughout the file
      const result = b4a.allocUnsafe(original.length)
      original.copy(result)
      
      const numMutations = Math.floor(original.length * mutationSize)
      for (let i = 0; i < numMutations; i++) {
        const pos = Math.floor(Math.random() * original.length)
        result[pos] = (result[pos] + 1) % 256
      }
      return result
    }
    
    case 'insert': {
      // Insert new data at random positions
      const insertData = b4a.from('INSERTED_')
      const numInserts = Math.floor(original.length * mutationSize / insertData.length)
      const totalInsertSize = numInserts * insertData.length
      const result = b4a.allocUnsafe(original.length + totalInsertSize)
      
      // Generate sorted insert positions to avoid position shifting issues
      const insertPositions = []
      for (let i = 0; i < numInserts; i++) {
        insertPositions.push(Math.floor(Math.random() * original.length))
      }
      insertPositions.sort((a, b) => a - b)
      
      let srcPos = 0
      let destPos = 0
      
      for (let i = 0; i < insertPositions.length; i++) {
        const insertPos = insertPositions[i] + (i * insertData.length) // Adjust for previous insertions
        const actualInsertPos = Math.min(insertPos, srcPos + (original.length - srcPos))
        
        // Copy original data up to insert position
        const copyLen = actualInsertPos - srcPos
        if (copyLen > 0) {
          original.copy(result, destPos, srcPos, srcPos + copyLen)
          destPos += copyLen
          srcPos += copyLen
        }
        
        // Insert new data
        insertData.copy(result, destPos)
        destPos += insertData.length
      }
      
      // Copy remaining original data
      if (srcPos < original.length) {
        original.copy(result, destPos, srcPos)
        destPos += original.length - srcPos
      }
      
      return result.subarray(0, destPos)
    }
    
    case 'delete': {
      // Delete chunks of data
      const chunkSize = Math.max(1, Math.floor(original.length * mutationSize / 10))
      const numDeletes = Math.min(10, Math.floor(original.length / chunkSize))
      
      if (numDeletes === 0) return b4a.from(original)
      
      // Generate non-overlapping delete positions
      const deleteRanges = []
      for (let i = 0; i < numDeletes; i++) {
        const start = Math.floor(Math.random() * (original.length - chunkSize))
        const end = Math.min(start + chunkSize, original.length)
        deleteRanges.push({ start, end })
      }
      
      // Sort and merge overlapping ranges
      deleteRanges.sort((a, b) => a.start - b.start)
      const mergedRanges = []
      let currentRange = deleteRanges[0]
      
      for (let i = 1; i < deleteRanges.length; i++) {
        const range = deleteRanges[i]
        if (range.start <= currentRange.end) {
          // Merge overlapping ranges
          currentRange.end = Math.max(currentRange.end, range.end)
        } else {
          mergedRanges.push(currentRange)
          currentRange = range
        }
      }
      mergedRanges.push(currentRange)
      
      // Calculate result size
      let totalDeleted = 0
      for (const range of mergedRanges) {
        totalDeleted += range.end - range.start
      }
      
      const result = b4a.allocUnsafe(original.length - totalDeleted)
      let srcPos = 0
      let destPos = 0
      
      for (const range of mergedRanges) {
        // Copy data before deleted range
        if (srcPos < range.start) {
          const copyLen = range.start - srcPos
          original.copy(result, destPos, srcPos, range.start)
          destPos += copyLen
        }
        srcPos = range.end // Skip deleted section
      }
      
      // Copy remaining data
      if (srcPos < original.length) {
        original.copy(result, destPos, srcPos)
        destPos += original.length - srcPos
      }
      
      return result.subarray(0, destPos)
    }
    
    case 'move': {
      // Move blocks of data around - simplified implementation
      const result = b4a.allocUnsafe(original.length)
      original.copy(result)
      
      const blockSize = Math.max(10, Math.floor(original.length * 0.01))
      const numMoves = Math.min(3, Math.floor(original.length / blockSize / 4))
      
      for (let i = 0; i < numMoves; i++) {
        const sourceStart = Math.floor(Math.random() * (original.length - blockSize))
        const destStart = Math.floor(Math.random() * (original.length - blockSize))
        
        if (sourceStart !== destStart) {
          // Simple swap operation
          const tempBlock = b4a.allocUnsafe(blockSize)
          result.copy(tempBlock, 0, sourceStart, sourceStart + blockSize)
          result.copy(result, sourceStart, destStart, destStart + blockSize)
          tempBlock.copy(result, destStart)
        }
      }
      
      return result
    }
    
    case 'replace': {
      // Replace chunks with different data
      const result = b4a.allocUnsafe(original.length)
      original.copy(result)
      
      const chunkSize = Math.max(10, Math.floor(original.length * mutationSize / 5))
      const numReplacements = Math.min(5, Math.floor(original.length / chunkSize))
      
      for (let i = 0; i < numReplacements; i++) {
        const pos = Math.floor(Math.random() * (original.length - chunkSize))
        const replacement = b4a.from(`REPLACE${i}_`.repeat(Math.ceil(chunkSize / 9)))
        const copyLen = Math.min(chunkSize, replacement.length, original.length - pos)
        replacement.copy(result, pos, 0, copyLen)
      }
      
      return result
    }
    
    default:
      throw new Error(`Unknown mutation type: ${mutationType}`)
  }
}

module.exports = {
  generateTestData,
  mutateData
}