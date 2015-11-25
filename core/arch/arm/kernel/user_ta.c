/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
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
#include <types_ext.h>
#include <stdlib.h>
#include <kernel/tee_rpc.h>
#include <kernel/tee_rpc_types.h>
#include <kernel/tee_ta_manager.h>
#include <kernel/thread.h>
#include <kernel/user_ta.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <mm/tee_mm.h>
#include <mm/tee_mmu.h>
#include <tee/tee_cryp_provider.h>
#include <tee/tee_cryp_utl.h>
#include <tee/tee_svc.h>
#include <signed_hdr.h>
#include <ta_pub_key.h>
#include <trace.h>
#include <utee_defines.h>
#include <util.h>

#include "elf_load.h"

#define TEE_TA_STACK_ALIGNMENT   8

static TEE_Result load_header(const struct shdr *signed_ta,
		struct shdr **sec_shdr)
{
	size_t s;

	if (!tee_vbuf_is_non_sec(signed_ta, sizeof(*signed_ta)))
		return TEE_ERROR_SECURITY;

	s = SHDR_GET_SIZE(signed_ta);
	if (!tee_vbuf_is_non_sec(signed_ta, s))
		return TEE_ERROR_SECURITY;

	/* Copy signed header into secure memory */
	*sec_shdr = malloc(s);
	if (!*sec_shdr)
		return TEE_ERROR_OUT_OF_MEMORY;
	memcpy(*sec_shdr, signed_ta, s);

	return TEE_SUCCESS;
}

static TEE_Result check_shdr(struct shdr *shdr)
{
	struct rsa_public_key key;
	TEE_Result res;
	uint32_t e = TEE_U32_TO_BIG_ENDIAN(ta_pub_key_exponent);
	size_t hash_size;

	if (shdr->magic != SHDR_MAGIC || shdr->img_type != SHDR_TA)
		return TEE_ERROR_SECURITY;

	if (TEE_ALG_GET_MAIN_ALG(shdr->algo) != TEE_MAIN_ALGO_RSA)
		return TEE_ERROR_SECURITY;

	res = tee_hash_get_digest_size(TEE_DIGEST_HASH_TO_ALGO(shdr->algo),
				       &hash_size);
	if (res != TEE_SUCCESS)
		return res;
	if (hash_size != shdr->hash_size)
		return TEE_ERROR_SECURITY;

	if (!crypto_ops.acipher.alloc_rsa_public_key ||
	    !crypto_ops.acipher.free_rsa_public_key ||
	    !crypto_ops.acipher.rsassa_verify ||
	    !crypto_ops.bignum.bin2bn)
		return TEE_ERROR_NOT_SUPPORTED;

	res = crypto_ops.acipher.alloc_rsa_public_key(&key, shdr->sig_size);
	if (res != TEE_SUCCESS)
		return res;

	res = crypto_ops.bignum.bin2bn((uint8_t *)&e, sizeof(e), key.e);
	if (res != TEE_SUCCESS)
		goto out;
	res = crypto_ops.bignum.bin2bn(ta_pub_key_modulus,
				       ta_pub_key_modulus_size, key.n);
	if (res != TEE_SUCCESS)
		goto out;

	res = crypto_ops.acipher.rsassa_verify(shdr->algo, &key, -1,
				SHDR_GET_HASH(shdr), shdr->hash_size,
				SHDR_GET_SIG(shdr), shdr->sig_size);
out:
	crypto_ops.acipher.free_rsa_public_key(&key);
	if (res != TEE_SUCCESS)
		return TEE_ERROR_SECURITY;
	return TEE_SUCCESS;
}

