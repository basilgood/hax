/* SPDX-License-Identifier: MIT */
#include "permission.h"

#include <ctype.h>
#include <jansson.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "buf.h"
#include "xalloc.h"
#include "system/path.h"

/* Session-scoped file-access gating. Pure and terminal-free: realpath() is the only filesystem
 * access — the containment check must see through symlinks, and the parent fallback resolves
 * new files about to be written. */

/* --- path resolution --- */

/* Expand `~` and join relative paths against the current directory. */
static char *absolute_path(const char *path)
{
    char *expanded = path_expand_home(path);
    if (!expanded)
        return NULL;
    if (expanded[0] == '/')
        return expanded;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        free(expanded);
        return NULL;
    }
    char *joined = path_join(cwd, expanded);
    free(expanded);
    return joined;
}

/* Resolve an absolute path to a symlink-free form. When the path itself does not exist — a new
 * file about to be written — walk up to the nearest existing ancestor, resolve that, and
 * re-append the missing suffix. */
static char *resolve_path(const char *path)
{
    char *resolved = realpath(path, NULL);
    if (resolved)
        return resolved;

    char *suffix = xstrdup(path);
    char *cursor = suffix + strlen(suffix);
    while (cursor > suffix) {
        cursor = strrchr(suffix, '/');
        if (!cursor)
            break;
        if (cursor == suffix) {
            /* Root is the nearest existing ancestor. */
            char *root = realpath("/", NULL);
            if (!root)
                break;
            char *result =
                strcmp(root, "/") == 0 ? xstrdup(suffix) : xasprintf("%s%s", root, suffix);
            free(root);
            free(suffix);
            return result;
        }
        *cursor = '\0';
        resolved = realpath(suffix, NULL);
        if (resolved) {
            char *result = xasprintf("%s/%s", resolved, cursor + 1);
            free(resolved);
            free(suffix);
            return result;
        }
    }
    free(suffix);
    return NULL;
}

/* --- workspace list --- */

void permission_init(struct permission *perm, const char *project_dir)
{
    memset(perm, 0, sizeof(*perm));
    if (project_dir && *project_dir)
        permission_add(perm, project_dir);
}

void permission_free(struct permission *perm)
{
    for (size_t i = 0; i < perm->n_workspaces; i++)
        free(perm->workspaces[i]);
    free(perm->workspaces);
    memset(perm, 0, sizeof(*perm));
}

void permission_add(struct permission *perm, const char *dir)
{
    if (!perm || !dir || !*dir)
        return;
    char *resolved = realpath(dir, NULL);
    if (!resolved)
        resolved = xstrdup(dir);
    for (size_t i = 0; i < perm->n_workspaces; i++) {
        if (strcmp(perm->workspaces[i], resolved) == 0) {
            free(resolved);
            return;
        }
    }
    if (perm->n_workspaces == perm->cap_workspaces) {
        perm->cap_workspaces = perm->cap_workspaces ? perm->cap_workspaces * 2 : 4;
        perm->workspaces =
            xrealloc(perm->workspaces, perm->cap_workspaces * sizeof(*perm->workspaces));
    }
    perm->workspaces[perm->n_workspaces++] = resolved;
}

/* --- special files --- */

/* Harmless special files — redirection sinks and sources, never data — are always allowed. */
static const char *const SPECIAL_PATHS[] = {
    "/dev/null", "/dev/zero",  "/dev/random", "/dev/urandom", "/dev/tty",
    "/dev/full", "/dev/stdin", "/dev/stdout", "/dev/stderr",
};

/* Prefixes: any file descriptor of this process. */
static const char *const SPECIAL_PREFIXES[] = {
    "/dev/fd/",
    "/proc/self/fd/",
};

static int is_special_path(const char *path)
{
    for (size_t i = 0; i < sizeof(SPECIAL_PATHS) / sizeof(SPECIAL_PATHS[0]); i++)
        if (strcmp(path, SPECIAL_PATHS[i]) == 0)
            return 1;
    for (size_t i = 0; i < sizeof(SPECIAL_PREFIXES) / sizeof(SPECIAL_PREFIXES[0]); i++)
        if (strncmp(path, SPECIAL_PREFIXES[i], strlen(SPECIAL_PREFIXES[i])) == 0)
            return 1;
    return 0;
}

