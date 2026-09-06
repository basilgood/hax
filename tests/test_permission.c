/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "harness.h"
#include "permission.h"
#include "xalloc.h"

static void test_contains_inside(void)
{
    char *ws = t_tempdir();
    char *sub = xasprintf("%s/sub", ws);
    mkdir(sub, 0700);
    char *file = xasprintf("%s/a.txt", ws);
    FILE *f = fopen(file, "w");
    fclose(f);
    char *fresh = xasprintf("%s/new.txt", ws);

    struct permission perm;
    permission_init(&perm, ws);
    EXPECT(permission_contains(&perm, ws));
    EXPECT(permission_contains(&perm, sub));
    EXPECT(permission_contains(&perm, file));
    /* A not-yet-existing file resolves through its existing parent. */
    EXPECT(permission_contains(&perm, fresh));
    permission_free(&perm);

    free(sub);
    free(file);
    free(fresh);
}

static void test_contains_outside(void)
{
    char *ws = t_tempdir();
    char *sibling = t_tempdir();
    char *sibling_file = xasprintf("%s/b.txt", sibling);
    FILE *f = fopen(sibling_file, "w");
    fclose(f);
    char *sibling_new = xasprintf("%s/nope.txt", sibling);
    /* A name sharing the workspace prefix is not inside it. */
    char *prefix_twin = xasprintf("%s_other", ws);
    mkdir(prefix_twin, 0700);

    struct permission perm;
    permission_init(&perm, ws);
    EXPECT(!permission_contains(&perm, sibling));
    EXPECT(!permission_contains(&perm, sibling_file));
    EXPECT(!permission_contains(&perm, sibling_new));
    EXPECT(!permission_contains(&perm, prefix_twin));
    permission_free(&perm);

    free(sibling_file);
    free(sibling_new);
    free(prefix_twin);
}

static void test_contains_symlink(void)
{
    char *ws = t_tempdir();
    char *outside = t_tempdir();
    char *link = xasprintf("%s/link", ws);
    EXPECT(symlink(outside, link) == 0);

    struct permission perm;
    permission_init(&perm, ws);
    /* realpath sees through the symlink: the target is outside. */
    EXPECT(!permission_contains(&perm, link));
    permission_free(&perm);

    free(link);
}

static void test_contains_relative(void)
{
    char *ws = t_tempdir();
    char *sub = xasprintf("%s/sub", ws);
    mkdir(sub, 0700);

    char saved[PATH_MAX];
    if (!getcwd(saved, sizeof(saved)))
        T_SKIP("cannot save cwd");
    if (chdir(ws) != 0)
        T_SKIP("cannot chdir into fixture");

    struct permission perm;
    permission_init(&perm, ws);
    EXPECT(permission_contains(&perm, "sub"));
    EXPECT(permission_contains(&perm, "sub/new.txt"));
    EXPECT(!permission_contains(&perm, "../outside"));
    EXPECT(!permission_contains(&perm, "/etc/passwd"));
    permission_free(&perm);

    EXPECT(chdir(saved) == 0);
    free(sub);
}

static void test_special(void)
{
    char *ws = t_tempdir();
    struct permission perm;
    permission_init(&perm, ws);

    EXPECT(permission_contains(&perm, "/dev/null"));
    EXPECT(permission_contains(&perm, "/dev/zero"));
    EXPECT(permission_contains(&perm, "/dev/urandom"));
    EXPECT(permission_contains(&perm, "/dev/tty"));
    EXPECT(permission_contains(&perm, "/dev/fd/0"));
    EXPECT(permission_contains(&perm, "/proc/self/fd/1"));
    /* Real devices and storage are not special-cased. */
    EXPECT(!permission_contains(&perm, "/dev/sda"));
    EXPECT(!permission_contains(&perm, "/dev/pts/0"));
    EXPECT(!permission_contains(&perm, "/dev/shm"));

    permission_free(&perm);
    free(ws);
}

static void expect_paths(const char *command, const char *const *want, size_t n_want)
{
    char **paths = permission_bash_paths(command);
    if (n_want == 0) {
        EXPECT(paths == NULL);
        return;
    }
    EXPECT(paths != NULL);
    if (!paths)
        return;
    size_t n = 0;
    while (paths[n])
        n++;
    if (n != n_want) {
        EXPECT(n == n_want);
        string_array_free(paths);
        return;
    }
    for (size_t i = 0; i < n; i++)
        EXPECT_STR_EQ(paths[i], want[i]);
    string_array_free(paths);
}

