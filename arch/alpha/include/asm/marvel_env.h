/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ALPHA_MARVEL_ENV_H
#define __ALPHA_MARVEL_ENV_H

/*
 * Ask the console to refresh the environmental readings of the GCT node at
 * @offset from the start of the GCT. Returns the console status, which is 1
 * or -0x4a on success.
 */
long marvel_cfgtree_partial_update(unsigned long offset);

#endif /* __ALPHA_MARVEL_ENV_H */
