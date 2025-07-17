# Slurm Docker Cluster

<p align="center">
    <b> <a href="../README.md">English</a> | 简体中文 </b>
</p>

**Slurm Docker Cluster**借助Docker Compose实现多容器Slurm集群的敏捷部署。本仓库简化了
开发、测试、轻量化应用下的健壮Slurm环境配置流程。

## 🏁 快速入门

为了在Docker中启动和运行Slurm，首先要确保以下工具已经被安装：

- **[Docker](https://docs.docker.com/get-docker/)**
- **[Docker Compose](https://docs.docker.com/compose/install/)**

然后克隆仓库：

```bash
git clone https://github.com/giovtorres/slurm-docker-cluster.git
cd slurm-docker-cluster
```

## 📦 容器和存储卷

本仓库所形成的部署由以下容器构成：

- **mysql**: 存储任务和集群数据；
- **slurmdbd**: 管理Slurm数据库；
- **slurmctld**: 负责任务和资源管理的Slurm控制节点；
- **c1, c2**: 计算节点（其上运行`slurmd`守护进程）。

### 持久化卷:
| 存储卷名 | 挂载点 |
| ------- | ------- |
| `etc_munge` | `/etc/munge` |
| `etc_slurm` | `/etc/slurm` |
| `slurm_jobdir` | `/data` |
| `var_lib_mysql` | `/var/lib/mysql` |
| `var_log_slurm` | `/var/log/slurm` |

## 🛠️ 构建Docker镜像

Docker Compose过程会自动提取`.env`文件中的内容，用于配置Slurm项目版本和Docker构建过程。

根据需要更新`.env`文件中的`SLURM_TAG`和`IMAGE_TAG`，然后通过以下命令构建镜像：

```bash
docker compose build
```

另一方面，你也可以直接通过`docker build`进行构建，此时需要在构建参数（`--build-arg`）中指定
[SLURM_TAG](https://github.com/SchedMD/slurm/tags)，并借助 ***(IMAGE_TAG)*** 标记容器版本以实现相同效果。

```bash
docker build --build-arg SLURM_TAG="slurm-21-08-6-1" -t slurm-docker-cluster:21.08.6 .
```

## 🚀 启动集群

当镜像构建完毕，就可以使用Docker Compose部署默认版本的slurm集群。

```bash
docker compose up -d
```

如果想要覆盖`.env`中的设置，并指定特定的slurm版本，需使用`IMAGE_TAG`环境变量:

```bash
IMAGE_TAG=21.08.6 docker compose up -d
```

该命令会以后台模式启动全部容器。你可以通过如下命令监控其状态：

```bash
docker compose ps
```

## 📝 注册集群

在容器启动并处于`running`状态后，通过 **SlurmDBD** 注册集群：

```bash
./register_cluster.sh
```

> **提示**: 在容器运行后稍等一会儿，等待守护进程进行初始化完成后在运行注册脚本，以避免类似如下的连接错误：
> `sacctmgr: error: Problem talking to the database: Connection refused`.

如要查看实时日志，使用：

```bash
docker compose logs -f
```

## 🖥️ 访问集群

通过在`slurmctld`容器中启动shell的方式与Slurm控制器交互：

```bash
docker exec -it slurmctld bash
```

在容器中，你可以运行任何Slurm命令：

```bash
[root@slurmctld /]# sinfo
PARTITION AVAIL  TIMELIMIT  NODES  STATE NODELIST
normal*      up 5-00:00:00      2   idle c[1-2]
```

## 🧑‍💻 提交任务

集群的所有节点挂载了`slurm_jobdir`数据卷，这使得任务文件可以通过`/data`目录共享。使用以下命令提交任务：

```bash
[root@slurmctld /]# cd /data/
[root@slurmctld data]# sbatch --wrap="hostname"
Submitted batch job 2
```

为了查看任务输出：

```bash
[root@slurmctld data]# cat slurm-2.out
c1
```

## 🔄 集群管理

### 停止与重启:

停止集群（不会移除任何容器）：

```bash
docker compose stop
```

随后在此启动它：

```bash
docker compose start
```

### 删除集群:

使用以下命令完全移除所有容器和相关的数据卷：

```bash
docker compose down -v
```

## ⚙️ 高级配置

你可以修改Slurm配置(`slurm.conf`, `slurmdbd.conf`)，无需重新构建容器。实用脚本`update_slurmfiles.sh`会检查配置更新并将配置分发进容器当中：
```bash
./update_slurmfiles.sh slurm.conf slurmdbd.conf
docker compose restart
```

这使得动态地添加和删除节点或者测试配置变得很容易。

## 🤝 共享

我们很欢迎社区共享！如果你想要添加特性，修补BUG，或者提升文档：

1. Fork本仓库.
2. 创建一个新的分支: `git checkout -b feature/your-feature`.
3. 提交Pull Request.

## 📄 协议

本项目遵循[MIT License](LICENSE).
