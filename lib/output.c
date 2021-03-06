/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010-2017 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"

static int
lws_0405_frame_mask_generate(struct lws *wsi)
{
	int n;
	/* fetch the per-frame nonce */

	n = lws_get_random(lws_get_context(wsi), wsi->ws->mask, 4);
	if (n != 4) {
		lwsl_parser("Unable to read from random device %s %d\n",
			    SYSTEM_RANDOM_FILEPATH, n);
		return 1;
	}

	/* start masking from first byte of masking key buffer */
	wsi->ws->mask_idx = 0;

	return 0;
}

/*
 * notice this returns number of bytes consumed, or -1
 */
int lws_issue_raw(struct lws *wsi, unsigned char *buf, size_t len)
{
	struct lws_context *context = lws_get_context(wsi);
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	size_t real_len = len;
	unsigned int n;
#if !defined(LWS_WITHOUT_EXTENSIONS)
	int m;
#endif

	/*
	 * Detect if we got called twice without going through the
	 * event loop to handle pending.  This would be caused by either
	 * back-to-back writes in one WRITABLE (illegal) or calling lws_write()
	 * from outside the WRITABLE callback (illegal).
	 */
	if (wsi->could_have_pending) {
		lwsl_hexdump_level(LLL_ERR, buf, len);
		lwsl_err("** %p: vh: %s, prot: %s, "
			 "Illegal back-to-back write of %lu detected...\n",
			 wsi, wsi->vhost->name, wsi->protocol->name,
			 (unsigned long)len);
		// assert(0);

		return -1;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_API_WRITE, 1);

	if (!len)
		return 0;
	/* just ignore sends after we cleared the truncation buffer */
	if (wsi->state == LWSS_FLUSHING_SEND_BEFORE_CLOSE &&
	    !wsi->trunc_len)
		return (int)len;

	if (wsi->trunc_len && (buf < wsi->trunc_alloc ||
	    buf > (wsi->trunc_alloc + wsi->trunc_len + wsi->trunc_offset))) {
		lwsl_hexdump_level(LLL_ERR, buf, len);
		lwsl_err("** %p: vh: %s, prot: %s, Sending new %lu, pending truncated ...\n"
			 "   It's illegal to do an lws_write outside of\n"
			 "   the writable callback: fix your code\n",
			 wsi, wsi->vhost->name, wsi->protocol->name,
			 (unsigned long)len);
		assert(0);

		return -1;
	}
#if !defined(LWS_WITHOUT_EXTENSIONS)
	m = lws_ext_cb_active(wsi, LWS_EXT_CB_PACKET_TX_DO_SEND, &buf, (int)len);
	if (m < 0)
		return -1;
	if (m) /* handled */ {
		n = m;
		goto handle_truncated_send;
	}
#endif
	if (!wsi->http2_substream && !lws_socket_is_valid(wsi->desc.sockfd))
		lwsl_warn("** error invalid sock but expected to send\n");

	/* limit sending */
	if (wsi->protocol->tx_packet_size)
		n = (int)wsi->protocol->tx_packet_size;
	else {
		n = (int)wsi->protocol->rx_buffer_size;
		if (!n)
			n = context->pt_serv_buf_size;
	}
	n += LWS_PRE + 4;
	if (n > len)
		n = (int)len;

	/* nope, send it on the socket directly */
	lws_latency_pre(context, wsi);
	n = lws_ssl_capable_write(wsi, buf, n);
	lws_latency(context, wsi, "send lws_issue_raw", n, n == len);

	/* something got written, it can have been truncated now */
	wsi->could_have_pending = 1;

	switch (n) {
	case LWS_SSL_CAPABLE_ERROR:
		/* we're going to close, let close know sends aren't possible */
		wsi->socket_is_permanently_unusable = 1;
		return -1;
	case LWS_SSL_CAPABLE_MORE_SERVICE:
		/*
		 * nothing got sent, not fatal.  Retry the whole thing later,
		 * ie, implying treat it was a truncated send so it gets
		 * retried
		 */
		n = 0;
		break;
	}
#if !defined(LWS_WITHOUT_EXTENSIONS)
handle_truncated_send:
#endif
	/*
	 * we were already handling a truncated send?
	 */
	if (wsi->trunc_len) {
		lwsl_info("%p partial adv %d (vs %ld)\n", wsi, n, (long)real_len);
		wsi->trunc_offset += n;
		wsi->trunc_len -= n;

		if (!wsi->trunc_len) {
			lwsl_info("** %p partial send completed\n", wsi);
			/* done with it, but don't free it */
			n = (int)real_len;
			if (wsi->state == LWSS_FLUSHING_SEND_BEFORE_CLOSE) {
				lwsl_info("** %p signalling to close now\n", wsi);
				return -1; /* retry closing now */
			}
		}
		/* always callback on writeable */
		lws_callback_on_writable(wsi);

		return n;
	}

	if ((unsigned int)n == real_len)
		/* what we just sent went out cleanly */
		return n;

	/*
	 * Newly truncated send.  Buffer the remainder (it will get
	 * first priority next time the socket is writable).
	 */
	lwsl_debug("%p new partial sent %d from %lu total\n", wsi, n,
		    (unsigned long)real_len);

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_WRITE_PARTIALS, 1);
	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_B_PARTIALS_ACCEPTED_PARTS, n);

	/*
	 *  - if we still have a suitable malloc lying around, use it
	 *  - or, if too small, reallocate it
	 *  - or, if no buffer, create it
	 */
	if (!wsi->trunc_alloc || real_len - n > wsi->trunc_alloc_len) {
		lws_free(wsi->trunc_alloc);

		wsi->trunc_alloc_len = (unsigned int)(real_len - n);
		wsi->trunc_alloc = lws_malloc(real_len - n,
					      "truncated send alloc");
		if (!wsi->trunc_alloc) {
			lwsl_err("truncated send: unable to malloc %lu\n",
				 (unsigned long)(real_len - n));
			return -1;
		}
	}
	wsi->trunc_offset = 0;
	wsi->trunc_len = (unsigned int)(real_len - n);
	memcpy(wsi->trunc_alloc, buf + n, real_len - n);

	/* since something buffered, force it to get another chance to send */
	lws_callback_on_writable(wsi);

	return (int)real_len;
}

