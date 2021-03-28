/*
 * Copyright (c) 2021, Pierro Zachareas, et al.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// Compile with: gcc -O2 -Wall -Wextra -pthread -o helsing helsing.c
// Check memory with valgrind --tool=massif
// Check threads with thread sanitizer -fsanitize=thread

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>	// isdigit

/*--------------------------- COMPILATION OPTIONS  ---------------------------*/
#define THREADS 1
#define thread_t uint16_t

/*
 * VERBOSE_LEVEL:
 * 0 - Count fang pairs
 * 1 - Print fang pairs
 * 2 - Count vampire numbers
 * 3 - Print vampire numbers in OEIS format
 */

#define VERBOSE_LEVEL 2

#define MIN_FANG_PAIRS 1 // requires VERBOSE_LEVEL > 1
#define DISPLAY_PROGRESS false
#define MEASURE_RUNTIME false

/*
 * CACHE:
 *
 * 	This code was originally written by Jens Kruse Andersen and is included
 * with their permission. Adjustments were made to improve runtime, memory
 * usage and access patterns, and accomodate features such as multithreading.
 * Source: http://primerecords.dk/vampires/index.htm
 *
 * There are two things that this optimization does:
 *
 * 1) Reduce computations by caching & minimize cache size
 * 	Given a set of numbers {123456, 125634, 345612}, in order to convert
 * 	them to arrays of digits, 3 * 6 = 18 modulo & division operations are
 * 	required. However if we calculate and store the {12, 34, 56}, we can
 * 	reconstruct the original numbers {[12][34][56], [12][56][34],
 * 	[34][56][12]} and their arrays of digits, with only 3 * 2 = 6 modulo &
 * 	division + 3~9 load operations.
 *
 * 	Given a product A and its fangs B & C, in order to store the sum of
 * 	each digit, we can use an array of 10 elements.	Because B & C are both
 * 	fangs it will be always true that arr_A and arr_B + arr_C have the same
 * 	total sum of digits and therefore we can avoid storing one of the
 * 	elements. I chose not to store the 0s.
 *
 * 	In this specific case an array of elements would waste memory due to
 * 	overhead. A more efficient solution would be to represent the array
 * 	with a single base-m number.
 *
 * 2) Data parallelism
 * 	Because each element requires very little space, we can use a single
 * 	32/64 bit unsigned integer in place of the aforementioned array.
 *
 * 	It just so happens that all the elements get processed at once (in
 * 	parallel), memory alignment issues are avoided and the base-m for a 64
 * 	bit element tops at 128	(2^7). It's as if we had assigned dedicated
 * 	bitfields and we get to use bitshift operations.
 *
 * Note to future developers; it's possible to make the array even smaller:
 * 	1. By using 32-bit elements and then expanding them to 64 bits.
 * 	   That would result in 50% array size and 247% runtime*.
 *
 * 	2. By performing extra division and modulo operations.
 * 	   That would result in 10% array size and 188% runtime*.
 *
 * 	3. By using 3 * 16-bits instead of 64-bits.
 * 	   That would result in 75% array size and 121% runtime*.
 *
 * 	*Based on some of my testing. Your mileage may vary.
 */

#define CACHE true
#define ELEMENT_BITS 64

/*
 * MAX_TILE_SIZE:
 *
 * Maximum value: 18446744073709551615ULL (2^64 -1)
 *
 * 	Because there is no simple way to predict the amount of vampire numbers
 * for a given range, MAX_TILE_SIZE can be used to limit the memory usage of
 * heapsort.
 *
 * Heapsort shouldn't use more memory than:
 * THREADS * (sizeof(bthandle) + MAX_TILE_SIZE * sizeof(btree))
 */

#define AUTO_TILE_SIZE true
#define MAX_TILE_SIZE 99999999999ULL

#define LINK_SIZE 100
#define SANITY_CHECK false

/*
 * Both vamp_t and fang_t must be unsigned, vamp_t should be double the size
 * of fang_t and fang_max, vamp_max should be set accordingly.
 *
 * You should be able to change vamp_t up to 256-bit without any issues.
 * If you want to go any higher check the uint8_t for overflow.
*/

typedef unsigned long long vamp_t;
#define vamp_max ULLONG_MAX

typedef unsigned long fang_t;
#define fang_max ULONG_MAX

typedef uint8_t digit_t;
typedef uint8_t length_t;

/*---------------------------- PREPROCESSOR STUFF ----------------------------*/
// DIGMULT = ELEMENT_BITS/(10 - DIGSKIP)
#if ELEMENT_BITS == 32
	typedef uint32_t digits_t;
	#define DIGMULT 3
	#define DIG_BASE 11
