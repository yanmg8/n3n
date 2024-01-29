/**
 * (C) 2007-22 - ntop.org and contributors
 * Copyright (C) 2023 Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not see see <http://www.gnu.org/licenses/>
 *
 */

/*
 * This file has a large amount of duplication with the edge_management.c
 * code.  In the fullness of time, they should both be merged
 */


#include <errno.h>       // for errno
#include <connslot/strbuf.h> // for strbuf_t
#include <n3n/ethernet.h>    // for is_null_mac
#include <n3n/logging.h> // for traceEvent
#include <n3n/strings.h> // for ip_subnet_to_str, sock_to_cstr
#include <stdbool.h>
#include <stdint.h>      // for uint8_t, uint32_t
#include <stdio.h>       // for snprintf, size_t, sprintf, NULL
#include <string.h>      // for memcmp, memcpy, strerror, strncpy
#include <sys/types.h>   // for ssize_t, time_t
#include "management.h"  // for mgmt_req_t, send_reply, mgmt_handler_t, mgmt...
#include "n2n.h"         // for n2n_edge_t, sn_community, N2N_SN_PK...
#include "n2n_define.h"    // for N2N_SN_PKTBUF_SIZE, UNPURGEABLE
#include "n2n_typedefs.h"  // for n2n_edge_t, sn_community, peer_info, sn_stats_t
#include "peer_info.h"   // for peer_info, peer_info_t
#include "uthash.h"      // for UT_hash_handle, HASH_ITER, HASH_COUNT

#ifdef _WIN32
#include "win32/defs.h"
#else
#include <sys/socket.h>  // for sendto, socklen_t
#endif


int load_allowed_sn_community (n2n_edge_t *sss); /* defined in sn_utils.c */

static void mgmt_reload_communities (mgmt_req_t *req, strbuf_t *buf) {

    if(req->type!=N2N_MGMT_WRITE) {
        mgmt_error(req, buf, "writeonly");
        return;
    }

    if(!req->sss->community_file) {
        mgmt_error(req, buf, "nofile");
        return;
    }

    int ok = load_allowed_sn_community(req->sss);
    send_json_1uint(req, buf, "row", "ok", ok);
}

static void mgmt_timestamps (mgmt_req_t *req, strbuf_t *buf) {
    sb_printf(buf,
              "{"
              "\"_tag\":\"%s\","
              "\"_type\":\"row\","
              "\"start_time\":%lu,"
              "\"last_fwd\":%ld,"
              "\"last_reg_super\":%ld}\n",
              req->tag,
              req->sss->start_time,
              req->sss->sn_stats.last_fwd,
              req->sss->sn_stats.last_reg_super);

    send_reply(req, buf);
}

static void mgmt_packetstats (mgmt_req_t *req, strbuf_t *buf) {
    sb_printf(buf,
              "{"
              "\"_tag\":\"%s\","
              "\"_type\":\"row\","
              "\"type\":\"forward\","
              "\"tx_pkt\":%lu}\n",
              req->tag,
              req->sss->sn_stats.fwd);

    send_reply(req, buf);
    sb_zero(buf);

    sb_printf(buf,
              "{"
              "\"_tag\":\"%s\","
              "\"_type\":\"row\","
              "\"type\":\"broadcast\","
              "\"tx_pkt\":%lu}\n",
              req->tag,
              req->sss->sn_stats.broadcast);

    send_reply(req, buf);
    sb_zero(buf);

    sb_printf(buf,
              "{"
              "\"_tag\":\"%s\","
              "\"_type\":\"row\","
              "\"type\":\"reg_super\","
              "\"rx_pkt\":%lu,"
              "\"nak\":%lu}\n",
              req->tag,
              req->sss->sn_stats.reg_super,
              req->sss->sn_stats.reg_super_nak);

    /* Note: reg_super_nak is not currently incremented anywhere */

    send_reply(req, buf);
    sb_zero(buf);

    /* Generic errors when trying to sendto() */
    sb_printf(buf,
              "{"
              "\"_tag\":\"%s\","
              "\"_type\":\"row\","
              "\"type\":\"errors\","
              "\"tx_pkt\":%lu}\n",
              req->tag,
              req->sss->sn_stats.errors);

    send_reply(req, buf);
}

static void mgmt_communities (mgmt_req_t *req, strbuf_t *buf) {
    struct sn_community *community, *tmp;
    dec_ip_bit_str_t ip_bit_str = {'\0'};

    HASH_ITER(hh, req->sss->communities, community, tmp) {

        sb_printf(buf,
                  "{"
                  "\"_tag\":\"%s\","
                  "\"_type\":\"row\","
                  "\"community\":\"%s\","
                  "\"purgeable\":%i,"
                  "\"is_federation\":%i,"
                  "\"ip4addr\":\"%s\"}\n",
                  req->tag,
                  (community->is_federation) ? "-/-" : community->community,
                  community->purgeable,
                  community->is_federation,
                  (community->auto_ip_net.net_addr == 0) ? "" : ip_subnet_to_str(ip_bit_str, &community->auto_ip_net));

        send_reply(req, buf);
        sb_zero(buf);
    }
}

