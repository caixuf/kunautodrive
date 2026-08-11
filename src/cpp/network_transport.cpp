/**
 * network_transport.cpp — 分布式 TCP 传输层 (基于 flowcoro::net)
 */

#include "network_transport.h"

#if defined(_WIN32)

#include <cstring>

struct NetworkTransport {
    NetTransportStats stats;
};

extern "C" {

NetworkTransport* net_transport_create(const char* bind_addr, uint16_t port,
                                       MessageBus* bus, DiscoveryManager* dm) {
    (void)bind_addr; (void)port; (void)bus; (void)dm;
    return new NetworkTransport{};
}

void net_transport_destroy(NetworkTransport* t) { delete t; }
int net_transport_start(NetworkTransport* t) { return t ? 0 : -1; }
void net_transport_stop(NetworkTransport* t) { (void)t; }
int net_transport_bridge_topic(NetworkTransport* t, const char* topic) { (void)t; (void)topic; return 0; }
int net_transport_unbridge_topic(NetworkTransport* t, const char* topic) { (void)t; (void)topic; return 0; }
int net_transport_connect(NetworkTransport* t, const char* host, uint16_t port) { (void)t; (void)host; (void)port; return -1; }
int net_transport_disconnect(NetworkTransport* t, const char* host, uint16_t port) { (void)t; (void)host; (void)port; return -1; }
int net_transport_connection_count(NetworkTransport* t) { (void)t; return 0; }
void net_transport_get_stats(NetworkTransport* t, NetTransportStats* stats) {
    if (stats) {
        if (t) *stats = t->stats;
        else std::memset(stats, 0, sizeof(*stats));
    }
}

} /* extern "C" */

#else

#include <flowcoro/net.h>
#include <flowcoro/thread_pool.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

/* ══════════════════════════════════════════════════════════ */
/* 网络帧协议                                                  */
/* ══════════════════════════════════════════════════════════ */

static std::string serialize_frame(const Message* msg) {
    uint32_t payload = sizeof(Message);
    uint32_t net_len = htonl(payload);
    Message wire;
    message_bus_copy_message(&wire, msg);
    std::string frame;
    frame.append((const char*)&net_len, 4);
    frame.append((const char*)&wire, sizeof(Message));
    return frame;
}

static bool deserialize_frame(const uint8_t* data, size_t len, Message* msg) {
    if (len < sizeof(Message)) return false;
    memcpy(msg, data, sizeof(Message));
    msg->_loaned_data = nullptr;
    msg->_loaned_release = nullptr;
    msg->_loaned_release_ctx = nullptr;
    return true;
}

/* ══════════════════════════════════════════════════════════ */
/* Bridge subscription (needs to be outside NetworkTransport)  */
/* ══════════════════════════════════════════════════════════ */

struct NetworkTransport;  /* fwd */

struct BridgeSub {
    NetworkTransport* t;
    std::string       topic;
};

static constexpr const char* NET_FORWARD_PREFIX = "@net:";

/* ══════════════════════════════════════════════════════════ */
/* 连接状态                                                    */
/* ══════════════════════════════════════════════════════════ */

struct PeerConn {
    std::string            host;
    uint16_t               port;
    int                    fd;          // -1 = disconnected
    std::atomic<bool>      active;
    std::mutex             write_mutex; // protect concurrent writes
    std::vector<uint8_t>   rx_buffer;
};

struct NetworkTransport {
    std::string             bind_addr;
    uint16_t                port;
    MessageBus*             bus;
    DiscoveryManager*       discovery;

    int                     listen_fd;
    std::atomic<bool>       running;

    std::vector<PeerConn*>  peers;
    std::mutex              peers_mutex;

    std::vector<std::string> bridged_topics;
    std::mutex              bridge_mutex;

    /* Background threads */
    std::thread             accept_thread;
    std::thread             recv_thread;

    NetTransportStats       stats;

    /* Bridge subscriptions */
    std::vector<BridgeSub*> subs;
};

/* ══════════════════════════════════════════════════════════ */
/* 出站桥接回调                                                */
/* ══════════════════════════════════════════════════════════ */

static void bridge_outbound_cb(const Message* msg, void* user_data) {
    auto* sub = (BridgeSub*)user_data;
    auto* t   = sub->t;
    if (!msg || strncmp(msg->sender, NET_FORWARD_PREFIX,
                        strlen(NET_FORWARD_PREFIX)) == 0) {
        return;
    }

    std::string frame = serialize_frame(msg);

    std::lock_guard<std::mutex> lock(t->peers_mutex);
    for (auto* peer : t->peers) {
        if (!peer->active || peer->fd < 0) continue;

        std::lock_guard<std::mutex> wlock(peer->write_mutex);
        size_t sent = 0;
        while (sent < frame.size()) {
            ssize_t n = send(peer->fd, frame.data() + sent,
                             frame.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd pfd = { peer->fd, POLLOUT, 0 };
                if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLOUT))
                    continue;
            }
            t->stats.send_errors++;
            peer->active = false;
            close(peer->fd);
            peer->fd = -1;
            break;
        }
        if (sent == frame.size()) {
            t->stats.bytes_sent += (uint64_t)sent;
            t->stats.msgs_sent++;
        }
    }
}