#elif ELEMENT_BITS == 64
	typedef uint64_t digits_t;
	#define DIGMULT 7
	#define DIG_BASE 128
#endif

#if MEASURE_RUNTIME
	#if defined(CLOCK_MONOTONIC)
		#define SPDT_CLK_MODE CLOCK_MONOTONIC
	#elif defined(CLOCK_REALTIME)
		#define SPDT_CLK_MODE CLOCK_REALTIME
	#endif
#endif

#if (VERBOSE_LEVEL == 0)
	#define COUNT_RESULTS
#elif (VERBOSE_LEVEL == 1)
	#define DUMP_RESULTS
#elif (VERBOSE_LEVEL == 2)
	#define PROCESS_RESULTS
#elif (VERBOSE_LEVEL == 3)
	#define STORE_RESULTS
	#define PROCESS_RESULTS
	#define PRINT_RESULTS
#endif

/*----------------------------- HELPER FUNCTIONS -----------------------------*/
static length_t length(vamp_t x)
{
	length_t length = 1;
	for (; x > 9; x /= 10)
		length++;
	return length;
}

static bool length_isodd(vamp_t x)
{
	return (length(x) % 2);
}

static vamp_t pow10v(length_t exponent) // pow10 for vamp_t.
{
	#if SANITY_CHECK
		assert(exponent <= length(vamp_max) - 1);
	#endif
	vamp_t base = 1;
	for (; exponent > 0; exponent--)
		base *= 10;
	return base;
}

/*
 * willoverflow:
 * Checks if (10 * x + digit) will overflow, without causing and overflow.
 */
static bool willoverflow(vamp_t x, digit_t digit)
{
	assert(digit < 10);
	if (x > vamp_max / 10)
		return true;
	if (x == vamp_max / 10 && digit > vamp_max % 10)
		return true;
	return false;
}

static int atov(const char *str, vamp_t *number) // ASCII to vamp_t
{
	assert(str != NULL);
	assert(number != NULL);
	vamp_t ret = 0;
	for (length_t i = 0; isdigit(str[i]); i++) {
		digit_t digit = str[i] - '0';
		if (willoverflow(ret, digit))
			return 1;
		ret = 10 * ret + digit;
	}
	*number = ret;
	return 0;
}

static bool notrailingzero(fang_t x)
{
	return ((x % 10) != 0);
}

static vamp_t get_min(vamp_t min, vamp_t max)
{
	if (length_isodd(min)) {
		length_t min_length = length(min);
		if (min_length < length(max))
			min = pow10v(min_length);
		else
			min = max;
	}
	return min;
}

static vamp_t get_max(vamp_t min, vamp_t max)
{
	if (length_isodd(max)) {
		length_t max_length = length(max);
		if (max_length > length(min))
			max = pow10v(max_length - 1) - 1;
		else
			max = min;
	}
	return max;
}

static vamp_t get_lmax(vamp_t lmin, vamp_t max)
{
	vamp_t lmax;
	if (length(lmin) < length(vamp_max)) {
		lmax = pow10v(length(lmin)) - 1;
		if (lmax < max)
			return lmax;
	}
	return max;
}

static fang_t sqrtv_floor(vamp_t x) // vamp_t sqrt to fang_t.
{
	vamp_t x2 = x / 2;
	vamp_t root = x2;
	if (root > 0) {
		vamp_t tmp = (root + x / root) / 2;
		while (tmp < root) {
			root = tmp;
			tmp = (root + x / root) / 2;
		}
		return root;
	}
	return x;
}

static fang_t sqrtv_roof(vamp_t x)
{
	fang_t root = sqrtv_floor(x);
	if (root == fang_max)
		return root;

	return (x / root);
}

// Modulo 9 lack of congruence
static bool con9(vamp_t x, vamp_t y)
{
	return ((x + y) % 9 != (x * y) % 9);
}

static vamp_t div_roof (vamp_t x, vamp_t y)
{
	return (x/y + !!(x%y));
}

static vamp_t get_tilesize(
	__attribute__((unused)) vamp_t lmin,
	__attribute__((unused)) vamp_t lmax)
{
	vamp_t tile_size = vamp_max;

	#if AUTO_TILE_SIZE
		tile_size = (lmax - lmin) / (4 * THREADS + 2);
	#endif

	if (tile_size > MAX_TILE_SIZE)
		tile_size = MAX_TILE_SIZE;

	return tile_size;
}

