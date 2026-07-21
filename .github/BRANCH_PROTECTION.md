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

## Ruleset: tags matching `v*`

Settings -> Rules -> Rulesets -> New ruleset -> **Target: Tags**.
Target pattern: `v*`.

Enable **Restrict creations**, **Restrict updates**, and **Restrict deletions**.

There is no "who can push" dropdown. *Who* is expressed entirely through the
**Bypass list**, and this is the part that is easy to get wrong in both
directions:

- An **empty** bypass list blocks everyone, including `release.yml` — its
  `GITHUB_TOKEN` acts as `github-actions[bot]`, which is subject to the same
  ruleset. The release workflow would fail on the tag push.
- Adding **Repository admin** makes the rule advisory for the sole maintainer,
  who then bypasses it automatically. That documents intent; it does not
  enforce anything.

The configuration that actually enforces "release tags are cut by the workflow,
not by hand" is: bypass list contains **only the GitHub Actions app**. The
maintainer then cannot hand-cut a `v*` tag, and the only path to one is
dispatching `release.yml`, which is gated on the coherence checks. That is the
enforcement side of the release-process finding.

If the GitHub Actions app is not offered as a bypass actor (availability differs
between user-owned and organization repositories), the fallback is a
fine-grained PAT with `contents: write`, stored as a repository secret and used
by `release.yml` in place of `GITHUB_TOKEN`.

Note that `release.yml` runs on `ubuntu-latest` and needs no self-hosted runner,
so this path is usable as-is.
