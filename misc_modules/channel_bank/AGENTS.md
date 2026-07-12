# Channel Bank Module Instructions

These instructions apply to all work under `misc_modules/channel_bank/` and
supplement the repository-root `AGENTS.md`.

## Scope and platform preservation

- Keep Channel Bank work scoped to this module unless the user explicitly
  requests a core or cross-module change.
- Desktop/macOS uses `src/main.cpp`; iOS uses `src/main_ios.cpp`. Preserve both
  implementations and their platform-specific CMake wiring.
- Do not overwrite the iOS implementation with the desktop implementation, or
  vice versa. Share behavior deliberately when a change belongs on both.
- Use a task-specific build directory or a build directory known to match the
  current branch. Do not trust a CMake cache copied from another worktree.
- Never commit models, recordings, runtime configuration, app bundles,
  dependency output, or build output.

## Required validation

At minimum, configure the repository with Channel Bank enabled and build the
`channel_bank` target. Build the full `sdrpp` target when shared interfaces,
module registration, packaging, or runtime integration changed. For iOS-facing
changes, also build the iOS simulator application.

Before deployment, record the commit being tested and whether the worktree is
dirty. A successful library link is not sufficient deployment validation.

## Private deployment configuration

The public repository intentionally contains no SSH host, user name, launchd
labels, detached-server commands, or private paths. Before any Channel Bank
deployment, locate the repository root and read `../LOCAL_DEPLOYMENT.md`
relative to that root.

- If the private guide is absent, stop and ask the user for it. Do not guess
  the host, service labels, application path, or signing procedure.
- Never stage or commit the private guide or reproduce its private values in a
  tracked file, commit message, pull request, or public log.
- The private guide is operational configuration, not source code. Git pulls
  and fresh clones do not replace it.

## Remote macOS completion workflow

For Channel Bank work intended for runtime testing, the default completion
workflow includes deploying the complete macOS test application after build
validation, unless the user explicitly requests build-only work.

Follow the private guide in this order:

1. Build and validate the complete macOS application locally.
2. Stage the new app bundle on the remote Mac without touching the installed
   app or stopping services yet.
3. Inspect and record which configured SDR++ server targets are running. The
   private guide may include both plist-managed LaunchAgents and explicitly
   authorized detached servers. Only listed targets may be stopped or
   restarted.
4. Stop loaded LaunchAgents with `launchctl bootout`; do not merely kill their
   processes because launchd may immediately restart them. Stop an authorized
   detached server only by matching its complete documented command, recording
   its single PID, and sending that PID `SIGTERM`. Never use broad `pkill`,
   `killall`, or partial process-name matching.
5. Preserve exactly one complete rollback app using the path in the private
   guide, then replace the installed test app with the staged bundle.
6. Clear extended attributes. Sign every nested Mach-O file and dynamic
   library first, then sign the outer app bundle. Do not rely on deprecated
   `codesign --deep` signing behavior.
7. Run full strict codesign verification. If it fails, do not start services
   from the new bundle; restore the rollback app and verify it instead.
8. Restart only targets that were running before deployment. Use their plist
   files and `launchctl bootstrap` for LaunchAgents. Use the exact documented
   command, root, and log redirection for an authorized detached server.
9. Verify each expected service, process, listening port, and recent log. A
   successful copy or `launchctl` exit status alone is not enough.

Never stop or restart unrelated GUI instances, unlisted SDR++ servers, or
services not listed in the private guide unless the user explicitly broadens
the deployment scope.

If any step after service shutdown fails, prioritize restoring the known-good
app and the prior service state before investigating the new build.
