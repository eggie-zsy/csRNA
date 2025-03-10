
#include "libhgrnic.h"

#define SLEEP_CNT 1000

void wait(uint32_t n) {
    for (uint32_t i = 0; i < n; ++i);
}

int cpu_sync(struct ibv_context *context) {

    HGRNIC_PRINT("Start cpu_sync!\n");

    struct hghca_context *dvr = context->dvr;

    /* post completion to sync reg */
    dvr->sync[0] = 1;//像写doorbell一样，写寄存器的syncreg

    /* Wait for other CPUs comming */
    do {
        get_time(context);
        // wait(SLEEP_CNT);
    } while (dvr->sync[0] != 1);//读取sync读取的是syncSucc
    HGRNIC_PRINT("cpu_sync: wait for other CPUs comming!\n");

    /* post exit to sync reg */
    dvr->sync[0] = 0;

    /* Wait for other CPUs exit */
    do {
        get_time(context);
        // wait(SLEEP_CNT);
    } while (dvr->sync[0] != 0);

    HGRNIC_PRINT("cpu_sync: out!\n");
    return 0;
}


void trans_wait(struct ibv_context *context) {

    struct hghca_context *dvr = (struct hghca_context *)context->dvr;

    ioctl(dvr->fd, HGKFD_IOC_CHECK_GO, NULL);
}

//向硬件设备写入cmd，用户态驱动
uint8_t write_cmd(int fd, unsigned long request, void *args) {
    while (ioctl(fd, request, (void *)args)) {
        HGRNIC_PRINT(" %ld ioctl failed try again\n", request);
        wait(SLEEP_CNT);
        // usleep(1);
    }
    do {
        wait(SLEEP_CNT);
    } while (ioctl(fd, HGKFD_IOC_CHECK_GO, NULL));

    return 0;
}
//用户态驱动
//打开一个设备，初始化ICM，注册一个基础MR,创建基础QP和CQ，写入它们的基础CQC和QPC至RNIC中
int ibv_open_device(struct ibv_context *context, uint16_t lid) {

    context->lid = lid;
    
    /* Init fd */
    context->dvr = malloc(sizeof(struct hghca_context));
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    char file_name[100];
    sprintf(file_name, KERNEL_FILE_NAME "%d", cpu_id);
    dvr->fd = open(file_name, O_RDWR);//以读写模式打开设备
    HGRNIC_PRINT(" open /dev/hangu_rnic success!\n");

    /* map doorbell to user space */
    dvr->doorbell = mmap(NULL, DB_LEN, PROT_READ | PROT_WRITE, 
            MAP_SHARED, dvr->fd, 0);
    dvr->sync     = (void *)((uint64_t)dvr->doorbell + 8);
    HGRNIC_PRINT(" get dvr->doorbell 0x%lx\n", (uint64_t)dvr->doorbell);
    
    /* Init ICM */
    //初始化ICM，会更新RNIC中的cache基地址
    struct kfd_ioctl_init_dev_args *args = 
            (struct kfd_ioctl_init_dev_args *)malloc(sizeof(struct kfd_ioctl_init_dev_args));
    args->qpc_num_log = 16; /* useless here */
    args->cqc_num_log = 16; /* useless here */
    args->mpt_num_log = 19; /* useless here */
    args->mtt_num_log = 19; /* useless here */
    write_cmd(dvr->fd, HGKFD_IOC_INIT_DEV, (void *)args);//写入初始化指令
    free(args);
    
    /* Init communication management */
    struct ibv_mr_init_attr mr_attr;
    mr_attr.length = PAGE_SIZE;//4096字节
    mr_attr.flag = MR_FLAG_RD | MR_FLAG_WR | MR_FLAG_LOCAL;
    context->cm_mr = ibv_reg_mr(context, &mr_attr);   //注册一波MR，返回注册的MR的相关信息

    struct ibv_cq_init_attr cq_attr;
    cq_attr.size_log = PAGE_SIZE_LOG;
    context->cm_cq = ibv_create_cq(context, &cq_attr);
    HGRNIC_PRINT(" ibv_open_device cq lkey: 0x%x, vaddr 0x%lx, mtt_index 0x%x, paddr 0x%lx\n", 
            context->cm_cq->mr->lkey, (uint64_t)context->cm_cq->mr->addr, context->cm_cq->mr->mtt->mtt_index, context->cm_cq->mr->mtt->paddr);

    struct ibv_qp_create_attr qp_attr;
    qp_attr.sq_size_log = PAGE_SIZE_LOG;
    qp_attr.rq_size_log = PAGE_SIZE_LOG;
    context->cm_qp = ibv_create_qp(context, &qp_attr);

    context->cm_qp->ctx = context;
    context->cm_qp->type = QP_TYPE_UD;//建立连接的QP是UD连接
    context->cm_qp->cq = context->cm_cq;//好像是一个QP对应一个CQ，可能
    context->cm_qp->snd_wqe_offset = 0;
    context->cm_qp->rcv_wqe_offset = 0;
    context->cm_qp->lsubnet.llid = context->lid;
    context->cm_qp->qkey = QKEY_CM;
    ibv_modify_qp(context, context->cm_qp);

    context->cm_rcv_posted_off = RCV_WR_BASE;//这三个是什么意思
    context->cm_rcv_acked_off  = RCV_WR_BASE;
    context->cm_snd_off        = SND_WR_BASE;
    context->cm_rcv_num        = 0;

    HGRNIC_PRINT(" Exit ibv_open_device: out!\n");
    return 0;
}

