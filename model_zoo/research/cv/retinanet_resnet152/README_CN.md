# 1. 内容

<!-- TOC -->

- <span id="content">[Retinanet 描述](#-Retinanet-描述)</span>
- [模型架构](#模型架构)
- [数据集](#数据集)
- [环境要求](#环境要求)
- [脚本说明](#脚本说明)
    - [脚本和示例代码](#脚本和示例代码)
    - [脚本参数](#脚本参数)
    - [训练过程](#训练过程)
        - [用法](#用法)
        - [运行](#运行)
        - [结果](#结果)
    - [评估过程](#评估过程)
        - [用法](#usage)
        - [运行](#running)
        - [结果](#outcome)
    - [模型说明](#模型说明)
        - [性能](#性能)
            - [训练性能](#训练性能)
            - [推理性能](#推理性能)
- [随机情况的描述](#随机情况的描述)
- [ModelZoo 主页](#modelzoo-主页)

<!-- /TOC -->

## [Retinanet 描述](#content)

RetinaNet算法源自2018年Facebook AI Research的论文 Focal Loss for Dense Object Detection。该论文最大的贡献在于提出了Focal Loss用于解决类别不均衡问题，从而创造了RetinaNet（One Stage目标检测算法）这个精度超越经典Two Stage的Faster-RCNN的目标检测网络。

[论文](https://arxiv.org/pdf/1708.02002.pdf)
Lin T Y , Goyal P , Girshick R , et al. Focal Loss for Dense Object Detection[C]// 2017 IEEE International Conference on Computer Vision (ICCV). IEEE, 2017:2999-3007.

## [模型架构](#content)

Retinanet的整体网络架构如下所示：

[链接](https://arxiv.org/pdf/1708.02002.pdf)

## [数据集](#content)

数据集可参考文献.

MSCOCO2017

- 数据集大小: 19.3G, 123287张80类彩色图像

    - 训练:19.3G, 118287张图片

    - 测试:1814.3M, 5000张图片

- 数据格式:RGB图像.

    - 注意：数据将在src/dataset.py 中被处理

## [环境要求](#content)

- 硬件（Ascend）
    - 使用Ascend处理器准备硬件环境。
- 架构
    - [MindSpore](https://www.mindspore.cn/install/en)
- 想要获取更多信息，请检查以下资源：
    - [MindSpore 教程](https://www.mindspore.cn/tutorial/training/en/master/index.html)
    - [MindSpore Python API](https://www.mindspore.cn/doc/api_python/en/master/index.html)

## [脚本说明](#content)

### [脚本和示例代码](#content)

```shell
.
└─Retinanet_resnet152
  ├─README.md
  ├─scripts
    ├─run_single_train.sh                     # 使用Ascend环境单卡训练
    ├─run_distribute_train.sh                 # 使用Ascend环境八卡并行训练
    ├─run_eval.sh                             # 使用Ascend环境运行推理脚本
  ├─src
    ├─backbone.py                             # 网络模型定义
    ├─bottleneck.py                           # 网络颈部定义
    ├─config.py                               # 参数配置
    ├─dataset.py                              # 数据预处理
    ├─retinahead.py                           # 网络预测头部定义
    ├─init_params.py                          # 参数初始化
    ├─lr_generator.py                         # 学习率生成函数
    ├─coco_eval                               # coco数据集评估
    ├─box_utils.py                            # 先验框设置
    ├─_init_.py                               # 初始化
  ├─train.py                                  # 网络训练脚本
  └─eval.py                                   # 网络推理脚本

```

### [脚本参数](#content)

```python
在train.py和config.py脚本中使用到的主要参数是:
"img_shape": [640, 640],                                                                        # 图像尺寸
"num_retinanet_boxes": 76725,                                                                   # 设置的先验框总数
"match_thershold": 0.5,                                                                         # 匹配阈值
"softnms_sigma": 0.5,                                                                           # softnms算法σ值
"nms_thershold": 0.6,                                                                           # 非极大抑制阈值
"min_score": 0.1,                                                                               # 最低得分
"max_boxes": 100,                                                                               # 检测框最大数量
"global_step": 0,                                                                               # 全局步数
"lr_init": 1e-6,                                                                                # 初始学习率
"lr_end_rate": 5e-3,                                                                            # 最终学习率与最大学习率的比值
"warmup_epochs1": 2,                                                                            # 第一阶段warmup的周期数
"warmup_epochs2": 5,                                                                           # 第二阶段warmup的周期数
"warmup_epochs3": 23,                                                                           # 第三阶段warmup的周期数
"warmup_epochs4": 60,                                                                          # 第四阶段warmup的周期数
"warmup_epochs5": 160,                                                                          # 第五阶段warmup的周期数
"momentum": 0.9,                                                                                # momentum
"weight_decay": 1.5e-4,                                                                         # 权重衰减率
"num_default": [9, 9, 9, 9, 9],                                                                 # 单个网格中先验框的个数
"extras_out_channels": [256, 256, 256, 256, 256],                                               # 特征层输出通道数
"feature_size": [75, 38, 19, 10, 5],                                                            # 特征层尺寸
"aspect_ratios": [(0.5,1.0,2.0), (0.5,1.0,2.0), (0.5,1.0,2.0), (0.5,1.0,2.0), (0.5,1.0,2.0)],   # 先验框大小变化比值
"steps": ( 8, 16, 32, 64, 128),                                                                 # 先验框设置步长
"anchor_size":(32, 64, 128, 256, 512),                                                          # 先验框尺寸
"prior_scaling": (0.1, 0.2),                                                                    # 用于调节回归与回归在loss中占的比值
"gamma": 2.0,                                                                                   # focal loss中的参数
"alpha": 0.75,                                                                                 # focal loss中的参数
"mindrecord_dir": "/opr/root/data/MindRecord_COCO",                                             # mindrecord文件路径
"coco_root": "/opr/root/data/",                                                                 # coco数据集路径
"train_data_type": "train2017",                                                                 # train图像的文件夹名
"val_data_type": "val2017",                                                                     # val图像的文件夹名
"instances_set": "annotations_trainval2017/annotations/instances_{}.json",                     # 标签文件路径
"coco_classes": ('background', 'person', 'bicycle', 'car', 'motorcycle', 'airplane', 'bus',     # coco数据集的种类
                 'train', 'truck', 'boat', 'traffic light', 'fire hydrant',
                 'stop sign', 'parking meter', 'bench', 'bird', 'cat', 'dog',
                 'horse', 'sheep', 'cow', 'elephant', 'bear', 'zebra',
                 'giraffe', 'backpack', 'umbrella', 'handbag', 'tie',
                 'suitcase', 'frisbee', 'skis', 'snowboard', 'sports ball',
                 'kite', 'baseball bat', 'baseball glove', 'skateboard',
                 'surfboard', 'tennis racket', 'bottle', 'wine glass', 'cup',
                 'fork', 'knife', 'spoon', 'bowl', 'banana', 'apple',
                 'sandwich', 'orange', 'broccoli', 'carrot', 'hot dog', 'pizza',
                 'donut', 'cake', 'chair', 'couch', 'potted plant', 'bed',
                 'dining table', 'toilet', 'tv', 'laptop', 'mouse', 'remote',
                 'keyboard', 'cell phone', 'microwave', 'oven', 'toaster', 'sink',
                 'refrigerator', 'book', 'clock', 'vase', 'scissors',
                 'teddy bear', 'hair drier', 'toothbrush'),
"num_classes": 81,                                                                              # 数据集类别数
"voc_root": "",                                                                                 # voc数据集路径
"voc_dir": "",
"image_dir": "",                                                                                # 图像路径
"anno_path": "",                                                                                # 标签文件路径
"save_checkpoint": True,                                                                        # 保存checkpoint
"save_checkpoint_epochs": 1,                                                                    # 保存checkpoint epoch数
"keep_checkpoint_max":1,                                                                        # 保存checkpoint的最大数量
"save_checkpoint_path": "./model",                                                              # 保存checkpoint的路径
"finish_epoch":0,                                                                               # 已经运行完成的 epoch 数
"checkpoint_path":"/opr/root/reretina/retinanet2/LOG0/model/retinanet-400_458.ckpt"             # 用于验证的checkpoint路径
```

### [训练过程](#content)

#### 用法

您可以使用python或shell脚本进行训练。shell脚本的用法如下:

- Ascend:

```训练
# 八卡并行训练示例：

创建 RANK_TABLE_FILE
sh run_distribute_train.sh DEVICE_NUM EPOCH_SIZE LR DATASET RANK_TABLE_FILE PRE_TRAINED(optional) PRE_TRAINED_EPOCH_SIZE(optional)

# 单卡训练示例：

sh run_distribute_train.sh DEVICE_ID EPOCH_SIZE LR DATASET PRE_TRAINED(optional) PRE_TRAINED_EPOCH_SIZE(optional)

```

> 注意:

  RANK_TABLE_FILE相关参考资料见[链接](https://www.mindspore.cn/tutorial/training/en/master/advanced_use/distributed_training_ascend.html), 获取device_ip方法详见[链接](https://gitee.com/mindspore/mindspore/tree/master/model_zoo/utils/hccl_tools).

#### 运行

``` 运行
# 训练示例

  python:
    data和存储mindrecord文件的路径在config里设置

      # 单卡训练示例：

      python train.py
  shell:
      Ascend:

      # 八卡并行训练示例(在retinanet目录下运行)：

      sh scripts/run_distribute_train.sh 8 500 0.1 coco RANK_TABLE_FILE(创建的RANK_TABLE_FILE的地址) PRE_TRAINED(预训练checkpoint地址) PRE_TRAINED_EPOCH_SIZE（预训练EPOCH大小）
      例如：sh scripts/run_distribute_train.sh 8 500 0.1 coco scripts/rank_table_8pcs.json /dataset/retinanet-322_458.ckpt 322

      # 单卡训练示例(在retinanet目录下运行)：

      sh scripts/run_single_train.sh 0 500 0.1 coco /dataset/retinanet-322_458.ckpt 322

```

#### 结果

训练结果将存储在示例路径中。checkpoint将存储在 `./model` 路径下，训练日志将被记录到 `./log.txt` 中，训练日志部分示例如下：

``` 训练日志
epoch: 117 step: 916, loss is 0.8391866
lr:[0.025187]
epoch time: 444315.944 ms, per step time: 485.061 ms
epoch: 118 step: 916, loss is 1.070719
lr:[0.025749]
epoch time: 444288.450 ms, per step time: 485.031 ms
epoch: 119 step: 916, loss is 1.1607553
lr:[0.026312]
epoch time: 444339.538 ms, per step time: 485.087 ms
epoch: 120 step: 916, loss is 1.174742
lr:[0.026874]
epoch time: 444237.851 ms, per step time: 484.976 ms
```

### [评估过程](#content)

#### <span id="usage">用法</span>

您可以使用python或shell脚本进行训练。shell脚本的用法如下:

```eval
sh scripts/run_eval.sh [DATASET] [DEVICE_ID]
```

#### <span id="running">运行</span>

```eval运行
# 验证示例

  python:
      Ascend: python eval.py
  checkpoint 的路径在config里设置
  shell:
      Ascend: sh scripts/run_eval.sh coco 0
```

> checkpoint 可以在训练过程中产生.

#### <span id="outcome">结果</span>

计算结果将存储在示例路径中，您可以在 `eval.log` 查看.

``` mAP
 Average Precision  (AP) @[ IoU=0.50:0.95 | area=   all | maxDets=100 ] = 0.357
 Average Precision  (AP) @[ IoU=0.50      | area=   all | maxDets=100 ] = 0.503
 Average Precision  (AP) @[ IoU=0.75      | area=   all | maxDets=100 ] = 0.400
 Average Precision  (AP) @[ IoU=0.50:0.95 | area= small | maxDets=100 ] = 0.141
 Average Precision  (AP) @[ IoU=0.50:0.95 | area=medium | maxDets=100 ] = 0.381
 Average Precision  (AP) @[ IoU=0.50:0.95 | area= large | maxDets=100 ] = 0.498
 Average Recall     (AR) @[ IoU=0.50:0.95 | area=   all | maxDets=  1 ] = 0.307
 Average Recall     (AR) @[ IoU=0.50:0.95 | area=   all | maxDets= 10 ] = 0.447
 Average Recall     (AR) @[ IoU=0.50:0.95 | area=   all | maxDets=100 ] = 0.458
 Average Recall     (AR) @[ IoU=0.50:0.95 | area= small | maxDets=100 ] = 0.172
 Average Recall     (AR) @[ IoU=0.50:0.95 | area=medium | maxDets=100 ] = 0.487
 Average Recall     (AR) @[ IoU=0.50:0.95 | area= large | maxDets=100 ] = 0.643

========================================

mAP: 0.3571988469737286
```

## [模型说明](#content)

### [性能](#content)

#### 训练性能

| 参数                        | Ascend                                |
| -------------------------- | ------------------------------------- |
| 模型名称                    | Retinanet                             |
| 运行环境                    | 华为云 Modelarts                      |
| 上传时间                    | 10/03/2021                           |
| MindSpore 版本             | 1.0.1                                 |
| 数据集                      | 123287 张图片                          |
| Batch_size                 | 16                                   |
| 训练参数                    | src/config.py                         |
| 优化器                      | Momentum                              |
| 损失函数                    | Focal loss                            |
| 最终损失                    | 0.69                               |
| 精确度 (8p)                 | mAP[0.3571]            |
| 训练总时间 (8p)             | 41h32m20s                        |
| 脚本                       | [Retianet script](https://gitee.com/mindspore/mindspore/tree/master/model_zoo/research/cv/retinanet_resnet152) |

#### 推理性能

| 参数                 | Ascend                      |
| ------------------- | :-------------------------- |
| 模型名称             | Retinanet                    |
| 运行环境             | 华为云 Modelarts             |
| 上传时间             | 10/03/2021                 |
| MindSpore 版本      | 1.0.1                        |
| 数据集              | 5k 张图片                   |
| Batch_size          | 1                          |
| 精确度              | mAP[0.3571]                  |
| 总时间              | 12 mins and 03 seconds       |

# [随机情况的描述](#内容)

在 `dataset.py` 脚本中, 我们在 `create_dataset` 函数中设置了随机种子. 我们在 `train.py` 脚本中也设置了随机种子.

# [ModelZoo 主页](#内容)

请核对官方 [主页](https://gitee.com/mindspore/mindspore/tree/master/model_zoo).