#include "vk_combuf.h"
#include "vk_commandpool.h"
#include "vk_buffer.h"
#include "vk_logs.h"
#include "vk_image.h"

#include "profiler.h"

#define LOG_MODULE combuf

#define MAX_COMMANDBUFFERS 6
#define MAX_QUERY_COUNT 128

#define MAX_BUFFER_BARRIERS 16
#define MAX_IMAGE_BARRIERS 16

#define BEGIN_INDEX_TAG 0x10000000

typedef struct {
	vk_combuf_t public;
	int used;
	struct {
		int timestamps_offset;
		int scopes[MAX_GPU_SCOPES];
		int scopes_count;
	} profiler;

	uint32_t tag;
} vk_combuf_impl_t;

static struct {
	vk_command_pool_t pool;

	vk_combuf_impl_t combufs[MAX_COMMANDBUFFERS];
	struct {
		VkQueryPool pool;
		uint64_t values[MAX_QUERY_COUNT * MAX_COMMANDBUFFERS];
	} timestamp;

	vk_combuf_scope_t scopes[MAX_GPU_SCOPES];
	int scopes_count;

	int entire_combuf_scope_id;

	uint32_t tag;
} g_combuf;

qboolean R_VkCombuf_Init( void ) {
	g_combuf.pool = R_VkCommandPoolCreate(MAX_COMMANDBUFFERS);
	if (!g_combuf.pool.pool)
		return false;

	const VkQueryPoolCreateInfo qpci = {
		.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
		.pNext = NULL,
		.queryType = VK_QUERY_TYPE_TIMESTAMP,
		.queryCount = COUNTOF(g_combuf.timestamp.values),
		.flags = 0,
	};

	XVK_CHECK(vkCreateQueryPool(vk_core.device, &qpci, NULL, &g_combuf.timestamp.pool));

	for (int i = 0; i < MAX_COMMANDBUFFERS; ++i) {
		vk_combuf_impl_t *const cb = g_combuf.combufs + i;

		cb->public.cmdbuf = g_combuf.pool.buffers[i];
		SET_DEBUG_NAMEF(cb->public.cmdbuf, VK_OBJECT_TYPE_COMMAND_BUFFER, "cmdbuf[%d]", i);

		cb->profiler.timestamps_offset = i * MAX_QUERY_COUNT;
	}

	g_combuf.entire_combuf_scope_id = R_VkGpuScope_Register("GPU");
	g_combuf.tag = 1; // Do not start with special value of zero

	return true;
}

void R_VkCombuf_Destroy( void ) {
	vkDestroyQueryPool(vk_core.device, g_combuf.timestamp.pool, NULL);
	R_VkCommandPoolDestroy(&g_combuf.pool);

	for (int i = 0; i < g_combuf.scopes_count; ++i) {
		Mem_Free((char*)g_combuf.scopes[i].name);
	}
}

vk_combuf_t* R_VkCombufOpen( void ) {
	for (int i = 0; i < MAX_COMMANDBUFFERS; ++i) {
		vk_combuf_impl_t *const cb = g_combuf.combufs + i;
		if (!cb->used) {
			cb->used = 1;
			return &cb->public;
		}
	}

	return NULL;
}

void R_VkCombufClose( vk_combuf_t* pub ) {
	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)pub;
	cb->used = 0;

	// TODO synchronize?
	// For now, external synchronization expected
}

void R_VkCombufBegin( vk_combuf_t* pub ) {
	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)pub;

	g_combuf.tag++;
	// Skip zero as special initial value for objects meaning "not yet used in combuf"
	if (g_combuf.tag == 0)
		g_combuf.tag = 1;

	cb->tag = g_combuf.tag;

	cb->profiler.scopes_count = 0;

	const VkCommandBufferBeginInfo beginfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	XVK_CHECK(vkBeginCommandBuffer(cb->public.cmdbuf, &beginfo));

	vkCmdResetQueryPool(cb->public.cmdbuf, g_combuf.timestamp.pool, cb->profiler.timestamps_offset, MAX_QUERY_COUNT);
	R_VkCombufScopeBegin(pub, g_combuf.entire_combuf_scope_id);
}

void R_VkCombufEnd( vk_combuf_t* pub ) {
	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)pub;
	R_VkCombufScopeEnd(pub, 0 | BEGIN_INDEX_TAG, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	XVK_CHECK(vkEndCommandBuffer(cb->public.cmdbuf));
}