/*------------------------------- linked list  -------------------------------*/
struct llist
{
	vamp_t value[LINK_SIZE];
	uint16_t current;
	struct llist *next;
};

static struct llist *llist_init(vamp_t value , struct llist *next)
{

	struct llist *new;
	if (next != NULL && next->current < LINK_SIZE) {
		new = next;
		new->value[new->current] = value;
		new->current += 1;
	} else {
		new = malloc(sizeof(struct llist));
		if (new == NULL)
			abort();

		new->value[0] = value;
		new->current = 1;
		new->next = next;
	}
	return new;
}

static void llist_free(struct llist *list)
{
	struct llist *tmp = list;
	for (struct llist *i = tmp; tmp != NULL; i = tmp) {
		tmp = tmp->next;
		free(i);
	}
}

#ifdef PRINT_RESULTS
static void llist_print(struct llist *list, vamp_t count)
{
	for (struct llist *i = list; i != NULL ; i = i->next) {
		for (uint16_t j = i->current; j > 0 ; j--) {
			fprintf(stdout, "%llu %llu\n", ++count, i->value[j - 1]);
			fflush(stdout);
		}
	}
}
#endif

/*---------------------------- linked list handle ----------------------------*/
struct llhandle
{
#ifdef STORE_RESULTS
	struct llist *head;
#endif
	vamp_t size;
};

static struct llhandle *llhandle_init()
{
	struct llhandle *new = malloc(sizeof(struct llhandle));
	if (new == NULL)
		abort();

#ifdef STORE_RESULTS
	new->head = NULL;
#endif
	new->size = 0;
	return new;
}

static void llhandle_free(struct llhandle *handle)
{
#ifdef STORE_RESULTS
	if (handle != NULL)
		llist_free(handle->head);
#endif
	free(handle);
}

static void llhandle_add(struct llhandle *handle, __attribute__((unused)) vamp_t value)
{
	if (handle == NULL)
		return;

#ifdef STORE_RESULTS
	handle->head = llist_init(value, handle->head);
#endif
	handle->size += 1;
}

/*------------------------------- binary tree  -------------------------------*/

struct btree
{
	struct btree *left;
	struct btree *right;
	vamp_t value;
	length_t height; //Should probably be less than 32
	uint8_t fang_pairs;
};

static struct btree *btree_init(vamp_t value)
{
	struct btree *new = malloc(sizeof(struct btree));
	if (new == NULL)
		abort();

	new->left = NULL;
	new->right = NULL;
	new->height = 0;
	new->value = value;
	new->fang_pairs = 1;
	return new;
}

static void btree_free(struct btree *tree)
{
	if (tree != NULL) {
		if (tree->left != NULL)
			btree_free(tree->left);
		if (tree->right != NULL)
			btree_free(tree->right);
	}
	free(tree);
}

static int is_balanced(struct btree *tree)
{
	if (tree == NULL)
		return 0;

	int lheight = 0;
	int rheight = 0;

	if (tree->left != NULL)
		lheight = tree->left->height;
	if (tree->right != NULL)
		rheight = tree->right->height;

	return (lheight - rheight);
}

static void btree_reset_height(struct btree *tree)
{
	#if SANITY_CHECK
		assert(tree != NULL);
	#endif
	tree->height = 0;
	if (tree->left != NULL && tree->left->height >= tree->height)
		tree->height = tree->left->height + 1;
	if (tree->right != NULL && tree->right->height >= tree->height)
		tree->height = tree->right->height + 1;
	#if SANITY_CHECK
		assert(tree->height <= 32);
	#endif
}

/*
 * Binary tree left rotation:
 *
 *     A                 B
 *    / \               / \
 *  ...  B     -->     A  ...
 *      / \           / \
 *     C  ...       ...  C
 *
 * The '...' are completely unaffected.
 */

static struct btree *btree_rotate_l(struct btree *tree)
{
	if (tree->right != NULL) {
		struct btree *right = tree->right;
		tree->right = right->left;
		btree_reset_height(tree);
		right->left = tree;
		btree_reset_height(right);
		return right;
	}
	return tree;
}

/*
 * Binary tree right rotation:
 *
 *       A             B
 *      / \           / \
 *     B  ...  -->  ...  A
 *    / \               / \
 *  ...  C             C  ...
 *
 * The '...' are completely unaffected.
 */

static struct btree *btree_rotate_r(struct btree *tree)
{
	if (tree->left != NULL) {
		struct btree *left = tree->left;
		tree->left = left->right;
		btree_reset_height(tree);
		left->right = tree;
		btree_reset_height(left);
		return left;
	}
	return tree;
}

