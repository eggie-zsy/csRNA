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
 *      <kangning18z@ict.ac.cn>
 *  Date : 2021.07.08
 */

#include "dev/rdma/hangu_driver.hh"

//内核态驱动，用于实现与模拟的gem5硬件对象通信
//2024.12第一次读完，读懂30%
//2.26第二次读完，读懂60%

HanGuDriver::HanGuDriver(Params *p)
  : EmulatedDriver(p), device(p->device) {
    // HANGU_PRINT(HanGuDriver, "HanGu RNIC driver.\n");
}

/**
 * Create an FD entry for the KFD inside of the owning process.
 */
//这段代码是一个典型的设备驱动程序的 open 函数实现，用于在操作系统中打开一个设备文件并为其分配文件描述符（FD）
int
HanGuDriver::open(ThreadContext *tc, int mode, int flags) {
    
    HANGU_PRINT(HanGuDriver, "open : %s.\n", filename);//把打开的文件名字打印出来

    auto process = tc->getProcessPtr(); //获取进程指针
    auto device_fd_entry = std::make_shared<DeviceFDEntry>(this, filename);
    int tgt_fd = process->fds->allocFD(device_fd_entry);
    cpu_id = tc->contextId();

    // Configure PCI config space
    configDevice();

    return tgt_fd;
}

void
HanGuDriver::configDevice() {
    
}

/**
 * Currently, mmap() will simply setup a mapping for the associated
 * rnic's send doorbells.
 */
Addr
HanGuDriver::mmap(ThreadContext *tc, Addr start, uint64_t length, int prot,
                int tgt_flags, int tgt_fd, int offset) {
    HANGU_PRINT(HanGuDriver, " rnic hangu_rnic doorbell mmap (start: %p, length: 0x%x,"
            "offset: 0x%x) cxt_id %d\n", start, length, offset, tc->contextId());

    auto process = tc->getProcessPtr();
    auto mem_state = process->memState;

    // Extend global mmap region if necessary.
    if (start == 0) {
        // Assume mmap grows down, as in x86 Linux.
        start = mem_state->getMmapEnd() - length;
        mem_state->setMmapEnd(start);
    }
    
    /**
     * Now map this virtual address to our PIO doorbell interface
     * in the page tables (non-cacheable).
     */
    AddrRangeList addrList = device->getAddrRanges();
    HANGU_PRINT(HanGuDriver, " addrList size %d\n", addrList.size());
    AddrRange baseAddrBar0 = addrList.front();
    HANGU_PRINT(HanGuDriver, " baseAddrBar0.start 0x%x, baseAddrBar0.size() 0x%x\n", baseAddrBar0.start(), baseAddrBar0.size());
    process->pTable->map(start, baseAddrBar0.start(), 64, false); // Actually, 36 is enough
    HANGU_PRINT(HanGuDriver, " rnic hangu_rnic doorbell mapped to 0x%x\n", start);
    hcrAddr = start;
    return start + 24;//返回的start可能是用户空间的虚拟地址，我不知道
}



