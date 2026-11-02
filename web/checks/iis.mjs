import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {mkdir,writeFile,readFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
const dir=path.dirname(fileURLToPath(import.meta.url));
const require=createRequire(process.env.LAPLACE_BROWSER_CHECK_PACKAGE||path.resolve(dir,'../../../Laplace/web/package.json'));
const {chromium}=require('@playwright/test');
const {default:AxeBuilder}=await import('file:///'+require.resolve('@axe-core/playwright').replaceAll('\\','/'));
const base=process.env.LAPLACE_MODULAR_WEB_URL||'https://dev.hartonomous.com/modular/';
const output=process.env.LAPLACE_MODULAR_WEB_EVIDENCE||'E:/Data/Laplace/work/operations/modular-web/browser';
await mkdir(output,{recursive:true});
const browser=await chromium.launch({headless:true});
const context=await browser.newContext({viewport:{width:1440,height:1000},acceptDownloads:true});
const page=await context.newPage();page.setDefaultTimeout(60000);
const errors=[],checks=[],axe=[],reads=[],challenges=[];
let failure=null,health=null,discovery=null;
page.on('pageerror',error=>errors.push(error.message));
page.on('response',response=>{if(response.url().startsWith(base+'api/'))reads.push({url:response.url(),status:response.status()})});
const mark=name=>{checks.push({name,passed:true});console.log('PASS '+name)};
const snapshot=async name=>page.screenshot({path:path.join(output,name+'.png'),fullPage:true});
const scan=async name=>{const result=await new AxeBuilder({page}).withTags(['wcag2a','wcag2aa','wcag21aa']).analyze();axe.push({name,violations:result.violations.map(x=>({id:x.id,impact:x.impact,targets:x.nodes.map(n=>n.target)}))})};
const waitDetail=async()=>page.waitForFunction(()=>{const selected=new URLSearchParams(location.hash.split('?')[1]).get('selected'),panel=document.getElementById('detail-panel');return panel?.dataset.resource===selected&&panel.dataset.state==='ready'});
try{
  await page.goto(base);
  await page.getByRole('heading',{name:'Knowledge',exact:true}).waitFor();
  await page.getByRole('heading',{name:'Search knowledge',exact:true}).waitFor();
  assert.equal(await page.locator('.nav a[href="#/sources"]').count(),0);
  await snapshot('live-entry');await scan('entry');mark('Knowledge opens directly with a unified search workspace and no source-first navigation');
  const model=await (await page.request.get(base+'api/schema')).json();
  assert.equal(model.schema,'laplace-view-schema/v1');assert.ok(model.banks.length>0);
  for(const bank of model.banks){assert.equal(bank.values.length,bank.slots);assert.deepEqual(bank.values.map(value=>value.slot),Array.from({length:bank.slots},(_,slot)=>slot));assert.ok(bank.values.every(value=>/^[a-f0-9]{32}$/.test(value.id)&&value.label));}
  mark('Installed schema returns canonical mask-bank values, identities and slot order');
  assert.deepEqual(model.operations.map(x=>x.name).sort(),['embeddings','forward','shape']);
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('dog');
  await page.getByRole('button',{name:'Search',exact:true}).click();await page.locator('.master .result').first().waitFor();
  await page.locator('.master .result').first().click();await waitDetail();
  await page.locator('.tabs').getByRole('link',{name:'Evidence',exact:true}).click();
  await page.getByRole('link',{name:'Browse source catalogue',exact:true}).click();
  await page.locator('.master .result').first().waitFor();
  discovery=await (await page.request.get(base+'api/library')).json();
  const source=discovery.items.find(x=>x.kind==='source');assert.ok(source,'Source discovery must be populated');
  assert.equal(discovery.database,'laplace');
  await page.locator('[data-filter=source_kind][value=source]').click();
  await page.locator('.master .result').filter({hasText:source.title}).first().click();await waitDetail();
  assert.equal(await page.locator('.detail .file-card').count(),source.children.length);
  await snapshot('live-sources');await scan('sources');mark('Evidence inspection reaches the actual source catalogue and its ordered files');

  await page.locator('.detail').getByRole('heading',{name:source.title,exact:true}).waitFor();
  assert.equal(await page.locator('.detail .file-card').count(),source.children.length);
  const sourceAddress=page.url();await page.reload();await waitDetail();
  assert.equal(page.url(),sourceAddress);await page.locator('.detail .file-card').first().waitFor();
  await page.locator('.detail .file-card').first().click();await waitDetail();
  assert.ok((await page.locator('.detail').innerText()).includes('File trunk'));
  const fileId=new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('selected').slice(7);
  const contentResponse=page.waitForResponse(base+'api/files/'+fileId);
  await page.locator('.tabs').getByRole('link',{name:'Content',exact:true}).click();
  const contentRead=await (await contentResponse).json(),recordedFile=contentRead.files.find(file=>file.file===fileId);
  assert.ok(recordedFile);assert.equal(typeof recordedFile.text,'string');assert.ok(recordedFile.text.length>0);
  await page.getByRole('region',{name:'Recorded file lines',exact:true}).waitFor();
  await snapshot('live-file-content');mark('Recorded file content comes from native file_outline and is readable beside the originating collection');
  await page.locator('.tabs').getByRole('link',{name:'Structure',exact:true}).click();
  await page.locator('tbody').waitFor();assert.ok((await page.locator('#detail-panel').innerText()).includes('File metadata'));
  await page.goBack();await page.goBack();await page.goBack();await waitDetail();
  await snapshot('live-source-detail');mark('Source to file detail, direct reload and browser Back work through ordinary UI links');

  await page.getByRole('link',{name:'Knowledge',exact:true}).click();
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('q'),'dog');

  assert.equal(await page.locator('[data-filter=kind]:checked').count(),0);
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('sabotage');
  await page.getByRole('button',{name:'Search',exact:true}).click();
  await page.locator('.result').filter({hasText:'sabotage.v'}).waitFor();
  assert.equal(await page.locator('[data-filter=kind]:checked').count(),0);
  assert.ok(await page.locator('.master .result').count()>=4);
  await page.locator('.result').first().click();await waitDetail();
  await page.getByText('Storage and read scope',{exact:true}).click();await page.locator('[data-tier-mismatch]').waitFor();
  assert.match(await page.locator('[data-tier-mismatch]').innerText(),/Stored tier 1.*text modality tier 2/);
  await page.locator('.tabs').getByRole('link',{name:'Structure',exact:true}).click();
  await page.locator('tbody tr').first().waitFor();assert.equal(await page.locator('tbody tr').count(),8);
  const repeated=await page.locator('tbody tr').evaluateAll(rows=>[rows[1].cells[2].textContent,rows[5].cells[2].textContent]);assert.equal(repeated[0],repeated[1]);
  const readAddress=page.url();await page.reload();await page.locator('tbody tr').first().waitFor();assert.equal(page.url(),readAddress);
  await page.locator('[data-filter=kind][value=physicality]').click();await page.waitForFunction(()=>document.querySelectorAll('.master .result').length===1);
  await page.locator('[data-filter=kind][value=physicality]').click();
  await snapshot('live-sabotage-structure');await scan('library');mark('User-entered search returns mixed live resources, preserves repeated positions and keeps filters optional');

  await page.locator('.tabs').getByRole('link',{name:'Lineage',exact:true}).click();
  await page.locator('.lineage-tree').first().waitFor();assert.ok((await page.locator('#detail-panel').innerText()).includes('sabotage.v'));
  await page.getByRole('button',{name:'Save reference to research'}).click();
  await page.getByRole('link',{name:'Research',exact:true}).click();
  await page.getByLabel('Research notes').fill('Follow the exact source and content references.');
  await page.getByRole('button',{name:'Save notes locally'}).click();await page.reload();
  assert.equal(await page.getByLabel('Research notes').inputValue(),'Follow the exact source and content references.');
  assert.ok(await page.locator('main a[href*=selected]').count()>0);mark('Research saves real navigable references and retains local notes across reload');

  await page.getByRole('link',{name:'Knowledge',exact:true}).click();
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('q'),'sabotage');
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('dog');
  await page.getByRole('button',{name:'Search',exact:true}).click();await page.locator('.result').first().waitFor();
  await page.locator('.result').first().click();await waitDetail();
  await page.locator('.tabs').getByRole('link',{name:'Evidence',exact:true}).click();
  await page.locator('.evidence-row').first().waitFor();assert.ok(await page.locator('.evidence-row').count()>0);
  await page.locator('.tabs').getByRole('link',{name:'Lineage',exact:true}).click();
  await page.locator('.lineage-tree').first().waitFor();await page.locator('#detail-panel').getByRole('link',{name:source.title,exact:true}).first().waitFor();
  await snapshot('live-claim-lineage');mark('Visible evidence and ancestry connect claims through file trunks to the actual named source');

  const readDownload=page.waitForEvent('download');await page.getByRole('button',{name:'Download readback'}).click();
  const download=await readDownload;const exported=JSON.parse(await readFile(await download.path(),'utf8'));
  assert.equal(exported.schema,'laplace-native-readback/v1');assert.equal(exported.query,'dog');assert.equal(exported.response.database,'laplace');assert.ok(exported.ancestry.nodes.some(n=>n.id===source.id));assert.ok(exported.read_at);mark('Selected-content download preserves the actual response, read time and traced ancestry');

  await page.getByRole('link',{name:'Knowledge',exact:true}).click();
  const cachedResponse=page.waitForResponse(base+'api/lookup?q=NOUN');
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('NOUN');
  await page.getByRole('button',{name:'Search',exact:true}).click();
  const cached=await (await cachedResponse).json();assert.equal(cached.cache_tier,2);assert.equal(cached.canonical_tier,2);
  await page.locator('.result').first().click();await waitDetail();
  await page.locator('.tabs').getByRole('link',{name:'Structure',exact:true}).click();
  await page.locator('tbody tr').first().waitFor();assert.equal(await page.locator('tbody tr').count(),4);
  mark('Ordinary library navigation uses the regenerated Highway while retaining stored metadata');

  await page.locator('.tabs').getByRole('link',{name:'Calculate',exact:true}).click();
  assert.equal(await page.getByLabel('Content',{exact:true}).inputValue(),'NOUN');
  await page.getByLabel('Content',{exact:true}).fill('sabotage');
  await page.getByLabel('Operation',{exact:true}).selectOption('shape');assert.equal(await page.locator('textarea').count(),2);
  await page.getByLabel('Operation',{exact:true}).selectOption('embeddings');assert.equal(await page.locator('textarea').count(),1);
  await page.getByRole('button',{name:'Run native operation',exact:true}).click();await page.getByRole('heading',{name:'Native result',exact:true}).waitFor();assert.ok((await page.locator('.operation-result').innerText()).includes('f040f1c29f98d0f5d1bd7f265ac2e1de'));mark('Calculation uses selected query context and native schemas for fields and canonical results');

  await page.getByRole('button',{name:'Sign in',exact:true}).click();const dialog=page.getByRole('dialog');await dialog.waitFor();
  for(const provider of ['google','microsoft']){
    const href=await dialog.locator(`[data-provider="${provider}"]`).getAttribute('href'),url=new URL(href,base);
    assert.equal(url.origin,new URL(base).origin);assert.match(url.searchParams.get('returnUrl'),/^\/modular\//);
    const response=await page.request.get(url.href,{maxRedirects:0});assert.equal(response.status(),302);
    const authorization=new URL(response.headers().location);assert.equal(authorization.searchParams.get('response_type'),'code');assert.equal(authorization.searchParams.get('code_challenge_method'),'S256');assert.ok(authorization.searchParams.get('state'));assert.ok(authorization.searchParams.get('nonce'));
    const callback=new URL(authorization.searchParams.get('redirect_uri'));assert.equal(callback.origin,new URL(base).origin);assert.equal(callback.pathname,'/signin-'+provider);
    challenges.push({provider,authorizationHost:authorization.hostname,callback:callback.origin+callback.pathname,pkce:'S256'});
  }
  await scan('identity');await page.keyboard.press('Escape');mark('Visible provider links produce real OIDC authorization-code and PKCE challenges');

  await page.setViewportSize({width:390,height:844});await page.goto(base);await page.getByRole('heading',{name:'Knowledge',exact:true}).waitFor();
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('dog');await page.getByRole('button',{name:'Search',exact:true}).click();await page.locator('.master .result').first().waitFor();
  await snapshot('live-mobile-search');await scan('mobile-search');
  await page.locator('.master .result').first().click();await waitDetail();
  const heading=await page.locator('.detail h1').boundingBox();assert.ok(heading.y<500);assert.ok(await page.getByRole('link',{name:'Back to results'}).isVisible());
  assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));await snapshot('live-mobile-detail');await scan('mobile-detail');mark('Mobile search and selected content remain visible without horizontal overflow');

  // Fault injection checks the failure UI, without substituting invented content.
  await page.route(base+'api/lookup?*',route=>route.abort('failed'));
  await page.goto(base+'#/library?q=connection-failure-check');await page.getByRole('alert').first().waitFor();
  assert.ok((await page.getByRole('alert').first().innerText()).includes('content, assertions and containment'));
  await page.unroute(base+'api/lookup?*');await page.getByRole('textbox',{name:'Search content or an identifier'}).fill('sabotage');await page.getByRole('button',{name:'Search',exact:true}).click();await page.locator('.result').first().waitFor();mark('Failed reads show a recoverable error and subsequent real searches succeed');

  await page.setViewportSize({width:1440,height:1000});
  const observation='dog cat QzxwvuUnobservedScope703 dog';
  const phraseResponse=page.waitForResponse(response=>response.url().startsWith(base+'api/lookup?')&&new URL(response.url()).searchParams.get('q')===observation);
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill(observation);
  await page.getByRole('button',{name:'Search',exact:true}).click();
  const phrase=await (await phraseResponse).json();
  assert.equal(phrase.discovery.schema,'laplace-discovery/v1');assert.equal(phrase.recorded,false);assert.equal(phrase.storage,'computed');assert.equal(phrase.discovery.root_present,false);
  assert.ok(!phrase.discovery.matches.some(match=>match.id===phrase.id));
  const dog=await (await page.request.get(base+'api/lookup?q=dog')).json();
  const memberPositions=phrase.discovery.matches.filter(match=>match.id===dog.id&&match.match==='member');
  assert.equal(memberPositions.length,2);assert.notEqual(memberPositions[0].ordinal,memberPositions[1].ordinal);
  await page.getByText('Laplace calculated an identity for this input;', {exact:false}).waitFor();
  const dogResult=page.locator('.master .result').filter({hasText:'2 query member positions'}).first();
  await dogResult.waitFor();assert.equal(await page.locator(`.master .result[href*="${phrase.id}"]`).count(),0);
  await dogResult.click();await waitDetail();
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('q'),observation);
  await snapshot('live-known-members-unrecorded-root');
  await scan('known-members-unrecorded-root');
  mark('An actual unrecorded phrase excludes its calculated root while retaining known repeated members and originating query context');
  const boundedObservation='Z'.repeat(100);
  const boundedResponse=page.waitForResponse(response=>response.url().startsWith(base+'api/lookup?')&&new URL(response.url()).searchParams.get('q')===boundedObservation);
  await page.getByRole('textbox',{name:'Search content or an identifier'}).fill(boundedObservation);
  await page.getByRole('button',{name:'Search',exact:true}).click();
  const bounded=await (await boundedResponse).json();
  assert.equal(bounded.parts_total,100);assert.equal(bounded.constituents.length,64);assert.equal(bounded.discovery.members_complete,false);
  await page.getByText('This read includes 64 of 100 query constituents.',{exact:false}).waitFor();
  mark('Installed Native and browser disclose the returned member prefix instead of implying complete discovery');

  health=await (await page.request.get(base+'health')).json();assert.equal(health.data,'native-live');assert.match(health.release,/^[a-f0-9]{64}$/);
  assert.equal((await page.request.get(new URL('/health',base).href)).status(),200);
  assert.deepEqual(errors,[]);assert.deepEqual(axe.flatMap(x=>x.violations),[]);mark('No browser exceptions or WCAG A/AA violations; modular and mono IIS health are available');
}catch(error){failure=String(error.stack||error);await snapshot('failure').catch(()=>{});console.error(failure);process.exitCode=1}
finally{
  await writeFile(path.join(output,'receipt.json'),JSON.stringify({status:failure?'failed':'passed',utc:new Date().toISOString(),origin:base,release:health?.release,checks,axe,errors,reads,challenges,discovery:discovery?{database:discovery.database,trunks:discovery.items.length,sourceTrunks:discovery.items.filter(x=>x.kind==='source').length,fileTrunks:discovery.items.filter(x=>x.kind==='file').length}:null,failure},null,2)+'\n');
  await browser.close();
}
