// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2012 Jens Kruse Andersen
 * Copyright (c) 2021 Pierro Zachareas
 */

#include <stdlib.h>

#include "configuration.h"
#include "helper.h"
#include "cache.h"

#if CACHE

digits_t set_dig(fang_t number)
{
#if ELEMENT_BITS < 64

	digits_t ret = 0;
	digits_t tmp[10] = {0};
	for (; number > 0; number /= 10)
		tmp[number % 10] += 1;

	for (digit_t i = 1; i < 10; i++)
		ret = ret * DIG_BASE + tmp[i];

	return ret;

#else
	digits_t ret = 0;
	for (fang_t i = number; i > 0; i /= 10) {
		digit_t digit = i % 10;
		if (digit >= 1)
			ret += (digits_t)1 << ((digit - 1) * DIGMULT);
	}
	return ret;
#endif
}

struct cache *cache_init(vamp_t max)
{
	struct cache *new = NULL;
	new = malloc(sizeof(struct cache));
	if (new == NULL)
		abort();

	fang_t length_a = length(max) / 3;
	fang_t length_b = length(max) - (2 * length_a);
	new->size = pow10v(length_b);

	if (length_a < 3)
		new->power_a = new->size;
	else
		new->power_a = pow10v(length_a);

	new->dig = malloc(sizeof(digits_t) * new->size);
	if (new->dig == NULL)
		abort();

	for (fang_t d = 0; d < new->size; d++)
		new->dig[d] = set_dig(d);
	return new;
}

void cache_free(struct cache *ptr)
{
	if (ptr != NULL)
		free(ptr->dig);
	free(ptr);
}

#endif /* CACHE */