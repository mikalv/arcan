/*
 * Copyright 2017, Björn Ståhl
 * Description: CLI image file viewer
 * License: 3-Clause BSD, see COPYING file in arcan source repository
 * Reference: http://arcan-fe.com, README.MD
 */
#include <arcan_shmif.h>
#include <arcan_shmif_tuisym.h>
#include <unistd.h>
#include <getopt.h>
#include <stdarg.h>
#include "imgload.h"

static void progress_report(float progress);
#define STBIR_PROGRESS_REPORT(val) progress_report(val)
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize.h"

int image_size_limit_mb = 64;
bool disable_syscall_flt = false;
static struct draw_state* last_ds;

struct draw_state {
	shmif_pixel pad_col;
	struct arcan_shmif_cont* con;
	bool stdin_pending, loaded;
	struct img_state* playlist;
	struct img_state* cur;
	int pl_ind, pl_size;
	int timeout;
	int wnd_lim, wnd_pending, wnd_act;
	int wnd_prev, wnd_next;
	int step_timer, init_timer;
	int out_w, out_h;
	bool non_interactive;
	bool aspect_ratio;
	bool source_size;
	bool loop;
};

void debug_message(const char* msg, ...)
{
#ifdef DEBUG
	va_list args;
	va_start( args, msg );
		vfprintf(stderr,  msg, args );
	va_end( args);
#endif
}

/*
 * caller guarantee:
 * h-y > 0 && w-x > 0
 * state->w <= out_w && state->h <= out_h
 */
static void blit(struct arcan_shmif_cont* dst,
	const struct img_data* const src, const struct draw_state* const state)
{
/* draw pad color if the active image is incorrect */
	if (!src || !src->ready){
		for (int row = 0; row < dst->h; row++)
			for (int col = 0; col < dst->h; col++)
				dst->vidp[row * dst->pitch + col] = state->pad_col;
		arcan_shmif_signal(dst, SHMIF_SIGVID);
		return;
	}

/* scale to fit or something more complex? */
	int dw = state->out_w ?
		(dst->w > state->out_h ? state->out_h : dst->h) : dst->w;
	int dh = state->out_h ?
		(dst->h > state->out_h ? state->out_h : dst->h) : dst->h;

/* sanity check */
	if (src->buf_sz != src->w * src->h * 4)
		return;

	int pad_w = dst->w - dw;
	int pad_h = dst->h - dh;
	int src_stride = src->w * 4;

/* early out, no transform */
	if (dst->w == src->w && dst->h == src->h && !src->x && !src->y){
		debug_message("full-blit[%d*%d]\n", (int)dst->w, (int)dst->h);
		for (size_t row = 0; row < dst->h; row++)
			memcpy(&dst->vidp[row*dst->pitch],
				&src->buf[row*src_stride], src_stride);
		goto done;
	}

/* stretch-blit for zoom in/out or pan */
	debug_message("blit[%d+%d*%d+%d] -> [%d,%d]:pad(%d,%d)\n",
		(int)src->w, (int)src->x, (int)src->h, (int)src->y,
		(int)dw, (int)dh, pad_w, pad_h);
	stbir_resize_uint8(
		&src->buf[src->y * src_stride + src->x],
		src->w - src->x, src->h - src->y,
		src_stride, dst->vidb, dw, dh, dst->stride, sizeof(shmif_pixel)
	);

/* pad with color */
	for (int y = 0; y < dh; y++){
		shmif_pixel* vidp = dst->vidp + y * dst->pitch;
		for (int x = dw; x < dst->w; x++)
			vidp[x] = 0;
	}

/* pad last few rows */
	for (int y = dh; y < dst->h; y++){
		shmif_pixel* vidp = dst->vidp + y * dst->pitch;
		for (int x = 0; x < dst->w; x++)
			*vidp++ = state->pad_col;
	}

done:
	arcan_shmif_signal(dst, SHMIF_SIGVID | SHMIF_SIGBLK_NONE);
}

