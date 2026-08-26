# Mooncake A3 插桩版本构建、安装与验证流程

本文说明修改 `file_storage.cpp`、`real_client.cpp` 等 Mooncake 原生源码后，
如何通过独立 Git 分支在 A3 测试环境重新编译 NPU wheel、安装、重启并确认
运行进程加载了正确的原生库。

目标源码基线：

```text
v0.3.11.post1
e9c61075720039bcfc5fffd19f847608402be3d0
```

推荐流程：

```text
优化分支提交并推送
        ↓
A3 测试环境检出精确 commit
        ↓
使用测试环境原有 CANN/Python 参数编译
        ↓
构建并计算 NPU wheel 哈希
        ↓
安装到启动 vLLM 的同一 Python 环境
        ↓
重启所有加载 Mooncake 的 worker/rank
        ↓
核对 wheel、安装 .so、进程已加载 .so
        ↓
运行基线负载和 SSD→HBM 探针
```

## 1. 提交并推送独立优化分支

以下修改应包含在同一个 commit 中：

- `mooncake-store/src/file_storage.cpp`
- `mooncake-store/src/real_client.cpp`
- `mooncake-store/include/offload_perf_trace.h`
- `mooncake-transfer-engine/example/ssd_direct_hbm_probe.cpp`
- 相关 `CMakeLists.txt`
- `scripts/ascend` 下的采集器、分析器和 README

记录完整 commit SHA：

```bash
git rev-parse HEAD
git status --short
```

不需要先合并到 `main`。应先直接验证优化分支，验证完成后再决定是否合并。

## 2. 在 A3 测试环境检出精确版本

测试机能够访问代码仓时，建议使用新目录，不要覆盖正在运行的生产源码目录：

```bash
git clone --branch perf/a3-ssd-direct-baseline \
  https://github.com/lhecust721/Mooncake.git \
  Mooncake-a3-ssd-direct

cd Mooncake-a3-ssd-direct
git submodule update --init --recursive

git rev-parse HEAD
git status --short
```

必须确认：

- A3 环境的 commit SHA 与开发侧一致；
- 工作区没有意外修改；
- submodule 完整。

如果 A3 环境不能访问 GitHub，可通过以下方式离线传输：

- 内部 Git 镜像；
- `git bundle`；
- 保留 `.git` 和 commit SHA 的完整源码包；
- 内部制品仓。

不要只复制两个 `.cpp`，否则容易遗漏头文件、CMake 和验证脚本。

## 3. 使用启动 vLLM 的同一个 Python 环境

激活实际运行 vLLM 的虚拟环境：

```bash
source /path/to/vllm-venv/bin/activate

which python
python --version
python -m pip --version
python -m pip show mooncake-transfer-engine-npu
```

保存旧环境信息和旧 wheel，便于回滚：

```bash
python -m pip show -f mooncake-transfer-engine-npu
python -m pip freeze | grep -i mooncake
```

不要使用另一个 Python 构建扩展后再安装到 vLLM 环境。不同 Python ABI 可能导致
`store.so` 无法加载。

## 4. 加载实际 CANN 环境

仓库 NPU Release CI 的示例为：

```bash
source /usr/local/Ascend/cann-9.0.0/set_env.sh
```

测试机必须使用实际安装的版本。可先定位：

```bash
find /usr/local/Ascend -name set_env.sh
```

加载后检查：

```bash
echo "$ASCEND_HOME_PATH"
echo "$LD_LIBRARY_PATH"
npu-smi info
```

## 5. 使用独立构建目录配置 CMake

不要删除测试机已有 `build`，使用单独目录：

```bash
PYTHON_BIN="$(which python)"

cmake -S . -B build-a3-ssd-direct \
  -DUSE_ASCEND_DIRECT=ON \
  -DUSE_CUDA=OFF \
  -DWITH_EP=OFF \
  -DUSE_HTTP=ON \
  -DUSE_ETCD=ON \
  -DSTORE_USE_ETCD=ON \
  -DBUILD_UNIT_TESTS=OFF \
  -DBUILD_EXAMPLES=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DPython3_EXECUTABLE="$PYTHON_BIN"
```

注意：

- `BUILD_EXAMPLES=ON` 用于编译 `ssd_direct_hbm_probe`；
- `USE_ASCEND_DIRECT=ON` 启用 Ascend Direct，并设置共享库构建；
- `USE_ETCD`、`STORE_USE_ETCD` 等参数必须与当前部署一致；
- 最稳妥的方法是复制现有生产 wheel 的构建参数，仅加入本次必要选项；
- 不要仅为了匹配 CI 而改变实际 metadata/backend 模式。

## 6. 编译主库和探针

完整编译：

```bash
cmake --build build-a3-ssd-direct -j"$(nproc)"
```

检查产物：

