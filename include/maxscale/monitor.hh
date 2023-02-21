/*
 * Copyright (c) 2018 MariaDB Corporation Ab
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
#pragma once

/**
 * @file include/maxscale/monitor.hh - The public monitor interface
 */

#include <maxscale/ccdefs.hh>

#include <atomic>
#include <mutex>
#include <openssl/sha.h>
#include <maxbase/json.hh>
#include <maxbase/semaphore.hh>
#include <maxbase/stopwatch.hh>
#include <maxbase/worker.hh>
#include <maxscale/config.hh>
#include <maxscale/server.hh>

namespace maxscale
{
class Monitor;
}

class DCB;
struct json_t;
class ExternalCmd;
typedef struct st_mysql MYSQL;

/**
 * @verbatim
 * The "module object" structure for a backend monitor module
 *
 * Monitor modules monitor the backend databases that MaxScale connects to.
 * The information provided by a monitor is used in routing decisions.
 * @endverbatim
 *
 * @see load_module
 */
struct MXS_MONITOR_API
{
    /**
     * @brief Create the monitor.
     *
     * This entry point is called once when MaxScale is started, for creating the monitor.
     * If the function fails, MaxScale will not start. The returned object must inherit from
     * the abstract base monitor class and implement the missing methods.
     *
     * @param name Configuration name of the monitor
     * @param module Module name of the monitor
     * @return Monitor object
     */
    maxscale::Monitor* (* createInstance)(const std::string& name, const std::string& module);
};

/**
 * Monitor configuration parameters names
 */
extern const char CN_BACKEND_CONNECT_ATTEMPTS[];
extern const char CN_BACKEND_CONNECT_TIMEOUT[];
extern const char CN_BACKEND_READ_TIMEOUT[];
extern const char CN_BACKEND_WRITE_TIMEOUT[];
extern const char CN_DISK_SPACE_CHECK_INTERVAL[];
extern const char CN_EVENTS[];
extern const char CN_JOURNAL_MAX_AGE[];
extern const char CN_MONITOR_INTERVAL[];
extern const char CN_SCRIPT[];
extern const char CN_SCRIPT_TIMEOUT[];

/**
 * The monitor API version number. Any change to the monitor module API
 * must change these versions using the rules defined in modinfo.h
 */
#define MXS_MONITOR_VERSION {5, 0, 0}

/** Monitor events */
enum mxs_monitor_event_t
{
    UNDEFINED_EVENT   = 0,
    MASTER_DOWN_EVENT = (1 << 0),   /**< master_down */
    MASTER_UP_EVENT   = (1 << 1),   /**< master_up */
    SLAVE_DOWN_EVENT  = (1 << 2),   /**< slave_down */
    SLAVE_UP_EVENT    = (1 << 3),   /**< slave_up */
    SERVER_DOWN_EVENT = (1 << 4),   /**< server_down */
    SERVER_UP_EVENT   = (1 << 5),   /**< server_up */
    SYNCED_DOWN_EVENT = (1 << 6),   /**< synced_down */
    SYNCED_UP_EVENT   = (1 << 7),   /**< synced_up */
    DONOR_DOWN_EVENT  = (1 << 8),   /**< donor_down */
    DONOR_UP_EVENT    = (1 << 9),   /**< donor_up */
    LOST_MASTER_EVENT = (1 << 10),  /**< lost_master */
    LOST_SLAVE_EVENT  = (1 << 11),  /**< lost_slave */
    LOST_SYNCED_EVENT = (1 << 12),  /**< lost_synced */
    LOST_DONOR_EVENT  = (1 << 13),  /**< lost_donor */
    NEW_MASTER_EVENT  = (1 << 14),  /**< new_master */
    NEW_SLAVE_EVENT   = (1 << 15),  /**< new_slave */
    NEW_SYNCED_EVENT  = (1 << 16),  /**< new_synced */
    NEW_DONOR_EVENT   = (1 << 17),  /**< new_donor */
    RELAY_UP_EVENT    = (1 << 18),  /**< relay_up */
    RELAY_DOWN_EVENT  = (1 << 19),  /**< relay_down */
    LOST_RELAY_EVENT  = (1 << 20),  /**< lost_relay */
    NEW_RELAY_EVENT   = (1 << 21),  /**< new_relay */
    BLR_UP_EVENT      = (1 << 22),  /**< blr_up */
    BLR_DOWN_EVENT    = (1 << 23),  /**< blr_down */
    LOST_BLR_EVENT    = (1 << 24),  /**< lost_blr */
    NEW_BLR_EVENT     = (1 << 25),  /**< new_blr */
    ALL_EVENTS        =
        (MASTER_DOWN_EVENT | MASTER_UP_EVENT | SLAVE_DOWN_EVENT | SLAVE_UP_EVENT | SERVER_DOWN_EVENT
         | SERVER_UP_EVENT | SYNCED_DOWN_EVENT | SYNCED_UP_EVENT | DONOR_DOWN_EVENT | DONOR_UP_EVENT
         | LOST_MASTER_EVENT | LOST_SLAVE_EVENT | LOST_SYNCED_EVENT | LOST_DONOR_EVENT | NEW_MASTER_EVENT
         | NEW_SLAVE_EVENT | NEW_SYNCED_EVENT | NEW_DONOR_EVENT | RELAY_UP_EVENT | RELAY_DOWN_EVENT
         | LOST_RELAY_EVENT | NEW_RELAY_EVENT | BLR_UP_EVENT | BLR_DOWN_EVENT | LOST_BLR_EVENT | NEW_BLR_EVENT),
};