int permission_contains(const struct permission *perm, const char *path)
{
    if (!perm || !path)
        return 0;
    char *absolute = absolute_path(path);
    if (!absolute)
        return 0;
    if (is_special_path(absolute)) {
        free(absolute);
        return 1;
    }
    char *resolved = resolve_path(absolute);
    free(absolute);
    if (!resolved)
        return 0;
    int inside = 0;
    for (size_t i = 0; i < perm->n_workspaces; i++) {
        const char *workspace = perm->workspaces[i];
        size_t workspace_len = strlen(workspace);
        if (strncmp(resolved, workspace, workspace_len) == 0 &&
            (resolved[workspace_len] == '\0' || resolved[workspace_len] == '/')) {
            inside = 1;
            break;
        }
    }
    free(resolved);
    return inside;
}

/* --- bash path extraction --- */

static void token_push(char ***tokens, size_t *n, size_t *cap, struct buf *text)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *tokens = xrealloc(*tokens, *cap * sizeof(**tokens));
    }
    (*tokens)[(*n)++] = buf_steal(text);
}

/* Split a command into shell words, stripping quotes and backslash escapes. Shell
 * metacharacters (`;|&()<>`) separate words, so a redirect target is its own token. */
static char **tokenize(const char *command, size_t *n_out)
{
    char **tokens = NULL;
    size_t n = 0, cap = 0;
    struct buf text;
    buf_init(&text);

    for (const char *p = command; *p;) {
        if (isspace((unsigned char)*p) || strchr(";|&()<>", *p)) {
            if (text.len)
                token_push(&tokens, &n, &cap, &text);
            p++;
            continue;
        }
        if (*p == '\'') {
            p++;
            while (*p && *p != '\'') {
                buf_append(&text, p, 1);
                p++;
            }
            if (*p == '\'')
                p++;
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')) {
                    buf_append(&text, p + 1, 1);
                    p += 2;
                } else {
                    buf_append(&text, p, 1);
                    p++;
                }
            }
            if (*p == '"')
                p++;
        } else if (*p == '\\') {
            if (p[1]) {
                buf_append(&text, p + 1, 1);
                p += 2;
            } else {
                p++;
            }
        } else {
            buf_append(&text, p, 1);
            p++;
        }
    }
    if (text.len)
        token_push(&tokens, &n, &cap, &text);
    buf_free(&text);
    *n_out = n;
    return tokens;
}

/* Per-command option table: which single-dash option letters, long options, and key=value
 * assignments take a path value. */
struct bash_spec {
    const char *name;
    const char *path_opts;  /* single-dash letters, e.g. "oT" for curl -o/-T */
    const char *path_lopts; /* space-separated long option names */
    const char *path_keys;  /* space-separated key=value names */
};

static const struct bash_spec BASH_SPECS[] = {
    {"curl", "oT", "output output-dir upload-file", NULL},
    {"wget", "OPi", "output-document directory-prefix input-file", NULL},
    {"tar", "CfT", "directory file files-from", NULL},
    {"unzip", "d", NULL, NULL},
    {"git", "C", "git-dir work-tree", NULL},
    {"make", "Cf", "directory file makefile", NULL},
    {"cmake", "SB", "source build", NULL},
    {"dd", NULL, NULL, "if of"},
    {"install", "t", "target-directory", NULL},
};

static const struct bash_spec *find_spec(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(BASH_SPECS) / sizeof(BASH_SPECS[0]); i++)
        if (strcmp(BASH_SPECS[i].name, name) == 0)
            return &BASH_SPECS[i];
    return NULL;
}

/* Leading wrappers and environment assignments do not name the command. */
static const char *const WRAPPER_COMMANDS[] = {
    "sudo", "doas", "env", "nohup", "time", "command", "nice", "exec",
};

static const char *command_name(char **tokens, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *word = tokens[i];
        if (!*word)
            continue;
        int wrapper = 0;
        for (size_t w = 0; w < sizeof(WRAPPER_COMMANDS) / sizeof(WRAPPER_COMMANDS[0]); w++)
            if (strcmp(word, WRAPPER_COMMANDS[w]) == 0) {
                wrapper = 1;
                break;
            }
        if (wrapper)
            continue;
        if (strchr(word, '=') && word[0] != '-')
            continue; /* env NAME=value assignment */
        return word;
    }
    return NULL;
}

