/*
 * Scene Input Overlay
 *
 * A video source that draws a live multiview-style grid of:
 *   - every input (source) inside a scene you choose, and/or
 *   - up to 12 extra sources you pick yourself, independent of that scene.
 *
 * Add it to any scene via Sources -> + -> "Scene Input Overlay".
 */

#include <obs-module.h>
#include <util/threading.h>
#include <graphics/vec4.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define SOURCE_ID "scene_input_overlay"
#define MAX_EXTRA 12
#define REFRESH_INTERVAL 0.5f

#define S_SCENE "scene"
#define S_SHOW_SCENE "show_scene_sources"
#define S_HIDDEN "include_hidden"
#define S_GROUPS "expand_groups"
#define S_EXTRA_COUNT "extra_count"
#define S_WIDTH "width"
#define S_HEIGHT "height"
#define S_COLUMNS "columns"
#define S_SPACING "spacing"
#define S_BG "bg_color"
#define S_TILE "tile_color"
#define S_PREVIEW "preview_off_air"

#define T_(v) obs_module_text(v)

/* ------------------------------------------------------------------ */

struct source_list {
	obs_source_t **items;
	size_t num;
	size_t cap;
};

static void list_push(struct source_list *l, obs_source_t *src)
{
	if (l->num == l->cap) {
		size_t new_cap = l->cap ? l->cap * 2 : 8;
		l->items = brealloc(l->items, new_cap * sizeof(*l->items));
		l->cap = new_cap;
	}
	l->items[l->num++] = src;
}

struct overlay {
	obs_source_t *source;
	pthread_mutex_t mutex;

	struct source_list tiles; /* strong refs to every source we draw */
	bool visible;             /* we are being shown somewhere */
	bool rendering;           /* recursion guard (graphics thread only) */
	float elapsed;

	uint32_t width;
	uint32_t height;
	uint32_t columns; /* 0 = automatic */
	uint32_t spacing;
	uint32_t bg_color;
	uint32_t tile_color;

	char *scene_name;
	bool show_scene;
	bool include_hidden;
	bool expand_groups;
	bool preview_off_air;

	volatile bool live; /* true = visible on stream */
	obs_hotkey_id hk_live;
	obs_hotkey_id hk_off;
	obs_hotkey_id hk_toggle;

	int extra_count;
	char *extras[MAX_EXTRA];
};

struct collect_ctx {
	struct overlay *s;
	struct source_list *out;
};

/* ------------------------------------------------------------------ */
/* Collecting sources                                                  */
/* ------------------------------------------------------------------ */

static bool is_video_source(obs_source_t *src)
{
	return (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) != 0;
}

/* Takes ownership of the reference passed in. */
static void push_source(struct collect_ctx *c, obs_source_t *src)
{
	if (!src)
		return;

	if (src == c->s->source || !is_video_source(src)) {
		obs_source_release(src);
		return;
	}

	list_push(c->out, src);
}

static bool scene_item_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	struct collect_ctx *c = param;
	UNUSED_PARAMETER(scene);

	if (!c->s->include_hidden && !obs_sceneitem_visible(item))
		return true;

	if (c->s->expand_groups && obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, scene_item_cb, c);
		return true;
	}

	push_source(c, obs_source_get_ref(obs_sceneitem_get_source(item)));
	return true;
}

/* Caller must hold s->mutex. */
static void refresh_locked(struct overlay *s)
{
	struct source_list fresh = {0};
	struct collect_ctx ctx = {s, &fresh};

	if (s->show_scene && s->scene_name && *s->scene_name) {
		obs_source_t *scene_src = obs_get_source_by_name(s->scene_name);
		if (scene_src) {
			obs_scene_t *scene = obs_scene_from_source(scene_src);
			if (scene)
				obs_scene_enum_items(scene, scene_item_cb, &ctx);
			obs_source_release(scene_src);
		}
	}

	for (int i = 0; i < s->extra_count; i++) {
		if (s->extras[i] && *s->extras[i])
			push_source(&ctx, obs_get_source_by_name(s->extras[i]));
	}

	/* Take new "showing" references first so sources that stay in the
	 * list never drop to zero (which would unload/reload them). */
	if (s->visible) {
		for (size_t i = 0; i < fresh.num; i++)
			obs_source_inc_showing(fresh.items[i]);
	}

	for (size_t i = 0; i < s->tiles.num; i++) {
		if (s->visible)
			obs_source_dec_showing(s->tiles.items[i]);
		obs_source_release(s->tiles.items[i]);
	}
	bfree(s->tiles.items);
	s->tiles = fresh;
}