LWS_VISIBLE int lws_write(struct lws *wsi, unsigned char *buf, size_t len,
			  enum lws_write_protocol wp)
{
	struct lws_context_per_thread *pt = &wsi->context->pt[(int)wsi->tsi];
	int masked7 = (wsi->mode == LWSCM_WS_CLIENT);
	unsigned char is_masked_bit = 0;
	unsigned char *dropmask = NULL;
	struct lws_tokens eff_buf;
	size_t orig_len = len;
	int pre = 0, n, wp1f = wp & 0x1f;

	if (wsi->parent_carries_io) {
		struct lws_write_passthru pas;

		pas.buf = buf;
		pas.len = len;
		pas.wp = wp;
		pas.wsi = wsi;

		if (wsi->parent->protocol->callback(wsi->parent,
				LWS_CALLBACK_CHILD_WRITE_VIA_PARENT,
				wsi->parent->user_space,
				(void *)&pas, 0))
			return 1;

		return (int)len;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_C_API_LWS_WRITE, 1);

	if ((int)len < 0) {
		lwsl_err("%s: suspicious len int %d, ulong %lu\n", __func__,
				(int)len, (unsigned long)len);
		return -1;
	}

	lws_stats_atomic_bump(wsi->context, pt, LWSSTATS_B_WRITE, len);

#ifdef LWS_WITH_ACCESS_LOG
	wsi->access_log.sent += len;
#endif
	if (wsi->vhost)
		wsi->vhost->conn_stats.tx += len;

