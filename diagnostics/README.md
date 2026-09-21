# Windows 10 已连接但没有按键输入：源码检查与诊断

检查日期：2026-09-21。结论：**尚未确认本次故障根因；配对成功、HID 枚举成功和可用的 Input Report 通路是三个不同条件。** 不支持将问题首先归因于 keyboard.inf，也不需要首先升级整个 ZMK。

## 版本和证据边界

- 本仓库 `config/west.yml` 和正式 Actions 均选择 ZMK `v0.3`。
- 本地 `.zmk/zmk` 干净，HEAD 为 `edf5c0814fd3ea202e43aad2d68fd32e882a518c`，标签 `v0.3`，提交日期 2025-08-01。
- 其 `app/west.yml` 指定 Zephyr `zmkfirmware/zephyr` 的 `v3.5.0+zmk-fixes`；这个引用不能代替具体构建的 SHA。
- 直接模块：nanopb `8c60555d6277a0360c876bd85d491fc4fb0cd74a`；zmk-studio-messages `6cb4c283e76209d59c45fbcb218800cd19e9339d`。其余模块由 Zephyr manifest 导入。
- 本地 west 配置排除了 Zephyr 等项目，没有最终 `.config`、完整依赖或已刷入固件的构建证据。不能断言设备当前运行的二进制就是本地 HEAD。
- GATT 实现额外核对了检查当日该 Zephyr 引用的 `subsys/bluetooth/host/gatt.c`、`Kconfig.gatt` 和公开头文件。GitHub API 限流，未取得该远端引用的具体 SHA。这部分结论需用实际 Actions 的 frozen manifest 复核。
- 新工作流保存 `resolved-manifest.yml`、`zephyr.config`、DTS、ELF、map、UF2 和实际应用的 diff；并校验 ZMK SHA，防止补丁误套到不同版本。

## 普通按键与配对码的路径

以下路径均相对于 ZMK 仓库：

```text
矩阵扫描驱动
  -> app/src/physical_layouts.c: zmk_physical_layout_kscan_callback / 扫描工作队列
  -> 矩阵坐标转 position，raise_zmk_position_state_changed
  -> app/src/keymap.c: zmk_keymap_position_state_changed
  -> zmk_keymap_apply_position_state -> zmk_behavior_invoke_binding
  -> app/src/behaviors/behavior_key_press.c: 按下/释放回调
  -> raise_zmk_keycode_state_changed_from_encoded
  -> app/src/hid_listener.c: hid_listener_keycode_pressed / released
  -> app/src/hid.c: zmk_hid_press / release，更新 modifier 和报告体
  -> app/src/endpoints.c: zmk_endpoints_send_report -> send_keyboard_report
  -> BLE 分支：zmk_hid_get_keyboard_report -> zmk_hog_send_keyboard_report
  -> app/src/hog.c: k_msgq_put -> hog_work_q
  -> send_keyboard_report_callback -> zmk_ble_active_profile_conn
  -> bt_gatt_notify_cb(conn, params)
  -> Zephyr subsys/bluetooth/host/gatt.c: gatt_notify
  -> bt_att_create_pdu(BT_ATT_OP_NOTIFY) -> bt_att_send -> L2CAP/HCI/控制器
```

HOG 发送的是 keyboard report **body**，不包含 USB 使用的前置 report ID；Report Reference descriptor 标识对应 report ID。不能给 BLE body 随意补 report ID。

配对码走 `app/src/ble.c` 的 `zmk_ble_handle_key_user`：收集数字键释放事件，Enter 时调用 `bt_conn_auth_passkey_entry()`，完成 SMP 认证。这条路径不依赖 HOG Input Report 的 CCC 或 notification 成功。认证监听器返回 HANDLED 会终止后续监听器；具体先后顺序还受链接后的订阅表影响，不能未经 map 核对就把所有 HID 消失都归因于它。

因此“能输入配对码 + HID Keyboard Device 存在 + 普通键无输入”完全可能：前两项分别证明认证和枚举取得进展，并不证明订阅恢复、输出路由、发送或 Windows 对通知的处理正常。

