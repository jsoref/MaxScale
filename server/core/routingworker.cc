/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-12-27
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxscale/routingworker.hh>

#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <vector>
#include <sstream>
#include <sys/epoll.h>
#include <maxbase/atomic.hh>
#include <maxbase/average.hh>
#include <maxbase/semaphore.hh>
#include <maxscale/cn_strings.hh>
#include <maxscale/config.hh>
#include <maxscale/clock.hh>
#include <maxscale/mainworker.hh>
#include <maxscale/json_api.hh>
#include <maxscale/utils.hh>
#include <maxscale/statistics.hh>

#include "internal/listener.hh"
#include "internal/modules.hh"
#include "internal/server.hh"
#include "internal/session.hh"

#define WORKER_ABSENT_ID (-1)

using maxbase::AverageN;
using maxbase::Semaphore;
using maxbase::Worker;
using maxbase::WorkerLoad;
using maxscale::RoutingWorker;
using maxscale::Closer;
using std::vector;
using std::stringstream;

namespace
{

/**
 * Unit variables.
 */
struct this_unit
{
    bool            initialized;        // Whether the initialization has been performed.
    int             nWorkers;           // How many routing workers there are.
    RoutingWorker** ppWorkers;          // Array of routing worker instances.
    mxb::AverageN** ppWorker_loads;     // Array of load averages for workers.
    int             epoll_listener_fd;  // Shared epoll descriptor for listening descriptors.
    int             id_min_worker;      // The smallest routing worker id.
    int             id_max_worker;      // The largest routing worker id.
    bool            running;            // True if worker threads are running
} this_unit =
{
    false,              // initialized
    0,                  // nWorkers
    nullptr,            // ppWorkers
    nullptr,            // ppWorker_loads
    -1,                 // epoll_listener_fd
    WORKER_ABSENT_ID,   // id_min_worker
    WORKER_ABSENT_ID,   // id_max_worker
    false,
};

thread_local struct this_thread
{
    int current_worker_id;      // The worker id of the current thread
} this_thread =
{
    WORKER_ABSENT_ID
};

bool can_close_dcb(mxs::BackendConnection* b)
{
    mxb_assert(b->dcb()->role() == DCB::Role::BACKEND);
    const int SHOW_SHUTDOWN_TIMEOUT = 2;
    auto idle = MXS_CLOCK_TO_SEC(mxs_clock() - b->dcb()->last_read());
    return idle > SHOW_SHUTDOWN_TIMEOUT || b->can_close();
}
}

namespace maxscale
{

json_t* RoutingWorker::MemoryUsage::to_json() const
{
    json_t* pMu = json_object();

    json_object_set_new(pMu, "query_classifier", json_integer(this->query_classifier));
    json_object_set_new(pMu, "zombies", json_integer(this->zombies));
    json_object_set_new(pMu, "sessions", json_integer(this->sessions));
    json_object_set_new(pMu, "total", json_integer(this->total));

    return pMu;
}

RoutingWorker::ConnPoolEntry::ConnPoolEntry(mxs::BackendConnection* pConn)
    : m_created(time(nullptr))
    , m_pConn(pConn)
{
    mxb_assert(m_pConn);
}

RoutingWorker::ConnPoolEntry::~ConnPoolEntry()
{
    mxb_assert(!m_pConn);
}

RoutingWorker::DCBHandler::DCBHandler(RoutingWorker* pOwner)
    : m_owner(*pOwner)
{
}

// Any activity on a backend DCB that is in the persistent pool, will
// cause the dcb to be evicted.
void RoutingWorker::DCBHandler::ready_for_reading(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::write_ready(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::error(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}

void RoutingWorker::DCBHandler::hangup(DCB* pDcb)
{
    m_owner.evict_dcb(static_cast<BackendDCB*>(pDcb));
}


RoutingWorker::RoutingWorker(mxb::WatchdogNotifier* pNotifier)
    : mxb::WatchedWorker(pNotifier)
    , m_callable(this)
    , m_pool_handler(this)
{
}

RoutingWorker::~RoutingWorker()
{
    remove_pollable(this);
    m_callable.cancel_dcalls();
}

// static
bool RoutingWorker::init(mxb::WatchdogNotifier* pNotifier)
{
    mxb_assert(!this_unit.initialized);

    this_unit.epoll_listener_fd = epoll_create(MAX_EVENTS);

    if (this_unit.epoll_listener_fd != -1)
    {
        int nWorkers = config_threadcount();
        auto max_count = Config::ParamThreadsCount::MAX_COUNT;
        // 0-inited arrays.
        RoutingWorker** ppWorkers = new(std::nothrow) RoutingWorker* [max_count]();
        AverageN** ppWorker_loads = new(std::nothrow) AverageN* [max_count];

        if (ppWorkers && ppWorker_loads)
        {
            int id_min_worker = -1;
            int id_max_worker = 0;

            size_t rebalance_window = mxs::Config::get().rebalance_window.get();

            MXB_AT_DEBUG(int id_prev = -1);
            int i;
            for (i = 0; i < nWorkers; ++i)
            {
                RoutingWorker* pWorker = RoutingWorker::create(pNotifier, this_unit.epoll_listener_fd);
                AverageN* pAverage = new AverageN(rebalance_window);

                if (pWorker && pAverage)
                {
                    int id = pWorker->id();

                    // We require the routing worker ids to be consecutive.
                    mxb_assert(id_prev == -1 || (id_prev + 1 == id));
                    MXB_AT_DEBUG(id_prev = id);

                    if (id_min_worker == -1)
                    {
                        id_min_worker = id;
                    }

                    if (id > id_max_worker)
                    {
                        id_max_worker = id;
                    }

                    ppWorkers[i] = pWorker;
                    ppWorker_loads[i] = pAverage;
                }
                else
                {
                    for (int j = i - 1; j >= 0; --j)
                    {
                        delete ppWorker_loads[j];
                        delete ppWorkers[j];
                    }

                    delete[] ppWorkers;
                    ppWorkers = NULL;
                    break;
                }
            }

            if (ppWorkers && ppWorker_loads)
            {
                this_unit.ppWorkers = ppWorkers;
                this_unit.ppWorker_loads = ppWorker_loads;
                this_unit.nWorkers = nWorkers;
                this_unit.id_min_worker = id_min_worker;
                this_unit.id_max_worker = id_max_worker;

                this_unit.initialized = true;
            }
        }
        else
        {
            MXB_OOM();
            close(this_unit.epoll_listener_fd);
        }
    }
    else
    {
        MXB_ALERT("Could not allocate an epoll instance.");
    }

    return this_unit.initialized;
}

void RoutingWorker::finish()
{
    mxb_assert(this_unit.initialized);

    for (int i = this_unit.nWorkers - 1; i >= 0; --i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        delete pWorker;
        this_unit.ppWorkers[i] = NULL;

        mxb::Average* pWorker_load = this_unit.ppWorker_loads[i];
        delete pWorker_load;
    }

    delete[] this_unit.ppWorkers;
    this_unit.ppWorkers = NULL;

    close(this_unit.epoll_listener_fd);
    this_unit.epoll_listener_fd = 0;

    this_unit.initialized = false;
}

// static
bool RoutingWorker::add_listener(Listener* pListener)
{
    bool rv = true;

    int fd = pListener->poll_fd();

    // This must be level-triggered (i.e. the default). Since this is
    // intended for listening sockets and each worker will call accept()
    // just once before going back the epoll_wait(), using EPOLLET would
    // mean that if there are more clients to be accepted than there are
    // threads returning from epoll_wait() for an event, then some clients
    // would be accepted only when a new client has connected, thus causing
    // a new EPOLLIN event.
    uint32_t events = EPOLLIN;
    Pollable* pPollable = pListener;

    struct epoll_event ev;

    ev.events = events;
    ev.data.ptr = pPollable;

    if (epoll_ctl(this_unit.epoll_listener_fd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
        Worker::resolve_poll_error(fd, errno, EPOLL_CTL_ADD);
        rv = false;
    }

    return rv;
}

// static
bool RoutingWorker::remove_listener(Listener* pListener)
{
    bool rv = true;

    int fd = pListener->poll_fd();

    struct epoll_event ev = {};

    if (epoll_ctl(this_unit.epoll_listener_fd, EPOLL_CTL_DEL, fd, &ev) != 0)
    {
        Worker::resolve_poll_error(fd, errno, EPOLL_CTL_DEL);
        rv = false;
    }

    return rv;
}

RoutingWorker* RoutingWorker::get(int worker_id)
{
    mxb_assert(this_unit.initialized);

    if (worker_id == FIRST)
    {
        worker_id = this_unit.id_min_worker;
    }

    bool valid = (worker_id >= this_unit.id_min_worker && worker_id <= this_unit.id_max_worker);

    return valid ? this_unit.ppWorkers[worker_id  - this_unit.id_min_worker] : nullptr;
}

RoutingWorker* RoutingWorker::get_current()
{
    RoutingWorker* pWorker = NULL;

    int worker_id = get_current_id();

    if (worker_id != WORKER_ABSENT_ID)
    {
        pWorker = RoutingWorker::get(worker_id);
    }

    return pWorker;
}

int RoutingWorker::get_current_id()
{
    return this_thread.current_worker_id;
}

int RoutingWorker::index() const
{
    return id() - this_unit.id_min_worker;
}

// static
RoutingWorker* RoutingWorker::get_by_index(int index)
{
    mxb_assert(index >= 0);
    mxb_assert(index < this_unit.nWorkers);

    return this_unit.ppWorkers[index];
}

// static
bool RoutingWorker::start_workers()
{
    bool rv = true;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (!pWorker->start(MAKE_STR("Worker-" << std::setw(2) << std::setfill('0') << i)))
        {
            MXB_ALERT("Could not start routing worker %d of %d.", i, this_unit.nWorkers);
            rv = false;
            // At startup, so we don't even try to clean up.
            break;
        }
    }

    if (rv)
    {
        this_unit.running = true;
    }

    return rv;
}

// static
bool RoutingWorker::is_running()
{
    return this_unit.running;
}

// static
void RoutingWorker::join_workers()
{
    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        pWorker->join();
    }

    this_unit.running = false;
}

// static
bool RoutingWorker::shutdown_complete()
{
    bool rval = true;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->state() != Worker::FINISHED && pWorker->state() != Worker::STOPPED)
        {
            rval = false;
        }
    }

