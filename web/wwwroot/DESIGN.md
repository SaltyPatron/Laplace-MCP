# Laplace modular website: product and interaction design

Design proposal, 30 October 2026. The interactive preview is a design artifact over captured modular data. It is not a deployed replacement, an implemented identity service, or proof of the invention's semantic completeness.

## Product intent

Make the whole invention usable as a connected working environment: find information, understand what it is, inspect how it is constructed and supported, follow its connections, reason and act with it, and retain an attributable result.

The master/detail analogy describes interaction continuity and shared model-derived behavior. It does not prescribe ASP.NET MVC, server-rendered Razor, CRUD scaffolding, a fixed linguistic ladder, or a framework. The CIEDigital reference demonstrates that type configuration, query construction, routing and views can share infrastructure. Its historic implementation and cryptography are not a design template.

The invention determines scope. Today's endpoints determine which contracts need implementation. Missing capabilities are engineering work, not permission to erase a product requirement.

## Product organization

| Area | User purpose | Shared resources and operations |
| --- | --- | --- |
| Home | Begin a lookup or continue meaningful work | Search, recent authorized research, explicit connection state |
| Library | Discover and inspect content and connections | Collections, resource detail, typed filters, related collections, structure and evidence |
| Research | Keep an investigation coherent | Saved queries, references, comparisons, notes, witnessed conversations, calculations and receipts |
| Sources | Assess origin and authority | Witnesses, dependence roots, versions, licenses, corpus/file lineage, attestations and outcomes |
| Workbench | Admit, construct, compute and synthesize | Acquisition, grammars, recipes, ingestion, code, domain tools, model work and bounded jobs |
| Exports | Obtain and manage attributable artifacts | Scope selection, projections, model recipes, licenses, attribution, quotes, jobs and downloads |
| Account menu | Manage identity, membership and purchases | Linked providers, sessions, workspaces, API credentials, subscriptions, receipts and donations |

Primary navigation stays small. Resources can be reached through search, context links and saved work. A source or model does not need a second, incompatible inspector because it was reached from a workbench. Operator diagnostics sit within relevant administration and receipt views.

Domain experiences can be rich. A code resource needs a code viewer, outline and navigable declarations; media needs an appropriate viewer; a game needs a board or domain representation. These are registered renderers and operations inside the shared context, not a demand that every object become the same table.

## Resource semantics

| Concept | Required distinction in the UI |
| --- | --- |
| Entity | Canonical content identity. A name is a display/realization, not a globally unique semantic interpretation. |
| Physicality | A typed structural view, with its exact selected path/trajectory. It must link back to its canonical entity. |
| Occurrence | A particular appearance in a parent/source/context, with a stable anchor and ordered position or run range. Repeated equal content remains distinguishable. |
| Tier | Recursive structural altitude. It is not an ontology, relevance score or fixed codepoint-to-concept ladder. |
| Type, modality and masks | Declared facets with defined scope and provenance. A type is not inferred from tier or a page title. |
| Claim | A proposition with ordered members and potentially n-ary roles. SRO is one possible projection. |
| Attestation | A witness's assertion of a claim, including qualifiers, context, series observations, outcome and lineage. |
| Source/witness | Attributable origin, version, rights and dependence roots. Copies and repeated assertions do not silently become independent support. |
| Standing | Strength, uncertainty, volatility and witness breadth, with unavailable state explicit. It is distinct from source trust, geometric distance and ranking. |
| Relation | Typed and directed where applicable, with roles and context. Containment is not synonymy; locality is not semantic equivalence. |
| Operation/receipt | Explicit inputs, semantic program/version, scope, work envelope, alternatives, unresolved obligations, outputs, attribution and outcome. |

All canonical content retains its BLAKE3 address. Claim or source projections may point to the same entity without creating another content identity. Operational records and structural/occurrence selectors have explicit typed addresses; the browser does not invent hashes for them.