## 已确认的源码行为

1. `endpoints.c:is_ble_ready()` 只检查 active profile 是否处于 CONNECTED，没有检查安全等级或 keyboard CCC。设备显示已连接时，HOG 完全可能尚不可用。
2. `hog.c:host_requests_notification` 仅由多个报告共用的 CCC 回调写入，没有任何读取者，**不会阻止发送**。回调 value 还是聚合状态，不能代替 `bt_gatt_is_subscribed(conn, keyboard_attr, BT_GATT_CCC_NOTIFY)`。
3. 当前 Zephyr 引用默认 `BT_GATT_ENFORCE_SUBSCRIPTION=y`。未订阅时 `gatt_notify()` 返回 `-EINVAL`；不能把该错误一律解读为坏参数。最终默认值需由 `zephyr.config` 确认。
4. `zmk_hog_send_keyboard_report()` 返回 0 只代表入队并尝试提交工作。真正的 notify 结果异步产生：`-EPERM` 请求 L2 加密但不检查请求结果；其他错误只 LOG_DBG；失败报告没有重新入队。`hid_listener()` 还丢弃内部 pressed/released 函数的返回值。
5. `zmk_ble_active_profile_conn()` 每次按 active profile 的 peer 地址 lookup 并获得引用，发送后 unref；没有发现 HOG 长期缓存失效 conn 指针。队列只存报告体、不存目标 profile，因此切换 profile 与排队并发时有发送到新目标的可能，但不能据此解释固定 profile 下持续无输入。
6. 入队后找不到 active conn 时，已取出的报告被丢弃并退出当前工作；队列满时丢弃最旧报告。这能解释短暂丢键，但持续无输入必须寻找持续失败条件。
7. `ble.c` 存储 ZMK profile 地址、active slot；Zephyr 另行保存 bond 和 CCC。`main.c` 调用 settings 初始化与加载。Zephyr CCC 恢复按 identity/peer 和 attribute handle 匹配，源码存在“handle 找不到”和“无可用 CCC cfg”的分支。固件 GATT 布局变化、单边清 bond 或存储失败有必要排查。
8. `security_changed()` 只记日志；不重发失败报告。`pairing_complete()` 更新 profile 地址；原代码没有注册 pairing_failed 诊断。passkey Enter 正常提交后会清理认证连接引用，因此成功输入 Enter 后一直被 passkey 状态截获并非首选解释。
9. `write_ctrl_point()` 只保存字段，没有让发送因 suspend 值而停住；没有实现 BLE Boot Keyboard/Protocol Mode 路径，不能假设存在一个可切换的 boot-report 发送器。

## 工程配置审查

| 项目 | 观察与影响 |
| --- | --- |
| USB/BLE | board defconfig 同时启用。USB HID ready 时默认优先 USB；keymap 没有 `&out OUT_BLE`。插线抓日志可能改变普通键输出，但不妨碍 BLE 配对码输入。 |
| Passkey | 已有 `CONFIG_ZMK_BLE_PASSKEY_ENTRY=y`，不能再将“启用 passkey”当成新修复。 |
| Settings/NVS | board 启用 NVS、SETTINGS_NVS、FLASH 等；ZMK_BLE implies SETTINGS/BT_SETTINGS。未发现显式禁用。storage 区间 `[0xec000,0xf4000)` 为 32 KiB，与代码及 bootloader 分区不重叠。 |
| SMP/peripheral | ZMK_BLE selects BT_SMP、SC_PAIR_ONLY、APP_PAIRING_ACCEPT、PERIPHERAL。 |
| 连接和配对容量 | 非 split 的 ZMK 默认 MAX_CONN=5、MAX_PAIRED=5；没有本项目覆盖。只给 3 个 slot 放快捷键不代表底层只有 3 个。 |
| HID | 未见 NKRO/扩展报告等自定义覆盖；源码 choice 首项为 HKRO，默认 6 键。尚无最终 `.config`，不把推导默认值当作实测配置。 |
| GATT | ZMK 默认 NOTIFY_MULTIPLE=n；此非 split 工程 AUTO_SEC_REQ 默认关闭。CCC 读取/写入要求加密。 |
| PHY/privacy | 没有本项目 PHY 2M、privacy、SMP overwrite 或实验选项覆盖。2M 的实际值需看 `.config`。 |
| 自定义代码 | 电量传感器和状态 LED，没有自定义 HID/notify 发送。LED 只根据 profile open/connected 状态判断，不证明 HOG 就绪。 |

