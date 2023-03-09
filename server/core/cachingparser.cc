/*
 * Copyright (c) 2023 MariaDB plc
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

#include <maxscale/cachingparser.hh>
#include <atomic>
#include <map>
#include <random>
#include <maxscale/buffer.hh>
#include <maxscale/cn_strings.hh>
#include <maxscale/json_api.hh>
// TODO: Remove mariadb dependency.
#include <maxscale/protocol/mariadb/mysql.hh>
#include <maxscale/routingworker.hh>

namespace
{

const char CN_CACHE[] = "cache";
const char CN_CACHE_SIZE[] = "cache_size";
const char CN_CLASSIFICATION[] = "classification";
const char CN_HITS[] = "hits";

class ThisUnit
{
public:
    ThisUnit()
        : m_cache_max_size(std::numeric_limits<int64_t>::max())
    {
    }

    ThisUnit(const ThisUnit&) = delete;
    ThisUnit& operator=(const ThisUnit&) = delete;

    int64_t cache_max_size() const
    {
        // In principle, std::memory_order_acquire should be used here, but that causes
        // a performance penalty of ~5% when running a sysbench test.
        return m_cache_max_size.load(std::memory_order_relaxed);
    }

    void set_cache_max_size(int64_t cache_max_size)
    {
        // In principle, std::memory_order_release should be used here.
        m_cache_max_size.store(cache_max_size, std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> m_cache_max_size;
};

ThisUnit this_unit;

class QCInfoCache;

thread_local struct
{
    QCInfoCache* pInfo_cache { nullptr };
    uint32_t     options { 0 };
    bool         use_cache { true };
} this_thread;


bool use_cached_result()
{
    return this_unit.cache_max_size() != 0 && this_thread.use_cache;
}

bool has_not_been_parsed(GWBUF* pStmt)
{
    // A GWBUF has not been parsed, if it does not have a parsing info object attached.
    return pStmt->get_classifier_data_ptr() == nullptr;
}

/**
 * @class QCInfoCache
 *
 * An instance of this class maintains a mapping from a canonical statement to
 * the QC_STMT_INFO object created by the actual query classifier.
 */
class QCInfoCache
{
public:
    QCInfoCache(const QCInfoCache&) = delete;
    QCInfoCache& operator=(const QCInfoCache&) = delete;

    QCInfoCache()
        : m_reng(m_rdev())
    {
        memset(&m_stats, 0, sizeof(m_stats));
    }

    ~QCInfoCache()
    {
    }

    void inc_ref()
    {
        mxb_assert(m_refs >= 0);
        ++m_refs;
    }

    int32_t dec_ref()
    {
        mxb_assert(m_refs > 0);
        return --m_refs;
    }

    QC_STMT_INFO* peek(std::string_view canonical_stmt) const
    {
        auto i = m_infos.find(canonical_stmt);

        return i != m_infos.end() ? i->second.sInfo.get() : nullptr;
    }

    std::shared_ptr<QC_STMT_INFO> get(mxs::Parser::Plugin* pPlugin, std::string_view canonical_stmt)
    {
        std::shared_ptr<QC_STMT_INFO> sInfo;
        qc_sql_mode_t sql_mode = pPlugin->parser().get_sql_mode();

        auto i = m_infos.find(canonical_stmt);

        if (i != m_infos.end())
        {
            Entry& entry = i->second;

            if ((entry.sql_mode == sql_mode)
                && (entry.options == this_thread.options))
            {
                sInfo = entry.sInfo;

                ++entry.hits;
                ++m_stats.hits;
            }
            else
            {
                // If the sql_mode or options has changed, we discard the existing result.
                erase(i);

                ++m_stats.misses;
            }
        }
        else
        {
            ++m_stats.misses;
        }

        return sInfo;
    }

