/*
 * Copyright (c) 2018 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifdef __APPLE__
#define __APPLE_USE_RFC_3542 /* to use IPV6_PKTINFO */
#endif
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "picotls/openssl.h"
#include "h2o/string_.h"
#include "h2o/http3_common.h"
#include "h2o/http3_internal.h"
#include "h2o/multithread.h"

struct st_h2o_http3_ingress_unistream_t {
    /**
     * back pointer
     */
    quicly_stream_t *quic;
    /**
     *
     */
    h2o_buffer_t *recvbuf;
    /**
     *
     */
    int (*handle_input)(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                        const uint8_t *src_end, const char **err_desc);
};

/**
 * maximum payload size excluding DATA frame; stream receive window MUST be at least as big as this
 */
#define MAX_FRAME_SIZE 16384

const ptls_iovec_t h2o_http3_alpn[1] = {{(void *)H2O_STRLIT("h3-23")}};

/**
 * Sends a packet, returns if the connection is still maintainable (false is returned when not being able to send a packet from the
 * designated source address).
 */
int h2o_http3_send_datagram(h2o_http3_ctx_t *ctx, quicly_datagram_t *p)
{
    int ret;
    struct msghdr mess;
    struct iovec vec;
    union {
        struct cmsghdr hdr;
        char buf[
#ifdef IPV6_PKTINFO
            CMSG_SPACE(sizeof(struct in6_pktinfo))
#elif defined(IP_PKTINFO)
            CMSG_SPACE(sizeof(struct in_pktinfo))
#else
            CMSG_SPACE(1)
#endif
        ];
    } cmsg;

    memset(&mess, 0, sizeof(mess));
    mess.msg_name = &p->dest.sa;
    mess.msg_namelen = quicly_get_socklen(&p->dest.sa);
    vec.iov_base = p->data.base;
    vec.iov_len = p->data.len;
    mess.msg_iov = &vec;
    mess.msg_iovlen = 1;
    if (p->src.sa.sa_family != AF_UNSPEC) {
        size_t cmsg_bodylen = 0;
        memset(&cmsg, 0, sizeof(cmsg));
        switch (p->src.sa.sa_family) {
        case AF_INET:
#ifdef IP_PKTINFO
            if (*ctx->sock.port != p->src.sin.sin_port)
                return 0;
            cmsg.hdr.cmsg_level = IPPROTO_IP;
            cmsg.hdr.cmsg_type = IP_PKTINFO;
            cmsg_bodylen = sizeof(struct in_pktinfo);
            ((struct in_pktinfo *)CMSG_DATA(&cmsg.hdr))->ipi_addr = p->src.sin.sin_addr;
#else
            h2o_fatal("IP_PKTINFO not available");
#endif
            break;
        case AF_INET6:
#ifdef IPV6_PKTINFO
            if (*ctx->sock.port != p->src.sin6.sin6_port)
                return 0;
            cmsg.hdr.cmsg_level = IPPROTO_IPV6;
            cmsg.hdr.cmsg_type = IPV6_PKTINFO;
            cmsg_bodylen = sizeof(struct in6_pktinfo);
            ((struct in6_pktinfo *)CMSG_DATA(&cmsg.hdr))->ipi6_addr = p->src.sin6.sin6_addr;
#else
            h2o_fatal("IPV6_PKTINFO not available");
#endif
            break;
        default:
            h2o_fatal("unexpected address family");
            break;
        }
        mess.msg_control = &cmsg;
        cmsg.hdr.cmsg_len = (socklen_t)CMSG_LEN(cmsg_bodylen);
        mess.msg_controllen = (socklen_t)CMSG_SPACE(cmsg_bodylen);
    }

    while ((ret = (int)sendmsg(h2o_socket_get_fd(ctx->sock.sock), &mess, 0)) == -1 && errno == EINTR)
        ;

    if (ret == -1) {
        /* The UDP stack returns EINVAL (linux) or EADDRNOTAVAIL (darwin, and presumably other BSD) when it was unable to use the
         * designated source address.  We communicate that back to the caller so that the connection can be closed immediately. */
        if (p->src.sa.sa_family != AF_UNSPEC && (errno == EINVAL || errno == EADDRNOTAVAIL))
            return 0;

        /* Temporary failure to send a packet is not a permanent error fo the connection. (TODO do we want do something more
         * specific?) */
        perror("sendmsg failed");
    }

    return 1;
}

static void ingress_unistream_on_destroy(quicly_stream_t *qs, int err)
{
    struct st_h2o_http3_ingress_unistream_t *stream = qs->data;
    h2o_buffer_dispose(&stream->recvbuf);
    free(stream);
}

static int ingress_unistream_on_receive(quicly_stream_t *qs, size_t off, const void *input, size_t len)
{
    h2o_http3_conn_t *conn = *quicly_get_data(qs->conn);
    struct st_h2o_http3_ingress_unistream_t *stream = qs->data;
    int ret;

    /* save received data */
    if ((ret = h2o_http3_update_recvbuf(&stream->recvbuf, off, input, len)) != 0)
        return ret;

    /* respond with fatal error if the stream is closed */
    if (quicly_recvstate_transfer_complete(&stream->quic->recvstate))
        return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;

    /* determine bytes that can be handled */
    const uint8_t *src = (const uint8_t *)stream->recvbuf->bytes,
                  *src_end = src + quicly_recvstate_bytes_available(&stream->quic->recvstate);
    if (src == src_end)
        return 0;

    /* handle the bytes (TODO retain err_desc) */
    const char *err_desc = NULL;
    ret = stream->handle_input(conn, stream, &src, src_end, &err_desc);

    /* remove bytes that have been consumed */
    size_t bytes_consumed = src - (const uint8_t *)stream->recvbuf->bytes;
    if (bytes_consumed != 0) {
        h2o_buffer_consume(&stream->recvbuf, bytes_consumed);
        quicly_stream_sync_recvbuf(stream->quic, bytes_consumed);
    }

    return ret;
}

