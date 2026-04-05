# Smart LBSS 运行指南

## 环境现状

已确认可用：
- Python 3.10 + 虚拟环境 `.venv/`（依赖已安装）
- MySQL 8.0（运行中，root 有密码）
- Mosquitto MQTT（运行中）
- Contiki-NG（`~/contiki-ng`）
- Java 21（Cooja 模拟器依赖）

---

## 第零步：安装 MySQL 和 Mosquitto（WSL Ubuntu 22.04）

> **注意**：WSL 默认不使用 systemd 作为 init 系统，因此 `systemctl` 命令不可用，需要使用 `service` 命令来管理服务。

### 0.1 安装 MySQL

```bash
sudo apt update
sudo apt install mysql-server
```

### 0.2 启动 MySQL

```bash
sudo service mysql start
```

验证状态：

```bash
sudo service mysql status
```

### 0.3 安装 Mosquitto

```bash
sudo apt install mosquitto mosquitto-clients
```

### 0.4 启动 Mosquitto

Mosquitto 启动时可能报错 `unable to open pidfile`，需要先创建 pidfile 目录：

```bash
sudo mkdir -p /run/mosquitto
sudo chown mosquitto:mosquitto /run/mosquitto
sudo service mosquitto start
```

验证状态：

```bash
sudo service mosquitto status
```

### 0.5 设置开机自启（可选）

每次启动 WSL 时自动启动这两个服务，可以在 `~/.bashrc` 末尾添加：

```bash
sudo service mysql start >/dev/null 2>&1 &
sudo mkdir -p /run/mosquitto && sudo chown mosquitto:mosquitto /run/mosquitto && sudo service mosquitto start >/dev/null 2>&1 &
```

---

## 第零步（续）：安装 Contiki-NG（如果还没有）

> **重要**：Contiki-NG 使用 git submodule 管理 Cooja 模拟器等组件。克隆时必须使用 `--recursive` 参数，否则 submodule 不会被下载，导致 Cooja 无法运行（报错 `build.xml does not exist!`）。

### 正确做法（推荐，使用 SSH 协议）

> 前提：已将本机 SSH 公钥上传到 GitHub。

```bash
cd ~
git clone --recursive git@github.com:contiki-ng/contiki-ng.git
```

### 备选做法（HTTPS 协议）

```bash
cd ~
git clone --recursive https://github.com/contiki-ng/contiki-ng.git
```

### 错误做法（不要使用）

```bash
# 不要这样做！--depth=1 不会拉取 submodule，会导致 Cooja 等组件缺失
git clone --depth=1 git@github.com:contiki-ng/contiki-ng.git
```

### 如果已经 clone 了但缺少 submodule

可以补救，手动初始化缺失的 submodule：

```bash
cd ~/contiki-ng
git submodule update --init --recursive
```

> 注意：`--recursive` 会下载较多内容（几百 MB），但能确保所有组件完整可用。

---

## 第一步：配置 MySQL 数据库

MySQL root 用户有密码，需要创建一个项目专用的数据库用户。

### 1.1 登录 MySQL

```bash
sudo mysql
```

### 1.2 在 MySQL 中执行以下命令

```sql
CREATE USER 'lbss'@'localhost' IDENTIFIED BY '12345678';
GRANT ALL PRIVILEGES ON *.* TO 'lbss'@'localhost' WITH GRANT OPTION;
CREATE DATABASE IF NOT EXISTS ugrid;
FLUSH PRIVILEGES;
EXIT;
```

### 1.3 验证登录

```bash
mysql -u lbss -p12345678 -e "SHOW DATABASES;"
```

应该能看到 `ugrid` 数据库。

---

## 第二步：修改 RCA 配置文件

编辑 `RCA/rca.py`，找到第 23-28 行的 MySQL 配置：

```python
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "",
    "port": 3306,
}
```

改为：

```python
MYSQL_CONFIG = {
    "host": "localhost",
    "user": "lbss",
    "password": "12345678",
    "port": 3306,
}
```

> 注意：当前 `rca.py` 中已经配置了密码 `12345678`，如果你创建的用户密码不同，需要保持一致。

---

## 第三步：启动 RCA（远程控制应用）

### 3.1 激活虚拟环境

