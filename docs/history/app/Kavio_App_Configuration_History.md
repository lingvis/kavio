# Kavio App Configuration 变更记录

此文件仅记录配置协议的演进决策，不存放过期版本的完整规范。
正式且唯一的现行说明：[`Kavio_App_Configuration_Spec.md`](../../modules/app/Kavio_App_Configuration_Spec.md)。

## 演进要点

1. 早期 App 配置曾包含独立的定义文件与默认值文件；现已取消独立的 `config/default.json`。
2. Schema Description 统一放入 App 包根目录的 `schema_description.json`，与 `kavio_package.json` 同级；不存在配置项时写入 `{}`。
3. 配置项初始值写在 Schema `fields[].value`，App Device SDK 从字段初始值生成本地配置，支持读取和更新。
4. 已取消在 App Manifest 中携带 `SchemaConfigPb` / `schemaConfig` 的方式；打包时把 Schema 原样加入包文件和完整性列表。
5. 新协议和实现调整直接修改现行文档，不再按版本号新建规范副本。协议字段格式版本仍以文档正文和代码定义为准。