	if (wsi->ws && wsi->ws->tx_draining_ext && lws_state_is_ws(wsi->state)) {
		/* remove us from the list */
		struct lws **w = &pt->tx_draining_ext_list;

		wsi->ws->tx_draining_ext = 0;
		/* remove us from context draining ext list */
		while (*w) {
			if (*w == wsi) {
				*w = wsi->ws->tx_draining_ext_list;
				break;
			}
			w = &((*w)->ws->tx_draining_ext_list);
		}
		wsi->ws->tx_draining_ext_list = NULL;
		wp = (wsi->ws->tx_draining_stashed_wp & 0xc0) |
				LWS_WRITE_CONTINUATION;
		wp1f = wp & 0x1f;

		lwsl_ext("FORCED draining wp to 0x%02X\n", wp);
	}

	lws_restart_ws_ping_pong_timer(wsi);

	if (wp1f == LWS_WRITE_HTTP ||
	    wp1f == LWS_WRITE_HTTP_FINAL ||
	    wp1f == LWS_WRITE_HTTP_HEADERS_CONTINUATION ||
	    wp1f == LWS_WRITE_HTTP_HEADERS)
		goto send_raw;

	/* if not in a state to send ws stuff, then just send nothing */

	if (!lws_state_is_ws(wsi->state) &&
	    ((wsi->state != LWSS_RETURNED_CLOSE_ALREADY &&
	      wsi->state != LWSS_WAITING_TO_SEND_CLOSE_NOTIFICATION &&
	      wsi->state != LWSS_AWAITING_CLOSE_ACK) ||
			    wp1f != LWS_WRITE_CLOSE)) {
		//assert(0);
		lwsl_debug("binning %d %d\n", wsi->state, wp1f);
		return 0;
	}

	/* if we are continuing a frame that already had its header done */

	if (wsi->ws->inside_frame) {
		lwsl_debug("INSIDE FRAME\n");
		goto do_more_inside_frame;
	}

	wsi->ws->clean_buffer = 1;

	/*
	 * give a chance to the extensions to modify payload
	 * the extension may decide to produce unlimited payload erratically
	 * (eg, compression extension), so we require only that if he produces
	 * something, it will be a complete fragment of the length known at
	 * the time (just the fragment length known), and if he has
	 * more we will come back next time he is writeable and allow him to
	 * produce more fragments until he's drained.
	 *
	 * This allows what is sent each time it is writeable to be limited to
	 * a size that can be sent without partial sends or blocking, allows
	 * interleaving of control frames and other connection service.
	 */
	eff_buf.token = (char *)buf;
	eff_buf.token_len = (int)len;

	switch ((int)wp) {
	case LWS_WRITE_PING:
	case LWS_WRITE_PONG:
	case LWS_WRITE_CLOSE:
		break;
	default:
#if !defined(LWS_WITHOUT_EXTENSIONS)
		lwsl_debug("LWS_EXT_CB_PAYLOAD_TX\n");
		n = lws_ext_cb_active(wsi, LWS_EXT_CB_PAYLOAD_TX, &eff_buf, wp);
		if (n < 0)
			return -1;

		if (n && eff_buf.token_len) {
			lwsl_debug("drain len %d\n", (int)eff_buf.token_len);
			/* extension requires further draining */
			wsi->ws->tx_draining_ext = 1;
			wsi->ws->tx_draining_ext_list =
					pt->tx_draining_ext_list;
			pt->tx_draining_ext_list = wsi;
			/* we must come back to do more */
			lws_callback_on_writable(wsi);
			/*
			 * keep a copy of the write type for the overall
			 * action that has provoked generation of these
			 * fragments, so the last guy can use its FIN state.
			 */
			wsi->ws->tx_draining_stashed_wp = wp;
			/* this is definitely not actually the last fragment
			 * because the extension asserted he has more coming
			 * So make sure this intermediate one doesn't go out
			 * with a FIN.
			 */
			wp |= LWS_WRITE_NO_FIN;
		}
#endif
		if (eff_buf.token_len && wsi->ws->stashed_write_pending) {
			wsi->ws->stashed_write_pending = 0;
			wp = (wp &0xc0) | (int)wsi->ws->stashed_write_type;
			wp1f = wp & 0x1f;
		}
	}

