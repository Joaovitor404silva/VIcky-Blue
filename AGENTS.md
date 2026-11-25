# Agent instructions

VIcky-Blue is a single-title fork of KytyPS5. See [README.md](README.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).

## Hard rules

- Never name the target title or its series anywhere: code, comments, commit messages, branch
  names, pull requests, issues, logs. Write "the target title".
- Never add game files, dumps, firmware or keys to the repository.
- `scripts/policy-check.sh` enforces both; run it on changed files and commit messages when in
  doubt.

## Commit and push frequently

- Commit each logical change as soon as it works, instead of letting unrelated edits pile up
  in the working tree.
- Keep commits small and focused, with a descriptive message in the `area: summary` form and a
  body that explains what changed and why. Say plainly when something is a workaround.
- Push after committing, so the remote always reflects the current state of the work.

## Keep the README status current

The "Current Status" section of [README.md](README.md) is the project's status record. Update
it in the same commit or push whenever the state of the project changes: how far the target
title gets (boot, menus, in-game), what is known broken, which temporary workarounds are in
place, and which platforms are tested.
