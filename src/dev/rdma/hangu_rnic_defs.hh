/*
 *======================= START OF LICENSE NOTICE =======================
 *  Copyright (C) 2021 Kang Ning, NCIC, ICT, CAS.
 *  All Rights Reserved.
 *
 *  NO WARRANTY. THE PRODUCT IS PROVIDED BY DEVELOPER "AS IS" AND ANY
 *  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL DEVELOPER BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THE PRODUCT, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *======================== END OF LICENSE NOTICE ========================
 *  Primary Author: Kang Ning
 *  <kangning18z@ict.ac.cn>
 */

/**
 * @file
 * Han Gu RNIC register defination declaration.
 */


#ifndef __HANGU_RNIC_DEFS_HH__
#define __HANGU_RNIC_DEFS_HH__
#include <memory>              //用于智能指针
#include "base/bitfield.hh"
#include "dev/net/etherpkt.hh"
#include "debug/HanGu.hh"
#include "sim/eventq.hh"
#include "dev/rdma/kfd_ioctl.h"


//用于打印不同颜色的日志，应该没啥用
#ifdef COLOR    

#define HANGU_PRINT(name, x, ...) do {                                      \
            DPRINTF(name, "[\033[1;33m" #name "\033[0m] " x, ##__VA_ARGS__);\
        } while (0)

#else

#define HANGU_PRINT(name, x, ...) do {                     \
            DPRINTF(name, "[" #name "] " x, ##__VA_ARGS__);\
        } while (0)

#endif

#define QPN_NUM   (512 * 3)


#define PAGE_SIZE_LOG 12     //页面大小的对数为12
#define PAGE_SIZE (1 << PAGE_SIZE_LOG)  //页面大小2^12=4096个字节