//创建完成队列CQ，返回创建的CQ信息，CQ拥有访问MR的密钥lkey
//创建CQ需要alloc一个CQN,然后在MPT/MTT中注册转译信息，随后写CQC数据至CQCresc
//CQCresc的idx为cqnum，entry中包含访问MR的MPT密钥lkey
//ok
struct ibv_cq * ibv_create_cq(struct ibv_context *context, struct ibv_cq_init_attr *cq_attr) {

    HGRNIC_PRINT(" enter ibv_create_cq!\n");
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    struct ibv_cq *cq = (struct ibv_cq *)malloc(sizeof(struct ibv_cq));

    /* Allocate CQ */
    struct kfd_ioctl_alloc_cq_args *create_cq_args = 
            (struct kfd_ioctl_alloc_cq_args *)malloc(sizeof(struct kfd_ioctl_alloc_cq_args));
    write_cmd(dvr->fd, HGKFD_IOC_ALLOC_CQ, (void *)create_cq_args);
    cq->cq_num  = create_cq_args->cq_num;
    cq->ctx     = context;
    cq->offset  = 0;
    cq->cpl_cnt = 0;
    free(create_cq_args);
    
    
    /* Init (Allocate and write) MTT && MPT */
    struct ibv_mr_init_attr *mr_attr = 
            (struct ibv_mr_init_attr *)malloc(sizeof(struct ibv_mr_init_attr));
    mr_attr->flag   = MR_FLAG_RD | MR_FLAG_LOCAL;
    mr_attr->length = (1 << cq_attr->size_log); // (PAGE_SIZE << 2); // !TODO: Now the size is a fixed number of 1 page
    cq->mr = ibv_reg_mr(context, mr_attr);
    free(mr_attr);

    /* write CQC */
    struct kfd_ioctl_write_cqc_args *write_cqc_args = 
            (struct kfd_ioctl_write_cqc_args *)malloc(sizeof(struct kfd_ioctl_write_cqc_args));
    write_cqc_args->cq_num   = cq->cq_num;
    write_cqc_args->offset   = cq->offset;
    write_cqc_args->lkey     = cq->mr->lkey;
    write_cqc_args->size_log = PAGE_SIZE_LOG;
    write_cmd(dvr->fd, HGKFD_IOC_WRITE_CQC, (void *)write_cqc_args);
    free(write_cqc_args);
    return cq;
}

/**
 * @note Allocate a batch of QP, with conntinuous qpn and the same qp_attr
 */
struct ibv_qp * ibv_create_batch_qp(struct ibv_context *context, struct ibv_qp_create_attr *qp_attr, uint32_t batch_size) {

    HGRNIC_PRINT(" enter ibv_create_batch_qp!\n");

    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    struct ibv_qp *qp = (struct ibv_qp *)malloc(sizeof(struct ibv_qp) * batch_size);
    memset(qp, 0, sizeof(struct ibv_qp));

