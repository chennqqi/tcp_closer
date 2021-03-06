/*
 * Copyright 2017 Kristian Evensen <kristian.evensen@gmail.com>
 *
 * This file is part of TCP closer. TCP closer is free software: you can
 * redistribute it and/or modify it under the terms of the Lesser GNU General
 * Public License as published by the Free Software Foundation, either version 3
 * of the License, or (at your option) any later version.
 *
 * TCP closer is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * TCP closer. If not, see http://www.gnu.org/licenses/.
 */

#ifndef TCP_CLOSER_H
#define TCP_CLOSER_H

#include <stdint.h>
#include <stdbool.h>

struct inet_diag_bc_op;
struct mnl_socket;
struct backend_event_loop;
struct backend_epoll_handle;
struct backend_timeout_handle;

//This is an artifical limitation introduced by me, but is large enough for at
//least my use-cases. Since there is no EQ operator, we need to check a port for
//both LE and GE. In case of a match, we also need to be able to jump directly
//to destination ports/end. This means that one port requires five op-structs
//and will consume 20 bytes.
//
//The correct limit is something closer to 0xFFFF/20 source and destination
//ports, with some adjustmenets for the first and last operation. The value used
//to store the offset to jump to is a short, so maximum offset from any op is
//0xFFFF
#define MAX_NUM_PORTS 128

struct tcp_closer_ctx {
    struct backend_event_loop *event_loop;
    struct inet_diag_bc_op *diag_filter;
    struct mnl_socket *diag_dump_socket;
    struct backend_epoll_handle *dump_handle;
    struct backend_timeout_handle *dump_timeout;
    struct mnl_socket *diag_destroy_socket;
    struct backend_epoll_handle *destroy_handle;
    FILE *logfile;

    uint32_t diag_filter_len;
    uint32_t dump_interval;

    //Limit for tcpi_last_data_recv before killing socket
    uint32_t idle_time;

    //An (optional) upper limit for last_data_recv. The reason for adding an
    //upper limit is that last_data_recv contains a bogus value right after
    //connection enters the ESTABLISHED state (before data is received). On my
    //machines, last_data_recv is always around 200+ hours. The limit can be
    //used to ignore such connections.
    uint32_t last_data_recv_limit;

    uint8_t socket_family;

    bool verbose_mode;
    bool use_netlink;
    bool dump_in_progress;
    bool use_syslog;
};

#endif
