/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-10-04
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#include <maxbase/ccdefs.hh>

#include <maxsql/odbc.hh>
#include <maxsql/odbc_helpers.hh>
#include <maxbase/json.hh>
#include <maxbase/log.hh>
#include <maxbase/string.hh>
#include <maxbase/assert.hh>

#include <sql.h>
#include <sqlext.h>
#include <unistd.h>

namespace maxsql
{
class ODBCImp
{
public:
    ODBCImp(std::string dsn);

    ~ODBCImp();

    bool connect();

    void disconnect();

    const std::string& error() const;

    int errnum() const;

    const std::string& sqlstate() const;

    bool query(const std::string& sql, Output* output);

    void set_row_limit(size_t limit);

    std::map<std::string, std::map<std::string, std::string>> drivers();

private:
    template<class Hndl>
    void get_error(int hndl_type, Hndl hndl)
    {
        SQLLEN n = 0;
        SQLRETURN ret = SQLGetDiagField(hndl_type, hndl, 0, SQL_DIAG_NUMBER, &n, 0, 0);

        for (int i = 0; i < n; i++)
        {
            SQLCHAR sqlstate[6];
            SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
            SQLINTEGER native_error;
            SQLSMALLINT msglen = 0;

            if (SQLGetDiagRec(hndl_type, hndl, i + 1, sqlstate, &native_error,
                              msg, sizeof(msg), &msglen) != SQL_NO_DATA)
            {
                m_sqlstate = (const char*)sqlstate;
                m_error.assign((const char*)msg, msglen);
                m_errnum = native_error;
            }
        }
    }

    template<class T>
    bool get_int_attr(int col, int attr, T* t)
    {
        bool ok = false;
        long value = 0;

        if (SQL_SUCCEEDED(SQLColAttribute(m_stmt, col, attr, nullptr, 0, nullptr, &value)))
        {
            *t = value;
            ok = true;
        }

        return ok;
    }

    bool                    process_response(SQLRETURN ret, Output* handler);
    std::vector<ColumnInfo> get_headers(int columns);
    bool                    get_normal_result(int columns, Output* handler);
    bool                    get_batch_result(int columns, Output* handler);
    bool                    data_truncation();
    bool                    can_batch();

    SQLHENV     m_env;
    SQLHDBC     m_conn;
    SQLHSTMT    m_stmt;
    std::string m_dsn;
    std::string m_error;
    std::string m_sqlstate;
    int         m_errnum = 0;
    size_t      m_row_limit = 0;

