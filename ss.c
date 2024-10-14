#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <strings.h>
#include <stdbool.h>

#include <raylib.h>
#include <raymath.h>

#define Font XFont
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Font

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

#include "font.h"
#include "hash.c"

#define DEBUG 0

#define streq(str1, str2) (strcmp(str1, str2) == 0)
#define strcaseeq(str1, str2) (strcasecmp(str1, str2) == 0)
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define panic(...) do { \
	eprintf(__VA_ARGS__); \
	deinit_raylib(); \
	exit(1); \
} while (0)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define UNUSED __attribute__((unused))

#define WHXY_UNPACK \
	float w = whxy.w; \
	float h = whxy.h; \
	float x = whxy.x; \
	float y = whxy.y;

#define WHXY_UNPACK_I32 \
	i32 w = (i32) whxy.w; \
	i32 h = (i32) whxy.h; \
	i32 x = (i32) whxy.x; \
	i32 y = (i32) whxy.y;

#define center_x (GetScreenWidth() / 2)
#define center_y (GetScreenHeight() / 2)

#define BACKGROUND_COLOR ((Color) {10, 10, 10, 255})

#define WINDOW_FLAGS (FLAG_FULLSCREEN_MODE | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST)

#define DARKEN_FACTOR 0.45f

#define SCROLL_SENSITIVITY 350.0f
#define BOOSTED_SCROLL_SENSITIVITY (SCROLL_SENSITIVITY*3)

#define BRUSH_RADIUS_SENSITIVITY 1.0f
#define BOOSTED_BRUSH_RADIUS_SENSITIVITY 3.0f

#define ZOOM_SPEED 1.2f
#define BOOSTED_ZOOM_SPEED 2.2f
#define MIN_ZOOM 0.7f
#define MAX_ZOOM 10.0f
#define STARTING_ZOOM 1.0f

#define PANNING_FACTOR 5.0f
#define PANNING_ZOOM_FACTOR 0.18f

#define SCROLL_SPEED 150.0f
#define SMOOTHING_FACTOR 0.1f

#define RADIUS_ZOOM_OUT_FACTOR 7.6f
#define STARTING_RADIUS 150

#define RESIZE_RING_RADIUS 8.0f
#define RESIZE_RING_THICKNESS 1.3f
#define RESIZE_RING_SEGMENTS 25
#define RESIZE_RING_COLOR ((Color) {0, 170, 47, 255})

#define GLSL_VERSION 300

#define XSCREENSHOTS \
	X(screenshot); \
	X(darker_screenshot);

#define XTEXTURES \
	X(screenshot_texture); \
	X(darker_screenshot_texture);

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef u64 usize;

typedef struct { u8 r, g, b; } RGB;

typedef struct { float w, h, x, y; } whxy_t;

enum {
	SELECTION_POISONED = 0,
	SELECTION_INSIDE,
	SELECTION_UPPER_LEFT,
	SELECTION_UPPER_RIGHT,
	SELECTION_BOTTOM_LEFT,
	SELECTION_BOTTOM_RIGHT
};

enum {
	PASSED,
	NOT_PASSED,
	PASSED_WITHOUT_VALUE_UNEXPECTEDLY,
};

// Stolen from: <https://github.com/NSinecode/Raylib-Drawing-texture-in-circle/blob/master/CircleTexture.frag>
const char* CIRCLE_SHADER =
"#version 330\n"
"in vec2 fragTexCoord;\n"
"in vec4 fragColor;\n"
"uniform sampler2D texture0;\n"
"uniform vec4 colDiffuse;\n"
"uniform vec2 center;\n"
"uniform float radius;\n"
"uniform float smoothness;\n"
"uniform vec2 renderSize;\n"
"out vec4 finalColor;\n"
"void main()\n"
"{\n"
"    vec2 NNfragTexCoord = fragTexCoord * renderSize;\n"
"    float L = length(center - NNfragTexCoord);\n"
"    float edgeThreshold = radius; \n"
"    float alpha = smoothstep(edgeThreshold - smoothness, edgeThreshold, L);\n"
"    if (L <= radius) {\n"
"        finalColor = texture(texture0, fragTexCoord) * fragColor;\n"
"        finalColor.a *= (1.0 - alpha);\n"
"    } else {\n"
"        finalColor = vec4(0.0);\n"
"    }\n"
"}";

#define OUTPUT_FILE_NAME "screenshot"
#define OUTPUT_FILE_EXTENSION ".png"

static size_t output_file_name_len = 0;

#define BRUSH_COLOR RED
#define BRUSH_RADIUS 3.0f

static Color brush_color = BRUSH_COLOR;
static float brush_radius = BRUSH_RADIUS;

static float zoom = STARTING_ZOOM;

static u32 radius = STARTING_RADIUS;

static bool resizing_now, drawing_now = false;
static u8 resizing_what = SELECTION_POISONED;

