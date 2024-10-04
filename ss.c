#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>

#include <raylib.h>
#include <raymath.h>

#define Font XFont
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Font

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

#define DEBUG 0

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define panic(...) do { \
	eprintf(__VA_ARGS__); \
	deinit_raylib(); \
	exit(1); \
} while (0)

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

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define BACKGROUND_COLOR ((Color) {10, 10, 10, 255})

#define WINDOW_FLAGS (FLAG_FULLSCREEN_MODE | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST)

#define DARKEN_FACTOR 0.45f

#define SCROLL_SENSITIVITY 350.0f
#define BOOSTED_SCROLL_SENSITIVITY (SCROLL_SENSITIVITY*3)

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

#define BRUSH_RADIUS 3.0f
#define BRUSH_COLOR RED

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

static float zoom = STARTING_ZOOM;

static u32 radius = STARTING_RADIUS;

static bool pan_mode, alt_mode, selection_mode, resize_mode;
static bool resizing_now, drawing_now = false;
static u8 resizing_what = SELECTION_POISONED;

#define SELECTION_UNINITIALIZED 0.0f
static Vector2 selection_start, selection_end = {SELECTION_UNINITIALIZED, SELECTION_UNINITIALIZED};

static Vector2 cur_pos, image_pos, dmouse_pos = {0};

static Display *xdisplay = NULL;
static XWindowAttributes gwa = {0};

static Image screenshot, darker_screenshot = {0};
static Texture2D screenshot_texture, darker_screenshot_texture = {0};

static uint8_t *original_image_data = NULL;

static RenderTexture2D canvas = {0};

static bool immediate_screenshot_and_exit = false;
#define IMMEDIATE_SCREENSHOT_AND_EXIT_FLAG "screenshot"

INLINE static void init_raylib(void)
{
	const int m = GetCurrentMonitor();
	SetTargetFPS(144);
	SetTraceLogLevel(LOG_NONE);
	if (!DEBUG) SetConfigFlags(WINDOW_FLAGS);
	InitWindow(GetMonitorWidth(m), GetMonitorHeight(m), "ss");
	SetExitKey(0);
	HideCursor();
}

INLINE static void deinit_raylib(void)
{
	UnloadRenderTexture(canvas);
	CloseWindow();
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
				 SELECTION_UNINITIALIZED,
				 sizeof(selection_start));

	memset(&selection_end,
				 SELECTION_UNINITIALIZED,
				 sizeof(selection_end));

	selection_mode = false;
	resize_mode = false;
}

INLINE static void stop_resizing(void)
{
	resizing_now = false;
	resizing_what = SELECTION_POISONED;
}

INLINE static float clamp(float v, float min, float max)
{
	float ret = v < min ? min : v;
	if (ret > max) ret = max;
	return ret;
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

INLINE static uint8_t *draw_canvas_into_image(uint8_t *data, int w, int h)
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

INLINE static void save_image_data(uint8_t *data, int w, int h)
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

INLINE static bool selection_check_collisions(Vector2 mouse_pos)
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

static void handle_input(void)
{
	const float wheel_move = GetMouseWheelMove();
	const Vector2 mouse_pos = GetMousePosition();

	cur_pos = mouse_pos;
	alt_mode = IsKeyDown(KEY_LEFT_ALT);

	if (resizing_now) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
	} else if (alt_mode || resize_mode) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
	} else {
		HideCursor();
	}

	if (selection_mode && !resize_mode) {
		selection_end = cur_pos;
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

	if (!alt_mode && (!resize_mode || (resize_mode && !resizing_now))) {
		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			drawing_now = true;
			BeginTextureMode(canvas);
			{
				const int nsteps = (int) Vector2Distance(dmouse_pos, mouse_pos);
				for (int step = 0; step < nsteps; step++) {
					const Vector2 ipos = Vector2Lerp(dmouse_pos,
																					 mouse_pos,
																					 (float) step / nsteps);
	
					DrawCircle((int) ipos.x, (int) ipos.y, BRUSH_RADIUS, BRUSH_COLOR);
				}
			}
			EndTextureMode();
		} else if (drawing_now) {
			drawing_now = false;
		}
	}

	if (IsKeyPressed(KEY_ESCAPE)) {
		if (selection_mode) {
			stop_resizing();
			stop_selection_mode();
		} else {
			zoom = STARTING_ZOOM;
			radius = STARTING_RADIUS;
			image_pos = Vector2Zero();
			cur_pos = (Vector2) {center_x, center_y};
			SetMousePosition(center_x, center_y);
		}
	}

	else if (IsKeyPressed(KEY_ENTER)) {
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
		} else {
			save_fullscreen();
		}
	}

	else if (IsKeyPressed(KEY_C)) {
		clear_canvas();
	}

	if (wheel_move != 0) {
		if(IsKeyDown(KEY_CAPS_LOCK) || IsKeyDown(KEY_LEFT_CONTROL)) {
			float offset = -SCROLL_SPEED*wheel_move;

			if (offset <= 0) {
				offset*=-RADIUS_ZOOM_OUT_FACTOR;
			}

			const float sine = sin(offset);
			const float sens = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_SCROLL_SENSITIVITY : SCROLL_SENSITIVITY;
			const float tradius = MIN(MIN(gwa.width, gwa.height), MAX(15.0, radius + sine*sens));
			radius += (tradius - radius)*SMOOTHING_FACTOR;
		} else {
			Vector2 offset = Vector2DivideValue(Vector2Subtract(mouse_pos, image_pos), zoom);

			const float zs = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_ZOOM_SPEED : ZOOM_SPEED;
			zoom += wheel_move*0.1f*zs;
			zoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);

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
	if (selection_start.x == SELECTION_UNINITIALIZED) return;
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

INLINE static void preserve_original_image_data(void)
{
	original_image_data = (uint8_t *) malloc(sizeof(RGB)*
																					 screenshot.width*
																					 screenshot.height);

	memcpy(original_image_data,
				 screenshot.data,
				 sizeof(RGB)*screenshot.width*screenshot.height);
}

i32 main(int argc, char *argv[])
{
	if (argc > 1) {
		if (strstr(argv[1], IMMEDIATE_SCREENSHOT_AND_EXIT_FLAG) != NULL) {
			immediate_screenshot_and_exit = true;
		}
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
			} else if (alt_mode) {
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

	return 0;
}