static int ingress_unistream_on_receive_reset(quicly_stream_t *qs, int err)
{
    return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;
}

static int qpack_encoder_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                             const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    while (*src != src_end) {
        int64_t *unblocked_stream_ids;
        size_t num_unblocked;
        int ret;
        if ((ret = h2o_qpack_decoder_handle_input(conn->qpack.dec, &unblocked_stream_ids, &num_unblocked, src, src_end,
                                                  err_desc)) != 0)
            return ret;
        /* TODO handle unblocked streams */
    }
    return 0;
}

static int qpack_decoder_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                             const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    while (*src != src_end) {
        int ret;
        if ((ret = h2o_qpack_encoder_handle_input(conn->qpack.enc, src, src_end, err_desc)) != 0)
            return ret;
    }
    return 0;
}

static int control_stream_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                                       const uint8_t *src_end, const char **err_desc)
{
    h2o_http3_read_frame_t frame;
    int ret;

    do {
        if ((ret = h2o_http3_read_frame(&frame, quicly_is_client(conn->quic), H2O_HTTP3_STREAM_TYPE_CONTROL, src, src_end,
                                        err_desc)) != 0) {
            if (ret == H2O_HTTP3_ERROR_INCOMPLETE)
                ret = 0;
            break;
        }
        if (h2o_http3_has_received_settings(conn) == (frame.type == H2O_HTTP3_FRAME_TYPE_SETTINGS) ||
            frame.type == H2O_HTTP3_FRAME_TYPE_DATA)
            return H2O_HTTP3_ERROR_FRAME_UNEXPECTED;
        if ((ret = conn->callbacks->handle_control_stream_frame(conn, frame.type, frame.payload, frame.length, err_desc)) != 0)
            break;
    } while (*src != src_end);

    return ret;
}

static int unknown_stream_type_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream,
                                            const uint8_t **src, const uint8_t *src_end, const char **err_desc)
{
    /* just consume the input */
    *src = src_end;
    return 0;
}

static int unknown_type_handle_input(h2o_http3_conn_t *conn, struct st_h2o_http3_ingress_unistream_t *stream, const uint8_t **src,
                                     const uint8_t *src_end, const char **err_desc)
{
    if (*src == src_end) {
        /* a sender is allowed to close or reset a unidirectional stream */
        return 0;
    }

    /* read the type, or just return if incomplete */
    uint64_t type;
    if ((type = quicly_decodev(src, src_end)) == UINT64_MAX)
        return 0;

    switch (type) {
    case H2O_HTTP3_STREAM_TYPE_CONTROL:
        conn->_control_streams.ingress.control = stream;
        stream->handle_input = control_stream_handle_input;
        break;
    case H2O_HTTP3_STREAM_TYPE_QPACK_ENCODER:
        conn->_control_streams.ingress.qpack_encoder = stream;
        stream->handle_input = qpack_encoder_stream_handle_input;
        break;
    case H2O_HTTP3_STREAM_TYPE_QPACK_DECODER:
        conn->_control_streams.ingress.qpack_decoder = stream;
        stream->handle_input = qpack_decoder_stream_handle_input;
        break;
    default:
        quicly_request_stop(stream->quic, H2O_HTTP3_ERROR_STREAM_CREATION);
        stream->handle_input =
            unknown_stream_type_handle_input; /* TODO reconsider quicly API; do we need to read data after sending STOP_SENDING? */
        break;
    }

    return stream->handle_input(conn, stream, src, src_end, err_desc);
}

static void egress_unistream_on_destroy(quicly_stream_t *qs, int err)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;
    h2o_buffer_dispose(&stream->sendbuf);
    free(stream);
}

static void egress_unistream_on_send_shift(quicly_stream_t *qs, size_t delta)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;
    h2o_buffer_consume(&stream->sendbuf, delta);
}

static int egress_unistream_on_send_emit(quicly_stream_t *qs, size_t off, void *dst, size_t *len, int *wrote_all)
{
    struct st_h2o_http3_egress_unistream_t *stream = qs->data;

    if (*len >= stream->sendbuf->size - off) {
        *len = stream->sendbuf->size - off;
        *wrote_all = 1;
    } else {
        *wrote_all = 0;
    }
    memcpy(dst, stream->sendbuf->bytes + off, *len);
    return 0;
}

static int egress_unistream_on_send_stop(quicly_stream_t *qs, int err)
{
    return H2O_HTTP3_ERROR_CLOSED_CRITICAL_STREAM;
}