int
HanGuDriver::ioctl(ThreadContext *tc, unsigned req, Addr ioc_buf) {
    auto &virt_proxy = tc->getVirtProxy();
    //传入的ioc_buf是用户空间的虚拟地址
    if (HGKFD_IOC_GET_TIME == req) {//获取当前时间
        HANGU_PRINT(HanGuDriver, " ioctl: HGKFD_IOC_GET_TIME %ld\n", curTick());

        /* Get && copy current time */
        TypedBufferArg<kfd_ioctl_get_time_args> args(ioc_buf);
        //用ioc_buf初始化arg，在模拟空间new一个bufptr，curTick的值先存储在模拟空间bufptr，再复制到用户空间ioc_buf
        args->cur_time = curTick();
        args.copyOut(virt_proxy);

        return 0;
    } else if (checkHcr(virt_proxy)) {//后面有定义
        HANGU_PRINT(HanGuDriver, " `` bit is still high! Try again later.\n");
        return -1;
    }
    
    Addr pAddr;
    auto process = tc->getProcessPtr();
    process->pTable->translate(ioc_buf, pAddr);//iocbuf应该是个虚拟地址，根据当前用户进程的模拟页表，得到在模拟硬件中的物理地址

    switch (req) {
      case HGKFD_IOC_INIT_DEV: // Input
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_INIT_DEV.\n");
            
            TypedBufferArg<kfd_ioctl_init_dev_args> args(ioc_buf);
            // kfd_ioctl_init_dev_args结构体内有以下内容
            // uint8_t qpc_num_log;
            // uint8_t cqc_num_log;
            // uint8_t mpt_num_log;
            // uint8_t mtt_num_log;
            args.copyIn(virt_proxy);

            initMailbox(process);   //初始化mailbox,完成其页表映射
            HANGU_PRINT(HanGuDriver, " HGKFD_IOC_INIT_DEV mailbox initialized\n");
            
            // We don't use input parameter here
            //后面有写
            initIcm(virt_proxy, RESC_LEN_LOG, RESC_LEN_LOG, RESC_LEN_LOG, RESC_LEN_LOG);
            //支持16个进程，每个进程每个资源最多2^16个资源条目
            //RESC_LEN_LOG=20
        }
        break;
      case HGKFD_IOC_ALLOC_MTT: // Input Output
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_ALLOC_MTT.\n");
            
            TypedBufferArg<kfd_ioctl_init_mtt_args> args(ioc_buf);
            args.copyIn(virt_proxy);
            //模拟空间中new了一个sizeof（kfd_ioctl_init_mtt_args）的buffer，
            //将iocbuffer（用户空间）开始的sizeof（kfd_ioctl_init_mtt_args）个字节复制过去（复制到模拟空间中，args->指向模拟空间）
            //args经过了ioc_buf的初始化
            allocMtt(process, args);
            HANGU_PRINT(HanGuDriver, " HGKFD_IOC_ALLOC_MTT mtt allocated\n");
            
            uint32_t last_mtt_index = (args->mtt_index + args->batch_size - 1);
            if (!isIcmMapped(mttMeta, last_mtt_index)) { /* last mtt index in this allocation */
               //如果刚才最后一个mtt_index所在虚拟页未在ICM映射表中映射，即上次分配的16页还足以容纳这次batch_size个entry
               //则从mtt_index(注意不是last_mtt_index)所在页开始连续分配16页，
               //即使是最大批处理（MR是512，一个entry8字节），也不可能不覆盖last_mtt_index所在页
                HANGU_PRINT(HanGuDriver, " HGKFD_IOC_ALLOC_MTT mtt not mapped\n");
                Addr icmVPage = allocIcm (process, mttMeta, args->mtt_index);  
               
                writeIcm(virt_proxy, HanGuRnicDef::ICMTYPE_MTT, mttMeta, icmVPage);
                HANGU_PRINT(HanGuDriver, " HGKFD_IOC_ALLOC_MTT mtt ICM mapping is written\n");
            }

            args.copyOut(virt_proxy);//获得用户态的数据，刚才操作的arg都操作的是模拟空间bufptr指向的内存，所以复制过来
        }
        break;
      case HGKFD_IOC_WRITE_MTT: // Input
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_WRITE_MTT.\n");
            
            TypedBufferArg<kfd_ioctl_init_mtt_args> args(ioc_buf);
            args.copyIn(virt_proxy);

            writeMtt(virt_proxy, args);//将args上的关于mtt转换地址的信息写入gem5模拟空间
        }
        break;
      case HGKFD_IOC_ALLOC_MPT: // Output
        {   
            TypedBufferArg<kfd_ioctl_alloc_mpt_args> args(ioc_buf);
            args.copyIn(virt_proxy);
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_ALLOC_MPT. batch_size %d\n", args->batch_size);

            allocMpt(process, args);

            HANGU_PRINT(HanGuDriver, " get into ioctl HGKFD_IOC_ALLOC_MPT: mpt_start_index: %d\n", args->mpt_index);
            if (!isIcmMapped(mptMeta, args->mpt_index + args->batch_size - 1)) {
                Addr icmVPage = allocIcm(process, mptMeta, args->mpt_index);
                writeIcm(virt_proxy, HanGuRnicDef::ICMTYPE_MPT, mptMeta, icmVPage);
            }

            args.copyOut(virt_proxy);
        }
        break;
      case HGKFD_IOC_WRITE_MPT: // Input
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_WRITE_MPT.\n");

            TypedBufferArg<kfd_ioctl_write_mpt_args> args(ioc_buf);
            args.copyIn(virt_proxy);

            writeMpt(virt_proxy, args);//将args上的关于MPT的信息写入gem5模拟空间
        }
        break;
      case HGKFD_IOC_ALLOC_CQ: // Output
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_ALLOC_CQ.\n");

            TypedBufferArg<kfd_ioctl_alloc_cq_args> args(ioc_buf);

            args.copyIn(virt_proxy);//这句话原代码未加

            allocCqc(args);

            if (!isIcmMapped(cqcMeta, args->cq_num)) {
                Addr icmVPage = allocIcm (process, cqcMeta, args->cq_num);
                writeIcm(virt_proxy, HanGuRnicDef::ICMTYPE_CQC, cqcMeta, icmVPage);
            }

            args.copyOut(virt_proxy);
        }
        break;
      case HGKFD_IOC_WRITE_CQC: // Input
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_WRITE_CQC.\n");

            TypedBufferArg<kfd_ioctl_write_cqc_args> args(ioc_buf);
            args.copyIn(virt_proxy);

            writeCqc(virt_proxy, args);
        }
        break;
      case HGKFD_IOC_ALLOC_QP: // Output
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_ALLOC_QP.\n");

            TypedBufferArg<kfd_ioctl_alloc_qp_args> args(ioc_buf);
            args.copyIn(virt_proxy);

            allocQpc(args);

            if (!isIcmMapped(qpcMeta, args->qp_num + args->batch_size - 1)) {
                Addr icmVPage = allocIcm (process, qpcMeta, args->qp_num);
                writeIcm(virt_proxy, HanGuRnicDef::ICMTYPE_QPC, qpcMeta, icmVPage);
            }

            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_ALLOC_QP, qp_num: 0x%x(%d), batch_size %d\n", args->qp_num, RESC_LIM_MASK&args->qp_num, args->batch_size);

            args.copyOut(virt_proxy);
        }
        break;
      case HGKFD_IOC_WRITE_QPC: // Input
        {   
            HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_WRITE_QPC\n");

            TypedBufferArg<kfd_ioctl_write_qpc_args> args(ioc_buf);
            args.copyIn(virt_proxy);

            writeQpc(virt_proxy, args);
        }
        break;
      case HGKFD_IOC_CHECK_GO: 
        {   
            /* We don't check `go` bit here, cause it 
             * has been checked at the beginning of ioctl. */
            // HANGU_PRINT(HanGuDriver, " ioctl : HGKFD_IOC_CHECK_GO, `GO` is cleared.\n");
        }
        break;
      default:
        {
            fatal("%s: bad ioctl %d\n", req);
        }
        break;
    }
    return 0;
}
/* -------------------------- HCR {begin} ------------------------ */
//查询硬件doorbell的GO位
uint8_t 
HanGuDriver::checkHcr(PortProxy& portProxy) {

    uint32_t goOp;
    HANGU_PRINT(HanGuDriver, " Start read `GO`.\n");
    //portProxy是一个代理
    portProxy.readBlob(hcrAddr + (Addr)&(((HanGuRnicDef::Hcr*)0)->goOpcode), &goOp, sizeof(goOp));

    
    if ((goOp >> 31) == 1) {
    // HANGU_PRINT(HanGuDriver, " `GO` is still high\n");
        return 1;
    }
    // HANGU_PRINT(HanGuDriver, " `GO` is cleared.\n");
    return 0;
}
//写HCR 
void 
HanGuDriver::postHcr(PortProxy& portProxy, uint64_t inParam, 
        uint32_t inMod, uint64_t outParam, uint8_t opcode) {
    
    HanGuRnicDef::Hcr hcr;
//     hcr结构体
//     struct Hcr {    //硬件控制寄存器（HCR, Hardware Control Register）
//     uint32_t inParam_l;  //低 32 位和高 32 位输入参数。
//     uint32_t inParam_h;
//     uint32_t inMod;     //输入模式，可能控制命令的执行方式
//     uint32_t outParam_l; //低 32 位和高 32 位输出参数，存储命令执行结果。
//     uint32_t outParam_h;
//     uint32_t goOpcode;   //操作码
//     };
    HANGU_PRINT(HanGuDriver, " Start Write hcr\n");
    hcr.inParam_l  = inParam & 0xffffffff;
    hcr.inParam_h  = inParam >> 32;
    hcr.inMod      = inMod;
    hcr.outParam_l = outParam & 0xffffffff;
    hcr.outParam_h = outParam >> 32;
    hcr.goOpcode   = (1 << 31) | opcode;
    // HANGU_PRINT(HanGuDriver, " inParam_l: 0x%x\n", hcr.inParam_l);
    // HANGU_PRINT(HanGuDriver, " inParam_h: 0x%x\n", hcr.inParam_h);
    // HANGU_PRINT(HanGuDriver, " inMod: 0x%x\n", hcr.inMod);
    // HANGU_PRINT(HanGuDriver, " outParam_l: 0x%x\n", hcr.outParam_l);
    // HANGU_PRINT(HanGuDriver, " outParam_h: 0x%x\n", hcr.outParam_h);
    // HANGU_PRINT(HanGuDriver, " goOpcode: 0x%x\n", hcr.goOpcode);
    portProxy.writeBlob(hcrAddr, &hcr, sizeof(hcr));
}

