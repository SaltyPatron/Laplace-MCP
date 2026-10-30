import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {mkdir,writeFile} from 'node:fs/promises';
import assert from 'node:assert/strict';
const dir=path.dirname(fileURLToPath(import.meta.url));
// Browser tools are development-only. The served application has no dependency on mono.
const require=createRequire(process.env.LAPLACE_BROWSER_CHECK_PACKAGE||path.resolve(dir,'../../../Laplace/web/package.json'));
const {chromium}=require('@playwright/test');
const {default:AxeBuilder}=await import('file:///'+require.resolve('@axe-core/playwright').replaceAll('\\','/'));
const base=process.env.LAPLACE_MODULAR_WEB_URL||'https://dev.hartonomous.com/modular/';
const output=process.env.LAPLACE_MODULAR_WEB_EVIDENCE||'E:/Data/Laplace/work/operations/modular-web/browser';
await mkdir(output,{recursive:true});
const browser=await chromium.launch({headless:true});
const context=await browser.newContext({viewport:{width:1440,height:1000},acceptDownloads:true});
const page=await context.newPage();
const errors=[],failures=[],checks=[],axeResults=[];
page.on('pageerror',e=>errors.push(String(e)));
page.on('response',r=>{if(r.status()>=400)failures.push({url:r.url(),status:r.status()})});
const mark=name=>checks.push({name,passed:true});
const settle=()=>page.evaluate(()=>new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r))));
const snapshot=async name=>{await settle();await page.screenshot({path:path.join(output,name+'.png'),fullPage:true});};
const scan=async name=>{const result=await new AxeBuilder({page}).withTags(['wcag2a','wcag2aa','wcag21aa']).analyze();axeResults.push({name,violations:result.violations.map(x=>({id:x.id,impact:x.impact,nodes:x.nodes.map(n=>n.target)}))});};
try{
 const healthResponse=await page.request.get(base+'health');assert.equal(healthResponse.status(),200);const health=await healthResponse.json();assert.equal(health.service,'laplace-modular-web');assert.match(health.release,/^[a-f0-9]{64}$/);mark('HTTPS IIS health identifies the exact modular release');
 const mono=await page.request.get(new URL('/health',base).href);assert.equal(mono.status(),200);mark('Mono remains available on the root origin');
 await page.goto(base);await page.locator('button[data-action=signin]:not([disabled])').waitFor();assert.equal(await page.evaluate(()=>window.LAPLACE_RUNTIME.identityMode),'same-origin');assert.equal(await page.evaluate(()=>window.LAPLACE_RUNTIME.build),health.release);await snapshot('iis-home');await scan('iis-home');mark('IIS serves the modular browser with matching runtime metadata');
 await page.getByRole('button',{name:'Sign in',exact:true}).click();const dialog=page.getByRole('dialog');await dialog.waitFor();
 const challenges=[];
 for(const provider of ['google','microsoft']){
   const link=dialog.locator(`[data-provider="${provider}"]`);await link.waitFor();const href=await link.getAttribute('href');const url=new URL(href,base);assert.equal(url.origin,new URL(base).origin);assert.match(url.searchParams.get('returnUrl'),/^\/modular\//);
   // Follow only the configured server challenge. Do not log state, nonce or cookies.
   const response=await page.request.get(url.href,{maxRedirects:0});assert.equal(response.status(),302);const authorization=new URL(response.headers().location);
   assert.equal(authorization.searchParams.get('response_type'),'code');assert.ok(authorization.searchParams.get('state'));assert.ok(authorization.searchParams.get('nonce'));assert.equal(authorization.searchParams.get('code_challenge_method'),'S256');assert.ok(authorization.searchParams.get('code_challenge'));
   const callback=new URL(authorization.searchParams.get('redirect_uri'));assert.equal(callback.origin,new URL(base).origin);assert.equal(callback.pathname,'/signin-'+provider);
   if(provider==='google')assert.equal(authorization.hostname,'accounts.google.com');else assert.equal(authorization.hostname,'login.microsoftonline.com');
   challenges.push({provider,status:response.status(),authorizationHost:authorization.hostname,callback:callback.origin+callback.pathname,pkce:'S256',returnToModular:true});
 }
 await snapshot('iis-signin');await scan('iis-signin');await page.keyboard.press('Escape');mark('Google and Microsoft produce real OIDC code/PKCE challenges and preserve the modular return address');
 await page.getByRole('link',{name:'Open the captured collection'}).click();await page.getByRole('heading',{name:'dog',exact:true}).waitFor();await snapshot('iis-library');await scan('iis-library');mark('Collection and canonical detail render from the IIS application');
 await page.getByRole('link',{name:'Evidence',exact:true}).click();await page.getByRole('heading',{name:'Claims involving this content'}).waitFor();await page.reload();await page.getByRole('heading',{name:'Claims involving this content'}).waitFor();assert.equal(await page.locator('input[name=q]').inputValue(),'dog');mark('Evidence selection and query survive IIS reload');
 await page.getByRole('link',{name:'Claim 01',exact:true}).first().click();await page.getByRole('heading',{name:'Claim 01',exact:true}).waitFor();await page.goBack();await page.getByRole('heading',{name:'Claims involving this content'}).waitFor();mark('Resource navigation and Back retain the research address');
 await page.getByRole('link',{name:'Structure',exact:true}).click();await page.getByRole('heading',{name:'Ordered constituents'}).waitFor();assert.deepEqual(await page.locator('#detail-panel tbody td:first-child').allTextContents(),['1','2','3']);mark('Exact constituent order remains navigable');
 await page.getByRole('button',{name:'Save reference to research',exact:true}).click();await page.getByRole('link',{name:'Research',exact:true}).first().click();await page.getByLabel('Research notes',{exact:true}).fill('IIS acceptance: canonical content, source assertions and standing stay distinct.');await page.reload();assert.match(await page.getByLabel('Research notes',{exact:true}).inputValue(),/^IIS acceptance:/);mark('Local research state survives navigation and reload');
 await page.goto(base+'#/exports');const download=page.waitForEvent('download');await page.getByRole('button',{name:'Download captured data'}).click();assert.equal((await download).suggestedFilename(),'laplace-dog-captured-readback.json');mark('IIS UI produces the explicitly labelled snapshot download');
 await page.setViewportSize({width:390,height:844});await page.goto(base+'#/library?q=dog&selected=entity%3A739118a25a01c0155d0689e390715dda');await page.getByRole('heading',{name:'dog',exact:true}).waitFor();assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);const composition=await page.getByRole('heading',{name:'Composition',exact:true}).boundingBox();assert.ok(composition.y<844);await snapshot('iis-mobile-entity');await scan('iis-mobile-entity');mark('Mobile IIS detail shows useful structure in the first viewport without horizontal overflow');
 await page.getByRole('link',{name:'Back to results',exact:true}).click();await page.locator('.master').waitFor({state:'visible'});await page.getByRole('button',{name:'More navigation'}).click();await page.getByRole('dialog').getByRole('link',{name:'Workbench',exact:true}).click();await page.getByRole('heading',{name:'Workbench',exact:true}).waitFor();mark('Compact mobile navigation preserves access to all areas');
 assert.deepEqual(errors,[]);assert.deepEqual(failures,[]);mark('No browser exceptions or failed same-origin resources');
 assert.equal(axeResults.flatMap(x=>x.violations).length,0);mark('Automated accessibility scans report no violations on inspected IIS views');
 const result={status:'passed',testedAt:new Date().toISOString(),url:base,release:health.release,checks,challenges,accessibility:axeResults,errors,failures,limitations:['Resource data is a captured modular readback, not live native resource endpoints.','OAuth provider challenges verified; human consent, callback completion and an authenticated user session were not exercised.','Research persists locally; no Stripe transaction, ingestion or model synthesis executed.']};
 await writeFile(path.join(output,'receipt.json'),JSON.stringify(result,null,2));console.log(JSON.stringify({status:result.status,url:base,release:health.release,checks:checks.length,axeViews:axeResults.length,challenges}));
}finally{await context.close();await browser.close();}
