# 目录

<!-- TOC -->

- [目录](#目录)
- [图卷积网络描述](#图卷积网络描述)
- [模型架构](#模型架构)
- [数据集](#数据集)
- [环境要求](#环境要求)
- [快速入门](#快速入门)
    - [用法](#用法)
    - [启动](#启动)
- [脚本说明](#脚本说明)
    - [脚本及样例代码](#脚本及样例代码)
    - [脚本参数](#脚本参数)
    - [培训、评估、测试过程](#培训评估测试过程)
        - [用法](#用法-1)
        - [启动](#启动-1)
        - [结果](#结果)
    - [推理过程](#推理过程)
        - [导出MindIR](#导出mindir)
        - [在Ascend310执行推理](#在ascend310执行推理)
        - [结果](#结果)
- [模型描述](#模型描述)
    - [性能](#性能)
- [随机情况说明](#随机情况说明)
- [ModelZoo主页](#modelzoo主页)

<!-- /TOC -->

## 图卷积网络描述

图卷积网络（GCN）于2016年提出，旨在对图结构数据进行半监督学习。它提出了一种基于卷积神经网络有效变体的可扩展方法，可直接在图上操作。该模型在图边缘的数量上线性缩放，并学习隐藏层表示，这些表示编码了局部图结构和节点特征。

[论文](https://arxiv.org/abs/1609.02907):  Thomas N. Kipf, Max Welling.2016.Semi-Supervised Classification with Graph Convolutional Networks.In ICLR 2016.

## 模型架构

GCN包含两个图卷积层。每一层以节点特征和邻接矩阵为输入，通过聚合相邻特征来更新节点特征。

## 数据集

| 数据集  | 类型             | 节点 | 边 | 类 | 特征 | 标签率 |
| -------  | ---------------:|-----:| ----:| ------:|--------:| ---------:|
| Cora    | Citation network | 2708  | 5429  | 7       | 1433     | 0.052      |
| Citeseer| Citation network | 3327  | 4732  | 6       | 3703     | 0.036      |

## 环境要求

- 硬件（Ascend处理器）
    - 准备Ascend或GPU处理器搭建硬件环境。
- 框架
    - [MindSpore](https://gitee.com/mindspore/mindspore)
- 如需查看详情，请参见如下资源：
    - [MindSpore教程](https://www.mindspore.cn/tutorial/training/zh-CN/master/index.html)
    - [MindSpore Python API](https://www.mindspore.cn/doc/api_python/zh-CN/master/index.html)

## 快速入门

- 安装[MindSpore](https://www.mindspore.cn/install)

- 从github下载/kimiyoung/planetoid提供的数据集Cora或Citeseer

- 将数据集放到任意路径，文件夹应该包含如下文件（以Cora数据集为例）：

```text
.
└─data
    ├─ind.cora.allx
    ├─ind.cora.ally
    ├─ind.cora.graph
    ├─ind.cora.test.index
    ├─ind.cora.tx
    ├─ind.cora.ty
    ├─ind.cora.x
    └─ind.cora.y
```

- 为Cora或Citeseer生成MindRecord格式的数据集

### 用法

```buildoutcfg
cd ./scripts
# SRC_PATH为下载的数据集文件路径，DATASET_NAME为Cora或Citeseer
sh run_process_data.sh [SRC_PATH] [DATASET_NAME]
```

### 启动

```text
# 为Cora生成MindRecord格式的数据集
sh run_process_data.sh ./data cora
# 为Citeseer生成MindRecord格式的数据集
sh run_process_data.sh ./data citeseer
```

- Running on local with Ascend

```bash
# 在 cora 或 citeseer 数据集上训练, DATASET_NAME 设置为 cora 或 citeseer
sh run_train.sh [DATASET_NAME]
```

- Running on [ModelArts](https://support.huaweicloud.com/modelarts/)

```bash
# 在 ModelArts 上使用 单卡训练 cora 数据集
# (1) 执行a或者b
#       a. 在 default_config.yaml 文件中设置 "enable_modelarts=True"
#          在 default_config.yaml 文件中设置 "data_dir='/cache/data/cora'"
#          在 default_config.yaml 文件中设置 "train_nodes_num=140"
#          在 default_config.yaml 文件中设置 其他参数
#       b. 在网页上设置 "enable_modelarts=True"
#          在网页上设置 "data_dir='/cache/data/cora'"
#          在网页上设置 "train_nodes_num=140"
#          在网页上设置 其他参数
# (2) 上传你的数据集到 S3 桶上
# (3) 在网页上设置你的代码路径为 "/path/gcn"
# (4) 在网页上设置启动文件为 "train.py"
# (5) 在网页上设置"训练数据集"、"训练输出文件路径"、"作业日志路径"等
# (6) 创建训练作业
#
# 在 ModelArts 上使用 单卡训练 citeseer 数据集
# (1) 执行a或者b
#       a. 在 default_config.yaml 文件中设置 "enable_modelarts=True"
#          在 default_config.yaml 文件中设置 "data_dir='/cache/data/citeseer'"
#          在 default_config.yaml 文件中设置 "train_nodes_num=120"
#          在 default_config.yaml 文件中设置 其他参数
#       b. 在网页上设置 "enable_modelarts=True"
#          在网页上设置 "data_dir='/cache/data/citeseer'"
#          在网页上设置 "train_nodes_num=120"
#          在网页上设置 其他参数
# (2) 上传你的数据集到 S3 桶上
# (3) 在网页上设置你的代码路径为 "/path/gcn"
# (4) 在网页上设置启动文件为 "train.py"
# (5) 在网页上设置"训练数据集"、"训练输出文件路径"、"作业日志路径"等
# (6) 创建训练作业
```

## 脚本说明

### 脚本及样例代码

```shell
.
└─gcn
  ├─README.md
  ├─README_CN.md
  ├─model_utils
  | ├─__init__.py       # 初始化文件
  | ├─config.py         # 参数配置
  | ├─device_adapter.py # ModelArts的设备适配器
  | ├─local_adapter.py  # 本地适配器
  | └─moxing_adapter.py # ModelArts的模型适配器
  |
  ├─scripts
  | ├─run_infer_310.sh     # Ascend310 推理shell脚本
  | ├─run_process_data.sh  # 生成MindRecord格式的数据集
  | └─run_train.sh         # 启动训练，目前只支持Ascend后端
  |
  ├─src
  | ├─config.py            # 参数配置
  | ├─dataset.py           # 数据预处理
  | ├─gcn.py               # GCN骨干
  | └─metrics.py           # 损失和准确率
  |
  ├─default_config.py      # 配置文件
  ├─export.py              # 导出脚本
  ├─mindspore_hub_conf.py  # mindspore hub 脚本
  ├─postprocess.py         # 后处理脚本
  ├─preprocess.py          # 预处理脚本
  └─train.py               # 训练网络，每个训练轮次后评估验证结果收敛后，训练停止，然后进行测试。
```

### 脚本参数

训练参数可以在config.py中配置。

```text
"learning_rate": 0.01,            # 学习率
"epochs": 200,                    # 训练轮次
"hidden1": 16,                    # 第一图卷积层隐藏大小
"dropout": 0.5,                   # 第一图卷积层dropout率
"weight_decay": 5e-4,             # 第一图卷积层参数的权重衰减
"early_stopping": 10,             # 早停容限
```

### 培训、评估、测试过程

#### 用法

```text
# 使用Cora或Citeseer数据集进行训练，DATASET_NAME为Cora或Citeseer
sh run_train.sh [DATASET_NAME]
```

#### 启动

```bash
sh run_train.sh cora
```

#### 结果

训练结果将保存在脚本路径下，文件夹名称以“train”开头。您可在日志中找到如下结果：

```text
Epoch:0001 train_loss= 1.95373 train_acc= 0.09286 val_loss= 1.95075 val_acc= 0.20200 time= 7.25737
Epoch:0002 train_loss= 1.94812 train_acc= 0.32857 val_loss= 1.94717 val_acc= 0.34000 time= 0.00438
Epoch:0003 train_loss= 1.94249 train_acc= 0.47857 val_loss= 1.94337 val_acc= 0.43000 time= 0.00428
Epoch:0004 train_loss= 1.93550 train_acc= 0.55000 val_loss= 1.93957 val_acc= 0.46400 time= 0.00421
Epoch:0005 train_loss= 1.92617 train_acc= 0.67143 val_loss= 1.93558 val_acc= 0.45400 time= 0.00430
...
Epoch:0196 train_loss= 0.60326 train_acc= 0.97857 val_loss= 1.05155 val_acc= 0.78200 time= 0.00418
Epoch:0197 train_loss= 0.60377 train_acc= 0.97143 val_loss= 1.04940 val_acc= 0.78000 time= 0.00418
Epoch:0198 train_loss= 0.60680 train_acc= 0.95000 val_loss= 1.04847 val_acc= 0.78000 time= 0.00414
Epoch:0199 train_loss= 0.61920 train_acc= 0.96429 val_loss= 1.04797 val_acc= 0.78400 time= 0.00413
Epoch:0200 train_loss= 0.57948 train_acc= 0.96429 val_loss= 1.04753 val_acc= 0.78600 time= 0.00415
Optimization Finished!
Test set results: cost= 1.00983 accuracy= 0.81300 time= 0.39083
...
```

## 推理过程

### [导出MindIR](#contents)

```shell
python export.py --ckpt_file [CKPT_PATH] --file_name [FILE_NAME] --file_format [FILE_FORMAT]
```

参数ckpt_file为必填项，
`EXPORT_FORMAT` 必须在 ["AIR", "MINDIR"]中选择。

### 在Ascend310执行推理

在执行推理前，mindir文件必须通过`export.py`脚本导出。以下展示了使用minir模型执行推理的示例。

```shell
# Ascend310 推理
bash run_infer_310.sh [MINDIR_PATH] [DATASET_NAME] [DATASET_PATH] [NEED_PREPROCESS] [DEVICE_ID]
```

- `DATASET_NAME` 表示数据集名称，取值范围： ['cora', 'citeseer']。
- `NEED_PREPROCESS` 表示数据是否需要预处理，取值范围：'y' 或者 'n'。
- `DEVICE_ID` 可选，默认值为0。

### result

推理结果保存在脚本执行的当前路径，你可以在acc.log中看到以下精度计算结果。

```bash
Test set results: accuracy= 0.81300
```

## 模型描述

### 性能

| 参数                 | GCN                                                            |
| -------------------------- | -------------------------------------------------------------- |
| 资源                   | Ascend 910；系统 Euler2.8                                                     |
| 上传日期              | 2020-06-09                                    |
| MindSpore版本          | 0.5.0-beta                                                     |
| 数据集                    | Cora/Citeseer                                                  |
| 训练参数        | epoch=200                                                      |
| 优化器                 | Adam                                                           |
| 损失函数              | Softmax交叉熵                                          |
| 准确率                   | 81.5/70.3                                                      |
| 参数(B)             | 92160/59344                                                    |
| 脚本                    | <https://gitee.com/mindspore/mindspore/tree/master/model_zoo/official/gnn/gcn> |

## 随机情况说明

以下两种随机情况：

- 根据入参--seed在train.py中设置种子。
- 随机失活操作。

train.py已经设置了一些种子，避免权重初始化的随机性。若需关闭随机失活，将src/config.py中相应的dropout_prob参数设置为0。

## ModelZoo主页

请浏览官网[主页](https://gitee.com/mindspore/mindspore/tree/master/model_zoo)。