namespace maxscale
{

/**
 * The linked list of servers that are being monitored by the monitor module.
 */
class MonitorServer
{
public:
    class ConnectionSettings
    {
    public:
        using seconds = std::chrono::seconds;

        std::string username;       /**< Monitor username */
        std::string password;       /**< Monitor password */
        seconds     connect_timeout;/**< Connect timeout in seconds for mysql_real_connect */
        seconds     write_timeout;  /**< Timeout in seconds for each attempt to write to the server.
                                     *   There are retries and the total effective timeout value is two
                                     *   times the option value. */
        seconds read_timeout;       /**< Timeout in seconds to read from the server. There are retries
                                     *   and the total effective timeout value is three times the
                                     *   option value. */
        int64_t connect_attempts;   /**< How many times a connection is attempted */
    };

    /**
     * Container shared between the monitor and all its servers. May be read concurrently, but only
     * written when monitor is stopped.
     */
    class SharedSettings
    {
    public:
        ConnectionSettings conn_settings;       /**< Monitor-level connection settings */
        DiskSpaceLimits    monitor_disk_limits; /**< Monitor-level disk space limits */
    };

    /* Return type of mon_ping_or_connect_to_db(). */
    enum class ConnectResult
    {
        OLDCONN_OK,     /* Existing connection was ok and server replied to ping. */
        NEWCONN_OK,     /* No existing connection or no ping reply. New connection created
                         * successfully. */
        REFUSED,        /* No existing connection or no ping reply. Server refused new connection. */
        TIMEOUT,        /* No existing connection or no ping reply. Timeout on new connection. */
        ACCESS_DENIED   /* Server refused new connection due to authentication failure */
    };

    /** Status change requests */
    enum StatusRequest
    {
        NO_CHANGE,
        MAINT_OFF,
        MAINT_ON,
        DRAINING_OFF,
        DRAINING_ON,
    };

    // When a monitor detects that a server is down, these bits should be cleared.
    static constexpr uint64_t SERVER_DOWN_CLEAR_BITS {SERVER_RUNNING | SERVER_AUTH_ERROR | SERVER_MASTER
                                                      | SERVER_SLAVE | SERVER_RELAY | SERVER_JOINED
                                                      | SERVER_BLR};

    /**
     * Ping or connect to a database. If connection does not exist or ping fails, a new connection
     * is created. This will always leave a valid database handle in @c *ppCon, allowing the user
     * to call MySQL C API functions to find out the reason of the failure. Also measures server ping.
     *
     * @param sett        Connection settings
     * @param pServer     A server
     * @param ppConn      Address of pointer to a MYSQL instance. The instance should either be
     *                    valid or NULL.
     * @param pError      Pointer where the error message is stored
     *
     * @return Connection status.
     */
    static ConnectResult
    ping_or_connect_to_db(const ConnectionSettings& sett, SERVER& server, MYSQL** ppConn,
                          std::string* pError);

