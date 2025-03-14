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

# 从文件中提取 RDMA WRITE 部分的数据
def extract_rdma_write_data(file_path):
    qp_numbers = []
    msg_rates = []

    with open(file_path, 'r') as file:
        content = file.read()

        # 使用正则表达式匹配 RDMA WRITE 部分的数据
        rdma_write_blocks = re.findall(
            r"(\d+) QPs \* \d+ clients \* \d+ CPU_NUM = (\d+)\s*RDMA WRITE[\s\S]*?msg_rate\s+([\d.]+) Mops/s",
            content
        )

        for block in rdma_write_blocks:
            qp_numbers.append(int(block[1]))  # QP 数量
            msg_rates.append(float(block[2]))  # 消息速率

    return qp_numbers, msg_rates

# 文件路径
file_path_1 = "record-V1.5-final_QP_CACHE_CAP300RECAP64.txt"
file_path_2 = "record-V1.5-final_QP_CACHE_CAP300RECAP32.txt"

# 提取两个文件中的 RDMA WRITE 数据
qp_numbers_1, msg_rates_1 = extract_rdma_write_data(file_path_1)
qp_numbers_2, msg_rates_2 = extract_rdma_write_data(file_path_2)

# 将数据按 QP 数升序排列
def sort_data(qp_numbers, msg_rates):
    sorted_data = sorted(zip(qp_numbers, msg_rates), key=lambda x: x[0])
    return [x[0] for x in sorted_data], [x[1] for x in sorted_data]

qp_numbers_1, msg_rates_1 = sort_data(qp_numbers_1, msg_rates_1)
qp_numbers_2, msg_rates_2 = sort_data(qp_numbers_2, msg_rates_2)

# 打印提取的数据
print("QP Numbers (File 1):", qp_numbers_1)
print("Message Rates (File 1, Mops/s):", msg_rates_1)
print("QP Numbers (File 2):", qp_numbers_2)
print("Message Rates (File 2, Mops/s):", msg_rates_2)

# 创建图形和轴
fig, ax = plt.subplots(figsize=(12, 8))  # 设置图形大小

# 绘制 File 1 的 RDMA WRITE msg_rate 曲线
color_1 = 'tab:blue'
ax.set_xlabel('QP Number', fontsize=16)
ax.set_ylabel('Message Rate (Mops/s)', fontsize=16)
ax.plot(qp_numbers_1, msg_rates_1, color=color_1, marker='o', label='WRITE Message Rate (File 1)', linewidth=3)

# 绘制 File 2 的 RDMA WRITE msg_rate 曲线
color_2 = 'tab:green'
ax.plot(qp_numbers_2, msg_rates_2, color=color_2, marker='^', label='WRITE Message Rate (File 2)', linewidth=3)

# 设置纵坐标起始位置
ax.set_ylim(bottom=0)

# 设置横坐标为线性比例
ax.set_xscale('linear')

# 添加图例
ax.legend(loc='upper left', fontsize=14)

# 显示图形
plt.title('RDMA WRITE Message Rate Comparison', fontsize=18)
plt.tight_layout()  # 自动调整布局
plt.show()