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
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

#include <raylib.h>
#include <raymath.h>

#define DEBUG 0
#define WAYLAND

#ifndef WAYLAND
#define Font XFont
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#undef Font
#endif

#define SCRATCH_BUFFER_IMPLEMENTATION
#include "scratch_buffer.h"

#include "font.h"
#include "hash.c"

extern char **environ;

#if DEBUG != 0
#define TIMESTAMP(label) do { \
	struct timespec _ts; \
	clock_gettime(CLOCK_REALTIME, &_ts); \
	eprintf("[%s] %ld.%09ld\n", label, _ts.tv_sec, _ts.tv_nsec); \
} while (0)
#else
#define TIMESTAMP(label)
#endif

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

#define WINDOW_FLAGS (FLAG_WINDOW_UNDECORATED | FLAG_WINDOW_TOPMOST | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT)

#define DARKEN_FACTOR 0.45f

#define FONT_SIZE 20.0f

#define SCROLL_SENSITIVITY 350.0f
#define BOOSTED_SCROLL_SENSITIVITY (SCROLL_SENSITIVITY*3)

#define SELECTION_DRAG_THRESHOLD 4.0f
#define MIN_SELECTION_SIZE       2.0f


#define BRUSH_RADIUS_SENSITIVITY 1.0f
#define BOOSTED_BRUSH_RADIUS_SENSITIVITY 3.0f

#define ZOOM_SPEED 1.2f
#define BOOSTED_ZOOM_SPEED 2.2f
#define MIN_ZOOM 0.4f
#define MAX_ZOOM 10.0f
#define STARTING_ZOOM 1.0f
#define BOOSTED_ZOOM_STEP 1.30f
#define ZOOM_STEP 1.12f
#define ZOOM_SMOOTHING_FACTOR 0.25f

#define PANNING_FACTOR 1.0f
#define PAN_ZOOM_EXPONENT 0.5f

#define SCROLL_SPEED 150.0f
#define SMOOTHING_FACTOR 0.1f

#define RADIUS_ZOOM_OUT_FACTOR 7.6f
#define STARTING_RADIUS 150

#define RESIZE_RING_RADIUS        10.0f
#define RESIZE_RING_THICKNESS     1.3f
#define RESIZE_RING_SEGMENTS      30
#define RESIZE_RING_HIT_RADIUS    14.0f   // much bigger than the visual radius - forgiving grab area
#define RESIZE_RING_ANIM_SPEED    14.0f   // higher = snappier fill/unfill

#define RESIZE_RING_COLOR         ((Color) {0, 170, 47, 255})
#define RESIZE_RING_HOVER_COLOR   ((Color) {60, 210, 100, 255})
#define RESIZE_RING_PRESSED_COLOR ((Color) {255, 165, 0, 255})

#define SELECTION_CORNER_COUNT 4

static float corner_fill[SELECTION_CORNER_COUNT] = {0}; // animated 0..1 per corner

static inline float ease_towards(float current, float target, float speed, float dt)
{
	return current + (target - current) * (1.0f - expf(-speed * dt));
}

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
	SELECTION_UPPER_LEFT = 0,
	SELECTION_UPPER_RIGHT,
	SELECTION_BOTTOM_LEFT,
	SELECTION_BOTTOM_RIGHT,
	SELECTION_INSIDE,     // not a corner index, never used on corners[]/corner_fill[]
	SELECTION_POISONED    // sentinel, never used on corners[]/corner_fill[]
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

#define OUTPUT_DIR_NAME "Pictures"
#define OUTPUT_FILE_NAME "screenshot"
#define OUTPUT_FILE_EXTENSION ".png"

static char   output_file_base[PATH_MAX + 64]; // e.g. /home/user/Pictures/screenshot
static size_t output_file_name_len = 0;

#define BRUSH_COLOR RED
#define BRUSH_RADIUS 3.0f

static Color brush_color = BRUSH_COLOR;
static float brush_radius = BRUSH_RADIUS;

static float zoom = STARTING_ZOOM;
static float target_zoom = STARTING_ZOOM;

