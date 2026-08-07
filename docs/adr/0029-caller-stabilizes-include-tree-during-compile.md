# ADR-0029 caller stabilizes include tree during compile

状态: 生效
日期: 2026-08
影响: filesystem-backed shader discovery/compile、shader JIT、RadRay DXC fork

## 背景

filesystem-backed 语义要求 compiler 在预处理需要 include 时读取当前磁盘文件。一次 batch 可能
包含多个 target lane，discovery 与 concrete compile 也可能是相邻但独立的 invocation。若 RadRay
内部为此建立 source snapshot、跨调用缓存或文件锁，就会重新承担外部构建系统的版本协调职责，并
偏离用户要求的即时 filesystem 读取。

## 决策

caller/build system 负责在一次 discovery、一次 concrete compile 和一次 multi-target batch 的
稳定窗口内不修改 include tree。RadRay/DXC 不创建跨 invocation 的 include snapshot、文件锁或
include content cache；每个 invocation 按标准 DXC 规则读取当时的文件系统内容。违反稳定窗口时，
结果由实际文件打开时序决定，不提供额外一致性承诺。

## 放弃的方案及代价

- **compiler 内部收集并复用 include snapshot**：重新引入 caller-owned closure 的同类复杂度，
  且会掩盖磁盘源码已经变化的事实。
- **围绕 shaderlib 加文件锁**：RadRay 无法控制外部编辑器、构建器或其他进程的锁协议，锁也会把
  普通读取路径变成隐藏的同步点。
- **把 include 版本写入 request identity**：违背 include 内容不属于 shader identity 的决定，且
  仍不能保证多个 lane 的原子读取。

## 必须保持为真

- compiler 每次需要 include 时直接访问 filesystem。
- discovery/compile/batch 之间不共享 RadRay include cache 或 snapshot。
- include tree 的写入协调属于 caller/build system，不属于 JIT 或 DXC fork。