void h2o_http3_on_create_unidirectional_stream(quicly_stream_t *qs)
{
    if (quicly_stream_is_self_initiated(qs)) {
        /* create egress unistream */
        static const quicly_stream_callbacks_t callbacks = {egress_unistream_on_destroy, egress_unistream_on_send_shift,
                                                            egress_unistream_on_send_emit, egress_unistream_on_send_stop};
        struct st_h2o_http3_egress_unistream_t *stream = h2o_mem_alloc(sizeof(*stream));
        qs->data = stream;
        qs->callbacks = &callbacks;
        stream->quic = qs;
        h2o_buffer_init(&stream->sendbuf, &h2o_socket_buffer_prototype);
    } else {
        /* create ingress unistream */
        static const quicly_stream_callbacks_t callbacks = {
            ingress_unistream_on_destroy, NULL, NULL, NULL, ingress_unistream_on_receive, ingress_unistream_on_receive_reset};
        struct st_h2o_http3_ingress_unistream_t *stream = h2o_mem_alloc(sizeof(*stream));
        qs->data = stream;
        qs->callbacks = &callbacks;
        stream->quic = qs;
        h2o_buffer_init(&stream->recvbuf, &h2o_socket_buffer_prototype);
        stream->handle_input = unknown_type_handle_input;
    }
}

static int open_egress_unistream(h2o_http3_conn_t *conn, struct st_h2o_http3_egress_unistream_t **stream, h2o_iovec_t initial_bytes)
{
    quicly_stream_t *qs;
    int ret;

    if ((ret = quicly_open_stream(conn->quic, &qs, 1)) != 0)
        return ret;
    *stream = qs->data;
    assert((*stream)->quic == qs);

    h2o_buffer_append(&(*stream)->sendbuf, initial_bytes.base, initial_bytes.len);
    return quicly_stream_sync_sendbuf((*stream)->quic, 1);
}

static uint8_t *accept_hashkey_flatten_address(uint8_t *p, quicly_address_t *addr)
{
    switch (addr->sa.sa_family) {
    case AF_INET:
        *p++ = 4;
        memcpy(p, &addr->sin.sin_addr.s_addr, 4);
        p += 4;
        memcpy(p, &addr->sin.sin_port, 2);
        p += 2;
        break;
    case AF_INET6:
        *p++ = 6;
        memcpy(p, addr->sin6.sin6_addr.s6_addr, 16);
        p += 16;
        memcpy(p, &addr->sin.sin_port, 2);
        p += 2;
        break;
    case AF_UNSPEC:
        *p++ = 0;
    default:
        h2o_fatal("unknown protocol family");
        break;
    }
    return p;
}

static uint64_t calc_accept_hashkey(quicly_address_t *destaddr, quicly_address_t *srcaddr, ptls_iovec_t src_cid)
{
    /* prepare key */
    static __thread EVP_CIPHER_CTX *cipher = NULL;
    if (cipher == NULL) {
        static uint8_t key[PTLS_AES128_KEY_SIZE];
        H2O_MULTITHREAD_ONCE({ ptls_openssl_random_bytes(key, sizeof(key)); });
        cipher = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(cipher, EVP_aes_128_cbc(), NULL, key, NULL);
    }

    uint8_t buf[(1 + 16 + 2) * 2 + QUICLY_MAX_CID_LEN_V1 + PTLS_AES_BLOCK_SIZE] = {0};
    uint8_t *p = buf;

    /* build plaintext to encrypt */
    p = accept_hashkey_flatten_address(p, destaddr);
    p = accept_hashkey_flatten_address(p, srcaddr);
    memcpy(p, src_cid.base, src_cid.len);
    p += src_cid.len;
    assert(p <= buf + sizeof(buf));
    size_t bytes_to_encrypt = ((p - buf) + PTLS_AES_BLOCK_SIZE - 1) / PTLS_AES_BLOCK_SIZE * PTLS_AES_BLOCK_SIZE;
    assert(bytes_to_encrypt <= sizeof(buf));

    { /* encrypt */
        EVP_EncryptInit_ex(cipher, NULL, NULL, NULL, NULL);
        int bytes_encrypted = 0, ret = EVP_EncryptUpdate(cipher, buf, &bytes_encrypted, buf, (int)bytes_to_encrypt);
        assert(ret);
        assert(bytes_encrypted == bytes_to_encrypt);
    }

    /* use the last `size_t` bytes of the CBC output as the result */
    uint64_t result;
    memcpy(&result, buf + bytes_to_encrypt - sizeof(result), sizeof(result));
    /* avoid 0 (used as nonexist) */
    if (result == 0)
        result = 1;
    return result;
}

static void drop_from_acceptmap(h2o_http3_ctx_t *ctx, h2o_http3_conn_t *conn)
{
    if (conn->_accept_hashkey != 0) {
        khint_t iter;
        if ((iter = kh_get_h2o_http3_acceptmap(ctx->conns_accepting, conn->_accept_hashkey)) != kh_end(ctx->conns_accepting))
            kh_del_h2o_http3_acceptmap(ctx->conns_accepting, iter);
        conn->_accept_hashkey = 0;
    }
}

