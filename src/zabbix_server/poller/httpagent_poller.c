#include "log.h"
#include "zbxalgo.h"
#include "zbxcommon.h"
#include "zbxhttp.h"
#include "zbxcacheconfig.h"
#include <event.h>
#include <event2/thread.h>
#include "poller.h"
#include "zbxserver.h"
#include "zbx_item_constants.h"
#include "zbxpreproc.h"
#include "zbxself.h"
#include "zbxnix.h"
#include "zbx_rtc_constants.h"
#include "zbxrtc.h"
#include "zbxtime.h"
#include "zbxtypes.h"

static ZBX_THREAD_LOCAL struct event_base	*base;
static ZBX_THREAD_LOCAL struct event		*curl_timeout;
static ZBX_THREAD_LOCAL CURLM			*curl_handle;

ZBX_VECTOR_DECL(int32, int)
ZBX_VECTOR_IMPL(int32, int)
typedef struct
{
	unsigned char		poller_type;
	int			processed;
	int			queued;
	int			processing;
	int			config_timeout;
	const char		*config_source_ip;
	struct event		*async_items_timer;
	zbx_vector_uint64_t	itemids;
	zbx_vector_int32_t	errcodes;
	zbx_vector_int32_t	lastclocks;
}
zbx_poller_config_t;

typedef struct
{
	zbx_uint64_t	itemid;
	zbx_uint64_t	hostid;
	unsigned char	value_type;
	unsigned char	flags;
	unsigned char	state;
	char		*posts;
	char		*status_codes;
}
zbx_dc_item_context_t;
typedef struct
{
	zbx_poller_config_t	*poller_config;
	zbx_http_context_t	http_context;
	zbx_dc_item_context_t	item_context;
}
zbx_httpagent_context;

typedef struct
{
	struct event *event;
	curl_socket_t sockfd;
}
zbx_curl_context_t;

static void	httpagent_context_create(zbx_httpagent_context *httpagent_context)
{
	zbx_http_context_create(&httpagent_context->http_context);
}

static void	httpagent_context_clean(zbx_httpagent_context *httpagent_context)
{
	zbx_free(httpagent_context->item_context.status_codes);
	zbx_free(httpagent_context->item_context.posts);
	zbx_http_context_destroy(&httpagent_context->http_context);
}

static int	async_httpagent_add(zbx_dc_item_t *item, AGENT_RESULT *result, zbx_poller_config_t *poller_config)
{
	char			*error = NULL;
	int			ret;
	zbx_httpagent_context	*httpagent_context = zbx_malloc(NULL, sizeof(zbx_httpagent_context));
	CURLcode		err;
	CURLMcode		merr;

	httpagent_context_create(httpagent_context);

	httpagent_context->poller_config = poller_config;
	httpagent_context->item_context.itemid = item->itemid;
	httpagent_context->item_context.hostid = item->host.hostid;
	httpagent_context->item_context.value_type = item->value_type;
	httpagent_context->item_context.flags = item->flags;
	httpagent_context->item_context.state = item->state;
	httpagent_context->item_context.posts = item->posts;
	item->posts = NULL;
	httpagent_context->item_context.status_codes = item->status_codes;
	item->status_codes = NULL;

	if (SUCCEED != (ret = zbx_http_request_prepare(&httpagent_context->http_context, item->request_method,
			item->url, item->query_fields, item->headers, httpagent_context->item_context.posts,
			item->retrieve_mode, item->http_proxy, item->follow_redirects, item->timeout, 1,
			item->ssl_cert_file, item->ssl_key_file, item->ssl_key_password, item->verify_peer,
			item->verify_host, item->authtype, item->username, item->password, NULL, item->post_type,
			item->output_format, poller_config->config_source_ip, &error)))
	{
		SET_MSG_RESULT(result, error);

		goto fail;
	}

	if (CURLE_OK != (err = curl_easy_setopt(httpagent_context->http_context.easyhandle, CURLOPT_PRIVATE,
			httpagent_context)))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Cannot set pointer to private data: %s",
				curl_easy_strerror(err)));

		goto fail;
	}

	if (CURLM_OK != (merr = curl_multi_add_handle(curl_handle, httpagent_context->http_context.easyhandle)))
	{
		SET_MSG_RESULT(result, zbx_dsprintf(NULL, "Cannot add a standard curl handle to the multi stack: %s",
				curl_multi_strerror(merr)));

		goto fail;
	}

	poller_config->processing++;
	return SUCCEED;
fail:
	httpagent_context_clean(httpagent_context);
	zbx_free(httpagent_context);

	return NOTSUPPORTED;
}

