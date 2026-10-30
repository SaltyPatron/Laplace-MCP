# Modular browser application

The modular UI is hosted independently from the monorepo UI. On HART-DESKTOP,
IIS serves it at `https://dev.hartonomous.com/modular/` in the
`laplace-modular-web` application pool. The root mono application remains at `/`.

The small .NET host serves browser assets, public runtime configuration and a
health endpoint. It implements no substrate semantics and does not connect to
PostgreSQL. A packaged native reader connects to the declared modular database.
Identity discovery, Google/Microsoft challenges and session reads
use the installed same-origin `/v1/auth` service. OAuth callbacks remain on that
identity service; no provider secrets or service bearer tokens enter this host.

Home and Sources discover stored source/file trunks from the native kind bank
on physicality, independently of the witness registry. The source's ordered
file collection comes from its actual content path. Library lookup includes
mixed resource projections by default; optional kind/tier facets filter the
returned collection. Canonical-ID links resolve live details. Physicalities
retain repeated ordered positions and stored vertex annotations. Attestation
rows and bounded structural ancestry are separate views. A registry row adds
metadata; it does not define the witness trunk's existence.

The thin browser API exposes `GET api/library`, `api/catalog`, `api/lookup?q=`,
`api/resources/{id}`, `api/provenance/{id}`, `api/sources`, `api/surface`, and
`POST api/operations/{embeddings|forward|shape}`. Only these explicit read
operations are forwarded; arbitrary filesystem/shell/ingestion routes are not.
No captured-data fallback is shipped. Collection paging is over one returned
read; the trunk catalogue has a canonical-ID cursor. Separate requests are new
read snapshots, not a pinned cross-request historical snapshot.

The IIS identity launches one native process on an ephemeral loopback port.
Its bearer token is generated in memory and never reaches the browser. A
Windows job terminates the reader when its host exits or recycles. PostgreSQL
uses verified TLS and SSPI as `laplace_modular_web`, with read-only grants and
read-only transactions. Native owns decomposition, rendering, containment,
attestation and all database queries. Cache/DLL resources come from the selected
immutable modular installation; the matching web release contains `lpm.exe`.

Research references and notes are browser-local. Readback and note downloads
contain actual responses and references; they are not signed synthesis receipts.
Stripe, acquisition/recipe editing, write jobs, witnessed conversation and model
synthesis are not integrated into this browser application. The wider product
in `wwwroot/DESIGN.md` and `wwwroot/openapi.json` remains a proposed contract.

The installed identity service is intentionally shared during coexistence. This
is not a standalone identity deployment; removing mono requires replacing that
explicit dependency with the equivalent shared identity service first.

Deploy through the Operations-owned declaration and publisher:

```powershell
pwsh -File ../Laplace-Operations/installation/Initialize-ModularWeb.ps1
pwsh -File ../Laplace-Operations/installation/Publish-ModularWeb.ps1
```

The publisher builds outside the source tree, creates an immutable release,
grants declared operator and pool access, activates only the modular IIS
application, verifies native read-only database scope and a live lookup, checks
mono health, and writes a deployment receipt. Initial provisioning grants the
dedicated SSPI role read access; it does not seed/reset substrate data. IIS
configuration needs elevation. The mono release stays independently owned.

The child pool needs read access to the parent site's configuration. Publication
grants read access to the parent web directory and its immediate files, without
inheriting into the mono releases or runtime state. The child inherits the
installed parent ANCM handler and supplies its own process configuration.

Verify the deployed IIS URL with:

```powershell
node web/checks/iis.mjs
```

The check uses the workspace's existing Playwright/axe development tools by
default. `LAPLACE_BROWSER_CHECK_PACKAGE` can select another package containing
those tools; no browser tooling is a dependency of the deployed application.
It verifies both configured provider challenges without logging their state,
nonce or cookies. Human provider consent and authenticated callback completion
are separate acceptance steps.

The browser check begins at a fresh Home with no IDs or queries. It follows
source/file controls, direct reload and Back, enters search through the form,
inspects structure/evidence/ancestry, saves notes, downloads a real response,
runs an operation, and checks mobile and recoverable read failure states.