正式 build.yaml 另有 settings_reset 固件，它与 OMM 正常固件是不同构建目标；不要把重置固件作为日常测试固件。

## 假设优先级与日志判据

目前优先级较高的是 Windows/Realtek 会话与订阅恢复问题：重启 Windows 曾恢复，而蓝牙开关和服务重启无效，与此方向相容，但不足以区分 CCC、控制器或 Windows HID 消费路径。

| 故障时日志 | 下一步判断 |
| --- | --- |
| 没有 physical_layouts 的行列/position 事件 | 先查扫描、供电、休眠，不先查 notify。 |
| 有 position/behavior，缺少 HID event | 检查 layer/behavior 和事件监听器截获；结合 passkey-capture 日志与 map。 |
| `endpoint=USB` | 普通报告走 USB；查输出优先级。诊断配置禁用 USB HID 以排除此干扰。 |
| active 与 Windows peer-profile 不一致 / 无 active conn | 查当前 slot、profile settings 和地址解析。其他 host 已连接不能代替 active host 就绪。 |
| `subscribed=0`、notify `-EINVAL` | 优先查 keyboard CCC 的写入/恢复；不要只看 consumer CCC 回调。 |
| `sec=1` 或 `-EPERM` | 查加密、bond、安全回调，以及 security-request 返回值。原版未返回 EPERM 不能证明已加密，原因见下面独立修正。 |
| `-ENOTCONN` | 发送时连接并非 CONNECTED；查断开时间、原因与 profile 切换。 |
| `-EAGAIN` | 检查 BT readiness；只有启用了 ENFORCE_CHANGE_UNAWARE 等相关条件才进一步归因于缓存同步。 |
| `-ENOMEM` / queue-full / submit<0 | 查 ATT buffer、队列或工作线程；不要用盲目增大 buffer 替代定位。 |
| notify-attempt 后无结果 | 检查工作线程阻塞、缓冲等待或日志丢失。 |
| 正确 peer、BLE 输出、sec>=2、subscribed=1、result=0、tx-complete 持续出现 | 发送已到本地协议栈/TX 完成阶段；进一步抓空口或 Windows 蓝牙日志。notification 没有 ATT 应答，TX callback 也不证明 Windows 已产生键盘输入。 |

上述订阅和安全状态是日志采样时刻的快照，可能与发送之间发生变化。日志本身会影响时序；完整 debug 会记录键码，复现请只输入测试字符，不要输入真实密码。

## 最小诊断补丁与 Actions 使用

`zmk-v0.3-ble-diagnostics.patch` 修改 upstream 的 5 个文件：

- `app/src/hog.c`：CCC handle/聚合值、按连接查询订阅、目标地址/slot/state/security、notify 返回值、本地 TX completion、security request 返回值、keyboard queue/submit 结果。
- `app/src/ble.c`：连接/断开及 reason、安全变化、pairing complete/bonded/failed、passkey capture/submission、profile 保存结果。
- `app/src/endpoints.c`：每个 keyboard report 的实际输出 endpoint。
- `app/src/hid_listener.c`：按下/释放处理结果。
- `app/src/main.c`：settings 初始化和加载结果。

扫描、behavior、HID usage 等沿用已有 DEBUG 日志，不额外逐字节打印报告。没有升级版本、清除存储、强制订阅或重试发送。补丁文件位于正式仓库，`.zmk/zmk` 本地 checkout 保持不变。

新工作流 `.github/workflows/ble-diagnostics.yml` 只支持手动触发，沿用 v0.3 官方构建容器及 module 参数：

