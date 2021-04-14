/*
 * Copyright (c) 2021 Pierro Zachareas
 */

#include "configuration.h"

#ifdef CHECKSUM_RESULTS
#include <stdlib.h>
#include <stdio.h>
#include <openssl/evp.h>
#include "hash.h"
#endif

#ifdef CHECKSUM_RESULTS

void hash_init(struct hash **ptr)
{
	struct hash *new = malloc(sizeof(struct hash));
	if (new == NULL)
		abort();

	OpenSSL_add_all_digests();

	new->md = EVP_get_digestbyname(DIGEST_NAME);
	if (!new->md) {
		printf("Unknown message digest %s\n", DIGEST_NAME);
		exit(1);
	}

	new->md_size = EVP_MD_size(new->md);
	new->md_value = malloc(sizeof(unsigned char) * new->md_size);

	for (int i = 0; i < new->md_size; i++)
		new->md_value[i] = 0;

	new->mdctx = EVP_MD_CTX_create();

	*ptr = new;	
}

void hash_free(struct hash *ptr)
{
	EVP_MD_CTX_destroy(ptr->mdctx);
	ptr->mdctx = NULL;
	free(ptr->mdctx);
	free(ptr->md_value);
	free(ptr);
	EVP_cleanup();
}

void hash_print(struct hash *ptr)
{
	fprintf(stderr, "Digest %s is: ", DIGEST_NAME);
	for(int i = 0; i < ptr->md_size; i++)
		fprintf(stderr, "%02x", ptr->md_value[i]);
	fprintf(stderr, "\n");
}
#endif /* CHECKSUM_RESULTS */