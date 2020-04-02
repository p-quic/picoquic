#include "bpf.h"

#define UNUSABLE_RTT_COEF 8
#define COOLDOWN_RTT_COEF 8

protoop_arg_t path_manager(picoquic_cnx_t* cnx) {
    /* Now, even the server MUST itself setup its sending paths */
    bpf_data *bpfd = get_bpf_data(cnx);
    uniflow_data_t *ud = NULL;

    /* Don't go further if the address exchange is not complete! */
    if (!bpfd->nb_sending_proposed || !bpfd->nb_receiving_proposed) {
        PROTOOP_PRINTF(cnx, "Address exchange is not complete\n");
        return 0;
    }

    /* FIXME this is not really an issue per-se, but we need to prioritize then on which addresses we will create paths */
    if (bpfd->nb_loc_addrs * bpfd->nb_rem_addrs > MAX_SENDING_UNIFLOWS) {
        PROTOOP_PRINTF(cnx, "%d max paths is not enough for a full mesh between %d loc and %d rem addrs\n", MAX_SENDING_UNIFLOWS, bpfd->nb_loc_addrs, bpfd->nb_rem_addrs);
        return 0;
    }

#ifdef PATH_MONITORING
    uint64_t now = picoquic_current_time();
    for (int i = 0; i < bpfd->nb_sending_proposed; i++) {
        ud = bpfd->sending_uniflows[i];
        if (ud->state == uniflow_active) {
            picoquic_packet_context_t *ctx = (picoquic_packet_context_t *) get_path(ud->path, AK_PATH_PKT_CTX, picoquic_packet_context_application);
            if (get_pkt_ctx(ctx, AK_PKTCTX_LATEST_PROGRESS_TIME) < get_pkt_ctx(ctx, AK_PKTCTX_LATEST_RETRANSMIT_TIME) && get_pkt_ctx(ctx, AK_PKTCTX_LATEST_RETRANSMIT_TIME) + (UNUSABLE_RTT_COEF * get_path(pd->path, AK_PATH_SMOOTHED_RTT, 0)) < now) {
                ud->state = uniflow_unusable;
                ud->failure_count++;
                ud->cooldown_time = now + ((COOLDOWN_RTT_COEF * get_path(ud->path, AK_PATH_SMOOTHED_RTT, 0)) << ud->failure_count);
                bpfd->nb_sending_active--;

                LOG {
                    char from[48], to[48];
                    struct sockaddr *laddr = bpfd->loc_addrs[pd->loc_addr_id - 1].sa;
                    struct sockaddr *raddr = bpfd->rem_addrs[pd->rem_addr_id - 1].sa;
                    LOG_EVENT(cnx, "multipath", "sending_uniflow_unusable", "timeout",
                              "{\"uniflow_id\": %" PRIu64 ", \"path\": \"%p\", \"loc_addr\": \"%s\", \"rem_addr\": \"%s\", \"cooldown\": %" PRIu64 "}",
                              ud->uniflow_id, (protoop_arg_t) ud->path,
                              (protoop_arg_t) inet_ntop(laddr->sa_family, (laddr->sa_family == AF_INET)
                                                                          ? (void *) &(((struct sockaddr_in *) laddr)->sin_addr)
                                                                          : (void *) &(((struct sockaddr_in6 *) laddr)->sin6_addr),
                                                        from, sizeof(from)),
                              (protoop_arg_t) inet_ntop(raddr->sa_family, (raddr->sa_family == AF_INET)
                                                                          ? (void *) &(((struct sockaddr_in *) raddr)->sin_addr)
                                                                          : (void *) &(((struct sockaddr_in6 *) raddr)->sin6_addr),
                                                        to, sizeof(to)),
                              ud->cooldown_time);
                }
            }
        }
    }

    for (int i = 0; i < bpfd->nb_sending_proposed; i++) {
        ud = bpfd->uniflows[i];
        if (ud->state == uniflow_unusable && ud->cooldown_time < now) {
            ud->state = uniflow_closed;

            LOG {
                char from[48], to[48];
                struct sockaddr *laddr = bpfd->loc_addrs[pd->loc_addr_id - 1].sa;
                struct sockaddr *raddr = bpfd->rem_addrs[pd->rem_addr_id - 1].sa;
                LOG_EVENT(cnx, "multipath", "sending_uniflow_closed", "cooldown",
                          "{\"uniflow_id\": %" PRIu64 ", \"path\": \"%p\", \"loc_addr\": \"%s\", \"rem_addr\": \"%s\"}",
                          ud->uniflow_id, (protoop_arg_t) ud->path,
                          (protoop_arg_t) inet_ntop(laddr->sa_family, (laddr->sa_family == AF_INET)
                                                                      ? (void *) &(((struct sockaddr_in *) laddr)->sin_addr)
                                                                      : (void *) &(((struct sockaddr_in6 *) laddr)->sin6_addr),
                                                    from, sizeof(from)),
                          (protoop_arg_t) inet_ntop(raddr->sa_family, (raddr->sa_family == AF_INET)
                                                                      ? (void *) &(((struct sockaddr_in *) raddr)->sin_addr)
                                                                      : (void *) &(((struct sockaddr_in6 *) raddr)->sin6_addr),
                                                    to, sizeof(to)),
                );
            }
            reserve_path_update(cnx, ud->uniflow_id, 0);
        }
    }
#endif

    if (bpfd->nb_sending_active >= N_SENDING_UNIFLOWS) {
        return 0;
    }

    for (int loc = 0; loc < bpfd->nb_loc_addrs; loc++) {
        addr_data_t *adl = &bpfd->loc_addrs[loc];
        for (int rem = 0; adl->sa && rem < bpfd->nb_rem_addrs; rem++) {
            addr_data_t *adr = &bpfd->rem_addrs[rem];

            for (int uniflow_idx = 0; adr->sa && uniflow_idx < bpfd->nb_sending_proposed; uniflow_idx++) {
                ud = bpfd->sending_uniflows[uniflow_idx];
                if (ud->state == uniflow_ready && bpfd->nb_sending_active < N_SENDING_UNIFLOWS) {
                    ud->state = uniflow_active;
                    ud->loc_addr_id = (uint8_t) (loc + 1);
                    ud->rem_addr_id = (uint8_t) (rem + 1);
                    set_path(ud->path, AK_PATH_LOCAL_ADDR_LEN, 0, (adl->is_v6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                    my_memcpy((struct sockaddr_storage *) get_path(ud->path, AK_PATH_LOCAL_ADDR, 0), adl->sa, (adl->is_v6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                    set_path(ud->path, AK_PATH_IF_INDEX_LOCAL, 0, (unsigned long) adl->if_index);
                    set_path(ud->path, AK_PATH_PEER_ADDR_LEN, 0, (adr->is_v6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                    my_memcpy((struct sockaddr_storage *) get_path(ud->path, AK_PATH_PEER_ADDR, 0), adr->sa, (adr->is_v6) ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in));
                    bpfd->nb_sending_active++;

                    LOG {
                        char from[48], to[48];
                        LOG_EVENT(cnx, "multipath", "uniflow_activated", "",
                                  "{\"uniflow_id\": %" PRIu64 ", \"path\": \"%p\", \"loc_addr\": \"%s\", \"rem_addr\": \"%s\"}",
                                  ud->uniflow_id, (protoop_arg_t) ud->path,
                                  (protoop_arg_t) inet_ntop(adl->sa->sa_family, (adl->sa->sa_family == AF_INET)
                                                                                ? (void *) &(((struct sockaddr_in *) adl->sa)->sin_addr)
                                                                                : (void *) &(((struct sockaddr_in6 *) adl->sa)->sin6_addr),
                                                            from, sizeof(from)),
                                  (protoop_arg_t) inet_ntop(adr->sa->sa_family, (adr->sa->sa_family == AF_INET)
                                                                                ? (void *) &(((struct sockaddr_in *) adr->sa)->sin_addr)
                                                                                : (void *) &(((struct sockaddr_in6 *) adr->sa)->sin6_addr),
                                                            to, sizeof(to))
                        );
                    }
                    break;
                } else if ((ud->state == uniflow_active || ud->state == uniflow_unusable) &&
                           (picoquic_compare_addr((struct sockaddr *) get_path(ud->path, AK_PATH_PEER_ADDR, 0), bpfd->rem_addrs[rem].sa) == 0 &&
                            picoquic_compare_addr((struct sockaddr *) get_path(ud->path, AK_PATH_LOCAL_ADDR, 0), bpfd->loc_addrs[loc].sa) == 0)) {
                    break;
                }
            }
        }
    }

    return 0;
}