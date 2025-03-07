---
sidebar_label: 元数据
title: 存储元数据的 Information_Schema 数据库
---

TDengine 内置了一个名为 `INFORMATION_SCHEMA` 的数据库，提供对数据库元数据、数据库系统信息和状态的访问，例如数据库或表的名称，当前执行的 SQL 语句等。该数据库存储有关 TDengine 维护的所有其他数据库的信息。它包含多个只读表。实际上，这些表都是视图，而不是基表，因此没有与它们关联的文件。所以对这些表只能查询，不能进行 INSERT 等写入操作。`INFORMATION_SCHEMA` 数据库旨在以一种更一致的方式来提供对 TDengine 支持的各种 SHOW 语句（如 SHOW TABLES、SHOW DATABASES）所提供的信息的访问。与 SHOW 语句相比，使用 SELECT ... FROM INFORMATION_SCHEMA.tablename 具有以下优点：

1. 可以使用 USE 语句将 INFORMATION_SCHEMA 设为默认数据库
2. 可以使用 SELECT 语句熟悉的语法，只需要学习一些表名和列名
3. 可以对查询结果进行筛选、排序等操作。事实上，可以使用任意 TDengine 支持的 SELECT 语句对 INFORMATION_SCHEMA 中的表进行查询
4. TDengine 在后续演进中可以灵活的添加已有 INFORMATION_SCHEMA 中表的列，而不用担心对既有业务系统造成影响
5. 与其他数据库系统更具互操作性。例如，Oracle 数据库用户熟悉查询 Oracle 数据字典中的表

Note: 由于 SHOW 语句已经被开发者熟悉和广泛使用，所以它们仍然被保留。

本章将详细介绍 `INFORMATION_SCHEMA` 这个内置元数据库中的表和表结构。

## INS_DNODES

提供 dnode 的相关信息。也可以使用 SHOW DNODES 来查询这些信息。

| #   |    **列名**    | **数据类型** | **说明**                  |
| --- | :------------: | ------------ | ------------------------- |
| 1   |     vnodes     | SMALLINT     | dnode 中的实际 vnode 个数 |
| 2   | support_vnodes | SMALLINT     | 最多支持的 vnode 个数     |
| 3   |     status     | BINARY(10)   | 当前状态                  |
| 4   |      note      | BINARY(256)  | 离线原因等信息            |
| 5   |       id       | SMALLINT     | dnode id                  |
| 6   |    endpoint    | BINARY(134)  | dnode 的地址              |
| 7   |     create     | TIMESTAMP    | 创建时间                  |

## INS_MNODES

提供 mnode 的相关信息。也可以使用 SHOW MNODES 来查询这些信息。

| #   |  **列名**   | **数据类型** | **说明**           |
| --- | :---------: | ------------ | ------------------ |
| 1   |     id      | SMALLINT     | mnode id           |
| 2   |  endpoint   | BINARY(134)  | mnode 的地址       |
| 3   |    role     | BINARY(10)   | 当前角色           |
| 4   |  role_time  | TIMESTAMP    | 成为当前角色的时间 |
| 5   | create_time | TIMESTAMP    | 创建时间           |

## INS_MODULES

提供组件的相关信息。也可以使用 SHOW MODULES 来查询这些信息

| #   | **列名** | **数据类型** | **说明**   |
| --- | :------: | ------------ | ---------- |
| 1   |    id    | SMALLINT     | module id  |
| 2   | endpoint | BINARY(134)  | 组件的地址 |
| 3   |  module  | BINARY(10)   | 组件状态   |

## INS_QNODES

当前系统中 QNODE 的信息。也可以使用 SHOW QNODES 来查询这些信息。

| #   |  **列名**   | **数据类型** | **说明**     |
| --- | :---------: | ------------ | ------------ |
| 1   |     id      | SMALLINT     | qnode id     |
| 2   |  endpoint   | BINARY(134)  | qnode 的地址 |
| 3   | create_time | TIMESTAMP    | 创建时间     |

## INS_CLUSTER

存储集群相关信息。

| #   |  **列名**   | **数据类型** | **说明**   |
| --- | :---------: | ------------ | ---------- |
| 1   |     id      | BIGINT       | cluster id |
| 2   |    name     | BINARY(134)  | 集群名称   |
| 3   | create_time | TIMESTAMP    | 创建时间   |

## INS_DATABASES

提供用户创建的数据库对象的相关信息。也可以使用 SHOW DATABASES 来查询这些信息。

