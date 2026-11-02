# Mooncake Master Snapshot/Restore 与 KV Pool 复用问题分析

- 日期：2026-11-01（更新：2026-11-02）
- 版本：`mooncake-transfer-engine-npu 0.3.12.post1`
- 场景：vLLM-Ascend + `AscendStoreConnector`（Mooncake KV pool，非 PD 的 `MooncakeConnector`）
- Master 容器：`lihong-vllm-ascend_0.30.0rc2`
- vLLM 容器：`lihong-mooncakessdoffload`

## 1. 结论摘要

1. 当前 0.3.12.post1 的 snapshot **确实保存了 key 元数据**（`metadata` 中 `shards` 非空，包含 key、size、replica 等信息）。
2. 但 restore 的 `ApplySnapshotState` 会清理“lease 在**恢复时刻**已过期且未 soft-pin”的对象。因此：
   - KV pool 中 `PutEnd` 后未被再次访问的空闲 key，lease 正常情况下就是过期状态；
   - 这类 key 在 master 重启恢复时会被删除；
   - 即使短暂有读取续租，只要停机时间超过 lease 剩余时间，恢复时仍会被删除。
3. 因此，**当前 snapshot 元数据恢复不是 KV pool 的持久化机制**。它面向的是 master 快速 HA 恢复：client/segment/DRAM 数据还活着、活跃对象 lease 仍有效时，把 master 的段/索引/任务元数据接回来。
4. 若需要 KV pool 跨 master 重启复用，需要额外方案：
   - soft pin（0.3.12 可用但当前 vLLM-Ascend 未启用，且有 TTL/淘汰限制）；
   - 修改 restore 清理策略（或增加配置开关）；
   - client 侧重注册/重建索引；
   - 评估新版本（v0.3.13/main）的 snapshot/oplog HA 机制。
5. **2026-11-02 补充：SSD/LOCAL_DISK key 与 DRAM key 的恢复机制不同。** SSD 数据和索引持久化在 `/data1/mooncakenvme/rank_*/` 的 `.bucket` + `.meta` 文件中，client 启动/重连后通过 `MountLocalDiskSegment` + `ScanMeta` + `ReRegisterOffloadedObjects` 重新注册，不依赖 master snapshot；而 DRAM key 的索引只在 master 内存中，client 没有持久化索引/重扫描接口，因此仍依赖 snapshot/HA。实测不开 snapshot、重启 vllm + master 后，SSD 恢复出 22,917 个 key、`SSD Storage: 268.56 GB / 800.00 GB`，而 DRAM 为 `0 B / 60 GB`。

## 2. Mooncake 写入与 lease 语义

Mooncake 的写入是两阶段：

- `PutStart`：master 分配 replica（DRAM buffer），状态 `PROCESSING`，返回 buffer 描述符；client 写入 KV 数据。
- `PutEnd`：client 写完后通知 master；master 将 replica 标记为 `COMPLETE`，清理 processing 状态、结算 quota，并调用：

```cpp
// mooncake-store/src/master_service.cpp:2905 (v0.3.12.post1)
metadata.GrantLease(0, default_kv_soft_pin_ttl_);
```

源码注释说明：

```cpp
// 1. Set lease timeout to now, indicating that the object has no lease
//    at beginning.
// 2. If this object has soft pin enabled, set it to be soft pinned.
```

结论：

- `lease` 不是对象数据的生命周期 TTL，而是“读保护租约”；
- `PutEnd` 时没有读者，所以把 lease 设为 now（表示当前无租约）；
- 后续 `Get` / `ExistKey` 才会重新授予 `now + default_kv_lease_ttl`，保护读取期间不被淘汰；
- 空闲对象 lease 过期后不会立即删除，仍可被后续 Get 命中，只有在内存水位/淘汰策略下才可能被 evict。

## 3. Restore 清理逻辑与根因

恢复时 `RestoreState()` 先取当前时间 `now`，再调用 `ApplySnapshotState(now)`。清理逻辑使用**恢复时刻**的 `now`：

```cpp
// mooncake-store/src/master_service.cpp:6051-6055 (v0.3.12.post1)
if (it->second.HasDiffRepStatus(ReplicaStatus::COMPLETE) ||
    (it->second.IsLeaseExpired(cleanup_now) &&
     !it->second.IsSoftPinned(cleanup_now))) {
    VLOG(1) << "clear metadata key=" << it->first;
    it = EraseMetadata(...);
}
```

即：

- replica 不完整 → 删除；
- replica 完整，但 lease 在恢复时刻已过期，且未 soft-pin → 删除；
- 只有恢复时刻 lease 仍有效，或 soft-pin 未过期（0.3.12）→ 保留。

