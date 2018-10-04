#include <http_parser.h>
#include "asterism_inner_http.h"
#include "asterism_core.h"
#include "asterism_log.h"
#include "asterism_utils.h"

static struct asterism_http_inner_s *inner_new(struct asterism_s *as)
{
    struct asterism_http_inner_s *obj = __zero_malloc_st(struct asterism_http_inner_s);
    obj->as = as;
    int ret = uv_tcp_init(as->loop, &obj->socket);
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
cleanup:
    if (ret != 0)
    {
        AS_FREE(obj);
        obj = 0;
    }
    return obj;
}

static void inner_delete(struct asterism_http_inner_s *obj)
{
    AS_FREE(obj);
}

static void inner_close_cb(
    uv_handle_t *handle)
{
    struct asterism_http_inner_s *obj = (struct asterism_http_inner_s *)handle;
    inner_delete(obj);
}

static void inner_close(
    struct asterism_http_inner_s *obj)
{
    if (obj && !uv_is_closing((uv_handle_t *)&obj->socket))
        uv_close((uv_handle_t *)&obj->socket, inner_close_cb);
}

//////////////////////////////////////////////////////////////////////////////////////////////////

static struct asterism_http_incoming_s *incoming_new(struct asterism_s *as)
{
    struct asterism_http_incoming_s *obj = __zero_malloc_st(struct asterism_http_incoming_s);
    obj->as = as;
    int ret = uv_tcp_init(as->loop, &obj->socket);
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
cleanup:
    if (ret != 0)
    {
        AS_FREE(obj);
        obj = 0;
    }
    return obj;
}

static void incoming_delete(struct asterism_http_incoming_s *obj)
{
    if (obj->remote_host)
        AS_FREE(obj->remote_host);
    if (obj->username)
        AS_FREE(obj->username);
    if (obj->password)
        AS_FREE(obj->password);
    AS_FREE(obj);
}

static void incoming_close_cb(
    uv_handle_t *handle)
{
    int ret = 0;
    struct asterism_http_incoming_s *obj = (struct asterism_http_incoming_s *)handle;
    incoming_delete(obj);
    asterism_log(ASTERISM_LOG_DEBUG, "http connection is closing");
}

static void incoming_close(
    struct asterism_http_incoming_s *obj)
{
    if (obj && !uv_is_closing((uv_handle_t *)&obj->socket))
        uv_close((uv_handle_t *)&obj->socket, incoming_close_cb);
}

static void incoming_data_read_alloc_cb(
    uv_handle_t *handle,
    size_t suggested_size,
    uv_buf_t *buf)
{
    struct asterism_http_incoming_s *incoming = (struct asterism_http_incoming_s *)handle;
    if (incoming->link)
	{
		buf->len = ASTERISM_TCP_BLOCK_SIZE;
		buf->base = incoming->buffer;
		incoming->buffer_len = 0;
    }
    else
	{
		if (incoming->header_parsed)
		{
			buf->base = 0;
			buf->len = 0;
			incoming_close(incoming);
			return;
		}
		buf->base = incoming->buffer + incoming->buffer_len;
		buf->len = ASTERISM_MAX_PROTO_SIZE - incoming->buffer_len;
    }
}

static int on_url(http_parser *parser, const char *at, size_t length)
{
    struct asterism_http_incoming_s *obj = __container_ptr(struct asterism_http_incoming_s, parser, parser);
    if (obj->connect_host.p)
    {
        obj->connect_host.len += length;
    }
    else
    {
        obj->connect_host.p = at;
        obj->connect_host.len += length;
    }
    return 0;
}

static int on_header_field(http_parser *parser, const char *at, size_t length)
{
    struct asterism_http_incoming_s *obj = __container_ptr(struct asterism_http_incoming_s, parser, parser);
    if (obj->http_header_field_temp.p)
    {
        obj->http_header_field_temp.len += length;
    }
    else
    {
        obj->http_header_field_temp.p = at;
        obj->http_header_field_temp.len += length;
    }
    if (obj->http_header_value_temp.p)
    {
        if (obj->header_auth_parsed)
        {
            obj->auth_info = obj->http_header_value_temp;
            obj->header_auth_parsed = 0;
        }
        //asterism_log(ASTERISM_LOG_DEBUG, "on_header_value %.*s", obj->http_header_value_temp.len, obj->http_header_value_temp.p);
        obj->http_header_value_temp.p = 0;
        obj->http_header_value_temp.len = 0;
    }
    return 0;
}

