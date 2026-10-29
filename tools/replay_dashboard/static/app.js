'use strict';
const $ = s => document.querySelector(s);
const COLORS = ['#75889b','#267cae','#8264b3'];
const TRANSIT_COLORS = ['#7852bb','#5369ba','#a3479a','#42699b','#77548e','#447f88'];
const params = new URLSearchParams(location.search);
const S = {time:0,playing:false,buffering:false,bufferSince:0,totalBufferMs:0,seeking:false,seekVersion:0,resumeAfterSeek:false,focus:null,blocks:new Map(),pending:new Map(),frames:{},scenes:new Map(),lastWall:0,lastDraw:0,lastPaint:0,lastPrefetch:0,lastStats:0,key:'',dirty:true,trendKey:'',trendRequest:0,pathBlocks:new Map(),pathPending:new Map(),pathRetryAt:new Map(),frameRetryAt:new Map(),speed:1,controllers:new Map(),heat:true,loadVersion:0,frameLatencyMs:0,pathLatencyMs:0,trendStart:null,trendEnd:null,globalRequest:0,completionRequest:0};
let meta, recording, completionChart, hotspotChart, globalChart, throughputChart, utilizationChart, nodes, leaves, devices, access;
const esc = v => String(v ?? '').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const finite = v => typeof v==='number' && Number.isFinite(v);
const number = (v,d=1) => finite(v) ? v.toFixed(d) : '—';
const percent = v => finite(v) ? number(v*100,0)+'%' : '—';
const pair = (a,b) => Math.min(a,b)+'-'+Math.max(a,b);
const direction = (a,b) => a+'-'+b;
const heat = u => !finite(u) ? ['#c6cdd4',.2,.7] : u>=.9 ? ['#df6257',.98,3.7] : u>=.7 ? ['#df9d37',.86,2.8] : u>=.35 ? ['#428ec0',.58,1.8] : ['#a8cfe6',u>0 ? .35 : .14,.9];
const transitColor = id => {const index=Math.max(0,leaves.indexOf(id));return TRANSIT_COLORS[index]||'hsl('+(205+100*(index-6)/Math.max(1,leaves.length-6))+',55%,44%)';};
function progressAt(c,t){const rows=S.blocks.get(blockId(t))?.progress?.[c.id]||[];let a=0,b=rows.length;while(a<b){const m=(a+b)>>1;if(rows[m].time<=t+1e-12)a=m+1;else b=m;}return a?rows[a-1]:{completed:0,running:0};}
async function get(url,signal){const r=await fetch(url,{signal});if(!r.ok||r.status===202)throw Error(r.status+': '+await r.text());const raw=await r.text(),data=JSON.parse(raw);Object.defineProperty(data,'_bytes',{value:new TextEncoder().encode(raw).byteLength});return data;}
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
function blockId(t){return Math.floor(Math.max(meta.start,Math.min(t,meta.end-1e-8))/meta.blockSeconds);}
function trimCache(cache,currentKey,newKey,budget,maxBlocks){
 const size=()=>[...cache.values()].reduce((n,v)=>n+(v._bytes||0),0);
 const index=k=>Number(String(k).split('@').at(-1)),current=index(currentKey);
 while(cache.size>maxBlocks||size()>budget){
   // Keep the currently displayed and just-arrived blocks; prefer future over history.
   const candidates=[...cache.keys()].filter(k=>k!==currentKey&&k!==newKey);
   if(!candidates.length)break;
   candidates.sort((a,b)=>{
      const distance=k=>{const d=index(k)-current;return d<0?-d*2:d;};
      return distance(b)-distance(a);
   });
   cache.delete(candidates[0]);
 }
}
function cancelLoads(pathsOnly=false){
 if(!pathsOnly)++S.loadVersion;
 for(const [key,controller] of S.controllers){if(!pathsOnly||key.startsWith('path:')){controller.abort();S.controllers.delete(key);}}
 if(!pathsOnly)S.pending.clear();
 S.pathPending.clear();
 if(pathsOnly){S.pathBlocks.clear();S.pathRetryAt.clear();}
}
function rememberLatency(field,start){const ms=performance.now()-start;S[field]=S[field] ? S[field]*.8+ms*.2 : ms;}
async function ensureFrames(t){
 const id=blockId(t);if(S.blocks.has(id))return;if(S.pending.has(id))return S.pending.get(id);
 const controller=new AbortController(),requestKey='frame:'+id,version=S.loadVersion,start=performance.now();S.controllers.set(requestKey,controller);
 const p=get('/api/frames?start='+Math.max(meta.start,id*meta.blockSeconds)+'&end='+Math.min(meta.end,(id+1)*meta.blockSeconds),controller.signal).then(data=>{
   if(controller.signal.aborted||version!==S.loadVersion)return;
   rememberLatency('frameLatencyMs',start);S.blocks.set(id,data);S.frameRetryAt.delete(id);S.dirty=true;
   trimCache(S.blocks,blockId(S.time),id,32*1024*1024,12);
 }).finally(()=>{if(S.pending.get(id)===p){S.pending.delete(id);S.controllers.delete(requestKey);}});
 S.pending.set(id,p);return p;
}
function pathKey(t){return direction(S.focus.src,S.focus.dst)+'@'+blockId(t);}
async function ensurePaths(t){
 const id=blockId(t),key=pathKey(t);if(S.pathBlocks.has(key))return;if(S.pathPending.has(key))return S.pathPending.get(key);
 const focus={...S.focus};
 const controller=new AbortController(),requestKey='path:'+key,version=S.loadVersion,start=performance.now();S.controllers.set(requestKey,controller);
 const p=get('/api/paths?src='+focus.src+'&dst='+focus.dst+'&start='+Math.max(meta.start,id*meta.blockSeconds)+'&end='+Math.min(meta.end,(id+1)*meta.blockSeconds),controller.signal).then(data=>{
   if(controller.signal.aborted||version!==S.loadVersion||key!==direction(S.focus.src,S.focus.dst)+'@'+id)return;
   rememberLatency('pathLatencyMs',start);S.pathBlocks.set(key,data);S.pathRetryAt.delete(key);S.dirty=true;
   trimCache(S.pathBlocks,pathKey(S.time),key,16*1024*1024,8);
 }).finally(()=>{if(S.pathPending.get(key)===p){S.pathPending.delete(key);S.controllers.delete(requestKey);}});
 S.pathPending.set(key,p);return p;
}
function pathFailure(key,error){
 if(error.name==='AbortError')return;
 S.pathRetryAt.set(key,performance.now()+3000);
 if(params.get('debug')==='1')console.warn('Replay path fetch failed; will retry',key,error);
}
function requestPath(t){
 const key=pathKey(t);
 if(S.pathBlocks.has(key)||performance.now()<(S.pathRetryAt.get(key)||0))return;
 ensurePaths(t).catch(e=>pathFailure(key,e));
}
function prefetchAhead(t){
 const current=blockId(t),duration=meta.blockSeconds;
 const frameAhead=Math.max(duration,Math.min(3,S.speed*Math.max(2,4*S.frameLatencyMs/1000)));
 const pathAhead=Math.max(duration,Math.min(1.5,S.speed*Math.max(1.5,4*S.pathLatencyMs/1000)));
 const last=blockId(meta.end-1e-8);
 const futureTime=id=>Math.min(meta.end-1e-8,(id+.5)*duration);
 for(let id=current+1;id<=Math.min(last,blockId(t+frameAhead))&&S.pending.size<2;id++){
   if(S.blocks.has(id)||S.pending.has(id)||performance.now()<(S.frameRetryAt.get(id)||0))continue;
   ensureFrames(futureTime(id)).catch(e=>{
      if(e.name!=='AbortError'){
        S.frameRetryAt.set(id,performance.now()+3000);
        if(params.get('debug')==='1')console.warn('Replay frame prefetch failed',id,e);
      }
   });
 }
 for(let id=current+1;id<=Math.min(last,blockId(t+pathAhead))&&S.pathPending.size<1;id++){
   const probe=futureTime(id),key=pathKey(probe);
   if(S.pathBlocks.has(key)||S.pathPending.has(key)||performance.now()<(S.pathRetryAt.get(key)||0))continue;
   requestPath(probe);
 }
}
async function ensure(t){
 await ensureFrames(t);
 requestPath(t); // Path detail is optional for advancing the playback clock.
}
function setBuffering(value){
 if(value===S.buffering)return;
 if(value)S.bufferSince=performance.now();
 else S.totalBufferMs+=performance.now()-S.bufferSince;
 S.buffering=value;
}
function topologyAt(c,t){return [...c.topologyStates].reverse().find(s=>s.time<=t)||c.topologyStates[0];}
function frameAt(c,t){
 const rows=S.blocks.get(blockId(t))?.cases[c.id]||[];
 const frame=rows.find(r=>r.start<=t&&r.end>t);if(frame){const loaded=S.pathBlocks.has(pathKey(t)),detail=S.pathBlocks.get(pathKey(t))?.cases[c.id]?.[String(frame.id)]||[];return {...frame,nodePaths:detail,pathLoading:c.capabilities.nodePaths&&!loaded};}
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
function edgeForPair(state,src,dst){
 return state?.edges?.find(e=>e.src===src&&e.dst===dst)||state?.edges?.find(e=>e.src===dst&&e.dst===src)||null;
}
function capacityLabel(v){
 if(!finite(v))return '待确认';
 return Math.abs(v)>=1000?number(v/1000,2)+' Tbps':number(v,0)+' Gbps';
}
function focusChangeDetails(c,change,focus){
 const delta=change.changes.find(d=>pair(d.src,d.dst)===pair(focus.src,focus.dst));
 if(!delta)return null;
 if(change.applied){
  const states=[...c.topologyStates].sort((a,b)=>a.time-b.time);
  let afterIndex=states.findIndex(s=>s.epoch===change.epoch);
  if(afterIndex<0)afterIndex=states.findIndex(s=>Math.abs(s.time-change.time)<1e-8);
  const afterState=afterIndex>=0?states[afterIndex]:topologyAt(c,change.time);
  const beforeState=afterIndex>0?states[afterIndex-1]:states.filter(s=>s.time<change.time).at(-1);
  const beforeEdge=edgeForPair(beforeState,focus.src,focus.dst),afterEdge=edgeForPair(afterState,focus.src,focus.dst);
  const beforeCapacity=delta.before===0?0:finite(beforeEdge?.capacity)?beforeEdge.capacity:null;
  const afterCapacity=delta.after===0?0:finite(afterEdge?.capacity)?afterEdge.capacity:null;
  return {...delta,beforeCapacity,afterCapacity,capacityKnown:beforeCapacity!==null&&afterCapacity!==null};
 }
 const state=topologyAt(c,change.planTime??S.time),edge=edgeForPair(state,focus.src,focus.dst);
 const perCircuit=edge&&edge.circuits>0&&finite(edge.capacity)?edge.capacity/edge.circuits:null;
 const beforeCapacity=delta.before===0?0:perCircuit===null?null:delta.before*perCircuit;
 const afterCapacity=delta.after===0?0:perCircuit===null?null:delta.after*perCircuit;
 return {...delta,beforeCapacity,afterCapacity,capacityKnown:beforeCapacity!==null&&afterCapacity!==null};
}
function setup(){
 $('#globalCards').innerHTML=meta.cases.map((c,i)=>'<article class="global-card" style="--case:'+COLORS[i%3]+'"><div class="card-heading"><h2>'+esc(c.label)+'</h2><span>最终 FCT · 固定共同完成任务集</span></div><div class="quantiles">'+['p50','p90','p99'].map(k=>{
  const q=c.quantiles[k],gain=c.mode==='ecmp'?'基线':q.improvement==null?'无共同完成任务':(q.improvement>=0?'降低 ':'增加 ')+number(Math.abs(q.improvement))+'%';
  return '<div><div class="metric-label">'+k.toUpperCase()+'</div><div class="metric-value">'+number(q.seconds*1000,2)+'<small>ms</small></div><div class="gain '+(q.improvement<0?'regression':'')+'">'+gain+'</div></div>';
 }).join('')+'<div><div class="metric-label" title="存储接入链路双向实际发送比特 / 双向容量时间积分">存储侧双向利用率</div><div class="metric-value">'+percent(c.storageUtilization)+'</div><div class="gain">完整仿真时间段</div></div><div><div class="metric-label" title="Storage → Leaf 与 Leaf → Storage 的 tx_wire_bytes × 8 / 仿真总时长，含协议头与重传">存储侧总吞吐</div><div class="metric-value">'+number(c.storageThroughputGbps,1)+'</div><div class="gain">Gbps · 双向全程平均</div></div></div><div class="live-summary"><span>当前完成 <b id="complete-'+i+'">—</b> / '+c.totalTasks+'</span><span>进行中 <b id="running-'+i+'">—</b></span></div></article>').join('');
 $('#networks').innerHTML=meta.cases.map((c,i)=>'<article class="network-row" style="--case:'+COLORS[i%3]+'"><div class="case-side"><div><div class="case-name">'+esc(c.label)+'</div><div class="case-purpose">同一仿真时刻</div></div><div class="global-live"><div title="存储侧 Storage → Leaf 与 Leaf → Storage 双向接入发送量，含协议头和重传"><label>存储侧双向吞吐</label><b id="throughput-'+i+'">—</b><small>Gbps · TX + RX</small></div><div title="Leaf 间发送比特数 / 有效容量时间积分"><label>Fabric 利用率</label><b id="util-'+i+'">—</b></div><div><label>最大利用率</label><b id="max-'+i+'">—</b></div></div></div><div class="network-space"><canvas class="network" id="network-'+i+'" aria-label="'+esc(c.label)+' 链路热力拓扑"></canvas><span class="network-watermark">上排 L＝交换机　下排 C＝计算 / S＝存储　接入间距已压缩</span></div><div class="action-side"><div class="action-label" id="action-label-'+i+'"></div><div class="action-title" id="action-title-'+i+'"></div><div class="action-lines" id="action-lines-'+i+'"></div><div class="action-footer" id="action-footer-'+i+'"></div></div></article>').join('');
 $('#focus').innerHTML=meta.focusOptions.map(p=>'<option value="'+direction(p.src,p.dst)+'">L'+p.src+' → L'+p.dst+'</option>').join('');
 $('#focus').value=direction(S.focus.src,S.focus.dst);$('#focus').disabled=false;
 $('#focus').onchange=e=>{const [src,dst]=e.target.value.split('-').map(Number);cancelLoads(true);S.focus={src,dst};S.dirty=true;requestPath(S.time);prefetchAhead(S.time);loadHotspot().catch(fail);};
 $('#focus').onpointerdown=updateOptions;
 $('#focus').onfocus=updateOptions;
 $('#heatToggle').disabled=false;
 $('#heatToggle').onclick=()=>{S.heat=!S.heat;$('#heatToggle').setAttribute('aria-pressed',String(S.heat));$('#heatToggle').textContent='热点展示：'+(S.heat?'开':'关');$('#heatLegend').classList.toggle('heat-off',!S.heat);$('#heatDescription').textContent=S.heat?'颜色＝利用率；线宽＝发送速率（各组同尺度）':'背景为灰色结构线；彩色路径与交换机＝当前焦点的中转';S.dirty=true;};
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
 const paths=focusedPaths(frame),mids=midpoints(c,frame),focusKeys=new Set([pair(S.focus.src,S.focus.dst)]),flow=new Map(),nodeRates=new Map(),pathColors=new Map(),endpointRoles=new Map();
 const addFlow=(a,b,rate)=>{const k=direction(a,b),old=flow.get(k);flow.set(k,{a,b,rate:(old?.rate||0)+rate});focusKeys.add(pair(a,b));};
 for(const p of paths){
   const sourceRoles=endpointRoles.get(p.srcNode)||new Set();sourceRoles.add('P');endpointRoles.set(p.srcNode,sourceRoles);
   const destRoles=endpointRoles.get(p.dstNode)||new Set();destRoles.add('D');endpointRoles.set(p.dstNode,destRoles);
  addFlow(p.srcNode,p.src,p.gbps);addFlow(p.dst,p.dstNode,p.gbps);
  if(p.transit<0)addFlow(p.src,p.dst,p.gbps);else{addFlow(p.src,p.transit,p.gbps);addFlow(p.transit,p.dst,p.gbps);}
  if(p.transit>=0)for(const [a,b] of [[p.src,p.transit],[p.transit,p.dst]])pathColors.set(pair(a,b),transitColor(p.transit));
  for(const [id,key] of [[p.srcNode,'tx'],[p.dstNode,'rx']]){const n=nodeRates.get(id)||{tx:0,rx:0};n[key]+=p.gbps;nodeRates.set(id,n);}
 }
 if(!c.capabilities.nodePaths){
  const hops=[[S.focus.src,S.focus.dst],...mids.flatMap(m=>[[S.focus.src,m],[m,S.focus.dst]])];
  for(const [a,b] of hops){focusKeys.add(pair(a,b));const e=observed.get(direction(a,b));if(e?.gbps>0)addFlow(a,b,e.gbps);}
  for(const m of mids)for(const [a,b] of [[S.focus.src,m],[m,S.focus.dst]])pathColors.set(pair(a,b),transitColor(m));
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
  const pathColor=pathColors.get(k)||'#244c69';
  if(focal)stroke(ctx,g,pathColor,S.heat?width+(e.fabric?3:2):3.8,S.heat ? .5 : .95);
  if(!deleted)stroke(ctx,g,S.heat?color:focal?pathColor:'#c4cdd5',S.heat?(.7+3.3*Math.log1p(measured?.gbps||0)/Math.log1p(Math.max(1,meta.rateScaleGbps)))*(e.fabric?1:.65):focal?2:.7,S.heat?focal?Math.max(.65,opacity):opacity:focal?1:.28,S.heat&&!finite(u));
  if(S.heat&&finite(u)&&u>=.7&&measured)arrow(ctx,g,color,measured.src===e.dst);
 }
 // Focus labels keep the selected direction explicit; background heat remains max(A→B,B→A).
 const hops=[[S.focus.src,S.focus.dst],...mids.flatMap(m=>[[S.focus.src,m],[m,S.focus.dst]])];
 hops.forEach(([a,b],index)=>{
  const g=geometries.get(pair(a,b));if(!g)return;
  const e=observed.get(direction(a,b)),p=at(g,index===0 ? .5 : .42);
  const text='L'+a+'→L'+b+' '+percent(e?.utilization)+(index===0?' · '+number(e?.gbps,0)+'/'+number(e?.capacity,0)+'G':'');
  const color=pathColors.get(pair(a,b))||'#244c69';
  if(e?.gbps>0)arrow(ctx,g,color,positions.get(a)!==g.a);
  badge(ctx,text,p[0],p[1]+(index%2?9:-9),color);
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
   const roles=n.role==='compute-server'?endpointRoles.get(n.id):null;
   const endpointRole=roles?.size===2?'P/D':roles?.has('P')?'P':roles?.has('D')?'D':null;
   ctx.save();ctx.fillStyle=active?'#244c69':upper?'#edf3f7':n.role==='storage-server'?'#e3daef':'#d7eaf4';ctx.strokeStyle=active?'#173a55':upper?'#8ca6b9':n.role==='storage-server'?'#9a83b6':'#75a6c0';ctx.lineWidth=active?1.8:1;
   if(upper){ctx.fillStyle=n.role==='storage-leaf'?'#e3daef':'#d7eaf4';if(mids.includes(n.id)){ctx.fillStyle=transitColor(n.id);ctx.strokeStyle=transitColor(n.id);}else if(active){ctx.strokeStyle='#173a55';ctx.lineWidth=2.5;}}
   ctx.beginPath();
   if(upper)ctx.rect(x-10,y-6,20,12);
   else if(n.role==='storage-server')ctx.rect(x-5,y-5,10,10);
   else if(endpointRole==='D'){ctx.moveTo(x,y-6);ctx.lineTo(x+6,y);ctx.lineTo(x,y+6);ctx.lineTo(x-6,y);ctx.closePath();}
   else if(endpointRole==='P/D'){ctx.moveTo(x-5,y-3);ctx.lineTo(x,y-6);ctx.lineTo(x+5,y-3);ctx.lineTo(x+5,y+3);ctx.lineTo(x,y+6);ctx.lineTo(x-5,y+3);ctx.closePath();}
   else ctx.arc(x,y,5,0,Math.PI*2);
   ctx.fill();ctx.stroke();
   ctx.textAlign='center';ctx.fillStyle=active?'#24445b':'#7c91a0';ctx.font=(upper?'10':'9')+'px ui-monospace,monospace';
   if(upper&&mids.includes(n.id))ctx.fillStyle=transitColor(n.id);
   const prefix=upper?'L':n.role==='storage-server'?'S':endpointRole||'C';
   ctx.fillText(prefix+n.id+(upper&&mids.includes(n.id)?' 中转':''),x,y+17);
   if(topRates.has(n.id)){const rate=nodeRates.get(n.id);ctx.font='8px ui-monospace,monospace';ctx.fillText((rate.tx?'↑'+number(rate.tx,0):'')+(rate.tx&&rate.rx?' / ':'')+(rate.rx?'↓'+number(rate.rx,0):'')+'G',x,y-10);}
   ctx.restore();
  }
  const routes=[];
 for(const f of flow.values()){
  const base=geometries.get(pair(f.a,f.b));if(!base||f.rate<=0)continue;
  const g=positions.get(f.a)===base.a?base:{a:base.b,b:base.a,c:base.c};
  routes.push({g,rate:f.rate,color:pathColors.get(pair(f.a,f.b))||(S.heat?heat(observed.get(direction(f.a,f.b))?.utilization)[0]:'#244c69')});
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
 $('#action-label-'+i).textContent='焦点 L'+f.src+' → L'+f.dst+'　·　正向 '+percent(e?.utilization)+' / 反向 '+percent(reverse?.utilization);
 const payload=paths.reduce((n,p)=>n+(finite(p.gbps)?p.gbps:0),0);
 const isToe=c.mode.startsWith('toe')&&change;
 let title=mids.length?'实际中转路径':'直达方向与接入流量',lines='';
 if(isToe){
  title=change.failed?'重配未生效':change.applied?'重配已生效':'重配待生效';
  const globalLabel=change.applied?'全局拓扑变化':'计划拓扑变化';
  lines+='<div class="toe-summary">'+globalLabel+'：新增 '+change.added+' 条连接 · 拆除 '+change.removed+' 条连接 · 影响 '+change.changes.length+' 对 Leaf</div>';
  const focal=focusChangeDetails(c,change,f);
  if(!focal){
   lines+='<div class="focus-change">焦点 L'+f.src+' ↔ L'+f.dst+'：本次重配未调整该 Leaf 对</div>';
  }else if(change.applied){
   lines+='<div class="focus-change">焦点连接数 '+focal.before+' → '+focal.after+' 条</div>';
   lines+='<div>单方向容量 '+capacityLabel(focal.beforeCapacity)+' → '+capacityLabel(focal.afterCapacity)+'</div>';
  }else{
   lines+='<div class="focus-change">计划连接数 '+focal.before+' → '+focal.after+' 条</div>';
   if(change.failed){
    const actualEdge=edgeForPair(topologyAt(c,S.time),f.src,f.dst),actualCapacity=actualEdge?.capacity??0;
    lines+='<div>实际单方向容量 '+capacityLabel(actualCapacity)+'（未变化）</div>';
   }else if(focal.capacityKnown){
    lines+='<div>计划单方向容量 '+capacityLabel(focal.beforeCapacity)+' → '+capacityLabel(focal.afterCapacity)+'</div>';
   }else{
    lines+='<div>计划单方向容量待生效后确认</div>';
   }
  }
 }
 if(mids.length){
  lines+=mids.map(m=>'<div class="route-label" style="color:'+transitColor(m)+'">L'+f.src+' → L'+m+' → L'+f.dst+'</div>').join('');
 }else{
  lines+='<div class="route-label">直达：L'+f.src+' → L'+f.dst+'</div>';
 }
 if(paths.length){
  const pairs=new Map();
  for(const p of paths){
   const key=String(p.srcNode)+'-'+String(p.dstNode);
   const row=pairs.get(key)||{srcNode:p.srcNode,dstNode:p.dstNode,gbps:0};
   row.gbps+=finite(p.gbps)?p.gbps:0;pairs.set(key,row);
  }
  const largest=[...pairs.values()].sort((a,b)=>b.gbps-a.gbps||a.srcNode-b.srcNode||a.dstNode-b.dstNode);
  lines+='<div>端点载荷合计 '+number(payload,2)+' Gbps</div>';
  lines+='<ul class="endpoint-list">'+largest.slice(0,3).map(p=>'<li><span>P'+p.srcNode+' → D'+p.dstNode+'</span><span>'+number(p.gbps,2)+' Gbps</span></li>').join('')+(largest.length>3?'<li class="endpoint-ellipsis">…</li>':'')+'</ul>';
 }else{
  lines+='<div>'+(frame.pathLoading?'路径详情加载中':detailAvailable(c,S.time)?'当前窗口未观测到该方向的端点载荷':'此时间段未采集端到端路径详情')+'</div>';
 }
 $('#action-title-'+i).textContent=title;$('#action-lines-'+i).innerHTML=lines;
 const phase={draining:'排空',guard:'保护等待',applied:'拓扑生效',missing:'无采样'}[frame.phase]||'';
 const footer=['窗口 '+number(frame.start,6)+'–'+number(frame.end,6)+' s','正向发送 '+number(e?.gbps,0)+' / 容量 '+number(e?.capacity,0)+' Gbps'];
 if(phase)footer.push(phase);
 $('#action-footer-'+i).innerHTML=footer.join('<br>');
}
function draw(){
 meta.cases.forEach((c,i)=>{
  const frame=frameAt(c,S.time);S.frames[c.id]=frame;makeScene(c,frame,i);action(c,frame,i);
  $('#throughput-'+i).textContent=number(frame.global?.storageThroughputGbps,2);
  $('#util-'+i).textContent=percent(frame.global?.fabricUtilization);
  $('#max-'+i).textContent=percent(frame.global?.maxUtilization);
 });
}
function cursor(){
 const events=meta.cases.flatMap((c,i)=>c.events.filter(e=>['te-policy-published','topology-applied','toe-draining'].includes(e.event)).map(e=>({xAxis:e.time,name:c.label+' '+e.event,lineStyle:{color:COLORS[i%3],type:'dotted',width:1},label:{show:false}})));
 return {silent:true,symbol:'none',label:{show:false},lineStyle:{color:'#91a3b1',type:'dashed',width:1},data:[{xAxis:S.time},...events]};
}
function chartBase(){
 return {animation:false,tooltip:{trigger:'axis',confine:true,valueFormatter:v=>finite(v)?number(v,3):'无采样'},grid:{left:48,right:14,top:16,bottom:25},dataZoom:[{type:'inside',filterMode:'none',start:0,end:100,zoomOnMouseWheel:false,moveOnMouseWheel:false}],xAxis:{type:'value',min:S.trendStart??recording.start,max:S.trendEnd??recording.end,axisLabel:{fontSize:8,color:'#8c9daa',formatter:v=>v.toFixed((S.trendEnd-S.trendStart)<10?2:0)+'s'},splitLine:{show:false},axisLine:{lineStyle:{color:'#dce6ec'}}},yAxis:{type:'value',min:0,splitNumber:2,axisLabel:{fontSize:8,color:'#8c9daa'},splitLine:{lineStyle:{color:'#edf2f5'}}}};
}
function line(id,data,color,dashed=false,name=id){return {id,name,type:'line',showSymbol:false,step:'end',connectNulls:false,lineStyle:{color,width:dashed?1:1.6,type:dashed?'dashed':'solid'},data};}
function trendQuery(chart){return 'start='+S.trendStart+'&end='+S.trendEnd+'&points='+Math.max(100,Math.min(1600,Math.round(chart.getWidth()*2)));}
async function loadCompletion(){
 const request=++S.completionRequest,data=await get('/api/completion?'+trendQuery(completionChart));if(request!==S.completionRequest)return;
 completionChart.setOption({...chartBase(),series:meta.cases.map((c,i)=>({...line(c.id,data.cases[c.id].map(p=>[p.time,p.completed]),COLORS[i%3],false,c.label+' 完成数'),markLine:i===0?cursor():undefined}))},{notMerge:true});
}
async function loadHotspot(force=false){
 const f={...S.focus},k=direction(f.src,f.dst)+'@'+S.trendStart+':'+S.trendEnd;if(!force&&k===S.trendKey)return;
 S.trendKey=k;const request=++S.trendRequest,data=await get('/api/trend?src='+f.src+'&dst='+f.dst+'&'+trendQuery(hotspotChart));if(request!==S.trendRequest)return;
 $('#hotspotTitle').textContent='焦点 L'+f.src+' → L'+f.dst;
 hotspotChart.setOption({...chartBase(),series:meta.cases.flatMap((c,i)=>[
  {...line(c.id+'-rate',data.cases[c.id].map(p=>[p.time,p.gbps]),COLORS[i%3],false,c.label+' 发送 Gbps'),markLine:i===0?cursor():undefined},
  line(c.id+'-cap',data.cases[c.id].map(p=>[p.time,p.capacity]),COLORS[i%3],true,c.label+' 容量 Gbps')
 ])},{notMerge:true});
}
async function loadGlobal(){
 const request=++S.globalRequest,data=await get('/api/global-trend?'+trendQuery(throughputChart));if(request!==S.globalRequest)return;
 throughputChart.setOption({...chartBase(),series:meta.cases.map((c,i)=>({...line(c.id+'-throughput',data.cases[c.id].map(p=>[p.time,p.storageThroughputGbps]),COLORS[i%3],false,c.label+' 存储双向 Gbps'),markLine:i===0?cursor():undefined}))},{notMerge:true});
 const metric=$('#utilMetric').value;
 utilizationChart.setOption({...chartBase(),series:meta.cases.map((c,i)=>({...line(c.id+'-util',data.cases[c.id].map(p=>[p.time,finite(p[metric])?p[metric]*100:null]),COLORS[i%3],false,c.label+' 利用率 %'),markLine:i===0?cursor():undefined}))},{notMerge:true});
 globalChart.setOption({...chartBase(),grid:{left:39,right:28,top:16,bottom:25},yAxis:[{...chartBase().yAxis,max:v=>Math.max(100,Math.ceil(v.max/20)*20)},{...chartBase().yAxis,position:'right',splitLine:{show:false}}],series:meta.cases.flatMap((c,i)=>[
  {...line(c.id+'-max',data.cases[c.id].map(p=>[p.time,finite(p.maxUtilization)?p.maxUtilization*100:null]),COLORS[i%3],false,c.label+' 峰值利用率 %'),markLine:i===0?cursor():undefined},
  {...line(c.id+'-hot',data.cases[c.id].map(p=>[p.time,p.hotLinks]),COLORS[i%3],true,c.label+' 热点方向数'),yAxisIndex:1}
 ])},{notMerge:true});
}
function charts(){return [completionChart,hotspotChart,globalChart,throughputChart,utilizationChart];}
async function reloadTrends(){await Promise.all([loadCompletion(),loadHotspot(true),loadGlobal()]);$('#trendStatus').textContent=number(S.trendStart,3)+'–'+number(S.trendEnd,3)+' s · 聚合均值 / 热点峰值；滚轮可放大 / 缩小，点击可定位';}
function trendControls(){
  const clamp=(value,min,max)=>Math.max(min,Math.min(max,value));
  const setRange=(start,end)=>{
   const fullStart=recording.start,fullEnd=recording.end,total=fullEnd-fullStart;
   if(!finite(start)||!finite(end)||end<=start||total<=0)return;
   const minSpan=Math.min(meta.windowSeconds,total);
   const span=clamp(end-start,minSpan,total);
   const center=(start+end)/2;
   start=center-span/2;end=center+span/2;
   if(start<fullStart){end+=fullStart-start;start=fullStart;}
   if(end>fullEnd){start-=end-fullEnd;end=fullEnd;}
   S.trendStart=clamp(start,fullStart,fullEnd);
   S.trendEnd=clamp(end,fullStart,fullEnd);
   reloadTrends().catch(fail);
  };
  $('#trendAll').onclick=()=>setRange(recording.start,recording.end);
  $('#trendLocal').onclick=()=>{
   const width=Math.min(1,recording.end-recording.start);
   const start=Math.max(recording.start,Math.min(S.time-width/2,recording.end-width));
   setRange(start,start+width);
  };
  $('#utilMetric').onchange=()=>loadGlobal().catch(fail);

  let zoomTimer,pendingRange=null;
  const scheduleRange=(start,end,delayMs=100)=>{
   if(!finite(start)||!finite(end)||end<=start)return;
   pendingRange=[start,end];
   clearTimeout(zoomTimer);
   zoomTimer=setTimeout(()=>{
    const range=pendingRange;
    pendingRange=null;
    if(range)setRange(range[0],range[1]);
   },delayMs);
  };
  const dimension=(value,total,fallback)=>{
   if(finite(value))return value;
   if(typeof value==='string'&&value.endsWith('%'))return total*parseFloat(value)/100;
   return fallback;
  };

  for(const chart of charts()){
   chart.getZr().on('click',e=>{
    if(!chart.containPixel('grid',[e.offsetX,e.offsetY]))return;
    const point=chart.convertFromPixel('grid',[e.offsetX,e.offsetY]);
    if(point&&finite(point[0]))seekTo(point[0]).catch(fail);
   });

   // Use a controlled wheel scale instead of ECharts' small default increments.
   // Positive deltaY (wheel down) zooms out; negative deltaY zooms in.
   chart.getDom().addEventListener('wheel',event=>{
    const rect=chart.getDom().getBoundingClientRect();
    const x=event.clientX-rect.left,y=event.clientY-rect.top;
    if(!chart.containPixel('grid',[x,y]))return;
    event.preventDefault();

    const base=pendingRange||[S.trendStart,S.trendEnd];
    const baseStart=base[0],baseEnd=base[1],span=baseEnd-baseStart;
    const total=recording.end-recording.start;
    if(span<=0||total<=0)return;

    const option=chart.getOption();
    const grid=Array.isArray(option.grid)?(option.grid[0]||{}):(option.grid||{});
    const left=dimension(grid.left,chart.getWidth(),48);
    const right=dimension(grid.right,chart.getWidth(),14);
    const plotWidth=Math.max(1,chart.getWidth()-left-right);
    const ratio=clamp((x-left)/plotWidth,0,1);
    const anchor=baseStart+ratio*span;

    const wheelUnits=event.deltaMode===1?event.deltaY*16/100:
     event.deltaMode===2?event.deltaY*chart.getHeight()/100:event.deltaY/100;
    const factor=Math.pow(1.4,clamp(wheelUnits,-3,3));
    const minSpan=Math.min(meta.windowSeconds,total);
    const nextSpan=clamp(span*factor,minSpan,total);
    if(Math.abs(nextSpan-span)<1e-9)return;

    let start=anchor-ratio*nextSpan,end=start+nextSpan;
    if(start<recording.start){end+=recording.start-start;start=recording.start;}
    if(end>recording.end){start-=end-recording.end;end=recording.end;}
    start=clamp(start,recording.start,recording.end);
    end=clamp(end,recording.start,recording.end);
    scheduleRange(start,end,90);
   },{passive:false});

   // Keep drag-based dataZoom support; wheel zoom is handled above.
   chart.on('datazoom',event=>{
    const z=event.batch?.[0]||event;
    const base=pendingRange||[S.trendStart,S.trendEnd];
    const span=base[1]-base[0];
    const start=finite(z.startValue)?z.startValue:base[0]+span*(z.start??0)/100;
    const end=finite(z.endValue)?z.endValue:base[0]+span*(z.end??100)/100;
    scheduleRange(start,end,120);
   });
  }
}
function updateText(){
 $('#time').textContent=number(S.time,3);
 meta.cases.forEach((c,i)=>{$('#complete-'+i).textContent=progressAt(c,S.time).completed;$('#running-'+i).textContent=progressAt(c,S.time).running;});
 const parts=meta.cases.map(c=>{const f=S.frames[c.id],e=f?.edges.find(e=>e.src===S.focus.src&&e.dst===S.focus.dst);return c.label+' '+percent(e?.utilization);});
 const selected=$('#focus').selectedOptions[0];
 if(selected)selected.textContent='L'+S.focus.src+' → L'+S.focus.dst+'　'+parts.join(' / ');
 $('#caption').textContent='L'+S.focus.src+' → L'+S.focus.dst+'：'+parts.join(' / ')+'。'+(S.heat?'热力表示链路负载；':'热点已关闭；')+'彩色中转 Leaf 对应当前窗口的实际路径。';
 if(!S.seeking)$('#timeline').value=S.time;
 $('#currentClock').textContent=number(S.time,3)+' s';
 $('#play').textContent=S.playing?'暂停播放':S.time>=recording.end?'重新播放':'开始播放';
 $('#playbackState').textContent=S.seeking?'正在定位':S.buffering?'缓冲中（等待 Frame）':S.time>=recording.end?'回放完成':S.playing?'播放中':'已暂停';
 $('#buffer').textContent=S.buffering?'加载播放帧中':S.pathPending.has(pathKey(S.time))?'路径详情后台加载中':'数据就绪';
 if(performance.now()-(S.lastCursor||0)<250)return;S.lastCursor=performance.now();
 for(const [chart,id] of [[completionChart,meta.cases[0].id],[hotspotChart,meta.cases[0].id+'-rate'],[globalChart,meta.cases[0].id+'-max'],[throughputChart,meta.cases[0].id+'-throughput'],[utilizationChart,meta.cases[0].id+'-util']])chart.setOption({series:[{id,markLine:cursor()}]});
}
function setPlaying(value){S.playing=value;if(!value)setBuffering(false);S.lastWall=performance.now();updateText();}
async function stepFrame(delta){
 const version=S.stepRequest=(S.stepRequest||0)+1,at=S.time;
 S.seeking=false;S.resumeAfterSeek=false;S.playing=false;
 const probe=Math.max(recording.start,Math.min(recording.end,S.time+delta*meta.windowSeconds));
 await ensure(probe);
 if(version!==S.stepRequest||S.time!==at)return;
 const boundaries=[recording.start,recording.end,...[...S.blocks.values()].flatMap(b=>Object.values(b.cases).flatMap(rows=>rows.flatMap(f=>[f.start,f.end])))];
 const candidates=boundaries.filter(t=>delta>0?t>S.time+1e-10:t<S.time-1e-10);
 const next=candidates.length?(delta>0?Math.min(...candidates):Math.max(...candidates)):probe;
 await seekTo(next);
}
async function seekTo(time,resume=false){
 S.stepRequest=(S.stepRequest||0)+1;
 const version=++S.seekVersion;cancelLoads();setBuffering(false);
 S.time=Math.max(recording.start,Math.min(recording.end,time));S.playing=false;S.lastWall=performance.now();S.dirty=true;
 $('#buffer').textContent='正在定位';updateText();
 await ensure(S.time);
 if(version!==S.seekVersion)return;
 draw();paint();updateText();S.dirty=true;$('#buffer').textContent='数据就绪';
 if(resume&&S.time<recording.end)setPlaying(true);
}
function controls(){
 const timeline=$('#timeline');timeline.min=recording.start;timeline.max=recording.end;timeline.step=meta.windowSeconds;timeline.value=S.time;
 $('#speed').onchange=e=>{S.speed=Number(e.target.value);S.lastWall=performance.now();};
 $('#endClock').textContent=number(recording.end,3)+' s';
 $('#play').disabled=false;$('#restart').disabled=false;timeline.disabled=false;
 $('#prevFrame').disabled=false;$('#nextFrame').disabled=false;
 $('#prevFrame').onclick=()=>stepFrame(-1).catch(fail);$('#nextFrame').onclick=()=>stepFrame(1).catch(fail);
 $('#play').onclick=()=>{
  S.seeking=false;S.resumeAfterSeek=false;++S.seekVersion;
  if(S.time>=recording.end)seekTo(recording.start,true).catch(fail);else setPlaying(!S.playing);
 };
 $('#restart').onclick=()=>{S.seeking=false;S.resumeAfterSeek=false;seekTo(recording.start,true).catch(fail);};
 let seekTimer=null;
 timeline.oninput=()=>{
  if(!S.seeking){S.resumeAfterSeek=S.playing;S.seeking=true;setPlaying(false);}
  const target=Number(timeline.value);
  $('#currentClock').textContent=number(target,3)+' s';
  $('#time').textContent=number(target,3);
  $('#playbackState').textContent='拖动定位';
  clearTimeout(seekTimer);
  seekTimer=setTimeout(()=>seekTo(target).catch(fail),180);
 };
 const finish=()=>{
  if(!S.seeking)return;
  clearTimeout(seekTimer);
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
  if(S.blocks.has(blockId(next))){
    S.time=next;setBuffering(false);
    if(next>=recording.end){
      if(params.get('loop')==='1'){S.time=recording.start;ensure(S.time).catch(fail);}
      else S.playing=false;
    }
  }else{
    setBuffering(true);
    ensureFrames(next).catch(fail);
  }
  requestPath(S.time); // Slow path details cannot block live topology or global metrics.
  if(wall-S.lastPrefetch>=200){prefetchAhead(S.time);S.lastPrefetch=wall;}
  if(params.get('debug')==='1'&&wall-S.lastStats>=10000){
    console.debug('[Replay playback]',{time:S.time,speed:S.speed,framePending:S.pending.size,
      pathPending:S.pathPending.size,frameCache:S.blocks.size,pathCache:S.pathBlocks.size,
      frameLatencyMs:Math.round(S.frameLatencyMs),pathLatencyMs:Math.round(S.pathLatencyMs),
      totalBufferMs:Math.round(S.totalBufferMs+(S.buffering?performance.now()-S.bufferSince:0))});
    S.lastStats=wall;
  }
 }
 if(wall-S.lastDraw>33){
  const frames=meta.cases.map(c=>frameAt(c,S.time)),key=frames.map(f=>f.id).join('|')+'|'+direction(S.focus.src,S.focus.dst)+'|'+Math.floor(S.time/meta.windowSeconds);
  if(S.dirty||key!==S.key){draw();S.key=key;S.dirty=meta.cases.some(c=>!S.scenes.has(c.id));}
  updateText();S.lastDraw=wall;
 }
 if(S.dirty||wall-S.lastPaint>=33){paint(wall);S.lastPaint=wall;}
 requestAnimationFrame(frameLoop);
}
function frameLoop(wall){try{tick(wall);}catch(error){fail(error);}}
async function init(){
 if(!window.echarts)throw Error('本地 ECharts 文件未加载');
 await waitReady();meta=await get('/api/bootstrap');recording=meta.recording;S.time=recording.start;S.focus={...meta.focus};S.trendStart=recording.start;S.trendEnd=recording.end;
 nodes=meta.topology.nodes;leaves=meta.topology.leaves;devices=nodes.filter(n=>!leaves.includes(n.id)).map(n=>n.id);
 const groups=new Map();
 for(const e of meta.topology.links)if(e.kind==='access'){const k=pair(e.src,e.dst),g=groups.get(k)||{src:Math.min(e.src,e.dst),dst:Math.max(e.src,e.dst),circuits:0};g.circuits++;groups.set(k,g);}access=[...groups.values()];
 $('#sourceLabel').textContent=meta.synthetic?'SMOKE 合成数据 · 非实验结果':'实际实验日志';
 $('#sourceLabel').classList.toggle('warning',meta.synthetic);
 $('#topologyInfo').textContent=nodes.length+' 节点 · '+leaves.length+' Leaf · '+meta.topology.fabricCircuits+' OCS';
 $('#cohort').textContent=number(meta.windowSeconds*1000,0)+' ms 统计；最终 P50/P90/P99：共同完成 '+meta.commonTasks+' 个任务；未完成：'+meta.cases.map(c=>c.label+' '+(c.totalTasks-c.completedTasks)).join(' / ')+'。节点路径为源 Leaf 窗口发送量。';
 $('#completedLegend').innerHTML=meta.cases.map((c,i)=>'<span style="color:'+COLORS[i%3]+'">● '+esc(c.label)+'</span>').join('　');
 setup();completionChart=echarts.init($('#completionChart'));hotspotChart=echarts.init($('#hotspotChart'));globalChart=echarts.init($('#globalChart'));throughputChart=echarts.init($('#throughputChart'));utilizationChart=echarts.init($('#utilizationChart'));trendControls();
 await Promise.all([ensure(S.time),reloadTrends()]);
 prefetchAhead(S.time);
 await new Promise(resolve=>requestAnimationFrame(resolve));
 draw();paint();controls();updateText();updateOptions();
 window.addEventListener('resize',()=>{charts().forEach(c=>c.resize());S.dirty=true;});
 if(window.ResizeObserver){const observer=new ResizeObserver(()=>{S.dirty=true;});document.querySelectorAll('.network-space').forEach(el=>observer.observe(el));}
 $('#preparing').hidden=true;setPlaying(params.get('autoplay')==='1');requestAnimationFrame(frameLoop);
}
init().catch(fail);
