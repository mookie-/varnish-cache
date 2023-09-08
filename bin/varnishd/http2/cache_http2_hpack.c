/*-
 * Copyright (c) 2016-2019 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "config.h"

#include "cache/cache_varnishd.h"

#include <ctype.h>
#include <stdio.h>

#include "http2/cache_http2.h"
#include "vct.h"

// rfc9113,l,2493,2528
static h2_error
h2h_checkhdr(const struct http *hp, const char *b, size_t namelen, size_t len)
{
	const char *p;
	enum {
		FLD_NAME_FIRST,
		FLD_NAME,
		FLD_VALUE_FIRST,
		FLD_VALUE
	} state;

	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	AN(b);
	assert(namelen >= 2);	/* 2 chars from the ': ' that we added */
	assert(namelen <= len);
	assert(b[namelen - 2] == ':');
	assert(b[namelen - 1] == ' ');

	if (namelen == 2) {
		VSLb(hp->vsl, SLT_BogoHeader, "Empty name");
		return (H2SE_PROTOCOL_ERROR);
	}

	// VSLb(hp->vsl, SLT_Debug, "CHDR [%.*s] [%.*s]",
	//     (int)namelen, b, (int)(len - namelen), b + namelen);

	state = FLD_NAME_FIRST;
	for (p = b; p < b + namelen - 2; p++) {
		switch(state) {
		case FLD_NAME_FIRST:
			state = FLD_NAME;
			if (*p == ':')
				break;
			/* FALL_THROUGH */
		case FLD_NAME:
			if (*p <= 0x20 || *p >= 0x7f) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal field header name (control): %.*s",
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}
			if (isupper(*p)) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal field header name (upper-case): %.*s",
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}
			if (!vct_istchar(*p) || *p == ':') {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal field header name (non-token): %.*s",
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}
			break;
		default:
			WRONG("http2 field name validation state");
		}
	}

	state = FLD_VALUE_FIRST;
	for (p = b + namelen; p < b + len; p++) {
		switch(state) {
		case FLD_VALUE_FIRST:
			if (vct_issp(*p)) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal field value start %.*s",
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}
			state = FLD_VALUE;
			/* FALL_THROUGH */
		case FLD_VALUE:
			if (!vct_ishdrval(*p)) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal field value %.*s",
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}
			break;
		default:
			WRONG("http2 field value validation state");
		}
	}
	if (state == FLD_VALUE && vct_issp(b[len - 1])) {
		VSLb(hp->vsl, SLT_BogoHeader,
		    "Illegal val (end) %.*s",
		    (int)(len > 20 ? 20 : len), b);
		return (H2SE_PROTOCOL_ERROR);
	}
	return (0);
}