static u32 radius = STARTING_RADIUS;

static bool resizing_now, drawing_now = false;
static u8 resizing_what = SELECTION_POISONED;

static bool timer_mode = false;
static time_t timer_start = 0;

static Font font = {0};

static Shader circle_shader = {0};
static int circle_shader_radius_loc = -1;
static int circle_shader_center_loc = -1;
static int circle_shader_resol_loc = -1;
static int circle_shader_smoothness_loc = -1;

static bool pan_mode, alt_mode, selection_mode, resize_mode = false;
static Vector2 resize_anchor;
static bool suppress_draw_until_release = false;

#define DOUBLE_UNINITIALIZED 0.0f

static bool color_selector_mode = false;
static float color_selector_mode_ending = DOUBLE_UNINITIALIZED;
static Vector2 color_selector_entered_position = {DOUBLE_UNINITIALIZED, DOUBLE_UNINITIALIZED};

static Vector2 selection_start, selection_end = {DOUBLE_UNINITIALIZED, DOUBLE_UNINITIALIZED};

static Vector2 cur_pos, image_pos, dmouse_pos = {0};

#ifndef WAYLAND
static Display *xdisplay = NULL;
static XWindowAttributes gwa = {0};
#endif

int screen_width, screen_height;

static Image screenshot, darker_screenshot = {0};
static Texture2D screenshot_texture, darker_screenshot_texture = {0};

static u8 *original_image_data = NULL;

static u8 *drawn_data_buffer  = NULL; // full-res working copy of the image
static u8 *crop_data_buffer   = NULL; // holds the (possibly cropped) output
static u8 *row_scratch_buffer = NULL; // one row, used for in-place flips

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
	SetTraceLogLevel(LOG_NONE);
	if (!DEBUG) SetConfigFlags(WINDOW_FLAGS);
	TIMESTAMP("  before InitWindow");

	InitWindow(screen_width, screen_height, "ss");
	SetWindowPosition(0, 0);
	TIMESTAMP("  after InitWindow");

	const int m = GetCurrentMonitor();
	const int fps = GetMonitorRefreshRate(m);
	SetTargetFPS(fps);
	TIMESTAMP("  after monitor/fps queries");

	font = LoadFont_Font();
	TIMESTAMP("  after LoadFont_Font");

	HideCursor();

	circle_shader = LoadShaderFromMemory(0, CIRCLE_SHADER);
	TIMESTAMP("  after LoadShaderFromMemory");

	circle_shader_radius_loc     = GetShaderLocation(circle_shader, "radius");
	circle_shader_center_loc     = GetShaderLocation(circle_shader, "center");
	circle_shader_resol_loc      = GetShaderLocation(circle_shader, "renderSize");
	circle_shader_smoothness_loc = GetShaderLocation(circle_shader, "smoothness");
	TIMESTAMP("  after GetShaderLocation calls");

	raylib_initialized = true;
}

INLINE static void deinit_raylib(void)
{
	if (raylib_initialized) {
		UnloadShader(circle_shader);
		UnloadTexture(font.texture);
		UnloadRenderTexture(canvas);
		CloseWindow();
	}
}

INLINE static Vector2 Vector2Value(float value)
{
	return (Vector2) {value, value};
}

INLINE static Vector2 Vector2DivideValue(Vector2 v, float div)
{
	return (Vector2) { v.x / div, v.y / div };
}

INLINE static Vector2 screen_to_image(Vector2 screen_pos)
{
	return Vector2DivideValue(Vector2Subtract(screen_pos, image_pos), zoom);
}