static TEE_Result load_elf(struct tee_ta_ctx *ctx, struct shdr *shdr,
			const struct shdr *nmem_shdr)
{
	TEE_Result res;
	struct tee_ta_param param = { 0 };
	size_t hash_ctx_size;
	void *hash_ctx = NULL;
	uint32_t hash_algo;
	uint8_t *nwdata = (uint8_t *)nmem_shdr + SHDR_GET_SIZE(shdr);
	size_t nwdata_len = shdr->img_size;
	void *digest = NULL;
	struct elf_load_state *elf_state = NULL;
	struct ta_head *ta_head;
	void *p;
	size_t vasize;

	if (!crypto_ops.hash.get_ctx_size || !crypto_ops.hash.init ||
	    !crypto_ops.hash.update || !crypto_ops.hash.final) {
		res = TEE_ERROR_NOT_IMPLEMENTED;
		goto out;
	}
	hash_algo = TEE_DIGEST_HASH_TO_ALGO(shdr->algo);
	res = crypto_ops.hash.get_ctx_size(hash_algo, &hash_ctx_size);
	if (res != TEE_SUCCESS)
		goto out;
	hash_ctx = malloc(hash_ctx_size);
	if (!hash_ctx) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}
	res = crypto_ops.hash.init(hash_ctx, hash_algo);
	if (res != TEE_SUCCESS)
		goto out;
	res = crypto_ops.hash.update(hash_ctx, hash_algo,
				     (uint8_t *)shdr, sizeof(struct shdr));
	if (res != TEE_SUCCESS)
		goto out;

	res = elf_load_init(hash_ctx, hash_algo, nwdata, nwdata_len,
			    &elf_state);
	if (res != TEE_SUCCESS)
		goto out;

	res = elf_load_head(elf_state, sizeof(struct ta_head), &p, &vasize,
			    &ctx->is_32bit);
	if (res != TEE_SUCCESS)
		goto out;
	ta_head = p;

	ctx->mm = tee_mm_alloc(&tee_mm_sec_ddr, vasize);
	if (!ctx->mm) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}

	/* Currently all TA must execute from DDR */
	if (!(ta_head->flags & TA_FLAG_EXEC_DDR)) {
		res = TEE_ERROR_BAD_FORMAT;
		goto out;
	}
	/* Temporary assignment to setup memory mapping */
	ctx->flags = TA_FLAG_EXEC_DDR;

	/* Ensure proper aligment of stack */
	ctx->stack_size = ROUNDUP(ta_head->stack_size,
				  TEE_TA_STACK_ALIGNMENT);

	ctx->mm_stack = tee_mm_alloc(&tee_mm_sec_ddr, ctx->stack_size);
	if (!ctx->mm_stack) {
		EMSG("Failed to allocate %zu bytes for user stack",
		     ctx->stack_size);
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}

	/*
	 * Map physical memory into TA virtual memory
	 */

	res = tee_mmu_init(ctx);
	if (res != TEE_SUCCESS)
		goto out;

	res = tee_mmu_map(ctx, &param);
	if (res != TEE_SUCCESS)
		goto out;

	tee_mmu_set_ctx(ctx);

	res = elf_load_body(elf_state, tee_mmu_get_load_addr(ctx));
	if (res != TEE_SUCCESS)
		goto out;

	digest = malloc(shdr->hash_size);
	if (!digest) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto out;
	}

	res = crypto_ops.hash.final(hash_ctx, hash_algo, digest,
				    shdr->hash_size);
	if (res != TEE_SUCCESS)
		goto out;

	if (memcmp(digest, SHDR_GET_HASH(shdr), shdr->hash_size) != 0)
		res = TEE_ERROR_SECURITY;

	cache_maintenance_l1(DCACHE_AREA_CLEAN,
			     (void *)tee_mmu_get_load_addr(ctx), vasize);
	cache_maintenance_l1(ICACHE_AREA_INVALIDATE,
			     (void *)tee_mmu_get_load_addr(ctx), vasize);
out:
	elf_load_final(elf_state);
	free(digest);
	free(hash_ctx);
	return res;
}

/*-----------------------------------------------------------------------------
 * Loads TA header and hashes.
 * Verifies the TA signature.
 * Returns context ptr and TEE_Result.
 *---------------------------------------------------------------------------*/
static TEE_Result ta_load(const TEE_UUID *uuid, const struct shdr *signed_ta,
			struct tee_ta_ctx **ta_ctx)
{
	TEE_Result res;
	/* man_flags: mandatory flags */
	uint32_t man_flags = TA_FLAG_USER_MODE | TA_FLAG_EXEC_DDR;
	/* opt_flags: optional flags */
	uint32_t opt_flags = man_flags | TA_FLAG_SINGLE_INSTANCE |
	    TA_FLAG_MULTI_SESSION | TA_FLAG_UNSAFE_NW_PARAMS |
	    TA_FLAG_INSTANCE_KEEP_ALIVE;
	struct tee_ta_ctx *ctx = NULL;
	struct shdr *sec_shdr = NULL;
	struct ta_head *ta_head;