static h2_error
h2h_addhdr(struct h2h_decode *d, struct http *hp, char *b, size_t namelen,
    size_t len)
{
	/* XXX: This might belong in cache/cache_http.c */
	const char *b0;
	int disallow_empty;
	unsigned n;
	char *p;
	unsigned u;

	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	AN(b);
	assert(namelen >= 2);	/* 2 chars from the ': ' that we added */
	assert(namelen <= len);

	disallow_empty = 0;

	if (len > UINT_MAX) {	/* XXX: cache_param max header size */
		VSLb(hp->vsl, SLT_BogoHeader, "Header too large: %.20s", b);
		return (H2SE_ENHANCE_YOUR_CALM);
	}

	b0 = b;
	if (b[0] == ':') {
		/* Match H/2 pseudo headers */
		/* XXX: Should probably have some include tbl for
		   pseudo-headers */
		if (!strncmp(b, ":method: ", namelen)) {
			b += namelen;
			len -= namelen;
			n = HTTP_HDR_METHOD;
			disallow_empty = 1;

			/* First field cannot contain SP or CTL */
			for (p = b, u = 0; u < len; p++, u++) {
				if (vct_issp(*p) || vct_isctl(*p))
					return (H2SE_PROTOCOL_ERROR);
			}
		} else if (!strncmp(b, ":path: ", namelen)) {
			b += namelen;
			len -= namelen;
			n = HTTP_HDR_URL;
			disallow_empty = 1;

			// rfc9113,l,2693,2705
			if (len > 0 && *b != '/' &&
			    strncmp(b, "*", len) != 0) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Illegal :path pseudo-header %.*s",
				    (int)len, b);
				return (H2SE_PROTOCOL_ERROR);
			}

			/* Second field cannot contain LWS or CTL */
			for (p = b, u = 0; u < len; p++, u++) {
				if (vct_islws(*p) || vct_isctl(*p))
					return (H2SE_PROTOCOL_ERROR);
			}
		} else if (!strncmp(b, ":scheme: ", namelen)) {
			/* XXX: What to do about this one? (typically
			   "http" or "https"). For now set it as a normal
			   header, stripping the first ':'. */
			if (d->has_scheme) {
				VSLb(hp->vsl, SLT_BogoHeader,
				    "Duplicate pseudo-header %.*s%.*s",
				    (int)namelen, b0,
				    (int)(len > 20 ? 20 : len), b);
				return (H2SE_PROTOCOL_ERROR);
			}

			b++;
			len-=1;
			n = hp->nhd;
			d->has_scheme = 1;

			for (p = b + namelen, u = 0; u < len-namelen;
			    p++, u++) {
				if (vct_issp(*p) || vct_isctl(*p))
					return (H2SE_PROTOCOL_ERROR);
			}

			if (!u)
				return (H2SE_PROTOCOL_ERROR);
		} else if (!strncmp(b, ":authority: ", namelen)) {
			b+=6;
			len-=6;
			memcpy(b, "host", 4);
			n = hp->nhd;
		} else {
			/* Unknown pseudo-header */
			VSLb(hp->vsl, SLT_BogoHeader,
			    "Unknown pseudo-header: %.*s",
			    (int)(len > 20 ? 20 : len), b);
			return (H2SE_PROTOCOL_ERROR);	// rfc7540,l,2990,2992
		}
	} else
		n = hp->nhd;

	if (n < HTTP_HDR_FIRST) {
		/* Check for duplicate pseudo-header */
		if (hp->hd[n].b != NULL) {
			VSLb(hp->vsl, SLT_BogoHeader,
			    "Duplicate pseudo-header %.*s%.*s",
			    (int)namelen, b0, (int)(len > 20 ? 20 : len), b);
			return (H2SE_PROTOCOL_ERROR);	// rfc7540,l,3158,3162
		}
	} else {
		/* Check for space in struct http */
		if (n >= hp->shd) {
			VSLb(hp->vsl, SLT_LostHeader, "Too many headers: %.*s",
			    (int)(len > 20 ? 20 : len), b);
			return (H2SE_ENHANCE_YOUR_CALM);
		}
		hp->nhd++;
	}

	hp->hd[n].b = b;
	hp->hd[n].e = b + len;

	if (disallow_empty && !Tlen(hp->hd[n])) {
		VSLb(hp->vsl, SLT_BogoHeader,
		    "Empty pseudo-header %.*s",
		    (int)namelen, b0);
		return (H2SE_PROTOCOL_ERROR);
	}

	return (0);
}

