# 使用说明

首次配置或从其他开发板切换到 CoreS3 时，执行：

```bash
idf.py -DBOARD_TYPE=m5stack-core-s3 -DBOARD_NAME=m5stack-core-s3 set-target esp32s3
```

该命令会自动加载 `m5stack-core-s3/sdkconfig.defaults`，选择 CoreS3 使用的
Quad PSRAM 和 GC0308 摄像头配置。不要直接复用其他 ESP32-S3 开发板生成的
`sdkconfig`。

**开发编译**

```bash
idf.py build
```

**发布编译**

```bash
python ./scripts/release.py m5stack-core-s3
```

**烧录**

```bash
idf.py flash
```

> [!NOTE]
> 进入下载模式：长按复位按键(约3秒)，直至内部指示灯亮绿色，松开按键。
