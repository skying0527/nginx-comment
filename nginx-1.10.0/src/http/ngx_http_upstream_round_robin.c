
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define ngx_http_upstream_tries(p) ((p)->number                               \
                                    + ((p)->next ? (p)->next->number : 0))


static ngx_http_upstream_rr_peer_t *ngx_http_upstream_get_peer(
    ngx_http_upstream_rr_peer_data_t *rrp);

#if (NGX_HTTP_SSL)

static ngx_int_t ngx_http_upstream_empty_set_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_empty_save_session(ngx_peer_connection_t *pc,
    void *data);

#endif

/*
 * 这个函数主要用于构造后端服务其列表，即把从配置文件中获取到的后端服务器的信息分类
 * 管理起来，在解析完http块下的main级别配置项之后调用，每个upstream块都对应需要调用
 * 这个函数
 */
ngx_int_t
ngx_http_upstream_init_round_robin(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_url_t                      u;
    ngx_uint_t                     i, j, n, w;
    ngx_http_upstream_server_t    *server;
    ngx_http_upstream_rr_peer_t   *peer, **peerp;
    ngx_http_upstream_rr_peers_t  *peers, *backup;

    us->peer.init = ngx_http_upstream_init_round_robin_peer;

    /*
     * 创建后端服务器列表，并且非备份服务器和备份服务器分别组成两个列表，每一个后端服务器用
     * 一个结构体ngx_http_upstream_rr_peer_t表示，列表最前面需要带有一个头信息，所以需要用
     * 一个结构体ngx_http_upstream_rr_peers_t来组织这些后端服务器组成的列表。非备份服务器列表
     * 挂载在us->peers.data字段下，备份服务器列表挂载在非备份服务器列表head域里的next字段下。
     */

    if (us->servers) {
        server = us->servers->elts;

        /* 构建非备份后端服务器组成的列表 */

        n = 0;
        w = 0;

        /*
         * 遍历配置文件中配置的所有后端服务器，计算总的后端服务器个数(因为一个后端服务器可能有多个ip地址)
         * 以及总的权重值。如果一个后端服务器有多个ip，那么就算有多个后端服务器配置
         */
        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {  // 备份服务器后续单独组成一个列表
                continue;
            }

            /*
             * 如果某台后端服务器有多个ip地址，那么就当做有多个后端服务器配置，需要分配对应数量的配置内存
             */
            n += server[i].naddrs;
            w += server[i].naddrs * server[i].weight;
        }

        /* n == 0表明upstream块里面没有配置server选项，这是不合理的 */
        if (n == 0) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "no servers in upstream \"%V\" in %s:%ui",
                          &us->host, us->file_name, us->line);
            return NGX_ERROR;
        }

        /* ngx_http_upstream_rr_peers_t对象用来管理后端服务器列表，备份和非备份的都用这个对象管理 */
        peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t));
        if (peers == NULL) {
            return NGX_ERROR;
        }

        /* 申请用来存放非备份服务器配置的内存 */
        peer = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peer_t) * n);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        /* 根据解析得到的信息设置后端服务器列表管理对象ngx_http_upstream_rr_peers_t中的字段值 */
        peers->single = (n == 1);
        peers->number = n;
        peers->weighted = (w != n);
        peers->total_weight = w;
        peers->name = &us->host;

        n = 0;
        peerp = &peers->peer;

        /* 将从配置文件中获取的非备份后端服务器信息保存到ngx_http_upstream_rr_peer_t对象中统一管理 */
        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {
                continue;
            }

            /* 一个后端服务器可能有多个ip地址(一个域名可能有多个冗余ip) */
            for (j = 0; j < server[i].naddrs; j++) {
                peer[n].sockaddr = server[i].addrs[j].sockaddr;
                peer[n].socklen = server[i].addrs[j].socklen;
                peer[n].name = server[i].addrs[j].name;
                peer[n].weight = server[i].weight;
                peer[n].effective_weight = server[i].weight;
                peer[n].current_weight = 0;
                peer[n].max_fails = server[i].max_fails;
                peer[n].fail_timeout = server[i].fail_timeout;
                peer[n].down = server[i].down;
                peer[n].server = server[i].name;

                *peerp = &peer[n];
                peerp = &peer[n].next;
                n++;
            }
        }

        /* 将管理着非备份后端服务器列表的ngx_http_upstream_rr_peers_t对象挂载到us->peer.data中 */
        us->peer.data = peers;

        /* backup servers */

        /* 构建备份后端服务器组成的列表 */

        n = 0;
        w = 0;

        for (i = 0; i < us->servers->nelts; i++) {
            if (!server[i].backup) {
                continue;
            }

            n += server[i].naddrs;
            w += server[i].naddrs * server[i].weight;
        }

        /* 没有非备份后端服务器也是可以的，所以这里返回NGX_OK */
        if (n == 0) {
            return NGX_OK;
        }

        /* ngx_http_upstream_rr_peers_t对象用来管理后端服务器列表，备份和非备份的都用这个对象管理 */
        backup = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t));
        if (backup == NULL) {
            return NGX_ERROR;
        }

         /* 申请用来存放备份服务器配置的内存 */
        peer = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peer_t) * n);
        if (peer == NULL) {
            return NGX_ERROR;
        }

        /* 根据解析得到的信息设置后端服务器列表管理对象ngx_http_upstream_rr_peers_t中的字段值 */
        peers->single = 0;  // 因为存在备份服务器，所以对于整个upstream块来说就不只有一个后端服务器了
        backup->single = 0;
        backup->number = n;
        backup->weighted = (w != n);
        backup->total_weight = w;
        backup->name = &us->host;

        n = 0;
        peerp = &backup->peer;

        /* 将从配置文件中获取的备份后端服务器信息保存到ngx_http_upstream_rr_peer_t对象中统一管理 */
        for (i = 0; i < us->servers->nelts; i++) {
            if (!server[i].backup) {
                continue;
            }

            /* 一个后端服务器可能有多个ip地址(一个域名可能有多个冗余ip) */
            for (j = 0; j < server[i].naddrs; j++) {
                peer[n].sockaddr = server[i].addrs[j].sockaddr;
                peer[n].socklen = server[i].addrs[j].socklen;
                peer[n].name = server[i].addrs[j].name;
                peer[n].weight = server[i].weight;
                peer[n].effective_weight = server[i].weight;
                peer[n].current_weight = 0;
                peer[n].max_fails = server[i].max_fails;
                peer[n].fail_timeout = server[i].fail_timeout;
                peer[n].down = server[i].down;
                peer[n].server = server[i].name;

                *peerp = &peer[n];
                peerp = &peer[n].next;
                n++;
            }
        }

        /* 将备份后端服务器组成的列表管理对象挂载到非备份后端服务器列表管理对象的next字段下 */
        peers->next = backup;

        return NGX_OK;
    }


    /* an upstream implicitly defined by proxy_pass, etc. */
    /*
     * 程序执行到这里表明没有配置upstream块，只是用proxy_pass等命令配置了一个url，用来指定
     * 后端服务器，这个时候也会创建一个存储upstream块配置信息的结构体，只是这个结构体里面
     * servers等信息为NULL。这个时候就需要用url参数指定的域名来解析ip地址。如果url参数对应
     * 的域名也对应多个ip地址，那么也需要进行管理，另外，这个时候是没有备份服务器的
     */

    if (us->port == 0) {
        ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                      "no port in upstream \"%V\" in %s:%ui",
                      &us->host, us->file_name, us->line);
        return NGX_ERROR;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));

    /* 获取域名字符串以及端口信息 */
    u.host = us->host;
    u.port = us->port;

    /* 解析域名，获取ip地址以及主机名字 */
    if (ngx_inet_resolve_host(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
                          "%s in upstream \"%V\" in %s:%ui",
                          u.err, &us->host, us->file_name, us->line);
        }

        return NGX_ERROR;
    }

    n = u.naddrs;  // 域名对应的ip地址个数

    /* 创建管理后端服务器列表的对象 */
    peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peers_t));
    if (peers == NULL) {
        return NGX_ERROR;
    }

    peer = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_rr_peer_t) * n);
    if (peer == NULL) {
        return NGX_ERROR;
    }

    peers->single = (n == 1);
    peers->number = n; // proxy_pass等命令配置的后端服务器个数(以ip计数)
    peers->weighted = 0;  // 采用默认配置权重，标志位清零
    peers->total_weight = n;  // 总权重就是后端服务器个数(以ip计数，默认权重是1)
    peers->name = &us->host;

    peerp = &peers->peer;

    /* 将域名对应的多个ip地址分别用一个后端服务器信息对象进行管理 */
    for (i = 0; i < u.naddrs; i++) {
        peer[i].sockaddr = u.addrs[i].sockaddr;
        peer[i].socklen = u.addrs[i].socklen;
        peer[i].name = u.addrs[i].name;
        peer[i].weight = 1;
        peer[i].effective_weight = 1;
        peer[i].current_weight = 0;
        peer[i].max_fails = 1;
        peer[i].fail_timeout = 10;
        *peerp = &peer[i];
        peerp = &peer[i].next;
    }

    /* 将管理后端服务器列表的对象挂载到us->peer.data */
    us->peer.data = peers;

    /* implicitly defined upstream has no backup servers */

    return NGX_OK;
}

