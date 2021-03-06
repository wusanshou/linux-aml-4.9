/*
 * drivers/amlogic/crypto/aml-sha-dma.c
 *
 * Copyright (C) 2017 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/hw_random.h>
#include <linux/platform_device.h>

#include <linux/device.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/delay.h>
#include <linux/crypto.h>
#include <linux/cryptohash.h>
#include <crypto/scatterwalk.h>
#include <crypto/algapi.h>
#include <crypto/sha.h>
#include <crypto/hash.h>
#include <crypto/internal/hash.h>
#include <linux/amlogic/iomap.h>
#include <linux/amlogic/cpu_version.h>
#include "aml-crypto-dma.h"

/* SHA flags */
#define SHA_FLAGS_BUSY			BIT(0)
#define	SHA_FLAGS_FINAL			BIT(1)
#define SHA_FLAGS_DMA_ACTIVE		BIT(2)
#define SHA_FLAGS_OUTPUT_READY		BIT(3)
#define SHA_FLAGS_INIT			BIT(4)
#define SHA_FLAGS_DMA_READY		BIT(5)
#define SHA_FLAGS_DMA_FAST		BIT(6)

#define SHA_FLAGS_FINUP         BIT(16)
#define SHA_FLAGS_SHA1	        BIT(17)
#define SHA_FLAGS_SHA224        BIT(18)
#define SHA_FLAGS_SHA256        BIT(19)
#define SHA_FLAGS_HMAC          BIT(20)
#define SHA_FLAGS_ERROR	        BIT(21)

#define SHA_OP_UPDATE	1
#define SHA_OP_FINAL	2

#define SHA_BUFFER_LEN		PAGE_SIZE

#define DMA_THREAD_REG (DMA_T0 + SHA_THREAD_INDEX)
#define DMA_STATUS_REG (DMA_STS0 + SHA_THREAD_INDEX)
u8 map_in_sha_dma;
struct aml_sha_dev;

struct aml_sha_reqctx {
	struct aml_sha_dev	*dd;
	unsigned long	flags;
	unsigned long	op;

	u64	digcnt[2];
	size_t	bufcnt;
	size_t	buflen;
	dma_addr_t	dma_addr;
	dma_addr_t	hash_addr;

	/* walk state */
	struct scatterlist	*sg;
	unsigned int	offset;	/* offset in current sg */
	unsigned int	total;	/* total request */

	size_t block_size;
	uint32_t fast_nents;

	u8 *digest;
	u8	buffer[0] __aligned(sizeof(u32));
};

struct aml_sha_ctx {
	struct aml_sha_dev	*dd;
	u8			key[SHA256_BLOCK_SIZE];
	u32			keylen;
	unsigned long		flags;
};

#define AML_SHA_QUEUE_LENGTH	50

struct aml_sha_dev {
	struct list_head	list;
	struct device		*dev;
	int			irq;

	spinlock_t		lock;
	int			err;
	struct tasklet_struct	done_task;

	unsigned long		flags;
	struct crypto_queue	queue;
	struct ahash_request	*req;

	void	*descriptor;
	dma_addr_t	dma_descript_tab;
};

struct aml_sha_drv {
	struct list_head	dev_list;
	spinlock_t		lock;
};

static struct aml_sha_drv aml_sha = {
	.dev_list = LIST_HEAD_INIT(aml_sha.dev_list),
	.lock = __SPIN_LOCK_UNLOCKED(aml_sha.lock),
};

static size_t aml_sha_append_sg(struct aml_sha_reqctx *ctx)
{
	size_t count = 0;

	while ((ctx->bufcnt < ctx->buflen) && ctx->total) {
		count = min(ctx->sg->length - ctx->offset, ctx->total);
		count = min(count, ctx->buflen - ctx->bufcnt);

		if (count <= 0)
			break;

		scatterwalk_map_and_copy(ctx->buffer + ctx->bufcnt, ctx->sg,
			ctx->offset, count, 0);

		ctx->bufcnt += count;
		ctx->offset += count;
		ctx->total -= count;

		if (ctx->offset == ctx->sg->length) {
			ctx->sg = sg_next(ctx->sg);
			if (ctx->sg)
				ctx->offset = 0;
			else
				ctx->total = 0;
		}
	}

	return 0;
}

static int aml_sha_init(struct ahash_request *req)
{
	struct crypto_ahash *tfm = crypto_ahash_reqtfm(req);
	struct aml_sha_ctx *tctx = crypto_ahash_ctx(tfm);
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	struct aml_sha_dev *dd = NULL;
	struct aml_sha_dev *tmp;

	spin_lock_bh(&aml_sha.lock);
	if (!tctx->dd) {
		list_for_each_entry(tmp, &aml_sha.dev_list, list) {
			dd = tmp;
			break;
		}
		tctx->dd = dd;
	} else {
		dd = tctx->dd;
	}

	spin_unlock_bh(&aml_sha.lock);

	ctx->dd = dd;

	ctx->flags = 0;

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "init: digest size: %d\n",
		crypto_ahash_digestsize(tfm));