static int on_header_value(http_parser *parser, const char *at, size_t length)
{
    struct asterism_http_incoming_s *obj = __container_ptr(struct asterism_http_incoming_s, parser, parser);
    if (obj->http_header_value_temp.p)
    {
        obj->http_header_value_temp.len += length;
    }
    else
    {
        obj->http_header_value_temp.p = at;
        obj->http_header_value_temp.len += length;
    }
    if (obj->http_header_field_temp.p)
    {
        if (asterism_vcmp(&obj->http_header_field_temp, "Proxy-Authorization") == 0)
        {
            obj->header_auth_parsed = 1;
        }
        //asterism_log(ASTERISM_LOG_DEBUG, "on_header_field %.*s", obj->http_header_field_temp.len, obj->http_header_field_temp.p);
        obj->http_header_field_temp.p = 0;
        obj->http_header_field_temp.len = 0;
    }
    return 0;
}

static int on_message_complete(http_parser *parser)
{
    struct asterism_http_incoming_s *obj = __container_ptr(struct asterism_http_incoming_s, parser, parser);
    obj->header_parsed = 1;
    if (obj->http_header_value_temp.p)
    {
        if (obj->header_auth_parsed)
        {
            obj->auth_info = obj->http_header_value_temp;
            obj->header_auth_parsed = 0;
        }
        //asterism_log(ASTERISM_LOG_DEBUG, "on_header_value %.*s", obj->http_header_value_temp.len, obj->http_header_value_temp.p);
        obj->http_header_value_temp.p = 0;
        obj->http_header_value_temp.len = 0;
    }
    if (obj->connect_host.p)
    {
        //asterism_log(ASTERISM_LOG_DEBUG, "on_url %.*s", obj->connect_host.len, obj->connect_host.p);
    }
    return 0;
}

static http_parser_settings parser_settings = {
    0,
    on_url,
    0,
    on_header_field,
    on_header_value,
    0,
    0,
    on_message_complete,
    0,
    0};

static void incoming_shutdown_cb(
    uv_shutdown_t *req,
    int status)
{
    struct asterism_http_incoming_s *incoming = (struct asterism_http_incoming_s *)req->data;
    if (status != 0)
    {
        goto cleanup;
    }
    incoming->fin_send = 1;
    if (incoming->fin_recv)
    {
        incoming_close(incoming);
    }
cleanup:
    if (status != 0)
    {
        incoming_close(incoming);
    }
    AS_FREE(req);
}

static int incoming_end(
    struct asterism_http_incoming_s *incoming)
{
    int ret = 0;
    uv_shutdown_t *req = 0;
    //////////////////////////////////////////////////////////////////////////
    req = __zero_malloc_st(uv_shutdown_t);
    req->data = incoming;
    ret = uv_shutdown(req, (uv_stream_t *)&incoming->socket, incoming_shutdown_cb);
    if (ret != 0)
        goto cleanup;
cleanup:
    if (ret != 0)
    {
        if (req)
        {
            AS_FREE(req);
        }
    }
    return ret;
}

static void handshake_write_cb(
	uv_write_t *req,
	int status)
{
	struct asterism_write_req_s *write_req = (struct asterism_write_req_s *)req;
	free(write_req->write_buffer.base);
	free(write_req);
}

