# Contributing to VIcky-Blue

VIcky-Blue is a fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5), licensed under
GPL-2.0. These rules apply to everything that ends up in or around this repository: code,
comments, commit messages, branch names, pull requests, issues, discussions, review comments,
screenshots and logs.

## 1. Do not name the target title

This fork concentrates on a single title. That title, its series, and any abbreviation,
numeral variant, nickname or obvious misspelling of either must not appear anywhere in this
repository or its tracker.

- Write "the target title" instead.
- Scrub logs, paths and screenshots before posting them. Dump folder names and window titles
  are the usual leaks.
- Do not work around the automated check with creative spelling. If a reader can tell which
  game you mean, it is a violation.

Commits and pull requests that break this rule are rejected and must be rewritten, not just
followed up with a fix. Issues and comments that break it are edited or removed.

## 2. No game files

Never commit, attach or link to:

- game dumps or any part of one (executables, modules, archives, `sce_sys` / `sce_module`
  contents, assets, shaders extracted from a game, save data);
- console firmware, system modules, keys or decrypted system software;
- memory dumps, captures or traces that embed copyrighted game data.

Short disassembly or log excerpts needed to explain a fix are fine as long as they follow
rule 1. Test fixtures must be synthetic.

## 3. AI-assisted contributions are allowed

You may use AI tools to write code, tests, documentation and commit messages. You remain
responsible for what you submit: you must understand it, have built and tested it, and it
must follow every other rule here. AI co-author trailers are fine.

## Enforcement

`scripts/policy-check.sh` implements rules 1 and 2. It runs:

- locally through [pre-commit](https://pre-commit.com) — install both hook types once:

  ```sh
  pre-commit install --hook-type pre-commit --hook-type commit-msg
  ```

- in CI (`.github/workflows/policy.yml`) on every push, pull request, issue and comment.

The automated check is a floor, not the definition of the rule. Maintainers have the final
say.

## Code

Follow the existing style (`.clang-format`, `.clang-tidy`) and keep commits focused. Commit
subjects use the upstream `area: summary` form.
