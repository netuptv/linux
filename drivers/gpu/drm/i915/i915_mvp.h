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
 */

#ifndef _I915_MVP_H_
#define _I915_MVP_H_

struct i915_mvp_buf_record {
	u64 start_time;
	u64 end_time;
};

struct i915_mvp_req_record {
	u64 cpu_time;
	u64 gpu_time;
	pid_t pid;
	u32 perf_tag;
	struct i915_mvp_buf_record *cpu_addr;
	u64 gpu_addr;
};

bool i915_mvp_is_enabled(void);

int i915_mvp_enable(struct drm_device *dev, bool enable);

int i915_mvp_start_task(struct drm_device *dev, struct intel_engine_cs *ring,
			struct drm_i915_gem_request *req);

int i915_mvp_end_task(struct drm_device *dev, struct intel_engine_cs *ring,
		      struct drm_i915_gem_request *req);

void i915_mvp_init_req(struct drm_i915_gem_request *req, u32 perf_tag);

void i915_mvp_read_req(struct drm_i915_gem_request *req);

#endif
