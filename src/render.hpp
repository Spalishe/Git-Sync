#include <cairo/cairo.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C"
{
#endif
	void handle_messages(cairo_t* cr);
	void render_draw(cairo_t* cr);

	extern uint32_t scr_height;
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "assets.hpp"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
struct Color
{
	Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b)
	{
	}
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

template <typename T>
struct pending_message_t
{
	uint32_t space;
	double time_add;
	ImageID icon;
	bool rotating;
	T tuple;
};

struct BaseMessage
{
	uint32_t space;
	uint32_t time_add;
	ImageID icon;
	bool rotating;

	BaseMessage(uint32_t s, uint32_t t, ImageID i, bool r)
		: space(s), time_add(t), icon(i), rotating(r) {}

	virtual void call_add_message_int(cairo_t* cr) = 0;
	virtual ~BaseMessage()						   = default;
};

struct parts_t
{
	std::string text;
	Color color;
	double w;
	double h;
	double x;
};

struct message_t
{
	uint32_t id;
	// I hate chrono for this piece of shit.
	std::chrono::time_point<std::chrono::high_resolution_clock, std::chrono::duration<double>> time; // CurTime() + time; Expiration
	ImageID icon;
	bool rotating;
	std::vector<parts_t> parts;
	uint32_t space;
	double h;
	double y;
	float alpha;
};

struct text_size
{
	double width;
	double height;
};
inline float get_font_size()
{
	return 8 * (scr_height / 480.0f);
}
inline text_size get_text_size(cairo_t* cr, const std::string& msg)
{
	cairo_select_font_face(cr, "Roboto", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

	cairo_set_font_size(cr, get_font_size());

	cairo_font_extents_t font_extents;
	cairo_font_extents(cr, &font_extents);
	double stable_height = font_extents.ascent + font_extents.descent;

	cairo_text_extents_t extents;
	cairo_text_extents(cr, msg.c_str(), &extents);
	double stable_width = extents.x_advance;

	text_size ts = { .width = stable_width, .height = stable_height };
	return ts;
}

static uint32_t id = 0;
inline std::vector<message_t> messages;
inline std::mutex mtx;
template <typename T>
void add_message_int(cairo_t* cr, uint32_t space, double time_add, ImageID icon, bool rotating, T tuple_args)
{
	id++;
	double x  = 0;
	double mh = 0;
	std::vector<parts_t> parts;
	Color color = Color(255, 255, 255);

	std::apply([&](auto&&... item)
	{
		([&]()
		{
			using T1 = std::decay_t<decltype(item)>;

			if constexpr(std::is_same_v<T1, Color>)
			{
				color = item;
			}
			else if constexpr(std::is_same_v<T1, std::string>)
			{
				text_size ts = get_text_size(cr, item);

				parts.push_back({ .text	 = item,
								  .color = color,
								  .w	 = ts.width,
								  .h	 = ts.height,
								  .x	 = x });

				x += ts.width;
				if(mh < ts.height)
					mh = ts.height;
			}
		}(), ...);
	}, tuple_args);

	auto now = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double> add(time_add);
	auto new_time = now + add;

	std::lock_guard<std::mutex>
		lock(mtx);
	messages.push_back({ .id	   = id,
						 .time	   = new_time,
						 .icon	   = icon,
						 .rotating = rotating,
						 .parts	   = parts,
						 .space	   = space,
						 .h		   = mh,
						 .y		   = ((uint32_t)messages.size() + 1) * mh,
						 .alpha	   = 0 });
}

template <typename TupleType>
struct PendingMessageImpl : public BaseMessage
{
	TupleType tuple_data;

	PendingMessageImpl(uint32_t s, uint32_t t, ImageID i, bool r, TupleType&& tuple)
		: BaseMessage(s, t, i, r), tuple_data(std::move(tuple)) {}

	void call_add_message_int(cairo_t* cr) override
	{
		add_message_int(cr, space, time_add, icon, rotating, tuple_data);
	}
};

inline std::vector<std::unique_ptr<BaseMessage>> pending_messages;
template <typename... Args>
void add_message(uint32_t space, double time_add, ImageID icon, bool rotating, Args&&... args)
{
	auto tuple		= std::make_tuple(std::forward<Args>(args)...);
	using TupleType = decltype(tuple);

	std::lock_guard<std::mutex> lock(mtx);
	pending_messages.push_back(
		std::make_unique<PendingMessageImpl<TupleType>>(space, time_add, icon, rotating, std::move(tuple)));
}

#endif