namespace HanGuRnicDef {     //HanGuRnicDef的命名空间

struct QpcResc;  //两个结构体，目前不知道干什么用的
struct CqcResc;


struct Hcr {    //硬件控制寄存器（HCR, Hardware Control Register）
    uint32_t inParam_l;  //低 32 位和高 32 位输入参数。
    uint32_t inParam_h;
    uint32_t inMod;     //输入模式，可能控制命令的执行方式
    uint32_t outParam_l; //低 32 位和高 32 位输出参数，存储命令执行结果。
    uint32_t outParam_h;
    uint32_t goOpcode;   //操作码
};

// INIT_ICM：初始化 ICM（Internal Control Memory）。
// WRITE_ICM：写入 ICM 数据。
// WRITE_MPT：写入 MPT（Memory Protection Table）。
// WRITE_MTT：写入 MTT（Memory Translation Table）。
// WRITE_QPC：写入队列对上下文（QPC）。
// WRITE_CQC：写入完成队列上下文（CQC）
const uint8_t INIT_ICM  = 0x01;
const uint8_t WRITE_ICM = 0x02;
const uint8_t WRITE_MPT = 0x03;
const uint8_t WRITE_MTT = 0x04;
const uint8_t WRITE_QPC = 0x05;
const uint8_t WRITE_CQC = 0x06;

// opcode：操作码，定义执行的命令类型。
// num：数量，可能用于表示批量操作的数量。
// qpn：Queue Pair Number，指定目标队列对的编号。
// offset：偏移量，可能用于定位内存或队列中的具体位置
struct Doorbell {//doorbell寄存器
    uint8_t  opcode;
    uint8_t  num;
    uint32_t qpn;
    uint32_t offset;
};
// Command Opcode for Doorbell command
const uint8_t OPCODE_NULL       = 0x00;
const uint8_t OPCODE_SEND       = 0x01;
const uint8_t OPCODE_RECV       = 0x02;
const uint8_t OPCODE_RDMA_WRITE = 0x03;
const uint8_t OPCODE_RDMA_READ  = 0x04;

struct DoorbellFifo {//好像和doorbell寄存器比较类似，目前不知道什么东西，是fifo
    DoorbellFifo (uint8_t  opcode, uint8_t  num, 
            uint32_t qpn, uint32_t offset) {
        this->opcode = opcode;
        this->num = num;
        this->qpn = qpn;
        this->offset = offset;
    }
    uint8_t  opcode;
    uint8_t  num;
    uint32_t qpn;
    uint32_t offset;
    // Addr     qpAddr;
};
typedef std::shared_ptr<DoorbellFifo> DoorbellPtr;//DoorbellPtr智能指针


/* Mailbox offset in CEU command */
// INIT_ICM初始化icm
struct  InitResc {
    uint8_t qpsNumLog; //qp数量的对数
    uint8_t cqsNumLog; //cq数量的对数
    uint8_t mptNumLog; //mpt数量的对数
    uint64_t qpcBase;  //qpc基址指针
    uint64_t cqcBase;
    uint64_t mptBase;
    uint64_t mttBase;
};
// const uint32_t MBOX_INIT_SZ = 0x20;

// 该结构体用于记录 ICMT（Internal Control Memory Table）中条目的相关信息，包含页面号以及对应的虚拟地址和物理地址。
struct IcmResc {
    uint16_t pageNum;//物理页号
    uint64_t vAddr;
    uint64_t pAddr;
};
//某种编号
const uint8_t ICMTYPE_MPT = 0x01;
const uint8_t ICMTYPE_MTT = 0x02;
const uint8_t ICMTYPE_QPC = 0x03;
const uint8_t ICMTYPE_CQC = 0x04;

/* WRITE_MTT */
//MTT 是 RDMA 中用于管理内存的表格，它将虚拟地址映射到物理地址。MttResc结构体用于指定要写入 MTT 的物理地址
struct MttResc {
    uint64_t pAddr;
};

/* WRITE_MPT */
struct MptResc {
    uint32_t flag;
    uint32_t key;
    uint64_t startVAddr;
    uint64_t length;
    uint64_t mttSeg;
};
// Same as ibv_mr_flag
const uint8_t MPT_FLAG_RD     = (1 << 0);
const uint8_t MPT_FLAG_WR     = (1 << 1);
const uint8_t MPT_FLAG_LOCAL  = (1 << 2);
const uint8_t MPT_FLAG_REMOTE = (1 << 3);

// WRITE_QP
//我猜是一个QPC所包含的数据
struct QpcResc {
    uint8_t flag; // QP state, not used now
    uint8_t qpType;//RC,UD之类的
    uint8_t sqSizeLog; /* The size of SQ in log (It is now fixed at 4KB, which is 12) */
    uint8_t rqSizeLog; /* The size of RQ in log (It is now fixed at 4KB, which is 12) */
    uint16_t sndWqeOffset;
    uint16_t rcvWqeOffset;
    uint16_t lLid; // Local LID
    uint16_t dLid; // Dest  LID
    uint32_t srcQpn; // Local qpn
    uint32_t destQpn; // Dest qpn
    uint32_t sndPsn; // next send psn
    uint32_t ackPsn; // last acked psn
    uint32_t expPsn; // next receive (expect) psn
    uint32_t cqn;
    uint32_t sndWqeBaseLkey; // send wqe base lkey
    uint32_t rcvWqeBaseLkey; // receive wqe base lkey
    uint32_t qkey;//内存访问控制？不清楚
    uint32_t reserved[52];
};
const uint8_t QP_TYPE_RC = 0x00;//四种传输服务类型
const uint8_t QP_TYPE_UC = 0x01;
const uint8_t QP_TYPE_RD = 0x02;
const uint8_t QP_TYPE_UD = 0x03;

// WRITE_CQ
//我猜是CQC所包含的数据
struct CqcResc {
    uint32_t cqn;
    uint32_t offset; // The offset of the CQ
    uint32_t lkey;   // lkey of the CQ

    // !TODO We don't implement it now.
    uint32_t sizeLog; // The size of CQ. (It is now fixed at 4KB)
};

//这是一个常量，表示在写操作（Write）中使用的标志位。通过设置该标志位，可以指示完成队列中此写操作完成时，会生成一个中断或通知
//以上信息来自gpt，尚不知道什么意思
const uint32_t WR_FLAG_SIGNALED = (1 << 31);

// Send descriptor struct
//发送描述符结构体
//现在理解TxDesc为SQE的内容，指示了任务的内容   2025.1.9
struct TxDesc {

    TxDesc(TxDesc * tx) { //拷贝构造函数，传入一个TxDesc型指针时，将这个指针指向的TxDesc的值复制到本TxDesc的值上
        this->len    = tx->len;
        this->lkey   = tx->lkey;
        this->lVaddr = tx->lVaddr;
        this->flags  = tx->flags;
        this->sendType.destQpn = tx->sendType.destQpn;
        this->sendType.dlid    = tx->sendType.dlid;
        this->sendType.qkey    = tx->sendType.qkey;
    }