由于 KV pool 对象在 `PutEnd` 后 lease 就是过期状态，restore 会清掉这些空闲 key。这不是快照编码问题，而是恢复策略与 KV pool 复用场景的冲突。

## 4. 实测过程与证据

测试配置（相对原配置的变更）：

```bash
export MOONCAKE_SNAPSHOT_LOCAL_PATH=/data1/mooncake_snapshots

nohup mooncake_master \
    --port 50388 \
    --metrics_port 19004 \
    --eviction_high_watermark_ratio 0.9 \
    --eviction_ratio 0.2 \
    --enable-snapshot=true \
    --snapshot-interval-seconds=15 \
    --snapshot-retention-count=10 \
    --snapshot-object-store-type=local \
    --enable-snapshot-restore=true \
    --client_ttl 60 \
    --enable_offload=false \
    --default_kv_lease_ttl 600000 > master.log 2>&1 &
```

| 参数 | 原值 | 测试值 | 说明 |
|---|---:|---:|---|
| `--default_kv_lease_ttl` | 30000 (30s) | 600000 (10min) | 仅用于验证 lease 在恢复时仍有效的情况 |
| `--snapshot-interval-seconds` | 60 | 15 | 加快测试 |
| `--snapshot-retention-count` | 3 | 10 | 保留测试快照 |

### 4.1 失败：快照有 key，但 restore 后 key 被清

- 推理后 master：`master_key_count=1`，`master_allocated_bytes=12582912`
- 快照 `20261101_154154_541`：
  - `metadata` 341B，解压后包含 key；
  - key 的 `lease_timeout = 1793547693218`（约 15:41:33.218，即 PutEnd 时刻，已过期）。
- 重启后：

```text
[Restore] Total allocated size after restore: 0
master_key_count 0
master_allocated_bytes 0
```

说明：snapshot 中有 key，但 restore 时被清理。

### 4.2 成功：恢复前续租，restore 保留 key

对 key 调用 `is_exist()`（触发 `ExistKey`，会刷新 lease）后：

- lease 更新为 `16:01:49.125`
- 快照 `20261101_155215_029` 保存了新的 lease
- 立即重启 master（不带 skip-cleanup）：

```text
[Restore] Total allocated size after restore: 12582912
[Restore] Successfully restored state from snapshot: 20261101_155215_029

master_key_count 1
master_allocated_bytes 12582912
```

说明：lease 在恢复时刻仍有效时，key 元数据可以恢复。

### 4.3 诊断：跳过清理可恢复过期 key

设置：

```bash
MOONCAKE_MASTER_SERVICE_SNAPSHOT_TEST_SKIP_CLEANUP=1
```

重启后：

```text
[Restore] Total allocated size after restore: 12582912
master_key_count 1
```

说明：清理逻辑确实是 key 被删除的直接原因。该环境变量是测试开关，会跳过所有清理，不建议生产直接使用。

### 4.4 证据文件

- `/data1/mooncake_snapshots/evidence_restore_ok_20261101_155215_029`
- `/workspace/master.log.restore_success_*`
- `/workspace/master.log.run1_*`、`master.log.run2_*`、`master.log.run3_*`、`master.log.run4_*`
- 当前 master metrics：`master_key_count=1`，`master_allocated_bytes=12582912`

## 5. Snapshot restore 到底恢复什么

它恢复的是 **master 的元数据状态**，不是 KV 数据本身：

- segment 注册信息、容量、allocator/已分配状态；
- replica 元数据、`replica_next_id`、discarded replicas；
- 恢复时刻仍有效 lease 的对象，或（0.3.12）soft-pinned 且未过期的对象；
- task manager（offload/promotion/copy/move 等任务）；
- tenant quota policy、evicted-key FIFO/catalog 等。

注意：**KV 实际数据不在 snapshot 中**。snapshot 只包含对象元数据与 replicas 描述，真正的 KV 字节位于 client 的 DRAM segment。因此：

- 只重启 master、vLLM/client 不重启：DRAM 数据还在，恢复活跃对象元数据后有复用价值；
- 如果 vLLM 也重启：数据本身已丢失，恢复全部 key 元数据也没有意义，甚至会产生悬空元数据；
- 恢复策略因此保守地丢弃“lease 过期且未 pin”的空闲对象。

## 6. 对用户疑问的澄清

### 6.1 这是 KV pool，不是 PD 传输

