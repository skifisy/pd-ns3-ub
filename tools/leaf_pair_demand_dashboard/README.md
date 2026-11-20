# P–S / D–S 交换机对流量分析

独立的本地分析工具：读取已有 ECMP Replay 日志，生成 SQLite 缓存，通过 Python 服务提供交互网页。
**需要运行本地服务，不是双击 HTML 的纯静态网页。** 不修改 ns-3 转发逻辑，不要求提供 traffic.csv、task_statistics.csv 或性能汇总 CSV。

## 安装与使用

在仓库根目录执行：

```bash
python3 -m venv .venv-leaf-analysis
. .venv-leaf-analysis/bin/activate
pip install -r tools/leaf_pair_demand_dashboard/requirements.txt

python3 tools/leaf_pair_demand_dashboard/prepare.py \
  --case /path/to/ecmp_case \
  --out-dir /path/to/leaf_pair_analysis \
  --throughput-scale 100000

python3 tools/leaf_pair_demand_dashboard/server.py \
  --data /path/to/leaf_pair_analysis \
  --port 8000
```

访问 `http://localhost:8000`。`--case` 可指定 case 根目录（日志在 `replay/` 内），也可直接指定日志目录。
如果 `node.csv` / `topology.csv` 在其他目录，增加 `--topology /path/to/topology`。
导入过程会打印窗口数、载荷行数与已建索引的方向数；输入未变时再次 prepare 会复用缓存，`--force` 强制重建。
查询阶段只读缓存，不扫描原 CSV，也不导入任务数据。

`--throughput-scale` 默认 **1**；只有实验使用了对应缩放时才设置 100000。
该倍率同时乘到显示的发送速率和容量上，不改变时间、利用率、高负载持续时间或原始字节数。
例如两根 4 Mbps 并行链路，倍率 100000 时显示单方向容量 800 Gbps；不会将双向容量相加。

服务默认仅监听 `127.0.0.1`。日志在远程机器时，可在该机器上运行上述命令，再从本机转发端口：

```bash
ssh -N -L 8000:127.0.0.1:8000 user@simulation-host
```

也支持 `--host 0.0.0.0` 监听远程地址。网页和 ECharts 均由本地服务提供，不需要 CDN。
ECharts 复用仓库 `tools/replay_dashboard/static/vendor/`，请保留该目录及其中的许可证。
缓存成功重建后应重启服务；运行中的服务检测到缓存替换会要求重启，避免混用旧摘要和新数据。

## 角色与矩阵

默认按主机身份和实际接入连接识别 Leaf：主机 0–11 为 Prefill，12–15 为 Decode；其余非 SWITCH 且 `portNum != 64` 的节点为 Storage。
支持 node.csv 中的节点区间。不根据交换机编号猜测角色，也不将最后四台交换机固定认作 Storage。
默认 OCS 拓扑会得到 P37–P48、D49–D52、S53–S56，共 48 个 P–S 对和 16 个 D–S 对。
自动识别结果显示在图表标签中，请与实验配置核对。

主机角色编号不同可设置 `--prefill-nodes` / `--decode-nodes`。接入不符合该约定或一个 Leaf 同时接入不同角色时，明确指定 Leaf 集合：

```bash
python3 tools/leaf_pair_demand_dashboard/prepare.py \
  --case /path/to/ecmp_case --out-dir /path/to/analysis \
  --prefill-leaves 37-48 --decode-leaves 49-52 --storage-leaves 53-56 \
  --throughput-scale 100000
```

集合支持 `37-48`、`37..48`、`37,39,41-43`；空字符串可禁用 P 或 D 组。
三个 Leaf 集合均显式指定时不需要 node.csv / topology.csv，仍必须提供 replay_topology.csv。
每个 P / D 与每个 S 都会生成图，即使它们没有直连电路或没有流量，也保留零容量 / 零流量结果。

## 网页操作

- P–S、D–S 分区，默认按计算 Leaf 行、Storage Leaf 列排列；支持折叠、平均速率 / 峰值 / 高负载持续时间排序。
- 拖动顶部导航选区、输入精确开始 / 结束时间，或使用放大、缩小、平移、全程按钮；所有图共享同一时间范围。
- 任一小图滚轮以鼠标位置为中心缩放，按住鼠标拖动平移；鼠标时间参考线同步到其他可见图。
- 双向曲线分别展示，可仅看 P / D→S 或 S→P / D。单方向容量绘为橙色虚线，拓扑变化时按生效时间阶梯显示。
- 默认统一纵轴，方便横向比较；也可切换每图独立纵轴。全局最大值会包含未滚动到的小图。
- 实线是显示桶的平均速率，淡色线是桶内原始窗口峰值。全程视图保留突发峰值；放大后按需读取原始窗口。
- 热点色块展示全程最大原始窗口需求 / 直连容量比或物理利用率，点击跳到对应图。统计明细始终标为**全程统计**，不随选区改变。