    TxDesc() {//默认构造函数，所有参数为0
        this->len    = 0;
        this->lkey   = 0;
        this->lVaddr = 0;
        this->flags  = 0;
        this->sendType.destQpn = 0;
        this->sendType.dlid    = 0;
        this->sendType.qkey    = 0;
    }

    bool isSignaled () {
        return (this->flags & WR_FLAG_SIGNALED) != 0;//检测操作有没有完成，具体机制未知
    }

    uint32_t len;//发送传输数据的长度
    uint32_t lkey;//本地密钥，用于访问本地内存MR
    uint64_t lVaddr;//本地虚拟地址

    union {
        struct {        
            uint32_t dlid;//目标lid
            uint32_t qkey;//qkey是啥不知道，可能是每个QP拥有的一个密钥
            uint32_t destQpn;//目标qpn
        } sendType;         //发送是一个send类型指令
        struct {
            uint32_t rkey;  //远程key
            uint32_t rVaddr_l; //远程虚拟地址低32位和高32位
            uint32_t rVaddr_h; 
        } rdmaType;        //发送是一个rdma类型指令
    };
    // uint8_t opcode;
    //一个联合体，目前不知道里面存储的东西是干什么的
    union {
        uint32_t flags;
        uint8_t  opcode;
    };
};
typedef std::shared_ptr<TxDesc> TxDescPtr;


//接收描述符结构
// Receive Descriptor struct
struct RxDesc {

    RxDesc(RxDesc * rx) {
        this->len = rx->len;
        this->lkey = rx->lkey;
        this->lVaddr = rx->lVaddr;
    }

    RxDesc() {
        this->len    = 0;
        this->lkey   = 0;
        this->lVaddr = 0;
    }

    uint32_t len;
    uint32_t lkey;
    uint64_t lVaddr;
};
typedef std::shared_ptr<RxDesc> RxDescPtr;

//完成队列描述符
struct CqDesc {
    CqDesc(uint8_t srvType, uint8_t transType, 
            uint16_t byteCnt, uint32_t qpn, uint32_t cqn) {
        this->srvType   = srvType;
        this->transType = transType;
        this->byteCnt   = byteCnt;
        this->qpn       = qpn;
        this->cqn       = cqn;
    }
    uint8_t  srvType; //服务类型
    uint8_t  transType; //传输类型
    uint16_t byteCnt;  //字节数
    uint32_t qpn;  //可能是每个cq对应一个qp
    uint32_t cqn;
};
// const uint8_t CQ_ENTRY_SZ = 12;
typedef std::shared_ptr<CqDesc> CqDescPtr;

//内存注册（Memory Region, MR）的请求和响应，支持对描述符或数据的读写操作
/* Descriptor read & Data read&write request */
struct MrReqRsp {
    
    MrReqRsp(uint8_t type, uint8_t chnl, uint32_t lkey, 
            uint32_t len, uint32_t vaddr) {
        this->type = type;//读请求、读响应、写请求
        this->chnl = chnl;
        this->lkey = lkey;//密钥
        this->length = len;
        this->offset = vaddr;
        //虚拟地址偏移量，用于与内存保护表（MPT）的虚拟地址进行比较，计算内存页表（MTT）索引，并提供实际物理地址的偏移量
        this->wrDataReq = nullptr;
    }

