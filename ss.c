#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <raylib.h>

#define Font XFont
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Font

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef u64 usize;

typedef struct { u8 r, g, b; } RGB;

typedef struct { float w, h, x, y; } whxy_t;

#define WHXY_UNPACK \
	float w = whxy.w; \
	float h = whxy.h; \
	float x = whxy.x; \
	float y = whxy.y;

#define WHXY_UNPACK_USIZE \
	usize w = (usize) whxy.w; \
	usize h = (usize) whxy.h; \
	usize x = (usize) whxy.x; \
	usize y = (usize) whxy.y;

#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define panic(...) do { \
	eprintf(__VA_ARGS__); \
	deinit_raylib(); \
	exit(1); \
} while (0)

#define DEBUG 0

#define center_x (GetScreenWidth() / 2)
#define center_y (GetScreenHeight() / 2)

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define INLINE static inline

#define CIRCLE_TEXTURE_FILE_PATH "CircleTexture.frag"

#define BACKGROUND_COLOR ((Color) {10, 10, 10, 255})

#define WINDOW_FLAGS (FLAG_FULLSCREEN_MODE | FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST)

#define DARKEN_FACTOR 0.45f

#define SCROLL_SENSITIVITY 350.0f
#define BOOSTED_SCROLL_SENSITIVITY (SCROLL_SENSITIVITY*3)

#define ZOOM_SPEED 1.2f
#define BOOSTED_ZOOM_SPEED 2.2f
#define MIN_ZOOM 0.7f
#define MAX_ZOOM 10.0f

#define PANNING_FACTOR 5.0f
#define PANNING_ZOOM_FACTOR 0.2f

#define SCROLL_SPEED 150.0f
#define SMOOTHING_FACTOR 0.1f

#define RADIUS_ZOOM_OUT_FACTOR 7.6f
#define STARTING_RADIUS 150
#define STARTING_ZOOM 1.0f

#define GLSL_VERSION 300

#define XSCREENSHOTS \
	X(screenshot); \
	X(darker_screenshot);

#define XTEXTURES \
	X(screenshot_texture); \
	X(darker_screenshot_texture);

static float zoom = STARTING_ZOOM;

static u32 radius = STARTING_RADIUS;

static bool pan_mode, alt_mode, selection_mode, preserve_selection_mode = false;

#define SELECTION_UNINITIALIZED 0.0f
static Vector2 selection_start, selection_end = {SELECTION_UNINITIALIZED, SELECTION_UNINITIALIZED};

static Vector2 cur_pos, image_pos, prev_cur_pos = {0};

static bool raylib_initialized = false;

static Display *xdisplay = NULL;
static XWindowAttributes gwa = {0};

static Image screenshot, darker_screenshot = {0};
static Texture2D screenshot_texture, darker_screenshot_texture = {0};

static uint8_t *original_image_data = NULL;

INLINE void init_raylib(i32 w, i32 h)
{
	SetTargetFPS(144);
	SetTraceLogLevel(LOG_NONE);
	if (!DEBUG) SetConfigFlags(WINDOW_FLAGS);
	InitWindow(w, h, "ss");
	SetExitKey(0);
	HideCursor();
	raylib_initialized = true;
}

INLINE void deinit_raylib(void)
{
	if (raylib_initialized) {
		CloseWindow();
	}
}