static const char* myStrdup(const char *src) {
	const int len = strlen(src);
	char *ret = Mem_Malloc(vk_core.pool, len + 1);
	memcpy(ret, src, len);
	ret[len] = '\0';
	return ret;
}

#define ACCESS_WRITE_BITS (0 \
	| VK_ACCESS_2_SHADER_WRITE_BIT \
	| VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT \
	| VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT \
	| VK_ACCESS_2_TRANSFER_WRITE_BIT \
	| VK_ACCESS_2_HOST_WRITE_BIT \
	| VK_ACCESS_2_MEMORY_WRITE_BIT \
	| VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT \
	| VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR \
	)

#define ACCESS_READ_BITS (0 \
	| VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT \
	| VK_ACCESS_2_INDEX_READ_BIT \
	| VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT \
	| VK_ACCESS_2_UNIFORM_READ_BIT \
	| VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT \
	| VK_ACCESS_2_SHADER_READ_BIT \
	| VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT \
	| VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT \
	| VK_ACCESS_2_TRANSFER_READ_BIT \
	| VK_ACCESS_2_HOST_READ_BIT \
	| VK_ACCESS_2_MEMORY_READ_BIT \
	| VK_ACCESS_2_SHADER_SAMPLED_READ_BIT \
	| VK_ACCESS_2_SHADER_STORAGE_READ_BIT \
	| VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR \
	)

#define ACCESS_KNOWN_BITS (ACCESS_WRITE_BITS | ACCESS_READ_BITS)

#define PRINT_FLAG(mask, flag) \
	if ((flag) & (mask)) DEBUG("%s%s", prefix, #flag)
static void printAccessMask(const char *prefix, VkAccessFlags2 access) {
	PRINT_FLAG(access, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_INDEX_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_UNIFORM_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_TRANSFER_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_HOST_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_HOST_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_MEMORY_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_MEMORY_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	PRINT_FLAG(access, VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_CONDITIONAL_RENDERING_READ_BIT_EXT);
#ifdef VK_EXT_device_generated_commands
	PRINT_FLAG(access, VK_ACCESS_2_COMMAND_PREPROCESS_READ_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_COMMAND_PREPROCESS_WRITE_BIT_EXT);
#endif
	PRINT_FLAG(access, VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_SHADING_RATE_IMAGE_READ_BIT_NV);
	PRINT_FLAG(access, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_NV);
	PRINT_FLAG(access, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_NV);
	PRINT_FLAG(access, VK_ACCESS_2_FRAGMENT_DENSITY_MAP_READ_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_COLOR_ATTACHMENT_READ_NONCOHERENT_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_INVOCATION_MASK_READ_BIT_HUAWEI);
	PRINT_FLAG(access, VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR);
	PRINT_FLAG(access, VK_ACCESS_2_MICROMAP_READ_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT);
	PRINT_FLAG(access, VK_ACCESS_2_OPTICAL_FLOW_READ_BIT_NV);
	PRINT_FLAG(access, VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV);
}

static void printStageMask(const char *prefix, VkPipelineStageFlags2 stages) {
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_HOST_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_COPY_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_RESOLVE_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_BLIT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_CLEAR_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_TRANSFORM_FEEDBACK_BIT_EXT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_CONDITIONAL_RENDERING_BIT_EXT);
#ifdef VK_EXT_device_generated_commands
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_COMMAND_PREPROCESS_BIT_EXT);
#endif
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_SHADING_RATE_IMAGE_BIT_NV);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_SUBPASS_SHADER_BIT_HUAWEI);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_INVOCATION_MASK_BIT_HUAWEI);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_CLUSTER_CULLING_SHADER_BIT_HUAWEI);
	PRINT_FLAG(stages, VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV);
}

