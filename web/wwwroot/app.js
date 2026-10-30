/* Interaction design over a captured native readback. No credentials or live writes. */
(() => {
  'use strict';
  const capture = window.LAPLACE_PREVIEW_DATA;
  const runtime=window.LAPLACE_RUNTIME||{};
  const identity={enabled:runtime.identityMode==='same-origin',state:'loading',providers:[],user:null,error:null};
  const read = capture.search;
  const rootKey = `entity:${read.id}`;
  const glyphs = {
    logo:'<path d="M12 2 22 12 12 22 2 12Z"/><path d="m7 12 5-5 5 5-5 5Z"/><path d="M12 2v5m10 5h-5m-5 10v-5M2 12h5"/>',
    home:'<path d="m3 10 9-7 9 7v11H3Z"/><path d="M9 21v-8h6v8"/>',
    library:'<rect x="3" y="4" width="5" height="16" rx="1"/><rect x="9" y="4" width="5" height="16" rx="1"/><path d="m16 5 4-1 3 15-4 1Z"/>',
    research:'<path d="M5 3h14v18H5Z"/><path d="M9 7h6M9 11h6M9 15h4"/>',
    source:'<path d="m12 3 9 5-9 5-9-5Z"/><path d="m3 12 9 5 9-5M3 16l9 5 9-5"/>',
    workbench:'<path d="M4 4h6v6H4Zm10 0h6v6h-6ZM4 14h6v6H4Zm10 0h6v6h-6Z"/>',
    export:'<path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"/>',
    search:'<circle cx="10.5" cy="10.5" r="6.5"/><path d="m16 16 5 5"/>',
    arrow:'<path d="M4 12h16m-6-6 6 6-6 6"/>',
    back:'<path d="M20 12H4m6-6-6 6 6 6"/>',
    user:'<circle cx="12" cy="8" r="4"/><path d="M4 21v-2a8 8 0 0 1 16 0v2"/>',
    pin:'<path d="m9 3 9 9-4 1-3 5-5-5 5-3Z"/><path d="m7 17-4 4"/>',
    close:'<path d="m6 6 12 12M6 18 18 6"/>',
    copy:'<rect x="8" y="8" width="12" height="13" rx="2"/><path d="M16 8V3H3v13h5"/>',
    evidence:'<path d="m12 2 9 4v6c0 5-9 10-9 10S3 17 3 12V6Z"/><path d="m8 12 3 3 5-6"/>',
    link:'<path d="m9 15 6-6m-8 4-2 2a4 4 0 0 0 6 6l4-4m-2-10 2-2a4 4 0 0 1 6 6l-4 4"/>',
    info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6m0-11v1"/>',
    more:'<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>',
    google:'<path d="M21 12h-9m8 0a8 8 0 1 1-2-6m2 6v5"/>',
    microsoft:'<path d="M3 3h8v8H3Zm10 0h8v8h-8ZM3 13h8v8H3Zm10 0h8v8h-8Z"/>'
  };
  const icon = name => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${glyphs[name] || glyphs.library}</svg>`;
  const h = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const short = id => `${id.slice(0,8)}…${id.slice(-4)}`;
  const badge = (text,tone='') => `<span class="badge ${tone}">${h(text)}</span>`;
  const resources = new Map();
  const add = (r) => { resources.set(r.key, {...resources.get(r.key), ...r}); return r.key; };
  const scalarNames = new Map(read.constituents.filter(x=>Number.isInteger(x.codepoint)).map(x=>[x.id,String.fromCodePoint(x.codepoint)]));
  const descriptors = {
    entity:{singular:'Entity',plural:'Entities',description:'Canonical content, independent of its appearances or authority.',tabs:['overview','structure','evidence','connections']},
    claim:{singular:'Claim',plural:'Claims',description:'An ordered proposition. Its identity is separate from who asserts it.',tabs:['overview','structure','evidence','connections']},
    physicality:{singular:'Physicality',plural:'Physicalities',description:'A returned structural view of an entity at a particular tier.',tabs:['overview','structure','connections']},
    attestation:{singular:'Attestation',plural:'Attestations',description:'An assertion about a claim from a particular source trunk.',tabs:['overview','evidence','connections']},
    source:{singular:'Source',plural:'Sources',description:'A witness and its attributable contribution to the substrate.',tabs:['overview','evidence','connections']},
    container:{singular:'Containing path',plural:'Containing paths',description:'A canonical composition returned because it contains the selected content.',tabs:['overview','structure','connections']}
  };
  add({key:rootKey,id:read.id,kind:'entity',roles:['entity'],label:'dog',summary:'Exact content · 3 ordered constituents',tier:read.tier,parts:read.parts,coord:read.coord,hilbert:read.hilbert,root:true});
  for(const c of read.constituents) add({key:`entity:${c.id}`,id:c.id,kind:'entity',roles:['entity'],label:scalarNames.get(c.id)||`Constituent ${short(c.id)}`,summary:`Tier ${c.tier}${Number.isInteger(c.codepoint)?' · U+'+c.codepoint.toString(16).toUpperCase().padStart(4,'0'):''}`,tier:c.tier,codepoint:c.codepoint,coord:c.coord,parts:[],leaf:true});
  read.physicality.forEach((p,i)=>add({key:`physicality:${read.id}:${p.tier}:${i}`,id:read.id,kind:'physicality',roles:['physicality'],label:`dog · tier ${p.tier} structure`,summary:`${p.n} ordered constituents · structural read`,tier:p.tier,parts:p.constituents,selector:i,claim:p.claim}));
  read.claims.forEach((c,i)=>add({key:`entity:${c.entity}`,id:c.entity,kind:'claim',roles:['entity','claim'],label:`Claim ${String(i+1).padStart(2,'0')}`,summary:`${c.n} members · standing not available`,tier:c.tier,parts:c.constituents,standing:c,ordinal:i+1}));
  read.occurrences.forEach((c,i)=>add({key:`entity:${c.entity}`,id:c.entity,kind:'container',roles:['entity','container'],label:`Containing path ${String(i+1).padStart(2,'0')}`,summary:`Tier ${c.tier} · ${c.n} ordered constituents`,tier:c.tier,parts:c.constituents,ordinal:i+1}));
  read.attestations.forEach((a,i)=>{
    const trunk=`source:${a.trunk}`;
    add({key:trunk,id:a.trunk,kind:'source',roles:['source'],label:'Unregistered source trunk',summary:'Referenced by attestations · witness registration not returned',registered:a.witness,trust:a.trust});
    add({key:`attestation:${a.claim}:${a.trunk}`,id:null,kind:'attestation',roles:['attestation'],label:`Attestation ${String(i+1).padStart(2,'0')}`,summary:`${a.games} observation · ${resources.get(`entity:${a.claim}`)?.label || 'claim'}`,claimKey:`entity:${a.claim}`,sourceKey:trunk,record:a});
  });
  // A referenced entity can be navigated even when its detail was not in this snapshot.
  for(const r of [...resources.values()]) for(const id of r.parts||[]) if(!resources.has(`entity:${id}`)) add({key:`entity:${id}`,id,kind:'entity',roles:['entity'],label:`Unresolved member ${short(id)}`,summary:'Referenced content · detail not captured',parts:[],unresolved:true});
  let notebook;
  const storageKey='laplace-modular-design-research-v1';
  try{notebook=JSON.parse(localStorage.getItem(storageKey)||'null')}catch{}
  if(!notebook || !Array.isArray(notebook.pins))notebook={pins:[],note:''};
  const persist=()=>{try{localStorage.setItem(storageKey,JSON.stringify(notebook))}catch{notify('Browser storage is unavailable. Export your notes to keep them.')}};
  const rootHref = '#/library?q=dog&kind=entity&selected='+encodeURIComponent(rootKey);
  const toLibrary=(patch={})=>{
    const route=readRoute();const p=new URLSearchParams(route.page==='library'?route.params:[]);
    for(const [key,value] of Object.entries(patch)){if(value===null||value==='')p.delete(key);else p.set(key,String(value));}
    return '#/library'+(p.size?'?'+p:'');
  };
  const linkTo=(key,label,patch={})=>`<a href="${h(toLibrary({selected:key,tab:null,...patch}))}">${h(label||resources.get(key)?.label||key)}</a>`;
  const rowLabel=r=>r.unresolved?'Detail not captured':descriptors[r.kind].singular;
  function readRoute(){const raw=location.hash.slice(1)||'/home';const [path,query='']=raw.split('?');return{page:path.split('/')[1]||'home',params:new URLSearchParams(query)};}
  let lastPage='';
  let lastHash=location.hash;
  const scrollPositions=new Map();
  if('scrollRestoration' in history)history.scrollRestoration='manual';
  function render(){
    const route=readRoute();const page=['home','library','research','sources','workbench','exports','scope'].includes(route.page)?route.page:'home';
    const titles={home:'Home',library:'Library',research:'Research',sources:'Sources',workbench:'Workbench',exports:'Exports',scope:'Design coverage'};
    const views={home:homeView,library:libraryView,research:researchView,sources:sourcesView,workbench:workbenchView,exports:exportsView,scope:scopeView};
    const previousFocus=document.activeElement?.dataset.focus;
    document.getElementById('app').innerHTML=`<aside class="sidebar"><a href="#/home" class="brand" aria-label="Laplace home">${icon('logo')}<span>LAPLACE</span></a><nav class="nav" aria-label="Main navigation">${[['home','Home','home'],['library','Library','library'],['research','Research','research'],['sources','Sources','source'],['workbench','Workbench','workbench'],['exports','Exports','export']].map(([key,label,img],i)=>`${i===3?'<div class="nav-caption">Resources & work</div>':''}<a href="#/${key}" title="${label}" class="${page===key?'active ':''}${i>2?'secondary':''}" ${page===key?'aria-current="page"':''}>${icon(img)}<span>${label}</span></a>`).join('')}<button class="mobile-more subtle" data-action="menu" aria-label="More navigation">${icon('more')}</button></nav><div class="sidebar-bottom"><span class="badge green">Modular Laplace</span><p>Content. Evidence. Connections.</p><p><span class="scope-dot"></span>Captured desktop readback</p></div></aside><div class="shell"><header class="topbar"><span class="location">${h(titles[page])}</span><div class="top-actions"><a class="preview-link" href="#/scope">${identity.enabled?'IIS · captured modular data':'Design preview · captured data'}</a>${identityButton()}</div></header><main id="main" class="content" tabindex="-1">${views[page](route.params)}</main></div>`;
    const selected=resources.get(route.params.get('selected'));
    document.title=`${page==='library'&&selected?selected.label+' · ':''}${titles[page]} · Laplace`;
    if(lastPage&&lastPage!==page){document.getElementById('main').focus({preventScroll:true});window.scrollTo(0,0)}
    else if(previousFocus){document.querySelector(`[data-focus="${CSS.escape(previousFocus)}"]`)?.focus({preventScroll:true})}
    lastPage=page;
  }
  function searchForm(value='',hero=false){return `<form class="search" data-form="search" role="search"><label class="visually-hidden" for="query">Search content or an identifier</label>${icon('search')}<input id="query" name="q" value="${h(value)}" placeholder="Search content or paste an identifier" autocomplete="off"><button class="primary" type="submit">Search${hero?icon('arrow'):''}</button></form>`;}
  function homeView(){return `<section class="hero"><div class="eyebrow">A connected library of knowledge</div><h1>Follow the information.<br>See what holds it together.</h1><p>Find content, inspect its structure and evidence, and follow its connections without losing your place.</p>${searchForm('',true)}<div class="search-note">Captured modular lookup: <a href="${rootHref}">dog</a>. Browse its content, evidence and connections.</div></section><div class="section-line"><h2>Start with a question. Keep the context.</h2><a href="#/scope">View design coverage ${icon('arrow')}</a></div><section class="cards" aria-label="Ways to work"><article class="card"><div class="icon-box">${icon('library')}</div><h2>Explore the library</h2><p>Move from a collection to an object, its evidence, and the structures around it.</p><footer><a href="${rootHref}">Open the captured collection ${icon('arrow')}</a></footer></article><article class="card"><div class="icon-box">${icon('research')}</div><h2>Keep a research thread</h2><p>Collect references and notes while retaining the exact identity and context of each result.</p><footer><a href="#/research">Open your research ${icon('arrow')}</a></footer></article><article class="card"><div class="icon-box">${icon('source')}</div><h2>Understand the evidence</h2><p>Inspect what was asserted, which source was involved, and what remains unresolved.</p><footer><a href="${h(toLibrary({q:'dog',kind:'claim',selected:rootKey,tab:'evidence'}))}">Inspect claim records ${icon('arrow')}</a></footer></article></section><section class="home-foot"><div><div class="section-line"><h2>In this readback</h2>${badge('Captured · 30 Oct 2026')}</div><div class="return-row"><div><strong>${linkTo(rootKey,'dog')}</strong><p>Exact content · tier 1 · ordered d, o, g composition</p></div>${icon('arrow')}</div><div class="return-row"><div><strong><a href="${h(toLibrary({q:'dog',kind:'claim',selected:null,tab:null}))}">Claim records</a></strong><p>8 returned · standing unavailable for all 8</p></div>${icon('arrow')}</div><div class="return-row"><div><strong><a href="#/sources">Source lineage</a></strong><p>One referenced trunk · no registered witness returned</p></div>${icon('arrow')}</div></div><div><div class="section-line"><h2>More than lookup</h2></div><p class="muted small">The same resource navigation supports documents, code, media, conversations, calculations, models, and their derivation receipts. Domain tools operate on those resources.</p><div class="inset-note">The full design includes ingestion and recipe work, witnessed conversation, software construction, domain operations, synthesis, and reproducible exports. Their backend requirements are documented alongside this preview.</div></div></section>`;}
  function libraryView(params){
    const kind=params.get('kind')||'entity',q=params.get('q')||'',tier=params.get('tier')||'';
    const selected=resources.get(params.get('selected'));
    const qn=q.trim().toLowerCase();
    // q=dog is the captured lookup context, not a fictional search of uncaptured data.
    const isContext=!qn||qn==='dog'||qn===read.id;
    const rows=[...resources.values()].filter(r=>r.roles.includes(kind)&&(!tier||String(r.tier)===tier)&&(isContext||`${r.label} ${r.id||''}`.toLowerCase().includes(qn)));
    const selectedInCollection=selected&&rows.some(r=>r.key===selected.key);
    const pageSize=6,pages=Math.max(1,Math.ceil(rows.length/pageSize)),pageNumber=Math.max(1,Math.min(pages,Number.parseInt(params.get('page')||'1',10)||1));
    const pageRows=rows.slice((pageNumber-1)*pageSize,pageNumber*pageSize);
    const pager=pages>1?`<nav class="collection-pager" aria-label="Captured collection pages">${pageNumber>1?`<a href="${h(toLibrary({page:pageNumber-1}))}" aria-label="Previous collection page">${icon('back')}</a>`:'<span></span>'}<span>${pageNumber} / ${pages}</span>${pageNumber<pages?`<a href="${h(toLibrary({page:pageNumber+1}))}" aria-label="Next collection page">${icon('arrow')}</a>`:'<span></span>'}</nav>`:'';
    const master=rows.length?pageRows.map(r=>`<a class="resource-row ${selected?.key===r.key?'selected':''}" href="${h(toLibrary({selected:r.key,tab:null}))}" ${selected?.key===r.key?'aria-current="true"':''}><strong>${h(r.label)}</strong><p>${h(r.summary)}</p>${badge(rowLabel(r))}</a>`).join(''):`<div class="empty">${icon('search')}<h2>No captured matches</h2><p>Try dog or clear the filter. This preview searches its captured records only.</p><a href="${h(toLibrary({q:'dog',tier:null,selected:null,page:null}))}">Reset filters</a></div>`;
    return `<div class="library-page ${selected?'has-selection':''}"><div class="page-top"><div><div class="eyebrow">Discover & inspect</div><h1>Library</h1><p>Find content, inspect its evidence, and follow its connections.</p></div><a href="#/research" class="button">${icon('research')}<span>Research</span></a></div><div class="filters">${searchForm(q)}<label class="filter-label">View<select name="kind" data-filter="kind">${Object.entries(descriptors).map(([k,d])=>`<option value="${k}" ${kind===k?'selected':''}>${d.plural}</option>`).join('')}</select></label><label class="filter-label">Tier<select name="tier" data-filter="tier"><option value="">Any</option>${[0,1,2,3].map(t=>`<option value="${t}" ${tier===String(t)?'selected':''}>${t}</option>`).join('')}</select></label></div><div class="result-caption">${rows.length} matching captured records${pages>1?' · page '+pageNumber+' of '+pages:''}. This is a bounded readback, not the database catalogue.${selected&&!selectedInCollection?' The open detail is outside this collection filter.':''}</div><section class="library ${selected?'has-detail':''}" aria-label="Resource collection and detail"><div class="master"><div class="master-title">${h(descriptors[kind]?.plural||'Resources')} ${badge(rows.length)}</div>${master}${pager}</div><article class="detail">${selected?detailView(selected,params):`<div class="empty">${icon('library')}<h2>Select a resource</h2><p>Its detail opens here. Your query and filters stay in the address, so you can return to this collection.</p>${rows[0]?`<a class="button" href="${h(toLibrary({selected:rows[0].key}))}">Open ${h(rows[0].label)} ${icon('arrow')}</a>`:''}</div>`}</article></section></div>`;
  }
  function detailView(r,params){
    const descriptor=descriptors[r.kind];const tab=descriptor.tabs.includes(params.get('tab'))?params.get('tab'):'overview';
    const pinned=notebook.pins.includes(r.key);
    return `<a class="back-mobile" href="${h(toLibrary({selected:null,tab:null}))}">${icon('back')} Back to results</a><div class="detail-heading"><div>${badge(descriptor.singular)} ${r.tier!==undefined?badge('Tier '+r.tier):''}<h1 tabindex="-1">${h(r.label)}</h1></div><div class="detail-actions"><button data-action="pin" data-key="${h(r.key)}" aria-pressed="${pinned}" aria-label="${pinned?'Remove reference from':'Save reference to'} research">${icon('pin')}<span class="button-label">${pinned?'Saved':'Save'}</span></button></div></div><p class="detail-intro">${h(descriptor.description)}</p>${r.id?`<div class="identity-line"><code>${h(r.id)}</code><button class="subtle" data-action="copy" data-text="${h(r.id)}" aria-label="Copy full entity identifier">${icon('copy')}</button></div>`:''}<nav class="tabs" aria-label="Resource detail views">${descriptor.tabs.map(t=>`<a data-focus="tab-${t}" href="${h(toLibrary({tab:t==='overview'?null:t}))}" ${tab===t?'aria-current="page"':''}>${({overview:'Overview',structure:'Structure',evidence:'Evidence',connections:'Connections'})[t]}</a>`).join('')}</nav><div id="detail-panel">${tab==='overview'?overview(r):tab==='structure'?structure(r):tab==='evidence'?evidence(r):connections(r)}</div>`;
  }
  function facts(items){return `<dl class="facts">${items.map(([label,value,wide])=>`<div${wide?' class="wide"':''}><dt>${h(label)}</dt><dd>${value}</dd></div>`).join('')}</dl>`;}
  function partName(id){const p=resources.get('entity:'+id);return p?.label||`Member ${short(id)}`;}
  function memberList(parts){return `<div class="member-list">${parts.map((id,i)=>`<span class="member">${i+1} · ${linkTo('entity:'+id,scalarNames.get(id)||id.slice(0,8)+'…')}</span>`).join('')}</div>`;}
  function overview(r){
    if(r.unresolved)return `<section class="panel-section"><h2>Referenced, not yet resolved</h2><p>This member is present in the native composition. Its content and type were not captured.</p><div class="inset-note">The production resource endpoint must resolve this address directly. The connected MCP selected-ID read currently returns “input or id”; this preview does not invent the missing content.</div></section>${connections(r)}`;
    if(r.kind==='attestation'){const a=r.record;return `<section class="panel-section"><h2>An assertion with its source</h2>${facts([['Claim',linkTo(r.claimKey)],['Source trunk',linkTo(r.sourceKey)],['Observations',h(a.games)],['Series score',h(a.score)],['Witness registration',a.witness?'Registered':'Not returned'],['Trust field',`${h(a.trust)} ${badge('Source not registered','amber')}`],['Occurrence position',a.position===null?'Not returned':h(a.position)],['Tokens',h(a.tokens)]])}</section><div class="inset-note">The record is present. The source has not been returned as a registered witness, and this claim has no available standing. A score of 1 in this series is not a probability that the proposition is true.</div>`;}
    if(r.kind==='source'){const all=[...resources.values()].filter(a=>a.sourceKey===r.key);return `<section class="panel-section"><h2>Source registration is unresolved</h2><p>This trunk is referenced by ${all.length} captured attestations. The sources read returned no registered witnesses.</p>${facts([['Registration',badge('Not registered','amber')],['Attributed records',String(all.length)],['Source name','Not returned'],['License and version','Not returned']])}</section><div class="inset-note">Missing source registration is an explicit gap. It neither erases the attestation records nor establishes trusted provenance.</div><section class="panel-section"><h2>Attributed assertions</h2>${all.slice(0,4).map(a=>evidenceRow(a)).join('')}<a href="${h(toLibrary({selected:r.key,tab:'evidence'}))}">View all ${all.length} assertions ${icon('arrow')}</a></section>`;}
    if(r.kind==='claim'){return `<section class="panel-section"><h2>The proposition</h2><p>${r.parts.length} ordered members. Member labels and semantic roles must be resolved by the resource service; the interface preserves their order without assuming a three-part relation.</p>${memberList(r.parts)}</section><section class="panel-section"><h2>Standing</h2><div class="inset-note">Standing is unavailable for this claim in the captured response. The returned numeric zero fields are not presented as measured belief or refutation.</div></section><section class="panel-section"><h2>Supporting records</h2>${evidence(r)}</section>`;}
    if(r.kind==='physicality')return `<section class="panel-section"><h2>A structural view of dog</h2><p>This view refers to the same canonical entity. Tier and row role describe this structure; they do not create a different content identity.</p>${facts([['Canonical content',linkTo(rootKey)],['Tier',h(r.tier)],['Returned row role',r.claim?'Claim':'Composition'],['Constituent positions',String(r.parts.length)]])}</section>${structure(r)}<p class="preview-note">This preview addresses the returned physicality by entity, tier, and response ordinal. The production API needs an explicit stable structural selector.</p>`;
    if(r.leaf)return `<section class="panel-section"><h2>Canonical scalar</h2><div class="reading">${h(r.label)}</div>${facts([['Unicode scalar',Number.isInteger(r.codepoint)?'U+'+r.codepoint.toString(16).toUpperCase().padStart(4,'0'):'Not returned'],['Tier',h(r.tier)],['Included in',linkTo(rootKey,'dog')]])}</section><p class="preview-note">This scalar was returned as a constituent of dog. No wider claims about it were requested.</p>`;
    if(r.kind==='container')return `<section class="panel-section"><h2>Exact containment</h2><p>This composition was returned as a path containing dog. Containment establishes a structural connection; it does not establish equivalence or semantic agreement.</p>${facts([['Tier',h(r.tier)],['Constituent positions',String(r.parts.length)],['Selected content',linkTo(rootKey)],['Positions holding dog',r.parts.map((id,i)=>id===read.id?String(i+1):null).filter(Boolean).join(', ')]])}</section>${structure(r)}`;
    return `<section class="panel-section"><h2>Composition</h2><p>Three ordered constituents form this written content.</p><div class="part-pills">${r.parts.map((id,i)=>`<a class="part-pill" href="${h(toLibrary({selected:'entity:'+id,tab:null}))}">${h(scalarNames.get(id)||'?')}<small>position ${i+1}</small></a>`).join('')}</div></section><div class="summary-grid"><div><span class="fact-value">${read.claims_read}</span><p>claim records returned</p><a class="small" href="${h(toLibrary({tab:'evidence'}))}">Inspect evidence ${icon('arrow')}</a></div><div><span class="fact-value">${read.occurrences_read}</span><p>containing paths found</p><a class="small" href="${h(toLibrary({tab:'connections'}))}">Follow containment ${icon('arrow')}</a></div></div><section class="panel-section"><h2>Evidence status</h2><p>Standing is unavailable for all eight claim records. Their source trunk is not returned as a registered witness.</p><p class="small">No interpretation was resolved in this readback. Follow the evidence to inspect what was observed.</p></section><details><summary class="small muted">Identity and placement</summary><div class="panel-section">${facts([['BLAKE3 entity ID',`<code>${h(r.id)}</code>`,true],['Coordinate',`<span class="mono">${r.coord.map(c=>c.toFixed(8)).join(', ')}</span>`,true],['Hilbert locality',`<code>${h(r.hilbert)}</code>`],['Identity computation',h(read.ms.identity)+' ms']])}</div></details>`;
  }
  function structure(r){
    if(!r.parts?.length)return `<div class="empty">${icon('library')}<h2>${r.leaf?'A captured leaf':'Structure not captured'}</h2><p>${r.leaf?'No child composition is included for this scalar.':'This response did not include this resource’s structural detail.'}</p></div>`;
    const shown=r.parts.slice(0,18);
    return `<section class="panel-section"><h2>Ordered constituents</h2><p>Every position is retained, including repeated references to the same content. ${r.parts.length>shown.length?`Showing the first ${shown.length} of ${r.parts.length} positions.`:''}</p><div class="table-wrap"><table><thead><tr><th scope="col">Position</th><th scope="col">Content</th><th scope="col">Identity</th></tr></thead><tbody>${shown.map((id,i)=>`<tr><td>${i+1}</td><td>${linkTo('entity:'+id,partName(id))}</td><td><span class="mono">${h(short(id))}</span></td></tr>`).join('')}</tbody></table></div></section>`;
  }
  function evidenceRow(r){return `<div class="evidence-row"><div><h3>${linkTo(r.key)}</h3><p>${h(r.summary)}</p>${r.kind==='claim'?memberList(r.parts):`<p>${linkTo(r.claimKey)} · ${linkTo(r.sourceKey)}</p>`}</div>${badge(r.kind==='claim'?'Standing unavailable':'Record present',r.kind==='claim'?'amber':'')}</div>`;}
  function evidence(r){
    if(r.kind==='attestation')return `<section class="panel-section"><h2>Trace the assertion</h2>${facts([['Claim',linkTo(r.claimKey)],['Source',linkTo(r.sourceKey)],['Readback',h(capture.capturedAt)],['Scope','Captured modular database laplace']])}</section>`;
    const assertions=[...resources.values()].filter(a=>a.kind==='attestation'&&(r.root||a.claimKey===r.key||a.sourceKey===r.key));
    const claims=r.root?read.claims.map(c=>resources.get('entity:'+c.entity)):[];
    return `${r.root?`<section class="panel-section"><h2>Claims involving this content</h2><p>${claims.length} claim records were returned. Their source assertions and standing are inspected separately.</p>${claims.map(evidenceRow).join('')}</section>`:''}<section class="panel-section"><h2>Source assertions</h2>${assertions.length?`<p class="small muted">${assertions.length} captured records. A record’s presence does not establish independent corroboration.</p>${assertions.map(evidenceRow).join('')}`:`<p class="muted small">No assertions about this resource were included in the captured readback.</p>`}</section>`;
  }
  function connections(r){
    const incoming=[...resources.values()].filter(x=>x.key!==r.key&&r.id&&x.parts?.includes(r.id));
    const assertions=[...resources.values()].filter(x=>x.claimKey===r.key||x.sourceKey===r.key);
    const members=(r.parts||[]).map(id=>resources.get('entity:'+id));
    const group=(label,rows,note)=>`<section class="panel-section"><h2>${h(label)}</h2><p>${h(note)}</p>${rows.length?rows.slice(0,12).map(x=>`<div class="return-row"><div><strong>${linkTo(x.key)}</strong><p>${h(x.summary)}</p></div>${icon('arrow')}</div>`).join(''):'<p class="small muted">No records of this connection were captured.</p>'}</section>`;
    return `${r.root?`<div class="inset-note">Containment, semantic relations, and geometric neighborhoods are different connections. This readback establishes containment; it does not return a semantic-neighbor catalogue.</div>`:''}${group('Contains →',members,'Ordered content references from this composition.')}${group('← Contained by',incoming,r.root?`The service found ${read.occurrences_read} containing paths; ${read.occurrences.length} were captured under fan=12. Claims are listed independently below.`:'Structures in this snapshot that refer to the same content identity.')}${group('Asserted through',assertions,'The attributable assertion records that connect a claim and source trunk.')}<p class="preview-note">Additional relation kinds require typed endpoint links. The UI does not substitute geometric distance for a semantic connection.</p>`;
  }
  function researchView(){const pins=notebook.pins.map(k=>resources.get(k)).filter(Boolean);return `<div class="page-top"><div><div class="eyebrow">Your working context</div><h1>Research</h1><p>Keep exact references and your reasoning together. Return to any object in its original lookup context.</p></div><button data-action="download-notebook">${icon('export')} Export notes</button></div><div class="two-col"><section class="sheet"><div class="section-line"><h2>Saved references</h2>${badge(pins.length)}</div>${pins.length?pins.map(r=>`<div class="return-row"><div><strong>${linkTo(r.key)}</strong><p>${h(r.summary)}</p></div><button class="subtle" data-action="pin" data-key="${h(r.key)}" aria-label="Remove ${h(r.label)}">${icon('close')}</button></div>`).join(''):`<div class="empty">${icon('pin')}<h2>Build a research thread</h2><p>Save an entity, claim, source, or assertion from its detail. Each reference retains its exact address.</p><a class="button" href="${rootHref}">Explore dog ${icon('arrow')}</a></div>`}<div class="panel-section"><label for="research-note"><strong>Research notes</strong></label><textarea id="research-note" class="note-input" placeholder="What have you established? What still needs evidence?">${h(notebook.note)}</textarea><button data-action="save-note">Save notes locally</button><p class="preview-note">References and notes in this preview are stored in this browser only. Export them to retain a portable copy. No account or server workspace has been created.</p></div></section><aside class="sheet"><h2>A continuing investigation</h2><ol class="step-list"><li><span><strong>Preserve the question</strong>Query, filters, selected resource, and view belong to the research address.</span></li><li><span><strong>Keep uncertainty visible</strong>Distinguish a proposition, an assertion, a standing estimate, and an unresolved question.</span></li><li><span><strong>Trace the result</strong>Retain content IDs, source lineage, scope, and operation receipts with each contribution.</span></li></ol><div class="inset-note">The full product adds saved collections, comparison, receipted queries and witnessed conversation to this same resource workflow. They are not simulated here.</div></aside></div>`;}
  function sourcesView(){const list=[...resources.values()].filter(r=>r.kind==='source');return `<div class="page-top"><div><div class="eyebrow">Lineage & authority</div><h1>Sources</h1><p>See which sources contributed, what they assert, and which provenance is still unresolved.</p></div></div><div class="two-col"><section class="sheet"><h2>No registered witnesses returned</h2><p class="muted">The sources endpoint returned an empty registered-witness list. The claim read still returned eight assertions attributed to the trunk below.</p>${list.map(r=>`<div class="return-row"><div><strong>${linkTo(r.key)}</strong><p class="mono">${h(r.id)}</p></div>${badge('Not registered','amber')}</div>`).join('')}<p class="preview-note">“No registered witnesses returned” is scoped to this captured readback. It is not a global count of ingested content or a statement that the assertions do not exist.</p></section><aside class="sheet"><h2>Source detail</h2><p class="muted small">A registered source’s detail needs its version, license, witness identity, dependence roots, corpus and file hierarchy, admitted claims, acquisition history, and receipts.</p><a href="#/workbench">Manage source admission ${icon('arrow')}</a></aside></div>`;}
  const workItems=[
    ['source','Source admission','Declare provenance, acquisition, license, grammar and dependency requirements. Inspect admitted content and its receipts.'],
    ['library','Grammar & recipe studio','Inspect chained packaging and inner grammars, typed symbols, proposed recipes and unresolved obligations. Preview plan, claims and round trip.'],
    ['workbench','Ingestion & learning','Follow streaming admission, deduplication, attestation and standing updates through receipted jobs. Retries preserve witnessing identity.'],
    ['research','Conversation & computation','Carry context, alternatives, calculations, obligations and outcomes through a witnessed research trajectory.'],
    ['link','Code & domain tools','Find declarations and callers, inspect structure, construct software and run domain operations against exact resource versions.'],
    ['export','Synthesis & model work','Choose explicit content scope, recipe, target and work envelope. Inspect attribution and derivation before downloading an artifact.']
  ];
  function workbenchView(){return `<div class="page-top"><div><div class="eyebrow">Apply & extend the substrate</div><h1>Workbench</h1><p>Operations act on the same resources you inspect in the library. Inputs, progress, outputs and receipts remain connected.</p></div></div><div class="inset-note">This page maps the full intended workbench. The preview does not execute jobs. Native operation contracts and permission-aware job endpoints are required for the workflows below.</div><section class="sheet" style="margin-top:24px"><div class="tool-list">${workItems.map(([img,title,desc])=>`<article class="tool-row">${icon(img)}<div><h3>${h(title)}</h3><p>${h(desc)}</p></div>${badge('Contract required')}</article>`).join('')}</div></section><p class="preview-note">API capability discovery determines availability. Domain-specific editors and viewers extend the shared resource shell; a universal raw-property table is not the final interface for code, images, audio, games or models.</p>`;}
  function exportsView(){return `<div class="page-top"><div><div class="eyebrow">Portable, attributable results</div><h1>Exports</h1><p>Take an inspectable result with you. Keep its identity, scope, source lineage, and derivation attached.</p></div></div><div class="two-col"><section class="sheet"><div class="section-line"><h2>Captured research data</h2>${badge('Available in preview','green')}</div><p class="muted">Download the actual modular response used by this design, including its acquisition scope and known gaps. This is a data export, not a synthesized model.</p>${facts([['Format','JSON'],['Content','dog lookup and provenance readback'],['Capture',h(capture.capturedAt)],['Charge','Free snapshot download']])}<div style="margin-top:24px"><button class="primary" data-action="download-data">${icon('export')} Download captured data</button></div><p class="preview-note">The exported file contains no access tokens, OAuth credentials, or account details.</p></section><aside class="sheet"><h2>Commercial workflows</h2><p class="muted small">Stripe belongs in explicit purchases and account management, with clear deliverables and receipts.</p><div class="tool-list">${[['Reproducible exports','Scope, format, attribution and resource estimate before a quote.'],['Subscriptions','Named service and work allowances, with a customer portal.'],['Donations','An explicit contribution, separate from knowledge access.']].map(([t,d])=>`<div class="panel-section"><h3>${t}</h3><p class="muted small">${d}</p></div>`).join('')}</div><div class="inset-note">Payments are not connected in this preview. No prices, checkout completions, plans or entitlements are invented.</div></aside></div>`;}
  const scopeItems=[
    ['Shared resource navigation','Collections, detail, ordered structure, evidence and typed links. Query, filters and selection survive reload and Back.','Interactive'],
    ['Research continuity','Local reference collection, notes and exact snapshot export. Account-backed research and compare require contracts.','Partly interactive'],
    ['Identity & authority','Google and Microsoft OIDC; sessions, explicit account linking, workspaces and capability-based resource authorization.','Specified'],
    ['Universal content','Entity identity, physicality, occurrence, tier, type, modality, relation, mask, source, claim and attestation are distinct concepts.','Specified'],
    ['Reasoning & conversation','Query-relative interpretation, ambiguity, typed evidence, calculations, bounded work and witnessed receipts.','Specified'],
    ['Admission & recipes','Acquisition, grammar chaining, library lookup, AST inspection, recipe authoring, preview, streaming jobs and readback.','Specified'],
    ['Construction & domain operations','Code browsing and construction, modality-specific viewers, games, tools, calculations and receipted outcomes.','Specified'],
    ['Synthesis & commerce','Reproducible synthesis and model export, exact attribution, explicit quotes, subscriptions, donations and signed Stripe webhooks.','Specified']
  ];
  function scopeView(){return `<div class="page-top"><div><div class="eyebrow">Reviewable design · not a deployed product</div><h1>One product, shared foundations</h1><p>This preview tests the interaction model against real modular data. The accompanying design and contracts cover the full intended product and name the missing backend work.</p></div></div><div class="scope-list">${scopeItems.map(([title,desc,status])=>`<article class="card"><div style="margin-bottom:12px">${badge(status,status==='Interactive'?'green':'')}</div><h3>${h(title)}</h3><p>${h(desc)}</p></article>`).join('')}</div><section class="sheet long-read" style="margin-top:24px"><h2>Readback and limits</h2><p class="muted small">Captured from the connected modular MCP on ${h(capture.capturedAt)}. Search input dog, fan=12. Eight claim records, twelve of seventy-three containing paths, eight source assertions, no registered witnesses returned. The selected-ID MCP read returned “input or id”; no labels or provenance were invented to fill that gap.</p><p class="muted small">${identity.enabled?'This IIS application uses the installed same-origin identity service for real provider sign-in and session reads. It does not execute live resource queries, ingestion, model synthesis or payments.':'The preview contains no live login, payment, query execution, ingestion, model synthesis, or account writes.'} Its local notebook and JSON downloads are functional. Changing a resource descriptor changes shared collection and detail rendering; special content viewers and operations are an explicit extension point.</p><div class="link-list"><a href="DESIGN.md">Read the product and interaction design ${icon('arrow')}</a><a href="openapi.json">Read the proposed browser API contract ${icon('arrow')}</a><a href="ACCEPTANCE.md">Read the implementation acceptance criteria ${icon('arrow')}</a></div></section>`;}
  function identityButton(){
    const pending=identity.enabled&&identity.state==='loading';
    const label=identity.user?(identity.user.name||identity.user.email||'Account'):(pending?'Checking session…':'Sign in');
    return `<button data-action="signin" aria-label="${identity.user?'Your account':'Sign in'}" ${pending?'disabled':''}>${icon('user')}<span class="account-label">${h(label)}</span></button>`;
  }
  async function refreshIdentity(){
    identity.state='loading';identity.error=null;render();
    try{
      const response=await fetch(runtime.identityPath+'/me',{credentials:'same-origin',cache:'no-store',signal:AbortSignal.timeout(10000)});
      if(!response.ok)throw new Error('Identity service unavailable ('+response.status+').');
      const state=await response.json();
      identity.providers=Array.isArray(state.providers)?state.providers:[];
      identity.user=state.authenticated===true&&state.user?state.user:null;
      identity.state='ready';
    }catch(error){identity.state='error';identity.error=error.name==='TimeoutError'?'The identity service did not respond in time.':error.message;}
    render();
  }
  function openIdentityDialog(){
    if(!identity.enabled){
      showDialog('Sign in to Laplace',`<p>This offline design preview has no identity service.</p><div class="providers"><button data-action="provider" data-provider="Google">${icon('google')} Continue with Google</button><button data-action="provider" data-provider="Microsoft">${icon('microsoft')} Continue with Microsoft</button></div><p class="preview-note">Use the IIS application for actual sign-in. These offline controls do not authenticate or create accounts.</p>`);return;
    }
    if(identity.state==='error'){showDialog('Identity service unavailable',`<p>${h(identity.error)}</p><button data-action="retry-identity">Retry connection</button>`);return;}
    if(identity.user){
      showDialog('Your account',`<p><strong>${h(identity.user.name||identity.user.email||'Signed-in account')}</strong>${identity.user.email?'<br>'+h(identity.user.email):''}</p><p>Signed in through ${h(identity.user.provider||'the shared identity service')}. This session is shared with the mono application on this origin.</p><p class="preview-note">Research notes in this version remain local to this browser. Resource data remains a labelled capture.</p><button data-action="logout">Sign out of this origin</button>`);return;
    }
    const providers=identity.providers.flatMap(provider=>{
      try{
        const url=new URL(provider.loginUrl,location.origin);
        if(url.origin!==location.origin||!url.pathname.startsWith(runtime.identityPath+'/login/'))return [];
        url.searchParams.set('returnUrl',location.pathname+location.search+location.hash);
        return [`<a class="button" data-provider="${h(provider.id)}" href="${h(url.pathname+url.search)}">${icon(provider.id)}Continue with ${h(provider.displayName)}</a>`];
      }catch{return [];}
    });
    showDialog('Sign in to Laplace',`<p>Use your account to continue. You will return to this research address after signing in.</p><div class="providers">${providers.length?providers.join(''):'<p>No identity providers are configured on this host.</p>'}</div><p class="preview-note">Google and Microsoft are handled by the existing identity service. The modular application shares its browser session.</p>`);
  }
  async function logout(){
    try{
      const response=await fetch(runtime.identityPath+'/logout',{method:'POST',credentials:'same-origin',signal:AbortSignal.timeout(10000)});
      if(!response.ok)throw new Error('Sign-out failed ('+response.status+').');
      document.getElementById('dialog').close();await refreshIdentity();notify('Signed out of this origin.');
    }catch(error){notify(error.message);}
  }
  let noticeTimer;
  function notify(message){const el=document.getElementById('notice');el.textContent=message;el.classList.add('visible');clearTimeout(noticeTimer);noticeTimer=setTimeout(()=>el.classList.remove('visible'),4500);}
  function download(name,data){const blob=new Blob([JSON.stringify(data,null,2)+'\n'],{type:'application/json'});const url=URL.createObjectURL(blob);const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);notify('Download prepared.');}
  function showDialog(title,body){const dlg=document.getElementById('dialog');dlg.innerHTML=`<div class="dialog-top"><h2 id="dialog-title">${h(title)}</h2><button class="subtle" data-action="close-dialog" aria-label="Close dialog">${icon('close')}</button></div>${body}`;if(!dlg.open)dlg.showModal();}
  document.addEventListener('submit',e=>{if(e.target.matches('[data-form="search"]')){e.preventDefault();const q=new FormData(e.target).get('q');location.hash=toLibrary({q,selected:null,tab:null,page:null});}});
  document.addEventListener('change',e=>{if(e.target.matches('[data-filter]'))location.hash=toLibrary({[e.target.dataset.filter]:e.target.value,selected:null,tab:null,page:null});});
  document.addEventListener('click',async e=>{
    if(e.target.closest('.skip')){e.preventDefault();document.getElementById('main').focus();return;}
    if(e.target.closest('dialog a'))document.getElementById('dialog').close();const b=e.target.closest('[data-action]');if(!b)return;const action=b.dataset.action;
    if(action==='menu')showDialog('Navigate Laplace',`<nav class="link-list" aria-label="All navigation">${[['home','Home'],['library','Library'],['research','Research'],['sources','Sources'],['workbench','Workbench'],['exports','Exports']].map(([key,label])=>`<a class="button" href="#/${key}">${h(label)} ${icon('arrow')}</a>`).join('')}</nav>`);
    if(action==='signin')openIdentityDialog();
    if(action==='retry-identity'){document.getElementById('dialog').close();await refreshIdentity();}
    if(action==='logout')await logout();
    if(action==='provider')showDialog(`${b.dataset.provider} connection required`,`<p>This design preview has no identity server. The live product will use the provider’s hosted authorization screen and return to your original research address.</p><p>Provider secrets and the modular service token stay on the server. A browser session identifies your account; resource access is evaluated independently.</p><button data-action="close-dialog">Return to preview</button>`);
    if(action==='close-dialog')document.getElementById('dialog').close();
    if(action==='pin'){const key=b.dataset.key;const index=notebook.pins.indexOf(key);if(index===-1)notebook.pins.push(key);else notebook.pins.splice(index,1);persist();render();notify(index===-1?'Reference saved to research.':'Reference removed.');}
    if(action==='save-note'){notebook.note=document.getElementById('research-note').value;persist();notify('Notes saved in this browser.');}
    if(action==='copy'){try{await navigator.clipboard.writeText(b.dataset.text);notify('Full identifier copied.')}catch{showDialog('Entity identifier',`<p class="mono">${h(b.dataset.text)}</p><p>Select and copy this identifier.</p>`);}}
    if(action==='download-data')download('laplace-dog-captured-readback.json',capture);
    if(action==='download-notebook')download('laplace-research.json',{format:'laplace-design-notebook-v1',exportedAt:new Date().toISOString(),note:notebook.note,references:notebook.pins.map(key=>({key,resource:resources.get(key),capture:capture.capturedAt,query:capture.query,href:toLibrary({q:'dog',selected:key,tab:null})})),scope:'Local design preview; not an authenticated research receipt'});
    if(action==='more-results')showDialog('A bounded design collection','<p>The native readback is sampled. This preview shows up to 16 collection rows to keep the interaction review focused. The production API needs snapshot-bound cursor pagination and qualified counts; it must not imply a complete catalogue from this sample.</p>');
  });
  document.addEventListener('input',e=>{if(e.target.id==='research-note'){notebook.note=e.target.value;persist();}});
  window.addEventListener('hashchange',()=>{
    scrollPositions.set(lastHash,window.scrollY);
    render();
    window.scrollTo(0,scrollPositions.get(location.hash)||0);
    lastHash=location.hash;
  });
  // Expose read-only design metadata for acceptance, not an application integration API.
  window.LAPLACE_PREVIEW=Object.freeze({resourceCount:resources.size,descriptors:Object.keys(descriptors),mode:'captured-readback'});
  render();
  if(identity.enabled)refreshIdentity();
})();