正确。当前是 `AscendStoreConnector` + Mooncake KV pool，用于跨请求、跨实例 prefill 复用；不是 `MooncakeConnector` 的 PD KV 传输。之前把两者混在一起的分析已修正。

### 6.2 `PutEnd` 为什么把 lease 设为 now

因为 lease 是读保护租约，不是数据 TTL。PutEnd 后没有读者，所以 lease 置为 now；后续 Get/ExistKey 才续租。空闲对象 lease 过期是正常状态。

### 6.3 `lease TTL > 快照间隔 + 重启耗时` 不成立

- 对空闲 KV pool 对象，没有 Get/ExistKey 就不会续租，lease TTL 设多大都没用；
- cleanup 使用恢复时刻的 now，不是快照时刻。即使快照时 lease 有效，只要停机时间超过剩余 lease，恢复时仍会被清；
- 重启时间不可控，因此 lease 不能作为持久化手段；
- 实测：`default_kv_lease_ttl=600000` 但不 touch key，restore 后仍为 `allocated=0 / key=0`；
- 30s lease + 60s 快照间隔不是根因。

## 7. 可行方案

### 方案 A：修改 restore 清理策略（推荐用于 KV pool 持久化）

- 对 complete 对象不要仅因 lease 过期就删除；
- 或增加显式配置开关（例如 `snapshot_restore_keep_expired_keys`），KV pool 场景默认保留；
- 需要额外处理 client/segment 存活校验与脏元数据风险；
- 当前可用 `MOONCAKE_MASTER_SERVICE_SNAPSHOT_TEST_SKIP_CLEANUP=1` 验证，但不建议生产直接使用。

### 方案 B：soft pin（0.3.12 可用，需改 client）

- Put 时设置 `ReplicateConfig.with_soft_pin=True`；
- 调大 `--default_kv_soft_pin_ttl` 覆盖最大预期停机窗口；
- restore 时 0.3.12 的 `!IsSoftPinned` 例外会保留该对象；
- 限制：仍是有限 TTL；`allow_evict_soft_pinned_objects=1` 时高水位仍可能淘汰；
- 当前 vLLM-Ascend `mooncake_backend.py:_build_replicate_config()` 未设置 `with_soft_pin`，需要 patch。

### 方案 C：客户端重新注册/重建索引

- master 恢复 segment 后，由 client 启动时重新上报/校验 key；
- 需要修改 vLLM-Ascend connector；
- 不依赖 lease 过期策略，但实现复杂度较高。

### 方案 D：评估新版本

- v0.3.13 的 `ApplySnapshotState` 清理条件中已没有 `!IsSoftPinned` 例外（只剩 `IsLeaseExpired(cleanup_now)`），不能指望升级自动解决；
- main 分支已重构 snapshot/oplog HA，需单独评估其对象恢复语义。

## 8. 建议

1. 如果目标是验证 master 元数据恢复：当前行为符合“只恢复活跃/有效对象”的设计，测试时应保证 key 在恢复时刻 lease 有效（例如恢复前 `ExistKey` 续租），或使用 skip-cleanup 做诊断。
2. 如果目标是 KV pool 跨重启复用：不要把 `default_kv_lease_ttl` 当持久化 TTL；优先评估修改 restore 清理策略或客户端重注册，soft pin 可作为过渡方案。
3. 生产环境不建议使用 `MOONCAKE_MASTER_SERVICE_SNAPSHOT_TEST_SKIP_CLEANUP=1`，它只是验证手段。
4. 如需继续分析，建议下一步：
   - 验证 soft pin 在 0.3.12 下跨重启恢复的完整链路；
   - 设计 restore 保留过期 key 的开关及 client/segment 校验逻辑；
   - 对比 v0.3.13/main 的 snapshot/oplog HA 行为。

## 9. 2026-11-02 补充：SSD offload 与 DRAM key 的恢复机制对比

### 9.1 最新实验：关闭 snapshot，重启 vllm + master

master 启动参数（无 snapshot）：

```bash
mooncake_master --port 50388 --metrics_port 19004 \
  --eviction_high_watermark_ratio 0.01 --eviction_ratio 1 \
  --default_kv_lease_ttl 1000 --client_ttl=120 \
  --enable_offload=true --offload_on_evict false --promotion_on_hit false
```

> 注意：gflags 的 bool flag 不接受空格分隔的 `false`。`--offload_on_evict false` 实际会被解析为 true，日志中会看到 `Offload-on-evict mode enabled`。如需关闭，应写 `--offload_on_evict=false --promotion_on_hit=false`。

时间线与指标：