	/*
	 * an extension did something we need to keep... for example, if
	 * compression extension, it has already updated its state according
	 * to this being issued
	 */
	if ((char *)buf != eff_buf.token) {
		/*
		 * ext might eat it, but not have anything to issue yet.
		 * In that case we have to follow his lead, but stash and
		 * replace the write type that was lost here the first time.
		 */
		if (len && !eff_buf.token_len) {
			if (!wsi->ws->stashed_write_pending)
				wsi->ws->stashed_write_type = (char)wp & 0x3f;
			wsi->ws->stashed_write_pending = 1;
			return (int)len;
		}
		/*
		 * extension recreated it:
		 * need to buffer this if not all sent
		 */
		wsi->ws->clean_buffer = 0;
	}

	buf = (unsigned char *)eff_buf.token;
	len = eff_buf.token_len;

	if (!buf) {
		lwsl_err("null buf (%d)\n", (int)len);
		return -1;
	}

	switch (wsi->ws->ietf_spec_revision) {
	case 13:
		if (masked7) {
			pre += 4;
			dropmask = &buf[0 - pre];
			is_masked_bit = 0x80;
		}

		switch (wp & 0xf) {
		case LWS_WRITE_TEXT:
			n = LWSWSOPC_TEXT_FRAME;
			break;
		case LWS_WRITE_BINARY:
			n = LWSWSOPC_BINARY_FRAME;
			break;
		case LWS_WRITE_CONTINUATION:
			n = LWSWSOPC_CONTINUATION;
			break;

		case LWS_WRITE_CLOSE:
			n = LWSWSOPC_CLOSE;
			break;
		case LWS_WRITE_PING:
			n = LWSWSOPC_PING;
			break;
		case LWS_WRITE_PONG:
			n = LWSWSOPC_PONG;
			break;
		default:
			lwsl_warn("lws_write: unknown write opc / wp\n");
			return -1;
		}

		if (!(wp & LWS_WRITE_NO_FIN))
			n |= 1 << 7;

		if (len < 126) {
			pre += 2;
			buf[-pre] = n;
			buf[-pre + 1] = (unsigned char)(len | is_masked_bit);
		} else {
			if (len < 65536) {
				pre += 4;
				buf[-pre] = n;
				buf[-pre + 1] = 126 | is_masked_bit;
				buf[-pre + 2] = (unsigned char)(len >> 8);
				buf[-pre + 3] = (unsigned char)len;
			} else {
				pre += 10;
				buf[-pre] = n;
				buf[-pre + 1] = 127 | is_masked_bit;
#if defined __LP64__
					buf[-pre + 2] = (len >> 56) & 0x7f;
					buf[-pre + 3] = len >> 48;
					buf[-pre + 4] = len >> 40;
					buf[-pre + 5] = len >> 32;
#else
					buf[-pre + 2] = 0;
					buf[-pre + 3] = 0;
					buf[-pre + 4] = 0;
					buf[-pre + 5] = 0;
#endif
				buf[-pre + 6] = (unsigned char)(len >> 24);
				buf[-pre + 7] = (unsigned char)(len >> 16);
				buf[-pre + 8] = (unsigned char)(len >> 8);
				buf[-pre + 9] = (unsigned char)len;
			}
		}
		break;
	}

do_more_inside_frame:

	/*
	 * Deal with masking if we are in client -> server direction and
	 * the wp demands it
	 */