    MonitorServer(SERVER* server, const SharedSettings& shared);

    virtual ~MonitorServer();

    /**
     * Set pending status bits in the monitor server
     *
     * @param bits      The bits to set for the server
     */
    void set_pending_status(uint64_t bits);

    /**
     * Clear pending status bits in the monitor server
     *
     * @param bits      The bits to clear for the server
     */
    void clear_pending_status(uint64_t bits);

    /**
     * Store the current server status to the previous and pending status
     * fields of the monitored server.
     */
    void stash_current_status();

    static bool status_changed(uint64_t before, uint64_t after);

    bool        status_changed();
    bool        auth_status_changed();
    bool        should_print_fail_status();
    std::string get_connect_error(ConnectResult rval);
    void        log_connect_error(ConnectResult rval);

    /**
     * Report query error to log.
     */
    void mon_report_query_error();

    /**
     * Ping or connect to a database. If connection does not exist or ping fails, a new connection is created.
     * This will always leave a valid database handle in the database->con pointer, allowing the user to call
     * MySQL C API functions to find out the reason of the failure.
     *
     * @return Connection status
     */
    ConnectResult ping_or_connect();

    /**
     * Fetch 'session_track_system_variables' and other variables from the server, if they have not
     * been fetched recently.
     *
     * @return  True, if the variables were fetched, false otherwise.
     */
    bool maybe_fetch_variables();

    /**
     * Update the Uptime status variable of the server
     */
    void fetch_uptime();

    const char* get_event_name();

    /**
     * Determine a monitor event, defined by the difference between the old
     * status of a server and the new status.
     *
     * @return The event for this state change
     */
    static mxs_monitor_event_t event_type(uint64_t before, uint64_t after);

    /**
     * Calls event_type with previous and current server state
     *
     * @note This function must only be called from mon_process_state_changes
     */
    mxs_monitor_event_t get_event_type() const;

    void log_state_change(const std::string& reason);

    /**
     * Is this server ok to update disk space status. Only checks if the server knows of valid disk space
     * limits settings and that the check has not failed before. Disk space check interval should be
     * checked by the monitor.
     *
     * @return True, if the disk space should be checked, false otherwise.
     */
    bool can_update_disk_space_status() const;

    /**
     * @brief Update the disk space status of a server.
     *
     * After the call, the bit @c SERVER_DISK_SPACE_EXHAUSTED will be set on
     * @c pMonitored_server->pending_status if the disk space is exhausted
     * or cleared if it is not.
     */
    void update_disk_space_status();

    void add_status_request(StatusRequest request);
    void apply_status_requests();

    bool is_database() const;

    mxb::Json journal_data() const;
    void      read_journal_data(const mxb::Json& data);

    using EventList = std::vector<std::string>;

    /**
     * If a monitor module implements custom events, it should override this function so that it returns
     * a list of new events for the current tick. The list should be cleared at the start of a tick.
     *
     * The default implementation returns an empty list.
     *
     * @return New custom events
     */
    virtual const EventList& new_custom_events() const;

    const ConnectionSettings& conn_settings() const;

    static bool is_access_denied_error(int64_t errornum);

    SERVER* server = nullptr;       /**< The server being monitored */
    MYSQL*  con = nullptr;          /**< The MySQL connection */
    int     mon_err_count = 0;

    uint64_t mon_prev_status = -1;      /**< Status before starting the current monitor loop */
    uint64_t pending_status = 0;        /**< Status during current monitor loop */

    int64_t node_id = -1;           /**< Node id, server_id for M/S or local_index for Galera */
    int64_t master_id = -1;         /**< Master server id of this node */

    mxs_monitor_event_t last_event {SERVER_DOWN_EVENT}; /**< The last event that occurred on this server */
    time_t              triggered_at {time(nullptr)};   /**< Time when the last event was triggered */

private:
    const SharedSettings& m_shared;     /**< Settings shared between all servers of the monitor */

    std::atomic_int m_status_request {NO_CHANGE};       /**< Status change request from admin */
    bool            m_ok_to_check_disk_space {true};    /**< Set to false if check fails */

    mxb::TimePoint m_last_variables_update;

    // Latest connection error
    std::string m_latest_error;