static void	async_httpagent_done(CURL *easy_handle, CURLcode err)
{
	long			response_code;
	char			*error, *out = NULL;
	AGENT_RESULT		result;
	char			*status_codes;
	zbx_httpagent_context	*httpagent_context;
	zbx_dc_item_context_t	*item_context;
	zbx_timespec_t		timespec;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);
	
	curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &httpagent_context);

	zbx_timespec(&timespec);

	zbx_init_agent_result(&result);
	status_codes = httpagent_context->item_context.status_codes;
	item_context = &httpagent_context->item_context;

	if (SUCCEED == zbx_http_handle_response(easy_handle, &httpagent_context->http_context, err, &response_code,
			&out, &error) && SUCCEED == zbx_handle_response_code(status_codes, response_code, out, &error))
	{

		SET_TEXT_RESULT(&result, out);
		out = NULL;
		zbx_preprocess_item_value(item_context->itemid, item_context->hostid,item_context->value_type,
				item_context->flags, &result, &timespec, ITEM_STATE_NORMAL, NULL);
	}
	else
	{
		SET_MSG_RESULT(&result, error);
		zbx_preprocess_item_value(item_context->itemid, item_context->hostid, item_context->value_type,
				item_context->flags, NULL, &timespec, ITEM_STATE_NOTSUPPORTED, result.msg);
	}

	zbx_free_agent_result(&result);
	zbx_free(out);

	zbx_vector_uint64_append(&httpagent_context->poller_config->itemids, httpagent_context->item_context.itemid);
	zbx_vector_int32_append(&httpagent_context->poller_config->errcodes, SUCCEED);
	zbx_vector_int32_append(&httpagent_context->poller_config->lastclocks, timespec.sec);

	httpagent_context->poller_config->processing--;
	httpagent_context->poller_config->processed++;

	zabbix_log(LOG_LEVEL_DEBUG, "finished processing itemid:" ZBX_FS_UI64, httpagent_context->item_context.itemid);

	curl_multi_remove_handle(curl_handle, easy_handle);
	httpagent_context_clean(httpagent_context);
	zbx_free(httpagent_context);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

static void	poller_requeue_items(zbx_poller_config_t *poller_config)
{
	int	nextcheck;

	if (0 == poller_config->itemids.values_num)
		return;
	
	zbx_dc_poller_requeue_items(poller_config->itemids.values, poller_config->lastclocks.values,
			poller_config->errcodes.values, poller_config->itemids.values_num,
			ZBX_POLLER_TYPE_HTTPAGENT, &nextcheck);

	zabbix_log(LOG_LEVEL_DEBUG, "%s() requeued:%d", __func__, poller_config->itemids.values_num);

	zbx_vector_uint64_clear(&poller_config->itemids);
	zbx_vector_int32_clear(&poller_config->lastclocks);
	zbx_vector_int32_clear(&poller_config->errcodes);

	if (FAIL != nextcheck && nextcheck <= time(NULL))
		event_active(poller_config->async_items_timer, 0, 0);
}

static void	check_multi_info(void)
{
	CURLMsg	*message;
	int	pending;

	while (NULL != (message = curl_multi_info_read(curl_handle, &pending)))
	{
		zabbix_log(LOG_LEVEL_DEBUG, "pending cURL messages:%d", pending);

		switch (message->msg)
		{
			case CURLMSG_DONE:
				async_httpagent_done(message->easy_handle, message->data.result);
				break;
			default:
				zabbix_log(LOG_LEVEL_DEBUG, "curl message:%d", message->msg);
				break;
		}
	}
}

static void	on_timeout(evutil_socket_t fd, short events, void *arg)
{
	int	running_handles;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running_handles);
	check_multi_info();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

static int	start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
	zabbix_log(LOG_LEVEL_DEBUG, "%s() timeout:%ld", __func__, timeout_ms);

	if(0 > timeout_ms)
	{
		evtimer_del(curl_timeout);
	}
	else
	{
		struct timeval tv;

		if(0 == timeout_ms)
			timeout_ms = 1;	/* 0 means directly call socket_action, but we will do it in a bit */

		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		evtimer_del(curl_timeout);
		evtimer_add(curl_timeout, &tv);
	}

	return 0;
}