static int incoming_parse_connect(
	struct asterism_http_incoming_s *incoming,
	ssize_t nread,
	const uv_buf_t *buf
) 
{
	size_t nparsed = http_parser_execute(&incoming->parser, &parser_settings, buf->base, nread);
	if (incoming->parser.http_errno != 0)
	{
		asterism_log(ASTERISM_LOG_DEBUG, "%s", http_errno_description((enum http_errno)incoming->parser.http_errno));
		return -1;
	}
	if (nparsed != nread)
		return -1;
	if (!incoming->header_parsed &&
		incoming->buffer_len == ASTERISM_MAX_HTTP_HEADER_SIZE)
		return -1;
	if (incoming->header_parsed)
	{
		if (incoming->parser.method != HTTP_CONNECT)
			return -1;
		if (!incoming->connect_host.len)
			return -1;
		if (!incoming->auth_info.len)
			return -1;
		if (incoming->connect_host.len > MAX_HOST_LEN)
			return -1;
		incoming->remote_host = (char *)asterism_strdup_nul(incoming->connect_host).p;
		struct asterism_str base_prefix = asterism_mk_str("Basic ");
		if (asterism_strncmp(incoming->auth_info, base_prefix, base_prefix.len) != 0)
			return -1;
		char decode_buffer[128] = { 0 };
		int deocode_size = sizeof(decode_buffer);
		int parsed = asterism_base64_decode(
			(const unsigned char *)incoming->auth_info.p + base_prefix.len,
			(int)(incoming->auth_info.len - base_prefix.len),
			decode_buffer,
			&deocode_size);
		if (parsed != incoming->auth_info.len - base_prefix.len)
			return -1;
		char *split_pos = strchr(decode_buffer, ':');
		if (!split_pos)
			return -1;
		*split_pos = 0;
		incoming->username = as_strdup(decode_buffer);
		incoming->password = as_strdup(split_pos + 1);
		asterism_log(ASTERISM_LOG_DEBUG, "http request username: %s , password: %s", incoming->username, incoming->password);
		struct asterism_session_s sefilter;
		sefilter.username = incoming->username;
		struct asterism_session_s* session = RB_FIND(asterism_session_tree_s, &incoming->as->sessions, &sefilter);
		//获取步到此用户
		if (!session)
			return -1;
		//密码验证失败
		if (strcmp(session->password, incoming->password))
			return -1;

		//session->inner = incoming;
		//incoming->tunnel_connected = 1;
		incoming->session = session;

		//创建握手会话
		struct asterism_handshake_s* handshake = __zero_malloc_st(struct asterism_handshake_s);
		handshake->inner = (struct asterism_stream_s *)incoming;
		handshake->id = asterism_tunnel_new_handshake_id();

		//通过session转发连接请求
		struct asterism_write_req_s* req = __zero_malloc_st(struct asterism_write_req_s);

		//注意修改这里，分配内存****
		struct asterism_trans_proto_s *connect_data = 
			(struct asterism_trans_proto_s *)malloc(sizeof(struct asterism_trans_proto_s) +
			incoming->connect_host.len + 2 + 4 );

		connect_data->version = ASTERISM_TRANS_PROTO_VERSION;
		connect_data->cmd = ASTERISM_TRANS_PROTO_CONNECT;

		char *off = (char *)connect_data + sizeof(struct asterism_trans_proto_s);
		*(uint32_t *)off = htonl(handshake->id);
		off += 4;
		*(uint16_t *)off = htons((uint16_t)incoming->connect_host.len);
		off += 2;
		memcpy(off, incoming->connect_host.p, incoming->connect_host.len);
		off += incoming->connect_host.len;

		uint16_t packet_len = (uint16_t)(off - (char *)connect_data);
		connect_data->len = htons(packet_len);

		req->write_buffer.base = (char *)connect_data;
		req->write_buffer.len = packet_len;

		incoming->buffer_len = 0;

		int write_ret = uv_write((uv_write_t*)req, (uv_stream_t*)session->outer, &req->write_buffer, 1, handshake_write_cb);
		if (write_ret != 0) {
			free(req->write_buffer.base);
			free(req);
			free(handshake);
			return -1;
		}

		RB_INSERT(asterism_handshake_tree_s, &incoming->as->handshake_set, handshake);
		asterism_log(ASTERISM_LOG_DEBUG, "header_parsed");
	}
	return 0;
}


static void incoming_read_cb(
	uv_stream_t *stream,
	ssize_t nread,
	const uv_buf_t *buf);

static void link_write_cb(
	uv_write_t *req,
	int status)
{
	struct asterism_http_incoming_s *incoming = (struct asterism_http_incoming_s *)req->data;
	if (uv_read_start((uv_stream_t *)&incoming->socket, incoming_data_read_alloc_cb, incoming_read_cb)) {
		incoming_close(incoming);
	}
}

