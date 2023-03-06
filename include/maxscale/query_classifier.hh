/*
 * Copyright (c) 2018 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-02-21
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#pragma once

#include <map>
#include <maxscale/ccdefs.hh>
#include <maxscale/qc_stmt_info.hh>
#include <maxscale/parser.hh>

/**
 * qc_init_kind_t specifies what kind of initialization should be performed.
 */
enum qc_init_kind_t
{
    QC_INIT_SELF   = 0x01,  /*< Initialize/finalize the query classifier itself. */
    QC_INIT_PLUGIN = 0x02,  /*< Initialize/finalize the plugin. */
    QC_INIT_BOTH   = 0x03
};

/**
 * QUERY_CLASSIFIER defines the object a query classifier plugin must
 * implement and return.
 *
 * To a user of the query classifier functionality, it can in general
 * be ignored.
 */
class QUERY_CLASSIFIER
{
public:
    /**
     * Called once to setup the query classifier
     *
     * @param sql_mode  The default sql mode.
     * @param args      The value of `query_classifier_args` in the configuration file.
     *
     * @return QC_RESULT_OK, if the query classifier could be setup, otherwise
     *         some specific error code.
     */
    virtual int32_t setup(qc_sql_mode_t sql_mode, const char* args) = 0;

    /**
     * Called once at process startup. Typically not required, as the standard module loader already
     * calls this function through the module interface.
     *
     * @return QC_RESULT_OK, if the process initialization succeeded.
     */
    virtual int32_t process_init(void) = 0;

    /**
     * Called once at process shutdown.
     */
    virtual void process_end(void) = 0;

    /**
     * Called once per each thread.
     *
     * @return QC_RESULT_OK, if the thread initialization succeeded.
     */
    virtual int32_t thread_init(void) = 0;

    /**
     * Called once when a thread finishes.
     */
    virtual void thread_end(void) = 0;

    /**
     * Called to explicitly parse a statement.
     *
     * @param stmt     The statement to be parsed.
     * @param collect  A bitmask of @c qc_collect_info_t values. Specifies what information
     *                 should be collected. Only a hint and must not restrict what information
     *                 later can be queried.
     * @param result   On return, the parse result, if @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t parse(GWBUF* stmt, uint32_t collect, int32_t* result) = 0;

    /**
     * Reports the type of the statement.
     *
     * @param stmt  A COM_QUERY or COM_STMT_PREPARE packet.
     * @param type  On return, the type mask (combination of @c qc_query_type_t),
     *              if @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_type_mask(GWBUF* stmt, uint32_t* type) = 0;

    /**
     * Reports the operation of the statement.
     *
     * @param stmt  A COM_QUERY or COM_STMT_PREPARE packet.
     * @param type  On return, the operation (one of @c qc_query_op_t), if
     *              @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_operation(GWBUF* stmt, int32_t* op) = 0;

    /**
     * Reports the name of a created table.
     *
     * @param stmt  A COM_QUERY or COM_STMT_PREPARE packet.
     * @param name  On return, the name of the created table, if
     *              @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_created_table_name(GWBUF* stmt, std::string_view* name) = 0;

    /**
     * Reports whether a statement is a "DROP TABLE ..." statement.
     *
     * @param stmt           A COM_QUERY or COM_STMT_PREPARE packet
     * @param is_drop_table  On return, non-zero if the statement is a DROP TABLE
     *                       statement, if @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t is_drop_table_query(GWBUF* stmt, int32_t* is_drop_table) = 0;

    /**
     * Returns all table names.
     *
     * @param stmt   A COM_QUERY or COM_STMT_PREPARE packet.
     * @param names  On return, the names of the statement, if @c QC_RESULT_OK
     *               is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_table_names(GWBUF* stmt, std::vector<QcTableName>* names) = 0;

    /**
     * Reports the database names.
     *
     * @param stmt   A COM_QUERY or COM_STMT_PREPARE packet.
     * @param names  On return, the database names, if
     *               @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_database_names(GWBUF* stmt, std::vector<std::string_view>* names) = 0;

    /**
     * Reports KILL information.
     *
     * @param stmt    A COM_QUERY or COM_STMT_PREPARE packet.
     * @param pKill   Pointer where the KILL information is stored.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource exhaustion or equivalent.
     */
    virtual int32_t get_kill_info(GWBUF* stmt, QC_KILL* pKill) = 0;

