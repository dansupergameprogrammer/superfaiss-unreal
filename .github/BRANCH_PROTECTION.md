# Branch protection — required configuration

This repository's CI is defined in `.github/workflows/`. GitHub does not let
a workflow file configure branch protection on itself — the setting is
applied here as documentation; a repository admin applies it under
**Settings > Branches > Branch protection rules** for `main`.

## Rule: `main`

- **Require a pull request before merging** — enabled.
- **Require status checks to pass before merging** — enabled, with:
  - `Require branches to be up to date before merging` — enabled.
  - Required checks (exact job names, as they appear once each workflow has
    run at least once on this repository):
    - `coherence-checks / vendored-coherence`
    - `coherence-checks / version-identity`
    - `coherence-checks / doc-signatures`
    - `coherence-checks / asset-references`
    - `plugin-build / editor-build`
    - `plugin-build / automation-tests`
    - `plugin-build / shipping-build`
    - `plugin-build / package-plugin`
- **Require conversation resolution before merging** — enabled.
- **Do not allow bypassing the above settings** — enabled (includes
  administrators).

`plugin-build`'s `orphan-sweep` job is intentionally NOT in this list: it
only runs meaningfully on the maintainer's own self-hosted runner (see the
prerequisite note at the top of `plugin-build.yml`), so it cannot gate a
pull request from a fork or a contributor without that runner's access. It
still runs on every push to `main` and is checked before every release.

## Rule: tags matching `v*`

- **Restrict who can push matching tags** — enabled, allowing only the
  `release.yml` workflow (via its `GITHUB_TOKEN`) and repository admins.
  This is the enforcement side of "release tags are cut by the workflow, not
  by hand" — a hand-pushed tag that skips `release.yml`'s coherence gate is
  exactly the F4 finding this rule closes.
