# Acceptance boundaries for the modular website

This file distinguishes delivered browser read workflows from full product acceptance. Read workflows and deployment do not establish witnessed conversation, ingestion, synthesis or payments.

## Live IIS read workflows

- A fresh Home discovers actual stored source/file trunks without a prepared query or identifier. Source hierarchy and ordered files are navigable from visible controls.
- Home, Library, Research, Sources, Workbench and Exports render at desktop and mobile widths.
- Query, kind/tier filters, selected resource and selected view survive reload and browser Back/Forward.
- A child, claim, physicality, assertion or source reference opens through shared resource detail. Following a link retains lookup context.
- Ordered constituent positions preserve repeated equal identities. Unknown labels and unavailable standing stay explicit.
- Source/file trunks are discovered through stored physicality roles. Containment provenance and registry metadata are separate; registry counts are not a census of witness trunks.
- Research references and notes persist locally; exported notes and live native data are valid JSON with read context.
- Sign-in controls discover the installed identity service and produce real provider challenges. No account consent or payment completion is simulated.
- Explicit native read operations execute through browser forms. Failed reads stay distinct from empty results and can recover.
- Keyboard access, dialog focus, contextual titles, accessible names, contrast and responsive layout are checked with Playwright and axe. No uncaught exceptions, missing local files or page-wide horizontal overflow.

## Identity and authority: required live acceptance

- Real Google and Microsoft round trips against the configured modular origin: logged-out entry, consent, callback validation, session readback, deep-link return, logout and revocation.
- Denied consent, expired correlation, invalid state/nonce/token/issuer/audience, duplicate callbacks and provider failure have bounded recoverable behavior.
- Account linking requires proof of both sessions; same-email identities cannot merge themselves or gain installation-owner privileges.
- Owner bindings, workspace membership, private resources and operator actions are tested through the actual policy path, including denied users.
- Browser never receives a service token, client secret or provider refresh token. Mutation CSRF, return URL validation and cookie/session isolation work in the configured hosts.

## Resource and research acceptance

- Direct canonical-ID reads work across HTTP, MCP and browser without renaming the input as text. URI encoding handles Unicode, literal percent signs and embedded path characters correctly.
- Discovery describes types, fields, render hints, relation roles, facets, masks and available operations. Unknown types have a safe useful fallback.
- Real multi-domain data exercises canonical identity, multiple physicalities/tiers, ordered constituents, wide structures, repeated occurrences, source paths, n-ary claims and scope qualifiers.
- Position/run anchors distinguish equal content at different locations. Traversal retains relation direction and does not flatten roles or mistake geometry for meaning.
- Claim identity, testimony, witness trust, dependence roots, standing and uncertainty remain separately inspectable. Absence of standing is not 0% confidence; refutation is not missing data.
- Collections paginate consistently under concurrent writes using snapshot-bound cursors. Counts identify scope and exact/estimate/bounded/unknown status. Authorization changes invalidate cached data appropriately.
- Search can retain competing interpretations and unresolved obligations. Literal-form lookup does not automatically choose a sense or language.
- Saving/reopening a research workspace retains query, scope, filters, trail, exact references, notes, comparisons and operation receipts. Sharing does not leak private content.
- Slow/cancelled/partially failed queries retain useful independent results, report progress/recovery and never overwrite a later selection with stale data.

## Full product acceptance

- Witnessed conversation preserves context, corrections, calculations, alternatives, feedback and source/derivation links through restart and readback.
- Source onboarding, dependency order, acquisition, file tree, grammar chaining, AST/field inspection, recipe proposal/edit, plan/claims/no-load/round-trip preview and streamed admission are real operations with durable receipts.
- Idempotent retry does not multiply source witnessing. Failed/cancelled jobs distinguish committed output from uncommitted work.
- Code search/detail, declaration/caller/callee navigation and construction/tool outcomes point to exact canonical versions and toolchain/environment receipts.
- At least text, code and one nontext domain exercise specialized viewers through shared resource contracts. The types are not constrained to the initial sample or a lexical-only ontology.
- Local/remote content, authorized scope, cache residency and unavailable service state remain distinct. New modalities and relation kinds do not require a new navigation architecture.
- Model/data export scope, format/recipe, target, source licenses, exact attribution and reproducibility are inspectable. Exported IDs and content match the selected substrate state; round-trip guarantees are tested only where declared.
- Paid exports, subscriptions and donations use real Stripe test-mode Checkout, portal and verified webhook state. Duplicate/reordered events, checkout abandonment, payment failure, retries, refunds/disputes and fulfillment failure do not create duplicate charges or unjustified entitlements.
- Basic entitled lookup, evidence and relationship navigation remain usable independently of an export purchase. Compute limits are explicit work envelopes, not hidden changes to semantic authority.

## Delivery evidence

Source build, focused tests, CI, installed package, identity registration, database migration, deployment receipt and authenticated runtime/browser readback are separate evidence. A design screenshot, a 200 health response, an unauthenticated 401, a native text proof or a single successful lookup cannot substitute for them.

Staging and production changes use declared CI/CD. Existing installations, unrelated dirty work, data and credentials are preserved. Do not call the product complete until the required live flows above are delivered or the user explicitly changes scope.