static struct btree *btree_balance(struct btree *tree)
{
	#if SANITY_CHECK
		assert(tree != NULL);
	#endif
	int isbalanced = is_balanced(tree);
	if (isbalanced > 1) {
		if (is_balanced(tree->left) < 0) {
			tree->left = btree_rotate_l(tree->left);
			btree_reset_height(tree); //maybe optional?
		}
		tree = btree_rotate_r(tree);
	}
	else if (isbalanced < -1) {
		if (is_balanced(tree->right) > 0) {
			tree->right = btree_rotate_r(tree->right);
			btree_reset_height(tree); //maybe optional?
		}
		tree = btree_rotate_l(tree);

	}
	return tree;
}

static struct btree *btree_add(
	struct btree *tree,
	vamp_t node,
	vamp_t *count)
{
	if (tree == NULL) {
		*count += 1;
		return btree_init(node);
	}
	if (node == tree->value) {
		tree->fang_pairs += 1;
		return tree;
	}
	else if (node < tree->value)
		tree->left = btree_add(tree->left, node, count);
	else
		tree->right = btree_add(tree->right, node, count);

	btree_reset_height(tree);
	tree = btree_balance(tree);
	return tree;
}

static struct btree *btree_cleanup(
	struct btree *tree,
	vamp_t number,
	struct llhandle *lhandle,
	vamp_t *btree_size)
{
	if (tree == NULL)
		return NULL;

	tree->right = btree_cleanup(tree->right, number, lhandle, btree_size);

	if (tree->value >= number) {
		if (tree->fang_pairs >= MIN_FANG_PAIRS) {
			llhandle_add(lhandle, tree->value);
		}
		struct btree *tmp = tree->left;
		tree->left = NULL;
		btree_free(tree);
		*btree_size -= 1;

		tree = btree_cleanup(tmp, number, lhandle, btree_size);
	}

	if (tree == NULL)
		return NULL;
	btree_reset_height(tree);
	tree = btree_balance(tree);

	return tree;
}

/*---------------------------- binary tree handle ----------------------------*/

struct bthandle
{
	#ifdef PROCESS_RESULTS
		struct btree *tree;
		vamp_t size;
	#endif
};

static struct bthandle *bthandle_init()
{
	struct bthandle *new = NULL;
	#ifdef PROCESS_RESULTS
		new = malloc(sizeof(struct bthandle));
		if (new == NULL)
			abort();

		new->tree = NULL;
		new->size = 0;
	#endif
	return new;
}

static void bthandle_free(__attribute__((unused)) struct bthandle *handle)
{
	#ifdef PROCESS_RESULTS
		if (handle != NULL)
			btree_free(handle->tree);
		free(handle);
	#endif
}

static void bthandle_add(
	__attribute__((unused)) struct bthandle *handle,
	__attribute__((unused)) vamp_t number)
{
	#ifdef PROCESS_RESULTS
		#if SANITY_CHECK
			assert(handle != NULL);
		#endif
		handle->tree = btree_add(handle->tree, number, &(handle->size));
	#endif
}

static void bthandle_reset(__attribute__((unused)) struct bthandle *handle)
{
	#ifdef PROCESS_RESULTS
		btree_free(handle->tree);
		handle->tree = NULL;
		handle->size = 0;
	#endif
}

/*
 * Move inactive data from binary tree to linked list
 * and free up memory. Works best with low thread counts.
 */
void bthandle_cleanup(
	__attribute__((unused)) struct bthandle *handle,
	__attribute__((unused)) struct llhandle *lhandle,
	__attribute__((unused)) vamp_t number)
{
#ifdef PROCESS_RESULTS
	struct btree *tree = handle->tree;
	vamp_t *size = &(handle->size);
	handle->tree = btree_cleanup(tree, number, lhandle, size);
#endif
}

/*----------------------------------- tile -----------------------------------*/
struct tile
{
	vamp_t lmin; // local minimum
	vamp_t lmax; // local maximum

#ifdef PROCESS_RESULTS
	struct llhandle *result;
	bool complete;
#endif
};

static struct tile *tile_init(vamp_t min, vamp_t max)
{
	struct tile *new = malloc(sizeof(struct tile));
	if (new == NULL)
		abort();

	new->lmin = min;
	new->lmax = max;

#ifdef PROCESS_RESULTS
	new->result = NULL;
	new->complete = false;
#endif
	return new;
}

