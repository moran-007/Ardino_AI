const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '..', 'offline_wake.cpp'), 'utf8');
const condition = source.match(/wakeInterface->detect\(wakeModel,\s*samples\)\s*(==\s*WAKENET_DETECTED)/);
if (!condition || !/if\s*\(detected\)\s*\{/.test(source)) throw Error('Official wake predicate or dispatch condition not found');
const triggers = new Function('result', 'WAKENET_DETECTED', 'WAKENET_CHANNEL_VERIFIED',
  `return result ${condition[1]};`);
for (const [result, expected] of [[1, true], [-1, false], [0, false]]) {
  if (triggers(result, 1, -1) !== expected) throw Error(`Incorrect wake event: ${result}`);
}
console.log('PASS: actual detection triggers; channel verification and no-detect do not');