#endif

	switch (crypto_ahash_digestsize(tfm)) {
	case SHA1_DIGEST_SIZE:
		ctx->flags |= SHA_FLAGS_SHA1;
		ctx->block_size = SHA1_BLOCK_SIZE;
		break;
	case SHA224_DIGEST_SIZE:
		ctx->flags |= SHA_FLAGS_SHA224;
		ctx->block_size = SHA224_BLOCK_SIZE;
		break;
	case SHA256_DIGEST_SIZE:
		ctx->flags |= SHA_FLAGS_SHA256;
		ctx->block_size = SHA256_BLOCK_SIZE;
		break;
	default:
		return -EINVAL;
	}

	ctx->digest = 0;
	ctx->bufcnt = 0;
	ctx->digcnt[0] = 0;
	ctx->digcnt[1] = 0;
	ctx->fast_nents = 0;
	ctx->buflen = SHA_BUFFER_LEN;

	return 0;
}

static int aml_sha_xmit_dma(struct aml_sha_dev *dd, struct dma_dsc *dsc,
		uint32_t nents, int final)
{
	int i = 0;
	u32 mode;
	struct aml_sha_ctx *tctx = crypto_tfm_ctx(dd->req->base.tfm);
	struct aml_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	size_t length = 0;

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "xmit_dma: digcnt: 0x%llx 0x%llx, nents: %u, final: %d\n",
		ctx->digcnt[1], ctx->digcnt[0], nents, final);
#endif

	mode = MODE_SHA1;

	if (ctx->flags & SHA_FLAGS_SHA224)
		mode = MODE_SHA224;
	else if (ctx->flags & SHA_FLAGS_SHA256)
		mode = MODE_SHA256;

	if (final) {
		kfree(ctx->digest);
		ctx->digest = kzalloc(SHA256_DIGEST_SIZE, GFP_KERNEL);
		ctx->hash_addr = dma_map_single(dd->dev, ctx->digest,
				SHA256_DIGEST_SIZE, DMA_FROM_DEVICE);
		if (dma_mapping_error(dd->dev, ctx->hash_addr)) {
			dev_err(dd->dev, "hash %d bytes error\n",
					SHA256_DIGEST_SIZE);
			return -EINVAL;
		}
	} else {
		ctx->hash_addr = 0;
	}

	for (i = 0; i < nents; i++) {
		dsc[i].tgt_addr = (uintptr_t)ctx->hash_addr;
		dsc[i].dsc_cfg.b.enc_sha_only = 1;
		dsc[i].dsc_cfg.b.mode = mode;
		dsc[i].dsc_cfg.b.begin =
			(i == 0 && !(ctx->digcnt[0] || ctx->digcnt[1]) &&
			 !(tctx->flags & SHA_FLAGS_HMAC));
		dsc[i].dsc_cfg.b.end = final;
		dsc[i].dsc_cfg.b.op_mode = OP_MODE_SHA;
		dsc[i].dsc_cfg.b.eoc = (i == (nents - 1));
		dsc[i].dsc_cfg.b.owner = 1;
	}
	dma_sync_single_for_device(dd->dev, dd->dma_descript_tab,
			PAGE_SIZE, DMA_TO_DEVICE);

	aml_dma_debug(dsc, nents, __func__);
	/* should be non-zero before next lines to disable clocks later */
	for (i = 0; i < nents; i++) {
		length = dsc->dsc_cfg.b.length;
		ctx->digcnt[0] += length;
		if (ctx->digcnt[0] < length)
			ctx->digcnt[1]++;
	}

	if (final)
		dd->flags |= SHA_FLAGS_FINAL; /* catch last interrupt */

	dd->flags |=  SHA_FLAGS_DMA_ACTIVE;

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev,
		"xmit before : digcnt: 0x%llx 0x%llx, length: %zd, final: %d\n",
		ctx->digcnt[1], ctx->digcnt[0], length, final);
#endif
	/* Start DMA transfer */
	aml_write_crypto_reg(DMA_THREAD_REG,
			(uintptr_t) dd->dma_descript_tab | 2);

	return -EINPROGRESS;
}

static int aml_sha_xmit_start(struct aml_sha_dev *dd, struct dma_dsc *dsc,
		uint32_t nents, int final)
{
	return aml_sha_xmit_dma(dd, dsc, nents, final);
}

static int aml_sha_xmit_dma_map(struct aml_sha_dev *dd,
					struct aml_sha_reqctx *ctx,
					size_t length, int final)
{
	struct dma_dsc *dsc = dd->descriptor;

	ctx->dma_addr = dma_map_single(dd->dev, ctx->buffer,
				ctx->buflen, DMA_TO_DEVICE);
	if (dma_mapping_error(dd->dev, ctx->dma_addr)) {
		dev_err(dd->dev, "dma %zd bytes error\n", ctx->buflen);
		return -EINVAL;
	}

	dma_sync_single_for_device(dd->dev, ctx->dma_addr,
			SHA_BUFFER_LEN, DMA_TO_DEVICE);

	dsc->src_addr = (uintptr_t)ctx->dma_addr;
	dsc->dsc_cfg.d32 = 0;
	dsc->dsc_cfg.b.length = length;

	/* next call does not fail... so no unmap in the case of error */
	return aml_sha_xmit_start(dd, dsc, 1, final);
}