    uint8_t  type  ; /* 1 - wreq; 2 - rreq, 3 - rrsp; */
    uint8_t  chnl  ; /* 1 - wreq TX cq; 2 - wreq RX cq; 3 - wreq TX data; 4 - wreq RX data;
                      * 5 - rreq TX Desc; 6 - rreq RX Desc; 7 - rreq TX Data; 8 - rreq RX Data */
    uint32_t lkey  ;
    uint32_t length; /* in Bytes */
    uint32_t offset; /* Accessed VAddr, used to compared with vaddr in MPT, 
                      * and calculate MTT Index, Besides, this field also provides
                      * access offset to the actual paddr. 
                      * !TODO: Now we only support lower 16 bit comparasion with Vaddr, 
                      * which means support maximum 16KB for one MR. */
    union {
        TxDesc  *txDescRsp;
        RxDesc  *rxDescRsp;
        CqDesc  *cqDescReq;
        uint8_t *wrDataReq;
        uint8_t *rdDataRsp;
        uint8_t *data;
    };
};
typedef std::shared_ptr<MrReqRsp> MrReqRspPtr;
const uint8_t DMA_TYPE_WREQ = 0x01;
const uint8_t DMA_TYPE_RREQ = 0x02;
const uint8_t DMA_TYPE_RRSP = 0x03;

const uint8_t TPT_WCHNL_TX_CQUE = 0x01;
const uint8_t TPT_WCHNL_RX_CQUE = 0x02;
const uint8_t TPT_WCHNL_TX_DATA = 0x03;
const uint8_t TPT_WCHNL_RX_DATA = 0x04;
const uint8_t MR_RCHNL_TX_DESC = 0x05;
const uint8_t MR_RCHNL_RX_DESC = 0x06;
const uint8_t MR_RCHNL_TX_DATA = 0x07;
const uint8_t MR_RCHNL_RX_DATA = 0x08;

//Cxt请求/响应结构
struct CxtReqRsp {
    CxtReqRsp (uint8_t type, uint8_t chnl, uint32_t num, uint32_t sz = 1, uint8_t idx = 0) {
        this->type = type;
        this->chnl = chnl;
        this->num  = num;
        this->sz   = sz;
        this->idx  = idx;
        this->txCqcRsp = nullptr;
    }
    uint8_t type; // 1: qp wreq; 2: qp rreq; 3: qp rrsp; 4: cq rreq; 5: cq rrsp; 6: sq addr rreq
    uint8_t chnl; // 1: tx Channel; 2: rx Channel
    uint32_t num; // Resource num (QPN or CQN).
    uint32_t sz; // request number of the resources, used in qpc read (TX)
    uint8_t  idx; // used to uniquely identify the req pkt */
    union {
        QpcResc  *txQpcRsp;
        QpcResc  *rxQpcRsp;
        QpcResc  *txQpcReq;
        QpcResc  *rxQpcReq;
        CqcResc  *txCqcRsp;
        CqcResc  *rxCqcRsp;
    };
};
typedef std::shared_ptr<CxtReqRsp> CxtReqRspPtr;
const uint8_t CXT_WREQ_QP = 0x01;
const uint8_t CXT_RREQ_QP = 0x02;
const uint8_t CXT_RRSP_QP = 0x03;
const uint8_t CXT_RREQ_CQ = 0x04;
const uint8_t CXT_RRSP_CQ = 0x05;
const uint8_t CXT_RREQ_SQ = 0x06; /* read sq addr */
const uint8_t CXT_CREQ_QP = 0x07; /* create request */
const uint8_t CXT_CHNL_TX = 0x01;
const uint8_t CXT_CHNL_RX = 0x02;

struct DF2DD {
    uint8_t  opcode;
    uint8_t  num;
    uint32_t qpn;
};

struct DD2DP {
    QpcResc *qpc; 
    TxDesc  *desc; // tx descriptor
};

struct DP2RG {
    QpcResc*     qpc; 
    TxDescPtr    desc; // tx descriptor
    EthPacketPtr txPkt;
};
typedef std::shared_ptr<DP2RG> DP2RGPtr;

struct RA2RG {
    QpcResc *qpc;
    EthPacketPtr txPkt;
};

struct DmaReq {//Addr即int64
    DmaReq (Addr addr, int size, Event *event, uint8_t *data, uint32_t chnl=0) {
        this->addr  = addr;
        this->size  = size;
        this->event = event;
        this->data  = data;
        this->chnl  = chnl;
        this->rdVld = 0;
        this->schd  = 0;
        this->reqType = 0;
    }
    Addr         addr  ; 
    int          size  ; 
    Event       *event ;
    uint8_t      rdVld ; /* the dma req's return data is valid */
    uint8_t     *data  ; 
    uint32_t     chnl  ; /* channel number the request belongs to, see below DMA_REQ_* for details */
    Tick         schd  ; /* when to schedule the event */
    uint8_t      reqType; /* type of request: 0 for read request, 1 for write request */
};
typedef std::shared_ptr<DmaReq> DmaReqPtr;

struct PendingElem {
    uint8_t idx;
    uint8_t chnl;
    uint32_t qpn;
    CxtReqRspPtr reqPkt;
    bool has_dma;
    