/*
 * 该函数会设置get和free方法，以及构造指示所有后端服务器是否被选择过的位图，除此之外，
 * 也会将下面data字段挂载的后端服务器列表设置到r->upstream->peer.data上。该函数在构造
 * 发送往上游服务器的请求时调用，见函数ngx_http_upstream_init_request()
 */
ngx_int_t
ngx_http_upstream_init_round_robin_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                         n;
    ngx_http_upstream_rr_peer_data_t  *rrp;

    /* 创建ngx_http_upstream_rr_peer_data_t对象，挂载到r->upstream->peer.data上 */
    rrp = r->upstream->peer.data;

    if (rrp == NULL) {
        rrp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_rr_peer_data_t));
        if (rrp == NULL) {
            return NGX_ERROR;
        }

        r->upstream->peer.data = rrp;
    }

    /*
     * rrp->peers就是用来管理后端服务器列表的对象，在这里设置为us->peer.data，
     * us->peer.data在函数ngx_http_upstream_init_round_robin中构造。
     */
    /*
     *     将后端服务器列表挂载到peers字段中。对于不同的客户端请求，函数
     * ngx_http_upstream_init_round_robin_peer()都会被调用一次，r->upstream->peer.data
     * 也会被重新创建，但是构造us->peer.data后端服务器列表信息的函数只会在配置文件解析完main级别
     * 配置项之后调用一次，之后不会再调用(函数为:ngx_http_upstream_init_round_robin)。
     *     rrp->peers = us->peer.data;这条赋值语句表明对于不同的客户端请求，他们所使用的后端服务器
     * 列表信息是同一份的，也就是说先来的客户端请求会对后来的客户端请求如何选择后端服务器有影响。
     * 举个例子，假如有两个客户端连接，第一个客户端连接选择了其中一台后端服务器，那么就会更新这一台
     * 后端服务器的一些信息，比如当前权重，被选中次数及有效权重等信息到后端服务器列表中的对应位置，
     * 这些都会对后续第二个客户端请求有影响，因为不同的客户端请求之间所使用的后端服务器列表信息是同一份。
     *     但是下面用来指示后端服务器是否被选中的位图是每个请求都会创建一份，也就是说位图在不同的客户端
     * 请求之间是不共用的。比如对于同一台后端服务器，在第一个客户端请求的时候被选中了，它的权重信息被
     * 第一个请求处理更新了，位图中对应位置也被置位了。此时来了第二个客户端请求，由于这台后端服务器权重
     * 信息被第一个请求更新过，所以会影响第二个客户端请求的选择，但是在第二个请求中对应该后端服务器的
     * 位图中的位是没有被置位的。因为位图是每个请求独有的。
     */
    rrp->peers = us->peer.data;
    rrp->current = NULL;

    n = rrp->peers->number;  // rrp->peers->number指的是非备份后端服务器的总数(以ip计数)

    /*
     * 如果存在备份的后端服务器，并且备份的后端服务器总数(以ip计数)比非备份的后端服务器总数多，
     * 则以非备份的后端服务器总数赋值给局部变量n
     */
    if (rrp->peers->next && rrp->peers->next->number > n) {
        n = rrp->peers->next->number;
    }

    /*
     * 比较后端服务器总数(以ip计数)是否超过了一个uintptr_t类型的位数，用于后续构造位图，
     * 如果总数n小于uintptr_t类型的位数，则用一个uintptr_t类型的变量来存储位图，这个位图
     * 是用来标记在一轮选择中，某个后端服务器是否被选中的标志位。位图是面向一轮选择的，即
     * 针对一个客户端请求。
     */
    if (n <= 8 * sizeof(uintptr_t)) {
        rrp->tried = &rrp->data;
        rrp->data = 0;

    } else {
        /*
         * 程序执行到这里表明后端服务器的总数超过了一个uintptr_t类型的位数，那么就需要多个
         * uintptr_t类型的变量来存放位图了，下面就是计算需要多少个uintptr_t类型变量的算法，
         * 这里以37台后端服务器为例说明下，假设是在32位机器上，那么uintptr_t类型就是32位的。
         * 那么我们就需要两个uintptr_t类型的变量，所以有:
         * (37 + 31)/32 = 2
         */
        n = (n + (8 * sizeof(uintptr_t) - 1)) / (8 * sizeof(uintptr_t));

        rrp->tried = ngx_pcalloc(r->pool, n * sizeof(uintptr_t));
        if (rrp->tried == NULL) {
            return NGX_ERROR;
        }
    }

    /* 设置get、free回调方法，这两个是用来选择和释放本次要连接的后端服务器的 */
    r->upstream->peer.get = ngx_http_upstream_get_round_robin_peer;
    r->upstream->peer.free = ngx_http_upstream_free_round_robin_peer;

    /*
     * 设置在连接一个远端服务器时，当前连接出现异常失败后可以重试的次数，
     * 函数ngx_http_upstream_tries()会计算后端服务器的总数，包括备份和非备份
     */
    r->upstream->peer.tries = ngx_http_upstream_tries(rrp->peers);
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session =
                               ngx_http_upstream_set_round_robin_peer_session;
    r->upstream->peer.save_session =
                               ngx_http_upstream_save_round_robin_peer_session;
