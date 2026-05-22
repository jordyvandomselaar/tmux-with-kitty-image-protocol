/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

static struct images	all_images = TAILQ_HEAD_INITIALIZER(all_images);
static u_int		all_images_count;
#define MAX_IMAGE_COUNT 20
#ifdef ENABLE_KITTY_IMAGES
#define MAX_TOTAL_IMAGE_COUNT (MAX_IMAGE_COUNT * MAX_IMAGE_COUNT)
#define MAX_KITTY_IMAGE_BYTES ((size_t)MAX_IMAGE_COUNT * INPUT_BUF_DEFAULT_SIZE)
#define MAX_KITTY_TOTAL_IMAGE_BYTES \
	((size_t)MAX_TOTAL_IMAGE_COUNT * INPUT_BUF_DEFAULT_SIZE)
#else
#define MAX_TOTAL_IMAGE_COUNT MAX_IMAGE_COUNT
#endif

#ifdef ENABLE_KITTY_IMAGES
static u_int		next_kitty_public_image_id = 0x80000000U;
static u_int		next_kitty_image_id = 0x80000000U;
static u_int		next_kitty_placement_id = 0x80000000U;
static u_int		kitty_images_generation;
static size_t		kitty_images_bytes;
#endif

static void		 image_fallback(char **, enum image_type, u_int, u_int);

static void printflike(3, 4)
image_log(struct image *im, const char* from, const char* fmt, ...)
{
	va_list	ap;
	char	s[128];

	if (log_get_level() == 0)
		return;

	if (fmt == NULL) {
		log_debug("%s: %p (%ux%u %u,%u)", from, im, im->sx, im->sy,
		    im->px, im->py);
		return;
	}

	va_start(ap, fmt);
	vsnprintf(s, sizeof s, fmt, ap);
	va_end(ap);

	log_debug("%s: %p (%ux%u %u,%u): %s", from, im, im->sx, im->sy,
	    im->px, im->py, s);
}

#ifdef ENABLE_KITTY_IMAGES
static void
image_redraw_if_visible(struct image *im)
{
	struct window_pane	*wp;

	if (im->type != IMAGE_KITTY || im->hidden)
		return;
	if (im->s == NULL || im->images != &im->s->images)
		return;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->screen == im->s) {
			server_redraw_window(wp->window);
			return;
		}
	}
}
#endif

static void
image_free(struct image *im)
{
	image_log(im, __func__, NULL);

#ifdef ENABLE_KITTY_IMAGES
	if (im->type == IMAGE_KITTY && !im->hidden) {
		image_redraw_if_visible(im);
		if (++kitty_images_generation == 0)
			kitty_images_generation = 1;
	}
	if (im->type == IMAGE_KITTY) {
		if (kitty_images_bytes >= kitty_size_in_bytes(im->data.kitty))
			kitty_images_bytes -= kitty_size_in_bytes(im->data.kitty);
		else
			kitty_images_bytes = 0;
	}
#endif

	TAILQ_REMOVE(&all_images, im, all_entry);
	all_images_count--;

	TAILQ_REMOVE(im->images, im, entry);

	switch (im->type) {
#ifdef ENABLE_SIXEL
	case IMAGE_SIXEL:
		sixel_free(im->data.sixel);
		break;
#endif
#ifdef ENABLE_KITTY_IMAGES
	case IMAGE_KITTY:
		kitty_free(im->data.kitty);
		break;
#endif
	default:
		break;
	}

	free(im->fallback);
	free(im);
}

#ifdef ENABLE_KITTY_IMAGES
static u_int
image_next_kitty_id(u_int *next)
{
	u_int	id;

	id = (*next)++;
	if (id == 0) {
		id = 0x80000000U;
		*next = id + 1;
	}
	return (id);
}

static int
image_contains_kitty_cell(struct image *im, u_int px, u_int py)
{
	if (im->hidden)
		return (0);
	return (px >= im->px && px < im->px + im->sx &&
	    py >= im->py && py < im->py + im->sy);
}