INLINE static Vector2 image_to_screen(Vector2 image_coord)
{
	return Vector2Add(Vector2Multiply(image_coord, Vector2Value(zoom)), image_pos);
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

#ifdef WAYLAND
static void capture_screen(void)
{
	int pipefd[2];
	if (pipe(pipefd) != 0) {
		panic("could not create pipe: %s\n", strerror(errno));
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&actions, pipefd[0]);
	posix_spawn_file_actions_addclose(&actions, pipefd[1]);

	char *spawn_argv[] = {"grim", "-t", "ppm", "-", NULL};

	pid_t pid;
	int spawn_ret = posix_spawnp(&pid, "grim", &actions, NULL, spawn_argv, environ);

	posix_spawn_file_actions_destroy(&actions);
	close(pipefd[1]);

	if (spawn_ret != 0) {
		close(pipefd[0]);
		panic("could not execute `grim`: %s\n", strerror(spawn_ret));
	}

	FILE *pipe = fdopen(pipefd[0], "rb");
	if (!pipe) {
		panic("fdopen on grim pipe failed: %s\n", strerror(errno));
	}

	//
	//
	// Parse PPM (P6) header: "P6\n<w> <h>\n<maxval>\n" then raw binary RGB
	//
	//

	int w = 0, h = 0, maxval = 0;
	if (fscanf(pipe, "P6 %d %d %d", &w, &h, &maxval) != 3) {
		panic("failed to parse PPM header from grim output\n");
	}

	fgetc(pipe);  // Consume the single whitespace byte between header and binary data.

	if (w <= 0 || h <= 0) {
		panic("invalid PPM dimensions from grim (%d x %d)\n", w, h);
	}

	screen_width  = w;
	screen_height = h;

	const usize plane_size = (usize) w * (usize) h * sizeof(RGB);
	u8 *data = (u8 *) malloc(plane_size);
	if (!data) {
		panic("failed to allocate screenshot buffer\n");
	}

	usize total_read = 0;
	while (total_read < plane_size) {
		usize n = fread(data + total_read, 1, plane_size - total_read, pipe);
		if (n == 0) break;
		total_read += n;
	}

	fclose(pipe);

	int status;
	waitpid(pid, &status, 0);

	if (total_read != plane_size) {
		panic("short read from grim: expected %zu bytes, got %zu\n", plane_size, total_read);
	}

	fill_image(&screenshot,
						 w, h,
						 PIXELFORMAT_UNCOMPRESSED_R8G8B8,
						 data);
}

static void compute_darker_screenshot(void)
{
	const u32 w = screenshot.width;
	const u32 h = screenshot.height;
	const usize plane_size = (usize) w * (usize) h * sizeof(RGB);

	u8 *darker_data = (u8 *) malloc(plane_size);
	if (!darker_data) {
		panic("failed to allocate darker screenshot buffer\n");
	}

	const u8 *src = (const u8 *) screenshot.data;

	for (usize i = 0; i < plane_size; i++) {
		darker_data[i] = darken_channel(src[i]);
	}

	fill_image(&darker_screenshot,
						 w, h,
						 PIXELFORMAT_UNCOMPRESSED_R8G8B8,
						 darker_data);
}
#else
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

	screen_width  = w;
	screen_height = h;

	u8 *data = (u8 *) malloc(w*h*sizeof(RGB));
	u8 *darker_data = (u8 *) malloc(w*h*sizeof(RGB));

	for (usize y = 0; y < h; y++) {
		for (usize x = 0; x < w; x++) {
			const u32 p = XGetPixel(ximage, x, y);
			const usize idx = (y*w + x)*sizeof(RGB);

			const u8 r = (p & ximage->red_mask)   >> 16;
			const u8 g = (p & ximage->green_mask) >> 8;
			const u8 b = (p & ximage->blue_mask)  >> 0;

			data[idx + 0] = r;
			data[idx + 1] = g;
			data[idx + 2] = b;

			darker_data[idx + 0] = darken_channel(r);
			darker_data[idx + 1] = darken_channel(g);
			darker_data[idx + 2] = darken_channel(b);
		}
	}

	XDestroyImage(ximage);

	fill_image(&screenshot,        w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8, data);
	fill_image(&darker_screenshot, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8, darker_data);
}
#endif

