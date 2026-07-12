# Unified Fork Workflow

This fork keeps all maintained SDR++ work on `integration/main` while retaining
the official project as the `upstream` remote. Feature branches remain small
and task-specific, which lets multiple Codex tasks start from the same complete
baseline without losing platform or module work.

## New Codex task prompt

Use a prompt such as:

> Work in the SDR++ repository. Fetch `origin`, start from
> `origin/integration/main`, and create `codex/vdl2-next`. Work only on the VDL2
> decoder. Read `AGENTS.md` first, preserve the macOS and iOS builds, run the
> relevant build validation, then commit and push the feature branch.

Replace `vdl2-next` and the requested scope with the actual task. The branch
name describes the work; it is not the module itself.

## Bringing in official upstream changes

Update `integration/main` in a clean worktree, fetch both remotes, merge the
latest `upstream/master`, resolve conflicts without dropping fork modules, and
run the combined validation before pushing. Keep the pre-integration tags and
do not force-push the integration branch.

## Recovery

The original feature tips are tagged on the fork as
`pre-integration/2026-07-12/*`. If a later merge regresses a module, compare or
restore that module from its recovery tag rather than guessing at its prior
state.
