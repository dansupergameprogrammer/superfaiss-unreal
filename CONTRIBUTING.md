# Contributing

## Scope

This repository is the Unreal Engine integration for SuperFAISS. The
underlying search library lives in a separate repository and is vendored
into `SuperFAISSUnreal/Source/ThirdParty/SuperFAISS/` — see
`VENDORED_VERSION.txt` in that directory for the exact core commit vendored.
Changes to the vendored tree itself belong upstream, in the core repository,
not as a direct edit here.

## Before opening a pull request

- Run the coherence checks locally: `python SuperFAISSUnreal/ci/check_*.py
  --help` for each script's arguments. These same checks run in CI and block
  merge on failure.
- Build the plugin against the Editor target and confirm the `SuperFAISS.*`
  automation suite passes.
- Keep commits scoped to one change; write commit messages that describe the
  change, not the process of making it.

## Required CI checks

Every pull request must pass `coherence-checks` (vendored coherence, version
identity, documented signatures, asset references) and `plugin-build`
(editor build, automation tests, Shipping build, `RunUAT BuildPlugin`). See
`.github/BRANCH_PROTECTION.md` for the exact required-check list.

## Versioning

The plugin's `VersionName` in `SuperFAISSUnreal.uplugin`, the top entry of
`SuperFAISSUnreal/CHANGELOG.md`, the vendored core's `version.h`, and the
vendored core's own `CHANGELOG.md` top entry must always agree — this is
enforced by `check_version_identity.py`. Release tags are cut by the
`release` GitHub Actions workflow, not by hand.

## License

By contributing, you agree that your contribution is licensed under this
repository's license (see `LICENSE`).
