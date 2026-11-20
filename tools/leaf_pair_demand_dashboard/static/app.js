/* Local ECharts, bounded data requests, one shared range for every Leaf pair. */
"use strict";
const $ = id => document.getElementById(id);
const colors = ["#4f46c8", "#028896"];
const state = {info:null, range:null, metric:"payload", data:null, overview:null,
  cards:new Map(), charts:new Map(), visible:new Set(), request:0, abort:null, timer:null,
  hoverBusy:false, pointerBusy:false, overviewBusy:false};
const fmt = (v, digits=3) => v == null ? "—" : Number(v).toLocaleString("en-US", {maximumFractionDigits:digits});
const pct = v => v == null ? "—" : `${fmt(v*100,1)}%`;
const seconds = v => Number(v.toPrecision(12));
const status = (message, error=false) => { $("status").textContent=message; $("status").classList.toggle("error",error); };
function selectedDirections(pair) {
  const direction=$("direction").value;
  return direction==="forward" ? [pair.directions[0]] : direction==="reverse" ? [pair.directions[1]] : pair.directions;
}
function directionLabel(pair, direction) {
  return direction===pair.directions[0] ? `${pair.group}${pair.compute} → S${pair.storage}` : `S${pair.storage} → ${pair.group}${pair.compute}`;
}
function summary(pair, direction) { return state.info.summaries[String(direction)][state.metric]; }
function summaryValue(pair, key) { return Math.max(0,...selectedDirections(pair).map(d=>summary(pair,d)?.[key] ?? 0)); }

async function getJSON(url, signal) {
  const response=await fetch(url,{signal});
  if (!response.ok) {
    const body=await response.json().catch(()=>({detail:response.statusText}));
    throw new Error(typeof body.detail==="string" ? body.detail : JSON.stringify(body.detail));
  }
  return response.json();
}

function clampRange(start,end) {
  const {info}=state;
  const minimum=Math.min(info.end-info.start,Math.max(1e-9,info.sample_interval_seconds/20));
  let width=Math.min(info.end-info.start,Math.max(minimum,end-start));
  start=Math.max(info.start,Math.min(info.end-width,start));
  return [seconds(start),seconds(start+width)];
}
function setRange(start,end,immediate=false) {
  state.range=clampRange(start,end);
  $("start").value=state.range[0]; $("end").value=state.range[1];
  $("rangeLabel").textContent=`${fmt(state.range[1]-state.range[0],6)} s 窗口 · 全部图表联动`;
  syncOverview();
  // Keep the axes responsive while the server resolves the next data grain.
  for (const chart of state.charts.values()) chart.setOption({xAxis:{min:state.range[0],max:state.range[1]}});
  clearTimeout(state.timer);
  // Invalidate an in-flight response as soon as the user changes the range.
  state.request++;
  if(state.abort) state.abort.abort();
  state.timer=setTimeout(loadSeries,immediate?0:140);
}
function zoom(factor,anchor=.5) {
  const [a,b]=state.range, width=(b-a)*factor, point=a+(b-a)*anchor;
  setRange(point-width*anchor,point+width*(1-anchor));
}
function syncOverview() {
  if (!state.overview) return;
  state.overviewBusy=true;
  state.overview.dispatchAction({type:"dataZoom",startValue:state.range[0],endValue:state.range[1]});
  state.overviewBusy=false;
}

async function loadSeries() {
  const token=++state.request;
  if(state.abort) state.abort.abort();
  const abort=new AbortController(); state.abort=abort;
  status("正在按当前时间范围查询…");
  const visibleCard=state.cards.values().next().value;
  const points=Math.max(80,Math.min(650,Math.round((visibleCard?.element.clientWidth || 350)*.9)));
  const params=new URLSearchParams({start:state.range[0],end:state.range[1],metric:state.metric,points});
  try {
    const data=await getJSON(`/api/series?${params}`,abort.signal);
    if(token!==state.request) return;
    state.data=data;
    drawVisible();
    const counts=Object.values(data.directions).map(x=>x.bins.length);
    status(`已显示 ${fmt(data.start,6)}–${fmt(data.end,6)} s · 每方向最多 ${Math.max(...counts)} 点 · ${data.level===0?"原始采样窗口":"多粒度汇总，保留原始峰值"}`);
  } catch(error) { if(error.name!=="AbortError" && token===state.request) status(`加载失败：${error.message}`,true); }
}

