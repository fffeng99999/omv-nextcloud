# openmediavault-nextcloud

OpenMediaVault 8.x 原生插件：把 **Nextcloud** 文件同步与共享服务集成进 OMV Web 界面。

- 插件只做 **OMV 侧集成**（配置、注册栈、生命周期、状态展示），**不改动 Nextcloud 自身功能**；Nextcloud 的业务一律在其原生界面管理。
- 采用 **Compose 轨**：Docker Compose 栈由 **openmediavault-compose 插件**渲染与管理，本插件通过 Compose RPC（`Compose.setFile`）把栈注册进去，**不自己写栈文件**。
- 版本号体系：`8.x.y`（大版本跟随 OMV 大版本）。

## 依赖

- `openmediavault (= 8.*)`
- `openmediavault-compose (>= 8.2.0)`：栈的生命周期通过 Compose 插件 helper `omv-compose-run` 执行，该脚本自 Compose 8.2.0 起提供。**使用前必须先在 `服务 → Compose → 设置` 中选好 Compose 共享文件夹。**

## 工作原理

- 配置存于 `conf.service.nextcloud`（datamodel + confdb create 脚本 + RPC + 模块）。
- 保存设置时 RPC `set()` 调用 `Compose.setFile` 把栈发布到 Compose 插件：
  栈文件落在 `<Compose 共享文件夹>/nextcloud/nextcloud.yml` 与 `nextcloud.env`（含 `compose.yml`/`.env` 软链），
  因此在 `服务 → Compose → 文件 / 服务` 页也能看到、编辑、启停该栈。
- 数据/数据库共享文件夹通过 Compose 占位符 `${{ sf:"<共享文件夹名>" }}` 引用，共享文件夹挪位置由 Compose 在部署时解析。
- 「应用」时 Salt state（`srv/salt/omv/deploy/nextcloud/`）只做生命周期：
  `enable` 分支 `omv-compose-run nextcloud up -d`，关闭分支 `down`（**绝不带 -v、不删数据**）；Compose 插件未配置共享文件夹时整段跳过，绝不导致 Apply 失败。
- 部署/启停/升级/日志走 RPC 后台任务（`omv-nextcloud-ctl` 经 `omv-compose-run` 执行 `docker compose`），任务弹窗实时显示输出。

## 配置字段

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `enable` | boolean | `false` | 总开关。关闭时「应用」会移除容器（数据/数据库保留）。 |
| `version` | string | `34.0.4-apache` | 镜像 `nextcloud:<version>` 的标签。 |
| `port` | integer | `8080` | Web 界面宿主端口（容器内固定 80）。 |
| `timeZone` | string | `Etc/UTC` | 容器内时区。 |
| `dataRef` | string | `""` | 共享文件夹（映射容器 `/var/www/html`）。`enable` 时必填。 |
| `dbRef` | string | `""` | 共享文件夹（映射容器 `/var/lib/mysql`）。`enable` 时必填。 |
| `dbPassword` | string | `""` | 数据库用户密码。`enable` 时必填。 |
| `dbUsername` | string | `nextcloud` | 数据库用户名。 |
| `dbDatabaseName` | string | `nextcloud` | 数据库名。 |
| `adminUser` | string | `admin` | Nextcloud 管理员账号名。 |
| `adminPassword` | string | `""` | 管理员密码，仅用于初始化。`enable` 时必填。 |
| `trustedDomains` | string | `""` | 空格分隔的信任域名/IP；留空时用 `gethostname()`。 |

> 数据目录与管理员密码未配置完成前，插件**不注册栈**（`syncComposeStack` 守卫），因此服务在配好数据目录前无法启动。

## 渲染出的栈

栈名 `nextcloud`，三个服务（`nextcloud` / `db`（mariadb:11.4）/ `redis`（redis:7-alpine））。
环境变量由插件写入 `nextcloud.env`（`NEXTCLOUD_VERSION`、`NEXTCLOUD_PORT`、`TZ`、`DB_PASSWORD`、`DB_USERNAME`、`DB_DATABASE_NAME`、`ADMIN_USER`、`ADMIN_PASSWORD`、`TRUSTED_DOMAINS`），
由栈内的 `${...}` 读取。

## 安装 / 卸载

```bash
# 安装（需先装好 openmediavault-compose 并在 Compose 设置里选好共享文件夹）
apt-get install ./openmediavault-nextcloud_8.0.0_all.deb

# 卸载（保留数据/数据库共享文件夹）
apt-get remove openmediavault-nextcloud
# 彻底清除（删除配置节点）
apt-get purge openmediavault-nextcloud
```

- 安装时 **不启动服务**：装完后到 `服务 → Nextcloud → 设置` 选好数据/数据库共享文件夹、填写管理员密码，勾选总开关并「应用」才会拉起栈。
- 卸载时 `prerm` 调用 `omv-nextcloud-ctl unregister` 从 Compose 插件注销栈（移除容器与生成的栈文件；**数据与数据库共享文件夹不动**）。

## 栈归 omv-compose 管理

栈文件由 openmediavault-compose 插件落盘并管理，本插件只生成内容（body + env）后经 `Compose.setFile` 提交；
请勿手工编辑 `<Compose 共享文件夹>/nextcloud/nextcloud.yml`（头部标注 auto-generated），要改栈内容请走 Compose 页或本插件的设置页。

## 状态与版本

设置页显示：服务状态（运行中/部分/停止/未注册）、运行版本（读 `http://127.0.0.1:<port>/status.php` 的 `versionstring`）、
最新上游版本（GitHub `nextcloud/server` releases 页重定向）与更新提示、日志查看、渲染出的 compose 文件预览，以及「打开 Nextcloud」按钮。

## 发布流程

push tag `v*` → GitHub Actions（`.github/workflows/build.yml`）lint + 构建 `_all.deb` + 附到 Release；
`${GITHUB_USER}` 占位符由 `render-vars.sh` 按 `debian/variables.env` 渲染。
