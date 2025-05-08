import os
import time
import sys

SERVER_LID = 10

NUM_CPUS = 10
CPU_CLK = "2GHz"
EN_SPEED = "100Gbps"
PCI_SPEED = "128Gbps"

class Param:
    def __init__(self, num_nodes, qpc_cache_cap, reorder_cap, op_mode):
        self.num_nodes = num_nodes
        self.qpc_cache_cap = qpc_cache_cap
        self.reorder_cap = reorder_cap
        self.op_mode = op_mode

def cmd_run_sim(debug, test_prog, option, params, output_file,threadid):
    """生成仿真运行命令"""
    cmd = "/root/myzsy/csRNA/build/X86/gem5.opt"
    cmd += " --outdir=/root/myzsy/csRNA/m5_out/"+str(threadid)

    # 添加调试选项
    if debug != "":
        debug = " --debug-flags=" + debug
    cmd += debug

    # 执行脚本
    cmd += " /root/myzsy/csRNA/configs/example/rdma/hangu_rnic_se.py"
    cmd += " --cpu-clock " + CPU_CLK
    cmd += " --num-cpus " + str(NUM_CPUS)
    cmd += " -c " + test_prog
    cmd += " -o " + option
    cmd += " --node-num " + str(params.num_nodes)
    cmd += " --ethernet-linkspeed " + EN_SPEED
    cmd += " --pci-linkspeed " + PCI_SPEED
    cmd += " --qpc-cache-cap " + str(params.qpc_cache_cap)
    cmd += " --reorder-cap " + str(params.reorder_cap)
    cmd += " --mem-size 4096MB"
    cmd += f" > {output_file}"  # 动态指定输出文件路径

    return cmd

def execute_program(debug, test_prog, option, params,output_file,thread_id):
    output_file = os.path.join("/root/myzsy/csRNA/scripts", output_file)
    cmd_list = [
        f"cd ../tests/test-progs/hangu-rnic/src && make THREAD_ID={thread_id}"
    ]
    cmd_list.append(cmd_run_sim(debug, test_prog, option, params,output_file,thread_id))

    for cmd in cmd_list:
        print(cmd)
        rtn = os.system(cmd)
        if rtn != 0:
            raise Exception("\033[0;31;40mError for cmd " + cmd + "\033[0m")
        time.sleep(0.1)
        

def main():
    if len(sys.argv) < 9:
        raise Exception("\033[0;31;40mMissing input parameter. Needs 9.\033[0m")
    params = Param(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
    output_file = sys.argv[5]  # 获取输出文件路径
    server_copy = sys.argv[6]  # 传入的 server 副本
    client_copy = sys.argv[7]  # 传入的 client 副本
    server_copy,_= os.path.splitext(server_copy)  # 分离文件名和扩展名
    client_copy,_= os.path.splitext(client_copy)  # 分离文件名和扩展名

    #makefile_copy=sys.argv[8]
    #thread_id=sys.argv[9]
    thread_id=sys.argv[8]

    num_nodes = params.num_nodes
    svr_lid = SERVER_LID  # svr_lid = 10

    debug = ""
    test_prog = "'/root/myzsy/csRNA/tests/test-progs/hangu-rnic/bin/"+str(server_copy)
    opt = "'-s " + str(svr_lid) + " -t " + str(num_nodes - 1) + " -m " + str(params.op_mode)

    # 分别运行 num_nodes-1 个 client
    for i in range(num_nodes - 1):
        test_prog += ";/root/myzsy/csRNA/tests/test-progs/hangu-rnic/bin/"+str(client_copy)
        opt += ";-s " + str(svr_lid) + " -l " + str(svr_lid + i + 1) + " -t " + str(num_nodes - 1) + " -m " + str(params.op_mode)
    test_prog += "'"
    opt += "'"

    return execute_program(debug=debug, test_prog=test_prog, option=opt, params=params, output_file=output_file,thread_id=thread_id)

if __name__ == "__main__":
    main()

