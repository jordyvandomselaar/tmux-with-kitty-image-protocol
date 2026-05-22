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
#include <zlib.h>

#include "tmux.h"

#define KITTY_CHUNK_LIMIT 4096
#define KITTY_INFLATE_LIMIT INPUT_BUF_DEFAULT_SIZE
#define KITTY_PNG_HEADER_SIZE 24
#define KITTY_PNG_SIGNATURE_SIZE 8
#define KITTY_PNG_IHDR_SIZE 13
#define KITTY_QUIET_SUPPRESS_RESPONSES 2
#define KITTY_QUIET_UNCHANGED ((u_int)-1)

struct kitty_control_override {
	char		 key;
	const char	*value;
};

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
	u_int		 source_w;    /* w=: source rectangle width */
	u_int		 source_h;    /* h=: source rectangle height */
	u_int		 cell_x;      /* X=: horizontal cell offset */
	u_int		 cell_y;      /* Y=: vertical cell offset */
	u_int		 terminal_image_id;
	u_int		 terminal_placement_id;
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
		case 'w':
			ki->source_w = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'h':
			ki->source_h = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'X':
			ki->cell_x = strtonum(val, 0, UINT_MAX, &errstr);
			if (errstr != NULL)
				return (-1);
			break;
		case 'Y':
			ki->cell_y = strtonum(val, 0, UINT_MAX, &errstr);
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
kitty_inflate_payload(const u_char *in, size_t inlen, u_char **out,
    size_t *outlen)
{
	z_stream	 zs;
	size_t		 cap;
	int		 ret;

	if (inlen == 0 || inlen > UINT_MAX)
		return (0);

	cap = inlen * 2;
	if (cap < 4096)
		cap = 4096;
	if (cap > KITTY_INFLATE_LIMIT)
		cap = KITTY_INFLATE_LIMIT;

	*out = xmalloc(cap);
	*outlen = 0;

	memset(&zs, 0, sizeof zs);
	zs.next_in = (Bytef *)in;
	zs.avail_in = (uInt)inlen;
	if (inflateInit(&zs) != Z_OK)
		goto fail;

	for (;;) {
		zs.next_out = *out + *outlen;
		zs.avail_out = (uInt)(cap - *outlen);
		ret = inflate(&zs, Z_NO_FLUSH);
		*outlen = cap - zs.avail_out;

		if (ret == Z_STREAM_END) {
			inflateEnd(&zs);
			return (1);
		}
		if (ret != Z_OK)
			break;
		if (*outlen == KITTY_INFLATE_LIMIT)
			break;
		if (*outlen == cap) {
			cap *= 2;
			if (cap > KITTY_INFLATE_LIMIT)
				cap = KITTY_INFLATE_LIMIT;
			*out = xrealloc(*out, cap);
		}
	}

	inflateEnd(&zs);

fail:
	free(*out);
	*out = NULL;
	*outlen = 0;
	return (0);
}

static int
kitty_decode_payload(struct kitty_image *ki, u_char **out, size_t *outlen)
{
	u_char	*decoded_data;
	size_t	 size;
	int	 decoded_len;

	if (ki->encoded == NULL || ki->encodedlen == 0)
		return (0);

	size = ((ki->encodedlen + 3) / 4) * 3;
	decoded_data = xmalloc(size);
	decoded_len = b64_pton(ki->encoded, decoded_data, size);
	if (decoded_len == -1) {
		free(decoded_data);
		return (0);
	}
	if (ki->compression == '\0') {
		*out = decoded_data;
		*outlen = decoded_len;
		return (1);
	}
	if (ki->compression != 'z') {
		free(decoded_data);
		return (0);
	}
	if (!kitty_inflate_payload(decoded_data, decoded_len, out, outlen)) {
		free(decoded_data);
		return (0);
	}
	free(decoded_data);
	return (1);
}