static struct image *
image_find_kitty_source(struct screen *s, struct kitty_image *ki)
{
	struct image		*im;
	struct kitty_image	*existing;
	u_int			 image_id, image_num;

	image_id = kitty_get_image_id(ki);
	image_num = kitty_get_image_num(ki);
	if (image_id == 0 && image_num == 0)
		return (NULL);

	TAILQ_FOREACH(im, &s->images, entry) {
		if (im->type != IMAGE_KITTY)
			continue;
		existing = im->data.kitty;
		if (kitty_get_action(existing) != 'T' &&
		    kitty_get_action(existing) != 't')
			continue;
		if (image_id != 0 && kitty_get_image_id(existing) == image_id)
			return (im);
		if (image_num != 0 && kitty_get_image_num(existing) == image_num)
			return (im);
	}
	return (NULL);
}

static int
image_kitty_image_id_in_use1(struct images *images, u_int image_id)
{
	struct image		*im;
	struct kitty_image	*existing;

	TAILQ_FOREACH(im, images, entry) {
		if (im->type != IMAGE_KITTY)
			continue;
		existing = im->data.kitty;
		if (kitty_get_image_id(existing) == image_id)
			return (1);
	}
	return (0);
}

static int
image_kitty_image_id_in_use(struct screen *s, u_int image_id)
{
	if (image_id == 0)
		return (1);
	return (image_kitty_image_id_in_use1(&s->images, image_id) ||
	    image_kitty_image_id_in_use1(&s->saved_images, image_id));
}

static void
image_assign_kitty_public_id(struct screen *s, struct kitty_image *ki)
{
	u_int	id;

	if (kitty_get_image_id(ki) != 0 || kitty_get_image_num(ki) == 0)
		return;

	do {
		id = image_next_kitty_id(&next_kitty_public_image_id);
	} while (image_kitty_image_id_in_use(s, id));
	kitty_set_image_id(ki, id);
}

int
image_kitty_has_source(struct screen *s, struct kitty_image *ki)
{
	return (image_find_kitty_source(s, ki) != NULL);
}

u_int
image_kitty_generation(void)
{
	return (kitty_images_generation);
}

static int
image_is_kitty_source_referenced(struct image *im)
{
	struct image		*other;
	u_int			 image_id;

	if (im->type != IMAGE_KITTY)
		return (0);
	if (kitty_get_action(im->data.kitty) != 'T' &&
	    kitty_get_action(im->data.kitty) != 't')
		return (0);
	image_id = kitty_get_terminal_image_id(im->data.kitty);
	if (image_id == 0)
		return (0);

	TAILQ_FOREACH(other, im->images, entry) {
		if (other == im || other->type != IMAGE_KITTY)
			continue;
		if (kitty_get_terminal_image_id(other->data.kitty) == image_id &&
		    kitty_get_action(other->data.kitty) == 'p')
			return (1);
	}
	return (0);
}

static int
image_free_oldest_from(struct images *images)
{
	struct image	*im;

	TAILQ_FOREACH(im, images, entry) {
		if (!image_is_kitty_source_referenced(im)) {
			image_free(im);
			return (1);
		}
	}
	if (TAILQ_EMPTY(images))
		return (0);
	image_free(TAILQ_FIRST(images));
	return (1);
}

static int
image_free_oldest_from_screen(struct screen *s)
{
	if (image_free_oldest_from(&s->images))
		return (1);
	return (image_free_oldest_from(&s->saved_images));
}

static int
image_free_oldest(void)
{
	struct image	*im;

	TAILQ_FOREACH(im, &all_images, all_entry) {
		if (!image_is_kitty_source_referenced(im)) {
			image_free(im);
			return (1);
		}
	}
	if (TAILQ_EMPTY(&all_images))
		return (0);
	image_free(TAILQ_FIRST(&all_images));
	return (1);
}

static size_t
image_kitty_bytes_from(struct images *images)
{
	struct image	*im;
	size_t		 bytes = 0;

	TAILQ_FOREACH(im, images, entry) {
		if (im->type == IMAGE_KITTY)
			bytes += kitty_size_in_bytes(im->data.kitty);
	}
	return (bytes);
}