    return rval;
}

RoutingWorker::SessionsById& RoutingWorker::session_registry()
{
    return m_sessions;
}

const RoutingWorker::SessionsById& RoutingWorker::session_registry() const
{
    return m_sessions;
}

void RoutingWorker::destroy(DCB* pDcb)
{
    mxb_assert(pDcb->owner() == this);

    m_zombies.push_back(pDcb);
}

/**
 * If a second has passed since last keepalive tick, tick all sessions again.
 * Also checks connection pool for expired connections.
 */
void RoutingWorker::process_timeouts()
{
    auto now = mxs_clock();
    if (now >= m_next_timeout_check)
    {
        /** Because the resolutions of the timeouts is one second, we only need to
         * check them once per second. One heartbeat is 100 milliseconds. */
        m_next_timeout_check = now + 10;

        for (auto& elem : m_sessions)
        {
            auto* pSes = static_cast<Session*>(elem.second);
            ClientDCB* pClient = pSes->client_dcb;
            if (pClient->state() == DCB::State::POLLING)
            {
                auto idle = now - std::max(pClient->last_read(), pClient->last_write());
                pSes->tick(MXS_CLOCK_TO_SEC(idle));
            }
        }
    }
}

void RoutingWorker::delete_zombies()
{
    Zombies slow_zombies;
    // An algorithm cannot be used, as the final closing of a DCB may cause
    // other DCBs to be registered in the zombie queue.

    while (!m_zombies.empty())
    {
        DCB* pDcb = m_zombies.back();
        m_zombies.pop_back();
        MXS_SESSION::Scope scope(pDcb->session());

        bool can_close = true;

        if (pDcb->role() == DCB::Role::CLIENT)
        {
            // Check if any of the backend DCBs isn't ready to be closed. If so, delay the closing of the
            // client DCB until the backend connections have fully established and finished authenticating.
            const auto& dcbs = static_cast<Session*>(pDcb->session())->backend_connections();
            can_close = std::all_of(dcbs.begin(), dcbs.end(), can_close_dcb);
        }

        if (can_close)
        {
            MXB_DEBUG("Ready to close session %lu", pDcb->session() ? pDcb->session()->id() : 0);
            DCB::Manager::call_destroy(pDcb);
        }
        else
        {
            MXB_DEBUG("Delaying destruction of session %lu", pDcb->session() ? pDcb->session()->id() : 0);
            slow_zombies.push_back(pDcb);
        }
    }

    mxb_assert(m_zombies.empty());
    m_zombies.insert(m_zombies.end(), slow_zombies.begin(), slow_zombies.end());
}

void RoutingWorker::add(DCB* pDcb)
{
    MXB_AT_DEBUG(auto rv = ) m_dcbs.insert(pDcb);
    mxb_assert(rv.second);
}

void RoutingWorker::remove(DCB* pDcb)
{
    auto it = m_dcbs.find(pDcb);
    mxb_assert(it != m_dcbs.end());
    m_dcbs.erase(it);
}