/* -------------------------- HCR {end} ------------------------ */


/* -------------------------- ICM {begin} ------------------------ */

//内核态驱动中的InitIcm，发送给硬件后执行InitIcm的操作
void 
HanGuDriver::initIcm(PortProxy& portProxy, uint8_t qpcNumLog, uint8_t cqcNumLog, 
        uint8_t mptNumLog, uint8_t mttNumLog) {
    //每种资源（mttMeta, mptMeta, cqcMeta, qpcMeta）的初始化逻辑类似
    //**Resc即资源的entry
    Addr startPtr = 0;
    //MTT的entry只是一个8字节的物理地址
    mttMeta.start     = startPtr;
    mttMeta.size      = ((1 << mttNumLog) * sizeof(HanGuRnicDef::MttResc));
    mttMeta.entrySize = sizeof(HanGuRnicDef::MttResc);//每个entry 8字节
    mttMeta.entryNumLog = mttNumLog;
    mttMeta.entryNumPage= (1 << (mttNumLog-(12-3)));
    //需要几页装下mtt的全部条目
    mttMeta.bitmap    = new uint8_t[mttMeta.entryNumPage]; 
    memset(mttMeta.bitmap, 0, mttMeta.entryNumPage);//置为0
    startPtr += mttMeta.size;
    HANGU_PRINT(HanGuDriver, " mttMeta.entryNumPage 0x%x\n", mttMeta.entryNumPage);
    //MPT的entry是一个32字节结构体
    //     struct MptResc {
    //     uint32_t flag;
    //     uint32_t key;
    //     uint64_t startVAddr;
    //     uint64_t length;
    //     uint64_t mttSeg;
    // };
    mptMeta.start = startPtr;
    mptMeta.size  = ((1 << mptNumLog) * sizeof(HanGuRnicDef::MptResc));
    mptMeta.entrySize = sizeof(HanGuRnicDef::MptResc);
    mptMeta.entryNumLog = mptNumLog;
    mptMeta.entryNumPage = (1 << (mptNumLog-(12-5)));
    mptMeta.bitmap = new uint8_t[mptMeta.entryNumPage];
    memset(mptMeta.bitmap, 0, mptMeta.entryNumPage);
    startPtr += mptMeta.size;
    HANGU_PRINT(HanGuDriver, " mptMeta.entryNumPage 0x%x\n", mptMeta.entryNumPage);

    //CQC的entry是一个16字节结构体
    // struct CqcResc {
    //     uint32_t cqn;
    //     uint32_t offset; // The offset of the CQ
    //     uint32_t lkey;   // lkey of the CQ

    //     // !TODO We don't implement it now.
    //     uint32_t sizeLog; // The size of CQ. (It is now fixed at 4KB)
    // };
    cqcMeta.start = startPtr;
    cqcMeta.size  = ((1 << cqcNumLog) * sizeof(HanGuRnicDef::CqcResc));
    cqcMeta.entrySize = sizeof(HanGuRnicDef::CqcResc);
    cqcMeta.entryNumLog = cqcNumLog;
    cqcMeta.entryNumPage = (1 << (cqcNumLog-(12-4)));
    cqcMeta.bitmap = new uint8_t[cqcMeta.entryNumPage];
    memset(cqcMeta.bitmap, 0, cqcMeta.entryNumPage);
    startPtr += cqcMeta.size;
    HANGU_PRINT(HanGuDriver, " cqcMeta.entryNumPage 0x%x\n", cqcMeta.entryNumPage);
      
    //QPC的entry是一个256字节的结构体
    qpcMeta.start = startPtr;
    qpcMeta.size  = ((1 << qpcNumLog) * sizeof(HanGuRnicDef::QpcResc));
    qpcMeta.entrySize   = sizeof(HanGuRnicDef::QpcResc);
    qpcMeta.entryNumLog = qpcNumLog;
    qpcMeta.entryNumPage = (1 << (qpcNumLog-(12-8)));
    qpcMeta.bitmap = new uint8_t[qpcMeta.entryNumPage];
    memset(qpcMeta.bitmap, 0, qpcMeta.entryNumPage);
    HANGU_PRINT(HanGuDriver, " qpcMeta.entryNumPage 0x%x\n", qpcMeta.entryNumPage);
    
    startPtr += qpcMeta.size;
   //     struct InitResc {
   //     uint8_t qpsNumLog; //qp数量的对数
   //     uint8_t cqsNumLog; //cq数量的对数
   //     uint8_t mptNumLog; //mpt数量的对数
   //     uint64_t qpcBase;  //qpc基址指针
   //     uint64_t cqcBase;
   //     uint64_t mptBase;
   //     uint64_t mttBase;
   // };
    /* put initResc into mailbox */
    HanGuRnicDef::InitResc initResc;
    initResc.qpcBase   = qpcMeta.start;
    initResc.qpsNumLog = qpcNumLog;
    initResc.cqcBase   = cqcMeta.start;
    initResc.cqsNumLog = cqcNumLog;
    initResc.mptBase   = mptMeta.start;
    initResc.mptNumLog = mptNumLog;
    initResc.mttBase   = mttMeta.start;
    HANGU_PRINT(HanGuDriver, " qpcMeta.start: 0x%lx, cqcMeta.start : 0x%lx, mptMeta.start : 0x%lx, mttMeta.start : 0x%lx\n", 
            qpcMeta.start, cqcMeta.start, mptMeta.start, mttMeta.start);
    portProxy.writeBlob(mailbox.vaddr, &initResc, sizeof(HanGuRnicDef::InitResc));
    //把initResc内容写到mailbox虚拟地址对应的地方，或许是已经写入其物理地址了

    //mailbox结构体包含一个虚拟地址一个物理地址
    postHcr(portProxy, (uint64_t)mailbox.paddr, 0, 0, HanGuRnicDef::INIT_ICM);//写HCR
    //INIT_ICM宏为0x01,是一个opcode
}