static void
image_prepare_kitty(struct screen *s, struct kitty_image *ki, int hidden)
{
	struct image	*upload;
	char		 action;
	u_int		 image_id;

	action = kitty_get_action(ki);
	if (action == 'T' || action == 't')
		image_assign_kitty_public_id(s, ki);

	if (kitty_get_terminal_image_id(ki) == 0) {
		if (action == 'p') {
			upload = image_find_kitty_source(s, ki);
			if (upload != NULL) {
				image_id =
				    kitty_get_terminal_image_id(upload->data.kitty);
				kitty_set_terminal_image_id(ki, image_id);
				kitty_copy_source_metadata(ki, upload->data.kitty);
			}
		} else if (action == 'T' || action == 't')
			kitty_set_terminal_image_id(ki,
			    image_next_kitty_id(&next_kitty_image_id));
	}

	if (!hidden && (action == 'T' || action == 'p') &&
	    kitty_get_terminal_placement_id(ki) == 0) {
		kitty_set_terminal_placement_id(ki,
		    image_next_kitty_id(&next_kitty_placement_id));
	}
}

static int
image_kitty_make_room(struct screen *s, struct kitty_image *ki)
{
	size_t	bytes, screen_bytes;

	bytes = kitty_size_in_bytes(ki);
	if (bytes > MAX_KITTY_IMAGE_BYTES)
		return (0);
	screen_bytes = image_kitty_bytes_from(&s->images) +
	    image_kitty_bytes_from(&s->saved_images);
	while (screen_bytes + bytes > MAX_KITTY_IMAGE_BYTES) {
		if (!image_free_oldest_from_screen(s))
			return (0);
		screen_bytes = image_kitty_bytes_from(&s->images) +
		    image_kitty_bytes_from(&s->saved_images);
	}
	while (kitty_images_bytes + bytes > MAX_KITTY_TOTAL_IMAGE_BYTES) {
		if (image_free_oldest_from_screen(s))
			continue;
		if (!image_free_oldest())
			return (0);
	}
	return (1);
}

static int
image_match_kitty(struct image *im, struct kitty_image *ki, char what)
{
	struct kitty_image	*existing;
	u_int			 image_id, image_num, placement_id, x, y;

	if (im->type != IMAGE_KITTY)
		return (0);
	existing = im->data.kitty;

	image_id = kitty_get_image_id(ki);
	image_num = kitty_get_image_num(ki);
	placement_id = kitty_get_placement_id(ki);
	x = kitty_get_delete_x(ki);
	y = kitty_get_delete_y(ki);

	if (im->hidden &&
	    (what == '\0' || strchr("aincpqrxyz", what) != NULL))
		return (0);

	switch (what) {
	case 'a':
		return (!im->hidden);
	case 'A':
		return (1);
	case 'i':
	case 'I':
		if (image_id == 0 || kitty_get_image_id(existing) != image_id)
			return (0);
		if (placement_id != 0)
			return (kitty_get_placement_id(existing) == placement_id);
		return (1);
	case 'n':
	case 'N':
		if (image_num == 0 || kitty_get_image_num(existing) != image_num)
			return (0);
		if (placement_id != 0)
			return (kitty_get_placement_id(existing) == placement_id);
		return (1);
	case 'c':
	case 'C':
		return (image_contains_kitty_cell(im, im->s->cx, im->s->cy));
	case 'f':
	case 'F':
		return (0);
	case 'p':
	case 'P':
		if (x == 0 || y == 0)
			return (0);
		return (image_contains_kitty_cell(im, x - 1, y - 1));
	case 'q':
	case 'Q':
		if (x == 0 || y == 0)
			return (0);
		return (kitty_get_z_index(existing) == kitty_get_z_index(ki) &&
		    image_contains_kitty_cell(im, x - 1, y - 1));
	case 'x':
	case 'X':
		if (x == 0 || im->hidden)
			return (0);
		return (x - 1 >= im->px && x - 1 < im->px + im->sx);
	case 'y':
	case 'Y':
		if (y == 0 || im->hidden)
			return (0);
		return (y - 1 >= im->py && y - 1 < im->py + im->sy);
	case 'z':
	case 'Z':
		return (!im->hidden &&
		    kitty_get_z_index(existing) == kitty_get_z_index(ki));
	case 'r':
	case 'R':
		if (x == 0 || y == 0 || x > y)
			return (0);
		image_id = kitty_get_image_id(existing);
		return (image_id >= x && image_id <= y);
	default:
		if (placement_id != 0 && image_id != 0)
			return (kitty_get_image_id(existing) == image_id &&
			    kitty_get_placement_id(existing) == placement_id);
		if (placement_id != 0 && image_num != 0)
			return (kitty_get_image_num(existing) == image_num &&
			    kitty_get_placement_id(existing) == placement_id);
		if (placement_id != 0)
			return (kitty_get_placement_id(existing) == placement_id);
		if (image_id != 0 && kitty_get_placement_id(existing) == 0 &&
		    kitty_get_terminal_placement_id(existing) == 0)
			return (kitty_get_image_id(existing) == image_id);
		if (image_num != 0 && kitty_get_placement_id(existing) == 0 &&
		    kitty_get_terminal_placement_id(existing) == 0)
			return (kitty_get_image_num(existing) == image_num);
		return (0);
	}
}