    PendingElem(uint8_t idx, uint8_t chnl, CxtReqRspPtr reqPkt, bool has_dma) {
        this->idx     = idx;
        this->chnl    = chnl;
        this->qpn     = reqPkt->num;
        this->reqPkt  = reqPkt;
        this->has_dma = has_dma;
    }

    PendingElem() {
        this->idx     = 0;
        this->chnl    = 0;
        this->qpn     = 0;
        this->reqPkt  = nullptr;
        this->has_dma = false;
    }

    ~PendingElem() {
     //HANGU_PRINT(Debug::CxtResc, "[CxtResc] ~PendingElem()\n");
        // 打印调试信息
    }
};
typedef std::shared_ptr<PendingElem> PendingElemPtr;

struct WindowElem {
    WindowElem(EthPacketPtr txPkt, uint32_t qpn, 
            uint32_t psn, TxDescPtr txDesc) {
        this->txPkt = txPkt;
        this->qpn   = qpn;
        this->psn   = psn;
        this->txDesc = txDesc;
    };
    EthPacketPtr txPkt;
    uint32_t qpn;
    uint32_t psn;

    // used for RDMA read only
    TxDescPtr txDesc;
};
typedef std::shared_ptr<WindowElem> WindowElemPtr;
typedef std::list<WindowElemPtr> WinList;//一个双向链表，存储windowElemPtr指针

/* Window List for one QP */
struct WinMapElem {
    WinList *list;      /* List of send packet and its attached information */
    uint32_t firstPsn;  /* First PSN in the list */
    uint32_t lastPsn;   /* Last PSN in the list */
    uint32_t cqn;       /* CQN for this QP (SQ) */
};
//BTH(Base Transport Header)，是对消息的传输层描述，包括OpCode, Destination QPN, 发送方PSN等字段
struct BTH {
    /* srv_type : trans_type : dest qpn
     * [31:29]     [28:24]     [23:0]   
     */
    uint32_t op_destQpn;

    /* needAck :  psn
     * [31:24]   [23:0]
     */
    uint32_t needAck_psn;
};
const uint8_t PKT_BTH_SZ = 8; // in bytes
const uint8_t PKT_TRANS_SEND_FIRST = 0x01;
const uint8_t PKT_TRANS_SEND_MID   = 0x02;
const uint8_t PKT_TRANS_SEND_LAST  = 0x03;
const uint8_t PKT_TRANS_SEND_ONLY  = 0x04;//非多包发送
const uint8_t PKT_TRANS_RWRITE_ONLY= 0x05;
const uint8_t PKT_TRANS_RREAD_ONLY = 0x06;
const uint8_t PKT_TRANS_ACK        = 0x07;//是确认包

struct DETH {//什么头部不太清楚
    uint32_t qKey;    
    uint32_t srcQpn;
};
const uint8_t PKT_DETH_SZ = 8; // in bytes

//RETH(RDMA Extended Transport Header)
//这部分是可选的，SEND的时候不需要，WIRTE的时候存在，主要用于描述远端的VA、RKey、DMA length信息。
struct RETH {
    uint32_t rVaddr_l;
    uint32_t rVaddr_h;
    uint32_t rKey;
    uint32_t len;
};
const uint8_t PKT_RETH_SZ = 16; // in bytes
//ACK头部
struct AETH {
    // syndrome :   msn
    // [31:24]    [23:0]
    uint32_t syndrome_msn;
};
const uint8_t PKT_AETH_SZ = 4; // in bytes
const uint8_t RSP_ACK = 0x01;
const uint8_t RSP_NAK = 0x02;


//两个用于获取+写入32/64位数据对应位置数据的宏定义函数
//已看懂
#define ADD_FIELD32(NAME, OFFSET, BITS) \
    inline uint32_t NAME() { return bits(_data, OFFSET+BITS-1, OFFSET); } \
    inline void NAME(uint32_t d) { replaceBits(_data, OFFSET+BITS-1, OFFSET,d); }

#define ADD_FIELD64(NAME, OFFSET, BITS) \
    inline uint64_t NAME() { return bits(_data, OFFSET+BITS-1, OFFSET); } \
    inline void NAME(uint64_t d) { replaceBits(_data, OFFSET+BITS-1, OFFSET,d); }


//定义了一个regs结构体，里面放置了各种寄存器变量
struct Regs : public Serializable {
    template<class T>
    struct Reg {
        T _data;
        T operator()() { return _data; }
        const Reg<T> &operator=(T d) { _data = d; return *this;}
        bool operator==(T d) { return d == _data; }
        void operator()(T d) { _data = d; }
        Reg() { _data = 0; }
        void serialize(CheckpointOut &cp) const//目前还不能理解这个serialize干什么的
        {
            SERIALIZE_SCALAR(_data);
        }
        void unserialize(CheckpointIn &cp)
        {
            UNSERIALIZE_SCALAR(_data);
        }
    };
    