static void process_packets(h2o_http3_ctx_t *ctx, quicly_address_t *destaddr, quicly_address_t *srcaddr, uint8_t ttl,
                            quicly_decoded_packet_t *packets, size_t num_packets)
{
    h2o_http3_conn_t *conn = NULL;
    size_t accepted_packet_index = SIZE_MAX;

    /* find the matching connection, by first looking at the CID (all packets as client, or Handshake, 1-RTT packets as server) */
    if (packets[0].cid.dest.plaintext.node_id == ctx->next_cid.node_id &&
        packets[0].cid.dest.plaintext.thread_id == ctx->next_cid.thread_id) {
        khiter_t iter = kh_get_h2o_http3_idmap(ctx->conns_by_id, packets[0].cid.dest.plaintext.master_id);
        if (iter != kh_end(ctx->conns_by_id)) {
            conn = kh_val(ctx->conns_by_id, iter);
            /* CID-based matching on Initial and 0-RTT packets should only be applied for clients */
            if (!quicly_is_client(conn->quic) && packets[0].cid.dest.might_be_client_generated)
                conn = NULL;
        } else if (!packets[0].cid.dest.might_be_client_generated) {
            /* send stateless reset when we could not find a matching connection for a 1 RTT packet */
            if (packets[0].octets.len >= QUICLY_STATELESS_RESET_PACKET_MIN_LEN) {
                quicly_datagram_t *dgram =
                    quicly_send_stateless_reset(ctx->quic, &destaddr->sa, &srcaddr->sa, packets[0].cid.dest.encrypted.base);
                if (dgram != NULL) {
                    h2o_http3_send_datagram(ctx, dgram);
                    ctx->quic->packet_allocator->free_packet(ctx->quic->packet_allocator, dgram);
                }
            }
            return;
        }
    } else if (!packets[0].cid.dest.might_be_client_generated) {
        /* forward 1-RTT packets belonging to different nodes, threads */
        if (ttl == 0)
            return;
        uint64_t offending_node_id = packets[0].cid.dest.plaintext.node_id;
        if (ctx->forward_packets != NULL && ctx->forward_packets(ctx, &offending_node_id, packets[0].cid.dest.plaintext.thread_id,
                                                                 destaddr, srcaddr, ttl, packets, num_packets))
            return;
        /* non-authenticating 1-RTT packets are potentially stateless resets (FIXME handle them, note that we need to use a hashdos-
         * resistant hash map that also meets constant-time comparison requirements) */
        return;
    }

    if (conn == NULL) {
        /* Initial or 0-RTT packet, use 4-tuple to match the thread and the connection */
        assert(packets[0].cid.dest.might_be_client_generated);
        uint64_t accept_hashkey = calc_accept_hashkey(destaddr, srcaddr, packets[0].cid.src);
        if (ctx->accept_thread_divisor != 0) {
            uint32_t offending_thread = accept_hashkey % ctx->accept_thread_divisor;
            if (offending_thread != ctx->next_cid.thread_id) {
                if (ctx->forward_packets != NULL)
                    ctx->forward_packets(ctx, NULL, offending_thread, destaddr, srcaddr, ttl, packets, num_packets);
                return;
            }
        }
        khiter_t iter = kh_get_h2o_http3_acceptmap(ctx->conns_accepting, accept_hashkey);
        if (iter == kh_end(ctx->conns_accepting)) {
            /* a new connection for this thread (at least on this process); accept or delegate to newer process */
            if (ctx->acceptor == NULL) {
                /* This is the offending thread but it is not accepting, which means that the process (or the thread) is
                 * gracefully shutting down.  Let the application process forward the packet to the next generation. */
                if (ctx->forward_packets != NULL)
                    ctx->forward_packets(ctx, NULL, ctx->next_cid.thread_id, destaddr, srcaddr, ttl, packets, num_packets);
                return;
            }
            /* try to accept any of the Initial packets being received */
            size_t i;
            for (i = 0; i != num_packets; ++i)
                if ((packets[i].octets.base[0] & 0xf0) == 0xc0)
                    if ((conn = ctx->acceptor(ctx, destaddr, srcaddr, packets + i)) != NULL)
                        break;
            if (conn == NULL)
                return;
            accepted_packet_index = i;
            conn->_accept_hashkey = accept_hashkey;
            int r;
            iter = kh_put_h2o_http3_acceptmap(conn->ctx->conns_accepting, accept_hashkey, &r);
            assert(iter != kh_end(conn->ctx->conns_accepting));
            kh_val(conn->ctx->conns_accepting, iter) = conn;
            goto Respond;
        }
        conn = kh_val(ctx->conns_accepting, iter);
        assert(!quicly_is_client(conn->quic));
    }

    { /* receive packets to the found connection */
        assert(conn != NULL);
        if (!quicly_is_destination(conn->quic, &destaddr->sa, &srcaddr->sa, packets))
            return;
        size_t i;
        for (i = 0; i != num_packets; ++i) {
            /* FIXME process errors? */
            if (i != accepted_packet_index)
                quicly_receive(conn->quic, &destaddr->sa, &srcaddr->sa, packets + i);
        }
    }

Respond:
    /* for locality, emit packets belonging to the same connection NOW! */
    if (!h2o_http3_send(conn))
        conn = NULL;
    if (conn != NULL && ctx->notify_conn_update != NULL)
        ctx->notify_conn_update(ctx, conn);
}

