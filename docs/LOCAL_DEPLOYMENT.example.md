# Local SDR++ Deployment Guide Template

Copy this template to `../LOCAL_DEPLOYMENT.md` relative to the repository root
and fill it in locally. The resulting file is outside the Git repository and
must not be committed.

## Target

- SSH target: `<user>@<host>`
- launchd domain: `gui/<uid>`
- Installed application: `/Applications/<test-app>.app`
- Single rollback application: `/Applications/<test-app>.previous.app`
- Remote staging path: `/private/tmp/<test-app>.new.app`

## Managed services

List only services that this deployment is authorized to stop and restart.

| Purpose | Label | Plist | Port | stdout | stderr |
| --- | --- | --- | --- | --- | --- |
| Server 1 | `<label>` | `~/Library/LaunchAgents/<label>.plist` | `<port>` | `<path>` | `<path>` |

Also list any SDR++ processes or services that must explicitly remain
untouched.

## Required procedure

1. Build and validate the full app locally.
2. Copy the new bundle to the remote staging path.
3. Record which authorized services are loaded.
4. Boot out only loaded authorized services.
5. Replace the single rollback copy with the current known-good app.
6. Move the staged app into the installed location.
7. Clear extended attributes and sign nested Mach-O files before the bundle.
8. Verify the installed bundle with strict codesign verification.
9. Bootstrap only the services that were previously loaded.
10. Verify service state, listening ports, processes, and logs.
11. On failure, restore the rollback app and prior service state.

Document any host-specific permission requirements, but do not add passwords,
private keys, access tokens, or other secrets to this file.
