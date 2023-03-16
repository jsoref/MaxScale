/*
 * Copyright (c) 2023 MariaDB plc
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

#include "postgresprotocol.hh"
#include "pgprotocoldata.hh"
#include <maxscale/protocol2.hh>
#include <deque>

class PgBackendConnection final : public mxs::BackendConnection
{
public:
    PgBackendConnection(MXS_SESSION* session, SERVER* server, mxs::Component* component);

    void ready_for_reading(DCB* dcb) override;
    void write_ready(DCB* dcb) override;
    void error(DCB* dcb) override;
    void hangup(DCB* dcb) override;

    bool write(GWBUF&& buffer) override;

    void finish_connection() override;

    uint64_t can_reuse(MXS_SESSION* session) const override;
    bool     reuse(MXS_SESSION* session, mxs::Component* upstream, uint64_t reuse_type) override;
    bool     established() override;
    void     set_to_pooled() override;
    void     ping() override;
    bool     can_close() const override;

    void              set_dcb(DCB* dcb) override;
    const BackendDCB* dcb() const override;
    BackendDCB*       dcb() override;
    mxs::Component*   upstream() const override;

    json_t* diagnostics() const override;
    size_t  sizeof_buffers() const override;

private:
    enum class State
    {
        INIT,
        SSL_REQUEST,
        SSL_HANDSHAKE,
        AUTH,
        STARTUP,
        BACKLOG,
        ROUTING,
        FAILED,
    };

    bool check_size(const GWBUF& buffer, size_t bytes);
    void handle_error(const std::string& error, mxs::ErrorType type = mxs::ErrorType::TRANSIENT);
    void send_ssl_request();
    void send_startup_message();

    bool handle_ssl_request();
    bool handle_ssl_handshake();
    bool handle_startup();
    bool handle_auth();
    bool handle_backlog();
    bool handle_routing();

    GWBUF process_packets(GWBUF& buffer);

    PgProtocolData& protocol_data()
    {
        return *static_cast<PgProtocolData*>(m_session->protocol_data());
    }

    MXS_SESSION*    m_session;
    mxs::Component* m_upstream;
    BackendDCB*     m_dcb;
    mxs::Reply      m_reply;
    State           m_state {State::INIT};

    uint32_t m_process_id {0};      // The process ID on the backend server
    uint32_t m_secret_key {0};      // Secret key for canceling requests

    // Backlog of packets that need to be written again. These are only buffered for the duratio of the
    // connection creation and authentication after which they are re-sent to the write() function.
    std::vector<GWBUF> m_backlog;

    // A queue of commands that are being executed. The queue will be empty if only one result is expected.
    std::deque<uint8_t> m_track_queue;
};