static void set_ident(struct arcan_shmif_cont* out,
	const char* prefix, const char* str)
{
	struct arcan_event ev = {.ext.kind = ARCAN_EVENT(IDENT)};
	size_t lim = sizeof(ev.ext.message.data)/sizeof(ev.ext.message.data[1]);
	size_t len = strlen(str);

	if (len > lim)
		str += len - lim - 1;

	snprintf((char*)ev.ext.message.data, lim, "%s%s", prefix, str);
	debug_message("new ident: %s%s\n", prefix, str);
	arcan_shmif_enqueue(out, &ev);
}

static void progress_report(float state)
{
	static float last_state;
	if (state < 1.0){
		char msgbuf[16];
		if (state - last_state > 0.1){
			last_state = state;
			snprintf(msgbuf, 16, "resizing(%.2d%%) ", (int)(state*100));
			set_ident(last_ds->con, msgbuf, last_ds->cur->fname);
		}
	}
	else {
		last_state = 0.0;
		set_ident(last_ds->con, "", last_ds->cur->fname);
	}
}

/* sweep O(n) slots for pending loads as we want to reel them in immediately
 * to keep the active number of zombies down and shrink our own memory alloc */
static void poll_pl(struct draw_state* ds, int step)
{
	for (size_t i = 0; i < ds->pl_size && ds->wnd_pending; i++)
	{
		if (ds->playlist[i].out && ds->playlist[i].proc){
			if (imgload_poll(&ds->playlist[i])){
				if (ds->playlist[i].is_stdin)
					ds->stdin_pending = false;
				ds->playlist[i].life = 0;
				ds->wnd_pending--;
			}
/* tick the timeout timer (if one has been set) and kill the worker if it
 * takes too long */
			else if (step && ds->playlist[i].life > 0){
				ds->playlist[i].life--;
				if (!ds->playlist[i].life && !ds->playlist[i].out->ready){
					fprintf(stderr, "worker (%s) timed out\n", ds->playlist[i].fname);
					imgload_reset(&ds->playlist[i]);
					ds->playlist[i].life = -1;
				}
			}
		}
	}
}

/* spawn a new worker if:
 *  1. one isn't active on the slot
 *  2. slot isn't stdin or slot is stdin but there's no stdin- working
 *  3. slot hasn't timed out before
 */
static bool try_dispatch(struct draw_state* ds, int ind)
{
	if ((!ds->playlist[ind].out && ds->playlist[ind].life >= 0) ||
		(!ds->playlist[ind].out->ready && ds->playlist[ind].proc)){

		if (ds->playlist[ind].is_stdin && ds->stdin_pending)
		return false;

		if (imgload_spawn(ds->con, &ds->playlist[ind])){
			if (ds->playlist[ind].is_stdin)
				ds->stdin_pending = true;
			ds->wnd_pending++;
			ds->playlist[ind].life = ds->timeout;
			debug_message("queued %s[%d], pending: %d\n",
				ds->playlist[ind].fname,  ind, ds->wnd_pending);
			return true;
		}
	}

	return false;
}

static struct img_state* set_playlist_pos(struct draw_state* ds, int i)
{
	poll_pl(ds, 0);

/* range-check, if we don't loop, return to start */
	if (i < 0)
		i = ds->pl_size-1;

	if (i >= ds->pl_size){
		if (ds->loop)
			i = 0;
		else
			return NULL;
	}

/* FIXME: we should drop 'n' outdated slots */

/* fill up new worker slots, but ONLY if the current index has been loaded
 * to prevent the queue from stalling the next desired item */
	ds->pl_ind = i;
	do {
		try_dispatch(ds, i);
		i = (i + 1) % ds->pl_size;
	} while (ds->playlist[ds->pl_ind].out &&
		ds->wnd_pending < ds->wnd_lim && i != ds->pl_ind);

	return &ds->playlist[ds->pl_ind];
}

