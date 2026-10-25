/* The configuration backup (issue #979, 2026-10-11): three cards for System >
   Maintenance over /api/backup (config_backup/HTTP_API.md). An on-demand chunk
   (/ui/backup.js): PAGES.system loads it into the Maintenance pane.

   1. Configuration backup: a file for the computer with the five parts that may
      leave the device, each a tick; Remove sensitive data; Encrypt the file with
      the device's backup key.
   2. Copies on the device: the full copy, certificates included, on the internal
      flash and the SD card, each with its own Restore; the key, downloadable.
   3. Restore from a file: the preview of what a backup holds before anything
      changes.
   Key material never leaves the device: no certificates part in a file, and the
   file browser cannot reach /data/certs or the copies. */
(()=>{
const PARTS=[
  {id:"settings",title:"Settings",
   help:"Every settings page: WiFi, Bluetooth, CAN bus, connections, MQTT, VPN, power saving, USB, logging, rules and timers, data destinations, Home Assistant.",
   count:s=>s.groups+" groups"},
  {id:"vehicles",title:"Vehicles and parameters",
   help:"Your cars with their PID tables and filters, the standard-PID scan and the chosen vehicle profile.",
   count:s=>s.cars+(s.cars===1?" car":" cars")+" · "+s.pids+" PIDs"},
  {id:"databases",title:"Code databases and DBC files",
   help:"Trouble-code databases and CAN databases you uploaded.",
   count:s=>{const n=(s.dtc_db||0)+(s.dbc||0);return n+(n===1?" file":" files");}},
  {id:"scripts",title:"Scripts",help:"Stored Berry scripts.",
   count:s=>s.count+(s.count===1?" script":" scripts")},
  {id:"dashboard",title:"Dashboard layout",help:"Tile order, sizes and ranges.",
   count:s=>s.present?"1 file":"none"},
  {id:"certs",title:"Certificates",localOnly:true,
   help:"Certificate sets for MQTT, Home Assistant and HTTPS destinations: CA, client certificate and client key.",
   count:s=>s.sets+(s.sets===1?" set":" sets")},
];
const kb=b=>fmt.bytes(b);
const day=iso=>{if(!iso)return"";const d=new Date(iso);return isNaN(d)?iso:d.toLocaleDateString(undefined,{day:"numeric",month:"short",year:"numeric"});};
const when=iso=>{if(!iso)return"unknown date";const d=new Date(iso);return isNaN(d)?iso:day(iso)+", "+d.toLocaleTimeString(undefined,{hour:"2-digit",minute:"2-digit"});};
const today=()=>{const d=new Date();return d.getFullYear()+"-"+String(d.getMonth()+1).padStart(2,"0")+"-"+String(d.getDate()).padStart(2,"0");};
const heading=(t,first)=>h("div",{class:"bk-h",style:"font-size:12px;font-weight:600;color:var(--text-3);text-transform:uppercase;letter-spacing:.04em;"+(first?"margin-top:6px":"margin-top:18px;padding-top:14px;border-top:1px solid var(--border)")},t);
const hasData=(p,d)=>!!d&&(p.id==="dashboard"?d.present:true);
/* what a part leaves out (issue #989): [{name, why}], the last one a bare
   "and N more"; a note under the row instead of a silent gap */
const leftOut=(l,lead)=>Array.isArray(l)&&l.length?lead+": "+l.map(x=>x.name?x.name+" ("+x.why+")":x.why).join(", ")+".":null;

/* one row of a parts list: tick | words | count and size (or why it is out) */
function partRow(p,data,picked,onPick,state){
  const avail=hasData(p,data)&&!state.blocked;
  const cb=h("input",{type:"checkbox",checked:avail&&picked.has(p.id),disabled:!avail,onchange:e=>onPick(p.id,e.target.checked)});
  const why=!hasData(p,data)?(state.absentWhy||"Nothing to save"):state.blocked;
  const right=avail?h("div",{class:"help mono",style:"text-align:right;white-space:nowrap"},p.count(data),h("br"),kb(data.bytes))
    :h("div",{class:"help",style:"text-align:right;max-width:190px;color:"+(state.blocked?"var(--text-2)":"var(--text-3)")},why);
  return h("label",{class:"bk-row","data-part":p.id,style:"display:grid;grid-template-columns:22px 1fr auto;gap:2px 12px;align-items:center;padding:9px 4px;border-top:1px solid var(--border);cursor:"+(avail?"pointer":"default")+";opacity:"+(avail?"1":".7")},
    cb,h("div",{},h("b",{style:"font-size:13.5px"},p.title),h("div",{class:"help"},p.help),
      state.note?h("div",{class:"help",style:"color:var(--warning);margin-top:2px"},state.note):null),right);
}

/* the key's state, shared by the two cards; the device marks it downloaded
   when it serves the file, so the state is re-read after a click */
function keyBits(st,repaint){
  const state=h("span",{class:"help"});
  const btn=h("a",{class:"btn sm bk-key",href:API+"/api/backup/key",download:"wican-"+st.device_id+"-backup-key.txt",onclick:()=>{setTimeout(repaint,1500);}},ic("key"),"Download key");
  const paint=()=>{const k=st.key||{};
    state.replaceChildren(k.downloaded?chip("downloaded "+(day(k.downloaded_at)||"before"),"ok"):chip("never downloaded","warn")," ",h("span",{},k.created?"created "+day(k.created):"created at first start"));};
  paint();
  return {btn,state,paint,downloaded:()=>!!(st.key&&st.key.downloaded)};
}

/* ---- card 1: the downloadable backup ---- */
function backupCard(st,repaintAll){
  const picked=new Set(PARTS.filter(p=>!p.localOnly&&hasData(p,st.parts[p.id])).map(p=>p.id));
  let redact=false,encrypt=false;
  const list=h("div",{class:"bk-list"});
  const master=h("input",{type:"checkbox",checked:true,onchange:e=>{PARTS.filter(p=>!p.localOnly).forEach(p=>{if(e.target.checked)picked.add(p.id);else picked.delete(p.id);});paint();}});
  const total=h("span",{class:"help mono"});
  const head=h("label",{class:"bk-master",style:"display:grid;grid-template-columns:22px 1fr auto;gap:12px;align-items:center;padding:6px 4px;cursor:pointer"},master,h("b",{style:"font-size:13.5px"},"Everything that may leave the device"),total);
  const sumName=h("span",{class:"mono bk-name",style:"font-weight:600;word-break:break-all"});
  const sumChips=h("span",{class:"rowflex",style:"gap:6px;flex-wrap:wrap"});
  const sumSize=h("span",{class:"help"});
  const dl=h("a",{class:"btn pri bk-download",href:"#"},ic("download"),"Download backup");
  const key=keyBits(st,repaintAll);
  const keyNudge=banner("warn","key",h("div",{},h("b",{},"Download the key before the backup.")," Without the key file an encrypted backup cannot be opened on another WiCAN, or on this one after a repair.",h("div",{style:"margin-top:8px"},key.btn)));
  const redactRow=h("div",{class:"frow","data-key":"bk_redact"},h("label",{},"Remove sensitive data"),h("div",{class:"ctl"},switchCtl(false,v=>{redact=v;paint();})),
    h("div",{class:"help"},"Leaves out passwords, keys and tokens: WiFi and access point passwords, MQTT, APN and VPN credentials, webhook addresses. The file still restores everything else, and a WiCAN restored from it keeps the passwords and keys it already has."));
  const encryptRow=h("div",{class:"frow","data-key":"bk_encrypt"},h("label",{},"Encrypt the file"),h("div",{class:"ctl"},switchCtl(false,v=>{encrypt=v;paint();})),
    h("div",{class:"help"},"AES-256-GCM with the backup key this device keeps (see Copies on the device, below). Only this WiCAN, or one given the key file, can read it."));
  const foot=h("div",{class:"help",style:"margin-top:8px"});
  function paint(){
    list.replaceChildren(...PARTS.map(p=>partRow(p,st.parts[p.id],picked,(id,on)=>{if(on)picked.add(id);else picked.delete(id);paint();},
      {blocked:p.localOnly?"Never leave the device: kept in the copies on the device, below":null,note:leftOut((st.parts[p.id]||{}).left_out,"Not in the backup")})));
    const usable=PARTS.filter(p=>!p.localOnly&&hasData(p,st.parts[p.id]));
    const n=usable.filter(p=>picked.has(p.id)).length;
    master.checked=n===usable.length&&n>0;master.indeterminate=n>0&&n<usable.length;
    let bytes=usable.filter(p=>picked.has(p.id)).reduce((a,p)=>a+(st.parts[p.id].bytes||0),0);
    if(encrypt)bytes=Math.round(bytes*1.34)+320;
    total.textContent=n+" of "+usable.length+" · about "+kb(bytes);
    sumName.textContent="wican-"+st.device_id+"-"+today()+(encrypt?"-encrypted":"")+".json";
    sumChips.replaceChildren(encrypt?chip("encrypted","ok"):chip("plain text","warn"),redact?chip("passwords removed","info"):null);
    sumSize.textContent="about "+kb(bytes)+(encrypt?", opens on this WiCAN or with the key file":redact?", safe to share":", keep it private: it holds every password");
    keyNudge.style.display=encrypt&&!key.downloaded()?"":"none";
    const blocked=n===0||(encrypt&&!key.downloaded());
    dl.classList.toggle("disabled",blocked);dl.setAttribute("aria-disabled",blocked?"true":"false");
    dl.title=n===0?"Tick at least one part":(encrypt&&!key.downloaded())?"Download the key first":"";
    const q="include="+[...picked].join(",")+(redact?"&redact=1":"")+(encrypt?"&encrypt=1":"");
    if(blocked){dl.removeAttribute("href");dl.removeAttribute("download");}
    else{dl.href=API+"/api/backup?"+q;dl.download=sumName.textContent;}
    foot.replaceChildren("Not in the file: "+(st.excluded||[]).join(", ")+".");
  }
  paint();
  const el=card("Configuration backup","download",
    h("div",{style:"font-size:13px;color:var(--text-2);margin-bottom:10px"},"A file for your computer that puts this WiCAN, or a replacement, back the way it is. Choose what goes in, then download."),
    heading("What to include",true),head,list,foot,
    heading("Protect the file"),redactRow,encryptRow,keyNudge,
    h("div",{class:"bk-file",style:"display:grid;grid-template-columns:minmax(0,1fr) auto;gap:14px;align-items:center;margin-top:14px;padding:12px 14px;background:var(--surface-2);border:1px solid var(--border);border-radius:9px"},
      h("div",{style:"display:flex;flex-direction:column;gap:5px;min-width:0"},h("div",{class:"rowflex",style:"gap:8px;flex-wrap:wrap"},sumName,sumChips),sumSize),dl));
  el.id="bk-backup-card";el.repaint=paint;
  return el;
}

/* ---- card 2: the copies the device keeps for itself ---- */
function copiesCard(st,repaintAll,autoRows){
  /* read through st: a repaint swaps st.copies for a fresh object */
  const cur=()=>st.copies||{internal:{},sd:{}};
  const key=keyBits(st,repaintAll);
  const rows=h("div",{class:"bk-copies"});
  const refreshBtn=h("button",{class:"btn sm bk-refresh",onclick:async()=>{
    const c=cur();
    /* a copy held since a factory reset (issue #987) is the undo: a refresh
       lets it go, so it asks first */
    if(((c.internal||{}).held||(c.sd||{}).held)&&!await confirmModal("A copy holds the settings from before the factory reset. Refreshing replaces them with the settings this WiCAN has now, and the undo is gone. To undo the reset instead, use Restore on that copy.","Let the copies follow the device again?"))return;
    refreshBtn.disabled=true;
    try{const r=await api("/api/backup/copies/refresh",{method:"POST"});toast("Copies refreshed: "+(r.written||[]).join(", "),"ok");await repaintAll(true);}
    catch(e){toast(e.message,"err");}
    refreshBtn.disabled=false;}},ic("refresh"),"Refresh the copies now");
  const keyNudge=banner("warn","key",h("b",{},"Download the key once.")," The copies are sealed with it. This WiCAN opens them by itself; a replacement reading the SD card needs the key file.");
  const keyRow=h("div",{class:"frow","data-key":"bk_key"},h("label",{},"Backup key"),
    h("div",{class:"ctl"},h("div",{class:"rowflex",style:"gap:10px;flex-wrap:wrap"},key.btn,key.state)),
    h("div",{class:"help"},"Made once on this device from its hardware random source and kept outside the settings, so a factory reset keeps it. Keep the key file apart from the backups. It is the same key that seals an encrypted download."));
  function placeRow(title,sub,data,where){
    /* held (issue #987): the settings part is the one from before the factory
       reset until the user restores it or lets the copy go */
    const held=!!(data&&data.present&&data.held);
    const words=h("div",{},h("b",{style:"font-size:13.5px"},title),h("div",{class:"help"},held?"Holding the settings from before the factory reset"+(data.held_since?" of "+day(data.held_since):"")+". Restore to undo the reset, or refresh the copies to let them follow the device again.":sub));
    let right;
    if(!data||!data.present){
      /* a card in the slot WiCAN cannot read (2026-10-25): the chip and the page's format dialog */
      const bad=data&&data.card===false&&data.unreadable?data.unreadable:null;
      right=bad?h("div",{class:"rowflex",style:"gap:10px;justify-content:flex-end;align-items:center"},chip(bad.state==="failed"?"not answering":"not readable",bad.state==="failed"?"crit":"warn"),bad.state==="failed"?null:sdFormatLink())
        :h("div",{class:"help",style:"text-align:right;max-width:200px"},data&&data.card===false?"No SD card in the slot":"No copy yet");
    }else{
      const heldChip=held?chip("held","warn"):null;
      if(heldChip)heldChip.classList.add("bk-held-chip");
      right=h("div",{class:"rowflex",style:"gap:10px;flex-wrap:wrap;justify-content:flex-end"},heldChip,
        h("div",{class:"help mono",style:"text-align:right;white-space:nowrap"},when(data.refreshed),h("br"),data.parts+" parts · "+kb(data.bytes)),
        h("button",{class:"btn sm bk-restore-copy","data-where":where,onclick:async e=>{
          e.target.closest("button").disabled=true;
          try{const m=await api("/api/backup/copies/"+where);restorePreview(m,repaintAll);}
          catch(err){toast(err.message,"err");}
          e.target.closest("button").disabled=false;}},ic("upload"),"Restore"));
    }
    return h("div",{class:"bk-place"+(held?" bk-held":""),"data-where":where,style:"display:grid;grid-template-columns:22px 1fr auto;gap:2px 12px;align-items:center;padding:9px 4px;border-top:1px solid var(--border)"},
      h("span",{class:"cico",style:"color:var(--text-3)"},ic(where==="sd"?"harddrive":"cpu")),words,right);
  }
  function paint(){
    const c=cur();
    const sd=Object.assign({},c.sd||{});
    const other=sd.device_id&&sd.device_id!==st.device_id;
    /* the slot holds a card WiCAN cannot read (2026-10-25): the status poll's `sd` summary */
    const sdx=!sd.card&&typeof sdSummary==="function"?sdSummary():null;
    sd.unreadable=sdx&&(sdx.state==="unreadable"||sdx.state==="failed")?sdx:null;
    rows.replaceChildren(
      placeRow("Internal flash","Survives a factory reset: restore from it to undo one.",c.internal,"internal"),
      placeRow("SD card",sd.card?(other?"This card came from another WiCAN ("+sd.device_id+"): restore from it to take its configuration over.":"Move the card to a replacement WiCAN and restore from it there."+(sd.free_bytes?" "+kb(sd.free_bytes)+" free.":""))
        :sd.unreadable?(sd.unreadable.state==="failed"?"The card in the slot does not answer. Take it out and put it back in.":"The card in the slot is not readable yet. Format it to keep a copy here that can move to another WiCAN.")
        :"Insert a card to keep a copy you can move to another WiCAN.",sd,"sd"));
    key.paint();
    keyNudge.style.display=key.downloaded()?"none":"";
  }
  paint();
  const el=card("Copies on the device","harddrive",
    h("div",{style:"font-size:13px;color:var(--text-2);margin-bottom:10px"},"A full copy, certificates included, that never leaves WiCAN: on the internal flash, and on the SD card when one is in. Sealed with the backup key."),
    heading("Where",true),rows,
    h("div",{style:"margin-top:10px;display:flex;justify-content:flex-end"},refreshBtn),
    heading("How"),...autoRows,keyRow,keyNudge);
  el.id="bk-copies-card";el.repaint=paint;
  return el;
}

/* ---- the restore preview, for a file from the computer or a copy on the device ---- */
const WARN={
  key_missing:()=>banner("warn","lock",h("b",{},"Encrypted with another WiCAN's key.")," Choose that device's key file to open it. The key never changes on a device, so one key file opens every backup it made."),
  bad_key:()=>banner("crit","alert",h("b",{},"That key does not open this backup.")," Check the key file: it must be the one of the WiCAN that made it."),
  other_device:()=>banner("info","info",h("b",{},"From another WiCAN.")," The ticked parts replace this device's. The backup key of this device is not changed."),
  redacted:()=>banner("info","info",h("b",{},"Passwords, keys and tokens were removed from this backup.")," The ones stored on this WiCAN are kept."),
  newer:()=>banner("crit","alert",h("b",{},"Made with newer firmware.")," Update this WiCAN first: a newer backup can hold settings this firmware does not know."),
};
function restorePreview(m,repaintAll){
  const me=m.device_id&&m.other_device===false;
  const picked=new Set(Object.keys(m.parts||{}));
  const list=h("div",{class:"bk-list"});
  const go=h("button",{class:"btn pri bk-go"},ic("upload"),"Restore and restart");
  const fact=(k,v)=>[h("span",{class:"help",style:"font-weight:600;padding-top:1px"},k),h("span",{style:"font-size:13px"},v)];
  const facts=h("div",{class:"bk-facts",style:"display:grid;grid-template-columns:max-content 1fr;gap:5px 16px;align-items:start;padding:10px 12px;background:var(--surface-2);border:1px solid var(--border);border-radius:9px"},
    ...fact(m.source==="file"?"File":"Copy",h("span",{class:m.source==="file"?"mono":"",style:"word-break:break-all;font-weight:600"},m.name||"backup")),
    ...fact("Taken",when(m.created)),
    ...fact("From",m.other_device?"another WiCAN ("+m.device_id+")":"this WiCAN"),
    ...fact("Firmware",(m.fw_version||"unknown")+((m.warnings||[]).includes("newer")?", newer than this device's":m.version===1?", today's settings-only format":"")));
  const warns=h("div",{style:"display:flex;flex-direction:column;gap:8px"},...(m.warnings||[]).map(w=>WARN[w]?WARN[w]():null));
  const keyPick=h("label",{class:"btn sm bk-unlock",style:"align-self:flex-start"},ic("key"),"Choose key file…",
    h("input",{type:"file",accept:".txt",style:"display:none",onchange:async e=>{
      const f=e.target.files[0];if(!f)return;
      try{const text=await f.text();const next=await api("/api/backup/unlock",{method:"POST",body:{key:text}});close();restorePreview(next,repaintAll);}
      catch(err){toast(err.message,"err");}
      e.target.value="";}}));
  let keepIdentity=true,keepVpn=false;
  const other=m.other_device&&!m.locked;
  const identity=other?h("label",{class:"rowflex bk-keep-names",style:"gap:8px;align-items:flex-start;font-size:13px;color:var(--text-2);padding:2px 4px"},
    h("input",{type:"checkbox",checked:true,style:"margin-top:3px",onchange:e=>{keepIdentity=e.target.checked;}}),
    h("span",{},h("b",{style:"color:var(--text)"},"Keep this WiCAN's own names"),h("br"),"The access point name and the mDNS name stay as they are; everything else in Settings comes from the backup.")):null;
  const vpnKeys=other&&!m.redacted?h("label",{class:"rowflex bk-keep-vpn",style:"gap:8px;align-items:flex-start;font-size:13px;color:var(--text-2);padding:2px 4px"},
    h("input",{type:"checkbox",checked:false,style:"margin-top:3px",onchange:e=>{keepVpn=e.target.checked;}}),
    h("span",{},h("b",{style:"color:var(--text)"},"Keep this WiCAN's VPN settings and keys"),h("br"),"Unticked, this WiCAN takes over the other one's tunnel identity (private key, peer, endpoint): the right thing for a replacement. Tick it when both devices stay in use: two peers cannot share one key.")):null;
  const errs=h("div",{style:"display:flex;flex-direction:column;gap:6px"});
  function paint(){
    if(m.locked){
      list.replaceChildren(h("div",{style:"padding:14px 4px;color:var(--text-3);font-size:13px"},"What the backup holds appears once it is unlocked."));
      go.disabled=true;go.title="Unlock it first";return;
    }
    list.replaceChildren(...PARTS.map(p=>partRow(p,(m.parts||{})[p.id],picked,(id,on)=>{if(on)picked.add(id);else picked.delete(id);paint();},
      {absentWhy:p.localOnly&&m.source==="file"?"Never in a file: restore them from a copy on the device":"Not in this backup",note:leftOut((m.left_out||{})[p.id],"This backup left out")})));
    errs.replaceChildren(...Object.entries(m.errors||{}).map(([k,v])=>banner("crit","alert",h("b",{},(PARTS.find(p=>p.id===k)||{title:k}).title+": "),v)));
    go.disabled=!!m.refused||picked.size===0;go.title=m.refused?"This backup cannot be restored":picked.size===0?"Tick at least one part":"";
  }
  paint();
  const body=h("div",{style:"display:flex;flex-direction:column;gap:12px"},
    facts,warns,m.locked?keyPick:null,identity,vpnKeys,
    h("div",{},h("div",{style:"font-size:12px;font-weight:600;color:var(--text-3);text-transform:uppercase;letter-spacing:.04em;margin-bottom:2px"},"What to restore"),list),errs,
    m.locked||m.refused?null:h("div",{class:"help"},"Each ticked part replaces what this WiCAN has today, as a whole. The device checks the entire backup first; if any part is rejected, nothing changes. Then it restarts."));
  const title=m.source==="internal"?"Restore the copy on the internal flash":m.source==="sd"?"Restore the copy on the SD card":"Restore from "+(m.other_device?"another WiCAN's backup":"a backup of this WiCAN");
  const close=modal({title,body,actions:[{label:"Cancel",fn:()=>{api("/api/backup/staged",{method:"DELETE"}).catch(()=>{});}}]});
  const box=document.querySelector("#modal-root .modal");
  if(box)box.style.maxWidth="640px";
  const acts=document.querySelector("#modal-root .modal .acts");
  if(acts)acts.append(go);
  go.onclick=async()=>{
    go.disabled=true;
    try{const r=await api("/api/backup/restore",{method:"POST",body:{include:[...picked],keep_identity:keepIdentity,keep_vpn_keys:keepVpn}});
      close();toast("Restored "+(r.restored||[]).length+" parts","ok");if(r.reboot!==false)expectReboot("Applying the backup");}
    catch(e){toast("Restore failed: "+e.message,"err");go.disabled=false;}
  };
  void me;
  return close;
}

/* ---- card 3: a file from the computer ---- */
function restoreCard(repaintAll){
  const pick=h("label",{class:"btn bk-pick"},ic("upload"),"Choose backup file…",
    h("input",{type:"file",accept:".json",style:"display:none",onchange:async e=>{
      const f=e.target.files[0];if(!f)return;
      try{const m=await api("/api/backup/inspect?name="+encodeURIComponent(f.name.slice(0,60)),{method:"POST",body:await f.arrayBuffer()});restorePreview(m,repaintAll);}
      catch(err){toast("Could not read the file: "+err.message,"err");}
      e.target.value="";}}));
  const el=card("Restore from a file","upload",
    h("div",{style:"font-size:13px;color:var(--text-2);margin-bottom:12px"},"Choose a backup file from your computer. WiCAN reads it, shows what is inside and checks every part before anything changes. Nothing is restored until you confirm. A copy kept on the device restores from its own Restore button, above."),
    h("div",{style:"display:grid;grid-template-columns:minmax(0,1fr) auto;gap:14px;align-items:center"},
      h("span",{class:"help"},"Plain or encrypted files, from this or another WiCAN. Files from older firmware are brought up to date on the way in; files from newer firmware are refused."),pick));
  el.id="bk-restore-card";
  return el;
}

/* the three cards, in order; the holder repaints itself from a fresh status */
async function cards(){
  let st=await api("/api/backup/status");
  const autoRows=await settingsForm("config_backup",["auto_copy"],{visible:false});
  const holder=[];
  const repaintAll=async(fresh)=>{
    if(fresh){try{const n=await api("/api/backup/status");Object.assign(st,n);}catch{}}
    else{try{const n=await api("/api/backup/status");st.key=n.key;st.copies=n.copies;}catch{}}
    holder.forEach(c=>c.repaint&&c.repaint());};
  holder.push(backupCard(st,repaintAll),copiesCard(st,repaintAll,autoRows),restoreCard(repaintAll));
  return holder;
}
PAGES.__backup={cards,restorePreview,PARTS};
})();