// Stolen from: <https://github.com/NSinecode/Raylib-Drawing-texture-in-circle/blob/master/CircleTextureDrawing.cpp>
static void DrawCollisionTextureCircle(Texture2D texture,
																Vector2 pos,
																Vector2 circle_center,
																float radius,
																Color color)
{
	SetShaderValue(circle_shader, circle_shader_radius_loc, &radius, SHADER_UNIFORM_FLOAT);

	const float ci_ce[2] = {circle_center.x, circle_center.y};
	SetShaderValue(circle_shader, circle_shader_center_loc, &ci_ce, SHADER_UNIFORM_VEC2);

	const float resolution[2] = {(float) texture.width, (float) texture.height};
	SetShaderValue(circle_shader, circle_shader_resol_loc, &resolution, SHADER_UNIFORM_VEC2);

	const float smoothness = 10.0f;
	SetShaderValue(circle_shader, circle_shader_smoothness_loc, &smoothness, SHADER_UNIFORM_FLOAT);

	BeginShaderMode(circle_shader);

	DrawTextureEx(texture, pos, 0, zoom, color);

	EndShaderMode();
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

INLINE static void init_output_path(void)
{
	const char *home = getenv("HOME");
	if (!home) {
		panic("could not determine HOME directory (is $HOME set?)\n");
	}

	char dir_path[PATH_MAX];
	snprintf(dir_path, sizeof(dir_path), "%s/%s", home, OUTPUT_DIR_NAME);

	if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
		panic("could not create directory `%s`: %s\n", dir_path, strerror(errno));
	}

	snprintf(output_file_base, sizeof(output_file_base), "%s/%s", dir_path, OUTPUT_FILE_NAME);
	output_file_name_len = strlen(output_file_base);
}

#define get_file_path(...) get_file_path_(__VA_ARGS__, 0)

// add _<number> at the end if needed to prevent overwriting
char *get_file_path_(char *file_path, u64 rec_count)
{
	if (access(file_path, F_OK) == 0) {
		scratch_buffer_clear();

		char *number_start = file_path + output_file_name_len;
		if (*number_start == '\0') {
			scratch_buffer_printf("%s_%zu.png", output_file_base, 0);
		} else {
			u64 number = strtoull(number_start + 1, NULL, 10);
			scratch_buffer_printf("%s_%zu.png", output_file_base, number + 1);
		}

		return get_file_path_(scratch_buffer_to_string(), rec_count++);
	}

	return file_path;
}

static char *initial_output_path(void)
{
	scratch_buffer_clear();
	scratch_buffer_printf("%s%s", output_file_base, OUTPUT_FILE_EXTENSION);
	return scratch_buffer_to_string();
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

	UnloadImage(canvas_image);

	return image.data;
}

INLINE static void flip_vertical_inplace(u8 *data, i32 w, i32 h)
{
	const usize row_size = (usize) w * sizeof(RGB);
	for (i32 y = 0; y < h / 2; y++) {
		u8 *top = data + (usize) y * row_size;
		u8 *bot = data + (usize) (h - 1 - y) * row_size;
		memcpy(row_scratch_buffer, top, row_size);
		memcpy(top, bot, row_size);
		memcpy(bot, row_scratch_buffer, row_size);
	}
}