void h2o_http3_read_socket(h2o_http3_ctx_t *ctx, h2o_socket_t *sock, h2o_http3_preprocess_received_cb preprocess)
{
    int fd = h2o_socket_get_fd(sock);

    while (1) {
        uint8_t buf[16384], *bufpt = buf;
        struct {
            struct msghdr mess;
            quicly_address_t destaddr, srcaddr;
            struct iovec vec;
            uint8_t ttl;
            char controlbuf[
#ifdef IPV6_PKTINFO
                CMSG_SPACE(sizeof(struct in6_pktinfo))
#elif defined(IP_PKTINFO)
                CMSG_SPACE(sizeof(struct in_pktinfo))
#else
                CMSG_SPACE(1)
#endif
            ];
        } dgrams[32];
        size_t dgram_index, num_dgrams;
        ssize_t rret;

        /* read datagrams */
        for (dgram_index = 0; dgram_index < sizeof(dgrams) / sizeof(dgrams[0]) && buf + sizeof(buf) - bufpt > 2048; ++dgram_index) {
            /* read datagram */
        Read:
            memset(dgrams + dgram_index, 0, sizeof(dgrams[dgram_index]));
            dgrams[dgram_index].mess.msg_name = &dgrams[dgram_index].srcaddr;
            dgrams[dgram_index].mess.msg_namelen = sizeof(dgrams[dgram_index].srcaddr);
            dgrams[dgram_index].vec.iov_base = bufpt;
            dgrams[dgram_index].vec.iov_len = buf + sizeof(buf) - bufpt;
            dgrams[dgram_index].mess.msg_iov = &dgrams[dgram_index].vec;
            dgrams[dgram_index].mess.msg_iovlen = 1;
            dgrams[dgram_index].mess.msg_control = &dgrams[dgram_index].controlbuf;
            dgrams[dgram_index].mess.msg_controllen = sizeof(dgrams[dgram_index].controlbuf);
            while ((rret = recvmsg(fd, &dgrams[dgram_index].mess, 0)) <= 0 && errno == EINTR)
                ;
            if (rret <= 0)
                break;
            dgrams[dgram_index].vec.iov_len = rret;
            { /* fetch destination address */
                struct cmsghdr *cmsg;
                for (cmsg = CMSG_FIRSTHDR(&dgrams[dgram_index].mess); cmsg != NULL;
                     cmsg = CMSG_NXTHDR(&dgrams[dgram_index].mess, cmsg)) {
#ifdef IP_PKTINFO
                    if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                        dgrams[dgram_index].destaddr.sin.sin_family = AF_INET;
                        dgrams[dgram_index].destaddr.sin.sin_addr = ((struct in_pktinfo *)CMSG_DATA(cmsg))->ipi_addr;
                        dgrams[dgram_index].destaddr.sin.sin_port = *ctx->sock.port;
                        goto DestAddrFound;
                    }
#endif
#ifdef IPV6_PKTINFO
                    if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
                        dgrams[dgram_index].destaddr.sin6.sin6_family = AF_INET6;
                        dgrams[dgram_index].destaddr.sin6.sin6_addr = ((struct in6_pktinfo *)CMSG_DATA(cmsg))->ipi6_addr;
                        dgrams[dgram_index].destaddr.sin6.sin6_port = *ctx->sock.port;
                        goto DestAddrFound;
                    }
#endif
                }
                dgrams[dgram_index].destaddr.sa.sa_family = AF_UNSPEC;
            DestAddrFound:;
            }
            if (preprocess != NULL) {
                /* preprocess (and drop the packet if it failed) */
                if (!preprocess(ctx, &dgrams[dgram_index].mess, &dgrams[dgram_index].destaddr, &dgrams[dgram_index].srcaddr,
                                &dgrams[dgram_index].ttl))
                    goto Read;
            } else {
                dgrams[dgram_index].ttl = ctx->default_ttl;
            }
            assert(dgrams[dgram_index].srcaddr.sa.sa_family == AF_INET || dgrams[dgram_index].srcaddr.sa.sa_family == AF_INET6);
            bufpt += rret;
        }
        num_dgrams = dgram_index;
        if (num_dgrams == 0)
            break;

        /* convert dgrams to decoded packets and process */
        quicly_decoded_packet_t packets[64];
        size_t packet_index = 0;
        for (dgram_index = 0; dgram_index != num_dgrams; ++dgram_index) {
            if (packet_index != 0 &&
                !(dgram_index == 0 ||
                  h2o_socket_compare_address(&dgrams[dgram_index - 1].srcaddr.sa, &dgrams[dgram_index].srcaddr.sa, 1) != 0 ||
                  !((dgrams[dgram_index - 1].destaddr.sa.sa_family == AF_UNSPEC &&
                     dgrams[dgram_index].destaddr.sa.sa_family == AF_UNSPEC) ||
                    h2o_socket_compare_address(&dgrams[dgram_index - 1].destaddr.sa, &dgrams[dgram_index].destaddr.sa, 1) == 0) ||
                  dgrams[dgram_index - 1].ttl != dgrams[dgram_index].ttl)) {
                process_packets(ctx, &dgrams[dgram_index - 1].destaddr, &dgrams[dgram_index - 1].srcaddr,
                                dgrams[dgram_index - 1].ttl, packets, packet_index);
                packet_index = 0;
            }
            size_t off = 0;
            while (off != dgrams[dgram_index].vec.iov_len) {
                size_t plen = quicly_decode_packet(ctx->quic, packets + packet_index, dgrams[dgram_index].vec.iov_base + off,
                                                   dgrams[dgram_index].vec.iov_len - off);
                if (plen == SIZE_MAX)
                    break;
                off += plen;
                if (packet_index == sizeof(packets) / sizeof(packets[0]) - 1 ||
                    !(packet_index == 0 ||
                      h2o_memis(packets[0].cid.dest.encrypted.base, packets[0].cid.dest.encrypted.len,
                                packets[packet_index].cid.dest.encrypted.base, packets[packet_index].cid.dest.encrypted.len))) {
                    process_packets(ctx, &dgrams[dgram_index].destaddr, &dgrams[dgram_index].srcaddr, dgrams[dgram_index].ttl,
                                    packets, packet_index + 1);
                    packet_index = 0;
                } else {
                    ++packet_index;
                }
            }
        }
        if (packet_index != 0)
            process_packets(ctx, &dgrams[dgram_index - 1].destaddr, &dgrams[dgram_index - 1].srcaddr, dgrams[dgram_index - 1].ttl,
                            packets, packet_index);
    }
}