- `01:47:40` master 启动，初始 `Mem Storage: 0 B / 0 B`、`SSD Storage: 0 B / 0 B`、`Keys: 0`、`Clients: 0`
- `01:49:55` vllm prefill 挂载 DRAM segment + local disk segment
- `01:50:00` master：`Mem Storage: 0 B / 30.00 GB`、`SSD Storage: 258.83 GB / 400.00 GB`、`Keys: 22087`、`Clients: 1`
- `01:50:12` vllm decode 挂载 DRAM segment + local disk segment
- `01:50:20` master：`Mem Storage: 0 B / 60.00 GB`、`SSD Storage: 268.56 GB / 800.00 GB`、`Keys: 22917`、`Clients: 2`
- `file_cache_nums_ = 22917`，`master_allocated_file_size_bytes = 288362594304`（约 268.6GB）
- `master_allocated_bytes = 0`（DRAM 为空，因为 vllm 重启后 DRAM 数据已丢失）

master 日志中的关键行：

```text
Mount local disk segment with client id is : ..., enable offloading is: 1
SSD Storage: 258.83 GB / 400.00 GB | Keys: 22087 | Clients: 1
SSD Storage: 268.56 GB / 800.00 GB | Keys: 22917 | Clients: 2
```

vllm 侧关键日志：

```text
file_storage.cpp:328 IsEnableOffloading result: true
mooncake_backend.py:299 Mooncake SSD offload enabled (Mode A): path=/data1/mooncakenvme
```

### 9.2 SSD 的索引/元数据在哪里

SSD offload 路径下（例如 `/data1/mooncakenvme/rank_0/`）有两类文件：

```text
<id>.bucket   # KV 数据
<id>.meta     # bucket 的 keys + StorageObjectMetadata，持久化索引
```

实际观察到的样例：

```text
7346380218778.bucket   163580040 bytes
7346380218778.meta         2418 bytes
7346380218779.bucket   264244680 bytes
7346380218779.meta         3906 bytes
```

元数据是双份的：

| 位置 | 作用 |
|---|---|
| SSD 上的 `.meta` 文件 | 持久化索引，client 重启/master 重启后可以重建 |
| client 进程内存里的 `object_bucket_map_` / bucket 索引 | 运行时加速查询的缓存 |
| master 内存里的 LOCAL_DISK replica 元数据 | 运行时协调/路由索引，可被 client 重新注册重建 |

所以 SSD 的 durable source 是磁盘上的 `.meta` + `.bucket`，不是 master snapshot。

恢复链路：

```text
vllm 启动 / master 重启后 heartbeat 返回 SEGMENT_NOT_FOUND
  -> FileStorage::Init() / Heartbeat()
  -> MountLocalDiskSegment()
  -> ReportSsdCapacity()
  -> ScanMeta() 扫描 .meta
  -> ReRegisterOffloadedObjects()
  -> NotifyOffloadSuccess() 把 key 注册回 master
```

源码对应：

- `BucketStorageBackend::ScanMeta`：`mooncake-store/src/storage_backend.cpp:2276`
- `FileStorage::ReRegisterOffloadedObjects`：`mooncake-store/src/file_storage.cpp:1217`
- `StorageObjectMetadata` / `BucketMetadata`：`mooncake-store/include/storage_backend.h`

### 9.3 DRAM key 为什么 client 不保存

- `MountSegment` 只是把一个 DRAM buffer 注册给 master，告诉 master “这块内存可用”。
- 真正的 `key -> segment / buffer` 元数据，是 `PutStart/PutEnd` 时由 **master** 创建并保存在 master 的 `MetadataShards` 中。
- client 侧没有 DRAM key 的持久化清单，也没有 `ScanMeta`/`ReRegisterDramObjects` 这类接口。
- vLLM 自己可能有 prefix cache 的 block/hash 信息，但这不是 Mooncake 的完整 key 索引，也没有用于 master 重建的协议。
- 如果只重启 master、vllm 不重启：DRAM 数据还在 client 内存里，但 master 索引没了，当前只能靠 snapshot 恢复。
- 如果 vllm 也重启：DRAM 数据本身没了，即使 client 保存了 key 清单，恢复出来也是悬空索引。

理论上可以增加一套 DRAM 版本的 `ScanMeta + ReRegisterDramObjects`：

```text
client 维护 DRAM key -> buffer 的完整索引
master 重启后 client 检测到 epoch/generation 变化
client 调用 ReRegisterDramObjects()
master 重建 key 元数据
```

但当前 0.3.12 没有实现，原因包括：

