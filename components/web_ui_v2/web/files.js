/* web_ui_v2: the File Manager page as an on-demand chunk (2026-09-07, meatpi:
   "improve the UI and UX of the Files tab, rename it File Manager").
   Loaded by index.html's PAGES.files stub from /ui/files.js the first time the
   page opens (embedded, gzipped; web_ui_v2/CMakeLists.txt minifies it with the
   same rjsmin as the page). A classic script sharing the page's global scope
   (h, page, api, dataTable, modal, meter, …); registers PAGES.__files. The
   preview (tools/webui_preview/make_preview.py) inlines it so the jsdom probes
   run it.

   What it does: a storage landing (internal flash / SD card cards with usage
   meters, "not mounted" when the card is absent); inside a mount a breadcrumb
   that also drives the URL (#/files/sd/logs deep links keep working), Up, a
   usage line, filter + sort, multi-select with Delete selected, New folder,
   multi-file Upload with a progress bar and drag-and-drop onto the list,
   per-row Preview (text files ≤ 64 KB), Download, Copy path and Delete, a
   one-line description on the folders users meet, and the logger's active
   file marked "in use" (from /api/logger) with a hint to pause logging. */
(()=>{
const CSS=`/* File Manager (2026-09-07) */
.fm-head{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-bottom:10px}
.fm-crumb{display:flex;flex-wrap:wrap;gap:5px;align-items:center;font-size:13.5px;min-height:30px}
.fm-crumb a{color:var(--text-2);text-decoration:none;font-weight:600}.fm-crumb a:hover{color:var(--primary)}
.fm-crumb .cur{font-weight:700;color:var(--text)}.fm-crumb .sep{color:var(--text-3)}
.fm-tools{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-left:auto}
.fm-tools input[type=search]{width:160px}.fm-tools select{width:auto}
.fm-use{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--text-3);margin:0 0 10px}.fm-use .meter{width:140px}.fm-use svg{width:14px;height:14px}
.fm-mounts{display:grid;grid-template-columns:repeat(auto-fill,minmax(300px,1fr));gap:12px;margin-bottom:14px}
.fm-mount{display:grid;grid-template-columns:44px 1fr;gap:12px;align-items:start;cursor:pointer;border:1px solid var(--border);border-radius:12px;background:var(--surface);padding:14px 16px;transition:border-color .15s,box-shadow .15s}
.fm-mount:hover,.fm-mount:focus{border-color:var(--primary);box-shadow:var(--shadow);outline:none}.fm-mount.off{cursor:default;opacity:.75}.fm-mount.off:hover{border-color:var(--border);box-shadow:none}
.fm-mount .mi{width:44px;height:44px;border-radius:10px;background:var(--primary-tint);display:flex;align-items:center;justify-content:center}.fm-mount .mi svg{width:22px;height:22px;color:var(--primary)}
.fm-mount .mt{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:4px}.fm-mount .mn{font-weight:700;font-size:14px}.fm-mount .mp{font-family:var(--mono);font-size:11.5px;color:var(--text-3)}
.fm-mount .mo{margin-left:auto;font-size:12px;color:var(--primary);font-weight:600;white-space:nowrap}
.fm-mount .md{font-size:12.5px;color:var(--text-2);line-height:1.45}.fm-mount .mu{font-size:12px;color:var(--text-3);margin-top:8px}.fm-mount .meter{margin-top:6px}
.fm-body{position:relative;min-height:120px}
.fm-body.drag::after{content:"Drop to upload into this folder";position:absolute;inset:0;display:flex;align-items:center;justify-content:center;border:2px dashed var(--primary);border-radius:12px;background:color-mix(in srgb,var(--primary) 8%,transparent);color:var(--primary);font-weight:700;font-size:15px;pointer-events:none}
.fm-name{display:flex;align-items:center;gap:8px;min-width:0}.fm-name>svg{flex:0 0 auto;color:var(--text-3);width:16px;height:16px}
.fm-name a{color:var(--text);text-decoration:none}.fm-name a:hover{color:var(--primary)}.fm-name .fn{font-family:var(--mono);font-size:12.5px;font-weight:600;word-break:break-all}
.fm-desc{font-size:11.5px;color:var(--text-3);margin:2px 0 0 24px}
.fm-acts{display:flex;gap:4px;justify-content:flex-end;flex-wrap:nowrap;white-space:nowrap}
.tbl tr.fm-row.sel td{background:var(--primary-tint)}
.tbl td.fm-chk,.tbl th.fm-chk{width:26px;padding-right:0}
.fm-body td.num{white-space:nowrap}
.fm-prog{margin:0 0 10px;font-size:12.5px;color:var(--text-2)}.fm-prog .meter{margin-top:6px}
.fm-pre{max-height:60vh;overflow:auto;background:var(--term-bg);color:var(--term-text);border:1px solid var(--border);border-radius:9px;padding:10px 14px;font-family:var(--mono);font-size:12px;line-height:1.5;white-space:pre-wrap;word-break:break-all;margin:0}
.fm-hint{font-size:12.5px;color:var(--text-3);margin-top:10px;line-height:1.5}
.modal.fm-wide{max-width:min(940px,94vw);width:100%}`;
document.head.append(h("style",{},CSS));

const MOUNTS=[
  {path:"/data",name:"Internal flash",icon:"cpu",desc:"Certificates, scripts, the dashboard layout and cached web libraries. Small, so keep big files on the SD card."},
  {path:"/sd",name:"SD card",icon:"save",desc:"Logger output, firmware files and anything large.",bit:"sdcard_mounted"}];
/* the folders users meet, in one line each */
const KNOWN={"/data/autopid":"AutoPID configuration and vehicle profiles","/data/cache":"downloaded web libraries: safe to delete, fetched again on demand",
  "/data/certs":"certificates: manage them under System → Certificates","/data/scripts":"Berry scripts: edit them on the Scripts page",
  "/data/web":"custom web assets served by the WiCAN","/data/dashboard.json":"dashboard layout (Dashboard → Customise)",
  "/sd/logs":"logger output: the Logger page decodes these files","/sd/fw":"firmware files for updates","/sd/cache":"downloaded web libraries (SD fallback)","/sd/devlog":"development logs"};
const TEXT_EXT=/\.(txt|md|json|jsonl|csv|log|be|asc|pem|crt|key|js|css|html?|ya?ml|ini|cfg|conf|candump|trc|xml|svg)$/i;
const PREVIEW_MAX=65536;
/* only names ic() really has (index.html IC table): data files, logs, code, text, keys */
const ICON_BY_EXT={db:"harddrive",sqlite:"harddrive",wdl:"harddrive",blf:"harddrive",mf4:"harddrive",bin:"harddrive",gz:"harddrive",
  csv:"logs",jsonl:"logs",log:"logs",asc:"logs",candump:"logs",trc:"logs",
  json:"code",js:"code",css:"code",html:"code",htm:"code",be:"code",xml:"code",svg:"code",yml:"code",yaml:"code",ini:"code",cfg:"code",conf:"code",
  txt:"book",md:"book",pem:"key",key:"key",crt:"shield",corrupt:"alert"};
/* by extension, for files the firmware produces in a known state */
const EXT_HINT={corrupt:"set aside by the logger: the file was corrupt; what could be read was salvaged already. Download it and run tools/db_recover.py for more, then delete it."};

PAGES.__files=async(view)=>{
  const p=page(view,null,"File Manager","Browse, upload, download and tidy the internal flash and the SD card.");
  /* the fs API validates paths literally: never percent-encode the slashes */
  const enc=s=>encodeURIComponent(s).replace(/%2F/gi,"/");
  const join=(a,b)=>(a==="/"?"":a.replace(/\/$/,""))+"/"+b;
  const parent=x=>x.split("/").slice(0,-1).join("/")||"/";
  const mountOf=x=>MOUNTS.find(m=>x===m.path||x.startsWith(m.path+"/"));
  const ext=n=>{const m=/\.([a-z0-9]+)$/i.exec(n);return m?m[1].toLowerCase():"";};
  /* deep link: #/files/sd/logs opens that folder (the Logger page's "Open folder") */
  let path=(location.hash.match(/^#\/files(\/[^?]+)$/)||[])[1]||"/";
  const S={filter:"",sort:"name",sel:new Set(),bits:null,logger:null,ents:[]};

  /* ---- header: breadcrumb + tools ---- */
  const crumbEl=h("div",{class:"fm-crumb"});
  const filterEl=h("input",{type:"search",placeholder:"Filter names…",title:"Show only names containing this text",oninput:()=>{S.filter=filterEl.value.trim().toLowerCase();paint();}});
  const sortEl=h("select",{title:"Sort order",onchange:()=>{S.sort=sortEl.value;paint();}},
    h("option",{value:"name"},"Sort: name"),h("option",{value:"size"},"Sort: size"),h("option",{value:"type"},"Sort: type"));
  const upBtn=h("button",{class:"btn sm gh",type:"button",title:"Up one level",onclick:()=>go(parent(path))},ic("up"),"Up");
  const mkBtn=h("button",{class:"btn sm",type:"button",title:"Create a folder here",onclick:()=>mkdir()},ic("plus"),"New folder");
  const upInput=h("input",{type:"file",style:"display:none",onchange:e=>{uploadFiles([...e.target.files]);e.target.value="";}});
  upInput.multiple=true;
  const upLbl=h("label",{class:"btn sm pri",title:"Upload files into this folder, or drop them onto the list"},ic("upload"),"Upload",upInput);
  const delSel=h("button",{class:"btn sm danger",type:"button",title:"Delete every selected item",onclick:()=>deleteSelected()},ic("trash"),"Delete selected");
  const refBtn=h("button",{class:"btn sm gh",type:"button",title:"Refresh",onclick:()=>load()},ic("refresh"));
  const head=h("div",{class:"fm-head rowflex"},crumbEl,h("div",{class:"fm-tools"},filterEl,sortEl,upBtn,mkBtn,upLbl,delSel,refBtn));
  const useEl=h("div",{class:"fm-use"});
  const progEl=h("div",{class:"fm-prog"});
  const body=h("div",{class:"fm-body"});
  p.append(head,useEl,progEl,body);
  /* style.display, not the hidden attribute: the page's .btn display rule would override it */
  const show=(el,on)=>{el.style.display=on?"":"none";};
  show(useEl,false);show(progEl,false);show(delSel,false);
  body.ondragover=e=>{e.preventDefault();body.classList.add("drag");};
  body.ondragleave=()=>body.classList.remove("drag");
  body.ondrop=e=>{e.preventDefault();body.classList.remove("drag");uploadFiles([...((e.dataTransfer&&e.dataTransfer.files)||[])]);};

  function go(to){path=to;S.sel.clear();S.filter="";filterEl.value="";load();}
  function syncHash(){try{history.replaceState(null,"",path==="/"?"#/files":"#/files"+path);}catch{}}
  function paintCrumb(){
    const parts=path.split("/").filter(Boolean);
    const els=[h("a",{href:"#",title:"All storage",onclick:e=>{e.preventDefault();go("/");}},"Storage")];
    let acc="";
    parts.forEach((x,i)=>{acc+="/"+x;const cur=acc,last=i===parts.length-1;
      els.push(h("span",{class:"sep"},"/"),last?h("span",{class:"cur"},x):h("a",{href:"#",title:"Open "+cur,onclick:e=>{e.preventDefault();go(cur);}},x));});
    crumbEl.replaceChildren(...els);
    /* the landing keeps only Refresh: nothing else applies until a folder is open */
    const root=path==="/";
    [upBtn,filterEl,sortEl,mkBtn,upLbl].forEach(el=>show(el,!root));
    upInput.disabled=root;
  }

  /* ---- loading ---- */
  async function load(){
    syncHash();paintCrumb();show(useEl,false);show(delSel,false);body.replaceChildren(loading());
    if(path==="/"){await paintRoot();return;}
    const m=mountOf(path);
    try{
      const[r,info,lg]=await Promise.all([api("/api/fs/list?path="+enc(path)),m?tryGet("/api/fs/info?path="+m.path):null,tryGet("/api/logger")]);
      S.ents=r.entries||[];S.logger=lg;
      if(m&&info&&info.total){show(useEl,true);
        useEl.replaceChildren(ic(m.icon),h("span",{},m.name+" · "+fmt.bytes(info.used)+" of "+fmt.bytes(info.total)+" used"),meter(info.used,info.total));}
      paint();
    }catch(e){
      const sdOff=m&&m.path==="/sd"&&S.bits&&S.bits.sdcard_mounted===false;
      body.replaceChildren(banner(sdOff?"warn":"crit","alert",
        sdOff?"No SD card is mounted. Insert a card, then refresh. ":("Cannot open "+path+": "+e.message+". "),
        h("a",{href:"#",onclick:ev=>{ev.preventDefault();go("/");}},"Back to storage")));
    }
  }
  async function paintRoot(){
    const infos=await Promise.all(MOUNTS.map(m=>tryGet("/api/fs/info?path="+m.path)));
    body.replaceChildren(h("div",{class:"fm-mounts"},...MOUNTS.map((m,i)=>{
      const info=infos[i];
      const mounted=!(m.bit&&S.bits&&S.bits[m.bit]===false)&&!!(info&&info.total);
      const pct=mounted?info.used/info.total*100:0;
      /* a card in the slot WiCAN cannot read (2026-10-25): the tile says what it
         holds and offers the format (the page's dialog); a card that does not
         answer is a fault, no button */
      const sdx=m.path==="/sd"&&!mounted&&typeof sdSummary==="function"?sdSummary():null;
      const sdBad=!!sdx&&(sdx.state==="unreadable"||sdx.state==="failed"),dead=sdBad&&sdx.state==="failed",fixable=sdBad&&!dead;
      const card=h("div",{class:"fm-mount"+(mounted||fixable?"":" off"),role:"button",title:mounted?"Open "+m.path:fixable?"The card is not readable: format it to use it":m.name+" is not available",
          onclick:()=>{if(mounted)go(m.path);else if(fixable)sdFormatDialog();else toast(m.name+" is not available","err");},
          onkeydown:e=>{if(e.key==="Enter"&&mounted)go(m.path);}},
        h("div",{class:"mi"},ic(m.icon)),
        h("div",{},
          h("div",{class:"mt"},h("span",{class:"mn"},m.name),h("span",{class:"mp"},m.path),
            mounted?null:sdBad?chip(dead?"not answering":"not readable",dead?"crit":"warn"):chip("not mounted","warn"),
            mounted?h("span",{class:"mo"},"Open ›"):fixable?h("button",{class:"btn sm pri",type:"button",style:"margin-left:auto",onclick:e=>{e.stopPropagation();sdFormatDialog();}},"Format…"):null),
          h("div",{class:"md"},m.desc),
          h("div",{class:"mu"},mounted?fmt.bytes(info.used)+" of "+fmt.bytes(info.total)+" used ("+pct.toFixed(0)+"%)"
            :dead?"The card does not answer ("+sdx.reason+"). Take it out and put it back in, or try another card."
            :fixable?sdCardLine({name:sdx.name,capacity_bytes:sdx.capacity_bytes})+". "+(SD_FOUND[sdx.reason]||SD_FOUND.unknown)+" Format it as FAT32 to use it here: everything on it is erased."
            :(m.path==="/sd"?"Insert an SD card, then refresh.":"Unavailable")),
          mounted?meter(info.used,info.total):null));
      card.tabIndex=0;return card;})),
      h("div",{class:"fm-hint"},"Pick a storage area. Folders you will meet: ",h("code",{},"/data/scripts")," (Scripts page), ",h("code",{},"/data/certs")," (System → Certificates), ",
        h("code",{},"/data/cache")," (web libraries, safe to delete), ",h("code",{},"/sd/logs")," (Logger). Drop files onto a folder listing to upload them."));
  }

  /* ---- the listing ---- */
  const iconFor=e=>e.dir?"folder":(ICON_BY_EXT[ext(e.name)]||"logs");
  function activeNames(){const lg=S.logger;if(!lg||!lg.running||lg.paused||lg.dir!==path)return new Set();
    return new Set([lg.file,lg.can&&lg.can.file].filter(Boolean));}
  function visible(){
    const l=S.ents.filter(e=>!S.filter||e.name.toLowerCase().includes(S.filter));
    const dirFirst=(a,b)=>(b.dir?1:0)-(a.dir?1:0);
    if(S.sort==="size")l.sort((a,b)=>dirFirst(a,b)||(b.size||0)-(a.size||0)||a.name.localeCompare(b.name));
    else if(S.sort==="type")l.sort((a,b)=>dirFirst(a,b)||ext(a.name).localeCompare(ext(b.name))||a.name.localeCompare(b.name));
    else l.sort((a,b)=>dirFirst(a,b)||a.name.localeCompare(b.name,undefined,{numeric:true}));
    return l;
  }
  function paint(){
    const list=visible(),act=activeNames();
    for(const n of[...S.sel])if(!S.ents.some(e=>e.name===n))S.sel.delete(n);
    show(delSel,S.sel.size>0);delSel.replaceChildren(ic("trash"),"Delete selected ("+S.sel.size+")");
    if(!S.ents.length){body.replaceChildren(h("div",{class:"empty"},"Empty folder",h("div",{class:"fm-hint"},"Drop files here, or use Upload and New folder.")));return;}
    if(!list.length){body.replaceChildren(h("div",{class:"empty"},"No names contain \""+S.filter+"\""));return;}
    const allChk=h("input",{type:"checkbox",title:"Select all",onchange:()=>{if(allChk.checked)list.forEach(e=>S.sel.add(e.name));else S.sel.clear();paint();}});
    allChk.checked=list.length>0&&list.every(e=>S.sel.has(e.name));
    const t=dataTable([
      {label:allChk,get:e=>{const cb=h("input",{type:"checkbox",title:"Select "+e.name,onchange:ev=>{if(ev.target.checked)S.sel.add(e.name);else S.sel.delete(e.name);paint();}});cb.checked=S.sel.has(e.name);return cb;}},
      {label:"Name",get:e=>{const full=join(path,e.name),known=KNOWN[full]||(e.dir?null:EXT_HINT[ext(e.name)]);
        return h("div",{},h("div",{class:"fm-name"},ic(iconFor(e)),
          e.dir?h("a",{href:"#",class:"fn",title:"Open "+full,onclick:ev=>{ev.preventDefault();go(full);}},e.name):h("span",{class:"fn"},e.name),
          act.has(e.name)?chip("in use","warn"):null),
          known?h("div",{class:"fm-desc"},known):null);}},
      {label:"Size",align:"right",get:e=>e.dir?"-":fmt.bytes(e.size)},
      {label:"",get:e=>h("div",{class:"fm-acts"},...actions(e,act.has(e.name)))}
    ],list,{responsive:false});
    body.replaceChildren(t,path===((S.logger||{}).dir||"")?note("",["These are the logger's files. The ",h("a",{href:"#/logger"},"Logger page")," charts and exports them; the file being written is locked until logging is paused."]):null);
    [...body.querySelectorAll("tbody tr")].forEach((tr,i)=>{tr.classList.add("fm-row");tr.dataset.name=list[i].name;tr.classList.toggle("sel",S.sel.has(list[i].name));
      const td=tr.firstElementChild;if(td)td.classList.add("fm-chk");});
    const th=body.querySelector("thead th");if(th)th.classList.add("fm-chk");
  }
  function actions(e,inUse){
    const full=join(path,e.name),url=API+"/api/fs/download?path="+enc(full);
    if(e.dir)return[h("button",{class:"btn sm gh",type:"button",title:"Open "+full,onclick:()=>go(full)},ic("folder"),"Open"),
      h("button",{class:"btn sm gh danger",type:"button",title:"Delete this folder (it must be empty)",onclick:()=>del(e)},ic("trash"))];
    const a=[];
    if(!inUse&&TEXT_EXT.test(e.name)&&e.size<=PREVIEW_MAX)a.push(h("button",{class:"btn sm gh",type:"button",title:"Show the file here",onclick:()=>preview(e)},ic("monitor"),"Preview"));
    if(inUse)a.push(h("button",{class:"btn sm gh",type:"button",title:"The logger is writing this file: pause logging on the Logger page to download it",
      onclick:()=>toast("The logger is writing this file. Pause logging on the Logger page first.","err")},ic("lock"),"In use"));
    else a.push(h("a",{class:"btn sm gh",title:"Download "+e.name,href:url,download:e.name},ic("down"),"Download"));
    a.push(h("button",{class:"btn sm gh",type:"button",title:"Copy "+full+" to the clipboard (for scripts and rules)",onclick:()=>copyPath(full)},"Copy path"));
    const d=h("button",{class:"btn sm gh danger",type:"button",title:inUse?"In use, pause logging first":"Delete "+e.name,onclick:()=>del(e)},ic("trash"));
    d.disabled=inUse;a.push(d);
    return a;
  }

  /* ---- actions ---- */
  function explain(err,isDir){const m=(err&&err.message)||String(err);
    if(/in use/i.test(m))return"The logger is writing this file. Pause logging on the Logger page first.";
    if(isDir&&/invalid path|not empty|ENOTEMPTY/i.test(m))return"The folder was not deleted: only empty folders can be deleted.";
    if(/503|transfer/i.test(m))return"Another transfer is running. Try again in a moment.";
    return m;}
  /* the page's copyText works over plain http (navigator.clipboard does not) */
  function copyPath(full){if(!copyText(full,"Copied "+full))promptModal("Path (copy it from here)",full);}
  async function del(e){
    const full=join(path,e.name);
    if(!await confirmModal(e.dir?"Delete the folder "+e.name+"? Only an empty folder can be deleted.":"Delete "+e.name+"? This cannot be undone.","Delete"))return;
    try{await api("/api/fs/file?path="+enc(full),{method:"DELETE"});toast("Deleted "+e.name,"ok");S.sel.delete(e.name);load();}
    catch(err){toast(explain(err,e.dir),"err");}
  }
  async function deleteSelected(){
    const names=[...S.sel];if(!names.length)return;
    if(!await confirmModal("Delete "+names.length+" selected item(s)? Folders must be empty. This cannot be undone.","Delete selected"))return;
    let ok=0;const bad=[];
    for(const n of names){const e=S.ents.find(x=>x.name===n)||{};
      try{await api("/api/fs/file?path="+enc(join(path,n)),{method:"DELETE"});ok++;}catch(err){bad.push(n+": "+explain(err,e.dir));}}
    S.sel.clear();toast(ok+" deleted"+(bad.length?", "+bad.length+" failed":""),bad.length?"err":"ok");
    if(bad.length)modal({title:"Not deleted",body:h("div",{},...bad.map(b=>h("div",{class:"help",style:"margin:4px 0"},b))),actions:[{label:"Close"}]});
    load();
  }
  async function mkdir(){
    if(path==="/")return toast("Open a storage area first","err");
    const n=((await promptModal("New folder name"))||"").trim();
    if(!n)return;
    if(/[\\/]/.test(n))return toast("A folder name cannot contain slashes","err");
    /* the path rides the query string like every other /api/fs route (the old page
       posted a JSON body, which the firmware answered with "invalid path") */
    try{await api("/api/fs/mkdir?path="+enc(join(path,n)),{method:"POST"});toast("Created "+n,"ok");load();}
    catch(err){toast(explain(err),"err");}
  }
  /* XHR so big uploads (firmware, SD fills) show progress; the plain fetch path
     is the fallback when XHR is unavailable or the request never left */
  function xhrUpload(url,f,onProg){return new Promise((res,rej)=>{
    let x;try{x=new XMLHttpRequest();}catch{return rej(new Error("no xhr"));}
    x.open("POST",url);
    if(x.upload)x.upload.onprogress=ev=>{if(ev.lengthComputable)onProg(ev.loaded/ev.total);};
    x.onload=()=>{if(x.status>=200&&x.status<300)res();else{let m="HTTP "+x.status;try{m=JSON.parse(x.responseText).error||m;}catch{}rej(new Error(m));}};
    x.onerror=()=>rej(new Error("network error"));
    const fd=new FormData();fd.append("file",f,f.name);x.send(fd);});}
  async function uploadFiles(files){
    if(!files.length)return;
    if(path==="/")return toast("Open a storage area first","err");
    const have=new Set(S.ents.map(e=>e.name)),dup=files.filter(f=>have.has(f.name)).map(f=>f.name);
    if(dup.length&&!await confirmModal("Replace "+dup.join(", ")+"?","Files already exist"))return;
    const bar=h("div",{class:"meter"},h("i",{style:"width:0%"})),txt=h("div",{});
    progEl.replaceChildren(txt,bar);show(progEl,true);
    let ok=0;
    for(let i=0;i<files.length;i++){const f=files[i],full=join(path,f.name);
      txt.textContent="Uploading "+f.name+" ("+(i+1)+" of "+files.length+", "+fmt.bytes(f.size)+")…";bar.firstChild.style.width="0%";
      try{await xhrUpload(API+"/api/fs/upload?path="+enc(full),f,pr=>{bar.firstChild.style.width=(pr*100).toFixed(0)+"%";});ok++;}
      catch(err){
        if(/^(no xhr|network error)$/.test(err.message)){
          try{const fd=new FormData();fd.append("file",f,f.name);await api("/api/fs/upload?path="+enc(full),{method:"POST",body:fd});ok++;}
          catch(e2){toast("Upload of "+f.name+" failed: "+explain(e2),"err");}
        }else toast("Upload of "+f.name+" failed: "+explain(err),"err");
      }
    }
    show(progEl,false);if(ok)toast(ok+" file(s) uploaded","ok");load();
  }
  async function preview(e){
    const full=join(path,e.name);
    modal({title:e.name,body:h("div",{},h("div",{class:"help",style:"margin-bottom:8px"},full+" · "+fmt.bytes(e.size)),loading()),
      actions:[{label:"Download",fn:()=>{const a=h("a",{href:API+"/api/fs/download?path="+enc(full),download:e.name});document.body.append(a);a.click();a.remove();}},{label:"Close"}]});
    const m=document.querySelector("#modal-root .modal");if(m)m.classList.add("fm-wide");
    const slot=()=>m&&m.querySelector(".loading");
    try{let data=await api("/api/fs/download?path="+enc(full));
      if(typeof data!=="string")data=JSON.stringify(data,null,2);
      else if(/\.json$/i.test(e.name)){try{data=JSON.stringify(JSON.parse(data),null,2);}catch{}}
      const s=slot();if(s)s.replaceWith(h("div",{},h("div",{class:"help",style:"margin-bottom:6px"},data.split("\n").length+" line(s)"),h("pre",{class:"fm-pre"},data)));
    }catch(err){const s=slot();if(s)s.replaceWith(banner("crit","alert",explain(err)));}
  }

  const st=await tryGet("/api/status");S.bits=(st&&st.bits)||null;
  await load();
  return null;
};
})();