function chartLimit() {
  if($("yscale").value==="auto" || !state.data) return undefined;
  let maximum=0;
  for(const pair of state.info.pairs) for(const direction of selectedDirections(pair)) {
    const d=state.data.directions[String(direction)];
    for(const row of d.bins) maximum=Math.max(maximum,row[2],$("showPeak").checked?row[3]:0);
    if($("showCapacity").checked) for(const row of d.capacity) maximum=Math.max(maximum,row[1]);
  }
  return maximum>0?maximum*1.08:1;
}
function stepPoints(bins,column) {
  const points=[];
  for(const row of bins) points.push([row[0],row[column]],[row[1],row[column]]);
  return points;
}
function renderChart(card,limit) {
  const pair=card.pair;
  let chart=state.charts.get(pair.id);
  if(!chart) {
    chart=echarts.init(card.plot,null,{renderer:"canvas"});
    state.charts.set(pair.id,chart);
    chart.on("updateAxisPointer",event=>{
      if(state.pointerBusy || !event.axesInfo?.length) return;
      const value=event.axesInfo[0].value;
      state.pointerBusy=true;
      for(const [id,other] of state.charts) if(id!==pair.id && state.visible.has(id))
        other.dispatchAction({type:"updateAxisPointer",x:other.convertToPixel({xAxisIndex:0},value),y:60});
      state.pointerBusy=false;
    });
  }
  const series=[];
  for(const direction of selectedDirections(pair)) {
    const index=direction===pair.directions[0]?0:1, d=state.data.directions[String(direction)];
    const label=directionLabel(pair,direction);
    series.push({name:label,id:`mean-${direction}`,type:"line",data:stepPoints(d.bins,2),
      showSymbol:false,lineStyle:{width:1.5,color:colors[index]},itemStyle:{color:colors[index]},
      areaStyle:{color:colors[index],opacity:.045},animation:false});
    if($("showPeak").checked) series.push({name:`${label} 峰值`,id:`peak-${direction}`,type:"line",
      data:stepPoints(d.bins,3),showSymbol:false,lineStyle:{width:1,color:colors[index],opacity:.38},
      itemStyle:{color:colors[index]},animation:false});
    if($("showCapacity").checked) series.push({name:`${label} 容量`,id:`capacity-${direction}`,type:"line",
      step:"end",data:d.capacity,showSymbol:false,lineStyle:{color:index?"#dbab5a":"#bc771b",width:1.4,type:"dashed"},
      itemStyle:{color:"#bc771b"},animation:false});
  }
  chart.setOption({animation:false,grid:{left:52,right:15,top:26,bottom:30},
    title:{text:"Gbps",left:4,top:3,textStyle:{fontSize:10,fontWeight:400,color:"#8993a3"}},
    xAxis:{type:"value",min:state.range[0],max:state.range[1],axisLabel:{fontSize:9,color:"#8993a3",formatter:v=>fmt(v,4)},
      axisLine:{lineStyle:{color:"#dfe4ec"}},splitLine:{show:false},axisPointer:{show:true,snap:false,label:{show:false}}},
    yAxis:{type:"value",min:0,max:limit,axisLabel:{fontSize:9,color:"#8993a3",formatter:v=>fmt(v,3)},
      splitLine:{lineStyle:{color:"#f0f2f7"}}},
    tooltip:{trigger:"axis",confine:true,transitionDuration:0,formatter:params=>{
      if(!params.length) return "";
      const t=params[0].axisValue;
      let text=`<b>${fmt(t,6)} s</b>`;
      for(const direction of selectedDirections(pair)) {
        const d=state.data.directions[String(direction)];
        const bin=d.bins.find(r=>r[0]<=t && t<=r[1]);
        if(!bin) continue;
        text+=`<br>${directionLabel(pair,direction)}<br>均值 ${fmt(bin[2],6)} · 峰值 ${fmt(bin[3],6)} Gbps`;
        text+=`<br>采样 / 汇总区间 ${fmt(bin[0],6)}–${fmt(bin[1],6)} s`;
      }
      return text;
    }},series},true);
}
function drawVisible() {
  if(!state.data || state.data.metric!==state.metric) return;
  const limit=chartLimit();
  for(const id of state.visible) { const card=state.cards.get(id); if(!card.element.closest("[hidden]")) renderChart(card,limit); }
}