static bool timer_mode = false;
static time_t timer_start = 0;

static Font font = {0};

static bool pan_mode, alt_mode, selection_mode, resize_mode = false;

#define DOUBLE_UNINITIALIZED 0.0f

static bool color_selector_mode = false;
static float color_selector_mode_ending = DOUBLE_UNINITIALIZED;
static Vector2 color_selector_entered_position = {DOUBLE_UNINITIALIZED, DOUBLE_UNINITIALIZED};

static Vector2 selection_start, selection_end = {DOUBLE_UNINITIALIZED, DOUBLE_UNINITIALIZED};

static Vector2 cur_pos, image_pos, dmouse_pos = {0};

static Display *xdisplay = NULL;
static XWindowAttributes gwa = {0};

static Image screenshot, darker_screenshot = {0};
static Texture2D screenshot_texture, darker_screenshot_texture = {0};

static u8 *original_image_data = NULL;

static RenderTexture2D canvas = {0};

static bool immediate_screenshot_and_exit = false;
#define IMMEDIATE_SCREENSHOT_AND_EXIT_FLAG "screenshot"

static const Color colors[] = {
	GRAY,
	DARKGRAY,
	YELLOW,
	GOLD,
	ORANGE,
	PINK,
	RED,
	MAROON,
	GREEN,
	LIME,
	DARKGREEN,
	SKYBLUE,
	BLUE,
	DARKBLUE,
	PURPLE,
	VIOLET,
	DARKPURPLE,
	BEIGE,
	BROWN,
	DARKBROWN,
	WHITE,
	BLACK,
	MAGENTA,
	RAYWHITE
};

#define COLORS_COUNT (sizeof(colors) / sizeof(Color))

#define COLORS_PADDING 5.0f
#define COLOR_PREVIEW_SIZE ((Vector2) {27.4f, 40.0f})
#define COLOR_SELECTOR_WINDOW_SIZE ((Vector2) {200.0f, 185.5f})
#define COLOR_SELECTOR_WINDOW_CURSOR_PADDING ((Vector2) {50.0f, -150.0f})

// Precomputed relative positions of preview-tiles inside of the
// color selector window when you press B.
static Vector2 COLOR_POSITIONS[COLORS_COUNT] = {
	{5.0, 5.0},
	{37.4, 5.0},
	{69.8, 5.0},
	{102.2, 5.0},
	{134.6, 5.0},
	{167.0, 5.0},
	{5.0, 50.0},
	{37.4, 50.0},
	{69.8, 50.0},
	{102.2, 50.0},
	{134.6, 50.0},
	{167.0, 50.0},
	{5.0, 95.0},
	{37.4, 95.0},
	{69.8, 95.0},
	{102.2, 95.0},
	{134.6, 95.0},
	{167.0, 95.0},
	{5.0, 140.0},
	{37.4, 140.0},
	{69.8, 140.0},
	{102.2, 140.0},
	{134.6, 140.0},
	{167.0, 140.0},
};

// Compile time hash table 'char *color_name -> Color color',
// with precomputed hashes using `gperf`
static const Color color_map[62] = {
	[39]	= LIGHTGRAY,
	[34]	= GRAY,
	[18]	= DARKGRAY,
	[61]	= YELLOW,
	[14]	= GOLD,
	[26]	= ORANGE,
	[4]		= PINK,
	[3]		= RED,
	[6]		= MAROON,
	[25]	= GREEN,
	[29]	= LIME,
	[19]	= DARKGREEN,
	[37]	= SKYBLUE,
	[9]		= BLUE,
	[23]	= DARKBLUE,
	[31]	= PURPLE,
	[36]	= VIOLET,
	[20]	= DARKPURPLE,
	[10]	= BEIGE,
	[35]	= BROWN,
	[24]	= DARKBROWN,
	[15]	= WHITE,
	[40]	= BLACK,
	[30]	= BLANK,
	[27]	= MAGENTA,
	[13]	= RAYWHITE
};

static bool raylib_initialized = false;

INLINE static void init_raylib(void)
{
	const int m = GetCurrentMonitor();
	SetTargetFPS(144);
	SetTraceLogLevel(LOG_NONE);
	if (!DEBUG) SetConfigFlags(WINDOW_FLAGS);
	InitWindow(GetMonitorWidth(m), GetMonitorHeight(m), "ss");
	font = LoadFont_Font();
	SetExitKey(0);
	HideCursor();
	raylib_initialized = true;
}

INLINE static void deinit_raylib(void)
{
	if (raylib_initialized) {
		UnloadTexture(font.texture);
		UnloadRenderTexture(canvas);
		CloseWindow();
	}
}

INLINE static void clear_canvas(void)
{
	BeginTextureMode(canvas);
	ClearBackground(BLANK);
	EndTextureMode();
}

