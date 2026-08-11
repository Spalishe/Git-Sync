#include "render.hpp"
#include "assets.hpp"
#include <cairo/cairo.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>

uint32_t scr_height = 1080;
std::unordered_map<ImageID, LoadedAsset> loaded_assets;

/* draw text into cairo_surface */
text_size draw_test_text(cairo_t* cr, const char* text, uint32_t x, uint32_t y, float r, float g, float b, float a)
{
	/* draw white text near top center */
	cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL,
						   CAIRO_FONT_WEIGHT_BOLD);
	double font_size = get_font_size();
	cairo_set_font_size(cr, font_size);

	cairo_font_extents_t font_ext;
	cairo_font_extents(cr, &font_ext);

	cairo_text_extents_t ext;
	cairo_text_extents(cr, text, &ext);

	double font_height = font_ext.ascent + font_ext.descent;
	double center_y	   = (double)y + (font_height / 2.0) - font_ext.descent;

	/* black shadow */
	cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
	cairo_move_to(cr, (double)x + 2.0, center_y + 2.0);
	cairo_show_text(cr, text);

	/* white text */
	cairo_set_source_rgba(cr, r, g, b, a);
	cairo_move_to(cr, (double)x, center_y);
	cairo_show_text(cr, text);

	text_size t = { ext.x_advance, font_height };

	return t;
}

void handle_messages(cairo_t* cr)
{
	if(pending_messages.empty()) return;
	for(auto& item : pending_messages)
	{
		item->call_add_message_int(cr);
	}
	pending_messages.clear();
}

void draw_image_rotated(cairo_t* cr, cairo_surface_t* surface, uint32_t center_x, uint32_t center_y, double angle_rad)
{
	int img_w = cairo_image_surface_get_width(surface);
	int img_h = cairo_image_surface_get_height(surface);

	cairo_save(cr);

	cairo_translate(cr, center_x, center_y);

	cairo_rotate(cr, angle_rad);

	cairo_set_source_surface(cr, surface, -img_w / 2.0, -img_h / 2.0);

	cairo_paint(cr);

	cairo_restore(cr);
}

double FrameTime(void)
{
	static struct timespec last_ts = { 0, 0 };
	static double dt			   = 0.016666;

	struct timespec current_ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &current_ts);

	if(last_ts.tv_sec != 0)
	{
		double seconds	   = (double)(current_ts.tv_sec - last_ts.tv_sec);
		double nanoseconds = (double)(current_ts.tv_nsec - last_ts.tv_nsec);
		dt				   = seconds + nanoseconds * 1e-9;
	}

	last_ts = current_ts;
	return dt;
}

void render_draw(cairo_t* cr)
{
	uint32_t x = 10;
	uint32_t y = 10;
	auto ct	   = std::chrono::high_resolution_clock::now();

	double frm = FrameTime();
	for(int i = 0; i < messages.size();)
	{
		auto& message = messages[i];
		auto time	  = message.time;
		uint32_t newy = (i + 1) * message.h;
		if(message.y != newy)
		{
			// stupid clangd shows that lerp is not exists in std bruuu
			message.y = std::round(std::lerp(newy, message.y, 0.9f));
		}

		uint32_t my	   = y + message.y;
		uint32_t space = message.space;

		cairo_set_source_rgba(cr, 1, 1, 1, message.alpha);

		if(message.icon != IMG_NONE)
		{
			if(!loaded_assets[message.icon].valid)
			{
				loaded_assets[message.icon].asset = load_embedded_image(message.icon);
				loaded_assets[message.icon].valid = true;
			}
			if(message.rotating)
			{
				std::chrono::duration<double> duration_seconds = ct - time;
				double seconds								   = duration_seconds.count();
				double deg									   = std::fmod((seconds * 120.0), 360.0) * (M_PI / 180.0);

				draw_image_rotated(cr, loaded_assets[message.icon].asset, x + space + 8, my, deg);
			}
			else
			{
				cairo_save(cr);
				cairo_surface_t* img = loaded_assets[message.icon].asset;
				if(img && cairo_surface_status(img) == CAIRO_STATUS_SUCCESS)
				{
					double img_x = x + space;
					double img_y = my - 8;

					int img_w = cairo_image_surface_get_width(img);
					int img_h = cairo_image_surface_get_height(img);

					cairo_set_source_surface(cr, img, img_x, img_y);

					cairo_rectangle(cr, img_x, img_y, img_w, img_h);
					cairo_clip(cr);

					cairo_paint_with_alpha(cr, message.alpha);
				}
				cairo_restore(cr);
			}
		}

		for(auto& part : message.parts)
		{
			draw_test_text(cr, part.text.c_str(), x + space + 20 + part.x, my, part.color.r / 255.0f, part.color.g / 255.0f, part.color.b / 255.0f, message.alpha);
		}

		bool inc = true;

		if(time < ct)
		{
			if(message.alpha > 0)
			{
				message.alpha -= frm * 4;
			}
			else
			{
				messages.erase(messages.begin() + i);
				inc = false;
			}
		}
		else
		{
			if(message.alpha < 1)
			{
				message.alpha += frm * 4;
			}
		}
		if(inc) i++;
	}
}
