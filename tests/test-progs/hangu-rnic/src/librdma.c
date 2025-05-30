#include "librdma.h"

#include <stdio.h>
#include "libhgrnic.h"


/**
 * @note Post Recv request for QP0 (Connection Management QP)
 * @return returns actual posted recv wr.
 *         0 for not acquire. -1 for error.
 */
//连接建立相关
int cm_post_recv(struct ibv_context *ctx, int wr_num) {
    struct ibv_wqe *recv_wqe = (struct ibv_wqe *)malloc(sizeof(struct ibv_wqe) * wr_num);
    int i;
    
    /* Build Receive WQE */
    for (i = 0; (ctx->cm_rcv_num < RCV_WR_MAX) && (i < wr_num); ++i) {
        recv_wqe[i].length = sizeof(struct rdma_cr);
        recv_wqe[i].mr = ctx->cm_mr;
        recv_wqe[i].offset = ctx->cm_rcv_posted_off;
        recv_wqe[i].trans_type = IBV_TYPE_RECV;

        ++ctx->cm_rcv_num;
        ctx->cm_rcv_posted_off += sizeof(struct rdma_cr);
        if (ctx->cm_rcv_posted_off + sizeof(struct rdma_cr) > RCV_WR_MAX * sizeof(struct rdma_cr)) {
            ctx->cm_rcv_posted_off = 0;
        }
        // RDMA_PRINT(librdma, "cm_post_recv : %d, flag 0x%x base addr 0x%lx, off 0x%lx\n", 
        //     i, ((struct rdma_cr *)(recv_wqe[i].mr->addr + recv_wqe[i].offset))->flag, (uint64_t)recv_wqe[i].mr->addr, (uint64_t)recv_wqe[i].offset);
    }
    
    ibv_post_recv(ctx, recv_wqe, ctx->cm_qp, i);
    free(recv_wqe);
    // RDMA_PRINT(librdma, "cm_post_recv : exit\n");
    return i;
}

/**
 * @note Post Send request for QP0 (Connection Management QP)
 * @return Returns actual posted send wr.
 *         0 for not acquire. -1 for error.
 */
//连接建立相关
int cm_post_send(struct ibv_context *ctx, struct rdma_cr *cr_info, int wr_num, uint16_t dlid) {
    struct ibv_wqe *send_wqe = (struct ibv_wqe *)malloc(sizeof(struct ibv_wqe) * wr_num);
    struct rdma_cr *cm_mr;

    if (wr_num > SND_WR_MAX) {
        wr_num = SND_WR_MAX;
        RDMA_PRINT(librdma, "cm_post_send: There's not enough room for CM to post send!\n");
        assert(wr_num < SND_WR_MAX);
    }
    
    for (int i = 0; i < wr_num; ++i) {

        cm_mr = (struct rdma_cr *)(ctx->cm_mr->addr + ctx->cm_snd_off);
        memcpy(cm_mr, &(cr_info[i]), sizeof(struct rdma_cr));
        
        send_wqe[i].length = sizeof(struct rdma_cr);
        send_wqe[i].mr = ctx->cm_mr;
        send_wqe[i].offset = ctx->cm_snd_off;

        /* Add Send element */
        send_wqe[i].trans_type = IBV_TYPE_SEND;
        send_wqe[i].send.dlid = dlid;
        send_wqe[i].send.dqpn = ctx->cm_qp->qp_num;
        send_wqe[i].send.qkey = QKEY_CM;

        // RDMA_PRINT(librdma, "cm_post_send[%d]: flag 0x%x base addr 0x%lx, off 0x%lx\n", 
        //     i, cr_info[i].flag, (uint64_t)send_wqe[i].mr->addr, (uint64_t)send_wqe[i].offset);

        ctx->cm_snd_off += sizeof(struct rdma_cr);
        if (ctx->cm_snd_off + sizeof(struct rdma_cr) > SND_WR_BASE + SND_WR_MAX * sizeof(struct rdma_cr)) {
            ctx->cm_snd_off = SND_WR_BASE;
        }
    }
    ibv_post_send(ctx, send_wqe, ctx->cm_qp, wr_num);//用qkey
    free(send_wqe);
    // RDMA_PRINT(librdma, "cm_post_send: after free\n");

    return wr_num;
}