//ICM中的某个资源的某个条目有没有被映射
uint8_t 
HanGuDriver::isIcmMapped(RescMeta &rescMeta, Addr index) {
    
    //将虚拟地址右移 12 位，得到虚拟页号（一个页通常是 4KB)
    Addr icmVPage = (rescMeta.start + index * rescMeta.entrySize) >> 12;
    //icmAddrmap ICM的地址映射表, std::unordered_map<Addr, Addr> icmAddrmap; <icm vaddr page, icm paddr>
    return (icmAddrmap.find(icmVPage) != icmAddrmap.end());
}

//分配ICM的资源，为某个resc分配，连续分配16页映射关系
Addr 
HanGuDriver::allocIcm(Process *process, RescMeta &rescMeta, Addr index) {
    Addr icmVPage = (rescMeta.start + index * rescMeta.entrySize) >> 12;//获得某个资源某个entry的虚拟页号

    while (icmAddrmap.find(icmVPage) != icmAddrmap.end()) { /* cause we allocate multiply resources one time, 
                                                             * the resources may be cross-page. */
        //如果此index所在的虚拟页已经被映射，则找下一个页，指导这一页没有被映射
        ++icmVPage;
    }
    HANGU_PRINT(HanGuDriver, " rescMeta.start: 0x%lx, index 0x%x, entrySize %d icmVPage 0x%lx\n", rescMeta.start, index, rescMeta.entrySize, icmVPage);
   // ICM_ALLOC_PAGE_NUM = 16 ，一次性分配的页为16页
    for (uint32_t i =  0; i < ICM_ALLOC_PAGE_NUM; ++i) {
        if (i == 0) {
            icmAddrmap[icmVPage] = process->system->allocPhysPages(ICM_ALLOC_PAGE_NUM);
        } else {
            icmAddrmap[icmVPage + i] = icmAddrmap[icmVPage] + (i << 12);
        }
        HANGU_PRINT(HanGuDriver, " icmAddrmap[0x%x(%d)]: 0x%lx\n", icmVPage+i, i, icmAddrmap[icmVPage+i]);
    }
    return icmVPage;
}