	if (masked7) {
		if (!wsi->ws->inside_frame)
			if (lws_0405_frame_mask_generate(wsi)) {
				lwsl_err("frame mask generation failed\n");
				return -1;
			}

		/*
		 * in v7, just mask the payload
		 */
		if (dropmask) { /* never set if already inside frame */
			for (n = 4; n < (int)len + 4; n++)
				dropmask[n] = dropmask[n] ^ wsi->ws->mask[
					(wsi->ws->mask_idx++) & 3];

			/* copy the frame nonce into place */
			memcpy(dropmask, wsi->ws->mask, 4);
		}
	}

send_raw:
	switch (wp1f) {
	case LWS_WRITE_TEXT:
	case LWS_WRITE_BINARY:
	case LWS_WRITE_CONTINUATION:
		if (!wsi->h2_stream_carries_ws)
			break;
		/* fallthru */
	case LWS_WRITE_CLOSE:
/*		lwsl_hexdump(&buf[-pre], len); */
	case LWS_WRITE_HTTP:
	case LWS_WRITE_HTTP_FINAL:
	case LWS_WRITE_HTTP_HEADERS:
	case LWS_WRITE_HTTP_HEADERS_CONTINUATION:
	case LWS_WRITE_PONG:
	case LWS_WRITE_PING:
#ifdef LWS_WITH_HTTP2
		/*
		 * ws-over-h2 ends up here after the ws framing applied
		 */
		if (wsi->mode == LWSCM_HTTP2_SERVING ||
		    wsi->mode == LWSCM_HTTP2_WS_SERVING) {
			unsigned char flags = 0;

			n = LWS_H2_FRAME_TYPE_DATA;
			if (wp1f == LWS_WRITE_HTTP_HEADERS) {
				n = LWS_H2_FRAME_TYPE_HEADERS;
				if (!(wp & LWS_WRITE_NO_FIN))
					flags = LWS_H2_FLAG_END_HEADERS;
				if (wsi->h2.send_END_STREAM ||
				    (wp & LWS_WRITE_H2_STREAM_END)) {
					flags |= LWS_H2_FLAG_END_STREAM;
					wsi->h2.send_END_STREAM = 1;
				}
			}

			if (wp1f == LWS_WRITE_HTTP_HEADERS_CONTINUATION) {
				n = LWS_H2_FRAME_TYPE_CONTINUATION;
				if (!(wp & LWS_WRITE_NO_FIN))
					flags = LWS_H2_FLAG_END_HEADERS;
				if (wsi->h2.send_END_STREAM ||
				    (wp & LWS_WRITE_H2_STREAM_END)) {
					flags |= LWS_H2_FLAG_END_STREAM;
					wsi->h2.send_END_STREAM = 1;
				}
			}

			if ((wp1f == LWS_WRITE_HTTP ||
			     wp1f == LWS_WRITE_HTTP_FINAL) &&
			    wsi->http.tx_content_length) {
				wsi->http.tx_content_remain -= len;
				lwsl_info("%s: wsi %p: tx_content_remain = %llu\n",
					  __func__, wsi,
					  (unsigned long long)wsi->http.tx_content_remain);
				if (!wsi->http.tx_content_remain) {
					lwsl_info("%s: selecting final write mode\n",
						  __func__);
					wp = LWS_WRITE_HTTP_FINAL;
					wp1f = wp & 0x1f;
				}
			}

			if (wp1f == LWS_WRITE_HTTP_FINAL ||
			    (wp & LWS_WRITE_H2_STREAM_END)) {
			    //lws_get_network_wsi(wsi)->h2.END_STREAM) {
				lwsl_info("%s: setting END_STREAM\n", __func__);
				flags |= LWS_H2_FLAG_END_STREAM;
				wsi->h2.send_END_STREAM = 1;
			}

			/* if any ws framing, account for that too */
			return lws_h2_frame_write(wsi, n, flags, wsi->h2.my_sid,
						  (int)len + pre, buf - pre);
		}
#endif
		return lws_issue_raw(wsi, (unsigned char *)buf - pre, len + pre);
	default:
		break;
	}

