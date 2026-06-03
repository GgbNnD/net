import re

with open('/home/cells/school/net/project1/latex/report.tex', 'r', encoding='utf-8') as f:
    content = f.read()

# Remove the previously added section (from \section{协议设计启发 to right before \section{理论计算与可行性分析})
pattern_to_remove = re.compile(r'\\section\{协议设计启发：与 CAN 总线的对比\}.*?(?=\\section\{理论计算与可行性分析\})', re.DOTALL)
content = pattern_to_remove.sub('', content)

# Define the new Section 2
new_section_2 = r"""\section{设计启发：从 CAN 总线到 CSMA/DCR}
本协议的设计灵感直接来源于对工业界广泛使用的 CAN（Controller Area Network）总线与传统以太网（CSMA/CD）底层机制的优劣势思考。

在传统的工业控制与车载网络中，CAN 总线因其基于 ID 的非破坏性逐位仲裁（Bitwise Arbitration）机制，能够保证重要报文的绝对确定性与极低延迟，完美契合“硬实时”需求。然而，逐位仲裁要求极苛刻的物理层同步——全网必须在 1 个比特的时间内达成电平共识。这一物理限制导致 CAN 的通信速率受限（经典 CAN 仅 1Mbps），难以满足现代智能网络（如高清视频流、高频雷达数据）对高带宽的渴求。

另一方面，传统以太网拥有百兆乃至千兆的带宽潜力，但其冲突后的“随机退避”机制会导致高负载下延迟不可控。

\textbf{如何兼得以太网的高带宽与 CAN 总线的确定性？} 这便是提出 CSMA/DCR 协议的核心出发点。我们意识到：
\begin{enumerate}
	\item 要突破带宽瓶颈，就必须放弃 CAN 的“逐位仲裁”；
	\item 要解决延迟不可控，就必须放弃以太网的“随机退避”。
\end{enumerate}

由此，我们构想出一种融合机制：在常态下，允许像以太网一样发生碰撞以换取无开销的高带宽；但在碰撞发生后，\textbf{借鉴 CAN 总线的 ID 优先级思想，将其从“电平域”转移到“时间域”}。即通过发送 Jam 信号统一叫停全网，随后按节点 ID 分配“微时隙（Micro-slots）”进行轮询。这种“先碰撞，后排队”的策略，使得 CSMA/DCR 既彻底摆脱了逐位仲裁的物理速率限制，又在网络拥塞时获得了与 CAN 总线等效的确定性调度能力。

"""

# Insert the new Section 2 right before \section{协议设计与流程分析}
content = content.replace(r'\section{协议设计与流程分析}', new_section_2 + r'\section{协议设计与流程分析}')

with open('/home/cells/school/net/project1/latex/report.tex', 'w', encoding='utf-8') as f:
    f.write(content)