    void insert(mxs::Parser::Plugin* pPlugin,
                std::string_view canonical_stmt,
                std::shared_ptr<QC_STMT_INFO> sInfo)
    {
        mxb_assert(peek(canonical_stmt) == nullptr);

        // 0xffffff is the maximum packet size, 4 is for packet header and 1 is for command byte. These are
        // MariaDB/MySQL protocol specific values that are also defined in <maxscale/protocol/mysql.h> but
        // should not be exposed to the core.
        constexpr int64_t max_entry_size = 0xffffff - 5;

        // RoutingWorker::nRunning() and not Config::n_threads, as the former tells how many
        // threads are currently running and the latter how many they eventually will be.
        // When increasing there will not be a difference, but when decreasing there will be.
        int nRunning = mxs::RoutingWorker::nRunning();
        int64_t cache_max_size = this_unit.cache_max_size() / (nRunning != 0 ? nRunning : 1);

        /** Because some queries cause much more memory to be used than can be measured,
         *  the limit is reduced here. In the future the cache entries will be changed so
         *  that memory fragmentation is minimized.
         */
        cache_max_size *= 0.65;

        int64_t size = entry_size(sInfo.get());

        if (size < max_entry_size && size <= cache_max_size)
        {
            int64_t required_space = (m_stats.size + size) - cache_max_size;

            if (required_space > 0)
            {
                make_space(required_space);
            }

            if (m_stats.size + size <= cache_max_size)
            {
                qc_sql_mode_t sql_mode = pPlugin->parser().get_sql_mode();

                m_infos.emplace(canonical_stmt,
                                Entry(pPlugin, std::move(sInfo), sql_mode, this_thread.options));

                ++m_stats.inserts;
                m_stats.size += size;
            }
        }
    }

    void update_total_size(int32_t delta)
    {
        m_stats.size += delta;
    }

    void get_stats(QC_CACHE_STATS* pStats)
    {
        *pStats = m_stats;
    }

    void get_state(std::map<std::string, QC_CACHE_ENTRY>& state) const
    {
        for (const auto& info : m_infos)
        {
            std::string stmt = std::string(info.first);
            const Entry& entry = info.second;

            auto it = state.find(stmt);

            if (it == state.end())
            {
                QC_CACHE_ENTRY e {};

                e.hits = entry.hits;
                e.result = entry.pPlugin->get_result_from_info(entry.sInfo.get());

                state.insert(std::make_pair(stmt, e));
            }
            else
            {
                QC_CACHE_ENTRY& e = it->second;

                e.hits += entry.hits;
#if defined (SS_DEBUG)
                QC_STMT_RESULT result = entry.pPlugin->get_result_from_info(entry.sInfo.get());

                mxb_assert(e.result.status == result.status);
                mxb_assert(e.result.type_mask == result.type_mask);
                mxb_assert(e.result.op == result.op);
#endif
            }
        }
    }

    int64_t clear()
    {
        int64_t rv = 0;

        for (auto& kv : m_infos)
        {
            rv += entry_size(kv.second.sInfo.get());
        }

        m_infos.clear();

        return rv;
    }

private:
    struct Entry
    {
        Entry(mxs::Parser::Plugin* pPlugin,
              std::shared_ptr<QC_STMT_INFO> sInfo,
              qc_sql_mode_t sql_mode,
              uint32_t options)
            : pPlugin(pPlugin)
            , sInfo(std::move(sInfo))
            , sql_mode(sql_mode)
            , options(options)
            , hits(0)
        {
        }

        mxs::Parser::Plugin*          pPlugin;
        std::shared_ptr<QC_STMT_INFO> sInfo;
        qc_sql_mode_t                 sql_mode;
        uint32_t                      options;
        int64_t                       hits;
    };

    typedef std::unordered_map<std::string_view, Entry> InfosByStmt;

    int64_t entry_size(const QC_STMT_INFO* pInfo)
    {
        const int64_t map_entry_overhead = 4 * sizeof(void *);
        const int64_t constant_overhead = sizeof(std::string_view) + sizeof(Entry) + map_entry_overhead;

        return constant_overhead + pInfo->size();
    }

    int64_t entry_size(const InfosByStmt::value_type& entry)
    {
        return entry_size(entry.second.sInfo.get());
    }

    void erase(InfosByStmt::iterator& i)
    {
        mxb_assert(i != m_infos.end());

        m_stats.size -= entry_size(*i);

        m_infos.erase(i);

        ++m_stats.evictions;
    }

    bool erase(std::string_view canonical_stmt)
    {
        bool erased = false;

        auto i = m_infos.find(canonical_stmt);
        mxb_assert(i != m_infos.end());

        if (i != m_infos.end())
        {
            erase(i);
            erased = true;
        }

        return erased;
    }

    void make_space(int64_t required_space)
    {
        int64_t freed_space = 0;

        std::uniform_int_distribution<> dis(0, m_infos.bucket_count() - 1);

        while ((freed_space < required_space) && !m_infos.empty())
        {
            freed_space += evict(dis);
        }
    }

    int64_t evict(std::uniform_int_distribution<>& dis)
    {
        int64_t freed_space = 0;

        int bucket = dis(m_reng);
        mxb_assert((bucket >= 0) && (bucket < static_cast<int>(m_infos.bucket_count())));

        auto i = m_infos.begin(bucket);

        // We just remove the first entry in the bucket. In the general case
        // there will be just one.
        if (i != m_infos.end(bucket))
        {
            freed_space += entry_size(*i);

            MXB_AT_DEBUG(bool erased = ) erase(i->first);
            mxb_assert(erased);
        }

        return freed_space;
    }

