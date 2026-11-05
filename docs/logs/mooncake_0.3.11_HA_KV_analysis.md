# Mooncake 0.3.11.post1 HA + vLLM Qwen3 KV Pool 倒换分析总结

## 1. 环境与版本

- 客户端容器：`lihong-mooncakessdoffload`，运行 Qwen3-30B-A3B-w8a8 prefill/decode。
- Master 容器：`lihong-vllm-ascend_0.30.0rc2`，运行 Mooncake master primary/standby。
- etcd：`etcd-docker-0/1/2`，host network，连接串 `127.0.0.1:2379;127.0.0.1:2381;127.0.0.1:2382`。
- Mooncake 版本：两个容器均已回退到 `mooncake-transfer-engine-npu 0.3.13.post1 -> 0.3.11.post1`。
- vLLM：`0.30.0rc2`。
- vLLM 日志：
  - `/workspace/vllm_prefill.log`
  - `/workspace/vllm_decode.log`
- Mooncake 日志：
  - `/workspace/mooncake_master_0.3.11.log`
  - `/workspace/mooncake_standby_0.3.11.log`

## 2. 回退 0.3.11.post1 的参数适配

### 2.1 Master 参数

0.3.11.post1 仍然有效的参数：

- `eviction_high_watermark_ratio`
- `eviction_ratio`
- `default_kv_lease_ttl`
- `client_ttl`
- `enable_ha`
- `ha_backend_type`
- `ha_backend_connstring`
- `cluster_id`
- `enable_snapshot`
- `enable_snapshot_restore`
- `snapshot_interval_seconds`
- `snapshot_retention_count`
- `snapshot_object_store_type`
- `snapshot_catalog_store_type`
- `rpc_address`
- `rpc_port`
- `metrics_port`
- `enable_offload`
- `promotion_on_hit`

0.3.11.post1 **不存在**的参数，需要从配置中删除：

- `enable_oplog`
- `oplog_poll_interval_ms`
- `oplog_batch_max_entries`

重要修正：

- standby 原来的 `enable_ha` 是 `false`，并没有加入 HA，已改为 `true`。
- 增加 `rpc_address: 71.10.29.141`，避免 HA 模式下 `master_view` 被写成 `0.0.0.0`。
- `client_ttl` 在 YAML 中实际不生效，两个版本日志都显示默认 10；已通过启动脚本参数 `--client_ttl 60` 生效。
- primary 使用 `metrics_port=9003`，standby 使用 `metrics_port=19004`。

当前主备配置关键项：

```yaml
enable_ha: true
ha_backend_type: "etcd"
ha_backend_connstring: "127.0.0.1:2379;127.0.0.1:2381;127.0.0.1:2382"
cluster_id: "mooncake_cluster"
rpc_address: "71.10.29.141"
rpc_port: 50388   # standby: 50389
```

### 2.2 客户端参数

`mooncake.json` 中以下参数在 0.3.11.post1 有效：

- `metadata_server: P2PHANDSHAKE`
- `protocol: ascend`
- `device_name`
- `master_server_address: etcd://...`
- `global_segment_size`
- `preferred_segment`
- `prefer_alloc_in_same_node`
- `enable_ssd_offload`
- `ssd_offload_path`

环境变量在 0.3.11.post1 中有效：

- `MOONCAKE_OFFLOAD_LOCAL_BUFFER_SIZE_BYTES`
- `MOONCAKE_OFFLOAD_TOTAL_SIZE_LIMIT_BYTES`
- `MOONCAKE_OFFLOAD_BUCKET_MAX_TOTAL_SIZE`
- `MOONCAKE_OFFLOAD_BUCKET_EVICTION_POLICY`
- `MC_STORE_USE_HUGEPAGE`
- `MC_STORE_HUGEPAGE_SIZE`

已给 prefill/decode 脚本补充：

```bash
export MC_STORE_CLUSTER_ID=mooncake_cluster
```

注意事项：

