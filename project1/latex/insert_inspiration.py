import re

with open('/home/cells/school/net/project1/latex/report.tex', 'r', encoding='utf-8') as f:
    content = f.read()

new_section = r"""
\section{协议设计启发：与 CAN 总线的对比}
CSMA/DCR 协议在设计目标与应用场景上与工业界广泛使用的 CAN（Controller Area Network）总线协议有着高度的相似性，但其底层的核心实现机制与突破点有着本质的区别。

\subsection{设计思想的传承}
两者都致力于解决传统 CSMA/CD 在高负载下因频繁碰撞和“随机退避”导致的延迟不可控问题。在发生竞争时，它们都摒弃了基于概率的随机重传，转而采用基于节点物理 ID 或优先级的确定性排队机制，从而满足工业控制、车载网络等场景对“硬实时（Hard Real-time）”的严苛要求。

\subsection{核心机制的区别与突破}
尽管目标一致，但两者在处理冲突的机制上截然不同，这也赋予了 CSMA/DCR 独特的优势：
\begin{itemize}
	\item \textbf{仲裁机制与冲突性质：} CAN 总线采用\textbf{非破坏性的逐位仲裁（Bitwise Arbitration）}。依靠物理层的“线与”逻辑，优先级高的报文在发送头部 ID 阶段直接覆盖低优先级报文，过程无停顿；而 CSMA/DCR 采用\textbf{破坏性碰撞与微时隙排队}。首次碰撞会破坏报文，但随后通过 Jam 信号叫停全网，立即切换到基于 ID 的确定性时隙排队（类似 TDMA）。
	\item \textbf{状态切换机制：} CAN 总线始终处于单一的竞争与仲裁状态中；CSMA/DCR 则是一种双状态切换协议——在低负载下保持纯粹 CSMA 的低延迟竞争状态，在冲突后瞬间平滑切换至高吞吐的确定性排队状态。
	\item \textbf{物理层带宽瓶颈的突破（核心优势）：} CAN 总线的逐位仲裁要求极苛刻的物理层同步（必须在 $1$ 个比特时间内让全网达成电平共识），这导致其速率受到严重限制（经典 CAN 最高仅 1Mbps），无法在长距离上实现高带宽。CSMA/DCR 避开了逐位仲裁，其微时隙设计只要求大于端到端的传播时延。因此，\textbf{CSMA/DCR 可以轻松部署在百兆甚至千兆的高速以太网物理层上}。它在提供极高带宽的同时，完美继承了类似 CAN 总线的确定性。
\end{itemize}
综上所述，CSMA/DCR 协议在思想上借鉴了 CAN 的确定性，但在机制上是对以太网 CSMA/CD 的重大改良，它用时间片排队代替了电平排队，是一种极具潜力的高速确定性网络协议。
"""

content = content.replace(r'\section{理论计算与可行性分析}', new_section + '\n' + r'\section{理论计算与可行性分析}')

with open('/home/cells/school/net/project1/latex/report.tex', 'w', encoding='utf-8') as f:
    f.write(content)