#endif

    return NGX_OK;
}


ngx_int_t
ngx_http_upstream_create_round_robin_peer(ngx_http_request_t *r,
    ngx_http_upstream_resolved_t *ur)
{
    u_char                            *p;
    size_t                             len;
    socklen_t                          socklen;
    ngx_uint_t                         i, n;
    struct sockaddr                   *sockaddr;
    ngx_http_upstream_rr_peer_t       *peer, **peerp;
    ngx_http_upstream_rr_peers_t      *peers;
    ngx_http_upstream_rr_peer_data_t  *rrp;

    rrp = r->upstream->peer.data;

    if (rrp == NULL) {
        rrp = ngx_palloc(r->pool, sizeof(ngx_http_upstream_rr_peer_data_t));
        if (rrp == NULL) {
            return NGX_ERROR;
        }

        r->upstream->peer.data = rrp;
    }

    /* 申请管理后端服务器列表的对象 */
    peers = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_rr_peers_t));
    if (peers == NULL) {
        return NGX_ERROR;
    }

    /*
     * 申请用于存放后端服务器信息的内存，以ip计数作为后端服务器个数，
     * 因为一个后端服务器可能有多个ip 
     */
    peer = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_rr_peer_t)
                                * ur->naddrs);
    if (peer == NULL) {
        return NGX_ERROR;
    }

    /* 设置后端服务器列表管理对象的信息 */
    peers->single = (ur->naddrs == 1);
    peers->number = ur->naddrs;
    peers->name = &ur->host;

    /* 构造后端服务器列表 */
    if (ur->sockaddr) {
        peer[0].sockaddr = ur->sockaddr;
        peer[0].socklen = ur->socklen;
        peer[0].name = ur->host;
        peer[0].weight = 1;
        peer[0].effective_weight = 1;
        peer[0].current_weight = 0;
        peer[0].max_fails = 1;
        peer[0].fail_timeout = 10;
        peers->peer = peer;

    } else {
        peerp = &peers->peer;

        for (i = 0; i < ur->naddrs; i++) {

            socklen = ur->addrs[i].socklen;

            sockaddr = ngx_palloc(r->pool, socklen);
            if (sockaddr == NULL) {
                return NGX_ERROR;
            }

            ngx_memcpy(sockaddr, ur->addrs[i].sockaddr, socklen);

            switch (sockaddr->sa_family) {
#if (NGX_HAVE_INET6)
            case AF_INET6:
                ((struct sockaddr_in6 *) sockaddr)->sin6_port = htons(ur->port);
                break;
#endif
            default: /* AF_INET */
                ((struct sockaddr_in *) sockaddr)->sin_port = htons(ur->port);
            }

            p = ngx_pnalloc(r->pool, NGX_SOCKADDR_STRLEN);
            if (p == NULL) {
                return NGX_ERROR;
            }

            /* 将ip地址转换为字符串形式，存放在p中 */
            len = ngx_sock_ntop(sockaddr, socklen, p, NGX_SOCKADDR_STRLEN, 1);

            peer[i].sockaddr = sockaddr;
            peer[i].socklen = socklen;
            peer[i].name.len = len;
            peer[i].name.data = p;
            peer[i].weight = 1;
            peer[i].effective_weight = 1;
            peer[i].current_weight = 0;
            peer[i].max_fails = 1;
            peer[i].fail_timeout = 10;
            *peerp = &peer[i];
            peerp = &peer[i].next;
        }
    }

    rrp->peers = peers;
    rrp->current = NULL;

     /*
      * 比较后端服务器总数(以ip计数)是否超过了一个uintptr_t类型的位数，用于后续构造位图，
      * 如果总数n小于uintptr_t类型的位数，则用一个uintptr_t类型的变量来存储位图，这个位图
      * 是用来标记在一轮选择中，某个后端服务器是否被选中的标志位。位图是面向一轮选择的，即
      * 针对一个客户端请求。
      */    
    if (rrp->peers->number <= 8 * sizeof(uintptr_t)) {
        rrp->tried = &rrp->data;
        rrp->data = 0;

    } else {
         /*
          * 程序执行到这里表明后端服务器的总数超过了一个uintptr_t类型的位数，那么就需要多个
          * uintptr_t类型的变量来存放位图了，下面就是计算需要多少个uintptr_t类型变量的算法，
          * 这里以37台后端服务器为例说明下，假设是在32位机器上，那么uintptr_t类型就是32位的。
          * 那么我们就需要两个uintptr_t类型的变量，所以有:
          * (37 + 31)/32 = 2
          */
        n = (rrp->peers->number + (8 * sizeof(uintptr_t) - 1))
                / (8 * sizeof(uintptr_t));

        rrp->tried = ngx_pcalloc(r->pool, n * sizeof(uintptr_t));
        if (rrp->tried == NULL) {
            return NGX_ERROR;
        }
    }

    /* 设置get、free回调方法，这两个方法是用来选择和释放本次要连接的后端服务器的 */
    r->upstream->peer.get = ngx_http_upstream_get_round_robin_peer;
    r->upstream->peer.free = ngx_http_upstream_free_round_robin_peer;

     /*
      * 设置在连接一个远端服务器时，当前连接出现异常失败后可以重试的次数，
      * 函数ngx_http_upstream_tries()会计算后端服务器的总数，包括备份和非备份
      */
    r->upstream->peer.tries = ngx_http_upstream_tries(rrp->peers);