//一个入口函数
//rdma资源初始化
struct rdma_resc *rdma_resc_init(int num_mr, int num_cq, int num_qp, uint16_t llid, int num_rem) {
    int i = 0;

    /* Allocate memory for struct rdma_resc */
    struct rdma_resc *resc = (struct rdma_resc *)malloc(sizeof(struct rdma_resc));
    memset(resc, 0, sizeof(struct rdma_resc));
    resc->num_mr  = num_mr;
    resc->num_cq  = num_cq;
    resc->num_qp  = num_qp;
    resc->num_rem = num_rem;
    resc->mr = (struct ibv_mr **)malloc(sizeof(struct ibv_mr*) * num_mr);
    resc->cq = (struct ibv_cq **)malloc(sizeof(struct ibv_cq*) * num_cq);
    resc->qp = (struct ibv_qp **)malloc(sizeof(struct ibv_qp*) * num_qp * num_rem);//连接数*每个连接的QP数
    resc->rinfo = (struct rem_info *)malloc(sizeof(struct rem_info) * num_rem);

    /* device initialization */
    struct ibv_context *ctx = (struct ibv_context *)malloc(sizeof(struct ibv_context));
    //打开设备，建立第一个进程的QP0（连接建立QP），初始化QP0的CQ,SQ,RQ，接收发送的MR
    //这个ctx应该是初始化内容的元数据
    ibv_open_device(ctx, llid);
    resc->ctx = ctx;
    RDMA_PRINT(librdma, "ibv_open_device : doorbell address 0x%lx\n", (long int)ctx->dvr);

    /* Post receive to CM */
    //为QP0设立接收区并往RQ放入一个receive的WQE
    cm_post_recv(ctx, RCV_WR_MAX);

    /* Create MR */
    struct ibv_mr_init_attr mr_attr;
    mr_attr.length = 1 << 12;//一页
    mr_attr.flag = MR_FLAG_RD | MR_FLAG_WR | MR_FLAG_LOCAL | MR_FLAG_REMOTE;
    for (i = 0; i < num_mr; ++i) {
        resc->mr[i] = ibv_reg_mr(ctx, &mr_attr);
        RDMA_PRINT(librdma, "ibv_reg_mr: lkey 0x%x(%d)\n", resc->mr[i]->lkey, resc->mr[i]->lkey&RESC_LIM_MASK);
    }
    RDMA_PRINT(librdma, "Init MR finish!\n");

    /* Create CQ */
    struct ibv_cq_init_attr cq_attr;
    cq_attr.size_log = 12;
    for (i = 0; i < num_cq; ++i) {
        resc->cq[i] = ibv_create_cq(ctx, &cq_attr);
        RDMA_PRINT(librdma, "ibv_create_cq: cqn 0x%x(%d)\n", resc->cq[i]->cq_num, resc->cq[i]->cq_num&RESC_LIM_MASK);
    }
    RDMA_PRINT(librdma, "Init CQ finish!\n");

    /* Create QP */
    struct ibv_qp_create_attr qp_attr;
    qp_attr.sq_size_log = 12;
    qp_attr.rq_size_log = 12;
    // for (i = 0; i < num_qp * num_rem; ++i) {
    //     resc->qp[i] = ibv_create_qp(ctx, &qp_attr);
    //     RDMA_PRINT(librdma, "ibv_create_qp: qpn %d\n", resc->qp[i]->qp_num);
    // }
    struct ibv_qp *qp_tmp = ibv_create_batch_qp(ctx, &qp_attr, num_qp * num_rem);
    for (i = 0; i < num_qp * num_rem; ++i) {
        resc->qp[i] = &(qp_tmp[i]);
        // RDMA_PRINT(librdma, "ibv_create_qp: qpn 0x%x(%d)\n", resc->qp[i]->qp_num, resc->qp[i]->qp_num&RESC_LIM_MASK);
    }
    RDMA_PRINT(librdma, "Init QP finish\n");

