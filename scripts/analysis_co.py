import os
import shutil
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading

write_lock = threading.Lock()

CLIENT_NUM = 3
QP_CACHE_CAP = 200
REORDER_CAP = 16
QP_NUM_LIST = [1,2,4,8,16,32,64,128,256,512]
WR_TYPE = 0  # 0 - rdma write; 1 - rdma read
PCIE_TYPE = "X16"
VERSION = "V1.5-final"
RECORD_FILENAME = "res_out/record-" + VERSION + "_QP_CACHE_CAP" + str(QP_CACHE_CAP) + "RECAP" + str(REORDER_CAP) + ".txt"
TEST_OUTPUT_DIR = "res_out/test"


def clear_test_directory():
    if os.path.exists(TEST_OUTPUT_DIR):
        for filename in os.listdir(TEST_OUTPUT_DIR):
            file_path = os.path.join(TEST_OUTPUT_DIR, filename)
            try:
                if os.path.isfile(file_path) or os.path.islink(file_path):
                    os.unlink(file_path)
                elif os.path.isdir(file_path):
                    shutil.rmtree(file_path)
            except Exception as e:
                print(f"Failed to delete {file_path}. Reason: {e}")
    else:
        os.makedirs(TEST_OUTPUT_DIR)

    for qps_per_clt in QP_NUM_LIST:
        output_file = os.path.join(TEST_OUTPUT_DIR, f"rnic_sys_test_{qps_per_clt}.txt")
        with open(output_file, "w") as f:
            f.write("")


def create_file_copy(src_file, thread_id):
    base_name = os.path.basename(src_file)
    copy_name = f"{os.path.splitext(base_name)[0]}_thread{thread_id}{os.path.splitext(base_name)[1]}"
    copy_path = os.path.join(os.path.dirname(src_file), copy_name)
    shutil.copy(src_file, copy_path)
    return copy_path


def modify_include_statement(file_path, librdma_copy_name):
    file_data = ""
    with open(file_path, "r", encoding="utf-8") as f:
        for line in f:
            if '#include "librdma.h"' in line:
                line = f'#include "{librdma_copy_name}"'
            file_data += line

    with open(file_path, "w", encoding="utf-8") as f:
        f.write(file_data)


def change_param(qps_per_clt, thread_id):
    librdma_copy = create_file_copy("../tests/test-progs/hangu-rnic/src/librdma.h", thread_id)
    librdma_copy_name = os.path.basename(librdma_copy)

    file_data = ""
    with open(librdma_copy, "r", encoding="utf-8") as f:
        for line in f:
            if "#define TEST_QP_NUM" in line:
                line = "#define TEST_QP_NUM   " + str(qps_per_clt) + "\n"
            file_data += line

    with open(librdma_copy, "w", encoding="utf-8") as f:
        f.write(file_data)

    server_copy = create_file_copy("../tests/test-progs/hangu-rnic/src/server.c", thread_id)
    client_copy = create_file_copy("../tests/test-progs/hangu-rnic/src/client.c", thread_id)
    librdma_c_copy = create_file_copy("../tests/test-progs/hangu-rnic/src/librdma.c", thread_id)
    
    modify_include_statement(server_copy, librdma_copy_name)
    modify_include_statement(client_copy, librdma_copy_name)
    modify_include_statement(librdma_c_copy, librdma_copy_name)

    makefile_copy = create_file_copy("../tests/test-progs/hangu-rnic/src/Makefile", thread_id)
    return librdma_copy, server_copy, client_copy, librdma_c_copy, makefile_copy


def execute_program(node_num, qpc_cache_cap, reorder_cap, output_file, librdma_copy, server_copy, client_copy, librdma_c_copy, makefile_copy,thread_id):
    # os.system(f"cd ../tests/test-progs/hangu-rnic/src && make -f {os.path.basename(makefile_copy)}")
    cmd = f"python3 run_hangu_co.py {node_num} {qpc_cache_cap} {reorder_cap} {WR_TYPE} {output_file} {os.path.basename(server_copy)} {os.path.basename(client_copy)} {os.path.basename(makefile_copy)} {thread_id}"
    print(cmd)
    rtn = os.system(cmd)
    if rtn != 0:
        raise Exception("Error: Failed to run simulation")