static int
image_remove_kitty(struct screen *s, struct kitty_image *ki, char what)
{
	struct image	*im, *im1;
	int		 redraw = 0;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (image_match_kitty(im, ki, what)) {
			image_free(im);
			redraw = 1;
		}
	}
	return (redraw);
}

int
image_kitty_delete(struct screen *s, struct kitty_image *ki)
{
	char	what;

	if (s == NULL || ki == NULL)
		return (0);

	what = kitty_get_delete_what(ki);
	if (what == '\0')
		what = 'a';
	if (strchr("aAiInNcCfFpPqQxXyYzZrR", what) == NULL)
		return (-1);
	return (image_remove_kitty(s, ki, what));
}

static int
image_kitty_replace(struct screen *s, struct kitty_image *ki)
{
	struct image		*im, *im1;
	struct kitty_image	*existing;
	char	 action;
	u_int	 image_id, image_num;
	int	 redraw = 0;

	if (s == NULL || ki == NULL)
		return (0);

	action = kitty_get_action(ki);
	if (action == 'T' || action == 't') {
		image_id = kitty_get_image_id(ki);
		image_num = kitty_get_image_num(ki);
		if (image_id != 0 || image_num != 0) {
			TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
				if (im->type != IMAGE_KITTY)
					continue;
				existing = im->data.kitty;
				if ((image_id != 0 &&
				    kitty_get_image_id(existing) == image_id) ||
				    (image_num != 0 &&
				    kitty_get_image_num(existing) == image_num)) {
					if (!im->hidden)
						redraw = 1;
					image_free(im);
				}
			}
			return (redraw);
		}
	}
	return (image_remove_kitty(s, ki, '\0'));
}
#endif

static u_int
image_count_from(struct images *images)
{
	struct image	*im;
	u_int		 count = 0;

	TAILQ_FOREACH(im, images, entry)
		count++;
	return (count);
}

static int
image_free_all1(struct images *images)
{
	struct image	*im, *im1;
	int		 redraw = !TAILQ_EMPTY(images);

	if (redraw)
		log_debug ("%s", __func__);
	TAILQ_FOREACH_SAFE(im, images, entry, im1)
		image_free(im);
	return (redraw);
}

int
image_free_all(struct screen *s)
{
	return (image_free_all1(&s->images));
}

int
image_resize(struct screen *s)
{
	struct image	*im, *im1;
	int		 redraw = 0;
#ifdef ENABLE_KITTY_IMAGES
	u_int		 sx = screen_size_x(s), sy = screen_size_y(s), nx, ny;
#endif

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
#ifdef ENABLE_KITTY_IMAGES
		if (im->type == IMAGE_KITTY) {
			if (im->hidden)
				continue;
			if (im->px >= sx || im->py >= sy) {
				image_free(im);
				redraw = 1;
				continue;
			}
			nx = im->sx;
			if (nx > sx - im->px)
				nx = sx - im->px;
			ny = im->sy;
			if (ny > sy - im->py)
				ny = sy - im->py;
			if (nx == 0 || ny == 0) {
				image_free(im);
				redraw = 1;
				continue;
			}
			if (nx != im->sx || ny != im->sy) {
				im->sx = nx;
				im->sy = ny;
				free(im->fallback);
				image_fallback(&im->fallback, im->type, im->sx,
				    im->sy);
				redraw = 1;
			}
			continue;
		}
#endif
		image_free(im);
		redraw = 1;
	}
	return (redraw);
}

