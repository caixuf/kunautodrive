# 御书房 · 第一阁：数理公理与微物理推导谱

---

## 1. 严格 12-6 兰纳-琼斯势能与解析力场方程 (Lennard-Jones 12-6 Potential & Force Field)

### 1.1 势能定义
兰纳-琼斯势能（Lennard-Jones 12-6 Potential）精确刻画了非键结粒子间近距离泡利斥力与中距离范德华引力的平衡：

$$V_{\text{LJ}}(r) = 4\varepsilon \left[ \left(\frac{\sigma}{r}\right)^{12} - \left(\frac{\sigma}{r}\right)^6 \right]$$

- $\varepsilon$：势阱深度（Potential Well Depth），控制组织聚集吸引的刚度；
- $\sigma$：零势平衡碰撞直径（Collision Diameter），当 $r = \sigma$ 时，$V_{\text{LJ}}(\sigma) = 0$。

### 1.2 解析微分与多体相互作用力推导
根据经典力学，相互作用力为势能的负梯度：$\mathbf{F}(\mathbf{r}) = -\nabla V(r) = -\frac{dV}{dr} \frac{\mathbf{r}}{r}$。

对标量距离 $r$ 求导：

$$\frac{dV}{dr} = 4\varepsilon \left[ -12 \frac{\sigma^{12}}{r^{13}} + 6 \frac{\sigma^6}{r^7} \right] = -\frac{24\varepsilon}{r} \left[ 2\left(\frac{\sigma}{r}\right)^{12} - \left(\frac{\sigma}{r}\right)^6 \right]$$

代入得到 3D 向量相互作用力解析式：

$$\mathbf{F}_{\text{LJ}}(\mathbf{r}) = \frac{24\varepsilon}{r^2} \left[ 2\left(\frac{\sigma}{r}\right)^{12} - \left(\frac{\sigma}{r}\right)^6 \right] \mathbf{r}$$

### 1.3 物理极值与相变临界点
令标量力 $F(r) = 0$：

$$2\left(\frac{\sigma}{r}\right)^{12} = \left(\frac{\sigma}{r}\right)^6 \implies \left(\frac{\sigma}{r}\right)^6 = \frac{1}{2} \implies r_m = 2^{1/6}\sigma \approx 1.122462\,\sigma$$

- **强排斥区**（$r < r_m$）：$F > 0$（表现为剧烈斥力，防止细胞重叠和拓扑塌缩）；
- **势阱吸引区**（$r_m < r < r_{\text{cut}}$）：$F < 0$（表现为范德华引力，驱动功能相关细胞凝聚成器官）；
- **远距离无相互作用区**（$r > r_{\text{cut}} = 3\sigma$）：力场平滑截断至 0。

---

## 2. 半经典 WKB 量子势垒隧穿积分 (Wentzel-Kramers-Brillouin Barrier Tunneling Integral)

### 2.1 物理薛定谔方程背景
对于处于一维势阱中的量子波函数 $\psi(x)$：

$$-\frac{\hbar^2}{2m} \frac{d^2\psi}{dx^2} + V(x)\psi = E\psi$$

当粒子能量 $E < V(x)$ 时，在势垒区 $[x_1, x_2]$ 内动量为虚数 $p(x) = i\sqrt{2m(V(x)-E)}$。

### 2.2 WKB 势垒穿透系数积分
在半经典近似下，波函数穿透势垒的隧穿透射系数为：

$$T_{\text{WKB}} \approx \exp\left( -2 \int_{x_1}^{x_2} \frac{\sqrt{2m(V(x) - E)}}{\hbar} dx \right)$$

### 2.3 在演化空间中的离散化映射
在 FlowEngine 演化拓扑空间中：
- 势垒宽度 $L = \| \Delta \mathbf{w} \|_{\text{basin}}$（当前停滞盆地与全局最优盆地的参数/拓扑距离）；
- 势垒相对高度 $\Delta V = V_{\text{barrier}} - E = \max(0.1, (t_{\text{stagnant}} - 50) \times \gamma)$；
- 空间相干辐射场强度 $I_{\text{radiation}} = |\Psi(\mathbf{r})|^2$ 提供额外共振能量，降低有效作用量：

$$P_{\text{tunnel}} = \exp\left( - \frac{2 L \sqrt{2 m_{\text{eff}} \Delta V}}{\hbar_{\text{eff}} \cdot (1.0 + \alpha I_{\text{radiation}})} \right)$$

---

## 3. 广义洛特卡-沃尔泰拉多物种营养级动力学 (Generalized Lotka-Volterra Dynamics)

### 3.1 种群能量微分方程
设第 $i$ 个物种的能量密度为 $E_i$，其时间演化遵循带捕食与放牧项的微分方程：

$$\frac{dE_i}{dt} = r_i E_i + \sum_{j=1}^N \alpha_{ij} E_i E_j - m_i E_i$$

- $r_i$：内禀生长率（做市商从市场养分池吸收流动性）；
- $\alpha_{ij}$：物种间交互矩阵：
  - $\alpha_{\text{Herbivore}, \text{Producer}} > 0$（初级消费者吸收做市商挂单流动性）；
  - $\alpha_{\text{Predator}, \text{Herbivore}} > 0$（套利掠食者猎杀单腿暴露）；
  - $\alpha_{ij} = -\alpha_{ji}$（能量守恒封闭转移）；
- $m_i$：基础代谢消耗率。

### 3.2 香农生物多样性指数健康度
$$H = -\sum_{i=1}^4 p_i \ln(p_i), \quad p_i = \frac{N_i}{\sum_{k=1}^4 N_k}$$
理论最大平衡多样性 $H_{\max} = \ln(4) \approx 1.38629$。

---

## 4. 微观市场冲击成本平方根定律 (Square-Root Impact Law)

依据 Almgren-Chriss 与 Bouchaud 理论，大单撮合瞬时冲击成本与委托量和可用深度的平方根成正比：

$$\Delta P_{\text{impact}} = \gamma \cdot P_{\text{base}} \cdot \sqrt{\frac{V_{\text{order}}}{V_{\text{available}}}}$$

实际执行撮合成交均价：

$$P_{\text{exec}} = P_{\text{base}} + \text{sign}(\text{direction}) \cdot \Delta P_{\text{impact}}$$

---

## 5. 资金会计恒等式穿透平账模型 (Balance Invariant Formulation)

系统任一时刻的净资产权益（Total Equity）必须严格满足守恒公理：

$$\text{CurrentEquity} \equiv \text{InitialBalance} + \sum_{k=1}^M \text{RealizedPnL}_k - \sum_{k=1}^M \text{Commission}_k + \text{UnrealizedFloatingPnL}$$

对账误差断言：

$$|\text{CurrentEquity} - \text{CalculatedEquity}| < 10^{-4} \text{ RMB}$$