1. DRAM 是易失缓存，client 重启后数据就没了，维护持久化索引收益有限；
2. master 元数据不只是 key 列表，还包括 replica、lease、segment、task、quota 等，client 只能重建 key 部分；
3. vLLM 的 KV block 会被复用/淘汰，client 侧维护实时、完整、无锁的全局 key 索引复杂度和开销都高；
4. 当前设计把 DRAM 元数据的权威来源放在 master，用 snapshot/HA 恢复。

### 9.4 为什么 SSD 场景不开 snapshot 反而更好

- `master_snapshot_codec.cpp` 明确注释：

```cpp
// Use the existing SegmentSerializer which only handles SegmentManager
// Note: NoFSegmentManager is not currently serialized in snapshots
```

- `SegmentSerializer` 仍会序列化 `SegmentManager::client_local_disk_segment_`（快照里的 `"ld"`），包括：
  - `enable_offloading`
  - pending offloading tasks
  - `ssd_total_capacity_bytes`
- 开启 snapshot 时，restore 会把旧 `ld`/client_id 恢复回来，`OffloadObjectHeartbeat` 找到 client 后直接返回 OK，client 不会走 `SEGMENT_NOT_FOUND -> MountLocalDiskSegment -> ScanMeta`。
- 同时 restore 的 lease 清理会删除过期的 key 元数据。
- 结果：master 以为 SSD segment 还在、client 以为不用重注册，但旧 key 元数据已经丢了，SSD 对象变成孤儿。
- 不开 snapshot 时，master 空启动，client 收到 `SEGMENT_NOT_FOUND`，执行完整的重新 mount + ScanMeta + 重注册，状态更干净。

因此对 SSD 持久化来说，snapshot 不是恢复机制，反而可能抑制 client 的完整恢复。

### 9.5 DRAM vs SSD 对比

| | DRAM key | SSD/LOCAL_DISK key |
|---|---|---|
| KV 数据位置 | vllm 进程 DRAM segment | `/data1/mooncakenvme/rank_*/*.bucket` |
| 索引/元数据位置 | master 内存 `MetadataShards` | client `.meta` 文件 + 内存缓存 + master 重建索引 |
| 持久化 | 否 | 是 |
| client 重启后 | 数据丢失 | 数据保留，`ScanMeta` 可恢复 |
| master 重启后 | 需要 snapshot 恢复索引 | client 可扫描并重新注册 |
| snapshot 作用 | DRAM 索引恢复（受 lease/pin 限制） | 不是必须，可能抑制 client 重注册 |
| 0.3.12 是否支持 client 重建 | 否（无 `ReRegisterDramObjects`） | 是（`ScanMeta` + `ReRegisterOffloadedObjects`） |

### 9.6 补充建议

- **SSD 场景**：以 client + 磁盘为准。master 重启后应强制 client 重新 mount local disk segment 并 rescan，或让 heartbeat 返回 `SEGMENT_NOT_FOUND`；不要依赖 master snapshot 恢复 SSD 状态。
- **SSD 容量**：`ReportSsdCapacity` 目前只在 `FileStorage::Init()` 调用；重新 mount 路径也应重新上报，否则会出现 `SSD Storage: X / 0 B`。
- **DRAM 场景**：snapshot 只用于 master 单独重启时的索引恢复；如果 vllm 也重启，DRAM 数据丢失是本质限制，snapshot 无法恢复数据。
- **gflags**：bool flag 使用 `--flag=false`，不要用 `--flag false`。
- **snapshot 与 SSD**：在当前 0.3.12 实现下，SSD 持久化场景关闭 snapshot 或至少不要让 restore 恢复 `ld`，可以避免“半恢复”抑制 client 重注册。

## 10. 日志附件

以下日志已随本文档一起上传到仓库：

- `docs/logs/master_resetafter.log`
- `docs/logs/master_resetbefor.log`
- `docs/logs/mooncake_SSD.log`
- `docs/logs/mooncake_qwen_DRAM.log`
- `docs/logs/mooncake.log`（最新 master 日志 `/workspace/mooncake.log`，用于 2026-11-02 的 SSD 恢复验证）
- `docs/logs/vllm_prefill.log`（vllm 容器 `/workspace/vllm_prefill.log`）

对应容器内路径：

master 容器 `lihong-vllm-ascend_0.30.0rc2`：

```text
/workspace/master_resetafter.log
/workspace/master_resetbefor.log
/workspace/mooncake_SSD.log
/workspace/mooncake_qwen_DRAM.log
/workspace/mooncake.log
```

vllm 容器 `lihong-mooncakessdoffload`：

```text
/workspace/vllm_prefill.log
```