RoutingWorker::ConnectionResult
RoutingWorker::get_backend_connection(SERVER* pSrv, MXS_SESSION* pSes, mxs::Component* pUpstream)
{
    auto* pServer = static_cast<Server*>(pSrv);
    auto* pSession = static_cast<Session*>(pSes);

    if (pServer->persistent_conns_enabled() && pServer->is_running())
    {
        auto pool_conn = pool_get_connection(pSrv, pSession, pUpstream);
        if (pool_conn)
        {
            // Connection found from pool, return it.
            return {false, pool_conn};
        }
    }

    ConnectionResult rval;
    const auto max_allowed_conns = pServer->max_routing_connections();
    auto& stats = pServer->stats();

    if (max_allowed_conns > 0)
    {
        // Server has a connection count limit. Check that we are not already at the limit.
        auto curr_conns = stats.n_current_conns() + stats.n_conn_intents();
        if (curr_conns >= max_allowed_conns)
        {
            // Looks like all connection slots are in use. This may be pessimistic in case an intended
            // connection fails in another thread.
            rval.conn_limit_reached = true;
        }
        else
        {
            // Mark intent, then read current conn value again. This is not entirely accurate, but does
            // avoid overshoot (assuming memory orderings are correct).
            auto intents = stats.add_conn_intent();
            if (intents + stats.n_current_conns() <= max_allowed_conns)
            {
                auto new_conn = pSession->create_backend_connection(pServer, this, pUpstream);
                if (new_conn)
                {
                    stats.add_connection();
                    rval.conn = new_conn;
                }
            }
            else
            {
                rval.conn_limit_reached = true;
            }
            stats.remove_conn_intent();
        }
    }
    else
    {
        // No limit, just create new connection.
        auto new_conn = pSession->create_backend_connection(pServer, this, pUpstream);
        if (new_conn)
        {
            stats.add_connection();
            rval.conn = new_conn;
        }
    }

    return rval;
}

std::pair<uint64_t, mxs::BackendConnection*>
RoutingWorker::ConnectionPool::get_connection(MXS_SESSION* session)
{
    mxs::BackendConnection* rval = nullptr;
    uint64_t best_reuse = mxs::BackendConnection::REUSE_NOT_POSSIBLE;
    auto best = m_contents.end();

    for (auto it = m_contents.begin(); it != m_contents.end(); ++it)
    {
        auto current_reuse = it->first->can_reuse(session);

        if (current_reuse > best_reuse)
        {
            best = it;
            best_reuse = current_reuse;

            if (current_reuse == mxs::BackendConnection::OPTIMAL_REUSE)
            {
                break;
            }
        }
    }

    if (best != m_contents.end())
    {
        rval = best->second.release_conn();
        m_contents.erase(best);
        m_stats.times_found++;
    }
    else
    {
        m_stats.times_empty++;
    }

    return {best_reuse, rval};
}

void RoutingWorker::ConnectionPool::set_capacity(int global_capacity)
{
    // Capacity has changed, recalculate local capacity.
    m_capacity = global_capacity / this_unit.nWorkers;
}

mxs::BackendConnection*
RoutingWorker::pool_get_connection(SERVER* pSrv, MXS_SESSION* pSes, mxs::Component* pUpstream)
{
    std::lock_guard<std::mutex> guard(m_pool_lock);

    auto pServer = static_cast<Server*>(pSrv);
    mxb_assert(pServer);
    auto pSession = static_cast<Session*>(pSes);
    bool proxy_protocol = pServer->proxy_protocol();
    mxs::BackendConnection* found_conn = nullptr;

    auto it = m_pool_group.find(pServer);
    if (it != m_pool_group.end())
    {
        ConnectionPool& conn_pool = it->second;

        while (!found_conn)
        {
            auto [reuse, candidate] = conn_pool.get_connection(pSession);

            // If no candidate could be found, stop right away.
            if (!candidate)
            {
                break;
            }

            BackendDCB* pDcb = candidate->dcb();
            mxb_assert(candidate == pDcb->protocol());
            // Put back the original handler.
            pDcb->set_handler(candidate);
            pSession->link_backend_connection(candidate);

            if (candidate->reuse(pSes, pUpstream, reuse))
            {
                found_conn = candidate;
            }
            else
            {
                // Reusing the current candidate failed. Close connection, then try with another candidate.
                pSession->unlink_backend_connection(candidate);
                MXB_WARNING("Failed to reuse a persistent connection.");
                if (pDcb->state() == DCB::State::POLLING)
                {
                    pDcb->disable_events();
                    pDcb->shutdown();
                }

                BackendDCB::close(pDcb);
                pServer->stats().remove_connection();
                notify_connection_available(pServer);
            }
        }

        if (found_conn)
        {
            // Put the dcb back to the regular book-keeping.
            mxb_assert(m_dcbs.find(found_conn->dcb()) == m_dcbs.end());
            m_dcbs.insert(found_conn->dcb());
        }
    }
    // else: the server does not have an entry in the pool group.

    return found_conn;
}

bool RoutingWorker::move_to_conn_pool(BackendDCB* pDcb)
{
    std::lock_guard<std::mutex> guard(m_pool_lock);

    bool moved_to_pool = false;
    auto* pServer = static_cast<Server*>(pDcb->server());
    long global_pool_cap = pServer->persistpoolmax();
    // For pooling to be possible, several conditions must be met. Check the most obvious ones first.
    if (global_pool_cap > 0)
    {
        auto* pSession = pDcb->session();
        auto* pConn = pDcb->protocol();
        // Pooling enabled for the server. Check connection, session and server status.
        if (pDcb->state() == DCB::State::POLLING && !pDcb->hanged_up() && pConn->established()
            && pSession && pSession->can_pool_backends()
            && pServer->is_running())
        {
            // All ok. Try to add the connection to pool.
            auto pool_iter = m_pool_group.find(pServer);

            if (pool_iter == m_pool_group.end())
            {
                // First pooled connection for the server.
                ConnectionPool new_pool(this, pServer, global_pool_cap);
                pool_iter = m_pool_group.emplace(pServer, std::move(new_pool)).first;
            }

            auto& pool = pool_iter->second;
            if (pool.has_space())
            {
                pool.add_connection(pConn);
                moved_to_pool = true;
            }

            if (moved_to_pool)
            {
                pConn->set_to_pooled();
                pDcb->clear();
                // Change the handler to one that will close the DCB in case there
                // is any activity on it.
                pDcb->set_handler(&m_pool_handler);

                // Remove the dcb from the regular book-keeping.
                auto it = m_dcbs.find(pDcb);
                mxb_assert(it != m_dcbs.end());
                m_dcbs.erase(it);
            }
        }
    }
    return moved_to_pool;
}

void RoutingWorker::pool_close_all_conns()
{
    for (auto& kv : m_pool_group)
    {
        kv.second.close_all();
    }
    m_pool_group.clear();
}


void RoutingWorker::pool_close_all_conns_by_server(SERVER* pSrv)
{
    std::lock_guard<std::mutex> guard(m_pool_lock);
    auto it = m_pool_group.find(pSrv);
    if (it != m_pool_group.end())
    {
        it->second.close_all();
        m_pool_group.erase(it);
    }
}

