/*
 * Copyright (c) 2016 DeNA Co., Ltd., Ichito Nagata
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "async.h"
#include "h2o/redis.h"
#include "h2o/socket.h"

static void attach_loop(redisAsyncContext *ac, h2o_loop_t *loop);

static void on_redis_connect(const redisAsyncContext *redis, int status)
{
    h2o_redis_conn_t *conn = (h2o_redis_conn_t *)redis->data;
    if (status == REDIS_OK) {
        conn->state = H2O_REDIS_CONNECTION_STATE_CONNECTED;
        if (conn->on_connect != NULL) {
            conn->on_connect();
        }
    } else {
        conn->state = H2O_REDIS_CONNECTION_STATE_CLOSED;
        conn->_redis = NULL;
        if (conn->on_close != NULL) {
            conn->on_close(redis->errstr);
        }
    }
}

static void on_redis_disconnect(const redisAsyncContext *redis, int status)
{
    h2o_redis_conn_t *conn = (h2o_redis_conn_t *)redis->data;
    conn->state = H2O_REDIS_CONNECTION_STATE_CLOSED;
    conn->_redis = NULL;
    if (conn->on_close != NULL) {
        conn->on_close(redis->errstr);
    }
}

h2o_redis_conn_t *h2o_redis_create_connection(h2o_loop_t *loop, size_t sz)
{
    h2o_redis_conn_t *conn = h2o_mem_alloc(sz);
    memset(conn, 0, sz);

    conn->loop = loop;
    conn->state = H2O_REDIS_CONNECTION_STATE_CLOSED;
    h2o_timeout_init(conn->loop, &conn->_defer_timeout, 0);

    return conn;
}

static void invoke_deferred(h2o_redis_conn_t *conn, h2o_timeout_entry_t *timeout)
{
    h2o_timeout_link(conn->loop, &conn->_defer_timeout, timeout);
}

static void on_connect_error_deferred(h2o_timeout_entry_t *timeout_entry)
{
    h2o_redis_conn_t *conn = H2O_STRUCT_FROM_MEMBER(h2o_redis_conn_t, _timeout_entry, timeout_entry);
    on_redis_disconnect(conn->_redis, REDIS_ERR);
    h2o_timeout_unlink(timeout_entry);
    redisAsyncFree(conn->_redis);
}

void h2o_redis_connect(h2o_redis_conn_t *conn, const char *host, uint16_t port)
{
    if (conn->state != H2O_REDIS_CONNECTION_STATE_CLOSED) {
        return;
    }

    redisAsyncContext *redis = redisAsyncConnect(host, port);
    if (redis == NULL) {
        h2o_fatal("no memory");
    }

    conn->_redis = redis;
    conn->_redis->data = conn;
    conn->state = H2O_REDIS_CONNECTION_STATE_CONNECTING;

    if (redis->err != REDIS_OK) {
        /* some connection failures can be detected at this time */
        conn->_timeout_entry.cb = on_connect_error_deferred;
        invoke_deferred(conn, &conn->_timeout_entry);
        return;
    }

    attach_loop(redis, conn->loop);
    redisAsyncSetConnectCallback(redis, on_redis_connect);
    redisAsyncSetDisconnectCallback(redis, on_redis_disconnect);
}

void h2o_redis_disconnect(h2o_redis_conn_t *conn)
{
    if (conn->state != H2O_REDIS_CONNECTION_STATE_CLOSED) {
        assert(conn->_redis != NULL);
        conn->state = H2O_REDIS_CONNECTION_STATE_CLOSED;
        redisAsyncDisconnect(conn->_redis);
    }
}

static void on_command(redisAsyncContext *redis, void *_reply, void *privdata)
{
    redisReply *reply = (redisReply *)_reply;
    struct st_h2o_redis_command_t *command = (struct st_h2o_redis_command_t *)privdata;

    int err = H2O_REDIS_ERROR_NONE;
    char *errstr = NULL;
    if (redis->err != REDIS_OK) {
        switch (redis->err) {
        case REDIS_ERR_IO:
        case REDIS_ERR_EOF:
            err = H2O_REDIS_ERROR_CONNECTION;
            break;
        case REDIS_ERR_PROTOCOL:
            err = H2O_REDIS_ERROR_PROTOCOL;
            break;
        case REDIS_ERR_OOM:
        case REDIS_ERR_OTHER:
            err = H2O_REDIS_ERROR_UNKNOWN;
            break;
        default:
            assert(!"FIXME");
        }
        errstr = redis->errstr;
    }

    if (command->cb != NULL) {
        command->cb(reply, command->data, err, errstr);
    }
    free(command);
}

