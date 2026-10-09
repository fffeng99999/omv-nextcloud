<?php

/**
 * This file is part of OpenMediaVault.
 *
 * @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
 * @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
 *
 * OpenMediaVault is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * OpenMediaVault is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

namespace Engined\Rpc;

/**
 * Nextcloud RPC service.
 *
 * Settings (conf.service.nextcloud) live in the OMV configuration
 * database. The Nextcloud Docker Compose stack itself is owned by the
 * openmediavault-compose plugin: on save the plugin publishes (or
 * updates) the stack through the Compose RPC (Compose.setFile). The
 * stack therefore follows the Compose plugin conventions
 * (<compose shared folder>/nextcloud/nextcloud.yml + nextcloud.env,
 * plus the compose.yml/.env symlinks) and shows up in the Compose web
 * UI (Services -> Compose -> Files / Services) where it can be
 * inspected, edited, started and stopped. The plugin never renders
 * those files itself, so the Compose plugin stays the single writer.
 *
 * The stack is controlled through 'docker compose' (via the
 * /usr/sbin/omv-compose-run helper used by the Compose plugin) with
 * the /usr/sbin/omv-nextcloud-ctl helper (background tasks with live
 * output). The plugin only manages the OMV-side integration
 * (registration, lifecycle, status, directory migration); Nextcloud
 * itself is never modified.
 */
class OMVRpcServiceNextcloud extends \OMV\Rpc\ServiceAbstract
{
    const CTL = "/usr/sbin/omv-nextcloud-ctl";

    /**
     * Name of the stack in the openmediavault-compose plugin. It is
     * also the name of the stack directory below the compose shared
     * folder and the base name of the generated stack files.
     */
    const STACK_NAME = "nextcloud";

    /**
     * The shared folder references managed by the plugin. Each of them
     * can be moved to another shared folder from the settings page.
     */
    const REF_PROPERTIES = ["dataRef", "dbRef"];

    /**
     * State directory and request file used by the migration helpers.
     * The ctl helper writes one state file per property
     * (nextcloud-migration-<property>.json); the RPC writes the request
     * file holding the target folders that are currently selected in
     * the settings form (not saved yet).
     */
    const STATE_DIR = "/var/lib/openmediavault";
    const REQUEST_FILE = "/var/lib/openmediavault/nextcloud-migrate-request.json";

    /**
     * Settings fields allowed to be persisted by set(). Everything else
     * sent by the form (read-only status fields) is stripped.
     */
    private $settingsFields = [
        "enable", "version", "port", "timeZone",
        "dataRef", "dbRef",
        "dbPassword", "dbUsername", "dbDatabaseName",
        "adminUser", "adminPassword", "trustedDomains"
    ];

    public function getName()
    {
        return "Nextcloud";
    }

    public function initialize()
    {
        $this->registerMethod("get");
        $this->registerMethod("set");
        $this->registerMethod("start");
        $this->registerMethod("stop");
        $this->registerMethod("restart");
        $this->registerMethod("upgrade");
        $this->registerMethod("getLog");
        $this->registerMethod("getShowConfig");
        $this->registerMethod("testDir");
        $this->registerMethod("migrate");
        $this->registerMethod("cleanupOld");
    }

    /**
     * Run omv-nextcloud-ctl with the given arguments and return the
     * trimmed stdout. Throws on failure when $throwOnError is set.
     */
    private function runCtl(array $args, bool $throwOnError = true): array
    {
        $cmd = new \OMV\System\Process(self::CTL, $args);
        $cmd->setRedirect2to1();
        $cmd->setQuiet(true);
        $output = [];
        $exitStatus = 0;
        $cmd->execute($output, $exitStatus);
        if (0 !== $exitStatus && $throwOnError) {
            throw new \OMV\ExecException($cmd, $output, $exitStatus);
        }
        return [$exitStatus, $output];
    }