//在模拟硬件中写入ICM映射表的16组映射关系，以满足最大批处理需求
void
HanGuDriver::writeIcm(PortProxy& portProxy, uint8_t rescType, RescMeta &rescMeta, Addr icmVPage) {
    
    // put IcmResc into mailbox
    HanGuRnicDef::IcmResc icmResc;
    // struct IcmResc {
    //     uint16_t pageNum;
    //     uint64_t vAddr;
    //     uint64_t pAddr;
    // };
    icmResc.pageNum = ICM_ALLOC_PAGE_NUM; // now we support ICM_ALLOC_PAGE_NUM（16） pages
    icmResc.vAddr   = icmVPage << 12;     
    icmResc.pAddr   = icmAddrmap[icmVPage];
    portProxy.writeBlob(mailbox.vaddr, &icmResc, sizeof(HanGuRnicDef::InitResc));
    //把icmResc内容写到mailbox虚拟地址对应的地方【却是模拟空间的位置】
    HANGU_PRINT(HanGuDriver, " pageNum %d, vAddr 0x%lx, pAddr 0x%lx\n", icmResc.pageNum, icmResc.vAddr, icmResc.pAddr);
    //告诉HCR命令是WRITE_ICM，写的modifier为1，一次性写入16页映射
    postHcr(portProxy, (uint64_t)mailbox.paddr, 1, rescType, HanGuRnicDef::WRITE_ICM);
}