static void tile_free(struct tile *ptr)
{
#ifdef PROCESS_RESULTS
	if (ptr != NULL)
		llhandle_free(ptr->result);
#endif
	free(ptr);
}

/*---------------------------------- matrix ----------------------------------*/
struct matrix
{
	struct tile **arr;
	vamp_t size;
	vamp_t unfinished; // First tile that hasn't been accepted.
	vamp_t cleanup;    // Last tile that hasn't been processed.
	fang_t fmax;
};

static struct matrix *matrix_init()
{
	struct matrix *new = malloc(sizeof(struct matrix));
	if (new == NULL)
		abort();

	new->arr = NULL;
	new->size = 0;
	new->unfinished = 0;
	new->fmax = 0;

	#ifdef PROCESS_RESULTS
		new->cleanup = 0;
	#endif

	return new;
}

static void matrix_free(struct matrix *ptr)
{
	if (ptr == NULL)
		return;

	if (ptr->arr != NULL) {
		for (vamp_t i = 0; i < ptr->size; i++)
			tile_free(ptr->arr[i]);
		free(ptr->arr);
	}
	free(ptr);
}

static void matrix_set(struct matrix *ptr, vamp_t lmin, vamp_t lmax)
{
	assert(lmin <= lmax);
	assert(ptr->arr == NULL);

	ptr->unfinished = 0;

	length_t fang_length = length(lmin) / 2;
	if (fang_length == length(fang_max))
		ptr->fmax = fang_max;
	else
		ptr->fmax = pow10v(fang_length); // Max factor value.

	#ifdef PROCESS_RESULTS
		ptr->cleanup = 0;
	#endif

	if (ptr->fmax < fang_max) {
		vamp_t fmaxsquare = ptr->fmax;
		fmaxsquare *= ptr->fmax;
		if (lmax > fmaxsquare && lmin <= fmaxsquare)
			lmax = fmaxsquare; // Max can be bigger than fmax ^ 2: 9999 > 99 ^ 2.
	}

	vamp_t tile_size = get_tilesize(lmin, lmax);

	ptr->size = div_roof((lmax - lmin + 1), tile_size + (tile_size < vamp_max));
	ptr->arr = malloc(sizeof(struct tile *) * ptr->size);
	if (ptr->arr == NULL)
		abort();

	vamp_t x = 0;
	vamp_t iterator = tile_size;
	for (vamp_t i = lmin; i <= lmax; i += iterator + 1) {
		if (lmax - i < tile_size)
			iterator = lmax - i;

		ptr->arr[x++] = tile_init(i, i + iterator);

		if (i == lmax)
			break;
		if (i + iterator == vamp_max)
			break;
	}
	ptr->arr[ptr->size - 1]->lmax = lmax;
}

static void matrix_reset(struct matrix *ptr)
{
	for (vamp_t i = 0; i < ptr->size; i++)
		tile_free(ptr->arr[i]);
	free(ptr->arr);
	ptr->arr = NULL;
}

static void matrix_print(
	__attribute__((unused)) struct matrix *ptr,
	__attribute__((unused)) vamp_t *count)
{
#if (defined PROCESS_RESULTS && defined PRINT_RESULTS)
	for (vamp_t x = ptr->cleanup; x < ptr->size; x++)
		if (ptr->arr[x] != NULL) {
			llist_print(ptr->arr[x]->result->head, *count);
			*count += ptr->arr[x]->result->size;
		}
#endif
}

// matrix_progress requires mutex lock
static void matrix_progress(__attribute__((unused)) struct matrix *ptr)
{
	#if (defined PROCESS_RESULTS &&  DISPLAY_PROGRESS)
		fprintf(stderr, "%llu, %llu", ptr->arr[ptr->cleanup]->lmin, ptr->arr[ptr->cleanup]->lmax);
		fprintf(stderr, "  %llu/%llu\n", ptr->cleanup + 1, ptr->size);
	#endif
}

/*---------------------------------- cache  ----------------------------------*/
struct cache
{
#if CACHE
	digits_t *dig;
	fang_t size;
	fang_t power_a;
#endif
};

static digits_t set_dig(fang_t number)
{
	digits_t ret = 0;
	#if ELEMENT_BITS == 64
		for (fang_t i = number; i > 0; i /= 10) {
			digit_t digit = i % 10;
			if (digit >= 1)
				ret += (digits_t)1 << ((digit - 1) * DIGMULT);
		}
	#else
		digits_t tmp[10] = {0};
		for (; number > 0; number /= 10)
			tmp[number % 10] += 1;

		for (digit_t i = 1; i < 10; i++)
			ret = ret * DIG_BASE + tmp[i];
	#endif
	return ret;
}