static void on_read(h2o_socket_t *sock, const char *err)
{
    h2o_http3_ctx_t *ctx = sock->data;
    h2o_http3_read_socket(ctx, sock, NULL);
}

static void on_timeout(h2o_timer_t *timeout)
{
    h2o_http3_conn_t *conn = H2O_STRUCT_FROM_MEMBER(h2o_http3_conn_t, _timeout, timeout);
    h2o_http3_send(conn);
}

int h2o_http3_read_frame(h2o_http3_read_frame_t *frame, int is_client, uint64_t stream_type, const uint8_t **_src,
                         const uint8_t *src_end, const char **err_desc)
{
    const uint8_t *src = *_src;

    if ((frame->type = quicly_decodev(&src, src_end)) == UINT64_MAX)
        return H2O_HTTP3_ERROR_INCOMPLETE;
    if ((frame->length = quicly_decodev(&src, src_end)) == UINT64_MAX)
        return H2O_HTTP3_ERROR_INCOMPLETE;
    frame->_header_size = (uint8_t)(src - *_src);

    /* read the content of the frame (unless it's a DATA frame) */
    if (frame->type != H2O_HTTP3_FRAME_TYPE_DATA) {
        if (frame->length >= MAX_FRAME_SIZE) {
            *err_desc = "H3 frame too large";
            return H2O_HTTP3_ERROR_GENERAL_PROTOCOL; /* FIXME is this the correct code? */
        }
        if (src_end - src < frame->length)
            return H2O_HTTP3_ERROR_INCOMPLETE;
        frame->payload = src;
        src += frame->length;
    }

    /* validate frame type */
    switch (frame->type) {
#define FRAME(id, req_clnt, req_srvr, ctl_clnt, ctl_srvr)                                                                          \
    case H2O_HTTP3_FRAME_TYPE_##id:                                                                                                \
        switch (stream_type) {                                                                                                     \
        case H2O_HTTP3_STREAM_TYPE_REQUEST:                                                                                        \
            if (req_clnt && !is_client)                                                                                            \
                goto Validation_Success;                                                                                           \
            if (req_srvr && is_client)                                                                                             \
                goto Validation_Success;                                                                                           \
            break;                                                                                                                 \
        case H2O_HTTP3_STREAM_TYPE_CONTROL:                                                                                        \
            if (ctl_clnt && !is_client)                                                                                            \
                goto Validation_Success;                                                                                           \
            if (ctl_srvr && is_client)                                                                                             \
                goto Validation_Success;                                                                                           \
            break;                                                                                                                 \
        default:                                                                                                                   \
            h2o_fatal("enxpected stream type");                                                                                    \
            break;                                                                                                                 \
        }                                                                                                                          \
        break
        /* clang-format off */
        /*   +----------------+-------------+-------------+
         *   |                | req-stream  | ctrl-stream |
         *   |     frame      +------+------+------+------+
         *   |                |client|server|client|server|
         *   +----------------+------+------+------+------+ */
        FRAME( DATA           ,    1 ,    1 ,    0 ,    0 );
        FRAME( HEADERS        ,    1 ,    1 ,    0 ,    0 );
        FRAME( PRIORITY       ,    0 ,    0 ,    1 ,    0 );
        FRAME( CANCEL_PUSH    ,    0 ,    0 ,    1 ,    1 );
        FRAME( SETTINGS       ,    0 ,    0 ,    1 ,    1 );
        FRAME( PUSH_PROMISE   ,    0 ,    1 ,    0 ,    0 );
        FRAME( GOAWAY         ,    0 ,    0 ,    1 ,    1 );
        FRAME( MAX_PUSH_ID    ,    0 ,    0 ,    1 ,    0 );
        FRAME( DUPLICATE_PUSH ,    0 ,    1 ,    0 ,    0 );
        /*   +----------------+------+------+------+------+ */
        /* clang-format on */
#undef FRAME
    default:
        /* ignore extension frames that we do not handle */
        goto Validation_Success;
    }
    return H2O_HTTP3_ERROR_FRAME_UNEXPECTED;
Validation_Success:;

    *_src = src;
    return 0;
}

void h2o_http3_init_context(h2o_http3_ctx_t *ctx, h2o_loop_t *loop, h2o_socket_t *sock, quicly_context_t *quic,
                            h2o_http3_accept_cb acceptor, h2o_http3_notify_connection_update_cb notify_conn_update)
{
    assert(quic->stream_open != NULL);

    *ctx = (h2o_http3_ctx_t){loop,
                             {sock},
                             quic,
                             {0} /* thread_id, node_id are set by h2o_http3_set_context_identifier */,
                             kh_init_h2o_http3_idmap(),
                             kh_init_h2o_http3_acceptmap(),
                             notify_conn_update};
    ctx->sock.sock->data = ctx;
    ctx->sock.addrlen = h2o_socket_getsockname(ctx->sock.sock, (void *)&ctx->sock.addr);
    assert(ctx->sock.addrlen != 0);
    switch (ctx->sock.addr.ss_family) {
    case AF_INET:
        ctx->sock.port = &((struct sockaddr_in *)&ctx->sock.addr)->sin_port;
        break;
    case AF_INET6:
        ctx->sock.port = &((struct sockaddr_in6 *)&ctx->sock.addr)->sin6_port;
        break;
    default:
        assert(!"unexpected address family");
        break;
    }
    h2o_linklist_init_anchor(&ctx->clients);
    ctx->acceptor = acceptor;

    h2o_socket_read_start(ctx->sock.sock, on_read);
}