def print_result(file_name, qps_per_clt):
    """从输出文件中提取结果并记录"""
    bandwidth = 0
    msg_rate = 0
    latency = 0
    cnt = 0
    with open(file_name) as f:
        for line in f.readlines():
            if "start time" in line:  # 提取数据以便计算
                bandwidth += float(line.split(',')[2].strip().split(' ')[1])
                msg_rate += float(line.split(',')[3].strip().split(' ')[1])
                latency += float(line.split(',')[4].strip().split(' ')[1])
                cnt += 1

    bandwidth = round(bandwidth, 2)
    msg_rate = round(msg_rate, 2)
    latency = round(latency / (cnt * 1000.0), 2)
    

    res_data = "==========================================\n"
    res_data += str(qps_per_clt) + " QPs * " + str(CLIENT_NUM) + " clients * " + str(cnt) + " CPU_NUM = " + str(
        qps_per_clt * CLIENT_NUM * cnt) + "\n"
    if WR_TYPE == 1:
        res_data += "RDMA READ\n"
    else:
        res_data += "RDMA WRITE\n"
    res_data += "QPS_PER_CLT " + str(qps_per_clt) + "\n"
    res_data += "CPU_NUM     " + str(cnt) + "\n"
    res_data += "bandwidth   " + str(bandwidth) + " MB/s\n"
    res_data += "msg_rate    " + str(msg_rate) + " Mops/s\n"
    res_data += "latency    " + str(latency) + " us\n"
    res_data += "==========================================\n\n"

    with write_lock:
        with open(RECORD_FILENAME, "a+", encoding="utf-8") as f:
            f.write(res_data)

    return bandwidth, msg_rate, latency

def run_test(qps_per_clt, thread_id):
    librdma_copy, server_copy, client_copy, librdma_c_copy, makefile_copy = change_param(qps_per_clt, thread_id)
    output_file = os.path.join(TEST_OUTPUT_DIR, f"rnic_sys_test_{qps_per_clt}.txt")
    print("=============================================")
    print("qps_per_clt is : %d" % (qps_per_clt))
    print("=============================================\n\n\n\n")
    try:
        execute_program(CLIENT_NUM + 1, QP_CACHE_CAP, REORDER_CAP, output_file, librdma_copy, server_copy, client_copy, librdma_c_copy, makefile_copy,thread_id)
    except Exception as e:
        print(f"Program execution error! {qps_per_clt}: {e}")
        return qps_per_clt, None, None, None

     # 获取结果

    bw, mr, lat= print_result(output_file, qps_per_clt)
    os.remove(librdma_copy)
    os.remove(server_copy)
    os.remove(client_copy)
    os.remove(librdma_c_copy)
    os.remove(makefile_copy)
    print(f"Deleted temporary files: {librdma_copy}, {server_copy}, {client_copy}, {librdma_c_copy}, {makefile_copy}")
    return qps_per_clt, bw, mr, lat

def main():
    # 清空 test 目录并初始化创建输出文件
    clear_test_directory()

    bandwidth = []
    msg_rate = []
    latency = []
    # 编译 hangu-rnic 源码和 gem5
    # os.system("cd ../tests/test-progs/hangu-rnic/src && make")
    os.system("cd ../ && scons build/X86/gem5.opt -j$(nproc)")

    # 使用线程池并行执行测试
    with ThreadPoolExecutor(max_workers=len(QP_NUM_LIST)) as executor:
        futures = {executor.submit(run_test, qps_per_clt, i): qps_per_clt for i, qps_per_clt in enumerate(QP_NUM_LIST)}

        for future in as_completed(futures):
            qps_per_clt, bw, mr, lat = future.result()
            if bw is not None:
                bandwidth.append((qps_per_clt, bw))
                msg_rate.append((qps_per_clt, mr))
                latency.append((qps_per_clt, lat))
                

    # 按 QP 数量排序结果
    bandwidth.sort(key=lambda x: x[0])
    msg_rate.sort(key=lambda x: x[0])
    latency.sort(key=lambda x: x[0])
    
    # 打印结果
    print("QP_NUM_LIST:", QP_NUM_LIST)
    print("Bandwidth:", [x[1] for x in bandwidth])
    print("Msg Rate:", [x[1] for x in msg_rate])
    print("Latency:", [x[1] for x in latency])
    

    # 将结果写入文件
    with open(RECORD_FILENAME, "a+", encoding="utf-8") as f:
        f.write(" ".join(list(map(str, QP_NUM_LIST))))
        f.write("\n")
        f.write(" ".join(list(map(str, [x[1] for x in bandwidth]))))
        f.write("\n")
        f.write(" ".join(list(map(str, [x[1] for x in msg_rate]))))
        f.write("\n")
        f.write(" ".join(list(map(str, [x[1] for x in latency]))))
        f.write("\n")
       

if __name__ == "__main__":
    main()