int
image_free_all_saved(struct screen *s)
{
	return (image_free_all1(&s->saved_images));
}

void
image_reparent_all(struct images *images)
{
	struct image	*im;

	TAILQ_FOREACH(im, images, entry)
		im->images = images;
}

/* Create text placeholder for an image. */
static void
image_fallback(char **ret, enum image_type type, u_int sx, u_int sy)
{
	char	*buf, *label;
	u_int	 py, size, lsize;
	const char *type_name;

	switch (type) {
#ifdef ENABLE_SIXEL
	case IMAGE_SIXEL:
		type_name = "SIXEL";
		break;
#endif
#ifdef ENABLE_KITTY_IMAGES
	case IMAGE_KITTY:
		type_name = "KITTY";
		break;
#endif
	default:
		type_name = "UNKNOWN";
		break;
	}

	/* Allocate first line. */
	lsize = xasprintf(&label, "%s IMAGE (%ux%u)\r\n", type_name, sx, sy) + 1;
	if (sx < lsize - 3)
		size = lsize - 1;
	else
		size = sx + 2;

	/* Remaining lines. Every placeholder line has \r\n at the end. */
	size += (sx + 2) * (sy - 1) + 1;
	*ret = buf = xmalloc(size);

	/* Render first line. */
	if (sx < lsize - 3) {
		memcpy(buf, label, lsize);
		buf += lsize - 1;
	} else {
		memcpy(buf, label, lsize - 3);
		buf += lsize - 3;
		memset(buf, '+', sx - lsize + 3);
		buf += sx - lsize + 3;
		snprintf(buf, 3, "\r\n");
		buf += 2;
	}

	/* Remaining lines. */
	for (py = 1; py < sy; py++) {
		memset(buf, '+', sx);
		buf += sx;
		snprintf(buf, 3, "\r\n");
		buf += 2;
	}

	free(label);
}

static struct image *
image_store1(struct screen *s, enum image_type type, void *data, int hidden)
{
	struct image	*im;

	im = xcalloc(1, sizeof *im);

	im->type = type;
	im->s = s;
	im->images = &s->images;
	im->hidden = hidden;

	im->px = s->cx;
	im->py = s->cy;

	switch (type) {
#ifdef ENABLE_SIXEL
	case IMAGE_SIXEL:
		im->data.sixel = data;
		if (!hidden)
			sixel_size_in_cells(im->data.sixel, &im->sx, &im->sy);
		break;
#endif
#ifdef ENABLE_KITTY_IMAGES
	case IMAGE_KITTY:
		image_prepare_kitty(s, data, hidden);
		if (!image_kitty_make_room(s, data)) {
			free(im);
			return (NULL);
		}
		image_kitty_replace(s, data);
		im->data.kitty = data;
		if (!hidden)
			kitty_size_in_cells(im->data.kitty, &im->sx, &im->sy);
		break;
#endif
	default:
		break;
	}

	if (!hidden)
		image_fallback(&im->fallback, type, im->sx, im->sy);

	while (image_count_from(&s->images) + image_count_from(&s->saved_images) +
	    1 >= MAX_IMAGE_COUNT) {
#ifdef ENABLE_KITTY_IMAGES
		if (!image_free_oldest_from_screen(s))
			break;
#else
		if (TAILQ_EMPTY(&s->images))
			break;
		image_free(TAILQ_FIRST(&s->images));
#endif
	}
	while (all_images_count + 1 >= MAX_TOTAL_IMAGE_COUNT &&
	    !TAILQ_EMPTY(&all_images)) {
#ifdef ENABLE_KITTY_IMAGES
		if (image_free_oldest_from_screen(s))
			continue;
		if (!image_free_oldest())
			break;
#else
		image_free(TAILQ_FIRST(&all_images));
#endif
	}

	image_log(im, __func__, NULL);
	TAILQ_INSERT_TAIL(&s->images, im, entry);

	TAILQ_INSERT_TAIL(&all_images, im, all_entry);
#ifdef ENABLE_KITTY_IMAGES
	if (type == IMAGE_KITTY)
		kitty_images_bytes += kitty_size_in_bytes(data);
#endif
	all_images_count++;

	return (im);
}

struct image*
image_store(struct screen *s, enum image_type type, void *data)
{
	return (image_store1(s, type, data, 0));
}

