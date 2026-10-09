#!/usr/bin/env dash
#
# This file is part of OpenMediaVault.
#
# @license   https://www.gnu.org/licenses/gpl.html GPL Version 3
# @author    ${GITHUB_USER} <${GITHUB_USER}@users.noreply.github.com>
#
# OpenMediaVault is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.

set -e

. /usr/share/openmediavault/scripts/helper-functions

########################################################################
# Update the configuration.
# <config>
#   <services>
#     <nextcloud>
#       <enable>0|1</enable>
#       <version>34.0.4-apache</version>
#       <port>8080</port>
#       <timeZone>Etc/UTC</timeZone>
#       <dataRef>uuid-or-empty</dataRef>
#       <dbRef>uuid-or-empty</dbRef>
#       <dbPassword>...</dbPassword>
#       <dbUsername>nextcloud</dbUsername>
#       <dbDatabaseName>nextcloud</dbDatabaseName>
#       <adminUser>admin</adminUser>
#       <adminPassword>...</adminPassword>
#       <trustedDomains></trustedDomains>
#     </nextcloud>
#   </services>
# </config>
########################################################################
if ! omv_config_exists "/config/services/nextcloud"; then
	omv_config_add_node "/config/services" "nextcloud"
	omv_config_add_key "/config/services/nextcloud" "enable" "0"
	omv_config_add_key "/config/services/nextcloud" "version" "34.0.4-apache"
	omv_config_add_key "/config/services/nextcloud" "port" "8080"
	omv_config_add_key "/config/services/nextcloud" "timeZone" "Etc/UTC"
	omv_config_add_key "/config/services/nextcloud" "dataRef" ""
	omv_config_add_key "/config/services/nextcloud" "dbRef" ""
	omv_config_add_key "/config/services/nextcloud" "dbPassword" ""
	omv_config_add_key "/config/services/nextcloud" "dbUsername" "nextcloud"
	omv_config_add_key "/config/services/nextcloud" "dbDatabaseName" "nextcloud"
	omv_config_add_key "/config/services/nextcloud" "adminUser" "admin"
	omv_config_add_key "/config/services/nextcloud" "adminPassword" ""
	omv_config_add_key "/config/services/nextcloud" "trustedDomains" ""
fi

exit 0
