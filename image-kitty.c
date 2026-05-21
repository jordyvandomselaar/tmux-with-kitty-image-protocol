/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Thomas Adam <thomas@xteddy.org>
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

#include <netinet/in.h>

#include <resolv.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#define KITTY_CHUNK_LIMIT 4096
#define KITTY_PNG_HEADER_SIZE 24

/*
 * kitty_image stores the raw decoded pixel data and metadata from a kitty
 * graphics protocol APC sequence. It is used to re-emit the sequence to the
 * outer terminal on redraw.
 */
struct kitty_image {
	/* Control-data fields parsed from the APC sequence. */
	char		 action;      /* a=: 'T'=transmit+display, 't', 'p', 'd' */
	u_int		 format;      /* f=: 32=RGBA, 24=RGB, 100=PNG */
	char		 medium;      /* t=: 'd'=direct, 'f'=file, 't'=tmp, 's'=shm */
	u_int		 pixel_w;     /* s=: source image pixel width */
	u_int		 pixel_h;     /* v=: source image pixel height */
	u_int		 cols;        /* c=: display columns (0=auto) */
	u_int		 rows;        /* r=: display rows (0=auto) */
	u_int		 image_id;    /* i=: image id (0=unassigned) */
	u_int		 image_num;   /* I=: image number */
	u_int		 placement_id; /* p=: placement id */
	u_int		 more;        /* m=: 1=more chunks coming, 0=last */
	u_int		 has_more;
	u_int		 quiet;       /* q=: suppress responses */
	u_int		 cursor_policy; /* C=: 1=do not move cursor after display */
	u_int		 delete_x;    /* x=: delete cell/column/range start */
	u_int		 delete_y;    /* y=: delete cell/row/range end */
	int		 z_index;     /* z=: z-index */
	char		 compression; /* o=: 'z'=zlib, 0=none */
	char		 delete_what; /* d=: delete target (used with a=d) */

	/* Cell size at the time of parsing (from the owning window). */
	u_int		 xpixel;
	u_int		 ypixel;

	/* Original base64-encoded payload (concatenated across all chunks). */
	char		*encoded;
	size_t		 encodedlen;

	char		*ctrl;
	size_t		 ctrllen;
};

/*
 * Parse control-data key=value pairs from a kitty APC sequence.
 * Format: key=value,key=value,...
 */