	/*
	 * give any active extensions a chance to munge the buffer
	 * before send.  We pass in a pointer to an lws_tokens struct
	 * prepared with the default buffer and content length that's in
	 * there.  Rather than rewrite the default buffer, extensions
	 * that expect to grow the buffer can adapt .token to
	 * point to their own per-connection buffer in the extension
	 * user allocation.  By default with no extensions or no
	 * extension callback handling, just the normal input buffer is
	 * used then so it is efficient.
	 *
	 * callback returns 1 in case it wants to spill more buffers
	 *
	 * This takes care of holding the buffer if send is incomplete, ie,
	 * if wsi->ws->clean_buffer is 0 (meaning an extension meddled with
	 * the buffer).  If wsi->ws->clean_buffer is 1, it will instead
	 * return to the user code how much OF THE USER BUFFER was consumed.
	 */

	n = lws_issue_raw_ext_access(wsi, buf - pre, len + pre);
	wsi->ws->inside_frame = 1;
	if (n <= 0)
		return n;

	if (n == (int)len + pre) {
		/* everything in the buffer was handled (or rebuffered...) */
		wsi->ws->inside_frame = 0;
		return (int)orig_len;
	}

	/*
	 * it is how many bytes of user buffer got sent... may be < orig_len
	 * in which case callback when writable has already been arranged
	 * and user code can call lws_write() again with the rest
	 * later.
	 */

	return n - pre;
}

LWS_VISIBLE int lws_serve_http_file_fragment(struct lws *wsi)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	struct lws_process_html_args args;
	lws_filepos_t amount, poss;
	unsigned char *p, *pstart;
#if defined(LWS_WITH_RANGES)
	unsigned char finished = 0;