    for (i = 0; i < resc->num_rem; ++i) {
        /* Initialize remote info */
        resc->rinfo[i].sum = 0;
        resc->rinfo[i].start_off = i * resc->num_qp;
        resc->rinfo[i].sync_flag = 0;
    }

    /* Init cpl descriptor */
    resc->desc = (struct cpl_desc **)malloc(sizeof(struct cpl_desc *) * MAX_CPL_NUM);
    for (int i = 0; i < MAX_CPL_NUM; ++i) {
        resc->desc[i] = (struct cpl_desc *)malloc(sizeof(struct cpl_desc));
    }
    
    return resc;
}

int rdma_resc_destroy(struct rdma_resc * resc) {
    for (int i = 0; i < MAX_CPL_NUM; ++i) {
        free(resc->desc[i]);
    }
    free(resc->desc);
}

/**
 * @note: Listen connect management request
 * @input
 * @param resc stores all rdma related resources
 * @output
 * @param cr_info returned connection request information
 * @return returns number of requests. 
 *         0 for not acquire. -1 for error.
 */
//返回听到的来自MR的接受区域的东西，不包括同步信息
struct rdma_cr *rdma_listen(struct rdma_resc *resc, int *cm_cpl_num) {
    struct ibv_context *ctx = resc->ctx;
    struct rdma_cr *cr_info = NULL;
    int cnt = 0; /* count recv cpl */
    uint32_t req_cnt = 0;

    struct cpl_desc **desc = resc->desc;
    for (int i = 0; i < 10; ++i) {
        int res = ibv_poll_cpl(ctx->cm_cq, desc, MAX_CPL_NUM);//res是得到的读取CQ数目
        if (res) {//如果轮询成功
            
            RDMA_PRINT(librdma, "rdma_listen: ibv_poll_cpl finish ! return is %d, cpl_cnt %d\n", res, ctx->cm_cq->cpl_cnt);

            for (int j  = 0; j < res; ++j) {
                if (desc[j]->trans_type == IBV_TYPE_RECV) {
                    ++cnt;//记录完成的IBV_TYPE_RECV类任务数目
                    RDMA_PRINT(librdma, "rdma_listen: ibv_poll_cpl recv %d bytes CR.\n", desc[j]->byte_cnt);
                }
            }
            break;
        }
    }

    /* Fetch rdma_cr from cm_mr */
    if (cnt) {
        cr_info = (struct rdma_cr *)malloc(sizeof(struct rdma_cr) * cnt);
    }
    for (int i = 0; i < cnt; ++i) {
        struct rdma_cr *cr_tmp = ((struct rdma_cr *)(ctx->cm_mr->addr + ctx->cm_rcv_acked_off));

        /* This is a sync pkt, we need to record the sync record */
        if (cr_tmp->flag == CR_TYPE_SYNC) {

            /* find the sync req, and store it */
            for (int j = 0; j < resc->num_rem; ++j) {
                if (cr_tmp->src_lid == resc->rinfo[j].dlid) {
                    resc->rinfo[j].sync_flag = 1;//同步完成
                    break;
                }
            }
            RDMA_PRINT(librdma, "rdma_listen: CR_TYPE_SYNC\n");
        } else {
            memcpy(&(cr_info[req_cnt]), cr_tmp, sizeof(struct rdma_cr));//存储这个cr_tmp
            ++req_cnt;
        }

        /* Clear cpl data */
        cr_tmp->flag = CR_TYPE_NULL;
        
        --ctx->cm_rcv_num;//完成一个receive任务
        ctx->cm_rcv_acked_off += sizeof(struct rdma_cr);
        if (ctx->cm_rcv_acked_off + sizeof(struct rdma_cr) > RCV_WR_MAX * sizeof(struct rdma_cr)) {
            ctx->cm_rcv_acked_off = 0;
        }
    }

    /* Post CM recv to RQ */
    //如果RQ没满，可再发送一个receive的WQE
    if (ctx->cm_rcv_num < RCV_WR_MAX) {
        int rcv_wqe_num = cm_post_recv(ctx, RCV_WR_MAX);
        RDMA_PRINT(librdma, "rdma_listen: Replenish %d Recv WQEs\n", rcv_wqe_num);
    }