    std::vector<ColumnInfo> m_columns;
};

ResultBuffer::ResultBuffer(const std::vector<ColumnInfo>& infos, size_t row_limit)
{
    size_t row_size = 0;

    for (const auto& i : infos)
    {
        row_size += buffer_size(i);
    }

    mxb_assert(row_size > 0);
    row_count = MAX_BATCH_SIZE / row_size;

    if (row_limit)
    {
        row_count = std::min(row_limit, row_count);
    }

    mxb_assert(row_count > 0);
    row_status.resize(row_count);
    columns.reserve(infos.size());

    for (const auto& i : infos)
    {
        columns.emplace_back(row_count, buffer_size(i), sql_to_c_type(i.data_type));
    }
}

size_t ResultBuffer::buffer_size(const ColumnInfo& c) const
{
    // return std::min(1024UL * 1024, std::max(c.buffer_size, c.size) + 1);

    switch (c.data_type)
    {
    case SQL_BIT:
    case SQL_TINYINT:
        return sizeof(SQLCHAR);

    case SQL_SMALLINT:
        return sizeof(SQLSMALLINT);

    case SQL_INTEGER:
    case SQL_BIGINT:
        return sizeof(SQLINTEGER);

    case SQL_REAL:
        return sizeof(SQLREAL);

    case SQL_FLOAT:
    case SQL_DOUBLE:
        return sizeof(SQLDOUBLE);

        // Treat everything else as a string, keeps things simple. Also keep the buffer smaller than 1Mib,
        // some varchars seems to be blobs in reality.
    default:
        return std::min(1024UL * 1024, std::max(c.buffer_size, c.size) + 1);
    }
}

bool ResultBuffer::Column::is_null(int row) const
{
    return indicators[row] == SQL_NULL_DATA;
}

std::string ResultBuffer::Column::to_string(int row) const
{
    std::string rval;
    const uint8_t* ptr = buffers.data() + buffer_size * row;
    const SQLLEN len = *(indicators.data() + row);

    switch (buffer_type)
    {
    case SQL_C_BIT:
    case SQL_C_UTINYINT:
        rval = std::to_string(*reinterpret_cast<const SQLCHAR*>(ptr));
        break;

    case SQL_C_USHORT:
        rval = std::to_string(*reinterpret_cast<const SQLUSMALLINT*>(ptr));
        break;

    case SQL_C_ULONG:
        rval = std::to_string(*reinterpret_cast<const SQLUINTEGER*>(ptr));
        break;

    case SQL_C_FLOAT:
        mxb_assert_message(!true, "Floats shouldn't be used, they are broken in C/ODBC");
        rval = std::to_string(*reinterpret_cast<const SQLREAL*>(ptr));
        break;

    case SQL_C_DOUBLE:
        rval = std::to_string(*reinterpret_cast<const SQLDOUBLE*>(ptr));
        break;

        // String, date, time et cetera. Keeps things simple as DATETIME structs are a little messy.
    default:
        if (len != SQL_NULL_DATA)
        {
            rval.assign((const char*)ptr, len);
        }
        else
        {
            rval = "<NULL>";
        }
        break;
    }

    return rval;
}

json_t* ResultBuffer::Column::to_json(int row) const
{
    json_t* rval = nullptr;
    const uint8_t* ptr = buffers.data() + buffer_size * row;
    const SQLLEN len = *(indicators.data() + row);

    switch (buffer_type)
    {
    case SQL_C_BIT:
    case SQL_C_UTINYINT:
        rval = json_integer(*reinterpret_cast<const SQLCHAR*>(ptr));
        break;

    case SQL_C_USHORT:
        rval = json_integer(*reinterpret_cast<const SQLUSMALLINT*>(ptr));
        break;

    case SQL_C_ULONG:
        rval = json_integer(*reinterpret_cast<const SQLUINTEGER*>(ptr));
        break;

    case SQL_C_FLOAT:
        mxb_assert_message(!true, "Floats shouldn't be used, they are broken in C/ODBC");
        rval = json_real(*reinterpret_cast<const SQLREAL*>(ptr));
        break;

    case SQL_C_DOUBLE:
        rval = json_real(*reinterpret_cast<const SQLDOUBLE*>(ptr));
        break;

        // String, date, time et cetera. Keeps things simple as DATETIME structs are a little messy.
    default:
        if (len != SQL_NULL_DATA)
        {
            rval = json_stringn((const char*)ptr, len);
        }
        else
        {
            rval = json_null();
        }
        break;
    }

    mxb_assert(rval);
    return rval;
}

bool JsonResult::ok_result(int64_t rows_affected)
{
    mxb::Json obj(mxb::Json::Type::OBJECT);
    obj.set_int("last_insert_id", 0);
    obj.set_int("warnings", 0);
    obj.set_int("affected_rows", rows_affected);
    m_result.add_array_elem(std::move(obj));
    return true;
}

bool JsonResult::resultset_start(const std::vector<ColumnInfo>& metadata)
{
    m_data = mxb::Json{mxb::Json::Type::ARRAY};
    m_fields = mxb::Json{mxb::Json::Type::ARRAY};

    for (const auto& col : metadata)
    {
        m_fields.add_array_elem(mxb::Json(json_string(col.name.c_str()), mxb::Json::RefType::STEAL));
    }

    return true;
}

bool JsonResult::resultset_rows(const std::vector<ColumnInfo>& metadata,
                                ResultBuffer& res,
                                uint64_t rows_fetched)
{
    int columns = metadata.size();

    for (uint64_t i = 0; i < rows_fetched; i++)
    {
        mxb::Json row(mxb::Json::Type::ARRAY);

        if (res.row_status[i] == SQL_ROW_SUCCESS || res.row_status[i] == SQL_ROW_SUCCESS_WITH_INFO)
        {
            for (int c = 0; c < columns; c++)
            {
                if (!res.columns[c].is_null(i))
                {
                    row.add_array_elem(mxb::Json(res.columns[c].to_json(i), mxb::Json::RefType::STEAL));
                }
            }

            m_data.add_array_elem(std::move(row));
        }
    }

    return true;
}

bool JsonResult::resultset_end()
{
    mxb::Json obj(mxb::Json::Type::OBJECT);
    obj.set_object("fields", std::move(m_fields));
    obj.set_object("data", std::move(m_data));
    m_result.add_array_elem(std::move(obj));
    return true;
}

ODBCImp::ODBCImp(std::string dsn)
    : m_dsn(std::move(dsn))
{
    SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &m_env);
    SQLSetEnvAttr(m_env, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    // The DBC handler must be allocated after the ODBC version is set, otherwise the SQLConnect
    // function returns SQL_INVALID_HANDLE.
    SQLAllocHandle(SQL_HANDLE_DBC, m_env, &m_conn);
}

ODBCImp::~ODBCImp()
{
    SQLFreeHandle(SQL_HANDLE_STMT, m_stmt);
    SQLDisconnect(m_conn);
    SQLFreeHandle(SQL_HANDLE_DBC, m_conn);
    SQLFreeHandle(SQL_HANDLE_ENV, m_env);
}

bool ODBCImp::connect()
{
    SQLSetConnectAttr(m_conn, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
    SQLSetConnectAttr(m_conn, SQL_ATTR_TXN_ISOLATION, (SQLPOINTER)SQL_TXN_REPEATABLE_READ, 0);

    SQLCHAR outbuf[1024];
    SQLSMALLINT s2len;
    SQLRETURN ret = SQLDriverConnect(m_conn, nullptr, (SQLCHAR*)m_dsn.c_str(), m_dsn.size(),
                                     outbuf, sizeof(outbuf), &s2len, SQL_DRIVER_NOPROMPT);

    if (ret == SQL_ERROR)
    {
        get_error(SQL_HANDLE_DBC, m_conn);
    }
    else
    {
        SQLAllocHandle(SQL_HANDLE_STMT, m_conn, &m_stmt);
    }

    return SQL_SUCCEEDED(ret);
}

void ODBCImp::disconnect()
{
    SQLDisconnect(m_conn);
}

const std::string& ODBCImp::error() const
{
    return m_error;
}

int ODBCImp::errnum() const
{
    return m_errnum;
}

const std::string& ODBCImp::sqlstate() const
{
    return m_sqlstate;
}

std::map<std::string, std::map<std::string, std::string>> ODBCImp::drivers()
{
    std::map<std::string, std::map<std::string, std::string>> rval;
    SQLCHAR drv[512];
    std::vector<SQLCHAR> attr(1024);
    SQLSMALLINT drv_sz = 0;
    SQLSMALLINT attr_sz = 0;
    SQLUSMALLINT dir = SQL_FETCH_FIRST;
    SQLRETURN ret;

    while (SQL_SUCCEEDED(ret = SQLDrivers(m_env, dir, drv, sizeof(drv), &drv_sz,
                                          attr.data(), attr.size(), &attr_sz)))
    {
        if (ret == SQL_SUCCESS_WITH_INFO && data_truncation())
        {
            // The buffer was too small, need more space
            attr.resize(attr.size() * 2);
            dir = SQL_FETCH_FIRST;
        }
        else
        {
            dir = SQL_FETCH_NEXT;
            std::map<std::string, std::string> values;

            // The values are separated by nulls and terminated by nulls. Once we find an empty string, we've
            // reached the end of the attribute list.
            for (char* ptr = (char*)attr.data(); *ptr; ptr += strlen(ptr) + 1)
            {
                if (auto tok = mxb::strtok(ptr, "="); tok.size() >= 2)
                {
                    values.emplace(std::move(tok[0]), std::move(tok[1]));
                }
            }

            for (auto kw : {"Driver", "Driver64"})
            {
                if (auto it = values.find(kw); it != values.end())
                {
                    // Check that the driver is actually installed. For some reason there are drivers
                    // defined by default on some systems (Fedora 36) that aren't actually installed.
                    if (access(it->second.c_str(), F_OK) == 0)
                    {
                        rval.emplace((char*)drv, std::move(values));
                        break;
                    }
                }
            }
        }
    }

    return rval;
}

bool ODBCImp::query(const std::string& query, Output* output)
{
    SQLRETURN ret = SQLExecDirect(m_stmt, (SQLCHAR*)query.c_str(), query.size());
    return process_response(ret, output);
}

void ODBCImp::set_row_limit(size_t limit)
{
    m_row_limit = limit;
}

bool ODBCImp::process_response(SQLRETURN ret, Output* handler)
{
    mxb_assert(handler);
    bool ok = false;

    if (SQL_SUCCEEDED(ret))
    {
        ok = true;

        do
        {
            SQLSMALLINT columns = 0;
            SQLNumResultCols(m_stmt, &columns);

            if (columns == 0)
            {
                SQLLEN rowcount = 0;
                SQLRowCount(m_stmt, &rowcount);
                handler->ok_result(rowcount);
            }
            else if (columns > 0)
            {
                m_columns = get_headers(columns);
                handler->resultset_start(m_columns);
                ok = can_batch() ? get_batch_result(columns, handler) : get_normal_result(columns, handler);
                handler->resultset_end();
            }
        }
        while (SQL_SUCCEEDED(SQLMoreResults(m_stmt)));

        SQLCloseCursor(m_stmt);
    }
    else
    {
        get_error(SQL_HANDLE_STMT, m_stmt);
    }

    return ok;
}

bool ODBCImp::data_truncation()
{
    constexpr std::string_view truncated = "01004";
    SQLLEN n = 0;
    SQLGetDiagField(SQL_HANDLE_STMT, m_stmt, 0, SQL_DIAG_NUMBER, &n, 0, 0);

    for (int i = 0; i < n; i++)
    {
        SQLCHAR sqlstate[6];
        SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
        SQLINTEGER native_error;
        SQLSMALLINT msglen = 0;

        if (SQLGetDiagRec(SQL_HANDLE_STMT, m_stmt, i + 1, sqlstate, &native_error,
                          msg, sizeof(msg), &msglen) != SQL_NO_DATA)
        {
            if ((const char*)sqlstate == truncated)
            {
                return true;
            }
        }
    }

    return false;
}

std::vector<ColumnInfo> ODBCImp::get_headers(int columns)
{
    std::vector<ColumnInfo> cols;
    cols.reserve(columns);

    for (SQLSMALLINT i = 0; i < columns; i++)
    {
        char name[256] = "";
        SQLSMALLINT namelen = 0;
        SQLSMALLINT data_type;
        SQLULEN colsize;
        SQLSMALLINT digits;
        SQLSMALLINT nullable;

        SQLRETURN ret = SQLDescribeCol(m_stmt, i + 1, (SQLCHAR*)name, sizeof(name), &namelen,
                                       &data_type, &colsize, &digits, &nullable);

        if (SQL_SUCCEEDED(ret))
        {
            ColumnInfo info;
            info.name = name;
            info.size = colsize;
            info.data_type = data_type;
            info.digits = digits;
            info.nullable = nullable;

            if (!get_int_attr(i + 1, SQL_DESC_OCTET_LENGTH, &info.buffer_size))
            {
                get_error(SQL_HANDLE_STMT, m_stmt);
                SQLCloseCursor(m_stmt);
                return {};
            }

            cols.push_back(std::move(info));
        }
        else if (ret == SQL_ERROR)
        {
            get_error(SQL_HANDLE_STMT, m_stmt);
            SQLCloseCursor(m_stmt);
            return {};
        }
    }

    return cols;
}

bool ODBCImp::get_normal_result(int columns, Output* handler)
{
    SQLRETURN ret;
    ResultBuffer res(m_columns, 1);

    bool ok = true;

    while (SQL_SUCCEEDED(ret = SQLFetch(m_stmt)))
    {
        for (SQLSMALLINT i = 0; i < columns; i++)
        {
            auto& c = res.columns[i];
            ret = SQLGetData(m_stmt, i + 1, c.buffer_type, c.buffers.data(), c.buffers.size(),
                             c.indicators.data());

            while (ret == SQL_SUCCESS_WITH_INFO && data_truncation())
            {
                auto old_size = c.buffers.size() - 1;   // Minus one since these are null-terminated strings
                c.buffers.resize(c.indicators.front());
                c.buffer_size = c.buffers.size();
                ret = SQLGetData(m_stmt, i + 1, c.buffer_type, c.buffers.data() + old_size,
                                 c.buffers.size() - old_size, c.indicators.data());
            }

            if (ret == SQL_ERROR)
            {
                get_error(SQL_HANDLE_STMT, m_stmt);
                ok = false;
                break;
            }
        }

        if (!handler->resultset_rows(m_columns, res, 1))
        {
            ok = false;
            break;
        }
    }

    SQLCloseCursor(m_stmt);

    if (ret == SQL_ERROR)
    {
        get_error(SQL_HANDLE_STMT, m_stmt);
        ok = false;
    }

    return ok;
}

bool ODBCImp::get_batch_result(int columns, Output* handler)
{
    ResultBuffer res(m_columns, m_row_limit);

    SQLULEN rows_fetched = 0;
    SQLSetStmtAttr(m_stmt, SQL_ATTR_ROW_BIND_TYPE, (void*)SQL_BIND_BY_COLUMN, 0);
    SQLSetStmtAttr(m_stmt, SQL_ATTR_ROW_ARRAY_SIZE, (void*)res.row_count, 0);
    SQLSetStmtAttr(m_stmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0);
    SQLSetStmtAttr(m_stmt, SQL_ATTR_ROW_STATUS_PTR, res.row_status.data(), 0);

    SQLRETURN ret;
    bool ok = true;

    for (int i = 0; i < columns; i++)
    {
        ret = SQLBindCol(m_stmt, i + 1, res.columns[i].buffer_type, res.columns[i].buffers.data(),
                         res.columns[i].buffer_size, res.columns[i].indicators.data());

        if (!SQL_SUCCEEDED(ret))
        {
            ok = false;
            break;
        }
    }

    size_t total_rows = 0;
    bool below_limit = true;

    while (ok && below_limit && SQL_SUCCEEDED(ret = SQLFetch(m_stmt)))
    {
        total_rows += rows_fetched;

        if (m_row_limit > 0 && total_rows > m_row_limit)
        {
            rows_fetched = total_rows - m_row_limit;
            below_limit = false;
        }

        if (!handler->resultset_rows(m_columns, res, rows_fetched))
        {
            ok = false;
        }
    }

    if (ret == SQL_ERROR)
    {
        get_error(SQL_HANDLE_STMT, m_stmt);

        if (m_error.empty())
        {
            get_error(SQL_HANDLE_DBC, m_conn);
        }

        ok = false;
    }

    return ok;
}

bool ODBCImp::can_batch()
{
    for (const auto& i : m_columns)
    {
        size_t buffer_size = 0;

        switch (i.data_type)
        {
        // If the result has LOBs in it, the data should be retrieved one row at a time using
        // SQLGetData instead of using an array to fetch multiple rows at a time.
        case SQL_WLONGVARCHAR:
        case SQL_LONGVARCHAR:
        case SQL_LONGVARBINARY:
            return i.size < 16384;

        default:
            // Around the maximum value of a VARCHAR field. Anything bigger than this should be read one value
            // at a time to reduce memory usage.
            constexpr size_t MAX_CHUNK_SIZE = 65536;

            if (i.size == 0 || i.size > MAX_CHUNK_SIZE)
            {
                // The driver either doesn't know how big the value is or it is way too large to be batched.
                return false;
            }
        }
    }

    return true;
}

ODBC::ODBC(std::string dsn)
    : m_imp(std::make_unique<mxq::ODBCImp>(std::move(dsn)))
{
}

ODBC::~ODBC()
{
}

ODBC::ODBC(ODBC&& other)
{
    m_imp = move(other.m_imp);
}

ODBC& ODBC::operator=(ODBC&& other)
{
    m_imp = move(other.m_imp);
    return *this;
}

bool ODBC::connect()
{
    return m_imp->connect();
}

void ODBC::disconnect()
{
    m_imp->disconnect();
}

const std::string& ODBC::error() const
{
    return m_imp->error();
}

int ODBC::errnum() const
{
    return m_imp->errnum();
}

const std::string& ODBC::sqlstate() const
{
    return m_imp->sqlstate();
}

bool ODBC::query(const std::string& sql, mxq::Output* output)
{
    return m_imp->query(sql, output);
}

void ODBC::set_row_limit(size_t limit)
{
    m_imp->set_row_limit(limit);
}

// static
std::map<std::string, std::map<std::string, std::string>> ODBC::drivers()
{
    // This simplifies the driver querying. We don't need a connection but we do need a valid environment
    // handle to get the drivers.
    auto tmp = std::make_unique<mxq::ODBCImp>("");
    return tmp->drivers();
}
}
