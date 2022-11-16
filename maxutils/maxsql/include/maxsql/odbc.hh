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
#pragma once

#include <maxsql/ccdefs.hh>

#include <optional>
#include <string>
#include <vector>
#include <map>
#include <memory>

#include <maxbase/json.hh>

namespace maxsql
{

struct ColumnInfo
{
    std::string name;           // Column name
    int         data_type {0};  // ODBC data type
    size_t      size {0};       // The size of the SQL type (e.g. Unicode characters)
    size_t      buffer_size {0};// The "octet" size, i.e. size in bytes
    int         digits {0};     // Number of digits, zero if not applicable
    bool        nullable {0};   // If column is nullable
};

struct ResultBuffer
{
    struct Column
    {
        Column(size_t row_count, size_t buffer_sz, int type)
            : buffer_size(buffer_sz)
            , buffer_type(type)
            , buffers(row_count * buffer_size)
            , indicators(row_count)
        {
        }

        bool        is_null(int row) const;
        std::string to_string(int row) const;
        json_t*     to_json(int row) const;

        size_t               buffer_size;   // Size of one value
        int                  buffer_type;   // ODBC C data type
        std::vector<uint8_t> buffers;       // Buffer that contains the column values
        std::vector<long>    indicators;    // Indicator values for each of the column values
    };

    // TODO: Make this configurable. 10Mb isn't that much when there's only one table and 10Mb is a lot when
    // there's 1000 tables.
    static constexpr size_t MAX_BATCH_SIZE = 1024 * 1024 * 10;

    ResultBuffer(const std::vector<ColumnInfo>& infos, size_t row_limit = 0);

    size_t buffer_size(const ColumnInfo& c) const;

    size_t                      row_count = 0;
    std::vector<Column>         columns;
    std::vector<unsigned short> row_status;
};

class Output
{
public:
    virtual ~Output() = default;

    // Called whenever an empty result (i.e. an OK packet) is received.
    virtual bool ok_result(int64_t rows_affected) = 0;

    // Called before the first row of the resultset is read.
    virtual bool resultset_start(const std::vector<ColumnInfo>& metadata) = 0;

    // Called for each batch of rows read. The value of `rows_fetched` contains how many rows of data are
    // available.
    virtual bool resultset_rows(const std::vector<ColumnInfo>& metadata,
                                ResultBuffer& res, uint64_t rows_fetched) = 0;

    // Called when the resultset ends.
    virtual bool resultset_end() = 0;
};

// Creates a mxb::Json result
struct JsonResult : public Output
{
    bool ok_result(int64_t rows_affected) override;
    bool resultset_start(const std::vector<ColumnInfo>& metadata) override;
    bool resultset_rows(const std::vector<ColumnInfo>& metadata, ResultBuffer& res,
                        uint64_t rows_fetched) override;
    bool resultset_end() override;

    mxb::Json result()
    {
        return m_result;
    }

private:
    mxb::Json m_result{mxb::Json::Type::ARRAY};
    mxb::Json m_data;
    mxb::Json m_fields;
};

// Discards the result
struct NoResult : public Output
{
    virtual bool ok_result(int64_t rows_affected) override
    {
        return true;
    }

    bool resultset_start(const std::vector<ColumnInfo>& metadata) override
    {
        return true;
    }

    bool resultset_rows(const std::vector<ColumnInfo>& metadata, ResultBuffer& res,
                        uint64_t rows_fetched) override
    {
        return true;
    }

    bool resultset_end() override
    {
        return true;
    }
};

// The concrete implementation class is defined in the source file. We can't include the ODBC files in a
// header as they add very disruptive defines that cause problems elsewhere.
class ODBCImp;

class ODBC
{
public:
    ODBC(const ODBC&) = delete;
    ODBC& operator=(const ODBC&) = delete;

    ODBC(ODBC&&);
    ODBC& operator=(ODBC&&);

    /**
     * Get available ODBC drivers
     *
     * @return Map of driver definitions by name and their parameters. Only returns drivers for which a driver
     *         library was found.
     */
    static std::map<std::string, std::map<std::string, std::string>> drivers();

    /**
     * Create a new ODBC instance
     *
     * @param dsn The connection string given to the driver manager
     */
    ODBC(std::string dsn);

    ~ODBC();

    /**
     * Connect to the database
     *
     * @return True if the connection was created successfully.
     */
    bool connect();

    /**
     * Disconnect the connection
     */
    void disconnect();

    /**
     * Get the latest error message
     *
     * @return The latest error message or an empty string if no errors have occurred
     */
    const std::string& error() const;

    /**
     * Get the latest error number
     *
     * @return The latest error number or 0 if no errors have occurred
     */
    int errnum() const;

    /**
     * Get the latest SQLSTATE
     *
     * @return The latest SQLSTATE or an empty string if no SQLSTATE is available
     */
    const std::string& sqlstate() const;

    bool query(const std::string& sql, Output* output);

    /**
     * Set maximum number of rows to fetch
     *
     * @param limit The number of rows to fetch, 0 for no limit.
     */
    void set_row_limit(size_t limit);

private:
    std::unique_ptr<ODBCImp> m_imp;
};
}
