# Edge Linux Gateway · 嵌入式 Linux 设备采集网关

面向**嵌入式 Linux 应用开发岗**的完整工程：在 Linux 网关上采集 Modbus RTU 温湿度传感器数据，进行高温告警与本地持久化，通过 HTTP API 和离线网页供运维人员查看。适合作为机柜、实验室环境、设备间监测的项目起点。

主体使用 **C++17 + Linux/POSIX + SQLite + CMake**。没有开发板也可以完整演示；接入协议匹配的传感器后可使用同一程序采集。这里的“工业”指应用场景，不代表已通过工业认证或现场可靠性验收。

[![Build and test](https://github.com/3086707459-ai/edge-linux-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/3086707459-ai/edge-linux-gateway/actions/workflows/ci.yml)

## 快速运行

Ubuntu/Debian、树莓派 64 位系统等 Linux 环境；GCC 9+、CMake 3.16+。Windows 用户可使用 WSL2 的模拟模式；USB 串口接入需自行配置 USB 转发。

```bash
git clone https://github.com/3086707459-ai/edge-linux-gateway.git
cd edge-linux-gateway
sudo apt-get update
sudo apt-get install -y build-essential cmake libsqlite3-dev python3 curl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/edge-gateway --config config/gateway.conf --check
./build/edge-gateway --config config/gateway.conf
```

浏览器打开 <http://127.0.0.1:8080>。默认使用模拟数据，20 个样本一周期，温度在 24～33°C 之间变化，可观察高温告警和恢复。默认每秒采集一次；按 `Ctrl+C` 排空已接收的存储任务并退出。

另一个终端查看接口：

```bash
curl -s http://127.0.0.1:8080/api/v1/status | python3 -m json.tool
curl -s 'http://127.0.0.1:8080/api/v1/samples?limit=10'
curl -s http://127.0.0.1:8080/api/v1/alarms
curl -s http://127.0.0.1:8080/metrics
```

## 已实现的能力

| 能力 | 具体实现 |
| --- | --- |
| Linux 事件循环 | `epoll` LT 模式统一处理串口、HTTP、`timerfd` 定时、`signalfd` 退出 |
| 串口通信 | `termios`、非阻塞读写、部分写入处理、可配置波特率与 N/E/O 校验 |
| Modbus RTU | 功能码 `0x04`、CRC16、拆包/多帧、带符号温度、异常帧与非法值处理 |
| 故障恢复 | 响应超时、连续 3 次超时后重开串口、设备断开后定期重连 |
| 线程同步 | 独立数据库线程、互斥锁/条件变量、有界队列、关闭唤醒与任务排空 |
| 本地存储 | SQLite WAL、FULL 同步级别、样本/告警同事务、重启保留记录与告警状态 |
| 告警 | 高阈值触发、低阈值恢复，避免阈值附近反复告警 |
| 运维入口 | HTTP 只读 API、健康检查、Prometheus 文本指标、离线监控网页 |
| 资源边界 | 32 个 HTTP 客户端、4 KiB 请求头、2 秒请求/发送期限、每次最多 200 条历史 |
| 工程交付 | CMake、ARM64 工具链示例、systemd、Docker、CI 配置、自动化测试和中文文档 |

## 无硬件验证真实串口路径

模拟模式用于演示业务；下面的 PTY 传感器则真正经过 Linux 串口文件描述符、RTU 请求、CRC 和解码逻辑。

终端 A：

```bash
python3 tools/modbus_sensor.py --link /tmp/edge-sensor --fragment --bad-crc-every 4 --drop-every 5
```

终端 B：

```bash
cp config/gateway.conf /tmp/edge-pty.conf
sed -i 's/^source=.*/source=modbus/; s|^serial_device=.*|serial_device=/tmp/edge-sensor|' /tmp/edge-pty.conf
./build/edge-gateway --config /tmp/edge-pty.conf
```

请先停止之前的网关实例，避免端口或数据库锁冲突。PTY 传感器默认只模拟起始地址为 0 的两个输入寄存器。

## 工程结构

```text
include/edge/       RAII 描述符、协议、队列、存储、HTTP 与配置接口
src/               Linux reactor 与业务实现，内置离线监控页
config/            默认可运行配置
tools/             Python PTY 传感器、故障注入及实测采样工具
tests/             C++ 单元测试和 Python 进程级集成测试
deploy/            systemd 服务与目标板安装脚本
cmake/             ARM64 sysroot 工具链示例
docs/              架构、API、硬件接入、部署、求职学习及验证说明
.github/workflows/ GCC Debug/Release 与 ASan/UBSan 自动构建测试
```

## 接入硬件与上板

推荐从已有 Linux 开发板 + 带自动收发方向控制的 USB-RS485 适配器 + Modbus RTU 温湿度传感器开始。协议必须与[硬件接入说明](docs/hardware.md)中的寄存器表一致，其他设备需要修改映射。

在目标板原生编译最容易起步；已有 SDK 时可以交叉编译：

```bash
export EDGE_SYSROOT=/path/to/target-sdk/sysroot
export CROSS_COMPILE=aarch64-linux-gnu-
cmake -S . -B build-arm64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build-arm64 -j2
```

sysroot 必须包含 **ARM64 版本**的 SQLite 头文件、库及 C/C++ 运行库；不能链接主机 x86 库。目标板安装方式参见[部署文档](docs/deployment.md)。ARM 硬件运行和交叉编译需在你的实际开发板/SDK 上验证。

## 阅读路径与求职使用

1. [架构与设计取舍](docs/architecture.md)：线程、事件循环、数据生命周期和失败行为。
2. [API 与配置](docs/api.md)：配置项、响应语义及鉴权。
3. [实际硬件接入](docs/hardware.md)：接线、寄存器约定及现场检查。
4. [部署与维护](docs/deployment.md)：systemd、远程访问、备份和故障排查。
5. [验证与测试](docs/testing.md)：已测边界和未验收内容。
6. [学习与面试讲解](docs/interview.md)：理解顺序、动手改造和可核实的简历表述。

## 边界与扩展

当前针对单路串口、单个从站和固定的两个输入寄存器；HTTP 提供数据拉取，没有实现 MQTT、云端补传、远程控制、OTA、I²C/GPIO 或内核驱动。队列溢出、数据库写入失败或断电时，尚未提交的数据可能丢失，不能承诺端到端零丢失。数据库行数有上限，但数据库文件大小与闪存寿命仍需结合设备测量。

默认只监听本机。远程查看优先使用 SSH 隧道；改为非回环监听时必须配置至少 16 字符的 `EDGE_API_TOKEN`，并由 HTTPS 反向代理保护网络传输。详细说明见部署文档。

许可证：[MIT](LICENSE)。