- 必须保持 `use_layerwise: false`。0.3.11 客户端没有 `batch_*_session_*` 这组 layerwise API。
- 0.3.11 的 `MooncakeDistributedStore.setup()` 不支持 `tenant_id`，当前未配置。
- `save_decode_cache`、`consumer_is_to_put`、`lookup_rpc_port`、`backend` 等 vLLM connector 参数仍被支持。

## 3. ha_backend_connstring 与 etcd DNS

0.3.11.post1 同时支持：

- `ha_backend_connstring`
- `etcd_endpoints`

两个都配置时以 `ha_backend_connstring` 为准。

格式为分号分隔的 endpoint 列表，支持 `host:port` 和 `http://host:port`。k8s headless Service 场景建议列出每个 etcd pod 的稳定 DNS：

```yaml
enable_ha: true
ha_backend_type: "etcd"
ha_backend_connstring: "etcd-0.etcd-headless.mooncake.svc.cluster.local:2379;etcd-1.etcd-headless.mooncake.svc.cluster.local:2379;etcd-2.etcd-headless.mooncake.svc.cluster.local:2379"
```

DNS 名称可以通过系统解析使用。已在容器内用模拟 pod FQDN 验证 master 能正常完成 etcd 选主。  
建议不要只依赖单个 headless Service 名称，应显式列出所有 etcd pod DNS，避免单点解析/故障切换问题。也不应使用普通 ClusterIP Service。

## 4. 0.3.11.post1 的核心限制：batch OpLog 不生效

### 4.1 现象

0.3.11.post1 下，vLLM 使用 batch put（`batch_put_from_multi_buffers` -> `BatchPutStart/BatchPutEnd`），但 master 中：

```text
ha_oplog_last_sequence_id = 0
ha_oplog_applied_sequence_id = 0
ha_oplog_pending_entries = 0
etcd /oplog/mooncake_cluster/  为空
```

同时 batch put 实际成功：

```text
master_batch_put_start_requests_total = 416
master_batch_put_end_requests_total   = 416
Keys = 24576
```

这说明不是 OpLog 定时器未触发，而是 leader 侧没有为 batch put 写 OpLog。

### 4.2 版本差异

0.3.11.post1 中有：

```text
EtcdOpLogStore
HotStandbyService
OpLogReplicator
OpLogApplier::ApplyPutEnd / ApplyPutRevoke / ApplyRemove
```

但没有：

```text
MasterService::InitializeBatchOpLogWriter
MasterService::AppendOpLogWithDurableFinalize
MasterService::ReserveBatchOpLogSlot
OrderedOpLogWriter
OpLogBatchRecord
OpLogBatchStorage
enable_oplog
```

这些在 0.3.13.post1 中存在，`enable_oplog` 的说明是：

```text
Enable HA metadata replication through batch-record OpLog
default: false
```

结论：

- 0.3.11.post1 的 OpLog 路径覆盖不到 vLLM 的 batch put。
- vLLM 虽然成功写入了 KV 元数据，但不会产生 OpLog 条目。
- 因此 standby 没有东西可复制，snapshot 找不到 OpLog 边界。

## 5. Snapshot 报错含义

日志：

```text
key=/oplog/mooncake_cluster/latest, error=key not found in etcd
[Snapshot] Failed to build descriptor before fork,
code=OPLOG_ENTRY_NOT_FOUND,
msg=failed to resolve snapshot sequence boundary
```

含义：

- snapshot 需要先读取 `/oplog/<cluster_id>/latest` 作为一致性边界。
- 0.3.11 + batch put 下这个 key 永远不存在。
- snapshot 在 fork 子进程前就失败，每 `snapshot_interval_seconds=60` 秒重试一次。
- 这不是数据损坏，也不是 failover 导致的；failover 之前也一直存在。

实际检查：

```text
/data1/mooncake_snapshots       空目录
etcd /oplog/mooncake_cluster/   无 key
etcd snapshot 相关 key          无
```

因此：