INLINE static void fill_image(Image *image,
											 int w, int h,
											 int fmt,
											 void *data)
{
	image->width = (i32) w;
	image->height = (i32) h;
	image->mipmaps = 1;
	image->format = fmt;
	image->data = data;
}

INLINE static u8 darken_channel(u8 c)
{
	return MIN(0xFF, MAX(0, c*DARKEN_FACTOR));
}

static void capture_screen(Window root, XWindowAttributes gwa)
{
	XImage *ximage = XGetImage(xdisplay,
														 root,
														 0, 0,
														 gwa.width,
														 gwa.height,
														 AllPlanes,
														 ZPixmap);

	if (!ximage) {
		panic("could not capture screen using `XGetImage`\n");
	}

	const u32 w = ximage->width;
	const u32 h = ximage->height;

	u8 *data = (u8 *) malloc(w*h*sizeof(RGB));
	u8 *darker_data = (u8 *) malloc(w*h*sizeof(RGB));

	for (usize y = 0; y < h; y++) {
		for (usize x = 0; x < w; x++) {
			const u32 p = XGetPixel(ximage, x, y);
			const usize idx = (y*w + x)*sizeof(RGB);

			const u8 r = (p & ximage->red_mask)   >> 16;
			const u8 g = (p & ximage->green_mask) >> 8;
			const u8 b = (p & ximage->blue_mask)  >> 0;

			data[idx]     = r;
			data[idx + 1] = g;
			data[idx + 2] = b;

			darker_data[idx]     = darken_channel(r);
			darker_data[idx + 1] = darken_channel(g);
			darker_data[idx + 2] = darken_channel(b);
		}
	}

	XDestroyImage(ximage);

	fill_image(&screenshot,
						 w, h,
						 PIXELFORMAT_UNCOMPRESSED_R8G8B8,
						 data);

	fill_image(&darker_screenshot,
						 w, h,
						 PIXELFORMAT_UNCOMPRESSED_R8G8B8,
						 darker_data);
}

// Stolen from: <https://github.com/NSinecode/Raylib-Drawing-texture-in-circle/blob/master/CircleTextureDrawing.cpp>
static void DrawCollisionTextureCircle(Texture2D texture,
																Vector2 pos,
																Vector2 circle_center,
																float radius,
																Color color)
{
	const Shader shader = LoadShaderFromMemory(0, CIRCLE_SHADER);
	const int radius_loc = GetShaderLocation(shader, "radius");
	SetShaderValue(shader, radius_loc, &radius, SHADER_UNIFORM_FLOAT);

	const float ci_ce[2] = {circle_center.x, circle_center.y};
	const int center_loc = GetShaderLocation(shader, "center");
	SetShaderValue(shader, center_loc, &ci_ce, SHADER_UNIFORM_VEC2);

	const float resolution[2] = {texture.width, texture.height};
	const int resol_loc = GetShaderLocation(shader, "renderSize");
	SetShaderValue(shader, resol_loc, &resolution, SHADER_UNIFORM_VEC2);

	const float smoothness = 10.0f;
	const int smoothnessLoc = GetShaderLocation(shader, "smoothness");
	SetShaderValue(shader, smoothnessLoc, &smoothness, SHADER_UNIFORM_FLOAT);

	BeginShaderMode(shader);

	DrawTextureEx(texture, pos, 0, zoom, color);

	EndShaderMode();
	UnloadShader(shader);
}

INLINE static void stop_selection_mode(void)
{
	memset(&selection_start,
				 DOUBLE_UNINITIALIZED,
				 sizeof(selection_start));

	memset(&selection_end,
				 DOUBLE_UNINITIALIZED,
				 sizeof(selection_end));

	selection_mode = false;
	resize_mode = false;
}

INLINE static void stop_color_selector_mode(void)
{
	memset(&color_selector_entered_position,
				 DOUBLE_UNINITIALIZED,
				 sizeof(color_selector_entered_position));

	color_selector_mode = false;
}

INLINE static void stop_resizing(void)
{
	resizing_now = false;
	resizing_what = SELECTION_POISONED;
}

INLINE static void stop_timer_mode(void)
{
	timer_mode = false;
	timer_start = -1;
}

INLINE static whxy_t get_selection_data(void)
{
	return (whxy_t) {
		.w = fabsf(selection_end.x - selection_start.x),
		.h = fabsf(selection_end.y - selection_start.y),
		.x = fminf(selection_start.x, selection_end.x),
		.y = fminf(selection_start.y, selection_end.y)
	};
}

#define get_file_path(...) get_file_path_(__VA_ARGS__, 0)

