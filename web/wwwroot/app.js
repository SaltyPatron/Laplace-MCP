/* Live native projections. Browser code owns navigation and presentation only. */
(() => {
  'use strict';
  const runtime=window.LAPLACE_RUNTIME||{}, base=runtime.basePath||'';
  const h=v=>String(v??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const short=id=>id?`${id.slice(0,8)}…${id.slice(-4)}`:'';
  const number=v=>Number(v).toLocaleString();
  const paths={logo:'M12 2 22 12 12 22 2 12Z M7 12l5-5 5 5-5 5Z',home:'m3 10 9-7 9 7v11H3Z M9 21v-8h6v8',library:'M3 4h5v16H3Z M10 4h5v16h-5Z M18 4l4 15-4 1-4-15Z',research:'M5 3h14v18H5Z M9 7h6 M9 11h6 M9 15h4',source:'m12 3 9 5-9 5-9-5Z M3 12l9 5 9-5 M3 16l9 5 9-5',workbench:'M4 4h6v6H4Z M14 4h6v6h-6Z M4 14h6v6H4Z M14 14h6v6h-6Z',export:'M12 3v12 m-5-5 5 5 5-5 M4 16v5h16v-5',search:'M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0 M16 16l5 5',arrow:'M4 12h16 m-6-6 6 6-6 6',back:'M20 12H4 m6-6-6 6 6 6',user:'M16 8a4 4 0 1 1-8 0 4 4 0 0 1 8 0 M4 21v-2a8 8 0 0 1 16 0v2',pin:'m9 3 9 9-4 1-3 5-5-5 5-3Z M7 17l-4 4',close:'m6 6 12 12 M6 18 18 6',copy:'M8 8h12v13H8Z M16 8V3H3v13h5',link:'m9 15 6-6 M7 13l-2 2a4 4 0 0 0 6 6l4-4 M13 7l2-2a4 4 0 0 1 6 6l-4 4',more:'M4 12h2 M11 12h2 M18 12h2',google:'M21 12h-9 m8 0a8 8 0 1 1-2-6 m2 6v5',microsoft:'M3 3h8v8H3Z M13 3h8v8h-8Z M3 13h8v8H3Z M13 13h8v8h-8Z'};
  const icon=name=>`<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="${paths[name]||paths.library}"/></svg>`;
  const badge=(text,tone='')=>`<span class="badge ${tone}">${h(text)}</span>`;
  const descriptors={};
  const model=()=>request('schema','schema',data=>Object.assign(descriptors,data.views));
  const requests=new Map(), resources=new Map(), labels=new Map(), readbacks=new Map(), graphs=new Map(), observedSources=new Map();
  const queryDrafts=new Map();
  const identity={state:'loading',providers:[],user:null,error:null};
  let lastQuery='',lastPage='',lastCollection='',lastDetail='',noticeTimer,operationResult=null,operationBusy=false,selectedOperation='embeddings',activeOperationDraft='';
  const operationDrafts={};
  const storageKey='laplace-modular-research-v2';
  let notebook;
  try{notebook=JSON.parse(localStorage.getItem(storageKey)||'null')}catch{}
  if(!notebook||!Array.isArray(notebook.references))notebook={note:'',references:[]};
  const persist=()=>{try{localStorage.setItem(storageKey,JSON.stringify(notebook))}catch{notify('Browser storage is unavailable. Export your notes to retain them.')}};
  const route=()=>{const [path,query='']=(location.hash.slice(1)||'/library').split('?');return{page:path.slice(1),params:new URLSearchParams(query)}};
  const world=window.LaplaceWorld,readModel=window.LaplaceRead;
  let lastWorld=new URLSearchParams();
  function worldRoute(page,patch={}){const current=route(),params=(current.params.size||['library','sources'].includes(current.page))?current.params:lastWorld;return world.address(page,params,patch)}
  function library(patch={}){const current=route(),page=current.page==='sources'?'sources':'library',params=(current.params.size||['library','sources'].includes(current.page))?current.params:lastWorld;const updates={...patch};if(Object.hasOwn(patch,'selected')&&patch.selected!==params.get('selected')&&!Object.hasOwn(patch,'parent')){updates.parent=null;updates.at=null;updates.form=null;updates.line=null}return world.address(page,params,updates)}

  const entityKey=id=>'entity:'+id;
  const label=id=>labels.get(id)||short(id);
  const link=(id,text)=>`<a href="${h(library({selected:entityKey(id),tab:null}))}">${h(text||label(id))}</a>`;
  const resourceLink=r=>`<a href="${h(library({selected:r.key,tab:null}))}">${h(r.label)}</a>`;
  function discoveryNotice(read){
    if(!read)return '';
    const notices=[];
    if(!readModel.known(read))notices.push(read.storage==='computed'?'Laplace calculated an identity for this input; no recorded or cached content was returned for that complete input. Known constituents and recorded relationships are listed when returned.':'No recorded or cached content was returned for this exact address.');
    if(read.discovery?.members_complete===false)notices.push(`This read includes ${read.constituents?.length||0} of ${read.parts_total} query constituents. It cannot establish absence outside that returned part.`);
    return notices.map(message=>`<p class="inset-note" role="status">${h(message)}</p>`).join('');
  }
  function retainedContext(params){const fields=['world','source','evidence','epoch','from','until','rank','group','relation','arena','event','authority'].filter(key=>params.has(key));return fields.length?`<aside aria-label="Retained context"><p>These coordinates remain in your address, but this search does not apply them as constraints.</p>${facts(fields.map(key=>[key,h(params.getAll(key).join(' · '))]))}</aside>`:''}
  const add=r=>{resources.set(r.key,{...resources.get(r.key),...r});return r.key};
  const facts=items=>`<dl class="facts">${items.map(([k,v])=>`<div><dt>${h(k)}</dt><dd>${v}</dd></div>`).join('')}</dl>`;
  const section=(title,content)=>`<section class="panel-section"><h2>${h(title)}</h2>${content}</section>`;
  const status=(title,message,action='')=>`<div class="empty" role="status">${icon('library')}<h2>${h(title)}</h2><p>${h(message)}</p>${action}</div>`;
  function request(key,path,accept){
    if(requests.has(key))return requests.get(key);
    const entry={state:'loading',path};requests.set(key,entry);
    fetch(base+'/api/'+path,{cache:'no-store',credentials:'same-origin',signal:AbortSignal.timeout(60000)})
      .then(async response=>{if(!response.ok){let problem={};try{problem=await response.json()}catch{}throw new Error(problem.detail||`Read failed (HTTP ${response.status}).`)}return response.json()})
      .then(data=>{if(requests.get(key)!==entry)return;entry.state='ready';entry.data=data;entry.readAt=new Date().toISOString();accept?.(data);render()})
      .catch(error=>{if(requests.get(key)!==entry)return;entry.state='error';entry.error=error.name==='TimeoutError'?'The native read timed out. Retry to reconnect.':error.message;render()});
    return entry;
  }
  function requestView(entry,ready){return entry.state==='ready'?ready(entry.data):entry.state==='error'?status('Read unavailable',entry.error,'<button data-action="retry">Retry read</button>'):status('Reading the substrate…','Loading live content and its structural references.');}
  function register(read){
    const previousRead=readbacks.get(read.id);
    if(read.computed_tier===null&&previousRead?.computed_tier!==null&&previousRead?.computed_tier!==undefined)read.computed_tier=previousRead.computed_tier;
    readbacks.set(read.id,read);
    for(const item of read.labels||[])if(item.text)labels.set(item.id,item.text);
    if(read.display?.title)labels.set(read.id,read.display.trunk_kind==='file'?read.display.title.split('/').filter(Boolean).at(-1):read.display.title);
    for(const item of read.constituents||[])if(Number.isInteger(item.codepoint)&&item.codepoint>=0)labels.set(item.id,String.fromCodePoint(item.codepoint));
    const parts=read.parts?.length?read.parts:read.physicality?.[0]?.constituents||[];
    const previous=resources.get(entityKey(read.id));
    const rootKind=read.display?.trunk_kind|| (read.physicality?.some(p=>p.claim)?'claim':previous?.kind==='container'?'container':'entity');
    add({key:entityKey(read.id),id:read.id,kind:rootKind,tier:read.tier,label:label(read.id),parts,recorded:read.recorded,read,summary:read.recorded?'Stored canonical content':read.storage?.endsWith('-cache')?'Canonical content in '+read.storage:read.storage==='unresolved'?'Unresolved identity':'Computed canonical identity'});
    for(const [i,p] of (read.physicality||[]).entries())add({key:`physicality:${read.id}:${i}`,id:read.id,kind:'physicality',tier:p.tier,label:label(read.id),parts:p.constituents,recorded:true,record:p,read,summary:`${p.n} ordered positions · paired with ${short(read.id)}`});
    for(const member of read.constituents||[])if(readModel.known(member))add({key:entityKey(member.id),id:member.id,kind:resources.get(entityKey(member.id))?.kind||'entity',tier:member.tier,label:label(member.id),parts:(member.constituents||[]).map(part=>part.id),recorded:member.recorded,storage:member.storage,summary:member.recorded?'Stored constituent content':'Canonical constituent in '+member.storage});
    for(const context of [read,...read.constituents||[]])for(const [kind,rows] of [['claim',context.claims||[]],['container',context.occurrences||[]]])for(const item of rows)add({key:entityKey(item.entity),id:item.entity,kind,tier:item.tier,label:label(item.entity),parts:item.constituents,recorded:true,record:item,summary:`${item.n} ordered positions${kind==='claim'?(item.stood?' · standing available':' · standing not returned'):''}`});
    for(const [i,a] of (read.attestations||[]).entries()){
      const source={id:a.trunk,key:entityKey(a.trunk),kind:'source',label:label(a.trunk),registered:a.witness,trust:a.trust,summary:'Source trunk reached by native attestation traversal'};
      observedSources.set(a.trunk,{...observedSources.get(a.trunk),...source});add(source);
      add({key:`attestation:${a.claim}:${a.trunk}:${i}`,id:null,kind:'attestation',label:'Assertion of '+label(a.claim),record:a,summary:'Attributed to '+label(a.trunk)});
    }
    for(const r of [...resources.values()])for(const id of r.parts||[])if(!resources.has(entityKey(id)))add({key:entityKey(id),id,kind:'entity',label:label(id),summary:'Referenced content · open to resolve'});
  }
  function collection(read){
    const rows=new Map();
    const include=(key,method,match)=>{const resource=resources.get(key);if(!resource)return;const existing=rows.get(key);if(existing){if(!existing.methods.includes(method))existing.methods.push(method);if(match)existing.matches.push(match)}else rows.set(key,{...resource,methods:[method],matches:match?[match]:[]})};
    for(const match of readModel.matches(read))include(entityKey(match.id),match.match,match);
    for(const context of [read,...read.constituents||[]]){for(const claim of context.claims||[])include(entityKey(claim.entity),'assertions');for(const parent of context.occurrences||[])include(entityKey(parent.entity),'containment')}
    for(const [index] of (read.physicality||[]).entries())include(`physicality:${read.id}:${index}`,'structure');
    return [...rows.values()];
  }
  function lookup(q){
    const content=request('query:'+q,'lookup?q='+encodeURIComponent(q),read=>{register(read);requests.get('query:'+q).rows=collection(read)});
    const following=request('continuations:'+q,'continuations?q='+encodeURIComponent(q));
    const rows=new Map((content.rows||[]).map(row=>[row.key,{...row,methods:[...row.methods]}]));
    for(const span of following.data?.forward||[]){
      if(!/^[0-9a-f]{32}$/i.test(span.next))continue;
      const key=entityKey(span.next),existing=rows.get(key);
      if(existing){if(!existing.methods.includes('continuations'))existing.methods.push('continuations')}
      else{const item={...resources.get(key),key,id:span.next,kind:'entity',label:label(span.next),summary:`Observed continuation · ${span.times} occurrences`,methods:['continuations'],record:span};if(!resources.has(key))add(item);rows.set(key,{...item})}
    }
    const methods=[['content, assertions and containment',content],['observed continuations',following]];
    return {state:methods.some(([,entry])=>entry.state==='ready')?'ready':methods.every(([,entry])=>entry.state==='error')?'error':'loading',error:methods.filter(([,entry])=>entry.state==='error').map(([name,entry])=>name+': '+entry.error).join(' · '),pending:methods.some(([,entry])=>entry.state==='loading'),data:content.data,rows:[...rows.values()],methods};
  }
  function browse(after=''){return request('library:'+after,'library'+(after?'?after='+encodeURIComponent(after):''),data=>{for(const item of data.items){const title=item.kind==='file'?item.title.split('/').filter(Boolean).at(-1):item.title;labels.set(item.id,title);const resource={key:entityKey(item.id),id:item.id,kind:item.kind==='trunk'?'container':item.kind,tier:item.tier,label:title,parts:[item.metadata,item.content].filter(Boolean),children:item.children||[],recorded:true,summary:item.kind==='source'?`${item.children?.length??0} ordered members in its file collection`:item.title};add(resource);if(['source','file'].includes(resource.kind))observedSources.set(item.id,resource)}requests.get('library:'+after).rows=data.items.map(item=>({...resources.get(entityKey(item.id))}))})}
  function detailRead(id){return request('resource:'+id,'resources/'+id,read=>{register(read);const source=observedSources.get(id);if(source)add({...source,label:label(id),kind:read.display?.trunk_kind||source.kind,parts:read.physicality?.[0]?.constituents||read.parts,tier:read.tier,read,recorded:read.recorded})})}
  function provenance(id){return request('provenance:'+id,'provenance/'+id,graph=>{graphs.set(id,graph);for(const node of graph.nodes){const title=node.trunk_kind==='file'?node.label.split('/').filter(Boolean).at(-1):node.label;if(title)labels.set(node.id,title);if(node.metadata_trunk||node.registered){const source={id:node.id,key:entityKey(node.id),kind:node.trunk_kind||'source',tier:node.tier,label:title||label(node.id),registered:node.registered,trust:node.trust,metadataTrunk:node.metadata_trunk,summary:node.metadata_trunk?'Metadata trunk in the containment ancestry':'Registered source trunk'};observedSources.set(node.id,{...observedSources.get(node.id),...source});add(source)}}})}
  function searchForm(value='',hero=false){const draft=queryDrafts.get(location.hash);return `<form class="search" data-form="search" role="search"><label class="visually-hidden" for="query">Search content or an identifier</label>${icon('search')}<input id="query" name="q" value="${h(draft??value)}" placeholder="Search content or paste a canonical ID" autocomplete="off" required><button type="submit" class="primary">Search${hero?icon('arrow'):''}</button></form>`}
  function checkboxFacet(title,key,values,selected){
    const options=[...new Set([...values,...selected])];
    if(options.length<2&&!selected.length)return '';
    return `<fieldset class="collection-facet"><legend>${h(title)}</legend>${options.map(value=>`<label><input type="checkbox" data-filter="${key}" data-focus="filter-${h(key)}-${h(value)}" value="${h(value)}" ${selected.includes(String(value))?'checked':''}>${h(key.endsWith('kind')?descriptors[value]?.plural||value:value)} <small>(${values.filter(item=>String(item)===String(value)).length})</small></label>`).join('')}</fieldset>`;
  }
  function libraryView(params,sourceMode=false){
    const q=sourceMode?'':params.get('q')||'',kindKey=sourceMode?'source_kind':'kind',tierKey=sourceMode?'source_tier':'tier',pageKey=sourceMode?'source_page':'page',afterKey=sourceMode?'source_after':'after',filterKey=sourceMode?'source_filter':'filter',kinds=params.getAll(kindKey).filter(value=>value!=='all'),tiers=params.getAll(tierKey),matched=sourceMode?[]:params.getAll('matched'),selected=params.get('selected'),filter=params.get(filterKey)||'';
    lastWorld=new URLSearchParams(params);
    if(q)lastQuery=q;
    const entry=q?lookup(q):sourceMode?browse(params.get(afterKey)||''):{state:'ready',rows:[]};
    const all=entry.rows||[];
    const scoped=sourceMode?all.filter(row=>['source','file'].includes(row.kind)):all;
    const rows=scoped.filter(row=>(!kinds.length||kinds.includes(row.kind))&&(!tiers.length||tiers.includes(String(row.tier)))&&(!matched.length||row.methods?.some(method=>matched.includes(method)))&&(!filter||[row.label,row.summary].some(text=>text?.toLocaleLowerCase().includes(filter.toLocaleLowerCase()))));
    const pageSize=12,pages=Math.max(1,Math.ceil(rows.length/pageSize)),page=Math.max(1,Math.min(pages,Number(params.get(pageKey))||1));
    const open=selected?all.find(row=>row.key===selected)||resources.get(selected)||{key:selected,id:selected.split(':')[1],kind:'entity',label:short(selected.split(':')[1]),summary:'Resolving canonical resource'}:null;
    const master=requestView(entry,()=>rows.length?rows.slice((page-1)*pageSize,page*pageSize).map(row=>`<a class="result ${row.key===selected?'selected':''}" href="${h(library({selected:row.key,tab:null}))}" ${row.key===selected?'aria-current="true"':''}><div class="result-top"><span class="result-name">${h(row.label)}</span>${row.tier!==undefined?`<span class="tier">T${row.tier}</span>`:''}</div><p>${h(row.summary)}</p>${badge(descriptors[row.kind]?.name||row.kind)} ${row.matches?.filter(match=>match.match==='member').length>1?`<small>${row.matches.filter(match=>match.match==='member').length} query member positions</small>`:''} ${row.methods?`<small>Matched by ${h(row.methods.join(', '))}</small>`:''}</a>`).join(''):status(!sourceMode&&!q?'Search knowledge':'No records in this view',!sourceMode&&!q?'Enter a subject or content. Content, assertion, containment and observed continuation searches run together.':'Clear filters to see all records returned in this read.',q||sourceMode?`<a href="${h(library({[kindKey]:null,[tierKey]:null,matched:sourceMode?params.getAll('matched'):null,[filterKey]:null,[pageKey]:null}))}">Clear filters</a>`:''));
    const read=q?entry.data:null;
    const caption=read?`${rows.length} of ${scoped.length} results · root read: ${number(read.claims_read)} claims and ${number(read.occurrences_read)} containing paths · each root/member read returns up to ${read.fan} of each.`:q?`${rows.length} results from completed search methods.`:sourceMode?`${rows.length} of ${scoped.length} sources and files in this catalogue page.`:'Search methods combine in one result collection.';
    const pager=`<nav class="collection-pager" aria-label="Collection pages"><span>${page>1?`<a href="${h(library({[pageKey]:page-1}))}" aria-label="Previous page">${icon('back')}</a>`:''}</span><span>Page ${page} of ${pages}</span><span>${page<pages?`<a href="${h(library({[pageKey]:page+1}))}" aria-label="Next page">${icon('arrow')}</a>`:''}</span></nav>`;
    const cataloguePages=!q&&entry.data?.has_more?`<a class="button catalogue-next" href="${h(library({[afterKey]:entry.data.next,[pageKey]:null,selected:null,tab:null}))}">Next catalogue page ${icon('arrow')}</a>`:'';
    return `<div class="library-page ${selected?'has-selection':''}"><div class="page-top"><div><h1>${sourceMode?'Sources and files':'Knowledge'}</h1><p>${sourceMode?'Inspect where a claim came from and follow its recorded content.':'Search content, assertions and relations. Select a result to keep investigating beside the same collection.'}</p></div><button data-action="refresh">Refresh</button></div><div class="filters">${sourceMode?`<form class="search" data-form="source-filter"><label class="visually-hidden" for="source-filter">Filter sources and files</label><input id="source-filter" name="filter" value="${h(filter)}" placeholder="Filter sources and files"><button>Filter</button></form>`:searchForm(q)}</div>${retainedContext(params)}<div class="collection-facets" role="group" aria-label="Collection filters">${checkboxFacet('Resource types',kindKey,scoped.map(row=>row.kind),kinds)}${checkboxFacet('Tiers',tierKey,scoped.map(row=>row.tier).filter(tier=>tier!==undefined).sort((a,b)=>a-b).map(String),tiers)}${checkboxFacet('Matched by','matched',scoped.flatMap(row=>row.methods||[]),matched)}<a href="${h(library({[kindKey]:null,[tierKey]:null,matched:sourceMode?params.getAll('matched'):null,[filterKey]:null,[pageKey]:null}))}">Clear filters</a></div><p class="result-caption">${h(caption)}${q?entry.pending?' Search in progress; completed results are available.':'':''}${scoped.length?' Checkbox counts cover this read before filtering.':''}</p>${q?discoveryNotice(entry.data):''}${entry.error?`<p class="search-error" role="alert">${h(entry.error)} <button data-action="retry">Retry unavailable methods</button></p>`:''}<section class="library ${selected?'has-detail':''}" aria-label="Resource collection and detail"><div class="master"><div class="master-title">Records ${badge(rows.length)}</div>${master}${pages>1?pager:''}${cataloguePages}</div><article class="detail" aria-label="Selected record">${open?detailView(open,params):status('Select a result','Inspect its meaning, assertions and relations here. Your search, filters and page remain in place.')}</article></section></div>`;
  }
  function detailView(r,params){
    const tab=['overview','structure','evidence','lineage','connections','calculate','content'].includes(params.get('tab'))?params.get('tab'):'overview';
    const native=r.id?detailRead(r.id):null;
    const current={...resources.get(r.key),...r},descriptor=descriptors[current.kind]||descriptors.entity||{name:current.kind,description:''};
    const calculationInput=params.get('q'),canCalculate=calculationInput&&requests.get('query:'+calculationInput)?.data?.id===r.id;
    const tabs=r.kind==='attestation'?['overview','evidence']:['overview',...(current.kind==='file'?['content']:[]),'structure','evidence','lineage','connections',...(canCalculate?['calculate']:[])];
    const pinned=notebook.references.some(x=>x.key===r.key);
    const contents=()=>tab==='content'&&current.kind==='file'?fileView(current,params):tab==='calculate'&&canCalculate?workbenchView({embedded:true,input:calculationInput}):tab==='structure'?structure(current):tab==='evidence'?evidence(current):tab==='lineage'?lineage(current):tab==='connections'?connections(current):overview(current);
    return `<a class="back-mobile" href="${h(library({selected:null,tab:null}))}">${icon('back')} Back to results</a><div class="detail-heading"><div>${badge(descriptor.name)} ${current.tier!==undefined?badge('Tier '+current.tier):''}<h1>${h(current.label)}</h1></div><div class="detail-actions">${r.id?`<button data-action="export-current" data-key="${h(r.id)}" aria-label="Download readback">${icon('export')}<span class="button-label">Download</span></button>`:''}<button data-action="pin" data-key="${h(r.key)}" aria-pressed="${pinned}" aria-label="${pinned?'Remove reference from':'Save reference to'} research">${icon('pin')}<span class="button-label">${pinned?'Saved':'Save'}</span></button></div></div><p class="detail-intro">${h(descriptor.description)}</p>${r.id?`<div class="identity-line"><code>${h(r.id)}</code><button class="subtle" data-action="copy" data-text="${h(r.id)}" aria-label="Copy full entity identifier">${icon('copy')}</button></div>`:''}${occurrenceView(current,params)}<nav class="tabs" aria-label="Resource detail views">${tabs.map(t=>`<a data-focus="tab-${t}" href="${h(library({tab:t==='overview'?null:t}))}" ${tab===t?'aria-current="page"':''}>${t[0].toUpperCase()+t.slice(1)}</a>`).join('')}</nav><div id="detail-panel" data-resource="${h(r.key)}" data-state="${native?.state||'ready'}">${native&&native.state!=='ready'?requestView(native,contents):contents()}</div>`;
  }
  function currentRead(r){return r.id?readbacks.get(r.id)||r.read:null}
  function fileView(resource,params){
    const entry=request('file:'+resource.id,'files/'+resource.id);
    return requestView(entry,data=>{
      const file=data.files?.find(item=>item.file===resource.id);
      if(!file)return status('Recorded file unavailable','The native read returned no matching file.');
      const text=typeof file.text==='string'?file.text:'',lines=text.split('\n'),requested=Number(params.get('line')||1);
      const valid=Number.isSafeInteger(requested)&&requested>=1&&requested<=lines.length;
      const start=valid?Math.max(1,requested-5):1,end=Math.min(lines.length,start+199);
      return section('Recorded content',`<p>${h(file.path||resource.label)}</p><p class="small muted">Native reconstruction of the selected file. This read is bounded; it does not certify a byte-exact original artifact.</p>${!valid?'<p role="alert">The requested line was not returned in this read.</p>':''}${text?`<div class="code-window" role="region" aria-label="Recorded file lines">${lines.slice(start-1,end).map((line,index)=>`<div id="code-line-${start+index}" class="code-line ${start+index===requested?'selected':''}"><a aria-label="Line ${start+index}" href="${h(library({line:start+index,tab:'content'}))}">${start+index}</a><code>${h(line)||' '}</code></div>`).join('')}</div><nav class="collection-pager" aria-label="Recorded text pages">${start>1?`<a href="${h(library({line:Math.max(1,start-195),tab:'content'}))}">Previous lines</a>`:'<span></span>'}<span>Lines ${start}–${end} of ${lines.length} returned</span>${end<lines.length?`<a href="${h(library({line:end+1,tab:'content'}))}">Next lines</a>`:'<span></span>'}</nav>`:'<p>No reconstructed text was returned.</p>'}`)+section('Declarations',file.declarations?.length?`<ol class="declaration-list">${file.declarations.map(declaration=>`<li>${Number.isSafeInteger(declaration.line)&&declaration.line>0?`<a href="${h(library({line:declaration.line,tab:'content'}))}">${h(declaration.name)}</a>`:h(declaration.name)} ${badge(declaration.kind)} ${declaration.line?'· line '+h(declaration.line):''}</li>`).join('')}</ol>`:'<p>No typed declarations were returned for this file.</p>')+`<details class="storage-detail"><summary>Content read scope</summary><p>Native file_outline · canonical file <code>${h(resource.id)}</code> · ${h(file.bytes)} reconstructed bytes returned · read ${h(entry.readAt)}</p></details>`;
    });
  }
  function occurrenceAddress(id,parent,ordinal,form=0){return library({selected:entityKey(id),parent,at:String(ordinal),form:String(form),tab:null,line:null})}
  function occurrenceView(resource,params){
    const parent=params.get('parent'),raw=params.get('at'),form=params.get('form')||'0';
    if(!parent||raw===null)return '';
    if(!/^[a-f0-9]{32}$/i.test(parent)||!/^\d+$/.test(raw)||!Number.isSafeInteger(Number(raw))||!/^\d+$/.test(form)||!Number.isSafeInteger(Number(form)))return '<p role="alert">Invalid occurrence reference.</p>';
    return requestView(detailRead(parent),read=>{
      const parts=read.physicality?.[Number(form)]?.constituents||(Number(form)===0?read.parts:[])||[],ordinal=Number(raw);
      if(parts[ordinal]!==resource.id)return '<p role="alert">This occurrence was not returned at the requested parent and ordinal. The reference has been retained.</p>';
      return `<aside class="occurrence-context" aria-label="Selected occurrence">Position ${ordinal+1} in <a href="${h(library({selected:entityKey(parent),tab:'structure',parent:null,at:null,form:null}))}">${h(label(parent))}</a><p class="small muted">Physicality ${h(Number(form)+1)} in this bounded read. Parent, physicality and logical ordinal distinguish repeated equal content; this address does not bind a database snapshot.</p></aside>`;
    });
  }
  function overview(r){
    if(r.kind==='attestation'){const a=r.record;return section('An assertion and its source',facts([['Claim',link(a.claim)],['Source trunk',link(a.trunk)],['Games',h(a.games)],['Tokens',h(a.tokens)],['Series score',h(a.score)],['Position',h(a.position??'Not returned')]]))+'<p class="inset-note">The series score describes the assertion’s recorded outcomes. It is distinct from the claim’s standing and its uncertainty.</p>'}
    const read=currentRead(r),parts=r.kind==='physicality'?r.parts:read?.physicality?.[0]?.constituents||read?.parts||r.parts||[];
    if(read&&!readModel.known(read))return section(read.storage==='computed'?'Calculated identity':'Unresolved address',`<p>${read.storage==='computed'?'This deterministic calculation does not establish recorded or cached content for the selected root.':'The exact identifier was not returned from the database or selected canonical caches.'}</p>${facts([['Storage',h(read.storage)],['Canonical ID',h(r.id)]])}<p>Related known content may still be available in the originating search.</p>`);
    if(read?.display&&['source','file'].includes(r.kind)){
      const display=read.display;
      return section(r.kind==='source'?'Source collection':'Recorded file',facts([['Tier',h(read.tier)],['Storage',badge(read.recorded?'Returned as stored':'Not returned as stored','green')],['Metadata',link(display.metadata,r.kind==='file'?'Inspect file metadata':'Inspect source metadata')],['Content',link(display.content,r.kind==='file'?'Open recorded content':'Open ordered file collection')]]))+(r.kind==='source'&&r.children?.length?section('Files in source order',`<div class="file-grid">${r.children.map(id=>`<a class="file-card" href="${h(library({selected:entityKey(id),tab:null}))}">${icon('library')}<span><strong>${h(label(id))}</strong><small>${h(id)}</small></span>${icon('arrow')}</a>`).join('')}</div>`):r.kind==='file'?section('Provenance',`<p>${h(display.title)}</p><a href="${h(library({tab:'lineage'}))}">Follow this file to its source ${icon('arrow')}</a>`):'');
    }
    const claims=read?.claims||[];
    const knowledge=section(r.kind==='claim'?'Assertion':'Content',`<p class="knowledge-content">${h(label(r.id))}</p>${r.kind==='claim'&&parts.length?members(parts,r.id):''}`)+(claims.length?section('Assertions involving this content',claims.map(claim=>`<div class="return-row"><div><strong>${link(claim.entity)}</strong><p>${claim.constituents.map(id=>link(id)).join(' · ')}</p></div>${claim.stood?badge('Standing recorded','green'):''}</div>`).join('')):'');
    return `${knowledge}${r.record?.stood?section('Standing',facts([['Rating',h(r.record.rating)],['Deviation',h(r.record.deviation)],['Confidence',h(r.record.confidence)],['Matches',number(r.record.matches)]])):r.kind==='claim'?section('Standing','<p>No standing was returned for this claim.</p>'):''}${parts.length&&r.kind!=='claim'?section('Ordered composition',members(parts,r.id)):''}${facets(read?.entity_facets,'Classifications')}<details class="storage-detail"><summary>Storage and read scope</summary>${facts([['Storage',badge(read?.recorded?'Database':read?.storage||'Unresolved')],['Stored tier',h(read?.stored_tier??r.tier??'Not returned')],['Physicalities returned',number(read?.physicality?.length||0)],['Claim records read',number(read?.claims_read||0)],['Containing paths read',number(read?.occurrences_read||0)]])}${read?.computed_tier!=null&&read?.stored_tier!=null&&read.computed_tier!==read.stored_tier?`<p class="inset-note" data-tier-mismatch>Stored tier ${h(read.stored_tier)} differs from the native text modality tier ${h(read.computed_tier)} for this same ID.</p>`:''}</details>`;
  }
  function members(parts,parent){return `<div class="member-list">${parts.map((id,i)=>`<span class="member">${i+1} · ${parent?`<a href="${h(occurrenceAddress(id,parent,i,0))}">${h(label(id))}</a>`:link(id)}</span>`).join('')}</div>`}
  function facets(items,title){return items?.length?section(title,`<dl class="facet-list">${items.map(field=>`<div><dt>${h(field.bank)} <small>${h(field.carrier)}</small></dt><dd>${field.values.map(value=>link(value.id,value.label)).join(' · ')}</dd></div>`).join('')}</dl>`):''}
  function structure(r){
    const read=currentRead(r),stored=r.kind==='physicality'?[r.record]:read?.physicality||[],cached=!stored.length&&read?.storage?.endsWith('-cache')&&read.parts?.length;
    const views=cached?[{tier:read.canonical_tier??read.tier,n:read.parts_total,constituents:read.parts,source:read.storage}]:stored;
    const name=id=>id===read?.display?.metadata?(r.kind==='file'?'File metadata':'Source metadata'):id===read?.display?.content?(r.kind==='file'?'Recorded file content':'File collection'):label(id);
    const roles=new Map((model().data?.vertex_roles||[]).map(role=>[role.value,role.name]));
    const children=r.children?.length?section('Files in source order',`<div class="file-grid">${r.children.map(id=>`<a class="file-card" href="${h(library({selected:entityKey(id),tab:null}))}"><span><strong>${h(label(id))}</strong><small>${h(id)}</small></span>${icon('arrow')}</a>`).join('')}</div>`):'';
    return (views.length?views.map((p,index)=>{
      let vertexIndex=0,repeat=0;
      const rows=p.constituents.map((id,i)=>{const occurrence=p.vertices?.[vertexIndex];if(occurrence&&++repeat>=occurrence.run){vertexIndex++;repeat=0}return `<tr><td>${i+1}</td><td><a href="${h(occurrenceAddress(id,r.id,i,r.kind==='physicality'?Number(r.key.split(':').at(-1)):index))}">${h(name(id))}</a></td><td><code>${h(id)}</code></td><td>${h(roles.get(occurrence?.said)??'Constituent')}</td></tr>`}).join('');
      return section(views.length===1?'Physicality':'Physicality '+(index+1),`<p>Physical form of ${link(read.id)} · tier ${h(p.tier)} · ${number(p.n)} ordered positions${p.source?' · '+h(p.source):''}</p><div class="table-wrap"><table><thead><tr><th scope="col">Position</th><th scope="col">Content</th><th scope="col">Canonical ID</th><th scope="col">Use in this path</th></tr></thead><tbody>${rows}</tbody></table></div>${facets(p.facets,'Physicality classifications')}${p.vertices?'<details class="stored-annotations">':''}${p.vertices?`<summary>Stored carrier values</summary><p class="small muted">Mask: <code>${h(p.mask)}</code></p><pre class="json-result">${h(JSON.stringify(p.vertices,null,2))}</pre></details>`:''}`);
    }).join(''):section('Structure','<p>No stored physicality was returned for this resource.</p>'))+children;
  }
  function evidence(r){
    const read=currentRead(r),assertions=r.kind==='attestation'?[r.record]:r.kind==='source'?[...readbacks.values()].flatMap(x=>x.attestations||[]).filter(a=>a.trunk===r.id):(read?.attestations||[]);
    const unique=assertions.filter((a,i,all)=>all.findIndex(b=>b.claim===a.claim&&b.trunk===a.trunk)===i);
    return section('Source-attributed assertions',unique.length?unique.map(a=>`<div class="evidence-row"><div><h3>${link(a.claim)}</h3><p>Source trunk ${link(a.trunk)}</p><p>${h(a.games)} games · ${h(a.tokens)} tokens · score ${h(a.score)}${a.position!==null?' · position '+h(a.position):''}</p></div><span>${badge(a.witness?'Registry metadata available':'Containment-attributed')}</span></div>`).join(''):'<p>The native attestation read returned no assertion rows for this selection. Inspect Lineage for structural containment; this result is not a census of source trunks.</p>')+`<p><a href="${h(worldRoute('sources',{selected:null,tab:null}))}">Browse source catalogue</a></p>`+(read?.claims?.length?section('Claims involving this content',read.claims.map(c=>`<div class="return-row"><div><strong>${link(c.entity)}</strong><p>${c.constituents.length} ordered members · ${c.stood?'standing returned':'standing not returned'}</p></div>${icon('arrow')}</div>`).join('')):'');
  }
  function lineage(r){
    if(!r.id)return '';
    return requestView(provenance(r.id),graph=>{
      const nodes=new Map(graph.nodes.map(n=>[n.id,n]));
      let rendered=0;
      const tree=(id,trail=[])=>trail.length>=16||rendered>=512?'':`<ol class="lineage-tree">${graph.edges.filter(e=>e.child===id).map(e=>{if(rendered++>=512)return '';const n=nodes.get(e.parent);return `<li><strong>${link(e.parent,n?.label||label(e.parent))}</strong> ${badge(n?.metadata_trunk?'Metadata trunk':n?.root?'Structural root':'Containing structure')}<p class="small muted">Member ${e.ordinal+1} · position ${e.position} · run ${e.run} · said ${e.said}</p><code>${h(e.parent)}</code>${trail.includes(e.parent)?'':tree(e.parent,[...trail,id])}</li>`}).join('')}</ol>`;
      return `<p class="inset-note">Containment runs from this content through structures, records, files and source trunks. These structural edges are shown separately from assertion semantics in Evidence.</p>${section('Containment ancestry',graph.edges.length?tree(r.id):'<p>No parent edges were returned for this resource in this read.</p>')}<p class="preview-note">${graph.complete?'The bounded native walk exhausted its returned ancestry.':'The native walk reached a bound; additional ancestry may exist.'} ${graph.nodes.length} nodes · ${graph.edges.length} ordered edges · ${graph.levels} levels. Limits: ${graph.limits.nodes} nodes, ${graph.limits.levels} levels, ${graph.limits.holders_per_kind} holders per kind and frontier member. Registry membership is metadata, not the definition of a witness.</p>`;
    });
  }
  function connections(r){const read=currentRead(r);return section('Contains →',members(r.kind==='physicality'?r.parts||[]:read?.physicality?.[0]?.constituents||read?.parts||r.parts||[],r.id))+section('← Containing paths',(read?.occurrences||[]).map(c=>`<div class="return-row"><div><strong>${link(c.entity)}</strong><p>Tier ${c.tier} · ${c.n} ordered positions</p></div>${icon('arrow')}</div>`).join('')||'<p>No containing paths were returned in this read.</p>')+'<p class="preview-note">A containing path is a composition, not a unique contextual occurrence ID. Ordered positions are retained when following its structure. Geometric proximity is not substituted for semantic relations.</p>'}
  function sourcesView(params){return libraryView(params,true)}
  function researchView(){return `<div class="page-top"><div><div class="eyebrow">Keep your working context</div><h1>Research</h1><p>Exact references and notes remain connected to the question that led you there.</p></div><button data-action="export-notes">${icon('export')} Export notes</button></div><div class="two-col"><section class="sheet"><h2>Saved references</h2>${notebook.references.length?notebook.references.map(r=>`<div class="return-row"><div><strong><a href="${h(r.href)}">${h(r.label)}</a></strong><p>${h(r.kind)} · saved ${h(new Date(r.savedAt).toLocaleString())}</p><code>${h(r.id||r.key)}</code></div><button class="subtle" data-action="unpin" data-key="${h(r.key)}" aria-label="Remove ${h(r.label)}">${icon('close')}</button></div>`).join(''):status('Build a research thread','Save references from any resource detail, then return to their original query and view.')}<div class="panel-section"><label for="research-note"><strong>Research notes</strong></label><textarea id="research-note" class="note-input" placeholder="What have you established? What still needs evidence?">${h(notebook.note)}</textarea><button data-action="save-note">Save notes locally</button></div></section><aside class="sheet"><h2>Your references are portable</h2><p class="muted small">Notes and references are stored in this browser. Export a copy to retain them outside this device. A reference records its address, canonical identity and save time.</p><p class="muted small">Account-backed collaborative research and witnessed conversation are not connected in this version.</p></aside></div>`}
  function resultValue(value,depth=0){
    if(value===null||value===undefined)return '<span class="muted">Absent</span>';
    if(typeof value!=='object')return typeof value==='string'&&/^[0-9a-f]{32}$/.test(value)?link(value,value):h(value);
    if(depth>4)return '<span class="muted">Expand the stored response for this nested value.</span>';
    if(Array.isArray(value)){
      if(!value.length)return '<span class="muted">No rows returned</span>';
      if(value.every(item=>item===null||typeof item!=='object'))return value.map(item=>resultValue(item,depth+1)).join(' · ');
      const keys=[...new Set(value.flatMap(item=>item&&typeof item==='object'?Object.keys(item):['value']))];
      return `<div class="table-wrap"><table><thead><tr>${keys.map(key=>`<th scope="col">${h(key.replaceAll('_',' '))}</th>`).join('')}</tr></thead><tbody>${value.map(item=>`<tr>${keys.map(key=>`<td>${resultValue(item?.[key],depth+1)}</td>`).join('')}</tr>`).join('')}</tbody></table></div>`;
    }
    return facts(Object.entries(value).map(([key,item])=>[key.replaceAll('_',' '),resultValue(item,depth+1)]));
  }
  function workbenchView({embedded=false,input=lastQuery}={}){
    const schema=model(),operations=schema.data?.operations||[],operation=operations.find(item=>item.name===selectedOperation)||operations[0];
    if(!operation)return requestView(schema,()=>status('No available operations','The native service returned no permitted operation schemas.'));
    const draftKey=operation.name+':'+(embedded?input:'manual');
    activeOperationDraft=draftKey;
    const draft=operationDrafts[draftKey]||{input},spec=operation.inputSchema;
    const result=operationResult?.scope===draftKey?operationResult:null;
    const fields=Object.entries(spec.properties||{}).filter(([,field])=>field.type==='string');
    return `${embedded?'':'<div class="page-top"><div><h1>Native calculations</h1><p>Identify content, inspect observed continuations and compare trajectories.</p></div></div>'}<div class="two-col"><section class="sheet"><form data-form="operation" data-operation-scope="${h(draftKey)}" class="operation-form"><label for="operation">Operation</label><select id="operation" name="operation">${operations.map(item=>`<option value="${h(item.name)}" ${operation.name===item.name?'selected':''}>${h(item.name)}</option>`).join('')}</select><p class="small muted">${h(operation.description)}</p>${fields.map(([name,field])=>`<label for="operation-${h(name)}">${name==='input'?'Content':h(name.replaceAll('_',' '))}</label><textarea id="operation-${h(name)}" name="${h(name)}" ${spec.required?.includes(name)?'required':''} maxlength="4096">${h(draft[name]||'')}</textarea>`).join('')}<button class="primary" ${operationBusy?'disabled':''}>${operationBusy?'Reading…':'Run native operation'}</button></form></section><section class="sheet operation-result">${result?section(result.error?'Operation unavailable':'Native result',result.error?`<p role="alert">${h(result.error)}</p>`:`${resultValue(result.data)}<details><summary>Stored response</summary><pre class="json-result">${h(JSON.stringify(result.data,null,2))}</pre></details><button data-action="export-operation">Export result</button>`):'<h2>Result</h2><p class="muted">Choose an operation and enter content.</p>'}</section></div>`;
  }
  function exportsView(){const query=lastQuery?requests.get('query:'+lastQuery):null;return `<div class="page-top"><div><div class="eyebrow">Portable, attributable results</div><h1>Exports</h1><p>Keep the actual response, its canonical references and the scope of the read.</p></div></div><div class="two-col"><section class="sheet"><h2>Live research readback</h2>${query?.state==='ready'?`${facts([['Query',h(lastQuery)],['Database',h(query.data.database)],['Read time',h(new Date(query.readAt).toLocaleString())],['Canonical ID',h(query.data.id)]])}<p class="muted small">The JSON export includes the live native response and any ancestry read in this browser context. It is a readback record, not a signed synthesis or model cartridge.</p><button class="primary" data-action="export-read">${icon('export')} Download live readback</button>`:'<p>Run a library lookup to export its live response.</p><a href="#/library">Open the library</a>'}<p class="preview-note">Lookup, evidence, notes and readback downloads have no payment gate.</p></section><aside class="sheet"><h2>Research notebook</h2><p>${number(notebook.references.length)} saved references and your notes from this browser.</p><button data-action="export-notes">Download notebook</button><p><a href="#/research">Review saved research ${icon('arrow')}</a></p></aside></div>`}
  function render(){
    const schema=model();
    const current=route(),pages={home:libraryView,library:libraryView,research:researchView,sources:sourcesView,workbench:workbenchView,exports:exportsView},page=pages[current.page]?current.page:'home';
    const active=document.activeElement,focus=active?.dataset.focus,queryFocus=active?.id==='query',selection=queryFocus?[active.selectionStart,active.selectionEnd]:null;
    const collectionParams=new URLSearchParams(current.params);collectionParams.delete('selected');collectionParams.delete('tab');
    const collectionKey=page+'?'+collectionParams,detailKey=current.params.get('selected')+':'+current.params.get('tab');
    const masterScroll=lastCollection===collectionKey?document.querySelector('.master')?.scrollTop||0:0,detailScroll=lastDetail===detailKey?document.querySelector('.detail')?.scrollTop||0:0;
    const navQuery=lastQuery?'?q='+encodeURIComponent(lastQuery):'';
    document.getElementById('app').innerHTML=`<aside class="sidebar"><a href="${h(worldRoute('library'))}" class="brand" aria-label="Laplace home">${icon('logo')}<span>LAPLACE</span></a><nav class="nav" aria-label="Main navigation">${[['library','Knowledge','search'],['research','Research','research']].map(([key,title,img])=>`<a href="${h(worldRoute(key))}" class="${key===page?'active ':''}" title="${title}" ${key===page?'aria-current="page"':''}>${icon(img)}<span>${title}</span></a>`).join('')}<button class="mobile-more subtle" data-action="menu" aria-label="More navigation">${icon('more')}</button></nav><div class="sidebar-bottom">${badge('Modular Laplace','green')}<p>Knowledge workspace</p></div></aside><div class="shell"><header class="topbar"><span class="location">${h(page==='library'?'Knowledge':page[0].toUpperCase()+page.slice(1))}</span><div class="top-actions"><a class="preview-link" href="${h(worldRoute('library'))}">Knowledge workspace</a><button data-action="signin" ${identity.state==='loading'?'disabled':''}>${icon('user')}<span class="account-label">${h(identity.user?.name||identity.user?.email||(identity.state==='loading'?'Checking session…':'Sign in'))}</span></button></div></header><main class="content" id="main" tabindex="-1">${schema.state==='error'?`<p role="alert">View descriptions unavailable: ${h(schema.error)} <button data-action="retry">Retry descriptions</button></p>`:''}${pages[page](current.params)}</main></div>`;
    document.title=(resources.get(current.params.get('selected'))?.label||(page==='library'?'Knowledge':page[0].toUpperCase()+page.slice(1)))+' · Laplace';
    if(focus)document.querySelector(`[data-focus="${CSS.escape(focus)}"]`)?.focus({preventScroll:true});
    if(queryFocus){const input=document.getElementById('query');input?.focus({preventScroll:true});if(selection)input?.setSelectionRange(...selection)}
    const master=document.querySelector('.master'),detail=document.querySelector('.detail');if(master)master.scrollTop=masterScroll;if(detail)detail.scrollTop=detailScroll;lastCollection=collectionKey;lastDetail=detailKey;
    const heading=document.querySelector('.detail h1');if(heading)heading.tabIndex=0;
    if(lastPage&&lastPage!==page){document.getElementById('main').focus({preventScroll:true});window.scrollTo(0,0)}lastPage=page;
  }
  async function refreshIdentity(){try{const response=await fetch(runtime.identityPath+'/me',{credentials:'same-origin',cache:'no-store',signal:AbortSignal.timeout(10000)});if(!response.ok)throw new Error('The identity service is unavailable.');const data=await response.json();identity.user=data.authenticated?data.user:null;identity.providers=data.providers||[];identity.state='ready'}catch(error){identity.state='error';identity.error=error.message}render()}
  function dialog(title,body){const el=document.getElementById('dialog');el.innerHTML=`<div class="dialog-top"><h2 id="dialog-title">${h(title)}</h2><button class="subtle" data-action="close-dialog" aria-label="Close dialog">${icon('close')}</button></div>${body}`;if(!el.open)el.showModal()}
  function signIn(){if(identity.state==='error'){dialog('Identity service unavailable',`<p>${h(identity.error)}</p><button data-action="retry-identity">Retry connection</button>`);return}if(identity.user){dialog('Your account',`<p>${h(identity.user.name||identity.user.email)}</p><p>Notes in this browser remain local.</p><button data-action="logout">Sign out of this origin</button>`);return}const providers=identity.providers.flatMap(p=>{try{const url=new URL(p.loginUrl,location.origin);if(url.origin!==location.origin||!url.pathname.startsWith(runtime.identityPath+'/login/'))return [];url.searchParams.set('returnUrl',location.pathname+location.search+location.hash);return [`<a class="button" data-provider="${h(p.id)}" href="${h(url.pathname+url.search)}">${icon(p.id)}Continue with ${h(p.displayName)}</a>`]}catch{return []}});dialog('Sign in to Laplace',`<p>You will return to this research address after signing in.</p><div class="providers">${providers.join('')||'<p>No providers are configured.</p>'}</div><p class="preview-note">The same-origin identity service handles Google and Microsoft authorization and the browser session.</p>`)}
  function notify(message){const el=document.getElementById('notice');el.textContent=message;el.classList.add('visible');clearTimeout(noticeTimer);noticeTimer=setTimeout(()=>el.classList.remove('visible'),4000)}
  function download(name,data){const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)+'\n'],{type:'application/json'})),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)}
  document.addEventListener('submit',async event=>{
    if(event.target.matches('[data-form="source-filter"]')){event.preventDefault();location.hash=library({source_filter:String(new FormData(event.target).get('filter')||''),source_page:null})}
    if(event.target.matches('[data-form="search"]')){event.preventDefault();const q=String(new FormData(event.target).get('q')||'');location.hash=library({q,selected:null,tab:null,page:null,after:null})}
    if(event.target.matches('[data-form="operation"]')){event.preventDefault();const data=new FormData(event.target),scope=event.target.dataset.operationScope,operation=data.get('operation'),input=data.get('input'),other=data.get('other');operationBusy=true;operationResult=null;render();try{const response=await fetch(base+'/api/operations/'+operation,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({input,other}),signal:AbortSignal.timeout(60000)});const result=await response.json();if(!response.ok)throw new Error(result.detail||'The native operation could not complete.');operationResult={scope,operation,input,other,data:result,readAt:new Date().toISOString()}}catch(error){operationResult={scope,error:error.message}}operationBusy=false;render()}
  });
  document.addEventListener('change',e=>{if(e.target.id==='operation'){selectedOperation=e.target.value;operationResult=null;render()}if(e.target.matches('[data-filter]')){const key=e.target.dataset.filter,values=route().params.getAll(key).filter(value=>value!=='all'),next=e.target.checked?[...new Set([...values,e.target.value])]:values.filter(value=>value!==e.target.value);location.hash=library({[key]:next,[route().page==='sources'?'source_page':'page']:null})}});
  document.addEventListener('input',e=>{if(e.target.closest('[data-form=operation]')){operationDrafts[activeOperationDraft]??={};operationDrafts[activeOperationDraft][e.target.name]=e.target.value}if(e.target.id==='query')queryDrafts.set(location.hash,e.target.value);if(e.target.id==='research-note'){notebook.note=e.target.value;persist()}});
  document.addEventListener('click',async event=>{
    if(event.target.closest('.skip')){event.preventDefault();document.getElementById('main').focus();return}
    if(event.target.closest('dialog a'))document.getElementById('dialog').close();
    const button=event.target.closest('[data-action]');if(!button)return;
    const action=button.dataset.action;
    if(action==='signin')signIn();
    if(action==='close-dialog')document.getElementById('dialog').close();
    if(action==='retry-identity'){document.getElementById('dialog').close();identity.state='loading';render();await refreshIdentity()}
    if(action==='logout'){try{const response=await fetch(runtime.identityPath+'/logout',{method:'POST',credentials:'same-origin'});if(!response.ok)throw new Error('Sign out failed.');document.getElementById('dialog').close();await refreshIdentity()}catch(error){notify(error.message)}}
    if(action==='menu')dialog('Navigate Laplace',`<nav class="link-list" aria-label="All navigation">${['library','research'].map(key=>`<a class="button" href="${h(worldRoute(key))}">${key[0].toUpperCase()+key.slice(1)} ${icon('arrow')}</a>`).join('')}</nav>`);
    if(action==='refresh'||action==='retry'){for(const [key,value] of requests)if(action==='refresh'||value.state==='error')requests.delete(key);render()}
    if(action==='pin'){const r=resources.get(button.dataset.key),index=notebook.references.findIndex(x=>x.key===r?.key);if(index>=0)notebook.references.splice(index,1);else if(r)notebook.references.push({key:r.key,id:r.id,kind:r.kind,label:r.label,href:location.hash,savedAt:new Date().toISOString()});persist();render();notify(index>=0?'Reference removed.':'Reference saved in this browser.')}
    if(action==='unpin'){notebook.references=notebook.references.filter(r=>r.key!==button.dataset.key);persist();render()}
    if(action==='save-note'){persist();notify('Notes saved in this browser.')}
    if(action==='copy'){try{await navigator.clipboard.writeText(button.dataset.text);notify('Canonical ID copied.')}catch{dialog('Canonical ID',`<p class="mono">${h(button.dataset.text)}</p>`)}}
    if(action==='export-notes')download('laplace-research.json',{schema:'laplace-local-research/v1',exportedAt:new Date().toISOString(),...notebook});
    if(action==='export-current'){const id=button.dataset.key,read=readbacks.get(id),q=route().params.get('q'),entry=q?requests.get('query:'+q):null;if(read)download('laplace-readback-'+short(id)+'.json',{schema:'laplace-native-readback/v1',query:q||null,query_root:entry?.data?.id||null,selection:{id,href:location.hash,projection:route().params.get('selected'),discovery_matches:entry?.rows?.find(row=>row.key===route().params.get('selected'))?.matches||[]},read_at:(requests.get('resource:'+id)?.readAt||entry?.readAt),release:runtime.build,response:read,ancestry:graphs.get(id)||null})}
    if(action==='export-read'){const entry=requests.get('query:'+lastQuery);if(entry?.state==='ready')download('laplace-readback.json',{schema:'laplace-native-readback/v1',query:lastQuery,readAt:entry.readAt,release:runtime.build,response:entry.data,ancestry:graphs.get(entry.data.id)||null})}
    if(action==='export-operation'&&operationResult)download('laplace-operation.json',operationResult);
  });
  const scrollPositions=new Map();let lastHash=location.hash;
  if('scrollRestoration' in history)history.scrollRestoration='manual';
  window.addEventListener('hashchange',()=>{scrollPositions.set(lastHash,scrollY);render();window.scrollTo(0,scrollPositions.get(location.hash)||0);lastHash=location.hash});
  render();refreshIdentity();
})();
