# HTTP API 与配置

## 接口

| 方法与路径 | 用途 | 返回/状态 |
| --- | --- | --- |
| `GET /` | 离线仪表盘 | HTML；此页面不需要 token，数据接口需要 |
| `GET /healthz` | 采集与存储健康状态 | `{"healthy":true}`，正常 200，异常或无首次采样 503 |
| `GET /api/v1/status` | 最新样本与运行统计 | JSON，接口正常响应为 200，具体健康状态见 healthy |
| `GET /api/v1/samples?limit=50` | 已提交样本，id 降序 | JSON 数组 |
| `GET /api/v1/alarms?limit=50` | 已提交告警/恢复事件，id 降序 | JSON 数组 |
| `GET /metrics` | Prometheus 0.0.4 文本格式 | 本次运行计数与当前状态 |

`limit` 缺省 50，只允许一个参数，整数范围 1～200；非法参数为 400。未知路径 404，非 GET 为 405。重复 HTTP 头、带请求体、缺少 HTTP/1.1 Host、Transfer-Encoding 均拒绝。请求头超限为 431；空闲超时/超过连接容量直接断开。此服务器不支持 keep-alive、TLS、HTTP/2、分页游标和数据修改。

启用 `EDGE_API_TOKEN` 后，除 `/` 外均需：

```http
Authorization: Bearer <token>
```

鉴权失败返回 401。没有 CORS 放行。页面的令牌只放在 JS 内存，不使用 localStorage 或 cookie；使用 HTTPS 代理或 SSH 隧道传输。

## 数据语义

样本字段：

```json
{"id":123,"timestamp_ms":1792252800000,"temperature_c":31.2,"humidity_pct":52.4,"alarm":true}
```

数据库样本的 `id` 是跨重启递增的 SQLite id。`status.latest` 是最近成功采集的内存快照，其 `id=0` 表示尚未关联持久化 id，不能用于查询数据库记录。历史接口只返回已提交数据，因此可能暂时落后于最新状态。

事件字段：

```json
{"id":3,"sample_id":123,"timestamp_ms":1792252800000,"active":true,"temperature_c":31.2}
```

`active=true` 表示进入高温告警，`false` 表示恢复。样本与事件可能因各自行数保留策略独立过期。

`status` 字段：

| 字段 | 含义 |
| --- | --- |
| source / connected | simulate/modbus；模拟模式恒为连接，串口模式表示设备 fd 是否打开 |
| healthy / stale | 样本存在、未过期、设备连接且最近写入未失败时健康；过期阈值为 max(3×采集间隔, 2×响应超时) |
| acquired / persisted | 本次运行采集成功数/提交成功数；二者不一定同时增长 |
| queue_depth / storage_dropped | 待取出的写任务数/未能入队的任务总数 |
| storage_ok / storage_errors | 最近一次写入是否成功/失败任务总数 |
| timeouts / crc_errors / malformed_frames | RTU 超时/CRC 错误/解码器丢弃的非法片段计数 |
| modbus_exceptions | 有效 Modbus 异常响应数 |
| serial_failures / serial_reconnects | 串口打开或 I/O 失败数/首次成功之后重新打开成功数 |
| uptime_ms / latest | 单调时钟计算的运行时长/最新数据，首次成功前为 null |

`healthy` 并不保证没有历史丢弃；应同时监测存储丢弃和错误计数增长。`malformed_frames` 会按丢弃片段或字节增加，不能当作精确的总线帧数。所有运行计数重启清零，数据库历史保留。

## 配置表

纯 `key=value`；仅整行 `#` 注释，没有行尾注释、引号转义或配置热更新。重复/未知键会报错。修改后重启进程。

| 键 | 默认 | 约束/说明 |
| --- | --- | --- |
| source | simulate | simulate 或 modbus |
| serial_device | /dev/ttyUSB0 | 串口路径，真实设备优先使用稳定设备名 |
| baud / parity | 9600 / N | 波特率 9600/19200/38400/57600/115200；N/E/O，8 数据位、1 停止位 |
| unit | 1 | 从站地址 1～247 |
| register_start | 0 | 0～65534，连续读取两个输入寄存器 |
| interval_ms | 1000 | 50～3600000，采集完成/超时前不会发下一请求 |
| timeout_ms | 300 | 20～10000，包含请求发送时间，考虑设备响应时间设置 |
| reconnect_ms | 1000 | 100～60000 |
| bind / port | 127.0.0.1 / 8080 | IPv4 字面量；端口 1～65535 |
| database | gateway.db | 持久化文件路径，相对路径基于进程工作目录；父目录必须存在且可写 |
| queue_capacity | 256 | 1～4096 个待写样本 |
| max_rows | 100000 | 1～10000000；分别限制样本和告警记录数 |
| alarm_high | 30 | -40～125°C，达到此值告警 |
| alarm_hysteresis | 2 | (0,50]°C，降至 high-hysteresis 时恢复 |

`EDGE_API_TOKEN` 只从环境变量读取，不接受配置项。非 127.0.0.0/8 地址监听时要求 16～256 个无空格可打印 ASCII 字符。回环地址也可以启用 token。`--check` 校验配置和环境，不打开数据库/串口；不能代替运行验证。
