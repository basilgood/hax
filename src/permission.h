/* SPDX-License-Identifier: MIT */
#ifndef HAX_PERMISSION_H
#define HAX_PERMISSION_H

#include <stddef.h>

/* Session-scoped file-access gating: a list of workspace directories the agent may touch
 * freely; anything outside requires user approval. Pure and terminal-free — no picker, no
 * subprocesses. realpath() is the only filesystem access: the containment check must see
 * through symlinks, and the doc's parent fallback resolves new files. */

struct permission {
    char **workspaces; /* owned, absolute, symlink-free */
    size_t n_workspaces;
    size_t cap_workspaces;
};

/* Start with `project_dir` as the only workspace. */
void permission_init(struct permission *perm, const char *project_dir);
void permission_free(struct permission *perm);

/* Append `dir` as an approved workspace, deduplicated. */
void permission_add(struct permission *perm, const char *dir);

/* Return 1 when `path` resolves inside a workspace, or names a harmless special file
 * (/dev/null and friends) that never needs approval. */
int permission_contains(const struct permission *perm, const char *path);

/* Return the first path in the tool call's arguments that lies outside every workspace, or NULL
 * when the call is free. read/edit/write contribute their "path" argument; bash contributes
 * path-like tokens extracted from the command. The result is owned by the caller. */
char *permission_gate(const struct permission *perm, const char *tool_name, const char *args_json);

/* Extract path-like tokens from a shell command: redirect targets, absolute paths, `~/...`,
 * `../...`, and per-command option values. Returns an owned NULL-terminated array of owned
 * strings. */
char **permission_bash_paths(const char *command);

/* The directory an approval of `path` grants: the path itself when it is an existing directory,
 * otherwise its parent. Owned by the caller. */
char *permission_approval_dir(const char *path);

/* The model-facing error for a denied call. Owned by the caller. */
char *permission_denied_message(const char *path);

#endif /* HAX_PERMISSION_H */