## Interaction model

1. A lookup starts with a question, content or address and an explicit accessible world/scope. It can return exact identities, candidate interpretations, typed matches and unresolved ambiguity. Matching the form `dog` does not select an animal sense or language by itself.
2. A collection offers meaningful summaries and typed facets derived from discovery. Search, filters, sorting, cursor/snapshot and selected view belong to its shareable address. Collections distinguish exact, bounded, estimated and unknown counts.
3. Selecting a result opens a useful summary immediately. Readable content, kind, scope and available evidence lead; identifiers and coordinates are available as secondary details.
4. Detail presents overview, exact structure, evidence and typed connections where supported. These are resource views, not separate products. The selected view survives reload and sharing.
5. Every evidence item links to its proposition, source, assertion/context and relevant receipt. Summary results offer the same traceability without forcing a separate search for provenance.
6. Following a connection retains its direction, relation identity, context and the originating research trail. Back returns to the prior query, filters, position and selected item. Related collections are ordinary addressable collections.
7. Save, compare, cite, converse, compute and export act on the selected resource/reference set. They do not require retyping an ID into another tool.

Desktop supports a collection alongside detail. Narrow screens show collection and detail sequentially with an explicit return link. The global header remains compact; identity and payment controls do not displace the first useful result. Avoid independent scrolling boxes for every subsection.

UI state that names content belongs in the URL. Private notes, sensitive queries and private research state stay in an authorized workspace record; share links must not leak them into public URLs. A public deep link does not confer permission to its target. The server rechecks the complete referenced scope.

## Discovery and reusable rendering

The resource service needs a versioned discovery contract, not just a route list. `/v1/surface` currently exposes service operations but does not describe the full browser resource model.

Discovery declares resource kinds, available type definitions, fields, render hints from a finite allowlist, typed facets and operators, relation descriptors, view availability, operation references and capability states. Schemas use standard JSON Schema/OpenAPI constructs; semantic relations and type evidence remain linked resources. The fundamental identity/provenance laws are not arbitrary administrator-editable metadata.

The frontend registry provides a shared collection renderer, resource inspector, evidence list, relationship collection, ordered structure viewer, job/receipt display and account flow. Type-specific renderers extend these primitives for text, code, image, audio, video, games, model structure and future domains. Unrecognized types have an intelligible summary/link fallback; unsupported operations explain the actual missing capability.

Do not fetch executable UI code from untrusted source metadata or interpret arbitrary template strings. Resource data is escaped. Discoverable links and actions are validated against approved origins, methods and operation schemas. Backend capabilities are rechecked on execution; hiding a button is not authorization.

The backend owns mask definitions and decoding. Current Native banks distinguish `kind` flags from enum fields such as `upos`, `lexfile`, `deprel`, `vnrole` and `wnframe`. Enums store slot+1, with zero absent. The UI must not treat every bank as a bitmap or copy an older Wiki layout. Return the applicable manifest digest and semantic labels with field/row scope. Preserve 64-bit masks, Hilbert values and counts as canonical strings rather than lossy JavaScript numbers.

## Browser/API boundary

Use a same-origin browser gateway for sessions, workspace authorization, anti-forgery, account records and payment orchestration. It calls modular HTTP operations through a typed client. Native remains responsible for identity, decomposition, structure, masks, evidence, standing, query/cognition, calculations and attribution. The gateway does not read PostgreSQL directly to rebuild semantic results, scrape CLI logs or implement alternate mathematics.

The existing shared modular bearer token is a service credential, not a browser identity. Never embed it in JavaScript, browser storage or a downloadable configuration. The trusted gateway must enforce resource scope; longer-term native request context must carry the relevant authorization/world and program scope without relying on a single global token as per-user authority.

