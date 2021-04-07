// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2021 Pierro Zachareas
 */

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

#include "configuration.h"
#include "llhandle.h"
#include "tile.h"
#include "matrix.h"
#include "cache.h"
#include "vargs.h"
#include "targs.h"
#include "targs_handle.h"
#include "checkpoint.h"

struct targs_handle *targs_handle_init(vamp_t max)
{
	struct targs_handle *new = malloc(sizeof(struct targs_handle));
	if (new == NULL)
		abort();

	new->mat = matrix_init();
	cache_init(&(new->digptr), max);
	new->counter = 0;
	new->read = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(new->read, NULL);
	new->write = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(new->write, NULL);

	for (thread_t thread = 0; thread < THREADS; thread++)
		new->targs[thread] = targs_t_init(new->read, new->write, new->mat, &(new->counter), new->digptr);
	return new;
}

void targs_handle_free(struct targs_handle *ptr)
{
	if (ptr == NULL)
		return;

	pthread_mutex_destroy(ptr->read);
	free(ptr->read);
	pthread_mutex_destroy(ptr->write);
	free(ptr->write);
	matrix_free(ptr->mat);
	cache_free(ptr->digptr);

	for (thread_t thread = 0; thread < THREADS; thread++)
		targs_t_free(ptr->targs[thread]);

	free(ptr);
}

void targs_handle_print(struct targs_handle *ptr)
{
#if MEASURE_RUNTIME
	double total_time = 0.0;
	fprintf(stderr, "Thread  Runtime Count\n");
	for (thread_t thread = 0; thread<THREADS; thread++) {
		fprintf(stderr, "%u\t%.2lfs\t%llu\n", thread, ptr->targs[thread]->runtime, ptr->targs[thread]->total);
		total_time += ptr->targs[thread]->runtime;
	}
	fprintf(stderr, "\nFang search took: %.2lf s, average: %.2lf s\n", total_time, total_time / THREADS);
#endif

#if defined COUNT_RESULTS ||  defined DUMP_RESULTS
	fprintf(stderr, "Found: %llu valid fang pairs.\n", ptr->counter);
#else
	fprintf(stderr, "Found: %llu vampire numbers.\n", ptr->counter);
#endif
}

void *thread_worker(void *void_args)
{
	struct targs_t *args = (struct targs_t *)void_args;
	thread_timer_start(args);
	struct vargs *vamp_args = vargs_init(args->digptr);
	struct tile *current = NULL;
	bool active = true;

	while (active) {
		active = false;

// Critical section start
		pthread_mutex_lock(args->read);
		if (args->mat->unfinished < args->mat->size) {
			current = args->mat->arr[args->mat->unfinished];
			active = true;
			args->mat->unfinished += 1;
		}
		pthread_mutex_unlock(args->read);
// Critical section end

		if (active) {
			vampire(current->lmin, current->lmax, vamp_args, args->mat->fmax);

// Critical section start
			pthread_mutex_lock(args->write);
#ifdef PROCESS_RESULTS
			current->result = vargs_getlhandle(vamp_args);
			while (
				args->mat->cleanup < args->mat->size &&
				args->mat->arr[args->mat->cleanup]->result != NULL)
			{
				llhandle_print(args->mat->arr[args->mat->cleanup]->result, *(args->count));
				
				*(args->count) += args->mat->arr[args->mat->cleanup]->result->size;
				matrix_progress(args->mat);
				
				save_checkpoint(args->mat->arr[args->mat->cleanup]->lmax, *(args->count));

				tile_free(args->mat->arr[args->mat->cleanup]);
				args->mat->arr[args->mat->cleanup] = NULL;
				args->mat->cleanup += 1;
			}
#else
			*(args->count) += vamp_args->local_count;
#endif
			pthread_mutex_unlock(args->write);
// Critical section end
			vargs_reset(vamp_args);
		}
	}
#if MEASURE_RUNTIME
	args->total += vamp_args->total;
#endif
	vargs_free(vamp_args);
	thread_timer_stop(args);
	return 0;
}