/* SPDX-License-Identifier: MIT */
#include "permission_ask.h"

#include <stdlib.h>
#include <unistd.h>

#include "permission.h"
#include "render/spinner.h"
#include "terminal/interrupt.h"
#include "terminal/picker.h"

int permission_ask(struct permission *perm, const char *path, struct spinner *spinner)
{
    /* Headless runs keep hax's original behavior: nobody is present to answer, so nothing is
     * gated. The gate is an interactive protection. */
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return 1;

    char *approval_dir = permission_approval_dir(path);
    if (!approval_dir)
        return 0;

    struct picker_item items[] = {
        {.label = "Allow", .detail = path, .description = "Allow this one call only."},
        {.label = "Deny",
         .detail = path,
         .description = "Return a recoverable error to the agent."},
        {.label = "Add workspace",
         .detail = approval_dir,
         .description = "Allow all access under this directory for the rest of the session."},
    };
    struct picker_opts opts = {
        .title = "allow access outside the workspace?",
        .items = items,
        .item_count = sizeof(items) / sizeof(items[0]),
    };

    /* The picker owns the terminal: release the spinner and the interrupt watcher for its
     * duration, then restore both. All calls are no-ops when inactive. */
    interrupt_resolve_pending_escape();
    interrupt_disarm();
    spinner_hide(spinner);
    long choice = picker_run(&opts);
    spinner_show(spinner);
    interrupt_clear_requests();
    interrupt_arm();

    int allowed = 0;
    if (choice == 0) {
        allowed = 1;
    } else if (choice == 2) {
        permission_add(perm, approval_dir);
        allowed = 1;
    }
    free(approval_dir);
    return allowed;
}
