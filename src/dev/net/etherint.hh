/*
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* @file
 * Class representing the actual interface between two ethernet
 * components.
 */

#ifndef __DEV_NET_ETHERINT_HH__
#define __DEV_NET_ETHERINT_HH__

#include <string>

#include "dev/net/etherpkt.hh"
#include "mem/port.hh"

/*
 * Class representing the actual interface between two ethernet
 * components.  These components are intended to attach to another
 * ethernet interface on one side and whatever device on the other.
 */
//以太网口，继承自port
class EtherInt : public Port
{
  protected:
    mutable std::string portName;//portname设置为mutable，所以可以在const成员函数中被修改
    EtherInt *peer;//指向另一个以太网口

  public:
    EtherInt(const std::string &name, int idx=InvalidPortID)//构造函数，传入一个string引用，一个idx默认值为-1（无效）
        : Port(name, idx), portName(name), peer(NULL) {}//调用父类的构造函数，portname和peer初始化
    virtual ~EtherInt() {}

    /** Return port name (for DPRINTF). */
    const std::string &name() const { return portName; }//返回名字的引用，便于直接修改

    void bind(Port &peer) override;//覆写父类的bind函数
    void unbind() override;        //覆写父类的unbind函数

    void setPeer(EtherInt *p);     //设置连接对象
    EtherInt* getPeer() { return peer; }  //获取对象的指针

    void recvDone() { peer->sendDone(); }  //完成接收
    virtual void sendDone() = 0;          //纯虚函数，需要被其子类覆写，否则子类无法实例化

    bool sendPacket(EthPacketPtr packet)     //发送一个以太网包
    { return peer ? peer->recvPacket(packet) : true; }  //如果peer是null，返回true，否则让peer接受这个数据包
    virtual bool recvPacket(EthPacketPtr packet) = 0;  //纯虚函数，需要被子类覆写，接受一个数据包，被覆写的内容在其子类hangurnicInt内部

    bool askBusy() {return peer->isBusy(); }  //询问peer是否忙，返回bool值
    virtual bool isBusy() { return false; }   //可被覆写，永远告诉对方自己不忙
};

#endif // __DEV_NET_ETHERINT_HH__