static int aml_sha_update_dma_slow(struct aml_sha_dev *dd)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	unsigned int final;
	size_t count;

	ctx->flags &= ~SHA_FLAGS_DMA_FAST;
	aml_sha_append_sg(ctx);

	final = (ctx->flags & SHA_FLAGS_FINUP) && !ctx->total;

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev,
	"slow: bufcnt: %zd, digcnt: 0x%llx 0x%llx, final: %d, total: %u\n",
	 ctx->bufcnt, ctx->digcnt[1], ctx->digcnt[0], final, ctx->total);
#endif

	if (IS_ALIGNED(ctx->bufcnt, ctx->block_size) || final) {
		count = ctx->bufcnt;
		ctx->bufcnt = 0;
		return aml_sha_xmit_dma_map(dd, ctx, count, final);
	}
	return 0;
}

static int aml_sha_update_dma_start(struct aml_sha_dev *dd)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	unsigned int length = 0, final = 0, tail = 0;
	struct scatterlist *sg;
	struct dma_dsc *dsc = dd->descriptor;

#if AML_CRYPTO_DEBUG
		dev_info(dd->dev, "start: total: %u, fast_nents: %u offset: %u\n",
				ctx->total, ctx->fast_nents, ctx->offset);
#endif

	if (!ctx->total)
		return 0;

	/* walk across (nents - 1) sg */
	/* because the last sg maybe only partially used */
	while (ctx->fast_nents > 1) {
		ctx->sg = sg_next(ctx->sg);
		ctx->fast_nents--;
	}

	if (ctx->offset == ctx->sg->length) {
		ctx->sg = sg_next(ctx->sg);
		if (ctx->sg)
			ctx->offset = 0;
		else
			return 0;
	}

	ctx->fast_nents = 0;

	if (ctx->bufcnt || ctx->offset || ctx->total < ctx->block_size)
		return aml_sha_update_dma_slow(dd);

	sg = ctx->sg;

	while (ctx->total && ctx->fast_nents < MAX_NUM_TABLES && sg) {
#if AML_CRYPTO_DEBUG
		dev_info(dd->dev,
		"fast: dig: 0x%llx 0x%llx, bufcnt: %zd, total: %u, sglen: %u\n",
		ctx->digcnt[1], ctx->digcnt[0],
		ctx->bufcnt, ctx->total, ctx->sg->length);
#endif

		length = min(ctx->total, sg->length);

		if (!(ctx->flags & SHA_FLAGS_FINUP)) {
			/* not last sg must be ctx->block_size aligned */
			tail = length & (ctx->block_size - 1);
			length -= tail;
		}

		ctx->total -= length;
		ctx->offset = length; /* offset where to start slow */

		final = (ctx->flags & SHA_FLAGS_FINUP) && !ctx->total;

		dma_map_sg(dd->dev, sg, 1, DMA_TO_DEVICE);

		ctx->dma_addr = sg_dma_address(sg);
		dsc[ctx->fast_nents].src_addr = (uintptr_t)ctx->dma_addr;
		dsc[ctx->fast_nents].dsc_cfg.d32 = 0;
		dsc[ctx->fast_nents].dsc_cfg.b.length = length;

		sg = sg_next(sg);
		ctx->fast_nents++;

#if AML_CRYPTO_DEBUG
		dev_info(dd->dev, "fast: total: %u, offset: %u, tail: %u\n",
				ctx->total, ctx->offset, tail);
#endif

		if (tail)
			break;
	}
	if (ctx->fast_nents) {
		ctx->flags |= SHA_FLAGS_DMA_FAST;
		return aml_sha_xmit_start(dd, dsc, ctx->fast_nents, final);
	}

	return 0;
}

static int aml_sha_update_dma_stop(struct aml_sha_dev *dd)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(dd->req);

	if (ctx->flags & SHA_FLAGS_DMA_FAST)
		dma_unmap_sg(dd->dev, ctx->sg, ctx->fast_nents, DMA_TO_DEVICE);
	else
		dma_unmap_single(dd->dev, ctx->dma_addr,
				ctx->buflen, DMA_TO_DEVICE);
	if (ctx->hash_addr)
		dma_unmap_single(dd->dev, ctx->hash_addr,
				SHA256_DIGEST_SIZE, DMA_FROM_DEVICE);
	return 0;
}

static int aml_sha_update_req(struct aml_sha_dev *dd)
{
	int err;
#if AML_CRYPTO_DEBUG
	struct ahash_request *req = dd->req;
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
#endif

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "update_req: total: %u, digcnt: 0x%llx 0x%llx\n",
		ctx->total, ctx->digcnt[1], ctx->digcnt[0]);
#endif

	err = aml_sha_update_dma_start(dd);

	return err;
}