/* -------------------------- ICM {end} ------------------------ */


/* -------------------------- Resc {begin} ------------------------ */
//为某种元数据分配资源
//获取分配资源的最终资源号
uint32_t 
HanGuDriver::allocResc(uint8_t rescType, RescMeta &rescMeta) {
    uint32_t i = 0, j = 0;
    uint32_t rescNum = 0;
    while (rescMeta.bitmap[i] == 0xff) {
        ++i;
    }
    rescNum = i * 8;

    while ((rescMeta.bitmap[i] >> j) & 0x01) {
        ++rescNum;
        ++j;
    }
    rescMeta.bitmap[i] |= (1 << j);

    rescNum += (cpu_id << RESC_LIM_LOG);//最大单个资源的entry数量2^16个
    return rescNum;
}
/* -------------------------- Resc {end} ------------------------ */

/* -------------------------- MTT {begin} ------------------------ */
//分配MTT资源
//根据args中的vaddr，查询进程页表放入args中的paddr，在mttmeta的bitmap中增加batch_size个bit
void 
HanGuDriver::allocMtt(Process *process, 
        TypedBufferArg<kfd_ioctl_init_mtt_args> &args) {
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        args->mtt_index = allocResc(HanGuRnicDef::ICMTYPE_MTT, mttMeta);
        //根据mttmeta中的bitmap信息分配一个资源编号给mtt_index

        //args虽指向内核缓存，里面的vaddr却是从用户空间复制过来的，所以可以用用户进程页表翻译
        process->pTable->translate((Addr)args->vaddr[i], (Addr &)args->paddr[i]);
        HANGU_PRINT(HanGuDriver, " HGKFD_IOC_ALLOC_MTT: vaddr: 0x%lx, paddr: 0x%lx mtt_index %d\n", 
                (uint64_t)args->vaddr[i], (uint64_t)args->paddr[i], args->mtt_index);
    }
    args->mtt_index -= (args->batch_size - 1);//返回批处理第一个mtt_index
}
//写入MTT到gem5模拟内存
//根据args中的paddr，复制到batch_size个mttResc中，并将batch_size个mttResc写出
void 
HanGuDriver::writeMtt(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_init_mtt_args> &args) {
    
    // put mttResc into mailbox
    HanGuRnicDef::MttResc mttResc[MAX_MR_BATCH];//最多512个MttResc ENRTRY
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        mttResc[i].pAddr = args->paddr[i];
    }
    portProxy.writeBlob(mailbox.vaddr, mttResc, sizeof(HanGuRnicDef::MttResc) * args->batch_size);

    postHcr(portProxy, (uint64_t)mailbox.paddr, args->mtt_index, args->batch_size, HanGuRnicDef::WRITE_MTT);
}
/* -------------------------- MTT {end} ------------------------ */