static int
kitty_parse_control(const char *ctrl, size_t ctrllen, struct kitty_image *ki)
{
	const char	*p = ctrl, *end = ctrl + ctrllen, *errstr;
	char		 key[4], val[32];
	size_t		 klen, vlen;

	while (p < end) {
		klen = 0;
		while (p < end && *p != '=' && klen < sizeof(key) - 1)
			key[klen++] = *p++;
		key[klen] = '\0';
		if (p >= end || *p != '=')
			return (-1);
		p++;

		vlen = 0;
		while (p < end && *p != ',' && vlen < sizeof(val) - 1)
			val[vlen++] = *p++;
		val[vlen] = '\0';
		if (p < end && *p == ',')
			p++;

		if (klen != 1)
			continue;

		switch (key[0]) {
		case 'a':
			ki->action = val[0];
			break;
		case 'f':
			ki->format = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 't':
			ki->medium = val[0];
			break;
		case 's':
			ki->pixel_w = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'v':
			ki->pixel_h = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'c':
			ki->cols = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'r':
			ki->rows = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'i':
			ki->image_id = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'I':
			ki->image_num = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'p':
			ki->placement_id = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'm':
			ki->more = strtonum(val, 0, 1, &errstr);
			if (errstr != NULL)
				return (-1);
			ki->has_more = 1;
			break;
		case 'q':
			ki->quiet = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'C':
			ki->cursor_policy = strtonum(val, 0, 1, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'x':
			ki->delete_x = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'y':
			ki->delete_y = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'z':
			ki->z_index = strtonum(val, INT_MIN, INT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'o':
			ki->compression = val[0];
			break;
		case 'd':
			ki->delete_what = val[0];
			break;
		}
	}
	return (0);
}

static int
kitty_decode_payload(struct kitty_image *ki, u_char **out, size_t *outlen)
{
	size_t	 size;
	int	 decoded;

	if (ki->encoded == NULL || ki->encodedlen == 0)
		return (0);

	size = ((ki->encodedlen + 3) / 4) * 3;
	*out = xmalloc(size);
	decoded = b64_pton(ki->encoded, *out, size);
	if (decoded == -1) {
		free(*out);
		*out = NULL;
		return (0);
	}
	*outlen = decoded;
	return (1);
}

static u_int
kitty_get_be32(const u_char *p)
{
	return (((u_int)p[0] << 24) | ((u_int)p[1] << 16) |
	    ((u_int)p[2] << 8) | p[3]);
}

static void
kitty_update_png_size(struct kitty_image *ki)
{
	u_char	*out;
	size_t	 outlen;

	if (ki->format != 100 || ki->encoded == NULL || ki->encodedlen == 0)
		return;
	if (ki->pixel_w != 0 && ki->pixel_h != 0)
		return;

	if (!kitty_decode_payload(ki, &out, &outlen))
		return;
	if (outlen >= KITTY_PNG_HEADER_SIZE &&
	    memcmp(out, "\211PNG\r\n\032\n", 8) == 0 &&
	    memcmp(out + 12, "IHDR", 4) == 0) {
		if (ki->pixel_w == 0)
			ki->pixel_w = kitty_get_be32(out + 16);
		if (ki->pixel_h == 0)
			ki->pixel_h = kitty_get_be32(out + 20);
	}
	free(out);
}

static int
kitty_validate_png_payload(struct kitty_image *ki, u_char *out, size_t outlen)
{
	if (outlen < KITTY_PNG_HEADER_SIZE)
		return (0);
	if (memcmp(out, "\211PNG\r\n\032\n", 8) != 0)
		return (0);
	if (memcmp(out + 12, "IHDR", 4) != 0)
		return (0);
	if (ki->pixel_w == 0)
		ki->pixel_w = kitty_get_be32(out + 16);
	if (ki->pixel_h == 0)
		ki->pixel_h = kitty_get_be32(out + 20);
	return (ki->pixel_w != 0 && ki->pixel_h != 0);
}

static int
kitty_validate_raw_payload(struct kitty_image *ki, size_t outlen)
{
	size_t	bytes;
	u_int	depth;

	if (ki->pixel_w == 0 || ki->pixel_h == 0)
		return (0);
	if (ki->format == 24)
		depth = 3;
	else if (ki->format == 32)
		depth = 4;
	else
		return (0);
	if (ki->pixel_w > SIZE_MAX / ki->pixel_h)
		return (0);
	bytes = ki->pixel_w * ki->pixel_h;
	if (bytes > SIZE_MAX / depth)
		return (0);
	return (outlen == bytes * depth);
}

/*
 * Parse a kitty APC body (after the leading 'G').
 * Stores the original control string and base64 payload verbatim for
 * pass-through re-emission to the outer terminal.
 */
struct kitty_image *
kitty_parse(const u_char *buf, size_t len, u_int xpixel, u_int ypixel)
{
	struct kitty_image	*ki;
	const u_char		*semi;
	const char		*ctrl;
	size_t			 ctrllen, paylen;

	if (len == 0)
		return (NULL);

	semi = memchr(buf, ';', len);
	if (semi != NULL) {
		ctrl = (const char *)buf;
		ctrllen = semi - buf;
		paylen = len - ctrllen - 1;
	} else {
		ctrl = (const char *)buf;
		ctrllen = len;
		paylen = 0;
	}

	ki = xcalloc(1, sizeof *ki);
	ki->xpixel = xpixel;
	ki->ypixel = ypixel;
	ki->action = 'T';
	ki->format = 32;
	ki->medium = 'd';

	if (kitty_parse_control(ctrl, ctrllen, ki) != 0) {
		free(ki);
		return (NULL);
	}

	if (paylen > 0) {
		ki->encoded = xmalloc(paylen + 1);
		memcpy(ki->encoded, semi + 1, paylen);
		ki->encoded[paylen] = '\0';
		ki->encodedlen = paylen;
	}

	ki->ctrl = xmalloc(ctrllen + 1);
	memcpy(ki->ctrl, ctrl, ctrllen);
	ki->ctrl[ctrllen] = '\0';
	ki->ctrllen = ctrllen;

	return (ki);
}

void
kitty_free(struct kitty_image *ki)
{
	if (ki == NULL)
		return;
	free(ki->encoded);
	free(ki->ctrl);
	free(ki);
}

/*
 * Get the size in cells of a kitty image. If cols/rows are 0 (auto),
 * calculate from pixel dimensions. Returns size via sx/sy pointers.
 */
void
kitty_size_in_cells(struct kitty_image *ki, u_int *sx, u_int *sy)
{
	kitty_update_png_size(ki);

	*sx = ki->cols;
	*sy = ki->rows;

	/*
	 * If cols/rows are 0, they mean "auto" - calculate from
	 * pixel dimensions.
	 */
	if (*sx == 0 && ki->pixel_w > 0 && ki->xpixel > 0) {
		*sx = (ki->pixel_w + ki->xpixel - 1) / ki->xpixel;
	}
	if (*sy == 0 && ki->pixel_h > 0 && ki->ypixel > 0) {
		*sy = (ki->pixel_h + ki->ypixel - 1) / ki->ypixel;
	}

	/* If still 0, use a reasonable default */
	if (*sx == 0)
		*sx = 10;
	if (*sy == 0)
		*sy = 10;
}

int
kitty_has_height(struct kitty_image *ki)
{
	kitty_update_png_size(ki);

	if (ki->rows != 0)
		return (1);
	if (ki->pixel_h != 0 && ki->ypixel != 0)
		return (1);
	return (0);
}

int
kitty_validate_payload(struct kitty_image *ki)
{
	u_char	*out;
	size_t	 outlen;
	char	 action;
	int	 valid;

	action = ki->action;
	if (action != 'T' && action != 't')
		return (1);
	if (ki->medium != 'd')
		return (1);
	if (ki->compression != '\0')
		return (0);
	if (!kitty_decode_payload(ki, &out, &outlen))
		return (0);

	if (ki->format == 100)
		valid = kitty_validate_png_payload(ki, out, outlen);
	else
		valid = kitty_validate_raw_payload(ki, outlen);
	free(out);
	return (valid);
}

char
kitty_get_action(struct kitty_image *ki)
{
	return (ki->action);
}

char
kitty_get_medium(struct kitty_image *ki)
{
	return (ki->medium);
}

u_int
kitty_get_image_id(struct kitty_image *ki)
{
	return (ki->image_id);
}

u_int
kitty_get_rows(struct kitty_image *ki)
{
	return (ki->rows);
}

u_int
kitty_get_placement_id(struct kitty_image *ki)
{
	return (ki->placement_id);
}

u_int
kitty_get_image_num(struct kitty_image *ki)
{
	return (ki->image_num);
}

char
kitty_get_delete_what(struct kitty_image *ki)
{
	return (ki->delete_what);
}

int
kitty_get_cursor_policy(struct kitty_image *ki)
{
	return (ki->cursor_policy);
}

u_int
kitty_get_delete_x(struct kitty_image *ki)
{
	return (ki->delete_x);
}

u_int
kitty_get_delete_y(struct kitty_image *ki)
{
	return (ki->delete_y);
}

int
kitty_get_z_index(struct kitty_image *ki)
{
	return (ki->z_index);
}

u_int
kitty_get_quiet(struct kitty_image *ki)
{
	return (ki->quiet);
}

int
kitty_has_more(struct kitty_image *ki)
{
	return (ki->has_more);
}

int
kitty_is_incomplete(struct kitty_image *ki)
{
	return (ki->has_more && ki->more != 0);
}

int
kitty_is_continuation(struct kitty_image *ki)
{
	char	*p, *end;

	if (!ki->has_more || ki->ctrl == NULL)
		return (0);
	for (p = ki->ctrl, end = ki->ctrl + ki->ctrllen; p < end; p++) {
		if (p == ki->ctrl || p[-1] == ',') {
			if (p + 1 >= end || p[1] != '=')
				return (0);
			if (p[0] != 'm' && p[0] != 'q')
				return (0);
		}
	}
	return (1);
}

static void
kitty_finish_chunks(struct kitty_image *ki)
{
	char	*p, *end;

	ki->more = 0;
	ki->has_more = 1;

	if (ki->ctrl == NULL)
		return;
	for (p = ki->ctrl, end = ki->ctrl + ki->ctrllen; p < end; p++) {
		if ((p == ki->ctrl || p[-1] == ',') && p + 2 < end &&
		    p[0] == 'm' && p[1] == '=') {
			p[2] = '0';
			return;
		}
	}
}

static char *
kitty_control_with_more(struct kitty_image *ki, int more, size_t *outlen)
{
	char	*ctrl;
	char	*p, *end;
	size_t	 offset;

	for (p = ki->ctrl, end = ki->ctrl + ki->ctrllen; p < end; p++) {
		if ((p == ki->ctrl || p[-1] == ',') && p + 2 < end &&
		    p[0] == 'm' && p[1] == '=') {
			ctrl = xmalloc(ki->ctrllen + 1);
			memcpy(ctrl, ki->ctrl, ki->ctrllen);
			ctrl[ki->ctrllen] = '\0';
			offset = p - ki->ctrl;
			ctrl[offset + 2] = more ? '1' : '0';
			*outlen = ki->ctrllen;
			return (ctrl);
		}
	}

	if (ki->ctrllen == 0)
		*outlen = xasprintf(&ctrl, "m=%d", more ? 1 : 0);
	else
		*outlen = xasprintf(&ctrl, "%s,m=%d", ki->ctrl, more ? 1 : 0);
	return (ctrl);
}

static char *
kitty_control_for_chunk(struct kitty_image *ki, int first, int more,
    size_t *outlen)
{
	char	*ctrl;

	if (first)
		return (kitty_control_with_more(ki, more, outlen));

	if (ki->action == 'f' && ki->quiet != 0) {
		*outlen = xasprintf(&ctrl, "a=f,m=%d,q=%u", more ? 1 : 0,
		    ki->quiet);
		return (ctrl);
	}
	if (ki->action == 'f') {
		*outlen = xasprintf(&ctrl, "a=f,m=%d", more ? 1 : 0);
		return (ctrl);
	}
	if (ki->quiet != 0) {
		*outlen = xasprintf(&ctrl, "m=%d,q=%u", more ? 1 : 0,
		    ki->quiet);
		return (ctrl);
	}
	*outlen = xasprintf(&ctrl, "m=%d", more ? 1 : 0);
	return (ctrl);
}

int
kitty_append(struct kitty_image *ki, struct kitty_image *chunk, size_t limit)
{
	char	*encoded;
	size_t	 encodedlen;

	if (ki == NULL || chunk == NULL || !chunk->has_more)
		return (-1);
	if (ki->encodedlen > limit || chunk->encodedlen > limit ||
	    ki->encodedlen + chunk->encodedlen > limit)
		return (-1);

	if (chunk->encodedlen != 0) {
		encodedlen = ki->encodedlen + chunk->encodedlen;
		encoded = xrealloc(ki->encoded, encodedlen + 1);
		memcpy(encoded + ki->encodedlen, chunk->encoded, chunk->encodedlen);
		encoded[encodedlen] = '\0';
		ki->encoded = encoded;
		ki->encodedlen = encodedlen;
	}

	if (chunk->more == 0) {
		kitty_finish_chunks(ki);
		return (1);
	}
	return (0);
}

/*
 * Serialize a kitty_image back into APC escape sequences for transmission
 * to the terminal, splitting large payloads into protocol-sized chunks.
 */
char *
kitty_print(struct kitty_image *ki, size_t *outlen)
{
	char	*out, *ctrl;
	size_t	 total, pos, offset, remaining, ctrllen, chunk;
	int	 first = 1, more;

	if (ki == NULL || ki->ctrl == NULL)
		return (NULL);

	/* Calculate total length: ESC _ G + ctrl + ; + encoded + ESC \ */
	total = 3 + ki->ctrllen;  /* \033_G + ctrl */
	if (ki->encoded != NULL && ki->encodedlen > 0) {
		total += 1 + ki->encodedlen;  /* ; + encoded */
	}
	total += 2;  /* \033\\ */

	if (ki->encodedlen <= KITTY_CHUNK_LIMIT || ki->encoded == NULL ||
	    ki->encodedlen == 0) {
		out = xmalloc(total + 1);
		*outlen = total;

		/* Build the sequence */
		pos = 0;
		memcpy(out + pos, "\033_G", 3);
		pos += 3;
		memcpy(out + pos, ki->ctrl, ki->ctrllen);
		pos += ki->ctrllen;

		if (ki->encoded != NULL && ki->encodedlen > 0) {
			out[pos++] = ';';
			memcpy(out + pos, ki->encoded, ki->encodedlen);
			pos += ki->encodedlen;
		}

		memcpy(out + pos, "\033\\", 2);
		pos += 2;
		out[pos] = '\0';

		return (out);
	}

	out = xmalloc(1);
	total = 0;
	offset = 0;
	while (offset < ki->encodedlen) {
		remaining = ki->encodedlen - offset;
		chunk = remaining > KITTY_CHUNK_LIMIT ? KITTY_CHUNK_LIMIT : remaining;
		more = (chunk < remaining);
		if (more)
			chunk -= (chunk % 4);
		ctrl = kitty_control_for_chunk(ki, first, more, &ctrllen);

		out = xrealloc(out, total + 3 + ctrllen + 1 + chunk + 2 + 1);
		pos = total;
		memcpy(out + pos, "\033_G", 3);
		pos += 3;
		memcpy(out + pos, ctrl, ctrllen);
		pos += ctrllen;
		out[pos++] = ';';
		memcpy(out + pos, ki->encoded + offset, chunk);
		pos += chunk;
		memcpy(out + pos, "\033\\", 2);
		pos += 2;
		out[pos] = '\0';

		free(ctrl);
		offset += chunk;
		total = pos;
		first = 0;
	}
	*outlen = total;

	return (out);
}

char *
kitty_delete_all(size_t *outlen)
{
	char	*out;

	out = xstrdup("\033_Ga=d,d=a\033\\");
	*outlen = strlen(out);
	return (out);
}
