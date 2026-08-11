#pragma once
#include <cairo/cairo.h>
#include <cstring>

#include "assets/brick_add.h"
#include "assets/brick_delete.h"
#include "assets/brick_edit.h"
#include "assets/comment.h"
#include "assets/information.h"
#include "assets/panel_open.h"
#include "assets/script_add.h"
#include "assets/script_delete.h"
#include "assets/script_edit.h"
#include <unordered_map>

enum ImageID : int
{
	IMG_NONE		  = 0,
	IMG_BRICK_ADD	  = 1,
	IMG_BRICK_DELETE  = 2,
	IMG_BRICK_EDIT	  = 3,
	IMG_SCRIPT_ADD	  = 4,
	IMG_SCRIPT_DELETE = 5,
	IMG_SCRIPT_EDIT	  = 6,
	IMG_INFORMATION	  = 7,
	IMG_COMMENT		  = 8,
};

struct EmbeddedAsset
{
	const unsigned char* data;
	unsigned int length;
};

struct LoadedAsset
{
	bool valid = false;
	cairo_surface_t* asset;
};

inline std::unordered_map<ImageID, EmbeddedAsset> ASSET_REGISTRY = {
	{ IMG_BRICK_ADD,	 { brick_add_png, brick_add_png_len }		  },
	{ IMG_BRICK_DELETE,	{ brick_delete_png, brick_delete_png_len }   },
	{ IMG_BRICK_EDIT,	  { brick_edit_png, brick_edit_png_len }		 },
	{ IMG_SCRIPT_ADD,	  { script_add_png, script_add_png_len }		 },
	{ IMG_SCRIPT_DELETE, { script_delete_png, script_delete_png_len } },
	{ IMG_SCRIPT_EDIT,   { script_edit_png, script_edit_png_len }		},
	{ IMG_INFORMATION,   { information_png, information_png_len }		},
	{ IMG_COMMENT,	   { comment_png, comment_png_len }				}
};

extern std::unordered_map<ImageID, LoadedAsset> loaded_assets;

inline cairo_status_t read_png_from_buffer(void* closure, unsigned char* data, unsigned int length)
{
	unsigned char** buffer_ptr = (unsigned char**)closure;
	memcpy(data, *buffer_ptr, length);
	*buffer_ptr += length;
	return CAIRO_STATUS_SUCCESS;
}

inline cairo_surface_t* load_embedded_image(ImageID id)
{
	unsigned char* current_position = (unsigned char*)ASSET_REGISTRY[id].data;

	return cairo_image_surface_create_from_png_stream(
		read_png_from_buffer,
		&current_position);
}