- `enable_snapshot: true` 在 0.3.11 + batch put 下没有实际作用，只会刷错误日志。
- standby 启动/提升时日志明确显示：
  - `No snapshot available, falling back to OpLog-only bootstrap`
  - `[Restore] No previous snapshot found, starting fresh`
- standby 无法从 snapshot 恢复，因为根本没有 snapshot。
- 即使 snapshot 能生成，它保存的也是 master 元数据（key -> segment/replica 映射），不是 DRAM 中的 KV 数据本身。

## 6. HA 倒换测试结果

### 6.1 第一次倒换（约 14:08:54 - 14:09:05）

- 旧 primary 停止。
- standby 在 `14:08:59.599` 完成 promotion。
- 客户端出现：
  - `RPC call failed: End of file`
  - `Failed to ping master`
  - `Client not available`
  - `master_view key not found in etcd`
  - `Reconnected to master 71.10.29.141:50389`
- KV 数据面没有出现：
  - `Remote connection failed in lookup`
  - `Failed to get N keys`
  - `KV load failed`
- 控制面中断约 11 秒，之后恢复。

### 6.2 第二次倒换（约 14:52:49 - 14:53:01，开启 DEBUG）

Mooncake 侧：

```text
14:52:49        旧 primary 停止
14:52:55.094    standby WATCHING -> PROMOTING
14:52:55.999    leader_warmup
14:53:01.007    role=leader, state=serving
master_view = 71.10.29.141:50389
```

vLLM prefill 侧：

```text
14:52:50.912    RPC call failed: End of file                 3 次
14:52:50~55     Failed to ping master                         6 次
14:52:54.994~55 master_view key not found in etcd             171 次
14:52:57        KV lookup exists_sample=[-900, ...]           第 1 次
14:53:01        KV lookup exists_sample=[-900, ...]           第 2 次
14:53:01.453    Reconnected to master 71.10.29.141:50389
14:53:01.701    Reconnected to master 71.10.29.141:50389
```

关键发现：

- `-900` 是 Mooncake 错误码 `ErrorCode::RPC_FAIL`（RPC 错误范围 -900 ~ -999）。
- prefill 有 **2 次 KV existence 查询失败**：
  - 每次 `keys=80`、`multi_tp_keys=160`
  - `exists_count=0/160`
  - `exists_sample=[-900, ...]`
  - 共约 320 个 key 查询返回 `RPC_FAIL`
- 这两次失败发生在 master 切换窗口，是 master RPC 不可用导致的直接查询失败。
- 但失败被当作 cache miss 处理：
  - `KV pool worker skip get ... reason=no_load_spec`
  - 没有发起真正的 KV GET
  - 没有 `Remote connection failed in lookup`
  - 没有 `Failed to get N keys`
  - 没有 `KV load failed`
  - 没有 HTTP 5xx
- 重连后查询恢复正常：
  - `exists_sample=[0, ...]` 表示正常 miss
  - `exists_sample=[1, ...]` 表示正常 hit
  - 不再出现 `-900`

### 6.3 倒换后的缓存状态

倒换前旧 primary：

```text
Keys: 8230
Mem Storage: 48.22 GB
ha_oplog_last_sequence_id: 0
```

倒换后新 leader：

```text
14:09:15  Keys: 2048
14:09:25  Keys: 2560
14:18:05  Keys: 24576
ha_oplog_last_sequence_id: 0
```

说明新 leader 不是通过 OpLog 恢复旧元数据，而是重新接收客户端写入、重新填充 KV pool。

从 KV pool 服务角度看：

- 旧 key -> segment/replica 元数据丢失。
- 旧 KV 即使物理上还在 client DRAM segment 中，也没有索引能找到，查询会 miss。
- 如果 vLLM client 进程没有重启，DRAM segment 可能还在，但已成孤儿数据，后续会被覆盖/回收。
- 如果 vLLM client 也重启，DRAM 数据彻底丢失。

因此，0.3.11.post1 倒换后，旧 DRAM KV pool 对服务来说等同于全部丢失，客户端需要重新计算/重新 put。