    bool should_fetch_variables();
    bool fetch_variables();
};

/**
 * Representation of the running monitor.
 */
class Monitor
{
public:
    class Test;
    friend class Test;

    using ServerVector = std::vector<MonitorServer*>;

    Monitor(const std::string& name, const std::string& module);
    virtual ~Monitor();

    static bool connection_is_ok(MonitorServer::ConnectResult connect_result);

    static std::string get_server_monitor(const SERVER* server);

    /**
     * Is the current thread/worker the main worker?
     *
     * @return True if it is, false otherwise.
     */
    static bool is_main_worker();

    /*
     * Convert a monitor event (enum) to string.
     *
     * @param   event    The event
     * @return  Text description
     */
    static const char* get_event_name(mxs_monitor_event_t event);

    /**
     * Is the monitor running?
     *
     * @return True if monitor is running.
     */
    virtual bool is_running() const = 0;

    /**
     * Get running state as string.
     *
     * @return "Running" or "Stopped"
     */
    const char* state_string() const;

    const char* name() const;

    /**
     * Get the configured servers for this monitor
     *
     * @return The list of servers the monitor was configured with
     */
    const ServerVector& servers() const;

    /**
     * Get the real list of servers that are a part of this cluster
     *
     * For dynamic monitors, this is the set of servers that were derived from the initial set of bootstrap
     * servers. For static monitors, this is the same as the list of servers returned by servers().
     *
     * @return The real list of servers that are a part of this cluster. This should be used whenever a set of
     *         servers is needed for routing or querying purposes.
     */
    virtual std::vector<SERVER*> real_servers() const;

    /**
     * Get the list of servers that were configured for this monitor
     *
     * This list is identical to the one given as the `servers` parameter in the configuration file or the
     * `servers` relationship in the JSON representation. For dynamic monitors, this list of servers is not
     * necessarily actively monitored if they are only used to bootstrap the cluster.
     *
     * @return The list of servers this monitor was configured with.
     */
    std::vector<SERVER*> configured_servers() const;

    /**
     * Specification for the common monitor parameters
     */
    static mxs::config::Specification* specification();

    mxs::config::Configuration& base_configuration();

    virtual mxs::config::Configuration& configuration() = 0;

    /**
     * Get text-form settings.
     *
     * @return Monitor configuration parameters
     */
    const mxs::ConfigParameters& parameters() const;

    /**
     * @return The number of monitoring cycles the monitor has done
     */
    long ticks() const;

    /**
     * Starts the monitor. If the monitor requires polling of the servers, it should create
     * a separate monitoring thread.
     *
     * @return True, if the monitor could be started, false otherwise.
     */
    virtual bool start() = 0;

    /**
     * Stops the monitor.
     */
    void stop();

    /**
     * Stop a monitor if it's safe to do so.
     *
     * @return Boolean tells if monitor was stopped. If not, an error message is given.
     */
    std::tuple<bool, std::string> soft_stop();

    virtual void request_immediate_tick() = 0;

    /**
     * @brief The monitor should populate associated services.
     */
    virtual void populate_services();

    /**
     * Deactivate the monitor. Stops the monitor and removes all servers.
     */
    void deactivate();

    json_t* to_json(const char* host) const;

    /**
     * Return diagnostic information about the monitor
     *
     * @return A JSON object representing the state of the monitor
     * @see jansson.h
     */
    virtual json_t* diagnostics() const = 0;

    /**
     * Return diagnostic information about a server monitored by the monitor
     *
     * @return A JSON object representing the detailed server information
     *
     * @note This is combined with the existing "public" server information found in the Server class.
     */
    virtual json_t* diagnostics(MonitorServer* server) const = 0;

    /**
     * Set status of monitored server.
     *
     * @param srv   Server, must be monitored by this monitor.
     * @param bit   The server status bit to be sent.
     * @errmsg_out  If the setting of the bit fails, on return the human readable
     *              reason why it could not be set.
     *
     * @return True, if the bit could be set.
     */
    bool set_server_status(SERVER* srv, int bit, std::string* errmsg_out);

