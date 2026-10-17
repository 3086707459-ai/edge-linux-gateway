# 部署与维护

## 原生构建与安装

先按 README 构建测试；目标板是 ARM 时，应在板上构建或使用兼容 SDK 的交叉编译产物。确认目标系统具有匹配的 SQLite 和 C++ 运行库：

```bash
file build/edge-gateway
ldd build/edge-gateway
sudo sh deploy/install.sh build/edge-gateway
sudo /usr/local/bin/edge-gateway --config /etc/edge-gateway/gateway.conf --check
sudo systemctl enable --now edge-gateway
systemctl status edge-gateway
journalctl -u edge-gateway -f
```

安装脚本创建专用系统用户、持久化目录和服务；已有配置会保留，二进制和 service 文件会更新。不会自动启动服务。默认安装配置仍为模拟模式，数据库位于 `/var/lib/edge-gateway/gateway.db`。

service 使用非 root 用户、只允许状态目录写入、关闭能力集，并由 systemd 在异常退出后重启。`TimeoutStopSec=90` 允许排空队列，持续存储阻塞超过此时限时仍会被强制终止。

真实串口按硬件文档设置；安装脚本在 dialout 组存在时会把服务用户加入该组。若发行版使用其他设备组，需设置对应补充组并重启服务。service 没有启用 `PrivateDevices`，因为应用需要访问串口。

升级前 `systemctl stop edge-gateway`，确认停止后替换二进制；保留数据库和配置，再启动观察。当前仅支持 schema version 1，遇到其他版本会拒绝启动，没有自动降级或跨版本迁移工具。

## 远程查看

保持 `bind=127.0.0.1`，在自己的电脑建立 SSH 隧道：

```bash
ssh -L 8080:127.0.0.1:8080 user@device
```

本机浏览器打开 <http://127.0.0.1:8080>。如果本机 8080 被占用，可使用 `-L 18080:127.0.0.1:8080` 并打开本机 18080。

需要局域网监听时将 bind 设成对应 IPv4 地址或 0.0.0.0，并在 `/etc/edge-gateway/environment` 中设置 `EDGE_API_TOKEN=...`，文件权限 0600；使用足够长的随机值，不要提交进仓库。配置非回环监听而未提供至少 16 字符 token 时，程序启动失败。

此 HTTP 服务不提供 TLS；Bearer token 直接通过明文 HTTP 传输时仍可能泄露。面向远程网络应使用正确配置的 HTTPS 反向代理，或继续采用 SSH/VPN；应用端口只向需要的网络开放。

## Docker 演示

Dockerfile 提供可复现的构建方式，依赖 Debian 官方仓库。在支持 Docker 的 Linux 主机：

```bash
docker build -t edge-linux-gateway .
docker volume create edge-gateway-data
docker run --rm --name edge-gateway --network host \
  -v edge-gateway-data:/data edge-linux-gateway
```

默认回环监听配合 Linux host network。不要在这种默认配置下仅使用 `-p 8080:8080`，因为应用监听容器内部回环地址，端口映射不能代替绑定配置。Docker Desktop 的 host network 可用性依环境而异，优先直接用 Linux/WSL 构建演示。真实串口建议先原生部署；容器下设备透传和组权限需另行配置。

容器进程 UID/GID 为 10001，命名卷首次使用时可继承镜像目录权限。已有 bind mount 需要保证目录能由 10001 读写。此文档没有把 Docker 运行或 ARM 上板标记为已测，见测试报告。

## 备份与容量

不要在运行期间仅复制 `.db` 文件，WAL 中可能还有已提交记录。任选一种方式：

```bash
# 停服务后复制；停服务期间不采集
sudo systemctl stop edge-gateway
sudo cp -a /var/lib/edge-gateway /path/to/backup/edge-gateway
sudo systemctl start edge-gateway

# 或安装 sqlite3 CLI，使用 SQLite 在线备份 API（路径按实际修改）
sqlite3 /var/lib/edge-gateway/gateway.db ".backup '/path/to/backup/gateway.db'"
```

默认保留 100000 条采样，1 Hz 时约 27.8 小时；采样和事件各自有限额。频繁 FULL 提交会产生存储写入负担。闪存部署需要测量写放大、预留容量和介质寿命；如需更高吞吐，可设计批量事务，同时明确批量窗口内断电丢失边界。

## 故障排查

| 现象 | 检查 |
| --- | --- |
| 启动提示 database lock | 是否有另一个实例、父目录是否存在且可写、数据库路径是否正确 |
| HTTP bind/listen failed | 端口占用、绑定地址是否在本机、低端口权限 |
| 已打开串口但无采样 | 从站地址、功能码、寄存器映射、波特率、校验、A/B、设备供电 |
| crc_errors 增长 | 波特率/校验配置、线路干扰、适配器方向控制、传感器响应帧 |
| storage_errors 增长 | 磁盘空间、只读文件系统、SQLite 外部写锁及 I/O 日志 |
| queue_depth/丢弃增长 | 存储延迟、采样频率；先减慢采样，再分析存储线程 |
| 网页 401 | 是否启用了 EDGE_API_TOKEN；在页面填写同一令牌 |
| healthy=false | 首次采样尚未完成、数据过期、串口未连接、最近写入失败 |

使用 `top`、`pidstat`、`strace -f -p PID`、`lsof` 等工具定位问题。测试命令不能代替本设备的 CPU/RSS/延迟测量。