void RoutingWorker::ConnectionPool::close_expired()
{
    auto* pServer = static_cast<Server*>(m_target_server);
    auto max_age = pServer->persistmaxtime();

    time_t now = time(nullptr);
    vector<mxs::BackendConnection*> expired_conns;

    // First go through the list and gather the expired connections.
    auto it = m_contents.begin();
    while (it != m_contents.end())
    {
        ConnPoolEntry& entry = it->second;
        if (entry.hanged_up() || (now - entry.created() > max_age))
        {
            expired_conns.push_back(entry.release_conn());
            it = m_contents.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Check that pool is not over capacity. This can only happen if user reduces capacity via a runtime
    // config modification.
    int64_t over_cap_conns = m_contents.size() - m_capacity;
    if (over_cap_conns > 0)
    {
        // Just take the first extra connections found.
        int conns_removed = 0;
        auto remover_it = m_contents.begin();
        while (conns_removed < over_cap_conns)
        {
            expired_conns.push_back(remover_it->second.release_conn());
            remover_it = m_contents.erase(remover_it);
            conns_removed++;
        }
    }

    for (auto* pConn : expired_conns)
    {
        m_owner->close_pooled_dcb(pConn->dcb());
    }
}

void RoutingWorker::ConnectionPool::remove_and_close(mxs::BackendConnection* conn)
{
    auto it = m_contents.find(conn);
    mxb_assert(it != m_contents.end());
    it->second.release_conn();
    m_contents.erase(it);
    m_owner->close_pooled_dcb(conn->dcb());
}

void RoutingWorker::ConnectionPool::close_all()
{
    // Close all entries in the server-specific pool.
    for (auto& pool_entry : m_contents)
    {
        BackendDCB* dcb = pool_entry.second.release_conn()->dcb();
        m_owner->close_pooled_dcb(dcb);
    }
    m_contents.clear();
}

bool RoutingWorker::ConnectionPool::empty() const
{
    return m_contents.empty();
}

void RoutingWorker::ConnectionPool::add_connection(mxs::BackendConnection* conn)
{
    m_contents.emplace(conn, ConnPoolEntry(conn));
    m_stats.max_size = std::max(m_stats.max_size, m_contents.size());
}

RoutingWorker::ConnectionPool::ConnectionPool(mxs::RoutingWorker* owner, SERVER* target_server,
                                              int global_capacity)
    : m_owner(owner)
    , m_target_server(target_server)
{
    set_capacity(global_capacity);
}

RoutingWorker::ConnectionPool::ConnectionPool(RoutingWorker::ConnectionPool&& rhs)
    : m_contents(std::move(rhs.m_contents))
    , m_owner(rhs.m_owner)
    , m_target_server(rhs.m_target_server)
    , m_capacity(rhs.m_capacity)
    , m_stats(rhs.m_stats)
{
}

bool RoutingWorker::ConnectionPool::has_space() const
{
    return (int)m_contents.size() < m_capacity;
}

RoutingWorker::ConnectionPoolStats RoutingWorker::ConnectionPool::stats() const
{
    m_stats.curr_size = m_contents.size();
    return m_stats;
}

void RoutingWorker::evict_dcb(BackendDCB* pDcb)
{
    std::lock_guard<std::mutex> guard(m_pool_lock);

    auto it = m_pool_group.find(pDcb->server());
    mxb_assert(it != m_pool_group.end());
    ConnectionPool& conn_pool = it->second;
    conn_pool.remove_and_close(pDcb->protocol());
}

void RoutingWorker::close_pooled_dcb(BackendDCB* pDcb)
{
    // Put the DCB back into the regular book-keeping.
    mxb_assert(m_dcbs.find(pDcb) == m_dcbs.end());
    m_dcbs.insert(pDcb);

    if (pDcb->state() == DCB::State::POLLING)
    {
        pDcb->disable_events();
        pDcb->shutdown();
    }

    auto* srv = pDcb->server();
    BackendDCB::close(pDcb);
    srv->stats().remove_connection();
    notify_connection_available(srv);
}

bool RoutingWorker::pre_run()
{
    this_thread.current_worker_id = id();

    bool rv = modules_thread_init() && qc_thread_init(QC_INIT_SELF);

    if (rv)
    {
        // Every second, check connection pool for expired connections. Ideally, every pooled
        // connection would set their own timer.
        auto check_pool_cb = [this](Callable::Action action){
                if (action == Callable::EXECUTE)
                {
                    pool_close_expired();
                }
                return true;
            };
        m_callable.dcall(1s, check_pool_cb);

        // The normal connection availability notification is not fool-proof, as it's only sent to the
        // current worker. Every now and then, each worker should check for connections regardless since
        // some may be available.
        auto activate_eps_cb = [this](Callable::Action action) {
                if (action == Callable::Action::EXECUTE)
                {
                    activate_waiting_endpoints();
                }
                return true;
            };
        m_callable.dcall(5s, activate_eps_cb);

        auto timeout_eps_cb = [this](Callable::Action action) {
                if (action == Callable::Action::EXECUTE)
                {
                    fail_timed_out_endpoints();
                }
                return true;
            };
        m_callable.dcall(10s, timeout_eps_cb);
    }
    else
    {
        MXB_ERROR("Could not perform thread initialization for all modules. Thread exits.");
        this_thread.current_worker_id = WORKER_ABSENT_ID;
    }

    return rv;
}

void RoutingWorker::post_run()
{
    pool_close_all_conns();

    // See MainWorker::post_run for an explanation why this is done here
    m_storage.clear();

    qc_thread_end(QC_INIT_SELF);
    modules_thread_finish();
    // TODO: Add service_thread_finish().
    this_thread.current_worker_id = WORKER_ABSENT_ID;
}

/**
 * Creates a worker instance.
 * - Allocates the structure.
 * - Creates a pipe.
 * - Adds the read descriptor to the polling mechanism.
 *
 * @param pNotifier          The watchdog notifier.
 * @param epoll_listener_fd  The file descriptor of the epoll set to which listening
 *                           sockets will be placed.
 *
 * @return A worker instance if successful, otherwise NULL.
 */
// static
RoutingWorker* RoutingWorker::create(mxb::WatchdogNotifier* pNotifier, int epoll_listener_fd)
{
    RoutingWorker* pThis = new(std::nothrow) RoutingWorker(pNotifier);

    if (pThis)
    {
        // The shared epoll instance descriptor is *not* added using EPOLLET (edge-triggered)
        // because we want it to be level-triggered. That way, as long as there is a single
        // active (accept() can be called) listening socket, epoll_wait() will return an event
        // for it.
        if (pThis->add_pollable(EPOLLIN, pThis))
        {
            MXB_INFO("Epoll instance for listening sockets added to worker epoll instance.");
        }
        else
        {
            MXB_ERROR("Could not add epoll instance for listening sockets to "
                      "epoll instance of worker: %s",
                      mxb_strerror(errno));
            delete pThis;
            pThis = NULL;
        }
    }
    else
    {
        MXB_OOM();
    }

    return pThis;
}

void RoutingWorker::epoll_tick()
{
    process_timeouts();

    delete_zombies();

    for (auto& func : m_epoll_tick_funcs)
    {
        func();
    }

    if (m_rebalance.perform)
    {
        rebalance();
    }
}

int RoutingWorker::poll_fd() const
{
    return this_unit.epoll_listener_fd;
}

/**
 * Handler for events occurring in the shared epoll instance.
 *
 * @param events  The events.
 *
 * @return What actions were performed.
 */
uint32_t RoutingWorker::handle_poll_events(mxb::Worker* pWorker, uint32_t events, Pollable::Context context)
{
    mxb_assert(pWorker == this);

    struct epoll_event epoll_events[1];

    // We extract just one event
    int nfds = epoll_wait(this_unit.epoll_listener_fd, epoll_events, 1, 0);

    uint32_t actions = mxb::poll_action::NOP;

    if (nfds == -1)
    {
        MXB_ERROR("epoll_wait failed: %s", mxb_strerror(errno));
    }
    else if (nfds == 0)
    {
        MXB_DEBUG("No events for worker %d.", id());
    }
    else
    {
        MXB_DEBUG("1 event for routing worker %d.", id());
        Pollable* pPollable = static_cast<Pollable*>(epoll_events[0].data.ptr);

        actions = pPollable->handle_poll_events(this, epoll_events[0].events, Pollable::NEW_CALL);
    }

    return actions;
}

// static
size_t RoutingWorker::broadcast(Task* pTask, Semaphore* pSem)
{
    // No logging here, function must be signal safe.
    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(pTask, pSem, EXECUTE_AUTO))
        {
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::broadcast(std::unique_ptr<DisposableTask> sTask)
{
    DisposableTask* pTask = sTask.release();
    Worker::inc_ref(pTask);

    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->post_disposable(pTask, EXECUTE_AUTO))
        {
            ++n;
        }
    }

    Worker::dec_ref(pTask);

    return n;
}

// static
size_t RoutingWorker::broadcast(const std::function<void ()>& func,
                                mxb::Semaphore* pSem,
                                mxb::Worker::execute_mode_t mode)
{
    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(func, pSem, mode))
        {
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_serially(Task& task)
{
    Semaphore sem;
    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(&task, &sem, EXECUTE_AUTO))
        {
            sem.wait();
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_serially(const std::function<void()>& func)
{
    Semaphore sem;
    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->execute(func, &sem, EXECUTE_AUTO))
        {
            sem.wait();
            ++n;
        }
    }

    return n;
}

// static
size_t RoutingWorker::execute_concurrently(Task& task)
{
    Semaphore sem;
    return sem.wait_n(RoutingWorker::broadcast(&task, &sem));
}

// static
size_t RoutingWorker::execute_concurrently(const std::function<void()>& func)
{
    Semaphore sem;
    return sem.wait_n(RoutingWorker::broadcast(func, &sem, EXECUTE_AUTO));
}

// static
size_t RoutingWorker::broadcast_message(uint32_t msg_id, intptr_t arg1, intptr_t arg2)
{
    // NOTE: No logging here, this function must be signal safe.

    size_t n = 0;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        Worker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        if (pWorker->post_message(msg_id, arg1, arg2))
        {
            ++n;
        }
    }

    return n;
}