    /**
     * Clear status of monitored server.
     *
     * @param srv   Server, must be monitored by this monitor.
     * @param bit   The server status bit to be cleared.
     * @errmsg_out  If the clearing of the bit fails, on return the human readable
     *              reason why it could not be cleared.
     *
     * @return True, if the bit could be cleared.
     */
    bool clear_server_status(SERVER* srv, int bit, std::string* errmsg_out);

    json_t* monitored_server_json_attributes(const SERVER* srv) const;

    /**
     * Check if monitor owns the cluster
     *
     * The monitor that owns is the one who decides the state of the servers in a multi-MaxScale cluster.
     * Currently only mariadbmon implements cooperative monitoring.
     *
     * The default implementation always returns true.
     *
     * @return True if this monitor owns and controls the cluster.
     */
    virtual bool is_cluster_owner() const
    {
        return true;
    }

    /**
     * Check if monitor is dynamic
     *
     * A dynamic monitor only uses the servers specified in the configuration as "bootstrap"
     * servers, that is, for connecting to the cluster. The monitor will create a volatile
     * server instance for each server in the cluster.
     */
    virtual bool is_dynamic() const
    {
        return false;
    }

    const std::string m_name;           /**< Monitor instance name. */
    const std::string m_module;         /**< Name of the monitor module */

    json_t* parameters_to_json() const;

protected:
    /**
     * Stop the monitor. If the monitor uses a polling thread, the thread should be stopped.
     */
    virtual void do_stop() = 0;

    /**
     * Subclass-specific soft-stop.
     *
     * @return True if success. On fail, also return an error message.
     */
    virtual std::tuple<bool, std::string> do_soft_stop() = 0;

    /**
     * Check if the monitor user can execute a query. The query should be such that it only succeeds if
     * the monitor user has all required permissions. Servers which are down are skipped.
     *
     * @param query Query to test with
     * @return True on success, false if monitor credentials lack permissions
     */
    bool test_permissions(const std::string& query);

    /**
     * Detect and handle state change events. This function should be called by all monitors at the end
     * of each monitoring cycle. The function logs state changes and executes the monitor script on
     * servers whose status changed.
     */
    void detect_handle_state_changes();

    /**
     * Remove old format journal file if it exists. Remove this function in MaxScale 2.7.
     */
    void remove_old_journal();

    /**
     * @brief Called when a server has been added to the monitor.
     *
     * The default implementation will add the server to associated
     * services.
     *
     * @param server  A server.
     */
    virtual void server_added(SERVER* server);

    /**
     * @brief Called when a server has been removed from the monitor.
     *
     * The default implementation will remove the server from associated
     * services.
     *
     * @param server  A server.
     */
    virtual void server_removed(SERVER* server);

    /**
     * Transform the list of normal servers into their monitored counterpart
     *
     * @param servers The servers to transform
     * @return True on success and the monitored servers, false if one or more of the servers is not monitored
     *         by this monitor
     */
    std::pair<bool, std::vector<MonitorServer*>>
    get_monitored_serverlist(const std::vector<SERVER*>& servers);

    /**
     * Find the monitored server representing the server.
     *
     * @param search_server Server to search for
     * @return Found monitored server or NULL if not found
     */
    MonitorServer* get_monitored_server(SERVER* search_server);

    /**
     * Check if admin is requesting setting or clearing maintenance status on the server and act accordingly.
     * Should be called at the beginning of a monitor loop.
     */
    void check_maintenance_requests();

    /**
     * @brief Hangup connections to failed servers
     *
     * Injects hangup events for DCB that are connected to servers that are down.
     */
    void hangup_failed_servers();

    MonitorServer* find_parent_node(MonitorServer* target);

    std::string child_nodes(MonitorServer* parent);

    /**
     * Checks if it's time to check disk space. If true is returned, the internal timer is reset
     * so that the next true is only returned once disk_space_check_interval has again passed.
     *
     * @return True if disk space should be checked
     */
    bool check_disk_space_this_tick();

    bool server_status_request_waiting() const;

    /**
     * Returns the human-readable reason why the server changed state
     *
     * @param server The server that changed state
     *
     * @return The human-readable reason why the state change occurred or
     *         an empty string if no information is available
     */
    virtual std::string annotate_state_change(mxs::MonitorServer* server)
    {
        return "";
    }