    private function getSettingsObject($context): array
    {
        return \OMV\Rpc\Rpc::call("Config", "get", [
            "id" => "conf.service.nextcloud"
        ], $context);
    }

    /**
     * Extract the running version from the /status.php JSON response
     * (the 'versionstring' field, e.g. "34.0.4.1").
     */
    private function parseVersion(array $output): string
    {
        $raw = trim(implode("\n", $output));
        $json = json_decode($raw, true);
        if (!is_array($json) || !isset($json["versionstring"])) {
            return "";
        }
        return (string)$json["versionstring"];
    }

    /**
     * Settings object enriched with read-only status information.
     */
    public function get($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);

        $object = $this->getSettingsObject($context);

        // Stack state. The status text is returned as the English msgid;
        // the Workbench form page runs it through the 'translate' filter
        // (request.get.transform merges the transformed keys into the
        // response, so all other fields are preserved).
        list(, $statusOut) = $this->runCtl(["status"], false);
        $state = trim(implode("\n", $statusOut));
        if (!in_array($state, ["running", "partial", "stopped", "notinstalled"])) {
            $state = "stopped";
        }
        $object["running"] = ("running" === $state);
        $object["statusText"] = [
            "running" => "Running",
            "partial" => "Partial",
            "stopped" => "Stopped",
            "notinstalled" => "Not installed"
        ][$state];

        list(, $versionOut) = $this->runCtl(["version"], false);
        $runningVersion = $this->parseVersion($versionOut);
        $object["runningVersion"] = $runningVersion;

        list(, $latestOut) = $this->runCtl(["latest"], false);
        $latestVersion = trim(implode("\n", $latestOut));
        $object["latestVersion"] = $latestVersion;

        // Update is available when the running version is older than
        // the latest upstream release tag (leading 'v' ignored). When
        // either side is unknown the check reports false.
        $updateAvailable = false;
        if (("" !== $runningVersion) && ("" !== $latestVersion)) {
            $updateAvailable = version_compare(
                $runningVersion, ltrim($latestVersion, "v"), "<"
            );
        }
        $object["updateAvailable"] = $updateAvailable;

        // Resolve the stack directory through the ctl helper (it lives
        // below the Compose plugin shared folder). Empty while the
        // Compose plugin is not configured.
        list(, $pathOut) = $this->runCtl(["path"], false);
        $dir = trim(implode("\n", $pathOut));
        $object["composeFile"] = ("" !== $dir)
            ? $dir."/".self::STACK_NAME.".yml" : "";