namespace
{

std::vector<Worker::STATISTICS> get_stats()
{
    std::vector<Worker::STATISTICS> rval;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];
        mxb_assert(pWorker);

        rval.push_back(pWorker->statistics());
    }

    return rval;
}
}

// static
Worker::STATISTICS RoutingWorker::get_statistics()
{
    auto s = get_stats();

    STATISTICS cs;

    cs.n_read = mxs::sum(s, &STATISTICS::n_read);
    cs.n_write = mxs::sum(s, &STATISTICS::n_write);
    cs.n_error = mxs::sum(s, &STATISTICS::n_error);
    cs.n_hup = mxs::sum(s, &STATISTICS::n_hup);
    cs.n_accept = mxs::sum(s, &STATISTICS::n_accept);
    cs.n_polls = mxs::sum(s, &STATISTICS::n_polls);
    cs.n_pollev = mxs::sum(s, &STATISTICS::n_pollev);
    cs.evq_avg = mxs::avg(s, &STATISTICS::evq_avg);
    cs.evq_max = mxs::max(s, &STATISTICS::evq_max);
    cs.maxqtime = mxs::max(s, &STATISTICS::maxqtime);
    cs.maxexectime = mxs::max(s, &STATISTICS::maxexectime);
    cs.n_fds = mxs::sum_element(s, &STATISTICS::n_fds);
    cs.n_fds = mxs::min_element(s, &STATISTICS::n_fds);
    cs.n_fds = mxs::max_element(s, &STATISTICS::n_fds);
    cs.qtimes = mxs::avg_element(s, &STATISTICS::qtimes);
    cs.exectimes = mxs::avg_element(s, &STATISTICS::exectimes);

    return cs;
}

// static
bool RoutingWorker::get_qc_stats_by_index(int index, QC_CACHE_STATS* pStats)
{
    class Task : public Worker::Task
    {
    public:
        Task(QC_CACHE_STATS* pStats)
            : m_stats(*pStats)
        {
        }

        void execute(Worker&) override final
        {
            qc_get_cache_stats(&m_stats);
        }

    private:
        QC_CACHE_STATS& m_stats;
    };

    RoutingWorker* pWorker = RoutingWorker::get_by_index(index);

    if (pWorker)
    {
        Semaphore sem;
        Task task(pStats);
        pWorker->execute(&task, &sem, EXECUTE_AUTO);
        sem.wait();
    }

    return pWorker != nullptr;
}

// static
void RoutingWorker::get_qc_stats(std::vector<QC_CACHE_STATS>& all_stats)
{
    class Task : public Worker::Task
    {
    public:
        Task(std::vector<QC_CACHE_STATS>* pAll_stats)
            : m_all_stats(*pAll_stats)
        {
            m_all_stats.resize(this_unit.nWorkers);
        }

        void execute(Worker& worker) override final
        {
            int index = static_cast<RoutingWorker&>(worker).index();
            mxb_assert(index >= 0 && index < this_unit.nWorkers);

            QC_CACHE_STATS& stats = m_all_stats[index];

            qc_get_cache_stats(&stats);
        }

    private:
        std::vector<QC_CACHE_STATS>& m_all_stats;
    };

    Task task(&all_stats);
    mxs::RoutingWorker::execute_concurrently(task);
}

namespace
{

json_t* qc_stats_to_json(const char* zHost, int id, const QC_CACHE_STATS& stats)
{
    json_t* pStats = json_object();
    json_object_set_new(pStats, "size", json_integer(stats.size));
    json_object_set_new(pStats, "inserts", json_integer(stats.inserts));
    json_object_set_new(pStats, "hits", json_integer(stats.hits));
    json_object_set_new(pStats, "misses", json_integer(stats.misses));
    json_object_set_new(pStats, "evictions", json_integer(stats.evictions));

    json_t* pAttributes = json_object();
    json_object_set_new(pAttributes, "stats", pStats);

    json_t* pSelf = mxs_json_self_link(zHost, "qc_stats", std::to_string(id).c_str());

    json_t* pJson = json_object();
    json_object_set_new(pJson, CN_ID, json_string(std::to_string(id).c_str()));
    json_object_set_new(pJson, CN_TYPE, json_string("qc_stats"));
    json_object_set_new(pJson, CN_ATTRIBUTES, pAttributes);
    json_object_set_new(pJson, CN_LINKS, pSelf);

    return pJson;
}
}