static void incoming_read_cb(
    uv_stream_t *stream,
    ssize_t nread,
    const uv_buf_t *buf)
{
    struct asterism_http_incoming_s *incoming = (struct asterism_http_incoming_s *)stream;
    if (nread > 0)
    {
        if (incoming->link)
        {
			memset(&incoming->link->write_req, 0, sizeof(incoming->link->write_req));
			incoming->link->write_req.data = stream;
			uv_buf_t _buf;
			_buf.base = buf->base;
			_buf.len = nread;
			if (uv_write(&incoming->link->write_req, (uv_stream_t *)incoming->link, &_buf, 1, link_write_cb)) {
				incoming_close(incoming);
				return;
			}
			if (uv_read_stop(stream)) {
				incoming_close(incoming);
				return;
			}
            //transport
			//asterism_log(ASTERISM_LOG_DEBUG, "%.*s", nread, buf->base);
        }
        else
        {
            incoming->buffer_len += (unsigned int)nread;
			if (incoming_parse_connect(incoming, nread, buf) != 0) {
				incoming_close(incoming);
			}
		}
    }
    else if (nread == 0)
    {
		return;
    }
    else if (nread == UV_EOF)
    {
        incoming->fin_recv = 1;
        if (incoming->fin_send)
        {
            incoming_close(incoming);
        }
        else
        {
            incoming_end(incoming);
        }
    }
    else
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror((int)nread));
        incoming_close(incoming);
    }
}

static void inner_accept_cb(
    uv_stream_t *stream,
    int status)
{
    int ret = ASTERISM_E_OK;
    asterism_log(ASTERISM_LOG_DEBUG, "new http connection is comming");

    struct asterism_http_inner_s *inner = (struct asterism_http_inner_s *)stream;
    struct asterism_http_incoming_s *incoming = 0;
    if (status != 0)
    {
        goto cleanup;
    }
    incoming = incoming_new(inner->as);
    if (!incoming)
    {
        goto cleanup;
    }
    http_parser_init(&incoming->parser, HTTP_REQUEST);
    ret = uv_accept((uv_stream_t *)&inner->socket, (uv_stream_t *)&incoming->socket);
    if (ret != 0)
    {
        ret = ASTERISM_E_FAILED;
        goto cleanup;
    }
    ret = uv_read_start((uv_stream_t *)&incoming->socket, incoming_data_read_alloc_cb, incoming_read_cb);
    if (ret != 0)
    {
        ret = ASTERISM_E_FAILED;
        goto cleanup;
    }
cleanup:
    if (ret != 0)
    {
        incoming_close(incoming);
    }
}

int asterism_inner_http_init(
    struct asterism_s *as,
    const char *ip, unsigned int *port, int ipv6)
{
    int ret = ASTERISM_E_OK;
    void *addr = 0;
    int name_len = 0;
    struct asterism_http_inner_s *inner = inner_new(as);
    if (!inner)
    {
        goto cleanup;
    }

    if (ipv6)
    {
        addr = __zero_malloc_st(struct sockaddr_in6);
        name_len = sizeof(struct sockaddr_in6);
        ret = uv_ip6_addr(ip, (int)*port, (struct sockaddr_in6 *)addr);
    }
    else
    {
        addr = __zero_malloc_st(struct sockaddr_in);
        name_len = sizeof(struct sockaddr_in);
        ret = uv_ip4_addr(ip, (int)*port, (struct sockaddr_in *)addr);
    }
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
    ret = uv_tcp_bind(&inner->socket, (const struct sockaddr *)addr, 0);
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
    ret = uv_tcp_getsockname(&inner->socket, (struct sockaddr *)addr, &name_len);
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
    if (ipv6)
    {
        *port = ntohs(((struct sockaddr_in6 *)addr)->sin6_port);
    }
    else
    {
        *port = ntohs(((struct sockaddr_in *)addr)->sin_port);
    }

    ret = uv_listen((uv_stream_t *)&inner->socket, ASTERISM_NET_BACKLOG, inner_accept_cb);
    if (ret != 0)
    {
        asterism_log(ASTERISM_LOG_DEBUG, "%s", uv_strerror(ret));
        ret = ASTERISM_E_SOCKET_LISTEN_ERROR;
        goto cleanup;
    }
cleanup:
    if (ret)
    {
        inner_close(inner);
    }
    return ret;
}