    /* allocate QP */
    uint32_t batch_cnt = 0;
    uint32_t batch_left = batch_size;
    struct kfd_ioctl_alloc_qp_args *qp_args = 
            (struct kfd_ioctl_alloc_qp_args *)malloc(sizeof(struct kfd_ioctl_alloc_qp_args));
    while (batch_left > 0) {

        uint32_t sub_bsz = (batch_left > MAX_QPC_BATCH) ? MAX_QPC_BATCH : batch_left;
        
        qp_args->batch_size = sub_bsz;
        write_cmd(dvr->fd, HGKFD_IOC_ALLOC_QP, (void *)qp_args);
        for (uint32_t i = 0; i < sub_bsz; ++i) {
            qp[batch_cnt + i].qp_num = qp_args->qp_num + i;
            // HGRNIC_PRINT(" Get out of HGKFD_IOC_ALLOC_QP! the %d-th qp, qpn is : 0x%x(%d)\n", batch_cnt + i, qp[batch_cnt + i].qp_num, qp[batch_cnt + i].qp_num&RESC_LIM_MASK);
        }

        batch_cnt  += sub_bsz;
        batch_left -= sub_bsz;
        assert(batch_cnt + batch_left == batch_size);
    }
    free(qp_args);

    // Init (Allocate and write) QP MTT && MPT
    struct ibv_mr_init_attr *mr_attr = 
            (struct ibv_mr_init_attr *)malloc(sizeof(struct ibv_mr_init_attr));
    mr_attr->flag   = MR_FLAG_WR | MR_FLAG_LOCAL;
    mr_attr->length = (1 << qp_attr->sq_size_log); // !TODO: Now the size is a fixed number of 1 page
    struct ibv_mr *tmp_mr = ibv_reg_batch_mr(context, mr_attr, batch_size * 2);
    for (uint32_t i = 0; i < batch_size; ++i) {
        qp[i].rcv_mr = &(tmp_mr[2 * i]);
        qp[i].snd_mr = &(tmp_mr[2 * i + 1]);
        HGRNIC_PRINT(" Get out of ibv_reg_batch_mr in create_qp! qpn is : 0x%x rcv_mr 0x%x snd_mr 0x%x\n", 
                qp[i].qp_num, qp[i].rcv_mr->lkey, qp[i].snd_mr->lkey);
    }
    free(mr_attr);

    return qp;
}

/**
 * @note Now, SQ and RQ has their own MR respectively.
 */
//创建SQ,RQ
//分配一个QPN,创建CQ和RQ在MR中的转译关系，返回创建的QP的信息
struct ibv_qp * ibv_create_qp(struct ibv_context *context, struct ibv_qp_create_attr *qp_attr) {

    HGRNIC_PRINT(" enter ibv_create_qp!\n");

    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    struct ibv_qp *qp = (struct ibv_qp *)malloc(sizeof(struct ibv_qp));
    memset(qp, 0, sizeof(struct ibv_qp));

    // allocate QP
    struct kfd_ioctl_alloc_qp_args *qp_args = 
            (struct kfd_ioctl_alloc_qp_args *)malloc(sizeof(struct kfd_ioctl_alloc_qp_args));
    qp_args->batch_size = 1;
    write_cmd(dvr->fd, HGKFD_IOC_ALLOC_QP, (void *)qp_args);
    qp->qp_num = qp_args->qp_num;
    HGRNIC_PRINT(" Get out of HGKFD_IOC_ALLOC_QP! qpn is : 0x%x\n", qp->qp_num);
    free(qp_args);

    // Init (Allocate and write) SQ MTT && MPT
    struct ibv_mr_init_attr *mr_attr = 
            (struct ibv_mr_init_attr *)malloc(sizeof(struct ibv_mr_init_attr));
    mr_attr->flag   = MR_FLAG_WR | MR_FLAG_LOCAL;
    mr_attr->length = (1 << qp_attr->sq_size_log); // !TODO: Now the size is a fixed number of 1 page
    qp->snd_mr = ibv_reg_mr(context, mr_attr);

    // Init (Allocate and write) RQ MTT && MPT 
    mr_attr->flag   = MR_FLAG_WR | MR_FLAG_LOCAL;
    mr_attr->length = (1 << qp_attr->rq_size_log); // !TODO: Now the size is a fixed number of 1 page
    qp->rcv_mr = ibv_reg_mr(context, mr_attr);
    HGRNIC_PRINT(" Get out of ibv_reg_mr in create_qp! qpn is : 0x%x\n", qp->qp_num);
    free(mr_attr);