INLINE void fill_image(Image *image,
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

void capture_screen(Window root, XWindowAttributes gwa)
{
	XImage *ximage = XGetImage(xdisplay, root, 0, 0, gwa.width, gwa.height, AllPlanes, ZPixmap);
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

			darker_data[idx]     = MIN(0xFF, MAX(0, (i32) (r*DARKEN_FACTOR)));
			darker_data[idx + 1] = MIN(0xFF, MAX(0, (i32) (g*DARKEN_FACTOR)));
			darker_data[idx + 2] = MIN(0xFF, MAX(0, (i32) (b*DARKEN_FACTOR)));
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
void DrawCollisionTextureCircle(Texture2D texture,
																Vector2 pos,
																Vector2 circle_center,
																float radius,
																Color color)
{
	Shader shader = LoadShader(0, TextFormat(CIRCLE_TEXTURE_FILE_PATH, GLSL_VERSION));
	int radius_loc = GetShaderLocation(shader, "radius");
	SetShaderValue(shader, radius_loc, &radius, SHADER_UNIFORM_FLOAT);

	float ci_ce[2] = {circle_center.x, circle_center.y};
	int center_loc = GetShaderLocation(shader, "center");
	SetShaderValue(shader, center_loc, &ci_ce, SHADER_UNIFORM_VEC2);

	float resolution[2] = {texture.width, texture.height};
	int resol_loc = GetShaderLocation(shader, "renderSize");
	SetShaderValue(shader, resol_loc, &resolution, SHADER_UNIFORM_VEC2);

	float smoothness = 10.0f;
	int smoothnessLoc = GetShaderLocation(shader, "smoothness");
	SetShaderValue(shader, smoothnessLoc, &smoothness, SHADER_UNIFORM_FLOAT);

	BeginShaderMode(shader);

	DrawTextureEx(texture, pos, 0, zoom, color);

	EndShaderMode();
	UnloadShader(shader);
}

INLINE void stop_selection_mode(void)
{
	memset(&selection_start,
				 SELECTION_UNINITIALIZED,
				 sizeof(selection_start));

	memset(&selection_end,
				 SELECTION_UNINITIALIZED,
				 sizeof(selection_end));

	selection_mode = false;
	preserve_selection_mode = false;
}

INLINE float clamp(float v, float min, float max)
{
	float ret = v < min ? min : v;
	if (ret > max) ret = max;
	return ret;
}

INLINE whxy_t get_selection_data(void)
{
	return (whxy_t) {
		.w = fabsf(selection_end.x - selection_start.x),
		.h = fabsf(selection_end.y - selection_start.y),
		.x = fminf(selection_start.x, selection_end.x),
		.y = fminf(selection_start.y, selection_end.y)
	};
}

INLINE void save_fullscreen(char *file_path)
{
	stbi_write_png(file_path,
								 screenshot.width,
								 screenshot.height,
								 sizeof(RGB),
								 original_image_data,
								 sizeof(RGB)*screenshot.width);
}

INLINE void save_image_data(char *file_path,
													  uint8_t *data,
													  int w, int h)
{
	stbi_write_png(file_path,
								 w, h,
								 sizeof(RGB),
								 data,
								 sizeof(RGB)*w);
}

void handle_input(void)
{
	const float mouse_move = GetMouseWheelMove();
	const Vector2 mouse_pos = GetMousePosition();

	cur_pos = mouse_pos;
	alt_mode = IsKeyDown(KEY_LEFT_ALT);

	if (alt_mode || preserve_selection_mode) {
		ShowCursor();
		SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
	} else {
		HideCursor();
	}

	if (IsKeyPressed(KEY_ESCAPE)) {
		if (selection_mode) {
			stop_selection_mode();
		} else {
			zoom = STARTING_ZOOM;
			radius = STARTING_RADIUS;
			image_pos = (Vector2) {0};
			cur_pos = (Vector2) {center_x, center_y};
		}
	}

	else if (IsKeyPressed(KEY_ENTER)) {
		if (selection_mode) {
			const whxy_t whxy = get_selection_data();

			WHXY_UNPACK_USIZE

			u8 *data = (u8 *) malloc(w*h*sizeof(RGB));

			for (usize row = 0; row < h; row++) {
				usize src_offset = ((y + row)*screenshot.width + x)*sizeof(RGB);
				usize dst_offset = row*w*sizeof(RGB);
				memcpy(data + dst_offset, original_image_data + src_offset, w*sizeof(RGB));
			}

			stop_selection_mode();

			save_image_data("screenshot.png", data, w, h);
		} else {
			save_fullscreen("screenshot.png");
		}
	}

	if (selection_mode && !preserve_selection_mode) {
		selection_end = cur_pos;
	}

	if (mouse_move != 0) {
		if (IsKeyDown(KEY_CAPS_LOCK) || IsKeyDown(KEY_LEFT_CONTROL)) {
			float offset = -SCROLL_SPEED*mouse_move;

			if (offset <= 0) {
				offset*=-RADIUS_ZOOM_OUT_FACTOR;
			}

			const float sine = sin(offset);
			const float sens = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_SCROLL_SENSITIVITY : SCROLL_SENSITIVITY;
			const float tradius = MIN(MIN(gwa.width, gwa.height), MAX(15.0, radius + sine*sens));
			radius += (tradius - radius)*SMOOTHING_FACTOR;
		} else {
			Vector2 offset = {
				(mouse_pos.x - image_pos.x) / zoom,
				(mouse_pos.y - image_pos.y) / zoom
			};

			const float zs = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_ZOOM_SPEED : ZOOM_SPEED;
			zoom += mouse_move*0.1f*zs;
			zoom = clamp(zoom, MIN_ZOOM, MAX_ZOOM);

			image_pos.x = mouse_pos.x - offset.x*zoom;
			image_pos.y = mouse_pos.y - offset.y*zoom;
		}
	}

	if (IsKeyDown(KEY_SPACE)) {
		if (!pan_mode) {
			pan_mode = true;
			prev_cur_pos = mouse_pos;
		}

		const float dx = mouse_pos.x - prev_cur_pos.x;
		const float dy = mouse_pos.y - prev_cur_pos.y;

		image_pos.x += (dx*PANNING_FACTOR)*(zoom*PANNING_ZOOM_FACTOR);
		image_pos.y += (dy*PANNING_FACTOR)*(zoom*PANNING_ZOOM_FACTOR);

		prev_cur_pos = mouse_pos;
	} else {
		pan_mode = false;
	}

	if (alt_mode && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
		if (!selection_mode) {
			selection_start = mouse_pos;			
			selection_mode = true;
		} else if (!preserve_selection_mode
					 &&	!IsMouseButtonDown(MOUSE_LEFT_BUTTON))
		{
			stop_selection_mode();
			return;
		}

		if (!preserve_selection_mode
		&&	IsMouseButtonPressed(MOUSE_RIGHT_BUTTON))
		{
			preserve_selection_mode = true;
		}
	} else if (selection_mode && !preserve_selection_mode) {
		stop_selection_mode();
	}
}

void draw_selection(void)
{
	if (selection_mode
	&&	selection_start.x != SELECTION_UNINITIALIZED)
	{
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
									 (Vector2) {0},
									 0,
									 WHITE);
	}
}

INLINE void save_original_image_data(void)
{
	original_image_data = (uint8_t *) malloc(sizeof(RGB)*
																					 screenshot.width*
																					 screenshot.height);

	memcpy(original_image_data,
				 screenshot.data,
				 sizeof(RGB)*screenshot.width*screenshot.height);
}

i32 main(void)
{
	xdisplay = XOpenDisplay(NULL);
	if (!xdisplay) {
		panic("could not to open X display");
	}

	const Window root = DefaultRootWindow(xdisplay);
	XGetWindowAttributes(xdisplay, root, &gwa);

	capture_screen(root, gwa);
	save_original_image_data();

	XCloseDisplay(xdisplay);
	init_raylib(gwa.width, gwa.height);

	screenshot_texture = LoadTextureFromImage(screenshot);
	darker_screenshot_texture = LoadTextureFromImage(darker_screenshot);

	cur_pos = (Vector2) {center_x, center_y};

	while (!WindowShouldClose()) {
		handle_input();
		BeginDrawing();
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
		EndDrawing();
	}

#define X UnloadImage
	XSCREENSHOTS
#undef X

#define X UnloadTexture
	XTEXTURES
#undef X

	deinit_raylib();

	return 0;
}
