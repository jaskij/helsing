// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2021 Pierro Zachareas
 */

#ifndef HELSING_VARGS_H
#define HELSING_VARGS_H

#include "configuration.h"
#include "llhandle.h"
#include "bthandle.h"
#include "cache.h"

struct vargs /* Vampire arguments */
{
	struct cache *digptr;
	struct bthandle *thandle;
	struct llhandle *lhandle;
	vamp_t local_count;
};

void vargs_new(struct vargs **ptr, struct cache *digptr);
void vargs_free(struct vargs *args);
void vargs_reset(struct vargs *args);
struct llhandle *vargs_getlhandle(struct vargs *args);
void vampire(vamp_t min, vamp_t max, struct vargs *args, fang_t fmax);

#ifdef PROCESS_RESULTS
void vargs_btnode_cleanup(struct vargs *args, vamp_t number);
static inline void vargs_set_lc(struct vargs *ptr)
{
	ptr->local_count = ptr->lhandle->size;
}
#else /* PROCESS_RESULTS */
static inline void vargs_btnode_cleanup(
	__attribute__((unused)) struct vargs *args,
	__attribute__((unused)) vamp_t number)
{
}
static inline void vargs_set_lc(__attribute__((unused)) struct vargs *ptr)
{
}
#endif /* PROCESS_RESULTS */

#if defined COUNT_RESULTS ||  defined DUMP_RESULTS
static inline void vargs_iterate_local_count(struct vargs *ptr)
{
	ptr->local_count += 1;
}
#else /* defined COUNT_RESULTS ||  defined DUMP_RESULTS */
static inline void vargs_iterate_local_count(
	__attribute__((unused)) struct vargs *ptr)
{
}
#endif /* defined COUNT_RESULTS ||  defined DUMP_RESULTS */

#ifdef DUMP_RESULTS
static inline void vargs_print_results(
	vamp_t product,
	fang_t multiplier,
	fang_t multiplicand)
{
	printf("%llu = %lu x %lu\n", product, multiplier, multiplicand);
}
#else /* DUMP_RESULTS */
static inline void vargs_print_results(
	__attribute__((unused)) vamp_t product,
	__attribute__((unused)) fang_t multiplier,
	__attribute__((unused)) fang_t multiplicand)
{
}
#endif /* DUMP_RESULTS */
#endif /* HELSING_VARGS_H */