static int aml_sha_final_req(struct aml_sha_dev *dd)
{
	int err = 0;

	err = aml_sha_update_dma_slow(dd);

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "final_req: err: %d\n", err);
#endif

	return err;
}

static void aml_sha_copy_ready_hash(struct ahash_request *req)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);

	if (!req->result)
		return;

	if (!ctx->digest)
		return;

	if (ctx->flags & SHA_FLAGS_SHA1)
		memcpy(req->result, ctx->digest, SHA1_DIGEST_SIZE);
	else if (ctx->flags & SHA_FLAGS_SHA224)
		memcpy(req->result, ctx->digest, SHA224_DIGEST_SIZE);
	else if (ctx->flags & SHA_FLAGS_SHA256)
		memcpy(req->result, ctx->digest, SHA256_DIGEST_SIZE);

	kfree(ctx->digest);
}

static int aml_sha_finish_hmac(struct ahash_request *req)
{
	struct aml_sha_ctx *tctx = crypto_tfm_ctx(req->base.tfm);
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	struct aml_sha_dev *dd = ctx->dd;
	struct dma_dsc *dsc = dd->descriptor;
	u32 mode;
	u32 ds = 0;
	u8 *key;
	dma_addr_t dma_key = 0;

	if (!ctx->digest)
		return -1;

	ctx->hash_addr = dma_map_single(dd->dev, ctx->digest,
			SHA256_DIGEST_SIZE, DMA_BIDIRECTIONAL);

	key = kmalloc(tctx->keylen, GFP_KERNEL);
	memcpy(key, tctx->key, tctx->keylen);
	dma_key = dma_map_single(dd->dev, key,
			tctx->keylen, DMA_TO_DEVICE);

	mode = MODE_SHA1;
	if (ctx->flags & SHA_FLAGS_SHA224)
		mode = MODE_SHA224;
	else if (ctx->flags & SHA_FLAGS_SHA256)
		mode = MODE_SHA256;

	if (ctx->flags & SHA_FLAGS_SHA1)
		ds = SHA1_DIGEST_SIZE;
	else if (ctx->flags & SHA_FLAGS_SHA224)
		ds = SHA224_DIGEST_SIZE;
	else if (ctx->flags & SHA_FLAGS_SHA256)
		ds = SHA256_DIGEST_SIZE;
	/* opad */
	dsc[0].src_addr = (uintptr_t)dma_key;
	dsc[0].tgt_addr = 0;
	dsc[0].dsc_cfg.d32 = 0;
	dsc[0].dsc_cfg.b.length = tctx->keylen;
	dsc[0].dsc_cfg.b.mode = mode;
	dsc[0].dsc_cfg.b.enc_sha_only = 1;
	dsc[0].dsc_cfg.b.op_mode = OP_MODE_HMAC_O;
	dsc[0].dsc_cfg.b.begin = 1;
	dsc[0].dsc_cfg.b.end = 0;
	dsc[0].dsc_cfg.b.eoc = 0;
	dsc[0].dsc_cfg.b.owner = 1;

	/* 2nd stage hash */
	dsc[1].src_addr = (uintptr_t)ctx->hash_addr;
	dsc[1].tgt_addr = (uintptr_t)ctx->hash_addr;
	dsc[1].dsc_cfg.d32 = 0;
	dsc[1].dsc_cfg.b.length = ds;
	dsc[1].dsc_cfg.b.mode = mode;
	dsc[1].dsc_cfg.b.enc_sha_only = 1;
	dsc[1].dsc_cfg.b.begin = 0;
	dsc[1].dsc_cfg.b.end = 1;
	dsc[1].dsc_cfg.b.eoc = 1;
	dsc[1].dsc_cfg.b.owner = 1;

	dma_sync_single_for_device(dd->dev, dd->dma_descript_tab,
			PAGE_SIZE, DMA_TO_DEVICE);
	aml_dma_debug(dsc, 2, __func__);
	aml_write_crypto_reg(DMA_THREAD_REG,
			(uintptr_t) dd->dma_descript_tab | 2);
	while (aml_read_crypto_reg(DMA_STATUS_REG) == 0)
		;
	aml_write_crypto_reg(DMA_STATUS_REG, 0xf);
	dma_unmap_single(dd->dev, dma_key,
			tctx->keylen, DMA_TO_DEVICE);
	dma_unmap_single(dd->dev, ctx->hash_addr,
			SHA256_DIGEST_SIZE, DMA_BIDIRECTIONAL);

	return 0;
}

static int aml_sha_finish(struct ahash_request *req)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	struct aml_sha_ctx *tctx = crypto_tfm_ctx(req->base.tfm);
	int err = 0;
#if AML_CRYPTO_DEBUG
	struct aml_sha_dev *dd = ctx->dd;
#endif

	if (ctx->digcnt[0] || ctx->digcnt[1]) {
		if (tctx->flags & SHA_FLAGS_HMAC)
			err = aml_sha_finish_hmac(req);
		aml_sha_copy_ready_hash(req);
	}

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "finish digcnt: 0x%llx 0x%llx, bufcnt: %zd\n",
			ctx->digcnt[1], ctx->digcnt[0], ctx->bufcnt);