struct lent {
	const char* lbl;
	const char* descr;
	const char* def;
	int defsym;
	bool(*ptr)(struct draw_state*);
};

static void set_active(struct draw_state* ds)
{
	if (!ds || !ds->cur){
		set_ident(ds->con, "missing playlist item", "");
		return;
	}

	if (ds->cur->proc){
		ds->loaded = false;
		return;
	}

	ds->loaded = true;
	if (ds->cur->broken){
		set_ident(ds->con, (char*)ds->cur->msg, ds->cur->fname);
	}
/* Source buffer determines window size. A caveat with this approach is
 * that though the maxw/maxh may fit, there might not be enough permitted
 * memory service side. In those cases, /2 the dimensions until a resize
 * is accepted */
	else {
		if (ds->source_size){
			size_t dw = ds->cur->out->w;
			size_t dh = ds->cur->out->h;

			while (dh && dh && !arcan_shmif_resize(ds->con, dw, dh)){
				debug_message("resize to %zu*%zu rejected, trying %zu*%zu\n",
					dw, dh, dw >> 1, dh >> 1);
				dw >>= 1;
				dh >>= 1;
			}
			debug_message("resized window to %zu*%zu\n", dw, dh);
		}
		set_ident(ds->con, "", ds->cur->fname);
		blit(ds->con, (struct img_data*) ds->cur->out, ds);
	}
}

static bool step_next(struct draw_state* state)
{
	state->cur = set_playlist_pos(state, state->pl_ind + 1);
	return true;
}

static bool step_prev(struct draw_state* state)
{
	state->cur = set_playlist_pos(state, state->pl_ind - 1);
	return true;
}

static bool source_size(struct draw_state* state)
{
	if (!state->source_size){
		state->source_size = true;
	}
	return false;
}

static bool server_size(struct draw_state* state)
{
	if (state->source_size){
		state->source_size = false;
		return arcan_shmif_resize(state->con, state->out_w, state->out_h);
	}
	else
		return false;
}

static bool zoom_out(struct draw_state* state)
{
	return true;
}

static bool zoom_in(struct draw_state* state)
{
	return true;
}

static bool pl_toggle(struct draw_state* state)
{
	return false;
}

static bool aspect_ratio(struct draw_state* state)
{
	state->aspect_ratio = !state->aspect_ratio;
	return true;
}

static const struct lent labels[] = {
	{"PREV", "Step to previous entry in playlist", "LEFT", TUIK_H, step_prev},
	{"NEXT", "Step to next entry in playlist", "RIGHT", TUIK_L, step_next},
	{"PL_TOGGLE", "Toggle playlist stepping on/off", "SPACE", TUIK_SPACE, pl_toggle},
	{"SOURCE_SIZE", "Resize the window to fit image size", "Z", TUIK_F5, source_size},
	{"SERVER_SIZE", "Use the recommended connection size", "M", TUIK_F6, server_size},
	{"ASPECT_TOGGLE", "Maintain aspect ratio", "A", TUIK_TAB, aspect_ratio},
	{"ZOOM_IN", "Increment the scale factor (integer)", "+", TUIK_F1, zoom_in},
	{"ZOOM_OUT", "Decrement the scale factor (integer)", "-", TUIK_F2, zoom_out},
	{NULL, NULL}
};

static const struct lent* find_label(const char* label)
{
	const struct lent* cur = labels;
	while(cur->ptr){
		if (strcmp(label, cur->lbl) == 0)
			return cur;
		cur++;
	}
	return NULL;
}

static const struct lent* find_sym(unsigned sym)
{
	const struct lent* cur = labels;
	if (sym == 0)
		return NULL;

	while(cur->ptr){
		if (cur->defsym == sym)
			return cur;
		cur++;
	}

	return NULL;
}