// add _<number> at the end if needed to prevent overwriting
char *get_file_path_(char *file_path, u64 rec_count)
{
	if (access(file_path, F_OK) == 0) {
		scratch_buffer_clear();
		char *number_start = file_path + output_file_name_len;
		if (*number_start == '\0') {
			scratch_buffer_printf("%s_%zu.png", OUTPUT_FILE_NAME, 0);
		} else {
			//										skip `_`
			u64 number = strtoull(number_start + 1, NULL, 10);
			scratch_buffer_printf("%s_%zu.png", OUTPUT_FILE_NAME, number + 1);
		}

		return get_file_path_(scratch_buffer_to_string(), rec_count++);
	}

	return file_path;
}

INLINE static u8 *draw_canvas_into_image(u8 *data, int w, int h)
{
	Image image = (Image) {
		.data = data,
		.width = w,
		.height = h,
		.mipmaps = screenshot.mipmaps,
		.format = screenshot.format
	};

	Image canvas_image = LoadImageFromTexture(canvas.texture);

	Rectangle src_rec = {0, 0, canvas_image.width, canvas_image.height};
	Rectangle dst_rec = {0, 0, image.width, image.height};

	ImageDraw(&image, canvas_image, src_rec, dst_rec, WHITE);

	return image.data;
}

INLINE static void save_fullscreen(void)
{
	const char *file_path = get_file_path(OUTPUT_FILE_NAME
																				OUTPUT_FILE_EXTENSION);
	Image image = (Image) {
		.data = original_image_data,
		.width = screenshot.width,
		.height = screenshot.height,
		.mipmaps = screenshot.mipmaps,
		.format = screenshot.format
	};

	image.data = draw_canvas_into_image(image.data, image.width, image.height);

	ExportImage(image, file_path);
}

INLINE static void save_image_data(u8 *data, int w, int h)
{
	const char *file_path = get_file_path(OUTPUT_FILE_NAME
																				OUTPUT_FILE_EXTENSION);

	Image image = (Image) {
		.data = data,
		.width = w,
		.height = h,
		.mipmaps = screenshot.mipmaps,
		.format = screenshot.format
	};

	ExportImage(image, file_path);
}

INLINE static i32 wrap(i32 x, i32 max)
{
	x %= max;
	if (x < 0) x += max;
	return x;
}

INLINE static u8 *crop_image(const u8 *img_data,
														 i32 img_w, i32 img_h,
														 i32 w, i32 h,
														 i32 x, i32 y)
{
	u8 *data = (u8 *) malloc(w*h*sizeof(RGB));
	for (i32 row = 0; row < h; row++) {
		i32 wy = wrap(y + row, img_h);
		for (i32 col = 0; col < w; col++) {
			i32 wx = wrap(x + col, img_w);
			i32 src_offset = (wy*img_w + wx)*sizeof(RGB);
			i32 dst_offset = (row*w + col)*sizeof(RGB);
			memcpy(data + dst_offset, img_data + src_offset, sizeof(RGB));
		}
	}

	return data;
}

INLINE static void get_selection_corners(whxy_t whxy,
																				 Vector2 *upper_left,
																				 Vector2 *upper_right,
																				 Vector2 *bottom_left,
																				 Vector2 *bottom_right)
{
	WHXY_UNPACK

	upper_left->x = x;
	upper_left->y = y;

	upper_right->x = x + w;
	upper_right->y = y;

	bottom_left->x = x;
	bottom_left->y = y + h;

	bottom_right->x = x + w;
	bottom_right->y = y + h;
}

static bool selection_check_collisions(Vector2 mouse_pos)
{
	const whxy_t whxy = get_selection_data();
	WHXY_UNPACK

	const Rectangle rec = (Rectangle) {
		.width = w,
		.height = h,
		.x = x,
		.y = y,
	};

	return CheckCollisionPointRec(mouse_pos, rec);
}

static u8 selection_check_corner_collisions(Vector2 mouse_pos)
{
	Vector2 up_l, up_r, bot_l, bot_r = {0};
	get_selection_corners(get_selection_data(),
												&up_l, &up_r,
												&bot_l, &bot_r);

	if (CheckCollisionPointCircle(mouse_pos, up_l, RESIZE_RING_RADIUS)) {
		return SELECTION_UPPER_LEFT;
	}

	if (CheckCollisionPointCircle(mouse_pos, up_r, RESIZE_RING_RADIUS)) {
		return SELECTION_UPPER_RIGHT;
	}

	if (CheckCollisionPointCircle(mouse_pos, bot_l, RESIZE_RING_RADIUS)) {
		return SELECTION_BOTTOM_LEFT;
	}

	if (CheckCollisionPointCircle(mouse_pos, bot_r, RESIZE_RING_RADIUS)) {
		return SELECTION_BOTTOM_RIGHT;
	}

	return SELECTION_POISONED;
}

INLINE static Vector2 Vector2Value(float value)
{
	return (Vector2) {value, value};
}

INLINE static Vector2 Vector2DivideValue(Vector2 v, float div)
{
	return (Vector2) { v.x / div, v.y / div };
}