static qboolean makeBufferBarrier(VkBufferMemoryBarrier2* out_bmb, const r_vkcombuf_barrier_buffer_t *const bufbar, VkPipelineStageFlags2 dst_stage, uint32_t cb_tag) {
	vk_buffer_t *const buf = bufbar->buffer;
	const qboolean is_write = (bufbar->access & ACCESS_WRITE_BITS) != 0;
	const qboolean is_read = (bufbar->access & ACCESS_READ_BITS) != 0;
	ASSERT((bufbar->access & ~(ACCESS_KNOWN_BITS)) == 0);

	if (buf->sync.combuf_tag != cb_tag) {
		// This buffer hasn't been yet used in this command buffer, no need to issue a barrier
		buf->sync.combuf_tag = cb_tag;
		buf->sync.write = is_write
			? (r_vksync_scope_t){.access = bufbar->access & ACCESS_WRITE_BITS, .stage = dst_stage}
			: (r_vksync_scope_t){.access = 0, .stage = 0 };
		buf->sync.read = is_read
			? (r_vksync_scope_t){.access = bufbar->access & ACCESS_READ_BITS, .stage = dst_stage}
			: (r_vksync_scope_t){.access = 0, .stage = 0 };
		return false;
	}

	*out_bmb = (VkBufferMemoryBarrier2) {
		.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		.pNext = NULL,
		.buffer = buf->buffer,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
		.dstStageMask = dst_stage,
		.dstAccessMask = bufbar->access,
	};

	// TODO: support read-and-write scenarios
	ASSERT(is_read ^ is_write);
	if (is_write) {
		// Write is synchronized with previous reads and writes
		out_bmb->srcStageMask = buf->sync.write.stage | buf->sync.read.stage;
		out_bmb->srcAccessMask = buf->sync.write.access | buf->sync.read.access;

		// Store where write happened
		buf->sync.write.access = bufbar->access;
		buf->sync.write.stage = dst_stage;

		// If there were no previous reads or writes, there no reason to synchronize with anything
		if (out_bmb->srcStageMask == 0)
			return false;

		// Reset read state
		// TOOD is_read? for read-and-write
		buf->sync.read.access = 0;
		buf->sync.read.stage = 0;
	}

	if (is_read) {
		// Read is synchronized with previous writes only
		out_bmb->srcStageMask = buf->sync.write.stage;
		out_bmb->srcAccessMask = buf->sync.write.access;

		// Check whether this is a new barrier
		if ((buf->sync.read.access & bufbar->access) != bufbar->access
			&& (buf->sync.read.stage & dst_stage) != dst_stage) {
			// Remember this read happened
			buf->sync.read.access |= bufbar->access;
			buf->sync.read.stage |= dst_stage;
		} else {
			// Already synchronized, no need to do anything
			return false;
		}

		// Also skip issuing a barrier, if there were no previous writes -- nothing to sync with
		// Note that this needs to happen late, as all reads must still be recorded in sync.read fields
		if (buf->sync.write.stage == 0)
			return false;
	}

	if (LOG_VERBOSE) {
		DEBUG("  srcAccessMask = %llx", (unsigned long long)out_bmb->srcAccessMask);
		printAccessMask("   ", out_bmb->srcAccessMask);
		DEBUG("  dstAccessMask = %llx", (unsigned long long)out_bmb->dstAccessMask);
		printAccessMask("   ", out_bmb->dstAccessMask);
		DEBUG("  srcStageMask = %llx", (unsigned long long)out_bmb->srcStageMask);
		printStageMask("   ", out_bmb->srcStageMask);
		DEBUG("  dstStageMask = %llx", (unsigned long long)out_bmb->dstStageMask);
		printStageMask("   ", out_bmb->dstStageMask);
	}

	return true;
}