1. 将本次改动提交并推送到 GitHub；工作流进入默认分支后，在 Actions 中选择 **Build BLE diagnostic firmware → Run workflow**。目前未自动提交、推送或触发远端构建。
2. 下载 **OMM-BLE-diagnostics** artifact，保留 `resolved-manifest.yml` 和 `zephyr.config`。只有 Actions 成功才能视为编译验证通过。
3. 刷该 artifact 的 `zmk.uf2`，打开新增 USB CDC 串口保存日志。诊断配置 `CONFIG_ZMK_USB=n` 只禁用 ZMK USB HID 输出，USB logging 会选择 USB device stack，保留串口；蓝牙报告格式没有因此改变。
4. 先录一次正常连接，按/放 A；故障后在重启/清配对前录同样操作，再分别比较键盘重启、Windows 重启、最后才做双端清配对。每次只改变一个条件。
5. 重点比较 `BLE-DIAG` 行，以及 Zephyr 的 CCC restore/write 日志。若开机早期串口日志没接到，不要把缺失日志当作未执行；重连后的每次发送仍会打印实际订阅状态。
6. 诊断完毕刷回正常构建。不要长期使用高频 DEBUG 固件。

工作流会应用诊断 patch，而不是只设置日志开关；原 `.github/workflows/build.yml` 和 `build.yaml` 没有改变。

本地验证：两份 patch 对目标 v0.3 均通过 `git apply --check --whitespace=error-all`，Actions YAML 可解析。**未执行 GitHub Actions，未完成固件编译或硬件复现。**

## 单独的权限检查修正：不是已证实的故障修复

`zmk-v0.3-hog-value-permissions.patch` 只有三行行为改动：notification 的属性从 `hog_svc.attrs[5/9/13]` 改为 `[6/10/14]`，即 keyboard/consumer/mouse 的 characteristic value。

依据：GATT 宏会为每个 characteristic 产生 declaration 和 value 两个属性。原属性是 declaration，权限只有 READ。核对的 Zephyr `bt_gatt_notify_cb()` 会把 declaration 转成 value handle 发送，却仍将原始 `params->attr` 交给 `gatt_notify()` 的加密权限检查。因而 value 上的 READ_ENCRYPT 没有在该检查处生效。传入 value 不改变线上报告 handle/格式，却能使该权限检查真正检查 value。实际发包还有其他协议栈安全约束，不能将这里的缺口夸大成已证实的空口明文传输。

这是已核对源码组合中的权限检查缺口；**尚不能证明它触发了本次持续无输入**。它也不会自动修复 CCC、Windows 状态或重发首次失败报告。因此诊断工作流默认不应用这份 patch。拿到实际 Zephyr SHA 并确认逻辑相同后，可先保存基线日志，再在独立实验中应用它；若安全等级长期不足，比较 EPERM → security request → security_changed → 后续按键的行为。

没有足够证据提供一个声称“已修复 Windows 无输入”的 patch。

## 外部资料与后续受控实验