#if (NGX_HTTP_SSL)
    r->upstream->peer.set_session = ngx_http_upstream_empty_set_session;
    r->upstream->peer.save_session = ngx_http_upstream_empty_save_session;
#endif

    return NGX_OK;
}

/*
 * 从连接对应的upstream块内选择一台合适的后端服务器，后续会向这个服务器发起建链请求
 */
ngx_int_t
ngx_http_upstream_get_round_robin_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_rr_peer_data_t  *rrp = data;

    ngx_int_t                      rc;
    ngx_uint_t                     i, n;
    ngx_http_upstream_rr_peer_t   *peer;
    ngx_http_upstream_rr_peers_t  *peers;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "get rr peer, try: %ui", pc->tries);

    pc->cached = 0;
    pc->connection = NULL;

    /* 获取管理后端服务器列表的对象 */
    peers = rrp->peers;

    /* 获取写锁 */
    ngx_http_upstream_rr_peers_wlock(peers);

    /* 如果只有一个后端服务器，那么就选这个后端服务器 */
    if (peers->single) {
        peer = peers->peer;

        if (peer->down) {
            goto failed;
        }

        /* 设置rrp->current为当前选中的后端服务器对象 */
        rrp->current = peer;

    } else {

        /* there are several peers */
        /*
         * 程序执行到这里说明有多台后端服务器，则调用ngx_http_upstream_get_peer()获取
         * 一台最合适的后端服务器。
         */
        peer = ngx_http_upstream_get_peer(rrp);

        if (peer == NULL) {
            goto failed;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "get rr peer, current: %p %i",
                       peer, peer->current_weight);
    }

    /* 将选中的那个后端服务器地址信息设置到主动连接对象中 */
    pc->sockaddr = peer->sockaddr;
    pc->socklen = peer->socklen;
    pc->name = &peer->name;

    /* 记录此后端服务器选中的次数递增 */
    peer->conns++;

    ngx_http_upstream_rr_peers_unlock(peers);

    return NGX_OK;