void h2o_http3_dispose_context(h2o_http3_ctx_t *ctx)
{
    assert(kh_size(ctx->conns_by_id) == 0);
    assert(kh_size(ctx->conns_accepting) == 0);
    assert(h2o_linklist_is_empty(&ctx->clients));

    h2o_socket_close(ctx->sock.sock);
    kh_destroy_h2o_http3_idmap(ctx->conns_by_id);
    kh_destroy_h2o_http3_acceptmap(ctx->conns_accepting);
}

void h2o_http3_set_context_identifier(h2o_http3_ctx_t *ctx, uint32_t accept_thread_divisor, uint32_t thread_id, uint64_t node_id,
                                      uint8_t ttl, h2o_http3_forward_packets_cb forward_packets_cb)
{
    ctx->accept_thread_divisor = accept_thread_divisor;
    ctx->next_cid.thread_id = thread_id;
    ctx->next_cid.node_id = node_id;
    ctx->forward_packets = forward_packets_cb;
    ctx->default_ttl = ttl;
}

void h2o_http3_close_connection(h2o_http3_conn_t *conn, int err, const char *reason_phrase)
{
    switch (quicly_get_state(conn->quic)) {
    case QUICLY_STATE_FIRSTFLIGHT:
        conn->callbacks->destroy_connection(conn);
        break;
    case QUICLY_STATE_CONNECTED:
        quicly_close(conn->quic, err, reason_phrase);
        h2o_http3_schedule_timer(conn);
        break;
    default:
        /* only need to wait for the socket close */
        break;
    }
}

void h2o_http3_close_all_connections(h2o_http3_ctx_t *ctx)
{
    h2o_http3_conn_t *conn;

    kh_foreach_value(ctx->conns_by_id, conn, { h2o_http3_close_connection(conn, 0, NULL); });
    kh_foreach_value(ctx->conns_accepting, conn, { h2o_http3_close_connection(conn, 0, NULL); });
}

size_t h2o_http3_num_connections(h2o_http3_ctx_t *ctx)
{
    return kh_size(ctx->conns_by_id) + kh_size(ctx->conns_accepting);
}

void h2o_http3_init_conn(h2o_http3_conn_t *conn, h2o_http3_ctx_t *ctx, const h2o_http3_conn_callbacks_t *callbacks)
{
    *conn = (h2o_http3_conn_t){ctx, NULL, callbacks};
    h2o_timer_init(&conn->_timeout, on_timeout);
}

void h2o_http3_dispose_conn(h2o_http3_conn_t *conn)
{
    if (conn->qpack.dec != NULL)
        h2o_qpack_destroy_decoder(conn->qpack.dec);
    if (conn->qpack.enc != NULL)
        h2o_qpack_destroy_encoder(conn->qpack.enc);
    if (conn->quic != NULL) {
        khiter_t iter;
        /* unregister from maps */
        if ((iter = kh_get_h2o_http3_idmap(conn->ctx->conns_by_id, quicly_get_master_id(conn->quic)->master_id)) !=
            kh_end(conn->ctx->conns_by_id))
            kh_del_h2o_http3_idmap(conn->ctx->conns_by_id, iter);
        drop_from_acceptmap(conn->ctx, conn);
        quicly_free(conn->quic);
    }
    h2o_timer_unlink(&conn->_timeout);
}

int h2o_http3_setup(h2o_http3_conn_t *conn, quicly_conn_t *quic)
{
    int ret;

    conn->quic = quic;
    *quicly_get_data(conn->quic) = conn;

    /* setup h3 objects, only when the connection state has been created */
    if (quicly_get_state(quic) > QUICLY_STATE_CONNECTED)
        goto Exit;

    conn->qpack.dec = h2o_qpack_create_decoder(H2O_HTTP3_DEFAULT_HEADER_TABLE_SIZE, 100 /* FIXME */);

    { /* register to the idmap */
        int r;
        khiter_t iter = kh_put_h2o_http3_idmap(conn->ctx->conns_by_id, quicly_get_master_id(conn->quic)->master_id, &r);
        assert(iter != kh_end(conn->ctx->conns_by_id));
        kh_val(conn->ctx->conns_by_id, iter) = conn;
    }

    { /* open control streams, send SETTINGS */
        static const uint8_t client_first_flight[] = {H2O_HTTP3_STREAM_TYPE_CONTROL, H2O_HTTP3_FRAME_TYPE_SETTINGS, 0};
        static const uint8_t server_first_flight[] = {H2O_HTTP3_STREAM_TYPE_CONTROL, H2O_HTTP3_FRAME_TYPE_SETTINGS, 2,
                                                      H2O_HTTP3_SETTINGS_NUM_PLACEHOLDERS, H2O_HTTP3_MAX_PLACEHOLDERS};
        h2o_iovec_t first_flight = quicly_is_client(conn->quic) ? h2o_iovec_init(client_first_flight, sizeof(client_first_flight))
                                                                : h2o_iovec_init(server_first_flight, sizeof(server_first_flight));
        if ((ret = open_egress_unistream(conn, &conn->_control_streams.egress.control, first_flight)) != 0)
            return ret;
    }

    { /* open QPACK encoder & decoder streams */
        static const uint8_t encoder_first_flight[] = {H2O_HTTP3_STREAM_TYPE_QPACK_ENCODER};
        static const uint8_t decoder_first_flight[] = {H2O_HTTP3_STREAM_TYPE_QPACK_DECODER};
        if ((ret = open_egress_unistream(conn, &conn->_control_streams.egress.qpack_encoder,
                                         h2o_iovec_init(encoder_first_flight, sizeof(encoder_first_flight)))) != 0 ||
            (ret = open_egress_unistream(conn, &conn->_control_streams.egress.qpack_decoder,
                                         h2o_iovec_init(decoder_first_flight, sizeof(decoder_first_flight)))) != 0)
            return ret;
    }

Exit:
    h2o_http3_schedule_timer(conn);
    return 0;
}