static qboolean makeImageBarrier(VkImageMemoryBarrier2* out_imb, const r_vkcombuf_barrier_image_t *const imgbar, VkPipelineStageFlags2 dst_stage) {
	r_vk_image_t *const img = imgbar->image;
	const qboolean is_write = (imgbar->access & ACCESS_WRITE_BITS) != 0;
	const qboolean is_read = (imgbar->access & ACCESS_READ_BITS) != 0;
	const VkImageLayout old_layout = (!is_read) ? VK_IMAGE_LAYOUT_UNDEFINED : img->sync.layout;
	const qboolean is_layout_transfer = imgbar->layout != old_layout;
	ASSERT((imgbar->access & ~(ACCESS_KNOWN_BITS)) == 0);

	*out_imb = (VkImageMemoryBarrier2) {
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		.pNext = NULL,
		.srcStageMask = img->sync.write.stage,
		.srcAccessMask = img->sync.write.access,
		.dstStageMask = dst_stage,
		.dstAccessMask = imgbar->access,
		.oldLayout = old_layout,
		.newLayout = imgbar->layout,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = img->image,
		.subresourceRange = (VkImageSubresourceRange) {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};

	// TODO: support read-and-write scenarios
	//ASSERT(is_read ^ is_write);

	if (is_write || is_layout_transfer) {
		out_imb->srcStageMask |= img->sync.read.stage;
		out_imb->srcAccessMask |= img->sync.read.access;

		img->sync.write.access = imgbar->access;
		img->sync.write.stage = dst_stage;

		img->sync.read.access = 0;
		img->sync.read.stage = 0;
	}

	if (is_read) {
		const qboolean same_access = (img->sync.read.access & imgbar->access) != imgbar->access;
		const qboolean same_stage = (img->sync.read.stage & dst_stage) != dst_stage;

		if (same_access && same_stage && !is_layout_transfer)
			return false;

		img->sync.read.access |= imgbar->access;
		img->sync.read.stage |= dst_stage;
	}

	if (!is_layout_transfer && out_imb->srcAccessMask == 0 && out_imb->srcStageMask == 0) {
		return false;
	}

	if (LOG_VERBOSE) {
		DEBUG("  srcAccessMask = %llx", (unsigned long long)out_imb->srcAccessMask);
		printAccessMask("   ", out_imb->srcAccessMask);
		DEBUG("  dstAccessMask = %llx", (unsigned long long)out_imb->dstAccessMask);
		printAccessMask("   ", out_imb->dstAccessMask);
		DEBUG("  srcStageMask = %llx", (unsigned long long)out_imb->srcStageMask);
		printStageMask("   ", out_imb->srcStageMask);
		DEBUG("  dstStageMask = %llx", (unsigned long long)out_imb->dstStageMask);
		printStageMask("   ", out_imb->dstStageMask);
		DEBUG("  oldLayout = %s (%llx)", R_VkImageLayoutName(out_imb->oldLayout), (unsigned long long)out_imb->oldLayout);
		DEBUG("  newLayout = %s (%llx)", R_VkImageLayoutName(out_imb->newLayout), (unsigned long long)out_imb->newLayout);
	}

	// Store new layout
	img->sync.layout = imgbar->layout;

	return true;
}

void R_VkCombufIssueBarrier(vk_combuf_t* combuf, r_vkcombuf_barrier_t bar) {
	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)combuf;

	BOUNDED_ARRAY(VkBufferMemoryBarrier2, buffer_barriers, MAX_BUFFER_BARRIERS);
	for (int i = 0; i < bar.buffers.count; ++i) {
		const r_vkcombuf_barrier_buffer_t *const bufbar = bar.buffers.items + i;
		if (LOG_VERBOSE) {
			DEBUG(" buf[%d]: buf=%llx barrier:", i, (unsigned long long)bufbar->buffer->buffer);
		}

		VkBufferMemoryBarrier2 bmb;
		if (!makeBufferBarrier(&bmb, bufbar, bar.stage, cb->tag)) {
			continue;
		}

		BOUNDED_ARRAY_APPEND_ITEM(buffer_barriers, bmb);
	}

	BOUNDED_ARRAY(VkImageMemoryBarrier2, image_barriers, MAX_IMAGE_BARRIERS);
	for (int i = 0; i < bar.images.count; ++i) {
		const r_vkcombuf_barrier_image_t *const imgbar = bar.images.items + i;
		if (LOG_VERBOSE) {
			DEBUG(" img[%d]: img=%llx (%s) barrier:", i, (unsigned long long)imgbar->image->image, imgbar->image->name);
		}

		VkImageMemoryBarrier2 imb;
		if (!makeImageBarrier(&imb, imgbar, bar.stage)) {
			continue;
		}

		BOUNDED_ARRAY_APPEND_ITEM(image_barriers, imb);
	}

	if (buffer_barriers.count == 0 && image_barriers.count == 0)
		return;

	vkCmdPipelineBarrier2(combuf->cmdbuf, &(VkDependencyInfo) {
		.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		.pNext = NULL,
		.dependencyFlags = 0,
		.bufferMemoryBarrierCount = buffer_barriers.count,
		.pBufferMemoryBarriers = buffer_barriers.items,
		.imageMemoryBarrierCount = image_barriers.count,
		.pImageMemoryBarriers = image_barriers.items,
	});
}


int R_VkGpuScope_Register(const char *name) {
	// Find existing scope with the same name
	for (int i = 0; i < g_combuf.scopes_count; ++i) {
		if (Q_strcmp(name, g_combuf.scopes[i].name) == 0)
			return i;
	}

	if (g_combuf.scopes_count == MAX_GPU_SCOPES) {
		gEngine.Con_Printf(S_ERROR "Cannot register GPU profiler scope \"%s\": max number of scope %d reached\n", name, MAX_GPU_SCOPES);
		return -1;
	}

	g_combuf.scopes[g_combuf.scopes_count].name = myStrdup(name);

	return g_combuf.scopes_count++;
}