    InfosByStmt        m_infos;
    QC_CACHE_STATS     m_stats;
    std::random_device m_rdev;
    std::mt19937       m_reng;
    int32_t            m_refs { 0 };
};

/**
 * @class QCInfoCacheScope
 *
 * QCInfoCacheScope is somewhat like a guard or RAII class that
 * in the constructor
 * - figures out whether the query classification cache should be used,
 * - checks whether the classification result already exists, and
 * - if it does attaches it to the GWBUF
 * and in the destructor
 * - if the query classification result was not already present,
 *   stores the result it in the cache.
 */
class QCInfoCacheScope
{
public:
    QCInfoCacheScope(const QCInfoCacheScope&) = delete;
    QCInfoCacheScope& operator=(const QCInfoCacheScope&) = delete;

    QCInfoCacheScope(mxs::Parser::Plugin* pPlugin, GWBUF* pStmt)
        : m_pPlugin(pPlugin)
        , m_pStmt(pStmt)
    {
        auto pInfo = static_cast<QC_STMT_INFO*>(m_pStmt->get_classifier_data_ptr());
        m_info_size_before = pInfo ? pInfo->size() : 0;

        if (use_cached_result() && has_not_been_parsed(m_pStmt))
        {
            m_canonical = m_pStmt->get_canonical(); // Not from the QC, but from GWBUF.

            // TODO: Remove mariadb dependency.
            if (mariadb::is_com_prepare(*pStmt))
            {
                // P as in prepare, and appended so as not to cause a
                // need for copying the data.
                m_canonical += ":P";
            }

            std::shared_ptr<QC_STMT_INFO> sInfo = this_thread.pInfo_cache->get(m_pPlugin, m_canonical);
            if (sInfo)
            {
                m_info_size_before = sInfo->size();
                m_pStmt->set_classifier_data(std::move(sInfo));
                m_canonical.clear();    // Signals that nothing needs to be added in the destructor.
            }
        }
    }

    ~QCInfoCacheScope()
    {
        bool exclude = exclude_from_cache();

        if (!m_canonical.empty() && !exclude)
        {   // Cache for the first time
            auto sInfo = m_pStmt->get_classifier_data();
            mxb_assert(sInfo);

            // Now from QC and this will have the trailing ":P" in case the GWBUF
            // contained a COM_STMT_PREPARE.
            std::string_view canonical = m_pPlugin->info_get_canonical(sInfo.get());
            mxb_assert(m_canonical == canonical);

            this_thread.pInfo_cache->insert(m_pPlugin, canonical, std::move(sInfo));
        }
        else if (!exclude)
        {   // The size might have changed
            auto pInfo = m_pStmt->get_classifier_data_ptr();
            auto info_size_after = pInfo ? pInfo->size() : 0;

            if (m_info_size_before != info_size_after)
            {
                mxb_assert(m_info_size_before < info_size_after);
                this_thread.pInfo_cache->update_total_size(info_size_after - m_info_size_before);
            }
        }
    }

private:
    mxs::Parser::Plugin* m_pPlugin;
    GWBUF*               m_pStmt;
    std::string          m_canonical;
    size_t               m_info_size_before;

    bool exclude_from_cache() const
    {
        constexpr const int is_autocommit = QUERY_TYPE_ENABLE_AUTOCOMMIT | QUERY_TYPE_DISABLE_AUTOCOMMIT;
        uint32_t type_mask = m_pPlugin->parser().get_type_mask(m_pStmt);
        return (type_mask & is_autocommit) != 0;
    }
};

}

