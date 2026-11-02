// Isolated browser fixtures for navigation and selection. These are not live reads.
import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
const directory = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(process.env.LAPLACE_BROWSER_CHECK_PACKAGE || path.resolve(directory, '../../../Laplace/web/package.json'));
const { chromium, expect } = require('@playwright/test');
const AxeBuilder = require('@axe-core/playwright').default;
const root = path.resolve(directory, '../wwwroot');
const output = path.resolve(process.env.LAPLACE_LIBRARY_UI_EVIDENCE || path.resolve(directory, '../../../artifacts/library-ui'));
await mkdir(output, { recursive: true });
const id = value => value.toString(16).padStart(32, '0');
const kinds = ['entity', 'physicality', 'claim', 'container', 'attestation', 'source', 'file'];
const schema = { views: Object.fromEntries(kinds.map(kind => [kind, { name: kind, plural: kind + ' records', description: 'Fixture ' + kind }])), vertex_roles: [], operations: [] };
const items = [
  { id: id(1), kind: 'source', tier: 4, title: 'Fixture source', metadata: id(80), content: id(81), children: [id(2), id(3)] },
  ...Array.from({ length: 14 }, (_, index) => ({ id: id(index + 2), kind: 'file', tier: 3, title: `fixture/file-${index + 1}.txt`, children: [] })),
];
const read = (value, display) => ({ id: value, tier: display?.tier ?? 2, recorded: true, storage: 'database', stored_tier: 2, computed_tier: null, physicality: value === id(52) ? [{ tier: 4, n: 3, constituents: [id(50), id(51), id(50)] }] : [], parts: [], labels: [], attestations: [], claims: [], occurrences: [], claims_read: 0, occurrences_read: 0, fan: 24, ...(display ? { display: { trunk_kind: display.kind, title: display.title, metadata: id(80), content: id(81) } } : {}) });
const lookup = { ...read(id(50)), labels: [{ id: id(50), text: 'Fixture entity' }, { id: id(51), text: 'Fixture claim' }, { id: id(52), text: 'Fixture container' }], physicality: [{ tier: 2, n: 0, constituents: [] }], claims: [{ entity: id(51), tier: 3, n: 0, constituents: [], stood: false }], occurrences: [{ entity: id(52), tier: 4, n: 0, constituents: [] }] };
const mixed = { ...read(id(70)), recorded: false, storage: 'computed', stored_tier: null, computed_tier: 3,
  labels: [{ id: id(71), text: 'Known Sodium' }, { id: id(72), text: 'Known Chloride' }, { id: id(52), text: 'Known containing composition' }],
  constituents: [71, 72, 71].map(value => ({ id: id(value), tier: 2, recorded: true, storage: 'database', stored_tier: 2, claims: [], occurrences: value === 71 ? [{ entity: id(52), tier: 4, n: 3, constituents: [id(71), id(72), id(71)] }] : [] })),
  discovery: { schema: 'laplace-discovery/v1', root_present: false, members_complete: true, scope: 'returned-read', matches: [71, 72, 71].map((value, ordinal) => ({ id: id(value), tier: 2, recorded: true, storage: 'database', stored_tier: 2, match: 'member', via: id(70), ordinal })) } };
const cacheOnly = { ...read(id(80)), recorded: false, storage: 'highway-cache', stored_tier: null, labels: [{ id: id(80), text: 'NOUN' }],
  discovery: { schema: 'laplace-discovery/v1', root_present: true, members_complete: true, scope: 'returned-read', matches: [{ id: id(80), tier: 2, recorded: false, storage: 'highway-cache', match: 'exact' }] } };