    return qp;
}

int ibv_modify_batch_qp(struct ibv_context *context, struct ibv_qp *qp, uint32_t batch_size) {
    HGRNIC_PRINT(" enter ibv_modify_batch_qp!\n");
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;

    /* write QP */
    struct kfd_ioctl_write_qpc_args *qpc_args = 
            (struct kfd_ioctl_write_qpc_args *)malloc(sizeof(struct kfd_ioctl_write_qpc_args));
    memset(qpc_args, 0, sizeof(struct kfd_ioctl_write_qpc_args));
    uint32_t batch_cnt = 0;
    uint32_t batch_left = batch_size;
    while (batch_left > 0) {
        
        uint32_t sub_bsz = (batch_left > MAX_QPC_BATCH) ? MAX_QPC_BATCH : batch_left;
        HGRNIC_PRINT(" ibv_modify_batch_qp! batch_cnt %d batch_left %d sub_bsz %d\n", batch_cnt, batch_left, sub_bsz);

        qpc_args->batch_size = sub_bsz;
        for (int i = 0; i < sub_bsz; ++i) {
            qpc_args->flag    [i] = qp[batch_cnt + i].flag;
            qpc_args->type    [i] = qp[batch_cnt + i].type;
            qpc_args->llid    [i] = qp[batch_cnt + i].lsubnet.llid;
            qpc_args->dlid    [i] = qp[batch_cnt + i].dsubnet.dlid;
            qpc_args->src_qpn [i] = qp[batch_cnt + i].qp_num;
            qpc_args->dest_qpn[i] = qp[batch_cnt + i].dest_qpn;
            qpc_args->snd_psn [i] = qp[batch_cnt + i].snd_psn;
            qpc_args->ack_psn [i] = qp[batch_cnt + i].ack_psn;
            qpc_args->exp_psn [i] = qp[batch_cnt + i].exp_psn;
            qpc_args->cq_num  [i] = qp[batch_cnt + i].cq->cq_num;
            qpc_args->snd_wqe_base_lkey[i] = qp[batch_cnt + i].snd_mr->lkey;
            qpc_args->rcv_wqe_base_lkey[i] = qp[batch_cnt + i].rcv_mr->lkey;
            qpc_args->snd_wqe_offset   [i] = qp[batch_cnt + i].snd_wqe_offset;
            qpc_args->rcv_wqe_offset   [i] = qp[batch_cnt + i].rcv_wqe_offset;
            qpc_args->qkey       [i] = qp[batch_cnt + i].qkey;
            qpc_args->sq_size_log[i] = PAGE_SIZE_LOG; // qp->snd_mr->length;
            qpc_args->rq_size_log[i] = PAGE_SIZE_LOG; // qp->rcv_mr->length;

            // HGRNIC_PRINT(" ibv_modify_batch_qp! qpn 0x%x\n", qp[batch_cnt + i].qp_num);
        }
        write_cmd(dvr->fd, HGKFD_IOC_WRITE_QPC, qpc_args);
        
        batch_cnt  += sub_bsz;
        batch_left -= sub_bsz;
        assert(batch_cnt + batch_left == batch_size);
    }
    free(qpc_args);
    
    HGRNIC_PRINT(" ibv_modify_batch_qp: out!\n");
    return 0;
}