| #   |       **列名**       | **数据类型**     | **说明**                                         |
| --- | :------------------: | ---------------- | ------------------------------------------------ |
| 1   |         name         | BINARY(32)       | 数据库名                                         |
| 2   |     create_time      | TIMESTAMP        | 创建时间                                         |
| 3   |       ntables        | INT              | 数据库中表的数量，包含子表和普通表但不包含超级表 |
| 4   |       vgroups        | INT              | 数据库中有多少个 vgroup                          |
| 6   |       replica        | INT              | 副本数                                           |
| 7   |        quorum        | BINARY(3)        | 强一致性                                         |
| 8   |       duration       | INT              | 单文件存储数据的时间跨度                         |
| 9   |         keep         | INT              | 数据保留时长                                     |
| 10  |        buffer        | INT              | 每个 vnode 写缓存的内存块大小，单位 MB           |
| 11  |       pagesize       | INT              | 每个 VNODE 中元数据存储引擎的页大小，单位为 KB   |
| 12  |        pages         | INT              | 每个 vnode 元数据存储引擎的缓存页个数            |
| 13  |       minrows        | INT              | 文件块中记录的最大条数                           |
| 14  |       maxrows        | INT              | 文件块中记录的最小条数                           |
| 15  |         comp         | INT              | 数据压缩方式                                     |
| 16  |      precision       | BINARY(2)        | 时间分辨率                                       |
| 17  |        status        | BINARY(10)       | 数据库状态                                       |
| 18  |      retention       | BINARY (60)      | 数据的聚合周期和保存时长                         |
| 19  |    single_stable     | BOOL             | 表示此数据库中是否只可以创建一个超级表           |
| 20  |      cachemodel      | BINARY(60)       | 表示是否在内存中缓存子表的最近数据               |
| 21  |      cachesize       | INT              | 表示每个 vnode 中用于缓存子表最近数据的内存大小  |
| 22  |      wal_level       | INT              | WAL 级别                                         |
| 23  |   wal_fsync_period   | INT              | 数据落盘周期                                     |
| 24  | wal_retention_period | INT              | WAL 的保存时长                                   |
| 25  |  wal_retention_size  | INT              | WAL 的保存上限                                   |
| 26  |   wal_roll_period    | INT              | wal 文件切换时长                                 |
| 27  |   wal_segment_size   | wal 单个文件大小 |

## INS_FUNCTIONS

用户创建的自定义函数的信息。

| #   |  **列名**   | **数据类型** | **说明**       |
| --- | :---------: | ------------ | -------------- |
| 1   |    name     | BINARY(64)   | 函数名         |
| 2   |   comment   | BINARY(255)  | 补充说明       |
| 3   |  aggregate  | INT          | 是否为聚合函数 |
| 4   | output_type | BINARY(31)   | 输出类型       |
| 5   | create_time | TIMESTAMP    | 创建时间       |
| 6   |  code_len   | INT          | 代码长度       |
| 7   |   bufsize   | INT          | buffer 大小    |

## INS_INDEXES

提供用户创建的索引的相关信息。也可以使用 SHOW INDEX 来查询这些信息。

| #   |     **列名**     | **数据类型** | **说明**                                                                           |
| --- | :--------------: | ------------ | ---------------------------------------------------------------------------------- |
| 1   |     db_name      | BINARY(32)   | 包含此索引的表所在的数据库名                                                       |
| 2   |    table_name    | BINARY(192)  | 包含此索引的表的名称                                                               |
| 3   |    index_name    | BINARY(192)  | 索引名                                                                             |
| 4   |   column_name    | BINARY(64)   | 建索引的列的列名                                                                   |
| 5   |    index_type    | BINARY(10)   | 目前有 SMA 和 FULLTEXT                                                             |
| 6   | index_extensions | BINARY(256)  | 索引的额外信息。对 SMA 类型的索引，是函数名的列表。对 FULLTEXT 类型的索引为 NULL。 |

## INS_STABLES

提供用户创建的超级表的相关信息。

| #   |   **列名**    | **数据类型** | **说明**                 |
| --- | :-----------: | ------------ | ------------------------ |
| 1   |  stable_name  | BINARY(192)  | 超级表表名               |
| 2   |    db_name    | BINARY(64)   | 超级表所在的数据库的名称 |
| 3   |  create_time  | TIMESTAMP    | 创建时间                 |
| 4   |    columns    | INT          | 列数目                   |
| 5   |     tags      | INT          | 标签数目                 |
| 6   |  last_update  | TIMESTAMP    | 最后更新时间             |
| 7   | table_comment | BINARY(1024) | 表注释                   |
| 8   |   watermark   | BINARY(64)   | 窗口的关闭时间           |
| 9   |   max_delay   | BINARY(64)   | 推送计算结果的最大延迟   |
| 10  |    rollup     | BINARY(128)  | rollup 聚合函数          |

## INS_TABLES

提供用户创建的普通表和子表的相关信息

| #   |   **列名**    | **数据类型** | **说明**         |
| --- | :-----------: | ------------ | ---------------- |
| 1   |  table_name   | BINARY(192)  | 表名             |
| 2   |    db_name    | BINARY(64)   | 数据库名         |
| 3   |  create_time  | TIMESTAMP    | 创建时间         |
| 4   |    columns    | INT          | 列数目           |
| 5   |  stable_name  | BINARY(192)  | 所属的超级表表名 |
| 6   |      uid      | BIGINT       | 表 id            |
| 7   |   vgroup_id   | INT          | vgroup id        |
| 8   |      ttl      | INT          | 表的生命周期     |
| 9   | table_comment | BINARY(1024) | 表注释           |
| 10  |     type      | BINARY(20)   | 表类型           |