static void take_screenshot(void)
{
	if (selection_mode) {
		const whxy_t whxy = get_selection_data();

		WHXY_UNPACK_I32

		x = fabsf(x - image_pos.x) / zoom;
		y = fabsf(y - image_pos.y) / zoom;
		w /= zoom;
		h /= zoom;

		u8 *drawn_data = (u8 *) malloc(screenshot.width*screenshot.height*sizeof(RGB));
		memcpy(drawn_data, original_image_data, screenshot.width*screenshot.height*sizeof(RGB));

		Image image = (Image) {
			.data = drawn_data,
			.width = screenshot.width,
			.height = screenshot.height,
			.mipmaps = screenshot.mipmaps,
			.format = screenshot.format
		};

		// TODO: avoid flipping the image twice, but flip canvas once
		ImageFlipVertical(&image);

		image.data = draw_canvas_into_image(image.data, image.width, image.height);

		ImageFlipVertical(&image);

		u8 *data = crop_image(image.data,
													screenshot.width,
													screenshot.height,
													w, h, x, y);

		stop_selection_mode();
		save_image_data(data, w, h);

		free(data);
		free(drawn_data);
	} else {
		save_fullscreen();
	}

	clear_canvas();
}

static i32 check_color_selector_collisions(Vector2 mouse_pos)
{
	const Vector2 rpos = Vector2Add(color_selector_entered_position,
																	COLOR_SELECTOR_WINDOW_CURSOR_PADDING);

	for (size_t color_idx = 0; color_idx < COLORS_COUNT; color_idx++) {
		const Vector2 tile_pos = Vector2Add(rpos, COLOR_POSITIONS[color_idx]);
		const Rectangle tile_rect = (Rectangle) {
			.x = tile_pos.x, .y = tile_pos.y,
			.width = COLOR_PREVIEW_SIZE.x, .height = COLOR_PREVIEW_SIZE.y
		};

		if (CheckCollisionPointRec(mouse_pos, tile_rect)) {
			return (i32) color_idx;
		}
	}

	return -1;
}