static u_int
kitty_get_be32(const u_char *p)
{
	return (((u_int)p[0] << 24) | ((u_int)p[1] << 16) |
	    ((u_int)p[2] << 8) | p[3]);
}

static int
kitty_png_bit_depth_valid(u_int bit_depth, u_int color_type)
{
	switch (color_type) {
	case 0:
		return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
		    bit_depth == 8 || bit_depth == 16);
	case 2:
		return (bit_depth == 8 || bit_depth == 16);
	case 3:
		return (bit_depth == 1 || bit_depth == 2 || bit_depth == 4 ||
		    bit_depth == 8);
	case 4:
		return (bit_depth == 8 || bit_depth == 16);
	case 6:
		return (bit_depth == 8 || bit_depth == 16);
	}
	return (0);
}

static u_int
kitty_png_samples_per_pixel(u_int color_type)
{
	switch (color_type) {
	case 2:
		return (3);
	case 4:
		return (2);
	case 6:
		return (4);
	default:
		return (1);
	}
}

static int
kitty_png_expected_size(u_int width, u_int height, u_int bit_depth,
    u_int color_type, uint64_t *expected)
{
	uint64_t	 samples, row_bits, row_bytes;

	samples = kitty_png_samples_per_pixel(color_type);
	if (width == 0 || height == 0)
		return (0);
	if (width > UINT64_MAX / samples / bit_depth)
		return (0);
	row_bits = (uint64_t)width * samples * bit_depth;
	if (row_bits > UINT64_MAX - 7)
		return (0);
	row_bytes = (row_bits + 7) / 8 + 1;
	if (row_bytes > UINT64_MAX / height)
		return (0);
	*expected = row_bytes * height;
	return (1);
}