//或许是写QPC【初始化并写入】
int ibv_modify_qp(struct ibv_context *context, struct ibv_qp *qp) {
    HGRNIC_PRINT(" enter ibv_modify_qp!\n");
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;

    /* write QP */
    struct kfd_ioctl_write_qpc_args *qpc_args = 
            (struct kfd_ioctl_write_qpc_args *)malloc(sizeof(struct kfd_ioctl_write_qpc_args));
    memset(qpc_args, 0, sizeof(struct kfd_ioctl_write_qpc_args));
    qpc_args->batch_size = 1;
    qpc_args->flag    [0] = qp->flag;
    qpc_args->type    [0] = qp->type;
    qpc_args->llid    [0] = qp->lsubnet.llid;
    qpc_args->dlid    [0] = qp->dsubnet.dlid;
    qpc_args->src_qpn [0] = qp->qp_num;
    qpc_args->dest_qpn[0] = qp->dest_qpn;
    qpc_args->snd_psn [0] = qp->snd_psn;
    qpc_args->ack_psn [0] = qp->ack_psn;
    qpc_args->exp_psn [0] = qp->exp_psn;
    qpc_args->cq_num  [0] = qp->cq->cq_num;
    qpc_args->snd_wqe_base_lkey[0] = qp->snd_mr->lkey;//QP访问发送和接收描述符使用的lkey为SQ/RQ的MR密钥
    qpc_args->rcv_wqe_base_lkey[0] = qp->rcv_mr->lkey;
    qpc_args->snd_wqe_offset   [0] = qp->snd_wqe_offset;
    qpc_args->rcv_wqe_offset   [0] = qp->rcv_wqe_offset;
    qpc_args->qkey       [0] = qp->qkey;
    qpc_args->sq_size_log[0] = PAGE_SIZE_LOG; // qp->snd_mr->length;
    qpc_args->rq_size_log[0] = PAGE_SIZE_LOG; // qp->rcv_mr->length;
    write_cmd(dvr->fd, HGKFD_IOC_WRITE_QPC, qpc_args);
    free(qpc_args);
    return 0;
}
//批处理注册MR
struct ibv_mr * ibv_reg_batch_mr(struct ibv_context *context, struct ibv_mr_init_attr *mr_attr, uint32_t batch_size) {
    HGRNIC_PRINT(" ibv_reg_batch_mr!\n");
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    struct ibv_mr *mr =  (struct ibv_mr *)malloc(sizeof(struct ibv_mr) * batch_size);

    uint32_t batch_cnt = 0;
    uint32_t batch_left = batch_size;
    struct kfd_ioctl_init_mtt_args *mtt_args =
                (struct kfd_ioctl_init_mtt_args *)malloc(sizeof(struct kfd_ioctl_init_mtt_args));
    struct kfd_ioctl_alloc_mpt_args *mpt_alloc_args = 
                (struct kfd_ioctl_alloc_mpt_args *)malloc(sizeof(struct kfd_ioctl_alloc_mpt_args));
    struct kfd_ioctl_write_mpt_args *mpt_args = 
                (struct kfd_ioctl_write_mpt_args *)malloc(sizeof(struct kfd_ioctl_write_mpt_args));
    while (batch_left > 0) {
        uint32_t sub_bsz = 0;
        sub_bsz = (batch_left > MAX_MR_BATCH) ? MAX_MR_BATCH : batch_left;//如果大于最大批处理规模，按最大的来
        
        /* Init (Allocate and write) MTT */
        for (uint32_t i = 0; i < sub_bsz; ++i) {
            /* Calc needed number of pages */
            mr[batch_cnt + i].num_mtt = (mr_attr->length >> 12) + (mr_attr->length & 0xFFF) ? 1 : 0;
            assert(mr[batch_cnt + i].num_mtt == 1);//每块MR占用一页
            
            /* !TODO: Now, we require allocated memory's start 
            * vaddr is at the boundry of one page */ 
            mr[batch_cnt + i].addr   = memalign(PAGE_SIZE, mr_attr->length);
            memset(mr[batch_cnt + i].addr, 0, mr_attr->length);
            mr[batch_cnt + i].ctx = context;
            mr[batch_cnt + i].flag   = mr_attr->flag;
            mr[batch_cnt + i].length = mr_attr->length;
            mr[batch_cnt + i].mtt    = (struct ibv_mtt *)malloc(sizeof(struct ibv_mtt) * mr->num_mtt);

            mr[batch_cnt + i].mtt[0].vaddr = (void *)(mr[batch_cnt + i].addr);
            mtt_args->vaddr[i] = mr[batch_cnt + i].mtt[0].vaddr;
        }
        mtt_args->batch_size = sub_bsz;
        write_cmd(dvr->fd, HGKFD_IOC_ALLOC_MTT, (void *)mtt_args);
        for (uint32_t i = 0; i < sub_bsz; ++i) {
            mr[batch_cnt + i].mtt[0].mtt_index = mtt_args->mtt_index + i;
            mr[batch_cnt + i].mtt[0].paddr = mtt_args->paddr[i];
        }
        mtt_args->batch_size = sub_bsz;
        write_cmd(dvr->fd, HGKFD_IOC_WRITE_MTT, (void *)mtt_args);

        /* Allocate MPT */
        mpt_alloc_args->batch_size = sub_bsz;
        write_cmd(dvr->fd, HGKFD_IOC_ALLOC_MPT, (void *)mpt_alloc_args);
        for (uint32_t i = 0; i < sub_bsz; ++i) {
            mr[batch_cnt + i].lkey = mpt_alloc_args->mpt_index + i;
            assert(mr[batch_cnt + i].lkey == mr[batch_cnt + i].mtt->mtt_index);
            // HGRNIC_PRINT(" ibv_reg_batch_mr: mpt_idx 0x%x mtt_idx 0x%x\n", mr[batch_cnt + i].lkey, mr[batch_cnt + i].mtt->mtt_index);
        }

        /* Write MPT */
        mpt_args->batch_size = sub_bsz;
        for (uint32_t i = 0; i < sub_bsz; ++i) {
            mpt_args->flag[i]      = mr[batch_cnt + i].flag;
            mpt_args->addr[i]      = (uint64_t) mr[batch_cnt + i].addr;
            mpt_args->length[i]    = mr[batch_cnt + i].length;
            mpt_args->mtt_index[i] = mr[batch_cnt + i].mtt[0].mtt_index;
            mpt_args->mpt_index[i] = mr[batch_cnt + i].lkey;
        }
        write_cmd(dvr->fd, HGKFD_IOC_WRITE_MPT, (void *)mpt_args);

        /* update finished  */
        batch_left -= sub_bsz;
        batch_cnt += sub_bsz;
        assert(batch_cnt + batch_left == batch_size);
    }
    free(mtt_args);
    free(mpt_alloc_args);
    free(mpt_args);