namespace maxscale
{

CachingParser::CachingParser(Plugin* pPlugin)
    : m_plugin(*pPlugin)
    , m_parser(pPlugin->parser())
{
}

//static
void CachingParser::thread_init()
{
    if (!this_thread.pInfo_cache)
    {
        this_thread.pInfo_cache = new QCInfoCache;
    }

    this_thread.pInfo_cache->inc_ref();
}

//static
void CachingParser::thread_finish()
{
    mxb_assert(this_thread.pInfo_cache);

    if (this_thread.pInfo_cache->dec_ref() == 0)
    {
        delete this_thread.pInfo_cache;
        this_thread.pInfo_cache = nullptr;
    }
}

//static
bool CachingParser::set_properties(const QC_CACHE_PROPERTIES& properties)
{
    bool rv = false;

    if (properties.max_size >= 0)
    {
        if (properties.max_size == 0)
        {
            MXB_NOTICE("Query classifier cache disabled.");
        }

        this_unit.set_cache_max_size(properties.max_size);
        rv = true;
    }
    else
    {
        MXB_ERROR("Ignoring attempt to set size of query classifier "
                  "cache to a negative value: %" PRIi64 ".",
                  properties.max_size);
    }

    return rv;
}

//static
void CachingParser::get_properties(QC_CACHE_PROPERTIES* pProperties)
{
    pProperties->max_size = this_unit.cache_max_size();
}

namespace
{

json_t* get_params(json_t* pJson)
{
    json_t* pParams = mxb::json_ptr(pJson, MXS_JSON_PTR_PARAMETERS);

    if (pParams && json_is_object(pParams))
    {
        if (auto pSize = mxb::json_ptr(pParams, CN_CACHE_SIZE))
        {
            if (!json_is_null(pSize) && !json_is_integer(pSize))
            {
                pParams = nullptr;
            }
        }
    }

    return pParams;
}
}

//static
bool CachingParser::set_properties(json_t* pJson)
{
    bool rv = false;

    json_t* pParams = get_params(pJson);

    if (pParams)
    {
        rv = true;

        QC_CACHE_PROPERTIES cache_properties;
        get_properties(&cache_properties);

        json_t* pValue;

        if ((pValue = mxb::json_ptr(pParams, CN_CACHE_SIZE)))
        {
            cache_properties.max_size = json_integer_value(pValue);
            // If get_params() did its job, then we will not
            // get here if the value is negative.
            mxb_assert(cache_properties.max_size >= 0);
        }

        if (rv)
        {
            MXB_AT_DEBUG(bool set = ) mxs::CachingParser::set_properties(cache_properties);
            mxb_assert(set);
        }
    }

    return rv;
}

//static
std::unique_ptr<json_t> CachingParser::get_properties_as_resource(const char* zHost)
{
    QC_CACHE_PROPERTIES properties;
    get_properties(&properties);

    json_t* pParams = json_object();
    json_object_set_new(pParams, CN_CACHE_SIZE, json_integer(properties.max_size));

    json_t* pAttributes = json_object();
    json_object_set_new(pAttributes, CN_PARAMETERS, pParams);

    json_t* pSelf = json_object();
    json_object_set_new(pSelf, CN_ID, json_string(CN_QUERY_CLASSIFIER));
    json_object_set_new(pSelf, CN_TYPE, json_string(CN_QUERY_CLASSIFIER));
    json_object_set_new(pSelf, CN_ATTRIBUTES, pAttributes);

    return std::unique_ptr<json_t>(mxs_json_resource(zHost, MXS_JSON_API_QC, pSelf));
}

namespace
{

json_t* cache_entry_as_json(const std::string& stmt, const QC_CACHE_ENTRY& entry)
{
    json_t* pHits = json_integer(entry.hits);

    json_t* pClassification = json_object();
    json_object_set_new(pClassification,
                        CN_PARSE_RESULT, json_string(mxs::parser::to_string(entry.result.status)));
    std::string type_mask = mxs::Parser::type_mask_to_string(entry.result.type_mask);
    json_object_set_new(pClassification, CN_TYPE_MASK, json_string(type_mask.c_str()));
    json_object_set_new(pClassification,
                        CN_OPERATION,
                        json_string(mxs::Parser::op_to_string(entry.result.op)));

    json_t* pAttributes = json_object();
    json_object_set_new(pAttributes, CN_HITS, pHits);
    json_object_set_new(pAttributes, CN_CLASSIFICATION, pClassification);

    json_t* pSelf = json_object();
    json_object_set_new(pSelf, CN_ID, json_string(stmt.c_str()));
    json_object_set_new(pSelf, CN_TYPE, json_string(CN_CACHE));
    json_object_set_new(pSelf, CN_ATTRIBUTES, pAttributes);

    return pSelf;
}
}

std::unique_ptr<json_t> CachingParser::content_as_resource(const char* zHost)
{
    std::map<std::string, QC_CACHE_ENTRY> state;

    // Assuming the classification cache of all workers will roughly be similar
    // (which will be the case unless something is broken), collecting the
    // information serially from all routing workers will consume 1/N of the
    // memory that would be consumed if the information were collected in
    // parallel and then coalesced here.

    mxs::RoutingWorker::execute_serially([&state]() {
                                             mxs::CachingParser::get_thread_cache_state(state);
                                         });

    json_t* pData = json_array();

    for (const auto& p : state)
    {
        const auto& stmt = p.first;
        const auto& entry = p.second;

        json_t* pEntry = cache_entry_as_json(stmt, entry);

        json_array_append_new(pData, pEntry);
    }

    return std::unique_ptr<json_t>(mxs_json_resource(zHost, MXS_JSON_API_QC_CACHE, pData));
}

//static
int64_t CachingParser::clear_thread_cache()
{
    int64_t rv = 0;
    QCInfoCache* pCache = this_thread.pInfo_cache;

    if (pCache)
    {
        rv = pCache->clear();
    }

    return rv;
}

//static
void CachingParser::get_thread_cache_state(std::map<std::string, QC_CACHE_ENTRY>& state)
{
    QCInfoCache* pCache = this_thread.pInfo_cache;

    if (pCache)
    {
        pCache->get_state(state);
    }
}

//static
bool CachingParser::get_thread_cache_stats(QC_CACHE_STATS* pStats)
{
    bool rv = false;

    QCInfoCache* pInfo_cache = this_thread.pInfo_cache;

    if (pInfo_cache && use_cached_result())
    {
        pInfo_cache->get_stats(pStats);
        rv = true;
    }

    return rv;
}

//static
std::unique_ptr<json_t> CachingParser::get_thread_cache_stats_as_json()
{
    QC_CACHE_STATS stats = {};
    get_thread_cache_stats(&stats);

    std::unique_ptr<json_t> sStats { json_object() };
    json_object_set_new(sStats.get(), "size", json_integer(stats.size));
    json_object_set_new(sStats.get(), "inserts", json_integer(stats.inserts));
    json_object_set_new(sStats.get(), "hits", json_integer(stats.hits));
    json_object_set_new(sStats.get(), "misses", json_integer(stats.misses));
    json_object_set_new(sStats.get(), "evictions", json_integer(stats.evictions));

    return sStats;
}

//static
void CachingParser::set_thread_cache_enabled(bool enabled)
{
    this_thread.use_cache = enabled;
}

mxs::Parser::Plugin& CachingParser::plugin() const
{
    return m_plugin;
}

qc_parse_result_t CachingParser::parse(GWBUF* pStmt, uint32_t collect) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);

    return m_parser.parse(pStmt, collect);
}

