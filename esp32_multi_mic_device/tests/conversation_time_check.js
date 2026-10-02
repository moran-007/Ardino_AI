const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'esp32_cloud_ota_device.ino'), 'utf8');
const prefixes = ['function esc(', 'const chatTimeFormat=', 'function chatTime(',
  'function bubble(', 'async function refreshConversation('];
const scripts = source.split(/\r?\n/).flatMap(line => {
  const match = line.match(/^\s*("(?:[^"\\]|\\.)*")\s*$/);
  if (!match) return [];
  const value = JSON.parse(match[1]);
  return prefixes.some(prefix => value.startsWith(prefix)) ? [value] : [];
});
assert.equal(scripts.length, prefixes.length);
const script = scripts.join('\n');
const at = Date.parse('2026-10-01T05:30:00Z') / 1000;
const records = [
  {user: '今天天气怎么样？', assistant: '今天有小雨。', user_at: at, assistant_at: at + 8},
  {user: '旧记录', assistant: '没有历史时间数据', at_ms: 1000},
  {user: '未联网时的记录', assistant: '时间尚未同步', user_at: 0, assistant_at: 0},
];
const current = {current_user_text: '你好，能听见吗？', current_assistant_text: '能听见。',
  current_user_at: at + 60, current_assistant_at: at + 65};
const elements = {'current-chat': {}, 'history-chat': {}};
const context = {Intl, Date, document: {getElementById: id => elements[id]},
  adminFetch: async () => ({ok: true, text: async () => records.map(x => JSON.stringify(x)).join('\n')})};
vm.createContext(context);
vm.runInContext(script, context);

async function main() {
  assert.equal(context.chatTime(at), '2026-10-01 13:30:00');
  assert.equal(context.chatTime(undefined), '时间未记录');
  for (const value of [0, null, -1, 'bad', Infinity, 8640000000001]) {
    assert.equal(context.chatTime(value), '时间未同步');
  }
  assert.equal(context.bubble('user', '', '你', at), '');
  assert.ok(context.bubble('user', '<img onerror=alert(1)>', '你', at).includes('&lt;img'));
  await context.refreshConversation(current);
  assert.ok(elements['current-chat'].innerHTML.includes('2026-10-01 13:31:00'));
  assert.ok(elements['current-chat'].innerHTML.includes('2026-10-01 13:31:05'));
  const history = elements['history-chat'].innerHTML;
  assert.ok(history.includes('2026-10-01 13:30:00'));
  assert.ok(history.includes('2026-10-01 13:30:08'));
  assert.equal((history.match(/时间未记录/g) || []).length, 2);
  assert.equal((history.match(/时间未同步/g) || []).length, 2);
  await context.refreshConversation(current);
  assert.equal(elements['history-chat'].innerHTML, history);
  await context.refreshConversation({});
  assert.ok(elements['current-chat'].innerHTML.includes('等待新一轮对话'));
  console.log('PASS: production chat timestamps, timezone, old records, refresh and escaping');

  if (process.argv.includes('--fixture')) {
    const header = fs.readFileSync(path.join(root, 'web_admin_ui.h'), 'utf8').match(/R"HTML\(([\s\S]*?)\)HTML"/)[1];
    const html = header + '<fieldset><legend>当前对话</legend><div id="current-chat" class="chat"></div></fieldset>' +
      '<fieldset><legend>历史记录</legend><div id="history-chat" class="chat"></div></fieldset></main></div><script>' +
      `const records=${JSON.stringify(records)};const current=${JSON.stringify(current)};` +
      'async function adminFetch(){return {ok:true,text:async()=>records.map(x=>JSON.stringify(x)).join("\\n")};}' +
      script + '\nrefreshConversation(current);</script></body></html>';
    const output = path.resolve(root, '../output/playwright/conversation-time.html');
    fs.mkdirSync(path.dirname(output), {recursive: true});
    fs.writeFileSync(output, html);
    console.log(output);
  }
}
main().catch(error => {console.error(error); process.exitCode = 1;});