    *cm_cpl_num = req_cnt;
    if (*cm_cpl_num == 0 && cnt != 0) {
        free(cr_info);
        cr_info = NULL;
    }
    return cr_info;
}
//发送一个send
int rdma_connect(struct rdma_resc *resc, struct rdma_cr *cr_info, uint16_t *dest_info, int cm_req_num) {
    struct ibv_context *ctx = resc->ctx;

    int i = 0, cnt;
    while (i < cm_req_num) {
        cnt = 0;
        while (i + cnt < cm_req_num) {
            if (cnt == SND_WR_MAX) { /* in case that post send 
                                      * cm req surpass specified */
                break;
            } else if (dest_info[cnt + i] == dest_info[i]) {
                ++cnt;
            } else {
                break;
            }
        }

        /* Post Same destination in one doorbell */
        RDMA_PRINT(librdma, "rdma_connect: cm_post_send dest_info 0x%x cnt %d\n", dest_info[i], cnt);

        cm_post_send(ctx, &(cr_info[i]), cnt, dest_info[i]);
        i += cnt;
    }
    
}

/**
 * @note poll CR Receive Completion.
 * 
 * 
 */
struct cpl_desc **rdma_poll_cm_rcv_cpl(struct rdma_resc *resc, int *cnt) {
    struct ibv_context *ctx = resc->ctx;
    struct rdma_cr *cr_info;
    struct cpl_desc **desc = resc->desc;
        
    /* Fetch Recv cpl in CQ */
    *cnt = 0;
    while (1) {
        trans_wait(resc->ctx);
        int res = ibv_poll_cpl(ctx->cm_cq, desc, MAX_CPL_NUM);
        
        if (res) {
            
            RDMA_PRINT(librdma, "rdma_poll_cm_rcv_cpl: ibv_poll_cpl finish ! return is %d, cpl cnt %d\n", res, ctx->cm_cq->cpl_cnt);

            for (int j  = 0; j < res; ++j) {
                RDMA_PRINT(librdma, "rdma_poll_cm_rcv_cpl: ibv_poll_cpl recv %d bytes CR Data.\n", desc[j]->byte_cnt);
                if (desc[j]->trans_type == IBV_TYPE_RECV) {
                    memcpy(desc[*cnt], desc[j], sizeof(struct cpl_desc));
                    ++(*cnt);
                }
            }
            break;
        }
    }
    return desc;
}

//向每一个建立的连接发送同步需求
int post_sync(struct rdma_resc *resc) {
    struct rdma_cr *cr_info = (struct rdma_cr *)malloc(sizeof(struct rdma_cr));
    
    memset(cr_info, 0, sizeof(struct rdma_cr));
    cr_info->flag = CR_TYPE_SYNC;//这是同步需求
    cr_info->src_lid = resc->ctx->lid;//告诉对方本lid
    
    for (int i = 0; i < resc->num_rem; ++i) {
        cm_post_send(resc->ctx, cr_info, 1, resc->rinfo[i].dlid);
    }
    free(cr_info);

    return 0;
}

