/*
 *   Copyright (C) 2022 SUSE LLC
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Written by Olaf Kirch <okir@suse.com>
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>

#include "util.h"
#include "eventlog.h"
#include "runtime.h"

enum {
	__TPM2_ALG_sha1 = 4,
	__TPM2_ALG_sha256 = 11,
	__TPM2_ALG_sha384 = 12,
	__TPM2_ALG_sha512 = 13,

	TPM2_ALG_MAX
};

#define DESCRIBE_ALGO(name, size) \
	__DESCRIBE_ALGO(name, __TPM2_ALG_ ## name, size)
#define __DESCRIBE_ALGO(name, id, size) \
	[id]	= { id,		#name,		size }
static tpm_algo_info_t		tpm_algorithms[TPM2_ALG_MAX] = {
	DESCRIBE_ALGO(sha1,		20),
	DESCRIBE_ALGO(sha256,		32),
	DESCRIBE_ALGO(sha384,		48),
	DESCRIBE_ALGO(sha512,		64),
};

const tpm_algo_info_t *
__digest_by_tpm_alg(unsigned int algo_id, const tpm_algo_info_t *algorithms, unsigned int num_algoritms)
{
	const tpm_algo_info_t *algo;

	if (algo_id >= num_algoritms)
		return NULL;

	algo = &algorithms[algo_id];
	if (algo->digest_size == 0)
		return NULL;

	return algo;
}

const tpm_algo_info_t *
digest_by_tpm_alg(unsigned int algo_id)
{
	return __digest_by_tpm_alg(algo_id, tpm_algorithms, TPM2_ALG_MAX);
}

const tpm_algo_info_t *
digest_by_name(const char *name)
{
	const tpm_algo_info_t *algo;
	int i;

	for (i = 0, algo = tpm_algorithms; i < TPM2_ALG_MAX; ++i, ++algo) {
		if (algo->openssl_name && !strcasecmp(algo->openssl_name, name))
			return algo;
	}

	return NULL;
}

const char *
digest_algo_name(const tpm_evdigest_t *md)
{
	static char temp[32];
	const char *name;

	if (md->algo == NULL)
		return "unknown";

	if ((name = md->algo->openssl_name) == NULL) {
		snprintf(temp, sizeof(temp), "TPM2_ALG_%u", md->algo->tcg_id);
		name = temp;
	}

	return name;
}

const char *
digest_print(const tpm_evdigest_t *md)
{
	static char buffer[1024];

	snprintf(buffer, sizeof(buffer), "%s: %s",
			digest_algo_name(md),
			digest_print_value(md));
	return buffer;
}

const char *
digest_print_value(const tpm_evdigest_t *md)
{
	static char buffer[2 * sizeof(md->data) + 1];
	unsigned int i;

	assert(md->size <= sizeof(md->data));
        for (i = 0; i < md->size; i++)
                sprintf(buffer + 2 * i, "%02x", md->data[i]);
	return buffer;
}

const tpm_evdigest_t *
digest_compute(const tpm_algo_info_t *algo_info, const void *data, unsigned int size)
{
	static tpm_evdigest_t md;
	digest_ctx_t *ctx;

	memset(&md, 0, sizeof(md));
	ctx = digest_ctx_new(algo_info);
	if (ctx == NULL)
		return NULL;

	digest_ctx_update(ctx, data, size);
	if (!digest_ctx_final(ctx, &md))
		return NULL;

	digest_ctx_free(ctx);
	return &md;
}

const tpm_evdigest_t *
digest_buffer(const tpm_algo_info_t *algo_info, struct buffer *buffer)
{
	return digest_compute(algo_info, buffer_read_pointer(buffer), buffer_available(buffer));
}

const tpm_evdigest_t *
digest_from_file(const tpm_algo_info_t *algo_info, const char *filename, int flags)
{
	const tpm_evdigest_t *md;
	buffer_t *buffer;

	buffer = runtime_read_file(filename, flags);

	md = digest_compute(algo_info,
			buffer_read_pointer(buffer),
			buffer_available(buffer));
	buffer_free(buffer);

	return md;
}


bool
digest_equal(const tpm_evdigest_t *a, const tpm_evdigest_t *b)
{
	return a->algo == b->algo && a->size == b->size && !memcmp(a->data, b->data, a->size);
}

bool
digest_is_zero(const tpm_evdigest_t *md)
{
	unsigned int i;
	unsigned char x = 0;

	for (i = 0; i < md->size; ++i)
		x |= md->data[i];

	return x == 0;
}

bool
digest_is_invalid(const tpm_evdigest_t *md)
{
	unsigned int i;
	unsigned char x = 0xFF;

	for (i = 0; i < md->size; ++i)
		x &= md->data[i];

	return x == 0xFF;
}

struct digest_ctx {
	EVP_MD_CTX *	mdctx;

	tpm_evdigest_t	md;
};

digest_ctx_t *
digest_ctx_new(const tpm_algo_info_t *algo_info)
{
	const EVP_MD *evp_md;
	digest_ctx_t *ctx;

	evp_md = EVP_get_digestbyname(algo_info->openssl_name);
	if (evp_md == NULL) {
		error("Unknown message digest %s\n", algo_info->openssl_name);
		return NULL;
	}

	ctx = calloc(1, sizeof(*ctx));
	ctx->mdctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx->mdctx, evp_md, NULL);

	ctx->md.algo = algo_info;

	return ctx;
}

void
digest_ctx_update(digest_ctx_t *ctx, const void *data, unsigned int size)
{
	if (ctx->mdctx == NULL)
		fatal("%s: trying to update digest after having finalized it\n", __func__);

	EVP_DigestUpdate(ctx->mdctx, data, size);
}

tpm_evdigest_t *
digest_ctx_final(digest_ctx_t *ctx, tpm_evdigest_t *result)
{
	tpm_evdigest_t *md = &ctx->md;

	if (ctx->mdctx) {
		EVP_DigestFinal_ex(ctx->mdctx, md->data, &md->size);

		EVP_MD_CTX_free(ctx->mdctx);
		ctx->mdctx = NULL;
	}

	if (result) {
		*result = *md;
		md = result;
	}

	return md;

}

void
digest_ctx_free(digest_ctx_t *ctx)
{
	(void) digest_ctx_final(ctx, NULL);

	free(ctx);
}