        return $object;
    }

    public function set($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $data = array_intersect_key($params, array_flip($this->settingsFields));

        // A shared folder reference may only be pointed to another
        // folder after the data has been copied with the 'Migrate data'
        // button. That record is what makes the removal of the previous
        // data (scheduled below and executed right after the settings
        // have been saved) safe. Properties that did not change are
        // left completely untouched.
        $current = $this->getSettingsObject($context);
        foreach (self::REF_PROPERTIES as $prop) {
            $oldRef = (string)($current[$prop] ?? "");
            $newRef = (string)($data[$prop] ?? "");
            $state = $this->readMigrationState($prop);
            if ($newRef === $oldRef) {
                // Clear a stale cleanup marker of an earlier migration.
                if (!empty($state)) {
                    $state["pendingCleanup"] = "";
                    $this->writeMigrationState($prop, $state);
                }
                continue;
            }
            $migrated = (true === ($state["completed"] ?? false))
                && (($state["fromRef"] ?? "") === $oldRef)
                && (($state["toRef"] ?? "") === $newRef);
            if (!$migrated && $this->folderHasData($prop, $oldRef)) {
                throw new \OMV\Exception(sprintf(
                    "The %s folder was changed without migrating its ".
                    "data first. Use 'Test directories' to validate the ".
                    "new folder and 'Migrate data' to copy it, then save ".
                    "again.", $this->refLabel($prop)
                ));
            }
            $state["pendingCleanup"] = $oldRef;
            $this->writeMigrationState($prop, $state);
        }

        $result = \OMV\Rpc\Rpc::call("Config", "set", [
            "id" => "conf.service.nextcloud",
            "data" => $data
        ], $context);

        // Publish (create or update) the Nextcloud stack in the Compose
        // plugin so it follows the openmediavault-compose conventions
        // and shows up in the Compose web UI. The Compose plugin renders
        // the stack files; this plugin does not write them itself.
        $this->syncComposeStack($context, $this->getSettingsObject($context));

        return $result;
    }

    /**
     * Create or update the Nextcloud stack in the openmediavault-compose
     * plugin. The stack body references the data and database shared
     * folders through the Compose plugin's '${{ sf:"<name>" }}'
     * placeholder, so a changed shared folder path is resolved by the
     * Compose plugin on deploy without this plugin having to re-render
     * anything.
     */
    private function syncComposeStack($context, array $config): void
    {
        $dataRef = (string)($config["dataRef"] ?? "");
        $dbRef = (string)($config["dbRef"] ?? "");
        $adminPassword = (string)($config["adminPassword"] ?? "");
        if (("" === $dataRef) || ("" === $dbRef) || ("" === $adminPassword)) {
            // Storage or the administrator credentials are not
            // configured yet - nothing usable to publish.
            return;
        }
        $dataName = $this->sharedFolderName($context, $dataRef);
        $dbName = $this->sharedFolderName($context, $dbRef);
        if (("" === $dataName) || ("" === $dbName)) {
            throw new \OMV\Exception(
                "The selected shared folders could not be resolved."
            );
        }

        $params = [
            "name" => self::STACK_NAME,
            "description" => "Nextcloud (managed by the ".
                "openmediavault-nextcloud plugin)",
            "body" => $this->buildComposeBody($dataName, $dbName),
            "showenv" => true,
            "env" => $this->buildComposeEnv($config),
            "showoverride" => false,
            "override" => ""
        ];
        $uuid = $this->findStackUuid($context);
        if ("" !== $uuid) {
            $params["uuid"] = $uuid;
        }

        try {
            \OMV\Rpc\Rpc::call("Compose", "setFile", $params, $context);
        } catch (\Exception $e) {
            throw new \OMV\Exception(sprintf(
                "The Nextcloud stack could not be registered in the Compose ".
                "plugin: %s. Install openmediavault-compose and set the ".
                "shared folder for the compose files (Services -> Compose ".
                "-> Settings) first.",
                $e->getMessage()
            ));
        }
    }

    /**
     * UUID of the Nextcloud stack in the Compose plugin (empty when it
     * does not exist yet).
     */
    private function findStackUuid($context): string
    {
        $files = \OMV\Rpc\Rpc::call("Config", "get", [
            "id" => "conf.service.compose.file"
        ], $context);
        if (!is_array($files)) {
            return "";
        }
        foreach ($files as $file) {
            if (($file["name"] ?? "") === self::STACK_NAME) {
                return (string)($file["uuid"] ?? "");
            }
        }
        return "";
    }

    /**
     * Name of a shared folder by reference (empty when unknown).
     */
    private function sharedFolderName($context, string $ref): string
    {
        if ("" === $ref) {
            return "";
        }
        $folders = \OMV\Rpc\Rpc::call("Config", "get", [
            "id" => "conf.system.sharedfolder"
        ], $context);
        if (!is_array($folders)) {
            return "";
        }
        foreach ($folders as $folder) {
            if (($folder["uuid"] ?? "") === $ref) {
                return (string)($folder["name"] ?? "");
            }
        }
        return "";
    }

    /**
     * Docker Compose file body for the Nextcloud stack (no auto-generated
     * header; the Compose plugin prepends its own). The data and database
     * shared folders are injected through the %%DATA%%/%%DB%% markers so
     * the nowdoc can stay verbatim.
     */
    private function buildComposeBody(string $dataName, string $dbName): string
    {
        $body = <<<'YAML'
name: nextcloud

services:
  nextcloud:
    container_name: nextcloud_app
    image: nextcloud:${NEXTCLOUD_VERSION:-34.0.4-apache}
    volumes:
      - ${{ sf:"%%DATA%%" }}:/var/www/html
    environment:
      - MYSQL_HOST=db
      - MYSQL_DATABASE=${DB_DATABASE_NAME}
      - MYSQL_USER=${DB_USERNAME}
      - MYSQL_PASSWORD=${DB_PASSWORD}
      - REDIS_HOST=redis
      - NEXTCLOUD_ADMIN_USER=${ADMIN_USER}
      - NEXTCLOUD_ADMIN_PASSWORD=${ADMIN_PASSWORD}
      - NEXTCLOUD_TRUSTED_DOMAINS=${TRUSTED_DOMAINS}
      - TZ=${TZ}
    ports:
      - '${NEXTCLOUD_PORT:-8080}:80'
    depends_on:
      db:
        condition: service_healthy
      redis:
        condition: service_healthy
    restart: always

  db:
    container_name: nextcloud_db
    image: mariadb:11.4
    command: --transaction-isolation=READ-COMMITTED --binlog-format=ROW
    volumes:
      - ${{ sf:"%%DB%%" }}:/var/lib/mysql
    environment:
      - MYSQL_ROOT_PASSWORD=${DB_PASSWORD}
      - MYSQL_DATABASE=${DB_DATABASE_NAME}
      - MYSQL_USER=${DB_USERNAME}
      - MYSQL_PASSWORD=${DB_PASSWORD}
    restart: always
    healthcheck:
      test: ["CMD", "healthcheck.sh", "--connect", "--innodb_initialized"]
      interval: 10s
      timeout: 5s
      retries: 10

  redis:
    container_name: nextcloud_redis
    image: redis:7-alpine
    restart: always
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 10s
      timeout: 5s
      retries: 10
YAML;
        $body = strtr($body, [
            "%%DATA%%" => $dataName,
            "%%DB%%" => $dbName
        ]);

        return $body."\n";
    }

    /**
     * Environment file content for the Nextcloud stack.
     */
    private function buildComposeEnv(array $config): string
    {
        $version = trim((string)($config["version"] ?? ""));
        if ("" === $version) {
            $version = "34.0.4-apache";
        }
        $trustedDomains = trim((string)($config["trustedDomains"] ?? ""));
        if ("" === $trustedDomains) {
            $trustedDomains = gethostname();
        }
        $lines = [
            'NEXTCLOUD_VERSION='.$version,
            'NEXTCLOUD_PORT='.(string)($config["port"] ?? "8080"),
            'TZ='.(string)($config["timeZone"] ?? "Etc/UTC"),
            'DB_PASSWORD='.(string)($config["dbPassword"] ?? ""),
            'DB_USERNAME='.(string)($config["dbUsername"] ?? "nextcloud"),
            'DB_DATABASE_NAME='.(string)($config["dbDatabaseName"] ?? "nextcloud"),
            'ADMIN_USER='.(string)($config["adminUser"] ?? "admin"),
            'ADMIN_PASSWORD='.(string)($config["adminPassword"] ?? ""),
            'TRUSTED_DOMAINS='.$trustedDomains
        ];
        return implode("\n", $lines)."\n";
    }

    /**
     * Run a stack operation as a background task (the WebUI shows a
     * task dialog with the live 'docker compose' output). This keeps
     * the HTTP request short even when images have to be pulled first.
     */
    private function execCtlTask($context, string $op): string
    {
        return $this->execBgCtl([$op]);
    }

    public function start($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "up");
    }

    public function stop($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "down");
    }

    public function restart($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "restart");
    }

    public function upgrade($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "upgrade");
    }

    public function getLog($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "logs");
    }

    public function getShowConfig($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        return $this->execCtlTask($context, "showcompose");
    }

    /**
     * Validate the shared folders that are currently selected in the
     * settings form (not saved yet). Only the properties whose value
     * differs from the saved configuration are checked.
     */
    public function testDir($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->writeRequest($this->changedRefs($params, $context));
        return $this->execBgCtl(["testdirall"]);
    }

    /**
     * Copy every changed shared folder to the location selected in the
     * settings form (not saved yet). The stack is stopped, the data is
     * copied and verified, and the completed migration is recorded so
     * that the settings can be saved afterwards. The stack is not
     * started again until the settings are saved and applied, because
     * the stack file mounts the folders through the Compose plugin
     * placeholder and cannot be pointed at the new locations ad hoc.
     */
    public function migrate($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        $this->writeRequest($this->changedRefs($params, $context));
        return $this->execBgCtl(["migrateall"]);
    }

    /**
     * Remove the data of the previously used shared folders. Scheduled
     * by set() and executed as a background task by the Save button, so
     * the user sees the progress of the removal.
     */
    public function cleanupOld($params, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_ADMINISTRATOR]);
        // The ctl helper clears the pending cleanup marker of a property
        // itself, but only after its data has really been removed, so a
        // failed removal can be repeated by saving again.
        return $this->execBgCtl(["cleanupall"]);
    }

    /**
     * The shared folder references selected in the form that differ from
     * the saved configuration, keyed by property name.
     */
    private function changedRefs(array $params, $context): array
    {
        $current = $this->getSettingsObject($context);
        $changed = [];
        foreach (self::REF_PROPERTIES as $prop) {
            $newRef = (string)($params[$prop] ?? "");
            $oldRef = (string)($current[$prop] ?? "");
            if (("" !== $newRef) && ($newRef !== $oldRef)) {
                $changed[$prop] = $newRef;
            }
        }
        return $changed;
    }

    private function writeRequest(array $refs): void
    {
        @file_put_contents(
            self::REQUEST_FILE,
            json_encode($refs, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE)
        );
    }

    private function refLabel(string $prop): string
    {
        $labels = [
            "dataRef" => "data",
            "dbRef" => "database"
        ];
        return $labels[$prop] ?? $prop;
    }

    /**
     * Read the migration state file of a property written by the ctl
     * helper.
     */
    private function readMigrationState(string $prop): array
    {
        $file = self::STATE_DIR."/nextcloud-migration-".$prop.".json";
        if (!is_readable($file)) {
            return [];
        }
        $state = json_decode((string)file_get_contents($file), true);
        return is_array($state) ? $state : [];
    }

    private function writeMigrationState(string $prop, array $state): void
    {
        @file_put_contents(
            self::STATE_DIR."/nextcloud-migration-".$prop.".json",
            json_encode($state, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE)
        );
    }

    /**
     * True when the given shared folder already holds data that belongs
     * to the property. An empty (or not yet initialised) folder may be
     * switched to without migrating, because there is nothing to
     * preserve.
     */
    private function folderHasData(string $prop, string $ref): bool
    {
        if ("" === $ref) {
            return false;
        }
        $cmd = new \OMV\System\Process(self::CTL, ["hasdata", $prop, $ref]);
        $cmd->setRedirect2to1();
        $cmd->setQuiet(true);
        $output = [];
        $exitStatus = 0;
        $cmd->execute($output, $exitStatus);
        return (0 === $exitStatus);
    }

    /**
     * Run the ctl helper as a background task and return the task status
     * file name so the task dialog can poll the live output.
     */
    private function execBgCtl(array $args): string
    {
        return $this->execBgProc(
            function ($bgStatusFilename, $bgOutputFilename) use ($args) {
                $cmd = new \OMV\System\Process(self::CTL, $args);
                $cmd->setRedirect2to1();
                if (0 !== ($exitStatus = $this->exec(
                    $cmd, $output, $bgOutputFilename
                ))) {
                    throw new \OMV\ExecException($cmd, $output, $exitStatus);
                }
                return $output;
            }
        );
    }
}