```bash
cd ~/project/smart-lbss
source .venv/bin/activate
```

### 3.2 启动 RCA

```bash
cd RCA
python rca.py
```

启动成功后会看到类似输出：

```
[INFO] Database inizializzato
[INFO] Poll loop avviato (CoAPthon sync)
 * Running on http://0.0.0.0:3000
```

> **注意**：此时会持续报错 `Errore poll ugrid ug1`，这是正常的，因为还没有设备在运行。RCA 会每 5 秒尝试连接一次 CoAP 设备。

### 3.3 验证 RCA 是否工作

打开**另一个终端**，测试 API：

```bash
curl http://localhost:3000/api/status
```

应该返回 `{}`（空数据，因为没有设备连接）。

---

## 第四步：启动 CA（客户端应用）

保持 RCA 在第一个终端运行，打开**第二个终端**：

```bash
cd ~/project/smart-lbss
source .venv/bin/activate
cd CA
python client_application.py
```

会看到一个终端界面（TUI），显示电池状态面板。由于没有设备连接，会显示 "Nessun dato disponibile"（无可用数据）。

底部命令行可以输入命令，输入 `help` 查看可用命令，输入 `quit` 退出。

---

## 第五步（可选）：在 Cooja 中运行设备仿真

如果需要完整测试系统，需要在 Cooja 模拟器中运行嵌入式固件。

### 5.1 安装 Gradle（首次需要）

新版 Cooja 已从 Ant 迁移到 Gradle 构建系统，需要 **Gradle 8.x**。Ubuntu 自带的 Gradle 版本太老（4.x），无法使用。

有两种方式解决：

#### 方式一：使用 Gradle Wrapper（推荐，但需要网络下载）

Cooja 自带了 `gradlew` 脚本，会自动下载所需版本的 Gradle。但国内网络可能下载卡住。

**如果自动下载卡住，可以手动下载并放置：**

1. **手动下载 Gradle 8.14.2**
   - 官方地址：https://services.gradle.org/distributions/gradle-8.14.2-bin.zip
   - 国内镜像（更快）：https://mirrors.cloud.tencent.com/gradle/gradle-8.14.2-bin.zip
   - 或用浏览器/迅雷等工具下载

2. **创建目录并放置文件**

   ```bash
   # 创建目标目录（hash 目录名可能略有不同，先运行一次 gradlew 让它创建目录结构）
   mkdir -p ~/.gradle/wrapper/dists/gradle-8.14.2-bin
   
   # 将下载的 zip 文件复制到该目录
   # 注意：gradle 会自动解压，不需要手动解压
   cp ~/Downloads/gradle-8.14.2-bin.zip ~/.gradle/wrapper/dists/gradle-8.14.2-bin/
   ```

3. **重新运行**

   ```bash
   cd ~/contiki-ng/tools/cooja
   ./gradlew build
   ```

#### 方式二：手动安装 Gradle 8.x

1. **下载并安装 Gradle 8.14.2**

   ```bash
   # 下载（如果官方地址慢，用腾讯镜像）
   wget https://mirrors.cloud.tencent.com/gradle/gradle-8.14.2-bin.zip -P /tmp/
   
   # 解压到 /opt
   sudo unzip /tmp/gradle-8.14.2-bin.zip -d /opt/
   
   # 创建软链接
   sudo ln -sf /opt/gradle-8.14.2/bin/gradle /usr/local/bin/gradle
   
   # 验证版本
   gradle --version
   ```

2. **构建并启动 Cooja**

   ```bash
   cd ~/contiki-ng/tools/cooja
   gradle run
   ```

### 5.2 修复 Makefile 路径

项目中的 Makefile 默认 Contiki-NG 在相对路径 `../../../`，需要修改为绝对路径：

```bash
cd ~/project/smart-lbss
sed -i 's|CONTIKI = .*|CONTIKI = $(HOME)/contiki-ng|' \
    rpl-border-router/Makefile \
    uGridController/Makefile \
    BatteryController/Makefile
```

### 5.3 构建并启动 Cooja

```bash
cd ~/contiki-ng/tools/cooja
gradle run
```

首次运行会下载依赖，需要等待一段时间。构建完成后会弹出 Java GUI 窗口。

