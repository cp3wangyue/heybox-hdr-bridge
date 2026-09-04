# Integration

帧替换与原编码链接回（P5，§9）。**Gate P3（无损透传 POC）与 Gate P4（算法）都通过后才实现。**

- 按编码器输入格式分支：NV12 / BGRA / P010 / I420-CPU（§9.1 表）
- 保持原 Width/Height、帧率、时间戳、裁剪矩形、旋转信息
- 输出资源按原纹理描述建立 BindFlags/MiscFlags/KeyedMutex，不擅自简化
- Debug 诊断叠加层（§9.3）：输入/输出格式、路径、GPU 耗时、Bypass 开关