static void handle_input(void)
{
	const float wheel_move = GetMouseWheelMove();
	const Vector2 mouse_pos = GetMousePosition();

	if (!color_selector_mode) {
		cur_pos = mouse_pos;
	}

	alt_mode = IsKeyDown(KEY_LEFT_ALT);

	if (color_selector_mode) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_ARROW);
	} else if (resizing_now) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
	} else if (alt_mode || resize_mode || drawing_now) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
	} else {
		HideCursor();
	}

	if (selection_mode && !resize_mode) {
		selection_end = cur_pos;
	}

	// Wait quarter of a second to not draw accidentally
	if (color_selector_mode_ending != DOUBLE_UNINITIALIZED) {
		if (GetTime() - color_selector_mode_ending > 0.25) {
			color_selector_mode_ending = DOUBLE_UNINITIALIZED;
		} else {
			return;
		}
	}

	if (resizing_now) {
		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			switch (resizing_what) {
			case SELECTION_INSIDE: {
				const Vector2 delta = Vector2Subtract(mouse_pos, dmouse_pos);
				selection_start = Vector2Add(selection_start, delta);
				selection_end = Vector2Add(selection_end, delta);
			} break;

			case SELECTION_UPPER_LEFT: {
				if (mouse_pos.x > 0) {
					selection_start.x = mouse_pos.x;
				}
				selection_start.y = mouse_pos.y;
			} break;

			case SELECTION_UPPER_RIGHT: {
				if (mouse_pos.x > 0) {
					selection_end.x = mouse_pos.x;
				}
				selection_start.y = mouse_pos.y;
			} break;

			case SELECTION_BOTTOM_LEFT: {
				if (mouse_pos.x > 0) {
					selection_start.x = mouse_pos.x;
				}
				selection_end.y = mouse_pos.y;
			} break;

			case SELECTION_BOTTOM_RIGHT: {
				if (mouse_pos.x > 0) {
					selection_end.x = mouse_pos.x;
				}
				selection_end.y = mouse_pos.y;
			} break;

			default: panic("unreachable"); break;
			}
		} else {
			stop_resizing();
		}
	}

	if (!drawing_now && resize_mode && !resizing_now && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
		const u8 corner = selection_check_corner_collisions(mouse_pos);
		if (corner != SELECTION_POISONED) {
			resizing_now = true;
			resizing_what = corner;
		} else if (alt_mode && selection_check_collisions(mouse_pos)) {
			resizing_now = true;
			resizing_what = SELECTION_INSIDE;
		}
	}

	if (!color_selector_mode && !alt_mode && (!resize_mode || (resize_mode && !resizing_now))) {
		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			drawing_now = true;
			BeginTextureMode(canvas);
			{
				const int nsteps = (int) Vector2Distance(dmouse_pos, mouse_pos);
				for (int step = 0; step < nsteps; step++) {
					const Vector2 ipos = Vector2Lerp(dmouse_pos,
																					 mouse_pos,
																					 (float) step / nsteps);

					DrawCircle((int) ipos.x, (int) ipos.y, brush_radius, brush_color);
				}
			}
			EndTextureMode();
		} else if (drawing_now) {
			drawing_now = false;
		}
	}

	if (IsKeyPressed(KEY_ESCAPE)) {
		stop_timer_mode();
		if (color_selector_mode) {
			stop_color_selector_mode();
		} else if (selection_mode) {
			stop_resizing();
			stop_selection_mode();
		} else {
			clear_canvas();
			zoom = STARTING_ZOOM;
			radius = STARTING_RADIUS;
			image_pos = Vector2Zero();
			SetMousePosition(center_x, center_y);
			cur_pos = (Vector2) {center_x, center_y};
		}
	}

	else if (IsKeyPressed(KEY_ENTER)) {
		take_screenshot();
	}

	else if (IsKeyPressed(KEY_C)) {
		clear_canvas();
	}

	else if (IsKeyPressed(KEY_T)) {
		timer_mode = true;
		timer_start = clock();
	}

	else if (IsKeyPressed(KEY_B)) {
		if (!color_selector_mode) {
			color_selector_mode = true;
			color_selector_entered_position = mouse_pos;
		} else {
			stop_color_selector_mode();
		}
	}

	if (color_selector_mode && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		const i32 tile_idx = check_color_selector_collisions(mouse_pos);
		if (tile_idx >= 0) {
			color_selector_mode_ending = GetTime();
			brush_color = colors[tile_idx];
		}
		stop_color_selector_mode();
	}

	if (wheel_move != 0) {
		if (color_selector_mode) {
			const float sens = IsKeyDown(KEY_LEFT_SHIFT) ?
				BOOSTED_BRUSH_RADIUS_SENSITIVITY :
				BRUSH_RADIUS_SENSITIVITY;

			const float new_brush_radius = brush_radius + wheel_move*sens;
			brush_radius = Clamp(new_brush_radius, 1.0f, 50.0f);
		} else if (IsKeyDown(KEY_CAPS_LOCK) || IsKeyDown(KEY_LEFT_CONTROL)) {
			float offset = -SCROLL_SPEED*wheel_move;

			if (offset <= 0) {
				offset*=-RADIUS_ZOOM_OUT_FACTOR;
			}

			const float sine = sin(offset);
			const float sens = IsKeyDown(KEY_LEFT_SHIFT) ?
				BOOSTED_SCROLL_SENSITIVITY :
				SCROLL_SENSITIVITY;

			const float tradius = MIN(MIN(gwa.width, gwa.height), MAX(15.0, radius + sine*sens));
			radius += (tradius - radius)*SMOOTHING_FACTOR;
		} else {
			Vector2 offset = Vector2DivideValue(Vector2Subtract(mouse_pos, image_pos), zoom);

			const float zs = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_ZOOM_SPEED : ZOOM_SPEED;
			zoom += wheel_move*0.1f*zs;
			zoom = Clamp(zoom, MIN_ZOOM, MAX_ZOOM);

			image_pos = Vector2Subtract(mouse_pos,
																	Vector2Multiply(offset, Vector2Value(zoom)));
		}
	}

	if (IsKeyDown(KEY_SPACE)) {
		if (!pan_mode) {
			pan_mode = true;
		}
		Vector2 delta = Vector2Subtract(mouse_pos, dmouse_pos);
		delta = Vector2Scale(delta, PANNING_FACTOR*(zoom*PANNING_ZOOM_FACTOR));
		image_pos = Vector2Add(image_pos, delta);
		cur_pos = mouse_pos;
	} else {
		pan_mode = false;
	}

	if (alt_mode) {
		if (!selection_mode && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
			selection_start = mouse_pos;
			selection_mode = true;
		} else if (selection_mode && !IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
			resize_mode = true;
		}
	} else if (selection_mode && !resize_mode) {
		stop_selection_mode();
	}

	dmouse_pos = cur_pos;
}