    HGRNIC_PRINT(" ibv_reg_batch_mr!: out!\n");
    return mr;
}
//注册MR，ok
struct ibv_mr * ibv_reg_mr(struct ibv_context *context, struct ibv_mr_init_attr *mr_attr) {
    HGRNIC_PRINT(" ibv_reg_mr!\n");
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    struct ibv_mr *mr =  (struct ibv_mr *)malloc(sizeof(struct ibv_mr));

    /* Calc needed number of pages */
    mr->num_mtt = (mr_attr->length >> 12) + (mr_attr->length & 0xFFF) ? 1 : 0;//1
    assert(mr->num_mtt == 1);
    
    /* !TODO: Now, we require allocated memory's start 
     * vaddr is at the boundry of one page */ 
    //MTT记载了MR的地址转译关系，一次性注册一页信息
    mr->addr   = memalign(PAGE_SIZE, mr_attr->length);//虚拟地址4096对齐的4096字节大小区域
    memset(mr->addr, 0, mr_attr->length);
    mr->ctx = context;
    mr->flag   = mr_attr->flag;
    mr->length = mr_attr->length;
    mr->mtt    = (struct ibv_mtt *)malloc(sizeof(struct ibv_mtt) * mr->num_mtt);
    for (uint64_t i = 0; i < mr->num_mtt; ++i) {
        mr->mtt[i].vaddr = (void *)(mr->addr + (i << PAGE_SIZE_LOG));
        /* Init (Allocate and write) MTT */
        struct kfd_ioctl_init_mtt_args *mtt_args =
                (struct kfd_ioctl_init_mtt_args *)malloc(sizeof(struct kfd_ioctl_init_mtt_args));
        mtt_args->batch_size = 1;//批处理规模=1
        mtt_args->vaddr[0] = mr->mtt[i].vaddr;
        write_cmd(dvr->fd, HGKFD_IOC_ALLOC_MTT, (void *)mtt_args);
        mr->mtt[i].mtt_index = mtt_args->mtt_index;
        mr->mtt[i].paddr = mtt_args->paddr[0];
        write_cmd(dvr->fd, HGKFD_IOC_WRITE_MTT, (void *)mtt_args);
        free(mtt_args);
    }

    /* Allocate MPT */
    struct kfd_ioctl_alloc_mpt_args *mpt_alloc_args = 
            (struct kfd_ioctl_alloc_mpt_args *)malloc(sizeof(struct kfd_ioctl_alloc_mpt_args));
    mpt_alloc_args->batch_size = 1;
    write_cmd(dvr->fd, HGKFD_IOC_ALLOC_MPT, (void *)mpt_alloc_args);
    mr->lkey = mpt_alloc_args->mpt_index;
    free(mpt_alloc_args);