	res = load_header(signed_ta, &sec_shdr);
	if (res != TEE_SUCCESS)
		goto error_return;

	res = check_shdr(sec_shdr);
	if (res != TEE_SUCCESS)
		goto error_return;

	/*
	 * ------------------------------------------------------------------
	 * 2nd step: Register context
	 * Alloc and init the ta context structure, alloc physical/virtual
	 * memories to store/map the TA.
	 * ------------------------------------------------------------------
	 */

	/*
	 * Register context
	 */

	/* code below must be protected by mutex (multi-threaded) */
	ctx = calloc(1, sizeof(struct tee_ta_ctx));
	if (ctx == NULL) {
		res = TEE_ERROR_OUT_OF_MEMORY;
		goto error_return;
	}
	TAILQ_INIT(&ctx->open_sessions);
	TAILQ_INIT(&ctx->cryp_states);
	TAILQ_INIT(&ctx->objects);
	TAILQ_INIT(&ctx->storage_enums);
#if defined(CFG_SE_API)
	ctx->se_service = NULL;
#endif

	res = load_elf(ctx, sec_shdr, signed_ta);
	if (res != TEE_SUCCESS)
		goto error_return;

	ctx->load_addr = tee_mmu_get_load_addr(ctx);
	ta_head = (struct ta_head *)(vaddr_t)ctx->load_addr;

	if (memcmp(&ta_head->uuid, uuid, sizeof(TEE_UUID)) != 0) {
		res = TEE_ERROR_SECURITY;
		goto error_return;
	}

	/* check input flags bitmask consistency and save flags */
	if ((ta_head->flags & opt_flags) != ta_head->flags ||
	    (ta_head->flags & man_flags) != man_flags) {
		EMSG("TA flag issue: flags=%x opt=%X man=%X",
		     ta_head->flags, opt_flags, man_flags);
		res = TEE_ERROR_BAD_FORMAT;
		goto error_return;
	}

	ctx->flags = ta_head->flags;
	ctx->uuid = ta_head->uuid;
	ctx->entry_func = ta_head->entry.ptr64;

	ctx->ref_count = 1;

	condvar_init(&ctx->busy_cv);
	TAILQ_INSERT_TAIL(&tee_ctxes, ctx, link);
	*ta_ctx = ctx;

	DMSG("Loaded TA at 0x%" PRIxPTR, tee_mm_get_smem(ctx->mm));
	DMSG("ELF load address 0x%x", ctx->load_addr);

	tee_mmu_set_ctx(NULL);
	/* end thread protection (multi-threaded) */

	free(sec_shdr);
	return TEE_SUCCESS;

error_return:
	free(sec_shdr);
	tee_mmu_set_ctx(NULL);
	if (ctx != NULL) {
		tee_mmu_final(ctx);
		tee_mm_free(ctx->mm_stack);
		tee_mm_free(ctx->mm);
		free(ctx);
	}
	return res;
}

static void init_utee_param(struct utee_params *up,
			const struct tee_ta_param *p)
{
	size_t n;

	up->types = p->types;
	for (n = 0; n < TEE_NUM_PARAMS; n++) {
		uintptr_t a;
		uintptr_t b;

		switch (TEE_PARAM_TYPE_GET(p->types, n)) {
		case TEE_PARAM_TYPE_MEMREF_INPUT:
		case TEE_PARAM_TYPE_MEMREF_OUTPUT:
		case TEE_PARAM_TYPE_MEMREF_INOUT:
			a = (uintptr_t)p->params[n].memref.buffer;
			b = p->params[n].memref.size;
			break;
		case TEE_PARAM_TYPE_VALUE_INPUT:
		case TEE_PARAM_TYPE_VALUE_INOUT:
			a = p->params[n].value.a;
			b = p->params[n].value.b;
			break;
		default:
			a = 0;
			b = 0;
			break;
		}
		/* See comment for struct utee_params in utee_types.h */
		up->vals[n * 2] = a;
		up->vals[n * 2 + 1] = b;
	}
}