INLINE static void save_fullscreen(void)
{
	const char *file_path = get_file_path(initial_output_path());

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
	const char *file_path = get_file_path(initial_output_path());

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

INLINE static void crop_image(const u8 *img_data,
														 i32 img_w, i32 img_h,
														 i32 w, i32 h,
														 i32 x, i32 y,
														 u8 *out_data)
{
	for (i32 row = 0; row < h; row++) {
		i32 wy = wrap(y + row, img_h);
		for (i32 col = 0; col < w; col++) {
			i32 wx = wrap(x + col, img_w);
			i32 src_offset = (wy*img_w + wx)*sizeof(RGB);
			i32 dst_offset = (row*w + col)*sizeof(RGB);
			memcpy(out_data + dst_offset, img_data + src_offset, sizeof(RGB));
		}
	}
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
	const whxy_t whxy = get_selection_data(); // image space
	WHXY_UNPACK

	const Vector2 top_left = image_to_screen((Vector2) {x, y});
	const Rectangle rec = {
		.width = w * zoom,
		.height = h * zoom,
		.x = top_left.x,
		.y = top_left.y,
	};

	return CheckCollisionPointRec(mouse_pos, rec);
}

static u8 selection_check_corner_collisions(Vector2 mouse_pos)
{
	Vector2 up_l, up_r, bot_l, bot_r = {0};
	get_selection_corners(get_selection_data(), &up_l, &up_r, &bot_l, &bot_r);

	up_l  = image_to_screen(up_l);
	up_r  = image_to_screen(up_r);
	bot_l = image_to_screen(bot_l);
	bot_r = image_to_screen(bot_r);

	if (CheckCollisionPointCircle(mouse_pos, up_l, RESIZE_RING_RADIUS))  return SELECTION_UPPER_LEFT;
	if (CheckCollisionPointCircle(mouse_pos, up_r, RESIZE_RING_RADIUS))  return SELECTION_UPPER_RIGHT;
	if (CheckCollisionPointCircle(mouse_pos, bot_l, RESIZE_RING_RADIUS)) return SELECTION_BOTTOM_LEFT;
	if (CheckCollisionPointCircle(mouse_pos, bot_r, RESIZE_RING_RADIUS)) return SELECTION_BOTTOM_RIGHT;

	return SELECTION_POISONED;
}

static void take_screenshot(void)
{
	if (selection_mode) {
		const whxy_t whxy = get_selection_data();
		WHXY_UNPACK_I32

		memcpy(drawn_data_buffer, original_image_data, sizeof(RGB)*screenshot.width*screenshot.height);

		flip_vertical_inplace(drawn_data_buffer, screenshot.width, screenshot.height);

		draw_canvas_into_image(drawn_data_buffer, screenshot.width, screenshot.height);
		flip_vertical_inplace(drawn_data_buffer, screenshot.width, screenshot.height);

		crop_image(drawn_data_buffer, screenshot.width, screenshot.height, w, h, x, y, crop_data_buffer);

		stop_selection_mode();
		save_image_data(crop_data_buffer, w, h);
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

static bool handle_input(void)
{
	const float wheel_move = GetMouseWheelMove();
	const Vector2 mouse_pos = GetMousePosition();

	if (!color_selector_mode) {
		cur_pos = mouse_pos;
	}

	if (selection_mode && !resize_mode) {
    selection_end = screen_to_image(cur_pos);
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

	// Wait quarter of a second to not draw accidentally
	if (color_selector_mode_ending != DOUBLE_UNINITIALIZED) {
		if (GetTime() - color_selector_mode_ending > 0.25) {
			color_selector_mode_ending = DOUBLE_UNINITIALIZED;
		} else {
			return false;
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

			case SELECTION_UPPER_LEFT:
			case SELECTION_UPPER_RIGHT:
			case SELECTION_BOTTOM_LEFT:
			case SELECTION_BOTTOM_RIGHT: {
			    const Vector2 drag = screen_to_image(mouse_pos);

			    float dx = drag.x - resize_anchor.x;
			    float dy = drag.y - resize_anchor.y;

			    // Keep at least MIN_SELECTION_SIZE away from the anchor on
			    // whichever side we're currently on, so the rect can't collapse
			    // to zero right at the flip point — but let the sign flip once
			    // the mouse has actually crossed to the other side.
			    if (fabsf(dx) < MIN_SELECTION_SIZE) {
			        dx = (dx < 0.0f) ? -MIN_SELECTION_SIZE : MIN_SELECTION_SIZE;
			    }
			    if (fabsf(dy) < MIN_SELECTION_SIZE) {
			        dy = (dy < 0.0f) ? -MIN_SELECTION_SIZE : MIN_SELECTION_SIZE;
			    }

			    const Vector2 clamped = {
			        resize_anchor.x + dx,
			        resize_anchor.y + dy,
			    };

			    selection_start.x = fminf(resize_anchor.x, clamped.x);
			    selection_end.x   = fmaxf(resize_anchor.x, clamped.x);
			    selection_start.y = fminf(resize_anchor.y, clamped.y);
			    selection_end.y   = fmaxf(resize_anchor.y, clamped.y);
			} break;

			default: panic("unreachable"); break;
			}
		} else {
			stop_resizing();
		}
	}

	if (!drawing_now && !pan_mode && resize_mode && !resizing_now && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
    const u8 corner = selection_check_corner_collisions(mouse_pos);
    if (corner != SELECTION_POISONED) {
        resizing_now = true;
        resizing_what = corner;

        switch (corner) {
        case SELECTION_UPPER_LEFT:
            resize_anchor = selection_end;
            break;
        case SELECTION_UPPER_RIGHT:
            resize_anchor = (Vector2) { selection_start.x, selection_end.y };
            break;
        case SELECTION_BOTTOM_LEFT:
            resize_anchor = (Vector2) { selection_end.x, selection_start.y };
            break;
        case SELECTION_BOTTOM_RIGHT:
            resize_anchor = selection_start;
            break;
        default: break;
        }

    } else if (alt_mode && selection_check_collisions(mouse_pos)) {
        resizing_now = true;
        resizing_what = SELECTION_INSIDE;
    }
	}

	if (suppress_draw_until_release) {
		if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
		    suppress_draw_until_release = false;
		}

	} else if (!color_selector_mode && !pan_mode && !alt_mode && (!resize_mode || (resize_mode && !resizing_now))) {
		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			drawing_now = true;

			const Vector2 img_start = screen_to_image(dmouse_pos);
			const Vector2 img_end   = screen_to_image(mouse_pos);

			BeginTextureMode(canvas);
			{
        const int nsteps = MAX(1, (int) Vector2Distance(img_start, img_end));

				for (int step = 0; step < nsteps; step++) {
					const Vector2 ipos = Vector2Lerp(img_start, img_end, (float) step / nsteps);
					DrawCircle((int) ipos.x, (int) ipos.y, brush_radius, brush_color);
				}
			}
			EndTextureMode();

		} else if (drawing_now) {
			drawing_now = false;
		}
	}

	if (IsKeyPressed(KEY_C)) {
		stop_timer_mode();
		if (color_selector_mode) {
			stop_color_selector_mode();

		} else if (selection_mode) {
			stop_resizing();
			stop_selection_mode();

		} else {
			clear_canvas();
			zoom = STARTING_ZOOM;
			target_zoom = STARTING_ZOOM;
			radius = STARTING_RADIUS;
			image_pos = Vector2Zero();
			SetMousePosition(center_x, center_y);
			cur_pos = (Vector2) {center_x, center_y};
		}
	}

	else if (IsKeyPressed(KEY_ENTER)) {
		take_screenshot();
		return true;
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

			const float tradius = MIN(MIN(screen_width, screen_height), MAX(15.0, radius + sine*sens));
			radius += (tradius - radius)*SMOOTHING_FACTOR;

		} else {
      const float step = IsKeyDown(KEY_LEFT_SHIFT) ? BOOSTED_ZOOM_STEP : ZOOM_STEP;
      target_zoom *= powf(step, wheel_move);
      target_zoom = Clamp(target_zoom, MIN_ZOOM, MAX_ZOOM);
		}
	}

	if (fabsf(zoom - target_zoom) > 0.0005f) {
		const Vector2 anchor = screen_to_image(mouse_pos);
		zoom = Lerp(zoom, target_zoom, ZOOM_SMOOTHING_FACTOR);
		image_pos = Vector2Subtract(mouse_pos, Vector2Multiply(anchor, Vector2Value(zoom)));
	} else {
		zoom = target_zoom;
	}

	if (IsKeyDown(KEY_SPACE)) {
		if (!pan_mode) {
			pan_mode = true;
		}

		if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			Vector2 delta = Vector2Subtract(mouse_pos, dmouse_pos);
			const float pan_scale = PANNING_FACTOR * powf(zoom, PAN_ZOOM_EXPONENT);
			delta = Vector2Scale(delta, pan_scale);
			image_pos = Vector2Add(image_pos, delta);
		}

		cur_pos = mouse_pos;

	} else {
		if (pan_mode && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
			suppress_draw_until_release = true;
		}

		pan_mode = false;
	}

	if (alt_mode) {
		if (!selection_mode && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
      selection_start = screen_to_image(mouse_pos);
			selection_end = selection_start;
			selection_mode = true;

		} else if (selection_mode && !IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
			// Snap start->top-left, end->bottom-right so the corner
			// switch below (which assumes that layout) is always correct,
			// regardless of which direction the box was originally dragged.
			const whxy_t whxy = get_selection_data();
			selection_start = (Vector2) { whxy.x, whxy.y };
			selection_end   = (Vector2) { whxy.x + whxy.w, whxy.y + whxy.h };
			resize_mode = true;
		}

	} else if (selection_mode && !resize_mode) {
		// Alt released and no corner is actively being dragged — exit
		// the tool entirely so the cursor/mode snaps back to painting.
		stop_resizing();
		stop_selection_mode();
	}

	dmouse_pos = cur_pos;

	return false;
}