    /**
     * Contains monitor base class settings. Since monitors are stopped before a setting change,
     * the items cannot be modified while a monitor is running. No locking required.
     */
    class Settings : public mxs::config::Configuration
    {
    public:
        using seconds = std::chrono::seconds;
        using milliseconds = std::chrono::milliseconds;

        Settings(const std::string& name, Monitor* monitor);

        bool post_configure(const std::map<std::string, mxs::ConfigParameters>& nested_params) override final;


        std::string          type;      // Always "monitor"
        const MXS_MODULE*    module;    // The monitor module
        std::vector<SERVER*> servers;   // The configured servers

        milliseconds interval;          /**< Monitor interval in milliseconds */
        std::string  script;            /**< Script triggered by events */
        seconds      script_timeout;    /**< Timeout in seconds for the monitor scripts */
        uint32_t     events;            /**< Bitfield of events which trigger the script */
        seconds      journal_max_age;   /**< Maximum age of journal file */

        // The disk space threshold, in string form (TODO: add custom data type)
        std::string disk_space_threshold;
        // How often should a disk space check be made at most.
        milliseconds disk_space_check_interval;

        // TODO: Either add arbitrarily deep nesting of structs in Configurations or separate these into
        // something else. Now the values are stored twice.
        MonitorServer::ConnectionSettings conn_settings;

        // Settings shared between all servers of the monitor.
        MonitorServer::SharedSettings shared;

    private:
        Monitor* m_monitor;
    };

    const Settings&                          settings() const;
    const MonitorServer::ConnectionSettings& conn_settings() const;

    /**< Number of monitor ticks ran. Derived classes should increment this whenever completing a tick. */
    std::atomic_long m_ticks {0};

protected:
    /**
     * Can a server be disabled, that is, set to maintenance or draining mode.
     *
     * @param server      A server being monitored by this monitor.
     * @param type        Type of disabling attempted.
     * @param errmsg_out  If cannot be, on return explanation why.
     *
     * @return True, if the server can be disabled, false otherwise.
     *
     * @note The default implementation return true.
     */
    enum class DisableType
    {
        MAINTENANCE,
        DRAIN,
    };
    virtual bool can_be_disabled(const MonitorServer& server, DisableType type,
                                 std::string* errmsg_out) const;

    /**
     * Read monitor journal from json file.
     */
    void read_journal();

    /**
     * Write monitor journal to json file
     */
    void write_journal();

    /**
     * Write monitor journal if it needs updating.
     */
    void write_journal_if_needed();

    /**
     * Call when journal needs updating.
     */
    void request_journal_update();

    bool post_configure();

    friend bool Settings::post_configure(const std::map<std::string, mxs::ConfigParameters>& nested_params);

private:
    /**
     * Creates a new monitored server object. Called by monitor configuration code. If a monitor wants to
     * implements its own server-class, it must override this function.
     *
     * @param server The base server object
     * @param shared Base class settings shared with servers
     * @return A new monitored server
     */
    virtual MonitorServer* create_server(SERVER* server, const MonitorServer::SharedSettings& shared);

    /**
     * A derived class should override this function if it wishes to save its own journal data.
     * This is called when saving the monitor journal.
     *
     * @param data Journal data with base class fields
     */
    virtual void save_monitor_specific_journal_data(mxb::Json& data);

    /**
     * A derived class should override this function if it wishes to load its own journal data.
     * This is called when loading the monitor journal.
     *
     * @param data Json from journal file
     */
    virtual void load_monitor_specific_journal_data(const mxb::Json& data);

    bool add_server(SERVER* server);
    void remove_all_servers();

    /**
     * Launch a command. All default script variables will be replaced.
     *
     * @param ptr  The server which has changed state
     * @return Return value of the executed script or -1 on error.
     */
    int launch_command(MonitorServer* ptr, const std::string& event_name);

    enum class CredentialsApproach
    {
        INCLUDE,
        EXCLUDE,
    };

    /**
     * Create a list of server addresses and ports.
     *
     * @param status Server status bitmask. At least one bit must match with a server for it to be included
     * in the resulting list. 0 allows all servers regardless of status.
     * @param approach Whether credentials should be included or not.
     * @return Comma-separated list
     */
    std::string gen_serverlist(int status, CredentialsApproach approach = CredentialsApproach::EXCLUDE);

