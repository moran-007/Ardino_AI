#pragma once

#include "memo_manager.h"

namespace MemoWeb {
template <typename Page, typename Escape>
inline void append(Page &page, Escape escape) {
  const bool startTodo = MemoManager::count(MemoManager::Kind::Note) == 0 &&
                         MemoManager::count(MemoManager::Kind::Todo) > 0;
  page += F("<style>.memo-switch{display:grid;grid-template-columns:1fr 1fr;gap:4px;padding:4px;background:#edf2f5;border:1px solid #dbe3e9;border-radius:8px;margin:4px 0 20px}.memo-switch button{width:100%;min-width:0;border:0;border-radius:6px;background:transparent;color:#526577;padding:10px 6px;box-shadow:none}.memo-switch button[aria-selected=true]{background:#fff;color:#156f68;box-shadow:0 1px 3px #172a351a}.memo-count{display:inline-block;margin-left:6px;font-size:12px}.memo-item{padding:16px 0;border-bottom:1px solid #e2e8ee}.memo-item:first-child{padding-top:0}.memo-head{display:flex;justify-content:space-between;gap:12px;align-items:center;font-size:12px;color:#697c8b}.memo-content{margin:10px 0;white-space:pre-wrap;overflow-wrap:anywhere}.memo-item[data-done=true] .memo-content{color:#758693;text-decoration:line-through}.memo-add{margin-top:20px}.memo-view[hidden]{display:none!important}.memo-empty{padding:22px 0;color:#748594;text-align:center}.memo-item .actions{margin:8px 0 0;display:flex;flex-wrap:wrap;gap:8px}</style>");
  page += F("<fieldset id='memos'><legend>便签与待办</legend><div class='memo-switch' role='tablist' aria-label='便签与待办'>");
  for (int kind = 0; kind < 2; ++kind) {
    const bool todo = kind == 1;
    const char *key = todo ? "todo" : "note";
    const auto memoKind = todo ? MemoManager::Kind::Todo : MemoManager::Kind::Note;
    page += "<button type='button' role='tab' id='memo-tab-" + String(key) + "' aria-controls='memo-view-" + key +
            "' aria-selected='" + (todo == startTodo ? "true" : "false") + "' tabindex='" + (todo == startTodo ? "0" : "-1") + "'>";
    page += todo ? F("待办") : F("便签");
    page += "<span class='memo-count'>" + String(MemoManager::count(memoKind)) + "</span></button>";
  }
  page += F("</div>");
  for (int kind = 0; kind < 2; ++kind) {
    const bool todo = kind == 1;
    const char *key = todo ? "todo" : "note";
    const auto memoKind = todo ? MemoManager::Kind::Todo : MemoManager::Kind::Note;
    page += "<div class='memo-view' role='tabpanel' id='memo-view-" + String(key) + "' aria-labelledby='memo-tab-" + key + "'";
    if (todo != startTodo) page += F(" hidden");
    page += F(">");
    if (todo) page += "<p class='meta'>未完成 " + String(MemoManager::pendingTodoCount()) + " 条</p>";
    size_t ordinal = 0;
    MemoManager::Entry entry;
    for (size_t i = 0; i < MemoManager::count(); ++i) {
      if (!MemoManager::get(i, entry) || entry.kind != memoKind) continue;
      ++ordinal;
      page += "<article class='memo-item' data-done='" + String(entry.completed ? "true" : "false") + "'><div class='memo-head'><span>第 " + String(ordinal) + " 条</span>";
      if (todo) page += entry.completed ? F("<span class='ok'>已完成</span>") : F("<span class='warn'>未完成</span>");
      page += F("</div><p class='memo-content'>");
      page += escape(entry.text);
      page += F("</p><div class='actions'>");
      if (todo) {
        page += "<form method='post' action='/memos/toggle'><input type='hidden' name='id' value='" + String(entry.id) + "'><input type='hidden' name='completed' value='" + (entry.completed ? "0" : "1") + "'><button class='secondary' type='submit'>";
        page += entry.completed ? F("恢复") : F("完成");
        page += F("</button></form>");
      }
      page += "<form method='post' action='/memos/delete'><input type='hidden' name='id' value='" + String(entry.id) + "'><button class='danger' type='submit'>删除</button></form></div></article>";
    }
    if (!ordinal) page += todo ? F("<p class='memo-empty'>暂无待办</p>") : F("<p class='memo-empty'>暂无便签</p>");
    page += "<form class='memo-add' method='post' action='/memos/add'><input type='hidden' name='kind' value='" + String(key) + "'><label for='memo-add-" + key + "'>";
    page += todo ? F("新增待办") : F("新增便签");
    page += "</label><input id='memo-add-" + String(key) + "' name='text' maxlength='180' required><p><button type='submit'>添加</button></p></form>";
    page += F("<details><summary>批量清理</summary><form method='post' action='/memos/clear' onsubmit='return confirm(&quot;确认清理所选内容？此操作不可撤销。&quot;)'>");
    if (todo) page += F("<label>清理范围</label><select name='scope'><option value='completed'>已完成待办</option><option value='todos'>全部待办</option></select>");
    else page += F("<input type='hidden' name='scope' value='notes'>");
    page += todo ? F("<p><button class='danger' type='submit'>清理待办</button></p>") : F("<p><button class='danger' type='submit'>清空便签</button></p>");
    page += F("</form></details></div>");
  }
  page += F("</fieldset><script>(()=>{const tabs=Array.from(document.querySelectorAll('#memos [role=tab]'));function select(tab,focus){tabs.forEach(item=>{const on=item===tab;item.setAttribute('aria-selected',String(on));item.tabIndex=on?0:-1;document.getElementById(item.getAttribute('aria-controls')).hidden=!on;});if(focus)tab.focus();try{sessionStorage.setItem('memo-tab',tab.id);}catch(e){}}tabs.forEach((tab,index)=>{tab.addEventListener('click',()=>select(tab,false));tab.addEventListener('keydown',event=>{let next;if(event.key==='ArrowRight')next=(index+1)%tabs.length;else if(event.key==='ArrowLeft')next=(index+tabs.length-1)%tabs.length;else if(event.key==='Home')next=0;else if(event.key==='End')next=tabs.length-1;else return;event.preventDefault();select(tabs[next],true);});});try{const saved=tabs.find(tab=>tab.id===sessionStorage.getItem('memo-tab'));if(saved)select(saved,false);}catch(e){}})();</script>");
}
}  // namespace MemoWeb