static void mgmt_edges (mgmt_req_t *req, strbuf_t *buf) {
    struct sn_community *community, *tmp;
    struct peer_info *peer, *tmpPeer;
    macstr_t mac_buf;
    n2n_sock_str_t sockbuf;
    dec_ip_bit_str_t ip_bit_str = {'\0'};

    HASH_ITER(hh, req->sss->communities, community, tmp) {
        HASH_ITER(hh, community->edges, peer, tmpPeer) {

            sb_printf(buf,
                      "{"
                      "\"_tag\":\"%s\","
                      "\"_type\":\"row\","
                      "\"community\":\"%s\","
                      "\"ip4addr\":\"%s\","
                      "\"purgeable\":%i,"
                      "\"macaddr\":\"%s\","
                      "\"sockaddr\":\"%s\","
                      "\"proto\":\"%s\","
                      "\"desc\":\"%s\","
                      "\"last_seen\":%li}\n",
                      req->tag,
                      (community->is_federation) ? "-/-" : community->community,
                      (peer->dev_addr.net_addr == 0) ? "" : ip_subnet_to_str(ip_bit_str, &peer->dev_addr),
                      peer->purgeable,
                      (is_null_mac(peer->mac_addr)) ? "" : macaddr_str(mac_buf, peer->mac_addr),
                      sock_to_cstr(sockbuf, &(peer->sock)),
                      ((peer->socket_fd >= 0) && (peer->socket_fd != req->sss->sock)) ? "TCP" : "UDP",
                      peer->dev_desc,
                      peer->last_seen);

            send_reply(req, buf);
            sb_zero(buf);
        }
    }
}

// Forward define so we can include this in the mgmt_handlers[] table
static void mgmt_help (mgmt_req_t *req, strbuf_t *buf);

static const mgmt_handler_t mgmt_handlers[] = {
    { .cmd = "supernodes", .help = "Reserved for edge", .func = mgmt_unimplemented},

    { .cmd = "stop", .flags = FLAG_WROK, .help = "Gracefully exit edge", .func = mgmt_stop},
    { .cmd = "verbose", .flags = FLAG_WROK, .help = "Manage verbosity level", .func = mgmt_verbose},
    { .cmd = "reload_communities", .flags = FLAG_WROK, .help = "Reloads communities and user's public keys", .func = mgmt_reload_communities},
    { .cmd = "communities", .help = "List current communities", .func = mgmt_communities},
    { .cmd = "edges", .help = "List current edges/peers", .func = mgmt_edges},
    { .cmd = "timestamps", .help = "Event timestamps", .func = mgmt_timestamps},
    { .cmd = "packetstats", .help = "Traffic statistics", .func = mgmt_packetstats},
    { .cmd = "help", .flags = FLAG_WROK, .help = "Show JSON commands", .func = mgmt_help},
};

// TODO: want to keep the mgmt_handlers defintion const static, otherwise
// this whole function could be shared
static void mgmt_help (mgmt_req_t *req, strbuf_t *buf) {
    /*
     * Even though this command is readonly, we deliberately do not check
     * the type - allowing help replies to both read and write requests
     */

    int i;
    int nr_handlers = sizeof(mgmt_handlers) / sizeof(mgmt_handler_t);
    for( i=0; i < nr_handlers; i++ ) {
        mgmt_help_row(req, buf, mgmt_handlers[i].cmd, mgmt_handlers[i].help);
        sb_zero(buf);
    }
}

// TODO: DRY
static void handleMgmtJson (mgmt_req_t *req, char *udp_buf, const int recvlen) {

    strbuf_t *buf;
    char cmdlinebuf[80];

    /* save a copy of the commandline before we reuse the udp_buf */
    strncpy(cmdlinebuf, udp_buf, sizeof(cmdlinebuf)-1);
    cmdlinebuf[sizeof(cmdlinebuf)-1] = 0;

    traceEvent(TRACE_DEBUG, "mgmt json %s", cmdlinebuf);

    /* we reuse the buffer already on the stack for all our strings */
    /* FIXME, this is a hack as it uses strbuf_t internals */
    buf = (strbuf_t *)udp_buf;
    sb_zero(buf);
    buf->capacity = N2N_SN_PKTBUF_SIZE;
    buf->capacity_max = buf->capacity;

    if(!mgmt_req_init2(req, buf, (char *)&cmdlinebuf)) {
        // if anything failed during init
        return;
    }

    int handler;
    lookup_handler(handler, mgmt_handlers, req->argv0);
    if(handler == -1) {
        mgmt_error(req, buf, "unknowncmd");
        return;
    }

    if((req->type==N2N_MGMT_WRITE) && !(mgmt_handlers[handler].flags & FLAG_WROK)) {
        mgmt_error(req, buf, "readonly");
        return;
    }

    /*
     * TODO:
     * The tag provided by the requester could contain chars
     * that make our JSON invalid.
     * - do we care?
     */
    send_json_1str(req, buf, "begin", "cmd", req->argv0);
    sb_zero(buf);

    mgmt_handlers[handler].func(req, buf);
    sb_zero(buf);

    send_json_1str(req, buf, "end", "cmd", req->argv0);
    return;
}

int process_mgmt (n2n_edge_t *sss,
                  const struct sockaddr *sender_sock, socklen_t sock_size,
                  char *mgmt_buf,
                  size_t mgmt_size,
                  time_t now) {

    mgmt_req_t req;

    traceEvent(TRACE_DEBUG, "process_mgmt");

    req.eee = NULL;
    req.sss = sss;
    req.mgmt_sock = sss->udp_mgmt_sock;
    req.keep_running = sss->keep_running;
    req.mgmt_password = sss->conf.mgmt_password;
    memcpy(&req.sender_sock, sender_sock, sock_size);
    req.sock_len = sock_size;

    /* avoid parsing any uninitialized junk from the stack */
    mgmt_buf[mgmt_size] = 0;

    if((mgmt_buf[0] >= 'a' || mgmt_buf[0] <= 'z') && (mgmt_buf[1] == ' ')) {
        /* this is a JSON request */
        handleMgmtJson(&req, mgmt_buf, mgmt_size);
        return 0;
    }

    traceEvent(TRACE_WARNING, "unknown mgmt request");
    return 0;
}
