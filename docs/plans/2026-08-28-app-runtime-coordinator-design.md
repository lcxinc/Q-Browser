# AppRuntimeCoordinator 设计

## 目标

为同一应用的多个标签页建立单一的包版本权威，同时让每个标签页保留独立的
Worker 监督状态和不可变的已验证包租约。Task14 只实现生命周期线程上的纯协调器；
现有 `UpdateLifecycleCoordinator` 和 Host 的异步 drain 适配在后续任务迁移。

## 边界与所有权

- `PackageStore` 仍是持久化激活状态的唯一写入者。协调器通过 friend 访问现有
  的原子激活、LKG 和回滚内部，不把这些内部方法扩展成公共 API。
- `PackageInstaller` 负责签名、内容、manifest、入口和不可变文件验证。协调器只
  保存 `VerifiedPackageLease`，不会把包目录字符串当作授权本身。
- 协调器按 app ID 实例化并只在 lifecycle 线程调用。每个
  `TabLaunchAuthority(tabId, runtimeIncarnation)` 拥有一个 `WorkerSupervisor`、
  当前租约、attempt key 和共享 `AuthorityAdmissionToken`。
- 事件必须用完整的 tab、runtime incarnation、attempt 和 lease epoch 定位；
  `WorkerAttemptKey` 单独使用永远不构成身份。

## 版本状态

协调器维护 current、candidate 和 LKG 描述符。安装操作调用现有 installer，完成
候选提交和激活，但不发出 Launch action；随后重新验证当前目录并生成 candidate
描述符。旧标签的 lease 不随 current 改变而改写，新标签和用户 reload 读取 current
candidate，崩溃 restart 读取该标签已经固定的 lease。离线启动只验证并描述持久化的
current/LKG，同样不自动启动 Worker。

候选第一次达到持续健康窗口时，协调器只调用一次 `markCurrentLastKnownGood`。
Store generation 变化不能使同版本的其他 candidate lease 失效；候选租约的内容和
authority epoch 仍由协调器显式比较。

## Action 与结果

公共 API 使用计划中定义的 `AppRuntimeResult`、`AppRuntimeAction`、
`FullAttemptKey` 和 `AuthorityDrainBatch`。action vector 是消费者唯一的执行契约，
不能从缺少的 sibling action 推断状态。正常启动/重启按稳定 tab ID 排序；rollback
的顺序固定为所有 `Revoke`、所有 `Stop`，再是 `AwaitAuthorityDrain`，drain 完成后
才允许 `RecoverFromLkg`/`Launch`。

## 两阶段 authority drain

候选 crash-loop 时：

1. lifecycle 先收集同一失败候选的完整受影响集合，并对每个 token 调用
   `beginRevoke()`；此阶段不等待条件变量，也不重新打开 token。
2. 生成一个 batch ID、一个共享 monotonic deadline 和所有 revocation tickets，
   通过 `AwaitAuthorityDrain` action 交给注入的 fake consumer。真实等待发生在
   lifecycle 之外；测试通过 `authorityDrainCompleted` 或
   `authorityDrainTimedOut` 回送结果。

在 deadline 前完成时，按稳定顺序恢复受影响标签。超时时先发
`FailedClosed`、`Stop`、`IsolateSession`，保留后台 drain，禁止该 batch 的任何
恢复或启动；稍后的 completed 事件只做清理。无关 app、已固定旧版本标签和 sibling
的 heartbeat/admission 继续处理。

## 事件规则

- admission 必须匹配完整 key、租约内容和当前未撤销 token；迟到或已撤销事件返回
  `IgnoredStale`/`Rejected`，不产生 action。
- clean exit、startup/admission failure、crash exit、cleanup failure 保持不同的
  结果路径；cleanup 的致命失败进入 `FailedClosed`。
- shutdown 将所有已 admission 的标签退休，并拒绝之后的事件。
- telemetry 只记录一次 app 级健康晋升和 rollback transition，不写入 tab ID、
  PID、route 或用户数据。

## 测试策略

先在 `tst_app_runtime_coordinator` 中写纯状态 RED 测试，使用 deterministic fake
drain consumer，不依赖 Host、GUI、进程或网络。测试覆盖候选/旧版本租约隔离、独立
attempt、一次性晋升、完整 rollback fan-out、超时 failed-closed、迟到事件、shutdown
和 action 顺序；随后与现有 update/crash/offline/production 测试一起构建运行。