/* ══════════════════════════════════════════════════════════ */
/* 入站接收线程                                                */
/* ══════════════════════════════════════════════════════════ */

static void recv_thread_fn(NetworkTransport* t) {
    uint8_t buf[8192];

    while (t->running) {
        /* Messages received this cycle, published AFTER releasing peers_mutex.
         * Publishing under the lock is unsafe: message_bus delivery can re-enter
         * the transport (e.g. bridge_outbound_cb also locks peers_mutex), which
         * would self-deadlock and permanently starve every other peers_mutex
         * user (accept, outbound bridge, and connection_count used by the
         * monitor/state-file writer). */
        std::vector<Message> inbound;

        {
            /* Poll all peer connections — lock held only for the poll loop,
             * never across usleep() or bus publish. */
            std::lock_guard<std::mutex> lock(t->peers_mutex);
            for (auto* peer : t->peers) {
                if (!peer->active || peer->fd < 0) continue;

                ssize_t n = recv(peer->fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n == 0) {
                    peer->active = false;
                    close(peer->fd);
                    peer->fd = -1;
                    t->stats.disconnects++;
                    printf("[net_transport] peer %s:%u disconnected\n",
                           peer->host.c_str(), peer->port);
                    continue;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    peer->active = false;
                    close(peer->fd);
                    peer->fd = -1;
                    t->stats.disconnects++;
                    continue;
                }

                peer->rx_buffer.insert(peer->rx_buffer.end(), buf, buf + n);
                while (peer->rx_buffer.size() >= 4) {
                    uint32_t net_len = 0;
                    memcpy(&net_len, peer->rx_buffer.data(), 4);
                    uint32_t payload = ntohl(net_len);
                    if (payload != sizeof(Message)) {
                        peer->active = false;
                        close(peer->fd);
                        peer->fd = -1;
                        t->stats.disconnects++;
                        break;
                    }
                    size_t frame_size = 4u + (size_t)payload;
                    if (peer->rx_buffer.size() < frame_size) break;

                    t->stats.bytes_received += frame_size;
                    t->stats.msgs_received++;
                    Message msg;
                    if (deserialize_frame(peer->rx_buffer.data() + 4,
                                           payload, &msg)) {
                        inbound.push_back(msg);
                    }
                    peer->rx_buffer.erase(peer->rx_buffer.begin(),
                                          peer->rx_buffer.begin() + frame_size);
                }
            }
        }

        /* Publish outside the lock. */
        for (const auto& msg : inbound) {
            char sender[MSG_BUS_MAX_SENDER_LEN];
            snprintf(sender, sizeof(sender), "%s%s",
                     NET_FORWARD_PREFIX, msg.sender);
            message_bus_publish(t->bus, msg.topic, sender,
                                msg.data, msg.data_size);
        }

        usleep(10000); /* 10ms poll interval */
    }
}

/* ══════════════════════════════════════════════════════════ */
/* Accept 线程                                                 */
/* ══════════════════════════════════════════════════════════ */

static void accept_thread_fn(NetworkTransport* t) {
    while (t->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(t->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);
                continue;
            }
            break;
        }

        /* Set TCP_NODELAY for low-latency messaging */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        fcntl(client_fd, F_SETFL, O_NONBLOCK);

        /* Create peer entry for this connection */
        auto* peer = new PeerConn();
        peer->host   = inet_ntoa(client_addr.sin_addr);
        peer->port   = ntohs(client_addr.sin_port);
        peer->fd     = client_fd;
        peer->active = true;

        {
            std::lock_guard<std::mutex> lock(t->peers_mutex);
            t->peers.push_back(peer);
        }
        t->stats.connects++;

        printf("[net_transport] accepted connection from %s:%u (total=%zu)\n",
               peer->host.c_str(), peer->port, t->peers.size());
    }
}

/* ══════════════════════════════════════════════════════════ */
/* Discovery 集成                                              */
/* ══════════════════════════════════════════════════════════ */

static void discovery_cb(const TopologyGraph* graph, const NodeInfo* node,
                         bool joined, void* user_data) {
    (void)graph;
    auto* t = (NetworkTransport*)user_data;
    if (!joined || !node->alive || node->ipv4_address == 0) return;

    struct in_addr a;
    a.s_addr = node->ipv4_address;
    const char* host = inet_ntoa(a);
    if (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "0.0.0.0") == 0) return;

    uint16_t port = node->unicast_port ? node->unicast_port : NET_DEFAULT_PORT;
    printf("[net_transport] discovered %s → auto-connect %s:%u\n",
           node->name, host, port);
    net_transport_connect(t, host, port);
}

/* ══════════════════════════════════════════════════════════ */
/* 公开 API                                                    */
/* ══════════════════════════════════════════════════════════ */