## INS_TAGS

| #   |  **列名**   | **数据类型**  | **说明**               |
| --- | :---------: | ------------- | ---------------------- |
| 1   | table_name  | BINARY(192)   | 表名                   |
| 2   |   db_name   | BINARY(64)    | 该表所在的数据库的名称 |
| 3   | stable_name | BINARY(192)   | 所属的超级表表名       |
| 4   |  tag_name   | BINARY(64)    | tag 的名称             |
| 5   |  tag_type   | BINARY(64)    | tag 的类型             |
| 6   |  tag_value  | BINARY(16384) | tag 的值               |

## INS_USERS

提供系统中创建的用户的相关信息。

| #   |  **列名**   | **数据类型** | **说明** |
| --- | :---------: | ------------ | -------- |
| 1   |  user_name  | BINARY(23)   | 用户名   |
| 2   |  privilege  | BINARY(256)  | 权限     |
| 3   | create_time | TIMESTAMP    | 创建时间 |

## INS_GRANTS

提供企业版授权的相关信息。

| #   |  **列名**   | **数据类型** | **说明**                                           |
| --- | :---------: | ------------ | -------------------------------------------------- |
| 1   |   version   | BINARY(9)    | 企业版授权说明：official(官方授权的)/trial(试用的) |
| 2   |  cpu_cores  | BINARY(9)    | 授权使用的 CPU 核心数量                            |
| 3   |   dnodes    | BINARY(10)   | 授权使用的 dnode 节点数量                          |
| 4   |   streams   | BINARY(10)   | 授权创建的流数量                                   |
| 5   |    users    | BINARY(10)   | 授权创建的用户数量                                 |
| 6   |  accounts   | BINARY(10)   | 授权创建的帐户数量                                 |
| 7   |   storage   | BINARY(21)   | 授权使用的存储空间大小                             |
| 8   | connections | BINARY(21)   | 授权使用的客户端连接数量                           |
| 9   |  databases  | BINARY(11)   | 授权使用的数据库数量                               |
| 10  |    speed    | BINARY(9)    | 授权使用的数据点每秒写入数量                       |
| 11  |  querytime  | BINARY(9)    | 授权使用的查询总时长                               |
| 12  | timeseries  | BINARY(21)   | 授权使用的测点数量                                 |
| 13  |   expired   | BINARY(5)    | 是否到期，true：到期，false：未到期                |
| 14  | expire_time | BINARY(19)   | 试用期到期时间                                     |

## INS_VGROUPS

系统中所有 vgroups 的信息。

| #   | **列名**  | **数据类型** | **说明**                                               |
| --- | :-------: | ------------ | ------------------------------------------------------ |
| 1   | vgroup_id | INT          | vgroup id                                              |
| 2   |  db_name  | BINARY(32)   | 数据库名                                               |
| 3   |  tables   | INT          | 此 vgroup 内有多少表                                   |
| 4   |  status   | BINARY(10)   | 此 vgroup 的状态                                       |
| 5   | v1_dnode  | INT          | 第一个成员所在的 dnode 的 id                           |
| 6   | v1_status | BINARY(10)   | 第一个成员的状态                                       |
| 7   | v2_dnode  | INT          | 第二个成员所在的 dnode 的 id                           |
| 8   | v2_status | BINARY(10)   | 第二个成员的状态                                       |
| 9   | v3_dnode  | INT          | 第三个成员所在的 dnode 的 id                           |
| 10  | v3_status | BINARY(10)   | 第三个成员的状态                                       |
| 11  |  nfiles   | INT          | 此 vgroup 中数据/元数据文件的数量                      |
| 12  | file_size | INT          | 此 vgroup 中数据/元数据文件的大小                      |
| 13  |   tsma    | TINYINT      | 此 vgroup 是否专用于 Time-range-wise SMA，1: 是, 0: 否 |

## INS_CONFIGS

系统配置参数。

| #   | **列名** | **数据类型** | **说明**     |
| --- | :------: | ------------ | ------------ |
| 1   |   name   | BINARY(32)   | 配置项名称   |
| 2   |  value   | BINARY(64)   | 该配置项的值 |

## INS_DNODE_VARIABLES

系统中每个 dnode 的配置参数。

| #   | **列名** | **数据类型** | **说明**     |
| --- | :------: | ------------ | ------------ |
| 1   | dnode_id | INT          | dnode 的 ID  |
| 2   |   name   | BINARY(32)   | 配置项名称   |
| 3   |  value   | BINARY(64)   | 该配置项的值 |