static int
kitty_png_validate_idat(const u_char *idat, size_t idatlen,
    uint64_t expected, int check_expected)
{
	u_char		 out[4096];
	z_stream	 zs;
	size_t		 total = 0;
	int		 ret;

	if (idatlen == 0 || idatlen > UINT_MAX)
		return (0);
	if (check_expected && expected > SIZE_MAX)
		return (0);

	memset(&zs, 0, sizeof zs);
	zs.next_in = (Bytef *)idat;
	zs.avail_in = (uInt)idatlen;
	if (inflateInit(&zs) != Z_OK)
		return (0);

	do {
		zs.next_out = out;
		zs.avail_out = sizeof out;
		ret = inflate(&zs, Z_NO_FLUSH);
		total += sizeof out - zs.avail_out;
		if (check_expected && total > expected) {
			inflateEnd(&zs);
			return (0);
		}
	} while (ret == Z_OK);

	inflateEnd(&zs);
	if (ret != Z_STREAM_END || zs.avail_in != 0)
		return (0);
	return (!check_expected || total == expected);
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
	u_char		*idat = NULL, *new_idat;
	const u_char	*type, *data;
	size_t		 pos, length, idatlen = 0;
	uLong		 crc;
	uint64_t	 expected = 0;
	u_int		 width = 0, height = 0, bit_depth = 0, color_type = 0;
	u_int		 compression = 0, filter = 0, interlace = 0, stored_crc;
	int		 seen_ihdr = 0, seen_idat = 0, seen_iend = 0;
	int		 seen_plte = 0, after_idat = 0, valid = 0;

	if (outlen < KITTY_PNG_SIGNATURE_SIZE + 12)
		return (0);
	if (memcmp(out, "\211PNG\r\n\032\n", KITTY_PNG_SIGNATURE_SIZE) != 0)
		return (0);

	pos = KITTY_PNG_SIGNATURE_SIZE;
	while (pos + 12 <= outlen) {
		length = kitty_get_be32(out + pos);
		if (length > outlen - pos - 12)
			goto out;
		type = out + pos + 4;
		data = out + pos + 8;

		crc = crc32(0L, Z_NULL, 0);
		crc = crc32(crc, type, 4);
		if (length != 0)
			crc = crc32(crc, data, (uInt)length);
		stored_crc = kitty_get_be32(data + length);
		if ((u_int)crc != stored_crc)
			goto out;

		if (!seen_ihdr) {
			if (memcmp(type, "IHDR", 4) != 0 ||
			    length != KITTY_PNG_IHDR_SIZE)
				goto out;
			seen_ihdr = 1;
			width = kitty_get_be32(data);
			height = kitty_get_be32(data + 4);
			bit_depth = data[8];
			color_type = data[9];
			compression = data[10];
			filter = data[11];
			interlace = data[12];
			if (width == 0 || height == 0 || compression != 0 ||
			    filter != 0 || interlace > 1)
				goto out;
			if (!kitty_png_bit_depth_valid(bit_depth, color_type))
				goto out;
			pos += length + 12;
			continue;
		}

		if (memcmp(type, "IHDR", 4) == 0)
			goto out;
		if (memcmp(type, "PLTE", 4) == 0) {
			if (seen_idat || length == 0 || length % 3 != 0)
				goto out;
			seen_plte = 1;
		} else if (memcmp(type, "IDAT", 4) == 0) {
			if (length == 0 || after_idat ||
			    idatlen > SIZE_MAX - length)
				goto out;
			new_idat = xrealloc(idat, idatlen + length);
			idat = new_idat;
			memcpy(idat + idatlen, data, length);
			idatlen += length;
			seen_idat = 1;
		} else if (memcmp(type, "IEND", 4) == 0) {
			if (length != 0 || !seen_idat || pos + 12 != outlen)
				goto out;
			seen_iend = 1;
			break;
		} else {
			if (seen_idat)
				after_idat = 1;
			if (type[0] >= 'A' && type[0] <= 'Z')
				goto out;
		}

		pos += length + 12;
	}

	if (!seen_ihdr || !seen_idat || !seen_iend)
		goto out;
	if (color_type == 3 && !seen_plte)
		goto out;
	if (interlace == 0 && !kitty_png_expected_size(width, height, bit_depth,
	    color_type, &expected))
		goto out;
	if (!kitty_png_validate_idat(idat, idatlen, expected, interlace == 0))
		goto out;

	if (ki->pixel_w == 0)
		ki->pixel_w = width;
	if (ki->pixel_h == 0)
		ki->pixel_h = height;
	valid = (ki->pixel_w != 0 && ki->pixel_h != 0);

out:
	free(idat);
	return (valid);
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

size_t
kitty_size_in_bytes(struct kitty_image *ki)
{
	if (ki == NULL)
		return (0);
	return (sizeof *ki + ki->encodedlen + ki->ctrllen);
}

int
kitty_exceeds_chunk_limit(struct kitty_image *ki)
{
	return (ki != NULL && ki->encodedlen > KITTY_CHUNK_LIMIT);
}

void
kitty_copy_source_metadata(struct kitty_image *dst, struct kitty_image *src)
{
	if (dst == NULL || src == NULL)
		return;

	kitty_update_png_size(src);
	if (dst->format == 0)
		dst->format = src->format;
	if (dst->pixel_w == 0)
		dst->pixel_w = src->pixel_w;
	if (dst->pixel_h == 0)
		dst->pixel_h = src->pixel_h;
	if (dst->delete_x == 0)
		dst->delete_x = src->delete_x;
	if (dst->delete_y == 0)
		dst->delete_y = src->delete_y;
	if (dst->source_w == 0)
		dst->source_w = src->source_w;
	if (dst->source_h == 0)
		dst->source_h = src->source_h;
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

void
kitty_set_image_id(struct kitty_image *ki, u_int image_id)
{
	ki->image_id = image_id;
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
kitty_get_terminal_image_id(struct kitty_image *ki)
{
	return (ki->terminal_image_id);
}

void
kitty_set_terminal_image_id(struct kitty_image *ki, u_int image_id)
{
	ki->terminal_image_id = image_id;
}

u_int
kitty_get_terminal_placement_id(struct kitty_image *ki)
{
	return (ki->terminal_placement_id);
}

void
kitty_set_terminal_placement_id(struct kitty_image *ki, u_int placement_id)
{
	ki->terminal_placement_id = placement_id;
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
	return (ki->has_more && ki->more != 0 && ki->encodedlen % 4 == 0);
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

static void
kitty_control_append(char **ctrl, size_t *len, const char *data,
    size_t datalen)
{
	size_t	oldlen;

	oldlen = *len;
	*ctrl = xrealloc(*ctrl, oldlen + (oldlen != 0) + datalen + 1);
	if (oldlen != 0)
		(*ctrl)[(*len)++] = ',';
	memcpy(*ctrl + *len, data, datalen);
	*len += datalen;
	(*ctrl)[*len] = '\0';
}

static int
kitty_control_has_override(const struct kitty_control_override *overrides,
    size_t noverrides, const char *token, size_t tokenlen)
{
	size_t	i;

	if (tokenlen < 2 || token[1] != '=')
		return (0);
	for (i = 0; i < noverrides; i++) {
		if (overrides[i].key == token[0])
			return (1);
	}
	return (0);
}

static char *
kitty_control_with_overrides(struct kitty_image *ki,
    const struct kitty_control_override *overrides, size_t noverrides,
    size_t *outlen)
{
	const char	*p, *end, *next;
	char		*ctrl, *tmp;
	size_t		 len, tokenlen, i, tmplen;

	ctrl = xmalloc(1);
	ctrl[0] = '\0';
	len = 0;

	p = ki->ctrl;
	end = ki->ctrl + ki->ctrllen;
	while (p < end) {
		next = memchr(p, ',', end - p);
		if (next == NULL)
			next = end;
		tokenlen = next - p;
		if (tokenlen != 0 && !kitty_control_has_override(overrides,
		    noverrides, p, tokenlen))
			kitty_control_append(&ctrl, &len, p, tokenlen);
		p = next;
		if (p < end && *p == ',')
			p++;
	}

	for (i = 0; i < noverrides; i++) {
		if (overrides[i].value == NULL)
			continue;
		tmplen = xasprintf(&tmp, "%c=%s", overrides[i].key,
		    overrides[i].value);
		kitty_control_append(&ctrl, &len, tmp, tmplen);
		free(tmp);
	}

	*outlen = len;
	return (ctrl);
}

static size_t
kitty_control_add_quiet(struct kitty_control_override *overrides, size_t n,
    u_int quiet, char *quietbuf, size_t quietbuflen)
{
	if (quiet == KITTY_QUIET_UNCHANGED)
		return (n);
	snprintf(quietbuf, quietbuflen, "%u", quiet);
	overrides[n].key = 'q';
	overrides[n].value = quietbuf;
	return (n + 1);
}

static size_t
kitty_control_add_namespace(struct kitty_image *ki,
    struct kitty_control_override *overrides, size_t n, char *imagebuf,
    size_t imagebuflen, char *placementbuf, size_t placementbuflen)
{
	if (ki->terminal_image_id != 0) {
		snprintf(imagebuf, imagebuflen, "%u", ki->terminal_image_id);
		overrides[n].key = 'i';
		overrides[n].value = imagebuf;
		n++;
		overrides[n].key = 'I';
		overrides[n].value = NULL;
		n++;
	}
	if (ki->terminal_placement_id != 0) {
		snprintf(placementbuf, placementbuflen, "%u",
		    ki->terminal_placement_id);
		overrides[n].key = 'p';
		overrides[n].value = placementbuf;
		n++;
	}
	return (n);
}

static char *
kitty_control_for_command(struct kitty_image *ki, u_int quiet,
    const struct kitty_control_override *extra, size_t nextra, size_t *outlen)
{
	struct kitty_control_override	 overrides[16];
	char				 quietbuf[32], imagebuf[32];
	char				 placementbuf[32];
	size_t				 i, n;

	n = kitty_control_add_quiet(overrides, 0, quiet, quietbuf,
	    sizeof quietbuf);
	n = kitty_control_add_namespace(ki, overrides, n, imagebuf,
	    sizeof imagebuf, placementbuf, sizeof placementbuf);
	for (i = 0; i < nextra; i++)
		overrides[n++] = extra[i];
	return (kitty_control_with_overrides(ki, overrides, n, outlen));
}

static char *
kitty_control_for_chunk(struct kitty_image *ki, int first, int more,
    u_int quiet, const struct kitty_control_override *extra, size_t nextra,
    size_t *outlen)
{
	struct kitty_control_override	 overrides[16];
	char				 morebuf[2], quietbuf[32], imagebuf[32];
	char				 placementbuf[32];
	char				*ctrl;
	size_t				 i, n;

	if (first) {
		snprintf(morebuf, sizeof morebuf, "%d", more ? 1 : 0);
		overrides[0].key = 'm';
		overrides[0].value = morebuf;
		n = kitty_control_add_quiet(overrides, 1, quiet, quietbuf,
		    sizeof quietbuf);
		n = kitty_control_add_namespace(ki, overrides, n, imagebuf,
		    sizeof imagebuf, placementbuf, sizeof placementbuf);
		for (i = 0; i < nextra; i++)
			overrides[n++] = extra[i];
		return (kitty_control_with_overrides(ki, overrides, n, outlen));
	}

	if (quiet == KITTY_QUIET_UNCHANGED)
		quiet = ki->quiet;
	if (ki->action == 'f' && quiet != 0) {
		*outlen = xasprintf(&ctrl, "a=f,m=%d,q=%u", more ? 1 : 0,
		    quiet);
		return (ctrl);
	}
	if (ki->action == 'f') {
		*outlen = xasprintf(&ctrl, "a=f,m=%d", more ? 1 : 0);
		return (ctrl);
	}
	if (quiet != 0) {
		*outlen = xasprintf(&ctrl, "m=%d,q=%u", more ? 1 : 0,
		    quiet);
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
	if (kitty_exceeds_chunk_limit(chunk))
		return (-1);
	if (chunk->more != 0 && chunk->encodedlen % 4 != 0)
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

static char *
kitty_print_with_overrides(struct kitty_image *ki, size_t *outlen, u_int quiet,
    const struct kitty_control_override *extra, size_t nextra)
{
	char	*out, *ctrl;
	size_t	 total, pos, offset, remaining, ctrllen, chunk;
	int	 first = 1, more;

	if (ki == NULL || ki->ctrl == NULL)
		return (NULL);

	ctrl = kitty_control_for_command(ki, quiet, extra, nextra, &ctrllen);

	/* Calculate total length: ESC _ G + ctrl + ; + encoded + ESC \ */
	total = 3 + ctrllen;  /* \033_G + ctrl */
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
		memcpy(out + pos, ctrl, ctrllen);
		pos += ctrllen;

		if (ki->encoded != NULL && ki->encodedlen > 0) {
			out[pos++] = ';';
			memcpy(out + pos, ki->encoded, ki->encodedlen);
			pos += ki->encodedlen;
		}

		memcpy(out + pos, "\033\\", 2);
		pos += 2;
		out[pos] = '\0';

		free(ctrl);
		return (out);
	}
	free(ctrl);

	out = xmalloc(1);
	total = 0;
	offset = 0;
	while (offset < ki->encodedlen) {
		remaining = ki->encodedlen - offset;
		chunk = remaining > KITTY_CHUNK_LIMIT ? KITTY_CHUNK_LIMIT : remaining;
		more = (chunk < remaining);
		if (more)
			chunk -= (chunk % 4);
		ctrl = kitty_control_for_chunk(ki, first, more, quiet, extra,
		    nextra, &ctrllen);

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

/*
 * Serialize a kitty_image back into APC escape sequences for transmission
 * to the terminal, splitting large payloads into protocol-sized chunks.
 */
char *
kitty_print(struct kitty_image *ki, size_t *outlen)
{
	return (kitty_print_with_overrides(ki, outlen, KITTY_QUIET_UNCHANGED,
	    NULL, 0));
}

char *
kitty_print_quiet(struct kitty_image *ki, size_t *outlen)
{
	return (kitty_print_with_overrides(ki, outlen,
	    KITTY_QUIET_SUPPRESS_RESPONSES, NULL, 0));
}

char *
kitty_print_clipped(struct kitty_image *ki, u_int xoff, u_int yoff,
    u_int cellsx, u_int cellsy, size_t *outlen)
{
	struct kitty_control_override	 overrides[6];
	char				 xbuf[32], ybuf[32], wbuf[32];
	char				 hbuf[32], cbuf[32], rbuf[32];
	u_int				 sx, sy, source_x, source_y;
	u_int				 source_w, source_h;
	uint64_t			 left, right, top, bottom;

	if (ki == NULL || cellsx == 0 || cellsy == 0)
		return (NULL);
	if (ki->cell_x != 0 || ki->cell_y != 0)
		return (NULL);

	kitty_update_png_size(ki);
	if (ki->pixel_w == 0 || ki->pixel_h == 0)
		return (NULL);

	kitty_size_in_cells(ki, &sx, &sy);
	if (sx == 0 || sy == 0 || xoff >= sx || yoff >= sy)
		return (NULL);
	if (cellsx > sx - xoff || cellsy > sy - yoff)
		return (NULL);

	source_x = ki->delete_x;
	source_y = ki->delete_y;
	if (source_x >= ki->pixel_w || source_y >= ki->pixel_h)
		return (NULL);
	source_w = ki->source_w;
	if (source_w == 0)
		source_w = ki->pixel_w - source_x;
	if (source_w > ki->pixel_w - source_x)
		source_w = ki->pixel_w - source_x;
	source_h = ki->source_h;
	if (source_h == 0)
		source_h = ki->pixel_h - source_y;
	if (source_h > ki->pixel_h - source_y)
		source_h = ki->pixel_h - source_y;
	if (source_w == 0 || source_h == 0)
		return (NULL);

	left = source_x + ((uint64_t)source_w * xoff) / sx;
	right = source_x + ((uint64_t)source_w * (xoff + cellsx)) / sx;
	top = source_y + ((uint64_t)source_h * yoff) / sy;
	bottom = source_y + ((uint64_t)source_h * (yoff + cellsy)) / sy;
	if (right <= left || bottom <= top)
		return (NULL);

	snprintf(xbuf, sizeof xbuf, "%llu", (unsigned long long)left);
	snprintf(ybuf, sizeof ybuf, "%llu", (unsigned long long)top);
	snprintf(wbuf, sizeof wbuf, "%llu", (unsigned long long)(right - left));
	snprintf(hbuf, sizeof hbuf, "%llu", (unsigned long long)(bottom - top));
	snprintf(cbuf, sizeof cbuf, "%u", cellsx);
	snprintf(rbuf, sizeof rbuf, "%u", cellsy);
	overrides[0].key = 'x';
	overrides[0].value = xbuf;
	overrides[1].key = 'y';
	overrides[1].value = ybuf;
	overrides[2].key = 'w';
	overrides[2].value = wbuf;
	overrides[3].key = 'h';
	overrides[3].value = hbuf;
	overrides[4].key = 'c';
	overrides[4].value = cbuf;
	overrides[5].key = 'r';
	overrides[5].value = rbuf;

	return (kitty_print_with_overrides(ki, outlen,
	    KITTY_QUIET_SUPPRESS_RESPONSES, overrides, 6));
}

char *
kitty_delete_all(size_t *outlen)
{
	char	*out;

	out = xstrdup("\033_Ga=d,d=a,q=2\033\\");
	*outlen = strlen(out);
	return (out);
}
