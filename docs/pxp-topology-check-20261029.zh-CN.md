# 分流、混合站及动态道岔的核查与复现

本次发布包含此前本地验证的分流路线、独立支线、新沙道岔误等和吕梁 87 号修复。历史报告保存各自构建与输入身份，不应将历史构建结果混成当前版本结果。

## 本轮发现的动态修改缺口

正常轨道修改通过 `YapfNotifyTrackLayoutChange` 使附近保存路线失效；连续合法 PBS 预约仍需保留。此前重建上下文主要在 YAPF 入口进行，但限制检查可能在进入 YAPF 前就因失效路线而返回等待，于是已经登记的列车没有及时恢复有效分流选择。

`ChooseTrainTrack` 现在只在保存路线为空、已有区域上下文、当前为明确车站／路点／车辆段目标时，从原生连续预约末端调用已有 `PrepareFromContext`。恢复只使用原有登记，不获取新路签、不预约轨道、不发布尚未成功的计划；加载、离站、连挂等非明确物理目标继续使用原有调度语义。已有有效计划、普通单出口及优先级策略不变。

新增 `split-crossing-edit` 使用真正的建轨／拆轨命令，在已有选路后改动未预约的道岔。它断言旧计划清空、已预约前缀保持不变、后续选路重建并实际走新道岔，同时检查独立快车仍在区域内时不产生错误持续待避。旧生产对象在同一专项中失败，修复必须通过实际行驶与重载，不能仅通过失效标志断言。

## 混合站与合流分流同时存在

`mixed-rejoin-stop`、`mixed-rejoin-both-stop` 在同一布局中组合不同入口合流、普通待避股道／正常停站平台、站后分流及后续再次合流。后者还要求快车在正线站台完成真实停站。试验使用库存车辆、正常建站／建轨／建信号及调度命令，不写入车辆位置或速度，不清除 PBS，不改寻路惩罚。

近入口有真实接近检测信号；过远快车不一定构成必须等待的追越。平台切换、停站、车尾出清、再次发车及路签释放必须由实际事件证明。正常远快车放行、原正交交叉和既有单待避站测试保留，不为新增测试修改原预期。

## 可重复验收

在配置好项目 Windows Ninja 构建及完整基础图形资源后，执行：

```text
cmake --build build-win64 --target openttd openttd_test
build-win64/runtime/openttd_test.exe
python regression/automatic_yield/run.py --build-dir build-win64 --baseset <完整基础资源目录> --output <输出目录> --braking 0
python regression/automatic_yield/run.py --build-dir build-win64 --baseset <完整基础资源目录> --output <另一输出目录> --braking 1
python regression/automatic_yield/early_route_driving.py --build-dir build-win64 --baseset <完整基础资源目录> --output <输出目录> --braking 0
python regression/automatic_yield/early_route_driving.py --build-dir build-win64 --baseset <完整基础资源目录> --output <另一输出目录> --braking 1
```

核心矩阵每种制动模型 256 次；提前选路矩阵 26 场景、每种制动模型 104 次；另用真实 2cc 资源运行 `grf-basic,grf-200,grf-160-250,grf-consecutive`，两种制动模型共 32 次。合计 752 次行驶／读档。观察器应冻结正式对象并记录身份，不能混用修改前后的对象。

覆盖现有布局、新建普通待避线及第二／第三股道、全站台、混合、多信号、多入口多出口、合流、分流、再次合流、动态增删轨道、满线和连续快车。还应重放玩家原图的新沙下行和吕梁 87／66，检查联机在独立缓存预热及实际轨道改动后的确定性，记录原始输入与结果。

这一覆盖矩阵不是对任意自由站场的穷尽证明。缺少合法停车空间、未知后续调度、控制区之外的远处再次合流、持续饱和、任意 NewGRF 组合和跨机器长期联机，需要各自现场验证。原生已有合法预约下继续采用旧线路，也不能直接认定为过期缓存；禁止为了使用新道岔非法取消预约。