#ifdef ENABLE_KITTY_IMAGES
struct image*
image_store_kitty_upload(struct screen *s, struct kitty_image *ki)
{
	return (image_store1(s, IMAGE_KITTY, ki, 1));
}

int
image_kitty_insert_lines(struct screen *s, u_int py, u_int ny, u_int bottom)
{
	struct image	*im, *im1;
	uint64_t	 itop, ibottom, region_end, new_top, new_bottom, visible;
	int		 redraw = 0;

	if (ny == 0 || py > bottom)
		return (0);
	if (ny > bottom + 1 - py)
		ny = bottom + 1 - py;
	region_end = (uint64_t)bottom + 1;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->type != IMAGE_KITTY || im->hidden || im->sy == 0)
			continue;
		itop = im->py;
		ibottom = (uint64_t)im->py + im->sy;
		if (ibottom <= py || itop >= region_end)
			continue;
		image_log(im, __func__, "py=%u, ny=%u, bottom=%u", py, ny,
		    bottom);

		if (itop < py || ibottom > region_end) {
			image_free(im);
			redraw = 1;
			continue;
		}

		new_top = itop + ny;
		if (new_top >= region_end) {
			image_free(im);
			redraw = 1;
			continue;
		}
		new_bottom = ibottom + ny;
		if (new_bottom > region_end) {
			visible = region_end - new_top;
			if (visible == 0) {
				image_free(im);
				redraw = 1;
				continue;
			}
			im->py += ny;
			im->sy = visible;
			free(im->fallback);
			image_fallback(&im->fallback, im->type, im->sx, im->sy);
			redraw = 1;
			continue;
		}
		im->py += ny;
		redraw = 1;
	}
	return (redraw);
}

int
image_kitty_delete_lines(struct screen *s, u_int py, u_int ny, u_int bottom)
{
	struct image	*im, *im1;
	uint64_t	 itop, ibottom, delete_end, region_end, removed;
	int		 redraw = 0;

	if (ny == 0 || py > bottom)
		return (0);
	if (ny > bottom + 1 - py)
		ny = bottom + 1 - py;
	delete_end = (uint64_t)py + ny;
	region_end = (uint64_t)bottom + 1;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->type != IMAGE_KITTY || im->hidden || im->sy == 0)
			continue;
		itop = im->py;
		ibottom = (uint64_t)im->py + im->sy;
		if (ibottom <= py || itop >= region_end)
			continue;
		image_log(im, __func__, "py=%u, ny=%u, bottom=%u", py, ny,
		    bottom);

		if (ibottom > region_end || itop < py) {
			image_free(im);
			redraw = 1;
			continue;
		}
		if (itop < delete_end) {
			removed = delete_end - itop;
			if (removed >= im->sy) {
				image_free(im);
				redraw = 1;
				continue;
			}
			im->kitty_yoff += removed;
			im->sy -= removed;
			im->py = py;
			free(im->fallback);
			image_fallback(&im->fallback, im->type, im->sx, im->sy);
			redraw = 1;
			continue;
		}
		im->py -= ny;
		redraw = 1;
	}
	return (redraw);
}

int
image_kitty_scroll_down(struct screen *s, u_int lines)
{
	return (image_kitty_insert_lines(s, s->rupper, lines, s->rlower));
}
#endif

static int
image_text_obscure(struct image *im)
{
#ifdef ENABLE_KITTY_IMAGES
	if (im->type == IMAGE_KITTY) {
		if (im->fallback_hidden)
			return (0);
		im->fallback_hidden = 1;
		return (1);
	}
#endif
	image_free(im);
	return (1);
}

static int
image_check_line1(struct screen *s, u_int py, u_int ny, int text)
{
	struct image	*im, *im1;
	int		 redraw = 0, in;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->hidden)
			continue;
		in = (py + ny > im->py && py < im->py + im->sy);
		image_log(im, __func__, "py=%u, ny=%u, in=%d", py, ny, in);
		if (in) {
#ifdef ENABLE_KITTY_IMAGES
			if (im->type == IMAGE_KITTY && !text)
				continue;
#endif
			if (text) {
				if (image_text_obscure(im))
					redraw = 1;
			} else {
				image_free(im);
				redraw = 1;
			}
		}
	}
	return (redraw);
}

