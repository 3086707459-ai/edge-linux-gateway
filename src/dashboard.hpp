#pragma once
namespace edge {
inline constexpr const char* dashboard = R"HTML(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Edge Linux Gateway</title>
<style>
:root{color-scheme:dark;font:16px system-ui;background:#101823;color:#e2ebf3}body{max-width:1060px;margin:auto;padding:32px 20px}header{display:flex;justify-content:space-between;gap:16px;align-items:center}h1{font-size:26px;margin:8px 0}small,.muted{color:#98abc0}#badge{padding:8px 14px;border-radius:20px;background:#243750}section{display:grid;grid-template-columns:repeat(3,1fr);gap:16px;margin:24px 0}.card{background:#1a2636;border:1px solid #304055;border-radius:14px;padding:22px}.value{font-size:38px;font-weight:650;margin:12px 0}canvas{width:100%;height:210px}.wide{grid-column:1/-1}table{width:100%;border-collapse:collapse;font-size:14px}td,th{text-align:left;padding:10px 6px;border-bottom:1px solid #304055}input,button{background:#243750;color:inherit;border:1px solid #526983;border-radius:6px;padding:9px}input{max-width:260px}#message{color:#ffce85;min-height:24px}footer{font-size:13px;line-height:1.8;color:#98abc0}@media(max-width:650px){section{grid-template-columns:1fr}.wide{grid-column:auto}header{display:block}.value{font-size:32px}}
</style>
<header><div><small>EMBEDDED LINUX / DEVICE TELEMETRY</small><h1>设备采集与监控网关</h1></div><span id="badge">连接中</span></header>
<p class="muted">串口采集 · 本地持久化 · 温度告警</p>
<details><summary>API 访问令牌</summary><p><input id="token" type="password" autocomplete="off" placeholder="启用鉴权时填入 Bearer token"> <button id="connect">连接</button></p><small>令牌只保存在当前页面内存，刷新后清除。远程使用请通过 SSH 隧道或 HTTPS 代理。</small></details>
<p id="message"></p>
<section><div class="card"><small>温度 / °C</small><div class="value" id="temp">--</div><small id="updated">等待首次采集</small></div>
<div class="card"><small>相对湿度 / %RH</small><div class="value" id="humidity">--</div><small id="source">数据源 --</small></div>
<div class="card"><small>温度告警</small><div class="value" id="alarm">--</div><small id="saved">持久化 --</small></div>
<div class="card wide"><small>最近温度曲线 / °C</small><canvas id="chart"></canvas></div>
<div class="card wide"><small>最近告警与恢复记录</small><table><thead><tr><th>时间</th><th>事件</th><th>温度 °C</th></tr></thead><tbody id="events"></tbody></table></div></section>
<footer>模拟模式按周期变化，用于演示告警与恢复。历史曲线只包含已写入数据库的样本。<br>设备状态、重连与存储错误可通过 /api/v1/status 和 /metrics 查看。</footer>
<script>
let token='',busy=false;
const el=id=>document.getElementById(id);
const time=n=>new Date(n).toLocaleString();
async function get(path){const r=await fetch(path,{headers:token?{Authorization:'Bearer '+token}:{},cache:'no-store'});if(!r.ok)throw Error(r.status===401?'请填写正确的 API 访问令牌':'接口返回 '+r.status);return r.json()}
function plot(rows){const c=el('chart'),dpr=window.devicePixelRatio||1,w=c.clientWidth,h=210;c.width=w*dpr;c.height=h*dpr;const g=c.getContext('2d');g.scale(dpr,dpr);g.clearRect(0,0,w,h);if(!rows.length)return;const data=rows.slice().reverse().map(s=>s.temperature_c),low=Math.floor(Math.min(...data)-2),high=Math.ceil(Math.max(...data)+2),x=i=>42+i*(w-56)/Math.max(1,data.length-1),y=v=>h-24-(v-low)*(h-45)/(high-low);g.font='12px system-ui';g.fillStyle='#98abc0';g.strokeStyle='#304055';for(let j=0;j<4;j++){const v=low+(high-low)*j/3;g.fillText(v.toFixed(1),2,y(v)+4);g.beginPath();g.moveTo(38,y(v));g.lineTo(w,y(v));g.stroke()}g.strokeStyle='#72e5c0';g.lineWidth=2;g.beginPath();data.forEach((v,i)=>i?g.lineTo(x(i),y(v)):g.moveTo(x(i),y(v)));g.stroke()}
async function refresh(){if(busy)return;busy=true;try{const [s,rows,events]=await Promise.all([get('/api/v1/status'),get('/api/v1/samples?limit=100'),get('/api/v1/alarms?limit=10')]);el('badge').textContent=s.healthy?'运行正常':'采集或存储异常';el('badge').style.color=s.healthy?'#72e5c0':'#ffce85';el('source').textContent='数据源 '+s.source;el('saved').textContent='本次已保存 '+s.persisted+' · 待写入 '+s.queue_depth;el('message').textContent=s.storage_dropped||s.storage_errors?'存储丢弃 '+s.storage_dropped+'，写入失败 '+s.storage_errors+'，请检查设备日志。':'';const v=s.latest;if(v){el('temp').textContent=v.temperature_c.toFixed(1);el('humidity').textContent=v.humidity_pct.toFixed(1);el('alarm').textContent=v.alarm?'高温':'正常';el('alarm').style.color=v.alarm?'#ff9c91':'#72e5c0';el('updated').textContent=time(v.timestamp_ms)+(s.stale?' · 数据过期':'')}el('events').replaceChildren(...events.map(e=>{const tr=document.createElement('tr');[time(e.timestamp_ms),e.active?'高温告警':'温度恢复',e.temperature_c.toFixed(1)].forEach(v=>{const td=document.createElement('td');td.textContent=v;tr.append(td)});return tr}));plot(rows)}catch(e){el('badge').textContent='连接异常';el('message').textContent=e.message}finally{busy=false}}
el('connect').onclick=()=>{token=el('token').value;el('token').value='';refresh()};window.addEventListener('resize',refresh);refresh();setInterval(refresh,1500);
</script></html>)HTML";
}
