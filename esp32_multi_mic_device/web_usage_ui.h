#pragma once
#include <Arduino.h>

namespace UsageWeb {
template <typename Page>
inline void append(Page &page) {
  page += F(R"HTML(<h3>本设备用量与费用</h3>
<div style='display:flex;align-items:end;gap:10px;flex-wrap:wrap'><div><label for='usage-period'>统计周期</label><select id='usage-period'><option value='30d'>近 30 天</option><option value='7d'>近 7 天</option><option value='24h'>近 24 小时</option><option value='all'>累计</option></select></div><button id='usage-load' type='button'>读取用量</button></div>
<p id='usage-status' role='status' aria-live='polite'></p><div id='usage-result' hidden><p id='usage-total'></p><label for='usage-grain'>价格曲线（人民币）</label><select id='usage-grain'><option value='day'>日</option><option value='week'>周</option><option value='month'>月</option></select><svg id='usage-chart' viewBox='0 0 640 210' role='img' aria-label='已计价费用曲线' style='width:100%;max-height:220px'></svg><p id='usage-chart-note' class='meta'></p><div style='overflow-x:auto'><table style='width:100%;border-collapse:collapse;font-size:13px'><thead><tr><th>日期</th><th>使用量</th><th>单价</th><th title='费用（人民币）'>总价格</th></tr></thead><tbody id='usage-rows'></tbody></table></div><p id='usage-note' class='meta'></p></div>
<style>#usage-rows td,#usage-result th{text-align:left;padding:8px;border-bottom:1px solid #cbd8d5;vertical-align:top;overflow-wrap:anywhere}#usage-result table{min-width:500px}#usage-rows td:first-child{white-space:nowrap}#usage-rows td:last-child{font-variant-numeric:tabular-nums}@media(max-width:600px){#usage-result table{min-width:0}#usage-result table,#usage-rows{display:block}#usage-result thead{display:none}#usage-rows tr{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);border-bottom:1px solid #cbd8d5;padding:8px 0}#usage-rows td{border:0;padding:4px}#usage-rows td:nth-child(2),#usage-rows td:nth-child(3){grid-column:1/-1}#usage-rows td:nth-child(4){grid-column:2;grid-row:1;text-align:right;font-weight:600}#usage-rows td:nth-child(3):before{content:'单价：'}}</style>
<script>(()=>{const $=id=>document.getElementById(id),button=$('usage-load'),status=$('usage-status'),ns='http://www.w3.org/2000/svg';let report=null;
const cost=value=>value==null?'暂无计价金额':value+' 元';
function draw(){const rows=report?.cost_series?.[$('usage-grain').value]||[],svg=$('usage-chart');svg.replaceChildren();
const values=rows.map(r=>r.cost!=null?Number(r.cost):r.calls===0?0:null),known=values.filter(v=>v!==null),max=Math.max(...known,0.000001);
function add(tag,attrs,text){const el=document.createElementNS(ns,tag);for(const k of Object.keys(attrs))el.setAttribute(k,attrs[k]);if(text!=null)el.textContent=text;svg.append(el);return el;}
add('line',{x1:65,y1:175,x2:625,y2:175,stroke:'#cbd8d5'});add('text',{x:4,y:22,fill:'#6c7c7a','font-size':11},Number(max.toPrecision(5))+' 元');let points=[];const flush=()=>{if(points.length)add('polyline',{points:points.join(' '),fill:'none',stroke:'#168e83','stroke-width':2});points=[];};
rows.forEach((row,i)=>{const value=values[i];if(value===null){flush();return;}const x=65+(rows.length<2?280:i*560/(rows.length-1)),y=175-value/max*145;points.push(x+','+y);const dot=add('circle',{cx:x,cy:y,r:3,fill:'#168e83'}),title=document.createElementNS(ns,'title');title.textContent=row.date+'：'+(row.cost||'0')+' 元'+(row.unpriced_calls?'（含未计价调用）':'')+(row.partial_calls?'（费用不完整）':'');dot.append(title);});flush();
if(rows.length){add('text',{x:65,y:202,fill:'#6c7c7a','font-size':11},rows[0].date);add('text',{x:625,y:202,'text-anchor':'end',fill:'#6c7c7a','font-size':11},rows.at(-1).date);}
$('usage-chart-note').textContent=(known.length?'按北京时间汇总，未计价时留空；指向圆点查看金额。':'所选周期暂无人民币计价金额。')+(report?.series_truncated?' 曲线显示最近一段历史，合计含全部记录。':'');}
$('usage-grain').onchange=draw;
button.onclick=async()=>{button.disabled=true;$('usage-period').disabled=true;$('usage-result').hidden=true;status.textContent='正在读取…';
try{const r=await adminFetch('/ai/usage?period='+encodeURIComponent($('usage-period').value));if(!r.ok)throw new Error(await r.text());const d=await r.json();report=d;
$('usage-rows').replaceChildren(...(d.expense_rows||[]).map(row=>{const tr=document.createElement('tr'),quantity=row.kind==='llm'?'输入 '+row.input_tokens+'／输出 '+row.output_tokens+' Token':row.quantity+row.unit;
const warning=(row.unpriced_calls?'；'+row.unpriced_calls+' 次未计价':'')+(row.unreported_calls?'；'+row.unreported_calls+' 次未回传用量':'')+(row.partial_calls?'；'+row.partial_calls+' 次费用不完整':'');
const values=[row.date,row.model+' · '+quantity,row.unit_price,cost(row.cost_cny)+warning];values.forEach(value=>{const td=document.createElement('td');td.textContent=value;tr.append(td);});return tr;}));
$('usage-total').textContent='已计价总额：'+cost(d.totals.total)+(d.unpriced_calls?'；'+d.unpriced_calls+' 次未计价':'')+(d.partial_calls?'；'+d.partial_calls+' 次费用不完整':'')+(d.foreign_cost?'；非人民币未计入':'');$('usage-note').textContent='单价取调用时记录；金额保留两位小数，曲线保留原始精度。未扣赠送额度，实际扣费以厂商账单为准。'+(d.omitted_expense_rows?' 表格显示最近 '+d.expense_rows.length+' 项，完整历史请在服务器查看。':'');draw();
status.textContent='已更新 · '+new Date(d.updated_at).toLocaleString('zh-CN');$('usage-result').hidden=false;
}catch(e){status.textContent='读取失败：'+e.message;}finally{button.disabled=false;$('usage-period').disabled=false;}};
})();</script>)HTML");
}
}
