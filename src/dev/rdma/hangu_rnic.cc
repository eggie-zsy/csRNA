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
 * Device model for Han Gu RNIC.
 */

 #include "dev/rdma/hangu_rnic.hh"


 #include <algorithm>
 #include <memory>
 #include <queue>
 
 #include "base/inet.hh"
 #include "base/trace.hh"
 #include "base/random.hh"
 #include "debug/Drain.hh"
 #include "dev/net/etherpkt.hh"
 #include "debug/HanGu.hh"
 #include "mem/packet.hh"
 #include "mem/packet_access.hh"
 #include "params/HanGuRnic.hh"
 #include "sim/stats.hh"
 #include "sim/system.hh"
 
 //开始于12.25 12：12
 //第一次通读结束2025.2.20 12：56 
 //第二次精读结束2025.2.24 15：31，完成精读，读懂95%
 using namespace HanGuRnicDef;
 using namespace Net;
 using namespace std;
 
 HanGuRnic::HanGuRnic(const Params *p)
   : RdmaNic(p), etherInt(NULL),
     doorbellVector(p->reorder_cap),//Number of concurrent request for one qpc req channel
     //doorbellvector的容量为reorder_cap
     ceuProcEvent      ([this]{ ceuProc();      }, name()),
     doorbellProcEvent ([this]{ doorbellProc(); }, name()),
     mboxEvent([this]{ mboxFetchCpl();    }, name()),
     //以上用于注册事件
     rdmaEngine  (this, name() + ".RdmaEngine", p->reorder_cap),
     mrRescModule(this, name() + ".MrRescModule", p->mpt_cache_num, p->mtt_cache_num),
     cqcModule   (this, name() + ".CqcModule", p->cqc_cache_num),
     qpcModule   (this, name() + ".QpcModule", p->qpc_cache_cap, p->reorder_cap),
     dmaEngine   (this, name() + ".DmaEngine"),
 
     dmaReadDelay(p->dma_read_delay), dmaWriteDelay(p->dma_write_delay),
     pciBandwidth(p->pci_speed),
     etherBandwidth(p->ether_speed),
     LinkDelay     (p->link_delay),
     ethRxPktProcEvent([this]{ ethRxPktProc(); }, name()) {
 
     HANGU_PRINT(HanGuRnic, " qpc_cache_cap %d  reorder_cap %d cpuNum 0x%x\n", p->qpc_cache_cap, p->reorder_cap, p->cpu_num);
 
     cpuNum = p->cpu_num;
     syncCnt = 0;
     syncSucc = 0;
 
    //放满index
     for (int i = 0; i < p->reorder_cap; ++i) {
         df2ccuIdxFifo.push(i);
     }
 
     etherInt = new HanGuRnicInt(name() + ".int", this);//初始化rnic的以太网口
 
     mboxBuf = new uint8_t[4096];//我猜是mailbox的buffer，一共4096个字节
 
     // Set the MAC address
     //将64位MAC地址放入字节数组macAddr中
     memset(macAddr, 0, ETH_ADDR_LEN);
     for (int i = 0; i < ETH_ADDR_LEN; ++i) {
         macAddr[ETH_ADDR_LEN - 1 - i] = (p->mac_addr >> (i * 8)) & 0xff;
     }
 
     BARSize[0]  = (1 << 12);
     BARAddrs[0] = 0xc000000000000000;
 }
 
 HanGuRnic::~HanGuRnic() {
     delete etherInt;
 }
 
 void
 HanGuRnic::init() {
     PciDevice::init();
 }
 
 Port &
 HanGuRnic::getPort(const std::string &if_name, PortID idx) {
     if (if_name == "interface")
         return *etherInt;
     return RdmaNic::getPort(if_name, idx);
 }
 
 ///////////////////////////// HanGuRnic::PIO relevant {begin}//////////////////////////////
 
 Tick
 HanGuRnic::writeConfig(PacketPtr pkt) {
     int offset = pkt->getAddr() & PCI_CONFIG_SIZE;
     if (offset < PCI_DEVICE_SPECIFIC) {
         PciDevice::writeConfig(pkt);
     }
     else {
         panic("Device specific PCI config space not implemented.\n");
     }
 
     /* !TODO: We will implement PCI configuration here.
      * Some work may need to be done here based for the pci 
      * COMMAND bits, we don't realize now. */
 
     return configDelay;
 }
 
 
 Tick
 HanGuRnic::read(PacketPtr pkt) {
     int bar;
     Addr daddr;
 
     if (!getBAR(pkt->getAddr(), bar, daddr)) {
         panic("Invalid PCI memory access to unmapped memory.\n");
     }
 
     /* Only HCR Space (BAR0-1) is allowed */
     assert(bar == 0);
 
     /* Only 32bit accesses allowed */
     assert(pkt->getSize() == 4);
 
     /* Handle read of register here.
      * Here we only implement read go bit */
     if (daddr == (Addr)&(((HanGuRnicDef::Hcr*)0)->goOpcode)) {/* Access `GO` bit */
         pkt->setLE<uint32_t>(regs.cmdCtrl.go()<<31 | regs.cmdCtrl.op());//pkt的data被设置为cmdCtrl的32位数据
     } else if (daddr == 0x20) {/* Access `sync` reg */
         pkt->setLE<uint32_t>(syncSucc);//将pkt的data设置为32位syncSucc数据
     } else {
         pkt->setLE<uint32_t>(0);//否则32位0
     }
 
     pkt->makeAtomicResponse();
     return pioDelay;
 }
 //用户程序向RNIC的regs中写入HCR数据、DOORBELL数据
 Tick
 HanGuRnic::write(PacketPtr pkt) {
     int bar;
     Addr daddr;
 
     HANGU_PRINT(PioEngine, " PioEngine.write: pkt addr 0x%x, size 0x%x\n",
             pkt->getAddr(), pkt->getSize());
 
     if (!getBAR(pkt->getAddr(), bar, daddr)) {
         panic("Invalid PCI memory access to unmapped memory.\n");
     }
 
     /* Only BAR 0 is allowed */
     assert(bar == 0);
     
     if (daddr == 0 && pkt->getSize() == sizeof(Hcr)) { //写HCR
         HANGU_PRINT(PioEngine, " PioEngine.write: HCR, inparam: 0x%x\n", pkt->getLE<Hcr>().inParam_l);
         //将寄存器组中各个寄存器的各个段设置为pkt中存储的HCR数据的各个位，好像没写DOORBELL
         regs.inParam.iparaml(pkt->getLE<Hcr>().inParam_l);
         regs.inParam.iparamh(pkt->getLE<Hcr>().inParam_h);
         regs.modifier = pkt->getLE<Hcr>().inMod;
         regs.outParam.oparaml(pkt->getLE<Hcr>().outParam_l);
         regs.outParam.oparamh(pkt->getLE<Hcr>().outParam_h);
         regs.cmdCtrl = pkt->getLE<Hcr>().goOpcode;
 
         /* Schedule CEU */
         if (!ceuProcEvent.scheduled()) { 
             schedule(ceuProcEvent, curTick() + clockPeriod());
         }
     //daddr是寄存器在内存映射的地址偏移
     } else if (daddr == 0x18 && pkt->getSize() == sizeof(uint64_t)) {   //写DOORBELL
 
         /*  Used to Record start of time */
         HANGU_PRINT(HanGuRnic, " PioEngine.write: Doorbell, value %#X pio interval %ld\n", pkt->getLE<uint64_t>(), curTick() - this->tick); 
         
         regs.db._data = pkt->getLE<uint64_t>();//将pkt携带的64位数据，写入regs寄存器组中的doorbell中
         
         DoorbellPtr dbell = make_shared<DoorbellFifo>(regs.db.opcode(), 
             regs.db.num(), regs.db.qpn(), regs.db.offset());
         pio2ccuDbFifo.push(dbell);    //创建一个指向堆的doorbell指针，并推入pio2ccuDbFifo中待ccu处理
 
         /* Record last tick */
         this->tick = curTick();
 
         /* Schedule doorbellProc */
         if (!doorbellProcEvent.scheduled()) { //调度处理db的事件，盲猜ccu会从pio2ccuDbFifo取数据进行处理
             schedule(doorbellProcEvent, curTick() + clockPeriod());
         }
 
         HANGU_PRINT(HanGuRnic, " PioEngine.write: qpn %d, opcode %x, num %d\n", 
                 regs.db.qpn(), regs.db.opcode(), regs.db.num());
     } else if (daddr == 0x20 && pkt->getSize() == sizeof(uint32_t)) { /* latency sync */
         
         HANGU_PRINT(HanGuRnic, " PioEngine.write: sync bit, value %#X, syncCnt %d\n", pkt->getLE<uint32_t>(), syncCnt); 
         
         if (pkt->getLE<uint32_t>() == 1) {//如果写进来的标志为1
             syncCnt += 1;
             assert(syncCnt <= cpuNum);
             if (syncCnt == cpuNum) {
                 syncSucc = 1;
             }
         } else {//如果写进来的是0
             assert(syncCnt > 0);
             syncCnt -= 1;
             if (syncCnt == 0) {
                 syncSucc = 0;
             }
         }
 
         HANGU_PRINT(HanGuRnic, " PioEngine.write: sync bit end, value %#X, syncCnt %d\n", pkt->getLE<uint32_t>(), syncCnt); 
     } else {
         panic("Write request to unknown address : %#x && size 0x%x\n", daddr, pkt->getSize());
     }
 
     pkt->makeAtomicResponse();
     return pioDelay;
 }
 ///////////////////////////// HanGuRnic::PIO relevant {end}//////////////////////////////
 //1.23 第一次读完
 ///////////////////////////// HanGuRnic::CCU relevant {begin}//////////////////////////////
 
 // Event of CCU after mailbox being fetched
 //mboxevent处理函数
 void
 HanGuRnic::mboxFetchCpl () {
 
     HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl!\n");
     switch (regs.cmdCtrl.op()) {
       case INIT_ICM ://初始化所有ICM的基础数据
         HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: INIT_ICM command!\n");
         regs.mptBase   = ((InitResc *)mboxBuf)->mptBase;
         regs.mttBase   = ((InitResc *)mboxBuf)->mttBase;
         regs.qpcBase   = ((InitResc *)mboxBuf)->qpcBase;
         regs.cqcBase   = ((InitResc *)mboxBuf)->cqcBase;
         regs.mptNumLog = ((InitResc *)mboxBuf)->mptNumLog;
         regs.qpcNumLog = ((InitResc *)mboxBuf)->qpsNumLog;
         regs.cqcNumLog = ((InitResc *)mboxBuf)->cqsNumLog;
         mrRescModule.mptCache.setBase(regs.mptBase);//设置各个元数据的基地址
         mrRescModule.mttCache.setBase(regs.mttBase);
         qpcModule.setBase(regs.qpcBase);
         cqcModule.cqcCache.setBase(regs.cqcBase);
         break;
       case WRITE_ICM:
         //写ICM的批处理chunknum为regs.modifier
         //为每个resc-cache写入ICM映射关系
         switch (regs.outParam.oparaml()) {
           case ICMTYPE_MPT:
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_MPT command!\n");
             mrRescModule.mptCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
             break;
           case ICMTYPE_MTT:
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_MTT command!\n");
             mrRescModule.mttCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
             break;
           case ICMTYPE_QPC:
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_QPC command!\n");
             qpcModule.icmStore((IcmResc *)mboxBuf, regs.modifier);
             break;
           case ICMTYPE_CQC:
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: ICMTYPE_CQC command!\n");
             cqcModule.cqcCache.icmStore((IcmResc *)mboxBuf, regs.modifier);
             break;
           default: /* ICM mapping do not belong any Resources. */
             panic("ICM mapping do not belong any Resources.\n");
         }
         break;
       case WRITE_MPT:
         HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MPT command! mod %d ouParam %d\n", regs.modifier, regs.outParam._data);
         for (int i = 0; i < regs.outParam._data; ++i) {
             MptResc *tmp = (((MptResc *)mboxBuf) + i);
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MPT command! mpt_index %d tmp_addr 0x%lx\n", tmp->key, (uintptr_t)tmp);
             mrRescModule.mptCache.rescWrite(tmp->key, tmp);
         }
         break;
       case WRITE_MTT:
         HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_MTT command!\n");
         for (int i = 0; i < regs.outParam._data; ++i) {
             mrRescModule.mttCache.rescWrite(regs.modifier + i, ((MttResc *)mboxBuf) + i);
         }
         break;
         
       case WRITE_QPC:
         HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_QPC command! 0x%lx\n", (uintptr_t)mboxBuf);
         for (int i = 0; i < regs.outParam._data; ++i) {
             CxtReqRspPtr qpcReq = make_shared<CxtReqRsp>(CXT_CREQ_QP, CXT_CHNL_TX, 0); /* last param is useless here */
             qpcReq->txQpcReq = new QpcResc;//要写的QPC信息
             memcpy(qpcReq->txQpcReq, (((QpcResc *)mboxBuf) + i), sizeof(QpcResc));
             HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_QPC command! i %d qpn 0x%x(%d), addr 0x%lx\n", 
                     i, qpcReq->txQpcReq->srcQpn, qpcReq->txQpcReq->srcQpn&QPN_MASK, (uintptr_t)qpcReq->txQpcReq);
             qpcReq->num = qpcReq->txQpcReq->srcQpn;
             qpcModule.postQpcReq(qpcReq); /* post create request to qpcModule */
         }
         delete[] mboxBuf;//mboxbuf数据转移至qpcReq->txQpcReq，所以可删除
         break;
       case WRITE_CQC:
         HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: WRITE_CQC command! regs_mod %d mb 0x%lx\n", regs.modifier,  (uintptr_t)mboxBuf);
         cqcModule.cqcCache.rescWrite(regs.modifier, (CqcResc *)mboxBuf);
         break;
       default:
         panic("Bad inputed command.\n");
     }
     regs.cmdCtrl.go(0); // Set command indicator as finished.
 
     HANGU_PRINT(CcuEngine, " CcuEngine.CEU.mboxFetchCpl: `GO` bit is down!\n");
 }
 //处理程序向HCR寄存器写入的控制数据，maybe
 //CCU会通过dma从内存获取数据,并触发mboxevent
 void
 HanGuRnic::ceuProc () {
     
     HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc!\n");
 
     int size;
     switch (regs.cmdCtrl.op()) {//查看cmdCtrl寄存器的op字节
       case INIT_ICM :
         size = sizeof(InitResc); // MBOX_INIT_SZ;
         mboxBuf = (uint8_t *)new InitResc;
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: INIT_ICM command!\n");
         break;
       case WRITE_ICM:
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_ICM command!\n");
         size = regs.modifier * sizeof(IcmResc); // regs.modifier * MBOX_ICM_ENTRY_SZ;
         mboxBuf = (uint8_t *)new IcmResc[regs.modifier];
         break;
       case WRITE_MPT:
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_MPT command!\n");
         size = regs.outParam._data * sizeof(MptResc);
         mboxBuf = (uint8_t *)new MptResc[regs.outParam._data];
         break;
       case WRITE_MTT:
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_MTT command!\n");
         size = regs.outParam._data * sizeof(MttResc);
         mboxBuf = (uint8_t *)new MttResc[regs.outParam._data];
         break;
       case WRITE_QPC:
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_QPC command! batch_size %ld\n", regs.outParam._data);
         size = regs.outParam._data * sizeof(QpcResc);
         mboxBuf = (uint8_t *)new QpcResc[regs.outParam._data];
         break;
       case WRITE_CQC:
         HANGU_PRINT(CcuEngine, " CcuEngine.ceuProc: WRITE_CQC command!\n");
         size = sizeof(CqcResc); // MBOX_CQC_ENTRY_SZ;
         mboxBuf = (uint8_t *)new CqcResc;
         break;
       default:
         size = 0;
         panic("Bad input command.\n");
     }
     //maibox最大占据32页,拥有32<<12字节的地址空间
     assert(size > 0 && size <= (MAILBOX_PAGE_NUM << 12)); /* size should not be zero */
 
     /* read mailbox through dma engine */
     DmaReqPtr dmaReq = make_shared<DmaReq>(pciToDma(regs.inParam._data), size, 
             &mboxEvent, mboxBuf, 0); /* last param is useless here */
 
     //inParam里装的mailbox的物理地址，dma去取这个地方的东西，相当于可以取到mailbox虚拟地址写的initresc内容【在driver里实现的】
     ccuDmaReadFifo.push(dmaReq);
     if (!dmaEngine.dmaReadEvent.scheduled()) {
         schedule(dmaEngine.dmaReadEvent, curTick() + clockPeriod());
     }
 }
 
 /**
  * @brief Doorbell Forwarding Unit
  * Forwarding doorbell to RDMAEngine.DFU.
  * Post QPC read request to read relatived QPC information.
  */
 //从要写入的DB数据中，根据QPN，向QPCmodule请求QPC数据，QPCmodule在获取QPC数据后，会调用DFUprocessing函数处理
 void
 HanGuRnic::doorbellProc () {
 
     HANGU_PRINT(HanGuRnic, " CCU.doorbellProc! db_size %d\n", pio2ccuDbFifo.size());
 
     /* If there's no valid idx, exit the schedule */
     //indexfifo存储可用乱序索引，用于不同QP乱序无阻塞访问【猜测】
     if (df2ccuIdxFifo.size() == 0) {
         HANGU_PRINT(CcuEngine, " CCU.doorbellProc, If there's no valid idx, exit the schedule\n");
         return;
     }
 
     /* read doorbell info */
     assert(pio2ccuDbFifo.size());
     DoorbellPtr dbell = pio2ccuDbFifo.front();
     pio2ccuDbFifo.pop();
 
     /* Push doorbell to doorbell fifo */
     uint8_t idx = df2ccuIdxFifo.front();
     df2ccuIdxFifo.pop();
     doorbellVector[idx] = dbell;
     /* We don't schedule it here, cause it should be 
      * scheduled by Context Module. */
     // if (!rdmaEngine.dfuEvent.scheduled()) { /* Schedule RdmaEngine.dfuProcessing */
     //     schedule(rdmaEngine.dfuEvent, curTick() + clockPeriod());
     // }
 
     /* Post QP addr request to QpcModule */
     //读取QPC，结束时调用DFU处理
     CxtReqRspPtr qpAddrReq = make_shared<CxtReqRsp>(CXT_RREQ_SQ, 
             CXT_CHNL_TX, dbell->qpn, 1, idx); // regs.db.qpn()
     qpAddrReq->txQpcRsp = new QpcResc;
     qpcModule.postQpcReq(qpAddrReq);
 
     HANGU_PRINT(CcuEngine, " CCU.doorbellProc: db.qpn %d df2ccuIdxFifo.size %d idx %d\n", 
             dbell->qpn, df2ccuIdxFifo.size(), idx);
 
     /* If there still has elem in fifo, schedule myself again */
     if (df2ccuIdxFifo.size() && pio2ccuDbFifo.size()) {
         if (!doorbellProcEvent.scheduled()) {
             schedule(doorbellProcEvent, curTick() + clockPeriod());
         }
     }
 
     HANGU_PRINT(CcuEngine, " CCU.doorbellProc: out!\n");
 }
 ///////////////////////////// HanGuRnic::CCU relevant {end}//////////////////////////////
 
 ///////////////////////////// HanGuRnic::RDMA Engine relevant {begin}//////////////////////////////
 //ok，将一个8位整型转化为32位整型
 uint32_t
 HanGuRnic::RdmaEngine::txDescLenSel (uint8_t num) {
     return (uint32_t)num;
 }
 
 /**
  * @brief Descriptor fetching Unit
  * Post descriptor read request and recv relatived QPC information
  * Pass useful information to DDU.
  */
 //DFU处理模块，接收来自QPCmodule的QPC信息，然后交由DDU处理
 void
 HanGuRnic::RdmaEngine::dfuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dfuProcessing!\n");
 
     /* read qpc sq addr */
     //DFU会通过txQpAddrReqFifo向QPC模块发送读取QPC信息，然后返回的信息会通过txQpAddrRspFifo读取
     assert(rnic->qpcModule.txQpAddrRspFifo.size());
     CxtReqRspPtr qpcRsp = rnic->qpcModule.txQpAddrRspFifo.front();
     uint8_t idx = qpcRsp->idx;
     rnic->qpcModule.txQpAddrRspFifo.pop();
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dfuProcessing: idx %d\n", idx);
     // for (int i = 0; i < rnic->doorbellVector.size(); ++i) {
     //     HANGU_PRINT(RdmaEngine, " RdmaEngine.dfuProcessing:doorbellVec %d\n", (rnic->doorbellVector[i] != nullptr));
     // }
 
     /* Get doorbell rrelated to the qpc
      * If the index fifo is empty, reschedule ccu.dfu event */
     assert(rnic->doorbellVector[idx] != nullptr);
     DoorbellPtr dbell = rnic->doorbellVector[idx];
     rnic->doorbellVector[idx] = nullptr;
     rnic->df2ccuIdxFifo.push(idx); //回收idx
     if ((rnic->df2ccuIdxFifo.size() == 1) && rnic->pio2ccuDbFifo.size()) { 
         if (!rnic->doorbellProcEvent.scheduled()) {
             rnic->schedule(rnic->doorbellProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     /* Post doorbell to DDU */
     df2ddFifo.push(dbell);
     /* We don't schedule it here, cause it should be 
      * scheduled by Memory Region Module, dmaRrspProcessing */
     // if (!dduEvent.scheduled()) { /* Schedule RdmaEngine.dduProcessing */
     //     rnic->schedule(dduEvent, curTick() + rnic->clockPeriod());
     // }
 
     assert(qpcRsp->txQpcRsp->srcQpn == dbell->qpn);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dfuProcessing:"
             " Post descriptor to MR Module! sndBaselkey: %d, qpn %d, num %d, opcode %d, dbell->offset %d\n", 
             qpcRsp->txQpcRsp->sndWqeBaseLkey, dbell->qpn, dbell->num, dbell->opcode, dbell->offset);
 
     /* Post Descriptor read request to MR Module */
     //把读取描述符的请求，发送给MRmodule，MRmodule处理此读请求后，将读取的数据放入txdescRspfifo中，并调用DDUevent
     MrReqRspPtr descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DESC,
             qpcRsp->txQpcRsp->sndWqeBaseLkey, 
             txDescLenSel(dbell->num) << 5, dbell->offset);
     descReq->txDescRsp = new TxDesc[dbell->num];
     rnic->descReqFifo.push(descReq);
     if (!rnic->mrRescModule.transReqEvent.scheduled()) { /* Schedule MrRescModule.transReqProcessing */
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
     
     /* If doorbell fifo & addr fifo has event, schedule myself again. */
     if (rnic->qpcModule.txQpAddrRspFifo.size()) {
         if (!dfuEvent.scheduled()) { /* Schedule DfuProcessing */
             rnic->schedule(dfuEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dfuProcessing: out!\n");
 }
 
 /**
  * @note Called by dduEvent, I am scheduled by MrRescModule.dmaRrspProcessing
  *       and myself.
  *       
  *       This function is used to read QPC from CxtRescModule. We request the 
  *       QPC even if last cycle we request the same QPC (though this QPC is 
  *       decayed). We did this beacuse we hope Context Module knows the QPC 
  *       requirement. Later in rgu, we just abandon the fetched QPC, and 
  *       replace it with QPC stored in rgu.
  *       
  */
 void
 HanGuRnic::RdmaEngine::dduProcessing () {
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dduProcessing!\n");
 
     /* If there's no valid idx, exit the schedule */
     if (dp2ddIdxFifo.size() == 0) {
         HANGU_PRINT(HanGuRnic, " RdmaEngine.dduProcessing: If there's no valid idx, exit the schedule\n");
         return;
     }
 
     if (this->allowNewDb) {//如果允许新的DB
         /* Fetch Doorbell from DFU fifo */
         assert(df2ddFifo.size());
         assert(this->dduDbell == nullptr);
         this->dduDbell = df2ddFifo.front();
         df2ddFifo.pop();
         this->allowNewDb = false;//可能decode状态时不允许再allowNewDb
         HANGU_PRINT(RdmaEngine, " RdmaEngine.dduProcessing: Get one Doorbell!\n");
     }
 
     /* Fetch one descriptor from tx descriptor fifo */
     assert(rnic->txdescRspFifo.size()); /* TPT calls this function, so 
                                          * txDescFifo should have items */
     TxDescPtr txDesc = rnic->txdescRspFifo.front();
     rnic->txdescRspFifo.pop();
 
     /* Put one descriptor to waiting Memory */
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dduProcessing: desc->len 0x%x, desc->lkey 0x%x, desc->lvaddr 0x%x, desc->opcode 0x%x, desc->flags 0x%x, dduDbell->qpn 0x%x\n", 
             txDesc->len, txDesc->lkey, txDesc->lVaddr, txDesc->opcode, txDesc->flags, dduDbell->qpn);
     uint8_t idx = dp2ddIdxFifo.front();
     dp2ddIdxFifo.pop();//使用一个index
     assert(dd2dpVector[idx] == nullptr);
     dd2dpVector[idx] = txDesc;
     /* We don't schedule it here, cause it should be 
      * scheduled by Context Module */
     // if (!dpuEvent.scheduled()) { /* Schedule RdmaEngine.dpuProcessing */
     //     rnic->schedule(dpuEvent, curTick() + rnic->clockPeriod());
     // }
 
     /* Post qp read request to QpcModule */
     CxtReqRspPtr qpcRdReq = make_shared<CxtReqRsp>(CXT_RREQ_QP, CXT_CHNL_TX, dduDbell->qpn, 1, idx); /* dduDbell->num */
     qpcRdReq->txQpcRsp = new QpcResc;
     rnic->qpcModule.postQpcReq(qpcRdReq);//另一种读取QPC的操作，结束后调用DPU处理
 
     /* update allowNewDb */
     --this->dduDbell->num;//递减这个num，直到为0，结束此次批处理
     if (this->dduDbell->num == 0) {
         this->allowNewDb = true;
         this->dduDbell = nullptr;
     }
 
     /* Schedule myself again if there's new descriptor
      * or there remains descriptors to post */
     if (dp2ddIdxFifo.size() && rnic->txdescRspFifo.size() && 
             ((allowNewDb && df2ddFifo.size()) || (!allowNewDb))) {
             //要么本次批处理还没结束（即!allowNewDb）
             //要么结束了，可进行下一次批处理但df2ddFifo要有传过来的DBell（即allowNewDb && df2ddFifo.size()）
         if (!dduEvent.scheduled()) { /* Schedule myself */
             rnic->schedule(dduEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dduProcessing: out!\n");
 }
 
 uint32_t
 HanGuRnic::RdmaEngine::getRdmaHeadSize (uint8_t opcode, uint8_t qpType) {
     switch(opcode) {
       case OPCODE_SEND ://RDMA-send
         switch (qpType) {
           case QP_TYPE_RC://如果是RC传输类型
             return PKT_BTH_SZ;//8字节
           case QP_TYPE_UD://如果是UD传输类型
             return PKT_BTH_SZ + PKT_DETH_SZ;//16字节
           default:
             panic("QP type error!");
             return 0;
         }
       case OPCODE_RDMA_WRITE://write或read原语
       case OPCODE_RDMA_READ:
         assert(qpType == QP_TYPE_RC);//只支持RC传输类型
         return PKT_BTH_SZ + PKT_RETH_SZ;
       default:
         panic("Error! Post wrong descriptor type to send queue. (in getRdmaHeadSize) opcode %d\n", opcode);
         return 0;
     }
 }
 
 /**
  * @note Called by dpuEvent, it's scheduled by CxtRescModule.cxtRspProcessing 
  *       and myself. 
  *       QPC in qpcModule.txQpcRspFifo may be decayed. We just put it to rgu, and 
  *       rgu may replace it with its own newer QPC.
  */
 void
 HanGuRnic::RdmaEngine::dpuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing!\n");
 
     /* Get Context from Context Module */
     assert(rnic->qpcModule.txQpcRspFifo.size());
     CxtReqRspPtr dpuQpc = rnic->qpcModule.txQpcRspFifo.front();
     rnic->qpcModule.txQpcRspFifo.pop();
     assert((dpuQpc->txQpcRsp->qpType == QP_TYPE_RC) ||
             (dpuQpc->txQpcRsp->qpType == QP_TYPE_UD)); /* we should only use RC and UD type QP */
 
     /* Get one descriptor entry from RdmaEngine.dduProcessing */
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing: num %d, idx %d\n", dpuQpc->num, dpuQpc->idx);
     uint8_t idx = dpuQpc->idx;
     assert(dd2dpVector[idx] != nullptr);
     TxDescPtr desc = dd2dpVector[idx];
     dd2dpVector[idx] = nullptr;
     assert(desc->len <= 4096);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing:"
                 " Get descriptor entry from RdmaEngine.dduProcessing, len: %d, lkey: %d, opcode: %d, rkey: %d\n", 
                 desc->len, desc->lkey, desc->opcode, desc->rdmaType.rkey);
 
     /* schedule ddu if dp2ddIdxFifo is empty */
     dp2ddIdxFifo.push(idx);//回收一个index
     if ((dp2ddIdxFifo.size() == 1) && rnic->txdescRspFifo.size() && 
             ((allowNewDb && df2ddFifo.size()) || (!allowNewDb))) {
         if (!dduEvent.scheduled()) {
             rnic->schedule(dduEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     /* Generate request packet (RDMA read/write, send) */
     EthPacketPtr txPkt = std::make_shared<EthPacketData>(16384);
     txPkt->length = ETH_ADDR_LEN * 2 + getRdmaHeadSize(desc->opcode, dpuQpc->txQpcRsp->qpType); /* ETH_ADDR_LEN * 2 means length of 2 MAC addr */
 
     /* Post Descriptor & QPC & request packet pointer to RdmaEngine.rguProcessing */
     DP2RGPtr dp2rg = make_shared<DP2RG>();
     dp2rg->desc = desc;
     dp2rg->qpc  = dpuQpc->txQpcRsp;
     dp2rg->txPkt= txPkt;
     dp2rgFifo.push(dp2rg);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing: Post Desc & QPC to RdmaEngine.rguProcessing qpn: %d. sndPsn %d qpType %d dpuQpc->sz %d\n", 
             dp2rg->qpc->srcQpn, dp2rg->qpc->sndPsn, dp2rg->qpc->qpType, dpuQpc->sz);
 
     /* Post Data read request to Memory Region Module (TPT) */
     MrReqRspPtr rreq;
     switch(desc->opcode) {
       case OPCODE_SEND ://如果是rdma-send或者rdma-write操作
       case OPCODE_RDMA_WRITE:
         /* Post Data read request to Data Read Request FIFO.
          * Fetch data from host memory */
         HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing: Push Data read request to MrRescModule.transReqProcessing: len %d vaddr 0x%x\n", desc->len, desc->lVaddr);
         rreq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_TX_DATA,
                 desc->lkey, desc->len, (uint32_t)(desc->lVaddr&0xFFF));
         rreq->rdDataRsp = txPkt->data + txPkt->length; /* Address Rsp data (from host memory) should be located */
         //此处将需要放置读取数据的位置一并传给DMAengine，DMA可直接将数据写入此地址
         rnic->dataReqFifo.push(rreq);
         //读取工作数据，结束时调用rgrr模块
         if (!rnic->mrRescModule.transReqEvent.scheduled()) {
             rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
         }
         break;
       case OPCODE_RDMA_READ://因为不用发什么东西，只需要等着读就行，调用rgrr模块
         /* Schedule rg&rru to start Processing RDMA read. 
          * Cause RDMA read don't need to read data from host memory */
         if (!rgrrEvent.scheduled()) { /* Schedule RdmaEngine.rgrrProcessing */
             rnic->schedule(rgrrEvent, curTick() + rnic->clockPeriod());
         }
         break;
       default:
         panic("Error! Post wrong descriptor type to send queue. desc->opcode %d\n", desc->opcode);
         break;
     }
 
     /* Recall myself if there's new descriptor and QPC */
     if (rnic->qpcModule.txQpcRspFifo.size()) {
         if (!dpuEvent.scheduled()) { /* Schedule myself */
             rnic->schedule(dpuEvent, curTick() + rnic->clockPeriod());
         }
     }
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.dpuProcessing: out!\n");
 }
 
 /**
  * @note 
  *      Resend from first elem of the window.
  *      We didn't implement retransmission mechanism yet.
  *      We assume no packet lost now. 
  */
 //默认无传输可靠性问题发生
 void
 HanGuRnic::RdmaEngine::reTransPkt(WinMapElem *winElem, uint32_t pktCnt) {
     return;
 }
 
 //RDMAread响应操作
 void
 HanGuRnic::RdmaEngine::rdmaReadRsp(EthPacketPtr rxPkt, WindowElemPtr winElem) {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rdmaReadRsp\n");
 
     // Post Data Wrte request to fifo
     MrReqRspPtr dataWreq = make_shared<MrReqRsp>(
                 DMA_TYPE_WREQ, TPT_WCHNL_TX_DATA,
                 winElem->txDesc->lkey, 
                 winElem->txDesc->len, 
                 (uint32_t)(winElem->txDesc->lVaddr & 0xFFF));
     dataWreq->wrDataReq = new uint8_t[dataWreq->length]; /* copy data, because the packet will be deleted soon */
     memcpy(dataWreq->wrDataReq, rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_AETH_SZ, dataWreq->length);
     rnic->dataReqFifo.push(dataWreq);
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.RRU.rdmaReadRsp: RDMA read Data is: %s\n", dataWreq->wrDataReq);
 
     // Schedule TPT to start Post completion.
     if (!rnic->mrRescModule.transReqEvent.scheduled()) {
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
 }
 
 //TX发送完成处理模块
 void
 HanGuRnic::RdmaEngine::postTxCpl(uint8_t qpType, uint32_t qpn, 
         uint32_t cqn, TxDescPtr desc) {
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.postTxCpl! qpn %d, cqn %d \n", qpn, cqn);
 
     /* signaled to CQ based on the info from wqe */
     if (!desc->isSignaled()) {
         return;
     }
     
     /* Post related info into scu Fifo */
     CqDescPtr cqDesc = make_shared<CqDesc>(qpType, 
             desc->opcode, desc->len, qpn, cqn);
     rg2scFifo.push(cqDesc);//此为要写的CQ
 
     /* Post Cqc req to CqcModule */
     CxtReqRspPtr cqcRdReq = make_shared<CxtReqRsp>(CXT_RREQ_CQ, CXT_CHNL_TX, cqn);
     cqcRdReq->txCqcRsp = new CqcResc;
     rnic->cqcModule.postCqcReq(cqcRdReq);
     //向cqcmodule发送读取CQC的请求，并在读取后调用SCU模块处理
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.postTxCpl: out!\n");
 }
 
 /**
  * @note
  *      Response Receiving Unit Processing.
  *      This function is called by rgrrProcessing.
  *      Note that retransmission mechanism is not fully implemented. 
  *      At present, we only provide NACK perception. 
  */
 //RRU处理，处理ACK相关，重传相关操作
 void
 HanGuRnic::RdmaEngine::rruProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing!\n");
 
     /* Get RX ack data from fifo */
     assert(ra2rgFifo.size());
     EthPacketPtr rxPkt = ra2rgFifo.front();
     BTH *bth   = (BTH *)(rxPkt->data + ETH_ADDR_LEN * 2);
     AETH *aeth = (AETH *)(rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ);
     uint32_t destQpn = bth->op_destQpn & 0xFFFFFF;
     uint32_t ackPsn  = bth->needAck_psn & 0xFFFFFF;
     ra2rgFifo.pop();
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing:"
             " Get RX ack data from fifo destQpn %d, ackPsn %d\n", 
             destQpn, ackPsn);
 
     /* Get ACK bounded QP List from Window */
     WinMapElem* winElem;
     if (sndWindowList.find(destQpn) == sndWindowList.end()) {//如果没找到接收的QPN，则报错
         for (auto &item : sndWindowList) {
             uint32_t key = item.first;
             WinMapElem* val = item.second;
             HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing: key %d firstPsn %d lastPsn %d\n\n", 
                     key, val->firstPsn, val->lastPsn);
         }
         panic("[RdmaEngine] RdmaEngine.RGRRU.rruProcessing:"
             " cannot find windows elem according to destQpn\n");
     } 
     winElem = sndWindowList[destQpn];//获取接收的ACK对应的QP窗口序列
     if (winElem->list == nullptr) {
         panic("[RdmaEngine] RdmaEngine.RGRRU.rruProcessing:"
             " destQpn %d has no pending elem\n", destQpn);
     }
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing: Get ACK bounded QP List from Window\n");
 
     /* If RX ACK owns illegal psn, just abandon it. */
     if (winElem->firstPsn > ackPsn || winElem->lastPsn < ackPsn) {
         panic("[RdmaEngine] RdmaEngine.RGRRU.rruProcessing:"
             " RX ACK owns illegal PSN! firstPsn: %d, lastPsn: %d, ackPsn: %d\n", 
             winElem->firstPsn, winElem->lastPsn, ackPsn);
         ackPsn = winElem->firstPsn;
     }
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing: RX ACK owns legal PSN! firstPsn: %d, lastPsn: %d\n", 
             winElem->firstPsn, winElem->lastPsn);
 
     /* If the elem, in Window list, has PSN <= RX PKT's PSN, 
      * release the elem.
      * Note if the elem is a RDMA read packet, 
      * elem's PSN
      * retrans the RDMA read packet. */
     bool reTrans = false;
     while (winElem->firstPsn <= ackPsn) {
         
         /* If this is a NAK packet, resend this packet, 
          * and get out of the loop. */
         if (winElem->firstPsn == ackPsn &&
                 (aeth->syndrome_msn >> 24) == RSP_NAK) {
             reTransPkt(winElem, 1);
             break;//跳出while
         }
 
         /**
          * Process different type of trans packets
          */
         EthPacketPtr winPkt = winElem->list->front()->txPkt;
         switch (( ((BTH *)(winPkt->data + ETH_ADDR_LEN * 2))->op_destQpn >> 24 ) & 0x1F) {
           case PKT_TRANS_SEND_ONLY:
           case PKT_TRANS_RWRITE_ONLY:
             postTxCpl(QP_TYPE_RC, destQpn, winElem->cqn, 
                         winElem->list->front()->txDesc);
             break;
           case PKT_TRANS_RREAD_ONLY:
             if (winElem->firstPsn < ackPsn) {
                 reTransPkt(winElem, ackPsn - winElem->firstPsn);
                 reTrans = true;
             } else if (winElem->firstPsn == ackPsn) {
                 HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rruProcessing: Start RDMA read response receiving process\n");
                 rdmaReadRsp(rxPkt, winElem->list->front());
                 postTxCpl(QP_TYPE_RC, destQpn, winElem->cqn, 
                         winElem->list->front()->txDesc);
             }
             break;
           default:
             panic("winPkt type wrong!\n");
         }
         if (reTrans == true) {
             panic("reTrans!\n");
             break;
         }
         
         /**
          * Update the send window
          * Delete first elem in the list
          */
         ++winElem->firstPsn;
         --windowSize;
         windowFull = (windowSize >= windowCap);
         winElem->list->pop_front();
     }
 }
 //ok
 void 
 HanGuRnic::RdmaEngine::setMacAddr (uint8_t *dst, uint64_t src) {
     for (int i = 0; i < ETH_ADDR_LEN; ++i) {
         dst[ETH_ADDR_LEN - 1 - i] = (src >> (i * 8)) & 0xff;
     }
 }
 
 
 /**
  * @note
  *      Request Generation processing.
  *      This function is called by rgrrProcessing.
  *      !TODO: We don't implement multi-packet message, i.e., maximum size 
  *      for one message is 4KB. 
  */
 //需求产生模块，暂不支持多包发送
 void 
 HanGuRnic::RdmaEngine::rguProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.%s!\n", __func__);
 
     /* Get Descriptor & QPC & packet pointer 
      * from RdmaEngine.dpuProcessing */
     assert(dp2rgFifo.size());
     DP2RGPtr tmp = dp2rgFifo.front();
     TxDescPtr desc = tmp->desc;
     QpcResc *qpc = tmp->qpc;
     EthPacketPtr txPkt = tmp->txPkt;
     dp2rgFifo.pop();
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: qpType %d, qpn %d sndPsn %d sndWqeOffset %d\n", 
             qpc->qpType, qpc->srcQpn, qpc->sndPsn, qpc->sndWqeOffset);
 
     /* Get Request Data (send & RDMA write) 
      * from MrRescModule.dmaRrspProcessing. */
     MrReqRspPtr rspData; /* I have already gotten the address in txPkt, 
                            * so it is useless for me. */
     if (desc->opcode == OPCODE_SEND || desc->opcode == OPCODE_RDMA_WRITE) {//如果目标操作是发送相关的操作
         assert(rnic->txdataRspFifo.size());
         rspData = rnic->txdataRspFifo.front();
         rnic->txdataRspFifo.pop();//获取拿到的数据
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "Get Request Data: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", 
                 (rspData->data)[0], (rspData->data)[1], (rspData->data)[2], (rspData->data)[3], 
                 (rspData->data)[4], (rspData->data)[5], (rspData->data)[6], (rspData->data)[7]);
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "Get Request Data: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n", 
                 *(rspData->data+8), *(rspData->data + 9), *(rspData->data + 10), *(rspData->data + 11), 
                 *(rspData->data + 12), *(rspData->data + 13), *(rspData->data + 14), *(rspData->data + 15));
     }
 
     /* Generate Request packet Header
      * We don't implement multi-packet message now. */
     uint8_t *pktPtr = txPkt->data + ETH_ADDR_LEN * 2;
     uint8_t needAck = 0x01;
     uint32_t bthOp;
     if (desc->opcode == OPCODE_SEND && 
             qpc->qpType == QP_TYPE_RC) { /* RC Send */
         
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: RC send!\n");
 
         //构造BTH数据
         bthOp = ((qpc->qpType << 5) | PKT_TRANS_SEND_ONLY) << 24;
         needAck = 0x01;
         ((BTH *) pktPtr)->op_destQpn = bthOp | qpc->destQpn;
         ((BTH *) pktPtr)->needAck_psn = (needAck << 24) | qpc->sndPsn;
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "BTH head: 0x%x 0x%x\n", 
                 ((BTH *) pktPtr)->op_destQpn, ((BTH *) pktPtr)->needAck_psn);
         
     } else if (desc->opcode == OPCODE_SEND && 
             qpc->qpType == QP_TYPE_UD) { /* UD Send */
         
         //构造BTH+DETH头部
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: UD send!\n");
         
         /* Add BTH header */
         bthOp = ((qpc->qpType << 5) | PKT_TRANS_SEND_ONLY) << 24;
         needAck = 0x00;
         ((BTH *) pktPtr)->op_destQpn = bthOp | desc->sendType.destQpn;
         ((BTH *) pktPtr)->needAck_psn = (needAck << 24) | qpc->sndPsn;
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "BTH head: 0x%x 0x%x\n", 
                 ((BTH *) pktPtr)->op_destQpn, ((BTH *) pktPtr)->needAck_psn);
         pktPtr += PKT_BTH_SZ;
 
         /* Add DETH header */
         ((DETH *) pktPtr)->srcQpn = qpc->srcQpn;
         ((DETH *) pktPtr)->qKey = desc->sendType.qkey;
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "DETH head: 0x%x 0x%x\n", 
                 ((DETH *) pktPtr)->srcQpn, ((DETH *) pktPtr)->qKey);
         
         
     } else if (qpc->qpType == QP_TYPE_RC && 
             desc->opcode == OPCODE_RDMA_WRITE) { /* RC RDMA Write */
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: RC RDMA Write!\n");
         //构造BTH+RETH头部
         // Add BTH header
         bthOp = ((qpc->qpType << 5) | PKT_TRANS_RWRITE_ONLY) << 24;
         needAck = 0x01;
         ((BTH *) pktPtr)->op_destQpn = bthOp | qpc->destQpn;
         ((BTH *) pktPtr)->needAck_psn = (needAck << 24) | qpc->sndPsn;
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "BTH head: 0x%x 0x%x\n", 
                 ((BTH *) pktPtr)->op_destQpn, ((BTH *) pktPtr)->needAck_psn);
         pktPtr += PKT_BTH_SZ;
         
         // Add RETH header
         ((RETH *) pktPtr)->rVaddr_l = desc->rdmaType.rVaddr_l;
         ((RETH *) pktPtr)->rVaddr_h = desc->rdmaType.rVaddr_h;
         ((RETH *) pktPtr)->rKey = desc->rdmaType.rkey;
         ((RETH *) pktPtr)->len = desc->len;
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "RETH head: 0x%x 0x%x 0x%x 0x%x\n", 
                 ((RETH *) pktPtr)->rVaddr_l, ((RETH *) pktPtr)->rVaddr_h, 
                 ((RETH *) pktPtr)->rKey, ((RETH *) pktPtr)->len);
         
     } else if (qpc->qpType == QP_TYPE_RC && 
             desc->opcode == OPCODE_RDMA_READ) { /* RC RDMA Read */
         
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: RC RDMA Write!\n");
         //构造BTH+RETH头部
 
         // Add BTH header
         bthOp = ((qpc->qpType << 5) | PKT_TRANS_RREAD_ONLY) << 24;
         needAck = 0x01;
         ((BTH *) pktPtr)->op_destQpn = bthOp | qpc->destQpn;
         ((BTH *) pktPtr)->needAck_psn = (needAck << 24) | qpc->sndPsn;
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "BTH head: 0x%x 0x%x\n", 
                 ((BTH *) pktPtr)->op_destQpn, ((BTH *) pktPtr)->needAck_psn);
         pktPtr += PKT_BTH_SZ;
 
         // Add RETH header
         ((RETH *) pktPtr)->rVaddr_l = desc->rdmaType.rVaddr_l;
         ((RETH *) pktPtr)->rVaddr_h = desc->rdmaType.rVaddr_h;
         ((RETH *) pktPtr)->rKey = desc->rdmaType.rkey;
         ((RETH *) pktPtr)->len = desc->len;
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: "
                 "RETH head: 0x%x 0x%x 0x%x 0x%x\n", 
                 ((RETH *) pktPtr)->rVaddr_l, ((RETH *) pktPtr)->rVaddr_h, 
                 ((RETH *) pktPtr)->rKey, ((RETH *) pktPtr)->len);
 
     } else {
         panic("Unsupported opcode and QP type combination, "
                 "opcode: %d, type: %d\n", desc->opcode, qpc->qpType);
     }
 
     // Set MAC address
     uint64_t dmac, lmac;
     if (qpc->qpType == QP_TYPE_RC) {
         dmac = qpc->dLid;
         lmac = qpc->lLid;
     } else if (qpc->qpType == QP_TYPE_UD) {
         dmac = desc->sendType.dlid;//UD在本项目中默认是sendtype了
         //UD不是面向连接的传输，QPC不会时刻记录双方通讯信息，对方的lid记录于wqe工作描述符中，和TCP/UDP机理区别相似
         lmac = qpc->lLid;
     } else {
         panic("Unsupported QP type, opcode: %d, type: %d\n", desc->opcode, qpc->qpType);
     }
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: dmac 0x%lx, smac 0x%lx\n", dmac, lmac);
     setMacAddr(txPkt->data, dmac);
     setMacAddr(txPkt->data + ETH_ADDR_LEN, lmac);
 
     txPkt->length    += desc->len;//将数据长度加上去
     txPkt->simLength += desc->len;
 
     /* if the packet need ack, post pkt 
      * info into send window */
     if (needAck) {
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: Need ACK (RC type)!\n");
 
         /* Post Packet to send window */
         WindowElemPtr winElem = make_shared<WindowElem>(txPkt, qpc->srcQpn, 
                 qpc->sndPsn, desc);
         if (sndWindowList.find(qpc->srcQpn) == sndWindowList.end()) { // sndWindowList[qpc->srcQpn] == nullptr
             sndWindowList[qpc->srcQpn] = new WinMapElem;
             sndWindowList[qpc->srcQpn]->list = new WinList;
             sndWindowList[qpc->srcQpn]->cqn = qpc->cqn;
         }//如果无此QPN对应的窗口列表，则新建列表
         if (sndWindowList[qpc->srcQpn]->list->size() == 0) {//如果是新建列表或者空列表，则初始化一个firstpsn
             sndWindowList[qpc->srcQpn]->firstPsn = qpc->sndPsn;
         }
         sndWindowList[qpc->srcQpn]->lastPsn = qpc->sndPsn;
         sndWindowList[qpc->srcQpn]->list->push_back(winElem);
 
         assert(sndWindowList[qpc->srcQpn]->firstPsn <= sndWindowList[qpc->srcQpn]->lastPsn);
 
         /* Update the state of send window.  
          * If window is full, block RC transmission */
         ++windowSize;//block操作再rgrru处理模块中实现判断
         windowFull = (windowSize >= windowCap);
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: windowSize %d\n", windowSize);
     }
     
     /* Post Send Packet. Schedule RdmaEngine.sauProcessing 
      * to Send Packet through Ethernet Interface. */
     txsauFifo.push(txPkt);
     if (!sauEvent.scheduled()) {
         rnic->schedule(sauEvent, curTick() + rnic->clockPeriod());
     }
     messageEnd = true; /* Just ignore it now. */
 
     /* Update QPC */
     //包裹号增加1
     if (qpc->qpType == QP_TYPE_RC) {
         ++qpc->sndPsn;
     }
     
     /* Same as in userspace drivers */
     qpc->sndWqeOffset += sizeof(TxDesc);
     //sndwqeOffset是wqe的再SQ内部的偏移量
     if (qpc->sndWqeOffset + sizeof(TxDesc) > (1 << qpc->sqSizeLog)) {
         qpc->sndWqeOffset = 0;  /* qpc->sqSizeLog */ 
     }
     
     /* Post CQ if no need to acks. */
     //如果不需要ACK，即不可靠传输，则可立即告知完成任务，不再支持可靠性机制
     if (!needAck && messageEnd) {
         postTxCpl(qpc->qpType, qpc->srcQpn, qpc->cqn, desc);
     }
 
     /* !TODO: we may need to implement timer here. */
     delete qpc; /* qpc is useless *///此处数据是副本，因为是qpc的cache使用memcopy过来的，故现在没作用，操作不会误删cache上数据
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RGRRU.rguProcessing: out!\n");
 }
 
 bool
 HanGuRnic::RdmaEngine::isReqGen() {//本地需求是否已产生
     return dp2rgFifo.size() && (rnic->txdataRspFifo.size() ||
             (dp2rgFifo.front()->desc->opcode == OPCODE_RDMA_READ));
 }
 
 bool
 HanGuRnic::RdmaEngine::isRspRecv() {//是否收到信息
     return ra2rgFifo.size();
 }
 
 bool
 HanGuRnic::RdmaEngine::isWindowBlocked() {//返回窗口情况，用于发送，仅限于可靠RC传输类型
     return windowFull && 
             (dp2rgFifo.front()->qpc->qpType == QP_TYPE_RC);
 }
 
 /**
  * @note Called by rgrrEvent, scheduled by rdmaEngine.dpuProcessing, 
  *       rdmaEngine.rauProcessing, MrRescModule.dmaRrspProcessing and 
  *       my self.
  */
 //RGRRU处理单元
 void
 HanGuRnic::RdmaEngine::rgrrProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rgrrProcessing\n");
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rgrrProcessing: dp2rgFifo.size %d rnic->txdataRspFifo %d\n", 
             dp2rgFifo.size(), rnic->txdataRspFifo.size());
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rgrrProcessing: isRspRecv %d isReqGen %d windowSize %d\n", 
             isRspRecv(), isReqGen(), windowSize);
     
     /* Rsp has higher priority than req generation, 
      * in case of dead lock. */
     //设置接收优先级高于发送，防止死锁
     if (isRspRecv()) {
         rruProcessing();
     } else if (isReqGen()) {
         if (isWindowBlocked()) {
             return;
         }
         rguProcessing();
     }
 
     /* Schedule myself when there's req need to generate or 
      * Ack need to recv. */
     if (isRspRecv() || isReqGen()) {
         if (!rgrrEvent.scheduled()) {
             rnic->schedule(rgrrEvent, curTick() + rnic->clockPeriod());
         }
     }
 }
 
 //SCU处理函数，处理发送结束相关操作
 void
 HanGuRnic::RdmaEngine::scuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.scuProcessing!\n");
 
     assert(rnic->txCqcRspFifo.size());
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.scuProcessing: cq offset: %d, cq lkey %d, qpn %d, cqn %d, transtype: %d\n", 
             rnic->txCqcRspFifo.front()->txCqcRsp->offset, 
             rnic->txCqcRspFifo.front()->txCqcRsp->lkey, 
             rg2scFifo.front()->qpn, rg2scFifo.front()->cqn, rg2scFifo.front()->transType);
     
     /* Get Cq addr lkey, and post CQ WC to TPT */
     //根据CQC，将CQ数据写回TPT中
     MrReqRspPtr cqWreq = make_shared<MrReqRsp>(DMA_TYPE_WREQ, TPT_WCHNL_TX_CQUE,
             rnic->txCqcRspFifo.front()->txCqcRsp->lkey, sizeof(CqDesc), 
             rnic->txCqcRspFifo.front()->txCqcRsp->offset);
     rnic->txCqcRspFifo.pop();
     cqWreq->cqDescReq = new CqDesc(rg2scFifo.front()->srvType, 
                                     rg2scFifo.front()->transType, 
                                     rg2scFifo.front()->byteCnt, 
                                     rg2scFifo.front()->qpn, 
                                     rg2scFifo.front()->cqn);
     rg2scFifo.pop();
     rnic->cqWreqFifo.push(cqWreq);
 
     // Schedule tarnsReq event(TPT) to post CQ WC to TPT
     if (!rnic->mrRescModule.transReqEvent.scheduled()) { // If not scheduled yet, schedule the event.
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
 
     /* Schedule myself if still has elem in fifo */
     if (!rnic->txCqcRspFifo.empty() && !rg2scFifo.empty()) {
         if (!scuEvent.scheduled()) {
             rnic->schedule(scuEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.scuProcessing: out\n");
 
     return;
 
 }
 
 //发送仲裁器模块处理
 void
 HanGuRnic::RdmaEngine::sauProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.sauProcessing!\n");
 
     if (txsauFifo.empty()) {
         return;
     }
 
     /**
      * unit: ps
      * We don't use it, cause etherswitch has done this work
      */
     Tick bwDelay = txsauFifo.front()->length * rnic->etherBandwidth;
     //此处的length已是整个packet的大小
 
     /* Used only for Debug Print */
     BTH *bth = (BTH *)(txsauFifo.front()->data + ETH_ADDR_LEN * 2);
     uint8_t type = (bth->op_destQpn >> 24) & 0x1f;
     uint8_t srv  = bth->op_destQpn >> 29;
     // for (int i = 0; i < ETH_ADDR_LEN; ++i) {
     //     HANGU_PRINT(RdmaEngine, " RdmaEngine.sauProcessing, dmac[%d]: 0x%x smac[%d] 0x%x\n", i, dmac[i], i, smac[i]);
     // }
     HANGU_PRINT(RdmaEngine, " RdmaEngine.sauProcessing, type: %d srv : %d, BW %d, len %d, bwDelay %d\n", 
             type, srv, rnic->etherBandwidth, txsauFifo.front()->length, bwDelay);
 
     if (rnic->etherInt->sendPacket(txsauFifo.front())) {
         
         HANGU_PRINT(RdmaEngine, " RdmaEngine.sauProcessing: TxFIFO: Successful transmit!\n");
 
         rnic->txBytes += txsauFifo.front()->length;//记录已发送的字节数
         rnic->txPackets++;//记录已发送的包数
 
         txsauFifo.pop();
     }
 
     if (txsauFifo.size()) {
         if (!sauEvent.scheduled()) {
             rnic->schedule(sauEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.sauProcessing: out\n");
 }
 
 //判断是否是ACK类型的包
 bool 
 HanGuRnic::RdmaEngine::isAckPkt(EthPacketPtr rxPkt) {
     BTH *bth = (BTH *)(rxPkt->data + ETH_ADDR_LEN * 2);
     return ((bth->op_destQpn >> 24) & 0x1F) == PKT_TRANS_ACK;
 }
 
 //接收仲裁器处理模块
 void
 HanGuRnic::RdmaEngine::rauProcessing () {
     
     assert(rnic->rxFifo.size());
     EthPacketPtr rxPkt = rnic->rxFifo.front();
     BTH *bth = (BTH *)(rxPkt->data + ETH_ADDR_LEN * 2);
     // for (int i = 0; i < rxPkt->length; ++i) {
     //     HANGU_PRINT(RdmaEngine, " RdmaEngine.rauProcessing: data[%d]: 0x%x\n", i, (rxPkt->data)[i]);
     // }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rauProcessing: op_destQpn: 0x%x\n", bth->op_destQpn);
     
     
     if (((bth->op_destQpn >> 24) & 0x1F) == PKT_TRANS_ACK) { /* ACK packet, transform to RG&RRU */
         //如果是ACK类型接收包，则交给rgrru处理，
         /* pop ethernet pkt from RX channel */
         rnic->rxFifo.pop();
         
         ra2rgFifo.push(rxPkt);
         
         /* Schedule rg&rru to start Processing Response Receiving. */
         if (!rgrrEvent.scheduled()) {
             rnic->schedule(rgrrEvent, curTick() + rnic->clockPeriod());
         }
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.rauProcessing: Receive ACK packet, pass to RdmaEngine.RGRRU.rruProcessing!\n");
 
     } else if (rp2raIdxFifo.size()) { /* Incomming request packet, pass to RPU */
         
         /* pop ethernet pkt from RX channel */
         rnic->rxFifo.pop();
 
         /* read available idx */
         uint8_t idx = rp2raIdxFifo.front();
         rp2raIdxFifo.pop();
 
         HANGU_PRINT(RdmaEngine, " RdmaEngine.rauProcessing: "
                 "Receive request packet, pass to RdmaEngine.rpuProcessing. idx %d\n", idx);
         
         /* Post qpc rd req to qpcModule */
         CxtReqRspPtr rxQpcRdReq = make_shared<CxtReqRsp>(
                                 CXT_RREQ_QP, 
                                 CXT_CHNL_RX, 
                                 (bth->op_destQpn & 0xFFFFFF), //24位QPN
                                 1, 
                                 idx);
         rxQpcRdReq->rxQpcRsp = new QpcResc;
         rnic->qpcModule.postQpcReq(rxQpcRdReq);
         //获取QPC数据后调用RPU处理模块
 
         /* Post RX pkt to RPU */
         rs2rpVector[idx] = rxPkt;
         /* We don't schedule it here, cause it should be 
         * scheduled by Context Module */
         // if (!rpuEvent.scheduled()) { /* Schedule RdmaEngine.rpuProcessing */
         //     rnic->schedule(rpuEvent, curTick() + rnic->clockPeriod());
         // }
     }
 
     /* If there still has elem in fifo, schedule myself again */
     if (rnic->rxFifo.size() && (rp2raIdxFifo.size() || isAckPkt(rnic->rxFifo.front()))) {
         if (!rauEvent.scheduled()) {
             rnic->schedule(rauEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rauProcessing: out\n");
 }
 
 
 void 
 HanGuRnic::RdmaEngine::rpuWbQpc (QpcResc* qpc) {
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: Write back QPC to Context Module. RQ_offset %d, ePSN %d\n", 
             qpc->rcvWqeOffset, qpc->expPsn);
 
     /* post qpc wr req to qpcModule */
     /* !FIXME: We don't use it now, cause we update qpc when read it. */
     // CxtReqRspPtr rxQpcWrReq = make_shared<CxtReqRsp>(CXT_WREQ_QP, CXT_CHNL_RX, qpc->srcQpn);
     // rxQpcWrReq->txQpcReq = qpc;
     // rnic->qpcModule.postQpcReq(rxQpcWrReq);
 }
 
 //用于处理接收到的send类型数据包
 void
 HanGuRnic::RdmaEngine::rcvRpuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing!\n");
 
     /* Get rx descriptor from MrRescModule.dmaRrspProcessing */
     assert(rnic->rxdescRspFifo.size());
     RxDescPtr rxDesc = rnic->rxdescRspFifo.front();
     rnic->rxdescRspFifo.pop();
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: len %d, lkey %d, lVaddr 0x%lx\n", rxDesc->len, rxDesc->lkey, rxDesc->lVaddr);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: Get rx descriptor!\n");
 
     /* get Received packet (from wire) and qpc */
     std::pair<EthPacketPtr, QpcResc*> tmp = rp2rcvRpFifo.front();
     EthPacketPtr rxPkt = tmp.first; 
     QpcResc* qpcCopy = tmp.second; /* just a copy of qpc, original has been written back */
     rp2rcvRpFifo.pop();
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: get Received packet and qpc!\n");
 
     /* Write received data back to memory through MR Module */
     MrReqRspPtr dataWreq = make_shared<MrReqRsp>(
                 DMA_TYPE_WREQ, TPT_WCHNL_RX_DATA,
                 rxDesc->lkey,
                 rxDesc->len,
                 (uint32_t)(rxDesc->lVaddr&0xFFF));
     dataWreq->wrDataReq = new uint8_t[dataWreq->length];
     if (qpcCopy->qpType == QP_TYPE_RC) { /* copy data, because the packet will be deleted soon */
         memcpy(dataWreq->wrDataReq, rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ, dataWreq->length);
     } else {
         memcpy(dataWreq->wrDataReq, rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_DETH_SZ, dataWreq->length);
     }
     rnic->dataReqFifo.push(dataWreq);
     if (!rnic->mrRescModule.transReqEvent.scheduled()) {
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: data to be written back at 0x%x, offset 0x%x, data addr 0x%lx\n", 
             rxDesc->lVaddr, dataWreq->offset, (uintptr_t)(dataWreq->data));
     // for (int i = 0; i < dataWreq->length; ++i) {
     //     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: data[%d] 0x%x\n", i, (dataWreq->wrDataReq)[i]);
     // }
 
     //如果是RC，则需支持ACK回复发送方
     if (qpcCopy->qpType == QP_TYPE_RC) {
         
         EthPacketPtr txPkt = std::make_shared<EthPacketData>(16384);
         txPkt->length = ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_AETH_SZ;//length长度即一个ACK包的长度
         txPkt->simLength = 0;
 
         //设置12字节共2个MAC地址
         memcpy(txPkt->data, rxPkt->data + ETH_ADDR_LEN, ETH_ADDR_LEN); /* set dst mac addr */
         memcpy(txPkt->data + ETH_ADDR_LEN, rxPkt->data, ETH_ADDR_LEN); /* set src mac addr */
 
         //添加BTH头部
         uint32_t bthOp;
         uint8_t *pktPtr = txPkt->data + ETH_ADDR_LEN * 2;
         bthOp = ((qpcCopy->qpType << 5) | PKT_TRANS_ACK) << 24;
         //设置BTH的OP部分为ACK包裹类型
         ((BTH *) pktPtr)->op_destQpn = bthOp | qpcCopy->destQpn;
         ((BTH *) pktPtr)->needAck_psn =  qpcCopy->expPsn;
         pktPtr += PKT_BTH_SZ;
 
         /* Add AETH header */
         ((AETH *) pktPtr)->syndrome_msn = RSP_ACK << 24;//发送ACK，代表已接收到
         //接收到哪个就发哪个的ACK，看来不是累计ACK
 
         //通过SAU，将ACK包发送给接收方
         txsauFifo.push(txPkt);
         if (!sauEvent.scheduled()) {
             rnic->schedule(sauEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     /* Post related info into rcuProcessing for further processing */
     //告知完成了一个recv类型操作
     CqDescPtr cqDesc = make_shared<CqDesc>(qpcCopy->qpType, 
             OPCODE_RECV, rxDesc->len, qpcCopy->srcQpn, qpcCopy->cqn);
     rp2rcFifo.push(cqDesc);
 
     /* Post Cqc read request to CqcModule */
     CxtReqRspPtr rxCqcRdReq = make_shared<CxtReqRsp>(CXT_RREQ_CQ, CXT_CHNL_RX, qpcCopy->cqn);
     rxCqcRdReq->txCqcRsp = new CqcResc;
     rnic->cqcModule.postCqcReq(rxCqcRdReq);//稍后会调用RCU模块，用于处理此完成操作
 
     delete qpcCopy;
 
     /* schedule myself if there's still has elem in input fifo */
     if (rp2rcvRpFifo.size() && rnic->rxdescRspFifo.size()) {
         if (!rcvRpuEvent.scheduled()) {
             rnic->schedule(rcvRpuEvent, curTick() + rnic->clockPeriod());
         }
     }
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rcvRpuProcessing: out!\n");
 }
 //处理RDMAwrite包裹
 void
 HanGuRnic::RdmaEngine::wrRpuProcessing (EthPacketPtr rxPkt, QpcResc* qpc) {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.wrRPU!\n");
     
     //解析写数据包
     RETH *reth = (RETH *)(rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ);
     uint8_t *data = (rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_RETH_SZ);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.wrRPU: Parse received RDMA write packet!\n");
     
    //写回数据包中的数据，与send原语不同的是，RDMAwrite操作不需要rxdesc中指示地址，直接根据数据包中的地址即可写
     MrReqRspPtr dataWreq = make_shared<MrReqRsp>(
                 DMA_TYPE_WREQ, TPT_WCHNL_RX_DATA,
                 reth->rKey,
                 reth->len,
                 (uint32_t)(reth->rVaddr_l & 0xFFF));
     dataWreq->wrDataReq = new uint8_t[dataWreq->length];
     memcpy(dataWreq->wrDataReq, data, dataWreq->length); /* copy data, because the packet will be deleted soon */
     rnic->dataReqFifo.push(dataWreq);
     if (!rnic->mrRescModule.transReqEvent.scheduled()) {
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.wrRPU: Write data back to memory through TPT\n");
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.wrRPU: Recved RDMA write data: %s, rkey: 0x%x, len %d, rvaddr 0x%x\n", 
             dataWreq->wrDataReq, reth->rKey, reth->len, reth->rVaddr_l);
 
     //如果可靠协议，需要支持回复发送方ACK
     if (qpc->qpType == QP_TYPE_RC) {
         
         EthPacketPtr txPkt = std::make_shared<EthPacketData>(16384);
         txPkt->length = ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_AETH_SZ;
         txPkt->simLength = 0;
 
         /* Set Mac addr head */
         memcpy(txPkt->data, rxPkt->data + ETH_ADDR_LEN, ETH_ADDR_LEN); /* set dst mac addr */
         memcpy(txPkt->data + ETH_ADDR_LEN, rxPkt->data, ETH_ADDR_LEN); /* set src mac addr */
 
         /* Add BTH header */
         uint32_t bthOp;
         uint8_t *pktPtr = txPkt->data + ETH_ADDR_LEN * 2;
         bthOp = ((qpc->qpType << 5) | PKT_TRANS_ACK) << 24;
         ((BTH *) pktPtr)->op_destQpn = bthOp | qpc->destQpn;
         ((BTH *) pktPtr)->needAck_psn =  qpc->expPsn;
         pktPtr += PKT_BTH_SZ;
 
         /* Add AETH header */
         ((AETH *) pktPtr)->syndrome_msn = RSP_ACK << 24;
 
         /** Post Send Packet
          * Schedule sau to start Send Packet through Ethernet Interface.
          */
         txsauFifo.push(txPkt);
         if (!sauEvent.scheduled()) {
             rnic->schedule(sauEvent, curTick() + rnic->clockPeriod());
         }
     }
     //重要注释：此处RDMAwrite不需要写回CQ，因为RDMAwrite是单边原语，不需要接收方显式任务下达，所以也没有对应写回CQ操作
     //之后的RDMAread也是同理
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.wrRPU: out!\n");
 }
 
 
 /**
  * @note Process RDMA read incomming pkt, Requester part. (rdCplRpuProcessing is counterpart)
  * This part post data read request to DMAEngine */
 //处理RDMAread的请求包
 void
 HanGuRnic::RdmaEngine::rdRpuProcessing (EthPacketPtr rxPkt, QpcResc* qpc) {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRpuProcessing!\n");
     
     //解析接收的数据包
     RETH *reth = (RETH *)(rxPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRpuProcessing:"
             " Parse received RDMA read packet! len: %d, rKey: 0x%x, vaddr: 0x%x\n", reth->len, reth->rKey, reth->rVaddr_l);
 
     /* Generate RDMA read response packet */
     EthPacketPtr txPkt = std::make_shared<EthPacketData>(16384);
     txPkt->length = ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_AETH_SZ;
     txPkt->simLength = 0;
 
     /* Set Mac addr head */
     memcpy(txPkt->data, rxPkt->data + ETH_ADDR_LEN, ETH_ADDR_LEN); /* set dst mac addr */
     memcpy(txPkt->data + ETH_ADDR_LEN, rxPkt->data, ETH_ADDR_LEN); /* set src mac addr */
 
     /* Add BTH header */
     uint32_t bthOp;
     uint8_t *pktPtr = txPkt->data + ETH_ADDR_LEN * 2;
     bthOp = ((qpc->qpType << 5) | PKT_TRANS_ACK) << 24;
     ((BTH *) pktPtr)->op_destQpn = bthOp | qpc->destQpn;
     ((BTH *) pktPtr)->needAck_psn =  qpc->expPsn;
     pktPtr += PKT_BTH_SZ;
 
     /* Add AETH header */
     ((AETH *) pktPtr)->syndrome_msn = RSP_ACK << 24;
     pktPtr += PKT_AETH_SZ;
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRpuProcessing: Generate RDMA read response packet!\n");
     
     /* Read data from memory through MrRescModule.transReqProcessing */
     MrReqRspPtr dataRreq = make_shared<MrReqRsp>(
                 DMA_TYPE_RREQ, MR_RCHNL_RX_DATA,
                 reth->rKey,
                 reth->len,
                 (uint32_t)(reth->rVaddr_l & 0xFFF)); /* offset, within 4KB */
     dataRreq->rdDataRsp = pktPtr;
     rnic->dataReqFifo.push(dataRreq);
     if (!rnic->mrRescModule.transReqEvent.scheduled()) {
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
     //稍后调用rdCplRpu处理模块
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRpuProcessing:"
             " Read data from memory through MrRescModule.transReqProcessing!\n");
 
     
     /* Post response packet to RdmaEngine.RPU.rdCplRpuProcessing */
     rp2rpCplFifo.push(txPkt);
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRpuProcessing: out!\n");
 }
 //发送ACK包
 void
 HanGuRnic::RdmaEngine::rdCplRpuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRPUCpl!\n");
     
     // Get rsp pkt
     assert(!rnic->rxdataRspFifo.empty());
     assert(!rp2rpCplFifo.empty());
     rnic->rxdataRspFifo.pop();
     EthPacketPtr txPkt = rp2rpCplFifo.front();
     rp2rpCplFifo.pop();
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRPUCpl: data %s!\n", 
             (char *)(txPkt->data + ETH_ADDR_LEN * 2 + PKT_BTH_SZ + PKT_AETH_SZ));
     
     
     /** Post Send Packet
      * Schedule sau to start Send Packet through Ethernet Interface.
      */
     txsauFifo.push(txPkt);
     if (!sauEvent.scheduled()) {
         rnic->schedule(sauEvent, curTick() + rnic->clockPeriod());
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.RPU.rdRPUCpl: out!\n");
 }
 
 uint32_t 
 HanGuRnic::RdmaEngine::rxDescLenSel() {
     return 1;
 }
 //RPU处理模块
 void
 HanGuRnic::RdmaEngine::rpuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing!\n");
 
     /* Get QP context from CxtRescModule.cxtRspProcessing */
     assert(rnic->qpcModule.rxQpcRspFifo.size());
     QpcResc* qpc = rnic->qpcModule.rxQpcRspFifo.front()->rxQpcRsp;
     uint8_t idx = rnic->qpcModule.rxQpcRspFifo.front()->idx;
     rnic->qpcModule.rxQpcRspFifo.pop();
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: Get QPC from cxtRspProcessing. srcQpn: %d, dstQpn %d, qpc->rcvWqeOffset: %d, idx %d\n", 
             qpc->srcQpn, qpc->destQpn, qpc->rcvWqeOffset, idx);
     
     for (int i = 0; i < 100; ++i) {
         if (rs2rpVector[i] != nullptr) {
             HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: idx %d is valid\n", i);
         }
     }
     
     /* Get RX pkt from RdmaEngine.rauProcessing */
     assert(rs2rpVector[idx] != nullptr);
     EthPacketPtr rxPkt = rs2rpVector[idx];
     BTH *bth = (BTH *)(rxPkt->data + ETH_ADDR_LEN * 2);
     rs2rpVector[idx] = nullptr;
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: Get RX pkt from rpuProcessing, bth 0x%x, 0x%x\n", bth->op_destQpn, bth->needAck_psn);
 
     /* reschedule rau if rp2raIdxFifo is not empty && new rx pkt is comming */
     rp2raIdxFifo.push(idx);//回收此index
     if ((rp2raIdxFifo.size() == 1) && rnic->rxFifo.size()) {
         if (!rauEvent.scheduled()) {
             rnic->schedule(rauEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     MrReqRspPtr descReq;
     uint8_t pkt_opcode = (bth->op_destQpn >> 24) & 0x1F;
     QpcResc* qpcCopy;
     switch (pkt_opcode) {
       case PKT_TRANS_SEND_ONLY: /* Call rcvRpuProcessing() later. */
       //如果接收的数据包是SEND原语形式
         HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: PKT_TRANS_SEND_ONLY\n");
         
         /* Post rx descriptor Read request to mrRescModule.transReqProcessing  */
         descReq = make_shared<MrReqRsp>(DMA_TYPE_RREQ, MR_RCHNL_RX_DESC,
                 qpc->rcvWqeBaseLkey, rxDescLenSel() * sizeof(RxDesc), qpc->rcvWqeOffset);
         descReq->rxDescRsp = new RxDesc;
         rnic->descReqFifo.push(descReq);
         if (!rnic->mrRescModule.transReqEvent.scheduled()) { /* Scheduled MR module to read RX descriptor */
             rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
         }
         HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing:"
                 " Post rx descriptor read request to MR module. rq_lkey: 0x%x\n", 
                 qpc->rcvWqeBaseLkey);
 
         /* Post RX packet and qpc to RcvRPU */
         qpcCopy = new QpcResc;
         memcpy(qpcCopy, qpc, sizeof(QpcResc));
         rp2rcvRpFifo.emplace(rxPkt, qpcCopy);
         /* We don't schedule it here, cause it should be 
         * scheduled by MR Module */
         // if (!rcvRpuEvent.scheduled()) { /* Schedule RdmaEngine.RPU.rcvRpuProcessing */
         //     rnic->schedule(rcvRpuEvent, curTick() + rnic->clockPeriod());
         // }
 
         break;
       case PKT_TRANS_RWRITE_ONLY: /* Process RDMA Write */
         wrRpuProcessing(rxPkt, qpc);
         break;
       case PKT_TRANS_RREAD_ONLY: /* Process RDMA Read */
         rdRpuProcessing(rxPkt, qpc);
         break;
       default:
         panic("RX packet type is wrong: 0x%x\n", pkt_opcode);
     }
 
     /* Update QPC, 
      * and Write QPC back to CM module */
     // switch (pkt_opcode) {
     //   case PKT_TRANS_SEND_FIRST:
     //   case PKT_TRANS_SEND_MID:
     //   case PKT_TRANS_SEND_LAST:
     //   case PKT_TRANS_SEND_ONLY:
     //     /* Update the recvWqe in qpc */
     //     qpc->rcvWqeOffset += (rxDescLenSel() * sizeof(RxDesc));
     //     if (qpc->rcvWqeOffset + (rxDescLenSel() * sizeof(RxDesc)) > (1 << qpc->rqSizeLog)) {
     //         qpc->rcvWqeOffset = 0; /* Same as in userspace drivers */ /* qpc->rqSizeLog */ 
     //     }
     //     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: Update the recvWqe in qpc offset %d qpc->rqSizeLog %d\n", qpc->rcvWqeOffset, qpc->rqSizeLog);
     //     break;
     //   default:
     //     break;
     // }
     // if (qpc->qpType == QP_TYPE_RC) {
     //     ++qpc->expPsn;
     // }
     // rpuWbQpc(qpc);
     delete qpc; /* qpc is useless */
 
     /* if we have elem in input fifo, schedule myself again */
     if (rnic->qpcModule.rxQpcRspFifo.size()) {
         if (!rpuEvent.scheduled()) { /* Schedule RdmaEngine.rpuProcessing */
             rnic->schedule(rpuEvent, curTick() + rnic->clockPeriod());
         }
     }
     
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rpuProcessing: out\n");
 }
 
 //接收完成处理模块，写回CQ
 void
 HanGuRnic::RdmaEngine::rcuProcessing () {
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rcuProcessing\n");
     
     /* Get CQ addr lkey, and post CQ Work Completion to MR Module */
     assert(rnic->rxCqcRspFifo.size());
     MrReqRspPtr cqWreq = make_shared<MrReqRsp>(DMA_TYPE_WREQ, TPT_WCHNL_RX_CQUE,
             rnic->rxCqcRspFifo.front()->txCqcRsp->lkey, sizeof(CqDesc), 
             rnic->rxCqcRspFifo.front()->txCqcRsp->offset);
     rnic->rxCqcRspFifo.pop();
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rcuProcessing: cq lkey %d, cq offset %d\n", cqWreq->lkey, cqWreq->offset);
 
     cqWreq->cqDescReq = new CqDesc(rp2rcFifo.front()->srvType, 
                                     rp2rcFifo.front()->transType, 
                                     rp2rcFifo.front()->byteCnt, 
                                     rp2rcFifo.front()->qpn, 
                                     rp2rcFifo.front()->cqn);
     rp2rcFifo.pop();
 
     rnic->cqWreqFifo.push(cqWreq);
     if (!rnic->mrRescModule.transReqEvent.scheduled()) { // If not scheduled yet, schedule the event.
         rnic->schedule(rnic->mrRescModule.transReqEvent, curTick() + rnic->clockPeriod());
     }
 
     /* schedule myself if there's still has elem in input fifo */
     if (rp2rcFifo.size() && rnic->rxCqcRspFifo.size()) {
         if (!rcuEvent.scheduled()) {
             rnic->schedule(rcuEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RdmaEngine, " RdmaEngine.rcuProcessing: out\n");
 }
 
 ///////////////////////////// HanGuRnic::RDMA Engine relevant {end}//////////////////////////////
 
 //1784-3789共2000行，1.2开始读---1.10第一次读完
 //计划1.11-1.20完成hangu_rnic.cc第一次完整通读
 //1.21-1.25完成hangu_rnic.cc温故、细致读
 //1.25-1.30完成hangu_rnic.cc精读和内核驱动复习
 //2.1开始test等其余文件阅读
 ///////////////////////////// HanGuRnic::Resource Cache {begin}//////////////////////////////
 //ok
 template <class T, class S>
 uint32_t
 HanGuRnic::RescCache<T, S>::replaceScheme() {
     
     uint32_t cnt = random_mt.random(0, (int)cache.size() - 1);//生成0到(int)cache.size() - 1的随机数
     
     uint32_t rescNum = cache.begin()->first;
     for (auto iter = cache.begin(); iter != cache.end(); ++iter, --cnt) {
         // HANGU_PRINT(RescCache, " RescCache.replaceScheme: num %d, cnt %d\n", iter->first, cnt);
         if (cnt == 0) {
             rescNum = iter->first;
         }
     }
 
     return rescNum;
 }
 // Write evited elem back to memory
 // 参数addr好像是物理地址PCIe地址，被PCIe的RC转化为DMA物理地址
 //ok
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::storeReq(uint64_t addr, T *resc) {
 
     HANGU_PRINT(RescCache, " storeReq enter\n");
     
     DmaReqPtr dmaReq = make_shared<DmaReq>(rnic->pciToDma(addr), rescSz, 
             nullptr, (uint8_t *)resc, 0); /* rnic->dmaWriteDelay is useless here */
 
     //注意这里的dmaReq指向的数据在堆上，因为使用了make_shared
     dmaReq->reqType = 1; /* this is a write request */
     rnic->cacheDmaAccessFifo.push(dmaReq);
     /* Schedule for fetch cached resources through dma read. */
     if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
         rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
     }
 }
 
  // Read wanted elem from memory
  //只是向DMA引擎发送一个读请求，在本地存储一个待处理的请求
  //addr为icm翻译后的PCIe物理地址
  //cplEvent为cache访问请求指定的结束事件，如果是空指针，说明这是一个写请求
  //rescIdx至请求想要操作的cache索引
  //reqPkt不太清楚是啥
  //rspResc为请求数据返回的地址，如果是空，应该是个写请求
  //rescUpdate为这个请求希望更新cache的策略，为函数指针
  //ok
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::fetchReq(uint64_t addr, Event *cplEvent, 
         uint32_t rescIdx, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {
     
     HANGU_PRINT(RescCache, "fetchReq enter\n");
     
     //临时地址用来存放DMA返回的数据
     T *rescDma = new T; /* This is the origin of resc pointer in cache */
     
     /* Post dma read request to DmaEngine.dmaReadProcessing */
 
     //fetchCplEvent调度fetchRsp
     //这个事件放入包中保证dma引擎在返还数据后可调用，并把数据放入rescDma中，rescDMA因此也必须是指向堆
     //dmaReq也必须指向堆
     DmaReqPtr dmaReq = make_shared<DmaReq>(rnic->pciToDma(addr), rescSz, 
             &fetchCplEvent, (uint8_t *)rescDma, 0); /* last parameter is useless here */
 
     //向cacheDmaAccessFifo存放dma请求
     rnic->cacheDmaAccessFifo.push(dmaReq);
     //如果dma引擎现在空闲，通知其从中获取dma请求并去处理
     if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
         rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
     }
 
     //向rreq2rrspFifo中放入发送了dma请求但还未回应的待定cache请求包
     //注意此处的dmaReq不是副本，而就是原指针
     /* push event to fetchRsp */
     rreq2rrspFifo.emplace(cplEvent, rescIdx, rescDma, reqPkt, dmaReq, rspResc, rescUpdate);
 
     HANGU_PRINT(RescCache, " RescCache.fetchReq: fifo size %d\n", rreq2rrspFifo.size());
 }
 
 /* get fetched data from memory */
 //当接收到DMA发来的数据时，将数据存入本地
 //fetchCplEvent触发的处理函数，合理猜测在DmaEngine.dmaReadProcessing结束后被调度，用以处理dma返回的数据
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::fetchRsp() {
 
     HANGU_PRINT(RescCache, " RescCache.fetchRsp! capacity: %d, size %d, rescSz %d\n", capacity, cache.size(), sizeof(T));
     //待定的fifo里应该有东西
     if (rreq2rrspFifo.empty()) {
         return ;
     }
     //从中取出一个包
     CacheRdPkt rrsp = rreq2rrspFifo.front();
     if (rrsp.dmaReq->rdVld == 0) {
         //猜测如果dma成功，则dma引擎会修改rreq2rrspFifo中的rdVld位
         //之所以会修改，是由于rreq2rrspFifo中不是副本，所以修改发往DMA的等同修改这个fifo中的
         return;
     }
 
     rreq2rrspFifo.pop();
     //打印这个待定请求的相关信息，rescDma为临时存放dma返回数据的地方，rspResc为请求方的地址（猜测）
     HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescNum %d, dma_addr 0x%lx, rsp_addr 0x%lx, fifo size %d\n", 
             rrsp.rescIdx, (uint64_t)rrsp.rescDma, (uint64_t)rrsp.rspResc, rreq2rrspFifo.size());
     
     
     if (cache.find(rrsp.rescIdx) != cache.end()) { /* It has already been fetched */
         /* Abandon fetched resource, and put cache resource 
          * to FIFO. */ 
         memcpy((void *)rrsp.rescDma, (void *)(&(cache[rrsp.rescIdx])), sizeof(T));
         if (rrsp.rspResc) {//如果rrsp.rspResc不为空指针，则需要拷贝数据到返回地址
             memcpy((void *)rrsp.rspResc, (void *)(&(cache[rrsp.rescIdx])), sizeof(T));
         }
     
     } else { /* rsp Resc is not in cache */
         //如果dma获取的数据目前还不在cache中
         //则将获取的数据写入cache中
         if (cache.size() < capacity) {
             cache.emplace(rrsp.rescIdx, *(rrsp.rescDma));
             HANGU_PRINT(RescCache, " RescCache.fetchRsp: capacity %d size %d\n", capacity, cache.size());
         } else { /* Cache is full */
 
             HANGU_PRINT(RescCache, " RescCache.fetchRsp: Cache is full!\n");
             
             uint32_t wbRescNum = replaceScheme();//随机选择一个资源号踢出
             uint64_t pAddr = rescNum2phyAddr(wbRescNum);//通过ICM将这个资源号转化为PCIE物理地址
             T *wbReq = new T;
             memcpy(wbReq, &(cache[wbRescNum]), sizeof(T));
             storeReq(pAddr, wbReq);//将被踢出的条目写回内存中
 
             // Output printing
             if (sizeof(T) == sizeof(struct QpcResc)) {
                 struct QpcResc *val = (struct QpcResc *)(rrsp.rescDma);
                 struct QpcResc *rep = (struct QpcResc *)(wbReq);
                 HANGU_PRINT(RescCache, " RescCache.fetchRsp: qpn 0x%x, sndlkey 0x%x \n\n", 
                         val->srcQpn, val->sndWqeBaseLkey);
                 HANGU_PRINT(RescCache, " RescCache.fetchRsp: replaced qpn 0x%x, sndlkey 0x%x \n\n", 
                         rep->srcQpn, rep->sndWqeBaseLkey);
             }
             // T *cptr = rrsp.rescDma;
             // for (int i = 0; i < sizeof(T); ++i) {
             //     HANGU_PRINT(RescCache, " RescCache.fetchRsp: data[%d] 0x%x\n", i, ((uint8_t *)cptr)[i]);
             // }
 
             cache.erase(wbRescNum);
             cache.emplace(rrsp.rescIdx, *(rrsp.rescDma));
             HANGU_PRINT(RescCache, " RescCache.fetchRsp: capacity %d size %d, replaced idx %d pAddr 0x%lx\n", 
                     capacity, cache.size(), wbRescNum, pAddr);
         }
     
         /* Push fetched resource to FIFO */ 
         //如果rrsp是个读请求，则将数据给请求方
         if (rrsp.rspResc) {
             memcpy(rrsp.rspResc, rrsp.rescDma, sizeof(T));
         }
     }
     /* Schedule read response cplEvent */
     //仅限于read会触发的event
     if (rrsp.cplEvent == nullptr) { // this is a write request
         HANGU_PRINT(RescCache, " RescCache.fetchRsp: this is a write request!\n");
         //一般来说，这里不可能有写请求
     } else { // this is a read request
     //需要调度rrsp规定的结束事件cplEvent
         if (!rrsp.cplEvent->scheduled()) {
             rnic->schedule(rrsp.cplEvent, curTick() + rnic->clockPeriod());
         }
         //专用于读返回的fifo，将获取的数据和相关信息推入fifo中
         rrspFifo.emplace(rrsp.rescDma, rrsp.reqPkt);
     }
     HANGU_PRINT(RescCache, " RescCache.fetchRsp: Push fetched resource to FIFO!\n");
 
     /* Update content in cache entry
      * Note that this should be called last because 
      * we hope get older resource, not updated resource */
     if (rrsp.rescUpdate == nullptr) {
         HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescUpdate is null!\n");
     } else {
         //根据请求者rrsp中的rescUpdate策略再次更新cache
         HANGU_PRINT(RescCache, " RescCache.fetchRsp: rescUpdate is not null\n");
         rrsp.rescUpdate(cache[rrsp.rescIdx]);
     }
 
     /* Schdeule myself if we have valid elem */
     //如果rreq2rrspFifo中仍有元素，则调度自己继续处理来自DMA返回的数据
     //感觉不会成立，因为只有fetchreq往这个fifo中添加，fetchreq在readproc中执行，而readproc执行的条件是rreq2rreq2rrspFifo空
     if (rreq2rrspFifo.size()) {
         CacheRdPkt rrsp = rreq2rrspFifo.front();
         if (rrsp.dmaReq->rdVld) {
             //如果dma请求已被处理
             if (!fetchCplEvent.scheduled()) {
                 rnic->schedule(fetchCplEvent, curTick() + rnic->clockPeriod());
             }
         }
     } else { /* schedule readProc if it do not has pending read req **/
     //如果没有因DMA而待处理的请求了，则可以调度readProc，获取下一个cache请求
         if (!readProcEvent.scheduled()) {
             rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(RescCache, " RescCache.fetchRsp: out\n");
 }
 
 //ok
  /* Set base address of ICM space */
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::setBase(uint64_t base) {
     baseAddr = base;
 }
 /* ICM Write Request */
 //批处理chunk*pageNum个页面的映射关系
 //ok
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::icmStore(IcmResc *icmResc, uint32_t chunkNum) {
     HANGU_PRINT(RescCache, "icmStore enter\n");
 
     for (int i = 0; i < chunkNum; ++i) {
         
         uint32_t idx = (icmResc[i].vAddr - baseAddr) >> 12;
         DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: baseAddr 0x%lx, idx 0x%lx\n", baseAddr, idx);
         DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: vaddr 0x%lx\n", icmResc[i].vAddr);
         while (icmResc[i].pageNum) {
             icmPage[idx] = icmResc[i].pAddr;
             DPRINTF(HanGuRnic, "[HanGuRnic] mbox content: pAddr 0x%lx\n", icmResc[i].pAddr);
             
             /* Update param */
             --icmResc[i].pageNum;
             icmResc[i].pAddr += (1 << 12);
             ++idx;
         }
     }
 
     delete[] icmResc;//为什么要删除呢？
 }
 //将rescNum转化为物理地址
 //ICMPAGE指向ICM页表，内部存储了PPN
 //ok
 template <class T, class S>
 uint64_t
 HanGuRnic::RescCache<T, S>::rescNum2phyAddr(uint32_t num) {
     uint32_t vAddr = num * rescSz;//rescSz即一个entry大小
     uint32_t icmIdx = vAddr >> 12;
     uint32_t offset = vAddr & 0xfff;
     uint64_t pAddr = icmPage[icmIdx] + offset;
 
     return pAddr;
 }
 
 /**
  * @note This Func write back resource and put it back to rrspFifo
  * @param resc resource to be written
  * @param rescIdx resource num
  * @param rescUpdate This is a function pointer, if it is not 
  * nullptr, we execute the function to update cache entries.
  * 
  */
 //写接口函数，写命中立即完成，写丢失但cache有余量也立即完成，写丢失但cache满则一个时钟周期后调度dma事件
 //写没有复杂事件调度，因为写操作不在关键路径，功能仿真，暂不考虑门电路级微体系结构
 //committed on 1.5
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::rescWrite(uint32_t rescIdx, T *resc, const std::function<bool(T&, T&)> &rescUpdate) {
 
     HANGU_PRINT(RescCache, " RescCache.rescWrite! capacity: %d, size: %d rescSz %d, rescIndex %d\n", 
             capacity, cache.size(), sizeof(T), rescIdx);
     // if (sizeof(T) == sizeof(struct QpcResc)) {
     //     for (auto &item : cache) {
     //         uint32_t key = item.first;
     //         struct QpcResc *val = (struct QpcResc *)&(item.second);
     //         HANGU_PRINT(RescCache, " RescCache.rescWrite: cache elem is key 0x%x qpn 0x%x, sndlkey 0x%x \n\n", 
     //                 key, val->srcQpn, val->sndWqeBaseLkey);
     //     }
     // }
     //如果要写的索引在cache中
     if (cache.find(rescIdx) != cache.end()) { /* Cache hit */
 
         HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache hit\n");
         
         /* If there's specified update function */
         if (rescUpdate == nullptr) {//如果没有指定的写操作，默认操作是替换改索引的数据（使用resc指向的数据)
             T tmp = cache[rescIdx];
             cache.erase(rescIdx);
             delete &tmp;
             cache.emplace(rescIdx, *resc);//擦除是保证能写入新的数据
             HANGU_PRINT(RescCache, " RescCache.rescWrite: Resc is written\n");
         } else {
             rescUpdate(cache[rescIdx], *resc);
             HANGU_PRINT(RescCache, " RescCache.rescWrite: Desc updated\n");
         }
 
         // T *cptr = &(cache[rescIdx]);
         // for (int i = 0; i < sizeof(T); ++i) {
         //     HANGU_PRINT(RescCache, " RescCache.rescWrite: data[%d] 0x%x resc 0x%x\n", i, ((uint8_t *)cptr)[i], ((uint8_t *)resc)[i]);
         // }
 
         HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
     } else if (cache.size() < capacity) { /* Cache miss & insert elem directly */
     //cachemiss了，创造一个新的entry写入数据
         HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache miss\n");
 
         cache.emplace(rescIdx, *resc);
         
         HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
     } else if (cache.size() == capacity) { /* Cache miss & replace */
     //如果cachemiss且cache满了，则选择一个踢走，这是一个写穿的cache，写调用了storeReq函数，storeReq函数再调度dma事件
         HANGU_PRINT(RescCache, " RescCache.rescWrite: Cache miss & replace\n");
 
         /* Select one elem in cache to evict */
         uint32_t wbRescNum = replaceScheme();//选择一个索引号
         uint64_t pAddr = rescNum2phyAddr(wbRescNum);//转化为PCIe物理地址
         T *writeReq = new T;
         memcpy(writeReq, &(cache[wbRescNum]), sizeof(T));
         storeReq(pAddr, writeReq);
         //writeReq指向的数据不能是栈上，因为要传递给其他函数
 
         // T *cptr = &(cache[wbRescNum]);
         // HANGU_PRINT(RescCache, " RescCache.rescWrite: cptr 0x%lx\n", (uint64_t)cptr);
         // for (int i = 0; i < sizeof(T); ++i) {
         //     HANGU_PRINT(RescCache, " RescCache.rescWrite: data[%d] 0x%x resc 0x%x\n", i, ((uint8_t *)cptr)[i], ((uint8_t *)resc)[i]);
         // }
 
         // delete &(cache[wbRescNum]); /* It has been written to host memory */
         cache.erase(wbRescNum);
         cache.emplace(rescIdx, *resc);
         HANGU_PRINT(RescCache, " RescCache.rescWrite: wbRescNum %d, ICM_paddr_base 0x%x, new_index %d\n", wbRescNum, pAddr, rescIdx);
         HANGU_PRINT(RescCache, " RescCache: capacity %d size %d\n", capacity, cache.size());
     } else {
         panic(" RescCache.rescWrite: mismatch! capacity %d size %d\n", capacity, cache.size());
     }
 }
 
 
 /**
  * @note This Func get resource and put it back to rrspFifo.
  *      Note that this function returns resc in two data struct:
  *      1. rrspFifo, this Fifo stores reference to the cache.
  *      2. T *rspResc, this input is an optional, which may be "nullptr"
  * @param rescIdx resource num
  * @param cplEvent event to call wehn get desired data.
  * @param rspResc the address to which copy the cache entry
  * @param rescUpdate This is a function pointer, if it is not 
  * nullptr, we execute the function to update cache entries.
  * 
  */
 /* Read resource from Cache */
 //读接口函数，一个时钟周期后调度读处理
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::rescRead(uint32_t rescIdx, Event *cplEvent, S reqPkt, T *rspResc, const std::function<bool(T&)> &rescUpdate) {
 
     HANGU_PRINT(RescCache, " RescCache.rescRead! capacity: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
             capacity, rescIdx, (cplEvent == nullptr), sizeof(T), cache.size());
             //如果cplEvent是空指针，代表是想cache发出的一个写请求
 
     /* push event to fetchRsp */
     //将请求放入reqFifo中，读请求的cplEvent不应该为空，默认rescDMA和dmaReq都为空指针
     //这个reqPkt还不是太理解
     reqFifo.emplace(cplEvent, rescIdx, nullptr, reqPkt, nullptr, rspResc, rescUpdate);
    
    //如果目前没有读处理，则让cache处理读请求、
   
     if (!readProcEvent.scheduled()) {
         rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
     }
 
     HANGU_PRINT(RescCache, " RescCache.rescRead: out!\n");
 }
 
 /**
  * @note This Func get resource req and put it back to rrspFifo.
  *      Note that this function returns resc in two data struct:
  *      1. rrspFifo, this Fifo stores reference to the cache.
  *      2. T *rspResc, this input is an optional, which may be "nullptr"
  */
  //readProcEvent事件触发的处理函数
 
 template <class T, class S>
 void 
 HanGuRnic::RescCache<T, S>::readProc() {
 
     /* If there's pending read req or there's no req in reqFifo, 
      * do not process next rquest */
     //处理是有条件的，要求无缓存请求，且reqFifo不能为空
     //rreq2rrspFifo存放因cache miss而去主存获取数据的cache读请求
     if (rreq2rrspFifo.size() || reqFifo.empty()) {
         return;
     }
 
     /* Get cache rd req pkt from reqFifo */
     CacheRdPkt rreq = reqFifo.front();
     uint32_t rescIdx = rreq.rescIdx;
     reqFifo.pop();
    
     //打印出这个cache请求的信息
     HANGU_PRINT(RescCache, " RescCache.readProc! capacity: %d, rescIdx %d, is_write %d, rescSz: %d, size: %d\n", 
             capacity, rescIdx, (rreq.cplEvent == nullptr), sizeof(T), cache.size());
     // if (sizeof(T) == sizeof(struct QpcResc)) {
     //     for (auto &item : cache) {
     //         uint32_t key = item.first;
     //         struct QpcResc *val = (struct QpcResc *)&(item.second);
     //         HANGU_PRINT(RescCache, " RescCache.readProc0: cache elem is key 0x%x qpn 0x%x, sndlPsn %d \n\n", 
     //                 key, val->srcQpn, val->sndPsn);
     //     }
     // }
     //如果cache命中
     if (cache.find(rescIdx) != cache.end()) { /* Cache hit */
         HANGU_PRINT(RescCache, " RescCache.readProc: Cache hit\n");
         
         /** 
          * If rspResc is not nullptr, which means 
          * it need to put resc to rspResc, copy 
          * data in cache entry.
          */
         if (rreq.rspResc) {
             memcpy(rreq.rspResc, &cache[rescIdx], sizeof(T));
         }
 
         if (rreq.cplEvent == nullptr) { // This is write request
             HANGU_PRINT(RescCache, " RescCache.readProc: This is write request\n");
             //理论上不可能是写请求，因为只有读请求会被放入reqfifo中
         } else { // This is read request
             HANGU_PRINT(RescCache, " RescCache.readProc: This is read request\n");
             
             T *rescBack = new T;
              memcpy(rescBack, &cache[rescIdx], sizeof(T));
 
             /* Schedule read response event */
             if (!rreq.cplEvent->scheduled()) {
                 rnic->schedule(*(rreq.cplEvent), curTick() + rnic->clockPeriod());
             }
             rrspFifo.emplace(rescBack, rreq.reqPkt);//将cache中取出的数据放入回应请求的fifo中
             //rescBack需指向堆，以供请求方读取，若指向栈，则离开此函数后数据被销毁
         }
 
         /* Note that this should be called last because 
          * we hope get older resource, not updated resource */
         //仅在缓存命中时，如若请求中要求对cache本条目更改，则更改
         if (rreq.rescUpdate) {
             rreq.rescUpdate(cache[rescIdx]);
         }
 
         //缓存命中，于是一个时钟周期后从reqFifo中取出并处理下一个cache请求
         //缓存命中访问需2个时钟周期
         if (reqFifo.size()) {
             if (!readProcEvent.scheduled()) {
                 rnic->schedule(readProcEvent, curTick() + rnic->clockPeriod());
             }
         }
 
     } else if (cache.size() <= capacity) { //如果缓存丢失，则需要dma获取内存数据
         HANGU_PRINT(RescCache, " RescCache.readProc: Cache miss & read elem!\n");
         
         /* Fetch required data */
         //通过dma获取数据
         uint64_t pAddr = rescNum2phyAddr(rescIdx);//由ICM计算由索引到实际的PCIe物理地址
         fetchReq(pAddr, rreq.cplEvent, rescIdx, rreq.reqPkt, rreq.rspResc, rreq.rescUpdate);
 
         HANGU_PRINT(RescCache, " RescCache.readProc: resc_index %d, ICM paddr 0x%lx\n", rescIdx, pAddr);
 
     } else {
         panic(" RescCache.readProc: mismatch! capacity %d size %d\n", capacity, cache.size());
     }
 
     HANGU_PRINT(RescCache, " RescCache.readProc: out! capacity: %d, size: %d\n", capacity, cache.size());
 }
 
 ///////////////////////////// HanGuRnic::Resource Cache {end}//////////////////////////////
 
 
 ///////////////////////////// HanGuRnic::Translation & Protection Table {begin}//////////////////////////////
 HanGuRnic::MrRescModule::MrRescModule (HanGuRnic *i, const std::string n, 
         uint32_t mptCacheNum, uint32_t mttCacheNum)
   : rnic(i),
     _name(n),
     chnlIdx(0),
     dmaRrspEvent ([this]{ dmaRrspProcessing(); }, n),
     mptRspEvent  ([this]{ mptRspProcessing();  }, n),
     mttRspEvent  ([this]{ mttRspProcessing();  }, n),
     transReqEvent([this]{ transReqProcessing();}, n),
     mptCache(i, mptCacheNum, n),
     mttCache(i, mttCacheNum, n) { }
 
 
 bool 
 HanGuRnic::MrRescModule::isMRMatching (MptResc * mptResc, MrReqRspPtr mrReq) {
     if (mptResc->key != mrReq->lkey) {
         return false;
     }
     return true;
 }
 
 //先处理MPT
 void 
 HanGuRnic::MrRescModule::mptReqProcess (MrReqRspPtr mrReq) {
     
     HANGU_PRINT(MrResc, " mptReqProcess enter\n");
 
     /* Read MPT entry */
     mptCache.rescRead(mrReq->lkey, &mptRspEvent, mrReq);//读取mptcache中的数据，结束后调度mptRspEvent事件
 }
 //后处理MTT
 void 
 HanGuRnic::MrRescModule::mttReqProcess (uint64_t mttIdx, MrReqRspPtr mrReq) {
 
     HANGU_PRINT(MrResc, " mttReqProcess enter\n");
     
     /* Read MTT entry */
     mttCache.rescRead(mttIdx, &mttRspEvent, mrReq);
 }
 //根据获得的物理地址，通过dma获取数据
 void 
 HanGuRnic::MrRescModule::dmaReqProcess (uint64_t pAddr, MrReqRspPtr mrReq) {
     
     HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess!\n");
     
     if (mrReq->type == DMA_TYPE_WREQ) {//如果这是一个DMA写请求
 
         /* Post dma req to DMA engine */
         DmaReqPtr dmaWreq;
         switch (mrReq->chnl) {
           case TPT_WCHNL_TX_CQUE:
           case TPT_WCHNL_RX_CQUE:
             dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                     nullptr, mrReq->data, 0); /* last parameter is useless here */
             rnic->cqDmaWriteFifo.push(dmaWreq);
 
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write CQ Request, dmaReq->paddr is 0x%lx, offset %d\n", 
                     dmaWreq->addr, mrReq->offset);
             break;
           case TPT_WCHNL_TX_DATA:
           case TPT_WCHNL_RX_DATA:
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write Data request!\n");
             // for (int i = 0; i < mrReq->length; ++i) {
             //     HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: data[%d] 0x%x\n", i, mrReq->data[i]);
             // }
 
             dmaWreq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                     nullptr, mrReq->data, 0); /* last parameter is useless here */
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: write data Request, dmaReq->paddr is 0x%lx, offset %d, size %d\n", 
                     dmaWreq->addr, mrReq->offset, mrReq->length);
             rnic->dataDmaWriteFifo.push(dmaWreq);
 
             break;
         }
         //放入fifo后通知dma引擎处理写请求
         /* Schedule DMA write Engine */
         if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
             rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: schedule dmaWriteEvent\n");
         }
 
 
     } else if (mrReq->type == DMA_TYPE_RREQ) {//如果是一个DMA读请求
 
         DmaReqPtr dmaRdReq;
         switch (mrReq->chnl) {
           case MR_RCHNL_TX_DESC:
           case MR_RCHNL_RX_DESC:
 
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: read desc request lkey 0x%x len %d offset %d\n",
                     mrReq->lkey, mrReq->length, mrReq->offset);
 
             /* Post desc dma req to DMA engine */
             dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                     &dmaRrspEvent, mrReq->data, 0); /* last parameter is useless here */
             rnic->descDmaReadFifo.push(dmaRdReq);
             
             break;
           case MR_RCHNL_TX_DATA:
           case MR_RCHNL_RX_DATA:
 
             HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: read data request\n");
 
             /* Post data dma req to DMA engine */
             dmaRdReq = make_shared<DmaReq>(rnic->pciToDma(pAddr), mrReq->length, 
                     &dmaRrspEvent, mrReq->data, 0); /* last parameter is useless here */
             rnic->dataDmaReadFifo.push(dmaRdReq);
 
             break;
         }
 
         /* Push to Fifo, and dmaRrspProcessing 
          * will fetch for processing */   
         dmaReq2RspFifo.emplace(mrReq, dmaRdReq);
 
         /* Schedule for fetch cached resources through dma read. */
         //调度dma引擎的读处理事件
         if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
             rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
         }
     }
     HANGU_PRINT(MrResc, " MrRescModule.dmaReqProcess: out!\n");
 }
 
 //dma读处理返回数据之后需要的操作，推测为dmareadEvent所调度，这个事件被放入在dmardreq中，供dma引擎调度使用
 void 
 HanGuRnic::MrRescModule::dmaRrspProcessing() {
 
     HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing! FIFO size: %d\n", dmaReq2RspFifo.size());
     
     //dmaReq2RspFifo应该存放了之前待定的dma读请求，同时，首元素要valid
     //解决了一个问题，如果有一个DMA请求比较慢，之后进入此fifo的DMA请求先得到响应
     //为防止回应错位，先看首元素，保证其valid，即使比较慢也要等它好才进一步处理。
     if (dmaReq2RspFifo.empty() || 
             0 == dmaReq2RspFifo.front().second->rdVld) {
         return;
     }
 
     /* Get dma rrsp data */
     MrReqRspPtr tptRsp = dmaReq2RspFifo.front().first;
     dmaReq2RspFifo.pop();
 
     if (tptRsp->type == DMA_TYPE_WREQ) {//必须只有读请求才会调度dmaread
         panic("mrReq type error, write type req cannot put into dmaReq2RspFifo\n");
         return;
     }
     tptRsp->type = DMA_TYPE_RRSP;//读请求包修改为读回复包
 
     HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: tptRsp lkey %d length %d offset %d!\n", 
                 tptRsp->lkey, tptRsp->length, tptRsp->offset);
 
     Event *event;
     RxDescPtr rxDesc;
     TxDescPtr txDesc;
     switch (tptRsp->chnl) {
       case MR_RCHNL_TX_DESC://如果是读发送描述符
         event = &rnic->rdmaEngine.dduEvent;//则过会儿调度rdmaengine解码单元事件
        //i自增到总的资源数小于MR请求length为止
         for (uint32_t i = 0; (i * sizeof(TxDesc)) < tptRsp->length; ++i) {
             txDesc = make_shared<TxDesc>(tptRsp->txDescRsp + i);
             assert((txDesc->len != 0) && (txDesc->lVaddr != 0) && (txDesc->opcode != 0));
             rnic->txdescRspFifo.push(txDesc);
         }
         HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: size is %d, desc total len is %d!\n", 
                 rnic->txdescRspFifo.size(), tptRsp->length);
 
         break;
       case MR_RCHNL_RX_DESC://如果是读接受描述符
         event = &rnic->rdmaEngine.rcvRpuEvent;//则过会儿调度rdma引擎的rcvRPUevent事件
         for (uint32_t i = 0; (i * sizeof(RxDesc)) < tptRsp->length; ++i) {
             rxDesc = make_shared<RxDesc>(tptRsp->rxDescRsp + i);
             assert((rxDesc->len != 0) && (rxDesc->lVaddr != 0));
             rnic->rxdescRspFifo.push(rxDesc);
         }
         delete tptRsp->rxDescRsp;//rxDescRsp是智能指针tptRsp中的指针，该指针值不归智能指针管，且指向堆上数据，可以删除
 
         HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: rnic->rxdescRspFifo.size() is %d!\n", 
                 rnic->rxdescRspFifo.size());
 
         break;
       case MR_RCHNL_TX_DATA:
         event = &rnic->rdmaEngine.rgrrEvent;
         rnic->txdataRspFifo.push(tptRsp);
       
         break;
       case MR_RCHNL_RX_DATA:
         event = &rnic->rdmaEngine.rdCplRpuEvent;
         rnic->rxdataRspFifo.push(tptRsp);
       
         break;
       default:
         panic("TPT CHNL error, there should only exist RCHNL type!\n");
         return;
     }
 
     /* Schedule relevant event in REQ */
     if (!event->scheduled()) {
         rnic->schedule(*event, curTick() + rnic->clockPeriod());
     }
 
     /* Schedule myself if next elem in FIFO is ready */
     if (dmaReq2RspFifo.size() && dmaReq2RspFifo.front().second->rdVld) {
 
         if (!dmaRrspEvent.scheduled()) { /* Schedule myself */
             rnic->schedule(dmaRrspEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(MrResc, " MrRescModule.dmaRrspProcessing: out!\n");
 
 }
 //读取mptentry后，调用此函数
 void
 HanGuRnic::MrRescModule::mptRspProcessing() {
     HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing!\n");
 
     /* Get mpt resource & MR req pkt from mptCache rsp fifo */
     //从mptcache的rrspfifo（访问cache后返回的数据在这个fifo中）中取出一个pair
     MptResc *mptResc   = mptCache.rrspFifo.front().first;
     MrReqRspPtr reqPkt = mptCache.rrspFifo.front().second;
     mptCache.rrspFifo.pop();
    //打印获取的mptentry的各部分信息
     HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: mptResc->lkey 0x%x, len %d, chnl 0x%x, type 0x%x, offset %d\n", 
             mptResc->key, reqPkt->length, reqPkt->chnl, reqPkt->type, reqPkt->offset);
     if (reqPkt->type == 1) {//wreq
         for (int i = 0; i < reqPkt->length; ++i) {
             //把要写的数据打印出来
             HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: data[%d] 0x%x\n", i, reqPkt->data[i]);
         }
     }
 
     /* Match the info in MR req and mptResc */
     //lkey要一致才行
     if (!isMRMatching(mptResc, reqPkt)) {
         panic("[MrRescModule] mpt resc in MR is not match with reqPkt, \n");
     }
 
     /* Calculate required MTT index */
     // uint64_t mttIdx = mptResc->mttSeg + ((reqPkt->offset - (mptResc->startVAddr & 0xFFFF)) >> PAGE_SIZE_LOG);
     reqPkt->offset = reqPkt->offset & 0xFFF;
     uint64_t mttIdx = mptResc->mttSeg;
     HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: reqPkt->offset 0x%x, mptResc->startVAddr 0x%x, mptResc->mttSeg 0x%x, mttIdx 0x%x\n", 
             reqPkt->offset, mptResc->startVAddr, mptResc->mttSeg, mttIdx);
 
     /* Post mtt req */
     mttReqProcess(mttIdx, reqPkt);
 
     /* Schedule myself */
     if (mptCache.rrspFifo.size()) {
         if (!mptRspEvent.scheduled()) {
             rnic->schedule(mptRspEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(MrResc, " MrRescModule.mptRspProcessing: out!\n");
 }
 
 
 //获得mtt的entry后调度此函数
 void
 HanGuRnic::MrRescModule::mttRspProcessing() {
     HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing!\n");
     
     /* Get mttResc from mttCache Rsp fifo */
     MttResc *mttResc   = mttCache.rrspFifo.front().first;
     MrReqRspPtr reqPkt = mttCache.rrspFifo.front().second;
     mttCache.rrspFifo.pop();
     HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing: mttResc->paddr 0x%lx size %d mttCache.rrspFifo %d\n", 
             mttResc->pAddr, reqPkt->length, mttCache.rrspFifo.size());
 
     /* Post dma req */
     //获得了物理地址
     dmaReqProcess(mttResc->pAddr + reqPkt->offset, reqPkt);
 
     /* Schedule myself */
     if (mttCache.rrspFifo.size()) {
         
         if (!mttRspEvent.scheduled()) {
             rnic->schedule(mttRspEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(MrResc, " MrRescModule.mttRspProcessing: out!\n");
 }
 
 //入口函数
 //顺序轮询三个通道，三个通道都无请求
 void
 HanGuRnic::MrRescModule::transReqProcessing() {
 
     HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing!\n");
 
     uint8_t CHNL_NUM = 3;
     bool isEmpty[CHNL_NUM];
     //记录来自rdma_engine的三条请求fifo
     //分别发送、接受描述符读写请求；cq写请求；数据读写请求
     isEmpty[0] = rnic->descReqFifo.empty();
     isEmpty[1] = rnic->cqWreqFifo.empty() ;
     isEmpty[2] = rnic->dataReqFifo.empty();
     //打印三天通道的空闲情况
     HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing isEmpty[0] %d, isEmpty[1] %d, isEmpty[2] %d\n", 
             isEmpty[0], isEmpty[1], isEmpty[2]);
     
     MrReqRspPtr mrReq;
     //分别查看三个fifo
     for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
         if (isEmpty[chnlIdx] == false) {
             switch (chnlIdx) {
               case 0://即descReqFifo非空
                 mrReq = rnic->descReqFifo.front();
                 rnic->descReqFifo.pop();
                 HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Desc read request!\n");
                 break;
               case 1://即cqWreqFifo非空
                 mrReq = rnic->cqWreqFifo.front();
                 rnic->cqWreqFifo.pop();
                 HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Completion Queue write request, offset %d\n", mrReq->offset);
                 break;
               case 2://即dataReqFifo非空
                 mrReq = rnic->dataReqFifo.front();
                 rnic->dataReqFifo.pop();
                 HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: Data read/Write request! data addr 0x%lx\n", (uintptr_t)(mrReq->data));
                 
                 break;
             }
 
             HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: lkey 0x%x, offset 0x%x, length %d\n", 
                         mrReq->lkey, mrReq->offset, mrReq->length);
 
             /* Point to next chnl */
             ++chnlIdx;
             chnlIdx = chnlIdx % CHNL_NUM;//需要mod3，看来是交替执行
 
             /* Schedule this module again if there still has elem in fifo */
             //不断处理直到三个fifo都空
             //调度transReqEvent事件，这个事件的处理函数即自己
             if (!rnic->descReqFifo.empty() || 
                 !rnic->cqWreqFifo.empty()  || 
                 !rnic->dataReqFifo.empty()) {
                 if (!transReqEvent.scheduled()) {
                     rnic->schedule(transReqEvent, curTick() + rnic->clockPeriod());
                 }
             }
 
             /* Read MPT entry */
             mptReqProcess(mrReq);
 
             HANGU_PRINT(MrResc, " MrRescModule.transReqProcessing: out!\n");
             
             return;
         } else {
             /* Point to next chnl */
             ++chnlIdx;
             chnlIdx = chnlIdx % CHNL_NUM;
         }
     }
 }
 ///////////////////////////// HanGuRnic::Translation & Protection Table {end}//////////////////////////////
 
 //1.6 14:32 start
 ///////////////////////////// HanGuRnic::CqcModule {begin}//////////////////////////////
 //入口函数
 bool 
 HanGuRnic::CqcModule::postCqcReq(CxtReqRspPtr cqcReq) {
     //CQCmodue只处理cqc请求，其余不处理
     assert(cqcReq->type == CXT_RREQ_CQ);
 
     if (cqcReq->chnl == CXT_CHNL_TX) {//若请求发送相关的CQC
         rnic->txCqcReqFifo.push(cqcReq);
         //来自RDMAengine的scu（send completion unit）发送读取发送CQC的请求到此FIFO中
     } else if (cqcReq->chnl == CXT_CHNL_RX) {
         //来自RDMAengine的rcu（receive completion unit）发送读取接受CQC的请求到此FIFO中
         rnic->rxCqcReqFifo.push(cqcReq);
     } else {
         panic("[CqcModule]: cqcReq->chnl error! %d", cqcReq->chnl);
     }
 
     if (!cqcReqProcEvent.scheduled()) { /* Schedule cqcReqProc() */
         rnic->schedule(cqcReqProcEvent, curTick() + rnic->clockPeriod());
     }
 
     return true;
 }
 
 void 
 HanGuRnic::CqcModule::cqcRspProc() {
 
     HANGU_PRINT(CxtResc, " CqcModule.cqcRspProc!\n");
 
     assert(cqcCache.rrspFifo.size());
     CxtReqRspPtr cqcRsp = cqcCache.rrspFifo.front().second;
     //没有用cqcResc，因为数据已经复制到cqcCache.rrspFifo.front().second的.txCqcRsp
     //txCqcRsp是联合体内部指针
     cqcCache.rrspFifo.pop();
     uint8_t type = cqcRsp->type, chnl = cqcRsp->chnl;
     Event *e;
 
     /* Get event and push cqcRsp to relevant Fifo */
     if (type == CXT_RREQ_CQ && chnl == CXT_CHNL_TX) {
         cqcRsp->type = CXT_RRSP_CQ;
         e = &rnic->rdmaEngine.scuEvent;
 
         rnic->txCqcRspFifo.push(cqcRsp);
     } else if (type == CXT_RREQ_CQ && chnl == CXT_CHNL_RX) {
         cqcRsp->type = CXT_RRSP_CQ;
         e = &rnic->rdmaEngine.rcuEvent;
 
         rnic->rxCqcRspFifo.push(cqcRsp);
     } else {
         panic("[CqcModule]: cxtReq type error! type: %d, chnl %d", type, chnl);
     }
     
     /* schedule related module to return read rsp cqc */
     if (!e->scheduled()) {
         rnic->schedule(*e, curTick() + rnic->clockPeriod());
     }
 
     /* If there's still has elem to be 
      * processed, reschedule myself */
     if (cqcCache.rrspFifo.size()) {
         if (!cqcRspProcEvent.scheduled()) {/* Schedule myself */
             rnic->schedule(cqcRspProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(CxtResc, " CqcModule.cqcRspProc: out!\n");
 }
 
 /* cqc update function used in lambda expression */
 bool cqcReadUpdate(CqcResc &resc) {
 
     resc.offset += sizeof(CqDesc);
 
     if (resc.offset + sizeof(CqDesc) > (1 << resc.sizeLog)) {
         resc.offset = 0;
     }
     return true;
 }
 
 void
 HanGuRnic::CqcModule::cqcReqProc() {
 
     HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc!\n");
 
     uint8_t CHNL_NUM = 2;
     bool isEmpty[CHNL_NUM];
     //两个请求FIFO
     isEmpty[0] = rnic->txCqcReqFifo.empty();
     isEmpty[1] = rnic->rxCqcReqFifo.empty();
 
     CxtReqRspPtr cqcReq;
     for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
         if (isEmpty[chnlIdx] == false) {
             switch (chnlIdx) {
               case 0:
                 cqcReq = rnic->txCqcReqFifo.front();
                 rnic->txCqcReqFifo.pop();
                 assert(cqcReq->chnl == CXT_CHNL_TX);
 
                 HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: tx CQC read req posted!\n");
                 break;
               case 1:
                 cqcReq = rnic->rxCqcReqFifo.front();
                 rnic->rxCqcReqFifo.pop();
                 assert(cqcReq->chnl == CXT_CHNL_RX);
 
                 HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: rx CQC read req posted!\n");
                 break;
             }
 
             assert(cqcReq->type == CXT_RREQ_CQ);
 
             /* Read CQC from CQC Cache */
             //num是resource num，CQN或者QPN
             cqcCache.rescRead(cqcReq->num, &cqcRspProcEvent, cqcReq, cqcReq->txCqcRsp, [](CqcResc &resc) -> bool { return cqcReadUpdate(resc); });
             //cqcReq的txCqcRsp在联合体内，联合体内其余指针也能指向同一片区域
             /* Point to next chnl */
             ++chnlIdx;
             chnlIdx = chnlIdx % CHNL_NUM;
 
             /* Schedule myself again if there still has elem in fifo */
             if (rnic->txCqcReqFifo.size() || rnic->rxCqcReqFifo.size()) {
                 if (!cqcReqProcEvent.scheduled()) { /* schedule myself */
                     rnic->schedule(cqcReqProcEvent, curTick() + rnic->clockPeriod());
                 }
             }
             
             HANGU_PRINT(CxtResc, " CqcModule.cqcReqProc: out!\n");
 
             return;
         } else {
             /* Point to next chnl */
             ++chnlIdx;
             chnlIdx = chnlIdx % CHNL_NUM;
         }
     }
 }
 ///////////////////////////// HanGuRnic::CqcModule {end}//////////////////////////////
 
 ///////////////////////////// HanGuRnic::Cache {begin}//////////////////////////////
 template<class T>
 uint32_t 
 HanGuRnic::Cache<T>::replaceEntry() {
     //LRU替换策略，返回被踢出的条目num
     uint64_t min = seq_end;//seq_end应该是LRU时间戳的最小值
     uint32_t rescNum = cache.begin()->first;//小写cache是Cache内部的缓存实体，是一个哈希键值对，返回索引
     for (auto iter = cache.begin(); iter != cache.end(); ++iter) { // std::unordered_map<uint32_t, std::pair<T*, uint64_t>>::iterator
         if (min >= iter->second.second) {
             rescNum = iter->first;
         }
     }
     HANGU_PRINT(CxtResc, " HanGuRnic.Cache.replaceEntry: out! %d\n", rescNum);
     return rescNum;
 
     // uint32_t cnt = random_mt.random(0, (int)cache.size() - 1);
     
     // uint32_t rescNum = cache.begin()->first;
     // for (auto iter = cache.begin(); iter != cache.end(); ++iter, --cnt) {
     //     if (cnt == 0) {
     //         rescNum = iter->first;
     //     }
     // }
     // HANGU_PRINT(CxtResc, " HanGuRnic.Cache.replaceEntry: out!\n");
     // return rescNum;
 }
 //ok
 template<class T>
 bool 
 HanGuRnic::Cache<T>::lookupHit(uint32_t entryNum) {
     bool res = (cache.find(entryNum) != cache.end());
     if (res) { /* if hit update the state of the entry */
         cache[entryNum].second = seq_end++;//如果hit，则将此条目的seq_end加一
     }
     return res;
 }
 //ok
 template<class T>
 bool 
 HanGuRnic::Cache<T>::lookupFull(uint32_t entryNum) {
     return cache.size() == capacity;
 }
 //ok
 template<class T>
 bool 
 HanGuRnic::Cache<T>::readEntry(uint32_t entryNum, T* entry) {
     //必须要找到
     assert(cache.find(entryNum) != cache.end());
 
     memcpy(entry, cache[entryNum].first, sizeof(T));
     return true;
 }
 //ok
 //此函数接受一个lamda函数，这个lamda函数接受一个T&引用，update为指向此lamda函数的函数指针
 template<class T>
 bool 
 HanGuRnic::Cache<T>::updateEntry(uint32_t entryNum, const std::function<bool(T&)> &update) {
     assert(cache.find(entryNum) != cache.end());
     assert(update != nullptr);
 
     return update(*cache[entryNum].first);
 }
 //ok
 template<class T>
 bool 
 HanGuRnic::Cache<T>::writeEntry(uint32_t entryNum, T* entry) {
     assert(cache.find(entryNum) == cache.end()); /* could not find this entry in default */
 
     T *val = new T;
     memcpy(val, entry, sizeof(T));
     cache.emplace(entryNum, make_pair(val, seq_end++));
 
     // for (auto &item : cache) {
     //     uint32_t key = item.first;
     //     QpcResc *val  = (QpcResc *)item.second;
     //     HANGU_PRINT(CxtResc, " HanGuRnic.Cache.writeEntry: key %d srcQpn %d firstPsn %d\n\n", 
     //             key, val->srcQpn, val->sndPsn);
     // }
     return true;
 }
 //ok
 /* delete entry in cache */
 template<class T>
 T* 
 HanGuRnic::Cache<T>::deleteEntry(uint32_t entryNum) {
     assert(cache.find(entryNum) != cache.end());
     
     T *rtnResc = cache[entryNum].first;
     cache.erase(entryNum);
     assert(cache.size() == capacity - 1);
     return rtnResc;
 }
 ///////////////////////////// HanGuRnic::Cache {end}//////////////////////////////
 
 ///////////////////////////// HanGuRnic::PendingStruct {begin}//////////////////////////////
 //交换onlineIdx和offlineIdx
 void 
 HanGuRnic::PendingStruct::swapIdx() {
     uint8_t tmp = onlineIdx;
     onlineIdx   = offlineIdx;
     offlineIdx  = tmp;
     assert(onlineIdx != offlineIdx);
 }
 //pushElemProcEvent事件处理函数
 void 
 HanGuRnic::PendingStruct::pushElemProc() {
     
     assert(pushFifo.size());
     PendingElemPtr pElem = pushFifo.front();
     CxtReqRspPtr qpcReq = pElem->reqPkt;
     pushFifo.pop();
 
     HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: qpn %d idx %d chnl %d\n", pElem->qpn, pElem->idx, pElem->chnl);
     assert((pElem->qpn & QPN_MASK) <= QPN_NUM);//限制一个进程的QPN_NUM上限为512*3？
     assert((pElem->reqPkt->num & QPN_MASK) <= QPN_NUM);
     assert(qpcReq != nullptr);
 
     /* post pElem to pendingFifo */
     ++elemNum;
     if (pendingFifo[offlineIdx].size()) {
         pendingFifo[offlineIdx].push(pElem);
     } else {
         pendingFifo[onlineIdx].push(pElem);
     }
 
     /* schedule loadMem to post qpcReq dma pkt to dma engine */
     HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: has_dma %d\n", pElem->has_dma);
     if (pElem->has_dma) {
         //如果取出的元素是一个需要dma的，则执行loadMem
         rnic->qpcModule.loadMem(qpcReq);
     }
 
     /* If there are elem in fifo, schedule myself again */
     if (pushFifo.size()) {
         if (!pushElemProcEvent.scheduled()) {
             rnic->schedule(pushElemProcEvent, curTick() + rnic->clockPeriod());
         }
     }
     HANGU_PRINT(CxtResc, " PendingStruct.pushElemProc: out!\n");
 }
 
 /* push pElem to pushFifo */
 //将一个pendingelem放入pushfifo中调度pushElemProcEvent事件于一个时钟周期后执行
 bool 
 HanGuRnic::PendingStruct::push_elem(PendingElemPtr pElem) {
     pushFifo.push(pElem);
     if (!pushElemProcEvent.scheduled()) {
         rnic->schedule(pushElemProcEvent, curTick() + rnic->clockPeriod());
     }
     return true;
 }
 
 // return first elem in the fifo, the elem is not removed
 //从pendingFifo[onlineIdx]中读出首元素
 PendingElemPtr 
 HanGuRnic::PendingStruct::front_elem() {
     assert(pendingFifo[onlineIdx].size());
     /* read first pElem from pendingFifo */
     return pendingFifo[onlineIdx].front();
 }
 
 /* return first elem in the fifo, the elem is removed.
  * Note that if onlinePending is empty && offlinePending 
  * has elem, swap onlineIdx && offlineIdx */
 PendingElemPtr 
 HanGuRnic::PendingStruct::pop_elem() {
     
     assert(pendingFifo[onlineIdx].size() > 0);
     PendingElemPtr pElem = pendingFifo[onlineIdx].front();
     pendingFifo[onlineIdx].pop();
     --elemNum;
 
     /* if onlinePend empty, and offlinePend has elem, swap onlineIdx and offlineIdx */
     if (pendingFifo[onlineIdx].empty() && pendingFifo[offlineIdx].size()) {
         /* swap onlineIdx and offlineIdx */
         swapIdx();
     }
 
     HANGU_PRINT(CxtResc, " PendingStruct.pop_elem: exit, get_size() %d elemNum %d\n", get_size(), elemNum);
     
     return pElem;
 }
 
 /* read && pop one elem from offlinePending (to check the element) */
 //检查，从offline中取出一个元素并检查之
 PendingElemPtr 
 HanGuRnic::PendingStruct::get_elem_check() {
 
     assert(pendingFifo[offlineIdx].size() || pendingFifo[onlineIdx].size());
     
     if (pendingFifo[offlineIdx].empty()) {
         /* swap onlineIdx and offlineIdx */
         swapIdx();
     }
     PendingElemPtr pElem = pendingFifo[offlineIdx].front();
     pendingFifo[offlineIdx].pop();
     --elemNum;
 
     HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.get_elem_check: exit\n");
     return pElem;
 }
 
 /* pop the elem, and push to the online pendingFifo */
 void 
 HanGuRnic::PendingStruct::ignore_elem_check(PendingElemPtr pElem) {
     pendingFifo[onlineIdx].push(pElem);
     ++elemNum;
 
     HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.ignore_elem_check: exit\n");
 }
 
       /* pElem which is no_dma, hit in the cache. 
          * if it is the first, swap online and offline pendingFifo */
 void 
 HanGuRnic::PendingStruct::succ_elem_check() {
     if (pendingFifo[onlineIdx].size() == 0) {
         /* swap onlineIdx and offlineIdx */
         swapIdx();
     }
     HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.succ_elem_check: get_size %d, elemNum %d\n", 
             get_size(), elemNum);
 }
 
 /* call push_elem */
     /* pElem which is no_dma, not find entry in the cache.
          * and call push_elem */
 void 
 HanGuRnic::PendingStruct::push_elem_check(PendingElemPtr pElem) {
     if (pendingFifo[onlineIdx].size() == 0) {
         /* swap onlineIdx and offlineIdx */
         swapIdx();
     }
     push_elem(pElem);
     HANGU_PRINT(CxtResc, " QpcModule.PendingStruct.push_elem_check: exit\n");
 }
 ///////////////////////////// HanGuRnic::PendingStruct {end}//////////////////////////////
 //1.10 14：42第一次读完 
 //1.10 17：10第二次读完
 ///////////////////////////// HanGuRnic::QpcModule {begin}//////////////////////////////
 //应该是入口函数吧，其它模块向各自的请求fifo中放入自己的请求，并尝试调度让QPCmodue处理自己的请求
 //ok
 bool 
 HanGuRnic::QpcModule::postQpcReq(CxtReqRspPtr qpcReq) {
     //必须是请求的是qpc/sqc
     assert( (qpcReq->type == CXT_WREQ_QP) || 
             (qpcReq->type == CXT_RREQ_QP) || 
             (qpcReq->type == CXT_RREQ_SQ) ||
             (qpcReq->type == CXT_CREQ_QP));
     assert( (qpcReq->chnl == CXT_CHNL_TX) || 
             (qpcReq->chnl == CXT_CHNL_RX));
 
     if (qpcReq->type == CXT_CREQ_QP) {
         ccuQpcWreqFifo.push(qpcReq);
     } else if (qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_SQ) {
         txQpAddrRreqFifo.push(qpcReq);
     } else if (qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_QP) {
         txQpcRreqFifo.push(qpcReq);
     } else if (qpcReq->chnl == CXT_CHNL_RX && qpcReq->type == CXT_RREQ_QP) {
         rxQpcRreqFifo.push(qpcReq);
     } else {
         panic("[QpcModule.postQpcReq] invalid chnl %d or type %d", qpcReq->chnl, qpcReq->type);
     }
 
     if (!qpcReqProcEvent.scheduled()) { /* Schedule qpcReqProc() */
         rnic->schedule(qpcReqProcEvent, curTick() + rnic->clockPeriod());
     }
 
     return true;
 }
 //ok
 //处理发送时QPC请求结束时使用的cache更新策略，用来更新QPC
 //这一点十分重要，即处理一次更新一次QPC，QPC是实时数据，随着RNIC通信状态一直转换，要时刻更新
 bool qpcTxUpdate (QpcResc &resc, uint32_t sz) {
     if (resc.qpType == QP_TYPE_RC) {
         //如果此QP传输类型为RC，即RDMA可靠连接型传输
         resc.ackPsn += sz;//更新此QP最后一次ack的数据包编号信息
         resc.sndPsn += sz;//更新此QP下一次要发送的数据包编号信息
     }
     resc.sndWqeOffset += sz * sizeof(TxDesc);//更新此QP的sndWqeOffset
     //TxDesc是指QP的SQ中的SQE任务元素
     //每执行一次Tx任务都会访问QPCcache，而每次访问QPCcache都会增加sndWqeOffset，增加到SQ大小上限（4KB）时，下一次将此值重赋为0
     
     if (resc.sndWqeOffset + sizeof(TxDesc) > (1 << resc.sqSizeLog)) {
         //sqSizeLog：/* The size of SQ in log (It is now fixed at 4KB, which is 12) */
         resc.sndWqeOffset = 0; /* Same as in userspace drivers */
     }
     
     return true;
 }
 //ok
 //处理接受时QPC请求结束时使用的cache更新策略，用来更新QPC
 bool qpcRxUpdate (QpcResc &resc) {
     if (resc.qpType == QP_TYPE_RC) {
         resc.expPsn += 1;
         //期待的数据包序列号增加1
     }
     resc.rcvWqeOffset += sizeof(RxDesc);
     if (resc.rcvWqeOffset + sizeof(RxDesc) > (1 << resc.rqSizeLog)) {
         resc.rcvWqeOffset = 0; /* Same as in userspace drivers */
     }
     assert(resc.rqSizeLog == 12);
     return true;
 }
 //ok
 void 
 HanGuRnic::QpcModule::hitProc(uint8_t chnlNum, CxtReqRspPtr qpcReq) {
     qpcCache.readEntry(qpcReq->num, qpcReq->txQpcRsp);
     //读出来的数据已被复制到qpcReq->txQpcRsp中，txQpcRsp是联合体的成员
     assert(qpcReq->num == qpcReq->txQpcRsp->srcQpn);
    //确认返回的数据是请求想要的数据，不能给人家错的
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc.hitProc: qpn %d hit, chnlNum %d idx %d\n", 
             qpcReq->txQpcRsp->srcQpn, chnlNum, qpcReq->idx);
 
     /* Post rsp to related fifo, schedule related rsp receiving module */
     Event *e;
     if (chnlNum == 0) { // txQpAddrRspFifo
         txQpAddrRspFifo.push(qpcReq);
         e = &rnic->rdmaEngine.dfuEvent;
     } else if (chnlNum == 1) { // txQpcRspFifo
         /* update after read */
         uint32_t sz = qpcReq->sz;
         //lamda函数捕捉一个参数为sz
         //这个sz，官方注释：request number of the resources, used in qpc read (TX)
         qpcCache.updateEntry(qpcReq->num, [sz](QpcResc &qpc) { return qpcTxUpdate(qpc, sz); });
 
         txQpcRspFifo.push(qpcReq);
         e = &rnic->rdmaEngine.dpuEvent;
     } else if (chnlNum == 2) { // rxQpcRspFifo
         /* update after read */
         qpcCache.updateEntry(qpcReq->num, [](QpcResc &qpc) { return qpcRxUpdate(qpc); });
 
         rxQpcRspFifo.push(qpcReq);
         e = &rnic->rdmaEngine.rpuEvent;
     } else {
         panic("[QpcModule.readProc.hitProc] Unrecognized chnl %d or type %d", qpcReq->chnl, qpcReq->type);
     }
 
     if (!e->scheduled()) {
         rnic->schedule(*e, curTick() + rnic->clockPeriod());
     }
 }
 //读处理函数
 bool 
 HanGuRnic::QpcModule::readProc(uint8_t chnlNum, CxtReqRspPtr qpcReq) {
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc!\n");
 
     /* Lookup qpnHashMap to learn that if there's 
      * pending elem for this qpn in this channel. */
     if (qpnHashMap.find(qpcReq->num) != qpnHashMap.end()) {
         //寻找的QPC的QPN在qpnHashMap中找到，说明之前仍然有等待dma的相同QPN的QPC请求
         qpnHashMap[qpcReq->num]->reqCnt += 1;//更新此哈希表，待定数加1
 
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: related qpn is found in qpnHashMap qpn %d idx %d\n", 
                 qpcReq->num, qpcReq->idx);
 
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                 qpnHashMap.size(), pendStruct.get_size());
         //需要把这个req推入pending fifo中
         PendingElemPtr pElem =  make_shared<PendingElem>(qpcReq->idx, chnlNum, qpcReq, false); // new PendingElem(qpcReq->idx, chnlNum, qpcReq, false);
         //这个请求不需要dma，可能是只需等它的前序请求dma返回
         pendStruct.push_elem(pElem);
         //往pushfifp中推送此pendingElement
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                 qpnHashMap.size(), pendStruct.get_size());
 
         return true;
     }
     //如果没有在qpnHashMap发现相关性，则直接查看cache即可
     if (qpcCache.lookupHit(qpcReq->num)) { 
         //如果cache命中，则不许pend，将命中数据返回至对应的fifo中，并根据需要更新cache
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: cache hit, qpn %d\n", qpcReq->num);
 
         hitProc(chnlNum, qpcReq);//命中时后续处理函数
     } else { //cache miss的后续处理，要考虑pending fifo
 
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: cache miss, qpn %d, rtnCnt %d\n", qpcReq->num, rtnCnt);
 
         /* save req to pending fifo */
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                 qpnHashMap.size(), pendStruct.get_size());
         PendingElemPtr pElem = make_shared<PendingElem>(qpcReq->idx, chnlNum, qpcReq, true); 
         // new PendingElem(qpcReq->idx, chnlNum, qpcReq, true);
         //没在qpnHashMap中找到，则将带有has_dma标志的pendingelem推入pendstruct的pushfifo中
         //同时在qpnHashMap中创建一个entry
         pendStruct.push_elem(pElem);
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnMap.size() %d get_size() %d\n", 
                 qpnHashMap.size(), pendStruct.get_size());
 
         /* write an entry to qpnHashMap */
         QpnInfoPtr qpnInfo = make_shared<QpnInfo>(qpcReq->num); // new QpnInfo(qpcReq->num);
         qpnHashMap.emplace(qpcReq->num, qpnInfo);
         HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: qpnHashMap.size %d rtnCnt %d\n", qpnHashMap.size(), rtnCnt);
     }
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.readProc: out!\n");
     return true;
 }
 //QPC创建函数
 void 
 HanGuRnic::QpcModule::qpcCreate() {
     CxtReqRspPtr qpcReq = ccuQpcWreqFifo.front();
     ccuQpcWreqFifo.pop();
     //确认确实是一个QPC创建请求
     assert(qpcReq->type == CXT_CREQ_QP);
     //确认请求创建的qpn正确，txQpcReq即这个请求包含的需要创建的qpc数据
     assert(qpcReq->num == qpcReq->txQpcReq->srcQpn);
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcCreate: srcQpn %d sndBaseLkey %d\n", qpcReq->txQpcReq->srcQpn, qpcReq->txQpcReq->sndWqeBaseLkey);
     writeOne(qpcReq);
 
     /* delete useless qpc, cause writeEntry use memcpy 
      * to build cache entry. */
     delete qpcReq->txQpcReq;
 }
 
 //用于处理QPC的读请求，而非创建请求
 //每次使用qpcAccess，则在三个通道中轮询一次，最多发送三个readproc
 //由于QPCcache是非头部阻塞cache，所以一次发送多个readproc是被允许的
 void 
 HanGuRnic::QpcModule::qpcAccess() {
 
     uint8_t CHNL_NUM = 3;
     bool isEmpty[CHNL_NUM];
     isEmpty[0] = txQpAddrRreqFifo.empty();
     isEmpty[1] = txQpcRreqFifo.empty();
     isEmpty[2] = rxQpcRreqFifo.empty();
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: empty[0] %d empty[1] %d empty[2] %d\n", isEmpty[0], isEmpty[1], isEmpty[2]);
 
     CxtReqRspPtr qpcReq;
     for (uint8_t cnt = 0; cnt < CHNL_NUM; ++cnt) {
         if (isEmpty[this->chnlIdx] == false) {
             switch (this->chnlIdx) {
               case 0:
                 qpcReq = txQpAddrRreqFifo.front();
                 txQpAddrRreqFifo.pop();
                 assert(qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_SQ);
                 
                 break;
               case 1:
                 qpcReq = txQpcRreqFifo.front();
                 txQpcRreqFifo.pop();
                 assert(qpcReq->chnl == CXT_CHNL_TX && qpcReq->type == CXT_RREQ_QP);
                 
                 break;
               case 2:
                 qpcReq = rxQpcRreqFifo.front();
                 rxQpcRreqFifo.pop();
                 assert(qpcReq->chnl == CXT_CHNL_RX && qpcReq->type == CXT_RREQ_QP);
                 
                 break;
               default:
                 panic("[QpcModule.qpcReqProc.qpcAccess] chnlIdx error! %d", this->chnlIdx);
                 
                 break;
             }
 
             HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: qpn: %d, chnlIdx %d, idx %d rtnCnt %d\n", 
                     qpcReq->num, this->chnlIdx, qpcReq->idx, rtnCnt);
             assert((qpcReq->num & QPN_MASK) <= QPN_NUM);
 
             readProc(this->chnlIdx, qpcReq);
 
             /* Point to next chnl */
             ++this->chnlIdx;
             this->chnlIdx = this->chnlIdx % CHNL_NUM;
 
             HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc.qpcAccess: out!\n");
             return;
         } else {
             /* Point to next chnl */
             ++this->chnlIdx;
             this->chnlIdx = this->chnlIdx % CHNL_NUM;
         }
     }
 }
 //将一个QPC词条写入cache
 void 
 HanGuRnic::QpcModule::writeOne(CxtReqRspPtr qpcReq) {
     HANGU_PRINT(CxtResc, " QpcModule.writeOne!\n");
 
     HANGU_PRINT(CxtResc, " QpcModule.writeOne: srcQpn 0x%x, num %d, idx %d, chnl %d, sndBaseLkey %d\n", qpcReq->txQpcReq->srcQpn, qpcReq->num, qpcReq->idx, qpcReq->chnl, qpcReq->txQpcReq->sndWqeBaseLkey);
     assert(qpcReq->num == qpcReq->txQpcReq->srcQpn);
 
     if (qpcCache.lookupFull(qpcReq->num)) {
         //如果cache满了
         /* get replaced qpc */
         uint32_t wbQpn = qpcCache.replaceEntry();
         QpcResc* qpc = qpcCache.deleteEntry(wbQpn);
         //wbQpn是被踢出的qpn
         //qpc指向被踢出的QPC数据
         HANGU_PRINT(CxtResc, " QpcModule.writeOne: get replaced qpc 0x%x(%d)\n", wbQpn, (wbQpn & RESC_LIM_MASK));
         
         /* get related icm addr */
         uint64_t paddr = qpcIcm.num2phyAddr(wbQpn);
 
        //替换出来的QPC写回memory
         storeMem(paddr, qpc);
     }
 
     /* write qpc entry back to cache */
     //如果cache没有满，则写入cache中
     qpcCache.writeEntry(qpcReq->num, qpcReq->txQpcRsp);
     HANGU_PRINT(CxtResc, " QpcModule.writeOne: out!\n");
 }
 
 //用来将被踢出的QPC数据写回memory，通过dma
 void 
 HanGuRnic::QpcModule::storeMem(uint64_t paddr, QpcResc *qpc) {
     DmaReqPtr dmaReq = make_shared<DmaReq>(paddr, sizeof(QpcResc), 
             nullptr, (uint8_t *)qpc, 0); /* last param is useless here */
     dmaReq->reqType = 1; /* this is a write request */
     //qpcICM里的物理地址好像是直接的DMA物理地址而非PCIe地址，这一点似乎很重要的区别
     rnic->cacheDmaAccessFifo.push(dmaReq);
     if (!rnic->dmaEngine.dmaWriteEvent.scheduled()) {
         rnic->schedule(rnic->dmaEngine.dmaWriteEvent, curTick() + rnic->clockPeriod());
     }
 }
 //处理pendingfifo[online]中的has_dma的待定请求
 DmaReqPtr 
 HanGuRnic::QpcModule::loadMem(CxtReqRspPtr qpcReq) {
 
     HANGU_PRINT(CxtResc, " QpcModule.loadMem: Post qpn %d to dmaEngine, idx %d, pending size %d\n", 
             qpcReq->num, qpcReq->idx, pendStruct.get_size());
     
     PendingElemPtr pElem = pendStruct.front_elem();
     HANGU_PRINT(CxtResc, " QpcModule.loadMem: qpn %d chnl %d has_dma %d, idx %d\n", 
             pElem->qpn, pElem->chnl, pElem->has_dma, pElem->idx);
     assert((pElem->qpn & QPN_MASK) <= QPN_NUM);
 
     /* get qpc request icm addr, and post read request to ICM memory */
     //由ICM翻译资源索引为DMA物理地址，让dma去获取数据，返回时调用qpcRspProcEvent
     uint64_t paddr = qpcIcm.num2phyAddr(qpcReq->num);
     DmaReqPtr dmaReq = make_shared<DmaReq>(paddr, sizeof(QpcResc), 
             &qpcRspProcEvent, (uint8_t *)qpcReq->txQpcReq, 0); /* last param is useless here */
     rnic->cacheDmaAccessFifo.push(dmaReq);
     if (!rnic->dmaEngine.dmaReadEvent.scheduled()) {
         rnic->schedule(rnic->dmaEngine.dmaReadEvent, curTick() + rnic->clockPeriod());
     }
 
     return dmaReq;
 }
 // used to check no_dma pending element 
 uint8_t 
 HanGuRnic::QpcModule::checkNoDmaElem(PendingElemPtr pElem, uint8_t chnlNum, uint32_t qpn) {
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem!\n");
     QpnInfoPtr qInfo = qpnHashMap[qpn];
     assert(qInfo->reqCnt);//reqCnt不可能为0，放入pendingfifo中代表reqCnt至少为1
 
     /* check if qpn attached to this elem is in cache */
     if (qpcCache.lookupHit(qpn)) { /* cache hit */
         //cache-hit说明前序has_dma请求已将此qpn对应的qpc通过dma取来并writeone到了cache中
         HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem: qpcCache.lookupHit. qInfo->reqCnt %d\n", qInfo->reqCnt);
 
         /* update qpnHashMap, and delete invalid elem in qpnMap */
         qInfo->reqCnt -= 1;
         if (qInfo->reqCnt == 0) {
             --rtnCnt;
             qpnHashMap.erase(qpn);
         }
 
         /* return rsp to qpcRspFifo */
         hitProc(chnlNum, pElem->reqPkt);
 
         /* update pendingFifo */
         pendStruct.succ_elem_check();//在某些特殊条件下会swap index，需要日后研究一下
 
         return 0;
     } else if (qInfo->isReturned) { /* cache miss && accordingly qpc is returned */
         //可能返回后，由于cache替换策略问题导致返回的entry后续又被踢出
         HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc.checkNoDmaElem: cache miss && accordingly qpc is returned\n");
 
         /* delete isReturned && --rtnCnt */
         qInfo->reqRePosted();
         --rtnCnt;
         
         /* repost this request to pendingFifo */
         pElem->has_dma = 1; /* This request needs to post dma read request this time */
         pendStruct.push_elem_check(pElem);
 
         return 0;
     }
 
     return 1;
 }
 
 bool 
 HanGuRnic::QpcModule::isRspValidRun() {
     return (((rtnCnt != 0) && pendStruct.get_size()) || rnic->qpcDmaRdCplFifo.size());
 }
 //qpc从dma返回时调用的事件处理函数
 void 
 HanGuRnic::QpcModule::qpcRspProc() {
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc! rtnCnt %d qpnMap.size %d get_size() %d\n", 
             rtnCnt, qpnHashMap.size(), pendStruct.get_size());
     assert(rtnCnt <= qpnHashMap.size());
     assert(rtnCnt <= pendStruct.get_size());
     
     for (auto &item : qpnHashMap) {
         uint32_t   key = item.first;
         QpnInfoPtr val = item.second;
         HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: key %d qpn %d reqCnt %d\n\n", 
                 key, val->qpn, val->reqCnt);
     }
 
     if (rnic->qpcDmaRdCplFifo.size()) { /* processing dmaRsp pkt */
 
         HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: processing dmaRsp pkt!\n");
 
         PendingElemPtr pElem = pendStruct.front_elem();
         uint32_t qpn = pElem->reqPkt->num;
         uint8_t  chnlNum = pElem->chnl;
         QpnInfoPtr qInfo = qpnHashMap[qpn];
 
         if (pElem->has_dma) {//如果是has_dma的请求，则从qpcDmaRdCplFifo中弹出返回结果
             
             /* pop the dmaPkt */
             rnic->qpcDmaRdCplFifo.pop();
 
             /* update isReturned && rtnCnt */
             qInfo->firstReqReturned();//此qInfo词条中的isReturned位变为1
             ++rtnCnt;
             qInfo->reqCnt -= 1;
 
             /* write loaded qpc entry to qpc cache */
             writeOne(pElem->reqPkt);
 
             /* return rsp to qpcRspFifo */
             hitProc(chnlNum, pElem->reqPkt);
 
             HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: rtnCnt %d get_size() %d, qpnHashMap.size() %d, qInfo->reqCnt %d\n", 
                     rtnCnt, pendStruct.get_size(), qpnHashMap.size(), qInfo->reqCnt);
 
             /* delete invalid elem in qpnHashMap */
             if (qInfo->reqCnt == 0) {
                 --rtnCnt;
                 qpnHashMap.erase(qpn);
             }
 
             /* remove elem in pendingFifo */
             PendingElemPtr tmp = pendStruct.pop_elem();
         } else {
             //如果发现是no_dma，需要check一下，如果成功消除了顺序问题，则可从哈希表中消除。
             //如果因为前方因为cache替换导致获取的信息被踢出cache，则转换为dma类型pend，重新放回pushfifo中
             /* remove the elem in pendingFifo. No matter if we process it, it cannot be placed 
              * to the head of the pendingFifo again. */
             PendingElemPtr tmp = pendStruct.pop_elem();
 
             uint8_t rtn = checkNoDmaElem(pElem, chnlNum, qpn);
             if (rtn != 0) {
                 panic("[QpcModule.qpcRspProc] Error!");
             }
         }
     } else if (isRspValidRun()) {//若满足online内有数据，则在dma空闲的时钟周期轮询online，做一些检查工作
 
         if (pendStruct.get_pending_size()) { /* there's elem in pending fifo */
             HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: processing check pendingElem!\n");
 
             PendingElemPtr cpElem = pendStruct.get_elem_check();
             uint32_t qpn = cpElem->reqPkt->num;
             uint8_t chnlNum = cpElem->chnl;
             if (cpElem->has_dma) {
                 pendStruct.ignore_elem_check(cpElem);
             } else {
                 uint8_t rtn = checkNoDmaElem(cpElem, chnlNum, qpn);
                 if (rtn != 0) { /* no elem in cache && qpn hasn't returned, ignore it */
                     pendStruct.ignore_elem_check(cpElem);
                     HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: do not has dma, and check is ignored!\n");
                 }
             }
         } else { /* pendingfifo has not been parpared for processing */
             HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: pendingfifo has not been parpared for processing, maybe next cycle!\n");
         }
     }
 
     /* if there's under checked elem in pending fifo, 
      * schedule myself again */
     if (isRspValidRun()) {
         if (!qpcRspProcEvent.scheduled()) { /* schedule myself */
             rnic->schedule(qpcRspProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcRspProc: out! rtnCnt %d qpnMap.size %d get_size() %d\n", 
             rtnCnt, qpnHashMap.size(), pendStruct.get_size());
     assert(!(rtnCnt && (pendStruct.get_size() == 0)));
     assert(rtnCnt <= qpnHashMap.size());
     assert(rtnCnt <= pendStruct.get_size());
 }
 
 bool 
 HanGuRnic::QpcModule::isReqValidRun() {
     return (ccuQpcWreqFifo.size()   || 
             txQpAddrRreqFifo.size() || 
             txQpcRreqFifo.size()    || 
             rxQpcRreqFifo.size()      );
 }
 //处理QPC读写请求
 void
 HanGuRnic::QpcModule::qpcReqProc() {
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc!\n");
 
     if (ccuQpcWreqFifo.size()) {
         //如果ccuQpcWreqFifo有东西，处理QPC创建请求。创建优先级最高
         qpcCreate(); /* execute qpc entry create */
     } else {
         //否则处理QPC读请求
         qpcAccess(); /* execute qpc entry read || write */
     }
 
     HANGU_PRINT(CxtResc, " QpcModule.qpcReqProc: out!\n");
 
     /* Schedule myself again if there still has elem in fifo */
     //isReqValidRun()，如果四个请求fifo都空，则返回否，否则返回真
     if (isReqValidRun()) {
         if (!qpcReqProcEvent.scheduled()) { /* schedule myself */
             rnic->schedule(qpcReqProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 }
 ///////////////////////////// HanGuRnic::QpcModule {end}//////////////////////////////
 
 ///////////////////////////// HanGuRnic::DMA Engine {begin}//////////////////////////////
 //ok
 void 
 HanGuRnic::DmaEngine::dmaWriteCplProcessing() {
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaWriteCplProcessing! size %d\n", 
             dmaWrReq2RspFifo.front()->size);
     
     /* Pop related write request */
     DmaReqPtr dmaReq = dmaWrReq2RspFifo.front();
     dmaWrReq2RspFifo.pop();
 
     /* Schedule myself if there's item in fifo */
     if (dmaWrReq2RspFifo.size()) {
         rnic->schedule(dmaWriteCplEvent, dmaWrReq2RspFifo.front()->schd);
     }
 }
 
 //dmawriteevent处理函数
 void 
 HanGuRnic::DmaEngine::dmaWriteProcessing () {
 
     uint8_t CHNL_NUM = 3;
     bool isEmpty[CHNL_NUM];
     isEmpty[0] = rnic->cacheDmaAccessFifo.empty();
     isEmpty[1] = rnic->dataDmaWriteFifo.empty() ;
     isEmpty[2] = rnic->cqDmaWriteFifo.empty()   ;
 
     if (rnic->cacheDmaAccessFifo.size() && rnic->cacheDmaAccessFifo.front()->reqType == 0) { /* read request */
         /* shchedule dma read processing if this is a read request */
         if (!dmaReadEvent.scheduled()) {
             rnic->schedule(dmaReadEvent, curTick() + rnic->clockPeriod());
         }
 
         isEmpty[0] = true; /* Write Request. This also means empty */
     }
 
     if (isEmpty[0] & isEmpty[1] & isEmpty[2]) {
         return;
     }
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite! size0 %d, size1 %d, size2 %d\n", 
             rnic->cacheDmaAccessFifo.size(), rnic->dataDmaWriteFifo.size(), rnic->cqDmaWriteFifo.size());
 
     uint8_t cnt = 0;
     while (cnt < CHNL_NUM) {
         if (isEmpty[writeIdx] == false) {
             DmaReqPtr dmaReq;
             switch (writeIdx) {
               case 0 :
                 dmaReq = rnic->cacheDmaAccessFifo.front();
                 rnic->cacheDmaAccessFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is cacheDmaAccessFifo! addr 0x%lx\n", (uint64_t)(dmaReq->data));
                 break;
               case 1 :
                 dmaReq = rnic->dataDmaWriteFifo.front();
                 rnic->dataDmaWriteFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is dataDmaWriteFifo!\n");
                 break;
               case 2 :
                 dmaReq = rnic->cqDmaWriteFifo.front();
                 rnic->cqDmaWriteFifo.pop();
                 
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: Is cqDmaWriteFifo!\n");
                 
                 break;
             }
 
             // if (dmaReq->size == 40) {
             //     for (int i = 0; i < dmaReq->size; ++i) {
             //         HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: data[%d] is 0x%x\n", i, (dmaReq->data)[i]);
             //     }
             // }
             
             // unit: ps
             Tick bwDelay = (dmaReq->size + 32) * rnic->pciBandwidth;
             Tick delay = rnic->dmaWriteDelay + bwDelay;
             
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: dmaReq->addr 0x%x, dmaReq->size %d, delay %d, bwDelay %d!\n", 
             dmaReq->addr, dmaReq->size, delay, bwDelay);
 
             /* Send dma req to dma channel
              * this event is used to call rnic->dmaWrite() */
             dmaWReqFifo.push(dmaReq);
             if (!dmaChnlProcEvent.scheduled()) {
                 rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
             }
             
             /* Schedule DMA Write completion event */
             dmaReq->schd = curTick() + delay;
             dmaWrReq2RspFifo.push(dmaReq);
             if (!dmaWriteCplEvent.scheduled()) {
                 rnic->schedule(dmaWriteCplEvent, dmaReq->schd);
             }
             
             /* Point to next chnl */
             ++writeIdx;
             writeIdx = writeIdx % CHNL_NUM;
             
             // bwDelay = (bwDelay > rnic->clockPeriod()) ? bwDelay : rnic->clockPeriod();
             if (dmaWriteEvent.scheduled()) {
                 rnic->reschedule(dmaWriteEvent, curTick() + bwDelay);
             } else { // still schedule incase in time interval
                      // [curTick(), curTick() + rnic->dmaWriteDelay + bwDelay] , 
                      // one or more channel(s) schedule dmaWriteEvent
                 rnic->schedule(dmaWriteEvent, curTick() + bwDelay);
             }
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaWrite: out!\n");
             return;
         } else {
             ++cnt;
 
             ++writeIdx;
             writeIdx = writeIdx % CHNL_NUM;
         }
     }
 }
 
 //dma读结束处理
 void 
 HanGuRnic::DmaEngine::dmaReadCplProcessing() {
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing! cplSize %d\n", 
             dmaRdReq2RspFifo.front()->size);
 
     /* post related cpl pkt to related fifo */
     DmaReqPtr dmaReq = dmaRdReq2RspFifo.front();
     dmaRdReq2RspFifo.pop();
     dmaReq->rdVld = 1;//标记为1意思是已获取DMA数据
     Event *e = &rnic->qpcModule.qpcRspProcEvent;
     //如果是QPC，则把返回的数据放入qpcDmaRdCplFifo中
     if (dmaReq->event == e) { /* qpc dma read cpl pkt */
         HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: push to rnic->qpcModule.qpcRspProcEvent!\n");
         rnic->qpcDmaRdCplFifo.push(dmaReq);
 
         if (dmaReq->size == 256) {
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: 0x%lx!\n", uintptr_t(dmaReq->data));
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: qpn %d dqpn %d!\n", ((QpcResc *)dmaReq->data)->srcQpn, ((QpcResc *)dmaReq->data)->destQpn);
         }
     }
 
     /* Schedule related completion event */
     if (!(dmaReq->event)->scheduled()) {
         rnic->schedule(*(dmaReq->event), curTick() + rnic->clockPeriod());
     }
 
     /* Schedule myself if there's item in fifo */
     if (dmaRdReq2RspFifo.size()) {
         rnic->schedule(dmaReadCplEvent, dmaRdReq2RspFifo.front()->schd);
     }
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaReadCplProcessing: out!\n");
 }
 //读处理入口
 void 
 HanGuRnic::DmaEngine::dmaReadProcessing () {
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead! \n");
     
     uint8_t CHNL_NUM = 4;
     bool isEmpty[CHNL_NUM];
     isEmpty[0] = rnic->cacheDmaAccessFifo.empty();
     isEmpty[1] = rnic->descDmaReadFifo.empty() ;
     isEmpty[2] = rnic->dataDmaReadFifo.empty() ;
     isEmpty[3] = rnic->ccuDmaReadFifo.empty()  ;
 
     /* If there has write request, schedule dma Write Proc */
     if (rnic->cacheDmaAccessFifo.size() && rnic->cacheDmaAccessFifo.front()->reqType == 1) { /* write request */
         /* shchedule dma write processing if this is a write request */
         if (!dmaWriteEvent.scheduled()) {
             rnic->schedule(dmaWriteEvent, curTick() + rnic->clockPeriod());
         }
 
         isEmpty[0] = true; /* Write Request. This also means empty */
         //这里不懂为什么等于true，可能是防止处理两次cacheDmaAccessFifo中的请求
     }
     
     if (isEmpty[0] & isEmpty[1] & isEmpty[2] & isEmpty[3]) {
         return;
     }
 
     HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: in! \n");
 
     uint8_t cnt = 0;
     while (cnt < CHNL_NUM) {
         if (isEmpty[readIdx] == false) {
             DmaReqPtr dmaReq;
             switch (readIdx) {
               case 0 :
                 dmaReq = rnic->cacheDmaAccessFifo.front();
                 rnic->cacheDmaAccessFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is cacheDmaAccessFifo!\n");
                 break;
               case 1 :
                 dmaReq = rnic->descDmaReadFifo.front();
                 rnic->descDmaReadFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is descDmaReadFifo!\n");
                 break;
               case 2 :
                 dmaReq = rnic->dataDmaReadFifo.front();
                 rnic->dataDmaReadFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is dataDmaReadFifo!\n");
                 break;
               case 3 :
                 dmaReq = rnic->ccuDmaReadFifo.front();
                 rnic->ccuDmaReadFifo.pop();
                 HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: Is ccuDmaReadFifo!\n");
                 break;
             }
             
             // unit: ps
             //ps在gem5默认一个tick
             Tick bwDelay = (dmaReq->size + 32) * rnic->pciBandwidth;
             Tick delay = rnic->dmaReadDelay + bwDelay;
             
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: dmaReq->addr 0x%x, dmaReq->size %d, delay %d, bwDelay %d!\n", 
             dmaReq->addr, dmaReq->size, delay, bwDelay);
 
             /* Send dma req to dma channel, 
              * this event is used to call rnic->dmaRead() */
             dmaRReqFifo.push(dmaReq);
             if (!dmaChnlProcEvent.scheduled()) {
                 rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
             }
             
             /* Schedule DMA read completion event */
             dmaReq->schd = curTick() + delay;
             dmaRdReq2RspFifo.push(dmaReq);
             if (!dmaReadCplEvent.scheduled()) {
                 rnic->schedule(dmaReadCplEvent, dmaReq->schd);
             }
 
 
             /* Point to next chnl */
             ++readIdx;
             readIdx = readIdx % CHNL_NUM;
 
             /* Reschedule the dma read event. delay is (byte count * bandwidth) */
             if (dmaReadEvent.scheduled()) {
                 rnic->reschedule(dmaReadEvent, curTick() + bwDelay);
                 //这个应该是修改schedule的时间，其余模块都是一个时钟周期后schedule了dma模块
                 //但是dma模块无法处理这么快的请求，所以修改为每过bwdelay才会处理
             } else { // still schedule incase in time interval
                      // [curTick(), curTick() + rnic->dmaReadDelay * dmaReq->size] , 
                      // one or more channel(s) schedule dmaReadEvent
                 rnic->schedule(dmaReadEvent, curTick() + bwDelay);
 
                 //每过一个bwdelay处理一个dma
             }
             
             HANGU_PRINT(DmaEngine, " DMAEngine.dmaRead: out! \n");
             return;
         } else {
             ++cnt;
             ++readIdx;
             readIdx = readIdx % CHNL_NUM;
         }
     }
 }
 //处理chnl通道的
 //写优先级更高
 void 
 HanGuRnic::DmaEngine::dmaChnlProc () {
     if (dmaWReqFifo.empty() && dmaRReqFifo.empty()) {
         return ;
     }
 
     /* dma write has the higher priority, cause it is the duty of 
      * app logic to handle the write-after-read error. DMA channel 
      * only needs to avoid read-after-write error (when accessing 
      * the same address) */
     //RAW：Read After Write，假设指令j是在指令i后面执行的指令，RAW表示指令i将数据写入寄存器后，指令j才能从这个寄存器读取数据。如果指令j在指令i写入寄存器前尝试读出该寄存器的内容，将得到不正确的数据。
    // WAR：Write After Read，假设指令j是在指令i后面执行的指令，WAR表示指令i读出数据后，指令j才能写这个寄存器。如果指令j在指令i读出数据前就写该寄存器，将使得指令i读出的数据不正确
 
     DmaReqPtr dmaReq;
     if (dmaWReqFifo.size()) { 
         
         dmaReq = dmaWReqFifo.front();
         dmaWReqFifo.pop();
         rnic->dmaWrite(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
     } else if (dmaRReqFifo.size()) {
 
         dmaReq = dmaRReqFifo.front();
         dmaRReqFifo.pop();
         rnic->dmaRead(dmaReq->addr, dmaReq->size, nullptr, dmaReq->data);
     }
     
     /* schedule myself to post the dma req to the channel */
     if (dmaWReqFifo.size() || dmaRReqFifo.size()) {
         if (!dmaChnlProcEvent.scheduled()) {
             rnic->schedule(dmaChnlProcEvent, curTick() + rnic->clockPeriod());
         }
     }
 }
 ///////////////////////////// HanGuRnic::DMA Engine {end}//////////////////////////////
 
 
 ///////////////////////////// Ethernet Link Interaction {begin}//////////////////////////////
    /* Ethernet callback */
    // When TX done，仅仅打印调试信息
 void
 HanGuRnic::ethTxDone() {
 
     DPRINTF(HanGuRnic, "Enter ethTxDone!\n");
 }
 //mac地址6个字节，48位，所以宏ETH_ADDR_LEN为6
 //比较本设备的源mac与目标mac是否相等
 bool
 HanGuRnic::isMacEqual(uint8_t *devSrcMac, uint8_t *pktDstMac) {
     for (int i = 0; i < ETH_ADDR_LEN; ++i) {
         if (devSrcMac[i] != pktDstMac[i]) {
             return false;
         }
     }
     return true;
 }
 
 bool
 HanGuRnic::ethRxDelay(EthPacketPtr pkt) {
 
     HANGU_PRINT(HanGuRnic, " ethRxDelay!\n");
     
     /* dest addr is not local, then abandon it */
     //macAddr是本地RNIC的MAC地址，如果这个包的目标mac不是自己，说明这个包不是给自己的，直接丢弃
     if (isMacEqual(macAddr, pkt->data) == false) {
         return true;
     }
 
     /* Update statistic */
     //这两个是需要统计的数据
     rxBytes += pkt->length;
     rxPackets++;
 
     /* post rx pkt to ethRxPktProc */
     //       LinkDelay   /* related to link delay processing */
     Tick sched = curTick() + LinkDelay;
     ethRxDelayFifo.emplace(pkt, sched);
     if (!ethRxPktProcEvent.scheduled()) {
         schedule(ethRxPktProcEvent, sched);
     }
 
     HANGU_PRINT(HanGuRnic, " ethRxDelay: out!\n");
 
     return true;
 }
 //ethRxPktProcEvent对应的处理函数，会打印很多调试信息，好像主要是打印调试消息用的
 void
 HanGuRnic::ethRxPktProc() {
 
     HANGU_PRINT(HanGuRnic, " ethRxPktProc!\n");
     
     //从ethRxDelayFifo中弹出首元素
     EthPacketPtr pkt = ethRxDelayFifo.front().first;
     Tick sched = ethRxDelayFifo.front().second;
     ethRxDelayFifo.pop();
 
     /* Only used for debugging */
     //BTH(Base Transport Header)，是对消息的传输层描述，包括OpCode, Destination QPN, 发送方PSN等字段
     BTH *bth = (BTH *)(pkt->data + ETH_ADDR_LEN * 2);//怎么获取的？没看懂
     uint8_t type = (bth->op_destQpn >> 24) & 0x1f;//transtype
     uint8_t srv  = bth->op_destQpn >> 29;//servetype，有RC/UD等
     if (srv == QP_TYPE_RC) { //如果是可靠连接
         if (type == PKT_TRANS_SEND_ONLY) {
             HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, SEND_ONLY RC, data: %s.\n", 
                     (char *)(pkt->data + 8));//打印的应该是去掉以太网头后的数据
         } else if (type == PKT_TRANS_RWRITE_ONLY) {
             RETH *reth = (RETH *)(pkt->data + PKT_BTH_SZ + ETH_ADDR_LEN * 2);
             HANGU_PRINT(HanGuRnic, " ethRxPktProc:"
                     " Receiving packet from wire, RDMA Write data: %s, len %d, raddr 0x%x, rkey 0x%x op_destQpn %d\n", 
                     (char *)(pkt->data + sizeof(BTH) + sizeof(RETH) + ETH_ADDR_LEN * 2), reth->len, reth->rVaddr_l, reth->rKey, ((BTH *)pkt->data)->op_destQpn);
         } else if (type == PKT_TRANS_RREAD_ONLY) {
             
         } else if (type == PKT_TRANS_ACK) {//如果不是普通包，而是一个ACK包
             HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, Trans ACK needAck_psn: 0x%x\n", ((BTH *)pkt->data)->needAck_psn);
         }
     } else if (srv == QP_TYPE_UD) {
         if (type == PKT_TRANS_SEND_ONLY) {
             
             uint8_t *u8_tmp = (pkt->data + 16 + ETH_ADDR_LEN * 2);
             HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, SEND UD data\n");
             HANGU_PRINT(HanGuRnic, " ethRxPktProc: data: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x.\n", 
                     u8_tmp[0], u8_tmp[1], u8_tmp[2], u8_tmp[3], u8_tmp[4], u8_tmp[5], u8_tmp[6], u8_tmp[7]);
             HANGU_PRINT(HanGuRnic, " ethRxPktProc: data: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x.\n", 
                     u8_tmp[8], u8_tmp[9], u8_tmp[10], u8_tmp[11], u8_tmp[12], u8_tmp[13], u8_tmp[14], u8_tmp[15]);
                     //打印16个字节的数据
         }
     }
     HANGU_PRINT(HanGuRnic, " ethRxPktProc: Receiving packet from wire, trans_type: 0x%x, srv: 0x%x.\n", type, srv);
     /* Schedule RAU for pkt receiving */
     rxFifo.push(pkt);
     if (!rdmaEngine.rauEvent.scheduled()) {
         schedule(rdmaEngine.rauEvent, curTick() + clockPeriod());
     }
     /* Schedule myself if there is element in ethRxDelayFifo */
     if (ethRxDelayFifo.size()) {
         sched = ethRxDelayFifo.front().second;
         if (!ethRxPktProcEvent.scheduled()) {
             schedule(ethRxPktProcEvent, sched);
         }
     }
 
     HANGU_PRINT(HanGuRnic, " ethRxPktProc: out!\n");
 }
 
 ///////////////////////////// Ethernet Link Interaction {end}//////////////////////////////
 
 
 DrainState
 HanGuRnic::drain() {
     
     DPRINTF(HanGuRnic, "HanGuRnic not drained\n");
     return DrainState::Draining;
 }
 
 void
 HanGuRnic::drainResume() {
     Drainable::drainResume();
 
     DPRINTF(HanGuRnic, "resuming from drain");
 }
 
 void
 HanGuRnic::serialize(CheckpointOut &cp) const {
     PciDevice::serialize(cp);
 
     regs.serialize(cp);
 
     DPRINTF(HanGuRnic, "Get into HanGuRnic serialize.\n");
 }
 
 void
 HanGuRnic::unserialize(CheckpointIn &cp) {
     PciDevice::unserialize(cp);
 
     regs.unserialize(cp);
 
     DPRINTF(HanGuRnic, "Get into HanGuRnic unserialize.\n");
 }
 
 HanGuRnic *
 HanGuRnicParams::create() {
     return new HanGuRnic(this);
 }