`openapi.json` describes the implemented `/api` browser boundary. `/api/schema` obtains view projections, mask banks, occurrence roles and operation input schemas from Native. Entity identity is shared across these projections: a format tree references a content entity rather than renumbering or cloning it. The broader research, ingestion, synthesis and commerce interfaces discussed here remain design work beyond that deployed boundary.

Important contract rules:

- Batch structured reads and label resolution; no N+1 request per constituent, and no client reconstruction of semantics from raw logs.
- Exact entity resolution, occurrence anchors, typed relationships and witness/standing detail must work for canonical IDs, not only text queries.
- Pagination is cursor-based and bound to authorized scope, filter/sort and read snapshot. Expired snapshots have a typed recovery path; pagination cannot silently mix states.
- Counts carry a scope and qualification. Unknown, empty, failed, filtered and not-resident are different states.
- Successful panels can remain usable when another independent read fails. Superseded responses cannot overwrite a newer selection. Navigating away cancels expendable work.
- Long work has bounded admission, durable job identity, progress, cancellation semantics, explicit partial/final status and receipts. It cannot present an endless ellipsis as successful execution.
- Normal errors use Problem Details with useful recovery and a secondary correlation ID. Authorization failures trigger identity/access guidance instead of repeated red fetch-error panels.
- Caches include principal/workspace/world, capabilities and snapshot scope. Private authorized data is never reused across users through an unqualified shared cache.

## Identity first

Implement Google and Microsoft with supported OpenID Connect middleware, authorization code and PKCE, provider discovery and validated issuer/audience/signature/state/nonce. Store provider credentials and tokens server-side. Use an opaque Secure HttpOnly browser session with durable revocable session state, explicit CSRF protection for mutations and a safe local return address.

External identity keys are issuer plus subject, with provider-specific tenant constraints where relevant. Account linking requires an explicit authenticated flow and proof of the identity being added; matching email alone does not merge accounts or confer owner authority. Existing declared owner bindings are installation policy, not an assumption that every provider login is an administrator.

Sign-in preserves the original resource/research address. Existing configured OAuth clients are reusable only after the modular origin's exact callback URIs are registered and environment isolation is established. Provider client registration, signing/key storage, cookie names, session database, redirect origin and logout behavior are explicit deployment inputs. Reading secrets or finding keys is not evidence that an end-to-end sign-in works.

Admission separates identity, resource authorization, world/data scope and compute allowance. Anonymous shell/help/allowed public content is useful without exposing operator-only API reads. A 401 does not become a generic service outage. Account session controls include list/revoke/logout; sensitive changes require appropriate reauthentication.

