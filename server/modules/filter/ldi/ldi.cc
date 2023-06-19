/*
 * Copyright (c) 2023 MariaDB plc
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl.
 *
 * Change Date: 2027-03-14
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#define MXB_MODULE_NAME "ldi"
#include "ldi.hh"
#include "ldisession.hh"

namespace cnf = mxs::config;

namespace
{
namespace ldi
{
constexpr const uint64_t CAPS = MXS_NO_MODULE_CAPABILITIES;

cnf::Specification spec(MXB_MODULE_NAME, cnf::Specification::FILTER);
cnf::ParamString key(
    &ldi::spec, "key", "S3 API key", "", cnf::Param::AT_RUNTIME);
cnf::ParamString secret(
    &ldi::spec, "secret", "S3 API secret", "", cnf::Param::AT_RUNTIME);
cnf::ParamString region(
    &ldi::spec, "region", "S3 region", "us-east-1", cnf::Param::AT_RUNTIME);
cnf::ParamString host(
    &ldi::spec, "host", "S3 host", "", cnf::Param::AT_RUNTIME);
cnf::ParamCount port(
    &ldi::spec, "port", "S3 port", 0, cnf::Param::AT_RUNTIME);
cnf::ParamBool no_verify(
    &ldi::spec, "no_verify", "Skip certificate verification", false, cnf::Param::AT_RUNTIME);
cnf::ParamBool use_http(
    &ldi::spec, "use_http", "Use unencrypted communication", false, cnf::Param::AT_RUNTIME);
cnf::ParamString import_user(
    &ldi::spec, "import_user", "User for Xpand data imports", "", cnf::Param::AT_RUNTIME);
cnf::ParamPassword import_password(
    &ldi::spec, "import_password", "Password for import_user", "", cnf::Param::AT_RUNTIME);
}
}

extern "C" MXS_MODULE* MXS_CREATE_MODULE()
{
    static MXS_MODULE info =
    {
        mxs::MODULE_INFO_VERSION,
        MXB_MODULE_NAME,
        mxs::ModuleType::FILTER,
        mxs::ModuleStatus::IN_DEVELOPMENT,
        MXS_FILTER_VERSION,
        "S3 data loading filter",
        "1.0.0",
        ldi::CAPS,
        &mxs::FilterApi<LDI>::s_api,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &ldi::spec
    };

    return &info;
}

LDI::LDI::Config::Config(const std::string& name)
    : cnf::Configuration(name, &ldi::spec)
{
    add_native(&Config::m_v, &Values::key, &ldi::key);
    add_native(&Config::m_v, &Values::secret, &ldi::secret);
    add_native(&Config::m_v, &Values::region, &ldi::region);
    add_native(&Config::m_v, &Values::host, &ldi::host);
    add_native(&Config::m_v, &Values::port, &ldi::port);
    add_native(&Config::m_v, &Values::no_verify, &ldi::no_verify);
    add_native(&Config::m_v, &Values::use_http, &ldi::use_http);
    add_native(&Config::m_v, &Values::import_user, &ldi::import_user);
    add_native(&Config::m_v, &Values::import_password, &ldi::import_password);
}

bool LDI::LDI::Config::post_configure(const std::map<std::string, mxs::ConfigParameters>& nested_params)
{
    m_values.assign(m_v);
    return true;
}

LDI::LDI(const std::string& name)
    : m_config(name)
{
}

// static
LDI* LDI::create(const char* zName)
{
    return new LDI(zName);
}


mxs::FilterSession* LDI::newSession(MXS_SESSION* pSession, SERVICE* pService)
{
    return LDISession::create(pSession, pService, this);
}

json_t* LDI::diagnostics() const
{
    return nullptr;
}

uint64_t LDI::getCapabilities() const
{
    return ldi::CAPS;
}