#pragma once

#include "r_textures.h"
#include "vk_core.h"
#include "vk_image.h"
#include "vk_const.h"

#include "unordered_roadmap.h"

typedef struct vk_texture_s
{
	urmom_header_t hdr_;

	int width, height;
	uint32_t flags;
	int total_size;

	struct {
		r_vk_image_t image;
		VkDescriptorSet descriptor_unorm;
	} vk;

	int refcount;

	// TODO "cache" eviction
	// int used_maps_ago;
} vk_texture_t;

#define TEX_NAME(tex) ((tex)->hdr_.key)

typedef enum {
	kColorspaceNative,
	kColorspaceLinear,
	kColorspaceGamma,
} colorspace_hint_e;

qboolean R_VkTexturesInit( void );
void R_VkTexturesShutdown( void );

qboolean R_VkTexturesSkyboxUpload( const char *name, rgbdata_t *const sides[6], colorspace_hint_e colorspace_hint, qboolean placeholder);

qboolean R_VkTextureUpload(int index, vk_texture_t *tex, rgbdata_t *const *const layers, int num_layers, colorspace_hint_e colorspace_hint);
void R_VkTextureDestroy(int index, vk_texture_t *tex);

// FIXME s/R_/R_Vk/
void R_TextureAcquire( unsigned int texnum );
void R_TextureRelease( unsigned int texnum );

#define R_TextureUploadFromBufferNew(name, pic, flags) R_TextureUploadFromBuffer(name, pic, flags, false)

int R_TextureUploadFromFileEx( const char *filename, colorspace_hint_e colorspace, qboolean force_reload );
// Used by materials to piggy-back onto texture name-to-index hash table
int R_TextureCreateDummy_FIXME( const char *name );

VkDescriptorImageInfo R_VkTextureGetSkyboxDescriptorImageInfo( void );
const VkDescriptorImageInfo* R_VkTexturesGetAllDescriptorsArray( void );
VkDescriptorSet R_VkTextureGetDescriptorUnorm( uint index );