failed:

    /*
     * 程序执行到这里表明在非备份服务器列表没有找到合适的非备份服务器，这个时候如果配置了
     * 备份服务器，则从备份服务器列表中获取合适的后端服务器
     */
    if (peers->next) {

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0, "backup servers");

        rrp->peers = peers->next;

        /*
         * 计算所有备份服务器需要的位图个数，我们知道，在ngx_http_upstream_init_round_robin_peer()
         * 函数中，申请rrp->tried成员指向的位图内存地址时是按照备份服务器总数和非备份服务器总数
         * 中的大数来申请所需内存的，因此rrp->tried中的内存肯定是够用的。
         */
        n = (rrp->peers->number + (8 * sizeof(uintptr_t) - 1))
                / (8 * sizeof(uintptr_t));

        /* 初始化所有备份服务器对应的位图，即位图中的位清零 */
        for (i = 0; i < n; i++) {
            rrp->tried[i] = 0;
        }

        ngx_http_upstream_rr_peers_unlock(peers);

        /* 从备份服务器列表中选取一台后端服务器作为此次要连接的后端服务器 */
        rc = ngx_http_upstream_get_round_robin_peer(pc, rrp);

        if (rc != NGX_BUSY) {
            return rc;
        }

        ngx_http_upstream_rr_peers_wlock(peers);
    }

    /* all peers failed, mark them as live for quick recovery */
    /*
     * 程序执行到这里表明从非备份服务器列表和备份服务器列表中获取合适的后端服务器
     * 都失败了，则只能返回NGX_BUSY。
     */

    /* 将所有的后端服务器的失败次数清零，表示后续请求可以继续从这些服务器中进行选择 */
    for (peer = peers->peer; peer; peer = peer->next) {
        peer->fails = 0;
    }

    ngx_http_upstream_rr_peers_unlock(peers);

    pc->name = peers->name;

    return NGX_BUSY;
}