    /**
     * Reports the prepare name.
     *
     * @param stmt  A COM_QUERY or COM_STMT_PREPARE packet.
     * @param name  On return, the name of a prepare statement, if
     *              @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_prepare_name(GWBUF* stmt, std::string_view* name) = 0;

    /**
     * Reports field information.
     *
     * @param stmt    A COM_QUERY or COM_STMT_PREPARE packet.
     * @param infos   On return, array of field infos, if @c QC_RESULT_OK is returned.
     * @param n_infos On return, the size of @c infos, if @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_field_info(GWBUF* stmt, const QC_FIELD_INFO** infos, uint32_t* n_infos) = 0;

    /**
     * Reports function information.
     *
     * @param stmt    A COM_QUERY or COM_STMT_PREPARE packet.
     * @param infos   On return, array of function infos, if @c QC_RESULT_OK is returned.
     * @param n_infos On return, the size of @c infos, if @c QC_RESULT_OK is returned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_function_info(GWBUF* stmt, const QC_FUNCTION_INFO** infos, uint32_t* n_infos) = 0;

    /**
     * Return the preparable statement of a PREPARE statement.
     *
     * @param stmt             A COM_QUERY or COM_STMT_PREPARE packet.
     * @param preparable_stmt  On return, the preparable statement (provided @c stmt is a
     *                         PREPARE statement), if @c QC_RESULT_OK is returned. Otherwise
     *                         NULL.
     *
     * @attention The returned GWBUF is the property of @c stmt and will be deleted when
     *            @c stmt is. If the preparable statement need to be retained beyond the
     *            lifetime of @c stmt, it must be cloned.
     *
     * @return QC_RESULT_OK, if the parsing was not aborted due to resource
     *         exhaustion or equivalent.
     */
    virtual int32_t get_preparable_stmt(GWBUF* stmt, GWBUF** preparable_stmt) = 0;

    /**
     * Set the version of the server. The version may affect how a statement
     * is classified. Note that the server version is maintained separately
     * for each thread.
     *
     * @param version  Version encoded as MariaDB encodes the version, i.e.:
     *                 version = major * 10000 + minor * 100 + patch
     */
    virtual void set_server_version(uint64_t version) = 0;

    /**
     * Get the thread specific version assumed of the server. If the version has
     * not been set, all values are 0.
     *
     * @param version  The version encoded as MariaDB encodes the version, i.e.:
     *                 version = major * 10000 + minor * 100 + patch
     */
    virtual void get_server_version(uint64_t* version) = 0;

    /**
     * Gets the sql mode of the *calling* thread.
     *
     * @param sql_mode  The mode.
     *
     * @return QC_RESULT_OK
     */
    virtual int32_t get_sql_mode(qc_sql_mode_t* sql_mode) = 0;

    /**
     * Sets the sql mode for the *calling* thread.
     *
     * @param sql_mode  The mode.
     *
     * @return QC_RESULT_OK if @sql_mode is valid, otherwise QC_RESULT_ERROR.
     */
    virtual int32_t set_sql_mode(qc_sql_mode_t sql_mode) = 0;

    /**
     * Gets the options of the *calling* thread.
     *
     * @return Bit mask of values from qc_option_t.
     */
    virtual uint32_t get_options() = 0;

    /**
     * Sets the options for the *calling* thread.
     *
     * @param options Bits from qc_option_t.
     *
     * @return QC_RESULT_OK if @c options is valid, otherwise QC_RESULT_ERROR.
     */
    virtual int32_t set_options(uint32_t options) = 0;

    /**
     * Get result from info.
     *
     * @param  The info whose result should be returned.
     *
     * @return The result of the provided info.
     */
    virtual QC_STMT_RESULT get_result_from_info(const QC_STMT_INFO* info) = 0;

    /**
     * Return statement currently being classified.
     *
     * @param ppStmp  Pointer to pointer that on return will point to the
     *                statement being classified.
     * @param pLen    Pointer to value that on return will contain the length
     *                of the returned string.
     *
     * @return QC_RESULT_OK if a statement was returned (i.e. a statement is being
     *         classified), QC_RESULT_ERROR otherwise.
     */
    virtual int32_t get_current_stmt(const char** ppStmt, size_t* pLen) = 0;

    /**
     * Get canonical statement
     *
     * @param info  The info whose canonical statement should be returned.
     *
     * @attention - The string_view refers to data that remains valid only as long
     *              as @c info remains valid.
     *            - If @c info is of a COM_STMT_PREPARE, then the canonical string will
     *              be suffixed by ":P".
     *
     * @return The canonical statement.
     */
    virtual std::string_view info_get_canonical(const QC_STMT_INFO* info) = 0;
};

/**
 * Loads and sets up the default query classifier.
 *
 * This must be called once during the execution of a process. The query
 * classifier functions can only be used if this function first and thereafter
 * the @c qc_process_init return true.
 *
 * MaxScale calls this function, so plugins should not do that.
 *
 * @param cache_properties  If non-NULL, specifies the properties of the QC cache.
 * @param sql_mode          The default sql mode.
 * @param plugin_name       The name of the plugin from which the query classifier
 *                          should be loaded.
 * @param plugin_args       The arguments to be provided to the query classifier.
 *
 * @return True if the query classifier could be loaded and initialized,
 *         false otherwise.
 *
 * @see qc_process_init qc_thread_init
 */
// TODO: To be removed.
QUERY_CLASSIFIER* qc_setup(const QC_CACHE_PROPERTIES* cache_properties,
                           qc_sql_mode_t sql_mode,
                           const char* plugin_name,
                           const char* plugin_args);

bool qc_setup(const QC_CACHE_PROPERTIES* cache_properties);