int h2o_http3_send(h2o_http3_conn_t *conn)
{
    quicly_datagram_t *packets[16];
    size_t num_packets, i;

    do {
        num_packets = sizeof(packets) / sizeof(packets[0]);
        int ret = quicly_send(conn->quic, packets, &num_packets);
        switch (ret) {
        case 0:
            for (i = 0; i != num_packets; ++i) {
                if (!h2o_http3_send_datagram(conn->ctx, packets[i])) {
                    /* FIXME close the connection immediately */
                    break;
                }
                conn->ctx->quic->packet_allocator->free_packet(conn->ctx->quic->packet_allocator, packets[i]);
            }
            for (; i != num_packets; ++i)
                conn->ctx->quic->packet_allocator->free_packet(conn->ctx->quic->packet_allocator, packets[i]);
            break;
        case QUICLY_ERROR_FREE_CONNECTION:
            conn->callbacks->destroy_connection(conn);
            return 0;
        default:
            fprintf(stderr, "quicly_send returned %d\n", ret);
            abort();
        }
    } while (num_packets == sizeof(packets) / sizeof(packets[0]));

    h2o_http3_schedule_timer(conn);

    return 1;
}

int h2o_http3_update_recvbuf(h2o_buffer_t **buf, size_t off, const void *src, size_t len)
{
    size_t new_size = off + len;

    if ((*buf)->size < new_size) {
        h2o_buffer_reserve(buf, new_size - (*buf)->size);
        (*buf)->size = new_size;
    }

    memcpy((*buf)->bytes + off, src, len);
    return 0;
}

void h2o_http3_schedule_timer(h2o_http3_conn_t *conn)
{
    int64_t timeout = quicly_get_first_timeout(conn->quic);
    if (h2o_timer_is_linked(&conn->_timeout)) {
#if !H2O_USE_LIBUV /* optimization to skip registering a timer specifying the same time */
        if (timeout == conn->_timeout.expire_at)
            return;
#endif
        h2o_timer_unlink(&conn->_timeout);
    }
    uint64_t now = h2o_now(conn->ctx->loop), delay = now < timeout ? timeout - now : 0;
    h2o_timer_link(conn->ctx->loop, delay, &conn->_timeout);
}

int h2o_http3_handle_settings_frame(h2o_http3_conn_t *conn, const uint8_t *payload, size_t length, const char **err_desc)
{
    const uint8_t *src = payload, *src_end = src + length;
    uint32_t header_table_size = H2O_HTTP3_DEFAULT_HEADER_TABLE_SIZE;

    assert(!h2o_http3_has_received_settings(conn));

    while (src != src_end) {
        uint64_t id;
        uint64_t value;
        if ((id = quicly_decodev(&src, src_end)) == UINT64_MAX)
            goto Malformed;
        if ((value = quicly_decodev(&src, src_end)) == UINT64_MAX)
            goto Malformed;
        switch (id) {
        case H2O_HTTP3_SETTINGS_MAX_HEADER_LIST_SIZE:
            conn->peer_settings.max_header_list_size = (uint64_t)value;
            break;
        case H2O_HTTP3_SETTINGS_NUM_PLACEHOLDERS:
            if (!quicly_is_client(conn->quic))
                goto Malformed;
            conn->peer_settings.num_placeholders = (uint64_t)value;
            break;
        case H2O_HTTP3_SETTINGS_HEADER_TABLE_SIZE:
            header_table_size = (uint32_t)value;
            break;
        /* TODO add */
        default:
            break;
        }
    }

    conn->qpack.enc = h2o_qpack_create_encoder(header_table_size, 100 /* FIXME */);
    return 0;
Malformed:
    *err_desc = "malformed SETTINGS frame";
    return H2O_HTTP3_ERROR_FRAME;
}

void h2o_http3_send_qpack_stream_cancel(h2o_http3_conn_t *conn, quicly_stream_id_t stream_id)
{
    struct st_h2o_http3_egress_unistream_t *stream = conn->_control_streams.egress.qpack_decoder;

    /* allocate and write */
    h2o_iovec_t buf = h2o_buffer_reserve(&stream->sendbuf, stream->sendbuf->size + H2O_HPACK_ENCODE_INT_MAX_LENGTH);
    assert(buf.base != NULL);
    stream->sendbuf->size += h2o_qpack_decoder_send_stream_cancel(conn->qpack.dec, (uint8_t *)buf.base, stream_id);

    /* notify the transport */
    H2O_HTTP3_CHECK_SUCCESS(quicly_stream_sync_sendbuf(stream->quic, 1) == 0);
}

void h2o_http3_send_qpack_header_ack(h2o_http3_conn_t *conn, const void *bytes, size_t len)
{
    struct st_h2o_http3_egress_unistream_t *stream = conn->_control_streams.egress.qpack_encoder;

    assert(stream != NULL);
    h2o_buffer_append(&stream->sendbuf, bytes, len);
    H2O_HTTP3_CHECK_SUCCESS(quicly_stream_sync_sendbuf(stream->quic, 1));
}
