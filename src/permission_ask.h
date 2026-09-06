/* SPDX-License-Identifier: MIT */
#ifndef HAX_PERMISSION_ASK_H
#define HAX_PERMISSION_ASK_H

#include "permission.h"
#include "render/spinner.h"

/* Ask the user whether the agent may touch `path` outside the workspace. Returns 1 when the call
 * may proceed — Allow, or Add workspace, which also grants the containing directory for the rest
 * of the session — and 0 when it is denied (Deny or cancel). Headless runs (no TTY) return 1
 * without asking: nobody is present, so they keep hax's original ungated behavior. */
int permission_ask(struct permission *perm, const char *path, struct spinner *spinner);

#endif /* HAX_PERMISSION_ASK_H */