#endif
	int n, m;

	lwsl_debug("wsi->http2_substream %d\n", wsi->http2_substream);

	while (!lws_send_pipe_choked(wsi)) {

		if (wsi->trunc_len) {
			if (lws_issue_raw(wsi, wsi->trunc_alloc +
					  wsi->trunc_offset,
					  wsi->trunc_len) < 0) {
				lwsl_info("%s: closing\n", __func__);
				goto file_had_it;
			}
			continue;
		}

		if (wsi->http.filepos == wsi->http.filelen)
			goto all_sent;

		n = 0;

		pstart = pt->serv_buf + LWS_H2_FRAME_HEADER_LENGTH;

		p = pstart;

#if defined(LWS_WITH_RANGES)
		if (wsi->http.range.count_ranges && !wsi->http.range.inside) {

			lwsl_notice("%s: doing range start %llu\n", __func__,
				    wsi->http.range.start);

			if ((long long)lws_vfs_file_seek_cur(wsi->http.fop_fd,
						   wsi->http.range.start -
						   wsi->http.filepos) < 0)
				goto file_had_it;

			wsi->http.filepos = wsi->http.range.start;

			if (wsi->http.range.count_ranges > 1) {
				n =  lws_snprintf((char *)p,
						context->pt_serv_buf_size -
						LWS_H2_FRAME_HEADER_LENGTH,
					"_lws\x0d\x0a"
					"Content-Type: %s\x0d\x0a"
					"Content-Range: bytes %llu-%llu/%llu\x0d\x0a"
					"\x0d\x0a",
					wsi->http.multipart_content_type,
					wsi->http.range.start,
					wsi->http.range.end,
					wsi->http.range.extent);
				p += n;
			}

			wsi->http.range.budget = wsi->http.range.end -
						   wsi->http.range.start + 1;
			wsi->http.range.inside = 1;
		}
#endif

		poss = context->pt_serv_buf_size - n - LWS_H2_FRAME_HEADER_LENGTH;

		if (wsi->http.tx_content_length)
			if (poss > wsi->http.tx_content_remain)
				poss = wsi->http.tx_content_remain;

		/*
		 * if there is a hint about how much we will do well to send at one time,
		 * restrict ourselves to only trying to send that.
		 */
		if (wsi->protocol->tx_packet_size &&
		    poss > wsi->protocol->tx_packet_size)
			poss = wsi->protocol->tx_packet_size;

#if defined(LWS_WITH_HTTP2)
		m = lws_h2_tx_cr_get(wsi);
		if (!m) {
			lwsl_info("%s: came here with no tx credit", __func__);
			return 0;
		}
		if ((lws_filepos_t)m < poss)
			poss = m;
		/*
		 * consumption of the actual payload amount sent will be handled
		 * when the http2 data frame is sent
		 */
#endif

#if defined(LWS_WITH_RANGES)
		if (wsi->http.range.count_ranges) {
			if (wsi->http.range.count_ranges > 1)
				poss -= 7; /* allow for final boundary */
			if (poss > wsi->http.range.budget)
				poss = wsi->http.range.budget;
		}
#endif
		if (wsi->sending_chunked) {
			/* we need to drop the chunk size in here */
			p += 10;
			/* allow for the chunk to grow by 128 in translation */
			poss -= 10 + 128;
		}

		if (lws_vfs_file_read(wsi->http.fop_fd, &amount, p, poss) < 0)
			goto file_had_it; /* caller will close */

		if (wsi->sending_chunked)
			n = (int)amount;
		else
			n = lws_ptr_diff(p, pstart) + (int)amount;

		lwsl_debug("%s: sending %d\n", __func__, n);

		if (n) {
			lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT,
					context->timeout_secs);

			if (wsi->interpreting) {
				args.p = (char *)p;
				args.len = n;
				args.max_len = (unsigned int)poss + 128;
				args.final = wsi->http.filepos + n ==
					     wsi->http.filelen;
				args.chunked = wsi->sending_chunked;
				if (user_callback_handle_rxflow(
				     wsi->vhost->protocols[
				     (int)wsi->protocol_interpret_idx].callback,
				     wsi, LWS_CALLBACK_PROCESS_HTML,
				     wsi->user_space, &args, 0) < 0)
					goto file_had_it;
				n = args.len;
				p = (unsigned char *)args.p;
			} else
				p = pstart;

#if defined(LWS_WITH_RANGES)
			if (wsi->http.range.send_ctr + 1 ==
				wsi->http.range.count_ranges && // last range
			    wsi->http.range.count_ranges > 1 && // was 2+ ranges (ie, multipart)
			    wsi->http.range.budget - amount == 0) {// final part
				n += lws_snprintf((char *)pstart + n, 6,
					"_lws\x0d\x0a"); // append trailing boundary
				lwsl_debug("added trailing boundary\n");
			}
#endif
			m = lws_write(wsi, p, n,
				      wsi->http.filepos + amount == wsi->http.filelen ?
					LWS_WRITE_HTTP_FINAL :
					LWS_WRITE_HTTP
				);
			if (m < 0)
				goto file_had_it;

			wsi->http.filepos += amount;

#if defined(LWS_WITH_RANGES)
			if (wsi->http.range.count_ranges >= 1) {
				wsi->http.range.budget -= amount;
				if (wsi->http.range.budget == 0) {
					lwsl_notice("range budget exhausted\n");
					wsi->http.range.inside = 0;
					wsi->http.range.send_ctr++;

					if (lws_ranges_next(&wsi->http.range) < 1) {
						finished = 1;
						goto all_sent;
					}
				}
			}
#endif

			if (m != n) {
				/* adjust for what was not sent */
				if (lws_vfs_file_seek_cur(wsi->http.fop_fd,
							   m - n) ==
							     (lws_fileofs_t)-1)
					goto file_had_it;
			}
		}

all_sent:
		if ((!wsi->trunc_len && wsi->http.filepos >= wsi->http.filelen)
#if defined(LWS_WITH_RANGES)
		    || finished)