/* 真正执行加权轮询算法的函数 */
static ngx_http_upstream_rr_peer_t *
ngx_http_upstream_get_peer(ngx_http_upstream_rr_peer_data_t *rrp)
{
    time_t                        now;
    uintptr_t                     m;
    ngx_int_t                     total;
    ngx_uint_t                    i, n, p;
    ngx_http_upstream_rr_peer_t  *peer, *best;

    /* 获取当前缓存时间 */
    now = ngx_time();

    best = NULL;
    total = 0;

#if (NGX_SUPPRESS_WARN)
    p = 0;
#endif

    for (peer = rrp->peers->peer, i = 0;
         peer;
         peer = peer->next, i++)
    {

        /* 
         * i的值为当前遍历的后端服务器的索引，
         * n的值为具体的某个位图，
         * m的值为当前所遍历的后端服务器在某个位图中对应的位
         */
        n = i / (8 * sizeof(uintptr_t));
        m = (uintptr_t) 1 << i % (8 * sizeof(uintptr_t));

        /* rrp->tried[n] & m为1表明这台服务器已经选择过了，本次跳过 */
        if (rrp->tried[n] & m) {
            continue;
        }

        /* peer->down状态为down的服务器跳过 */
        if (peer->down) {
            continue;
        }

        /*
         * 如果在fail_timeout时间范围内某台服务器的连接失败次数达到了max_fails，
         * 那么这台服务器本轮选择跳过
         */
        if (peer->max_fails
            && peer->fails >= peer->max_fails
            && now - peer->checked <= peer->fail_timeout)
        {
            continue;
        }

        /* 计算当前权重 */
        peer->current_weight += peer->effective_weight;

        /* total为本次可以选择的所有后端服务器的有效权重之和 */
        total += peer->effective_weight;

        /* 更新有效权重，如果有效权重小于配置权重，则递增有效权重 */
        if (peer->effective_weight < peer->weight) {
            peer->effective_weight++;
        }

        /* 更新目前得到的权重最高的后端服务器 */
        if (best == NULL || peer->current_weight > best->current_weight) {
            best = peer;
            p = i;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    /* rrp->current设置为本次选中的后端服务器对象 */
    rrp->current = best;

    /*
     * 经过上面的流程，p的值为本次选中的后端服务器索引，因为本次选中了，所以需要将
     * 该后端服务器对应的位图中的位置置位，n即为本次选中的后端服务器所在位图，m则为
     * 本次选中的后端服务器在位图中对应的位
     */
    n = p / (8 * sizeof(uintptr_t));
    m = (uintptr_t) 1 << p % (8 * sizeof(uintptr_t));

    /* 将本次选中的后端服务器对应位图中的位置位，表明该服务器已经选中过了 */
    rrp->tried[n] |= m;

    /* 
     * 更新本次选中的后端服务器的当前权重，计算方法为当前权重减去本次可以选择的所有
     * 后端服务器权重之和作为本次选中后端服务器的当前权重。
     */
    best->current_weight -= total;

    /*
     *     如果现在距离这台服务器上次被选中但处理失败的时间(这台服务器此前在fail_timeout内处理失败次数大于
     * max_fails次而不参与选举)大于fail_timeout，则说明这台服务器又可以重新参与连接了，本台后端服务器此次
     * 被选中，更新checked。如果本台服务器后续连接处理也成功了，这个时候会在ngx_stream_upstream_free_round_robin_peer
     * 函数中将fails次数清零，激活这台服务器(因为这个时候checked肯定不等于accessed)。如果本台服务器后续连接
     * 处理又失败了，那么accessed和checked时间又会被更新为出异常时间，这样的话本台服务器的fails次数在原来
     * max_fails基础上又被递增了。下一个客户端请求来的时候就可能会因为now - best->checked小于fail_timeout
     * 会不选这台服务器，对于处于这种状态的服务器，只有过了fail_timeout时间被选中并且后续处理也成功，才有
     * 机会被激活(fails清零)，否则每次都只能等fail_timeout过了，才会参与选举，如果选举成功但是后续处理失败，
     * 又只能等fail_timeout过了再参与选举(上面498行的判断由fail_timeout和fails同时制约)，以此类推。
     */
    if (now - best->checked > best->fail_timeout) {
        best->checked = now;
    }

    return best;
}

/*
 * 该函数主要用来设置本次连接选中的后端服务器的一些信息，例如，如果Nginx在向这个后端服务器
 * 发起建链失败或者请求处理失败，则需要记录失败次数，以及重新计算权重等。
 */
void
ngx_http_upstream_free_round_robin_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_rr_peer_data_t  *rrp = data;

    time_t                       now;
    ngx_http_upstream_rr_peer_t  *peer;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "free rr peer %ui %ui", pc->tries, state);

    /* TODO: NGX_PEER_KEEPALIVE */

    /* 获取此次选中的后端服务器，也是本次要释放的后端服务器 */
    peer = rrp->current;

    ngx_http_upstream_rr_peers_rlock(rrp->peers);
    ngx_http_upstream_rr_peer_lock(rrp->peers, peer);

    /* rrp->peers->single为1表示只有一台后端服务器 */
    if (rrp->peers->single) {

        /* 记录此后端服务器选中的次数递减 */
        peer->conns--;

        ngx_http_upstream_rr_peer_unlock(rrp->peers, peer);
        ngx_http_upstream_rr_peers_unlock(rrp->peers);

        pc->tries = 0;  // 将主动连接的重试次数清零
        return;
    }

    /*
     * state & NGX_PEER_FAILED表明此次要释放的后端服务器在向其发送请求的时候失败了，
     * 这个时候需要记录一些关于这台后端服务器的信息，如失败次数，更新有效权重等
     */
    if (state & NGX_PEER_FAILED) {
        now = ngx_time();

        peer->fails++;
        peer->accessed = now;
        peer->checked = now;

        if (peer->max_fails) {
            /* 更新有效权重 */
            peer->effective_weight -= peer->weight / peer->max_fails;

            /*
             * 如果本台服务器的重连次数已经达到了在规定时间内允许失败的最大次数，
             * 那么这台服务器就只能暂时不提供服务了，后续再选择的时候，由于
             * peer->fails和now - peer->checked <= peer->fail_timeout不满足而不被选中，
             * 这个从ngx_http_upstream_get_peer中可以看到。
             * 并且由于peer->fails没有清零，只能等到now - peer->checked > fail_timeout
             * 时，也就是现在距离最近一次被选中但后续处理失败的时间过了fail_timeout这个
             * 时间范围后，才能重新参与选择，如果重新参与选择，被选中的话，checked被更新
             * 并且后续如果处理成功，那么就会走下面那个else分支，由于accessed小于checked，
             * 那么就会将这台服务器的fails清零，激活这台服务器。但是如果重新参与选择，也
             * 被选中，checked被更新，但是后续处理不成功，那么由于fails没有被清零，则又会
             * 进入到这个分支，后续这台服务器又会暂时停止服务，等过了fail_timeout后才能
             * 重新参与选择，以此类推。
             */
            if (peer->fails >= peer->max_fails) {
                ngx_log_error(NGX_LOG_WARN, pc->log, 0,
                              "upstream server temporarily disabled");
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "free rr peer failed: %p %i",
                       peer, peer->effective_weight);

        if (peer->effective_weight < 0) {
            peer->effective_weight = 0;
        }

    } else {

        /* mark peer live if check passed */

        /* 程序执行到这里表明此台服务器是可以成功连接的，因此将其之前累计的连接失败次数清零 */
        /*
         * 从上面那个if分支可以看出，只要后端服务器被选中但是后续处理过程中失败了，那么checked和accessed
         * 的时间总是一样的，那什么时候accessed和checked会不一样呢?我们知道accessed记录的是最近一次检测到
         * 后端服务器异常的时间点，checked什么时候会被单独更新呢?
         * 如果某台服务器(这台服务器此前在fail_timeout内处理失败次数大于max_fails次而不参与选举)过了fail_timeout后
         * 重新参与连接并被被选中，更新checked。如果本台服务器后续连接处理也成功了(会执行到这里)，这个时候会
         * 将fails次数清零，激活这台服务器(因为这个时候checked肯定不等于accessed)。如果本台服务器后续连接
         * 处理又失败了，那么accessed和checked时间又会被更新为出异常时间(上面的if分支)，这样的话本台服务器的
         * fails次数在原来max_fails基础上又被递增了。下一个客户端请求来的时候就可能会因为now - best->checked
         * 小于fail_timeout会不选这台服务器，对于处于这种状态的服务器，只有过了fail_timeout时间被选中并且后续
         * 处理也成功，才有机会被激活(fails清零)，否则每次都只能等fail_timeout过了，才会参与选举，如果选举成功
         * 但是后续处理失败，又只能等fail_timeout过了再参与选举，以此类推。
         */
        if (peer->accessed < peer->checked) {
            peer->fails = 0;
        }
    }

    /* 记录此后端服务器选中的次数递减 */
    peer->conns--;

    ngx_http_upstream_rr_peer_unlock(rrp->peers, peer);
    ngx_http_upstream_rr_peers_unlock(rrp->peers);

    /* 因为已经使用了一次连接，那么需要将可重试次数递减 */
    if (pc->tries) {
        pc->tries--;
    }
}


#if (NGX_HTTP_SSL)

ngx_int_t
ngx_http_upstream_set_round_robin_peer_session(ngx_peer_connection_t *pc,
    void *data)
{
    ngx_http_upstream_rr_peer_data_t  *rrp = data;

    ngx_int_t                      rc;
    ngx_ssl_session_t             *ssl_session;
    ngx_http_upstream_rr_peer_t   *peer;
#if (NGX_HTTP_UPSTREAM_ZONE)
    int                            len;
#if OPENSSL_VERSION_NUMBER >= 0x0090707fL
    const
#endif
    u_char                        *p;
    ngx_http_upstream_rr_peers_t  *peers;
    u_char                         buf[NGX_SSL_MAX_SESSION_SIZE];
#endif

    peer = rrp->current;

#if (NGX_HTTP_UPSTREAM_ZONE)
    peers = rrp->peers;

    if (peers->shpool) {
        ngx_http_upstream_rr_peers_rlock(peers);
        ngx_http_upstream_rr_peer_lock(peers, peer);

        if (peer->ssl_session == NULL) {
            ngx_http_upstream_rr_peer_unlock(peers, peer);
            ngx_http_upstream_rr_peers_unlock(peers);
            return NGX_OK;
        }

        len = peer->ssl_session_len;

        ngx_memcpy(buf, peer->ssl_session, len);

        ngx_http_upstream_rr_peer_unlock(peers, peer);
        ngx_http_upstream_rr_peers_unlock(peers);

        p = buf;
        ssl_session = d2i_SSL_SESSION(NULL, &p, len);

        rc = ngx_ssl_set_session(pc->connection, ssl_session);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "set session: %p", ssl_session);

        ngx_ssl_free_session(ssl_session);

        return rc;
    }
#endif

    ssl_session = peer->ssl_session;

    rc = ngx_ssl_set_session(pc->connection, ssl_session);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "set session: %p", ssl_session);

    return rc;
}