static void test_bash_paths(void)
{
    static const char *const append[] = {"/tmp/log"};
    expect_paths("echo x >> /tmp/log", append, 1);
    static const char *const input[] = {"/etc/passwd"};
    expect_paths("cat < /etc/passwd", input, 1);
    static const char *const glued_redir[] = {"/tmp/x"};
    expect_paths("echo a>/tmp/x", glued_redir, 1);
    static const char *const fd_redir[] = {"/dev/null"};
    expect_paths("ls 2>/dev/null", fd_redir, 1);
    static const char *const quoted_redir[] = {"/tmp/a b"};
    expect_paths("echo x >\"/tmp/a b\"", quoted_redir, 1);
    static const char *const absolute[] = {"/etc"};
    expect_paths("ls /etc", absolute, 1);
    static const char *const home[] = {"~/.ssh/id_rsa"};
    expect_paths("cat ~/.ssh/id_rsa", home, 1);
    static const char *const dotdot[] = {"../other"};
    expect_paths("cd ../other && make", dotdot, 1);
    static const char *const curl[] = {"/tmp/out"};
    expect_paths("curl -o /tmp/out https://example.com", curl, 1);
    static const char *const curl_dir[] = {"/tmp/d"};
    expect_paths("curl --output-dir /tmp/d -O https://example.com", curl_dir, 1);
    expect_paths("curl -o/tmp/out https://example.com", curl, 1);
    static const char *const tar[] = {"/tmp/a.tar", "/tmp/dest"};
    expect_paths("tar -xvf /tmp/a.tar -C /tmp/dest", tar, 2);
    static const char *const tar_glued[] = {"/tmp/a.tar"};
    expect_paths("tar -xvf/tmp/a.tar", tar_glued, 1);
    static const char *const git[] = {"/tmp/repo"};
    expect_paths("git -C /tmp/repo status", git, 1);
    static const char *const dd[] = {"/dev/zero", "/tmp/out"};
    expect_paths("dd if=/dev/zero of=/tmp/out bs=1M", dd, 2);
    static const char *const sudo[] = {"/etc/passwd"};
    expect_paths("sudo rm /etc/passwd", sudo, 1);
    static const char *const env[] = {"/tmp"};
    expect_paths("env FOO=bar ls /tmp", env, 1);
    static const char *const assign[] = {"/tmp/x"};
    expect_paths("FOO=bar cmd /tmp/x", assign, 1);
    static const char *const unzip[] = {"/tmp/x"};
    expect_paths("unzip -d /tmp/x file.zip", unzip, 1);
    static const char *const make[] = {"/tmp"};
    expect_paths("make -C /tmp build", make, 1);
    static const char *const cmake[] = {"/tmp/src", "/tmp/build"};
    expect_paths("cmake -S /tmp/src -B /tmp/build", cmake, 2);
    static const char *const install[] = {"/tmp/dir"};
    expect_paths("install -t /tmp/dir file", install, 1);
    static const char *const wget[] = {"/tmp/out"};
    expect_paths("wget -O /tmp/out https://example.com", wget, 1);

    /* Shell metacharacters separate words: no glued `;` on redirect targets. */
    static const char *const sep_semicolon[] = {"/tmp/y"};
    expect_paths("echo x > /tmp/y; ls", sep_semicolon, 1);
    static const char *const sep_glued[] = {"/tmp/x"};
    expect_paths("echo a>/tmp/x;ls", sep_glued, 1);
    static const char *const sep_pipe[] = {"/etc/passwd"};
    expect_paths("cmd | cat /etc/passwd", sep_pipe, 1);
    static const char *const sep_and[] = {"/tmp/x"};
    expect_paths("cmd && rm /tmp/x", sep_and, 1);
    static const char *const sep_paren[] = {"/tmp"};
    expect_paths("(cd /tmp && make)", sep_paren, 1);
    static const char *const devnull[] = {"/dev/null"};
    expect_paths("rg foo 2>/dev/null; ls", devnull, 1);
    expect_paths("echo x 2>&1", NULL, 0);

    /* A relative redirect target resolves against the workspace and is not extracted. */
    expect_paths("echo marker42 > out.txt", NULL, 0);
    expect_paths("echo \"a > /tmp/x\"", NULL, 0);
    expect_paths("echo hello", NULL, 0);
    expect_paths("ls", NULL, 0);
    expect_paths("", NULL, 0);
    expect_paths(NULL, NULL, 0);
}