static struct cache *cache_init(__attribute__((unused)) vamp_t max)
{
	struct cache *new = NULL;
	#if CACHE
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
	#endif
	return new;
}

static void cache_free(__attribute__((unused)) struct cache *ptr)
{
#if CACHE
	if (ptr != NULL)
		free(ptr->dig);
	free(ptr);
#endif
}

/*----------------------------------------------------------------------------*/
struct vargs	/* Vampire arguments */
{
	vamp_t local_count;
	struct cache *digptr;

#ifdef PROCESS_RESULTS
	struct bthandle *thandle;
	struct llhandle *lhandle;
#endif
#if MEASURE_RUNTIME
	vamp_t total;
#endif
};

static struct vargs *vargs_init(struct cache *digptr)
{
	struct vargs *new = malloc(sizeof(struct vargs));
	if (new == NULL)
		abort();

	new->local_count = 0;
	new->digptr = digptr;

#ifdef PROCESS_RESULTS
	new->lhandle = llhandle_init();
	new->thandle = bthandle_init();
#endif
#if MEASURE_RUNTIME
	new->total = 0;
#endif
	return new;
}

static void vargs_free(struct vargs *args)
{
#ifdef PROCESS_RESULTS
	bthandle_free(args->thandle);
	llhandle_free(args->lhandle);
#endif
	free (args);
}

static void vargs_reset(struct vargs *args)
{
	args->local_count = 0;

#ifdef PROCESS_RESULTS
	args->lhandle = llhandle_init();
	bthandle_reset(args->thandle);
#endif
}

#ifdef PROCESS_RESULTS
static struct llhandle *vargs_getlhandle(__attribute__((unused)) struct vargs *args)
{
	struct llhandle *ret = NULL;
#ifdef PROCESS_RESULTS
	ret = args->lhandle;
	args->lhandle = NULL;
#endif
	return ret;
}
#endif

static void vargs_btree_cleanup(
	__attribute__((unused)) struct vargs *args,
	__attribute__((unused)) vamp_t number)
{
#ifdef PROCESS_RESULTS
	args->thandle->tree = btree_cleanup(args->thandle->tree, number, args->lhandle, &(args->thandle->size));
#endif
}

/*----------------------------------------------------------------------------*/
#if !CACHE

static void vampire(vamp_t min, vamp_t max, struct vargs *args, fang_t fmax)
{
	fang_t min_sqrt = sqrtv_roof(min);
	fang_t max_sqrt = sqrtv_floor(max);

	for (fang_t multiplier = fmax; multiplier >= min_sqrt; multiplier--) {
		if (multiplier % 3 == 1)
			continue;

		fang_t multiplicand = div_roof(min, multiplier); // fmin * fmax <= min - 10^n
		bool mult_zero = notrailingzero(multiplier);

		fang_t multiplicand_max;
		if (multiplier >= max_sqrt)
			multiplicand_max = max / multiplier;
		else
			multiplicand_max = multiplier;
			// multiplicand can be equal to multiplier:
			// 5267275776 = 72576 * 72576.

		while (multiplicand <= multiplicand_max && con9(multiplier, multiplicand))
			multiplicand++;

		if (multiplicand <= multiplicand_max) {
			vamp_t product_iterator = multiplier;
			product_iterator *= 9; // <= 9 * 2^32
			vamp_t product = multiplier;
			product *= multiplicand; // avoid overflow

			length_t mult_array[10] = {0};
			for (fang_t i = multiplier; i > 0; i /= 10)
				mult_array[i % 10] += 1;

			for (; multiplicand <= multiplicand_max; multiplicand += 9) {
				uint16_t product_array[10] = {0};
				for (vamp_t p = product; p > 0; p /= 10)
					product_array[p % 10] += 1;

				for (digit_t i = 0; i < 10; i++)
					if (product_array[i] < mult_array[i])
						goto vampire_exit;

				digit_t temp;
				for (fang_t m = multiplicand; m > 0; m /= 10) {
					temp = m % 10;
					if (product_array[temp] == 0)
						goto vampire_exit;
					else
						product_array[temp]--;
				}
				for (digit_t i = 0; i < 9; i++)
					if (product_array[i] != mult_array[i])
						goto vampire_exit;

				if (mult_zero || notrailingzero(multiplicand)) {
					#if defined COUNT_RESULTS ||  defined DUMP_RESULTS
						args->local_count += 1;
					#endif
					#ifdef DUMP_RESULTS
						printf("%llu = %lu x %lu\n", product, multiplier, multiplicand);
					#endif
					#ifdef PROCESS_RESULTS
						bthandle_add(args->thandle, product);
					#endif
				}
vampire_exit:
				product += product_iterator;
			}
			if (multiplier < max_sqrt && mult_zero)
				vargs_btree_cleanup(args, product);
		}
	}
	vargs_btree_cleanup(args, 0);
	#if MEASURE_RUNTIME
		#ifdef PROCESS_RESULTS
			args->total += args->lhandle->size;
		#elif defined COUNT_RESULTS ||  defined DUMP_RESULTS
			args->total += args->local_count;
		#endif
	#endif
	return;
}