#endif

	return err;
}

static void aml_sha_finish_req(struct ahash_request *req, int err)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	struct aml_sha_dev *dd = ctx->dd;

	if (!err) {
		if (SHA_FLAGS_FINAL & dd->flags)
			err = aml_sha_finish(req);
	} else {
		ctx->flags |= SHA_FLAGS_ERROR;
	}

	/* atomic operation is not needed here */
	dd->flags &= ~(SHA_FLAGS_BUSY | SHA_FLAGS_FINAL |
			SHA_FLAGS_DMA_READY | SHA_FLAGS_OUTPUT_READY);

	if (req->base.complete)
		req->base.complete(&req->base, err);

	/* handle new request */
	tasklet_schedule(&dd->done_task);
}

static int aml_sha_hw_init(struct aml_sha_dev *dd)
{
	if (!(dd->flags & SHA_FLAGS_INIT)) {
		dd->flags |= SHA_FLAGS_INIT;
		dd->err = 0;
	}

	return 0;
}

static int aml_sha_buff_init(struct aml_sha_dev *dd)
{
	int err = -ENOMEM;

	dd->descriptor = (void *)__get_free_pages(GFP_KERNEL, 0);
	if (!dd->descriptor) {
		dev_err(dd->dev, "unable to alloc pages.\n");
		goto err_alloc;
	}

	dd->dma_descript_tab = dma_map_single(dd->dev, dd->descriptor,
			PAGE_SIZE, DMA_TO_DEVICE);

	if (dma_mapping_error(dd->dev, dd->dma_descript_tab)) {
		dev_err(dd->dev, "dma descriptor error\n");
		err = -EINVAL;
		goto err_map_descriptor;
	}

	return 0;

err_map_descriptor:
	dma_unmap_single(dd->dev, dd->dma_descript_tab, PAGE_SIZE,
			DMA_TO_DEVICE);
err_alloc:
	if (err)
		pr_err("error: %d\n", err);
	return err;
}

static int aml_sha_handle_queue(struct aml_sha_dev *dd,
				  struct ahash_request *req)
{
	struct crypto_async_request *async_req, *backlog;
	struct aml_sha_reqctx *ctx;
	unsigned long flags;
	int err = 0, ret = 0;

	spin_lock_irqsave(&dd->lock, flags);
	if (req)
		ret = ahash_enqueue_request(&dd->queue, req);

	if (SHA_FLAGS_BUSY & dd->flags) {
		spin_unlock_irqrestore(&dd->lock, flags);
		return ret;
	}

	backlog = crypto_get_backlog(&dd->queue);
	async_req = crypto_dequeue_request(&dd->queue);
	if (async_req)
		dd->flags |= SHA_FLAGS_BUSY;

	spin_unlock_irqrestore(&dd->lock, flags);

	if (!async_req)
		return ret;

	if (backlog)
		backlog->complete(backlog, -EINPROGRESS);

	req = ahash_request_cast(async_req);
	dd->req = req;
	ctx = ahash_request_ctx(req);

#if AML_CRYPTO_DEBUG
	dev_info(dd->dev, "handling new req, op: %lu, nbytes: %d\n",
						ctx->op, req->nbytes);
#endif

	err = aml_sha_hw_init(dd);

	if (err)
		goto err1;


	if (ctx->op == SHA_OP_UPDATE) {
		err = aml_sha_update_req(dd);
		/* no final() after finup() */
		if (err != -EINPROGRESS && (ctx->flags & SHA_FLAGS_FINUP))
			err = aml_sha_final_req(dd);
	} else if (ctx->op == SHA_OP_FINAL) {
		err = aml_sha_final_req(dd);
	}

err1:
	if (err != -EINPROGRESS)
		/* done_task will not finish it, so do it here */
		aml_sha_finish_req(req, err);

	return ret;
}

static int aml_sha_enqueue(struct ahash_request *req, unsigned int op)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	struct aml_sha_ctx *tctx = crypto_tfm_ctx(req->base.tfm);
	struct aml_sha_dev *dd = tctx->dd;

	ctx->op = op;

	return aml_sha_handle_queue(dd, req);
}

static int aml_sha_update(struct ahash_request *req)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);

	if (!req->nbytes)
		return 0;

	ctx->total = req->nbytes;
	ctx->sg = req->src;
	ctx->offset = 0;
	return aml_sha_enqueue(req, SHA_OP_UPDATE);
}

static int aml_sha_final(struct ahash_request *req)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);

	ctx->flags |= SHA_FLAGS_FINUP;

	if (ctx->flags & SHA_FLAGS_ERROR)
		return 0; /* uncompleted hash is not needed */

	if (ctx->bufcnt)
		return aml_sha_enqueue(req, SHA_OP_FINAL);
	else
		return aml_sha_finish(req); /* copy ready hash */
}

