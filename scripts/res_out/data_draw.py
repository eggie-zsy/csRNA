import matplotlib
# Use Agg backend which doesn't require display
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import re
import sys
import os
from matplotlib.ticker import FuncFormatter
def format_k(x, pos):
    if x >= 1000:
        return f'{int(x/1000)}k'
    else:
        return f'{int(x)}'
    
plt.rcParams.update({
    'font.size': 16,
    'axes.titlesize': 18,
    'axes.labelsize': 16,
    'xtick.labelsize': 14,
    'ytick.labelsize': 14,
    'legend.fontsize': 14,
})

def extract_rdma_data(file_path):
    qp_numbers_write = []
    msg_rates_write = []
    latencies_write = []
    bandwidth_write = []
    qp_numbers_read = []
    msg_rates_read = []
    latencies_read = []

    with open(file_path, 'r') as file:
        content = file.read()

        rdma_write_blocks = re.findall(
            r"(\d+) QPs \* \d+ clients \* \d+ CPU_NUM = (\d+)\s*RDMA WRITE[\s\S]*?bandwidth\s+([\d.]+) MB/s[\s\S]*?msg_rate\s+([\d.]+) Mops/s[\s\S]*?latency\s+([\d.]+) us",
            content
        )

        for block in rdma_write_blocks:
            qp_numbers_write.append(int(block[1]))
            bandwidth_write.append(float(block[2]))
            msg_rates_write.append(float(block[3]))
            latencies_write.append(float(block[4]))

        rdma_read_blocks = re.findall(
            r"(\d+) QPs \* \d+ clients \* \d+ CPU_NUM = (\d+)\s*RDMA READ[\s\S]*?msg_rate\s+([\d.]+) Mops/s[\s\S]*?latency\s+([\d.]+) us",
            content
        )

        for block in rdma_read_blocks:
            qp_numbers_read.append(int(block[1]))
            msg_rates_read.append(float(block[2]))
            latencies_read.append(float(block[3]))

    return (qp_numbers_write, msg_rates_write, latencies_write,
            qp_numbers_read, msg_rates_read, latencies_read, bandwidth_write)

def extract_rdma_write_data(file_paths):
    all_qp_numbers = []
    all_msg_rates = []

    for file_path in file_paths:
        qp_numbers_write, msg_rates_write, _, _, _, _, _ = extract_rdma_data(file_path)
        all_qp_numbers.append(qp_numbers_write)
        all_msg_rates.append(msg_rates_write)

    return all_qp_numbers, all_msg_rates

def extract_rdma_write_data4(file_paths):
    all_qp_numbers = []
    all_latency = []

    for file_path in file_paths:
        qp_numbers_write, _, write_latency, _, _, _, _ = extract_rdma_data(file_path)
        all_qp_numbers.append(qp_numbers_write)
        all_latency.append(write_latency)

    return all_qp_numbers, all_latency

def extract_rdma_write_data6(file_paths):
    all_qp_numbers = []
    all_latency = []
    all_bandwidth = []

    for file_path in file_paths:
        qp_numbers_write, all_msg_rate, write_latency, _, _, _, bandwidth_write = extract_rdma_data(file_path)
        all_qp_numbers.append(qp_numbers_write)
        all_latency.append(write_latency)
        all_bandwidth.append(bandwidth_write)

    return all_qp_numbers, all_msg_rate, all_latency, all_bandwidth

def sort_data(qp_numbers, msg_rates, latencies):
    sorted_data = sorted(zip(qp_numbers, msg_rates, latencies), key=lambda x: x[0])
    return [x[0] for x in sorted_data], [x[1] for x in sorted_data], [x[2] for x in sorted_data]

