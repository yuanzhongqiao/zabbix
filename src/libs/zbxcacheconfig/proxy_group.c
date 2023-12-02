/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "proxy_group.h"
#include "dbconfig.h"
#include "dbsync.h"

ZBX_PTR_VECTOR_IMPL(pg_proxy_ptr, zbx_pg_proxy_t *)
ZBX_PTR_VECTOR_IMPL(pg_group_ptr, zbx_pg_group_t *)
ZBX_PTR_VECTOR_IMPL(pg_host_ptr, zbx_pg_host_t *)
ZBX_VECTOR_IMPL(pg_host, zbx_pg_host_t)

/******************************************************************************
 *                                                                            *
 * Purpose: sync proxy groups with configuration cache                        *
 *                                                                            *
 * Parameters: sync     - [IN] db synchronization data                        *
 *             revision - [IN] current sync revision                          *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - proxy_groupid                                                *
 *           1 - failover_delay                                               *
 *           2 - min_online                                                   *
 *                                                                            *
 ******************************************************************************/
void	dc_sync_proxy_group(zbx_dbsync_t *sync, zbx_uint64_t revision)
{
	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_dc_proxy_group_t	*pg;
	int			ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		zbx_uint64_t	proxy_groupid;
		int		found;

		ZBX_STR2UINT64(proxy_groupid, row[0]);

		pg = (zbx_dc_proxy_group_t *)DCfind_id(&config->proxy_groups, proxy_groupid, sizeof(ZBX_DC_PROXY),
				&found);

		if (0 == found)
			pg->host_mapping_revision = 0;

		if (FAIL == zbx_is_time_suffix(row[1], &pg->failover_delay, ZBX_LENGTH_UNLIMITED))
		{
			zabbix_log(LOG_LEVEL_WARNING, "invalid proxy group '" ZBX_FS_UI64 "' failover delay '%s', "
					"using 60 seconds default value", pg->proxy_groupid, row[1]);
			pg->failover_delay = SEC_PER_MIN;
		}

		pg->min_online = atoi(row[2]);

		pg->revision = revision;
	}

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (pg = (zbx_dc_proxy_group_t *)zbx_hashset_search(&config->proxy_groups, &rowid)))
			continue;

		zbx_hashset_remove_direct(&config->proxy_groups, pg);
	}

	if (0 != sync->add_num + sync->update_num + sync->remove_num)
		config->revision.proxy_group = revision;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Purpose: update local proxy group cache                                    *
 *                                                                            *
 * Parameter: groups   - [IN/OUT] local proxy group cache                     *
 *            revision - [IN/OUT] local proxy group cache revision            *
 *                                                                            *
 * Return value: SUCCEED - local cache was updated                            *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_dc_get_proxy_groups(zbx_hashset_t *groups, zbx_uint64_t *revision)
{
	int		ret = FAIL;
	zbx_uint64_t	old_revision = *revision;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (*revision >= config->revision.proxy_group)
		goto out;

	zbx_hashset_iter_t	iter;
	zbx_dc_proxy_group_t	*dc_group;
	zbx_pg_group_t		*group;

	zbx_hashset_iter_reset(groups, &iter);
	while (NULL != (group = (zbx_pg_group_t *)zbx_hashset_iter_next(&iter)))
		group->flags = ZBX_PG_GROUP_FLAGS_NONE;

	RDLOCK_CACHE;

	*revision = config->revision.proxy_group;

	zbx_hashset_iter_reset(&config->proxy_groups, &iter);
	while (NULL != (dc_group = (zbx_dc_proxy_group_t *)zbx_hashset_iter_next(&iter)))
	{
		if (NULL == (group = (zbx_pg_group_t *)zbx_hashset_search(groups, &dc_group->proxy_groupid)))
		{
			zbx_pg_group_t	group_local = {.proxy_groupid = dc_group->proxy_groupid};

			group = (zbx_pg_group_t *)zbx_hashset_insert(groups, &group_local, sizeof(group_local));
			zbx_vector_pg_proxy_ptr_create(&group->proxies);
			zbx_vector_uint64_create(&group->hostids);
			zbx_vector_uint64_create(&group->new_hostids);
			group->flags = ZBX_PG_GROUP_SYNC_ADDED;
		}
		else
			group->flags = ZBX_PG_GROUP_SYNC_MODIFIED;

		group->sync_revision = *revision;

		if (dc_group->revision > group->revision)
		{
			group->revision = dc_group->revision;
			group->failover_delay = dc_group->failover_delay;
			group->min_online = dc_group->min_online;
		}
	}

	UNLOCK_CACHE;

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s revision:" ZBX_FS_UI64 "->" ZBX_FS_UI64, __func__,
			zbx_result_string(ret), old_revision, *revision);
	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: get locally cached proxy lastaccess from configuration cache      *
 *                                                                            *
 * Parameter: proxies - [IN/OUT] local proxy cache                            *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_get_group_proxy_lastaccess(zbx_hashset_t *proxies)
{
	zbx_hashset_iter_t	iter;
	zbx_pg_proxy_t		*proxy;

	zbx_hashset_iter_reset(proxies, &iter);

	RDLOCK_CACHE;

	while (NULL != (proxy = (zbx_pg_proxy_t *)zbx_hashset_iter_next(&iter)))
	{
		ZBX_DC_PROXY	*dc_proxy;

		if (NULL == (dc_proxy = (ZBX_DC_PROXY *)zbx_hashset_search(&config->proxies, &proxy->proxyid)))
			proxy->lastaccess = 0;
		else
			proxy->lastaccess = dc_proxy->lastaccess;
	}

	UNLOCK_CACHE;
}

