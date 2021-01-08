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

#ifndef __REDIS_CLUSTER_PROXY_CLUSTER_H__
#define __REDIS_CLUSTER_PROXY_CLUSTER_H__

#include "sds.h"
#include "adlist.h"
#include "rax.h"
#include "config.h"
#include <hiredis.h>
#include <time.h>

#define CLUSTER_SLOTS 16384
#define CLUSTER_RECONFIG_ERR        -1
#define CLUSTER_RECONFIG_ENDED      0
#define CLUSTER_RECONFIG_WAIT       1
#define CLUSTER_RECONFIG_STARTED    2
#define getClusterNodeContext(node) (node->connection->context)
#define isClusterNodeConnected(node) (node->connection->connected)

struct redisCluster;
struct clusterNode;

typedef struct redisClusterConnection {
    redisContext *context;
    list *requests_to_send;
    list *requests_pending;
    int connected;
    int has_read_handler;
    int authenticating;                   /* 正在处于认证过程中(auth命令还未发送完成) */
    int authenticated;                    /* 该条连接是否已经通过认证 */
    struct clusterNode *node;
} redisClusterConnection;

typedef struct clusterNode {
    redisClusterConnection *connection;   /* 当前节点的连接对象 */
    struct redisCluster *cluster;         /* 节点所属集群 */
    sds ip;                               /* 节点ip */
    int port;                             /* 节点端口号 */
    sds name;                             /* 当前节点的节点id */
    int flags;
    sds replicate;                        /* 如果是slave节点, 那么记录它的master节点id */
    int is_replica;                       /* 是不是slave节点 */
    int *slots;                           /* 存放当前节点持有的具体slot id */
    int slots_count;                      /* 当前节点持有slot数量 */
    int replicas_count; /* Value is always -1 until 'PROXY CLUSTER' command
                         * counts all replicas and stores the result in
                         * `replicas_count`, that is actually used as a
                         * cache. */
    sds *migrating; /* An array of sds where even strings are slots and odd
                     * strings are the destination node IDs.
                     * 第一个元素存储slot, 第二个元素存储迁出节点id, 以此类推
                     */
    sds *importing; /* An array of sds where even strings are slots and odd
                     * strings are the source node IDs.
                     * 第一个元素存储slot, 第二个元素存储迁入节点id, 以此类推
                     */
    int migrating_count; /* Length of the migrating array (migrating slots*2), migrating数组的大小 */
    int importing_count; /* Length of the importing array (importing slots*2), importing数组的大小 */
    struct clusterNode *duplicated_from;   /* 是从哪个clusterNode复制而来的 */
} clusterNode;

typedef struct redisCluster {
    int thread_id;                          /* 当前redisCluster对象属于哪个thread */
    list *nodes;                            /* 存储当前集群的节点链表             */
    rax  *slots_map;                        /* 存放slot和节点的对应关系           */
    rax  *nodes_by_name;                    /* 存储节点id和节点的映射关系         */
    list *master_names;
    int masters_count;                      /* master节点的数量                   */
    int replicas_count;                     /* slave节点的数量                    */
    redisClusterEntryPoint *entry_point;    /* 可达节点地址                       */
    rax  *requests_to_reprocess;
    int is_updating;
    int update_required;
    int broken;
    struct redisCluster *duplicated_from;
    list *duplicates;
    void *owner; /* Can be the client in case of private cluster, 当前redisCluster是否被某个client独占 */
} redisCluster;

redisCluster *createCluster(int thread_id);
int resetCluster(redisCluster *cluster);
redisCluster *duplicateCluster(redisCluster *source);
void freeCluster(redisCluster *cluster);
int fetchClusterConfiguration(redisCluster *cluster,
                              redisClusterEntryPoint* entry_points,
                              int entry_points_count);
redisContext *clusterNodeConnect(clusterNode *node);
void clusterNodeDisconnect(clusterNode *node);
clusterNode *searchNodeBySlot(redisCluster *cluster, int slot);
clusterNode *getNodeByKey(redisCluster *cluster, char *key, int keylen,
                          int *getslot);
clusterNode *getNodeByName(redisCluster *cluster, const char *name);
clusterNode *getFirstMappedNode(redisCluster *cluster);
list *clusterGetMasterNames(redisCluster *cluster);
int updateCluster(redisCluster *cluster);
void clusterAddRequestToReprocess(redisCluster *cluster, void *r);
void clusterRemoveRequestToReprocess(redisCluster *cluster, void *r);
int clusterNodeAuth(clusterNode *node, char *auth, char *user, char **err);
redisClusterConnection *createClusterConnection(void);
void freeClusterConnection(redisClusterConnection *conn);
redisClusterEntryPoint *copyEntryPoint(redisClusterEntryPoint *source);
void freeEntryPoints(redisClusterEntryPoint *entry_points, int count);
#endif /* __REDIS_CLUSTER_PROXY_CLUSTER_H__ */
