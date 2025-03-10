import matplotlib
matplotlib.use('TkAgg')  # 使用 TkAgg 后端
import matplotlib.pyplot as plt
import re

# 设置全局字体大小
plt.rcParams.update({
    'font.size': 16,          # 全局字体大小
    'axes.titlesize': 18,     # 标题字体大小
    'axes.labelsize': 16,     # 坐标轴标签字体大小
    'xtick.labelsize': 14,    # X 轴刻度字体大小
    'ytick.labelsize': 14,    # Y 轴刻度字体大小
    'legend.fontsize': 14,    # 图例字体大小
})

# 从文件中提取 RDMA WRITE 和 RDMA READ 部分的数据
def extract_rdma_data(file_path):
    qp_numbers_write = []
    msg_rates_write = []
    latencies_write = []
    qp_numbers_read = []
    msg_rates_read = []
    latencies_read = []

    with open(file_path, 'r') as file:
        content = file.read()

        # 使用正则表达式匹配 RDMA WRITE 部分的数据
        rdma_write_blocks = re.findall(
            r"(\d+) QPs \* \d+ clients \* \d+ CPU_NUM = (\d+)\s*RDMA WRITE[\s\S]*?msg_rate\s+([\d.]+) Mops/s[\s\S]*?latency\s+([\d.]+) us",
            content
        )

        for block in rdma_write_blocks:
            qp_numbers_write.append(int(block[1]))
            msg_rates_write.append(float(block[2]))
            latencies_write.append(float(block[3]))

        # 使用正则表达式匹配 RDMA READ 部分的数据
        rdma_read_blocks = re.findall(
            r"(\d+) QPs \* \d+ clients \* \d+ CPU_NUM = (\d+)\s*RDMA READ[\s\S]*?msg_rate\s+([\d.]+) Mops/s[\s\S]*?latency\s+([\d.]+) us",
            content
        )

        for block in rdma_read_blocks:
            qp_numbers_read.append(int(block[1]))
            msg_rates_read.append(float(block[2]))
            latencies_read.append(float(block[3]))

    return (qp_numbers_write, msg_rates_write, latencies_write,
            qp_numbers_read, msg_rates_read, latencies_read)

# 文件路径
file_path = "record-V1.5-final_QP_CACHE_CAP300RECAP32.txt"

# 提取数据
(qp_numbers_write, msg_rates_write, latencies_write,
 qp_numbers_read, msg_rates_read, latencies_read) = extract_rdma_data(file_path)

# 将数据按 QP 数升序排列
def sort_data(qp_numbers, msg_rates, latencies):
    sorted_data = sorted(zip(qp_numbers, msg_rates, latencies), key=lambda x: x[0])
    return [x[0] for x in sorted_data], [x[1] for x in sorted_data], [x[2] for x in sorted_data]

qp_numbers_write, msg_rates_write, latencies_write = sort_data(qp_numbers_write, msg_rates_write, latencies_write)
qp_numbers_read, msg_rates_read, latencies_read = sort_data(qp_numbers_read, msg_rates_read, latencies_read)

# 打印提取的数据
print("QP Numbers (WRITE):", qp_numbers_write)
print("Message Rates (WRITE, Mops/s):", msg_rates_write)
print("Latencies (WRITE, us):", latencies_write)
print("QP Numbers (READ):", qp_numbers_read)
print("Message Rates (READ, Mops/s):", msg_rates_read)
print("Latencies (READ, us):", latencies_read)

# 创建图形和轴
fig, ax1 = plt.subplots(figsize=(12, 8))  # 设置图形大小

# 绘制 RDMA WRITE msg_rate 曲线
color = 'tab:blue'
ax1.set_xlabel('QP Number', fontsize=16)
ax1.set_ylabel('Message Rate (Mops/s)', color=color, fontsize=16)
ax1.plot(qp_numbers_write, msg_rates_write, color=color, marker='o', label='WRITE Message Rate', linewidth=3)
ax1.tick_params(axis='y', labelcolor=color, labelsize=14)
ax1.tick_params(axis='x', labelsize=14)

# 设置左侧纵坐标起始位置为 20
ax1.set_ylim(bottom=20)

# 绘制 RDMA READ msg_rate 曲线
color = 'tab:green'
ax1.plot(qp_numbers_read, msg_rates_read, color=color, marker='^', label='READ Message Rate', linewidth=3)

# 创建第二个 y 轴，绘制 RDMA WRITE latency 曲线
ax2 = ax1.twinx()
color = 'tab:red'
ax2.set_ylabel('Latency (us)', color=color, fontsize=16)
ax2.plot(qp_numbers_write, latencies_write, color=color, marker='s', label='WRITE Latency', linewidth=3)
ax2.tick_params(axis='y', labelcolor=color, labelsize=14)

# 绘制 RDMA READ latency 曲线
color = 'tab:purple'
ax2.plot(qp_numbers_read, latencies_read, color=color, marker='D', label='READ Latency', linewidth=3)

# 横坐标保持线性比例（不拉伸）
ax1.set_xscale('linear')

# 添加图例，调整位置到右下角
fig.legend(loc='lower right', bbox_to_anchor=(0.95, 0.1), fontsize=14)

# 显示图形
plt.title('Message Rate and Latency vs QP Number (RDMA WRITE & READ)', fontsize=18)
plt.tight_layout()  # 自动调整布局
plt.show()