#else /* !CACHE */

static void vampire(vamp_t min, vamp_t max, struct vargs *args, fang_t fmax)
{
	fang_t min_sqrt = sqrtv_roof(min);
	fang_t max_sqrt = sqrtv_floor(max);

	fang_t power_a = args->digptr->power_a;
	digits_t *dig = args->digptr->dig;

	for (fang_t multiplier = fmax; multiplier >= min_sqrt; multiplier--) {
		if (multiplier % 3 == 1)
			continue;

		fang_t multiplicand = div_roof(min, multiplier); // fmin * fmax <= min - 10^n
		bool mult_zero = notrailingzero(multiplier);

		fang_t multiplicand_max;
		if (multiplier >= max_sqrt)
			multiplicand_max = max / multiplier;
		else
			multiplicand_max = multiplier;
			// multiplicand can be equal to multiplier:
			// 5267275776 = 72576 * 72576.

		while (multiplicand <= multiplicand_max && con9(multiplier, multiplicand))
			multiplicand++;

		if (multiplicand <= multiplicand_max) {
			vamp_t product_iterator = multiplier;
			product_iterator *= 9; // <= 9 * 2^32
			vamp_t product = multiplier;
			product *= multiplicand; // avoid overflow

			fang_t step0 = product_iterator % power_a;
			fang_t step1 = product_iterator / power_a;

			fang_t e0 = multiplicand % power_a;
			fang_t e1 = multiplicand / power_a;

			/*
			 * digd = dig[multiplier];
			 * Each digd is calculated and accessed only once, we don't need to store them in memory.
			 * We can calculate digd on the spot and make the dig array 10 times smaller.
			 */

			digits_t digd;

			if (min_sqrt >= args->digptr->size)
				digd = set_dig(multiplier);
			else
				digd = dig[multiplier];

			fang_t de0 = product % power_a;
			fang_t de1 = (product / power_a) % power_a;
			fang_t de2 = ((product / power_a) / power_a);

			for (; multiplicand <= multiplicand_max; multiplicand += 9) {
				if (digd + dig[e0] + dig[e1] == dig[de0] + dig[de1] + dig[de2])
					if (mult_zero || notrailingzero(multiplicand)) {
					#if defined COUNT_RESULTS ||  defined DUMP_RESULTS
						args->local_count += 1;
					#endif
					#ifdef DUMP_RESULTS
						printf("%llu = %lu x %lu\n", product, multiplier, multiplicand);
					#endif
					#ifdef PROCESS_RESULTS
						bthandle_add(args->thandle, product);
					#endif
					}
				e0 += 9;
				if (e0 >= power_a) {
					e0 -= power_a;
					e1 ++;
				}
				de0 += step0;
				if (de0 >= power_a) {
					de0 -= power_a;
					de1 += 1;
				}
				de1 += step1;
				if (de1 >= power_a) {
					de1 -= power_a;
					de2 += 1;
				}
				product += product_iterator;
			}
			if (multiplier < max_sqrt && mult_zero)
				vargs_btree_cleanup(args, product);
		}
	}
	vargs_btree_cleanup(args, 0);
	#if MEASURE_RUNTIME
		#ifdef PROCESS_RESULTS
			args->total += args->lhandle->size;
		#elif defined COUNT_RESULTS ||  defined DUMP_RESULTS
			args->total += args->local_count;
		#endif
	#endif
	return;
}
#endif  /* !CACHE */

/*--------------------------------- Threads  ---------------------------------*/
struct targs_t
{
	pthread_mutex_t *read;
	pthread_mutex_t *write;
	struct matrix *mat;
	vamp_t *count;
	double	runtime;
	struct cache *digptr;

#if MEASURE_RUNTIME
	struct timespec start;
	vamp_t total; // total amount of numbers this thread has discovered
#endif
};

