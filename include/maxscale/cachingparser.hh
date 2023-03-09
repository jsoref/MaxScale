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
#pragma once

#include <maxscale/ccdefs.hh>
#include <map>
#include <maxscale/parser.hh>

/**
 * QC_CACHE_STATS provides statistics of the cache.
 */
struct QC_CACHE_STATS
{
    int64_t size;       /** The current size of the cache. */
    int64_t inserts;    /** The number of inserts. */
    int64_t hits;       /** The number of hits. */
    int64_t misses;     /** The number of misses. */
    int64_t evictions;  /** The number of evictions. */
};

/**
 * Public interface to query classifier cache state.
 */
struct QC_CACHE_ENTRY
{
    int64_t        hits;
    QC_STMT_RESULT result;
};

/**
 * QC_CACHE_PROPERTIES specifies the limits of the query classification cache.
 */
struct QC_CACHE_PROPERTIES
{
    int64_t max_size;   /** The maximum size of the cache. */
};

namespace maxscale
{

class CachingParser : public mxs::Parser
{
public:
    CachingParser(const CachingParser&) = delete;
    CachingParser& operator = (const CachingParser&) = delete;

    static void thread_init();
    static void thread_finish();

    static bool                    set_properties(const QC_CACHE_PROPERTIES& properties);
    static void                    get_properties(QC_CACHE_PROPERTIES* pProperties);

    static bool                    set_properties(json_t* pJson);
    static std::unique_ptr<json_t> get_properties_as_resource(const char* zHost);

    static int64_t                 clear_thread_cache();
    static void                    get_thread_cache_state(std::map<std::string, QC_CACHE_ENTRY>& state);
    static bool                    get_thread_cache_stats(QC_CACHE_STATS* pStats);
    static std::unique_ptr<json_t> get_thread_cache_stats_as_json();
    static void                    set_thread_cache_enabled(bool enable);

    static std::unique_ptr<json_t> content_as_resource(const char* zHost);


    Plugin& plugin() const override;

    qc_parse_result_t parse(GWBUF* pStmt, uint32_t collect) const override;

    GWBUF            create_buffer(const std::string& statement) const override;
    std::string_view get_created_table_name(GWBUF* pStmt) const override;
    DatabaseNames    get_database_names(GWBUF* pStmt) const override;
    void             get_field_info(GWBUF* pStmt,
                                    const QC_FIELD_INFO** ppInfos,
                                    size_t* pnInfos) const override;
    void             get_function_info(GWBUF* pStmt,
                                       const QC_FUNCTION_INFO** infos,
                                       size_t* n_infos) const override;
    QC_KILL          get_kill_info(GWBUF* pStmt) const override;
    qc_query_op_t    get_operation(GWBUF* pStmt) const override;
    uint32_t         get_options() const override;
    GWBUF*           get_preparable_stmt(GWBUF* pStmt) const override;
    std::string_view get_prepare_name(GWBUF* pStmt) const override;
    uint64_t         get_server_version() const override;
    qc_sql_mode_t    get_sql_mode() const override;
    TableNames       get_table_names(GWBUF* pStmt) const override;
    uint32_t         get_trx_type_mask(GWBUF* pStmt) const override;
    uint32_t         get_type_mask(GWBUF* pStmt) const override;
    bool             is_drop_table_query(GWBUF* pStmt) const override;

    bool set_options(uint32_t options) override;
    void set_sql_mode(qc_sql_mode_t sql_mode) override;
    void set_server_version(uint64_t version) override;

protected:
    CachingParser(Plugin* pPlugin);

    Plugin&      m_plugin;
    mxs::Parser& m_parser;
};

}