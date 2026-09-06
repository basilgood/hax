# Workspace permissions (draft)

Design draft for a session-scoped file-system permission feature. Inspired by zaly's
permission manager (`packages/agent/src/permissions/` in the zaly repo) and the
mcp-filesystem-server path validation; deliberately much smaller than either.

## Goal

The agent is free inside the project it was launched in. Any access outside — read or
write, through any tool — requires the user's approval for that session. The user keeps
full rights in every shell; the sandbox only constrains the agent.

Threat model: mistakes, not malice. The user works in git worktrees, so a destructive
mistake inside the workspace is recoverable (recreate the worktree). The feature protects
the irremediable stuff: home directories, other projects, system files.

## Model

One in-memory, session-scoped list:

```
workspaces = [project dir]  (+ approved dirs, session-only)
```

- The project dir is the worktree root, auto-detected at session start
  (`git rev-parse --show-toplevel`; handles worktrees via their `.git` file; fallback to
  cwd outside a repo).
- Approvals add the **containing directory** of the requested path — never a single file
  (a file-only grant is useless: listing the dir or reading a sibling re-asks) and never
  a parent (the user approves the project, not its surroundings).
- An approved workspace has exactly the same permissions as the main worktree: full
  read/write. No read-only tier, no per-workspace rules.
- Harmless special files (`/dev/null`, `/dev/zero`, `/dev/random`, `/dev/urandom`, `/dev/tty`,
  `/dev/full`, `/dev/stdin`, `/dev/stdout`, `/dev/stderr`, `/dev/fd/*`, `/proc/self/fd/*`) are
  always allowed: they are redirection sinks and sources, not data. Real devices (`/dev/sda`),
  terminals (`/dev/pts/*`), and storage (`/dev/shm`) are not special-cased.
- Nothing persists. Approvals die with the session; the next session starts with just the
  project dir. No config schema changes.

## Tools

- **read / edit / write** — deterministic: the tool takes a path argument. Resolve it
  (absolute, clean, separator-normalized containment, realpath with parent fallback for
  new files, per mcp-filesystem-server `validatePath`). Inside a workspace → free.
  Outside → ask.
- **bash** — extract path-like tokens from the command: redirect targets (`>`, `>>`,
  `<`), absolute paths, `~/...`, `../...`, plus a small per-command spec table for
  common tools (zaly's `TOOLS[cmd].reads/writes(args)`). Same containment check, same
  ask. Commands with no extractable paths operate on cwd (the workspace) → free.

## Ask flow

One picker, same for every tool:

```
Allow / Deny / Add workspace (the containing dir)
```

"Add workspace" appends the containing directory to the session workspace list and the
call proceeds. Deny returns a recoverable error to the model.

Headless runs (no TTY) are not gated: nobody is present to answer, so they keep hax's
original behavior. The gate is an interactive protection.

## Known limitations

- Bash path extraction is heuristic: `$VAR`-expanded paths and command substitution
  (`cat "$HOME/.ssh/id_rsa"`, `$(cat /etc/passwd)`) are not extracted and not gated.
  Acceptable for the mistake threat model — a deliberate `$HOME` read is not a mistake.
  (zaly lives with the same gap.)
- Containment is lexical; a symlink inside a workspace pointing outside is treated as
  inside unless the realpath check is applied. Keep the realpath check.
- No sensitive-file deny list (zaly's `.env`/`.ssh` patterns). Out of scope for the
  mistake model; can be added later as a small deny list.

## Deliberately cut

- Command rules and pattern grammar (`Bash(git push:*)`, `read(/src/**)`) — the gate is
  location, not intent.
- Presets (strict/readonly/permissive/yolo) — hax's current behavior is the only mode.
- OS-level sandboxing (landlock) — deterministic but heavy; the heuristic gate is
  proportionate to the threat model.
- Persistence of approvals (zaly's FIXME) — the user wants session-scoped access only.
- Read/write tiering — an approved workspace is fully trusted, like the main worktree.

## Prompt

One sentence added to the system prompt: "File access outside the current workspace
requires user approval; hax will ask." The git-safety paragraph stays as-is — it governs
git behavior, not file access, so there is no contradiction.

## Implementation sketch

- `src/permission.{c,h}` — pure, terminal-free, unit-testable: workspace list,
  containment check, bash path extraction. No I/O.
- Hook in `agent_loop.c` before `tool->run()`: gate read/edit/write (path argument) and
  bash (extracted paths).
- Ask flow via the existing picker UI; "Add workspace" mutates the session list.
- Tests: containment and extraction are pure → unit tests without a TTY; ask flow → e2e
  with the mock provider.
