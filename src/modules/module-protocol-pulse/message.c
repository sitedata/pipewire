/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

struct descriptor {
	uint32_t length;
	uint32_t channel;
	uint32_t offset_hi;
	uint32_t offset_lo;
	uint32_t flags;
};

enum {
	TAG_INVALID = 0,
	TAG_STRING = 't',
	TAG_STRING_NULL = 'N',
	TAG_U32 = 'L',
	TAG_U8 = 'B',
	TAG_U64 = 'R',
	TAG_S64 = 'r',
	TAG_SAMPLE_SPEC = 'a',
	TAG_ARBITRARY = 'x',
	TAG_BOOLEAN_TRUE = '1',
	TAG_BOOLEAN_FALSE = '0',
	TAG_BOOLEAN = TAG_BOOLEAN_TRUE,
	TAG_TIMEVAL = 'T',
	TAG_USEC = 'U'  /* 64bit unsigned */,
	TAG_CHANNEL_MAP = 'm',
	TAG_CVOLUME = 'v',
	TAG_PROPLIST = 'P',
	TAG_VOLUME = 'V',
	TAG_FORMAT_INFO = 'f',
};

struct message {
	struct spa_list link;
	uint32_t channel;
	uint32_t allocated;
	uint32_t length;
	uint32_t offset;
	uint8_t *data;
};

static int message_get(struct message *m, ...);

static int read_u8(struct message *m, uint8_t *val)
{
	if (m->offset + 1 > m->length)
		return -ENOSPC;
	*val = m->data[m->offset];
	m->offset++;
	return 0;
}

static int read_u32(struct message *m, uint32_t *val)
{
	if (m->offset + 4 > m->length)
		return -ENOSPC;
	memcpy(val, &m->data[m->offset], 4);
	*val = ntohl(*val);
	m->offset += 4;
	return 0;
}
static int read_u64(struct message *m, uint64_t *val)
{
	uint32_t tmp;
	int res;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	*val = ((uint64_t)tmp) << 32;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	*val |= tmp;
	return 0;
}

static int read_sample_spec(struct message *m, struct sample_spec *ss)
{
	int res;
	uint8_t tmp;
	if ((res = read_u8(m, &tmp)) < 0)
		return res;
	ss->format = tmp;
	if ((res = read_u8(m, &ss->channels)) < 0)
		return res;
	return read_u32(m, &ss->rate);
}

static int read_props(struct message *m, struct pw_properties *props)
{
	int res;

	while (true) {
		char *key;
		void *data;
		uint32_t length;

		if ((res = message_get(m,
				TAG_STRING, &key,
				TAG_INVALID)) < 0)
			return res;

		if (key == NULL)
			break;

		if ((res = message_get(m,
				TAG_U32, &length,
				TAG_INVALID)) < 0)
			return res;
		if (length > MAX_TAG_SIZE)
			return -EINVAL;

		if ((res = message_get(m,
				TAG_ARBITRARY, &data, length,
				TAG_INVALID)) < 0)
			return res;

		pw_log_debug("%s %s", key, (char*)data);
		pw_properties_set(props, key, data);
	}
	return 0;
}

static int read_arbitrary(struct message *m, const void **val, size_t length)
{
	uint32_t len;
	int res;
	if ((res = read_u32(m, &len)) < 0)
		return res;
	if (len != length)
		return -EINVAL;
	if (m->offset + length > m->length)
		return -ENOSPC;
	*val = m->data + m->offset;
	m->offset += length;
	return 0;
}

static int read_string(struct message *m, char **str)
{
	uint32_t n, maxlen = m->length - m->offset;
	n = strnlen(m->data + m->offset, maxlen);
	if (n == maxlen)
		return -EINVAL;
	*str = m->data + m->offset;
	m->offset += n + 1;
	return 0;
}

static int read_timeval(struct message *m, struct timeval *tv)
{
	int res;
	uint32_t tmp;

	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	tv->tv_sec = tmp;
	if ((res = read_u32(m, &tmp)) < 0)
		return res;
	tv->tv_usec = tmp;
	return 0;
}

static int read_channel_map(struct message *m, struct channel_map *map)
{
	int res;
	uint8_t i, tmp;

	if ((res = read_u8(m, &map->channels)) < 0)
		return res;
	if (map->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < map->channels; i ++) {
		if ((res = read_u8(m, &tmp)) < 0)
			return res;
		map->map[i] = tmp;
	}
	return 0;
}
static int read_volume(struct message *m, float *vol)
{
	int res;
	uint32_t v;
	if ((res = read_u32(m, &v)) < 0)
		return res;
	*vol = ((float)v) / 0x10000U;
	return 0;
}

