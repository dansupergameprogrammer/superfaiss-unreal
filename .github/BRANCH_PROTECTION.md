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

## Tags: no ruleset, deliberately

No tag protection rule is configured, and this is a decision rather than an
omission.

GitHub Rulesets expresses *who* only through a bypass list, and on a user-owned
repository the selectable actors are Deploy keys, the Repository admin /
Maintain / Write roles, and a pair of Copilot apps. The GitHub Actions app is
not among them, so a workflow's `GITHUB_TOKEN` cannot be granted bypass. Every
role entry resolves to the sole maintainer, and a fine-grained PAT carries that
same identity, so any of those choices makes the rule advisory against the only
person it would apply to. The one genuinely distinct actor is a deploy key,
which would mean a write-capable private key held in repository secrets.

That cost buys protection against a single failure mode: a maintainer hand-cutting
a release tag outside the gate. Releases here are produced by an automated
publish sequence that runs the coherence checks before tagging, so the failure
mode is not one this project has. The checks themselves are the gate, and they
run on every push and before every release.