```bash
find build-a3-ssd-direct -type f \
  \( -name 'store*.so' \
     -o -name 'libmooncake_store.so' \
     -o -name 'ssd_direct_hbm_probe' \)
```

预期包括：

```text
build-a3-ssd-direct/mooncake-integration/store.<python-abi>.so
build-a3-ssd-direct/mooncake-store/src/libmooncake_store.so
build-a3-ssd-direct/mooncake-transfer-engine/example/ssd_direct_hbm_probe
```

只重新编译探针时可执行：

```bash
cmake --build build-a3-ssd-direct \
  --target ssd_direct_hbm_probe \
           hbm_p2p_mapping_probe \
           nvme_hbm_l2_read_probe \
  -j"$(nproc)"
```

仅编译探针不会更新 Mooncake 主线插桩。主线插桩必须重新构建 `store.so`、
`libmooncake_store.so` 和 NPU wheel。

## 7. 构建 NPU wheel

当前分支的 `scripts/build_wheel.sh` 明确要求 `auditwheel repair` 不再复制和重命名
`libtransfer_engine.so`。原因是 wheel 构建前已经将该库放入 `mooncake/`；如果
`auditwheel` 再生成 `mooncake_transfer_engine_npu.libs/libtransfer_engine-<hash>.so`，
同一进程会加载两份 Transfer Engine，并形成两套 `globalConfig()` 等全局状态。
这会让 Store 和 Ascend transport 对 FabricMem 配置产生不一致，后续 SSD/HBM
基线也就不可信。

获取当前 Python major/minor：

```bash
PYTHON_VERSION="$(python -c \
  'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')"
```

构建 NPU wheel：

```bash
NPU_BUILD=1 \
PYTHON_VERSION="$PYTHON_VERSION" \
BUILD_DIR=build-a3-ssd-direct \
OUTPUT_DIR=dist-a3-ssd-direct \
./scripts/build_wheel.sh
```

wheel 通常位于：

```text
mooncake-wheel/dist-a3-ssd-direct/
```

检查并计算哈希：

```bash
find mooncake-wheel/dist-a3-ssd-direct \
  -maxdepth 1 -name '*.whl' -type f

sha256sum mooncake-wheel/dist-a3-ssd-direct/*.whl
```

安装前检查 wheel 中的 Transfer Engine 数量：

```bash
WHEEL=$(find mooncake-wheel/dist-a3-ssd-direct \
  -maxdepth 1 -type f -name '*.whl' | head -1)

python -m zipfile -l "$WHEEL" | grep 'libtransfer_engine'
```

期望仅有：

```text
mooncake/libtransfer_engine.so
```

以下路径必须不存在：

```text
mooncake_transfer_engine_npu.libs/libtransfer_engine-<hash>.so
```

建议同时解压 wheel，检查所有 Mooncake 原生扩展的动态依赖：

```bash
CHECK_DIR=$(mktemp -d)
python -m zipfile -e "$WHEEL" "$CHECK_DIR"

find "$CHECK_DIR" -type f -name '*.so' -print0 | while IFS= read -r -d '' so; do
  readelf -d "$so" 2>/dev/null | grep -q 'transfer_engine' || continue
  echo "===== $so ====="
  readelf -d "$so" | grep -E 'NEEDED.*transfer_engine|RPATH|RUNPATH'
done
```

不应出现：

```text
NEEDED libtransfer_engine-<hash>.so
```

`NPU_BUILD=1` 会将包名设置为：

```text
mooncake-transfer-engine-npu
```

## 8. 停止旧 worker 并安装新 wheel

正在运行的进程已经映射旧 `.so`。在进程运行期间覆盖 wheel，不会让旧进程使用
新代码。

建议先停止测试环境中的 Prefill/Decode vLLM worker，然后使用启动 vLLM 的同一
Python 安装：

```bash
python -m pip install \
  --force-reinstall \
  --no-deps \
  /绝对路径/mooncake_transfer_engine_npu-0.3.11.post1-*.whl
```

对于依赖已完整的网络隔离测试环境，`--no-deps` 可以防止 pip 尝试联网解析依赖。

安装后检查：

```bash
python -m pip show mooncake-transfer-engine-npu

python -c \
  'import mooncake.store; print(mooncake.store.__file__)'
```

检查动态依赖和哈希：

```bash
STORE_SO="$(python -c \
  'import mooncake.store; print(mooncake.store.__file__)')"

ldd "$STORE_SO"
sha256sum "$STORE_SO"
```

如果 `ldd` 显示 `libmooncake_store.so`，继续定位并计算该库哈希：

```bash
ldd "$STORE_SO" | grep libmooncake_store
```

再检查安装目录中是否残留 auditwheel 生成的第二份库：

```bash
PKG_ROOT=$(python -c \
  'import pathlib, mooncake; print(pathlib.Path(mooncake.__file__).resolve().parent.parent)')

find "$PKG_ROOT/mooncake" \
     "$PKG_ROOT/mooncake_transfer_engine_npu.libs" \
  -maxdepth 1 -type f -name 'libtransfer_engine*.so' -print 2>/dev/null
```

