/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*           Xavier Leroy, projet Cristal, INRIA Rocquencourt          */
/*                                                                     */
/*  Copyright 2001 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../LICENSE.     */
/*                                                                     */
/***********************************************************************/

/* Registration of global memory roots */

#ifndef CAML_GLOBROOTS_H
#define CAML_GLOBROOTS_H

#include "mlvalues.h"
#include "roots.h"
#include "memory.h"

void caml_scan_global_roots(scanning_action f, void*);

void caml_cleanup_deleted_roots(void);

#ifdef NATIVE_CODE
void caml_register_dyn_global(void *v);
#endif

#endif /* CAML_GLOBROOTS_H */