void
h2h_decode_init(const struct h2_sess *h2)
{
	struct h2h_decode *d;

	CHECK_OBJ_NOTNULL(h2, H2_SESS_MAGIC);
	CHECK_OBJ_NOTNULL(h2->new_req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(h2->new_req->http, HTTP_MAGIC);
	AN(h2->decode);
	d = h2->decode;
	INIT_OBJ(d, H2H_DECODE_MAGIC);
	VHD_Init(d->vhd);
	d->out_l = WS_ReserveAll(h2->new_req->http->ws);
	/*
	 * Can't do any work without any buffer
	 * space. Require non-zero size.
	 */
	XXXAN(d->out_l);
	d->out = h2->new_req->http->ws->f;
	d->reset = d->out;
}

/* Possible error returns:
 *
 * H2E_COMPRESSION_ERROR: Lost compression state due to incomplete header
 * block. This is a connection level error.
 *
 * H2E_ENHANCE_YOUR_CALM: Ran out of workspace or http header space. This
 * is a stream level error.
 */
h2_error
h2h_decode_fini(const struct h2_sess *h2)
{
	h2_error ret;
	struct h2h_decode *d;

	CHECK_OBJ_NOTNULL(h2, H2_SESS_MAGIC);
	d = h2->decode;
	CHECK_OBJ_NOTNULL(h2->new_req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(d, H2H_DECODE_MAGIC);
	WS_ReleaseP(h2->new_req->http->ws, d->out);
	if (d->vhd_ret != VHD_OK) {
		/* HPACK header block didn't finish at an instruction
		   boundary */
		VSLb(h2->new_req->http->vsl, SLT_BogoHeader,
		    "HPACK compression error/fini (%s)", VHD_Error(d->vhd_ret));
		ret = H2CE_COMPRESSION_ERROR;
	} else if (d->error == NULL && !d->has_scheme) {
		VSLb(h2->vsl, SLT_Debug, "Missing :scheme");
		ret = H2SE_MISSING_SCHEME; //rfc7540,l,3087,3090
	} else
		ret = d->error;
	d->magic = 0;
	return (ret);
}

/* Possible error returns:
 *
 * H2E_COMPRESSION_ERROR: Lost compression state due to invalid header
 * block. This is a connection level error.
 *
 * H2E_PROTOCOL_ERROR: Malformed header or duplicate pseudo-header.
 *		       Violation of field name/value charsets
 */
h2_error
h2h_decode_bytes(struct h2_sess *h2, const uint8_t *in, size_t in_l)
{
	struct http *hp;
	struct h2h_decode *d;
	size_t in_u = 0;

	CHECK_OBJ_NOTNULL(h2, H2_SESS_MAGIC);
	CHECK_OBJ_NOTNULL(h2->new_req, REQ_MAGIC);
	hp = h2->new_req->http;
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(hp->ws, WS_MAGIC);
	AN(hp->ws->r);
	d = h2->decode;
	CHECK_OBJ_NOTNULL(d, H2H_DECODE_MAGIC);

	/* Only H2E_ENHANCE_YOUR_CALM indicates that we should continue
	   processing. Other errors should have been returned and handled
	   by the caller. */
	assert(d->error == 0 || d->error == H2SE_ENHANCE_YOUR_CALM);

	while (1) {
		AN(d->out);
		assert(d->out_u <= d->out_l);
		d->vhd_ret = VHD_Decode(d->vhd, h2->dectbl, in, in_l, &in_u,
		    d->out, d->out_l, &d->out_u);

		if (d->vhd_ret < 0) {
			VSLb(hp->vsl, SLT_BogoHeader,
			    "HPACK compression error (%s)",
			    VHD_Error(d->vhd_ret));
			d->error = H2CE_COMPRESSION_ERROR;
			break;
		} else if (d->vhd_ret == VHD_OK || d->vhd_ret == VHD_MORE) {
			assert(in_u == in_l);
			break;
		}

		if (d->error == H2SE_ENHANCE_YOUR_CALM) {
			d->out_u = 0;
			assert(d->out_u < d->out_l);
			continue;
		}

		switch (d->vhd_ret) {
		case VHD_NAME_SEC:
			/* XXX: header flag for never-indexed header */
		case VHD_NAME:
			assert(d->namelen == 0);
			if (d->out_l - d->out_u < 2) {
				d->error = H2SE_ENHANCE_YOUR_CALM;
				break;
			}
			d->out[d->out_u++] = ':';
			d->out[d->out_u++] = ' ';
			d->namelen = d->out_u;
			break;

		case VHD_VALUE_SEC:
			/* XXX: header flag for never-indexed header */
		case VHD_VALUE:
			assert(d->namelen > 0);
			if (d->out_l - d->out_u < 1) {
				d->error = H2SE_ENHANCE_YOUR_CALM;
				break;
			}
			d->error = h2h_checkhdr(hp, d->out, d->namelen,
			    d->out_u);
			if (d->error)
				break;
			d->error = h2h_addhdr(d, hp, d->out,
			    d->namelen, d->out_u);
			if (d->error)
				break;
			d->out[d->out_u++] = '\0'; /* Zero guard */
			d->out += d->out_u;
			d->out_l -= d->out_u;
			d->out_u = 0;
			d->namelen = 0;
			break;

		case VHD_BUF:
			d->error = H2SE_ENHANCE_YOUR_CALM;
			break;

		default:
			WRONG("Unhandled return value");
			break;
		}

		if (d->error == H2SE_ENHANCE_YOUR_CALM) {
			d->out = d->reset;
			d->out_l = hp->ws->r - d->out;
			d->out_u = 0;
			assert(d->out_u < d->out_l);
		} else if (d->error)
			break;
	}

	if (d->error == H2SE_ENHANCE_YOUR_CALM)
		return (0); /* Stream error, delay reporting until
			       h2h_decode_fini so that we can process the
			       complete header block */
	return (d->error);
}