/******************************************************************************
 *                                                                            *
 * Purpose: update host proxy map revision in configuration cache for the     *
 *          specified groups.                                                 *
 *                                                                            *
 * Parameter: groupids - [IN] target group identifiers                        *
 *            revision - [IN] host proxy map revision                         *
 *                                                                            *
 ******************************************************************************/
void	zbx_dc_update_group_hpmap_revision(zbx_vector_uint64_t *groupids, zbx_uint64_t revision)
{
	WRLOCK_CACHE;

	for (int i = 0; i < groupids->values_num; i++)
	{
		zbx_pg_group_t	*group;

		if (NULL != (group = (zbx_pg_group_t *)zbx_hashset_search(&config->proxy_groups, &groupids->values[i])))
			group->mapping_revision = revision;
	}

	UNLOCK_CACHE;
}

static void	dc_register_host_proxy(zbx_dc_host_proxy_t *hp)
{
	zbx_dc_host_proxy_index_t	*hpi, hpi_local = {.host = hp->host};

	if (NULL == (hpi = (zbx_dc_host_proxy_index_t *)zbx_hashset_search(&config->host_proxy_index, &hpi_local)))
	{
		hpi = (zbx_dc_host_proxy_index_t *)zbx_hashset_insert(&config->host_proxy_index, &hpi_local,
				sizeof(hpi_local));
		dc_strpool_acquire(hpi->host);
	}

	hpi->host_proxy = hp;
}

static void	dc_deregister_host_proxy(zbx_dc_host_proxy_t *hp)
{
	zbx_dc_host_proxy_index_t	*hpi, hpi_local = {.host = hp->host};

	if (NULL != (hpi = (zbx_dc_host_proxy_index_t *)zbx_hashset_search(&config->host_proxy_index, &hpi_local)))
	{
		dc_strpool_release(hpi->host);
		zbx_hashset_remove_direct(&config->host_proxy_index, hpi);
	}
}

void	dc_update_host_proxy(const char *host_old, const char *host_new)
{
	zbx_dc_host_proxy_index_t	*hpi, hpi_local = {.host = host_old};

	if (NULL != (hpi = (zbx_dc_host_proxy_index_t *)zbx_hashset_search(&config->host_proxy_index, &hpi_local)))
	{
		dc_strpool_release(hpi->host);
		zbx_hashset_remove_direct(&config->host_proxy_index, hpi);

		hpi_local.host_proxy = hpi->host_proxy;
		hpi_local.host = dc_strpool_intern(host_new);
		zbx_hashset_insert(&config->host_proxy_index, &hpi_local, sizeof(hpi_local));
	}
}

/******************************************************************************
 *                                                                            *
 * Purpose: sync host proxy links with configuration cache                    *
 *                                                                            *
 * Parameters: sync     - [IN] db synchronization data                        *
 *                                                                            *
 * Comments: The result contains the following fields:                        *
 *           0 - hostproxyid                                                  *
 *           1 - hostid                                                       *
 *           2 - host                                                         *
 *           3 - proxyid                                                      *
 *           4 - revision                                                     *
 *           5 - host.host (NULL on proxies)                                  *
 *                                                                            *
 ******************************************************************************/
void	dc_sync_host_proxy(zbx_dbsync_t *sync)
{
	char			**row;
	zbx_uint64_t		rowid;
	unsigned char		tag;
	zbx_dc_host_proxy_t	*hp;
	int			ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	while (SUCCEED == (ret = zbx_dbsync_next(sync, &rowid, &row, &tag)))
	{
		/* removed rows will be always added at the end */
		if (ZBX_DBSYNC_ROW_REMOVE == tag)
			break;

		zbx_uint64_t	hostproxyid;
		int		found;

		ZBX_STR2UINT64(hostproxyid, row[0]);

		hp = (zbx_dc_host_proxy_t *)DCfind_id(&config->host_proxy, hostproxyid, sizeof(ZBX_DC_PROXY),
				&found);

		ZBX_DBROW2UINT64(hp->hostid, row[1]);
		ZBX_STR2UINT64(hp->proxyid, row[3]);
		ZBX_STR2UINT64(hp->revision, row[4]);

		if (SUCCEED != zbx_db_is_null(row[5]))
			dc_strpool_replace(found, &hp->host, row[5]);
		else
			dc_strpool_replace(found, &hp->host, row[2]);

		dc_register_host_proxy(hp);
	}

	for (; SUCCEED == ret; ret = zbx_dbsync_next(sync, &rowid, &row, &tag))
	{
		if (NULL == (hp = (zbx_dc_host_proxy_t *)zbx_hashset_search(&config->proxy_groups, &rowid)))
			continue;

		dc_deregister_host_proxy(hp);
		zbx_hashset_remove_direct(&config->host_proxy, hp);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}