    // Waits until the status change request is processed
    void wait_for_status_change();

    mxb::StopWatch   m_disk_space_checked;              /**< When was disk space checked the last time */
    std::atomic_bool m_status_change_pending {false};   /**< Set when admin requests a status change. */

    /**
     * Has something changed such that journal needs to be updated. This is separate from the time-based
     * condition. */
    bool   m_journal_update_needed {true};
    time_t m_journal_updated {0};               /**< When was journal last updated? */
    time_t m_journal_max_save_interval {5 * 60};/**< How often to update journal at minimum */

    std::unique_ptr<ExternalCmd> m_scriptcmd;   /**< External command representing the monitor script */

    ServerVector          m_servers;    /**< Monitored servers */
    mxs::ConfigParameters m_parameters; /**< Configuration parameters in text form */
    Settings              m_settings;   /**< Base class settings */

    std::string journal_filepath() const;
};

/**
 * An abstract class which helps implement a monitor based on a maxbase::Worker thread.
 */
class MonitorWorker : public Monitor
                    , protected maxbase::Worker
{
public:
    MonitorWorker(const MonitorWorker&) = delete;
    MonitorWorker& operator=(const MonitorWorker&) = delete;

    ~MonitorWorker();

    bool is_running() const override final;

    /**
     * @brief Starts the monitor.
     *
     * - Calls @c has_sufficient_permissions(), if it has not been done earlier.
     * - Updates the 'script' and 'events' configuration parameters.
     * - Starts the monitor thread.
     *
     * - Once the monitor thread starts, it will
     *   - Load the server journal and update @c m_master.
     *   - Call @c pre_loop().
     *   - Enter a loop where it, until told to shut down, will
     *     - Check whether there are maintenance requests.
     *     - Call @c tick().
     *     - Call @c process_state_changes()
     *     - Hang up failed servers.
     *     - Store the server journal (@c m_master assumed to reflect the current situation).
     *     - Sleep until time for next @c tick().
     *   - Call @c post_loop().
     *
     * @return True, if the monitor started, false otherwise.
     */
    bool start() override final;

    void request_immediate_tick() override;

    /**
     * @brief Obtain diagnostics
     *
     * The implementation should create a JSON object and fill it with diagnostics
     * information. The default implementation returns an object that is populated
     * with the keys 'script' and 'events' if they have been set, otherwise the
     * object is empty.
     *
     * @return An object, if there is information to return, NULL otherwise.
     */
    json_t* diagnostics() const override;

    /**
     * Obtain diagnostics about a monitored server
     *
     * The implementation should return a JSON object with detailed diagnostic information that is amended to
     * the general server diagnostic information. For example, mariadbmon would return replication
     * information.
     *
     * The default implementation returns an empty JSON object.
     *
     * @return JSON object that describes the server or NULL if no information is available
     */
    json_t* diagnostics(MonitorServer* server) const override;

    /**
     * Get current time from the monotonic clock.
     *
     * @return Current time
     */
    static int64_t get_time_ms();

protected:
    MonitorWorker(const std::string& name, const std::string& module);

    void do_stop() override final;

    std::tuple<bool, std::string> do_soft_stop() override;

    /**
     * @brief Check whether the monitor has sufficient rights
     *
     * The implementation should check whether the monitor user has sufficient
     * rights to access the servers. The default implementation returns True.
     *
     * @return True, if the monitor user has sufficient rights, false otherwise.
     */
    virtual bool has_sufficient_permissions();

    /**
     * @brief Flush pending server status to each server.
     *
     * This function is expected to flush the pending status to each server.
     * The default implementation simply copies monitored_server->pending_status
     * to server->status.
     */
    virtual void flush_server_status();

    /**
     * @brief Monitor the servers
     *
     * This function is called once per monitor round, and the concrete
     * implementation should probe all servers and set server status bits.
     */
    virtual void tick() = 0;

    /**
     * @brief Called before the monitor loop is started
     *
     * The default implementation does nothing.
     */
    virtual void pre_loop();

    /**
     * @brief Called after the monitor loop has ended.
     *
     * The default implementation does nothing.
     */
    virtual void post_loop();

    /**
     * @brief Called after tick returns
     *
     * The default implementation will call @Monitor::detect_state_changes. Overriding functions
     * should do the same before proceeding with their own processing.
     */
    virtual void process_state_changes();

    /**
     * Should a monitor tick be ran immediately? The base class version always returns false. A monitor can
     * override this to add specific conditions. This function is called every MXS_MON_BASE_INTERVAL_MS
     * (100 ms) by the monitor worker thread, which then runs a monitor tick if true is returned.
     *
     * @return True if tick should be ran
     */
    virtual bool immediate_tick_required();

    Worker::Callable  m_callable;       /**< Context for own dcalls */
    std::atomic<bool> m_thread_running; /**< Thread state. Only visible inside MonitorInstance. */

private:
    bool           m_checked;       /**< Whether server access has been checked. */
    mxb::Semaphore m_semaphore;     /**< Semaphore for synchronizing with monitor thread. */
    int64_t        m_loop_called;   /**< When was the loop called the last time. */

    std::atomic_bool m_immediate_tick_requested {false};    /**< Should monitor tick immediately? */

    bool pre_run() override final;
    void post_run() override final;

    bool call_run_one_tick();
    void run_one_tick();
};

class MonitorWorkerSimple : public MonitorWorker
{
public:
    MonitorWorkerSimple(const MonitorWorkerSimple&) = delete;
    MonitorWorkerSimple& operator=(const MonitorWorkerSimple&) = delete;

protected:
    MonitorWorkerSimple(const std::string& name, const std::string& module)
        : MonitorWorker(name, module)
    {
    }