/* -------------------------- MPT {begin} ------------------------ */

void 
HanGuDriver::allocMpt(Process *process, 
        TypedBufferArg<kfd_ioctl_alloc_mpt_args> &args) {
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        args->mpt_index = allocResc(HanGuRnicDef::ICMTYPE_MPT, mptMeta);
        HANGU_PRINT(HanGuDriver, " HGKFD_IOC_ALLOC_MPT: mpt_index %d batch_size %d\n", args->mpt_index, args->batch_size);
    }
    args->mpt_index -= (args->batch_size - 1);
}

//批处理   
void 
HanGuDriver::writeMpt(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_mpt_args> &args) {
    // put MptResc into mailbox
    HanGuRnicDef::MptResc mptResc[MAX_MR_BATCH];//最多512个MptResc ENRTRY
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        mptResc[i].flag       = args->flag     [i];
        mptResc[i].key        = args->mpt_index[i];
        mptResc[i].length     = args->length   [i];
        mptResc[i].startVAddr = args->addr     [i];
        mptResc[i].mttSeg     = args->mtt_index[i];
        HANGU_PRINT(HanGuDriver, " HGKFD_IOC_WRITE_MPT: mpt_index %d(%d) mtt_index %d(%d) batch_size %d\n", 
                args->mpt_index[i], mptResc[i].key, args->mtt_index[i], mptResc[i].mttSeg, args->batch_size);
    }
    portProxy.writeBlob(mailbox.vaddr, mptResc, sizeof(HanGuRnicDef::MptResc) * args->batch_size);

    postHcr(portProxy, (uint64_t)mailbox.paddr, args->mpt_index[0], args->batch_size, HanGuRnicDef::WRITE_MPT);
}
/* -------------------------- MPT {end} ------------------------ */


/* -------------------------- CQC {begin} ------------------------ */
void 
HanGuDriver::allocCqc (TypedBufferArg<kfd_ioctl_alloc_cq_args> &args) {
    args->cq_num = allocResc(HanGuRnicDef::ICMTYPE_CQC, cqcMeta);
}
    
void 
HanGuDriver::writeCqc(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_cqc_args> &args) {
    /* put CqcResc into mailbox */
    HanGuRnicDef::CqcResc cqcResc;
    cqcResc.cqn    = args->cq_num  ;
    cqcResc.lkey   = args->lkey    ;
    cqcResc.offset = args->offset  ;
    cqcResc.sizeLog= args->size_log;
    portProxy.writeBlob(mailbox.vaddr, &cqcResc, sizeof(HanGuRnicDef::CqcResc));

    postHcr(portProxy, (uint64_t)mailbox.paddr, args->cq_num, 0, HanGuRnicDef::WRITE_CQC);
}
/* -------------------------- CQC {end} ------------------------ */


/* -------------------------- QPC {begin} ------------------------ */
void 
HanGuDriver::allocQpc(TypedBufferArg<kfd_ioctl_alloc_qp_args> &args) {
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        // HANGU_PRINT(HanGuDriver, " allocQpc: qpc_bitmap: 0x%x 0x%x 0x%x\n", qpcMeta.bitmap[0], qpcMeta.bitmap[1], qpcMeta.bitmap[2]);
        args->qp_num = allocResc(HanGuRnicDef::ICMTYPE_QPC, qpcMeta);
        HANGU_PRINT(HanGuDriver, " allocQpc: qpc_bitmap:  0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n"
                                                        " 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n"
                                                        " 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n"
                                                        " 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n", 
                qpcMeta.bitmap[0], qpcMeta.bitmap[1], qpcMeta.bitmap[2], qpcMeta.bitmap[3], qpcMeta.bitmap[4], qpcMeta.bitmap[5], qpcMeta.bitmap[6], qpcMeta.bitmap[7], 
                qpcMeta.bitmap[8], qpcMeta.bitmap[9], qpcMeta.bitmap[10], qpcMeta.bitmap[11], qpcMeta.bitmap[12], qpcMeta.bitmap[13], qpcMeta.bitmap[14], qpcMeta.bitmap[15], 
                qpcMeta.bitmap[16], qpcMeta.bitmap[17], qpcMeta.bitmap[18], qpcMeta.bitmap[19], qpcMeta.bitmap[20], qpcMeta.bitmap[21], qpcMeta.bitmap[22], qpcMeta.bitmap[23], 
                qpcMeta.bitmap[24], qpcMeta.bitmap[25], qpcMeta.bitmap[26], qpcMeta.bitmap[27], qpcMeta.bitmap[28], qpcMeta.bitmap[29], qpcMeta.bitmap[30], qpcMeta.bitmap[31]);
    }
    args->qp_num -= (args->batch_size - 1);
}
    