int R_VkCombufScopeBegin(vk_combuf_t* cumbuf, int scope_id) {
	if (scope_id < 0)
		return -1;

	ASSERT(scope_id < g_combuf.scopes_count);

	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)cumbuf;
	if (cb->profiler.scopes_count == MAX_GPU_SCOPES)
		return -1;

	cb->profiler.scopes[cb->profiler.scopes_count] = scope_id;

	vkCmdWriteTimestamp(cb->public.cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_combuf.timestamp.pool, cb->profiler.timestamps_offset + cb->profiler.scopes_count * 2);

	return (cb->profiler.scopes_count++) | BEGIN_INDEX_TAG;
}

void R_VkCombufScopeEnd(vk_combuf_t* combuf, int begin_index, VkPipelineStageFlagBits pipeline_stage) {
	if (begin_index < 0)
		return;

	ASSERT(begin_index & BEGIN_INDEX_TAG);
	begin_index ^= BEGIN_INDEX_TAG;

	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)combuf;

	vkCmdWriteTimestamp(cb->public.cmdbuf, pipeline_stage, g_combuf.timestamp.pool, cb->profiler.timestamps_offset + begin_index * 2 + 1);
}

static uint64_t getGpuTimestampOffsetNs( uint64_t latest_gpu_timestamp, uint64_t latest_cpu_timestamp_ns ) {
	// FIXME this is an incorrect check, we need to carry per-device extensions availability somehow. vk_core-vs-device refactoring pending
	if (!vkGetCalibratedTimestampsEXT) {
		// Estimate based on supposed submission time, assuming that we submit, and it starts computing right after cmdbuffer closure
		// which may not be true. But it's all we got
		// TODO alternative approach: estimate based on end timestamp
		const uint64_t gpu_begin_ns = (double) latest_gpu_timestamp * vk_core.physical_device.properties.limits.timestampPeriod;
		return latest_cpu_timestamp_ns - gpu_begin_ns;
	}

	const VkCalibratedTimestampInfoEXT cti[2] = {
		{
			.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
			.pNext = NULL,
			.timeDomain = VK_TIME_DOMAIN_DEVICE_EXT,
		},
		{
			.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
			.pNext = NULL,
#if defined(_WIN32)
			.timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT,
#else
			.timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT,
#endif
		},
	};

	uint64_t timestamps[2] = {0};
	uint64_t max_deviation[2] = {0};
	vkGetCalibratedTimestampsEXT(vk_core.device, 2, cti, timestamps, max_deviation);

	const uint64_t cpu = aprof_time_platform_to_ns(timestamps[1]);
	const uint64_t gpu = (double)timestamps[0] * vk_core.physical_device.properties.limits.timestampPeriod;
	return cpu - gpu;
}

vk_combuf_scopes_t R_VkCombufScopesGet( vk_combuf_t *pub ) {
	APROF_SCOPE_DECLARE_BEGIN(function, __FUNCTION__);
	vk_combuf_impl_t *const cb = (vk_combuf_impl_t*)pub;

	uint64_t *const timestamps = g_combuf.timestamp.values + cb->profiler.timestamps_offset;
	const int timestamps_count = cb->profiler.scopes_count * 2;

	if (timestamps_count) {
		vkGetQueryPoolResults(vk_core.device, g_combuf.timestamp.pool, cb->profiler.timestamps_offset, timestamps_count, timestamps_count * sizeof(uint64_t), timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

		const uint64_t timestamp_offset_ns = getGpuTimestampOffsetNs( timestamps[1], aprof_time_now_ns() );
		const double timestamp_period = vk_core.physical_device.properties.limits.timestampPeriod;

		for (int i = 0; i < timestamps_count; ++i) {
			const uint64_t gpu_ns = timestamps[i] * timestamp_period;
			timestamps[i] = timestamp_offset_ns + gpu_ns;
		}
	}

	APROF_SCOPE_END(function);

	return (vk_combuf_scopes_t){
		.timestamps = g_combuf.timestamp.values + cb->profiler.timestamps_offset,
		.scopes = g_combuf.scopes,
		.entries = cb->profiler.scopes,
		.entries_count = cb->profiler.scopes_count,
	};
}
