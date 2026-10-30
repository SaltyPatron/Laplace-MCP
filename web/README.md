# Modular browser application

The modular UI is hosted independently from the monorepo UI. On HART-DESKTOP,
IIS serves it at `https://dev.hartonomous.com/modular/` in the
`laplace-modular-web` application pool. The root mono application remains at `/`.

The small .NET host serves browser assets, public runtime configuration and a
health endpoint. It implements no substrate semantics and does not connect to
PostgreSQL. Identity discovery, Google/Microsoft challenges and session reads
use the installed same-origin `/v1/auth` service. OAuth callbacks remain on that
identity service; no provider secrets or service bearer tokens enter this host.

**Current data boundary:** resource screens use a labelled captured modular
readback. They are not connected to live native collection/query endpoints.
Research references and notes are browser-local. Stripe and workbench operations
are not connected. `wwwroot/DESIGN.md` and `wwwroot/openapi.json` describe the
proposed wider product, not already implemented APIs.

The installed identity service is intentionally shared during coexistence. This
is not a standalone identity deployment; removing mono requires replacing that
explicit dependency with the equivalent shared identity service first.

Deploy through the Operations-owned declaration and publisher:

```powershell
pwsh -File ../Laplace-Operations/installation/Publish-ModularWeb.ps1
```

The publisher builds outside the source tree, creates an immutable release,
grants declared operator and pool access, activates only the modular IIS
application, verifies its health and root content, and writes a deployment
receipt. IIS configuration needs elevation. No database migration or existing
mono release switch is part of this deployment.

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
