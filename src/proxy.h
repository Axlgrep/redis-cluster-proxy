/*
 * Copyright (C) 2019  Giuseppe Fabio Nicotra <artix2 at gmail dot com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __REDIS_CLUSTER_PROXY_H__
#define __REDIS_CLUSTER_PROXY_H__

#ifdef __STDC_NO_ATOMICS__
#error "Missing support for C11 Atomics, please update your C compiler."
#endif

#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <hiredis.h>
#include <time.h>
#include "ae.h"
#include "anet.h"
#include "cluster.h"
#include "commands.h"
#include "sds.h"
#include "rax.h"
#include "config.h"
#include "version.h"

#define CLIENT_STATUS_NONE          0
#define CLIENT_STATUS_LINKED        1
#define CLIENT_STATUS_UNLINKED      2

#define PROXY_MAIN_THREAD_ID -1
#define PROXY_UNKN_THREAD_ID -999

#define getClientLoop(c) (proxy.threads[c->thread_id]->loop)

struct client;
typedef struct proxyThread {
    int thread_id;
    int io[2];
    pthread_t thread;
    redisCluster *cluster;   /* 每个thread都有自己的集群信息   */
    aeEventLoop *loop;       /* 每个thread都有自己的event loop */
    list *clients;
    list *unlinked_clients;
    list *pending_messages;  /* 存放还没完全通过pipe发送给thread的消息 */
    list *connections_pool;
    int is_spawning_connections;
    uint64_t next_client_id; /* 单调递增的一个客户端id, 溢出后会重置 */
    _Atomic uint64_t process_clients;  /* 该线程上的client数量 */
    sds msgbuffer;
} proxyThread;

typedef struct clientRequest {
    struct client *client;
    uint64_t id;                     /* 请求ID, 客户端接收到请求之后转发到后端Cache节点, 返回Reply并不一定是有序的, 需要依赖这个ID来进行排序, 返回给Client Reply */
    sds buffer;                      /* 用于接收客户端请求的buffer */
    int query_offset;                /* 当前已经解析的buffer偏移量 */
    int is_multibulk;                /* 当前解析的命令是不是multibulk形式, 取值(-1, 0, 1) */
    int argc;                        /* 当前请求的参数个数 */
    int num_commands;
    long long pending_bulks;         /* 当前解析的命令总共有多少bulk */
    long long current_bulk_length;   /* 当前解析的bulk长度为多少 */
    int *offsets;                    /* int array, 用于存储命令每个参数的距离buffer的起始偏移量 */
    int *lengths;                    /* int array, 用于存储命令每个参数的长度 */
    int offsets_size;                /* 实际上就是offsets/lengths数组的字节数, 默认值是 sizeof(int) * QUERY_OFFSETS_MIN_SIZE */
    int slot;
    clusterNode *node;
    struct redisCommandDef *command; /* 指向当前命令的redisCommandDef */
    size_t written;                  /* buffer已经写到cluster的偏移量 */
    int parsing_status;
    int has_write_handler;           /* 如果该值为1, 表示当前请求还没有完整的写到Redis */
    int need_reprocessing;
    int parsed;
    int owned_by_client;             /* 应该是标记当前request是不是私有连接发送(锁定连接) */
    int closes_transaction;
    list *child_requests;
    rax  *child_replies;
    uint64_t max_child_reply_id;
    struct clientRequest *parent_request;
    /* Pointers to *listNode used in various list. They allow to quickly
     * have a reference to the node instead of searching it via listSearchKey.
     */
    listNode *requests_lnode; /* Pointer to node in client->requests list, 指向当前clientRequest在client->requests链表中的位置 */
    listNode *requests_to_send_lnode; /* Pointer to node in
                                       * redisClusterConnection->
                                       *  requests_to_send list */
    listNode *requests_pending_lnode; /* Pointer to node in
                                       * redisClusterConnection->
                                       * requests_pending list */
} clientRequest;

typedef struct {
    aeEventLoop *main_loop;
    int fds[BINDADDR_MAX + 1];
    int fd_count;
    int unixsocket_fd;
    int tcp_backlog;
    char neterr[ANET_ERR_LEN];
    struct proxyThread **threads;
    _Atomic uint64_t numclients;   /* 当前连接client总数 */
    rax *commands;
    int min_reserved_fds;
    time_t start_time;
    sds configfile;
    size_t system_memory_size;
    pthread_t main_thread;
    _Atomic int exit_asap;
} redisClusterProxy;

typedef struct client {
    uint64_t id;                     /* 客户端id(每个线程会维护一个单调递增的client id) */
    int fd;
    sds ip;
    int port;
    sds addr;
    int thread_id;                   /* 所属线程id */
    sds obuf;
    size_t written;
    list *reply_array;
    int status;                      /* 当前client的状态, 连接状态还是断开连接状态 */
    int has_write_handler;
    int flags;
    uint64_t next_request_id;        /* client会为下一个请求分配ID, 赋值给clientRequest里面的id */
    struct clientRequest *current_request; /* Currently reading */
    uint64_t min_reply_id;           /* 在id为min_reply_id之前的clientRequest都是已经从Cache节点接收到Reply并且添加到自己的obuf里面的, 用这个变量来保证proxy有序的向client返回Reply */
    rax *unordered_replies;
    list *requests;                  /* All client's requests */
    list *requests_to_process;       /* Requests not completely parsed */
    int requests_with_write_handler; /* Number of request that are still
                                      * being writing to cluster,
                                      * 这里应该是记录当前还有多少个请求在共享连接上
                                      * 待发送给cluster, 推测应该是在client断连之后,
                                      * 根据这个值来判断什么时候才能释放client对象 */
    list *requests_to_reprocess;     /* Requestst to re-process after cluster
                                      * re-configuration completes */
    int pending_multiplex_requests;  /* Number of request that have to be
                                      * written/read before sending requests
                                      * to private cluster connection,
                                      * 实际上就是说在使用私有连接转发请求之前,
                                      * 还有多少请求在共享连接(全局的)上还未处理完成*/

    redisCluster *cluster;
    int multi_transaction;           /* 客户端当前是否处于事务状态中 */
    clientRequest *multi_request;    /* 指向multi命令的clientRequest */
    clusterNode *multi_transaction_node; /* 如果对当前处于事务当中, 执行事务的目标Cache节点 */
    sds auth_user;                  /* Used by client who wants to authenticate
                                     * itself with different credentials from
                                     * the ones used in the proxy config */
    sds auth_passw;
    /* Pointers to *listNode used in various list. They allow to quickly
     * have a reference to the node instead of searching it via listSearchKey.
     */
    listNode *clients_lnode; /* Pointer to node in thread->clients list */
    listNode *unlinked_clients_lnode; /* Pointer to node in
                                       * thread->unlinked_clients list */
} client;

int getCurrentThreadID(void);
int processRequest(clientRequest *req, int *parsing_status,
    clientRequest **next);
void freeRequest(clientRequest *req);
void freeRequestList(list *request_list);
void onClusterNodeDisconnection(clusterNode *node);

#endif /* __REDIS_CLUSTER_PROXY_H__ */
