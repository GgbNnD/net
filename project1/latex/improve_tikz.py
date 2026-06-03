import re

with open('/home/cells/school/net/project1/latex/report.tex', 'r', encoding='utf-8') as f:
    content = f.read()

new_tikz = r"""
\tikzstyle{startstop} = [rectangle, rounded corners, minimum width=2.8cm, minimum height=0.8cm,text centered, draw=black, fill=red!20, font=\small\sffamily]
\tikzstyle{process} = [rectangle, minimum width=2.8cm, minimum height=0.8cm, text centered, draw=black, fill=orange!20, inner sep=4pt, font=\small\sffamily]
\tikzstyle{decision} = [diamond, aspect=2, minimum width=2.8cm, minimum height=0.8cm, text centered, draw=black, fill=green!20, inner sep=0pt, font=\small\sffamily]
\tikzstyle{arrow} = [thick,->,>=stealth, rounded corners]

\begin{figure}[H]
	\centering
	\resizebox{0.48\textwidth}{!}{
		\begin{tikzpicture}[node distance=1.8cm and 2.2cm]
			\node (start) [startstop] {节点有数据发送};
			\node (sense) [decision, below=0.8cm of start] {信道空闲?};
			\node (wait) [process, left=1.0cm of sense] {持续侦听};
			\node (send) [process, below=0.8cm of sense] {开始发送数据};
			\node (col) [decision, below=0.8cm of send] {检测到冲突?};
			\node (jam) [process, right=1.0cm of col] {发送Jam并停止};
			\node (backoff) [process, above=1.8cm of jam] {二进制指数退避};
			\node (success) [startstop, below=0.8cm of col] {发送成功};

			\draw [arrow] (start) -- (sense);
			\draw [arrow] (sense) -- node[anchor=south] {否} (wait);
			\draw [arrow] (wait.north) |- ([yshift=0.6cm]sense.north) -- (sense.north);
			\draw [arrow] (sense) -- node[anchor=east] {是} (send);
			\draw [arrow] (send) -- (col);
			\draw [arrow] (col) -- node[anchor=east] {否} (success);
			\draw [arrow] (col) -- node[anchor=south] {是} (jam);
			\draw [arrow] (jam) -- (backoff);
			\draw [arrow] (backoff.north) |- ([yshift=0.6cm]sense.north) -- (sense.north);
		\end{tikzpicture}
	}
	\hfill
	\resizebox{0.48\textwidth}{!}{
		\begin{tikzpicture}[node distance=1.8cm and 2.2cm]
			\node (start) [startstop] {节点有数据发送};
			\node (sense) [decision, below=0.8cm of start] {信道空闲?};
			\node (wait) [process, left=1.0cm of sense] {等待解决期结束};
			\node (send) [process, below=0.8cm of sense] {开始发送数据};
			\node (col) [decision, below=0.8cm of send] {检测到冲突?};
			\node (jam) [process, right=1.0cm of col] {发送Jam};
			\node (dcr) [process, below=0.6cm of jam] {进入确定性排队};
			\node (send_dcr) [process, below=0.6cm of dcr] {微时隙发送};
			\node (success) [startstop, below=0.8cm of col] {发送成功};

			\draw [arrow] (start) -- (sense);
			\draw [arrow] (sense) -- node[anchor=south] {否/冲突解决期} (wait);
			\draw [arrow] (wait.north) |- ([yshift=0.6cm]sense.north) -- (sense.north);
			\draw [arrow] (sense) -- node[anchor=east] {是} (send);
			\draw [arrow] (send) -- (col);
			\draw [arrow] (col) -- node[anchor=east] {否} (success);
			\draw [arrow] (col) -- node[anchor=south] {是} (jam);
			\draw [arrow] (jam) -- (dcr);
			\draw [arrow] (dcr) -- (send_dcr);
			\draw [arrow] (send_dcr.west) |- (success.east);
		\end{tikzpicture}
	}
	\caption{左图：传统 CSMA/CD 协议流程；右图：新型 CSMA/DCR 协议流程}
\end{figure}
"""

old_tikz_pattern = re.compile(r'\\tikzstyle\{startstop\}.*?\\end\{figure\}', re.DOTALL)
new_content = old_tikz_pattern.sub(new_tikz.strip().replace('\\', '\\\\'), content)

with open('/home/cells/school/net/project1/latex/report.tex', 'w', encoding='utf-8') as f:
    f.write(new_content)