static bool dispatch_event(
	struct arcan_shmif_cont* con, struct arcan_event* ev, struct draw_state* ds)
{
	if (ev->category == EVENT_IO){
		if (ds->non_interactive)
			return false;

/* drag/zoom/pan/... */
		if (ev->io.devkind == EVENT_IDEVKIND_MOUSE){
			return false;
		}
		else if (ev->io.datatype == EVENT_IDATATYPE_DIGITAL){
			if (!ev->io.input.translated.active || !ev->io.label[0])
				return false;

			const struct lent* lent = find_label(ev->io.label);
			if (lent)
				return lent->ptr(ds);
		}
		else if (ev->io.datatype == EVENT_IDATATYPE_TRANSLATED){
			if (!ev->io.input.translated.active)
				return false;
			const struct lent* lent = ev->io.label[0] ?
				find_label(ev->io.label) : find_sym(ev->io.input.translated.keysym);
			if (lent)
				return lent->ptr(ds);
		}
	}
	else if (ev->category == EVENT_TARGET)
		switch(ev->tgt.kind){
		case TARGET_COMMAND_DISPLAYHINT:
			if (ev->tgt.ioevs[0].iv && ev->tgt.ioevs[1].iv){
				if (!ds->source_size)
					return arcan_shmif_resize(con,
						ev->tgt.ioevs[0].iv, ev->tgt.ioevs[1].iv);
				}
		break;
		case TARGET_COMMAND_STEPFRAME:
			if (ev->tgt.ioevs[1].iv == 0xfeed)
				poll_pl(ds, 1);

			if (ds->step_timer > 0){
				if (ds->step_timer == 0){
					ds->step_timer = ds->init_timer;
					ds->cur = set_playlist_pos(ds, ds->pl_ind + 1);
				}
			}
		break;
		case TARGET_COMMAND_EXIT:
			arcan_shmif_drop(con);
			return false;
		break;
		default:
		break;
		}
	return false;
}

static int show_use(const char* msg)
{
	printf("Usage: aloadimage [options] file1 .. filen\n"
"-h    \t--help        \tthis text\n"
"-l    \t--loop        \tStep back to file1 after reaching filen in playlist\n"
"-m num\t--limit-mem   \tSet loader process memory limit to [num] MB\n"
"-b    \t--block-input \tIgnore keyboard and mouse input\n"
"-r num\t--readahead   \tSet the upper playlist preload limit\n"
"-t sec\t--step-time   \tSet playlist step time (~seconds)\n"
"-T sec\t--timeout     \tSet worker kill- timeout\n"
"-a    \t--aspect      \tMaintain aspect ratio when scaling\n"
#ifdef ENABLE_SECCOMP
"-X    \t--no-sysflt   \tDisable seccomp- syscall filtering\n"
#endif
"-S    \t--server-size \tScale to fit server- suggested window size\n"
"-d str\t--display     \tSet/override the display server connection path\n");
	return EXIT_FAILURE;
}

static void expose_labels(struct arcan_shmif_cont* con)
{
	const struct lent* cur = labels;
	while(cur->lbl){
		arcan_event ev = {
			.category = EVENT_EXTERNAL,
			.ext.kind = ARCAN_EVENT(LABELHINT),
			.ext.labelhint.idatatype = EVENT_IDATATYPE_DIGITAL
		};
			snprintf(ev.ext.labelhint.label,
			sizeof(ev.ext.labelhint.label)/sizeof(ev.ext.labelhint.label[0]),
			"%s", cur->lbl
		);
		snprintf(ev.ext.labelhint.initial,
			sizeof(ev.ext.labelhint.initial)/sizeof(ev.ext.labelhint.initial[0]),
			"%s", cur->def
		);
		cur++;
		arcan_shmif_enqueue(con, &ev);
	}
}

