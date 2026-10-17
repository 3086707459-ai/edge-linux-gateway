# 验证记录与复现

## 本次已执行

验证批次：2026-10-18（Asia/Shanghai）。环境为 Linux x86_64 开发容器、GCC 13.3.0、CMake 4.4.4、SQLite 3.53.1。容器没有系统 SQLite 开发包，本次从 SQLite 官方 amalgamation 构建临时静态库，通过 CMake 的 `SQLite3_INCLUDE_DIR` / `SQLite3_LIBRARY` 指向它；仓库不携带第三方依赖源码或二进制。正常 Ubuntu/Debian 使用发行版的 `libsqlite3-dev` 即可。

| 验证 | 结果 |
| --- | --- |
| Debug，严格编译警告 + Werror | 编译通过 |
| Release，严格编译警告 + Werror | 编译通过 |
| C++ 单元测试 | 52 项检查通过；使用运行期 check，不受 NDEBUG 影响 |
| Debug CTest | 2/2 通过：core 与 integration |
| Release CTest | 2/2 通过 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 2/2 通过；本环境关闭 LeakSanitizer，见下文 |
| 配置检查 --check | 通过 |
| Python 语法 / shell 语法 / dashboard JS 语法 | 通过 |
| 本机 HTTP 延迟与 RSS 快照 | 运行 5 秒，原始值见 local-measurement.json |

## 自动化覆盖

单元测试涵盖：标准 CRC 向量、RTU 请求字节序、负温度、部分帧、CRC 后恢复、异常帧、越界值、连续帧、噪声缓存边界、告警滞回、恢复持久化告警状态、队列满/关闭/唤醒、HTTP 请求校验、配置异常、SQLite 排空/保留/重启 id。

集成测试启动真正的网关子进程，通过 TCP、SQLite 与 Linux PTY 验证：

- 模拟采集、历史、告警进入/恢复、保留行数、重启后 id 递增。
- HTTP 分片请求、40 次请求的并发批次、慢请求过期、请求头大小、非法参数与非 GET。
- token 鉴权、非回环监听、页面不泄露 token。
- 第二个进程被数据库锁拒绝。
- PTY 上传感器响应分片、CRC 错误、丢帧超时、Modbus 异常、断开/新建 PTY/稳定链接重连、负温度。
- 外部 SQLite 写事务持锁导致提交失败，健康状态 503；释放锁后恢复，错误运行退出码为 2。
- SIGKILL 后 SQLite integrity_check 为 ok，已确认提交的记录仍存在，重新启动继续采集。

**SIGKILL 恢复验证不等于物理断电验证**：内核页缓存和存储电源仍存在。没有以该测试推断真实掉电零丢失。

## 复现

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure

cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DEDGE_SANITIZE=ON
cmake --build build-sanitize -j2
ctest --test-dir build-sanitize --output-on-failure
```

受限容器可能报 `LeakSanitizer ... Can't open /proc/.../task`。本次就是该环境限制，重跑命令为：

```bash
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

此时地址越界与未定义行为检查仍启用，**未完成泄漏扫描**。SQLite 临时静态库自身没有插桩，sanitizer 结论仅覆盖应用中插桩的代码及运行时可检测行为。仓库的 GitHub Actions 没有关闭泄漏扫描，需以实际远端执行结果为准。

自测性能：在另一个终端运行网关后，取得 PID 再执行：

```bash
python3 tools/measure.py --pid <gateway-pid> --duration 30
```

工具每约 50 ms 发起一次状态请求，报告请求耗时与 `/proc/PID/status` 的最大采样 VmRSS；不是饱和压测，也没有测量 CPU 使用率。附带 5 秒快照仅用来证明工具可运行，不作为 ARM 板卡性能、长期资源上限或产品吞吐承诺。

## 尚待实际环境验收

ARM64 交叉编译、真实开发板和 USB-RS485、物理掉电、串口电气时序、长期稳定性、闪存磨损、磁盘满、Docker 构建/运行、systemd 安装，以及 GitHub Actions 的远端执行都需要相应环境验证。配置和脚本已提供，但不把“已提供”写成“已通过”。前端已完成接口和 JS 语法验证，尚未进行多浏览器视觉验收。