static void draw_selection(void)
{
	if (selection_start.x == DOUBLE_UNINITIALIZED) return;
	DrawTextureEx(darker_screenshot_texture,
								image_pos,
								0,
								zoom,
								WHITE);

	const whxy_t whxy = get_selection_data();
	WHXY_UNPACK

	const Rectangle selection = {
		.height = h,
		.width = w,
		.x = x,
		.y = y
	};

	const Rectangle src_rect = {
		.x = (x - image_pos.x) / zoom,
		.y = (y - image_pos.y) / zoom,
		.width = w / zoom,
		.height = h / zoom
	};

	DrawTexturePro(screenshot_texture,
								 src_rect,
								 selection,
								 Vector2Zero(),
								 0,
								 WHITE);

	Vector2 up_l, up_r, bot_l, bot_r = {0};
	get_selection_corners(whxy, &up_l, &up_r, &bot_l, &bot_r);

	DrawRing(up_l,
					 RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS,
					 RESIZE_RING_RADIUS,
					 0.0f, 365.0f,
					 RESIZE_RING_SEGMENTS,
					 RESIZE_RING_COLOR);

	DrawRing(up_r,
					 RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS,
					 RESIZE_RING_RADIUS,
					 0.0f, 365.0f,
					 RESIZE_RING_SEGMENTS,
					 RESIZE_RING_COLOR);

	DrawRing(bot_l,
					 RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS,
					 RESIZE_RING_RADIUS,
					 0.0f, 365.0f,
					 RESIZE_RING_SEGMENTS,
					 RESIZE_RING_COLOR);

	DrawRing(bot_r,
					 RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS,
					 RESIZE_RING_RADIUS,
					 0.0f, 365.0f,
					 RESIZE_RING_SEGMENTS,
					 RESIZE_RING_COLOR);
}

static void draw_canvas(void)
{
	Rectangle src_rect = {
		0, 0,
		(float) canvas.texture.width,
		(float) -canvas.texture.height
	};

	Rectangle dst_rec = {
		0, 0,
		(float) GetScreenWidth(),
		(float) GetScreenHeight()
	};

	Vector2 origin = {0};
	DrawTexturePro(canvas.texture, src_rect, dst_rec, origin, 0.0f, WHITE);
}

static void handle_timer_mode(void)
{
	const time_t now = clock();
	const double elapsed = (double) (now - timer_start) / CLOCKS_PER_SEC;
	if (elapsed >= 1.0) {
		stop_timer_mode();
		take_screenshot();
		return;
	}

	scratch_buffer_clear();
	scratch_buffer_printf("screenshot will be taken "
												"in %.2lf seconds..",
												10.0f-elapsed*10.0f);

	char *text = scratch_buffer_to_string();

	const float spacing = 2.0f;
	const float font_size = 20.0f;

	const Vector2 size = MeasureTextEx(font, text, font_size, spacing);

	const float x = GetScreenWidth()*0.97 - size.x;
	const float y = GetScreenHeight()*0.97 - size.y;

	const float pad = 50.0f;

	DrawRectangle(x - pad/2,
								y - pad/2,
								size.x + pad,
								size.y + pad,
								(Color){0, 0, 0, 150});

	DrawTextEx(font,
						 text,
						 (Vector2) {x, y},
						 font_size,
						 spacing,
						 WHITE);
}

static void handle_color_selector_mode(void)
{
	const Vector2 rpos = Vector2Add(color_selector_entered_position,
																	COLOR_SELECTOR_WINDOW_CURSOR_PADDING);

	DrawRectangleV(rpos, COLOR_SELECTOR_WINDOW_SIZE, WHITE);

	for (size_t color_idx = 0; color_idx < COLORS_COUNT; color_idx++) {
		const Vector2 draw_pos = Vector2Add(rpos, COLOR_POSITIONS[color_idx]);
		DrawRectangleV(draw_pos, COLOR_PREVIEW_SIZE, colors[color_idx]);
		DrawRectangleLinesEx((Rectangle) {
			.x = draw_pos.x, .y = draw_pos.y,
			.width = COLOR_PREVIEW_SIZE.x, .height = COLOR_PREVIEW_SIZE.y
		}, 1.0f, BLACK);
	}

	DrawCircleV(GetMousePosition(),
							brush_radius,
							brush_color);
}

INLINE static void preserve_original_image_data(void)
{
	original_image_data = (u8 *) malloc(sizeof(RGB)*
																			screenshot.width*
																			screenshot.height);

	memcpy(original_image_data,
				 screenshot.data,
				 sizeof(RGB)*screenshot.width*screenshot.height);
}

static size_t argc;
#define FLAG_CAP 256
static char **argv, flag_value[FLAG_CAP + 1];

INLINE static int check_flag(char *flag, bool expect_value)
{
	scratch_buffer_clear();
	scratch_buffer_append(flag);
	char *lowerflag = scratch_buffer_copy();

	for (size_t i = 1; i < argc; ++i) {
		if (strcaseeq(lowerflag, argv[i])) {
			if (!expect_value) return PASSED;

			if (i + 1 >= argc)
				return PASSED_WITHOUT_VALUE_UNEXPECTEDLY;

			memcpy(flag_value, argv[i + 1], FLAG_CAP);
			return PASSED;
		} else {
			char *pos = strchr(argv[i], '=');
			if (pos == NULL) continue;

			const size_t idx = pos - argv[i];
			scratch_buffer_clear();
			memcpy(scratch_buffer.str, argv[i], idx);
			scratch_buffer.len = idx;
			scratch_buffer_append_char('\0');

			const char *flag_str = TextToLower(scratch_buffer.str);

			if (!strcaseeq(lowerflag, flag_str)) continue;
			if (!expect_value) return PASSED;

			scratch_buffer_clear();
			scratch_buffer_append(argv[i] + idx + 1);
			scratch_buffer_append_char('\0');

			// characters after `=`
			char *flag_value_str = scratch_buffer_to_string();

			if (flag_value_str == NULL || *flag_value_str == '\0')
				return PASSED_WITHOUT_VALUE_UNEXPECTEDLY;

			memcpy(flag_value, flag_value_str, FLAG_CAP);
			return PASSED;
		}
	}

	return NOT_PASSED;
}

