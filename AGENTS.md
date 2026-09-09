# Backplane KiCad

- Use `gpt-5.6-luna` subagents for most bounded implementation, investigation, and review tasks. The primary agent integrates and verifies the result.
- New commits must use author and committer `i2cjak <build@amemb.com>`. Verify `git var GIT_AUTHOR_IDENT` and `git var GIT_COMMITTER_IDENT` before committing.
- Before every push, verify the authenticated account is `i2cjak`, the destination is the public `i2cjak/Backplane_KiCad` repository, and review the outgoing commits, diff, and relevant checks. Preserve upstream commit identities and history.
- Base changes on stable KiCad releases. Keep IPC backports focused, record their upstream revisions, and verify both schematic and PCB behavior. Never label an untested backport a working runtime.
- Preserve file compatibility with unmodified KiCad 10.0.6. Do not raise native file-format versions or emit newer unsupported tokens. Store fork-only metadata in adjacent `.backplane.json` companions, preserve those companions through save/copy/export staging, and verify native save/reopen with stock 10.0.6 before releasing.
- This repository and corresponding source for distributed binaries must remain public. Preserve upstream licenses, authorship, and third-party notices. Ship licenses and matching source with release binaries.
- Coordinate the runtime archive layout and supported platforms with the sibling Backplane app. Validate the packaged runtime on a host without a separate KiCad installation.
- Never add OpenAI copyright or ownership notices.