## 数据口径与输入约束

| 输入 | 用途 |
|---|---|
| `replay_run.csv` | case 身份、采样间隔、载荷采集范围、记录起止时间 |
| `replay_topology.csv` | 每个拓扑 epoch 的真实有向物理端口和速率，累加并行链路容量 |
| `replay_link_samples.csv` | 每窗口物理发送字节与容量积分，支持 node_pair / physical_port 两种聚合 |
| `replay_node_path_samples.csv` | 可选；按实际 src_leaf→dst_leaf 汇总同窗口所有端点、中转路径和工作线程的业务载荷 |
| `node.csv` / `topology.csv` | 自动识别角色与接入关系；显式指定全部 Leaf 集合时可省略 |

所有输入支持 `.csv` / `.csv.gz` 和 UTF-8 BOM。不会读取 `.tr` 或已有 Replay Dashboard 的缓存。

**业务载荷发送速率**是源 Leaf 成功送入 Fabric 的 OD 观测发送量，包含重传；不是无背压的潜在需求，也不是目的端去重交付速率。
**物理链路吞吐**包含协议开销、重传和该链路上其他 OD 的流量。业务载荷 / 直连容量比不能称为物理链路利用率。
源、目的 Leaf 的方向按实际数据包方向保留，不因 READ 请求的发起方向反转。

载荷日志缺失、关闭采集或 `detail_windows` 不是 `all` 时，仅开放物理吞吐，并明确提示载荷不可用，不把未采集区间填成零需求。
全程载荷日志中没有记录的有效采样窗口按零发送量统计；若运行元数据报告未归属路径数据包，会提示载荷可能偏低。
物理窗口必须连续、无重叠且按时间排列，同一窗口的全部物理记录必须相邻；载荷记录可乱序。
重复物理端口 / 节点对、混合聚合方式、缺失电路记录、容量与速率不符、跨拓扑生效边界、窗口缺口及与运行起止时间不符均会报错。
导入失败不会替换已完成的缓存。缺少完整 run 元数据时，只能验证文件中实际给出的窗口，不能证明日志没有被整体截短。

时间以原日志纳秒字段转换为整数皮秒，保留事件拆分的亚纳秒边界；不强制重采样成 10 ms。
速率为 `8 × bytes / 实际窗口时长`；每个粗桶同时保存字节和、容量积分、最小 / 最大容量和原始峰值。
所有峰值都先按 OD / 物理节点对汇总同一原始窗口，再取最大值，不对不同端点或不同时刻的峰值相加。
空闲窗口进入均值、P95 / P99 统计；分位数按窗口时长加权，避免很短的事件窗口获得与完整窗口相同的权重。
高负载持续时间为原始窗口比率 ≥80% / ≥90% 的时长之和；零容量时利用率无定义，表格显示“—”。

时间范围查询仅在整个汇总桶都落入选区时采用该桶；两端递归读取更细的数据，防止选区外的峰值混入。
若选区切入一个原始窗口，曲线保留该窗口的平均速率和峰值，只裁剪其横轴范围；不推断窗口内更细的实际发送过程。

缓存按四倍窗口数逐级聚合，以索引查询；每方向返回点数通常不超过 `points + O(log 窗口数)`。
原始窗口、零流量样本及多级索引保存在磁盘；Python 内存主要为窗口索引和单方向统计数组，不载入全部载荷行。
浏览器仅初始化可见区域附近的图表，移出区域后销毁实例；每方向按图宽请求有限数据点，并取消过期的缩放查询。
还未对真实 3000 万任务日志进行性能实测；缓存空间随窗口数和交换机对数量增长，与任务总数不直接对应。

## 验证

无需运行 ns-3。安装服务依赖后：

```bash
pip install httpx
python3 -m unittest discover -s tools/leaf_pair_demand_dashboard/tests -p 'test_*.py'
```

测试使用明确标记的合成 CSV，覆盖双向容量、端口 / 节点对聚合一致性、事件短窗口、空闲分位数、峰值、范围裁剪、gzip、缓存复用、导入失败保留旧缓存与 HTTP API。
可选浏览器检查（需要 Playwright 和 Chromium）：

```bash
python3 tools/leaf_pair_demand_dashboard/tests/browser_smoke.py
```

该检查生成 64 对合成数据，验证页面加载、统一缩放、滚轮、拖动平移、方向 / 指标切换、排序和容量显示。