    /**
     * @brief Update server information
     *
     * The implementation should probe the server in question and update
     * the server status bits.
     */
    virtual void update_server_status(MonitorServer* pMonitored_server) = 0;

    /**
     * @brief Called right at the beginning of @c tick().
     *
     * The default implementation does nothing.
     */
    virtual void pre_tick();

    /**
     * @brief Called right before the end of @c tick().
     *
     * The default implementation does nothing.
     */
    virtual void post_tick();

    MonitorServer* m_master {nullptr};      /**< Master server */

protected:
    /**
     * A derived class overriding this function should first call this base version.
     */
    void pre_loop() override;

    /**
     * A derived class overriding this function should last call this base version.
     */
    void post_loop() override;

private:
    /**
     * @brief Monitor the servers
     *
     * This function is called once per monitor round. It does the following:
     * - Perform any maintenance or drain state changes requested by user
     *
     * -Then, for each server:
     *
     *   - Do nothing, if the server is in maintenance.
     *   - Store the previous status of the server.
     *   - Set the pending status of the monitored server object
     *     to the status of the corresponding server object.
     *   - Ensure that there is a connection to the server.
     *     If there is, @c update_server_status() is called.
     *     If there is not, the pending status will be updated accordingly and
     *     @c update_server_status() will *not* be called.
     *   - After the call, update the error count of the server if it is down.
     *
     * - Flush states for all servers
     * - Launch monitor scripts for events
     * - Hangup failed servers
     * - Store monitor journal
     */
    void tick() override final;
};

/**
 * The purpose of the template MonitorApi is to provide an implementation
 * of the monitor C-API. The template is instantiated with a class that
 * provides the actual behaviour of a monitor.
 */
template<class MonitorInstance>
class MonitorApi
{
public:
    MonitorApi() = delete;
    MonitorApi(const MonitorApi&) = delete;
    MonitorApi& operator=(const MonitorApi&) = delete;

    static Monitor* createInstance(const std::string& name, const std::string& module)
    {
        MonitorInstance* pInstance = NULL;
        MXS_EXCEPTION_GUARD(pInstance = MonitorInstance::create(name, module));
        return pInstance;
    }

    static MXS_MONITOR_API s_api;
};

template<class MonitorInstance>
MXS_MONITOR_API MonitorApi<MonitorInstance>::s_api =
{
    &MonitorApi<MonitorInstance>::createInstance,
};
}

/**
 * This helper class exposes some monitor private functions. Should be used with test code.
 */
class mxs::Monitor::Test
{
protected:
    explicit Test(mxs::Monitor* monitor);
    virtual ~Test();
    void remove_servers();
    void add_server(SERVER* new_server);

    std::unique_ptr<mxs::Monitor> m_monitor;
};