/* ------------------------------------------------------------------ */
/* Source callbacks                                                    */
/* ------------------------------------------------------------------ */

static const char *overlay_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SceneInputOverlay");
}

static void overlay_update(void *data, obs_data_t *settings)
{
	struct overlay *s = data;
	char key[32];

	pthread_mutex_lock(&s->mutex);

	s->width = (uint32_t)obs_data_get_int(settings, S_WIDTH);
	s->height = (uint32_t)obs_data_get_int(settings, S_HEIGHT);
	if (s->width < 16)
		s->width = 16;
	if (s->height < 16)
		s->height = 16;

	s->columns = (uint32_t)obs_data_get_int(settings, S_COLUMNS);
	s->spacing = (uint32_t)obs_data_get_int(settings, S_SPACING);
	s->bg_color = (uint32_t)obs_data_get_int(settings, S_BG);
	s->tile_color = (uint32_t)obs_data_get_int(settings, S_TILE);

	s->show_scene = obs_data_get_bool(settings, S_SHOW_SCENE);
	s->include_hidden = obs_data_get_bool(settings, S_HIDDEN);
	s->expand_groups = obs_data_get_bool(settings, S_GROUPS);
	s->preview_off_air = obs_data_get_bool(settings, S_PREVIEW);

	bfree(s->scene_name);
	s->scene_name = bstrdup(obs_data_get_string(settings, S_SCENE));

	s->extra_count = (int)obs_data_get_int(settings, S_EXTRA_COUNT);
	if (s->extra_count < 0)
		s->extra_count = 0;
	if (s->extra_count > MAX_EXTRA)
		s->extra_count = MAX_EXTRA;

	for (int i = 0; i < MAX_EXTRA; i++) {
		snprintf(key, sizeof(key), "extra_%d", i + 1);
		bfree(s->extras[i]);
		s->extras[i] = bstrdup(obs_data_get_string(settings, key));
	}

	refresh_locked(s);
	pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------ */
/* Live control: button + hotkeys                                      */
/* ------------------------------------------------------------------ */

static void set_live(struct overlay *s, bool live)
{
	if (os_atomic_load_bool(&s->live) == live)
		return;
	os_atomic_set_bool(&s->live, live);
	obs_log(LOG_INFO, "'%s' is now %s", obs_source_get_name(s->source), live ? "LIVE" : "off air");
}

static void hotkey_live_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_live(data, true);
}

static void hotkey_off_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_live(data, false);
}

static void hotkey_toggle_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct overlay *s = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		set_live(s, !os_atomic_load_bool(&s->live));
}

static bool live_button_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct overlay *s = data;
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);

	if (!s)
		return false;
	set_live(s, !os_atomic_load_bool(&s->live));
	return true; /* refresh the dialog so the button label updates */
}

static void *overlay_create(obs_data_t *settings, obs_source_t *source)
{
	struct overlay *s = bzalloc(sizeof(struct overlay));
	s->source = source;
	pthread_mutex_init(&s->mutex, NULL);

	/* Always starts off air, so nothing appears on stream by accident. */
	os_atomic_set_bool(&s->live, false);

	s->hk_live = obs_hotkey_register_source(source, "scene_input_overlay.send_live", T_("HotkeySendLive"),
						hotkey_live_cb, s);
	s->hk_off = obs_hotkey_register_source(source, "scene_input_overlay.take_off_air", T_("HotkeyTakeOffAir"),
					       hotkey_off_cb, s);
	s->hk_toggle = obs_hotkey_register_source(source, "scene_input_overlay.toggle_live", T_("HotkeyToggleLive"),
						  hotkey_toggle_cb, s);

	overlay_update(s, settings);
	return s;
}

