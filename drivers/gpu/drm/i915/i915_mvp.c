/*
 * Copyright  2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *	Jabin Wu <jabin.wu@intel.com>
 */

#include <linux/kernel.h>
#include <drm/drm.h>
#include "i915_drv.h"
#include "i915_trace.h"

struct i915_mvp_info {
	struct drm_i915_gem_object *buf_obj;
	int buf_offset;
	u64 ggtt_offset;
	char __iomem *cpu_addr;
	bool enabled;
};
struct i915_mvp_info i915_mvp;

#define CIRCLE_BUF_SIZE (256 * PAGE_SIZE)
#define MVP_GEN6_MI_PIPE_CONTROL_TIMESTAMP  (0x03 << 14)
#define MVP_GEN6_MI_FLUSH_DW_TIMESTAMP      (0x03 << 14)
#define MVP_END_TIME_OFFSET 8

static int i915_mvp_init(struct drm_device *dev)
{
	int ret;
	struct drm_i915_private *dev_priv = dev->dev_private;

	i915_mvp.buf_obj = i915_gem_alloc_object(dev, CIRCLE_BUF_SIZE);
	if (i915_mvp.buf_obj == NULL) {
		DRM_ERROR("Failed to allocate mvp bo\n");
		return -ENOMEM;
	}

	ret = i915_gem_obj_ggtt_pin(i915_mvp.buf_obj, PAGE_SIZE, PIN_MAPPABLE);
	if (ret) {
		DRM_ERROR("Failed to pin mvp bo\n");
		goto err_unref;
	}

	i915_mvp.ggtt_offset = i915_gem_obj_ggtt_offset(i915_mvp.buf_obj);
	i915_mvp.cpu_addr =
	    ioremap_wc(dev_priv->gtt.mappable_base +
		       i915_gem_obj_ggtt_offset(i915_mvp.buf_obj),
		       CIRCLE_BUF_SIZE);
	if (i915_mvp.cpu_addr == NULL) {
		DRM_ERROR("Failed to pin mvp bo\n");
		ret = -ENOSPC;
		goto err_unpin;
	}

	i915_mvp.enabled = true;
	i915_mvp.buf_offset = 0;

	return 0;

 err_unpin:
	i915_gem_object_ggtt_unpin(i915_mvp.buf_obj);
 err_unref:
	drm_gem_object_unreference(&i915_mvp.buf_obj->base);
	return ret;
}

static int i915_mvp_exit(void)
{
	i915_gem_free_object(&i915_mvp.buf_obj->base);
	iounmap(i915_mvp.cpu_addr);
	i915_mvp.buf_obj = NULL;
	i915_mvp.cpu_addr = NULL;
	i915_mvp.ggtt_offset = 0;
	i915_mvp.enabled = false;
	i915_mvp.buf_offset = 0;
	return 0;
}

static void i915_mvp_get_buf_space(struct drm_i915_gem_request *req)
{
	if (i915_mvp.buf_offset + sizeof(struct i915_mvp_buf_record) >= CIRCLE_BUF_SIZE) {
		i915_mvp.buf_offset = 0;
	}
	req->mvp_req.cpu_addr =
	    (struct i915_mvp_buf_record *)(i915_mvp.cpu_addr + i915_mvp.buf_offset);
	req->mvp_req.gpu_addr = i915_mvp.ggtt_offset + i915_mvp.buf_offset;
	i915_mvp.buf_offset += sizeof(struct i915_mvp_buf_record);
}

static void i915_mvp_mi_pipe_control(struct intel_engine_cs *ring,
				     struct drm_i915_gem_request *req,
				     u_int32_t addr)
{
	int ret;
	struct intel_ringbuffer *ringbuf = req->ringbuf;

	ret = intel_logical_ring_begin(req, 6);
	if (ret)
		return;
	intel_logical_ring_emit(ringbuf, GFX_OP_PIPE_CONTROL(6));
	intel_logical_ring_emit(ringbuf,
				MVP_GEN6_MI_PIPE_CONTROL_TIMESTAMP |
				PIPE_CONTROL_GLOBAL_GTT_IVB);
	intel_logical_ring_emit(ringbuf, addr);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_advance(ringbuf);
}

static void i915_mvp_mi_flush_dw(struct intel_engine_cs *ring,
				 struct drm_i915_gem_request *req,
				 u_int32_t addr)
{
	int ret;
	struct intel_ringbuffer *ringbuf = req->ringbuf;

	ret = intel_logical_ring_begin(req, 6);
	if (ret)
		return;
	intel_logical_ring_emit(ringbuf,
				(MI_FLUSH_DW + 2) | MVP_GEN6_MI_FLUSH_DW_TIMESTAMP);
	intel_logical_ring_emit(ringbuf,
				((addr & 0xFFFFFFF8) | MI_FLUSH_DW_USE_GTT));
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_emit(ringbuf, 0);
	intel_logical_ring_advance(ringbuf);

}

void i915_mvp_init_req(struct drm_i915_gem_request *req, u32 perf_tag)
{
	if (!i915_mvp.enabled)
		return;
	req->mvp_req.perf_tag = perf_tag;
	req->mvp_req.pid = current->pid;
	req->mvp_req.cpu_addr = NULL;
}

void i915_mvp_read_req(struct drm_i915_gem_request *req)
{
	if (i915_mvp.enabled && req->mvp_req.cpu_addr != NULL)
		trace_i915_mvp_read_req(req);
}

int i915_mvp_start_task(struct drm_device *dev,
			struct intel_engine_cs *ring,
			struct drm_i915_gem_request *req)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u_int64_t gtime, ctime;
	int this_cpu;

	if (!i915_mvp.enabled)
		return 0;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	this_cpu = raw_smp_processor_id();
	ctime = cpu_clock(this_cpu);
	gtime = I915_READ(RING_TIMESTAMP(ring->mmio_base));

	i915_mvp_get_buf_space(req);
	req->mvp_req.cpu_time = ctime;
	req->mvp_req.gpu_time = gtime;

	if (ring->id == RCS)
		i915_mvp_mi_pipe_control(ring, req, req->mvp_req.gpu_addr);
	else
		i915_mvp_mi_flush_dw(ring, req, req->mvp_req.gpu_addr);
	return 0;
}

int i915_mvp_end_task(struct drm_device *dev,
		      struct intel_engine_cs *ring,
		      struct drm_i915_gem_request *req)
{
	if (!i915_mvp.enabled)
		return 0;

	WARN_ON(!mutex_is_locked(&dev->struct_mutex));

	if (ring->id == RCS)
		i915_mvp_mi_pipe_control(ring, req, req->mvp_req.gpu_addr +
					MVP_END_TIME_OFFSET);
	else
		i915_mvp_mi_flush_dw(ring, req, req->mvp_req.gpu_addr +
					MVP_END_TIME_OFFSET);

	return 0;
}

bool i915_mvp_is_enabled(void)
{
	return i915_mvp.enabled;
}

int i915_mvp_enable(struct drm_device *dev, bool enable)
{
	if (!i915_mvp.enabled && enable)
		return i915_mvp_init(dev);
	if (i915_mvp.enabled && !enable)
		return i915_mvp_exit();
	return 0;
}