static void draw_selection(void)
{
	if (selection_start.x == DOUBLE_UNINITIALIZED) return;
	DrawTextureEx(darker_screenshot_texture, image_pos, 0, zoom, WHITE);

	const whxy_t whxy = get_selection_data(); // image space

	if (whxy.w < SELECTION_DRAG_THRESHOLD && whxy.h < SELECTION_DRAG_THRESHOLD) {
		return; // just a click so far, nothing to reveal/resize yet
	}

	WHXY_UNPACK

	const Rectangle src_rect = { x, y, w, h }; // already matches texture pixels 1:1, no conversion needed

	const Vector2 screen_top_left = image_to_screen((Vector2) {x, y});
	const Rectangle selection = {
		.x = screen_top_left.x, .y = screen_top_left.y,
		.width = w * zoom, .height = h * zoom
	};

	DrawTexturePro(screenshot_texture, src_rect, selection, Vector2Zero(), 0, WHITE);

	Vector2 corners[SELECTION_CORNER_COUNT];
	get_selection_corners(whxy, &corners[SELECTION_UPPER_LEFT],  &corners[SELECTION_UPPER_RIGHT],
                              &corners[SELECTION_BOTTOM_LEFT], &corners[SELECTION_BOTTOM_RIGHT]);


	const Vector2 mouse_pos      = GetMousePosition();
	const bool    mouse_down     = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
	const float   dt             = GetFrameTime();
	const u8      hovered_corner = selection_check_corner_collisions(mouse_pos);

	for (int i = 0; i < SELECTION_CORNER_COUNT; i++) {
		corners[i] = image_to_screen(corners[i]);

		const bool is_hovered = (hovered_corner == i);
		const float target    = is_hovered ? 1.0f : 0.0f;
		corner_fill[i] = ease_towards(corner_fill[i], target, RESIZE_RING_ANIM_SPEED, dt);

		// always-visible outline
		DrawRing(corners[i], RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS, RESIZE_RING_RADIUS,
		         0.0f, 365.0f, RESIZE_RING_SEGMENTS, RESIZE_RING_COLOR);

		// filling pie-slice, growing with corner_fill[i]; swaps color while actively dragging
		if (corner_fill[i] > 0.01f) {
			const Color fill_color = (is_hovered && mouse_down) ? RESIZE_RING_PRESSED_COLOR
			                                                     : RESIZE_RING_HOVER_COLOR;
			DrawCircleSector(corners[i], RESIZE_RING_RADIUS - RESIZE_RING_THICKNESS,
			                  0.0f, 360.0f * corner_fill[i], RESIZE_RING_SEGMENTS, fill_color);
		}
	}
}