    struct INPARAM : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;//操作符不会自动继承，需要使用父类的操作符重载
        ADD_FIELD64(iparaml,0,32);//iparaml是它的低32位
        ADD_FIELD64(iparamh,32,32); //iparaml是它的高32位
    };
    INPARAM inParam;

    uint32_t modifier;
    
    struct OUTPARAM : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;
        ADD_FIELD64(oparaml,0,32);
        ADD_FIELD64(oparamh,32,32);
    };
    OUTPARAM outParam;
    //CMDCTRL 是一个32位reg
    struct CMDCTRL : public Reg<uint32_t> {
        using Reg<uint32_t>::operator=;
        ADD_FIELD32(op,0,8);
        ADD_FIELD32(go,31,1);
    };
    CMDCTRL cmdCtrl;

    struct DOORBELL : public Reg<uint64_t> {
        using Reg<uint64_t>::operator=;
        ADD_FIELD64(dbl,0,32);   //doorbell低32位
        ADD_FIELD64(dbh,32,32);  //doorbell高32位
        ADD_FIELD64(opcode,0,4); //以下是获取doorbell的各个段的数值
        ADD_FIELD64(offset,4,28);
        ADD_FIELD64(num,32,8);
        ADD_FIELD64(qpn,40,24);
    };
    DOORBELL db;

    //我猜是地址
    uint64_t mptBase;
    uint64_t mttBase;
    uint64_t qpcBase;
    uint64_t cqcBase;
   //各元数据数量的对数
    uint8_t  mptNumLog;
    uint8_t  mttNumLog;
    uint8_t  qpcNumLog;
    uint8_t  cqcNumLog;

    //打印值到输出流
    void serialize(CheckpointOut &cp) const override {
        paramOut(cp, "inParam", inParam._data);
        paramOut(cp, "modifier", modifier);
        paramOut(cp, "outParam", outParam._data);
        paramOut(cp, "cmdCtrl", cmdCtrl._data);
        paramOut(cp, "db", db._data);
        paramOut(cp, "mptBase", mptBase);
        paramOut(cp, "mttBase", mttBase);
        paramOut(cp, "qpcBase", qpcBase);
        paramOut(cp, "cqcBase", cqcBase);
        paramOut(cp, "mptNumLog", mptNumLog);
        paramOut(cp, "mttNumLog", mttNumLog);
        paramOut(cp, "qpcNumLog", qpcNumLog);
        paramOut(cp, "cqcNumLog", cqcNumLog);
    }

    //从检查点中恢复一些值，如果无法恢复则报错
    void unserialize(CheckpointIn &cp) override {

        paramIn(cp, "inParam", inParam._data);
        paramIn(cp, "modifier", modifier);
        paramIn(cp, "outParam", outParam._data);
        paramIn(cp, "cmdCtrl", cmdCtrl._data);
        paramIn(cp, "db", db._data);
        paramIn(cp, "mptBase", mptBase);
        paramIn(cp, "mttBase", mttBase);
        paramIn(cp, "qpcBase", qpcBase);
        paramIn(cp, "cqcBase", cqcBase);
        paramIn(cp, "mptNumLog", mptNumLog);
        paramIn(cp, "mttNumLog", mttNumLog);
        paramIn(cp, "qpcNumLog", qpcNumLog);
        paramIn(cp, "cqcNumLog", cqcNumLog);
    }
};

} // namespace HanGuRnicDef

#endif // __HANGU_RNIC_DEFS_HH__

//2024.12.16  15:36第一次看完
//2024.12.28  10:45第二次看完