/*
 * Copyright (c) 2022 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-04-08
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#pragma once
#include <maxscale/ccdefs.hh>
#include <memory>

namespace ssh
{
class Session;
class Channel;
}

namespace ssh_util
{
// LibSSH defines the Session-class, which may be shared between multiple channels (i.e. running commands).
// Store it in a shared_ptr to ensure proper destruction.
using SSession = std::shared_ptr<ssh::Session>;

/**
 * Start an SSH session. Reads private key from file, connects to server and authenticates. The server must
 * already be listed in the known_hosts-file.
 *
 * @param host Host address
 * @param user Username
 * @param keyfile Private key file
 * @param timeout Connection timeout
 * @return If success, the session. On error, an error message.
 */
std::tuple<SSession, std::string>
init_ssh_session(const std::string& host, const std::string& user, const std::string& keyfile,
                 std::chrono::milliseconds timeout);

struct CmdResult
{
    enum class Type
    {
        OK,         /**< Command was sent and output + return code fetched */
        SSH_FAIL,   /**< Failed to send command or read result */
        TIMEOUT     /**< Command timed out */
    };

    Type        type {Type::SSH_FAIL};  /**< Result type */
    int         rc {-1};                /**< If command completed, its return code */
    std::string output;                 /**< Command standard output */
    std::string error_output;           /**< Command error output or ssh error message */
};

CmdResult run_cmd(ssh::Session& read_error_stream, const std::string& cmd, std::chrono::milliseconds timeout);

class AsyncCmd
{
public:
    AsyncCmd(std::shared_ptr<ssh::Session> ses, std::unique_ptr<ssh::Channel> chan);
    ~AsyncCmd();

    enum class Status {READY, SSH_FAIL, BUSY};
    Status update_status();

    const std::string& output() const;
    const std::string& error_output() const;
    int                rc() const;

private:
    // The session can be shared between multiple channels, each running a command.
    std::shared_ptr<ssh::Session> m_ses;
    std::unique_ptr<ssh::Channel> m_chan;

    int         m_rc {-1};              /**< If command completed, its return code */
    std::string m_output;               /**< Command standard output */
    std::string m_error_output;         /**< Command error output or ssh error message */
    Status      m_status {Status::SSH_FAIL};
};

/**
 * Start an async ssh command.
 *
 * @param ses Existing ssh session
 * @param cmd The command to execute
 * @return If successful, the object. On failure, an error string.
 */
std::tuple<std::unique_ptr<AsyncCmd>, std::string>
start_async_cmd(std::shared_ptr<ssh::Session> ses, const std::string& cmd);

std::string form_cmd_error_msg(const CmdResult& res, const char* cmd);
}