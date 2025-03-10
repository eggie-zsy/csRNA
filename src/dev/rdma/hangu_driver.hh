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
 *  Date: 2021.07.08
 */

/**
 * @file
 * The HanGuDriver implements an RnicDriver for an RDMA NIC
 * agent. 
 */

#ifndef __RDMA_HANGU_DRIVER_HH__
#define __RDMA_HANGU_DRIVER_HH__

#include "hangu_rnic_defs.hh"
#include "dev/rdma/kfd_ioctl.h"
#include "base/time.hh"

#include "dev/rdma/rdma_nic.hh"
#include "debug/HanGu.hh"
#include "params/HanGuDriver.hh"

#include "sim/proxy_ptr.hh"
#include "sim/process.hh"
#include "sim/system.hh"
#include "cpu/thread_context.hh"
#include "sim/syscall_emul_buf.hh"

#include "base/types.hh"
#include "sim/emul_driver.hh"

//这个文件应该是网卡的内核态驱动

class RdmaNic;
class PortProxy;
class ThreadContext;

struct HanGuDriverParams;
//可能是一个虚拟设备驱动，主要用来模拟与硬件设备的交互
class HanGuDriver final : public EmulatedDriver {//不能被继承
  public:
    typedef HanGuDriverParams Params;
    HanGuDriver(Params *p);     //hangudriver的构造函数，传入一个HanGuDriverParams型指针
    
    //用于打开与设备的连接，设置设备的操作模式
    int open(ThreadContext *tc, int mode, int flags);
    //用于发送设备控制命令，req 指定请求类型，ioc_buf 是传递给设备的数据缓冲区。
    int ioctl(ThreadContext *tc, unsigned req, Addr ioc_buf) override;
    Addr mmap(ThreadContext *tc, Addr start, uint64_t length,
    //这个函数用于内存映射操作，可能是将设备的某个内存区域映射到进程的地址空间
              int prot, int tgtFlags, int tgtFd, int offset);

  protected:
    /**
     * RDMA agent (device) that is controled by this driver.
     */
    RdmaNic *device;    //这个驱动控制的RNIC
    
    Addr hcrAddr;      //硬件控制寄存器的地址

    void configDevice(); //配置设备

  private:

    /* -------CPU_ID{begin}------- */
    uint8_t cpu_id;
    /* -------CPU_ID{end}------- */

    /* -------HCR {begin}------- */
    uint8_t checkHcr(PortProxy& portProxy);

    void postHcr(PortProxy& portProxy, 
            uint64_t inParam, uint32_t inMod, uint64_t outParam, uint8_t opcode);
    /* -------HCR {end}------- */

    /* ------- Resc {begin} ------- */
    struct RescMeta {
        Addr     start ;  // start index (icm vaddr) of the resource
        uint64_t size  ;  // size of the resource(in byte)
        uint32_t entrySize; // size of one entry (in byte)
        uint8_t  entryNumLog;
        uint32_t entryNumPage;
        
        // ICM space bitmap, one bit indicates one page.
        uint8_t *bitmap;  // resource bitmap, 
    };

    uint32_t allocResc(uint8_t rescType, RescMeta &rescMeta);
    /* ------- Resc {end} ------- */

    /* -------ICM resources {begin}------- */
    /* we use one entry to store one page */

    //ICM的地址映射表
    std::unordered_map<Addr, Addr> icmAddrmap; // <icm vaddr page, icm paddr>
    
    //初始化ICM
    void initIcm(PortProxy& portProxy, uint8_t qpcNumLog, 
            uint8_t cqcNumLog, uint8_t mptNumLog, uint8_t mttNumLog);
    
    //ICM页是否被映射
    uint8_t isIcmMapped(RescMeta &rescMeta, Addr index);

    /**
     * @brief Allocate ICM space paddr. Allocate ICM_ALLOC_PAGE_NUM pages one time
     * 
     * @param process 
     * @param rescMeta 
     * @param index Start index of allocated resource this time
     * @return Addr: start Virtual Page of the ICM space
     */
    //分配ICM物理地址空间，一次分配4页
    Addr allocIcm(Process *process, RescMeta &rescMeta, Addr index);

    // write <icm vaddr, paddr> into hardware
    //写映射表至硬件中
    void writeIcm(PortProxy& portProxy, uint8_t rescType, RescMeta &rescMeta, Addr icmVPage);
    /* -------ICM resources {end}------- */


    /* -------MTT resources {begin}------- */
    //MTT元数据
    RescMeta mttMeta;

    // allocate mtt resources
    //分配MTT资源
    void allocMtt(Process *process, TypedBufferArg<kfd_ioctl_init_mtt_args> &args);
    
    //写MTT
    void writeMtt(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_init_mtt_args> &args);
    /* -------MTT resources {end}------- */

    /* -------MPT resources {begin}------- */
    //MPT的元数据
    RescMeta mptMeta;

    //allocate mpt resources
    //分配MPT的资源
    void allocMpt(Process *process, TypedBufferArg<kfd_ioctl_alloc_mpt_args> &args);
    
    //写MPT
    void writeMpt(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_mpt_args> &args);
    /* -------MTT resources {end}------- */

    /* -------CQC resources {begin}------- */
    //CQC的元数据
    RescMeta cqcMeta;

    //分配CQC资源
    void allocCqc(TypedBufferArg<kfd_ioctl_alloc_cq_args> &args);
    
    //写CQC
    void writeCqc(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_cqc_args> &args);
    /* -------CQC resources {end}------- */

    /* -------QPC resources {begin}------- */
    RescMeta qpcMeta;

    // allocate qp resources
    void allocQpc(TypedBufferArg<kfd_ioctl_alloc_qp_args> &args);
    
    void writeQpc(PortProxy& portProxy, TypedBufferArg<kfd_ioctl_write_qpc_args> &args);
    /* -------QPC resources {end}------- */

    /* -------mailbox {begin} ------- */

    // Addr mailbox;
    struct Mailbox {
        Addr paddr;
        Addr vaddr;
    };
    Mailbox mailbox;

    void initMailbox(Process *process);
    /* -------mailbox {end} ------- */

};

#endif // __RDMA_HANGU_DRIVER_HH__