def mode_1(file_path):
    (qp_numbers_write, msg_rates_write, latencies_write,
     qp_numbers_read, msg_rates_read, latencies_read, _) = extract_rdma_data(file_path)

    qp_numbers_write, msg_rates_write, latencies_write = sort_data(qp_numbers_write, msg_rates_write, latencies_write)
    qp_numbers_read, msg_rates_read, latencies_read = sort_data(qp_numbers_read, msg_rates_read, latencies_read)


    fig, ax1 = plt.subplots(figsize=(12, 8))

    color = 'tab:blue'
    ax1.set_xlabel('QP Number', fontsize=24)  # 可以单独设置更大的字体
    ax1.set_ylabel('Message Rate (Mops/s)', color=color, fontsize=24)
    ax1.plot(qp_numbers_write, msg_rates_write, color=color, marker='o', label='WRITE Message Rate', linewidth=3)
    ax1.tick_params(axis='y', labelcolor=color, labelsize=24)  # 设置刻度标签大小
    ax1.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    
    ax1.set_ylim(20, 50)

    color = 'tab:green'
    ax1.plot(qp_numbers_read, msg_rates_read, color=color, marker='^', label='READ Message Rate', linewidth=3)

    ax2 = ax1.twinx()
    color = 'tab:red'
    ax2.set_ylabel('Latency (us)', color=color, fontsize=24)
    ax2.plot(qp_numbers_write, latencies_write, color=color, marker='s', label='WRITE Latency', linewidth=3)
    ax2.tick_params(axis='y', labelcolor=color, labelsize=24)

    color = 'tab:purple'
    ax2.plot(qp_numbers_read, latencies_read, color=color, marker='D', label='READ Latency', linewidth=3)

    ax1.set_xscale('linear')
    ax1.xaxis.set_major_formatter(FuncFormatter(format_k))

    fig.legend(loc='lower right', bbox_to_anchor=(0.9, 0.1), fontsize=24)  # 设置图例字体大小

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_1_rdma_write_read.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_2(file_paths):
    all_qp_numbers, all_msg_rates = extract_rdma_write_data(file_paths)

    for i in range(len(all_qp_numbers)):
        combined = list(zip(all_qp_numbers[i], all_msg_rates[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            all_qp_numbers[i][j] = combined[j][0]
            all_msg_rates[i][j] = combined[j][1]

    print(all_qp_numbers)
    print(all_msg_rates)
    fig, ax = plt.subplots(figsize=(12, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red', 'tab:purple', 'tab:orange', 'tab:brown', 'tab:pink']
    labels = ['Head-Blocked', 'RECAP1', 'RECAP4', 'RECAP8', 'RECAP16', 'RECAP32', 'RECAP64']

    for i in range(len(file_paths)):
        ax.plot(all_qp_numbers[i], all_msg_rates[i], color=colors[i], marker='o', label=labels[i], linewidth=3)

    ax.set_xlabel('QP Number',fontsize=24)
    ax.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax.set_ylabel('Message Rate (Mops/s)',fontsize=24)
    ax.set_ylim(0, 50)
    ax.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小
    ax.legend(loc='upper right', ncol=4)

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_2_rdma_write_msg_rate.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_3(file_paths):
    all_qp_numbers, all_msg_rates = extract_rdma_write_data(file_paths)
    for i in range(len(all_qp_numbers)):
        combined = list(zip(all_qp_numbers[i], all_msg_rates[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            all_qp_numbers[i][j] = combined[j][0]
            all_msg_rates[i][j] = combined[j][1]
    fig, ax = plt.subplots(figsize=(14, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red']
    labels = ['QPC_CACHE_CAP100', 'QPC_CACHE_CAP200', 'QPC_CACHE_CAP300']

    for i in range(len(file_paths)):
        ax.plot(all_qp_numbers[i], all_msg_rates[i], color=colors[i], marker='o', label=labels[i], linewidth=3)

    ax.set_xlabel('QP Number',fontsize=24)
    ax.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax.set_ylabel('Message Rate (Mops/s)',fontsize=24)
    ax.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小
    ax.legend(loc='lower right', ncol=1,fontsize=24)

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_3_rdma_write_cap_comparison.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_4(file_paths):
    all_qp_numbers, all_latencies = extract_rdma_write_data4(file_paths)
    for i in range(len(all_qp_numbers)):
        combined = list(zip(all_qp_numbers[i], all_latencies[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            all_qp_numbers[i][j] = combined[j][0]
            all_latencies[i][j] = combined[j][1]

    fig, ax = plt.subplots(figsize=(12, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red', 'tab:purple', 'tab:orange', 'tab:brown', 'tab:pink']
    labels = ['Head-Blocked', 'RECAP1', 'RECAP4', 'RECAP8', 'RECAP16', 'RECAP32', 'RECAP64']

    for i in range(len(file_paths)):
        ax.plot(all_qp_numbers[i], all_latencies[i], color=colors[i], marker='o', label=labels[i], linewidth=3)


    ax.set_xlabel('QP Number',fontsize=24)
    ax.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax.set_ylabel('Latency (us)',fontsize=24)
    ax.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小    
    ax.set_ylim(3, 5.5)
    ax.legend(loc='lower right', ncol=2,fontsize=24)


    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_4_rdma_write_latency.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_5(file_paths):
    all_qp_numbers, all_latencies = extract_rdma_write_data4(file_paths)
    for i in range(len(all_qp_numbers)):
        combined = list(zip(all_qp_numbers[i], all_latencies[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            all_qp_numbers[i][j] = combined[j][0]
            all_latencies[i][j] = combined[j][1]

    fig, ax = plt.subplots(figsize=(14, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red']
    labels = ['QPC_CACHE_CAP100', 'QPC_CACHE_CAP200', 'QPC_CACHE_CAP300']

    for i in range(len(file_paths)):
        ax.plot(all_qp_numbers[i], all_latencies[i], color=colors[i], marker='o', label=labels[i], linewidth=3)

    ax.set_xlabel('QP Number',fontsize=24)
    ax.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax.set_ylabel('Latency (us)',fontsize=24)
    ax.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小    
    ax.set_ylim(3, 4.7)
    ax.legend(loc='lower right', ncol=2,fontsize=24)

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_5_rdma_write_latency_cap_comparison.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_6(file_paths):
    all_qp_numbers, all_msg_rates = extract_rdma_write_data(file_paths)
    fig, ax1 = plt.subplots()

    colors = ['b']
    labels = ['Mellanox ConnectX-4']


    ax1.plot(all_qp_numbers[0], all_msg_rates[0], marker='o', label=f'{labels[0]}', color=colors[0])

    ax1.set_xlabel('QP Number')
    ax1.set_ylabel('Message Rate (Mops/s)')
    ax1.tick_params(axis='y')
    ax1.legend(loc='upper right')

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_6.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")
def mode_7(file_paths):
    all_qp_numbers, all_msg_rates = extract_rdma_write_data(file_paths)
    fig, ax1 = plt.subplots()

    colors = ['b']
    labels = ['RNIC in Gem5 (Head-blocked)']

    ax1.plot(all_qp_numbers[0], all_msg_rates[0], marker='o', label=f'{labels[0]}', color=colors[0])

    ax1.set_xlabel('QP Number')
    ax1.set_ylabel('Message Rate (Mops/s)')
    ax1.tick_params(axis='y')
    ax1.legend(loc='upper right')

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_7.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
def mode_8(file_paths):
    qp_numbers_write,_, latencies_write,bandwidth_write  = extract_rdma_write_data6(file_paths)
    
    for i in range(len(qp_numbers_write)):
        combined = list(zip(qp_numbers_write[i], latencies_write[i], bandwidth_write[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            qp_numbers_write[i][j] = combined[j][0]
            latencies_write[i][j] = combined[j][1]
            bandwidth_write[i][j] = combined[j][2]

    print(qp_numbers_write)
    fig, ax1 = plt.subplots(figsize=(12, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red','tab:purple']
    labels = ['Bandwidth-64 Bytes', 'Bandwidth-128 Bytes', 'Bandwidth-256 Bytes','Bandwidth-512 Bytes']


    ax1.set_xlabel('QP Number',fontsize=24)
    ax1.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax1.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax1.set_ylabel('Bandwidth (MBps/s)',fontsize=24)
    ax1.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小

    for i in range(len(file_paths)):
        ax1.plot(qp_numbers_write[i],bandwidth_write[i], color=colors[i], marker='o', label=labels[i], linewidth=3)
    fig.legend(loc='upper right', bbox_to_anchor=(0.9, 0.88), ncol=2,fontsize=20)

    # ax1.set_xlabel('QP Number',fontsize=24)
    # ax1.xaxis.set_major_formatter(FuncFormatter(format_k))
    # ax1.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    # ax1.set_ylabel('Latency (us)',fontsize=24)
    # ax1.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小
    # for i in range(len(file_paths)):
    #     ax1.plot(qp_numbers_write[i],latencies_write[i], color=colors[i], marker='o',label=labels[i], linewidth=3)
    
    # fig.legend(loc='lower right', bbox_to_anchor=(0.85, 0.1), ncol=1,fontsize=24)

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_8.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")

def mode_9(file_paths):
    qp_numbers_write,_, latencies_write,bandwidth_write  = extract_rdma_write_data6(file_paths)
    
    for i in range(len(qp_numbers_write)):
        combined = list(zip(qp_numbers_write[i], latencies_write[i], bandwidth_write[i]))
        combined.sort(key=lambda x: x[0], reverse=True)
        for j in range(len(combined)):
            qp_numbers_write[i][j] = combined[j][0]
            latencies_write[i][j] = combined[j][1]
            bandwidth_write[i][j] = combined[j][2]


    fig, ax1 = plt.subplots(figsize=(12, 8))

    colors = ['tab:blue', 'tab:green', 'tab:red','tab:purple','tab:orange']
    labels = ['RNIC in Gem5 (no-Headblock)', 'Mellanox ConnectX-5', 'Mellanox ConnectX-6','SRNIC','TCP']


    ax1.set_xlabel('QP Number',fontsize=24)
    ax1.xaxis.set_major_formatter(FuncFormatter(format_k))
    ax1.tick_params(axis='x', labelsize=24)  # 设置x轴刻度标签大小
    ax1.set_ylabel('Bandwidth (MBps/s)',fontsize=24)
    ax1.tick_params(axis='y', labelsize=24)  # 设置x轴刻度标签大小


    for i in range(len(file_paths)):
        ax1.plot(qp_numbers_write[i],bandwidth_write[i], color=colors[i], marker='o', label=labels[i], linewidth=3)
    fig.legend(loc='upper right', bbox_to_anchor=(0.9, 0.88), ncol=2,fontsize=20)

    output_dir = "output_plots"
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "mode_9.png")
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Plot saved to: {output_path}")


def main():
    if len(sys.argv) < 2:
        print("Please provide mode parameter: 1 or 2")
        return

    mode = int(sys.argv[1])

    if mode == 1:
        file_path = "record-V1.5-final_QP_CACHE_CAP300RECAP64.txt"
        mode_1(file_path)
    elif mode == 2:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP200RECAP0.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP1.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP4.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP8.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP16.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP64.txt"
        ]
        mode_2(file_paths)
    elif mode == 3:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP100RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP300RECAP32.txt"
        ]
        mode_3(file_paths)
    elif mode == 4:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP200RECAP0.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP1.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP4.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP8.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP16.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP64.txt"
        ]
        mode_4(file_paths)
    elif mode == 5:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP100RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP200RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP300RECAP32.txt"
        ]
        mode_5(file_paths)
    elif mode == 6:
        file_paths = [
            "record-mellanox_connectx-4.txt",
        ]
        mode_6(file_paths)
    elif mode == 7:
        file_paths = [
            "record-partoffcfs_usedformode7.txt",
        ]
        mode_7(file_paths)
    elif mode == 8:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP300RECAP32.txt",
            "record-V1.5-final_QP_CACHE_CAP300RECAP32_long.txt",
            "record-V1.5-final_QP_CACHE_CAP300RECAP32longlong.txt",
            "record-V1.5-final_QP_CACHE_CAP300RECAP32verylong.txt"
        ]
        mode_8(file_paths)    
    elif mode == 9:
        file_paths = [
            "record-V1.5-final_QP_CACHE_CAP300RECAP32verylongformode9.txt",
            "record-mellanox-cx5.txt",
            "record-mellanox-cx6.txt",
            "record-SRNIC.txt",
            "record-TCP.txt"
        ]
        mode_9(file_paths)    
    else:
        print("Invalid mode parameter")

if __name__ == "__main__":
    main()