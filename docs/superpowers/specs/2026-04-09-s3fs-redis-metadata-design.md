# s3fs-fuse Redis 元数据与容量展示设计

- 日期: 2026-04-09
- 目标分支: `master`
- 决策: 采用“基于当前 `s3fs-fuse` 仓库扩展”的方案，不新建独立文件系统项目

## 1. 背景与问题

当前 `s3fs-fuse` 已有进程内元数据缓存（`StatCache`），但存在两个限制：

1. 容量展示
   `statfs` 返回的是广告值，不是实际已用容量。核心逻辑位于 [src/s3fs.cpp#L3175](/Users/renzheng.wang/Workspace/bigc1/s3fs-fuse/src/s3fs.cpp#L3175)。
   `-o bucket_size` 只是手工设定总容量，逻辑位于 [src/s3fs.cpp#L5103](/Users/renzheng.wang/Workspace/bigc1/s3fs-fuse/src/s3fs.cpp#L5103)。
2. 元数据性能
   `getattr/readdir` 热路径依赖 `HEAD/LIST` 网络请求。
   当前 `StatCache` 是进程内缓存，无法跨进程/跨节点复用，定义在 [src/cache.h#L45](/Users/renzheng.wang/Workspace/bigc1/s3fs-fuse/src/cache.h#L45)。

## 2. 目标与非目标

### 2.1 目标

1. 保持 `s3fs-fuse` 现有行为默认不变（不启用 Redis 时行为与上游一致）。
2. 增加可选 Redis 元数据二级缓存（L2），提升 `getattr/readdir` 命中率并减少 `HEAD/LIST`。
3. 支持可选的容量展示增强，在 `statfs` 中返回“可配置总量 + Redis 维护的已用量”。
4. 降低上游同步成本：最小化对 `s3fs.cpp` 的侵入改动。

### 2.2 非目标

1. 不改变对象数据读写语义（不实现强一致分布式锁）。
2. 不在第一阶段实现完整分布式事务型元数据系统。
3. 不追求“外部工具写入 S3 后秒级强一致容量统计”。

## 3. 总体架构

采用两级缓存模型：

1. L1: 现有进程内 `StatCache`（保留）。
2. L2: Redis 元数据缓存（新增，可选）。
3. 回源: 现有 S3 `HEAD/LIST` 逻辑（保留）。

读路径原则：

1. 先查 L1。
2. L1 miss 时查 L2 Redis。
3. L2 miss 时回源 S3。
4. 回源成功后回填 L1 + L2。

写路径原则：

1. 成功写入 S3 后，失效或更新 L1/L2 对应条目。
2. 不改变现有 FUSE 操作成功/失败语义。

## 4. 模块设计

### 4.1 新增接口层

新增抽象接口（示例命名）：

- `MetadataBackend`（仅负责 L2）
- `NullMetadataBackend`（默认实现，不做任何事）
- `RedisMetadataBackend`（基于 hiredis）
- `MetadataBackendFactory`（按挂载参数选择实现）

接口能力覆盖现有 `StatCache` 关键操作：

1. `GetStat/PutStat/DeleteStat`
2. `GetS3ObjList/PutS3ObjList`
3. `GetNegative/PutNegative`
4. `ClearByPrefix`（用于目录改名/删除后的批量失效）

### 4.2 与现有 `StatCache` 集成方式

在不大改调用点前提下，将 Redis 集成在 `StatCache` 内部：

1. `Get*` 流程:
   先走当前内存树；miss 后查 L2，若命中则回填当前内存树并返回。
2. `Add/Update/Del*` 流程:
   维持当前内存行为，并同步写入/失效 L2。

优点：

1. `s3fs.cpp` 调用点基本不动。
2. 上游合并时冲突集中在 `cache.* / cache_node.*` 以及新文件。

### 4.3 Redis 数据模型

键空间统一前缀：

- `s3fs:{bucket}:{mount_prefix_hash}:...`

核心键：

1. `meta:{path}`
   类型: Hash
   字段: `type, mode, uid, gid, size, atime, mtime, ctime, etag, has_meta, meta_blob, update_ts`
2. `neg:{path}`
   类型: String
   值: `1`
   TTL: `negative_ttl`
3. `dir:{path}`
   类型: String（序列化后的目录对象列表，压缩 JSON）
   TTL: `dirlist_ttl`
4. `capacity:used_bytes`
   类型: String(Integer)
5. `capacity:updated_at`
   类型: String(Integer, epoch seconds)
6. `capacity:reconcile_lock`
   类型: String（短时分布式锁）

TTL 策略：

1. `meta:{path}` 使用 `stat_cache_expire` 同步语义（可配置）。
2. `dir:{path}` 默认短 TTL（例如 30-120s）降低陈旧目录风险。
3. `neg:{path}` 默认更短 TTL（例如 10-30s）减少外部写入导致的假阴性。

## 5. 容量展示设计

### 5.1 `statfs` 展示规则

新增 `-o capacity_mode=`：

1. `legacy`（默认）:
   保持现有行为（`f_blocks = bucket_block_count`，`f_bfree = f_blocks`）。
2. `redis`:
   `f_blocks = bucket_size_bytes / f_bsize`
   `f_bfree = max(bucket_size_bytes - used_bytes, 0) / f_bsize`
   `f_bavail = f_bfree`

`capacity_mode=redis` 下仍使用 `-o bucket_size` 作为总配额来源。
若未显式设置 `bucket_size`，默认值设为 `1T`。

### 5.2 `used_bytes` 更新策略

第一阶段采用“增量更新 + 周期校准”：

1. 增量更新:
   在确定对象大小变化的成功路径上更新 Redis `used_bytes`。
2. 周期校准:
   定时扫描 S3（按挂载前缀）重算已用量，写回 Redis。
3. 锁:
   使用 `capacity:reconcile_lock` 防止多实例重复全量扫描。

说明：

1. 若存在 s3fs 外部写入，增量值可能短暂偏差，由校准收敛。
2. 大桶场景可提高校准周期，避免高频全量扫描。

## 6. 关键读写流程

### 6.1 getattr

1. `StatCache(L1)` 命中直接返回。
2. L1 miss -> Redis L2 查询 `meta:{path}` 或 `neg:{path}`。
3. L2 miss -> 走现有 `head_request` 路径。
4. 成功后回填 L1 + L2。

### 6.2 readdir

1. 先查 `GetS3ObjList`（L1 -> L2）。
2. miss 时走现有 `list_bucket`。
3. `readdir_multi_head` 中继续优先查缓存（L1/L2），仅对 miss 发 `HEAD`。
4. 新鲜目录结果写回 `dir:{path}` 与相关 `meta:{child}`。

### 6.3 变更操作

对 `create/unlink/rmdir/rename/truncate/release` 成功路径执行：

1. 精确更新或失效对应 `meta/dir/neg`。
2. 目录级操作使用前缀失效，保证可见性优先于缓存命中。

## 7. 配置与兼容性

新增挂载参数（提议）：

1. `redis_meta=redis://host:port/db`
2. `redis_connect_timeout=`
3. `redis_readwrite_timeout=`
4. `redis_key_prefix=`
5. `redis_meta_ttl=`
6. `redis_dirlist_ttl=`
7. `redis_negative_ttl=`
8. `capacity_mode=legacy|redis`
9. `capacity_reconcile_interval=`

兼容性约束：

1. 不配置 `redis_meta` 时，所有新逻辑旁路。
2. Redis 不可用时自动降级为 L1 + 回源，不影响挂载可用性。
3. `capacity_mode=redis` 且未显式配置 `bucket_size` 时，按 `1T` 处理。

## 8. 构建与依赖改造

1. `configure.ac` 增加可选 `hiredis` 探测与编译开关。
2. `src/Makefile.am` 按开关编译 `redis_metadata_backend.cpp` 等新文件。
3. 文档 `doc/man/s3fs.1.in` 增加新参数说明。

## 9. 测试策略

### 9.1 单元测试

1. Redis 编解码与键模型测试。
2. TTL/过期与 negative cache 行为测试。
3. L1 miss -> L2 hit 回填路径测试。

### 9.2 集成测试

1. 启 Redis + s3proxy，验证 `getattr/readdir` 命中率提升。
2. `capacity_mode=redis` 下验证 `df` 输出变化逻辑。
3. 多实例同时挂载同一桶，验证缓存可复用与降级行为。

### 9.3 回归测试

1. 不启用 Redis 时，复用现有测试，保证行为不变。
2. 对 `rename/unlink/rmdir` 做缓存失效回归。

## 10. 分阶段实施计划

### 阶段 A: 框架接入（低风险）

1. 引入后端接口与空实现。
2. 接入配置项和编译开关。
3. 保证默认路径完全不变。

### 阶段 B: 元数据 L2

1. `GetStat/AddStat/DelStat/GetS3ObjList/AddS3ObjList` 接入 Redis。
2. 完成命中回填逻辑和失效逻辑。

### 阶段 C: 容量增强

1. 新增 `capacity_mode=redis`。
2. 实现 `used_bytes` 增量更新与周期校准。

### 阶段 D: 稳定性与发布

1. 压测与故障注入（Redis 中断、超时）。
2. 完善文档与默认值。

## 11. 上游同步策略

1. 代码组织策略
   将新增逻辑放在新文件中（如 `metadata_backend_*`, `capacity_tracker_*`），避免修改大量上游核心函数。
2. 改动边界策略
   `s3fs.cpp` 仅做参数解析和少量调用点对接。
3. 提交策略
   按“接口层 -> Redis 层 -> 容量层 -> 文档测试层”拆分小提交，便于 `git rebase upstream/master`。
4. 发布策略
   新能力全部由显式参数启用，默认行为与上游一致，降低回归与冲突概率。

## 12. 风险与应对

1. Redis 成为性能瓶颈
   对策: pipeline、批量读写、短路径本地 L1 优先。
2. 外部写入导致缓存陈旧
   对策: 缩短 `dir/negative` TTL + 周期校准。
3. 容量统计偏差
   对策: 增量更新 + 周期全量校准，必要时提供强制重算命令。
4. 上游改动冲突
   对策: 维持薄侵入和可选特性开关。

## 13. 验收标准

1. 功能
   启用 Redis 后，`getattr/readdir` 在热路径明显减少 `HEAD/LIST` 请求次数。
2. 容量
   `capacity_mode=redis` 下 `df` 可显示受配额与已用量影响的剩余空间。
3. 兼容
   不启用 Redis 时，行为与当前版本一致，现有测试通过。
4. 可运维
   Redis 故障时系统可自动降级，不影响基础读写可用性。