void 
HanGuDriver::writeQpc(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_qpc_args> &args) {
    /* put QpcResc into mailbox */
    HanGuRnicDef::QpcResc qpcResc[MAX_QPC_BATCH]; // = (HanGuRnicDef::QpcResc *)mailbox.vaddr;
    memset(qpcResc, 0, sizeof(HanGuRnicDef::QpcResc) * args->batch_size);
    for (uint32_t i = 0; i < args->batch_size; ++i) {
        qpcResc[i].flag           = args->flag             [i];
        qpcResc[i].qpType         = args->type             [i];
        qpcResc[i].srcQpn         = args->src_qpn          [i];
        qpcResc[i].lLid           = args->llid             [i];
        qpcResc[i].cqn            = args->cq_num           [i];
        qpcResc[i].sndWqeBaseLkey = args->snd_wqe_base_lkey[i];
        qpcResc[i].sndWqeOffset   = args->snd_wqe_offset   [i];
        qpcResc[i].sqSizeLog      = args->sq_size_log      [i];
        qpcResc[i].rcvWqeBaseLkey = args->rcv_wqe_base_lkey[i];
        qpcResc[i].rcvWqeOffset   = args->rcv_wqe_offset   [i];
        qpcResc[i].rqSizeLog      = args->rq_size_log      [i];

        qpcResc[i].ackPsn  = args->ack_psn [i];
        qpcResc[i].sndPsn  = args->snd_psn [i];
        qpcResc[i].expPsn  = args->exp_psn [i];
        qpcResc[i].dLid    = args->dlid    [i];
        qpcResc[i].destQpn = args->dest_qpn[i];

        qpcResc[i].qkey    = args->qkey[i];

        HANGU_PRINT(HanGuDriver, " writeQpc: qpn: 0x%x\n", qpcResc[i].srcQpn);
    }
    HANGU_PRINT(HanGuDriver, " writeQpc: args->batch_size: %d\n", args->batch_size);
    portProxy.writeBlob(mailbox.vaddr, qpcResc, sizeof(HanGuRnicDef::QpcResc) * args->batch_size);
    HANGU_PRINT(HanGuDriver, " writeQpc: args->batch_size1: %d\n", args->batch_size);

    postHcr(portProxy, (uint64_t)mailbox.paddr, args->src_qpn[0], args->batch_size, HanGuRnicDef::WRITE_QPC);
}


/* -------------------------- QPC {end} ------------------------ */

/* -------------------------- Mailbox {begin} ------------------------ */

//mailbox 32页虚拟内存到物理内存完成映射
void 
HanGuDriver::initMailbox(Process *process) {

    uint32_t allocPages = MAILBOX_PAGE_NUM;
    
    mailbox.paddr = process->system->allocPhysPages(allocPages);
    //maibox面向进程建立，mailbox.vaddr是用户空间的虚拟地址，好像不是内核的虚拟地址
    
    // Assume mmap grows down, as in x86 Linux.
    auto mem_state = process->memState;
    mailbox.vaddr = mem_state->getMmapEnd() - (allocPages << 12);
    mem_state->setMmapEnd(mailbox.vaddr);
    process->pTable->map(mailbox.vaddr, mailbox.paddr, (allocPages << 12), false);

    HANGU_PRINT(HanGuDriver, " mailbox.vaddr : 0x%x, mailbox.paddr : 0x%x\n", 
            mailbox.vaddr, mailbox.paddr);
}

/* -------------------------- Mailbox {end} ------------------------ */

HanGuDriver*
HanGuDriverParams::create()
{
    return new HanGuDriver(this);
}

//2024.12.20 下午16：05第一次读完
//2024.12.23 下午14：20第三次读完，深入理解
//2024.12.25 下午12：06第四次读完，深入理解