static const struct option longopts[] = {
	{"help", no_argument, NULL, 'h'},
	{"loop", no_argument, NULL, 'l'},
	{"step-time", required_argument, NULL, 't'},
	{"block-input", no_argument, NULL, 'b'},
	{"timeout", required_argument, NULL, 'T'},
	{"limit-mem", required_argument, NULL, 'm'},
	{"readahead", required_argument, NULL, 'r'},
	{"no-sysflt", no_argument, NULL, 'X'},
	{"server-size", no_argument, NULL, 'S'},
	{"display", no_argument, NULL, 'd'},
	{"aspect", no_argument, NULL, 'a'}
};

int main(int argc, char** argv)
{
	if (argc <= 1)
		return show_use("invalid/missing arguments");

	struct img_state playlist[argc];
	struct draw_state ds = {
		.source_size = true,
		.wnd_lim = 5,
		.init_timer = 0,
		.pad_col = SHMIF_RGBA(32, 32, 32, 255),
		.playlist = playlist
	};
	last_ds = &ds;

	int ch;
	while((ch = getopt_long(argc, argv,
		"ht:bd:T:m:r:XS", longopts, NULL)) >= 0)
		switch(ch){
		case 'h' : return show_use(""); break;
		case 't' : ds.init_timer = strtoul(optarg, NULL, 10) * 5; break;
		case 'b' : ds.non_interactive = true; break;
		case 'd' : setenv("ARCAN_CONNPATH", optarg, 10); break;
		case 'T' : ds.timeout = strtoul(optarg, NULL, 10) * 5; break;
		case 'l' : ds.loop = true; break;
		case 'a' : ds.aspect_ratio = true; break;
		case 'm' : image_size_limit_mb = strtoul(optarg, NULL, 10); break;
		case 'r' : ds.wnd_lim = strtoul(optarg, NULL, 10); break;
		case 'X' : disable_syscall_flt = true; break;
		case 'S' : ds.source_size = false; break;
		default:
			fprintf(stderr, "unknown/ignored option: %c\n", ch);
		break;
		}
	ds.step_timer = ds.init_timer;

/* parse opts and update ds accordingly */
	for (int i = optind; i < argc; i++){
		playlist[ds.pl_size] = (struct img_state){
			.fname = argv[i],
			.is_stdin = strcmp(argv[i], "-") == 0
		};
		ds.pl_size++;
	}
	if (ds.pl_size == 0)
		return show_use("no images found");

/* dispatch workers */
	ds.cur = set_playlist_pos(&ds, 0);

/* connect while the workers are busy */
	struct arcan_shmif_cont cont = arcan_shmif_open(
		SEGID_MEDIA, SHMIF_ACQUIRE_FATALFAIL, NULL);
	ds.con = &cont;
	blit(&cont, NULL, &ds);

/* 200ms timer for automatic stepping and load/poll, if this value is
 * changed, do trhe same for the multipliers to init_timer and timeout */
	arcan_shmif_enqueue(&cont, &(struct arcan_event){
		.ext.kind = ARCAN_EVENT(CLOCKREQ),
		.ext.clock.rate = 5,
		.ext.clock.id = 0xfeed
	});

	while (ds.cur && cont.addr){
		if (!ds.loaded){
			if (imgload_poll(ds.cur)){
				ds.loaded = true;
				debug_message("loaded: %s, pending: %d/%d\n",
					ds.cur->fname, (int)ds.wnd_pending, (int)ds.wnd_lim);
				set_active(&ds);
			}
			else
				set_ident(&cont, "loading: ", ds.cur->fname);
		}
		poll_pl(&ds, 0);

/* Block for one event, then flush out any burst. Blit on any change */
		arcan_event ev;
		if (!arcan_shmif_wait(&cont, &ev))
			break;
		bool dirty = dispatch_event(&cont, &ev, &ds);
		while(arcan_shmif_poll(&cont, &ev) > 0)
			dirty |= dispatch_event(&cont, &ev, &ds);
		poll_pl(&ds, 0);

		if (dirty && ds.cur && ds.cur->out && ds.cur->out->ready){
			blit(&cont, (struct img_data*) ds.cur->out, &ds);
		}
	}
	return EXIT_SUCCESS;
}