int
image_check_line(struct screen *s, u_int py, u_int ny)
{
	return (image_check_line1(s, py, ny, 0));
}

int
image_check_text_line(struct screen *s, u_int py, u_int ny)
{
	return (image_check_line1(s, py, ny, 1));
}

static int
image_check_area1(struct screen *s, u_int px, u_int py, u_int nx, u_int ny,
    int text)
{
	struct image	*im, *im1;
	int		 redraw = 0, in;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->hidden)
			continue;
		in = (py < im->py + im->sy &&
		    py + ny > im->py &&
		    px < im->px + im->sx &&
		    px + nx > im->px);
		image_log(im, __func__, "py=%u, ny=%u, in=%d", py, ny, in);
		if (in) {
#ifdef ENABLE_KITTY_IMAGES
			if (im->type == IMAGE_KITTY && !text)
				continue;
#endif
			if (text) {
				if (image_text_obscure(im))
					redraw = 1;
			} else {
				image_free(im);
				redraw = 1;
			}
		}
	}
	return (redraw);
}

int
image_check_area(struct screen *s, u_int px, u_int py, u_int nx, u_int ny)
{
	return (image_check_area1(s, px, py, nx, ny, 0));
}

int
image_check_text_area(struct screen *s, u_int px, u_int py, u_int nx, u_int ny)
{
	return (image_check_area1(s, px, py, nx, ny, 1));
}

int
image_scroll_up_region(struct screen *s, u_int lines, u_int top, u_int bottom)
{
	struct image		*im, *im1;
	int			 redraw = 0;
	uint64_t		 itop, ibottom, delete_end, region_end;
#ifdef ENABLE_SIXEL
	struct sixel_image	*new;
	u_int			 sx;
#endif
#if defined(ENABLE_SIXEL) || defined(ENABLE_KITTY_IMAGES)
	u_int			 removed, sy;
#endif

	if (lines == 0 || top > bottom)
		return (0);
	if (lines > bottom + 1 - top)
		lines = bottom + 1 - top;
	delete_end = (uint64_t)top + lines;
	region_end = (uint64_t)bottom + 1;

	TAILQ_FOREACH_SAFE(im, &s->images, entry, im1) {
		if (im->hidden)
			continue;
		itop = im->py;
		ibottom = (uint64_t)im->py + im->sy;
		if (ibottom <= top || itop >= region_end)
			continue;
		image_log(im, __func__, "lines=%u, top=%u, bottom=%u", lines,
		    top, bottom);
		if (itop < top || ibottom > region_end) {
			image_free(im);
			redraw = 1;
			continue;
		}
		if (itop >= delete_end) {
			im->py -= lines;
			redraw = 1;
			continue;
		}
		if (ibottom <= delete_end) {
			image_free(im);
			redraw = 1;
			continue;
		}

		/* Image is partially scrolled off - need to crop it */
		sy = ibottom - delete_end;
		removed = im->sy - sy;
		switch (im->type) {
#ifdef ENABLE_SIXEL
		case IMAGE_SIXEL:
			sx = im->sx;
			image_log(im, __func__, "sixel, lines=%u, sy=%u", lines,
			    sy);

			new = sixel_scale(im->data.sixel, 0, 0, 0, removed, sx, sy,
			    1);
			sixel_free(im->data.sixel);
			im->data.sixel = new;

			im->py = top;
			sixel_size_in_cells(im->data.sixel, &im->sx, &im->sy);

			free(im->fallback);
			image_fallback(&im->fallback, im->type, im->sx, im->sy);
			redraw = 1;
			break;
#endif
#ifdef ENABLE_KITTY_IMAGES
		case IMAGE_KITTY:
			im->kitty_yoff += removed;
			im->py = top;
			im->sy = sy;

			free(im->fallback);
			image_fallback(&im->fallback, im->type, im->sx, im->sy);
			redraw = 1;
			break;
#endif
		default:
			break;
		}
	}
	return (redraw);
}

int
image_scroll_up(struct screen *s, u_int lines)
{
	if (screen_size_y(s) == 0)
		return (0);
	return (image_scroll_up_region(s, lines, 0, screen_size_y(s) - 1));
}
