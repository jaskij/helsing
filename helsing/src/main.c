// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2021 Pierro Zachareas
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>

#include "configuration.h"
#include "configuration_adv.h"
#include "helper.h"
#include "taskboard.h"
#include "targs_handle.h"
#include "checkpoint.h"
#include "interval.h"
#include "options.h"

static vamp_t get_lmax(vamp_t lmin, vamp_t max)
{
	if (length(lmin) < length(vamp_max)) {
		vamp_t lmax = pow_v(length(lmin)) - 1;
		if (lmax < max)
			return lmax;
	}
	return max;
}

int main(int argc, char *argv[])
{
	vamp_t min = 0, max = 0;
	struct options_t options;
	struct interval_t interval;
	struct taskboard *progress = NULL;

	if (options_init(&options, argc, argv, &min, &max))
		goto out;
	if (interval_set(&interval, min, max))
		goto out;
	if (touch_checkpoint(options, interval))
		goto out;

	taskboard_new(&progress, options);

	if (load_checkpoint(&interval, progress))
		goto out;

	pthread_t *threads = malloc(sizeof(pthread_t) * options.threads);
	struct targs_handle *thhandle = NULL;
	targs_handle_new(&thhandle, options, interval.max, progress);

	for (; interval.complete < interval.max;) {
		vamp_t lmin = get_min(interval.complete + 1,  interval.max);
		vamp_t lmax = get_lmax(lmin, interval.max);
		taskboard_set(progress, lmin, lmax);
		fprintf(stderr, "Checking interval: [%llu, %llu]\n", lmin, lmax);
		for (thread_t thread = 0; thread < options.threads; thread++)
			assert(pthread_create(&threads[thread], NULL, thread_function, (void *)(thhandle->targs[thread])) == 0);
		for (thread_t thread = 0; thread < options.threads; thread++)
			pthread_join(threads[thread], 0);

		interval.complete = lmax;
	}
	targs_handle_print(thhandle);
	targs_handle_free(thhandle);
	free(threads);
out:
	taskboard_free(progress);
	return 0;
}