//轮询同步信息
int poll_sync(struct rdma_resc *resc) {
    /* Recv Sync CR Data */
    struct ibv_context *ctx = resc->ctx;
    struct rdma_cr *cr_info;
    struct cpl_desc **desc = resc->desc;
    int cnt;
    int polled_sync_num = 0;

    /* count already synced */
    for (int i = 0; i < resc->num_rem; ++i) {
        if (resc->rinfo[i].sync_flag == 1) {
            ++polled_sync_num;
        }
    }
    //全部已同步完成
    if (polled_sync_num == resc->num_rem) {
        return 0;
    }

    while (1) {

        /* replenish cm recv wqe *///补充wqe
        if (ctx->cm_rcv_num < (RCV_WR_MAX / 2)) {
            int rcv_wqe_num = cm_post_recv(ctx, RCV_WR_MAX);
            RDMA_PRINT(librdma, "poll_sync: Replenish %d Recv WQEs\n", rcv_wqe_num);
        }
        
        /* Fetch Recv cpl in CQ */
        cnt = 0;
        while (1) {
            int res = ibv_poll_cpl(ctx->cm_cq, desc, MAX_CPL_NUM);
            // RDMA_PRINT(librdma, "poll_sync: (ibv_poll_cpl) finish ! return is %d, cpl cnt %d\n", res, ctx->cm_cq->cpl_cnt);
            
            if (res) {
                for (int j  = 0; j < res; ++j) {
                    // RDMA_PRINT(librdma, "poll_sync: (ibv_poll_cpl) finish! recv %d bytes, trans type is %d.\n", desc[j]->byte_cnt, desc[j]->trans_type);
                    /* count number of recv completion */
                    if (desc[j]->trans_type == IBV_TYPE_RECV) {
                        ++cnt;
                    }
                }
                break;
            }
        }

        RDMA_PRINT(librdma, "poll_sync: we got %d RECV CPL, cpl cnt %d, polled num %d\n", 
                cnt, ctx->cm_cq->cpl_cnt, polled_sync_num);
        
        for (int i = 0; i < cnt; ++i) {
            cr_info = (struct rdma_cr *)(ctx->cm_mr->addr + ctx->cm_rcv_acked_off + i * sizeof(struct rdma_cr));

            RDMA_PRINT(librdma, "poll_sync: show : cr_info flag 0x%x, cr_info->src_lid %d\n", 
                    cr_info->flag, cr_info->src_lid);
        }
        
        /* Read every Connection Request (CR) data in cm_mr */
        for (int i = 0; i < cnt; ++i) {
            cr_info = (struct rdma_cr *)(ctx->cm_mr->addr + ctx->cm_rcv_acked_off);

            RDMA_PRINT(librdma, "poll_sync: cr_info flag 0x%x, cr_info->src_lid %d\n", 
                    cr_info->flag, cr_info->src_lid);

            /* Update CM CPL pointer */
            --ctx->cm_rcv_num;
            ctx->cm_rcv_acked_off += sizeof(struct rdma_cr);
            if (ctx->cm_rcv_acked_off >= RCV_WR_MAX * sizeof(struct rdma_cr)) {
                ctx->cm_rcv_acked_off = 0;
            }

            /* This is a sync pkt, update related information */
            if (cr_info->flag == CR_TYPE_SYNC) {

                for (int j = 0; j < resc->num_rem; ++j) {
                    if (cr_info->src_lid == resc->rinfo[j].dlid) {
                        if (resc->rinfo[j].sync_flag == 0) {
                            resc->rinfo[j].sync_flag = 1;
                            ++polled_sync_num;
                        }
                        break;
                    }
                }
            }

            /* Clear CR cpl data */
            cr_info->flag = CR_TYPE_NULL;

            /* Sync polled all, Exit */
            if (polled_sync_num == resc->num_rem) {
                for (int j = 0; j < resc->num_rem; ++j) {
                    resc->rinfo[j].sync_flag = 0;//为啥是0
                }
                return 0;
            }
        }
    }

    return -1;
}

int rdma_recv_sync(struct rdma_resc *resc) {

    RDMA_PRINT(librdma, "rdma_recv_sync!\n");

    /* Recv Sync CR Data */
    poll_sync(resc);
    RDMA_PRINT(librdma, "rdma_recv_sync: Recv Sync CR Data\n");

    /* Find Sync Data, Send CR Sync Data back and Exit */
    post_sync(resc);
    RDMA_PRINT(librdma, "rdma_recv_sync: out\n");

    return 0;

}

int rdma_send_sync(struct rdma_resc *resc) {

    RDMA_PRINT(librdma, "rdma_send_sync!\n");

    /* Send Sync CR data */
    post_sync(resc);
    RDMA_PRINT(librdma, "rdma_send_sync: Send Sync CR data %d\n", resc->num_rem);

    /* Recv Sync CR Data */
    poll_sync(resc);
    RDMA_PRINT(librdma, "rdma_send_sync: Recv Sync CR Data %d\n", resc->num_rem);

    return 0;
}