static void update_from_utee_param(struct tee_ta_param *p,
			const struct utee_params *up)
{
	size_t n;

	for (n = 0; n < TEE_NUM_PARAMS; n++) {
		switch (TEE_PARAM_TYPE_GET(p->types, n)) {
		case TEE_PARAM_TYPE_MEMREF_OUTPUT:
		case TEE_PARAM_TYPE_MEMREF_INOUT:
			/* See comment for struct utee_params in utee_types.h */
			p->params[n].memref.size = up->vals[n * 2 + 1];
			break;
		case TEE_PARAM_TYPE_VALUE_OUTPUT:
		case TEE_PARAM_TYPE_VALUE_INOUT:
			/* See comment for struct utee_params in utee_types.h */
			p->params[n].value.a = up->vals[n * 2];
			p->params[n].value.b = up->vals[n * 2 + 1];
			break;
		default:
			break;
		}
	}
}

static TEE_Result user_ta_enter(TEE_ErrorOrigin *err,
			struct tee_ta_session *session,
			enum utee_entry_func func, uint32_t cmd,
			struct tee_ta_param *param)
{
	TEE_Result res;
	struct utee_params *usr_params;
	tee_paddr_t usr_stack;
	tee_uaddr_t stack_uaddr;
	struct tee_ta_ctx *ctx = session->ctx;
	tee_uaddr_t params_uaddr;
	TEE_ErrorOrigin serr = TEE_ORIGIN_TEE;

	TEE_ASSERT((ctx->flags & TA_FLAG_EXEC_DDR) != 0);

	/* Map user space memory */
	res = tee_mmu_map(ctx, param);
	if (res != TEE_SUCCESS)
		goto cleanup_return;

	/* Switch to user ctx */
	tee_ta_set_current_session(session);

	/* Make room for usr_params at top of stack */
	usr_stack = tee_mm_get_smem(ctx->mm_stack) + ctx->stack_size;
	usr_stack -= sizeof(struct utee_params);
	usr_params = (struct utee_params *)usr_stack;
	init_utee_param(usr_params, param);

	res = tee_mmu_kernel_to_user(ctx, (tee_vaddr_t)usr_params,
				     &params_uaddr);
	if (res != TEE_SUCCESS)
		goto cleanup_return;

	res = tee_mmu_kernel_to_user(ctx, usr_stack, &stack_uaddr);
	if (res != TEE_SUCCESS)
		goto cleanup_return;

	res = thread_enter_user_mode(func, tee_svc_kaddr_to_uref(session),
				     params_uaddr, cmd, stack_uaddr,
				     ctx->entry_func, ctx->is_32bit,
				     &ctx->panicked, &ctx->panic_code);
	/*
	 * According to GP spec the origin should allways be set to the
	 * TA after TA execution
	 */
	serr = TEE_ORIGIN_TRUSTED_APP;

	if (ctx->panicked) {
		DMSG("tee_user_ta_enter: TA panicked with code 0x%x\n",
		     ctx->panic_code);
		serr = TEE_ORIGIN_TEE;
		res = TEE_ERROR_TARGET_DEAD;
	}

	/* Copy out value results */
	update_from_utee_param(param, usr_params);

cleanup_return:
	/* Restore original ROM mapping */
	tee_ta_set_current_session(NULL);

	/*
	 * Clear the cancel state now that the user TA has returned. The next
	 * time the TA will be invoked will be with a new operation and should
	 * not have an old cancellation pending.
	 */
	session->cancel = false;

	/*
	 * Can't update *err until now since it may point to an address
	 * mapped for the user mode TA.
	 */
	*err = serr;

	return res;
}

static TEE_Result rpc_free(uint32_t handle)
{
	struct teesmc32_param params;

	memset(&params, 0, sizeof(params));
	params.attr = TEESMC_ATTR_TYPE_VALUE_INPUT;
	params.u.value.a = handle;
	return thread_rpc_cmd(TEE_RPC_FREE_TA, 1, &params);
}

/*
 * Load a TA via RPC with UUID defined by input param uuid. The virtual
 * address of the TA is recieved in out parameter ta
 *
 * Function is not thread safe
 */