    /* Write MPT */
    struct kfd_ioctl_write_mpt_args *mpt_args = 
            (struct kfd_ioctl_write_mpt_args *)malloc(sizeof(struct kfd_ioctl_write_mpt_args));
    mpt_args->batch_size = 1;
    mpt_args->flag[0]      = mr->flag;
    mpt_args->addr[0]      = (uint64_t) mr->addr;
    mpt_args->length[0]    = mr->length;
    mpt_args->mtt_index[0] = mr->mtt[0].mtt_index;
    mpt_args->mpt_index[0] = mr->lkey;
    write_cmd(dvr->fd, HGKFD_IOC_WRITE_MPT, (void *)mpt_args);
    free(mpt_args);

    HGRNIC_PRINT(" ibv_reg_mr: out!\n");
    return mr;
}


/**
 * @note    Post Send (Send/RDMA Write/RDMA Read) request (list) to hardware.
 *          Support any number of WQE posted.
 * 
 */
//各类发送相关的操作的用户态驱动
//这个是数据相关操作，需要绕开内核的系统调用
//生成相关的WQE内容和DOORBELL，批处理num个
//小疑问，有没有把doorbell发送出去
int ibv_post_send(struct ibv_context *context, struct ibv_wqe *wqe, struct ibv_qp *qp, uint8_t num) {
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    volatile uint64_t *doorbell = dvr->doorbell;
    //此前在opendevice操作中，doorbell已建立mmap映射，映射的是dvr->fd这个设备
    struct send_desc *tx_desc;
    uint16_t sq_head = qp->snd_wqe_offset;
    uint8_t first_trans_type = wqe[0].trans_type;
    int snd_cnt = 0;
    for (int i = 0; i < num; ++i) {
        /* Get send Queue */
        //获取发送WQE的方式是从【SQ的起始虚拟地址addr+此QP的snd_wqe_offset】所在的位置获取
        //先创建一个tx_desc
        tx_desc = (struct send_desc *) (qp->snd_mr->addr + qp->snd_wqe_offset);

        /* Add Base unit */
        // tx_desc->opcode = (i == num - 1) ? IBV_TYPE_NULL : wqe[i+1].trans_type;
        tx_desc->flags  = 0;
        tx_desc->flags  = wqe[i].flag;
        tx_desc->opcode = wqe[i].trans_type;

        /* Add data unit */
        tx_desc->len = wqe[i].length;
        tx_desc->lkey = wqe[i].mr->lkey;
        tx_desc->lVaddr = (uint64_t)wqe[i].mr->addr + wqe[i].offset;//WQE告诉硬件获取发送数据的地方

        /* Add RDMA unit */
        //如果wqe是RDMA写或者读，此时QP建立的默认是RC
        if (wqe[i].trans_type == IBV_TYPE_RDMA_WRITE || 
            wqe[i].trans_type == IBV_TYPE_RDMA_READ) {
            tx_desc->rdma_type.rkey = wqe[i].rdma.rkey;
            tx_desc->rdma_type.rVaddr_h = wqe[i].rdma.raddr >> 32;
            tx_desc->rdma_type.rVaddr_l = wqe[i].rdma.raddr & 0xffffffff;
        }

        /* Add UD Send unit */
        //如果是UD的Send操作，wqe需要注明对方的信息，如果是RC，面向建立这些信息不需要
        if (wqe[i].trans_type == IBV_TYPE_SEND &&
            qp->type == QP_TYPE_UD) {
            tx_desc->send_type.dest_qpn = wqe[i].send.dqpn;
            tx_desc->send_type.dlid = wqe[i].send.dlid;
            tx_desc->send_type.qkey = wqe[i].send.qkey;
        }
    
        /* update send queue */
        ++snd_cnt;
        qp->snd_wqe_offset += sizeof(struct send_desc);//每加入一个wqe，offset就要更改，指向下一个WQE
        //如果发现发现加完后，剩下的空间不足以容纳又一个wqe
        //则先把前面的发走，生成对应的doorbell
        //此doorbell生成后把sq_head重置回0，first_trans_type重置看情况，如果刚好此次num个批处理装下【下一个就装不下】
        //则将first_trans_type置为null，如果此次批处理没结束就装不下，则first_trans_type设置为下一个wqe的传输类型
        //将snd_cnt变0，snd_wqe_offset也置零，预示着重置SQ
        //
        if (qp->snd_wqe_offset + sizeof(struct send_desc) > qp->snd_mr->length) { 
            /* In case the remaining space * is not enough for one descriptor. */
            //如果指向下一个WQE导致溢出        
            /* Post send doorbell */
            // tx_desc->opcode  = IBV_TYPE_NULL;
            uint32_t db_low  = (sq_head << 4) | first_trans_type;
            uint32_t db_high = (qp->qp_num << 8) | snd_cnt;
            *doorbell = ((uint64_t)db_high << 32) | db_low;
            //生成DOORBELL，生成doorbell根据此次批处理第一次时的sq_head、first_trans_type和qp_num数据构成
            
            sq_head = 0;
            first_trans_type = (i == num - 1) ? IBV_TYPE_NULL : wqe[i+1].trans_type;
            snd_cnt = 0;
            qp->snd_wqe_offset = 0; /* SQ MR is allocated in page, so 
                                     * the start address (offset) is 0 */
            
            // HGRNIC_PRINT(" 1db_low is 0x%x, db_high is 0x%x\n", db_low, db_high);
        }

        // uint8_t *u8_tmp = (uint8_t *)tx_desc;
        // for (int i = 0; i < sizeof(struct send_desc); ++i) {
        //     HGRNIC_PRINT(" data[%d] 0x%x\n", i, u8_tmp[i]);
        // }
    }

    if (snd_cnt) {
        /* Post send doorbell */
        uint32_t db_low  = (sq_head << 4) | first_trans_type;
        uint32_t db_high = (qp->qp_num << 8) | snd_cnt;
        *doorbell = ((uint64_t)db_high << 32) | db_low;

        // HGRNIC_PRINT(" db_low is 0x%x, db_high is 0x%x\n", db_low, db_high);
    }

    return 0;
}
//用户态驱动，绕开内核
int ibv_post_recv(struct ibv_context *context, struct ibv_wqe *wqe, struct ibv_qp *qp, uint8_t num) {
    struct hghca_context *dvr = (struct hghca_context *)context->dvr;
    // struct Doorbell *boorbell = dvr->doorbell;

    struct recv_desc *rx_desc;

    for (int i = 0; i < num; ++i) {
        /* Get Receive Queue */
        rx_desc = (struct recv_desc *) (qp->rcv_mr->addr + qp->rcv_wqe_offset);
        
        /* Add basic element */
        rx_desc->len = wqe[i].length;
        rx_desc->lkey = wqe[i].mr->lkey;
        rx_desc->lVaddr = (uint64_t)wqe[i].mr->addr + wqe[i].offset;
        
        // HGRNIC_PRINT(" len is %d, lkey is %d, lvaddr is 0x%lx\n", rx_desc->len, rx_desc->lkey, rx_desc->lVaddr);
    
        /* update Receive Queue */
        qp->rcv_wqe_offset += sizeof(struct recv_desc);
        if (qp->rcv_wqe_offset  + sizeof(struct recv_desc) > qp->rcv_mr->length) { /* In case the remaining space 
                                                                                   * is not enough for one descriptor. */
            qp->rcv_wqe_offset = 0; /* RQ MR is allocated in page, so 
                                     * the start address (offset) is 0 */
        }
    }

    return 0;
}

/**
 * @note Poll at most 100 cpl one time
 * 
 */
//轮询一个CQ，如果发现有元素，则将MR里的CQ内容复制到desc地址
int ibv_poll_cpl(struct ibv_cq *cq, struct cpl_desc **desc, int max_num) {
    int cnt = 0;

    for (cnt = 0; cnt < max_num; ++cnt) {
        struct cpl_desc *cq_desc = (struct cpl_desc *)(cq->mr->addr + cq->offset);
        if (cq_desc->byte_cnt != 0) {
            memcpy(desc[cnt], cq_desc, sizeof(struct cpl_desc));
            // memset(cq_desc, 0, sizeof(struct cpl_desc));
            cq_desc->byte_cnt = 0; /* clear CQ cpl */

            /* Update offset */
            ++cq->cpl_cnt;
            cq->offset += sizeof(struct cpl_desc);
            if (cq->offset + sizeof(struct cpl_desc) > cq->mr->length) {
                cq->offset = 0;
            }
        } else {
            break;
        }
    }

    return cnt;
}