// static
std::unique_ptr<json_t> RoutingWorker::get_qc_stats_as_json_by_index(const char* zHost, int index)
{
    std::unique_ptr<json_t> sStats;

    QC_CACHE_STATS stats;

    if (get_qc_stats_by_index(index, &stats))
    {
        json_t* pJson = qc_stats_to_json(zHost, index, stats);

        stringstream self;
        self << MXS_JSON_API_QC_STATS << index;

        sStats.reset(mxs_json_resource(zHost, self.str().c_str(), pJson));
    }

    return sStats;
}

// static
std::unique_ptr<json_t> RoutingWorker::get_qc_stats_as_json(const char* zHost)
{
    vector<QC_CACHE_STATS> all_stats;

    get_qc_stats(all_stats);

    std::unique_ptr<json_t> sAll_stats(json_array());

    int id = 0;
    for (const auto& stats : all_stats)
    {
        json_t* pJson = qc_stats_to_json(zHost, id, stats);

        json_array_append_new(sAll_stats.get(), pJson);
        ++id;
    }

    return std::unique_ptr<json_t>(mxs_json_resource(zHost, MXS_JSON_API_QC_STATS, sAll_stats.release()));
}

// static
RoutingWorker* RoutingWorker::pick_worker()
{
    static uint32_t id_generator = 0;
    int id = this_unit.id_min_worker
        + (mxb::atomic::add(&id_generator, 1, mxb::atomic::RELAXED) % this_unit.nWorkers);
    return get(id);
}

void RoutingWorker::register_epoll_tick_func(std::function<void ()> func)
{
    m_epoll_tick_funcs.push_back(func);
}

// static
void RoutingWorker::collect_worker_load(size_t count)
{
    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        auto* pWorker = this_unit.ppWorkers[i];
        auto* pWorker_load = this_unit.ppWorker_loads[i];

        if (pWorker_load->size() != count)
        {
            pWorker_load->resize(count);
        }

        pWorker_load->add_value(pWorker->load(mxb::WorkerLoad::ONE_SECOND));
    }
}

// static
bool RoutingWorker::balance_workers()
{
    bool balancing = false;

    int threshold = mxs::Config::get().rebalance_threshold.get();

    if (threshold != 0)
    {
        balancing = balance_workers(threshold);
    }

    return balancing;
}

// static
bool RoutingWorker::balance_workers(int threshold)
{
    bool balancing = false;

    int min_load = 100;
    int max_load = 0;
    RoutingWorker* pTo = nullptr;
    RoutingWorker* pFrom = nullptr;

    auto rebalance_period = mxs::Config::get().rebalance_period.get();
    // If rebalance_period is != 0, then the average load has been updated
    // and we can use it.
    bool use_average = rebalance_period != std::chrono::milliseconds(0);

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        RoutingWorker* pWorker = this_unit.ppWorkers[i];

        int load;

        if (use_average)
        {
            mxb::Average* pLoad = this_unit.ppWorker_loads[i];
            load = pLoad->value();
        }
        else
        {
            // If we can't use the average, we use one second load.
            load = pWorker->load(mxb::WorkerLoad::ONE_SECOND);
        }

        if (load < min_load)
        {
            min_load = load;
            pTo = pWorker;
        }

        if (load > max_load)
        {
            max_load = load;
            pFrom = pWorker;
        }
    }

    int diff_load = max_load - min_load;

    if (diff_load > threshold)
    {
        MXB_NOTICE("Difference in load (%d) between the thread with the maximum load (%d) the thread "
                   "with the minimum load (%d) exceeds the 'rebalance_threshold' value of %d, "
                   "moving work from the latter to the former.",
                   diff_load, max_load, min_load, threshold);
        balancing = true;
    }

    if (balancing)
    {
        mxb_assert(pFrom);
        mxb_assert(pTo);

        if (!pFrom->execute([pFrom, pTo]() {
                                pFrom->rebalance(pTo);
                            }, Worker::EXECUTE_QUEUED))
        {
            MXB_ERROR("Could not post task to worker, worker load balancing will not take place.");
        }
    }

    return balancing;
}

void RoutingWorker::rebalance(RoutingWorker* pTo, int nSessions)
{
    // We can't balance here, because if a single epoll_wait() call returns
    // both the rebalance-message (sent from balance_workers() above) and
    // an event for a DCB that we move to another worker, we would crash.
    // So we only make a note and rebalance in epoll_tick().
    m_rebalance.set(pTo, nSessions);
}

void RoutingWorker::rebalance()
{
    mxb_assert(m_rebalance.pTo);
    mxb_assert(m_rebalance.perform);

    const int n_requested_moves = m_rebalance.nSessions;
    if (n_requested_moves == 1)
    {
        // Just one, move the most active one.
        int max_io_activity = 0;
        Session* pMax_session = nullptr;

        for (auto& kv : m_sessions)
        {
            auto pSession = static_cast<Session*>(kv.second);
            if (pSession->is_movable())
            {
                int io_activity = pSession->io_activity();

                if (io_activity > max_io_activity)
                {
                    max_io_activity = io_activity;
                    pMax_session = pSession;
                }
            }
        }

        if (pMax_session)
        {
            pMax_session->move_to(m_rebalance.pTo);
        }
        else if (!m_sessions.empty())
        {
            MXB_INFO("Could not move any sessions from worker %i because all its sessions are in an "
                     "unmovable state.", id());
        }
    }
    else if (n_requested_moves > 1)
    {
        // TODO: Move all sessions in one message to recipient worker.

        std::vector<Session*> sessions;

        // If more than one, just move enough sessions is arbitrary order.
        for (auto& kv : m_sessions)
        {
            auto pSession = static_cast<Session*>(kv.second);
            if (pSession->is_movable())
            {
                sessions.push_back(pSession);
                if (sessions.size() == (size_t)n_requested_moves)
                {
                    break;
                }
            }
        }

        int n_available_sessions = m_sessions.size();
        int n_movable_sessions = sessions.size();
        if (n_movable_sessions < n_requested_moves && n_available_sessions >= n_requested_moves)
        {
            // Had enough sessions but some were not movable.
            int non_movable = n_available_sessions - n_movable_sessions;
            MXB_INFO("%i session(s) out of %i on worker %i are in an unmovable state.",
                     non_movable, n_available_sessions, id());
        }

        for (auto* pSession : sessions)
        {
            pSession->move_to(m_rebalance.pTo);
        }
    }

    m_rebalance.reset();
}

namespace
{

class MemoryTask : public Worker::Task
{
public:
    MemoryTask(uint32_t nThreads)
        : m_tmus(nThreads)
    {
    }

    void execute(Worker& worker) override final
    {
        auto& rworker = static_cast<RoutingWorker&>(worker);

        m_tmus[rworker.index()] = rworker.calculate_memory_usage();
    }