static struct targs_t *targs_t_init(
	pthread_mutex_t *read,
	pthread_mutex_t *write,
	struct matrix *mat,
	vamp_t *count,
	struct cache *digptr)
{
	struct targs_t *new = malloc(sizeof(struct targs_t));
	if (new == NULL)
		abort();

	new->read = read;
	new->write = write;
	new->mat = mat;
	new->count = count;
	new->runtime = 0.0;
	new->digptr = digptr;

#if MEASURE_RUNTIME
	new->total = 0;
#endif
	return new;
}

static void targs_t_free(struct targs_t *ptr)
{
	free(ptr);
}

static void thread_timer_start(__attribute__((unused)) struct targs_t *ptr)
{
#if MEASURE_RUNTIME
	clock_gettime(SPDT_CLK_MODE, &(ptr->start));
#endif
}

static void thread_timer_stop(__attribute__((unused)) struct targs_t *ptr)
{
#if MEASURE_RUNTIME
	struct timespec finish;
	clock_gettime(SPDT_CLK_MODE, &(finish));
	double elapsed = (finish.tv_sec - ptr->start.tv_sec);
	elapsed += (finish.tv_nsec - ptr->start.tv_nsec) / 1000000000.0;
	ptr->runtime = elapsed;
#endif
}

/*----------------------------------------------------------------------------*/
struct targs_handle
{
	struct targs_t *targs[THREADS];
	struct matrix *mat;
	struct cache *digptr;
	vamp_t counter;
	pthread_mutex_t *read;
	pthread_mutex_t *write;
};

static struct targs_handle *targs_handle_init(vamp_t max)
{
	struct targs_handle *new = malloc(sizeof(struct targs_handle));
	if (new == NULL)
		abort();

	new->mat = matrix_init();
	new->digptr = cache_init(max);
	new->counter = 0;
	new->read = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(new->read, NULL);
	new->write = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(new->write, NULL);

	for (thread_t thread = 0; thread < THREADS; thread++)
		new->targs[thread] = targs_t_init(new->read, new->write, new->mat, &(new->counter), new->digptr);
	return new;
}

static void targs_handle_free(struct targs_handle *ptr)
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

static void targs_handle_print(struct targs_handle *ptr)
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

/*----------------------------------------------------------------------------*/
static void *thread_worker(void *void_args)
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
				current->complete = true;
				while (
					args->mat->cleanup < args->mat->size &&
					args->mat->arr[args->mat->cleanup]->complete)
				{
					#ifdef PRINT_RESULTS
						llist_print(args->mat->arr[args->mat->cleanup]->result->head, *(args->count));
					#endif
					*(args->count) += args->mat->arr[args->mat->cleanup]->result->size;
					matrix_progress(args->mat);

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

/*----------------------------------------------------------------------------*/
int main(int argc, char* argv[])
{
	if (argc != 3) {
		printf("Usage: helsing [min] [max]\n");
		return 0;
	}
	vamp_t min, max;
	if (atov(argv[1], &min) || atov(argv[2], &max)) {
		fprintf(stderr, "Input out of range: [0, %llu]\n", vamp_max);
		return 1;
	}
	if (min > max) {
		fprintf(stderr, "Invalid arguments, min <= max\n");
		return 1;
	}
	if (max > 9999999999 && ELEMENT_BITS == 32) {
		fprintf(stderr, "WARNING: the code might produce false ");
		fprintf(stderr, "positives, please set ELEMENT_BITS to 64.\n");
		return 0;
	}

	min = get_min(min, max);
	max = get_max(min, max);

	vamp_t lmin = min;
	vamp_t lmax = get_lmax(lmin, max);

	pthread_t threads[THREADS];
	struct targs_handle *thhandle = targs_handle_init(max);

	for (; lmax <= max;) {
		fprintf(stderr, "Checking range: [%llu, %llu]\n", lmin, lmax);
		matrix_set(thhandle->mat, lmin, lmax);
		for (thread_t thread = 0; thread < THREADS; thread++)
			assert(pthread_create(&threads[thread], NULL, thread_worker, (void *)(thhandle->targs[thread])) == 0);
		for (thread_t thread = 0; thread < THREADS; thread++)
			pthread_join(threads[thread], 0);

		matrix_print(thhandle->mat, &(thhandle->counter));
		matrix_reset(thhandle->mat);
		if (lmax == max)
			break;

		lmin = get_min(lmax + 1, max);
		lmax = get_lmax(lmin, max);
	}
	targs_handle_print(thhandle);
	targs_handle_free(thhandle);
	return 0;
}