- [ZMK 官方连接排障](https://zmk.dev/docs/troubleshooting/connection-issues)明确记录 USB 输出优先级、Windows 已连接但无输入，以及部分 Windows Realtek/Intel 的 PHY 2M 兼容性问题。当前已启用 passkey；可在诊断基线之后单独测试 `CONFIG_BT_CTLR_PHY_2M=n`，不要同时修改实验安全选项、清 bond 和升级固件。
- [本次核对的 Zephyr GATT 源码引用](https://github.com/zmkfirmware/zephyr/blob/v3.5.0%2Bzmk-fixes/subsys/bluetooth/host/gatt.c)；[订阅检查 Kconfig](https://github.com/zmkfirmware/zephyr/blob/v3.5.0%2Bzmk-fixes/subsys/bluetooth/host/Kconfig.gatt)。这些是引用链接，实际构建请以 artifact 中冻结的提交号为准。

下一项最有价值的证据是故障时连续按/放 A 的日志，以及相同构建正常时的对照，尤其是 **endpoint、peer/profile、security、keyboard subscription、notify result** 五项。

## 首轮实机结果与 1M PHY 对照

用户提供的实际配置确认：2M PHY=y，USB HID=n，USB logging=y，两个开机重置选项均未启用，CCC lazy loading/store-on-write 均启用。启动日志标识 Zephyr build `dacab4875df7`。

首次日志显示恢复 slot 0 的主机地址，约 6.81 秒连接到该主机，约 6.85 秒安全等级升至 4、HOG CCC 恢复；数据库 hash 与保存值一致。约 6.884 秒后用户观察到日志停止、Windows 短暂已连接后断开。没有捕获到断开回调或 fault，不能宣称已证明 MCU 死锁，也不能宣称 bond 被清除。

Windows 蓝牙关闭并重启键盘后，三次 A 的按下/释放共六个事件都经过扫描、HID 更新、BLE:0 报告入队和工作队列；因为没有连接而丢弃报告属于预期行为。用户随后开启 Windows 蓝牙，再次出现短暂连接后断开、无新增日志。因此后续优先隔离连接触发的问题，同时保留诊断日志或 USB 通道自身异常的可能。

工作流新增 `disable_2m` 复选框，默认 false 保留基线。勾选后只增加 `CONFIG_BT_CTLR_PHY_2M=n`，不改日志、安全选项或配对数据。构建完成会检查最终 `.config` 确实禁用 2M，artifact 名为 `OMM-BLE-diagnostics-1M`。该变体尚待 Actions 编译和硬件对照；此前日志没有实际 PHY 协商记录，故这只是有官方兼容性依据的实验，不能视为已经定位 2M 故障。

测试：刷 1M 变体，Windows 蓝牙先关闭，确认本地按键日志正常，再开启蓝牙观察连接和三次 A 的结果。保留旧配对和同样的 USB/串口工具。若仍失败，下一个独立实验应隔离同一 Windows 上的 USB 日志路径或诊断开销；不要同时清 bond 或叠加其他修复。

## 后续观察：1M 与纯电池仍失败，增加异常检测

1M 测试仍出现同样现象；连续按 A 的另一段日志首次捕获到 `sec=1 subscribed=1 len=8 result=0` 的 notify 结果，随后加密成功事件出现，日志中途截断。没有 TX completion/fault/disconnect 证据，不能把 notify 返回 0 当作 Windows 已接收，也不能断言未加密发送就是断开根因。

用户进一步确认：完全拔掉 USB、纯电池重启后仍短暂连接再断开。这排除了 USB 线/串口软件作为该连接失败的必要条件，但未排除固件内部日志开销、控制器异常、线程或 Windows 端的问题。

新增 `fault_diagnostics` 复选框：用 `ble-fault-diagnostics.conf` 替代原诊断配置，将 ZMK/GATT/CONN/SMP 日志降至 INFO，保留 BLE-DIAG 的连接、HID、订阅及返回值记录，并开启 ASSERT、HW_STACK_PROTECTION、FAULT_DUMP=2、THREAD_NAME。保持已有线程栈大小，不应用独立 HOG 权限修正，也不改变配对数据。构建会检查最终配置值。

此变体用于捕获断言、栈溢出和其他 fault，不是已证实的修复。降低日志与增加检测都会改变时序/内存布局；若它恢复正常，仍须进一步分别验证，不能直接宣布栈溢出或日志过量为根因。异常时 USB 后端也可能无法输出，因此没有 fault 行不能排除崩溃。

为与上一轮比较，运行时同时勾选 `disable_2m` 和 `fault_diagnostics`，下载 `OMM-BLE-diagnostics-1M-fault`。先关闭 Windows 蓝牙启动并接串口，然后开启蓝牙、按 A，保存完整日志和 ELF/map。优先检查 ASSERTION、MPU/FAULT、stack overflow、Current thread、PC，以及 BLE-DIAG disconnected 的原因码。此变体尚未经过 Actions 编译或实机验证。

机制参考：[Zephyr Fatal Errors](https://docs.zephyrproject.org/latest/kernel/services/other/fatal.html)。