static void draw_canvas(void)
{
	Rectangle src_rect = {
		0, 0,
		(float) canvas.texture.width,
		(float) -canvas.texture.height
	};

	Rectangle dst_rec = {
		image_pos.x, image_pos.y,
		(float) canvas.texture.width * zoom,
		(float) canvas.texture.height * zoom
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

	const Vector2 size = MeasureTextEx(font, text, FONT_SIZE, spacing);

	const float x = GetScreenWidth()*0.97 - size.x;
	const float y = GetScreenHeight()*0.97 - size.y;

	const float pad = 50.0f;

	DrawRectangle(x - pad/2, y - pad/2, size.x + pad, size.y + pad, (Color){0, 0, 0, 150});
	DrawTextEx(font, text, (Vector2) {x, y}, FONT_SIZE, spacing, WHITE);
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

#ifndef WAYLAND
	xdisplay = XOpenDisplay(NULL);
	if (!xdisplay) {
		panic("could not to open X display");
	}

	const Window root = DefaultRootWindow(xdisplay);
	XGetWindowAttributes(xdisplay, root, &gwa);
#endif

	cur_pos = (Vector2) {center_x, center_y};
	init_output_path();

	TIMESTAMP("start");

#ifndef WAYLAND
	capture_screen(root, gwa);
#else
	capture_screen();
#endif

	TIMESTAMP("after capture_screen");

	preserve_original_image_data();

	TIMESTAMP("after preserve_original_image_data");

	if (immediate_screenshot_and_exit) {
		save_fullscreen();
		exit(0);
	}

#ifdef WAYLAND
	compute_darker_screenshot();
#endif

	const usize max_crop_w = (usize) (screenshot.width  / MIN_ZOOM) + 1;
	const usize max_crop_h = (usize) (screenshot.height / MIN_ZOOM) + 1;

	drawn_data_buffer  = (u8 *) malloc(sizeof(RGB) * screenshot.width * screenshot.height);
	crop_data_buffer   = (u8 *) malloc(sizeof(RGB) * max_crop_w * max_crop_h);
	row_scratch_buffer = (u8 *) malloc(sizeof(RGB) * screenshot.width);

	if (!drawn_data_buffer || !crop_data_buffer || !row_scratch_buffer) {
		panic("failed to allocate scratch buffers\n");
	}

	init_raylib();

	TIMESTAMP("after init_raylib");

	canvas = LoadRenderTexture(screenshot.width, screenshot.height);
	TIMESTAMP("after LoadTextureFromImage");
	clear_canvas();

	TIMESTAMP("after canvas setup");

	screenshot_texture = LoadTextureFromImage(screenshot);
	SetTextureFilter(screenshot_texture, TEXTURE_FILTER_BILINEAR);

	darker_screenshot_texture = LoadTextureFromImage(darker_screenshot);
	SetTextureFilter(darker_screenshot_texture, TEXTURE_FILTER_BILINEAR);

	TIMESTAMP("after SetTextureFilters");

	cur_pos = GetMousePosition();

	while (!WindowShouldClose()) {
		if (handle_input()) break;

		BeginDrawing();
		{
			ClearBackground(BACKGROUND_COLOR);
			if (selection_mode) {
				draw_selection();

			} else if (drawing_now || color_selector_mode) {
				DrawTextureEx(screenshot_texture, image_pos, 0, zoom, WHITE);

			} else {
				DrawTextureEx(darker_screenshot_texture, image_pos, 0, zoom, WHITE);

				// Translate screen coordinates
				Vector2 texture_center = screen_to_image(cur_pos);

				// Scale the radius so it visually stays the same size
				float scaled_radius = radius / zoom;

				DrawCollisionTextureCircle(screenshot_texture, image_pos, texture_center, scaled_radius, WHITE);

				if (pan_mode) {
					const char *label = "M";
					const float spacing = 2.0f;
					const Vector2 size = MeasureTextEx(font, label, FONT_SIZE, spacing);
					const Vector2 pos = { cur_pos.x - size.x/2.0f, cur_pos.y - size.y/2.0f };

					DrawTextEx(font, label, (Vector2){pos.x + 1, pos.y + 1}, FONT_SIZE, spacing, BLACK); // cheap shadow for legibility
					DrawTextEx(font, label, pos, FONT_SIZE, spacing, WHITE);
				}
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

#ifndef WAYLAND
	XCloseDisplay(xdisplay);
#endif

	free(original_image_data);
	free(drawn_data_buffer);
	free(crop_data_buffer);
	free(row_scratch_buffer);

	if (argc > 1) {
		memory_release();
	}

	return 0;
}