static int aml_sha_finup(struct ahash_request *req)
{
	struct aml_sha_reqctx *ctx = ahash_request_ctx(req);
	int err1, err2;

	ctx->flags |= SHA_FLAGS_FINUP;

	err1 = aml_sha_update(req);
	if (err1 == -EINPROGRESS || err1 == -EBUSY)
		return err1;

	/*
	 * final() has to be always called to cleanup resources
	 * even if update() failed, except EINPROGRESS
	 */
	err2 = aml_sha_final(req);

	return err1 ?: err2;
}

static int aml_sha_digest(struct ahash_request *req)
{
	return aml_sha_init(req) ?: aml_sha_finup(req);
}

static int aml_sha_setkey(struct crypto_ahash *tfm, const u8 *key,
		      unsigned int keylen)
{
	struct aml_sha_ctx *tctx = crypto_ahash_ctx(tfm);
	struct aml_sha_dev *dd = 0;
	struct aml_sha_dev *tmp = 0;
	struct dma_dsc *dsc = 0;
	uint32_t bs = 0;
	uint32_t ds = 0;
	int err = 0;
	dma_addr_t dma_key = 0;
	uint8_t *key_raw = 0;
	uint32_t mode = MODE_SHA1;
	uint32_t map_len = 0;
	uint32_t ipad = 0;

	spin_lock_bh(&aml_sha.lock);
	if (!tctx->dd) {
		list_for_each_entry(tmp, &aml_sha.dev_list, list) {
			dd = tmp;
			break;
		}
		tctx->dd = dd;
	} else {
		dd = tctx->dd;
	}
	dsc = dd->descriptor;

	spin_unlock_bh(&aml_sha.lock);

	switch (crypto_ahash_digestsize(tfm)) {
	case SHA1_DIGEST_SIZE:
		bs = SHA1_BLOCK_SIZE;
		ds = SHA1_DIGEST_SIZE;
		mode = MODE_SHA1;
		break;
	case SHA224_DIGEST_SIZE:
		bs = SHA224_BLOCK_SIZE;
		ds = SHA224_DIGEST_SIZE;
		mode = MODE_SHA224;
		break;
	case SHA256_DIGEST_SIZE:
		bs = SHA256_BLOCK_SIZE;
		ds = SHA256_DIGEST_SIZE;
		mode = MODE_SHA256;
		break;
	default:
		return -EINVAL;
	}

	if (keylen > bs)
		key_raw = kzalloc(keylen, GFP_KERNEL);
	else
		key_raw = kzalloc(bs, GFP_KERNEL);

	memcpy(key_raw, key, keylen);
	map_len = keylen > bs ? keylen : bs;
	dma_key = dma_map_single(dd->dev, key_raw,
			map_len, DMA_BIDIRECTIONAL);
	if (keylen > bs) {
		dsc[0].src_addr = (uintptr_t)dma_key;
		dsc[0].tgt_addr = (uintptr_t)dma_key;
		dsc[0].dsc_cfg.d32 = 0;
		dsc[0].dsc_cfg.b.length = keylen;
		dsc[0].dsc_cfg.b.mode = mode;
		dsc[0].dsc_cfg.b.enc_sha_only = 1;
		dsc[0].dsc_cfg.b.begin = 1;
		dsc[0].dsc_cfg.b.end = 1;
		dsc[0].dsc_cfg.b.op_mode = OP_MODE_SHA;
		dsc[0].dsc_cfg.b.eoc = 0;
		dsc[0].dsc_cfg.b.owner = 1;
		keylen = ds;
		ipad = 1;
	}

	/* ipad */
	dsc[ipad].src_addr = (uintptr_t)dma_key;
	dsc[ipad].tgt_addr = 0;
	dsc[ipad].dsc_cfg.d32 = 0;
	dsc[ipad].dsc_cfg.b.length = keylen;
	dsc[ipad].dsc_cfg.b.mode = mode;
	dsc[ipad].dsc_cfg.b.enc_sha_only = 1;
	dsc[ipad].dsc_cfg.b.op_mode = OP_MODE_HMAC_I;
	dsc[ipad].dsc_cfg.b.begin = 1;
	dsc[ipad].dsc_cfg.b.end = 0;
	dsc[ipad].dsc_cfg.b.eoc = 1;
	dsc[ipad].dsc_cfg.b.owner = 1;

	dma_sync_single_for_device(dd->dev, dd->dma_descript_tab,
			PAGE_SIZE, DMA_TO_DEVICE);
	aml_dma_debug(dsc, ipad + 1, __func__);
	aml_write_crypto_reg(DMA_THREAD_REG,
			(uintptr_t) dd->dma_descript_tab | 2);
	while (aml_read_crypto_reg(DMA_STATUS_REG) == 0)
		;
	aml_write_crypto_reg(DMA_STATUS_REG, 0xf);
	dma_unmap_single(dd->dev, dma_key,
			map_len, DMA_BIDIRECTIONAL);
	tctx->keylen = keylen;
	memcpy(tctx->key, key_raw, keylen);

	kfree(key_raw);
	return err;
}
static int aml_sha_cra_init_alg(struct crypto_tfm *tfm, const char *alg_base)
{
	crypto_ahash_set_reqsize(__crypto_ahash_cast(tfm),
				 sizeof(struct aml_sha_reqctx) +
				 SHA_BUFFER_LEN);
	return 0;
}

