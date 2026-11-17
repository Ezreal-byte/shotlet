const assert = require('assert')
const fs = require('fs')
const path = require('path')

const addonPath = process.argv[2] ? path.resolve(process.argv[2]) : [
  path.resolve(__dirname, `../build/native/${process.platform}-${process.arch}/shotlet.node`),
  path.resolve(__dirname, '../build/shotlet.node'),
  path.resolve(__dirname, '../build/Release/shotlet.node'),
].find((candidate) => fs.existsSync(candidate))

assert.ok(addonPath && fs.existsSync(addonPath), 'shotlet.node build output not found')

const addon = require(addonPath)

assert.strictEqual(typeof addon.capture, 'function')
assert.strictEqual(typeof addon.configure, 'function')
assert.doesNotThrow(() => addon.configure({ logDir: require('os').tmpdir() }))
assert.doesNotThrow(() => addon.configure({ logLevel: 'debug', consoleLog: false, consoleLogLevel: 'warn' }))
assert.doesNotThrow(() => addon.configure({ logLevel: 'info' }))
for (const language of ['auto', 'zh-CN', 'en']) {
  assert.doesNotThrow(() => addon.configure({ language }))
}
for (const language of ['', 'fr', 'EN', 42, null, true]) {
  assert.throws(() => addon.configure({ language }), /language/)
}
assert.throws(() => addon.configure({ language: 'zh-CN', consoleLog: 'false' }), /boolean/)
for (const theme of ['auto', 'light', 'dark']) {
  assert.doesNotThrow(() => addon.configure({ theme }))
}
for (const theme of ['', 'LIGHT', 'system', 42, null, true, {}]) {
  assert.throws(() => addon.configure({ theme }), /theme/)
}
assert.throws(() => addon.configure({ theme: 'light', language: 'invalid' }), /language/)
assert.throws(() => addon.configure({ theme: 'dark', consoleLog: 'false' }), /boolean/)
assert.throws(() => addon.configure({ logLevel: 'verbose' }), /logLevel/)
assert.throws(() => addon.configure({ consoleLogLevel: 'verbose' }), /consoleLogLevel/)
assert.throws(() => addon.configure({ consoleLog: 'false' }), /boolean/)
assert.throws(() => addon.configure({ logDir: '' }), /non-empty/)
assert.throws(() => addon.configure(null), /options object/)
assert.throws(() => addon.configure([]), /options object/)
assert.strictEqual(typeof addon.exitCodes, 'object')
assert.strictEqual(addon.exitCodes.success, 0)
assert.strictEqual(addon.exitCodes.error, 1)
assert.strictEqual(addon.exitCodes.cancelled, 2)

// capture() 返回 { code, status, ok, result? }，仅交互完成 Pin/OCR 后包含 result。