Provider details: [Google OIDC](https://developers.google.com/identity/openid-connect/openid-connect), [Microsoft authorization code and PKCE](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-auth-code-flow). Use supported libraries rather than implementing these protocols by hand.

## Payments and exports

Knowledge lookup, ordinary inspection, evidence traversal and following entitled connections do not get arbitrary “unlock” charges. Content entitlement, authority and compute are independent. A subscription must not change what a proposition means or silently make an answer less informed.

An export flow begins with an inspectable scope and requested projection: selected resources, documents/structured formats, recipes, target architecture or model artifact, versions, licenses, witness attribution and declared work. Ordinary research-data downloads can be free. A paid synthesis/export has an explicit server quote for a concrete deliverable and work allowance; the user sees those terms before checkout.

Stripe Checkout, customer subscriptions/portal and donations are distinct workflows. The server creates Checkout sessions with idempotency and binds them to the authenticated purchaser, authorized scope and quote. Payment success is established by verified durable webhook/reconciliation state, not the return URL. Process duplicate and out-of-order events safely; distinguish funds state from fulfillment/job state; retry delivery without charging again.

Model synthesis/export retains the canonical scope, recipe/target and BLAKE3-addressed vocabulary identity, derivation receipt and exact royalty attribution. Commercial cartridge encryption belongs to the export implementation; the website does not expose decryption material or equate obfuscated URLs with permission. Downloads are authorized artifacts with expiry/revocation policy, not public filesystem paths.

Payments documentation: [Stripe Checkout](https://docs.stripe.com/payments/checkout), [subscription events](https://docs.stripe.com/billing/subscriptions/webhooks), [customer portal](https://docs.stripe.com/customer-management), [signed webhook handling](https://docs.stripe.com/webhooks).

## Full workbench scope

The architecture includes the corpus estate, source addition and acquisition, dependencies/order DAG, file trees, chained Tree-sitter inspection, AST fields and symbols, highway identifiers, unresolved grammar/recipe obligations, Laplace-generated recipe proposals, manual refinement, plan/claims/no-load/round-trip previews, streamed admission, cancellation and durable job receipts. No user flow requires hand-editing native source to admit a supported new corpus.

Research includes joint interpretation, competing senses/bindings, typed structural/semantic/calculation/evidence channels, unresolved obligations, explicit scope and work envelope, reproducible comparisons, source-grounded realization and witnessed multi-turn context. Conversation is a trajectory, not an isolated chat widget with a fabricated fixed window.

Code and domain work includes source/declaration navigation, callers/callees, ordered structural detail, exact references, construction and witnessed compile/test/run outcomes. Rich domain controls and future modalities attach to the same resource/receipt contracts.

Synthesis includes target-neutral scope and program selection, reproducible model/export recipes, attribution, artifact status and delivery. Federation and remote/local residency distinguish where content is available from whether it exists or is authorized. Cache availability is not semantic authority.

## Verified data behind this preview

The connected modular MCP returned canonical `dog` entity `739118a25a01c0155d0689e390715dda`, tier 1, three scalar constituents; eight claim records, seventy-three containing paths, twelve paths sampled with fan=12, and eight assertion records attributed to one unregistered trunk. All eight claim rows have `stood:false`. `sources` returned an empty registered list. The separate Engine `hop dog` read reported zero attested claims. These facts have different meanings and are not collapsed into one number.

The current API array named `occurrences` contains holder compositions with constituents; it does not provide a unique anchored occurrence record. The preview calls them containing paths. Backend occurrence anchors are required before claiming the full repeated-occurrence research flow.

Canonical-ID lookup via the connected MCP returned `{"error":"input or id"}` even for the ID returned by text lookup. Source inspection shows tools/call passes its entire JSON-RPC body to `on_search`; the string-field helper finds the envelope `id` before arguments.id. This is a reproducible MCP failure with a plausible source-level cause. Direct HTTP parity and a focused repair remain to be verified; the preview does not manufacture member labels to bypass it.

No live OAuth or payment flows were executed. No application source or installed website was replaced. The prototype's local notebook, navigation, filters and JSON downloads are functional interaction design.

## Engineering sequence

1. Establish product ownership and the modular browser host boundary. Implement identity configuration/session lifecycle first, with real provider round trips and resource authorization. Do not move monorepo semantic handlers into the gateway.
2. Implement discovery, exact resource/label batches, typed collection/filter contracts and native read parity. Fix selected-ID routing and deliver stable structure/occurrence addresses. Implement the shared shell and resource renderers against those contracts.
3. Implement persistent research, evidence/source/standing views, related collections, comparisons and operation receipts. Carry query context through every domain viewer.
4. Implement the workbench family, witnessed conversation/computation, construction/domain tools and export/synthesis contracts. Capability gaps stay explicit until owned backend work closes them.
5. Integrate Stripe against concrete account/export/subscription/donation workflows, then complete installation/CI delivery and authenticated browser acceptance on declared environments.

This is dependency ordering, not permission to declare the whole product complete after identity or one lookup. `ACCEPTANCE.md` lists the acceptance boundaries.

The installed API contract uses [OpenAPI 3.1.2](https://spec.openapis.org/oas/v3.1.2.html) and JSON Schema. The current host returns `error` and `detail` fields for failed reads; the earlier Problem Details design is not its deployed error contract.