static int read_cvolume(struct message *m, struct cvolume *vol)
{
	int res;
	uint8_t i;

	if ((res = read_u8(m, &vol->channels)) < 0)
		return res;
	if (vol->channels > CHANNELS_MAX)
		return -EINVAL;
	for (i = 0; i < vol->channels; i ++) {
		if ((res = read_volume(m, &vol->values[i])) < 0)
			return res;
	}
	return 0;
}

static int read_format_info(struct message *m, struct format_info *info)
{
	int res;
	uint8_t tag, encoding;

	if ((res = read_u8(m, &tag)) < 0)
		return res;
	if (tag != TAG_U8)
		return -EPROTO;
	if ((res = read_u8(m, &encoding)) < 0)
		return res;
	info->encoding = encoding;

	if ((res = read_u8(m, &tag)) < 0)
		return res;
	if (tag != TAG_PROPLIST)
		return -EPROTO;

	info->props = pw_properties_new(NULL, NULL);
	if (info->props == NULL)
		return -errno;
	return read_props(m, info->props);
}

static int message_get(struct message *m, ...)
{
	va_list va;
	int res;

	va_start(va, m);

	while (true) {
		int tag = va_arg(va, int);
		uint8_t dtag;
		if (tag == TAG_INVALID)
			break;

		if ((res = read_u8(m, &dtag)) < 0)
			return res;

		switch (dtag) {
		case TAG_STRING:
			if (tag != TAG_STRING)
				return -EINVAL;
			if ((res = read_string(m, va_arg(va, char**))) < 0)
				return res;
			break;
		case TAG_STRING_NULL:
			if (tag != TAG_STRING)
				return -EINVAL;
			*va_arg(va, char**) = NULL;
			break;
		case TAG_U8:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u8(m, va_arg(va, uint8_t*))) < 0)
				return res;
			break;
		case TAG_U32:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u32(m, va_arg(va, uint32_t*))) < 0)
				return res;
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_u64(m, va_arg(va, uint64_t*))) < 0)
				return res;
			break;
		case TAG_SAMPLE_SPEC:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_sample_spec(m, va_arg(va, struct sample_spec*))) < 0)
				return res;
			break;
		case TAG_ARBITRARY:
		{
			const void **val = va_arg(va, const void**);
			size_t len = va_arg(va, size_t);
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_arbitrary(m, val, len)) < 0)
				return res;
			break;
		}
		case TAG_BOOLEAN_TRUE:
			if (tag != TAG_BOOLEAN)
				return -EINVAL;
			*va_arg(va, bool*) = true;
			break;
		case TAG_BOOLEAN_FALSE:
			if (tag != TAG_BOOLEAN)
				return -EINVAL;
			*va_arg(va, bool*) = false;
			break;
		case TAG_TIMEVAL:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_timeval(m, va_arg(va, struct timeval*))) < 0)
				return res;
			break;
		case TAG_CHANNEL_MAP:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_channel_map(m, va_arg(va, struct channel_map*))) < 0)
				return res;
			break;
		case TAG_CVOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_cvolume(m, va_arg(va, struct cvolume*))) < 0)
				return res;
			break;
		case TAG_PROPLIST:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_props(m, va_arg(va, struct pw_properties*))) < 0)
				return res;
			break;
		case TAG_VOLUME:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_volume(m, va_arg(va, float*))) < 0)
				return res;
			break;
		case TAG_FORMAT_INFO:
			if (dtag != tag)
				return -EINVAL;
			if ((res = read_format_info(m, va_arg(va, struct format_info*))) < 0)
				return res;
			break;
		}
	}
	va_end(va);

	return 0;
}

static int ensure_size(struct message *m, uint32_t size)
{
	uint32_t alloc;
	if (m->length + size <= m->allocated)
		return size;

	alloc = SPA_ROUND_UP_N(SPA_MAX(m->allocated + size, 4096u), 4096u);
	if ((m->data = realloc(m->data, alloc)) == NULL)
		return -errno;
	m->allocated = alloc;
	return size;
}

static void write_8(struct message *m, uint8_t val)
{
	if (ensure_size(m, 1) > 0)
		m->data[m->length] = val;
	m->length++;
}

static void write_32(struct message *m, uint32_t val)
{
	val = htonl(val);
	if (ensure_size(m, 4) > 0)
		memcpy(m->data + m->length, &val, 4);
	m->length += 4;
}