extern "C" {

NetworkTransport* net_transport_create(const char* bind_addr, uint16_t port,
                                       MessageBus* bus, DiscoveryManager* dm) {
    auto* t = new NetworkTransport{}; /* value-init: POD/atomic 置零，非平凡成员默认构造 */
    t->bind_addr  = bind_addr ? bind_addr : "0.0.0.0";
    t->port       = port ? port : NET_DEFAULT_PORT;
    t->bus        = bus;
    t->discovery  = dm;
    t->listen_fd  = -1;

    if (dm) {
        discovery_set_unicast_port(dm, t->port);
        discovery_set_change_callback(dm, discovery_cb, t);
    }
    return t;
}

void net_transport_destroy(NetworkTransport* t) {
    if (!t) return;
    if (t->running) net_transport_stop(t);
    for (auto* sub : t->subs) {
        message_bus_unsubscribe_ex(t->bus, sub->topic.c_str(),
                                   bridge_outbound_cb, sub);
        delete sub;
    }
    for (auto* p : t->peers) {
        if (p->fd >= 0) close(p->fd);
        delete p;
    }
    delete t;
}

int net_transport_start(NetworkTransport* t) {
    if (!t || t->running) return -1;

    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0) return -1;

    int reuse = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(t->listen_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(t->bind_addr.c_str());
    addr.sin_port        = htons(t->port);

    if (bind(t->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(t->listen_fd);
        return -1;
    }
    listen(t->listen_fd, 32);

    t->running = true;
    t->accept_thread = std::thread(accept_thread_fn, t);
    t->recv_thread   = std::thread(recv_thread_fn, t);

    printf("[net_transport] listening on %s:%u\n", t->bind_addr.c_str(), t->port);
    return 0;
}

void net_transport_stop(NetworkTransport* t) {
    if (!t || !t->running) return;
    t->running = false;
    if (t->listen_fd >= 0) { close(t->listen_fd); t->listen_fd = -1; }
    if (t->accept_thread.joinable()) t->accept_thread.join();
    if (t->recv_thread.joinable())   t->recv_thread.join();
    printf("[net_transport] stopped\n");
}

int net_transport_bridge_topic(NetworkTransport* t, const char* topic) {
    if (!t || !topic) return -1;
    {
        std::lock_guard<std::mutex> lock(t->bridge_mutex);
        for (const auto& bridged : t->bridged_topics) {
            if (bridged == topic) return 0;
        }
    }
    auto* sub = new BridgeSub();
    sub->t = t; sub->topic = topic;
    if (message_bus_subscribe(t->bus, topic, bridge_outbound_cb, sub) != 0) {
        delete sub;
        return -1;
    }
    std::lock_guard<std::mutex> lock(t->bridge_mutex);
    t->subs.push_back(sub);
    t->bridged_topics.emplace_back(topic);
    return 0;
}

int net_transport_unbridge_topic(NetworkTransport* t, const char* topic) {
    if (!t || !topic) return -1;
    std::lock_guard<std::mutex> lock(t->bridge_mutex);
    for (auto it = t->subs.begin(); it != t->subs.end(); ++it) {
        BridgeSub* sub = *it;
        if (sub->topic == topic) {
            message_bus_unsubscribe_ex(t->bus, topic, bridge_outbound_cb, sub);
            t->subs.erase(it);
            for (auto topic_it = t->bridged_topics.begin();
                 topic_it != t->bridged_topics.end(); ++topic_it) {
                if (*topic_it == topic) {
                    t->bridged_topics.erase(topic_it);
                    break;
                }
            }
            delete sub;
            return 0;
        }
    }
    return -1;
}

int net_transport_connect(NetworkTransport* t, const char* host, uint16_t port) {
    if (!t || !host) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host);
    addr.sin_port = htons(port);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    fcntl(fd, F_SETFL, O_NONBLOCK);

    auto* peer = new PeerConn();
    peer->host   = host;
    peer->port   = port;
    peer->fd     = fd;
    peer->active = true;

    {
        std::lock_guard<std::mutex> lock(t->peers_mutex);
        t->peers.push_back(peer);
    }
    t->stats.connects++;
    return 0;
}

int net_transport_disconnect(NetworkTransport* t, const char* host, uint16_t port) {
    if (!t) return -1;
    std::lock_guard<std::mutex> lock(t->peers_mutex);
    for (auto* p : t->peers) {
        if (p->host == host && p->port == port) {
            p->active = false;
            if (p->fd >= 0) { close(p->fd); p->fd = -1; }
            t->stats.disconnects++;
            return 0;
        }
    }
    return -1;
}

int net_transport_connection_count(NetworkTransport* t) {
    if (!t) return 0;
    int n = 0;
    std::lock_guard<std::mutex> lock(t->peers_mutex);
    for (auto* p : t->peers) if (p->active) n++;
    return n;
}

void net_transport_get_stats(NetworkTransport* t, NetTransportStats* stats) {
    if (t && stats) *stats = t->stats;
}

} /* extern "C" */

#endif /* _WIN32 */