#else
		)
#endif
		     {
			wsi->state = LWSS_HTTP;
			/* we might be in keepalive, so close it off here */
			lws_vfs_file_close(&wsi->http.fop_fd);
			
			lwsl_debug("file completed\n");

			if (wsi->protocol->callback &&
			    user_callback_handle_rxflow(wsi->protocol->callback,
					    	    	wsi, LWS_CALLBACK_HTTP_FILE_COMPLETION,
					    	    	wsi->user_space, NULL,
					    	    	0) < 0) {
					/*
					 * For http/1.x, the choices from
					 * transaction_completed are either
					 * 0 to use the connection for pipelined
					 * or nonzero to hang it up.
					 *
					 * However for http/2. while we are
					 * still interested in hanging up the
					 * nwsi if there was a network-level
					 * fatal error, simply completing the
					 * transaction is a matter of the stream
					 * state, not the root connection at the
					 * network level
					 */
					if (wsi->http2_substream)
						return 1;
					else
						return -1;
				}

			return 1;  /* >0 indicates completed */
		}
	}

	lws_callback_on_writable(wsi);

	return 0; /* indicates further processing must be done */

file_had_it:
	lws_vfs_file_close(&wsi->http.fop_fd);

	return -1;
}

#if LWS_POSIX
LWS_VISIBLE int
lws_ssl_capable_read_no_ssl(struct lws *wsi, unsigned char *buf, int len)
{
	struct lws_context *context = wsi->context;
	struct lws_context_per_thread *pt = &context->pt[(int)wsi->tsi];
	int n;

	lws_stats_atomic_bump(context, pt, LWSSTATS_C_API_READ, 1);

	n = recv(wsi->desc.sockfd, (char *)buf, len, 0);
	if (n >= 0) {
		if (wsi->vhost)
			wsi->vhost->conn_stats.rx += n;
		lws_stats_atomic_bump(context, pt, LWSSTATS_B_READ, n);
		lws_restart_ws_ping_pong_timer(wsi);
		return n;
	}
#if LWS_POSIX
	if (LWS_ERRNO == LWS_EAGAIN ||
	    LWS_ERRNO == LWS_EWOULDBLOCK ||
	    LWS_ERRNO == LWS_EINTR)
		return LWS_SSL_CAPABLE_MORE_SERVICE;
#endif
	lwsl_notice("error on reading from skt : %d\n", LWS_ERRNO);
	return LWS_SSL_CAPABLE_ERROR;
}

LWS_VISIBLE int
lws_ssl_capable_write_no_ssl(struct lws *wsi, unsigned char *buf, int len)
{
	int n = 0;

#if LWS_POSIX
	n = send(wsi->desc.sockfd, (char *)buf, len, MSG_NOSIGNAL);
//	lwsl_info("%s: sent len %d result %d", __func__, len, n);
	if (n >= 0)
		return n;

	if (LWS_ERRNO == LWS_EAGAIN ||
	    LWS_ERRNO == LWS_EWOULDBLOCK ||
	    LWS_ERRNO == LWS_EINTR) {
		if (LWS_ERRNO == LWS_EWOULDBLOCK) {
			lws_set_blocking_send(wsi);
		}

		return LWS_SSL_CAPABLE_MORE_SERVICE;
	}
#else
	(void)n;
	(void)wsi;
	(void)buf;
	(void)len;
	// !!!
#endif

	lwsl_debug("ERROR writing len %d to skt fd %d err %d / errno %d\n",
			len, wsi->desc.sockfd, n, LWS_ERRNO);

	return LWS_SSL_CAPABLE_ERROR;
}
#endif
LWS_VISIBLE int
lws_ssl_pending_no_ssl(struct lws *wsi)
{
	(void)wsi;
#if defined(LWS_WITH_ESP32)
	return 100;
#else
	return 0;
#endif
}