static void write_string(struct message *m, const char *s)
{
	write_8(m, s ? TAG_STRING : TAG_STRING_NULL);
	if (s != NULL) {
		int len = strlen(s) + 1;
		if (ensure_size(m, len) > 0)
			strcpy(&m->data[m->length], s);
		m->length += len;
	}
}
static void write_u8(struct message *m, uint8_t val)
{
	write_8(m, TAG_U8);
	write_8(m, val);
}

static void write_u32(struct message *m, uint32_t val)
{
	write_8(m, TAG_U32);
	write_32(m, val);
}

static void write_64(struct message *m, uint8_t tag, uint64_t val)
{
	write_8(m, tag);
	write_32(m, val >> 32);
	write_32(m, val);
}

static void write_sample_spec(struct message *m, struct sample_spec *ss)
{
	write_8(m, TAG_SAMPLE_SPEC);
	write_8(m, ss->format);
	write_8(m, ss->channels);
	write_32(m, ss->rate);
}

static void write_arbitrary(struct message *m, const void *p, size_t length)
{
	write_8(m, TAG_ARBITRARY);
	write_32(m, length);
	if (ensure_size(m, length) > 0)
		memcpy(m->data + m->length, p, length);
	m->length += length;
}

static void write_boolean(struct message *m, bool val)
{
	write_8(m, val ? TAG_BOOLEAN_TRUE : TAG_BOOLEAN_FALSE);
}

static void write_timeval(struct message *m, struct timeval *tv)
{
	write_8(m, TAG_TIMEVAL);
	write_32(m, tv->tv_sec);
	write_32(m, tv->tv_usec);
}

static void write_channel_map(struct message *m, struct channel_map *map)
{
	uint8_t i;
	write_8(m, TAG_CHANNEL_MAP);
	write_8(m, map->channels);
	for (i = 0; i < map->channels; i ++)
		write_8(m, map->map[i]);
}

static void write_volume(struct message *m, float vol)
{
	write_8(m, TAG_VOLUME);
	write_32(m, vol * 0x10000U);
}

static void write_cvolume(struct message *m, struct cvolume *cvol)
{
	uint8_t i;
	write_8(m, TAG_CVOLUME);
	write_8(m, cvol->channels);
	for (i = 0; i < cvol->channels; i ++)
		write_32(m, cvol->values[i] * 0x10000U);
}

static void write_props(struct message *m, struct pw_properties *props)
{
	const struct spa_dict_item *it;
	write_8(m, TAG_PROPLIST);
	if (props != NULL) {
		spa_dict_for_each(it, &props->dict) {
			int l = strlen(it->value);
			write_string(m, it->key);
			write_u32(m, l+1);
			write_arbitrary(m, it->value, l+1);
		}
	}
	write_string(m, NULL);
}

static void write_format_info(struct message *m, struct format_info *info)
{
	write_8(m, TAG_FORMAT_INFO);
	write_u8(m, (uint8_t) info->encoding);
	write_props(m, info->props);
}

static int message_put(struct message *m, ...)
{
	va_list va;

	va_start(va, m);

	while (true) {
		int tag = va_arg(va, int);
		if (tag == TAG_INVALID)
			break;

		switch (tag) {
		case TAG_STRING:
			write_string(m, va_arg(va, const char *));
			break;
		case TAG_U8:
			write_u8(m, (uint8_t)va_arg(va, int));
			break;
		case TAG_U32:
			write_u32(m, (uint32_t)va_arg(va, uint32_t));
			break;
		case TAG_S64:
		case TAG_U64:
		case TAG_USEC:
			write_64(m, tag, va_arg(va, uint64_t));
			break;
		case TAG_SAMPLE_SPEC:
			write_sample_spec(m, va_arg(va, struct sample_spec*));
			break;
		case TAG_ARBITRARY:
		{
			const void *p = va_arg(va, const void*);
			size_t length = va_arg(va, size_t);
			write_arbitrary(m, p, length);
			break;
		}
		case TAG_BOOLEAN:
			write_boolean(m, va_arg(va, int));
			break;
		case TAG_TIMEVAL:
			write_timeval(m, va_arg(va, struct timeval*));
			break;
		case TAG_CHANNEL_MAP:
			write_channel_map(m, va_arg(va, struct channel_map*));
			break;
		case TAG_CVOLUME:
			write_cvolume(m, va_arg(va, struct cvolume*));
			break;
		case TAG_PROPLIST:
			write_props(m, va_arg(va, struct pw_properties*));
			break;
		case TAG_VOLUME:
			write_volume(m, va_arg(va, double));
			break;
		case TAG_FORMAT_INFO:
			write_format_info(m, va_arg(va, struct format_info*));
			break;
		}
	}
	va_end(va);

	return 0;
}