INLINE static Color color_try_from_str(const char *str)
{
	const size_t len = strlen(str);
	const char *upper_str = TextToUpper(str);
	if (is_color(upper_str, len) == NULL) {
		return BLANK;
	} else {
		return color_map[hash(upper_str, len)];
	}
}

INLINE static float parse_float_or_panic(const char *str)
{
	char *end;
	const float ret = strtof(str, &end);
	if (end == flag_value || *end != '\0') {
		panic("failed to parse `%s` to float\n", str);
	} else if (errno == ERANGE) {
		if (ret == HUGE_VAL) {
			panic("overflew when tried to parse `%s` to float\n", str);
		} else {
			panic("underflew when tried to parse `%s` to float\n", str);
		}
	}
	return ret;
}

INLINE static void provided_flag_example(const char *flag)
{
	printf("try to provide a flag following way:\n");
	printf("%s=<value> or %s <value>\n", flag, flag);
}

static void handle_flags(void)
{
	int code;

	code = check_flag(IMMEDIATE_SCREENSHOT_AND_EXIT_FLAG, false);
	if (code == PASSED) {
		immediate_screenshot_and_exit = true;
	}

	code = check_flag("brush_color", true);
	if (code == PASSED_WITHOUT_VALUE_UNEXPECTEDLY) {
		panic("expected `brush_color` flag to have a value\n");
	} else if (code == PASSED) {
		const Color new_brush_color = color_try_from_str(flag_value);
		if (memcmp(&new_brush_color, &BLANK, sizeof(Color)) == 0) {
			eprintf("unexpected color: `%s`\n", flag_value);
			provided_flag_example("brush_color");
			exit(1);
		}

		brush_color = new_brush_color;
	}

	code = check_flag("brush_radius", true);
	if (code == PASSED_WITHOUT_VALUE_UNEXPECTEDLY) {
		panic("expected `brush_radius` flag to have a value\n");
	} else if (code == PASSED) {
		brush_radius = parse_float_or_panic(flag_value);
	}
}

i32 main(int argc_, char **argv_)
{
	argc = (size_t) argc_;
	argv = argv_;

	if (argc > 1) {
		memory_init(1);
		handle_flags();
	}

	xdisplay = XOpenDisplay(NULL);
	if (!xdisplay) {
		panic("could not to open X display");
	}

	const Window root = DefaultRootWindow(xdisplay);
	XGetWindowAttributes(xdisplay, root, &gwa);

	cur_pos = (Vector2) {center_x, center_y};
	output_file_name_len = strlen(OUTPUT_FILE_NAME);

	capture_screen(root, gwa);
	preserve_original_image_data();

	if (immediate_screenshot_and_exit) {
		save_fullscreen();
		exit(0);
	}

	init_raylib();

	canvas = LoadRenderTexture(gwa.width, gwa.height);
	clear_canvas();

	screenshot_texture = LoadTextureFromImage(screenshot);
	darker_screenshot_texture = LoadTextureFromImage(darker_screenshot);

	while (!WindowShouldClose()) {
		handle_input();
		BeginDrawing();
		{
			ClearBackground(BACKGROUND_COLOR);
			if (selection_mode) {
				draw_selection();
			} else if (alt_mode || drawing_now || color_selector_mode) {
				DrawTextureEx(screenshot_texture,
											image_pos,
											0,
											zoom,
											WHITE);

			} else {
				DrawTextureEx(darker_screenshot_texture,
											image_pos,
											0,
											zoom,
											WHITE);

				DrawCollisionTextureCircle(screenshot_texture,
																	 image_pos,
																	 cur_pos,
																	 radius,
																	 WHITE);
			}

			draw_canvas();

			if (timer_mode) {
				handle_timer_mode();
			}

			if (color_selector_mode) {
				handle_color_selector_mode();
			}
		}
		EndDrawing();
	}

#define X UnloadImage
	XSCREENSHOTS
#undef X

#define X UnloadTexture
	XTEXTURES
#undef X

	deinit_raylib();
	XCloseDisplay(xdisplay);

	if (argc > 1) {
		memory_release();
	}

	return 0;
}