function createCards() {
  const observer=new IntersectionObserver(entries=>{
    for(const entry of entries) {
      const id=entry.target.dataset.pair;
      if(entry.isIntersecting) state.visible.add(id);
      else {
        state.visible.delete(id);
        state.charts.get(id)?.dispose(); state.charts.delete(id);
      }
    }
    drawVisible();
  },{rootMargin:"250px"});
  for(const role of ["P","D"]) {
    const pairs=state.info.pairs.filter(p=>p.group===role);
    if(!pairs.length) continue;
    const section=document.createElement("section"), heading=document.createElement("div");
    heading.className="group-heading";
    heading.innerHTML=`<div><h2>${role}–S 流量矩阵 <small>· ${pairs.length} 对</small></h2><p>${role==="P"?"Prefill":"Decode"} Leaf × Storage Leaf，共享全局时间轴</p></div><button type="button">收起</button>`;
    const grid=document.createElement("div"); grid.className="chart-grid group-content"; grid.id=`grid-${role}`;
    grid.style.setProperty("--columns",Math.min(4,state.info.roles.S.length));
    heading.querySelector("button").onclick=event=>{
      grid.hidden=!grid.hidden; event.target.textContent=grid.hidden?"展开":"收起";
      requestAnimationFrame(()=>{for(const chart of state.charts.values()) chart.resize();drawVisible();});
    };
    for(const pair of pairs) {
      const element=document.createElement("article"); element.className="chart-card"; element.id=`pair-${pair.id}`; element.dataset.pair=pair.id;
      const title=document.createElement("div"); title.className="chart-heading";
      const name=document.createElement("h3"); name.textContent=pair.label;
      const small=document.createElement("small"); small.textContent="全局时间联动";
      title.append(name,small);
      const plot=document.createElement("div"); plot.className="chart";
      element.append(title,plot); grid.append(element);
      state.cards.set(pair.id,{pair,element,plot}); observer.observe(element);
      plot.addEventListener("wheel",event=>{
        event.preventDefault();
        const bounds=plot.getBoundingClientRect();
        const anchor=Math.max(0,Math.min(1,(event.clientX-bounds.left-52)/Math.max(1,bounds.width-67)));
        zoom(Math.exp(Math.max(-.5,Math.min(.5,event.deltaY*.002))),anchor);
      },{passive:false});
      let drag=null;
      plot.addEventListener("pointerdown",event=>{
        if(event.button!==0) return;
        drag={x:event.clientX,range:[...state.range]}; plot.setPointerCapture(event.pointerId);
      });
      plot.addEventListener("pointermove",event=>{
        if(!drag || Math.abs(event.clientX-drag.x)<3) return;
        const shift=(event.clientX-drag.x)/Math.max(1,plot.clientWidth-67)*(drag.range[1]-drag.range[0]);
        setRange(drag.range[0]-shift,drag.range[1]-shift);
      });
      const release=()=>{if(drag){drag=null;clearTimeout(state.timer);loadSeries();}};
      plot.addEventListener("pointerup",release); plot.addEventListener("pointercancel",release);
    }
    section.append(heading,grid); $("groups").append(section);
  }
}
function sortCards() {
  const mode=$("sort").value;
  for(const role of ["P","D"]) {
    let pairs=state.info.pairs.filter(p=>p.group===role);
    const key={peak:"peak_bps",mean:"mean_bps",hot:"hot90_seconds"}[mode];
    if(key) pairs=pairs.slice().sort((a,b)=>summaryValue(b,key)-summaryValue(a,key));
    const grid=$(`grid-${role}`); if(!grid) continue;
    for(const pair of pairs) grid.append(state.cards.get(pair.id).element);
  }
}
function jump(pair) {
  const element=state.cards.get(pair.id).element;
  element.parentElement.hidden=false;
  element.scrollIntoView({behavior:"smooth",block:"center"});
  element.classList.add("highlight"); setTimeout(()=>element.classList.remove("highlight"),1800);
}
function drawSummaries() {
  const heatmaps=$("heatmaps"); heatmaps.replaceChildren();
  const scale=state.info.throughput_scale/1e9;
  for(const role of ["P","D"]) {
    if(!state.info.roles[role].length) continue;
    const table=document.createElement("table"); table.className="heatmap";
    const top=table.insertRow(); top.append(document.createElement("th"));
    for(const storage of state.info.roles.S) {const th=document.createElement("th");th.textContent=`S${storage}`;top.append(th);}
    for(const compute of state.info.roles[role]) {
      const row=table.insertRow(),th=document.createElement("th"); th.textContent=`${role}${compute}`; row.append(th);
      for(const storage of state.info.roles.S) {
        const pair=state.info.pairs.find(p=>p.compute===compute && p.storage===storage), td=row.insertCell();
        const ratios=selectedDirections(pair).map(d=>summary(pair,d)?.peak_ratio).filter(v=>v!=null);
        const ratio=ratios.length?Math.max(...ratios):null;
        td.textContent=pct(ratio); td.style.background=ratio==null?"#eef0f5":`hsl(${230-Math.min(1.25,ratio)*155} 65% ${96-Math.min(1.25,ratio)*20}%)`;
        td.title=`${pair.label} · 全程峰值 ${fmt(summaryValue(pair,"peak_bps")*scale)} Gbps`;
        td.tabIndex=0;td.setAttribute("role","button");td.onclick=()=>jump(pair);
        td.onkeydown=e=>{if(e.key==="Enter" || e.key===" "){e.preventDefault();jump(pair);}};
      }
    }
    heatmaps.append(table);
  }
  const body=$("statistics"); body.replaceChildren();
  const pairs=state.info.pairs.slice().sort((a,b)=>summaryValue(b,"peak_bps")-summaryValue(a,"peak_bps"));
  for(const pair of pairs) for(const direction of selectedDirections(pair)) {
    const s=summary(pair,direction),row=document.createElement("tr");
    const values=[directionLabel(pair,direction),...['mean_bps','peak_bps','p95_bps','p99_bps'].map(k=>fmt(s[k]*scale,6)),
      pct(s.mean_ratio),pct(s.peak_ratio),fmt(s.hot80_seconds,6),fmt(s.hot90_seconds,6)];
    for(const value of values) {const td=document.createElement("td");td.textContent=value;row.append(td);}
    body.append(row);
  }
  $("metricNote").textContent=state.metric==="payload"?
    "业务载荷：源 Leaf 成功送入 Fabric 的有向 OD 发送量，含重传；与直连容量之比不是物理链路利用率。":
    "物理吞吐：所有并行直连链路的实际发送量，含协议开销、重传和其他经过流量；容量按单方向统计。";
  $("ratioTitle").textContent=state.metric==="wire"?"平均利用率":"均值 / 直连容量";
  $("statsNote").textContent="统计覆盖整个记录，含空闲窗口；P95 / P99 按窗口时长加权。高负载时间按原始窗口计算，未连接时不计算利用率。";
  sortCards();
}

