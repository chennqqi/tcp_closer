#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <libmnl/libmnl.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <arpa/inet.h>
#include <linux/in.h>
#include <sys/types.h>
#include <pwd.h>
#include <linux/tcp.h>

#include "tcp_closer_netlink.h"
#include "tcp_closer_proc.h"
#include "tcp_closer.h"

static const char* tcp_states_map[] = {
    [TCP_ESTABLISHED] = "ESTABLISHED",
    [TCP_SYN_SENT] = "SYN-SENT",
    [TCP_SYN_RECV] = "SYN-RECV",
    [TCP_FIN_WAIT1] = "FIN-WAIT-1",
    [TCP_FIN_WAIT2] = "FIN-WAIT-2",
    [TCP_TIME_WAIT] = "TIME-WAIT",
    [TCP_CLOSE] = "CLOSE",
    [TCP_CLOSE_WAIT] = "CLOSE-WAIT",
    [TCP_LAST_ACK] = "LAST-ACK",
    [TCP_LISTEN] = "LISTEN",
    [TCP_CLOSING] = "CLOSING"
};

int send_diag_msg(struct tcp_closer_ctx *ctx)
{
    uint8_t diag_buf[MNL_SOCKET_BUFFER_SIZE];
    struct nlmsghdr *nlh;
    struct inet_diag_req_v2 *diag_req;

    nlh = mnl_nlmsg_put_header(diag_buf);
    nlh->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    nlh->nlmsg_type = SOCK_DIAG_BY_FAMILY;

    diag_req = mnl_nlmsg_put_extra_header(nlh, sizeof(struct inet_diag_req_v2));
    //TODO: Add a -4/-6 command line option
    diag_req->sdiag_family = AF_INET;
    diag_req->sdiag_protocol = IPPROTO_TCP;

    //We are only interested in established connections and need the tcp-info
    //struct
    diag_req->idiag_ext |= (1 << (INET_DIAG_INFO - 1));
    diag_req->idiag_states = 1 << TCP_ESTABLISHED;

    mnl_attr_put(nlh, INET_DIAG_REQ_BYTECODE, ctx->diag_filter_len, ctx->diag_filter);

    return mnl_socket_sendto(ctx->diag_dump_socket, diag_buf, nlh->nlmsg_len);
}

void destroy_socket(struct tcp_closer_ctx *ctx, struct inet_diag_msg *diag_msg)
{
    uint8_t destroy_buf[MNL_SOCKET_BUFFER_SIZE];
    struct nlmsghdr *nlh;
    struct inet_diag_req_v2 *destroy_req;

    nlh = mnl_nlmsg_put_header(destroy_buf);
    nlh->nlmsg_type = SOCK_DESTROY;
    //TODO: Add ACK so we can output some sensible error messages
    nlh->nlmsg_flags = NLM_F_REQUEST;

    destroy_req = mnl_nlmsg_put_extra_header(nlh, sizeof(struct inet_diag_req_v2));
    //TODO: Add 4/6 flag to command line
    destroy_req->sdiag_family = diag_msg->idiag_family;
    destroy_req->sdiag_protocol = IPPROTO_TCP;

    //Copy ID from diag_msg returned by kernel
    destroy_req->id = diag_msg->id;

    mnl_socket_sendto(ctx->diag_destroy_socket, destroy_buf, nlh->nlmsg_len);
}