    void fill(json_t* pStats)
    {
        RoutingWorker::MemoryUsage pmu;

        json_t* pThreads = json_array();

        for (size_t i = 0; i < m_tmus.size(); ++i)
        {
            const auto& tmu = m_tmus[i];

            json_array_append_new(pThreads, tmu.to_json());
            pmu += tmu;
        }

        json_object_set_new(pStats, "process", pmu.to_json());
        json_object_set_new(pStats, "threads", pThreads);
    }

private:
    std::vector<RoutingWorker::MemoryUsage> m_tmus;
};

}

//static
std::unique_ptr<json_t> RoutingWorker::memory_to_json(const char* zHost)
{
    MemoryTask task(this_unit.nWorkers);
    RoutingWorker::execute_concurrently(task);

    json_t* pAttr = json_object();
    task.fill(pAttr);

    json_t* pMemory = json_object();
    json_object_set_new(pMemory, CN_ID, json_string(CN_MEMORY));
    json_object_set_new(pMemory, CN_TYPE, json_string(CN_MEMORY));
    json_object_set_new(pMemory, CN_ATTRIBUTES, pAttr);

    return std::unique_ptr<json_t>(mxs_json_resource(zHost, MXS_JSON_API_MEMORY, pMemory));
}

RoutingWorker::MemoryUsage RoutingWorker::calculate_memory_usage() const
{
    MemoryUsage rv;

    QC_CACHE_STATS qc;
    if (qc_get_cache_stats(&qc))
    {
        rv.query_classifier = qc.size;
    }

    for (const DCB* pZombie : m_zombies)
    {
        rv.zombies += pZombie->runtime_size();
    }

    const Registry<MXS_SESSION>& sessions = session_registry();
    for (const auto& kv : sessions)
    {
        rv.sessions += kv.second->runtime_size();
    }

    rv.total = rv.query_classifier + rv.zombies + rv.sessions;

    return rv;
}

// static
void RoutingWorker::start_shutdown()
{
    broadcast([]() {
                  auto worker = RoutingWorker::get_current();
                  worker->m_callable.dcall(100ms, &RoutingWorker::try_shutdown, worker);
              }, nullptr, EXECUTE_AUTO);
}

bool RoutingWorker::try_shutdown()
{
    pool_close_all_conns();

    if (m_sessions.empty())
    {
        shutdown();
    }
    else
    {
        for (const auto& s : m_sessions)
        {
            s.second->kill();
        }
    }

    return true;
}

void RoutingWorker::register_session(MXS_SESSION* ses)
{
    MXB_AT_DEBUG(bool rv = ) m_sessions.add(ses);
    mxb_assert(rv);
}

void RoutingWorker::deregister_session(uint64_t session_id)
{
    MXB_AT_DEBUG(bool rv = ) m_sessions.remove(session_id);
    mxb_assert(rv);
}

void RoutingWorker::pool_set_size(const std::string& srvname, int64_t size)
{
    auto rworker = RoutingWorker::get_current();
    std::lock_guard<std::mutex> guard(rworker->m_pool_lock);
    // Check if the worker has a pool with the given server name and update if found.
    // The pool may not exist if pooling was previously disabled or empty.
    for (auto& kv : rworker->m_pool_group)
    {
        if (kv.first->name() == srvname)
        {
            kv.second.set_capacity(size);
            break;
        }
    }
}

RoutingWorker::ConnectionPoolStats RoutingWorker::pool_get_stats(const SERVER* pSrv)
{
    mxb_assert(mxs::MainWorker::is_main_worker());
    RoutingWorker::ConnectionPoolStats rval;

    for (int i = 0; i < this_unit.nWorkers; ++i)
    {
        rval.add(this_unit.ppWorkers[i]->pool_stats(pSrv));
    }

    return rval;
}

RoutingWorker::ConnectionPoolStats RoutingWorker::pool_stats(const SERVER* pSrv)
{
    ConnectionPoolStats rval;
    std::lock_guard<std::mutex> guard(m_pool_lock);

    auto it = m_pool_group.find(pSrv);

    if (it != m_pool_group.end())
    {
        rval = it->second.stats();
    }

    return rval;
}

void RoutingWorker::add_conn_wait_entry(ServerEndpoint* ep)
{
    m_eps_waiting_for_conn[ep->server()].push_back(ep);
}

void RoutingWorker::erase_conn_wait_entry(ServerEndpoint* ep)
{
    auto map_iter = m_eps_waiting_for_conn.find(ep->server());
    mxb_assert(map_iter != m_eps_waiting_for_conn.end());
    // The element is surely found in both the map and the set.
    auto& ep_deque = map_iter->second;
    // Erasing from the middle of a deque is inefficient, as possibly a large number of elements
    // needs to be moved. TODO: set the element to null and erase later.
    ep_deque.erase(std::find(ep_deque.begin(), ep_deque.end(), ep));

    if (ep_deque.empty())
    {
        m_eps_waiting_for_conn.erase(map_iter);
    }
}

void RoutingWorker::notify_connection_available(SERVER* server)
{
    // A connection to a server should be available, either in the pool or a new one can be created.
    // Cannot be certain due to other threads. Do not activate any connections here, only schedule a check.

    // In the vast majority of cases (whenever idle pooling is disabled) the map is empty.
    if (!m_eps_waiting_for_conn.empty() && !m_ep_activation_scheduled)
    {
        if (m_eps_waiting_for_conn.count(server) > 0)
        {
            // An endpoint is waiting for connection to this server.
            auto func = [this]() {
                    activate_waiting_endpoints();
                    m_ep_activation_scheduled = false;
                    return false;
                };

            // The check will run once execution returns to the event loop.
            execute(func, execute_mode_t::EXECUTE_QUEUED);
            m_ep_activation_scheduled = true;
        }
    }
}

/**
 * A connection slot to at least one server should be available. Add as many connections as possible.
 */
void RoutingWorker::activate_waiting_endpoints()
{
    auto map_iter = m_eps_waiting_for_conn.begin();
    while (map_iter != m_eps_waiting_for_conn.end())
    {
        auto& ep_set = map_iter->second;
        bool keep_activating = true;

        while (keep_activating && !ep_set.empty())
        {
            bool erase_from_set = false;
            auto it_first = ep_set.begin();
            auto* ep = *it_first;
            auto res = ep->continue_connecting();

            switch (res)
            {
            case ServerEndpoint::ContinueRes::SUCCESS:
                // Success, remove from wait list.
                erase_from_set = true;
                break;

            case ServerEndpoint::ContinueRes::WAIT:
                // No connection was available, perhaps connection limit was reached. Continue waiting.
                // Do not try to connect to this server again right now.
                keep_activating = false;
                break;

            case ServerEndpoint::ContinueRes::FAIL:
                // Resuming the connection failed. Either connection was resumed but writing packets failed
                // or something went wrong in creating a new connection. Close the endpoint. The endpoint map
                // must not be modified by the handle_failed_continue call.
                erase_from_set = true;
                ep->handle_failed_continue();
                break;
            }

            if (erase_from_set)
            {
                ep_set.erase(it_first);
            }
        }

        if (ep_set.empty())
        {
            map_iter = m_eps_waiting_for_conn.erase(map_iter);
        }
        else
        {
            map_iter++;
        }
    }
}