async function loadOverview() {
  const metric=state.metric;
  const params=new URLSearchParams({start:state.info.start,end:state.info.end,metric,points:300});
  const data=await getJSON(`/api/series?${params}`);
  if(state.metric!==metric) return;
  const all=Object.values(data.directions), first=all[0].bins;
  const total=first.map((row,index)=>[row[0],all.reduce((sum,d)=>sum+d.bins[index][2],0)]);
  state.overviewBusy=true;
  state.overview.setOption({animation:false,grid:{left:52,right:16,top:18,bottom:43},
    title:{text:"全程总发送均值 · 拖动下方选区统一缩放",left:4,top:0,textStyle:{fontSize:10,fontWeight:400,color:"#7c8799"}},
    xAxis:{type:"value",min:state.info.start,max:state.info.end,axisLabel:{show:false},splitLine:{show:false}},
    yAxis:{type:"value",axisLabel:{fontSize:9,color:"#8993a3"},splitLine:{show:false}},
    dataZoom:[{type:"slider",xAxisIndex:0,bottom:2,height:22,startValue:state.range[0],endValue:state.range[1],
      filterMode:"none",realtime:true,showDetail:false,brushSelect:true}],
    series:[{type:"line",data:total,showSymbol:false,lineStyle:{width:1.2,color:"#8880c8"},areaStyle:{color:"#e9e7f8"}}]},true);
  state.overviewBusy=false;
}
async function initialize() {
  try {
    state.info=await getJSON("/api/bootstrap"); state.metric=state.info.metrics[0];
    $("metric").value=state.metric;
    for(const option of $("metric").options) option.disabled=!state.info.metrics.includes(option.value);
    $("mode").textContent=`${state.info.case_id} · ${state.info.mode}${state.info.synthetic?" · 合成数据":""}`;
    const facts=[["交换机对",state.info.pairs.length],["记录时长",`${fmt(state.info.end-state.info.start)} s`],
      ["采样间隔",`${fmt(state.info.sample_interval_seconds*1000,6)} ms`],["速率 / 容量倍率",`${fmt(state.info.throughput_scale)}×`]];
    for(const [label,value] of facts){const item=document.createElement("div");item.className="fact";item.append(document.createTextNode(label));const b=document.createElement("b");b.textContent=value;item.append(b);$("facts").append(item);}
    $("warnings").textContent=state.info.warnings.join(" ");
    createCards(); drawSummaries();
    state.overview=echarts.init($("overview"));
    state.overview.on("datazoom",event=>{
      if(state.overviewBusy) return;
      const item=event.batch?.[0] || event;
      const span=state.info.end-state.info.start;
      setRange(item.startValue??state.info.start+span*item.start/100,item.endValue??state.info.start+span*item.end/100);
    });
    setRange(state.info.start,state.info.end,true);
    await loadOverview();
    $("timeForm").onsubmit=event=>{
      event.preventDefault();const start=Number($("start").value),end=Number($("end").value);
      if(!Number.isFinite(start)||!Number.isFinite(end)||end<=start||start<state.info.start||end>state.info.end){status("请输入记录范围内有效的开始和结束时间。",true);return;}
      setRange(start,end,true);
    };
    $("reset").onclick=()=>setRange(state.info.start,state.info.end,true);
    $("zoomIn").onclick=()=>zoom(.5);$("zoomOut").onclick=()=>zoom(2);
    $("panLeft").onclick=()=>{const[a,b]=state.range;setRange(a-(b-a)*.3,b-(b-a)*.3);};
    $("panRight").onclick=()=>{const[a,b]=state.range;setRange(a+(b-a)*.3,b+(b-a)*.3);};
    $("metric").onchange=()=>{state.metric=$("metric").value;drawSummaries();loadSeries();loadOverview().catch(e=>status(e.message,true));};
    $("direction").onchange=()=>{drawSummaries();drawVisible();};
    for(const id of ["yscale","showCapacity","showPeak"]) $(id).onchange=drawVisible;
    $("sort").onchange=sortCards;
    let resizeTimer;window.addEventListener("resize",()=>{clearTimeout(resizeTimer);resizeTimer=setTimeout(()=>{state.overview.resize();for(const chart of state.charts.values())chart.resize();loadSeries();},180);});
  } catch(error) {status(`初始化失败：${error.message}`,true);}
}
initialize();
