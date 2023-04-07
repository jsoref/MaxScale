/**
 * @file lots_of_row.cpp INSERT extremely big number of rows
 * - do INSERT of 100 rows in the loop 2000 times
 * - do SELECT *
 */


#include <iostream>
#include <maxtest/testconnections.hh>
#include <maxtest/galera_cluster.hh>
#include <maxtest/sql_t1.hh>

int main(int argc, char* argv[])
{
    TestConnections* Test = new TestConnections(argc, argv);
    char sql[10240];

    Test->maxscale->connect_maxscale();
    create_t1(Test->maxscale->conn_rwsplit);

    Test->tprintf("INSERTing data");

    Test->try_query(Test->maxscale->conn_rwsplit, "BEGIN");
    for (int i = 0; i < 2000; i++)
    {
        Test->reset_timeout();
        create_insert_string(sql, 100, i);
        Test->try_query(Test->maxscale->conn_rwsplit, "%s", sql);
    }
    Test->try_query(Test->maxscale->conn_rwsplit, "COMMIT");

    Test->tprintf("done, syncing slaves");
    Test->tprintf("Trying SELECT");
    Test->reset_timeout();
    Test->try_query(Test->maxscale->conn_rwsplit, (char*) "SELECT * FROM t1");

    Test->check_maxscale_alive();
    int rval = Test->global_result;
    delete Test;
    return rval;
}