## 7. 是否必须开 DEBUG 才能看到 KV 查询失败

本次 `-900` 失败不是异常，而是 `m_store.exists()` 正常返回错误码。相关代码：

- `lookup_scheduler()` 只有在抛异常时才打：
  ```text
  logger.error("Remote connection failed in lookup...")
  ```
- `mooncake_backend.get()` 在返回错误码时打：
  ```text
  logger.error("Failed to get N keys out of M...")
  ```
- 但 `exists` 返回 `-900` 时不会抛异常，代码把它当成非 1 的 miss，不会打 ERROR。
- 唯一能看到的地方是 DEBUG 日志：
  ```text
  KV pool lookup request ... exists_sample=[-900, ...]
  ```

因此：

- 要看 KV 查询是否返回错误码：必须开 `VLLM_LOGGING_LEVEL=DEBUG`。
- 要看是否抛异常/请求是否失败：默认 INFO 就够。
- 建议 HA 倒换验证时保留 DEBUG，并重点 grep：
  ```bash
  grep -a "exists_sample=\[-" vllm_prefill.log
  ```
- 如果只想看 KV key/range 级事件，可以只开：
  ```bash
  export VLLM_ASCEND_KVPOOL_RANGE_DEBUG=1
  ```

C++ Mooncake 客户端日志（`master_client.cpp`、`client_service.cpp`、`etcd_helper.cpp`）走 glog，不受 `VLLM_LOGGING_LEVEL` 控制。

## 8. 结论

| 维度 | 结论 |
|---|---|
| 服务是否中断 | 没有，prefill/decode 继续处理请求 |
| 是否有 Master 倒换导致的 KV 查询失败 | 有，第二次测试中 2 次 `exists` 查询返回 `RPC_FAIL=-900` |
| 是否有 KV 数据加载失败 | 没有 |
| 是否自动恢复 | 是，14:53:01 重连成功，之后查询正常 |
| 旧 KV pool 是否保留 | 没有，0.3.11 不复制 batch KV 元数据 |
| snapshot 是否可用 | 不可用，缺少 OpLog 边界 |
| standby 是否可从 snapshot 恢复 | 不能，没有 snapshot，也没有 OpLog |
| 是否符合 0.3.11 预期 | 符合，属于选主切换 + 冷缓存重建 |

## 9. 建议

如果 HA 需要 **KV 元数据连续 + snapshot 可用**：

1. 使用 `0.3.13.post1` 或更高版本。
2. 主备都设置：
   ```yaml
   enable_ha: true
   enable_oplog: true
   ```
3. 保持相同 `cluster_id`。
4. 确认 leader 上 `ha_oplog_last_sequence_id` 持续增长，etcd 出现：
   ```text
   /oplog/mooncake_cluster/latest
   /oplog/mooncake_cluster/batches/...
   ```

如果继续使用 0.3.11.post1：

- 保留 HA 选主，但接受 failover 后 KV pool 元数据丢失。
- 建议关闭 snapshot，避免每 60 秒刷 `OPLOG_ENTRY_NOT_FOUND`：
  ```yaml
  enable_snapshot: false
  enable_snapshot_restore: false
  ```
- 不要配置 `enable_oplog`，0.3.11 没有这个参数，配了也会被忽略。
- 如果要求 failover 后 KV pool 立即可用，需要保证 client 进程不重启，并升级到支持 batch-record OpLog 的版本。
- 如果 client 也会重启，仅靠 DRAM 无法保留 KV，需要 SSD offload 或其他持久化方案。

## 10. 当前运行状态

- 目前只有倒换后提升的 leader 在运行：`71.10.29.141:50389`。
- 原 primary（`50388`）没有重新拉起，当前没有 standby 冗余。
- 要恢复冗余，可重新启动：
  ```bash
  /workspace/master_primary.sh
  ```
- 但在 0.3.11.post1 下，即使恢复 standby，也只能做选主冗余，不能复制 vLLM batch put 的 KV 元数据。