### 5.4 创建仿真

1. **File → New simulation**，名称填 `Smart-LBSS`

2. **添加 Border Router 节点**：
   - Motes → Add motes → **Cooja mode**
   - 在弹出窗口的 **Contiki process source** 处，选择 `~/project/smart-lbss/rpl-border-router/border-router.c`
   - 点击 **Compile** 等待编译完成
   - 编译成功后点击 **Create** 创建节点
   - 右键节点 → **Set IP address** → 设为 `fd00::1`

3. **添加 uGridController 节点**：
   - 同上，Motes → Add motes → **Cooja mode**
   - 选择 `~/project/smart-lbss/uGridController/ugrid_controller.c`

4. **添加 BatteryController 节点**（可以添加多个）：
   - 同上，Motes → Add motes → **Cooja mode**
   - 选择 `~/project/smart-lbss/BatteryController/battery_controller.c`

5. 点击工具栏的 **Start** 按钮（绿色播放图标）运行仿真

> **说明**：Cooja mode 会将代码编译为 x86_64 原生程序在宿主机上运行，无需 MSP430 交叉编译工具链。Sky mode 和 Z1 mode 需要 MSP430 工具链（版本 ≥ 4.7），Ubuntu 自带的 gcc-msp430 (4.6.3) 太旧，需要从 TI 官网下载新版。

### 5.6 启动 tunslip6 连接

在**另一个终端**中：

```bash
cd ~/contiki-ng/tools/serial-io
sudo ./tunslip6 fd00::1/64 -s /dev/ttyUSB0
```

> 注意：Cooja 中 border router 的串口设备路径可能不同，需要在 Cooja 中查看。

### 5.7 修改 RCA 的设备 IP

编辑 `RCA/rca.py` 第 37-40 行：

```python
UGRIDS = {
    "ug1": {
        "coap_state_uri": "coap://[fd00::f6ce:36ac:9afa:6be2]/dev/state",
    },
}
```

将 IP 地址改为 Cooja 中 uGridController 节点实际分配的 IPv6 地址。

---

## 常用操作

### 查看 RCA 日志

```bash
tail -f ~/project/smart-lbss/RCA/rca.log
```

### 停止 RCA

在运行 RCA 的终端按 `Ctrl+C`。

### 停止 CA

在 CA 界面中输入 `quit` 或按 `Ctrl+C`。

### 重新激活虚拟环境

每次打开新终端都需要：

```bash
cd ~/project/smart-lbss
source .venv/bin/activate
```

---

## 架构说明

```
┌─────────────────────────────────────────────┐
│  Cooja 模拟器（可选）                          │
│  Border Router → uGridCtrl → BatCtrl        │
│  (CoAP over 6LoWPAN/RPL)                     │
└──────────────────┬──────────────────────────┘
                   │ CoAP over IPv6
                   ▼
┌─────────────────────────────────────────────┐
│  RCA (RCA/rca.py)                            │
│  - Flask REST API :3000                      │
│  - MySQL 存储                                 │
│  - MQTT 告警                                  │
│  - CoAP 客户端（连接设备）                      │
└──────────────────┬──────────────────────────┘
                   │ HTTP REST
                   ▼
┌─────────────────────────────────────────────┐
│  CA (CA/client_application.py)               │
│  - 终端 TUI 界面                              │
│  - 实时监控 + 控制命令                         │
└─────────────────────────────────────────────┘
```

## 不使用 Cooja 时

只运行 RCA + CA 即可。RCA 会持续尝试连接设备并报错，这是正常的。CA 会显示无数据。

## 关于 ML 部分

`ML/` 目录下的 Jupyter Notebook 是**训练阶段**使用的，用于：
1. 训练电池健康预测模型（soh-prediction.ipynb）
2. 训练功率预测模型（power-prediction.ipynb）
3. 使用 emlearn 将模型导出为 C 头文件

导出的模型已经放在 `includes/battery_soh_model.h` 和 `includes/power_predictor_model.h` 中，嵌入式固件直接引用这些头文件进行推理，**不需要运行 Notebook**。

如果你想重新训练模型，可以在虚拟环境中运行：

```bash
source .venv/bin/activate
cd ML
jupyter notebook
```
