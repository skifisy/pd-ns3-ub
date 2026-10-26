'use strict';
const $ = s => document.querySelector(s);
const COLORS = ['#75889b','#267cae','#8264b3'];
const params = new URLSearchParams(location.search);
const S = {time:0,playing:false,seeking:false,seekVersion:0,resumeAfterSeek:false,focus:null,blocks:new Map(),pending:new Map(),frames:{},scenes:new Map(),lastWall:0,lastDraw:0,key:'',dirty:true,trendKey:'',trendRequest:0,pathBlocks:new Map(),pathPending:new Map(),speed:1,controllers:new Map()};
let meta, recording, completionChart, hotspotChart, globalChart, nodes, leaves, devices, access;
const esc = v => String(v ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const finite = v => typeof v==='number' && Number.isFinite(v);
const number = (v,d=1) => finite(v) ? v.toFixed(d) : '—';
const percent = v => finite(v) ? number(v*100,0)+'%' : '—';
const pair = (a,b) => Math.min(a,b)+'-'+Math.max(a,b);
const direction = (a,b) => a+'-'+b;
const heat = u => !finite(u) ? ['#c6cdd4',.2,.7] : u>=.9 ? ['#df6257',.98,3.7] : u>=.7 ? ['#df9d37',.86,2.8] : u>=.35 ? ['#428ec0',.58,1.8] : ['#a8cfe6',u>0 ? .35 : .14,.9];
function progressAt(c,t){const rows=c.completionBins;let a=0,b=rows.length;while(a<b){const m=(a+b)>>1;if(rows[m].time<=t)a=m+1;else b=m;}return a?rows[a-1]:{completed:0,running:0};}
async function get(url,signal){const r=await fetch(url,{signal});if(!r.ok||r.status===202)throw Error(r.status+': '+await r.text());return r.json();}
function fail(e){if(e.name==='AbortError')return;S.playing=false;$('#play').textContent='开始播放';$('#error').hidden=false;$('#errorText').textContent=e.message||String(e);}
const delay = ms => new Promise(resolve=>setTimeout(resolve,ms));

async function waitReady(){
 for(;;){
  const p=await get('/api/status');
  $('#prepareTitle').textContent=p.phaseLabel||'准备日志';
  $('#prepareCase').textContent=(p.caseId?'当前实验组：'+p.caseId+'　':'')+'已用时 '+number(p.elapsedSeconds,1)+' 秒';
  $('#prepareFile').textContent=p.file||'';
  $('#prepareFill').style.width=finite(p.filePercent)?Math.min(100,p.filePercent)+'%':'0%';
  $('#prepareNumbers').textContent=(finite(p.filePercent)?'当前文件读取 '+number(p.filePercent,1)+'%　':'')+
   (p.fileBytesTotal?'已读 '+number(p.fileBytesRead/1048576,1)+' / '+number(p.fileBytesTotal/1048576,1)+' MiB　':'')+
   '已解析 '+(p.rows||0).toLocaleString()+' 行';
  $('#prepareDetail').textContent='已处理文件 '+(p.filesDone||0)+'　'+
   (p.samples?'本组窗口 '+p.samples+'　':'')+(finite(p.simulationTime)?'仿真时间 '+number(p.simulationTime)+' s。 ':'')+
   (finite(p.casePercent)?'本组日志读取 '+number(p.casePercent,1)+'%。 ':'')+
   '百分比为读取进度，统计与索引完成后才进入回放。';
  if(p.status==='error')throw Error((p.file?p.file+'\n':'')+(p.error||'预处理失败'));
  if(p.status==='ready')return;
  await delay(500);
 }
}
function blockId(t){return Math.floor(Math.max(meta.start,Math.min(t,meta.end-1e-8))/5);}
function cancelLoads(pathsOnly=false){
 for(const [key,controller] of S.controllers){if(!pathsOnly||key.startsWith('path:')){controller.abort();S.controllers.delete(key);}}
 if(!pathsOnly)S.pending.clear();S.pathPending.clear();
}
async function ensureFrames(t){
 const id=blockId(t);if(S.blocks.has(id))return;if(S.pending.has(id))return S.pending.get(id);
 $('#buffer').textContent='缓存中';
 const controller=new AbortController(),requestKey='frame:'+id;S.controllers.set(requestKey,controller);
 const p=get('/api/frames?start='+Math.max(meta.start,id*5)+'&end='+Math.min(meta.end,(id+1)*5),controller.signal).then(data=>{
  S.blocks.set(id,data);S.dirty=true;
  while(S.blocks.size>4){const current=blockId(S.time),keys=[...S.blocks.keys()].filter(k=>k!==current&&k!==id);if(!keys.length)break;S.blocks.delete(keys[0]);}
 }).finally(()=>{if(S.pending.get(id)===p){S.pending.delete(id);S.controllers.delete(requestKey);}});
 S.pending.set(id,p);return p;
}
function pathKey(t){return direction(S.focus.src,S.focus.dst)+'@'+blockId(t);}
async function ensurePaths(t){
 const id=blockId(t),key=pathKey(t);if(S.pathBlocks.has(key))return;if(S.pathPending.has(key))return S.pathPending.get(key);
 const focus={...S.focus};
 const controller=new AbortController(),requestKey='path:'+key;S.controllers.set(requestKey,controller);
 const p=get('/api/paths?src='+focus.src+'&dst='+focus.dst+'&start='+Math.max(meta.start,id*5)+'&end='+Math.min(meta.end,(id+1)*5),controller.signal).then(data=>{
  S.pathBlocks.set(key,data);S.dirty=true;
  while(S.pathBlocks.size>4){const keys=[...S.pathBlocks.keys()].filter(k=>k!==pathKey(S.time)&&k!==key);if(!keys.length)break;S.pathBlocks.delete(keys[0]);}
 }).finally(()=>{if(S.pathPending.get(key)===p){S.pathPending.delete(key);S.controllers.delete(requestKey);}});
 S.pathPending.set(key,p);return p;
}
async function ensure(t){await Promise.all([ensureFrames(t),ensurePaths(t)]);$('#buffer').textContent='数据就绪';}
function topologyAt(c,t){return [...c.topologyStates].reverse().find(s=>s.time<=t)||c.topologyStates[0];}
function frameAt(c,t){
 const rows=S.blocks.get(blockId(t))?.cases[c.id]||[];
 const frame=rows.find(r=>r.start<=t&&r.end>t);if(frame){const detail=S.pathBlocks.get(pathKey(t))?.cases[c.id]?.[String(frame.id)]||[];return {...frame,nodePaths:detail};}
 const top=topologyAt(c,t),event=[...c.events].reverse().find(e=>e.time<=t);
 return {id:t>=c.end?'finished':'missing',start:t,end:t,topologyEpoch:top.epoch,teEpoch:event?.teEpoch??0,phase:t>=c.end?'finished':'missing',edges:top.edges,pathSamples:[],nodePaths:[],global:{}};
}
function detailAvailable(c,t){return c.capabilities.nodePaths&&(c.detailWindows===null||c.detailWindows.some(w=>w[0]<=t&&t<w[1]));}
function focusedPaths(frame){return (frame.nodePaths||[]).filter(p=>p.src===S.focus.src&&p.dst===S.focus.dst&&p.payloadBytes>0);}
function midpoints(c,frame){
 if(c.capabilities.nodePaths)return [...new Set(focusedPaths(frame).filter(p=>p.transit>=0).map(p=>p.transit))];
 if(c.hasPathSamples)return [...new Set((frame.pathSamples||[]).filter(p=>p.src===S.focus.src&&p.dst===S.focus.dst&&p.transit>=0&&p.payloadBytes>0).map(p=>p.transit))];
 return [...new Set(c.routeIntervals.filter(r=>r.src===S.focus.src&&r.dst===S.focus.dst&&r.transit>=0&&r.start<=S.time&&r.end>S.time).map(r=>r.transit))];
}
function changeAt(c,t){
 const groups=c.changes.filter(g=>(g.planTime??g.time)<=t);if(!groups.length)return null;
 const g=groups.at(-1),applied=g.time!=null&&g.time<=t;
 return {...g,applied,failed:!applied&&c.events.some(e=>e.event==='toe-drain-timeout'&&e.toeEpoch===g.epoch&&e.time<=t)};
}
function setup(){
 $('#globalCards').innerHTML=meta.cases.map((c,i)=>'<article class="global-card" style="--case:'+COLORS[i%3]+'"><div class="card-heading"><h2>'+esc(c.label)+'</h2><span>最终 FCT · 固定共同完成任务集</span></div><div class="quantiles">'+['p50','p90','p99'].map(k=>{
  const q=c.quantiles[k],gain=c.mode==='ecmp'?'基线':q.improvement==null?'无共同完成任务':(q.improvement>=0?'降低 ':'增加 ')+number(Math.abs(q.improvement))+'%';
  return '<div><div class="metric-label">'+k.toUpperCase()+'</div><div class="metric-value">'+number(q.seconds,3)+'<small>s</small></div><div class="gain '+(q.improvement<0?'regression':'')+'">'+gain+'</div></div>';
 }).join('')+'</div><div class="live-summary"><span>当前完成 <b id="complete-'+i+'">—</b> / '+c.totalTasks+'</span><span>进行中 <b id="running-'+i+'">—</b></span></div></article>').join('');
 $('#networks').innerHTML=meta.cases.map((c,i)=>'<article class="network-row" style="--case:'+COLORS[i%3]+'"><div class="case-side"><div><div class="case-name">'+esc(c.label)+'</div><div class="case-purpose">同一仿真时刻</div></div><div class="global-live"><div><label>最大利用率</label><b id="max-'+i+'">—</b></div><div><label>高负载方向</label><b id="hot-'+i+'">—</b><small>互连利用率 ≥ '+number(recording.hotspotThreshold*100,0)+'%</small></div></div></div><div class="network-space"><canvas class="network" id="network-'+i+'" aria-label="'+esc(c.label)+' 链路热力拓扑"></canvas><span class="network-watermark">上排 L＝交换机　下排 C＝计算 / S＝存储　接入间距已压缩</span></div><div class="action-side"><div class="action-label" id="action-label-'+i+'"></div><div class="action-title" id="action-title-'+i+'"></div><div class="action-lines" id="action-lines-'+i+'"></div><div class="action-footer" id="action-footer-'+i+'"></div></div></article>').join('');
 $('#focus').innerHTML=meta.focusOptions.map(p=>'<option value="'+direction(p.src,p.dst)+'">L'+p.src+' → L'+p.dst+'</option>').join('');
 $('#focus').value=direction(S.focus.src,S.focus.dst);$('#focus').disabled=false;
 $('#focus').onchange=e=>{const [src,dst]=e.target.value.split('-').map(Number);cancelLoads(true);S.focus={src,dst};S.dirty=true;ensurePaths(S.time).catch(fail);loadHotspot().catch(fail);};
 $('#focus').onpointerdown=updateOptions;
 $('#focus').onfocus=updateOptions;
}
function updateOptions(){
 const maps=meta.cases.map(c=>new Map((S.frames[c.id]?.edges||[]).map(e=>[direction(e.src,e.dst),e])));
 for(const option of $('#focus').options){
  const [src,dst]=option.value.split('-');
  option.textContent='L'+src+' → L'+dst+'　'+meta.cases.map((c,i)=>c.label+' '+percent(maps[i].get(option.value)?.utilization)).join(' / ');
 }
}
function layout(w,h){
 const left=28,right=w-22,leafY=h-72,deviceY=h-25,positions=new Map();
 leaves.forEach((id,i)=>positions.set(id,[left+(right-left)*i/Math.max(1,leaves.length-1),leafY]));
 devices.forEach((id,i)=>positions.set(id,[left+(right-left)*i/Math.max(1,devices.length-1),deviceY]));
 return {positions,leafY,deviceY};
}
function curve(a,b,positions,isFabric,focal){
 const pa=positions.get(a),pb=positions.get(b);
 const lift=isFabric?Math.min(pa[1]-22,(focal?38:13)+Math.abs(pb[0]-pa[0])*(focal ? .052 : .045)+(Math.min(a,b)%3)*5):0;
 return {a:pa,b:pb,c:[(pa[0]+pb[0])/2,(pa[1]+pb[1])/2-2*lift]};
}
function at(g,t){const u=1-t;return [u*u*g.a[0]+2*u*t*g.c[0]+t*t*g.b[0],u*u*g.a[1]+2*u*t*g.c[1]+t*t*g.b[1]];}
function stroke(ctx,g,color,width,opacity=1,dashed=false){
 ctx.save();ctx.globalAlpha=opacity;ctx.strokeStyle=color;ctx.lineWidth=width;ctx.setLineDash(dashed?[3,4]:[]);
 ctx.beginPath();ctx.moveTo(...g.a);ctx.quadraticCurveTo(...g.c,...g.b);ctx.stroke();ctx.restore();
}
function arrow(ctx,g,color,reverse=false){
 const p=at(g,reverse ? .18 : .82),q=at(g,reverse ? .2 : .8),angle=Math.atan2(p[1]-q[1],p[0]-q[0]);
 ctx.save();ctx.translate(...p);ctx.rotate(angle);ctx.fillStyle=color;ctx.beginPath();ctx.moveTo(3,0);ctx.lineTo(-3,-2);ctx.lineTo(-3,2);ctx.closePath();ctx.fill();ctx.restore();
}
function badge(ctx,text,x,y,color){
 ctx.save();ctx.font='9px ui-monospace, monospace';const width=ctx.measureText(text).width+8;
 const canvasWidth=ctx.canvas.width/(ctx.getTransform().a||1);
 x=Math.max(width/2+3,Math.min(canvasWidth-width/2-3,x));
 ctx.fillStyle='rgba(252,253,254,.96)';ctx.fillRect(x-width/2,y-6,width,13);ctx.fillStyle=color;ctx.textAlign='center';ctx.textBaseline='middle';ctx.fillText(text,x,y);ctx.restore();
}
function makeScene(c,frame,i){
 const canvas=$('#network-'+i),bounds=canvas.parentElement.getBoundingClientRect();
 const w=Math.round(bounds.width),h=Math.round(bounds.height),dpr=Math.min(window.devicePixelRatio||1,2);
 if(w<=0||h<=0){S.scenes.delete(c.id);return;}
 const width=Math.round(w*dpr),height=Math.round(h*dpr);
 if(canvas.width!==width)canvas.width=width;
 if(canvas.height!==height)canvas.height=height;
 const buffer=document.createElement('canvas');buffer.width=canvas.width;buffer.height=canvas.height;
 const ctx=buffer.getContext('2d');ctx.scale(dpr,dpr);ctx.lineCap='round';
 const {positions}=layout(w,h),observed=new Map(frame.edges.map(e=>[direction(e.src,e.dst),e]));
 const paths=focusedPaths(frame),mids=midpoints(c,frame),focusKeys=new Set([pair(S.focus.src,S.focus.dst)]),flow=new Map(),nodeRates=new Map();
 const addFlow=(a,b,rate)=>{const k=direction(a,b),old=flow.get(k);flow.set(k,{a,b,rate:(old?.rate||0)+rate});focusKeys.add(pair(a,b));};
 for(const p of paths){
  addFlow(p.srcNode,p.src,p.gbps);addFlow(p.dst,p.dstNode,p.gbps);
  if(p.transit<0)addFlow(p.src,p.dst,p.gbps);else{addFlow(p.src,p.transit,p.gbps);addFlow(p.transit,p.dst,p.gbps);}
  for(const [id,key] of [[p.srcNode,'tx'],[p.dstNode,'rx']]){const n=nodeRates.get(id)||{tx:0,rx:0};n[key]+=p.gbps;nodeRates.set(id,n);}
 }
 if(!c.capabilities.nodePaths){
  const hops=[[S.focus.src,S.focus.dst],...mids.flatMap(m=>[[S.focus.src,m],[m,S.focus.dst]])];
  for(const [a,b] of hops){focusKeys.add(pair(a,b));const e=observed.get(direction(a,b));if(e?.gbps>0)addFlow(a,b,e.gbps);}
 }
 const bundles=new Map();
 for(const e of topologyAt(c,S.time).edges)if(e.src<e.dst&&leaves.includes(e.src)&&leaves.includes(e.dst))bundles.set(pair(e.src,e.dst),{...e,fabric:true});
 for(const e of access)bundles.set(pair(e.src,e.dst),{...e,fabric:false});
 const change=changeAt(c,S.time),show=change&&!change.failed&&(!change.applied||S.time-change.time<Math.max(8,(recording.end-recording.start)*.025));
 const changes=new Map(show?change.changes.map(d=>[pair(d.src,d.dst),d]):[]);
 for(const [k,d] of changes)if(!bundles.has(k))bundles.set(k,{src:d.src,dst:d.dst,fabric:true,circuits:change.applied?d.after:d.before});
 const geometries=new Map();
 const sorted=[...bundles].map(([k,e])=>{
  const f=observed.get(direction(e.src,e.dst)),r=observed.get(direction(e.dst,e.src));
  const measured=(r?.utilization??-1)>(f?.utilization??-1)?r:f;
  return {k,e,measured,u:measured?.utilization};
 }).sort((a,b)=>(a.u??-1)-(b.u??-1));
 for(const {k,e,measured,u} of sorted){
  const focal=focusKeys.has(k),g=curve(e.src,e.dst,positions,e.fabric,focal);geometries.set(k,g);
  const [color,opacity,width]=heat(u),deleted=changes.has(k)&&!e.circuits;
  if(focal)stroke(ctx,g,'#244c69',width+(e.fabric?3:2),.35);
  if(!deleted)stroke(ctx,g,color,(.7+3.3*Math.log1p(measured?.gbps||0)/Math.log1p(Math.max(1,meta.rateScaleGbps)))*(e.fabric?1:.65),focal?Math.max(.65,opacity):opacity,!finite(u));
  if(finite(u)&&u>=.7&&measured)arrow(ctx,g,color,measured.src===e.dst);
 }
 // Focus labels keep the selected direction explicit; background heat remains max(A→B,B→A).
 const hops=[[S.focus.src,S.focus.dst],...mids.slice(0,2).flatMap(m=>[[S.focus.src,m],[m,S.focus.dst]])];
 hops.forEach(([a,b],index)=>{
  const g=geometries.get(pair(a,b));if(!g)return;
  const e=observed.get(direction(a,b)),p=at(g,index===0 ? .5 : .42);
  const text='L'+a+'→L'+b+' '+percent(e?.utilization)+(index===0?' · '+number(e?.gbps,0)+'/'+number(e?.capacity,0)+'G':'');
  if(e?.gbps>0)arrow(ctx,g,'#244c69',positions.get(a)!==g.a);
  badge(ctx,text,p[0],p[1]+(index%2?9:-9),'#294f6d');
 });
 for(const [k,d] of changes){
  const g=geometries.get(k);if(!g)continue;
  const p=at(g,.64),color=d.delta>0?'#11896c':'#cb5b51';
  if(!change.applied||d.after===0)stroke(ctx,g,color,1.7,.8,true);
  badge(ctx,(d.delta>0?'+':'')+d.delta+' '+d.before+'→'+d.after+(change.applied?'':' 待'),p[0],p[1]+10,color);
 }
 const topRates=new Set([...nodeRates].sort((a,b)=>(b[1].tx+b[1].rx)-(a[1].tx+a[1].rx)).slice(0,5).map(([id])=>id));
 for(const n of nodes){
  const [x,y]=positions.get(n.id),upper=leaves.includes(n.id),active=upper?(n.id===S.focus.src||n.id===S.focus.dst||mids.includes(n.id)):nodeRates.has(n.id);
  ctx.save();ctx.fillStyle=active?'#244c69':upper?'#edf3f7':n.role==='storage-server'?'#e3daef':'#d7eaf4';ctx.strokeStyle=active?'#173a55':upper?'#8ca6b9':n.role==='storage-server'?'#9a83b6':'#75a6c0';ctx.lineWidth=active?1.8:1;
  ctx.beginPath();if(upper)ctx.rect(x-10,y-6,20,12);else if(n.role==='storage-server')ctx.rect(x-5,y-5,10,10);else ctx.arc(x,y,5,0,Math.PI*2);ctx.fill();ctx.stroke();
  ctx.textAlign='center';ctx.fillStyle=active?'#24445b':'#7c91a0';ctx.font=(upper?'10':'9')+'px ui-monospace,monospace';
  ctx.fillText((upper?'L':n.role==='storage-server'?'S':'C')+n.id,x,y+17);
  if(topRates.has(n.id)){const rate=nodeRates.get(n.id);ctx.font='8px ui-monospace,monospace';ctx.fillText((rate.tx?'↑'+number(rate.tx,0):'')+(rate.tx&&rate.rx?' / ':'')+(rate.rx?'↓'+number(rate.rx,0):'')+'G',x,y-10);}
  ctx.restore();
 }
 const routes=[];
 for(const f of flow.values()){
  const base=geometries.get(pair(f.a,f.b));if(!base||f.rate<=0)continue;
  const g=positions.get(f.a)===base.a?base:{a:base.b,b:base.a,c:base.c};
  routes.push({g,rate:f.rate,color:heat(observed.get(direction(f.a,f.b))?.utilization)[0]});
 }
 S.scenes.set(c.id,{canvas,buffer,dpr,routes});
}
function paint(wall){
 for(const scene of S.scenes.values()){
  const ctx=scene.canvas.getContext('2d');ctx.setTransform(1,0,0,1,0,0);ctx.clearRect(0,0,scene.canvas.width,scene.canvas.height);ctx.drawImage(scene.buffer,0,0);ctx.save();ctx.scale(scene.dpr,scene.dpr);
  for(const r of scene.routes){
   const n=Math.min(4,Math.max(1,Math.ceil(r.rate/400)));
   for(let j=0;j<n;j++){const t=(S.time*.19+j/n)%1,p=at(r.g,t);ctx.beginPath();ctx.fillStyle=r.color;ctx.arc(p[0],p[1],2,0,Math.PI*2);ctx.fill();}
  }ctx.restore();
 }
}
function action(c,frame,i){
 const f=S.focus,e=frame.edges.find(e=>e.src===f.src&&e.dst===f.dst),reverse=frame.edges.find(e=>e.src===f.dst&&e.dst===f.src),paths=focusedPaths(frame),mids=midpoints(c,frame),change=changeAt(c,S.time);
 $('#action-label-'+i).textContent='焦点 L'+f.src+' → L'+f.dst+'　·　'+percent(e?.utilization);
 const payload=paths.reduce((n,p)=>n+p.gbps,0);
 let title=mids.length?'实际中转路径':'直达方向与接入流量',lines='';
 if(c.mode.startsWith('toe')&&change){
  title=change.failed?'重配未生效':change.applied?'重配已生效':'重配待生效';
  const rotate=rows=>{if(rows.length<=2)return rows;const at=Math.floor(Math.max(0,S.time-(change.planTime??change.time))/2)%rows.length;return [rows[at],rows[(at+1)%rows.length]];};
  lines='<div>'+(!change.applied?'计划：':'已变更：')+'增加 '+change.added+' / 减少 '+change.removed+' 条</div>'+
   [...rotate(change.changes.filter(d=>d.delta>0)),...rotate(change.changes.filter(d=>d.delta<0))].map(d=>'<div class="'+(d.delta>0?'added':'removed')+'">L'+d.src+'↔L'+d.dst+' '+(d.delta>0?'+':'')+d.delta+'　'+d.before+'→'+d.after+'</div>').join('');
  if(change.failed)lines='<div>排空超时；实际连接和容量保持原值。</div>';
 }else{
  lines=mids.slice(0,2).map(m=>'<div class="route-label">L'+f.src+' → L'+m+' → L'+f.dst+'</div>').join('');
  if(!mids.length)lines='<div>观察 L'+f.src+' → L'+f.dst+'</div>';
  if(paths.length){
   const largest=[...paths].sort((a,b)=>b.gbps-a.gbps).slice(0,2);
   lines+='<div>该方向节点载荷发送 '+number(payload,0)+' Gbps</div>'+largest.map(p=>'<div>节点 '+p.srcNode+' → '+p.dstNode+'　'+number(p.gbps,0)+' Gbps</div>').join('');
  }else lines+='<div>'+(detailAvailable(c,S.time)?'本窗口无该方向载荷记录':'此时间段未采集端到端路径详情')+'</div>';
 }
 const phase={steady:'正常转发',draining:'排空',guard:'保护等待',applied:'拓扑生效',finished:'实验结束',missing:'无采样'}[frame.phase]||frame.phase;
 $('#action-title-'+i).textContent=title;$('#action-lines-'+i).innerHTML=lines;
 $('#action-footer-'+i).innerHTML='窗口 '+number(frame.start,3)+'–'+number(frame.end,3)+' s<br>链路发送 <b>'+number(e?.gbps,0)+'</b> / 容量 <b>'+number(e?.capacity,0)+'</b> Gbps<br>反向 '+percent(reverse?.utilization)+'　·　'+esc(phase)+'　·　拓扑 v'+frame.topologyEpoch+'<br>'+(c.capabilities.accessSamples?'接入层有链路采样':'接入层无采样，保留灰色结构线');
}
function draw(){
 meta.cases.forEach((c,i)=>{
  const frame=frameAt(c,S.time);S.frames[c.id]=frame;makeScene(c,frame,i);action(c,frame,i);
  $('#max-'+i).textContent=percent(frame.global?.maxUtilization);
  $('#hot-'+i).textContent=frame.global?.hotLinks??'—';
 });
}
function cursor(){return {silent:true,symbol:'none',label:{show:false},lineStyle:{color:'#a2b4c2',type:'dashed',width:1},data:[{xAxis:S.time}]};}
function chartBase(){
 return {animation:false,tooltip:{show:false},grid:{left:39,right:10,top:13,bottom:20},xAxis:{type:'value',min:recording.start,max:recording.end,axisLabel:{fontSize:8,color:'#8c9daa',formatter:v=>v.toFixed(0)+'s'},splitLine:{show:false},axisLine:{lineStyle:{color:'#dce6ec'}}},yAxis:{type:'value',min:0,splitNumber:2,axisLabel:{fontSize:8,color:'#8c9daa'},splitLine:{lineStyle:{color:'#edf2f5'}}}};
}
function line(id,data,color,dashed=false){return {id,type:'line',silent:true,showSymbol:false,step:'end',connectNulls:false,lineStyle:{color,width:dashed?1:1.6,type:dashed?'dashed':'solid'},data};}
function drawCompletion(){
 completionChart.setOption({...chartBase(),series:meta.cases.map((c,i)=>({...line(c.id,c.completionBins.map(p=>[p.time,p.completed]),COLORS[i%3]),markLine:i===0?cursor():undefined}))});
}
async function loadHotspot(){
 const f=S.focus,k=direction(f.src,f.dst);if(k===S.trendKey)return;
 S.trendKey=k;const request=++S.trendRequest;const data=await get('/api/trend?src='+f.src+'&dst='+f.dst);if(request!==S.trendRequest)return;
 $('#hotspotTitle').textContent='焦点 L'+f.src+' → L'+f.dst;
 hotspotChart.setOption({...chartBase(),series:meta.cases.flatMap((c,i)=>[
  {...line(c.id+'-rate',data.cases[c.id].map(p=>[p.time,p.gbps]),COLORS[i%3]),markLine:i===0?cursor():undefined},
  line(c.id+'-cap',data.cases[c.id].map(p=>[p.time,p.capacity]),COLORS[i%3],true)
 ])},{notMerge:true});
}
async function loadGlobal(){
 const data=await get('/api/global-trend'),base=chartBase();
 globalChart.setOption({...base,grid:{left:34,right:28,top:13,bottom:20},yAxis:[{...base.yAxis,max:v=>Math.max(100,Math.ceil(v.max/20)*20)},{...base.yAxis,position:'right',splitLine:{show:false}}],series:meta.cases.flatMap((c,i)=>[
  {...line(c.id+'-max',data.cases[c.id].map(p=>[p.time,finite(p.maxUtilization)?p.maxUtilization*100:null]),COLORS[i%3]),markLine:i===0?cursor():undefined},
  {...line(c.id+'-hot',data.cases[c.id].map(p=>[p.time,p.hotLinks]),COLORS[i%3],true),yAxisIndex:1}
 ])});
}
function updateText(){
 $('#time').textContent=number(S.time,1);
 meta.cases.forEach((c,i)=>{$('#complete-'+i).textContent=progressAt(c,S.time).completed;$('#running-'+i).textContent=progressAt(c,S.time).running;});
 const parts=meta.cases.map(c=>{const f=S.frames[c.id],e=f?.edges.find(e=>e.src===S.focus.src&&e.dst===S.focus.dst);return c.label+' '+percent(e?.utilization);});
 const selected=$('#focus').selectedOptions[0];
 if(selected)selected.textContent='L'+S.focus.src+' → L'+S.focus.dst+'　'+parts.join(' / ');
 $('#caption').textContent='L'+S.focus.src+' → L'+S.focus.dst+'：'+parts.join(' / ')+'。比较原热点、中转负载和其他热路径。';
 if(!S.seeking)$('#timeline').value=S.time;
 $('#currentClock').textContent=number(S.time,1)+' s';
 $('#play').textContent=S.playing?'暂停播放':S.time>=recording.end?'重新播放':'开始播放';
 $('#playbackState').textContent=S.seeking?'正在定位':S.time>=recording.end?'回放完成':S.playing?'播放中':'已暂停';
 completionChart.setOption({series:[{id:meta.cases[0].id,markLine:cursor()}]});
 hotspotChart.setOption({series:[{id:meta.cases[0].id+'-rate',markLine:cursor()}]});
 globalChart.setOption({series:[{id:meta.cases[0].id+'-max',markLine:cursor()}]});
}
function setPlaying(value){S.playing=value;S.lastWall=performance.now();updateText();}
async function seekTo(time,resume=false){
 const version=++S.seekVersion;cancelLoads();
 S.time=Math.max(recording.start,Math.min(recording.end,time));S.playing=false;S.lastWall=performance.now();S.dirty=true;
 $('#buffer').textContent='正在定位';updateText();
 await ensure(S.time);
 if(version!==S.seekVersion)return;
 draw();paint();updateText();S.dirty=true;$('#buffer').textContent='数据就绪';
 if(resume&&S.time<recording.end)setPlaying(true);
}
function controls(){
 const timeline=$('#timeline');timeline.min=recording.start;timeline.max=recording.end;timeline.value=S.time;
 $('#speed').onchange=e=>{S.speed=Number(e.target.value);S.lastWall=performance.now();};
 $('#endClock').textContent=number(recording.end,1)+' s';
 $('#play').disabled=false;$('#restart').disabled=false;timeline.disabled=false;
 $('#play').onclick=()=>{
  S.seeking=false;S.resumeAfterSeek=false;++S.seekVersion;
  if(S.time>=recording.end)seekTo(recording.start,true).catch(fail);else setPlaying(!S.playing);
 };
 $('#restart').onclick=()=>{S.seeking=false;S.resumeAfterSeek=false;seekTo(recording.start,true).catch(fail);};
 timeline.oninput=()=>{
  if(!S.seeking){S.resumeAfterSeek=S.playing;S.seeking=true;}
  seekTo(Number(timeline.value)).catch(fail);
 };
 const finish=()=>{
  if(!S.seeking)return;
  const resume=S.resumeAfterSeek;S.seeking=false;S.resumeAfterSeek=false;
  seekTo(Number(timeline.value),resume).catch(fail);
 };
 timeline.onchange=finish;timeline.onblur=finish;timeline.onpointercancel=finish;
}
function tick(wall){
 const dt=S.lastWall?Math.min(.15,(wall-S.lastWall)/1000):0;S.lastWall=wall;
 if(S.playing){
  let remaining=dt,next=S.time;
  while(remaining>0&&next<recording.end){
   const segment=recording.segments.find(s=>s.start<=next&&s.end>next)||recording.segments.at(-1),toEnd=(segment.end-next)/(S.speed);
   if(toEnd<=remaining){next=segment.end;remaining-=toEnd;}else{next+=remaining*S.speed;remaining=0;}
  }
  next=Math.min(next,recording.end);
  if(S.blocks.has(blockId(next))&&S.pathBlocks.has(pathKey(next))){S.time=next;if(next>=recording.end){if(params.get('loop')==='1'){S.time=recording.start;ensure(S.time).catch(fail);}else S.playing=false;}}else ensure(next).catch(fail);
  const id=blockId(S.time);if(S.time>(id+1)*5-2)ensure(Math.min(recording.end-1e-8,(id+1)*5+.001)).catch(fail);
 }
 if(wall-S.lastDraw>140){
  const frames=meta.cases.map(c=>frameAt(c,S.time)),key=frames.map(f=>f.id).join('|')+'|'+direction(S.focus.src,S.focus.dst)+'|'+Math.floor(S.time*2);
  if(S.dirty||key!==S.key){draw();S.key=key;S.dirty=meta.cases.some(c=>!S.scenes.has(c.id));}
  updateText();S.lastDraw=wall;
 }
 paint(wall);requestAnimationFrame(frameLoop);
}
function frameLoop(wall){try{tick(wall);}catch(error){fail(error);}}
async function init(){
 if(!window.echarts)throw Error('本地 ECharts 文件未加载');
 await waitReady();meta=await get('/api/bootstrap');recording=meta.recording;S.time=recording.start;S.focus={...meta.focus};
 nodes=meta.topology.nodes;leaves=meta.topology.leaves;devices=nodes.filter(n=>!leaves.includes(n.id)).map(n=>n.id);
 const groups=new Map();
 for(const e of meta.topology.links)if(e.kind==='access'){const k=pair(e.src,e.dst),g=groups.get(k)||{src:Math.min(e.src,e.dst),dst:Math.max(e.src,e.dst),circuits:0};g.circuits++;groups.set(k,g);}access=[...groups.values()];
 $('#sourceLabel').textContent=meta.synthetic?'SMOKE 合成数据 · 非实验结果':'实际实验日志';
 $('#sourceLabel').classList.toggle('warning',meta.synthetic);
 $('#topologyInfo').textContent=nodes.length+' 节点 · '+leaves.length+' Leaf · '+meta.topology.fabricCircuits+' OCS';
 $('#cohort').textContent='500 ms 统计；最终 P50/P90/P99：共同完成 '+meta.commonTasks+' 个任务；未完成：'+meta.cases.map(c=>c.label+' '+(c.totalTasks-c.completedTasks)).join(' / ')+'。节点路径为源 Leaf 窗口发送量。';
 $('#completedLegend').innerHTML=meta.cases.map((c,i)=>'<span style="color:'+COLORS[i%3]+'">● '+esc(c.label)+'</span>').join('　');
 setup();completionChart=echarts.init($('#completionChart'));hotspotChart=echarts.init($('#hotspotChart'));globalChart=echarts.init($('#globalChart'));drawCompletion();
 await Promise.all([ensure(S.time),loadHotspot(),loadGlobal()]);
 await new Promise(resolve=>requestAnimationFrame(resolve));
 draw();paint();controls();updateText();updateOptions();
 window.addEventListener('resize',()=>{completionChart.resize();hotspotChart.resize();globalChart.resize();S.dirty=true;});
 if(window.ResizeObserver){const observer=new ResizeObserver(()=>{S.dirty=true;});document.querySelectorAll('.network-space').forEach(el=>observer.observe(el));}
 $('#preparing').hidden=true;setPlaying(params.get('autoplay')==='1');requestAnimationFrame(frameLoop);
}
init().catch(fail);