/* Whether `key` (of length key_len) is one of the space-separated words in `list`. */
static int key_in_list(const char *list, const char *key, size_t key_len)
{
    for (const char *p = list;;) {
        while (*p == ' ')
            p++;
        if (!*p)
            return 0;
        const char *end = strchr(p, ' ');
        size_t word_len = end ? (size_t)(end - p) : strlen(p);
        if (word_len == key_len && strncmp(p, key, key_len) == 0)
            return 1;
        if (!end)
            return 0;
        p = end;
    }
}

static void add_path(char ***paths, size_t *n, size_t *cap, const char *path)
{
    if (!path || !*path)
        return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *paths = xrealloc(*paths, *cap * sizeof(**paths));
    }
    (*paths)[(*n)++] = xstrdup(path);
}

static void extract_word(char ***paths, size_t *n, size_t *cap, const char *word,
                         const struct bash_spec *spec)
{
    if (!*word)
        return;
    if (word[0] == '/' || word[0] == '~' || strncmp(word, "..", 2) == 0) {
        add_path(paths, n, cap, word);
        return;
    }
    if (word[0] == '-') {
        if (word[1] == '-') {
            /* --long=value */
            const char *eq = strchr(word, '=');
            if (spec && spec->path_lopts && eq && eq > word + 2 &&
                key_in_list(spec->path_lopts, word + 2, (size_t)(eq - word - 2)))
                add_path(paths, n, cap, eq + 1);
        } else if (spec && spec->path_opts) {
            /* -xvf/tmp/a.tar: the first option letter taking a path consumes the rest. */
            for (const char *p = word + 1; *p; p++) {
                if (strchr(spec->path_opts, *p)) {
                    if (p[1])
                        add_path(paths, n, cap, p + 1);
                    break;
                }
            }
        }
        return;
    }
    if (spec && spec->path_keys) {
        const char *eq = strchr(word, '=');
        if (eq && eq > word && key_in_list(spec->path_keys, word, (size_t)(eq - word)))
            add_path(paths, n, cap, eq + 1);
    }
}

char **permission_bash_paths(const char *command)
{
    if (!command)
        return NULL;
    size_t n_tokens = 0;
    char **tokens = tokenize(command, &n_tokens);
    if (!tokens)
        return NULL;
    const struct bash_spec *spec = find_spec(command_name(tokens, n_tokens));

    char **paths = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < n_tokens; i++)
        extract_word(&paths, &n, &cap, tokens[i], spec);
    for (size_t i = 0; i < n_tokens; i++)
        free(tokens[i]);
    free(tokens);
    if (n == 0)
        return NULL;
    paths = xrealloc(paths, (n + 1) * sizeof(*paths));
    paths[n] = NULL;
    return paths;
}

/* --- gating --- */

char *permission_gate(const struct permission *perm, const char *tool_name, const char *args_json)
{
    if (!perm || !tool_name || !args_json)
        return NULL;
    json_error_t json_error;
    json_t *arguments = json_loads(args_json, 0, &json_error);
    if (!arguments)
        return NULL;

    char *outside = NULL;
    if (strcmp(tool_name, "bash") == 0) {
        const char *command = json_string_value(json_object_get(arguments, "command"));
        if (command) {
            char **paths = permission_bash_paths(command);
            if (paths) {
                for (size_t i = 0; paths[i]; i++) {
                    if (!permission_contains(perm, paths[i])) {
                        outside = xstrdup(paths[i]);
                        break;
                    }
                }
                string_array_free(paths);
            }
        }
    } else {
        const char *path = json_string_value(json_object_get(arguments, "path"));
        if (path && !permission_contains(perm, path))
            outside = xstrdup(path);
    }
    json_decref(arguments);
    return outside;
}

char *permission_approval_dir(const char *path)
{
    if (!path || !*path)
        return NULL;
    char *absolute = absolute_path(path);
    if (!absolute)
        return NULL;
    char *resolved = resolve_path(absolute);
    free(absolute);
    if (!resolved)
        return NULL;
    struct stat st;
    if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode))
        return resolved;
    char *slash = strrchr(resolved, '/');
    if (slash && slash != resolved) {
        *slash = '\0';
        return resolved;
    }
    free(resolved);
    return xstrdup("/");
}

char *permission_denied_message(const char *path)
{
    return xasprintf("permission denied: %s is outside the workspace", path);
}