static int aml_sha_cra_init(struct crypto_tfm *tfm)
{
	return aml_sha_cra_init_alg(tfm, NULL);
}

static void aml_sha_cra_exit(struct crypto_tfm *tfm)
{
}

static int aml_hmac_cra_init(struct crypto_tfm *tfm)
{
	struct aml_sha_ctx *tctx = crypto_tfm_ctx(tfm);

	tctx->flags |= SHA_FLAGS_HMAC;
	return aml_sha_cra_init_alg(tfm, NULL);
}

static void aml_hmac_cra_exit(struct crypto_tfm *tfm)
{
}

static struct ahash_alg sha_algs[] = {
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.halg = {
			.digestsize	= SHA1_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "sha1",
				.cra_driver_name  = "aml-sha1",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA1_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_sha_cra_init,
				.cra_exit	  = aml_sha_cra_exit,
			}
		}
	},
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.halg = {
			.digestsize	= SHA256_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "sha256",
				.cra_driver_name  = "aml-sha256",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA256_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_sha_cra_init,
				.cra_exit	  = aml_sha_cra_exit,
			}
		}
	},
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.halg = {
			.digestsize	= SHA224_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "sha224",
				.cra_driver_name  = "aml-sha224",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA224_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_sha_cra_init,
				.cra_exit	  = aml_sha_cra_exit,
			}
		}
	},
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.setkey         = aml_sha_setkey,
		.halg = {
			.digestsize	= SHA1_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "hmac(sha1)",
				.cra_driver_name  = "aml-hmac-sha1",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA1_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_hmac_cra_init,
				.cra_exit	  = aml_hmac_cra_exit,
			}
		}
	},
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.setkey         = aml_sha_setkey,
		.halg = {
			.digestsize	= SHA224_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "hmac(sha224)",
				.cra_driver_name  = "aml-hmac-sha224",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA224_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_hmac_cra_init,
				.cra_exit	  = aml_hmac_cra_exit,
			}
		}
	},
	{
		.init		= aml_sha_init,
		.update		= aml_sha_update,
		.final		= aml_sha_final,
		.finup		= aml_sha_finup,
		.digest		= aml_sha_digest,
		.setkey         = aml_sha_setkey,
		.halg = {
			.digestsize	= SHA256_DIGEST_SIZE,
			/* although we don't support import/export, */
			/* let's cheat it. */
			.statesize =  sizeof(struct aml_sha_ctx),
			.base	= {
				.cra_name	  = "hmac(sha256)",
				.cra_driver_name  = "aml-hmac-sha256",
				.cra_priority	  = 300,
				.cra_flags	  = CRYPTO_ALG_ASYNC,
				.cra_blocksize	  = SHA256_BLOCK_SIZE,
				.cra_ctxsize	  = sizeof(struct aml_sha_ctx),
				.cra_alignmask	  = 0,
				.cra_module	  = THIS_MODULE,
				.cra_init	  = aml_hmac_cra_init,
				.cra_exit	  = aml_hmac_cra_exit,
			}
		}
	}
};

static void aml_sha_done_task(unsigned long data)
{
	struct aml_sha_dev *dd = (struct aml_sha_dev *)data;
	struct aml_sha_reqctx *ctx = ahash_request_ctx(dd->req);
	int err = 0;

	if (!(SHA_FLAGS_BUSY & dd->flags)) {
		aml_sha_handle_queue(dd, NULL);
		return;
	}

	dma_sync_single_for_cpu(dd->dev, dd->dma_descript_tab,
			PAGE_SIZE, DMA_FROM_DEVICE);
	aml_dma_debug(dd->descriptor, ctx->fast_nents ?
			ctx->fast_nents : 1, __func__);

	if (SHA_FLAGS_DMA_READY & dd->flags) {
		if (SHA_FLAGS_DMA_ACTIVE & dd->flags) {
			dd->flags &= ~SHA_FLAGS_DMA_ACTIVE;
			aml_sha_update_dma_stop(dd);
			if (dd->err) {
				err = dd->err;
				goto finish;
			}
		}
		if (SHA_FLAGS_OUTPUT_READY & dd->flags) {
			/* hash or semi-hash ready */
			dd->flags &= ~(SHA_FLAGS_DMA_READY |
					SHA_FLAGS_OUTPUT_READY);
			err = aml_sha_update_dma_start(dd);
			if (err != -EINPROGRESS)
				goto finish;
		}
	}
	return;

finish:
	/* finish curent request */
	aml_sha_finish_req(dd->req, err);
}

