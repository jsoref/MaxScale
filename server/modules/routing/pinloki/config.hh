/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-03-14
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#pragma once

#include "gtid.hh"

#include <maxscale/ccdefs.hh>

#include <maxbase/stopwatch.hh>
#include <maxscale/paths.hh>
#include <maxscale/config2.hh>
#include <maxscale/key_manager.hh>

#include <string>

namespace pinloki
{

std::string gen_uuid();

using namespace std::literals::chrono_literals;
using namespace std::literals::string_literals;

class Config : public mxs::config::Configuration
{
public:
    Config(const std::string& name, std::function<bool()> callback);
    Config(Config&&) = default;

    static const mxs::config::Specification* spec();

    /** Make a full path. This prefixes "name" with m_binlog_dir/,
     *  unless the first character is a forward slash.
     */
    std::string path(const std::string& name) const;

    std::string inventory_file_path() const;
    std::string gtid_file_path() const;
    std::string requested_gtid_file_path() const;
    std::string master_info_file() const;
    uint32_t    server_id() const;

    // Network timeout
    std::chrono::seconds net_timeout() const;
    // Automatic master selection
    bool select_master() const;
    bool ddl_only() const;
    void disable_select_master();

    const std::string& key_id() const;

    mxb::Cipher::AesMode encryption_cipher() const;

    // File purging
    int32_t             expire_log_minimum_files() const;
    wall_time::Duration expire_log_duration() const;
    wall_time::Duration purge_startup_delay() const;
    wall_time::Duration purge_poll_timeout() const;

    bool post_configure(const std::map<std::string, mxs::ConfigParameters>& nested_params) override;

private:
    /** Where the binlog files are stored */
    std::string m_binlog_dir;
    /** Name of gtid file */
    std::string m_gtid_file = "rpl_state";
    /** Master configuration file name */
    std::string m_master_info_file = "master-info.json";
    /** Name of the binlog inventory file. */
    std::string m_binlog_inventory_file = "binlog.index";
    /* Hashing directory (properly indexing, but the word is already in use) */
    std::string m_binlog_hash_dir = ".hash";
    /** Where the current master details are stored */
    std::string m_master_ini_path;
    /** Server id reported to the Master */
    int64_t m_server_id;
    /** uuid reported to the server */
    std::string m_uuid = gen_uuid();
    /** uuid reported to the slaves */
    std::string m_master_uuid;
    /** mariadb version reported to the slaves, defaults to the actual master */
    std::string m_master_version;
    /** host name reported to the slaves, defaults to the master's host name */
    std::string m_master_hostname;
    /** If set, m_slave_hostname is sent to the master during registration */
    std::string m_slave_hostname;
    /** Service user */
    std::string m_user = "maxskysql";
    /** Service password */
    std::string m_password = "skysql";
    /** Request master to send a binlog event at this interval , default 5min*/
    maxbase::Duration m_heartbeat_interval = maxbase::Duration(300s);

    /**
     *  Master connection retry timeout. Default 60s.
     */
    maxbase::Duration m_connect_retry_tmo = 60s;

    std::chrono::seconds m_net_timeout;
    bool                 m_select_master;
    bool                 m_select_master_disabled {false};
    bool                 m_ddl_only {false};
    std::string          m_encryption_key_id;
    mxb::Cipher::AesMode m_encryption_cipher;

    int64_t             m_expire_log_minimum_files;
    wall_time::Duration m_expire_log_duration;
    wall_time::Duration m_purge_startup_delay;
    wall_time::Duration m_purge_poll_timeout;

    std::function<bool()> m_cb;
};
}