GWBUF CachingParser::create_buffer(const std::string& statement) const
{
    return m_parser.create_buffer(statement);
}

std::string_view CachingParser::get_created_table_name(GWBUF* query) const
{
    QCInfoCacheScope scope(&m_plugin, query);
    return m_parser.get_created_table_name(query);
}

CachingParser::DatabaseNames CachingParser::get_database_names(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_database_names(pStmt);
}

void CachingParser::get_field_info(GWBUF* pStmt,
                                   const QC_FIELD_INFO** ppInfos,
                                   size_t* pnInfos) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    m_parser.get_field_info(pStmt, ppInfos, pnInfos);
}

void CachingParser::get_function_info(GWBUF* pStmt,
                                      const QC_FUNCTION_INFO** ppInfos,
                                      size_t* pnInfos) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    m_parser.get_function_info(pStmt, ppInfos, pnInfos);
}

QC_KILL CachingParser::get_kill_info(GWBUF* query) const
{
    QCInfoCacheScope scope(&m_plugin, query);
    return m_parser.get_kill_info(query);
}

qc_query_op_t CachingParser::get_operation(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_operation(pStmt);
}

uint32_t CachingParser::get_options() const
{
    return m_parser.get_options();
}

GWBUF* CachingParser::get_preparable_stmt(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_preparable_stmt(pStmt);
}

std::string_view CachingParser::get_prepare_name(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_prepare_name(pStmt);
}

uint64_t CachingParser::get_server_version() const
{
    return m_parser.get_server_version();
}

qc_sql_mode_t CachingParser::get_sql_mode() const
{
    return m_parser.get_sql_mode();
}

CachingParser::TableNames CachingParser::get_table_names(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_table_names(pStmt);
}

uint32_t CachingParser::get_trx_type_mask(GWBUF* pStmt) const
{
    return m_parser.get_trx_type_mask(pStmt);
}

uint32_t CachingParser::get_type_mask(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.get_type_mask(pStmt);
}

bool CachingParser::is_drop_table_query(GWBUF* pStmt) const
{
    QCInfoCacheScope scope(&m_plugin, pStmt);
    return m_parser.is_drop_table_query(pStmt);
}

bool CachingParser::set_options(uint32_t options)
{
    bool rv = m_parser.set_options(options);

    if (rv)
    {
        this_thread.options = options;
    }

    return rv;
}

void CachingParser::set_sql_mode(qc_sql_mode_t sql_mode)
{
    m_parser.set_sql_mode(sql_mode);
}

void CachingParser::set_server_version(uint64_t version)
{
    m_parser.set_server_version(version);
}

}