static irqreturn_t aml_sha_irq(int irq, void *dev_id)
{
	struct aml_sha_dev *sha_dd = dev_id;
	uint8_t status = aml_read_crypto_reg(DMA_STATUS_REG);

	if (status) {
		if (status == 0x1)
			pr_err("irq overwrite\n");
		if (SHA_FLAGS_DMA_ACTIVE & sha_dd->flags) {
			sha_dd->flags |= SHA_FLAGS_OUTPUT_READY;
			sha_dd->flags |= SHA_FLAGS_DMA_READY;
			aml_write_crypto_reg(DMA_STATUS_REG, 0xf);
			tasklet_schedule(&sha_dd->done_task);
			return IRQ_HANDLED;
		} else {
			return IRQ_NONE;
		}
	}
	return IRQ_NONE;
}

static void aml_sha_unregister_algs(struct aml_sha_dev *dd)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sha_algs); i++)
		crypto_unregister_ahash(&sha_algs[i]);
}

static int aml_sha_register_algs(struct aml_sha_dev *dd)
{
	int err, i, j;

	for (i = 0; i < ARRAY_SIZE(sha_algs); i++) {
		err = crypto_register_ahash(&sha_algs[i]);
		if (err)
			goto err_sha_algs;
	}

	return 0;

err_sha_algs:
	for (j = 0; j < i; j++)
		crypto_unregister_ahash(&sha_algs[j]);
	return err;
}

static int aml_sha_probe(struct platform_device *pdev)
{
	struct aml_sha_dev *sha_dd;
	struct device *dev = &pdev->dev;
	struct resource *res_irq = 0;
	struct resource *res_base = 0;
	int err = -EPERM;

	sha_dd = kzalloc(sizeof(struct aml_sha_dev), GFP_KERNEL);
	if (sha_dd == NULL) {
		err = -ENOMEM;
		goto sha_dd_err;
	}

	sha_dd->dev = dev;

	platform_set_drvdata(pdev, sha_dd);


	res_irq = platform_get_resource(pdev, IORESOURCE_IRQ, SHA_THREAD_INDEX);

	res_base = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res_base) {
		dev_err(dev, "error to get normal IORESOURCE_MEM.\n");
		goto sha_dd_err;
	} else {
		if (cryptoreg_offset) {
			cryptoreg_offset = ioremap(res_base->start,
					resource_size(res_base));
			map_in_sha_dma = 1;
		}
	}

	INIT_LIST_HEAD(&sha_dd->list);

	tasklet_init(&sha_dd->done_task, aml_sha_done_task,
					(unsigned long)sha_dd);

	crypto_init_queue(&sha_dd->queue, AML_SHA_QUEUE_LENGTH);

	sha_dd->irq = res_irq->start;
	err = request_irq(sha_dd->irq, aml_sha_irq, IRQF_SHARED, "aml-sha",
						sha_dd);
	if (err) {
		dev_err(dev, "unable to request sha irq.\n");
		goto res_err;
	}

	aml_sha_hw_init(sha_dd);

	err = aml_sha_buff_init(sha_dd);
	if (err)
		goto res_err;

	spin_lock(&aml_sha.lock);
	list_add_tail(&sha_dd->list, &aml_sha.dev_list);
	spin_unlock(&aml_sha.lock);

	err = aml_sha_register_algs(sha_dd);
	if (err)
		goto err_algs;

	dev_info(dev, "Aml SHA1/SHA224/SHA256 dma\n");

	return 0;

err_algs:
	spin_lock(&aml_sha.lock);
	list_del(&sha_dd->list);
	spin_unlock(&aml_sha.lock);

	if (map_in_sha_dma) {

		iounmap(cryptoreg_offset);
		map_in_sha_dma = 0;
	}

	free_irq(sha_dd->irq, sha_dd);
res_err:
	tasklet_kill(&sha_dd->done_task);
	kfree(sha_dd);
	sha_dd = NULL;
sha_dd_err:
	dev_err(dev, "initialization failed.\n");

	return err;
}

static int aml_sha_remove(struct platform_device *pdev)
{
	static struct aml_sha_dev *sha_dd;

	sha_dd = platform_get_drvdata(pdev);
	if (!sha_dd)
		return -ENODEV;
	spin_lock(&aml_sha.lock);
	list_del(&sha_dd->list);
	spin_unlock(&aml_sha.lock);

	aml_sha_unregister_algs(sha_dd);

	tasklet_kill(&sha_dd->done_task);

	if (map_in_sha_dma) {

		iounmap(cryptoreg_offset);
		map_in_sha_dma = 0;
	}

	if (sha_dd->irq >= 0)
		free_irq(sha_dd->irq, sha_dd);

	kfree(sha_dd);
	sha_dd = NULL;

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id aml_sha_dt_match[] = {
	{	.compatible = "amlogic,sha_dma",
	},
	{},
};
#else
#define aml_aes_dt_match NULL
#endif
static struct platform_driver aml_sha_driver = {
	.probe		= aml_sha_probe,
	.remove		= aml_sha_remove,
	.driver		= {
		.name	= "aml_sha_dma",
		.owner	= THIS_MODULE,
		.of_match_table	= aml_sha_dt_match,
	},
};

module_platform_driver(aml_sha_driver);

MODULE_DESCRIPTION("Aml SHA (1/256/224) hw acceleration support.");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("matthew.shyu <matthew.shyu@amlogic.com>");