const server = createServer(async (req, res) => {
  if (req.url === '/runtime.js') { res.setHeader('Content-Type', 'text/javascript'); res.end('window.LAPLACE_RUNTIME={basePath:"",identityPath:"/v1/auth"};'); return; }
  const file = ({ '/': 'index.html', '/app.js': 'app.js', '/world-context.js': 'world-context.js', '/read-model.js': 'read-model.js', '/styles.css': 'styles.css' })[req.url];
  if (!file) { res.writeHead(404); res.end(); return; }
  res.setHeader('Content-Type', file.endsWith('.css') ? 'text/css' : file.endsWith('.js') ? 'text/javascript' : 'text/html');
  res.end(await readFile(path.join(root, file)));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
let passed = false;
const errors = [], checks = [], accessibility = [];
const mark = name => { checks.push(name); console.log('PASS ' + name); };
try {
  browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
  const page = await context.newPage();
  page.on('pageerror', error => errors.push(error.message));
  let failLookup = false, libraryReads = 0;
  await page.route('**/v1/auth/me', route => route.fulfill({ json: { authenticated: false, providers: [] } }));
  await page.route('**/api/**', route => {
    const url = new URL(route.request().url()), endpoint = url.pathname.slice(5);
    if (endpoint === 'schema') return route.fulfill({ json: schema });
    if (endpoint === 'library') { libraryReads++; return route.fulfill({ json: { items, database: 'fixture', has_more: !url.searchParams.has('after'), next: id(15) } }); }
    if (endpoint === 'continuations') return route.fulfill({ json: { forward: ['computed-only', 'mixed-phrase', 'cache-only'].includes(url.searchParams.get('q')) ? [] : [{ next: id(51), times: 3, i: 0, j: 1 }] } });
    if (endpoint === 'lookup') return route.fulfill(failLookup ? { status: 503, json: { detail: 'Fixture reader unavailable' } } : { json: url.searchParams.get('q') === 'computed-only'
      ? { ...read(id(70)), recorded: false, storage: 'computed', stored_tier: null, computed_tier: 2 }
      : url.searchParams.get('q') === 'mixed-phrase' ? mixed : url.searchParams.get('q') === 'cache-only' ? cacheOnly : lookup });
    if (endpoint.startsWith('files/')) return route.fulfill({ json: { database: 'fixture', files: [{ file: endpoint.slice(6), path: 'fixture/file-1.txt', bytes: 52, text: 'class Fixture {\n  render() { return "<safe>"; }\n}', declarations: [{ name: 'Fixture', kind: 'class', line: 1 }, { name: 'render', kind: 'method', line: 2 }] }] } });
    if (endpoint.startsWith('resources/')) { const selected = endpoint.slice(10); return route.fulfill({ json: selected === id(80) ? cacheOnly : selected === id(70) ? { ...read(id(70)), recorded: false, storage: 'computed' } : read(selected, items.find(item => item.id === selected)) }); }
    return route.fulfill({ status: 503, json: { detail: 'Optional fixture read unavailable' } });
  });
  const base = `http://127.0.0.1:${server.address().port}/`;
  await page.goto(base);
  await expect(page.getByRole('heading', { name: 'Knowledge', exact: true })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Search knowledge', exact: true })).toBeVisible();
  assert.equal(libraryReads, 0, 'knowledge entry must not load source catalogues');
  mark('Knowledge entry starts with search and does not substitute source discovery');
  await page.goto(base + '#/sources');
  await expect(page.locator('.master .result')).toHaveCount(12);
  await page.getByRole('link', { name: 'Next page', exact: true }).click();
  await expect(page.locator('.master .result')).toHaveCount(3);
  await expect(page.getByRole('link', { name: 'Next catalogue page', exact: true })).toBeVisible();
  await page.getByRole('link', { name: 'Previous page', exact: true }).click();
  await page.locator('.master .result').first().click();
  await expect(page.locator('.detail h1')).toHaveText('Fixture source');
  await expect(page.locator('.detail .file-card')).toHaveCount(2);
  const address = page.url(), reads = libraryReads;
  await page.locator('.detail .file-card').first().click();
  await expect(page.locator('.detail h1')).toHaveText('file-1.txt');
  assert.ok(page.url().includes('#/sources?'), 'file navigation must retain its source collection');
  assert.equal(libraryReads, reads, 'selection must retain the collection read');
  await page.goBack(); await expect(page.locator('.detail h1')).toHaveText('Fixture source');
  assert.equal(page.url(), address);
  await page.reload(); await expect(page.locator('.detail h1')).toHaveText('Fixture source');
  await page.screenshot({ path: path.join(output, 'modular-sources-fixture.png'), fullPage: true });
  mark('Sources use master/detail; child selection retains the collection; paging, Back and reload');

  await page.getByRole('link', { name: 'Knowledge', exact: true }).click();
  await page.getByRole('textbox', { name: 'Search content or an identifier' }).fill('fixture');
  await page.getByRole('button', { name: 'Search', exact: true }).click();
  await expect(page.locator('.master .result')).toHaveCount(4);
  await page.locator('[data-filter=matched][value=continuations]').click();
  await expect(page.locator('.master .result')).toHaveCount(1);
  await page.locator('[data-filter=matched][value=continuations]').click();
  await expect(page.locator('.master .result')).toHaveCount(4);
  mark('Content, assertions, structure and continuation reads combine; method checkboxes filter one collection');
  await page.goto(base + '#/library?q=fixture&epoch=2026&rank=confidence');
  await page.locator('.master .result').filter({ hasText: 'Fixture container' }).click();
  await page.getByRole('navigation', { name: 'Resource detail views' }).getByRole('link', { name: 'Structure', exact: true }).click();
  await expect(page.locator('tbody tr')).toHaveCount(3);
  const occurrences = page.locator('tbody tr td:nth-child(2) a');
  assert.notEqual(await occurrences.nth(0).getAttribute('href'), await occurrences.nth(2).getAttribute('href'));
  await occurrences.nth(2).click();
  await expect(page.getByRole('complementary', { name: 'Selected occurrence' })).toContainText('Position 3');
  await page.reload();
  await expect(page.getByRole('complementary', { name: 'Selected occurrence' })).toContainText('Position 3');
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('epoch'), '2026');
  await page.getByRole('navigation', { name: 'Resource detail views' }).getByRole('link', { name: 'Evidence', exact: true }).click();
  await page.getByRole('link', { name: 'Browse source catalogue', exact: true }).click();
  await page.locator('[data-filter=source_kind][value=file]').click();
  await page.locator('.master .result').first().click();
  await page.getByRole('navigation', { name: 'Resource detail views' }).getByRole('link', { name: 'Content', exact: true }).click();
  await expect(page.getByRole('region', { name: 'Recorded file lines' })).toContainText('<safe>');
  await expect(page.locator('.code-window safe')).toHaveCount(0);
  await page.locator('.declaration-list').getByRole('link', { name: 'render', exact: true }).click();
  await expect(page.locator('.code-line.selected')).toContainText('render()');
  await page.reload();
  await expect(page.locator('.code-line.selected')).toContainText('render()');
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('line'), '2');
  await page.getByRole('link', { name: 'Knowledge', exact: true }).click();
  await expect(page.getByRole('textbox', { name: 'Search content or an identifier' })).toHaveValue('fixture');
  assert.equal(new URLSearchParams(new URL(page.url()).hash.split('?')[1]).get('rank'), 'confidence');
  await expect(page.locator('[data-filter=kind]:checked')).toHaveCount(0);
  await page.getByRole('link', { name: 'Research', exact: true }).click();
  await page.reload();
  await page.getByRole('link', { name: 'Knowledge', exact: true }).click();
  await expect(page.getByRole('textbox', { name: 'Search content or an identifier' })).toHaveValue('fixture');
  await page.screenshot({ path: path.join(output, 'modular-world-file-fixture.png'), fullPage: true });
  mark('Repeated occurrences remain distinct; content and declaration lines are native-backed; source, research and knowledge transitions retain peer scope');
  await page.goto(base + '#/library?q=fixture');
  await page.locator('[data-filter=kind][value=entity]').click();
  await expect(page.locator('.master .result')).toHaveCount(1);
  await page.locator('[data-filter=kind][value=claim]').focus(); await page.keyboard.press('Space');
  await expect(page.locator('.master .result')).toHaveCount(2);
  await expect(page.locator('[data-filter=kind][value=claim]')).toBeFocused();
  await page.locator('[data-filter=tier][value="3"]').click();
  await expect(page.locator('.master .result')).toHaveCount(1);
  await page.locator('.master .result').first().click();
  await expect(page.locator('.detail h1')).toHaveText('Fixture claim');
  await expect(page.locator('.master .result')).toHaveCount(1);
  await page.reload(); await expect(page.locator('.detail h1')).toHaveText('Fixture claim');
  await expect(page.locator('.master .result')).toHaveCount(1);
  await expect(page.locator('[data-filter=kind]:checked')).toHaveCount(2);
  await expect(page.locator('[data-filter=tier]:checked')).toHaveCount(1);
  await page.screenshot({ path: path.join(output, 'modular-library-fixture.png'), fullPage: true });
  mark('Visible multi-select type/tier checkboxes; keyboard and focus retention; OR/AND filters survive selection and reload');
  for (const width of [1440, 768, 390]) {
    await page.setViewportSize({ width, height: 1000 });
    for (const name of ['Knowledge', 'Research']) await expect(page.getByRole('link', { name, exact: true })).toBeVisible();
    assert.ok(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1), `overflow at ${width}`);
    const result = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21aa']).analyze();
    accessibility.push({ width, violations: result.violations.map(item => ({ id: item.id, nodes: item.nodes.map(node => node.target) })) });
  }
  await page.getByRole('link', { name: 'Back to results', exact: true }).click();
  await expect(page.locator('.master')).toBeVisible();
  await expect(page.locator('.master .result')).toHaveCount(1);
  await expect(page.locator('[data-filter=kind]:checked')).toHaveCount(2);
  await page.screenshot({ path: path.join(output, 'modular-mobile-fixture.png'), fullPage: true });
  mark('Labelled navigation stays visible at desktop/tablet/mobile sizes; mobile return preserves filters');
  failLookup = true;
  await page.goto(base + '#/library?q=failure');
  await expect(page.getByRole('alert')).toContainText('Fixture reader unavailable');
  await expect(page.locator('.master .result')).toHaveCount(1);
  await expect(page.getByRole('heading', { name: 'No records in this view' })).toHaveCount(0);
  failLookup = false;
  await page.getByRole('button', { name: 'Retry unavailable methods', exact: true }).click();
  await expect(page.locator('.master .result')).toHaveCount(4);
  mark('Reader failures remain distinguishable from empty collections and retry successfully');
  await page.goto(base + '#/library?q=computed-only');
  await expect(page.getByRole('heading', { name: 'No records in this view', exact: true })).toBeVisible();
  await expect(page.locator('.master .result')).toHaveCount(0);
  await expect(page.getByRole('status').filter({ hasText: 'calculated an identity' })).toBeVisible();
  mark('A computed identity is not a knowledge result or proof of recorded content');
  await page.goto(base + '#/library?q=mixed-phrase');
  await expect(page.locator('.master .result')).toHaveCount(3);
  await expect(page.locator(`.master .result[href*="${id(70)}"]`)).toHaveCount(0);
  const sodium = page.locator('.master .result').filter({ hasText: 'Known Sodium' });
  await expect(sodium).toContainText('2 query member positions');
  await expect(page.locator('.master .result').filter({ hasText: 'Known Chloride' })).toHaveCount(1);
  await expect(page.locator('.master .result').filter({ hasText: 'Known containing composition' })).toHaveCount(1);
  await sodium.click();
  await expect(page.locator('.detail h1')).toHaveText('Known Sodium');
  const downloadEvent = page.waitForEvent('download');
  await page.getByRole('button', { name: 'Download readback', exact: true }).click();
  const download = await downloadEvent, exported = JSON.parse(await readFile(await download.path(), 'utf8'));
  assert.equal(exported.query, 'mixed-phrase');
  assert.equal(exported.query_root, id(70));
  assert.deepEqual(exported.selection.discovery_matches.map(match => match.ordinal), [0, 2]);
  await page.reload();
  await expect(page.locator('.detail h1')).toHaveText('Known Sodium');
  await page.goto(base + `#/library?q=computed-only&selected=entity%3A${id(70)}`);
  await expect(page.getByRole('heading', { name: 'Calculated identity', exact: true })).toBeVisible();
  await expect(page.locator('.master .result')).toHaveCount(0);
  await page.goto(base + '#/library?q=cache-only');
  await expect(page.locator('.master .result')).toHaveCount(1);
  await expect(page.locator('.master .result')).toContainText('NOUN');
  await expect(page.locator('.master .result')).toContainText('Canonical content in highway-cache');
  mark('Known members and containment survive a missing phrase root; duplicate query positions and originating readback context are retained; canonical cache content remains discoverable');
  assert.deepEqual(errors, []);
  assert.deepEqual(accessibility.flatMap(item => item.violations), []);
  passed = true;
} finally {
  await writeFile(path.join(output, 'modular-receipt.json'), JSON.stringify({ status: passed ? 'passed' : 'failed', kind: 'isolated-browser-fixtures', checks, errors, accessibility }, null, 2));
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
