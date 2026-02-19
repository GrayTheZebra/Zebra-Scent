#include "WebUi.h"

const char UI_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zebra-Scent</title>
<style>
  :root{--bg:#0b0f14;--card:#121a24;--muted:#93a4b8;--text:#e7eef8;--ok:#22c55e;--bad:#ef4444;}
  body{margin:0;font-family:system-ui,Segoe UI,Roboto,Arial;background:var(--bg);color:var(--text);}
  header{padding:16px 18px;border-bottom:1px solid #1f2a37;display:flex;gap:12px;align-items:center;justify-content:space-between;}
  h1{font-size:18px;margin:0;letter-spacing:.3px;}
  .wrap{padding:18px;max-width:980px;margin:0 auto;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}
  .card{background:var(--card);border:1px solid #1f2a37;border-radius:14px;padding:14px;box-shadow:0 8px 20px rgba(0,0,0,.25);}
  .row{display:flex;justify-content:space-between;align-items:center;gap:10px;}
  .muted{color:var(--muted);font-size:13px;}
  .pill{font-size:12px;padding:4px 8px;border-radius:999px;border:1px solid #243244;color:var(--muted);}
  .pill.ok{color:var(--ok);border-color:rgba(34,197,94,.35);}
  .pill.bad{color:var(--bad);border-color:rgba(239,68,68,.35);}
  button{background:#0f172a;color:var(--text);border:1px solid #263446;border-radius:12px;padding:10px 12px;cursor:pointer;}
  button:hover{border-color:#3b82f6;}
  button.on{background:rgba(34,197,94,.18);border-color:rgba(34,197,94,.35);}
  button.off{background:rgba(239,68,68,.12);border-color:rgba(239,68,68,.25);}
  .btnrow{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;}
  input,select{background:#0f172a;color:var(--text);border:1px solid #263446;border-radius:10px;padding:10px;width:100%;}
  .table{width:100%;border-collapse:collapse;}
  .table td,.table th{border-bottom:1px solid #1f2a37;padding:10px 8px;text-align:left;font-size:13px;}
  .table th{color:var(--muted);font-weight:600;}
  footer{padding:14px 18px;color:var(--muted);font-size:12px;}
  code{color:#cbd5e1;}
</style>
</head><body>
<header>
  <h1>Zebra-Scent</h1>
  <div class="row">
    <span class="pill" id="ip">IP: …</span>
    <span class="pill" id="mqtt">MQTT: …</span>
    <span class="pill" id="time">Time: …</span>
  </div>
</header>

<div class="wrap">
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:800">Alle Diffuser</div>
        <div class="muted" id="allStatus">—</div>
      </div>
      <div class="btnrow">
        <button class="on" onclick="setAll(1)">Alles AN</button>
        <button class="off" onclick="setAll(0)">Alles AUS</button>
      </div>
    </div>
  </div>

  <div style="height:14px"></div>
  <div class="grid" id="cards"></div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Namen</div>
        <div class="muted">Nur gespeichert wenn du auf “Namen speichern” klickst.</div>
      </div>
      <div class="btnrow">
        <button onclick="saveNames()">Namen speichern</button>
        <button onclick="loadNames()">Neu laden</button>
      </div>
    </div>
    <div style="height:10px"></div>
    <div class="grid" id="nameGrid" style="grid-template-columns:repeat(auto-fit,minmax(240px,1fr));"></div>
  </div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Zeitpläne</div>
        <div class="muted">“Ausgang X von HH:MM bis HH:MM an” • täglich oder nach Wochentagen</div>
      </div>
      <button onclick="addRule()">+ Regel</button>
    </div>
    <div style="height:10px"></div>
    <table class="table" id="schedTable">
      <thead><tr><th>#</th><th>Aktiv</th><th>Kanal</th><th>Von</th><th>Bis</th><th>Tage</th><th></th></tr></thead>
      <tbody></tbody>
    </table>
    <div class="btnrow">
      <button onclick="saveAllSchedules()">Speichern</button>
      <button onclick="reloadSchedules()">Neu laden</button>
    </div>
  </div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Einstellungen</div>
        <div class="muted">MQTT bearbeiten • WLAN neu einrichten (AP via WiFiManager)</div>
      </div>
      <div class="btnrow">
        <button onclick="saveMqtt()">MQTT speichern</button>
        <button onclick="wifiReset()" class="off">WLAN neu</button>
      </div>
    </div>
    <div style="height:10px"></div>
    <div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(240px,1fr));">
      <div><div class="muted">MQTT Host (leer = MQTT aus)</div><input id="mqttHost"></div>
      <div><div class="muted">MQTT Port</div><input id="mqttPort"></div>
      <div><div class="muted">MQTT User</div><input id="mqttUser"></div>
      <div><div class="muted">MQTT Pass (leer = unverändert)</div><input id="mqttPass" type="password"></div>
      <div><div class="muted">BaseTopic</div><input id="baseTopic"></div>
      <div><div class="muted">HA Discovery Prefix</div><input id="haPrefix"></div>
    </div>
  </div>
</div>

<footer>
  Home Assistant: 8 Switches + 1 Master-Switch per MQTT Discovery.
</footer>

<script>
let state={mask:0,timeOk:false,nowMin:0,ip:"",names:[],allOn:false,anyOn:false};
let rules=[];
let namesUiInitialized=false;   // <-- key: init once and do not re-render in refresh loop

function bitOn(mask,idx){return (mask&(1<<idx))!==0;}
function minToHHMM(m){const h=String(Math.floor(m/60)).padStart(2,'0');const mm=String(m%60).padStart(2,'0');return h+":"+mm;}
function hhmmToMin(s){const p=s.split(':');if(p.length!==2)return 0;const h=parseInt(p[0],10),m=parseInt(p[1],10);if(isNaN(h)||isNaN(m))return 0;return (Math.max(0,Math.min(23,h))*60+Math.max(0,Math.min(59,m)));}
function daysToText(mask){const names=["Mo","Di","Mi","Do","Fr","Sa","So"];let out=[];for(let i=0;i<7;i++)if(mask&(1<<i))out.push(names[i]);return out.length?out.join(","):"—";}
async function apiGet(url){const r=await fetch(url);return await r.json();}

async function setCh(ch,v){await fetch(`/api/set?ch=${ch}&v=${v?1:0}`,{method:'POST'});await refreshState();}
async function setAll(v){await fetch(`/api/set?ch=0&v=${v?1:0}`,{method:'POST'});await refreshState();}

function renderCards(){
  const el=document.getElementById('cards');el.innerHTML='';
  for(let i=1;i<=8;i++){
    const on=bitOn(state.mask,i-1);
    const nm=(state.names && state.names[i-1]) ? state.names[i-1] : `Diffuser ${i}`;
    const card=document.createElement('div');
    card.className='card';
    card.innerHTML=`
      <div class="row">
        <div>
          <div style="font-weight:800">${nm}</div>
          <div class="muted">Kanal ${i} • Status: <b>${on?'ON':'OFF'}</b></div>
        </div>
        <button class="${on?'on':'off'}" onclick="setCh(${i},${on?0:1})">${on?'AUS':'AN'}</button>
      </div>
      <div class="btnrow">
        <button onclick="setCh(${i},1)">AN</button>
        <button onclick="setCh(${i},0)">AUS</button>
        <button onclick="setCh(${i},${on?0:1})">Toggle</button>
      </div>`;
    el.appendChild(card);
  }
}

function updateAllStatus(){
  const el = document.getElementById('allStatus');
  if(state.allOn) el.textContent="Status: ALLE AN";
  else if(state.anyOn) el.textContent="Status: TEILWEISE AN";
  else el.textContent="Status: ALLE AUS";
}

function initNamesUI(){
  const g=document.getElementById('nameGrid'); g.innerHTML='';
  for(let i=1;i<=8;i++){
    const div=document.createElement('div');
    div.innerHTML = `
      <div class="muted">Kanal ${i}</div>
      <input id="nm${i}" placeholder="z.B. Rosenduft">`;
    g.appendChild(div);
  }
  namesUiInitialized=true;
}

function fillNamesInputsFromState(){
  for(let i=1;i<=8;i++){
    const el = document.getElementById('nm'+i);
    if(!el) continue;
    const nm=(state.names && state.names[i-1]) ? state.names[i-1] : `Diffuser ${i}`;
    el.value = nm;
  }
}

async function loadNames(){
  const s = await apiGet('/api/names');
  state.names = s.names || state.names;
  if(!namesUiInitialized) initNamesUI();
  fillNamesInputsFromState();
  renderCards();
}

async function saveNames(){
  const arr=[];
  for(let i=1;i<=8;i++){
    const v = document.getElementById('nm'+i).value.trim();
    arr.push(v.length ? v : `Diffuser ${i}`);
  }
  await fetch('/api/names', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({names: arr})});
  await refreshState();
  alert("Gespeichert. HA Discovery wurde (falls MQTT online) neu gesendet.");
}

// Schedules
function renderSchedules(){
  const tb=document.querySelector('#schedTable tbody');tb.innerHTML='';
  rules.forEach((r,idx)=>{
    const tr=document.createElement('tr');
    tr.innerHTML=`
      <td>${idx}</td>
      <td><input type="checkbox" ${r.en?'checked':''} onchange="rules[${idx}].en=this.checked"></td>
      <td><select onchange="rules[${idx}].ch=parseInt(this.value,10)">
        ${Array.from({length:8},(_,i)=>`<option value="${i+1}" ${r.ch==i+1?'selected':''}>${i+1}</option>`).join('')}
      </select></td>
      <td><input value="${minToHHMM(r.s)}" onchange="rules[${idx}].s=hhmmToMin(this.value)"></td>
      <td><input value="${minToHHMM(r.e)}" onchange="rules[${idx}].e=hhmmToMin(this.value)"></td>
      <td><button onclick="toggleDays(${idx})">${daysToText(r.d)}</button></td>
      <td><button onclick="delRule(${idx})">löschen</button></td>`;
    tb.appendChild(tr);
  });
}
function toggleDays(idx){
  const cur=daysToText(rules[idx].d);
  const inp=prompt("Tage (Mo,Di,Mi,Do,Fr,Sa,So) kommasepariert oder 'all'/'none':",cur);
  if(inp===null)return;
  const v=inp.trim().toLowerCase();
  if(v==="all"){rules[idx].d=0x7F;renderSchedules();return;}
  if(v==="none"){rules[idx].d=0x00;renderSchedules();return;}
  const map={mo:0,di:1,mi:2,do:3,fr:4,sa:5,so:6};
  let m=0;v.split(',').map(x=>x.trim()).forEach(x=>{if(map[x]!==undefined)m|=(1<<map[x]);});
  rules[idx].d=m;renderSchedules();
}
function addRule(){if(rules.length>=16){alert("Max 16 Regeln");return;}rules.push({en:true,ch:1,s:8*60,e:9*60,d:0x7F});renderSchedules();}
function delRule(idx){rules.splice(idx,1);renderSchedules();}
async function saveAllSchedules(){
  const fixed=Array.from({length:16},(_,i)=>rules[i]||{en:false,ch:1,s:0,e:0,d:0x7F});
  await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({rules:fixed})});
  alert("Gespeichert");
}
async function reloadSchedules(){
  const s=await apiGet('/api/schedules');
  rules=(s.rules||[]).slice(0,16);
  while(rules.length && !rules[rules.length-1].en && rules.length>1) rules.pop();
  renderSchedules();
}

// MQTT config
async function loadMqtt(){
  const c = await apiGet('/api/mqtt');
  mqttHost.value  = c.mqttHost || "";
  mqttPort.value  = c.mqttPort || 1883;
  mqttUser.value  = c.mqttUser || "";
  baseTopic.value = c.baseTopic || "zebrascent";
  haPrefix.value  = c.haPrefix || "homeassistant";
}
async function saveMqtt(){
  const body={
    mqttHost:mqttHost.value.trim(),
    mqttPort:parseInt(mqttPort.value.trim()||"1883",10),
    mqttUser:mqttUser.value.trim(),
    baseTopic:(baseTopic.value.trim()||"zebrascent"),
    haPrefix:(haPrefix.value.trim()||"homeassistant")
  };
  const pass=mqttPass.value; if(pass && pass.length) body.mqttPass=pass;
  await fetch('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  mqttPass.value="";
  alert("Gespeichert. MQTT reconnect + HA Discovery neu.");
}
async function wifiReset(){
  if(!confirm("WLAN wirklich zurücksetzen? Danach startet ein AP zur Neueinrichtung.")) return;
  await fetch('/wifireset',{method:'POST'});
}

async function refreshState(){
  state=await apiGet('/api/state');
  document.getElementById('ip').textContent="IP: "+(state.ip||"—");
  document.getElementById('mqtt').textContent="MQTT: "+(state.mqttEnabled?(state.mqttConnected?"connected":"offline"):"disabled");
  document.getElementById('mqtt').className="pill "+(state.mqttEnabled?(state.mqttConnected?"ok":"bad"):"");
  document.getElementById('time').textContent="Time: "+(state.timeOk?minToHHMM(state.nowMin):"not set");
  document.getElementById('time').className="pill "+(state.timeOk?"ok":"bad");

  updateAllStatus();
  renderCards();

  // IMPORTANT: do NOT touch name inputs here -> keeps focus
  if(!namesUiInitialized){ initNamesUI(); fillNamesInputsFromState(); }
}

(async()=>{
  await loadMqtt();
  await refreshState();
  await loadNames();        // names only loaded once initially
  await reloadSchedules();
  setInterval(refreshState,1500);
})();
</script>
</body></html>
)HTML";