void RoutingWorker::fail_timed_out_endpoints()
{
    // Check the oldest endpoints. Fail the ones which have been waiting for too long.
    auto now = epoll_tick_now();
    auto it_map = m_eps_waiting_for_conn.begin();
    while (it_map != m_eps_waiting_for_conn.end())
    {
        auto& ep_deq = it_map->second;
        // The oldest ep:s are at the front of the deque. Close timed out ones until an element is no
        // longer timed out.
        auto it = ep_deq.begin();

        while (it != ep_deq.end())
        {
            auto* ep = *it;
            if (now - ep->conn_wait_start() > ep->session()->multiplex_timeout())
            {
                ep->handle_timed_out_continue();
                it = ep_deq.erase(it);
            }
            else
            {
                break;
            }
        }

        if (ep_deq.empty())
        {
            it_map = m_eps_waiting_for_conn.erase(it_map);
        }
        else
        {
            ++it_map;
        }
    }
}

void RoutingWorker::pool_close_expired()
{
    std::lock_guard<std::mutex> guard(m_pool_lock);

    // Close expired connections in the thread local pool. If the server is down, purge all connections.
    for (auto& kv : m_pool_group)
    {
        auto* pServer = kv.first;
        auto& server_pool = kv.second;

        if (pServer->is_down())
        {
            server_pool.close_all();
        }
        else
        {
            server_pool.close_expired();
        }
    }
}

bool RoutingWorker::conn_to_server_needed(const SERVER* srv) const
{
    return m_eps_waiting_for_conn.find(srv) != m_eps_waiting_for_conn.end();
}

void RoutingWorker::ConnectionPoolStats::add(const ConnectionPoolStats& rhs)
{
    curr_size += rhs.curr_size;
    max_size += rhs.max_size;
    times_found += rhs.times_found;
    times_empty += rhs.times_empty;
}

class RoutingWorker::InfoTask : public Worker::Task
{
public:
    InfoTask(const char* zHost, uint32_t nThreads)
        : m_zHost(zHost)
    {
        m_data.resize(nThreads);
    }

    void execute(Worker& worker) override final
    {
        RoutingWorker& rworker = static_cast<RoutingWorker&>(worker);
        mxb_assert(rworker.is_current());

        json_t* pStats = json_object();

        add_stats(rworker, pStats);

        json_t* pAttr = json_object();
        json_object_set_new(pAttr, "stats", pStats);

        int index = rworker.index();
        stringstream ss;
        ss << index;

        json_t* pJson = json_object();
        json_object_set_new(pJson, CN_ID, json_string(ss.str().c_str())); // Index is the id for the outside.
        json_object_set_new(pJson, CN_TYPE, json_string(CN_THREADS));
        json_object_set_new(pJson, CN_ATTRIBUTES, pAttr);
        json_object_set_new(pJson, CN_LINKS, mxs_json_self_link(m_zHost, CN_THREADS, ss.str().c_str()));

        mxb_assert((size_t)index < m_data.size());
        m_data[index] = pJson;
    }

    json_t* resource()
    {
        json_t* pArr = json_array();

        for (auto it = m_data.begin(); it != m_data.end(); it++)
        {
            json_array_append_new(pArr, *it);
        }

        return mxs_json_resource(m_zHost, MXS_JSON_API_THREADS, pArr);
    }

    json_t* resource(int index)
    {
        stringstream self;
        self << MXS_JSON_API_THREADS << index;
        return mxs_json_resource(m_zHost, self.str().c_str(), m_data[index]);
    }

private:
    static void add_stats(const RoutingWorker& rworker, json_t* pStats)
    {
        const Worker::STATISTICS& s = rworker.statistics();
        json_object_set_new(pStats, "reads", json_integer(s.n_read));
        json_object_set_new(pStats, "writes", json_integer(s.n_write));
        json_object_set_new(pStats, "errors", json_integer(s.n_error));
        json_object_set_new(pStats, "hangups", json_integer(s.n_hup));
        json_object_set_new(pStats, "accepts", json_integer(s.n_accept));
        json_object_set_new(pStats, "avg_event_queue_length", json_integer(s.evq_avg));
        json_object_set_new(pStats, "max_event_queue_length", json_integer(s.evq_max));
        json_object_set_new(pStats, "max_exec_time", json_integer(s.maxexectime));
        json_object_set_new(pStats, "max_queue_time", json_integer(s.maxqtime));

        int64_t nCurrent = rworker.current_fd_count();
        int64_t nTotal = rworker.total_fd_count();
        json_object_set_new(pStats, "current_descriptors", json_integer(nCurrent));
        json_object_set_new(pStats, "total_descriptors", json_integer(nTotal));

        json_t* pLoad = json_object();
        json_object_set_new(pLoad, "last_second", json_integer(rworker.load(Worker::Load::ONE_SECOND)));
        json_object_set_new(pLoad, "last_minute", json_integer(rworker.load(Worker::Load::ONE_MINUTE)));
        json_object_set_new(pLoad, "last_hour", json_integer(rworker.load(Worker::Load::ONE_HOUR)));

        json_object_set_new(pStats, "load", pLoad);

        json_object_set_new(pStats, "query_classifier_cache", qc_get_cache_stats_as_json());

        json_object_set_new(pStats, "sessions", json_integer(rworker.session_registry().size()));
        json_object_set_new(pStats, "zombies", json_integer(rworker.m_zombies.size()));

        RoutingWorker::MemoryUsage mu = rworker.calculate_memory_usage();
        json_object_set_new(pStats, "memory", mu.to_json());
    }

private:
    vector<json_t*> m_data;
    const char*     m_zHost;
};
}

namespace
{

class FunctionTask : public Worker::DisposableTask
{
public:
    FunctionTask(std::function<void ()> cb)
        : m_cb(cb)
    {
    }

    void execute(Worker& worker) override final
    {
        m_cb();
    }

protected:
    std::function<void ()> m_cb;
};
}

json_t* mxs_rworker_to_json(const char* zHost, int index)
{
    Worker* target = RoutingWorker::get_by_index(index);
    RoutingWorker::InfoTask task(zHost, index + 1);
    Semaphore sem;

    target->execute(&task, &sem, Worker::EXECUTE_AUTO);
    sem.wait();

    return task.resource(index);
}

json_t* mxs_rworker_list_to_json(const char* host)
{
    RoutingWorker::InfoTask task(host, this_unit.nWorkers);
    RoutingWorker::execute_concurrently(task);
    return task.resource();
}

namespace
{

class WatchdogTask : public Worker::Task
{
public:
    WatchdogTask()
    {
    }

    void execute(Worker& worker) override final
    {
        // Success if this is called.
    }
};
}

void mxs_rworker_watchdog()
{
    MXB_INFO("MaxScale watchdog called.");
    WatchdogTask task;
    RoutingWorker::execute_concurrently(task);
}