void
ngx_http_upstream_save_round_robin_peer_session(ngx_peer_connection_t *pc,
    void *data)
{
    ngx_http_upstream_rr_peer_data_t  *rrp = data;

    ngx_ssl_session_t             *old_ssl_session, *ssl_session;
    ngx_http_upstream_rr_peer_t   *peer;
#if (NGX_HTTP_UPSTREAM_ZONE)
    int                            len;
    u_char                        *p;
    ngx_http_upstream_rr_peers_t  *peers;
    u_char                         buf[NGX_SSL_MAX_SESSION_SIZE];
#endif

#if (NGX_HTTP_UPSTREAM_ZONE)
    peers = rrp->peers;

    if (peers->shpool) {

        ssl_session = SSL_get0_session(pc->connection->ssl->connection);

        if (ssl_session == NULL) {
            return;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "save session: %p", ssl_session);

        len = i2d_SSL_SESSION(ssl_session, NULL);

        /* do not cache too big session */

        if (len > NGX_SSL_MAX_SESSION_SIZE) {
            return;
        }

        p = buf;
        (void) i2d_SSL_SESSION(ssl_session, &p);

        peer = rrp->current;

        ngx_http_upstream_rr_peers_rlock(peers);
        ngx_http_upstream_rr_peer_lock(peers, peer);

        if (len > peer->ssl_session_len) {
            ngx_shmtx_lock(&peers->shpool->mutex);

            if (peer->ssl_session) {
                ngx_slab_free_locked(peers->shpool, peer->ssl_session);
            }

            peer->ssl_session = ngx_slab_alloc_locked(peers->shpool, len);

            ngx_shmtx_unlock(&peers->shpool->mutex);

            if (peer->ssl_session == NULL) {
                peer->ssl_session_len = 0;

                ngx_http_upstream_rr_peer_unlock(peers, peer);
                ngx_http_upstream_rr_peers_unlock(peers);
                return;
            }

            peer->ssl_session_len = len;
        }

        ngx_memcpy(peer->ssl_session, buf, len);

        ngx_http_upstream_rr_peer_unlock(peers, peer);
        ngx_http_upstream_rr_peers_unlock(peers);

        return;
    }
#endif

    ssl_session = ngx_ssl_get_session(pc->connection);

    if (ssl_session == NULL) {
        return;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "save session: %p", ssl_session);

    peer = rrp->current;

    old_ssl_session = peer->ssl_session;
    peer->ssl_session = ssl_session;

    if (old_ssl_session) {

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "old session: %p", old_ssl_session);

        /* TODO: may block */

        ngx_ssl_free_session(old_ssl_session);
    }
}


static ngx_int_t
ngx_http_upstream_empty_set_session(ngx_peer_connection_t *pc, void *data)
{
    return NGX_OK;
}


static void
ngx_http_upstream_empty_save_session(ngx_peer_connection_t *pc, void *data)
{
    return;
}

#endif