static void parse_diag_msg(struct tcp_closer_ctx *ctx, struct inet_diag_msg *diag_msg,
                           int payload_len)
{
    struct nlattr *attr;
    struct tcp_info *tcpi = NULL;
    char local_addr_buf[INET6_ADDRSTRLEN];
    char remote_addr_buf[INET6_ADDRSTRLEN];
    struct passwd *uid_info = NULL;

    memset(local_addr_buf, 0, sizeof(local_addr_buf));
    memset(remote_addr_buf, 0, sizeof(remote_addr_buf));

    //(Try to) Get user info
    uid_info = getpwuid(diag_msg->idiag_uid);

    if(diag_msg->idiag_family == AF_INET){
        inet_ntop(AF_INET, (struct in_addr*) &(diag_msg->id.idiag_src), 
            local_addr_buf, INET_ADDRSTRLEN);
        inet_ntop(AF_INET, (struct in_addr*) &(diag_msg->id.idiag_dst), 
            remote_addr_buf, INET_ADDRSTRLEN);
    } else {
        inet_ntop(AF_INET6, (struct in_addr6*) &(diag_msg->id.idiag_src),
                local_addr_buf, INET6_ADDRSTRLEN);
        inet_ntop(AF_INET6, (struct in_addr6*) &(diag_msg->id.idiag_dst),
                remote_addr_buf, INET6_ADDRSTRLEN);
    }

    attr = (struct nlattr*) (diag_msg+1);
    payload_len -= sizeof(struct inet_diag_msg);

    while(mnl_attr_ok(attr, payload_len)){
        if (attr->nla_type != INET_DIAG_INFO) {
            payload_len -= attr->nla_len;
            attr = mnl_attr_next(attr);
            continue;
        }

        tcpi = (struct tcp_info*) mnl_attr_get_payload(attr);
        break;
    }

    //No need to check for tcpi, if it could not be attached then message would
    //not be send from kernel

    if (ctx->verbose_mode) {
        fprintf(stdout, "Found connection:\nUser: %s (UID: %u) Src: %s:%d Dst: %s:%d\n",
                uid_info == NULL ? "Not found" : uid_info->pw_name,
                diag_msg->idiag_uid, local_addr_buf, ntohs(diag_msg->id.idiag_sport),
                remote_addr_buf, ntohs(diag_msg->id.idiag_dport));
        fprintf(stdout, "\tState: %s RTT: %gms (var. %gms) "
                "Recv. RTT: %gms Snd_cwnd: %u/%u "
                "Last_data_recv: %ums ago\n",
                tcp_states_map[tcpi->tcpi_state],
                (double) tcpi->tcpi_rtt/1000,
                (double) tcpi->tcpi_rttvar/1000,
                (double) tcpi->tcpi_rcv_rtt/1000,
                tcpi->tcpi_unacked,
                tcpi->tcpi_snd_cwnd,
                tcpi->tcpi_last_data_recv);
    }

    //tcp_last_ack_recv can be updated by for example a proxy replying to TCP
    //keep-alives, so we only check tcpi_last_data_recv. This timer keeps track
    //of actual data going through the connection
    if (ctx->idle_time && tcpi->tcpi_last_data_recv < ctx->idle_time) {
        return;
    }

    fprintf(stdout, "Will destroy src: %s:%d dst: %s:%d last_data_recv: %ums\n",
            local_addr_buf, ntohs(diag_msg->id.idiag_sport),  remote_addr_buf,
            ntohs(diag_msg->id.idiag_dport), tcpi->tcpi_last_data_recv);

    if (ctx->use_netlink) {
        destroy_socket(ctx, diag_msg);
    } else {
        destroy_socket_proc(diag_msg->idiag_inode);
    }
}

int32_t recv_diag_msg(struct tcp_closer_ctx *ctx)
{
    struct nlmsghdr *nlh;
    struct nlmsgerr *err;
    uint8_t recv_buf[MNL_SOCKET_BUFFER_SIZE];
    struct inet_diag_msg *diag_msg;
    int32_t numbytes, payload_len;

    while(1){
        numbytes = mnl_socket_recvfrom(ctx->diag_dump_socket, recv_buf, sizeof(recv_buf));
        nlh = (struct nlmsghdr*) recv_buf;

        while(mnl_nlmsg_ok(nlh, numbytes)){
            if(nlh->nlmsg_type == NLMSG_DONE) {
                return 0;
            }

            if(nlh->nlmsg_type == NLMSG_ERROR){
                err = NLMSG_DATA(nlh);

                if (err->error) {
                    fprintf(stderr, "Error in netlink message: %s (%u)\n",
                            strerror(-err->error), -err->error);
                    return 1;
                }
            }

            fprintf(stdout, "Got diag msg\n");

            //TODO: Switch these to mnl too
            diag_msg = mnl_nlmsg_get_payload(nlh);
            payload_len = mnl_nlmsg_get_payload_len(nlh);
            parse_diag_msg(ctx, diag_msg, payload_len);
            
            nlh = mnl_nlmsg_next(nlh, &numbytes);
        }
    }

    return 0;
}