必须只输出 `mooncake/libtransfer_engine.so`。如果哈希库仍存在，先确认所有
Mooncake/vLLM 进程已停止，再使用干净虚拟环境安装；不要在进程仍映射 `.so` 时
手工删除库文件。

## 9. 启动前启用插桩

必须在启动 Mooncake/vLLM 进程前设置：

```bash
export MOONCAKE_OFFLOAD_PERF_TRACE=1
```

插桩开关在进程内缓存。进程启动并首次读取后再修改环境变量，不保证生效。

然后使用原有部署脚本启动：

- Mooncake master；
- Prefill vLLM worker；
- 使用 Mooncake Store 的 Decode worker；
- 所有 TP/DP rank。

为了避免混合版本，测试环境中的 Mooncake 组件应统一使用新 wheel 并统一重启。

## 10. 确认运行进程加载的新 `.so`

查询进程：

```bash
pgrep -af 'vllm|mooncake'
```

检查每个 worker：

```bash
grep -E 'mooncake|store.so|ascend' /proc/<PID>/maps
```

专门核对运行时 Transfer Engine：

```bash
awk '/libtransfer_engine.*\.so/ {print $NF}' /proc/<PID>/maps | sort -u
```

每个进程必须只输出同一份：

```text
.../site-packages/mooncake/libtransfer_engine.so
```

不得同时出现 `mooncake_transfer_engine_npu.libs/libtransfer_engine-<hash>.so`。
启动日志还应包含 `Fabric mem mode is enabled`，且不应因这次打包问题退回
`aclrtMallocHost`。修复后需重新验证 `HcclBatchPut`/KV PUT；不能仅凭库数量
推断 HCCL 参数错误已经消失。

运行身份与平台采集器：

```bash
python scripts/ascend/collect_a3_ssd_direct_env.py \
  --pid <PREFILL_PID> \
  --pid <OTHER_RANK_PID> \
  --wheel /绝对路径/新构建的.whl \
  --ssd-path /nvme/mooncake_offload \
  --npu-bdf 0000:01:00.0 \
  --nvme-bdf 0000:81:00.0 \
  --strict \
  --strict-platform \
  --output artifacts/a3-environment.json
```

使用测试机真实 BDF：

```bash
lspci -D -nn
```

进入正式负载前，至少确认：

```text
source_commit                  PASS
package_version                PASS
wheel_vs_installed_native      PASS
loaded_vs_installed_native     PASS
loaded_transfer_engine_count   1
```

`direct_dma_qualified=NO` 是当前预期结果。采集器不会仅根据 PCIe/P2PDMA
配置宣称已经实现物理直传。

## 11. 运行基线负载并现场查看

实时分析 owner/requester 日志：

```bash
tail -F owner.log requester.log | \
  python scripts/ascend/analyze_offload_perf_trace.py - --live \
    --output artifacts/offload-live.json
```

现场应看到：

```text
LIVE stage=ssd_to_staging ...
LIVE stage=staging_to_hbm ...
LIVE stage=offload_get_total ...
```

## 12. 单独运行 SSD→HBM 探针

探针不会被 Mooncake 自动调用，需要手工执行：

```bash
./build-a3-ssd-direct/mooncake-transfer-engine/example/ssd_direct_hbm_probe \
  --file /nvme/mooncake_offload/probe.bin \
  --size 4194304 \
  --offset 0 \
  --device 0 | \
  tee artifacts/ssd-direct-hbm-probe.jsonl
```

当前探针最高只能自动证明：

```text
L0_HBM_IDENTIFIED
L1_ODIRECT_ACCEPTED
```

即使得到 L1，也不能直接声明 NVMe→HBM 物理 DMA。L2/L3 仍需要 Ascend
buffer 注册、驱动映射、direct/fallback 计数和 PCIe/IOMMU/SMMU 运行证据。

## 13. 回滚

保留测试前的旧 wheel。需要回滚时：

```bash
python -m pip install \
  --force-reinstall \
  --no-deps \
  /绝对路径/旧版本.whl
```

回滚安装后必须再次重启所有加载 Mooncake 的进程，并通过 `/proc/<PID>/maps`
确认旧库已经重新加载。

## 14. 证据链要求

测试报告必须同时保存：

```text
源码 commit SHA
        ↓
CMake 参数和 CANN/Python 版本
        ↓
wheel 文件及 SHA-256
        ↓
安装目录中的 store.so/libmooncake_store.so 哈希
        ↓
/proc/<PID>/maps 中实际加载的 .so 路径和哈希
        ↓
基线测量、探针输出和 TTFT 结果
```

源码分支正确不等于运行包正确。只有上述证据一致，才能确认测试结果对应本次修改。