static void test_gate(void)
{
    char *ws = t_tempdir();
    char *outside = t_tempdir();
    char *outside_file = xasprintf("%s/x.txt", outside);
    FILE *f = fopen(outside_file, "w");
    fclose(f);
    char *inside = xasprintf("%s/a.txt", ws);

    struct permission perm;
    permission_init(&perm, ws);

    char *inside_args = xasprintf("{\"path\":\"%s\"}", inside);
    char *outside_args = xasprintf("{\"path\":\"%s\"}", outside_file);
    EXPECT(permission_gate(&perm, "read", inside_args) == NULL);
    char *got = permission_gate(&perm, "read", outside_args);
    EXPECT(got != NULL);
    if (got)
        EXPECT_STR_EQ(got, outside_file);
    free(got);

    char *bash_inside = xasprintf("{\"command\":\"echo hi > %s\"}", inside);
    char *bash_outside = xasprintf("{\"command\":\"echo hi > %s\"}", outside_file);
    EXPECT(permission_gate(&perm, "bash", bash_inside) == NULL);
    got = permission_gate(&perm, "bash", bash_outside);
    EXPECT(got != NULL);
    if (got)
        EXPECT_STR_EQ(got, outside_file);
    free(got);

    /* Tools without a path argument are never gated. */
    EXPECT(permission_gate(&perm, "task_wait", "{}") == NULL);
    EXPECT(permission_gate(&perm, "read", NULL) == NULL);
    EXPECT(permission_gate(&perm, "read", "{not json") == NULL);

    /* Special files never gate, even behind a command separator. */
    EXPECT(permission_gate(&perm, "bash", "{\"command\":\"ls 2>/dev/null\"}") == NULL);
    EXPECT(permission_gate(&perm, "bash", "{\"command\":\"rg foo 2>/dev/null; ls\"}") == NULL);
    EXPECT(permission_gate(&perm, "read", "{\"path\":\"/dev/null\"}") == NULL);

    permission_free(&perm);
    free(inside);
    free(inside_args);
    free(outside_args);
    free(bash_inside);
    free(bash_outside);
    free(outside_file);
}

static void test_approval_dir(void)
{
    char *ws = t_tempdir();
    char *sub = xasprintf("%s/sub", ws);
    mkdir(sub, 0700);
    char *file = xasprintf("%s/a.txt", ws);
    FILE *f = fopen(file, "w");
    fclose(f);
    char *fresh = xasprintf("%s/new.txt", ws);

    char *dir = permission_approval_dir(sub);
    EXPECT_STR_EQ(dir, sub);
    free(dir);

    dir = permission_approval_dir(file);
    EXPECT_STR_EQ(dir, ws);
    free(dir);

    dir = permission_approval_dir(fresh);
    EXPECT_STR_EQ(dir, ws);
    free(dir);

    dir = permission_approval_dir("/");
    EXPECT_STR_EQ(dir, "/");
    free(dir);

    free(sub);
    free(file);
    free(fresh);
}

static void test_add_dedupe(void)
{
    char *ws = t_tempdir();
    char *other = t_tempdir();

    struct permission perm;
    permission_init(&perm, ws);
    permission_add(&perm, ws);
    EXPECT(perm.n_workspaces == 1);
    permission_add(&perm, other);
    EXPECT(perm.n_workspaces == 2);
    permission_add(&perm, other);
    EXPECT(perm.n_workspaces == 2);
    EXPECT(permission_contains(&perm, other));
    permission_free(&perm);
}

int main(void)
{
    test_contains_inside();
    test_contains_outside();
    test_contains_symlink();
    test_contains_relative();
    test_special();
    test_bash_paths();
    test_gate();
    test_approval_dir();
    test_add_dedupe();
    T_REPORT();
}