static void overlay_destroy(void *data)
{
	struct overlay *s = data;

	pthread_mutex_lock(&s->mutex);
	for (size_t i = 0; i < s->tiles.num; i++) {
		if (s->visible)
			obs_source_dec_showing(s->tiles.items[i]);
		obs_source_release(s->tiles.items[i]);
	}
	bfree(s->tiles.items);
	s->tiles.items = NULL;
	s->tiles.num = 0;
	pthread_mutex_unlock(&s->mutex);

	for (int i = 0; i < MAX_EXTRA; i++)
		bfree(s->extras[i]);
	bfree(s->scene_name);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void overlay_show(void *data)
{
	struct overlay *s = data;

	pthread_mutex_lock(&s->mutex);
	if (!s->visible) {
		s->visible = true;
		for (size_t i = 0; i < s->tiles.num; i++)
			obs_source_inc_showing(s->tiles.items[i]);
	}
	pthread_mutex_unlock(&s->mutex);
}

static void overlay_hide(void *data)
{
	struct overlay *s = data;

	pthread_mutex_lock(&s->mutex);
	if (s->visible) {
		s->visible = false;
		for (size_t i = 0; i < s->tiles.num; i++)
			obs_source_dec_showing(s->tiles.items[i]);
	}
	pthread_mutex_unlock(&s->mutex);
}

static void overlay_tick(void *data, float seconds)
{
	struct overlay *s = data;

	s->elapsed += seconds;
	if (s->elapsed < REFRESH_INTERVAL)
		return;
	s->elapsed = 0.0f;

	pthread_mutex_lock(&s->mutex);
	refresh_locked(s);
	pthread_mutex_unlock(&s->mutex);
}

static uint32_t overlay_width(void *data)
{
	struct overlay *s = data;
	return s->width;
}

static uint32_t overlay_height(void *data)
{
	struct overlay *s = data;
	return s->height;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void draw_rect(float x, float y, float w, float h, uint32_t rgba)
{
	if ((rgba >> 24) == 0 || w < 1.0f || h < 1.0f)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	struct vec4 c;

	vec4_from_rgba(&c, rgba);
	gs_effect_set_vec4(color, &c);

	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_draw_sprite(NULL, 0, (uint32_t)w, (uint32_t)h);
	gs_matrix_pop();
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
}

static uint32_t auto_columns(size_t n, float w, float h)
{
	uint32_t best = 1;
	float best_fit = 0.0f;

	for (uint32_t c = 1; c <= n; c++) {
		uint32_t r = (uint32_t)((n + c - 1) / c);
		float tw = w / (float)c;
		float th = h / (float)r;
		float fit = fminf(tw, th * (16.0f / 9.0f));
		if (fit > best_fit) {
			best_fit = fit;
			best = c;
		}
	}
	return best;
}

static void overlay_render(void *data, gs_effect_t *effect)
{
	struct overlay *s = data;
	UNUSED_PARAMETER(effect);

	if (s->rendering)
		return;

	pthread_mutex_lock(&s->mutex);

	/* Off air: draw nothing on the stream. While off air we can still
	 * show a preview where the source is merely being edited / staged
	 * (e.g. the preview side of Studio Mode) but not in program. */
	if (!os_atomic_load_bool(&s->live) && (!s->preview_off_air || obs_source_active(s->source))) {
		pthread_mutex_unlock(&s->mutex);
		return;
	}

	s->rendering = true;

	gs_blend_state_push();
	gs_reset_blend_state();

	const float w = (float)s->width;
	const float h = (float)s->height;
	const size_t n = s->tiles.num;

	draw_rect(0.0f, 0.0f, w, h, s->bg_color);

	if (n > 0) {
		uint32_t cols = s->columns ? s->columns : auto_columns(n, w, h);
		if (cols > n)
			cols = (uint32_t)n;
		uint32_t rows = (uint32_t)((n + cols - 1) / cols);

		const float sp = (float)s->spacing;
		const float cw = (w - sp * (float)(cols + 1)) / (float)cols;
		const float ch = (h - sp * (float)(rows + 1)) / (float)rows;

		if (cw >= 1.0f && ch >= 1.0f) {
			for (size_t i = 0; i < n; i++) {
				obs_source_t *src = s->tiles.items[i];
				const float x = sp + (float)(i % cols) * (cw + sp);
				const float y = sp + (float)(i / cols) * (ch + sp);

				draw_rect(x, y, cw, ch, s->tile_color);

				const uint32_t sw = obs_source_get_width(src);
				const uint32_t sh = obs_source_get_height(src);
				if (!sw || !sh)
					continue;

				const float scale = fminf(cw / (float)sw, ch / (float)sh);
				const float ox = x + (cw - (float)sw * scale) * 0.5f;
				const float oy = y + (ch - (float)sh * scale) * 0.5f;

				gs_matrix_push();
				gs_matrix_translate3f(ox, oy, 0.0f);
				gs_matrix_scale3f(scale, scale, 1.0f);
				obs_source_video_render(src);
				gs_matrix_pop();
			}
		}
	}

	gs_blend_state_pop();

	s->rendering = false;
	pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------ */
/* Properties                                                          */
/* ------------------------------------------------------------------ */

struct enum_ctx {
	obs_property_t *list;
	obs_source_t *self;
	bool video_only;
};

static bool enum_names_cb(void *param, obs_source_t *src)
{
	struct enum_ctx *e = param;

	if (src == e->self)
		return true;
	if (obs_source_is_group(src))
		return true;
	if (e->video_only && !is_video_source(src))
		return true;

	const char *name = obs_source_get_name(src);
	obs_property_list_add_string(e->list, name, name);
	return true;
}

static void set_extra_visibility(obs_properties_t *props, int count)
{
	char key[32];

	for (int i = 0; i < MAX_EXTRA; i++) {
		snprintf(key, sizeof(key), "extra_%d", i + 1);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, i < count);
	}
}

static bool extra_count_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	set_extra_visibility(props, (int)obs_data_get_int(settings, S_EXTRA_COUNT));
	return true;
}

static obs_properties_t *overlay_properties(void *data)
{
	struct overlay *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	struct enum_ctx ctx;
	char label[128];
	char key[32];

	ctx.self = s ? s->source : NULL;

	/* Send live / take off air */
	const bool is_live = s && os_atomic_load_bool(&s->live);
	obs_properties_add_button2(props, "live_button", is_live ? T_("TakeOffAir") : T_("SendLive"),
				   live_button_clicked, s);
	obs_properties_add_bool(props, S_PREVIEW, T_("PreviewOffAir"));

	/* Scene picker */
	p = obs_properties_add_list(props, S_SCENE, T_("Scene"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("None"), "");
	ctx.list = p;
	ctx.video_only = false;
	obs_enum_scenes(enum_names_cb, &ctx);

	obs_properties_add_bool(props, S_SHOW_SCENE, T_("ShowSceneSources"));
	obs_properties_add_bool(props, S_HIDDEN, T_("IncludeHidden"));
	obs_properties_add_bool(props, S_GROUPS, T_("ExpandGroups"));

	/* Layout */
	obs_properties_add_int(props, S_WIDTH, T_("Width"), 64, 8192, 1);
	obs_properties_add_int(props, S_HEIGHT, T_("Height"), 64, 8192, 1);
	obs_properties_add_int(props, S_COLUMNS, T_("Columns"), 0, 12, 1);
	obs_properties_add_int(props, S_SPACING, T_("Spacing"), 0, 128, 1);
	obs_properties_add_color_alpha(props, S_BG, T_("BackgroundColor"));
	obs_properties_add_color_alpha(props, S_TILE, T_("TileColor"));

	/* User-chosen extra sources */
	p = obs_properties_add_int_slider(props, S_EXTRA_COUNT, T_("ExtraCount"), 0, MAX_EXTRA, 1);
	obs_property_set_modified_callback(p, extra_count_modified);

	for (int i = 0; i < MAX_EXTRA; i++) {
		snprintf(key, sizeof(key), "extra_%d", i + 1);
		snprintf(label, sizeof(label), "%s %d", T_("ExtraSource"), i + 1);

		p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, T_("None"), "");

		ctx.list = p;
		ctx.video_only = true;
		obs_enum_sources(enum_names_cb, &ctx);
		ctx.video_only = false;
		obs_enum_scenes(enum_names_cb, &ctx);
	}

	int count = 0;
	if (s) {
		obs_data_t *settings = obs_source_get_settings(s->source);
		count = (int)obs_data_get_int(settings, S_EXTRA_COUNT);
		obs_data_release(settings);
	}
	set_extra_visibility(props, count);

	return props;
}

static void overlay_defaults(obs_data_t *d)
{
	obs_data_set_default_string(d, S_SCENE, "");
	obs_data_set_default_bool(d, S_PREVIEW, true);
	obs_data_set_default_bool(d, S_SHOW_SCENE, true);
	obs_data_set_default_bool(d, S_HIDDEN, false);
	obs_data_set_default_bool(d, S_GROUPS, true);
	obs_data_set_default_int(d, S_EXTRA_COUNT, 0);
	obs_data_set_default_int(d, S_WIDTH, 1920);
	obs_data_set_default_int(d, S_HEIGHT, 1080);
	obs_data_set_default_int(d, S_COLUMNS, 0);
	obs_data_set_default_int(d, S_SPACING, 8);
	obs_data_set_default_int(d, S_BG, (long long)0xFF101010);
	obs_data_set_default_int(d, S_TILE, (long long)0xFF000000);
}

/* ------------------------------------------------------------------ */

static struct obs_source_info overlay_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = overlay_get_name,
	.create = overlay_create,
	.destroy = overlay_destroy,
	.update = overlay_update,
	.get_defaults = overlay_defaults,
	.get_properties = overlay_properties,
	.get_width = overlay_width,
	.get_height = overlay_height,
	.video_tick = overlay_tick,
	.video_render = overlay_render,
	.show = overlay_show,
	.hide = overlay_hide,
};

bool obs_module_load(void)
{
	obs_register_source(&overlay_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