static TEE_Result rpc_load(const TEE_UUID *uuid, struct shdr **ta,
			uint32_t *handle)
{
	TEE_Result res;
	struct teesmc32_param params[2];
	paddr_t phpayload = 0;
	paddr_t cookie = 0;
	struct tee_rpc_load_ta_cmd *cmd_load_ta;
	uint32_t lhandle;

	if (!uuid || !ta || !handle)
		return TEE_ERROR_BAD_PARAMETERS;

	/* get a rpc buffer */
	thread_optee_rpc_alloc_payload(sizeof(struct tee_rpc_load_ta_cmd),
				   &phpayload, &cookie);
	if (!phpayload)
		return TEE_ERROR_OUT_OF_MEMORY;

	if (!ALIGNMENT_IS_OK(phpayload, struct tee_rpc_load_ta_cmd)) {
		res = TEE_ERROR_GENERIC;
		goto out;
	}

	if (core_pa2va(phpayload, &cmd_load_ta)) {
		res = TEE_ERROR_GENERIC;
		goto out;
	}

	memset(params, 0, sizeof(params));
	params[0].attr = TEESMC_ATTR_TYPE_MEMREF_INOUT |
			 TEESMC_ATTR_CACHE_DEFAULT << TEESMC_ATTR_CACHE_SHIFT;
	params[1].attr = TEESMC_ATTR_TYPE_MEMREF_OUTPUT |
			 TEESMC_ATTR_CACHE_DEFAULT << TEESMC_ATTR_CACHE_SHIFT;

	params[0].u.memref.buf_ptr = phpayload;
	params[0].u.memref.size = sizeof(struct tee_rpc_load_ta_cmd);
	params[1].u.memref.buf_ptr = 0;
	params[1].u.memref.size = 0;

	memset(cmd_load_ta, 0, sizeof(struct tee_rpc_load_ta_cmd));
	memcpy(&cmd_load_ta->uuid, uuid, sizeof(TEE_UUID));

	res = thread_rpc_cmd(TEE_RPC_LOAD_TA, 2, params);
	if (res != TEE_SUCCESS) {
		goto out;
	}

	lhandle = cmd_load_ta->supp_ta_handle;
	if (core_pa2va(params[1].u.memref.buf_ptr, ta)) {
		rpc_free(lhandle);
		res = TEE_ERROR_GENERIC;
		goto out;
	}
	*handle = lhandle;

out:
	thread_optee_rpc_free_payload(cookie);
	return res;
}

static TEE_Result init_session_with_signed_ta(const TEE_UUID *uuid,
				const struct shdr *signed_ta,
				struct tee_ta_session *s)
{
	TEE_Result res;

	DMSG("   Load dynamic TA");
	/* load and verify */
	res = ta_load(uuid, signed_ta, &s->ctx);
	if (res != TEE_SUCCESS)
		return res;

	DMSG("      dyn TA : %pUl", (void *)&s->ctx->uuid);

	return res;
}

static TEE_Result user_ta_enter_open_session(struct tee_ta_session *s,
			struct tee_ta_param *param, TEE_ErrorOrigin *eo)
{
	return user_ta_enter(eo, s, UTEE_ENTRY_FUNC_OPEN_SESSION, 0, param);
}

static TEE_Result user_ta_enter_invoke_cmd(struct tee_ta_session *s,
			uint32_t cmd, struct tee_ta_param *param,
			TEE_ErrorOrigin *eo)
{
	return user_ta_enter(eo, s, UTEE_ENTRY_FUNC_INVOKE_COMMAND, cmd, param);
}

static void user_ta_enter_close_session(struct tee_ta_session *s)
{
	TEE_ErrorOrigin eo;
	struct tee_ta_param param = { 0 };

	user_ta_enter(&eo, s, UTEE_ENTRY_FUNC_CLOSE_SESSION, 0, &param);
}

static const struct tee_ta_ops user_ta_ops = {
	.enter_open_session = user_ta_enter_open_session,
	.enter_invoke_cmd = user_ta_enter_invoke_cmd,
	.enter_close_session = user_ta_enter_close_session,
};

TEE_Result tee_ta_init_user_ta_session(const TEE_UUID *uuid,
			struct tee_ta_session *s)
{
	TEE_Result res;
	struct shdr *ta = NULL;
	uint32_t handle = 0;


	/* Request TA from tee-supplicant */
	res = rpc_load(uuid, &ta, &handle);
	if (res != TEE_SUCCESS)
		return res;

	res = init_session_with_signed_ta(uuid, ta, s);
	/*
	 * Free normal world shared memory now that the TA either has been
	 * copied into secure memory or the TA failed to be initialized.
	 */
	rpc_free(handle);

	if (res == TEE_SUCCESS)
		s->ctx->ops = &user_ta_ops;
	return res;
}