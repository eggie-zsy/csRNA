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
 * RDMA NIC Device statistic declaration.
 */

#ifndef __RDMA_RDMANIC_HH__
#define __RDMA_RDMANIC_HH__

#include "base/statistics.hh"
#include "dev/pci/device.hh"
#include "params/RdmaNic.hh"
#include "sim/sim_object.hh"

class EtherInt;//一个以太网口类

class RdmaNic : public PciDevice {//继承自PciDevice
  public:
    typedef RdmaNicParams Params;
    RdmaNic(const Params *params)
        : PciDevice(params) //RDMANIC的构造函数只是让父类PciDevice接受这个参数并初始化
    {}

    const Params *
    params() const {
        return dynamic_cast<const Params *>(_params);
    }

  public:
    void regStats();

  protected:
    Stats::Scalar txBytes;//标量
    Stats::Scalar rxBytes;
    Stats::Scalar txPackets;
    Stats::Scalar rxPackets;
    Stats::Scalar descDmaReads;
    Stats::Scalar descDmaWrites;
    Stats::Scalar descDmaRdBytes;
    Stats::Scalar descDmaWrBytes;
    Stats::Formula totBandwidth; //公式
    Stats::Formula totPackets;
    Stats::Formula totBytes;
    Stats::Formula totPacketRate;
    Stats::Formula txBandwidth;
    Stats::Formula rxBandwidth;
    Stats::Formula txPacketRate;
    Stats::Formula rxPacketRate;
};

#endif // __RDMA_RDMANIC_HH__
// totBandwidth：表示总带宽。
// totPackets：表示总数据包数。
// totBytes：表示总字节数。
// totPacketRate：表示总数据包率。
// txBandwidth：表示发送带宽。
// rxBandwidth：表示接收带宽。
// txPacketRate：表示发送数据包率。
// rxPacketRate：表示接收数据包率。

//2024.12.17 17:06第一次读完
