/*
 * Copyright (c) 2015 Intel Corporation
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
 */
#include "i915_drv.h"
#include "intel_mocs.h"

static const struct drm_i915_mocs_entry gen_9_mocs_table[] = {
	{0x00000009, 0x0010}, /* ED_UC LLC/eLLC EDSCC:0 L3SCC:0 L3_UC */
	{0x00000038, 0x0030}, /* ED_PTE LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x0000003b, 0x0030}, /* ED_WB LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000037, 0x0030}, /* ED_WB LLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000039, 0x0030}, /* ED_UC LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000037, 0x0010}, /* ED_WB LLC LRU_L EDSCC:0 L3SCC:0 L3_UC */
	{0x00000039, 0x0010}, /* ED_UC LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_UC */
	{0x00000017, 0x0010}, /* ED_WB LLC LRU_S EDSCC:0 L3SCC:0 L3_UC */
	{0x0000003b, 0x0010}, /* ED_WB LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_UC */
	{0x00000033, 0x0030}, /* ED_WB eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000033, 0x0010}, /* ED_WB eLLC LRU_L EDSCC:0 L3SCC:0 L3_UC */
	{0x00000017, 0x0030}, /* ED_WB LLC LRU_S EDSCC:0 L3SCC:0 L3_WB */
	{0x00000019, 0x0010}, /* ED_UC LLC/eLLC LRU_S EDSCC:0 L3SCC:0 L3_UC */
};

static const struct drm_i915_mocs_entry gen_9_GT3e_mocs_table[] = {
	{0x00000009, 0x0010}, /* ED_UC LLC/eLLC EDSCC:0 L3SCC:0 L3_UC */
	{0x00000038, 0x0030}, /* ED_PTE LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x0000003b, 0x0030}, /* ED_WB LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000033, 0x0030},
	{0x00000033, 0x0010},
	{0x00000039, 0x0010},
	{0x00000013, 0x0010},
	{0x0000003b, 0x0010},
	{0x00000039, 0x0030},
	{0x00000037, 0x0030},
	{0x00000037, 0x0010},
	{0x0000001b, 0x0030},
	{0x00000003, 0x0010},
	{0x0000001b, 0x0010},
};

static const struct drm_i915_mocs_entry gen_9_GT4e_mocs_table[] = {
	{0x00000009, 0x0010}, /* ED_UC LLC/eLLC EDSCC:0 L3SCC:0 L3_UC */
	{0x00000038, 0x0030}, /* ED_PTE LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x0000003b, 0x0030}, /* ED_WB LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000033, 0x0030},
	{0x0000003b, 0x0010},
	{0x00000039, 0x0010},
	{0x00000033, 0x0010},
	{0x0000001b, 0x0010},
	{0x00000039, 0x0030},
	{0x00000037, 0x0030},
	{0x00000037, 0x0010},
	{0x0000001b, 0x0030},
	{0x00000003, 0x0010},
	{0x00000013, 0x0010},
};

static const struct drm_i915_mocs_entry broxton_mocs_table[] = {
	{0x00000009, 0x0010}, /* ED_UC LLC/eLLC EDSCC:0 L3SCC:0 L3_UC */
	{0x00000038, 0x0030}, /* ED_PTE LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x0000003b, 0x0030}, /* ED_WB LLC/eLLC LRU_L EDSCC:0 L3SCC:0 L3_WB */
	{0x00000005, 0x0010}, /* ED_UC LLC EDSCC:0 L3SCC:0 L3_UC */
	{0x00000005, 0x0030}, /* ED_UC LLC EDSCC:0 L3SCC:0 L3_WB */
};

/**
 * get_mocs_settings
 *
 * This function will return the values of the MOCS table that needs to
 * be programmed for the platform. It will return the values that need
 * to be programmed and if they need to be programmed.
 *
 * If the return values is false then the registers do not need programming.
 */
bool get_mocs_settings(struct drm_device *dev,
			struct drm_i915_mocs_table *table) {
	bool	result = false;
	struct	drm_i915_private *dev_priv = dev->dev_private;

	if (INTEL_INFO(dev)->gen == 9) {
		if (IS_BROXTON(dev)) {
			table->size = ARRAY_SIZE(broxton_mocs_table);
			table->table = broxton_mocs_table;
			result = true;
		} else if (IS_SKL_GT3(dev) && dev_priv->ellc_size) {
			/* GT3e */
			table->size = ARRAY_SIZE(gen_9_GT3e_mocs_table);
			table->table = gen_9_GT3e_mocs_table;
			result = true;
		} else if (IS_SKL_GT4(dev) && dev_priv->ellc_size) {
			table->size = ARRAY_SIZE(gen_9_GT4e_mocs_table);
			table->table = gen_9_GT4e_mocs_table;
			result = true;
		} else {
			/* fall back all other not Linux xcode POR cases
			 * to use GT2 mocs table */
			table->size = ARRAY_SIZE(gen_9_mocs_table);
			table->table = gen_9_mocs_table;
			result = true;
		}
	}
	return result;

}
