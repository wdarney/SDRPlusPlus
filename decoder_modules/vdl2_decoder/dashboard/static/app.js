'use strict';
const $ = id => document.getElementById(id);
const form = $('filters');
let live = true, busy = false, next = null, revision = 0;
let deleting = false, deleteToken = null, generation = null;
let filters = new URLSearchParams();
const shown = new Set();
function el(tag, text, cls) { const n = document.createElement(tag); if (text !== undefined) n.textContent = text; if (cls) n.className = cls; return n; }
function scalar(v) { return v === undefined || v === null ? '—' : typeof v === 'object' ? JSON.stringify(v) : String(v); }
function tree(value, name, depth=0) {
  if (value === null || typeof value !== 'object' || depth > 30) return el('div', `${name}: ${scalar(value)}`, 'leaf');
  const d = el('details', undefined, 'tree'); d.append(el('summary', `${name} · ${Array.isArray(value) ? value.length + ' items' : Object.keys(value).length + ' fields'}`));
  d.addEventListener('toggle', () => { if (d.open && d.childElementCount === 1) for (const [k,v] of Object.entries(value)) d.append(tree(v,k,depth+1)); });
  return d;
}
function decimal(v) { return typeof v === 'number' ? v.toFixed(1) : '—'; }
function row(entry) {
  const m = entry.message, tr = el('tr'); tr.dataset.id = entry.id;
  if (m.protocol === 'ADS-C') tr.className = 'adsc';
  function cell(main, sub, cls) { const td=el('td'); td.append(el('span', scalar(main), cls)); if(sub !== undefined) td.append(el('small',scalar(sub))); tr.append(td); return td; }
  const date = typeof m.timestamp === 'number' ? new Date(m.timestamp * 1000) : null;
  const stamp = date && Number.isFinite(date.getTime()) ? date.toISOString() : null;
  cell(stamp ? stamp.slice(11,23) : '—', stamp ? stamp.slice(0,10) : undefined);
  cell(m.protocol || 'Legacy record', m.transport, 'protocol'); cell(m.tail || m.src?.address, m.flight);
  cell(m.direction, `${m.src?.type || '?'} ${m.src?.address || '—'} → ${m.dst?.type || '?'} ${m.dst?.address || '—'}`);
  cell(m.schema_version === 2 && typeof m.freq === 'number' ? (m.freq/1e6).toFixed(3) : '—', `SNR ${decimal(m.snr)} dB · FEC ${scalar(m.fec)} · PPM ${decimal(m.ppm)}`);
  const td=el('td'), d=el('details');
  const text= typeof m.text === 'string' ? m.text : '';
  d.append(el('summary',text ? text.slice(0,160) + (text.length>160?'…':'') : 'Inspect decoded message'));
  d.addEventListener('toggle',()=>{ if(!d.open || d.childElementCount>1)return;
    const detail=el('div',undefined,'detail');detail.append(el('h3','Human-readable message'),el('pre',text || 'No display text'));
    detail.append(el('h3','Decoded protocol tree'),tree(m.decoded || {},'decoded'));
    const metadata={...m};delete metadata.decoded;delete metadata.text;
    detail.append(el('h3','Reception metadata'),tree(metadata,'metadata'));
    const raw=el('details');raw.append(el('summary','Complete JSON'),el('pre',JSON.stringify(m,null,2)));detail.append(raw);d.append(detail);
  });td.append(d);tr.append(td);return tr;
}
async function get(url) {const r=await fetch(url);if(!r.ok)throw Error(`HTTP ${r.status}`);return r.json();}
async function refresh(older=false) {
  if(busy || deleting)return; busy=true;const version=revision;
  try {
    const q=new URLSearchParams(filters); if(older && next)q.set('before',next);
    const [data,stats]=await Promise.all([get('/api/messages?'+q),get('/api/status')]);
    if(version!==revision)return;
    deleteToken=stats.delete_token;
    if(generation !== null && generation !== data.generation) {
      shown.clear();$('messages').replaceChildren();next=null;
      if(older){revision++;setTimeout(()=>refresh(),0);generation=data.generation;return;}
    }
    generation=data.generation;
    if(!older) { // Refresh the live window while preserving expanded existing rows.
      const existing=new Map([...$('messages').children].map(n=>[Number(n.dataset.id),n]));
      shown.clear();const fragment=document.createDocumentFragment();
      for(const e of data.messages){shown.add(e.id);fragment.append(existing.get(e.id)||row(e));}
      $('messages').replaceChildren(fragment);
    } else for(const e of data.messages)if(!shown.has(e.id)){shown.add(e.id);$('messages').append(row(e));}
    next=data.next_before;$('older').hidden=!next;$('empty').hidden=shown.size>0;
    $('total').textContent=stats.total.toLocaleString();
    $('paths').textContent=stats.paths.map(p=>`${p.protocol || 'Legacy'} / ${p.transport || '—'}: ${p.count}`).join(' · ') || 'Awaiting reception';
    $('reader').textContent=stats.reader.state==='watching'?'Watching file':stats.reader.state==='waiting'?'Waiting for SDR++':'Reader error';
    const invalid=stats.reader.invalid_lines || 0;
    $('status').textContent=stats.reader.error || `${live?'Live · Latest 200':'Paused'} · ${shown.size} shown · ${invalid} invalid lines skipped`;
    $('status').className=stats.reader.state==='error'?'error':'';
    const selected=$('freq').value;
    $('freq').replaceChildren(new Option('All frequencies',''),...stats.frequencies.map(f=>new Option((f/1e6).toFixed(3)+' MHz',String(f))));$('freq').value=selected;
    // Include generic network/control protocols actually present in the feed.
    const p=form.elements.protocol, values=new Set([...p.options].map(o=>o.value));
    for(const path of stats.paths)if(path.protocol&&!values.has(path.protocol)){p.add(new Option(path.protocol,path.protocol));values.add(path.protocol);}
  } catch(e) {$('status').textContent='Dashboard disconnected · '+e.message;$('status').className='error';}
  finally {busy=false;}
}
function apply() {
  revision++;filters=new URLSearchParams();
  for(const [k,v] of new FormData(form))if(v && k!=='day')filters.set(k,v.trim());
  const day=form.elements.day.value;
  if(day){const start=Date.parse(day+'T00:00:00Z')/1000;filters.set('since',start);filters.set('until',start+86400);}
  next=null;shown.clear();$('messages').replaceChildren();
  // A filter change during a request gets its own refresh after that request.
  const retry=()=>busy?setTimeout(retry,50):refresh();retry();
}
form.addEventListener('submit',e=>{e.preventDefault();apply();});
form.addEventListener('reset',()=>setTimeout(apply,0));
$('live').onclick=()=>{live=!live;$('live').textContent=live?'Pause live':'Resume live';if(live)refresh();else $('status').textContent='Paused · reception continues in background';};
$('older').onclick=()=>{live=false;$('live').textContent='Resume live';refresh(true);};
setInterval(()=>{if(live)refresh();},3000);refresh();

$('delete-all').onclick=async()=>{
  if(deleting || !deleteToken)return;
  if(!window.confirm('Delete ALL stored dashboard messages, including those hidden by filters? This cannot be undone. Existing JSONL contents will be skipped; the source file stays intact. New messages will continue to appear.'))return;
  deleting=true;revision++;$('delete-all').disabled=true;
  try {
    const response=await fetch('/api/messages/delete-all',{
      method:'POST',headers:{'Content-Type':'application/json','X-Dashboard-Token':deleteToken},
      body:JSON.stringify({confirm:true})
    });
    const result=await response.json();
    if(!response.ok)throw Error(result.error || 'Delete failed');
    shown.clear();$('messages').replaceChildren();next=null;
    $('total').textContent='0';$('paths').textContent='Awaiting reception';
    $('older').hidden=true;$('empty').hidden=false;
    $('status').textContent=`Deleted ${result.deleted} stored messages. Waiting for new reception.`;
    // Refresh even when paused, after any older request has drained.
    const reload=()=>busy?setTimeout(reload,50):refresh();setTimeout(reload,0);
  } catch(e) {$('status').textContent=e.message;$('status').className='error';}
  finally {deleting=false;$('delete-all').disabled=false;}
};