static void on_command_error_deferred(h2o_timeout_entry_t *entry)
{
    struct st_h2o_redis_command_t *command = H2O_STRUCT_FROM_MEMBER(struct st_h2o_redis_command_t, _timeout_entry, entry);
    h2o_timeout_unlink(entry);
    on_command(command->conn->_redis, NULL, command);
}

static h2o_redis_command_t *create_command(h2o_redis_conn_t *conn, h2o_redis_command_cb cb, void *cb_data)
{
    h2o_redis_command_t *command = h2o_mem_alloc(sizeof(h2o_redis_command_t));
    *command = (struct st_h2o_redis_command_t){NULL};
    command->conn = conn;
    command->cb = cb;
    command->data = cb_data;
    command->_timeout_entry.cb = on_command_error_deferred;
    return command;
}

h2o_redis_command_t *h2o_redis_command(h2o_redis_conn_t *conn, h2o_redis_command_cb cb, void *cb_data, const char *format, ...)
{
    h2o_redis_command_t *command = create_command(conn, cb, cb_data);

    if (conn->state == H2O_REDIS_CONNECTION_STATE_CLOSED) {
        invoke_deferred(conn, &command->_timeout_entry);
    } else {
        va_list ap;
        va_start(ap, format);
        int ret = redisvAsyncCommand(conn->_redis, on_command, command, format, ap);
        va_end(ap);

        if (ret != REDIS_OK)
            invoke_deferred(conn, &command->_timeout_entry);
    }
    return command;
}

h2o_redis_command_t *h2o_redis_command_argv(h2o_redis_conn_t *conn, h2o_redis_command_cb cb, void *cb_data, int argc, const char **argv, const size_t *argvlen)
{
    h2o_redis_command_t *command = create_command(conn, cb, cb_data);

    if (conn->state == H2O_REDIS_CONNECTION_STATE_CLOSED) {
        invoke_deferred(conn, &command->_timeout_entry);
    } else if (redisAsyncCommandArgv(conn->_redis, on_command, command, argc, argv, argvlen) != REDIS_OK) {
        invoke_deferred(conn, &command->_timeout_entry);
    }
    return command;
}

void h2o_redis_free(h2o_redis_conn_t *conn)
{
    if (conn->state != H2O_REDIS_CONNECTION_STATE_CLOSED) {
        assert(conn->_redis != NULL);
        redisAsyncDisconnect(conn->_redis);
    }
    h2o_timeout_dispose(conn->loop, &conn->_defer_timeout);
    free(conn);
}

/* redis socket adapter */

struct st_redis_socket_data_t {
    redisAsyncContext *context;
    h2o_socket_t *socket;
};

static void on_read(h2o_socket_t *sock, const char *err)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)sock->data;
    redisAsyncHandleRead(p->context);
}

static void on_write(h2o_socket_t *sock, const char *err)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)sock->data;
    redisAsyncHandleWrite(p->context);
}

static void socket_add_read(void *privdata)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)privdata;
    h2o_socket_read_start(p->socket, on_read);
}

static void socket_del_read(void *privdata)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)privdata;
    h2o_socket_read_stop(p->socket);
}

static void socket_add_write(void *privdata)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)privdata;
    if (!h2o_socket_is_writing(p->socket)) {
        h2o_socket_notify_write(p->socket, on_write);
    }
}

static void socket_cleanup(void *privdata)
{
    struct st_redis_socket_data_t *p = (struct st_redis_socket_data_t *)privdata;
    h2o_socket_close(p->socket);
    p->context->c.fd = -1; /* prevent hiredis from closing fd twice */
    free(p);
}

static void attach_loop(redisAsyncContext *ac, h2o_loop_t *loop)
{
    redisContext *c = &(ac->c);

    struct st_redis_socket_data_t *p = h2o_mem_alloc(sizeof(*p));
    *p = (struct st_redis_socket_data_t){NULL};

    ac->ev.addRead = socket_add_read;
    ac->ev.delRead = socket_del_read;
    ac->ev.addWrite = socket_add_write;
    ac->ev.cleanup = socket_cleanup;
    ac->ev.data = p;

#if H2O_USE_LIBUV
    p->socket = h2o_uv__poll_create(loop, c->fd, (uv_close_cb)free);
#else
    p->socket = h2o_evloop_socket_create(loop, c->fd, H2O_SOCKET_FLAG_DONT_READ);
#endif

    p->socket->data = p;
    p->context = ac;
}