/**
 * Loads and setups the default query classifier, and performs
 * process and thread initialization.
 *
 * This is primary intended for making the setup of stand-alone
 * test-programs simpler.
 *
 * @param cache_properties  If non-NULL, specifies the properties of the QC cache.
 * @param sql_mode          The default sql mode.
 * @param plugin_name       The name of the plugin from which the query classifier
 *                          should be loaded.
 * @param plugin_args       The arguments to be provided to the query classifier.
 *
 * @return True if the query classifier could be loaded and initialized,
 *         false otherwise.
 *
 * @see qc_end.
 */
QUERY_CLASSIFIER* qc_init(const QC_CACHE_PROPERTIES* cache_properties,
                          qc_sql_mode_t sql_mode,
                          const char* plugin_name,
                          const char* plugin_args);

/**
 * Performs thread and process finalization.
 *
 * This is primary intended for making the tear-down of stand-alone
 * test-programs simpler.
 */
void qc_end();

/**
 * Intializes the query classifier.
 *
 * This function should be called once, provided @c qc_setup returned true,
 * before the query classifier functionality is used.
 *
 * MaxScale calls this functions, so plugins should not do that.
 *
 * @param kind  What kind of initialization should be performed.
 *              Combination of qc_init_kind_t.
 *
 * @return True, if the process wide initialization could be performed.
 *
 * @see qc_process_end qc_thread_init
 */
bool qc_process_init(uint32_t kind);

/**
 * Finalizes the query classifier.
 *
 * A successful call of @c qc_process_init should before program exit be
 * followed by a call to this function. MaxScale calls this function, so
 * plugins should not do that.
 *
 * @param kind  What kind of finalization should be performed.
 *              Combination of qc_init_kind_t.
 *
 * @see qc_process_init qc_thread_end
 */
void qc_process_end(uint32_t kind);

/**
 * Loads a particular query classifier.
 *
 * In general there is no need to use this function, but rely upon qc_init().
 * However, if there is a need to use multiple query classifiers concurrently
 * then this function provides the means for that. Note that after a query
 * classifier has been loaded, it must explicitly be initialized before it
 * can be used.
 *
 * @param plugin_name  The name of the plugin from which the query classifier
 *                     should be loaded.
 *
 * @return A QUERY_CLASSIFIER object if successful, NULL otherwise.
 *
 * @see qc_unload
 */
QUERY_CLASSIFIER* qc_load(const char* plugin_name);

/**
 * Unloads an explicitly loaded query classifier.
 *
 * @see qc_load
 */
void qc_unload(QUERY_CLASSIFIER* classifier);

/**
 * Performs thread initialization needed by the query classifier. Should
 * be called in every thread.
 *
 * MaxScale calls this function, so plugins should not do that.
 *
 * @param kind  What kind of initialization should be performed.
 *              Combination of qc_init_kind_t.
 *
 * @return True if the initialization succeeded, false otherwise.
 *
 * @see qc_thread_end
 */
bool qc_thread_init(uint32_t kind);

/**
 * Performs thread finalization needed by the query classifier.
 * A successful call to @c qc_thread_init should at some point be
 * followed by a call to this function.
 *
 * MaxScale calls this function, so plugins should not do that.
 *
 * @param kind  What kind of finalization should be performed.
 *              Combination of qc_init_kind_t.
 *
 * @see qc_thread_init
 */
void qc_thread_end(uint32_t kind);

/**
 * Get cache statistics for the calling thread.
 *
 * @return An object if caching is enabled, NULL otherwise.
 */
json_t* qc_get_cache_stats_as_json();

/**
 * Return statement currently being classified.
 *
 * @param ppStmp  Pointer to pointer that on return will point to the
 *                statement being classified.
 * @param pLen    Pointer to value that on return will contain the length
 *                of the returned string.
 *
 * @return True, if a statement was returned (i.e. a statement is being
 *         classified), false otherwise.
 *
 * @note A string /may/ be returned /only/ when this function is called from
 *       a signal handler that is called due to the classifier causing
 *       a crash.
 */
bool qc_get_current_stmt(const char** ppStmt, size_t* pLen);

/**
 * Common query classifier properties as JSON.
 *
 * @param zHost  The MaxScale host.
 *
 * @return A json object containing properties.
 */
std::unique_ptr<json_t> qc_as_json(const char* zHost);

/**
 * Alter common query classifier properties.
 *
 * @param pJson  A JSON object.
 *
 * @return True, if the object was valid and parameters could be changed,
 *         false otherwise.
 */
bool qc_alter_from_json(json_t* pJson);

/**
 * Return query classifier cache content.
 *
 * @param zHost      The MaxScale host.
 *
 * @return A json object containing information about the query classifier cache.
 */
std::unique_ptr<json_t> qc_cache_as_json(const char* zHost);

/**
 * Classify statement
 *
 * @param zHost      The MaxScale host.
 * @param statement  The statement to be classified.
 *
 * @return A json object containing information about the statement.
 */
std::unique_ptr<json_t> qc_classify_as_json(const char* zHost, const std::string& statement);