static void	curl_perform(int fd, short event, void *arg)
{
	int			running_handles;
	int			flags = 0;
	zbx_curl_context_t	*context;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if(event & EV_READ)
		flags |= CURL_CSELECT_IN;
	if(event & EV_WRITE)
		flags |= CURL_CSELECT_OUT;

	context = (zbx_curl_context_t *)arg;
	curl_multi_socket_action(curl_handle, context->sockfd, flags, &running_handles);

	zabbix_log(LOG_LEVEL_DEBUG, "running_handles:%d", running_handles);

	check_multi_info();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

static zbx_curl_context_t	*create_curl_context(curl_socket_t sockfd)
{
	zbx_curl_context_t	*context;

	context = (zbx_curl_context_t *) malloc(sizeof(*context));
	context->sockfd = sockfd;
	context->event = event_new(base, sockfd, 0, curl_perform, context);

	return context;
}

static void	destroy_curl_context(zbx_curl_context_t *context)
{
	event_del(context->event);
	event_free(context->event);
	free(context);
}

static int	handle_socket(CURL *easy, curl_socket_t s, int action, void *userp, void *socketp)
{
	zbx_curl_context_t	*curl_context;
	int			events = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() action:%d", __func__, action);

	switch (action)
	{
		case CURL_POLL_IN:
		case CURL_POLL_OUT:
		case CURL_POLL_INOUT:
			curl_context = socketp ? (zbx_curl_context_t *)socketp : create_curl_context(s);

			curl_multi_assign(curl_handle, s, (void *) curl_context);

			if(action != CURL_POLL_IN)
				events |= EV_WRITE;
			if(action != CURL_POLL_OUT)
				events |= EV_READ;

			events |= EV_PERSIST;

			event_del(curl_context->event);
			event_assign(curl_context->event, base, curl_context->sockfd, events, curl_perform,
					curl_context);
			event_add(curl_context->event, NULL);

		break;
		case CURL_POLL_REMOVE:
			if (NULL != socketp)
			{
				event_del(((zbx_curl_context_t*) socketp)->event);
				destroy_curl_context((zbx_curl_context_t*) socketp);
				curl_multi_assign(curl_handle, s, NULL);
			}
			break;
		default:
			break;

	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);

	return 0;
}

static void	async_items(evutil_socket_t fd, short events, void *arg)
{
	zbx_dc_item_t		item, *items;
	AGENT_RESULT		results[ZBX_MAX_HTTPAGENT_ITEMS];
	int			errcodes[ZBX_MAX_HTTPAGENT_ITEMS];
	zbx_timespec_t		timespec;
	int			i, num;
	zbx_poller_config_t	*poller_config = (zbx_poller_config_t *)arg;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	items = &item;
	num = zbx_dc_config_get_poller_items(poller_config->poller_type, poller_config->config_timeout,
			poller_config->processing, &items);

	if (0 == num)
		goto exit;

	zbx_prepare_items(items, errcodes, num, results, MACRO_EXPAND_YES);

	for (i = 0; i < num; i++)
		errcodes[i] = async_httpagent_add(&items[i], &results[i], poller_config);

	zbx_timespec(&timespec);

	/* process item values */
	for (i = 0; i < num; i++)
	{
		if (NOTSUPPORTED == errcodes[i] || AGENT_ERROR == errcodes[i] || CONFIG_ERROR == errcodes[i])
		{
			zbx_preprocess_item_value(items[i].itemid, items[i].host.hostid, items[i].value_type,
					items[i].flags, NULL, &timespec, ITEM_STATE_NOTSUPPORTED, results[i].msg);

			zbx_vector_uint64_append(&poller_config->itemids, items[i].itemid);
			zbx_vector_int32_append(&poller_config->errcodes, errcodes[i]);
			zbx_vector_int32_append(&poller_config->lastclocks, timespec.sec);
		}
	}

	zbx_preprocessor_flush();
	zbx_clean_items(items, num, results);
	zbx_dc_config_clean_items(items, NULL, num);

	if (items != &item)
		zbx_free(items);
exit:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%d", __func__, num);

	poller_config->queued += num;
}


static void	http_agent_poller_init(zbx_poller_config_t *poller_config, zbx_thread_poller_args *poller_args_in)
{
	CURLMcode	merr;
	CURLcode	err;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vector_uint64_create(&poller_config->itemids);
	zbx_vector_int32_create(&poller_config->lastclocks);
	zbx_vector_int32_create(&poller_config->errcodes);

	if (CURLE_OK != (err = curl_global_init(CURL_GLOBAL_ALL)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot initialize cURL: %s", curl_easy_strerror(err));

		exit(EXIT_FAILURE);
	}

	if (NULL == (curl_handle = curl_multi_init()))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot initialize cURL multi session");
		exit(EXIT_FAILURE);
	}

	if (NULL == (base = event_base_new()))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot initialize event base");
		exit(EXIT_FAILURE);
	}

	if (NULL == (curl_timeout = evtimer_new(base, on_timeout, NULL)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot create timer event");
		exit(EXIT_FAILURE);
	}

	if (CURLM_OK != (merr = curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket)))
	{
		zabbix_log(LOG_LEVEL_ERR, "Cannot set CURLMOPT_SOCKETFUNCTION: %s", curl_multi_strerror(merr));
		exit(EXIT_FAILURE);
	}

	if (CURLM_OK != (merr = curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout)))
	{
		zabbix_log(LOG_LEVEL_ERR, "Cannot set CURLMOPT_TIMERFUNCTION: %s", curl_multi_strerror(merr));
		exit(EXIT_FAILURE);
	}

	poller_config->config_source_ip = poller_args_in->config_comms->config_source_ip;
	poller_config->config_timeout = poller_args_in->config_comms->config_timeout;
	poller_config->poller_type = poller_args_in->poller_type;

	if (NULL == (poller_config->async_items_timer = evtimer_new(base, async_items, poller_config)))
	{
		zabbix_log(LOG_LEVEL_ERR, "cannot create async items timer event");
		exit(EXIT_FAILURE);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

ZBX_THREAD_ENTRY(httpagent_poller_thread, args)
{
	zbx_thread_poller_args	*poller_args_in = (zbx_thread_poller_args *)(((zbx_thread_args_t *)args)->args);

	double			sec, total_sec = 0.0;
	time_t			last_stat_time;
	zbx_ipc_async_socket_t	rtc;
	const zbx_thread_info_t	*info = &((zbx_thread_args_t *)args)->info;
	int			server_num = ((zbx_thread_args_t *)args)->info.server_num;
	int			process_num = ((zbx_thread_args_t *)args)->info.process_num;
	unsigned char		process_type = ((zbx_thread_args_t *)args)->info.process_type;
	struct timeval		tv = {1, 0};
	zbx_poller_config_t	poller_config = {.queued = 0, .processed = 0};

#define	STAT_INTERVAL	5	/* if a process is busy and does not sleep then update status not faster than */
				/* once in STAT_INTERVAL seconds */

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(info->program_type),
			server_num, get_process_type_string(process_type), process_num);

	zbx_update_selfmon_counter(info, ZBX_PROCESS_STATE_BUSY);

	zbx_setproctitle("%s #%d started", get_process_type_string(process_type), process_num);
	last_stat_time = time(NULL);

	zbx_rtc_subscribe(process_type, process_num, NULL, 0, poller_args_in->config_comms->config_timeout, &rtc);
	http_agent_poller_init(&poller_config, poller_args_in);

	while (ZBX_IS_RUNNING())
	{
		zbx_uint32_t	rtc_cmd;
		unsigned char	*rtc_data;
		struct timeval	tv_pending;

		sec = zbx_time();
		zbx_update_env(get_process_type_string(process_type), sec);

		if (0 == evtimer_pending(poller_config.async_items_timer, &tv_pending))
			evtimer_add(poller_config.async_items_timer, &tv);

		event_base_loop(base, EVLOOP_ONCE);

		poller_requeue_items(&poller_config);

		total_sec += zbx_time() - sec;

		if (STAT_INTERVAL <= time(NULL) - last_stat_time)
		{
			zbx_setproctitle("%s #%d [got %d values, queued %d in " ZBX_FS_DBL " sec]",
				get_process_type_string(process_type), process_num, poller_config.processed,
				poller_config.queued, total_sec);

			poller_config.processed = 0;
			poller_config.queued = 0;
			total_sec = 0.0;
			last_stat_time = time(NULL);
		}

		if (SUCCEED == zbx_rtc_wait(&rtc, info, &rtc_cmd, &rtc_data, 0) && 0 != rtc_cmd)
		{
			if (ZBX_RTC_SHUTDOWN == rtc_cmd)
				break;
		}
	}

	curl_multi_cleanup(curl_handle);
	event_free(curl_timeout);
	event_base_free(base);

	zbx_vector_uint64_clear(&poller_config.itemids);
	zbx_vector_int32_clear(&poller_config.lastclocks);
	zbx_vector_int32_clear(&poller_config.errcodes);
	zbx_vector_uint64_destroy(&poller_config.itemids);
	zbx_vector_int32_destroy(&poller_config.lastclocks);
	zbx_vector_int32_destroy(&poller_config.errcodes);

	zbx_setproctitle("%s #%d [terminated]", get_process_type_string(process_type), process_num);

	while (1)
		zbx_sleep(SEC_PER_MIN);
#undef STAT_INTERVAL
}
